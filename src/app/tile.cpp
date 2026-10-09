// ps5fwdgen - Shared rendering of a forwarder as a console-style tile/preview.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/tile.hpp"

#include "fwd/image.hpp"
#include "gfx/gl_batch.hpp"
#include "gfx/renderer.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace fwd
{

namespace
{

// A stable 0..1 hue from a string.
float hue_of(const std::string &seed)
{
    std::uint32_t h = 2166136261u;
    for (const char c : seed)
    {
        h ^= static_cast<unsigned char>(c);
        h *= 16777619u;
    }
    return static_cast<float>(h % 3600u) / 3600.0f;
}

hui::gfx::Color hsv(float h, float s, float v)
{
    h = h - static_cast<float>(static_cast<int>(h));
    const float i = h * 6.0f;
    const int seg = static_cast<int>(i) % 6;
    const float f = i - static_cast<float>(static_cast<int>(i));
    const float p = v * (1.0f - s);
    const float q = v * (1.0f - s * f);
    const float t = v * (1.0f - s * (1.0f - f));
    float r = v;
    float g = t;
    float b = p;
    switch (seg)
    {
    case 0:
        r = v;
        g = t;
        b = p;
        break;
    case 1:
        r = q;
        g = v;
        b = p;
        break;
    case 2:
        r = p;
        g = v;
        b = t;
        break;
    case 3:
        r = p;
        g = q;
        b = v;
        break;
    case 4:
        r = t;
        g = p;
        b = v;
        break;
    default:
        r = v;
        g = p;
        b = q;
        break;
    }
    return {r, g, b, 1.0f};
}

} // namespace

hui::gfx::BackdropSpec app_backdrop()
{
    hui::gfx::BackdropSpec spec;
    spec.mode = hui::gfx::BackdropMode::aurora;
    const std::array<hui::gfx::Color, 4> p = palette_for("ps5fwdgen");
    spec.colors[0] = p[0];
    spec.colors[1] = p[1];
    spec.colors[2] = p[2];
    spec.colors[3] = p[3];
    return spec;
}

std::array<hui::gfx::Color, 4> palette_for(const std::string &seed)
{
    const float h = hue_of(seed);
    return {
        hsv(h, 0.55f, 0.10f),
        hsv(h, 0.55f, 0.22f),
        hsv(h, 0.50f, 0.42f),
        hsv(h + 0.06f, 0.60f, 0.70f),
    };
}

hui::gfx::Color accent_for(const std::string &seed)
{
    return hsv(hue_of(seed) + 0.04f, 0.62f, 0.92f);
}

std::uint32_t upload_image_texture(hui::gfx::Renderer &renderer, const unsigned char *data,
                                   std::size_t size)
{
    int w = 0;
    int h = 0;
    std::vector<unsigned char> rgba;
    if (!decode_image(data, size, w, h, rgba) || w <= 0 || h <= 0)
        return 0;
    return renderer.batch().create_texture(w, h, rgba.data());
}

std::uint32_t load_icon_texture(hui::gfx::Renderer &renderer, const std::string &dir)
{
    const std::string path = dir + "/sce_sys/icon0.png";
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (file == nullptr)
        return 0;
    std::vector<unsigned char> bytes;
    unsigned char buffer[65536];
    std::size_t got;
    while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
        bytes.insert(bytes.end(), buffer, buffer + got);
    std::fclose(file);
    if (bytes.empty())
        return 0;
    return upload_image_texture(renderer, bytes.data(), bytes.size());
}

} // namespace fwd
