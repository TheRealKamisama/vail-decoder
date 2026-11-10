#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Band-like audio with no Morse in it, for counting false text: 180 s each, 8 kHz, 16-bit mono.

  tools/bandnoise.py OUTDIR

Writes white.wav (white noise), ssbfilter.wav (noise through a 300-2700 Hz receiver filter), pink_agc.wav (pink noise
whose level pumps by 4 dB, as an AGC does), qrn.wav (static crashes, 3 per second, on band noise), carriers_qsb.wav
(three unkeyed carriers fading by 8 dB), voice.wav (a voiced, syllabic SSB-like signal) and rtty.wav (45.45 Bd FSK,
170 Hz shift). The seed is fixed, so the files are the same each time. Needs numpy."""
import os
import sys
import wave

import numpy as np

RATE = 8000
SECONDS = 180
N = RATE * SECONDS


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    rng = np.random.default_rng(7)

    def save(name, x):
        x = x / np.max(np.abs(x)) * 0.3 * 32767
        with wave.open(os.path.join(out, name + ".wav"), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(RATE)
            w.writeframes(x.astype(np.int16).tobytes())

    def bandpass(x, lo, hi):
        f = np.fft.rfftfreq(len(x), 1 / RATE)
        g = 1 / np.sqrt(1 + (lo / np.maximum(f, 1)) ** 8) / np.sqrt(1 + (f / hi) ** 8)
        return np.fft.irfft(np.fft.rfft(x) * g, len(x))

    def slow(hz, depth_db):
        """a slowly wandering gain, depth_db per standard deviation"""
        k = int(SECONDS * hz) + 2
        v = np.interp(np.linspace(0, k - 1, N), np.arange(k), rng.normal(0, 1, k))
        return 10 ** (depth_db * v / 20)

    save("white", rng.normal(0, 1, N))
    save("ssbfilter", bandpass(rng.normal(0, 1, N), 300, 2700))
    pink = np.fft.irfft(np.fft.rfft(rng.normal(0, 1, N)) / np.sqrt(np.maximum(np.fft.rfftfreq(N, 1 / RATE), 20)), N)
    save("pink_agc", bandpass(pink, 300, 2700) * slow(0.5, 4))
    band = bandpass(rng.normal(0, 1, N), 300, 2700)
    imp = np.zeros(N)
    idx = rng.integers(0, N, SECONDS * 3)
    imp[idx] = rng.pareto(1.5, len(idx)) * 20
    crash = np.convolve(imp, np.exp(-np.arange(400) / 60), mode="same") * rng.normal(0, 1, N)
    save("qrn", band + bandpass(crash, 300, 2700))
    t = np.arange(N) / RATE
    c = np.zeros(N)
    for hz in (650, 820, 1100):
        c += 0.08 * slow(0.3, 8) * np.sin(2 * np.pi * (hz + 3 * np.sin(2 * np.pi * 0.01 * t)) * t)
    save("carriers_qsb", bandpass(rng.normal(0, 1, N), 300, 2700) + c)
    phase = np.cumsum(2 * np.pi * (120 + 30 * slow(2, 3)) / RATE)
    v = np.zeros(N)
    for k in range(1, 25):
        v += np.sin(k * phase) / k * np.exp(-((k * 120 - 700) / 600) ** 2)
    syllables = np.clip(np.sin(2 * np.pi * 4 * t + np.cumsum(rng.normal(0, .02, N))), 0, None) * (slow(0.3, 6) > 0.8)
    save("voice", bandpass(v * syllables, 300, 2700) * 0.5 + bandpass(rng.normal(0, 1, N), 300, 2700) * 0.3)
    bits = rng.integers(0, 2, int(SECONDS * 45.45) + 1)
    fsk = np.where(np.repeat(bits, int(RATE / 45.45) + 1)[:N], 1070, 900)
    save("rtty", 0.4 * np.sin(np.cumsum(2 * np.pi * fsk / RATE)) + bandpass(rng.normal(0, 1, N), 300, 2700))


if __name__ == "__main__":
    main()
