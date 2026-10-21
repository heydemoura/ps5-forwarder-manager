// ps5fwdgen - One game in an emulator's library, as a forwarder can start it.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

namespace fwd
{

struct LibraryGame
{
    std::string name;     // as the emulator shows it
    std::string path;     // what the forwarder gives the emulator
    std::string kind;     // the format, shown beside the name ("XEX", "CUE"...)
    std::string cover;    // a cover image file, if any
    std::string location; // the file this app reads (path, unless the emulator sees it elsewhere)
};

// The name to show for the game a forwarder of target starts at path: the
// game's folder for PS5X360's extracted games, a readable ROM title for
// SwanStationPS5 and ProsperoEden. Does not read files while drawing.
std::string library_game_name(const std::string &target, const std::string &path);

} // namespace fwd
