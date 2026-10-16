# Comparison with libmorsedec

Tested on 2026-10-17 against [libmorsedec](https://github.com/goodspeed34/libmorsedec) v0.2.0
(commit `27e96ea`), a GPL-3.0 C11 streaming CW decoder with a very similar design. No libmorsedec code
is part of this repository.

## Summary

- On identical audio, vail has a **3.7 %** character error rate and libmorsedec **10.4 %**
  (464 cases).
- The speed is about the same (≈ 1,400× real time on one core). vail allocates about 55 KB per live
  channel. libmorsedec reserves about 149 KiB for each of 32 preallocated slots (≈ 4.9 MB).
- libmorsedec does better under heavy impulse noise (4.0 % vs 7.3 %). This comes from its
  tonal-contrast confidence cap. Moved into vail, the cap helps impulse noise but costs more at
  40 WPM than it gains (overall 3.7 % → 4.4 %), so it is not adopted.

## Method

- **Same front end.** At 8 kHz, libmorsedec's automatic geometry matches vail exactly: a 1024-point
  FFT, an 85-sample hop, and 128 bins from 203.125 Hz.
- **Build.** libmorsedec was built unmodified. FFTW was replaced by a small exact double-precision
  DFT shim, which agrees with a direct DFT to about 1e-14. Two shim variants gave identical results in
  every test group.
- **Harness.** A small program feeds the WAV and then 4 s of silence, calls `md_flush`, and prints
  the current channel's text, which mirrors `vail_decode`.
- **Corpus.** Both decoders ran through `tools/eval.py --seeds 4`. Each WAV is seeded from its case
  name, so both decoders get byte-identical input. CER is the Levenshtein distance on upper-cased text
  with runs of spaces collapsed.

## Results (CER)

| Condition | vail | libmorsedec |
| --- | ---: | ---: |
| **Overall (464 cases)** | **3.7 %** | **10.4 %** |
| 12 WPM, 3 / 0 / −3 dB | 1.3 / 1.4 / 0.9 % | 7.1 / 6.9 / 10.9 % |
| 20 WPM, all SNRs | 1.0–1.5 % | 1.0–2.5 % |
| 30 WPM, no noise | 1.0 % | 9.4 % |
| 30 WPM, −3 dB | 2.0 % | 4.6 % |
| 40 WPM, no noise | 12.5 % | 28.9 % |
| 40 WPM, 10 / 3 / 0 / −3 dB | 0.8 / 1.2 / 7.1 / 36.3 % | 9.1 / 12.1 / 19.4 / 67.4 % |
| Jitter 0.2, 18 / 28 WPM | 5.0 / 7.1 % | 10.0 / 17.1 % |
| Fading, 18 / 28 WPM | 4.3 / 6.0 % | 5.2 / 6.5 % |
| Interfering signal 50–200 Hz away | 0.0 % | 0.0 % |
| Impulse noise, 2 / 8 clicks per s | 2.4 / 7.3 % | 1.6 / 4.0 % |
| Short overs `TEST`, `TU` | 0.0 % | 100 % (no output) |
| Short overs `K1ABC`, `5NN TU` | 0.0 % | 0.0 % |
| Pitch drift 25 Hz | 0.0 % | 0.0 % |

## Where the difference comes from

The two decoders use the same structure: windowed FFT, per-channel spectrum matching and envelope,
duration HMM, five-class log-duration model with 14-element re-estimation, code-table decoding with
split fallback, word Viterbi over a 26-character window, and 500-character messages. The gap comes
from a few tuning decisions, which were tested by moving each one across.

| Experiment | Result |
| --- | --- |
| libmorsedec + vail's data-fit term in timing re-estimation | 10.4 % → **7.2 %** (40 WPM @ 10 dB 9.1 → 1.2 %, 30 WPM no noise 9.4 → 1.0 %) |
| vail + libmorsedec's tonal-contrast cap on keying probability | 3.7 % → 4.4 % (impulse noise 7.3 → 4.8 %, but 40 WPM no noise 12.5 → 18.8 %) |

Most of the remaining gap comes from two things:

- **Short transmissions.** libmorsedec produces no text for transmissions shorter than one learning
  window. vail decodes the buffered elements when the carrier drops.
- **Slow-speed start.** libmorsedec misreads the opening at 12 WPM ("R TNX" → "ETE TTEX"). vail
  treats long silences in the learning window as ordinary gaps, and its data-fit term keeps the first
  estimate near the observed speed.

## Ideas worth testing in vail

These libmorsedec details were not measured here:

- a monotonic committed-time boundary, so no time is delivered twice after a carrier drop;
- starting segmentation in an established-gap state, so a mark at the very start is kept;
- skipping frames until the analysis window's main lobe holds real audio;
- selecting the current channel by smoothed absolute power with 4× hysteresis;
- the contrast cap, made speed-dependent or with a looser threshold.

libmorsedec also offers I/Q input, configurable sample rates and bands, and an external-spectrum
entry point. vail only takes 8 kHz.
