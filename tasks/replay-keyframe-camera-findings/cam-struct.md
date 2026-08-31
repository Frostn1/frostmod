# MX Bikes — camera struct layout recovered from the `cameras.cfg` parsers

Target: `mxbikes.exe.unpacked.exe`, x64, image base `0x140000000`.
All addresses given as **VA (RVA)**. RVA = VA − 0x140000000.

Everything marked **[C]** is CONFIRMED by disassembly (and cross-checked by at
least two independent code paths). **[I]** is INFERRED.

---

## 0. TL;DR — the camera element

```c
// stride 0x3c (60 bytes) — bike cameras, from bikes\<bike>\cameras.cfg
struct BikeCamera {           //                cfg key
/*0x00*/ char  name[0x18];    // camera%d/name      (24-byte buffer, unbounded strcpy)
/*0x18*/ int32 part;          // camera%d/part      0 = part-matrix A, 1 = part-matrix B, >=2 = world
/*0x1c*/ float position[3];   // camera%d/position  metres, in the part's local frame
/*0x28*/ float rotation[3];   // camera%d/rotation  DEGREES, EULER. only [0],[1] come from cfg;
                              //                    [2] is force-zeroed and never applied
/*0x34*/ float fov;           // camera%d/fov       DEGREES (engine default 60.0)
/*0x38*/ int32 gyro;          // camera%d/gyro      0/1 — re-level the horizon
};                            // sizeof == 0x3c

// stride 0x34 (52 bytes) — helmet cameras, from rider\helmets\<helmet>\cameras.cfg
struct HelmetCamera {
/*0x00*/ char  name[0x18];    // camera%d/name
/*0x18*/ float position[3];   // camera%d/position
/*0x24*/ float rotation[3];   // camera%d/rotation  same rule: [0],[1] parsed, [2] forced 0
/*0x30*/ float fov;           // camera%d/fov
};                            // sizeof == 0x34
```

`rotation[0]` → rotate about **X** (pitch), `rotation[1]` → rotate about **Y**
(yaw/heading). Applied as `M = RotY(rot[1]) · RotX(rot[0])`, then translated by
`position`, then pre-multiplied by the parent part matrix. **[C]**

Max 10 cameras per file in every parser (`cmp <i>, 0xa`). Index `%d` is 0-based.
Enumeration stops at the first index whose `camera%d/name` key is absent. **[C]**

---

## 1. Parse sites

### 1a. `camera%d/*` — 4 xref sites, 2 logical parsers × 2 files each

| function VA (RVA) | role | file | destination |
|---|---|---|---|
| `0x14011ad40` (`0x11ad40`) | **loader A**, whole function | *both* cfgs | global slot table |
| ├ chunk `0x14011ae08` (`0x11ae08`) | bike half of loader A | `%sbikes\%s\cameras.cfg` (arg `rdx`) | `slot + 0xf4 + i*0x3c` |
| └ chunk `0x14011af8b` (`0x11af8b`) | helmet half of loader A | `%srider\helmets\%s\cameras.cfg` (arg `r8`) | `slot + 0x35c + i*0x34` |
| `0x14011d090` (`0x11d090`) | **loader B**, bike | one cfg path (arg `rdx`) | pooled record `+0x20 + i*0x3c` |
| `0x14011d260` (`0x11d260`) | **loader B**, helmet | one cfg path (arg `rdx`) | pooled record `+0x288 + i*0x34` |

`.pdata` splits loader A into three chunks (`0x11ad40..0x11ae08`,
`0x11ae08..0x11af8b`, `0x11af8b..0x11b0d7`); it is one function. **[C]**

Signatures **[C]**:
```c
int load_cameras_into_slot(int* out_slot_idx, const char* bike_cfg, const char* helmet_cfg); // 0x14011ad40
int load_bike_cameras_record  (int* out_rec_id, const char* cfg_path);                       // 0x14011d090
int load_helmet_cameras_record(int* out_rec_id, const char* cfg_path);                       // 0x14011d260
int copy_records_into_slot(int* out_slot_idx, int bike_rec_id, int helmet_rec_id);           // 0x14011d3d0
```
Return 0 on success, 1 on failure (no free slot / cfg not openable).

Proof that `rdx` is the bike file and `r8` the helmet file: the call site at
`0x14005dbd2` (`0x5dbd2`) sprintf's `"%sbikes\%s\cameras.cfg"` into one buffer and
`"%srider\helmets\%s\cameras.cfg"` into another, then dispatches VM function id
`0x18b` whose binding at `0x140123606` forwards `arg0→rcx, arg1→rdx, arg2→r8`
into `0x14011ad40`. **[C]**

### 1b. `camset%d/*` — track/TV camera sets, 2 sites

| function VA (RVA) | role |
|---|---|
| `0x14011b420` (`0x11b420`) | opens the cfg, reads `numcamset`, then falls through |
| `0x14011b4b3` (`0x11b4b3`) | the camset + camset-camera loop (all `camset%d/...` keys) |

These are two `.pdata` chunks of one function; `0x11b420` is entered from the
VM binding at `0x1401235df`.
`load_camsets(const char* cfg /*rcx*/, int /*edx*/, void* /*r8*/, int /*r9d*/, float /*[rsp+0x20]*/)`.

---

## 2. Config-read primitives (reusable oracles)

All are varargs wrappers: they `vsprintf` the key from `fmt` + the `%d` args into
a stack buffer, then call the worker. Return **0 = found, 1 = missing**. **[C]**

| VA (RVA) | signature | notes |
|---|---|---|
| `0x1401569f0` (`0x1569f0`) | `int cfg_open(int* out_handle, const char* path)` | handle is a 1-based index into a 10-slot table at `0x140566c60`, stride `0x118` |
| `0x1401573a0` (`0x1573a0`) | `void cfg_close(int handle)` | |
| `0x140156eb0` (`0x156eb0`) | `int cfg_get_str(int h, char* out, const char* fmt, ...)` | **unbounded** copy into `out` |
| `0x140157060` (`0x157060`) | `int cfg_get_int(int h, int* out, const char* fmt, ...)` | worker `0x140156f50`, count fixed to 1 |
| `0x1401572b0` (`0x1572b0`) | `int cfg_get_float(int h, float* out, const char* fmt, ...)` | worker `0x140157150`, count fixed to 1 |
| `0x140157330` (`0x157330`) | `int cfg_get_floats(int h, float* out, int count, const char* fmt, ...)` | **`count` is the element count — this is how position=3 / rotation=2 is proven** |

Workers:
- `0x140156bf0` (`0x156bf0`) — `int cfg_lookup(int h_minus1, char* out, const char* key)`.
  Splits `key` on `/` (max 100 chars per segment, `0x64`-byte temps), walks
  `{`/`}` sections, splits `name = value` on `=`, strips at `;`. `_stricmp` compare.
- `0x140157150` (`0x157150`) — `memset(out, 0, count*4)`, lookup, then split the
  value on `,` and `_atof_l` each field, `cvtsd2ss` → `movss [out + i*4]`.
- `0x140156f50` (`0x156f50`) — the integer equivalent.

CRT identifications used above: `0x1402ab3fc = sin`, `0x1402ab402 = cos`,
`0x1402ab450 = atan`, `0x1402ab3f6 = atan2`, `0x1402ab3d6 = _atof_l`,
`0x1402ab340 = memset`, `0x1402aae86 = vsprintf`, `0x1402aae9e = strchr`,
`0x1402ab4b6 = _stricmp`. All resolved through the IAT. **[C]**

---

## 3. Exact parse-to-offset trace (bike half of loader A, `0x14011ae08`)

Base register expression is `rdi + r14 + K` where `r14` = table base
`0x140550260`, `rdi = slot*0x574 + i*0x3c`. Camera element base = `+0xf4`.

| insn VA | call | out ptr | key | elem offset | type |
|---|---|---|---|---|---|
| `0x14011aec0` | `cfg_get_str`    | `+0xf4`  | `camera%d/name`     | `+0x00` | `char[0x18]` |
| `0x14011aee3` | `cfg_get_int`    | `+0x10c` | `camera%d/part`     | `+0x18` | `int32` |
| `0x14011af05` | `cfg_get_floats` | `+0x110`, **count=3** | `camera%d/position` | `+0x1c` | `float[3]` |
| `0x14011af27` | `cfg_get_floats` | `+0x11c`, **count=2** | `camera%d/rotation` | `+0x28` | `float[2]` of a `float[3]` |
| `0x14011af42` | `mov [..],0`     | `+0x124` | — (hard-zero)       | `+0x30` | `float` = `rotation[2]` |
| `0x14011af46` | `cfg_get_float`  | `+0x128` | `camera%d/fov`      | `+0x34` | `float` |
| `0x14011af61` | `cfg_get_int`    | `+0x12c` | `camera%d/gyro`     | `+0x38` | `int32` |
| `0x14011af66` | `inc [+0xf0]`    | | camera count | | |
| `0x14011af70` | `add r13, 0x3c`  | | **stride = 0x3c** | | |

Helmet half (`0x14011af8b`), element base `+0x35c`, same pattern:
name `+0x00`, position `+0x18` (count 3), rotation `+0x24` (count 2),
hard-zero `+0x2c`, fov `+0x30`, `add rbp, 0x34` → **stride = 0x34**.

Loader B (`0x14011d090` / `0x14011d260`) reproduces the *identical* relative
offsets and strides against a different container — independent confirmation #2.

Independent confirmation #3: the record→slot copier `0x14011d414` (`0x11d414`)
copies each bike camera as `strcpy(name)` + **9 consecutive dwords** covering
`+0x18 … +0x38`, and each helmet camera as `strcpy(name)` + **7 consecutive
dwords** covering `+0x18 … +0x30`. That is what proves `rotation` is a
contiguous `float[3]` and not two floats plus an unrelated field. **[C]**

> Buffer-overflow note **[C]**: `cfg_get_str` copies without a length limit into a
> 24-byte `name`. A name longer than 23 chars overwrites `part`/`position`.

---

## 4. Container layouts

### 4a. Global camera slot table **[C]**
`g_camera_slots @ 0x140550260 (RVA 0x550260)`, **50 entries × 0x574 bytes**
(bounds `0x140550260 .. 0x140561308`, exactly 50 × 0x574).

```c
struct CameraSlot {            // 0x574
/*0x000*/ int32  in_use;
/*0x004*/ int32  a;                 // set by 0x14011b0e0
/*0x008*/ float  bike_world_pos[3]; // used by autozoom
/*0x014*/ int32  surface_id;        // matched against camset.surface
/*0x018*/ float  size[3];           // "size/x","size/y","size/z"  -> autozoom subject size
/*0x024*/ float  part_matrix_A[16]; // parent for camera.part == 0
/*0x064*/ float  part_matrix_B[16]; // parent for camera.part == 1
/*0x0a4*/ float  head_matrix[16];   // parent for helmet cameras
/*0x0e4*/ float  ref_point[3];      // copied straight to out+0x44
/*0x0f0*/ int32  num_bike_cameras;
/*0x0f4*/ BikeCamera   bike[10];    // 10 * 0x3c = 0x258
/*0x34c*/ float  ref_rot[3];        // "ref_rot/x","ref_rot/y","ref_rot/z"  (degrees)
/*0x358*/ int32  num_helmet_cameras;
/*0x35c*/ HelmetCamera helmet[10];  // 10 * 0x34 = 0x208
/*0x564*/ float  center[3];         // "center/x","center/y","center/z"
/*0x570*/ float  track_pos;         // compared against camset limit/start,limit/end
};                                  // 0x574 total
```

### 4b. Pooled parse record (loader B) **[C]**
`g_camera_record_pool @ 0x140550258 (RVA 0x550258)`, allocated by
`pool_alloc(void** list, int* out_id, int size)` at `0x14015c040` (`0x15c040`),
size `0x490`, lookup by id via `0x14015c270`.

```c
struct CameraRecord {          // 0x490
/*0x000*/ int32  kind;         // 0 = bike file, 1 = helmet file
/*0x004*/ float  center[3];
/*0x010*/ float  size[3];
/*0x01c*/ int32  num_bike;
/*0x020*/ BikeCamera   bike[10];
/*0x278*/ float  ref_rot[3];
/*0x284*/ int32  num_helmet;
/*0x288*/ HelmetCamera helmet[10];
};                             // 0x490
```

### 4c. Track camera sets **[C]**
`g_numcamset (int32) @ 0x14055024c (RVA 0x55024c)`,
`g_camsets (ptr) @ 0x140550250 (RVA 0x550250)`, element stride **0x40**.

```c
struct CamSet {                 // 0x40
/*0x00*/ int32 unk0;
/*0x04*/ char  name[0x20];      // camset%d/name
/*0x24*/ int32 surface;         // camset%d/surface
/*0x28*/ int32 num_cameras;     // camset%d/numcameras
/*0x2c*/ int32 pad;
/*0x30*/ CamSetCamera* cameras; // malloc(num * 0x74), memset 0
/*0x38*/ int32 active_camera;   // runtime
};

struct CamSetCamera {           // 0x74
/*0x00*/ int32 type;            // camset%d/camera%d/type  0..3
/*0x04*/ byte  runtime[0x20];   // never parsed
/*0x24*/ float pos[3];          // camset%d/camera%d/pos      (count 3)
/*0x30*/ float rot_x;           // camset%d/camera%d/rot [1]  DEGREES, pitch  (types 1,2,3)
/*0x34*/ float rot_y;           // camset%d/camera%d/rot [0]  DEGREES, yaw    (all types)
/*0x38*/ float pad;
/*0x3c*/ float fov;             // camset%d/camera%d/fov      DEGREES
/*0x40*/ float pos2[3];         // camset%d/camera%d/pos2     (count 3, types 2,3)
/*0x4c*/ float center[3];       // camset%d/camera%d/center   (count 3, type 3)
/*0x58*/ int32 limit_enable;    // camset%d/camera%d/limit/enable
/*0x5c*/ float limit_start;     // camset%d/camera%d/limit/start   (track position)
/*0x60*/ float limit_end;       // camset%d/camera%d/limit/end
/*0x64*/ int32 autozoom_enable; // camset%d/camera%d/autozoom/enable
/*0x68*/ float autozoom_ref;    // camset%d/camera%d/autozoom/reference
/*0x6c*/ float autozoom_min;    // camset%d/camera%d/autozoom/min   DEGREES
/*0x70*/ float autozoom_max;    // camset%d/camera%d/autozoom/max   DEGREES
};                              // 0x74
```

Note the **transposed store**: for types 1/2/3 the parser reads `rot` as 2 floats
into a stack pair and then writes `value[0] → +0x34` and `value[1] → +0x30`
(`0x14011b666..0x14011b685`, `0x14011b6b8..0x14011b6c9`). So the cfg order is
`rot = yaw, pitch` — the **opposite** of the bike/helmet `rotation = pitch, yaw`.
For `type == 0` a *single* float is read straight into `+0x34` (yaw only). **[C]**

Type dispatch in the parser (`0x14011b5d0`): 0 → `rot` scalar; 1 → `rot` vec2;
2 → `rot` vec2 + `pos2`; 3 → `center` vec3 + `pos2` vec3 (no `rot`).

---

## 5. Rotation representation — the SLERP question

**Euler angles, in DEGREES, two of three axes, XY order (no roll). Not a
quaternion, not a matrix.** **[C]**

Evidence chain:

1. Storage is 3 floats (`float[3]`), of which only `[0]` and `[1]` are ever
   written from the cfg; `[2]` is stored as literal 0 and **never read**.
2. The consumer `0x14011c060` (`0x11c060`) —
   `int camera_build_view(int camera_index, int slot, CamOut* out)` — does, for
   the bike branch at `0x14011c268`:
   ```
   mat4_identity(M)
   mat4_rotY_deg_post(M, cam.rotation[1])   ; 0x14011c290 -> 0x140266900
   mat4_rotX_deg_post(M, cam.rotation[0])   ; 0x14011c2a6 -> 0x140266820
   mat4_translate  (M, cam.position[0..2])  ; 0x14011c2ce -> 0x140266d60
   M = ParentMatrix * M                     ; 0x14011c2fb / 0x14011c31a
   ```
   Helmet branch at `0x14011c155` is identical (plus a `ref_rot` XYZ pre-rotation
   through `0x140266ac0 / 0x140266ba0 / 0x140266c80` and the head matrix).
   **No rotZ call exists on either path** — `rotation[2]` is dead.
3. The rotation builders are unambiguous:

   | VA (RVA) | signature | matrix | multiply |
   |---|---|---|---|
   | `0x140266820` (`0x266820`) | `mat4_rotX_deg(mat4* m, float deg)` | `[1 0 0; 0 c −s; 0 s c]` | `m = m·R` |
   | `0x140266900` (`0x266900`) | `mat4_rotY_deg(mat4* m, float deg)` | `[c 0 s; 0 1 0; −s 0 c]` | `m = m·R` |
   | `0x1402669e0` (`0x2669e0`) | `mat4_rotZ_deg(mat4* m, float deg)` | `[c −s 0; s c 0; 0 0 1]` | `m = m·R` |
   | `0x140266ac0` (`0x266ac0`) | `mat4_rotX_deg_pre` | same as X | `m = R·m` |
   | `0x140266ba0` (`0x266ba0`) | `mat4_rotY_deg_pre` | same as Y | `m = R·m` |
   | `0x140266c80` (`0x266c80`) | `mat4_rotZ_deg_pre` | same as Z | `m = R·m` |

   Each one does `cvtps2pd; mulsd [0x1403539e0]; call cos; call sin`, and
   `*(double*)0x1403539e0 == 0.0174532925199433 == π/180`. **Degrees, confirmed
   numerically.**
4. Supporting math primitives (all verified by reading them):
   `0x140266390` `mat4_identity` · `0x1402663e0` `mat4_mul(out,a,b)` ·
   `0x1402665f0` `mat4_copy` (64 bytes) · `0x140266750` `mat4_inverse_rigid`
   (transpose + negate translation) · `0x140266d60` `mat4_translate(m,x,y,z)`
   (adds into `m[3],m[7],m[11]` — translation lives in **column 3**) ·
   `0x140267ce0` `vec3_set` · `0x140267cb0` `vec3_sub` · `0x140267ae0` `vec3_cross` ·
   `0x140267c00` `vec3_normalize`.

**Consequence for a camera-path tool:** a camera orientation here is only 2 DOF
(pitch, yaw) in degrees, with roll structurally absent. You *can* build a
quaternion from `Ry(yaw)·Rx(pitch)` and SLERP it, and the result stays inside the
representable set as long as you do not introduce roll — but you cannot feed a
rolled orientation back into `cameras.cfg`, and interpolating the two Euler
scalars directly is *not* equal to SLERP (it is fine for small deltas, and yaw
needs ±180° wrap handling). The engine itself never slerps: it rebuilds the
matrix from the two angles every frame.

### Axis convention **[C]**
From the `gyro` block (`0x14011c32d..0x14011c499`): the code takes the camera's
local `(0,0,1)` as **forward**, uses world `(0,1,0)` as **up**, computes
`right = up × forward` then `up' = forward × right`, and re-packs them into
columns 0/1/2 of the matrix. So the matrix is row-major, column-vector
(`M·v`), local **+X = right, +Y = up, +Z = forward**, and the world is **Y-up**.
Therefore `rotation[0]` (rotX) is **pitch** and `rotation[1]` (rotY) is
**yaw/heading**. `gyro != 0` replaces the camera basis with a horizon-levelled
one built the same way.

---

## 6. FOV units

**Degrees. [C]** Three independent proofs:
- Failure path stores the literal default `0x42700000 = 60.0f` into the output
  fov (`0x14011c136`).
- The autozoom path (`0x14011cd47..0x14011ce34`) computes
  `fov = atan( (size.x+size.y+size.z)/3 / (2 · autozoom_reference · dist) ) · 114.5915590112021`
  where `114.5915590112021 == 2·180/π`, then clamps to
  `[autozoom_min, autozoom_max]` — so those two cfg keys are degrees too.
- The camset type-3 look-at path uses `atan2` × `57.295780181884766` (= 180/π).

---

## 7. Output struct of `camera_build_view` **[C]**

```c
struct CamOut {          // 0x50
/*0x00*/ float view[16]; // world transform of the camera (column-vector, Y-up)
/*0x40*/ float fov;      // degrees
/*0x44*/ float ref[3];   // copied from slot+0xe4
};
```
`int camera_build_view(int camera_index /*ecx*/, int slot /*edx*/, CamOut* out /*r8*/)`
at **`0x14011c060` (RVA `0x11c060`)**. `camera_index` is 1-based; `0` means
"auto-select the camset whose `surface` matches `slot.surface_id`". The index
space is concatenated: `[0, numcamset)` → track camsets, then
`[.., num_bike_cameras)` → bike cameras, then helmet cameras. Same ordering in
the name getter `0x14011cf70` (`0x11cf70`),
`int camera_get_name(int slot, int index, char* out)`.

Other useful entry points:
- `0x14011b0e0` (`0x11b0e0`) — `camera_slot_update(int slot, int a, int pos[3], int surface, const mat4* partA, const mat4* partB, const mat4* head, const float ref[3], float track_pos)`; NULL matrix args become identity.
- `0x14011b940` (`0x11b940`) — camset camera evaluation (distance² + orientation) per type.

---

## 8. Confidence summary

CONFIRMED by disassembly, cross-checked ≥2 ways:
- both camera struct layouts, every field offset, both strides (0x3c / 0x34)
- `rotation` is `float[3]`, only 2 parsed, 3rd forced 0 and never applied
- degrees for `rotation`, `ref_rot`, `fov`, `autozoom/min`, `autozoom/max`
- the config-read primitive set and their `count` semantics
- axis convention (Y-up world, +X/+Y/+Z = right/up/forward local)
- which parse site handles the bike file vs the helmet file
- camset / camset-camera layout and the transposed `rot` store
- `CameraSlot` (0x574) and `CameraRecord` (0x490) container layouts

INFERRED (not proven):
- the meaning of `part` values beyond "selects parent matrix A / B / none" —
  matrix A is `slot+0x24`, matrix B is `slot+0x64`; which physical bike body each
  corresponds to is set by the caller of `0x14011b0e0` and was not traced
- `CamSetCamera +0x04..0x24` is runtime scratch (it is memset-0 at load and never
  parsed; not every writer was traced)
- `CameraSlot +0x004` semantics
