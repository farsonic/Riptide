/*
 * Riptide - choice of the CPU specific checksum and copy routines.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * rt_arch holds the routines the hot paths call (see rt_arch.h). The OS
 * layer sets rt_arch_cpu (AmigaOS: SysBase->AttnFlags; host m68k test
 * builds: $RT_CPU) and rt_init() selects "auto". Writes of the pointers
 * are single aligned stores, so a SANA-II interrupt callback that reads
 * rt_arch.copy while the selection changes sees either the old or the new
 * routine, both of which are correct.
 */
#include "rt/rt_arch.h"
#include "rt/rt_os.h"
#include "rt/rt_errno.h"
#include "rt/rt_sysctl.h"

#ifdef RT_ARCH_M68K_ASM
/* Riptide needs a 68020; its routines are a safe start before rt_init(). */
struct rt_arch_ops rt_arch = { rt_csum_020, rt_csum_copy_020, rt_copy_020m, rt_copy_020m };
#else
struct rt_arch_ops rt_arch = { rt_csum_partial_c, rt_csum_copy_c, rt_copy_c, rt_copy_c };
#endif

u32 rt_arch_cpu;

static s32 arch_move16 = 1;          /* sysctl riptide.cpu.move16 */
static int arch_move16_vetoed;
static char arch_choice[8] = "auto"; /* last successful rt_arch_select() */
static char arch_model[12];          /* riptide.cpu.model */
static char arch_routines[96];       /* riptide.cpu.routines */

typedef u32  (*csum_fn)(const void *, u32, u32);
typedef u32  (*csum_copy_fn)(void *, const void *, u32, u32);
typedef void (*copy_fn)(void *, const void *, u32);

static const char *csum_name(csum_fn f)
{
#ifdef RT_ARCH_M68K_ASM
	if (f == rt_csum_020)
		return "csum_020";
#endif
	return f == rt_csum_partial_c ? "csum_c" : "?";
}

static const char *csum_copy_name(csum_copy_fn f)
{
#ifdef RT_ARCH_M68K_ASM
	if (f == rt_csum_copy_020)
		return "csum_copy_020";
#endif
	return f == rt_csum_copy_c ? "csum_copy_c" : "?";
}

static const char *copy_name(copy_fn f)
{
#ifdef RT_ARCH_M68K_ASM
	if (f == rt_copy_020)
		return "copy_020";
	if (f == rt_copy_020m)
		return "copy_020m";
	if (f == rt_copy_040)
		return "copy_040";
#endif
	return f == rt_copy_c ? "copy_c" : "?";
}

static void arch_update_text(void)
{
	if (rt_arch_cpu)
		rt_snprintf(arch_model, sizeof(arch_model), "%lu", (unsigned long)rt_arch_cpu);
	else
		rt_strlcpy(arch_model, "unknown", sizeof(arch_model));
	rt_snprintf(arch_routines, sizeof(arch_routines),
	            "csum=%s csum_copy=%s copy=%s copy_buf=%s",
	            csum_name(rt_arch.csum), csum_copy_name(rt_arch.csum_copy),
	            copy_name(rt_arch.copy), copy_name(rt_arch.copy_buf));
}

int rt_arch_describe(char *buf, u32 len)
{
	arch_update_text();
	return rt_snprintf(buf, len, "cpu: %s, checksum: %s, copy: %s, buffer copy: %s%s (routines %s)",
	                   rt_arch_cpu ? arch_model : "unknown",
	                   csum_name(rt_arch.csum), copy_name(rt_arch.copy),
	                   copy_name(rt_arch.copy_buf),
#ifdef RT_ARCH_M68K_ASM
	                   rt_arch.copy_buf == rt_copy_040 ? " (move16)" :
#endif
	                   "", arch_choice);
}

int rt_arch_select(const char *routines)
{
	struct rt_arch_ops o;
	int level;                   /* 0 = C, 20/40/60 = 680x0 routine set */
	char line[160];

	if (!routines || !strcmp(routines, "auto")) {
#ifdef RT_ARCH_M68K_ASM
		level = rt_arch_cpu >= 68060 ? 60 : rt_arch_cpu >= 68040 ? 40 : 20;
		if (rt_arch_cpu && rt_arch_cpu < 68020)
			level = 0;
#else
		level = 0;
#endif
		routines = "auto";
	} else if (!strcmp(routines, "generic")) {
		level = 0;
	} else if (!strcmp(routines, "020") || !strcmp(routines, "030")) {
		level = 20;
	} else if (!strcmp(routines, "040")) {
		level = 40;
	} else if (!strcmp(routines, "060")) {
		level = 60;
	} else {
		return -RT_EINVAL;
	}
#ifndef RT_ARCH_M68K_ASM
	if (level)
		return -RT_EOPNOTSUPP;
	o.csum = rt_csum_partial_c;
	o.csum_copy = rt_csum_copy_c;
	o.copy = o.copy_buf = rt_copy_c;
#else
	/* MOVE16 is an illegal instruction before the 68040 */
	if (level >= 40 && rt_arch_cpu && rt_arch_cpu < 68040)
		return -RT_EOPNOTSUPP;
	if (level && rt_arch_cpu && rt_arch_cpu < 68020)
		return -RT_EOPNOTSUPP;
	if (level == 0) {
		o.csum = rt_csum_partial_c;
		o.csum_copy = rt_csum_copy_c;
		o.copy = o.copy_buf = rt_copy_c;
	} else {
		/* One addx.l loop serves every 680x0: emulators give no 68060
		 * timing to justify a separate variant (docs/AMIGA.md). Plain
		 * move.l copies on the 68060 too; MOVE16 only between packet
		 * buffers. */
		o.csum = rt_csum_020;
		o.csum_copy = rt_csum_copy_020;
		o.copy = rt_copy_020m;
		o.copy_buf = (level >= 40 && arch_move16 && !arch_move16_vetoed) ? rt_copy_040 : rt_copy_020m;
	}
#endif
	rt_arch.csum = o.csum;
	rt_arch.csum_copy = o.csum_copy;
	rt_arch.copy = o.copy;
	rt_arch.copy_buf = o.copy_buf;
	rt_strlcpy(arch_choice, routines, sizeof(arch_choice));
	rt_arch_describe(line, sizeof(line));
	rt_log(RT_LOG_INFO, "%s", line);
	return 0;
}

void rt_arch_move16_veto(const char *why)
{
	if (arch_move16_vetoed)
		return;
	arch_move16_vetoed = 1;
	rt_log(RT_LOG_WARN, "cpu: move16 disabled: %s", why);
	rt_arch_select(arch_choice);
}

static int move16_handler(const struct rt_sysctl *e, void *old, u32 *oldlen,
                          const void *newp, u32 newlen)
{
	s32 v = arch_move16 && !arch_move16_vetoed;

	if (oldlen) {
		if (old) {
			if (*oldlen < sizeof(s32))
				return -RT_ENOMEM;
			memcpy(old, &v, sizeof(s32));
		}
		*oldlen = sizeof(s32);
	}
	if (newp) {
		if (newlen != sizeof(s32))
			return -RT_EINVAL;
		memcpy(&v, newp, sizeof(v));
		if (v < e->min || v > e->max)
			return -RT_EINVAL;
		if (v && arch_move16_vetoed)
			return -RT_EPERM;
		arch_move16 = v;
		rt_arch_select(arch_choice);
	}
	return 0;
}

static const struct rt_sysctl arch_sysctls[] = {
	{ "riptide.cpu.model", RT_SYSCTL_STRING, RT_SYSCTL_RO, 0, { 0 },
	  arch_model, 0, sizeof(arch_model), NULL, "CPU model (68020 ... 68060)" },
	{ "riptide.cpu.routines", RT_SYSCTL_STRING, RT_SYSCTL_RO, 0, { 0 },
	  arch_routines, 0, sizeof(arch_routines), NULL,
	  "checksum and copy routines in use (config: cpu routines auto|generic|020|040|060)" },
	{ "riptide.cpu.move16", RT_SYSCTL_INT, RT_SYSCTL_RW, 0, { 0 },
	  &arch_move16, 0, 1, move16_handler, "use MOVE16 for packet buffer copies (68040/68060)" },
};

void rt_arch_init(void)
{
	rt_sysctl_register(arch_sysctls, (int)RT_ARRAY_SIZE(arch_sysctls));
	rt_arch_select("auto");
}
