#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""End-to-end tests of vail-cli: known stations go in as audio, and what comes out is scored.

    python3 tests/e2e.py              run the checks; exit 1 if any fail
    python3 tests/e2e.py --table      also print every case's accuracy and ghost text
    python3 tests/e2e.py --filter S   only cases whose name contains S
    VAIL_CLI=path                     test another binary

Text is scored per station: a line belongs to the nearest station within MATCH_HZ, and anything else
is a ghost. CER is the edit distance over the length of what the station sent, word spacing aside.
"""
import array
import argparse
import json
import math
import os
import random
import re
import select
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import time
import wave
import warnings
from concurrent.futures import ThreadPoolExecutor

try:
    import fcntl
    import pty
    import termios
    TERMINAL = True
except ImportError:                   # not on Windows, whose console cannot be driven from a test like this
    TERMINAL = False

# The terminal tests fork with a feeding thread alive; the child only calls exec.
warnings.filterwarnings("ignore", category=DeprecationWarning, message=".*fork.*")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CLI = os.environ.get("VAIL_CLI", os.path.join(ROOT, "build", "vail-cli"))
CWGEN = os.path.join(ROOT, "build", "cwgen")
RATE = 8000
MATCH_HZ = 40
NOISE_DBFS = -26.0

QSO = ["CQ CQ CQ DE K1ABC K1ABC K1ABC PSE K", "TEST W1AW W1AW TEST W1AW W1AW TEST",
       "VVV VVV DE DL2XYZ DL2XYZ QTH MUNICH K", "CQ DX CQ DX DE JA1ZZZ JA1ZZZ UP 5 K",
       "R R TNX FER CALL UR RST 599 599 BK", "GM OM NAME IS JOHN JOHN HW CPY K",
       "QRZ QRZ DE G3XYZ G3XYZ K", "5NN TU DE VK3ABC VK3ABC QRZ",
       "ABT 73 ES GL DE F5ABC F5ABC SK", "CQ POTA CQ POTA DE N7XX N7XX K"]


def read_wav(path):
    with wave.open(path) as w:
        a = array.array("h")
        a.frombytes(w.readframes(w.getnframes()))
    return a


def write_wav(path, samples, rate=RATE, channels=1):
    with wave.open(path, "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(array.array("h", samples).tobytes())


def clamp(x):
    return max(-32768, min(32767, int(round(x))))


def station(tmp, name, hz, wpm, text, seed, args=()):
    """A noiseless station at unit-ish amplitude, normalised to full-scale peak = 1."""
    path = os.path.join(tmp, name + ".wav")
    subprocess.run([CWGEN, "-o", path, "-t", text, "-w", str(wpm), "-f", str(hz), "-r", str(seed), *args], check=True)
    a = read_wav(path)
    peak = max(1, max(abs(x) for x in a))
    return [x / peak for x in a]


def mix(tmp, name, stations, snr_db, seed=1, band=None, seconds=None, drive=None):
    """Mixes stations; snr_db is signal over noise power in 2500 Hz, per station (None: no noise)."""
    n = max(len(s["audio"]) + s.get("start", 0) for s in stations) if stations else 0
    n = max(n, int((seconds or 0) * RATE))
    out = [0.0] * n
    rng = random.Random(seed)
    sigma = 32768 * 10 ** (NOISE_DBFS / 20)
    noise_2500 = sigma ** 2 * 2500 / (RATE / 2)
    for s in stations:
        snr = s.get("snr", snr_db)
        if snr is None:
            amp = s.get("amp", 8000.0)
        else:
            amp = math.sqrt(2 * noise_2500 * 10 ** (snr / 10))
        o = s.get("start", 0)
        for i, v in enumerate(s["audio"]):
            out[o + i] += amp * v
    if snr_db is not None:
        if band:                      # a receiver's filter: only LO..HI Hz of the noise gets through
            noise = bandpassed_noise(n, sigma, band, rng)
        else:
            noise = [rng.gauss(0, sigma) for _ in range(n)]
        out = [a + b for a, b in zip(out, noise)]
    peak = max((abs(x) for x in out), default=0.0)
    if drive:                         # an input level set too high: the sound card clips
        out = [x * drive * 32767 / peak for x in out]
    elif peak > 26000:                # otherwise leave headroom, as a sensible input level would
        out = [x * 26000 / peak for x in out]
    path = os.path.join(tmp, name + ".wav")
    write_wav(path, [clamp(x) for x in out])
    return path


def bandpassed_noise(n, sigma, band, rng):
    lo, hi = band
    wn = [rng.gauss(0, sigma) for _ in range(n + 200)]
    def biquad(x, f, kind):
        w = 2 * math.pi * f / RATE
        alpha = math.sin(w) / (2 * 0.707)
        c = math.cos(w)
        if kind == "lp":
            b0, b1, b2 = (1 - c) / 2, 1 - c, (1 - c) / 2
        else:
            b0, b1, b2 = (1 + c) / 2, -(1 + c), (1 + c) / 2
        a0, a1, a2 = 1 + alpha, -2 * c, 1 - alpha
        b0, b1, b2, a1, a2 = b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0
        y, x1, x2, y1, y2 = [], 0.0, 0.0, 0.0, 0.0
        for v in x:
            o = b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
            x2, x1, y2, y1 = x1, v, y1, o
            y.append(o)
        return y
    for _ in range(4):                # 8 poles each side, a steep crystal-like skirt
        wn = biquad(wn, hi, "lp")
        wn = biquad(wn, lo, "hp")
    return wn[200:]


def decode(path, *args):
    r = subprocess.run([CLI, *args, path], capture_output=True, text=True)
    lines = []
    for line in r.stdout.splitlines():
        m = re.match(r"^(\d\d):(\d\d):(\d\d) +(\d+)  (.*)$", line)
        if not m:
            raise AssertionError("malformed output line: %r" % line)
        lines.append((int(m[1]) * 3600 + int(m[2]) * 60 + int(m[3]), int(m[4]), m[5]))
    return r.returncode, lines, r.stderr


def edit_distance(a, b):
    prev = list(range(len(b) + 1))
    for i, x in enumerate(a, 1):
        cur = [i]
        for j, y in enumerate(b, 1):
            cur.append(min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (x != y)))
        prev = cur
    return prev[-1]


def letters(s):
    """What is scored: word spacing is a judgement call of the decoder, the letters are not."""
    return "".join(s.replace("<", "").replace(">", "").split())


def score(lines, stations):
    """Returns (cer per station, ghost characters, ghost lines, per-station text)."""
    got = {s["hz"]: [] for s in stations}
    ghosts = []
    for t, hz, text in lines:
        best = min(stations, key=lambda s: abs(s.get("heard_hz", s["hz"]) - hz), default=None)
        if best is not None and abs(best.get("heard_hz", best["hz"]) - hz) <= MATCH_HZ:
            got[best["hz"]].append(text)
        else:
            ghosts.append((t, hz, text))
    cers, texts = {}, {}
    for s in stations:
        heard = " ".join(got[s["hz"]])
        sent = letters(s["text"])
        cers[s["hz"]] = edit_distance(letters(heard), sent) / max(1, len(sent))
        texts[s["hz"]] = heard
    return cers, sum(len(g[2].replace(" ", "")) for g in ghosts), ghosts, texts


# ------------------------------------------------------------------------------------- cases

def make_stations(tmp, spec):
    out = []
    for i, s in enumerate(spec):
        s = dict(s)
        s.setdefault("wpm", 25)
        s.setdefault("text", QSO[i % len(QSO)])
        s["audio"] = station(tmp, "st%d_%d" % (i, s["hz"]), s["hz"], s["wpm"], s["text"], 100 + i, s.get("gen", ()))
        s["start"] = int(s.pop("delay", 0.5) * RATE)
        out.append(s)
    return out


WPMS = [25, 18, 30, 22, 28, 20, 24, 26, 16, 32]
SPREAD = [{"hz": 400 + i * 350, "wpm": WPMS[i]} for i in range(10)]
FOUR = [{"hz": 500, "wpm": 25}, {"hz": 1400, "wpm": 30}, {"hz": 2300, "wpm": 18}, {"hz": 3300, "wpm": 22}]

# name: (station spec, snr, mix options, limits: max CER of any station, max ghost characters)
# The limits marked ENGINE are what the engine itself achieves today, kept as regression guards.
CASES = {
    "one-clean":       ([{"hz": 700}], None, {}, (0.02, 0)),
    "one-strong":      ([{"hz": 700}], 35, {}, (0.03, 0)),
    "one-medium":      ([{"hz": 700}], 10, {}, (0.03, 0)),
    "one-weak":        ([{"hz": 700}], -3, {}, (0.15, 0)),
    "high-3400":       ([{"hz": 3400}], 10, {}, (0.03, 0)),
    "low-250":         ([{"hz": 250}], 10, {}, (0.05, 0)),
    "four-clean":      (FOUR, None, {}, (0.03, 0)),
    "four-strong":     (FOUR, 30, {}, (0.05, 0)),
    "four-medium":     (FOUR, 10, {}, (0.05, 0)),
    "four-weak":       (FOUR, -3, {}, (0.20, 0)),
    "ten-strong":      (SPREAD, 25, {}, (0.08, 0)),
    "ten-medium":      (SPREAD, 8, {}, (0.08, 0)),
    "pair-300hz":      ([{"hz": 800, "wpm": 25}, {"hz": 1100, "wpm": 18}], 15, {}, (0.03, 0)),
    "pair-100hz":      ([{"hz": 800, "wpm": 25}, {"hz": 900, "wpm": 18}], 15, {}, (0.03, 0)),
    "pair-60hz":       ([{"hz": 800, "wpm": 25}, {"hz": 860, "wpm": 18}], 15, {}, (0.03, 0)),
    "strong-by-weak":  ([{"hz": 800, "wpm": 25, "snr": 30}, {"hz": 950, "wpm": 18, "snr": 8}], 8, {}, (0.03, 0)),
    "weak-beside":     ([{"hz": 800, "wpm": 25, "snr": 30}, {"hz": 900, "wpm": 18, "snr": 15}], 15, {}, (0.03, 0)),
    "digits-beside":   ([{"hz": 800, "wpm": 25, "text": "1 1 1 11 111 0 0 0 00 000"},       # runs of dashes
                         {"hz": 900, "wpm": 18, "text": "11111 22222 00000 99999"}], 15, {}, (0.03, 0)),
    "three-100hz":     ([{"hz": 700, "wpm": 18}, {"hz": 800, "wpm": 25}, {"hz": 900, "wpm": 22}], 10, {}, (0.03, 0)),
    "clipped":        ([{"hz": 700}], 20, {"drive": 4}, (0.05, 0)),
    "weak-far-strong": ([{"hz": 700, "wpm": 25, "snr": 30}, {"hz": 2500, "wpm": 20, "snr": 3}], 3, {}, (0.10, 0)),
    "weak-under-strong": ([{"hz": 600, "wpm": 28, "snr": 35}, {"hz": 1100, "wpm": 18, "snr": 6},
                           {"hz": 1900, "wpm": 22, "snr": 6}, {"hz": 2700, "wpm": 25, "snr": 6}], 6, {}, (0.10, 0)),
    "same-speed":      ([{"hz": 600, "wpm": 25, "snr": 20}, {"hz": 1000, "wpm": 25, "snr": 10},
                         {"hz": 1400, "wpm": 25, "snr": 10}], 10, {}, (0.08, 0)),
    "fading":          ([{"hz": 1200, "gen": ("-R", "0.5")}], 12, {}, (0.45, 0)),   # ENGINE: same CER as it alone
    "short":           ([{"hz": 1200, "text": "CQ DE K1ABC K"}], 15, {}, (0.05, 0)),
    "drift":           ([{"hz": 880, "heard_hz": 945, "wpm": 22, "gen": ("-d", "70")}], 15, {}, (0.08, 0)),   # through an overlap
    "hand-keyed":      ([{"hz": 1500, "wpm": 18, "gen": ("-j", "0.15", "-W", "3.5", "-G", "1.4")}], 15, {}, (0.10, 0)),
    "static":          ([{"hz": 1000, "gen": ("-Q", "2")}, {"hz": 2200, "wpm": 20, "gen": ("-Q", "2")}], 15, {}, (0.10, 0)),
    "overlap-919":     ([{"hz": 919}], 12, {}, (0.03, 0)),     # the overlap of the first two slices
    "overlap-1638":    ([{"hz": 1638}], 12, {}, (0.03, 0)),    # of the next two
    "overlap-2357":    ([{"hz": 2357}], 12, {}, (0.03, 0)),
    "overlap-3076":    ([{"hz": 3076}], 12, {}, (0.03, 0)),
    "boundary-pair":   ([{"hz": 880, "wpm": 25}, {"hz": 960, "wpm": 20}], 15, {}, (0.03, 0)),
    "filter-ssb":      (FOUR[:3], 10, {"band": (300, 2700)}, (0.05, 0)),
    "noise-white":     ([], 0, {"seconds": 120}, (0, 0)),
    "noise-ssb":       ([], 0, {"seconds": 120, "band": (300, 2700)}, (0, 0)),
    "noise-cw-filter": ([], 0, {"seconds": 120, "band": (450, 950)}, (0, 0)),
    "silence":         ([], None, {"seconds": 20}, (0, 0)),
}


def run_case(name, table_rows):
    spec, snr, opts, (max_cer, max_ghost) = CASES[name]
    with tempfile.TemporaryDirectory() as tmp:
        stations = make_stations(tmp, spec)
        path = mix(tmp, name, stations, snr, **opts)
        rc, lines, err = decode(path)
    cers, ghost_chars, ghosts, texts = score(lines, stations)
    worst = max(cers.values(), default=0.0)
    table_rows[name] = (worst, ghost_chars, cers, ghosts, texts)
    problems = []
    if rc != 0:
        problems.append("exit status %d: %s" % (rc, err.strip()))
    if worst > max_cer:
        problems.append("worst CER %.1f%% > %.1f%%" % (100 * worst, 100 * max_cer))
    if ghost_chars > max_ghost:
        problems.append("%d ghost characters > %d" % (ghost_chars, max_ghost))
    return problems


# ------------------------------------------------------------------------- behaviour checks

def check_long_text_is_whole():
    """More than a message's 500 characters: nothing lost or printed twice when old text is dropped."""
    text = " ".join("MSG%04d DE K1ABC" % i for i in range(70))
    with tempfile.TemporaryDirectory() as tmp:
        st = make_stations(tmp, [{"hz": 700, "wpm": 40, "text": text}])
        rc, lines, err = decode(mix(tmp, "long", st, 15))
    cers, ghost_chars, ghosts, texts = score(lines, st)
    cer = cers[700]
    if rc or cer > 0.02 or ghost_chars:
        return ["CER %.1f%%, %d ghost characters, exit %d" % (100 * cer, ghost_chars, rc)]
    if max(len(l[2]) for l in lines) > 80:
        return ["a line is longer than 80 characters"]
    return []


def check_repeating_text_is_whole():
    """The same when the text repeats, as a beacon's does, so that old and new text look alike."""
    problems = []
    for words, times in (("VVV", 250), ("TEST", 160), ("CQ CQ CQ DE K1ABC K1ABC K1ABC PSE K", 20)):
        text = " ".join([words] * times)
        with tempfile.TemporaryDirectory() as tmp:
            st = make_stations(tmp, [{"hz": 700, "wpm": 40, "text": text}])
            rc, lines, err = decode(mix(tmp, "repeat", st, 15))
        cers, ghost_chars, ghosts, texts = score(lines, st)
        printed = sum(len(letters(l[2])) for l in lines)
        if rc or cers[700] > 0.02 or ghost_chars or printed != len(letters(text)):
            problems.append("%s x %d: %d of %d letters printed, CER %.1f%%, %d ghost characters, exit %d" %
                            (words, times, printed, len(letters(text)), 100 * cers[700], ghost_chars, rc))
    return problems


def check_overs():
    """Two stations take turns on one frequency: both are heard, in order, whether the channel survives
    the gap between them or not."""
    problems = []
    a, b = QSO[0], QSO[6]
    with tempfile.TemporaryDirectory() as tmp:
        for gap in (3, 15):
            st = make_stations(tmp, [{"hz": 700, "wpm": 25, "text": a}, {"hz": 700, "wpm": 18, "text": b}])
            st[1]["start"] = st[0]["start"] + len(st[0]["audio"]) + gap * RATE
            rc, lines, err = decode(mix(tmp, "overs%d" % gap, st, 12))
            heard = letters(" ".join(l[2] for l in sorted(lines, key=lambda l: l[0])))
            sent = letters(a + b)
            cer = edit_distance(heard, sent) / len(sent)
            if rc or cer > 0.10 or any(abs(l[1] - 700) > MATCH_HZ for l in lines):
                problems.append("%d s apart: CER %.0f%%: %r" % (gap, 100 * cer, lines))
    return problems


def check_carrier():
    """A steady carrier prints nothing and does not hide the station beside it."""
    with tempfile.TemporaryDirectory() as tmp:
        st = make_stations(tmp, [{"hz": 1300, "text": QSO[0]}])
        carrier = {"hz": 1000, "text": "", "start": 0,
                   "audio": [math.sin(2 * math.pi * 1000 * i / RATE) for i in range(30 * RATE)]}
        rc, lines, err = decode(mix(tmp, "carrier", st + [carrier], 15))
    cers, ghost_chars, ghosts, texts = score(lines, st + [carrier])
    if rc or cers[1300] > 0.05 or ghost_chars or texts[1000]:
        return ["CER %.0f%%, carrier printed %r, ghosts %r" % (100 * cers[1300], texts[1000], ghosts)]
    return []


def check_formats():
    """A 48 kHz stereo file, and raw samples on stdin, decode like the original."""
    with tempfile.TemporaryDirectory() as tmp:
        st = make_stations(tmp, [{"hz": 1500, "text": QSO[0]}])
        path = mix(tmp, "base", st, 15)
        base = read_wav(path)
        up = []
        for i in range(len(base) - 1):
            for k in range(6):
                v = base[i] + (base[i + 1] - base[i]) * k // 6
                up += [v, v]
        stereo = os.path.join(tmp, "stereo48k.wav")
        write_wav(stereo, up, rate=48000, channels=2)
        problems = []
        rc, lines, err = decode(stereo)
        if rc or score(lines, st)[0][1500] > 0.03:
            problems.append("48 kHz stereo: exit %d: %r" % (rc, lines))
        r = subprocess.run([CLI, "-"], input=base.tobytes(), capture_output=True)
        out = r.stdout.decode()
        if r.returncode or "K1ABC K1ABC" not in out:
            problems.append("stdin: exit %d: %r" % (r.returncode, out))
    return problems


def check_band_option():
    """-b leaves out what is outside the band, and records the input."""
    with tempfile.TemporaryDirectory() as tmp:
        st = make_stations(tmp, FOUR)
        path = mix(tmp, "four", st, 15)
        rec = os.path.join(tmp, "rec.wav")
        rc, lines, err = decode(path, "-b", "1000-2000", "-r", rec)
        problems = []
        if rc or not lines or any(not 1000 <= l[1] <= 2000 for l in lines):
            problems.append("-b 1000-2000 gave %r" % (lines,))
        if rc == 0:
            sent, recorded = read_wav(path), read_wav(rec)
            if recorded[:len(sent)] != sent or not 0 <= len(recorded) - len(sent) < 85:   # the last frame is filled out
                problems.append("-r recorded %d samples of %d, or other samples" % (len(recorded), len(sent)))
    return problems


def check_errors():
    """Bad usage exits 2 and missing things exit 1, with a message on stderr and nothing on stdout."""
    problems = []
    for args, want in ((["--nonsense"], 2), (["-b", "9-1"], 2), (["-d"], 2), (["a", "b"], 2),
                       (["-d", "x", "file"], 2), (["no-such-file.wav"], 1)):
        r = subprocess.run([CLI, *args], capture_output=True, text=True)
        if r.returncode != want or r.stdout or not r.stderr.startswith("vail-cli: "):
            problems.append("%s: exit %d, stdout %r, stderr %r" % (args, r.returncode, r.stdout, r.stderr))
    return problems


def check_write_errors():
    """Output that cannot be written exits 1 and says why. A reader that has gone away only stops the run, with exit 0."""
    problems = []

    def expect(what, args, want, stdout, complaint):
        r = subprocess.run([CLI, *args], stdout=stdout, stderr=subprocess.PIPE, text=True)
        if r.returncode != want or (complaint not in r.stderr if complaint else r.stderr):
            problems.append("%s: exit %d, stderr %r" % (what, r.returncode, r.stderr))
        return r

    with tempfile.TemporaryDirectory() as tmp:
        st = make_stations(tmp, [{"hz": 700, "wpm": 25, "text": QSO[0]}])
        path = mix(tmp, "write", st, 15)
        cannot = "vail-cli: cannot write"
        readonly = os.open(path, os.O_RDONLY)                # writing to this fails on every platform
        expect("text to a read-only stdout", [path], 1, readonly, cannot)
        expect("--version to a read-only stdout", ["--version"], 1, readonly, cannot)
        os.close(readonly)
        reader, writer = os.pipe()
        os.close(reader)
        expect("text with the reader gone", [path], 0, writer, None)
        os.close(writer)
        if os.path.exists("/dev/full"):
            with open("/dev/full", "w") as full:
                expect("text to /dev/full", [path], 1, full, cannot)
                expect("--help to /dev/full", ["--help"], 1, full, cannot)
            r = expect("recording to /dev/full", ["-r", "/dev/full", path], 1, subprocess.PIPE, cannot + " /dev/full")
            if "K1ABC" not in r.stdout:
                problems.append("the text stopped with the recording: %r" % r.stdout)
    return problems


def check_version():
    """--version gives both versions, and they match the man page and the engine's header."""
    r = subprocess.run([CLI, "--version"], capture_output=True, text=True)
    m = re.fullmatch(r"vail-cli (\d+\.\d+\.\d+) \(vail engine (\d+\.\d+\.\d+)\)\nfull-screen view: (yes|no, .*)\n", r.stdout)
    if r.returncode or not m:
        return ["--version: exit %d, %r" % (r.returncode, r.stdout)]
    problems = []
    with open(os.path.join(ROOT, "vail-cli.1"), encoding="utf-8") as f:
        if '"vail-cli %s"' % m.group(1) not in f.read():
            problems.append("the man page does not say vail-cli %s" % m.group(1))
    with open(os.path.join(ROOT, "..", "src", "vail.h"), encoding="utf-8") as f:
        if not re.search(r'#define VAIL_VERSION\s+"%s"' % re.escape(m.group(2)), f.read()):
            problems.append("src/vail.h does not say %s" % m.group(2))
    return problems


def view_is_built():
    return "full-screen view: yes" in subprocess.run([CLI, "--version"], capture_output=True, text=True).stdout


def speech_like(seconds, f0=120.0, seed=1):
    """A voice as a sound card hears it: harmonics through moving formants, in syllables with pauses. Not a real voice."""
    n = int(seconds * RATE)
    rng = random.Random(seed)
    out = [0.0] * n
    phase = 0.0
    block = 80
    amps = []
    for i in range(n):
        t = i / RATE
        pitch = f0 * (1 + 0.15 * math.sin(2 * math.pi * 0.7 * t) + 0.05 * math.sin(2 * math.pi * 3.1 * t))
        phase += 2 * math.pi * pitch / RATE
        if i % block == 0:
            f1 = 600 + 250 * math.sin(2 * math.pi * 2.3 * t)
            f2 = 1500 + 500 * math.sin(2 * math.pi * 1.7 * t)
            f3 = 2600 + 300 * math.sin(2 * math.pi * 1.1 * t)
            amps = [sum(math.exp(-((k * pitch - f) / b) ** 2) for f, b in ((f1, 120), (f2, 180), (f3, 250))) / k ** 0.5
                    for k in range(1, 26)]
            on = (math.sin(2 * math.pi * 3.5 * t + 2 * math.sin(2 * math.pi * 0.4 * t)) > -0.2
                  and math.sin(2 * math.pi * 0.17 * t + 1) > -0.6)
        if on:
            out[i] = sum(a * math.sin((k + 1) * phase) for k, a in enumerate(amps) if a > 0.01)
    peak = max(1e-9, max(abs(x) for x in out))
    return [clamp(14000 * x / peak + rng.gauss(0, 150)) for x in out]


def check_not_morse_is_hidden():
    """Speech decodes as piles of E and T. It is left out and said to be, and --all still shows it."""
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "speech.wav")
        write_wav(path, speech_like(40))
        rc, shown, err = decode(path)
        rc2, everything, err2 = decode(path, "--all")
    count = lambda lines: sum(len(letters(l[2])) for l in lines)
    problems = []
    if rc or rc2:
        problems.append("exit %d and %d" % (rc, rc2))
    if count(everything) < 100:
        problems.append("--all printed only %d letters: nothing to test" % count(everything))
    if count(shown) > 0.15 * count(everything):
        problems.append("%d of %d letters shown: %r" % (count(shown), count(everything), shown[:5]))
    if "--all" not in err:
        problems.append("nothing says text was left out: %r" % err)
    if "--all" in err2:
        problems.append("--all still says text was left out: %r" % err2)
    return problems


def check_real_text_is_not_hidden():
    """What looks least like language, or is mostly the letters speech decodes to, still prints if it is real."""
    problems = []
    for text in ("XQZ7 JW4K 8ZR2 PV9D QT3F HX6B LK2M WC5Y 0GN1 UI7S",              # code groups
                 "1 1 1 11 111 0 0 0 00 000",                                       # digits alone
                 "5NN TU", "73", "R 73", "CQ",                                      # short overs
                 " ".join(["TEST"] * 8),                                            # nearly all E and T
                 "TEN NET ANT TAN NET TEN"):
        with tempfile.TemporaryDirectory() as tmp:
            st = make_stations(tmp, [{"hz": 1100, "wpm": 22, "text": text}])
            rc, lines, err = decode(mix(tmp, "real", st, 15))
        cer = edit_distance(letters(" ".join(l[2] for l in lines)), letters(text)) / len(letters(text))
        if rc or cer > 0.10:
            problems.append("%r: CER %.0f%%, exit %d: %r" % (text, 100 * cer, rc, lines))
    return problems


def check_lines_read_well():
    """One space between words, and prosigns as <SK>, so they cannot be taken for letters in a word."""
    problems = []
    with tempfile.TemporaryDirectory() as tmp:
        st = make_stations(tmp, [{"hz": 900, "wpm": 18, "text": QSO[0], "gen": ("-j", "0.15", "-W", "3.5", "-G", "1.8")}])
        rc, lines, err = decode(mix(tmp, "words", st, 12))
        rc2, raw, err2 = decode(mix(tmp, "words", st, 12), "--all")
    for hz_time, hz, text in lines + raw:
        if "  " in text or text != text.strip():
            problems.append("spacing: %r" % text)
        if re.search(r"[a-z]", text):
            problems.append("lower case: %r" % text)
    return problems or ([] if lines else ["no text"])


def check_one_line_per_signal():
    """A signal in the overlap of two slices is heard by two engines, and prints once."""
    with tempfile.TemporaryDirectory() as tmp:
        st = make_stations(tmp, [{"hz": 1700, "wpm": 22, "text": "CQ CQ DE K1ABC K1ABC PSE K"}])
        rc, lines, err = decode(mix(tmp, "twin", st, 25))
    heard = letters(" ".join(l[2] for l in lines))
    if rc or heard.count("K1ABCK1ABC") != 1:
        return ["exit %d: %r" % (rc, lines)]
    return []


def check_json():
    """JSON Lines carry the same text as the lines, and what only they have room for."""
    with tempfile.TemporaryDirectory() as tmp:
        st = make_stations(tmp, FOUR[:2])
        path = mix(tmp, "json", st, 15)
        rc, lines, err = decode(path)
        r = subprocess.run([CLI, "--json", path], capture_output=True, text=True)
    problems = []
    if r.returncode:
        problems.append("exit %d: %s" % (r.returncode, r.stderr))
    try:
        rows = [json.loads(l) for l in r.stdout.splitlines()]
    except ValueError as e:
        return ["not JSON (%s): %r" % (e, r.stdout[:200])]
    if [x["text"] for x in rows] != [l[2] for l in lines] or [x["frequency"] for x in rows] != [l[1] for l in lines]:
        problems.append("differs from the lines: %r" % rows[:2])
    calls = set()
    for x in rows:
        c = x.get("callsigns")
        if not isinstance(c, list) or len(set(c)) != len(c) or any(w not in x["text"].split() for w in c):
            problems.append("callsigns, which should be words of the text, each once: %r" % x)
        else:
            calls.update(c)
    if not {"K1ABC", "W1AW"} <= calls or calls & {"CQ", "DE", "TEST", "PSE", "K"}:
        problems.append("callsigns found: %s" % sorted(calls))
    for x in rows:
        if not (isinstance(x["time"], float) and isinstance(x["channel"], int) and 5 <= x["wpm"] <= 80 and "snr" in x
                and isinstance(x.get("score"), (int, float))):
            problems.append("fields: %r" % x)
        elif x["score"] < 1.0:
            problems.append("printed with a Morse score under the threshold: %r" % x)
        if "utc" in x:
            problems.append("a file has no time of day: %r" % x)
    return problems or ([] if rows else ["no rows"])


def check_seconds():
    """--seconds stops after that much audio, however long the file is."""
    with tempfile.TemporaryDirectory() as tmp:
        st = make_stations(tmp, [{"hz": 700, "wpm": 25, "text": " ".join(QSO)}])
        path = mix(tmp, "long", st, 15)
        rec = os.path.join(tmp, "rec.wav")
        r = subprocess.run([CLI, "-s", "10", "-r", rec, path], capture_output=True, text=True)
        seconds = len(read_wav(rec)) / RATE
    if r.returncode or not 9.9 < seconds < 10.1:
        return ["exit %d, %.2f s recorded" % (r.returncode, seconds)]
    return []


def stereo(left, right):
    n = max(len(left), len(right))
    left, right = list(left) + [0] * (n - len(left)), list(right) + [0] * (n - len(right))
    return [x for pair in zip(left, right) for x in pair]


def check_channel():
    """--channel picks one side of a stereo file or stream, and the two sides mix by default."""
    problems = []
    with tempfile.TemporaryDirectory() as tmp:
        a = make_stations(tmp, [{"hz": 700, "text": QSO[0]}])
        b = make_stations(tmp, [{"hz": 1500, "text": QSO[6]}])
        left, right = read_wav(mix(tmp, "l", a, 20)), read_wav(mix(tmp, "r", b, 20))
        path = os.path.join(tmp, "stereo.wav")
        samples = stereo(left, right)
        write_wav(path, samples, channels=2)
        raw = array.array("h", samples).tobytes()

        def pitches(lines):
            return {700 if abs(l[1] - 700) <= MATCH_HZ else 1500 if abs(l[1] - 1500) <= MATCH_HZ else l[1] for l in lines}

        for args, want in ((["-c", "1"], {700}), (["-c", "2"], {1500}), ([], {700, 1500})):
            rc, lines, err = decode(path, *args)
            if rc or pitches(lines) != want:
                problems.append("file %s: exit %d, heard %r" % (args, rc, pitches(lines)))
        r = subprocess.run([CLI, "-n", "2", "-c", "2", "-"], input=raw, capture_output=True)
        if r.returncode or b"PSE" in r.stdout or b"G3XYZ" not in r.stdout:
            problems.append("stream -c 2: exit %d: %r" % (r.returncode, r.stdout))
        r = subprocess.run([CLI, "-c", "3", path], capture_output=True, text=True)
        if r.returncode != 1 or "no channel 3" not in r.stderr:
            problems.append("-c 3 of 2: exit %d: %r" % (r.returncode, r.stderr))
    return problems


def check_raw_stream():
    """Raw samples at other rates and in other formats decode like the original."""
    problems = []
    with tempfile.TemporaryDirectory() as tmp:
        st = make_stations(tmp, [{"hz": 1500, "text": QSO[0]}])
        base = read_wav(mix(tmp, "base", st, 15))
    def upsampled(factor):
        out = []
        for i in range(len(base) - 1):
            for k in range(factor):
                out.append(base[i] + (base[i + 1] - base[i]) * k / factor)
        return out
    cases = {
        "48 kHz s16": (["-R", "48000"], array.array("h", [int(x) for x in upsampled(6)]).tobytes()),
        "48 kHz f32 stereo": (["--rate=48000", "--format=f32", "--channels=2"],
                              array.array("f", [v for x in upsampled(6) for v in (x / 32768, x / 32768)]).tobytes()),
        "8 kHz f32": (["-F", "f32"], array.array("f", [x / 32768 for x in base]).tobytes()),
        "cut short": ([], base.tobytes() + b"\x01"),
    }
    for name, (args, data) in cases.items():
        r = subprocess.run([CLI, *args, "-"], input=data, capture_output=True)
        if r.returncode or b"K1ABC K1ABC" not in r.stdout:
            problems.append("%s: exit %d: %r %r" % (name, r.returncode, r.stdout, r.stderr))
    return problems


def check_option_forms():
    """The usual ways of writing options, and messages that help when they are wrong."""
    problems = []
    with tempfile.TemporaryDirectory() as tmp:
        st = make_stations(tmp, FOUR)
        path = mix(tmp, "four", st, 15)
        want = decode(path, "-b", "1000-2000")[1]
        for args in (["--band", "1000-2000"], ["--band=1000-2000"], ["-b1000-2000"]):
            got = decode(path, *args)[1]
            if got != want or not got:
                problems.append("%s gave %r" % (args, got))
        got = decode(path, "-ab", "1000-2000")[1]
        if not got:
            problems.append("-ab 1000-2000 gave nothing")
    for args, wants in ((["--jsn"], "--json"), (["--devcie", "x"], "--device"), (["--bnad=1-2"], "--band"),
                        (["--plain=yes"], "no value"), (["-T", "-p"], "both"), (["-z"], "-z"),
                        (["-R", "48000", "x.wav"], "FILE of -"), (["-R", "48000"], "FILE of -"), (["-F", "s8", "-"], "s16 or f32"),
                        (["-R", "10", "-"], "sample rate"), (["-s", "0"], "seconds"), (["-c", "0"], "channel"),
                        (["-n", "1", "-c", "2", "-"], "--channels")):
        r = subprocess.run([CLI, *args], capture_output=True, text=True, input="")
        if r.returncode != 2 or wants not in r.stderr or r.stdout:
            problems.append("%s: exit %d, stdout %r, stderr %r" % (args, r.returncode, r.stdout, r.stderr))
    return problems


def check_documented():
    """Every option is in the help, the man page and the README, and they list no others."""
    r = subprocess.run([CLI, "--help"], capture_output=True, text=True)
    named = set(re.findall(r"^\s+-\w, (--[a-z]+)", r.stdout, re.M))
    problems = [] if len(named) >= 15 else ["--help names only %r" % sorted(named)]
    for name in ("vail-cli.1", "README.md"):
        with open(os.path.join(ROOT, name), encoding="utf-8") as f:
            text = f.read().replace("\\-", "-")
        missing = [o for o in sorted(named) if o not in text]
        if missing:
            problems.append("%s does not mention %s" % (name, ", ".join(missing)))
    return problems


def wait_for(proc, seconds):
    try:
        proc.wait(seconds)
        return True
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
        return False


def check_signals_on_idle_stream():
    """Ctrl+C or SIGTERM ends a run whose stream has gone quiet, says what was heard, and finishes a recording."""
    if os.name == "nt":
        return []
    problems = []
    with tempfile.TemporaryDirectory() as tmp:
        st = make_stations(tmp, [{"hz": 800, "text": QSO[0]}])
        data = read_wav(mix(tmp, "idle", st, 15)).tobytes()
        for sig in (signal.SIGINT, signal.SIGTERM):
            rec = os.path.join(tmp, "rec%d.wav" % sig)
            p = subprocess.Popen([CLI, "-r", rec, "-"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            p.stdin.write(data)
            p.stdin.flush()
            time.sleep(1.5)                  # the stream stays open, with nothing more on it
            p.send_signal(sig)
            if not wait_for(p, 10):
                problems.append("signal %d did not end the run" % sig)
                continue
            out = p.stdout.read().decode()
            p.stdin.close(), p.stdout.close(), p.stderr.close()
            if p.returncode or "K1ABC" not in out:
                problems.append("signal %d: exit %d: %r" % (sig, p.returncode, out))
            if len(read_wav(rec)) * 2 < len(data) - 2 * 85:
                problems.append("signal %d: the recording is short: %d of %d" % (sig, len(read_wav(rec)) * 2, len(data)))
    return problems


# ------------------------------------------------------------------- on a terminal (not Windows)

def strip_codes(data):
    return re.sub(r"\x1b(\[[0-9;?]*[ -/]*[@-~]|\([0-9A-Z]|[=>78])|\x0f|\x0e", "", data.decode("latin-1"))


class Terminal:
    """A program on a pseudo-terminal that is its controlling terminal, with stdin and stdout optionally elsewhere."""

    def __init__(self, args, stdin=None, stdout=None, term="xterm", env=None, cols=100, rows=30):
        self.data = b""
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            try:
                fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
                if stdin is not None:
                    os.dup2(stdin, 0)
                if stdout is not None:
                    os.dup2(stdout, 1)
                environment = dict(os.environ, TERM=term, **(env or {}))
                os.execve(CLI, [CLI, *args], environment)
            finally:
                os._exit(127)
        self.before = None

    def attributes(self):
        return termios.tcgetattr(self.fd)

    def pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            ready, _, _ = select.select([self.fd], [], [], 0.05)
            if ready:
                try:
                    chunk = os.read(self.fd, 65536)
                except OSError:
                    return
                if not chunk:
                    return
                self.data += chunk

    def wait_for_text(self, text, seconds=30):
        end = time.time() + seconds
        while time.time() < end:
            self.pump(0.1)
            if text in strip_codes(self.data):
                return True
        return False

    def send(self, keys):
        os.write(self.fd, keys.encode())

    def finish(self, seconds=15):
        """Returns the exit status, or None if it had to be killed."""
        end = time.time() + seconds
        while time.time() < end:
            self.pump(0.05)
            pid, status = os.waitpid(self.pid, os.WNOHANG)
            if pid:
                self.pump(0.1)
                return os.waitstatus_to_exitcode(status)
        os.kill(self.pid, signal.SIGKILL)
        os.waitpid(self.pid, 0)
        return None


def stream_then_hold(tmp, *stations, tail=7, end=False):
    """A pipe carrying a station, and then staying open with nothing on it, or with end, closing once all of it is written.
    Closing the write end from outside while the write is blocked cuts the stream short on macOS, after one pipe buffer."""
    st = make_stations(tmp, list(stations))
    samples = read_wav(mix(tmp, "term", st, 15, seconds=None))
    data = samples.tobytes() + b"\0\0" * (tail * RATE)
    reader, writer = os.pipe()
    def feed():
        try:
            os.write(writer, data)
        except OSError:
            pass
        if end:
            os.close(writer)
    threading.Thread(target=feed, daemon=True).start()
    return reader, writer


def sgr_before(param, word):
    """A pattern for word drawn just after an SGR escape that sets param, with any other escapes between."""
    return r"\x1b\[(?:[0-9]+;)*%s(?:;[0-9]+)*m(?:\x1b\[[0-9;]*m|\x1b\([0-9A-Z])*%s" % (param, re.escape(word))


def check_view_on_terminal():
    """The view opens by itself for a stream on a terminal, answers keys while the stream is idle, highlights callsigns,
    leaves the terminal as it found it, and prints the lines it was showing."""
    if not TERMINAL or not view_is_built():
        return []
    problems = []
    with tempfile.TemporaryDirectory() as tmp:
        reader, writer = stream_then_hold(tmp, {"hz": 800, "text": QSO[0]})
        t = Terminal(["-"], stdin=reader)
        os.close(reader)
        if not t.wait_for_text("SIGNALS"):
            problems.append("no view: %r" % strip_codes(t.data)[-300:])
        if not t.wait_for_text("  797  CQ CQ"):
            problems.append("no line in the log: %r" % strip_codes(t.data)[-300:])
        t.send("?")
        if not t.wait_for_text("Press any key"):
            problems.append("no help")
        t.send("x")
        t.pump(0.5)
        t.send("c")
        t.pump(0.5)
        t.send("q")
        status = t.finish()
        os.close(writer)
        text = t.data.decode("latin-1")
        if status != 0:
            problems.append("exit %r" % status)
        highlight = "1" if "NO_COLOR" in os.environ else "35"   # bold, and magenta where there is colour
        if not re.search(sgr_before(highlight, "K1ABC"), text) or re.search(sgr_before("35", "CQ"), text):
            problems.append("callsigns are not highlighted, or more than them is: %r" % text[-300:])
        if "\x1b[?1049h" not in text or "\x1b[?1049l" not in text:
            problems.append("the alternate screen was not used and left")
        else:
            after = text[text.rindex("\x1b[?1049l"):]
            if "K1ABC K1ABC" not in strip_codes(after.encode("latin-1")):
                problems.append("the lines were not printed at the end: %r" % after[-300:])
            if "\x1b[?25h" not in text[text.rindex("\x1b[?25l"):]:
                problems.append("the cursor was left hidden")
    return problems


def check_view_restores_terminal():
    """After the view, the terminal has its old settings: echo and line editing come back.

    A process stays as the session's leader, as a shell would. On macOS, a terminal stops answering once its leader has exited."""
    if not TERMINAL or not view_is_built():
        return []
    with tempfile.TemporaryDirectory() as tmp:
        reader, writer = stream_then_hold(tmp, {"hz": 800, "text": "CQ"}, tail=0)
        master, slave = pty.openpty()
        status_r, status_w = os.pipe()
        hold_r, hold_w = os.pipe()
        status = None
        before = termios.tcgetattr(slave)
        pid = os.fork()
        try:
            if pid == 0:
                try:
                    os.close(hold_w)
                    os.setsid()
                    fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
                    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 100, 0, 0))
                    program = os.fork()
                    if program == 0:
                        os.dup2(reader, 0)
                        os.dup2(slave, 1)
                        os.dup2(slave, 2)
                        os.execve(CLI, [CLI, "-"], dict(os.environ, TERM="xterm"))
                    _, st = os.waitpid(program, 0)
                    os.write(status_w, b"%d" % os.waitstatus_to_exitcode(st))
                    os.read(hold_r, 1)                       # until the settings have been read
                finally:
                    os._exit(127)
            os.close(reader)
            os.close(status_w)
            os.close(hold_r)
            seen = b""
            end = time.time() + 15
            while b"SIGNALS" not in seen and time.time() < end:
                if select.select([master], [], [], 0.1)[0]:
                    seen += os.read(master, 65536)
            during = termios.tcgetattr(slave)
            os.write(master, b"q")
            os.close(writer)
            end = time.time() + 15
            while time.time() < end and status is None:
                ready = select.select([master, status_r], [], [], 0.05)[0]
                if master in ready:
                    try:
                        os.read(master, 65536)
                    except OSError:
                        pass
                if status_r in ready:
                    status = int(os.read(status_r, 16) or b"-1")
            after = termios.tcgetattr(slave)
        finally:
            if status is None:
                os.kill(pid, signal.SIGKILL)
            os.close(hold_w)
            os.waitpid(pid, 0)
            os.close(status_r)
            os.close(master)
            os.close(slave)
    problems = []
    if b"SIGNALS" not in seen:
        problems.append("no view: %r" % seen[-200:])
    if during[3] == before[3]:
        problems.append("the view did not change the terminal's settings, so this test shows nothing")
    state = getattr(termios, "PENDIN", 0)                    # the kernel's own: BSD sets it on going back to line editing
    if after[3] & ~state != before[3] & ~state:
        problems.append("local modes not restored: %o, was %o" % (after[3], before[3]))
    if status != 0:
        problems.append("exit %r" % status)
    return problems


def check_view_beside_a_pipe():
    """With -T and stdout elsewhere, the view is on the terminal and stdout gets the lines as they finish."""
    if not TERMINAL or not view_is_built():
        return []
    problems = []
    with tempfile.TemporaryDirectory() as tmp:
        reader, writer = stream_then_hold(tmp, {"hz": 800, "text": QSO[0]})
        log = os.path.join(tmp, "out.txt")
        out = os.open(log, os.O_WRONLY | os.O_CREAT)
        t = Terminal(["-T", "-"], stdin=reader, stdout=out)
        os.close(reader)
        os.close(out)
        if not t.wait_for_text("  797  CQ CQ"):
            problems.append("no line in the view: %r" % strip_codes(t.data)[-300:])
        with open(log) as f:
            written = f.read()
        if "K1ABC K1ABC" not in written:
            problems.append("stdout has no line while the view is open: %r" % written)
        t.send("q")
        status = t.finish()
        os.close(writer)
        with open(log) as f:
            written = f.read()
        if status != 0 or written.count("K1ABC K1ABC K1ABC") != 1 or "\x1b" in written:
            problems.append("exit %r, stdout %r" % (status, written))
    return problems


def check_view_holds_at_the_end():
    """A file in the view stays on the screen when it has finished, until it is closed."""
    if not TERMINAL or not view_is_built():
        return []
    problems = []
    with tempfile.TemporaryDirectory() as tmp:
        st = make_stations(tmp, [{"hz": 800, "text": QSO[0]}])
        t = Terminal(["-T", mix(tmp, "f", st, 15)])
        if not t.wait_for_text("finished"):
            problems.append("does not say it has finished: %r" % strip_codes(t.data)[-300:])
        if "waiting for audio" in strip_codes(t.data):
            problems.append("a file has no input level to wait for")
        t.pump(1)
        if os.waitpid(t.pid, os.WNOHANG)[0]:
            return problems + ["it did not wait"]
        t.send("q")
        status = t.finish()
        if status != 0:
            problems.append("exit %r" % status)
    return problems


def check_no_view_on_terminal():
    """Where there can be no view, a terminal still works: lines, and -T says why it cannot."""
    if not TERMINAL:
        return []
    problems = []
    with tempfile.TemporaryDirectory() as tmp:
        st = make_stations(tmp, [{"hz": 800, "text": QSO[0]}])
        path = mix(tmp, "f", st, 15)
        # A file on a terminal is lines, unless asked otherwise.
        t = Terminal([path])
        status = t.finish(30)
        text = t.data.decode("latin-1")
        if status != 0 or "K1ABC K1ABC" not in text or "\x1b" in text:
            problems.append("a file: exit %r: %r" % (status, text[-200:]))
        # So is a stream with --plain, or on a terminal that cannot show the view.
        for args, term in ((["--plain", "-"], "xterm"), (["-"], "dumb")):
            reader, _ = stream_then_hold(tmp, {"hz": 800, "text": QSO[0]}, end=True)
            t = Terminal(args, stdin=reader, term=term)
            os.close(reader)
            status = t.finish(30)
            text = t.data.decode("latin-1")
            if status != 0 or "K1ABC K1ABC" not in text or "\x1b" in text:
                problems.append("%s on %s: exit %r: %r" % (args, term, status, text[-200:]))
        # --view needs a terminal, or a build with one.
        p = subprocess.run([CLI, "-T", path], capture_output=True, text=True, start_new_session=True)
        if p.returncode != 1 or "cannot open the full-screen view" not in p.stderr or p.stdout:
            problems.append("-T with no terminal: exit %d: %r" % (p.returncode, p.stderr))
        t = Terminal(["-T", path], term="dumb")
        status = t.finish(30)
        if status != 1 or "cannot open the full-screen view" not in t.data.decode("latin-1"):
            problems.append("-T on a dumb terminal: exit %r: %r" % (status, t.data[-200:]))
        # A terminal is no source of raw samples.
        t = Terminal(["-"])
        status = t.finish(10)
        if status != 2 or "pipe" not in t.data.decode("latin-1"):
            problems.append("- from a terminal: exit %r: %r" % (status, t.data[-200:]))
    return problems


CHECKS = {"long-text": check_long_text_is_whole, "repeating-text": check_repeating_text_is_whole, "overs": check_overs, "carrier": check_carrier, "formats": check_formats,
          "band-option": check_band_option, "errors": check_errors, "write-errors": check_write_errors, "version": check_version,
          "not-morse": check_not_morse_is_hidden, "real-text": check_real_text_is_not_hidden, "lines-read-well": check_lines_read_well,
          "one-line-per-signal": check_one_line_per_signal, "json": check_json, "seconds": check_seconds, "channel": check_channel,
          "raw-stream": check_raw_stream, "option-forms": check_option_forms, "documented": check_documented,
          "signals-on-idle-stream": check_signals_on_idle_stream}

# These fork, so they run one at a time, after the threads above have finished.
TERMINAL_CHECKS = {"view-on-terminal": check_view_on_terminal, "view-restores-terminal": check_view_restores_terminal,
                   "view-beside-a-pipe": check_view_beside_a_pipe, "view-holds-at-the-end": check_view_holds_at_the_end,
                   "no-view-on-terminal": check_no_view_on_terminal}


def outcome(check):
    """A check that raises has failed, and the checks after it still run."""
    try:
        return check()
    except Exception as e:
        return ["%s: %s" % (type(e).__name__, e)]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--table", action="store_true")
    ap.add_argument("--filter", default="")
    args = ap.parse_args()
    table = {}
    names = [n for n in CASES if args.filter in n]
    checks = [n for n in CHECKS if args.filter in n]
    failed = 0
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        results = {n: pool.submit(run_case, n, table) for n in names}
        results.update({n: pool.submit(CHECKS[n]) for n in checks})
        for n in names + checks:
            problems = outcome(results[n].result)
            failed += bool(problems)
            print("%-4s %s%s" % ("FAIL" if problems else "ok", n, ": " + "; ".join(problems) if problems else ""))
            if args.table and n in table:
                worst, ghost_chars, cers, ghosts, texts = table[n]
                for hz, c in cers.items():
                    print("       %5d Hz  CER %5.1f%%  %s" % (hz, 100 * c, texts[hz]))
                for t, hz, text in ghosts:
                    print("       ghost %5d Hz  t=%3d  %s" % (hz, t, text))
    terminal = [n for n in TERMINAL_CHECKS if args.filter in n]
    for n in terminal:
        problems = outcome(TERMINAL_CHECKS[n])
        failed += bool(problems)
        print("%-4s %s%s" % ("FAIL" if problems else "ok", n, ": " + "; ".join(problems) if problems else ""))
    print("%d of %d failed" % (failed, len(names) + len(checks) + len(terminal)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
