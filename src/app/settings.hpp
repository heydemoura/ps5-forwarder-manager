// ps5fwdgen - User settings: a small key=value file in the title's storage.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

namespace fwd
{

struct Settings
{
    // Where forwarders are written and listed. ShadowMountPlus scans this.
    std::string forwarders_root = "/data/homebrew";
    // SteamGridDB personal API key (steamgriddb.com/profile/preferences/api).
    std::string steamgriddb_key;
    // Index into the kit's theme list.
    int theme = 0;
    // Circle confirms (Japanese layout).
    bool swap_confirm = false;
    // Play interface sounds.
    bool sounds = true;
    // Fewer animations.
    bool reduced_motion = false;
    // Ask the forwarder site to convert non-AT9 audio (uploads the file).
    bool convert_audio_online = true;

    static Settings load(const std::string &path);
    bool save(const std::string &path) const;
};

} // namespace fwd
