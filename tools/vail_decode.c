/* vail_decode — decode Morse from a WAV file (or raw s16le @ 8 kHz) with the vail core.
 *
 *   vail_decode [-a] [-s] [-v] [-p HZ] file.wav
 *     -a     print the text of every channel that produced any (with its pitch), including channels
 *            retired before the end of the file
 *     -s     stream the host character list (API_CharAt) as it is produced
 *     -v     print state / pitch / SNR / WPM every API_POLL_FRAMES (15) frames to stderr
 *     -p HZ  lock the pitch at HZ from the start (API_PitchLock)
 *     -T F   write a per-frame trace of every channel pipeline to file F
 *     -E F   write every classified element of every channel to file F (id frame duration kind bin)
 *     -m     operating mode on: emulate a host that applies API_DeleteCount before appending,
 *            and print the resulting host text
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/vail.h"
#include "wav.h"

extern FILE *vail_trace, *vail_elems;

static int16_t *resample(const int16_t *in, long n, int rate, long *outN)
{
    long m = (long)((double)n * VAIL_SAMPLE_RATE / rate), i;
    int16_t *out = malloc((size_t)(m ? m : 1) * sizeof(int16_t));
    for (i = 0; i < m; i++) {
        double t = (double)i * rate / VAIL_SAMPLE_RATE;
        long k = (long)t;
        double f = t - k, a = in[k < n ? k : n - 1], b = in[k + 1 < n ? k + 1 : n - 1];
        out[i] = (int16_t)lrint(a + (b - a) * f);
    }
    *outN = m;
    return out;
}

int main(int argc, char **argv)
{
    int all = 0, stream = 0, verbose = 0, lockHz = 0, mode = 0, rate, i, frame = 0, hostLen = 0;
    static char host[1 << 16];
    const char *path = NULL;
    long n, pos;
    int16_t *s, tail[VAIL_HOP_SAMPLES];
    vail_fe *fe;
    vail_api *h;
    static char text[4096];
    enum { MAXCH = 256 };
    static struct { int id, hz; char *text; } seen[MAXCH];
    int nseen = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-a")) all = 1;
        else if (!strcmp(argv[i], "-s")) stream = 1;
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "-m")) mode = 1;
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) lockHz = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-T") && i + 1 < argc) vail_trace = fopen(argv[++i], "w");
        else if (!strcmp(argv[i], "-E") && i + 1 < argc) vail_elems = fopen(argv[++i], "w");
        else path = argv[i];
    }
    if (!path) { fprintf(stderr, "usage: %s [-a] [-s] [-v] [-p HZ] file.wav\n", argv[0]); return 2; }
    s = wav_read(path, &n, &rate);
    if (!s) { fprintf(stderr, "cannot read %s\n", path); return 1; }
    if (rate != VAIL_SAMPLE_RATE) {
        long m;
        int16_t *r = resample(s, n, rate, &m);
        free(s); s = r; n = m;
    }
    fe = vail_fe_new();
    h = API_New();
    if (lockHz > 0) API_PitchLock(h, lockHz);
    if (mode) API_ModeSet(h, 1);
    memset(tail, 0, sizeof tail);

    /* the file, then 4 s of silence so that the carrier drops and the tail is flushed */
    for (pos = 0; pos < n + 4 * VAIL_SAMPLE_RATE; pos += VAIL_HOP_SAMPLES) {
        const int16_t *blk;
        float spec[VAIL_N_BINS];
        int16_t tmp[VAIL_HOP_SAMPLES];
        if (pos + VAIL_HOP_SAMPLES <= n) blk = s + pos;
        else {
            long k;
            for (k = 0; k < VAIL_HOP_SAMPLES; k++) tmp[k] = (pos + k < n) ? s[pos + k] : 0;
            blk = tmp;
        }
        memcpy(spec, vail_fe_process(fe, blk), sizeof spec);
        API_Feed(h, spec);
        frame++;
        if (mode) {
            int c, cnt = API_CharCount(h), d = API_DeleteCount(h);
            hostLen = d > hostLen ? 0 : hostLen - d;
            for (c = 0; c < cnt && hostLen < (int)sizeof host - 1; c++) host[hostLen++] = (char)API_CharAt(h, c).ch;
        }
        if (all && frame % 32 == 0) {           /* remember every channel's latest text */
            int c, k, nc = vail_channel_count_all(h);
            for (c = 0; c < nc; c++) {
                int hz, id, len = vail_channel_text(h, c, text, sizeof text, &hz, &id);
                if (len <= 0) continue;
                for (k = 0; k < nseen && seen[k].id != id; k++) {}
                if (k == nseen) { if (nseen == MAXCH) continue; seen[nseen].id = id; seen[nseen].text = NULL; nseen++; }
                seen[k].hz = hz;
                free(seen[k].text);
                seen[k].text = strdup(text);
            }
        }
        if (stream) {
            int c, cnt = API_CharCount(h);
            for (c = 0; c < cnt; c++) putchar(API_CharAt(h, c).ch);
            if (cnt) fflush(stdout);
        }
        if (verbose && frame % 15 == 0)
            fprintf(stderr, "t=%7.2fs state=%d pitch=%d snr=%d wpm=%d ch=%d\n", frame * 0.010625,
                    API_State(h), API_Pitch(h), API_Snr(h), API_Wpm(h), API_ChannelCount(h));
    }
    if (stream) putchar('\n');
    if (mode) { host[hostLen] = 0; printf("%s\n", host); }
    if (all) {
        int c;
        for (c = 0; c < nseen; c++) {
            printf("%5d Hz: %s\n", seen[c].hz, seen[c].text);
            free(seen[c].text);
        }
    } else if (!stream && !mode) {
        vail_current_text(h, text, sizeof text);
        printf("%s\n", text);
    }
    API_Free(h);
    vail_fe_free(fe);
    free(s);
    return 0;
}
