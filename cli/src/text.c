/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <ctype.h>
#include "text.h"
#include "vail.h"

/* The engine writes a prosign as two lower-case letters. */
static int is_prosign(const char *s, int left)
{
    return left >= 2 && islower((unsigned char)s[0]) && islower((unsigned char)s[1]);
}

int text_tidy(const char *text, const unsigned char *types, int len, char *out, unsigned char *outTypes)
{
    int i = 0, n = 0;
    while (i < len && text[i] == ' ') i++;
    while (i < len) {
        unsigned char c = (unsigned char)text[i];
        int from = n, step = 1, type = types ? types[i] : VAIL_WORD_GENERIC;
        if (c == ' ') {
            while (i < len && text[i] == ' ') i++;
            if (i < len) out[n++] = ' ';
            step = 0;
            type = VAIL_WORD_GENERIC;
        } else if (is_prosign(text + i, len - i)) {
            out[n++] = '<';
            out[n++] = (char)toupper((unsigned char)text[i]);
            out[n++] = (char)toupper((unsigned char)text[i + 1]);
            out[n++] = '>';
            step = 2;
        } else {
            out[n++] = c > 32 && c < 127 ? (char)toupper(c) : '?';
        }
        if (outTypes) for (; from < n; from++) outTypes[from] = (unsigned char)type;
        i += step;
    }
    out[n] = 0;
    return n;
}
