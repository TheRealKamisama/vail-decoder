/* SPDX-License-Identifier: GPL-3.0-or-later */
/* text — decoded text, on its way to a person. Whether it is Morse at all is the engine's to say
 * (vail_channel_confidence). */
#ifndef TEXT_H
#define TEXT_H

/* As text_tidy needs at most this many bytes for len bytes of text. */
#define TEXT_TIDY_SIZE(len) (2 * (len) + 1)

/* Copies the engine's text to out, which has room for TEXT_TIDY_SIZE(len), and returns its length: spaces between words
 * become one, a prosign is written <SK> rather than sk, and anything unprintable becomes ?. types, if not NULL, is the
 * word class (VAIL_WORD_*) of each byte of text, and outTypes, which has the same room as out, gets that of each byte of
 * out. */
int text_tidy(const char *text, const unsigned char *types, int len, char *out, unsigned char *outTypes);

#endif
