/* SPDX-License-Identifier: GPL-3.0-or-later */
/* options — what the command line asked for. */
#ifndef OPTIONS_H
#define OPTIONS_H

#include "audio.h"

typedef struct {
    const char *path;                        /* a file, "-" for stdin, or NULL for a sound device */
    const char *device;
    const char *recordPath;
    int lo, hi;
    int channel;                             /* counting from 1; 0 mixes all of them */
    double seconds;                          /* stop after this much audio; 0: at the end of it */
    audio_raw raw;
    int view, plain, json, all;
    int list, help, version;
} options;

/* Returns 1 if the options can be used. If not, it has said why on stderr, and the exit status is 2. */
int options_parse(int argc, char **argv, options *o);

void options_print_help(void);

#endif
