// ps5fwdgen - Choose a tile icon or background: from a file, or SteamGridDB.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "app/screen.hpp"

#include <functional>
#include <memory>
#include <vector>

namespace fwd
{
struct Context;

enum class ArtKind
{
    icon,       // -> 512x512 PNG
    background, // -> 3840x2160 BC7 DDS
};

// on_ready receives the already-encoded bytes (PNG for icon, DDS for
// background), then the screen pops itself.
std::unique_ptr<Screen> make_art_screen(Context &context, ArtKind kind, std::string suggested_query,
                                        std::function<void(std::vector<unsigned char>)> on_ready);
} // namespace fwd
