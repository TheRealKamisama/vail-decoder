/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio.h"
#include "miniaudio.h"
#include "report.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <poll.h>
#include <unistd.h>
#endif

#define RATE       8000
#define RING_FRAMES (4 * RATE)               /* room for 4 s if decoding stalls */
#define FILTER_ORDER MA_MAX_FILTER_ORDER     /* keeps what lies above 4 kHz from aliasing into the band */

/* Bytes taken from stdin at a time, and what they can become: at the lowest rate, 4 kHz, resampling doubles them. */
#define STREAM_BYTES   8192
#define STREAM_SAMPLES (STREAM_BYTES / 2)
#define STREAM_FIFO    (2 * STREAM_SAMPLES + 2 * AUDIO_MAX_READ)

typedef enum { SRC_DEVICE, SRC_FILE, SRC_STDIN } source;

struct audio {
    source kind;
    char name[256];
    int channel;                             /* counting from 1; 0: mixed */
    ma_context context;
    ma_device device;
    ma_pcm_rb ring;
    ma_decoder decoder;
    int16_t *fileFrames;                     /* a file's frames, all its channels, when one is being picked out */
    int fileChannels;
    volatile ma_uint32 stopped;
    volatile long lost;
    /* stdin */
    audio_raw raw;
    int frameBytes;
    ma_resampler resampler;
    int resampling, ended, readFailed;
    unsigned char pending[STREAM_BYTES];
    size_t pendingBytes;
    float mono[STREAM_SAMPLES];
    float resampled[2 * STREAM_SAMPLES];
    int16_t fifo[STREAM_FIFO];
    int fifoLen;
};

struct audio_rec {
    ma_encoder encoder;
    FILE *file;                              /* ours, as miniaudio does not report a failed write to its own files */
    const char *path;
    int error;                               /* errno of the first failure */
};

static void complain(const char *what, ma_result r)
{
    report("%s: %s", what, ma_result_description(r));
}

static void print_devices(FILE *to, const ma_device_info *infos, ma_uint32 n)
{
    ma_uint32 i;
    for (i = 0; i < n; i++)
        fprintf(to, "%3u  %s%s\n", (unsigned)i, infos[i].name, infos[i].isDefault ? "  (default)" : "");
}

int audio_list(void)
{
    ma_context ctx;
    ma_device_info *infos;
    ma_uint32 n;
    ma_result r = ma_context_init(NULL, 0, NULL, &ctx);
    if (r != MA_SUCCESS) { complain("no sound system", r); return 1; }
    r = ma_context_get_devices(&ctx, NULL, NULL, &infos, &n);
    if (r != MA_SUCCESS) { complain("cannot list devices", r); ma_context_uninit(&ctx); return 1; }
    print_devices(stdout, infos, n);
    if (n == 0) report("no input devices found");
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
        report("there is no device %s; these are the input devices:", sel);
        print_devices(stderr, infos, n);
        return NULL;
    }
    for (i = 0; i < n; i++) {
        if (!contains_nocase(infos[i].name, sel)) continue;
        if (found) {
            report("\"%s\" matches more than one device; give its number or more of its name:", sel);
            print_devices(stderr, infos, n);
            return NULL;
        }
        found = &infos[i];
    }
    if (!found) {
        report("no device matches \"%s\"; these are the input devices:", sel);
        print_devices(stderr, infos, n);
    }
    return found;
}

static void on_samples(ma_device *dev, void *out, const void *in, ma_uint32 frames)
{
    audio *a = dev->pUserData;
    const ma_int16 *p = in;
    ma_uint32 stride = a->channel ? dev->capture.channels : 1;
    (void)out;
    if (a->channel) p += a->channel - 1;
    while (frames > 0) {
        ma_uint32 n = frames, i;
        void *dst;
        if (ma_pcm_rb_acquire_write(&a->ring, &n, &dst) != MA_SUCCESS || n == 0) {
            a->lost += (long)frames;
            return;
        }
        for (i = 0; i < n; i++) ((ma_int16 *)dst)[i] = p[i * stride];
        ma_pcm_rb_commit_write(&a->ring, n);
        p += n * stride;
        frames -= n;
    }
}

static void on_notification(const ma_device_notification *note)
{
    audio *a = note->pDevice->pUserData;
    if (note->type == ma_device_notification_type_stopped) a->stopped = 1;
}

audio *audio_open_device(const char *sel, int channel)
{
    audio *a = calloc(1, sizeof *a);
    ma_device_config cfg;
    ma_device_info *infos;
    ma_uint32 n;
    const ma_device_info *info = NULL;
    ma_result r;

    if (!a) return NULL;
    a->kind = SRC_DEVICE;
    a->channel = channel;
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
    cfg.capture.channels = channel ? 0 : 1;           /* 0: as the device has them. Mixed, both sides of stereo decode */
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
    if (channel > (int)a->device.capture.channels) {
        report("%s has only %u channel(s), so there is no channel %d", a->name, (unsigned)a->device.capture.channels, channel);
        goto fail_device;
    }
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

audio *audio_open_file(const char *path, int channel)
{
    audio *a = calloc(1, sizeof *a);
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_s16, channel ? 0 : 1, RATE);   /* 0: as the file has them */
    ma_result r;
    if (!a) return NULL;
    a->kind = SRC_FILE;
    a->channel = channel;
    cfg.resampling.algorithm = ma_resample_algorithm_linear;
    cfg.resampling.linear.lpfOrder = FILTER_ORDER;
    r = ma_decoder_init_file(path, &cfg, &a->decoder);
    if (r != MA_SUCCESS) {
        report("cannot read %s: %s", path, ma_result_description(r));
        free(a);
        return NULL;
    }
    snprintf(a->name, sizeof a->name, "%s", path);
    if (channel) {
        a->fileChannels = (int)a->decoder.outputChannels;
        if (channel > a->fileChannels) {
            report("%s has only %d channel(s), so there is no channel %d", path, a->fileChannels, channel);
            audio_close(a);
            return NULL;
        }
        a->fileFrames = malloc((size_t)a->fileChannels * AUDIO_MAX_READ * sizeof *a->fileFrames);
        if (!a->fileFrames) { audio_close(a); return NULL; }
    }
    return a;
}

audio *audio_open_stdin(const audio_raw *raw, int channel)
{
    audio *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    a->kind = SRC_STDIN;
    a->channel = channel;
    a->raw = *raw;
    a->frameBytes = raw->channels * (raw->format == AUDIO_F32 ? 4 : 2);
    strcpy(a->name, "stdin");
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    if (raw->rate != RATE) {
        ma_resampler_config cfg = ma_resampler_config_init(ma_format_f32, 1, (ma_uint32)raw->rate, RATE,
                                                           ma_resample_algorithm_linear);
        ma_result r;
        cfg.linear.lpfOrder = FILTER_ORDER;
        r = ma_resampler_init(&cfg, NULL, &a->resampler);
        if (r != MA_SUCCESS) { complain("cannot resample stdin", r); free(a); return NULL; }
        a->resampling = 1;
    }
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

static int read_file(audio *a, int16_t *samples, int n)
{
    ma_uint64 got = 0;
    if (a->channel) {
        ma_uint64 i;
        ma_decoder_read_pcm_frames(&a->decoder, a->fileFrames, (ma_uint64)n, &got);
        for (i = 0; i < got; i++) samples[i] = a->fileFrames[i * (ma_uint64)a->fileChannels + (ma_uint64)a->channel - 1];
    } else {
        ma_decoder_read_pcm_frames(&a->decoder, samples, (ma_uint64)n, &got);
    }
    if (got == 0) return AUDIO_END;
    if (got < (ma_uint64)n) memset(samples + got, 0, ((size_t)n - (size_t)got) * sizeof *samples);
    return AUDIO_OK;
}

/* Whether a read of stdin would not block, so that a stream gone quiet cannot hold up keys and signals. */
static int stdin_ready(void)
{
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD available;
    if (GetFileType(h) != FILE_TYPE_PIPE) return 1;
    if (!PeekNamedPipe(h, NULL, 0, NULL, &available, NULL)) return 1;     /* the writer has gone: the read sees the end */
    return available > 0;
#else
    struct pollfd p;
    p.fd = 0;
    p.events = POLLIN;
    return poll(&p, 1, 0) != 0;                                           /* an error is for the read to report */
#endif
}

static long read_stdin(void *to, size_t n)
{
#ifdef _WIN32
    return _read(0, to, (unsigned)n);
#else
    return (long)read(0, to, n);
#endif
}

static float raw_sample(const audio *a, const unsigned char *p)
{
    if (a->raw.format == AUDIO_F32) {
        uint32_t bits = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
        float v;
        memcpy(&v, &bits, sizeof v);
        return isfinite(v) ? v : 0.0f;
    }
    return (int16_t)((unsigned)p[0] | (unsigned)p[1] << 8) / 32768.0f;
}

static void fifo_put(audio *a, const float *in, int n)
{
    int i;
    for (i = 0; i < n && a->fifoLen < STREAM_FIFO; i++) {
        float v = in[i] * 32768.0f;
        a->fifo[a->fifoLen++] = (int16_t)(v >= 32767.0f ? 32767 : v <= -32768.0f ? -32768 : lrintf(v));
    }
}

/* Turns the complete frames among the pending bytes into 8 kHz samples. */
static void convert_pending(audio *a)
{
    size_t frames = a->pendingBytes / (size_t)a->frameBytes, f;
    int width = a->raw.format == AUDIO_F32 ? 4 : 2, c;
    for (f = 0; f < frames; f++) {
        const unsigned char *p = a->pending + f * (size_t)a->frameBytes;
        if (a->channel) {
            a->mono[f] = raw_sample(a, p + (a->channel - 1) * width);
        } else {
            float sum = 0;
            for (c = 0; c < a->raw.channels; c++) sum += raw_sample(a, p + c * width);
            a->mono[f] = sum / (float)a->raw.channels;
        }
    }
    a->pendingBytes -= frames * (size_t)a->frameBytes;
    memmove(a->pending, a->pending + frames * (size_t)a->frameBytes, a->pendingBytes);
    if (a->resampling) {
        size_t used = 0;
        while (used < frames) {
            ma_uint64 in = frames - used, made = 2 * STREAM_SAMPLES;
            if (ma_resampler_process_pcm_frames(&a->resampler, a->mono + used, &in, a->resampled, &made) != MA_SUCCESS) break;
            if (in == 0 && made == 0) break;
            used += (size_t)in;
            fifo_put(a, a->resampled, (int)made);
        }
    } else {
        fifo_put(a, a->mono, (int)frames);
    }
}

static int read_stream(audio *a, int16_t *samples, int n)
{
    int have;
    while (a->fifoLen < n && !a->ended) {
        long got;
        if (!stdin_ready()) return AUDIO_WAIT;
        got = read_stdin(a->pending + a->pendingBytes, sizeof a->pending - a->pendingBytes);
        if (got < 0) {
            if (errno == EINTR || errno == EAGAIN) return AUDIO_WAIT;
            report("cannot read stdin: %s", strerror(errno));
            a->readFailed = 1;
            a->ended = 1;
        } else if (got == 0) {
            a->ended = 1;                    /* a sample cut short by the end of the stream is left out */
        } else {
            a->pendingBytes += (size_t)got;
            convert_pending(a);
        }
    }
    if (a->fifoLen == 0) return AUDIO_END;
    have = a->fifoLen < n ? a->fifoLen : n;
    memcpy(samples, a->fifo, (size_t)have * sizeof *samples);
    if (have < n) memset(samples + have, 0, (size_t)(n - have) * sizeof *samples);
    a->fifoLen -= have;
    memmove(a->fifo, a->fifo + have, (size_t)a->fifoLen * sizeof *a->fifo);
    return AUDIO_OK;
}

int audio_read(audio *a, int16_t *samples, int n)
{
    if (a->kind == SRC_DEVICE) return read_device(a, samples, n);
    if (a->kind == SRC_FILE) return read_file(a, samples, n);
    return read_stream(a, samples, n);
}

const char *audio_name(const audio *a) { return a->name; }
int  audio_is_device(const audio *a) { return a->kind == SRC_DEVICE; }
int  audio_is_stream(const audio *a) { return a->kind == SRC_STDIN; }
int  audio_failed(const audio *a) { return a->readFailed || (a->kind == SRC_DEVICE && a->stopped); }
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
        free(a->fileFrames);
    } else if (a->resampling) {
        ma_resampler_uninit(&a->resampler, NULL);
    }
    free(a);
}

static void rec_failed(audio_rec *r)
{
    if (!r->error) r->error = errno ? errno : EIO;
}

static ma_result rec_on_write(ma_encoder *e, const void *data, size_t n, size_t *written)
{
    audio_rec *r = e->pUserData;
    *written = fwrite(data, 1, n, r->file);
    if (*written == n) return MA_SUCCESS;
    rec_failed(r);
    return MA_ERROR;
}

static ma_result rec_on_seek(ma_encoder *e, ma_int64 offset, ma_seek_origin origin)
{
    audio_rec *r = e->pUserData;
    int whence = origin == ma_seek_origin_start ? SEEK_SET : origin == ma_seek_origin_end ? SEEK_END : SEEK_CUR;
    if (fseek(r->file, (long)offset, whence) == 0) return MA_SUCCESS;
    rec_failed(r);
    return MA_ERROR;
}

audio_rec *audio_rec_open(const char *path)
{
    audio_rec *r = calloc(1, sizeof *r);
    ma_encoder_config cfg = ma_encoder_config_init(ma_encoding_format_wav, ma_format_s16, 1, RATE);
    ma_result e;
    if (!r) return NULL;
    r->path = path;
    r->file = fopen(path, "wb");
    if (!r->file) {
        report("cannot write %s: %s", path, strerror(errno));
        free(r);
        return NULL;
    }
    e = ma_encoder_init(rec_on_write, rec_on_seek, r, &cfg, &r->encoder);
    if (e != MA_SUCCESS) {
        report("cannot write %s: %s", path, ma_result_description(e));
        fclose(r->file);
        free(r);
        return NULL;
    }
    return r;
}

int audio_rec_write(audio_rec *r, const int16_t *samples, int n)
{
    ma_encoder_write_pcm_frames(&r->encoder, samples, (ma_uint64)n, NULL);
    return !r->error;
}

int audio_rec_close(audio_rec *r)
{
    int ok;
    if (!r) return 1;
    ma_encoder_uninit(&r->encoder);          /* this writes the length into the header */
    if (fclose(r->file) == EOF) rec_failed(r);
    ok = !r->error;
    if (!ok) report("cannot write %s: %s", r->path, strerror(r->error));
    free(r);
    return ok;
}
