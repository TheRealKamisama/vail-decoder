/* SPDX-License-Identifier: GPL-3.0-or-later */
/* vail-cli — decode every CW signal a rig's sound card can hear, as lines of text.
 *
 * stdout carries finished lines and nothing else. On a terminal, a sound device's signals are also shown
 * below them while they are being heard, in a live area that is cleared before each line is printed. */
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "audio.h"
#include "band.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#define VERSION      "0.1.0"
#define FRAME_SECONDS (VAIL_HOP_SAMPLES / (double)VAIL_SAMPLE_RATE)
#define DRAW_FRAMES  19                      /* about 5 times a second */
#define LEVEL_FRAMES 47                      /* the input level is measured over about half a second */
#define MAX_ROWS     64
#define MAX_COLS     240

static const char USAGE[] =
"usage: vail-cli [options] [file]\n"
"\n"
"Decodes every CW signal in the audio passband, from the system's default sound input,\n"
"or from a sound file (WAV, FLAC, MP3). A file of - is raw 16-bit 8 kHz mono from stdin.\n"
"\n"
"  -l, --list         list the sound input devices and exit\n"
"  -d, --device DEV   use this device: its number in the list, or part of its name\n"
"  -b, --band LO-HI   only decode signals between LO and HI Hz (default: %d-%d)\n"
"  -r, --record FILE  also record the audio to a WAV file (8 kHz, for replaying later)\n"
"  -h, --help         show this help and exit\n"
"  -V, --version      show the version and exit\n"
"\n"
"Each line of output is: time (UTC, or from the start of a file), pitch in Hz, text.\n";

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int s) { (void)s; g_stop = 1; }

static void sleep_ms(int ms)
{
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec t;
    t.tv_sec = 0;
    t.tv_nsec = ms * 1000000L;
    nanosleep(&t, NULL);
#endif
}

static int stdout_is_terminal(void)
{
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode;
    if (!_isatty(_fileno(stdout)) || !GetConsoleMode(h, &mode)) return 0;
    return SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;   /* else no live area */
#else
    return isatty(1);
#endif
}

static void terminal_size(int *rows, int *cols)
{
    *rows = 24;
    *cols = 80;
#ifdef _WIN32
    {
        CONSOLE_SCREEN_BUFFER_INFO info;
        if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) {
            *rows = info.srWindow.Bottom - info.srWindow.Top + 1;
            *cols = info.srWindow.Right - info.srWindow.Left + 1;
        }
    }
#else
    {
        struct winsize w;
        if (ioctl(1, TIOCGWINSZ, &w) == 0 && w.ws_row > 0 && w.ws_col > 0) {
            *rows = w.ws_row;
            *cols = w.ws_col;
        }
    }
#endif
}

/* --------------------------------------------------------------------------------- output */

typedef struct {
    band *band;
    int live;                                /* sound device: wall-clock times */
    int showArea;                            /* terminal: signals are shown as they are heard */
    int areaRows;
    char area[(MAX_ROWS + 1) * (MAX_COLS + 16)], lastArea[(MAX_ROWS + 1) * (MAX_COLS + 16)];
    double levelSum;
    int levelCount, clipped;
    double levelDb;                          /* -inf when the input is silent */
    int clipping;
} output;

static void clear_area(output *o)
{
    if (o->areaRows > 0) {
        printf("\033[%dA\r\033[J", o->areaRows);
        o->areaRows = 0;
    }
    o->lastArea[0] = 0;
}

static void put_line(void *ctx, long frame, int hz, const char *text)
{
    output *o = ctx;
    time_t when;
    int secs;
    if (o->showArea) clear_area(o);
    if (o->live) {
        const struct tm *tm;
        when = time(NULL) - (time_t)((band_frame(o->band) - frame) * FRAME_SECONDS);
        tm = gmtime(&when);
        secs = tm ? tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec : 0;
    } else {
        secs = (int)(frame * FRAME_SECONDS);
    }
    printf("%02d:%02d:%02d %5d  %s\n", secs / 3600, secs / 60 % 60, secs % 60, hz, text);
    if (fflush(stdout) == EOF) g_stop = 1;   /* nobody is reading any more */
}

static void measure_level(output *o, const int16_t *s)
{
    int i;
    for (i = 0; i < VAIL_HOP_SAMPLES; i++) {
        o->levelSum += (double)s[i] * s[i];
        o->clipped |= s[i] >= 32767 || s[i] <= -32768;
    }
    if (++o->levelCount == LEVEL_FRAMES) {
        double power = o->levelSum / (LEVEL_FRAMES * VAIL_HOP_SAMPLES) / (32768.0 * 32768.0);
        o->levelDb = power > 0 ? 10 * log10(power) : -HUGE_VAL;
        o->clipping = o->clipped;
        o->levelSum = 0;
        o->levelCount = o->clipped = 0;
    }
}

/* Appends one row, cut to the width of the terminal, keeping the end of the text (the newest part). */
static char *put_row(char *p, int cols, int hz, const char *text)
{
    int room = cols - 17, len = (int)strlen(text);
    if (room < 8) room = 8;
    if (room > MAX_COLS) room = MAX_COLS;
    if (len > room) { text += len - room; len = room; }
    return p + sprintf(p, "\033[2m         %5d  %.*s\033[0m\n", hz, len, text);
}

static void draw_area(output *o)
{
    band_signal sig[MAX_ROWS];
    char *p = o->area;
    int rows, cols, n, i;
    terminal_size(&rows, &cols);
    rows -= 2;                               /* the status row, and the cursor's */
    if (rows > MAX_ROWS) rows = MAX_ROWS;
    n = band_signals(o->band, sig, rows > 0 ? rows : 0);
    for (i = 0; i < n; i++) p = put_row(p, cols, sig[i].hz, sig[i].text);
    if (o->clipping)
        p += sprintf(p, "\033[2minput is clipping: turn the audio level down\033[0m\n");
    else if (isinf(o->levelDb))
        p += sprintf(p, "\033[2minput is silent\033[0m\n");
    else
        p += sprintf(p, "\033[2minput %.0f dBFS\033[0m\n", o->levelDb);
    if (!strcmp(o->area, o->lastArea)) return;
    clear_area(o);
    fputs(o->area, stdout);
    o->areaRows = n + 1;
    strcpy(o->lastArea, o->area);
    if (fflush(stdout) == EOF) g_stop = 1;
}

/* ------------------------------------------------------------------------------- arguments */

static int usage_error(const char *fmt, const char *arg)
{
    fprintf(stderr, "vail-cli: ");
    fprintf(stderr, fmt, arg);
    fprintf(stderr, "\nTry vail-cli --help.\n");
    return 2;
}

static int parse_band(const char *s, int *lo, int *hi)
{
    char end;
    if (sscanf(s, "%d-%d%c", lo, hi, &end) != 2) return 0;
    return *lo >= 0 && *lo < *hi && *lo <= BAND_MAX_HZ && *hi >= BAND_MIN_HZ;
}

/* The value of the option at argv[*i], or NULL if it has none. */
static const char *option_value(int argc, char **argv, int *i)
{
    return *i + 1 < argc ? argv[++*i] : NULL;
}

int main(int argc, char **argv)
{
    static output o;
    const char *device = NULL, *path = NULL, *recordPath = NULL;
    int lo = BAND_MIN_HZ, hi = BAND_MAX_HZ, i, r, status = 0, list = 0, optionsDone = 0;
    audio *a;
    audio_rec *rec = NULL;
    band *b;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (optionsDone || arg[0] != '-' || !strcmp(arg, "-")) {
            if (path) return usage_error("only one file at a time: %s", arg);
            path = arg;
        } else if (!strcmp(arg, "--")) {
            optionsDone = 1;
        } else if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
            printf(USAGE, BAND_MIN_HZ, BAND_MAX_HZ);
            return 0;
        } else if (!strcmp(arg, "-V") || !strcmp(arg, "--version")) {
            printf("vail-cli %s\n", VERSION);
            return 0;
        } else if (!strcmp(arg, "-l") || !strcmp(arg, "--list")) {
            list = 1;
        } else if (!strcmp(arg, "-d") || !strcmp(arg, "--device")) {
            if (!(device = option_value(argc, argv, &i))) return usage_error("%s needs a device", arg);
        } else if (!strcmp(arg, "-r") || !strcmp(arg, "--record")) {
            if (!(recordPath = option_value(argc, argv, &i))) return usage_error("%s needs a file", arg);
        } else if (!strcmp(arg, "-b") || !strcmp(arg, "--band")) {
            const char *v = option_value(argc, argv, &i);
            if (!v) return usage_error("%s needs LO-HI", arg);
            if (!parse_band(v, &lo, &hi)) return usage_error("not a band of audio frequencies: %s", v);
        } else {
            return usage_error("unknown option: %s", arg);
        }
    }
    if (list) return audio_list();
    if (path && device) return usage_error("%s", "a file and a device cannot both be read");

    a = !path ? audio_open_device(device) : strcmp(path, "-") ? audio_open_file(path) : audio_open_stdin();
    if (!a) return 1;
    if (recordPath && !(rec = audio_rec_open(recordPath))) { audio_close(a); return 1; }
    b = band_new(lo, hi, put_line, &o);
    if (!b) { audio_rec_close(rec); audio_close(a); return 1; }

    o.band = b;
    o.live = audio_is_live(a);
    o.showArea = o.live && stdout_is_terminal();
    o.levelDb = -HUGE_VAL;
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);                /* a closed pipe is seen as a failed write */
#endif
    if (o.live) fprintf(stderr, "vail-cli: listening on %s, %d-%d Hz; Ctrl+C to stop\n", audio_name(a), lo, hi);

    while (!g_stop) {
        int16_t samples[VAIL_HOP_SAMPLES];
        r = audio_read(a, samples, VAIL_HOP_SAMPLES);
        if (r == AUDIO_END) break;
        if (r == AUDIO_WAIT) { sleep_ms(5); continue; }
        if (rec) audio_rec_write(rec, samples, VAIL_HOP_SAMPLES);
        if (o.showArea) measure_level(&o, samples);
        band_feed(b, samples);
        if (o.showArea && band_frame(b) % DRAW_FRAMES == 0) draw_area(&o);
    }

    if (audio_failed(a)) {
        if (o.showArea) clear_area(&o);
        fflush(stdout);
        fprintf(stderr, "vail-cli: the sound device stopped\n");
        status = 1;
    }
    band_finish(b);
    if (o.showArea) { clear_area(&o); fflush(stdout); }
    if (audio_lost_samples(a) > 0)
        fprintf(stderr, "vail-cli: decoding fell behind: lost %.1f s of audio\n",
                audio_lost_samples(a) / (double)VAIL_SAMPLE_RATE);

    band_free(b);
    audio_rec_close(rec);
    audio_close(a);
    return status;
}
