# Antivirus false positives

FrostMod's binaries are periodically flagged by a handful of antivirus engines on
VirusTotal. This document explains **why**, what we do about it in the build, and
**how to file a false-positive dispute** with each vendor when it happens.

## TL;DR — it's a false positive

A representative VirusTotal scan flagged **6 of ~70 engines**, and *every* hit was
a machine-learning / heuristic classifier, not a signature match:

| Engine | Label | Detection type |
|---|---|---|
| SentinelOne (Static ML) | `Static AI - Suspicious PE` | ML |
| Symantec | `ML.Attribute.HighConfidence` | ML |
| Elastic | `Malicious (high Confidence)` | ML |
| Trapmine | `Malicious.moderate.ml.score` | ML |
| MaxSecure | `Trojan.Malware.<n>.susgen` | generic/ML |
| Bkav Pro | `W32.Malware.<hash>` | generic bucket |

Every signature-based engine — **Microsoft Defender, Kaspersky, ESET,
BitDefender, Sophos, CrowdStrike, Malwarebytes, GData, DrWeb** — reports
**Undetected**. "Only the static-AI engines fire, none of the signature engines
do" is the textbook fingerprint of a legitimate-but-unusual binary.

## Why FrostMod trips ML heuristics

FrostMod is a mod loader / in-game overlay for MX Bikes. Its legitimate mechanics
are, feature-for-feature, the same ones malware uses — so a static classifier that
has never run the program scores it as an injector/trojan:

- **Remote-thread DLL injection** into `mxbikes.exe` — `OpenProcess` →
  `VirtualAllocEx` → `WriteProcessMemory` → `CreateRemoteThread(LoadLibraryA)`
  (`src/launcher.cpp`).
- **Runtime API hooking** of the OpenGL present path via MinHook to draw the
  overlay (`src/frostmod.cpp`).
- **RWX trampoline** — `VirtualAlloc(PAGE_EXECUTE_READWRITE)` with hand-written
  machine code patched into the game at fixed offsets (`src/frostmod.cpp`,
  `src/offsets.h`).
- **HKCU Run-key auto-start** (`--install-startup`, `src/launcher.cpp`).
- **Self-updater** that downloads `frostmod.exe`/`frostmod.dll` from GitHub
  Releases and relaunches the new exe (`--update`, `src/launcher.cpp`) — the
  classic downloader / self-replacing-executable pattern.

None of this is malicious; it is just indistinguishable from an injector to a
model that only sees the static PE.

## What the build does to reduce false positives

Ranked by impact:

1. **Code signing (biggest lever).** An Authenticode signature plus accrued
   reputation is the strongest benign signal and flips most ML engines. The build
   supports it but it is **OFF until a certificate is supplied** — see
   `CMakeLists.txt` (`FROSTMOD_SIGN`). With an OV/PFX cert:
   ```
   cmake -B build -A x64 -DFROSTMOD_SIGN=ON \
         -DFROSTMOD_SIGN_PFX="C:/keys/frostmod.pfx" -DFROSTMOD_SIGN_PASSWORD="****"
   ```
   With an EV cert on a hardware token, pass the thumbprint instead:
   ```
   cmake -B build -A x64 -DFROSTMOD_SIGN=ON -DFROSTMOD_SIGN_SHA1=<thumbprint>
   ```
2. **PE version resource (done, free).** `src/version_info.rc.in` embeds
   CompanyName/ProductName/FileDescription/FileVersion/OriginalFilename into both
   binaries so they no longer look like a metadata-less dropper.
3. **Not packed (already true).** FrostMod uses no UPX/packer — keep it that way;
   packing is a major ML red flag on its own.
4. **Stable release hashes + reputation.** Ship consistent artifacts per release
   so SmartScreen/vendor reputation can accumulate and suppress ML hits over time.

## Filing a false-positive dispute

**Before you start:** make sure the exact flagged file is public on VirusTotal and
copy its **SHA-256** and the **VT report URL** — nearly every vendor wants one or
both. Then hit **Reanalyze** on the VT report first; a detection sometimes clears
on its own after the vendor's model updates.

> VirusTotal only *aggregates* other engines and has **no** dispute button or
> verdict of its own — you must contact each flagging vendor directly.
> Ref: https://docs.virustotal.com/docs/false-positive

Fill in before sending:
- `SHA256_EXE` / `SHA256_DLL` — hashes of the flagged files
- `VT_URL` — the VirusTotal report link
- `RELEASE_URL` — https://github.com/Frostn1/frostmod/releases/latest

### Channels (verified July 2026)

| # | Vendor | Channel | Public? | Notes |
|---|---|---|---|---|
| 1 | **Symantec / Broadcom** | https://symsubmit.symantec.com/ → **"Clean Software Incorrectly Detected"** tile | ✅ Public | Official. Upload file or give MD5/SHA256 + VT link. Reference # by email. |
| 2 | **Elastic** | Form: https://forms.gle/LSYYPu9iS4Ex5R8p8 | ✅ Public | Official (posted by Elastic staff). **Email no longer accepted** — form only. |
| 3 | **SentinelOne** | Support ticket via customer portal; non-customer email `report@sentinelone.com` | ⚠️ Mostly customer-gated | Email is community-sourced, not vendor-published. Often clears with reputation. |
| 4 | **Bkav** | Email `fpreport@bkav.com` (cc `bkav@bkav.com`) | ✅ Public email | Address community-sourced; no dedicated FP form found. Attach sample + VT link. |
| 5 | **MaxSecure** | Email `tech@maxpcsecure.com` / `info@maxpcsecure.com`; or their "Submit Samples" tool | ✅ Public email | No FP web form. Send sample + VT link. |
| 6 | **Trapmine** | Email `fp@trapmine.com` | ⚠️ Uncertain | IP acquired by SonicWall (2023); mailbox may be unmonitored. Low priority — likely ages out on a VT re-scan. |

Confidence: **Symantec, Elastic, and the VirusTotal guidance are first-party
confirmed.** The SentinelOne/Bkav/MaxSecure/Trapmine emails are consistent across
independent false-positive directories but are **not** published on the vendors'
own sites — treat them as best-available.

### Ready-to-paste justification

> **Subject:** False positive on signed open-source software — FrostMod
>
> Your engine flags the following files from my open-source project FrostMod, a
> mod loader / in-game overlay for the game MX Bikes. These are false positives.
>
> - frostmod.exe — SHA-256: `SHA256_EXE`
> - frostmod.dll — SHA-256: `SHA256_DLL`
> - VirusTotal report: `VT_URL`
> - Source code (MIT, fully public): https://github.com/Frostn1/frostmod
> - Official release: `RELEASE_URL`
>
> Your detection is a machine-learning/heuristic verdict; no signature-based
> engine (Microsoft, Kaspersky, ESET, BitDefender, Sophos, CrowdStrike) detects
> these files. FrostMod performs DLL injection and API hooking **into a single
> game process the user launches**, purely to render an in-game overlay and load
> mods — the same mechanics as legitimate overlays (Discord, Steam, RivaTuner).
> The full source is public and the binaries are unmodified builds of it.
>
> Please review and whitelist. I'm happy to provide anything else you need.

## When it recurs

New releases produce new hashes, so a fresh build can re-trip the same ML engines
until it earns reputation. The durable fix is **code signing + reputation**; the
per-release dispute above is the stopgap. Re-run the VT **Reanalyze** a few days
after signing/submitting to confirm the detections have cleared.
