/* cwgen — synthesise Morse test audio (8 kHz, 16-bit mono WAV) for exercising vail.
 *
 *   cwgen -o out.wav [-t TEXT] [-w WPM] [-f HZ] [-n SNR_DB] [-j JITTER] [-r SEED]
 *         [-l LEAD_S] [-F FADE_HZ] [-D FADE_DEPTH] [-d DRIFT_HZ] [-S HZ:WPM:REL_DB:TEXT]...
 *
 *   -n   SNR in dB, signal power over noise power in a 2500 Hz bandwidth (default: no noise)
 *   -j   element-length jitter, standard deviation as a fraction of the nominal length
 *   -F/-D  sinusoidal fading (QSB) rate and depth (0..1)
 *   -d   linear pitch drift over the whole file
 *   -Q   impulse noise (QRN): mean number of clicks per second, each a decaying burst at 3x signal level
 *   -S   extra signal (up to 3), level relative to the main one
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

static void build_keying(Keying *k, const SigDef *d, double lead, double jitter)
{
    double u = 1.2 / d->wpm, t = lead;
    const char *p;
    memset(k, 0, sizeof *k);
    for (p = d->text; *p; p++) {
        const char *m;
        if (*p == ' ') { t += 4 * u * (1 + jitter * grand()); continue; }   /* 3 already added + 4 = 7 */
        m = morse(*p);
        if (!m) continue;
        for (; *m; m++) {
            double len = (*m == '.' ? 1 : 3) * u * (1 + jitter * grand());
            if (len < 0.3 * u) len = 0.3 * u;
            key_add(k, t, t + len);
            t += len;
            t += u * (1 + jitter * grand());
        }
        t += 2 * u * (1 + jitter * grand());
    }
    k->end = t;
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
    const char *out = NULL;
    double snr = 1e9, jitter = 0, lead = 3.0, fadeHz = 0, fadeDepth = 0, drift = 0, end = 0, amp = 0.3, qrn = 0, burst = 0;
    long n, s;
    int16_t *pcm;
    double phase[MAXSIG] = { 0 };

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
        build_keying(&key[i], &sig[i], lead + 0.37 * i, jitter);
        if (key[i].end > end) end = key[i].end;
    }
    end += 1.0;
    n = (long)(end * RATE);
    pcm = malloc(n * sizeof(int16_t));
    {
        /* noise sigma: SNR = (amp^2/2) / (sigma^2 * 2500/4000) */
        double sigma = (snr > 500) ? 0 : sqrt((amp * amp / 2) / pow(10, snr / 10) / (2500.0 / 4000.0));
        for (s = 0; s < n; s++) {
            double t = (double)s / RATE, y = sigma * grand();
            if (qrn > 0 && urand() < qrn / RATE) burst = 3 * amp;
            if (burst > 1e-4) { y += burst * grand(); burst *= 0.995; }
            for (i = 0; i < nsig; i++) {
                double hz = sig[i].hz + drift * t / end;
                double g = amp * pow(10, sig[i].rel / 20) * envelope(&key[i], t, &idx[i]);
                if (fadeHz > 0) g *= 1 - fadeDepth * (0.5 + 0.5 * sin(2 * M_PI * fadeHz * t + i));
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
    return 0;
}
