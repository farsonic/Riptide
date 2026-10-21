/*
 * Riptide - diagnostics: the section profiler ("profile") and the debug
 * log by category ("debug"), both cheap enough to leave in every build.
 *
 * Profiler. Hot paths are bracketed with RT_PROF_ENTER(s)/RT_PROF_EXIT(s).
 * While profiling is off each probe is one test of the global rt_prof_on.
 * Sections run with the stack lock held and nest: a section's time is
 * exclusive of the sections it calls (one stack of open sections, under
 * the lock; rt_unlock() closes what is still open, so a section never
 * spans a sleep). Counters (RT_PROF_T0/RT_PROF_ADD) are for code outside
 * the lock: the SANA-II receive copy (driver context), lock waits, and
 * the Riptide process' idle time in Wait(). Times come from
 * rt_prof_ticks(), a free running counter of rt_prof_hz() per second
 * (AmigaOS: the EClock; host: microseconds). Wrapping is fine: only
 * differences are used, and no single interval lasts longer than a wrap.
 *
 * Debug log. RT_DBG(cat, fmt, ...) appends a time stamped line to a ring
 * buffer in memory when 'cat' is enabled ("debug tcp,driver"); otherwise
 * it is one test of rt_dbg_mask and the arguments are not evaluated. Call
 * it with the stack lock held. "debug show" prints the ring, "debug dump
 * FILE" writes it through rt_dbg_dump_hook (set by the OS layer).
 *
 * Build with -DRT_NO_DIAG to compile every probe out.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_DIAG_H
#define RT_DIAG_H

#include "rt_types.h"

/* ---- profiler ---- */

/* Sections (stack of open sections, exclusive times) */
#define RT_PROF_STACK      0   /* Riptide process: work under the lock outside the sections below */
#define RT_PROF_TIMERS     1   /* timer callbacks (TCP retransmit/delayed ACK, ARP, DHCP...) */
#define RT_PROF_IP_IN      2
#define RT_PROF_IP6_IN     3
#define RT_PROF_TCP_IN     4
#define RT_PROF_UDP_IN     5
#define RT_PROF_TCP_OUT    6   /* building and sending TCP segments, ACKs included */
#define RT_PROF_CSUM       7   /* rt_csum_partial(): checksums without a copy */
#define RT_PROF_TO_APP     8   /* copying received data to the application */
#define RT_PROF_FROM_APP   9   /* copying data to send from the application */
#define RT_PROF_DRV_TX     10  /* handing a frame to the driver (SendIO / MNI_Send) */
#define RT_PROF_NSECT      11
/* Counters (not on the section stack) */
#define RT_PROF_DRV_RX     11  /* driver copying a received frame into Riptide (CopyToBuff) */
#define RT_PROF_LOCK_APP   12  /* applications waiting for the stack lock */
#define RT_PROF_LOCK_STACK 13  /* the Riptide process waiting for the stack lock */
#define RT_PROF_IDLE       14  /* the Riptide process waiting for work */
#define RT_PROF_N          15

#ifndef RT_NO_DIAG
extern volatile int rt_prof_on;
extern int rt_prof_depth;
void rt_prof_enter(int s);
void rt_prof_exit(int s);
void rt_prof_unwind(void);           /* close every open section (lock release) */
void rt_prof_add(int c, u32 ticks);  /* counters */
#define RT_PROF_ENTER(s) do { if (rt_prof_on) rt_prof_enter(s); } while (0)
#define RT_PROF_EXIT(s)  do { if (rt_prof_on) rt_prof_exit(s); } while (0)
#define RT_PROF_T0()     (rt_prof_on ? rt_prof_ticks() | 1u : 0u)
#define RT_PROF_ADD(c, t0) do { if (t0) rt_prof_add((c), rt_prof_ticks() - (t0)); } while (0)
#else
#define rt_prof_on 0
#define rt_prof_depth 0
static inline void rt_prof_unwind(void) { }
static inline void rt_prof_add(int c, u32 ticks) { (void)c; (void)ticks; }
#define RT_PROF_ENTER(s) do { } while (0)
#define RT_PROF_EXIT(s)  do { } while (0)
#define RT_PROF_T0()     0u
#define RT_PROF_ADD(c, t0) do { (void)(t0); } while (0)
#endif

/* OS layer: the profiling clock */
u32 rt_prof_ticks(void);
u32 rt_prof_hz(void);

/* "profile start|stop|reset|show"; output (or the error) goes to out */
int rt_prof_command(int argc, char **argv, char *out, u32 outlen);
/* Text of "profile show" */
int rt_prof_report(char *out, u32 outlen);

/* ---- debug log ---- */

#define RT_DBG_TCP     0x0001
#define RT_DBG_IP      0x0002   /* IPv4, ICMP, fragments, routing */
#define RT_DBG_IP6     0x0004   /* IPv6, ICMPv6 */
#define RT_DBG_DRIVER  0x0008   /* SANA-II, MNI */
#define RT_DBG_TIMER   0x0010
#define RT_DBG_SOCKET  0x0020   /* socket calls, UDP */
#define RT_DBG_DNS     0x0040
#define RT_DBG_DHCP    0x0080   /* DHCP and DHCPv6 */
#define RT_DBG_ARP     0x0100   /* ARP and IPv6 neighbour discovery */
#define RT_DBG_LOG     0x0200   /* copies of the log messages (rt_log) */
#define RT_DBG_ALL     0x03ff

#ifndef RT_NO_DIAG
extern u32 rt_dbg_mask;
void rt_dbg_printf(u32 cat, const char *fmt, ...) RT_PRINTF(2, 3);
#define RT_DBG(cat, ...) do { if (rt_dbg_mask & (cat)) rt_dbg_printf((cat), __VA_ARGS__); } while (0)
#else
#define rt_dbg_mask 0u
#define RT_DBG(cat, ...) do { } while (0)
#endif
/* Append one line as category 'cat' (used for RT_DBG_LOG). Lock held. */
void rt_dbg_line(u32 cat, const char *text);

/* "debug [CATS|all|none] [size=KB] | show [N] | clear | dump FILE" */
int rt_dbg_command(int argc, char **argv, char *out, u32 outlen);
/* Copy the ring (oldest complete line first) to buf; returns the length.
 * With buf NULL returns the length needed. */
u32 rt_dbg_snapshot(char *buf, u32 len);
/* Set by the OS layer: write the ring to 'path' (called with the lock
 * held; may drop it while writing). NULL: "debug dump" is not supported. */
extern int (*rt_dbg_dump_hook)(const char *path, char *err, u32 errlen);

/* Registers net.riptide.profile (from rt_init) */
void rt_diag_init(void);

#endif
