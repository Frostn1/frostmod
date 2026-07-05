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
explicit DLL path. The log is written next to the binaries
(`…\build\bin\Release\frostmod.log`, falling back to `%TEMP%` if that folder is
read-only) — the console streams it for you.

### Reload isn't taking?

Watch the log the console streams. In order, you want to see: `[tick] render hook
alive` (the per-frame hook fires — required for reload to run at all), then a
`[capture] scanner …` line (only appears once the game scans — that's why you open
a content menu first), then on reload `[reload] … scanner returned …`. If you
never see `[tick]`, the game isn't calling the present function we hooked; if you
see `[reload] ABORT … not captured`, load FrostMod before the game's startup scan
(plugin mode, or start `frostmod.exe` before launching).

**Reload has several strategies** — cycle them with **`F7`** in-game or **`S`** in
the console and watch the log for which one makes the track appear: `A` replay the
`.pkz` scan, `A+` reset + replay (default), `A++` reset + replay all captured
scans, `B` call the scanner directly (works without a capture, experimental). Bad
attempts are caught (`FAULTED - caught`) rather than crashing the game.

## Run as a plugin (recommended)

`frostmod.dll` is also a **PiBoSo plugin**. Drop it in MX Bikes' `plugins` folder
and the game loads it at startup itself — no injector, and (crucially) it's loaded
*before* the game's one-time mods scan, which is what the live reload needs. It
exports the PiBoSo identity/lifecycle functions (`GetModID` → `"mxbikes"`,
`GetInterfaceVersion` → `9`, `GetModDataVersion` → `8`, `Startup`, `Shutdown`) and
installs the same hooks from `Startup()`. The injector (`frostmod.exe`) still works
as a fallback and log viewer.

Full details — exports, lifecycle, hooks, files — are in
[docs/PLUGIN.md](docs/PLUGIN.md).

## How it works

It hooks the game's folder scanner (`0x158be0`) and registry reset (`0x159340`),
records the arguments the game passes when it scans, and replays them on the
render thread when you hit Reload. Offsets are in `src/offsets.h`.

**Offsets are validated at runtime.** Those RVAs come from one specific
`mxbikes.exe`; a game update shifts them. So FrostMod doesn't trust the address —
it checks the bytes at that RVA against a known signature, and if they don't
match it scans the executable for the pattern and uses whatever it finds. The
log tells you which happened (`[sig] … VERIFIED`, `… RELOCATED`, or `… not
found`). If the scanner can't be located, the content hooks are skipped (reload
is disabled) instead of hooking the wrong code — the mods list and logs still work.

`frostmod.exe` stays resident: it injects when the game launches and **re-injects
automatically on every relaunch**, so a rebuilt DLL always takes effect (Windows
won't hot-swap a DLL that's already loaded in a live process — you must relaunch
the game, which FrostMod now handles for you).

The scanner skips folders it already loaded, so a plain re-scan does nothing —
that's why FrostMod resets the registry first. If new files still don't appear,
check the log and the notes in `frostmod.cpp`.

Offline use only. Don't inject during online sessions.

## Server-list spam filter (client-side)

MX Bikes gets its online server list from the master server
(`master.mx-bikes.com`, UDP 54200). FrostMod can hide junk / ad "ghost" servers
from *your* browser — purely client-side, nobody else is affected.

Rules live in `frostmod_serverfilter.txt` (created next to the binaries, with docs,
on first run): hide by **unjoinable** (ping shows "---" — the ghost/ad signal, on by
default), name substring, name regex, too-many-from-one-IP, password-locked, or
empty. Edit the file and press `R` (or reload) to apply it live. Hidden servers are
logged as `[filter] hid '<name>' (<reason>)`.

Enable it with **`frostmod.exe --filter-servers`** (opt-in — it splices the server
browser's populate loop at `0x0ABAB6`, verified against the expected bytes first, to
skip rows the rules reject). With the default `hideUnjoinable`, the ghost/ad servers
disappear from the browser. Purely client-side — only your view changes.

## License

FrostMod is under the MIT License — see LICENSE. It bundles MinHook
(BSD 2-Clause) — see THIRD-PARTY-LICENSES.txt.