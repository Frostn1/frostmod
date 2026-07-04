# FrostMod ❄

Reloads the MX Bikes mods folder without restarting the game.

MX Bikes only scans your mods folders at startup, so tracks or skins you add
while it's running don't show up until you relaunch. FrostMod adds a small
floating window with a Reload button (and an F8 hotkey) that re-runs the game's
own content scan. Same idea as BakkesMod. Windows x64 only.

## Build

Needs Visual Studio (Desktop development with C++) and CMake. Windows only — it's
a DLL for a Windows game. MinHook is fetched automatically.

    cmake -B build -A x64
    cmake --build build --config Release

Outputs `frostmod.dll` and `injector.exe` to `build\bin`.

## Use

1. Start MX Bikes windowed or borderless so the window shows (in fullscreen, use F8).
2. Run `injector.exe` to load the DLL into `mxbikes.exe`.
3. Open the track/bike menu once so FrostMod can grab the scan arguments.
4. Add a `.pkz` to your mods folder.
5. Click Reload (or press F8).

Logs go to `%TEMP%\frostmod.log`.

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