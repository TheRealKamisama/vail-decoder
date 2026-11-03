/* SPDX-License-Identifier: GPL-3.0-or-later */
/* audio — 8 kHz mono 16-bit audio from a sound device, a recording or stdin. */
#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>

typedef struct audio audio;

enum { AUDIO_END = -1, AUDIO_WAIT = 0, AUDIO_OK = 1 };

/* Prints the input devices to stdout. Returns 0, or 1 with a message on stderr. */
int audio_list(void);

/* Each returns NULL with a message on stderr on failure.
 * device: a number from audio_list, part of a device's name, or NULL for the system default.
 * path:   a sound file (WAV, FLAC, MP3), resampled as needed.
 * stdin:  raw signed 16-bit little-endian samples, 8 kHz mono. */
audio *audio_open_device(const char *device);
audio *audio_open_file(const char *path);
audio *audio_open_stdin(void);

/* Reads exactly n samples, or says why it cannot: AUDIO_WAIT means none are ready yet (devices only),
 * AUDIO_END that the source ended, on its own or because a device stopped. */
int audio_read(audio *a, int16_t *samples, int n);

const char *audio_name(const audio *a);          /* the device's name, for messages */
int  audio_is_live(const audio *a);              /* a sound device, as opposed to a file or a stream */
int  audio_failed(const audio *a);               /* the device stopped on its own */
long audio_lost_samples(const audio *a);         /* samples dropped because decoding fell behind */
void audio_close(audio *a);

/* A WAV recording of what is read; NULL with a message on stderr on failure. */
typedef struct audio_rec audio_rec;
audio_rec *audio_rec_open(const char *path);
int audio_rec_write(audio_rec *r, const int16_t *samples, int n);   /* 0 once the recording has failed */
int audio_rec_close(audio_rec *r);               /* 0 if any of it failed, with a message on stderr; r may be NULL */

#endif
