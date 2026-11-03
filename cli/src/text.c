/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <ctype.h>
#include <string.h>
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

/* Speech and music turn into many short channels of the Morse code's shortest letters, in words of one letter. Real
 * traffic does not look like that, even when decoded badly: over the measurements in docs/engine-confidence.md these five
 * letters are a median of 96 % of garbage and 25 % of CW (95th percentile 41 %). A word of nothing but them can be real, as
 * TEST mostly is, so they count only among short words. A digit alone is real too, as in 1 1 1 0 0 0, so a word of one
 * symbol counts in general only when many different ones are scattered about, as from music. Text is judged by its last
 * characters, so that a channel can recover from what it decoded earlier. */
#define WINDOW_BYTES       160
#define MIN_SYMBOLS        12      /* with fewer, there are no shares to speak of */
#define MAX_ONE_LETTER     0.45    /* of the words, being one of the five letters */
#define MAX_ONE_SYMBOL     0.55    /* of the words, being any one symbol ... */
#define SCATTERED_SYMBOLS  4       /* ... when at least this many different symbols do it */
#define MAX_SHORT_LETTERS  0.72
#define SHORT_WORD_MEAN    2.5

static int is_short_letter(int c) { return c && strchr("ETIAN", c) != NULL; }
static int is_digit(int c) { return c >= '0' && c <= '9'; }

int text_looks_like_morse(const char *text, int len)
{
    int symbols = 0, shortLetters = 0, words = 0, oneLetter = 0, inWord = 0, wordShort = 0, wordLong = 0, wordDigit = 0;
    int oneSymbol = 0, scattered = 0, sawWord = 0, i;
    unsigned char seenAlone[256] = { 0 };
    if (len > WINDOW_BYTES) {
        text += len - WINDOW_BYTES;
        len = WINDOW_BYTES;
    }
    for (i = 0; i <= len; i++) {
        if (i == len || text[i] == ' ') {
            if (inWord) {
                words++;
                oneLetter += inWord == 1 && wordShort;
                if (inWord == 1) {
                    unsigned char c = (unsigned char)text[i - 1];
                    oneSymbol++;
                    scattered += !seenAlone[c];
                    seenAlone[c] = 1;
                }
                sawWord |= wordDigit || (inWord >= 2 && wordLong);     /* TU, 73, 5NN or 5, not TE or EE */
            }
            inWord = wordShort = wordLong = wordDigit = 0;
            continue;
        }
        symbols++;
        inWord++;
        if (is_prosign(text + i, len - i)) {
            wordLong = 1;
            i++;
        } else if (is_short_letter(text[i])) {
            shortLetters++;
            wordShort = 1;
        } else {
            wordLong = 1;
            wordDigit |= is_digit(text[i]);
        }
    }
    if (symbols < MIN_SYMBOLS) return sawWord;
    if (oneLetter > MAX_ONE_LETTER * words) return 0;
    if (oneSymbol > MAX_ONE_SYMBOL * words && scattered >= SCATTERED_SYMBOLS) return 0;
    if (shortLetters > MAX_SHORT_LETTERS * symbols && symbols < SHORT_WORD_MEAN * words) return 0;
    return 1;
}
