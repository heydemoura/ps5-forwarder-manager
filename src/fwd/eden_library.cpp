// ps5fwdgen - ProsperoEden's game files, cached names and covers.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fwd/eden_library.hpp"

#include "core/json.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>

namespace fwd
{
namespace
{

std::string lower(std::string text)
{
    for (char &c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string join(const std::string &root, const std::string &name)
{
    return root + (root == "/" ? "" : "/") + name;
}

std::string read_text(const std::string &path, std::size_t limit)
{
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (file == nullptr)
        return {};
    std::string text(limit + 1, '\0');
    const std::size_t got = std::fread(text.data(), 1, text.size(), file);
    const bool ok = std::ferror(file) == 0 && got <= limit;
    std::fclose(file);
    text.resize(ok ? got : 0);
    return text;
}

bool valid_root(std::string_view path)
{
    if (path.empty() || path.size() > 240 || path.front() != '/')
        return false;
    for (unsigned char c : path)
        if (c < 32 || c == 127 || c == '\\')
            return false;
    if (path == "/")
        return true;
    for (std::size_t start = 1; start <= path.size();)
    {
        const std::size_t end = path.find('/', start);
        const std::string_view part = path.substr(start, end - start);
        if (part.empty() || part == "." || part == "..")
            return false;
        if (end == std::string_view::npos)
            break;
        start = end + 1;
    }
    return true;
}

std::string rom_kind(const std::string &name)
{
    const std::size_t dot = name.find_last_of('.');
    const std::string ext = dot == std::string::npos ? "" : lower(name.substr(dot + 1));
    if (ext != "nsp" && ext != "xci" && ext != "nsz" && ext != "xcz")
        return {};
    std::string kind = ext;
    for (char &c : kind)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return kind;
}

bool addon_name(const std::string &name)
{
    const std::string text = lower(name);
    for (const char *tag : {"[upd]", "[update]", "[dlc]", "(update)", "(dlc)"})
        if (text.find(tag) != std::string::npos)
            return true;
    // Dump tags commonly carry the 16-digit Switch title ID. Base games end
    // in 000, updates in 800 and DLC in other nonzero low twelve bits.
    for (std::size_t at = text.find('['); at != std::string::npos; at = text.find('[', at + 1))
        if (at + 17 < text.size() && text[at + 17] == ']' && text.compare(at + 1, 2, "01") == 0)
        {
            const std::string id = text.substr(at + 1, 16);
            if (std::all_of(id.begin(), id.end(),
                            [](unsigned char c) { return std::isxdigit(c); }) &&
                id.substr(13) != "000")
                return true;
        }
    const std::string title = lower(eden_game_title(name));
    return title.ends_with(" update") || title.ends_with(" dlc");
}

LibraryGame make_game(const std::string &path, const std::string &roms,
                      const std::string &data_root)
{
    LibraryGame game;
    const std::string prefix = roms + "/";
    game.path = path.starts_with(prefix) ? path.substr(prefix.size()) : path;
    game.location = path;
    game.name = eden_game_title(path);
    const std::size_t slash = path.find_last_of('/');
    game.kind = rom_kind(path.substr(slash == std::string::npos ? 0 : slash + 1));
    if (!game.path.empty() && game.path.front() != '/')
    {
        const std::string cache = join(data_root, "covers/") + eden_cover_key(game.path);
        std::string name = read_text(cache + ".name", 512);
        const std::size_t first = name.find_first_not_of(" \t\r\n");
        const std::size_t last = name.find_last_not_of(" \t\r\n");
        if (first != std::string::npos && name.find('\0') == std::string::npos)
            game.name = name.substr(first, last - first + 1);
        struct stat info
        {
        };
        if (::stat((cache + ".tga").c_str(), &info) == 0 && S_ISREG(info.st_mode))
            game.cover = cache + ".tga";
    }
    return game;
}

void scan(const std::string &dir, const std::string &roms, const std::string &data_root,
          bool folders, std::vector<LibraryGame> &games)
{
    DIR *directory = ::opendir(dir.c_str());
    if (directory == nullptr)
        return;
    while (const dirent *entry = ::readdir(directory))
    {
        const std::string name = entry->d_name;
        if (name.empty() || name.front() == '.')
            continue;
        const std::string path = join(dir, name);
        struct stat info
        {
        };
        // Do not follow directory links into unrelated trees or cycles.
        if (::lstat(path.c_str(), &info) != 0)
            continue;
        if (S_ISDIR(info.st_mode) && folders)
            scan(path, roms, data_root, false, games);
        else if (S_ISREG(info.st_mode) && !rom_kind(name).empty() && !addon_name(name))
            games.push_back(make_game(path, roms, data_root));
    }
    ::closedir(directory);
}

} // namespace

std::string eden_game_files(const std::string &data_root)
{
    json::Value config;
    if (!json::parse(read_text(join(data_root, "config/prosperoeden.json"), 1u << 20), config))
        return data_root;
    const std::string value(config.get("game_files").as_string());
    return valid_root(value) ? value : data_root;
}

std::string eden_cover_key(std::string_view relative_path)
{
    std::uint32_t hash = 2166136261u;
    for (unsigned char c : relative_path)
        hash = (hash ^ c) * 16777619u;
    char text[9];
    std::snprintf(text, sizeof(text), "%08x", static_cast<unsigned>(hash));
    return text;
}

std::vector<LibraryGame> read_eden_library(const std::string &data_root)
{
    std::vector<LibraryGame> games;
    const std::string roms = join(eden_game_files(data_root), "roms");
    scan(roms, roms, data_root, true, games);
    std::sort(games.begin(), games.end(),
              [](const LibraryGame &a, const LibraryGame &b)
              {
                  const std::string an = lower(a.name), bn = lower(b.name);
                  return an == bn ? a.path < b.path : an < bn;
              });
    return games;
}

LibraryGame eden_game_from_file(const std::string &path, const std::string &data_root)
{
    return make_game(path, join(eden_game_files(data_root), "roms"), data_root);
}

std::string eden_game_title(std::string_view path)
{
    const std::size_t slash = path.find_last_of('/');
    if (slash != std::string_view::npos)
        path.remove_prefix(slash + 1);
    const std::size_t dot = path.find_last_of('.');
    if (dot != std::string_view::npos && dot > 0)
        path = path.substr(0, dot);
    if (path.starts_with("[Game] "))
        path.remove_prefix(7);
    const std::string_view original = path;
    const std::size_t tag = path.find_first_of("[(");
    if (tag != std::string_view::npos && tag > 0)
        path = path.substr(0, tag);
    while (!path.empty() && (path.back() == ' ' || path.back() == '_' || path.back() == '-'))
        path.remove_suffix(1);
    return std::string(path.empty() ? original : path);
}

} // namespace fwd
