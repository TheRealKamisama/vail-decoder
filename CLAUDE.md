# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repository is

This is the vail project: a real-time CW/Morse decoding core in C99, meant to be built into firmware and
apps, plus `cli/` (vail-cli), a product built on that core. `cleanroom/` is the project's
development documentation, a **clean-room behavioural specification** that the implementation in
`src/` is built from. Code must conform to `cleanroom/` (in particular `13-conformance.md`); where the
implementation departs from it on purpose, the departure is recorded in `12-open-items.md` §E.

## Commands

No Makefile; `build.sh` compiles everything into `build/` (`OUT=dir` for another directory). The
host may lack a system compiler; any C99 compiler works via `CC`, e.g. `CC="zig cc" ./build.sh`.

```
./build.sh                                   # build/vail_decode, build/cwgen, build/test_conformance
./build/test_conformance                     # 13-conformance vectors, primitive cross-checks, unit tests
./build/cwgen -o x.wav -t "CQ DE K1ABC" -w 25 -f 700 -n 3    # synthetic Morse; options in its header comment
./build/vail_decode [-a] [-m] [-s] [-v] [-T trace.txt] [-E elems.txt] x.wav # decode a WAV (any rate, 16-bit)
python3 tools/eval.py [--quick] [--seeds N] [--filter SUBSTR] [--show]   # CER over the original grid
python3 tools/weak.py [--full] [--builds A,B] [--snr ..] [--cond ..] [--show]   # weak-signal grid
python3 tools/elemcheck.py [--build DIR] [--snr -10,-12] [--cond clean]  # element errors vs ground truth
python3 tools/lmgen.py                       # regenerates src/lm_table.h (CVD character trigram)
python3 tools/closepair.py [--builds A,B]    # two close stations, both scored, ghosts counted (docs/close-signals.md)
python3 tools/notmorse.py [--seeds N] [--show] [--wav F..]   # speech, music, data modes: false text, and real text lost
(cd cli && make && make test)                # build/vail-cli and its end-to-end tests; make NCURSES=0 leaves the view out
```

`./build-windows.sh` cross-compiles `build/win/vail_decode.exe`. `vail-cli -r rec.wav` records a live session
for replaying here with `vail_decode` for tuning.

`eval.py` uses `VAIL_BUILD` (default `build/`) so tuning variants can be compared;
`tools/ab.sh NAME "-DSTA_TUNE_GRID_FIT=0" [--seeds 4]` builds `build/v_NAME` with those flags and
prints its overall CER (full table in `build/v_NAME/eval.txt`). Use `--seeds 4` (464 cases) before
adopting a change; 2 seeds is too noisy for differences under ~0.5 %. Both eval tools run in parallel.
Weak-signal work is measured with `tools/weak.py --builds build,build/v_NAME` (1440 cases by default,
`--full` 4200): builds are compared on identical audio, and the summary gives the SNR at 10 % and
25 % CER; differences under ~0.3 % in its mean are noise. `vail_decode -T` writes a per-frame trace of
every channel (bin, floor, amplitude, SNR estimate, LLR, keyProb, carrier, noise references,
log-odds, emitted elements, STA means, CVD characters).

## The engine and `cli/`

`src/` is the engine and must stay embeddable: no audio, threads, file I/O or new dependencies, and nothing in it may
know about `cli/`. `cli/` depends on the engine and has its own `Makefile`, `tests/` and vendored
`third_party/miniaudio` (which has no SPDX line of its own). `cli/src/band.c` reads the whole FFT through
`vail_internal.h`, so a change to `struct vail_fe` or the window must be checked there too. A change to the engine's
behaviour is measured with `tools/eval.py` and `tools/weak.py`; `cli/tests/e2e.py` only guards the product.

ncurses is optional for `cli/` (`view.c` compiles to stubs without it), and everything but the full-screen view must work
without it. The engine gives no measure of whether a channel is Morse at all, so `cli/src/text.c` judges from the text; what
the engine would need for this is in `docs/engine-confidence.md`.

## Versions and releases

The engine (`VAIL_VERSION` in `src/vail.h`) and vail-cli (`VERSION` in `cli/src/main.c` and the `.TH` line of
`cli/vail-cli.1`) are versioned separately, and `cli/tests/e2e.py` checks that those places agree. Pushing a tag
`vail-cli-vX.Y.Z` runs `.github/workflows/release.yml`, which builds and tests every platform, refuses a tag that differs
from the binary's version, and creates a draft release. Check the draft, then `gh release edit TAG --draft=false`.

## Clean-room rules (binding, from `cleanroom/README.md` §0)

Every edit to the document set must follow these:

1. No source-system, repository, package, file, library, vendor or author names.
2. No addresses, memory layout, field offsets, internal representation details, tool names, or
   original identifiers (class/unit/method/type names). Use only the symbolic names this spec defines.
3. No source-platform names (OS, runtime, language implementation).
4. Algorithms go in the spec's own pseudocode and formulas. Constants are decimal with units;
   representation precision is mentioned only when it changes behaviour.
5. Anything not determined is marked `[UNDETERMINED]` in place **and** registered in
   `12-open-items.md` (categories A structural/dead code, B values/formulas, C host conventions,
   D semantics; each entry has determined / undetermined / suggested-default columns). Never write a
   guess as fact. A suggested default is an engineering choice, not an observation. Deliberate
   implementation departures go in category E with the switch name and the measured effect.

## Document conventions

- Module symbols: `FE`, `BCM`, `SIG`, `SPM`, `ENV`, `SEG`, `STA`, `CHR`, `WRD`, `MSG`, `API`;
  filter primitives `RUNMAX(n)`, `RUNMIN(n)`, `BOXAVG(n,k)`. The shared enums (`CH_STATE_*`,
  `API_STATE_*`, `SEG_KIND_*`, `WE_*`, `WT_*`) are defined once in `README.md` §2.2. Do not invent
  another set.
- Fields are camelCase; constants are UPPER_SNAKE with a module prefix (`ENV_HYST_ON`).
- Every module document (02–09) has four sections: responsibilities, interface (inputs/outputs),
  fields and state, algorithm and parameters. Each formula lists every constant it uses. Field
  tables name a producer and a consumer for each field, or mark it `[UNDETERMINED]`.
- Time is counted in **frames** (1 frame = 85 samples @ 8000 Hz = 10.625 ms). The frame is the only
  internal time unit.

## Architecture (big picture)

Per-frame, one-way pipeline (`01-overview.md` §3, `11-module-contracts.md` §1), with the weak-signal
replacements of ENV and CHR switched in (see `docs/weak-signal.md`):

```
FE → BCM → for each channel: SIG{ SPM → ENV → SEG ⇄ STA (⇄ SPD) → CVD → WRD } → MSG → API
```

- `FE` produces a 128-bin power `Spectrum`: bin `j` = `203.125 + 7.8125·j` Hz, from a 1024-point
  unnormalised FFT, kept bins 26…153.
- `BCM` owns the frame counter (the only timestamp), the 200-frame spectrum history, the peak map,
  the noise floor and the channel table. It creates, merges (every 32 frames) and retires channels,
  and picks the current channel. Each channel runs its own `SIG` instance.
- The only feedback loop is `SEG ↔ STA`: `SEG` sends element history to `STA`, and `STA` sends
  back the duration priors (5-class log-duration model) that drive `SEG`'s HMM (and `CVD`). `SPD`
  watches that loop and restarts `STA` from another speed when a fixed-timing model fits much better.
- `CHR` decodes elements through a 64-entry code table. A miss never drops elements: it splits,
  then falls back to a placeholder. `WRD` runs Viterbi over a template state graph to set
  word-end/word-class. `MSG` keeps 500 chars, shifts left by 125 on overflow, and dedups by
  `elementOrdinal`.
- Weak-signal path (each a `*_TUNE_*` switch, on by default): `ENV_TUNE_LIK` (likelihood keying
  detector: Rician/exponential LLRs, noise from the bin's key-up frames bounded by its side bins, EM
  SNR estimate; `LIK_TUNE_PEAK` keyed peak tracking), `SEG_TUNE_NORM_DUR` (normalised duration hazards), `SPD_TUNE` (speed bank),
  `CVD_TUNE` (characters decoded directly from SEG's per-frame input by `cvd.c`; CHR is then only the
  code table), `SIG_TUNE_REPLAY` / `SIG_TUNE_OVER` (re-decoding once the timing is learned, per
  transmission), `BCM_TUNE_TEXT_RATE` (a channel needs sustained text to become current),
`LIK_TUNE_OWN_NOISE` and `BCM_TUNE_KEEP_ON_SWITCH` (close stations, `docs/close-signals.md`). SEG's input
  is `SegObs` (log-odds of key-down, rising and falling edge frames); with `ENV_TUNE_LIK=0` it is the
  reference keyProb as log-odds.

## Code layout

One file per module under `src/` (`fe.c`, `bcm.c`, `sig.c`, `sigext.c` = SPM+ENV, `seg.c`, `sta.c`,
`spd.c`, `cvd.c`, `chr.c`, `wrd.c`, `msg.c`, `api.c`, `filt.c` = primitives); `vail.h` is the public host
API, `vail_internal.h` holds every module struct and the spec constants, `lm_table.h` is generated by
`tools/lmgen.py`. The project is GPL-3.0-or-later (`LICENSE`): every source file starts with an
`SPDX-License-Identifier: GPL-3.0-or-later` line (after the shebang in scripts), new files too.
Comments cite spec sections (`05 §8.6`). Things that are easy to get wrong:

- `SEG` emits batches newest-first (as the spec does); `sig.c` reverses them before `CHR`, which works
  in ascending time order. `CHR`'s run boundary element is the gap that *follows* a character.
- `WRD` is re-run from scratch on the last 26 characters each time (A13); `MSG` dedups by
  `elementOrdinal`, so the re-sent tail must carry identical ordinals.
- Tuning switches are `*_TUNE_*` macros (`sta.c`, `seg.c`, `vail_internal.h`, `api.c`); 0 restores the
  reference behaviour. Measure any change with `tools/eval.py` and `tools/weak.py` before and after.
- `CVD` runs on the same per-frame input as `SEG` and takes its durations from `STA`; when `STA`'s
  model changes (`modelDirty`, a speed-bank snap) `cvd_set_model` must follow. Its ordinals are SEG
  steps (the end of the gap after each code), so they stay unique and increasing for `MSG`.
- The FE window cutoff is 0.0023 (30 Hz noise bandwidth), not the spec's 0.003125; ENV derives its
  LLR scale from the window, so a host that computes its own spectrum must use the same window.
- A channel's centre bin is moved by SPM (strong signals) and by ENV's keyed peak tracking
  (`env_peak_move`, weak ones); the history rings in ENV keep the old bin's powers across a move.
- `cleanroom/` is not part of this repository. The weak-signal departures are recorded in
  `docs/weak-signal.md` and belong in `12-open-items.md` §E where the document set is kept.

## Where to put and check things

- `10-parameters.md` indexes every constant. When a value changes in a module document, update it
  there too, and in `11-module-contracts.md` §4 if ownership changes.
- `13-conformance.md` has numeric test vectors derived from the formulas. Recompute them when you
  change a constant (e.g. the 23.16 WPM prior speed, the pitch/bin vectors, the `envKeyProb` values).
- `11-module-contracts.md` is the single summary of per-module invariants and failure modes.
- There are two frequency conversions: spectrum bin→Hz uses base 203.125, while the host interface
  (`API_Pitch`, pin commands) uses `200 + rint(bin·7.8125)` and `rint((f−200)/7.8125)`, clamped
  to `[10, 117]`. Keep them separate.
- `BCM_PEAK_THRESH` (3.5, birth) and `SPM_PEAK_MIN_POWER` (7.5, tracking) are two thresholds on the
  same `BCM.peakRatio` vector, which `04` calls `trackArray`.
- `cleanroom/` is the only documentation for the project.
