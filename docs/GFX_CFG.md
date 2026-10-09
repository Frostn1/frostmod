# Bike `gfx.cfg`: rider contact points and moving parts

What MX Bikes reads from a bike's `gfx.cfg`, where it keeps it, and which keys FrostMod can put
onto a running bike without a restart. For modders tuning a model swap, and for the Frost's
Studio gfx.cfg editor.

Source: static analysis of MX Bikes beta21e (image base `0x140000000`, addresses below are RVAs),
the stock bikes in `bikes.pkz`, `rider.pkz`, and community blends. **[P]** = read in the exe or a
file. **[I]** = inferred.

## Where the game reads it

- `0x45020` builds a vehicle's gfx block at **vehicle record `+0x274`** (`0x5D7D4`, memset
  `0x4C74`). It formats `"%sbikes\%s"` + `"%s\gfx.cfg"` (`0x45164`, `0x45193`) from the bike entry
  (`+0x4C0` root, `+0x00` folder) and opens it (`0x451BC`). **[P]**
- It calls the part parser `0x419C0` twice: the bike into block `+0x000` with no prefix
  (`0x4540D`), and the cockpit into block `+0x3C4` with the prefix `cockpit/` (`0x45524`), then sets
  block `+0x3BC = 1` (`0x45529`). **[P]**
- `0x45020` is called by vehicle create `0x5CAE0` (`0x5D804`), the rider re-load `0x5E570`
  (`0x5F266`), the garage bike apply `0xE4550` (`0xE545A`) and 8 other sites. So the game reads
  `gfx.cfg` again whenever it builds the vehicle. **[P]**
- None of FrostMod's reload paths (`reload_mods`, the refresh kinds, `refresh_bike_model`) call
  `0x45020`. They rebuild the content lists, not a live vehicle. `refresh_bike_model` only logs a
  notice. **[P]** (`src/offsets.h` step table; `src/frostmod.cpp`)
- Every frame, the vehicle gfx update `0x59300` calls `0x523E0(block, part)` for the bike and the
  cockpit (`0x593D7`, `0x5936A`). It reads the parsed fields again, so a changed number in the
  block shows on the next frame. **[P]**
- Loose files win over `.pkz` ones path by path. A blend ships its own `gfx.cfg`; OEM bikes in
  encrypted `.pkz` files have none to edit. **[P]** file lists, **[I]** override order.

## Frames and units

- Distances are **metres**. Angles (`maxrot`) are **degrees**. **[I]**: stock values (grips at
  x = ±0.34, levers 10-20).
- Axes: **x = rider's right, y = up, z = forward**. **[P]** for parts (stock bounds, fork
  measurements); the left grip has negative x on every stock bike and blend.
- A key's frame is the **part block it sits in**:
  - `steer { ... }` keys are in the **steer part** frame: origin at the steering head
    (`.geom chassis_steer`, e.g. `0, 0.9912, 0.3372` on the 450), turning with the bars. The grip
    position is multiplied by the steer transform each frame (`0x58915`..`0x58938`). **[P]**
  - `chassis { ... }` keys are in the **chassis** frame. **[I]**
- A lever's `axis` is a rotation axis in the **lever node's own frame**. Stock bikes use `y` /
  `y-`, Blender blends often use `x` / `z`, because their empties are oriented differently. **[I]**
- `name =` and `link_obj =` are **node names in the bike's `.edf`**. They are looked up once at
  parse time (bus `0x78`) and stored as handles. **[P]**

## Keys

Offsets are into a part descriptor (block `+0x000` for the bike, `+0x3C4` for the cockpit). The
**Live** column is what FrostMod can apply without a rejoin. Everything else needs the game to build
the vehicle again.

### Grips (hand IK targets), in `steer { }`

| Key | Type | Offset | Live | Notes |
|---|---|---|---|---|
| `leftgrip/type`, `rightgrip/type` | int | `0x37C`, `0x39C` | yes | Stock: `1`. Passed to the hand IK (bus `0x8A`). FrostMod accepts 0-3. |
| `leftgrip/link_obj`, `rightgrip/link_obj` | node name | `0x380`, `0x3A0` (handle) | no | If set and found, the grip follows that node and `pos` is not used. |
| `leftgrip/pos/x,y,z` | float | `0x384` / `0x388` / `0x38C` | yes | Hand target, steer frame. |
| `leftgrip/dir/x,y,z` | float | `0x390` / `0x394` / `0x398` | yes | Hand direction. Stock bikes leave it out. |
| `rightgrip/pos/x,y,z` | float | `0x3A4` / `0x3A8` / `0x3AC` | yes | |
| `rightgrip/dir/x,y,z` | float | `0x3B0` / `0x3B4` / `0x3B8` | yes | |

The rider's hand reaches the grip by IK from its own `rider\riders\<rider>\gfx.cfg`:
`lefthand { refobj = riderRIG_LeftWrist pos {...} endeffector = riderRIG_LeftElbow root = ... }`.
**[P]** `rider.pkz rider/riders/default_mx/gfx.cfg`.

### Levers and pedals

Each lever is `{ name, axis, maxrot }`. `axis` is one of `x y z x- y- z-`, stored as `0 1 2 4 5 6`
(bit 2 = negative). A missing or unknown axis stays `0` (x). At full input the node turns
`maxrot` degrees about `axis` (`0x53579`..`0x53587`). **[P]**

| Block | Key | Node handle | Axis | maxrot | Live |
|---|---|---|---|---|---|
| `steer` | `throttlegrip` | `0x80` | `0x84` | `0x88` | axis, maxrot |
| `steer` | `brakelever` | `0x8C` | `0x90` | `0x94` | axis, maxrot |
| `chassis` | `rearbrakepedal` | `0x98` | `0x9C` | `0xA0` | axis, maxrot |
| `steer` | `clutchlever` | `0xB8` | `0xBC` | `0xC0` | axis, maxrot |
| `chassis` | `shifter` | `0xC4` | `0xC8` | `0xCC` | axis, maxrot |

`rearbrakepedal` also has `link`, `ref { obj pos }`, `linkpos {x y z}` and
`mastercylinder { link_obj x y z }`. `mastercylinder/x,y,z` (`0xAC`/`0xB0`/`0xB4`) is live, the rest
is not. `chassis/clutch { name axis }` exists too (axis `0xD4`, not live).

### Shock, chain, rider offset, in `chassis { }` unless noted

| Key | Type | Offset | Live |
|---|---|---|---|
| `shock/name`, `shock/refobj`, `shock/link/obj` | node names | | no |
| `shock/pos`, `shock/rot` | float | (parse-time only) | no |
| `shock/link/x,y,z` | float | `0xDC` / `0xE0` / `0xE4` | yes |
| `shock/slideaxis` | axis | `0xF0` | yes |
| `shock/reflength`, `shock/slidescale` | float | `0xF4`, `0xF8` | yes |
| `chain/name`, `chain/texture`, `chain/engine/link_obj`, `chain/ref/...` | | | no |
| `chain/pos`, `chain/rot` | float | (parse-time only) | no |
| `chain/engine/x,y,z` | float | `0x108` / `0x10C` / `0x110` | yes (used when there is no `engine/link_obj`, flag `0x100`) |
| `chain/axis` | `u`, `u-`, `v`, ... | `0x1C8` | no |
| `chain/ratio` | float | `0x1CC` | yes |
| `rider { xform { x y z } }` (top level, no prefix, also used for the cockpit) | float | `0x1E8` / `0x1EC` / `0x1F0` | yes |

`rider/xform` moves the whole rider relative to the bike each frame (`0x54419`..`0x5446D`, then
bus `0x5A` on the rider body). **[P]** Stock bikes and blends leave it out.

### Not live

- `model`, `shadow`, `ambientshadow`, `temporary` `{ file name }`, the `.hrc` files, every
  `cockpit` model.
- `plate { texture }`, `tyres`, `dirt_color`, `combined_dirt`.
- `exhaust { pos dir smoke {top_rpm max_rpm} }`: a different record (vehicle `+0x4EEC`,
  `+0x19C`..`+0x1B8`), parsed by `0x4FB60` at vehicle create (`0x5DA3F`). **[P]**
- `steer/xform`, `front_susp/xform` and the other part `xform` keys (not mapped here).

## Feet and footpegs

There is **no footpeg key** in a bike's `gfx.cfg` or `.geom`. **[P]** (exe strings: no `peg` or
`foot` key in the bike blocks; stock `450xf.geom` / `gfx.cfg`)

- The rider's legs are posed by the shared rider animation set, `rider\animations\mx\` (`rider.anm`
  to `rider42.anm`, `rider_stand.anm`) or `sm`, picked by the rider model's `animations = mx`.
  Only the hands are IK'd, to the grips. **[P]** `rider.pkz`.
- The rider is placed on the bike by the physics `.geom` key `rider = 0, -0.0042, -0.0358` (the
  same on all five stock bikes; `rider_mass`, `rider_view` beside it). That file belongs to the
  bike's physics and is not part of a blend. **[P]**
- So the pegs on a swapped model must sit where the stock animation puts the boots. The only
  per-model handle is `rider { xform }`, which moves the whole rider (hands then re-reach the grips
  by IK). **[I]**

## FrostMod: applying it live

- **F8 → `G` (Watch gfx.cfg)**, off by default, saved as `gfxwatch=1` in `frostmod_radar.cfg`.
  Every 0.5 s FrostMod checks the loose `gfx.cfg` of each loaded bike (`<mods>\bikes\<Bike>\gfx.cfg`,
  which is the active FrostMod Models variant). A new write time or size re-applies the live keys
  to every loaded vehicle on that bike, bike and cockpit. Changed values go to `frostmod.log` as
  `[gfx] slot N key: old -> new`.
- **Command file verb `reload_bike_gfx`**: the same, once. `bikeId` is the bike folder name;
  without it every loaded bike is re-read.

  ```json
  {"verb":"reload_bike_gfx","bikeId":"MX2OEM_2023_KTM_250_SX-F","at":1760000000000}
  ```

  Write it to `%TEMP%\frostmod_cmd.json` (or beside `frostmod.dll`) after saving, same as the
  other verbs. `at` makes a repeat count as a new command.
- Only keys present in the file are written. Removing a key does not reset it.
- Keys marked not live are logged as `needs a rejoin to show`. Rejoining, or anything else that
  makes the game build the vehicle again, reads them.
- Safety: before the first write FrostMod compares 46 instructions the offsets came from
  (`src/gfxcfg.h`, `kSites`) with the running exe. One difference and the feature stays off. A
  value the game could not use (not a number, out of range, unknown axis) is skipped and logged.
- It writes only floats and small ints the game's own parser writes, so it works in the pits and
  on track. It changes only what this PC draws; other riders see your bike as they loaded it.
