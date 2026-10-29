/* SPDX-License-Identifier: GPL-3.0-or-later */
/* cwgen — synthesise Morse test audio (8 kHz, 16-bit mono WAV) for exercising vail.
 *
 *   cwgen -o out.wav [-t TEXT] [-w WPM] [-f HZ] [-n SNR_DB] [-j JITTER] [-r SEED]
 *         [-l LEAD_S] [-F FADE_HZ] [-D FADE_DEPTH] [-R DOPPLER_HZ] [-d DRIFT_HZ] [-B LO:HI]
 *         [-W DASH_RATIO] [-G GAP_STRETCH] [-S HZ:WPM:REL_DB:TEXT]...
 *
 *   -n   SNR in dB, signal power over noise power in a 2500 Hz bandwidth (default: no noise).
 *        With noise the noise level is fixed (-20 dBFS) and the signal is scaled to the SNR, so
 *        weak-signal files do not clip.
 *   -j   element-length jitter, standard deviation as a fraction of the nominal length
 *   -F/-D  sinusoidal fading (QSB) rate and depth (0..1)
 *   -R   Rayleigh fading (ionospheric QSB) with this Doppler spread; mean signal power is kept
 *   -d   linear pitch drift over the whole file
 *   -B   receiver filter: the noise passes a 4th-order band-pass from LO to HI Hz
 *   -W   dash length in dots (default 3; hand senders often use 2.5..4)
 *   -G   letter and word gaps stretched by this factor (default 1; > 1 is Farnsworth-like spacing)
 *   -Q   impulse noise (QRN): mean number of clicks per second, each a decaying burst at 3x signal level
 *   -S   extra signal (up to 3), level relative to the main one
 *   -K F write the key-down intervals of the main signal to file F ("start end" in seconds)
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wav.h"

#define RATE 8000
#define MAXSIG 4

static const char *morse(int c)
{
    static const char *letters[26] = { ".-", "-...", "-.-.", "-..", ".", "..-.", "--.", "....", "..", ".---",
        "-.-", ".-..", "--", "-.", "---", ".--.", "--.-", ".-.", "...", "-", "..-", "...-", ".--", "-..-", "-.--", "--.." };
    static const char *digits[10] = { "-----", ".----", "..---", "...--", "....-", ".....", "-....", "--...", "---..", "----." };
    if (c >= 'a' && c <= 'z') c -= 32;
    if (c >= 'A' && c <= 'Z') return letters[c - 'A'];
    if (c >= '0' && c <= '9') return digits[c - '0'];
    switch (c) {
    case '/': return "-..-.";
    case '?': return "..--..";
    case '=': return "-...-";
    case '.': return ".-.-.-";
    case ',': return "--..--";
    case '+': return ".-.-.";
    }
    return NULL;
}

static unsigned long long rng = 88172645463325252ULL;
static double urand(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (rng >> 11) * (1.0 / 9007199254740992.0); }
static double grand(void) { double u = urand() + 1e-300, v = urand(); return sqrt(-2 * log(u)) * cos(2 * M_PI * v); }

typedef struct { double hz, wpm, rel; const char *text; } SigDef;

/* key-down intervals in seconds for one signal */
typedef struct { double *on, *off; int n, cap; double end; } Keying;

static void key_add(Keying *k, double t0, double t1)
{
    if (k->n == k->cap) {
        k->cap = k->cap ? k->cap * 2 : 256;
        k->on = realloc(k->on, k->cap * sizeof(double));
        k->off = realloc(k->off, k->cap * sizeof(double));
    }
    k->on[k->n] = t0; k->off[k->n] = t1; k->n++;
}

static void build_keying(Keying *k, const SigDef *d, double lead, double jitter, double dash, double stretch)
{
    double u = 1.2 / d->wpm, t = lead;
    const char *p;
    memset(k, 0, sizeof *k);
    for (p = d->text; *p; p++) {
        const char *m;
        if (*p == ' ') { t += 4 * stretch * u * (1 + jitter * grand()); continue; }   /* 3 already added + 4 = 7 */
        m = morse(*p);
        if (!m) continue;
        for (; *m; m++) {
            double len = (*m == '.' ? 1 : dash) * u * (1 + jitter * grand());
            if (len < 0.3 * u) len = 0.3 * u;
            key_add(k, t, t + len);
            t += len;
            t += u * (1 + jitter * grand());
        }
        t += (3 * stretch - 1) * u * (1 + jitter * grand());
    }
    k->end = t;
}

/* 4th-order Butterworth high-pass and low-pass (two biquads each, RBJ cookbook), flat in the pass band */
typedef struct { double b0, b1, b2, a1, a2, x1, x2, y1, y2; } Biquad;

static void biquad_init(Biquad *q, double hz, int high, double Q)
{
    double w = 2 * M_PI * hz / RATE, c = cos(w), al = sin(w) / (2 * Q), a0 = 1 + al;
    memset(q, 0, sizeof *q);
    q->b0 = (high ? (1 + c) : (1 - c)) / 2 / a0;
    q->b1 = (high ? -(1 + c) : (1 - c)) / a0;
    q->b2 = q->b0;
    q->a1 = -2 * c / a0;
    q->a2 = (1 - al) / a0;
}

static double biquad_run(Biquad *q, double x)
{
    double y = q->b0 * x + q->b1 * q->x1 + q->b2 * q->x2 - q->a1 * q->y1 - q->a2 * q->y2;
    q->x2 = q->x1; q->x1 = x; q->y2 = q->y1; q->y1 = y;
    return y;
}

/* Rayleigh fading gain per sample: complex Gaussian low-passed to the Doppler spread (two one-pole
 * stages at 100 Hz, linearly interpolated), normalised to unit mean power over the file */
static double *rayleigh_gain(long n, double doppler)
{
    long blocks = n / 80 + 2, b, s;
    double *ri = malloc(blocks * sizeof(double)), *rq = malloc(blocks * sizeof(double)), *g = malloc(n * sizeof(double));
    double a = exp(-2 * M_PI * doppler / 100.0), i1 = grand(), i2 = i1, q1 = grand(), q2 = q1, pw = 0;
    for (b = -200; b < blocks; b++) {                  /* 2 s of settling */
        i1 = a * i1 + sqrt(1 - a * a) * grand(); i2 = a * i2 + sqrt(1 - a * a) * i1;
        q1 = a * q1 + sqrt(1 - a * a) * grand(); q2 = a * q2 + sqrt(1 - a * a) * q1;
        if (b >= 0) { ri[b] = i2; rq[b] = q2; }
    }
    for (s = 0; s < n; s++) {
        double f = (s % 80) / 80.0;
        long k = s / 80;
        double i = ri[k] + (ri[k + 1] - ri[k]) * f, q = rq[k] + (rq[k + 1] - rq[k]) * f;
        g[s] = sqrt(i * i + q * q);
        pw += g[s] * g[s];
    }
    pw = sqrt(pw / n);
    for (s = 0; s < n; s++) g[s] /= pw;
    free(ri); free(rq);
    return g;
}

/* raised-cosine keyed envelope, 5 ms edges */
static double envelope(const Keying *k, double t, int *idx)
{
    const double r = 0.005;
    while (*idx < k->n && k->off[*idx] + r < t) (*idx)++;
    if (*idx >= k->n) return 0;
    {
        double a = k->on[*idx], b = k->off[*idx];
        if (t < a || t > b + r) return 0;
        if (t < a + r) return 0.5 - 0.5 * cos(M_PI * (t - a) / r);
        if (t <= b) return 1;
        return 0.5 + 0.5 * cos(M_PI * (t - b) / r);
    }
}

int main(int argc, char **argv)
{
    SigDef sig[MAXSIG];
    Keying key[MAXSIG];
    int nsig = 1, i, idx[MAXSIG] = { 0 };
    const char *out = NULL, *keyOut = NULL;
    double snr = 1e9, jitter = 0, lead = 3.0, fadeHz = 0, fadeDepth = 0, drift = 0, end = 0, amp = 0.3, qrn = 0, burst = 0;
    double doppler = 0, bandLo = 0, bandHi = 0, dash = 3, stretch = 1, *ray = NULL;
    long n, s;
    int16_t *pcm;
    double phase[MAXSIG] = { 0 };
    Biquad bq[4];

    sig[0].hz = 700; sig[0].wpm = 20; sig[0].rel = 0; sig[0].text = "CQ CQ DE W1AW W1AW K";
    for (i = 1; i + 1 < argc; i += 2) {
        const char *a = argv[i], *v = argv[i + 1];
        if (!strcmp(a, "-o")) out = v;
        else if (!strcmp(a, "-t")) sig[0].text = v;
        else if (!strcmp(a, "-w")) sig[0].wpm = atof(v);
        else if (!strcmp(a, "-f")) sig[0].hz = atof(v);
        else if (!strcmp(a, "-n")) snr = atof(v);
        else if (!strcmp(a, "-j")) jitter = atof(v);
        else if (!strcmp(a, "-r")) rng = strtoull(v, NULL, 10) * 2654435761ULL + 1;
        else if (!strcmp(a, "-l")) lead = atof(v);
        else if (!strcmp(a, "-F")) fadeHz = atof(v);
        else if (!strcmp(a, "-D")) fadeDepth = atof(v);
        else if (!strcmp(a, "-d")) drift = atof(v);
        else if (!strcmp(a, "-Q")) qrn = atof(v);
        else if (!strcmp(a, "-R")) doppler = atof(v);
        else if (!strcmp(a, "-K")) keyOut = v;
        else if (!strcmp(a, "-W")) dash = atof(v);
        else if (!strcmp(a, "-G")) stretch = atof(v);
        else if (!strcmp(a, "-B")) {
            const char *c = strchr(v, ':');
            if (!c) { fprintf(stderr, "bad -B\n"); return 2; }
            bandLo = atof(v); bandHi = atof(c + 1);
        }
        else if (!strcmp(a, "-S") && nsig < MAXSIG) {
            char *c = strdup(v), *t1 = strchr(c, ':'), *t2, *t3;
            if (!t1 || !(t2 = strchr(t1 + 1, ':')) || !(t3 = strchr(t2 + 1, ':'))) { fprintf(stderr, "bad -S\n"); return 2; }
            *t1 = *t2 = *t3 = 0;
            sig[nsig].hz = atof(c); sig[nsig].wpm = atof(t1 + 1); sig[nsig].rel = atof(t2 + 1); sig[nsig].text = t3 + 1;
            nsig++;
        } else { fprintf(stderr, "unknown option %s\n", a); return 2; }
    }
    if (!out) { fprintf(stderr, "usage: cwgen -o out.wav [options]\n"); return 2; }
    for (i = 0; i < nsig; i++) {
        build_keying(&key[i], &sig[i], lead + 0.37 * i, jitter, dash, stretch);
        if (key[i].end > end) end = key[i].end;
    }
    end += 1.0;
    n = (long)(end * RATE);
    pcm = malloc(n * sizeof(int16_t));
    if (doppler > 0) ray = rayleigh_gain(n, doppler);
    if (keyOut) {                                  /* third column: fading gain at the element centre */
        FILE *kf = fopen(keyOut, "w");
        if (kf) {
            for (i = 0; i < key[0].n; i++) {
                long c = (long)((key[0].on[i] + key[0].off[i]) / 2 * RATE);
                fprintf(kf, "%.5f %.5f %.3f\n", key[0].on[i], key[0].off[i], ray && c < n ? ray[c] : 1.0);
            }
            fclose(kf);
        }
    }
    if (bandHi > bandLo) {
        biquad_init(&bq[0], bandLo, 1, 0.5411961); biquad_init(&bq[1], bandLo, 1, 1.3065630);
        biquad_init(&bq[2], bandHi, 0, 0.5411961); biquad_init(&bq[3], bandHi, 0, 1.3065630);
    }
    {
        /* SNR = (amp^2/2) / (sigma^2 * 2500/4000): the noise sits at -20 dBFS and the signal follows
         * the SNR, both scaled down together if the signal would exceed half of full scale */
        double sigma = 0;
        if (snr <= 500) {
            sigma = 0.1;
            amp = sqrt(2 * pow(10, snr / 10) * sigma * sigma * (2500.0 / 4000.0));
            if (amp > 0.5) { sigma *= 0.5 / amp; amp = 0.5; }
        }
        for (s = 0; s < n; s++) {
            double t = (double)s / RATE, y = sigma * grand();
            if (bandHi > bandLo) for (i = 0; i < 4; i++) y = biquad_run(&bq[i], y);
            if (qrn > 0 && urand() < qrn / RATE) burst = 3 * amp;
            if (burst > 1e-4) { y += burst * grand(); burst *= 0.995; }
            for (i = 0; i < nsig; i++) {
                double hz = sig[i].hz + drift * t / end;
                double g = amp * pow(10, sig[i].rel / 20) * envelope(&key[i], t, &idx[i]);
                if (fadeHz > 0) g *= 1 - fadeDepth * (0.5 + 0.5 * sin(2 * M_PI * fadeHz * t + i));
                if (ray && i == 0) g *= ray[s];
                phase[i] += 2 * M_PI * hz / RATE;
                if (phase[i] > 2 * M_PI) phase[i] -= 2 * M_PI;
                y += g * sin(phase[i]);
            }
            y *= 32767;
            if (y > 32767) y = 32767;
            if (y < -32768) y = -32768;
            pcm[s] = (int16_t)lrint(y);
        }
    }
    if (wav_write(out, pcm, n, RATE)) { fprintf(stderr, "cannot write %s\n", out); return 1; }
    free(pcm);
    free(ray);
    return 0;
}
