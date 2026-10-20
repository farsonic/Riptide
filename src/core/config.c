/*
 * Riptide - configuration file lines (generic keywords) and IPv4 address
 * parsing/formatting helpers.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * rt_config_line() is called with the stack lock held (it changes
 * interfaces, routes and the resolver configuration).
 */
#include "sock_internal.h"

#define CFG_MAXARGS 16
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

static int cfg_route(int argc, char **argv, char *err, u32 errlen)
{
	static const char usage[] = "route add|del DEST[/BITS]|default GATEWAY";
	u32 dst, mask, gw = 0;
	int add, bits, i = 2, r;

	if (need_args(argc, 3, err, errlen, usage))
		return -RT_EINVAL;
	if (!strcmp(argv[1], "add"))
		add = 1;
	else if (!strcmp(argv[1], "del") || !strcmp(argv[1], "delete"))
		add = 0;
	else
		return cfg_err(err, errlen, -RT_EINVAL, "route: unknown command '%s'", argv[1]);
	if (!strcmp(argv[i], "-net") || !strcmp(argv[i], "-host"))
		i++;
	if (i >= argc)
		return cfg_err(err, errlen, -RT_EINVAL, "usage: %s", usage);
	if (!strcmp(argv[i], "default")) {
		dst = 0;
		mask = 0;
	} else if (parse_prefix(argv[i], &dst, &mask, &bits) < 0) {
		return cfg_err(err, errlen, -RT_EINVAL, "route: bad destination '%s'", argv[i]);
	}
	i++;
	if (i < argc && !strcmp(argv[i], "netmask")) {
		if (i + 1 >= argc || rt_parse_ipv4(argv[i + 1], &mask) < 0)
			return cfg_err(err, errlen, -RT_EINVAL, "route: bad netmask", NULL);
		i += 2;
	}
	if (i < argc) {
		if (rt_parse_ipv4(argv[i], &gw) < 0)
			return cfg_err(err, errlen, -RT_EINVAL, "route: bad gateway '%s'", argv[i]);
		i++;
	} else if (add) {
		return cfg_err(err, errlen, -RT_EINVAL, "route: gateway missing (%s)", usage);
	}
	if (add) {
		r = rt_route_add(dst, mask, gw, NULL, RT_RTF_UP | RT_RTF_STATIC, 0);
		if (r == -RT_ENETUNREACH)
			return cfg_err(err, errlen, r, "route: gateway %s is not on a connected network",
			               argv[i - 1]);
		if (r == -RT_EEXIST)
			return cfg_err(err, errlen, r, "route: route exists", NULL);
	} else {
		r = rt_route_delete(dst, mask, gw);
		if (r < 0)
			return cfg_err(err, errlen, r, "route: no such route", NULL);
	}
	if (r < 0)
		return cfg_err(err, errlen, r, "route: %s", rt_strerror(-r));
	return 0;
}

static int cfg_ifconfig(int argc, char **argv, char *err, u32 errlen)
{
	struct rt_netif *nif;
	u32 addr = 0, mask = 0, peer = 0, bcast = 0;
	int bits = 0, i, r, up = -1, have_addr = 0, mtu = 0, have_mask = 0;
	const char *end;

	if (need_args(argc, 2, err, errlen,
	              "ifconfig IFNAME [A.B.C.D[/BITS]] [netmask M] [peer A.B.C.D] [mtu N] [up|down]"))
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
		                      peer ? peer : nif->dstaddr);
		if (r < 0)
			return cfg_err(err, errlen, r, "ifconfig: %s", rt_strerror(-r));
	}
	if (up == 1)
		rt_netif_up(nif);
	else if (up == 0)
		rt_netif_down(nif);
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
		if (need_args(argc, 2, err, errlen, "nameserver A.B.C.D"))
			return -RT_EINVAL;
		if (rt_parse_ipv4(argv[1], &a) < 0 || a == 0)
			return cfg_err(err, errlen, -RT_EINVAL, "nameserver: bad address '%s'", argv[1]);
		r = rt_dns_add_server(a);
		if (r < 0)
			return cfg_err(err, errlen, r, "nameserver: %s", rt_strerror(-r));
		return 0;
	}
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
		struct rt_netif *nif;
		if (need_args(argc, 2, err, errlen, "dhcp IFNAME"))
			return -RT_EINVAL;
		nif = rt_netif_find(argv[1]);
		if (!nif)
			return cfg_err(err, errlen, -RT_ENXIO, "dhcp: no interface '%s'", argv[1]);
		r = rt_dhcp_start(nif, NULL);
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
