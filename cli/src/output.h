/* SPDX-License-Identifier: GPL-3.0-or-later */
/* output — what goes to stdout: finished lines, and on a terminal without the full-screen view, the signals being heard
 * below them. A reader that goes away ends the run; any other failed write is an error too. */
#ifndef OUTPUT_H
#define OUTPUT_H

#include "band.h"

#define OUTPUT_STAMP_SIZE 9                  /* HH:MM:SS */

/* wallClock: times are UTC rather than from the start of the audio. */
void output_setup(int json, int wallClock);

/* The time of a line. */
void output_stamp(const band_line *line, long nowFrame, char stamp[OUTPUT_STAMP_SIZE]);

void output_line(const band_line *line, long nowFrame);
void output_text(const char *stamp, int hz, const char *text);   /* a line that was held back */

/* Below the finished lines: each signal's unprinted text, then a status line. */
void output_area_draw(const band *b, const char *status);
void output_area_clear(void);

void output_flush(void);
int  output_ended(void);                     /* nothing more can be written */
int  output_failed(void);                    /* and that was an error */
void output_finish(void);                    /* closes stdout, which can be the first to report a failed write */

int  output_is_terminal(void);

#endif
