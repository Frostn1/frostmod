# tools/rederive — re-derive the offsets when PiBoSo ships a build

Every address in `src/offsets.h` belongs to one build. beta21d → beta21e moved all of
them and renumbered the message ids, and nothing about `base + 0xecd00` announces that
it is no longer the customization loader — it is simply whatever now lives there.

This tool turns "re-do the RE" into "run a script and read a diff". It takes the new
`mxbikes.exe`, decrypts the Steam DRM wrapper itself, and looks the offsets up again by
what they *are* — the strings a function owns, the imports it calls, the array it
multiplies an index through — rather than by where they used to be.

    ./rederive.py ~/Downloads/mxbikes.exe                  # the diff
    ./rederive.py mxbikes.exe --header                     # an offsets.h block
    ./rederive.py mxbikes.exe --rust                       # mxb-app's two constants
    ./rederive.py mxbikes.exe --json new.json              # machine-readable
    ./rederive.py mxbikes.exe --check                      # exit 1 on any mismatch
    ./rederive.py mxbikes.exe --unpack-to unpacked.exe     # just the SteamStub decrypt
    ./selftest.py                                          # the regression test

Needs Python 3.10+ and `cryptography`. No disassembler, no Windows, no IDA.

## What it does on a new build

1. **Unpacks.** The shipping exe is SteamStub v3.x: `.text` is AES-encrypted in the
   file. `steamstub.py` decodes the stub header (rolling XOR, signature `0xC0DEC0DF`),
   ECB-decrypts the IV under the AES key, CBC-decrypts the section and restores the
   original entry point. Verified byte-exact against Steamless's output for beta21e —
   all `0x31F800` bytes of `.text`. This used to be the step that needed a Windows box.
2. **Indexes.** `.pdata` gives an authoritative function table (8741 entries for
   beta21e), so every reference can be attributed to the function that made it without
   disassembling anything. Chunked functions fold back together through their unwind
   chain info.
3. **Resolves.** Each anchor in `anchors.py` is a rule, not an address. They run in
   dependency order; later ones build on what earlier ones found.
4. **Reports.** Every row says how it was found and how far it moved:

   ```
    -> RVA_MP_MSG_HANDLER            0x2a10e0  high    (was 0x29f880)
           owner of 'HOSTED' + 'ENDPOINT' + 'PING' + 'ECHO'
    ~  CMD_JOIN                         0x389  low     (baseline value kept)
    !! RVA_SB_WORLD_CMD                      -  none
           ambiguous owner of 'ID_SPECTATE' + 'ID_JOIN': 0xaa260, 0xad960
   ```

   `->` moved, `~` carried from the baseline and *not* re-derived, `!!` needs a human.
   Nothing is ever printed as re-derived when it wasn't.

## The rules

| kind | how it pins things down |
|---|---|
| `Strings` | the one function referencing all of these literals — the strongest anchor there is, since a build keeps its strings while moving every address |
| `ImportUser` | the function that calls `recvfrom` / `sendto` / `getaddrinfo` |
| `Array` / `ArrayOf` | the `mov r64,[rip+g]` … `imul r,r,imm` pair — recovers the list global **and** its entry stride together, which is the only way to notice a build that grew the struct |
| `CountOf` | the loop-bound dword that sits beside loads of that list |
| `SharedGlobals` | what two functions in one subsystem both touch, minus what each owns alone |
| `ClusterBase` + `Rel` | a screen's state block, pinned once and indexed by delta |
| `GlobalNearString` | the local global nearest to where a function handles a literal |
| `Aob` | a masked byte pattern — the fallback for functions with no strings of their own |
| `Const` | carried from the baseline, always flagged |

## What it can't do, and says so

Struct field offsets (`TRK_NAME = +0x20`), protocol ids (`CMD_JOIN = 0x389`) and the
three binary-reader leaf helpers are **meaning, not addresses**. Nothing in the image
says `+0x4C0` is the cfg name. Those 18 entries are carried from the baseline and
reported as carried. If a build changes one, this tool will not catch it — the game
will, and the report tells you which entries were never checked.

## Why you can trust it on a build nobody has seen

`selftest.py` is the argument, and it is four claims:

- our unpack equals Steamless's, byte for byte;
- all 42 derived MX Bikes offsets come back out of the current build unchanged;
- feeding the packed exe and the unpacked one gives identical answers;
- run against **GP Bikes** — a different binary of the same engine — the same rules
  find `RVA_CONTENT_INIT = 0xfb650` and `RVA_SCAN_FOLDER = 0x18f150`, which are the
  values `offsets.h` reached by hand for that game, and correctly report its track
  stride as `0x4b4` rather than MX Bikes' `0x4c4`.

That last one is the real evidence: the rules found offsets in a binary they were never
tuned against.

## When a build lands

```sh
./rederive.py /path/to/new/mxbikes.exe --check    # what moved, what broke
./rederive.py /path/to/new/mxbikes.exe --header   # paste into src/offsets.h
./rederive.py /path/to/new/mxbikes.exe --rust     # paste into mxb-app gameproc.rs
```

Work the `!!` rows by hand, add whatever anchor would have caught them, then refresh
`baselines/mxb.json` and re-run `./selftest.py` so the next build starts from here.

`--rust` also emits the new `TimeDateStamp` for `KNOWN_GOOD_BUILDS`, which is the list
that stops MXB App starting a thread at the old loader address on a build where that
address is now something else.

## Files

| | |
|---|---|
| `rederive.py` | CLI, resolver, report and emitters |
| `steamstub.py` | SteamStub v3.x unpacker |
| `pe.py` | PE64 reader: sections, `.pdata`, imports, strings, rip-relative xrefs, AOB |
| `analysis.py` | function-scoped indices, stride discovery, count voting |
| `rules.py` | the rule vocabulary |
| `anchors.py` | what each offset *is*, one entry per symbol |
| `baselines/` | known-good values per title |
| `selftest.py` | the regression test |
