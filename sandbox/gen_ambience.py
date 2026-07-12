#!/usr/bin/env python3
"""Generate the game's background ambience: the floating-bowls field
recording laid over a pink noise bed, both loop-softened.

    python3 gen_ambience.py [path/to/recording.mp3]

Writes ../src/resources/ambience.wav (mono 16-bit, 22050 Hz).

The recording has a hard cut when looped; both it and the pink noise get an
equal-power crossfade of the tail into the head, so the seam disappears.
Pink noise (1/f, 3 dB/octave falloff) fills the low end and masks whatever
seam residue survives. The mp3 is decoded with gst-launch (no ffmpeg here).
"""

import subprocess
import sys
import tempfile
import wave
from pathlib import Path

import numpy as np

SAMPLE_RATE = 22050
CROSSFADE = 1.5    # seconds blended across the loop seam
BOWLS_GAIN = 0.85  # the recording carries the ambience...
PINK_GAIN = 0.22   # ...the pink bed sits underneath
OUT = Path(__file__).parent.parent / "src" / "resources" / "ambience.wav"
DEFAULT_MP3 = Path.home() / "Documents" / "Floating bowls - Contemporary art exhibition place bellecour Lyon.mp3"


def decode_mp3(path):
    """mp3 -> mono float array at SAMPLE_RATE, via gstreamer."""
    with tempfile.NamedTemporaryFile(suffix=".wav") as tmp:
        subprocess.run(
            ["gst-launch-1.0", "-q", "filesrc", f"location={path}", "!",
             "decodebin", "!", "audioconvert", "!", "audioresample", "!",
             f"audio/x-raw,rate={SAMPLE_RATE},channels=1,format=S16LE", "!",
             "wavenc", "!", "filesink", f"location={tmp.name}"],
            check=True)
        with wave.open(tmp.name) as w:
            raw = w.readframes(w.getnframes())
    return np.frombuffer(raw, dtype=np.int16).astype(np.float64) / 32768.0


def loop_soften(x, fade_samples):
    """Equal-power crossfade of the tail into the head; the returned array
    is fade_samples shorter and loops without a seam."""
    body = x[:-fade_samples].copy()
    t = np.linspace(0.0, 1.0, fade_samples)
    body[:fade_samples] = x[-fade_samples:] * np.cos(t * np.pi / 2) + body[:fade_samples] * np.sin(t * np.pi / 2)
    return body


def pink_noise(samples, seed=1):
    """White noise shaped 1/f in the frequency domain."""
    rng = np.random.default_rng(seed)
    white = rng.standard_normal(samples)
    spectrum = np.fft.rfft(white)
    freqs = np.fft.rfftfreq(samples, 1.0 / SAMPLE_RATE)
    freqs[0] = freqs[1]  # keep DC finite
    spectrum /= np.sqrt(freqs)
    pink = np.fft.irfft(spectrum, samples)
    return pink / np.max(np.abs(pink))


def main():
    mp3 = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_MP3
    fade = int(SAMPLE_RATE * CROSSFADE)

    bowls = decode_mp3(mp3)
    bowls /= np.max(np.abs(bowls))
    bowls = loop_soften(bowls, fade)  # the recording's hard loop, softened

    pink = loop_soften(pink_noise(len(bowls) + fade), fade)

    mix = bowls * BOWLS_GAIN + pink * PINK_GAIN
    mix *= 0.9 / np.max(np.abs(mix))
    samples = (mix * 32767.0).astype(np.int16)

    with wave.open(str(OUT), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(samples.tobytes())

    print(f"wrote {OUT} ({len(samples) / SAMPLE_RATE:.1f}s, {OUT.stat().st_size // 1024} KB)")


if __name__ == "__main__":
    main()
