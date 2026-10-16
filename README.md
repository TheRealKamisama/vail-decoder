# vail-decoder

A real-time CW (Morse code) decoder core in portable C99, with no dependencies beyond the C math
library. It is named after Alfred Vail, who worked with Samuel Morse on the telegraph and the code.

vail takes 8 kHz audio. It finds every CW signal in the 200–1200 Hz band, follows each one in its
own channel, learns each sender's timing, and produces text that is revised as more context arrives.

## Features

- **Multichannel.** Signals are found and tracked independently. Each channel has its own timing
  model and message, and the strongest decoding channel is selected automatically, or locked by pitch.
- **Adaptive timing.** A duration-aware HMM segments keying into elements. It uses a five-class
  log-duration model (dot, dash, element gap, letter gap, word gap) that is re-estimated as the
  sender keys, so speed changes and hand-keyed timing are followed without configuration.
- **Robust character and word decoding.** Unknown patterns are split before falling back to a
  placeholder. A word-level Viterbi pass over a template graph (dictionary words, reports,
  prosigns, callsign grammar) places word boundaries.
- **Streaming and bounded.** One 10.625 ms frame (85 samples) per call. No threads and no callbacks.
  Each message holds at most 500 characters.
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
./build/test_conformance                                    # numeric conformance checks
python3 tools/eval.py [--quick] [--seeds N] [--show]        # character error rate over a synthetic grid
```

`cwgen` also simulates timing jitter (`-j`), fading (`-F`/`-D`), pitch drift (`-d`), impulse noise
(`-Q`) and interfering signals (`-S`); see the header comment of `tools/cwgen.c`.

`vail_decode` options: `-p HZ` locks the pitch, `-s` streams characters as they are produced, `-m`
emulates a host applying delete counts, `-v` prints state/pitch/SNR/WPM, and `-T file` writes a
per-frame trace of every channel.

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

There is one-way processing per frame. The only feedback loop is between segmentation and the
timing statistics.

```
FE → BCM → for each channel: SIG { SPM → ENV → SEG ⇄ STA → CHR → WRD } → MSG → API
```

| File | Stage |
| --- | --- |
| `fe.c` | 1024-point FFT front end, 128-bin power spectrum (7.8125 Hz bins) |
| `bcm.c` | band manager: peak map, noise floor, channel creation, merging, retirement, selection |
| `sigext.c` | spectrum matching and envelope / soft keying probability |
| `seg.c`, `sta.c` | duration HMM segmentation and adaptive five-class timing model |
| `chr.c`, `wrd.c` | code-table character decoding and word-boundary Viterbi |
| `msg.c`, `api.c` | bounded, revisable messages and the host interface |

Behaviour tweaks are `*_TUNE_*` compile-time switches (`0` restores the reference behaviour). Use
`tools/ab.sh NAME "-DFLAG=..." --seeds 4` to measure one against the synthetic grid.

## Accuracy

On the built-in synthetic grid (`tools/eval.py --seeds 4`, 464 cases: 12–40 WPM, no noise to
−3 dB SNR, jitter, fading, interference, impulse noise, drift, short messages), the overall character
error rate is **3.7 %**. A head-to-head comparison with a related decoder is in
[docs/libmorsedec-comparison.md](docs/libmorsedec-comparison.md).
