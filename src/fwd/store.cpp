// ps5fwdgen - Reading, writing and listing forwarders under /data/homebrew.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fwd/store.hpp"

#include "core/json.hpp"
#include "core/save_file.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fwd
{

namespace
{

bool exists(const std::string &path)
{
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

bool is_dir(const std::string &path)
{
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool read_binary(const std::string &path, std::vector<unsigned char> &out)
{
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
        return false;
    out.clear();
    unsigned char buffer[65536];
    for (;;)
    {
        const ssize_t got = ::read(fd, buffer, sizeof(buffer));
        if (got < 0)
        {
            ::close(fd);
            return false;
        }
        if (got == 0)
            break;
        out.insert(out.end(), buffer, buffer + got);
    }
    ::close(fd);
    return true;
}

bool write_binary(const std::string &path, const unsigned char *data, std::size_t size)
{
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0777);
    if (fd < 0)
        return false;
    std::size_t written = 0;
    while (written < size)
    {
        const ssize_t n = ::write(fd, data + written, size - written);
        if (n <= 0)
        {
            ::close(fd);
            return false;
        }
        written += static_cast<std::size_t>(n);
    }
    return ::close(fd) == 0;
}

bool copy_file(const std::string &from, const std::string &to)
{
    std::vector<unsigned char> data;
    if (!read_binary(from, data))
        return false;
    return write_binary(to, data.data(), data.size());
}

// Recursively remove a directory tree.
bool remove_tree(const std::string &path)
{
    DIR *dir = ::opendir(path.c_str());
    if (dir != nullptr)
    {
        struct dirent *entry;
        while ((entry = ::readdir(dir)) != nullptr)
        {
            const std::string name = entry->d_name;
            if (name == "." || name == "..")
                continue;
            const std::string child = path + "/" + name;
            if (is_dir(child))
                remove_tree(child);
            else
                ::unlink(child.c_str());
        }
        ::closedir(dir);
    }
    return ::rmdir(path.c_str()) == 0 || !exists(path);
}

// Turns a forwarder.json "args" list back into rom / exit_after_game / extra.
void split_args(const json::Value &args, Forwarder &out)
{
    const std::size_t count = args.size();
    std::size_t i = 0;
    while (i < count)
    {
        const std::string key(args.at(i).as_string());
        if (key == "--rom" && i + 1 < count)
        {
            out.rom = std::string(args.at(i + 1).as_string());
            i += 2;
            continue;
        }
        if (key == "--exit-after-game")
        {
            out.exit_after_game = true;
            i += 1;
            continue;
        }
        // Anything else: keep as an extra argument, pairing a following value
        // that does not itself look like a flag.
        Argument extra;
        extra.key = key;
        if (i + 1 < count)
        {
            const std::string next(args.at(i + 1).as_string());
            if (!next.empty() && next.rfind("--", 0) != 0)
            {
                extra.value = next;
                i += 2;
                out.extra_args.push_back(std::move(extra));
                continue;
            }
        }
        i += 1;
        out.extra_args.push_back(std::move(extra));
    }
}

} // namespace

bool load_forwarder(const std::string &dir, Forwarder &out)
{
    std::string text;
    if (!hui::save::read_file(dir + "/forwarder.json", &text, 64u << 10))
        return false;
    json::Value config;
    if (!json::parse(text, config, nullptr) || !config.is_object())
        return false;

    out = Forwarder{};
    // The folder name is the title ID.
    const std::size_t slash = dir.find_last_of('/');
    out.title_id = slash == std::string::npos ? dir : dir.substr(slash + 1);
    out.target = std::string(config.get("target").as_string());

    // New format: an "args" list. Old format: "rom" / "exit_after_game" keys.
    if (config.get("args").is_array())
        split_args(config.get("args"), out);
    if (config.has("rom"))
        out.rom = std::string(config.get("rom").as_string());
    if (config.has("exit_after_game"))
        out.exit_after_game = config.get("exit_after_game").as_bool();

    // Display name from param.json.
    std::string param;
    if (hui::save::read_file(dir + "/sce_sys/param.json", &param, 64u << 10))
    {
        json::Value meta;
        if (json::parse(param, meta, nullptr))
        {
            const json::Value &localized = meta.get("localizedParameters");
            const std::string def(localized.get("defaultLanguage").as_string("en-US"));
            out.display_name = std::string(localized.get(def).get("titleName").as_string());
            if (out.display_name.empty())
                out.display_name = std::string(localized.get("en-US").get("titleName").as_string());
        }
    }

    out.has_icon = exists(dir + "/sce_sys/icon0.png");
    out.has_backgrounds = exists(dir + "/sce_sys/pic0.dds");
    out.has_music = exists(dir + "/sce_sys/snd0.at9");
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
        if (!is_dir(path))
            continue;
        if (!exists(path + "/forwarder.json") || !exists(path + "/eboot.bin"))
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

WriteResult write_forwarder(const std::string &root, const std::string &template_root,
                            const Forwarder &forwarder, const Assets &assets)
{
    if (!valid_title_id(forwarder.title_id))
        return {false, "title ID must be PPSA and five digits"};
    if (forwarder.target.empty())
        return {false, "a target app is required"};

    const std::string final_dir = root + "/" + forwarder.title_id;
    const std::string existing_sys = final_dir + "/sce_sys";
    const bool editing = exists(final_dir + "/forwarder.json");
    if (!editing && assets.icon_png.empty() && !exists(existing_sys + "/icon0.png"))
        return {false, "a tile icon is required"};

    // Stage in a temporary sibling so a scan never catches a partial tile.
    const std::string stage = root + "/.staging-" + forwarder.title_id;
    remove_tree(stage);
    if (!hui::save::ensure_directory(root))
        return {false, "cannot create " + root};
    if (::mkdir(stage.c_str(), 0777) != 0 && errno != EEXIST)
        return {false, std::string("cannot create staging dir: ") + std::strerror(errno)};
    const std::string stage_sys = stage + "/sce_sys";
    const std::string stage_module = stage + "/sce_module";
    ::mkdir(stage_sys.c_str(), 0777);
    ::mkdir(stage_module.c_str(), 0777);

    const auto cleanup_fail = [&](const std::string &message) -> WriteResult
    {
        remove_tree(stage);
        return {false, message};
    };

    // The shared launcher binaries.
    if (!copy_file(template_root + "/eboot.bin", stage + "/eboot.bin"))
        return cleanup_fail("missing template eboot.bin");
    if (!copy_file(template_root + "/sce_module/libc.prx", stage_module + "/libc.prx"))
        return cleanup_fail("missing template sce_module/libc.prx");

    // The two JSON documents.
    const std::string forwarder_json = forwarder.forwarder_json();
    if (!write_binary(stage + "/forwarder.json",
                      reinterpret_cast<const unsigned char *>(forwarder_json.data()),
                      forwarder_json.size()))
        return cleanup_fail("cannot write forwarder.json");
    const std::string param_json = forwarder.param_json();
    if (!write_binary(stage_sys + "/param.json",
                      reinterpret_cast<const unsigned char *>(param_json.data()),
                      param_json.size()))
        return cleanup_fail("cannot write param.json");

    // Art: use what was given, else carry over what the old folder had.
    const auto place = [&](const std::vector<unsigned char> &bytes, const char *name) -> bool
    {
        const std::string target = stage_sys + "/" + name;
        if (!bytes.empty())
            return write_binary(target, bytes.data(), bytes.size());
        const std::string prior = existing_sys + "/" + name;
        if (editing && exists(prior))
            return copy_file(prior, target);
        return true; // optional and absent
    };
    if (!place(assets.icon_png, "icon0.png"))
        return cleanup_fail("cannot write icon0.png");
    if (!place(assets.pic0_dds, "pic0.dds"))
        return cleanup_fail("cannot write pic0.dds");
    if (!place(assets.pic1_dds, "pic1.dds"))
        return cleanup_fail("cannot write pic1.dds");
    if (!place(assets.music_at9, "snd0.at9"))
        return cleanup_fail("cannot write snd0.at9");

    // Promote: remove the old folder, then rename the staging dir into place.
    remove_tree(final_dir);
    if (::rename(stage.c_str(), final_dir.c_str()) != 0)
    {
        // A cross-device rename can fail; fall back to a recursive copy.
        return cleanup_fail(std::string("cannot publish forwarder: ") + std::strerror(errno));
    }
    return {true, {}};
}

bool remove_forwarder(const std::string &root, const std::string &title_id)
{
    if (!valid_title_id(title_id))
        return false;
    return remove_tree(root + "/" + title_id);
}

} // namespace fwd
