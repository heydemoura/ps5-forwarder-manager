// ps5fwdgen - Make sure a forwarder launcher is running.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Forwarder tiles ask a resident payload on 127.0.0.1:10199 to start their
// target. Forwarder Manager ships its own open-source launcher
// (launcher/fwd_launcher.c, built into assets/launcher/fwd-launcher.elf) and,
// when nothing answers on that port, sends it to elfldr (127.0.0.1:9021) so
// forwarders work without any other payload loaded. A launcher that is already
// running (this one from an earlier start, or ps5-app-launcher) is left alone.
#pragma once

#include <string>

namespace launcher
{

enum class State
{
    checking,      // the probe has not finished yet
    already_up,    // something already served the port when the app started
    injected,      // the built-in launcher was sent to elfldr and came up
    no_elfldr,     // nothing on the port, and elfldr refused the connection
    failed,        // sent, but the port never came up (or the payload is missing)
};

// Starts the check on a background thread; returns at once.
void ensure_running(const std::string &payload_path);

State state();
const char *describe(State state);

} // namespace launcher
