# Comparison with Morse Expert

Morse Expert is a CW decoder for Android by Alex Shovkoplyas, VE3NEA, the author of CW Skimmer. It is freeware, with a premium
version. This compares what it does, as [its developer's page](https://ve3nea.github.io/MorseExpert) describes version 1.18
(read on 2026-11-07), with vail-cli and the vail engine. Morse Expert runs only on Android, so it was not run here, and nothing
below compares how well the two decode.

## Side by side

| | Morse Expert 1.18 | vail |
| --- | --- | --- |
| Signals | one at a time: with more than one in its passband it switches between them by itself, and a tap on the waterfall locks it to one | every signal at once, each in its own row |
| Band | waterfall 200–1200 Hz, decodes 300–1100 Hz | 200–3900 Hz; `--band` narrows it |
| Speed | 12–45 WPM | follows the sender: measured at 12–40 WPM (`tools/eval.py`), with a speed bank from 12 to 45 WPM |
| Pitch | automatic | automatic, for each signal |
| Input | the phone's microphone, or a cable from the radio | any sound input device; WAV, FLAC and MP3 files; raw samples on stdin, as from `rtl_fm` |
| Level | a level bar, to be kept within −50 to −20 dBFS | a meter in dBFS, and an alert when the input clips or is silent |
| Display | waterfall and text | a row per signal with SNR, speed, state and text, and a log of finished lines; no waterfall |
| Text | two modes: General Text, and Ham Radio QSO, which does "extra processing of the decoded text to improve word segmentation and highlight the callsigns and special words" | one: `WRD` always sets word ends with templates for callsigns, reports and prosigns, and classes each word, but the classes are not shown |
| Text that is not Morse | no squelch, threshold or confidence is described | the Morse score: lines whose keying is not Morse are left out, `--all` shows them, and `--json` gives each line's score |
| Output | select text to copy or share it, or save it to a file on the phone; a Pause Decoding button | finished lines on stdout, JSON Lines with SNR, speed and score, and a WAV recording of the session |
| Platforms | Android, 64-bit only since 1.17 | Linux, macOS and Windows; the engine is C99 with no audio, threads or file I/O, for firmware and apps |
| Decoder | "the same algorithms as used in CW Skimmer" | a likelihood keying detector, a duration HMM with adaptive timing, and a character Viterbi decoder with a trigram |
| Licence | freeware; the premium version has no ads and lets the user choose text colours | GPL-3.0-or-later |

## Ideas for vail

1. **Highlight callsigns and reports,** as the Ham Radio QSO mode does. vail has the hard part: `WRD` already classes each
   word, and `API_CharAt` gives the class with each character. But only for the current channel: `vail_channel_text`, which
   vail-cli reads for every channel, is plain text. A per-channel call that gives characters with their word class would let
   the view colour callsigns and reports, and `--json` list them. This is also where the callsign vocabulary belongs, which
   did not work as a gate on text ([engine-confidence.md](engine-confidence.md#a-callsign-vocabulary)).
2. **Follow one signal,** as a tap on Morse Expert's waterfall does. In vail-cli's view, a key could pick a signal's row and
   show only its text, at full width. `--band` already narrows the band for a pipe or a log, and the engine has
   `API_PitchLock` for its current channel.
3. **See the band.** A waterfall is how Morse Expert's users find and tune signals. The view could show one line of the
   spectrum across the band, with each signal's place marked, from the FFT that `cli/src/band.c` already reads.
4. **A recommended level.** The meter could mark the range of input level that decodes best, as Morse Expert gives
   −50 to −20 dBFS. vail-cli already says when the input clips or is silent.
5. **Pause.** A key that holds the view still while it is read, with decoding and the log going on.
6. **Plain text.** Morse Expert's default mode is General Text. vail's word templates and trigram are built for amateur radio
   exchanges, and how they do on plain language, such as bulletins or practice text, has not been measured. A mode for it would
   be an engine switch, measured as any other.
7. **A phone app.** Morse Expert is in its users' pockets. The engine is built to be embedded, so an Android or iOS front end is
   possible, with the same per-signal score.

Where vail is ahead: every signal at once over 3.7 kHz, against one at a time over 800 Hz; files, pipes, JSON and recording,
for logging and scripts; a score of whether a signal is Morse at all; and an open engine that other programs can build in.

## To compare decoding

Play the same audio into both: the recordings listed in [engine-confidence.md](engine-confidence.md#the-recordings) into the
phone through a cable (its developer advises a cable over the microphone, as acoustic echo "can degrade decoding accuracy, or
even completely destroy decoding"), and the same files into `vail-cli`. Synthetic audio from `cwgen`, with its known text,
would let both be scored by character error rate, as `tools/eval.py` scores vail.
