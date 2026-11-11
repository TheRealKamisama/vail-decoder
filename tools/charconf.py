#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""How well vail_char.confidence tells wrong characters from right ones, on the weak-signal grid.

  tools/charconf.py [--builds DIR[,DIR...]] [--seeds N] [--wpm 20,25,30] [--snr -4,-6,...] [--cond clean,...]
                    [--wav F[,F...]] [--jobs N] [--show]

Each case of tools/weak.py's grid is decoded with `vail_decode -m -q` (the host text of the current channel and each
character's confidence). The decoded text is aligned to the sent text by edit distance; a decoded character that lines
up with the same sent character is right, any other (a substitution or an insertion) is wrong. Spaces are left out.
--wav adds recordings with no Morse in them, where every character is wrong.

For each build it prints the AUC (the chance that a wrong character has a lower confidence than a right one) and, for
a range of thresholds, the share of wrong and of right characters below it: the ones a host fading at that threshold
would fade."""
import os
import subprocess
import sys
from multiprocessing import Pool

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import weak  # noqa: E402

THRESHOLDS = [0.5, 1, 2, 3, 5, 8, 12, 16, 20, 30]


def decode(build, wav):
    out = subprocess.run([os.path.join(build, "vail_decode"), "-m", "-q", wav], check=True, capture_output=True,
                         text=True).stdout
    # the confidences are the last line; the host text may hold the newline and '>' the API inserts on a channel switch
    text, _, last = out[:-1].rpartition("\n")
    confs = [float(x) for x in last.split()]
    pairs, prev_space, after_newline = [], True, False
    for ch, c in zip(text, confs):                  # upper-cased, runs of spaces collapsed, as weak.norm
        if after_newline and ch == ">":             # the API's mark, not decoded text
            after_newline = False
            continue
        after_newline = ch == "\n"
        if ch.isspace():
            if not prev_space:
                pairs.append((" ", c))
            prev_space = True
        else:
            pairs.append((ch.upper(), c))
            prev_space = False
    while pairs and pairs[-1][0] == " ":
        pairs.pop()
    return pairs


def align(ref, hyp):
    """for each character of hyp, whether it lines up with the same character of ref"""
    n, m = len(ref), len(hyp)
    d = [[0] * (m + 1) for _ in range(n + 1)]
    for i in range(n + 1):
        d[i][0] = i
    for j in range(m + 1):
        d[0][j] = j
    for i in range(1, n + 1):
        for j in range(1, m + 1):
            d[i][j] = min(d[i - 1][j] + 1, d[i][j - 1] + 1, d[i - 1][j - 1] + (ref[i - 1] != hyp[j - 1]))
    ok, i, j = [False] * m, n, m
    while i > 0 and j > 0:
        if d[i][j] == d[i - 1][j - 1] + (ref[i - 1] != hyp[j - 1]):
            ok[j - 1] = ref[i - 1] == hyp[j - 1]
            i, j = i - 1, j - 1
        elif d[i][j] == d[i][j - 1] + 1:
            j -= 1
        else:
            i -= 1
    return ok


def score(args):
    case, build, wav = args
    pairs = decode(build, wav)
    hyp = "".join(p[0] for p in pairs)
    ok = align(weak.norm(case["text"]), hyp) if case else [False] * len(hyp)
    return [(c, good) for (ch, c), good in zip(pairs, ok) if ch != " "], hyp


def auc(wrong, right):
    """P(a wrong character's confidence < a right one's), ties counting a half"""
    if not wrong or not right:
        return float("nan")
    allv = sorted([(v, 0) for v in wrong] + [(v, 1) for v in right])
    rank, i, sum_right = 1, 0, 0.0
    while i < len(allv):
        j = i
        while j < len(allv) and allv[j][0] == allv[i][0]:
            j += 1
        mid = (rank + rank + (j - i) - 1) / 2
        sum_right += mid * sum(1 for k in range(i, j) if allv[k][1])
        rank += j - i
        i = j
    nr, nw = len(right), len(wrong)
    return (sum_right - nr * (nr + 1) / 2) / (nr * nw)


def main():
    argv = sys.argv[1:]

    def opt(name, default):
        return argv[argv.index(name) + 1] if name in argv else default
    builds = [os.path.abspath(b) for b in opt("--builds", os.path.join(weak.ROOT, "build")).split(",")]
    wpms = [int(x) for x in opt("--wpm", "20,25,30").split(",")]
    snrs = [int(x) for x in opt("--snr", "-4,-6,-8,-10,-12,-14").split(",")]
    conds = opt("--cond", "clean,hand,qsb,rx,qrn").split(",")
    seeds = int(opt("--seeds", "1"))
    jobs = int(opt("--jobs", str(os.cpu_count() or 4)))
    wavs_extra = [os.path.abspath(w) for w in opt("--wav", "").split(",") if w]
    show = "--show" in argv

    cases = weak.make_cases(wpms, snrs, conds, seeds, len(weak.TEXTS))
    wavdir = os.path.join(builds[0], "weak")
    os.makedirs(wavdir, exist_ok=True)
    with Pool(jobs) as pool:
        wavs = pool.map(weak.gen, [(c, os.path.join(builds[0], "cwgen"), wavdir) for c in cases], chunksize=4)
        work = [(c, w) for c, w in zip(cases, wavs)] + [(None, w) for w in wavs_extra]
        results = {b: pool.map(score, [(c, b, w) for c, w in work], chunksize=4) for b in builds}

    for b in builds:
        res = results[b]
        print(f"== {os.path.relpath(b, weak.ROOT)}  ({len(cases)} cases, {len(wavs_extra)} without Morse)")
        groups = [("all Morse", [r for (c, _), r in zip(work, res) if c])]
        for snr in snrs:
            groups.append((f"{snr} dB", [r for (c, _), r in zip(work, res) if c and c["snr"] == snr]))
        if len(wpms) > 1:
            for w in wpms:
                groups.append((f"{w} WPM", [r for (c, _), r in zip(work, res) if c and c["wpm"] == w]))
        if wavs_extra:
            groups.append(("no Morse", [r for (c, _), r in zip(work, res) if not c]))
        print(f"{'':>10s} {'right':>6s} {'wrong':>6s} {'AUC':>6s}   wrong / right faded below " +
              " ".join(f"{t:>9g}" for t in THRESHOLDS))
        for name, rs in groups:
            vals = [v for chars, _ in rs for v in chars]
            right = [c for c, g in vals if g]
            wrong = [c for c, g in vals if not g]
            row = []
            for t in THRESHOLDS:
                fw = 100 * sum(1 for c in wrong if c < t) / max(1, len(wrong))
                fr = 100 * sum(1 for c in right if c < t) / max(1, len(right))
                row.append(f"{fw:4.0f}/{fr:<4.0f}")
            a = auc(wrong, right)
            print(f"{name:>10s} {len(right):6d} {len(wrong):6d} {a:6.3f}   {'':26s}" + " ".join(row))
        if show:
            for (c, w), (chars, hyp) in zip(work, res):
                marks = []
                it = iter(chars)
                for ch in hyp:
                    if ch == " ":
                        marks.append(" ")
                        continue
                    conf, good = next(it)
                    marks.append(ch if good else ch.lower())
                print(f"{c['name'] if c else os.path.basename(w):30s} {''.join(marks)}")


if __name__ == "__main__":
    main()
