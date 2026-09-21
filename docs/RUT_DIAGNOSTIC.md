# Native rut diagnostics — Phase 0 observation and Phase 1 signed pulse

With `--rut-diag` alone, this build observes MX Bikes' stock terrain-deformation point writer
and the received-block apply path. Observation mode does not widen a rut, raise a shoulder,
alter a terrain cell, or change a network packet. The purpose is to measure the
sign/scale/caller/block/replication facts needed before that gameplay work is safe. The
separately armed Phase-1 mode later in this guide performs one bounded signed-cell test.

The diagnostic defaults **off**. The launcher flag creates `frostmod_rutdiag.flag` beside
the binaries for this run; launching once without the flag deletes it. The DLL additionally
requires MX Bikes beta21e (`TimeDateStamp 0x6A21833D`) and exact 32-byte writer/apply
signatures. On any mismatch it logs `REFUSED` and leaves that stock path untouched.

## Build and install

On a Windows x64 developer prompt, from the repository root:

```bat
cmake -B build -A x64
cmake --build build --config Release --target frostmod frostmod_app
ctest --test-dir build -C Release --output-on-failure
```

Copy `build\bin\Release\frostmod.exe` and `build\bin\Release\frostmod.dll` together into a
temporary diagnostic folder. Close MX Bikes before replacing an existing loaded DLL. Start:

```bat
frostmod.exe --rut-diag --no-update-check
```

The DLL path is intentionally omitted: this diagnostic launcher recognizes both flags and
defaults to `frostmod.dll` beside the executable. If the launcher instead prints `DLL not
found: ...--rut-diag`, stop. That `frostmod.exe` is an older or stale artifact which does not
contain the Phase-0 argument parser; adding `frostmod.dll` as the first argument does not make
that build diagnostic-capable. Rebuild the `frostmod_app` target and copy its newly produced
`build\bin\Release\frostmod.exe` beside the matching diagnostic DLL, then confirm its printed
build date/time changed before continuing.

The launcher prints that rut diagnostics are on. `frostmod.log` appears beside the two
binaries (or `%TEMP%\frostmod.log` only if that folder is not writable). A successful hook
starts with:

```text
[rutdiag] writer ARMED observation-only @ RVA 0x1f5ac0 ...
[rutdiag] received-block apply ARMED observation-only @ RVA 0x1f60c0 ...
```

No `ARMED` line means do not test: use the adjacent `REFUSED` or signature line to diagnose
the build mismatch. The first 48 native writer calls are logged, then every 256th call. The
first 24 received blocks are logged, then every 16th block, so a long ride stays bounded
without a join-time full-grid sweep hiding the later riding samples.

## Solo smoke test

Use a deformable dirt track and a fresh session.

1. Idle on track for five seconds, then ride one slow, straight 30–50 m pass at steady
   throttle. Stop for five seconds.
2. Ride a constant-radius left turn, then a constant-radius right turn.
3. Repeat the straight line once, deliberately following the first rut.
4. If practical, repeat once braking hard (front load) and once accelerating hard (rear
   load). Do not infer a wheel label unless caller/coordinate sequences separate in the log.
5. A block-boundary pass is useful only if the log shows neighboring block numbers for the
   four cells; no special track location is required in advance.

Success in game means completely stock deformation and no crash, hitch, or visual change
caused by FrostMod. Success in `frostmod.log` means sampled calls show a stable thread id and
caller RVA, finite position/magnitude, four plausible neighboring cells, positive stock
cell deltas when the rut is accepted, and the corresponding dirty byte changing or remaining
set. Turns should move the cell coordinates in two axes; a boundary sample should name two
or four block ids. Repeated passes should accumulate further signed cell changes.

Stop and roll back if either hook logs `REFUSED`, the game crashes or develops a repeatable
hitch, coordinates/cells are implausible, a stock writer call changes more than its four
reported cells, or the watching client sees a rut without a corresponding apply record.

## Private two-client test

1. Start a private dedicated server and join with two clients running this diagnostic build
   with `--rut-diag`: one rider and one parked/spectating watcher. Begin on a fresh
   track/session if possible. The apply hook on the watcher is what proves the block arrived.
2. On the diagnostic client, repeat the straight, left/right turn, repeat-line, braking, and
   acceleration actions above. The watching client should remain parked or spectating.
3. Confirm the watching client sees the same ordinary stock rut appear and deepen. Its log
   should contain `[rutdiag/apply]` lines naming the corresponding region and signed deltas,
   plus the authoritative and 16-bit height changes applied there. Save both `frostmod.log`
   files; also record whether a late rejoin reconstructs the rut and emits apply lines.

This observation-only build cannot prove a negative shoulder survives the network, because
`--rut-diag` intentionally never injects one. Phase 1 below is a separate, explicitly armed,
one-shot private test. It is not enabled by observation mode and is not normal FrostMod
gameplay behavior.

## Phase 1: one-shot negative transport test

These Phase-1 edits are currently **uncommitted manual-review changes**. A checkout of the
already-published branch does not contain them. After manual review, they need a follow-up
commit/push or an explicitly supplied patch/artifact before another Windows machine can build
this mode.

The pulse answers one question only: can one small signed negative terrain cell survive the
stock sender/server/receiver path and produce the opposite height sign on both clients? It is
not a rut-shaping kernel. `--rut-negative-pulse` implies the diagnostic hooks but writes a
separate marker; `--rut-diag` by itself remains observation-only.

Safety rules are fail-closed:

- MX Bikes beta21e and both exact writer/apply signatures must match.
- The stock writer runs once with its original arguments before any pulse decision.
- The stock call must have increased all four expected footprint cells.
- The target must be a terrain-interior cell cardinally adjacent to that footprint, outside
  every stock-touched block, with an outgoing value of zero, a clear dirty byte, and an
  entirely zero 64x64 outbound block.
- The source writes exactly one signed value, `-0x00040000` (`-262144`), then marks only that
  block. It assigns zero to the fixed value rather than adding, so local integer wrap and
  same-cell overlap cannot occur.
- The authoritative/rendered terrain grid and packet payloads are never edited directly.
- The one-shot stays closed for the rest of the process, including content reloads and
  session changes. If no candidate is safe, bounded `REFUSED candidate` lines are logged and
  the pulse keeps waiting without modifying anything.

### Windows rebuild and copy

After the Phase-1 changes have been published or applied locally, close MX Bikes and every
old FrostMod launcher, then run from PowerShell at the repository root:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --target frostmod frostmod_app
ctest --test-dir build -C Release --output-on-failure

New-Item -ItemType Directory -Force .\tmp | Out-Null
Copy-Item .\build\bin\Release\frostmod.exe, .\build\bin\Release\frostmod.dll .\tmp\ -Force
Get-Item .\tmp\frostmod.exe, .\tmp\frostmod.dll | Select-Object Name, LastWriteTime, Length
```

### Solo quick start

```powershell
Set-Location .\tmp
.\frostmod.exe --rut-diag --rut-negative-pulse --no-update-check
```

1. Start one rider in a private local-host session on flat, deformable dirt. Ride one slow lap,
   return to the menu, rejoin without restarting the game, and ride briefly again.
2. Success is exactly one `[rutpulse] FIRED`, followed by one `role=source-match` with
   `delta=-262144`, `observed=-262144`, an authoritative difference of `-262144`, and a
   positive `height_delta`; there must be no second `FIRED`, crash, or repeatable hitch.
3. Send back the complete `frostmod.log`. If no pulse fires after one lap, send the log anyway;
   the run is inconclusive.

See the [detailed solo reference](#detailed-solo-reference-optional) for refusal meanings,
exact log shapes, stop conditions, and the optional later watcher test.

### Detailed solo reference (optional)

1. Start a private local-host session with one rider on an initially flat, deformable area.
   Close other FrostMod/game processes so the log belongs to this run.
2. Ride slowly and steadily until the log contains exactly one line matching:

   ```text
   [rutpulse] FIRED id=... source_writer_call=... target=(x,y)#cell block=... outbound=0>-262144 dirty=0>1 delta=-262144 one_shot=complete
   ```

   If it never fires after a lap, the test is inconclusive: save the log and stop rather than
   relaxing a precondition. Bounded `REFUSED candidate` lines are expected while it waits.
3. The same log must later contain a received-block summary with `neg=1` and the pulse cell,
   followed by:

   ```text
   [rutpulse/apply] id=<same id> role=source-match ... target=(same x,y)#same-cell block=same-block delta=-262144 authoritative=A>B observed=-262144 height16=H0>H1 height_delta=positive
   ```

   Verify numerically that `B - A == -262144`; the exact `observed=-262144` shows the signed
   value survived the local client/server round trip. A positive `height_delta` shows the
   physical/rendered sign is a rise. `height_delta=0` proves transport only and leaves the
   physical result inconclusive.
4. Confirm ordinary riding remains stable: no crash, repeatable hitch, sudden bike catch, or
   visibly exaggerated spike/trench around the test area.
5. Return to the menu and rejoin, or reload content, without restarting the game. Ride again
   long enough to encounter accepted rut writes. There must be no second `[rutpulse] FIRED`;
   the process-wide one-shot remains closed.
6. Save the complete `frostmod.log`. This solo result is the required gate for the next
   controlled shape experiment.

Stop and preserve the log on any crash/hitch, a second fire, `role=source-mismatch`, a
different or wrapped delta, another negative cell, a non-positive height result, or block/cell
disagreement. Do not continue to a stronger pulse.

### Optional later two-client fanout confirmation

Remote fanout is useful but is not a blocker once the solo round trip passes. When a second
machine/rider becomes available, run the source as above and start the parked/spectating
watcher in a separate binary folder with observation only:

```powershell
Set-Location .\tmp
.\frostmod.exe --rut-diag --no-update-check
```

The watcher must not show pulse arming, but must show both diagnostic hooks. Its log should
contain `role=watcher` for the rider's cell/block with the same `delta=-262144`, exact
authoritative difference, and positive height response. Until that optional run is completed,
record the limitation explicitly: remote client fanout remains unverified even though the
solo local client/server round trip passed.

### What this de-risks—and what Phase 2 must achieve

If the required solo Phase 1 passes, it establishes the local signed shoulder primitive and
client/server round trip; remote client fanout remains a separately tracked limitation. The
final goal is naturally generated, good-looking, rideable ruts on initially flatter tracks,
not the sharp stock trench that can catch or flip a bike. A future continuous kernel must meet
all of these acceptance criteria before it is suitable for normal use:

- tracks begin reasonably smooth, with riders and bikes creating believable roughness rather
  than requiring exaggerated baked-in trenches;
- a shallow center cut with smoothly rounded raised shoulders;
- a multi-cell cross-section with spatial smoothing, never one-cell cliffs or discontinuities;
- conservative per-pass accumulation so repeated riding deepens the line gradually;
- hard depth and slope caps that prevent crash-inducing geometry; and
- rideable, visually natural deformation across different dirt types, including low-speed
  behavior without janky catches or abrupt steering kicks; and
- consistent rider/watcher physics and visuals across the stock network path once optional
  remote fanout is available to verify.

The one-shot deliberately does not implement any of that shaping. It removes the sign and
transport uncertainty first, so the later smooth kernel can be designed around a proven
negative primitive instead of combining networking risk with geometry risk.

## Disable and roll back

Close MX Bikes and run `frostmod.exe --rut-diag --no-update-check` once without
`--rut-negative-pulse` to delete only the pulse marker while retaining observation. Run
`frostmod.exe --no-update-check` without either flag to delete both markers; the next game
process uses pure stock terrain behavior. For a complete uninstall, close MX Bikes and remove
the temporary diagnostic folder or restore the prior `frostmod.exe`/`frostmod.dll`. No game
file, track, save, or server configuration is modified by these diagnostics.
