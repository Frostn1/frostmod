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
- enables **live mod reload** (press **`R`** / **`F8`** after dropping in a `.pkz`),
- enables the **server-browser spam filter** (hides cheat/ad "ghost" servers), and
- shows the **in-game overlay**.

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
| `--game <id>` | `mxb` \| `gpb` \| `krp` | `mxb` | Which title to attach to. On `gpb` (GP Bikes) reload is **off** — its offsets are derived but unconfirmed and crashed the game (see `--unsafe-reload`). On `krp` (Kart Racing Pro) reload is off because its offsets are not derived at all yet: the plugin loads and the overlay, radar and session block work, and one capture run closes the gap (`tasks/kart-racing-pro-port.md`). The server-browser filter is MX Bikes only on both. |
| `--process <name>` | exe name | `mxbikes.exe` | Inject into a process by image name (`gpbikes.exe`, `kart.exe`). `--game` is the friendlier form of the same thing, and both now set the mods folder and the plugin identity alike. |
| `--mods "<path>"` | folder | `Documents\PiBoSo\<title>\mods` | Watch a different mods folder. The default follows `--game`/`--process`, so GP Bikes reads GP Bikes' folder and Kart Racing Pro reads `Kart Racing Pro\mods`. |
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
| `--probe-overjump` | — | off | Logs the session settings block the engine is handed at every session start (`[overjump] …` plus a hex dump of the 0x21C bytes), and decodes whether the **overjump crash** is on. Run testing, host with the option unchecked, and join a server that sets `overjump_crash = 0` — the three lines together say whether the client honours the setting it was given. |
| `--force-overjump-off` | — | off | Implies `--probe-overjump`, and additionally **clears** the crash flag as the session starts. Offline and testing only: on a server that wants the crash this is a client-side cheat. |
| `--unsafe-reload` | — | off | Run the reload on a title whose step table hasn't been confirmed — today that means **GP Bikes, where it has crashed the game**. Only worth arming to collect a log: every step is written to `frostmod.log` *before* it runs, so the last `[reload] step …` line names the loader that faults. Send that log in. |
| `--unsafe-reload-from=<n>` | step number | `1` | As `--unsafe-reload` (which it implies), but skips straight to step `n`. On GP Bikes the reload dies on **step 1 (`tracks`, `0x139A0`) every time**, so `--unsafe-reload-from=2` asks the one question that log can't answer: if steps 2–13 then complete, that single loader is unsafe to re-run; if step 2 dies the same way, replaying *any* loader from this call site is what kills the game. |

## Controls

### Console (the `frostmod.exe` window)

| Key | Action |
|-----|--------|
| `R` | Reload mods |
| `D` | Dump the current server list to the log |
| `Q` / `Ctrl+C` | Quit (the game keeps running) |

### In-game (works in fullscreen)

Press **`F8`** to open the FrostMod **menu** (top-left). While it's open, press an
item's number; `Esc` or `F8` closes it. Everything lives here instead of a separate
F-key per feature.

| In the menu | Action |
|-------------|--------|
| `1` | Reload mods (rescans content from disk, with a progress bar) |
| `2` | Toggle the corner hint overlay |
| `3` | Bike model swap — swap a bike's model (whole file set) for another (see below) |
| `4` | Radar — heading-up disc of riders around you (`PageUp`/`PageDown` = range) |
| `5` | Rider outlines — on-screen box around each rider |
| `6` | Overlay size — steps 75 → 200 %; the menu stays open so you can see it change |
| `7` | Hide overlay — everything FrostMod draws, for recording (see below) |

The overlay sizes itself to your screen, so it takes up the same share of a 4K display
as it does of a 1080p one. Row `6` is on top of that, for when you want it bigger (or
smaller) than that. Radar blips and outlines are colored by lap status: **white** = same
lap as you, **red** = a rider lapping you (a lap ahead), **blue** = a rider you are
lapping (backmarker). The toggles, the range and the overlay size persist across
restarts, in `frostmod_radar.cfg` next to `frostmod.log`.

### Hide overlay (menu `7`)

`F7` hides **everything FrostMod draws** — the corner pill, the radar, the rider outlines.
It is stronger than the overlay toggle at menu `2`, which only drops the corner hint. Press
it again to bring it all back; it is also on the menu at F8 → `7`.

The key keeps working while the overlay is hidden, and F8 brings the overlay back before
opening anything — so you cannot get stuck looking at a UI you can't see. It is not
remembered between sessions: it is something you turn on to record.

**The game's own HUD and the replay control bar are not FrostMod's to hide.** Turn the HUD
off in MX Bikes' own options; the replay bar belongs to the game's interface, and the only
thing FrostMod could do to it is destroy the window, which would take the transport
controls with it. So it leaves both alone.

### Bike model swap (menu `3`)

In MX Bikes a bike lives at `mods\bikes\<Bike>\` as loose files. A "model" is the whole
top-level file set — `model.edf` (the mesh) **and** its `.hrc`/`.cfg` lineup/alignment files,
which are tuned to that mesh and swap together. Only `paints\` (universal liveries) stays put.

1. Add each alternative model as a **folder** in the bike's library:
   `mods\bikes\<Bike>\FrostMod Models\<Name>\`, containing that model's full file set
   (`model.edf` + its `.hrc`/`.cfg`). Create the `FrostMod Models` folder if it isn't there.
2. F8 → `3`, pick the **bike**, then pick a **variant**. Enter swaps its files in and reloads
   — the new model appears without a restart.
3. The model you were using is auto-saved back into the library (as `Original` on the first
   swap), so you can always pick it again to revert. `paints\` is never touched.

If the model files are locked (you're currently riding that bike), the swap rolls back with
the bike left intact and asks you to exit the bike first.

Reloading (menu `1`, or `R` in the console) makes new tracks, bikes, and skins appear
immediately — no restart, no loading screen.

## Files

All of these live **next to the binaries** (the folder containing `frostmod.exe`
and `frostmod.dll`), so the launcher and the injected DLL always agree on them.

| File | Purpose |
|------|---------|
| `frostmod.log` | The live log, streamed into the console. Falls back to `%TEMP%\frostmod.log` if that folder is read-only. |
| `frostmod_serverfilter.yaml` | Your server-filter rules. Auto-created on first run with a documented header, and auto-upgraded when the shipped defaults change (the old file is backed up to `.bak` first). Edit it and reload (`R`) to apply — see the comments inside, or the rule types below. |
| `frostmod_filter.flag`, `frostmod_dumplist.flag`, `frostmod_probe.flag`, `frostmod_unsafe_reload.flag` | Internal on/off markers the launcher writes so the DLL knows which optional hooks to install. You don't edit these; the flags above manage them. |
| `frostmod_cmd.json` | One command from [MXB App](https://github.com/Frostn1/mxb-app) — `{"verb":…}`, today `reload_mods` or `refresh_bike_model`. The app writes it, the DLL acts on it and remembers it, and nothing deletes it. `%TEMP%\frostmod_cmd.json` is read as well, because that is where MXB App on Windows writes. |

### Running inside a Wine prefix (Linux and macOS)

MXB App starts `frostmod.exe` inside the same prefix as the game — Proton's on
Linux, your CrossOver/Whisky/Wine bottle on macOS — which is the only place it can
do its work: the game it injects into is a Windows process in that prefix, and so
is FrostMod. Nothing here needs a flag for it, but two things follow from where the
app is standing:

- **It signals with a file, not an event.** MXB App is a native Linux or macOS
  process outside the prefix, so it cannot pulse `Local\FrostModReload`. Reload from
  the app arrives as `reload_mods` in `frostmod_cmd.json` instead, which the DLL
  picks up within about a fifth of a second. `R` in the console and `F8` in game are
  unaffected — those are already inside.
- **v0.13.0 or newer.** Older builds only read that file when an event told them
  to, so an older FrostMod in a prefix reloads on `F8` and ignores the app.

On macOS the app leaves FrostMod in its own data folder rather than copying it into
the bottle, and reaches it as `Z:\Users\…` — so a bottle whose `Z:` drive has been
removed can't be driven from the app at all. Add one mapped to `/` in the wrapper's
drive settings.

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

Drop `frostmod.dlo` into the game's `plugins` folder and the game loads it at
startup — no launcher needed (`frostmod.exe --game <id> --install-plugin` does it for you).
You lose the console (mods list / log stream / `R`), but reload (`F8`), the overlay, and the
filter all still work, as far as they are ported for that title. Details:
[PLUGIN.md](PLUGIN.md).

## Notes

- **Offline use only.** Don't inject during online sessions.
- If the console shows no output from the DLL, the folder may not be writable, or
  the game isn't calling the present hook yet — check `frostmod.log`.
