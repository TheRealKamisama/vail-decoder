# Close signals

**Status: fixed for stations of similar strength down to about 60 Hz apart, and for a station 15 dB below its
neighbour down to about 80 Hz.** What is left is listed under [What remains](#what-remains). All of these numbers come
from synthetic audio; the fix has not been measured on a recording of a real band.

This matters most to hosts that decode many signals at once, such as `cli/`. On a busy band, stations 100 Hz apart
are normal.

## Reproduce

```sh
./build.sh
./build/cwgen -o x.wav -t "CQ CQ CQ DE K1ABC K1ABC K1ABC PSE K" -w 25 -f 800 -n 15 -r 1 \
              -S "900:18:0:TEST W1AW W1AW TEST W1AW W1AW TEST"
./build/vail_decode -a x.wav
```

```
  794 Hz: CQ CQ CQ DE K1ABC K1ABC K1ABC PSE K
  895 Hz: TEST W1AW W1AW TEST W1AW W1AW TEST
```

Built with `-DLIK_TUNE_OWN_NOISE=0 -DBCM_TUNE_KEEP_ON_SWITCH=0`, the same audio gives the engine's earlier result:

```
  794 Hz: CQ CQ CQ DE KL OAB  R K1ABC K . MABC PSE K
  895 Hz: TEST WW ? ? . TOT O N WW ? ? = MW TEST WL W A W V , A W TEST
```

## Measurements

`python3 tools/closepair.py --builds A,B,... --seeds 4` measures this. The first station is at 800 Hz, 25 WPM, 15 dB
SNR in 2500 Hz, and the second is above it at 18 WPM. Each cell is the mean CER of the first/second station in percent,
over three text pairs and four seeds, with word spacing not scored.

Second station level with the first:

| spacing (Hz) | 60 | 80 | 100 | 150 | 200 | 300 |
| --- | --: | --: | --: | --: | --: | --: |
| both switches off | 5/56 | 13/80 | 14/75 | 2/42 | 3/0 | 3/0 |
| **engine** | **0/0** | **0/0** | **0/0** | **0/5** | **0/0** | **0/0** |
| reference detector (`-DENV_TUNE_LIK=0`) | 1/1 | 0/0 | 0/0 | 0/0 | 0/0 | 0/0 |

Second station 15 dB below the first:

| spacing (Hz) | 60 | 80 | 100 | 150 | 200 | 300 |
| --- | --: | --: | --: | --: | --: | --: |
| both switches off | 1/52 | 4/24 | 6/14 | 0/10 | 0/0 | 0/0 |
| **engine** | **0/48** | **0/3** | **0/1** | **0/0** | **0/0** | **0/0** |
| reference detector (`-DENV_TUNE_LIK=0`) | 0/79 | 0/12 | 0/1 | 0/0 | 0/1 | 0/0 |

`vail_decode -a` prints every channel that produced text, and a channel within 40 Hz of a station is scored as that
station's text. The 5 at 150 Hz is such a channel, in 2 of the 12 cases: both stations' own channels are correct in all
12. `closepair.py` counts these ghost letters separately. They are 0 in every cell of both tables, before and after,
except that 150 Hz cell, which goes from 1.2 to 1.3 letters per case on the same channel (see
[What remains](#what-remains)).

The same comparison on cases `closepair.py` does not have, each with four seeds and scoring only each station's own
channel. Levels are chosen so that cwgen clips at most a few samples, because several strong signals can add up to more:

| case | stations | both switches off | engine |
| --- | --- | --: | --: |
| three, 100 Hz apart | 5 dB SNR | 3/24/7 | 0/0/0 |
| three, 80 Hz apart | 5 dB SNR | 4/15/7 | 0/0/0 |
| four, 100 Hz apart | 3 dB SNR | 1/0/15/20 | 0/0/2/6 |
| 100 Hz, runs of digits | 10 dB SNR | 136/105 | 0/0 |
| 100 Hz, both 12 WPM | 10 dB SNR | 18/62 | 0/0 |
| 100 Hz, both 35 WPM | 10 dB SNR | 11/27 | 0/0 |
| 100 Hz, second starts late | 10 dB SNR | 13/11 | 0/0 |
| 100 Hz, weak | 0 dB SNR | 2/3 | 0/0 |
| 100 Hz, very weak | −5 dB SNR | 2/3 | 1/2 |
| 100 Hz, second 15 dB stronger | 0 / 15 dB SNR | 8/21 | 0/0 |
| 80 Hz, second 6 dB weaker | 15 / 9 dB SNR | 13/32 | 0/0 |

No regression on the engine's grids, on identical audio. Differences under about 0.3 % in the weak mean or 0.5 % on the
original grid are noise:

| grid | both switches off | engine |
| --- | --: | --: |
| `tools/weak.py` (1440 cases), mean CER | 23.15 % | 23.16 % |
| SNR at 10 % / 25 % CER | −8.2 / −10.8 dB | −8.2 / −10.8 dB |
| `tools/weak.py --cond qrm,qrn,drift` (1080 cases), mean CER | 19.73 % | 19.74 % |
| SNR at 10 % / 25 % CER | −9.6 / −11.1 dB | −9.6 / −11.1 dB |
| `tools/eval.py --seeds 4` (464 cases), overall CER | 0.2 % | 0.2 % |

Three of the original grid's 37 rows change: `qrm50` 1.7 → 0.0, `jit0.2_w28` 3.3 → 2.9, and `jit0.2_w18` 2.1 → 3.3. The
`qrn` and `w*_s10` rows do not move. The largest change in a weak grid cell is `hand` at −10 dB, 9.3 → 10.0. With both
switches at 0, `eval.py`'s full output is byte-identical to the engine before the change, and both weak grids match it.

## Why the grids did not show it

`eval.py`'s `qrm50`, `qrm100` and `qrm200` rows have a neighbouring station, and they score 0–2 %. But the neighbour
is 6 dB weaker and only the stronger station is scored, which is the station this problem spares. `weak.py`'s `qrm`
condition has a neighbour 120 Hz away and 3 dB weaker, and also scores only one station. `closepair.py` scores both.

## The cause

Three separate faults, the first two hidden by the same thing.

The likelihood detector needs the noise power in its bin. It uses the bin's own key-up average, kept between the mean
levels of the bins 6 to 16 either side (`lo` and `hi` in `env_apply_lik`, `src/sigext.c`). For a lone station `hi` is
about the true noise, so the clamp holds the estimate down whatever the average does. A neighbour 50 to 150 Hz away
puts its main lobe among one side's bins and lifts `hi` by up to some 200 times. That removes the ceiling, and two
weaknesses of the average then show:

1. **The fallback.** With too few key-up frames (`goSum` ≤ 8), which a run of dashes causes, the noise fell back to
   `hi`. In the digit `1` of `W1AW`, at 100 Hz, it jumped from about 1 to 221 in units of the true noise.
2. **The mark edges.** A frame on the edge of a mark has far less power than a key-down frame is expected to have, so
   the model calls it key-up, and it entered the average capped at 10 times `lo`. In a run of dashes these frames
   dominate. In the `O` of `OM`, at 150 Hz, the own estimate went from 0.9 to 5.7 in 0.2 s, with the true noise still
   about 1.

Either way the power in noise units, `y`, drops while the SNR estimate `s`, with its memory of ±35 frames, does not.
The log-likelihood ratio of key-down goes negative below about `y = s/4`, so real marks are called key-up. They then
enter the average as well, which makes it self-sustaining for most of a second. This is why the errors land on dashes:
`K1ABC` → `KL OABC`, `W1AW` → `WW ? ?`.

The third fault is independent, and is why the first station lost letters even at 300 Hz. When the current channel
changed, `bcm_set_current` flushed the old one, a false carrier drop in the middle of what it was decoding:
`CQ CQ CQ` → `CQ C  A CQ`. With two stations of similar quality, that happens early in almost every case.

### What the cause was not

This document used to blame the broadband test in `frame_noise`, on the grounds that a neighbour lifts the median of
the side bins and makes key-up frames look impulsive. A per-frame trace of the 100 Hz pair refutes that as the cause.
The test does fire far more often (38–40 % of key-up frames against 7–11 % with the neighbour at 300 Hz), but the mean
log-odds of those frames barely change. Changing `broad` only helped indirectly.

The two candidates that changed `broad` are rejected. With the noise hold in place, the median over offsets 6 to 48 is
worse than the unchanged test (40/8 at 60 Hz and 2/142 at 150 Hz, against 9/5 and 2/11, on two seeds).

Its regression on the `w*_s10` rows was not a keying problem. On `w12_s10`, 19 of the 20 cases are perfect, and the
whole row's 11.8 % is one case where the real station at 692 Hz still decodes perfectly, while a channel on its skirt at
653 Hz produces a stream of `E` and `5` and is picked as the current channel, the only one `eval.py` scores. So the
narrow median was also silencing skirt channels, which is a reason to leave it alone.

The earlier explanation of the other candidate's `qrn2` loss, that a static burst is not spectrally flat, was wrong
about the test audio: cwgen's bursts are white. Since that candidate is rejected, the real reason was not pursued.

## The fix

Both are `*_TUNE_*` switches, on by default, and 0 restores the earlier behaviour. `src/` gains no dependency.

**`LIK_TUNE_OWN_NOISE`** (`src/sigext.c`) makes the own average sound without the ceiling. Neither half is enough alone:
without the hold, rejecting frames drains the key-up weight onto the raised fallback, and the table is far worse (19/65
at 100 Hz, measured with `LIK_OWN_REJECT` at 5).

```c
if (L->goSum > 8.0) L->noiseHeld = L->noiseOwn;
noise = L->goSum > 8.0 ? L->noiseOwn : (LIK_TUNE_OWN_NOISE && L->noiseHeld > 0.0f ? L->noiseHeld : hi);
...
if (LIK_TUNE_OWN_NOISE && L->x[u & M] > LIK_OWN_REJECT * noise) go = 0.0;
```

`LIK_OWN_REJECT` is 12: exponential noise exceeds 12 times its mean about once in 160000 frames, so no real noise is
lost and the estimate is not biased. A lower value also starts to cost weak signals, while the level pair does not care:

| `LIK_OWN_REJECT` | 5 | 8 | **12** | 20 | 40 |
| --- | --: | --: | --: | --: | --: |
| weak grid, mean CER (off: 23.15 %) | 23.37 % | 23.25 % | **23.16 %** | 23.15 % | |
| level pair at 60–100 Hz | 0/0 | 0/0 | **0/0** | 0/0 | 0/0 |
| −15 dB pair at 80 / 100 Hz | 0/1, 0/0 | 0/1, 0/0 | **0/3, 0/1** | 0/3, 0/2 | 0/4, 0/3 |

The clamp to `lo`..`hi` still applies, so noise that falls, or rises on both sides, moves the estimate. Noise that rises
on one side only does not lift `lo`, and the rejection then stops the estimate re-training. That makes no difference up
to 25 dB over the floor, but at 30 dB it costs 1.5–2.3 % against 0–0.8 %. See
[review-close-signals-and-cli.md](review-close-signals-and-cli.md).

**`BCM_TUNE_KEEP_ON_SWITCH`** (`src/bcm.c`) leaves a channel undisturbed when it stops being current. The flush is the
reference behaviour (`03 §4.10`), but no grid depends on it. Without this switch the first station keeps 2 to 4 % of
errors at every spacing.

These departures belong in `12-open-items.md` §E where the document set is kept, with the switch names and the numbers
above.

## What remains

- **A weak station at 60 Hz.** 15 dB below its neighbour, the second station scores 48 %. Its text is usually correct but
  starts late (`W1AW TEST`, `AA1ZZZ UP 5 K`), and in a few cases it is whole. `BCM` compares a peak with the bins 5 away
  on either side, and at 60 Hz one of them is on the strong station's main lobe, so a channel usually starts only when
  that station stops. The keying detector is not the limit here, and the reference detector is worse (79 %).

  A separate birth test for bins 6 to 8 from a decoding channel took this cell to 0/1, but it was **not adopted**. In
  noiseless audio it started channels in the tail of a station ending, which added an 8-letter ghost to the `four-clean`
  case of `cli/tests/e2e.py`. A real station started at −18 dB from its neighbour and the false ones at −13 to −25 dB,
  so strength does not separate them.
- **A weak station between two stronger ones.** The figures above are for one neighbour. With neighbours on both sides
  `lo` rises too, and the fix does not help: 8 to 12 dB below them at 100 to 120 Hz, the station loses 80 to 86 %, about
  the same as before. From 150 Hz it is clean.
- **Closer than 60 Hz.** At 40 Hz, level, the second station still loses 11 % (10 dB SNR).
- **A very weak station beside a very strong one.** 25 dB down at 80 to 100 Hz, and at −10 dB SNR, the weak station is
  not copied (84 % and 79 %, unchanged by the fix).
- **A packed band.** With four stations 100 Hz apart, the two outermost still lose 2 and 6 % (from 15 and 20 %).
- **Ghosts on a strong signal's skirt.** A channel started by noise a few bins from a station can decode that station's
  key clicks, such as the 981 Hz channel behind the 150 Hz cell. It predates this work and also appears with a lone
  station on the same seed. `cli/` hides such ghosts.
- **cwgen clips several strong signals.** It limits each signal to half of full scale, not their sum, so two at 15 dB
  SNR clip about 0.4 % of their samples, and three clip far more, with visible intermodulation channels (`2f1 − f2`).
  This is why the extra table above uses lower levels. `closepair.py`'s level table has the mild form. cwgen was not
  changed, since that would change the audio of existing grid rows.
