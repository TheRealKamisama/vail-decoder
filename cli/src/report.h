/* SPDX-License-Identifier: GPL-3.0-or-later */
/* report — messages for the user, on stderr. A full-screen view owns the terminal, so while one is open they are held
 * until it has closed. */
#ifndef REPORT_H
#define REPORT_H

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 1, 2)))
#endif
void report(const char *fmt, ...);       /* "vail-cli: " goes in front and a newline after */

void report_hold(void);
void report_release(void);               /* prints what was held */
const char *report_last(void);           /* the newest message held, or NULL */

#endif
