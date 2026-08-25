# Free-roam camera — detail not carried in the consolidated doc

## Per-frame integration, exactly as coded (branch body at `0xC6E58`)

`dt` = `int32` ms at `0xE54388`, divided by 1000. Action states are 15 **floats** at `0xF3DAE0`
(analog axes give a continuum; digital tests are `> 0.5f`), polled once per frame by the loop at
`0xC3F4F` from the binding array at `0xE554CC`.

```c
float speed = 1.25f * powf(2.0f, (float)speed_level);        // level 4 -> 20.0

if (state[10] > 0.5f) {                                       // ReplayRoll held
    roll = fmodf(roll + (state[6]-state[7])*dt*25.0f, 360.0f);   // rot L/R becomes roll
    // double-tap Roll within 300 ms -> roll = 0   (0xC6E88)
} else {
    float r = tanf(fov*DEG2RAD*0.5f) * dt * 200.0f;           // look rate scales with zoom
    yaw = fmodf(yaw - state[6]*r + state[7]*r, 360.0f);
}
float p = tanf(fov*DEG2RAD*0.5f) * dt * 150.0f;
pitch = fminf(pitch + state[9]*p,  80.0f);
pitch = fmaxf(pitch - state[8]*p, -80.0f);

if (state[14] > 0.1f) fov = fmaxf(fov / (1.0f + 1.3f*state[14]*dt),  1.5f);  // FovClose
if (state[13] > 0.1f) fov = fminf(fov * (1.0f + 1.3f*state[13]*dt), 90.0f);  // FovOpen

vec3 X = M*(1,0,0), Y = M*(0,1,0), Z = M*(0,0,1);
pos += Z * (state[2]-state[3]) * dt * speed * 1.00f;          // fwd/back
pos += X * (state[1]-state[0]) * dt * speed * 0.50f;          // strafe  (half rate)
pos += Y * (state[4]-state[5]) * dt * speed * 0.25f;          // up/down (quarter rate)
```

Action index map (0-based, all three recovery paths agree): 0 MoveLeft, 1 MoveRight, 2 MoveForward,
3 MoveBackward, 4 MoveUp, 5 MoveDown, 6 RotLeft, 7 RotRight, 8 RotUp, 9 RotDown, 10 Roll,
11 SpeedInc, 12 SpeedDec, 13 FovOpen, 14 FovClose.

Init (replay-state enter, `0xC3469`): pos = seed + (0,2,0); yaw from atan2; roll 0; speed level 4;
**pitch stored negated** relative to its source angle.

Bonus: the final world camera matrix of *every* game state is mirrored to `0xE54398` (4x4 row-major,
60 bytes written) — useful for *reading* the camera in any mode.

## AOB signatures (`??` = wildcard; each verified to match exactly once in `.text`)

```
S1  replay-state UPDATE fn entry (hook)              RVA 0xC3B60
    B8 B8 2A 00 00 E8 ?? ?? ?? ?? 48 2B E0 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 E0 29 00 00
S2  per-frame poll -> action-state floats            RVA 0xC3F4F
    48 8D 3D ?? ?? ?? ?? 83 F8 01 48 8D 1D ?? ?? ?? ?? BE 0F 00 00 00 0F 44 8C 24 A0 00 00 00
      bytes 3..6 = &bindings[0]   bytes 13..16 = &state[0]
S3  camera-mode dispatch head                        RVA 0xC65B0
    8B 3D ?? ?? ?? ?? 8B 15 ?? ?? ?? ?? F2 44 0F 10 3D ?? ?? ?? ?? 41 8D 0C 3F 4C 8D 2D ?? ?? ?? ??
      bytes 2..5 = &numOnboardCams   bytes 8..11 = &cameraMode
S5  yaw store    RVA 0xC6FBC   F3 0F 11 0D ?? ?? ?? ?? 0F 14 F6 0F 5A C6 F2 0F 59 C7 F2 41 0F 59 C6
S6  pitch store  RVA 0xC704F   F3 0F 11 15 ?? ?? ?? ?? 76 09 F3 44 0F 11 05 ?? ?? ?? ??
S7  roll store   RVA 0xC6F2B   F3 0F 11 0D ?? ?? ?? ?? E9 ?? ?? ?? ?? 0F 14 F6 0F 5A C6
S8  fov store (zoom in, clamp 1.5)   RVA 0xC70C3   F3 0F 11 35 ?? ?? ?? ?? 76 0B 0F 28 F0 F3 0F 11 35 ?? ?? ?? ??
S10 speed level inc/dec (1..6)       RVA 0xC7147   8B 05 ?? ?? ?? ?? 83 F8 06 7D 10 FF C0 89 05 ?? ?? ?? ??
S12 position store x,y,z             RVA 0xC783A   F3 0F 11 05 ?? ?? ?? ?? 0F 5A CC F2 0F 59 CE F2 0F 5A C2 F2 0F 5C D9 F3 0F 11 05 ?? ?? ?? ??
      bytes 4..7 = &pos.x   26..29 = &pos.y   34..37 = &pos.z
S16 end-of-frame RESEED guard        RVA 0xC8BA9   8B 05 ?? ?? ?? ?? 8B 15 ?? ?? ?? ?? 8D 0C 07 8D 41 01 3B D0 0F 84 ?? ?? ?? ??
```

Resolution strategy: scan S3 → `cameraMode` + `numOnboardCams`; S2 → binding/state arrays; S12 →
position; S5/S6/S7 → yaw/pitch/roll; S8 → fov; S10 → speed level. That leaves only the hook itself
needing a code address, and no hard-coded data RVA at all.

## Replay clock signatures

```
CLOCK ADVANCE   RVA 0xC4EE1
    8B 15 ?? ?? ?? ?? 44 39 35 ?? ?? ?? ?? 75 24 0F AF 15 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 03 15 ?? ??
    +0x02 dt(0xE54388)  +0x09 slow(0xE56660)  +0x12 speed(0xE56674)  +0x18 cur(0xE5666C)  +0x1E rem(0xE56670)
    Resolving any one displacement recovers the whole block.
SLIDER SEEK     RVA 0xBF40F   48 8D 15 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? 85 C0 0F 85 3C 01 00 00
ID_PLAY_PAUSE   RVA 0xBEEE6   48 8D 15 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? 85 C0 75 43 8D 70 01 39 05 ?? ?? ?? ??
```

Transport controls (all in the replay UI handler `0xBDEEB`): rewind-to-start `0xBEDD9`,
`ID_REWIND` `0xBEDEC`, `ID_REWIND_FRAME` `0xBEE4B` (cur -= 30), `ID_REWIND_SLOW` `0xBEE83`,
`ID_PLAY_PAUSE` `0xBEEE6`, `ID_ADVANCE_FRAME` `0xBEF3C` (cur += 30), `ID_SLOW_MOTION` `0xBEF74`,
`ID_FAST_FORWARD` `0xBEFCE`, `ID_REPLAY_SLIDER` `0xBF40F`.

## Cross-version signatures (unique in both beta21d and beta21e)

```
SIG-CAM-LOAD   camera-set allocate + cameras.cfg load, fn head
    40 57 41 54 41 56 41 57 48 83 EC 38 4C 8D 35 ?? ?? ?? ?? 45 33 FF 48 8B FA 4D 8B E0
    48 8D 15 ?? ?? ?? ?? 49 8B C6 45 8B CF 0F 1F 80 00 00 00 00
    The `4C 8D 35 ?? ?? ?? ??` at +12 IS the camera-set array base lea — one signature yields
    both the function and the global.
SIG-RPLACT-JT  replay action 15-way jump table
    4C 63 1D ?? ?? ?? ?? 41 83 FB 0E 0F 87 ?? ?? ?? ?? 42 8B 8C 9B ?? ?? ?? ?? 48 03 CB FF E1
    `41 83 FB 0E` (cmp r11d, 14) is the 15-action bound, stable across both MX builds.
```

Camera-set array base by build: beta21e `0x550260`, beta21d `0x550000`, GP Bikes `0x4F6A20`.
Layout identical in all three; base differs in every one. Resolve it, never pin it.
