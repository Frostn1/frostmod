# Port FrostMod to Kart Racing Pro

## Where this stands

**Shipped (v0.15):** Kart Racing Pro is a title FrostMod knows. `--game krp` attaches to
`kart.exe`, uses `Documents\PiBoSo\Kart Racing Pro\mods`, installs the plugin into KRP's
`plugins` folder, and the plugin loads: overlay, radar, rider outlines and the MXB App
session block all run off the sanctioned callbacks.

**Not shipped:** live mod reload. `namespace krp` in `src/offsets.h` is deliberately empty
and `GAME_KRP.content_derived()` is false, so `RequestReload` refuses rather than replaying
another title's addresses — the mistake that took GP Bikes down in v0.10.0 and again in
v0.11.0. Everything below is about closing that gap.

## Why the usual route is blocked

`kart.exe` is SteamStub-wrapped, exactly as `gpbikes.exe` was:

| | MX Bikes | GP Bikes | Kart Racing Pro |
|---|---|---|---|
| PE | x64, ImageBase `0x140000000` | same | same |
| `.text` vsize | 3,274,675 | 3,483,267 | 3,676,131 |
| Sections | `… .rsrc .reloc .bind` | `… .rsrc .bind` | `… .rsrc .bind` (no `.reloc`) |
| `.text` entropy (shipped) | 8.000 | 8.000 | 8.000 |

`.rdata` and `.data` are **not** encrypted — every anchor string below was read straight out
of the shipped file — but `.text` is, so no function can be found statically without running
Steamless on Windows first. GP Bikes was done that way and the resulting table still crashed
the game, twice, because static string-matching identified 8 of its 13 loaders by guesswork.

So this port takes the other route: **let the game tell us.** That costs one boot of KRP
and produces addresses that are true for the exact build being run, not for a copy.

## The capture run

1. Build this branch and install the plugin for KRP:

       cmake -B build -A x64 && cmake --build build --config Release
       build\bin\frostmod.exe --game krp --install-plugin

   (Plugin mode, not injection: it is loaded before the game's one-time content scan, which
   is the only moment the boot loaders are called.)

2. Start Kart Racing Pro, let it reach the main menu, quit.

3. Send `frostmod.log` (next to `frostmod.dll`).

## Reading the log

With no derived offsets, FrostMod sweeps `.text` for `mxb::SIG_SCAN_FOLDER` instead of
trusting an RVA. That signature is byte-identical between MX Bikes and GP Bikes, which is
the reason to expect it to match a third build of the same engine.

**`[sig]` — one line, and it is `krp::RVA_SCAN_FOLDER`:**

    [sig] Kart Racing Pro: scanner found at RVA 0x1a2b3c by signature (after 1400ms) ...

If instead it says the signature never appeared, KRP's scanner prologue differs and this
route needs an AOB derived for it; nothing else in the port is affected.

**`[stack]` — one line per content category, and they are the reload table.** Each is a
stack walk from inside the scanner:

    [stack] dir='' ext='pkz': 0x1a2b3c <- 0x1c0f4 <- 0x139a0 <- 0xfb95f <- 0x402e5(ext)
                              scanner     dispatcher  LOADER    boot-init return

- The frame **just above the dispatcher** with a small RVA is the content loader — one
  `RLStep::rva` per category. `dir`/`ext` on the same line name the category (`tracks`,
  `karts`, `tyres`, `helmets`, …), which is what `RLStep::what` should say.
- The frame above it is the return address **inside boot init**, so the lowest of them
  minus its prologue gives `krp::RVA_CONTENT_INIT`; they should all fall in one contiguous
  run of `call` sites, as MX's `0xef210` and GP's `0xfb650` do.
- **Two call sites inside one loader under a single boot-init return** means that loader
  scans both the game dir and the mods dir itself — the `SC` shape, `dir = 0`, no z-globals.
  A category whose two directory scans have *different* boot-init returns is the `DIR`
  shape, and needs the three list globals and the two operands, which this log does not
  give. GP Bikes' loaders were entirely SC; MX Bikes' are mixed.

The stack-shot cap is raised to 96 for a title with no table (it is 16 for a ported one),
because GP's port was left guessing at 8 of 13 categories when its log stopped at 5.

## Then

Fill `namespace krp`, point `GAME_KRP` at it, and leave `reload_verified` **false**:
`--unsafe-reload` is how the first run of a new table is collected, one SEH-guarded step at
a time, with each step's name written to the log *before* it runs. A table earns `true` only
after someone watches a reload finish on Kart Racing Pro itself.

`tests/offsets_test.cpp::krp_is_plugin_only_until_its_offsets_are_derived` asserts the
current state, so filling any of this in has to come past that test — update it in the same
commit.

## Also open

- **Does `EventInit` carry a server name?** `krp_example.c` (data version 6) says the event
  ends at `m_iType`, 748 bytes, with no server name and no GUID. MX Bikes' published example
  is *also* missing the tail its shipped build has, so KRP's may have one too. `EventInit`
  logs a `[session] NOTE:` line whenever the game hands it more than 748 bytes — if the
  capture log has that line, the tail is worth deriving (MX's is
  `char server[64]; int serverType; char guid[100]`, which would make 916).
  Until then the session block takes the room key from `RaceEvent`'s `m_szName`, and the
  `[cb] RaceEvent fired` line in the same log says whether that callback reaches the client
  at all — on MX Bikes it does not.
- **The un-ported groups** (server browser filter, master protocol, direct connect, the
  in-game vehicle array) are off for KRP via `offsets_complete = false`, and stay off.
- **MXB App** knows `mxb` and `gpb` only (`src-tauri/src/game.rs`). A KRP profile there is
  separate work in that repo; the id `krp` is already what it would send.

## Anchors confirmed present in `kart.exe`

Read out of the unencrypted `.rdata`, and enough to know the analogous code exists:

    %s*.pkz    %s.pkz    %skarts\%s\%s.cfg    %stracks\%s%s\%s.ini    kart.ini
    plugins    %s*.dlo   GetModID / GetModDataVersion / GetInterfaceVersion
    Startup / Shutdown / EventInit / RunTelemetry / Draw / RaceTrackPosition /
    RaceClassification / RaceAddEntry            (the full callback table, in MX's order)

Note the content folder is `karts`, not `bikes` — nothing FrostMod does today keys on that,
but a step table's `what` labels should say `karts`.
