/* API — host interface (09-host-interface.md) and host-side message assembly (08 §4.2–§4.6). */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "vail_internal.h"

#define API_PITCH_ORIGIN_HZ   200
#define API_PITCH_BIN_HZ      7.8125
#define API_PITCH_BIN_MIN     10
#define API_PITCH_BIN_MAX     (N_BINS - 11)
#define API_PITCH_LOCK_RADIUS 10
#define API_SYNTH_NEWLINE     10
#define API_SYNTH_MARK        62
#ifndef API_TUNE_EXACT_DELETE
#define API_TUNE_EXACT_DELETE 1   /* tuning E7 (12-open-items.md §E) */
#endif

struct vail_api {
    Bcm bcm;
    vail_char *list; int listCount, listCap;
    int deleteCount, pending, mode;
    int lastCurrentId;
    int channelChanged;
};

vail_api *API_New(void)
{
    vail_api *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    bcm_init(&h->bcm);
    return h;
}

void API_Free(vail_api *h)
{
    if (!h) return;
    bcm_free(&h->bcm);
    free(h->list);
    free(h);
}

static void list_add(vail_api *h, uint8_t ch, int32_t wordType, uint8_t lowSnr)
{
    if (h->listCount == h->listCap) {
        h->listCap = h->listCap ? h->listCap * 2 : 64;
        h->list = realloc(h->list, (size_t)h->listCap * sizeof(vail_char));
    }
    h->list[h->listCount].ch = ch;
    h->list[h->listCount].wordType = wordType;
    h->list[h->listCount].lowSnr = lowSnr;
    h->listCount++;
}

static void append_emitted(vail_api *h, const Char *recs, int n)     /* 08 §4.3 */
{
    int base = h->listCount, i;
    for (i = 0; i < n; i++) {
        list_add(h, recs[i].charCode, recs[i].wordType, recs[i].lowSnr);
        if (recs[i].wordEnd == WE_SHORT) list_add(h, ' ', WT_GENERIC, 0);
        else if (recs[i].wordEnd == WE_LONG) { list_add(h, ' ', WT_GENERIC, 0); list_add(h, ' ', WT_GENERIC, 0); }
    }
    h->pending += h->listCount - base;
}

int API_Feed(vail_api *h, float *s)                        /* 09 §2.3 */
{
    Channel *cur;
    int i;
    for (i = 0; i < N_BINS; i++)
        if (!isfinite(s[i])) s[i] = 0.0f;
    bcm_on_frame(&h->bcm, s);

    h->deleteCount = 0;                                    /* FrameReset (08 §4.6) */
    h->listCount = 0;
    cur = h->bcm.currentChannel;
    if ((cur ? cur->id : 0) != h->lastCurrentId) {
        if (h->lastCurrentId != 0 && cur) {
            list_add(h, API_SYNTH_NEWLINE, WT_GENERIC, 0);
            list_add(h, API_SYNTH_MARK, WT_GENERIC, 0);
        }
        h->pending = 0;                                    /* OnChannelChanged */
        h->channelChanged = 1;
        h->lastCurrentId = cur ? cur->id : 0;
    }
    if (cur) {
        Msg *m = cur->msgA;
        if (h->channelChanged) {                           /* SelectEmitted (08 §4.2) */
            Char tmp[MSG_CAPACITY];
            int n = msg_chars_since(m, m->lastIdxE, tmp);
            append_emitted(h, tmp, n);
            if (n > 0) m->lastIdxE = tmp[n - 1].elementOrdinal;
            h->channelChanged = 0;
        } else {
            if (h->mode && cur->frameReplaced > 0) {       /* RecomputeDelete (08 §4.4) */
                int d = API_TUNE_EXACT_DELETE ? cur->frameDelete : cur->frameReplaced + 1;
                if (!API_TUNE_EXACT_DELETE) append_emitted(h, cur->frameNew, cur->frameNewCount);
                if (d > h->pending) d = h->pending;
                h->deleteCount = d;
                h->pending -= d;
                if (!API_TUNE_EXACT_DELETE) cur->frameNewCount = 0;
            }
            if (cur->frameNewCount > 0) {
                append_emitted(h, cur->frameNew, cur->frameNewCount);
                m->lastIdxE = cur->frameNew[cur->frameNewCount - 1].elementOrdinal;
            }
        }
    }
    for (i = 0; i < h->bcm.nChannels; i++) {
        Channel *ch = h->bcm.channels[i];
        ch->frameNewCount = 0;
        ch->frameReplaced = 0;
        ch->frameDelete = 0;
    }
    return API_State(h);
}

int API_State(const vail_api *h)                           /* 09 §2.4 */
{
    const Channel *c = h->bcm.currentChannel;
    int decoding;
    if (!c) return API_STATE_NONE;
    decoding = bcm_is_decoding(c);
    if (c->pinnedFlag) return decoding ? API_STATE_LOCKED_DECODING : API_STATE_LOCKED_PAUSE;
    return decoding ? API_STATE_DECODING : API_STATE_PAUSE;
}

int API_Pitch(const vail_api *h)
{
    const Channel *c = h->bcm.currentChannel;
    if (!c) return API_NOT_AVAILABLE;
    return API_PITCH_ORIGIN_HZ + (int)nearbyint(c->bin * API_PITCH_BIN_HZ);
}

void API_PitchLock(vail_api *h, int hz)                    /* 09 §2.6 */
{
    Bcm *b = &h->bcm;
    Channel *c = b->currentChannel;
    int bin = (int)nearbyint((hz - API_PITCH_ORIGIN_HZ) / API_PITCH_BIN_HZ);
    if (bin < API_PITCH_BIN_MIN) bin = API_PITCH_BIN_MIN;
    if (bin > API_PITCH_BIN_MAX) bin = API_PITCH_BIN_MAX;
    if (!c || !c->pinnedFlag) {
        Channel *best = NULL;
        int bestD = API_PITCH_LOCK_RADIUS + 1, i;
        for (i = 0; i < b->nChannels; i++) {
            int d = abs(b->channels[i]->bin - bin);
            if (b->channels[i]->state != CH_STATE_DEAD && d < bestD) { bestD = d; best = b->channels[i]; }
        }
        if (!best) best = bcm_new_channel(b, bin);
        bcm_set_current(b, best);
        best->pinnedFlag = 1;
    } else {
        c->pinnedFlag = 0;
    }
}

int API_Snr(const vail_api *h)                             /* 09 §2.7 */
{
    const Channel *c = h->bcm.currentChannel;
    double x;
    if (!c) return API_NOT_AVAILABLE;
    x = c->pipeline->env.envMeanSquare;
    if (x < 1.0) x = 1.0;
    return (int)nearbyint(10.0 * log10(x)) - 10;
}

int API_Wpm(const vail_api *h)                             /* 09 §2.8 */
{
    const Channel *c = h->bcm.currentChannel;
    if (!c) return API_NOT_AVAILABLE;
    return (int)nearbyint(sta_speed(&c->pipeline->sta));
}

int API_ChannelCount(const vail_api *h)                    /* 09 §2.9 */
{
    int i, n = 0;
    if (!h->bcm.currentChannel) return API_NOT_AVAILABLE;
    for (i = 0; i < h->bcm.nChannels; i++) n += h->bcm.channels[i]->pipeline->signalPresent != 0;
    return n;
}

int API_CharCount(const vail_api *h) { return h->listCount; }

vail_char API_CharAt(const vail_api *h, int i)
{
    vail_char none = { '?', 0, 0 };
    if (i < 0 || i >= h->listCount) return none;          /* host error (12-open-items C4) */
    return h->list[i];
}

int API_DeleteCount(const vail_api *h) { return h->mode ? h->deleteCount : 0; }
int API_ModeGet(const vail_api *h) { return h->mode & 1; }
void API_ModeSet(vail_api *h, int v) { h->mode = v != 0; }

int vail_current_text(const vail_api *h, char *buf, int cap)
{
    if (!h->bcm.currentChannel) { if (cap > 0) buf[0] = 0; return 0; }
    return msg_render(h->bcm.currentChannel->msgA, buf, cap);
}

int vail_channel_count_all(const vail_api *h) { return h->bcm.nChannels; }

int vail_channel_text(const vail_api *h, int idx, char *buf, int cap, int *pitchHz, int *id)
{
    const Channel *c;
    if (idx < 0 || idx >= h->bcm.nChannels) return -1;
    c = h->bcm.channels[idx];
    if (id) *id = c->id;
    if (pitchHz) *pitchHz = API_PITCH_ORIGIN_HZ + (int)nearbyint(c->bin * API_PITCH_BIN_HZ);
    return msg_render(c->msgA, buf, cap);
}
