// ps5fwdgen - A development-only self-test of the forwarder write path.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/dev_selftest.hpp"

#include "fwd/forwarder.hpp"
#include "fwd/image.hpp"
#include "fwd/store.hpp"
#include "platform/ps5/system.hpp"

#include <cstdio>
#include <string>
#include <unistd.h>
#include <vector>

namespace fwd
{

namespace
{

constexpr char kTrigger[] = "/data/ps5fwdgen-dev/selftest.txt";

// A plain diagonal gradient, so the generated icon is recognisable on the
// home screen without shipping a test image.
std::vector<unsigned char> gradient_rgba(int size)
{
    std::vector<unsigned char> rgba(static_cast<std::size_t>(size) * size * 4);
    for (int y = 0; y < size; ++y)
    {
        for (int x = 0; x < size; ++x)
        {
            const std::size_t i = (static_cast<std::size_t>(y) * size + x) * 4;
            rgba[i + 0] = static_cast<unsigned char>(x * 255 / size);
            rgba[i + 1] = static_cast<unsigned char>(y * 255 / size);
            rgba[i + 2] = 160;
            rgba[i + 3] = 255;
        }
    }
    return rgba;
}

bool read_line(const std::string &path, std::string &out)
{
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (file == nullptr)
        return false;
    char buffer[256] = {};
    const std::size_t got = std::fread(buffer, 1, sizeof(buffer) - 1, file);
    std::fclose(file);
    buffer[got] = '\0';
    out = buffer;
    const std::size_t newline = out.find_first_of("\r\n");
    if (newline != std::string::npos)
        out.resize(newline);
    return true;
}

} // namespace

void run_dev_selftest(const std::string &forwarders_root, const std::string &template_root)
{
    std::string line;
    if (!read_line(kTrigger, line))
        return;

    // Optional "name|target|rom".
    std::string name = "Self Test";
    std::string target = "PPSA99008";
    std::string rom = "Self Test ROM.nsp";
    const std::size_t bar1 = line.find('|');
    if (bar1 != std::string::npos)
    {
        name = line.substr(0, bar1);
        const std::size_t bar2 = line.find('|', bar1 + 1);
        if (bar2 != std::string::npos)
        {
            target = line.substr(bar1 + 1, bar2 - bar1 - 1);
            rom = line.substr(bar2 + 1);
        }
    }

    Forwarder forwarder;
    forwarder.title_id = new_title_id(forwarders_root);
    forwarder.display_name = name.empty() ? "Self Test" : name;
    forwarder.target = target.empty() ? "PPSA99008" : target;
    forwarder.rom = rom;
    forwarder.exit_after_game = true;

    Assets assets;
    const std::vector<unsigned char> rgba = gradient_rgba(kIconSize);
    assets.icon_png = make_icon_png_rgba(rgba.data(), kIconSize, kIconSize);

    const WriteResult result = write_forwarder(forwarders_root, template_root, forwarder, assets);
    hui::sys::log("[FWD] selftest title=%s name=\"%s\" target=%s ok=%d err=%s",
                  forwarder.title_id.c_str(), forwarder.display_name.c_str(),
                  forwarder.target.c_str(), result.ok ? 1 : 0, result.error.c_str());

    (void)::unlink(kTrigger);
}

} // namespace fwd
