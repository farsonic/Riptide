/*
 * Riptide - the Amiga's own defaults, where they differ from the core's.
 * stackproc.c applies the lines after rt_init() and before riptide.conf
 * (which can override them); RiptidePrefs shows them as the defaults of
 * its settings, and tests/t_prefs_conf.c checks its defaults against the
 * core with these lines applied. Plain C, no AmigaOS headers.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_AMIGA_DEFAULTS_H
#define RT_AMIGA_DEFAULTS_H

/* riptide.conf lines. A connection starts with a 64 KB receive buffer
 * (about what the 48 queued SANA-II reads absorb in one burst) and
 * auto-tuning grows it up to 256 KB only when the program drains that
 * fast: a 68020 never gets there, a 68040/060 or PiStorm on a 30-50 ms
 * Internet path needs it (a 32 KB/64 KB cap held them near 1 MB/s).
 * mDNS answers to the host name with .local added ("mdns off" in the
 * file turns it off); the self-test leaves the non-sysctl lines out.
 * An initializer: static const char *const v[] = RT_AMIGA_DEFAULTS; */
#define RT_AMIGA_DEFAULTS { \
	"sysctl net.inet.tcp.recvspace=65536", \
	"sysctl net.inet.tcp.rmem_max=262144", \
	"mdns on", \
	0 }

/* receive requests kept at a SANA-II driver for IP ("reads=") */
#define RT_AMIGA_READS_DEF 48

#endif
