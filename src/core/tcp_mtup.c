/*
 * Riptide - packetization layer path MTU discovery for TCP (RFC 4821),
 * after Linux's icsk_mtup (tcp_mtup_init, tcp_mtu_probe in tcp_output.c,
 * tcp_mtup_probe_success/failed in tcp_input.c and the black hole reaction
 * in tcp_timer.c's tcp_mtu_probing()).
 *
 * Classic path MTU discovery depends on ICMP "fragmentation needed" /
 * "packet too big" reaching the sender. Where a firewall drops those (a
 * PMTU black hole, e.g. behind a PPPoE or VPN link with MTU < 1500), a
 * connection's full sized segments vanish without a trace: the handshake
 * and small requests work, bulk data stalls until the user gives up.
 *
 * net.inet.tcp.mtu_probing:
 *   0  off: only ICMP lowers the MSS
 *   1  (default, as Linux) off until a black hole is suspected: after
 *      tcp_retries1 timeouts of the same segment the connection drops to
 *      base_mss (and halves from there on each further run of timeouts),
 *      then probes back up
 *   2  always on: every connection starts at base_mss and probes upwards
 * A probe is one segment of a size halfway between the largest MTU known
 * to work (search_low) and the smallest known not to (search_high + 1),
 * sent while the regular segments stay at the working size. Its ACK raises
 * search_low; a loss of just the probe (detected by fast retransmit or a
 * retransmission of its range) lowers search_high. Once the range is
 * narrower than probe_threshold bytes the search stops, and restarts after
 * probe_interval seconds in case the path has improved.
 *
 * A black hole hit is remembered per destination (net.inet.tcp.mtu_cache)
 * for probe_interval seconds, so the next connection to the same host
 * starts at the working size instead of stalling again. This TCP-only
 * cache does not touch the IP layer's PMTU cache: a guess must not shrink
 * UDP datagrams.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "tcp.h"
#include "rt/rt_sysctl.h"

#define MTUP_CACHE_SIZE 16

s32 sysctl_tcp_mtu_probing = 1;
s32 sysctl_tcp_base_mss = 1024;           /* Linux TCP_BASE_MSS */
s32 sysctl_tcp_mtu_probe_floor = 48;      /* Linux TCP_MIN_SND_MSS */
s32 sysctl_tcp_probe_threshold = 8;
s32 sysctl_tcp_probe_interval = 600;      /* seconds */

struct mtup_entry {
	rt_in6_addr daddr;
	u32 stamp_s;                 /* tcp_seconds() when recorded */
	u16 low;                     /* an MTU that worked (search_low) */
	u8  valid;
};

static struct mtup_entry mtup_cache[MTUP_CACHE_SIZE];

static struct mtup_entry *mtup_find(const rt_in6_addr *daddr)
{
	int i;

	for (i = 0; i < MTUP_CACHE_SIZE; i++) {
		struct mtup_entry *e = &mtup_cache[i];
		if (!e->valid)
			continue;
		if (tcp_seconds() - e->stamp_s >= (u32)sysctl_tcp_probe_interval) {
			e->valid = 0;
			continue;
		}
		if (rt_in6_eq(&e->daddr, daddr))
			return e;
	}
	return NULL;
}

static void mtup_remember(const struct tcp_sock *tp)
{
	struct mtup_entry *e = mtup_find(&tp->daddr), *old = NULL;
	int i;

	if (!e) {
		for (i = 0; i < MTUP_CACHE_SIZE; i++) {
			e = &mtup_cache[i];
			if (!e->valid)
				break;
			if (!old || (s32)(e->stamp_s - old->stamp_s) < 0)
				old = e;
		}
		if (i == MTUP_CACHE_SIZE)
			e = old;
		e->daddr = tp->daddr;
		e->valid = 1;
	}
	e->stamp_s = tcp_seconds();
	e->low = tp->mtup_search_low;
}

static void mtup_forget(const struct tcp_sock *tp)
{
	struct mtup_entry *e = mtup_find(&tp->daddr);

	if (e)
		e->valid = 0;
}

/* Linux tcp_mtup_init(): before the first tcp_sync_mss() of a connection,
 * with mss_clamp, tcp_header_len and net_hlen set */
void tcp_mtup_init(struct tcp_sock *tp)
{
	const struct mtup_entry *e;
	u32 low;

	tp->mtup_enabled = (u8)(sysctl_tcp_mtu_probing > 1);
	tp->mtup_search_high = (u16)(tp->rx_opt.mss_clamp + TCP_HDR_LEN + tp->net_hlen);
	low = tcp_mss_to_mtu(tp, (u32)sysctl_tcp_base_mss);
	tp->mtup_probe_size = 0;
	tp->mtup_probe_ts = tcp_jiffies();
	if (sysctl_tcp_mtu_probing && (e = mtup_find(&tp->daddr)) != NULL) {
		/* this destination was a black hole lately: start at what worked */
		tp->mtup_enabled = 1;
		low = e->low;
	}
	if (low > tp->mtup_search_high)
		low = tp->mtup_search_high;
	tp->mtup_search_low = (u16)low;
}

/* Linux tcp_mtu_probing(): the retransmission timer ran out tcp_retries1
 * times for one segment. The first time enables probing (dropping to
 * search_low via tcp_sync_mss); each later time halves the size. */
void tcp_mtu_blackhole(struct tcp_sock *tp)
{
	u32 mss, floor;

	if (!sysctl_tcp_mtu_probing)
		return;
	if (!tp->mtup_enabled) {
		tp->mtup_enabled = 1;
		tp->mtup_probe_ts = tcp_jiffies();
	} else {
		mss = tcp_mtu_to_mss(tp, tp->mtup_search_low) >> 1;
		mss = RT_MIN((u32)sysctl_tcp_base_mss, mss);
		floor = RT_MAX((u32)sysctl_tcp_mtu_probe_floor, (u32)TCP_MIN_SND_MSS);
		mss = RT_MAX(mss, floor);
		tp->mtup_search_low = (u16)tcp_mss_to_mtu(tp, mss);
	}
	tp->mtup_probe_size = 0;
	if (tp->mtup_search_low < tp->pmtu_cookie) {
		rt_tcpstat.TCPMTUPBlackhole++;
		mtup_remember(tp);
	}
	tcp_sync_mss(tp, tp->pmtu_cookie);
}

/* Linux tcp_mtup_probe_success(): the probe's whole range was ACKed */
void tcp_mtup_probe_success(struct tcp_sock *tp)
{
	u32 val, full;

	/* the probe used one cwnd slot; scale cwnd to the bigger packets */
	val = (u32)((u64)tp->snd_cwnd * tcp_mss_to_mtu(tp, tp->mss_cache) / tp->mtup_probe_size);
	tp->snd_cwnd = RT_MAX(val, 1u);
	tp->snd_cwnd_cnt = 0;
	tp->snd_cwnd_stamp = tcp_jiffies();
	tp->snd_ssthresh = tcp_current_ssthresh(tp);
	tp->mtup_search_low = tp->mtup_probe_size;
	tp->mtup_probe_size = 0;
	tcp_sync_mss(tp, tp->pmtu_cookie);
	rt_tcpstat.TCPMTUPSuccess++;
	full = RT_MIN((u32)tp->rx_opt.mss_clamp + TCP_HDR_LEN + tp->net_hlen, (u32)tp->pmtu_cookie);
	if (tp->mtup_search_low + (u32)sysctl_tcp_probe_threshold >= full)
		mtup_forget(tp);       /* back to the full size: no black hole any more */
	else if (mtup_find(&tp->daddr))
		mtup_remember(tp);
}

/* Linux tcp_mtup_probe_failed(): the probe was lost (and only it) */
void tcp_mtup_probe_failed(struct tcp_sock *tp)
{
	tp->mtup_search_high = (u16)(tp->mtup_probe_size - 1);
	tp->mtup_probe_size = 0;
	rt_tcpstat.TCPMTUPFail++;
}

/* Linux tcp_mtu_check_reprobe(): the search has converged; after
 * probe_interval seconds widen it again in case the path changed */
void tcp_mtu_check_reprobe(struct tcp_sock *tp)
{
	u32 interval = (u32)sysctl_tcp_probe_interval * 1000u;

	if ((s32)(tcp_jiffies() - tp->mtup_probe_ts) >= (s32)interval) {
		tp->mtup_search_high = (u16)RT_MIN((u32)tp->rx_opt.mss_clamp + TCP_HDR_LEN + tp->net_hlen,
		                                   (u32)tp->pmtu_cookie);
		tp->mtup_search_low = (u16)tcp_mss_to_mtu(tp, tp->mss_cache);
		tp->mtup_probe_ts = tcp_jiffies();
	}
}

static void mtup_flush(void)
{
	memset(mtup_cache, 0, sizeof(mtup_cache));
}

/* net.inet.tcp.mtu_cache: one line per black hole destination, writing clears */
static int mtup_sysctl_cache(const struct rt_sysctl *e, void *old, u32 *oldlen,
                             const void *newp, u32 newlen)
{
	if (oldlen) {
		char *o = old;
		u32 cap = old ? *oldlen : 0, n = 0, need = 1;
		int i;
		char line[80], a[48];

		for (i = 0; i < MTUP_CACHE_SIZE; i++) {
			const struct mtup_entry *f = &mtup_cache[i];
			u32 l;
			if (!f->valid || tcp_seconds() - f->stamp_s >= (u32)sysctl_tcp_probe_interval)
				continue;
			if (rt_in6_is_v4mapped(&f->daddr))
				rt_snprintf(a, sizeof(a), "%u.%u.%u.%u", f->daddr.b[12], f->daddr.b[13],
				            f->daddr.b[14], f->daddr.b[15]);
			else
				rt_inet6_ntop(&f->daddr, a, sizeof(a));
			l = (u32)rt_snprintf(line, sizeof(line), "%s mtu %u age %lu s\n", a, (unsigned)f->low,
			                     (unsigned long)(tcp_seconds() - f->stamp_s));
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
	if (newp && newlen <= 64)
		mtup_flush();
	return 0;
}

static const struct rt_sysctl mtup_sysctls[] = {
	{ "net.inet.tcp.mtu_probing", RT_SYSCTL_INT, RT_SYSCTL_RW, 0, { 0 }, &sysctl_tcp_mtu_probing, 0, 2,
	  NULL, "PLPMTUD (RFC 4821): 0 off, 1 after a black hole, 2 always" },
	{ "net.inet.tcp.base_mss", RT_SYSCTL_INT, RT_SYSCTL_RW, 0, { 0 }, &sysctl_tcp_base_mss, 48, 65495,
	  NULL, "MSS a suspected PMTU black hole falls back to" },
	{ "net.inet.tcp.mtu_probe_floor", RT_SYSCTL_INT, RT_SYSCTL_RW, 0, { 0 }, &sysctl_tcp_mtu_probe_floor,
	  48, 65495, NULL, "lowest MSS black hole halving goes to" },
	{ "net.inet.tcp.probe_threshold", RT_SYSCTL_INT, RT_SYSCTL_RW, 0, { 0 }, &sysctl_tcp_probe_threshold,
	  1, 65535, NULL, "stop probing when the MTU range is this narrow" },
	{ "net.inet.tcp.probe_interval", RT_SYSCTL_INT, RT_SYSCTL_RW, 0, { 0 }, &sysctl_tcp_probe_interval,
	  1, 86400, NULL, "s until a converged MTU search restarts" },
	{ "net.inet.tcp.mtu_cache", RT_SYSCTL_STRING, RT_SYSCTL_RW, 0, { 0 }, NULL, 0, 0,
	  mtup_sysctl_cache, "PMTU black hole destinations (writing clears)" },
};

void tcp_mtup_module_init(void)
{
	mtup_flush();
	rt_sysctl_register(mtup_sysctls, (int)RT_ARRAY_SIZE(mtup_sysctls));
}
