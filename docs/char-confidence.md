# Character confidence: how sure was the decoder of each letter?

Since `CVD` replaced `CHR`, every character came out as a confident letter: `vail_char.lowSnr` was never set, and a garbled
code could not show up as `*` any more, because `CVD` only produces codes of the table. Near the noise floor that is letter
soup that looks like real text (`K 9 2 TO C QR 1K` for a message at −16 dB). The engine now gives each character a
confidence, so that a host can fade the letters it guessed. Decoding is unchanged.

`vail_char.confidence` is in nats: how much more likely the text the decoder chose is than the best text with another
character there, or none. `lowSnr` is 1 below 3, the default threshold. On the weak-signal grid:

| | right characters faded | wrong characters faded |
| --- | ---: | ---: |
| −4 … −8 dB | 1–5 % | 44–50 % |
| −10 dB | 14 % | 54 % |
| −12 dB | 34 % | 69 % |
| −14 dB (61 % of the copy wrong) | 58 % | 81 % |
| 8–45 WPM, −4 … −12 dB | 5–14 % | 40–69 % |
| RTTY, voice, carriers, pumping noise (no Morse) | | 69 % (all of it below 12) |

## The old flag, and why it is not back

`CHR` set `lowSnr` when the smoothed envelope of any frame of a character's elements, or of the gap after it, was below 1.3,
just above where the carrier drops. That is a signal-level flag, not an error flag. In the reference engine it marked a few
characters at −4 dB and most of them at −8 dB, right ones (`name is`, `599`) as much as wrong ones. A host that faded by it
would fade most of what the weak-signal work gained. With `ENV_TUNE_LIK` the envelope is the SNR estimate, which stays well
above 1.3 down to −12 dB, so even with `CHR` it hardly fires.

## The margin

At step `t`, `CVD` holds one score and one history for each of its states (mark and gap chains of every code, and the letter
gap). For a code `c` ending at step `e`, the margin is

```
margin(c) = max { score(s) : the path of s has c ending within e ± tolerance }
          − max { score(s) : it has not }
```

clamped to [0, `VAIL_CONF_MAX`] (50). The path "has not" if it has another code there, or none at all: a reading in which the
keying was noise is an alternative too. So a junk letter that only just beats "nothing here" scores near 0. The scores
include the duration priors and the trigram, so a letter that the context alone carried is doubted as well. When the best
state's own path has not, the margin is 0 and the other states are not looked at.

**When.** In a Viterbi decoder the survivors merge: some frames after a character, every state's history runs through the
same node, and its competitors are gone. Taken at commit time (`CVD_LAG`, 80 frames), the margin reads "no doubt" for most
letters, wrong ones included. So each code is scored once, a delay after it ends, while the alternatives still live: 3.5
units (`CVD_CONF_DELAY`, at STA's speed), but no less than 20 frames (`CVD_CONF_DELAY_MIN`). The tolerance is 1.5 units
(`CVD_CONF_TOL`, at least 2 frames). A code is scored even when it is not on the best path then, in which case its margin
is 0; that covers a letter the decoder only came round to later. A code committed sooner (at a carrier drop) is scored at
commit. When STA's model changes and the delay shrinks, the codes in between are still scored.

AUC is the chance that a wrong character scores lower than a right one. On the grid at 20–30 WPM (all SNRs, and at −8 and
−12 dB), with a delay fixed in frames, as the first version had it:

| When the margin is taken | all | −8 dB | −12 dB |
| --- | ---: | ---: | ---: |
| at commit, first code after the commit point | 0.790 | 0.677 | 0.645 |
| at commit, signed | 0.844 | 0.731 | 0.686 |
| 5 frames after the code | 0.755 | 0.763 | 0.658 |
| 10 frames | 0.883 | 0.868 | 0.728 |
| 20 frames | 0.885 | 0.868 | 0.734 |
| 40 frames | 0.873 | 0.831 | 0.706 |

Five frames is too soon: the gap after the code is not yet long enough to tell a letter gap from an element gap. At 20–30
WPM, 20 frames is 3.5–5.3 units. A delay fixed in frames fails slow senders, whose letter gap is still open after 20 frames;
one fixed in units shortens it for fast senders, who still gain from the time. 630 cases at 8–45 WPM (−4, −8, −12 dB;
clean, hand and qsb), AUC and the share of wrong / right characters below 3:

| WPM | 20 frames | 3.5 units | 2.5 units | 4.5 units | **3.5 units, ≥ 20 frames** |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 8 | 0.706 (59/33) | 0.848 | 0.858 | 0.860 | **0.847 (40/5)** |
| 12 | 0.876 (57/7) | 0.871 | 0.883 | 0.874 | **0.871 (50/5)** |
| 18 | 0.860 (56/10) | 0.859 | 0.863 | 0.860 | **0.859 (55/10)** |
| 25 | 0.854 (59/12) | 0.852 | 0.860 | 0.856 | **0.855 (59/12)** |
| 30 | 0.896 (70/11) | 0.893 | 0.893 | 0.892 | **0.896 (69/11)** |
| 40 | 0.867 (62/11) | 0.850 | 0.849 | 0.852 | **0.867 (62/11)** |
| 45 | 0.821 (56/14) | 0.810 | 0.813 | 0.806 | **0.822 (56/14)** |

The tolerance hardly matters: 3, 8 and 20 frames gave 0.887, 0.885 and 0.885 at 20–30 WPM, and a floor of 8 frames under
1.5 units moves the AUC by at most 0.001 at 8–45 WPM.

## The full table

`tools/charconf.py` (900 cases: 20, 25 and 30 WPM, −4 to −14 dB, clean, hand, qsb, rx and qrn, the ten texts of
`tools/weak.py`, one seed) and the four files of `tools/bandnoise.py` with Morse-like false text (`rtty`, `voice`,
`carriers_qsb`, `pink_agc`). Each cell is the share of wrong / right characters below that confidence. The newline and `>`
the API inserts when the current channel changes are left out.

| | right | wrong | AUC | < 0.5 | < 1 | < 2 | **< 3** | < 5 | < 8 | < 12 | < 16 | < 20 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| all Morse | 20086 | 4698 | 0.886 | 28/3 | 40/5 | 57/9 | **70/12** | 84/20 | 94/34 | 98/57 | 99/74 | 100/86 |
| −4 dB | 4263 | 153 | 0.902 | 13/0 | 20/0 | 32/1 | **44/1** | 60/2 | 73/4 | 85/19 | 92/42 | 95/62 |
| −6 dB | 4178 | 209 | 0.880 | 23/0 | 28/1 | 40/1 | **48/2** | 64/3 | 70/11 | 86/34 | 96/59 | 98/79 |
| −8 dB | 4062 | 287 | 0.868 | 21/1 | 27/2 | 40/3 | **50/5** | 64/10 | 84/24 | 94/58 | 99/81 | 100/93 |
| −10 dB | 3648 | 608 | 0.789 | 21/3 | 29/5 | 43/9 | **54/14** | 70/27 | 87/53 | 98/82 | 100/95 | 100/99 |
| −12 dB | 2626 | 1370 | 0.733 | 27/9 | 39/13 | 57/23 | **69/34** | 86/54 | 97/79 | 100/95 | 100/99 | 100/100 |
| −14 dB | 1309 | 2071 | 0.653 | 34/20 | 47/28 | 68/45 | **81/58** | 94/79 | 99/94 | 100/99 | 100/100 | 100/100 |
| no Morse | 0 | 692 | | 31 | 41 | 57 | **69** | 85 | 96 | 100 | 100 | 100 |

A host that fades gradually can map 0 → faintest and about 12 → normal; almost every wrong character is below 12.

## No change to decoding

The margin only reads `CVD`'s states. The text of `vail_decode`, `vail_decode -a` and `vail_decode -m` is byte-identical with
`CVD_TUNE_CONF` on and off, and with the first version of this change (which scored with a different delay and memo), on
all 1253 files above (3759 runs each); vail-cli's end-to-end tests pass.

## Cost

About 9.5 % more CPU for a busy band (360 s of fading carriers and voice, many channels: 5.44 s → 5.95 s, best of three;
the first version, which memoised in a cleared array and scanned every state for every code, took 6.37 s). 8.3 KB more per
channel, all in `CVD`: a confidence and a memo stamp per history node (4 bytes × 2048) and 256 bytes of codes waiting to be
scored, on about 180 KB. `Char` is still 24 bytes: `confidence` takes the padding that `lowSnr` left.

## In the API

`vail_char` (`src/vail.h`) has a new last field, `float confidence`; `lowSnr` is `confidence < CVD_CONF_DOUBT` (3). That
changes the struct a host gets from `API_CharAt`, so the engine is now 0.2.0 (`VAIL_VERSION`); a host built against an
older header must be recompiled. Spaces, and the marks the API inserts on a channel switch, carry `VAIL_CONF_MAX`. A
character's confidence is fixed when it is first given out; a character `MSG` replaces comes back with its own. With
`CVD_TUNE=0` (`CHR` decodes) the old flag is kept, and `confidence` is 0 where it is set and `VAIL_CONF_MAX` elsewhere.
With `CVD_TUNE_CONF=0`, every character is confident.

The host character list (`API_CharAt`) is the current channel's. Per-channel records, which vail-cli would need to fade text,
come with `vail_channel_chars` on the callsign-highlighting branch, which must then copy `confidence` too.

## Limits

- **Confident junk.** About a third of the false text from RTTY, voice and carriers scores 3 or more, though none 12 or
  more. RTTY's keying is clean, so the decoder is sure of its reading; whether the channel is Morse at all is
  `vail_channel_confidence`'s question ([engine-confidence.md](engine-confidence.md)).
- **Confident mistakes.** About 30 % of the wrong characters score 3 or more, most of them at −12 dB and below. Why they
  do was not looked into.
- **Fast keying** separates less well (AUC 0.82 at 45 WPM, against 0.86–0.90 at 12–40), and the delay does not help it.
- **Word spaces are not scored.** A space gets `VAIL_CONF_MAX`; a word split or joined in the wrong place is not doubted.
- **Measured on synthetic audio.** The one real set with a full text, recorded telegrams ([telegrams.md](telegrams.md)),
  agrees where the decoder copies (44 % of wrong and 10 % of right characters below 3) but is not off-air audio.
  [real-audio-plan.md](real-audio-plan.md) plans a labeled set recorded from public receivers.

## Measuring again

```sh
python3 tools/bandnoise.py noise/                                       # the files with no Morse
python3 tools/charconf.py --wav noise/rtty.wav,noise/voice.wav,noise/carriers_qsb.wav,noise/pink_agc.wav
python3 tools/charconf.py --wpm 8,12,18,25,30,40,45 --snr -4,-8,-12 --cond clean,hand,qsb    # the speed table
python3 tools/charconf.py --builds build,build/v_NAME --show            # variants on identical audio; --show marks wrong letters
./build/vail_decode -m -q file.wav                                      # host text, then each character's confidence
```
