/* SPDX-License-Identifier: GPL-3.0-or-later */
/* text — decoded text, on its way to a person: how it reads, and whether it is worth reading. */
#ifndef TEXT_H
#define TEXT_H

/* As text_tidy needs at most this many bytes for len bytes of text. */
#define TEXT_TIDY_SIZE(len) (2 * (len) + 1)

/* Copies the engine's text to out, which has room for TEXT_TIDY_SIZE(len), and returns its length: spaces between words
 * become one, a prosign is written <SK> rather than sk, and anything unprintable becomes ?. */
int text_tidy(const char *text, int len, char *out);

/* Whether the end of a channel's text reads like Morse rather than like other audio decoded as Morse. The engine gives
 * no measure of this, so it is judged from the text; see docs/engine-confidence.md for what that can and cannot catch. */
int text_looks_like_morse(const char *text, int len);

#endif
