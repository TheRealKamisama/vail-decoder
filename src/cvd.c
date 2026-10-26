/* CVD — character Viterbi decoder (see vail_internal.h). */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "vail_internal.h"
#include "lm_table.h"

#define NEG      (-1e30f)
#define CAP      400            /* durations (frames) over which the class pmfs are normalised */
#define HAZ_MIN  0.002
#define HAZ_MAX  0.999

/* ---- the code trie, built once from the CHR table */
typedef struct {
    int16_t child[2];           /* dot, dash; -1 if none */
    int16_t parent;
    int8_t elem;                /* element leading here: 0 dot, 1 dash (-1 at the root) */
    int16_t chr;                /* table index of the code ending here, -1 if none */
    float logEnd;               /* log P(code ends here | reached here) */
    float logCont;              /* log P(another element follows | reached here) */
    float logBranch[2];         /* log P(that element is a dot / a dash | another follows) */
} TrieNode;

static TrieNode g_trie[CVD_MAX_NODES];
static int g_nTrie = 0;
static int8_t g_lmSym[CHR_TABLE_LEN];        /* bigram symbol of each table entry, -1 for prosigns */
static float g_lmBase[CHR_TABLE_LEN];        /* log of its symbol's share of the table weight */

static double trie_mass(int n, const double *w)
{
    double m = w[n];
    int e;
    for (e = 0; e < 2; e++) if (g_trie[n].child[e] >= 0) m += trie_mass(g_trie[n].child[e], w);
    return m;
}

static void build_trie(void)
{
    double w[CVD_MAX_NODES] = { 0 }, mass[CVD_MAX_NODES];
    int i, n;
    g_nTrie = 1;
    g_trie[0].child[0] = g_trie[0].child[1] = -1;
    g_trie[0].parent = -1;
    g_trie[0].elem = -1;
    g_trie[0].chr = -1;
    for (i = 0; i < chr_table_size(); i++) {
        const char *p = chr_table_pattern(i);
        int cur = 0;
        for (; *p; p++) {
            int e = *p == '-';
            if (g_trie[cur].child[e] < 0) {
                if (g_nTrie == CVD_MAX_NODES) break;
                n = g_nTrie++;
                g_trie[n].child[0] = g_trie[n].child[1] = -1;
                g_trie[n].parent = (int16_t)cur;
                g_trie[n].elem = (int8_t)e;
                g_trie[n].chr = -1;
                g_trie[cur].child[e] = (int16_t)n;
            }
            cur = g_trie[cur].child[e];
        }
        g_trie[cur].chr = (int16_t)i;
        w[cur] += chr_table_weight(i);
    }
    for (i = 0; i < chr_table_size(); i++) {
        const char *o = chr_table_out(i), *q = o[0] && !o[1] ? strchr(LM_SYMS, o[0]) : NULL;
        double ws = 0.0, wt = 0.0;
        int j;
        g_lmSym[i] = (int8_t)(q ? q - LM_SYMS : -1);
        for (j = 0; j < chr_table_size(); j++) {
            wt += chr_table_weight(j);
            if (!strcmp(chr_table_out(j), o)) ws += chr_table_weight(j);
        }
        g_lmBase[i] = (float)log(ws / wt);
    }
    for (n = 0; n < g_nTrie; n++) mass[n] = trie_mass(n, w);
    for (n = 0; n < g_nTrie; n++) {
        double cont = mass[n] - w[n];
        int e;
        g_trie[n].logEnd = (float)(g_trie[n].chr >= 0 ? log(w[n] / mass[n]) : NEG);
        g_trie[n].logCont = (float)(cont > 0.0 ? log(cont / mass[n]) : NEG);
        for (e = 0; e < 2; e++)
            g_trie[n].logBranch[e] = (float)(g_trie[n].child[e] >= 0 && cont > 0.0
                                             ? log(mass[g_trie[n].child[e]] / cont) : NEG);
    }
}

/* ---- duration hazards from STA's class scores */
static void hazards(const double *q, int nmax, float *lend, float *lcont)
{
    double surv = 1.0, h, tail = 0.0, mrl = 0.0;
    int i;
    for (i = 1; i < nmax; i++) {
        h = surv > 1e-12 ? q[i] / surv : HAZ_MAX;
        if (h < HAZ_MIN) h = HAZ_MIN;
        if (h > HAZ_MAX) h = HAZ_MAX;
        lend[i] = (float)log(h);
        lcont[i] = (float)log(1.0 - h);
        surv -= q[i];
    }
    for (i = nmax; i <= CAP; i++) { tail += q[i]; mrl += (i - nmax + 1) * q[i]; }
    h = (tail > 1e-12 && mrl > 0.0) ? tail / mrl : HAZ_MAX;
    if (h < HAZ_MIN) h = HAZ_MIN;
    if (h > HAZ_MAX) h = HAZ_MAX;
    lend[nmax] = (float)log(h);
    lcont[nmax] = (float)log(1.0 - h);
}

/* pmf over 1..CAP frames of the sum of STA classes c0..c1 */
static void class_pmf(const Sta *sta, int c0, int c1, double *q)
{
    double z = 0.0;
    int i, c;
    for (i = 1; i <= CAP; i++) {
        q[i] = 0.0;
        for (c = c0; c <= c1; c++) q[i] += sta_score_of(sta, c, (float)i);
        z += q[i];
    }
    for (i = 1; i <= CAP; i++) q[i] /= z;
}

void cvd_set_model(Cvd *v, const Sta *sta)
{
    double q[CAP + 1];
    int i;
    for (i = 1; i <= CVD_WP_MAX; i++) {                 /* P(word gap | gap length), as CHR's wordSpaceProb */
        double s3 = sta_score_of(sta, 3, (float)i), s4 = sta_score_of(sta, 4, (float)i);
        v->wordProb[i] = (float)(s4 / (s3 + s4 + 1e-30));
    }
    v->wordProb[0] = v->wordProb[1];
    class_pmf(sta, 0, 0, q); hazards(q, CVD_MARK_MAX, v->markEnd[0], v->markCont[0]);
    class_pmf(sta, 1, 1, q); hazards(q, CVD_MARK_MAX, v->markEnd[1], v->markCont[1]);
    class_pmf(sta, 2, 2, q); hazards(q, CVD_GAP_MAX, v->gapEnd, v->gapCont);
    class_pmf(sta, 3, 4, q); hazards(q, CVD_CGAP_MAX, v->cgapEnd, v->cgapCont);
}

/* ---- history pool */
static void pool_clear(Cvd *v)
{
    int i;
    for (i = 0; i < CVD_POOL; i++) v->pool[i].parent = i + 1 < CVD_POOL ? i + 1 : -1;
    v->freeHead = 0;
    v->freeCount = CVD_POOL;
}

#define KEEP_FINAL 4            /* events kept beyond the commit point: two codes and gaps of n-gram context */

static void mark_chain(Cvd *v, int32_t h)
{
    int final = 0;
    while (h >= 0 && !v->pool[h].mark) {
        v->pool[h].mark = 1;
        if (v->pool[h].step <= v->committedStep && ++final >= KEEP_FINAL) break;   /* older history is final */
        h = v->pool[h].parent;
    }
}

/* mark-and-sweep over the histories held by the states; a chain end whose parent is collected loses it */
static void pool_gc(Cvd *v)
{
    int i, d, n;
    for (i = 0; i < CVD_POOL; i++) v->pool[i].mark = 0;
    for (n = 1; n < g_nTrie; n++) {
        for (d = 1; d <= CVD_MARK_MAX; d++) mark_chain(v, v->markH[n][d]);
        for (d = 1; d <= CVD_GAP_MAX; d++) mark_chain(v, v->gapH[n][d]);
    }
    for (d = 1; d <= CVD_CGAP_MAX; d++) mark_chain(v, v->cgapH[d]);
    v->freeHead = -1;
    for (i = CVD_POOL - 1; i >= 0; i--) {
        int32_t p = v->pool[i].parent;
        if (v->pool[i].mark && p >= 0 && !v->pool[p].mark) v->pool[i].parent = -1;
    }
    v->freeCount = 0;
    for (i = CVD_POOL - 1; i >= 0; i--)
        if (!v->pool[i].mark) { v->pool[i].parent = v->freeHead; v->freeHead = i; v->freeCount++; }
}

/* (collection runs at the start of a frame, never here: live histories may sit in locals) */
static int32_t node_new(Cvd *v, int32_t parent, int isGap, int value)
{
    int32_t i;
    if (v->freeHead < 0) return parent;                       /* pool exhausted: drop this event */
    i = v->freeHead;
    v->freeHead = v->pool[i].parent;
    v->freeCount--;
    v->pool[i].parent = parent;
    v->pool[i].step = v->step;
    v->pool[i].value = (int16_t)value;
    v->pool[i].isGap = (uint8_t)isGap;
    v->pool[i].mark = 0;
    return i;
}

/* ---- decoding */
void cvd_reset(Cvd *v, int step)
{
    int n, d;
    for (n = 0; n < CVD_MAX_NODES; n++) {
        for (d = 0; d <= CVD_MARK_MAX; d++) { v->mark[n][d] = NEG; v->markH[n][d] = -1; }
        for (d = 0; d <= CVD_GAP_MAX; d++) { v->gap[n][d] = NEG; v->gapH[n][d] = -1; }
    }
    for (d = 0; d <= CVD_CGAP_MAX; d++) { v->cgap[d] = NEG; v->cgapH[d] = -1; }
    v->cgap[CVD_CGAP_MAX] = 0.0f;
    pool_clear(v);
    v->step = step;
    v->committedStep = step - 1;
    v->outCount = 0;
}

void cvd_init(Cvd *v)
{
    if (!g_nTrie) build_trie();
    memset(v, 0, sizeof *v);
    cvd_reset(v, 0);
}

static double tri(int a, int b, int c)
{
    return exp(LM_TRIGRAM[a * (LM_NSYM + 1) + b][c] / 16.0);
}

/* symbol of the code before history node h (a gap node) and the probability that the gap was a word
 * gap; returns the code's node or -1 */
static int32_t prev_code(const Cvd *v, int32_t h, int *sym, double *word)
{
    const CvdNode *g, *c;
    int d;
    *sym = LM_SPACE;
    *word = 1.0;
    if (h < 0 || !v->pool[h].isGap || v->pool[h].parent < 0) return -1;
    g = &v->pool[h];
    c = &v->pool[g->parent];
    if (c->isGap || g_lmSym[c->value] < 0) return -1;
    d = g->step - c->step;
    *sym = g_lmSym[c->value];
    *word = v->wordProb[d < 0 ? 0 : (d > CVD_WP_MAX ? CVD_WP_MAX : d)];
    return g->parent;
}

/* log P(code | the two codes before it and the gaps between) - log P(code) under the table weights:
 * the character n-gram replaces the table's unigram; each gap is a word gap with the probability its
 * length gives */
static float context_gain(const Cvd *v, int chr, int32_t h)
{
    int sym = g_lmSym[chr], s1, s2;
    double w0, w1, p;
    int32_t c1;
    if (sym < 0) return 0.0f;
    c1 = prev_code(v, h, &s1, &w0);
    if (CVD_LM_ORDER < 3) {
        p = w0 * exp(LM_BIGRAM[LM_SPACE][sym]) + (1.0 - w0) * exp(LM_BIGRAM[s1][sym]);
        return (float)(log(p) - g_lmBase[chr]);
    }
    if (c1 < 0) { s2 = LM_SPACE; w1 = 1.0; }
    else prev_code(v, v->pool[c1].parent, &s2, &w1);
    p = w0 * tri(s1, LM_SPACE, sym)
        + (1.0 - w0) * (w1 * tri(LM_SPACE, s1, sym) + (1.0 - w1) * tri(s2, s1, sym));
    return (float)(log(p) - g_lmBase[chr]);
}

void cvd_update(Cvd *v, const SegObs *o)
{
    const float x = o->keyLogOdds, r = o->riseLogOdds, f = o->fallLogOdds, lm = CVD_LM_WEIGHT;
    float endMark[CVD_MAX_NODES], endGap[CVD_MAX_NODES], bestChar = NEG, endC = NEG, a, b, top;
    int32_t endMarkH[CVD_MAX_NODES], endGapH[CVD_MAX_NODES], bestCharH = -1, endCH = -1;
    int n, d, bestCharNode = -1;

    if (v->freeCount < 8) pool_gc(v);
    /* 1. run ends, from the previous frame's scores */
    for (n = 1; n < g_nTrie; n++) {
        const TrieNode *t = &g_trie[n];
        const float *le = v->markEnd[t->elem];
        float bm = NEG;
        int32_t bh = -1;
        for (d = 1; d <= CVD_MARK_MAX; d++)
            if (v->mark[n][d] + le[d] > bm) { bm = v->mark[n][d] + le[d]; bh = v->markH[n][d]; }
        endMark[n] = bm;
        endMarkH[n] = bh;
        if (t->chr >= 0 && bm > NEG / 2) {
            float sc = bm + lm * t->logEnd + CVD_LM_BIGRAM * context_gain(v, t->chr, bh);
            if (sc > bestChar) { bestChar = sc; bestCharH = bh; bestCharNode = n; }
        }
        bm = NEG;
        bh = -1;
        if (t->child[0] >= 0 || t->child[1] >= 0)
            for (d = 1; d <= CVD_GAP_MAX; d++)
                if (v->gap[n][d] + v->gapEnd[d] > bm) { bm = v->gap[n][d] + v->gapEnd[d]; bh = v->gapH[n][d]; }
        endGap[n] = bm;
        endGapH[n] = bh;
    }
    for (d = 1; d <= CVD_CGAP_MAX; d++)
        if (v->cgap[d] + v->cgapEnd[d] > endC) { endC = v->cgap[d] + v->cgapEnd[d]; endCH = v->cgapH[d]; }

    /* 2. runs that continue, with this frame's emission */
    for (n = 1; n < g_nTrie; n++) {
        const float *lc = v->markCont[g_trie[n].elem];
        float *m = v->mark[n], *g = v->gap[n];
        int32_t *mh = v->markH[n], *gh = v->gapH[n];
        a = m[CVD_MARK_MAX] + lc[CVD_MARK_MAX];
        b = m[CVD_MARK_MAX - 1] + lc[CVD_MARK_MAX - 1];
        if (a >= b) m[CVD_MARK_MAX] = a + x;
        else { m[CVD_MARK_MAX] = b + x; mh[CVD_MARK_MAX] = mh[CVD_MARK_MAX - 1]; }
        for (d = CVD_MARK_MAX - 1; d >= 2; d--) { m[d] = m[d - 1] + lc[d - 1] + x; mh[d] = mh[d - 1]; }
        a = g[CVD_GAP_MAX] + v->gapCont[CVD_GAP_MAX];
        b = g[CVD_GAP_MAX - 1] + v->gapCont[CVD_GAP_MAX - 1];
        if (a >= b) g[CVD_GAP_MAX] = a;
        else { g[CVD_GAP_MAX] = b; gh[CVD_GAP_MAX] = gh[CVD_GAP_MAX - 1]; }
        for (d = CVD_GAP_MAX - 1; d >= 2; d--) { g[d] = g[d - 1] + v->gapCont[d - 1]; gh[d] = gh[d - 1]; }
    }
    a = v->cgap[CVD_CGAP_MAX] + v->cgapCont[CVD_CGAP_MAX];
    b = v->cgap[CVD_CGAP_MAX - 1] + v->cgapCont[CVD_CGAP_MAX - 1];
    if (a >= b) v->cgap[CVD_CGAP_MAX] = a;
    else { v->cgap[CVD_CGAP_MAX] = b; v->cgapH[CVD_CGAP_MAX] = v->cgapH[CVD_CGAP_MAX - 1]; }
    for (d = CVD_CGAP_MAX - 1; d >= 2; d--) { v->cgap[d] = v->cgap[d - 1] + v->cgapCont[d - 1]; v->cgapH[d] = v->cgapH[d - 1]; }

    /* 3. runs that start this frame */
    {
        int32_t gapNode = endC > NEG / 2 ? node_new(v, endCH, 1, 0) : -1;
        for (n = 1; n < g_nTrie; n++) {
            const TrieNode *t = &g_trie[n];
            int p = t->parent;
            if (t->child[0] >= 0 || t->child[1] >= 0) {
                v->gap[n][1] = endMark[n] + lm * t->logCont + f;
                v->gapH[n][1] = endMarkH[n];
            }
            if (p == 0) {
                v->mark[n][1] = endC + lm * g_trie[0].logBranch[t->elem] + r;
                v->markH[n][1] = gapNode;
            } else {
                v->mark[n][1] = endGap[p] + lm * g_trie[p].logBranch[t->elem] + r;
                v->markH[n][1] = endGapH[p];
            }
        }
        v->cgap[1] = bestChar + f;
        v->cgapH[1] = bestChar > NEG / 2 ? node_new(v, bestCharH, 0, g_trie[bestCharNode].chr) : -1;
    }

    /* 4. renormalise */
    top = v->cgap[1];
    for (d = 2; d <= CVD_CGAP_MAX; d++) if (v->cgap[d] > top) top = v->cgap[d];
    for (n = 1; n < g_nTrie; n++) {
        for (d = 1; d <= CVD_MARK_MAX; d++) if (v->mark[n][d] > top) top = v->mark[n][d];
        for (d = 1; d <= CVD_GAP_MAX; d++) if (v->gap[n][d] > top) top = v->gap[n][d];
    }
    for (n = 1; n < g_nTrie; n++) {
        for (d = 1; d <= CVD_MARK_MAX; d++) v->mark[n][d] = v->mark[n][d] > NEG / 2 ? v->mark[n][d] - top : NEG;
        for (d = 1; d <= CVD_GAP_MAX; d++) v->gap[n][d] = v->gap[n][d] > NEG / 2 ? v->gap[n][d] - top : NEG;
    }
    for (d = 1; d <= CVD_CGAP_MAX; d++) v->cgap[d] = v->cgap[d] > NEG / 2 ? v->cgap[d] - top : NEG;
    v->step++;
}

/* history of the best state now */
static int32_t best_history(const Cvd *v)
{
    float top = NEG;
    int32_t h = -1;
    int n, d;
    for (d = 1; d <= CVD_CGAP_MAX; d++) if (v->cgap[d] > top) { top = v->cgap[d]; h = v->cgapH[d]; }
    for (n = 1; n < g_nTrie; n++) {
        for (d = 1; d <= CVD_MARK_MAX; d++) if (v->mark[n][d] > top) { top = v->mark[n][d]; h = v->markH[n][d]; }
        for (d = 1; d <= CVD_GAP_MAX; d++) if (v->gap[n][d] > top) { top = v->gap[n][d]; h = v->gapH[n][d]; }
    }
    return h;
}

static void emit_code(Cvd *v, int chr, int32_t anchor, int gapLen, int last, const Sta *sta)
{
    const char *s = chr_table_out(chr);
    int m = (int)strlen(s), j;
    float wsp = CHR_WORDSPACE_SENTINEL;
    if (!last) {
        Element e;
        memset(&e, 0, sizeof e);
        e.durationSigned = -gapLen;
        sta_classify(sta, &e);
        wsp = gapLen <= -CHR_MIN_GAP_LEN ? e.scores[4] / (e.scores[3] + e.scores[4] + CHR_EPS) : CHR_WORDSPACE_SENTINEL;
    }
    for (j = 1; j <= m && v->outCount < CVD_OUT_MAX; j++) {
        Char *c = &v->out[v->outCount++];
        memset(c, 0, sizeof *c);
        c->charCode = (uint8_t)s[j - 1];
        c->elementOrdinal = anchor - m + j;
        c->wordSpaceProb = j == m ? wsp : CHR_WORDSPACE_DEFAULT;
        c->wordEnd = j == m && last ? WE_LONG : WE_NONE;
        c->wordType = WT_GENERIC;
    }
}

void cvd_commit(Cvd *v, int flush, const Sta *sta)
{
    int32_t path[512], h = best_history(v);
    int n = 0, i;
    v->outCount = 0;
    while (h >= 0 && v->pool[h].step > v->committedStep && n < 512) { path[n++] = h; h = v->pool[h].parent; }
    /* path[n-1] is the oldest uncommitted event: characters alternate with the gaps that follow them */
    for (i = n - 1; i >= 0; i--) {
        const CvdNode *c = &v->pool[path[i]];
        if (c->isGap) continue;
        if (i > 0 && v->pool[path[i - 1]].isGap) {
            const CvdNode *g = &v->pool[path[i - 1]];
            if (!flush && g->step > v->step - CVD_LAG) break;
            emit_code(v, c->value, g->step, g->step - c->step, 0, sta);
            v->committedStep = g->step;
        } else {
            if (!flush) break;
            emit_code(v, c->value, v->step, v->step - c->step, 1, sta);
            v->committedStep = v->step;
        }
    }
    if (flush && v->outCount > 0) {
        v->out[v->outCount - 1].wordSpaceProb = CHR_WORDSPACE_SENTINEL;
        v->out[v->outCount - 1].wordEnd = WE_LONG;
    }
}
