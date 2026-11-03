#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""What vail-cli prints from audio that is not Morse, and what it keeps of audio that is (docs/engine-confidence.md).

  python3 tools/notmorse.py [--cli PATH] [--seeds N] [--seconds N] [--show]

Every input is decoded twice, with --all and by default, and the symbols printed are compared. Word spaces do not count, and
a prosign counts as its two letters. Everything is synthetic, speech and music most of all: real recordings go further, and
any given with --wav FILE... are measured too. A change to the engine's confidence should move the first table a long way
and the second not at all."""
import argparse
import array
import math
import os
import random
import re
import subprocess
import sys
import tempfile
import wave
from collections import Counter
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RATE = 8000

REAL_TEXT = {
    "qso": "CQ CQ CQ DE K1ABC K1ABC K1ABC PSE K GM OM UR RST 599 599 NAME IS JOHN HW CPY K DE W1AW W1AW SK",
    "contest": "TEST W1AW W1AW TEST W1AW 5NN 123 TU TEST W1AW 5NN 124 TU QRZ TEST W1AW W1AW",
    "ragchew": "VVV VVV DE DL2XYZ DL2XYZ QTH MUNICH MUNICH RIG IS FT991 ANT DIPOLE WX SUNNY TEMP 12C",
    "numbers": "5NN 14 5NN 14 TU 599 25 CA 5NN 05 NY DE VK3ABC QRZ 599 001 001 TU",
    "french": "ABT 73 ES GL DE F5ABC F5ABC SK BONJOUR CHER AMI MERCI POUR LE CONTACT NOM PIERRE PIERRE",
    "groups": "XQZ7 JW4K 8ZR2 PV9D QT3F HX6B LK2M WC5Y 0GN1 UI7S RA4E TO8V BZ3P NM6H",
}
REAL_SHORT = {"tu": "TU", "5nn": "5NN TU", "r73": "R 73", "test": "TEST", "ten": "TEN NET ANT TAN", "itis": "IT IS A TEN"}
# wpm, SNR in 2500 Hz, cwgen options
CONDITIONS = ((25, 20, ()), (18, 6, ()), (32, 0, ()), (22, -6, ()), (22, -10, ()), (15, -12, ()),
              (22, 3, ("-R", "0.5")), (18, 12, ("-j", "0.15", "-W", "3.5", "-G", "1.4")), (28, 6, ("-Q", "2")))


def write_wav(path, x, peak=None):
    top = peak or max(1e-9, max(abs(v) for v in x))
    scale = 26000.0 / top
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(array.array("h", [max(-32768, min(32767, int(v * scale))) for v in x]).tobytes())


def biquad(x, f, kind):
    w = 2 * math.pi * f / RATE
    alpha = math.sin(w) / (2 * 0.707)
    c = math.cos(w)
    b0, b1, b2 = ((1 - c) / 2, 1 - c, (1 - c) / 2) if kind == "lp" else ((1 + c) / 2, -(1 + c), (1 + c) / 2)
    a0, a1, a2 = 1 + alpha, -2 * c, 1 - alpha
    b0, b1, b2, a1, a2 = b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0
    out, x1, x2, y1, y2 = [], 0.0, 0.0, 0.0, 0.0
    for v in x:
        y = b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        x2, x1, y2, y1 = x1, v, y1, y
        out.append(y)
    return out


def ssb_filter(x):
    for _ in range(2):
        x = biquad(biquad(x, 2700, "lp"), 300, "hp")
    return x


def speech(n, rng, f0):
    out, phase, amps, on = [0.0] * n, 0.0, [], False
    for i in range(n):
        t = i / RATE
        pitch = f0 * (1 + 0.15 * math.sin(2 * math.pi * 0.7 * t) + 0.05 * math.sin(2 * math.pi * 3.1 * t))
        phase += 2 * math.pi * pitch / RATE
        if i % 80 == 0:
            f = ((600 + 250 * math.sin(2 * math.pi * 2.3 * t), 120), (1500 + 500 * math.sin(2 * math.pi * 1.7 * t), 180),
                 (2600 + 300 * math.sin(2 * math.pi * 1.1 * t), 250))
            amps = [sum(math.exp(-((k * pitch - c) / b) ** 2) for c, b in f) / k ** 0.5 for k in range(1, 26)]
            on = (math.sin(2 * math.pi * 3.5 * t + 2 * math.sin(2 * math.pi * 0.4 * t)) > -0.2
                  and math.sin(2 * math.pi * 0.17 * t + 1) > -0.6)
        if on:
            out[i] = sum(a * math.sin((k + 1) * phase) for k, a in enumerate(amps) if a > 0.01)
    return out


def music(n, rng):
    out, scale, note = [0.0] * n, (262, 294, 330, 349, 392, 440, 494, 523, 587, 659, 784), int(RATE * 0.35)
    for s in range(0, n, note):
        tones = [rng.choice(scale) for _ in range(3)]
        for i in range(s, min(n, s + note)):
            t = (i - s) / RATE
            env = math.exp(-t * 5)
            out[i] = env * sum(a * math.sin(2 * math.pi * f * h * t) for f in tones for h, a in ((1, 1), (2, .5), (3, .25)))
    return out


def rtty(n, rng, f, shift=170, baud=45.45):
    out, phase, bit, length = [], 0.0, 0, int(RATE / baud)
    for i in range(n):
        if i % length == 0:
            bit = rng.getrandbits(1)
        phase += 2 * math.pi * (f + shift * bit) / RATE
        out.append(math.sin(phase))
    return out


def on_off(n, rng, f, rate):
    out, bit, length = [], 0, int(RATE / rate)
    for i in range(n):
        if i % length == 0:
            bit = rng.getrandbits(1)
        out.append(bit * math.sin(2 * math.pi * f * i / RATE))
    return out


def pumped_noise(n, rng):
    x = ssb_filter([rng.gauss(0, 1) for _ in range(n)])
    return [v * (1 + .8 * math.sin(2 * math.pi * .3 * i / RATE)) * (1 + .5 * math.sin(2 * math.pi * 2.3 * i / RATE))
            for i, v in enumerate(x)]


def with_noise(x, rng, ratio):
    return [v + rng.gauss(0, ratio) for v in x]


def not_morse_audio(kind, seconds, seed):
    n, rng = int(seconds * RATE), random.Random(seed)
    if kind == "speech":
        return with_noise(speech(n, rng, 100 + 40 * (seed % 3)), rng, 0.02)
    if kind == "ssb-speech":
        return with_noise(ssb_filter(speech(n, rng, 160 + 30 * (seed % 3))), rng, 0.05)
    if kind == "music":
        return with_noise(music(n, rng), rng, 0.01)
    if kind == "rtty":
        return with_noise(rtty(n, rng, 1000 + 300 * (seed % 3)), rng, 0.1)
    if kind == "on-off data":
        return with_noise(on_off(n, rng, 900 + 200 * (seed % 3), 6 + 3 * (seed % 3)), rng, 0.1)
    return pumped_noise(n, rng)


NOT_MORSE = ("speech", "ssb-speech", "music", "rtty", "on-off data", "pumped noise")


def decode(cli, path, *args):
    r = subprocess.run([cli, *args, path], capture_output=True, text=True)
    return [m[1] for m in (re.match(r"^\S+ +\d+  (.*)$", line) for line in r.stdout.splitlines()) if m]


def symbols(lines):
    return sum(len(re.sub(r"[ <>]", "", t)) for t in lines)


def both(cli, path):
    return decode(cli, path, "--all"), decode(cli, path)


def measure_not_morse(args, seed, kind, tmp):
    path = os.path.join(tmp, "n%d_%s.wav" % (seed, kind.replace(" ", "")))
    write_wav(path, not_morse_audio(kind, args.seconds, seed))
    every, shown = both(args.cli, path)
    return kind, every, shown


def measure_real(args, seed, group, name, text, cond, tmp):
    wpm, snr, gen = cond
    path = os.path.join(tmp, "r%d_%s_%d_%d_%d.wav" % (seed, name, wpm, snr, len(gen)))
    hz = 500 + 7 * ((seed * 131 + wpm * 37 + abs(snr) * 11 + len(name) * 53) % 400)
    subprocess.run([args.cwgen, "-o", path, "-t", text, "-w", str(wpm), "-f", str(hz), "-n", str(snr),
                    "-r", str(seed * 1000 + wpm), "-l", "1", *gen], check=True)
    every, shown = both(args.cli, path)
    return group, name, cond, text, every, shown


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cli", default=os.path.join(ROOT, "cli", "build", "vail-cli"))
    ap.add_argument("--cwgen", default=os.path.join(ROOT, "cli", "build", "cwgen"))
    ap.add_argument("--seeds", type=int, default=3)
    ap.add_argument("--seconds", type=int, default=60)
    ap.add_argument("--show", action="store_true", help="also print what was lost, and what got through")
    ap.add_argument("--wav", nargs="*", default=[], help="real recordings to measure as well")
    args = ap.parse_args()
    with tempfile.TemporaryDirectory() as tmp, ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        bad = [pool.submit(measure_not_morse, args, seed, kind, tmp) for seed in range(args.seeds) for kind in NOT_MORSE]
        good = [pool.submit(measure_real, args, seed, group, name, text, cond, tmp)
                for seed in range(args.seeds) for group, texts in (("long", REAL_TEXT), ("short", REAL_SHORT))
                for name, text in texts.items() for cond in CONDITIONS]
        rec = [(p, pool.submit(both, args.cli, p)) for p in args.wav]
        bad = [f.result() for f in bad]
        good = [f.result() for f in good]
        rec = [(p, f.result()) for p, f in rec]

    print("Not Morse: lines and symbols printed, with --all -> by default")
    totals = Counter()
    for kind, every, shown in bad:
        totals[kind, "la"] += len(every)
        totals[kind, "ld"] += len(shown)
        totals[kind, "sa"] += symbols(every)
        totals[kind, "sd"] += symbols(shown)
    for kind in NOT_MORSE:
        print("  %-14s lines %5d -> %5d    symbols %6d -> %6d  (%3.0f %% shown)" % (
            kind, totals[kind, "la"], totals[kind, "ld"], totals[kind, "sa"], totals[kind, "sd"],
            100 * totals[kind, "sd"] / max(1, totals[kind, "sa"])))

    print("\nReal CW: symbols printed, with --all -> by default")
    real = Counter()
    for group, name, cond, text, every, shown in good:
        real[group, "cases"] += 1
        real[group, "sa"] += symbols(every)
        real[group, "sd"] += symbols(shown)
    for group in ("long", "short"):
        print("  %-6s %4d cases    symbols %6d -> %6d  (%.2f %% lost)" % (
            group, real[group, "cases"], real[group, "sa"], real[group, "sd"],
            100 * (real[group, "sa"] - real[group, "sd"]) / max(1, real[group, "sa"])))

    if rec:
        print("\nRecordings: lines and symbols printed, with --all -> by default")
        for path, (every, shown) in rec:
            print("  %-40s lines %5d -> %5d    symbols %6d -> %6d" % (os.path.basename(path), len(every), len(shown),
                                                                      symbols(every), symbols(shown)))
    if args.show:
        print("\nReal CW that lost text:")
        for group, name, cond, text, every, shown in good:
            if symbols(every) != symbols(shown):
                print("  %-5s %-8s snr %+3d %-5s all: %-60s default: %s" % (group, name, cond[1], "".join(cond[2][:1]),
                      " | ".join(every)[:60], " | ".join(shown)[:40]))
        print("\nWhat got through, the first of each:")
        seen = set()
        for kind, every, shown in bad:
            if shown and kind not in seen:
                seen.add(kind)
                print("  %-14s %s" % (kind, " | ".join(shown)[:110]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
