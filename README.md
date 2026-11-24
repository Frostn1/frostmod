# FrostMod

[![Latest release](https://img.shields.io/github/v/release/Frostn1/frostmod?sort=semver)](https://github.com/Frostn1/frostmod/releases/latest)
[![Release date](https://img.shields.io/github/release-date/Frostn1/frostmod)](https://github.com/Frostn1/frostmod/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/Frostn1/frostmod/total)](https://github.com/Frostn1/frostmod/releases)
[![License: MIT](https://img.shields.io/github/license/Frostn1/frostmod)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows%20x64-blue)](#build)

FrostMod is a client-side toolkit for MX Bikes — and, as far as each title's offsets are
derived, for the other two PiBoSo sims.

## Titles

| | MX Bikes | GP Bikes | Kart Racing Pro |
|---|---|---|---|
| Plugin loads (overlay, voice session block) | yes | yes | yes |
| Live mod reload | yes | not confirmed — refused unless armed | **not yet** |
| Server-browser spam filter | yes | no | no |

Pick a title with `--game mxb` / `gpb` / `krp` (or just start the game — the mods folder
follows whichever one you point at). One binary serves all three.

Reload on a title FrostMod has not derived the addresses for is **refused**, not attempted:
replaying one game's function table inside another calls arbitrary code, and it has taken
GP Bikes down twice. Kart Racing Pro needs one capture run to close that gap — see
[tasks/kart-racing-pro-port.md](tasks/kart-racing-pro-port.md).

## Features

- **Live mod reload** - drop in a track, bike, or skin and it shows up in-game
  instantly without restarting the game.
- **Server-browser spam filter** - hide ad / cheat-shop / "ghost" servers from
  your list. Purely client-side, nobody else is affected.
- **In-game overlay** - a small status hint that shows FrostMod is live and
  reports each reload.
- **Session block for MXB App** - publishes which server you are on and where every
  rider is, so the app can put you in voice with the people around you. Nobody
  installs anything else: MXB App does the voice itself, peer to peer.
- **FrostServer (dedicated servers)** - a server-side companion plugin that
  publishes the track a server is running plus its mxb-mods.com download link, so
  a client can fetch and install that track without leaving the game. The server
  side ships now; the one-click client download button is in progress. See
  [FrostServer](#frostserver-dedicated-servers).

## Build

Windows x64 only. MinHook is fetched automatically during configure.

You need Visual Studio Community 2026 with the **Desktop development with C++**
workload, and CMake. Open the **x64 Native Tools Command Prompt for VS 2026** (a
normal Command Prompt won't have `cl.exe` on `PATH`), then from the project root:

    cmake -B build -A x64
    cmake --build build --config Release

This outputs `frostmod.exe` (the launcher you run) and `frostmod.dll` to
`build\bin`, plus the `frostserver.dll` / `frostserver.dlo` / `frostserver.exe`
server-side companion (see [FrostServer](#frostserver-dedicated-servers)).

## Usage

Run **`frostmod.exe`**. It waits for `mxbikes.exe`, injects `frostmod.dll`, and
stays open as a console - listing the mods it finds and streaming its log. For another
title, add `--game gpb` or `--game krp`: that picks the process to wait for, the mods
folder to list, and the plugin identity to answer with.

- **Reload mods** - drop a `.pkz` into your mods folder, then press **`R`** in the
  console, or **`F8`** in-game to open the FrostMod menu and pick **Reload all mods**. For
  new paints only, **Refresh paints** is much faster; **Refresh gear and paints** covers new
  helmets, boots and rider models too; **Refresh tracks** (`T`) and **Refresh bikes** (`B`)
  rescan just those. A refresh asked for while a bike, paint or event
  chooser is open waits until you leave that screen.
- **Menu key** - `F8` opens the FrostMod menu unless you pick another key. If `F8` clashes
  with something else (ReShade screenshots, say), open the menu, press **`K`** (Change menu
  key) and press the new key, with Ctrl/Shift/Alt if you want them; `Esc` cancels. It is saved
  as `menukey=` in `frostmod_radar.cfg`, next to `frostmod.log` (for example
  `menukey=Ctrl+F9`). Everywhere these docs say `F8`, read your menu key.
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

## FrostMod Fixes (crash guards only)

`frostmod_fixes.dlo` is the crash-guard part of FrostMod as a separate MX Bikes
plugin. It is for people who want the crash fixes without the overlay, network,
MXB App or an account. Download `FrostModFixes.zip`, then copy
`frostmod_fixes.dlo` into `MX Bikes\plugins`.

All guards are on by default. An optional `frostmod_fixes.cfg` beside the plugin
can turn individual guards off with `key=0`. The keys are `mapguard`,
`shockguard`, `bikelistguard`, `wakeguard`, `listboxguard`, `ptr64fix`,
`riderinfoguard`, `mipmapguard`, `eventcfgguard` and `nanrecover`. The trainer, tyre-fallback,
GHS and terrain guards have no off key. `frostmod_fixes.log`, also beside the
plugin, is rewritten each game start.

Do not install it alongside full FrostMod. If `frostmod.dlo` is in the same
plugins folder, or full FrostMod is already running in the game, FrostMod Fixes
installs nothing and records the reason in its log. MXB App users already have
the guards through full FrostMod and do not need this plugin.

If `mxbfix.dlo` is in the same plugins folder, FrostMod Fixes reads MXBFix's
`mxbfix.ini`. While the matching MXBFix fix is enabled, it leaves these five
guards to MXBFix: mapguard (`gate-vertex-pointer`), terrain
(`ground-nan-guard`), shockguard (`suspension-nan-guard`), GHS
(`trainer-cleanup-reset`) and trainer (`trainer-series-lookup`). Every other
FrostMod guard stays on. Set the matching MXBFix key to `0` to use FrostMod's
guard instead.

In FrostMod 0.51.0 + 0.52.0 reports on the current game build, from 1,321
reports by 523 riders between 2026-11-03 and 2026-11-15, these were the main
crashes the guards address:

- Quad-batch vertex writes while riding, mostly for riders using ReShade:
  45.2% (`+0x24085C` 43.3%, `+0x2408C4` 1.0%, `+0x240904` 0.9%) - `mapguard`.
- Rider info read while someone joins your server mid-handshake (menus, loading):
  7.9% (`+0x7A860` 6.9%, `+0x7A8E0` 1.0%) - `riderinfoguard`.
- A menu texture loaded without memory: 4.7% (`+0x255F1F` 3.2%,
  `+0x255EF0` 1.5%) - `mipmapguard`.
- A bad string passed by the game to `msvcr90.dll+0x36EDE`: 4.0%, partly
  covered by the trainer guard and tyre fallback. Part of this crash is still open.
- Rarer crashes from older reports: terrain NaN (`+0x1F1923`), GHS
  (`+0x11D753`), shock NaN (`+0x1A29AE`), the physics NaN reset, bike list
  (`+0x45B0`), wake (`+0x3B940`), list box (`+0x1FED9E`) and ptr64 (`+0x2A42F0`).

The first three account for 57.8% of the crashes we see in those reports. The
GPU-driver crashes (16.7%) and crashes in other plugins or overlays (5.3%) are
outside what any game-side guard can fix. Together, they are about 22% of reports.


## FrostServer (dedicated servers)

**FrostServer** is FrostMod's server-side companion — a separate deliverable for
people who run an MX Bikes **dedicated server**. Drop `frostserver.dlo` into the
server's `plugins` folder and it publishes, over a tiny read-only HTTP API, the
track the server is currently running and a mxb-mods.com link to download it (you
set the per-track links in `frostserver.yaml`). A FrostMod client can then fetch
that link and install the track — via the
[MXB App](https://github.com/Frostn1/mxb-app) — so players join without hunting
the map down and restarting the game.

A standalone `frostserver.exe` serves the same API with no game attached, for
testing (`frostserver.exe --track "Some Track Name"`). Full HTTP contract, config
format, and setup are in [docs/FROSTSERVER.md](docs/FROSTSERVER.md).

> The server side (this) ships now. The one-click **Download this map** button in
> the client's server browser is the next step.


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
