/*
 * Riptide - TCP unit and robustness tests: integer cube root (CUBIC),
 * option parsing incl. malformed options, SYN cookie encode/decode, and
 * a fuzzer that feeds thousands of random (partly in-window) segments
 * and ICMP errors into rt_tcp_input / rt_tcp_icmp_error for a listener
 * and a live connection. Run under `make SAN=1` to catch memory errors.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "t_tcp_common.h"
#include "../src/core/tcp.h"

#define LO 0x7f000001u

static u32 rnd_state = 12345;
static u32 rnd(void)
{
	rnd_state ^= rnd_state << 13;
	rnd_state ^= rnd_state >> 17;
	rnd_state ^= rnd_state << 5;
	return rnd_state;
}

static void test_cubic_root(void)
{
	u64 a;
	u32 i, bad = 0;

	for (a = 0; a < 70000; a++) {
		u32 r = tcp_cubic_root(a);
		if ((u64)r * r * r > a || (u64)(r + 1) * (r + 1) * (r + 1) <= a)
			bad++;
	}
	for (i = 0; i < 200000; i++) {
		u64 x = ((u64)rnd() << 32 | rnd()) >> (rnd() % 64);
		u32 r;
		if (x > ((u64)1 << 62))
			continue;
		r = tcp_cubic_root(x);
		if ((u64)r * r * r > x || (u64)(r + 1) * (r + 1) * (r + 1) <= x)
			bad++;
	}
	CHECKV(bad == 0, "bad %u", bad);
}

static void test_options(void)
{
	u8 th[60];
	struct tcp_rx_opt o;
	int i, n;

	/* well formed SYN: MSS 1400, SACK_PERM, TS, NOP, WS 7 */
	memset(th, 0, sizeof(th));
	th[13] = TCPHDR_SYN;
	{
		static const u8 opt[] = { 2, 4, 0x05, 0x78, 4, 2, 8, 10, 0, 0, 0, 5, 0, 0, 0, 0,
		                          1, 3, 3, 7 };
		memcpy(th + 20, opt, sizeof(opt));
	}
	memset(&o, 0, sizeof(o));
	o.mss_clamp = 536;
	tcp_parse_options(th, 40, &o, 0);
	CHECK(o.mss_clamp == 1400 && o.sack_ok && o.saw_tstamp && o.rcv_tsval == 5 &&
	      o.wscale_ok && o.snd_wscale == 7);

	/* window scale > 14 is clamped (RFC 7323 2.3) */
	th[39] = 20;
	memset(&o, 0, sizeof(o));
	tcp_parse_options(th, 40, &o, 0);
	CHECK(o.snd_wscale == 14);

	/* user MSS clamps peer MSS */
	memset(&o, 0, sizeof(o));
	o.user_mss = 1000;
	tcp_parse_options(th, 40, &o, 0);
	CHECK(o.mss_clamp == 1000);

	/* zero-length option must stop parsing, not loop */
	memset(th + 20, 0, 40);
	th[20] = 2;
	th[21] = 0;
	memset(&o, 0, sizeof(o));
	tcp_parse_options(th, 40, &o, 0);
	CHECK(o.mss_clamp == 0);

	/* option overrunning the header is ignored */
	th[20] = 8;
	th[21] = 10;
	memset(&o, 0, sizeof(o));
	tcp_parse_options(th, 24, &o, 0);
	CHECK(!o.saw_tstamp);

	/* wrong sized timestamp / MSS ignored; options only on SYN */
	th[20] = 8; th[21] = 9;
	memset(&o, 0, sizeof(o));
	tcp_parse_options(th, 40, &o, 0);
	CHECK(!o.saw_tstamp);
	th[13] = TCPHDR_ACK;
	th[20] = 2; th[21] = 4; th[22] = 1; th[23] = 0;
	memset(&o, 0, sizeof(o));
	tcp_parse_options(th, 24, &o, 0);
	CHECK(o.mss_clamp == 0);

	/* SACK block offset recorded only when negotiated and sane */
	th[20] = 1; th[21] = 1; th[22] = 5; th[23] = 10;
	memset(&o, 0, sizeof(o));
	o.sack_ok = 1;
	tcp_parse_options(th, 40, &o, 1);
	CHECK(o.sack_off == 22);
	th[23] = 11;
	memset(&o, 0, sizeof(o));
	o.sack_ok = 1;
	tcp_parse_options(th, 40, &o, 1);
	CHECK(o.sack_off == 0);

	/* random garbage never reads past hdr_len (ASan checks) */
	for (n = 0; n < 20000; n++) {
		u32 hl = 20 + 4 * (rnd() % 11);
		u8 *buf = malloc(hl);
		for (i = 0; i < (int)hl; i++)
			buf[i] = (u8)rnd();
		buf[13] = (u8)rnd();
		memset(&o, 0, sizeof(o));
		o.sack_ok = rnd() & 1;
		o.tstamp_ok = rnd() & 1;
		tcp_parse_options(buf, hl, &o, (int)(rnd() & 1));
		CHECK(o.sack_off == 0 || o.sack_off + 2u <= hl);
		free(buf);
	}
}

static rt_in6_addr m4(u32 a)
{
	rt_in6_addr x;
	rt_in6_set_v4mapped(&x, a);
	return x;
}

static void test_cookies(void)
{
	rt_in6_addr L = m4(rt_htonl(0x0a000001)), R = m4(rt_htonl(0x0a000002));
	rt_in6_addr L6, R6;
	const rt_in6_addr *l = &L, *r = &R;
	u32 isn;
	u16 lp = rt_htons(80), rp = rt_htons(40000), mss;
	int i;

	rt_lock();
	for (i = 0; i < 4; i++) {
		mss = (u16)(tcp_msstab[i] + 3);
		isn = tcp_cookie_init_seq(l, r, lp, rp, 1000, &mss);
		CHECK(mss == tcp_msstab[i]);
		CHECK(tcp_cookie_check(isn, l, r, lp, rp, 1000) == i);
		CHECK(tcp_cookie_check(isn ^ 0x00f0f000u, l, r, lp, rp, 1000) < 0);
		CHECK(tcp_cookie_check(isn, l, r, lp, rp, 1000 + 0x123456) < 0);
		CHECK(tcp_cookie_check(isn, l, r, lp, rt_htons(40001), 1000) < 0);
	}
	/* IPv6 address pairs hash all 128 bits */
	CHECK(rt_inet6_pton("2001:db8::1", &L6) == 0 && rt_inet6_pton("2001:db8::2", &R6) == 0);
	mss = 1440;
	isn = tcp_cookie_init_seq(&L6, &R6, lp, rp, 7, &mss);
	CHECK(mss == 1440 && tcp_cookie_check(isn, &L6, &R6, lp, rp, 7) == 2);
	R6.b[3] ^= 1;          /* differs only in the high bits */
	CHECK(tcp_cookie_check(isn, &L6, &R6, lp, rp, 7) < 0);
	rt_unlock();
}

/* ---- fuzzing ---- */
struct target {
	u32 saddr, daddr;    /* network order: segment source / destination */
	u16 sport, dport;
	struct tcp_sock *tp; /* receiving connection (for in-window values) */
};

static struct tcp_sock *lookup(const struct target *t)
{
	rt_in6_addr d = m4(t->daddr), sa = m4(t->saddr);
	return tcp_lookup_established(&d, t->dport, &sa, t->sport);
}

static void inject(const struct target *t, int mode)
{
	struct rt_iphdr ip;
	struct rt_buf *b;
	u32 optlen = (rnd() % 3) ? 4 * (rnd() % 11) : 0;
	u32 plen = (rnd() % 4) ? 0 : rnd() % 1600;
	u32 len = 20 + optlen + plen, i;
	u8 *th;
	u32 seq = rnd(), ack = rnd();

	b = rt_buf_alloc(RT_BUF_HEADROOM, len);
	if (!b)
		return;
	th = rt_buf_put(b, len);
	for (i = 0; i < len; i++)
		th[i] = (u8)rnd();
	memcpy(th, &t->sport, 2);
	memcpy(th + 2, &t->dport, 2);
	if (t->tp && t->tp->state != RT_TCPS_CLOSED && mode) {
		/* near the window edges */
		seq = t->tp->rcv_nxt + (rnd() % 3 ? (rnd() % 4096) : (u32)-(s32)(rnd() % 4096));
		ack = t->tp->snd_una + (rnd() % 2 ? (rnd() % 64) : (u32)-(s32)(rnd() % 64));
	}
	rt_put_be32(th + 4, seq);
	rt_put_be32(th + 8, ack);
	th[12] = (u8)(((20 + optlen) / 4) << 4);
	if (rnd() % 16 == 0)
		th[12] = (u8)rnd();          /* broken data offset */
	th[13] = (u8)rnd();
	if (rnd() % 4 == 0) {
		/* well-formed-ish options with random content */
		u8 *o = th + 20;
		u32 k = 0;
		while (k + 2 <= optlen) {
			u8 kind = (u8)(rnd() % 10), l2 = (u8)(rnd() % 12);
			o[k] = kind;
			o[k + 1] = l2;
			k += l2 > 1 ? l2 : 1;
		}
	}
	memset(&ip, 0, sizeof(ip));
	ip.ver_ihl = 0x45;
	ip.protocol = RT_IPPROTO_TCP;
	ip.tos = (u8)rnd();
	ip.saddr = t->saddr;
	ip.daddr = t->daddr;
	b->flags = (rnd() % 8) ? RT_BUFF_CSUM_OK : 0;
	rt_tcp_input(b, &ip);
}

static void fuzz_icmp(const struct target *t)
{
	u8 q[60];
	struct rt_iphdr *ip = (struct rt_iphdr *)(void *)q;
	u8 types[] = { 3, 4, 5, 11, 12, 0, 8 };
	u32 i;

	for (i = 0; i < sizeof(q); i++)
		q[i] = (u8)rnd();
	ip->ver_ihl = (rnd() % 4) ? 0x45 : (u8)rnd();
	/* the quoted packet is ours: source = receiving side */
	ip->saddr = t->daddr;
	ip->daddr = t->saddr;
	memcpy(q + 20, &t->dport, 2);
	memcpy(q + 22, &t->sport, 2);
	if (t->tp && rnd() % 2)
		rt_put_be32(q + 24, t->tp->snd_una + rnd() % 2000);
	rt_tcp_icmp_error(types[rnd() % sizeof(types)], (u8)(rnd() % 16),
	                  (rnd() % 2) ? rnd() % 2000 : rnd(), ip, 20 + rnd() % 41);
}

static struct rt_sockctx cs, ss;

static void test_fuzz(void)
{
	struct rt_sockaddr_in a = t_sa(0, 6000), d = t_sa(LO, 6000), me;
	struct target tg[4];
	int lfd, cfd, afd, i, r, n = 0;
	u32 alen = sizeof(me);
	char buf[256];

	lfd = rt_api_socket(&ss, RT_AF_INET, RT_SOCK_STREAM, 0);
	CHECK(rt_api_bind(&ss, lfd, &a, sizeof(a)) == 0);
	CHECK(rt_api_listen(&ss, lfd, 8) == 0);
	cfd = rt_api_socket(&cs, RT_AF_INET, RT_SOCK_STREAM, 0);
	CHECK(rt_api_connect(&cs, cfd, &d, sizeof(d)) == 0);
	afd = rt_api_accept(&ss, lfd, NULL, NULL);
	CHECK(afd >= 0);
	rt_api_getsockname(&cs, cfd, &me, &alen);

	rt_lock();
	/* listener, server side of the connection, client side, nobody */
	tg[0].saddr = rt_htonl(0x7f000002); tg[0].sport = rt_htons(1234);
	tg[0].daddr = rt_htonl(LO); tg[0].dport = rt_htons(6000); tg[0].tp = NULL;
	tg[1].saddr = rt_htonl(LO); tg[1].sport = me.sin_port;
	tg[1].daddr = rt_htonl(LO); tg[1].dport = rt_htons(6000);
	tg[1].tp = lookup(&tg[1]);
	tg[2].saddr = rt_htonl(LO); tg[2].sport = rt_htons(6000);
	tg[2].daddr = rt_htonl(LO); tg[2].dport = me.sin_port;
	tg[2].tp = lookup(&tg[2]);
	tg[3].saddr = rt_htonl(0x7f000003); tg[3].sport = rt_htons(999);
	tg[3].daddr = rt_htonl(LO); tg[3].dport = rt_htons(6001); tg[3].tp = NULL;
	CHECK(tg[1].tp != NULL && tg[2].tp != NULL);
	rt_unlock();

	for (i = 0; i < 20000; i++) {
		struct target *t = &tg[rnd() % 4];
		rt_lock();
		/* the connection may be reset by the fuzz: re-look it up */
		if (t == &tg[1] || t == &tg[2])
			t->tp = lookup(t);
		if (rnd() % 10 == 0)
			fuzz_icmp(t);
		else
			inject(t, (int)(rnd() % 4 != 0));
		rt_unlock();
		if (i % 500 == 0) {
			/* keep real traffic flowing too */
			t_set_nbio(&cs, cfd, 1);
			t_set_nbio(&ss, afd, 1);
			rt_api_send(&cs, cfd, "ping", 4, 0);
			while ((r = rt_api_recv(&ss, afd, buf, sizeof(buf), 0)) > 0)
				n += r;
			usleep(1000);
		}
	}
	printf("fuzz: 20000 segments injected, %d bytes of real traffic received, "
	       "challenge ACKs %u, resets sent %u\n", n, rt_tcpstat.TCPChallengeACK, rt_tcpstat.OutRsts);
	rt_api_close(&cs, cfd);
	rt_api_close(&ss, afd);
	rt_api_close(&ss, lfd);
	/* the stack must still work afterwards */
	{
		struct rt_sockaddr_in a2 = t_sa(0, 6002), d2 = t_sa(LO, 6002);
		lfd = rt_api_socket(&ss, RT_AF_INET, RT_SOCK_STREAM, 0);
		CHECK(rt_api_bind(&ss, lfd, &a2, sizeof(a2)) == 0);
		CHECK(rt_api_listen(&ss, lfd, 8) == 0);
		cfd = rt_api_socket(&cs, RT_AF_INET, RT_SOCK_STREAM, 0);
		CHECK(rt_api_connect(&cs, cfd, &d2, sizeof(d2)) == 0);
		afd = rt_api_accept(&ss, lfd, NULL, NULL);
		CHECK(afd >= 0);
		CHECK(rt_api_send(&cs, cfd, "hello", 5, 0) == 5);
		t_set_nbio(&ss, afd, 0);
		CHECK(rt_api_recv(&ss, afd, buf, 5, RT_MSG_WAITALL) == 5);
		rt_api_close(&cs, cfd);
		rt_api_close(&ss, afd);
		rt_api_close(&ss, lfd);
	}
}

int main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	alarm(300);
	test_cubic_root();
	test_options();
	if (rt_host_start() != 0)
		return 1;
	rt_ctx_init(&cs, 16, NULL);
	rt_ctx_init(&ss, 16, NULL);
	test_cookies();
	test_fuzz();
	rt_ctx_destroy(&cs);
	rt_ctx_destroy(&ss);
	rt_host_stop();
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
