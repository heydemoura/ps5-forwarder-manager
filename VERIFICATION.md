# Verification status

This records what was verified for the PS5 Forwarder Generator and the one
step that needs the owner's SteamGridDB API key.

## Verified on the console (192.168.0.77, firmware 12.20)

- **Kit-based OpenGL build** deploys over FTP, launches, and renders at a
  steady 60 fps (`[FWD] frames=600 mean=~16.67ms`).
- **Lapy launch-time elevation**: the app asks ArkSama's PS5-Lapy-JB-Daemon
  for `/data` access at startup and proves a write/read/list
  (`[FWD] elevation status=ok path=daemon`), then relocates its app root.
- **Debug-log listener** on TCP 3232 (klogsrv) captured every run.
- **Create through the real UI**: scripted input drove Triangle -> New, typed
  the name into the on-screen keyboard prompt, opened the file-picker (kit
  file-manager design: breadcrumb + per-kind icons) and picked a ROM, opened
  the art chooser and encoded a 512x512 icon, and pressed Generate
  (`[FWD] ui-generate title=PPSA99583 ...`). ShadowMountPlus registered it and
  launching the tile forwarded `--rom game.nsp` to PPSA99008 via
  ps5-app-launcher.
- **Edit through the real UI**: opened an existing forwarder, changed its name
  via the prompt, saved; `param.json` titleName updated, icon preserved.

## Verified off-console (host + live endpoints)

- **Forwarder format**: generated `param.json` is byte-equal to the site's
  template; `forwarder.json` matches `{target,args}`; DDS background is
  8294548 bytes (same as the console's), icon a valid 512x512 PNG.
- **Non-AT9 audio conversion**: a WAV POSTed to the site's `/api/convert-at9`
  returns a valid RIFF/WAVE ATRAC9 file; the app uses the same multipart form.
- **SteamGridDB client, live API**: the real `steamgriddb.cpp` + `json.cpp`
  ran against `https://www.steamgriddb.com/api/v2` and correctly handled the
  responses (request built, `Authorization: Bearer` sent, success/errors
  envelope parsed). On-device the search reached the live server from the
  elevated app (real HTTP 401 for an invalid key, no transport/TLS error).
- **SteamGridDB success path**: the identical client + image code ran
  search -> games, grids -> assets, download, and 512x512 icon encode against
  a server serving the documented SteamGridDB response shapes (used because no
  valid key was available; the server required the bearer header).

## The one step that needs your API key

A successful authenticated response from the real SteamGridDB API needs a key
tied to a Steam account (steamgriddb.com/profile/preferences/api). None is
stored on the console or this machine, and it cannot be obtained without a
Steam login. Two ways to complete it:

1. On a host with this repo and libcurl:

   ```
   # build once (Homebrew curl shown; any libcurl dev headers work)
   clang++ -std=c++20 -I. -Isrc -I<curl-include> \
     tools/sgdb_check.cpp src/net/steamgriddb.cpp src/core/json.cpp \
     src/fwd/image.cpp tools/net_host.cpp -L<curl-lib> -lcurl -o sgdb_check
   ./sgdb_check <YOUR_KEY> "Celeste"
   # prints the games found, grids, download size, and the encoded 512x512 icon
   ```

2. On the console: launch the app, open Settings, enter your key, create a
   forwarder, choose the tile icon, pick "Search SteamGridDB", type a game.
   The search, thumbnail grid, and icon download run on-device.

`tools/sgdb_check.cpp` and `tools/net_host.cpp` are the host harness used for
verification (the real client code with a host libcurl transport).
