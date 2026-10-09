// ps5fwdgen - Shared rendering of a forwarder as a console-style tile/preview.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "fwd/forwarder.hpp"
#include "gfx/backdrop_spec.hpp"
#include "gfx/draw_list.hpp"

#include <array>
#include <string>

namespace hui::gfx
{
class Renderer;
}

namespace fwd
{

// A four-colour backdrop palette (for gfx::BackdropMode::aurora) derived from a
// seed string, so the screen's colour follows the selection.
// The shared Aurora backdrop for every screen but the home (which colours it
// per selection), so the whole interface shares one design language.
hui::gfx::BackdropSpec app_backdrop();

std::array<hui::gfx::Color, 4> palette_for(const std::string &seed);

// The accent colour for a seed (glows, focus rings, labels).
hui::gfx::Color accent_for(const std::string &seed);

// Decode a forwarder's icon0.png (if present) and upload it as a GL texture;
// returns 0 when there is no icon. `dir` is the forwarder's folder.
std::uint32_t load_icon_texture(hui::gfx::Renderer &renderer, const std::string &dir);

// Decode an in-memory PNG/JPEG and upload it; 0 on failure.
std::uint32_t upload_image_texture(hui::gfx::Renderer &renderer, const unsigned char *data,
                                   std::size_t size);

} // namespace fwd
