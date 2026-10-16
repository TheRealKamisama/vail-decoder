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

void seg_build_duration_tables(Seg *s)                     /* 05 §4.1 */
{
    double survival = 1.0, hazard;
    int i;
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
    if (d[20].score + d[20].logContinue <= d[19].score + d[19].logContinue) {
        d[20].score = d[19].score + d[19].logContinue + s->keyLogOdds;
        s->onRun = 0;
    } else {
        d[20].score = d[20].score + d[20].logContinue + s->keyLogOdds;
        s->onRun++;
    }
    for (i = 19; i >= 2; i--)
        d[i].score = d[i - 1].score + d[i - 1].logContinue + s->keyLogOdds;

    bestOffLen = SEG_OFF_MAX + s->offRun;
    bestOffScore = d[50].score + d[50].logEnd;
    for (i = SEG_OFF_MAX - 1; i >= 1; i--) {
        sc = d[20 + i].score + d[20 + i].logEnd;
        if (sc > bestOffScore) { bestOffLen = i; bestOffScore = sc; }
    }
    s->offDurations[s->windowIndex] = -bestOffLen;
    if (d[50].score + d[50].logContinue <= d[49].score + d[49].logContinue) {
        d[50].score = d[49].score + d[49].logContinue + s->spaceLogOdds;
        s->offRun = 0;
    } else {
        d[50].score = d[50].score + d[50].logContinue + s->spaceLogOdds;
        s->offRun++;
    }
    for (i = 29; i >= 2; i--)
        d[20 + i].score = d[20 + i - 1].score + d[20 + i - 1].logContinue + s->spaceLogOdds;

    d[1].score = bestOffScore + s->keyLogOdds;
    d[21].score = bestOnScore + s->spaceLogOdds;
}

static void seg_reset(Seg *s)                              /* 05 §4.5 */
{
    int i;
    s->windowIndex = 0;
    s->carrierDrop = 0;
    s->outCount = 0;
    s->onRun = s->offRun = 0;
    for (i = 2; i <= SEG_ON_MAX; i++) s->durState[i].score = SEG_LOG_ZERO;
    for (i = 2; i <= SEG_OFF_MAX; i++) s->durState[20 + i].score = SEG_LOG_ZERO;
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
        if (d[20 + i].score > bestScore) { best = -i; bestScore = d[20 + i].score; }
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
            newScore[20 + i] = survives(s, -i) ? d[20 + i].score - bestScore : SEG_LOG_ZERO;
        for (i = 1; i <= SEG_ON_MAX + SEG_OFF_MAX; i++) d[i].score = newScore[i];
        memmove(s->onDurations, s->onDurations + SEG_CONTEXT_KEEP, SEG_CONTEXT_KEEP * sizeof(int));
        memmove(s->offDurations, s->offDurations + SEG_CONTEXT_KEEP, SEG_CONTEXT_KEEP * sizeof(int));
        memmove(s->bitHist, s->bitHist + SEG_CONTEXT_KEEP, SEG_CONTEXT_KEEP);
        s->windowIndex = SEG_CONTEXT_KEEP;
    }
}

void seg_update(Seg *s, int bit, float keyProb)            /* 05 §4.2 */
{
    s->bitHist[s->windowIndex] = (uint8_t)(bit != 0);
    s->keyLogOdds = lnf_floor(keyProb);
    s->spaceLogOdds = lnf_floor(1.0 - keyProb);
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
