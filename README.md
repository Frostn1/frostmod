# FrostMod ❄

A small client-side toolkit for MX Bikes: reload your mods without restarting the
game, and clean up the online server browser. Same idea as BakkesMod. Windows x64 only.

MX Bikes only scans your mods folders at startup, so tracks, bikes, or skins you
add while it's running don't show up until you relaunch — and its server browser
keeps filling up with ad / "ghost" spam. FrostMod fixes both from the outside: a
live **mod reload** (no restart, no loading screen) and a client-side **server-list
filter**, plus a small in-game overlay. Trigger reload from the `frostmod.exe`
console, an **F8** hotkey, or the overlay.

## Build

Windows x64 only — it's a DLL for a 64-bit Windows game. MinHook is fetched
automatically during configure.

### Prerequisites

- Visual Studio Community 2026
- **Desktop development with C++** workload installed
- CMake

### Open the correct command prompt

Do **not** use a normal Command Prompt. Open the **x64 Native Tools Command
Prompt for VS 2026** — it puts the Visual C++ compiler (`cl.exe`) on `PATH` so
CMake can find it. Verify with:

    cl

If it prints a compiler version, the environment is configured correctly.

### Build the project

From the project root (the directory containing `CMakeLists.txt`):

    cmake -B build -A x64
    cmake --build build --config Release

Outputs `frostmod.exe` (the launcher you run) and `frostmod.dll` (the injected
mod) to `build\bin`.

### Common issues

**`'cl' is not recognized`** — you're in a normal Command Prompt. Open the
**x64 Native Tools Command Prompt for VS 2026** instead.

**`Generator "NMake Makefiles" does not support platform specification`** —
`NMake Makefiles` doesn't accept `-A x64`. Force the Visual Studio generator:

    cmake -B build -G "Visual Studio 18 2026" -A x64

**`CMAKE_CXX_COMPILER not set`** — either the **Desktop development with C++**
workload isn't installed, or you started from a normal Command Prompt instead of
the **x64 Native Tools Command Prompt for VS 2026**.

## Use

1. Run **`frostmod.exe`**. It waits for `mxbikes.exe`, injects `frostmod.dll`,
   then stays open showing:
   - the `.pkz` mods it found in your MX Bikes mods folder (live — new files you
     drop in are printed as they appear), and
   - a live stream of `frostmod.log`.
2. Add a `.pkz` — track, bike, skin, or gear — to your mods folder.
3. Reload with **`R`** in the `frostmod.exe` console or **F8** in-game. The new
   content appears in the menus immediately — no loading screen, no bounce to the
   menu. Press `Q` (or Ctrl+C) to quit the console.

A small in-game overlay (top-left; toggle with **F7**) shows FrostMod is live and
reports each reload. You can start `frostmod.exe` before the game — it waits and
injects on launch.

Options: `frostmod.exe --process gpbikes.exe`, `--mods "D:\path\mods"`,
`--filter-servers` (see below), or an explicit DLL path. The log is written next to
the binaries (`…\build\bin\Release\frostmod.log`, falling back to `%TEMP%` if that
folder is read-only) — the console streams it for you.

### Reload isn't taking?

Watch the log the console streams. You want to see `[tick] render hook alive` (the
per-frame hook fires — required for reload to run at all), then on reload
`[reload] surgical content reload …` → `[reload] done`. If you never see `[tick]`,
the game isn't calling the present function we hooked. If reload aborts with an
offsets-mismatch, the game build has shifted from the one `offsets.h` was built for
(see *How it works*).

## Run as a plugin (recommended)

`frostmod.dll` is also a **PiBoSo plugin**. Drop it in MX Bikes' `plugins` folder
and the game loads it at startup itself — no injector needed. It exports the PiBoSo
identity/lifecycle functions (`GetModID` → `"mxbikes"`, `GetInterfaceVersion` → `9`,
`GetModDataVersion` → `8`, `Startup`, `Shutdown`) and installs the same hooks from
`Startup()`. The injector (`frostmod.exe`) still works as a fallback and log viewer.

Full details — exports, lifecycle, hooks, files — are in
[docs/PLUGIN.md](docs/PLUGIN.md).

## How it works

**Reload.** FrostMod calls the game's own content-load routine — the same per-type
loaders MX Bikes runs at startup (tracks, bikes, tyres, gear, …) — directly on the
render thread, but **stops before** the input/sound/Steam re-init and the menu
transition. So every content list is rebuilt from disk and new mods appear live,
with no loading screen and no bounce to the menu. Offsets are in `src/offsets.h`.

**Offsets are validated at runtime.** Those RVAs come from one specific
`mxbikes.exe`; a game update shifts them. So FrostMod doesn't trust the address — it
checks the bytes at the scanner RVA against a known signature, and if they don't
match it scans the executable for the pattern and uses whatever it finds. The log
tells you which happened (`[sig] … VERIFIED`, `… RELOCATED`, or `… not found`). If
the build can't be validated, reload is disabled instead of calling the wrong code —
the mods list, overlay, and logs still work.

`frostmod.exe` stays resident: it injects when the game launches and **re-injects
automatically on every relaunch**, so a rebuilt DLL always takes effect (Windows
won't hot-swap a DLL that's already loaded in a live process — you must relaunch the
game, which FrostMod handles for you).

Offline use only. Don't inject during online sessions.

## Server-list spam filter (client-side)

MX Bikes gets its online server list from the master server
(`master.mx-bikes.com`, UDP 54200). FrostMod can hide junk / ad "ghost" servers
from *your* browser — purely client-side, nobody else is affected.

Rules live in `frostmod_serverfilter.txt` (created next to the binaries, with docs,
on first run): hide by **name** substring, name **regex** (catches ad URLs / cheat
shops / Discord invites), **unjoinable** (ping shows "---"), too-many-from-one-IP,
password-locked, or empty. Edit the file and reload (**`R`**) to apply it live;
hidden servers are logged as `[filter] hid '<name>' (<reason>)`.

Enable it with **`frostmod.exe --filter-servers`** (opt-in — it splices the server
browser's populate loop, verified against the expected bytes first, to drop rows the
rules reject). Purely client-side — only your view changes.

## License

FrostMod is under the MIT License — see LICENSE. It bundles MinHook
(BSD 2-Clause) — see THIRD-PARTY-LICENSES.txt.
