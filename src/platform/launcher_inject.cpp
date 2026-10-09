// ps5fwdgen - Make sure a forwarder launcher is running.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform/launcher_inject.hpp"

#include "platform/ps5/system.hpp"
#include "psfwd.h"

#include <atomic>
#include <cstdio>
#include <pthread.h>
#include <vector>

namespace launcher
{

namespace
{

std::atomic<int> g_state{static_cast<int>(State::checking)};
std::string g_payload;

void set_state(State state)
{
    g_state.store(static_cast<int>(state));
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

void *run(void *)
{
    if (psfwd_launcher_running())
    {
        set_state(State::already_up);
        hui::sys::log("[FWD] launcher: one is already serving 127.0.0.1:%d", PSFWD_LAUNCHER_PORT);
        return nullptr;
    }
    std::vector<unsigned char> elf;
    if (!read_file(g_payload, elf))
    {
        set_state(State::failed);
        hui::sys::log("[FWD] launcher: payload missing at %s", g_payload.c_str());
        return nullptr;
    }
    if (!psfwd_ensure_launcher(elf.data(), elf.size()))
    {
        // Either elfldr refused the payload, or it never came up.
        set_state(State::no_elfldr);
        hui::sys::log("[FWD] launcher: could not start it through elfldr (127.0.0.1:%d)",
                      PSFWD_ELFLDR_PORT);
        return nullptr;
    }
    set_state(State::injected);
    hui::sys::log("[FWD] launcher: built-in launcher injected (%zu bytes)", elf.size());
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
