/* SEG — element segmenter (05-element-segmentation.md, Part I). */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "vail_internal.h"

static float lnf_floor(double p) { return (float)log(p > SEG_LOG_FLOOR ? p : SEG_LOG_FLOOR); }

void seg_init(Seg *s, Sta *sta)
{
    int i;
    memset(s, 0, sizeof *s);
    s->statsRef = sta;
    for (i = 0; i <= SEG_ON_MAX + SEG_OFF_MAX; i++) s->durState[i].score = 0.0f;
    seg_build_duration_tables(s);
}

/* tuning: hazards from the duration pmf normalised over SEG_DUR_CAP frames, with the absorbing state
 * (the last one of each side) taking the hazard of a geometric tail of the same mean residual length.
 * Without it the unnormalised class scores leave about half the mark mass "beyond" the last state,
 * which makes a mark of any length nearly free to extend. 0 restores 05 §4.1. */
#ifndef SEG_TUNE_NORM_DUR
#define SEG_TUNE_NORM_DUR 1
#endif
#define SEG_DUR_CAP       400
#define SEG_HAZ_MIN       0.002
#define SEG_HAZ_MAX       0.999

static void build_side(DurState *d, int nmax, int sign, const Sta *sta)
{
    double q[SEG_DUR_CAP + 1], z = 0.0, surv = 1.0, h, tail = 0.0, mrl = 0.0;
    int i;
    for (i = 1; i <= SEG_DUR_CAP; i++) { q[i] = sta_total_prob(sta, sign * i); z += q[i]; }
    for (i = 1; i <= SEG_DUR_CAP; i++) q[i] /= z;
    for (i = 1; i < nmax; i++) {
        h = surv > 1e-12 ? q[i] / surv : SEG_HAZ_MAX;
        if (h < SEG_HAZ_MIN) h = SEG_HAZ_MIN;
        if (h > SEG_HAZ_MAX) h = SEG_HAZ_MAX;
        d[i].logEnd = lnf_floor(h);
        d[i].logContinue = lnf_floor(1.0 - h);
        surv -= q[i];
    }
    for (i = nmax; i <= SEG_DUR_CAP; i++) { tail += q[i]; mrl += (i - nmax + 1) * q[i]; }
    h = (tail > 1e-12 && mrl > 0.0) ? tail / mrl : SEG_HAZ_MAX;
    if (h < SEG_HAZ_MIN) h = SEG_HAZ_MIN;
    if (h > SEG_HAZ_MAX) h = SEG_HAZ_MAX;
    d[nmax].logEnd = lnf_floor(h);
    d[nmax].logContinue = lnf_floor(1.0 - h);
}

void seg_tables_from(DurState *d, const Sta *sta)
{
    build_side(d, SEG_ON_MAX, +1, sta);
    build_side(d + SEG_ON_MAX, SEG_OFF_MAX, -1, sta);
}

void seg_build_duration_tables(Seg *s)                     /* 05 §4.1 */
{
    double survival = 1.0, hazard;
    int i;
    if (SEG_TUNE_NORM_DUR) {
        seg_tables_from(s->durState, s->statsRef);
        return;
    }
    for (i = 1; i <= SEG_ON_MAX; i++) {
        hazard = 1.0 - sta_total_prob(s->statsRef, +i) / survival;
        s->durState[i].logContinue = lnf_floor(hazard);
        s->durState[i].logEnd = lnf_floor(1.0 - hazard);
        survival *= hazard;
    }
    survival = 1.0;
    for (i = 1; i <= SEG_OFF_MAX; i++) {
        hazard = 1.0 - sta_total_prob(s->statsRef, -i) / survival;
        s->durState[SEG_ON_MAX + i].logContinue = lnf_floor(hazard);
        s->durState[SEG_ON_MAX + i].logEnd = lnf_floor(1.0 - hazard);
        survival *= hazard;
    }
}

static void forward_step(Seg *s)                           /* 05 §4.3 */
{
    enum { ON = SEG_ON_MAX, OFF = SEG_OFF_MAX };
    DurState *d = s->durState;
    int i, bestOnLen, bestOffLen;
    float bestOnScore, bestOffScore, sc;

    bestOnLen = SEG_ON_MAX + s->onRun;
    bestOnScore = d[SEG_ON_MAX].score + d[SEG_ON_MAX].logEnd;
    for (i = SEG_ON_MAX - 1; i >= 1; i--) {
        sc = d[i].score + d[i].logEnd;
        if (sc > bestOnScore) { bestOnLen = i; bestOnScore = sc; }
    }
    s->onDurations[s->windowIndex] = bestOnLen;
    if (d[ON].score + d[ON].logContinue <= d[ON - 1].score + d[ON - 1].logContinue) {
        d[ON].score = d[ON - 1].score + d[ON - 1].logContinue + s->keyLogOdds;
        s->onRun = 0;
    } else {
        d[ON].score = d[ON].score + d[ON].logContinue + s->keyLogOdds;
        s->onRun++;
    }
    for (i = ON - 1; i >= 2; i--)
        d[i].score = d[i - 1].score + d[i - 1].logContinue + s->keyLogOdds;

    bestOffLen = SEG_OFF_MAX + s->offRun;
    bestOffScore = d[ON + OFF].score + d[ON + OFF].logEnd;
    for (i = SEG_OFF_MAX - 1; i >= 1; i--) {
        sc = d[ON + i].score + d[ON + i].logEnd;
        if (sc > bestOffScore) { bestOffLen = i; bestOffScore = sc; }
    }
    s->offDurations[s->windowIndex] = -bestOffLen;
    if (d[ON + OFF].score + d[ON + OFF].logContinue <= d[ON + OFF - 1].score + d[ON + OFF - 1].logContinue) {
        d[ON + OFF].score = d[ON + OFF - 1].score + d[ON + OFF - 1].logContinue + s->spaceLogOdds;
        s->offRun = 0;
    } else {
        d[ON + OFF].score = d[ON + OFF].score + d[ON + OFF].logContinue + s->spaceLogOdds;
        s->offRun++;
    }
    for (i = OFF - 1; i >= 2; i--)
        d[ON + i].score = d[ON + i - 1].score + d[ON + i - 1].logContinue + s->spaceLogOdds;

    d[1].score = bestOffScore + s->riseLogOdds;
    d[ON + 1].score = bestOnScore + s->fallLogOdds;
}

static void seg_reset(Seg *s)                              /* 05 §4.5 */
{
    int i;
    s->windowIndex = 0;
    s->carrierDrop = 0;
    s->outCount = 0;
    s->onRun = s->offRun = 0;
    for (i = 2; i <= SEG_ON_MAX; i++) s->durState[i].score = SEG_LOG_ZERO;
    for (i = 2; i <= SEG_OFF_MAX; i++) s->durState[SEG_ON_MAX + i].score = SEG_LOG_ZERO;
}

static uint8_t or_bits(const Seg *s, int pos, int n)
{
    int k, lo = pos - n + 1;
    uint8_t f = 0;
    if (lo < 0) lo = 0;
    for (k = lo; k <= pos && k < SEG_WINDOW_LEN; k++) f |= s->bitHist[k];
    return f;
}

static int expand_len(const Seg *s, int st)
{
    if (st == SEG_ON_MAX) return SEG_ON_MAX + s->onRun;
    if (st == -SEG_OFF_MAX) return -SEG_OFF_MAX - s->offRun;
    return st;
}

/* 05 §4.4.3 survival pruning: is the state consistent with the newest committed element out[0]? */
static int survives(const Seg *s, int st)
{
    int emitLen, pos;
    if (s->outCount == 0) return 1;
    emitLen = expand_len(s, st);
    pos = s->windowIndex + 1;
    for (;;) {
        pos -= abs(emitLen);
        if (pos < 0) return 0;
        emitLen = (emitLen <= 0) ? s->onDurations[pos] : s->offDurations[pos];
        if (pos + s->stepCounter - s->windowIndex == s->out[0].elementRef
            && emitLen == s->out[0].durationSigned)
            return 1;
        if (pos < SEG_CONTEXT_KEEP) return 0;
    }
}

static void traceback_and_emit(Seg *s)                     /* 05 §4.4 */
{
    DurState *d = s->durState;
    int i, best = 1, emitLen, pos, n = 0;
    float bestScore = d[1].score;

    for (i = 2; i <= SEG_ON_MAX; i++)
        if (d[i].score > bestScore) { best = i; bestScore = d[i].score; }
    for (i = 1; i <= SEG_OFF_MAX; i++)
        if (d[SEG_ON_MAX + i].score > bestScore) { best = -i; bestScore = d[SEG_ON_MAX + i].score; }
    emitLen = expand_len(s, best);

    pos = s->windowIndex + 1;
    for (;;) {
        pos -= abs(emitLen);
        if (pos < 0) break;
        emitLen = (emitLen <= 0) ? s->onDurations[pos] : s->offDurations[pos];
        if ((pos < SEG_CONTEXT_KEEP || s->carrierDrop) && n < SEG_EMIT_CAPACITY) {
            Element *e = &s->out[n++];
            memset(e, 0, sizeof *e);
            e->durationSigned = emitLen;
            e->elementRef = pos + s->stepCounter - s->windowIndex;
            e->qualityFlag = or_bits(s, pos, abs(emitLen));
        }
    }
    s->outCount = n;

    if (s->carrierDrop) {
        for (i = 1; i <= SEG_ON_MAX + SEG_OFF_MAX; i++) d[i].score = SEG_LOG_ZERO;
        d[best > 0 ? best : SEG_ON_MAX - best].score = 0.0f;
        s->windowIndex = 0;
    } else {
        float newScore[SEG_ON_MAX + SEG_OFF_MAX + 1];
        for (i = 1; i <= SEG_ON_MAX; i++)
            newScore[i] = survives(s, i) ? d[i].score - bestScore : SEG_LOG_ZERO;
        for (i = 1; i <= SEG_OFF_MAX; i++)
            newScore[SEG_ON_MAX + i] = survives(s, -i) ? d[SEG_ON_MAX + i].score - bestScore : SEG_LOG_ZERO;
        for (i = 1; i <= SEG_ON_MAX + SEG_OFF_MAX; i++) d[i].score = newScore[i];
        memmove(s->onDurations, s->onDurations + SEG_CONTEXT_KEEP, SEG_CONTEXT_KEEP * sizeof(int));
        memmove(s->offDurations, s->offDurations + SEG_CONTEXT_KEEP, SEG_CONTEXT_KEEP * sizeof(int));
        memmove(s->bitHist, s->bitHist + SEG_CONTEXT_KEEP, SEG_CONTEXT_KEEP);
        s->windowIndex = SEG_CONTEXT_KEEP;
    }
}

/* log(1 + e^x) without overflow */
static double softplus(double x) { return x > 30.0 ? x : (x < -30.0 ? exp(x) : log1p(exp(x))); }

void seg_update(Seg *s, int bit, const SegObs *o)          /* 05 §4.2 */
{
    s->bitHist[s->windowIndex] = (uint8_t)(bit != 0);
    s->keyLogOdds = (float)-softplus(-o->keyLogOdds);      /* log keyProb */
    s->spaceLogOdds = (float)-softplus(o->keyLogOdds);     /* log (1 - keyProb) */
    if (s->keyLogOdds < SEG_LOG_ZERO) s->keyLogOdds = SEG_LOG_ZERO;
    if (s->spaceLogOdds < SEG_LOG_ZERO) s->spaceLogOdds = SEG_LOG_ZERO;
    s->riseLogOdds = s->spaceLogOdds + o->riseLogOdds;
    s->fallLogOdds = s->spaceLogOdds + o->fallLogOdds;
    forward_step(s);
    if (s->windowIndex == SEG_WINDOW_LEN - 1 || s->carrierDrop) {
        if (s->windowIndex < SEG_CONTEXT_KEEP) seg_reset(s);
        else traceback_and_emit(s);
    } else {
        s->windowIndex++;
        s->outCount = 0;
    }
    s->stepCounter++;
    s->carrierDrop = 0;
}
