# Changelog

## Unreleased

Tune a bike's gfx.cfg without restarting the game.

### Added
- **Watch gfx.cfg (F8 → `G`, off by default).** Saving a loaded bike's loose `gfx.cfg` puts its
  grip positions, lever axes and angles, chain ratio, shock link and rider offset onto the bike
  within a second, bike and cockpit, in the pits or on track. The game reads these fields every
  frame, so FrostMod writes the new values where the game's own parser put them. Names, files,
  textures and the exhaust still need a rejoin, and the status line says so. Off on any game build
  whose code differs from the one checked.
- `reload_bike_gfx` command verb: the same, once, for MXB App and Frost's Studio after a save.
- `docs/GFX_CFG.md`: the gfx.cfg keys for rider contact points and moving parts, their units,
  frames and where the game keeps them. Footpegs have no key; the rider animation places the feet.

## 2026-10-08 - v0.49.11

Fix for a menu crash caused by a broken tyre mod.

### Fixed
- A bike that names a tyre that isn't loaded (for example an empty `mods\tyres\p_mx` folder) no longer crashes the bike list. FrostMod uses the first loaded tyre and logs which one was missing.

### Added
- Tyre-state log for the physics NaN crash. Rides on `nantrap=1`, off by default.

## 2026-10-08 - v0.49.10

NaN crash trap for physics crashes.

### Added
- NaN crash trap (`nantrap=1`, off by default). Logs where a physics NaN starts and sends it with crash reports.

## 2026-10-07 - v0.49.9

Less RAM for MX Bikes: an opt-in setting uploads big textures compressed, and a memory diagnostic shows where the game's RAM goes.

### Added
- **Compressed textures (opt-in, `texcompress=1` in frostmod_radar.cfg).** MX Bikes renders with
  OpenGL, and the driver keeps a copy of every texture in normal RAM. With this on, big RGB/RGBA
  textures from the game are uploaded as DXT1/DXT5, so the driver's copy is 4 to 8 times
  smaller. Measured on Cooper MX: committed memory 6,589 -> 5,253 MB, driver memory in RAM
  3,961 -> 2,596 MB, no GL errors. Skipped: render targets, textures under 256 px or not a
  power of two, alpha/luminance/float/depth formats, cube maps, anything a plugin uploads, and
  any texture the game later updates in place (put back to its original format). Off by
  default until it has been checked by eye: DXT can show blocky artifacts on some maps.
- **Memory diagnostic (opt-in, `memdiag=1`).** Every 10 s and on track load, frostmod.log gets
  a `[memdiag]` line: live texture bytes by format, buffer bytes, and the process's private and
  working-set memory. `scripts/memmap.ps1` breaks a running game's memory down by type.

## 2026-10-07 - v0.49.8

Refresh paints, gear, tracks or bikes alone, without crashing under an open chooser; the Coach line follows live ruts; the rejoin fix and the overjump option are removed.

### Added
- **Refresh just paints, or just gear, from F8.** The one "Reload mods" row is now three:
  `1` Refresh paints (the six paint lists, then paints re-applied to riders on track), `2`
  Refresh gear and paints (rider gear models and every paint list), `3` Reload all mods (as
  before). The other rows move down one key; Rebuild riders is now `0`.
- **Refresh tracks / Refresh bikes** (F8 `T` / `B`, and MXB App's `refresh_tracks` /
  `refresh_bikes`). Every row of the reload table is now mapped (the STEP MAP in `offsets.h`):
  nothing reads the track list, so tracks rebuild alone; bikes rebuild with the series (one per
  bike category) and the bike paints (which hold bike indexes). Two waiting requests of
  different kinds merge into the full reload.

### Fixed
- **No reload under an open chooser.** Three crashes in one player's log were the game's
  `_stricmp` reading a dead pointer in a bike chooser page's handler, 2.6 to 6.4 s after a full
  reload, on a menu page: the page still pointed into the lists the reload had freed. Every
  refresh (F8, MXB App, the console's `R`, a track or model change) now waits while a bike,
  paint, profile or event-setup page is open (or the online pits, which carry the same
  chooser), and runs as soon as the player leaves it. Requests made meanwhile merge into one.
  The cause, now traced: the bikes loader replaces the per-bike state array with an
  uninitialised one, while the chooser keeps its old count of entries in it. After the bikes
  row FrostMod now fills the array the way the game's caching pass does and empties the
  chooser's view, which the game rebuilds when the chooser opens.
- **MXB Coach: the line follows the ruts.** On a server that digs the track live (mxbserver), the
  line rode over the top of rut walls instead of down in the rut. The game's own ground was
  sampled once per event and never again, and the `.ground` file and the sheet's terrain are the
  `.trh` as built, with no ruts. Now the square of ground the line is about to cross (56 m, centred
  16 m ahead) is sampled again about once a second, through the same game height query (which
  reads the current heights, ruts included). It runs on the same thread, in the same 0.3 ms
  slices, using at most 3% of wall time. A slice far over its cap turns the refresh off for the
  event, and the line keeps the ground it has. Moved cells rebuild the line straight away.
- **MXB Coach: the live ground always wins** over the `.ground` file and the sheet's terrain when
  it is there and lines up (`coachline::PickGround`). The log says which ground the line stands on,
  once a session and again whenever it changes. Every 30 s it also gives the refresh's passes,
  cells moved and time spent.
- **MXB Coach: the log rotates instead of stopping when full.** At 256 KB it used to stop writing,
  so later sessions left no lines at all. Now the full file becomes `mxbcoach.log.1` and a new one
  starts with a copy of the startup lines.

### Removed
- **The overjump option is gone.** It wrote a flag into the game's session settings and swapped
  the engine's command-bus pointer to do it. FrostMod no longer touches either: `--probe-overjump`,
  `--force-overjump-off` and any leftover `frostmod_overjump.flag` are ignored.
- **The rejoin fix is gone.** It rewrote a call in the game's disconnect code and freed other
  riders' vehicle records, and on 2026-10-04 it freed the local rider's own bike. FrostMod no
  longer touches the disconnect path at all: a rider who leaves and rejoins is handled exactly
  as the game ships. `rejoinfix=` and `settingsver=` lines in an existing `frostmod_radar.cfg`
  are ignored and dropped on the next save. The bike-change cleanup (freeing your old bike's
  record after an accepted change) is unchanged.

## 2026-10-03 - v0.49.7

MXB Coach in-game line: it stays on the track through the helmet camera, the camera button and a
crash, and is never drawn through a view that is not the game's camera.

- **The report.** A rider on 0.49.0, in the helmet camera: "it stopped following the track and
  locked to the camera" (a ribbon running off to the side; a line hanging at the horizon, moving
  with the view). Sean's own 0.49.5 log in the helmet camera shows how: the learnt view (0.48.1)
  refused the true camera whenever the rider's head turned the bike more than 30 degrees in the
  view (`camera_not_where_it_was`), the camera was lost, and after 2 s the line was drawn through
  the telemetry fallback, a view made from the bike's heading (39 times in 1h30). That view is fixed
  to the screen, not the track.
- **The telemetry fallback is off** unless `line_fallback=1` is in hud.ini. With no camera found
  there is no line (`not drawn: no_camera=`), rather than a line in the wrong place.
- **The learnt view checks the distance always and the direction only for a camera that keeps the
  bike steady in view** (the chase camera; the log says the spread). It is a relock's check only:
  while a camera is followed, continuity and the eye watch hold it.
- **One camera, not one set of axes.** The 30-frame lock and every relock count only frames in which
  the pick is the same camera as the frame before: its eye within 3 m, its distance to the bike
  within 0.5 m, and once the bike has moved 2 m, its eye moved with it. A relock with the axes known
  draws after 5 such frames, so a one-frame object never does.
- **The camera button.** With no camera held and a learnt view, the nearest load without that view is
  also followed; once it has been one camera moving with the bike for 30 frames while riding steadily
  (not within 2 s of a crash), it is taken and the old view forgotten (`view changed: ...`). The eye
  leaving its usual distance while riding is a camera change too: the old view is forgotten and the
  search wakes. 0.48.1 kept the old view for ever once the eye watch had dropped the lock.
- **Letting go** in one place (coachline::Relock): no camera for 3 s while riding forgets the learnt
  view, for 8 s with a lock held drops the lock. Down or in a menu nothing is let go.
- **The projection** is the one in effect when the picked load was issued; a load in a 3D pass other
  than the main one (the environment cube's square faces) is left out. When no projection has the
  window's shape for 60 frames (three screens, a render scaled to the window), the main pass is the
  one with the widest depth range; aspects up to 8 are a camera's (was 4).
- The shader-uniform camera is watched by the eye watch like the modelview one.
- **Log:** `view: onboard|chase|far (eye N m from the bike), projection ...` when it changes,
  `camera locked: ... (onboard view), the same camera N frames running and moving with the bike`,
  `relock (view_change|eye_off_near_crash|view_unmatched|lock_stale): ...`, and in the 10 s line
  `view=`, the learnt distance and direction spread, `view_changes=`, `other_pass_loads=`,
  `fallback=`, and `no_camera`, `relock_unconfirmed`, `eye_not_moving_with_bike` in `not drawn:`.
- Test: a ride through 0.49.6's rules and these (chase, camera button, helmet camera looking into
  the corners, a crash): 0.49.6 loses the helmet camera for 419 of 2490 frames (62 through the
  fallback); 0.49.7 draws 2460 through the camera, none through anything else, takes the helmet
  camera 31 frames after the button and 1 frame after getting up.

## 2026-10-03 - v0.49.6

MXB Coach: the game no longer waits in the telemetry callback (755 Compound stutter).

- Sean's 0.49.4 perf log on 755 Compound: frames of 51-138 ms whose RunTelemetry took 30-124 ms, with
  our render-thread share ~0.007 ms. RunTelemetry now copies the sample into a ring and returns; a
  worker thread does the recorder's writes and flushes, cues, gear and pace, DirectInput polls and
  every file check. Only the game's height query stays on the telemetry thread, in 0.3 ms slices
  (was 1 ms) at 10% duty; the terrain marker file is written by the worker; publishing the sampled
  ground try-locks.
- tests/telemetry_bench (3000 calls against a built .dlo): RunTelemetry p99 89-124 us / max 0.4-12 ms
  on 0.49.5, p99 ~4 us on 0.49.6. New perf slot `wk.telemetry` and `telemetry queue drops`.
- Log lines may be 1500 characters (the perf lines were cut at 120).

## 2026-10-03 - v0.49.5

MXB Coach in-game line: a newer sheet mid-ride no longer glitches the line or drops the frame rate.

- Coach rewrites the sheet after laps. The plugin read and parsed it inside RunLap under the lock, then
  swapped whatever lap it held straight in (a partial or crashed lap, another line) and re-learnt the
  depth snap. Now a short-lived worker reads, parses and checks the file with no lock held; the frame
  side only swaps the result in, at the line.
- The reference lap is replaced only if it is whole: no gap or reset, lap closes, length plausible for
  the track, positions rising, and within 20 m of the line in use. Otherwise the line in use stays
  (logged). The same lap again changes nothing; a better one fades in over 1.5 s. The game's ground and
  its alignment are never touched, and the depth snap is not reset.

## 2026-10-03 - v0.49.4

MXB Coach with MXBMRP3: the recorder never waits inside the game's frame, its GL hooks do nothing
outside the 3D pass of a riding frame, and the log says where every frame's time went.

- **Frame report** (`perf` lines every 10 s): swap-to-swap frame time p50/p99/max and frames over
  50 ms; per hook and callback (QueryPerformanceCounter, plain counters, no locks) count, passthroughs,
  total and max; each frame over 50 ms with what we did in it (our time, the game's own present, Draw
  and RunTelemetry, loads, camera state, depth read); `perf/draw`: the Draw callback cadence, which is
  what MXBMRP3's FPS counter shows; whether MXBMRP3 is loaded; the threads each part runs on.
- **Never waits.** Draw and RaceTrackPosition try g_mu instead of waiting for RunTelemetry (Draw hands
  the game last frame's HUD); the log is written by its own thread; the game window and the window
  aspect are looked up once a second or two instead of every frame.
- **GL hooks gated.** A modelview load is only looked at in the world pass (perspective projection) of a
  riding frame with the line on; the HUD pass (ortho), where the engine draws MXBMRP3, is a passthrough.
  The camera search is bounded: 3 s, then 500 ms every 3 s while nothing is found.
- **Fewer driver round trips.** Error/framebuffer/program checks once a second by the clock (was every
  64th frame); one targeted glPushAttrib for the line and the jump calls (was two GL_ALL_ATTRIB_BITS).
- **`safe_mode=auto|1|0`** in hud.ini, auto = on when MXBMRP3 is loaded: no depth reads, no shader-uniform
  hooks (glUseProgram stays), no 30 s camera diagnostics, a shorter camera search (1 s, then 250 ms
  every 5 s).
- `tests/glhook_bench`: 1200 world loads + 20k HUD primitives a frame, hook overhead 0.49.0 vs 0.49.4.

## 2026-10-02 - v0.49.3

FrostMod: hands the game's master server list to MXB App.

- While the game runs MXB App cannot log in to the master, so a server newer than its remembered book
  was joinable in-game but never listed in the app. The server-browser hook now writes the address
  and name of every row the game built (public IPv4 only, filtered rows left out) to
  `frostmod_masterlist.txt` beside the log, when the list changes. Read-only on the game side.


## 2026-10-02 - v0.49.2

FrostMod: the log, filter and settings no longer move to %TEMP% at game start.

- **Root cause.** The game's `Startup()` re-resolved the log path while the Init thread was writing
  its load banner. `fopen_s` opens for writing with no sharing, so the probe hit FrostMod's own open
  handle and fell back to `%TEMP%\frostmod.log`. Everything FrostMod keeps next to its log moved with
  it: `frostmod_filter.flag` and `frostmod_serverfilter.yaml` (the server filter stayed inert, so cheat
  servers showed), `frostmod_mods.txt`, the command file. The real log stopped after its banner.
  Seen on a player's PC in 7 starts on 0.41.0 and 0.45.1 in one day.
- The log is opened shared (`_fsopen`, `_SH_DENYNO`) with a short retry, so a reader or another
  thread never makes a write or the path probe fail. Once the path is FrostMod's own folder it never
  moves again, and the path is changed under the log lock.
- A fallback to `%TEMP%` is logged with the reason (in both Init and Startup), as are a stand-down
  (with the named mutex and this copy's path) and a failed Init thread start.

## 2026-10-02 - v0.49.0

MXB Coach in-game text: a style, size and place for each text item.

- **Per-item look.** The jump call (SINGLE, DOUBLE, ... with its speed), MORE SPEED, the gear badge
  and the cue box each take `<item>_style` (`default`, `block`, `bold`, `italic`), `<item>_size`
  (0.5..3, times today's size), `<item>_x` / `<item>_y` (screen fractions 0..1, y the top of the text)
  and `<item>_anchor` (`left`, `center`, `right`: the edge x is). Items are `jump`, `pace`, `gear`
  and `cue` (the cue keeps `cue_x` / `cue_y`, and `cue` for on/off); `jump_text`, `pace_text` and
  `gear_badge` switch each word off on its own. `default` is today's look: the game's font on the HUD,
  the line's `text_style` on the line. A block style on the HUD draws the 5 x 7 font as screen quads
  and falls back to the game's font when the frame has no quads to spare.
- **On the line or fixed on screen.** `jump_place=screen` takes the nearest jump call off the line
  and draws it at `jump_x` / `jump_y` (middle of the screen, 0.45 down, until set). MORE SPEED and the
  gear badge stay under the gap row and beside the cue box until their `_x` / `_y` are written.
- Every key is optional: a hud.ini without them looks exactly as before. Re-read about once a second.
## 2026-10-02 - v0.48.2

MXB Coach and FrostMod: fewer driver round trips per frame (frame rate with a heavy HUD plugin).

- Every glGet* / glGetError is a round trip to the driver's worker thread. The ground line asked
  about nine per frame (error drains, framebuffer, program, active texture, twice over for the marks)
  and FrostMod's overlay one more. Now: the program is followed through the glUseProgram hook, the
  active texture is restored by glPopAttrib, the framebuffer, error checks and the overlay's viewport
  are asked every 32nd to 64th frame, and the marks share the line's state.
- The fixed-function modelview search (about 1000 loads a frame) is skipped on menu, pause and
  loading frames.

## 2026-10-02 - v0.48.1

MXB Coach in-game line: a relock after a crash has to be the camera that was drawing.

- After the bike was put back, the search took whichever fixed-function load had its eye nearest the
  bike, and the eye watch then learnt that load's distance as the usual one, so a wrong object could
  become "the camera" and the line stayed misaligned. The distance and direction in which the drawing
  camera holds the bike are now learnt while it draws, and a relock must hold the bike the same way
  (1.5 m, 30 degrees); a load that doesn't is counted (`camera_not_where_it_was` in the 10 s log) and
  skipped. After 3 s riding with no load that agrees the view is forgotten and the search starts
  over, so changing camera still works. The agreement is checked on every load, every frame.
- Once the axes are settled, "a model matrix at the bike" is tested under those axes only. Under all
  48 a true chase camera heading near a compass point on a track near the origin matched some
  permutation of the bike's coordinates and was refused.

## 2026-10-02 - v0.48.0

MXB Coach in-game line: near fade, pace and gear hints on by default, steady jump marks.

- **Near fade.** The line fades out toward the rider so the ruts it runs through stay visible
  beside the bike: clear at the rider, solid `line_fade` metres ahead (default 8, 0 = off, up to
  30). The fade is worked out from the rider's place on the line each frame, not from the last
  rebuild, so it does not step as the ribbon rebuilds.
- **Pace arrows and gear hints.** Both were off until `pace=1` / `gear=1` was in `hud.ini`, and
  Coach only wrote them when switched on in its HUD list, so a rider who never did saw neither.
  They are now on whenever the line is (`pace=0`, `gear=0` turn each off).
- **Jump marks no longer wobble.** Takeoff bars, landing boxes, flight dashes and labels were placed
  by their distance from the rider on a ribbon rebuilt only every two metres, so each moved up to
  two metres and snapped back at every rebuild, and took the ground correction held for a place
  they weren't at. They are now placed by their metre on Coach's line and take the correction for
  that metre. Takeoff bar spread over an approach, in the test: 2.0 m before, 0.0 m after.

## 2026-10-02 - v0.47.0

### Fixed
- **MXB Coach: the game no longer hangs when the line samples the game's ground (0.46.0).**
  0.46.0 asked the game's height query from inside RunTelemetry while holding the plugin's lock,
  from the very first telemetry tick, and located it with GetModuleHandle (the loader's locks)
  under that lock too; the game's Draw waits on the same lock, and SavageMX hung on the first
  tick. Now: nothing that asks the game holds the lock; the exe's base comes from the PEB; it
  starts only after 3 s of riding and once the slot's heightfield reads the same twice a second
  apart; at most 1 ms of asking a slice and a tenth of wall time; a watchdog turns it off for the
  event if a slice runs over 8 ms or 2 s in all; and a marker file (version only, no heights)
  turns it off for good on that version if the game ever hangs or dies mid-way. The line falls
  back on the track's grid file or the depth snap.

## 2026-10-02 � v0.46.2

### Fixed
- **MXB Coach: the line no longer climbs rut walls.** The ribbon took each edge's height from
  the ground under that edge, so a rut wall under one edge of the 0.7 m line tilted it up the
  wall. The line now stands on the ground under its centre (the path actually ridden) and its
  edges lean only as the ground does over a wider window, within 8 cm of it: a berm's lean is
  kept, a wall is cut down to it.
- **MXB Coach: jumps are no longer called SINGLE from a guess.** On a sheet without the lap's
  air (an older one, before Coach's first whole lap on the track) the flight is predicted from
  each lip, and the prediction runs short of the real flight: on four tracks whose sheets have
  both, it named 17 of 58 jumps right and said SINGLE for most of the rest (the lap had cleared
  doubles, tables and steps). A predicted jump is now a plain JUMP at its lip, with no landing
  box; the named calls (SINGLE, DOUBLE, TABLE, STEP UP and so on) come from the lap's own air.

## 2026-10-02 — v0.46.1

### Added
- **MXB Coach: the line's look is the rider's.** `hud.ini` takes new optional `[hud]` keys, set
  from MXB Coach's Settings and picked up live (the file is re-read every second):
  `line_width` (×0.25–3), `line_opacity` (0.1–1), `col_gas`, `col_coast`, `col_light`,
  `col_heavy` (the line's gradient), `col_fast`, `col_slow` (the pace hints), all `#RRGGBB`;
  `line_text` (0 hides the jump calls and their speed, the gear sign on the line and MORE SPEED,
  leaving just the line); `text_size` (×0.5–2); and `text_style` (`block`, `bold` or `italic`:
  the on-line block font drawn fatter or leaning; the game's own font, used for MORE SPEED, has
  neither). A key that isn't there is today's look, so an older Coach changes nothing.

## 2026-10-02 — v0.46.0

### Added
- **MXB Coach: the ground line on locked and secured tracks.** The line now sits on the game's
  own ground, whatever the track: `mxbcoach.dlo` asks the game's height query (the sampler
  FrostMod guards, `mxbikes.exe+0x1F1720`) for the track's ground at every half metre, a
  millisecond and a half per telemetry tick, so a 550 m track is ready a few seconds into the
  first stint. It is used ahead of a `<track>.ground` file (it is the surface the physics rides
  on, and at 0.5 m against the file's ~1 m) and checked against the rider the same way.
  - Memory only: the grid is never written to disk, logged, or sent to the app, and is dropped
    at the end of the event. The game is only read: the query writes nothing but its answer.
  - Only on the build the addresses were read from (beta21e): the query and the accessor the
    track slots are decoded out of must match their signatures at their addresses exactly.
    Anything else, or a fault, and the line works as before (the grid file, then the depth snap).
  - Log: `terrain:` lines say whether it was found, which track slot was sampled, when it was
    ready and how much answered, and whether it lines up (`game ground aligned ...`).
## 2026-10-02 — v0.45.4

### Fixed
- **MXB Coach: v0.45.2 froze the game and made menu clicks wait.** Its asynchronous depth reads
  (pixel-pack buffers, fences polled from the swap hook) ran on every frame the line had a camera,
  the pause screen and menus over the track included, read every snap point each time, and never
  flushed their fences. They are gone: the depth is read with a plain `glReadPixels` again, at
  most once a second for all probes together, and only while riding (a telemetry sample and an
  on-track HUD draw both within 300 ms). In a menu, the pause screen or a loading screen the swap
  hook builds, reads and draws nothing. The snap reads only its check until the check agrees.
  The v0.45.2 axes lookup and the grid re-read-on-change are kept.

## 2026-10-02 — v0.45.3

### Fixed
- **MXB Coach: the track's own ground is no longer thrown away on a supercross track.** The
  check that `<track>.ground` lines up with the rider used the first 200 riding samples, and on
  a supercross track those are all the run out of the gate, which stands past the edge of the
  track's terrain (Steezy Mx - SMX - Carson: a 170 m terrain, the gate at x=193). Only 20 of them
  landed on the grid, under the half it needs, so the right ground was refused for the session
  ("0 samples on the grid - NOT this track's ground"). Now only samples on the ground with the grid under them count, a metre
  apart; a grid the laps never land on is still refused, after a minute of riding off it. The log
  line says how many were off it.
- The gear line no longer blames an old MXB Coach when there is simply no sheet for the track yet.

## 2026-10-02 — v0.45.2

### Fixed
- **MXB Coach: stutter with the ground line on.** Its depth reads (the snap every 200 ms, the
  depth-mode probe every 2 s) were synchronous `glReadPixels`, each one a full CPU-GPU pipeline
  flush: a hitch about five times a second even on a fast PC. They now go into pixel-pack
  buffers with a fence and are picked up a frame or two later, without ever waiting, and are
  decided through the camera of the frame they were read on. A GL without buffer objects or
  fences keeps the old reads, at most once a second. The game's pack buffer binding and pixel
  store state are left as they were.
- The camera's axes are no longer looked up by building 96 strings a frame, and a track grid
  file that doesn't parse is no longer re-read whole every second.

## 2026-10-02 — v0.45.1

### Reverted
- **Paint sync at join time (v0.43.0) is reverted.** `frostmod.dlo` is back to v0.42.2's paint
  handling: `refresh_paints` runs when MXB App asks, with no loading-screen/pits gate, the
  `paints_staged` command is gone, and so are the `[paintgate]` / `[paintscan]` log lines.
  MXB Coach (`mxbcoach.dlo`) keeps everything through v0.45.0.

## 2026-10-02 — v0.45.0

### Added
- **Gear hints** (hud.ini `gear=1`, off until asked for; the sign on the line needs `ground`
  too, the badge doesn't). MXB Coach now writes the gear its lap was in at every point (a `GEAR`
  chunk in the `.hud` sheet; an older sheet has none and shows no hints). Where Coach's lap
  changes gear, and you are in neither of his two gears, an arrow and the gear to be in
  (**▲3** shift up to 3, **▼2** down to 2) stand on the line 6 m before the shift, and a small
  badge with the same arrow and number sits beside the cue box. At the shift and for 12 m after
  it, only his new gear is right, so a rider still in the old one is told to change.
  - Quiet: nothing while crashed, crawling, in neutral or in the air (unless Coach's own shift is
    in the air, as on a jump's face). A hint needs 0.3 s to start, 0.7 s of not being wanted to
    end, shows at least 1.2 s, fades in and out, and goes from up to down only through nothing.
  - The log says what it saw (`gear: up to 4 (gear 2, Coach shifts 30 m ahead)`).

## 2026-10-02 — v0.44.1

### Added
- **Jump calls on the line.** Where Coach's lap jumped, the line now shows a white bar across it
  at the lip, a faint box where the lap landed with a dotted arc of its flight between, and a
  label standing over the lip, readable from 30-50 m, named the way a rider would: **SINGLE**
  off a real lip, **DOUBLE** over a gap onto a separate landing face, **STEP UP** / **STEP
  DOWN**, **TABLE** clean over a flat top, **JUMP ON** / **JUMP OFF** when the lap landed on a
  table or left its top, and in a supercross rhythm lane (three or more crests close together)
  **SINGLE** / **DOUBLE** / **TRIPLE** / **QUAD** by the crests cleared. **ROLL** where the lap
  stayed on the ground over a big face. Under it, the speed the lap took off at (`45 KMH`).
- Hops and skips are not called: under 0.35 s or 5 m in the air, off a lip under 0.5 m, or a
  short flight (under 0.6 s) off a natural crest rather than a built face.
- The faces are counted on the track's own ground under the line (the sheet's TRRN). With the
  matching MXB Coach the sheet also carries where the lap was in the air (new `AIRH` chunk);
  an older sheet with the ground but not the air gets its flights predicted from each lip's
  angle and the lap's speed there, and no ROLL calls.
- A HUD option, `jumps` in `hud.ini` (on by default, shown only with the line on the track);
  MXB Coach lists it as "Jump calls on the line". The log says how many calls a sheet gave and
  where they came from (`jumps: 6 calls from the lap's air: DOUBLE=2 ...`).
- Built with v0.44.0's pace hints: both draw over the line, the pace chevrons and the jump marks
  side by side.

## 2026-10-02 — v0.44.0

### Added
- **Pace hints on the line on the track** (hud.ini `pace=1`, off until asked for; drawn over the
  line, so it needs `ground` too). Your speed is compared with Coach's at the same spot and
  with what is coming up. The line's own colours stay what to do (green gas, white coast,
  yellow/red brake); the hint is drawn over them:
  - **Too fast** (8% over Coach, or a braking-distance model saying you would run 4 m or more
    past his slowest point of the next corner): magenta chevrons on the line ahead point back at
    you up to the braking point, and its yellow and red come sooner by about that overshoot.
  - **Too slow** (8% under, never with a braking zone close ahead): cyan chevrons point on and
    the gas is a brighter green. Before a jump lip that needs speed (found from the track's own
    ground in the sheet), a cyan gate across the line at the lip and **MORE SPEED** on the HUD.
  - It doesn't flicker: the comparison is smoothed, a hint needs its threshold held to start and
    half of it held to end, shows at least a second, and fades in and out.
- The log says what the hints saw (`pace: too fast (+12% on Coach, overshoot 9 m, ...)`).
## 2026-10-02 — v0.43.5

### Fixed
- **No more jumping, sinking or crawling line on tracks without their ground file.** The
  correction taken from the game's depth swung by two metres: it was kept by distance ahead of
  you, so the ground slid through it as you rode, and nothing checked the depth reading meant
  what it was taken to mean. Now it is only used while the ground just ahead of the bike, read
  back the same way, agrees with the bike's own height; it is kept by place on the line, moves
  a few centimetres at a time and never more than 30 cm. The log says when it is on or off and
  why (`ground: snap on|off`).
- The line stands on Coach's own lap's heights along it when the sheet carries them (a newer
  MXB Coach), on the centreline's rise otherwise, and its rows sit at fixed places on the line,
  so a rebuild as you ride draws the same rows in the same places. The log's 10 s line reports
  the most any of them moved (`jitter dy= shift=`).
- Depth 1.0 under the line (sky, or a cleared buffer) no longer makes the line draw over
  everything for a few seconds.

## 2026-10-02 — v0.43.4

### Fixed
- **The line lies on the track's own ground from the first second, no laps needed.** MXB Coach
  now reads the track's height file the moment it sees you ride it and writes it beside the
  sheets (`<track>.ground`); the line stands on it everywhere, lifted a little more where the
  ground is steep so it never dips into lips and bumps. FrostMod checks once, from your own
  riding, that the file lines up with the track and logs it (`ground: trh aligned dx,dz,dy`).
  Locked (.mxbsecure) tracks can't be read, and there the line snaps to what the game draws.
- **Crashes.** The line hides while you are down and for a moment after you get up, and after a
  crash or the game putting you back on the track it starts again from where the bike really
  is: the camera is found again, the line re-anchored, the ground re-read. It never draws from
  an old camera after a crash. A camera that drifts away from the bike (the crash camera, a
  replay view) is let go within 0.3 s.
- **Blinking.** The line fought the track's own surface for the same pixels; it now sits a few
  centimetres clear and wins the depth test by a margin. No value that isn't a number can reach
  the line any more, and the ground snap ignores single bad readings (rain, a rider passing).
  `snap=nan` in the log only ever meant "no readings": it now says `snap=off` when the line is on
  the track's own ground and doesn't need one.

## 2026-10-02 — v0.43.3

### Fixed
- **The line no longer swings across the track.** It started from where Coach's sheet said you
  were along the lap, and on a lap ridden longer or shorter than the track's centreline that is
  tens of metres off by the end of a lap (49 m on a 6% longer lap), so the line appeared across
  the corner ahead, turned 90 degrees, with its colours and heights from the wrong place. It now
  starts from where you actually are and runs along Coach's line from there.
- **It sits on the dirt even without the track's ground in the sheet.** It stands on your own
  ground and follows the centreline's rise from there, then reads what the game drew under it a
  few times a second and moves onto that surface, so it neither floats nor sinks out of sight.
- **Gas is green.** Without Coach's throttle and brake in the sheet, steady speed read as
  coasting; now anything that isn't slowing is gas. With them, coasting is a throttle under
  about 15%. The blend runs over time as well as distance.
- **The camera lock can't settle on a rotated copy of the view.** On a tie, the plain axes win.
- The log now says whether the sheet carries the track's ground and why not (`ground: sheet
  terrain=...`), where the colours come from (`ground: colours: source=DRIV|speed|cues`), and
  the 10 s line counts picked and drawn frames for those 10 s, with the ground snap's state.

## 2026-10-02 — v0.43.2

### Fixed
- **The blue line lies on the dirt instead of floating over it.** With a sheet from the
  matching MXB Coach, the line follows the track's own ground under each edge, every half
  metre: over the whoops, up the berms, leaning with the camber, 2 cm up and depth-tested so
  bikes and the ground in front hide it. Older sheets keep the centreline heights.
- **No more glitching.** The camera is now followed from frame to frame: a frame without it no
  longer drops the line (the last camera carries it for a tenth of a second, and the lock holds
  for half a second), and a different camera for a single frame is never drawn from. The log
  says every 10 s why any frame with a camera drew nothing (`ground: not drawn: ...`).

### Changed
- **The line shows how Coach's lap was ridden, everywhere, not just at the cue.** Green on the
  gas, white coasting, yellow on light braking, red on heavy braking, fading from one to the
  next over a few metres with no seams (a colour per point, blended across the ribbon). It
  comes from Coach's throttle, brake and speed when the sheet carries them, otherwise from the
  lap's own times; the cue sheet's brake cues only when neither can say. On 755 Compound that
  is 11 braking zones a lap.

## 2026-10-02 — v0.43.1

### Fixed
- **The blue line on the track finds MX Bikes' camera where it really is.** The game hands
  its shaders nothing (v0.42.2 counted zero) and builds each object's position from the camera
  on the CPU, so the camera is now read (never changed) from those per-object matrices: the
  track's own one is the camera, and it is kept once it has followed your bike for 30 frames
  (`ground: camera locked: fixed-function modelview ...`).
- **A line even when no camera is found.** After two seconds without one, the line is drawn
  from a helmet view worked out from the bike's own position and direction of travel
  (`ground: camera=fallback-onboard`). It is an approximation, made for the onboard camera;
  the chase and TV cameras get no line from it.
- The log's first 30 seconds now also count every other way the game might pass a camera
  (`ground/diag entry points: ...`).

## 2026-10-01 — v0.43.0

One version for every binary in this repo: `frostmod.dlo` and `mxbcoach.dlo` both report
0.43.0. MXB Coach is unchanged from v0.42.2 (below).

### Changed
- **Synced paints no longer refresh while you ride.** MXB App's paint sync used to make
  FrostMod rebuild the paint lists and repaint every rider the moment a paint arrived, up to
  three times per sync, on track. That stalls the frame the network shares, which froze or
  dropped riders. Now a refresh waits for a moment where a hitch costs nothing: the join's own
  loading screen (at most once per join), the pits, or the menus. Asks that arrive together are
  one refresh. MXB App's full content reload waits for the pits the same way.
- New command `paints_staged` (MXB App's paint sync): repaints only riders whose paint was
  missing, plus anyone who arrived while the lists were rebuilt, instead of every rider.
  `refresh_paints` (your own look) still repaints everyone, and now also waits while riding.

### Diagnostics
- `[paintgate]` lines: when a join starts (the connection dialog), phase changes (menu, joining,
  pits, riding), when `EventInit` names the server, when a refresh runs, and what it cost
  (`[paintgate] cost:` - total, longest frame, and the paint pass).
- `[paintscan]` lines count the game's own paint-folder scans apart from FrostMod's, with the
  time since the join started. One join answers whether the game rescans paints on its own.

## 2026-10-01 — v0.42.2

### Fixed
- **The blue line on the track takes the camera from the game's shaders.** v0.42.1 showed the
  game's old-style camera never moves: the real one is handed to the shaders. FrostMod now
  reads (never changes) the camera matrices the game gives its shaders, keeps the one whose
  eye follows your bike for 30 frames running, and logs which (`ground: camera locked: ...`).
- The line is drawn over the scene until FrostMod has looked at the game's depth buffer under
  it; then the bike and the hills hide it if that buffer holds the scene (`ground: depth at
  the line ...`).
- For the first 30 seconds the log lists, once a second, the matrices nearest a camera at your
  bike (`ground/diag`).

## 2026-10-01 — v0.42.1

### Fixed
- **The blue line on the track now finds the camera.** In v0.42.0 the game's camera was seen
  hundreds of times a second but never accepted, so nothing was drawn. The camera is now
  followed however the game builds it (whole, or turned and then moved), and the way the
  game's 3D axes line up with the bike's position is worked out on track rather than assumed:
  once the same answer holds for 30 frames in a row it is kept, and the log says which
  (`ground: axes locked: ...`).
- For the first 30 seconds with the line on, the Coach log writes once a second where the bike
  is and where each camera the game used sits (`ground/diag` lines), so a track where it still
  does not appear can be fixed from the log alone.

## 2026-10-01 — v0.42.0

### Added
- **MXB Coach draws the line to take on the track itself**, not only on the map: a blue
  ribbon on the ground for the next 60 m of Coach's line, fading out with distance, red
  through Coach's braking zones when the cue sheet has them. The bike and the hills hide it
  the way they hide the track. It comes on with the map's trail (`trail=1`), or on its own
  with `ground=1` in `hud.ini`; `ground=0` keeps it off.
- It only draws through the chase and helmet cameras, and only once the game's own camera
  has been found next to your bike. Anything else (a TV camera, the menus, a replay) and it
  stays away rather than drawing a line in the wrong place. The Coach log says what it found
  every ten seconds (`ground:` lines).

## 2026-09-28 — v0.41.0

### Added
- **FrostMod can run purely as a game plugin**, with nothing injected: MXB App can install it
  as `pluginsrostmod.dlo` and stop starting `frostmod.exe`. As a plugin it loads before the
  game scans the mods folder and gets the game's session events itself, so the separate
  `frostmod_session.dlo` copy is no longer needed.
- **`frostmod.dir`**: a one-line file beside `frostmod.dlo` naming the folder FrostMod keeps
  its files in (log, settings, flags, the MXB App command file, crash reports). MXB App points
  it at its own FrostMod folder, so nothing ends up in the game's `plugins` folder.

### Fixed
- **Only one FrostMod per game.** If a plugin copy and an injected copy both end up in the
  game, the second one now stands down instead of installing the same hooks twice. The log
  says which mode FrostMod is running in.

## 2026-09-28 — v0.40.4

### Fixed
- **One more mid-race crash blocked (`mxbikes.exe+0x1F1EFC`).** It is the same game bug
  as the ground-height crash FrostMod already blocks: when a physics calculation briefly
  produces an invalid position, a second ground lookup in the game read far outside its
  map and took the game down. FrostMod now turns that lookup away for an invalid position,
  as the game does for a position off the map. A search of the game code found no other
  lookup with the same flaw. Seen in a player's crash report mid-race.

## 2026-09-28 — v0.40.3

### Changed
- **Riders show new helmets, boots and rider models without rejoining.** When MXB App brings
  in a gear model that a rider already on the server is wearing, FrostMod rebuilds that rider
  with it as soon as the gear lists are refreshed, if you are in the pits. If you are riding,
  it waits until you are back in the pits: while you ride the game holds off building riders,
  and one rebuilt then would stay invisible.
- **"Rebuild riders (load new gear models)" is now a normal F8 item (F8 → 8)**, no longer
  behind the developer menu. It rebuilds every other rider with their gear loaded fresh, and
  only runs in the pits.

## 2026-09-27 — v0.40.2

### Changed (developer menu only, `devmenu=1`)
- **"Rebuild riders" now loads new gear models** (helmets, boots, rider models installed
  since a rider joined), not just paints. Each remote rider's objects are removed the way the
  game removes them at session end, then the game rebuilds the rider in full from the updated
  gear lists. Use it after a gear refresh. It only runs in the pits: while you ride, the game
  postpones rider builds and a rebuilt rider would stay invisible. Not yet tested in game.

## 2026-09-27 — v0.40.1

### Fixed
- **Changing bike on track no longer leaves you on a black screen.** The F8 "Change bike /
  gear" panel now only opens in the pits; on track it says "Go to the pits to change bike".
  A change the server accepted while you were on track made the game build the new bike in
  the pits while your ride carried on without one, which left the screen black until you went
  back to the pits. FrostMod reads which screen you are on from the game itself.

## 2026-09-27 — v0.40.0

### Added
- **Race mode shows the game only the tracks and bikes a race needs, without moving files.**
  When MXB App writes `frostmod_racemode.txt` next to `frostmod_mods.txt` (one mod per line,
  relative to `mods/`, e.g. `tracks/Red Bud` or `bikes/KTM 450.pkz`), the game's scan of
  `mods/tracks` and `mods/bikes` skips everything else, so a hidden `.pkz` is never even
  opened. Only the folders the file names anything under are filtered; rider gear, tyres and
  the stock game are untouched. No file means no filtering, so a crash never leaves a player
  with a short list. A new `race_filter` command re-reads the file and reloads mods, so a
  join from the in-game browser can be slimmed mid-session. The log says how many entries
  each scan hid (`[racemode] N of M tracks/bikes entries hidden`).

### Fixed
- **Changing bike no longer leaves the old one behind.** After a change in the pits (F8 → 0)
  the game builds the new bike next to the old one and never removes the old: it stayed on
  the pit stand with you spawned inside it, HUD plugins showed two speed/gear readouts, and a
  change made on track left a frozen rider behind. FrostMod now removes the old bike once the
  game has switched to the new one, the same way the game clears bikes at the end of a
  session. It only acts after a change FrostMod sent and the server accepted.

## 2026-09-27 — v0.39.4

### Fixed
- **No more long freeze when the game starts or you join a server.** A reload request that
  MXB App sent while the game was closed (after a paint sync, or when it saw your mods folder
  change) stayed waiting, and FrostMod acted on it the moment the next game started: a full
  rescan of every track, bike and piece of gear, right while the game was loading or joining.
  With a big mods folder, or one on another drive, that is a 10-20 second freeze. FrostMod now
  drops a request that arrived before the game started; the game reads the mods folder itself
  when it loads.

## 2026-09-27 — v0.39.3

### Removed
- **The radar and the rider outlines.** Both overlays, their F8 entries (old `4` and `5`),
  their settings and the OpenGL hooks the outlines needed are gone. The F8 menu now holds
  only what players use: reload mods, change bike / gear, bike model swap, server
  announcements, overlay size, the corner hint and hide-overlay. The rider data MXB App's
  voice chat reads is unchanged.

### Added
- **Change bike, paint or gear without leaving the server (F8 → 0).** In the pits, pick a
  different bike, bike paint, helmet (and its paint and goggles), suit, gloves or boots from
  what you have installed, and press Enter. FrostMod asks the server through the game's own
  change request; if it agrees, everyone sees your new bike or look without anyone rejoining.
  The server decides what is allowed - on an mxbserver: pits only, practice or warmup, bikes of
  the server's class - and the panel tells you why when it says no.
- **Installing a helmet, boots or rider model only rebuilds the rider lists.** MXB App can now
  ask FrostMod for a gear-only refresh (`refresh_gear`), which rescans helmets, boots, rider
  models and protections and their paints instead of every track and bike in the game. Each
  step of a reload is timed in the log.
- **Rebuild riders (F8 → 9, experimental).** Rebuilds every other rider with the game's own
  re-load, so paints you installed while they were on track show on them. It keeps them
  visible and hittable, and the "joined" message it would print is hidden. A new helmet or
  boots MODEL still needs them to rejoin.

## 2026-09-27 — v0.39.2

### Fixed
- **A rider rejoining no longer risks crashing your game.** When someone leaves and comes back,
  your game keeps their old bike and rider around and rebuilds them through a shortcut that
  reads a series entry using the bike's number - far past the end of the list. Usually that
  memory happens to be readable and you get the rejoined rider drawn on top of the one who left;
  sometimes it is not and the game crashes (seen live, in the middle of a practice session).
  FrostMod now clears a departing rider's bike and rider the way the game does at the end of a
  session, so a rejoin is built from scratch like a first join. MX Bikes only; part of the
  existing rejoin fix (`rejoinfix=0` in `frostmod_radar.cfg` turns it off).
- **A rider who rejoins shows their paint again.** When MXB App's paint sync asks FrostMod to
  refresh paints, FrostMod only re-applied paints that had just arrived. A rider who left and
  came back already had their paint installed, but the game rebuilds a returning rider without
  painting them, so they rode stock. A paint refresh now re-applies every other rider's
  installed paints.

## Unreleased

### Added
- **Release binaries can be code-signed.** The release build now signs frostmod.exe,
  frostmod.dll, frostmod.dlo, frostserver.exe, frostserver.dll, frostserver.dlo and
  mxbcoach.dlo with Azure Artifact Signing before it zips or uploads them, so Windows
  SmartScreen and antivirus see a verified publisher. It stays off until the Azure secrets
  are added to this repo; until then releases are unsigned, as before.

## 2026-09-27 — v0.39.1

### Fixed
- **A paint sync no longer stutters the game.** When MXB App downloaded or removed paints
  mid-session, FrostMod rebuilt every content list - tracks, bikes, tyres and the rest - which
  on a big mods folder is a noticeable hitch. It now rebuilds only the six paint lists (bike,
  suit, gloves, boots, helmet, goggles) and applies what arrived. Several requests in one sync
  collapse into one refresh, and a full reload asked for meanwhile still runs right after.

## 2026-09-26 — v0.39.0

### Added
- **The in-game pill reads "Game Integration v0.39.0 - F8"**, in mxbsecure blue on a lighter, more see-through black.
- **A paint you download mid-session shows up on that rider straight away.** Your game picks
  each rider's paint once, when their bike is built. If you don't have their paint then, they
  ride stock until you rejoin, even after the file lands in your paints folder. Now, when you
  reload mods (R / F8, or MXB App's Reload), FrostMod checks the riders who were stock for
  want of a paint and applies any that are now installed. Riders already showing their paint
  are not touched. MX Bikes only. Deleting a paint still leaves it on their bike until you
  rejoin.
- **The same goes for their gear.** A suit, gloves, boots, helmet or goggles paint that lands
  mid-session is applied on the next reload too, as long as you already have the helmet or
  boots model it belongs to. A helmet or boots model that is new to you still needs a rejoin.
- **MXB App's paint sync can trigger it for you.** FrostMod now answers the app's
  `refresh_paints` command, which the app sends after it downloads or removes paints, so the
  new looks appear without pressing R.

### Fixed
- **One crash is one crash report.** When another plugin's crash handler kept resuming a fault,
  the game could sit on the same broken instruction for half a minute, and FrostMod wrote a new
  report every half second (44 of them for one crash on OneTwoSixProvingGrounds). FrostMod now
  reports the fault once, notes that it was resumed, and after the third time on the same
  instruction lets the game close instead of hanging.

## 2026-09-24 — v0.38.0

### Fixed
- **MXB Coach starts coaching on the lap after your first good one, without restarting the
  game.** On a track Coach hadn't coached you on yet, the recorder only looked for Coach's cue
  sheet when the track loaded. Coach writes that sheet a few seconds after your first good lap,
  so the cues, the gap and the ghost only turned up after you quit and loaded the same track
  again. Now, while there's no sheet, the recorder checks for one about once a second and
  starts using it straight away, mid-lap included. A sheet already in use is still swapped only
  at the line. The gap and ghost sheet now updates during a session too; before, it was only
  read when the track loaded.

## 2026-09-19 — v0.37.0

### Fixed
- **The in-game server browser is unstuck as often as it sticks.** Come out of a server, open
  Browse, and the game can sit there saying "connection timeout" at a master server that is
  answering everybody else. FrostMod spots that and clears it for you, every time it happens,
  so an evening of hopping between servers keeps a working list the whole way through. When
  the master server itself is out it stands back and says so in the log instead, because
  nothing on your machine fixes that one.
- **A rider who leaves and comes back no longer arrives on top of the rider they used to be.**
  When someone drops out mid-session and rejoins, everyone else's game can end up drawing their
  old bike and their new one in the same place, the two wound through each other. It has been
  worse since the last game update: it used to take a bike change to show up, and now the same
  bike does it. Underneath, your game files a departing rider away in two places and the
  disconnect only empties one of them. The half holding their machine stays behind, and when
  they come back your game finds that leftover first and pours the new session into it.
  FrostMod empties both, using the game's own routine for it, so a returning rider arrives
  clean the way a first-time joiner does. MX Bikes only, nothing goes over the network, and it
  helps whoever has it installed whichever server they are on. Off by setting `rejoinfix=0` in
  `frostmod_radar.cfg`.

### Added
- **Servers can put a message on your screen in colour.** Anything a server says in chat comes
  out the same colour for everyone, because your game picks that colour and nothing the server
  sends changes it. So these do not come through chat. A server running FrostServer publishes
  its announcements, FrostMod fetches them from the server you are already on, and draws them
  itself just above the game's chat: any colour the admin picked, optionally pulsing, breathing
  or running through a rainbow, with small icons for a flag, a warning, a clock, a trophy and a
  few more. Ordinary server chat is untouched and still arrives, so riders without FrostMod
  lose nothing and see exactly what they saw before. Admins write the lines in
  `frostserver.yaml`, can have one go out whenever the track changes, and can send one live
  over HTTP with a password they set. You can turn the whole thing off in the F8 menu under
  "Server announcements", and that choice sticks. FrostMod only ever talks to the server you
  have joined, only while you are on it, and stops asking a server that has no FrostServer.

## 2026-09-18 — v0.36.0

### Added
- **Riders who used to be invisible are now drawn.** On a full gate there are riders you simply
  cannot see, with no warning that anyone is there. They are not lagging and they have not
  crashed out. Everyone else can see them perfectly well, and one of them can still land on you.
  It is worst on whoever is furthest away, which in a race usually means the leader, so you can
  be seconds off the lead with an empty track in front of you. Your game only draws a rider it
  has two recent enough positions for, and on a full gate the updates for the riders furthest
  from you arrive too far apart to qualify. FrostMod widens how far apart those updates may be,
  so those riders stay on screen. A rider drawn this way is smoothed. He glides rather than
  tracks, because there is genuinely less information about him, but you can see him and leave
  room. Off by setting `antifreeze=0` in `frostmod_radar.cfg`, where `antifreezems` sets the
  window in milliseconds (default 2000). MX Bikes only. It does not help with a rider the
  server has stopped sending anything about at all; that one still freezes, as it always did.
- **FrostServer stops the problem at the source, for everybody on the server.** The half above
  is on the player's machine and helps whoever installed it. This one runs on the dedicated
  server and helps every rider connected to it, including riders running no mod at all. On a
  full gate the server cannot fit everyone into one update packet, so it sends each player the
  riders nearest them and drops the rest. The riders furthest away get dropped over and over
  until they vanish. FrostServer gives a rider who is close to vanishing a slot in the next
  packet, ahead of somebody nearer who has updates to spare. No extra packets are sent and the
  packets do not get bigger, so the same bytes go to the same riders at the same rate. A rider
  kept on screen this way is in his real position rather than a smoothed one, which is why this
  is the better half of the fix. Drop `frostserver.dlo` in the dedicated server's `plugins`
  folder and it is on; `fair_send: false` in `frostserver.yaml` turns it off. Watch
  `frostserver.log` for the line saying this server was starving riders, which is how an admin
  knows it was happening here at all. MX Bikes only. Setup and limits in `docs/FROSTSERVER.md`.

## 2026-09-17 — v0.35.0

### Added
- **"Roll off here."** MXB Coach can now tell you when you are going long over a jump, and the
  recorder says it at the lip, where you can still do something about it. Spoken in both
  voices, and gated to the faster rider levels: easing off before a lip is only advice once you
  are clearing it every lap.

## 2026-09-17 — v0.34.0

### Fixed
- **A first go at the server browser saying "connection timeout".** Leave a server, open Browse
  again, and the game times out on a master server that is answering everyone else. Nothing in
  the menus brings the list back. Reading the game's code, it leaves its connection to the
  master half-open when a session ends, and its own browser will not open a new one while the
  old one is still there. FrostMod watches for that and closes it out for the game. **Not proven
  yet:** this is built from the disassembly, and nobody has watched it clear a stuck browser.
  The log says what it did. MXB App's Servers tab has a button for it as well, for the cases
  FrostMod deliberately leaves alone.
- **The crash at track load is stopped.** This is the one that takes the game down while a track
  is loading, before you ride — the most common MX Bikes crash there is, about two in five of
  every crash reported. The game saves your trainer with two of its fields never filled in, so
  whatever happened to be in memory at that moment is written into the file as if it were text.
  Next time that track loads, the game reads it back and tries to use it as a name, and walks
  off the end of memory. FrostMod now corrects a trainer as it is read, so the ones already on
  your disk load instead of crashing, and again as it is written, so no new one is spoiled.
  Trainers saved from the trainer screen keep their real settings; only fields that are not
  text are cleared.

## 2026-09-17 — v0.33.0

### Changed
- The log records what the game's drawing looks like from inside, the first time you go out on
  track. This is groundwork for drawing a line on the ground ahead of you, which needs to know
  where the camera is — something we have never been able to read, and until now had only
  looked for while sitting in the menu. Nothing is drawn differently; it writes a few hundred
  lines to the log once per run and stops.

## 2026-09-17 — v0.32.0

### Changed
- **The suspension is drawn on a bike.** It was two bars in a corner with an F and an R beside
  them, which tells you the numbers and nothing else. It is a bike from the side now: the travel
  fills down the fork leg and along the shock, where your eye already expects them, with a mark
  at the deepest each end has been. Drag it wherever you want it like the rest.

## 2026-09-17 — v0.31.0

### Added
- **The fence crash now says where it came from.** v0.30.0 stopped the game going down when a
  contact leaves the bike's position as something that is not a number. It could not say what
  made it. The first three times it happens in a session, the log now records the calls that led
  there, which is something a crash dump could never have shown: by the time the game used to
  fall over, the code that produced the bad number had long since finished. If this has been
  happening to you, your log now carries the answer.


## 2026-09-17 — v0.30.0

### Fixed
- **Going into a fence should not close the game.** A hard crash can leave the bike's position
  as something that is not a number, and the game then asks the track how high the ground is
  at that spot. The answer it reads is far outside the track, and the game goes down. FrostMod
  now declines that question the same way the game itself declines a spot outside the map.
  This is the crash a lot of people have been hitting mid-race.

## 2026-09-16 — v0.29.0

### Added
- **A crash now leaves a report MXB App can send.** Alongside the crash log and the dump,
  FrostMod writes a small file describing what happened: where the game faulted, the call
  stack, the track and server, how many riders were in the session, and what happened just
  before. MXB App picks that file up and sends it, so the same crash showing up for a lot of
  people is something we can see rather than guess at. The dump stays on your machine.

## 2026-09-16 — v0.28.0

### Fixed
- **The old trainers crash is stopped.** MX Bikes closes a trainer file it has already closed,
  and the game goes down on the spot. That is the crash riders have been putting down to old
  trainers for years. FrostMod now catches it and lets the game carry on, exactly as the
  game's own code does everywhere else it touches a trainer.
- FrostMod says so when something has already hooked that close, instead of reporting it as a
  game update. Two copies of FrostMod in one game is the usual reason, and the second one now
  leaves the first one's hook alone.

### Added
- **A crash to desktop now leaves a report.** When the game dies, the log gets the fault, the
  call stack, and what was happening at the time: on track or in a menu, which track and
  server, how many riders were in the session, and the last things that happened before it
  went. A crash dump lands next to the log, and Settings > Send logs picks both up. Nothing is
  sent anywhere on its own.

## 2026-09-16 — v0.27.0

### Fixed
- **The cues change as you ride.** The recorder read your cue sheet once when the session
  started and never looked at it again, so the same calls came at the same places every lap and
  every session no matter what MXB Coach worked out in between. It now takes a newer sheet at
  the start/finish line, where the calls are re-armed anyway.
- **Right-drag actually moves things now.** It never could: the plugin only looked at your mouse
  while you were riding a lap, which is the one moment you are watching the track and not the
  HUD. It now watches every frame, so you can move a part in the pits, between sessions, or
  wherever you like.
- **You can see what you are pointing at.** The game hides the mouse pointer on track, so moving
  a part meant aiming something invisible. The recorder draws its own pointer; it appears when
  you move the mouse and fades out again a couple of seconds after you stop.
- **The male voice says the right words.** "Gas now" was coming out as a word nobody recognised,
  and "Stand up" ran on into half a sentence. The clips themselves were wrong: the voice that
  made them said the word and then kept talking, and the trimming that was supposed to cut that
  off needed a longer pause than the male voice leaves. Three clips carried a whole second
  utterance, and because each clip is levelled over its full length, the real word was quiet and
  the rubbish was loud. `brake`, `gas` and `stand_up` are re-cut; the rest were already fine.
- **The voice keeps working after you crash.** Going down silenced it for the rest of the run.
  It was stopping the sound device fifty times a second for as long as you were on the ground,
  which is enough to leave it unable to play anything again. It now stops once, when you crash.

## 2026-09-16 — v0.26.0

### Added
- **The gap line moves too.** The line showing your gap to Coach's lap and whether you're
  sitting or standing was the one part still nailed to the middle of the screen — so if you
  dragged the map or the cue box under it, there was no way to get it out of the way. Right-drag
  it like the rest.

## 2026-09-16 — v0.25.0

### Added
- **Move anything on the HUD.** Hold the right mouse button on the track map, the suspension
  bars or the cue box and drag it wherever you want it. Let go and it stays there. Right-click
  anywhere else still does whatever it did before, and `move=0` in `hud.ini` turns it off.
- **The map shows which way you are pointing.** Your marker was a square, which told you where
  you were and nothing else. It is an arrow now, pointing the way the bike is going, so the map
  reads at a glance.

### Fixed
- **Your own marker stops disappearing off the map.** With the blue trail turned on there was
  not enough room left to draw it, and your dot and the ghost were the last things drawn — so
  they were the first to go missing. The map's line and the trail are now drawn at a sensible
  density for a map that size, and the last slots are held back for the markers that matter.

## 2026-09-15 — v0.24.0

### Fixed
- **The spoken cues keep talking for the whole session.** They used to stop after the first
  couple of clips and stay silent until you restarted the game. The recorder was reusing a
  sound buffer before your sound card had handed it back, and with only two buffers that meant
  two cues and then nothing. It now waits for each one to come back properly, and tidies them
  up every frame instead of only when the next cue is due.
- **"Gas" sounds like "Gas".** The clips were trimmed so tightly that the first sound of a
  short word was cut off, which is why it came out as "gss". Every clip has been regenerated
  with a gentler trim and a short run-in, and they are all checked to start on silence.

### Added
- **Pick the voice for your cues.** MXB Coach now offers a male voice as well as the original
  female one, and you can switch between them. Both are free, offline voices built into the
  plugin (credits in `NOTICE`).
- **Move the cue box.** The cues used to sit in the middle of the screen, right where you are
  looking. You can now put the box wherever you want it, and the section line follows it.
- **Suspension on the HUD.** A small bar for each end showing how much travel you are using
  right now, with a mark where it bottomed out on this run. Off until you turn it on.
- **See the line to take.** The track map can draw Coach's line as a blue trail ahead of you,
  so you can see where the lap wants you to go. Your own dot and Coach's ghost stay as they
  were. Off until you turn it on, and it only appears once Coach has sent a lap for the track.

## 2026-09-15 — v0.23.0

### Fixed
- **MXB Coach's live cues and HUD now appear on screen.** The recorder was asking the game to
  draw its text with a font it had never registered, so every line it drew — the cue, the
  section and its tip, the gap to Coach's lap, sit or stand, the setup card — was thrown away
  without a word. It now brings its own font and registers it, and the text shows. If you have
  never seen a cue in the game, this is why.

### Added
- **The recorder writes a log you can send us.** `mxbcoach.log`, next to your sessions, records
  what the recorder decided and why: whether it could draw text, which cue and HUD sheets it
  took or turned down, whether the session counts as practice, and whether the spoken cues
  could open your sound device. Each of those used to fail silently. It holds no rider name,
  no GUID and no server address.
- **MXB Coach can tell you which recorder you are running.** The plugin writes its version to
  `recorder.ini` when the game starts it, so the app can show the recorder that actually ran
  instead of taking your word for it.

## 2026-09-15 — v0.22.0

### Added
- **MXB Coach can say its live cues out loud.** When you turn the voice on in MXB Coach,
  `mxbcoach.dlo` speaks each cue as it shows in practice, like "Brake", "Gas" or "Stand up",
  at the volume you pick. It plays alongside your other plugins' sounds, such as a spotter,
  and a more important cue cuts off a less important one rather than talking over it. It's
  off until you turn it on. The voice is a free, offline one built into the plugin (credits
  in `NOTICE`). `src/coachvoice.h`, `tests/coachvoice_test.cpp`.
- **MXB Coach's in-game HUD.** In practice, `mxbcoach.dlo` now shows more round the cue:
  - the section you're in and a tip for it
  - your gap to Coach's lap ("vs Coach +0.34")
  - whether you're sitting or standing
  - a small track map with Coach's ghost and the next cue spots
  - your setup name when you stop, with a reminder to measure sag if Coach asks for one

  MXB Coach picks which parts show. The map stays off when MXBMRP3 is installed, since it has
  its own. The cue now sits lower, clear of MXBMRP3's panels. Parts of the map and gap code
  come from MXBMRP3 (credits in `NOTICE`). `src/coachhud.h`, `tests/coachhud_test.cpp`.

## 2026-09-15 — v0.21.0

### Added
- **The other riders in MXB Coach recordings.** `mxbcoach.dlo` now also records who else is
  on track, where they ride and their lap times, so MXB Coach can compare you with the rider
  just ahead and show the lines others take through each corner. It writes the event's riders
  (`ENTRY`), every bike's track position and x/y/z ten times a second with crashed and "this
  is you" flags (`POSITIONS`), and everyone's lap and split times (`RACE_LAP`, `RACE_SPLIT`),
  tags 12-15, only while a stint is recording. About 250 KB a minute with 20 riders.
  `src/others.h`, `tests/others_test.cpp`.

### Fixed
- **Sit and stand now read controller buttons bound in the game the usual way.** A Sit button
  on a gamepad is read from that pad. Buttons from controller plugins, and a Sit left unbound,
  are recorded as unknown.

## 2026-09-15 — v0.20.0

### Added
- **Sit and stand in MXB Coach recordings.** `mxbcoach.dlo` now notes when you sit and when
  you stand, so MXB Coach can show where you sat and where the fast lap stood. It watches the
  Sit button you set in the game's controls, on the keyboard or a controller, whether you hold
  it or press it to switch. With the game's automatic sitting switched on it can't tell, and
  the recording says so. `src/stance.h`, `tests/stance_test.cpp`.

## 2026-09-15 — v0.19.0

### Added
- **Live cues in `mxbcoach.dlo`.** In practice (a testing event, or a race event's practice
  session; never qualifying or a race), the recorder now shows the cues MXB Coach writes to
  `<save path>\mxbcoach\cues\<track>.<bike>.cue` (or `<track>.cue`): one short line, such as
  "Brake" or "Hold the gas", a moment before its spot at the bike's speed, on a dark backing
  near the top of the screen, through the game's `Draw` callback. The app picks the cues and
  sets the lead time, how long each shows, the gap between them and the cap per lap; the plugin
  holds a lower-priority cue when a more important one is due within the gap, skips a cue it
  has already passed, and clears on a crash. A sheet made for a track of another length is
  ignored. `src/coachcue.h`, `tests/coachcue_test.cpp`.

## 2026-09-14 — v0.18.0

### Added
- **`mxbcoach.dlo`, the MXB Coach lap recorder.** A new plugin, separate from FrostMod, that
  the MXB Coach app installs into the game's `plugins` folder. It records the rider's own
  telemetry at 50 Hz, with lap and split times and the track centreline, to one `.mxbc`
  file per stint under `<save path>\mxbcoach\sessions\`, for the app to turn into lap-time
  advice. It hooks nothing and draws nothing. Built and released alongside FrostMod.
  `src/mxbcoach.cpp`, `src/coachrec.h`, `tests/coachrec_test.cpp`.

## 2026-09-04 — v0.17.0

### Added
- **Session-only plugin mode.** A copy of `frostmod.dll` named `frostmod_session.dlo`,
  placed in the game's `plugins` folder, now loads with no hooks, no overlay and no
  offsets — it answers the plugin handshake and publishes the session to its own shared
  block, and nothing else. That block is how MXB App learns which server a rider is on:
  the server name arrives in `EventInit`, the game only calls that on a plugin it loaded
  itself, and the injected `frostmod.dll` is never asked. The mode is decided from the
  module's own file name, so there is no flag file to lose and no window in which it is
  not yet known. A hand-installed `frostmod.dlo` is a different name and keeps full plugin
  mode. `src/session.h`, `src/frostmod.cpp`, `tests/session_test.cpp`.

## 2026-09-01 — v0.16.3

### Added
- `tools/rederive` re-derives the offsets when PiBoSo ships a build. It decrypts the
  Steam DRM wrapper itself and looks each offset up by what it is — the strings a
  function owns, the imports it calls, the array it multiplies an index through — so a
  build that moves everything is a diff to read rather than a week of RE. 42 of the 60
  entries come back automatically; the 18 that are struct fields and protocol ids are
  carried from the baseline and always labelled as carried.
- `tools/rederive/selftest.py` proves it on known ground: the unpack matches Steamless
  byte for byte, every current MX Bikes offset comes back out unchanged, and the same
  rules find GP Bikes' content-init and folder-scanner unaided.

## 2026-09-01 — v0.16.2

### Fixed
- **MX Bikes no longer crashes the moment a session starts.** v0.16.1 read the overjump-crash
  setting by standing in front of the engine's command bus, and did it for everyone rather
  than only for whoever asked. One player's logs came back with 22 launches, 22 session
  starts and 22 crashes — every single attempt to ride, on the same instruction, where the
  build before it had run four hours without a fault. The probe is behind
  `--probe-overjump` again, so a normal run does not touch the bus at all.

## 2026-08-31 — v0.16.1

### Added
- Every session start now writes a line saying whether the overjump crash is on for that
  session, read from the settings the game starts it with.
- `--probe-overjump` adds a hex dump of that settings block. `--force-overjump-off` clears
  the crash flag as a session starts — offline and testing only.

## 2026-08-31 — v0.16.0

### Removed
- The in-game camera path editor. It is a separate mod now, with its own release.

### Changed
- `F7` still hides everything FrostMod draws for recording, and is now the menu's row `7`.
  It used to be one of the editor's rebindable keys, so it is a fixed key now.

## 2026-08-31 — v0.15.4

### Fixed
- After a model swap, the on-screen note says to switch bike category away and back.
  It used to say "re-select the bike", which doesn't load the new model — the mesh is
  cached until the category changes.

## 2026-08-30 — v0.15.0

### Added
- **Kart Racing Pro is a title FrostMod knows.** `--game krp` attaches to `kart.exe`, reads
  mods from `Documents\PiBoSo\Kart Racing Pro\mods`, and `--install-plugin` drops the
  `.dlo` into KRP's own `plugins` folder. The plugin loads and the overlay, the radar and
  rider outlines, and the session block MXB App reads all work there. Live mod reload does
  **not** ship for KRP: `kart.exe` is SteamStub-wrapped like `gpbikes.exe`, so its
  content-loader addresses cannot be read out of the file, and reload is refused rather than
  run at another game's addresses — which is what took GP Bikes down twice. One capture run
  on the game closes that gap; the recipe is in `tasks/kart-racing-pro-port.md`.
- **A title with no derived offsets now collects its own.** Instead of trusting an RVA it
  does not have, FrostMod sweeps `.text` for the scanner signature (byte-identical across
  these builds), hooks what it finds, and logs the RVA plus a stack walk per content
  category — one loader and one boot-init call site each. That log is the derivation. The
  stack-shot cap is raised from 16 to 96 for such a title, because GP Bikes' port was left
  guessing at 8 of its 13 categories when its log stopped short at 5.

### Fixed
- **Plugin mode now works on GP Bikes.** `GetModID` and `GetModDataVersion` answered
  `"mxbikes"` and `8` no matter which game had loaded the DLL, and a PiBoSo title silently
  drops a plugin whose identity is not its own — so the `.dlo` did nothing at all on GP
  Bikes, with no error anywhere to say why. All three exports now answer for the host
  process (`"gpbikes"`/12, `"krp"`/6, `"mxbikes"`/8), resolved at load time because the game
  asks before our init thread has run.
- **The radar reads each title's own structs.** The three games hand the same callbacks
  different payloads, and the code read all of them as MX Bikes': Kart Racing Pro's track
  position is 24 bytes where MX's is 28, so the size guard would have thrown away every kart
  on the grid and left the radar empty; GP Bikes' and KRP's classification entries carry an
  extra field ahead of the lap count, which the lap-status colouring would have read as a
  lap time. Every layout is now transcribed per title in `src/pluginsdk.h`, checked against
  PiBoSo's published examples by `tests/pluginsdk_test.cpp`.
- **`--install-plugin` installs into the title you pointed it at.** It looked for a running
  `mxbikes.exe` and warned about a missing `mxbikes.exe` whatever `--game` said, so
  installing for another title reported nonsense while quietly doing the right thing only
  when the folder was passed by hand.

## 2026-08-27 — v0.14.0

### Added
- **FrostMod now tells MXB App which server you are on, and where everyone is.** A shared
  block (`Local\FrostModSession`) carrying the server name, the track, your GUID and the
  live rider table. The app cannot see any of it for itself — it is a separate program with
  no view into the game — and it is what lets voice chat put you in a room with the people
  on your server, whether you joined from the app or from the game's own browser. Written
  every frame, read across the process boundary through a seqlock, so a reader that catches
  a half-written update tries again instead of hearing someone in the wrong place.
- **A crash is written to the log before the process goes down.** Every FrostMod log before
  this one simply stopped mid-line when the game died — the one question the log exists to
  answer was the one thing it never recorded. A last-chance exception filter now names the
  exception, the faulting `module+RVA`, and for an access violation or in-page error the
  address and what was being done to it. An in-page error also reports the filesystem's own
  NTSTATUS and points at cloud-backed mod folders, which is the shape that fault takes. It
  chains to whatever filter was already installed rather than swallowing it. Two things it
  can't promise, and doesn't claim: a filter installed after ours replaces it, and a stack
  overflow may leave too little stack to run it.
- **Overlay size, F8 → `6`.** Steps 75 → 200 % and the menu stays open while you press it, so
  you can see the size you are choosing. It applies on track as well as in the menus, and it
  persists in `frostmod_radar.cfg` alongside the radar settings.

### Fixed
- **The overlay no longer shrinks on a big screen.** In the game's menus FrostMod draws the
  overlay itself, and that path was laid out in raw pixels — so on a 4K display the F8 menu
  came out at half the apparent size it has at 1080p, which is exactly as readable as it
  sounds. It now sizes itself to the screen, so it takes the same share of a 4K one as of a
  1080p one. Nothing changes at 1080p or below. Rider outlines still follow the resolution
  only, so they keep fitting the riders they mark.
- **The server browser no longer writes a log line per server per repaint.** `SB_SuppressRow`
  runs inside the game's populate loop, which re-runs continuously while the browser is
  open, and it logged every row plus a hex window on every pass — measured at 8,569 lines in
  a single second, and 16.5 MB of a 17.4 MB log, every byte a synchronous write on the
  game's own thread. It now reports a tally per pass, and only when that tally changes, so a
  browser sitting still is silent. The per-row dump is still there behind
  `frostmod.exe --srv-debug`.

### Removed
- **Mumble positional audio.** MXB App does the voice itself now: peer to peer, in the app
  the rider already has, with nothing for anyone to install and nothing running on the
  server. Publishing our position to Mumble meant every rider first had to download Mumble,
  find a server to join and be on it at the same time — which is most of the reason nobody
  did. The position data it needed did not go away; it moved into the session block above,
  where the app can use it directly. Its F8 row is gone; `6` is the overlay size now.

## 2026-08-16 — v0.13.0

### Fixed
- **Every command MXB App has ever sent was ignored.** `HandleFrostModCommand` reads the
  verb out of `frostmod_cmd.json` with `JsonStringField`, and that function could not
  return `true`: the comment on its escape branch ended in a backslash, and a backslash at
  the end of a line splices the following line into the comment — the following line being
  the `if (ch == '"') return true;` that ends a value. So every read ran off the end of the
  document and reported failure, and every command was refused with *no 'verb' in the
  command file*. Nothing else is affected: reload from the console and `F8` are their own
  event and never went near this. The parser now lives in `src/cmdchannel.h`, with the same
  escape handling and a test that reads back a bike id holding both a quote and a
  backslash, so it can't go quiet again unnoticed.

### Added
- **A command can arrive as a file alone, with no event to announce it.** The DLL re-reads
  `frostmod_cmd.json` about five times a second and acts on it when it changes, as well as
  when `Local\FrostModCommand` fires. This is what lets MXB App drive FrostMod on **Linux
  and macOS**: there the game runs inside a Wine prefix — Proton's, or a CrossOver/Whisky
  bottle — and so does FrostMod, but the app is a native process *outside* that prefix. It
  can't open a Wine kernel object to pulse an event, and it can't resolve what `%TEMP%`
  means in there. What it can do is write into the folder it installed FrostMod into, one
  directory on disk seen from both sides, so that folder is now read alongside `%TEMP%`.
  Windows is untouched: the event still arrives, and the file it points at is still the one
  that gets read.
- **`reload_mods`**, the verb behind MXB App's Reload button. It does what `R` and `F8` do;
  it exists because a reload asked for from outside the prefix has no event to travel on.
- **`src/cmdchannel.h` and `tests/command_channel_test.cpp`** — when a command counts as
  new, kept clear of Win32 so CI can run it. The cases are the ways two mouths on one
  channel go wrong: yesterday's command running at start-up, one command dispatched twice
  because both paths saw it, the same command sent twice collapsing into one (MXB App
  stamps each with `at`), and two command files taking turns looking new to each other and
  reloading the game forever.

## 2026-08-12 — v0.12.1-beta.1

Diagnostic-only, and published as a **pre-release**: `releases/latest` skips it, so neither
`frostmod.exe --update` nor the MXB App will offer it. Download it by hand if you are the
one collecting the GP Bikes reload log.

### Added
- **`--unsafe-reload-from=<n>` skips to a given reload step**, for the one question a
  step-level crash log can't answer on its own. GP Bikes dies on step 1 (`tracks`,
  `0x139A0`) every time it is armed — three sessions out of three, with no step 2 ever
  reached. Skipping it separates the two explanations: if 2–13 then complete, that single
  loader is unsafe to re-run; if the new first step dies identically, replaying *any*
  loader from the present-thread call site is what kills the game. Implies
  `--unsafe-reload`, and rides in the same flag file — an empty one still reads as "start
  at step 1", so a flag file left by an older build behaves exactly as it did.

### Changed
- The reload's thread-id line no longer frames the two ids as the open question. A v0.12.0
  reporter log has the boot scan tid and the reload tid **identical** in all three sessions,
  so we already replay on the thread that owns the content lists — the race theory that
  motivated logging them is disproved. `tasks/gp-bikes-port.md` records what replaced it:
  the tracks loader zeroes its list globals without freeing them, and is stack-cookie
  guarded, so a failure there is `__fastfail` and SEH never sees it.

## 2026-08-12 — v0.12.0

### Fixed
- **Reload no longer takes GP Bikes down.** v0.11.0 replaced MX Bikes' table with one
  derived for GP Bikes, and that one crashes the game too: a reporter's log shows
  `[reload] surgical content reload` with no `[reload] done` after it and the process
  gone — one session died on its first reload, another on its fourth after three clean
  ones. The offsets are still unconfirmed, so GP Bikes now **refuses** the reload with an
  honest message instead of attempting it. MX Bikes is untouched.
- **The radar pointed riders in arbitrary directions.** `m_fYaw` is *degrees from north*
  and was being handed straight to `cosf`/`sinf`, which take radians — scaling every
  heading by 57.3. It isn't a small tilt: a rider held dead ahead swings from +86° to
  −139° across four degrees of real heading change, so blips spin as you steer. The three
  constants the code invited you to flip (`GroundUV`, `RAD_YAW_SIGN`, `RAD_YAW_OFFSET`)
  were all correct, and flipping them could never have fixed it. Shipped this way since
  v0.9.7.
- **A short element stride no longer reads off the end of the rider arrays.**
  `RaceTrackPosition` and `RaceClassification` walk the game's array with the stride it
  reports; a stride smaller than the struct we read meant each element's tail came out of
  the next one, and the last out of bounds. A *larger* stride is still accepted — that's
  just a later game build appending fields.

### Added
- **Proximity voice, via Mumble.** FrostMod publishes your position through Mumble's Link
  interface, so riders on the same server hear each other from where they actually are.
  Mumble does the voice; we only say where we are. On by default, toggled with **`6`** in
  the F8 menu, persisted in `frostmod_radar.cfg`. See [docs/MUMBLE.md](docs/MUMBLE.md) for
  the setup and the coordinate derivation.
- The facing vector is derived from the radar rather than guessed, and the two are checked
  against each other at every 30° of heading — if the radar points at someone, Mumble hears
  them in the same direction by construction. `LinkedMem`'s layout is a cross-process
  contract, so its size is pinned by a `static_assert`; getting it wrong would write
  Mumble's fields at the wrong offsets with no error anywhere.
- **Riders are grouped by the context `EventInit` carries** (`server|track`): its
  `SPluginsBikeEvent_t` holds the server name, the track ID *and* our own GUID.
  `RaceEvent` is a *race*-session callback — it fires on the dedicated server and never in
  a client session — so it is kept for the track half alone, and a rotation still updates
  it without losing the server name. Identity is our GUID, stable per install, where a
  race number is only unique within one session. Nothing is published until a context
  arrives: an empty context is still a context, and every rider carrying one would be
  grouped together regardless of the server they're on. `EventDeinit` clears it on
  leaving, so a stale one can't group you with whoever is in the next lobby.
- **Every plugin callback logs once on first arrival**, with the size the game passed
  (`[cb] EventInit fired (dataSize=…)`). The handler used to log only *after* its size
  check, so "never called" and "called with an unexpected payload" produced identical
  silence. One line now separates them.
- **Every reload step is logged before it runs** (`[reload] step 4/13 rva=0x14D90 - bikes`),
  on both titles. This is the only way a crash inside a replayed loader can be
  attributed: each step is SEH-guarded, so an ordinary access violation is swallowed and
  the reload finishes — what actually kills the process (heap corruption's fail-fast, a
  fault raised on another thread) leaves nothing behind. `Log()` writes through per line,
  so the last step in the log is the one that did it.
- **`--unsafe-reload`** arms an unconfirmed table anyway, for whoever is collecting that
  log. Off by default; the launcher and the DLL both say which mode the session is in.
- **Thread ids on the boot scan and reload lines.** GP's boot content load runs on the
  WinMain thread; we replay those loaders from whichever thread presents frames. If the
  two ids differ, the reload races the game's own use of the lists — which fits an
  intermittent kill. One report now settles it.
- `reload_verified` on `GameOffsets`, with tests. A table that exists but has never run on
  its title is the dangerous case, not the safe one — it looks like a working feature
  until it isn't. Both GP crashes shipped as tables that compiled fine.

### Changed
- Yaw carriers renamed `yawDeg` (`RadRider`, `RadBlip`, `RadBuildBlips`) so the unit
  travels with the value. The bug above was invisible at the call site precisely because
  a bare `yaw` says nothing about what it's measured in.
- The radar's conventions are documented as settled rather than pending a live run,
  confirmed against PiBoSo's SDK header and cross-checked against
  [MXBMRP3](https://github.com/thomas4f/mxbmrp3) (MIT), which draws a working radar for
  the same games from the same callback. The outline's GL view-projection capture is
  unaffected and still wants a Windows run.

### Notes
- What the reporter's log *does* confirm: GP's loaders really are self-contained (for
  tracks, tyres, rider and bikes the game-dir and mods-dir scans share one boot-init
  return with two call sites inside the loader), and `0x139A0`/`0x34AE0`/`0x34080`/
  `0x14D90`/`0x32090` are where the table says. The other 8 RVAs are still
  string-derived: `LogScanCallers` stops after 16 stack shots, so the log never reaches
  them.
- Unrelated, same log, still open: `wglUseFontBitmaps` failed on all 7 attempts on that
  machine (RTX 4060 Ti / 596.49), so every overlay panel draws its background quad with
  no text in it. `EnsureFont` latches after one try and never retries.

## 2026-08-08 — v0.11.0

### Fixed
- **Reloading mods no longer crashes GP Bikes.** v0.10.0 attached to `gpbikes.exe`
  correctly and then ran *MX Bikes'* reload table inside it. The reload was never the
  `RVA_CONTENT_INIT` constant that shipped with the GP port — that is only a gate — but a
  21-entry table of `mxbikes.exe` RVAs, half of them calls and half raw writes zeroing MX
  list globals. Fired into GP Bikes that called arbitrary functions and zeroed arbitrary
  memory; the per-step `__try/__except` meant it corrupted silently and died a moment
  later with nothing in the log. GP Bikes now has its own table, derived from the
  content-load section of its boot init (`0xfb95a`), covering tracks, tyres, rider, bikes,
  paints, helmets, riders, animations, stands and dashes.
- **FrostMod uses the mods folder of the game it is attached to.** The default was
  hardcoded to `Documents\PiBoSo\MX Bikes\mods` regardless of `--game`, so a GP Bikes
  session pointed the track manager, the inactive-tracks store and the model swap at MX
  Bikes' folders. `--process gpbikes.exe` now picks the right folder too.
- **The `registryReset` capture hook no longer installs on GP Bikes.** That RVA is MX
  Bikes' and has no twin derived, so v0.10.0 was splicing a detour into whatever code
  happened to live at `0x159340` in `gpbikes.exe`. Same for the `--bikecap` and
  `--dump-serverlist` diagnostics, which read MX's bike array and server-list blob.

### Changed
- **A title with no reload offsets now refuses the reload instead of attempting it.** The
  step table moved onto `GameOffsets`, so it is reached through the attached title and
  cannot fall back to another game's. If a table proves wrong, setting `reload_steps` to
  null degrades that title to an honest "reload not supported" rather than a crash.
- `frostmod.exe` prints which title it is driving, so a mismatch is visible at a glance.

### Added
- `tests/offsets_test.cpp` — asserts each title's offsets belong to that title, that MX's
  table is unchanged, and that a table with DIR steps supplies the operands they need. It
  is pure constants, so CI runs it (`ctest`) rather than only compiling it. Pointing GP at
  MX's table, the exact v0.10.0 defect, fails five of its checks.

### Notes
- GP Bikes' reload table was derived by static analysis against an unpacked `gpbikes.exe`
  and corroborated against a reporter's crash log, but has **not** been confirmed under a
  debugger. Verify on Windows before release: drop a `.pkz` into
  `Documents\PiBoSo\GP Bikes\mods\tracks`, press `R`, and confirm the track appears with
  no crash — then re-run the same flow on MX Bikes to prove the refactor didn't regress it.

## 2026-08-08 — v0.10.0

### Added
- **FrostMod attaches to GP Bikes and reloads its mods.** MXB App drives GP Bikes now, but
  FrostMod was gated off there because every RVA in `offsets.h` came from `mxbikes.exe`.
  The two constants live reload actually needs have been recovered from an unpacked
  `gpbikes.exe` — the boot content-load routine (`0xfb650`) and the VFS directory walker
  (`0x18f150`) — so dropping a `.pkz` into `Documents\PiBoSo\GP Bikes\mods` and pressing
  `R` works the same way it does for MX Bikes. Run `frostmod.exe --game gpb`, or
  `--process gpbikes.exe` as before; injected into the game, the DLL works out which title
  it is in from the host process itself.

  The scanner *signature* turned out to be shared: GP's prologue is byte-for-byte identical
  to MX Bikes', same `0x7f8` frame, so the existing signature-with-delta fallback that
  survives a game update keeps working for either title unchanged.

### Changed
- **Features whose offsets are MX-Bikes-only now stay off elsewhere** rather than firing at
  addresses that mean nothing in another build. The server-browser filter is the one that
  matters: it writes a jump at `RVA_SB_POPULATE_LOOP`, which on GP Bikes is unrelated code.
  A per-title `offsets_complete` flag gates it, and the log says why.
- `--update` refuses to run while *either* game is open, not just MX Bikes — the DLL is
  locked by whichever one loaded it.

### Notes
- The GP Bikes offsets were derived by static analysis and have **not** been confirmed under
  a debugger; see `tasks/gp-bikes-port.md` for how they were found and what remains. The
  server-browser filter, master protocol and direct connect are still MX Bikes only.

## 2026-08-08

### Fixed
- **The game no longer crashes to desktop after a model swap.** v0.9.9's instant model
  refresh replayed the game's bike-**apply** call (`fcn.1400E4550`) using the arguments of
  an earlier call, to make a swapped model appear without the class-switch away-and-back.
  It crashed the game — not at the swap, but at the **next bike the player selected by
  hand**, which is what made it read as "picking a bike crashes MX Bikes" rather than as
  anything to do with swapping a model. The replay is removed. It could not be made safe
  by checking harder:
  - the descriptor is a caller temporary we don't own, and the liveness test — is the
    captured bike name still somewhere in its `0x140` bytes — passes for a frame that has
    returned but not yet been overwritten, which is precisely the case it existed to catch;
  - `rcx`, the object the loader writes into, was never checked at all;
  - the surgical content reload rebuilds the very arrays the loader indexes, so after a
    swap the capture describes a world that no longer exists — and the replay waited for
    that reload to finish and then went ahead regardless;
  - the `__try`/`__except` around the call made it worse: an access violation inside a
    half-finished machine swap was swallowed and the game carried on with the wreckage,
    converting an immediate crash into a delayed, unattributable one.

  Both swap paths (F8 and the MXB App command channel) now say "model swapped — re-select
  the bike to see it" and leave the running game alone. The `refresh_bike_model` verb is
  still accepted and answered with that same notice, so an older MXB App gets the truth
  rather than silence. (`src/frostmod.cpp`.)

### Changed
- **The bike-apply hook is opt-in again (`frostmod_bikecap.flag`), as it was before
  v0.9.9.** Shipping it to everyone put our detour — and its speculative scan of the
  descriptor, which treats every aligned qword as a possible string pointer — in the path
  of every bike selection in every player's game, purely to feed a refresh that no longer
  exists. Its Stage-A diagnostics are unchanged and remain the only reason to arm it.
- **Release v0.9.11 — the fix ships as 0.9.11, not 0.9.10.** `v0.9.10-rc1` was already
  published from a branch that isn't on `main` and **still contains the replay**, and its
  `version.h` reads `0.9.10` too. Two builds claiming one version would be bad enough, but
  MXB App gates on the recorded tag numerically: a `v0.9.10` floor reads `v0.9.10-rc1` as
  new enough and hands it the very verb that crashes it. Taking the next number costs
  nothing and excludes that pre-release cleanly, without touching a tag that isn't ours.
  `FROSTMOD_VERSION` 0.9.9 → **0.9.11** (`src/version.h` + `CMakeLists.txt`), which is also
  what the release workflow's tag check compares against. MXB App v0.7.1 and up withhold
  `refresh_bike_model` from anything below this release, so updating is what stops the app
  asking for the unsafe path.
- **Doing this properly is still open.** It needs what Stage A was always meant to settle —
  which descriptor field holds the picked bike's name, and which call site is the garage
  one — after which the apply can be driven from a descriptor **we build**, rather than one
  we borrowed. Until that run happens on Windows, the re-select is the feature.

## 2026-08-07

### Added
- **Instant model refresh — a swapped model shows without the class-switch away-and-back.**
  Until now, changing a bike's model left the garage preview showing the old mesh: the
  surgical content reload rebuilds the content *catalogs*, not the live preview instance,
  so the only way to see the new model was to switch bike class away and back. The bike
  **apply** loader `fcn.1400E4550` — RE'd for the in-garage switcher — is the fix: it
  re-derives the selected bike by name and reloads the machine from
  `bikes\<Bike>\<Bike>.cfg`, so **replaying it with the same descriptor** makes the game
  re-read the bike from disk, which is exactly what away-and-back achieves without
  disturbing the player's selection. The hook is now **always installed** (its verbose
  Stage-A diagnostics stay opt-in behind `frostmod_bikecap.flag`) and records the last
  apply's args. A refresh replays that call, but only when the swapped bike is the one
  currently selected — decided by matching the bike name against the strings captured
  in/behind the descriptor, so we never needed to pin which field holds it. Wired into
  both swap paths: the F8 model swap (`MsApply`) and MXB App over the command channel.
  (`src/frostmod.cpp`.)
- **Command channel from MXB App (`Local\FrostModCommand` + `%TEMP%\frostmod_cmd.json`).**
  MXB App has been writing this command file and pulsing this event since its garage-switch
  groundwork, but **nothing in FrostMod ever listened** — only `Local\FrostModReload` was
  created, so `garage_swap_bike` was a dead contract that could never do anything. FrostMod
  now creates the event, polls it in `Tick` alongside the reload event, and dispatches the
  payload on the render thread. Verbs: `refresh_bike_model` (above) and `swap_bike`, which
  logs "not implemented yet" rather than silently ignoring the app. The reader is a small
  field scanner, not a general JSON parser — the file is machine-written with a known flat
  shape. Contract mirrored in mxb-app's `src-tauri/src/frostmod.rs`. (`src/frostmod.cpp`.)

### Changed
- **Replay safety.** The apply descriptor may be a caller stack temporary, so a stored
  pointer can go stale. Rather than probe stack bounds, the replay re-scans the descriptor
  and requires the captured bike name to still be there — a reused frame won't match, and
  we skip with a "re-select the bike" status instead of handing the loader garbage. The
  call itself is SEH-guarded in a POD-only helper (same rule as `SafeCopyStr`), and a
  re-entry flag stops our own replay overwriting the capture it is replaying.
- **Release v0.9.9.** Bumped `FROSTMOD_VERSION` 0.9.8 → 0.9.9 (`src/version.h` +
  `CMakeLists.txt`). Ships the instant model refresh + the MXB App command channel
  (above). Note this is the first release where the bike-apply hook (`0xE4550`) is
  installed for **everyone** rather than only under `frostmod_bikecap.flag` — the
  capture is what the refresh replays. It only records call arguments; nothing is
  replayed unless a model swap asks for it, and then only for the selected bike.
- **CHANGELOG date ordering repaired.** The `2026-07-23` section had landed above
  `2026-07-29` in a merge, breaking the newest-on-top rule, and ran straight into the
  next heading without a blank line.

## 2026-07-29
### Added
- **Diagnostic: plugin `Draw()`-dispatch watch across an F8 reload.** Investigating a report that a co-existing HUD plugin (MXBMRP3) goes dark after an F8 → `1 Reload mods`, while FrostMod's own HUD stays up. Hypothesis: the surgical content reload (which deliberately skips the game's reinit/UI-transition tail — see `RequestReload`/`kReloadSteps`) leaves the game no longer calling the plugin `Draw()` callback; FrostMod masks this on itself via its GL swap-hook fallback, but plugins without one simply stop rendering. `RequestReload()` now arms a 20 s window during which `Tick()` logs, once a second, presented `frames/s` vs plugin `Draw()/s` (`[drawdiag]` lines). If `frames/s` stays high while `Draw()/s` falls to 0 right after the reload, the mechanism is confirmed. Pure logging, self-silencing, no game state touched. To be removed once the cause is fixed. (`src/frostmod.cpp`.)
### Changed
- **Release v0.9.8.** Bumped `FROSTMOD_VERSION` 0.9.7 → 0.9.8 (`src/version.h` + `CMakeLists.txt`). Diagnostic patch: ships the `[drawdiag]` reload/`Draw()`-dispatch watch (above) to confirm why a co-existing HUD plugin stops rendering after an F8 reload; no behaviour change for normal users (log-only, self-silencing).

## 2026-07-23

### Added
- **In-garage bike switcher — Stage A (opt-in `--bikecap` diagnostic).** Groundwork for
  switching the whole bike (model + physics) live from the garage, offline, restricted to
  the race's class — paired with the MXB App UI. This first slice is **observation-only, no
  swap**: dropping an empty `frostmod_bikecap.flag` next to `frostmod.log` arms a read-only
  hook on the bike **apply** loader `fcn.1400E4550`. Per apply it logs the caller (to tell
  the garage caller from the on-track one), a hex/ASCII dump of the session descriptor
  (`rdx`) + a pointer-probe (to locate the picked bike's name field), and once the whole
  in-game bike array (index → `+0x00`/`+0x4C0` names) plus `entry[0]` bytes (to find the
  `[data] cat`/class offset). Settles the four static-RE unknowns so Stage B can build the
  switcher (capture-and-replay `0xE4550` with the target bike substituted). New bike offsets
  in `offsets.h` (list ptr `0xF4EDE8`, count `0xF48218`, stride `0x4334`, apply `0xE4550`,
  + AOB). Off by default; no game state touched. (`src/frostmod.cpp`, `src/offsets.h`.)

## 2026-07-16
### Added
- **In-game Radar + lap-aware Rider Outlines (F8 menu > `4` radar, `5` outlines).** A racing-spotter HUD built entirely on the sanctioned PiBoSo plugin callbacks — no memory reads of other players. New exports `RunTelemetry` (our world pos), `RaceTrackPosition` (every rider's live world pos + yaw each update), and `RaceAddEntry`/`RaceClassification` (race number → name + laps-done) feed a shared, mutex-guarded rider snapshot; "me" is identified as the track-position entry closest to our telemetry pos, so no extra RE is needed. **Radar** is a heading-up disc in the top-right (your bike points to the top; a blip at the top is directly ahead), rendered in both the PiBoSo `Draw()` path (on track) and the GL overlay (menus/injected), with `PageUp`/`PageDown` to change range (default 80 m). **Outlines** draw a box around each on-screen rider. **Three lap-status colors** on both blips and outlines, from `m_iNumLaps`: white = same lap, red = a rider lapping you (a lap ahead — let them by), blue = a rider you are lapping (a backmarker). Toggles + range persist in `frostmod_radar.cfg` next to `frostmod.log`. (`src/frostmod.cpp`.)
- **Camera view-projection capture for the outlines (fixed-function OpenGL).** The plugin API gives rider world positions but not the camera matrix needed to place a screen box, so we snoop `glMatrixMode`/`glLoadMatrixf` to grab the perspective PROJECTION + camera MODELVIEW and compose `VP = P·MV`, validated each frame by projecting our own position. When a valid VP isn't available (e.g. a core-profile/shader context), the outline **degrades gracefully to a screen-edge directional arrow** derived purely from the radar bearing — so the feature always renders something. A one-shot diagnostic logs `GL_VERSION`/`GLSL`/`RENDERER` + a short matrix-flow dump (`[esp/diag]` lines) so the first tester run confirms the capture path; the matrix hooks only do work while outlines are on. (`src/frostmod.cpp`.)
### Changed
- **Release v0.9.7.** Bumped `FROSTMOD_VERSION` 0.9.6 → 0.9.7 (`src/version.h` + `CMakeLists.txt`). Ships the radar + rider-outline HUD (above). A few world-axis / yaw-sign conventions are isolated as calibration constants (`GroundUV`, `RAD_YAW_SIGN`, `RAD_YAW_OFFSET`) pending confirmation on the Windows tester.
- **F8 menu adds `4 Radar (riders around you)` and `5 Rider outlines`.** (`src/frostmod.cpp`.)

## 2026-07-15
### Added
- **Diagnostic: `.edf` model-open capture (opt-in `frostmod_edfcap.flag`).** RE aid for auto-refreshing the garage bike preview after a model swap (today it needs a manual class switch away-and-back). Dropping an empty `frostmod_edfcap.flag` next to `frostmod.log` arms a read-only hook on `kernel32!CreateFileW/A` that logs every bike-model `.edf` file the game opens **with its call stack** (as `mxbikes.exe` RVAs). This settles the two open questions from the log alone: whether re-selecting the same bike re-reads `model.edf` from disk (mesh-cache test), and which scene function is the loader (top game RVA) + its selection caller. Off by default; no game state touched. (`src/frostmod.cpp`.)
- **In-game Bike Model Swap (F8 menu > 3).** New overlay tool to swap a bike's model live. In MX Bikes a bike at `<mods>\bikes\<Bike>\` is loose files, and a "model" is the whole top-level file set — `model.edf` (the mesh) **plus** its `.hrc`/`.cfg` lineup/alignment, which are tuned to that mesh and travel with it; only `paints\` (universal liveries) stays put. The tool is a two-level list: pick a bike, then pick a model variant. Variants are folders in a per-bike library `<Bike>\FrostMod Models\<Name>\` (each holding a full file set); applying one **auto-backs-up** the current set into the library (reversible — pick `Original` to restore) and moves the chosen variant's files into the bike folder, then reloads content so the new model shows without a restart. Move failures (e.g. files held open while riding that bike) roll back and abort with the bike intact. Keys: Up/Down move, Enter choose/swap, Esc back/close. (`src/frostmod.cpp`.)
### Changed
- **Release v0.9.6.** Bumped `FROSTMOD_VERSION` 0.9.5 → 0.9.6 (`src/version.h` + `CMakeLists.txt`). Diagnostic patch: ships the opt-in `.edf` model-open capture (above) to RE the garage bike-preview reload path; no behaviour change for normal users (the capture is off unless `frostmod_edfcap.flag` is present).
- **F8 menu decluttered to focus on model swap.** The menu now shows only `1 Reload mods`, `2 Toggle this overlay`, and `3 Bike model swap`. Track manager, Switch track, Track list, and Direct connect are hidden from the menu (their code is kept intact and simply not wired to a row, so any can be re-exposed by re-adding its `kMenu[]` entry + `MenuAction` case). Updated the F8-menu table in `docs/USAGE.md` to match. (`src/frostmod.cpp`, `docs/USAGE.md`.)
- **Release v0.9.5.** Bumped `FROSTMOD_VERSION` 0.9.4 → 0.9.5 (`src/version.h` + `CMakeLists.txt`). First full release since v0.9.3 — bundles the Bike Model Swap tool, the FrostServer dedicated-server companion (0.9.4, previously only an rc1 pre-release), and the direct-connect RE corrections.
### Fixed
- **Model-swap list no longer drops bikes whose active model is an Original without a loose `model.edf`.** `MsScanBikes` keyed list-membership solely on a loose `<Bike>\model.edf`, so a bike whose stock mesh lives in a `.pkz` (no root `model.edf`) vanished from F8 → `3` once swapped back to Original — stranding its remaining variants. Membership now also matches any bike that has a `FrostMod Models\` library folder, so swap-managed bikes stay listed and reachable regardless of whether the active set has a loose `model.edf`. (`src/frostmod.cpp`.)

## 2026-07-14
### Changed
- **Release v0.9.4.** Bumped `FROSTMOD_VERSION` 0.9.3 → 0.9.4 (`src/version.h` + `CMakeLists.txt`) — the first release to ship the FrostServer dedicated-server companion.
### Added
- **README — FrostServer section.** Documented the server-side companion in `README.md`: a Features bullet, a new "FrostServer (dedicated servers)" section, and the new build outputs, all linking to `docs/FROSTSERVER.md`.
- **Release packaging for FrostServer.** The Windows release CI now also builds and attaches `frostserver.exe` / `frostserver.dll` / `frostserver.dlo` plus a ready-to-run `FrostServer.zip` (plugin + standalone tester + README) to each published release, alongside the existing FrostMod assets. (`.github/workflows/release-build.yml`.)

## 2026-07-13
### Added
- **FrostServer — dedicated-server map/link API (`frostserver.dlo` / `frostserver.exe`).** New server-side companion that runs as a PiBoSo plugin on an MX Bikes dedicated server. It learns the running track via the sanctioned `RaceEvent()` callback — track name is `m_szTrackName` (+0x68), confirmed against the decompiled server plugin loader (resolver `0x14012A4F0`; race forwarder in the server module `0x14028FF81`; the `-dedicated` flag `0x565E64` gates only startup, never the plugin/race dispatch) — and serves a tiny read-only HTTP API so FrostMod clients can ask *what map are you running and where do I download it?* — returning a mxb-mods.com link the admin configures per track in `frostserver.yaml` (written with docs on first run, next to the plugin). Endpoints: `GET /frostserver/info` (current map + link, `currentMap:null` when idle), `GET /frostserver/maps` (the full configured table), `GET /health`. Default port `54210`. Ships with diagnostic callbacks (`EventInit`/`RaceSession`/`RaceAddEntry` + a raw ASCII-field dump on every event) so the first run on a real dedicated server empirically confirms which callbacks fire and where the track name sits. New CMake targets `frostserver` (→ `frostserver.dll`/`.dlo`, links `ws2_32`) and `frostserver_app` (→ `frostserver.exe`, standalone tester via `FROSTSERVER_EXE`, seed a track with `--track "Name"`), both signed when `-DFROSTMOD_SIGN=ON`. This is the server half of the "download a server's map without leaving the game" flow; the client button + MXB App `mxbapp://` handoff are separate follow-ups that consume this contract. Full HTTP contract + admin setup in `docs/FROSTSERVER.md`. `src/frostserver.cpp`, `CMakeLists.txt`, `docs/FROSTSERVER.md`.

## 2026-07-12
### Added
- **README release badges.** Added a shields.io badge row under the title — latest release, release date, total downloads, MIT license, and Windows x64 platform — all pulling live from the GitHub repo so they stay current automatically.
- **`Release.zip` — ready-to-run bundle on every release.** The release CI now also builds and attaches a `Release.zip` containing `frostmod.exe` + `frostmod.dll` + `frostmod.dlo`, the default `frostmod_serverfilter.yaml`, and a short `README.txt`, laid out under a `FrostMod/` folder so users can download, unzip, and run. The server-filter config is extracted at build time from `kDefaultConfig` in `src/serverfilter.cpp` (single source of truth — always matches what the DLL writes on first run), with a guard that fails the build if the extraction looks truncated. Opt-in `frostmod_*.flag` files are deliberately not bundled. The individual `frostmod.exe`/`.dll`/`.dlo` assets are still attached too (the `--update` self-installer fetches those by name).
- **GitHub Actions Windows build CI.** New `.github/workflows/release-build.yml` builds the x64 `frostmod.exe` / `frostmod.dll` / `frostmod.dlo` on `windows-latest` (`cmake -A x64` → `--build --config Release`) and, when a GitHub Release is *published*, attaches those three files as release assets under the exact names `frostmod.exe --update` looks for — plus a tag-vs-`FROSTMOD_VERSION` guard so a mismatched release can't ship. New `.github/workflows/ci.yml` runs the same build (no publish) on every push/PR touching `src/**` or the build files. Uses only official actions + the built-in `gh` token (no third-party actions, no secrets).
- **README "Troubleshooting" section — leads with "Is this a virus?".** New section answering the antivirus/SmartScreen question head-on, plus the common real failures (injection "access denied" → run as same user/elevated, no log/overlay, "already loaded" another injector, `--update` DLL-lock, game-not-detected). Points to `docs/antivirus-false-positives.md` for the full VirusTotal breakdown and per-vendor dispute templates.

### Fixed
- **Launcher build break — a comment ate the `gameDirArg` declaration.** Two `//` comments in `src/launcher.cpp` ended with a `\`, which is a line-continuation: the trailing backslash on the `doInstallPlugin` comment spliced the next line into the comment, commenting out `std::string gameDirArg;` and making it "undeclared" at its two use sites (MSVC `C2065`, warning `C4010`). Reworded both comments so they don't end in a backslash. Caught by the new Windows build CI on its first run.

### Changed
- **Release v0.9.3.** Bumped `FROSTMOD_VERSION` 0.9.2 → 0.9.3 (`src/version.h` + `CMakeLists.txt`). First release built and published by the new Windows CI — `frostmod.exe` / `frostmod.dll` / `frostmod.dlo` are attached as release assets automatically. Highlights since 0.9.2: plugin (`.dlo`) mode + `--install-plugin`, in-game track switcher and consolidated F8 overlay menu, one-command `--update` self-installer with a startup update check, YAML server-filter config, and AV false-positive hardening.
- **Code-signing docs corrected — the release binaries are NOT signed.** The SignPath Foundation declined for now (insufficient traction / download history); the plan is to re-apply as FrostMod grows, with a paid yearly certificate as the fallback. Removed every "code-signed via SignPath" claim from `README.md`, reworded the Troubleshooting reassurance to rest on open-source + build-it-yourself + plugin mode (no injection), and fixed the dispute-email subject in `docs/antivirus-false-positives.md` that still called the software "signed". (Build still supports optional `-DFROSTMOD_SIGN=ON`, off by default — a capability, not the current release state.)

## 2026-07-09
### Added
- **`--capture-master` — master-protocol capture (groundwork for a mimic master server).** The official `master.mx-bikes.com` (UDP 54200) has been going down, so we're building a community **mimic master** + a FrostMod redirect. First step is an opt-in, read-only protocol sniffer: `frostmod.exe --capture-master` writes `frostmod_capture.flag`, and the DLL then hooks the `ws2_32` exports (`sendto` / `recvfrom` / `getaddrinfo`) and logs **only** master-bound datagrams — UDP 54200 or the resolved `mx-bikes.com` IP — as `[cap]` / `[cap.hex]` / `[cap.str]` lines (dump-capped; packets pass through untouched). Captures the outbound `GETLIST` (client) and `REGISTER` (`mxbikes.exe --dedicated`) halves **even while the real master is down**, so we can reconstruct the login/GETLIST/REGISTER wire format. Hooking the exports (not the game's internal net wrappers) means no signature to author and no offset-drift risk. Fully inert unless the flag is present.
- **Direct connect — in-game IP box + endpoint validation (F8 menu → 6), preview step.** New **F8 → 6** one-line `IP[:port]` text field (digits / `.` / `:`, Backspace, **Enter**, **Esc**/**F8**), rendered in both overlay paths (GL + the sanctioned `Draw()`), reusing the track-search input; validates four `0–255` octets + optional port `1–65535` (default `54200`). **The connect itself is not wired yet — deferred pending RE.** A capstone xref sweep of `0xE53DE0` (the assumed target) found it is connect **output/state** (reset-to-sentinel; the 16-byte field is a *packed* binary address, not a settable ASCII host) with **no** standalone connect function; the real initiator is the engine **command bus `[0x566C48]` with cmd `0x389`** (JOIN handler ~`0x0F0Exx`), which needs live menu state and an `r8` input of unknown layout — and the browser JOIN actually writes a *different* struct (`rbx+0xE54030`), so the earlier "msg 0x385 fills 0xE53DE0" was a mix-up. So **Enter parses + logs the target and no-ops** (touches no memory). `offsets.h` now carries the corrected model (`RVA_CMD_BUS_PTR`, `CMD_JOIN`, `RVA_JOIN_DISPATCH`, the reset writers, the net-layer callers); next step is a runtime `r8` capture (x64dbg bp `0x0F0FE7`) to settle the input format, then wire the bus dispatch. No version bump.
- **`.dlo` plugin packaging.** MX Bikes auto-loads plugins named `*.dlo` from its `plugins\` folder, so the build now emits **`frostmod.dlo`** alongside `frostmod.dll` — a byte-for-byte copy (any Authenticode signature carries over), done as the last post-build step so it copies the final signed binary. Fixes plugin mode, which previously shipped only a `.dll` the game would never load. Injected mode still uses the `.dll`; plugin mode the `.dlo`.
- **`frostmod.exe --install-plugin [dir]`.** Copies `frostmod.dlo` (and a `frostmod_data\` folder if present) into `<MX Bikes>\plugins\`, creating it if needed. The install folder comes from the arg, or is auto-derived from a running `mxbikes.exe` (reusing the existing process discovery); clear guidance if it can't be found, and a friendly "close MX Bikes first" on a sharing violation. `--update` now also refreshes the local `frostmod.dlo` so a re-run of `--install-plugin` pushes the new build.
- **Sanctioned `Draw()` overlay (hybrid render path).** Implemented the optional PiBoSo `Draw()` callback: on track/spectate/replay the engine renders our overlay from quad/string arrays we build in normalized `0..1` space (ABGR) — no OpenGL, resolution-independent, and it can't silently hide on a non-GL/core context the way the GL overlay can. The existing immediate-mode GL overlay is kept as the fallback for menus / the server browser (where `Draw()` isn't called) and for injected mode; a per-frame `g_drawCalls` counter lets the `wglSwapBuffers` hook suppress the GL draw whenever `Draw()` is live, so there's never a double image. Input (`Tick`) and all overlay state are shared unchanged between the two renderers.

### Changed
- **Plugin install docs point at `.dlo` + `<install>\plugins\`.** `docs/PLUGIN.md` and `README.md` now state the plugin file is `frostmod.dlo` next to `mxbikes.exe` (no `.ini`), document `--install-plugin`, and describe the hybrid overlay; resolves the old "confirm the plugins location" TODO.

## 2026-07-06
### Fixed
- **Track switcher no longer crashes the game — live load is now opt-in.** Calling `fcn.1400BB510` from `F8 → 3 → Enter` mid-ride crashed (exactly the RE caveat: it manipulates testing-menu widgets that don't exist while riding, and the fault can land a frame later, past our SEH). The switcher is now **safe by default**: Enter just reads + logs the fields (`folder`/`disp`/`resolver@0x33C`) so `+0x33C` can be verified without risk. The real load (write session config + call `fcn.1400BB510`) only runs when armed via **`frostmod.exe --switch-live`** (a `frostmod_trackswitch.flag` the DLL checks) — and even then should only be triggered **from the testing menu**, not mid-ride. A true mid-ride switch still needs the teardown/return-to-menu sequence (further RE).

### Added
- **Track manager — search-by-text + select/unselect-all (F8 menu → 2).** The activate/deactivate checklist now filters and bulk-toggles, so wrangling a big library is fast. **F** (or **/**) opens a live search box: typed letters/digits/space build a case-insensitive substring filter over the track path, **Backspace** deletes, **Enter** commits (filter kept, back to list nav), **Esc** clears it. **A** selects/unselects **all rows in the current (filtered) view** — so `F` → `sand` → `A` → `Enter` bulk-activates every sand track in one pass. The cursor/scroll now walk a filtered view (`g_trkView`, indices into the full list) instead of the raw list; **Apply** still acts on the whole list, so staged toggles on rows hidden by the filter are preserved. The panel shows `N/total shown`, the search line with a caret while typing, and a `(no matches)` row when the filter empties the list. All render-thread, no locking; one shared key-edge table (`prevKey[]`) means Enter/Esc can't double-fire across a mode switch.
- **Track switcher — change the localhost map in-game (F8 menu → 3).** RE'd the testing-session load path: `fcn.1400BB510` (`RVA_TRK_LOAD_ENTER`) loads the track named in the session config `[0xE4B540]` (folder `+0x00`, resolver-name `+0x20`, matched against `entry+0x33C`) and re-enters — it takes no args and re-derives the index by name, so setting the index (`0x4CA3D4`) alone won't stick. **F8 → 3** opens a scrolled, keyboard-driven list of the game's loaded tracks (display names); **↑/↓** move, **Enter** writes the name config from the chosen entry (folder + `+0x33C`) and queues the heavy load+enter **on the game thread** (like the reload), **Esc**/**F8** cancels. Everything is SEH-guarded, and the fields it uses are logged on Enter (`[switch] request … folder=… resolver@0x33C=…`) so `+0x33C` can be verified before trusting a live switch. Whether re-entering mid-ride cleanly tears down the current map (vs needing to be at the testing menu first) is still to be confirmed at runtime.
- **Track manager — step 2: interactive activate/deactivate (F8 menu → 2).** The track library entry is now a real in-game manager instead of a log dump. **F8 → 2** opens a scrolled, keyboard-driven checklist of every track (active from `mods\tracks\**`, inactive from the `FrostMod Inactive Tracks` store, deduped by relative path). **↑/↓** move the cursor (with held-key auto-repeat so a long list scrolls), **Space** stages a track active/inactive (a pending row is marked amber with `*`), **Enter** applies, **Esc**/**F8** cancels. Apply physically moves each changed `.pkz` between the two trees (`MoveFileExA`, recreating the category subfolder via a kernel32-only `EnsureDirTree` — no new link deps) and then triggers **one** live reload (reusing `RequestReload`), so deactivated tracks leave `mods\tracks` and drop out of the game with no restart. A move that fails because the `.pkz` is held open (e.g. you're on that track) is logged and skipped, its stage reverted so the list stays truthful — never fatal. All render-thread, so no locking. The on-disk location is the source of truth (active ⇔ under `mods\tracks`); no manifest needed. Supersedes step 1's read-only F10 listing.
- **PE version resource on both binaries (AV false-positive hardening).** `frostmod.exe` and `frostmod.dll` were shipping with *no* version-info block, which — combined with being unsigned — reads as a "suspicious PE" to antivirus ML heuristics (SentinelOne "Static AI", Symantec "ML.Attribute.HighConfidence", Elastic, etc.). A new `src/version_info.rc.in` embeds CompanyName/ProductName/FileDescription/FileVersion/OriginalFilename (version driven from `FROSTMOD_VERSION` in CMake, kept in sync with `src/version.h`); CMake generates a per-target `.rc` and MSVC compiles it in automatically. Gives each binary a legitimate identity and removes one packaging red flag.
- **Optional Authenticode signing in the build (`-DFROSTMOD_SIGN=ON`).** Off by default and needs a certificate you supply. Post-build `signtool` step with SHA-256 + RFC3161 timestamp; supports an OV/PFX cert (`FROSTMOD_SIGN_PFX`/`FROSTMOD_SIGN_PASSWORD`) or an EV/token cert by thumbprint (`FROSTMOD_SIGN_SHA1`). Signing + reputation is the single biggest lever against ML false positives.
- **`docs/antivirus-false-positives.md`.** Explains why FrostMod trips AV ML heuristics (injection/hooking/self-updater mechanics that overlap with malware), what the build now does about it, and verified per-vendor false-positive dispute channels (Symantec SymSubmit, Elastic form, SentinelOne/Bkav/MaxSecure/Trapmine) with a ready-to-paste justification.

### Changed
- **In-game F-keys consolidated into one F8 menu.** Instead of a separate F-key per feature, **F8** now opens a small FrostMod menu (top-left); press an item's number to run it (`1` reload, `2` track library, `3` track list, `4` toggle the hint), `Esc`/`F8` to close. New features become a row in the menu rather than yet another F-key. The corner hint now reads `F8: menu`; the menu always draws while open (so you can't hide the overlay and lose the way back).
- **Track entry layout confirmed** (from the F9 dump): `+0x00` folder/id, `+0x20` display name, `+0x60` short name, `+0xB0` preview image — filled into `offsets.h` (`TRK_FOLDER/TRK_NAME/TRK_SHORT/TRK_IMAGE`), so the track-library/switcher UIs can show real names.

### Added
- **One-command update (`frostmod.exe --update`).** Downloads the latest release's `frostmod.exe` + `frostmod.dll` (WinHTTP, following GitHub's CDN redirects), verifies both, then swaps them in — the DLL directly (must close MX Bikes first, since it's locked while the game runs) and the running exe via the rename-self trick — and relaunches. Downloads to `.new` files and only swaps if both succeed, so a failed/interrupted download leaves your install untouched; stale `.old`/`.new` files are cleaned on next launch.
- **Update check.** On startup `frostmod.exe` asks the GitHub Releases API for the latest tag (off-thread via WinHTTP, so it never delays startup) and, if it's newer than the running build, prints an `UPDATE AVAILABLE: vX.Y.Z` banner with the download link (`frostmod.exe --update` installs it). Read-only and silent when offline/rate-limited; disable with `--no-update-check`.

### Changed
- **Server-filter config is now YAML (`frostmod_serverfilter.yaml`) and much leaner.** Replaced the comment-heavy `frostmod_serverfilter.txt` with a short YAML file: scalar toggles (`hideUnjoinable`/`hideEmpty`/`hideLocked`/`maxPerIP`) plus two block lists (`names:` and `regex:`). A server is hidden if its name contains any `names` entry or matches any `regex` (case-insensitive). The default ships with just the cheat-ghost rules and a 3-line header instead of ~35 comment lines. The parser is a minimal YAML subset (`key: value` + `- item`), values may be quoted (single-quote regex to keep backslashes literal), and `#` still starts a comment. Config version bumped to v4 → the file auto-writes on first run; an old `.txt` is simply ignored (delete it if you like).

### Added
- **Track library manager — step 1: F10 lists your on-disk tracks (active + inactive).** Toward activating/deactivating track `.pkz` files so `mods\tracks` stays lean. The launcher now writes `frostmod_mods.txt` so the DLL knows the mods folder; the DLL derives the inactive-tracks store as `…\MX Bikes\FrostMod Inactive Tracks` (a sibling of `mods`, **outside** the scanned tree so deactivated tracks are never loaded). **F10** recursively scans `mods\tracks\**\*.pkz` (active) and the inactive store (inactive) and logs them (`[trklib] [x]/[ ] …`). Read-only for now — no files are moved yet; next steps are the F10 keyboard overlay with checkboxes, then the move + JSON manifest (toggle many, then reload once).

## 2026-07-05
### Added
- **Track switcher — step 1: F9 dumps the track list.** Toward an in-game track switcher (pick a track and load it without logging out of localhost), **F9** now reads the game's track array (`RVA_TRACK_LIST`, stride 1220, count at `RVA_TRACK_COUNT`) and logs each entry's folder/name strings + a hex window for the first few, so the entry layout can be pinned. (Server-list dump is no longer on F9; use the console `D` key or `--dump-serverlist`.) Next steps: an F9 overlay list with keyboard nav, then wiring the selected track to the game's load→bike/rider→join flow (RE pending).
- **Auto-start at login (`frostmod.exe --install-startup`).** Registers a per-user `HKCU\…\Run` entry (no admin) so FrostMod launches automatically at every login — minimized — and, since it already watches for the game, injects into MX Bikes the moment it starts, however you launch it. One-time setup; `--install-startup` also keeps running immediately. Undo with `--uninstall-startup`. A new `--startup` flag (used by the login entry) starts the console minimized so it isn't in your face. The normal banner now shows whether auto-start is on.
- **F8 reload now shows a live progress bar + spinner instead of a "freeze".** The surgical reload used to run as one blocking call on the render thread, so no frame could present while it worked — it looked like the game hung. It's now a **per-frame step machine**: each content list (tracks, bikes, tyres, …) is rebuilt on its own frame, and the in-game overlay draws an advancing progress bar with a `|/-\` spinner and `Reloading mods… NN%` between steps. Because every step fully rebuilds one list, the game stays consistent between frames. A re-press while a reload is running is ignored. (Same content-load as before — just paced across frames so you can see it working.)

### Fixed
- **Server filter now genuinely hides rows (was a no-op masked by in-game "hide empty").** The `0x0ABAB6` hook detected spam correctly but never removed it — the apparent success was the game's hide-empty checkbox. A fresh IDA pass found why: the row is **committed at the first `setCellText` (`0x0ABA03`)** — a cell-write auto-extends the widget, there is no separate addRow — which is **before** `0x0ABAB6`, so we were always too late. The game's own name-search filter suppresses a row by skipping **before** `0x0ABA03` (`strstr` miss → `jmp 0x0ACE68` at `0x0AB9D3`). FrostMod now mirrors that: the hook moved to the **loop top `0x0AB960`**; `SB_SuppressRow(index=r14, rsp)` reads the record (`gameRsp + index*0x1D8`, name `+0x86`) and, on a match, the stub `jmp`s `0x0ACE68` so **no cell is ever written and the row never appears**. Counts and `SELECTED/INFO` indices stay consistent because they track displayed rows. Also confirmed `r12d==0` and that `0x0ABAB6` is gated by `[0x4C8F44]&1` (hide-empty), so it only ran with the checkbox on — which is exactly how the illusion happened.
- **Server filter self-locates across game builds.** It used a raw fixed RVA while the mod reload already AOB-scans + applies a build-drift **delta**; on a friend's slightly different `mxbikes.exe` the filter silently no-op'd. The filter now applies the same `g_sigDelta` and falls back to an `SIG_SB_LAN_CMD` AOB scan (hook + skip computed relative to the found function), with a clear "could not locate" log if the build truly differs.

### Added
- **`docs/USAGE.md` — full run/CLI reference.** Documents every command-line flag (common + diagnostic, with defaults), the console keys (`R`/`D`/`Q`) and in-game hotkeys (`F8`/`F7`/`F9`), the files FrostMod creates (log, `frostmod_serverfilter.txt`, the internal `.flag` markers), the server-filter rule types, and plugin mode. Linked from the README so it stays lean.
- **First published release — v0.9.0.** `frostmod.exe` + `frostmod.dll` attached on the GitHub Releases page. Headline features: live mod reload (**F8** in-game / **R** in the console), the server-browser cheat/ad-ghost filter (`frostmod.exe --filter-servers`), and the in-game overlay (**F7**).
- **Server-filter now actually HIDES (skip-at-emit).** A full IDA pass settled the mechanism: there is **no pre-build source array** to filter (the list lives in the UI list-widget behind the message manager `0x566C48`; `[rsp+0x86]` is already a per-build stack copy), and the game's own hide-empty/name filters work by **skipping at emit** — evaluating the record at `0x0ABAB6` and `jmp 0x0ACE68` (the row-skip label). RE also confirmed `r12d == 0` (stock branch = "maxplayers==0 → skip") and that skip-at-emit keeps the displayed list self-consistent (SELECTED/INFO indices are displayed-row positions), so **never** compact post-build. So `SB_FilterEntry` now returns a hide decision and the `0x0ABAB6` stub branches: **hide → `jmp 0x0ACE68`**, keep → original `cmp` via trampoline. This is exactly the stock skip path — the earlier crashes were the volatile-`r10` stack bug (already fixed by parking `rsp` in `rbp`), not the skip itself. Every row is still logged (`[srv] … HIDE` / `keep`). Default scope: cheat/ad ghosts only (config v3). `--filter-servers` now hides rather than previews.

### Changed
- **Server filter is now ON by default — no flag needed.** Running `frostmod.exe` with no arguments now enables *both* the mod reload and the server-browser filter. `--filter-servers` still works (explicit on), and a new **`--no-filter-servers`** opts out (reload only). Only the launcher changed — it drops the `frostmod_filter.flag` the DLL already looks for; the DLL is untouched.
- **README rewritten as a normal, scannable README** and the repo **About** set. Reframed FrostMod as a client-side toolkit (a **Features** list: live mod reload + server-browser spam filter + in-game overlay) instead of "just a reloader"; dropped the snowflake, the internals/RE "How it works" section, and the MX-Bikes-scan explanation; trimmed build/usage to the essentials. GitHub About + topics (`mx-bikes`, `modding`, `windows`, `dll-injection`, `bakkesmod`) added.
- **Server-filter default scope narrowed to cheat/ghost ads (config v3).** Per the chosen scope, the active default rules now target only the kaizo-style cheat-shop floods (`BUY CHE4TS … WWW.KAlZ0.PR0`) via a leet-tolerant regex `(che[a4]ts|k[a4][il1]z[o0]|\.pr0\b)` plus readable `che4ts`/`kaizo`/`kalz0` names. The hosting-ad, discord/URL, and streamer rules are shipped as **commented, opt-in** examples (uncomment to also hide). Config bumped to `v3` so the auto-upgrade regenerates it (old file → `.bak`).

### Removed
- **The floating Win32 "Reload Mods" window.** Redundant now that reload is driven by the in-game overlay + **F8** and the `frostmod.exe` console **R** — one fewer desktop window and background thread. Dropped `UiThread` / `WndProc` / `ID_BTN_RELOAD` / `g_hwnd` and the `CreateThread` that started it.

### Changed
- **Server-filter preview verified end-to-end; config now auto-upgrades.** The second read-only dump confirmed everything reads correctly — clean names from `+0x86`, sensible `cur=/cap=` (`cur=19 cap=32`), and pings that now resolve to real values. The kaizo cheat-ad ghost (`BUY CHE4TS … WWW.KAlZ0.PR0`, flooded ~8× at `cur=42 cap=42`, host `127.0.0.1`, `ping=---`) is correctly flagged `WOULD-HIDE` by the URL regex. The seeded name rules hadn't been applying because the on-disk `frostmod_serverfilter.txt` was never regenerated, so the config file now carries a **version sentinel** (`# frostmod-filter v2`) and the loader auto-rewrites an out-of-date file (backing the old one up to `.bak`) — no manual delete. Seeds tightened against the live list: ad/hosting hosts (`cbrhosting.com` — not the legit `CBRSERVERS.COM` — `mymxb`, `cheap dedi`, `server hosting`, `dedicated servers`, `rent your server`) and cheat-shop variants (`che4ts`, `kaizo`, `kalz0`, since they leetspeak `kaizo.pro` → `KAlZ0.PR0`), all kept precise so legit servers stay. Still read-only (no hiding yet).
- **Server-browser layout confirmed from the read-only dump; filter switched to NAME-based.** The dump settled the `SB_Entry` layout: the display name is at **`+0x86`** (not `+0x00`; the 2 bytes at `+0x84` are a binary field that only sometimes looks like ASCII), and players/max were **swapped** — `+0xC8` is the capacity and `+0xCC` the current player count (the field the game's own hide-empty `cmp` at `0x0ABAB6` tests). Biggest finding: **`ping` (`+0xDC`) is `"---"` for *every* server at list-build time** (pings resolve later), so `hideUnjoinable` would hide the whole browser — it's now **OFF by default** and the dump no longer filters on it. The reliable signal is the **name**, so the default config now seeds precise ad-host rules (`cbrhosting.com` — distinct from the legit `CBRSERVERS.COM` — `kaizo.pro`, `cheap dedi`, `server hosting`) plus the URL/discord regex. `SB_DumpEntry` now reads the name straight from `+0x86` and prints `cur=/cap=`; the hex window widened to `0xE0` so `+0xC8/+0xCC/+0xDC` are visible for re-confirmation. Delete `frostmod_serverfilter.txt` to regenerate the new defaults.
- **Surgical all-mods reload — no loading screen, no menu bounce.** Replaced the full-init reload (which re-ran `fcn.1400ef210` wholesale — re-initting Steam/sound/input and bouncing to the menu) with a replica of **only** its content-load section: every content list is cleared + rescanned from disk (tracks, bikes, tyres, helmets, boots, gloves, suits, …) using the game's own per-type loaders — the self-contained ones (`sub_140002460` tracks, `sub_140003100` bikes, called with ignored args) plus the `clear 3 globals → scan(&String) → scan(&mods)` blocks — then **stops before the init/transition**. So `R` / `F8` now makes new mods of any type appear live with no visible reload. Every loader call and list-clear is SEH-guarded; `String` (`0x3333EB`) is the empty game-dir base, mods path is `0xE54B44`. (Compile-verified on Windows pending; RVAs transcribed verbatim from the `fcn.1400ef210` decompile.)
- **Server filter stepped back to a crash-proof READ-ONLY dump.** Actually skipping rows kept crashing the browser: the populate loop's post-loop `DISPLAY_COUNT += RAW count` (`0x0ACE68`) re-counts a skipped row, so the list is later indexed past its end — and decrementing `DISPLAY_COUNT` per hidden row still wasn't enough. So `--filter-servers` now installs a **pure observer** at `0x0ABAB6` (`SB_DumpEntry`): it logs every server row to the console/log — index, best-guess name + offset, players, ping, type, and the filter verdict (`keep` / `WOULD-HIDE: reason`) — and **writes nothing back to the game**. It also dumps a hex+ASCII window (`[srv.hex]`) of the first few entries so we can finally pin the real `SB_Entry` name offset (name reads empty at `+0x00`). The name is now auto-located by scanning the entry header for the longest printable-ASCII run. The asm stub was also hardened: it parks `rsp` in the non-volatile `rbp` (not the volatile `r10`) across the call, so `SB_DumpEntry`'s work can't corrupt the return path. Once the dump confirms the fields, hiding can be rebuilt on solid ground.

### Added
- **Version string** (`FROSTMOD_VERSION`, new `src/version.h` shared by the DLL + launcher): shown in the in-game overlay (`FrostMod v0.9.0 — F8: reload mods`), the DLL load banner, and the `frostmod.exe` console header — one source of truth so they never drift.
- **In-game overlay** (OpenGL, drawn in the `wglSwapBuffers` hook): a small top-left corner hint — `FrostMod — F8: reload mods` — visible in fullscreen, plus a transient status line after a reload (`reloading… → reloaded — new content listed`). **F7** toggles it, **F8** reloads. Text is built once via `wglUseFontBitmaps` (Segoe UI); the whole draw is `glPushAttrib`/matrix push-pop guarded so it never disturbs the game's rendering, and it no-ops silently if the game ever runs a core GL profile. DLL now links `opengl32`.
- **Server-browser spam filter works** (`frostmod.exe --filter-servers`) — it correctly detects and hides unjoinable ghost/ad servers. First cut jumped to the row-skip target itself and crashed the game; the stub now instead **writes `r12d` into the row's `maxplayers`** so the game's own `cmp`/`jz` skips the row through its normal path (a single memory write, no control-flow surgery — no desync/crash).
- **Server-browser spam filter is LIVE** (`frostmod.exe --filter-servers`). Disasm of the populate loop showed the entries are a stack buffer addressed as `[rsp+rdi+field]` (entry = `rsp+rdi`; players `+0xC8`, maxplayers `+0xCC`, unjoinable/"---" `+0xDC` — corrected from `0xD8`), and the game's own `jz` at `0x0ABAB6` already targets the row-skip label `0x0ACE68`. So FrostMod splices `0x0ABAB6` with a MinHook hook to a hand-built stub that computes `rsp+rdi`, calls `SB_ShouldHideEntry`, and jumps to the skip target when the rules say hide (else runs the original `cmp`). The stub verifies the expected bytes before splicing and is opt-in (mid-function hook). With `hideUnjoinable` on by default, ghost/ad servers (ping "---") drop out of the browser.
- Manual server-list dump: press **`D`** in the console (`Local\FrostModDumpNow`) to dump the blob on demand — so you can open the online browser, wait for servers, then dump regardless of state timing. `hkMpMsg` also logs the handler firing + master state progression (`[srvlist] master handler call#N state=…`) so we can see whether the list reaches `state==3` (complete). The `0x2A10E0` hook verified + installed cleanly in testing; it just needs the online browser opened to produce a list.
- Master-protocol RE opens a **clean, code-cave-free** filter path: the server-list reply arrives as a **text blob** at `0x9E3AE0`, written by the opcode handler `0x2A10E0` (clean prologue + AOB). Added those offsets, the reader-primitive RVAs, and `SIG_MP_MSG_HANDLER`. New `frostmod.exe --dump-serverlist`: the DLL signature-validates + hooks `0x2A10E0` and dumps the blob once per completed list (`[srvlist] …`, NUL shown as `|`, newline as `/`) so we can see the record format — then a follow-up edits the blob to drop spam/unjoinable servers before the browser parses it, no mid-function splice needed. Off by default.
- Server browser RE landed (from IDA): `offsets.h` now has the real networking + server-browser layout — `SB_Entry` fields (name `+0x00`, players `+0xC8`, maxplayers `+0xCC`, **ping `+0xD8` where `0xFFFFFFFF` = unjoinable**), the populate loop + row-skip target, UI-state globals, and AOB signatures for the LAN/world browser commands. `serverfilter` gains a **`hideUnjoinable`** rule (default ON) — the ping-"---" ghost/ad signal the user described — plus `SB_ShouldHideEntry()` in the DLL that reads an entry (SEH-guarded) and returns show/hide. Remaining to go live: a mid-function code-cave splice in the populate loop (needs the splice address + entry register + stolen bytes from RE).

### Changed
- **Reload rewired to the real content-loader + dead code removed.** The `[stack]` backtrace pinned it: `fcn.1400ef210` (reached via the API dispatcher `0x120CC0`) is the boot routine that clears and rebuilds **every** content list from disk. `R` / `F8` now just calls it on the game thread (SEH-guarded, resolved as `g_base + RVA_CONTENT_INIT` and validated in `.text`), so a newly-dropped `.pkz` registers live. Ripped out all the dead reload machinery it replaces — strategies `A/A+/A++/B`, the captured-scan replay (`g_scans`/`g_scanArgs`), the `ext=pkz` direct-call, `registryReset` replay, and the F7/`S` strategy-cycling (in-game **and** the console `S` key + `Local\FrostModCycle` event in the launcher) — which were built on the wrong mount/replay model and only ever printed the misleading `ext=pkz` / `ABORT: no .pkz scan captured` lines. **Caveat:** `fcn.1400ef210` is also the app's boot init (re-inits input/sound/Steam and returns to the menu — a soft restart); the lighter per-category reload that rebuilds just the track list without the re-init is the planned follow-up (`offsets.h RVA_TRACK_LOADER`, TODO — pin the writer of `dword_140f43298`).
- **Reload RE corrected (IDA + `--probe-mount` runtime capture).** Two earlier assumptions were wrong: (1) `0x15a9e0` is **not** a mounter — IDA shows it's a **6-arg `.pkz` entry iterator** `(char* pkzPath, char* subdirPrefix, char* ext, int* cursor, char* outName, void* outDesc)`; our 4-arg probe hook crashed startup by leaving its stack args (`a5`/`a6`, one of them a pointer it writes through) as garbage. (2) There is **no pkz "mount" to trigger at all** — the walker `0x158be0` reads the filesystem **live every call** (`findfirst *.pkz` + `fopen`), so a newly-dropped `.pkz` is already visible to any fresh scan; the game just runs the scan once at startup and the menus cache it. So "mount the new file / replay the scan / call `0x15a9e0`" (strategies A/B) can't work by design. The real reload = re-run the game's **content-loader** (which calls the walker via the engine API dispatcher `0x120CC0` through a global fn-ptr, so it's invisible to static xrefs). Added a `[stack]` diagnostic (`RtlCaptureStackBackTrace` in the crash-safe walker hook) that logs the boot-scan caller chain to pin that loader — the next step is to call it on reload.
- **Superseded:** early-load capture proved `0x158be0` is a generic **VFS directory walker** (called with ext `/`, `cfg`, `pnt` — never `pkz`), not the pkz loader. By the time it runs the `.pkz` are already mounted and appear as virtual dirs it walks, so replaying/calling it can never mount a newly-added track — which is why every reload strategy failed. Corrected the `offsets.h` labels; the real target is the pkz-**mount** function (`0x15a9e0`). *(See the corrected finding above — `0x15a9e0` is an iterator, and no mounting is involved.)*
- Added `frostmod.exe --probe-mount`: leaves a flag so the DLL hooks the pkz-mount function (`0x15a9e0`) and logs its args (`[mount] …`) at startup — to reveal which arg is the `.pkz` path so a real mount-based reload can be built. Observe-only pass-through, opt-in (default off).

- Reload is now a cycle-able **multi-strategy** experiment: `A` replay the captured `.pkz` scan, `A+` reset + replay `.pkz` (default), `A++` reset + replay every captured content scan, `B` construct + directly call the scanner (`<savePath>\mods`, ext `pkz`, fresh buffers — works without a capture). Cycle with `F7` in-game or `S` in the console (`Local\FrostModCycle` event); the active strategy is logged. `hkScan` now stores all distinct `(dir,ext)` scans (`g_scans`), and every replay/direct call goes through SEH-guarded wrappers so a wrong-argument attempt logs `FAULTED - caught` instead of crashing the game.

### Added
- FrostMod is now also a **PiBoSo plugin**, not just an injected DLL. Added the plugin exports (`GetModID`→"mxbikes", `GetModDataVersion`→8, `GetInterfaceVersion`→9, `Startup`, `Shutdown`) so MX Bikes loads `frostmod.dll` itself from its `plugins` folder at startup — before the one-time mods scan, with no injector or SteamStub timing race. `EnsureInit` guards so the injected and plugin paths init exactly once. The injector (`frostmod.exe`) stays as a fallback. New `docs/PLUGIN.md` documents the exports, lifecycle, hooks, and files. Researched the PiBoSo SDK: the plugin API is telemetry/timing/spectate/input + a `Draw()` overlay and exposes no server list or content-directory access, so filtering/refresh remain function hooks — the plugin is purely a cleaner loader.
- Server-list spam filter (client-side), scaffolded. New `serverfilter` module (`src/serverfilter.{h,cpp}`) with a config-driven rule engine (name substring, name regex, max-per-IP per refresh, hide-locked, hide-empty), reading `frostmod_serverfilter.txt` (created with docs + sensible ad-name defaults on first run). Loaded on DLL init and hot-reloaded on `R`. The game-side hook that feeds server entries in is stubbed behind `RVA_SRV_*` / `SIG_SRV_LIST_ADD` placeholders in `offsets.h` (RE pending) — until filled in, config loads but nothing is hidden.

## 2026-07-03
### Added
- Early injection + deferred hooking to catch the one-time startup mods scan. Testing showed `0x158be0` is a generic scanner and only `ui`/`str` ran through it after our (2s-late) injection — the mods/`pkz` scan happens earlier. Now `frostmod.exe` injects ~400ms after the game appears (override with `--wait <ms>`), and the DLL waits for SteamStub to decrypt the code (`WaitForScanner`) before installing content hooks, so it's hooked before the game runs its scan. Render hooks now wait for `opengl32` to load (it isn't mapped that early). Injection retries a few times since a just-launched process can briefly reject it. Compile-time stamp added to exe/dll output to catch stale builds.

### Fixed
- Reload replayed the wrong scan. The scanner is generic — `(status, dir, file-extension, buf)` — and the game calls it for many folders (e.g. `ui`/`str`). The old capture kept whatever was scanned last, so reload replayed a non-mods scan and did nothing. Now the hook logs every distinct `(dir, ext)` the game scans and keeps the `ext="pkz"` call (the mods/content mount) as the replay target.

### Added
- Signature (AOB) validation of `offsets.h`: before hooking, the DLL checks the bytes at the scanner RVA against the known signature; on mismatch it scans the executable sections for the pattern and uses the found address, logging `[sig] VERIFIED` / `RELOCATED (delta …)` / `not found`. If the scanner can't be located, content hooks are skipped (reload disabled) instead of hooking wrong code. The registry-reset (which has no signature) gets the same delta as a best-effort, flagged as unverified.
- `frostmod.exe` is now resident and auto-(re)injects: it waits for the game, injects on launch, and re-injects a fresh DLL on every relaunch — so a rebuilt DLL always takes effect without manual restart juggling. It no longer quits when the game closes (only `Q`/Ctrl+C exits).
- Reload reports clearly when content hooks weren't installed (offsets didn't match) instead of the misleading "not captured" message.

### Fixed
- Header/UI no longer prints a garbled snowflake (`Γ¥ä`): the UTF-8 emoji didn't render in the console/GDI code page, so the console banner and the in-game window label are now plain ASCII.
- The console now actually shows the DLL's log: both `frostmod.exe` and `frostmod.dll` write/read `frostmod.log` **next to the binaries** (same folder) instead of `%TEMP%`, which a Steam-launched game can resolve differently — that mismatch was hiding all DLL output. Falls back to `%TEMP%` only if the folder is read-only.
- Log tailing re-opens the file each poll, avoiding stale-handle/caching issues while the DLL appends.

### Added
- Render-hook heartbeat: the DLL logs `[tick] render hook alive` on the first presented frame, and `[event] reload signal received` when triggered from the console — so you can tell whether the per-frame hook (which runs the reload) is firing at all.
- Launcher prints a hint if no DLL output appears within a few seconds of injecting, and a reminder to open a content menu once so the scan can be captured.

### Added (earlier today)
- README: detailed Windows build guide for Visual Studio 2026 (prerequisites, x64 Native Tools prompt, build steps, common issues)
- `frostmod.exe` now runs as a persistent launcher/monitor: after injecting it stays open, lists the `.pkz` mods found in the MX Bikes mods folder, watches that folder and prints new/removed mods live, and streams `frostmod.log` to the console. Press `R` to reload, `Q`/Ctrl+C to quit.
- Launcher waits for `mxbikes.exe` if it isn't running yet, so you can start FrostMod first and have it inject at game launch (also captures the startup scan automatically).
- `--mods "<path>"` option to override the mods folder the launcher watches.
- Cross-process reload trigger: the DLL creates a `Local\FrostModReload` event that `frostmod.exe`'s `R` key signals, consumed on the render thread.

### Changed
- The executable now builds as `frostmod.exe` instead of `injector.exe` (CMake target `frostmod_app` with `OUTPUT_NAME frostmod`); `src/injector.cpp` renamed to `src/launcher.cpp`.
- README updated for the new `frostmod.exe` workflow (run `frostmod.exe`, not `injector.exe`).
