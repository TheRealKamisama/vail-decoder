# Plan: labeled real audio from public receivers

Status: planned, not started (2026-11-11).

Every number in [char-confidence.md](char-confidence.md), [weak-signal.md](weak-signal.md) and most of
[engine-confidence.md](engine-confidence.md) comes from synthetic audio. No public set of off-air CW with checked
transcripts exists (a search in November 2026 found only small or unlabeled collections, and projects asking for
recordings). This plan builds one from transmissions whose text is known in advance, recorded through public KiwiSDR
receivers, and uses it to check decoding (CER), the character confidence threshold and the channel Morse score.

## Sources with known text

| Source | What is known | Gives | Limits |
| --- | --- | --- | --- |
| NCDXF/IARU beacons | callsign and time slot from the published schedule | real HF fading and noise; a power ladder in every slot | short fixed text, one speed |
| W1AW code practice and bulletins | the text ARRL publishes for each session | long text at 5–40 WPM, real propagation | ARRL text and audio may not be redistributed |
| Contest and pileup audio, with Reverse Beacon Network spots | callsigns, frequency, time and speed from the spots | real keying, fast senders, crowded bands | only callsigns are labeled |
| IZ4KBW's off-air QSO archive | participant callsigns, key type and sometimes speed in each file name | hand keying (paddle, bug, straight key) on 80 and 40 m | no transcripts; partial labels only |
| IZ4KBW's AA_telegrams folder (done: [telegrams.md](telegrams.md)) | the full text, from a photo of each telegram | 15 complete messages with headers | telephone recordings of a tone in a room, not off-air |

**Beacons.** The NCDXF/IARU network has 18 beacons that take turns on 14.100, 18.110, 21.150, 24.930 and 28.200 MHz: each
sends its callsign and then four one-second dashes, stepping its power down, in a 10 s slot that repeats every 3 minutes.
(To be checked against ncdxf.org before building: the order of beacons, the speed, the power steps.) The schedule gives the
label for any slot; the dashes give the SNR at four levels 10 dB apart from the same station, which is what the character
confidence threshold needs. A Kiwi's GPS time places the slot to well under a second.

**W1AW.** Code practice runs at fixed times on fixed frequencies (ARRL's operating schedule), with the text of each session
published afterwards in the code practice archive. Recorded off-air from receivers at different distances, the same text
arrives with different SNR and fading. ARRL does not allow its material to be reproduced, so recordings and texts stay
local; the repository keeps only the manifest and the scripts.

**Callsign labels.** For contests and busy bands, the Reverse Beacon Network publishes every spot (callsign, frequency,
time, speed, SNR) as daily archives. A recording of the same band at the same time is labeled at the callsign level: how
many spotted calls the decoder printed, at what confidence, and how much other text it printed. IZ4KBW's file names give
the same kind of label for ragchew QSOs.

## The recorder

`tools/kiwirec.py` (to write), on top of the open-source KiwiSDR client (`kiwiclient`, its `kiwirecorder.py`; licence to
check before vendoring, otherwise required as an external tool):

- **Input**: a schedule of jobs (receiver, frequency, mode and passband, start, duration, label source).
- **Output**: one WAV per job (12 kHz from the Kiwi, mono, CW passband about 500 Hz wide with the tone near 700 Hz), and a
  manifest line: file, receiver, its location, frequency, start (GPS time), duration, label kind, label.
- **Politeness**: one connection per receiver at a time, sessions of a few minutes, a gap between sessions, no recording
  of receivers that are full (all channels in use), and a generic client name. Public Kiwis limit session time and channel
  count; the recorder must respect that and give up cleanly when refused.
- **Beacon mode**: given a band and a window, it records continuously and slices by the schedule afterwards into one clip
  per slot (callsign, beacon location, distance to the receiver, the four dash levels measured).
- **W1AW mode**: records the scheduled session on the frequency that the receiver hears best (a short probe of each
  frequency first), then fetches that session's text from the archive into the local data directory.
- **RBN mode**: records a band for a window and downloads the RBN archive for that day, keeping the spots that fall inside
  the passband and the window.

WebSDR (websdr.org) sites have no documented client interface, so they are left out unless KiwiSDR coverage is not enough.

## Finding good receivers

The public KiwiSDR list (kiwisdr.com) gives each receiver's location, antenna, number of channels and users. For each band:

1. Take receivers at a range of distances from the sources (beacons are worldwide; for W1AW, North America and Europe).
2. Probe each with a 3-minute beacon cycle and keep the ones where vail decodes at least a few callsigns: this ranks them
   by real noise floor and local interference, not by their own description.
3. Keep a short list per band and time of day, and spread recordings over several receivers so that one site's noise does
   not dominate the set.

## Labels and scoring

- **Beacons**: the label is the callsign. `tools/charconf.py` already aligns a decoded text with a reference; with a
  manifest (`--manifest labels.csv`, to add) it scores each clip against its label, and the dashes are left out of the
  reference. Tables by measured SNR and by distance.
- **W1AW**: the published text, upper-cased, with the archive's control characters mapped to prosigns. The decoder's text
  is aligned to it as now; a session that the decoder lost entirely (no carrier) is counted separately, not as 100 % CER.
- **Callsign labels**: recall of the listed callsigns, the confidence of those characters, and the amount of other text
  (all of it counted as unlabeled, not as wrong).
- **No Morse**: the same receivers on SSB, RTTY and FT8 segments, for false text and the Morse score
  (`tools/notmorse.py --wav`).

## Steps

1. Check the beacon schedule, the W1AW schedule and frequencies, and kiwiclient's licence and protocol.
2. Write `tools/kiwirec.py` with beacon mode, record one cycle from three receivers, and check the slicing by ear and
   against the schedule.
3. Add `--manifest` to `tools/charconf.py`; measure the beacon set (a few hundred clips over a week, several bands and
   times of day).
4. Add W1AW mode; record a week of sessions at several speeds from two or three receivers.
5. Add RBN mode for one contest weekend.
6. Write up the results beside the synthetic ones: CER, the confidence threshold (keep 3, or move it), the Morse score's
   false-text rate. Retune only what the real audio shows to be off.

## Data handling

Recordings, ARRL texts and RBN archives stay outside the repository (as the Freesound clips in
[engine-confidence.md](engine-confidence.md) did), in a directory given on the command line. The repository keeps the
scripts, and a manifest of what was recorded when and where, so that a set can be described and re-recorded.

## Open questions

- Which band and time windows give the most beacon slots heard per hour from a receiver in Europe or North America.
- Whether the W1AW archive text matches what was sent closely enough to score against (bulletins can differ from the
  published text).
- How to label hand-keyed QSOs beyond callsigns: copying a few minutes by ear is the only exact method found so far.
