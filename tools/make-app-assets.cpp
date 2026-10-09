// ps5fwdgen - Host tool: build the app's own sce_sys presentation assets.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Converts source artwork into the files the PS5 shell reads for this title:
//   icon0.png  - 512x512 launcher tile icon (from the icon artwork)
//   pic0.dds   - BC7 (DX10) selection background, 3840x2160
//   pic1.dds   - BC7 (DX10) launch/startup background, 3840x2160
//
// Reuses the in-tree encoders (src/fwd/image.cpp), so the output matches what
// the website and the app produce. DDS output size is explicit here (full 4K);
// the runtime forwarder path stays at 1080p for the 128 MB app heap.
//
// Build (host clang, see .env for CXX):
//   $CXX -std=c++20 -O2 -I src tools/make-app-assets.cpp src/fwd/image.cpp \
//        -o /tmp/make-app-assets
// Run:
//   make-app-assets --icon ICON.png --background BG.png --out sce_sys

#include "fwd/image.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{

constexpr int kAppBackgroundWidth = 3840;
constexpr int kAppBackgroundHeight = 2160;

std::vector<unsigned char> read_file(const std::string &path)
{
    std::vector<unsigned char> bytes;
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (f == nullptr)
        return bytes;
    unsigned char buffer[65536];
    std::size_t got;
    while ((got = std::fread(buffer, 1, sizeof(buffer), f)) > 0)
        bytes.insert(bytes.end(), buffer, buffer + got);
    std::fclose(f);
    return bytes;
}

bool write_file(const std::string &path, const std::vector<unsigned char> &bytes)
{
    std::FILE *f = std::fopen(path.c_str(), "wb");
    if (f == nullptr)
        return false;
    const bool ok = bytes.empty() ||
                    std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    std::fclose(f);
    return ok;
}

} // namespace

int main(int argc, char **argv)
{
    std::string icon_path;
    std::string background_path;
    std::string out_dir = "sce_sys";
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--icon" && i + 1 < argc)
            icon_path = argv[++i];
        else if (arg == "--background" && i + 1 < argc)
            background_path = argv[++i];
        else if (arg == "--out" && i + 1 < argc)
            out_dir = argv[++i];
        else
        {
            std::fprintf(stderr, "unknown or incomplete argument: %s\n", arg.c_str());
            return 2;
        }
    }
    if (icon_path.empty() || background_path.empty())
    {
        std::fprintf(stderr,
                     "usage: make-app-assets --icon ICON.png --background BG.png [--out DIR]\n");
        return 2;
    }

    int failures = 0;

    // icon0.png
    const std::vector<unsigned char> icon_src = read_file(icon_path);
    if (icon_src.empty())
    {
        std::fprintf(stderr, "could not read icon: %s\n", icon_path.c_str());
        ++failures;
    }
    else
    {
        const std::vector<unsigned char> icon = fwd::make_icon_png(icon_src.data(), icon_src.size());
        const std::string path = out_dir + "/icon0.png";
        if (icon.empty() || !write_file(path, icon))
        {
            std::fprintf(stderr, "icon0.png FAILED\n");
            ++failures;
        }
        else
            std::printf("icon0.png     %zu bytes (512x512)\n", icon.size());
    }

    // pic0.dds and pic1.dds (identical source; selection + launch background)
    const std::vector<unsigned char> bg_src = read_file(background_path);
    if (bg_src.empty())
    {
        std::fprintf(stderr, "could not read background: %s\n", background_path.c_str());
        ++failures;
    }
    else
    {
        const std::vector<unsigned char> dds = fwd::make_background_dds_sized(
            bg_src.data(), bg_src.size(), kAppBackgroundWidth, kAppBackgroundHeight);
        if (dds.empty())
        {
            std::fprintf(stderr, "background encode FAILED\n");
            ++failures;
        }
        else
        {
            for (const char *name : {"/pic0.dds", "/pic1.dds"})
            {
                const std::string path = out_dir + name;
                if (!write_file(path, dds))
                {
                    std::fprintf(stderr, "%s FAILED\n", name);
                    ++failures;
                }
                else
                    std::printf("%s      %zu bytes (%dx%d BC7)\n", name + 1, dds.size(),
                                kAppBackgroundWidth, kAppBackgroundHeight);
            }
        }
    }

    return failures == 0 ? 0 : 1;
}
