// ps5fwdgen - The forwarder model and its on-disk form.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fwd/forwarder.hpp"

#include "core/json.hpp"

#include <cstdlib>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fwd
{

namespace
{

// The emulator ids the site reserves (never handed out as a new forwarder id).
constexpr const char *kReserved[] = {"PPSA99008", "PPSA99764", "PPSA99203", "PPSA99100"};

bool is_reserved(const std::string &id)
{
    for (const char *reserved : kReserved)
    {
        if (id == reserved)
            return true;
    }
    return false;
}

bool directory_exists(const std::string &path)
{
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

} // namespace

std::string target_display_name(const std::string &title_id)
{
    for (const Target &target : known_targets())
        if (title_id == target.title_id && target.title_id[0] != '\0')
            return target.name;
    return title_id.empty() ? std::string("(no target)") : title_id;
}

std::vector<Target> known_targets()
{
    return {
        {"PPSA99008", "ProsperoEden", true, true},
        {"PPSA99764", "Porpoise", true, false},
        {"PPSA99203", "PS5SX2", true, false},
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

std::string Forwarder::forwarder_json() const
{
    json::Value root = json::Value::object();
    root.set("target", json::Value(target));
    json::Value array = json::Value::array();
    for (const Argument &argument : resolved_args())
    {
        array.push(json::Value(argument.key));
        if (!argument.value.empty())
            array.push(json::Value(argument.value));
    }
    root.set("args", std::move(array));
    return root.dump(2) + "\n";
}

std::string Forwarder::param_json() const
{
    // The five-digit concept code is the title ID without its PPSA prefix.
    const std::string concept_id = title_id.size() == 9 ? title_id.substr(4) : title_id;

    json::Value root = json::Value::object();
    json::Value age = json::Value::object();
    age.set("default", json::Value(0));
    root.set("ageLevel", std::move(age));
    root.set("applicationCategoryType", json::Value(0));
    root.set("applicationDrmType", json::Value("free"));
    root.set("attribute", json::Value(0));
    root.set("attribute2", json::Value(0));
    root.set("attribute3", json::Value(0));
    root.set("conceptId", json::Value(concept_id));
    root.set("contentBadgeType", json::Value(1));
    root.set("contentId", json::Value("UP9000-" + title_id + "_00-PS5FORWARDER0000"));
    root.set("contentVersion", json::Value("01.000.000"));
    root.set("downloadDataSize", json::Value(256));

    json::Value intents = json::Value::array();
    json::Value intent = json::Value::object();
    intent.set("intentType", json::Value("launchActivity"));
    intents.push(std::move(intent));
    json::Value game_intent = json::Value::object();
    game_intent.set("permittedIntents", std::move(intents));
    root.set("gameIntent", std::move(game_intent));

    json::Value localized = json::Value::object();
    localized.set("defaultLanguage", json::Value("en-US"));
    json::Value en = json::Value::object();
    const std::string name = display_name.empty() ? std::string("PS5 App") : display_name;
    en.set("titleName", json::Value(name));
    localized.set("en-US", std::move(en));
    root.set("localizedParameters", std::move(localized));

    root.set("masterVersion", json::Value("01.00"));
    json::Value pubtools = json::Value::object();
    pubtools.set("creationDate", json::Value("2026-08-22 00:00:00"));
    pubtools.set("loudnessSnd0", json::Value("-28.00"));
    pubtools.set("toolVersion", json::Value("2.00"));
    root.set("pubtools", std::move(pubtools));

    root.set("requiredSystemSoftwareVersion", json::Value("0x0000000000000000"));
    root.set("sdkVersion", json::Value("0x0000000000000000"));
    root.set("titleId", json::Value(title_id));
    root.set("versionFileUri", json::Value(""));
    return root.dump(2) + "\n";
}

std::string new_title_id(const std::string &forwarders_root)
{
    unsigned seed = static_cast<unsigned>(::time(nullptr)) ^ static_cast<unsigned>(::getpid());
    for (int attempt = 0; attempt < 4096; ++attempt)
    {
        seed = seed * 1103515245u + 12345u;
        const int number = 99200 + static_cast<int>((seed >> 8) % 700u);
        char id[16];
        (void)std::snprintf(id, sizeof(id), "PPSA%05d", number);
        const std::string candidate(id);
        if (is_reserved(candidate))
            continue;
        if (!forwarders_root.empty() && directory_exists(forwarders_root + "/" + candidate))
            continue;
        return candidate;
    }
    return "PPSA99200";
}

bool valid_title_id(const std::string &id)
{
    if (id.size() != 9)
        return false;
    const std::string prefix = id.substr(0, 4);
    if (prefix != "PPSA" && prefix != "CUSA" && prefix != "LAPY" && prefix != "FAKE")
        return false;
    for (std::size_t i = 4; i < 9; ++i)
    {
        if (id[i] < '0' || id[i] > '9')
            return false;
    }
    return true;
}

} // namespace fwd
