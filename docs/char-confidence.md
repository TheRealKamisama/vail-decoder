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
| −12 dB | 34 % | 70 % |
| −14 dB (61 % of the copy wrong) | 57 % | 81 % |
| RTTY, voice, carriers, pumping noise (no Morse) | | 67 % |

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
margin(c) = max { score(s) : the path of s has c ending within e ± CVD_CONF_TOL }
          − max { score(s) : it has not }
```

clamped to [0, `VAIL_CONF_MAX`] (50). The path "has not" if it has another code there, or none at all: a reading in which the
keying was noise is an alternative too. So a junk letter that only just beats "nothing here" scores near 0. The scores
include the duration priors and the trigram, so a letter that the context alone carried is doubted as well.

**When.** In a Viterbi decoder the survivors merge: some frames after a character, every state's history runs through the
same node, and its competitors are gone. Taken at commit time (`CVD_LAG`, 80 frames), the margin reads "no doubt" for most
letters, wrong ones included. So each code is scored once, `CVD_CONF_DELAY` (20) frames after it ends, while the
alternatives still live. A code is scored even when it is not on the best path then, in which case its margin is 0. That
covers a letter the decoder only came round to later. Only one code node is created per frame, so this is one pass over the
states per frame. A code committed sooner than that (at a carrier drop) is scored at commit.

AUC is the chance that a wrong character scores lower than a right one, over the grid below (all SNRs, and at −8 and −12 dB):

| When the margin is taken | all | −8 dB | −12 dB |
| --- | ---: | ---: | ---: |
| at commit, first code after the commit point (first try) | 0.790 | 0.677 | 0.645 |
| at commit, signed (`CVD_CONF_DELAY` 100) | 0.844 | 0.731 | 0.686 |
| 5 frames after the code | 0.755 | 0.763 | 0.658 |
| 10 frames | 0.883 | 0.868 | 0.728 |
| **20 frames (default)** | **0.885** | **0.868** | **0.734** |
| 40 frames | 0.873 | 0.831 | 0.706 |

Five frames is too soon: the gap after the code is not yet long enough to tell a letter gap from an element gap. The timing
tolerance hardly matters: 3, 8 and 20 frames give 0.887, 0.885 and 0.885.

## The full table

`tools/charconf.py` (900 cases: 20, 25 and 30 WPM, −4 to −14 dB, clean, hand, qsb, rx and qrn, the ten texts of
`tools/weak.py`, one seed) and the four files of `tools/bandnoise.py` with Morse-like false text (`rtty`, `voice`,
`carriers_qsb`, `pink_agc`). Each cell is the share of wrong / right characters below that confidence.

| | right | wrong | AUC | < 0.5 | < 1 | < 2 | **< 3** | < 5 | < 8 | < 12 | < 16 | < 20 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| all Morse | 20086 | 4704 | 0.885 | 28/3 | 40/5 | 57/9 | **70/12** | 84/20 | 94/34 | 98/57 | 99/74 | 100/86 |
| −4 dB | 4263 | 155 | 0.890 | 13/0 | 20/0 | 32/1 | **44/1** | 60/2 | 72/4 | 84/19 | 91/42 | 94/62 |
| −6 dB | 4178 | 210 | 0.876 | 23/0 | 28/1 | 40/1 | **48/2** | 63/3 | 70/11 | 85/34 | 96/59 | 98/79 |
| −8 dB | 4062 | 287 | 0.868 | 21/1 | 27/2 | 40/3 | **50/5** | 64/10 | 84/24 | 94/58 | 99/81 | 100/93 |
| −10 dB | 3648 | 610 | 0.785 | 21/3 | 29/5 | 43/9 | **54/14** | 70/27 | 86/53 | 98/82 | 100/95 | 100/99 |
| −12 dB | 2626 | 1370 | 0.734 | 27/9 | 39/13 | 57/23 | **70/34** | 86/54 | 97/79 | 100/95 | 100/99 | 100/100 |
| −14 dB | 1309 | 2072 | 0.654 | 34/20 | 47/28 | 68/44 | **81/57** | 94/79 | 99/95 | 100/99 | 100/100 | 100/100 |
| no Morse | 0 | 715 | | 30 | 39 | 54 | **67** | 83 | 93 | 97 | 97 | 97 |

A host that fades gradually can map 0 → faintest and about 12 → normal; almost every wrong character is below 12.

## No change to decoding

The margin only reads `CVD`'s states. The text of `vail_decode`, `vail_decode -a` and `vail_decode -m` is byte-identical with
`CVD_TUNE_CONF` on and off on all 913 files above (2739 runs), and vail-cli's 63 end-to-end tests pass.

## Cost

About 20 % more CPU for a busy band (360 s of fading carriers and voice, many channels: 5.4 s → 6.4–6.7 s), and 14.5 KB more
per channel (a confidence per history node, and scratch for the pass), on about 180 KB.

## In the API

`vail_char` (`src/vail.h`) has a new last field, `float confidence`; `lowSnr` is `confidence < CVD_CONF_DOUBT` (3). A host
built against the old header must be recompiled. Spaces, and the marks the API inserts on a channel switch, carry
`VAIL_CONF_MAX`. A character's confidence is fixed when it is first given out; a character `MSG` replaces comes back with its
own. With `CVD_TUNE=0` (`CHR` decodes) the old flag is kept, and `confidence` is 0 where it is set and `VAIL_CONF_MAX`
elsewhere. With `CVD_TUNE_CONF=0`, every character is confident.

The host character list (`API_CharAt`) is the current channel's. Per-channel records, which vail-cli would need to fade text,
come with `vail_channel_chars` on the callsign-highlighting branch, which must then copy `confidence` too.

## Limits

- **Confident junk.** A third of the false text from RTTY, voice and carriers scores 3 or more. RTTY's keying is clean, so
  the decoder is sure of its reading; whether the channel is Morse at all is `vail_channel_confidence`'s question
  ([engine-confidence.md](engine-confidence.md)).
- **Confident mistakes.** About 30 % of the wrong characters score 3 or more, most of them at −12 dB and below. Why they
  do was not looked into.
- **Word spaces are not scored.** A space gets `VAIL_CONF_MAX`; a word split or joined in the wrong place is not doubted.
- **Measured on synthetic audio.** No real recording was at hand; the thresholds should be checked on band recordings.

## Measuring again

```sh
python3 tools/bandnoise.py noise/                                       # the files with no Morse
python3 tools/charconf.py --wav noise/rtty.wav,noise/voice.wav,noise/carriers_qsb.wav,noise/pink_agc.wav
python3 tools/charconf.py --builds build,build/v_NAME --show            # variants on identical audio; --show marks wrong letters
./build/vail_decode -m -q file.wav                                      # host text, then each character's confidence
```
