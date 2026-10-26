/* SPD — speed bank: fixed-speed duration HMMs scored on SEG's input (see vail_internal.h). The forward
 * (sum over paths) likelihood is used: a best-path score lets every model explain weak marks away as
 * one long pause, which the slowest model does most cheaply. */
#include <math.h>
#include <string.h>
#include "vail_internal.h"

static const float SPD_UNITS[5] = { 1.0f, 3.0f, 1.0f, 3.0f, 7.0f };   /* dot dash gap lgap wgap */

void spd_means(const Spd *p, int k, float *means)
{
    double u = SPD_UNIT_NUM / p->wpm[k];
    int c;
    for (c = 0; c < 5; c++) means[c] = (float)log(u * SPD_UNITS[c]);
}

void spd_init(Spd *p)
{
    DurState d[SEG_ON_MAX + SEG_OFF_MAX + 1];
    Sta tmp;
    int k, i;
    memset(p, 0, sizeof *p);
    sta_init(&tmp);
    for (k = 0; k < SPD_N; k++) {
        p->wpm[k] = (float)(SPD_WPM_MIN * pow(SPD_WPM_MAX / SPD_WPM_MIN, (double)k / (SPD_N - 1)));
        spd_means(p, k, tmp.means);
        seg_tables_from(d, &tmp);
        for (i = 1; i <= SEG_ON_MAX + SEG_OFF_MAX; i++) p->hz[k][i] = expf(d[i].logEnd);
        p->alpha[k][SEG_ON_MAX + SEG_OFF_MAX] = 1.0;           /* start in a long gap */
    }
    p->alpha[SPD_CUR][SEG_ON_MAX + SEG_OFF_MAX] = 1.0;
    sta_free(&tmp);
}

void spd_set_current(Spd *p, const DurState *d)
{
    int i;
    for (i = 1; i <= SEG_ON_MAX + SEG_OFF_MAX; i++) p->hz[SPD_CUR][i] = expf(d[i].logEnd);
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
    double m = x > 0.0 ? x : 0.0, key, space, rise, fall;
    int k;
    if (r > m) m = r;
    if (f > m) m = f;
    key = exp(x - m); space = exp(-m); rise = exp(r - m); fall = exp(f - m);
    for (k = 0; k <= SPD_CUR; k++) {
        double c = step(p->hz[k], p->alpha[k], key, space, rise, fall);
        p->score[k] = lam * p->score[k] + log(c > 1e-300 ? c : 1e-300);
    }
    p->frames++;
    if (p->hold > 0) p->hold--;
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
