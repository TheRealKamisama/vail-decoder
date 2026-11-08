/* SPDX-License-Identifier: GPL-3.0-or-later */
/* vail-cli — decode every CW signal a rig's sound card can hear.
 *
 * stdout carries finished lines and nothing else, unless it is the terminal. There, a full-screen view (view.c, where
 * ncurses was found) or a live area below the lines (output.c) shows what is being heard. */
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "audio.h"
#include "band.h"
#include "options.h"
#include "output.h"
#include "report.h"
#include "view.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#define VERSION       "0.1.0"
#define FRAME_SECONDS (VAIL_HOP_SAMPLES / (double)VAIL_SAMPLE_RATE)
#define KEYS_MS       20                     /* how often keys are looked at */
#define DRAW_MS       200                    /* and the screen redrawn */
#define HOLD_MS       50                     /* when nothing else is going on */
#define LEVEL_FRAMES  47                     /* the input level is measured over about half a second */
#define SILENT_FRAMES 941                    /* 10 s of silence is worth telling someone without a screen */
#define CLIP_REPEAT_FRAMES 5647              /* say so again after a minute of it */

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int s) { (void)s; g_stop = 1; }

static void watch_signals(void)
{
#ifdef _WIN32
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
#else
    struct sigaction action;
    memset(&action, 0, sizeof action);
    sigemptyset(&action.sa_mask);
    action.sa_handler = on_signal;           /* no SA_RESTART: nothing should wait out a signal */
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGHUP, &action, NULL);        /* so that a recording still gets its length when the terminal closes */
    action.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &action, NULL);       /* a closed pipe is seen as a failed write */
#endif
}

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

static long now_ms(void)
{
#ifdef _WIN32
    return (long)GetTickCount64();
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
#endif
}

static int stdin_is_terminal(void)
{
#ifdef _WIN32
    return _isatty(_fileno(stdin));
#else
    return isatty(0);
#endif
}

/* The terminal needs to take escape codes for the live area. */
static int terminal_takes_codes(void)
{
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode;
    if (!GetConsoleMode(h, &mode)) return 0;
    return SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
#else
    const char *term = getenv("TERM");
    return term && *term && strcmp(term, "dumb");
#endif
}

/* ------------------------------------------------------------------------------ the input level */

typedef struct {
    double sum;
    int count, clipped;
    int known;                               /* there has been enough audio to measure */
    double db;                               /* -inf when the input is silent */
    int clipping;
    long silentFrames, clipFrames;
    int saidSilent, saidClipping;
} level;

static void measure_level(level *l, const int16_t *s)
{
    int i;
    for (i = 0; i < VAIL_HOP_SAMPLES; i++) {
        l->sum += (double)s[i] * s[i];
        l->clipped |= s[i] >= 32767 || s[i] <= -32768;
    }
    if (++l->count == LEVEL_FRAMES) {
        double power = l->sum / (LEVEL_FRAMES * VAIL_HOP_SAMPLES) / (32768.0 * 32768.0);
        l->db = power > 0 ? 10 * log10(power) : -HUGE_VAL;
        l->known = 1;
        l->clipping = l->clipped;
        l->sum = 0;
        l->count = l->clipped = 0;
    }
}

static void level_status(const level *l, char *out, size_t size)
{
    if (!l->known) snprintf(out, size, "waiting for audio");
    else if (l->clipping) snprintf(out, size, "input is clipping: turn the audio level down");
    else if (isinf(l->db)) snprintf(out, size, "input is silent");
    else snprintf(out, size, "input %.0f dBFS", l->db);
}

/* With nothing on the screen to show the level, the two things it would warn of are said once, when they happen. */
static void level_warn(level *l)
{
    if (!l->known) return;
    if (isinf(l->db)) {
        l->silentFrames++;
        if (l->silentFrames >= SILENT_FRAMES && !l->saidSilent) {
            report("input is silent: check the device, its level and whether this program may use it");
            l->saidSilent = 1;
        }
    } else {
        l->silentFrames = 0;
        l->saidSilent = 0;
    }
    if (l->clipFrames > 0) l->clipFrames--;
    if (l->clipping && l->clipFrames == 0) {
        report("input is clipping: turn the audio level down");
        l->clipFrames = CLIP_REPEAT_FRAMES;
    }
}

/* ------------------------------------------------------------------------------- one run */

typedef struct {
    const options *opt;
    audio *audio;
    audio_rec *rec;
    band *band;
    view *view;
    int viewOwnsStdout;                      /* stdout is the terminal the view is on, so lines wait for it to close */
    int showArea;
    level level;
} session;

static void on_line(void *ctx, const band_line *line)
{
    session *s = ctx;
    if (s->view) {
        char stamp[OUTPUT_STAMP_SIZE];
        output_stamp(line, band_frame(s->band), stamp);
        view_line(s->view, stamp, line->hz, line->text, line->types);
    }
    if (!s->viewOwnsStdout) output_line(line, band_frame(s->band));
}

static void clock_text(const session *s, char *out, size_t size)
{
    int secs;
    if (audio_is_device(s->audio)) {
        time_t now = time(NULL);
        const struct tm *tm = gmtime(&now);
        secs = tm ? tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec : 0;
        snprintf(out, size, "%02d:%02d:%02d UTC", secs / 3600, secs / 60 % 60, secs % 60);
    } else {
        secs = (int)(band_frame(s->band) * FRAME_SECONDS);
        snprintf(out, size, "%02d:%02d:%02d", secs / 3600 % 100, secs / 60 % 60, secs % 60);
    }
}

static void draw_screen(session *s, int finished)
{
    if (s->view) {
        char clock[32];
        view_status st;
        clock_text(s, clock, sizeof clock);
        memset(&st, 0, sizeof st);
        st.clock = clock;
        st.hasLevel = audio_is_device(s->audio) || audio_is_stream(s->audio);
        st.levelKnown = s->level.known;
        st.levelDb = s->level.db;
        st.clipping = s->level.clipping;
        st.finished = finished;
        st.lostSeconds = audio_lost_samples(s->audio) / (double)VAIL_SAMPLE_RATE;
        st.hiddenLines = band_hidden_lines(s->band);
        st.showAll = s->opt->all;
        view_draw(s->view, s->band, &st);
    } else if (s->showArea) {
        char status[64];
        level_status(&s->level, status, sizeof status);
        output_area_draw(s->band, status);
    }
}

/* Picks how to show what is being heard. Returns 0 if what was asked for cannot be done. */
static int choose_display(session *s)
{
    const options *o = s->opt;
    int streaming = audio_is_device(s->audio) || audio_is_stream(s->audio);
    int terminal = output_is_terminal();
    int wantView = o->view || (terminal && streaming && !o->plain && !o->json);
    if (wantView) {
        const char *reason = NULL;
        char source[300];
        if (o->channel) snprintf(source, sizeof source, "%s, channel %d", audio_name(s->audio), o->channel);
        else snprintf(source, sizeof source, "%s", audio_name(s->audio));
        s->view = view_open(source, o->lo, o->hi, &reason);
        if (!s->view && o->view) {
            report("cannot open the full-screen view: %s", reason);
            return 0;
        }
    }
    if (s->view) {
        report_hold();
        s->viewOwnsStdout = terminal;
    } else if (terminal && audio_is_device(s->audio) && !o->plain && !o->json && terminal_takes_codes()) {
        s->showArea = 1;
    }
    return 1;
}

static int decode(session *s, int *status)
{
    const options *o = s->opt;
    long limit = o->seconds > 0 ? (long)ceil(o->seconds / FRAME_SECONDS) : 0;
    long lastKeys = 0, lastDraw = 0, fed = 0;
    int live = audio_is_device(s->audio) || audio_is_stream(s->audio);

    while (!g_stop && !output_ended() && (!limit || fed < limit)) {
        int16_t samples[VAIL_HOP_SAMPLES];
        int r = audio_read(s->audio, samples, VAIL_HOP_SAMPLES);
        long now;
        if (r == AUDIO_END) break;
        if (r == AUDIO_OK) {
            if (s->rec && !audio_rec_write(s->rec, samples, VAIL_HOP_SAMPLES)) {   /* the text goes on without the recording */
                output_area_clear();
                output_flush();
                audio_rec_close(s->rec);
                s->rec = NULL;
                *status = 1;
            }
            if (live) {
                measure_level(&s->level, samples);
                if (!s->view && !s->showArea && audio_is_device(s->audio)) level_warn(&s->level);
            }
            band_feed(s->band, samples);
            fed++;
        }
        now = now_ms();
        if (s->view && now - lastKeys >= KEYS_MS) {
            lastKeys = now;
            if (view_keys(s->view)) return 1;
        }
        if ((s->view || s->showArea) && now - lastDraw >= DRAW_MS) {
            lastDraw = now;
            draw_screen(s, 0);
        }
        if (r == AUDIO_WAIT) sleep_ms(5);
    }
    return g_stop || output_ended();
}

/* The audio has ended by itself, and the view stays until it is closed, so what it shows can be read. */
static void hold_view(session *s)
{
    while (!g_stop && !view_keys(s->view)) {
        draw_screen(s, 1);
        sleep_ms(HOLD_MS);
    }
}

static int run(const options *o)
{
    static session s;
    int status = 0, stoppedByUser;
    long lost;

    s.opt = o;
    s.level.db = -HUGE_VAL;
    if (o->path && !strcmp(o->path, "-") && stdin_is_terminal()) {
        report("a FILE of - reads raw samples from stdin, which is the terminal. Send them through a pipe, as in:");
        fprintf(stderr, "  arecord -t raw -f S16_LE -r 48000 | vail-cli -R 48000 -\n");
        return 2;
    }
    s.audio = !o->path ? audio_open_device(o->device, o->channel)
            : strcmp(o->path, "-") ? audio_open_file(o->path, o->channel)
            : audio_open_stdin(&o->raw, o->channel);
    if (!s.audio) return 1;
    if (o->recordPath && !(s.rec = audio_rec_open(o->recordPath))) { audio_close(s.audio); return 1; }
    s.band = band_new(o->lo, o->hi, o->all, on_line, &s);
    if (!s.band) { audio_rec_close(s.rec); audio_close(s.audio); return 1; }

    output_setup(o->json, audio_is_device(s.audio));
    watch_signals();                         /* before ncurses starts, which would otherwise set its own */
    if (!choose_display(&s)) { band_free(s.band); audio_rec_close(s.rec); audio_close(s.audio); return 1; }
    if (audio_is_device(s.audio) && !s.view)
        report("listening on %s, %d-%d Hz; Ctrl+C to stop", audio_name(s.audio), o->lo, o->hi);
    draw_screen(&s, 0);

    stoppedByUser = decode(&s, &status);

    if (audio_failed(s.audio)) {
        if (s.showArea) output_area_clear();
        output_flush();
        if (audio_is_device(s.audio)) report("the sound device stopped");
        status = 1;
    }
    band_finish(s.band);
    if (s.showArea) { output_area_clear(); output_flush(); }
    if (s.view && !stoppedByUser) hold_view(&s);

    if (s.view) {
        view_close(s.view);
        if (s.viewOwnsStdout) {
            long gone = view_print_lines(s.view, output_text);
            if (gone > 0) report("the first %ld lines were too many to keep; a log file keeps them all", gone);
        }
        report_release();
    }
    view_free(s.view);
    lost = audio_lost_samples(s.audio);
    if (lost > 0) report("decoding fell behind: lost %.1f s of audio", lost / (double)VAIL_SAMPLE_RATE);
    if (band_hidden_lines(s.band) > 0)
        report("left out %ld lines whose keying did not fit Morse; --all shows them", band_hidden_lines(s.band));

    band_free(s.band);
    if (!audio_rec_close(s.rec)) status = 1;
    audio_close(s.audio);
    return status;
}

int main(int argc, char **argv)
{
    options o;
    int status;
    if (!options_parse(argc, argv, &o)) {
        status = 2;
    } else if (o.help) {
        options_print_help();
        status = 0;
    } else if (o.version) {
        printf("vail-cli %s (vail engine %s)\nfull-screen view: %s\n", VERSION, vail_version(),
               view_available() ? "yes" : "no, ncurses was not found when this was built");
        status = 0;
    } else if (o.list) {
        status = audio_list();
    } else {
        status = run(&o);
    }
    output_finish();
    return output_failed() ? 1 : status;
}
