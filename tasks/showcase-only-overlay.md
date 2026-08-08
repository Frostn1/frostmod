# Showcase-only overlay — strip the F8 menu + all in-game UI

**Goal:** FrostMod draws nothing in-game except a small always-on badge proving it is
running and attached. No F8 menu, no panels, no radar/ESP HUD. Feature code stays
compiled and re-enablable.

**Approach:** one compile-time switch, `#define FROSTMOD_UI 0`, near the top of
`src/frostmod.cpp`. Every menu / panel / HUD block is wrapped in `#if FROSTMOD_UI`.
Flip it to `1` and the whole UI comes back verbatim — smallest diff, trivial revert,
no dead-code warnings.

## Plan

- [x] Branch `chore/showcase-only-overlay` off `feature/frostserver-client`
- [x] `src/frostmod.cpp`: add the `FROSTMOD_UI` switch
- [x] Gate `MenuItem` / `kMenu[]` / `kMenuCount` / `MenuAction()`
- [x] Gate the GL panel renderers: `DrawTrackManager`, `DrawSwitcher`,
      `DrawDirectConnect`, `DrawModelSwap`, `DrawServerMaps` (+ its row helpers)
- [x] Gate the GL HUD: `GlCircle`, `GlTri`, `GlRectOutline`, `ClampF`,
      `DrawRadarGL`, `DrawEspGL`
- [x] Gate the PiBoSo HUD emitters: `EmitRadarPiBoSo`, `EmitEspPiBoSo`
- [x] `DrawOverlay()` — showcase path: one pill, `FrostMod vX.Y.Z  -  attached`
- [x] `BuildOverlayDrawLists()` — same, one quad + one string
- [x] Gate every `Tick()` input block: F8/digits, track manager, switcher,
      direct connect, model swap, server maps, radar PageUp/PageDown
- [x] `src/launcher.cpp`: drop the F8/menu advice from the console output
- [x] Scrub the now-wrong `[init]` / `[trklib]` log lines that advertise F8
- [x] `README.md`, `docs/USAGE.md`, `docs/PLUGIN.md`, `docs/FROSTSERVER.md`
- [x] `CHANGELOG.md`: dated entry
- [ ] Push the branch so Windows CI compiles it (only build verification available
      from this machine)
- [ ] Hand to Sean for the manual Windows-box check

## Explicitly NOT touched

Reload core, track manager, model swap, server maps + fsclient, direct connect,
radar/ESP ingest, server filter, master capture, `offsets.h`, `frostserver.cpp`.
All still compiled; reload is still reachable from `frostmod.exe` (R) and the MXB
App command channel.

## Consequences to confirm

- The reload progress bar and the transient status line go with the UI — a reload
  triggered from the console reports only to `frostmod.log`.
- The plugin data callbacks (`RunTelemetry`, `RaceTrackPosition`, …) keep feeding the
  radar snapshot; it is simply never drawn.
- `frostmod_radar.cfg` is still read/written; the values have no visible effect.
- No DB / schema surface in this repo.

## Review

Implemented and committed locally as `6ebd73f` on `chore/showcase-only-overlay`.

**Verified (what I could check from macOS)**
- `#if FROSTMOD_UI` / `#if !FROSTMOD_UI` / `#else` / `#endif` all balanced; no guard
  left open.
- Emulated the preprocessor at `FROSTMOD_UI 0` (3252 of 4259 lines active) and
  confirmed brace balance is 0 on the resulting translation unit.
- Grepped that active build for every symbol that moved behind a guard — `kMenu`,
  `kMenuCount`, `MenuItem`, `MenuAction`, `DrawTrackManager`, `DrawSwitcher`,
  `DrawDirectConnect`, `DrawModelSwap`, `DrawServerMaps`, `SmFormatRow`, `GlCircle`,
  `GlTri`, `GlRectOutline`, `ClampF`, `DrawRadarGL`, `DrawEspGL`, `EmitRadarPiBoSo`,
  `EmitEspPiBoSo`, `PB_ASPECT` — zero dangling references.
- Read back the post-preprocessor `Tick()`, `DrawOverlay()` and
  `BuildOverlayDrawLists()`; each is self-contained with no orphaned locals.
- `FillRect` / `GlText` / `EnsureFont` deliberately stay outside the guards — the
  badge needs them.

**Compiled — Windows CI green**
- Run [31243797962](https://github.com/Frostn1/frostmod/actions/runs/31243797962) on
  `chore/showcase-only-overlay`: configure + Release build + output check, all pass in
  51s on `windows-latest` / MSVC x64.
- **Zero new warnings.** The build emits exactly one, `C4010` (a `//` comment ending in
  `\` at `src/frostmod.cpp:930`), which is pre-existing — the prior run on
  `feature/frostserver-client` (31242604381) emits the same single warning. Confirms the
  prediction above: unreferenced statics left behind the guards cost nothing at MSVC's
  default warning level.
- CI does not fire on a branch push (only `main`, PRs, `workflow_dispatch`); this run was
  dispatched manually.

**NOT verified**
- Nothing has been run in the game. The badge's appearance on both render paths, and a
  console reload with the UI gone, still need the Windows box.

**Expected leftovers (harmless)**
Several statics are now referenced only from guarded code — `RadValidateVP`,
`SaveRadarSettings`, `RadBuildBlips`, `LapColorRGB`, `WorldToScreen01`, `VPProject01`,
`OpenServerMaps` and friends. MSVC at its default warning level (CI runs
`cmake -B build -A x64`, no `/W4`, no `/WX`) does not warn on unreferenced statics,
so this cannot fail the build.

**Behaviour change worth knowing**
The GL matrix-capture hooks (`hkGlMatrixMode` / `hkGlLoadMatrixf`) are still installed
and still run every frame, feeding a VP matrix nothing now reads. Left in place
deliberately — gating them would mean touching the hook install path, which is not
UI code.
