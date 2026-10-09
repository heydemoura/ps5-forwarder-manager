// ps5fwdgen - The forwarder model and its on-disk form.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A forwarder is a home-screen tile that starts another app (an emulator)
// with launch arguments, in the PS5 Forwarder Format (external/ps5-forwarder-
// format). This is the app's view of one: a ROM, an exit flag and extra
// arguments, which the store maps to the format's plain argument list.
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

// A friendly name for a target title ID (emulator name, or the ID itself).
std::string target_display_name(const std::string &title_id);

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

};

// Make a fresh, unused PPSA99xxx id (99200..99899), avoiding ids already in
// forwarders_root and the reserved emulator ids.
std::string new_title_id(const std::string &forwarders_root);

// A title ID is PPSA/CUSA/LAPY followed by five digits.
bool valid_title_id(const std::string &id);

} // namespace fwd
