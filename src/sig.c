/* SIG — per-signal pipeline aggregate: SPM → ENV → SEG → STA → CHR → WRD → MSG.
 * The glue rules (carrier drop, gate, quality, text flag, WRD batching) follow 03 §2.1,
 * 05 §0 and the defaults recorded in 12-open-items.md (A12, A13, D6, E8). With CVD_TUNE the
 * characters come from CVD, which runs on the same per-frame input as SEG; SEG and STA still
 * learn the timing that both use. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vail_internal.h"

FILE *vail_trace = NULL;
FILE *vail_elems = NULL;

void sig_init(Sig *s, int bin)
{
    memset(s, 0, sizeof *s);
    spm_init(&s->spm, bin);
    env_init(&s->env);
    sta_init(&s->sta);
    seg_init(&s->seg, &s->sta);
    spd_init(&s->spd);
    spd_set_current(&s->spd, s->seg.durState);
    s->cvd = malloc(sizeof(Cvd));
    cvd_init(s->cvd);
    cvd_set_model(s->cvd, &s->sta);
    s->replayObs = malloc(SIG_REPLAY_MAX * sizeof(SegObs));
    s->replayBit = malloc(SIG_REPLAY_MAX);
    s->replayDrop = malloc(SIG_REPLAY_MAX);
    chr_init(&s->chr);
    wrd_init(&s->wrd);
    s->segGate = 1;
}

void sig_free(Sig *s)
{
    spm_free(&s->spm);
    env_free(&s->env);
    sta_free(&s->sta);
    chr_free(&s->chr);
    wrd_free(&s->wrd);
    free(s->replayObs);
    free(s->replayBit);
    free(s->replayDrop);
    free(s->cvd);
}

/* CVD's committed characters gathered for this frame (a replay may commit several times) */
static void cvd_collect(Sig *s)
{
    int i;
    for (i = 0; i < s->cvd->outCount && s->cvdOutCount < SIG_CVD_OUT_MAX; i++) s->cvdOut[s->cvdOutCount++] = s->cvd->out[i];
}

/* SPD: when a fixed-timing model at another speed explains the recent frames better than SEG's current
 * one by SPD_SNAP_MARGIN, STA restarts from that speed */
static void speed_check(Sig *s)
{
    int kb, kRef;
    spd_update(&s->spd, &s->env.envObs);
    if (s->spd.hold > 0) return;
    kb = spd_best(&s->spd);
    kRef = spd_index_of(&s->spd, sta_speed(&s->sta));
    if (abs(kb - kRef) >= SPD_SNAP_STEPS && s->spd.score[kb] - s->spd.score[SPD_CUR] > SPD_SNAP_MARGIN) {
        spd_means(&s->spd, kb, s->sta.means);
        sta_reset(&s->sta);
        seg_build_duration_tables(&s->seg);
        spd_set_current(&s->spd, s->seg.durState);
        if (CVD_TUNE) cvd_set_model(s->cvd, &s->sta);
        s->spd.score[SPD_CUR] = s->spd.score[kb];
        s->spd.hold = SPD_HOLD;
    }
}

/* keeps SEG's input while STA learns, from the frame before the first element on */
static void replay_store(Sig *s, int carrier, int drop)
{
    if (s->sta.pendingCount == 0 && !carrier && !drop) s->replayCount = 0;   /* nothing to redo yet */
    if (s->replayCount == 0) { s->replayStep = s->seg.stepCounter; s->replayOverflow = 0; }
    if (s->replayCount < SIG_REPLAY_MAX) {
        s->replayObs[s->replayCount] = s->env.envObs;
        s->replayBit[s->replayCount] = s->env.envEnvelope < SEG_SILENCE_LEVEL;
        s->replayDrop[s->replayCount] = (uint8_t)drop;
        s->replayCount++;
    } else {
        s->replayOverflow = 1;
    }
}

/* segments the frames kept during the learning phase again with STA's learned model, replacing the live
 * SEG (and CVD); returns the elements newest first (as sta_commit), classified */
static int replay_learning(Sig *s, Element **outp)
{
    Seg tmp;
    Element *acc = NULL;
    int n = 0, cap = 0, i, j;
    seg_init(&tmp, &s->sta);
    for (i = 1; i < SEG_ON_MAX + SEG_OFF_MAX; i++) tmp.durState[i].score = SEG_LOG_ZERO;   /* in a long gap */
    tmp.stepCounter = s->replayStep;
    for (i = 0; i < s->replayCount; i++) {
        tmp.carrierDrop = s->replayDrop[i];
        seg_update(&tmp, s->replayBit[i], &s->replayObs[i]);
        if (tmp.outCount > 0) {
            if (n + tmp.outCount > cap) {
                cap = 2 * (n + tmp.outCount) + 64;
                acc = realloc(acc, (size_t)cap * sizeof(Element));
            }
            for (j = tmp.outCount - 1; j >= 0; j--) acc[n++] = tmp.out[j];   /* batches are newest first */
        }
    }
    s->seg = tmp;
    if (CVD_TUNE) {
        cvd_set_model(s->cvd, &s->sta);
        cvd_reset(s->cvd, s->replayStep);
        for (i = 0; i < s->replayCount; i++) {
            cvd_update(s->cvd, &s->replayObs[i]);
            if (s->replayDrop[i] && i + 1 < s->replayCount) {
                cvd_commit(s->cvd, 1, &s->sta);
                cvd_collect(s);
                cvd_reset(s->cvd, s->cvd->step);
            }
        }
        s->cvdReplayed = 1;
    }
    for (i = 0; i < n / 2; i++) { Element t = acc[i]; acc[i] = acc[n - 1 - i]; acc[n - 1 - i] = t; }
    for (i = 0; i < n; i++) sta_classify(&s->sta, &acc[i]);
    *outp = acc ? acc : malloc(sizeof(Element));
    return n;
}

/* CVD for one frame outside STA's learning phase, holding its output during the start of an over */
static void cvd_frame(Sig *s, int carrier, int drop, int modelChanged)
{
    if (!s->cvdReplayed) cvd_update(s->cvd, &s->env.envObs);
    if (SIG_TUNE_OVER) {
        if (carrier && !s->prevCarrierCvd && !s->overActive) {          /* an over starts */
            s->overActive = 1;
            s->overCount = 0;
            s->overStep = s->cvd->step - 1;
            s->overWpm = sta_speed(&s->sta);
            spd_forget(&s->spd);
        }
        if (s->overActive) {
            if (s->overCount < SIG_REPLAY_MAX) s->replayObs[s->overCount++] = s->env.envObs;
            if (drop || modelChanged || s->overCount >= SIG_OVER_MAX) {
                double r = sta_speed(&s->sta) / s->overWpm;
                if ((r > 1.0 + SIG_OVER_SPEED || r < 1.0 / (1.0 + SIG_OVER_SPEED)) && s->overCount < SIG_REPLAY_MAX) {
                    int i;
                    cvd_reset(s->cvd, s->overStep);
                    for (i = 0; i < s->overCount; i++) cvd_update(s->cvd, &s->replayObs[i]);
                }
                s->overActive = 0;
            }
        }
    }
    s->prevCarrierCvd = carrier;
    if (s->overActive) return;
    cvd_commit(s->cvd, drop, &s->sta);
    cvd_collect(s);
    if (drop) cvd_reset(s->cvd, s->cvd->step);
}

static void hist_append(Sig *s, const Char *c, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (s->histCount == WRD_WINDOW_MAX) {
            memmove(s->hist, s->hist + 1, (WRD_WINDOW_MAX - 1) * sizeof(Char));
            s->histCount--;
        }
        s->hist[s->histCount++] = c[i];
    }
}

static void trace_frame(const Sig *s, const Element *out, int nout, int carrier)
{
    int i;
    fprintf(vail_trace, "%d ch=%d bin=%d floor=%.4g amp=%.3f prom=%.3f env=%.3f thr=%.3f kp=%.3f car=%d",
            s->seg.stepCounter, s->id, s->spm.spmCenterBin, s->spm.spmFloor, s->spm.spmPeakAmp,
            s->spm.spmProminence, s->env.envEnvelope, s->env.envThreshold, s->env.envKeyProb, carrier);
    if (ENV_TUNE_LIK && s->env.lik.t > LIK_DELAY) {
        const Lik *L = &s->env.lik;
        int o = (L->t - 1 - LIK_DELAY) & (LIK_RING - 1);
        fprintf(vail_trace, " y=%.3g broad=%.3g N=%.3g L=%.3g R=%.3g O=%.3g abs=%d x=%.2f edge=%.2f",
                L->x[o] / L->noise, L->broad[o] / L->noise, L->noise, L->noiseL, L->noiseR, L->noiseOwn,
                s->frameBase + s->seg.stepCounter, s->env.envObs.keyLogOdds, s->env.envObs.riseLogOdds);
    }
    if (nout) fprintf(vail_trace, " mu=%.2f,%.2f,%.2f,%.2f,%.2f L%d", s->sta.means[0], s->sta.means[1],
                      s->sta.means[2], s->sta.means[3], s->sta.means[4], s->sta.learningPhase);
    if (SPD_TUNE && s->seg.stepCounter % 25 == 0) {
        int kb = spd_best(&s->spd);
        fprintf(vail_trace, " spd=%.1f/%.1f m=%.1f", s->spd.wpm[kb], sta_speed(&s->sta),
                s->spd.score[kb] - s->spd.score[SPD_CUR]);
    }
    for (i = 0; i < nout; i++) fprintf(vail_trace, " [%d k%d]", out[i].durationSigned, out[i].kind);
    for (i = 0; i < s->chr.charCount; i++) fprintf(vail_trace, " '%c'", s->chr.chars[i].charCode);
    for (i = 0; i < s->cvdOutCount; i++) fprintf(vail_trace, " \"%c\"", s->cvdOut[i].charCode);
    fputc('\n', vail_trace);
}

void sig_process_frame(Sig *s, const float *spectrum, const float *peakRatio, float noiseFloor, float bandPeak,
                       Msg *msg)
{
    Element *out = NULL;
    int nout = 0, drop, carrier, committed = 0, flushed = 0, nchars, modelChanged = 0;
    const Char *chars;
    float wpmBefore = sta_speed(&s->sta);

    s->hasText = 0;
    spm_track_peak(&s->spm, peakRatio);
    spm_process_frame(&s->spm, spectrum, noiseFloor);
    if (ENV_TUNE_LIK) {
        int c = s->spm.spmCenterBin, d, k, n = 0;
        float nl, nr, broad, floorPower = bandPeak * LIK_DYN_RANGE, beside[2 * LIK_PEAK_SPAN];
        if (floorPower < VAIL_TUNE_POWER_FLOOR) floorPower = VAIL_TUNE_POWER_FLOOR;
        spm_noise_sides(spectrum, c, &nl, &nr, &broad);
        for (k = -LIK_PEAK_SPAN; k <= LIK_PEAK_SPAN; k++)
            if (k) beside[n++] = spectrum[c + k < 0 ? 0 : (c + k >= N_BINS ? N_BINS - 1 : c + k)];
        env_apply_lik(&s->env, spectrum[c], beside, nl, nr, broad, floorPower);
        d = env_peak_move(&s->env);              /* a weak signal beside the centre bin (SPM tracks strong ones) */
        if (d && peakRatio[c] < SPM_PEAK_MIN_POWER && c + d >= SPM_BIN_MIN && c + d <= N_BINS - 1 - SPM_BIN_MARGIN)
            s->spm.spmCenterBin = c + d;
    } else {
        env_apply(&s->env, s->spm.spmProminence, s->spm.spmPeakAmp);
    }

    carrier = s->env.envCarrier;
    if (SPD_TUNE && carrier) speed_check(s);
    drop = (s->prevCarrier && !carrier) || s->flushRequest;
    s->flushRequest = 0;
    s->prevCarrier = carrier;

    if (SIG_TUNE_REPLAY && s->sta.learningPhase) replay_store(s, carrier, drop);
    s->seg.carrierDrop = drop;
    seg_update(&s->seg, s->env.envEnvelope < SEG_SILENCE_LEVEL, &s->env.envObs);

    if (s->seg.outCount > 0) {
        nout = sta_commit(&s->sta, s->seg.out, s->seg.outCount, &out);
        committed = 1;
        flushed = s->sta.flushFlag;
    }
    if (drop && s->sta.learningPhase) {               /* tuning E8 */
        Element *all;
        int nall = sta_flush_priming(&s->sta, &all);
        if (nall > 0) { free(out); out = all; nout = nall; committed = flushed = 1; }
    }
    s->cvdOutCount = 0;
    s->cvdReplayed = 0;
    if (committed) {
        if (drop) sta_reset(&s->sta);
        if (s->sta.modelDirty) {
            seg_build_duration_tables(&s->seg);
            spd_set_current(&s->spd, s->seg.durState);
            if (CVD_TUNE) cvd_set_model(s->cvd, &s->sta);
        }
        if (flushed) { chr_clear_history(&s->chr); s->histCount = 0; }
        if (flushed && SIG_TUNE_REPLAY && s->replayCount > 0 && !s->replayOverflow) {
            free(out);
            nout = replay_learning(s, &out);
        }
    }
    if (!s->sta.learningPhase) s->replayCount = 0;
    modelChanged = sta_speed(&s->sta) != wpmBefore;

    if (CVD_TUNE) {
        s->chr.charCount = 0;
        if (!s->sta.learningPhase) {
            cvd_frame(s, carrier, drop, modelChanged);
        } else {
            if (!SIG_TUNE_REPLAY) cvd_update(s->cvd, &s->env.envObs);   /* else replayed when STA has learned */
            s->prevCarrierCvd = 1;             /* the first over is the learning phase's */
        }
        chars = s->cvdOut;
        nchars = s->cvdOutCount;
    } else {
        if (nout > 0 || drop) {
            int i;
            for (i = 0; i < nout / 2; i++) {           /* newest-first → ascending time */
                Element t = out[i]; out[i] = out[nout - 1 - i]; out[nout - 1 - i] = t;
            }
            chr_add_elements(&s->chr, out, nout, drop);
        } else {
            s->chr.charCount = 0;
        }
        chars = s->chr.chars;
        nchars = s->chr.charCount;
    }
    if (vail_elems) {
        int i;
        for (i = 0; i < nout; i++)
            fprintf(vail_elems, "%d %d %d %d %d\n", s->id, s->frameBase + out[i].elementRef,
                    out[i].durationSigned, out[i].kind, s->spm.spmCenterBin);
    }
    if (vail_trace) trace_frame(s, out, nout, carrier);
    free(out);

    if (!s->sta.learningPhase && (nchars > 0 || (drop && s->histCount > 0))) {
        Char batch[WRD_WINDOW_MAX];
        hist_append(s, chars, nchars);
        memcpy(batch, s->hist, (size_t)s->histCount * sizeof(Char));
        wrd_reset_batch(&s->wrd);
        wrd_process_chars(&s->wrd, batch, s->histCount, drop);
        memcpy(s->hist, batch, (size_t)s->histCount * sizeof(Char));
        msg_merge_chars(msg, batch, s->histCount);
        s->hasText = msg->newTailCount > 0;
    }

    s->signalPresent = carrier;
    s->quality = s->env.envStrength;
    s->segGate = s->sta.learningPhase;
}
