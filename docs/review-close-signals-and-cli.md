# Review: the close-signals fix and vail-cli

A brief review from 2026-11-03, not a deep one. **Nothing listed here has been fixed.** What to improve is still to be
decided.

CER figures are per cent. "Against" is the same audio with `-DLIK_TUNE_OWN_NOISE=0 -DBCM_TUNE_KEEP_ON_SWITCH=0`.

## The close-signals fix

Merged into main. From a clean export it builds without warnings, conformance gives 4076 checks and 0 failures with the
switches on and off, and `cli`'s `make test` has 0 of 44 failing. See [close-signals.md](close-signals.md) for the fix.

Measured, 4 to 8 seeds:

| Finding | Result | Compared with the old engine |
| --- | --- | --- |
| A weak station between two stronger ones is lost, because neighbours on both sides raise `lo` itself | Neighbours 8–12 dB above it at 100–120 Hz: 80–86. At 3 dB above and 100 Hz: 30–35. Clean from 150 Hz | Not a regression: against 82–84, 40–41, 2–5 |
| Wideband noise on one side cannot re-train the estimate, because `lo` stays put and the frames are rejected | Up to +25 dB over the floor: 0–0.3. At +30 dB for 6 s: 1.5–2.3. At +30 dB and lasting: 0.6–1.9 | **Small regression at the extreme**: against 0–0.5, 0–0.8, 0.3–0.6 |
| Heavily clipped audio is garbage either way, and the garbage can be longer with the fix | Drive 4–8: 128–222 for a station that stops beside a stronger one | Against 121–225. On cwgen's own hard clip at 100 Hz: 197 against 90. At drive 2 it is mostly better: 55 against 100, but 170 against 159 |
| Removing the flush could lose a stopping station's tail | No tail lost. Beside a neighbour 15 dB stronger that goes on: 5.6, 0, 0 at 60, 100, 200 Hz | Better: against 21–44, 41–44, 0 |

The stopping-station test does not reach every path. A channel retired or merged with text pending, while a neighbour's
skirt holds its carrier up, was not exercised.

From reading the code, not measured:

- The hold has nothing to hold until a channel first reaches `goSum > 8`, so until then the fallback is still the raised
  `hi`. The one related case that was measured, a second station starting late at 100 Hz, copied cleanly.
- `noiseHeld` is frozen just as `goSum` falls through 8, when the average is noisiest and most affected by mark edges.
- The rejection threshold is relative to the noise it protects, so the poisoning loop is narrowed rather than closed.

Small items:

- `LIK_OWN_REJECT` has no `#ifndef`, so `-DLIK_OWN_REJECT=5` only warns and the header's 12 wins. Re-running the sweep in
  `close-signals.md` needs the guard back.
- The `8.0` in `env_apply_lik` is an unnamed constant, used twice.
- `closepair.py`'s ghost count treats the pieces of a station split across two channels as ghosts.
- `docs/close-signals-fix.md` is the task brief, and is now stale.
- cwgen limits each signal, not their sum, so several strong signals clip. See `close-signals.md`.

## vail-cli

`worktree-vail-cli-product` had nothing to merge: it is at the same commit as main before this merge, with no uncommitted
files. These findings are about `cli/` as it is on main, and none comes from the close-signals fix.

Reproduced:

1. **Periodic text is silently lost.** 400 × `VVV` at 40 WPM: 1200 letters sent, 480 printed, exit 0. 300 × `TEST`: 456 of
   1200. Unique text of 980 letters prints whole, which is what the `long-text` test uses. `dropped_bytes` in
   `cli/src/band.c` infers the message buffer's shift from its bytes, and periodic text defeats that.
2. **Write errors are reported as success.** `vail-cli a.wav > /dev/full` exits 0 with nothing on stderr. `main.c` treats
   any `fflush` failure as nobody reading, while the man page and README promise exit 1.
3. **`-b` can print a harmonic as a station.** A clipped 700 Hz station with `-b 1800-2300` prints its text at 2102 Hz. No
   engine runs for the excluded slices, so the ghost check cannot see the fundamental.

Reported from the reviewer's own runs, and not re-run:

4. A hidden channel stays hidden for good. A real station that starts on its bin within a few seconds is never printed.
5. The on-threshold in `judge` is the median × 8, which fails once the history is mostly key-down. A drifting station's
   second channel printed nothing for about 3.6 minutes, and a longer message could lose text.
6. `judge` compares on-frames at different relative thresholds, so a clipped station's harmonic can pass as real and the
   text prints twice.
7. Ctrl+C does not interrupt an idle pipe on stdin, because `signal()` restarts the blocking read on glibc.
8. Continuation lines carry the print time, not the time the text started.

From reading the code only:

9. Lines printed during the end-of-input drain are stamped up to 4 s early.
10. `make test BUILD=dir` still tests `cli/build/vail-cli`, which can be a stale binary.

## Where I would start

Item 1 of vail-cli, because it loses text with no sign of it, then item 2. On the engine, a weak station between two
stronger ones is the largest remaining gap on a busy band. The reviewer's suggestion there, untested, is a side reference
that a neighbour's lobe cannot lift, such as a median per side.
