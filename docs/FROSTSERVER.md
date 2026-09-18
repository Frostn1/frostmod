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

The HTTP API is **read-only** and serves nothing but the map names and links you
put in its config. It never touches files outside its own folder.

FrostServer does one thing beyond that API, and it is on by default: it keeps
riders on the far side of the track from going invisible on each other's screens.
That part **does** read and hook game memory, and it **does** change what goes out
on the wire. See [Keeping distant riders visible](#keeping-distant-riders-visible)
for exactly what it changes and how to turn it off. Nothing else in FrostServer
touches the game.

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

fair_send: true      # keep distant riders from going invisible; see below
fair_send_ticks: 9   # how far into a rider's silence to act, in 30ms ticks

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
- **`fair_send`** / **`fair_send_ticks`** — see
  [Keeping distant riders visible](#keeping-distant-riders-visible). On by default.
- **`maps`** — the track-name → link table. The key must match the track name the
  server reports; FrostServer logs that exact string every time a race starts, so
  the reliable way to fill this in is to run the track once and copy the name from
  `frostserver.log`.

## Keeping distant riders visible

On a full gate, riders cannot see some of the people they are racing. No bike, no
rider, no name. Those riders are still solid and can still land on someone who
cannot see them, and everyone else on the server sees them perfectly well. It is
worst on whoever is furthest away, which in a race usually means the leader.

The cause is on the server, not on the players' machines. A full gate does not fit
in one update packet, so the server sends each player the riders nearest them first
and drops the rest off the end. The riders furthest away get dropped over and over.
After about a third of a second of that, a player's game stops drawing them.

FrostServer gives a rider who is close to vanishing a slot in the next packet, ahead
of somebody nearer who has updates to spare. **No extra packets are sent and the
packets do not get bigger.** The same bytes go to the same riders at the same rate,
and only the choice of who is in them changes.

### Using it

Nothing to do. Drop `frostserver.dlo` in the `plugins` folder as above and it is on.

**Only the server needs it.** Players install nothing, change nothing, and do not
need FrostMod for this to help them. The packets reaching them are simply better.

Watch `frostserver.log` for the two lines that matter:

```
[fairsend] on: a rider silent for 9 ticks goes to the front of the next packet (their game gives up at 10)
[fairsend] this server is starving riders on a full gate; keeping them drawn from here on
```

The first says the hook is installed. The second appears the first time it actually
rescues somebody, and it is the useful one: it tells you this server *was* dropping
riders off people's screens. A server that never prints it never had the problem,
which on a small grid is normal.

If it cannot install, it says why and leaves the game alone. The usual reason is a
game update moving the code it hooks.

### Settings

| key | default | what it does |
|---|---|---|
| `fair_send` | `true` | `false` leaves the server exactly as PiBoSo ships it |
| `fair_send_ticks` | `9` | how many 30 ms ticks into a rider's silence to act |

`fair_send_ticks` is clamped to 1–9. A player's game gives up at 10 ticks, so 9 is
the last moment that still works, and lower values act earlier at the cost of
bumping nearby riders more often. There is no reason to change it.

Existing configs keep working without these keys and get the defaults. The config
version was deliberately **not** bumped, because a bump rewrites the file and would
put your `maps` list in a `.bak`.

### Limits, so nobody is surprised

- On a grid big enough that one packet cannot hold even the starving riders, this
  rotates who is starved rather than ending it. Still better than the stock order,
  which never rotates.
- A promoted rider takes a slot a nearer rider would have had, so that rider waits
  one tick. Riders close to you are sent every tick and have ticks to spare, so
  nothing shows on screen.
- **MX Bikes only.** The addresses are that title's. On GP Bikes or Kart Racing Pro
  it refuses to install and says so.
- There is a client-side half in FrostMod for players on servers that do not run
  FrostServer. It widens how long their game will wait before giving up on a rider.
  That one makes a distant rider visible but smoothed. This one is the better fix,
  because the rider's position stays accurate.

## Styled announcements

Every message a dedicated server sends in chat arrives in the same colour on every
player's screen, in the game's own font. That is decided by the receiving game, not by
the server, so there is nothing a server can put in a chat message that changes it.

So FrostServer does not send these as chat. It publishes them, FrostMod fetches them from
the server the player is already on, and FrostMod draws them itself — just above where the
game stacks its chat, in the colour you picked, with the animation you picked.

**What each kind of player sees.** A player running FrostMod sees your styled line. A
player without it sees nothing extra and loses nothing: ordinary server chat is untouched
and still arrives as it always did. So use ordinary chat for anything everyone must read,
and announcements for the things that are nicer in colour — the welcome, the rules, the
Discord, a warning before a session change.

### Writing them

Add an `announce:` list to `frostserver.yaml`:

```yaml
announce:
  - text: ':flag: Welcome to Frost MX - clean racing, no cutting'
    color: FF3B00
    style: pulse
    seconds: 8
    every: 300

  - text: ':clock: Qualifying starts on the hour'
    color: 7FD4FF
    style: rainbow
    seconds: 6

announce_session: true    # also say a line whenever the track changes
announce_token: ''        # see "Live announcements" below
```

| key | what it does |
|---|---|
| `text` | what to say. Cut at 160 characters. |
| `color` | `RRGGBB` hex. Default white. |
| `style` | `none`, `pulse` (brightness breathes), `fade` (alpha breathes) or `rainbow` (the colour runs along the line). Default `none`. |
| `seconds` | how long it stays on screen, 1–30. Default 8. |
| `every` | repeat every N seconds. Leave it out to say it once, when the server starts. |

Every line fades in and out whatever its style, so nothing pops on or off.

### Icons

These names draw a small symbol in the message's colour:

`:flag:` `:warn:` `:star:` `:clock:` `:trophy:` `:check:` `:cross:` `:bolt:` `:skull:`
`:heart:`

They are single-colour symbols, tinted to match the message — not colour emoji. A name
that is not on the list is left exactly as you typed it, so `2:30` stays `2:30`.

### Live announcements

Set `announce_token` to a password and you can send one right now, from anywhere that can
reach the server:

```
curl -X POST http://<server-ip>:54210/frostserver/announce \
  -H "Authorization: Bearer <your token>" \
  -H "Content-Type: application/json" \
  -d '{"text":":warn: Red flag - stop on track","color":"FF0000","style":"pulse","seconds":10}'
```

Left empty, the endpoint does not exist and returns `404` — which is also what it returns
to anyone probing for it. **It is a password on an open port; treat it like one.** Anyone
who has it can put words on your riders' screens.

### What players control

A rider can turn the overlay off entirely (FrostMod's F8 menu, "Server announcements"), and
that choice sticks. The position and FrostServer's API port are `servermsgy` and
`servermsgport` in `frostmod_radar.cfg`. If you change `port:` in `frostserver.yaml`, riders
must set `servermsgport` to match — which is a good reason to leave the port alone.

## HTTP API (the contract)

All responses are JSON (except `/health`), `Access-Control-Allow-Origin: *`,
`Connection: close`. Everything is `GET` except the one authenticated `POST` below.

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

### `GET /frostserver/messages?since=<seq>`

Announcements newer than `since` (omit it for everything the ring holds). `seq` at the top
level is the newest sequence the server has, so a client that has fallen behind can tell by
how much. The ring keeps the last 64; a client away longer than that misses the difference,
which is intended — an announcement is news, not a mailbox.

```json
{
  "seq": 12,
  "messages": [
    { "seq": 11, "text": ":flag: Welcome", "color": "FF3B00", "style": "pulse", "seconds": 8.00 },
    { "seq": 12, "text": "Qualifying in 5", "color": "7FD4FF", "style": "none", "seconds": 6.00 }
  ]
}
```

### `POST /frostserver/announce`

Publish one announcement now. Requires `Authorization: Bearer <announce_token>`; returns
`404` when no token is configured, `401` on a wrong one. The body is one message object —
`text` required, `color`, `style` and `seconds` optional and clamped the same way a
configured line is. Body cap 4 KB.

```json
{ "seq": 13 }
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
