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

typedef struct band band;

/* A finished piece of text. `frame` is when it started; one frame is 85 samples at 8 kHz. */
typedef void band_line_fn(void *ctx, long frame, int hz, const char *text);

/* A signal being heard. `text` has not been printed yet and may still change. */
typedef struct {
    int hz;
    const char *text;
} band_signal;

band *band_new(int loHz, int hiHz, band_line_fn *onLine, void *ctx);
void  band_free(band *b);

/* Decodes one frame of audio; calls onLine for anything that became final. */
void  band_feed(band *b, const int16_t samples[VAIL_HOP_SAMPLES]);

/* Feeds enough silence to finish what is in flight, then calls onLine for everything left. */
void  band_finish(band *b);

/* Writes up to max signals with unprinted text to out, lowest pitch first, and returns how many. */
int   band_signals(const band *b, band_signal *out, int max);

long  band_frame(const band *b);

#endif
