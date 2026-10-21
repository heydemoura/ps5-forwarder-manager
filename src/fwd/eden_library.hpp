// ps5fwdgen - ProsperoEden's game files, cached names and covers.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "fwd/library_game.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace fwd
{

inline constexpr char kEdenDataRoot[] = "/data/prosperoeden";

// game_files in config/prosperoeden.json, or data_root when absent/invalid.
// Covers remain in data_root/covers even when game_files is on another drive.
std::string eden_game_files(const std::string &data_root = kEdenDataRoot);

// The lowercase eight-digit FNV-1a key of a ROM's path inside roms/.
std::string eden_cover_key(std::string_view relative_path);

// Loose ROMs and ROMs one game folder down. Recognisable update/DLC file
// names are left out; encrypted package contents are not inspected.
std::vector<LibraryGame> read_eden_library(const std::string &data_root = kEdenDataRoot);

// A file-browser choice: relative to the configured roms/ when inside it,
// absolute otherwise. Uses the cached name and cover when available.
LibraryGame eden_game_from_file(const std::string &path,
                                const std::string &data_root = kEdenDataRoot);

// A readable filename fallback, without dump tags and the extension. Does
// no disk reads, so it can also be used while drawing an existing tile.
std::string eden_game_title(std::string_view path);

} // namespace fwd
