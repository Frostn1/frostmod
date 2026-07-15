# FrostServer — dedicated-server map/link API

**FrostServer** is FrostMod's server-side companion. It runs on an **MX Bikes
dedicated server** and exposes, over a tiny HTTP API, *which track the server is
currently running* and *where to download it* (a [mxb-mods.com](https://mxb-mods.com)
link the admin configures).

A FrostMod client can then, for a server whose track you don't have, fetch that
link, hand it to the **[MXB App](https://github.com/Frostn1/mxb-app)** to
download + extract, and live-reload the mods folder — so you go from "missing
track" to "on the server" without leaving the game. FrostServer is the first
piece of that flow; it defines the contract the client and MXB App consume.

FrostServer is **read-only** and serves nothing but the map names and links you
put in its config. It opens no game memory, changes no gameplay, and never
touches files outside its own folder.

## How it loads

FrostServer is a **PiBoSo plugin**: `frostserver.dlo`. Drop it in the dedicated
server's `plugins` folder (next to the server executable) and the server loads
it at startup. It learns the running track through the sanctioned `RaceEvent()`
plugin callback — no memory reverse-engineering.

> It is a *separate* artifact from `frostmod.dlo`. The client plugin belongs on
> players' machines; `frostserver.dlo` belongs on the dedicated server. Don't
> mix them.

There is also a standalone **`frostserver.exe`** that serves the same API with
no game attached — for testing the client / MXB App flow on a dev box:

```
frostserver.exe --track "Red Bud 2024"
```

## Config — `frostserver.yaml`

Written with documented defaults next to the plugin on first run. Edit it, then
restart the server (or the `.exe`) to pick up changes.

```yaml
# frostserver v1
port: 54210          # TCP port for the HTTP API; clients reach <server-ip>:<port>
name: 'My MX Server' # optional friendly name reported in /frostserver/info

# For each track this server runs, its mxb-mods.com download page.
# The KEY is the track name EXACTLY as FrostServer logs it — watch frostserver.log
# for a line like:  [race] current track: '<name>'  — and copy that name here.
# Quote names that contain spaces or ':'.
maps:
  'Red Bud 2024': https://mxb-mods.com/red-bud-2024/
  'Some MX Track': https://mxb-mods.com/some-mx-track/
```

- **`port`** — the API port. Clients reach the server at `http://<server-ip>:<port>`.
  Make sure it's open in the server's firewall / forwarded, like the game port.
- **`name`** — cosmetic; echoed back in `/frostserver/info`.
- **`maps`** — the track-name → link table. The key must match the track name the
  server reports; FrostServer logs that exact string every time a race starts, so
  the reliable way to fill this in is to run the track once and copy the name from
  `frostserver.log`.

## HTTP API (the contract)

All responses are JSON (except `/health`), `Access-Control-Allow-Origin: *`,
`Connection: close`. Only `GET` is supported.

### `GET /frostserver/info`

The current map and its link. `currentMap` is `null` when no race/track is
active.

```jsonc
{
  "frostserver": "0.9.3",           // FrostServer version
  "name": "My MX Server",           // configured server name ("" if unset)
  "currentMap": {
    "name": "Red Bud 2024",         // track name as the server reports it
    "link": "https://mxb-mods.com/red-bud-2024/",
    "haveLink": true                // false + "link": null if no config entry matches
  }
}
```

When idle:

```json
{ "frostserver": "0.9.3", "name": "My MX Server", "currentMap": null }
```

### `GET /frostserver/maps`

The full configured table — lets a client resolve *any* of the server's tracks,
not just the current one (e.g. to pre-download the rotation).

```json
{
  "maps": [
    { "name": "Red Bud 2024", "link": "https://mxb-mods.com/red-bud-2024/" },
    { "name": "Some MX Track", "link": "https://mxb-mods.com/some-mx-track/" }
  ]
}
```

### `GET /health`

`200 OK`, body `ok`. Liveness probe.

## How it fits the download flow

1. **Client** (FrostMod) sees a server running a track you don't have and calls
   `GET http://<server-ip>:<port>/frostserver/info`.
2. It reads `currentMap.link` and hands it to the MXB App via the `mxbapp://`
   deep link (`mxbapp://download?url=<link>`).
3. **MXB App** downloads + extracts the track into `mods/tracks`, then signals
   FrostMod (the existing `Local\FrostModReload` handshake) to **live-reload**.
4. The track appears in-game with no restart; you join the server.

Steps 2–4 are separate work items (MXB App deep link; client button + the
per-row server-IP RE). FrostServer (step 1) is the contract they build against.

## Build

Built by the FrostMod CMake project alongside the client:

```
cmake -B build -A x64
cmake --build build --config Release
```

Outputs `frostserver.dll` + `frostserver.dlo` (the plugin) and `frostserver.exe`
(standalone tester) to `build\bin`.
