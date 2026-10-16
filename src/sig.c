/* SIG — per-signal pipeline aggregate: SPM → ENV → SEG → STA → CHR → WRD → MSG.
 * The glue rules (carrier drop, gate, quality, text flag, WRD batching) follow 03 §2.1,
 * 05 §0 and the defaults recorded in 12-open-items.md (A12, A13, D6, E8). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vail_internal.h"

FILE *vail_trace = NULL;

void sig_init(Sig *s, int bin)
{
    memset(s, 0, sizeof *s);
    spm_init(&s->spm, bin);
    env_init(&s->env);
    sta_init(&s->sta);
    seg_init(&s->seg, &s->sta);
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

void sig_process_frame(Sig *s, const float *spectrum, const float *peakRatio, float noiseFloor, Msg *msg)
{
    Element *out = NULL;
    int nout = 0, drop, carrier, committed = 0, flushed = 0;

    s->hasText = 0;
    spm_track_peak(&s->spm, peakRatio);
    spm_process_frame(&s->spm, spectrum, noiseFloor);
    env_apply(&s->env, s->spm.spmProminence, s->spm.spmPeakAmp);

    carrier = s->env.envCarrier;
    drop = (s->prevCarrier && !carrier) || s->flushRequest;
    s->flushRequest = 0;
    s->prevCarrier = carrier;

    s->seg.carrierDrop = drop;
    seg_update(&s->seg, s->env.envEnvelope < SEG_SILENCE_LEVEL, s->env.envKeyProb);

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
    if (committed) {
        if (drop) sta_reset(&s->sta);
        if (s->sta.modelDirty) seg_build_duration_tables(&s->seg);
        if (flushed) { chr_clear_history(&s->chr); s->histCount = 0; }
    }
    if (nout > 0 || drop) {
        int i;
        for (i = 0; i < nout / 2; i++) {               /* newest-first → ascending time */
            Element t = out[i]; out[i] = out[nout - 1 - i]; out[nout - 1 - i] = t;
        }
        chr_add_elements(&s->chr, out, nout, drop);
    } else {
        s->chr.charCount = 0;
    }
    if (vail_trace) {
        int i;
        fprintf(vail_trace, "%d bin=%d floor=%.4g amp=%.3f prom=%.3f env=%.3f thr=%.3f kp=%.3f car=%d",
                s->seg.stepCounter, s->spm.spmCenterBin, s->spm.spmFloor, s->spm.spmPeakAmp,
                s->spm.spmProminence, s->env.envEnvelope, s->env.envThreshold, s->env.envKeyProb, carrier);
        if (nout) fprintf(vail_trace, " mu=%.2f,%.2f,%.2f,%.2f,%.2f L%d", s->sta.means[0], s->sta.means[1], s->sta.means[2], s->sta.means[3], s->sta.means[4], s->sta.learningPhase);
        for (i = 0; i < nout; i++) fprintf(vail_trace, " [%d k%d]", out[i].durationSigned, out[i].kind);
        for (i = 0; i < s->chr.charCount; i++) fprintf(vail_trace, " '%c'", s->chr.chars[i].charCode);
        fputc('\n', vail_trace);
    }
    free(out);

    if (!s->sta.learningPhase && (s->chr.charCount > 0 || (drop && s->histCount > 0))) {
        Char batch[WRD_WINDOW_MAX];
        hist_append(s, s->chr.chars, s->chr.charCount);
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
