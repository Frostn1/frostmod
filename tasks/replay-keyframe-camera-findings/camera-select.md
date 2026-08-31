# MX Bikes — Camera Selection System (static RE)

Target: `/Users/seandahan/Downloads/mxbikes.exe.unpacked.exe` (x64 PE, Steamless-unpacked)
Image base `0x140000000`. **RVA = VA − 0x140000000** throughout.
Method: static only (capstone + pefile via `re_tools.py`). Nothing was executed.

---

## 0. Executive summary

The plugin entry point paid off exactly as hoped. The chain is:

```
game state update  ──► Sim_Dispatch(msgId=0x3C9, nCam, pCamData, curSel, &sel)   [variadic router]
   0x1400C3B8C            0x140120CC0
                            └─► Plugins_SpectateCameras(...)   0x14012BA60
                                  └─► for each plugin: call plugin[i].SpectateCameras   (+0xF0)
```

The three globals a cinematic mod wants:

| Meaning | VA | RVA | Type |
|---|---|---|---|
| **ACTIVE CAMERA INDEX** | `0x1404CA2A0` | `0x4CA2A0` | `int` |
| **ACTIVE VEHICLE (rider) INDEX** | `0x1404CA2A8` | `0x4CA2A8` | `int`, raw slot 0..49 |
| **CAMERA COUNT `N`** (onboard cams for active bike) | `0x1404C91D4` | `0x4C91D4` | `int` |

There is **no camera array global**. `_pCameraData` is a *stack buffer built fresh on every call*, and its
element is **not a fixed-stride struct** — it is a run of packed NUL-terminated ASCII names. That is the
single most important structural finding here and it is CONFIRMED by three independent code paths.

---

## 1. Plugin function-pointer table (CONFIRMED)

### 1.1 Where the exports are resolved

Loader function: **VA `0x14012A4F0` / RVA `0x12A4F0`**
`GetProcAddress` IAT slot: `0x140321168` (RVA `0x321168`); `FreeLibrary` slot `0x140321160`.

Handshake before any callback is resolved:
1. `GetInterfaceVersion()` must return **9**  (`cmp eax, 9` @ `0x14012A539`)
2. `GetModID()` must `strcmp`-equal a fixed id string
3. `GetModDataVersion()` must equal a caller-supplied value (`esi`)

Only then are the 29 callbacks resolved and the plugin appended to the array.

### 1.2 The plugin record — `sizeof = 0x118` (CONFIRMED, stride visible at `imul edx,edx,0x118` @ `0x14012A874` and `add rdi,0x118` in every dispatch loop)

| Offset | Field | Notes |
|---|---|---|
| `0x000` | `HMODULE hModule` | from LoadLibrary of the `.dlo` |
| `0x008` | `int iModType` | from `Startup()` return: `-1`→reject, `1`/`2`/`3` stored verbatim, anything else → `0` |
| `0x010` | `Startup` | called at `0x14012A7C7` with the mod-data pointer |
| `0x018` | `Shutdown` | |
| `0x020` | `EventInit` | |
| `0x028` | `EventDeinit` | |
| `0x030` | `RunInit` | |
| `0x038` | `RunDeinit` | |
| `0x040` | `RunStart` | |
| `0x048` | `RunStop` | |
| `0x050` | `RunLap` | |
| `0x058` | `RunSplit` | |
| `0x060` | `RunTelemetry` | |
| `0x068` | `RaceEvent` | |
| `0x070` | `RaceDeinit` | |
| `0x078` | `RaceAddEntry` | |
| `0x080` | `RaceRemoveEntry` | |
| `0x088` | `RaceSession` | |
| `0x090` | `RaceSessionState` | |
| `0x098` | `RaceLap` | |
| `0x0A0` | `RaceSplit` | |
| `0x0A8` | `RaceSpeed` | |
| `0x0B0` | `RaceHoleshot` | |
| `0x0B8` | `RaceCommunication` | |
| `0x0C0` | `RaceClassification` | |
| `0x0C8` | `RaceTrackPosition` | |
| `0x0D0` | `RaceVehicleData` | |
| `0x0D8` | **`Draw`** | |
| `0x0E0` | `TrackCenterline` | |
| `0x0E8` | **`SpectateVehicles`** | |
| `0x0F0` | **`SpectateCameras`** | last GetProcAddress in the chain |
| `0x0F8` | `int numSprites` | from `DrawInit` |
| `0x100` | `int* spriteIds` | game-side texture handles, one per sprite name |
| `0x108` | `int numFonts` | from `DrawInit` |
| `0x110` | `int* fontIds` | game-side font handles |

**Bonus export not in the header you supplied:** `DrawInit` — resolved *lazily* in a second pass at
VA `0x14012A250` / RVA `0x12A250`, signature recovered as
`int DrawInit(int* pNumSprites, char** ppSpriteNames, int* pNumFonts, char** ppFontNames)`,
returns 0 on success; the name lists are packed NUL-terminated strings (same packing idiom as the camera
payload). Name string at `0x140342980`.

The resolution order above **is** the complete supported callback list, in order.

### 1.3 Plugin array globals (CONFIRMED)

| Global | VA | RVA |
|---|---|---|
| `g_iNumPlugins` (`int`) | `0x140565CB8` | `0x565CB8` |
| `g_pPlugins` (`SPlugin*`, `realloc`'d) | `0x140565CC0` | `0x565CC0` |

Registration tail @ `0x14012A85E`:
```
mov  edx,[g_iNumPlugins]; inc edx; imul edx,edx,0x118 ; realloc
mov  [g_pPlugins],rax
memcpy(g_pPlugins + n*0x118, localRecord, 0x118)
mov  [g_iNumPlugins], n+1
```

### 1.4 Per-callback dispatch wrappers

Each callback has its own tiny wrapper that walks `g_pPlugins`. The two that matter:

| Wrapper | VA | RVA | Behaviour |
|---|---|---|---|
| `Plugins_SpectateVehicles` | `0x14012B9C0` | `0x12B9C0` | loops all plugins, `call [plugin+0xE8]`, **stops at the first plugin returning 1** and returns 1 |
| `Plugins_SpectateCameras` | `0x14012BA60` | `0x12BA60` | identical, `call [plugin+0xF0]` |
| `Plugins_TrackCenterline` | `0x14012B920` | `0x12B920` | 3-arg, no early-out |
| `Plugins_Shutdown` | `0x14012B510` | `0x12B510` | also frees the array |

Loop body (SpectateCameras) — note the callback pointer is null-checked per plugin:
```
0x14012BAA0  mov  rax,[g_pPlugins]
0x14012BAA7  mov  r11,[rdi+rax+0xF0]      ; rdi = i*0x118
0x14012BAAF  test r11,r11 / je next
0x14012BAC0  call r11                      ; (ecx=nCam, rdx=pData, r8d=curSel, r9=piSelect)
0x14012BAC3  cmp  eax,1 / je  return 1
0x14012BAD1  add  rdi,0x118
```

---

## 2. The engine message router (CONFIRMED)

All 29 plugin wrappers, plus ~970 other engine services, are reached through one **variadic dispatcher**:

* `Sim_Dispatch(int msgId, ...)` — **VA `0x140120CC0` / RVA `0x120CC0`**
* Bound check `cmp ecx, 0x3CA` @ `0x140120CFB`; jump table at **RVA `0x1280B0`** (971 image-relative dwords, `rbx`-based).
* Reached only through a global function pointer, **`0x140566C48` / RVA `0x566C48`**, initialised by
  `lea rax,[rip-0x1335D]` @ `0x140134016` (and a second path @ `0x140134596`) inside `0x140133FA0`.
  11,195 call sites go through that one slot. There are **zero** direct `call 0x140120CC0` and zero data
  pointers to it in the file image — so a mod must either call `0x140120CC0` directly or read the slot.

Message IDs relevant here:

| ID | Meaning | Case body |
|---|---|---|
| `0x101` | resolve localisation key → string | `0x1401256C4` |
| `0x10A` | begin menu row / get row handle | `0x140125771` |
| `0x12A` / `0x12B` / `0x12D` | menu list add-item / set-value / clear | `0x140125AB9` / `0x140125AD1` / `0x140125B01` |
| `0x18D` | **get camera count for a vehicle model** | `0x140123653` |
| `0x18E` | **get camera name by index** | `0x14012368D` |
| `0x2D0` | key *pressed this frame* (DIK scancode) | `0x140125C35` |
| `0x2D1` | key *held* (DIK scancode) | `0x140125C49` |
| `0x3C7` | `TrackCenterline` | `0x140127FEB` |
| `0x3C8` | **`SpectateVehicles`** | `0x140128008` |
| `0x3C9` | **`SpectateCameras`** | `0x140128026` |

---

## 3. `SpectateCameras` call site — THE PRIZE (CONFIRMED)

**Calling function: VA `0x1400C3B8C` / RVA `0xC3B8C`** (range `0x1400C3B8C..0x1400C4271`).
This is the *spectate / replay* per-frame update: it builds the rider list, builds the camera list,
offers both to plugins, then processes the free-camera keyboard input.
It is reached through a computed state-machine dispatch — it has no direct callers.

### 3.1 The call

```
0x1400C3F1E  mov  r9d, [0x1404CA2A0]        ; _iCurSelection  <- ACTIVE CAMERA INDEX
0x1400C3F25  mov  edx, [rsp+0x70]           ; _iNumCameras
0x1400C3F29  lea  rax, [rsp+0xA0]           ; _piSelect       (stack local)
0x1400C3F31  lea  r8,  [rsp+0xD10]          ; _pCameraData    (stack buffer)
0x1400C3F39  mov  ecx, 0x3C9
0x1400C3F3E  mov  [rsp+0x20], rax
0x1400C3F43  call [0x140566C48]             ; Sim_Dispatch
```

### 3.2 `_iNumCameras` — traced back

`[rsp+0x70]` is a **stack local**, filled at `0x1400C3D5F`:

```
0x1400C3D2A  movsxd rax,[0x1404CA2A8]              ; ACTIVE VEHICLE INDEX
0x1400C3D38  js   -> no-vehicle branch
0x1400C3D3E  imul rax,rax,0x5B24                   ; vehicle array stride
0x1400C3D45  cmp  [rax+rbp+0x270], 1               ; entry has a loaded bike?
0x1400C3D53  mov  edx,[rax+rbp+0x50A8]             ; vehicle MODEL id
0x1400C3D5A  lea  r8,[rsp+0x70]
0x1400C3D5F  mov  ecx,0x18D  / call Sim_Dispatch   ; -> N
...
0x1400C3EC3  add  dword [rsp+0x70], 4              ; count = N + 4
   (else)
0x1400C3ECA  mov  dword [rsp+0x70], 2              ; no bike -> count = 2
```

**The same value is mirrored into a global** by the on-track path (`0x1400C52EA`, `0x1400C5411`):

> **`NUM_CAMERAS` (N, excluding the 4 synthetic entries) = VA `0x1404C91D4` / RVA `0x4C91D4`.**

`N` itself is `trackCams + bikeCams + helmetCams` (see §5.3).

### 3.3 `_pCameraData` — traced back → **packed NUL-terminated names, not a struct array**

`[rsp+0xD10]` is a scratch stack buffer. Each name is produced into `[rsp+0x28C0]`, then appended at a
running byte cursor `ebx`, which advances by `strlen+1` after each append (`repne scasb` + `not rcx`).
Append idiom, seen 4× verbatim in this function:

```
lea  rcx,[rsp+0x28C0]
movsxd rax, ebx                ; cursor
sub  rax, rcx
lea  rdx,[rsp+0x28C0]
lea  rcx,[rsp+rax+0xD10]       ; == cameraBuf + ebx - (rsp+0x28C0)   (strength-reduced)
copy loop: dest = cameraBuf + ebx + i
...
repne scasb ; ebx += strlen+1
```

So:

```c
// _pCameraData layout  (CONFIRMED)
// char names[];  // _iNumCameras consecutive NUL-terminated ASCII strings, no padding,
//                // no length prefix, VARIABLE stride. Walk with strlen()+1.
```

Build order (CONFIRMED — matches the ESC-menu label switch at `0x1400BE27A` exactly):

| index | content | source |
|---|---|---|
| `0` | localised `cc_autocamset` | msg `0x101`, key at `0x14033E3E0` |
| `1 .. N` | onboard camera names | msg `0x18E`, index `i-1` |
| `N+1` | localised `cc_orbitcam` | key at `0x14033E3F0` |
| `N+2` | localised `cc_freecam` | key at `0x14033E400` |
| `N+3` | localised `cc_freeroamcam` | key at `0x14033E410` |

Total `= N + 4`. When no bike is loaded, only indices `0` (`cc_autocamset`) and `1` (`cc_freeroamcam`)
exist and `_iNumCameras == 2`.

The names are **localised display strings**, so they change with the language file. The stable identity of
an entry is its *index*, not its text.

### 3.4 `_iCurSelection` / `_piSelect` write-back

```
0x1400C3F49  mov   ecx,[0x1404CA2A0]         ; current
0x1400C3F56  cmp   eax,1                     ; did any plugin claim it?
0x1400C3F65  cmove ecx,[rsp+0xA0]            ; take plugin's *_piSelect
0x1400C3F6D  mov   [0x1404CA2A0], ecx        ; <-- ACTIVE CAMERA INDEX
```

> **ACTIVE CAMERA INDEX = VA `0x1404CA2A0` / RVA `0x4CA2A0`.** Read *and* written here; a mod can drive it
> either by returning 1 from `SpectateCameras` or by writing the global directly.
> Note the read at `0x1400C3F1E` and the write at `0x1400C3F6D` hit the same dword — writing it from
> another thread races this function, so prefer the callback.

---

## 4. `SpectateVehicles` call site (CONFIRMED)

Same function (`0x1400C3B8C`), immediately above the camera block.

```
0x1400C3BBC  mov  r13d,[0x1404CA2A8]         ; ACTIVE VEHICLE INDEX (raw slot)
0x1400C3BE7  lea  rdi,[0x140F54938]          ; = vehArray + 0x5B18
0x1400C3C00  lea  r15,[0x141071640]          ; = vehArrayEnd + 0x5B18
loop @0x1400C3C07:
  cmp  [rdi-0x5B18], 0        ; entry+0x00 == 0 -> slot empty, skip
  mov  ecx,[rdi]              ; entry+0x5B18 = index into the race-entry list
  call 0x140117D60            ; fills {int raceNum; char name[0x20]; ...} at [rsp+0xA80]
  mov  [r12], raceNum         ; dest+0x00
  strcpy(dest+4, name)        ; dest+0x04
  cmp  ebp, r13d / cmove ebx, esi   ; ebx = COMPACTED index of the active rider
  inc  esi ; add r12,0x68 ; add rdi,0x5B24
0x1400C3C78:
  lea  rax,[rsp+0xA0]         ; _piSelect
  lea  r8, [rsp+0x1470]       ; _pVehicleData
  mov  r9d, ebx               ; _iCurSelection  (COMPACTED)
  mov  edx, esi               ; _iNumVehicles   (COMPACTED count)
  mov  ecx, 0x3C8 / call Sim_Dispatch
```

`_pVehicleData` element stride = **`0x68`** (`add r12,0x68`) = `SPluginsSpectateVehicle_t {int m_iRaceNum; char m_szName[100];}` ✔ matches the SDK header.

Write-back (`0x1400C3CA4`…`0x1400C3CDA`) re-walks the array to convert the plugin's **compacted** index
back to the **raw slot**:

```
0x1400C3CA9  mov  edi,[rsp+0xA0]             ; plugin's compacted pick
0x1400C3C9D  lea  rbp,[0x140F4EE20]          ; vehicle array base
0x1400C3CB9  lea  r8, [0x14106BB28]          ; vehicle array end
   walk: skip entries with [entry+0]==0, count compacted in ecx, raw in edx
0x1400C3CDA  mov  [0x1404CA2A8], edx         ; <-- ACTIVE VEHICLE INDEX (RAW slot)
```

> **Important asymmetry:** `SpectateVehicles`' `_iCurSelection`/`_piSelect` are **compacted** indices
> (occupied slots only). The global holds the **raw** slot. `SpectateCameras` has no such remap — its
> index and the global are the same number.

### 4.1 Vehicle / rider arrays (CONFIRMED)

| Item | VA | RVA | Notes |
|---|---|---|---|
| `g_Vehicles[50]` base | `0x140F4EE20` | `0xF4EE20` | end `0x14106BB28`; `(end-base)/0x5B24 == 50` exactly, and `cmp eax,0x32` appears at `0x1400C3357` and `0x1400C53C6` |
| stride | `0x5B24` | | |
| `+0x0000` | `int bSlotUsed` | | non-zero = occupied |
| `+0x0270` | `int bHasVehicle` | | `== 1` gates all camera queries |
| `+0x50A8` | `int iVehicleModelId` | | index into the model table, stride `0x574` |
| `+0x5B18` | `int iEntryListIndex` | | index into the race-entry list |
| race-entry list base | `0x140E4BA88` | `0xE4BA88` | `= 0x140E4B540 + 0x548`, stride `0x2A0`, 50 slots; `+0x000` active flag, `+0x264` race number, `+0x004` rider name |
| `ACTIVE_VEHICLE_INDEX` | `0x1404CA2A8` | `0x4CA2A8` | 69 references; `-1` = none |

---

## 5. `ID_CAMERAMODE` / `ID_CHANGE_CAMERA` + the camera enum

### 5.1 Strings

| String | VA | RVA |
|---|---|---|
| `ID_CHANGE_CAMERA` | `0x14033E3C8` | `0x33E3C8` |
| `ID_CAMERAMODE` | `0x14033E428` | `0x33E428` |
| `cc_autocamset` | `0x14033E3E0` | `0x33E3E0` |
| `cc_orbitcam` | `0x14033E3F0` | `0x33E3F0` |
| `cc_freecam` | `0x14033E400` | `0x33E400` |
| `cc_freeroamcam` | `0x14033E410` | `0x33E410` |
| `"VR"` | `0x14033E420` | `0x33E420` |
| `"ML"` / `"TR"` (mode badges) | `0x14033E438` / `0x14033E43C` | `0x33E438` / `0x33E43C` |

Both `ID_*` strings are consumed by the pause/ESC menu builder+handler:
**VA `0x1400BDEEB` / RVA `0xBDEEB`** (range `0x1400BDEEB..0x1400BF610`).

### 5.2 The camera enum — CONFIRMED

Recovered independently from **(a)** the menu label switch at `0x1400BE27A` and **(b)** the numpad
selection block at `0x1400C5476`, and cross-checked against the payload build order in §3.3:

```c
// value of ACTIVE_CAMERA_INDEX @ 0x1404CA2A0
0                 // trackside "auto camera set"  — the TV/director camera set (camset%d/... in track cfg)
1 .. N            // ONBOARD cameras, in order: track-global, then bike (cameras.cfg), then helmet
N + 1             // ORBIT camera        (cc_orbitcam)
N + 2             // FREE camera         (cc_freecam)
N + 3             // FREE-ROAM camera    (cc_freeroamcam)   <-- the user-flyable replay camera
N + 4             // "VR"  (menu-only entry; NOT present in the SpectateCameras payload)
// where N = [0x1404C91D4]
```

Menu label switch (`0x1400BE27A`), verbatim structure:
```
r8d = [0x1404CA2A0]
r8d == 0        -> "cc_autocamset"
ecx = [0x1404C91D4]           ; N
r8d == ecx+1    -> "cc_orbitcam"
r8d == ecx+2    -> "cc_freecam"
r8d == ecx+3    -> "cc_freeroamcam"
r8d == ecx+4    -> "VR"
else            -> msg 0x18E name for onboard camera (r8d - 1)
```
When no bike is loaded (`0x1400BE51C`): `0 -> cc_autocamset`, `1 -> cc_freeroamcam`, `2 -> "VR"`.

> **Free / user-controlled replay camera = `N + 3` (`cc_freeroamcam`).**
> `N + 2` (`cc_freecam`) is the *bike-anchored* free camera (it still tracks the rider);
> `N + 3` is the fully detached fly-anywhere one. Basis: `cc_freeroamcam` is the only special camera that
> survives when there is no vehicle at all (index 1 in the 2-entry list), which only a bike-independent
> camera can do. Confidence: high, but see §7 for the runtime check.

**Sub-mode variables** (these are what `ID_CAMERAMODE` displays; the row is only emitted when the current
camera is `N+2` or `N+3`):

| Variable | VA | RVA | Values |
|---|---|---|---|
| `FREECAM_SUBMODE` (for `N+2`) | `0x1404C9184` | `0x4C9184` | `{0,1}` — toggled by `inc; cdq; and 1; xor; sub` @ `0x1400C835E`. Badge: `1 -> "ML"`, else blank |
| `FREEROAM_SUBMODE` (for `N+3`) | `0x1404C91F8` | `0x4C91F8` | `{0,1,2}` — compared `== 2` @ `0x1400C71CF`. Badge: `1 -> "ML"`, `2 -> "TR"`, else blank |

`ID_CAMERAMODE` menu row emitted at `0x1400BE382`/`0x1400BE59B` (msg `0x10A`), value text at `0x1400BE3B1`/`0x1400BE5C8`.

### 5.3 Where `N` comes from — msg `0x18D` (CONFIRMED)

Implementation at `0x140123653`:
```c
N = g_iNumTrackCameras                   // [0x14055024C]
  + model[id].numBikeCameras             // [0x140550260 + id*0x574 + 0x0F0]
  + model[id].numHelmetCameras;          // [0x140550260 + id*0x574 + 0x358]
```

| Item | VA | RVA |
|---|---|---|
| `g_iNumTrackCameras` | `0x14055024C` | `0x55024C` |
| `g_pTrackCameras` (`void*`, stride `0x40`) | `0x140550250` | `0x550250` |
| `g_VehicleModels[]` base (stride `0x574`) | `0x140550260` | `0x550260` |
| `model[+0x0F0]` `int numBikeCameras` (max 10) | | |
| `model[+0x0F4]` bike camera array, stride `0x3C` | `0x140550354` (model 0) | `0x550354` |
| `model[+0x358]` `int numHelmetCameras` | | |
| `model[+0x35C]` helmet camera array, stride `0x34` | `0x1405505BC` (model 0) | `0x5505BC` |

Name getter msg `0x18E` (`0x14011CF70`) resolves the index across the three tiers in that order, with an
**unbounded `strcpy`** into the caller's buffer.

Onboard bike-camera record (stride `0x3C`), recovered from the `cameras.cfg` parser at
**VA `0x14011AE08` / RVA `0x11AE08`** (keys `camera%d/name|part|position|rotation|fov|gyro` at `0x140342078`…):

| Offset | Field |
|---|---|
| `+0x00` | `char szName[0x18]` |
| `+0x18` | `int iPart` |
| `+0x1C` | `float vPos[3]` |
| `+0x28` | `float vRot[2]` |
| `+0x30` | `int` (set to loop-index/flag @ `0x14011AF42`) |
| `+0x34` | `float fFov` |
| `+0x38` | `int/float gyro` |

Config paths: `%sbikes\%s\cameras.cfg` (`0x14033A260`), `%srider\helmets\%s\cameras.cfg` (`0x14033A278`).
Trackside TV sets use `camset%d/numcameras`, `camset%d/camera%d/{type,pos,pos2,rot,fov,center,limit/*,autozoom/*}` (`0x140342150`…).

### 5.4 The cycle-camera routine (CONFIRMED)

On-track / race camera+input update: **VA `0x1400C52EA` / RVA `0xC52EA`** (range `..0x1400C8AEB`).

**Next / previous camera** — `0x1400C556E`:
```
DIK_SUBTRACT (0x4A, numpad -):  if ([CAM] >  0) [CAM]--        @ 0x1400C557D..0x1400C5589
DIK_ADD      (0x4E, numpad +):  [CAM]++                        @ 0x1400C558F..0x1400C55A3
```

**Direct selection** — numpad digits, `camIndex = 10*bank + k + 1`, then range-checked against `N`:

| Key (DIK) | `k` | bank 0 special-case |
|---|---|---|
| `0x4F` NUMPAD1 | `-3` | `[CAM] = 0` (autocamset) |
| `0x50` NUMPAD2 | `-2` | `[CAM] = N + 1` (orbit) |
| `0x51` NUMPAD3 | `-1` | `[CAM] = N + 2` (freecam) |
| `0x52` NUMPAD0 | `-4` | `[CAM] = N + r15 + 1` |
| `0x4B` NUMPAD4 | `0` | — |
| `0x4C` NUMPAD5 | `+1` | — |
| `0x4D` NUMPAD6 | `+2` | — |
| `0x47` NUMPAD7 | `+3` | — |
| `0x48` NUMPAD8 | `+4` | — |
| `0x49` NUMPAD9 | `+5` | — |

**Camera bank / page** — `CAMERA_BANK` = VA `0x1404C9188` / RVA `0x4C9188`, set by the top-row digits:
`DIK_1(0x02)→1 if N≥6`, `DIK_2(0x03)→2 if N≥0x10`, `DIK_3(0x04)→3 if N≥0x1A`, `DIK_4(0x05)→4 if N≥0x24`,
`DIK_0(0x0B)→0` (writes @ `0x1400C5499/54BF/54EB/5517/5541/5568`).

**Final clamp** — `0x1400C58CA` (this is the authoritative upper bound on the index):
```
if (edi) { max = N + r15 + 2; if ([CAM] > max) [CAM] = max; }
else     { max = N + r15 + 1; if ([CAM] > max) [CAM] = max; }
```
with `r15 == 1` when a bike is loaded (`r15 = 0` in the no-vehicle branch @ `0x1400C543B`), i.e. **max = N+3**
— exactly the `cc_freeroamcam` index, matching the `N+4` payload length. Consistent, CONFIRMED.

**Menu control** — `ID_CHANGE_CAMERA` list in the ESC menu writes the index straight through:
```
0x1400BEA37  lea rdx,[ID_CHANGE_CAMERA] ; strcmp against the activated control id
0x1400BEA4A  mov [0x1404CA2A0], ebp     ; ebp = selected row
```

**Session reset** — `0x1400C33A4` (in `0x1400C3226`, the session/camera init) writes `r13d` into
`[0x1404CA2A0]`, and the same `r13d` into `[0x1404C9184]` (`0x1400C33DB`) and `[0x1404C91F8]`
(`0x1400C34F8`). `r13d` is established before this `.pdata` chunk, so its value is **UNKNOWN** statically —
breakpoint `0x1400C33A4` to read it. (`0x1400C33AB` / `0x1400C33B5` set two neighbouring globals to `4`.)

---

## 6. AOB signatures

`??` = wildcarded. All are rip-relative `disp32` or `rel32` unless noted.

**All 19 signatures below were regex-matched against the full memory-mapped image: each returns exactly
one hit, at the expected VA.** (Verified with `re.finditer` over `IMG`, `??` → `.` with `DOTALL`.)

```
; --- plugin table ---------------------------------------------------------

; [1] Plugin loader: GetProcAddress("SpectateCameras") + store to +0xF0   VA 0x14012A84A / RVA 0x12A84A
48 8D 15 ?? ?? ?? ?? 48 89 87 E8 00 00 00 FF 15 ?? ?? ?? ?? 8B 15 ?? ?? ?? ?? 48 8B 0D ?? ?? ??
;   the literal `48 89 87 E8 00 00 00` (mov [rdi+0xE8],rax) anchors SpectateVehicles' slot.

; [2] Plugin array growth: count + base globals, record stride 0x118      VA 0x14012A85E / RVA 0x12A85E
8B 15 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? FF C2 48 89 87 F0 00 00 00 69 D2 18 01 00 00 E8 ?? ?? ??
;   `69 D2 18 01 00 00` = imul edx,edx,0x118 -> pins sizeof(SPlugin). Do not wildcard.

; [3] Plugins_SpectateCameras loop body (call [plugin+0xF0])              VA 0x14012BAA0 / RVA 0x12BAA0
48 8B 05 ?? ?? ?? ?? 4C 8B 9C 07 F0 00 00 00 4D 85 DB 74 1B 4C 8B CE 44 8B C5 49 8B D4 41 8B CD
;   `74 1B` is a short rel8 — wildcard to `74 ??` if you expect codegen drift.

; [4] Plugins_SpectateVehicles loop body (call [plugin+0xE8])             VA 0x14012BA00 / RVA 0x12BA00
48 8B 05 ?? ?? ?? ?? 4C 8B 9C 07 E8 00 00 00 4D 85 DB 74 1B 4C 8B CE 44 8B C5 49 8B D4 41 8B CD

; --- router ---------------------------------------------------------------

; [5] Sim_Dispatch switch head (bound 0x3CA + jump table)                 VA 0x140120CFB / RVA 0x120CFB
81 F9 CA 03 00 00 0F 87 ?? ?? ?? ?? 4C 8D 0D ?? ?? ?? ?? 48 63 C1 41 8B 84 81 ?? ?? ?? ?? 49 03
;   VOLATILE: `CA 03` (message-id count) and the table disp32 `B0 80 12 00` both move every build.
;   Wildcarded above; keep `81 F9`, `0F 87`, `4C 8D 0D`, `48 63 C1`, `41 8B 84 81`, `49 03` fixed.

; --- spectate call sites (func 0x1400C3B8C) --------------------------------

; [6] SpectateCameras CALL SITE  (msg 0x3C9)   *** PRIZE ***              VA 0x1400C3F1E / RVA 0xC3F1E
44 8B 0D ?? ?? ?? ?? 8B 54 24 70 48 8D 84 24 A0 00 00 00 4C 8D 84 24 10 0D 00 00 B9 C9 03 00 00
;   `44 8B 0D ????????` -> ACTIVE_CAMERA_INDEX.  `B9 C9 03 00 00` = msgId 0x3C9 (VOLATILE across builds).
;   VOLATILE: every `24 xx xx 00 00` stack displacement (0x70, 0xA0, 0xD10) moves if the frame changes.

; [7] SpectateVehicles CALL SITE (msg 0x3C8)                              VA 0x1400C3C78 / RVA 0xC3C78
48 8D 84 24 A0 00 00 00 4C 8D 84 24 70 14 00 00 44 8B CB 8B D6 B9 C8 03 00 00 48 89 44 24 20 FF
;   VOLATILE: stack disps 0xA0 / 0x1470 / 0x20; msgId `C8 03`.

; [8] ACTIVE_CAMERA_INDEX write-back from plugin _piSelect                VA 0x1400C3F49 / RVA 0xC3F49
8B 0D ?? ?? ?? ?? 48 8D 3D ?? ?? ?? ?? 83 F8 01 48 8D 1D ?? ?? ?? ?? BE 0F 00 00 00 0F 44 8C 24
;   `83 F8 01` (cmp eax,1) + `0F 44 8C 24 ..` (cmove ecx,[rsp+..]) is the claim test. Stable anchor.

; [9] ACTIVE_VEHICLE_INDEX write-back from plugin _piSelect               VA 0x1400C3CDA / RVA 0xC3CDA
89 15 ?? ?? ?? ?? 4C 8D 84 24 C0 28 00 00 48 8D 15 ?? ?? ?? ?? B9 01 01
;   VOLATILE: stack disp 0x28C0; `B9 01 01` is the leading bytes of mov ecx,0x101.

; [10] read ACTIVE_VEHICLE_INDEX at top of spectate update                VA 0x1400C3BBC / RVA 0xC3BBC
44 8B 2D ?? ?? ?? ?? 4C 89 B4 24 98 2A 00 00 45 33 F6 4C 89 BC 24 90 2A
;   VOLATILE: stack disps 0x2A98 / 0x2A90.

; [11] vehicle entry scan bounds (base+0x5B18 .. end, stride 0x5B24)      VA 0x1400C3BE7 / RVA 0xC3BE7
48 8D 3D ?? ?? ?? ?? 44 0F 29 94 24 40 2A 00 00 44 0F 29 A4 24 20 2A 00 00 4C 8D 3D ?? ?? ?? ??
;   Prefer instead the stride literal: search `48 81 C7 24 5B 00 00` / `81 C3 24 5B 00 00`.

; --- camera mode / cycle ---------------------------------------------------

; [12] ESC-menu camera label switch on ACTIVE_CAM / NUM_CAMS              VA 0x1400BE27A / RVA 0xBE27A
44 8B 05 ?? ?? ?? ?? 45 85 C0 75 1F 4C 8D 84 24 40 03 00 00 48 8D 15 ?? ?? ?? ?? B9 01 01 00 00
;   Best single anchor for BOTH camera globals + the enum shape. VOLATILE: `75 1F`, stack disp 0x340.

; [13] ID_CHANGE_CAMERA menu control -> writes ACTIVE_CAMERA_INDEX        VA 0x1400BEA37 / RVA 0xBEA37
48 8D 15 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? 85 C0 75 10 89 2D ?? ?? ?? ?? B8 01 00
;   `89 2D ????????` is the write. VOLATILE: `75 10`.

; [14] cycle camera PREV (DIK_SUBTRACT 0x4A) then NEXT (DIK_ADD 0x4E)     VA 0x1400C556E / RVA 0xC556E
B9 D0 02 00 00 FF 15 ?? ?? ?? ?? 85 C0 74 12 8B 05 ?? ?? ?? ?? 85 C0 7E 08 FF C8 89 05 ?? ?? ??
;   `FF C8 89 05 ????????` = dec [ACTIVE_CAM].  `B9 D0 02 00 00` = msg 0x2D0 (VOLATILE).

; [15] ACTIVE_CAMERA_INDEX final clamp (max = N + r15 + 1/2)              VA 0x1400C58CA / RVA 0xC58CA
85 FF 74 11 41 8D 54 07 02 3B CA 7E 18 89 15 ?? ?? ?? ?? EB 10 41 8D 44 07 01 3B C8 0F 4F C8 89
;   `41 8D 54 07 02` / `41 8D 44 07 01` (lea edx,[r15+rax+2] / lea eax,[r15+rax+1]) pin the enum top end.

; [16] NUMPAD0 -> special camera (N + r15 + 1)                            VA 0x1400C5866 / RVA 0xC5866
8B 05 ?? ?? ?? ?? 85 C0 75 13 8B 05 ?? ?? ?? ?? 41 8D 4C 07 01 89 0D ?? ?? ?? ?? EB 47 8D 04 80
;   first `8B 05 ????????` -> CAMERA_BANK, second -> NUM_CAMERAS, `89 0D` -> ACTIVE_CAM.

; --- camera data services --------------------------------------------------

; [17] msg 0x18D -> NUM_CAMERAS refresh for active vehicle                VA 0x1400C5411 / RVA 0xC5411
8B 94 28 A8 50 00 00 4C 8D 05 ?? ?? ?? ?? B9 8D 01 00 00 FF 15 ?? ?? ?? ?? 44 89 BC
;   `8B 94 28 A8 50 00 00` pins vehicle field +0x50A8.  `4C 8D 05 ????????` -> &NUM_CAMERAS.

; [18] msg 0x18D impl: N = trackCams + model[+0xF0] + model[+0x358]       VA 0x140123653 / RVA 0x123653
48 8D 05 ?? ?? ?? ?? 4C 8D 84 24 D8 02 00 00 49 63 10 48 69 D2 74 05 00 00 8B 8C 10 58 03 00 00
;   `48 69 D2 74 05 00 00` = imul rdx,rdx,0x574 (model stride). `58 03` = +0x358.

; [19] msg 0x18E impl: camera-name getter (track / bike / helmet tiers)   VA 0x14011CF70 / RVA 0x11CF70
41 C6 00 00 8B 05 ?? ?? ?? ?? 4D 8B C8 3B D0 0F 8C ?? ?? ?? ?? 4C 63 D1 2B D0 48 8D 0D ?? ?? ??
;   `41 C6 00 00` (mov byte[r8],0) is the function's fingerprint. `8B 05 ????????` -> g_iNumTrackCameras.
```

---

## 7. Confidence table

| # | Finding | Status | If not CONFIRMED: the runtime check that settles it |
|---|---|---|---|
| 1 | Plugin record is `0x118` bytes, 29 callbacks at the listed offsets | **CONFIRMED** | — (`imul …,0x118` + `add rdi,0x118` + every store offset read directly) |
| 2 | Callback name→offset mapping | **CONFIRMED** | — (store always follows its own `GetProcAddress`; verified for all 29) |
| 3 | `DrawInit` is a 30th, lazily-resolved export | **CONFIRMED** | — |
| 4 | `g_iNumPlugins` `0x565CB8`, `g_pPlugins` `0x565CC0` | **CONFIRMED** | — |
| 5 | `Plugins_SpectateCameras` = `0x12BA60`, `SpectateVehicles` = `0x12B9C0` | **CONFIRMED** | — |
| 6 | Router `0x120CC0`, msg `0x3C8`/`0x3C9`, table RVA `0x1280B0` | **CONFIRMED** | — (jump-table entries decoded to the exact case bodies) |
| 7 | Camera call site `0xC3F1E`; vehicle call site `0xC3C78` | **CONFIRMED** | — |
| 8 | `ACTIVE_CAMERA_INDEX = 0x4CA2A0` | **CONFIRMED** | — (read + write in the same 5 instructions, plus 24 other writers) |
| 9 | `ACTIVE_VEHICLE_INDEX = 0x4CA2A8`, raw slot 0..49 | **CONFIRMED** | — |
| 10 | `NUM_CAMERAS = 0x4C91D4` | **CONFIRMED** | — |
| 11 | `_pCameraData` = packed NUL-terminated names, variable stride | **CONFIRMED** | — (cursor arithmetic decoded; corroborated by menu switch + `0x18E`) |
| 12 | `_pVehicleData` stride `0x68` = `{int; char[100]}` | **CONFIRMED** | — |
| 13 | Camera enum `0 / 1..N / N+1 orbit / N+2 free / N+3 freeroam` | **CONFIRMED** | — (two independent decode paths agree) |
| 14 | **`N+3` (`cc_freeroamcam`) is the detached user-flyable camera**, `N+2` is bike-anchored | **INFERRED** (high) | Attach, set `[0x1404CA2A0] = N+3` in a replay and confirm the camera detaches from the bike; then `N+2` and confirm it still tracks. Or: read the localisation file's `cc_freecam` / `cc_freeroamcam` values. |
| 15 | `FREECAM_SUBMODE 0x4C9184 ∈ {0,1}`, `FREEROAM_SUBMODE 0x4C91F8 ∈ {0,1,2}` | **CONFIRMED** (ranges) / **INFERRED** (meanings) | Badges are `"ML"` and `"TR"` — most likely *Mouse Look* and *Track (target)*. Toggle in-game and watch which behaviour changes. |
| 16 | `N+4` = `"VR"` exists only in the menu, not in the plugin payload | **CONFIRMED** | — (payload count is `N+4`, i.e. indices `0..N+3`) |
| 17 | Cycle prev/next = numpad `-` / `+` (`DIK 0x4A` / `0x4E`) | **CONFIRMED** | — |
| 18 | Numpad-digit direct selection + `10*bank+k+1` formula | **CONFIRMED** | — |
| 19 | `N = trackCams + bikeCams + helmetCams` | **CONFIRMED** | — |
| 20 | Bike camera record stride `0x3C` with the listed fields | **CONFIRMED** | — (parser key strings match field offsets 1:1) |
| 21 | Helmet camera record stride `0x34` | **INFERRED** | Only the stride (`imul rcx,rcx,0x34`) and the name-at-offset-0 are proven; field layout not decoded. Disassemble the `rider\helmets\%s\cameras.cfg` parser to settle it. |
| 22 | `0x1400C3B8C` = spectate/replay update, `0x1400C52EA` = on-track update | **INFERRED** (high) | Neither has a direct caller (computed state dispatch). Breakpoint each and observe which game state hits it. |
| 23 | `r15 == 1` in the clamp when a bike is loaded | **INFERRED** (high) | `r15` is set to `0` in the no-vehicle branch but its `1` value is established before the `.pdata` chunk boundary. Breakpoint `0x1400C58CA` and read `r15`. |
| 26 | Value written to the camera globals by the session reset at `0x1400C33A4` | **UNKNOWN** | `r13d` comes from outside the decoded chunk. Breakpoint `0x1400C33A4` and read `r13d` at session start. |
| 24 | Race-entry list base `0xE4BA88`, stride `0x2A0` | **INFERRED** | Field offsets exceed the stride relative to the raw `lea` base, so I folded `+0x548` into the base. Read two adjacent entries at runtime and confirm the rider names are `0x2A0` apart. |
| 25 | Router is reached only via `0x566C48` | **CONFIRMED** | — (no direct calls, no data pointers; only two `lea` initialisers) |

---

## 8. Practical notes for a cinematic mod

* **Switching camera:** implement `SpectateCameras`, return `1`, and write your index into `*_piSelect`.
  The index space is exactly the enum in §5.2 and the payload you receive is the authoritative name list
  for the current frame. Walk it with `strlen()+1` — do **not** assume a stride.
* **Switching rider:** implement `SpectateVehicles`. Remember `_iCurSelection`/`*_piSelect` are
  **compacted** (occupied slots only) while the global is the raw slot — the game does the remap for you
  at `0x1400C3CDA`, so just work in the compacted space the payload gives you.
* **Both callbacks are polled every frame** by the spectate update, and the *first* plugin to return `1`
  wins — a second plugin never sees the call. Return `0` when you have nothing to say.
* **Direct global writes work too** (`0x4CA2A0` / `0x4CA2A8`) but race the update function and are
  re-clamped at `0x1400C58CA` to `N+3`. The callback path is strictly safer.
* Camera names are **localised**; index is the stable identity, text is not.
