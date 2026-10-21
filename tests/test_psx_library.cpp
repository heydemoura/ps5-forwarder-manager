// ps5fwdgen - Host tests for SwanStationPS5's game list and its forwarders'
// arguments (fwd/psx_library.hpp).
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fwd/forwarder.hpp"
#include "fwd/library_game.hpp"
#include "fwd/psx_library.hpp"
#include "fwd/store.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace
{

int failures = 0;

void expect(bool condition, const std::string &what)
{
    if (!condition)
    {
        std::fprintf(stderr, "failed: %s\n", what.c_str());
        ++failures;
    }
}

void make_dirs(const std::string &path)
{
    for (std::size_t slash = path.find('/', 1);; slash = path.find('/', slash + 1))
    {
        (void)::mkdir(path.substr(0, slash).c_str(), 0755);
        if (slash == std::string::npos)
            break;
    }
}

void write(const std::string &path, const std::string &text = "x")
{
    make_dirs(path.substr(0, path.find_last_of('/')));
    std::FILE *file = std::fopen(path.c_str(), "wb");
    if (file == nullptr)
        std::abort();
    (void)std::fwrite(text.data(), 1, text.size(), file);
    (void)std::fclose(file);
}

const fwd::LibraryGame *find(const std::vector<fwd::LibraryGame> &games, const std::string &name)
{
    for (const fwd::LibraryGame &game : games)
        if (game.name == name)
            return &game;
    return nullptr;
}

} // namespace

int main()
{
    char pattern[] = "/tmp/psx-library-XXXXXX";
    const char *made = ::mkdtemp(pattern);
    if (made == nullptr)
        return 1;
    const std::string root = made;
    const std::string games = root + "/games";
    const std::string covers = root + "/covers";

    // A folder game: its .cue wins over the .bin tracks, with a cover beside it.
    const std::string sotn = games + "/Castlevania - Symphony of the Night (USA)";
    write(sotn + "/Castlevania - Symphony of the Night (USA).cue");
    write(sotn + "/Castlevania - Symphony of the Night (USA) (Track 1).bin");
    write(sotn + "/Castlevania - Symphony of the Night (USA) (Track 2).bin");
    write(sotn + "/cover.jpg");
    // A playlist beats its discs; a folder of .bin only is still a game.
    write(games + "/Final Fantasy VII [SCUS-94163]/Final Fantasy VII.m3u");
    write(games + "/Final Fantasy VII [SCUS-94163]/Final Fantasy VII (Disc 1).chd");
    write(games + "/Raw/game.bin");
    write(games + "/Empty/readme.txt");
    // Loose images: a .chd named like a cover in covers/, a .bin left out
    // because a sheet sits in the same folder.
    write(games + "/Crash Bandicoot (USA).chd");
    write(covers + "/Crash Bandicoot (USA).png");
    write(games + "/Spyro_the_Dragon (Disc 1).cue");
    write(games + "/Spyro_the_Dragon (Disc 1).bin");

    const std::vector<fwd::LibraryGame> list =
        fwd::read_psx_library({games, root + "/missing"}, covers);
    expect(list.size() == 5, "five games, got " + std::to_string(list.size()));
    const fwd::LibraryGame *castlevania = find(list, "Castlevania - Symphony of the Night (USA)");
    expect(castlevania != nullptr &&
               castlevania->path == sotn + "/Castlevania - Symphony of the Night (USA).cue" &&
               castlevania->kind == "CUE" && castlevania->cover == sotn + "/cover.jpg",
           "a folder game: its sheet and its cover");
    const fwd::LibraryGame *ff7 = find(list, "Final Fantasy VII");
    expect(ff7 != nullptr && ff7->kind == "M3U", "a playlist wins, [serial] dropped");
    expect(find(list, "Raw") != nullptr, "a folder of a lone .bin");
    expect(find(list, "Empty") == nullptr, "a folder without an image");
    const fwd::LibraryGame *crash = find(list, "Crash Bandicoot (USA)");
    expect(crash != nullptr && crash->cover == covers + "/Crash Bandicoot (USA).png",
           "a loose image, its cover in covers/");
    const fwd::LibraryGame *spyro = find(list, "Spyro the Dragon");
    expect(spyro != nullptr && spyro->kind == "CUE", "disc number and underscores cleaned");
    expect(list.front().name == "Castlevania - Symphony of the Night (USA)", "sorted by name");

    expect(fwd::psx_game_paths().size() == 18 &&
               fwd::psx_game_paths().front() == "/data/SwanStationPS5/games",
           "SwanStationPS5's folders");
    expect(fwd::library_game_name("PPSA98510", "/x/Tekken 3 (USA) [SCUS-94402].cue") ==
               "Tekken 3 (USA)",
           "name from a path");

    // The forwarder gives the full path as --rom, with the exit flag, and reads it back.
    expect(fwd::library_of("PPSA98510") == fwd::GameLibrary::psx &&
               fwd::library_of("PPSA50011") == fwd::GameLibrary::x360 &&
               fwd::library_of("PPSA99008") == fwd::GameLibrary::eden,
           "libraries by target");
    fwd::Forwarder forwarder;
    forwarder.title_id = "PPSA99778";
    forwarder.display_name = "Castlevania";
    forwarder.target = "PPSA98510";
    forwarder.rom = castlevania != nullptr ? castlevania->path : "";
    forwarder.exit_after_game = true;
    const std::vector<fwd::Argument> args = forwarder.resolved_args();
    expect(args.size() == 2 && args[0].key == "--rom" && args[0].value == forwarder.rom &&
               args[1].key == "--exit-after-game",
           "--rom <full path> --exit-after-game");
    write(root + "/template/eboot.bin");
    write(root + "/template/sce_module/libc.prx");
    make_dirs(root + "/forwarders");
    fwd::Assets assets;
    assets.icon_png = {0x89, 'P', 'N', 'G'};
    const fwd::WriteResult written =
        fwd::write_forwarder(root + "/forwarders", root + "/template", forwarder, assets);
    fwd::Forwarder loaded;
    expect(written.ok && fwd::load_forwarder(root + "/forwarders/PPSA99778", loaded) &&
               loaded.rom == forwarder.rom && loaded.exit_after_game,
           "the forwarder reads back");

    (void)std::system(("rm -rf '" + root + "'").c_str());
    if (failures != 0)
        return 1;
    std::puts("SwanStation library checks passed.");
    return 0;
}
