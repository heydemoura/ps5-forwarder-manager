// ps5fwdgen - Search SteamGridDB and pick art (icon grids or hero backgrounds).
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "app/screen.hpp"
#include "app/screens/art_screen.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fwd
{
struct Context;

// on_image receives the downloaded, still-encoded image bytes (PNG/JPEG from
// the CDN); the caller re-encodes to the PS5 format. The screen pops itself.
std::unique_ptr<Screen>
make_steamgriddb_screen(Context &context, ArtKind kind, std::string query,
                        std::function<void(std::vector<unsigned char>)> on_image);
} // namespace fwd
