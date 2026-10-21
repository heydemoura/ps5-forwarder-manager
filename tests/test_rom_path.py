#!/usr/bin/env python3
# ps5fwdgen - ROM picker paths survive forwarder generation and editing.
# Copyright (C) 2026 heydemoura
# SPDX-License-Identifier: GPL-3.0-or-later

import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

PROGRAM = r"""
#include "fwd/store.hpp"
#include "fwd/rom_name.hpp"
#include <cassert>
int main(int, char **argv)
{
    fwd::Forwarder game;
    game.title_id = "PPSA99583";
    game.target = argv[3];
    game.rom = fwd::rom_path_for(game.target, argv[4]);
    assert(game.rom == argv[5]);
    game.display_name = fwd::name_from_rom(game.rom);
    assert(game.display_name == argv[6]);
    game.exit_after_game = true;
    game.extra_args.push_back({"--example", "value"});
    const auto args = game.resolved_args();
    assert(args.size() == 3 && args[0].key == "--rom" && args[0].value == argv[5]);
    fwd::Assets assets;
    assets.icon_png = {137, 80, 78, 71};
    assert(fwd::write_forwarder(argv[1], argv[2], game, assets).ok);
    fwd::Forwarder loaded;
    assert(fwd::load_forwarder(std::string(argv[1]) + "/" + game.title_id, loaded));
    assert(loaded.rom == game.rom && loaded.display_name == game.display_name);
    assert(loaded.exit_after_game && loaded.extra_args.size() == 1);
    assert(loaded.extra_args[0].key == "--example" && loaded.extra_args[0].value == "value");
    loaded.display_name = "Edited title";
    assert(fwd::write_forwarder(argv[1], argv[2], loaded, {}).ok);
}
"""


class RomPathTests(unittest.TestCase):
    def test_picked_paths_generate_and_edit_without_losing_folders(self):
        cases = [
            ("PPSA99008", "/mnt/usb0/eden/roms/Mario Kart 8 Deluxe/Mario Kart 8 Deluxe.nsp",
             "Mario Kart 8 Deluxe/Mario Kart 8 Deluxe.nsp", "Mario Kart 8 Deluxe"),
            ("PPSA99008", "/mnt/usb1/moved/roms/Mario Kart 8 Deluxe/Mario Kart 8 Deluxe.nsp",
             "Mario Kart 8 Deluxe/Mario Kart 8 Deluxe.nsp", "Mario Kart 8 Deluxe"),
            ("PPSA99008", "/data/eden/roms/Game A/game.nsp", "Game A/game.nsp", "game"),
            ("PPSA99008", "/data/eden/roms/Game B/game.nsp", "Game B/game.nsp", "game"),
            ("PPSA99008", "/data/eden/roms/game.nsp", "game.nsp", "game"),
            ("PPSA99008", "/data/games/Game/game.xci", "/data/games/Game/game.xci", "game"),
            ("PPSA99008", "/data/myroms/Game/game.nsp", "/data/myroms/Game/game.nsp", "game"),
            ("PPSA99008", "/roms/Pokémon/roms/game.nsp", "Pokémon/roms/game.nsp", "game"),
            ("PPSA99008", "game.nsp", "game.nsp", "game"),
            ("PPSA99008", "Game/game.nsp", "Game/game.nsp", "game"),
            ("PPSA99764", "/data/roms/Game/game.iso", "game.iso", "game"),
            ("PPSA99203", "/data/roms/Game/game.iso", "game.iso", "game"),
            ("PPSA12345", "/data/roms/Game/game.bin", "game.bin", "game"),
        ]
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            template = folder / "template"
            (template / "sce_module").mkdir(parents=True)
            (template / "eboot.bin").write_bytes(b"test forwarder")
            (template / "sce_module/libc.prx").write_bytes(b"test runtime")
            (folder / "check.cpp").write_text(PROGRAM)
            include = ROOT / "external/ps5-forwarder-format/include"
            subprocess.run([shutil.which("clang") or "gcc", "-I" + str(include), "-c",
                            str(ROOT / "external/ps5-forwarder-format/src/psfwd.c"),
                            "-o", str(folder / "psfwd.o")], check=True)
            subprocess.run([shutil.which("clang++") or "g++", "-std=c++20",
                            "-I" + str(ROOT / "src"), "-I" + str(include),
                            str(folder / "check.cpp"), str(ROOT / "src/fwd/forwarder.cpp"),
                            str(ROOT / "src/fwd/store.cpp"), str(folder / "psfwd.o"),
                            "-o", str(folder / "check")], check=True)
            for target, picked, expected, name in cases:
                with self.subTest(target=target, picked=picked):
                    subprocess.run([str(folder / "check"), str(folder / "output"),
                                    str(template), target, picked, expected, name], check=True)
                    saved = json.loads((folder / "output/PPSA99583/forwarder.json").read_text())
                    self.assertEqual(saved["target"], target)
                    self.assertEqual(saved["args"],
                                     ["--rom", expected, "--example", "value", "--exit-after-game"])


if __name__ == "__main__":
    unittest.main()
