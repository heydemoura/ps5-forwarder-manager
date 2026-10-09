// ps5fwdgen - Sandbox elevation through a resident Lapy JB daemon.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform/elevation.hpp"

#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

namespace elevation
{

namespace
{

// The app's own sandbox path; the daemon sees it as
// /mnt/sandbox/<TITLE_ID>_NNN/download0/etahen_jailbreak and deletes it when
// the bump is done.
constexpr char kRequestPath[] = "/download0/etahen_jailbreak";
// Where a successful bump lets us reach the real console filesystem.
constexpr char kProofDir[] = "/data";

// The daemon polls every 250 ms; give it a generous window.
constexpr int kPollIntervalUs = 100 * 1000;
constexpr int kPollCount = 80; // ~8 seconds

const char *g_path = "none";

// A full /data proof: create, write, read back, compare, remove a
// PID-specific file, AND list the directory. ShadowMountPlus can mount /data
// into a sandboxed app so a file opens but readdir is refused; that is not
// elevation, so the listing must succeed too.
bool data_accessible() noexcept
{
    char probe[128];
    (void)std::snprintf(probe, sizeof(probe), "%s/.ps5fwdgen-elev-%ld", kProofDir,
                        static_cast<long>(getpid()));
    char payload[64];
    const int payload_length =
        std::snprintf(payload, sizeof(payload), "ps5fwdgen elevation proof pid=%ld\n",
                      static_cast<long>(getpid()));
    if (payload_length <= 0)
        return false;

    bool wrote = false;
    {
        const int fd = ::open(probe, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd >= 0)
        {
            wrote =
                ::write(fd, payload, static_cast<std::size_t>(payload_length)) == payload_length;
            ::close(fd);
        }
    }
    if (!wrote)
        return false;

    char readback[64] = {};
    bool matched = false;
    {
        const int fd = ::open(probe, O_RDONLY);
        if (fd >= 0)
        {
            const ssize_t got = ::read(fd, readback, sizeof(readback) - 1);
            ::close(fd);
            matched = got == payload_length &&
                      std::memcmp(readback, payload, static_cast<std::size_t>(got)) == 0;
        }
    }
    (void)::unlink(probe);
    if (!matched)
        return false;

    // The listing must work too.
    DIR *dir = ::opendir(kProofDir);
    if (dir == nullptr)
        return false;
    const bool listed = ::readdir(dir) != nullptr;
    ::closedir(dir);
    return listed;
}

// Writes {"PID":"<pid>"} where the daemon expects it. Returns false when the
// request file could not be created (no writable /download0).
bool publish_request() noexcept
{
    char payload[64];
    const int length =
        std::snprintf(payload, sizeof(payload), "{\"PID\":\"%ld\"}", static_cast<long>(getpid()));
    if (length <= 0)
        return false;
    const int fd = ::open(kRequestPath, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return false;
    const bool ok = ::write(fd, payload, static_cast<std::size_t>(length)) == length;
    ::close(fd);
    return ok;
}

bool request_consumed() noexcept
{
    return ::access(kRequestPath, F_OK) != 0;
}

} // namespace

Status request(Capability capability, const char *helper_path) noexcept
{
    (void)helper_path;
    if (capability != Capability::filesystem)
        return Status::invalid_request;

    // Already outside the sandbox (etaHEN, a previous launch, or a resident
    // service that bumped us before we asked)?
    if (data_accessible())
    {
        g_path = "existing";
        return Status::ok;
    }

    if (!publish_request())
    {
        g_path = "none";
        return Status::unavailable;
    }

    // Wait for the daemon to pick up the request (it deletes the file when
    // the bump is applied), checking for access along the way.
    bool consumed = false;
    for (int poll = 0; poll < kPollCount; ++poll)
    {
        ::usleep(kPollIntervalUs);
        if (request_consumed())
        {
            consumed = true;
            break;
        }
        if (data_accessible())
        {
            g_path = "daemon";
            return Status::ok;
        }
    }

    // Give the credential change a moment to settle after the file vanished,
    // then make the real proof the only thing that authorises /data use.
    for (int settle = 0; settle < 10; ++settle)
    {
        if (data_accessible())
        {
            g_path = "daemon";
            return Status::ok;
        }
        ::usleep(kPollIntervalUs);
    }

    // Tidy up a request the daemon never read, so a later launch starts clean.
    if (!consumed)
        (void)::unlink(kRequestPath);
    g_path = "none";
    return consumed ? Status::denied : Status::timeout;
}

const char *path() noexcept
{
    return g_path;
}

} // namespace elevation
