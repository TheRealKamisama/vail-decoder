/* SPDX-License-Identifier: GPL-3.0-or-later */
/* band — every CW signal in the audio passband, as settled lines of text.
 *
 * One FFT covers 0-4 kHz. Overlapping slices of its spectrum each feed their own vail engine, which is
 * how one engine's 1 kHz view becomes the whole passband. This module does no I/O. */
#ifndef BAND_H
#define BAND_H

#include <stdint.h>
#include "vail.h"

#define BAND_MIN_HZ 200
#define BAND_MAX_HZ 3900
#define BAND_LIVE_TEXT 320                   /* the bytes of a signal's newest text that are kept for showing */

typedef struct band band;

/* A finished piece of text. `frame` is when it started; one frame is 85 samples at 8 kHz. */
typedef struct {
    long frame;
    int channel;                             /* lines with the same number came from one channel of the engine */
    int hz, snr, wpm;
    float score;                             /* how far the line's keying is Morse, a log-odds: the larger of the score as
                                                it printed and its mean while its text came in */
    const char *text;
} band_line;

typedef void band_line_fn(void *ctx, const band_line *line);

/* A signal being heard. */
typedef struct {
    int hz, snr, wpm;
    float score;
    int decoding;                            /* else it has paused */
    char text[BAND_LIVE_TEXT];               /* its newest text, which may still change */
} band_signal;

/* showAll: print text whose keying does not fit Morse too. */
band *band_new(int loHz, int hiHz, int showAll, band_line_fn *onLine, void *ctx);
void  band_free(band *b);

/* Decodes one frame of audio; calls onLine for anything that became final. */
void  band_feed(band *b, const int16_t samples[VAIL_HOP_SAMPLES]);

/* Feeds enough silence to finish what is in flight, then calls onLine for everything left. */
void  band_finish(band *b);

/* Writes up to max signals to out, lowest pitch first, and returns how many. With unprintedOnly, only those with text
 * that has not been printed yet, and only that text. */
int   band_signals(const band *b, band_signal *out, int max, int unprintedOnly);

long  band_hidden_lines(const band *b);      /* lines left out because their keying did not fit Morse */
long  band_frame(const band *b);

#endif
