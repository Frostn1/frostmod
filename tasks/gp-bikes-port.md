# Port FrostMod to GP Bikes

## Context

MXB App now drives GP Bikes as well as MX Bikes (Frostn1/mxb-app#75). Every
feature ported except the ones that depend on FrostMod, because FrostMod is a
compiled MX Bikes plugin: its `frostmod.exe` waits for `mxbikes.exe`, and
`src/offsets.h` is a table of RVAs recovered from that specific binary.

In the app those features are gated off for GP Bikes by
`Caps { frostmod: false, instant_refresh: false }` in `src-tauri/src/game.rs`.
**The integration point is a one-line flip of those two flags** once FrostMod
ships a GP build — the app already hides the FrostMod panel, the settings
section and the "FrostMod will hot-reload…" hint for any title whose caps say so.

## What was verified (2026-08-08, against `~/Downloads/gpbikes.exe`)

Read this before planning: it settles what's cheap and what isn't.

| | MX Bikes | GP Bikes |
|---|---|---|
| PE | x64, ImageBase `0x140000000` | x64, ImageBase `0x140000000` — same |
| `.text` vsize | 3,274,675 | 3,483,267 (~6% larger) |
| Sections | `.text .rdata .data .pdata .rsrc .reloc .bind` | `.text .rdata .data .pdata .rsrc .bind` (no `.reloc`) |
| `.text` entropy (shipped) | 8.000 | 8.000 |

- **`gpbikes.exe` is SteamStub-wrapped** (`.bind` section) and its `.text` is
  encrypted on disk — entropy 8.000, no valid prologue bytes. It must be
  unpacked with **Steamless** before any static analysis, exactly as
  `mxbikes.exe` → `mxbikes.exe.unpacked.exe` was. (Unpacked MX `.text` entropy
  is 6.263 and starts `40 53 48 83 EC 30` = `push rbx; sub rsp,0x30`.)
- **`.rdata` / `.data` are *not* encrypted** — strings are readable in the
  shipped file. That is why the anchors below could be gathered without
  unpacking, and it means string-driven RE will work the moment it is unpacked.
- **No RVA from `offsets.h` can be reused.** Different build, different layout.
  Every constant has to be re-derived. Treat the existing file as a *map of what
  to look for*, not as values to adjust.
- **The engine is the same shape**, so the analogous functions exist. Anchor
  strings confirmed present in `gpbikes.exe`:

      %s*.pkz              %s.pkz              bikes\%s\%s.cfg   (29 hits)
      tracks\%s  (78)      misc\objs\objs.cfg  gpbikes.ini       plugins

## Tier-1 offsets — already derived (2026-08-08)

Recovered statically from `~/Downloads/gpbikes.exe.unpacked.exe`
(ImageBase `0x140000000`, `.text` entropy 6.035 — genuinely unpacked). **These are
the two constants live mod reload needs**, so tier 1 below is done; start at the
§1 refactor and plug these in.

```cpp
namespace gpb {
// Boot content-load + app-init. GP twin of mxb::RVA_CONTENT_INIT (0xef210).
constexpr uintptr_t RVA_CONTENT_INIT = 0xfb650;   // .pdata range 0xfb650-0xfcfb7
// Generic VFS directory walker (out_status, dir, ext, out_buf).
constexpr uintptr_t RVA_SCAN_FOLDER  = 0x18f150;  // 3 call sites, as on MX
}
```

**How `RVA_CONTENT_INIT = 0xfb650` was identified.** MX's `0xef210` references a
distinctive string set (`%sglobal.ini`, `mods`, `mxbikes.ini`, `cache`,
`%stextures\`, `nickname`, `lastprofile`, `%sprofiles`, `unnamedProfile`,
`%sprofiles\%s\profile.ini`, `texture_quality`, `core`, …). Fingerprinting every
`.pdata` function in GP against that set — with `mxbikes.ini` swapped for
`gpbikes.ini` — put `0xfb650` top of the list, sharing 19 of 23 strings. It is
also one of only three functions referencing `gpbikes.ini`. Corroborating:

- **Called from exactly one site** (`0x402e0`, in `0x402a0`) — matches "called
  once from WinMain".
- **Takes `int mode` in `ecx`**: the prologue opens `mov [rsp+8], ecx` before
  `mov eax,0x1518 / call __chkstk / sub rsp,rax`, i.e. the same
  `__fastcall(int mode, …)` shape as MX's, with a large frame.
- Its string set includes the GP-specific `%sprofiles\%s\calib.txt`,
  `%sprofiles\%s\controls.txt`, `testing.ini` — profile/boot work, as expected.

**How `RVA_SCAN_FOLDER = 0x18f150` was identified — two independent methods that
agree exactly.**

1. **Prologue signature.** MX's `SIG_SCAN_FOLDER`
   (`40 53 56 57 41 54 41 55 41 56 48 81 EC F8 07 00 00 48 8B 05`) matches
   `0x18f150` byte-for-byte, *including the same `0x7f8` stack size*. Searching GP
   for that pattern at a `.pdata` function start yields exactly **one** hit.
2. **`.pdata` index delta.** In MX, `scan_folder` sits 4 functions before the
   `%s*.pkz` wrapper. GP's `%s*.pkz` wrapper is `0x18f53b`; minus 4 entries →
   `0x18f150`.

It also has **3 call sites** (`0x16020b`, `0x237421`, `0x2bee1d`), matching MX's
3 (`0x1266a8`, `0x204041`, `0x296e8d`).

**The `.pkz` wrapper functions map across cleanly too**, which is a useful sanity
check that the two builds really are the same source:

| | MX | GP |
|---|---|---|
| `%s.pkz` refs | `0x1576f0`, `0x158fcb`, `0x15939b` | `0x18dc60`, `0x18f53b`, `0x18f90b` |
| `%s*.pkz` ref | `0x158fcb` | `0x18f53b` |
| `%s.pkz` string | `0x325358` | `0x358c5c` |
| `%s*.pkz` string | `0x325380` | `0x358c90` |

**Caveats.** Both are *static* derivations against this specific build; confirm
under x64dbg before trusting them in a release. And `RVA_CONTENT_INIT` carries
the same warning as on MX — it re-inits Steam/input/sound and transitions to the
UI, so calling it mid-game is a soft restart to the menu, not a surgical reload.
The reproduction scripts live in the mxb-app session scratchpad
(`pibore.py` / `fingerprint.py`: `.pdata` function ranges, RIP-relative xref
resolution, string fingerprint matching) — worth keeping if more offsets are
needed, since the same fingerprint-and-corroborate loop will find them.

## Recommended approach

### 1. Make FrostMod multi-game before re-deriving anything

`src/frostmod.cpp` has ~88 `mxb::` references. Don't fork the file. Mirror what
mxb-app did in `src-tauri/src/game.rs`:

- Split `offsets.h` into `namespace mxb` and `namespace gpb` behind a common
  `struct GameOffsets` (a POD of the RVAs + AOB pairs), and select one at
  runtime from the attached process image name (`mxbikes.exe` / `gpbikes.exe`).
- `launcher.cpp` waits for a specific exe — make the target a parameter
  (`--game mxb|gpb`, default: whichever it finds first).
- Add a capability flag per game so a feature with no GP offsets yet is
  *disabled*, not crashing on a `0x000000` RVA. `RVA_TRACK_LOADER` is already a
  `0x000000` TODO in the MX table — that pattern needs to become explicit.

Doing this first means partial GP support ships safely: badge works, reload
works, server filter says "not supported on GP Bikes yet" instead of jumping
into a wild address.

### 2. Take the free win: the PiBoSo plugin API is shared

FrostMod already runs as a PiBoSo plugin (`frostmod.dlo` in the game's
`plugins` folder) and draws its badge through the sanctioned `Draw()` callback.
**That API is common across PiBoSo titles** — GP Bikes loads `.dlo` plugins from
the same place. So the badge and anything else driven by the plugin callbacks
should port with little or no RE. Verify the SDK struct layout matches GP's
plugin headers before assuming it's byte-identical.

Note: the app's own experience is that the plugin API exposes roster/name/bike
data only — no paint data — so don't plan on it for anything content-related.

### 3. Re-derive offsets in this priority order

Only tier 1 is needed to flip `caps.frostmod` on.

**Tier 1 — live mod reload** (the feature the app actually gates on)
- **DONE (v0.11.0)** — but note the trap this section originally walked into.
  `RVA_CONTENT_INIT = 0xfb650` and `RVA_SCAN_FOLDER = 0x18f150` are *not* what the
  reload runs. `content_init` is only a gate ("did offsets resolve"); the reload is
  `kReloadSteps`, a transcription of the content-load section of boot init. v0.10.0
  wired in the two constants above, declared tier 1 done, and left GP Bikes running
  MX Bikes' step table — which crashed the game on the first reload.
  GP's own table is now in `gpb::kReloadSteps`, transcribed from `0xfb95a`.
- Deriving it is mechanical, and worth reusing for any future title: disassemble the
  contiguous run of loader calls inside boot init, then identify each by the path
  strings it and its callees reference (`%stracks`, `%styres`, `%srider`, `%sbikes`,
  …). The same pass run against MX's `0xef68e` reproduces the shipped 21-step table
  exactly, including the DIR steps' zeroed globals — that is how to check the method
  before trusting it on a new binary. Note the zero-stores are `mov [rip+X], <reg>`
  with a register xor'd to zero, not `mov [rip+X], 0`.
- GP's shape differs from MX's: MX inlines the per-directory dance into boot init for
  some categories (the DIR steps), GP wraps it inside each loader, so every GP step is
  self-contained and needs no z-globals or operands.

**Tier 2 — badge**: should come free via §2. Confirm, don't re-derive.

**Tier 3 — server browser filter**: the whole `RVA_SB_*` / `RVA_MP_*` group plus
`SB_Entry`. This is the largest block in `offsets.h` and the most build-fragile.
Note GP Bikes has its **own master server and default port** — MX's
`MXB_DEFAULT_PORT = 54200` does not apply, and no `master.*` hostname string is
present in either binary (it's likely assembled at runtime), so find it from the
master-protocol handler rather than by grepping strings.

**Tier 4 — direct connect**: already PREVIEW-only on MX (the command-bus input
layout is unresolved). Don't port an unfinished feature; leave it MX-only.

### 4. Prefer AOB signatures over raw RVAs for GP from day one

`offsets.h` already carries `SIG_*`/`SIG_*_MASK` pairs as drift fallbacks. GP
Bikes updates on its own cadence, and nobody wants to re-RE on every patch —
derive the signature at the same time as the RVA, not later.

## Field result — the v0.11.0 table crashes too (2026-08-09)

A reporter running v0.11.0 on GP Bikes (`frostmod-2.log`, Windows, RTX 4060 Ti):
`[reload] surgical content reload (stepped over 13 frames)...` with **no**
`[reload] done` after it and the process gone. Once on the session's first
reload, once on the fourth after three that completed in ~2s each. Same log's
v0.10.0 sessions show the older every-time crash, so this is a different, milder
failure — not the one v0.11.0 fixed.

So GP Bikes' reload is **off by default** as of v0.12.0 (`reload_verified =
false` in `GAME_GPB`), armed only with `--unsafe-reload`. What is still needed:

1. ~~**Which step.**~~ **ANSWERED — step 1, `tracks`, `0x139A0`.** See the
   v0.12.0 field result below.
2. ~~**Which thread.**~~ **ANSWERED — same thread. The race theory is dead.**
   See below.
3. **The 8 unconfirmed RVAs.** The log corroborates only tracks `0x139A0`,
   tyres `0x34AE0`, rider `0x34080`, bikes `0x14D90` and bike paints `0x32090`
   (each shows the game-dir and mods-dir scans sharing one boot-init return with
   two call sites inside the loader — the SC shape this table assumes).
   `LogScanCallers` stops after 16 stack shots, which is why the rest never
   appear. Raising that cap on a diagnostic build would corroborate the other 8
   from a plain startup log, with no reload and no crash — do that first, it is
   free.

## Field result — it is step 1, and the thread theory is dead (2026-08-12)

A reporter running **v0.12.0** on GP Bikes (`frostmod-3.txt`, Windows), armed
with `--unsafe-reload`, across three sessions:

- **All three die at `[reload] step 1/13 rva=0x139a0 - tracks`.** No step 2 in
  the whole 108 KB log, no `[reload] done`, file ends mid-reload. Armed, it is
  deterministic — not the intermittent failure v0.11.0 showed.
- **The boot scan tid and the reload tid are identical** in every session
  (33300, 27112, 34764). We already replay on the thread that owns the content
  lists, so item 2 above is answered and wrong. Cross it off.
- He was in the **menus**, not a session (no `[cb]` callbacks, no `EventInit`,
  `persp=0`), so no track needs to be loaded for this to happen. `[sig] scanner
  signature VERIFIED` rules out offset drift on his build.

Disassembly of `0x1400139A0` in `gpbikes.exe.unpacked.exe` adds two things:

- **The RVA is right.** Its two internal call sites are `0x13a18` (game dir) and
  `0x13a36` (mods dir) — exactly the frames the boot stacks carry.
- **It zeroes its list globals without freeing them** — `0x1405F21C8` (count),
  `0x14094B428` (track array base), `0x1405E42D0` (index array). Safe exactly
  once, at boot, when they are already null. A second call abandons live
  allocations and rebuilds underneath anything still pointing at them.
- **It is stack-cookie guarded**, ending in `call 0x1402DDC00`
  (`__security_check_cookie`) after `xor rcx, rsp`. A cookie failure is
  `__fastfail`, which **SEH cannot intercept** — which is why `RL_SC`'s guard
  never sees it and the process vanishes with nothing logged. The recursive
  directory walk at `0x13840` is 0x278/level under the loader's 0xFB8 frame and
  `scanFolder`'s 0x7F8; at boot that runs six frames above thread start, at
  reload it runs deep inside the present path — same thread, far deeper stack.

**Next:** `--unsafe-reload-from=2` (added here) skips the tracks step. If 2..13
then complete, that one loader is unsafe to re-run and the fix is targeted —
restore its globals, or drop tracks from GP's table. If step 2 dies identically,
the call site is the problem and the fix is *where* we replay from, not *what*.

## Verification

There is no unit test for this; it is verified by running it.

1. `cmake -B build -A x64 && cmake --build build --config Release` in the
   **x64 Native Tools Command Prompt** (Windows x64 only, MSVC).
2. `frostmod.exe --game gpb`, confirm it attaches to `gpbikes.exe` and the badge
   renders.
3. Drop a `.pkz` into `Documents\PiBoSo\GP Bikes\mods\tracks`, press `R`, and
   confirm the track appears in-game without a restart. That is the acceptance
   test for tier 1.
4. Re-run the whole flow against **MX Bikes** to prove the refactor in §1 didn't
   regress it — this is the main risk of the change, not the GP work.
5. In MXB App, flip `frostmod`/`instant_refresh` to `true` in `GPB` in
   `src-tauri/src/game.rs` and confirm the panel, the settings section and the
   install-hint reappear for GP Bikes.

## Constraints worth stating up front

- **Windows only.** The DLL is injected into a Windows x64 process and built
  with MSVC; none of it can be built or exercised on macOS or Linux.
- **Unpacking is a prerequisite**, and Steamless is a Windows/.NET tool. Nothing
  static can start until `gpbikes.exe.unpacked.exe` exists.
- Keep the AV-hardening in `CMakeLists.txt` (version resource per binary) for
  any new artifact — an unsigned PE with no version block trips ML heuristics.
