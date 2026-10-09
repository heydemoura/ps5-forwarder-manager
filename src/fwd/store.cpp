// ps5fwdgen - Reading, writing and listing forwarders under /data/homebrew.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The on-disk format is the PS5 Forwarder Format (external/ps5-forwarder-
// format, SPEC.md); the files are read and written by its library, psfwd.
// This file maps the app's model (a ROM, an exit flag and extra arguments) to
// and from the format's plain argument list.

#include "fwd/store.hpp"

#include "psfwd.h"

#include <algorithm>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace fwd
{

namespace
{

// What the app records as the forwarder's maker.
constexpr const char *kCreator = "Forwarder Manager";

bool is_dir(const std::string &path)
{
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// Turns a forwarder's argument list back into rom / exit_after_game / extra.
void split_args(const psfwd_info &info, Forwarder &out)
{
    int i = 0;
    while (i < info.argc)
    {
        const std::string key = info.argv[i];
        if (key == "--rom" && i + 1 < info.argc)
        {
            out.rom = info.argv[i + 1];
            i += 2;
            continue;
        }
        if (key == "--exit-after-game")
        {
            out.exit_after_game = true;
            i += 1;
            continue;
        }
        // Anything else is kept as an extra argument, pairing a following
        // value that does not itself look like a flag.
        Argument extra;
        extra.key = key;
        if (i + 1 < info.argc)
        {
            const std::string next = info.argv[i + 1];
            if (!next.empty() && next.rfind("--", 0) != 0)
            {
                extra.value = next;
                out.extra_args.push_back(std::move(extra));
                i += 2;
                continue;
            }
        }
        out.extra_args.push_back(std::move(extra));
        i += 1;
    }
}

psfwd_blob blob(const std::vector<unsigned char> &bytes)
{
    return {bytes.empty() ? nullptr : bytes.data(), bytes.size()};
}

} // namespace

bool load_forwarder(const std::string &dir, Forwarder &out)
{
    psfwd_info info;
    if (!psfwd_read(dir.c_str(), &info))
        return false;
    out = Forwarder{};
    out.title_id = info.title_id;
    out.display_name = info.name;
    out.target = info.target;
    split_args(info, out);
    out.has_icon = info.has_icon != 0;
    out.has_backgrounds = info.has_backgrounds != 0;
    out.has_music = info.has_music != 0;
    return true;
}

std::vector<Forwarder> scan_forwarders(const std::string &root)
{
    std::vector<Forwarder> forwarders;
    DIR *dir = ::opendir(root.c_str());
    if (dir == nullptr)
        return forwarders;
    struct dirent *entry;
    while ((entry = ::readdir(dir)) != nullptr)
    {
        const std::string name = entry->d_name;
        if (name.empty() || name[0] == '.')
            continue;
        const std::string path = root + "/" + name;
        if (!is_dir(path) || !psfwd_is_forwarder(path.c_str()))
            continue;
        Forwarder forwarder;
        if (load_forwarder(path, forwarder))
            forwarders.push_back(std::move(forwarder));
    }
    ::closedir(dir);
    std::sort(forwarders.begin(), forwarders.end(),
              [](const Forwarder &a, const Forwarder &b)
              {
                  if (a.display_name != b.display_name)
                      return a.display_name < b.display_name;
                  return a.title_id < b.title_id;
              });
    return forwarders;
}

int upgrade_forwarders(const std::string &root, const std::string &template_root)
{
    return psfwd_upgrade(root.c_str(), template_root.c_str());
}

WriteResult write_forwarder(const std::string &root, const std::string &template_root,
                            const Forwarder &forwarder, const Assets &assets)
{
    // The format stores a plain argument list.
    std::vector<std::string> args;
    for (const Argument &argument : forwarder.resolved_args())
    {
        args.push_back(argument.key);
        if (!argument.value.empty())
            args.push_back(argument.value);
    }
    std::vector<const char *> argv;
    for (const std::string &arg : args)
        argv.push_back(arg.c_str());

    psfwd_spec spec{};
    spec.title_id = forwarder.title_id.c_str();
    spec.name = forwarder.display_name.c_str();
    spec.target = forwarder.target.c_str();
    spec.argc = static_cast<int>(argv.size());
    spec.argv = argv.empty() ? nullptr : argv.data();
    spec.creator = kCreator;
    spec.icon0_png = blob(assets.icon_png);
    spec.pic0_dds = blob(assets.pic0_dds);
    spec.pic1_dds = blob(assets.pic1_dds);
    spec.snd0_at9 = blob(assets.music_at9);

    char error[256] = "";
    if (!psfwd_write(root.c_str(), template_root.c_str(), &spec, error, sizeof(error)))
        return {false, error};
    return {true, {}};
}

bool remove_forwarder(const std::string &root, const std::string &title_id)
{
    return psfwd_remove(root.c_str(), title_id.c_str()) != 0;
}

} // namespace fwd
