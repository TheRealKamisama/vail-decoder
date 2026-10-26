/* STA — element duration statistics model (05-element-segmentation.md, Part II). */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "vail_internal.h"

static const float STA_MIX_WEIGHT[5] = { 0.58f, 0.42f, 0.65f, 0.23f, 0.12f };
static const float STA_MEAN_INIT[5]  = { 1.62f, 2.70f, 1.50f, 2.72f, 3.56f };
static const float STA_VAR_INIT[5]   = { 0.0567f, 0.0509f, 0.0710f, 0.0779f, 0.0697f };
static const float STA_COV_PRIOR[5][5] = {
    { 0.0567f, 0.0492f, 0.0454f, 0.0474f, 0.0367f },
    { 0.0492f, 0.0509f, 0.0508f, 0.0502f, 0.0385f },
    { 0.0454f, 0.0508f, 0.0710f, 0.0602f, 0.0464f },
    { 0.0474f, 0.0502f, 0.0602f, 0.0779f, 0.0612f },
    { 0.0367f, 0.0385f, 0.0464f, 0.0612f, 0.0697f },
};
static const float STA_COV_PRIOR_INV[5][5] = {
    {  118.485229f, -134.391098f,  23.396200f,  -2.068530f,  -1.913050f },
    { -134.391098f,  231.483887f, -65.243530f, -20.121161f,   3.999120f },
    {   23.396200f,  -65.243530f,  64.486313f, -22.355499f,   0.419330f },
    {   -2.068530f,  -20.121161f, -22.355499f,  74.339058f, -38.187592f },
    {   -1.913050f,    3.999120f,   0.419330f, -38.187592f,  46.396938f },
};
static const float STA_GAP_SPLIT_PRIOR[8][8] = {
    { 0, 0, 0, 0, 0.00027f, 0.00014f, 0, 0 },
    { 0, 0, 0, 0.00027f, 0.00041f, 0.00082f, 0, 0 },
    { 0, 0.00041f, 0.00123f, 0.00519f, 0.00505f, 0.00191f, 0, 0 },
    { 0, 0.00027f, 0.00178f, 0.01939f, 0.04984f, 0.01065f, 0, 0 },
    { 0, 0.00096f, 0.04438f, 0.20306f, 0.08111f, 0, 0, 0 },
    { 0, 0, 0.01557f, 0.22682f, 0.22026f, 0, 0, 0 },
    { 0, 0, 0, 0.03482f, 0.07524f, 0, 0, 0 },
    { 0.00014f, 0, 0, 0, 0, 0, 0, 0 },
};

#define STA_COV_DECAY          0.003f
#define STA_TAIL_MED           0.0001f
#define STA_TAIL_LONG          0.1f
#define STA_TAIL_SIGMA         3.0f
#define STA_DUR_CAP            300.0f
#define STA_LOGLEN_CAP         5.703782475f
#define STA_PROB_CLAMP         0.99f
#define STA_VAR_RATIO_LO       0.15f
#define STA_VAR_RATIO_HI       0.5f
#define STA_VAR_RATIO_HI_WORD  1.0f
#define STA_MEAN_LONG_MARGIN   0.405465108f
#define STA_RATIO_MED          1.09861229f
#define STA_RATIO_WORD         2.07944154f
#define STA_PRIOR_EPS          1e-9
#define STA_LIK_EPS            1e-12
#define STA_TWO_PI             6.28318531f
#define STA_WPM_NUM            280.125f

/* implementation tuning (12-open-items.md §E); 0 restores the reference behaviour */
#ifndef STA_TUNE_PRIME_COMMIT
#define STA_TUNE_PRIME_COMMIT  0
#endif
#ifndef STA_TUNE_LONG_GAP
#define STA_TUNE_LONG_GAP      1
#endif
#ifndef STA_TUNE_GRID_FIT
#define STA_TUNE_GRID_FIT      1
#endif
#ifndef STA_TUNE_DROP_FLUSH_MARKS
#define STA_TUNE_DROP_FLUSH_MARKS 3
#endif
#ifndef STA_TUNE_VAR_PRIOR_REF
#define STA_TUNE_VAR_PRIOR_REF 0
#endif

static float STA_SPLIT_PRIOR[512];
static int splitPriorReady = 0;

static void build_split_prior(void)                        /* 05 §8.6 */
{
    static const double C7[8] = { 1, 7, 21, 35, 35, 21, 7, 1 };
    int a, b, c;
    for (a = 0; a < 8; a++)
        for (b = 0; b < 8; b++)
            for (c = 0; c < 8; c++) {
                double bin = C7[a] * pow(0.58, a) * pow(0.42, 7 - a);
                STA_SPLIT_PRIOR[64 * a + 8 * b + c] =
                    (float)log(bin * ((double)STA_GAP_SPLIT_PRIOR[b][c] + STA_PRIOR_EPS));
            }
    splitPriorReady = 1;
}

void sta_reset(Sta *s)
{
    memcpy(s->covariance, STA_COV_PRIOR, sizeof s->covariance);
    memcpy(s->variances, STA_VAR_INIT, sizeof s->variances);
    s->windowCount = 0;
    s->modelDirty = 1;
}

void sta_init(Sta *s)
{
    memset(s, 0, sizeof *s);
    if (!splitPriorReady) build_split_prior();
    memcpy(s->means, STA_MEAN_INIT, sizeof s->means);
    sta_reset(s);
    s->learningPhase = 1;
    s->flushFlag = 0;
    s->modelDirty = 0;
    s->windowLogLik = 0;
}

void sta_free(Sta *s) { free(s->pendingBuffer); s->pendingBuffer = NULL; }

float sta_score_of(const Sta *s, int c, float dur)         /* 05 §8.2 */
{
    float x = logf(dur), d = x - s->means[c];
    float pdf = (float)(exp(-(double)d * d / (2.0 * s->variances[c])) /
                        sqrt((double)STA_TWO_PI * s->variances[c]));
    float t = STA_MIX_WEIGHT[c] * pdf / dur;
    if (c < 3) return STA_MIX_WEIGHT[c] * t;
    if (c == 3) {
        float lo = s->means[2] + STA_TAIL_SIGMA * sqrtf(s->covariance[2][2]);
        float hi = s->means[3] + STA_TAIL_SIGMA * sqrtf(s->covariance[4][4]);
        return (lo < x && x < hi) ? t + STA_TAIL_MED / (hi - lo) : t;
    }
    {
        float lo = s->means[3] + STA_TAIL_SIGMA * sqrtf(s->covariance[3][3]);
        float hi = STA_DUR_CAP;
        return (lo < x && x < hi) ? t + STA_TAIL_LONG / (STA_DUR_CAP - lo) : t;
    }
}

float sta_total_prob(const Sta *s, int d)                  /* 05 §8.3 */
{
    float v;
    if (d > 0) v = sta_score_of(s, 0, (float)d) + sta_score_of(s, 1, (float)d);
    else v = sta_score_of(s, 2, (float)-d) + sta_score_of(s, 3, (float)-d) + sta_score_of(s, 4, (float)-d);
    return v < STA_PROB_CLAMP ? v : STA_PROB_CLAMP;
}

void sta_classify(const Sta *s, Element *e)                /* 05 §4.6.1 */
{
    if (e->durationSigned <= 0) {
        float dur = (float)(e->durationSigned == 0 ? 1 : -e->durationSigned);
        float top;
        e->scores[2] = sta_score_of(s, 2, dur);
        e->scores[3] = sta_score_of(s, 3, dur);
        e->scores[4] = fmaxf(sta_score_of(s, 4, dur), SEG_FEATURE_FLOOR);
        top = fmaxf(e->scores[3], e->scores[4]);
        e->kind = (e->scores[2] <= top) ? ((e->scores[3] <= e->scores[4]) ? 4 : 3) : 2;
    } else {
        e->scores[0] = sta_score_of(s, 0, (float)e->durationSigned);
        e->scores[1] = sta_score_of(s, 1, (float)e->durationSigned);
        e->kind = (e->scores[0] <= e->scores[1]) ? 1 : 0;
    }
}

static void update_class(Sta *s, int c, float x)           /* 05 §8.5 */
{
    float P[5][5], gain[5], lo, hi, r, delta, sinv;
    int i, j;
    if (x > STA_LOGLEN_CAP) x = STA_LOGLEN_CAP;
    r = s->means[c] - x;
    if (s->learningPhase) s->variances[c] = 0.5f * s->variances[c] + 0.5f * r * r;
    else                  s->variances[c] = 0.8f * s->variances[c] + 0.2f * r * r;
    {
        float mref = (STA_TUNE_VAR_PRIOR_REF && STA_MEAN_INIT[c] < s->means[c])  /* tuning E3 */
                     ? STA_MEAN_INIT[c] : s->means[c];
        lo = STA_VAR_RATIO_LO / mref; lo *= lo;
        hi = ((c == 4) ? STA_VAR_RATIO_HI_WORD : STA_VAR_RATIO_HI) / mref; hi *= hi;
    }
    s->variances[c] = fmaxf(lo, fminf(hi, s->variances[c]));
    for (i = 0; i < 5; i++)
        for (j = 0; j < 5; j++) P[i][j] = s->covariance[i][j] + STA_COV_DECAY * STA_COV_PRIOR[i][j];
    sinv = 1.0f / (s->variances[c] + P[c][c]);
    for (i = 0; i < 5; i++)
        for (j = 0; j < 5; j++) s->covariance[i][j] = P[i][j] - P[i][c] * P[c][j] * sinv;
    for (i = 0; i < 5; i++) gain[i] = s->covariance[i][c] / s->variances[c];
    delta = x - s->means[c];
    for (i = 0; i < 5; i++) s->means[i] += gain[i] * delta;
}

static int cmpf(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

static float mean_range(const float *v, int lo, int hi)    /* mean of v[lo..hi-1], 0 if empty */
{
    float s = 0; int i;
    if (hi <= lo) return 0.0f;
    for (i = lo; i < hi; i++) s += v[i];
    return s / (float)(hi - lo);
}

static void reestimate(Sta *s)                             /* 05 §8.6 */
{
    float marks[STA_GROUP_LEN], gaps[STA_GROUP_LEN], meansOld[5], candMean[5];
    double Z = 0, accum[5] = { 0 };
    int k, a, kk, c, i;
    Element *w = s->windowElements;

    for (k = 0; k < STA_GROUP_LEN; k++) {
        int mi = (w[0].durationSigned <= 0) ? 2 * k + 1 : 2 * k;
        int gi = (w[0].durationSigned <= 0) ? 2 * k : 2 * k + 1;
        int md = abs(w[mi].durationSigned), gd = abs(w[gi].durationSigned);
        marks[k] = logf((float)(md ? md : 1));
        gaps[k] = logf((float)(gd ? gd : 1));
    }
    if (STA_TUNE_LONG_GAP) {      /* tuning E2: a gap >= 60 frames counts as the longest ordinary gap */
        float lim = logf((float)STA_UPDATE_DUR_MAX), longest = -1.0f;
        for (k = 0; k < STA_GROUP_LEN; k++)
            if (gaps[k] < lim && gaps[k] > longest) longest = gaps[k];
        for (k = 0; k < STA_GROUP_LEN; k++)
            if (gaps[k] >= lim) gaps[k] = longest >= 0.0f ? longest : s->means[4];
    }
    qsort(marks, STA_GROUP_LEN, sizeof(float), cmpf);
    qsort(gaps, STA_GROUP_LEN, sizeof(float), cmpf);
    memcpy(meansOld, s->means, sizeof meansOld);

    for (a = 0; a <= 7; a++) {
        for (kk = 0; kk <= 4; kk++) {
            for (c = 0; c <= 5 - kk; c++) {
                int b = 7 - c - kk, p, q, nPos = 0;
                float cand[5], dotRef = 0, mah = 0;
                double wgt;
                cand[0] = mean_range(marks, 0, a);
                cand[1] = mean_range(marks, a, 7);
                cand[2] = mean_range(gaps, 0, b);
                cand[3] = mean_range(gaps, b, b + c);
                cand[4] = mean_range(gaps, b + c, 7);
                for (p = 0; p < 5; p++)
                    if (cand[p] > 0) {
                        dotRef += cand[p] - ((p == 1 || p == 3) ? STA_RATIO_MED : 0.0f);
                        nPos++;
                    }
                dotRef = nPos ? dotRef / (float)nPos : STA_MEAN_INIT[0];
                if (cand[0] == 0) cand[0] = dotRef;
                if (cand[1] == 0) cand[1] = dotRef + STA_RATIO_MED;
                if (cand[2] == 0) cand[2] = dotRef;
                if (cand[3] == 0) cand[3] = dotRef + STA_RATIO_MED;
                if (cand[4] == 0) cand[4] = dotRef + STA_RATIO_WORD;
                for (p = 0; p < 5; p++)
                    for (q = 0; q < 5; q++)
                        mah += STA_COV_PRIOR_INV[p][q] * (cand[p] - STA_MEAN_INIT[p]) * (cand[q] - STA_MEAN_INIT[q]);
                {
                    double sse = 0.0;
                    if (STA_TUNE_GRID_FIT) {               /* tuning E6: data-fit term of each split */
                        const float *src[5] = { marks, marks, gaps, gaps, gaps };
                        int lo[5] = { 0, a, 0, b, b + c }, hi[5] = { a, 7, b, b + c, 7 };
                        for (p = 0; p < 5; p++)
                            for (q = lo[p]; q < hi[p]; q++) {
                                double r = (double)src[p][q] - cand[p];
                                sse += r * r / STA_VAR_INIT[p];
                            }
                    }
                    wgt = exp(-0.5 * (double)mah + (double)STA_SPLIT_PRIOR[64 * a + 8 * b + c] - 0.5 * sse);
                }
                Z += wgt;
                for (i = 0; i < 5; i++) accum[i] += cand[i] * wgt;
            }
        }
    }
    for (i = 0; i < 5; i++) {
        double v = accum[i] / Z;
        candMean[i] = (Z > 0 && isfinite(v)) ? (float)v : STA_MEAN_INIT[i];
    }

    memcpy(s->means, candMean, sizeof candMean);
    for (i = 0; i < STA_WINDOW_LEN; i++) sta_classify(s, &w[i]);
    if (!(STA_TUNE_PRIME_COMMIT && s->learningPhase))   /* tuning E1: keep the candidate means while priming */
        memcpy(s->means, meansOld, sizeof meansOld);

    for (i = 0; i < STA_WINDOW_LEN; i++) {
        int d = abs(w[i].durationSigned);
        if (d < STA_UPDATE_DUR_MAX && d > 0) update_class(s, w[i].kind, logf((float)d));
    }
    if (s->means[4] < s->means[3] + STA_MEAN_LONG_MARGIN) s->means[4] = s->means[3] + STA_MEAN_LONG_MARGIN;
    {
        double lik = 1.0;
        for (i = 0; i < STA_WINDOW_LEN; i++)
            if (w[i].kind != 4) lik *= w[i].scores[w[i].kind];
        s->windowLogLik = log(lik + STA_LIK_EPS);
    }
}

int sta_commit(Sta *s, const Element *source, int n, Element **out)   /* 05 §8.4 */
{
    int i, m;
    Element *res;
    if (n < 0) n = 0;
    if (s->learningPhase) {
        if (s->pendingCount + n > s->pendingCap) {
            int cap = (s->pendingCount + n) * 2 + 64;
            s->pendingBuffer = realloc(s->pendingBuffer, (size_t)cap * sizeof(Element));
            s->pendingCap = cap;
        }
        memmove(s->pendingBuffer + n, s->pendingBuffer, (size_t)s->pendingCount * sizeof(Element));
        memcpy(s->pendingBuffer, source, (size_t)n * sizeof(Element));
        s->pendingCount += n;
    }
    s->modelDirty = 0;
    for (i = n - 1; i >= 0; i--) {
        /* gaps are the non-positive durations (05 §4.6.2) */
        int sNew = source[i].durationSigned > 0 ? 1 : -1;
        if (s->windowCount > 0) {
            int sOld = s->windowElements[s->windowCount - 1].durationSigned > 0 ? 1 : -1;
            if (sOld == sNew) continue;
        }
        s->windowElements[s->windowCount++] = source[i];
        if (s->windowCount == STA_WINDOW_LEN) {
            reestimate(s);
            s->windowCount = 0;
            s->modelDirty = 1;
        }
    }
    s->flushFlag = s->learningPhase && s->modelDirty;
    if (s->flushFlag) {
        s->learningPhase = 0;
        m = s->pendingCount;
        res = malloc((size_t)(m ? m : 1) * sizeof(Element));
        memcpy(res, s->pendingBuffer, (size_t)m * sizeof(Element));
        s->pendingCount = 0;
    } else {
        m = n;
        res = malloc((size_t)(m ? m : 1) * sizeof(Element));
        memcpy(res, source, (size_t)m * sizeof(Element));
    }
    for (i = 0; i < m; i++) sta_classify(s, &res[i]);
    *out = res;
    return m;
}

/* tuning E8: a carrier drop during priming hands over the buffered elements (as the first
 * re-estimate would) when they contain at least STA_TUNE_DROP_FLUSH_MARKS marks; 0 if not. */
int sta_flush_priming(Sta *s, Element **out)
{
    int i, marks = 0, m = s->pendingCount;
    Element *res;
    if (!s->learningPhase || STA_TUNE_DROP_FLUSH_MARKS <= 0) return 0;
    for (i = 0; i < m; i++) marks += s->pendingBuffer[i].durationSigned > 0;
    if (marks < STA_TUNE_DROP_FLUSH_MARKS) return 0;
    res = malloc((size_t)m * sizeof(Element));
    memcpy(res, s->pendingBuffer, (size_t)m * sizeof(Element));
    for (i = 0; i < m; i++) sta_classify(s, &res[i]);
    s->pendingCount = 0;
    s->learningPhase = 0;
    s->flushFlag = 1;
    *out = res;
    return m;
}

float sta_speed(const Sta *s)                              /* 05 §8.7 */
{
    double den = 0.58 * exp(s->means[0]) + 0.42 * exp(s->means[1]) + 0.65 * exp(s->means[2]);
    return (float)((double)STA_WPM_NUM / den);
}
