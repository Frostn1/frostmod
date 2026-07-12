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
  console, or **`F8`** in-game to open the FrostMod menu and pick **Reload**.
- **Filter servers** - on by default. Tune the rules in `frostmod_serverfilter.yaml`
  (created next to the binaries, with docs, on first run), or pass
  `--no-filter-servers` to turn it off.
- **Quit** - `Q` or Ctrl+C.

Full command-line reference — every flag, key, and file — is in
[docs/USAGE.md](docs/USAGE.md).

You can also run it as a **PiBoSo plugin**: drop `frostmod.dlo` into MX Bikes'
`plugins` folder (next to `mxbikes.exe`) and the game loads it at startup — no
injector needed. Run `frostmod.exe --install-plugin` to copy it there for you.
In plugin mode the in-game overlay renders through the game's sanctioned `Draw()`
callback. See [docs/PLUGIN.md](docs/PLUGIN.md).


## Troubleshooting

### "Is this a virus?" — my antivirus or SmartScreen flagged FrostMod

**No, FrostMod isn't malware.** It's open source under the MIT license, so you can
read every line in this repo — and if you'd rather not trust a prebuilt binary at
all, you can build your own from that source. The release binaries aren't
code-signed yet (see [Code signing](#code-signing) below), so Windows *will* warn
about them — but a warning isn't a detection. Two things can show up, and neither
means the file is infected:

- **SmartScreen "unknown publisher" / "not commonly downloaded."** Expected — the
  binaries aren't signed yet, and this is a *reputation* prompt, not a malware
  detection. Click **More info → Run anyway**.
- **A heuristic / "behavioral" antivirus hit** (often a generic name like
  `Injector`, `HackTool`, or `Wacatac`). To load mods live, `frostmod.exe` injects
  `frostmod.dll` into `mxbikes.exe` (the classic `CreateRemoteThread` +
  `LoadLibraryA` technique) and hooks a few game functions. That's what a mod
  loader *has* to do — but it's also what some malware does, so signature-less
  behavioral engines sometimes raise a **false positive**. FrostMod touches no file
  outside its own folder, makes no network connections of its own beyond the
  optional GitHub update check, and never persists itself anywhere except the login
  entry you explicitly opt into with `--install-startup`.

If you'd rather nothing be injected at all, run FrostMod in **plugin mode**: it
loads through MX Bikes' official PiBoSo plugin API (`frostmod.dlo` in the `plugins`
folder, or `frostmod.exe --install-plugin`) with **no injection and no remote
thread**, which almost never trips antivirus. See [docs/PLUGIN.md](docs/PLUGIN.md).

To be certain the binary is genuine:

- **Build it yourself** from source (see [Build](#build)) and run your own DLL —
  the surest check of all, and it sidesteps the "unknown publisher" prompt.
- Only download from the **official GitHub Releases page** — never a re-upload or
  mirror.
- If your AV still blocks it, add an exclusion for the FrostMod folder and/or
  **report the false positive** to your vendor so they can whitelist it.

For the full picture — which VirusTotal engines flag it and why (spoiler: only the
machine-learning ones, never the signature-based engines like Defender/Kaspersky/
ESET), plus copy-paste templates for disputing it with each vendor — see
[docs/antivirus-false-positives.md](docs/antivirus-false-positives.md).

### Injection fails / "access denied" / the DLL never loads

`frostmod.exe` has to open `mxbikes.exe` to inject. Run it as the **same Windows
user** as the game, and **elevated (Run as administrator)** if the game runs
elevated — otherwise injection fails with an access error. Or sidestep injection
entirely with **plugin mode** (above).

### Nothing happens / no log output / the overlay never appears

- Check **`frostmod.log`** (next to the binaries, or `%TEMP%\frostmod.log` if that
  folder is read-only) — it records what the DLL is doing.
- The overlay only shows once the game starts calling its present/draw hook; give
  it until you reach a menu or the track.
- Make sure the folder holding the binaries is **writable** — the DLL writes its log
  and flag files there.

### "… is already loaded (another injector running?)"

You already have FrostMod (or another injector) attached to that game process. Quit
the other instance, or fully close and relaunch MX Bikes, then start `frostmod.exe`
once.

### `--update` fails or says the DLL is locked

**Close MX Bikes first.** Windows locks `frostmod.dll` while the game has it loaded,
so the updater can't overwrite it. Then re-run `frostmod.exe --update`.

### The game isn't detected

FrostMod waits for `mxbikes.exe` by default. On a different executable (e.g. GP
Bikes) point it there with `--process gpbikes.exe`.


## Code signing

FrostMod's Windows binaries are **not code-signed yet**, so Windows will show
"unknown publisher" / SmartScreen warnings — see
[Troubleshooting](#troubleshooting) for why that's expected and how to confirm a
download is genuine.

Signing is on the roadmap. The plan is a free certificate from the
[SignPath Foundation](https://signpath.org/), which sponsors open-source projects —
they declined for now because FrostMod doesn't have enough traction / download
history yet, so the intent is to **re-apply as the project grows**. A paid
code-signing certificate (a recurring yearly cost) is the fallback if that stalls.
Until signing lands, build from source or download only from the official GitHub
Releases page.

## License

MIT — see [LICENSE](LICENSE). Bundles MinHook (BSD 2-Clause); see
THIRD-PARTY-LICENSES.txt.
