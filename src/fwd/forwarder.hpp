// ps5fwdgen - The forwarder model and its on-disk form.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A forwarder is a home-screen tile that starts another app (an emulator)
// with launch arguments, in the PS5 Forwarder Format (external/ps5-forwarder-
// format). This is the app's view of one: a ROM, an exit flag and extra
// arguments, which the store maps to the format's plain argument list.
#pragma once

#include "fwd/features.hpp"

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

// Whose game list the edit screen offers for a target (fwd/x360_library.hpp,
// fwd/psx_library.hpp, fwd/eden_library.hpp); none: a ROM file is picked
// from the file browser.
enum class GameLibrary : std::uint8_t
{
    none,
    x360, // PS5X360
    psx,  // SwanStationPS5
    eden, // ProsperoEden
};

// A known target app, for the picker.
struct Target
{
    const char *title_id;
    const char *name;
    bool rom_arg;         // takes --rom <file>
    bool exit_after_game; // supports --exit-after-game
    // A streaming client (ProsperoLight): no ROM, but a PC and one of its apps,
    // passed as --pc <name or address> and --app <name or number>; its exit
    // flag is --exit-after-stream.
    bool stream = false;
    Feature feature = Feature::none; // offered only while this flag is on
    // Takes the game as its only argument, a full path, instead of --rom
    // (PS5X360, see fwd/x360_library.hpp).
    bool game_path = false;
    GameLibrary library = GameLibrary::none; // games listed for it, with their launch paths
};

// The built-in emulator targets the site offers, plus "generic", without the
// ones whose feature flag is off (fwd/features.hpp). A forwarder on disk whose
// target is not listed shows its title ID and edits as a generic target.
std::vector<Target> known_targets();

// True when the target is a streaming client rather than an emulator.
bool is_stream_target(const std::string &title_id);

// True when the target takes the game's full path as its only argument.
bool takes_game_path(const std::string &title_id);

// The emulator's game list the target is picked from; none for a ROM file.
GameLibrary library_of(const std::string &title_id);

// ROM selected in the file browser: ProsperoEden takes the path inside roms/
// (portable with its game files folder), or the full path outside it. Other
// file-picker targets keep their existing file-name-only arguments.
std::string rom_path_for(const std::string &title_id, const std::string &path);

// A friendly name for a target title ID (emulator name, or the ID itself).
std::string target_display_name(const std::string &title_id);

struct Forwarder
{
    std::string title_id;     // PPSA99xxx (the folder name)
    std::string display_name; // shown under the tile
    std::string target;       // title ID of the app to launch
    std::string rom;          // ROM file path/name passed as --rom, or the game path (optional)
    std::string pc;           // streaming target: the saved PC (optional: the last one used)
    std::string app;          // streaming target: the app to start on that PC
    bool exit_after_game = false;
    std::vector<Argument> extra_args; // anything beyond rom/exit flags

    // Present on disk; the app keeps the paths so an edit can leave art alone.
    bool has_icon = false;
    bool has_backgrounds = false;
    bool has_music = false;

    // Build the launch argument list the way the site does: --rom first (when
    // set), then the extra arguments, then --exit-after-game (when set). A
    // streaming target gets --pc and --app first and --exit-after-stream last;
    // a target that takes a game path gets the path alone first.
    std::vector<Argument> resolved_args() const;
};

// Make a fresh, unused PPSA99xxx id (99200..99899), avoiding ids already in
// forwarders_root and the reserved emulator ids.
std::string new_title_id(const std::string &forwarders_root);

// A title ID is PPSA/CUSA/LAPY followed by five digits.
bool valid_title_id(const std::string &id);

} // namespace fwd
