# Engine confidence: is a channel's keying Morse?

The engine prints its best Morse reading of anything that keys on and off. Speech, music and data modes came out as streams of
letters, and vail-cli could only hide some of them by judging the text. The engine now scores each channel: how much better Morse
timing explains its keying than the best explanation that is not Morse. vail-cli prints a line only when its channel scores 1 or
more, and its text filter is gone. Decoding is unchanged.

| | Text filter (before) | Engine score (now) | Target |
| --- | ---: | ---: | ---: |
| RTTY, synthetic, symbols shown | 100 % | **0 %** | < 10 % |
| On-off data, synthetic | 100 % | **0 %** | < 10 % |
| Speech / SSB speech / music, synthetic | 3 / 3 / 10 % | **0.06 / 0.08 / 0 %** | |
| Morse, 162 long messages, symbols lost | 2 of 10169 | 2 of 10169 | ≤ 1 % at every condition |
| Morse, 162 short messages, symbols lost | 65 of 910 (7.1 %) | **49 of 910 (5.4 %)** | < 7.1 % |
| 26 recordings with no Morse in them, symbols shown | 7879 | **55** | |

## The problem

Noise and copyable CW were never the problem: white noise, filtered noise and static print nothing, and a station is copied well down to about −9 dB
in 2500 Hz. Audio that keys but is not Morse is. This is what the engine gives (`vail-cli --all`) over 180 s of each, from
`tools/notmorse.py`:

| Input | Lines | Symbols | What it looks like |
| --- | ---: | ---: | --- |
| Speech | 837 | 6522 | `E E  EE`, `T ETTEI  T`: dozens of short-lived channels |
| SSB speech | 794 | 7079 | the same |
| Music | 143 | 3024 | `9 5 4 7 1 89  8 P 6R` |
| RTTY | 29 | 1449 | `IENNWRYL1A3NA4 / MEISSTAH5ESL5LH`: steady, at exactly its two tones |
| On-off data, not Morse | 5 | 215 | `ATNNWAE MTTQATM A3TE` |
| Noise with pumping gain | 2 | 4 | `T`, `A` |

A text check cannot separate RTTY from real code groups: `XQZ7 JW4K 8ZR2` and `IENNWRYL1A3NA4` are both random letters. The
difference is in the keying, which only the engine sees: one has dashes three dots long, and the other does not.

## The score

```c
int vail_channel_confidence(const vail_api *h, int idx, float *evidence);
```

gives a log-odds in nats. It comes from `SPD`, which already ran forward passes of fixed-timing duration models over `SEG`'s
per-frame input (`SegObs`, the log-odds of key-down and of each edge) for its speed bank. It now runs more models on the same
input:

- **Morse:** the bank's 15 fixed speeds (12–45 WPM, unchanged, and still what `SPD_TUNE` snaps the speed to), and one more at
  `STA`'s current speed, held to 5–50 WPM. All have the standard timing: dash and letter gap of three units, word gap of seven.
- **Not Morse:** 13 models from 5 to 80 WPM, each with the mean and spread of log mark length, and of log gap length, of the
  standard timing at its speed, but in one broad mode: the same rhythm with no 1:3 marks and no 1:3:7 gaps. And 8 models of
  random bits, with bit periods from 2 to 24 frames (21 to 255 ms): a mark or a gap lasts at least ¾ of a bit, then ends at each
  further bit with even odds, as on-off data keys.

Each model's log-likelihood is leaky, with a memory of 1000 frames (10.6 s) of carrier, and moves only while `ENV` sees a carrier.
The score is the best Morse model less the best of the others. It starts at 0 with the channel and follows a change of sender
within about ten seconds. At 20 WPM it passes 1 at 0.8 s after the first mark and 10 at 4 s, at +6 dB and at −6 dB alike. A
channel of noise beside it stays near −2. On the recordings, lines of Morse score up to +45, as vail-cli scores a line (below).
In the 26 with no Morse in them, no line scores above +2, and 99 % of their symbols are in lines scoring below 0.

Three details decide whether it works:

- **`STA`'s own model is left out.** `STA` adapts its timing to whatever it is given: it reads RTTY, as the 128 ms window sees it,
  as Morse at 80 WPM. The model at `STA`'s speed keeps only that speed, with the standard ratios, and is held to the speeds Morse is
  sent at. Fitting `STA`'s ratios too, even only a weighting (marks longer by as much as gaps are shorter), let real RTTY reach
  +33.
- **Every model sees the same per-frame evidence,** so only the timing is compared, never the signal's strength. In these models
  (not the bank's own), the spread of log length is widened by (0.7 frames / mean length)²: a length is known to about a frame,
  which at speed is a large part of a dot.
- **A run longer than any Morse element counts for neither side:** key-down beyond 9 units at `STA`'s speed, or key-up beyond 21.
  Otherwise a long carrier or a long pause decides the score, as one model or another happens to explain it far better than the
  rest.

The not-Morse models came in as each was needed. With one broad-mode model beside each of the bank's speeds, music got through;
a bank of them from 5 to 80 WPM stopped it. Random on-off data and synthetic RTTY still fitted Morse better than any of those,
until the random-bit models. Real RTTY then still passed through the model at `STA`'s speed, which followed `STA` up to 80 WPM,
until that model was held to 50 WPM. A check in the CLI for two tones keyed in turn was designed for RTTY, and was not needed.

## No change to decoding

The score only reports, behind `SPD_TUNE_MORSE` (on by default; 0 leaves it out, and `vail_channel_confidence` then gives 0).
`vail_decode`'s output, with and without `-a`, is byte for byte the same as the engine's before this change, on 435 files: the
synthetic grids used here and the 41 recordings. `tools/eval.py --seeds 4` gives the same output for both (0.2 % CER over
464 cases), and `tools/weak.py` the same table on the same audio (1440 cases, 23.16 % mean CER, 10 % CER at −8.2 dB).

`vail_decode -a -c` prints each channel's last and highest score beside its text, and `vail_decode -T` traces it as `morse=`,
with the best Morse (`k`) and other (`n`) model and the speed of the model at `STA`'s speed, every 25 frames.

## Cost

**Memory.** `Spd` grows from 17,672 to 28,904 bytes, so each channel's `Sig` from 39,008 to 50,240 bytes (+11.2 KB, +29 %). The
hazard tables of the fixed-speed models and of the not-Morse models are the same in every channel, so they are built once, by
the first `spd_init`, as `STA`'s split prior is, and every channel reads them: 13 KB in all. A new channel is cheaper than before
this change: `spd_init` built the bank's tables in 0.9 ms, and now takes 1.5 µs. Nothing is allocated per frame.

**CPU.** 22 more forward passes over `SEG`'s 91 duration states each frame on a channel with a carrier, beside the bank's 16:
`spd_update` takes 9.9 µs instead of 4.2 µs on one core of a desktop x86-64. Whole runs, CPU time, median of 5:

| | Before | Now | |
| --- | ---: | ---: | ---: |
| `vail_decode`, one station keying for 89 s (22 WPM, 0 dB) | 0.43 s | 0.64 s | +49 % |
| `vail_decode`, 140 s of a busy 40 m band | 3.90 s | 4.22 s | +8 % |
| vail-cli (5 engines over 200–3900 Hz), the same 140 s | 19.7 s | 21.3 s | +8 % |

The first is the worst case, a carrier on nearly every frame. The forward probabilities are doubles, as the bank's are, so on a
part with only a single-precision FPU the passes cost more than their count suggests.

## In vail-cli

`cli/src/band.c` reads each channel's score every frame. A line prints (once 60 characters are settled, after 4 s of quiet, or
when the channel retires) only if its keying scores at least `MIN_SCORE` (1.0): the channel's score at that moment, or its mean
over the frames since the line's text began, whichever is higher (`line_score`). Each catches what the other misses:

- **The mean** keeps a line whose station was followed by other keying before the line printed. On a 30 m recording, a QSO line
  (`… 5NN 5NN … TNX QSO … GL 73 …`) settled just as a weak, fading station began on the same pitch, and by then the score was −56.
- **The score at that moment** keeps a short message, during which the score is still rising: the mean alone loses 12 % of
  short messages on seed 2, against 3.6 %.

The full-screen view, and the signals shown below the lines on a terminal, list a signal while its unprinted text scores 1 or
more in the same way. `--all` shows everything, the count of lines left out is still reported, `--json` gives each line's
`score` (rounded down to a tenth, so that `score >= 1` picks the lines shown by default), and `text_tidy` is kept.
`text_looks_like_morse` is gone.

The mean was added after the recordings showed that QSO line lost. On the synthetic seeds it changes nothing at a threshold of 1
but 0.11 % of speech shown on seeds 0 and 1.

## How the threshold was chosen

The threshold was chosen on synthetic seeds 0 and 1 (`tools/notmorse.py`'s inputs, decoded with `--all --json`, so that each
line keeps its score), with the score at print time alone. There, short messages lose the same 6.3 % from 0.5 to 1.5, and speech
is gone from 1, so 1 is in the middle of the flat part. With the mean added, as the CLI now has it, and measured on seed 2 too:

| Seeds | Threshold | 0 | 0.5 | 1 | 1.5 | 2 |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| 0, 1 | short Morse lost | 5.9 % | 6.3 % | 6.3 % | 6.3 % | 11.0 % |
| | speech / SSB speech shown | 1.78 / 0.95 % | 0.78 / 0.25 % | 0.11 / 0 % | 0 / 0 % | 0 / 0 % |
| 2 | short Morse lost | 3.6 % | 3.6 % | 3.6 % | 11.9 % | 13.6 % |
| | speech / SSB speech shown | 0.51 / 1.58 % | 0.24 / 0.68 % | 0 / 0.26 % | 0 / 0.26 % | 0 / 0.26 % |

Music, RTTY, on-off data and pumped noise show nothing at any of these, and long messages lose at most 0.06 %. It is a constant,
`MIN_SCORE`, not an option.

## Recordings

41 recordings from [Freesound](https://freesound.org), listed at the end, decoded by vail-cli with `--all` and by default. 9 are
of CW (a busy 40 m band, a 40 m pile-up, a noisy 40 m receiver, 30 m, 15 m with 8 signals at once, hand keying, a numbers
station's code groups, a beacon, and shortwave beeps that are mostly Morse), 3 are mixed (tuning across a band, data or voices
with Morse), 3 are unknown (labelled CW or numbers by their uploaders), and 26 have no Morse in them (RTTY, FT8, JT65, PSK31,
other data, SSB, AM broadcast, static, the Buzzer, rooms, a pub, a crowd, a classroom, piano and jazz). Symbols printed:

| Recordings | Files | With `--all` | Text filter | Score |
| --- | ---: | ---: | ---: | ---: |
| CW | 9 | 5681 | 3766 | 2420 |
| Mixed | 3 | 5679 | 777 | 20 |
| Unknown | 3 | 2088 | 339 | 283 |
| No Morse | 26 | 46985 | 7879 | **55** |

Both CLIs give the same lines with `--all` on every recording, so they differ only in what they leave out. On the CW
recordings, most of what the score leaves out is garbage, but not all of it (see Limits). On the mixed ones, the Morse prints
with both, such as `E VVVDEGIL` at +10.8 after the voices and `U I9BH W` at +12.1 among the data, and what the score leaves out
is the voices, data and tuning. The 9 words in it that read as callsigns or prosigns are in lines such as `E E E E DE` and
`<SK> AE8VM 1PS , 0E M5HC CJ1`, which all score between −1 and −40. What the 3 unknown recordings should print is not known.

## Limits

- **A short message** can be over before its channel scores 1. Of the 49 short-message symbols lost, 25 were at −6 dB and below,
  16 were misread anyway (`TNST` for `TEST`, `TH` for `TU`), and 8 were a correct `TEST`, once at 0 dB and once at +3 dB with
  fading. The text filter's worst case, `TEN NET ANT TAN`, now prints, as its keying is Morse whatever its letters.
- **Real text is lost on busy bands.** On the 9 CW recordings, of the words that read as callsigns, reports, prosigns or
  dictionary words, the text filter printed 250 and the score prints 208; of all other words, most of them garbage, 1393 and
  795. The real words lost are on channels whose keying scores far below 0, not near the threshold: at −2 only 11 more would
  print, and the recordings with no Morse would print 8934 symbols, more than the text filter's 7879.
- **Keying far from the standard timing scores low.** The clearest case is a contest station on the 15 m recording, at about
  45 WPM with dots of 3 frames and element gaps of 2: a unit of about 27 ms, the time scale of RTTY's 22 ms bits, at which 10.6 ms
  frames blur the 1:3 structure the score tests. That station, HA5JI, has three lines: the score prints
  `O HA5JI TEST HAMSJI TEST HA5JI HA5JI TEST TEST` at +4.3, and leaves out the other two, at −20.3 and −8.2, which the text
  filter printed. Accepting keying like it would take weighting on the Morse side, which let real RTTY reach +33. The beacon's
  one line, `CS3B O MWAO`, is lost too: the score reached +12 on the callsign, then fell as a fading carrier followed it.
- **Garbage that follows a station** on the same channel can print while the score is still high, such as `G T T` at +8.1 on
  the 15 m recording, right after `IA3TH NA 5NN 72 T` (+15.0) at the same 2008 Hz.
- **Per-character confidence** is a separate measure, `vail_char.confidence`: see [char-confidence.md](char-confidence.md).

## A callsign vocabulary

libmorsedec (see [libmorsedec-comparison.md](libmorsedec-comparison.md)) keeps a dictionary of callsign shapes, Q codes,
reports and prosigns, and lets it choose among alternative characters before committing them. Its grammar is the one `WRD`
already uses to set word ends and word classes. It has no channel confidence: only a per-frame cap on keying probability from
tonal contrast, which helped impulse noise in vail but cost more elsewhere, and a per-character `low_snr` flag.

As evidence that a channel is Morse, the vocabulary is strong but not safe. On the recordings, callsign-shaped words are 18.9 %
of the words in lines that score 5 or more, and 0.16 % of those from recordings with no Morse; reports are 6.0 % and 0.02 %.
Tried with the score as the line prints (before the mean was added), at 3 nats for each callsign or report, at most 6, with half
carried to the channel's next line, it let 8 more lines through: about half with callsigns that look real (`SP8CF 5NN 1D`,
`W1KK`), and the rest garbage, such as `T T N UR1AL C` from a crowd in a room. At 4.6 nats a word, synthetic audio that is not
Morse went from 0.03 % to 0.31 % shown. It was left out, as what it gains is lines that are as much garbage as text, and the
mean now lets `SP8CF 5NN 1D` through on its own. It would fit better as presentation, to highlight callsigns and
reports, than as a gate.

## Measuring again

```sh
python3 tools/notmorse.py --seeds 3 --show --wav recordings/*.wav    # the tables above
./build/vail_decode -a -c recording.wav                               # each channel's last and highest score
python3 tools/eval.py --seeds 4; python3 tools/weak.py               # decoding: unchanged with SPD_TUNE_MORSE on or off
```

`tools/notmorse.py` stops if vail-cli fails on a file, rather than counting it as printing nothing.

## The recordings

The high-quality MP3 preview of each was converted to 8 kHz mono 16-bit WAV with ffmpeg, some cut to their first 300 s. None of
them is in this repository.

| Freesound | Label | Licence | What it is |
| --- | --- | --- | --- |
| [698986](https://freesound.org/s/698986/) | cw | CC-BY-4.0 | klankbeeld: 8 Morse channels at once, 21018.3 kHz, wide filter |
| [204968](https://freesound.org/s/204968/) | cw | CC0 | Buss1000: lots of CW conversations, 40 m 7000-7012 kHz, various filters |
| [343740](https://freesound.org/s/343740/) | cw | CC0 | NebulousRoyale: hand-sent Morse at 4606.2 kHz, Poland |
| [254528](https://freesound.org/s/254528/) | cw | CC-BY-3.0 | kwahmah_02: 30 m hams, Twente WebSDR |
| [250163](https://freesound.org/s/250163/) | cw | CC-BY-3.0 | kwahmah_02: shortwave beeping, mostly Morse |
| [255771](https://freesound.org/s/255771/) | cw | CC-BY-3.0 | kwahmah_02: M03 numbers station in Morse (code groups) |
| [752469](https://freesound.org/s/752469/) | cw | CC0 | xperseai: noisy Morse, 40 m, Kenwood R-2000 |
| [65296](https://freesound.org/s/65296/) | cw | CC-BY-NC-3.0 | KGJones: 40 m pile-up calling a DX station |
| [30302](https://freesound.org/s/30302/) | cw | CC-BY-NC-4.0 | acclivity: beacon CS3B in static, down to 0.1 W |
| [396843](https://freesound.org/s/396843/) | mixed | CC-BY-NC-3.0 | gilgsn: static and wrong-sideband voices, then Morse |
| [260162](https://freesound.org/s/260162/) | mixed | CC-BY-4.0 | AlienXXX: data and Morse transmissions, tuning the dial |
| [243546](https://freesound.org/s/243546/) | mixed | CC0 | kb7clx: slow tune through 40 m from 7000 kHz, LSB (first 300 s) |
| [829419](https://freesound.org/s/829419/) | unknown | CC0 | Concord4: digital node at 7045 kHz called CW |
| [847169](https://freesound.org/s/847169/) | unknown | CC-BY-NC-4.0 | araguet: WebSDR 7040.9 kHz |
| [421705](https://freesound.org/s/421705/) | unknown | CC0 | funkmeldekopf: number stations 4590 kHz |
| [238845](https://freesound.org/s/238845/) | not | CC0 | galeku: RTTY, 14071 kHz (first 300 s) |
| [625314](https://freesound.org/s/625314/) | not | CC0 | Bryce835: RTTY and FT8, 3581 kHz, static (first 300 s) |
| [242991](https://freesound.org/s/242991/) | not | CC0 | kb7clx: JT65 and PSK31, 14078.5 kHz |
| [346112](https://freesound.org/s/346112/) | not | CC0 | NebulousRoyale: data transmission, 14664.8 kHz |
| [244622](https://freesound.org/s/244622/) | not | CC-BY-4.0 | copyc4t: digital data stream, 12956.7 kHz |
| [244621](https://freesound.org/s/244621/) | not | CC-BY-4.0 | copyc4t: blips at regular intervals, 12956.7 kHz |
| [243518](https://freesound.org/s/243518/) | not | CC-BY-4.0 | copyc4t: static with blips, 11846.8 kHz |
| [847258](https://freesound.org/s/847258/) | not | CC0 | Concord4: the Buzzer (UVB-76) with a jammer |
| [398610](https://freesound.org/s/398610/) | not | CC0 | klankbeeld: weak 20 m SSB with much static, 14237 kHz |
| [397389](https://freesound.org/s/397389/) | not | CC0 | kb7clx: 20 m SSB on Field Day, 14260 kHz (first 300 s) |
| [396869](https://freesound.org/s/396869/) | not | CC0 | kb7clx: 6 m USB, two receivers, Sporadic-E (first 300 s) |
| [455408](https://freesound.org/s/455408/) | not | CC-BY-NC-4.0 | W1ZY: SSB pile-up calling Japan (first 300 s) |
| [548441](https://freesound.org/s/548441/) | not | CC0 | endoplasmian: echoed phonetic alphabet voice, 8992 kHz |
| [863022](https://freesound.org/s/863022/) | not | CC0 | Jared_DiCarlo: Indonesian chanters near 7000 kHz |
| [670143](https://freesound.org/s/670143/) | not | CC0 | jess90: shortwave AM noise from a WebSDR |
| [845140](https://freesound.org/s/845140/) | not | CC-BY-4.0 | Funkelfang: shortwave static, tuning and faint voices |
| [626117](https://freesound.org/s/626117/) | not | CC0 | klankbeeld: two weak AM broadcast stations interfering |
| [521851](https://freesound.org/s/521851/) | not | CC0 | lartti: searching the shortwave bands, Tecsun PL-380 (first 300 s) |
| [396902](https://freesound.org/s/396902/) | not | CC0 | vedas: full MW and LW sweep with static |
| [455908](https://freesound.org/s/455908/) | not | CC0 | quadraslayer: people talking in a small room |
| [457286](https://freesound.org/s/457286/) | not | CC0 | Zac_Gist: noisy pub |
| [608996](https://freesound.org/s/608996/) | not | CC0 | Psykophobia: crowd chatter, 40 people (first 300 s) |
| [869656](https://freesound.org/s/869656/) | not | CC0 | hawa9874: classroom ambience |
| [135883](https://freesound.org/s/135883/) | not | CC-BY-3.0 | roby_caso: Bach fugue in G minor, grand piano |
| [570347](https://freesound.org/s/570347/) | not | CC0 | szegvari: late jazz piano |
| [521159](https://freesound.org/s/521159/) | not | CC-BY-4.0 | leonseptavaux: piano recorded with four mics |
