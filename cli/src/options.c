/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "band.h"
#include "options.h"
#include "report.h"
#include "view.h"

typedef struct {
    char shortName;
    const char *longName;
    const char *value;                       /* what it takes, for messages; NULL if nothing */
} option_spec;

static const option_spec OPTIONS[] = {
    { 'l', "list",     NULL },
    { 'd', "device",   "a device" },
    { 'c', "channel",  "a channel number" },
    { 'b', "band",     "LO-HI" },
    { 's', "seconds",  "a number of seconds" },
    { 'r', "record",   "a file" },
    { 'R', "rate",     "a sample rate" },
    { 'F', "format",   "s16 or f32" },
    { 'n', "channels", "a number of channels" },
    { 'T', "view",     NULL },
    { 'p', "plain",    NULL },
    { 'j', "json",     NULL },
    { 'a', "all",      NULL },
    { 'h', "help",     NULL },
    { 'V', "version",  NULL },
};
#define N_OPTIONS ((int)(sizeof OPTIONS / sizeof OPTIONS[0]))

void options_print_help(void)
{
    printf(
"usage: vail-cli [options] [FILE]\n"
"\n"
"Decodes every CW (Morse) signal in the audio passband at once. It listens to the default sound\n"
"input, or reads FILE: a WAV, FLAC or MP3 recording, or - for raw samples on stdin.\n"
"\n"
"Input:\n"
"  -l, --list            list the sound input devices and exit\n"
"  -d, --device DEV      use this device: its number in the list, or part of its name\n"
"  -c, --channel N       decode only this channel of the input (1 left, 2 right; default: mix them)\n"
"  -b, --band LO-HI      only decode signals between LO and HI Hz (default: %d-%d)\n"
"  -s, --seconds N       stop after N seconds of audio\n"
"  -r, --record FILE     also record the audio to a WAV file (8 kHz, for replaying later)\n"
"\n"
"Raw samples on stdin, always little-endian:\n"
"  -R, --rate HZ         samples per second (default: 8000)\n"
"  -F, --format FMT      s16 or f32 (default: s16)\n"
"  -n, --channels N      interleaved channels (default: 1)\n"
"\n"
"Output:\n"
"  -T, --view            full-screen view, on the terminal whatever stdout is%s\n"
"  -p, --plain           finished lines of text only, as on a pipe\n"
"  -j, --json            finished lines as JSON Lines, with SNR and speed\n"
"  -a, --all             also show text that does not read like Morse, such as decoded speech\n"
"\n"
"  -h, --help            show this help and exit\n"
"  -V, --version         show the version and exit\n"
"\n"
"Each line of text is: time (UTC, or from the start of a file or stream), pitch in Hz, text.\n"
"On a terminal, a sound device or a stream opens the full-screen view unless --plain is given.\n"
"\n"
"Examples:\n"
"  vail-cli -d \"USB Audio\"                           listen to a rig\n"
"  vail-cli -T >> band.log                           watch it and keep a log\n"
"  vail-cli | grep --line-buffered K1ABC             watch for a callsign\n"
"  vail-cli recording.wav                            decode a recording\n"
"  arecord -t raw -f S16_LE -r 48000 | vail-cli -R 48000 -\n",
        BAND_MIN_HZ, BAND_MAX_HZ, view_available() ? "" : " (not in this build)");
}

static int usage_error(const char *fmt, const char *arg)
{
    char message[512];
    snprintf(message, sizeof message, fmt, arg);
    report("%s", message);
    fprintf(stderr, "Try vail-cli --help.\n");
    return 0;
}

/* The edit distance of two short strings; a large number if they are too long to be worth comparing. */
static int distance(const char *a, const char *b)
{
    enum { MAX = 32 };
    int prev[MAX + 1], cur[MAX + 1], i, j, la = (int)strlen(a), lb = (int)strlen(b);
    if (la > MAX || lb > MAX) return MAX;
    for (j = 0; j <= lb; j++) prev[j] = j;
    for (i = 1; i <= la; i++) {
        cur[0] = i;
        for (j = 1; j <= lb; j++) {
            int best = prev[j - 1] + (a[i - 1] != b[j - 1]);
            if (prev[j] + 1 < best) best = prev[j] + 1;
            if (cur[j - 1] + 1 < best) best = cur[j - 1] + 1;
            cur[j] = best;
        }
        memcpy(prev, cur, sizeof(int) * (size_t)(lb + 1));
    }
    return prev[lb];
}

static int unknown_long_option(const char *name, size_t len)
{
    char given[64];
    int i, best = 3, match = -1;
    snprintf(given, sizeof given, "%.*s", (int)len, name);
    for (i = 0; i < N_OPTIONS; i++) {
        int d = distance(given, OPTIONS[i].longName);
        if (d < best) { best = d; match = i; }
    }
    if (match >= 0) {
        report("unknown option: --%s. Did you mean --%s?", given, OPTIONS[match].longName);
        fprintf(stderr, "Try vail-cli --help.\n");
        return 0;
    }
    return usage_error("unknown option: --%s", given);
}

static int parse_int(const char *s, int lo, int hi, int *out)
{
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s || *end || v < lo || v > hi) return 0;
    *out = (int)v;
    return 1;
}

static int parse_band(const char *s, int *lo, int *hi)
{
    char end;
    if (sscanf(s, "%d-%d%c", lo, hi, &end) != 2) return 0;
    return *lo >= 0 && *lo < *hi && *lo <= BAND_MAX_HZ && *hi >= BAND_MIN_HZ;
}

static int apply(options *o, const option_spec *spec, const char *v)
{
    char *end;
    switch (spec->shortName) {
    case 'l': o->list = 1; break;
    case 'T': o->view = 1; break;
    case 'p': o->plain = 1; break;
    case 'j': o->json = 1; break;
    case 'a': o->all = 1; break;
    case 'h': o->help = 1; break;
    case 'V': o->version = 1; break;
    case 'd': o->device = v; break;
    case 'r': o->recordPath = v; break;
    case 'b':
        if (!parse_band(v, &o->lo, &o->hi)) return usage_error("not a band of audio frequencies: %s", v);
        break;
    case 'c':
        if (!parse_int(v, 1, 32, &o->channel)) return usage_error("not a channel, which counts from 1: %s", v);
        break;
    case 's':
        o->seconds = strtod(v, &end);
        if (end == v || *end || !(o->seconds > 0 && o->seconds < 1e9)) return usage_error("not a number of seconds: %s", v);
        break;
    case 'R':
        if (!parse_int(v, 4000, 768000, &o->raw.rate)) return usage_error("not a sample rate from 4000 to 768000: %s", v);
        break;
    case 'n':
        if (!parse_int(v, 1, 32, &o->raw.channels)) return usage_error("not a number of channels from 1 to 32: %s", v);
        break;
    case 'F':
        if (!strcmp(v, "s16")) o->raw.format = AUDIO_S16;
        else if (!strcmp(v, "f32")) o->raw.format = AUDIO_F32;
        else return usage_error("not a sample format, which is s16 or f32: %s", v);
        break;
    default: break;
    }
    return 1;
}

static const option_spec *find_short(char c)
{
    int i;
    for (i = 0; i < N_OPTIONS; i++) if (OPTIONS[i].shortName == c) return &OPTIONS[i];
    return NULL;
}

static const option_spec *find_long(const char *name, size_t len)
{
    int i;
    for (i = 0; i < N_OPTIONS; i++)
        if (strlen(OPTIONS[i].longName) == len && !strncmp(OPTIONS[i].longName, name, len)) return &OPTIONS[i];
    return NULL;
}

static int missing_value(const char *written, const char *what)
{
    char message[128];
    snprintf(message, sizeof message, "%s needs %s", written, what);
    return usage_error("%s", message);
}

/* --name, or --name=value, or --name value. */
static int parse_long(options *o, int argc, char **argv, int *i)
{
    const char *name = argv[*i] + 2, *eq = strchr(name, '=');
    size_t len = eq ? (size_t)(eq - name) : strlen(name);
    const option_spec *spec = find_long(name, len);
    const char *value = eq ? eq + 1 : NULL;
    if (!spec) return unknown_long_option(name, len);
    if (!spec->value) {
        if (eq) return usage_error("--%s takes no value", spec->longName);
        return apply(o, spec, NULL);
    }
    if (!value) {
        if (*i + 1 >= argc) return missing_value(argv[*i], spec->value);
        value = argv[++*i];
    }
    return apply(o, spec, value);
}

/* -x, several at once as -Tj, or -x value, or -xvalue. */
static int parse_short(options *o, int argc, char **argv, int *i)
{
    const char *p = argv[*i] + 1;
    for (; *p; p++) {
        const option_spec *spec = find_short(*p);
        char written[3] = { '-', *p, 0 };
        if (!spec) return usage_error("unknown option: %s", written);
        if (!spec->value) {
            if (!apply(o, spec, NULL)) return 0;
            continue;
        }
        if (p[1]) return apply(o, spec, p + 1);
        if (*i + 1 >= argc) return missing_value(written, spec->value);
        return apply(o, spec, argv[++*i]);
    }
    return 1;
}

int options_parse(int argc, char **argv, options *o)
{
    int i, optionsDone = 0, rawGiven = 0;
    memset(o, 0, sizeof *o);
    o->lo = BAND_MIN_HZ;
    o->hi = BAND_MAX_HZ;
    o->raw.rate = VAIL_SAMPLE_RATE;
    o->raw.format = AUDIO_S16;
    o->raw.channels = 1;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];
        int ok;
        if (optionsDone || arg[0] != '-' || !strcmp(arg, "-")) {
            if (o->path) return usage_error("only one file at a time: %s", arg);
            o->path = arg;
            continue;
        }
        if (!strcmp(arg, "--")) {
            optionsDone = 1;
            continue;
        }
        rawGiven |= !strncmp(arg, "--rate", 6) || !strncmp(arg, "--format", 8) || !strncmp(arg, "--channels", 10)
                    || (arg[1] != '-' && strpbrk(arg, "RFn") == arg + 1);
        ok = arg[1] == '-' ? parse_long(o, argc, argv, &i) : parse_short(o, argc, argv, &i);
        if (!ok) return 0;
    }
    if (o->help || o->version || o->list) return 1;

    if (o->path && o->device) return usage_error("%s", "a file and a device cannot both be read");
    if (rawGiven && !(o->path && !strcmp(o->path, "-")))
        return usage_error("%s", "--rate, --format and --channels describe raw samples, so they need a FILE of -");
    if (o->path && !strcmp(o->path, "-") && o->channel > o->raw.channels)
        return usage_error("%s", "--channel is more than --channels");
    if (o->view && o->plain) return usage_error("%s", "--view and --plain cannot both be used");
    return 1;
}
