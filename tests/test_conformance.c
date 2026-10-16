/* Conformance checks from cleanroom/13-conformance.md, plus primitive cross-checks. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vail_internal.h"

static int failures = 0, checks = 0;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define NEAR(a, b, tol) (fabs((double)(a) - (double)(b)) <= (tol))

static int fe_peak_for_tone(double hz)
{
    vail_fe *fe = vail_fe_new();
    int16_t blk[FE_HOP_SAMPLES];
    const float *sp = NULL;
    long t = 0;
    int f, j, best = 0;
    for (f = 0; f < 20; f++) {
        int k;
        for (k = 0; k < FE_HOP_SAMPLES; k++, t++) blk[k] = (int16_t)lrint(10000 * sin(2 * M_PI * hz * t / 8000.0));
        sp = vail_fe_process(fe, blk);
    }
    for (j = 1; j < N_BINS; j++) if (sp[j] > sp[best]) best = j;
    for (j = 0; j < N_BINS; j++) CHECK(isfinite(sp[j]), "non-finite FE output");
    vail_fe_free(fe);
    return best;
}

static void test_fe(void)
{
    CHECK(fe_peak_for_tone(800) == 76, "800 Hz peak at %d, want 76", fe_peak_for_tone(800));
    CHECK(fe_peak_for_tone(1000) == 102, "1000 Hz peak at %d, want 102", fe_peak_for_tone(1000));
    CHECK(fe_peak_for_tone(600) == 51, "600 Hz peak at %d, want 51", fe_peak_for_tone(600));
}

static void test_primitives(void)
{
    int n, i, k;
    for (n = 1; n <= 12; n += 5) {
        RunExt mn, mx;
        BoxAvg ba;
        float hist[400];
        runext_init(&mn, n, 0);
        runext_init(&mx, n, 1);
        boxavg_init(&ba, n, 1);
        for (i = 0; i < 400; i++) {
            float v = (float)((i * 7919) % 97) - 40.0f, emn, emx, a, b, c;
            double sum = 0;
            hist[i] = v;
            a = runext_push(&mn, v); b = runext_push(&mx, v); c = boxavg_push(&ba, v);
            emn = emx = v;
            for (k = 0; k < n; k++) {
                float h = (i - k >= 0) ? hist[i - k] : hist[0];   /* warm-up value fills the window */
                if (h < emn) emn = h;
                if (h > emx) emx = h;
            }
            for (k = 0; k < n; k++) sum += (i - k >= 0) ? hist[i - k] : 0.0;
            CHECK(a == emn, "RUNMIN(%d) step %d: %g want %g", n, i, a, emn);
            CHECK(b == emx, "RUNMAX(%d) step %d: %g want %g", n, i, b, emx);
            CHECK(NEAR(c, sum / n, 1e-3), "BOXAVG(%d) step %d: %g want %g", n, i, c, sum / n);
        }
        runext_free(&mn); runext_free(&mx); boxavg_free(&ba);
    }
}

static void test_spm(void)
{
    Spm s;
    float sp[N_BINS];
    int i;
    for (i = 0; i < N_BINS; i++) sp[i] = 1.0f;
    sp[60] = 100.0f;
    spm_init(&s, 60);
    for (i = 0; i < 3000; i++) spm_process_frame(&s, sp, 1.0f);
    CHECK(NEAR(s.spmPeakAmp, 10.0, 1e-3), "spmPeakAmp %g want 10", s.spmPeakAmp);
    CHECK(NEAR(s.spmShoulder, 1.0, 1e-4), "spmShoulder %g want 1", s.spmShoulder);
    CHECK(NEAR(s.spmProminence, 9.0, 1e-3), "spmProminence %g want 9", s.spmProminence);
    spm_free(&s);
}

static void test_env(void)
{
    Env e;
    int i;
    env_init(&e);
    for (i = 0; i < 300; i++) env_apply(&e, 0.45f, 1.0f);
    CHECK(e.envCarrier == 0, "constant 0.45: carrier %d want 0", e.envCarrier);
    CHECK(e.envKeyProb == (float)ENV_KEY_INACTIVE, "inactive keyProb %g want 0.003", e.envKeyProb);
    for (i = 0; i < 300; i++) env_apply(&e, 0.55f, 1.0f);
    CHECK(e.envCarrier == 1, "constant 0.55: carrier %d want 1", e.envCarrier);
    CHECK(e.envKeyProb > 0.0f && e.envKeyProb < 1.0f, "active keyProb %g not in (0,1)", e.envKeyProb);
    for (i = 0; i < 300; i++) env_apply(&e, 0.45f, 1.0f);
    CHECK(e.envCarrier == 1, "falling to 0.45: carrier %d want 1 (hysteresis)", e.envCarrier);
    CHECK(e.envEnvelope >= 0.0f, "negative envelope");
    CHECK(e.envMeanLen <= ENV_SQ_MAX_LEN, "mean-square length over cap");
    env_free(&e);
    /* 13 §2.4 keying probability formula */
    CHECK(NEAR(0.5 * tanh(fmax(0.3, 2.0) - 1.0) + 0.5, 0.8808, 1e-4), "keyProb vector 1");
    CHECK(NEAR(0.5 * tanh(fmax(0.3, 0.2) - 1.0) + 0.5, 0.1978, 1e-4), "keyProb vector 2");
}

static void test_sta(void)
{
    Sta s;
    sta_init(&s);
    CHECK(NEAR(sta_speed(&s), 23.16, 0.01), "prior speed %g want 23.16", sta_speed(&s));
    CHECK((int)nearbyint(sta_speed(&s)) == 23, "prior WPM");
    {
        Element e; memset(&e, 0, sizeof e);
        e.durationSigned = 5; sta_classify(&s, &e); CHECK(e.kind == SEG_KIND_DOT, "5-frame mark kind %d", e.kind);
        e.durationSigned = 15; sta_classify(&s, &e); CHECK(e.kind == SEG_KIND_DASH, "15-frame mark kind %d", e.kind);
        e.durationSigned = -4; sta_classify(&s, &e); CHECK(e.kind == SEG_KIND_GAP_SHORT, "4-frame gap kind %d", e.kind);
        e.durationSigned = -15; sta_classify(&s, &e); CHECK(e.kind == SEG_KIND_GAP_MED, "15-frame gap kind %d", e.kind);
        e.durationSigned = -40; sta_classify(&s, &e); CHECK(e.kind == SEG_KIND_GAP_LONG, "40-frame gap kind %d", e.kind);
    }
    sta_free(&s);
}

static void feed_pattern(Chr *c, const char *pat, int *t, int finalKind)
{
    Element e[64];
    int n = 0;
    for (; *pat; pat++) {
        memset(&e[n], 0, sizeof e[n]);
        e[n].durationSigned = *pat == '.' ? 5 : 15;
        e[n].kind = *pat == '.' ? SEG_KIND_DOT : SEG_KIND_DASH;
        *t += e[n].durationSigned; e[n].elementRef = *t; n++;
        memset(&e[n], 0, sizeof e[n]);
        e[n].durationSigned = pat[1] ? -5 : -15;
        e[n].kind = pat[1] ? SEG_KIND_GAP_SHORT : finalKind;
        e[n].scores[3] = 0.5f; e[n].scores[4] = 0.5f;
        *t += 5; e[n].elementRef = *t; n++;
    }
    chr_add_elements(c, e, n, 0);
}

static void decode_str(const char *pat, char *out)
{
    Chr c;
    int t = 0, i;
    chr_init(&c);
    feed_pattern(&c, pat, &t, SEG_KIND_GAP_MED);
    for (i = 0; i < c.charCount; i++) out[i] = (char)c.chars[i].charCode;
    out[c.charCount] = 0;
    chr_free(&c);
}

static void test_chr(void)
{
    static const char *vec[][2] = { { ".-", "A" }, { "-...", "B" }, { ".....", "5" }, { "-----", "0" },
                                    { ".-.-.", "ar" }, { "-.-.--.-", "cq" }, { "...---", "2" } };
    char out[64];
    size_t i;
    for (i = 0; i < sizeof vec / sizeof vec[0]; i++) {
        decode_str(vec[i][0], out);
        CHECK(!strcmp(out, vec[i][1]), "pattern %s -> '%s' want '%s'", vec[i][0], out, vec[i][1]);
    }
    decode_str("............", out);
    CHECK(strlen(out) >= 1, "unknown pattern produced no character");
    {   /* elementOrdinal is non-decreasing; multi-character entries offset linearly */
        Chr c; int t = 0;
        chr_init(&c);
        feed_pattern(&c, ".-.-.", &t, SEG_KIND_GAP_MED);
        CHECK(c.charCount == 2 && c.chars[1].elementOrdinal == c.chars[0].elementOrdinal + 1, "ordinal offset");
        chr_free(&c);
    }
}

static void test_msg(void)
{
    Msg *m = malloc(sizeof *m);
    Char c;
    int i;
    msg_init(m);
    memset(&c, 0, sizeof c);
    for (i = 0; i < 500; i++) { c.charCode = 'A' + i % 26; c.elementOrdinal = i; msg_merge_chars(m, &c, 1); }
    CHECK(m->count == 500, "count %d want 500", m->count);
    c.elementOrdinal = 1000; msg_merge_chars(m, &c, 1);
    CHECK(m->count == 376, "after overflow count %d want 376", m->count);
    CHECK(m->chars[0].elementOrdinal == 125, "oldest 125 dropped (first ordinal %d)", m->chars[0].elementOrdinal);
    /* a re-sent identical tail is kept once */
    {
        int before = m->count;
        msg_merge_chars(m, &c, 1);
        CHECK(m->count == before && m->newTailCount == 0, "duplicate ordinal appended twice");
    }
    free(m);
}

static void test_api(void)
{
    vail_api *h = API_New();
    float sp[N_BINS];
    int i;
    for (i = 0; i < N_BINS; i++) sp[i] = 0.0f;
    sp[3] = NAN; sp[4] = INFINITY;
    CHECK(API_Feed(h, sp) == API_STATE_NONE, "state with no channel");
    CHECK(sp[3] == 0.0f && sp[4] == 0.0f, "non-finite input not zeroed");
    CHECK(API_Pitch(h) == -1 && API_Snr(h) == -1 && API_Wpm(h) == -1 && API_ChannelCount(h) == -1, "sentinels");
    API_PitchLock(h, 800);
    CHECK(API_Pitch(h) == 802, "pin 800 Hz -> pitch %d want 802", API_Pitch(h));
    API_PitchLock(h, 800);                          /* toggle: unlock */
    CHECK(API_State(h) == API_STATE_PAUSE, "unlocked state %d", API_State(h));
    API_Free(h);

    h = API_New(); API_PitchLock(h, 100);
    CHECK(API_Pitch(h) == 278, "pin 100 Hz -> %d want 278", API_Pitch(h));
    CHECK(API_State(h) == API_STATE_LOCKED_PAUSE, "locked state %d", API_State(h));
    API_Free(h);
    h = API_New(); API_PitchLock(h, 3000);
    CHECK(API_Pitch(h) == 1114, "pin 3000 Hz -> %d want 1114", API_Pitch(h));
    API_Free(h);
    {
        static const double xs[] = { 1, 10, 100, 0.5 };
        static const int want[] = { -10, 0, 10, -10 };
        for (i = 0; i < 4; i++) {
            h = API_New(); API_PitchLock(h, 700);
            ((Bcm *)h)->currentChannel->pipeline->env.envMeanSquare = (float)xs[i];   /* Bcm is the first member */
            CHECK(API_Snr(h) == want[i], "SNR(x=%g) = %d want %d", xs[i], API_Snr(h), want[i]);
            CHECK(API_Wpm(h) == 23, "prior WPM %d", API_Wpm(h));
            API_Free(h);
        }
    }
}

int main(void)
{
    test_fe();
    test_primitives();
    test_spm();
    test_env();
    test_sta();
    test_chr();
    test_msg();
    test_api();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
