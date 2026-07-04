# Changelog

## 2026-07-03
### Added
- README: detailed Windows build guide for Visual Studio 2026 (prerequisites, x64 Native Tools prompt, build steps, common issues)
- `frostmod.exe` now runs as a persistent launcher/monitor: after injecting it stays open, lists the `.pkz` mods found in the MX Bikes mods folder, watches that folder and prints new/removed mods live, and streams `frostmod.log` to the console. Press `R` to reload, `Q`/Ctrl+C to quit.
- Launcher waits for `mxbikes.exe` if it isn't running yet, so you can start FrostMod first and have it inject at game launch (also captures the startup scan automatically).
- `--mods "<path>"` option to override the mods folder the launcher watches.
- Cross-process reload trigger: the DLL creates a `Local\FrostModReload` event that `frostmod.exe`'s `R` key signals, consumed on the render thread.

### Changed
- The executable now builds as `frostmod.exe` instead of `injector.exe` (CMake target `frostmod_app` with `OUTPUT_NAME frostmod`); `src/injector.cpp` renamed to `src/launcher.cpp`.
- README updated for the new `frostmod.exe` workflow (run `frostmod.exe`, not `injector.exe`).
