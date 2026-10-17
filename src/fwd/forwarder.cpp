// ps5fwdgen - The forwarder model and its on-disk form.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fwd/forwarder.hpp"

#include "psfwd.h"

#include <cstdlib>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fwd
{

std::string target_display_name(const std::string &title_id)
{
    for (const Target &target : known_targets())
        if (title_id == target.title_id && target.title_id[0] != '\0')
            return target.name;
    return title_id.empty() ? std::string("(no target)") : title_id;
}

bool is_stream_target(const std::string &title_id)
{
    for (const Target &target : known_targets())
        if (target.stream && title_id == target.title_id)
            return true;
    return false;
}

bool takes_game_path(const std::string &title_id)
{
    for (const Target &target : known_targets())
        if (target.game_path && title_id == target.title_id)
            return true;
    return false;
}

GameLibrary library_of(const std::string &title_id)
{
    for (const Target &target : known_targets())
        if (title_id == target.title_id && target.title_id[0] != '\0')
            return target.library;
    return GameLibrary::none;
}

std::vector<Target> known_targets()
{
    // The target apps PS5 Forwarder Builder supports (from ps5-forwarder.mph.am):
    static const Target kAll[] = {
        {"PPSA99008", "ProsperoEden", true, true}, // Nintendo Switch (Eden)
        {"PPSA99764", "Porpoise", true, false},    // GameCube / Wii (RetroArch)
        {"PPSA99203", "PS5SX2", true, false},      // PlayStation 2 (PCSX2)
        {"PPSA50011", "PS5X360", true, false, false, Feature::none, true,
         GameLibrary::x360}, // Xbox 360
        {"PPSA98510", "SwanStationPS5", true, true, false, Feature::none, false,
         GameLibrary::psx}, // PlayStation (SwanStationPS5)
        {"PPSA00000", "Payload", true, false, false, Feature::payload_target}, // ELF payloads
        {"PPSA99002", "ProsperoLight", false, true, true}, // Moonlight/Sunshine streaming
        {"", "Generic (enter a title ID)", false, false},
    };
    std::vector<Target> targets;
    for (const Target &target : kAll)
        if (enabled(target.feature))
            targets.push_back(target);
    return targets;
}

std::vector<Argument> Forwarder::resolved_args() const
{
    std::vector<Argument> args;
    const bool stream = is_stream_target(target);
    if (stream)
    {
        if (!pc.empty())
            args.push_back({"--pc", pc});
        if (!app.empty())
            args.push_back({"--app", app});
    }
    else if (!rom.empty())
        args.push_back(takes_game_path(target) ? Argument{rom, ""} : Argument{"--rom", rom});
    for (const Argument &argument : extra_args)
        args.push_back(argument);
    if (exit_after_game)
        args.push_back({stream ? "--exit-after-stream" : "--exit-after-game", ""});
    return args;
}

std::string new_title_id(const std::string &forwarders_root)
{
    char id[PSFWD_TITLE_ID_SIZE];
    if (!psfwd_new_title_id(forwarders_root.c_str(), id))
        return "PPSA99200";
    return id;
}

bool valid_title_id(const std::string &id)
{
    return psfwd_valid_title_id(id.c_str()) != 0;
}

} // namespace fwd
