/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdarg.h>
#include <stdio.h>
#include "report.h"

#define HELD_MAX 16
#define HELD_LEN 512

static int g_holding;
static int g_held;
static char g_message[HELD_MAX][HELD_LEN];

void report(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (g_holding) {
        /* The first messages say what went wrong, so the last slot is the only one to be overwritten. */
        int slot = g_held < HELD_MAX ? g_held++ : HELD_MAX - 1;
        vsnprintf(g_message[slot], HELD_LEN, fmt, ap);
    } else {
        fputs("vail-cli: ", stderr);
        vfprintf(stderr, fmt, ap);
        fputc('\n', stderr);
    }
    va_end(ap);
}

void report_hold(void) { g_holding = 1; }

void report_release(void)
{
    int i;
    g_holding = 0;
    for (i = 0; i < g_held; i++) fprintf(stderr, "vail-cli: %s\n", g_message[i]);
    g_held = 0;
}

const char *report_last(void) { return g_held ? g_message[g_held - 1] : NULL; }
