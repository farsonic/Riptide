/*
 * Riptide - ip: show and change interfaces, addresses, routes and the
 * neighbour table with iproute2 syntax and output.
 *
 *   ip [-s] [-d] [-4 | -6] [-br] OBJECT [COMMAND [ARGS]]
 *   OBJECT := link | address | route | neigh | dhcp | dns   (abbreviations work)
 *
 * Data comes from the net.riptide.* tables (RiptideSysCtl), changes go to
 * the running stack as configuration commands through the RIPTIDE port.
 * "ip dhcp" and "ip dns" are Riptide extensions.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "cmdlib.h"
#include "gen/bsdsocket_calls.h"

static int opt_stats, opt_brief, opt_details;
static int opt_family;           /* 0 both, 4, 6 (-4 / -6 / -f inet|inet6) */

static void usage(void)
{
	cl_puts("Usage: ip [ OPTIONS ] OBJECT { COMMAND | help }\n"
	        "where  OBJECT := { link | address | route | neigh | dhcp | dns }\n"
	        "       OPTIONS := { -s[tatistics] | -d[etails] | -4 | -6 | -br[ief] | -V[ersion] }\n"
	        "\n"
	        "  ip link [show [dev] IF]\n"
	        "  ip link set [dev] IF { up | down | mtu N | promisc on|off | arp on|off | reads N |\n"
	        "                         tcpwindow N }\n"
	        "  ip link add NAME type sana2 device DEV [unit N] [mtu N] [reads N]\n"
	        "  ip link add NAME type mni driver FILE [unit N] [options TEXT]\n"
	        "  ip link del [dev] NAME\n"
	        "  ip link resetstats [[dev] IF]   (all interfaces without IF)\n"
	        "  ip addr [show [dev] IF]\n"
	        "  ip addr { add | replace } A.B.C.D/BITS [peer P] [broadcast B] dev IF\n"
	        "  ip addr del A.B.C.D[/BITS] dev IF | ip addr flush dev IF\n"
	        "  ip -6 addr { add | del } IPV6/PLEN dev IF [nodad]\n"
	        "  ip route [show | list] [cache]\n"
	        "  ip route { add | del | replace | change | append } PREFIX|default\n"
	        "           [via GW] [dev IF] [metric N] [mtu N]\n"
	        "  ip route get ADDR | ip route flush { dev IF | cache | all }\n"
	        "  ip -6 route [show] | ip -6 route { add | del } PREFIX|default [via GW] [dev IF]\n"
	        "  ip neigh [show [dev IF]]\n"
	        "  ip neigh { add | replace } ADDR lladdr MAC [dev IF] [nud permanent]\n"
	        "  ip neigh del ADDR [dev IF] | ip neigh flush [dev IF]\n"
	        "Riptide extensions:\n"
	        "  ip dhcp [show] [dev IF] | ip dhcp { start | stop | renew } [dev] IF\n"
	        "  ip dns [show] | ip dns { add | del } ADDR | ip dns search DOMAIN\n");
}

/* ------------------------------------------------------------ helpers */

static const char *link_state(const struct rt_ctl_iface *f)
{
	if (f->type == RT_IFT_LOOP || f->type == RT_IFT_PTP)
		return (f->flags & RT_IFF_UP) ? "UNKNOWN" : "DOWN";
	if (!(f->flags & RT_IFF_UP))
		return "DOWN";
	return (f->flags & RT_IFF_RUNNING) ? "UP" : "DOWN";
}

static void link_flags(const struct rt_ctl_iface *f, char *b, u32 len)
{
	static const struct { u16 bit; const char *name; } fl[] = {
		{ RT_IFF_LOOPBACK, "LOOPBACK" }, { RT_IFF_BROADCAST, "BROADCAST" },
		{ RT_IFF_POINTOPOINT, "POINTOPOINT" }, { RT_IFF_MULTICAST, "MULTICAST" },
		{ RT_IFF_NOARP, "NOARP" }, { RT_IFF_ALLMULTI, "ALLMULTI" }, { RT_IFF_PROMISC, "PROMISC" },
		{ RT_IFF_DEBUG, "DEBUG" }, { RT_IFF_UP, "UP" },
	};
	u32 o = 0, i;

	b[o++] = '<';
	b[o] = 0;
	if ((f->flags & RT_IFF_UP) && !(f->flags & RT_IFF_RUNNING))
		o += (u32)rt_snprintf(b + o, len - o, "NO-CARRIER,");
	for (i = 0; i < RT_ARRAY_SIZE(fl); i++)
		if (f->flags & fl[i].bit)
			o += (u32)rt_snprintf(b + o, len - o, "%s,", fl[i].name);
	if ((f->flags & RT_IFF_UP) && (f->flags & RT_IFF_RUNNING))
		o += (u32)rt_snprintf(b + o, len - o, "LOWER_UP,");
	if (o > 1)
		o--;
	rt_snprintf(b + o, len - o, ">");
}

static void print_link_line(const struct rt_ctl_iface *f)
{
	char fl[128], mac[64];

	link_flags(f, fl, sizeof(fl));
	cl_printf("%lu: %s: %s mtu %u ", (unsigned long)f->index, f->name, fl, (unsigned)f->mtu);
	if (f->tcp_window)
		cl_printf("tcpwindow %lu ", (unsigned long)f->tcp_window);
	cl_printf("state %s mode DEFAULT\n", link_state(f));
	if (f->type == RT_IFT_LOOP)
		cl_puts("    link/loopback 00:00:00:00:00:00 brd 00:00:00:00:00:00\n");
	else if (f->type == RT_IFT_ETHER)
		cl_printf("    link/ether %s brd ff:ff:ff:ff:ff:ff\n", cl_mac(f->hwaddr, 6, mac));
	else
		cl_puts("    link/none \n");
	if (opt_details && f->descr[0])
		cl_printf("    driver %s\n", f->descr);
	if (opt_stats) {
		cl_puts("    RX:  bytes  packets errors dropped  missed   mcast\n");
		cl_printf("    %10lu %8lu %6lu %7lu %7lu %7lu\n", (unsigned long)f->ibytes,
		          (unsigned long)f->ipackets, (unsigned long)f->ierrors, (unsigned long)f->iqdrops,
		          (unsigned long)f->noproto, (unsigned long)f->imcasts);
		cl_puts("    TX:  bytes  packets errors dropped carrier collsns\n");
		cl_printf("    %10lu %8lu %6lu %7lu %7lu %7lu\n", (unsigned long)f->obytes,
		          (unsigned long)f->opackets, (unsigned long)f->oerrors, 0UL, 0UL,
		          (unsigned long)f->collisions);
	}
}

/* ---- driver counters (net.riptide.drivers), ip -s / ip -s -s ---- */

static const char *drv_err_name(const struct rt_ctl_driver *r, s32 code, char *buf)
{
	static const char *const ioerr[] = { "", "IOERR_OPENFAIL", "IOERR_ABORTED", "IOERR_NOCMD",
	                                     "IOERR_BADLENGTH", "IOERR_BADADDRESS", "IOERR_UNITBUSY",
	                                     "IOERR_SELFTEST" };
	static const char *const s2err[] = { "", "S2ERR_NO_RESOURCES", "", "S2ERR_BAD_ARGUMENT",
	                                     "S2ERR_BAD_STATE", "S2ERR_BAD_ADDRESS", "S2ERR_MTU_EXCEEDED",
	                                     "", "S2ERR_NOT_SUPPORTED", "S2ERR_SOFTWARE",
	                                     "S2ERR_OUTOFSERVICE", "S2ERR_TX_FAILURE" };

	if (code == RT_CTL_DRV_ERR_OTHER)
		return "other";
	if (code == RT_CTL_DRV_ERR_LENGTH)
		return "bad length";
	if (!strcmp(r->kind, "sana2")) {
		if (code < 0 && code >= -7)
			return ioerr[-code];
		if (code > 0 && code <= 11 && s2err[code][0])
			return s2err[code];
	}
	rt_snprintf(buf, 16, "%ld", (long)code);
	return buf;
}

static void print_drv_errs(const char *what, const struct rt_ctl_driver *r, const struct rt_ctl_drv_err *e)
{
	char b[16];
	int i;

	if (!e[0].count)
		return;
	cl_printf("    %s errors:", what);
	for (i = 0; i < RT_CTL_DRV_NERR && e[i].count; i++)
		cl_printf(" %s %lu", drv_err_name(r, e[i].code, b), (unsigned long)e[i].count);
	cl_puts("\n");
}

static const char *special_name(const struct rt_ctl_drv_special *s, char *buf)
{
	if (s->type == S2SS_ETHERNET_BADMULTICAST)
		return "bad multicast";
	if (s->type == S2SS_ETHERNET_RETRIES)
		return "retries";
	if (s->type == S2SS_ETHERNET_FIFO_UNDERRUNS)
		return "FIFO underruns";
	if (s->name[0])
		return s->name;
	rt_snprintf(buf, 16, "type 0x%lx", (unsigned long)s->type);
	return buf;
}

static void print_driver(const struct rt_ctl_driver *r)
{
	static const char *const qn[RT_CTL_DRV_RXQ] = { "IPv4", "ARP", "IPv6" };
	static const char *const bn[RT_CTL_DRV_BATCH] = { "1", "2-3", "4-7", "8-15", "16-31", "32+" };
	u32 q, wakeups = 0;
	char b[16];

	for (q = 0; q < RT_CTL_DRV_BATCH; q++)
		wakeups += r->batch[q];
	cl_printf("    driver %s %s unit %lu, counters of the last %lu s\n", r->kind, r->driver,
	          (unsigned long)r->unit, (unsigned long)r->since_s);
	if (r->flags & RT_CTL_DRVF_SHARED) {
		cl_printf("    RX buffers: %lu, free %lu, lowest %lu, ran out %lu\n",
		          (unsigned long)r->rxq[0].cfg, (unsigned long)r->rxq[0].queued,
		          (unsigned long)r->rxq[0].low, (unsigned long)r->rxq[0].empty);
	} else if (!(r->flags & RT_CTL_DRVF_NOQ)) {
		cl_puts("    RX reads:");
		for (q = 0; q < RT_CTL_DRV_RXQ; q++)
			if (r->rxq[q].cfg)
				cl_printf(" %s %lu/%lu lowest %lu ran out %lu%s", qn[q], (unsigned long)r->rxq[q].queued,
				          (unsigned long)r->rxq[q].cfg, (unsigned long)r->rxq[q].low,
				          (unsigned long)r->rxq[q].empty, q + 1 < RT_CTL_DRV_RXQ ? "," : "");
		cl_puts("\n");
	}
	cl_printf("    RX: frames %lu bytes %lu errors %lu nobuf %lu, wakeups %lu, at most %lu frames per wakeup\n",
	          (unsigned long)r->rx_frames, (unsigned long)r->rx_bytes, (unsigned long)r->rx_errors,
	          (unsigned long)r->rx_nobuf, (unsigned long)wakeups, (unsigned long)r->batch_max);
	cl_printf("    TX: frames %lu bytes %lu errors %lu queue full %lu, in flight %lu (most %lu",
	          (unsigned long)r->tx_frames, (unsigned long)r->tx_bytes, (unsigned long)r->tx_errors,
	          (unsigned long)r->tx_full, (unsigned long)r->tx_inflight, (unsigned long)r->tx_inflight_max);
	if (r->tx_slots)
		cl_printf(" of %lu", (unsigned long)r->tx_slots);
	cl_puts(")\n");
	if (r->flags & RT_CTL_DRVF_DROPS)
		cl_printf("    driver dropped %lu frames (no free buffer)\n", (unsigned long)r->drv_drops);
	if (r->flags & RT_CTL_DRVF_GS)
		cl_printf("    driver stats: rx %lu tx %lu bad %lu overruns %lu unknown types %lu\n",
		          (unsigned long)r->gs_rx, (unsigned long)r->gs_tx, (unsigned long)r->gs_baddata,
		          (unsigned long)r->gs_overruns, (unsigned long)r->gs_unknown);
	if (opt_stats < 2)
		return;
	cl_puts("    RX frames per wakeup:");
	for (q = 0; q < RT_CTL_DRV_BATCH; q++)
		cl_printf(" %s: %lu", bn[q], (unsigned long)r->batch[q]);
	cl_puts("\n");
	print_drv_errs("RX", r, r->rxerr);
	print_drv_errs("TX", r, r->txerr);
	if (r->flags & RT_CTL_DRVF_DQ)
		cl_printf("    device query: hardware type %lu, level %lu, mtu %lu, raw mtu %lu, %lu bit/s\n",
		          (unsigned long)r->dq_hwtype, (unsigned long)r->dq_level, (unsigned long)r->dq_mtu,
		          (unsigned long)r->dq_rawmtu, (unsigned long)r->dq_bps);
	if (r->flags & RT_CTL_DRVF_GS)
		cl_printf("    driver stats: reconfigurations %lu, online since %lu, %lu.%lu s old\n",
		          (unsigned long)r->gs_reconfig, (unsigned long)r->gs_laststart_s,
		          (unsigned long)(r->gs_age_ms / 1000), (unsigned long)(r->gs_age_ms % 1000 / 100));
	else if (!strcmp(r->kind, "sana2"))
		cl_puts("    driver stats: not supported by the driver\n");
	if (r->flags & RT_CTL_DRVF_SS) {
		cl_puts("    special stats:");
		if (!r->nspecial)
			cl_puts(" none");
		for (q = 0; q < r->nspecial && q < RT_CTL_DRV_NSPECIAL; q++)
			cl_printf("%s %s %lu", q ? "," : "", special_name(&r->special[q], b),
			          (unsigned long)r->special[q].count);
		cl_puts("\n");
	}
}

/* net.riptide.drivers for ip -s. The driver's own statistics are fetched
 * by Riptide when someone reads them: ask again shortly for fresh ones. */
static void *get_drivers(struct rt_ctl_hdr *h)
{
	void *d = cl_dump(RT_CTL_DRIVERS, h);
	u32 k;

	for (k = 0; d && k < h->count; k++) {
		struct rt_ctl_driver *r = cl_rec(d, h, k);
		if (!strcmp(r->kind, "sana2") && r->gs_age_ms >= 500) {
			FreeVec(d);
			Delay(10);
			return cl_dump(RT_CTL_DRIVERS, h);
		}
	}
	return d;
}

static void print_driver_of(const struct rt_ctl_iface *f, void *dd, const struct rt_ctl_hdr *dh)
{
	u32 k;

	for (k = 0; dd && k < dh->count; k++) {
		struct rt_ctl_driver *r = cl_rec(dd, dh, k);
		if (r->index == f->index) {
			print_driver(r);
			return;
		}
	}
}

static void print_lft(const struct rt_ctl_iface *f)
{
	if (f->dhcp_state >= RT_DHCPS_BOUND && f->dhcp_lease_s != 0xffffffffUL)
		cl_printf("       valid_lft %lusec preferred_lft %lusec\n", (unsigned long)f->dhcp_left_s,
		          (unsigned long)f->dhcp_left_s);
	else
		cl_puts("       valid_lft forever preferred_lft forever\n");
}

static void print_addr_lines(const struct rt_ctl_iface *f)
{
	char a[16], b[16];
	int bits = rt_ctl_prefixlen(f->netmask);

	if (!f->addr)
		return;
	if (f->type == RT_IFT_PTP && f->dstaddr)
		cl_printf("    inet %s peer %s/%d", cl_ip(f->addr, a), cl_ip(f->dstaddr, b), bits);
	else
		cl_printf("    inet %s/%d", cl_ip(f->addr, a), bits);
	if (f->type != RT_IFT_PTP && f->broadaddr)
		cl_printf(" brd %s", cl_ip(f->broadaddr, b));
	cl_printf(" scope %s", f->type == RT_IFT_LOOP || (rt_ntohl(f->addr) >> 24) == 127 ? "host" : "global");
	if (f->dhcp_state >= RT_DHCPS_BOUND)
		cl_puts(" dynamic");
	cl_printf(" %s\n", f->name);
	print_lft(f);
}

static const char *scope6_name(const struct rt_ctl_addr6 *x)
{
	rt_in6_addr a;

	memcpy(a.b, x->addr, 16);
	if (rt_in6_is_loopback(&a))
		return "host";
	return x->scope == 2 ? "link" : x->scope == 5 ? "site" : "global";
}

static void print_lft6(u32 v)
{
	if (v == 0xffffffffUL)
		cl_puts("forever");
	else
		cl_printf("%lusec", (unsigned long)v);
}

static void print_addr6_lines(const struct rt_ctl_iface *f, void *d6, const struct rt_ctl_hdr *h6)
{
	char a[RT_INET6_ADDRSTRLEN];
	u32 k;

	for (k = 0; d6 && k < h6->count; k++) {
		struct rt_ctl_addr6 *x = cl_rec(d6, h6, k);
		if (x->ifindex != f->index)
			continue;
		cl_printf("    inet6 %s/%u scope %s", cl_ip6(x->addr, a), (unsigned)x->plen, scope6_name(x));
		if (x->flags & RT_CTL_IN6_TEMPORARY)
			cl_puts(" temporary");
		if (x->flags & RT_CTL_IN6_AUTOCONF)
			cl_puts(" dynamic");
		if (x->flags & RT_CTL_IN6_NODAD)
			cl_puts(" nodad");
		if (x->flags & RT_CTL_IN6_DEPRECATED)
			cl_puts(" deprecated");
		if (x->flags & RT_CTL_IN6_DUPLICATED)
			cl_puts(" dadfailed");
		if (x->flags & RT_CTL_IN6_TENTATIVE)
			cl_puts(" tentative");
		cl_puts(" \n       valid_lft ");
		print_lft6(x->valid_s);
		cl_puts(" preferred_lft ");
		print_lft6(x->pref_s);
		cl_puts("\n");
	}
}

static int has_addr6(const struct rt_ctl_iface *f, void *d6, const struct rt_ctl_hdr *h6)
{
	u32 k;

	for (k = 0; d6 && k < h6->count; k++)
		if (((struct rt_ctl_addr6 *)cl_rec(d6, h6, k))->ifindex == f->index)
			return 1;
	return 0;
}

static struct rt_ctl_iface *get_ifaces(void **dump, struct rt_ctl_hdr *h)
{
	*dump = cl_dump(RT_CTL_IFACES, h);
	return *dump ? cl_rec(*dump, h, 0) : NULL;
}

/* "dev IF" or a bare IF at argv[i]; returns the name or NULL */
static const char *dev_arg(int argc, char **argv, int *i)
{
	if (*i < argc && !strcmp(argv[*i], "dev")) {
		if (*i + 1 >= argc)
			return NULL;
		*i += 2;
		return argv[*i - 1];
	}
	if (*i < argc) {
		(*i)++;
		return argv[*i - 1];
	}
	return NULL;
}

static int no_dev(const char *name)
{
	cl_printf("Device \"%s\" does not exist.\n", name);
	return CL_ERROR;
}

/* send a command and translate the result */
static int run(const char *line)
{
	int r = cl_command(line, 1);

	if (r < 0)
		return CL_FAIL;
	return r ? CL_ERROR : CL_OK;
}

/* ------------------------------------------------------------ link / addr */

static int do_show_ifs(int argc, char **argv, int i, int addrs)
{
	struct rt_ctl_hdr h, h6, dh;
	void *d, *d6 = NULL, *dd = NULL;
	const char *only = NULL;
	u32 k, j;
	char a[RT_INET6_ADDRSTRLEN], mac[64], fl[128], ab[160];

	while (i < argc) {
		if (!strcmp(argv[i], "dev") || !strcmp(argv[i], "label")) {
			if (i + 1 >= argc) {
				usage();
				return CL_ERROR;
			}
			only = argv[i + 1];
			i += 2;
		} else if (!strcmp(argv[i], "up") || !strcmp(argv[i], "scope") ||
		           !strcmp(argv[i], "primary")) {
			i += !strcmp(argv[i], "scope") ? 2 : 1;
		} else {
			only = argv[i++];
		}
	}
	if (!get_ifaces(&d, &h))
		return CL_FAIL;
	if (only && !cl_find_iface(d, &h, only)) {
		FreeVec(d);
		return no_dev(only);
	}
	if (addrs && opt_family != 4)
		d6 = cl_dump(RT_CTL_ADDRS6, &h6);
	if (opt_stats && !opt_brief)
		dd = get_drivers(&dh);
	for (k = 0; k < h.count; k++) {
		struct rt_ctl_iface *f = cl_rec(d, &h, k);
		if (only && strcmp(f->name, only))
			continue;
		if (addrs && opt_family == 6 && !has_addr6(f, d6, &h6))
			continue;
		if (opt_brief) {
			if (addrs) {
				u32 o = 0;
				ab[0] = 0;
				if (f->addr && opt_family != 6)
					o = (u32)rt_snprintf(ab, sizeof(ab), "%s/%d ", cl_ip(f->addr, a),
					                     rt_ctl_prefixlen(f->netmask));
				for (j = 0; d6 && j < h6.count && o + 1 < sizeof(ab); j++) {
					struct rt_ctl_addr6 *x = cl_rec(d6, &h6, j);
					if (x->ifindex == f->index)
						o += (u32)rt_snprintf(ab + o, sizeof(ab) - o, "%s/%u ", cl_ip6(x->addr, a),
						                      (unsigned)x->plen);
				}
				cl_printf("%-16s %-14s %s\n", f->name, link_state(f), ab);
			} else {
				link_flags(f, fl, sizeof(fl));
				cl_printf("%-16s %-14s %-17s %s\n", f->name, link_state(f),
				          f->type == RT_IFT_ETHER ? cl_mac(f->hwaddr, 6, mac) :
				          f->type == RT_IFT_LOOP ? "00:00:00:00:00:00" : "", fl);
			}
			continue;
		}
		print_link_line(f);
		if (dd)
			print_driver_of(f, dd, &dh);
		if (addrs && opt_family != 6)
			print_addr_lines(f);
		if (addrs)
			print_addr6_lines(f, d6, &h6);
	}
	FreeVec(d);
	if (d6)
		FreeVec(d6);
	if (dd)
		FreeVec(dd);
	return CL_OK;
}

static int do_link(int argc, char **argv, int i)
{
	const char *cmd = i < argc ? argv[i++] : "show";
	char line[RIPTIDE_TEXTLEN];
	u32 o;

	if (cl_match(cmd, "show") || cl_match(cmd, "list") || !strcmp(cmd, "lst"))
		return do_show_ifs(argc, argv, i, 0);
	if (cl_match(cmd, "help")) {
		usage();
		return CL_OK;
	}
	if (cl_match(cmd, "set")) {
		const char *dev = dev_arg(argc, argv, &i), *reads = NULL;
		u32 base;
		if (!dev) {
			usage();
			return CL_ERROR;
		}
		o = base = (u32)rt_snprintf(line, sizeof(line), "ifconfig %s", dev);
		for (; i < argc; i++) {
			const char *k = argv[i], *v = i + 1 < argc ? argv[i + 1] : "";
			if (!strcmp(k, "reads")) {
				/* SANA-II read requests: a line of its own */
				reads = v;
				i++;
			} else if (!strcmp(k, "up") || !strcmp(k, "down")) {
				o += (u32)rt_snprintf(line + o, sizeof(line) - o, " %s", k);
			} else if (!strcmp(k, "mtu") || !strcmp(k, "metric") || !strcmp(k, "tcpwindow")) {
				o += (u32)rt_snprintf(line + o, sizeof(line) - o, " %s %s", k, v);
				i++;
			} else if (!strcmp(k, "promisc") || !strcmp(k, "arp")) {
				int on = !strcmp(v, "on");
				if (!on && strcmp(v, "off")) {
					cl_printf("Error: argument of \"%s\" must be \"on\" or \"off\"\n", k);
					return CL_ERROR;
				}
				o += (u32)rt_snprintf(line + o, sizeof(line) - o, " %s%s", on ? "" : "-", k);
				i++;
			} else {
				cl_printf("Error: either \"dev\" is duplicate, or \"%s\" is a garbage.\n", k);
				return CL_ERROR;
			}
		}
		if (reads) {
			int rc = o > base ? run(line) : CL_OK;
			return rc != CL_OK ? rc : run(cl_line("interface %s reads=%s", dev, reads));
		}
		return run(line);
	}
	if (cl_match(cmd, "add")) {
		const char *name = NULL, *type = NULL, *dev = NULL, *unit = "0", *mtu = NULL, *opts = NULL;
		const char *reads = NULL;
		for (; i < argc; i++) {
			const char *k = argv[i], *eq = strchr(k, '=');
			char key[16];
			const char *v;
			if (eq) {
				u32 n = (u32)(eq - k) < sizeof(key) - 1 ? (u32)(eq - k) : sizeof(key) - 1;
				memcpy(key, k, n);
				key[n] = 0;
				v = eq + 1;
			} else {
				rt_strlcpy(key, k, sizeof(key));
				v = i + 1 < argc ? argv[i + 1] : NULL;
			}
#define TAKE(var) do { var = v; if (!eq) i++; if (!v) { usage(); return CL_ERROR; } } while (0)
			if (!strcmp(key, "type"))
				TAKE(type);
			else if (!strcmp(key, "device") || !strcmp(key, "driver") || !strcmp(key, "mni"))
				TAKE(dev);
			else if (!strcmp(key, "unit"))
				TAKE(unit);
			else if (!strcmp(key, "mtu"))
				TAKE(mtu);
			else if (!strcmp(key, "reads"))
				TAKE(reads);
			else if (!strcmp(key, "options"))
				TAKE(opts);
			else if (!strcmp(key, "name") || !strcmp(key, "dev"))
				TAKE(name);
			else if (!name && !eq)
				name = k;
			else {
				cl_printf("Error: unknown argument \"%s\".\n", k);
				return CL_ERROR;
			}
#undef TAKE
		}
		if (!name || !type || !dev) {
			cl_puts("Error: ip link add NAME type { sana2 device DEV | mni driver FILE } [unit N]\n");
			return CL_ERROR;
		}
		if (!strcmp(type, "sana2") || !strcmp(type, "sana"))
			o = (u32)rt_snprintf(line, sizeof(line), "interface %s device=\"%s\" unit=%s", name, dev, unit);
		else if (!strcmp(type, "mni"))
			o = (u32)rt_snprintf(line, sizeof(line), "interface %s mni=\"%s\" unit=%s", name, dev, unit);
		else {
			cl_printf("Error: unknown link type \"%s\" (sana2 or mni).\n", type);
			return CL_ERROR;
		}
		if (mtu)
			o += (u32)rt_snprintf(line + o, sizeof(line) - o, " mtu=%s", mtu);
		if (reads)
			o += (u32)rt_snprintf(line + o, sizeof(line) - o, " reads=%s", reads);
		if (opts)
			rt_snprintf(line + o, sizeof(line) - o, " options=\"%s\"", opts);
		return run(line);
	}
	if (cl_match(cmd, "delete")) {
		const char *dev = dev_arg(argc, argv, &i);
		if (!dev) {
			usage();
			return CL_ERROR;
		}
		return run(cl_line("interface %s delete", dev));
	}
	if (!strcmp(cmd, "resetstats")) {
		const char *dev = i < argc ? dev_arg(argc, argv, &i) : "all";
		if (!dev) {
			usage();
			return CL_ERROR;
		}
		return run(cl_line("interface %s resetstats", dev));
	}
	cl_printf("Command \"%s\" is unknown, try \"ip link help\".\n", cmd);
	return CL_ERROR;
}

static int addr6_change(const char *cmd, const char *pfx, const char *dev, int nodad)
{
	rt_in6_addr a;
	int plen;
	char s[RT_INET6_ADDRSTRLEN];

	if (rt_inet6_parse_prefix(pfx, &a, &plen, NULL, 0) < 0) {
		cl_printf("Error: an inet6 prefix is expected rather than \"%s\".\n", pfx);
		return CL_ERROR;
	}
	rt_inet6_ntop(&a, s, sizeof(s));
	if (cl_match(cmd, "delete"))
		return run(cl_line("ifconfig %s inet6 %s delete", dev, s));
	if (!cl_match(cmd, "add") && !cl_match(cmd, "replace") && !cl_match(cmd, "change")) {
		cl_printf("Command \"%s\" is unknown, try \"ip address help\".\n", cmd);
		return CL_ERROR;
	}
	return run(cl_line("ifconfig %s inet6 %s/%d%s", dev, s, plen < 0 ? 128 : plen, nodad ? " nodad" : ""));
}

/* delete the IPv6 addresses of an interface except the link-local ones */
static int addr6_flush(const char *dev, u32 index)
{
	struct rt_ctl_hdr h6;
	void *d6 = cl_dump(RT_CTL_ADDRS6, &h6);
	char s[RT_INET6_ADDRSTRLEN];
	u32 k;
	int rc = CL_OK;

	if (!d6)
		return CL_FAIL;
	for (k = 0; k < h6.count; k++) {
		struct rt_ctl_addr6 *x = cl_rec(d6, &h6, k);
		if (x->ifindex != index || x->scope <= 2)
			continue;
		if (run(cl_line("ifconfig %s inet6 %s delete", dev, cl_ip6(x->addr, s))) != CL_OK)
			rc = CL_ERROR;
	}
	FreeVec(d6);
	return rc;
}

static int do_addr(int argc, char **argv, int i)
{
	const char *cmd = i < argc ? argv[i++] : "show";
	struct rt_ctl_hdr h;
	struct rt_ctl_iface *f;
	void *d;
	const char *dev = NULL, *pfx = NULL;
	char line[RIPTIDE_TEXTLEN], a[16];
	u32 addr, peer = 0, brd = 0;
	int bits, replace = 0, r, nodad = 0;

	if (cl_match(cmd, "show") || cl_match(cmd, "list") || !strcmp(cmd, "lst"))
		return do_show_ifs(argc, argv, i, 1);
	if (cl_match(cmd, "help")) {
		usage();
		return CL_OK;
	}
	for (; i < argc; i++) {
		const char *k = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
		if (!strcmp(k, "dev") && v) {
			dev = v;
			i++;
		} else if ((!strcmp(k, "peer") || !strcmp(k, "remote")) && v) {
			if (cl_parse_prefix(v, &peer, &bits) < 0)
				goto bad;
			i++;
		} else if ((!strcmp(k, "broadcast") || !strcmp(k, "brd")) && v) {
			if (strcmp(v, "+") && strcmp(v, "-") && cl_parse_ip(v, &brd) < 0)
				goto bad;
			i++;
		} else if ((!strcmp(k, "label") || !strcmp(k, "scope") || !strcmp(k, "valid_lft") ||
		            !strcmp(k, "preferred_lft")) && v) {
			i++;
		} else if (!strcmp(k, "local") && v) {
			pfx = v;
			i++;
		} else if (!strcmp(k, "nodad") || !strcmp(k, "noprefixroute") || !strcmp(k, "home") ||
		           !strcmp(k, "mngtmpaddr")) {
			nodad |= !strcmp(k, "nodad");
		} else if (!pfx) {
			pfx = k;
		} else {
			goto bad;
		}
	}
	if (cl_match(cmd, "flush")) {
		if (!dev) {
			cl_puts("Flush requires arguments.\n");
			return CL_ERROR;
		}
		if (!get_ifaces(&d, &h))
			return CL_FAIL;
		f = cl_find_iface(d, &h, dev);
		r = f && f->addr && opt_family != 6;
		bits = f ? (int)f->index : 0;
		FreeVec(d);
		if (!f)
			return no_dev(dev);
		if (r && run(cl_line("ifconfig %s delete", dev)) != CL_OK)
			return CL_ERROR;
		return opt_family == 4 ? CL_OK : addr6_flush(dev, (u32)bits);
	}
	if (pfx && dev && (opt_family == 6 || strchr(pfx, ':')))
		return addr6_change(cmd, pfx, dev, nodad);
	if (!pfx || !dev) {
		cl_puts("Not enough information: \"dev\" argument is required.\n");
		return CL_ERROR;
	}
	if (cl_parse_prefix(pfx, &addr, &bits) < 0)
		goto bad;
	if (!get_ifaces(&d, &h))
		return CL_FAIL;
	f = cl_find_iface(d, &h, dev);
	if (!f) {
		FreeVec(d);
		return no_dev(dev);
	}
	if (cl_match(cmd, "delete")) {
		r = f->addr == addr;
		FreeVec(d);
		if (!r) {
			cl_puts("Error: ipv4: Address not found.\n");
			return CL_ERROR;
		}
		return run(cl_line("ifconfig %s delete", dev));
	}
	if (cl_match(cmd, "replace") || cl_match(cmd, "change"))
		replace = 1;
	else if (!cl_match(cmd, "add")) {
		FreeVec(d);
		cl_printf("Command \"%s\" is unknown, try \"ip address help\".\n", cmd);
		return CL_ERROR;
	}
	if (!replace && f->addr && f->addr != addr) {
		cl_printf("Error: %s already has %s (Riptide keeps one IPv4 address per interface;\n"
		          "use \"ip addr replace\" or delete it first).\n", dev, cl_ip(f->addr, a));
		FreeVec(d);
		return CL_ERROR;
	}
	if (!replace && f->addr == addr && f->netmask == cl_mask(bits < 0 ? 32 : bits)) {
		FreeVec(d);
		cl_puts("Error: ipv4: Address already assigned.\n");
		return CL_ERROR;
	}
	FreeVec(d);
	r = rt_snprintf(line, sizeof(line), "ifconfig %s %s/%d", dev, cl_ip(addr, a), bits < 0 ? 32 : bits);
	if (peer)
		r += rt_snprintf(line + r, sizeof(line) - (u32)r, " peer %s", cl_ip(peer, a));
	if (brd)
		rt_snprintf(line + r, sizeof(line) - (u32)r, " broadcast %s", cl_ip(brd, a));
	return run(line);
bad:
	cl_printf("Error: an inet prefix is expected rather than \"%s\".\n", argv[i]);
	return CL_ERROR;
}

/* ------------------------------------------------------------ route */

static const char *proto_name(u8 p)
{
	switch (p) {
	case RT_PROTO_REDIRECT: return "redirect";
	case RT_PROTO_KERNEL: return "kernel";
	case RT_PROTO_BOOT: return "boot";
	case RT_PROTO_STATIC: return "static";
	case RT_PROTO_DHCP: return "dhcp";
	}
	return "unspec";
}

static void print_route(const struct rt_ctl_route *r, void *ifd, const struct rt_ctl_hdr *ih)
{
	struct rt_ctl_iface *f = ifd ? cl_iface_by_index(ifd, ih, r->ifindex) : NULL;
	char a[16];

	if (r->flags & RT_RTF_REJECT)
		cl_puts("unreachable ");
	if (!r->dst && !r->mask)
		cl_puts("default");
	else if (r->mask == 0xffffffffUL)
		cl_printf("%s", cl_ip(r->dst, a));
	else
		cl_printf("%s/%d", cl_ip(r->dst, a), rt_ctl_prefixlen(r->mask));
	if (r->flags & RT_RTF_GATEWAY)
		cl_printf(" via %s", cl_ip(r->gateway, a));
	if (r->ifname[0])
		cl_printf(" dev %s", r->ifname);
	cl_printf(" proto %s", proto_name(r->proto));
	if (r->proto == RT_PROTO_KERNEL && !(r->flags & RT_RTF_GATEWAY))
		cl_puts(" scope link");
	if (f && f->addr && (r->proto == RT_PROTO_KERNEL || r->proto == RT_PROTO_DHCP))
		cl_printf(" src %s", cl_ip(f->addr, a));
	if (r->metric)
		cl_printf(" metric %lu", (unsigned long)r->metric);
	if (r->mtu)
		cl_printf(" mtu %lu", (unsigned long)r->mtu);
	if (r->linkdown)
		cl_puts(" linkdown");
	cl_puts("\n");
	if (opt_stats && r->use)
		cl_printf("    used %lu\n", (unsigned long)r->use);
}

static const char *proto6_name(u8 p)
{
	return p == RT_PROTO_RA ? "ra" : proto_name(p);
}

static void print_route6(const struct rt_ctl_route6 *r)
{
	char a[RT_INET6_ADDRSTRLEN];
	static const u8 zero[16];

	if (r->flags & RT_RTF_REJECT)
		cl_puts("unreachable ");
	if (r->plen == 0)
		cl_puts("default");
	else if (r->plen == 128)
		cl_printf("%s", cl_ip6(r->dst, a));
	else
		cl_printf("%s/%u", cl_ip6(r->dst, a), (unsigned)r->plen);
	if (memcmp(r->gateway, zero, 16))
		cl_printf(" via %s", cl_ip6(r->gateway, a));
	if (r->ifname[0])
		cl_printf(" dev %s", r->ifname);
	cl_printf(" proto %s", proto6_name(r->proto));
	cl_printf(" metric %lu", (unsigned long)r->metric);
	if (r->expire_s)
		cl_printf(" expires %lusec", (unsigned long)r->expire_s);
	if (r->mtu)
		cl_printf(" mtu %lu", (unsigned long)r->mtu);
	if (r->linkdown)
		cl_puts(" linkdown");
	cl_printf(" pref %s\n", r->pref > 0 ? "high" : r->pref < 0 ? "low" : "medium");
	if (opt_stats && r->use)
		cl_printf("    used %lu\n", (unsigned long)r->use);
}

static int route6_show(int argc, char **argv, int i)
{
	struct rt_ctl_hdr h;
	void *d;
	int cache = 0;
	const char *dev = NULL;
	u32 k;
	char a[RT_INET6_ADDRSTRLEN];

	for (; i < argc; i++) {
		if (!strcmp(argv[i], "cache"))
			cache = 1;
		else if (!strcmp(argv[i], "dev") && i + 1 < argc)
			dev = argv[++i];
		else if (!strcmp(argv[i], "table") && i + 1 < argc)
			i++;
	}
	d = cl_dump(RT_CTL_ROUTES6, &h);
	if (!d)
		return CL_FAIL;
	for (k = 0; k < h.count && !cl_break(); k++) {
		struct rt_ctl_route6 *r = cl_rec(d, &h, k);
		if (dev && strcmp(r->ifname, dev))
			continue;
		if (!!(r->flags & (RT_RTF_PMTU | RT_RTF_DYNAMIC)) != cache)
			continue;
		if (cache) {
			cl_printf("%s dev %s", cl_ip6(r->dst, a), r->ifname);
			cl_puts("\n    cache");
			if (r->expire_s)
				cl_printf(" expires %lusec", (unsigned long)r->expire_s);
			if (r->mtu)
				cl_printf(" mtu %lu", (unsigned long)r->mtu);
			cl_puts("\n");
			continue;
		}
		print_route6(r);
	}
	FreeVec(d);
	return CL_OK;
}

static int prefix_match(const u8 *a, const u8 *p, int plen)
{
	int i;

	for (i = 0; i < plen; i++)
		if (((a[i >> 3] ^ p[i >> 3]) >> (7 - (i & 7))) & 1)
			return 0;
	return 1;
}

/* ip -6 route get: longest prefix over the table copy; the source is the
 * first usable address of the outgoing interface (link-local for link-local
 * destinations); the stack's RFC 6724 selection may differ in corner cases. */
static int route6_get(const char *target)
{
	struct rt_ctl_hdr h, ah;
	void *d, *ad;
	struct rt_ctl_route6 *best = NULL;
	struct cl_addr dst;
	char a[CL_ADDRSTRLEN];
	u32 k;
	static const u8 zero[16];

	if (cl_resolve_af(target, 6, &dst) < 0)
		return CL_ERROR;
	d = cl_dump(RT_CTL_ROUTES6, &h);
	ad = d ? cl_dump(RT_CTL_ADDRS6, &ah) : NULL;
	if (!d || !ad) {
		if (d)
			FreeVec(d);
		return CL_FAIL;
	}
	for (k = 0; k < ah.count; k++) {
		struct rt_ctl_addr6 *x = cl_rec(ad, &ah, k);
		if (!memcmp(x->addr, dst.a.b, 16)) {
			cl_printf("local %s dev lo0 proto kernel src ", cl_ip6(x->addr, a));
			cl_printf("%s metric 0 pref medium\n", cl_ip6(x->addr, a));
			FreeVec(d);
			FreeVec(ad);
			return CL_OK;
		}
	}
	for (k = 0; k < h.count; k++) {
		struct rt_ctl_route6 *r = cl_rec(d, &h, k);
		if ((r->flags & RT_RTF_PMTU) || r->linkdown || !prefix_match(dst.a.b, r->dst, r->plen))
			continue;
		if (dst.scope && r->ifindex != dst.scope)
			continue;
		if (!best || r->plen > best->plen || (r->plen == best->plen && r->metric < best->metric))
			best = r;
	}
	if (!best || (best->flags & RT_RTF_REJECT)) {
		cl_printf("RTNETLINK answers: %s\n", best ? "No route to host" : "Network is unreachable");
		FreeVec(d);
		FreeVec(ad);
		return CL_ERROR;
	}
	cl_printf("%s", cl_addr_str(&dst, a));
	if (memcmp(best->gateway, zero, 16))
		cl_printf(" via %s", cl_ip6(best->gateway, a));
	cl_printf(" dev %s proto %s", best->ifname, proto6_name(best->proto));
	for (k = 0; k < ah.count; k++) {
		struct rt_ctl_addr6 *x = cl_rec(ad, &ah, k);
		int ll = dst.a.b[0] == 0xfe && (dst.a.b[1] & 0xc0) == 0x80;
		if (x->ifindex == best->ifindex && !(x->flags & (RT_CTL_IN6_TENTATIVE | RT_CTL_IN6_DUPLICATED |
		                                                   RT_CTL_IN6_DEPRECATED)) &&
		    (x->scope == 2) == ll) {
			cl_printf(" src %s", cl_ip6(x->addr, a));
			break;
		}
	}
	cl_printf(" metric %lu pref medium\n", (unsigned long)best->metric);
	FreeVec(d);
	FreeVec(ad);
	return CL_OK;
}

static int route_show(int argc, char **argv, int i)
{
	struct rt_ctl_hdr h, ih;
	void *d, *ifd;
	int cache = 0;
	const char *dev = NULL;
	u32 k;
	char a[16];

	for (; i < argc; i++) {
		if (!strcmp(argv[i], "cache"))
			cache = 1;
		else if (!strcmp(argv[i], "dev") && i + 1 < argc)
			dev = argv[++i];
		else if (!strcmp(argv[i], "table") && i + 1 < argc)
			i++;
	}
	d = cl_dump(RT_CTL_ROUTES, &h);
	if (!d)
		return CL_FAIL;
	cl_sort_routes(d, &h);
	ifd = cl_dump(RT_CTL_IFACES, &ih);
	for (k = 0; k < h.count && !cl_break(); k++) {
		struct rt_ctl_route *r = cl_rec(d, &h, k);
		if (dev && strcmp(r->ifname, dev))
			continue;
		if (cache) {
			if (!(r->flags & (RT_RTF_PMTU | RT_RTF_DYNAMIC)))
				continue;
			cl_printf("%s", cl_ip(r->dst, a));
			if (r->flags & RT_RTF_GATEWAY)
				cl_printf(" via %s", cl_ip(r->gateway, a));
			cl_printf(" dev %s\n    cache", r->ifname);
			if (r->expire_s)
				cl_printf(" expires %lusec", (unsigned long)r->expire_s);
			if (r->mtu)
				cl_printf(" mtu %lu", (unsigned long)r->mtu);
			cl_puts("\n");
			continue;
		}
		if (r->flags & RT_RTF_PMTU)
			continue;
		print_route(r, ifd, &ih);
	}
	FreeVec(d);
	if (ifd)
		FreeVec(ifd);
	return CL_OK;
}

/* ip route get: the stack's lookup, done on the table copy (local
 * addresses first, then the first usable entry in table order, which is
 * longest prefix then metric). */
static int route_get(const char *target)
{
	struct rt_ctl_hdr h, ih;
	void *d, *ifd;
	struct rt_ctl_route *best = NULL, *pm = NULL;
	struct rt_ctl_iface *f;
	u32 dst, k, mtu;
	char a[16];

	if (cl_resolve(target, &dst) < 0)
		return CL_ERROR;
	d = cl_dump(RT_CTL_ROUTES, &h);
	ifd = d ? cl_dump(RT_CTL_IFACES, &ih) : NULL;
	if (!d || !ifd) {
		if (d)
			FreeVec(d);
		return CL_FAIL;
	}
	for (k = 0; k < ih.count; k++) {
		f = cl_rec(ifd, &ih, k);
		if ((f->addr == dst && f->addr) || ((rt_ntohl(dst) >> 24) == 127 && f->type == RT_IFT_LOOP)) {
			cl_printf("local %s dev lo0 src %s\n    cache <local>\n", cl_ip(dst, a),
			          (rt_ntohl(dst) >> 24) == 127 ? "127.0.0.1" : cl_ip(dst, a));
			FreeVec(d);
			FreeVec(ifd);
			return CL_OK;
		}
	}
	for (k = 0; k < h.count; k++) {
		struct rt_ctl_route *r = cl_rec(d, &h, k);
		if ((dst & r->mask) != r->dst)
			continue;
		if (r->flags & RT_RTF_PMTU) {
			if (!pm)
				pm = r;
			continue;
		}
		if (!best && (r->flags & RT_RTF_REJECT))
			best = r;
		if (!best && !r->linkdown && r->ifindex)
			best = r;
	}
	if (!best || (best->flags & RT_RTF_REJECT)) {
		cl_printf("RTNETLINK answers: %s\n", best ? "No route to host" : "Network is unreachable");
		FreeVec(d);
		FreeVec(ifd);
		return CL_ERROR;
	}
	f = cl_iface_by_index(ifd, &ih, best->ifindex);
	cl_printf("%s", cl_ip(dst, a));
	if (best->flags & RT_RTF_GATEWAY)
		cl_printf(" via %s", cl_ip(best->gateway, a));
	cl_printf(" dev %s", best->ifname);
	if (f && f->addr)
		cl_printf(" src %s", cl_ip(f->addr, a));
	cl_puts("\n    cache");
	mtu = f ? f->mtu : 0;
	if (best->mtu && best->mtu < mtu)
		mtu = best->mtu;
	if (pm && pm->mtu && pm->mtu < mtu) {
		cl_printf(" expires %lusec mtu %lu", (unsigned long)pm->expire_s, (unsigned long)pm->mtu);
	} else if (best->mtu) {
		cl_printf(" mtu %lu", (unsigned long)mtu);
	}
	cl_puts("\n");
	FreeVec(d);
	FreeVec(ifd);
	return CL_OK;
}

static int do_route(int argc, char **argv, int i)
{
	const char *cmd = i < argc ? argv[i++] : "show";
	char line[RIPTIDE_TEXTLEN];
	const char *op, *type = NULL;
	u32 o;

	if (cl_match(cmd, "show") || cl_match(cmd, "list") || !strcmp(cmd, "lst"))
		return opt_family == 6 ? route6_show(argc, argv, i) : route_show(argc, argv, i);
	if (cl_match(cmd, "help")) {
		usage();
		return CL_OK;
	}
	if (cl_match(cmd, "get")) {
		if (i >= argc) {
			cl_puts("Error: need at least a destination address\n");
			return CL_ERROR;
		}
		if (!strcmp(argv[i], "to") && i + 1 < argc)
			i++;
		if (opt_family == 6 || strchr(argv[i], ':'))
			return route6_get(argv[i]);
		return route_get(argv[i]);
	}
	if (cl_match(cmd, "flush")) {
		if (i >= argc) {
			cl_puts("\"ip route flush\" requires arguments.\n");
			return CL_ERROR;
		}
		if (!strcmp(argv[i], "cache"))
			return run("route flush cache");
		if (!strcmp(argv[i], "dev") && i + 1 < argc)
			return run(cl_line("route flush dev %s", argv[i + 1]));
		if (!strcmp(argv[i], "all") || !strcmp(argv[i], "table"))
			return run("route flush");
		cl_printf("Error: unsupported flush selector \"%s\".\n", argv[i]);
		return CL_ERROR;
	}
	if (cl_match(cmd, "add"))
		op = "add";
	else if (cl_match(cmd, "delete"))
		op = "del";
	else if (cl_match(cmd, "replace"))
		op = "replace";
	else if (cl_match(cmd, "change"))
		op = "change";
	else if (cl_match(cmd, "append"))
		op = "append";
	else {
		cl_printf("Command \"%s\" is unknown, try \"ip route help\".\n", cmd);
		return CL_ERROR;
	}
	if (i < argc && (!strcmp(argv[i], "unreachable") || !strcmp(argv[i], "blackhole") ||
	                 !strcmp(argv[i], "prohibit") || !strcmp(argv[i], "unicast")))
		type = argv[i++];
	if (i < argc && !strcmp(argv[i], "to"))
		i++;
	if (i >= argc) {
		cl_puts("Error: destination prefix is required.\n");
		return CL_ERROR;
	}
	o = (u32)rt_snprintf(line, sizeof(line), "route %s%s %s", op, opt_family == 6 ? " -inet6" : "", argv[i++]);
	for (; i < argc; i++)
		o += (u32)rt_snprintf(line + o, sizeof(line) - o, " %s", argv[i]);
	if (type && strcmp(type, "unicast"))
		rt_snprintf(line + o, sizeof(line) - o, " %s", type);
	return run(line);
}

/* ------------------------------------------------------------ neigh */

static void neigh6_show(const char *dev)
{
	struct rt_ctl_hdr h;
	void *d = cl_dump(RT_CTL_NEIGH6, &h);
	char a[RT_INET6_ADDRSTRLEN], mac[24];
	u32 k;

	for (k = 0; d && k < h.count; k++) {
		struct rt_ctl_neigh6 *n = cl_rec(d, &h, k);
		if (dev && strcmp(n->ifname, dev))
			continue;
		cl_printf("%s dev %s ", cl_ip6(n->addr, a), n->ifname);
		if (n->state != RT_NUD_INCOMPLETE && n->state != RT_NUD_FAILED)
			cl_printf("lladdr %s ", cl_mac(n->hwaddr, 6, mac));
		if (n->router)
			cl_puts("router ");
		if (opt_stats)
			cl_printf("used %lu/%lu/%lu probes 0 ", (unsigned long)n->age_s, (unsigned long)n->age_s,
			          (unsigned long)n->age_s);
		cl_printf("%s\n", rt_ctl_nud_name(n->state));
	}
	if (d)
		FreeVec(d);
}

static int do_neigh(int argc, char **argv, int i)
{
	const char *cmd = i < argc ? argv[i++] : "show";
	struct rt_ctl_hdr h;
	void *d;
	u32 k;
	char line[RIPTIDE_TEXTLEN], a[16], mac[24];

	if (cl_match(cmd, "show") || cl_match(cmd, "list") || !strcmp(cmd, "lst")) {
		const char *dev = NULL;
		u32 to = 0;
		int have_to = 0, bits;
		for (; i < argc; i++) {
			if (!strcmp(argv[i], "dev") && i + 1 < argc)
				dev = argv[++i];
			else if (cl_parse_prefix(argv[i], &to, &bits) == 0)
				have_to = 1;
		}
		d = opt_family == 6 ? NULL : cl_dump(RT_CTL_NEIGH, &h);
		if (!d && opt_family != 6)
			return CL_FAIL;
		for (k = 0; d && k < h.count; k++) {
			struct rt_ctl_neigh *n = cl_rec(d, &h, k);
			if ((dev && strcmp(n->ifname, dev)) || (have_to && n->addr != to))
				continue;
			cl_printf("%s dev %s ", cl_ip(n->addr, a), n->ifname);
			if (n->state != RT_NUD_INCOMPLETE)
				cl_printf("lladdr %s ", cl_mac(n->hwaddr, 6, mac));
			else
				cl_puts(" ");
			if (opt_stats)
				cl_printf("used %lu/%lu/%lu probes 0 ", (unsigned long)n->age_s,
				          (unsigned long)n->age_s, (unsigned long)n->age_s);
			cl_printf("%s\n", rt_ctl_nud_name(n->state));
		}
		if (d)
			FreeVec(d);
		if (opt_family != 4)
			neigh6_show(dev);
		return CL_OK;
	}
	if (cl_match(cmd, "help")) {
		usage();
		return CL_OK;
	}
	if (cl_match(cmd, "flush")) {
		const char *dev = NULL;
		if (i + 1 < argc && !strcmp(argv[i], "dev"))
			dev = argv[i + 1];
		else if (i < argc)
			dev = argv[i];
		return run(dev ? cl_line("arp flush dev %s", dev) : "arp flush");
	}
	if (cl_match(cmd, "add") || cl_match(cmd, "replace") || cl_match(cmd, "change") ||
	    cl_match(cmd, "delete")) {
		int del = cl_match(cmd, "delete");
		u32 o = (u32)rt_snprintf(line, sizeof(line), "arp %s", del ? "del" : "add");
		if (i < argc && !strcmp(argv[i], "to"))
			i++;
		for (; i < argc; i++) {
			if (!strcmp(argv[i], "nud") && i + 1 < argc) {
				if (strcmp(argv[i + 1], "permanent") && strcmp(argv[i + 1], "noarp") &&
				    strcmp(argv[i + 1], "reachable") && strcmp(argv[i + 1], "stale")) {
					cl_printf("Error: nud state \"%s\" is not supported.\n", argv[i + 1]);
					return CL_ERROR;
				}
				i++;
				continue;
			}
			if (!strcmp(argv[i], "proxy")) {
				cl_puts("Error: proxy ARP is not supported.\n");
				return CL_ERROR;
			}
			o += (u32)rt_snprintf(line + o, sizeof(line) - o, " %s", argv[i]);
		}
		return run(line);
	}
	cl_printf("Command \"%s\" is unknown, try \"ip neigh help\".\n", cmd);
	return CL_ERROR;
}

/* ------------------------------------------------------------ dhcp / dns */

static int do_dhcp(int argc, char **argv, int i)
{
	const char *cmd = i < argc ? argv[i] : "show";
	struct rt_ctl_hdr h;
	void *d;
	u32 k;
	char a[16], b[16];

	if (i < argc && (cl_match(cmd, "show") || cl_match(cmd, "list")))
		i++;
	else if (i < argc && (cl_match(cmd, "start") || cl_match(cmd, "stop") || cl_match(cmd, "renew") ||
	                      cl_match(cmd, "release"))) {
		const char *dev;
		const char *op = cl_match(cmd, "start") ? "start" : cl_match(cmd, "stop") ? "stop" :
		                 cl_match(cmd, "renew") ? "renew" : "release";
		i++;
		dev = dev_arg(argc, argv, &i);
		if (!dev) {
			cl_puts("Error: ip dhcp { start | stop | renew } dev IF\n");
			return CL_ERROR;
		}
		return run(cl_line("dhcp %s %s", dev, op));
	} else if (i < argc && strcmp(cmd, "dev")) {
		cl_printf("Command \"%s\" is unknown, try \"ip help\".\n", cmd);
		return CL_ERROR;
	}
	{
		const char *dev = NULL;
		if (i < argc)
			dev = dev_arg(argc, argv, &i);
		d = cl_dump(RT_CTL_IFACES, &h);
		if (!d)
			return CL_FAIL;
		if (dev && !cl_find_iface(d, &h, dev)) {
			FreeVec(d);
			return no_dev(dev);
		}
		for (k = 0; k < h.count; k++) {
			struct rt_ctl_iface *f = cl_rec(d, &h, k);
			if ((dev && strcmp(f->name, dev)) || (!dev && f->dhcp_state == RT_DHCPS_NONE))
				continue;
			cl_printf("%s: %s", f->name, rt_ctl_dhcp_state_name(f->dhcp_state));
			if (f->dhcp_state >= RT_DHCPS_BOUND) {
				cl_printf(" %s/%d server %s", cl_ip(f->addr, a), rt_ctl_prefixlen(f->netmask),
				          cl_ip(f->dhcp_server, b));
				if (f->dhcp_router)
					cl_printf(" router %s", cl_ip(f->dhcp_router, a));
				if (f->dhcp_lease_s == 0xffffffffUL)
					cl_puts(" lease infinite");
				else
					cl_printf(" lease %lus valid_lft %lusec", (unsigned long)f->dhcp_lease_s,
					          (unsigned long)f->dhcp_left_s);
			}
			cl_puts("\n");
		}
		FreeVec(d);
	}
	return CL_OK;
}

static int do_dns(int argc, char **argv, int i)
{
	const char *cmd = i < argc ? argv[i++] : "show";
	struct rt_ctl_hdr h;
	struct rt_ctl_dns *dn;
	void *d;
	u32 k;
	char a[16];

	if (cl_match(cmd, "show") || cl_match(cmd, "list")) {
		d = cl_dump(RT_CTL_DNS6, &h);
		if (d) {
			/* every server, both families, with where it came from */
			static const char *const src[] = { "", " (router advertisement)", " (DHCPv6)" };
			struct rt_ctl_dns6 *d6 = cl_rec(d, &h, 0);
			for (k = 0; k < d6->nservers && k < RT_CTL_MAXNS; k++) {
				struct cl_addr x;
				char s[CL_ADDRSTRLEN];
				memset(&x, 0, sizeof(x));
				memcpy(x.a.b, d6->servers[k].addr, 16);
				x.family = rt_in6_is_v4mapped(&x.a) ? RT_AF_INET : RT_AF_INET6;
				x.scope = d6->servers[k].ifindex;
				cl_printf("nameserver %s%s\n", cl_addr_str(&x, s),
				          d6->servers[k].source <= 2 ? src[d6->servers[k].source] : "");
			}
			if (d6->domain[0])
				cl_printf("search %s\n", d6->domain);
			FreeVec(d);
			return CL_OK;
		}
		d = cl_dump(RT_CTL_DNS, &h);
		if (!d)
			return CL_FAIL;
		dn = cl_rec(d, &h, 0);
		for (k = 0; k < dn->nservers && k < RT_CTL_MAXNS; k++)
			cl_printf("nameserver %s\n", cl_ip(dn->servers[k], a));
		if (dn->domain[0])
			cl_printf("search %s\n", dn->domain);
		FreeVec(d);
		return CL_OK;
	}
	if ((cl_match(cmd, "add") || cl_match(cmd, "delete")) && i < argc)
		return run(cl_line("nameserver %s %s", cl_match(cmd, "add") ? "add" : "del", argv[i]));
	if (cl_match(cmd, "flush"))
		return run("nameserver flush");
	if ((cl_match(cmd, "search") || cl_match(cmd, "domain")) && i < argc)
		return run(cl_line("domain %s", argv[i]));
	cl_puts("Usage: ip dns [show] | ip dns { add | del } ADDR | ip dns flush | ip dns search DOMAIN\n");
	return CL_ERROR;
}

/* ------------------------------------------------------------ main */

static int ip_main(int argc, char **argv)
{
	int i = 1;
	const char *obj;

	for (; i < argc && argv[i][0] == '-'; i++) {
		const char *o = argv[i] + 1;
		if (*o == '-')
			o++;
		if (!strcmp(o, "s") || cl_match(o, "statistics"))
			opt_stats++;
		else if (!strcmp(o, "ss"))
			opt_stats += 2;
		else if (cl_match(o, "brief") && strlen(o) >= 2)
			opt_brief = 1;
		else if (cl_match(o, "details"))
			opt_details = 1;
		else if (!strcmp(o, "4"))
			opt_family = 4;
		else if (!strcmp(o, "6"))
			opt_family = 6;
		else if (!strcmp(o, "f") || !strcmp(o, "family")) {
			if (++i >= argc) {
				usage();
				return CL_ERROR;
			}
			opt_family = !strcmp(argv[i], "inet6") ? 6 : !strcmp(argv[i], "inet") ? 4 : 0;
		} else if (cl_match(o, "Version")) {
			cl_puts("ip utility, Riptide 0.11\n");
			return CL_OK;
		} else if (cl_match(o, "help")) {
			usage();
			return CL_OK;
		} else if (!strcmp(o, "o") || cl_match(o, "oneline") || !strcmp(o, "n") || cl_match(o, "numeric")) {
			;
		} else {
			cl_printf("Option \"%s\" is unknown, try \"ip -help\".\n", argv[i]);
			return CL_ERROR;
		}
	}
	if (i >= argc) {
		usage();
		return CL_ERROR;
	}
	obj = argv[i++];
	if (cl_match(obj, "address") || !strcmp(obj, "addr"))
		return do_addr(argc, argv, i);
	if (cl_match(obj, "link"))
		return do_link(argc, argv, i);
	if (cl_match(obj, "route") || !strcmp(obj, "ro"))
		return do_route(argc, argv, i);
	if (cl_match(obj, "neighbour") || cl_match(obj, "neighbor"))
		return do_neigh(argc, argv, i);
	if (cl_match(obj, "dhcp"))
		return do_dhcp(argc, argv, i);
	if (cl_match(obj, "dns"))
		return do_dns(argc, argv, i);
	if (cl_match(obj, "help")) {
		usage();
		return CL_OK;
	}
	cl_printf("Object \"%s\" is unknown, try \"ip help\".\n", obj);
	return CL_ERROR;
}

CL_MAIN(ip_main, "ip")
