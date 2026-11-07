/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <ctype.h>
#include "text.h"

/* The engine writes a prosign as two lower-case letters. */
static int is_prosign(const char *s, int left)
{
    return left >= 2 && islower((unsigned char)s[0]) && islower((unsigned char)s[1]);
}

int text_tidy(const char *text, int len, char *out)
{
    int i = 0, n = 0;
    while (i < len && text[i] == ' ') i++;
    while (i < len) {
        unsigned char c = (unsigned char)text[i];
        if (c == ' ') {
            while (i < len && text[i] == ' ') i++;
            if (i < len) out[n++] = ' ';
        } else if (is_prosign(text + i, len - i)) {
            out[n++] = '<';
            out[n++] = (char)toupper((unsigned char)text[i]);
            out[n++] = (char)toupper((unsigned char)text[i + 1]);
            out[n++] = '>';
            i += 2;
        } else {
            out[n++] = c > 32 && c < 127 ? (char)toupper(c) : '?';
            i++;
        }
    }
    out[n] = 0;
    return n;
}
