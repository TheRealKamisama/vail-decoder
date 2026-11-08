# vail-cli

Decode every CW (Morse) signal your rig's sound card can hear. Think of it as a command-line CW Skimmer, built on the
[vail engine](../README.md) in this repository. It runs on Linux, macOS and Windows.

```console
$ vail-cli -d "USB Audio"
 vail-cli  USB Audio CODEC  200-3900 Hz                                 15:33:42 UTC   -14 dBFS [########..]
 SIGNALS  4
     Hz  SNR  WPM  STATE   TEXT
    609   18   24  decode  CQ CQ CQ DE K1ABC K1ABC K1ABC PSE K UR RST 599 599 NAME IS JOHN
   1203   14   18  decode  VVV VVV DE DL2XYZ DL2XYZ QTH MUNICH MUNICH RIG IS F
   2109   19   30  pause   .. W1AW W1AW TEST W1AW 5NN 123 TU TEST W1AW 5NN 124 TU QRZ TEST W1AW W1AW TEST W1AW
   3047    8   22  decode  CQ DX CQ DX DE JA1ZZZ JA1ZZZ UP 5 K R R TNX FER CALL UR RST 44
 LOG  2 lines
15:33:10  2109  TEST W1AW W1AW TEST W1AW 5NN 123 TU TEST W1AW 5NN 124 TU QRZ TEST W1AW W1AW
15:33:39  2109  TEST W1AW

 q quit  c clear  Up/Down PgUp/PgDn scroll  ? help
```

Each signal has a row that updates in place, and finished lines collect in the log below. Words the decoder takes for
callsigns are highlighted, in bold magenta (bold with `NO_COLOR`). When you quit, the terminal comes back as it was and the
lines are printed there, so nothing is lost with the screen.

Send stdout anywhere else and it is finished lines of text, always clean:

```sh
vail-cli | grep --line-buffered K1ABC        # watch for a callsign
vail-cli >> band.log                         # keep a log
vail-cli -T >> band.log                      # keep a log and watch the full-screen view
vail-cli --json | jq -r .text                # JSON Lines, with SNR, speed, Morse score and callsigns
vail-cli --json | jq -r '.callsigns[]'       # only the callsigns heard
vail-cli recording.wav                       # decode a recording (WAV, FLAC, MP3)
vail-cli --plain                             # lines on the terminal, as for a screen reader

arecord -t raw -f S16_LE -r 48000 | vail-cli -R 48000 -      # raw samples from any program
rtl_fm -M usb -f 7.03M -s 8k | vail-cli -                    # 8 kHz, 16-bit mono is the default
```

## Use

```
usage: vail-cli [options] [FILE]

Input:
  -l, --list            list the sound input devices and exit
  -d, --device DEV      use this device: its number in the list, or part of its name
  -c, --channel N       decode only this channel of the input (1 left, 2 right; default: mix them)
  -b, --band LO-HI      only decode signals between LO and HI Hz (default: 200-3900)
  -s, --seconds N       stop after N seconds of audio
  -r, --record FILE     also record the audio to a WAV file (8 kHz, for replaying later)

Raw samples on stdin, always little-endian:
  -R, --rate HZ         samples per second (default: 8000)
  -F, --format FMT      s16 or f32 (default: s16)
  -n, --channels N      interleaved channels (default: 1)

Output:
  -T, --view            full-screen view, on the terminal whatever stdout is
  -p, --plain           finished lines of text only, as on a pipe
  -j, --json            finished lines as JSON Lines, with SNR, speed, Morse score and callsigns
  -a, --all             also show text whose keying does not fit Morse, such as speech or RTTY

  -h, --help            show this help and exit
  -V, --version         show the version and exit
```

There is nothing to configure. With no options it listens to the system's default input. Set the rig to a wide filter to see
the whole passband, and keep the input level out of clipping: vail-cli tells you if it isn't. Both channels of a stereo input
are mixed unless `--channel` picks one, so dual-receiver rigs decode both receivers.

Keys in the view: `q` quits, `c` clears the log, the arrows, `PgUp`, `PgDn`, `Home` and `End` scroll it, and `?` shows help. The
SNR column is the engine's own estimate, in its narrow bandwidth, so it reads higher than an SNR in 2500 Hz.

Text can be revised as the decoder learns more, so a line is printed only once it is final: when a signal has been quiet for 4
seconds, or as soon as 60 characters are beyond the decoder's reach. Each line is the time the text started (UTC, or from the
start of a file or stream), the pitch in Hz, and the text. Prosigns are written `<AR>`, `<SK>`, `<BK>`.

If `vail-cli -` is left with nothing on stdin, or its stream goes quiet, Ctrl+C and the view's keys still work.

Exit status is 0 on success, on Ctrl+C, or when whatever was reading the output goes away, as in `vail-cli | head`. It is 1 on a
runtime error such as a missing device, a device that stops, output that cannot be written, or `--view` with no terminal to
use, and 2 on a usage error. If a recording cannot be written, that is reported at once, decoding goes on without it, and the
status is 1.

## Text that is not Morse

The decoder reads anything that keys on and off as Morse, so speech, music and data modes such as RTTY come out as letters: many
short pieces such as `E E  EE`, or random-looking text. The engine also scores each signal's keying, by how much better Morse
timing explains it (dashes three dots long, letter gaps of three and word gaps of seven) than the best explanation that is not
Morse. A line is printed only if its keying scores at least 1, either as the line is finished or on average while its text came
in, and the full-screen view lists a signal only while its text scores that well, which takes a second or two of Morse. A note
on stderr says how many lines were left out, `--all` shows everything, and `--json` gives each line's score.

On 26 recordings with no Morse in them, from RTTY, FT8 and PSK31 to SSB, a crowded room and a piano, 55 of 46985 characters
were printed. On synthetic audio, 3 minutes of each:

| | Characters with `--all` | Printed |
| --- | ---: | ---: |
| Speech, SSB speech, music, RTTY, on-off data, noise | 18293 | 10 |
| Morse, 162 long messages, from 20 dB down to −12 dB, with fading, static and hand keying | 10169 | 10167 |
| Morse, 162 short messages such as `TU` and `R 73` | 910 | 861 |

The score judges timing, so:

- A message of only a second or two can be over before it scores 1, most often at −6 dB and below.
- Keying far from the standard timing, such as a fast contest station that clips its spacing, can score low and be left out.

How the score works, and what it was measured on, is in [docs/engine-confidence.md](../docs/engine-confidence.md).

## Install

Binaries for Linux (x86_64), macOS (Apple silicon and Intel) and Windows (x86_64) are on the
[releases page](https://github.com/TheRealKamisama/vail-decoder/releases). Each archive holds the program, its man page,
this README and the license. The macOS binaries are not signed, so one downloaded with a browser first needs
`xattr -d com.apple.quarantine vail-cli`.

To build it, only a C compiler is needed; audio libraries are found at run time. ncurses is optional, for the full-screen view.

```sh
git clone https://github.com/TheRealKamisama/vail-decoder
cd vail-decoder/cli
make
sudo make install        # to /usr/local; or use PREFIX=...
```

`make` uses ncurses if it can find it. Without it you get everything except the view: on a terminal, finished lines with the
signals being heard below them. `vail-cli --version` says which you have. `make NCURSES=1` fails if it is missing, and `make
NCURSES=0` leaves it out.

- **Linux** needs PulseAudio, PipeWire, JACK or ALSA at run time, which any desktop already has. For the view, install
  `libncurses-dev` (Debian, Ubuntu), `ncurses-devel` (Fedora) or `ncurses` (Arch).
- **macOS** needs the Xcode command line tools to build, which include ncurses. The first run asks for microphone permission for
  your terminal; without it you get silence, which vail-cli reports as `input is silent`.
- **Windows** builds with MSYS2/MinGW (`pacman -S make mingw-w64-x86_64-gcc mingw-w64-x86_64-ncurses`, then `make`), or
  cross-compile from anywhere, without the view: `make CC="zig cc -target x86_64-windows-gnu" TARGET=Windows`. The view needs a
  real console rather than a pipe-only terminal such as mintty.

## How it works

```
sound card ─ miniaudio ─ 8 kHz mono ─ FFT ─┬─ slice 1 ─ vail engine ─┐
  or file                     0–4 kHz      ├─ slice 2 ─ vail engine ─┤
                                           ├─   ...                  ├─ one station each ─ final text ─ stdout
                                           └─ slice 5 ─ vail engine ─┘
```

In `src/`: `audio.c` gets samples, `band.c` turns them into lines, `text.c` tidies them, `output.c` writes stdout,
`view.c` is the full-screen view, `options.c` is the command line, and `main.c` runs it all.

- **One engine sees 1 kHz, so five share the band.** vail decodes 128 bins of a 1024-point FFT. Instead of
  mixing audio down five times, one FFT is run and overlapping slices of its spectrum go to five unmodified
  engines. They cover 203–3914 Hz, and each signal decodes as well as it would with an engine to itself.
- **A channel is a station only if it keys independently.** Sidelobes, key clicks, harmonics of a clipping
  sound card and the same signal seen by two slices all carry energy only while something stronger is keyed.
  A real station's energy has nothing to do with its neighbour's keying. Each channel is checked against the
  last 5.4 s of spectrum before any of its text is shown.
- **Only final text is printed.** The engine only ever revises a channel's last 26 characters, which is what
  makes a clean append-only output possible.

## Test

```sh
make test                  # known stations in, text and ghosts scored; the view on a pseudo-terminal
python3 tests/e2e.py --table   # the same, showing every station's text and every ghost
python3 ../tools/notmorse.py   # speech, music and data modes in, what prints and what real text is lost
```

Test audio is synthesised with the engine's own generator, covering noise, stations from 250 to 3400 Hz, up to
ten at once, the slice overlaps, a receiver's filter, clipping, long messages, and file, device and write errors.

No automatic test opens a sound card. Live capture has been tried on Linux through ALSA, with samples supplied by a test, so it has
not met real hardware, nor macOS or Windows. The view's tests run on Linux and macOS, on a pseudo-terminal; on Windows, the view is
only compiled.

## Known limits

- With one neighbour, stations of similar strength are separated down to about 60 Hz apart, and a station 15 dB below
  its neighbour down to about 80 Hz. Closer than that, or well below stations on both sides within about 120 Hz, expect
  lost text. See [docs/close-signals.md](../docs/close-signals.md).
- Deep, slow fading costs characters, as it does for the engine on its own.
- It needs a few seconds of a sender's keying to learn their timing before any text comes out.
- Audio input only: there is no I/Q input yet, so the bandwidth is what the rig's audio carries, up to 3.9 kHz.

## License

GPL-3.0-or-later, as the engine is. See [LICENSE](../LICENSE). miniaudio is public domain or MIT-0
(`third_party/miniaudio/LICENSE`).
