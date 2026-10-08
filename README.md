# PS5 Forwarder Generator

A native PlayStation 5 homebrew app that creates and edits home-screen
**forwarders**: tiles that launch another installed app (an emulator) with
launch arguments such as `--rom <file>`. It is a native port of the web tool
[ps5-forwarder.mph.am](https://ps5-forwarder.mph.am/) by Martin Pham, built for
a jailbroken console the owner develops on for personal use.

## What it does

- Lists the forwarders already in `/data/homebrew` and lets you edit or delete them.
- Creates a new forwarder: display name, target app, title ID, ROM file, an
  `--exit-after-game` flag, and launch arguments.
- Picks the ROM (and optional `.at9` music) with a built-in file browser over
  the console filesystem.
- Fetches tile icons and backgrounds from [SteamGridDB](https://www.steamgriddb.com/)
  (with your own API key) or from any image file, encoding them to the exact
  formats the PS5 shell expects: a 512x512 PNG icon and 3840x2160 BC7 DDS
  backgrounds.
- Writes a complete tile folder (shared `eboot.bin` + `libc.prx`,
  `forwarder.json`, `sce_sys/param.json`, art) that ShadowMountPlus registers.

## Requirements on the console

- A jailbroken PS5 with `elfldr`.
- [ArkSama's PS5-Lapy-JB-Daemon](https://github.com/ArkSama/PS5-Lapy-JB-Daemon)
  running (payload loader or autoload). The app asks it for `/data` access at
  launch; without it the app only shows a notice.
- [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus) to register the
  generated tiles.
- [ps5-app-launcher](https://github.com/MartinPham/ps5-app-launcher) loaded, so
  the forwarder tiles can start their targets.

## Building

This repository is built on the
[ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate)
with the [ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui)
OpenGL kit. From a Linux or WSL host with Clang 18, lld, Make, Ninja and
Python 3:

```bash
make                       # fetches the SDK, ps5-opengl and PacBrew libcurl, then builds
make deploy PS5_HOST=<ip>  # uploads dist/<TITLE_ID>/ to /data/homebrew over FTP
```

The build needs `PACBREW_PACKAGES=libcurl` (for the SteamGridDB HTTPS client,
since an elevated app cannot use `sceHttp`). See `.env.example`.

## Layout

| Path | What |
| --- | --- |
| `src/main.cpp` | Entry point: elevation, then the kit frame loop |
| `src/app/` | The app and its screens (home, edit, file picker, art, SteamGridDB, settings) |
| `src/fwd/` | The forwarder model, the on-disk store, and the icon/DDS image encoders |
| `src/net/` | libcurl glue and the SteamGridDB API v2 client |
| `src/platform/` | Sandbox elevation (the Lapy protocol) and app-root resolution |
| `src/{gfx,ui,audio,core,runtime}` | The ps5-homebrew-ui kit |
| `assets/forwarder-template/` | The shared launcher `eboot.bin` and `libc.prx` |

Licensed GPL-3.0-or-later. See `THIRD_PARTY_NOTICES.md`.
