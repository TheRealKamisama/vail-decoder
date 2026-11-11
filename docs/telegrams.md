# Telegrams: real recordings with a known text

IZ4KBW's public CW archive on Google Drive has a folder, AA_telegrams, of telegrams sent in Morse, each with a photo of the
telegram: Western Union forms from 1922 to 1963 and Italian ones from the 1910s to the 1960s. `tools/telegrams.tsv` holds
the text of 15 of them, read from the photos, with each recording's Drive id. These are the only real recordings with a
full text that the project has; the rest of the archive is QSOs labeled by call sign only
([real-audio-plan.md](real-audio-plan.md)).

```sh
python3 tools/telegrams.py DIR --fetch               # download into DIR (not the repository), convert, score build/
python3 tools/telegrams.py DIR --builds build,build/v_NAME --show
```

They are not off-air signals. Most are telephone recordings (the AMR codec) of a keyed tone played in a room, 90–500 s
each, at about 13–25 WPM. That makes them hard in ways the synthetic grids are not: echo, tones with strong harmonics, and
in some files the cleanest keying above the 1195 Hz top of the spectrum.

## Results (engine 0.2.0)

CER is counted on letters, digits and `/`, the telegram aligned to the decoded text with free ends (see the tool's header).

| | CER |
| --- | ---: |
| best channel of each file | 41.9 % |
| channel with the highest Morse score | 44.8 % |
| host text (the current channel) | 60.4 % |
| decoder pinned at the best channel's pitch | 41.6 % |

Six files are copied well (1–30 % pinned: `09-mar-1927`, `1922-nov-2`, `1938-sept-9`, `19401225`, `19410517`,
`19451023`, the last at 1 %), two partly (43–47 %), and seven badly (62–84 %).

**The host follows channels that are not Morse.** In `1922-nov-2` and `19401225` the host text is 80 % wrong, against
17–20 % on the right channel: the current channels' Morse scores (`vail_channel_confidence`) ended the file at −31 to −56
and never rose above −8, while the right channels' ended at 0 to +5 and peaked at +6.5 and +15. Taking the channel with the highest score would bring the host from 60 % to about 45 %.
Channel selection does not use the score, because decoding is kept independent of `SPD_TUNE_MORSE`.

**Echo makes STA lock at two to three times the speed.** The keyed tone (about 500 Hz in most files) rings: the gap
between the elements of a letter dips only about 10 dB, while the gap between letters falls to the floor. `SEG` then cuts
long key-downs into pieces (in `5.mp3`, 48 % of the marks and 68 % of the gaps are one frame or less), and `STA` settles
at 45–60 WPM for a sender at about 20. In `19630728` the speed bank found 21 WPM, but STA stayed at 50–58 for the whole
file; pinned, it was restarted after 45 s and the rest decoded well. The second harmonic (near 1000 Hz) rings less, and in
`09-mar-1927` it decodes at the right speed from the start, while the fundamental gives junk for the first 75 s.

**Harmonics above the spectrum.** Some files key at 830–850 Hz with the cleanest keying in the harmonics at 1500–2500 Hz,
which `FE` does not see. That is a property of these recordings rather than of band signals.

## Character confidence

On the pinned decodes, the share of wrong / right characters below each confidence:

| | wrong | right | AUC | < 1 | **< 3** | < 5 | < 8 | < 12 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 8 files under 50 % CER | 271 | 1247 | 0.753 | 24/3 | **44/10** | 58/21 | 77/44 | 94/73 |
| all 15 files | 661 | 1502 | 0.722 | 23/6 | **46/16** | 59/27 | 78/49 | 94/75 |

Where the decoder copies, the threshold of 3 fades 44 % of the wrong characters and 10 % of the right ones, about as on
the synthetic grid at −8 to −10 dB (50/5 to 54/14, [char-confidence.md](char-confidence.md)). Where it locked at the wrong
speed, much of the junk scores as confidently as real copy.

## The transcriptions

The text is the photo's, header lines included, as an operator would send it: `&` as `ES`, punctuation left out. Whether
each header was sent is not known, so a missed header counts as errors. The photo of `5.mp3` is handwritten and the least
certain. `Tfc-list.mp3` in the same folder has no photo and is left out.
