# Overlay: version pill only

Branch `chore/overlay-version-pill`, cut from `origin/main` (b584bfd, v0.11.0).

## Assignment

In-game FrostMod should stop showing the model-swap UI — and everything else — and be
only a version pill, with a way to hide it.

Decisions taken (asked and answered):

- **Scope**: everything but the pill goes. The F8 menu; the model-swap, track-manager,
  switcher and direct-connect panels; the radar disc and the rider outlines. Features
  behind them stay compiled in.
- **Hide**: `F8` toggles the pill. That is the *only* key FrostMod reads.
- **Reload**: the pill keeps the spinner / percent / progress bar while a reload runs.

## Approach

A single compile-time switch `FROSTMOD_UI` at the top of `src/frostmod.cpp`
(`0` = pill only, the shipped build; `1` = the full UI verbatim). Every menu, panel,
HUD renderer and keyboard block sits behind it, so the old build is one `#define` away
rather than a revert. This is the technique from the unmerged `chore/showcase-only-overlay`
branch, reimplemented on today's `main` (which that branch predates by the whole GP Bikes
port) and with the F8 hide toggle it lacked.

## Steps

- [ ] `src/frostmod.cpp` — add the `FROSTMOD_UI` switch + rationale comment.
- [ ] Gate behind `#if FROSTMOD_UI`: `kMenu[]` / `kMenuCount` / `MenuAction()`;
      `DrawTrackManager`, `DrawSwitcher`, `DrawDirectConnect`, `DrawModelSwap`,
      `DrawRadarGL`, `DrawEspGL`; `EmitRadarPiBoSo`, `EmitEspPiBoSo`.
- [ ] `DrawOverlay()` (GL swap-hook path) — UI-free branch draws one pill:
      idle `FrostMod vX - attached`, a live `SetStatus` line when one is set, or the
      reload spinner + percent + bar. Early-return unless `g_overlayOn`.
- [ ] `BuildOverlayDrawLists()` (PiBoSo `Draw()` path) — same three states, same
      literals, normalized coords. The two paths must not disagree.
- [ ] `Tick()` — read exactly one key: `F8` edge-toggles `g_overlayOn` and logs it.
      Remove the menu digits/Esc, track-manager nav+search, switcher, direct-connect,
      model-swap and PageUp/PageDown radar-range blocks. Nothing else is polled, so
      FrostMod can no longer shadow a game binding or fight another HUD plugin.
- [ ] `RadValidateVP()` — only ever ran under `g_radarOn || g_espOn`; gate with the HUD.
- [ ] `src/launcher.cpp` — drop the stale F8 advice (console banner, `--switch-live`
      note, header comment). Reload here is `R`, or the MXB App.
- [ ] `[init]` log lines — `press R / F8 to trigger` loses the F8.
- [ ] `README.md`, `docs/USAGE.md`, `docs/PLUGIN.md` — describe the pill and its F8
      toggle; mark panel/HUD behaviour as needing a `FROSTMOD_UI 1` build.
- [ ] `CHANGELOG.md` — entry under `## 2026-08-09`.

## Not changed

- Reload: `R` in `frostmod.exe`, `Local\FrostModReload` (the MXB App's path), and the
  plugin data callbacks all behave exactly as now.
- `Local\FrostModCommand` (`refresh_bike_model` / `swap_bike`) — untouched, so the MXB
  App's instant model refresh keeps working. `NoteModelNeedsReselect`'s status line is in
  fact one of the few statuses that still reaches the pill.
- `MsApply` and the rest of the model-swap engine stay compiled in; only the in-game
  panel that drove them is gone. Model swaps are the MXB App's job.
- `src/version.h` — no bump here; the release commit owns that.

## Verification

The DLL is MSVC + MinHook + Win32; it cannot be compiled on this machine. `ci.yml` builds
on `windows-latest` for every pull request, so the compile is verified there. Local checks
before hand-off: no ungated code references a gated symbol, and the GL and PiBoSo pill
branches agree on text and state.
