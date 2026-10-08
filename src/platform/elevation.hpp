// ps5fwdgen - Sandbox elevation through a resident Lapy JB daemon.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Asks ArkSama's PS5-Lapy-JB-Daemon (which must already be running on the
// console) to lift this process out of its sandbox, so /data/homebrew can be
// read and written. The daemon mimics etaHEN's jailbreak-on-demand API: the
// app drops a request file naming its PID under its own /download0, the daemon
// bumps the process credentials and deletes the file to say it is done.
//
// See https://github.com/ArkSama/PS5-Lapy-JB-Daemon.
#pragma once

#include <cstdint>

namespace elevation
{

enum class Capability : std::uint32_t
{
    filesystem = 1,
};

enum class Status : std::uint32_t
{
    ok = 0,              // /data was created, written, read back and listed
    invalid_request = 1, // unsupported capability
    unavailable = 2,     // request could not be written (no /download0)
    timeout = 11,        // the daemon never processed the request
    // The request was processed but /data still is not fully accessible
    // (no daemon running, or it refused this title).
    denied = 12,
};

// Call once during single-threaded startup. Returns ok only after a real
// /data write/read/list proof. helper_path is accepted for interface
// compatibility and ignored: this client uses the resident daemon only.
[[nodiscard]] Status request(Capability capability, const char *helper_path = nullptr) noexcept;

// Diagnostics: "existing" (already had access), "daemon" (the daemon lifted
// us), or "none".
[[nodiscard]] const char *path() noexcept;

} // namespace elevation
