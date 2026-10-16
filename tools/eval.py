#!/usr/bin/env python3
"""Evaluate vail_decode on synthetic Morse: generates cases with cwgen, decodes, reports CER.

  tools/eval.py [--quick] [--seeds N] [--filter SUBSTR] [--show]

CER = Levenshtein(reference, decoded) / len(reference), computed on upper-cased text with
runs of spaces collapsed (so word spacing counts, as one character per gap). CER-ns ignores
spaces entirely (character accuracy only)."""
import os
import subprocess
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.environ.get("VAIL_BUILD", os.path.join(ROOT, "build"))
TMP = os.path.join(BUILD, "eval")

TEXTS = [
    "CQ CQ CQ DE DL1ABC DL1ABC PSE K",
    "R TNX FER CALL UR RST 599 5NN NAME IS JOHN QTH BERLIN HW CPY",
    "VVV THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG 1234567890",
    "TU 73 ES GL DE G4XYZ SK",
    "QRZ? DE K1ABC/P K1ABC/P QRL? 73",
]


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


def cases(quick):
    out = []
    speeds = [12, 20, 30, 40] if not quick else [15, 25, 35]
    snrs = [None, 10, 3, 0, -3] if not quick else [None, 6, 0]
    for wpm in speeds:
        for snr in snrs:
            for ti, text in enumerate(TEXTS if not quick else TEXTS[:2]):
                pitch = 500 + 97 * ((wpm + ti) % 6)
                out.append(dict(name=f"w{wpm}_s{'inf' if snr is None else snr}_t{ti}", wpm=wpm, snr=snr,
                                text=text, pitch=pitch, jitter=0.0, fade=0.0))
    # hand keying (timing jitter), fading, drift
    for wpm in ([18, 28] if not quick else [22]):
        for j in (0.1, 0.2):
            out.append(dict(name=f"jit{j}_w{wpm}", wpm=wpm, snr=10, text=TEXTS[1], pitch=650, jitter=j, fade=0.0))
        out.append(dict(name=f"fade_w{wpm}", wpm=wpm, snr=6, text=TEXTS[2], pitch=750, jitter=0.05, fade=0.7))
    # interference (weaker second signal), impulse noise, pitch drift
    for off in ([50, 100, 200] if not quick else [100]):
        out.append(dict(name=f"qrm{off}", wpm=24, snr=10, text=TEXTS[1], pitch=700, jitter=0.03, fade=0.0,
                        extra=["-S", f"{700 + off}:18:-6:VVV TEST TEST DE OK1XYZ OK1XYZ AR"]))
    for q in ([2, 8] if not quick else [4]):
        out.append(dict(name=f"qrn{q}", wpm=22, snr=10, text=TEXTS[0], pitch=600, jitter=0.03, fade=0.0,
                        extra=["-Q", str(q)]))
    for i, t in enumerate(["TEST", "TU", "K1ABC", "5NN TU"]):
        out.append(dict(name=f"short{i}", wpm=20 + 4 * i, snr=6, text=t, pitch=620, jitter=0.03, fade=0.0))
    out.append(dict(name="drift", wpm=20, snr=6, text=TEXTS[1], pitch=650, jitter=0.03, fade=0.0,
                    extra=["-d", "25"]))
    return out


def with_seeds(base, seeds):
    out = []
    for c in base:
        for r in range(seeds):
            d = dict(c)
            d["name"] = c["name"] + f"_r{r}"
            out.append(d)
    return out


def run(case, show):
    os.makedirs(TMP, exist_ok=True)
    wav = os.path.join(TMP, case["name"] + ".wav")
    cmd = [os.path.join(BUILD, "cwgen"), "-o", wav, "-t", case["text"], "-w", str(case["wpm"]),
           "-f", str(case["pitch"]), "-j", str(case["jitter"]), "-r", str(zlib.crc32(case["name"].encode()) % 100000)]
    if case["snr"] is not None:
        cmd += ["-n", str(case["snr"])]
    if case["fade"]:
        cmd += ["-F", "0.3", "-D", str(case["fade"])]
    cmd += case.get("extra", [])
    subprocess.run(cmd, check=True)
    dec = subprocess.run([os.path.join(BUILD, "vail_decode"), wav], check=True, capture_output=True,
                         text=True).stdout
    ref, got = norm(case["text"]), norm(dec)
    cer = lev(ref, got) / max(1, len(ref))
    rn, gn = ref.replace(" ", ""), got.replace(" ", "")
    cerns = lev(rn, gn) / max(1, len(rn))
    if show:
        print(f"{case['name']:28s} {cer*100:6.1f}%  {got}")
    return cer, cerns


def main():
    quick = "--quick" in sys.argv
    show = "--show" in sys.argv
    flt = None
    if "--filter" in sys.argv:
        flt = sys.argv[sys.argv.index("--filter") + 1]
    seeds = int(sys.argv[sys.argv.index("--seeds") + 1]) if "--seeds" in sys.argv else 2
    groups = {}
    total = []
    for c in with_seeds(cases(quick), seeds):
        if flt and flt not in c["name"]:
            continue
        r = run(c, show)
        key = c["name"].rsplit("_r", 1)[0]
        key = key.rsplit("_t", 1)[0] if "_t" in key else key
        groups.setdefault(key, []).append(r)
        total.append(r)

    def avg(v, i):
        return 100 * sum(x[i] for x in v) / max(1, len(v))
    for k, v in groups.items():
        print(f"{k:24s} CER {avg(v, 0):6.1f}%  CER-ns {avg(v, 1):6.1f}%")
    print(f"{'OVERALL':24s} CER {avg(total, 0):6.1f}%  CER-ns {avg(total, 1):6.1f}%  ({len(total)} cases)")


if __name__ == "__main__":
    main()
