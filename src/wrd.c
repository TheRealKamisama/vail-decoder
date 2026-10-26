/* WRD — word segmenter (07-word-segmentation.md). */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "vail_internal.h"

#define WRD_W_GENERIC   0.1f
#define WRD_W_KEYWORD   0.01f
#define WRD_W_RST       0.6f
#define WRD_W_PROSIGN   0.01f
#define WRD_W_QRL       0.01f
#define WRD_W_CALLSIGN  0.6f
#define WRD_LINK_SHARPEN 0.85
#define WRD_CS_A 0.806858f
#define WRD_CS_B 0.076643f
#define WRD_CS_C 0.002267f
#define WRD_CS_D 0.075624f
#define WRD_CS_E 0.011884f
#define WRD_CS_F 0.000002f
#define WRD_CS_G 0.026237f
#define WRD_CS_H 0.000485f
#define WRD_CS_I 0.000002f
#define WRD_LIK_FIRST 35
#define WRD_LIK_LAST  92
#define WRD_EMIT_NONE        (-10000.0f)
#define WRD_EMIT_EXACT       0.0f
#define WRD_EMIT_DIGIT_SELF  (-2.303f)
#define WRD_EMIT_DIGIT_ANY   (-5.303f)
#define WRD_EMIT_LETTER_SELF (-3.258f)
#define WRD_EMIT_LETTER_ANY  (-3.738f)
#define WRD_EMIT_OTHER_SELF  (-2.303f)
#define WRD_WS_FORCED        2.0f
#define WRD_WS_NONE          0.0f
#define WRD_WS_FORCED_BONUS  100.0f
#define WRD_WS_NONE_BONUS    (-10000.0f)
#ifndef WRD_WS_SLOPE
#define WRD_WS_SLOPE         10.0   /* tuning: 07 §4.5 has 6.0; a clear letter or word gap then outweighs
                                     * the templates (long digit strings were split into groups) */
#endif
#define WRD_WS_TANH_IN_MAX   5.0
#define WRD_WS_TANH_OUT_MAX  0.9999
#define WRD_WS_EPS           0.00001
#define WRD_REP_MIN          4
#define WRD_REP_MAX          10
#define WRD_CAND_SCORE_MIN   (-10000.0f)
#define WRD_SCORE_RESET      (-10000.0f)

typedef struct { const char *text; float weight; } Tmpl;

/* ---- template tables (07 §3.4) */
static const Tmpl L1[] = { { "@", 0.199743f }, { "@@", 0.74399f }, { "#@", 0.054979f }, { "#@@", 0.001288f } };
static const Tmpl L2[] = { { "#", 0.939686f }, { "##", 0.052986f }, { "###", 0.00501f }, { "####", 0.002317f } };
static const Tmpl L3[] = { { "@", 0.021387f }, { "@@", 0.392918f }, { "@@@", 0.571806f },
                           { "@@@@", 0.009582f }, { "@@@@@", 0.002967f }, { "@@@@@@", 0.00134f } };
static const Tmpl S1[] = { { "#@@", 0.002199f }, { "@#@", 0.008175f }, { "@##", 0.014827f }, { "@@#@", 0.018811f },
                           { "#@", 0.045966f }, { "#@#", 0.054554f }, { "@", 0.055701f }, { "@#", 0.08404f },
                           { "@@", 0.219683f }, { "@@#", 0.496044f } };
static const Tmpl S2[] = { { "FJL", 0.000367f }, { "G", 0.00078f }, { "E", 0.001056f }, { "QRPP", 0.001446f },
                           { "ANT", 0.001675f }, { "T", 0.002891f }, { "J", 0.004314f }, { "LGT", 0.005002f },
                           { "AM", 0.007022f }, { "A", 0.007f }, { "LH", 0.015627f }, { "MM", 0.049772f },
                           { "QRP", 0.067441f }, { "M", 0.117189f }, { "#", 0.272562f }, { "P", 0.439088f } };
static const Tmpl RST[] = { { "5NN", 0.8f }, { "HNN", 0.05f }, { "SNN", 0.05f }, { "SENN", 0.05f }, { "ESNN", 0.05f },
                            { "IINN", 0.05f }, { "VNN", 0.05f }, { "FNN", 0.05f }, { "LNN", 0.05f }, { "BNN", 0.05f },
                            { "3#N", 0.05f }, { "4#N", 0.05f }, { "5#N", 0.05f }, { "3#9", 0.05f }, { "4#9", 0.05f },
                            { "5#9", 0.05f }, { "H#N", 0.05f } };
static const Tmpl PROSIGN[] = { { "DE", 0.45f }, { "TEST", 0.1f }, { "SS", 0.05f }, { "UP", 0.05f }, { "UP1", 0.05f },
                                { "UP2", 0.05f }, { "QRZ", 0.05f }, { "QRZ?", 0.05f }, { "FD", 0.05f },
                                { "NA", 0.05f }, { "CQ", 0.05f } };
static const Tmpl QRL[] = { { "QRL?", 1.0f }, { "QRL", 1.0f } };
static const Tmpl GENERIC[] = {
    { "+", 0.12f }, { "##", 0.03f }, { "###", 0.1f },
    { "*", 0.1672f }, { "**", 0.2569f }, { "***", 0.1939f }, { "****", 0.1745f }, { "*****", 0.0921f },
    { "******", 0.025f }, { "*******", 0.008f }, { "********", 0.006f }, { "*********", 0.004f },
    { "**********", 0.003f }, { "***********", 0.003f }, { "************", 0.003f },
    { "*************", 0.003f }, { "**************", 0.002f }, { "***************", 0.002f },
    { "****************", 0.002f },
};

static const char *const DICT_003[] = {
    "DX", "PSE", "KN", "BK", "TNX", "EE", "GM", "GE", "GA", "GN", "DR", "OM", "UR", "URS", "RST", "OP", "MY",
    "NAME", "QTH", "HR", "NR", "RIG", "ANT", "QSL", "CFM", "GL", "HPE", "CU", "AGN", "CUL", "RR",
    "HI", "HEE", "QSY", "QRT", "FER", "END", "QSO", "OF", "IT", "ITS", "HVE", "HAS", "HAD", "FB", "CAN",
    "ALL", "YES", "XYL", "YEAR", "YRS", "WX", "WEAK", "WESR", "WE", "WRK", "WKD", "WAS", "VY", "TKS", "SRI",
    "QRU", "QSB", "OUR", "OUT", "OLD", "NOW", "NW", "NOT", "NICE", "NEXT", "MUST", "MID", "ME", "MAN", "LIKE",
    "KNOW", "HOME", "HALF", "ELSE", "DONE", "DEG", "CIAO", "BTR", "BACK", "AT", "ARE", "ALSO", "AGE", "CPY",
    "TEMP", "RPRT", "GB", "QRX", "KW", "CW", "SSB", "PSK", "RTTY", "QSB", "NW", "QRU",
    "VIA", "BURO", "NA", "EU", "CALL",
};
static const char *const DICT_012[] = {
    "KHZ", "##F", "SNOW", "SHE", "PLUS", "##DB", "GOT", "SUM", "FAMILY", "MTRS", "FREQ", "AS", "PADDLE", "KEY",
    "KEYER", "INTO", "TOP", "ICOM", "RUNNING", "YOUR", "USE", "WHEN", "SEND", "LOT", "GET", "STN", "WTS", "#TT",
    "#OO", "#5O", "IOTA", "MUCH", "THAT", "HIS", "EMAIL", "THEY", "CANT", "BIG", "GO", "WELL", "DIPOLE", "QSLL",
    "CHAT", "TTL", "VERT", "WID", "TRY", "OFF", "G5RV", "GREAT", "GUESS", "BY", "OPR", "BT", "ONLY", "LOOP",
    "DAY", "PUT", "NEED", "WORK", "BED", "ELE", "QUAD", "YAGI", "USA", "BEAM", "RRR", "ARE", "FAR", "THEN",
    "BAND", "SOLID", "LID", "JUST", "BTU", "HW", "BAD", "PHONE", "COM", "SAME", "NO", "WILL", "SIG", "SIGS",
    "SURE", "QRM", "QRN", "MNI", "HAM", "GUD", "AND", "BEEN", "BEST", "BUT", "HW?", "THIS", "WHILE",
};
static const Tmpl DICT_HEAD[] = {
    { "K", 0.884f }, { "?", 0.3f }, { "=", 0.26f }, { "TU", 0.0522f }, { "TO", 0.0442f }, { "AR", 0.0361f },
    { "73", 0.0321f }, { "I", 0.0321f }, { "OK", 0.03f }, { "ES", 0.0281f }, { "SK", 0.0281f },
    { "A", 0.0241f }, { "SO", 0.0241f }, { "IN", 0.0201f }, { "IS", 0.0201f }, { "R", 0.0201f },
    { "THE", 0.0201f }, { "U", 0.0201f }, { "ABT", 0.0161f }, { "NEW", 0.0161f }, { "YOU", 0.0161f },
    { "ON", 0.0061f }, { "BE", 0.00003f },
};
#define N_OF(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* ---- build-time graph (07 §4.8) */
typedef struct { float weight; int target; } BEdge;
typedef struct {
    int occupied, label, kind, mapIndex;
    BEdge *out; int nOut, capOut;
    BEdge *in; int nIn, capIn;
} BNode;

typedef struct { BNode *n; int count, cap; int kind; } Builder;

static void edge_push(BEdge **v, int *n, int *cap, float w, int t)
{
    if (*n == *cap) { *cap = *cap ? *cap * 2 : 4; *v = realloc(*v, (size_t)*cap * sizeof(BEdge)); }
    (*v)[*n].weight = w; (*v)[*n].target = t; (*n)++;
}

static int node_new(Builder *b, int occupied, int label)
{
    BNode *x;
    if (b->count == b->cap) { b->cap = b->cap ? b->cap * 2 : 1024; b->n = realloc(b->n, (size_t)b->cap * sizeof(BNode)); }
    x = &b->n[b->count];
    memset(x, 0, sizeof *x);
    x->occupied = occupied; x->label = label; x->kind = b->kind;
    return b->count++;
}

static int new_build_state(Builder *b) { return node_new(b, 1, -1); }

static int successor(Builder *b, int from, int ch, float w)
{
    int i, t;
    for (i = 0; i < b->n[from].nOut; i++) {
        BEdge *e = &b->n[from].out[i];
        if (b->n[e->target].label == ch && b->n[e->target].kind == b->kind) { e->weight += w; return e->target; }
    }
    t = node_new(b, 0, ch);
    edge_push(&b->n[from].out, &b->n[from].nOut, &b->n[from].capOut, w, t);
    return t;
}

static void append_edge(Builder *b, int from, int to, float w)
{
    edge_push(&b->n[from].out, &b->n[from].nOut, &b->n[from].capOut, w, to);
}

static void chain(Builder *b, int from, int to, float scale, const char *text, float weight)
{
    float wt = scale * weight;
    int cur = from;
    for (; *text; text++) cur = successor(b, cur, (unsigned char)*text, wt);
    append_edge(b, cur, to, (float)pow(wt, WRD_LINK_SHARPEN));
}

static int level(Builder *b, int from, float scale, const Tmpl *t, int n)
{
    int accept = new_build_state(b), i;
    for (i = 0; i < n; i++) chain(b, from, accept, scale, t[i].text, t[i].weight);
    return accept;
}

static int level3(Builder *b, int from, float scale)      /* L1 → L2 → L3 */
{
    int a = level(b, from, scale, L1, N_OF(L1));
    int c = level(b, a, scale, L2, N_OF(L2));
    return level(b, c, scale, L3, N_OF(L3));
}

static void callsign_grammar(Builder *b, int root, float s)
{
    int core, p1, s1, p2, split, p3, p4;
    core = level3(b, root, s * (WRD_CS_A + WRD_CS_B + WRD_CS_C + WRD_CS_G + WRD_CS_H + WRD_CS_I));
    append_edge(b, core, root, WRD_CS_A);
    p1 = successor(b, core, '/', s * (WRD_CS_G + WRD_CS_H + WRD_CS_I));
    s1 = level(b, p1, s * (WRD_CS_G + WRD_CS_H + WRD_CS_I), S1, N_OF(S1));
    append_edge(b, s1, root, WRD_CS_G);
    {
        float sc = s * (WRD_CS_D + WRD_CS_E + WRD_CS_F);
        int pre = level(b, root, sc, S1, N_OF(S1));
        int sl = successor(b, pre, '/', sc);
        p2 = level3(b, sl, sc);
    }
    append_edge(b, p2, root, WRD_CS_D);
    split = new_build_state(b);
    append_edge(b, core, split, s * (WRD_CS_B + WRD_CS_C));
    append_edge(b, s1, split, s * (WRD_CS_H + WRD_CS_I));
    append_edge(b, p2, split, s * (WRD_CS_E + WRD_CS_F));
    {
        float sc = s * (WRD_CS_B + WRD_CS_C + WRD_CS_H + WRD_CS_I + WRD_CS_E + WRD_CS_F);
        p3 = level(b, successor(b, split, '/', sc), sc, S2, N_OF(S2));
    }
    append_edge(b, p3, root, s * (WRD_CS_B + WRD_CS_H + WRD_CS_E));
    {
        float sc = s * (WRD_CS_C + WRD_CS_I + WRD_CS_F);
        p4 = level(b, successor(b, p3, '/', sc), sc, S2, N_OF(S2));
    }
    append_edge(b, p4, root, s * (WRD_CS_C + WRD_CS_I + WRD_CS_F));
}

/* The global model is built once; every instance deep-copies it (07 §3.2). */
static HState *g_states; static int g_nStates;
static Selector *g_selectors; static int g_nSelectors;

static HState *resolve(Builder *b, int idx)
{
    return b->n[idx].occupied ? &g_selectors[b->n[idx].mapIndex].state : &g_states[b->n[idx].mapIndex];
}

static void build_global_model(void)
{
    Builder b;
    int root, i, j, selCount = 0, stCount = 0;
    memset(&b, 0, sizeof b);
    b.kind = WT_GENERIC;
    root = new_build_state(&b);
    b.kind = WT_CALL;     callsign_grammar(&b, root, WRD_W_CALLSIGN);
    b.kind = WT_DICT;
    for (i = 0; i < N_OF(DICT_HEAD); i++) chain(&b, root, root, WRD_W_KEYWORD, DICT_HEAD[i].text, DICT_HEAD[i].weight);
    for (i = 0; i < N_OF(DICT_003); i++) chain(&b, root, root, WRD_W_KEYWORD, DICT_003[i], 0.003f);
    for (i = 0; i < N_OF(DICT_012); i++) chain(&b, root, root, WRD_W_KEYWORD, DICT_012[i], 0.012f);
    b.kind = WT_REPORT;   for (i = 0; i < N_OF(RST); i++) chain(&b, root, root, WRD_W_RST, RST[i].text, RST[i].weight);
    b.kind = WT_PROSIGN;  for (i = 0; i < N_OF(PROSIGN); i++) chain(&b, root, root, WRD_W_PROSIGN, PROSIGN[i].text, PROSIGN[i].weight);
    b.kind = WT_QRL;      for (i = 0; i < N_OF(QRL); i++) chain(&b, root, root, WRD_W_QRL, QRL[i].text, QRL[i].weight);
    b.kind = WT_GENERIC;  for (i = 0; i < N_OF(GENERIC); i++) chain(&b, root, root, WRD_W_GENERIC, GENERIC[i].text, GENERIC[i].weight);

    /* Normalise: out-edge weights → log transition costs */
    for (i = 0; i < b.count; i++) {
        double S = 0, T;
        if (b.n[i].nOut == 0) continue;
        for (j = 0; j < b.n[i].nOut; j++) S += b.n[i].out[j].weight;
        T = log(S);
        for (j = 0; j < b.n[i].nOut; j++) b.n[i].out[j].weight = (float)(log((double)b.n[i].out[j].weight) - T);
    }
    /* MirrorIncoming */
    for (i = 0; i < b.count; i++)
        for (j = 0; j < b.n[i].nOut; j++) {
            BNode *t = &b.n[b.n[i].out[j].target];
            edge_push(&t->in, &t->nIn, &t->capIn, b.n[i].out[j].weight, i);
        }
    /* Finalise: numbering (root takes the extra, last selector slot) */
    for (i = 1; i < b.count; i++) b.n[i].mapIndex = b.n[i].occupied ? selCount++ : stCount++;
    b.n[0].mapIndex = selCount;
    g_nSelectors = selCount + 1;
    g_nStates = stCount;
    g_selectors = calloc((size_t)g_nSelectors, sizeof(Selector));
    g_states = calloc((size_t)g_nStates, sizeof(HState));
    for (i = 0; i < b.count; i++) {
        BNode *x = &b.n[i];
        if (x->occupied) {
            Selector *sel = &g_selectors[x->mapIndex];
            sel->nLinks = x->nIn;
            sel->links = calloc((size_t)(x->nIn ? x->nIn : 1), sizeof(ProtoLink));
            for (j = 0; j < x->nIn; j++) {
                sel->links[j].state = resolve(&b, x->in[j].target);
                sel->links[j].weight = x->in[j].weight;
            }
        } else {
            HState *st = &g_states[x->mapIndex];
            st->code = (uint8_t)x->label;
            st->label = (uint8_t)x->label;
            st->kind = x->kind;
            st->parent = resolve(&b, x->in[0].target);
            st->weight = x->in[0].weight;
        }
    }
    for (i = 0; i < b.count; i++) { free(b.n[i].out); free(b.n[i].in); }
    free(b.n);
}

/* relocate a pointer from the global model into this instance */
static HState *relocate(const Wrd *w, const HState *p)
{
    if (p >= g_states && p < g_states + g_nStates) return &w->states[p - g_states];
    {
        const char *c = (const char *)p, *base = (const char *)g_selectors;
        size_t idx = (size_t)(c - base) / sizeof(Selector);
        return &w->selectors[idx].state;
    }
}

static void rescore(Wrd *w)                                /* 07 §4.9 */
{
    int i;
    for (i = 0; i < w->nStates - 1; i++) w->states[i].score = WRD_SCORE_RESET;
    for (i = 0; i < w->nSelectors - 1; i++) w->selectors[i].state.score = WRD_SCORE_RESET;
    w->curSelector->state.score = 0.0f;
    w->savedSelector = NULL;
}

void wrd_init(Wrd *w)
{
    int i, j;
    if (!g_states) build_global_model();
    memset(w, 0, sizeof *w);
    w->nStates = g_nStates;
    w->nSelectors = g_nSelectors;
    w->states = malloc((size_t)w->nStates * sizeof(HState));
    w->selectors = malloc((size_t)w->nSelectors * sizeof(Selector));
    memcpy(w->states, g_states, (size_t)w->nStates * sizeof(HState));
    for (i = 0; i < w->nStates; i++) w->states[i].parent = relocate(w, g_states[i].parent);
    for (i = 0; i < w->nSelectors; i++) {
        w->selectors[i] = g_selectors[i];
        w->selectors[i].links = malloc((size_t)(g_selectors[i].nLinks ? g_selectors[i].nLinks : 1) * sizeof(ProtoLink));
        for (j = 0; j < g_selectors[i].nLinks; j++) {
            w->selectors[i].links[j].state = relocate(w, g_selectors[i].links[j].state);
            w->selectors[i].links[j].weight = g_selectors[i].links[j].weight;
        }
    }
    w->curSelector = &w->selectors[w->nSelectors - 1];
    rescore(w);
}

void wrd_free(Wrd *w)
{
    int i;
    for (i = 0; i < w->nSelectors; i++) free(w->selectors[i].links);
    free(w->selectors); free(w->states);
    w->selectors = NULL; w->states = NULL;
}

void wrd_reset_batch(Wrd *w)                               /* 07 §2.4 */
{
    w->windowLen = 0;
    rescore(w);
}

static int upcase(int k) { return (k >= 'a' && k <= 'z') ? k - 32 : k; }

static void prepare_scores(Wrd *w, const Char *ch)         /* 07 §4.3 */
{
    int i, k = upcase(ch->charCode);
    for (i = WRD_LIK_FIRST; i <= WRD_LIK_LAST; i++) w->like[i] = WRD_EMIT_NONE;
    if (k >= WRD_LIK_FIRST && k <= WRD_LIK_LAST) w->like[k] = WRD_EMIT_EXACT;
    if (k >= '0' && k <= '9') {
        w->like['#'] = WRD_EMIT_DIGIT_SELF;
        w->like['*'] = WRD_EMIT_DIGIT_ANY;
    } else if (k >= 'A' && k <= 'Z') {
        w->like['@'] = WRD_EMIT_LETTER_SELF;
        w->like['*'] = WRD_EMIT_LETTER_ANY;
    } else {
        w->like['+'] = WRD_EMIT_OTHER_SELF;
    }
}

static float word_space_bonus(const Char *ch)             /* 07 §4.5 */
{
    double x = ch->wordSpaceProb, t, p;
    if (ch->wordSpaceProb == WRD_WS_FORCED) return WRD_WS_FORCED_BONUS;
    if (ch->wordSpaceProb == WRD_WS_NONE) return WRD_WS_NONE_BONUS;
    t = WRD_WS_SLOPE * (x - 0.5);
    if (t > WRD_WS_TANH_IN_MAX) t = WRD_WS_TANH_IN_MAX;
    if (t < -WRD_WS_TANH_IN_MAX) t = -WRD_WS_TANH_IN_MAX;
    t = tanh(t);
    if (t > WRD_WS_TANH_OUT_MAX) t = WRD_WS_TANH_OUT_MAX;
    if (t < -WRD_WS_TANH_OUT_MAX) t = -WRD_WS_TANH_OUT_MAX;
    p = 0.5 * (t + 1.0);
    return (float)(log(p + WRD_WS_EPS) - log(1.0 - p + WRD_WS_EPS));
}

static void step(Wrd *w)                                   /* 07 §4.4, §4.5 */
{
    int i, j, l;
    w->curSelector->state.length = 0;
    for (i = w->nStates - 1; i >= 0; i--) {
        HState *st = &w->states[i];
        st->score = st->parent->score + st->weight + w->like[st->code];
        st->length = st->parent->length + 1;
    }
    for (j = 0; j < w->nSelectors; j++) {
        Selector *sel = &w->selectors[j];
        HState *best = sel->links[0].state;
        float bestScore = best->score + sel->links[0].weight;
        for (l = 1; l < sel->nLinks; l++) {
            float sc = sel->links[l].state->score + sel->links[l].weight;
            if (sc > bestScore) { best = sel->links[l].state; bestScore = sc; }
        }
        if (best != &sel->state) sel->state = *best;
        sel->state.score = bestScore;
    }
    w->curSelector->state.score += word_space_bonus(&w->window[w->windowLen - 1]);
}

static void install(Wrd *w, int code)                      /* 07 §4.6 */
{
    int n = w->windowLen, L = w->savedSelector->length;
    if (n - L >= 0 && n - L < n && code == w->window[n - L].charCode) { w->savedSelector = NULL; return; }
    w->curSelector->state = *w->savedSelector;
    rescore(w);
    if (n <= WRD_WINDOW_MAX) { w->wordSlot[n].length = L; w->wordSlot[n].kind = w->curSelector->state.kind; }
    if (n - L >= 0) { w->wordSlot[n - L].length = L; w->wordSlot[n - L].kind = w->curSelector->state.kind; }
    w->savedSelector = NULL;
}

static int same_tail(const Wrd *w, int i)
{
    int m = w->windowLen, j;
    if (i < 1) return 1;
    for (j = 1; j <= i; j++)
        if (w->window[m - i - j].charCode != w->window[m - j].charCode) return 0;
    return 1;
}

static void word_end_decision(Wrd *w)                      /* 07 §4.7 */
{
    int n = w->windowLen, k = n / 2 < WRD_REP_MAX ? n / 2 : WRD_REP_MAX, k0 = 0, i, j, l;
    HState *best = NULL;
    float bestScore = WRD_CAND_SCORE_MIN;
    for (i = WRD_REP_MIN; i <= k; i++)
        if (same_tail(w, i)) { k0 = i; break; }
    if (k0 == 0) return;
    if (2 * k0 < n && w->window[n - 2 * k0 - 1].charCode == w->window[n - 1].charCode) return;
    if (k0 >= 2)
        for (j = 2; j <= k0; j++)
            if (w->window[n - j].wordEnd != WE_NONE && w->window[n - k0 - j].wordEnd != WE_NONE) return;
    for (l = 0; l < w->curSelector->nLinks; l++) {
        HState *st = w->curSelector->links[l].state;
        if (st->length == k0 && st->kind != WT_REPORT && st->kind != WT_PROSIGN) {
            float sc = st->score + w->curSelector->links[l].weight;
            if (sc > bestScore) { best = st; bestScore = sc; }
        }
    }
    if (bestScore > WRD_CAND_SCORE_MIN) {
        w->savedSelector = best;
        if (w->window[n - 1].wordSpaceProb == WRD_WS_FORCED) install(w, 0);
    }
}

static void add_char(Wrd *w, const Char *ch)               /* 07 §2.3 */
{
    if (w->savedSelector) install(w, ch->charCode);
    if (w->windowLen + 1 > WRD_WINDOW_MAX) {
        memmove(w->window, w->window + 1, (WRD_WINDOW_MAX - 1) * sizeof(Char));
        memmove(w->wordSlot, w->wordSlot + 1, WRD_WINDOW_MAX * sizeof(WordRec));
        w->windowLen--;
    }
    w->window[w->windowLen++] = *ch;
    prepare_scores(w, ch);
    step(w);
    word_end_decision(w);
    w->wordSlot[w->windowLen - 1].length = w->curSelector->state.length;
    w->wordSlot[w->windowLen - 1].kind = w->curSelector->state.kind;
}

static void emit_word_ends(Wrd *w)                         /* 07 §4.10 */
{
    const HState *best;
    HState tmp;
    int i, j, k, kind;
    if (w->window[w->windowLen - 1].wordSpaceProb == WRD_WS_FORCED) {
        rescore(w);
        tmp = w->curSelector->state;
        best = &tmp;
    } else {
        best = &w->states[0];
        for (i = 1; i < w->nStates; i++)
            if (w->states[i].score > best->score) best = &w->states[i];
    }
    k = best->length; kind = best->kind;
    i = w->windowLen - 1;
    while (i >= 0) {
        if (k < 1) k = 1;
        w->window[i].wordType = kind;
        w->window[i].wordEnd = (w->window[i].wordSpaceProb == WRD_WS_FORCED) ? WE_LONG : WE_SHORT;
        for (j = i - 1; j >= 0 && j >= i - k + 1; j--) {
            w->window[j].wordType = kind;
            w->window[j].wordEnd = WE_NONE;
        }
        i -= k;
        if (i < 0) break;
        k = w->wordSlot[i].length;
        kind = w->wordSlot[i].kind;
    }
    {
        float c = best->score;
        for (i = 0; i < w->nStates - 1; i++) w->states[i].score -= c;
        for (i = 0; i < w->nSelectors - 1; i++) w->selectors[i].state.score -= c;
    }
}

void wrd_process_chars(Wrd *w, Char *chars, int n, int last)   /* 07 §2.3 ProcessChars */
{
    int i;
    for (i = 0; i < n; i++) add_char(w, &chars[i]);
    if (last && w->windowLen > 0) w->window[w->windowLen - 1].wordSpaceProb = WRD_WS_FORCED;
    if (w->windowLen > 0) emit_word_ends(w);
    /* hand the annotations back (window holds the most recent min(n, WRD_WINDOW_MAX) chars) */
    for (i = 0; i < w->windowLen && i < n; i++) chars[n - w->windowLen + i] = w->window[i];
}
