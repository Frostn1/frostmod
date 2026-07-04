# Changelog

## 2026-07-03
### Added
- Early injection + deferred hooking to catch the one-time startup mods scan. Testing showed `0x158be0` is a generic scanner and only `ui`/`str` ran through it after our (2s-late) injection — the mods/`pkz` scan happens earlier. Now `frostmod.exe` injects ~400ms after the game appears (override with `--wait <ms>`), and the DLL waits for SteamStub to decrypt the code (`WaitForScanner`) before installing content hooks, so it's hooked before the game runs its scan. Render hooks now wait for `opengl32` to load (it isn't mapped that early). Injection retries a few times since a just-launched process can briefly reject it. Compile-time stamp added to exe/dll output to catch stale builds.

### Fixed
- Reload replayed the wrong scan. The scanner is generic — `(status, dir, file-extension, buf)` — and the game calls it for many folders (e.g. `ui`/`str`). The old capture kept whatever was scanned last, so reload replayed a non-mods scan and did nothing. Now the hook logs every distinct `(dir, ext)` the game scans and keeps the `ext="pkz"` call (the mods/content mount) as the replay target.

### Added
- Signature (AOB) validation of `offsets.h`: before hooking, the DLL checks the bytes at the scanner RVA against the known signature; on mismatch it scans the executable sections for the pattern and uses the found address, logging `[sig] VERIFIED` / `RELOCATED (delta …)` / `not found`. If the scanner can't be located, content hooks are skipped (reload disabled) instead of hooking wrong code. The registry-reset (which has no signature) gets the same delta as a best-effort, flagged as unverified.
- `frostmod.exe` is now resident and auto-(re)injects: it waits for the game, injects on launch, and re-injects a fresh DLL on every relaunch — so a rebuilt DLL always takes effect without manual restart juggling. It no longer quits when the game closes (only `Q`/Ctrl+C exits).
- Reload reports clearly when content hooks weren't installed (offsets didn't match) instead of the misleading "not captured" message.

### Fixed
- Header/UI no longer prints a garbled snowflake (`Γ¥ä`): the UTF-8 emoji didn't render in the console/GDI code page, so the console banner and the in-game window label are now plain ASCII.
- The console now actually shows the DLL's log: both `frostmod.exe` and `frostmod.dll` write/read `frostmod.log` **next to the binaries** (same folder) instead of `%TEMP%`, which a Steam-launched game can resolve differently — that mismatch was hiding all DLL output. Falls back to `%TEMP%` only if the folder is read-only.
- Log tailing re-opens the file each poll, avoiding stale-handle/caching issues while the DLL appends.

### Added
- Render-hook heartbeat: the DLL logs `[tick] render hook alive` on the first presented frame, and `[event] reload signal received` when triggered from the console — so you can tell whether the per-frame hook (which runs the reload) is firing at all.
- Launcher prints a hint if no DLL output appears within a few seconds of injecting, and a reminder to open a content menu once so the scan can be captured.

### Added (earlier today)
- README: detailed Windows build guide for Visual Studio 2026 (prerequisites, x64 Native Tools prompt, build steps, common issues)
- `frostmod.exe` now runs as a persistent launcher/monitor: after injecting it stays open, lists the `.pkz` mods found in the MX Bikes mods folder, watches that folder and prints new/removed mods live, and streams `frostmod.log` to the console. Press `R` to reload, `Q`/Ctrl+C to quit.
- Launcher waits for `mxbikes.exe` if it isn't running yet, so you can start FrostMod first and have it inject at game launch (also captures the startup scan automatically).
- `--mods "<path>"` option to override the mods folder the launcher watches.
- Cross-process reload trigger: the DLL creates a `Local\FrostModReload` event that `frostmod.exe`'s `R` key signals, consumed on the render thread.

### Changed
- The executable now builds as `frostmod.exe` instead of `injector.exe` (CMake target `frostmod_app` with `OUTPUT_NAME frostmod`); `src/injector.cpp` renamed to `src/launcher.cpp`.
- README updated for the new `frostmod.exe` workflow (run `frostmod.exe`, not `injector.exe`).
