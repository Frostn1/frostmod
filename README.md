# FrostMod

FrostMod is a client-side toolkit for MX Bikes.

## Features

- **Live mod reload** - drop in a track, bike, or skin and it shows up in-game
  instantly without restarting the game.
- **Server-browser spam filter** - hide ad / cheat-shop / "ghost" servers from
  your list. Purely client-side, nobody else is affected.
- **In-game overlay** - a small status hint that shows FrostMod is live and
  reports each reload.

## Build

Windows x64 only. MinHook is fetched automatically during configure.

You need Visual Studio Community 2026 with the **Desktop development with C++**
workload, and CMake. Open the **x64 Native Tools Command Prompt for VS 2026** (a
normal Command Prompt won't have `cl.exe` on `PATH`), then from the project root:

    cmake -B build -A x64
    cmake --build build --config Release

This outputs `frostmod.exe` (the launcher you run) and `frostmod.dll` to
`build\bin`.

## Usage

Run **`frostmod.exe`**. It waits for `mxbikes.exe`, injects `frostmod.dll`, and
stays open as a console - listing the mods it finds and streaming its log.

- **Reload mods** - drop a `.pkz` into your mods folder, then press **`R`** in the
  console or **`F8`** in-game.
- **Filter servers** - on by default. Tune the rules in `frostmod_serverfilter.txt`
  (created next to the binaries, with docs, on first run), or pass
  `--no-filter-servers` to turn it off.
- **Quit** - `Q` or Ctrl+C.

Full command-line reference — every flag, key, and file — is in
[docs/USAGE.md](docs/USAGE.md).

You can also run it as a **PiBoSo plugin**: drop `frostmod.dll` into MX Bikes'
`plugins` folder and the game loads it at startup — no injector needed. See
[docs/PLUGIN.md](docs/PLUGIN.md).


## License

MIT — see [LICENSE](LICENSE). Bundles MinHook (BSD 2-Clause); see
THIRD-PARTY-LICENSES.txt.
