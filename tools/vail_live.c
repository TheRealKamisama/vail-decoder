/* vail_live — live Morse decoding from a Windows sound input device (e.g. a radio's audio output).
 *
 *   vail_live -l                         list input devices
 *   vail_live [-d N] [-p HZ] [-r rec.wav] [-a]
 *     -d N       capture from input device N (default: the system default input)
 *     -p HZ      lock onto the signal at HZ (API_PitchLock); default: follow the strongest signal
 *     -r F       also record the captured audio (8 kHz, 16-bit mono) to WAV file F
 *     -a         on exit, print the text of every channel heard (with its pitch)
 *
 * Audio is captured at 8000 Hz mono 16-bit (Windows resamples from the device rate). The decoder runs
 * with the operating mode on, so re-decoded text is corrected in place. Stop with Ctrl+C.
 * Build: ./build-windows.sh (cross-compiles with CC, a Windows-targeting C compiler).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/vail.h"
#include "wav.h"

#define NBUF        8
#define BUF_FRAMES  10                               /* 10 frames = 850 samples ≈ 106 ms per buffer */
#define BUF_SAMPLES (BUF_FRAMES * VAIL_HOP_SAMPLES)
#define HOST_MAX    (1 << 16)

static volatile LONG g_stop = 0;

static BOOL WINAPI on_ctrl(DWORD type) { (void)type; InterlockedExchange(&g_stop, 1); return TRUE; }

static void list_devices(void)
{
    UINT i, n = waveInGetNumDevs();
    printf("Input devices:\n");
    for (i = 0; i < n; i++) {
        WAVEINCAPSA caps;
        if (waveInGetDevCapsA(i, &caps, sizeof caps) == MMSYSERR_NOERROR) printf("  %u: %s\n", i, caps.szPname);
    }
    if (n == 0) printf("  (none)\n");
}

/* recording: write a WAV header now, patch the sizes on close */
static FILE *rec_open(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return NULL;
    fwrite("RIFF", 1, 4, f); wav_put32(f, 0); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); wav_put32(f, 16); wav_put16(f, 1); wav_put16(f, 1);
    wav_put32(f, VAIL_SAMPLE_RATE); wav_put32(f, VAIL_SAMPLE_RATE * 2); wav_put16(f, 2); wav_put16(f, 16);
    fwrite("data", 1, 4, f); wav_put32(f, 0);
    return f;
}

static void rec_close(FILE *f, long samples)
{
    fseek(f, 4, SEEK_SET); wav_put32(f, (uint32_t)(36 + 2 * samples));
    fseek(f, 40, SEEK_SET); wav_put32(f, (uint32_t)(2 * samples));
    fclose(f);
}

/* status line: state, pitch, speed, SNR, then the tail of the decoded text */
static void redraw(vail_api *h, const char *host, int hostLen)
{
    static const char *states[] = { "----", "pause", "DEC", "LOCK", "LOCK+DEC" };
    char line[160], tail[90];
    int st = API_State(h), start = hostLen > 70 ? hostLen - 70 : 0, k, n = 0;
    for (k = start; k < hostLen; k++) {
        char c = host[k];
        if (c == '\n' || c == '\r') { n = 0; continue; }   /* show only the text since the last switch */
        tail[n++] = c;
    }
    tail[n] = 0;
    if (st == API_STATE_NONE)
        snprintf(line, sizeof line, "[%-8s %4s Hz %3s wpm %3s dB] %s", states[st], "-", "-", "-", tail);
    else
        snprintf(line, sizeof line, "[%-8s %4d Hz %3d wpm %3d dB] %s", states[st],
                 API_Pitch(h), API_Wpm(h), API_Snr(h), tail);
    printf("\r%-118.118s", line);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    UINT dev = WAVE_MAPPER;
    int lockHz = 0, all = 0, i, frames = 0;
    const char *recPath = NULL;
    FILE *rec = NULL;
    long recSamples = 0;
    WAVEFORMATEX fmt;
    HWAVEIN win;
    HANDLE ev;
    WAVEHDR hdr[NBUF];
    static int16_t bufs[NBUF][BUF_SAMPLES];
    static char host[HOST_MAX];
    int hostLen = 0, next = 0;
    vail_fe *fe;
    vail_api *h;
    MMRESULT r;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-l")) { list_devices(); return 0; }
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) dev = (UINT)atoi(argv[++i]);
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) lockHz = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) recPath = argv[++i];
        else if (!strcmp(argv[i], "-a")) all = 1;
        else {
            fprintf(stderr, "usage: %s -l | [-d N] [-p HZ] [-r rec.wav] [-a]\n", argv[0]);
            return 2;
        }
    }

    memset(&fmt, 0, sizeof fmt);
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 1;
    fmt.nSamplesPerSec = VAIL_SAMPLE_RATE;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 2;
    fmt.nAvgBytesPerSec = VAIL_SAMPLE_RATE * 2;
    ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    r = waveInOpen(&win, dev, &fmt, (DWORD_PTR)ev, 0, CALLBACK_EVENT);
    if (r != MMSYSERR_NOERROR) {
        fprintf(stderr, "cannot open input device %d (error %u); try -l to list devices\n",
                dev == WAVE_MAPPER ? -1 : (int)dev, r);
        return 1;
    }
    for (i = 0; i < NBUF; i++) {
        memset(&hdr[i], 0, sizeof hdr[i]);
        hdr[i].lpData = (LPSTR)bufs[i];
        hdr[i].dwBufferLength = sizeof bufs[i];
        waveInPrepareHeader(win, &hdr[i], sizeof hdr[i]);
        waveInAddBuffer(win, &hdr[i], sizeof hdr[i]);
    }
    if (recPath && !(rec = rec_open(recPath))) fprintf(stderr, "cannot write %s\n", recPath);

    fe = vail_fe_new();
    h = API_New();
    API_ModeSet(h, 1);                                 /* deletes let us correct re-decoded text */
    if (lockHz > 0) API_PitchLock(h, lockHz);
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    printf("vail live decoder - listening (Ctrl+C to stop). Tune a CW signal to roughly 300-1100 Hz audio.\n");
    waveInStart(win);

    while (!g_stop) {
        WaitForSingleObject(ev, 200);
        while (hdr[next].dwFlags & WHDR_DONE) {        /* buffers complete in queue order */
            WAVEHDR *hd = &hdr[next];
            int nsamp = (int)(hd->dwBytesRecorded / 2), k;
            const int16_t *p = (const int16_t *)hd->lpData;
            if (rec) { fwrite(p, 2, (size_t)nsamp, rec); recSamples += nsamp; }
            for (k = 0; k + VAIL_HOP_SAMPLES <= nsamp; k += VAIL_HOP_SAMPLES) {
                float spec[VAIL_N_BINS];
                int c, cnt, d;
                memcpy(spec, vail_fe_process(fe, p + k), sizeof spec);
                API_Feed(h, spec);
                frames++;
                d = API_DeleteCount(h);
                hostLen = d > hostLen ? 0 : hostLen - d;
                cnt = API_CharCount(h);
                for (c = 0; c < cnt; c++) {
                    if (hostLen == HOST_MAX - 1) {     /* keep the newest half */
                        memmove(host, host + HOST_MAX / 2, HOST_MAX / 2);
                        hostLen -= HOST_MAX / 2;
                    }
                    host[hostLen++] = (char)API_CharAt(h, c).ch;
                }
            }
            hd->dwFlags &= ~WHDR_DONE;
            waveInAddBuffer(win, hd, sizeof *hd);
            next = (next + 1) % NBUF;
            if (frames % 10 == 0) redraw(h, host, hostLen);
        }
    }

    waveInStop(win);
    waveInReset(win);
    for (i = 0; i < NBUF; i++) waveInUnprepareHeader(win, &hdr[i], sizeof hdr[i]);
    waveInClose(win);
    printf("\n\nDecoded text:\n");
    host[hostLen] = 0;
    for (i = 0; i < hostLen; i++) if (host[i] == '>' && i > 0 && host[i - 1] == '\n') host[i] = ' ';
    printf("%s\n", host);
    if (all) {
        static char text[4096];
        int c, nc = vail_channel_count_all(h);
        printf("\nChannels still active:\n");
        for (c = 0; c < nc; c++) {
            int hz, id;
            if (vail_channel_text(h, c, text, sizeof text, &hz, &id) > 0) printf("%5d Hz: %s\n", hz, text);
        }
    }
    if (rec) { rec_close(rec, recSamples); printf("recorded %.1f s to %s\n", recSamples / 8000.0, recPath); }
    API_Free(h);
    vail_fe_free(fe);
    CloseHandle(ev);
    return 0;
}
