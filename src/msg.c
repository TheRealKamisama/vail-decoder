/* SPDX-License-Identifier: GPL-3.0-or-later */
/* MSG — message model (08-message-model.md). */
#include <string.h>
#include "vail_internal.h"

void msg_init(Msg *m)
{
    memset(m, 0, sizeof *m);
    m->lastIdxE = -2147483647 - 1;
}

static int char_equal(const Char *a, const Char *b)
{
    return a->charCode == b->charCode && a->wordType == b->wordType &&
           a->elementOrdinal == b->elementOrdinal && a->lowSnr == b->lowSnr &&
           a->wordSpaceProb == b->wordSpaceProb && a->wordEnd == b->wordEnd;
}

void msg_merge_chars(Msg *m, const Char *batch, int n)     /* 08 §4.1 */
{
    int overlap = 0, veq = 0;
    if (n <= 0) { m->replacedCount = 0; m->newTailCount = 0; return; }
    if (n > MSG_CAPACITY) { batch += n - MSG_CAPACITY; n = MSG_CAPACITY; }    /* 12-open-items C7 */
    while (overlap < m->count && m->chars[m->count - 1 - overlap].elementOrdinal >= batch[0].elementOrdinal)
        overlap++;
    while (veq < overlap && veq < n && char_equal(&m->chars[m->count - overlap + veq], &batch[veq]))
        veq++;
    memcpy(m->replaced, m->chars + m->count - overlap, (size_t)overlap * sizeof(Char));
    m->replacedCount = overlap;
    memcpy(m->newTail, batch + veq, (size_t)(n - veq) * sizeof(Char));
    m->newTailCount = n - veq;
    m->veq = veq;
    m->count -= overlap - veq;
    if (m->count + m->newTailCount > MSG_CAPACITY) {
        m->count -= MSG_SHIFT;
        memmove(m->chars, m->chars + MSG_SHIFT, (size_t)m->count * sizeof(Char));
    }
    if (m->newTailCount == 0) {
        m->clearFlagA = m->clearFlagB = 0;
    } else {
        memcpy(m->chars + m->count, m->newTail, (size_t)m->newTailCount * sizeof(Char));
        m->count += m->newTailCount;
    }
}

int msg_chars_since(const Msg *m, int32_t idIdx, Char *out)  /* 08 §4.5, default of 12-open-items D1 */
{
    int i = m->count - 1, scanned = 0, start, n;
    while (i >= 0 && scanned < MSG_BACKSCAN && m->chars[i].elementOrdinal > idIdx) { i--; scanned++; }
    /* the locating character is excluded; with no match inside the window the scanned
     * suffix (at most MSG_BACKSCAN characters) is returned */
    start = i + 1;
    n = m->count - start;
    memcpy(out, m->chars + start, (size_t)n * sizeof(Char));
    return n;
}

int msg_rendered_len(const Char *c)
{
    return 1 + (c->wordEnd == WE_SHORT ? 1 : (c->wordEnd == WE_LONG ? 2 : 0));
}

int msg_render(const Msg *m, char *buf, int cap)
{
    int i, n = 0;
    for (i = 0; i < m->count && n < cap - 3; i++) {
        buf[n++] = (char)m->chars[i].charCode;
        if (m->chars[i].wordEnd == WE_SHORT) buf[n++] = ' ';
        else if (m->chars[i].wordEnd == WE_LONG) { buf[n++] = ' '; buf[n++] = ' '; }
    }
    buf[n] = 0;
    return n;
}
