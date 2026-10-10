// ps5fwdgen - Sandbox elevation through PS5-Lapy-JB-Daemon.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Lifts this process out of its sandbox so /data/homebrew can be read and
// written, with Lapy (https://github.com/ArkSama/PS5-Lapy-JB-Daemon). In order:
//
//   1. /data is already fully usable: nothing to do.
//   2. A resident ArkSama daemon: it serves etaHEN's jailbreak-on-demand file,
//      /download0/etahen_jailbreak = {"PID":"<pid>"}, and deletes it when done.
//   3. A cooperative resident Lapy service (/download0/elevate_proc).
//   4. The exact-title one-shot helper bundled with the app (/app0/lapy.elf,
//      built from mpereiraesaa's Lapy fork by APP_LAPY_HELPER=1), streamed to
//      the local ELF loader on port 9021.
//
// Once a resident service claims a request, no helper is started for that
// launch. Only a real /data write/read/list proof returns ok.
#pragma once

#include <cstdint>

namespace elevation
{

enum class Capability : std::uint32_t
{
    filesystem = 1,
};

// Values 1-11 are the helper's wire statuses (elevation_protocol.hpp).
enum class Status : std::uint32_t
{
    ok = 0,
    invalid_request = 1,
    unsupported_version = 2,
    unsupported_capability = 3,
    target_mismatch = 4,
    unavailable = 5, // no helper file, or no writable /download0
    prepare_failed = 6,
    apply_failed = 7,
    rollback_failed = 8,
    transport_error = 9, // e.g. no ELF loader on port 9021
    protocol_error = 10,
    timeout = 11,
    // The resident daemon took the request, but /data is still not usable.
    denied = 12,
};

// Call once during single-threaded startup, before any thread exists.
[[nodiscard]] Status request(Capability capability,
                             const char *helper_path = "/app0/lapy.elf") noexcept;

// Diagnostics: "existing", "daemon", "resident", "helper" or "none".
[[nodiscard]] const char *path() noexcept;

} // namespace elevation
