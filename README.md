# vail-decoder

A real-time CW (Morse code) decoder core in portable C99, with no dependencies beyond the C math
library. It is named after Alfred Vail, who worked with Samuel Morse on the telegraph and the code.

vail takes 8 kHz audio. It finds every CW signal in the 200–1200 Hz band, follows each one in its
own channel, learns each sender's timing, and produces text that is revised as more context arrives.

## Features

- **Weak-signal copy.** Keying is scored as calibrated likelihoods (Rician key-down against
  exponential noise, with the noise and SNR estimated per channel), and characters are decoded
  directly from those per-frame likelihoods by a Viterbi search over the code table with a character
  trigram. At 18–30 WPM, 10 % CER is reached at −8.9 dB SNR in 2500 Hz and 25 % at −11.0 dB, about 4.4 dB
  further down than before. See [docs/weak-signal.md](docs/weak-signal.md).
- **Multichannel.** Signals are found and tracked independently. Each channel has its own timing
  model and message, and the strongest decoding channel is selected automatically, or locked by pitch.
- **Adaptive timing.** A duration-aware HMM segments keying into elements. It uses a five-class
  log-duration model (dot, dash, element gap, letter gap, word gap) that is re-estimated as the
  sender keys, so speed changes and hand-keyed timing are followed without configuration. A bank of
  fixed-speed models catches a speed estimate that has lost track. The start of a transmission, and
  of each later over, is decoded again once the sender's timing is known.
- **Robust to real bands.** Fading, static crashes, key clicks, a neighbouring station, receiver
  filter slopes and drift are part of the test grid. Noise alone produces no text.
- **Character and word decoding.** Only codes of the table can come out. A word-level Viterbi pass
  over a template graph (dictionary words, reports, prosigns, callsign grammar) places word boundaries.
- **Streaming and bounded.** One 10.625 ms frame (85 samples) per call. No threads and no callbacks.
  Each message holds at most 500 characters. About 350× real time on one core, about 180 KB per live
  channel.
- **Host-friendly text output.** The host can poll a delete count and append the new characters,
  and its copy of the text then matches the decoder's revised message exactly.

## Build

There is no Makefile. Any C99 compiler works:

```sh
./build.sh                         # build/vail_decode, build/cwgen, build/test_conformance
CC="zig cc" ./build.sh             # on hosts without a system compiler
./build-windows.sh                 # cross-compiles build/win/vail_live.exe and vail_decode.exe
```

## Tools

```sh
./build/cwgen -o x.wav -t "CQ DE K1ABC" -w 25 -f 700 -n 3   # synthesise Morse: 25 WPM, 700 Hz, 3 dB SNR
./build/vail_decode x.wav                                   # decode a 16-bit WAV (any rate; resampled to 8 kHz)
./build/vail_decode -a x.wav                                # text of every channel, with pitch
./build/test_conformance                                    # numeric conformance checks and unit tests
python3 tools/eval.py [--quick] [--seeds N] [--show]        # character error rate over a synthetic grid
python3 tools/weak.py [--full] [--builds A,B] [--show]      # weak-signal grid: CER against SNR, -4 to -14 dB
python3 tools/elemcheck.py [--snr -10,-12]                  # element errors against the generator's truth
```

`cwgen` also simulates timing jitter (`-j`), sinusoidal fading (`-F`/`-D`), Rayleigh fading (`-R`),
pitch drift (`-d`), a receiver filter (`-B`), hand-keyed weight and spacing (`-W`/`-G`), impulse noise
(`-Q`) and interfering signals (`-S`). It can also write the true key-down intervals (`-K`). See the
header comment of `tools/cwgen.c`.

`vail_decode` options: `-p HZ` locks the pitch, `-s` streams characters as they are produced, `-m`
emulates a host applying delete counts, `-v` prints state/pitch/SNR/WPM, `-T file` writes a
per-frame trace of every channel, and `-E file` writes every classified element of every channel.

`vail_live` (Windows) decodes from a sound input device: `-l` lists devices, `-d N` selects one,
`-p HZ` locks the pitch, and `-r rec.wav` records the session for replay with `vail_decode`.

## Library use

```c
#include "vail.h"

vail_fe  *fe = vail_fe_new();          /* spectrum front end */
vail_api *h  = API_New();              /* decoder */
int16_t block[VAIL_HOP_SAMPLES];       /* 85 samples @ 8000 Hz */
float spectrum[VAIL_N_BINS];

while (read_audio(block)) {
    memcpy(spectrum, vail_fe_process(fe, block), sizeof spectrum);
    API_Feed(h, spectrum);
    /* host text update: drop API_DeleteCount(h) chars, then append
       API_CharAt(h, 0 .. API_CharCount(h)-1) */
}
API_Free(h);
vail_fe_free(fe);
```

There are also queries for state, pitch, SNR, WPM and channel count (`API_State`, `API_Pitch`,
`API_Snr`, `API_Wpm`, `API_ChannelCount`). `API_PitchLock(h, hz)` locks the pitch. Full text is
available with `vail_current_text` and `vail_channel_text`. See `src/vail.h`.

## Architecture

There is one-way processing per frame. Timing feeds back from the element segmentation to the timing
statistics, which drive both the segmentation and the character decoder.

```
FE → BCM → for each channel: SIG { SPM → ENV → SEG ⇄ STA (⇄ SPD) → CVD → WRD } → MSG → API
```

| File | Stage |
| --- | --- |
| `fe.c` | 1024-point FFT front end, 128-bin power spectrum (7.8125 Hz bins) |
| `bcm.c` | band manager: peak map, noise floor, channel creation, merging, retirement, selection |
| `sigext.c` | spectrum matching, and the likelihood keying detector (noise, SNR, per-frame log-odds) |
| `seg.c`, `sta.c` | duration HMM segmentation and adaptive five-class timing model |
| `spd.c` | speed bank: fixed-speed duration models that catch a timing estimate that lost track |
| `cvd.c`, `lm_table.h` | character Viterbi decoder over the code table, character trigram (`tools/lmgen.py`) |
| `chr.c`, `wrd.c` | code table (and the reference element-to-character decoder), word-boundary Viterbi |
| `msg.c`, `api.c` | bounded, revisable messages and the host interface |
| `sig.c` | per-channel glue: carrier drops, re-decoding after learning and per over |

Behaviour tweaks are `*_TUNE_*` compile-time switches (`0` restores the reference behaviour). Use
`tools/ab.sh NAME "-DFLAG=..." --seeds 4` to measure one against the synthetic grid, or
`tools/weak.py --builds build,build/v_NAME` against the weak-signal grid.

## Accuracy

On the weak-signal grid (`tools/weak.py --full`, 4200 cases: 18–30 WPM, −4 to −14 dB SNR in 2500 Hz;
keyer and hand timing, Rayleigh fading, receiver filter, a neighbouring station, static crashes,
drift; QSO text and random groups):

| | before | now |
| --- | ---: | ---: |
| mean character error rate | 57.0 % | **21.4 %** |
| SNR for 10 % CER | −4.3 dB | **−8.9 dB** |
| SNR for 25 % CER | −6.5 dB | **−11.0 dB** |

On the original grid (`tools/eval.py --seeds 4`, 464 cases: 12–40 WPM, no noise to −3 dB SNR, jitter,
fading, interference, impulse noise, drift, short messages), the overall character error rate is
**0.2 %** (it was 3.7 %). Details and the changes behind these numbers are in
[docs/weak-signal.md](docs/weak-signal.md). An earlier head-to-head comparison with a related decoder
is in [docs/libmorsedec-comparison.md](docs/libmorsedec-comparison.md).
