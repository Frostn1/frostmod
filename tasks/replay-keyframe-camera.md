# Replay keyframe camera — RE results (MX Bikes beta21e)

Static RE against `mxbikes.exe.unpacked.exe` (x64 PE, image base `0x140000000`, fixed-function
OpenGL). All addresses below are **RVAs**; add the module base at runtime. Every address in §1–§3
was re-verified by resolving the rip-relative displacement at the named instruction.

Per-area detail, full disassembly traces and AOB signatures are in
`tasks/replay-keyframe-camera-findings/` — `freecam.md` (camera state + signatures),
`replay-clock.md`, `cam-struct.md` (`cameras.cfg` layouts), `camera-select.md` (plugin
callback table), `gl-matrix.md` (OpenGL pipeline).

**Bottom line: the feature is a state-write problem, not a rendering problem.** The game already
owns a 6-DOF free-roam replay camera whose entire state is flat statics, and a replay clock that is
a single writable `int32`. Keyframing = sample those on capture, interpolate and write them back on
playback.

## 1. Free-roam camera state — all flat statics, no pointer chain

| RVA | Type | Field | Notes |
|---|---|---|---|
| `0x4CA3C8` | `float[3]` | position x,y,z | world metres, **Y up**, contiguous |
| `0x4C91A0` | `float[3]` | yaw, pitch, roll | **degrees**, contiguous |
| `0x4C91FC` | `float` | field of view | degrees |
| `0x4CA3F0` | `int32` | speed level | 1..6, mult = `1.25 * 2^level` |
| `0x4CA2A0` | `int32` | active camera / mode | see §2 |
| `0x4C91D4` | `int32` | onboard camera count `N` | |

Orientation is **Euler degrees**, not a quaternion: the matrix is rebuilt every frame at `0xC7476`
as `M = Ry(yaw) · Rx(pitch) · Rz(roll)`, column-vector convention, +Z forward, translation in
column 3. Roll is real and animatable here — unlike `cameras.cfg` cameras, which parse only 2 of 3
rotation floats and never apply roll.

Engine-enforced clamps inside the update: pitch `[-80,+80]`, fov `[1.5,90]`, position to the track
AABB ±10 m and to `terrainHeight + 0.1`. Writing after `0xC7A39` bypasses all four.

## 2. The mode gate — the one thing that will silently break a naive implementation

With `N` = onboard camera count, the camera enum is:

```
0        auto / directed TV camera set
1..N     onboard cameras
N+1      orbit          N+2  free (bike-anchored)
N+3      FREE-ROAM  <-- the flyable one
N+4      VR
```

At `0xC8BA9`, the **last thing the replay update does every frame** is: if the mode is not `N+3` or
`N+4`, overwrite position/yaw/pitch/roll from the live camera. So a mod that writes the pose without
first forcing the mode gets clobbered once per frame and looks like nothing happened.

```c
*(int*)(base+0x4CA2A0) = (*(int*)(base+0x4C91D4) > 0) ? *(int*)(base+0x4C91D4) + 3 : 1;
```

Direct writes race the update and are re-clamped at `0xC58CA`. The **sanctioned** alternative: ship a
PiBoSo `.dlo` plugin and implement `SpectateCameras` — return 1 and write `N+3` into `_piSelect`.
The game polls it every frame and writes the result straight into `0x4CA2A0`. That removes the mode
half of the problem from the patching surface entirely. (Payload caveat: `_pCameraData` is **not** a
struct array — it is `_iNumCameras` packed NUL-terminated localised names. Walk with `strlen()+1`;
index is the stable identity, the text is not.)

## 3. Replay clock

| RVA | Type | Field |
|---|---|---|
| `0xE5666C` | `int32` | **current playback time, milliseconds** |
| `0xE56678` / `0xE56658` | `int32` | replay start / end (ms) |
| `0xE56674` | `int32` | speed, signed ±1..16, **0 = paused** |
| `0xE56660` | `int32` | slow-mo flag: ≠0 ⇒ speed is a divisor |
| `0xE56668` | `int32` | live/pinned-to-tail flag |
| `0x4C9194` | `int32` | game mode; `4` = loaded `.rpy`, `0`/`5` = instant, `3` = live |

**Scrubbing is a single dword write to `0xE5666C`** — that is literally all the game's own slider
handler does; the stream re-seeks on the next frame via `Dispatch(0x3AA, ...)` at `0xC50AB`, which
also applies the start/end/marker clamping for free. No call needed, no race.

Replay samples are stored one per **30 ms (33.33 Hz)**; frame-step buttons move the clock by exactly
±30. That is the natural quantum to snap keyframes to.

Hook site for a path player: **`0xC50B1`** — clock advanced, clamped, and stream positioned, scene
not yet submitted.

`.rpy` files are `'RPY\0'` + format version 10 and contain **no camera track** — the serializer at
`0x12F840` never touches the camera statics. Paths must be stored by us, keyed on replay ms. That is
fine: the ms timeline is stable and monotonic, which is exactly what keys need.

## 4. Fallback: the OpenGL API boundary

The game is fixed-function GL. It never calls `glPushMatrix`/`glPopMatrix`, and `glMultMatrixf` is
applied to `GL_PROJECTION` only — so every modelview change in the whole game is `glLoadIdentity` or
`glLoadMatrixf`, and a hook sees complete matrix state from one tracked variable. Every 3D scene pass
emits `glMultMatrixf(diag(1,1,-1,1))` → `glMatrixMode(GL_MODELVIEW)` → `glLoadMatrixf(view)` with no
GL call in between; that adjacency plus a full-window viewport test uniquely identifies the scene
view. All shaders transform via `ftransform()`/`gl_ModelViewMatrix`, so the fixed-function hook
covers the shader path too.

**But do not ship this as the primary route.** Overriding at the API boundary changes only what the
GPU is told: the game still culls and LODs against the *original* frustum, orients billboards from
the original camera, and anchors the sky dome to it. Result: pop-in, holes, and wrongly-facing
foliage. The clean equivalent is to write the camera world transform at `viewState+0x60` before
`Camera::Update` (`0x249510`) runs — that propagates to the view matrix, frustum planes, billboard
basis and sky in one shot. Keep the GL hook as the patch-proof degraded fallback only.

## 5. Offsets move between every build — the real engineering constraint

Comparing beta21d and beta21e, released **two days apart**: every function moved (by different,
non-uniform deltas in both directions), the camera-set global moved **backwards** by `0x260`, and the
engine's action/message IDs **renumbered by −2**. Pinned RVAs are therefore not a viable shipping
strategy for this feature.

What survived all three binaries tested (beta21d, beta21e, and GP Bikes — a different title on the
same engine, compiled at a different optimisation level):

- **String literals.** `find string → rip-relative xref → containing .pdata function` resolved 100%
  of targets in all three. This is the technique to build on.
- **Struct layouts.** Camera stride `0x3C`, camset slot stride `0x574` × 50, count at `+0xF0`,
  array at `+0xF4`, and the `name/part/pos[3]/rot[2]/fov/gyro` layout are byte-identical across two
  MX versions *and* a different title. Trust these hard.

Not durable: any raw RVA, any action/message ID, and any byte signature across titles (MX↔GP share
source, not object code — no cross-title AOB exists, and no wildcarding fixes that).

**Recommended resolver:** anchor on the `cameras.cfg`/`Replay*` string literals, walk to the
containing function, and read the globals out of that function's own `lea`/`mov` displacements at
startup. One anchor yields both the code site and the data address. AOB signatures (recorded in the
per-agent findings files) are unique in both MX builds and serve as the verification path.

## 6. What still needs a Windows session

Static analysis took this as far as it goes. Each item below is minutes, not hours:

1. **Sign conventions on screen** for yaw/pitch/roll. The maths is proven; which visual direction
   `+yaw`/`+pitch` point is not, and the init path stores pitch negated. Zero the three and see
   where it looks; tap a rot key and read the delta sign.
2. **Does writing `0x4CA2A0` alone enter free-roam?** Nothing re-derives the mode, so it should
   stick — unproven.
3. **Hook ordering** at `0xC50B1`: confirm it fires once per rendered frame, after the stream seek
   and before draw submission.
4. **Scrub round-trip**: write `start + 5000` to `0xE5666C` while paused. Validates the unit, the
   clamps and the write-to-scrub approach in one test.
5. **Replay object index** `0xE53DC8` — no `.text` writer found; read it live (likely 1).
6. Confirm position units are metres (fly 1 s at speed level 1 = 2.5 u/s against a ~2.1 m bike).

## 7. Consequences for the build

The RE that was the project's main risk is done and cost one session rather than the estimated
4–10 h of Windows time. Remaining work is ordinary engineering: a resolver, a hook, spline
interpolation over `(pos, yaw, pitch, roll, fov)` keyed on replay ms, and an editor UI. Catmull-Rom
on position and on the Euler channels is adequate — the engine itself never slerps, it rebuilds from
Euler angles every frame — with ±180° wrap handling on yaw and roll.
