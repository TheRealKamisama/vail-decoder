#!/usr/bin/env python3
"""Weak-signal evaluation of vail_decode: CER against SNR at 18-30 WPM, in parallel.

  tools/weak.py [--builds DIR[,DIR...]] [--seeds N] [--wpm 20,25,30] [--snr -4,-6,...]
                [--cond clean,hand,...] [--jobs N] [--show] [--filter SUBSTR] [--full]

Each case is synthesised once by cwgen (into DIR/weak/ of the first build) and decoded by every build,
so builds are compared on identical audio. SNR is signal power over noise power in 2500 Hz.

CER = Levenshtein(reference, decoded) / len(reference) on upper-cased text with runs of spaces
collapsed, capped at 100 % per case. The tables give the mean CER per SNR and condition; "SNR@10%"
is the SNR at which the mean CER (over all conditions) first rises above 10 %, interpolated."""
import os
import subprocess
import sys
import zlib
from multiprocessing import Pool

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

TEXTS = [
    "CQ CQ CQ DE DL1ABC DL1ABC PSE K",
    "R TNX FER CALL UR RST 599 5NN NAME IS JOHN QTH BERLIN HW CPY",
    "TU 73 ES GL DE G4XYZ SK",
    "QRZ? DE K1ABC/P K1ABC/P QRL? 73",
    "GM OM TNX FER RPRT UR RST 449 449 QSB HR",
    "W2XYZ DE JA1QRP 5NN 25 25 TU",
    "RIG IS IC7300 PWR 100W ANT DIPOLE WX SUNNY TEMP 22C",
    "VVV THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG 1234567890",
    "QXFJT 4KZ9R BWPLM 07HSE YDNCA",
    "OK FB DR OM SRI QRM BK",
]

# name -> cwgen options; jitter is the standard deviation of each element length as a fraction
CONDITIONS = {
    "clean": ["-j", "0.02"],                                         # keyer, white noise
    "hand":  ["-j", "0.08", "-W", "3.2", "-G", "1.25"],              # hand key: heavier dashes, wide spacing
    "qsb":   ["-j", "0.03", "-R", "0.25"],                           # Rayleigh fading, 0.25 Hz Doppler spread
    "rx":    ["-j", "0.03", "-B", "350:1100", "-d", "12"],           # narrow receiver filter, 12 Hz drift
    "qrm":   ["-j", "0.03", "-S", "+120:22:-3:VVV TEST DE OK1XYZ OK1XYZ AR"],   # neighbour 120 Hz away
    "qrn":   ["-j", "0.03", "-Q", "1.5"],                            # static crashes
    "drift": ["-j", "0.03", "-d", "40"],                             # 40 Hz drift over the message
}

DEFAULT_COND = "clean,hand,qsb,rx"
PICK_BY_PITCH = ("qrm",)


def lev(a, b):
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i]
        for j, cb in enumerate(b, 1):
            cur.append(min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (ca != cb)))
        prev = cur
    return prev[-1]


def norm(s):
    return " ".join(s.upper().split())


def make_cases(wpms, snrs, conds, seeds, ntexts):
    out = []
    for cond in conds:
        for wpm in wpms:
            for snr in snrs:
                for ti in range(ntexts):
                    for r in range(seeds):
                        name = f"{cond}_w{wpm}_s{snr}_t{ti}_r{r}"
                        h = zlib.crc32(name.encode())
                        out.append(dict(name=name, cond=cond, wpm=wpm, snr=snr, text=TEXTS[ti],
                                        pitch=450 + h % 500, seed=h % 100000))
    return out


def gen(args):
    case, cwgen, wavdir = args
    wav = os.path.join(wavdir, case["name"] + ".wav")
    if not os.path.exists(wav):
        opts = list(CONDITIONS[case["cond"]])
        for k, o in enumerate(opts):                    # extra-signal pitch is relative to the main one
            if o == "-S":
                hz, rest = opts[k + 1].split(":", 1)
                opts[k + 1] = f"{case['pitch'] + int(hz)}:{rest}"
        cmd = [cwgen, "-o", wav + ".tmp", "-t", case["text"], "-w", str(case["wpm"]), "-f", str(case["pitch"]),
               "-n", str(case["snr"]), "-r", str(case["seed"])] + opts
        subprocess.run(cmd, check=True)
        os.replace(wav + ".tmp", wav)
    return wav


def decode(args):
    case, build, wav = args
    if case["cond"] in PICK_BY_PITCH:
        # with a second signal the current channel at the end of the file may be that signal;
        # score the channel nearest the wanted pitch instead
        out = subprocess.run([os.path.join(build, "vail_decode"), "-a", wav], check=True, capture_output=True,
                             text=True).stdout
        dec, bestd = "", 26
        for line in out.splitlines():
            hz, _, text = line.partition(" Hz: ")
            if text and abs(int(hz) - case["pitch"]) < bestd:
                dec, bestd = text, abs(int(hz) - case["pitch"])
    else:
        dec = subprocess.run([os.path.join(build, "vail_decode"), wav], check=True, capture_output=True,
                             text=True).stdout
    ref, got = norm(case["text"]), norm(dec)
    return min(1.0, lev(ref, got) / max(1, len(ref))), got


def snr_at(snrs, cers, limit):
    """highest SNR (scanning downward) at which the CER curve crosses limit, linearly interpolated"""
    pts = sorted(zip(snrs, cers), reverse=True)
    if pts[0][1] > limit:
        return None
    for (s0, c0), (s1, c1) in zip(pts, pts[1:]):
        if c1 > limit:
            return s0 + (s1 - s0) * (limit - c0) / (c1 - c0)
    return pts[-1][0]


def main():
    argv = sys.argv[1:]

    def opt(name, default):
        return argv[argv.index(name) + 1] if name in argv else default
    builds = [os.path.abspath(b) for b in opt("--builds", os.path.join(ROOT, "build")).split(",")]
    full = "--full" in argv
    wpms = [int(x) for x in opt("--wpm", "18,22,25,28,30" if full else "20,25,30").split(",")]
    snrs = [float(x) if "." in x else int(x) for x in opt("--snr", "-4,-6,-8,-10,-12,-14").split(",")]
    conds = opt("--cond", ",".join(CONDITIONS) if full else DEFAULT_COND).split(",")
    seeds = int(opt("--seeds", "2"))
    ntexts = int(opt("--texts", str(len(TEXTS))))
    jobs = int(opt("--jobs", str(os.cpu_count() or 4)))
    flt = opt("--filter", None)
    show = "--show" in argv

    cases = [c for c in make_cases(wpms, snrs, conds, seeds, ntexts) if not flt or flt in c["name"]]
    wavdir = os.path.join(builds[0], "weak")
    os.makedirs(wavdir, exist_ok=True)
    with Pool(jobs) as pool:
        wavs = pool.map(gen, [(c, os.path.join(builds[0], "cwgen"), wavdir) for c in cases], chunksize=4)
        results = {}
        for b in builds:
            results[b] = pool.map(decode, [(c, b, w) for c, w in zip(cases, wavs)], chunksize=4)

    for b in builds:
        res = results[b]
        if show:
            for c, (cer, got) in zip(cases, res):
                print(f"{c['name']:30s} {cer * 100:6.1f}%  {got}")
        print(f"== {os.path.relpath(b, ROOT)}  ({len(cases)} cases)")
        print(f"{'SNR':>6s}" + "".join(f"{k:>8s}" for k in conds) + f"{'all':>8s}")
        curve = []
        for snr in snrs:
            row, allv = [], []
            for k in conds:
                v = [r[0] for c, r in zip(cases, res) if c["snr"] == snr and c["cond"] == k]
                allv += v
                row.append(100 * sum(v) / max(1, len(v)))
            curve.append(100 * sum(allv) / max(1, len(allv)))
            print(f"{snr:>6}" + "".join(f"{x:8.1f}" for x in row) + f"{curve[-1]:8.1f}")
        byw = []
        for w in wpms:
            v = [r[0] for c, r in zip(cases, res) if c["wpm"] == w]
            byw.append(f"{w}wpm {100 * sum(v) / max(1, len(v)):.1f}")
        mean = 100 * sum(r[0] for r in res) / max(1, len(res))
        t10, t25 = snr_at(snrs, curve, 10.0), snr_at(snrs, curve, 25.0)
        fmt = lambda t: "n/a" if t is None else f"{t:.1f} dB"
        print(f"mean CER {mean:.2f}%   SNR@10% {fmt(t10)}   SNR@25% {fmt(t25)}   " + "  ".join(byw))


if __name__ == "__main__":
    main()
