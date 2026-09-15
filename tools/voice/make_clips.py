#!/usr/bin/env python3
"""Generates mxbcoach.dlo's spoken cue clips into src/voice/.

One clip per cue kind (the list in src/coachvoice.h), spoken by Piper, an offline neural
text-to-speech engine, with the "en_US-ljspeech-high" voice: trained from scratch on the LJ
Speech dataset, which is public domain; the piper-voices model repository is MIT. See NOTICE
and docs/PLUGIN.md.

Each clip is 22.05 kHz 16-bit mono PCM, trimmed of silence, faded at both ends and
normalised to -1 dBFS peak. The DLL embeds them as resources and scales them by the rider's
volume; winmm plays PCM only, so they stay WAV.

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

VOICE = "en_US-ljspeech-high"
VOICE_URL = "https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/ljspeech/high/"
VOICE_SHA256 = "5d4f08ba6a2a48c44592eed3ce56bf85e9de3dd4e20df90541ae68a8310c029a"

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
GATE_DB = -40.0         # quieter than this, relative to the peak, is silence
PRE_S, POST_S = 0.015, 0.04
FADE_S = 0.008


def fetch_voice(cache: Path) -> Path:
    cache.mkdir(parents=True, exist_ok=True)
    onnx, cfg = cache / f"{VOICE}.onnx", cache / f"{VOICE}.onnx.json"
    for path in (onnx, cfg):
        if not path.exists():
            print(f"downloading {path.name}")
            urllib.request.urlretrieve(VOICE_URL + path.name, path)
    digest = hashlib.sha256(onnx.read_bytes()).hexdigest()
    if digest != VOICE_SHA256:
        sys.exit(f"{onnx} has sha256 {digest}, expected {VOICE_SHA256}")
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
    loud = np.nonzero(20 * np.log10(rms / peak) > GATE_DB)[0]
    if len(loud):
        start = max(0, loud[0] * win - int(RATE * PRE_S))
        end = min(len(audio), (loud[-1] + 1) * win + int(RATE * POST_S))
        audio = audio[start:end]
    fade = int(RATE * FADE_S)
    ramp = np.linspace(0.0, 1.0, fade)
    audio = audio.copy()
    audio[:fade] *= ramp
    audio[-fade:] *= ramp[::-1]
    audio *= PEAK / (float(np.max(np.abs(audio))) or 1.0)
    return np.clip(np.round(audio * 32767), -32768, 32767).astype("<i2")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cache", type=Path, default=Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / "frostmod-voice")
    ap.add_argument("--length-scale", type=float, default=0.9, help="under 1 speaks faster")
    args = ap.parse_args()

    voice = PiperVoice.load(fetch_voice(args.cache), espeak_data_dir=espeak_dir(args.cache))
    if voice.config.sample_rate != RATE:
        sys.exit(f"{VOICE} speaks at {voice.config.sample_rate} Hz, expected {RATE}")
    syn = SynthesisConfig(length_scale=args.length_scale, normalize_audio=True)

    OUT.mkdir(parents=True, exist_ok=True)
    total = 0
    for name, text in CLIPS:
        audio = np.concatenate([c.audio_float_array for c in voice.synthesize(text, syn)]).astype(np.float64)
        pcm = shape(audio)
        path = OUT / f"{name}.wav"
        with wave.open(str(path), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(RATE)
            w.writeframes(pcm.tobytes())
        size = path.stat().st_size
        total += size
        print(f"{name:12s} {len(pcm) / RATE:5.2f} s {size:7d} B  {text}")
    print(f"{len(CLIPS)} clips, {total} bytes")


if __name__ == "__main__":
    main()
