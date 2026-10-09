// Forwarder Manager - forwarder launcher payload.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A PS5 app may not start another app, so a forwarder tile asks a small
// resident payload to do it. This is that payload, written from scratch and
// open source: Forwarder Manager sends it to elfldr when it starts and nothing
// is serving the launcher port yet.
//
// It speaks the forwarder protocol the shared forwarder eboot.bin uses (the
// same wire format as ps5-app-launcher, so either can serve any forwarder):
//
//   request, 0x1020 bytes, little endian
//     0x00  u32  magic   'PFWL' (0x4C574650)
//     0x04  u32  version 1
//     0x08  char target[16]   title ID, NUL-terminated
//     0x18  u32  argc         number of launch arguments
//     0x1C  u32  args_len     bytes of args used, NULs included
//     0x20  char args[0x1000] the arguments, each NUL-terminated
//
//   reply, 0x18 bytes
//     0x00  u32  magic   'PFWL'
//     0x04  u32  version 1
//     0x08  u32  stage      0 launched, 1 bad request, 2 no signed-in user,
//                           3 the system refused the launch
//     0x0C  i32  first_rc   sceSystemServiceLaunchApp, plain parameters
//     0x10  i32  second_rc  ... and with a sized parameter block
//     0x14  i32  user       the foreground user the title was started for
//
// It listens on 127.0.0.1 only and logs to /data/forwarder-manager/
// launcher.log.

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifndef LAUNCHER_PORT
#define LAUNCHER_PORT 10199
#endif
#define MAGIC 0x4C574650u // "PFWL"
#define VERSION 1u
#define REQUEST_SIZE 0x1020
#define REPLY_SIZE 0x18
#define ARGS_SIZE 0x1000
#define MAX_ARGS 64

#ifndef LOG_DIR
#define LOG_DIR "/data/forwarder-manager"
#endif
#define LOG_PATH LOG_DIR "/launcher.log"

enum stage
{
    STAGE_LAUNCHED = 0,
    STAGE_BAD_REQUEST = 1,
    STAGE_NO_USER = 2,
    STAGE_REFUSED = 3,
};

// The launch parameter block. Only the size and the user are set; the rest
// stays zero.
struct launch_param
{
    uint32_t size;
    int32_t user_id;
    uint8_t reserved[24];
};

// From libSceUserService and libSceSystemService (the SDK ships the stubs).
int sceUserServiceInitialize(void *params);
int sceUserServiceGetForegroundUser(int32_t *user_id);
int sceSystemServiceLaunchApp(const char *title_id, char *const argv[],
                              struct launch_param *param);

static int g_log = -1;

static void log_line(const char *format, ...)
{
    char line[1024];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(line, sizeof(line) - 1, format, args);
    va_end(args);
    if (n < 0)
        return;
    if ((size_t)n > sizeof(line) - 2)
        n = (int)sizeof(line) - 2;
    line[n++] = '\n';
    if (g_log >= 0)
        (void)write(g_log, line, (size_t)n);
}

static uint32_t get_u32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void put_u32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

// PPSA / CUSA / LAPY followed by five digits.
static int valid_title(const char *t)
{
    if (strnlen(t, 16) != 9)
        return 0;
    if (memcmp(t, "PPSA", 4) != 0 && memcmp(t, "CUSA", 4) != 0 && memcmp(t, "LAPY", 4) != 0)
        return 0;
    for (int i = 4; i < 9; ++i)
        if (t[i] < '0' || t[i] > '9')
            return 0;
    return 1;
}

static int read_all(int fd, unsigned char *buffer, size_t size)
{
    size_t done = 0;
    while (done < size)
    {
        ssize_t got = read(fd, buffer + done, size - done);
        if (got <= 0)
            return (int)done;
        done += (size_t)got;
    }
    return (int)done;
}

static void write_all(int fd, const unsigned char *buffer, size_t size)
{
    size_t done = 0;
    while (done < size)
    {
        ssize_t sent = write(fd, buffer + done, size - done);
        if (sent <= 0)
            return;
        done += (size_t)sent;
    }
}

static void reply(int fd, uint32_t stage, int32_t first_rc, int32_t second_rc, int32_t user)
{
    unsigned char out[REPLY_SIZE];
    memset(out, 0, sizeof(out));
    put_u32(out + 0x00, MAGIC);
    put_u32(out + 0x04, VERSION);
    put_u32(out + 0x08, stage);
    put_u32(out + 0x0C, (uint32_t)first_rc);
    put_u32(out + 0x10, (uint32_t)second_rc);
    put_u32(out + 0x14, (uint32_t)user);
    write_all(fd, out, sizeof(out));
    log_line("reply: stage=%u first_rc=0x%08X second_rc=0x%08X user=0x%08X", stage,
             (unsigned)first_rc, (unsigned)second_rc, (unsigned)user);
}

static void serve(int fd)
{
    static unsigned char request[REQUEST_SIZE];
    struct timeval timeout = {5, 0};
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    const int got = read_all(fd, request, sizeof(request));
    if (got == 0)
        return; // a probe: Forwarder Manager checking that someone is here
    if (got != REQUEST_SIZE || get_u32(request) != MAGIC || get_u32(request + 4) != VERSION)
    {
        log_line("bad request (%d bytes)", got);
        reply(fd, STAGE_BAD_REQUEST, 0, 0, 0);
        return;
    }

    char target[17];
    memcpy(target, request + 0x08, 16);
    target[16] = '\0';
    const uint32_t argc = get_u32(request + 0x18);
    const uint32_t args_len = get_u32(request + 0x1C);
    if (!valid_title(target) || argc > MAX_ARGS || args_len > ARGS_SIZE)
    {
        log_line("bad request: target=\"%s\" argc=%u args_len=%u", target, argc, args_len);
        reply(fd, STAGE_BAD_REQUEST, 0, 0, 0);
        return;
    }

    // The arguments are NUL-terminated strings packed one after another.
    static char args[ARGS_SIZE + 1];
    memcpy(args, request + 0x20, args_len);
    args[args_len] = '\0';
    char *argv[MAX_ARGS + 1];
    uint32_t n = 0;
    size_t at = 0;
    while (n < argc && at < args_len)
    {
        argv[n++] = args + at;
        at += strnlen(args + at, args_len - at) + 1;
    }
    if (n != argc)
    {
        log_line("bad request: %u of %u arguments present", n, argc);
        reply(fd, STAGE_BAD_REQUEST, 0, 0, 0);
        return;
    }
    argv[n] = NULL;

    int32_t user = -1;
    const int user_rc = sceUserServiceGetForegroundUser(&user);
    log_line("request: start %s for user 0x%08X (foreground rc=0x%08X)", target, (unsigned)user,
             (unsigned)user_rc);
    for (uint32_t i = 0; i < n; ++i)
        log_line("  argv[%u] = %s", i, argv[i]);
    if (user_rc < 0 || user == -1)
    {
        reply(fd, STAGE_NO_USER, user_rc, 0, user);
        return;
    }

    // First with a parameter block the system sizes itself, then with an
    // explicit size: firmwares differ in which they accept.
    struct launch_param param;
    memset(&param, 0, sizeof(param));
    param.user_id = user;
    const int first = sceSystemServiceLaunchApp(target, argv, &param);
    log_line("sceSystemServiceLaunchApp(%s) structsize=0 rc=0x%08X", target, (unsigned)first);
    if (first >= 0)
    {
        reply(fd, STAGE_LAUNCHED, first, 0, user);
        return;
    }
    memset(&param, 0, sizeof(param));
    param.size = sizeof(param);
    param.user_id = user;
    const int second = sceSystemServiceLaunchApp(target, argv, &param);
    log_line("sceSystemServiceLaunchApp(%s) structsize=%u rc=0x%08X", target,
             (unsigned)sizeof(param), (unsigned)second);
    reply(fd, second >= 0 ? STAGE_LAUNCHED : STAGE_REFUSED, first, second, user);
}

int main(void)
{
    (void)mkdir(LOG_DIR, 0777);
    g_log = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0666);

    const int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0)
    {
        log_line("socket() failed: %s", strerror(errno));
        return 1;
    }
    const int on = 1;
    (void)setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(LAUNCHER_PORT);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(server, (struct sockaddr *)&address, sizeof(address)) != 0)
    {
        // Another launcher (this one, or ps5-app-launcher) already serves.
        log_line("bind() failed: %s; a launcher is already running", strerror(errno));
        close(server);
        return 0;
    }
    if (listen(server, 4) != 0)
    {
        log_line("listen() failed: %s", strerror(errno));
        close(server);
        return 1;
    }
    (void)sceUserServiceInitialize(NULL);
    log_line("forwarder launcher %s starting (pid %d), listening on 127.0.0.1:%d", "1.0",
             (int)getpid(), LAUNCHER_PORT);

    for (;;)
    {
        const int client = accept(server, NULL, NULL);
        if (client < 0)
        {
            if (errno == EINTR)
                continue;
            log_line("accept() failed: %s", strerror(errno));
            sleep(1);
            continue;
        }
        serve(client);
        close(client);
    }
}
