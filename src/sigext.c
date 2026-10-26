/* SPM (spectrum matcher) and ENV (envelope estimator), 04-signal-extraction.md. */
#include <math.h>
#include <stdlib.h>
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
    e->envSnr = LIK_S_INIT;
}

void env_free(Env *e)
{
    boxavg_free(&e->envSmoother);
    boxavg_free(&e->envThrShaper);
    runext_free(&e->envPeakHold);
    runext_free(&e->envMaxShort);
    runext_free(&e->envThrMinHold);
}

/* SEG input without an edge model, from a key-down probability (05 §4.2) */
static void env_obs_from_prob(Env *e, float keyProb)
{
    double p = keyProb < 1e-12f ? 1e-12 : (keyProb > 1.0f - 1e-7f ? 1.0 - 1e-7 : keyProb);
    e->envKeyProb = keyProb;
    e->envObs.keyLogOdds = (float)log(p / (1.0 - p));
    e->envObs.riseLogOdds = e->envObs.keyLogOdds;
    e->envObs.fallLogOdds = 0.0f;
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
    env_obs_from_prob(e, e->envKeyProb);
    /* (15) long-term mean square of amp36 = row 1 / f0 */
    if (e->envKeyProb > ENV_SQ_GATE) {
        int n = e->envMeanLen;
        double a = m[1][F_AMP];
        e->envMeanSquare = (float)(((double)e->envMeanSquare * n + a * a) / (n + 1));
        e->envMeanLen = (n + 1 < ENV_SQ_MAX_LEN) ? n + 1 : ENV_SQ_MAX_LEN;
    }
}

/* ------------------------------------------------------------------ likelihood ENV */

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

/* Noise references of one frame around channel bin c, from the bins at offsets LIK_NB_FIRST..LIK_NB_LAST:
 * the mean power on each side (a side with fewer than 5 bins in the spectrum copies the other), and the
 * frame's broadband level, the median of all those bins scaled to a mean, which a click or a static
 * crash raises but a signal occupying a few bins on one side does not. */
void spm_noise_sides(const float *sp, int c, float *left, float *right, float *broad)
{
    float v[2 * (LIK_NB_LAST - LIK_NB_FIRST + 1)];
    double l = 0.0, r = 0.0;
    int nl = 0, nr = 0, k, n = 0;
    for (k = LIK_NB_FIRST; k <= LIK_NB_LAST; k++) {
        if (c - k >= 0) { l += sp[c - k]; nl++; v[n++] = sp[c - k]; }
        if (c + k < N_BINS) { r += sp[c + k]; nr++; v[n++] = sp[c + k]; }
    }
    qsort(v, (size_t)n, sizeof(float), cmp_float);
    *broad = n ? v[n / 2] * 1.4427f : 0.0f;                      /* median of exponential = ln 2 x mean */
    *left = nl >= 5 ? (float)(l / nl) : -1.0f;
    *right = nr >= 5 ? (float)(r / nr) : -1.0f;
    if (*left < 0.0f) *left = *right;
    if (*right < 0.0f) *right = *left;
}

int env_peak_move(Env *e)
{
    Lik *L = &e->lik;
    const double *x = L->peakEx + LIK_PEAK_SPAN;           /* x[k], k = -span..span */
    int k, best = 0;
    if (!LIK_TUNE_PEAK || !e->envCarrier || L->peakHold > 0 || x[0] < LIK_PEAK_MIN) return 0;
    for (k = -LIK_PEAK_SPAN; k <= LIK_PEAK_SPAN; k++) if (x[k] > x[best]) best = k;
    if (best == 0 || best == -LIK_PEAK_SPAN || best == LIK_PEAK_SPAN || x[best] < LIK_PEAK_RATIO * x[0]) return 0;
    for (k = 0; k <= 2 * LIK_PEAK_SPAN; k++) L->peakEx[k] = 0.0;
    L->peakHold = LIK_PEAK_HOLD;
    return best > 0 ? 1 : -1;
}

/* LLR scale for frames that share noise: 1 / (1 + 2 x the summed power correlations of a frame with its
 * neighbours), from the FE window (02 §4.1) and hop */
static double lik_kappa(void)
{
    static double kappa = 0.0;
    if (kappa == 0.0) {
        double w[FE_FFT_SIZE], e = 0.0, sum = 0.0;
        int i, k;
        for (i = 0; i < FE_FFT_SIZE; i++) {
            double x = FE_TWO_PI * i / FE_FFT_SIZE, d = FE_TWO_PI * FE_WINDOW_CUTOFF * (i - FE_WINDOW_CENTER);
            w[i] = (0.42 - 0.5 * cos(x) + 0.08 * cos(2 * x)) * (d == 0.0 ? 1.0 : sin(d) / d);
            e += w[i] * w[i];
        }
        for (k = 1; k * FE_HOP_SAMPLES < FE_FFT_SIZE; k++) {
            double r = 0.0;
            for (i = 0; i + k * FE_HOP_SAMPLES < FE_FFT_SIZE; i++) r += w[i] * w[i + k * FE_HOP_SAMPLES];
            sum += (r / e) * (r / e);
        }
        kappa = 1.0 / (1.0 + 2.0 * sum);
    }
    return kappa;
}

/* ln I0(z), polynomial approximations of Abramowitz & Stegun 9.8.1 / 9.8.2 */
static double log_i0(double z)
{
    if (z < 3.75) {
        double t = z / 3.75;
        t *= t;
        return log(1.0 + t * (3.5156229 + t * (3.0899424 + t * (1.2067492 + t * (0.2659732
               + t * (0.0360768 + t * 0.0045813))))));
    } else {
        double u = 3.75 / z;
        double p = 0.39894228 + u * (0.01328592 + u * (0.00225319 + u * (-0.00157565 + u * (0.00916281
                   + u * (-0.02057706 + u * (0.02635537 + u * (-0.01647633 + u * 0.00392377)))))));
        return z - 0.5 * log(z) + log(p);
    }
}

/* log p(y | key down, SNR s) - log p(y | noise) for power y in noise units: Rician against exponential */
double lik_llr(double y, double s)
{
    return -s + log_i0(2.0 * sqrt(s * (y > 0.0 ? y : 0.0)));
}

/* noise power of frame k: the base noise, or the frame's broadband level when a click or static crash
 * raised it well above the noise and to within LIK_QRN_REL of the centre bin (a strong signal's own
 * leakage into the side bins is 40 dB or more down) */
static double frame_noise(const Lik *L, int k, double noise)
{
    double b = L->broad[k & (LIK_RING - 1)];
    return (b > LIK_QRN_RATIO * noise && b > LIK_QRN_REL * L->x[k & (LIK_RING - 1)]) ? b : noise;
}

/* LLR of frame k for a key-down power of gain x s x noise: an impulsive frame keeps the signal power
 * but sees more noise, so its SNR drops instead of its power being normalised away */
static double frame_llr(const Lik *L, int k, double noise, double s, double gain)
{
    double nk = frame_noise(L, k, noise);
    return lik_llr(L->x[k & (LIK_RING - 1)] / nk, gain * s * noise / nk);
}

static float clamp_llr(double x)
{
    return (float)(x > LIK_LLR_CLAMP ? LIK_LLR_CLAMP : (x < -LIK_LLR_CLAMP ? -LIK_LLR_CLAMP : x));
}

/* running mean over the last LIK_NOISE_WIN frames of a side level, impulses entering it clipped */
static double side_mean(float *ring, double *sum, int t, float v, float prev)
{
    const int M = LIK_RING - 1;
    if (t > 0 && v > 3.0f * prev) v = 3.0f * prev;
    ring[t & M] = v;
    *sum += v;
    if (t >= LIK_NOISE_WIN) *sum -= ring[(t - LIK_NOISE_WIN) & M];
    return *sum / (t + 1 < LIK_NOISE_WIN ? t + 1 : LIK_NOISE_WIN);
}

void env_apply_lik(Env *e, float power, const float *beside, float noiseLeft, float noiseRight, float broad,
                   float noiseMin)
{
    Lik *L = &e->lik;
    const int M = LIK_RING - 1, t = L->t;
    double s = e->envSnr, noise, lo, hi;

    L->x[t & M] = power;
    memcpy(L->xn[t & M], beside, sizeof L->xn[0]);
    L->broad[t & M] = broad;
    L->noiseL = (float)side_mean(L->nl, &L->nlSum, t, noiseLeft, L->noiseL);
    L->noiseR = (float)side_mean(L->nr, &L->nrSum, t, noiseRight, L->noiseR);
    /* the bin's own key-up frames decide once they carry some weight, kept between the two sides: a side
     * may be raised by a neighbouring signal or lowered by a receiver filter slope, and the own estimate
     * must not run away when the model is off */
    lo = L->noiseL < L->noiseR ? L->noiseL : L->noiseR;
    hi = L->noiseL < L->noiseR ? L->noiseR : L->noiseL;
    noise = L->goSum > 8.0 ? L->noiseOwn : hi;
    if (noise < lo) noise = lo;
    if (noise > hi) noise = hi;
    if (!(noise > noiseMin)) noise = noiseMin;
    L->noise = (float)noise;

    if (t >= 2) {                       /* EM step on frame u = t - 1 with a 3-frame local posterior */
        const int h = (LIK_SIG_WIN - 1) / 2, o = t - LIK_DELAY;
        int u = t - 1, k, old = u - LIK_NOISE_WIN;
        double l3 = 0.0, g, go, z, gs = 0.0, gys = 0.0, cap = LIK_OWN_CAP * lo;
        for (k = u - 1; k <= u + 1; k++) l3 += frame_llr(L, k, noise, s, 1.0);
        z = lik_kappa() * l3 + log(LIK_PRIOR_ON / (1.0 - LIK_PRIOR_ON));
        if (z > 40.0) z = 40.0;
        if (z < -40.0) z = -40.0;
        g = 1.0 / (1.0 + exp(-z));
        go = 1.0 - g;
        if (frame_noise(L, u, noise) > noise) g = go = 0.0;   /* impulsive frames do not train the model */
        L->g[u & M] = (float)g;
        L->gy[u & M] = (float)(g * (L->x[u & M] / noise - 1.0));
        L->go[u & M] = (float)go;
        /* a key-up frame above LIK_OWN_CAP x the lower side is leaked signal (a mark's edge), whose power
         * would otherwise dominate the average over a strong signal's gaps */
        L->gox[u & M] = (float)(go * (L->x[u & M] < cap ? L->x[u & M] : cap));
        /* key-up average (own noise): the last LIK_NOISE_WIN frames */
        L->goSum += L->go[u & M];
        L->goxSum += L->gox[u & M];
        if (old >= 0) {
            L->goSum -= L->go[old & M];
            L->goxSum -= L->gox[old & M];
        }
        if (L->goSum > 1e-6) L->noiseOwn = (float)(L->goxSum / L->goSum);
        if (LIK_TUNE_PEAK) {
            double lam = 1.0 - 1.0 / LIK_PEAK_TAU;
            for (k = 0; k <= 2 * LIK_PEAK_SPAN; k++) {
                float p = k == LIK_PEAK_SPAN ? L->x[u & M] : L->xn[u & M][k < LIK_PEAK_SPAN ? k : k - 1];
                L->peakEx[k] = lam * L->peakEx[k] + g * (p / noise - 1.0);
            }
            if (L->peakHold > 0) L->peakHold--;
        }
        /* key-down average (SNR): LIK_SIG_WIN frames centred on the output frame, triangular weights so
         * that fades are followed without shortening the window */
        for (k = o - h > 0 ? o - h : 0; k <= o + h; k++) {
            double wk = 1.0 - (double)abs(k - o) / (h + 1);
            gs += wk * L->g[k & M];
            gys += wk * L->gy[k & M];
        }
        s = (gys + LIK_W0 * LIK_S_MIN) / (gs + LIK_W0);
        if (s < LIK_S_MIN) s = LIK_S_MIN;
        e->envSnr = (float)s;
    }
    if (t >= LIK_DELAY) {
        int o = t - LIK_DELAY;
        L->llr = (float)frame_llr(L, o, noise, s, 1.0);
        e->envCarrier = e->envCarrier ? (s > LIK_CAR_OFF) : (s > LIK_CAR_ON);
        if (e->envCarrier) {
            double x = clamp_llr(LIK_KAPPA_GAIN * lik_kappa() * L->llr);
            e->envKeyProb = (float)(1.0 / (1.0 + exp(-x)));
            e->envObs.keyLogOdds = (float)x;
            if (LIK_EDGE_GAIN > 0.0f) {         /* the first frame of a mark or a gap sees part of the tone */
                e->envObs.riseLogOdds = clamp_llr(LIK_KAPPA_GAIN * lik_kappa() * frame_llr(L, o, noise, s, LIK_EDGE_GAIN));
                e->envObs.fallLogOdds = e->envObs.riseLogOdds;
            } else {
                e->envObs.riseLogOdds = (float)x;
                e->envObs.fallLogOdds = 0.0f;
            }
        } else {
            env_obs_from_prob(e, (float)ENV_KEY_INACTIVE);
        }
        if (e->envKeyProb > ENV_SQ_GATE) {      /* mean key-down power in noise units (API_Snr) */
            int n = e->envMeanLen;
            e->envMeanSquare = (float)(((double)e->envMeanSquare * n + L->x[o & M] / noise) / (n + 1));
            e->envMeanLen = (n + 1 < ENV_SQ_MAX_LEN) ? n + 1 : ENV_SQ_MAX_LEN;
        }
    } else {
        e->envCarrier = 0;
        env_obs_from_prob(e, (float)ENV_KEY_INACTIVE);
    }
    e->envEnvelope = (float)s;
    e->envThreshold = L->llr;
    e->envStrength = runext_push(&e->envPeakHold, e->envCarrier ? (float)s : 0.0f);
    L->t++;
}
