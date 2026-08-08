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
frostserver.exe --track "Red Bud 2024" [--port 54210] [--name "My Server"]
```

`--track` seeds the "current" map (the real plugin learns it from the game),
`--port`/`--name` override the config for a one-off run, and `--help` lists them.
Ctrl+C stops it cleanly.

### Diagnostics

FrostServer normally logs only what an admin needs. When verifying it against a
**new game build**, drop an empty `frostserver_probe.flag` next to
`frostserver.log` to also dump the raw ASCII fields of every race callback (this
is what confirms the track name really is at `+0x68` on that build). It's off by
default because it writes on every race event.

## Config — `frostserver.yaml`

Written with documented defaults next to the plugin on first run. Edit it, then
restart the server (or the `.exe`) to pick up changes.

```yaml
# frostserver v2
port: 54210          # TCP port for the HTTP API; clients reach <server-ip>:<port>
name: 'My MX Server' # optional friendly name reported in /frostserver/info
gamePort: 54200      # the MX Bikes port THIS server runs on

# For each track this server runs, its mxb-mods.com download page.
# The KEY is the track name EXACTLY as FrostServer logs it — watch frostserver.log
# for a line like:  [race] current track: '<name>'  — and copy that name here.
# Quote names that contain spaces or ':'.
maps:
  'Red Bud 2024': https://mxb-mods.com/red-bud-2024/
  'Some MX Track': https://mxb-mods.com/some-mx-track/
```

Edits are picked up **within one request** — no restart. (Changing `port` is the
one exception: the socket is bound at startup, so that needs a restart, and the
log says so when it notices.)

- **`port`** — the API port. Clients reach the server at `http://<server-ip>:<port>`.
  Make sure it's open in the server's firewall / forwarded, like the game port.
- **`name`** — cosmetic; echoed back in `/frostserver/info`.
- **`gamePort`** — the MX Bikes port this dedicated server listens on. Clients use
  it to confirm the FrostServer they reached belongs to the server row they picked.
  It matters when **one machine hosts several servers**: only one of them can own
  the FrostServer port, so without this a client could show server A's map next to
  server B's name. Give each server a different `port` and set each one's `gamePort`
  to its own game port.
- **`maps`** — the track-name → link table. The key must match the track name the
  server reports; FrostServer logs that exact string every time a race starts, so
  the reliable way to fill this in is to run the track once and copy the name from
  `frostserver.log`.

Track names are matched **loosely** — case-insensitive, ignoring spaces and
punctuation — so `Red Bud 2024`, `red bud 2024` and `RedBud2024` are all the same
key. You don't have to reproduce the game's punctuation exactly.

## HTTP API (the contract)

All responses are JSON (except `/health`), `Access-Control-Allow-Origin: *`,
`Connection: close`. Only `GET` is supported.

### `GET /frostserver/info`

The current map and its link. `currentMap` is `null` when no race/track is
active.

```jsonc
{
  "frostserver": "0.9.10",           // FrostServer version
  "protocol": 1,                    // contract version (absent on < 0.9.10)
  "gamePort": 54200,                // the MX Bikes port this server runs on
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
{ "frostserver": "0.9.10", "protocol": 1, "gamePort": 54200, "name": "My MX Server", "currentMap": null }
```

`protocol` is bumped whenever the shape changes in a way a client must notice.
A client that sees no `protocol` field is talking to a pre-0.9.10 FrostServer;
everything above except `protocol`/`gamePort` behaves identically, so old servers
keep working.

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

## The download flow (client side)

The client half is built and compiled in, but its in-game browser is only reachable
in a `FROSTMOD_UI 1` build (see [`src/frostmod.cpp`](../src/frostmod.cpp)); the
shipped build is UI-free. In such a build, **Server maps** lists the servers the
game knows about, with the map each one is running and whether you already have it:

```
  SERVER                     PLAYERS  MAP                    STATUS
> Frosty's Practice Server   4/24     Red Bud 2024           GET IT
  Some Other Server          0/32     Whiplash               have it
  A Third Server             8/40                            no FrostServer
```

Press **Enter** on a `GET IT` row and the track downloads and appears in-game
without a restart. End to end:

1. FrostMod reads the server's **IP:port** straight out of the game's own server
   list (see *RE provenance* below) and calls
   `GET http://<server-ip>:54210/frostserver/info`.
2. It compares `currentMap.name` against your installed tracks. If you have it,
   the row says so and Enter does nothing.
3. Otherwise it hands `currentMap.link` to the MXB App as
   `mxbapp://download?url=<link>`.
4. **MXB App** downloads + extracts the track into `mods/tracks`, then signals
   FrostMod over the `Local\FrostModReload` event.
5. FrostMod **live-reloads** content; the track is there, and you join.

If the MXB App isn't installed the `mxbapp://` scheme isn't registered, so
FrostMod falls back to opening the mxb-mods page in your browser and says so —
you download it by hand and press **`R`** in the `frostmod.exe` console.

Queries run on a worker thread with short timeouts and are cached, so a server
with no FrostServer costs one fast failure, not a stutter. Only servers currently
on screen are asked.

### Where the client gets a server's IP (RE provenance)

The game's master-list records carry the address, and the browser's rows are a
verbatim copy of those records — so no new hook was needed. Confirmed statically
against the unpacked `mxbikes.exe` (image base `0x140000000`):

- **The list array** is `[0x5985D8]` (pointer), count `[0x5985E0]`, stride `0x1D8`,
  filled by the master-list parser at `0x2A6B40`–`0x2A6D96`.
- **A record starts with two 19-byte address blobs** — public at `+0x00`, LAN at
  `+0x13` — with the server name at `+0x26`.
- **Blob format**, from the blob→sockaddr converter `0x2856E0`: `[0]` family
  (`0` = IPv4, `1` = IPv6), then the address bytes, then the port big-endian
  (`(b[5] << 8) | b[6]` for IPv4).
- Cross-checks: `0x2A67F0` memcpy's whole `0x1D8` records into the browser's stack
  buffer, so the long-known browser offsets (name `+0x86`, players `+0xCC`, ping
  `+0xDC`) are exactly these record offsets plus `0x60`; and the browser's JOIN
  path `0x0AA348`–`0x0AA3B8` copies precisely 8+8+2+1 = 19 bytes out of `+0x00`
  and `+0x13`.

FrostMod reads this array read-only and validates it (sane count, printable first
name) before believing it, falling back to the rows the server-filter hook
captured if the read looks wrong on a different game build.

## Why `RaceEvent` is reliable on a dedicated server (RE provenance)

Confirmed against the decompiled `mxbikes.exe` (image base `0x140000000`), so we
don't need a live server to trust the mechanism:

- **Plugins load on the dedicated build.** The `.dlo` loader (`0x14012A4F0`,
  enumerated at `0x14012A9B7`) resolves exports by name and appends each plugin
  struct (stride `0x118`) to a global list — **base `0x565CC0`, count
  `0x565CB8`**. `RaceEvent` is stored at `plugin+0x68`.
- **The fan-out is unconditional.** Dispatcher `0x14012AE70` loops that list and
  calls `[plugin+0x68](data, size)` for every loaded plugin — no client/dedicated
  gate. It is invoked from the shared engine command bus (case `0x140127EA1`), the
  same bus that runs the dedicated path's content-scan / track-registry work, so
  it executes headless.
- **The `-dedicated` flag (`0x565E64`) gates only startup init**, never the
  plugin/race dispatch.
- **Track field:** `SPluginsRaceEvent_t.m_szTrackName` at `+0x68`
  (event `m_iType == 6`), struct size `0xD0` (208).

One link — the server session object's vtable call at `0x14028FF81` that posts the
race-event command onto that bus — is assigned at runtime and can't be read
statically, but it lives in the server/session module and passes the RaceEvent
type, feeding the proven dispatcher. FrostServer's diagnostic callbacks
(`EventInit`/`RaceSession`/`RaceAddEntry` + the raw ASCII-field dump) confirm this
last inch on the first real dedicated-server run.

## Build

Built by the FrostMod CMake project alongside the client:

```
cmake -B build -A x64
cmake --build build --config Release
```

Outputs `frostserver.dll` + `frostserver.dlo` (the plugin) and `frostserver.exe`
(standalone tester) to `build\bin`.
