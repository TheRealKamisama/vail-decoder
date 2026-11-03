/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdlib.h>
#include "view.h"

#ifdef VAIL_CLI_NCURSES

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <curses.h>
#include "report.h"

#ifndef _WIN32
#include <unistd.h>
#endif

#define KEEP_LINES   20000                   /* about two days of a busy band, at a little over 100 bytes each */
#define STAMP_SIZE   9
#define PREFIX_COLS  16                      /* "HH:MM:SS  1234  " in a line of the log */
#define MIN_ROWS     9
#define MIN_COLS     40
#define MAX_SIGNALS  64
#define MAX_COLS     512
#define LEVEL_CELLS  10
#define LEVEL_RANGE  60.0                    /* dB shown on the meter */
#define PAIR_BAR     1
#define PAIR_ALERT   2
#define PAIR_LIVE    3

typedef struct {
    char stamp[STAMP_SIZE];
    int hz;
    char *text;
} log_line;

struct view {
    FILE *in, *out;
    SCREEN *screen;
    int colored;
    char source[128];
    int loHz, hiHz;
    log_line *log;                           /* a ring */
    long total;                              /* lines ever added */
    long clearedAt;                          /* lines from here on are shown */
    long scroll;                             /* lines back from the newest; 0 follows the new ones */
    int signalRows;                          /* grows with the band, so the rest of the screen stays put */
    int help, quit;
};

static const char *const HELP[] = {
    "q, Ctrl+C          quit",
    "c                  clear the log (it is still printed when you quit)",
    "Up, Down           scroll the log a line",
    "PgUp, PgDn         scroll it a page",
    "Home, End          go to the oldest line, or back to following the newest",
    "?                  this help",
    "",
    "SNR is the engine's estimate in dB, in its own narrow bandwidth. WPM is the sender's speed.",
    "A signal is listed once it has text that reads like Morse, which takes a few seconds of keying.",
    "Each finished line is final; the text beside a signal can still change.",
    "Lines also go to stdout if it is a file or a pipe, as in: vail-cli -T >> band.log",
    "",
    "Press any key to go back.",
};

int view_available(void) { return 1; }

static FILE *open_terminal(const char *windowsName, const char *mode)
{
#ifdef _WIN32
    return fopen(windowsName, mode);
#else
    (void)windowsName;
    return fopen("/dev/tty", mode);
#endif
}

view *view_open(const char *source, int loHz, int hiHz, const char **reason)
{
    view *v = calloc(1, sizeof *v);
#ifndef _WIN32
    const char *term = getenv("TERM");
    if (!term || !*term || !strcmp(term, "dumb")) {
        *reason = "this terminal cannot show it (TERM is not set, or is dumb)";
        free(v);
        return NULL;
    }
#endif
    *reason = "no terminal to use";
    if (!v) return NULL;
    v->log = calloc(KEEP_LINES, sizeof *v->log);
    v->in = open_terminal("CONIN$", "r");
    v->out = open_terminal("CONOUT$", "w");
    if (!v->log || !v->in || !v->out) goto fail;
#ifndef _WIN32
    if (tcgetpgrp(fileno(v->in)) != getpgrp()) {     /* in the background, touching the terminal would stop the program */
        *reason = "running in the background";
        goto fail;
    }
#endif
    v->screen = newterm(NULL, v->out, v->in);
    if (!v->screen) {
        *reason = "ncurses does not know this terminal";
        goto fail;
    }
    set_term(v->screen);
    cbreak();                                /* Ctrl+C stays a signal, which ends the run as it does without a view */
    noecho();
    nonl();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(0);
    if (has_colors() && !getenv("NO_COLOR")) {
        start_color();
        use_default_colors();
        init_pair(PAIR_BAR, COLOR_BLACK, COLOR_CYAN);
        init_pair(PAIR_ALERT, COLOR_WHITE, COLOR_RED);
        init_pair(PAIR_LIVE, COLOR_GREEN, -1);
        v->colored = 1;
    }
    snprintf(v->source, sizeof v->source, "%s", source);
    v->loHz = loHz;
    v->hiHz = hiHz;
    v->signalRows = 3;
    return v;

fail:
    if (v->in) fclose(v->in);
    if (v->out) fclose(v->out);
    free(v->log);
    free(v);
    return NULL;
}

void view_close(view *v)
{
    if (!v || !v->screen) return;
    set_term(v->screen);
    curs_set(1);
    endwin();
    delscreen(v->screen);
    v->screen = NULL;
    fclose(v->in);
    fclose(v->out);
    v->in = v->out = NULL;
}

void view_free(view *v)
{
    long i;
    if (!v) return;
    view_close(v);
    for (i = 0; i < KEEP_LINES; i++) free(v->log[i].text);
    free(v->log);
    free(v);
}

static long oldest_kept(const view *v)
{
    return v->total > KEEP_LINES ? v->total - KEEP_LINES : 0;
}

static long first_shown(const view *v)
{
    long kept = oldest_kept(v);
    return v->clearedAt > kept ? v->clearedAt : kept;
}

void view_line(view *v, const char *stamp, int hz, const char *text)
{
    log_line *l = &v->log[v->total % KEEP_LINES];
    char *copy = malloc(strlen(text) + 1);
    if (!copy) return;
    strcpy(copy, text);
    free(l->text);
    l->text = copy;
    snprintf(l->stamp, sizeof l->stamp, "%s", stamp);
    l->hz = hz;
    v->total++;
    if (v->scroll > 0) v->scroll++;          /* what is on the screen stays there */
}

long view_print_lines(const view *v, void (*print)(const char *stamp, int hz, const char *text))
{
    long i, kept = oldest_kept(v);
    for (i = kept; i < v->total; i++) {
        const log_line *l = &v->log[i % KEEP_LINES];
        print(l->stamp, l->hz, l->text);
    }
    return kept;
}

/* ---------------------------------------------------------------------------- keys */

static void scroll_log(view *v, long by)
{
    long most = v->total - first_shown(v) - 1;
    v->scroll += by;
    if (v->scroll > most) v->scroll = most;
    if (v->scroll < 0) v->scroll = 0;
}

int view_keys(view *v)
{
    int key, page = LINES > 8 ? LINES / 2 : 4;
    while ((key = wgetch(stdscr)) != ERR) {
        if (key == KEY_RESIZE) continue;
        if (v->help) {
            v->help = 0;
            continue;
        }
        switch (key) {
        case 'q': case 'Q': v->quit = 1; break;
        case '?': case 'h': case 'H': v->help = 1; break;
        case 'c': case 'C':
            v->clearedAt = v->total;
            v->scroll = 0;
            v->signalRows = 3;
            break;
        case KEY_UP: case 'k': scroll_log(v, 1); break;
        case KEY_DOWN: case 'j': scroll_log(v, -1); break;
        case KEY_PPAGE: scroll_log(v, page); break;
        case KEY_NPAGE: scroll_log(v, -page); break;
        case KEY_HOME: scroll_log(v, v->total); break;
        case KEY_END: v->scroll = 0; break;
        default: break;
        }
    }
    return v->quit;
}

/* --------------------------------------------------------------------------- drawing */

static void bar_on(const view *v) { if (v->colored) attron(COLOR_PAIR(PAIR_BAR)); else attron(A_REVERSE); }
static void bar_off(const view *v) { if (v->colored) attroff(COLOR_PAIR(PAIR_BAR)); else attroff(A_REVERSE); }

/* A whole row: text on the left and on the right, the left one giving way. */
static void draw_bar(const view *v, int row, const char *left, const char *right)
{
    int cols = COLS, r = (int)strlen(right), i;
    bar_on(v);
    move(row, 0);
    for (i = 0; i < cols; i++) addch(' ');
    if (r > cols) r = cols;
    mvaddnstr(row, 0, left, cols - r > 0 ? cols - r : 0);
    mvaddnstr(row, cols - r, right, r);
    bar_off(v);
}

static void format_level(const view_status *s, char *out, size_t size)
{
    char meter[LEVEL_CELLS + 1];
    int i, cells;
    if (!s->hasLevel || s->finished) {
        out[0] = 0;
        return;
    }
    if (!s->levelKnown) {
        snprintf(out, size, "waiting for audio ");
        return;
    }
    if (isinf(s->levelDb)) {
        snprintf(out, size, "silent ");
        return;
    }
    cells = (int)lrint((s->levelDb + LEVEL_RANGE) / LEVEL_RANGE * LEVEL_CELLS);
    if (cells < 0) cells = 0;
    if (cells > LEVEL_CELLS) cells = LEVEL_CELLS;
    for (i = 0; i < LEVEL_CELLS; i++) meter[i] = i < cells ? '#' : '.';
    meter[LEVEL_CELLS] = 0;
    snprintf(out, size, "%4.0f dBFS [%s] ", s->levelDb, meter);
}

static void draw_header(const view *v, const view_status *s)
{
    char left[256], right[96], level[48];
    snprintf(left, sizeof left, " vail-cli  %s  %d-%d Hz", v->source, v->loHz, v->hiHz);
    format_level(s, level, sizeof level);
    snprintf(right, sizeof right, "  %s  %s", s->clock, level);
    draw_bar(v, 0, left, right);
}

/* A signal's newest text, cut to fit, with .. where text has been left out. */
static void add_tail(int row, int col, int room, const char *text)
{
    int len = (int)strlen(text);
    if (room <= 2) return;
    if (len <= room) {
        mvaddnstr(row, col, text, room);
    } else {
        mvaddstr(row, col, "..");
        addnstr(text + len - (room - 2), room - 2);
    }
}

/* Returns the row after the pane. */
static int draw_signals(view *v, const band *b, const view_status *s, int row, int rows)
{
    static band_signal sig[MAX_SIGNALS];
    char title[96];
    int cols = COLS, want, n = band_signals(b, sig, MAX_SIGNALS, 0), i, shown;

    /* The pane takes up to half of what the log and it share; if the band is busier, the rest are counted in the title. */
    want = n > v->signalRows ? n : v->signalRows;
    if (want > (rows - 3) / 2) want = (rows - 3) / 2;
    if (want < 1) want = 1;
    v->signalRows = want;
    shown = n < want ? n : want;

    attron(A_BOLD);
    snprintf(title, sizeof title, " SIGNALS  %d", n);
    mvaddnstr(row, 0, title, cols);
    if (n > shown) { snprintf(title, sizeof title, ", %d not shown", n - shown); addnstr(title, cols); }
    attroff(A_BOLD);
    row++;
    attron(A_DIM);
    mvaddnstr(row, 0, "     Hz  SNR  WPM  STATE   TEXT", cols);
    attroff(A_DIM);
    row++;
    for (i = 0; i < want; i++, row++) {
        char left[64];
        int width;
        if (i >= shown) {
            if (i == 0 && !s->finished) {
                attron(A_DIM);
                mvaddnstr(row, 0, "   Listening. A signal needs a few seconds of keying to show here.", cols);
                attroff(A_DIM);
            }
            continue;
        }
        width = snprintf(left, sizeof left, "  %5d %4d %4d  %-6s  ", sig[i].hz, sig[i].snr, sig[i].wpm,
                         sig[i].decoding ? "decode" : "pause");
        if (sig[i].decoding && v->colored) attron(COLOR_PAIR(PAIR_LIVE));
        if (!sig[i].decoding) attron(A_DIM);
        mvaddnstr(row, 0, left, cols);
        add_tail(row, width, cols - width, sig[i].text);
        if (!sig[i].decoding) attroff(A_DIM);
        if (sig[i].decoding && v->colored) attroff(COLOR_PAIR(PAIR_LIVE));
    }
    return row;
}

/* How many bytes of text go on a row of this width, breaking between words if it can. */
static int fit_row(const char *text, int width)
{
    int len = (int)strlen(text), i;
    if (len <= width) return len;
    for (i = width; i > 0; i--) if (text[i] == ' ') return i;
    return width;
}

static int rows_of(const char *text, int width)
{
    int rows = 1, n;
    while ((n = fit_row(text, width)) < (int)strlen(text)) {
        text += n;
        while (*text == ' ') text++;
        rows++;
    }
    return rows;
}

/* Draws a line so that its last row is on `bottom`, leaving out whatever would go above `top`. */
static void draw_log_line(const log_line *l, int bottom, int top, int width, int rows)
{
    const char *text = l->text;
    int row = bottom - rows + 1, r;
    for (r = 0; r < rows; r++, row++) {
        int n = fit_row(text, width);
        if (row >= top) {
            if (r == 0) {
                char prefix[32];
                snprintf(prefix, sizeof prefix, "%s %5d  ", l->stamp, l->hz);
                mvaddnstr(row, 0, prefix, COLS);
            }
            mvaddnstr(row, PREFIX_COLS, text, n);
        }
        text += n;
        while (*text == ' ') text++;
    }
}

static void draw_log(const view *v, int row, int rows)
{
    long first = first_shown(v), shown = v->total - first, i;
    int width = COLS - PREFIX_COLS, bottom, used = 0;
    char title[96];
    if (v->scroll > 0) snprintf(title, sizeof title, " LOG  %ld lines, %ld newer below", shown, v->scroll);
    else snprintf(title, sizeof title, " LOG  %ld line%s", shown, shown == 1 ? "" : "s");
    attron(A_BOLD);
    mvaddnstr(row, 0, title, COLS);
    attroff(A_BOLD);
    if (shown == 0) {
        attron(A_DIM);
        mvaddnstr(row + 1, 0, "   Finished lines go here, when a signal pauses or a line fills up.", COLS);
        attroff(A_DIM);
        return;
    }
    /* Lines start at the top until there are enough to fill the pane; from then on, the newest is at the bottom. */
    for (i = v->total - 1 - v->scroll; i >= first && used < rows - 1; i--)
        used += rows_of(v->log[i % KEEP_LINES].text, width);
    bottom = row + (used < rows - 1 ? used : rows - 1);
    for (i = v->total - 1 - v->scroll; i >= first && bottom > row; i--) {
        const log_line *l = &v->log[i % KEEP_LINES];
        int need = rows_of(l->text, width);
        draw_log_line(l, bottom, row + 1, width, need);
        bottom -= need;
    }
}

static void draw_help(const view *v)
{
    int row = 2, i, n = (int)(sizeof HELP / sizeof HELP[0]);
    (void)v;
    attron(A_BOLD);
    mvaddnstr(1, 1, "Help", COLS - 1);
    attroff(A_BOLD);
    for (i = 0; i < n && row < LINES - 1; i++, row++) mvaddnstr(row, 2, HELP[i], COLS - 2);
}

static void draw_footer(const view *v, const view_status *s)
{
    char right[128];
    const char *alert = NULL, *held = report_last();
    right[0] = 0;
    if (held) alert = held;
    else if (s->clipping) alert = "input is clipping: turn the audio level down";
    else if (s->levelKnown && isinf(s->levelDb) && !s->finished) alert = "input is silent: check the device and its level";
    if (alert) {
        int cols = COLS;
        draw_bar(v, LINES - 1, s->finished ? " q quit" : " q quit  ? help", "");
        if (v->colored) attron(COLOR_PAIR(PAIR_ALERT)); else attron(A_BOLD);
        {
            int len = (int)strlen(alert) + 2, at = cols - len;
            if (at < 16) at = 16;
            if (at < cols) mvaddnstr(LINES - 1, at, alert, cols - at - 1);
        }
        if (v->colored) attroff(COLOR_PAIR(PAIR_ALERT)); else attroff(A_BOLD);
        return;
    }
    if (s->finished) snprintf(right, sizeof right, "finished: q to quit ");
    else if (v->scroll > 0) snprintf(right, sizeof right, "scrolled back: End to follow ");
    else if (s->lostSeconds > 0) snprintf(right, sizeof right, "decoding fell behind: lost %.1f s ", s->lostSeconds);
    else if (s->hiddenLines > 0 && !s->showAll) snprintf(right, sizeof right, "%ld lines hidden as not Morse (--all) ", s->hiddenLines);
    {
        const char *keys = " q quit  c clear  Up/Down PgUp/PgDn scroll  ? help";
        if ((int)(strlen(keys) + strlen(right)) >= COLS) keys = " q quit  ? help";
        draw_bar(v, LINES - 1, keys, right);
    }
}

void view_draw(view *v, const band *b, const view_status *s)
{
    int row;
    erase();
    if (LINES < MIN_ROWS || COLS < MIN_COLS) {
        mvaddnstr(0, 0, "vail-cli: this window is too small; q quits", COLS);
        refresh();
        return;
    }
    draw_header(v, s);
    if (v->help) {
        draw_help(v);
    } else {
        row = draw_signals(v, b, s, 1, LINES - 2);
        draw_log(v, row, LINES - 1 - row);
    }
    draw_footer(v, s);
    refresh();
}

#else /* no ncurses */

int view_available(void) { return 0; }

view *view_open(const char *source, int loHz, int hiHz, const char **reason)
{
    (void)source; (void)loHz; (void)hiHz;
    *reason = "this build has none, as ncurses was not found when it was built";
    return NULL;
}

void view_close(view *v) { (void)v; }
void view_free(view *v) { (void)v; }
int  view_keys(view *v) { (void)v; return 0; }
void view_line(view *v, const char *stamp, int hz, const char *text) { (void)v; (void)stamp; (void)hz; (void)text; }
void view_draw(view *v, const band *b, const view_status *status) { (void)v; (void)b; (void)status; }
long view_print_lines(const view *v, void (*print)(const char *, int, const char *)) { (void)v; (void)print; return 0; }

#endif
