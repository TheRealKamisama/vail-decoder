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

/* ---- weak-signal extensions (not in 13-conformance): likelihood ENV, SPD, CVD */

static unsigned long long trng = 0x9E3779B97F4A7C15ULL;
static double t_urand(void) { trng ^= trng << 13; trng ^= trng >> 7; trng ^= trng << 17; return ((trng >> 11) + 0.5) * (1.0 / 9007199254740992.0); }
static double t_exp(void) { return -log(t_urand()); }

static void test_lik(void)
{
    double s;
    /* the LLR rises with the power, is negative at the noise mean and positive at the key-down mean */
    for (s = 0.5; s < 200.0; s *= 2.0) {
        CHECK(lik_llr(1.0, s) < 0.0, "llr(noise mean, s=%g) %g not < 0", s, lik_llr(1.0, s));
        CHECK(lik_llr(1.0 + s, s) > 0.0, "llr(key-down mean, s=%g) %g not > 0", s, lik_llr(1.0 + s, s));
        CHECK(lik_llr(2.0, s) > lik_llr(1.0, s), "llr not increasing (s=%g)", s);
    }
    CHECK(NEAR(lik_llr(0.0, 3.0), -3.0, 1e-9), "llr(0, s) = -s");
    {   /* flat spectrum: both side levels are the level itself */
        float sp[N_BINS], l, r, b;
        int i;
        for (i = 0; i < N_BINS; i++) sp[i] = 2.0f;
        spm_noise_sides(sp, 60, &l, &r, &b);
        CHECK(NEAR(l, 2.0, 1e-6) && NEAR(r, 2.0, 1e-6), "side levels %g %g want 2", l, r);
        spm_noise_sides(sp, 2, &l, &r, &b);          /* near the edge one side copies the other */
        CHECK(NEAR(l, 2.0, 1e-6) && NEAR(r, 2.0, 1e-6), "edge side levels %g %g want 2", l, r);
    }
    {   /* exponential noise: no carrier; a keyed tone 13 dB above it: carrier, and the key-down frames
         * (36 frames earlier) carry positive log-odds, the key-up frames negative */
        Env e;
        float sp[N_BINS], l, r, b;
        int f, i, c = 60, pos = 0, neg = 0, onFrames = 0, offFrames = 0, carrier = 0;
        env_init(&e);
        for (f = 0; f < 1200; f++) {
            int key = f >= 600 && (f / 8) % 2 == 0;       /* 8 frames on, 8 off */
            for (i = 0; i < N_BINS; i++) sp[i] = (float)t_exp();
            if (key) sp[c] = (float)(t_exp() + 20.0);
            spm_noise_sides(sp, c, &l, &r, &b);
            {
                float beside[2 * LIK_PEAK_SPAN];
                int k, n = 0;
                for (k = -LIK_PEAK_SPAN; k <= LIK_PEAK_SPAN; k++) if (k) beside[n++] = sp[c + k];
                env_apply_lik(&e, sp[c], beside, l, r, b, 1e-10f);
            }
            if (f == 599) CHECK(!e.envCarrier, "carrier on in noise (SNR estimate %g)", e.envSnr);
            if (f >= 800) {
                int was = ((f - LIK_DELAY) / 8) % 2 == 0, mid = (f - LIK_DELAY) % 8;
                carrier += e.envCarrier;
                if (mid >= 2 && mid <= 5) {
                    if (was) { onFrames++; pos += e.envObs.keyLogOdds > 0.0f; }
                    else { offFrames++; neg += e.envObs.keyLogOdds < 0.0f; }
                }
            }
        }
        CHECK(carrier == 400, "carrier on %d of 400 keyed frames", carrier);
        CHECK(pos >= onFrames * 95 / 100, "key-down frames with positive log-odds: %d of %d", pos, onFrames);
        CHECK(neg >= offFrames * 95 / 100, "key-up frames with negative log-odds: %d of %d", neg, offFrames);
        CHECK(e.envSnr > 12.0f && e.envSnr < 26.0f, "SNR estimate %g want about 20", e.envSnr);
        env_free(&e);
    }
}

/* frames of clean keying of text at wpm, as SEG input; returns the frame count */
static int keyed_obs(const char *text, double wpm, SegObs *o, int max)
{
    double u = 112.94 / wpm, t = 0.0;
    int n = 0, k;
    const char *p;
    for (p = text; *p; p++) {
        const char *code = NULL;
        if (*p == ' ') { t += 4.0 * u; continue; }
        for (k = 0; k < chr_table_size(); k++)
            if (chr_table_out(k)[0] == *p && !chr_table_out(k)[1]) { code = chr_table_pattern(k); break; }
        for (; code && *code; code++) {
            double on = (*code == '.' ? 1.0 : 3.0) * u;
            int a = (int)(t + 0.5), b = (int)(t + on + 0.5), g = (int)(t + on + u + 0.5);
            for (; n < a && n < max; n++) { o[n].keyLogOdds = -8.0f; o[n].riseLogOdds = -8.0f; o[n].fallLogOdds = 0.0f; }
            for (; n < b && n < max; n++) { o[n].keyLogOdds = 8.0f; o[n].riseLogOdds = 8.0f; o[n].fallLogOdds = 0.0f; }
            for (; n < g && n < max; n++) { o[n].keyLogOdds = -8.0f; o[n].riseLogOdds = -8.0f; o[n].fallLogOdds = 0.0f; }
            t += on + u;
        }
        t += 2.0 * u;
    }
    for (k = 0; k < 300 && n < max; k++, n++) { o[n].keyLogOdds = -8.0f; o[n].riseLogOdds = -8.0f; o[n].fallLogOdds = 0.0f; }
    return n;
}

static void test_cvd(void)
{
    static SegObs o[4000];
    static const char *texts[] = { "PARIS PARIS", "CQ DE K1ABC", "5NN TU 73" };
    Cvd *v = malloc(sizeof *v);
    Spd *p = malloc(sizeof *p);
    Sta sta;
    size_t ti;
    for (ti = 0; ti < sizeof texts / sizeof texts[0]; ti++) {
        double wpm = 18.0 + 6.0 * ti;
        char got[64], want[64];
        int n = keyed_obs(texts[ti], wpm, o, 4000), i, m = 0, kb;
        const char *q;
        sta_init(&sta);
        for (i = 0; i < 5; i++) sta.means[i] = (float)log(112.94 / wpm * (i == 1 || i == 3 ? 3.0 : (i == 4 ? 7.0 : 1.0)));
        cvd_init(v);
        cvd_set_model(v, &sta);
        for (i = 0; i < n; i++) {
            int j;
            cvd_update(v, &o[i]);
            cvd_commit(v, i == n - 1, &sta);
            for (j = 0; j < v->outCount && m < 63; j++) got[m++] = (char)v->out[j].charCode;
        }
        got[m] = 0;
        for (q = texts[ti], m = 0; *q; q++) if (*q != ' ') want[m++] = *q;
        want[m] = 0;
        CHECK(!strcmp(got, want), "CVD decoded '%s' want '%s'", got, want);
        spd_init(p);
        for (i = 0; i < n; i++) spd_update(p, &o[i]);
        kb = spd_best(p);
        CHECK(fabs(log(p->wpm[kb] / wpm)) < 0.12, "SPD best %g WPM want %g", p->wpm[kb], wpm);
        sta_free(&sta);
    }
    free(v);
    free(p);
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
    test_lik();
    test_cvd();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
