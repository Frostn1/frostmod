# FrostMod — running it & command-line reference

`frostmod.exe` is a resident launcher: you start it, it waits for the game,
injects `frostmod.dll`, and stays open as a console that lists your mods, streams
the log, and lets you trigger a reload. This page documents every flag, key, and
file.

## Quick start

    frostmod.exe

That's the whole thing — **no flags needed**. With no arguments it:

- waits for `mxbikes.exe`, then injects `frostmod.dll` (and re-injects on every
  relaunch, so a rebuilt DLL always takes effect),
- enables **live mod reload** (press **`R`** after dropping in a `.pkz`),
- enables the **server-browser spam filter** (hides cheat/ad "ghost" servers), and
- shows the **in-game version pill**.

Run it as the **same user** as the game, and **elevated** if the game runs
elevated (otherwise injection fails with an access error).

## Command-line flags

Flags are optional. Order doesn't matter. Anything that isn't a recognized flag is
treated as an explicit path to the DLL.

### Common

| Flag | Argument | Default | What it does |
|------|----------|---------|--------------|
| *(none)* | — | — | Reload **and** the server filter are both on. |
| `--install-startup` | — | off | Run FrostMod automatically at every login (minimized) so it's always watching and injects when MX Bikes starts. Also keeps running now. Per-user, no admin. |
| `--uninstall-startup` | — | — | Stop running at login (removes the entry above). |
| `--no-filter-servers` | — | filter **on** | Turn the server filter off (reload only; leaves the browser untouched). |
| `--filter-servers` | — | *(already on)* | Force the filter on. Redundant now that it's the default; kept for clarity. |
| `--game <id>` | `mxb` \| `gpb` | `mxb` | Which title to attach to. `gpb` is GP Bikes — reload works there; the server-browser filter does not (its offsets are MX Bikes' and aren't ported). |
| `--process <name>` | exe name | `mxbikes.exe` | Inject into a process by image name. `--game` is the friendlier form of the same thing. |
| `--mods "<path>"` | folder | `Documents\PiBoSo\<title>\mods` | Watch a different mods folder. The default follows `--game`/`--process`, so GP Bikes reads GP Bikes' folder. |
| `<path>` | `.dll` path | `frostmod.dll` next to the exe | Positional: load a specific DLL build. |

### Advanced / diagnostic

| Flag | Argument | Default | What it does |
|------|----------|---------|--------------|
| `--update` | — | — | Download the latest release's `frostmod.exe` + `frostmod.dll` and install them (close MX Bikes first — the DLL is locked while it runs), then relaunch. |
| `--no-update-check` | — | check **on** | Skip the startup check that asks GitHub whether a newer FrostMod release exists. |
| `--wait <ms>` | milliseconds | `400` | Delay between spotting the game and injecting. Raise it (e.g. `--wait 2000`) if early injection ever destabilizes startup. |
| `--dump-serverlist` | — | off | Log the raw master server-list blob to `[srvlist] …` lines (for tuning filter rules). |
| `--capture-master` | — | off | RE diagnostic for the **mimic master server** — hooks the `ws2_32` exports and logs only master (UDP 54200 / resolved `mx-bikes.com` IP) traffic to `[cap]` / `[cap.hex]` / `[cap.str]` lines. Read-only. Open the online browser and/or run a local `mxbikes.exe --dedicated`, then share `frostmod.log`. |
| `--probe-mount` | — | off | RE diagnostic only — logs the content-loader's arguments (`[mount] …`). Not needed for normal use. |

## Controls

### Console (the `frostmod.exe` window)

| Key | Action |
|-----|--------|
| `R` | Reload mods |
| `D` | Dump the current server list to the log |
| `Q` / `Ctrl+C` | Quit (the game keeps running) |

### In-game (works in fullscreen)

FrostMod draws **one thing**: a small pill in the top-left corner.

| The pill shows | When |
|----------------|------|
| `FrostMod vX.Y.Z - attached` | idle — the DLL is loaded and the render hook is firing |
| `/ Reloading mods... 60%` + a progress bar | while a reload runs |
| a status line | briefly, after something worth reporting (e.g. the MXB App asking you to re-select a bike) |

| Key | Action |
|-----|--------|
| `F8` | Hide / show the pill |

`F8` is the **only** key FrostMod reads — no menu, no digits, no arrows — so it can't
shadow a game binding or fight another HUD plugin for a keystroke. Hidden means hidden:
a reload in progress won't force the pill back. Press `F8` again to bring it back.

There is no in-game menu. Model swaps, the track manager, the track switcher, direct
connect, the radar and the rider outlines are all still **compiled in** but no longer
reachable from the game — they need a `FROSTMOD_UI 1` build (the switch is at the top of
`src/frostmod.cpp`). Bike model swaps are the MXB App's job now.

The model-swap library layout is unchanged, since the engine behind it still ships: a
bike's alternative models live as folders under `mods\bikes\<Bike>\FrostMod Models\<Name>\`,
each holding a full file set (`model.edf` plus its `.hrc`/`.cfg` lineup/alignment files,
which are tuned to that mesh and swap together). `paints\` is never touched, and the model
you were using is auto-saved back into the library as `Original` on the first swap.

Reloading (`R` in the console, or the MXB App) makes new tracks, bikes, and skins appear
immediately — no restart, no loading screen.

## Files

All of these live **next to the binaries** (the folder containing `frostmod.exe`
and `frostmod.dll`), so the launcher and the injected DLL always agree on them.

| File | Purpose |
|------|---------|
| `frostmod.log` | The live log, streamed into the console. Falls back to `%TEMP%\frostmod.log` if that folder is read-only. |
| `frostmod_serverfilter.yaml` | Your server-filter rules. Auto-created on first run with a documented header, and auto-upgraded when the shipped defaults change (the old file is backed up to `.bak` first). Edit it and reload (`R`) to apply — see the comments inside, or the rule types below. |
| `frostmod_filter.flag`, `frostmod_dumplist.flag`, `frostmod_probe.flag` | Internal on/off markers the launcher writes so the DLL knows which optional hooks to install. You don't edit these; the flags above manage them. |

### Server-filter rules (`frostmod_serverfilter.yaml`)

A small YAML file. A server is hidden if its name **contains any `names` entry** or
**matches any `regex`** (both case-insensitive). Edit and reload (`R`) to apply.

```yaml
hideUnjoinable: false   # ping '---' - unreliable at list time, keep off
hideEmpty: false        # hide 0-player servers
hideLocked: false       # hide password-locked servers
maxPerIP: 0             # 0 = off; else hide servers past N from one IP per refresh
names:                  # case-insensitive substrings
  - che4ts
  - kaizo
regex:                  # ECMAScript; single-quote to keep backslashes literal
  - '(che[a4]ts|k[a4][il1]z[o0]|\.pr0\b)'
```

The shipped defaults target cheat/ad "ghost" spam only (so legit servers stay) — add
your own `names`/`regex` entries to hide more (hosting ads, Discord/URL self-promo, …).
Every server is logged as `[srv] … HIDE` (hidden) or `keep` (shown) so you can see
exactly what the rules did. `hideUnjoinable` shows `---` for *every* server at
list-build time, so leaving it off is correct. The file is versioned: when the shipped
defaults change it's rewritten and your old copy is kept as `.bak`.

## Run as a plugin instead

Drop `frostmod.dll` into MX Bikes' `plugins` folder and the game loads it at
startup — no launcher needed. You lose the console (mods list / log stream / `R`), so
reload comes from the MXB App, but the pill and the filter still work. Details:
[PLUGIN.md](PLUGIN.md).

## Notes

- **Offline use only.** Don't inject during online sessions.
- If the console shows no output from the DLL, the folder may not be writable, or
  the game isn't calling the present hook yet — check `frostmod.log`.
