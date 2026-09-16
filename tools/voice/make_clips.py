#!/usr/bin/env python3
"""Generates mxbcoach.dlo's spoken cue clips into src/voice/<voice>/.

One clip per cue kind (the list in src/coachvoice.h), in each voice the plugin embeds, spoken
by Piper, an offline neural text-to-speech engine. The rider picks the voice in MXB Coach and
the plugin reads it from cues\\voice.ini.

Both voices are redistributable, which is the whole reason these two were chosen (see NOTICE):

    female  en_US-ljspeech-high   trained from scratch on LJ Speech, public domain
    male    en_US-norman-medium   trained from scratch on LibriVox recordings, public domain

Both are Bryce Beattie's, both speak at 22.05 kHz, and the piper-voices model repository is
MIT. The voices fine-tuned from `lessac` are avoided: its Blizzard 2013 data is research-only,
and that taint carries into anything fine-tuned from it (`joe`, `kusal`, `alan`). `ryan` and
`hfc_male` are CC BY-NC-SA, so they are out too.

Each clip is 22.05 kHz 16-bit mono PCM, normalised to -1 dBFS peak, with a real lead-in kept
before the first sound and a pad of digital silence in front of that. The lead-in is the point:
trimming hard to the first loud frame ate the leading consonant of short words, which is why
"Gas" came out of the game as "gss".

    python3 -m venv .venv && .venv/bin/pip install piper-tts==1.8.0
    .venv/bin/python tools/voice/make_clips.py

Neural synthesis adds random noise, so a run doesn't reproduce the committed clips bit for bit.
"""

import argparse
import hashlib
import os
import sys
import urllib.request
import wave
from pathlib import Path

import numpy as np
from piper import PiperVoice, SynthesisConfig

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "src" / "voice"

BASE_URL = "https://huggingface.co/rhasspy/piper-voices/resolve/main/"

# name -> (piper voice, path under the voice repo, sha256 of the .onnx, length scale).
# The hashes pin exactly which model made the committed clips; under 1 speaks faster.
VOICES = {
    "female": ("en_US-ljspeech-high", "en/en_US/ljspeech/high/",
               "5d4f08ba6a2a48c44592eed3ce56bf85e9de3dd4e20df90541ae68a8310c029a", 0.90),
    "male": ("en_US-norman-medium", "en/en_US/norman/medium/",
             "b9739443232a80a59c7d18810dd856899bf16a7964725f5ab81ea49b1351cb71", 0.95),
}

# In cue-kind order, BRAKE (1) to SIT (10): src/coachvoice.h kClips and CMakeLists.txt.
CLIPS = [
    ("brake", "Brake!"),
    ("off_brakes", "Off the brakes!"),
    ("gas", "Gas!"),
    ("shift_up", "Shift up!"),
    ("shift_down", "Shift down!"),
    ("go_wide", "Go wide!"),
    ("cut_inside", "Cut inside!"),
    ("scrub", "Scrub it!"),
    ("stand_up", "Stand up!"),
    ("sit_down", "Sit down!"),
]

RATE = 22050
PEAK = 10 ** (-1 / 20)  # -1 dBFS

# Trimming, with a different gate at each end - which is the whole fix for "gss".
#
# The START gate is far below the old -40 dB: the burst that opens "Gas" runs 45-50 dB under
# the vowel behind it, so a -40 dB gate scored it as silence and cut it away, and the clip
# began mid-vowel. The END gate stays strict, because the same -55 dB at the tail keeps every
# breath the male voice leaves after a word and turns a 0.6 s clip into a 3 s one.
#
# PRE_S of real audio is kept in front of the first frame that opens the gate, and LEAD_S of
# digital silence is padded in front of that, so every clip starts at zero and the fade-in
# lands on the pad instead of on the first vowel.
START_GATE_DB = -55.0
END_GATE_DB = -38.0
PRE_S = 0.030
LEAD_S = 0.100
POST_S = 0.060
FADE_S = 0.004

# A silence longer than this ends the phrase, and everything after it is dropped.
#
# Asked for one short word, the male voice says it, pauses for the better part of a second and
# then says something else entirely - "Brake!" came out 3.7 s long, as the word, a gap, and a
# second utterance nobody asked for. Trimming on level alone keeps all of it, because the
# hallucinated tail is as loud as the word. Cutting at the first real gap keeps the word. It is
# well clear of the gaps inside a phrase: the closure in "Off the brakes" runs 50-80 ms.
GAP_S = 0.25


def fetch_voice(cache: Path, voice: str, path: str, want_sha: str) -> Path:
    cache.mkdir(parents=True, exist_ok=True)
    onnx, cfg = cache / f"{voice}.onnx", cache / f"{voice}.onnx.json"
    for f in (onnx, cfg):
        if not f.exists():
            print(f"downloading {f.name}")
            urllib.request.urlretrieve(BASE_URL + path + f.name, f)
    digest = hashlib.sha256(onnx.read_bytes()).hexdigest()
    if digest != want_sha:
        sys.exit(f"{onnx} has sha256 {digest}, expected {want_sha}")
    return onnx


def espeak_dir(cache: Path) -> Path:
    # piper-tts's macOS wheel ignores a data dir named espeak-ng-data and falls back to a path
    # from its CI build, so hand it the bundled data under another name.
    import piper

    link = cache / "espeak-data"
    if not link.exists():
        link.symlink_to(Path(piper.__file__).parent / "espeak-ng-data")
    return link


def shape(audio: np.ndarray) -> np.ndarray:
    peak = float(np.max(np.abs(audio))) or 1.0
    win = int(RATE * 0.01)
    frames = len(audio) // win
    rms = np.sqrt(np.mean(audio[: frames * win].reshape(frames, win) ** 2, axis=1) + 1e-12)
    db = 20 * np.log10(rms / peak)
    opens = np.nonzero(db > START_GATE_DB)[0]
    if len(opens):
        # Walk forward from the first sound to the end of the FIRST phrase: the last frame
        # above the end gate before a silence long enough to mean the voice has finished.
        first = int(opens[0])
        allowed = int(GAP_S / 0.01)
        last, quiet = first, 0
        for i in range(first, len(db)):
            if db[i] > END_GATE_DB:
                last, quiet = i, 0
            else:
                quiet += 1
                if quiet >= allowed:
                    break
        start = max(0, first * win - int(RATE * PRE_S))
        end = min(len(audio), (last + 1) * win + int(RATE * POST_S))
        audio = audio[start : max(end, start + win)]
    audio = np.concatenate([np.zeros(int(RATE * LEAD_S)), audio])
    fade = int(RATE * FADE_S)
    ramp = np.linspace(0.0, 1.0, fade)
    audio = audio.copy()
    audio[:fade] *= ramp
    audio[-fade:] *= ramp[::-1]
    audio *= PEAK / (float(np.max(np.abs(audio))) or 1.0)
    return np.clip(np.round(audio * 32767), -32768, 32767).astype("<i2")


def generate(name: str, cache: Path, length_scale: float) -> int:
    voice_id, path, want_sha, default_scale = VOICES[name]
    voice = PiperVoice.load(fetch_voice(cache, voice_id, path, want_sha), espeak_data_dir=espeak_dir(cache))
    if voice.config.sample_rate != RATE:
        sys.exit(f"{voice_id} speaks at {voice.config.sample_rate} Hz, expected {RATE}")
    syn = SynthesisConfig(length_scale=length_scale or default_scale, normalize_audio=True)

    out = OUT / name
    out.mkdir(parents=True, exist_ok=True)
    print(f"\n{name}: {voice_id}")
    total = 0
    for clip, text in CLIPS:
        audio = np.concatenate([c.audio_float_array for c in voice.synthesize(text, syn)]).astype(np.float64)
        pcm = shape(audio)
        f = out / f"{clip}.wav"
        with wave.open(str(f), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(RATE)
            w.writeframes(pcm.tobytes())
        size = f.stat().st_size
        total += size
        # The head level is the "gss" check: the clip has to open on silence, not mid-vowel.
        head = int(np.max(np.abs(pcm[: int(RATE * 0.02)])))
        print(f"  {clip:12s} {len(pcm) / RATE:5.2f} s {size:7d} B  head {head:5d}  {text}")
    print(f"  {len(CLIPS)} clips, {total} bytes")
    return total


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cache", type=Path,
                    default=Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / "frostmod-voice")
    ap.add_argument("--voice", choices=sorted(VOICES), action="append",
                    help="only this voice; repeatable. Default: all of them.")
    ap.add_argument("--length-scale", type=float, default=0.0, help="under 1 speaks faster; 0 = the voice's own")
    args = ap.parse_args()

    total = 0
    for name in args.voice or sorted(VOICES):
        total += generate(name, args.cache, args.length_scale)
    print(f"\n{total} bytes in all")


if __name__ == "__main__":
    main()
