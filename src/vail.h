/* SPDX-License-Identifier: GPL-3.0-or-later */
/* vail — real-time CW (Morse) decoding core. Public host interface (cleanroom/09-host-interface.md). */
#ifndef VAIL_H
#define VAIL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VAIL_N_BINS        128   /* Spectrum length */
#define VAIL_HOP_SAMPLES   85    /* new samples per frame @ 8000 Hz */
#define VAIL_SAMPLE_RATE   8000

#define API_NOT_AVAILABLE  (-1)

enum {
    API_STATE_NONE = 0,
    API_STATE_PAUSE = 1,
    API_STATE_DECODING = 2,
    API_STATE_LOCKED_PAUSE = 3,
    API_STATE_LOCKED_DECODING = 4
};

/* Spectrum front end (FE, 02). */
typedef struct vail_fe vail_fe;
vail_fe *vail_fe_new(void);
void vail_fe_free(vail_fe *fe);
/* Consumes exactly VAIL_HOP_SAMPLES samples, returns a pointer to the reused 128-bin Spectrum. */
const float *vail_fe_process(vail_fe *fe, const int16_t *samples);

/* Host-visible character object (09 §2.10). */
typedef struct {
    uint8_t ch;
    int32_t wordType;
    uint8_t lowSnr;
} vail_char;

typedef struct vail_api vail_api;   /* opaque handle H */

vail_api *API_New(void);
void API_Free(vail_api *h);
int  API_Feed(vail_api *h, float *spectrum);   /* spectrum: VAIL_N_BINS values, non-finite values are zeroed in place */
int  API_State(const vail_api *h);
int  API_Pitch(const vail_api *h);
void API_PitchLock(vail_api *h, int hz);
int  API_Snr(const vail_api *h);
int  API_Wpm(const vail_api *h);
int  API_ChannelCount(const vail_api *h);
int  API_CharCount(const vail_api *h);
vail_char API_CharAt(const vail_api *h, int i);
int  API_DeleteCount(const vail_api *h);
int  API_ModeGet(const vail_api *h);
void API_ModeSet(vail_api *h, int v);

/* Extensions for tools (not part of the 09 surface): full message text of the current channel
 * (characters plus materialised spaces), and of every channel. Return the string length. */
int vail_current_text(const vail_api *h, char *buf, int cap);
int vail_channel_count_all(const vail_api *h);
int vail_channel_text(const vail_api *h, int idx, char *buf, int cap, int *pitchHz, int *id);

#ifdef __cplusplus
}
#endif
#endif
