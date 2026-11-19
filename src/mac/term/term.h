/*
 * Riptide - the Riptide Command Terminal on the classic Mac OS: what its files share.
 * term_main.c: the window, the console and the commands; term_os.c: the Amiga calls the tools
 * make (exec, dos, timer.device) on the Mac; term_bsd.c: bsdsocket.library and riptide.library
 * on the stack's calls for programs (RTMAC_API).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_MAC_TERM_H
#define RT_MAC_TERM_H
#include "mui/mm.h"
#include "rt/riptide.h"
#include "mac_ipp_private.h"

/* term_main.c */
void term_out(const char *s, LONG n);           /* to the console (LF line ends) */
LONG term_in(char *buf, LONG n);                /* a typed line for a running command; 0 at EOF */
int  term_in_ready(void);                       /* a typed line is waiting */
void term_yield(void *arg);                     /* take the events, at most a tick's sleep */
void term_poll(void);                           /* take the events now and then (cheap when recent) */
int  term_break_pending(void *arg);             /* Command-period since the command began */
void term_break_take(void);                     /* the break was delivered: forget it */
extern const char *term_args;                   /* the running command's arguments, for GetArgStr */

/* src/mac/mui/mm_os.c */
void mm_paths_init(void);

/* term_bsd.c */
int  term_api_open(void);
void term_api_close(void);
int  term_api_ok(void);
int  term_api_command(const char *line, char *out, ULONG outlen);

/* the console's file handles (Input(), Output()) */
#define TERM_IN  ((BPTR)0x7E7E0001)
#define TERM_OUT ((BPTR)0x7E7E0002)

/* ostraps.S */
unsigned long long mac_us(void);                /* Microseconds */
#endif
