// ps5fwdgen - SteamGridDB API v2 client (search, grids, heroes, download).
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
//
// https://www.steamgriddb.com/api/v2 . Every call needs the user's API key
// (Authorization: Bearer). All calls block; run them on a worker thread.
#pragma once

#include <string>
#include <vector>

namespace sgdb
{

struct Game
{
    long id = 0;
    std::string name;
};

struct Asset
{
    long id = 0;
    std::string url;   // full-size image
    std::string thumb; // JPEG thumbnail
    int width = 0;
    int height = 0;
    std::string style;
    std::string author;
};

// Which art to fetch. icon -> square grids (plus an icons fallback);
// background -> heroes.
enum class Kind
{
    icon,
    background,
};

struct Result
{
    bool ok = false;
    std::string error; // set when ok is false (shown to the user)
};

// Search games by name. Fills out with up to a handful of matches.
Result search(const std::string &key, const std::string &term, std::vector<Game> &out);

// List art for a game. For icon: square grids 512/1024 (then 1024 icons as a
// fallback). For background: heroes at 1920x620 / 3840x1240.
Result assets(const std::string &key, Kind kind, long game_id, std::vector<Asset> &out);

// Download an image URL to memory (no key needed for the CDN).
Result download(const std::string &url, std::vector<unsigned char> &out);

} // namespace sgdb
