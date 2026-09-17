# frostmod

## Publishing rules for RE work (mandatory)

These cover every repo and every public writeup that comes out of reverse engineering
PiBoSo's games. Doing the analysis is defensible; publishing the wrong artefact is not.

**Never commit, release or attach:**

1. **Any DRM unpacker.** SteamStub or Steam-wrapper decryption code, an `--unpack-to` style
   flag, or a README describing the stub header, the AES key or the CBC decrypt. Analysing a
   copy you own falls under the reverse-engineering-for-interoperability exemptions;
   *distributing the tool* is the anti-trafficking prong of DMCA §1201, and that is the only
   part with real teeth. Keep it untracked, and keep it out of history.
2. **Game binaries** — packed, unpacked, patched or diffed. Not in a repo, not in a release
   asset, not attached to a post.
3. **Anything that lets someone play without owning the game.**

**Wording a crash writeup or a README:**

- Present findings as *observed behaviour* — crash dumps, Procmon, a debugger on a licensed
  copy. Never describe how to unwrap the exe and never name the unpacker.
- Prefer offsets in public components, and say whose they are. `msvcr90.dll+0x36EDE` is
  Microsoft's redistributable runtime with public symbols, not PiBoSo's code, and nowhere
  near a protection measure.
- Keep the free crash-fix work visibly unpaid and separate from the paid mxbsecure side. Free
  removes the damages story and strengthens the interoperability framing. The paid side
  enforces protection *for* creators, which is worth having on the record.

The realistic risk is not a courtroom. It is a GitHub DMCA takedown, a forum ban, an email, or
a plugin API that quietly breaks. The three items above are what would turn an annoyed
developer into one with a case.

If a letter or a takedown ever arrives: get an actual lawyer, do not pull the work
reflexively, and do not answer it alone.
