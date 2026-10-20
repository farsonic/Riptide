/*
 * Riptide - route: Linux net-tools style routing table tool.
 *
 *   route [-n] [-e] [-C]
 *   route add|del [-net|-host] TARGET|default [netmask M] [gw GW]
 *         [metric N] [reject] [[dev] IF]
 *
 * Adding and deleting use the BSD SIOCADDRT/SIOCDELRT ioctls (with
 * Riptide's RTF_MASK and rt_ifp extensions, see rt_bsd.h); a metric needs
 * the Riptide command port. The table comes from net.riptide.routes.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "cmdlib.h"
#include "gen/bsdsocket_calls.h"

static void usage(void)
{
	cl_puts("Usage: route [-nNvee] [-C]                 List kernel routing tables\n"
	        "       route [-v] add [-net|-host] target [netmask Nm] [gw Gw] [metric N] [reject] [[dev] If]\n"
	        "       route [-v] del [-net|-host] target [netmask Nm] [gw Gw] [[dev] If]\n"
	        "       target: A.B.C.D[/BITS] or default\n");
}

static void flags_str(const struct rt_ctl_route *r, char *b)
{
	u32 o = 0;

	if (r->flags & RT_RTF_UP) b[o++] = 'U';
	if (r->flags & RT_RTF_GATEWAY) b[o++] = 'G';
	if (r->flags & RT_RTF_HOST) b[o++] = 'H';
	if (r->flags & RT_RTF_DYNAMIC) b[o++] = 'D';
	if (r->flags & RT_RTF_PMTU) b[o++] = 'C';
	if (r->flags & RT_RTF_REJECT) b[o++] = '!';
	b[o] = 0;
}

static int show(int numeric, int ext, int cache)
{
	struct rt_ctl_hdr h;
	void *d = cl_dump(RT_CTL_ROUTES, &h);
	u32 k;
	char dst[64], gw[64], m[16], fl[8];

	if (!d)
		return CL_FAIL;
	cl_sort_routes(d, &h);
	cl_puts(cache ? "Kernel IP routing cache\n" : "Kernel IP routing table\n");
	if (ext)
		cl_puts("Destination     Gateway         Genmask         Flags   MSS Window  irtt Iface\n");
	else
		cl_puts("Destination     Gateway         Genmask         Flags Metric Ref    Use Iface\n");
	for (k = 0; k < h.count && !cl_break(); k++) {
		struct rt_ctl_route *r = cl_rec(d, &h, k);
		if (cache != ((r->flags & RT_RTF_PMTU) != 0))
			continue;
		if (!r->dst && !r->mask && !numeric)
			rt_strlcpy(dst, "default", sizeof(dst));
		else
			cl_ip(r->dst, dst);
		if (r->flags & RT_RTF_GATEWAY)
			cl_hostname(r->gateway, numeric, gw, sizeof(gw));
		else
			rt_strlcpy(gw, numeric ? "0.0.0.0" : "0.0.0.0", sizeof(gw));
		dst[15] = gw[15] = 0;
		flags_str(r, fl);
		if (ext)
			cl_printf("%-15s %-15s %-15s %-5s %5lu %-6u %4u %s\n", dst, gw, cl_ip(r->mask, m), fl,
			          (unsigned long)(r->mtu ? r->mtu - 40 : 0), 0U, 0U, r->ifname);
		else
			cl_printf("%-15s %-15s %-15s %-5s %-6lu %-6u %4lu %s\n", dst, gw, cl_ip(r->mask, m), fl,
			          (unsigned long)r->metric, 0U, (unsigned long)r->use, r->ifname);
	}
	FreeVec(d);
	return CL_OK;
}

static void sa_set(struct rt_sockaddr *sa, u32 a)
{
	struct rt_sockaddr_in sin;

	memset(&sin, 0, sizeof(sin));
	sin.sin_len = sizeof(sin);
	sin.sin_family = RT_AF_INET;
	sin.sin_addr = a;
	memcpy(sa, &sin, sizeof(sin));
}

static int modify(int add, int argc, char **argv, int i)
{
	struct rt_ortentry rt;
	struct rt_ctl_hdr h;
	u32 dst = 0, mask = 0, gw = 0, metric = 0, v;
	int host = 0, net = 0, bits = -1, have_mask = 0, reject = 0, have_metric = 0, e, fd;
	const char *dev = NULL, *target = NULL;
	char a[16], b[16];

	for (; i < argc; i++) {
		const char *k = argv[i], *n = i + 1 < argc ? argv[i + 1] : NULL;
		if (!strcmp(k, "-net")) net = 1;
		else if (!strcmp(k, "-host")) host = 1;
		else if (!strcmp(k, "netmask") && n) {
			if (cl_parse_ip(n, &mask) < 0) goto bad;
			have_mask = 1;
			i++;
		} else if ((!strcmp(k, "gw") || !strcmp(k, "gateway")) && n) {
			if (cl_resolve(n, &gw) < 0) return CL_ERROR;
			i++;
		} else if (!strcmp(k, "metric") && n) {
			if (cl_parse_uint(n, 0x7fffffff, &metric) < 0) goto bad;
			have_metric = metric != 0;
			i++;
		} else if ((!strcmp(k, "mss") || !strcmp(k, "window") || !strcmp(k, "irtt")) && n) {
			if (cl_parse_uint(n, 0x7fffffff, &v) < 0) goto bad;
			i++;
		} else if (!strcmp(k, "reject")) reject = 1;
		else if (!strcmp(k, "mod") || !strcmp(k, "dyn") || !strcmp(k, "reinstate")) ;
		else if (!strcmp(k, "dev") && n) { dev = n; i++; }
		else if (!target) target = k;
		else if (!dev) dev = k;
		else goto bad;
	}
	if (!target) {
		usage();
		return CL_ERROR;
	}
	if (!strcmp(target, "default")) {
		dst = 0;
		mask = 0;
		have_mask = 1;
	} else if (cl_parse_prefix(target, &dst, &bits) == 0) {
		if (bits >= 0) {
			mask = cl_mask(bits);
			have_mask = 1;
		}
	} else if (cl_resolve(target, &dst) < 0) {
		return CL_ERROR;
	}
	if (host) {
		mask = 0xffffffffUL;
		have_mask = 1;
	}
	if (net && !host && !have_mask && dst)
		mask = cl_classful(dst);
	if (have_metric) {
		/* ortentry has no metric: use the Riptide command port */
		int r = cl_command(cl_line("route %s %s/%d%s%s%s%s metric %lu%s", add ? "add" : "del",
		                           cl_ip(dst, a), have_mask ? rt_ctl_prefixlen(mask) :
		                           rt_ctl_prefixlen(cl_classful(dst)), gw ? " via " : "",
		                           gw ? cl_ip(gw, b) : "", dev ? " dev " : "", dev ? dev : "",
		                           (unsigned long)metric, reject ? " reject" : ""), 1);
		return r < 0 ? CL_FAIL : r ? CL_ERROR : CL_OK;
	}
	fd = cl_socket(RT_SOCK_DGRAM, 0);
	if (fd < 0)
		return CL_FAIL;
	memset(&rt, 0, sizeof(rt));
	sa_set(&rt.rt_dst, dst);
	sa_set(&rt.rt_gateway, gw);
	rt.rt_flags = RT_RTF_UP;
	if (gw)
		rt.rt_flags |= RT_RTF_GATEWAY;
	if (mask == 0xffffffffUL)
		rt.rt_flags |= RT_RTF_HOST;
	else if (have_mask) {
		rt.rt_flags |= RT_RTF_MASK;
		rt.rt_hash = mask;
	}
	if (reject)
		rt.rt_flags |= RT_RTF_REJECT;
	if (dev) {
		void *d = cl_dump(RT_CTL_IFACES, &h);
		struct rt_ctl_iface *f = d ? cl_find_iface(d, &h, dev) : NULL;
		if (f)
			rt.rt_ifp = f->index;
		if (d)
			FreeVec(d);
		if (!f) {
			cl_printf("%s: %s: No such device\n", add ? "SIOCADDRT" : "SIOCDELRT", dev);
			return CL_ERROR;
		}
	} else if (!gw && !reject && add) {
		cl_puts("SIOCADDRT: No such device (give a gateway or dev)\n");
		return CL_ERROR;
	}
	e = cl_ioctl(fd, add ? RT_SIOCADDRT : RT_SIOCDELRT, &rt);
	if (e) {
		cl_printf("%s: %s\n", add ? "SIOCADDRT" : "SIOCDELRT",
		          e == RT_ESRCH ? "No such process" : cl_strerror(e));
		return CL_ERROR;
	}
	return CL_OK;
bad:
	cl_printf("route: bad argument '%s'\n", argv[i]);
	return CL_ERROR;
}

static int route_main(int argc, char **argv)
{
	int i = 1, numeric = 0, ext = 0, cache = 0;

	for (; i < argc && argv[i][0] == '-' && strcmp(argv[i], "-net") && strcmp(argv[i], "-host"); i++) {
		const char *o = argv[i] + 1;
		if (!strcmp(argv[i], "--help")) {
			usage();
			return CL_OK;
		}
		for (; *o; o++) {
			if (*o == 'n' || *o == 'N') numeric = 1;
			else if (*o == 'e') ext++;
			else if (*o == 'C') cache = 1;
			else if (*o == 'v' || *o == 'F' || *o == '4') ;
			else if (*o == 'h') { usage(); return CL_OK; }
			else {
				usage();
				return CL_ERROR;
			}
		}
	}
	if (i >= argc)
		return show(numeric, ext, cache);
	if (!strcmp(argv[i], "add"))
		return modify(1, argc, argv, i + 1);
	if (!strcmp(argv[i], "del") || !strcmp(argv[i], "delete"))
		return modify(0, argc, argv, i + 1);
	usage();
	return CL_ERROR;
}

CL_MAIN(route_main, "route")
