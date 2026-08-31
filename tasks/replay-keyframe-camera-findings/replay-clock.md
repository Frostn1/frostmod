# MX Bikes — Replay playback clock & transport controls

Target: `/Users/seandahan/Downloads/mxbikes.exe.unpacked.exe` (x64 PE, Steamless-unpacked, image base `0x140000000`).
All addresses given as **VA** and **RVA** (RVA = VA − 0x140000000).

---

## 0. TL;DR for the camera-path mod

* The replay clock is **not** a float and **not** a heap field you have to chase — it is a plain
  **`int32` millisecond counter in BSS at `0x140E5666C` (RVA `0xE5666C`)**.
* To scrub: **write milliseconds into `0x140E5666C`**. That is literally all the game's own slider
  handler does; the engine re-seeks the stream on the next frame. No function call required.
* To read: read the same dword. `seconds = (double)value / 1000.0`.
* Hook site for "run right after the clock advanced, before render":
  **`0x1400C4F37` (RVA `0xC4F37`)** — the instruction right after the increment, inside the
  per-frame replay update `0x1400C4271`.

---

## 1. Replay playback state — the clock block

**Storage class: a flat block of nine `int32` statics in BSS.** Not a heap object, no pointer chain.
(`.data` is RVA `0x37C000..0x10A3100` but only `0x1A800` bytes are raw-backed, so `0xE566xx` is
zero-initialised BSS. That is why every dword reads 0 in the file image.)

| VA | RVA | Type | Field | Status |
|---|---|---|---|---|
| `0x140E56658` | `0xE56658` | `int32` | **Replay END time (ms)** | CONFIRMED |
| `0x140E5665C` | `0xE5665C` | `int32` | Marker END (ms), `-1` = unset (`ID_MARKER_END`) | CONFIRMED |
| `0x140E56660` | `0xE56660` | `int32` | **Slow-motion flag**: 0 = speed is a multiplier, ≠0 = speed is a divisor (1/N) | CONFIRMED |
| `0x140E56664` | `0xE56664` | `int32` | Marker START (ms), `-1` = unset (`ID_MARKER_START`) | CONFIRMED |
| `0x140E56668` | `0xE56668` | `int32` | **LIVE flag** — 1 = clock is pinned to the live tail (UI prints `live_replay`) | CONFIRMED |
| `0x140E5666C` | `0xE5666C` | `int32` | **CURRENT PLAYBACK TIME, milliseconds** | CONFIRMED |
| `0x140E56670` | `0xE56670` | `int32` | Sub-step remainder accumulator (ms), slow-mo only | CONFIRMED |
| `0x140E56674` | `0xE56674` | `int32` | **PLAYBACK SPEED** — signed; `0` = **PAUSED** | CONFIRMED |
| `0x140E56678` | `0xE56678` | `int32` | **Replay START time (ms)** | CONFIRMED |

Supporting global:

| VA | RVA | Meaning | Status |
|---|---|---|---|
| `0x140E54388` | `0xE54388` | Frame delta-time, **int32 milliseconds** | CONFIRMED |
| `0x140E53DC8` | `0xE53DC8` | Replay/session object index (1-based) passed to every dispatch call | CONFIRMED |
| `0x140E4B2E0` | `0xE4B2E0` | Table of 10 `void*` — the actual replay stream objects | CONFIRMED |
| `0x1404C9194` | `0x4C9194` | Game/session mode (see §4) | CONFIRMED |
| `0x140566C48` | `0x566C48` | Engine dispatch function pointer → `0x140120CC0` | CONFIRMED |

### 1.1 Unit is milliseconds — CONFIRMED, four independent ways

1. `0x140E54388` (the value added to the clock each frame) is converted to seconds by
   `divsd xmm1, [0x140353908]` where `[0x140353908] = 1000.0` (double) — at `0x14000BEF2` and
   `0x1400144CD`. A ms delta added to the clock ⇒ the clock is ms.
2. `0x1400C8A55`: `movsd xmm14, [0x140353908]` (= `1000.0`), then at `0x1400C9BEC`
   `movd xmm5,[0x140E5666C]; cvtdq2ps; cvtps2pd; divsd xmm0, xmm14` ⇒ the clock itself is divided
   by 1000.0 to get seconds.
3. Instant replay seeds the clock at `END − 10000` (`0x1400C36D9: add r11d, 0xFFFFD8F0`) — the
   canonical 10-second instant replay.
4. Frame-step buttons move the clock by exactly `±30` (`0x1400BEE5E` / `0x1400BEF4F`), and the
   stream seeker divides the time delta by 30 (`0x14012CA20`, magic `0x88888889` + `sar 4` = signed
   `/30`). **One replay sample every 30 ms ⇒ 33.33 Hz keyframe rate.**

### 1.2 Speed encoding — CONFIRMED

`speed = [0xE56674]`, `slow = [0xE56660]`:

| `slow` | `speed` | Effective rate | UI text |
|---|---|---|---|
| 0 | `0` | **paused** | `0x` |
| 0 | `1..16` | `1x .. 16x` forward | `%dx` |
| 0 | `-1..-16` | `1x .. 16x` reverse | `%dx` (negative) |
| ≠0 | `2..16` | `1/2x .. 1/16x` forward | `1/%dx` |
| ≠0 | `-2..-16` | `1/2x .. 1/16x` reverse | `-1/%dx` |

Clamped to ±16 everywhere (`cmp eax, 0x10` / `cmp eax, -0x10`).

### 1.3 Sample cursor — the replay is byte-offset indexed, not frame indexed

There **is** a heap object; it holds the stream, not the clock.
`obj = ((void**)0x140E4B2E0)[ [0x140E53DC8] − 1 ]`, index range 1..10.

| Offset | Type | Meaning | Status |
|---|---|---|---|
| `+0x18` | `void*` | Frame ring buffer base | CONFIRMED |
| `+0x20` | `int32` | Channel/rider count | CONFIRMED |
| `+0x30` + n·`0x8CA0` | struct | Per-rider channel array | CONFIRMED |
| `+0x1B777C` | `int32` | Ring wrap-around byte offset | CONFIRMED |
| `+0x1B7784` | `int32` | Ring head byte offset (stop condition) | CONFIRMED |
| `+0x1B7788` | `int32` | **Stream START time (ms)** — source of the `0xE56678` global | CONFIRMED |
| `+0x1B7790` | `void*` | Frame scratch buffer | CONFIRMED |
| `+0x1B7798` | `int32` | Live cursor: **byte offset** into `+0x18` | CONFIRMED |
| `+0x1B779C` | `int32` | Live cursor: time (ms) | CONFIRMED |
| `+0x1B77A0` | `void*` | Live cursor: decoded-frame ptr | CONFIRMED |
| `+0x1B77A8/AC/B0` | same triple | Scratch copy used during a seek | CONFIRMED |
| `+0x1B77BC` | `int32` | **Stream END time (ms) + 30**; the reported end is `this − 30` | CONFIRMED |

So the cursor is a **byte offset into a variable-length-frame ring buffer**, paired with a
millisecond time cursor. There is no integer frame index — but frame *n* from the end is exactly
`n * 30 ms`, which is what the seeker uses.

`[0xE56678]`/`[0xE56658]` are refreshed every frame from the object via dispatch cmd `0x3A8`
(`0x1400C4CFA`, `0x1400C34C3`).

---

## 2. The seek path (scrub to time T)

### 2.1 The slider handler — CONFIRMED

`ID_REPLAY_SLIDER` string at `0x14033E340`. Its value-changed branch is at
**`0x1400BF40F` (RVA `0xBF40F`)**, inside the replay UI command handler
`0x1400BDEEB..0x1400BF610` (RVA `0xBDEEB`).

Inferred signature of the containing handler:

```c
// 0x1400BDEEB  (RVA 0xBDEEB)
int ReplayUiCommand(int page /*r15d*/, int notify /*ebp*/, int param, const char* ctrlName /*rbx*/);
// notify == 2 -> slider value changed
```

The seek arithmetic at `0x1400BF466`:

```
slider = Dispatch(0x130, ctrlHandle, &out);      // integer 0..1000
start  = [0x140E56678];
end    = [0x140E56658];
[0x140E5666C] = (int)( (float)(end - start) * ((float)slider / 1000.0f) + (float)start );
```

The display path is the exact inverse (`0x1400BE83E`):
`slider = (int)((float)(cur - start) / (float)(end - start) * 1000.0f)`.

### 2.2 The directly callable seek — CONFIRMED

The engine exposes one dispatcher, a plain function assigned statically into a global pointer:

* **`0x140120CC0` (RVA `0x120CC0`)** — `int Dispatch(int cmd, ...)`, `__fastcall`, varargs spilled
  to the shadow area; jump table at `0x1401280B0` (RVA `0x1280B0`), valid `cmd` range `0..0x3CA`.
* Pointer copy at `0x140566C48` (RVA `0x566C48`), written at `0x14013403E` and `0x1401345BE`.
  Either is callable from a mod.

Replay commands:

| cmd | Handler | Signature | Status |
|---|---|---|---|
| `0x3A8` | inline at `0x140127CAF` (RVA `0x127CAF`) | `int GetRange(int idx, int* outStartMs, int* outEndMs)` | CONFIRMED |
| `0x3AA` | `0x14012D440` (RVA `0x12D440`) | `int Seek(int idx, int timeMs)` — returns 0 ok, 1 fail | CONFIRMED |
| `0x3AC` | `0x14012F840` (RVA `0x12F840`) | `int SaveReplay(int idx, const char* path, int fromMs, int toMs, ...)` | CONFIRMED |
| `0x3A9` | `0x14012D4D0` (RVA `0x12D4D0`) | replay frame decode/apply | INFERRED |

`Seek` worker: **`0x14012C950` (RVA `0x12C950`)** — `void SeekWorker(void* obj, int targetMs)`.
It steps the ring cursor backwards in 30 ms frames, `ceil((end − target)/30)` iterations.

**Recommendation for the mod:** do *not* call `Seek` directly. `0x1400C4271` calls
`Dispatch(0x3AA, [0xE53DC8], [0xE5666C])` every frame from the global clock, so writing
`0x140E5666C` is sufficient, race-free, and survives the game's own clamping (start/end/marker
clamps at `0x1400C4F37..0x1400C5014`).

---

## 3. Per-frame replay advance — the hook site

**Function: `0x1400C4271 .. 0x1400C52EA` (RVA `0xC4271`)** — the per-frame replay update.
**Clock advance block: `0x1400C4EE1 .. 0x1400C4F37` (RVA `0xC4EE1`).** CONFIRMED.

```c
int dt = *(int*)0x140E54388;                       // frame delta, ms
if (*(int*)0x140E56660 == 0) {                     // normal speed
    int step = dt * *(int*)0x140E56674 + *(int*)0x140E56670;
    *(int*)0x140E56670 = 0;
    *(int*)0x140E5666C += step;
} else {                                           // slow motion
    int acc = *(int*)0x140E56670 + dt;
    int q   = acc / *(int*)0x140E56674;
    *(int*)0x140E56670 = acc % *(int*)0x140E56674;  // remainder kept
    *(int*)0x140E5666C += q;
}
// ---- 0x1400C4F37: clamps (end, start, marker end, marker start) ----
// ---- 0x1400C5099: Dispatch(0x3AA, [0xE53DC8], [0xE5666C])  <-- the actual stream seek
// ---- 0x1400C5140+: apply decoded frame to each rider, then render
```

**Best hook: `0x1400C4F37` (RVA `0xC4F37`)** — clock already advanced, clamps not yet applied.
**Safer hook: `0x1400C50B1` (RVA `0xC50B1`)** — immediately after the
`Dispatch(0x3AA, …)` at `0x1400C50AB`, so the clock is advanced *and* clamped *and* the stream is
positioned, and the scene has not been submitted yet. A camera-path player wants the latter.

Reaching the end triggers the mode-dependent exit at `0x1400C4F43`
(`mode == 4` → `0x1400EC330("home/main_menu")`, `mode == 5` → `0x1400EC640`), otherwise it pins
`cur = end` and sets `speed = 0`.

---

## 4. "Is a replay loaded / playing?"

### 4.1 The mode variable — CONFIRMED

**`0x1404C9194` (RVA `0x4C9194`)**, `int32`. Written by the mode-entry function
**`0x1400C30F0` (RVA `0xC30F0`)** (`mov [0x1404C9194], ecx` at `0x1400C3115`), which jump-tables on
`ecx` (0..5) via the table at `0x1400C3B3C`.

The clock is then seeded per mode by the dispatcher at **`0x1400C354C` (RVA `0xC354C`)**:

| mode | Branch | Effect on the clock | Meaning |
|---|---|---|---|
| `0` | `0x1400C36AD` | `live=0, speed=1, cur = end − 10000` | **Instant replay** |
| `1`, `2` | `0x1400C3563` | `live=0, speed=0, cur = 0` | On-track / racing (replay paused) |
| `3` | `0x1400C35DE` | `live=1, speed=1, cur = end` | **Live / spectate** (following the tail) |
| `4` | `0x1400C359F` | `live=0, speed=1, cur = 0` | **Loaded `.rpy` playing from the start** |
| `5` | `0x1400C36AD` | `live=0, speed=1, cur = end − 10000` | Instant replay (alt. exit path) |

**Test a mod should use:**

```c
int  mode   = *(int*)0x1404C9194;                 // 4 = loaded .rpy, 0/5 = instant replay, 3 = live
int  live   = *(int*)0x140E56668;                 // 1 = pinned to live tail
int  speed  = *(int*)0x140E56674;                 // 0 = paused
int  start  = *(int*)0x140E56678, end = *(int*)0x140E56658;
bool replay_loaded  = (mode == 0 || mode == 3 || mode == 4 || mode == 5) && (end > start);
bool replay_playing = replay_loaded && speed != 0;
```

### 4.2 Relation to the plugin API `-1 = loaded replay` — INFERRED, not proven

`mxb_api.h` documents `SPluginsRaceEvent_t::m_iType` as `1 = testing, 2 = race, 4 = straight
rhythm, -1 = loaded replay`. Note the on-track modes here are `1` and `2` and the "straight rhythm"
value `4` collides numerically with the replay mode `4` in `0x1404C9194` — **so `0x1404C9194` is
NOT the same variable the plugin API reports.** The plugin export table is built at `0x14012A5A0`
(`EventInit` → slot `+0x20`, `RaceEvent` → slot `+0x60`, resolved by `GetProcAddress` at
`0x140321168`); I did not locate the site that stores `-1` into the `SPluginsRaceEvent_t` it passes.
Treat the plugin `-1` and `0x1404C9194 == 4` as *correlated but distinct* until measured.

**Runtime experiment to settle it:** load a `.rpy` from the main menu, and with a debugger
(or a trivial logging plugin implementing `RaceEvent`) print `m_iType` alongside
`*(int*)0x1404C9194`. If `m_iType == -1` exactly when `0x1404C9194 == 4`, the mapping is 1:1;
otherwise search for the true run-type global by breaking on the `RaceEvent` callback and walking
back to the struct's writer.

---

## 5. What the `.rpy` writer touches — is the camera in the file?

**Answer: no. The camera is purely live state. An offline `.rpy` camera track is not conceivable
without extending the format.** CONFIRMED (structurally).

`Dispatch(0x3AC, idx, path, fromMs, toMs, …)` → **`0x14012F840` (RVA `0x12F840`)**:

```
obj = ((void**)0x140E4B2E0)[idx-1];
FILE* f = fopen(path, "wb");                       // "wb" @ 0x1403412D4
fwrite("RPY", 1, 4, f);                            // magic 'RPY\0'  (0x595052)
fwrite(&(int){10}, 1, 4, f);                       // format version 10
fwrite(obj+0x00, 1, 12, f);                        // stream header
fwrite(obj+0x0C, 1, 4, f);
fwrite(obj+0x10, 1, 4, f);
// fromMs == -1 -> obj+0x1B7788 (start);  toMs == -1 -> obj+0x1B77BC - 30 (end)
// SeekWorker(obj, fromMs) at 0x14012C950, then streams the frame ring from obj+0x18
```

Every operand in the writer is either `obj`-relative (offsets `0x00..0x10`, `0x8C8C`, `0x8C98`,
`0x1B7768..0x1B77BC` — i.e. the header, the per-rider channel arrays at stride `0x8CA0`, and the
frame ring) or one of three `.rdata` strings (`"RPY"`, `"wb"`, `"rb"`). **It never reads the camera
statics.** The replay camera lives in a completely separate `.data` region (`0x1404C91A0`,
`0x1404C91A4`, `0x1404C91A8`, `0x1404C91FC`, `0x1404C91DC` — FOV/yaw/pitch/roll floats written by
`0x1400C3226`) and is driven at runtime by the `ReplayMove*` / `ReplayRot*` / `ReplayFov*` bindings.
Nothing copies those into the serializer.

Consequence for the mod: a camera path must be authored and replayed **by the mod**, keyed on
`[0x140E5666C]`. The `.rpy` gives you a stable, monotonic ms timeline to key against, which is
exactly what you need — but the file will never carry the path itself.

Related: `"Replay recorded and saved into %s"` (`0x140335C48`) is only referenced from
`0x140023AD0`, the **dedicated-server config/announce** routine (it parses `mxbikes.ini`,
`[replay] save`, `%sreplays`) — it is a log line, not the serializer.

---

## 6. AOB signatures

All ten verified **unique** across the whole image (single occurrence each). `??` = wildcard.

### CLOCK ADVANCE — VA `0x1400C4EE1` / RVA `0xC4EE1`
```
8B 15 ?? ?? ?? ?? 44 39 35 ?? ?? ?? ?? 75 24 0F AF 15 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 03 15 ?? ??
```
Wildcards are all rip-disp32: `+0x02` (→ dt `0xE54388`), `+0x09` (→ slow `0xE56660`),
`+0x12` (→ speed `0xE56674`), `+0x18` (→ cur `0xE5666C`), `+0x1E` (→ rem `0xE56670`).
Resolving any one of them recovers the whole clock block — e.g. `cur = &insn(+0x18).next + disp32`.

### SLIDER SEEK handler entry — VA `0x1400BF40F` / RVA `0xBF40F`
```
48 8D 15 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? 85 C0 0F 85 3C 01 00 00 83 ED 01 0F 84 80 00 00 00
```
`+0x03` rip-disp32 → `"ID_REPLAY_SLIDER"`; `+0x0B` rel32 → strcmp `0x1402AB4B6`.

### SLIDER → ms math — VA `0x1400BF466` / RVA `0xBF466`
```
8B 05 ?? ?? ?? ?? 44 8B 1D ?? ?? ?? ?? 66 0F 6E 44 24 58 66 0F 6E C8 0F 5B C0 44 2B D8 F3 0F 5E
```
`+0x02` → start `0xE56678`; `+0x09` → end `0xE56658`; `+0x21` → the `1000.0f` constant `0x140353A38`.

### `Seek(idx, ms)` (cmd `0x3AA`) — VA `0x14012D440` / RVA `0x12D440`
```
40 53 48 83 EC 20 FF C9 83 F9 09 77 6B 48 8D 1D ?? ?? ?? ?? 48 63 C1 48 8B 1C C3 48 85 DB 74 58
```
`+0x10` rip-disp32 → object table `0x140E4B2E0`.

### `GetRange(idx,&start,&end)` (cmd `0x3A8`) — VA `0x140127CAF` / RVA `0x127CAF`
```
48 8D 8C 24 D8 02 00 00 8B 01 48 8B 51 08 4C 8B 41 10 FF C8 83 F8 09 77 2E 48 98 49 8B 8C C1 E0
```
No wildcards needed (the table base `0xE4B2E0` is an absolute disp in a SIB form and is part of the
literal bytes — the trailing `E0` begins it; extend to 36 bytes to capture `E0 B2 E4 00` if you want
to read it out).

### `.rpy` writer (cmd `0x3AC`) — VA `0x14012F840` / RVA `0x12F840`
```
44 89 44 24 18 56 41 55 48 83 EC 68 FF C9 45 8B E9 4C 8B C2 83 F9 09 0F 87 7A 07 00 00 48 8D 35
```
`+0x20..0x23` (the four bytes following the trailing `48 8D 35`) is the rip-disp32 → `0x140E4B2E0`.

### engine `Dispatch` — VA `0x140120CC0` / RVA `0x120CC0`
```
48 8B C4 89 48 08 48 89 50 10 4C 89 40 18 4C 89 48 20 53 55 56 57 41 54 41 55 41 56 41 57 48 81
```
No wildcards. (Or read the pointer at `0x140566C48` instead.)

### instant-replay init (`end − 10000`) — VA `0x1400C36AD` / RVA `0xC36AD`
```
44 89 2D ?? ?? ?? ?? C7 05 ?? ?? ?? ?? 01 00 00 00 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 44 8B 1D ?? ??
```
`+0x03` → live `0xE56668`; `+0x09` → `0x1403E6FD8`; `+0x12`,`+0x17` rel32 calls; `+0x1E` → end `0xE56658`.

### `ID_PLAY_PAUSE` handler — VA `0x1400BEEE6` / RVA `0xBEEE6`
```
48 8D 15 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? 85 C0 75 43 8D 70 01 39 05 ?? ?? ?? ?? 75 15 41 8B
```
`+0x03` → `"ID_PLAY_PAUSE"`; `+0x0B` rel32 strcmp; `+0x18` → speed `0xE56674`.

### replay mode init dispatcher — VA `0x1400C354C` / RVA `0xC354C`
```
83 E9 03 0F 84 89 00 00 00 83 E9 01 74 45 83 F9 01 0F 84 4A 01 00 00 44 89 2D ?? ?? ?? ?? C7 05
```
`+0x1A` → live `0xE56668`; `+0x20` → `0x1403E6FD8`.

### per-frame replay update (function start) — VA `0x1400C4271` / RVA `0xC4271`
```
0F 29 B4 24 80 2A 00 00 0F 29 BC 24 70 2A 00 00 BA B8 00 00 00 B9 D1 02 00 00 44 0F 29 84 24 60
```
No wildcards.

### replay UI command handler (function start) — VA `0x1400BDEEB` / RVA `0xBDEEB`
```
4C 8D 4C 24 40 4C 8D 05 ?? ?? ?? ?? 41 8B D7 B9 0A 01 00 00 FF 15 ?? ?? ?? ?? 8B 54 24 40 B9 2D
```
`+0x08` rip-disp32; `+0x16` rip-disp32 → the dispatch pointer `0x140566C48`.

---

## 7. Transport controls (bonus — all CONFIRMED)

All live in `0x1400BDEEB`, dispatched by control-name strcmp; each ends with
`call 0x1400BD590` (RVA `0xBD590`) = "refresh replay UI".

| Control | String | Handler VA / RVA | Effect |
|---|---|---|---|
| (rewind to start) | — | `0x1400BEDD9` / `0xBEDD9` | `cur = start` |
| `ID_REWIND` | `0x14033E4A0` | `0x1400BEDEC` / `0xBEDEC` | speed>0 → `-1`; else `speed--` (to `-16`) |
| `ID_REWIND_FRAME` | `0x14033E4B0` | `0x1400BEE4B` / `0xBEE4B` | `cur -= 30`, speed=0, slow=0 |
| `ID_REWIND_SLOW` | `0x14033E4C0` | `0x1400BEE83` / `0xBEE83` | slow=1, speed `-2` then `-3..-16` |
| `ID_PLAY_PAUSE` | `0x14033E4D0` | `0x1400BEEE6` / `0xBEEE6` | speed==0 → `1`; else `0` (+clear slow/rem) |
| `ID_ADVANCE_FRAME` | `0x14033E4E0` | `0x1400BEF3C` / `0xBEF3C` | `cur += 30`, speed=0, slow=0 |
| `ID_SLOW_MOTION` | `0x14033E4F8` | `0x1400BEF74` / `0xBEF74` | slow=1, speed `2` then `3..16` |
| `ID_FAST_FORWARD` | `0x14033E508` | `0x1400BEFCE` / `0xBEFCE` | speed<1 → `2`; else `speed++` (to `16`); blocked while `live` |
| `ID_REPLAY_SLIDER` | `0x14033E340` | `0x1400BF40F` / `0xBF40F` | seek, see §2.1 |

`ReplaySpeedInc` / `ReplaySpeedDec` (`0x14033EC00` / `0x14033EC10`) are only the *binding names*;
`0x1400CB9D6` is the key-config UI that prints them. They route to `ID_FAST_FORWARD` /
`ID_REWIND` via the input layer (INFERRED — the binding→command table was not traced).

---

## 8. Status summary & open items

| Item | Status |
|---|---|
| Clock at `0xE5666C`, int32 ms | **CONFIRMED** |
| Start `0xE56678`, End `0xE56658`, int32 ms | **CONFIRMED** |
| Speed `0xE56674` int32, 0 = paused, ±1..16, slow flag `0xE56660` | **CONFIRMED** |
| Live flag `0xE56668` | **CONFIRMED** |
| Markers `0xE56664` / `0xE5665C`, `-1` = unset | **CONFIRMED** |
| Millisecond unit | **CONFIRMED** (4 ways, §1.1) |
| 30 ms sample quantum / 33.33 Hz | **CONFIRMED** |
| Slider seek `0xBF40F`, formula | **CONFIRMED** |
| `Seek(idx,ms)` `0x12D440`, `GetRange` `0x127CAF` | **CONFIRMED** |
| Advance block `0xC4EE1`, hook `0xC4F37` / after-seek `0x1400C50B1` | **CONFIRMED** |
| Mode var `0x4C9194` values 0/1/2/3/4/5 | **CONFIRMED** |
| `.rpy` = `'RPY\0'` + version 10; no camera in file | **CONFIRMED** |
| Mode `4` ⇔ plugin API `m_iType == -1` | **INFERRED** — see §4.2 experiment |
| `ReplaySpeedInc/Dec` → `ID_FAST_FORWARD`/`ID_REWIND` routing | **INFERRED** |
| Object index `0x140E53DC8` — no writer found in `.text` | **UNKNOWN** |

### Runtime experiments that would close the gaps

1. **Object index `0x140E53DC8`.** No `mov [rip+…], reg` writes it anywhere in `.text`; it is
   likely populated through the engine's named-variable registration (cmd `0x104`/`0x3A7` family).
   Load a replay and read the dword; if it is `1`, hardcoding `1` is safe for single-session use.
   Otherwise breakpoint-on-write to find the setter.
2. **Plugin `m_iType == -1` mapping** (§4.2).
3. **Hook-site ordering.** Set a conditional breakpoint at `0x1400C50B1` and confirm it fires once
   per rendered frame, after `Dispatch(0x3AA,…)` and before the draw submission at `0x1400C5140+`.
   If the camera is consumed earlier than expected, move the hook to `0x1400C4F37`.
4. **Scrub round-trip.** Write `start + 5000` to `0x140E5666C` while paused; the UI slider should
   jump to `(int)(5000/(end-start)*1000)` and the scene should show the frame at `+5 s`. That
   single test validates the unit, the clamps, and the write-to-scrub approach in one shot.
