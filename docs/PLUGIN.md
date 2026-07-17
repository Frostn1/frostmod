# FrostMod — Architecture & Plugin Documentation

FrostMod is a single `frostmod.dll` that (a) live-reloads the MX Bikes mods folder
and (b) filters spam "ghost" servers from the browser. It can be loaded **two
ways**, and runs the same code either way.

## How it loads

| Mode | How | Entry point | Notes |
|------|-----|-------------|-------|
| **PiBoSo plugin** (recommended) | Drop `frostmod.dlo` in MX Bikes' `plugins` folder (or run `frostmod.exe --install-plugin`) | game calls `Startup()` | Loaded by the game at startup — before the one-time mods scan — with no injector, no `CreateRemoteThread`, no SteamStub timing race. |
| **Injected** | Run `frostmod.exe` | `DllMain` | Fallback / dev loop. Streams the log to a console, watches the mods folder, re-injects on relaunch. |

Both paths call `EnsureInit()`, which starts `Init()` **exactly once** (guarded by
an atomic flag) — so if the DLL is both in the plugins folder *and* injected, it
still initializes only once.

## PiBoSo plugin interface (what we implement)

MX Bikes loads every DLL in its `plugins` folder and validates it by calling the
three identity functions; if they don't match it unloads the DLL. Values are from
`mxb_example.c` (https://www.mx-bikes.com/downloads/mxb_example.c) — **if a game
update changes them, the game will reject the plugin and these must be updated.**

| Export | Signature | We return / do |
|--------|-----------|----------------|
| `GetModID` | `char* GetModID()` | `"mxbikes"` — which game this plugin is for. |
| `GetModDataVersion` | `int GetModDataVersion()` | `8` — data-struct version for this MX Bikes build. |
| `GetInterfaceVersion` | `int GetInterfaceVersion()` | `9` — plugin API version. |
| `Startup` | `int Startup(char* savePath)` | Records `savePath` (game data folder), (re)ensures our hooks are up, returns `3` (telemetry rate 10 Hz; unused but must be valid to stay loaded). |
| `Shutdown` | `void Shutdown()` | Logs; hooks tear down with the process. |

All exports are `extern "C" __declspec(dllexport)` (undecorated names on x64).

We also implement the optional **`Draw()`** callback (`void Draw(int state, int* nQuads,
void** quads, int* nStrings, void** strings)`) — the sanctioned overlay path. On
track/spectate/replay the game calls it and we hand back arrays of quads + strings
(normalized `0..1` coords, ABGR color) for the engine to render; see *Feature 3* below.
We also consume the **rider-data callbacks** for the radar + outline HUD (see below):
`RunTelemetry()` (our own world position, so we can identify "me"), `RaceTrackPosition()`
(every rider's live world position + yaw each update), and `RaceAddEntry()` /
`RaceClassification()` (race number → name + laps-done, for the lap-status coloring). We
reset the rider table on `RaceSession()` / `RaceDeinit()`. All are read-only. The rest
(`SpectateVehicles/Cameras`) stay omitted; none of these expose the server list or the
mods directory, which is why filtering and refreshing are done with function hooks
(below), not through the API.

### Feature — Radar + lap-aware rider outlines (F8 → `4` / `5`)
A racing-spotter HUD built only on the callbacks above (no memory reads of other players).

- **Radar** (`4`): a heading-up disc, top-right. World deltas to each rider are rotated by
  our heading so a blip at the top is directly ahead; `PageUp`/`PageDown` change range.
- **Outlines** (`5`): a screen box around each on-screen rider. The plugin API does not
  expose the camera view-projection matrix, so we capture it from the fixed-function GL
  pipeline (hook `glMatrixMode`/`glLoadMatrixf`, compose `VP = P·MV`, validate per frame by
  projecting our own position). If no valid VP is available, the outline degrades to a
  screen-edge directional arrow from the radar bearing — always something on screen.
- **Lap colors** (both): white = same lap, red = a rider lapping you (a lap ahead),
  blue = a rider you are lapping (backmarker), from `m_iNumLaps`.
- Toggles + range persist in `frostmod_radar.cfg` (next to `frostmod.log`).

**Calibration / RE status (verify on the Windows tester).** A few world-axis and yaw-sign
conventions are isolated as constants in `src/frostmod.cpp` (`GroundUV`, `RAD_YAW_SIGN`,
`RAD_YAW_OFFSET`) and set after the first live run. The one-shot `[esp/diag]` log lines
(`GL_VERSION`/`GLSL`/`RENDERER` + a short matrix-flow dump) confirm whether the
fixed-function VP capture is viable or a shader/uniform capture is needed for the outline.

### Where the plugin goes
The plugin file is **`frostmod.dlo`** (a byte-for-byte copy of `frostmod.dll` — the game
auto-loads plugins named `*.dlo`). Drop it into MX Bikes'
**`<install>\plugins\`** folder, next to `mxbikes.exe` (e.g.
`…\steamapps\common\MX Bikes\plugins\`) — no `.ini` needed. Or run
**`frostmod.exe --install-plugin`** to copy it there for you (auto-detects a running
game, or pass the folder: `--install-plugin "…\MX Bikes"`).

## Init sequence (`Init`, on its own thread)

1. Resolve the log path next to the DLL (`InitLogPath`).
2. `MH_Initialize()` (MinHook).
3. Create the `Local\FrostModReload` event (cross-process reload trigger).
4. Spawn the floating UI window thread (`UiThread`).
5. `WaitForScanner()` — wait until SteamStub has decrypted the game code, then
   verify/relocate the scanner by signature and install the **content hooks**
   (`scanFolder`, `registryReset`). Doing this early is the whole point of plugin
   mode: we're hooked before the game's startup mods scan.
6. Install the **render hooks** (`gdi32!SwapBuffers` now; `opengl32!wglSwapBuffers`
   once that module is loaded) — these drive the per-frame `Tick()`.
7. Load the server-filter config.
8. Log `[init] ready`.

## Feature 1 — mods folder refresh

- `scanFolder` (RVA `0x158be0`) is a **generic** directory scanner:
  `(status, directory, file-extension, out-buf)`. The game calls it for many
  folders; the **mods mount** is the call with extension `"pkz"`. `hkScan`
  captures that call's args (and logs every distinct `dir|ext` seen).
- `registryReset` (RVA `0x159340`) rebuilds the content registry — needed because
  the scanner skips folders it already loaded.
- On **reload**, `DoReloadOnGameThread()` runs the selected **strategy** on the
  render thread (via the task queue drained in `Tick`). There are two families,
  four strategies — **cycle with `F7` in-game or `S` in the console** and watch
  the log to see which one actually makes a new track appear:

  | Strategy | What it does |
  |----------|--------------|
  | **A** `ReplayPkzScan` | Replay the captured `pkz` scan only. |
  | **A+** `ReplayResetThenPkz` (default) | Replay the captured `registryReset`, then the `pkz` scan. |
  | **A++** `ReplayAllContent` | Replay the reset, then **every** captured content scan. |
  | **B** `DirectCallScanner` | *Construct* the call ourselves (`<savePath>\mods`, ext `pkz`, fresh status/out buffers) and call the scanner — works even if nothing was captured. Experimental. |

  All calls go through SEH-guarded wrappers (`SafeCallScan`/`SafeCallReset`), so a
  wrong-argument attempt logs `FAULTED - caught` instead of crashing the game.
- Trigger a reload: `R` in `frostmod.exe`, `F8` in-game, the floating-window
  button, or by signalling `Local\FrostModReload`. Cycle strategy: `S` / `F7` /
  `Local\FrostModCycle`.
- **Note on captured args:** the scanner's `dir`/`ext` (rdx/r8) point into the
  module and stay valid, but the game's `status`/`out` buffers (rcx/r9) were on its
  stack. Strategies A* reuse the captured pointers (empirically fine); strategy B
  passes fresh zeroed buffers instead.
- **Status:** the replay strategies need the startup `pkz` scan captured — i.e.
  loaded before it (plugin mode, or inject-before-launch). If MX Bikes mounts mods
  via a different function than `0x158be0`, that function must be reverse-engineered
  and added to `offsets.h`; strategy B is the fallback to probe direct calls.

## Feature 2 — server-list spam filter

- The master server (`master.mx-bikes.com`, UDP 54200) sends the server list;
  the plugin API does **not** expose it, so we filter inside the client.
- `serverfilter` (`src/serverfilter.{h,cpp}`) is a config-driven rule engine:
  **hide-unjoinable (ping "---", the ghost/ad signal — on by default)**, name
  substring, name regex, `maxPerIP` per refresh, `hideLocked`, `hideEmpty`. Rules
  live in `frostmod_serverfilter.yaml` next to the DLL (written with docs + defaults
  on first run); press `R` to hot-reload.
- The RE is in `offsets.h`: the browser builds `SB_Entry` working copies
  (stride `0x1D8`; name `+0x00`, players `+0xC8`, maxplayers `+0xCC`, **ping `+0xD8`
  where `0xFFFFFFFF` = unjoinable**) and a populate loop (`RVA_SB_POPULATE_LOOP`)
  emits one row each, with a row-skip target (`RVA_SB_ROW_SKIP_TGT`).
- `SB_ShouldHideEntry(void* entry)` in `frostmod.cpp` reads an entry (SEH-guarded)
  and returns show/hide via `serverfilter::ShouldHide` — the callback the loop
  splice will use.
- **Remaining:** the emit is inline (a loop, not a per-row call), so it needs a
  mid-function **code-cave splice** (not a MinHook prologue hook): jmp to a stub
  that calls `SB_ShouldHideEntry(entryReg)` and, if true, jmps to
  `RVA_SB_ROW_SKIP_TGT`, else runs the stolen bytes and returns. To author it
  safely we need, at the splice site (near `RVA_SB_HIDE_EMPTY_BR`): the exact
  address, the register holding the `SB_Entry` pointer, and the overwritten bytes.

## Feature 3 — in-game overlay (hybrid render path)

The overlay (status pill, reload progress bar, F8 menu, track manager/switcher) has
**two renderers over one shared state**, chosen automatically:

- **Sanctioned `Draw()`** — used on track/spectate/replay in plugin mode.
  `BuildOverlayDrawLists()` fills static quad/string arrays in normalized `0..1` space
  (ABGR color) and `Draw()` hands them to the engine. No GL, resolution-independent, and
  it can't silently hide on a non-GL/core context.
- **GL fallback (`DrawOverlay`)** — immediate-mode GL drawn from the `wglSwapBuffers` hook.
  Covers the cases `Draw()` doesn't fire: **menus / the server browser** (the engine only
  calls `Draw()` in-world) and **injected mode** (no plugin exports are called).

`Draw()` bumps `g_drawCalls`; the swap hook draws the GL overlay **only** when that counter
didn't advance since the previous frame — so on track the sanctioned path owns the frame
(no double image) and everywhere else the GL path takes over. Input (`Tick()`, polled from
the swap hook) is unchanged and feeds both. Font/size for `Draw()` strings assume the engine
default (index 1); quads (panels, highlight, progress bar) need no font.

## Offsets & signature validation

`offsets.h` holds RVAs (base `0x140000000`) **and** AOB signatures. At runtime,
before hooking, FrostMod checks the bytes at each RVA against its signature; on a
mismatch it scans `.text` for the pattern (game updates move code); if it can't be
found, that hook is skipped rather than pointed at the wrong code. Logged as
`[sig] VERIFIED / RELOCATED / not found`.

## Files & IPC

| Path | Purpose |
|------|---------|
| `<dll folder>\frostmod.log` | activity log (exe and dll share it) |
| `<dll folder>\frostmod_serverfilter.yaml` | filter rules (editable) |
| `Local\FrostModReload` | named auto-reset event; cross-process reload trigger (`R`) |
| `Local\FrostModCycle` | named auto-reset event; cross-process cycle-strategy trigger (`S`) |

## Key internal functions (reference)

- `Log`, `InitLogPath` — logging to `<dll folder>\frostmod.log`.
- `EnsureInit`, `Init` — one-time startup (see sequence above).
- `WaitForScanner`, `ResolveScanner`, `MatchAt` / `GetExecRange` / `PatternScan` —
  signature validation + AOB scan.
- `hkScan`, `hkReset` — capture the game's content-load calls (all distinct
  `(dir,ext)` scans go into `g_scans`; the `pkz` one into `g_scanArgs`).
- `DoReloadOnGameThread`, `DoDirectCall`, `ReplayReset`, `SafeCallScan` /
  `SafeCallReset`, `CycleStrategy` / `StrategyName` — the reload strategies, queued
  to the render thread via `EnqueueGameThreadTask` / `DrainGameThreadTasks`.
- `Tick`, `hkSwapBuffers` / `hkWglSwapBuffers` — per-frame hook (F8, reload-event,
  task drain, heartbeat).
- `UiThread` / `WndProc` — the floating window.
- `serverfilter::Init` / `Reload` / `ShouldHide` — the filter engine.
- `DllMain`, `GetModID` / `GetModDataVersion` / `GetInterfaceVersion` / `Startup` /
  `Shutdown` — load entry points (injected vs plugin).
