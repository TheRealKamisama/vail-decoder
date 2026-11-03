/* SPDX-License-Identifier: GPL-3.0-or-later */
/* audio — 8 kHz mono 16-bit audio from a sound device, a recording or stdin. */
#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>

typedef struct audio audio;

enum { AUDIO_END = -1, AUDIO_WAIT = 0, AUDIO_OK = 1 };

/* How raw samples on stdin are laid out: always little-endian and interleaved. */
typedef enum { AUDIO_S16, AUDIO_F32 } audio_format;

typedef struct {
    int rate;                                /* samples per second, 4000 or more */
    audio_format format;
    int channels;
} audio_raw;

/* Prints the input devices to stdout. Returns 0, or 1 with a message on stderr. */
int audio_list(void);

/* Each returns NULL with a message on stderr on failure.
 * device:  a number from audio_list, part of a device's name, or NULL for the system default.
 * path:    a sound file (WAV, FLAC, MP3), resampled as needed.
 * stdin:   raw samples, resampled as needed.
 * channel: decode this channel of the input, counting from 1; 0 mixes them all. */
audio *audio_open_device(const char *device, int channel);
audio *audio_open_file(const char *path, int channel);
audio *audio_open_stdin(const audio_raw *raw, int channel);

/* Reads exactly n samples, at most AUDIO_MAX_READ, or says why it cannot: AUDIO_WAIT means none are ready yet (never
 * from a file), so a program waiting for a stream can still see keys and signals; AUDIO_END that the source ended, on
 * its own or because a device stopped. */
#define AUDIO_MAX_READ 256
int audio_read(audio *a, int16_t *samples, int n);

const char *audio_name(const audio *a);          /* the device's name or the file's, for messages */
int  audio_is_device(const audio *a);            /* a sound device: its times are the time of day */
int  audio_is_stream(const audio *a);            /* samples on stdin */
int  audio_failed(const audio *a);               /* the device stopped on its own, or stdin could not be read */
long audio_lost_samples(const audio *a);         /* samples dropped because decoding fell behind */
void audio_close(audio *a);

/* A WAV recording of what is read; NULL with a message on stderr on failure. */
typedef struct audio_rec audio_rec;
audio_rec *audio_rec_open(const char *path);
int audio_rec_write(audio_rec *r, const int16_t *samples, int n);   /* 0 once the recording has failed */
int audio_rec_close(audio_rec *r);               /* 0 if any of it failed, with a message on stderr; r may be NULL */

#endif
