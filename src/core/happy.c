/*
 * Riptide - Happy Eyeballs version 2 (RFC 8305): connect to a host name
 * over whichever of IPv6 and IPv4 works first.
 *
 * rt_he_connect() resolves A and AAAA in parallel (rt_resolve_name46()),
 * orders the addresses by RFC 6724 (rt_inet_sort_dest()) and interleaves
 * the families (RFC 8305 4: one address of the first family, then
 * alternating), then starts a non-blocking connect to the first one and
 * one more every rt_he_attempt_delay ms (250) or as soon as an attempt
 * fails, until one connects; the others are closed. A broken IPv6 path
 * (a blackholed address, a dead tunnel) costs 250 ms, not a connect
 * timeout.
 *
 * Legacy programs (gethostbyname() is IPv4 only) never see IPv6; for
 * getaddrinfo() users rt_he_aaaa_filter keeps AAAA out of AF_UNSPEC
 * lookups while the stack has no usable IPv6 route (see rt_he_v6_usable).
 *
 * Runs in the caller's task without the stack lock (like the resolver).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "infra.h"
#include "sock_internal.h"

s32 rt_he_enable = 1;            /* net.inet.he.enable */
s32 rt_he_attempt_delay = 250;   /* net.inet.he.attempt_delay (ms) */
extern s32 rt_he_resolution_delay; /* resolv.c: net.inet.he.resolution_delay */
s32 rt_he_prefer = 0;            /* net.inet.he.prefer: 0 RFC 6724, 4 IPv4, 6 IPv6 first */
s32 rt_he_aaaa_filter = 1;       /* net.inet.he.aaaa_filter */

#define HE_MAXADDR 16

static const struct rt_sysctl he_sysctl[] = {
	{ "net.inet.he.enable", RT_SYSCTL_INT, RT_SYSCTL_RW, 0, { 0 },
	  &rt_he_enable, 0, 1, NULL, "Happy Eyeballs: race IPv6 and IPv4 connects, parallel A/AAAA lookups" },
	{ "net.inet.he.attempt_delay", RT_SYSCTL_INT, RT_SYSCTL_RW, 0, { 0 },
	  &rt_he_attempt_delay, 10, 2000, NULL, "Happy Eyeballs: ms before the next connection attempt (RFC 8305: 250)" },
	{ "net.inet.he.resolution_delay", RT_SYSCTL_INT, RT_SYSCTL_RW, 0, { 0 },
	  &rt_he_resolution_delay, 0, 1000, NULL, "Happy Eyeballs: ms an A answer waits for the AAAA one (50)" },
	{ "net.inet.he.prefer", RT_SYSCTL_INT, RT_SYSCTL_RW, 0, { 0 },
	  &rt_he_prefer, 0, 6, NULL, "Happy Eyeballs: first family, 0 = RFC 6724 order, 4 = IPv4, 6 = IPv6" },
	{ "net.inet.he.aaaa_filter", RT_SYSCTL_INT, RT_SYSCTL_RW, 0, { 0 },
	  &rt_he_aaaa_filter, 0, 1, NULL, "getaddrinfo(AF_UNSPEC): no AAAA while IPv6 has no usable route" },
};

int rt_he_command(int argc, char **argv, char *out, u32 outlen)
{
	static const char usage[] = "usage: happy_eyeballs on|off | delay MS | resolution_delay MS | "
	                            "prefer auto|ipv6|ipv4 | aaaa_filter on|off | status";
	const char *v = argc >= 3 ? argv[2] : NULL, *e;
	unsigned long n;

	if (out && outlen)
		out[0] = 0;
	if (argc < 2 || !rt_strcasecmp(argv[1], "status")) {
		rt_snprintf(out, outlen, "happy eyeballs %s, attempt delay %ld ms, resolution delay %ld ms, "
		            "first %s, AAAA filter %s (IPv6 %s)\n", rt_he_enable ? "on" : "off",
		            (long)rt_he_attempt_delay, (long)rt_he_resolution_delay,
		            rt_he_prefer == 6 ? "IPv6" : rt_he_prefer == 4 ? "IPv4" : "by RFC 6724",
		            rt_he_aaaa_filter ? "on" : "off", rt_he_v6_usable() ? "usable" : "not usable");
		return 0;
	}
	if (argc == 2 && (!rt_strcasecmp(argv[1], "on") || !rt_strcasecmp(argv[1], "off"))) {
		rt_he_enable = !rt_strcasecmp(argv[1], "on");
		return 0;
	}
	if (argc == 3 && (!rt_strcasecmp(argv[1], "delay") || !rt_strcasecmp(argv[1], "attempt_delay") ||
	                  !rt_strcasecmp(argv[1], "resolution_delay"))) {
		int res = !rt_strcasecmp(argv[1], "resolution_delay");
		n = rt_strtoul(v, &e, 10);
		if (*e || e == v || (res ? n > 1000 : n < 10 || n > 2000)) {
			rt_snprintf(out, outlen, res ? "happy_eyeballs: resolution_delay is 0..1000 ms" :
			                               "happy_eyeballs: delay is 10..2000 ms");
			return -RT_EINVAL;
		}
		if (res)
			rt_he_resolution_delay = (s32)n;
		else
			rt_he_attempt_delay = (s32)n;
		return 0;
	}
	if (argc == 3 && !rt_strcasecmp(argv[1], "prefer")) {
		if (!rt_strcasecmp(v, "auto") || !rt_strcasecmp(v, "rfc6724"))
			rt_he_prefer = 0;
		else if (!rt_strcasecmp(v, "ipv6") || !strcmp(v, "6"))
			rt_he_prefer = 6;
		else if (!rt_strcasecmp(v, "ipv4") || !strcmp(v, "4"))
			rt_he_prefer = 4;
		else
			goto bad;
		return 0;
	}
	if (argc == 3 && !rt_strcasecmp(argv[1], "aaaa_filter") &&
	    (!rt_strcasecmp(v, "on") || !rt_strcasecmp(v, "off"))) {
		rt_he_aaaa_filter = !rt_strcasecmp(v, "on");
		return 0;
	}
bad:
	rt_snprintf(out, outlen, "%s", usage);
	return -RT_EINVAL;
}

void rt_he_init(void)
{
	rt_sysctl_register(he_sysctl, (int)RT_ARRAY_SIZE(he_sysctl));
}

/* Can IPv6 reach the Internet: a route to a global address with a
 * usable source address? (lock held) */
int rt_he_v6_usable(void)
{
	static const rt_in6_addr probe = { { 0x20, 0x01, 0x48, 0x60, 0x48, 0x60, 0, 0, 0, 0, 0, 0, 0, 0, 0x88, 0x88 } };
	struct rt_inet_route r;

	return rt_inet_route(&probe, 0, &r) == 0 && !rt_inaddr_is_any(&r.src) && !rt_in6_is_v4mapped(&r.src);
}

/* RFC 8305 4: the first address's family (or the preferred one) first,
 * then the families alternate; each family keeps its RFC 6724 order. */
static int interleave(rt_in6_addr *a, int n, int prefer)
{
	rt_in6_addr v6[HE_MAXADDR], v4[HE_MAXADDR];
	int n6 = 0, n4 = 0, i, k = 0, first6;

	for (i = 0; i < n; i++) {
		if (rt_in6_is_v4mapped(&a[i]))
			v4[n4++] = a[i];
		else
			v6[n6++] = a[i];
	}
	first6 = prefer == 6 ? n6 > 0 : prefer == 4 ? n4 == 0 : n > 0 && !rt_in6_is_v4mapped(&a[0]);
	for (i = 0; k < n; i++) {
		if (first6) {
			if (i < n6)
				a[k++] = v6[i];
			if (i < n4)
				a[k++] = v4[i];
		} else {
			if (i < n4)
				a[k++] = v4[i];
			if (i < n6)
				a[k++] = v6[i];
		}
	}
	return n;
}

static void he_sockaddr(const rt_in6_addr *a, u16 port, struct rt_sockaddr_in6 *s6, u32 *len)
{
	memset(s6, 0, sizeof(*s6));
	if (rt_in6_is_v4mapped(a)) {
		struct rt_sockaddr_in *s4 = (struct rt_sockaddr_in *)(void *)s6;
		s4->sin_len = sizeof(*s4);
		s4->sin_family = RT_AF_INET;
		s4->sin_port = rt_htons(port);
		s4->sin_addr = rt_in6_v4(a);
		*len = sizeof(*s4);
	} else {
		s6->sin6_len = sizeof(*s6);
		s6->sin6_family = RT_AF_INET6;
		s6->sin6_port = rt_htons(port);
		s6->sin6_addr = *a;
		*len = sizeof(*s6);
	}
}

/* a non-blocking connect to a; the fd (connected: *done = 1) or -errno */
static int he_start(struct rt_sockctx *c, const rt_in6_addr *a, u16 port, int type, int *done)
{
	struct rt_sockaddr_in6 sa;
	u32 len;
	s32 on = 1;
	int fd, r;

	*done = 0;
	fd = rt_api_socket(c, rt_in6_is_v4mapped(a) ? RT_AF_INET : RT_AF_INET6, type, 0);
	if (fd < 0)
		return fd;
	rt_api_ioctl(c, fd, RT_FIONBIO, &on);
	he_sockaddr(a, port, &sa, &len);
	r = rt_api_connect(c, fd, &sa, len);
	if (r == 0) {
		*done = 1;
		return fd;
	}
	if (r == -RT_EINPROGRESS || r == -RT_EALREADY || r == -RT_EWOULDBLOCK)
		return fd;
	rt_api_close(c, fd);
	return r;
}

int rt_he_connect(struct rt_sockctx *c, const char *host, u16 port, int family, int type, u32 timeout_ms,
                  struct rt_he_result *res)
{
	rt_in6_addr a[HE_MAXADDR];
	int fds[HE_MAXADDR], n, i, started = 0, pending = 0, winner = -1, last_err = -RT_ETIMEDOUT;
	u32 start = rt_now_ms(), next_at = start, delay;
	char canon[2];

	if (res)
		memset(res, 0, sizeof(*res));
	if (!host || !port)
		return -RT_EINVAL;
	if (!type)
		type = RT_SOCK_STREAM;
	if (!timeout_ms)
		timeout_ms = 75000;
	/* literals, then the resolver */
	{
		u32 v4;
		int plen;
		if (rt_parse_ipv4(host, &v4) == 0) {
			if (family == RT_AF_INET6)
				return -RT_EAFNOSUPPORT;
			rt_in6_set_v4mapped(&a[0], v4);
			n = 1;
		} else if (rt_inet6_parse_prefix(host, &a[0], &plen, NULL, 0) == 0 && plen < 0) {
			if (family == RT_AF_INET)
				return -RT_EAFNOSUPPORT;
			n = 1;
		} else {
			int fam = family;
			if (fam == RT_AF_UNSPEC && rt_he_aaaa_filter) {
				rt_lock();
				if (!rt_he_v6_usable())
					fam = RT_AF_INET;
				rt_unlock();
			}
			n = rt_he_enable ? rt_resolve_name46(host, a, HE_MAXADDR, fam, canon, sizeof(canon)) : 0;
			if (!rt_he_enable) {
				/* sequential lookups, AAAA then A */
				rt_in6_addr t6[8];
				u32 t4[8];
				int k, r;
				n = 0;
				if (fam != RT_AF_INET) {
					r = rt_resolve_name6(host, t6, 8, NULL, 0);
					for (k = 0; k < r && n < HE_MAXADDR; k++)
						a[n++] = t6[k];
				}
				if (fam != RT_AF_INET6) {
					r = rt_resolve_name(host, t4, 8, NULL, 0);
					for (k = 0; k < r && n < HE_MAXADDR; k++)
						rt_in6_set_v4mapped(&a[n++], t4[k]);
				}
				if (!n)
					return -RT_HOST_NOT_FOUND;
			}
			if (n < 0)
				return n;
			if (n > 1) {
				rt_lock();
				rt_inet_sort_dest(a, n);
				rt_unlock();
			}
			interleave(a, n, (int)rt_he_prefer);
		}
	}
	if (res)
		res->naddrs = (u16)n;
	/* off: one address after the other, each with its share of the time */
	delay = rt_he_enable ? (u32)rt_he_attempt_delay : timeout_ms / (u32)n;
	for (i = 0; i < HE_MAXADDR; i++)
		fds[i] = -1;
	for (;;) {
		u32 now = rt_now_ms(), wset[HE_MAXADDR / 32 + 8], wait;
		int maxfd = -1, r, done;
		if ((s32)(now - start) >= (s32)timeout_ms)
			break;
		/* the next attempt: on time, or now when nothing is pending */
		if (started < n && (pending == 0 || (s32)(now - next_at) >= 0)) {
			if (!rt_he_enable) {
				/* sequential: the previous attempt gives up */
				for (i = 0; i < started; i++)
					if (fds[i] >= 0) {
						rt_api_close(c, fds[i]);
						fds[i] = -1;
						pending--;
						last_err = -RT_ETIMEDOUT;
					}
			}
			r = he_start(c, &a[started], port, type, &done);
			started++;
			if (res)
				res->attempts = (u16)started;
			if (r < 0) {
				last_err = r;
				continue;
			}
			fds[started - 1] = r;
			pending++;
			next_at = rt_now_ms() + delay;
			if (done) {
				winner = started - 1;
				break;
			}
		}
		if (!pending) {
			if (started >= n)
				break;
			continue;
		}
		memset(wset, 0, sizeof(wset));
		for (i = 0; i < started; i++)
			if (fds[i] >= 0) {
				wset[fds[i] >> 5] |= 1u << (fds[i] & 31);
				if (fds[i] > maxfd)
					maxfd = fds[i];
			}
		if ((u32)(maxfd >> 5) >= RT_ARRAY_SIZE(wset)) {
			last_err = -RT_EMFILE;
			break;
		}
		wait = timeout_ms - (now - start);
		if (started < n && (u32)(next_at - now) < wait)
			wait = next_at - now;
		r = rt_api_select(c, maxfd + 1, NULL, wset, NULL, (s32)wait, NULL);
		if (r < 0) {
			last_err = r;
			break;
		}
		for (i = 0; r > 0 && i < started; i++) {
			s32 err = 0;
			u32 el = sizeof(err);
			if (fds[i] < 0 || !((wset[fds[i] >> 5] >> (fds[i] & 31)) & 1))
				continue;
			rt_api_getsockopt(c, fds[i], RT_SOL_SOCKET, RT_SO_ERROR, &err, &el);
			if (err == 0) {
				winner = i;
				break;
			}
			last_err = -err;
			rt_api_close(c, fds[i]);
			fds[i] = -1;
			pending--;
			next_at = rt_now_ms();      /* a failure starts the next one now */
		}
		if (winner >= 0)
			break;
	}
	for (i = 0; i < started; i++)
		if (fds[i] >= 0 && i != winner)
			rt_api_close(c, fds[i]);
	if (winner < 0)
		return last_err;
	{
		s32 off = 0;
		rt_api_ioctl(c, fds[winner], RT_FIONBIO, &off);
	}
	if (res) {
		res->addr = a[winner];
		res->ms = rt_now_ms() - start;
		res->index = (u16)winner;
	}
	return fds[winner];
}
