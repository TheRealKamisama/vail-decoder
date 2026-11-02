# Close signals: a known limit of the engine

**Status: open.** When two stations are closer than about 200 Hz, the engine loses much of their text, usually
the weaker station's. The spectrum has the information: the reference keying detector separates the same signals
almost perfectly. The likelihood detector, which gives the engine its weak-signal sensitivity, does not. The cause
is known and there is a candidate fix, but it is not ready. The engine's code has not been changed.

This matters most to hosts that decode many signals at once, such as `cli/`. On a busy band, stations 100 Hz
apart are normal.

## Reproduce

```sh
./build.sh
./build/cwgen -o x.wav -t "CQ CQ CQ DE K1ABC K1ABC K1ABC PSE K" -w 25 -f 800 -n 15 -r 1 \
              -S "900:18:0:TEST W1AW W1AW TEST W1AW W1AW TEST"
./build/vail_decode -a x.wav
```

```
  794 Hz: CQ CQ CQ DE KL OAB  R K1ABC K . MABC PSE K
  895 Hz: TEST WW ? ? . TOT O N WW ? ? = MW TEST WL W A W V , A W TEST
```

With the second station at 1100 Hz (`-S "1100:18:0:..."`), both are copied correctly.

## Measurements

`python3 tools/closepair.py --builds A,B,...` measures this. The first station is at 800 Hz, 25 WPM, 15 dB SNR in
2500 Hz, and the second is above it at 18 WPM. Each cell is the mean CER of the first/second station in percent, over
three text pairs and two seeds, with word spacing not scored.

Second station level with the first:

| spacing (Hz) | 60 | 80 | 100 | 150 | 200 | 300 |
| --- | --: | --: | --: | --: | --: | --: |
| engine today | 8/59 | 12/84 | 12/82 | 1/39 | 3/0 | 3/0 |
| reference detector (`-DENV_TUNE_LIK=0`) | 1/1 | 0/0 | 0/0 | 0/0 | 0/1 | 0/1 |
| candidate fix | 40/8 | 18/16 | 4/2 | 2/143 | 3/0 | 2/0 |

Second station 15 dB below the first:

| spacing (Hz) | 60 | 80 | 100 | 150 | 200 | 300 |
| --- | --: | --: | --: | --: | --: | --: |
| engine today | 0/55 | 4/18 | 5/13 | 0/8 | 0/0 | 0/0 |
| reference detector (`-DENV_TUNE_LIK=0`) | 0/76 | 0/12 | 0/0 | 0/0 | 0/1 | 0/0 |
| candidate fix | 0/56 | 0/4 | 0/1 | 0/1 | 0/0 | 0/0 |

`vail_decode -a` prints every channel that produced text, including sidelobe ghosts of a strong signal. A ghost
within 40 Hz of a station is scored as that station's text, which is how a cell can exceed 100 %. `cli/` hides such
ghosts, so through it these cases read somewhat better, but the letters of the real stations are the same.

The reference detector is not a way out: it gives up the 4.4 dB of weak-signal sensitivity described in
[weak-signal.md](weak-signal.md).

## Why the grids did not show it

`eval.py`'s `qrm50`, `qrm100` and `qrm200` rows have a neighbouring station, and they score 0–2 %. But the neighbour
is 6 dB weaker and only the stronger station is scored, which is the station this problem spares. `weak.py`'s `qrm`
condition has a neighbour 120 Hz away and 3 dB weaker, and also scores only one station. `closepair.py` scores both.

## The cause

`spm_noise_sides` (`src/sigext.c`) takes a frame's broadband level `broad` as the median of the 22 bins at offsets 6
to 16 either side of the channel. `frame_noise` treats a frame as impulsive when `broad` is more than
`LIK_QRN_RATIO` times the noise and more than `LIK_QRN_REL` of the centre bin. This is how the detector rides out
static crashes.

A strong neighbour fills most of one side, which is enough to lift that median. Whenever the neighbour keys down,
this channel's key-up frames pass both tests: they are scored against a noise level that is far too high, and they
do not train the model. The key-up evidence nearly vanishes, so gaps are lost and characters run together. A key-down
frame of the stronger station fails the `LIK_QRN_REL` test, which is why it survives.

The evidence is that changing nothing but `broad` restores most of the copy, as the candidate fix shows, and that the
reference detector, which has no such test, is unaffected.

## Candidates

Both change only `broad`. They were run against the current engine on `tools/weak.py` (1440 cases, on identical audio)
and `tools/eval.py --seeds 4` (464 cases).

| | weak grid, mean CER | SNR at 10 % CER | original grid | largest changes on the original grid |
| --- | --: | --: | --: | --- |
| engine today | 23.15 % | −8.2 dB | 0.2 % | |
| median of each side, take the lower | 22.71 % | −8.3 dB | 1.2 % | `qrn2` 0 → 51.6, `qrn8` 0 → 12.1, `w12_s10` 0 → 9.3 |
| median over offsets 6 to 48 (the candidate fix) | 22.75 % | −8.3 dB | 1.0 % | `w12_s10` 0 → 11.8, `w20_s10` 0.4 → 6.1, `qrn8` 0 → 4.0, `qrm50` 1.7 → 0 |

The first fails because a static burst is not spectrally flat, so requiring both sides misses it. The second is
better, since a neighbour fills few of its bins while a crash lifts most of them, but it is **not ready**:

- `w12_s10` and `w20_s10`, a single strong station, get worse, and why has not been found.
- `qrn8` gets worse.
- At 60–80 Hz it trades the second station's errors for the first's, and at 150 Hz it adds a ghost near a station.

```diff
--- a/src/sigext.c
+++ b/src/sigext.c
@@ spm_noise_sides
-    float v[2 * (LIK_NB_LAST - LIK_NB_FIRST + 1)];
+    float v[2 * (LIK_BROAD_LAST - LIK_NB_FIRST + 1)];     /* LIK_BROAD_LAST = 48 */
     double l = 0.0, r = 0.0;
     int nl = 0, nr = 0, k, n = 0;
-    for (k = LIK_NB_FIRST; k <= LIK_NB_LAST; k++) {
-        if (c - k >= 0) { l += sp[c - k]; nl++; v[n++] = sp[c - k]; }
-        if (c + k < N_BINS) { r += sp[c + k]; nr++; v[n++] = sp[c + k]; }
+    for (k = LIK_NB_FIRST; k <= LIK_BROAD_LAST; k++) {     /* a neighbouring signal fills few of these bins */
+        if (c - k >= 0) { v[n++] = sp[c - k]; if (k <= LIK_NB_LAST) { l += sp[c - k]; nl++; } }
+        if (c + k < N_BINS) { v[n++] = sp[c + k]; if (k <= LIK_NB_LAST) { r += sp[c + k]; nr++; } }
     }
```

## What a fix has to do

- Improve `tools/closepair.py` without regressing `tools/weak.py` and `tools/eval.py --seeds 4`, in particular the
  `qrn` rows and the strong-signal `w*_s10` rows.
- Be a `*_TUNE_*` switch, as for the engine's other departures, and be recorded in `12-open-items.md` §E.
- Then tighten the limits of the close-pair cases in `cli/tests/e2e.py`, which today only guard the current numbers.
