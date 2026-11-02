/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio.h"
#include "miniaudio.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#define RATE       8000
#define RING_FRAMES (4 * RATE)               /* room for 4 s if decoding stalls */
#define FILTER_ORDER MA_MAX_FILTER_ORDER     /* keeps what lies above 4 kHz from aliasing into the band */

typedef enum { SRC_DEVICE, SRC_FILE, SRC_STDIN } source;

struct audio {
    source kind;
    char name[256];
    ma_context context;
    ma_device device;
    ma_pcm_rb ring;
    ma_decoder decoder;
    volatile ma_uint32 stopping, stopped;
    volatile long lost;
};

struct audio_rec {
    ma_encoder encoder;
};

static void complain(const char *what, ma_result r)
{
    fprintf(stderr, "vail-cli: %s: %s\n", what, ma_result_description(r));
}

int audio_list(void)
{
    ma_context ctx;
    ma_device_info *infos;
    ma_uint32 n, i;
    ma_result r = ma_context_init(NULL, 0, NULL, &ctx);
    if (r != MA_SUCCESS) { complain("no sound system", r); return 1; }
    r = ma_context_get_devices(&ctx, NULL, NULL, &infos, &n);
    if (r != MA_SUCCESS) { complain("cannot list devices", r); ma_context_uninit(&ctx); return 1; }
    for (i = 0; i < n; i++)
        printf("%3u  %s%s\n", (unsigned)i, infos[i].name, infos[i].isDefault ? "  (default)" : "");
    if (n == 0) fprintf(stderr, "vail-cli: no input devices found\n");
    ma_context_uninit(&ctx);
    return n == 0;
}

static int contains_nocase(const char *haystack, const char *needle)
{
    size_t i, k, n = strlen(needle);
    for (i = 0; haystack[i]; i++) {
        for (k = 0; k < n && haystack[i + k]; k++)
            if (tolower((unsigned char)haystack[i + k]) != tolower((unsigned char)needle[k])) break;
        if (k == n) return 1;
    }
    return 0;
}

/* Finds what -d names: its number in the list, or the one device with this in its name. */
static const ma_device_info *select_device(const ma_device_info *infos, ma_uint32 n, const char *sel)
{
    const ma_device_info *found = NULL;
    ma_uint32 i;
    if (sel[0] && strspn(sel, "0123456789") == strlen(sel)) {
        unsigned long k = strtoul(sel, NULL, 10);
        if (k < n) return &infos[k];
        fprintf(stderr, "vail-cli: there is no device %s; see vail-cli -l\n", sel);
        return NULL;
    }
    for (i = 0; i < n; i++) {
        if (!contains_nocase(infos[i].name, sel)) continue;
        if (found) {
            fprintf(stderr, "vail-cli: \"%s\" matches more than one device:\n  %s\n  %s\n",
                    sel, found->name, infos[i].name);
            return NULL;
        }
        found = &infos[i];
    }
    if (!found) fprintf(stderr, "vail-cli: no device matches \"%s\"; see vail-cli -l\n", sel);
    return found;
}

static void on_samples(ma_device *dev, void *out, const void *in, ma_uint32 frames)
{
    audio *a = dev->pUserData;
    const ma_int16 *p = in;
    (void)out;
    while (frames > 0) {
        ma_uint32 n = frames;
        void *dst;
        if (ma_pcm_rb_acquire_write(&a->ring, &n, &dst) != MA_SUCCESS || n == 0) {
            a->lost += (long)frames;
            return;
        }
        memcpy(dst, p, n * sizeof *p);
        ma_pcm_rb_commit_write(&a->ring, n);
        p += n;
        frames -= n;
    }
}

static void on_notification(const ma_device_notification *note)
{
    audio *a = note->pDevice->pUserData;
    if (note->type == ma_device_notification_type_stopped) a->stopped = 1;
}

audio *audio_open_device(const char *sel)
{
    audio *a = calloc(1, sizeof *a);
    ma_device_config cfg;
    ma_device_info *infos;
    ma_uint32 n;
    const ma_device_info *info = NULL;
    ma_result r;

    if (!a) return NULL;
    a->kind = SRC_DEVICE;
    r = ma_context_init(NULL, 0, NULL, &a->context);
    if (r != MA_SUCCESS) { complain("no sound system", r); free(a); return NULL; }
    if (sel) {
        r = ma_context_get_devices(&a->context, NULL, NULL, &infos, &n);
        if (r != MA_SUCCESS) { complain("cannot list devices", r); goto fail_context; }
        info = select_device(infos, n, sel);
        if (!info) goto fail_context;
    }
    r = ma_pcm_rb_init(ma_format_s16, 1, RING_FRAMES, NULL, NULL, &a->ring);
    if (r != MA_SUCCESS) { complain("out of memory", r); goto fail_context; }

    cfg = ma_device_config_init(ma_device_type_capture);
    cfg.capture.pDeviceID = info ? &info->id : NULL;
    cfg.capture.format = ma_format_s16;
    cfg.capture.channels = 1;                         /* stereo is mixed, so both sides are decoded */
    cfg.sampleRate = RATE;
    cfg.resampling.algorithm = ma_resample_algorithm_linear;
    cfg.resampling.linear.lpfOrder = FILTER_ORDER;
    cfg.performanceProfile = ma_performance_profile_conservative;
    cfg.dataCallback = on_samples;
    cfg.notificationCallback = on_notification;
    cfg.pUserData = a;
    r = ma_device_init(&a->context, &cfg, &a->device);
    if (r != MA_SUCCESS) { complain("cannot open the device", r); goto fail_ring; }
    snprintf(a->name, sizeof a->name, "%s", a->device.capture.name);
    r = ma_device_start(&a->device);
    if (r != MA_SUCCESS) { complain("cannot start the device", r); goto fail_device; }
    return a;

fail_device:
    ma_device_uninit(&a->device);
fail_ring:
    ma_pcm_rb_uninit(&a->ring);
fail_context:
    ma_context_uninit(&a->context);
    free(a);
    return NULL;
}

audio *audio_open_file(const char *path)
{
    audio *a = calloc(1, sizeof *a);
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_s16, 1, RATE);
    ma_result r;
    if (!a) return NULL;
    a->kind = SRC_FILE;
    cfg.resampling.algorithm = ma_resample_algorithm_linear;
    cfg.resampling.linear.lpfOrder = FILTER_ORDER;
    r = ma_decoder_init_file(path, &cfg, &a->decoder);
    if (r != MA_SUCCESS) {
        fprintf(stderr, "vail-cli: cannot read %s: %s\n", path, ma_result_description(r));
        free(a);
        return NULL;
    }
    snprintf(a->name, sizeof a->name, "%s", path);
    return a;
}

audio *audio_open_stdin(void)
{
    audio *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    a->kind = SRC_STDIN;
    strcpy(a->name, "stdin");
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    return a;
}

static int read_device(audio *a, int16_t *samples, int n)
{
    int got = 0;
    if (a->stopped) return AUDIO_END;
    if ((int)ma_pcm_rb_available_read(&a->ring) < n) return AUDIO_WAIT;
    while (got < n) {
        ma_uint32 k = (ma_uint32)(n - got);
        void *src;
        if (ma_pcm_rb_acquire_read(&a->ring, &k, &src) != MA_SUCCESS || k == 0) break;
        memcpy(samples + got, src, k * sizeof *samples);
        ma_pcm_rb_commit_read(&a->ring, k);
        got += (int)k;
    }
    return got == n ? AUDIO_OK : AUDIO_WAIT;
}

int audio_read(audio *a, int16_t *samples, int n)
{
    if (a->kind == SRC_DEVICE) return read_device(a, samples, n);
    if (a->kind == SRC_FILE) {
        ma_uint64 got = 0;
        ma_decoder_read_pcm_frames(&a->decoder, samples, (ma_uint64)n, &got);
        if (got == 0) return AUDIO_END;
        if (got < (ma_uint64)n) memset(samples + got, 0, ((size_t)n - (size_t)got) * sizeof *samples);
        return AUDIO_OK;
    }
    {
        unsigned char raw[2 * 256];
        size_t got;
        int i;
        if (n > 256) n = 256;
        got = fread(raw, 2, (size_t)n, stdin);
        if (got == 0) return AUDIO_END;
        for (i = 0; i < n; i++)
            samples[i] = (size_t)i < got ? (int16_t)((unsigned)raw[2 * i] | (unsigned)raw[2 * i + 1] << 8) : 0;
        return AUDIO_OK;
    }
}

const char *audio_name(const audio *a) { return a->name; }
int  audio_is_live(const audio *a) { return a->kind == SRC_DEVICE; }
int  audio_failed(const audio *a) { return a->kind == SRC_DEVICE && a->stopped; }
long audio_lost_samples(const audio *a) { return a->lost; }

void audio_close(audio *a)
{
    if (!a) return;
    if (a->kind == SRC_DEVICE) {
        ma_device_uninit(&a->device);
        ma_pcm_rb_uninit(&a->ring);
        ma_context_uninit(&a->context);
    } else if (a->kind == SRC_FILE) {
        ma_decoder_uninit(&a->decoder);
    }
    free(a);
}

audio_rec *audio_rec_open(const char *path)
{
    audio_rec *r = calloc(1, sizeof *r);
    ma_encoder_config cfg = ma_encoder_config_init(ma_encoding_format_wav, ma_format_s16, 1, RATE);
    ma_result e;
    if (!r) return NULL;
    e = ma_encoder_init_file(path, &cfg, &r->encoder);
    if (e != MA_SUCCESS) {
        fprintf(stderr, "vail-cli: cannot write %s: %s\n", path, ma_result_description(e));
        free(r);
        return NULL;
    }
    return r;
}

void audio_rec_write(audio_rec *r, const int16_t *samples, int n)
{
    ma_encoder_write_pcm_frames(&r->encoder, samples, (ma_uint64)n, NULL);
}

void audio_rec_close(audio_rec *r)
{
    if (!r) return;
    ma_encoder_uninit(&r->encoder);
    free(r);
}
