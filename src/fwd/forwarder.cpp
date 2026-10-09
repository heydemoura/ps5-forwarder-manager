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

std::vector<Target> known_targets()
{
    // The target apps PS5 Forwarder Builder supports (from ps5-forwarder.mph.am):
    return {
        {"PPSA99008", "ProsperoEden", true, true},  // Nintendo Switch (Eden)
        {"PPSA99764", "Porpoise", true, false},     // GameCube / Wii (RetroArch)
        {"PPSA99203", "PS5SX2", true, false},       // PlayStation 2 (PCSX2)
        {"PPSA50011", "PS5X360", true, false},      // Xbox 360 (Xenia)
        {"PPSA00000", "Payload", true, false},      // ELF payload launcher
        {"", "Generic (enter a title ID)", false, false},
    };
}

std::vector<Argument> Forwarder::resolved_args() const
{
    std::vector<Argument> args;
    if (!rom.empty())
        args.push_back({"--rom", rom});
    for (const Argument &argument : extra_args)
        args.push_back(argument);
    if (exit_after_game)
        args.push_back({"--exit-after-game", ""});
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
