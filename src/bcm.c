/* BCM — band channel manager (03-band-channel-manager.md). */
#include <stdlib.h>
#include <string.h>
#include "vail_internal.h"

static int g_channelId = 0;

void bcm_init(Bcm *b)
{
    memset(b, 0, sizeof *b);
}

static void channel_free(Channel *ch)
{
    sig_free(ch->pipeline);
    free(ch->pipeline);
    free(ch->msgA);
    free(ch);
}

void bcm_free(Bcm *b)
{
    int i;
    for (i = 0; i < b->nChannels; i++) channel_free(b->channels[i]);
    free(b->channels);
    b->channels = NULL;
    b->nChannels = 0;
}

int bcm_is_decoding(const Channel *ch)
{
    return ch->state == CH_STATE_DECODING && ch->ageCounter < BCM_ISDECODING_MAX_AGE;
}

void bcm_set_current(Bcm *b, Channel *ch)                  /* 03 §4.10 SetCurrentChannel */
{
    if (b->currentChannel == ch) return;
    if (b->currentChannel) {
        b->currentChannel->isCurrent = 0;
        b->currentChannel->pipeline->flushRequest = 1;
    }
    b->currentChannel = ch;
    if (ch) ch->isCurrent = 1;
}

Channel *bcm_new_channel(Bcm *b, int bin)                  /* 03 §4.6 new-channel initialisation */
{
    Channel *ch = calloc(1, sizeof *ch);
    int first;
    ch->pipeline = malloc(sizeof(Sig));
    sig_init(ch->pipeline, bin);
    ch->msgA = malloc(sizeof(Msg));
    msg_init(ch->msgA);
    ch->bin = bin;
    ch->id = ++g_channelId;
    first = b->frameCount - BCM_REPLAY_INIT;
    if (first < 1) first = 1;
    ch->nextFrame = first;
    ch->replayCounter = b->frameCount - first;
    ch->pipeline->frameBase = first - LIK_DELAY;            /* both ENVs output LIK_DELAY frames late */
    ch->pipeline->id = ch->id;
    if (b->nChannels == b->capChannels) {
        b->capChannels = b->capChannels ? b->capChannels * 2 : 16;
        b->channels = realloc(b->channels, (size_t)b->capChannels * sizeof(Channel *));
    }
    b->channels[b->nChannels++] = ch;
    return ch;
}

static void kill(Channel *ch) { ch->state = CH_STATE_DEAD; ch->ageCounter = 0; }

static void merge_by_bin_distance(Bcm *b)                  /* 03 §4.7, effective rule of A4 */
{
    int i;
    if (b->nChannels < 2) return;
    for (i = b->nChannels - 2; i >= 0; i--) {
        Channel *a = b->channels[i], *c = b->channels[i + 1];
        int d;
        if (a->state >= CH_STATE_PAUSED || c->state >= CH_STATE_PAUSED) continue;
        d = c->bin - a->bin; if (d < 0) d = -d;
        if (d > BCM_MERGE_BIN_MAX_DIST) continue;
        if (a->isCurrent) { kill(c); continue; }
        if (c->isCurrent) { kill(a); continue; }
        if (a->pipeline->signalPresent && !c->pipeline->signalPresent) { kill(c); continue; }
        if (c->pipeline->signalPresent && !a->pipeline->signalPresent) { kill(a); continue; }
        if (a->msgA->count > c->msgA->count) kill(c);
        else kill(a);
    }
    /* MergeByText (03 §4.8) is inert in this system (A4): no text pointer is ever set. */
}

static void update_peak_map(Bcm *b)                        /* 03 §4.2–§4.4 */
{
    const float *in = b->inputSpectrum;
    int i, hiIdx = N_BINS - 1 - 10;
    for (i = 0; i < N_BINS; i++) {
        if (in[i] >= BCM_SPEC_GATE)
            b->smoothSpectrum[i] = (float)(BCM_SPEC_IIR_OLD * b->smoothSpectrum[i] + BCM_SPEC_IIR_NEW * in[i]);
        else
            b->smoothSpectrum[i] = 0.0f;
    }
    for (i = BCM_PEAK_SCAN_LO; i <= hiIdx; i++) {
        float mx = b->smoothSpectrum[i + BCM_RATIO_NEIGH] > b->smoothSpectrum[i - BCM_RATIO_NEIGH]
                   ? b->smoothSpectrum[i + BCM_RATIO_NEIGH] : b->smoothSpectrum[i - BCM_RATIO_NEIGH];
        if (mx < VAIL_TUNE_POWER_FLOOR) mx = VAIL_TUNE_POWER_FLOOR;                    /* tuning E4 */
        if (mx <= 0.0f) b->peakRatio[i] = 0.0f;
        else {
            double r = BCM_RATIO_MAXFACTOR * b->smoothSpectrum[i] / mx;
            b->peakRatio[i] = (float)(BCM_RATIO_IIR_OLD * b->peakRatio[i] + BCM_RATIO_IIR_NEW * r);
        }
    }
    if (b->frameCount > BCM_PEAK_WARMUP) {
        for (i = BCM_PEAK_SCAN_LO; i <= hiIdx; i++) {
            float p = b->peakRatio[i];
            if (p > BCM_PEAK_THRESH && p >= b->peakRatio[i - 1] && p >= b->peakRatio[i + 1]) {
                b->peakFlags[i - 1] = 0;
                b->peakFlags[i] = 1;
                b->peakFlags[i + 1] = 0;
            } else {
                b->peakFlags[i] = 0;
            }
        }
    }
}

static void mask(Bcm *b, int bin)
{
    int k;
    for (k = bin - BCM_MASK_RADIUS; k <= bin + BCM_MASK_RADIUS; k++)
        if (k >= 0 && k < N_BINS) b->peakFlags[k] = 0;
}

static void update_channels(Bcm *b)                        /* 03 §4.6 */
{
    int i, hiIdx = N_BINS - 1 - 10;
    for (i = b->nChannels - 1; i >= 0; i--) {
        if (b->channels[i]->state == CH_STATE_DEAD) {
            Channel *ch = b->channels[i];
            if (ch == b->currentChannel) bcm_set_current(b, NULL);
            memmove(b->channels + i, b->channels + i + 1, (size_t)(b->nChannels - i - 1) * sizeof(Channel *));
            b->nChannels--;
            channel_free(ch);
        }
    }
    for (i = 0; i < b->nChannels; i++) mask(b, b->channels[i]->bin);
    for (i = BCM_PEAK_SCAN_LO; i <= hiIdx; i++) {
        if (b->peakFlags[i]) {
            mask(b, i);
            bcm_new_channel(b, i);
        }
    }
}

static void select_current(Bcm *b)                         /* 03 §4.10 */
{
    Channel *best = NULL;
    float bestQ = BCM_QUALITY_SENTINEL;
    int i;
    if (b->currentChannel && b->currentChannel->pinnedFlag) return;
    for (i = 0; i < b->nChannels; i++) {
        Channel *ch = b->channels[i];
        float q = ch->pipeline->quality;
        if (BCM_TUNE_TEXT_RATE && ch->textRate < BCM_MIN_TEXT_RATE) continue;
        if (bcm_is_decoding(ch) && q > bestQ) { bestQ = q; best = ch; }
    }
    if (!best) return;
    if (!b->currentChannel) bcm_set_current(b, best);
    else if (bestQ > BCM_SWITCH_HYST * b->currentChannel->pipeline->quality) bcm_set_current(b, best);
}

static int cmpf(const void *x, const void *y)
{
    float a = *(const float *)x, c = *(const float *)y;
    return (a > c) - (a < c);
}

static float median_mid_slice(const float *in)             /* 03 §4.5 */
{
    float slice[BCM_NOISE_SLICE];
    int k0 = (N_BINS - BCM_NOISE_SLICE) / 2;
    memcpy(slice, in + k0, sizeof slice);
    qsort(slice, BCM_NOISE_SLICE, sizeof(float), cmpf);
    return slice[BCM_NOISE_SLICE / 2];
}

static void state_machine(Channel *ch)                     /* 03 §4.9 */
{
    Sig *s = ch->pipeline;
    int sig = s->signalPresent, pin = ch->pinnedFlag, ready = (s->segGate == 0);
    switch (ch->state) {
    case CH_STATE_NEW:
        if (ready) { ch->state = CH_STATE_DECODING; ch->ageCounter = 0; }
        else if (sig == ch->lastSignal) {
            ch->ageCounter++;
            if (ch->ageCounter > BCM_STALL_NEW && !pin) {
                if (ch->lastSignal) { ch->state = CH_STATE_PAUSED; ch->ageCounter = 0; }
                else ch->state = CH_STATE_DEAD;
            }
        } else { ch->lastSignal = sig; ch->ageCounter = 0; }
        break;
    case CH_STATE_DECODING:
        if (s->hasText) ch->ageCounter = 0;
        else {
            ch->ageCounter++;
            if (ch->ageCounter > BCM_STALL_DECODING && !pin) ch->state = CH_STATE_DEAD;
        }
        break;
    case CH_STATE_PAUSED:
        if (pin) { ch->state = CH_STATE_DECODING; ch->ageCounter = 0; }
        else if (sig) ch->ageCounter = 0;
        else {
            ch->ageCounter++;
            if (ch->ageCounter > BCM_STALL_PAUSED) ch->state = CH_STATE_DEAD;
        }
        break;
    default:
        break;
    }
}

static void channel_frame(Bcm *b, Channel *ch, const float *spectrum)   /* 03 §4.11 ChannelFrame */
{
    Msg *m = ch->msgA;
    sig_process_frame(ch->pipeline, spectrum, b->peakRatio, b->noiseFloor, b->bandPeak, m);
    ch->bin = ch->pipeline->spm.spmCenterBin;
    ch->textRate = ch->textRate * BCM_TEXT_DECAY + (ch->pipeline->hasText ? (float)m->newTailCount : 0.0f);
    if (ch->pipeline->hasText) {
        int i, room, n, d = 0;
        for (i = 0; i < m->replacedCount; i++) ch->frameReplaced += msg_rendered_len(&m->replaced[i]);
        /* compose "delete d, append newTail" into this frame's single edit */
        for (i = m->veq; i < m->replacedCount; i++) d += msg_rendered_len(&m->replaced[i]);
        while (d > 0 && ch->frameNewCount > 0) d -= msg_rendered_len(&ch->frameNew[--ch->frameNewCount]);
        ch->frameDelete += d > 0 ? d : 0;
        room = MSG_CAPACITY - ch->frameNewCount;
        n = m->newTailCount < room ? m->newTailCount : room;
        memcpy(ch->frameNew + ch->frameNewCount, m->newTail, (size_t)n * sizeof(Char));
        ch->frameNewCount += n;
    }
    state_machine(ch);
}

static void feed_channel(Bcm *b, Channel *ch)              /* 03 §4.11, contiguous catch-up (A11) */
{
    int n = 0;
    while (ch->nextFrame <= b->frameCount && n < BCM_REPLAY_BATCH) {
        const float *sp = (ch->nextFrame == b->frameCount)
                          ? b->inputSpectrum : b->historyRing[ch->nextFrame % BCM_HISTORY_FRAMES];
        channel_frame(b, ch, sp);
        ch->nextFrame++;
        n++;
    }
    ch->replayCounter = b->frameCount - ch->nextFrame + 1;
}

static int cmp_bin(const void *x, const void *y)
{
    const Channel *a = *(Channel *const *)x, *c = *(Channel *const *)y;
    return a->bin - c->bin;
}

void bcm_on_frame(Bcm *b, const float *spectrum)           /* 03 §2.3 */
{
    int i, periodic;
    b->frameCount++;
    periodic = (b->frameCount % BCM_MERGE_PERIOD) == 0;
    if (periodic) merge_by_bin_distance(b);
    b->inputSpectrum = spectrum;
    memcpy(b->historyRing[b->frameCount % BCM_HISTORY_FRAMES], spectrum, N_BINS * sizeof(float));
    b->bandPeak *= BCM_PEAK_DECAY;
    for (i = 0; i < N_BINS; i++) if (spectrum[i] > b->bandPeak) b->bandPeak = spectrum[i];
    update_peak_map(b);
    update_channels(b);
    select_current(b);
    if (periodic) b->noiseFloor = median_mid_slice(b->inputSpectrum);
    for (i = 0; i < b->nChannels; i++) feed_channel(b, b->channels[i]);
    if (periodic && b->nChannels > 1) qsort(b->channels, (size_t)b->nChannels, sizeof(Channel *), cmp_bin);
}
