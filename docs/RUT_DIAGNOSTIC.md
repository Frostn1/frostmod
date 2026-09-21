# Native rut diagnostic — Phase 0 test guide

This build observes MX Bikes' stock terrain-deformation point writer and the received-block
apply path. It does not widen a rut, raise a shoulder, alter a terrain cell, or change a
network packet. The purpose is to measure the sign/scale/caller/block/replication facts
needed before that gameplay work is safe.

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
it intentionally never injects one. The receiver's signed-underflow behavior is not yet safe
enough to make that experiment part of a normal FrostMod hook. A later controlled private
test can add one bounded adjacent-cell pulse after these logs confirm the object, scale, and
apply path.

## Disable and roll back

Close MX Bikes and run `frostmod.exe --no-update-check` once without `--rut-diag`; the
launcher deletes `frostmod_rutdiag.flag`, and the next game process uses pure stock terrain
behavior. For a complete uninstall, close MX Bikes and remove the temporary diagnostic
folder or restore the prior `frostmod.exe`/`frostmod.dll`. No game file, track, save, or
server configuration is modified by this diagnostic.
