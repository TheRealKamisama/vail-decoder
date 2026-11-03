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
import math
import os
import random
import re
import subprocess
import sys
import tempfile
import wave
from concurrent.futures import ThreadPoolExecutor

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
    return "".join(s.split())


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
        if rc == 0 and read_wav(rec)[:8000] != read_wav(path)[:8000]:
            problems.append("-r did not record the input")
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


CHECKS = {"long-text": check_long_text_is_whole, "repeating-text": check_repeating_text_is_whole, "overs": check_overs, "carrier": check_carrier, "formats": check_formats,
          "band-option": check_band_option, "errors": check_errors}


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
            problems = results[n].result()
            failed += bool(problems)
            print("%-4s %s%s" % ("FAIL" if problems else "ok", n, ": " + "; ".join(problems) if problems else ""))
            if args.table and n in table:
                worst, ghost_chars, cers, ghosts, texts = table[n]
                for hz, c in cers.items():
                    print("       %5d Hz  CER %5.1f%%  %s" % (hz, 100 * c, texts[hz]))
                for t, hz, text in ghosts:
                    print("       ghost %5d Hz  t=%3d  %s" % (hz, t, text))
    print("%d of %d failed" % (failed, len(names) + len(checks)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
