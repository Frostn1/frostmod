# Rider bones: recording the pose the game drew

MXB Coach's recorder (`mxbcoach.dlo`) writes the local rider's skeleton into the `.mxbc` file
with every telemetry sample: two new records, `RIDER_SKELETON` (tag 18) and `RIDER_BONES`
(tag 19). Before this, the file had the bike exactly but the rider only as inputs (`STANCE`,
`LEAN`), and anything drawing the rider had to guess the pose. MXB Cinema's guess turned the
head too far and stretched a leg against the same moment in game.

Code: `src/riderbones.h` (walk, build check, record bytes; no Win32), the
"rider's animated skeleton" block of `src/offsets.h` (every address and field), and the
`CaptureBones` / `PollBones` pair in `src/mxbcoach.cpp`. Test: `tests/riderbones_test.cpp`.

**Game build: MX Bikes beta21e, `TimeDateStamp 0x6A21833D`.** On anything else the recorder
writes one `RIDER_SKELETON` saying "unknown build" and no `RIDER_BONES`.

## Status

| | |
|---|---|
| Layout of objects, hierarchy and nodes | confirmed statically, instruction by instruction |
| Own-vehicle -> gfx block -> rider/bike body handles | confirmed statically (and the record/gfx block already confirmed live for FrostMod's rebuild work) |
| Node world matrix is what the mesh is skinned with | inferred: the draw path was not traced |
| Draw runs on the thread that computes the skeleton, after it is computed for the frame | inferred |
| Values read from a running game | **not yet** — no game was running when this was written |

## How it was found

Static analysis of the beta21e code, the same image the rest of `offsets.h` was read from.
Addresses are RVAs from the image base.

1. **Where the rider is drawn.** The rider-gfx block was already mapped for FrostMod's rider
   rebuild (see `offsets.h`): `+0x1D4` is the rider body object's handle. The pose routine `0x523E0`
   places that object every frame with engine command `0x5A` (set object matrix) from the
   rider body transform in the sim (`+0x438`), and places the bike body (`+0x000`) the same way
   from the chassis matrix times a fixed offset held at `+0x004`. Command `0x5A` (`0x219790`)
   copies 64 bytes into the object at `+0x11C`: the object's world matrix.
2. **The rider is animated through named nodes.** The rider build (`0x3F070`) loads
   `rider\animations\<type>\anim%d` with engine command `0x81` and looks animations up by name
   with `0x82` ("leaning sitting back", ...). The pose routine then drives them with `0x83`..
   `0x88` (weights at anim `+0x84`, times at `+0x8C`, anims `0x98` apart at object `+0x230`)
   and turns the head with `0x7A`, which writes three angles into one node at `+0xA8` and sets
   bit 8 of its flags. Command `0x81`'s loader (`0x21CCC0`) binds each animation track to a
   node by comparing the track name with the node's name at node `+0x00`. The stock `.anm`
   files carry 71 tracks named `Point01`, `riderRIG_Root`, ..., `riderRIG_Head`, ...,
   `riderRIG_armour`: those are the node names.
3. **The node array.** Every accessor goes `object +0x1F0` -> hierarchy, `+0x10` count,
   `+0x18` array, `0x3F8` bytes a node.
4. **Where the frame's pose ends up.** The animated-object update `0x221B90` (from the frame
   update `0x252BB0`) sets the object's hierarchy index `+0x1E8` to 0, blends the animations
   into each node's local matrix at `+0x68` (`0x21FDD0`), then walks the tree from the roots
   (`0x243220` -> `0x2430F0`, recursive): local `+0x68` is copied to `+0xC0`, the head-turn
   override is applied on top when bit 8 is set, and `world(+0x100) = parent world x local`,
   with the object's own `+0x11C` as the roots' parent. Children are `+0x290` (count) and
   `+0x298` (indices).
5. **Matrix convention.** The multiply (`0x2663E0`), identity (`0x266390`) and translate
   (`0x266D60`, adds into `+0x0C/+0x1C/+0x2C`) fix it: 4x4 floats, row-major, column vectors,
   translation in the last column.
6. **Handles to objects.** `0x15C270` is a binary search over the table at `[0x584870]`:
   `i32 count`, then 16-byte entries from `+4`, `i32 handle` and the object pointer at `+8`.
7. **Which record is the local rider.** `[0xE62710] - 1` indexes the vehicle records at
   `0xF4EE20` (`0x5B24` each); the in-session draw (`0xA7282`) passes that record's `+0x274`
   to the rider placement `0x59300`, which calls the pose routine.

So for the frame just drawn: node world matrices in world space, and the bike body's world
matrix beside them. `RIDER_BONES` stores `inverse(bike) x node`, which is the pose in the
bike's own frame no matter where the bike is, and is consistent with the bike as drawn rather
than with a telemetry sample taken at a slightly different moment.

## Version check

Two gates, both checked once per game session before anything is read:

- the exe's `TimeDateStamp` from its PE header must be `0x6A21833D`;
- nine code sites (`mxb::kRiderBoneSigs`) must match byte for byte in the running process, with
  call targets wildcarded. Each pins fields the walk depends on: the node stride, `+0x18`,
  `+0x100`, `+0xC0`, `+0x290/+0x298` (node world), `+0x1E8/+0x1F0` and the node count
  (animated update), `+0x11C` (set matrix), the handle table and its entry layout, the node
  name compare, the rider and bike body placement, and the own-vehicle globals.

Then every frame: every read is SEH-guarded, the bike matrix must be a rotation plus a move,
every node matrix must be finite and within a kilometre of the bike, and child indices must be
in range. Any failure is "not found" for that frame; nothing is ever estimated.

## Records

Layouts are pinned at the top of `src/riderbones.h` and by `tests/riderbones_test.cpp`.

`RIDER_SKELETON` (18), once per stint before the first `RIDER_BONES`, and again only if the
skeleton changes (a rider model swapped in the pits) or first becomes known:

```
0  u8   layout (1)
1  u8   status: 0 ok, 1 unknown game build, 2 not found
2  u8   node count N (0 unless ok)
3  u8   recorded count M
4  u32  exe TimeDateStamp
8  f32[3] bike body offset from the physics chassis (gfx +0x004), game axes, m
20 N x { u8 parent (255 = root), u8 recorded, u8 name length L, char name[L] }
```

`RIDER_BONES` (19), with each `SAMPLE` while the skeleton is known, 24 + 15*M bytes:

```
0  f32  track time s    (as the SAMPLE)
4  f32  lap position
8  u8   layout (1)
9  u8   M
10 u8   flags: bit 0 = repeat (no new frame drawn since the last one)
11 u8   0
12 f32[3] origin: the rider body object in the bike frame, m
24 M x { u8 node; i16 qx, qy, qz, qw (/32767); i16 x, y, z (mm from origin) }
```

Game axes throughout: x right, y up, z forward. The quaternion is the node's rotation in the
bike frame (not relative to its parent). Recorded nodes: every `riderRIG_*` except the
fingers, 40 on the stock rider, `riderRIG_Head` among them; 624 bytes a sample. Readers that
predate these tags skip them by length, as they skip every tag they don't know.

## Decoding (Python)

```python
import struct

def read_bones(path):
    d = open(path, 'rb').read()
    assert d[:4] == b'MXBC'
    at, names, frames = 8, None, []
    while at + 8 <= len(d):
        tag = d[at]
        n = struct.unpack_from('<I', d, at + 4)[0]
        p = d[at + 8:at + 8 + n]
        at += 8 + n
        if tag == 18:                                    # RIDER_SKELETON
            status, count = p[1], p[2]
            names, k = [], 20
            for _ in range(count):
                parent, recorded, ln = p[k], p[k + 1], p[k + 2]
                names.append(p[k + 3:k + 3 + ln].decode('latin1'))
                k += 3 + ln
            if status != 0:
                names = None                             # 1 unknown build, 2 not found
        elif tag == 19 and names:                        # RIDER_BONES
            t, lap = struct.unpack_from('<2f', p, 0)
            m, flags = p[9], p[10]
            ox, oy, oz = struct.unpack_from('<3f', p, 12)
            bones = {}
            for i in range(m):
                node, qx, qy, qz, qw, x, y, z = struct.unpack_from('<B7h', p, 24 + 15 * i)
                bones[names[node]] = ((qw / 32767, qx / 32767, qy / 32767, qz / 32767),
                                      (ox + x / 1000, oy + y / 1000, oz + z / 1000))
            frames.append((t, bones))
    return frames

# frames[k][1]['riderRIG_Head'] -> ((w, x, y, z), (x, y, z)) in the bike frame, game axes
```

## What a live game has to confirm

Nothing here launches the game. To confirm, with `mxbcoach.dlo` from this branch installed
and an ordinary stint ridden:

1. `mxbcoach.log` has a `bones` line reading `recording <N> nodes` (not `off:` or
   `not found:`); N is the rider model's node count, 71 if it holds just the stock rig. If it says `off:`, the line names the site that differs.
2. The stint's `.mxbc` decodes with the snippet above, `riderRIG_Head` sits roughly 1.5 m
   above the bike origin, and the repeat flag is mostly clear (the game draws faster than
   50 Hz).
3. A frame matches an F10 shot of the same moment: head turn and leg bend in the decoded pose
   against the screenshot.
