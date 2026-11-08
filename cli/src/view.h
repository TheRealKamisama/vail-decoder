/* SPDX-License-Identifier: GPL-3.0-or-later */
/* view — the full-screen view, on the controlling terminal, so stdin can carry samples and stdout can carry lines. It is
 * built only where ncurses is found; without it, view_available gives 0 and view_open gives NULL. */
#ifndef VIEW_H
#define VIEW_H

#include "band.h"

typedef struct view view;

typedef struct {
    const char *clock;                       /* the time to show */
    int hasLevel;                            /* the input is live, so its level is measured */
    int levelKnown;                          /* there has been enough audio to measure */
    double levelDb;                          /* the input's level; -inf if it is silent */
    int clipping;
    int finished;                            /* the audio ended, and the view is waiting to be closed */
    double lostSeconds;                      /* of audio, because decoding fell behind */
    long hiddenLines;
    int showAll;
} view_status;

int view_available(void);

/* source: what is being decoded. NULL if there is no terminal to use; reason then says why, for messages. */
view *view_open(const char *source, int loHz, int hiHz, const char **reason);
void  view_close(view *v);

/* Handles the keys that have been pressed. Returns 1 once the user has asked to quit. */
int   view_keys(view *v);

/* Keeps a finished line for showing, with its callsigns highlighted, and for printing with view_print_lines. types: as
 * band_line.types, or NULL. */
void  view_line(view *v, const char *stamp, int hz, const char *text, const unsigned char *types);

void  view_draw(view *v, const band *b, const view_status *status);

/* Prints the lines kept, for use after closing the terminal side: returns how many no longer fitted and were lost. */
long  view_print_lines(const view *v, void (*print)(const char *stamp, int hz, const char *text));

/* Frees what is left after view_close. */
void  view_free(view *v);

#endif
