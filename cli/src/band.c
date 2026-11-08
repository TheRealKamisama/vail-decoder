/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "band.h"
#include "text.h"
#include "vail_internal.h"   /* struct vail_fe: the whole spectrum of the FFT the engine's front end ran */

/* An engine looks for new signals in bins 10..117 of its 128-bin slice. Slices start every 92 FFT bins,
 * so their search ranges overlap by 16 bins (125 Hz) and together cover FFT bins 26..501: 203-3914 Hz. */
#define ENGINES     5
#define SLICE_FIRST 16
#define SLICE_STEP  92
#define SEARCH_LO   10
#define SEARCH_HI   117
#define HZ_PER_BIN  7.8125
#define FFT_BINS    (FE_FFT_SIZE / 2)

/* vail_channel_text reports a slice bin as 200 + bin * 7.8125 Hz. */
#define PITCH_TO_BIN(hz) ((int)lrint(((hz) - 200) / HZ_PER_BIN))

/* Whether a channel is a station of its own is judged from the last HISTORY_FRAMES (5.4 s) of spectrum. */
#define HISTORY_FRAMES 512
#define CHECK_FRAMES   32                   /* how often a channel not yet judged is looked at */
#define CHECKS_TO_PASS 2

/* The engine re-decodes only the last 26 characters of a channel, so anything older is final. */
#define REVISABLE_CHARS  WRD_WINDOW_MAX

#define LINE_CHARS   60                     /* characters on a line, spaces not counted */
#define QUIET_FRAMES 376                    /* 4 s without new text ends a line */
#define DRAIN_FRAMES 377                    /* 4 s of silence lets the engines finish */
#define TEXT_CAP     (3 * MSG_CAPACITY + 8) /* a character renders to at most 3 bytes */
#define TWIN_BINS    2                      /* 16 Hz: in an overlap of slices, two engines hear one signal this close */

/* A line is shown when the engine's confidence that its keying is Morse (a log-odds) is at least this, either as the line
 * prints or on average while its text came in (line_score). */
#define MIN_SCORE    1.0f

/* The engine forgets a channel's text when the channel retires, so each one is followed here. */
typedef struct {
    int id, engine, bin;                    /* bin: in the FFT, so comparable between engines */
    int snr, wpm, state;
    float score;                            /* the engine's confidence that this is Morse, a log-odds */
    float scoreSum;                         /* the score summed over the frames since the unprinted text began, */
    int scoreFrames;                        /* ... and those frames */
    int hidden;                             /* not shown: outside the band, or not a station */
    int passed;                             /* checks passed; at CHECKS_TO_PASS it is a station */
    int seen;
    int len;
    int chars;                              /* characters in the text, spaces not counted */
    int printed;                            /* characters already printed, spaces not counted */
    long dropped;                           /* the engine's count when the text was last taken */
    long changed;                         /* frame of the last change to the text */
    long started;                           /* frame when the unprinted text began */
    char text[TEXT_CAP];
    unsigned char types[TEXT_CAP];          /* the word class of each byte of text (VAIL_WORD_*) */
} track;

struct band {
    vail_fe *fe;
    vail_api *engine[ENGINES];
    int loBin, hiBin;
    long frame;
    uint32_t rng;
    float (*history)[FFT_BINS];             /* power spectra, a ring indexed by frame */
    track **tracks;
    int nTracks, capTracks;
    struct { int id, bin; } *channels;      /* every live channel of every engine, text or not */
    int nChannels, capChannels;
    int showAll;
    long hiddenLines;
    band_line_fn *onLine;
    void *ctx;
    vail_char chars[TEXT_CAP];              /* a channel's text with its word classes, as it is taken */
};

static int slice_start(int e) { return SLICE_FIRST + e * SLICE_STEP; }
static int bin_hz(int bin) { return (int)lrint(bin * HZ_PER_BIN); }

band *band_new(int loHz, int hiHz, int showAll, band_line_fn *onLine, void *ctx)
{
    band *b = calloc(1, sizeof *b);
    int e;
    if (!b) return NULL;
    b->rng = 2463534242u;
    b->fe = vail_fe_new();
    b->history = calloc(HISTORY_FRAMES, sizeof *b->history);
    if (!b->fe || !b->history) abort();
    b->loBin = (int)floor(loHz / HZ_PER_BIN);
    b->hiBin = (int)ceil(hiHz / HZ_PER_BIN);
    b->showAll = showAll;
    b->onLine = onLine;
    b->ctx = ctx;
    for (e = 0; e < ENGINES; e++) {
        int lo = slice_start(e) + SEARCH_LO, hi = slice_start(e) + SEARCH_HI;
        if (hi >= b->loBin && lo <= b->hiBin) b->engine[e] = API_New();   /* else nothing to find there */
    }
    return b;
}

static int is_station(const track *t) { return !t->hidden && t->passed >= CHECKS_TO_PASS; }

/* Offset in s of its nth character, spaces not counted; its length if it has fewer. */
static int offset_of_char(const char *s, int len, int nth)
{
    int i;
    for (i = 0; i < len; i++) {
        if (s[i] == ' ') continue;
        if (nth-- == 0) break;
    }
    return i;
}

static int count_chars(const char *s, int len)
{
    int i, n = 0;
    for (i = 0; i < len; i++) n += s[i] != ' ';
    return n;
}

/* Whether an older channel of another engine is on the same signal, as in an overlap of slices. It is not final: when the
 * older one retires, as a drifting signal leaves its slice, this one carries on. */
static int has_older_twin(const band *b, const track *t)
{
    int i;
    for (i = 0; i < b->nTracks; i++) {
        const track *o = b->tracks[i];
        if (o->id < t->id && o->engine != t->engine && o->seen && is_station(o) && abs(o->bin - t->bin) <= TWIN_BINS)
            return 1;
    }
    return 0;
}

/* How far the unprinted text's keying is Morse: the score now, or its mean while that text came in, whichever is higher.
 * The mean keeps a line whose station was followed by other keying before the line printed; the score now keeps a
 * short message, during which the score is still rising. */
static float line_score(const track *t)
{
    float mean = t->scoreFrames ? t->scoreSum / t->scoreFrames : t->score;
    return mean > t->score ? mean : t->score;
}

/* Prints up to LINE_CHARS characters of the unprinted text, ending at the end of a word if it can. */
static void print_line(band *b, track *t)
{
    char line[TEXT_TIDY_SIZE(TEXT_CAP)];
    unsigned char lineTypes[TEXT_TIDY_SIZE(TEXT_CAP)];
    int from = offset_of_char(t->text, t->len, t->printed), to = from, n = 0, end;
    while (to < t->len && n < LINE_CHARS) n += t->text[to++] != ' ';
    if (to < t->len && t->text[to] != ' ') {
        int word = to;
        while (word > from && t->text[word - 1] != ' ') word--;
        if (word > from) to = word;
    }
    for (end = to; end > from && t->text[end - 1] == ' '; end--) {}
    if (is_station(t) && end > from && !has_older_twin(b, t)) {
        if (b->showAll || line_score(t) >= MIN_SCORE) {
            band_line out;
            text_tidy(t->text + from, t->types + from, end - from, line, lineTypes);
            out.frame = t->started;
            out.channel = t->id;
            out.hz = bin_hz(t->bin);
            out.snr = t->snr;
            out.wpm = t->wpm;
            out.score = line_score(t);
            out.text = line;
            out.types = lineTypes;
            b->onLine(b->ctx, &out);
        } else {
            b->hiddenLines++;
        }
    }
    t->printed += count_chars(t->text + from, to - from);
    t->started = b->frame;
    t->scoreSum = 0.0f;
    t->scoreFrames = 0;
}

static void print_all(band *b, track *t)
{
    while (t->printed < count_chars(t->text, t->len)) print_line(b, t);
}

/* dropped: the characters the engine has shifted out of the front of this channel's text so far. chars: n records of the
 * text with their word classes, which can change while the text does not. */
static void track_update(band *b, track *t, const char *text, int len, const vail_char *chars, int n, long dropped)
{
    int gone = (int)(dropped - t->dropped), i;
    for (i = 0; i < len; i++) t->types[i] = (unsigned char)(i < n ? chars[i].wordType : VAIL_WORD_GENERIC);
    if (!gone && len == t->len && !memcmp(text, t->text, (size_t)len)) return;
    t->dropped = dropped;
    t->printed -= gone;
    if (t->printed < 0) t->printed = 0;
    if (t->printed >= count_chars(t->text, t->len) - gone) t->started = b->frame;   /* new text */
    memcpy(t->text, text, (size_t)len + 1);
    t->len = len;
    t->chars = count_chars(text, len);
    t->changed = b->frame;
}

static track *find_track(const band *b, int id)
{
    int i;
    for (i = 0; i < b->nTracks; i++) if (b->tracks[i]->id == id) return b->tracks[i];
    return NULL;
}

static track *new_track(band *b, int id, int engine, int bin)
{
    track *t = calloc(1, sizeof *t);
    if (!t) abort();
    t->id = id;
    t->engine = engine;
    t->bin = bin;
    t->hidden = bin < b->loBin || bin > b->hiBin;
    t->started = t->changed = b->frame;
    if (b->nTracks == b->capTracks) {
        b->capTracks = b->capTracks ? 2 * b->capTracks : 16;
        b->tracks = realloc(b->tracks, (size_t)b->capTracks * sizeof *b->tracks);
        if (!b->tracks) abort();
    }
    b->tracks[b->nTracks++] = t;
    return t;
}

static void poll_channels(band *b)
{
    char text[TEXT_CAP];
    int e, i;
    for (i = 0; i < b->nTracks; i++) b->tracks[i]->seen = 0;
    b->nChannels = 0;
    for (e = 0; e < ENGINES; e++) {
        int n;
        if (!b->engine[e]) continue;
        n = vail_channel_count_all(b->engine[e]);
        for (i = 0; i < n; i++) {
            int hz, id, len = vail_channel_text(b->engine[e], i, text, sizeof text, &hz, &id);
            int bin = slice_start(e) + PITCH_TO_BIN(hz);
            track *t;
            if (b->nChannels == b->capChannels) {
                b->capChannels = b->capChannels ? 2 * b->capChannels : 32;
                b->channels = realloc(b->channels, (size_t)b->capChannels * sizeof *b->channels);
                if (!b->channels) abort();
            }
            b->channels[b->nChannels].id = id;
            b->channels[b->nChannels].bin = bin;
            b->nChannels++;
            if (len <= 0) continue;                    /* still learning the sender's timing */
            t = find_track(b, id);
            if (!t) {
                t = new_track(b, id, e, bin);
                t->dropped = vail_channel_dropped(b->engine[e], i);
            }
            t->bin = bin;
            t->seen = 1;
            vail_channel_stats(b->engine[e], i, &t->snr, &t->wpm, &t->state);
            vail_channel_confidence(b->engine[e], i, &t->score);
            track_update(b, t, text, len, b->chars, vail_channel_chars(b->engine[e], i, b->chars, TEXT_CAP),
                         vail_channel_dropped(b->engine[e], i));
            if (t->printed < t->chars) {
                t->scoreSum += t->score;
                t->scoreFrames++;
            }
        }
    }
}

/* The power around a bin over the history, oldest first, and its peak. */
static float levels(const band *b, int bin, float *out)
{
    float peak = 0;
    int t, k;
    if (bin < 1) bin = 1;
    if (bin > FFT_BINS - 2) bin = FFT_BINS - 2;
    for (t = 0; t < HISTORY_FRAMES; t++) {
        const float *p = b->history[(b->frame + 1 + t) % HISTORY_FRAMES];
        float v = p[bin - 1];
        for (k = bin; k <= bin + 1; k++) if (p[k] > v) v = p[k];
        out[t] = v;
        if (v > peak) peak = v;
    }
    return peak;
}

static int cmp_float(const void *x, const void *y)
{
    float a = *(const float *)x, c = *(const float *)y;
    return (a > c) - (a < c);
}

/* A station keys whatever other stations are doing. What only carries energy while a stronger channel is
 * on is that channel's sidelobe, key click, harmonic or intermodulation, or the same signal seen by the
 * next engine. Returns -1 for that, 1 for a station, 0 if there is too little to go on. */
static int judge(const band *b, const track *t)
{
    enum { MIN_ON_FRAMES = 12 };
    static float mine[HISTORY_FRAMES], other[HISTORY_FRAMES], sorted[HISTORY_FRAMES];
    float peak = levels(b, t->bin, mine), on;
    int c, k, onFrames = 0;
    memcpy(sorted, mine, sizeof sorted);
    qsort(sorted, HISTORY_FRAMES, sizeof(float), cmp_float);
    on = sorted[HISTORY_FRAMES / 2] * 8;                 /* well clear of this bin's noise */
    if (on < peak / 100) on = peak / 100;
    for (k = 0; k < HISTORY_FRAMES; k++) onFrames += mine[k] > on;
    if (onFrames < MIN_ON_FRAMES) return 0;
    for (c = 0; c < b->nChannels; c++) {
        const track *o;
        float otherPeak;
        int both = 0, otherOn = 0;
        if (b->channels[c].id == t->id) continue;
        o = find_track(b, b->channels[c].id);
        if (o && o->hidden) continue;
        otherPeak = levels(b, b->channels[c].bin, other);
        if (otherPeak < peak || (otherPeak == peak && b->channels[c].id > t->id)) continue;
        for (k = 0; k < HISTORY_FRAMES; k++) {
            int isOn = other[k] > otherPeak / 32;
            otherOn += isOn;
            both += isOn && mine[k] > on;
        }
        if (otherOn < HISTORY_FRAMES * 9 / 10 && both >= onFrames * 9 / 10) return -1;
    }
    return 1;
}

static void settle(band *b)
{
    int i;
    for (i = b->nTracks - 1; i >= 0; i--) {
        track *t = b->tracks[i];
        if (!t->seen) {                                /* the channel retired, with its text */
            print_all(b, t);
            free(t);
            b->tracks[i] = b->tracks[--b->nTracks];
            continue;
        }
        if (!t->hidden && t->passed < CHECKS_TO_PASS && b->frame % CHECK_FRAMES == 0) {
            int verdict = judge(b, t);
            if (verdict < 0) t->hidden = 1;
            else t->passed += verdict;
        }
        while (is_station(t) && count_chars(t->text, t->len) - REVISABLE_CHARS - t->printed >= LINE_CHARS)
            print_line(b, t);
        if (is_station(t) && b->frame - t->changed >= QUIET_FRAMES) print_all(b, t);
    }
}

/* A little noise, far below any receiver's own. Perfectly clean audio has no noise floor at all, and then
 * the engine reads the faintest residues of a signal's keying as more signals. */
static void add_dither(band *b, const int16_t *in, int16_t *out)
{
    int i;
    for (i = 0; i < VAIL_HOP_SAMPLES; i++) {
        int sum = 0, k, v;
        for (k = 0; k < 4; k++) {                      /* xorshift32; the sum of four is near enough Gaussian */
            b->rng ^= b->rng << 13;
            b->rng ^= b->rng >> 17;
            b->rng ^= b->rng << 5;
            sum += (int)(b->rng & 7) - 3;
        }
        v = in[i] + sum;
        out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
}

void band_feed(band *b, const int16_t samples[VAIL_HOP_SAMPLES])
{
    int16_t dithered[VAIL_HOP_SAMPLES];
    float *power = b->history[(b->frame + 1) % HISTORY_FRAMES];
    int e, k;
    add_dither(b, samples, dithered);
    vail_fe_process(b->fe, dithered);
    for (k = SLICE_FIRST; k < FFT_BINS; k++) {
        double re = b->fe->work[2 * k], im = b->fe->work[2 * k + 1];
        double p = (re * re + im * im) * FE_POWER_SCALE;
        power[k] = isfinite(p) ? (float)p : 0.0f;
    }
    b->frame++;
    for (e = 0; e < ENGINES; e++)
        if (b->engine[e]) API_Feed(b->engine[e], power + slice_start(e));
    poll_channels(b);
    settle(b);
}

void band_finish(band *b)
{
    static const int16_t silence[VAIL_HOP_SAMPLES];
    int i;
    for (i = 0; i < DRAIN_FRAMES; i++) band_feed(b, silence);
    for (i = 0; i < b->nTracks; i++) print_all(b, b->tracks[i]);
}

int band_signals(const band *b, band_signal *out, int max, int unprintedOnly)
{
    char tidy[TEXT_TIDY_SIZE(TEXT_CAP)];
    unsigned char tidyTypes[TEXT_TIDY_SIZE(TEXT_CAP)];
    int i, n = 0;
    for (i = 0; i < b->nTracks && n < max; i++) {
        const track *t = b->tracks[i];
        band_signal s;
        int j, from = 0, len, cut;
        if (!is_station(t) || has_older_twin(b, t)) continue;
        if (!b->showAll && line_score(t) < MIN_SCORE) continue;
        if (unprintedOnly) {
            if (t->printed >= count_chars(t->text, t->len)) continue;
            from = offset_of_char(t->text, t->len, t->printed);
        }
        len = text_tidy(t->text + from, t->types + from, t->len - from, tidy, tidyTypes);
        cut = len < BAND_LIVE_TEXT ? 0 : len - (BAND_LIVE_TEXT - 1);   /* the newest text is what is kept */
        s.hz = bin_hz(t->bin);
        s.snr = t->snr;
        s.wpm = t->wpm;
        s.score = line_score(t);
        s.decoding = t->state == API_STATE_DECODING || t->state == API_STATE_LOCKED_DECODING;
        memcpy(s.text, tidy + cut, (size_t)(len - cut) + 1);
        memcpy(s.types, tidyTypes + cut, (size_t)(len - cut));
        for (j = n++; j > 0 && out[j - 1].hz > s.hz; j--) out[j] = out[j - 1];
        out[j] = s;
    }
    return n;
}

long band_hidden_lines(const band *b) { return b->hiddenLines; }
long band_frame(const band *b) { return b->frame; }

void band_free(band *b)
{
    int e, i;
    if (!b) return;
    for (i = 0; i < b->nTracks; i++) free(b->tracks[i]);
    free(b->tracks);
    free(b->channels);
    free(b->history);
    for (e = 0; e < ENGINES; e++) API_Free(b->engine[e]);
    vail_fe_free(b->fe);
    free(b);
}
