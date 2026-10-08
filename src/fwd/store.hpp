// ps5fwdgen - Reading, writing and listing forwarders under /data/homebrew.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "fwd/forwarder.hpp"

#include <string>
#include <vector>

namespace fwd
{

// The art to write with a forwarder. Empty vectors mean "leave what is on
// disk" when editing, or "none" when creating.
struct Assets
{
    std::vector<unsigned char> icon_png;  // 512x512 PNG
    std::vector<unsigned char> pic0_dds;  // 3840x2160 BC7 DDS (selected bg)
    std::vector<unsigned char> pic1_dds;  // 3840x2160 BC7 DDS (launch bg)
    std::vector<unsigned char> music_at9; // snd0.at9
};

struct WriteResult
{
    bool ok = false;
    std::string error;
};

// Lists every forwarder directory under root (a subfolder with forwarder.json
// next to eboot.bin). Missing or unreadable root yields an empty list.
std::vector<Forwarder> scan_forwarders(const std::string &root);

// Reads one forwarder folder. ok is false when forwarder.json is missing.
bool load_forwarder(const std::string &dir, Forwarder &out);

// Writes forwarder.title_id under root: the shared eboot.bin and libc.prx
// copied from template_root, forwarder.json, sce_sys/param.json, and whatever
// art is supplied. Icon is required when the folder does not already have one.
// Writes to a temporary sibling and renames into place so a scan never sees a
// half-written tile.
WriteResult write_forwarder(const std::string &root, const std::string &template_root,
                            const Forwarder &forwarder, const Assets &assets);

// Removes a forwarder folder and everything in it.
bool remove_forwarder(const std::string &root, const std::string &title_id);

} // namespace fwd
