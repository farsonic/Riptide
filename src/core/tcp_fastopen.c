/*
 * Riptide - TCP Fast Open, client side (RFC 7413): the cookie cache, the
 * per-destination SYN loss backoff and the global middlebox ("blackhole")
 * detection. Follows Linux net/ipv4/tcp_fastopen.c and the fastopen parts
 * of tcp_metrics.c; the SYN with data is built in tcp_output.c.
 *
 * A client learns a server's cookie from a SYN-ACK (asked for with an
 * empty option in its SYN) and on later connections to that server sends
 * it with the first data in the SYN, so a request reaches the server one
 * round trip earlier. Servers that do not know TFO ignore the option; if
 * the SYN-ACK does not acknowledge the data, it is sent again at once.
 * Some middleboxes drop SYNs with data or unknown options: a SYN with the
 * option that times out marks the destination (no TFO there for a while,
 * doubling) and counts towards a global switch-off for
 * net.inet.tcp.fastopen_blackhole seconds (doubling up to 64 times).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "tcp.h"
#include "rt/rt_sysctl.h"

#define FO_CACHE_SIZE  32
#define FO_BLACKHOLE_N 3         /* consecutive lost TFO SYNs before the global switch-off */
#define FO_LOSS_BASE_S 60        /* per destination: no TFO for 60 s << (syn_loss - 2) */

s32 sysctl_tcp_fastopen = 1;           /* bit 0: client; bit 1 (server) is not implemented */
s32 sysctl_tcp_fastopen_all = 0;       /* every connect() behaves as with TCP_FASTOPEN_CONNECT */
s32 sysctl_tcp_fastopen_blackhole = 3600;

struct fo_entry {
	rt_in6_addr daddr;
	u32 used_s;                  /* tcp_seconds() of the last use (LRU) */
	u32 loss_s;                  /* of the last lost SYN with the option */
	u16 mss;                     /* the server's MSS */
	u8  len;                     /* cookie length, 0 = none known yet */
	u8  syn_loss;                /* lost SYNs with the option in a row */
	u8  cookie[TCP_FASTOPEN_COOKIE_MAX];
	u8  valid;
};

static struct fo_entry fo_cache[FO_CACHE_SIZE];
static u32 fo_fail_run;                /* lost TFO SYNs in a row, all destinations */
static u32 fo_disable_times;           /* global switch-offs since the last success */
static u32 fo_disabled_until;          /* tcp_seconds(); 0 = enabled */

static struct fo_entry *fo_find(const rt_in6_addr *daddr)
{
	int i;

	for (i = 0; i < FO_CACHE_SIZE; i++)
		if (fo_cache[i].valid && rt_in6_eq(&fo_cache[i].daddr, daddr))
			return &fo_cache[i];
	return NULL;
}

/* the entry for daddr, taking a free or the least recently used one */
static struct fo_entry *fo_get(const rt_in6_addr *daddr)
{
	struct fo_entry *e = fo_find(daddr), *old = NULL;
	int i;

	if (e)
		return e;
	for (i = 0; i < FO_CACHE_SIZE; i++) {
		e = &fo_cache[i];
		if (!e->valid)
			break;
		if (!old || (s32)(e->used_s - old->used_s) < 0)
			old = e;
	}
	if (i == FO_CACHE_SIZE)
		e = old;
	memset(e, 0, sizeof(*e));
	e->daddr = *daddr;
	e->valid = 1;
	e->used_s = tcp_seconds();
	return e;
}

static int fo_globally_disabled(void)
{
	if (!fo_disabled_until)
		return 0;
	if ((s32)(tcp_seconds() - fo_disabled_until) >= 0) {
		fo_disabled_until = 0;
		fo_fail_run = 0;
		return 0;
	}
	return 1;
}

/* Linux tcp_fastopen_cookie_check(): may a connection to daddr use TFO?
 * 1: a cookie is known (copied to cookie/len, the server's MSS to *mss),
 * 0: send a cookie request, -1: no TFO option at all. */
int tcp_fastopen_lookup(const rt_in6_addr *daddr, u8 *cookie, u8 *len, u16 *mss)
{
	struct fo_entry *e;

	if (!(sysctl_tcp_fastopen & 1) || fo_globally_disabled())
		return -1;
	e = fo_find(daddr);
	if (!e)
		return 0;
	e->used_s = tcp_seconds();
	/* Linux: after the second lost SYN in a row, no TFO for a while */
	if (e->syn_loss > 1 &&
	    (s32)(tcp_seconds() - e->loss_s) < (s32)(FO_LOSS_BASE_S << RT_MIN(e->syn_loss - 2u, 6u)))
		return -1;
	if (!e->len)
		return 0;
	memcpy(cookie, e->cookie, e->len);
	*len = e->len;
	*mss = e->mss;
	return 1;
}

/* A SYN-ACK arrived for a SYN that carried the option (opt: its options,
 * mss_clamp = the server's MSS). */
void tcp_fastopen_synack(struct tcp_sock *tp, const struct tcp_rx_opt *opt)
{
	struct fo_entry *e = fo_get(&tp->daddr);
	int data_sent = (tp->fo_flags & TCP_FO_SYN_DATA) != 0;
	int data_acked = (tp->fo_flags & TCP_FO_DATA_ACKED) != 0;

	e->used_s = tcp_seconds();
	e->mss = opt->mss_clamp;
	if (opt->fo_len != TCP_FASTOPEN_NONE && opt->fo_len >= TCP_FASTOPEN_COOKIE_MIN) {
		memcpy(e->cookie, opt->fo_cookie, opt->fo_len);
		e->len = opt->fo_len;
	} else if (data_sent && !data_acked && !(tp->fo_flags & TCP_FO_SYN_LOST)) {
		e->len = 0;      /* the server got the SYN and ignored our cookie */
	}
	if (tp->fo_flags & TCP_FO_SYN_LOST) {
		/* the SYN with the option was lost, a plain one got through:
		 * a middlebox may drop TFO SYNs (Linux
		 * tcp_fastopen_active_detect_blackhole) */
		e->syn_loss = (u8)RT_MIN(e->syn_loss + 1u, 255u);
		e->loss_s = tcp_seconds();
		rt_tcpstat.TCPFastOpenSynLost++;
		if (++fo_fail_run >= FO_BLACKHOLE_N && sysctl_tcp_fastopen_blackhole) {
			u32 t = (u32)sysctl_tcp_fastopen_blackhole << RT_MIN(fo_disable_times, 6u);
			fo_disabled_until = tcp_seconds() + t;
			if (!fo_disabled_until)
				fo_disabled_until = 1;
			fo_disable_times++;
			fo_fail_run = 0;
			rt_tcpstat.TCPFastOpenBlackhole++;
			RT_DBG(RT_DBG_TCP, "TFO: %lu lost SYNs in a row, off for %lu s",
			       (unsigned long)FO_BLACKHOLE_N, (unsigned long)t);
		}
	} else {
		e->syn_loss = 0;
		fo_fail_run = 0;
		if (data_acked)
			fo_disable_times = 0;
	}
	if (data_sent) {
		if (data_acked)
			rt_tcpstat.TCPFastOpenActive++;
		else
			rt_tcpstat.TCPFastOpenActiveFail++;
	}
}

/* forget everything (net.inet.tcp.fastopen_cache=clear) */
static void fo_flush(void)
{
	memset(fo_cache, 0, sizeof(fo_cache));
	fo_fail_run = fo_disable_times = fo_disabled_until = 0;
}

/* net.inet.tcp.fastopen_cache: one line per server, writing clears */
static int fo_sysctl_cache(const struct rt_sysctl *e, void *old, u32 *oldlen,
                           const void *newp, u32 newlen)
{
	if (oldlen) {
		char *o = old;
		u32 cap = old ? *oldlen : 0, n = 0, need = 1;
		int i, j;
		char line[96], *p;

		if (fo_globally_disabled()) {
			p = line;
			p += rt_snprintf(p, sizeof(line), "disabled for %lu s (lost TFO SYNs)\n",
			                 (unsigned long)(fo_disabled_until - tcp_seconds()));
			need += (u32)(p - line);
			if (o && n + (u32)(p - line) < cap) {
				memcpy(o + n, line, (u32)(p - line));
				n += (u32)(p - line);
			}
		}
		for (i = 0; i < FO_CACHE_SIZE; i++) {
			const struct fo_entry *f = &fo_cache[i];
			char a[48];
			u32 l;
			if (!f->valid)
				continue;
			if (rt_in6_is_v4mapped(&f->daddr))
				rt_snprintf(a, sizeof(a), "%u.%u.%u.%u", f->daddr.b[12], f->daddr.b[13],
				            f->daddr.b[14], f->daddr.b[15]);
			else
				rt_inet6_ntop(&f->daddr, a, sizeof(a));
			p = line + rt_snprintf(line, sizeof(line), "%s %s", a, f->len ? "cookie " : "no cookie");
			for (j = 0; j < f->len; j++)
				p += rt_snprintf(p, 3, "%02x", f->cookie[j]);
			p += rt_snprintf(p, (u32)(line + sizeof(line) - p), " mss %u%s\n", (unsigned)f->mss,
			                 f->syn_loss ? " syn-lost" : "");
			l = (u32)(p - line);
			need += l;
			if (o && n + l < cap) {
				memcpy(o + n, line, l);
				n += l;
			}
		}
		if (o) {
			if (!cap)
				return -RT_ENOMEM;
			o[n < cap ? n : cap - 1] = 0;
			*oldlen = n + 1;
			if (need > cap)
				return -RT_ENOMEM;
		} else {
			*oldlen = need;
		}
	}
	if (newp && newlen <= 64)       /* any short word, e.g. "clear" */
		fo_flush();
	return 0;
}

static const struct rt_sysctl fo_sysctls[] = {
	{ "net.inet.tcp.fastopen", RT_SYSCTL_INT, RT_SYSCTL_RW, 0, { 0 }, &sysctl_tcp_fastopen, 0, 1,
	  NULL, "TCP Fast Open client (RFC 7413): 1 on, 0 off" },
	{ "net.inet.tcp.fastopen_all", RT_SYSCTL_INT, RT_SYSCTL_RW, 0, { 0 }, &sysctl_tcp_fastopen_all, 0, 1,
	  NULL, "every connect() as with TCP_FASTOPEN_CONNECT" },
	{ "net.inet.tcp.fastopen_blackhole", RT_SYSCTL_INT, RT_SYSCTL_RW, 0, { 0 },
	  &sysctl_tcp_fastopen_blackhole, 0, 86400, NULL, "TFO off for N s (doubling) after lost TFO SYNs" },
	{ "net.inet.tcp.fastopen_cache", RT_SYSCTL_STRING, RT_SYSCTL_RW, 0, { 0 }, NULL, 0, 0,
	  fo_sysctl_cache, "TFO cookies by server (writing clears)" },
};

void tcp_fastopen_init(void)
{
	fo_flush();
	rt_sysctl_register(fo_sysctls, (int)RT_ARRAY_SIZE(fo_sysctls));
}
