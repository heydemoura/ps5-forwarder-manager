// Forwarder Manager - the forwarder tile program (eboot.bin).
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Every forwarder tile runs this same eboot.bin. It reads forwarder.json next
// to it ({"target": "PPSA99008", "args": ["--rom", "Game.nsp"]}) and asks the
// forwarder launcher on 127.0.0.1:10199 to start the target with those
// arguments: an app may not start another app itself.
//
// Unlike the website's forwarder, it does not need the launcher to be loaded
// beforehand. When nothing answers on the launcher port it sends the launcher
// payload it carries (launcher/fwd_launcher.c, embedded at build time) to
// elfldr on 127.0.0.1:9021 and waits for it to come up. The wire format is the
// one documented at the top of launcher/fwd_launcher.c.
//
// It writes /data/ps5-forwarder/<TITLE_ID>.log when it can, sends the same
// lines to the console log (sceKernelDebugOutText), and shows a notification when something fails.
// Plain C with fixed buffers: the tile links no C++ runtime.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

// The launcher payload, generated from assets/launcher/fwd-launcher.elf.
#include "launcher_payload.inc"

#define LAUNCHER_PORT 10199
#define ELFLDR_PORT 9021
#define MAGIC 0x4C574650u // "PFWL"
#define VERSION 1u
#define REQUEST_SIZE 0x1020
#define REPLY_SIZE 0x18
#define ARGS_SIZE 0x1000
#define MAX_ARGS 64
#define MAX_TEXT 16384
#define QUEUED 0x80940010u // the tile is still running: the system queues the launch

struct notification
{
    uint8_t reserved[45];
    char message[3075];
};

int sceKernelSendNotificationRequest(uint32_t device, void *request, size_t size, int blocking);
int sceKernelDebugOutText(int channel, const char *text);

static char g_title[16] = "PPSA00000";
static FILE *g_log = NULL;

static void log_line(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void log_line(const char *format, ...)
{
    char text[1024];
    va_list args;
    va_start(args, format);
    (void)vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    char line[1100];
    (void)snprintf(line, sizeof(line), "[ps5-forwarder %s] %s\n", g_title, text);
    (void)sceKernelDebugOutText(0, line); // the console log (klog)
    if (g_log != NULL)
    {
        fprintf(g_log, "%s\n", text);
        fflush(g_log);
    }
}

// Logs and shows `message`, then ends the tile.
static void fail(const char *message) __attribute__((noreturn));
static void fail(const char *message)
{
    log_line("FAILED: %s", message);
    static struct notification request;
    memset(&request, 0, sizeof(request));
    (void)snprintf(request.message, sizeof(request.message), "Forwarder: %s", message);
    (void)sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
    if (g_log != NULL)
        fclose(g_log);
    exit(1);
}

// Reads a whole small file; returns its length, or -1.
static long read_file(const char *path, char *out, size_t size)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL)
        return -1;
    const size_t got = fread(out, 1, size - 1, file);
    fclose(file);
    out[got] = '\0';
    return (long)got;
}

// ---- a small JSON reader: one object of string and string-array values -------

struct json
{
    const char *text;
    size_t at;
    size_t size;
};

static void json_space(struct json *j)
{
    while (j->at < j->size && (j->text[j->at] == ' ' || j->text[j->at] == '\t' ||
                               j->text[j->at] == '\r' || j->text[j->at] == '\n'))
        ++j->at;
}

static int json_eat(struct json *j, char c)
{
    json_space(j);
    if (j->at < j->size && j->text[j->at] == c)
    {
        ++j->at;
        return 1;
    }
    return 0;
}

static int put_byte(char *out, size_t cap, size_t *n, char c)
{
    if (*n + 1 >= cap)
        return 0;
    out[(*n)++] = c;
    return 1;
}

// Reads a string into out (NUL-terminated). 0 on a malformed or too long value.
static int json_string(struct json *j, char *out, size_t cap)
{
    json_space(j);
    if (j->at >= j->size || j->text[j->at] != '"')
        return 0;
    ++j->at;
    size_t n = 0;
    while (j->at < j->size && j->text[j->at] != '"')
    {
        char c = j->text[j->at++];
        if (c == '\\' && j->at < j->size)
        {
            const char e = j->text[j->at++];
            switch (e)
            {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u':
            {
                if (j->at + 4 > j->size)
                    return 0;
                char hex[5];
                memcpy(hex, j->text + j->at, 4);
                hex[4] = '\0';
                const unsigned code = (unsigned)strtoul(hex, NULL, 16);
                j->at += 4;
                // Kept as UTF-8 for the basic plane.
                if (code < 0x80)
                {
                    if (!put_byte(out, cap, &n, (char)code))
                        return 0;
                }
                else if (code < 0x800)
                {
                    if (!put_byte(out, cap, &n, (char)(0xC0 | (code >> 6))) ||
                        !put_byte(out, cap, &n, (char)(0x80 | (code & 0x3F))))
                        return 0;
                }
                else if (!put_byte(out, cap, &n, (char)(0xE0 | (code >> 12))) ||
                         !put_byte(out, cap, &n, (char)(0x80 | ((code >> 6) & 0x3F))) ||
                         !put_byte(out, cap, &n, (char)(0x80 | (code & 0x3F))))
                    return 0;
                continue;
            }
            default: c = e; break; // \" \\ \/
            }
        }
        if (!put_byte(out, cap, &n, c))
            return 0;
    }
    if (j->at >= j->size)
        return 0;
    ++j->at; // the closing quote
    out[n] = '\0';
    return 1;
}

// Skips a value of another kind (number, true, false, null, object, array).
static int json_skip(struct json *j)
{
    json_space(j);
    int depth = 0;
    char ignored[256];
    while (j->at < j->size)
    {
        const char c = j->text[j->at];
        if (c == '"')
        {
            // A long string would not fit `ignored`; walk it by hand.
            ++j->at;
            while (j->at < j->size && j->text[j->at] != '"')
                j->at += j->text[j->at] == '\\' ? 2 : 1;
            ++j->at;
            if (depth == 0)
                return 1;
            continue;
        }
        if (c == '{' || c == '[')
            ++depth;
        else if (c == '}' || c == ']')
        {
            if (depth == 0)
                return 1;
            --depth;
            ++j->at;
            if (depth == 0)
                return 1;
            continue;
        }
        else if (c == ',' && depth == 0)
            return 1;
        ++j->at;
    }
    (void)ignored;
    return depth == 0;
}

struct config
{
    char target[16];
    char args[ARGS_SIZE]; // NUL-separated, as the request carries them
    size_t args_len;
    uint32_t argc;
};

static int parse_config(const char *text, size_t size, struct config *config, char *error,
                        size_t error_size)
{
    struct json j = {text, 0, size};
    memset(config, 0, sizeof(*config));
    if (!json_eat(&j, '{'))
    {
        snprintf(error, error_size, "is not a JSON object");
        return 0;
    }
    if (json_eat(&j, '}'))
        return 1;
    do
    {
        char key[64];
        if (!json_string(&j, key, sizeof(key)) || !json_eat(&j, ':'))
        {
            snprintf(error, error_size, "expected \"key\": value");
            return 0;
        }
        if (strcmp(key, "target") == 0)
        {
            if (!json_string(&j, config->target, sizeof(config->target)))
            {
                snprintf(error, error_size, "bad value for target");
                return 0;
            }
        }
        else if (strcmp(key, "args") == 0)
        {
            if (!json_eat(&j, '['))
            {
                snprintf(error, error_size, "bad value for args");
                return 0;
            }
            if (!json_eat(&j, ']'))
            {
                do
                {
                    if (config->argc >= MAX_ARGS)
                    {
                        snprintf(error, error_size, "too many entries in args");
                        return 0;
                    }
                    if (!json_string(&j, config->args + config->args_len,
                                     sizeof(config->args) - config->args_len))
                    {
                        snprintf(error, error_size,
                                 "bad value for args (or the arguments are too long)");
                        return 0;
                    }
                    config->args_len += strlen(config->args + config->args_len) + 1;
                    ++config->argc;
                } while (json_eat(&j, ','));
                if (!json_eat(&j, ']'))
                {
                    snprintf(error, error_size, "expected , or ] in args");
                    return 0;
                }
            }
        }
        else if (!json_skip(&j))
        {
            snprintf(error, error_size, "bad value for %s", key);
            return 0;
        }
    } while (json_eat(&j, ','));
    if (!json_eat(&j, '}'))
    {
        snprintf(error, error_size, "expected , or }");
        return 0;
    }
    json_space(&j);
    if (j.at != j.size)
    {
        snprintf(error, error_size, "text after the object");
        return 0;
    }
    return 1;
}

// PPSA / CUSA / LAPY followed by five digits.
static int valid_title(const char *t)
{
    if (strlen(t) != 9)
        return 0;
    if (memcmp(t, "PPSA", 4) != 0 && memcmp(t, "CUSA", 4) != 0 && memcmp(t, "LAPY", 4) != 0)
        return 0;
    for (int i = 4; i < 9; ++i)
        if (t[i] < '0' || t[i] > '9')
            return 0;
    return 1;
}

// The tile's own title ID, from its param.json.
static void read_own_title(void)
{
    static char param[MAX_TEXT];
    if (read_file("/app0/sce_sys/param.json", param, sizeof(param)) < 0)
        return;
    const char *key = strstr(param, "\"titleId\"");
    if (key == NULL)
        return;
    const char *colon = strchr(key + 9, ':');
    const char *open = colon != NULL ? strchr(colon, '"') : NULL;
    const char *close = open != NULL ? strchr(open + 1, '"') : NULL;
    if (open == NULL || close == NULL || close - open - 1 != 9)
        return;
    memcpy(g_title, open + 1, 9);
    g_title[9] = '\0';
}

// ---- sockets ------------------------------------------------------------------

static int connect_local(unsigned short port, int timeout_seconds)
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct timeval timeout = {timeout_seconds, 0};
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0)
    {
        close(fd);
        return -1;
    }
    return fd;
}

static int write_all(int fd, const unsigned char *data, size_t size)
{
    size_t done = 0;
    while (done < size)
    {
        const ssize_t sent = write(fd, data + done, size - done);
        if (sent <= 0)
            return 0;
        done += (size_t)sent;
    }
    return 1;
}

static int launcher_listening(void)
{
    const int fd = connect_local(LAUNCHER_PORT, 2);
    if (fd < 0)
        return 0;
    close(fd); // the launcher takes an empty connection as a probe
    return 1;
}

// Makes sure a launcher serves 127.0.0.1:10199, sending the one this tile
// carries to elfldr when nothing does.
static void ensure_launcher(void)
{
    if (launcher_listening())
    {
        log_line("a launcher is already running");
        return;
    }
    log_line("no launcher on 127.0.0.1:%d; sending the built-in one (%zu bytes) to elfldr",
             LAUNCHER_PORT, sizeof(kLauncherPayload));
    const int fd = connect_local(ELFLDR_PORT, 5);
    if (fd < 0)
        fail("no launcher is running, and elfldr is not reachable to start one");
    const int sent = write_all(fd, kLauncherPayload, sizeof(kLauncherPayload));
    (void)shutdown(fd, SHUT_WR);
    char sink[256];
    while (read(fd, sink, sizeof(sink)) > 0)
    {
    }
    close(fd);
    if (!sent)
        fail("could not send the launcher to elfldr");
    for (int attempt = 0; attempt < 30; ++attempt)
    {
        usleep(100 * 1000);
        if (launcher_listening())
        {
            log_line("built-in launcher is up after %d ms", (attempt + 1) * 100);
            return;
        }
    }
    fail("the launcher was sent to elfldr but did not start");
}

static void put_u32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static uint32_t get_u32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

int main(void)
{
    read_own_title();
    (void)mkdir("/data/ps5-forwarder", 0777);
    char log_path[64];
    (void)snprintf(log_path, sizeof(log_path), "/data/ps5-forwarder/%s.log", g_title);
    g_log = fopen(log_path, "w");

    // The configuration.
    static char text[MAX_TEXT];
    const char *path = "/app0/forwarder.json";
    long size = read_file(path, text, sizeof(text));
    if (size < 0)
    {
        path = "/app0/assets/forwarder.json";
        size = read_file(path, text, sizeof(text));
        if (size < 0)
            fail("forwarder.json is missing next to eboot.bin");
    }
    log_line("config: %s", path);
    static struct config config;
    char error[160];
    if (!parse_config(text, (size_t)size, &config, error, sizeof(error)))
    {
        char message[200];
        (void)snprintf(message, sizeof(message), "forwarder.json: %s", error);
        fail(message);
    }
    if (config.target[0] == '\0')
        fail("target is required");
    if (!valid_title(config.target))
        fail("target must be a title ID like PPSA99008");
    if (strcmp(config.target, g_title) == 0)
        fail("target is this forwarder's own title ID");

    // The request.
    static unsigned char request[REQUEST_SIZE];
    memset(request, 0, sizeof(request));
    put_u32(request + 0x00, MAGIC);
    put_u32(request + 0x04, VERSION);
    memcpy(request + 0x08, config.target, strlen(config.target));
    put_u32(request + 0x18, config.argc);
    put_u32(request + 0x1C, (uint32_t)config.args_len);
    memcpy(request + 0x20, config.args, config.args_len);
    char listed[ARGS_SIZE + 3 * MAX_ARGS + 16];
    size_t used = 0;
    listed[0] = '\0';
    for (size_t at = 0; at < config.args_len; at += strlen(config.args + at) + 1)
        used += (size_t)snprintf(listed + used, sizeof(listed) - used, " [%s]", config.args + at);
    log_line("starting %s with%s", config.target, config.argc == 0 ? " no arguments" : listed);

    // A launcher, started here if need be.
    ensure_launcher();

    const int fd = connect_local(LAUNCHER_PORT, 10);
    if (fd < 0)
        fail("the launcher is not running");
    log_line("sending the request");
    if (!write_all(fd, request, sizeof(request)))
    {
        close(fd);
        fail("could not send the request to the launcher");
    }
    unsigned char reply[REPLY_SIZE];
    size_t got = 0;
    while (got < sizeof(reply))
    {
        const ssize_t n = read(fd, reply + got, sizeof(reply) - got);
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    close(fd);
    if (got != sizeof(reply) || get_u32(reply) != MAGIC)
        fail("no reply from the launcher; see /data/forwarder-manager/launcher.log");
    const uint32_t stage = get_u32(reply + 0x08);
    const uint32_t first = get_u32(reply + 0x0C);
    const uint32_t second = get_u32(reply + 0x10);
    log_line("launcher reply: stage=%u first_rc=0x%08X second_rc=0x%08X user=0x%08X", stage,
             first, second, get_u32(reply + 0x14));

    char message[200];
    switch (stage)
    {
    case 0:
        break;
    case 2:
        (void)snprintf(message, sizeof(message), "no signed-in user to start %s for",
                       config.target);
        fail(message);
    case 3:
        if (first == QUEUED && second == QUEUED)
            break; // queued behind this tile: the system closes it and starts the target
        (void)snprintf(message, sizeof(message), "could not start %s (0x%08X, 0x%08X)",
                       config.target, first, second);
        fail(message);
    default:
        (void)snprintf(message, sizeof(message), "the launcher rejected the request (stage %u)",
                       stage);
        fail(message);
    }

    // The system closes this tile as the target starts.
    log_line("launch requested; waiting to be closed");
    for (int i = 0; i < 200; ++i)
        usleep(100 * 1000);
    log_line("still open after 20 s; closing");
    if (g_log != NULL)
        fclose(g_log);
    return 0;
}
