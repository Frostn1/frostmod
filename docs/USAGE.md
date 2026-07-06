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
| `--process <name>` | exe name | `mxbikes.exe` | Inject into a different process (e.g. `--process gpbikes.exe`). |
| `--mods "<path>"` | folder | `Documents\PiBoSo\MX Bikes\mods` | Watch a different mods folder (only affects the console's mod list). |
| `<path>` | `.dll` path | `frostmod.dll` next to the exe | Positional: load a specific DLL build. |

### Advanced / diagnostic

| Flag | Argument | Default | What it does |
|------|----------|---------|--------------|
| `--wait <ms>` | milliseconds | `400` | Delay between spotting the game and injecting. Raise it (e.g. `--wait 2000`) if early injection ever destabilizes startup. |
| `--dump-serverlist` | — | off | Log the raw master server-list blob to `[srvlist] …` lines (for tuning filter rules). |
| `--probe-mount` | — | off | RE diagnostic only — logs the content-loader's arguments (`[mount] …`). Not needed for normal use. |

## Controls

### Console (the `frostmod.exe` window)

| Key | Action |
|-----|--------|
| `R` | Reload mods |
| `D` | Dump the current server list to the log |
| `Q` / `Ctrl+C` | Quit (the game keeps running) |

### In-game (works in fullscreen)

| Key | Action |
|-----|--------|
| `F8` | Reload mods |
| `F7` | Show/hide the overlay |
| `F9` | Dump the current server list to the log |

Reloading (any of `R` / `F8`) rescans your content from disk and makes new
tracks, bikes, and skins appear immediately — no restart, no loading screen.

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
startup — no launcher needed. You lose the console (mods list / log stream / `R`),
but reload (`F8`), the overlay, and the filter all still work. Details:
[PLUGIN.md](PLUGIN.md).

## Notes

- **Offline use only.** Don't inject during online sessions.
- If the console shows no output from the DLL, the folder may not be writable, or
  the game isn't calling the present hook yet — check `frostmod.log`.
