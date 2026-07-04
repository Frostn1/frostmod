# FrostMod ❄ — live mods-folder reloader for MX Bikes

MX Bikes reads your mods/content folders **once at startup** and mounts every
`.pkz` into an in-memory virtual filesystem. Anything you add while the game is
running is ignored until you restart. FrostMod adds a tiny floating window with
a **Reload Mods** button (and an **F8** hotkey) that re-triggers the game's own
content scan, so new tracks/skins register without a restart — same idea as
BakkesMod for Rocket League.

> Status: **v0.1, testing scaffold.** The injection, overlay, hooking, and
> game-thread plumbing are complete and correct. The reload itself uses a
> capture-and-replay strategy (below) that you should validate at runtime — the
> log tells you exactly what happened.

---

## How it works

Recovered by static RE of `mxbikes.exe` (see `src/offsets.h`):

| what | function | RVA |
|------|----------|-----|
| folder scanner (globs `*.pkz`, mounts each) | `fcn.140158be0` | `0x158be0` |
| content-registry reset/rebuild | `fcn.140159340` | `0x159340` |

The scanner **de-duplicates already-registered folders and early-returns**, which
is exactly why the running game ignores new files. So FrostMod doesn't just
"call the scanner again". Instead it:

1. **Hooks** both functions and **records the arguments the game itself passes**
   the first time it scans (startup, or when you open a content menu).
2. On **Reload**, it **replays** those recorded calls — optionally resetting the
   registry first — **on the render thread** (inside the `SwapBuffers` hook), so
   it never mutates the VFS while the game is reading it.

This avoids guessing argument formats: we reuse the game's own, verbatim.

### The one catch

Capture needs the game to call the scanner **at least once while FrostMod is
loaded**:

- **Inject at launch** (proxy DLL) → startup scan is captured automatically.
- **Inject after launch** → open the **track/bike selection menu once** so the
  game scans, *then* Reload works. `frostmod.log` prints `[capture] ...` when it
  has the args.

---

## Build (Windows only, x64)

> **This must be built on Windows.** FrostMod is a Windows DLL injected into the
> Windows `mxbikes.exe`; it cannot be built or run on macOS/Linux. The `-A x64`
> flag below is a Visual Studio generator option and will error on other
> platforms (`Generator "Unix Makefiles" does not support platform specification`).

Requires Visual Studio 2019/2022 (Desktop development with C++, which includes
CMake). MinHook is fetched automatically.

```bat
cd frostmod
cmake -B build -A x64
cmake --build build --config Release
```

No Visual Studio generator (e.g. Ninja/MinGW only)? Drop `-A` and pick a generator:
`cmake -B build -G Ninja && cmake --build build`.

Outputs to `build\bin\`:
- `frostmod.dll` — the mod
- `injector.exe` — the loader

## Run

1. Launch MX Bikes. For the floating window to be visible, run the game
   **windowed or borderless** (exclusive fullscreen hides overlays — use F8 there).
2. Inject:
   ```bat
   cd build\bin
   injector.exe
   ```
   (Injects `frostmod.dll` into `mxbikes.exe`. Run elevated if the game is elevated.)
3. Open the track/bike selection menu once (lets FrostMod capture the scan args).
4. Drop a new `.pkz` into your mods folder.
5. Click **Reload Mods** (or press **F8**). Check the new content appears.

Log: `%TEMP%\frostmod.log` (and a debugger's OutputDebugString).

---

## Tuning the reload (if v0.1 doesn't pick up files)

The exact reload sequence is the one thing to confirm on real hardware. Two
strategies are built in (`ReloadStrategy` in `frostmod.cpp`):

- **`ResetThenScan`** (default) — replay registry-reset, then the scan.
- **`ScanOnly`** — replay just the scan.

Recommended validation with **x64dbg**:

1. Breakpoint `mxbikes.exe + 0x158be0`. Add a file, hit Reload, and watch whether
   it reaches the `*.pkz` glob or takes the early-return at `+0x158d35`.
2. If it early-returns, the dedup is blocking it → `ResetThenScan` is needed, or
   the registry entry for that folder must be invalidated first
   (`count @ 0x396754`, base @ `0x396760`, entries are `0x20c` bytes:
   `char pathA[260]; char pathB[260]; int32 flag@+0x208`; `flag==0` = plain dir).
3. Read `frostmod.log` — the `[capture]` line shows the real folder path and
   argument pointers the game used, which tells you if the right call was hooked.

If a game update moves the functions, switch from fixed RVAs to the AOB signature
in `offsets.h` (a `FindPattern` over `.text`).

---

## Safety / scope

- Single-player, local, developer-aware quality-of-life tooling. It calls the
  game's **own** content-scan code; it doesn't patch game logic or touch
  protection/anti-cheat.
- Injecting into online/ranked sessions can trip server-side checks — use it for
  offline/testing, and coordinate with the devs before shipping anything public.
- v0.1 leaves its hooks installed for the process lifetime (no clean unload).

## Files

```
frostmod/
├─ CMakeLists.txt        build (fetches MinHook)
├─ README.md
└─ src/
   ├─ offsets.h          RVAs + AOB signature
   ├─ frostmod.cpp       the DLL: UI, hooks, capture-replay reload
   └─ injector.cpp       LoadLibrary injector
```
