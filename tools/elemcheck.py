#!/usr/bin/env python3
"""Element-level error breakdown against the generator's ground truth.

  tools/elemcheck.py [--build DIR] [--wpm 20,25,30] [--snr -10,-12] [--cond clean] [--seeds N] [--show]

For every case of the tools/weak.py grid (same names, seeds and options) the WAV is rebuilt with the
true key-down intervals (cwgen -K), decoded with vail_decode -E, and the elements of the channel
nearest the true pitch are matched against the truth:
  marks: missed, spurious, split (one true mark, several decoded), merged (several true, one decoded),
         wrong kind (dot <-> dash);
  gaps between correctly matched marks: wrong class (element / letter / word gap).
Rates are per true mark (gap rates per true gap)."""
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import weak  # noqa: E402

FRAME = 85 / 8000.0
VERBOSE = "--verbose" in sys.argv


def load_truth(path):
    iv = []
    for line in open(path):
        a, b, g = line.split()
        iv.append(((float(a) * 8000 + 512) / 85, (float(b) * 8000 + 512) / 85, float(g)))
    return iv


def load_elems(path, bin_):
    by = {}
    for line in open(path):
        cid, start, dur, kind, b = map(int, line.split())
        by.setdefault(cid, []).append((start, dur, kind, b))
    best, bestn = None, 0                        # the channel near the pitch with the most marks
    for cid, el in by.items():
        d = abs(sum(e[3] for e in el) / len(el) - bin_)
        n = len([e for e in el if e[1] > 0])
        if d <= 3.5 and n > bestn:
            best, bestn = cid, n
    if best is None:
        return []
    last = {}
    for end, dur, kind, b in by[best]:           # re-sent elements (priming flush): keep the last
        last[end] = (end - abs(dur), dur, kind, b)
    return sorted(last.values())


def analyse(truth, elems, wpm):
    unit = 1.2 / wpm / FRAME
    marks = [(s, s + d, k) for s, d, k, _ in elems if d > 0]
    gaps = {}
    for s, d, k, _ in elems:
        if d <= 0:
            gaps[s] = k                          # keyed by the gap's start = the preceding mark's end
    st = dict(marks=len(truth), missed=0, missfade=0, missdot=0, missdash=0, split=0, merged=0, spurious=0, kind=0, gaps=0, gapcls=0, durerr=0.0)
    if not marks:
        st["missed"] = len(truth)
        return st
    lo, hi = marks[0][0] - 5 * unit, marks[-1][1] + 5 * unit
    cover = [[] for _ in truth]
    used = [[] for _ in marks]
    for i, (a, b, _) in enumerate(truth):
        for j, (s, e, k) in enumerate(marks):
            if min(b, e) - max(a, s) > 0.3 * min(b - a, e - s):
                cover[i].append(j)
                used[j].append(i)
    match = {}
    for i, (a, b, _) in enumerate(truth):
        if not cover[i]:
            st["missed"] += 1
            g = truth[i][2]
            st["missfade"] += g * g < 0.1
            st["missdash" if (b - a) > 2 * unit else "missdot"] += 1
            if VERBOSE:
                print(f"  missed {'dash' if (b - a) > 2 * unit else 'dot'} at frame {a:.0f}..{b:.0f}")
        elif len(cover[i]) > 1:
            st["split"] += 1
        else:
            j = cover[i][0]
            if len(used[j]) > 1:
                st["merged"] += 1
                continue
            match[i] = j
            s, e, k = marks[j]
            true_dash = (b - a) > 2 * unit
            if (k == 1) != true_dash:
                st["kind"] += 1
            st["durerr"] += abs((e - s) - (b - a)) / unit
    st["spurious"] = sum(1 for u in used if not u)
    for i in range(len(truth) - 1):
        if i in match and i + 1 in match and match[i + 1] == match[i] + 1:
            g = (truth[i + 1][0] - truth[i][1]) / unit
            tcls = 2 if g < 2 else (3 if g < 5 else 4)
            j = match[i]
            gs = marks[j][1]
            k = gaps.get(gs)
            if k is None:
                continue
            st["gaps"] += 1
            if k != tcls and not (tcls == 4 and k == 4):
                st["gapcls"] += 1
    return st


def main():
    argv = sys.argv[1:]

    def opt(name, default):
        return argv[argv.index(name) + 1] if name in argv else default
    build = os.path.abspath(opt("--build", os.path.join(weak.ROOT, "build")))
    wpms = [int(x) for x in opt("--wpm", "20,25,30").split(",")]
    snrs = [int(x) for x in opt("--snr", "-8,-10,-12").split(",")]
    conds = opt("--cond", "clean").split(",")
    seeds = int(opt("--seeds", "2"))
    show = "--show" in argv
    cases = weak.make_cases(wpms, snrs, conds, seeds, len(weak.TEXTS))
    tot = {}
    with tempfile.TemporaryDirectory() as tmp:
        for c in cases:
            wav, key, el = (os.path.join(tmp, c["name"] + x) for x in (".wav", ".key", ".el"))
            opts = list(weak.CONDITIONS[c["cond"]])
            for k, o in enumerate(opts):
                if o == "-S":
                    hz, rest = opts[k + 1].split(":", 1)
                    opts[k + 1] = f"{c['pitch'] + int(hz)}:{rest}"
            subprocess.run([os.path.join(build, "cwgen"), "-o", wav, "-t", c["text"], "-w", str(c["wpm"]),
                            "-f", str(c["pitch"]), "-n", str(c["snr"]), "-r", str(c["seed"]), "-K", key] + opts,
                           check=True)
            subprocess.run([os.path.join(build, "vail_decode"), "-E", el, wav], check=True, capture_output=True)
            st = analyse(load_truth(key), load_elems(el, (c["pitch"] - 203.125) / 7.8125), c["wpm"])
            g = (c["snr"], c["wpm"])
            acc = tot.setdefault(g, {})
            for k, v in st.items():
                acc[k] = acc.get(k, 0) + v
            if show:
                print(c["name"], st)
    print(f"{'snr':>4} {'wpm':>4} {'marks':>6} {'miss%':>6} {'(fade':>6} {'dot':>6} {'dash)':>6} {'spur%':>6} {'split%':>6} "
          f"{'merge%':>6} {'kind%':>6} {'gap%':>6} {'durerr':>6}")
    for (snr, wpm), a in sorted(tot.items(), key=lambda x: (-x[0][0], x[0][1])):
        m = max(1, a["marks"])
        mm = max(1, a["marks"] - a["missed"] - a["split"] - a["merged"])
        print(f"{snr:>4} {wpm:>4} {a['marks']:>6} {100*a['missed']/m:6.1f} {100*a['missfade']/m:6.1f} {100*a['missdot']/m:6.1f} "
              f"{100*a['missdash']/m:6.1f} {100*a['spurious']/m:6.1f} "
              f"{100*a['split']/m:6.1f} {100*a['merged']/m:6.1f} {100*a['kind']/mm:6.1f} "
              f"{100*a['gapcls']/max(1,a['gaps']):6.1f} {a['durerr']/mm:6.2f}")


if __name__ == "__main__":
    main()
