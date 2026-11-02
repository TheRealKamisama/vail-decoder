# Task: fix the neighbouring-station problem

Fix the neighbouring-station problem in the vail CW decoder engine. Read [close-signals.md](close-signals.md) first:
it has the reproduction, measurements, root cause, and the two fixes already tried. Also read `CLAUDE.md`, including
its rule that `src/` must stay embeddable.

## The problem

When two CW stations are closer than about 200 Hz, the engine loses much of their text, usually the weaker station's.
At 100 Hz, equal strength, 15 dB SNR, the error is 12% for the first station and 82% for the second. The reference
keying detector (`-DENV_TUNE_LIK=0`) copies the same audio almost perfectly, so the information is in the spectrum.
But that detector gives up 4.4 dB of weak-signal sensitivity, so it isn't the answer.

## Believed cause

`spm_noise_sides()` in `src/sigext.c` takes the frame's broadband level as the median of the bins at offsets 6-16
either side of the channel. A strong neighbour lifts it, `frame_noise()` then treats the frame as a static crash, and
the channel's key-up evidence is lost. The evidence for this is indirect: changing only that level restores most of
the copy. Confirm it with a `vail_decode -T` trace before relying on it.

## Fixes already tried

Both change only the broadband level.

- **Lower of the two per-side medians:** fails badly on static (`qrn2` goes from 0% to 51.6%), because bursts aren't
  spectrally flat.
- **Median over offsets 6-48 (diff in close-signals.md):** much better, but not acceptable. It takes `w12_s10` from 0%
  to 11.8%, `w20_s10` from 0.4% to 6.1% and `qrn8` from 0% to 4.0%, and on `tools/closepair.py` it does worse on the
  first station at 60-80 Hz and adds a ghost at 150 Hz. Nobody has found why the single strong-station `w*_s10` rows
  get worse. Find out, because it may reveal the real shape of the fix.

The fix doesn't have to be a tweak to this median. A different way to tell a neighbour from a static crash is fine if
it's simple.

## Measure

Measure before and after, always on identical audio against an unmodified baseline build:

```sh
python3 tools/closepair.py --builds build,build/v_NAME   # the target; scores both stations
python3 tools/weak.py --builds build,build/v_NAME        # baseline mean CER 23.15%, SNR at 10% CER -8.2 dB
VAIL_BUILD=build/v_NAME python3 tools/eval.py --seeds 4  # baseline overall CER 0.2%
```

Differences under about 0.3% in the weak grid's mean, or 0.5% on eval, are noise.

## Done means

1. `closepair` improves substantially for both stations at 80-150 Hz, with no new ghosts.
2. No regression on `weak.py` or `eval.py`, in particular the `qrn` and `w*_s10` rows. If some trade-off is
   unavoidable, stop and show the numbers rather than deciding yourself.
3. The fix is a `*_TUNE_*` compile-time switch, on by default, with 0 restoring today's behaviour, and `src/` gains no
   new dependencies.
4. `./build/test_conformance` passes. Recompute any vectors that a changed constant affects.
5. `close-signals.md` is updated with the fix and final numbers, and the README's Known limits section is corrected.
6. The close-pair limits in `cli/tests/e2e.py` are tightened to the new performance (`pair-100hz`, `pair-60hz`,
   `strong-by-weak`, `boundary-pair`), and `cd cli && make test` passes.

If close stations can't be fully fixed, say honestly how far it got and what remains. Don't weaken a test or a limit
to get a pass. Work on a branch and don't push to main.
