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
hear each other, just flat. We set it from `RaceEvent`:

```
mxbikes|<server/event name>|<track name>
```

Both halves matter. The server alone would keep riders grouped through a track change; the
track alone would group strangers on unrelated servers running the same map, and put their
voices at positions on a track they aren't sharing.

**We publish nothing until `RaceEvent` has given us a context.** An empty context is still
a context, so every rider carrying one would be grouped together — the opposite of what
this feature is for. No context, no positional data, and everyone is simply heard flat.

## What is unverified

- **That `RaceEvent` fires on the client.** It is confirmed on the *dedicated server* (see
  [FrostServer](FROSTSERVER.md) for the RE), and the callback is not documented as
  server-only. If it turns out not to fire client-side, the fail-closed guard above means
  proximity silently never engages — check `frostmod.log` for `[mumble] context:`.
- **Everything downstream of the block.** The struct layout is guarded by a
  `static_assert`, and the vector maths is checked against the radar, but whether Mumble
  actually picks us up has to be seen once on Windows with Mumble running.

## Files

| Path | Purpose |
|---|---|
| `src/mumblelink.{h,cpp}` | the Link block: shared memory, struct, per-frame write |
| `MumbleLink` (shared memory) | the interface itself; Mumble's own name for it |
| `frostmod_radar.cfg` | `mumble=0/1` alongside the radar's settings |
