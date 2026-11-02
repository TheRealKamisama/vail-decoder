#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Two stations close together, with both scored: the case eval.py's qrm rows do not cover.

  tools/closepair.py [--builds DIR[,DIR...]] [--seeds N] [--spacings 60,80,...] [--show]

The first station is at 800 Hz, 25 WPM, SNR 15 dB in 2500 Hz. The second is above it at 18 WPM, level with
it or 15 dB down. Each cell is the mean CER, in percent, of the first and second station. Word spacing is
not scored. Each case is synthesised once by DIR/cwgen of the first build and decoded by every build, so
builds are compared on identical audio. See docs/close-signals.md.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PITCH = 800
MATCH_HZ = 40
PAIRS = [("CQ CQ CQ DE K1ABC K1ABC K1ABC PSE K", "TEST W1AW W1AW TEST W1AW W1AW TEST"),
         ("VVV VVV DE DL2XYZ DL2XYZ QTH MUNICH K", "CQ DX CQ DX DE JA1ZZZ JA1ZZZ UP 5 K"),
         ("R R TNX FER CALL UR RST 599 599 BK", "GM OM NAME IS JOHN JOHN HW CPY K")]


def letters(s):
    return "".join(s.split())


def edit_distance(a, b):
    prev = list(range(len(b) + 1))
    for i, x in enumerate(a, 1):
        cur = [i]
        for j, y in enumerate(b, 1):
            cur.append(min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (x != y)))
        prev = cur
    return prev[-1]


def cer(heard, sent):
    return 100.0 * edit_distance(letters(heard), letters(sent)) / len(letters(sent))


def decode(build, path, second_hz):
    """The text of each station: every channel goes to the nearest station within MATCH_HZ."""
    out = subprocess.run([os.path.join(build, "vail_decode"), "-a", path], capture_output=True, text=True).stdout
    heard = {PITCH: [], second_hz: []}
    for line in out.splitlines():
        m = re.match(r"\s*(\d+) Hz: (.*)", line)
        if not m:
            continue
        hz = int(m[1])
        nearest = min(heard, key=lambda s: abs(s - hz))
        if abs(nearest - hz) <= MATCH_HZ:
            heard[nearest].append(m[2])
    return " ".join(heard[PITCH]), " ".join(heard[second_hz])


def case(builds, tmp, spacing, rel_db, pair, seed):
    first, second = PAIRS[pair]
    path = os.path.join(tmp, "c%d_%d_%d_%d.wav" % (spacing, rel_db, pair, seed))
    subprocess.run([os.path.join(builds[0], "cwgen"), "-o", path, "-t", first, "-w", "25", "-f", str(PITCH), "-n", "15",
                    "-r", str(100 + seed), "-S", "%d:18:%d:%s" % (PITCH + spacing, rel_db, second)], check=True)
    results = {}
    for b in builds:
        a, c = decode(b, path, PITCH + spacing)
        results[b] = (cer(a, first), cer(c, second), a, c)
    os.unlink(path)
    return results


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--builds", default=os.path.join(ROOT, "build"))
    ap.add_argument("--seeds", type=int, default=2)
    ap.add_argument("--spacings", default="60,80,100,150,200,300")
    ap.add_argument("--show", action="store_true", help="print the decoded text of every case")
    args = ap.parse_args()
    builds = args.builds.split(",")
    spacings = [int(s) for s in args.spacings.split(",")]
    with tempfile.TemporaryDirectory() as tmp, ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        for rel_db in (0, -15):
            futures = {s: [pool.submit(case, builds, tmp, s, rel_db, p, seed)
                           for p in range(len(PAIRS)) for seed in range(args.seeds)] for s in spacings}
            results = {s: [f.result() for f in fs] for s, fs in futures.items()}
            print("second station %+d dB against the first" % rel_db)
            print("%-24s" % "build (first/second)" + "".join("%9d" % s for s in spacings))
            for b in builds:
                row = "%-24s" % os.path.basename(b.rstrip("/"))
                for s in spacings:
                    rs = [r[b] for r in results[s]]
                    row += "%9s" % ("%.0f/%.0f" % (sum(r[0] for r in rs) / len(rs), sum(r[1] for r in rs) / len(rs)))
                print(row, flush=True)
                if args.show:
                    for s in spacings:
                        for r in results[s]:
                            print("    +%d: %s | %s" % (s, r[b][2], r[b][3]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
