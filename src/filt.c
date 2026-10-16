/* Filter primitives RUNMIN(n), RUNMAX(n), BOXAVG(n, k) (04 §5). */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "vail_internal.h"

void runext_init(RunExt *r, int n, int isMax)
{
    r->window = n;
    r->ringLen = 2 * n;
    r->ring = calloc((size_t)r->ringLen, sizeof(float));
    r->cursor = -1;
    r->acc = 0.0f;
    r->lastOut = 0.0f;
    r->isMax = isMax;
}

void runext_free(RunExt *r) { free(r->ring); r->ring = NULL; }

/* "better" means smaller for RUNMIN, larger for RUNMAX */
static int better(const RunExt *r, float a, float b) { return r->isMax ? (a > b) : (a < b); }

float runext_push(RunExt *r, float v)
{
    int n = r->window, k;
    float out;
    if (r->cursor == -1) {                          /* Prime */
        r->cursor = n - 1;
        for (k = 0; k < n; k++) r->ring[k] = v;
        r->acc = v;
        r->lastOut = v;
        return v;
    }
    r->cursor++;
    r->ring[r->cursor] = v;
    if (r->cursor == r->ringLen - 1) {              /* wrap: rebuild the suffix extrema */
        float acc = r->ring[r->cursor];
        for (k = n - 2; k >= 0; k--) {
            r->ring[k] = acc;
            if (better(r, r->ring[k + n], acc)) acc = r->ring[k + n];
        }
        out = acc;
        r->acc = v;
        r->cursor -= n;
    } else {
        float t;
        if (better(r, v, r->acc)) r->acc = v;
        t = r->ring[r->cursor - n];
        /* RUNMIN: out = (acc >= t) ? t : acc; RUNMAX mirrors it */
        if (r->isMax) out = (r->acc <= t) ? t : r->acc;
        else          out = (r->acc >= t) ? t : r->acc;
    }
    r->lastOut = out;
    return out;
}

void boxavg_init(BoxAvg *b, int n, int k)
{
    b->passes = k;
    b->window = n;
    b->ringLen = n + 1;
    b->cascadeA = calloc((size_t)(k + 1) * (size_t)b->ringLen, sizeof(float));
    b->cascadeB = calloc((size_t)(k + 1) * (size_t)b->ringLen, sizeof(float));
    b->weight = (float)pow((double)n, -(double)k);
    b->counter = 0;
    b->i1 = n - 1;
    b->J = n;
    b->i0 = 0;
}

void boxavg_free(BoxAvg *b)
{
    free(b->cascadeA); free(b->cascadeB);
    b->cascadeA = b->cascadeB = NULL;
}

static void cascade_step(BoxAvg *b, float *A, float v)
{
    int p, L = b->ringLen;
    A[b->J] = v;
    for (p = 1; p <= b->passes; p++) {
        v = A[p * L + b->i1] - A[(p - 1) * L + b->i0] + v;
        A[p * L + b->J] = v;
    }
}

float boxavg_push(BoxAvg *b, float v)
{
    int L = b->ringLen;
    float out;
    cascade_step(b, b->cascadeA, v);
    cascade_step(b, b->cascadeB, v);
    out = b->cascadeA[b->passes * L + b->J] * b->weight;
    b->i0 = (b->i0 + 1) % L;
    b->i1 = (b->i1 + 1) % L;
    b->J = (b->J + 1) % L;
    b->counter++;
    if (b->counter == 8 * L) {                      /* ResetScratch: restart the B running sums */
        size_t sz = (size_t)(b->passes + 1) * (size_t)L * sizeof(float);
        memcpy(b->cascadeA, b->cascadeB, sz);
        memset(b->cascadeB, 0, sz);
        b->counter = 0;
    }
    return out;
}
