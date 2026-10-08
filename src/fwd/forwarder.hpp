// ps5fwdgen - The forwarder model and its on-disk form.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A forwarder is a home-screen tile that asks ps5-app-launcher to start
// another app (an emulator) with launch arguments. Every forwarder shares the
// same eboot.bin and libc.prx; only forwarder.json, param.json and the
// pictures differ. This mirrors ps5-forwarder.mph.am.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fwd
{

// One launch argument. value is empty for a flag such as --exit-after-game.
struct Argument
{
    std::string key;
    std::string value;
};

// A known target app, for the picker.
struct Target
{
    const char *title_id;
    const char *name;
    bool rom_arg;          // takes --rom <file>
    bool exit_after_game;  // supports --exit-after-game
};

// The built-in emulator targets the site offers, plus "generic".
std::vector<Target> known_targets();

struct Forwarder
{
    std::string title_id;     // PPSA99xxx (the folder name)
    std::string display_name; // shown under the tile
    std::string target;       // title ID of the app to launch
    std::string rom;          // ROM file path/name passed as --rom (optional)
    bool exit_after_game = false;
    std::vector<Argument> extra_args; // anything beyond rom/exit flags

    // Present on disk; the app keeps the paths so an edit can leave art alone.
    bool has_icon = false;
    bool has_backgrounds = false;
    bool has_music = false;

    // Build the launch argument list the way the site does: --rom first (when
    // set), then the extra arguments, then --exit-after-game (when set).
    std::vector<Argument> resolved_args() const;

    // The forwarder.json body ({"target":..,"args":[..]} with a trailing \n).
    std::string forwarder_json() const;
    // The sce_sys/param.json body for this title (site-compatible).
    std::string param_json() const;
};

// Make a fresh, unused PPSA99xxx id (99200..99899), avoiding ids already in
// forwarders_root and the reserved emulator ids.
std::string new_title_id(const std::string &forwarders_root);

// A title ID is PPSA/CUSA/LAPY/FAKE followed by five digits.
bool valid_title_id(const std::string &id);

} // namespace fwd
