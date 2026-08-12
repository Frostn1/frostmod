# Proximity voice via Mumble

FrostMod publishes your position to [Mumble](https://www.mumble.info) through its **Link**
interface, so riders on the same server hear each other from where they actually are —
loud alongside you into a corner, faint once they're a straight away.

Mumble does the voice. We only say where we are.

## Why this is small

The Link interface is a named shared-memory block that Mumble polls. We write our **own**
avatar position and facing each frame; Mumble's server distributes that between clients and
does the attenuation and panning itself.

That last part is the whole design. We never have to answer *"which voice belongs to which
rider on track"* — the question that needs an identity join between the plugin's race
numbers and a rider's GUID, which we cannot make reliably. Every other rider's position
reaches Mumble from their own copy of this code.

## Setup

1. Install Mumble and connect to a server everyone on the grid is using.
2. In Mumble: **Settings → Audio Output → Positional Audio**, enabled.
3. Run MX Bikes with FrostMod. Proximity voice is on by default; toggle it with **`6`** in
   the F8 menu. The state persists in `frostmod_radar.cfg`.

Mumble can be started before or after the game — whoever creates the shared block first
wins and the other attaches.

## Coordinates

MX Bikes' world axes are **already** Mumble's, which is why nothing is converted:

| | Mumble expects | MX Bikes gives |
|---|---|---|
| X | right | right |
| Y | up | up |
| Z | forward | forward |
| units | metres | metres |
| handedness | left | left |

The facing vector comes from the SDK's yaw (degrees from north). For heading θ:

```
front = (sin θ, 0, cos θ)      top = (0, 1, 0)
```

That isn't asserted, it's derived from the radar, which was verified separately: a rider
dead ahead satisfies the radar's `rx = 0, ry = d`, and solving gives a world delta of
`(d·sin θ, 0, d·cos θ)`. The two are checked against each other at every 30° of heading —
if the radar points at someone, Mumble hears them in the same direction by construction.

**Camera = rider, deliberately.** Mumble listens from `fCameraPosition`. MX Bikes' chase
cameras sit behind and above the bike, and hearing from there would put every voice several
metres from where the rider is.

## The context, and why it fails closed

Mumble strips positional data between two users whose `context` bytes differ — they still
hear each other, just flat. We set it from **`EventInit`**, whose `SPluginsBikeEvent_t`
carries the server name, the track ID *and* our own GUID:

```
mxbikes|<server name>|<track id>
```

Both halves matter. The server alone would keep riders grouped through a track change; the
track alone would group strangers on unrelated servers running the same map, and put their
voices at positions on a track they aren't sharing.

**We publish nothing until a context exists.** An empty context is still a context, so
every rider carrying one would be grouped together — the opposite of what this feature is
for. No context, no positional data, and everyone is simply heard flat.

`RaceEvent` is kept for the track half alone — a server on a rotation changes track without
opening a new event — and the server name from `EventInit` survives it.

### Why not `RaceEvent` for the whole thing

It was the first attempt, and it **does not reach the client**: it is a *race*-session
callback, confirmed firing on the dedicated server (see [FrostServer](FROSTSERVER.md)) and
observed never firing in a client session. The fail-closed guard did its job — proximity
simply never engaged — but the failure was silent, because the handler logged only *after*
its size check, so "never called" and "called with an unexpected payload" looked identical.

Every plugin callback we implement now logs once on first arrival with the size the game
passed (`[cb] <name> fired (dataSize=N)`), so that class of failure costs one log line
rather than a guess.

## What is unverified

- **Everything downstream of the block.** The struct layout is guarded by a
  `static_assert`, and the vector maths is checked against the radar, but whether Mumble
  actually picks us up has to be seen once on Windows with Mumble running.
- **`SPluginsBikeEvent_t`'s layout.** We read `m_szServerName`, `m_szTrackID` and
  `m_szGUID`, which sit at the *end* of the struct, so every preceding field must match the
  SDK exactly — a wrong size anywhere ahead of them yields plausible garbage rather than an
  error. The size check rejects a payload smaller than expected, and the log line prints
  what actually arrived.

## Files

| Path | Purpose |
|---|---|
| `src/mumblelink.{h,cpp}` | the Link block: shared memory, struct, per-frame write |
| `MumbleLink` (shared memory) | the interface itself; Mumble's own name for it |
| `frostmod_radar.cfg` | `mumble=0/1` alongside the radar's settings |
