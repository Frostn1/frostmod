#!/usr/bin/env python3
"""Cut the second utterance off a clip that runs long.

Piper says the word and then, sometimes, keeps talking. `make_clips.py` ends a phrase
on a silence of GAP_S, and the male voice's pause before its extra speech is far shorter
than that -- in `gas` and `brake` there is no silence at all, only a dip. So those clips
shipped with a whole second sentence baked in, and because the clip is normalised over
its full length, the real word was scaled DOWN and the rubbish played at full volume.

The rule here needs no model and no regeneration: the female clip of the same word is the
reference for how long the word IS. Anything much past that is not the word. Inside that
window the cut lands at the quietest point, so it falls in a dip rather than mid-vowel.

Run:  python3 tools/voice/retrim.py [--write]
"""
import math, struct, sys, wave
from pathlib import Path

HERE = Path(__file__).resolve().parents[2] / "src" / "voice"
FRAME_MS = 5
GATE_DB = -40.0
# How much longer than the female's word the male's may legitimately be, when deciding WHERE
# to cut. Male speech here runs a little slower.
LONGER = 1.35
# And how much longer the whole clip has to be before it is touched at all. A clip a quarter
# longer is a slower speaker; one half again as long again is a second utterance. `scrub` sits
# at 1.27x with a real trailing consonant, and must not be trimmed.
DOUBLED = 1.5
# Never cut inside this much of the word itself.
KEEP = 0.85
FADE_MS = 8


def read(path):
    w = wave.open(str(path))
    assert w.getsampwidth() == 2 and w.getnchannels() == 1, path
    n, sr = w.getnframes(), w.getframerate()
    return list(struct.unpack(f"<{n}h", w.readframes(n))), sr


def write(path, data, sr):
    w = wave.open(str(path), "wb")
    w.setnchannels(1), w.setsampwidth(2), w.setframerate(sr)
    w.writeframes(struct.pack(f"<{len(data)}h", *data))
    w.close()


def frames(data, sr):
    step = max(1, int(sr * FRAME_MS / 1000))
    out = []
    for i in range(0, len(data), step):
        seg = data[i : i + step]
        if not seg:
            break
        rms = math.sqrt(sum(x * x for x in seg) / len(seg))
        out.append(20 * math.log10(rms / 32768) if rms > 0 else -99.0)
    return out, step


def voiced_end(data, sr):
    """Last frame above the gate, in seconds."""
    e, step = frames(data, sr)
    last = max((i for i, db in enumerate(e) if db > GATE_DB), default=len(e) - 1)
    return (last + 1) * step / sr


def cut_at(data, sr, limit_s):
    """The quietest 40 ms inside the window where the word must already have ended."""
    e, step = frames(data, sr)
    lo = int(KEEP * limit_s * sr / step)
    hi = min(len(e), int(LONGER * limit_s * sr / step))
    span = max(1, int(0.040 * sr / step))
    if hi - lo < span:
        return None
    best, best_db = lo, 1e9
    for i in range(lo, hi - span):
        db = sum(e[i : i + span]) / span
        if db < best_db:
            best, best_db = i, db
    return (best + span // 2) * step / sr


def main(write_out):
    changed = 0
    for wav in sorted((HERE / "male").glob("*.wav")):
        mine, sr = read(wav)
        fem_path = HERE / "female" / wav.name
        fem, fsr = read(fem_path)
        limit = voiced_end(fem, fsr)
        if len(mine) / sr <= (len(fem) / fsr) * DOUBLED:
            print(f"  {wav.name:14} {len(mine)/sr:.2f}s  ok ({len(mine)/sr/(len(fem)/fsr):.2f}x the female)")
            continue
        at = cut_at(mine, sr, limit)
        if at is None:
            print(f"  {wav.name:14} {len(mine)/sr:.2f}s  LONG but no clean cut found")
            continue
        end = int(at * sr)
        out = mine[:end]
        fade = min(int(FADE_MS * sr / 1000), len(out))
        for k in range(fade):  # out, so the cut is not a click
            out[len(out) - fade + k] = int(out[len(out) - fade + k] * (1 - (k + 1) / fade))
        peak = max((abs(x) for x in out), default=1) or 1
        gain = (32768 * 10 ** (-1.0 / 20)) / peak  # -1 dBFS over the word alone
        out = [max(-32768, min(32767, int(x * gain))) for x in out]
        print(f"  {wav.name:14} {len(mine)/sr:.2f}s -> {len(out)/sr:.2f}s  (female word ends {limit:.2f}s)")
        changed += 1
        if write_out:
            write(wav, out, sr)
    print(f"{changed} clip(s) {'rewritten' if write_out else 'would change'}")
    return changed


if __name__ == "__main__":
    main("--write" in sys.argv)
