// ps5fwdgen - ProsperoEden config, library, metadata and forwarder regressions.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/json.hpp"
#include "fwd/eden_library.hpp"
#include "fwd/image.hpp"
#include "fwd/library_game.hpp"
#include "fwd/store.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace
{
int failures = 0;

void expect(bool ok, const char *message)
{
    if (!ok)
    {
        std::fprintf(stderr, "failed: %s\n", message);
        ++failures;
    }
}

void write(const std::string &path, const std::string &text = "fixture")
{
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::FILE *file = std::fopen(path.c_str(), "wb");
    if (file == nullptr)
        std::abort();
    (void)std::fwrite(text.data(), 1, text.size(), file);
    std::fclose(file);
}

const fwd::LibraryGame *find(const std::vector<fwd::LibraryGame> &games, const std::string &path)
{
    for (const auto &game : games)
        if (game.path == path)
            return &game;
    return nullptr;
}
} // namespace

int main()
{
    char pattern[] = "/tmp/eden-library-XXXXXX";
    const char *made = ::mkdtemp(pattern);
    if (made == nullptr)
        return 1;
    const std::string root = made;
    const std::string data = root + "/data";
    const std::string config = data + "/config/prosperoeden.json";
    // The configured folder itself contains roms/: only the configured
    // prefix is stripped, preserving the game's real relative path.
    const std::string files = root + "/roms/USB Eden";
    const std::string roms = files + "/roms";
    expect(std::string(fwd::kEdenDataRoot) == "/data/prosperoeden", "console default");
    expect(fwd::eden_game_files(data) == data, "missing config uses default");
    expect(fwd::read_eden_library(data).empty(), "missing ROM folder is empty");
    write(data + "/roms/Default.nsp");
    expect(fwd::read_eden_library(data).size() == 1, "default ROM folder scanned");
    for (const std::string &text :
         {"{broken", "[]", "{}", "{\"game_files\":false}", "{\"game_files\":\"\"}",
          "{\"game_files\":\"relative\"}", "{\"game_files\":\"/data/../other\"}",
          "{\"game_files\":\"/data//other\"}", "{\"game_files\":\"/data/other/\"}"})
    {
        write(config, text);
        expect(fwd::eden_game_files(data) == data, "invalid setting falls back");
    }
    write(config, "{\"game_files\":\"/\"}");
    expect(fwd::eden_game_files(data) == "/", "root game files folder is valid");
    write(config, "{\"game_files\":" + json::quote(root + "/unmounted") + "}");
    expect(fwd::read_eden_library(data).empty(),
           "unavailable configured drive does not use stale default");
    write(config, "{\"unrelated\":123,\"game_files\":" + json::quote(files) + "}");
    expect(fwd::eden_game_files(data) == files, "configured game files folder");

    write(roms + "/Loose [0100000000000000][v0].nsp");
    write(roms + "/Game A/game.nsp");
    write(roms + "/Game B/game.nsp");
    write(roms + "/Game A/Game Update.nsp");
    write(roms + "/Game A/Game DLC.nsp");
    write(roms + "/Game A/Game [UPD][v42].nsp");
    write(roms + "/Game A/Game [0100000000000800].nsp");
    write(roms + "/Game A/Game [0100000000001001].nsp");
    write(roms + "/Game A/mods/Other.nsp");
    write(roms + "/Game A/update.nca");
    write(roms + "/.hidden.nsp");
    write(roms + "/.hidden/game.nsp");
    write(roms + "/Upper.XCI");
    write(roms + "/Compressed.nsz");
    write(roms + "/Compressed Cart.XCZ");
    write(roms + "/not-a-rom.txt");
    std::filesystem::create_directory(roms + "/not-a-file.nsp");
    (void)::symlink(roms.c_str(), (roms + "/cycle").c_str());
    expect(fwd::eden_cover_key("hello") == "4f9f2cab", "known FNV-1a vector");
    expect(fwd::eden_cover_key("Game A/game.nsp") != fwd::eden_cover_key("Game B/game.nsp"),
           "same filenames have different path hashes");
    const std::string cache = data + "/covers/" + fwd::eden_cover_key("Game A/game.nsp");
    write(cache + ".name", "  A cached Pokémon title\r\n");
    // Tiny uncompressed, top-origin 24-bit TGA, like Eden's covers.
    std::string tga(18 + 2 * 2 * 3, '\0');
    tga[2] = 2;
    tga[12] = 2;
    tga[14] = 2;
    tga[16] = 24;
    tga[17] = 32;
    for (std::size_t i = 18; i < tga.size(); ++i)
        tga[i] = static_cast<char>(i * 7);
    write(cache + ".tga", tga);
    const auto games = fwd::read_eden_library(data);
    expect(games.size() == 6, "loose, nested and compressed ROMs; no named add-ons or deep files");
    const auto *a = find(games, "Game A/game.nsp");
    const auto *b = find(games, "Game B/game.nsp");
    expect(a != nullptr && a->name == "A cached Pokémon title" &&
               a->location == roms + "/Game A/game.nsp" && a->cover == cache + ".tga" &&
               a->kind == "NSP",
           "cached name, TGA cover and relative launch path");
    expect(b != nullptr && b->name == "game" && b->cover.empty(), "uncached name fallback");
    expect(!games.empty() && games.front().path == "Game A/game.nsp", "sorted by display name");
    expect(find(games, "Upper.XCI") != nullptr && find(games, "Upper.XCI")->kind == "XCI",
           "case-insensitive ROM extensions");
    expect(fwd::eden_game_title("folder/[Game] My Game [0100000000000000][v0].nsp") == "My Game",
           "readable dump filename");
    expect(fwd::library_of("PPSA99008") == fwd::GameLibrary::eden &&
               fwd::library_game_name("PPSA99008", "Folder/Game [v0].nsp") == "Game",
           "Eden target and display name");
    const auto picked = fwd::eden_game_from_file(roms + "/Game A/game.nsp", data);
    expect(picked.path == "Game A/game.nsp" && picked.name == "A cached Pokémon title" &&
               picked.cover == cache + ".tga",
           "file picker imports the same metadata");
    const std::string outside = root + "/other/roms/Game.nsp";
    expect(fwd::eden_game_from_file(outside, data).path == outside,
           "outside configured roms retains absolute path, even with another roms component");
    write(cache + ".name", std::string(513, 'x'));
    expect(fwd::eden_game_from_file(roms + "/Game A/game.nsp", data).name == "game",
           "oversized cached name falls back");

    fwd::Forwarder tile;
    tile.title_id = "PPSA99778";
    tile.display_name = picked.name;
    tile.target = "PPSA99008";
    tile.rom = picked.path;
    tile.exit_after_game = true;
    fwd::Assets art;
    art.icon_png =
        fwd::make_poster_icon_png(reinterpret_cast<const unsigned char *>(tga.data()), tga.size());
    int w = 0, h = 0;
    std::vector<unsigned char> pixels;
    expect(!art.icon_png.empty() &&
               fwd::decode_image(art.icon_png.data(), art.icon_png.size(), w, h, pixels) &&
               w == 512 && h == 512,
           "TGA cover becomes a 512x512 tile icon");
    write(root + "/template/eboot.bin");
    write(root + "/template/sce_module/libc.prx");
    const auto written = fwd::write_forwarder(root + "/tiles", root + "/template", tile, art);
    fwd::Forwarder loaded;
    expect(written.ok && fwd::load_forwarder(root + "/tiles/PPSA99778", loaded) &&
               loaded.rom == "Game A/game.nsp" && loaded.display_name == picked.name &&
               loaded.has_icon && loaded.exit_after_game,
           "generated tile round-trip retains name, icon, relative ROM and exit flag");
    const auto args = loaded.resolved_args();
    expect(args.size() == 2 && args[0].key == "--rom" && args[0].value == "Game A/game.nsp" &&
               args[1].key == "--exit-after-game",
           "correct launch arguments");
    expect(fwd::write_forwarder(root + "/tiles", root + "/template", loaded, {}).ok,
           "editing preserves the imported icon");
    std::filesystem::remove_all(root);
    if (failures != 0)
        return 1;
    std::puts("ProsperoEden library checks passed.");
    return 0;
}
