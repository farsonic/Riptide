/*
 * Riptide - SNTP client (RFC 4330; RFC 5905 client mode) and the time zone.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Runs from timers in the stack thread on one kernel UDP socket (AF_INET6,
 * dual stack). Server names are looked up here too, asynchronously: an A
 * (then AAAA) query to the configured name servers on the same socket, as
 * the blocking resolver cannot run in the stack thread. A sync tries each
 * address in turn, 2 s each; the first valid reply wins. The clock is
 * stepped through rt_set_wallclock_hook when the offset is at least
 * ntp_step_ms (no slewing: AmigaOS has none), otherwise only measured.
 *
 * Time stamps are kept as seconds + microseconds since 1970 in 32-bit
 * parts: no 64-bit division (the 68060 traps it). NTP seconds convert with
 * u32 arithmetic, which also covers era 1 (from February 2036) up to 2106.
 */
#include "sock_internal.h"
#include "infra.h"
#include "resolv_internal.h"

#define NTP_PORT         123
#define DNS_PORT_        53
#define NTP_EPOCH        2208988800u      /* 1900-01-01 to 1970-01-01 */
#define NTP_NAMES        4
#define NTP_ADDRS        8
#define NTP_TIMEOUT_MS   2000
#define NTP_RETRY_S      30
#define NTP_DEFAULT_POOL "pool.ntp.org"

s32 rt_tz_offset;
s32 rt_tz_system;
u8  rt_tz_configured;
u8  rt_ntp_battclock;
int (*rt_set_wallclock_hook)(u32 sec, u32 usec);

struct ntp_tv {
	u32 sec, usec;
};

enum { NS_IDLE, NS_DNS, NS_QUERY };

static struct {
	u8  enabled;
	u8  state;
	u8  backoff;              /* failed syncs in a row */
	int nnames;
	char names[NTP_NAMES][64];
	u32 dhcp[4];              /* option 42, network order */
	int ndhcp;
	u32 interval_s, step_ms;
	/* the sync in progress */
	int name_i;               /* name being looked up */
	u16 dns_type;
	int dns_server;
	u16 dns_id;
	int naddr, addr_i;
	rt_in6_addr addr[NTP_ADDRS];
	char addr_name[NTP_ADDRS][64];
	struct ntp_tv t1;         /* our transmit time */
	u8  nonce[8];             /* our transmit stamp, echoed as originate */
	/* last result */
	u8  synced;
	u8  stratum;
	s32 off_sec, off_usec;    /* offset of the last sync */
	u32 delay_us;
	u32 last_ms;              /* rt_now_ms() of the last sync */
	u32 next_ms;
	rt_in6_addr last_addr;
	char last_name[64];
	char last_err[48];
	u8  stepped;
} ntp;

static struct rt_socket *ntp_so;
static struct sock_upcall ntp_upcall;
static struct rt_timer ntp_timer, ntp_rx_timer;
static u8 ntp_buf[512];

/* ---- time stamp arithmetic ---- */

static void tv_norm(s32 *sec, s32 *usec)
{
	while (*usec < 0) {
		*usec += 1000000;
		(*sec)--;
	}
	while (*usec >= 1000000) {
		*usec -= 1000000;
		(*sec)++;
	}
}

/* a - b */
static void tv_sub(const struct ntp_tv *a, const struct ntp_tv *b, s32 *sec, s32 *usec)
{
	*sec = (s32)(a->sec - b->sec);
	*usec = (s32)a->usec - (s32)b->usec;
	tv_norm(sec, usec);
}

/* NTP 32.32 fixed point to Unix time; u32 wrap handles era 1 */
void rt_ntp_ts_to_tv(const u8 *p, struct ntp_tv *t);
void rt_ntp_ts_to_tv(const u8 *p, struct ntp_tv *t)
{
	u32 frac = rt_get_be32(p + 4);

	t->sec = rt_get_be32(p) - NTP_EPOCH;
	t->usec = (u32)(rt_mul32x32(frac, 1000000u) >> 32);
}

/* offset = ((T2 - T1) + (T3 - T4)) / 2, delay = (T4 - T1) - (T3 - T2) */
int rt_ntp_compute(const struct ntp_tv *t1, const struct ntp_tv *t2, const struct ntp_tv *t3,
                   const struct ntp_tv *t4, s32 *off_sec, s32 *off_usec, s32 *delay_us);
int rt_ntp_compute(const struct ntp_tv *t1, const struct ntp_tv *t2, const struct ntp_tv *t3,
                   const struct ntp_tv *t4, s32 *off_sec, s32 *off_usec, s32 *delay_us)
{
	s32 as, au, bs, bu, s, u, odd;

	/* as a + (b - a) / 2: a and b are huge when the clock is years off,
	 * their difference is minus the delay */
	tv_sub(t2, t1, &as, &au);
	tv_sub(t3, t4, &bs, &bu);
	s = bs - as;
	u = bu - au;
	tv_norm(&s, &u);
	/* halve s.u (u in 0..999999) */
	odd = s & 1;
	s = (s - odd) / 2;
	u = (u + odd * 1000000) / 2;
	s += as;
	u += au;
	tv_norm(&s, &u);
	*off_sec = s;
	*off_usec = u;
	tv_sub(t4, t1, &as, &au);
	tv_sub(t3, t2, &bs, &bu);
	s = as - bs;
	u = au - bu;
	tv_norm(&s, &u);
	if (s > 2000 || s < -2000)
		return -1;
	*delay_us = s * 1000000 + u;
	return 0;
}

/* ---- the socket ---- */

static void ntp_rx(void *arg);

static void ntp_upcall_fn(struct rt_socket *so, u32 events, void *arg)
{
	if (events & RT_SEV_READ)
		rt_timer_after(&ntp_rx_timer, 0);
}

static int ntp_socket(void)
{
	struct rt_sockaddr_in6 sin;
	int r;

	if (ntp_so)
		return 0;
	ntp_upcall.fn = ntp_upcall_fn;
	r = sock_kernel_create_af(RT_AF_INET6, RT_SOCK_DGRAM, RT_IPPROTO_UDP, &ntp_upcall, &ntp_so);
	if (r < 0)
		return r;
	memset(&sin, 0, sizeof(sin));
	sin.sin6_len = sizeof(sin);
	sin.sin6_family = RT_AF_INET6;
	r = ntp_so->ops->bind(ntp_so, &sin);
	if (r < 0) {
		sock_kernel_close(ntp_so);
		ntp_so = NULL;
	}
	return r;
}

static int ntp_send_to(const rt_in6_addr *a, u32 scope, u16 port, const void *p, u32 len)
{
	struct rt_sockaddr_in6 to;
	struct rt_iov v;

	memset(&to, 0, sizeof(to));
	to.sin6_len = sizeof(to);
	to.sin6_family = RT_AF_INET6;
	to.sin6_port = rt_htons(port);
	to.sin6_addr = *a;
	to.sin6_scope_id = scope;     /* link-local name servers */
	v.base = (void *)p;
	v.len = len;
	return ntp_so->ops->send(ntp_so, &v, 1, 0, &to);
}

/* ---- one sync ---- */

static void sync_next(void);

static void sync_done(int ok, const char *err)
{
	ntp.state = NS_IDLE;
	if (ok) {
		ntp.backoff = 0;
		ntp.last_err[0] = 0;
		rt_timer_after(&ntp_timer, ntp.interval_s * 1000u);
		ntp.next_ms = rt_now_ms() + ntp.interval_s * 1000u;
	} else {
		u32 s = NTP_RETRY_S << (ntp.backoff < 6 ? ntp.backoff : 6);
		if (s > ntp.interval_s)
			s = ntp.interval_s;
		if (ntp.backoff < 255)
			ntp.backoff++;
		rt_strlcpy(ntp.last_err, err ? err : "no answer", sizeof(ntp.last_err));
		RT_DBG(RT_DBG_DNS, "ntp: sync failed (%s), again in %lu s", ntp.last_err, (unsigned long)s);
		rt_timer_after(&ntp_timer, s * 1000u);
		ntp.next_ms = rt_now_ms() + s * 1000u;
	}
}

static void add_addr(const rt_in6_addr *a, const char *name)
{
	int i;

	for (i = 0; i < ntp.naddr; i++)
		if (rt_in6_eq(&ntp.addr[i], a))
			return;
	if (ntp.naddr >= NTP_ADDRS)
		return;
	ntp.addr[ntp.naddr] = *a;
	rt_strlcpy(ntp.addr_name[ntp.naddr], name, sizeof(ntp.addr_name[0]));
	ntp.naddr++;
}

/* the names to use: configured, else DHCP's, else the pool */
static int name_count(void)
{
	return ntp.nnames ? ntp.nnames : ntp.ndhcp ? 0 : 1;
}

static const char *name_at(int i)
{
	return ntp.nnames ? ntp.names[i] : NTP_DEFAULT_POOL;
}

static void dns_send(void)
{
	rt_in6_addr sv[8];
	u32 ifx[8];
	u8 q[300];
	int ns = rt_dns_get_servers6(sv, ifx, NULL, 8), len;

	if (ns > 8)
		ns = 8;
	if (ntp.dns_server >= ns) {
		sync_next();
		return;
	}
	ntp.dns_id = (u16)rt_random32();
	len = dns_build_query(q, sizeof(q), ntp.dns_id, name_at(ntp.name_i), ntp.dns_type);
	if (len < 0 || ntp_send_to(&sv[ntp.dns_server], ifx[ntp.dns_server], DNS_PORT_, q, (u32)len) < 0) {
		ntp.dns_server++;
		rt_timer_after(&ntp_timer, 0);
		return;
	}
	ntp.state = NS_DNS;
	rt_timer_after(&ntp_timer, NTP_TIMEOUT_MS);
}

static void ntp_send(void)
{
	u8 p[48];

	memset(p, 0, sizeof(p));
	p[0] = 0x23;                         /* LI 0, version 4, client */
	rt_random_bytes(ntp.nonce, sizeof(ntp.nonce));
	memcpy(p + 40, ntp.nonce, 8);        /* RFC 4330 allows any value */
	rt_wallclock_tv(&ntp.t1.sec, &ntp.t1.usec);
	if (ntp_send_to(&ntp.addr[ntp.addr_i], 0, NTP_PORT, p, sizeof(p)) < 0) {
		ntp.addr_i++;
		sync_next();
		return;
	}
	RT_DBG(RT_DBG_DNS, "ntp: query %s", ntp.addr_name[ntp.addr_i]);
	ntp.state = NS_QUERY;
	rt_timer_after(&ntp_timer, NTP_TIMEOUT_MS);
}

/* the next step: look up the next name, or ask the next address */
static void sync_next(void)
{
	for (;;) {
		if (ntp.name_i < name_count()) {
			const char *n = name_at(ntp.name_i);
			rt_in6_addr a;
			u32 v4;
			if (rt_parse_ipv4(n, &v4) == 0) {
				rt_in6_set_v4mapped(&a, v4);
				add_addr(&a, n);
			} else if (rt_inet6_pton(n, &a) == 1) {
				add_addr(&a, n);
			} else if (ntp.dns_type == 0) {
				ntp.dns_type = DNS_TYPE_A;
				ntp.dns_server = 0;
				dns_send();
				return;
			} else if (ntp.dns_type == DNS_TYPE_A && ntp.naddr == 0) {
				ntp.dns_type = DNS_TYPE_AAAA;   /* no A record: try AAAA */
				ntp.dns_server = 0;
				dns_send();
				return;
			}
			ntp.name_i++;
			ntp.dns_type = 0;
			continue;
		}
		if (ntp.addr_i < ntp.naddr) {
			ntp_send();
			return;
		}
		sync_done(0, ntp.naddr ? "no valid answer" : "no server address");
		return;
	}
}

static void sync_start(void)
{
	int i;

	if (!ntp.enabled || ntp.state != NS_IDLE)
		return;
	if (ntp_socket() < 0) {
		sync_done(0, "no socket");
		return;
	}
	ntp.naddr = ntp.addr_i = ntp.name_i = 0;
	ntp.dns_type = 0;
	if (!ntp.nnames)
		for (i = 0; i < ntp.ndhcp; i++) {
			rt_in6_addr a;
			char b[16];
			rt_in6_set_v4mapped(&a, ntp.dhcp[i]);
			add_addr(&a, rt_format_ipv4(ntp.dhcp[i], b));
		}
	sync_next();
}

static void ntp_timer_fn(void *arg)
{
	switch (ntp.state) {
	case NS_IDLE:
		sync_start();
		break;
	case NS_DNS:                         /* no answer: next server */
		ntp.dns_server++;
		ntp.state = NS_IDLE;
		dns_send();
		break;
	case NS_QUERY:
		ntp.addr_i++;
		ntp.state = NS_IDLE;
		sync_next();
		break;
	}
}

/* a DNS answer for the name being looked up */
static void dns_answer(const u8 *msg, u32 len)
{
	struct dns_result *res = rt_zalloc(sizeof(*res));
	u32 a4[NTP_ADDRS];
	rt_in6_addr a6[NTP_ADDRS];
	int n, i;

	if (!res)
		return;
	if (ntp.dns_type == DNS_TYPE_A)
		res->addrs = a4;
	else
		res->addrs6 = a6;
	res->max = NTP_ADDRS;
	n = dns_parse_response(msg, len, ntp.dns_id, name_at(ntp.name_i), ntp.dns_type, res);
	rt_free(res);
	if (n == DNS_IGNORE)
		return;
	rt_timer_del(&ntp_timer);
	ntp.state = NS_IDLE;
	for (i = 0; i < n; i++) {
		rt_in6_addr a;
		if (ntp.dns_type == DNS_TYPE_A)
			rt_in6_set_v4mapped(&a, a4[i]);
		else
			a = a6[i];
		add_addr(&a, name_at(ntp.name_i));
	}
	if (n < 0 && n != -RT_HOST_NOT_FOUND && n != 0) {
		ntp.dns_server++;
		dns_send();
		return;
	}
	if (n <= 0 && ntp.dns_type == DNS_TYPE_A) {
		ntp.dns_type = DNS_TYPE_AAAA;
		ntp.dns_server = 0;
		dns_send();
		return;
	}
	ntp.name_i++;
	ntp.dns_type = 0;
	sync_next();
}

static void apply(const struct ntp_tv *t2, const struct ntp_tv *t3, const struct ntp_tv *t4, u8 stratum)
{
	s32 os, ou, d;
	u32 abs_ms;

	if (rt_ntp_compute(&ntp.t1, t2, t3, t4, &os, &ou, &d) < 0 || d < 0 || d > 10000000) {
		ntp.addr_i++;
		sync_next();
		return;
	}
	ntp.synced = 1;
	ntp.stratum = stratum;
	ntp.off_sec = os;
	ntp.off_usec = ou;
	ntp.delay_us = (u32)d;
	ntp.last_ms = rt_now_ms();
	ntp.last_addr = ntp.addr[ntp.addr_i];
	rt_strlcpy(ntp.last_name, ntp.addr_name[ntp.addr_i], sizeof(ntp.last_name));
	ntp.stepped = 0;
	/* |offset| in ms, saturated */
	if (os >= 0)
		abs_ms = os > 2000000 ? 0xffffffffu : (u32)os * 1000u + (u32)ou / 1000u;
	else
		abs_ms = os < -2000000 ? 0xffffffffu : (u32)(-os) * 1000u - (u32)ou / 1000u;
	if (abs_ms >= ntp.step_ms && rt_set_wallclock_hook) {
		struct ntp_tv now;
		s32 s, u;
		rt_wallclock_tv(&now.sec, &now.usec);
		s = (s32)now.sec + os;
		u = (s32)now.usec + ou;
		tv_norm(&s, &u);
		if (rt_set_wallclock_hook((u32)s, (u32)u) == 0)
			ntp.stepped = 1;
	}
	rt_log(RT_LOG_INFO, "ntp: %s stratum %u, offset %ld.%06lu s%s", ntp.last_name, (unsigned)stratum,
	       (long)os, (unsigned long)ou, ntp.stepped ? ", clock set" : "");
	sync_done(1, NULL);
}

static void ntp_answer(const u8 *p, u32 len, const rt_in6_addr *from)
{
	struct ntp_tv t2, t3, t4;
	u8 li, mode, vn, stratum;

	if (len < 48 || !rt_in6_eq(from, &ntp.addr[ntp.addr_i]))
		return;
	if (memcmp(p + 24, ntp.nonce, 8))
		return;                         /* not the answer to our query */
	rt_wallclock_tv(&t4.sec, &t4.usec);
	li = p[0] >> 6;
	vn = (p[0] >> 3) & 7;
	mode = p[0] & 7;
	stratum = p[1];
	rt_timer_del(&ntp_timer);
	ntp.state = NS_IDLE;
	if (stratum == 0) {
		/* kiss-o'-death: RATE slows us down, DENY/RSTR drop the server */
		char code[5];
		memcpy(code, p + 12, 4);
		code[4] = 0;
		rt_log(RT_LOG_WARN, "ntp: %s says %s", ntp.addr_name[ntp.addr_i], code);
		if (!memcmp(code, "RATE", 4) && ntp.interval_s < 36 * 3600u)
			ntp.interval_s *= 2;
		ntp.addr_i++;
		sync_next();
		return;
	}
	if (mode != 4 || vn < 3 || vn > 4 || li == 3 || stratum > 15 ||
	    rt_get_be32(p + 4) >= (16u << 16) || rt_get_be32(p + 8) >= (16u << 16) ||
	    !(rt_get_be32(p + 40) | rt_get_be32(p + 44))) {
		ntp.addr_i++;
		sync_next();
		return;
	}
	rt_ntp_ts_to_tv(p + 32, &t2);
	rt_ntp_ts_to_tv(p + 40, &t3);
	apply(&t2, &t3, &t4, stratum);
}

static void ntp_rx(void *arg)
{
	struct rt_sockaddr_in6 from;
	struct rt_iov v;
	int r, mf;

	while (ntp_so) {
		v.base = ntp_buf;
		v.len = sizeof(ntp_buf);
		mf = 0;
		memset(&from, 0, sizeof(from));
		r = ntp_so->ops->recv(ntp_so, &v, 1, RT_MSG_DONTWAIT, &from, &mf);
		if (r < 0)
			break;
		if (ntp.state == NS_DNS && from.sin6_port == rt_htons(DNS_PORT_))
			dns_answer(ntp_buf, (u32)r);
		else if (ntp.state == NS_QUERY && from.sin6_port == rt_htons(NTP_PORT))
			ntp_answer(ntp_buf, (u32)r, &from.sin6_addr);
	}
}

/* ---- configuration and status ---- */

void rt_ntp_dhcp_servers(const struct rt_netif *nif, const u32 *addrs, int n)
{
	int i, changed = n != ntp.ndhcp;

	if (n > 4)
		n = 4;
	for (i = 0; i < n; i++) {
		if (ntp.dhcp[i] != addrs[i])
			changed = 1;
		ntp.dhcp[i] = addrs[i];
	}
	ntp.ndhcp = n;
	/* a lease with servers: sync soon when they are the ones in use */
	if (changed && n && ntp.enabled && !ntp.nnames && ntp.state == NS_IDLE)
		rt_timer_after(&ntp_timer, 1000);
}

static void fmt_off(char *b, u32 len, s32 s, s32 u)
{
	/* s.u with u in 0..999999: print the signed value in ms */
	if (s < 0 && u) {
		s++;
		u = 1000000 - u;
		rt_snprintf(b, len, "-%ld.%03lu s", (long)(-s), (unsigned long)(u / 1000));
	} else if (s < 0) {
		rt_snprintf(b, len, "-%ld.000 s", (long)(-s));
	} else {
		rt_snprintf(b, len, "+%ld.%03lu s", (long)s, (unsigned long)(u / 1000));
	}
}

void rt_ntp_status(char *buf, u32 len)
{
	char off[32];
	s32 next = (s32)(ntp.next_ms - rt_now_ms());

	buf[0] = 0;
	if (!ntp.enabled)
		return;
	if (next < 0)
		next = 0;
	if (ntp.synced) {
		fmt_off(off, sizeof(off), ntp.off_sec, ntp.off_usec);
		rt_snprintf(buf, len, "NTP: %s, stratum %u, offset %s%s, delay %lu ms, %lu s ago, next in %lu s%s%s",
		            ntp.last_name, (unsigned)ntp.stratum, off, ntp.stepped ? " (clock set)" : "",
		            (unsigned long)(ntp.delay_us / 1000u), (unsigned long)((rt_now_ms() - ntp.last_ms) / 1000u),
		            (unsigned long)((u32)next / 1000u), ntp.last_err[0] ? "; last try: " : "", ntp.last_err);
	} else {
		rt_snprintf(buf, len, "NTP: not synchronised yet%s%s, next try in %lu s",
		            ntp.last_err[0] ? ", " : "", ntp.last_err, (unsigned long)((u32)next / 1000u));
	}
}

static void ntp_enable(int on)
{
	ntp.enabled = (u8)on;
	if (!on) {
		rt_timer_del(&ntp_timer);
		ntp.state = NS_IDLE;
		if (ntp_so) {
			rt_timer_del(&ntp_rx_timer);
			sock_kernel_close(ntp_so);
			ntp_so = NULL;
		}
		return;
	}
	if (ntp.state == NS_IDLE) {
		/* give the interfaces a moment to come up */
		rt_timer_after(&ntp_timer, 2000);
		ntp.next_ms = rt_now_ms() + 2000;
	}
}

int rt_ntp_command(int argc, char **argv, char *out, u32 outlen)
{
	static const char usage[] = "usage: ntp SERVER... | on | off | sync | status | interval SECONDS | "
	                            "step MS | battclock on|off";
	const char *a = argc > 1 ? argv[1] : "status";
	u32 v;
	int i;
	const char *e;

	out[0] = 0;
	if (!rt_strcasecmp(a, "status")) {
		rt_ntp_status(out, outlen);
		if (!out[0])
			rt_strlcpy(out, "NTP: off. \"Riptide NTP ON\" uses the DHCP lease's servers (else pool.ntp.org),\n"
			           "\"Riptide NTP SERVER...\" names them; to keep it, add \"ntp on\" to riptide.conf\n"
			           "(RiptidePrefs: Time page).", outlen);
		return 0;
	}
	if (!rt_strcasecmp(a, "off")) {
		ntp_enable(0);
		rt_strlcpy(out, "NTP: off", outlen);
		return 0;
	}
	if (!rt_strcasecmp(a, "on")) {
		ntp_enable(1);
		rt_strlcpy(out, ntp.nnames ? "NTP: on, first sync in 2 s (Riptide NTP STATUS shows it)" :
		           "NTP: on, servers from the DHCP lease or pool.ntp.org; first sync in 2 s", outlen);
		return 0;
	}
	if (!rt_strcasecmp(a, "sync")) {
		if (!ntp.enabled)
			ntp_enable(1);
		if (ntp.state == NS_IDLE) {
			ntp.backoff = 0;
			rt_timer_after(&ntp_timer, 0);
			rt_strlcpy(out, "NTP: syncing now (Riptide NTP STATUS shows the result)", outlen);
		} else {
			rt_strlcpy(out, "NTP: a sync is already running", outlen);
		}
		return 0;
	}
	if (!rt_strcasecmp(a, "interval") || !rt_strcasecmp(a, "step")) {
		if (argc != 3)
			goto bad;
		v = (u32)rt_strtoul(argv[2], &e, 10);
		if (*e || !*argv[2])
			goto bad;
		if ((a[0] | 0x20) == 'i') {
			if (v < 64 || v > 7 * 86400u) {
				rt_strlcpy(out, "ntp: interval is 64..604800 seconds", outlen);
				return -RT_EINVAL;
			}
			ntp.interval_s = v;
		} else {
			ntp.step_ms = v;
		}
		rt_snprintf(out, outlen, "NTP: %s %lu %s", (a[0] | 0x20) == 'i' ? "interval" : "step",
		            (unsigned long)v, (a[0] | 0x20) == 'i' ? "s" : "ms");
		return 0;
	}
	if (!rt_strcasecmp(a, "battclock")) {
		if (argc != 3 || (rt_strcasecmp(argv[2], "on") && rt_strcasecmp(argv[2], "off")))
			goto bad;
		rt_ntp_battclock = (argv[2][1] | 0x20) == 'n';
		rt_snprintf(out, outlen, "NTP: battclock %s", rt_ntp_battclock ? "on" : "off");
		return 0;
	}
	/* server list */
	if (argc - 1 > NTP_NAMES) {
		rt_snprintf(out, outlen, "ntp: at most %d servers", NTP_NAMES);
		return -RT_EINVAL;
	}
	for (i = 1; i < argc; i++)
		if (strlen(argv[i]) >= sizeof(ntp.names[0]) || argv[i][0] == '-')
			goto bad;
	for (i = 1; i < argc; i++)
		rt_strlcpy(ntp.names[i - 1], argv[i], sizeof(ntp.names[0]));
	ntp.nnames = argc - 1;
	if (ntp.state != NS_IDLE) {
		rt_timer_del(&ntp_timer);
		ntp.state = NS_IDLE;
	}
	ntp.enabled = 0;
	ntp_enable(1);
	rt_snprintf(out, outlen, "NTP: %lu server%s, first sync in 2 s", (unsigned long)ntp.nnames,
	            ntp.nnames == 1 ? "" : "s");
	return 0;
bad:
	rt_strlcpy(out, usage, outlen);
	return -RT_EINVAL;
}

/* "timezone +10:00", "timezone -5", "timezone auto" */
int rt_tz_command(int argc, char **argv, char *out, u32 outlen)
{
	const char *s, *e;
	u32 h, m = 0;
	int neg;

	out[0] = 0;
	if (argc != 2) {
		if (argc == 1) {
			s32 o = rt_tz_offset < 0 ? -rt_tz_offset : rt_tz_offset;
			rt_snprintf(out, outlen, "timezone %c%02lu:%02lu%s", rt_tz_offset < 0 ? '-' : '+',
			            (unsigned long)(o / 3600), (unsigned long)(o / 60 % 60),
			            rt_tz_configured ? "" : " (from the system)");
			return 0;
		}
		goto bad;
	}
	s = argv[1];
	if (!rt_strcasecmp(s, "auto")) {
		rt_tz_configured = 0;
		rt_tz_offset = rt_tz_system;
		return 0;
	}
	neg = *s == '-';
	if (*s == '+' || *s == '-')
		s++;
	h = (u32)rt_strtoul(s, &e, 10);
	if (e == s)
		goto bad;
	if (*e == ':') {
		s = e + 1;
		m = (u32)rt_strtoul(s, &e, 10);
		if (e == s)
			goto bad;
	}
	if (*e || h > 14 || m > 59)
		goto bad;
	rt_tz_offset = (s32)(h * 3600 + m * 60);
	if (neg)
		rt_tz_offset = -rt_tz_offset;
	rt_tz_configured = 1;
	return 0;
bad:
	rt_strlcpy(out, "usage: timezone [+-]HH[:MM] | auto   (local time = UTC + offset)", outlen);
	return -RT_EINVAL;
}

void rt_netsvc_init(void);   /* also starts mDNS (mdns.c) */
void rt_mdns_init(void);

void rt_netsvc_init(void)
{
	ntp.interval_s = 3600;
	ntp.step_ms = 500;
	rt_timer_init(&ntp_timer, ntp_timer_fn, NULL);
	rt_timer_init(&ntp_rx_timer, ntp_rx, NULL);
	rt_mdns_init();
}
