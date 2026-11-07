/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SPD — speed bank: fixed-speed duration HMMs scored on SEG's input (see vail_internal.h). The forward
 * (sum over paths) likelihood is used: a best-path score lets every model explain weak marks away as
 * one long pause, which the slowest model does most cheaply. */
#include <math.h>
#include <string.h>
#include "vail_internal.h"

static const float SPD_UNITS[5] = { 1.0f, 3.0f, 1.0f, 3.0f, 7.0f };   /* dot dash gap lgap wgap */

#define NULL_CAP     400        /* durations (frames) over which a null model is built, as SEG's tables */
#define NULL_HAZ_MIN 0.002
#define NULL_HAZ_MAX 0.999

/* pmf over 1..NULL_CAP of one side of a hazard table: states 1..n, the last absorbing */
static void side_pmf(const float *h, int n, double *q)
{
    double surv = 1.0;
    int i;
    for (i = 1; i <= NULL_CAP; i++) {
        double hi = h[i < n ? i : n];
        q[i] = surv * hi;
        surv *= 1.0 - hi;
    }
}

/* hazards of an unnormalised pmf over 1..NULL_CAP for states 1..n, the last one absorbing with the hazard of a geometric
 * tail of the same mean residual length, as SEG builds its tables (SEG_TUNE_NORM_DUR) */
static void side_hazards(const double *q, int n, float *h)
{
    double z = 0.0, surv = 1.0, tail = 0.0, mrl = 0.0, x;
    int i;
    for (i = 1; i <= NULL_CAP; i++) z += q[i];
    for (i = 1; i < n; i++) {
        x = surv > 1e-12 ? q[i] / z / surv : NULL_HAZ_MAX;
        h[i] = (float)(x < NULL_HAZ_MIN ? NULL_HAZ_MIN : (x > NULL_HAZ_MAX ? NULL_HAZ_MAX : x));
        surv -= q[i] / z;
    }
    for (i = n; i <= NULL_CAP; i++) { tail += q[i] / z; mrl += (i - n + 1) * q[i] / z; }
    x = (tail > 1e-12 && mrl > 0.0) ? tail / mrl : NULL_HAZ_MAX;
    h[n] = (float)(x < NULL_HAZ_MIN ? NULL_HAZ_MIN : (x > NULL_HAZ_MAX ? NULL_HAZ_MAX : x));
}

/* one side of a null model: a lognormal duration with the mean and variance of log duration of the model's side */
static void null_side(const float *h, int n, float *nh)
{
    double q[NULL_CAP + 1], m = 0.0, v = 0.0, z = 0.0;
    int i;
    side_pmf(h, n, q);
    for (i = 1; i <= NULL_CAP; i++) {
        double x = log((double)i);
        z += q[i];
        m += q[i] * x;
        v += q[i] * x * x;
    }
    m /= z;
    v = v / z - m * m;
    if (v < 1e-3) v = 1e-3;
    for (i = 1; i <= NULL_CAP; i++) {
        double d = log((double)i) - m;
        q[i] = exp(-d * d / (2.0 * v)) / i;
    }
    side_hazards(q, n, nh);
}

/* the null model of a model's hazard table: marks are states 1..SEG_ON_MAX, gaps the SEG_OFF_MAX after them */
static void null_from(float *nh, const float *h)
{
    null_side(h, SEG_ON_MAX, nh);
    null_side(h + SEG_ON_MAX, SEG_OFF_MAX, nh + SEG_ON_MAX);
}

/* the null model of random bits of a period (frames): each side at least SPD_GEO_FLOOR periods, then a constant hazard
 * that gives each further period even odds */
static void geo_null(float *nh, double bit)
{
    int floor = (int)(SPD_GEO_FLOOR * bit + 0.5), i;
    float h = (float)(1.0 - pow(0.5, 1.0 / bit));
    if (floor < 1) floor = 1;
    for (i = 1; i <= SEG_ON_MAX + SEG_OFF_MAX; i++) {
        int d = i <= SEG_ON_MAX ? i : i - SEG_ON_MAX;
        nh[i] = d < floor ? (float)NULL_HAZ_MIN : h;
    }
}

static void means_at(double wpm, float *means)
{
    double u = SPD_UNIT_NUM / wpm;
    int c;
    for (c = 0; c < 5; c++) means[c] = (float)log(u * SPD_UNITS[c]);
}

void spd_means(const Spd *p, int k, float *means) { means_at(p->wpm[k], means); }

/* hazards of the standard timing at a speed, with STA's initial spreads, each widened by a measurement error of frameSd
 * frames */
static void standard_hazards(double wpm, float *hz, double frameSd)
{
    DurState d[SEG_ON_MAX + SEG_OFF_MAX + 1];
    Sta tmp;
    int i;
    sta_init(&tmp);
    means_at(wpm, tmp.means);
    for (i = 0; i < 5; i++) {
        double r = frameSd / exp(tmp.means[i]);
        tmp.variances[i] += (float)(r * r);
    }
    seg_tables_from(d, &tmp);
    for (i = 1; i <= SEG_ON_MAX + SEG_OFF_MAX; i++) hz[i] = expf(d[i].logEnd);
    sta_free(&tmp);
}

/* The fixed-speed models' hazards and the null models' are the same in every channel, so they are built once, as STA's
 * split prior is (the first spd_init must not run alongside another), and every channel reads them. */
static float bankWpm[SPD_N];
static float bankHz[SPD_N][SEG_ON_MAX + SEG_OFF_MAX + 1];
static float nullHz[SPD_NULLS][SEG_ON_MAX + SEG_OFF_MAX + 1];
static int tablesReady = 0;

static void build_tables(void)
{
    float hz[SEG_ON_MAX + SEG_OFF_MAX + 1];
    int k;
    for (k = 0; k < SPD_N; k++) {
        bankWpm[k] = (float)(SPD_WPM_MIN * pow(SPD_WPM_MAX / SPD_WPM_MIN, (double)k / (SPD_N - 1)));
        standard_hazards(bankWpm[k], bankHz[k], 0.0);
    }
    for (k = 0; SPD_TUNE_MORSE && k < SPD_NULL_N; k++) {
        standard_hazards(SPD_NULL_WPM_MIN * pow(SPD_NULL_WPM_MAX / SPD_NULL_WPM_MIN, (double)k / (SPD_NULL_N - 1)), hz,
                         SPD_FRAME_SD);
        null_from(nullHz[k], hz);
    }
    for (k = 0; SPD_TUNE_MORSE && k < SPD_GEO_N; k++) {
        double bit = SPD_GEO_MIN * pow(SPD_GEO_MAX / SPD_GEO_MIN, SPD_GEO_N > 1 ? (double)k / (SPD_GEO_N - 1) : 0.0);
        geo_null(nullHz[SPD_NULL_N + k], bit);
    }
    tablesReady = 1;
}

void spd_init(Spd *p)
{
    int k;
    if (!tablesReady) build_tables();
    memset(p, 0, sizeof *p);
    memcpy(p->wpm, bankWpm, sizeof p->wpm);
    for (k = 0; k <= SPD_CUR; k++) p->alpha[k][SEG_ON_MAX + SEG_OFF_MAX] = 1.0;           /* start in a long gap */
    p->stdAlpha[SEG_ON_MAX + SEG_OFF_MAX] = 1.0;
    for (k = 0; SPD_TUNE_MORSE && k < SPD_NULLS; k++) p->nullAlpha[k][SEG_ON_MAX + SEG_OFF_MAX] = 1.0;
}

void spd_set_current(Spd *p, const DurState *d, const Sta *sta)
{
    float wpm = sta_speed(sta);
    int i;
    for (i = 1; i <= SEG_ON_MAX + SEG_OFF_MAX; i++) p->curHz[i] = expf(d[i].logEnd);
    if (!SPD_TUNE_MORSE) return;
    if (wpm > SPD_STD_WPM_MAX) wpm = (float)SPD_STD_WPM_MAX;
    if (wpm < SPD_STD_WPM_MIN) wpm = (float)SPD_STD_WPM_MIN;
    if (p->stdWpm <= 0.0f || fabs(log(wpm / p->stdWpm)) > 0.01) {
        p->stdWpm = wpm;
        standard_hazards(wpm, p->stdHz, SPD_FRAME_SD);
    }
}

/* one forward step of SEG's duration HMM (states as in 05 §4.3); returns the normalising sum */
static double step(const float *h, double *a, double key, double space, double rise, double fall)
{
    const int on = SEG_ON_MAX, off = SEG_OFF_MAX;
    double endOn = 0.0, endOff = 0.0, sum = 0.0;
    int i;
    for (i = 1; i <= on; i++) endOn += a[i] * h[i];
    for (i = 1; i <= off; i++) endOff += a[on + i] * h[on + i];
    a[on] = (a[on] * (1.0 - h[on]) + a[on - 1] * (1.0 - h[on - 1])) * key;
    for (i = on - 1; i >= 2; i--) a[i] = a[i - 1] * (1.0 - h[i - 1]) * key;
    a[on + off] = (a[on + off] * (1.0 - h[on + off]) + a[on + off - 1] * (1.0 - h[on + off - 1])) * space;
    for (i = off - 1; i >= 2; i--) a[on + i] = a[on + i - 1] * (1.0 - h[on + i - 1]) * space;
    a[1] = endOff * rise;
    a[on + 1] = endOn * fall;
    for (i = 1; i <= on + off; i++) sum += a[i];
    if (sum > 0.0) for (i = 1; i <= on + off; i++) a[i] /= sum;
    return sum;
}

void spd_update(Spd *p, const SegObs *o)
{
    /* emissions relative to key-up, scaled by a common factor (it cancels between the models) */
    double lam = 1.0 - 1.0 / SPD_TAU, x = o->keyLogOdds, r = o->riseLogOdds, f = o->fallLogOdds;
    double m = x > 0.0 ? x : 0.0, key, space, rise, fall, saved[SPD_N];
    int k;
    if (SPD_TUNE_MORSE) memcpy(saved, p->morseLik, sizeof saved);
    if (r > m) m = r;
    if (f > m) m = f;
    key = exp(x - m); space = exp(-m); rise = exp(r - m); fall = exp(f - m);
    for (k = 0; k <= SPD_CUR; k++) {
        double c = step(k < SPD_N ? bankHz[k] : p->curHz, p->alpha[k], key, space, rise, fall);
        double lc = log(c > 1e-300 ? c : 1e-300);
        p->score[k] = lam * p->score[k] + lc;
        if (SPD_TUNE_MORSE && k < SPD_N) p->morseLik[k] = (1.0 - 1.0 / SPD_MORSE_TAU) * p->morseLik[k] + lc;
    }
    if (SPD_TUNE_MORSE) {
        double unit = SPD_UNIT_NUM / (p->stdWpm > 0.0f ? p->stdWpm : 23.16), c;
        double mu = 1.0 - 1.0 / SPD_MORSE_TAU;
        int on = x > 0.0;
        if (on != p->runOn) { p->runOn = on; p->runLen = 0; }
        p->runLen++;
        if (p->runLen > (on ? SPD_RUN_MARK : SPD_RUN_GAP) * unit) mu = 1.0;          /* counts for neither side */
        for (k = 0; k < SPD_N; k++) if (mu == 1.0) p->morseLik[k] = saved[k];
        c = step(p->stdHz, p->stdAlpha, key, space, rise, fall);
        if (mu < 1.0) p->morseLik[SPD_STD] = mu * p->morseLik[SPD_STD] + log(c > 1e-300 ? c : 1e-300);
        for (k = 0; k < SPD_NULLS; k++) {
            c = step(nullHz[k], p->nullAlpha[k], key, space, rise, fall);
            if (mu < 1.0) p->nullLik[k] = mu * p->nullLik[k] + log(c > 1e-300 ? c : 1e-300);
        }
    }
    p->frames++;
    if (p->hold > 0) p->hold--;
}

float spd_morse_evidence(const Spd *p)
{
    double bm = p->morseLik[0], bn = p->nullLik[0];
    int k;
    for (k = 1; k <= SPD_STD; k++) if (p->morseLik[k] > bm) bm = p->morseLik[k];
    for (k = 1; k < SPD_NULLS; k++) if (p->nullLik[k] > bn) bn = p->nullLik[k];
    return (float)(bm - bn);
}

void spd_forget(Spd *p)
{
    int k;
    for (k = 0; k <= SPD_CUR; k++) p->score[k] = 0.0;
}

int spd_index_of(const Spd *p, float wpm)
{
    int k, best = 0;
    for (k = 1; k < SPD_N; k++)
        if (fabs(log(p->wpm[k] / wpm)) < fabs(log(p->wpm[best] / wpm))) best = k;
    return best;
}

int spd_best(const Spd *p)
{
    int k, best = 0;
    for (k = 1; k < SPD_N; k++) if (p->score[k] > p->score[best]) best = k;
    return best;
}
