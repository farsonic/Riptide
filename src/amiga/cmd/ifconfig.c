/*
 * Riptide - ifconfig: Linux net-tools style interface listing and
 * configuration.
 *
 *   ifconfig [-a] [-s] [IF]
 *   ifconfig IF [A.B.C.D[/BITS]] [netmask M] [broadcast B] [pointopoint P]
 *               [mtu N] [metric N] [[-]promisc] [[-]arp] [[-]allmulti]
 *               [del A.B.C.D] [up|down]
 *   ifconfig IF inet6 { add | del } IPV6[/PLEN]
 *
 * Changes use the standard BSD interface ioctls (SIOCAIFADDR, SIOCSIF*),
 * IPv6 addresses the configuration command channel; the listing comes from
 * the net.riptide.ifaces and addrs6 tables.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "cmdlib.h"
#include "gen/bsdsocket_calls.h"

static void usage(void)
{
	cl_puts("Usage:\n"
	        "  ifconfig [-a] [-v] [-s] <interface>\n"
	        "  ifconfig <interface> [<address>[/<bits>]] [netmask <address>] [broadcast <address>]\n"
	        "           [pointopoint <address>] [mtu <NN>] [metric <NN>] [[-]promisc] [[-]arp]\n"
	        "           [[-]allmulti] [del <address>] [up|down]\n"
	        "  ifconfig <interface> alias <address>[/<bits>] [netmask <address>] [broadcast <address>]\n"
	        "  ifconfig <interface>:<n> <address>[/<bits>] ...  (the same: a further address)\n"
	        "  ifconfig <interface> -alias <address>\n"
	        "  ifconfig <interface> inet6 {add|del} <address>[/<prefixlen>]\n"
	        "  ifconfig <interface> create vlan <id> vlandev <parent> [vlanpcp <0-7>]\n"
	        "  ifconfig <interface> destroy\n");
}

/* Linux numbering of the flag word shown by net-tools */
static u32 linux_flags(u16 f)
{
	u32 l = f & 0x03ff;                    /* UP..ALLMULTI share values */

	if (f & RT_IFF_MULTICAST)
		l |= 0x1000;
	return l;
}

static void flag_names(u16 f, char *b, u32 len)
{
	static const struct { u16 bit; const char *name; } fl[] = {
		{ RT_IFF_UP, "UP" }, { RT_IFF_BROADCAST, "BROADCAST" }, { RT_IFF_DEBUG, "DEBUG" },
		{ RT_IFF_LOOPBACK, "LOOPBACK" }, { RT_IFF_POINTOPOINT, "POINTOPOINT" },
		{ RT_IFF_NOTRAILERS, "NOTRAILERS" }, { RT_IFF_RUNNING, "RUNNING" }, { RT_IFF_NOARP, "NOARP" },
		{ RT_IFF_PROMISC, "PROMISC" }, { RT_IFF_ALLMULTI, "ALLMULTI" }, { RT_IFF_MULTICAST, "MULTICAST" },
	};
	u32 o = 0, i;

	b[0] = 0;
	for (i = 0; i < RT_ARRAY_SIZE(fl); i++)
		if (f & fl[i].bit)
			o += (u32)rt_snprintf(b + o, len - o, "%s%s", o ? "," : "", fl[i].name);
}

/* " (554.5 KiB)" like net-tools */
static void human(u32 v, char *b)
{
	static const char *const unit[] = { "B", "KiB", "MiB", "GiB" };
	u32 i = 0, frac = 0;

	while (v >= 1024 && i < 3) {
		frac = (v % 1024) * 10 / 1024;
		v /= 1024;
		i++;
	}
	if (i)
		rt_snprintf(b, 24, "%lu.%lu %s", (unsigned long)v, (unsigned long)frac, unit[i]);
	else
		rt_snprintf(b, 24, "%lu.0 B", (unsigned long)v);
}

static void print_inet6(const struct rt_ctl_iface *f, void *d6, const struct rt_ctl_hdr *h6)
{
	char a[RT_INET6_ADDRSTRLEN];
	u32 k;

	for (k = 0; d6 && k < h6->count; k++) {
		struct rt_ctl_addr6 *x = cl_rec(d6, h6, k);
		rt_in6_addr ad;
		if (x->ifindex != f->index)
			continue;
		memcpy(ad.b, x->addr, 16);
		cl_printf("        inet6 %s  prefixlen %u  scopeid %s", cl_ip6(x->addr, a), (unsigned)x->plen,
		          rt_in6_is_loopback(&ad) ? "0x10<host>" : x->scope == 2 ? "0x20<link>" :
		          x->scope == 5 ? "0x40<site>" : "0x0<global>");
		if (x->flags & RT_CTL_IN6_TENTATIVE)
			cl_puts("  tentative");
		if (x->flags & RT_CTL_IN6_DUPLICATED)
			cl_puts("  duplicated");
		if (x->flags & RT_CTL_IN6_TEMPORARY)
			cl_puts("  temporary");
		cl_puts("\n");
	}
}

static int verbose;

static const struct rt_ctl_driver *find_driver(const struct rt_ctl_iface *f, void *dd, const struct rt_ctl_hdr *dh)
{
	u32 k;

	for (k = 0; dd && k < dh->count; k++) {
		const struct rt_ctl_driver *r = cl_rec(dd, dh, k);
		if (r->index == f->index)
			return r;
	}
	return NULL;
}

/* ifconfig -v: the driver counters in one or two lines */
static void print_driver(const struct rt_ctl_driver *r)
{
	u32 q, wakeups = 0;

	for (q = 0; q < RT_CTL_DRV_BATCH; q++)
		wakeups += r->batch[q];
	if (!(r->flags & RT_CTL_DRVF_NOQ))
		cl_printf("        RX reads %lu/%lu  lowest %lu  ran out %lu  nobuf %lu\n",
		          (unsigned long)r->rxq[0].queued, (unsigned long)r->rxq[0].cfg,
		          (unsigned long)r->rxq[0].low, (unsigned long)r->rxq[0].empty, (unsigned long)r->rx_nobuf);
	cl_printf("        RX wakeups %lu  most frames per wakeup %lu  TX in flight %lu (most %lu)  queue full %lu\n",
	          (unsigned long)wakeups, (unsigned long)r->batch_max, (unsigned long)r->tx_inflight,
	          (unsigned long)r->tx_inflight_max, (unsigned long)r->tx_full);
}

/* the further IPv4 addresses (net.riptide.addrs4, Riptide 0.15) */
static void print_inet4_extra(const struct rt_ctl_iface *f, void *d4, const struct rt_ctl_hdr *h4)
{
	char a[16], b[16], c[16];
	u32 k;

	for (k = 0; d4 && k < h4->count; k++) {
		const struct rt_ctl_addr4 *x = cl_rec(d4, h4, k);
		if (x->ifindex != f->index || (x->flags & RT_CTL_A4_PRIMARY))
			continue;
		cl_printf("        inet %s  netmask %s", cl_ip(x->addr, a), cl_ip(x->netmask, b));
		if (x->broadaddr)
			cl_printf("  broadcast %s", cl_ip(x->broadaddr, c));
		cl_puts("  (alias)\n");
	}
}

static void print_if(const struct rt_ctl_iface *f, void *d6, const struct rt_ctl_hdr *h6,
                     void *d4, const struct rt_ctl_hdr *h4, const struct rt_ctl_driver *dr,
                     const struct rt_ctl_iface *par)
{
	char fl[128], a[16], b[16], c[16], h[24];
	u32 overruns = 0;

	flag_names(f->flags, fl, sizeof(fl));
	cl_printf("%s: flags=%lu<%s>  mtu %u\n", f->name, (unsigned long)linux_flags(f->flags), fl,
	          (unsigned)f->mtu);
	if (f->addr) {
		cl_printf("        inet %s  netmask %s", cl_ip(f->addr, a), cl_ip(f->netmask, b));
		if (f->type == RT_IFT_PTP && f->dstaddr)
			cl_printf("  destination %s", cl_ip(f->dstaddr, c));
		else if (f->broadaddr)
			cl_printf("  broadcast %s", cl_ip(f->broadaddr, c));
		cl_puts("\n");
	}
	print_inet4_extra(f, d4, h4);
	print_inet6(f, d6, h6);
	if (f->type == RT_IFT_ETHER)
		cl_printf("        ether %s  txqueuelen 1000  (Ethernet)\n", cl_mac(f->hwaddr, 6, fl));
	else if (f->type == RT_IFT_LOOP)
		cl_puts("        loop  txqueuelen 1000  (Local Loopback)\n");
	else
		cl_puts("        ppp  txqueuelen 3  (Point-to-Point Protocol)\n");
	human(f->ibytes, h);
	cl_printf("        RX packets %lu  bytes %lu (%s)\n", (unsigned long)f->ipackets,
	          (unsigned long)f->ibytes, h);
	if (dr)
		overruns = (dr->flags & RT_CTL_DRVF_DROPS) ? dr->drv_drops :
		           (dr->flags & RT_CTL_DRVF_GS) ? dr->gs_overruns : 0;
	cl_printf("        RX errors %lu  dropped %lu  overruns %lu  frame 0\n", (unsigned long)f->ierrors,
	          (unsigned long)f->iqdrops, (unsigned long)overruns);
	human(f->obytes, h);
	cl_printf("        TX packets %lu  bytes %lu (%s)\n", (unsigned long)f->opackets,
	          (unsigned long)f->obytes, h);
	cl_printf("        TX errors %lu  dropped 0 overruns 0  carrier 0  collisions %lu\n",
	          (unsigned long)f->oerrors, (unsigned long)f->collisions);
	if (par) {
		/* as FreeBSD's ifconfig */
		cl_printf("        vlan: %u", (unsigned)f->vlan_id);
		if (f->vlan_pcp == RT_CTL_VLAN_PCP_PRIO)
			cl_puts(" vlanpcp: priority");
		else
			cl_printf(" vlanpcp: %u", (unsigned)f->vlan_pcp);
		cl_printf(" parent interface: %s\n", par->name);
	} else if (f->descr[0] && f->type != RT_IFT_LOOP) {
		cl_printf("        device %s\n", f->descr);
	}
	if (verbose && dr)
		print_driver(dr);
	cl_puts("\n");
}

static void print_short(const struct rt_ctl_iface *f)
{
	char fl[8];
	u32 o = 0;

	if (f->flags & RT_IFF_BROADCAST) fl[o++] = 'B';
	if (f->flags & RT_IFF_LOOPBACK) fl[o++] = 'L';
	if (f->flags & RT_IFF_MULTICAST) fl[o++] = 'M';
	if (f->flags & RT_IFF_NOARP) fl[o++] = 'O';
	if (f->flags & RT_IFF_POINTOPOINT) fl[o++] = 'P';
	if (f->flags & RT_IFF_RUNNING) fl[o++] = 'R';
	if (f->flags & RT_IFF_UP) fl[o++] = 'U';
	fl[o] = 0;
	cl_printf("%-15s %5u %8lu %6lu %6lu %6lu %8lu %6lu %6lu %6lu %s\n", f->name, (unsigned)f->mtu,
	          (unsigned long)f->ipackets, (unsigned long)f->ierrors, (unsigned long)f->iqdrops, 0UL,
	          (unsigned long)f->opackets, (unsigned long)f->oerrors, 0UL, 0UL, fl);
}

static int list(int all, int shortfmt, const char *only)
{
	struct rt_ctl_hdr h, h6, dh, h4;
	void *d = cl_dump(RT_CTL_IFACES, &h), *d6, *dd, *d4;
	u32 k;

	if (!d)
		return CL_FAIL;
	d6 = shortfmt ? NULL : cl_dump(RT_CTL_ADDRS6, &h6);
	dd = shortfmt ? NULL : cl_dump(RT_CTL_DRIVERS, &dh);
	d4 = shortfmt ? NULL : cl_dump_quiet(RT_CTL_ADDRS4, &h4);
	if (only && !cl_find_iface(d, &h, only)) {
		cl_printf("%s: error fetching interface information: Device not found\n", only);
		FreeVec(d);
		return CL_ERROR;
	}
	if (shortfmt)
		cl_puts("Iface      MTU    RX-OK RX-ERR RX-DRP RX-OVR    TX-OK TX-ERR TX-DRP TX-OVR Flg\n");
	for (k = 0; k < h.count; k++) {
		struct rt_ctl_iface *f = cl_rec(d, &h, k);
		if (only ? strcmp(f->name, only) != 0 : (!all && !(f->flags & RT_IFF_UP)))
			continue;
		const struct rt_ctl_iface *par = NULL;
		/* VLAN fields: Riptide 0.16 records */
		if (h.recsize >= offsetof(struct rt_ctl_iface, nvlan) + 1 && f->vlan_parent)
			par = cl_iface_by_index(d, &h, f->vlan_parent);
		if (shortfmt)
			print_short(f);
		else
			print_if(f, d6, &h6, d4, &h4, find_driver(f, dd, &dh), par);
	}
	FreeVec(d);
	if (d4)
		FreeVec(d4);
	if (d6)
		FreeVec(d6);
	if (dd)
		FreeVec(dd);
	return CL_OK;
}

static void ifr_name(struct rt_ifreq *r, const char *name)
{
	memset(r, 0, sizeof(*r));
	rt_strlcpy(r->ifr_name, name, sizeof(r->ifr_name));
}

static void ifr_addr(struct rt_ifreq *r, u32 a)
{
	struct rt_sockaddr_in sin;

	memset(&sin, 0, sizeof(sin));
	sin.sin_len = sizeof(sin);
	sin.sin_family = RT_AF_INET;
	sin.sin_addr = a;
	memcpy(&r->ifr_ifru.ifru_addr, &sin, sizeof(sin));
}

static int fail(const char *what, int e)
{
	cl_printf("%s: %s\n", what, cl_strerror(e));
	return CL_ERROR;
}

static int configure(int fd, const char *ifn, int argc, char **argv, int i, int alias)
{
	struct rt_ifreq r;
	u32 addr = 0, mask = 0, brd = 0, peer = 0, del = 0, v;
	int have_addr = 0, bits = -1, up = -1, promisc = -1, arp = -1, allmulti = -1, e;
	long mtu = -1, metric = -1;
	s16 fl;

	for (; i < argc; i++) {
		const char *k = argv[i], *n = i + 1 < argc ? argv[i + 1] : NULL;
		if (!strcmp(k, "up")) up = 1;
		else if (!strcmp(k, "down")) up = 0;
		else if (!strcmp(k, "promisc") || !strcmp(k, "-promisc")) promisc = k[0] != '-';
		else if (!strcmp(k, "arp") || !strcmp(k, "-arp")) arp = k[0] != '-';
		else if (!strcmp(k, "allmulti") || !strcmp(k, "-allmulti")) allmulti = k[0] != '-';
		else if (!strcmp(k, "inet")) continue;
		else if (!strcmp(k, "alias") || !strcmp(k, "add")) alias = 1;
		else if ((!strcmp(k, "netmask")) && n) {
			if (cl_parse_ip(n, &mask) < 0) goto bad;
			i++;
		} else if ((!strcmp(k, "broadcast") || !strcmp(k, "-broadcast")) && n) {
			if (cl_parse_ip(n, &brd) < 0) goto bad;
			i++;
		} else if ((!strcmp(k, "pointopoint") || !strcmp(k, "dstaddr")) && n) {
			if (cl_parse_ip(n, &peer) < 0) goto bad;
			i++;
		} else if (!strcmp(k, "mtu") && n) {
			if (cl_parse_uint(n, 65535, &v) < 0) goto bad;
			mtu = (long)v;
			i++;
		} else if (!strcmp(k, "metric") && n) {
			if (cl_parse_uint(n, 0x7fffffff, &v) < 0) goto bad;
			metric = (long)v;
			i++;
		} else if ((!strcmp(k, "del") || !strcmp(k, "-alias")) && n) {
			if (cl_parse_prefix(n, &del, &bits) < 0) goto bad;
			i++;
		} else if (!have_addr && cl_parse_prefix(k, &addr, &bits) == 0) {
			have_addr = 1;
			if (bits >= 0)
				mask = cl_mask(bits);
		} else if (!have_addr && cl_need_socket() == 0 && cl_resolve(k, &addr) == 0) {
			have_addr = 1;
		} else {
			cl_printf("%s: unknown argument '%s'\n", cl_prog, k);
			usage();
			return CL_ERROR;
		}
	}
	if (del) {
		ifr_name(&r, ifn);
		ifr_addr(&r, del);
		if ((e = cl_ioctl(fd, RT_SIOCDIFADDR, &r)) != 0)
			return fail("SIOCDIFADDR", e);
	}
	if (have_addr && !alias) {
		/* the (primary) address is replaced, as net-tools does */
		ifr_name(&r, ifn);
		ifr_addr(&r, addr);
		if ((e = cl_ioctl(fd, RT_SIOCSIFADDR, &r)) != 0)
			return fail("SIOCSIFADDR", e);
		ifr_name(&r, ifn);
		/* a new address without a netmask gets the classful one (Linux) */
		ifr_addr(&r, mask ? mask : cl_classful(addr));
		if ((e = cl_ioctl(fd, RT_SIOCSIFNETMASK, &r)) != 0)
			return fail("SIOCSIFNETMASK", e);
		if (brd || peer) {
			ifr_name(&r, ifn);
			ifr_addr(&r, peer ? peer : brd);
			if ((e = cl_ioctl(fd, peer ? RT_SIOCSIFDSTADDR : RT_SIOCSIFBRDADDR, &r)) != 0)
				return fail(peer ? "SIOCSIFDSTADDR" : "SIOCSIFBRDADDR", e);
		}
	} else if (have_addr) {
		/* a further address (BSD SIOCAIFADDR) */
		struct rt_ifaliasreq ra;
		struct rt_sockaddr_in sin;

		memset(&ra, 0, sizeof(ra));
		rt_strlcpy(ra.ifra_name, ifn, sizeof(ra.ifra_name));
		memset(&sin, 0, sizeof(sin));
		sin.sin_len = sizeof(sin);
		sin.sin_family = RT_AF_INET;
		sin.sin_addr = addr;
		memcpy(&ra.ifra_addr, &sin, sizeof(sin));
		/* a new address without a netmask gets the classful one (Linux) */
		sin.sin_addr = mask ? mask : (addr ? cl_classful(addr) : 0);
		memcpy(&ra.ifra_mask, &sin, sizeof(sin));
		if (brd || peer) {
			sin.sin_addr = peer ? peer : brd;
			memcpy(&ra.ifra_broadaddr, &sin, sizeof(sin));
		}
		if ((e = cl_ioctl(fd, RT_SIOCAIFADDR, &ra)) != 0)
			return fail("SIOCAIFADDR", e);
	} else {
		if (mask) {
			ifr_name(&r, ifn);
			ifr_addr(&r, mask);
			if ((e = cl_ioctl(fd, RT_SIOCSIFNETMASK, &r)) != 0)
				return fail("SIOCSIFNETMASK", e);
		}
		if (brd) {
			ifr_name(&r, ifn);
			ifr_addr(&r, brd);
			if ((e = cl_ioctl(fd, RT_SIOCSIFBRDADDR, &r)) != 0)
				return fail("SIOCSIFBRDADDR", e);
		}
		if (peer) {
			ifr_name(&r, ifn);
			ifr_addr(&r, peer);
			if ((e = cl_ioctl(fd, RT_SIOCSIFDSTADDR, &r)) != 0)
				return fail("SIOCSIFDSTADDR", e);
		}
	}
	if (mtu >= 0) {
		s32 m = (s32)mtu;
		ifr_name(&r, ifn);
		memcpy(&r.ifr_ifru.ifru_mtu, &m, 4);
		if ((e = cl_ioctl(fd, RT_SIOCSIFMTU, &r)) != 0)
			return fail("SIOCSIFMTU", e);
	}
	if (metric >= 0) {
		s32 m = (s32)metric;
		ifr_name(&r, ifn);
		memcpy(&r.ifr_ifru.ifru_metric, &m, 4);
		if ((e = cl_ioctl(fd, RT_SIOCSIFMETRIC, &r)) != 0)
			return fail("SIOCSIFMETRIC", e);
	}
	if (up >= 0 || promisc >= 0 || arp >= 0 || allmulti >= 0) {
		ifr_name(&r, ifn);
		if ((e = cl_ioctl(fd, RT_SIOCGIFFLAGS, &r)) != 0)
			return fail("SIOCGIFFLAGS", e);
		memcpy(&fl, &r.ifr_ifru.ifru_flags, 2);
#define SETF(c, bit) do { if ((c) == 1) fl |= (bit); else if ((c) == 0) fl &= ~(bit); } while (0)
		SETF(up, RT_IFF_UP);
		SETF(promisc, RT_IFF_PROMISC);
		SETF(allmulti, RT_IFF_ALLMULTI);
		SETF(arp < 0 ? -1 : !arp, RT_IFF_NOARP);
#undef SETF
		memcpy(&r.ifr_ifru.ifru_flags, &fl, 2);
		if ((e = cl_ioctl(fd, RT_SIOCSIFFLAGS, &r)) != 0)
			return fail("SIOCSIFFLAGS", e);
	}
	return CL_OK;
bad:
	cl_printf("%s: bad value for '%s'\n", cl_prog, argv[i]);
	return CL_ERROR;
}

/* ifconfig IF inet6 add|del ADDR[/PLEN] (net-tools syntax) */
static int inet6_cmd(const char *ifn, int argc, char **argv, int i)
{
	rt_in6_addr a;
	int plen, r;
	char s[RT_INET6_ADDRSTRLEN];

	if (i + 1 >= argc || (strcmp(argv[i], "add") && strcmp(argv[i], "del")) ||
	    rt_inet6_parse_prefix(argv[i + 1], &a, &plen, NULL, 0) < 0) {
		usage();
		return CL_ERROR;
	}
	rt_inet6_ntop(&a, s, sizeof(s));
	if (!strcmp(argv[i], "del"))
		r = cl_command(cl_line("ifconfig %s inet6 %s delete", ifn, s), 1);
	else
		r = cl_command(cl_line("ifconfig %s inet6 %s/%d", ifn, s, plen < 0 ? 128 : plen), 1);
	return r < 0 ? CL_FAIL : r ? CL_ERROR : CL_OK;
}

static int ifconfig_main(int argc, char **argv)
{
	int i = 1, all = 0, shortfmt = 0, fd, alias = 0;
	struct rt_ifreq r;
	char ifn[RT_IFNAMSIZ + 8], *colon;

	for (; i < argc && argv[i][0] == '-'; i++) {
		if (!strcmp(argv[i], "-a"))
			all = 1;
		else if (!strcmp(argv[i], "-s"))
			shortfmt = 1;
		else if (!strcmp(argv[i], "-v"))
			verbose = 1;
		else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help") || !strcmp(argv[i], "-?")) {
			usage();
			return CL_OK;
		} else {
			usage();
			return CL_ERROR;
		}
	}
	if (i >= argc)
		return list(all, shortfmt, NULL);
	/* "eth0:1" (Linux alias label): a further address of eth0 */
	rt_strlcpy(ifn, argv[i], sizeof(ifn));
	colon = strchr(ifn, ':');
	if (colon) {
		*colon = 0;
		alias = 1;
	}
	if (i + 1 >= argc)
		return list(1, shortfmt, ifn);
	/* FreeBSD: ifconfig eth0.10 create vlan 10 vlandev eth0 / ifconfig eth0.10 destroy */
	if (!strcmp(argv[i + 1], "destroy"))
		return cl_command(cl_line("interface %s delete", ifn), 1) ? CL_ERROR : CL_OK;
	if (!strcmp(argv[i + 1], "create")) {
		const char *vid = NULL, *dev = NULL, *pcp = NULL;
		int k;
		for (k = i + 2; k + 1 < argc; k += 2) {
			if (!strcmp(argv[k], "vlan"))
				vid = argv[k + 1];
			else if (!strcmp(argv[k], "vlandev"))
				dev = argv[k + 1];
			else if (!strcmp(argv[k], "vlanpcp"))
				pcp = argv[k + 1];
			else
				break;
		}
		if (k != argc || !vid || !dev) {
			usage();
			return CL_ERROR;
		}
		return cl_command(pcp ? cl_line("interface %s vlan=%s parent=%s pcp=%s", ifn, vid, dev, pcp) :
		                  cl_line("interface %s vlan=%s parent=%s", ifn, vid, dev), 1) ? CL_ERROR : CL_OK;
	}
	fd = cl_socket(RT_SOCK_DGRAM, 0);
	if (fd < 0)
		return CL_FAIL;
	ifr_name(&r, ifn);
	if (cl_ioctl(fd, RT_SIOCGIFFLAGS, &r) != 0) {
		cl_printf("%s: ERROR while getting interface flags: No such device\n", ifn);
		return CL_ERROR;
	}
	if (i + 1 < argc && !strcmp(argv[i + 1], "inet6"))
		return inet6_cmd(ifn, argc, argv, i + 2);
	return configure(fd, ifn, argc, argv, i + 1, alias);
}

CL_MAIN(ifconfig_main, "ifconfig")
