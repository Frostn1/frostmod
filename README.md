# FrostMod ❄

Reloads the MX Bikes mods folder without restarting the game.

MX Bikes only scans your mods folders at startup, so tracks or skins you add
while it's running don't show up until you relaunch. FrostMod re-runs the game's
own content scan on demand — trigger it from the `frostmod.exe` console, an F8
hotkey, or a small floating in-game window. Same idea as BakkesMod. Windows x64 only.

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
   - a live stream of `frostmod.log` (capture / reload activity).
2. Open the track/bike menu once so FrostMod can capture the scan arguments.
3. Add a `.pkz` to your mods folder.
4. Reload with **`R`** in the `frostmod.exe` console, **F8** in-game, or the
   floating window button. Press `Q` (or Ctrl+C) to quit the console.

You can start `frostmod.exe` before the game — it waits and injects on launch,
which also lets it capture the startup scan automatically.

Options: `frostmod.exe --process gpbikes.exe`, `--mods "D:\path\mods"`, or an
explicit DLL path. Logs also go to `%TEMP%\frostmod.log`.

## How it works

It hooks the game's folder scanner (`0x158be0`) and registry reset (`0x159340`),
records the arguments the game passes when it scans, and replays them on the
render thread when you hit Reload. Offsets are in `src/offsets.h`.

The scanner skips folders it already loaded, so a plain re-scan does nothing —
that's why FrostMod resets the registry first. If new files still don't appear,
check the log and the notes in `frostmod.cpp`.

Offline use only. Don't inject during online sessions.

## License

FrostMod is under the MIT License — see LICENSE. It bundles MinHook
(BSD 2-Clause) — see THIRD-PARTY-LICENSES.txt.