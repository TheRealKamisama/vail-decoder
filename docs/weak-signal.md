# Weak-signal decoding

This note describes the changes that let vail copy CW roughly 4.4 dB further down into the noise at
the speeds that matter in practice (18–30 WPM), and how they were measured. Every change is a
compile-time switch. Setting the switch to `0` restores the reference behaviour, as for the other
`*_TUNE_*` switches.

## Results

The weak-signal grid is `tools/weak.py --full`: 4200 cases at 18, 22, 25, 28 and 30 WPM, at SNRs from −4 to
−14 dB in 2500 Hz, with ten texts (QSO phrases, a pangram, random five-character groups) and two seeds.
It covers seven conditions:

| Condition | What it simulates |
| --- | --- |
| `clean` | keyer timing (2 % jitter), white noise |
| `hand` | hand keying: 8 % jitter, dashes 3.2 dots, letter and word gaps × 1.25 |
| `qsb` | Rayleigh fading with 0.25 Hz Doppler spread |
| `rx` | noise through a 350–1100 Hz receiver filter, 12 Hz drift |
| `qrm` | a second station 120 Hz away, 3 dB weaker |
| `qrn` | static crashes, 1.5 per second |
| `drift` | 40 Hz drift over the message |

CER is the Levenshtein distance on upper-cased text with runs of spaces collapsed, capped at 100 % per
case. "SNR@x" is the SNR at which the mean CER over all conditions rises above x %.

| | before | after |
| --- | ---: | ---: |
| mean CER over the grid | 57.0 % | **21.4 %** |
| SNR@10 % | −4.3 dB | **−8.9 dB** |
| SNR@25 % | −6.5 dB | **−11.0 dB** |

| SNR | clean | hand | qsb | rx | qrm | qrn | drift |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| −6 dB | 12.3 → 0.5 | 10.8 → 1.8 | 51.8 → 24.3 | 9.0 → 1.1 | 11.7 → 1.1 | 15.6 → 0.8 | 13.7 → 0.6 |
| −8 dB | 45.7 → 2.7 | 48.4 → 2.6 | 62.6 → 29.3 | 42.9 → 2.0 | 49.2 → 2.9 | 51.1 → 3.1 | 46.5 → 2.2 |
| −10 dB | 89.5 → 8.2 | 89.3 → 9.2 | 71.3 → 41.7 | 86.9 → 9.8 | 86.7 → 10.7 | 89.8 → 10.9 | 87.9 → 10.4 |
| −12 dB | 90.7 → 32.4 | 88.9 → 32.6 | 78.5 → 52.4 | 90.8 → 28.0 | 82.9 → 38.8 | 90.1 → 35.0 | 87.7 → 34.8 |
| −14 dB | 95.7 → 62.8 | 94.0 → 66.0 | 86.9 → 65.5 | 95.1 → 59.4 | 90.5 → 65.4 | 95.8 → 64.9 | 93.9 → 64.3 |

Most of the remaining fading loss is physical. About 80 % of the missed marks at −4 dB fall in fades more
than 10 dB deep, where the instantaneous SNR is below −14 dB.

Other checks:

- **The original grid** (`tools/eval.py --seeds 4`, 464 cases, 12–40 WPM): 3.68 % → **0.22 %** overall,
  and 1.57 % → 0.20 % without 40 WPM. Noiseless input, static crashes and 40 WPM at 0 / −3 dB, which were
  7–35 %, are now about 1 %. No group is worse than before.
- **Alternating overs**: stations at 20/28, 25/18 and 22/22 WPM take turns, 0, 15 or 60 Hz apart. The CER
  of the host text built from the delete counts (`vail_decode -m`) is 78–82 → 8–9 % at −10 dB, 32–36 →
  2–2.6 % at −8 dB, and 1–4 → 0.5–0.7 % at −4 dB.
- **Input level**: the same files 30 dB quieter decode the same (10.6 → 10.7 % at −10 dB).
- **False text**: 6 minutes of white noise produce no characters. 6 minutes of noise through a narrow
  receiver filter produce 2 characters on a noise channel at a filter edge, none of them in the current
  channel.
- **Cost**: about 350× real time on one core (it was about 1200×). A live channel takes about 180 KB (it
  was about 55 KB).

## What changed, in pipeline order

### FE: a narrower analysis window (`FE_WINDOW_CUTOFF`)

The window's sinc factor is narrowed from 0.003125 to 0.0023 cycles per sample. The noise bandwidth
goes from 42 Hz to about 30 Hz, which is closer to a matched filter for the 40–60 ms elements of 20–30
WPM. Narrower still helps 18–25 WPM but costs at 30 WPM. The frames are still 10.6 ms apart, and a
one-unit gap at 30 WPM still leaves three clean key-up frames. The flat top is now ±10 Hz, so the
channel must sit on the signal's bin. See keyed peak tracking below.

### ENV: a likelihood keying detector (`ENV_TUNE_LIK`)

The reference ENV compares a prominence with an adaptive threshold. That threshold can only come down
when a sample exceeds 3.0. Below about −9 dB the peaks never do, so the threshold stays at its ceiling
and nothing is decoded. That was the cliff in the "before" column. The new detector works in
probabilities:

- **Observation model.** Noise-only power in a bin is exponential. Key-down power is Rician with
  per-frame SNR `s`. The per-frame log-likelihood ratio is `−s + ln I0(2√(s·y))` for the power `y` in noise
  units. It is scaled by κ = 1 / (1 + 2 Σ ρ²), where ρ is the power correlation of overlapping frames
  computed from the window. SEG gets this ratio as log-odds, so its HMM weighs evidence correctly against
  its duration prior.
- **Noise.** The bin's own key-up average (posterior-weighted, with leaked mark-edge power capped) is
  kept between the mean levels of the bins 6–16 to either side. A neighbouring station raises one side,
  and a receiver filter slope lowers one side. Neither moves the estimate outside the band set by the two
  sides. The noise never goes below 50 dB under the strongest bin, so digital silence or key-click
  splatter does not look like a signal.
- **SNR.** An EM step on 3-frame posteriors, with a triangular window of ±35 frames around the output
  frame. It follows fading without a short, noisy window.
- **Static crashes and clicks.** A frame whose side bins (median) are raised well above the noise and to
  within 13 dB of the centre bin is scored against its own broadband level. Its SNR drops instead of its
  power being normalised away.
- **Key edges.** The first frame of a mark and the first frame of a gap see about half of the tone through
  the window. SEG's rise/fall states score them with a reduced key-down power (`LIK_EDGE_GAIN`).
- **Carrier** is the SNR estimate with hysteresis (1.0 on, 0.6 off, per frame).
- **Keyed peak tracking** (`LIK_TUNE_PEAK`). SPM follows a signal only when its peak ratio exceeds 7.5,
  which a weak signal never reaches. So a weak channel stays where it was born, often a bin or two off,
  which costs up to 3 dB with the narrower window. The key-down excess of the bins ±3 around the centre
  is summed with the key-down posterior as weight. When a peak 1–2 bins away beats the centre by 15 %,
  the channel moves one bin. Excess still rising 3 bins away is the skirt of another signal and is left
  alone. Without that rule, channels beside a strong station walk into it. This gives 40 Hz drift at
  −14 dB 77 → 65 %, and two stations 15 Hz apart in one channel 10.9 → 9.4 % at −10 dB.

### SEG: a normalised duration model (`SEG_TUNE_NORM_DUR`)

The reference hazards come from unnormalised class scores, which leave about half of the mark
probability "beyond" the last state. As a result a mark of any length cost almost nothing to extend.
With saturated key probabilities this never mattered. With calibrated evidence the HMM merged
dash-gap-dash into one long mark. The hazards now come from the pmf normalised over 400 frames. The
absorbing states use the geometric tail of the same mean residual length. The state ranges grow to 30
(marks) and 60 (gaps) frames, so that word gaps at 20–25 WPM are modelled instead of falling into the
pause tail.

### SPD: a speed bank (`SPD_TUNE`)

At low SNR the SEG ↔ STA loop can lock onto a wrong speed. Merged elements look like a slower sender,
which merges more. At −12 dB a 30 WPM signal ended up at 17 WPM. Fifteen fixed-timing duration HMMs from 12
to 45 WPM run on SEG's input, plus one with SEG's current tables. If a fixed speed explains the last few
seconds better than the current model by 8 nats (forward likelihood: a best-path score would let every
model explain weak marks away as one long pause), STA restarts from that speed.

### CVD: a character Viterbi decoder (`CVD_TUNE`)

This is the largest single gain: about 1 dB, and more on hand-sent code. Instead of segmenting
elements first and looking characters up afterwards, CVD decodes characters directly from the per-frame
input. Its duration HMM is laid out as a trie of the code table. Every node has a mark chain (dot or
dash durations) and an element-gap chain, and a letter/word-gap chain links the codes. Element
boundaries, gap classes and character identities are decided together, and only table codes can come
out. Durations come from STA's model, so SEG and STA keep running to learn the timing. Histories are
passed as tokens and collected by mark-and-sweep. A fixed-lag traceback (`CVD_LAG`, 80 frames) commits
characters in CHR's output format, so WRD, MSG and the host interface are unchanged.

The character prior is a trigram, `src/lm_table.h`. It is generated by `tools/lmgen.py` from a synthetic
corpus of QSO, contest and plain-English text, written independently of the evaluation texts. Its weight
(0.5 against the table weights, `CVD_LM_BIGRAM`) is a compromise. QSO text gains about 2 points of CER.
Random five-letter groups stay within a point of the unigram. The previous and the second previous codes
are taken as a word start with the probability that the gap before them was a word gap.

### SIG: re-decoding after learning (`SIG_TUNE_REPLAY`, `SIG_TUNE_OVER`)

Until STA has learned the timing, SEG runs on the default 23 WPM prior. When learning ends, the frames
kept since the first element are segmented and decoded again with the learned model. The first tenth of a
message then has 5 % CER at −8 dB and 14 % at −10 dB, against 2–3 % and 7–10 % mid-message. Decoding it
straight on with the default prior gives 7 % and 25 %. Each later over (after a carrier drop) is handled
the same way. CVD holds its output until STA has re-estimated on the new over. If the speed
moved by more than 8 %, CVD decodes the over again from its start. Without this, a second station at
another speed on the same channel lost its first word.

### WRD: gap evidence weighs more (`WRD_WS_SLOPE`)

The word-space bonus maps the gap's word-gap probability through `tanh(slope · (p − ½))`. With the
spec's slope of 6 the templates overrode clear letter gaps, and digit strings such as `1234567890` came
out in groups. With slope 10 that stops. The original grid goes from 0.96 % to 0.22 %, because most of
its remaining errors were spacing, and the weak-signal mean improves by about half a point.

### BCM: text-rate gating (`BCM_TUNE_TEXT_RATE`)

A channel becomes current only while it produces text at a sustained rate (a leaky count of new
characters ≥ 1.5). Otherwise a noise channel's odd character, for example at a receiver filter edge,
takes over the display when the real station stops.

## Tools

| Tool | Purpose |
| --- | --- |
| `tools/weak.py [--full] [--builds A,B]` | the weak-signal grid above, in parallel; builds are compared on identical audio |
| `tools/elemcheck.py` | element errors (missed, spurious, split, merged, wrong kind, gap class) against the generator's ground truth |
| `tools/lmgen.py` | regenerates `src/lm_table.h` |
| `tools/eval.py` | the original grid, now in parallel |
| `cwgen -R / -B / -W / -G / -K` | Rayleigh fading, receiver filter, dash weight, gap stretch, ground-truth key intervals |
| `vail_decode -E file` | every classified element of every channel, for `elemcheck.py` |

`cwgen` no longer clips at low SNR. The noise is fixed at −20 dBFS and the signal follows the SNR.
Before, the noise grew with falling SNR and was clipped hard below about −6 dB.

## What did not help

These were measured and dropped:

- a bias towards key-down, or a penalty per mark: either way more errors;
- a looser or tighter STA variance floor: 0.15 is the optimum;
- an emission model that allows the key-down level to vary (a mixture over s/3, s and 3s);
- a shorter SNR window, or switching to one when fading is detected: better under QSB but worse
  everywhere else.

Merging two dots into a dash (a missed one-unit gap) is now the main error near the threshold. It is
close to the detection limit of a one-unit gap at these SNRs. Further gains would need word-level
context: a dictionary, callsign structure, repetition.

## Known issues

- WRD's callsign grammar can join an alphanumeric word to the next one across a clear word gap
  (`IC7300 PWR` → `IC7300PWR`). The reference decoder does the same, and the size of the word-space
  bonus does not change it.
- At −14 dB the speed estimate can still settle at half speed.
