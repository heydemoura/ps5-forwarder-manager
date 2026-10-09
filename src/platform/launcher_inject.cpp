// ps5fwdgen - Make sure a forwarder launcher is running.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform/launcher_inject.hpp"

#include "platform/ps5/system.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

namespace launcher
{

namespace
{

constexpr unsigned short kLauncherPort = 10199;
constexpr unsigned short kElfLoaderPort = 9021;

std::atomic<int> g_state{static_cast<int>(State::checking)};
std::string g_payload;

void set_state(State state)
{
    g_state.store(static_cast<int>(state));
}

// A connected TCP socket to 127.0.0.1:port, or -1.
int connect_local(unsigned short port, int timeout_seconds)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    timeval timeout{timeout_seconds, 0};
    (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0)
    {
        ::close(fd);
        return -1;
    }
    return fd;
}

// Something accepts connections on the launcher port. The launcher treats a
// connection closed without a request as a probe and says nothing.
bool launcher_listening()
{
    const int fd = connect_local(kLauncherPort, 2);
    if (fd < 0)
        return false;
    ::close(fd);
    return true;
}

bool read_file(const std::string &path, std::vector<unsigned char> &out)
{
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (file == nullptr)
        return false;
    unsigned char buffer[65536];
    std::size_t got;
    while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
        out.insert(out.end(), buffer, buffer + got);
    std::fclose(file);
    return !out.empty();
}

// elfldr reads an ELF until the sender closes its side, then runs it.
bool send_to_elfldr(const std::vector<unsigned char> &elf)
{
    const int fd = connect_local(kElfLoaderPort, 5);
    if (fd < 0)
        return false;
    std::size_t done = 0;
    while (done < elf.size())
    {
        const ssize_t sent = ::write(fd, elf.data() + done, elf.size() - done);
        if (sent <= 0)
            break;
        done += static_cast<std::size_t>(sent);
    }
    (void)::shutdown(fd, SHUT_WR);
    char sink[256];
    while (::read(fd, sink, sizeof(sink)) > 0)
    {
    }
    ::close(fd);
    return done == elf.size();
}

void *run(void *)
{
    if (launcher_listening())
    {
        set_state(State::already_up);
        hui::sys::log("[FWD] launcher: one is already serving 127.0.0.1:%u", kLauncherPort);
        return nullptr;
    }
    std::vector<unsigned char> elf;
    if (!read_file(g_payload, elf))
    {
        set_state(State::failed);
        hui::sys::log("[FWD] launcher: payload missing at %s", g_payload.c_str());
        return nullptr;
    }
    if (!send_to_elfldr(elf))
    {
        set_state(State::no_elfldr);
        hui::sys::log("[FWD] launcher: elfldr did not take the payload (127.0.0.1:%u)",
                      kElfLoaderPort);
        return nullptr;
    }
    // The payload binds within a moment of being loaded.
    for (int attempt = 0; attempt < 30; ++attempt)
    {
        ::usleep(100 * 1000);
        if (launcher_listening())
        {
            set_state(State::injected);
            hui::sys::log("[FWD] launcher: built-in launcher injected (%zu bytes), up after %d ms",
                          elf.size(), (attempt + 1) * 100);
            return nullptr;
        }
    }
    set_state(State::failed);
    hui::sys::log("[FWD] launcher: injected but nothing listens on %u", kLauncherPort);
    return nullptr;
}

} // namespace

void ensure_running(const std::string &payload_path)
{
    g_payload = payload_path;
    set_state(State::checking);
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 256u << 10);
    if (pthread_create(&thread, &attr, run, nullptr) != 0)
        run(nullptr);
    pthread_attr_destroy(&attr);
}

State state()
{
    return static_cast<State>(g_state.load());
}

const char *describe(State state)
{
    switch (state)
    {
    case State::checking:
        return "Checking for a forwarder launcher...";
    case State::already_up:
        return "Forwarder launcher: running";
    case State::injected:
        return "Forwarder launcher: built-in, started by this app";
    case State::no_elfldr:
        return "Forwarder launcher: not running (elfldr is not reachable)";
    case State::failed:
        return "Forwarder launcher: could not be started";
    }
    return "";
}

} // namespace launcher
