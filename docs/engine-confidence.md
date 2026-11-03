# Engine confidence: what a later revision needs

The engine always prints its best Morse reading of anything that keys on and off, and never says how far to trust that reading.
vail-cli now hides the worst of the result by judging the text, which is as far as a host can go. This note records the
problem, what the CLI does about it, and what the engine needs in order to do better. The engine has not been changed for
this, except for a read-only `vail_channel_stats`.

## The problem

Noise and copyable CW are not the problem: white noise, filtered noise and static print nothing, and a station is copied
well down to about −9 dB in 2500 Hz. Audio that keys but is not Morse is. These are lines and symbols from
`tools/notmorse.py` (3 seeds, 60 s each, so 180 s of each input), with `--all`, which shows what the engine gives:

| Input | Lines | Symbols | What it looks like |
| --- | ---: | ---: | --- |
| Speech | 837 | 6522 | `E E  EE`, `T ETTEI  T`: dozens of short-lived channels |
| SSB speech | 794 | 7079 | the same |
| Music | 143 | 3024 | `9 5 4 7 1 89  8 P 6R` |
| RTTY | 29 | 1449 | `IENNWRYL1A3NA4 / MEISSTAH5ESL5LH`: steady, at exactly its two tones |
| On-off data, not Morse | 5 | 215 | `ATNNWAE MTTQATM A3TE` |
| Noise with pumping gain | 2 | 4 | `T`, `A` |

**All of this audio is synthetic, the speech and music most of all, and none of it has been on the air.** The first thing the work
below needs is real recordings; see "What is needed from you".

## What the CLI does now

`text_looks_like_morse` in `cli/src/text.c` judges the last 160 bytes of a channel's text when a line is about to print, and when a
signal is about to be listed. With 12 symbols or more, it rejects text where:

- over 45 % of the words are a single one of E, T, I, A or N;
- over 55 % of the words are a single symbol of any kind, and at least 4 different symbols do it;
- over 72 % of the symbols are those five letters and words average under 2.5 symbols.

With fewer than 12, it needs a digit, or a word of two or more symbols that is not only those five letters. `--all` turns this off,
and a note on stderr counts the lines left out. Measured over the same audio, shown by default:

| Input | Symbols shown | |
| --- | ---: | --- |
| Speech | 220 of 6522 | 3 % |
| SSB speech | 213 of 7079 | 3 % |
| Music | 289 of 3024 | 10 % |
| RTTY | 1449 of 1449 | **100 %** |
| On-off data | 215 of 215 | **100 %** |
| Real CW, long messages (162 cases, 20 to −12 dB, fading, static, hand keying) | 10167 of 10169 | 2 lost |
| Real CW, short messages (162 cases) | 845 of 910 | 65 lost |

The thresholds were set while looking at this data, so these figures are optimistic.

The short messages lost are mostly `TEN NET ANT TAN` and text already misread, such as `T W` for `TU`. A few are real: `T U` for a `TU`
with its letters split apart, and `TEST` read as `TE AT`. A real message of only those five letters, and under 12 symbols, is not
printed, and no text check can tell it from a fragment of speech.

**What a text check can never do** is separate RTTY from real code groups. `XQZ7 JW4K 8ZR2` and `IENNWRYL1A3NA4` are both random
letters, so the check lets both through, and `tools/notmorse.py` includes code groups among its real messages so that it has to.
A difference exists only in the keying: one has dashes three dots long, and the other does not. That is below where a host can see.

## What the engine has today

These were checked in the code at the time of writing, and none is a measure of whether a channel is Morse:

- `vail_char.lowSnr` looks like one. It is set only in `chr.c`, from `SEG`'s `qualityFlag`. `cvd.c`, which decodes by default
  (`CVD_TUNE`), never sets it, so hosts always read 0.
- `API_Snr` and `vail_channel_stats` give the energy of the keyed envelope (`env.envMeanSquare`), so a loud voice has a high SNR.
- `API_Wpm` gives what `STA` has fitted, whether or not the fit is good.
- `MSG` keeps characters and word ends, with no score.

## What is needed

1. **A score for each channel:** how well its recent keying fits Morse, against the best explanation that is not Morse. For example,
   `int vail_channel_morse_score(const vail_api *h, int idx)`, or a field next to the others in `vail_channel_stats`, given as a log-odds
   so that hosts can pick their own threshold. It must exist before any text does, and must recover after a change of sender, so it
   needs to forget, as `SPD`'s scores do.
2. **No change to decoding.** The score only reports. `tools/eval.py` and `tools/weak.py` must give identical results with it on, and
   it goes behind a `*_TUNE_*` switch like the others.
3. **Still embeddable.** No new dependencies, no allocation per frame, and a measured cost per channel, since this is meant for
   firmware too. `SPD` runs 16 forward passes a frame already, so a further model is not free.
4. **Per-character confidence, if it comes cheaply.** It would let a host mark doubtful letters in weak copy, like `K 8 . C TN1ABC`. It is a
   separate question from whether a channel is Morse at all, and it should not hold up the channel score. Setting `lowSnr` from `CVD`
   would be the place for it, or else that field should be retired.

## Where a score could come from

**None of these has been tried.** They are in the order I would try them.

- **`SPD`'s likelihoods against a model that is not Morse.** `spd_update` keeps a leaky forward log-likelihood, `score[k]`, for 15 fixed
  speeds and the current model, all with Morse timing. What is missing is the other side of the comparison: a model of the same
  observations with no 1:3 structure, such as a single broad distribution of mark and space lengths. The margin between the best
  speed and that model is the most direct measure of "this keying is Morse", it is already running on the right input, and one more
  model costs little. This is the one to start with. It should separate RTTY, whose lengths come in multiples of about 22 ms and
  are blurred by a 128 ms window, but that needs measuring.
- **How decisive `STA`'s element classes are.** `sta_classify` scores every mark as dot or dash and every gap by its class. On real keying the
  winning class should dominate, and on other audio the scores should be close. A leaky mean of that margin is cheap. The worry is
  that `STA` adapts to what it sees and has strong priors, which may make any input look well fitted. `windowLogLik` has the same worry.
- **Purity of the tone, from the spectrum.** A keyed carrier has a steady level when on and is narrow. A voice moves and varies. `ENV`
  already keeps per-bin rings. This would help with speech and music, which the CLI now handles, but not RTTY, whose tones are also steady
  and narrow, so it is lower priority.
- **Two tones that key in turn, in `cli/src/band.c`.** RTTY's two tones are on when the other is off. `judge` already compares channels in
  the spectrum, so this needs no engine change, and it could be tried first if only RTTY matters. It would not catch single-tone data, and
  says nothing to other hosts of the engine.

## How to know it worked

`tools/notmorse.py` gives the tables above, and `--wav` adds real recordings. With a threshold chosen on one part of the data and
measured on the rest:

- RTTY and on-off data fall below 10 % of symbols shown. Today they are 100 %.
- Real CW keeps at least 99 % of long-message symbols at every condition in the table, and code groups print whole.
- Short real messages lose less than today's 7 %, as the check no longer has to judge them from their letters.
- `tools/weak.py` and `tools/eval.py` are unchanged, and `cli/tests/e2e.py` still passes.
- Any real recording improves or stays the same.

With that, most of `text_looks_like_morse` can go. Keep `--all`, the count of hidden lines, and the tidying in `text_tidy`.

## What is needed from you

1. **Real recordings.** `vail-cli -r sample.wav` records 8 kHz WAV while it decodes, and is what to send. Most useful is what prompted
   this: the audio from when it printed garbage. After that, a few minutes each of a busy CW band, SSB, RTTY or FT8, and whatever a default
   microphone hears in a room. Synthetic speech is a guess at speech.
2. **A decision on what the score is for.** Either hosts hide channels that score low, as the CLI does now, or the score is only shown. It
   changes how far down the threshold can go.
3. **A cost limit for firmware.** How much CPU per channel, and on what part, decides whether a further model in `SPD` is acceptable.
