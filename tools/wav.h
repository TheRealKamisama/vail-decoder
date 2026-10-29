/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Minimal PCM WAV reading/writing for the vail tools (16-bit mono). */
#ifndef VAIL_WAV_H
#define VAIL_WAV_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline uint32_t wav_u32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static inline uint16_t wav_u16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* Reads a 16-bit PCM WAV (mono, or the first channel of a multi-channel file).
 * Returns malloc'd samples, sets *n and *rate; NULL on error. A file that is not RIFF is read as raw s16le. */
static inline int16_t *wav_read(const char *path, long *n, int *rate)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf, *p, *end;
    long size;
    int16_t *out = NULL;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); size = ftell(f); fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)size + 1);
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) { fclose(f); free(buf); return NULL; }
    fclose(f);
    *rate = 8000;
    if (size < 12 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) {
        *n = size / 2;
        out = malloc((size_t)(*n ? *n : 1) * sizeof(int16_t));
        memcpy(out, buf, (size_t)*n * 2);
        free(buf);
        return out;
    }
    p = buf + 12; end = buf + size;
    {
        int channels = 1, bits = 16;
        while (p + 8 <= end) {
            uint32_t len = wav_u32(p + 4);
            if (!memcmp(p, "fmt ", 4) && p + 24 <= end) {
                channels = wav_u16(p + 10);
                *rate = (int)wav_u32(p + 12);
                bits = wav_u16(p + 22);
            } else if (!memcmp(p, "data", 4)) {
                long i, frames;
                if (bits != 16 || channels < 1) break;
                if (p + 8 + len > end) len = (uint32_t)(end - p - 8);
                frames = (long)len / (2 * channels);
                out = malloc((size_t)(frames ? frames : 1) * sizeof(int16_t));
                for (i = 0; i < frames; i++) out[i] = (int16_t)wav_u16(p + 8 + 2 * channels * i);
                *n = frames;
                break;
            }
            p += 8 + len + (len & 1);
        }
    }
    free(buf);
    return out;
}

static inline void wav_put32(FILE *f, uint32_t v) { unsigned char b[4] = { v, v >> 8, v >> 16, v >> 24 }; fwrite(b, 1, 4, f); }
static inline void wav_put16(FILE *f, uint16_t v) { unsigned char b[2] = { v, v >> 8 }; fwrite(b, 1, 2, f); }

static inline int wav_write(const char *path, const int16_t *s, long n, int rate)
{
    FILE *f = fopen(path, "wb");
    long i;
    if (!f) return -1;
    fwrite("RIFF", 1, 4, f); wav_put32(f, (uint32_t)(36 + 2 * n)); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); wav_put32(f, 16); wav_put16(f, 1); wav_put16(f, 1);
    wav_put32(f, (uint32_t)rate); wav_put32(f, (uint32_t)rate * 2); wav_put16(f, 2); wav_put16(f, 16);
    fwrite("data", 1, 4, f); wav_put32(f, (uint32_t)(2 * n));
    for (i = 0; i < n; i++) wav_put16(f, (uint16_t)s[i]);
    fclose(f);
    return 0;
}

#endif
