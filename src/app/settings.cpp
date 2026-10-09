// ps5fwdgen - User settings: a small key=value file in the title's storage.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/settings.hpp"

#include "core/save_file.hpp"

#include <cstdlib>
#include <string>
#include <string_view>

namespace fwd
{

namespace
{

// A key baked in at build time from the STEAMGRIDDB_API_KEY environment value
// (a GitHub Actions secret in CI, or .env locally). Empty when the build had
// no key. A key the user enters in Settings always overrides it.
#if defined(STEAMGRIDDB_API_KEY)
#define FWD_STRINGIFY2(x) #x
#define FWD_STRINGIFY(x) FWD_STRINGIFY2(x)
constexpr const char *kBuiltinSteamGridDbKey = FWD_STRINGIFY(STEAMGRIDDB_API_KEY);
#else
constexpr const char *kBuiltinSteamGridDbKey = "";
#endif

std::string_view trim(std::string_view value)
{
    while (!value.empty() &&
           (value.front() == ' ' || value.front() == '\t' || value.front() == '\r'))
        value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r'))
        value.remove_suffix(1);
    return value;
}

bool to_bool(std::string_view value)
{
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

} // namespace

Settings Settings::load(const std::string &path)
{
    Settings settings;
    settings.steamgriddb_key = kBuiltinSteamGridDbKey; // env/secret default
    std::string text;
    if (!hui::save::read_file(path, &text, 64u << 10))
        return settings;
    std::string_view rest(text);
    while (!rest.empty())
    {
        const std::size_t newline = rest.find('\n');
        std::string_view line = rest.substr(0, newline);
        rest = newline == std::string_view::npos ? std::string_view{} : rest.substr(newline + 1);
        line = trim(line);
        if (line.empty() || line.front() == '#')
            continue;
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos)
            continue;
        const std::string_view key = trim(line.substr(0, equals));
        const std::string_view value = trim(line.substr(equals + 1));
        if (key == "forwarders_root" && !value.empty())
            settings.forwarders_root = std::string(value);
        else if (key == "steamgriddb_key")
            settings.steamgriddb_key = std::string(value);
        else if (key == "theme")
            settings.theme = static_cast<int>(std::strtol(std::string(value).c_str(), nullptr, 10));
        else if (key == "swap_confirm")
            settings.swap_confirm = to_bool(value);
        else if (key == "sounds")
            settings.sounds = to_bool(value);
        else if (key == "reduced_motion")
            settings.reduced_motion = to_bool(value);
        else if (key == "convert_audio_online")
            settings.convert_audio_online = to_bool(value);
    }
    if (settings.steamgriddb_key.empty())
        settings.steamgriddb_key = kBuiltinSteamGridDbKey;
    return settings;
}

bool Settings::save(const std::string &path) const
{
    std::string text = "# ps5fwdgen settings\n";
    text += "forwarders_root=" + forwarders_root + "\n";
    text += "steamgriddb_key=" + steamgriddb_key + "\n";
    text += "theme=" + std::to_string(theme) + "\n";
    text += std::string("swap_confirm=") + (swap_confirm ? "1" : "0") + "\n";
    text += std::string("sounds=") + (sounds ? "1" : "0") + "\n";
    text += std::string("reduced_motion=") + (reduced_motion ? "1" : "0") + "\n";
    text += std::string("convert_audio_online=") + (convert_audio_online ? "1" : "0") + "\n";
    return hui::save::write_atomic(path, text).empty();
}

} // namespace fwd
