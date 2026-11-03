# vail-cli

Decode every CW (Morse) signal your rig's sound card can hear, as plain lines of text. Think of it as
a command-line CW Skimmer, built on the [vail engine](../README.md) in this repository. It runs on Linux, macOS
and Windows and builds into one self-contained binary.

```console
$ vail-cli -d "USB Audio"
vail-cli: listening on USB Audio CODEC, 200-3900 Hz; Ctrl+C to stop
15:33:16   500  CQ CQ CQ DE K1ABC K1ABC K1ABC PSE K
15:33:17  1398  TEST W1AW W1AW TEST W1AW W1AW TEST
15:33:16  3297  CQ DX CQ DX DE JA1ZZZ JA1ZZZ UP 5 K
          2297  VVV VVV DE DL2X
input -15 dBFS
```

Each line is the time the text started (UTC), the pitch in Hz, and the text. On a terminal, signals still
being heard are shown dimmed below the finished lines, with the input level. That part is never written to a
pipe or file, so stdout is always clean, finished lines:

```sh
vail-cli | grep --line-buffered K1ABC        # watch for a callsign
vail-cli >> band.log                         # keep a log
vail-cli recording.wav                       # decode a recording (WAV, FLAC, MP3)
rtl_fm -M usb -f 7.03M -s 8k | vail-cli -    # raw 16-bit 8 kHz mono from any program
```

## Use

```
usage: vail-cli [options] [file]

  -l, --list         list the sound input devices and exit
  -d, --device DEV   use this device: its number in the list, or part of its name
  -b, --band LO-HI   only decode signals between LO and HI Hz (default: 200-3900)
  -r, --record FILE  also record the audio to a WAV file (8 kHz, for replaying later)
  -h, --help         show this help and exit
  -V, --version      show the version and exit
```

There is nothing to configure. With no options it listens to the system's default input. Set the rig to a
wide filter to see the whole passband, and keep the input level out of clipping: vail-cli tells you if it
isn't. Both channels of a stereo input are mixed, so dual-receiver rigs decode both receivers.

Text can be revised as the decoder learns more, so a line is printed only once it is final: when a signal has
been quiet for 4 seconds, or as soon as 60 characters are beyond the decoder's reach. Lower-case letters are
prosigns (`ar`, `sk`, `bk`, ...).

Exit status is 0 on success, on Ctrl+C, or when whatever was reading the output goes away, as in `vail-cli | head`. It is
1 on a runtime error such as a missing device, a device that stops, or output that cannot be written, and 2 on a usage
error. If a recording cannot be written, that is reported at once, decoding goes on without it, and the status is 1.

## Install

Binaries for Linux (x86_64), macOS (Apple silicon and Intel) and Windows (x86_64) are on the
[releases page](https://github.com/TheRealKamisama/vail-decoder/releases). Each archive holds the program, its man page,
this README and the license. The macOS binaries are not signed, so one downloaded with a browser first needs
`xattr -d com.apple.quarantine vail-cli`.

To build it, only a C compiler is needed; audio libraries are found at run time.

```sh
git clone https://github.com/TheRealKamisama/vail-decoder
cd vail-decoder/cli
make
sudo make install        # to /usr/local; or use PREFIX=...
```

- **Linux** needs PulseAudio, PipeWire, JACK or ALSA at run time, which any desktop already has.
- **macOS** needs the Xcode command line tools to build. The first run asks for microphone permission
  for your terminal; without it you get silence, which vail-cli reports as `input is silent`.
- **Windows** builds with MSYS2/MinGW (`pacman -S make mingw-w64-x86_64-gcc`, then `make`), or cross-compile
  from anywhere: `make CC="zig cc -target x86_64-windows-gnu" TARGET=Windows`.

## How it works

```
sound card ─ miniaudio ─ 8 kHz mono ─ FFT ─┬─ slice 1 ─ vail engine ─┐
  or file                     0–4 kHz      ├─ slice 2 ─ vail engine ─┤
                                           ├─   ...                  ├─ one station each ─ final text ─ stdout
                                           └─ slice 5 ─ vail engine ─┘
```

That is all of it: `src/audio.c` gets samples, `src/band.c` turns them into lines, `src/main.c` is the
command line and the terminal.

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
make test                  # known stations in, text and ghosts scored
python3 tests/e2e.py --table   # the same, showing every station's text and every ghost
```

Test audio is synthesised with the engine's own generator, covering noise, stations from 250 to 3400 Hz, up to
ten at once, the slice overlaps, a receiver's filter, clipping, long messages, and file, device and write errors.

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
