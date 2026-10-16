/* FE — spectrum front end (02-frontend-spectrum.md). */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "vail_internal.h"

/* 1024-point real DFT, non-normalised, written back as interleaved re/im (02 §4.3).
 * Implemented as an in-place radix-2 complex FFT of 512 points plus the usual split step. */
static void real_fft_inplace(float *x, int n)
{
    static int ready = 0;
    static double cosT[FE_FFT_SIZE / 2], sinT[FE_FFT_SIZE / 2];
    int m = n / 2, i, j, k, len;
    double re[FE_FFT_SIZE / 2], im[FE_FFT_SIZE / 2];

    if (!ready) {
        for (i = 0; i < FE_FFT_SIZE / 2; i++) {
            cosT[i] = cos(FE_TWO_PI * i / FE_FFT_SIZE);
            sinT[i] = sin(FE_TWO_PI * i / FE_FFT_SIZE);
        }
        ready = 1;
    }
    for (i = 0; i < m; i++) { re[i] = x[2 * i]; im[i] = x[2 * i + 1]; }
    for (i = 1, j = 0; i < m; i++) {                 /* bit reversal */
        int bit = m >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            double t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (len = 2; len <= m; len <<= 1) {
        int step = n / len;                          /* twiddle index step in the n-point table */
        for (i = 0; i < m; i += len) {
            for (k = 0; k < len / 2; k++) {
                double wr = cosT[k * step], wi = -sinT[k * step];
                int a = i + k, b = i + k + len / 2;
                double tr = re[b] * wr - im[b] * wi, ti = re[b] * wi + im[b] * wr;
                re[b] = re[a] - tr; im[b] = im[a] - ti;
                re[a] += tr; im[a] += ti;
            }
        }
    }
    /* split: X[k] = (Z[k] + conj Z[m-k])/2 - i e^{-2πik/n} (Z[k] - conj Z[m-k])/2 */
    x[0] = (float)(re[0] + im[0]);
    x[1] = (float)(re[0] - im[0]);                   /* folded bin Re(512), outside the retained range */
    for (k = 1; k < m; k++) {
        double ar = re[k], ai = im[k], br = re[m - k], bi = -im[m - k];
        double er = 0.5 * (ar + br), ei = 0.5 * (ai + bi);
        double dr = 0.5 * (ar - br), di = 0.5 * (ai - bi);
        double wr = cosT[k], wi = -sinT[k];
        double tr = dr * wr - di * wi, ti = dr * wi + di * wr;   /* d * w */
        /* X = e - i*(d*w) */
        x[2 * k] = (float)(er + ti);
        x[2 * k + 1] = (float)(ei - tr);
    }
}

vail_fe *vail_fe_new(void)
{
    vail_fe *fe = calloc(1, sizeof *fe);
    int i;
    if (!fe) return NULL;
    fe->window[FE_WINDOW_CENTER] = 1.0f;             /* 02 §4.1 */
    fe->window[0] = 0.0f;
    for (i = 1; i < FE_FFT_SIZE / 2; i++) {
        double x = FE_TWO_PI * i / FE_FFT_SIZE;
        double w = 0.42 - 0.5 * cos(x) + 0.08 * cos(2 * x);
        double d = FE_TWO_PI * FE_WINDOW_CUTOFF * (i - FE_WINDOW_CENTER);
        double s = (d == 0.0) ? 1.0 : sin(d) / d;
        fe->window[i] = (float)(w * s);
        fe->window[FE_FFT_SIZE - i] = (float)(w * s);
    }
    return fe;
}

void vail_fe_free(vail_fe *fe) { free(fe); }

const float *vail_fe_process(vail_fe *fe, const int16_t *samples)
{
    int n, j;
    memmove(fe->frameBuf, fe->frameBuf + FE_HOP_SAMPLES,
            (FE_FFT_SIZE - FE_HOP_SAMPLES) * sizeof(float));
    for (n = 0; n < FE_HOP_SAMPLES; n++)
        fe->frameBuf[FE_FFT_SIZE - FE_HOP_SAMPLES + n] = (float)(samples[n] * FE_SAMPLE_SCALE);
    for (n = 0; n < FE_FFT_SIZE; n++)
        fe->work[n] = fe->frameBuf[n] * fe->window[n];
    real_fft_inplace(fe->work, FE_FFT_SIZE);
    for (j = 0; j < FE_N_BINS; j++) {
        int k = FE_BIN_FIRST + j;
        double re = fe->work[2 * k], im = fe->work[2 * k + 1];
        double p = (re * re + im * im) * FE_POWER_SCALE;
        fe->out[j] = isfinite(p) ? (float)p : 0.0f;
    }
    return fe->out;
}
