/*
 * Riptide - sysctl registry: every module registers static tables of
 * tunables, addressable by dotted name and (optionally) numeric MIB.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Call with the stack lock held (handlers may look at stack state).
 *
 * Numeric MIBs follow 4.4BSD / Miami 2.x (MiamiSysCtl()) where the variable
 * existed there. Top level: CTL_KERN 1, CTL_NET 4, Miami CTL_DNS 9,
 * CTL_INETD 10, CTL_SOCKET 11. Under net: PF_INET 2, then the protocol
 * number (ip 0, icmp 1, tcp 6, udp 17).
 *
 *   MIB              name                        owner
 *   1.10             kern.hostname               sysctl.c
 *   1.22             kern.domainname             sysctl.c
 *   4.2.0.1          net.inet.ip.forwarding      ip.c    (IPCTL_FORWARDING)
 *   4.2.0.2          net.inet.ip.redirect        ip.c    (IPCTL_SENDREDIRECTS)
 *   4.2.0.3          net.inet.ip.ttl             ip.c    (IPCTL_DEFTTL)
 *   4.2.1.1          net.inet.icmp.maskrepl      icmp.c  (ICMPCTL_MASKREPL)
 *   4.2.6.1          net.inet.tcp.rfc1323        tcp     (TCPCTL_DO_RFC1323)
 *   4.2.6.3          net.inet.tcp.mssdflt        tcp     (TCPCTL_MSSDFLT)
 *   4.2.6.6          net.inet.tcp.keepidle       tcp     (TCPCTL_KEEPIDLE)
 *   4.2.6.7          net.inet.tcp.keepintvl      tcp     (TCPCTL_KEEPINTVL)
 *   4.2.6.8          net.inet.tcp.sendspace      socket.c(TCPCTL_SENDSPACE)
 *   4.2.6.9          net.inet.tcp.recvspace      socket.c(TCPCTL_RECVSPACE)
 *   4.2.17.1         net.inet.udp.checksum       udp.c   (UDPCTL_CHECKSUM)
 *   4.2.17.3         net.inet.udp.maxdgram       udp.c   (UDPCTL_MAXDGRAM)
 *   4.2.17.4         net.inet.udp.recvspace      udp.c   (UDPCTL_RECVSPACE)
 *   9.0.0            dns.cache.size              resolv.c (Miami)
 *   9.0.1            dns.cache.flush             resolv.c (Miami)
 *   9.0.2            dns.cache.enabled           resolv.c (Miami)
 *   11.0             socket.maxqlen              socket.c (Miami)
 *
 * The statistics (net.inet.{ip,icmp,tcp,udp}.stats) are exported by name
 * only: BSD's TCPCTL_STATS (4.2.6.4) and UDPCTL_STATS (4.2.17.2) return
 * struct tcpstat/udpstat, whose layout differs from rt_*_stats, so
 * answering those MIBs would hand old tools garbage.
 */
#include "sock_internal.h"

#define SYSCTL_MAX_TABLES 64

struct sysctl_tab {
	const struct rt_sysctl *tab;
	int n;
};
static struct sysctl_tab sysctl_tabs[SYSCTL_MAX_TABLES];
static int sysctl_ntabs;

static const struct rt_sysctl *find_n(const char *name, u32 len)
{
	int t, i;

	for (t = 0; t < sysctl_ntabs; t++) {
		const struct rt_sysctl *e = sysctl_tabs[t].tab;
		for (i = 0; i < sysctl_tabs[t].n; i++, e++)
			if (strncmp(e->name, name, len) == 0 && e->name[len] == 0)
				return e;
	}
	return NULL;
}

const struct rt_sysctl *rt_sysctl_find(const char *name)
{
	if (!name)
		return NULL;
	return find_n(name, (u32)strlen(name));
}

void rt_sysctl_register(const struct rt_sysctl *tab, int n)
{
	int t, i;

	if (!tab || n <= 0)
		return;
	for (t = 0; t < sysctl_ntabs; t++)
		if (sysctl_tabs[t].tab == tab)
			return;            /* registered twice: ignore */
	if (sysctl_ntabs >= SYSCTL_MAX_TABLES) {
		rt_log(RT_LOG_ERR, "sysctl: too many tables, %s dropped", tab[0].name);
		return;
	}
	for (i = 0; i < n; i++)
		if (rt_sysctl_find(tab[i].name))
			rt_log(RT_LOG_WARN, "sysctl: duplicate %s (first one wins)", tab[i].name);
	sysctl_tabs[sysctl_ntabs].tab = tab;
	sysctl_tabs[sysctl_ntabs].n = n;
	sysctl_ntabs++;
}

const struct rt_sysctl *rt_sysctl_iter(int i)
{
	int t;

	if (i < 0)
		return NULL;
	for (t = 0; t < sysctl_ntabs; t++) {
		if (i < sysctl_tabs[t].n)
			return &sysctl_tabs[t].tab[i];
		i -= sysctl_tabs[t].n;
	}
	return NULL;
}

static int sysctl_do(const struct rt_sysctl *e, void *old, u32 *oldlen,
                     const void *newp, u32 newlen)
{
	u32 size;
	s32 v;

	if (newp && (e->flags & RT_SYSCTL_RO))
		return -RT_EPERM;
	if (e->handler)
		return e->handler(e, old, oldlen, newp, newlen);
	if (!e->ptr)
		return -RT_ENOENT;
	switch (e->type) {
	case RT_SYSCTL_INT:
		if (oldlen) {
			if (old) {
				if (*oldlen < sizeof(s32))
					return -RT_ENOMEM;
				memcpy(old, e->ptr, sizeof(s32));
			}
			*oldlen = sizeof(s32);
		}
		if (newp) {
			if (newlen != sizeof(s32))
				return -RT_EINVAL;
			memcpy(&v, newp, sizeof(v));
			if (v < e->min || v > e->max)
				return -RT_EINVAL;
			memcpy(e->ptr, &v, sizeof(v));
		}
		return 0;
	case RT_SYSCTL_STRING: {
		char *s = e->ptr;
		const char *ns = newp;
		if (oldlen) {
			size = (u32)strlen(s) + 1;
			if (old) {
				if (*oldlen < size) {
					if (*oldlen) {
						memcpy(old, s, *oldlen - 1);
						((char *)old)[*oldlen - 1] = 0;
					}
					return -RT_ENOMEM;
				}
				memcpy(old, s, size);
			}
			*oldlen = size;
		}
		if (newp) {
			while (newlen && ns[newlen - 1] == 0)
				newlen--;
			if (e->max <= 0 || newlen >= (u32)e->max)
				return -RT_EINVAL;
			for (size = 0; size < newlen; size++)
				if (ns[size] == 0)
					return -RT_EINVAL;
			memcpy(s, ns, newlen);
			s[newlen] = 0;
		}
		return 0;
	}
	case RT_SYSCTL_OPAQUE:
		if (newp)
			return -RT_EPERM;
		size = (u32)(e->max > 0 ? e->max : 0);
		if (oldlen) {
			if (old) {
				if (*oldlen < size)
					return -RT_ENOMEM;
				memcpy(old, e->ptr, size);
			}
			*oldlen = size;
		}
		return 0;
	}
	return -RT_EINVAL;
}

int rt_sysctl_byname(const char *name, void *old, u32 *oldlen, const void *newp, u32 newlen)
{
	const struct rt_sysctl *e = rt_sysctl_find(name);

	if (!e)
		return -RT_ENOENT;
	return sysctl_do(e, old, oldlen, newp, newlen);
}

int rt_sysctl_bymib(const s32 *mib, u32 miblen, void *old, u32 *oldlen,
                    const void *newp, u32 newlen)
{
	int t, i;
	u32 k;

	if (!mib || miblen == 0 || miblen > RT_SYSCTL_MAXMIB)
		return -RT_ENOENT;
	for (t = 0; t < sysctl_ntabs; t++) {
		const struct rt_sysctl *e = sysctl_tabs[t].tab;
		for (i = 0; i < sysctl_tabs[t].n; i++, e++) {
			if (e->miblen != miblen)
				continue;
			for (k = 0; k < miblen; k++)
				if (e->mib[k] != mib[k])
					break;
			if (k == miblen)
				return sysctl_do(e, old, oldlen, newp, newlen);
		}
	}
	return -RT_ENOENT;
}

static int parse_int(const char *s, s32 *out)
{
	static const char *const words[] = { "off", "on", "no", "yes", "false", "true" };
	const char *end;
	unsigned long v;
	unsigned i;
	int neg = 0;

	while (*s == ' ' || *s == '\t')
		s++;
	for (i = 0; i < RT_ARRAY_SIZE(words); i++) {
		if (rt_strcasecmp(s, words[i]) == 0) {
			*out = (s32)(i & 1);
			return 0;
		}
	}
	if (*s == '-' || *s == '+')
		neg = *s++ == '-';
	if (*s < '0' || *s > '9')
		return -RT_EINVAL;
	v = rt_strtoul(s, &end, 0);
	while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')
		end++;
	if (*end || v > (neg ? 0x80000000ul : 0x7ffffffful))
		return -RT_EINVAL;
	*out = neg ? (s32)(0 - (u32)v) : (s32)v;
	return 0;
}

int rt_sysctl_set_text(const char *name, const char *value)
{
	const struct rt_sysctl *e;
	u32 nlen;
	s32 v;
	int r;

	if (!name)
		return -RT_EINVAL;
	if (value) {
		nlen = (u32)strlen(name);
	} else {
		for (nlen = 0; name[nlen] && name[nlen] != '=' && name[nlen] != ' ' &&
		     name[nlen] != '\t'; nlen++)
			;
		if (!name[nlen])
			return -RT_EINVAL;
		value = name + nlen + 1;
		while (*value == ' ' || *value == '\t' || *value == '=')
			value++;
	}
	e = find_n(name, nlen);
	if (!e)
		return -RT_ENOENT;
	switch (e->type) {
	case RT_SYSCTL_INT:
		r = parse_int(value, &v);
		if (r < 0)
			return r;
		return sysctl_do(e, NULL, NULL, &v, sizeof(v));
	case RT_SYSCTL_STRING:
		return sysctl_do(e, NULL, NULL, value, (u32)strlen(value));
	}
	return -RT_EPERM;
}

int rt_sysctl_get_text(const struct rt_sysctl *e, char *buf, u32 len)
{
	u32 l;
	s32 v;
	int r;

	if (!e || !buf || len == 0)
		return -RT_EINVAL;
	buf[0] = 0;
	switch (e->type) {
	case RT_SYSCTL_INT:
		l = sizeof(v);
		r = sysctl_do(e, &v, &l, NULL, 0);
		if (r < 0)
			return r;
		return rt_snprintf(buf, len, "%d", (int)v);
	case RT_SYSCTL_STRING:
		l = len;
		r = sysctl_do(e, buf, &l, NULL, 0);
		if (r < 0 && r != -RT_ENOMEM)
			return r;
		return (int)strlen(buf);
	case RT_SYSCTL_OPAQUE:
		l = 0;
		r = sysctl_do(e, NULL, &l, NULL, 0);
		if (r < 0)
			return r;
		return rt_snprintf(buf, len, "<opaque, %u bytes>", (unsigned)l);
	}
	return -RT_EINVAL;
}

/* ---- generic entries ---- */

char rt_hostname[64];
char rt_domainname[64];

static const struct rt_sysctl generic_sysctls[] = {
	{ "kern.hostname", RT_SYSCTL_STRING, RT_SYSCTL_RW, 2, { 1, 10 },
	  rt_hostname, 0, sizeof(rt_hostname), NULL, "host name" },
	{ "kern.domainname", RT_SYSCTL_STRING, RT_SYSCTL_RW, 2, { 1, 22 },
	  rt_domainname, 0, sizeof(rt_domainname), NULL, "domain name" },
	{ "net.inet.ip.stats", RT_SYSCTL_OPAQUE, RT_SYSCTL_RO, 0, { 0 },
	  &rt_ipstat, 0, sizeof(rt_ipstat), NULL, "struct rt_ip_stats" },
	{ "net.inet.icmp.stats", RT_SYSCTL_OPAQUE, RT_SYSCTL_RO, 0, { 0 },
	  &rt_icmpstat, 0, sizeof(rt_icmpstat), NULL, "struct rt_icmp_stats" },
	{ "net.inet.tcp.stats", RT_SYSCTL_OPAQUE, RT_SYSCTL_RO, 0, { 0 },
	  &rt_tcpstat, 0, sizeof(rt_tcpstat), NULL, "struct rt_tcp_stats" },
	{ "net.inet.udp.stats", RT_SYSCTL_OPAQUE, RT_SYSCTL_RO, 0, { 0 },
	  &rt_udpstat, 0, sizeof(rt_udpstat), NULL, "struct rt_udp_stats" },
};

void rt_sysctl_init(void)
{
	rt_sysctl_register(generic_sysctls, (int)RT_ARRAY_SIZE(generic_sysctls));
}
