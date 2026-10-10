// ps5fwdgen - Elevation client regression: resident daemon, then Lapy helper.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Covers src/platform/elevation.cpp with the same stubbing approach as
// test_elevation.cpp: ArkSama's daemon step comes first, and the bundled
// helper runs only when nothing claimed a request.
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "../src/platform/elevation.cpp"

namespace test
{
struct State
{
    bool data_ok{};          // /data passes the probe
    bool data_after_claim{}; // the daemon lifts us when it takes the request
    bool daemon_claims{};    // a resident daemon deletes etahen_jailbreak
    int stat_errno{};        // what stat() reports while the file is there
    bool daemon_file{};
    unsigned socket_calls{};
    bool helper_read{};
    bool helper_done{};
    std::string daemon_request;
    std::vector<std::uint8_t> replies;
    std::size_t received{};
};

State state;

void reset()
{
    state = {};
    state.stat_errno = 0;
}

bool data_accessible()
{
    const bool claimed = !state.daemon_request.empty() && !state.daemon_file;
    return state.data_ok || state.helper_done || (state.data_after_claim && claimed);
}

void helper_replies()
{
    elevation::wire::Message prepare{};
    prepare.pid = 4242;
    prepare.kind = elevation::wire::Kind::prepare;
    auto response = prepare;
    response.kind = elevation::wire::Kind::response;
    state.replies.resize(2 * sizeof(prepare));
    std::memcpy(state.replies.data(), &prepare, sizeof(prepare));
    std::memcpy(state.replies.data() + sizeof(prepare), &response, sizeof(response));
}
} // namespace test

extern "C"
{
    pid_t getpid() noexcept
    {
        return 4242;
    }

    uid_t geteuid() noexcept
    {
        return 1000;
    }

    int seteuid(uid_t) noexcept
    {
        return 0;
    }

    int open(const char *path, int, ...)
    {
        const std::string_view name{path};
        if (name == "/download0/etahen_jailbreak")
        {
            test::state.daemon_file = true;
            test::state.daemon_request.clear();
            return 14;
        }
        if (name == "/download0/lapy_owned_result")
            return 10;
        if (name.starts_with("/download0/.elevate_proc."))
            return 11;
        if (name.starts_with("/data/.lapy_probe_"))
        {
            if (!test::data_accessible())
            {
                errno = EACCES;
                return -1;
            }
            return 12;
        }
        if (name == "/app0/lapy.elf")
            return 13;
        errno = ENOENT;
        return -1;
    }

    int stat(const char *path, struct stat *) noexcept
    {
        if (std::strcmp(path, "/download0/etahen_jailbreak") != 0)
        {
            errno = ENOENT;
            return -1;
        }
        // A resident daemon takes the request on its first poll.
        if (test::state.daemon_file && test::state.daemon_claims)
            test::state.daemon_file = false;
        if (!test::state.daemon_file)
        {
            errno = ENOENT;
            return -1;
        }
        if (test::state.stat_errno != 0)
        {
            errno = test::state.stat_errno;
            return -1;
        }
        return 0;
    }

    DIR *opendir(const char *path)
    {
        if (std::strcmp(path, "/data") != 0 || !test::data_accessible())
        {
            errno = EPERM;
            return nullptr;
        }
        return reinterpret_cast<DIR *>(&test::state);
    }

    int closedir(DIR *)
    {
        return 0;
    }

    std::string probe_data;

    ssize_t write(int descriptor, const void *buffer, size_t size)
    {
        const std::string_view bytes{static_cast<const char *>(buffer), size};
        if (descriptor == 14)
            test::state.daemon_request.append(bytes);
        else if (descriptor == 12)
            probe_data.assign(bytes);
        else if (descriptor != 10 && descriptor != 11)
        {
            errno = EBADF;
            return -1;
        }
        return static_cast<ssize_t>(size);
    }

    ssize_t read(int descriptor, void *buffer, size_t size)
    {
        if (descriptor == 13)
        {
            if (test::state.helper_read)
                return 0;
            test::state.helper_read = true;
            std::memcpy(buffer,
                        "\x7f"
                        "ELF",
                        std::min<size_t>(size, 4));
            return 4;
        }
        if (descriptor != 12)
        {
            errno = EBADF;
            return -1;
        }
        const auto count = std::min(size, probe_data.size());
        std::memcpy(buffer, probe_data.data(), count);
        return static_cast<ssize_t>(count);
    }

    off_t lseek(int descriptor, off_t offset, int whence) noexcept
    {
        return descriptor == 12 && offset == 0 && whence == SEEK_SET ? 0 : -1;
    }

    int close(int)
    {
        return 0;
    }

    int fchmod(int, mode_t) noexcept
    {
        return 0;
    }

    int rename(const char *, const char *) noexcept
    {
        return 0;
    }

    int unlink(const char *path) noexcept
    {
        if (std::strcmp(path, "/download0/etahen_jailbreak") == 0)
        {
            if (!test::state.daemon_file)
            {
                errno = ENOENT;
                return -1;
            }
            test::state.daemon_file = false;
        }
        return 0;
    }

    int usleep(useconds_t)
    {
        return 0;
    }

    int sceNetSocket(const char *, int, int, int)
    {
        ++test::state.socket_calls;
        return 20;
    }

    int sceNetSocketClose(int)
    {
        return 0;
    }

    int sceNetConnect(int, const void *, std::uint32_t)
    {
        return 0;
    }

    int sceNetSetsockopt(int, int, int, const void *, std::uint32_t)
    {
        return 0;
    }

    int sceNetSend(int, const void *, std::size_t size, int)
    {
        return static_cast<int>(size);
    }

    int sceNetRecv(int, void *bytes, std::size_t size, int)
    {
        const auto remaining = test::state.replies.size() - test::state.received;
        if (remaining == 0)
            return -1;
        const auto count = std::min(size, remaining);
        std::memcpy(bytes, test::state.replies.data() + test::state.received, count);
        test::state.received += count;
        if (test::state.received == test::state.replies.size())
            test::state.helper_done = true;
        return static_cast<int>(count);
    }
}

int main()
{
    using elevation::Capability;
    using elevation::Status;

    // Already elevated: nothing is asked.
    test::reset();
    test::state.data_ok = true;
    assert(elevation::request(Capability::filesystem) == Status::ok);
    assert(std::strcmp(elevation::path(), "existing") == 0);
    assert(test::state.daemon_request.empty() && test::state.socket_calls == 0);

    // A resident ArkSama daemon takes the request and lifts the app.
    test::reset();
    test::state.daemon_claims = true;
    test::state.data_after_claim = true;
    assert(elevation::request(Capability::filesystem) == Status::ok);
    assert(std::strcmp(elevation::path(), "daemon") == 0);
    assert(test::state.daemon_request == "{\"PID\":\"4242\"}");
    assert(test::state.socket_calls == 0);

    // No daemon, and the sandbox refuses stat() on the request file (seen on
    // the console): that is not a claim. The bundled helper does the work.
    test::reset();
    test::state.stat_errno = EPERM;
    test::helper_replies();
    assert(elevation::request(Capability::filesystem) == Status::ok);
    assert(std::strcmp(elevation::path(), "helper") == 0);
    assert(test::state.socket_calls == 1 && !test::state.daemon_file);

    // No daemon at all: the helper runs, and the request file is cleaned up.
    test::reset();
    test::helper_replies();
    assert(elevation::request(Capability::filesystem) == Status::ok);
    assert(std::strcmp(elevation::path(), "helper") == 0);
    assert(test::state.socket_calls == 1 && !test::state.daemon_file);

    // The daemon takes the request but /data stays shut: never a second
    // elevation attempt for this launch.
    test::reset();
    test::state.daemon_claims = true;
    assert(elevation::request(Capability::filesystem) == Status::denied);
    assert(std::strcmp(elevation::path(), "daemon") == 0);
    assert(test::state.socket_calls == 0);
}
