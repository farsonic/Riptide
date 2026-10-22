/*
 * Riptide - minimal timer.device definitions (plus SANA-II).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_AMIGA_DEVICES_H
#define RT_AMIGA_DEVICES_H

#include "exec.h"

/* ---- timer.device ---- */
#define UNIT_MICROHZ 0
#define UNIT_VBLANK  1
#define UNIT_ECLOCK  2
#define TR_ADDREQUEST (CMD_NONSTD + 0)
#define TR_GETSYSTIME (CMD_NONSTD + 1)
#define TR_SETSYSTIME (CMD_NONSTD + 2)

struct timeval {
	ULONG tv_secs;
	ULONG tv_micro;
};
struct EClockVal {
	ULONG ev_hi;
	ULONG ev_lo;
};
struct timerequest {
	struct IORequest tr_node;
	struct timeval tr_time;
};

extern struct Device *TimerBase;

/* ---- locale.library (the GMT offset only) ---- */
struct Locale {
	STRPTR loc_LocaleName;
	STRPTR loc_LanguageName;
	STRPTR loc_PrefLanguages[10];
	ULONG  loc_Flags;
	ULONG  loc_CodeSet;
	ULONG  loc_CountryCode;
	ULONG  loc_TelephoneCode;
	LONG   loc_GMTOffset;        /* minutes west of GMT */
	/* more fields follow; only ever read through OpenLocale()'s pointer */
};

#include "sana2.h"

#endif
