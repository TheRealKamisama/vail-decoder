#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Real recordings with a known text: the telegrams of tools/telegrams.tsv (docs/telegrams.md).

  tools/telegrams.py DIR [--fetch] [--builds DIR[,DIR...]] [--filter SUBSTR] [--jobs N] [--show]

DIR holds the recordings, which are not part of the repository; --fetch downloads the audio from Google Drive into
DIR/raw and converts it to 8 kHz WAV in DIR (needs ffmpeg). Each file is decoded three ways:

  channels  vail_decode -a -c: every channel's text and Morse score. "best" is the channel closest to the telegram,
            "by score" the one with the highest Morse score;
  host      vail_decode -m -q: the host text (the current channel), what an application shows;
  pinned    vail_decode -m -q -p HZ at the best channel's pitch: the decoder with the channel question taken away.

Text is compared without spaces or punctuation (letters, digits and '/'; a prosign counts as one character). The
telegram is aligned to the decoded text with free ends, so a preamble (VVV), the closing AR and text before or after
the message cost nothing; CER is the edits over the telegram's length. A decoded character inside the aligned stretch is
right when it lines up with the same character of the telegram. The confidence lines give, for the pinned decode, the
AUC (the chance that a wrong character scores lower than a right one) and the share of wrong / right characters below
each threshold, over all files and over the decodable ones (pinned CER under 50 %)."""
import os
import re
import subprocess
import sys
import urllib.request
from multiprocessing import Pool

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from charconf import auc  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
THRESHOLDS = [1, 3, 5, 8, 12]
DECODABLE = 0.5


def load():
    cases = []
    for line in open(os.path.join(HERE, "telegrams.tsv"), encoding="utf-8"):
        if line.startswith("#") or not line.strip():
            continue
        audio, drive_id, photo, text = line.rstrip("\n").split("\t")
        cases.append({"audio": audio, "id": drive_id, "photo": photo, "text": text,
                      "name": os.path.splitext(audio)[0].replace(" ", "_")})
    return cases


def fetch(cases, root):
    os.makedirs(os.path.join(root, "raw"), exist_ok=True)
    for c in cases:
        raw = os.path.join(root, "raw", c["audio"])
        wav = os.path.join(root, c["name"] + ".wav")
        if not os.path.exists(raw):
            url = f"https://drive.usercontent.google.com/download?id={c['id']}&export=download&confirm=t"
            req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
            with urllib.request.urlopen(req, timeout=300) as r:
                data = r.read()
            if data[:200].lstrip().lower().startswith((b"<!doctype", b"<html")):
                sys.exit(f"{c['audio']}: Drive returned a web page, not the file")
            with open(raw, "wb") as f:
                f.write(data)
            print(f"fetched {c['audio']} ({len(data) // 1024} KB)")
        if not os.path.exists(wav):
            subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-y", "-i", raw, "-ac", "1", "-ar", "8000", wav],
                           check=True)


def norm_pairs(pairs):
    """[(char, confidence)] as compared: upper case, letters, digits and '/', a prosign ('sk', 'ar', ...) as '#'"""
    out, i = [], 0
    while i < len(pairs):
        ch = pairs[i][0]
        if ch.islower() and i + 1 < len(pairs) and pairs[i + 1][0].islower():
            out.append(("#", pairs[i][1]))
            i += 2
            continue
        if ch.isalnum() or ch == "/":
            out.append((ch.upper(), pairs[i][1]))
        i += 1
    return out


def norm(s):
    return "".join(c for c, _ in norm_pairs([(ch, 0.0) for ch in s]))


def align(ref, hyp):
    """edit distance of ref against the best-matching stretch of hyp: (distance, start, end, right[start:end])"""
    n, m = len(ref), len(hyp)
    prev = [0] * (m + 1)                     # free start in hyp
    back = [bytearray(m + 1) for _ in range(n + 1)]
    for i in range(1, n + 1):
        cur = [i] + [0] * m
        back[i][0] = 1
        r, bi = ref[i - 1], back[i]
        for j in range(1, m + 1):
            d = prev[j - 1] + (r != hyp[j - 1])
            u = prev[j] + 1
            left = cur[j - 1] + 1
            if d <= u and d <= left:
                cur[j] = d
            elif u <= left:
                cur[j], bi[j] = u, 1
            else:
                cur[j], bi[j] = left, 2
        prev = cur
    end = min(range(m + 1), key=lambda j: prev[j])   # free end in hyp
    ok, i, j = {}, n, end
    while i > 0:
        b = back[i][j]
        if b == 0:
            ok[j - 1] = ref[i - 1] == hyp[j - 1]
            i, j = i - 1, j - 1
        elif b == 1:
            i -= 1
        else:
            ok[j - 1] = False
            j -= 1
    return prev[end], j, end, [ok.get(x, False) for x in range(j, end)]


def run(build, args):
    return subprocess.run([os.path.join(build, "vail_decode")] + args, check=True, capture_output=True, text=True,
                          errors="replace").stdout


def channels(out):
    rows = []
    for line in out.splitlines():
        m = re.match(r"\s*(\d+) Hz\s+(\S+)\s+(\S+): ?(.*)$", line)
        if m:
            rows.append((int(m[1]), float(m[3]), m[4]))      # pitch, highest Morse score, text
    return rows


def host(build, args):
    """vail_decode -m -q: [(char, confidence)] without the API's channel-switch marks, as compared"""
    out = run(build, ["-m", "-q"] + args)
    text, _, last = out[:-1].rpartition("\n")
    pairs, after_newline = [], False
    for ch, c in zip(text, (float(x) for x in last.split())):
        if after_newline and ch == ">":
            after_newline = False
            continue
        after_newline = ch == "\n"
        pairs.append((ch, c))
    return norm_pairs(pairs)


def score(args):
    build, case, root = args
    wav = os.path.join(root, case["name"] + ".wav")
    ref = norm(case["text"])
    rows = []
    for hz, peak, text in channels(run(build, ["-a", "-c", wav])):
        h = norm(text)
        rows.append((hz, peak, (align(ref, h)[0] if h else len(ref)) / len(ref)))
    res = {"name": case["name"], "n": len(ref), "chans": len(rows)}
    if not rows:
        rows = [(0, 0.0, 1.0)]
    res["best"] = min(rows, key=lambda r: r[2])
    res["by_score"] = max(rows, key=lambda r: r[1])
    for kind, extra in (("host", []), ("pinned", ["-p", str(res["best"][0])] if res["best"][0] else [])):
        hp = host(build, extra + [wav])
        hyp = "".join(c for c, _ in hp)
        d, s, e, ok = align(ref, hyp)
        conf = [c for _, c in hp[s:e]]
        res[kind] = {"cer": d / len(ref), "text": hyp[s:e], "ok": ok,
                     "wrong": [c for c, o in zip(conf, ok) if not o], "right": [c for c, o in zip(conf, ok) if o]}
    return build, res


def shares(wrong, right):
    if not wrong or not right:
        return "(too few characters)"
    cells = " ".join(f"<{t} {100 * sum(c < t for c in wrong) / len(wrong):.0f}/{100 * sum(c < t for c in right) / len(right):.0f}"
                     for t in THRESHOLDS)
    return f"wrong {len(wrong)}, right {len(right)}, AUC {auc(wrong, right):.3f}; wrong/right % below {cells}"


def report(build, res, show):
    print(f"\n== {build}")
    print(f"{'file':<24} {'chars':>5} {'chans':>5}  {'best Hz':>7} {'CER':>4}  {'by score':>8} {'CER':>4}  "
          f"{'host':>4}  {'pinned':>6}  {'wrong<3':>8}  {'right<3':>8}")
    tot = dict.fromkeys(("best", "by_score", "host", "pinned"), 0.0)
    n = 0
    for r in res:
        p = r["pinned"]
        print(f"{r['name'][:24]:<24} {r['n']:>5} {r['chans']:>5}  {r['best'][0]:>7} {100 * r['best'][2]:>3.0f}%  "
              f"{r['by_score'][0]:>8} {100 * r['by_score'][2]:>3.0f}%  {100 * r['host']['cer']:>3.0f}%  "
              f"{100 * p['cer']:>5.0f}%  {sum(c < 3 for c in p['wrong']):>3}/{len(p['wrong']):<4}  "
              f"{sum(c < 3 for c in p['right']):>3}/{len(p['right']):<4}")
        n += r["n"]
        tot["best"] += r["best"][2] * r["n"]
        tot["by_score"] += r["by_score"][2] * r["n"]
        tot["host"] += r["host"]["cer"] * r["n"]
        tot["pinned"] += p["cer"] * r["n"]
    print(f"CER over {n} characters: best channel {100 * tot['best'] / n:.1f} %, highest Morse score "
          f"{100 * tot['by_score'] / n:.1f} %, host {100 * tot['host'] / n:.1f} %, pinned {100 * tot['pinned'] / n:.1f} %; "
          f"the highest score is the best channel in {sum(r['by_score'][0] == r['best'][0] for r in res)} of {len(res)}")
    good = [r for r in res if r["pinned"]["cer"] < DECODABLE]
    for label, rs in (("pinned, all files", res), (f"pinned, {len(good)} decodable files", good)):
        print(f"{label}: " + shares([c for r in rs for c in r["pinned"]["wrong"]],
                                    [c for r in rs for c in r["pinned"]["right"]]))
    if show:
        for r in res:
            p = r["pinned"]
            print(f"\n{r['name']} pinned at {r['best'][0]} Hz (wrong characters in brackets):")
            print("".join(c if o else "[" + c + "]" for c, o in zip(p["text"], p["ok"])))


def main():
    argv = sys.argv[1:]

    def opt(name, default):
        if name in argv:
            i = argv.index(name)
            value = argv[i + 1]
            del argv[i:i + 2]
            return value
        return default

    builds = opt("--builds", os.environ.get("VAIL_BUILD", "build")).split(",")
    filt = opt("--filter", "")
    jobs = int(opt("--jobs", str(os.cpu_count() or 1)))
    show = "--show" in argv
    do_fetch = "--fetch" in argv
    argv = [a for a in argv if a not in ("--show", "--fetch")]
    if len(argv) != 1:
        sys.exit(__doc__)
    root = argv[0]
    cases = [c for c in load() if filt in c["audio"]]
    if do_fetch:
        fetch(cases, root)
    missing = [c["name"] + ".wav" for c in cases if not os.path.exists(os.path.join(root, c["name"] + ".wav"))]
    if missing:
        sys.exit(f"missing in {root}: {', '.join(missing)} (--fetch downloads them)")
    with Pool(jobs) as pool:
        out = pool.map(score, [(b, c, root) for b in builds for c in cases])
    for b in builds:
        report(b, [r for bb, r in out if bb == b], show)


if __name__ == "__main__":
    main()
