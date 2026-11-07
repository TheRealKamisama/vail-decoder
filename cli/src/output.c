/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "output.h"
#include "report.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#define FRAME_SECONDS (VAIL_HOP_SAMPLES / (double)VAIL_SAMPLE_RATE)
#define MAX_ROWS 64
#define MAX_COLS 240

static int g_json, g_wallClock;
static int g_ended, g_failed;
static int g_areaRows;
static char g_area[(MAX_ROWS + 1) * (MAX_COLS + 16)], g_lastArea[(MAX_ROWS + 1) * (MAX_COLS + 16)];

/* Call straight after the failed write. */
static int reader_is_gone(int err)
{
#ifdef _WIN32
    DWORD e = GetLastError();                /* the C library calls a closed pipe EINVAL */
    if (e == ERROR_NO_DATA || e == ERROR_BROKEN_PIPE) return 1;
#endif
    return err == EPIPE;
}

static void write_failed(int err)
{
    if (g_ended) return;
    g_ended = 1;
    if (reader_is_gone(err)) return;
    report("cannot write the output: %s", strerror(err));
    g_failed = 1;
}

void output_flush(void)
{
    if (fflush(stdout) == EOF) write_failed(errno);
}

int output_ended(void) { return g_ended; }
int output_failed(void) { return g_failed; }

void output_finish(void)
{
    /* A stdout that is already closed is only a failure if there was something to write, and flushing has said so. */
    output_flush();
    if (!g_ended && fclose(stdout) == EOF && errno != EBADF) write_failed(errno);
}

int output_is_terminal(void)
{
#ifdef _WIN32
    return _isatty(_fileno(stdout));
#else
    return isatty(1);
#endif
}

void output_setup(int json, int wallClock)
{
    g_json = json;
    g_wallClock = wallClock;
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);
}

static time_t when_heard(const band_line *line, long nowFrame)
{
    return time(NULL) - (time_t)((nowFrame - line->frame) * FRAME_SECONDS);
}

void output_stamp(const band_line *line, long nowFrame, char stamp[OUTPUT_STAMP_SIZE])
{
    unsigned secs;
    if (g_wallClock) {
        time_t when = when_heard(line, nowFrame);
        const struct tm *tm = gmtime(&when);
        secs = tm ? (unsigned)(tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec) : 0;
    } else {
        secs = (unsigned)(line->frame * FRAME_SECONDS);
    }
    stamp[0] = (char)('0' + secs / 36000 % 10);
    stamp[1] = (char)('0' + secs / 3600 % 10);
    stamp[2] = ':';
    stamp[3] = (char)('0' + secs / 600 % 6);
    stamp[4] = (char)('0' + secs / 60 % 10);
    stamp[5] = ':';
    stamp[6] = (char)('0' + secs / 10 % 6);
    stamp[7] = (char)('0' + secs % 10);
    stamp[8] = 0;
}

void output_text(const char *stamp, int hz, const char *text)
{
    printf("%s %5d  %s\n", stamp, hz, text);
}

/* The field names are those of libmorsedec's JSON Lines, as far as they mean the same. The score is rounded down, so that
 * "score" >= 1 picks the lines that are shown without --all. */
static void put_json(const band_line *line, long nowFrame)
{
    const unsigned char *p;
    printf("{\"time\":%.3f,", line->frame * FRAME_SECONDS);
    if (g_wallClock) {
        time_t when = when_heard(line, nowFrame);
        const struct tm *tm = gmtime(&when);
        if (tm) {
            printf("\"utc\":\"%04d-%02d-%02dT%02d:%02d:%02dZ\",", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                   tm->tm_hour, tm->tm_min, tm->tm_sec);
        }
    }
    printf("\"channel\":%d,\"frequency\":%d,\"snr\":%d,\"wpm\":%d,\"score\":%.1f,\"text\":\"", line->channel, line->hz,
           line->snr, line->wpm, floor(line->score * 10.0) / 10.0);
    for (p = (const unsigned char *)line->text; *p; p++) {
        if (*p == '"' || *p == '\\') printf("\\%c", *p);
        else if (*p < 32 || *p >= 127) printf("\\u%04x", *p);
        else putchar(*p);
    }
    printf("\"}\n");
}

void output_line(const band_line *line, long nowFrame)
{
    output_area_clear();
    if (g_json) {
        put_json(line, nowFrame);
    } else {
        char stamp[OUTPUT_STAMP_SIZE];
        output_stamp(line, nowFrame, stamp);
        output_text(stamp, line->hz, line->text);
    }
    output_flush();
}

/* ---------------------------------------------------------------------------- the live area */

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

void output_area_clear(void)
{
    if (g_areaRows > 0) {
        printf("\033[%dA\r\033[J", g_areaRows);
        g_areaRows = 0;
    }
    g_lastArea[0] = 0;
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

void output_area_draw(const band *b, const char *status)
{
    static band_signal sig[MAX_ROWS];
    char *p = g_area;
    int rows, cols, n, i;
    terminal_size(&rows, &cols);
    rows -= 2;                               /* the status row, and the cursor's */
    if (rows > MAX_ROWS) rows = MAX_ROWS;
    n = band_signals(b, sig, rows > 0 ? rows : 0, 1);
    for (i = 0; i < n; i++) p = put_row(p, cols, sig[i].hz, sig[i].text);
    p += sprintf(p, "\033[2m%s\033[0m\n", status);
    if (!strcmp(g_area, g_lastArea)) return;
    output_area_clear();
    fputs(g_area, stdout);
    g_areaRows = n + 1;
    strcpy(g_lastArea, g_area);
    output_flush();
}
