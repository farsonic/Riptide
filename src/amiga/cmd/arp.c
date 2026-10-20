/*
 * Riptide - arp: Linux net-tools style ARP cache tool.
 *
 *   arp [-n] [-v] [-i IF] [-a] [HOST]      list (-a: BSD style lines)
 *   arp [-i IF] -s HOST MAC [temp] [pub]   add a permanent entry
 *   arp [-i IF] -d HOST                    delete an entry
 *
 * Changes use the SIOCSARP/SIOCDARP ioctls, the listing the
 * net.riptide.neigh table.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "cmdlib.h"
#include "gen/bsdsocket_calls.h"

static void usage(void)
{
	cl_puts("Usage:\n"
	        "  arp [-vn] [-i <if>] [-a] [<hostname>]       display (all) hosts\n"
	        "  arp [-v] [-i <if>] -d <host>                delete a specified entry\n"
	        "  arp [-v] [-i <if>] -s <host> <hwaddr> [temp] set a new ARP entry\n");
}

static int list(int bsd, int numeric, const char *ifn, const char *host)
{
	struct rt_ctl_hdr h;
	void *d;
	u32 k, want = 0, shown = 0;
	char name[64], mac[24], a[16];

	if (host && cl_resolve(host, &want) < 0)
		return CL_ERROR;
	d = cl_dump(RT_CTL_NEIGH, &h);
	if (!d)
		return CL_FAIL;
	if (!bsd)
		cl_puts("Address                  HWtype  HWaddress           Flags Mask            Iface\n");
	for (k = 0; k < h.count && !cl_break(); k++) {
		struct rt_ctl_neigh *n = cl_rec(d, &h, k);
		int complete = n->state != RT_NUD_INCOMPLETE;
		if ((ifn && strcmp(ifn, n->ifname)) || (host && n->addr != want))
			continue;
		shown++;
		cl_hostname(n->addr, numeric, name, sizeof(name));
		if (bsd) {
			if (!numeric && !strcmp(name, cl_ip(n->addr, a)))
				rt_strlcpy(name, "?", sizeof(name));
			cl_printf("%s (%s) at ", numeric ? "?" : name, cl_ip(n->addr, a));
			if (complete)
				cl_printf("%s [ether] ", cl_mac(n->hwaddr, 6, mac));
			else
				cl_puts("<incomplete> ");
			if (n->state == RT_NUD_PERMANENT)
				cl_puts("PERM ");
			cl_printf("on %s\n", n->ifname);
		} else if (complete) {
			cl_printf("%-24s %-7s %-19s %-5s %-15s %s\n", name, "ether", cl_mac(n->hwaddr, 6, mac),
			          n->state == RT_NUD_PERMANENT ? "CM" : "C", "", n->ifname);
		} else {
			cl_printf("%-24s %-7s %-19s %-5s %-15s %s\n", name, "", "(incomplete)", "", "", n->ifname);
		}
	}
	FreeVec(d);
	if (host && !shown)
		cl_printf("%s (%s) -- no entry\n", host, cl_ip(want, a));
	return CL_OK;
}

static int set_del(int set, int argc, char **argv, int i)
{
	struct rt_arpreq ar;
	struct rt_sockaddr_in sin;
	u32 ip;
	s32 fl = RT_ATF_PERM;
	int fd, e;

	if (i >= argc || (set && i + 1 >= argc)) {
		usage();
		return CL_ERROR;
	}
	if (cl_resolve(argv[i], &ip) < 0)
		return CL_ERROR;
	memset(&ar, 0, sizeof(ar));
	memset(&sin, 0, sizeof(sin));
	sin.sin_len = sizeof(sin);
	sin.sin_family = RT_AF_INET;
	sin.sin_addr = ip;
	memcpy(&ar.arp_pa, &sin, sizeof(sin));
	if (set) {
		if (cl_parse_mac(argv[i + 1], ar.arp_ha.sa_data) < 0) {
			cl_printf("arp: invalid hardware address '%s'\n", argv[i + 1]);
			return CL_ERROR;
		}
		ar.arp_ha.sa_len = 18;
		for (i += 2; i < argc; i++) {
			if (!strcmp(argv[i], "temp"))
				fl &= ~RT_ATF_PERM;
			else if (!strcmp(argv[i], "pub"))
				fl |= RT_ATF_PUBL;
		}
		fl |= RT_ATF_COM;
	}
	memcpy(ar.arp_flags, &fl, 4);
	fd = cl_socket(RT_SOCK_DGRAM, 0);
	if (fd < 0)
		return CL_FAIL;
	e = cl_ioctl(fd, set ? RT_SIOCSARP : RT_SIOCDARP, &ar);
	if (e) {
		if (!set && e == RT_ENXIO)
			cl_printf("No ARP entry for %s\n", argv[i]);
		else
			cl_printf("%s: %s\n", set ? "SIOCSARP" : "SIOCDARP", cl_strerror(e));
		return CL_ERROR;
	}
	return CL_OK;
}

static int arp_main(int argc, char **argv)
{
	int i = 1, numeric = 0, bsd = 0, mode = 0;
	const char *ifn = NULL;

	for (; i < argc && argv[i][0] == '-'; i++) {
		const char *o = argv[i] + 1;
		if (!strcmp(argv[i], "--help")) {
			usage();
			return CL_OK;
		}
		for (; *o; o++) {
			if (*o == 'n') numeric = 1;
			else if (*o == 'a') bsd = 1;
			else if (*o == 'e' || *o == 'v') ;
			else if (*o == 's') mode = 's';
			else if (*o == 'd') mode = 'd';
			else if (*o == 'i' && i + 1 < argc) { ifn = argv[++i]; break; }
			else if (*o == 'H' && i + 1 < argc) { i++; break; }
			else if (*o == 'h') { usage(); return CL_OK; }
			else { usage(); return CL_ERROR; }
		}
		if (mode)
			break;
	}
	if (mode)
		return set_del(mode == 's', argc, argv, i + 1);
	return list(bsd, numeric, ifn, i < argc ? argv[i] : NULL);
}

CL_MAIN(arp_main, "arp")
