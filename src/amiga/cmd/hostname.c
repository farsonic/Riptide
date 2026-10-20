/*
 * Riptide - hostname: show or set the system's host name.
 *
 *   hostname [-s] [-f] [-d] [-i] [-I] [NAME]
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "cmdlib.h"
#include "gen/bsdsocket_calls.h"

static int hostname_main(int argc, char **argv)
{
	char name[128], a[16];
	int i, mode = 0;
	struct rt_ctl_hdr h;
	void *d;
	u32 k;

	for (i = 1; i < argc && argv[i][0] == '-'; i++) {
		const char *o = argv[i];
		if (!strcmp(o, "-h") || !strcmp(o, "--help")) {
			cl_puts("Usage: hostname [-s|-f|-d|-i|-I] [name]\n");
			return CL_OK;
		}
		if (!strcmp(o, "-s") || !strcmp(o, "-f") || !strcmp(o, "-d") || !strcmp(o, "-i") ||
		    !strcmp(o, "-I") || !strcmp(o, "--short") || !strcmp(o, "--fqdn") ||
		    !strcmp(o, "--domain") || !strcmp(o, "--ip-address") || !strcmp(o, "--all-ip-addresses"))
			mode = o[1] == '-' ? (o[2] == 's' ? 's' : o[2] == 'f' ? 'f' : o[2] == 'd' ? 'd' :
			                      o[2] == 'i' ? 'i' : 'I') : o[1];
		else {
			cl_printf("hostname: invalid option '%s'\n", o);
			return CL_ERROR;
		}
	}
	if (i < argc) {
		int r = cl_command(cl_line("hostname \"%s\"", argv[i]), 1);
		return r < 0 ? CL_FAIL : r ? CL_ERROR : CL_OK;
	}
	if (cl_need_socket() < 0)
		return CL_FAIL;
	name[0] = 0;
	gethostname((STRPTR)name, sizeof(name));
	name[sizeof(name) - 1] = 0;
	switch (mode) {
	case 's': {
		char *dot = strchr(name, '.');
		if (dot)
			*dot = 0;
		cl_printf("%s\n", name);
		break;
	}
	case 'd':
	case 'f':
		d = cl_dump(RT_CTL_DNS, &h);
		if (d) {
			struct rt_ctl_dns *dn = cl_rec(d, &h, 0);
			if (mode == 'd')
				cl_printf("%s\n", dn->domain);
			else if (dn->domain[0] && !strchr(name, '.'))
				cl_printf("%s.%s\n", name, dn->domain);
			else
				cl_printf("%s\n", name);
			FreeVec(d);
		}
		break;
	case 'i':
	case 'I':
		d = cl_dump(RT_CTL_IFACES, &h);
		if (!d)
			return CL_FAIL;
		for (k = 0; k < h.count; k++) {
			struct rt_ctl_iface *f = cl_rec(d, &h, k);
			if (f->addr && (f->flags & RT_IFF_UP) && (mode == 'i' || f->type != RT_IFT_LOOP))
				cl_printf("%s ", cl_ip(f->addr, a));
		}
		cl_puts("\n");
		FreeVec(d);
		break;
	default:
		cl_printf("%s\n", name);
	}
	return CL_OK;
}

CL_MAIN(hostname_main, "hostname")
