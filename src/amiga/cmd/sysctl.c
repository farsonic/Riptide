/*
 * Riptide - sysctl: read and set Riptide's tunables (Linux sysctl style).
 *
 *   sysctl -a                 all variables
 *   sysctl [-n] NAME...       one variable, or every one below a prefix
 *   sysctl [-w] NAME=VALUE    set
 *
 * Works through the RIPTIDE port ("sysctl ..." command).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "cmdlib.h"

static void usage(void)
{
	cl_puts("Usage:\n"
	        " sysctl [options] [variable[=value] ...]\n"
	        "Options:\n"
	        "  -a, -A, --all        display all variables\n"
	        "  -n, --values         print value only\n"
	        "  -w, --write          enable writing a value to a variable\n"
	        "  -q, --quiet          do not echo variable set\n"
	        "  -h, --help           display this help and exit\n");
}

static int sysctl_main(int argc, char **argv)
{
	char line[RIPTIDE_TEXTLEN];
	u32 o;
	int i, quiet = 0, r, any = 0;

	o = (u32)rt_snprintf(line, sizeof(line), "sysctl");
	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
			usage();
			return CL_OK;
		}
		if (!strcmp(a, "--all"))
			a = "-a";
		else if (!strcmp(a, "--values"))
			a = "-n";
		else if (!strcmp(a, "--write"))
			a = "-w";
		else if (!strcmp(a, "--quiet"))
			a = "-q";
		if (a[0] == '-') {
			const char *f;
			for (f = a + 1; *f; f++) {
				if (*f == 'q')
					quiet = 1;
				else if (!strchr("aAXnwe", *f)) {
					cl_printf("sysctl: invalid option -- '%c'\n", *f);
					usage();
					return CL_ERROR;
				}
			}
			if (!strchr(a, 'q') || a[2])
				o += (u32)rt_snprintf(line + o, sizeof(line) - o, " %s", a);
			if (strchr(a, 'a') || strchr(a, 'A') || strchr(a, 'X'))
				any = 1;
			continue;
		}
		o += (u32)rt_snprintf(line + o, sizeof(line) - o, " \"%s\"", a);
		any = 1;
		if (o >= sizeof(line) - 1) {
			cl_puts("sysctl: command line too long\n");
			return CL_ERROR;
		}
	}
	if (!any) {
		usage();
		return CL_ERROR;
	}
	r = cl_command(line, 0);
	if (r < 0)
		return CL_FAIL;
	if (!quiet || r)
		cl_puts(cl_reply());
	return r ? CL_ERROR : CL_OK;
}

CL_MAIN(sysctl_main, "sysctl")
