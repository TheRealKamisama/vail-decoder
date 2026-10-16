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
#define FE_WINDOW_CUTOFF   0.003125
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

typedef struct {
    BoxAvg envSmoother, envThrShaper;
    RunExt envPeakHold, envMaxShort, envThrMinHold;
    float envThrUpper;
    int envMeanLen;
    float mem[ENV_SHIFT_ROWS][ENV_SHIFT_ROW_FLOATS];   /* envShiftMem */
    float envKeyProb, envMeanSquare, envStrength;
    float envEnvelope, envThreshold;                   /* same-frame copies */
    int envCarrier;
} Env;

void env_init(Env *e);
void env_free(Env *e);
void env_apply(Env *e, float prominence, float amplitude);

/* --------------------------------------------------------------- SEG (05) */
#define SEG_SILENCE_LEVEL   1.3
#define SEG_ON_MAX          20
#define SEG_OFF_MAX         30
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
    int carrierDrop;
    DurState durState[SEG_ON_MAX + SEG_OFF_MAX + 1];
    Element out[SEG_EMIT_CAPACITY];
    int outCount;
    struct Sta *statsRef;
} Seg;

void seg_init(Seg *s, struct Sta *sta);
void seg_build_duration_tables(Seg *s);
void seg_update(Seg *s, int bit, float keyProb);

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
    Chr chr;
    Wrd wrd;
    Char hist[WRD_WINDOW_MAX]; int histCount;
    int prevCarrier;
    int flushRequest;
    int signalPresent;
    float quality;
    int hasText;
    int segGate;
} Sig;

extern FILE *vail_trace;   /* per-frame SIG trace of every channel when non-NULL (debug aid) */
void sig_init(Sig *s, int bin);
void sig_free(Sig *s);
/* Processes one frame; merges any new text into msg. */
void sig_process_frame(Sig *s, const float *spectrum, const float *peakRatio, float noiseFloor, Msg *msg);

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
} Channel;

typedef struct {
    float historyRing[BCM_HISTORY_FRAMES][N_BINS];
    const float *inputSpectrum;
    float smoothSpectrum[N_BINS], peakRatio[N_BINS];
    uint8_t peakFlags[N_BINS];
    Channel **channels; int nChannels, capChannels;
    int frameCount;
    float noiseFloor;
    Channel *currentChannel;
} Bcm;

void bcm_init(Bcm *b);
void bcm_free(Bcm *b);
void bcm_on_frame(Bcm *b, const float *spectrum);
void bcm_set_current(Bcm *b, Channel *ch);
Channel *bcm_new_channel(Bcm *b, int bin);
int bcm_is_decoding(const Channel *ch);

#endif
