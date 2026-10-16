/* SPM (spectrum matcher) and ENV (envelope estimator), 04-signal-extraction.md. */
#include <math.h>
#include <string.h>
#include "vail_internal.h"

/* ------------------------------------------------------------------ SPM */

void spm_init(Spm *s, int binIndex0)
{
    memset(s, 0, sizeof *s);
    runext_init(&s->spmSmoothMax, SPM_FLOOR_MAX_WINDOW, 1);
    runext_init(&s->spmNoiseMin, SPM_FLOOR_MIN_WINDOW, 0);
    s->spmCenterBin = binIndex0;
    s->spmCenterOffset = 0.0f;
}

void spm_free(Spm *s)
{
    runext_free(&s->spmSmoothMax);
    runext_free(&s->spmNoiseMin);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

void spm_track_peak(Spm *s, const float *t)               /* 04 §4.1 */
{
    int off, hit = 0;
    for (off = -SPM_SEARCH_RADIUS; off <= SPM_SEARCH_RADIUS; off++) {
        int i = s->spmCenterBin + off;
        if (i < 1 || i > N_BINS - 2) continue;
        if (t[i] > SPM_PEAK_MIN_POWER && t[i] > t[i - 1] && t[i] > t[i + 1]) { hit = 1; break; }
    }
    if (!hit && SPM_TUNE_EDGE_FOLLOW) {                     /* tuning E5: the peak lies beyond ±radius */
        int c = s->spmCenterBin, best = 0, d;
        float bestV = (float)SPM_PEAK_MIN_POWER;
        for (d = -SPM_SEARCH_RADIUS; d <= SPM_SEARCH_RADIUS; d += 2 * SPM_SEARCH_RADIUS) {
            int k, mono = 1, sg = d > 0 ? 1 : -1;
            if (c + d < 1 || c + d > N_BINS - 2) continue;
            for (k = 0; k < SPM_SEARCH_RADIUS; k++)
                if (!(t[c + sg * (k + 1)] > t[c + sg * k])) mono = 0;
            if (mono && t[c + d] > bestV) { bestV = t[c + d]; best = d; }
        }
        if (best) { off = best; hit = 1; }
    }
    if (hit) {
        double o = (double)s->spmCenterOffset * SPM_OFFSET_DECAY + off * SPM_OFFSET_GAIN;
        s->spmCenterOffset = (float)o;
        s->spmCenterBin += (int)nearbyint(s->spmCenterOffset);        /* round half to even */
        s->spmCenterOffset -= truncf(s->spmCenterOffset);
    }
    s->spmCenterBin = clampi(s->spmCenterBin, SPM_BIN_MIN, N_BINS - 1 - SPM_BIN_MARGIN);
}

void spm_process_frame(Spm *s, const float *sp, float noiseFloor)  /* 04 §4.2, §4.3 */
{
    float mx;
    if (noiseFloor < VAIL_TUNE_POWER_FLOOR) noiseFloor = VAIL_TUNE_POWER_FLOOR;   /* tuning E4 */
    mx = runext_push(&s->spmSmoothMax, noiseFloor);
    float mn = runext_push(&s->spmNoiseMin, mx);
    double inv, sum = 0.0;
    int k, i;

    s->spmFloor = (float)(SPM_FLOOR_DECAY * (double)s->spmFloor + SPM_FLOOR_GAIN * (double)mn);
    if (s->spmFloor == 0.0f) s->spmFloor = noiseFloor;
    if (s->spmFloor < SPM_FLOOR_ZERO_EPS) s->spmFloor = sp[s->spmCenterBin];
    inv = 1.0 / ((double)s->spmFloor + SPM_NORM_EPS);
    for (k = 0; k <= 2 * SPM_SHAPE_HALF_WIDTH; k++) {
        int j = clampi(s->spmCenterBin + k - SPM_SHAPE_HALF_WIDTH, 0, N_BINS - 1);
        s->spmShape[k] = (float)(sp[j] * inv);
    }
    for (i = SPM_SHOULDER_FIRST; i <= SPM_SHOULDER_LAST; i++) {
        double prod = (double)s->spmShape[10 - i] * s->spmShape[10 + i] + SPM_NORM_EPS;
        sum += log(prod);
    }
    s->spmShoulder = (float)exp(sum * SPM_SHOULDER_EXPONENT);
    s->spmPeakAmp = sqrtf(s->spmShape[10]);
    s->spmProminence = s->spmPeakAmp - s->spmShoulder;
}

/* ------------------------------------------------------------------ ENV */

/* field indices within a shift-memory row (04 §3.3) */
enum { F_AMP = 0, F_PROM = 1, F_SAMPLE = 2, F_ENV = 3, F_SOFT = 4, F_THRMIN = 5, F_THR = 6, F_CARRIER = 7 };

void env_init(Env *e)
{
    memset(e, 0, sizeof *e);
    boxavg_init(&e->envSmoother, ENV_ENV_WINDOW, 1);
    boxavg_init(&e->envThrShaper, ENV_THR_SHAPE_WINDOW, 1);
    runext_init(&e->envPeakHold, ENV_STRENGTH_WINDOW, 1);
    runext_init(&e->envMaxShort, ENV_PROM_MAX_WINDOW, 1);
    runext_init(&e->envThrMinHold, ENV_THR_MIN_WINDOW, 0);
    e->envThrUpper = (float)ENV_THR_UPPER;
    e->envKeyProb = (float)ENV_KEY_INACTIVE;
}

void env_free(Env *e)
{
    boxavg_free(&e->envSmoother);
    boxavg_free(&e->envThrShaper);
    runext_free(&e->envPeakHold);
    runext_free(&e->envMaxShort);
    runext_free(&e->envThrMinHold);
}

static double tanh_clamp(double x)
{
    if (x > ENV_KEY_TANH_LIMIT) return ENV_KEY_TANH_SAT;
    if (x < -ENV_KEY_TANH_LIMIT) return -ENV_KEY_TANH_SAT;
    return tanh(x);
}

void env_apply(Env *e, float a2, float a3)                 /* 04 §4.4 */
{
    float (*m)[ENV_SHIFT_ROW_FLOATS] = e->mem;
    float sample, envelope, v5, v24, soft, thresholdIn, v16;
    int carrier, flagPrev, dip;
    double hi;

    /* (1) advance: row r receives the previous row r+1 */
    memmove(&m[0][0], &m[1][0], (ENV_SHIFT_ROWS - 1) * ENV_SHIFT_ROW_FLOATS * sizeof(float));
    /* (2) clamp prominence (only ordered negatives) */
    if (a2 < 0.0f) a2 = 0.0f;
    /* (3) inputs */
    m[37][F_PROM] = a2;
    m[37][F_AMP] = a3;
    /* (4)(5) robust sample min(p1, max(p2, p0)) */
    {
        float p0 = m[37][F_PROM], p1 = m[36][F_PROM], p2 = m[35][F_PROM];
        float mx = p2 > p0 ? p2 : p0;
        sample = p1 < mx ? p1 : mx;
    }
    m[36][F_SAMPLE] = sample;
    /* (6) envelope */
    envelope = boxavg_push(&e->envSmoother, sample);
    if (envelope < 0.0f) envelope = 0.0f;          /* float rounding of the running sum */
    m[1][F_ENV] = envelope;
    /* (7) carrier hysteresis */
    flagPrev = m[0][F_CARRIER] != 0.0f;
    carrier = flagPrev ? (envelope > ENV_HYST_OFF) : (envelope > ENV_HYST_ON);
    /* (8) strength */
    e->envStrength = runext_push(&e->envPeakHold, envelope);
    /* (9) */
    v5 = runext_push(&e->envMaxShort, sample);
    v24 = boxavg_push(&e->envThrShaper, v5);
    /* (10) soft threshold */
    soft = (float)(ENV_THR_SHAPE_GAIN * fmax((double)v24 - ENV_THR_BASE, 0.0) + ENV_THR_BASE);
    m[24][F_SOFT] = soft;
    /* (11) dip detection: sample12 = row 24, sample13 = row 23, sample14 = row 22 */
    hi = fmax(m[24][F_SAMPLE], m[22][F_SAMPLE]);
    dip = ((double)m[23][F_SAMPLE] < hi)
          || ((double)m[23][F_SOFT] > ENV_DIP_BASE)
          || ((double)m[23][F_SAMPLE] <= ENV_DIP_RATIO * ((double)m[0][F_THR] + ENV_DIP_BASE));
    /* (12) threshold branch */
    if (dip) {
        thresholdIn = (float)ENV_THR_UPPER;
    } else {
        double g = fmax(m[23][F_PROM], 0.0);                     /* prom14 */
        double q = (float)(ENV_THR_ATTACK_GAIN * fmax(g - ENV_THR_BASE, 0.0) + ENV_THR_BASE);
        thresholdIn = (float)fmin(q, ENV_THR_UPPER);
    }
    v16 = runext_push(&e->envThrMinHold, thresholdIn);
    /* (13) floor: must not fall below the soft threshold 23 frames ago (row 1 / f4) */
    if (m[1][F_SOFT] > v16) v16 = m[1][F_SOFT];
    m[1][F_THRMIN] = v16;
    m[1][F_THR] = v16;
    m[1][F_CARRIER] = (float)carrier;
    e->envEnvelope = envelope;
    e->envThreshold = v16;
    e->envCarrier = carrier;
    /* (14) keying soft probability, prom36 = row 1 / f1 */
    if (carrier) {
        double x = fmax(ENV_KEY_MIN_LEVEL, (double)m[1][F_PROM]) - (double)v16;
        e->envKeyProb = (float)(ENV_KEY_HALF * tanh_clamp(x) + ENV_KEY_HALF);
    } else {
        e->envKeyProb = (float)ENV_KEY_INACTIVE;
    }
    /* (15) long-term mean square of amp36 = row 1 / f0 */
    if (e->envKeyProb > ENV_SQ_GATE) {
        int n = e->envMeanLen;
        double a = m[1][F_AMP];
        e->envMeanSquare = (float)(((double)e->envMeanSquare * n + a * a) / (n + 1));
        e->envMeanLen = (n + 1 < ENV_SQ_MAX_LEN) ? n + 1 : ENV_SQ_MAX_LEN;
    }
}
