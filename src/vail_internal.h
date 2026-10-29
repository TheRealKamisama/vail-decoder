/* SPDX-License-Identifier: GPL-3.0-or-later */
/* vail — internal module definitions (FE, BCM, SIG, SPM, ENV, SEG, STA, CHR, WRD, MSG).
 * Every constant and algorithm follows cleanroom/; section references are given as "02 §4.2" etc. */
#ifndef VAIL_INTERNAL_H
#define VAIL_INTERNAL_H

#include <stdint.h>
#include <stdio.h>
#include "vail.h"

/* ---------------------------------------------------------------- FE (02) */
#define FE_SAMPLE_RATE     8000
#define FE_HOP_SAMPLES     85
#define FE_FFT_SIZE        1024
#define FE_WINDOW_CENTER   512
#ifndef FE_WINDOW_CUTOFF
#define FE_WINDOW_CUTOFF   0.0023     /* tuning: 02 §4.1 has 0.003125 (45 Hz flat top); 0.0023 gives
                                         * 30 Hz noise bandwidth, closer to matched for 20-30 WPM */
#endif
#define FE_TWO_PI          6.283185307179586
#define FE_SAMPLE_SCALE    0.000030517578125
#define FE_BIN_FIRST       26
#define FE_BIN_LIMIT       154
#define FE_N_BINS          128
#define FE_POWER_SCALE     0.001
#define N_BINS             FE_N_BINS

/* tuning E4 (12-open-items.md §E): lowest power used as a normalisation reference; 0 disables it */
#ifndef VAIL_TUNE_POWER_FLOOR
#define VAIL_TUNE_POWER_FLOOR 1e-10f
#endif

struct vail_fe {
    float frameBuf[FE_FFT_SIZE];
    float window[FE_FFT_SIZE];
    float work[FE_FFT_SIZE];
    float out[FE_N_BINS];
};

/* ------------------------------------------------------- primitives (04 §5) */
typedef struct {
    float *ring;
    int window, ringLen, cursor;
    float acc, lastOut;
    int isMax;
} RunExt;                                   /* RUNMIN(n) / RUNMAX(n) */

typedef struct {
    int passes, window, ringLen;
    float *cascadeA, *cascadeB;             /* (passes+1) x ringLen */
    int i0, i1, J, counter;
    float weight;
} BoxAvg;                                   /* BOXAVG(n, k) */

void runext_init(RunExt *r, int n, int isMax);
void runext_free(RunExt *r);
float runext_push(RunExt *r, float v);
void boxavg_init(BoxAvg *b, int n, int k);
void boxavg_free(BoxAvg *b);
float boxavg_push(BoxAvg *b, float v);

/* --------------------------------------------------------------- SPM (04) */
#define SPM_SEARCH_RADIUS      2
#define SPM_PEAK_MIN_POWER     7.5
#define SPM_OFFSET_DECAY       0.97
#define SPM_OFFSET_GAIN        0.03
#define SPM_FLOOR_MAX_WINDOW   11
#define SPM_FLOOR_MIN_WINDOW   51
#define SPM_FLOOR_DECAY        0.99
#define SPM_FLOOR_GAIN         0.01
#define SPM_FLOOR_ZERO_EPS     1e-20
#define SPM_NORM_EPS           1e-12
#define SPM_SHAPE_HALF_WIDTH   10
#define SPM_SHOULDER_FIRST     4
#define SPM_SHOULDER_LAST      8
#define SPM_SHOULDER_EXPONENT  0.0625
#define SPM_BIN_MIN            10
#define SPM_BIN_MARGIN         10
#ifndef SPM_TUNE_EDGE_FOLLOW
#define SPM_TUNE_EDGE_FOLLOW   1   /* tuning E5 (12-open-items.md §E) */
#endif

typedef struct {
    RunExt spmSmoothMax, spmNoiseMin;
    int spmCenterBin;
    float spmCenterOffset;
    float spmShape[2 * SPM_SHAPE_HALF_WIDTH + 1];
    float spmPeakAmp, spmProminence, spmShoulder, spmFloor;
} Spm;

void spm_init(Spm *s, int binIndex0);
void spm_free(Spm *s);
void spm_track_peak(Spm *s, const float *trackArray);
void spm_process_frame(Spm *s, const float *spectrum, float noiseFloor);

/* --------------------------------------------------------------- ENV (04) */
#define ENV_SHIFT_ROWS        38
#define ENV_SHIFT_ROW_FLOATS  8
#define ENV_ENV_WINDOW        71
#define ENV_THR_SHAPE_WINDOW  9
#define ENV_PROM_MAX_WINDOW   17
#define ENV_THR_MIN_WINDOW    111
#define ENV_STRENGTH_WINDOW   141
#ifndef ENV_HYST_ON
#define ENV_HYST_ON           0.5
#endif
#ifndef ENV_HYST_OFF
#define ENV_HYST_OFF          0.4
#endif
#define ENV_THR_UPPER         3.0
#define ENV_THR_BASE          0.8
#define ENV_THR_SHAPE_GAIN    0.4
#define ENV_THR_ATTACK_GAIN   0.35
#define ENV_DIP_RATIO         0.5
#define ENV_DIP_BASE          3.0
#define ENV_KEY_MIN_LEVEL     0.3
#define ENV_KEY_HALF          0.5
#define ENV_KEY_TANH_LIMIT    5.0
#define ENV_KEY_TANH_SAT      0.9999
#define ENV_KEY_INACTIVE      0.003
#define ENV_SQ_GATE           0.8
#define ENV_SQ_MAX_LEN        2048
#define ENV_SIG_OBS_LEVEL     1.3

/* one frame of SEG input, as log-odds against key-up: a key-down frame, the first frame of a mark
 * (rising edge) and the first frame of a gap (falling edge). Without an edge model (05 §4.3) the
 * rising edge is a key-down frame and the falling edge a key-up frame (riseLogOdds = keyLogOdds,
 * fallLogOdds = 0); keyLogOdds = log(keyProb / (1 - keyProb)). */
typedef struct { float keyLogOdds, riseLogOdds, fallLogOdds; } SegObs;

/* Likelihood keying detector (ENV_TUNE_LIK, replaces the threshold of 04 §4.4). Per frame it takes the
 * channel bin's power and noise references from the bins beside it, and outputs, LIK_DELAY frames later,
 * the SEG input as calibrated log-likelihood ratios: noise-only power is exponential, key-down power is
 * Rician with per-frame SNR envSnr. The noise is the bin's own key-up average kept between the levels of
 * the two sides; envSnr is the key-down average over a window around the output frame (an EM step on
 * 3-frame posteriors); frames raised by a click or static crash are scored against their own broadband
 * level. The carrier is the SNR estimate with hysteresis. */
#ifndef ENV_TUNE_LIK
#define ENV_TUNE_LIK          1
#endif
#define LIK_DELAY             36     /* output lag, frames (as the reference ENV) */
#define LIK_RING              128    /* history length, power of two > 2 * LIK_DELAY + 2 */
#define LIK_NB_FIRST          6      /* noise reference: bins at offsets 6..16 on each side */
#define LIK_NB_LAST           16
#define LIK_NOISE_WIN         73     /* running means of the noise references, frames */
#ifndef LIK_SIG_WIN
#define LIK_SIG_WIN           71     /* triangular SNR window centred on the output frame (odd, at most
                                      * 2 * LIK_DELAY - 1) */
#endif
#ifndef LIK_KAPPA_GAIN
#define LIK_KAPPA_GAIN        1.25   /* x the LLR scale (derived from the FE window) of SEG's input; the
                                      * SNR estimation always uses the derived scale */
#endif
#ifndef LIK_EDGE_GAIN
#define LIK_EDGE_GAIN         0.35f  /* key-down power seen by the frame at a key edge; 0 = no edge model */
#endif
#define LIK_PRIOR_ON          0.40f  /* key-down prior of the EM posteriors */
#define LIK_S_MIN             0.25f  /* smallest per-frame SNR (power ratio) */
#define LIK_S_INIT            4.0f
#define LIK_W0                3.0f   /* pseudo-frames pulling the SNR estimate to LIK_S_MIN */
#ifndef LIK_CAR_ON
#define LIK_CAR_ON            1.0f   /* carrier hysteresis on the per-frame SNR */
#endif
#ifndef LIK_CAR_OFF
#define LIK_CAR_OFF           0.6f
#endif
#define LIK_QRN_RATIO         2.0f   /* a frame whose broadband level exceeds this x the noise ... */
#define LIK_QRN_REL           0.05f  /* ... and this fraction of the centre bin's power is impulsive */
#define LIK_LLR_CLAMP         20.0f  /* per-frame log-odds handed to SEG are limited to +-this */
#define LIK_OWN_CAP           10.0f  /* key-up frames enter the own noise average capped at this x side */
#ifndef LIK_DYN_RANGE
#define LIK_DYN_RANGE         1e-5f  /* noise is taken at least this x the band's peak power (50 dB) */
#endif
/* keyed peak tracking: the key-down excess of the bins around the centre, a leaky sum over about
 * LIK_PEAK_TAU frames, moves the channel one bin towards a peak 1 or 2 bins away when the peak beats
 * the centre by LIK_PEAK_RATIO. It follows weak signals, whose peak ratio never reaches SPM's tracking
 * threshold; excess still rising LIK_PEAK_SPAN bins away is another signal's skirt and is left alone */
#ifndef LIK_TUNE_PEAK
#define LIK_TUNE_PEAK         1
#endif
#define LIK_PEAK_SPAN         3
#define LIK_PEAK_TAU          128.0
#ifndef LIK_PEAK_RATIO
#define LIK_PEAK_RATIO        1.15
#endif
#define LIK_PEAK_MIN          20.0   /* centre excess (noise x frames) needed before moving */
#define LIK_PEAK_HOLD         64     /* frames between two moves */

typedef struct {
    float x[LIK_RING];                     /* centre power */
    float nl[LIK_RING], nr[LIK_RING];      /* left / right side levels, clipped */
    float broad[LIK_RING];                 /* broadband level of the frame */
    float g[LIK_RING], gy[LIK_RING];       /* EM terms: key-down posterior, posterior x (y - 1) */
    float go[LIK_RING], gox[LIK_RING];     /* key-up posterior, posterior x power */
    float xn[LIK_RING][2 * LIK_PEAK_SPAN]; /* powers of the bins centre - span .. centre + span, but the centre */
    int t;                                 /* frames seen */
    double nlSum, nrSum, goSum, goxSum;
    double peakEx[2 * LIK_PEAK_SPAN + 1];  /* key-down excess power of centre - span .. centre + span */
    int peakHold;
    float noise, noiseL, noiseR, noiseOwn; /* noise estimates: used, left, right, own key-up frames */
    float llr;                             /* LLR of the output frame */
} Lik;

typedef struct {
    BoxAvg envSmoother, envThrShaper;
    RunExt envPeakHold, envMaxShort, envThrMinHold;
    float envThrUpper;
    int envMeanLen;
    float mem[ENV_SHIFT_ROWS][ENV_SHIFT_ROW_FLOATS];   /* envShiftMem */
    float envKeyProb, envMeanSquare, envStrength;
    float envEnvelope, envThreshold;                   /* same-frame copies */
    int envCarrier;
    float envSnr;                                      /* per-frame SNR estimate (power ratio) */
    SegObs envObs;                                     /* SEG input of the output frame */
    Lik lik;
} Env;

void env_init(Env *e);
void env_free(Env *e);
void env_apply(Env *e, float prominence, float amplitude);
/* likelihood path: the channel bin's power, the noise references of spm_noise_sides, and the lowest
 * noise power accepted (dynamic range of the analysis; digital silence) */
void env_apply_lik(Env *e, float power, const float *beside, float noiseLeft, float noiseRight, float broad,
                   float noiseMin);
int env_peak_move(Env *e);                  /* -1, 0 or +1: the bin the channel should move by */
/* the beside argument of env_apply_lik: powers of the bins centre - LIK_PEAK_SPAN .. centre +
 * LIK_PEAK_SPAN without the centre, in order */
void spm_noise_sides(const float *spectrum, int centerBin, float *left, float *right, float *broad);
double lik_llr(double y, double s);

/* --------------------------------------------------------------- SEG (05) */
#define SEG_SILENCE_LEVEL   1.3
#ifndef SEG_ON_MAX
#define SEG_ON_MAX          30
#endif
#ifndef SEG_OFF_MAX
#define SEG_OFF_MAX         60
#endif
#define SEG_WINDOW_LEN      50
#define SEG_CONTEXT_KEEP    25
#define SEG_EMIT_CAPACITY   50
#define SEG_LOG_FLOOR       1e-12
#define SEG_LOG_ZERO        (-10000.0f)
#define SEG_FEATURE_FLOOR   0.001f

enum { SEG_KIND_DOT = 0, SEG_KIND_DASH, SEG_KIND_GAP_SHORT, SEG_KIND_GAP_MED, SEG_KIND_GAP_LONG };

typedef struct {
    int32_t durationSigned;
    int32_t elementRef;
    uint8_t qualityFlag;
    int32_t kind;
    float scores[5];
} Element;

typedef struct { float score, logEnd, logContinue; } DurState;

struct Sta;

typedef struct {
    int stepCounter, windowIndex;
    uint8_t bitHist[SEG_WINDOW_LEN];
    int onDurations[SEG_WINDOW_LEN], offDurations[SEG_WINDOW_LEN];
    int onRun, offRun;
    float keyLogOdds, spaceLogOdds;
    float riseLogOdds, fallLogOdds;   /* emissions of the first frame of a mark / of a gap */
    int carrierDrop;
    DurState durState[SEG_ON_MAX + SEG_OFF_MAX + 1];
    Element out[SEG_EMIT_CAPACITY];
    int outCount;
    struct Sta *statsRef;
} Seg;

void seg_init(Seg *s, struct Sta *sta);
void seg_build_duration_tables(Seg *s);
void seg_tables_from(DurState *d, const struct Sta *sta);   /* normalised hazards of sta's model */
void seg_update(Seg *s, int bit, const SegObs *obs);

/* --------------------------------------------------------------- STA (05) */
#define STA_WINDOW_LEN       14
#define STA_GROUP_LEN        7
#define STA_UPDATE_DUR_MAX   60
#define STA_PENDING_MAX      4096

typedef struct Sta {
    float means[5], variances[5], covariance[5][5];
    int modelDirty, learningPhase, flushFlag;
    Element *pendingBuffer;                 /* newest first, as in 05 §8.4 */
    int pendingCount, pendingCap;
    int windowCount;
    Element windowElements[STA_WINDOW_LEN];
    double windowLogLik;
} Sta;

void sta_init(Sta *s);
void sta_free(Sta *s);
void sta_reset(Sta *s);
float sta_score_of(const Sta *s, int c, float dur);
float sta_total_prob(const Sta *s, int d);
/* commit: source is the SEG batch (newest first). Returns the classified batch (newest first)
 * in *out (malloc'd, caller frees) and its length. */
int sta_commit(Sta *s, const Element *source, int n, Element **out);
void sta_classify(const Sta *s, Element *e);
float sta_speed(const Sta *s);
int sta_flush_priming(Sta *s, Element **out);

/* --------------------------------------------------------------- SPD (speed bank)
 * SPD_N explicit-duration HMMs, each with the standard timing (dash and letter gap 3 units, word gap 7)
 * at a fixed speed, run on SEG's input, plus one with SEG's current tables. Their leaky forward
 * log-likelihoods give a speed estimate that does not depend on SEG's own segmentation, whose duration
 * prior comes from STA: at low SNR that loop can lock onto a wrong speed (merged elements read as a
 * slower sender, and so on). */
#ifndef SPD_TUNE
#define SPD_TUNE        1
#endif
#define SPD_N           15
#define SPD_WPM_MIN     12.0
#define SPD_WPM_MAX     45.0
#define SPD_UNIT_NUM    112.94     /* frames per dot unit at 1 WPM (1.2 s / 10.625 ms) */
#ifndef SPD_TAU
#define SPD_TAU         400.0      /* score memory, frames of carrier */
#endif
#ifndef SPD_SNAP_MARGIN
#define SPD_SNAP_MARGIN 8.0        /* nats the best speed must lead the STA speed by to replace it */
#endif
#ifndef SPD_SNAP_STEPS
#define SPD_SNAP_STEPS  2          /* ... and be at least this many candidates (about 10 % each) away */
#endif
#define SPD_HOLD        150        /* frames between two replacements */

#define SPD_CUR         SPD_N      /* the extra model: SEG's current (STA-adapted) tables */

typedef struct {
    float hz[SPD_N + 1][SEG_ON_MAX + SEG_OFF_MAX + 1];      /* hazard (probability that the run ends) */
    double alpha[SPD_N + 1][SEG_ON_MAX + SEG_OFF_MAX + 1];  /* forward probabilities, normalised */
    double score[SPD_N + 1];
    float wpm[SPD_N];
    int frames, hold;
} Spd;

void spd_init(Spd *p);
void spd_set_current(Spd *p, const DurState *segTables);
void spd_update(Spd *p, const SegObs *obs);
void spd_forget(Spd *p);                    /* clears the scores (a new over starts) */
int spd_best(const Spd *p);                 /* index of the best fixed-speed candidate */
int spd_index_of(const Spd *p, float wpm);
void spd_means(const Spd *p, int k, float *means);

/* --------------------------------------------------------------- CHR (06) */
#define CHR_TABLE_LEN          64
#define CHR_PENDING_MAX        20
#define CHR_MIN_GAP_LEN        (-150)
#define CHR_WORDSPACE_DEFAULT  0.001f
#define CHR_WORDSPACE_SENTINEL 2.0f
#define CHR_EPS                1e-12f
#define CHR_PLACEHOLDER        "*"
#define CHR_PAT_MAX            64

enum { WE_NONE = 0, WE_SHORT = 1, WE_LONG = 2 };
enum { WT_GENERIC = 0, WT_DICT, WT_REPORT, WT_PROSIGN, WT_QRL, WT_CALL };

typedef struct {
    uint8_t charCode;
    int32_t wordType;
    int32_t elementOrdinal;
    uint8_t lowSnr;
    float wordSpaceProb;
    int32_t wordEnd;
} Char;

typedef struct {
    Element *elemBuf;        /* pending elements, ascending time order */
    int elemCount, elemCap;
    Char *chars;             /* output of the last call */
    int charCount, charCap;
} Chr;

void chr_init(Chr *c);
void chr_free(Chr *c);
void chr_clear_history(Chr *c);
/* elements: ascending time order */
void chr_add_elements(Chr *c, const Element *elements, int n, int flush);
const char *chr_lookup(const char *pat);
int chr_table_size(void);
const char *chr_table_out(int i);
const char *chr_table_pattern(int i);
float chr_table_weight(int i);

/* --------------------------------------------------------------- CVD (character Viterbi decoder)
 * Decodes characters directly from SEG's per-frame input with a duration HMM laid out as a trie of
 * the code table: every node carries a mark (dot or dash) and an element-gap duration chain, and an
 * inter-character gap chain links the codes. Element boundaries, gap classes and code identities are
 * thus decided together, with the table weights as character priors, and only codes of the table can
 * come out. Durations follow STA's model; a fixed-lag traceback of token-passing histories commits
 * characters in the CHR output format. */
#ifndef CVD_TUNE
#define CVD_TUNE         1
#endif
#define CVD_MAX_NODES    80
#define CVD_MARK_MAX     SEG_ON_MAX
#define CVD_GAP_MAX      30
#define CVD_CGAP_MAX     SEG_OFF_MAX
#define CVD_POOL         2048
#ifndef CVD_LAG
#define CVD_LAG          80          /* frames a gap must lie in the past before its code is committed */
#endif
#ifndef CVD_LM_WEIGHT
#define CVD_LM_WEIGHT    1.0f        /* scale of the character prior log-probabilities */
#endif
#ifndef CVD_LM_BIGRAM
#define CVD_LM_BIGRAM    0.5f        /* weight of the character bigram (tools/lmgen.py) over the table prior */
#endif
#ifndef CVD_LM_ORDER
#define CVD_LM_ORDER     3           /* 2: character bigram, 3: trigram */
#endif
#define CVD_OUT_MAX      64
#define CVD_WP_MAX       400

typedef struct { int32_t parent, step; int16_t value; uint8_t isGap, mark; } CvdNode;

typedef struct {
    float markEnd[2][CVD_MARK_MAX + 1], markCont[2][CVD_MARK_MAX + 1];   /* dot / dash hazards (log) */
    float gapEnd[CVD_GAP_MAX + 1], gapCont[CVD_GAP_MAX + 1];             /* element gap */
    float cgapEnd[CVD_CGAP_MAX + 1], cgapCont[CVD_CGAP_MAX + 1];         /* letter / word gap */
    float wordProb[CVD_WP_MAX + 1];                                       /* P(word gap | length) */
    float mark[CVD_MAX_NODES][CVD_MARK_MAX + 1];
    int32_t markH[CVD_MAX_NODES][CVD_MARK_MAX + 1];
    float gap[CVD_MAX_NODES][CVD_GAP_MAX + 1];
    int32_t gapH[CVD_MAX_NODES][CVD_GAP_MAX + 1];
    float cgap[CVD_CGAP_MAX + 1];
    int32_t cgapH[CVD_CGAP_MAX + 1];
    CvdNode pool[CVD_POOL];
    int32_t freeHead, freeCount;
    int32_t committedStep;   /* SEG step of the last committed code's end */
    int step;                /* SEG step of the next frame */
    Char out[CVD_OUT_MAX];
    int outCount;
} Cvd;

void cvd_init(Cvd *v);
void cvd_set_model(Cvd *v, const struct Sta *sta);
void cvd_reset(Cvd *v, int step);         /* in a long gap, no history */
void cvd_update(Cvd *v, const SegObs *obs);
/* commits the codes whose following gap ended at least CVD_LAG frames ago (all of them when flush)
 * into v->out / v->outCount */
void cvd_commit(Cvd *v, int flush, const struct Sta *sta);

/* --------------------------------------------------------------- WRD (07) */
#define WRD_WINDOW_MAX 26

typedef struct HState HState;
struct HState {
    uint8_t code, label;
    int32_t kind;
    HState *parent;
    float weight;
    float score;
    int32_t length;
};

typedef struct { HState *state; float weight; } ProtoLink;
typedef struct { HState state; ProtoLink *links; int nLinks; } Selector;

typedef struct { int32_t length, kind; } WordRec;

typedef struct {
    HState *states; int nStates;
    Selector *selectors; int nSelectors;
    WordRec wordSlot[WRD_WINDOW_MAX + 1];
    Char window[WRD_WINDOW_MAX];
    int windowLen;
    Selector *curSelector;
    HState *savedSelector;
    float like[128];
} Wrd;

void wrd_init(Wrd *w);
void wrd_free(Wrd *w);
void wrd_reset_batch(Wrd *w);
void wrd_process_chars(Wrd *w, Char *chars, int n, int last);

/* --------------------------------------------------------------- MSG (08) */
#define MSG_CAPACITY  500
#define MSG_SHIFT     125
#define MSG_BACKSCAN  10

typedef struct {
    Char chars[MSG_CAPACITY];
    int count;
    Char replaced[MSG_CAPACITY]; int replacedCount;
    int veq;                 /* confirmed-identical prefix of the last merge (08 §4.1 step 2) */
    Char newTail[MSG_CAPACITY];  int newTailCount;
    int32_t lastIdxE;
    int clearFlagA, clearFlagB;
} Msg;

void msg_init(Msg *m);
void msg_merge_chars(Msg *m, const Char *batch, int n);
int msg_chars_since(const Msg *m, int32_t idIdx, Char *out);
int msg_render(const Msg *m, char *buf, int cap);
int msg_rendered_len(const Char *c);   /* 1 + materialised spaces */

/* --------------------------------------------------------------- SIG */
typedef struct {
    Spm spm;
    Env env;
    Seg seg;
    Sta sta;
    Spd spd;
    Cvd *cvd;
    Chr chr;
    Wrd wrd;
    Char hist[WRD_WINDOW_MAX]; int histCount;
    int prevCarrier;
    int flushRequest;
    int signalPresent;
    float quality;
    int hasText;
    int segGate;
    int frameBase;           /* absolute frame of SEG step 0 (debug output only) */
    int id;                  /* channel id (debug output only) */
    /* SEG input kept while STA learns, re-segmented with the learned model when learning ends; later
     * the frames of the current over (SIG_TUNE_OVER) */
    SegObs *replayObs;
    uint8_t *replayBit, *replayDrop;
    int replayCount, replayStep, replayOverflow;
    Char cvdOut[256];        /* characters committed by CVD this frame */
    int cvdOutCount, cvdReplayed;
    /* the start of each later transmission (over) is decoded again once STA has seen it (SIG_TUNE_OVER) */
    int overActive, overCount, overStep, prevCarrierCvd;
    float overWpm;
} Sig;

/* tuning: after a carrier drop the next over may come from another station at another speed; CVD holds
 * its output until STA has re-estimated on the new over (at most SIG_OVER_MAX frames), and decodes the
 * over again from its start if the speed moved by more than SIG_OVER_SPEED; 0 decodes it straight on */
#ifndef SIG_TUNE_OVER
#define SIG_TUNE_OVER    1
#endif
#define SIG_OVER_MAX     400
#define SIG_OVER_SPEED   0.08
#define SIG_CVD_OUT_MAX  256

/* tuning: when STA leaves its learning phase the buffered frames are segmented again with the learned
 * timing (SEG ran on the default prior until then); 0 hands over the elements segmented so far */
#ifndef SIG_TUNE_REPLAY
#define SIG_TUNE_REPLAY  1
#endif
#define SIG_REPLAY_MAX   2048

extern FILE *vail_trace;   /* per-frame SIG trace of every channel when non-NULL (debug aid) */
extern FILE *vail_elems;   /* every classified element of every channel when non-NULL (debug aid):
                            * "id absStartFrame signedDuration kind pitchBin" */
void sig_init(Sig *s, int bin);
void sig_free(Sig *s);
/* Processes one frame; merges any new text into msg. */
void sig_process_frame(Sig *s, const float *spectrum, const float *peakRatio, float noiseFloor, float bandPeak,
                       Msg *msg);

/* --------------------------------------------------------------- BCM (03) */
#define BCM_HISTORY_FRAMES     200
#define BCM_SPEC_GATE          1e-12
#define BCM_SPEC_IIR_OLD       0.97
#define BCM_SPEC_IIR_NEW       0.03
#define BCM_RATIO_NEIGH        5
#define BCM_RATIO_MAXFACTOR    2.0
#define BCM_RATIO_IIR_OLD      0.8
#define BCM_RATIO_IIR_NEW      0.2
#define BCM_PEAK_THRESH        3.5
#define BCM_PEAK_WARMUP        200
#define BCM_PEAK_SCAN_LO       10
#define BCM_MASK_RADIUS        2
#define BCM_MERGE_PERIOD       32
#define BCM_MERGE_BIN_MAX_DIST 3
#define BCM_NOISE_SLICE        40
#define BCM_STALL_NEW          500
#define BCM_STALL_DECODING     1000
#define BCM_STALL_PAUSED       200
#define BCM_ISDECODING_MAX_AGE 140
#define BCM_SWITCH_HYST        2.0
#define BCM_QUALITY_SENTINEL   (-2.1475e9f)
#define BCM_REPLAY_INIT        199
#define BCM_REPLAY_BATCH       4
/* tuning: a channel becomes current only while it produces text at a sustained rate, so that a noise
 * channel's odd character (a receiver filter edge, a crash) does not take the display when the
 * station it follows stops; 0 restores 03 §4.10 */
#ifndef BCM_TUNE_TEXT_RATE
#define BCM_TUNE_TEXT_RATE     1
#endif
#define BCM_TEXT_DECAY         0.99f
#define BCM_MIN_TEXT_RATE      1.5f
#define BCM_PEAK_DECAY         0.998f   /* per frame, of the band's peak power (ENV dynamic range) */

enum { CH_STATE_NEW = 0, CH_STATE_DECODING, CH_STATE_PAUSED, CH_STATE_DEAD };

typedef struct {
    int state, ageCounter, lastSignal, bin;
    Sig *pipeline;
    Msg *msgA;
    int replayCounter;       /* frames still to catch up (see 12-open-items A11) */
    int nextFrame;           /* absolute frame number of the next spectrum this channel processes */
    int isCurrent, pinnedFlag;
    int id;
    /* per-frame accumulation for API */
    Char frameNew[MSG_CAPACITY]; int frameNewCount;
    int frameReplaced;       /* spec base: replaced records + their spaces, summed over the frame */
    int frameDelete;         /* host characters to remove before frameNew is appended (tuning E7) */
    float textRate;          /* new characters, leaky sum over about a second (BCM_TUNE_TEXT_RATE) */
} Channel;

typedef struct {
    float historyRing[BCM_HISTORY_FRAMES][N_BINS];
    const float *inputSpectrum;
    float smoothSpectrum[N_BINS], peakRatio[N_BINS];
    uint8_t peakFlags[N_BINS];
    Channel **channels; int nChannels, capChannels;
    int frameCount;
    float noiseFloor;
    float bandPeak;          /* peak-held strongest bin power (BCM_PEAK_DECAY: 20 dB in about 25 s) */
    Channel *currentChannel;
} Bcm;

void bcm_init(Bcm *b);
void bcm_free(Bcm *b);
void bcm_on_frame(Bcm *b, const float *spectrum);
void bcm_set_current(Bcm *b, Channel *ch);
Channel *bcm_new_channel(Bcm *b, int bin);
int bcm_is_decoding(const Channel *ch);

#endif
