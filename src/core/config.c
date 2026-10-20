/*
 * Riptide - configuration file lines (generic keywords) and IPv4 address
 * parsing/formatting helpers.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * rt_config_line() is called with the stack lock held (it changes
 * interfaces, routes and the resolver configuration).
 */
#include "sock_internal.h"
#include "infra.h"

#define CFG_MAXARGS 24
#define CFG_MAXLINE 1024

int rt_parse_ipv4(const char *s, u32 *addr)
{
	u32 a = 0;
	int part, digits;

	if (!s || !addr)
		return -RT_EINVAL;
	for (part = 0; part < 4; part++) {
		u32 v = 0;
		for (digits = 0; *s >= '0' && *s <= '9'; s++, digits++) {
			v = v * 10 + (u32)(*s - '0');
			if (digits >= 3 || v > 255)
				return -RT_EINVAL;
		}
		if (digits == 0)
			return -RT_EINVAL;
		a = (a << 8) | v;
		if (part < 3 && *s++ != '.')
			return -RT_EINVAL;
	}
	if (*s)
		return -RT_EINVAL;
	*addr = rt_htonl(a);
	return 0;
}

char *rt_format_ipv4(u32 addr, char *buf)
{
	u32 h = rt_ntohl(addr);

	rt_snprintf(buf, 16, "%u.%u.%u.%u", (unsigned)(h >> 24), (unsigned)((h >> 16) & 255),
	            (unsigned)((h >> 8) & 255), (unsigned)(h & 255));
	return buf;
}

static int is_space(char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

int rt_config_split(char *line, char **argv, int max)
{
	char *r = line, *w;
	int argc = 0;

	if (!line || !argv || max <= 0)
		return 0;
	for (;;) {
		while (is_space(*r))
			r++;
		if (!*r || *r == '#' || *r == ';')
			break;
		if (argc >= max)
			break;
		argv[argc++] = w = r;
		/* a word: quotes group, backslash escapes inside quotes */
		while (*r && !is_space(*r)) {
			if (*r == '"') {
				r++;
				while (*r && *r != '"') {
					if (*r == '\\' && r[1])
						r++;
					*w++ = *r++;
				}
				if (*r == '"')
					r++;
			} else {
				*w++ = *r++;
			}
		}
		if (*r)
			r++;
		*w = 0;
	}
	return argc;
}

static u32 prefix_mask(int bits)
{
	return bits <= 0 ? 0 : rt_htonl(0xffffffffu << (32 - bits));
}

/* "A.B.C.D[/BITS]" -> addr, mask (/32 when no BITS; *has_bits tells). */
static int parse_prefix(char *s, u32 *addr, u32 *mask, int *has_bits)
{
	char *slash = strchr(s, '/');
	const char *end;
	unsigned long bits;

	*has_bits = 0;
	if (slash) {
		*slash = 0;
		bits = rt_strtoul(slash + 1, &end, 10);
		if (end == slash + 1 || *end || bits > 32)
			return -RT_EINVAL;
		*mask = prefix_mask((int)bits);
		*has_bits = 1;
	} else {
		*mask = 0xffffffffu;
	}
	return rt_parse_ipv4(s, addr);
}

static u32 classful(u32 addr)
{
	u32 h = rt_ntohl(addr);

	if (!(h & 0x80000000u))
		return prefix_mask(8);
	if ((h & 0xc0000000u) == 0x80000000u)
		return prefix_mask(16);
	return prefix_mask(24);
}

static int cfg_err(char *err, u32 errlen, int r, const char *fmt, const char *arg)
{
	if (err && errlen)
		rt_snprintf(err, errlen, fmt, arg ? arg : "");
	return r;
}

static int need_args(int argc, int n, char *err, u32 errlen, const char *usage)
{
	if (argc >= n)
		return 0;
	return cfg_err(err, errlen, -RT_EINVAL, "usage: %s", usage);
}

static int parse_uint(const char *s, u32 max, u32 *out)
{
	const char *end;
	unsigned long v = rt_strtoul(s, &end, 0);

	if (end == s || *end || v > max)
		return -RT_EINVAL;
	*out = (u32)v;
	return 0;
}

/* Remove the routes that "replace" supersedes: same destination and
 * metric, except interface routes. */
static void route_remove_same(u32 dst, u32 mask, u32 metric)
{
	struct rt_route *r;

	for (;;) {
		for (r = rt_route_list(); r; r = r->next)
			if (r->dst == (dst & mask) && r->mask == mask && r->metric == metric &&
			    !(r->flags & (RT_RTF_PMTU | RT_RTF_CONNECTED)))
				break;
		if (!r)
			return;
		if (rt_route_delete_match(r->dst, r->mask, r->gateway, r->netif,
		                          r->flags & (RT_RTF_DHCP | RT_RTF_DYNAMIC | RT_RTF_STATIC)) < 0)
			return;
	}
}

/* route flush [dev IF] | route flush cache: every non-interface route
 * (of IF) or the PMTU/redirect cache. */
static int route_flush(struct rt_netif *nif, int cache)
{
	struct rt_route *r;
	int n = 0;

	for (;;) {
		for (r = rt_route_list(); r; r = r->next) {
			if (nif && r->netif != nif)
				continue;
			if (cache ? (r->flags & (RT_RTF_PMTU | RT_RTF_DYNAMIC)) != 0
			          : !(r->flags & (RT_RTF_PMTU | RT_RTF_CONNECTED)))
				break;
		}
		if (!r)
			return n;
		if (r->flags & RT_RTF_PMTU) {
			/* PMTU entries are not reachable through delete_match */
			if (!r->netif)
				return n;
			rt_route_flush_dynamic_if(r->netif);
			n++;
			continue;
		}
		if (rt_route_delete_match(r->dst, r->mask, r->gateway, r->netif,
		                          r->flags & (RT_RTF_DHCP | RT_RTF_DYNAMIC | RT_RTF_STATIC)) < 0)
			return n;
		n++;
	}
}

/* route add|del|replace|change|append DEST[/BITS]|default [netmask M]
 *       [via|gw] GATEWAY [dev IF] [metric N] [mtu N] [reject]
 * route flush [dev IF] | route flush cache
 * The classic form "route add default 10.0.2.2" and the net-tools words
 * -net/-host are accepted too. */
static int cfg_route(int argc, char **argv, char *err, u32 errlen)
{
	static const char usage[] =
		"route add|del|replace|change DEST[/BITS]|default [netmask M] [via] GATEWAY "
		"[dev IF] [metric N] [mtu N] | route flush [dev IF|cache]";
	struct rt_netif *nif = NULL;
	struct rt_route *rt;
	u32 dst, mask, gw = 0, metric = 0, mtu = 0, flags = RT_RTF_UP | RT_RTF_STATIC;
	int op, bits, i = 2, r, have_metric = 0, host = 0;

	if (need_args(argc, 2, err, errlen, usage))
		return -RT_EINVAL;
	if (!strcmp(argv[1], "flush")) {
		if (argc >= 3 && !strcmp(argv[2], "cache"))
			return route_flush(NULL, 1) < 0 ? -RT_EINVAL : 0;
		if (argc >= 4 && !strcmp(argv[2], "dev")) {
			nif = rt_netif_find(argv[3]);
			if (!nif)
				return cfg_err(err, errlen, -RT_ENODEV, "route: no interface '%s'", argv[3]);
		}
		route_flush(nif, 0);
		return 0;
	}
	if (need_args(argc, 3, err, errlen, usage))
		return -RT_EINVAL;
	if (!strcmp(argv[1], "add") || !strcmp(argv[1], "append"))
		op = 'a';
	else if (!strcmp(argv[1], "del") || !strcmp(argv[1], "delete"))
		op = 'd';
	else if (!strcmp(argv[1], "replace"))
		op = 'r';
	else if (!strcmp(argv[1], "change"))
		op = 'c';
	else
		return cfg_err(err, errlen, -RT_EINVAL, "route: unknown command '%s'", argv[1]);
	if (!strcmp(argv[i], "-net") || !strcmp(argv[i], "-host")) {
		host = argv[i][1] == 'h';
		i++;
	}
	if (i >= argc)
		return cfg_err(err, errlen, -RT_EINVAL, "usage: %s", usage);
	if (!strcmp(argv[i], "default") || !strcmp(argv[i], "0/0")) {
		dst = 0;
		mask = 0;
	} else if (parse_prefix(argv[i], &dst, &mask, &bits) < 0) {
		return cfg_err(err, errlen, -RT_EINVAL, "route: bad destination '%s'", argv[i]);
	}
	if (host)
		mask = 0xffffffffu;
	for (i++; i < argc; i++) {
		const char *k = argv[i];
		if (!strcmp(k, "netmask") || !strcmp(k, "genmask")) {
			if (i + 1 >= argc || rt_parse_ipv4(argv[i + 1], &mask) < 0)
				return cfg_err(err, errlen, -RT_EINVAL, "route: bad netmask", NULL);
			i++;
		} else if (!strcmp(k, "via") || !strcmp(k, "gw") || !strcmp(k, "gateway")) {
			if (i + 1 >= argc || rt_parse_ipv4(argv[i + 1], &gw) < 0)
				return cfg_err(err, errlen, -RT_EINVAL, "route: bad gateway after '%s'", k);
			i++;
		} else if (!strcmp(k, "dev")) {
			if (i + 1 >= argc || !(nif = rt_netif_find(argv[i + 1])))
				return cfg_err(err, errlen, -RT_ENODEV, "route: no interface '%s'",
				               i + 1 < argc ? argv[i + 1] : "");
			i++;
		} else if (!strcmp(k, "metric") || !strcmp(k, "preference") || !strcmp(k, "priority")) {
			if (i + 1 >= argc || parse_uint(argv[i + 1], 0x7fffffff, &metric) < 0)
				return cfg_err(err, errlen, -RT_EINVAL, "route: bad metric", NULL);
			have_metric = 1;
			i++;
		} else if (!strcmp(k, "mtu")) {
			if (i + 1 >= argc || parse_uint(argv[i + 1], 65535, &mtu) < 0 || mtu < 68)
				return cfg_err(err, errlen, -RT_EINVAL, "route: bad mtu", NULL);
			i++;
		} else if (!strcmp(k, "reject") || !strcmp(k, "unreachable") || !strcmp(k, "blackhole") ||
		           !strcmp(k, "prohibit")) {
			flags |= RT_RTF_REJECT;
		} else if (!strcmp(k, "proto") || !strcmp(k, "scope") || !strcmp(k, "table") ||
		           !strcmp(k, "src")) {
			i++;            /* accepted and ignored (iproute2 words) */
		} else if (!gw && rt_parse_ipv4(k, &gw) == 0) {
			;               /* classic positional gateway */
		} else {
			return cfg_err(err, errlen, -RT_EINVAL, "route: unexpected '%s'", k);
		}
	}
	if (op == 'd') {
		/* the first matching route, like Linux */
		for (rt = rt_route_list(); rt; rt = rt->next)
			if (rt->dst == (dst & mask) && rt->mask == mask && !(rt->flags & RT_RTF_PMTU) &&
			    (!gw || rt->gateway == gw) && (!nif || rt->netif == nif) &&
			    (!have_metric || rt->metric == metric))
				break;
		if (!rt || rt_route_delete_match(rt->dst, rt->mask, rt->gateway, rt->netif,
		                                 rt->flags & (RT_RTF_DHCP | RT_RTF_DYNAMIC |
		                                              RT_RTF_STATIC | RT_RTF_CONNECTED)) < 0)
			return cfg_err(err, errlen, -RT_ESRCH, "route: no such route", NULL);
		return 0;
	}
	if (!gw && !nif && !(flags & RT_RTF_REJECT))
		return cfg_err(err, errlen, -RT_EINVAL, "route: gateway missing (%s)", usage);
	if (op == 'c' && !rt_route_find(dst, mask, 0))
		return cfg_err(err, errlen, -RT_ESRCH, "route: no such route", NULL);
	if (op == 'r' || op == 'c')
		route_remove_same(dst, mask, metric);
	r = rt_route_add(dst, mask, gw, nif, flags, metric);
	if (r == -RT_ENETUNREACH) {
		char a[16];
		return cfg_err(err, errlen, r, "route: gateway %s is not on a connected network",
		               rt_format_ipv4(gw, a));
	}
	if (r == -RT_EEXIST)
		return cfg_err(err, errlen, r, "route: route exists", NULL);
	if (r < 0)
		return cfg_err(err, errlen, r, "route: %s", rt_strerror(-r));
	if (mtu) {
		for (rt = rt_route_list(); rt; rt = rt->next)
			if (rt->dst == (dst & mask) && rt->mask == mask && rt->gateway == gw &&
			    !(rt->flags & RT_RTF_PMTU) && rt->metric == metric)
				rt->mtu = mtu;
	}
	return 0;
}

static void if_set_flag(struct rt_netif *nif, u16 bit, int on)
{
	u16 nf = on ? (u16)(nif->flags | bit) : (u16)(nif->flags & ~bit);

	if (nf != nif->flags && nif->ioctl_flags)
		nif->ioctl_flags(nif, nf);
	nif->flags = nf;
}

static int cfg_ifconfig(int argc, char **argv, char *err, u32 errlen)
{
	struct rt_netif *nif;
	u32 addr = 0, mask = 0, peer = 0, bcast = 0;
	int bits = 0, i, r, up = -1, have_addr = 0, mtu = 0, have_mask = 0, del = 0;
	int promisc = -1, arp = -1;
	long metric = -1;
	const char *end;

	if (need_args(argc, 2, err, errlen,
	              "ifconfig IFNAME [A.B.C.D[/BITS]] [netmask M] [broadcast B] [peer A.B.C.D] "
	              "[mtu N] [metric N] [[-]promisc] [[-]arp] [delete] [up|down]"))
		return -RT_EINVAL;
	nif = rt_netif_find(argv[1]);
	if (!nif)
		return cfg_err(err, errlen, -RT_ENXIO, "ifconfig: no interface '%s'", argv[1]);
	for (i = 2; i < argc; i++) {
		const char *k = argv[i];
		if (!strcmp(k, "up")) {
			up = 1;
		} else if (!strcmp(k, "down")) {
			up = 0;
		} else if (!strcmp(k, "inet")) {
			continue;
		} else if (!strcmp(k, "promisc") || !strcmp(k, "-promisc")) {
			promisc = k[0] != '-';
		} else if (!strcmp(k, "arp") || !strcmp(k, "-arp")) {
			arp = k[0] != '-';
		} else if (!strcmp(k, "delete") || !strcmp(k, "del") || !strcmp(k, "-alias")) {
			del = 1;
		} else if (!strcmp(k, "metric")) {
			u32 v;
			if (++i >= argc || parse_uint(argv[i], 0x7fffffff, &v) < 0)
				return cfg_err(err, errlen, -RT_EINVAL, "ifconfig: bad metric", NULL);
			metric = (long)v;
		} else if (!strcmp(k, "peer") || !strcmp(k, "dest") || !strcmp(k, "pointopoint") ||
		           !strcmp(k, "broadcast") || !strcmp(k, "netmask")) {
			u32 *dst = !strcmp(k, "netmask") ? &mask : !strcmp(k, "broadcast") ? &bcast : &peer;
			if (++i >= argc || rt_parse_ipv4(argv[i], dst) < 0)
				return cfg_err(err, errlen, -RT_EINVAL, "ifconfig: bad address after '%s'", k);
			if (dst == &mask)
				have_mask = 1;
		} else if (!strcmp(k, "mtu")) {
			unsigned long v;
			if (++i >= argc)
				return cfg_err(err, errlen, -RT_EINVAL, "ifconfig: mtu needs a value", NULL);
			v = rt_strtoul(argv[i], &end, 10);
			if (*end || end == argv[i] || v < 68 || v > 65535)
				return cfg_err(err, errlen, -RT_EINVAL, "ifconfig: bad mtu '%s'", argv[i]);
			mtu = (int)v;
		} else if (!have_addr) {
			u32 m;
			if (parse_prefix(argv[i], &addr, &m, &bits) < 0)
				return cfg_err(err, errlen, -RT_EINVAL, "ifconfig: bad address '%s'", argv[i]);
			if (bits) {
				mask = m;
				have_mask = 1;
			}
			have_addr = 1;
		} else {
			return cfg_err(err, errlen, -RT_EINVAL, "ifconfig: unexpected '%s'", argv[i]);
		}
	}
	if (mtu)
		nif->mtu = (u16)mtu;
	if (metric >= 0)
		nif->metric = (u32)metric;
	if (promisc >= 0)
		if_set_flag(nif, RT_IFF_PROMISC, promisc);
	if (arp >= 0)
		if_set_flag(nif, RT_IFF_NOARP, !arp);
	if (del) {
		/* "ifconfig IF [A.B.C.D] delete": remove the address */
		if (have_addr && addr != nif->addr)
			return cfg_err(err, errlen, -RT_EADDRNOTAVAIL, "ifconfig: %s has no such address",
			               argv[1]);
		rt_netif_set_addr(nif, 0, 0, 0);
		have_addr = 0;
		peer = 0;
		have_mask = 0;
		bcast = 0;
	}
	if (have_addr) {
		if (!have_mask)
			mask = nif->type == RT_IFT_PTP && peer ? 0xffffffffu : classful(addr);
		r = rt_netif_set_addr(nif, addr, mask, nif->type == RT_IFT_PTP ? peer : bcast);
		if (r < 0)
			return cfg_err(err, errlen, r, "ifconfig: cannot set address: %s", rt_strerror(-r));
		if (up < 0)
			up = 1;
	} else if (peer || have_mask) {
		if (!nif->addr)
			return cfg_err(err, errlen, -RT_EINVAL, "ifconfig: %s has no address", argv[1]);
		r = rt_netif_set_addr(nif, nif->addr, have_mask ? mask : nif->netmask,
		                      peer ? peer : nif->type == RT_IFT_PTP ? nif->dstaddr : bcast);
		if (r < 0)
			return cfg_err(err, errlen, r, "ifconfig: %s", rt_strerror(-r));
	} else if (bcast) {
		if (!nif->addr || !(nif->flags & RT_IFF_BROADCAST))
			return cfg_err(err, errlen, -RT_EINVAL, "ifconfig: %s has no broadcast address",
			               argv[1]);
		nif->broadaddr = bcast;
	}
	if (up == 1)
		rt_netif_up(nif);
	else if (up == 0)
		rt_netif_down(nif);
	return 0;
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* "52:55:0a:00:02:02" (also '-' separated, single digit groups) */
static int parse_mac(const char *s, u8 *mac)
{
	int i, h, l;

	for (i = 0; i < 6; i++) {
		if ((h = hexval(*s)) < 0)
			return -RT_EINVAL;
		s++;
		if ((l = hexval(*s)) >= 0) {
			h = h * 16 + l;
			s++;
		}
		mac[i] = (u8)h;
		if (i < 5 && *s != ':' && *s != '-')
			return -RT_EINVAL;
		if (i < 5)
			s++;
	}
	return *s ? -RT_EINVAL : 0;
}

/* arp add A.B.C.D MAC [dev IF] | arp del A.B.C.D [dev IF] | arp flush [dev IF] */
static int cfg_arp(int argc, char **argv, char *err, u32 errlen)
{
	static const char usage[] = "arp add A.B.C.D MAC [dev IF] | arp del A.B.C.D [dev IF] | arp flush [dev IF]";
	struct rt_netif *nif = NULL;
	struct rt_nexthop nh;
	u32 ip = 0;
	u8 mac[6];
	int i, r, have_mac = 0;

	if (need_args(argc, 2, err, errlen, usage))
		return -RT_EINVAL;
	for (i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "dev")) {
			if (i + 1 >= argc || !(nif = rt_netif_find(argv[i + 1])))
				return cfg_err(err, errlen, -RT_ENODEV, "arp: no interface '%s'",
				               i + 1 < argc ? argv[i + 1] : "");
			i++;
		} else if (!strcmp(argv[i], "lladdr") || !strcmp(argv[i], "nud")) {
			if (argv[i][0] == 'n') {
				i++;
				continue;
			}
			if (i + 1 >= argc || parse_mac(argv[i + 1], mac) < 0)
				return cfg_err(err, errlen, -RT_EINVAL, "arp: bad hardware address", NULL);
			have_mac = 1;
			i++;
		} else if (!strcmp(argv[i], "permanent") || !strcmp(argv[i], "temp") ||
		           !strcmp(argv[i], "pub")) {
			if (argv[i][0] == 'p' && argv[i][1] == 'u')
				return cfg_err(err, errlen, -RT_EOPNOTSUPP, "arp: proxy ARP is not supported", NULL);
		} else if (!ip && rt_parse_ipv4(argv[i], &ip) == 0) {
			;
		} else if (!have_mac && parse_mac(argv[i], mac) == 0) {
			have_mac = 1;
		} else {
			return cfg_err(err, errlen, -RT_EINVAL, "arp: unexpected '%s'", argv[i]);
		}
	}
	if (!strcmp(argv[1], "flush")) {
		struct rt_netif *n;
		for (n = rt_netif_list(); n; n = n->next)
			if (!nif || n == nif)
				rt_arp_flush(n, 0);
		return 0;
	}
	if (!ip)
		return cfg_err(err, errlen, -RT_EINVAL, "usage: %s", usage);
	if (!strcmp(argv[1], "del") || !strcmp(argv[1], "delete")) {
		struct rt_arp_entry_info ai;
		if (rt_arp_get(nif, ip, &ai) < 0)
			return cfg_err(err, errlen, -RT_ENXIO, "arp: no entry for %s", argv[2]);
		rt_arp_delete(ip);
		return 0;
	}
	if (strcmp(argv[1], "add") && strcmp(argv[1], "replace") && strcmp(argv[1], "set"))
		return cfg_err(err, errlen, -RT_EINVAL, "arp: unknown command '%s'", argv[1]);
	if (!have_mac)
		return cfg_err(err, errlen, -RT_EINVAL, "arp: hardware address missing", NULL);
	if (!nif) {
		if (rt_ip_route(ip, &nh) < 0 || (nh.flags & RT_RTF_GATEWAY))
			return cfg_err(err, errlen, -RT_ENETUNREACH, "arp: %s is not on a connected network",
			               argv[2]);
		nif = nh.netif;
	}
	if (nif->type != RT_IFT_ETHER)
		return cfg_err(err, errlen, -RT_EINVAL, "arp: %s does not use ARP", nif->name);
	r = rt_arp_add_static(nif, ip, mac);
	if (r < 0)
		return cfg_err(err, errlen, r, "arp: %s", rt_strerror(-r));
	return 0;
}

static int cfg_dispatch(int argc, char **argv, char *err, u32 errlen,
                        int (*unknown)(int, char **, char *, u32))
{
	const char *k = argv[0];
	u32 a;
	int r;

	if (!strcmp(k, "hostname")) {
		if (need_args(argc, 2, err, errlen, "hostname NAME"))
			return -RT_EINVAL;
		if (strlen(argv[1]) >= sizeof(rt_hostname))
			return cfg_err(err, errlen, -RT_ENAMETOOLONG, "hostname: '%s' is too long", argv[1]);
		rt_strlcpy(rt_hostname, argv[1], sizeof(rt_hostname));
		return 0;
	}
	if (!strcmp(k, "domain")) {
		if (need_args(argc, 2, err, errlen, "domain NAME"))
			return -RT_EINVAL;
		if (strlen(argv[1]) >= sizeof(rt_domainname))
			return cfg_err(err, errlen, -RT_ENAMETOOLONG, "domain: '%s' is too long", argv[1]);
		rt_strlcpy(rt_domainname, argv[1], sizeof(rt_domainname));
		rt_dns_set_search(argv[1]);
		return 0;
	}
	if (!strcmp(k, "nameserver")) {
		/* nameserver [add] A.B.C.D | nameserver del A.B.C.D | nameserver flush */
		static const char usage[] = "nameserver [add|del] A.B.C.D | nameserver flush";
		int i = 1, del = 0;
		if (need_args(argc, 2, err, errlen, usage))
			return -RT_EINVAL;
		if (!strcmp(argv[1], "flush")) {
			rt_dns_clear_servers();
			return 0;
		}
		if (!strcmp(argv[1], "add") || !strcmp(argv[1], "del") || !strcmp(argv[1], "delete")) {
			del = argv[1][0] == 'd';
			i = 2;
		}
		if (need_args(argc, i + 1, err, errlen, usage))
			return -RT_EINVAL;
		if (rt_parse_ipv4(argv[i], &a) < 0 || a == 0)
			return cfg_err(err, errlen, -RT_EINVAL, "nameserver: bad address '%s'", argv[i]);
		if (del) {
			u32 sv[RT_CTL_MAXNS];
			int n = rt_dns_get_servers(sv, RT_CTL_MAXNS), j;
			for (j = 0; j < n && j < RT_CTL_MAXNS; j++)
				if (sv[j] == a)
					break;
			if (j >= n || j >= RT_CTL_MAXNS)
				return cfg_err(err, errlen, -RT_ESRCH, "nameserver: %s is not configured", argv[i]);
			rt_dns_remove_server(a);
			return 0;
		}
		r = rt_dns_add_server(a);
		if (r < 0)
			return cfg_err(err, errlen, r, "nameserver: %s", rt_strerror(-r));
		return 0;
	}
	if (!strcmp(k, "arp"))
		return cfg_arp(argc, argv, err, errlen);
	if (!strcmp(k, "host")) {
		if (need_args(argc, 3, err, errlen, "host NAME A.B.C.D"))
			return -RT_EINVAL;
		if (rt_parse_ipv4(argv[2], &a) < 0)
			return cfg_err(err, errlen, -RT_EINVAL, "host: bad address '%s'", argv[2]);
		r = rt_hosts_add(argv[1], a);
		if (r < 0)
			return cfg_err(err, errlen, r, "host: %s", rt_strerror(-r));
		return 0;
	}
	if (!strcmp(k, "route"))
		return cfg_route(argc, argv, err, errlen);
	if (!strcmp(k, "sysctl")) {
		if (need_args(argc, 2, err, errlen, "sysctl NAME=VALUE"))
			return -RT_EINVAL;
		r = argc >= 3 ? rt_sysctl_set_text(argv[1], argv[2]) : rt_sysctl_set_text(argv[1], NULL);
		if (r == -RT_ENOENT)
			return cfg_err(err, errlen, r, "sysctl: unknown variable '%s'", argv[1]);
		if (r == -RT_EPERM)
			return cfg_err(err, errlen, r, "sysctl: '%s' is read only", argv[1]);
		if (r < 0)
			return cfg_err(err, errlen, r, "sysctl: bad value for '%s'", argv[1]);
		return 0;
	}
	if (!strcmp(k, "ifconfig"))
		return cfg_ifconfig(argc, argv, err, errlen);
	if (!strcmp(k, "dhcp")) {
		/* dhcp IFNAME [start|stop|release|renew] */
		struct rt_netif *nif;
		const char *op = argc >= 3 ? argv[2] : "start";
		if (need_args(argc, 2, err, errlen, "dhcp IFNAME [start|stop|renew]"))
			return -RT_EINVAL;
		nif = rt_netif_find(argv[1]);
		if (!nif)
			return cfg_err(err, errlen, -RT_ENXIO, "dhcp: no interface '%s'", argv[1]);
		if (!strcmp(op, "start")) {
			r = rt_dhcp_start(nif, NULL);
		} else if (!strcmp(op, "stop") || !strcmp(op, "release")) {
			struct rt_dhcp_info di;
			if (rt_dhcp_get_info(nif, &di) == RT_DHCPS_NONE)
				return cfg_err(err, errlen, -RT_ESRCH, "dhcp: not running on %s", argv[1]);
			rt_dhcp_stop(nif);
			r = 0;
		} else if (!strcmp(op, "renew")) {
			r = rt_dhcp_renew(nif);
			if (r == -RT_ENOENT)
				return cfg_err(err, errlen, -RT_ESRCH, "dhcp: not running on %s", argv[1]);
		} else {
			return cfg_err(err, errlen, -RT_EINVAL, "dhcp: unknown command '%s'", op);
		}
		if (r < 0)
			return cfg_err(err, errlen, r, "dhcp: %s", rt_strerror(-r));
		return 0;
	}
	if (unknown)
		return unknown(argc, argv, err, errlen);
	return cfg_err(err, errlen, -RT_EINVAL, "unknown keyword '%s'", k);
}

int rt_config_line(const char *line, char *err, u32 errlen,
                   int (*unknown)(int argc, char **argv, char *err, u32 errlen))
{
	char *argv[CFG_MAXARGS];
	char *copy;
	u32 len;
	int argc, r;

	if (err && errlen)
		err[0] = 0;
	if (!line)
		return 0;
	len = (u32)strlen(line);
	if (len > CFG_MAXLINE)
		return cfg_err(err, errlen, -RT_E2BIG, "line too long", NULL);
	copy = rt_malloc(len + 1);
	if (!copy)
		return cfg_err(err, errlen, -RT_ENOMEM, "out of memory", NULL);
	memcpy(copy, line, len + 1);
	argc = rt_config_split(copy, argv, CFG_MAXARGS);
	r = argc ? cfg_dispatch(argc, argv, err, errlen, unknown) : 0;
	rt_free(copy);
	return r;
}
