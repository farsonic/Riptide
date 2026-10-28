/*
 * Riptide - ECN (RFC 3168) on TCP: the ECN-setup SYN of a client (default
 * net.inet.tcp.ecn=1), the answers that turn it on or off (a SYN-ACK with
 * ECE only; without ECE; with ECE and CWR from a reflecting middlebox),
 * the fallback of a retransmitted SYN (net.inet.tcp.ecn_fallback), ECT(0)
 * on new data only (not on ACKs, the SYN or retransmissions), the CE ->
 * ECE -> CWR loop in both directions, and IP_TOS keeping the ECN bits.
 * The peer is scripted: segments are captured on a fake point-to-point
 * interface and the answers injected with rt_tcp_input(), on the test
 * clock.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "t_tcp_common.h"
#include "../src/core/tcp.h"

#define ME   0x0a090001u    /* 10.9.0.1 */
#define PEER 0x0a090002u    /* 10.9.0.2 */
#define PEER_ISN 0x10000000u

#define F_FIN 0x01
#define F_SYN 0x02
#define F_RST 0x04
#define F_PSH 0x08
#define F_ACK 0x10
#define F_ECE 0x40
#define F_CWR 0x80

static struct rt_netif tun;
static struct rt_bufq capq;
static struct rt_sockctx ctx;

static int fake_output(struct rt_netif *nif, struct rt_buf *b, const u8 *dsthw, u16 et)
{
	(void)nif;
	(void)dsthw;
	(void)et;
	rt_bufq_tail(&capq, b);
	return 0;
}

static void advance(u32 ms)
{
	while (ms) {
		u32 step = ms > 20 ? 20 : ms;
		rt_host_advance_clock(step);
		rt_lock();
		rt_stack_poll();
		rt_unlock();
		ms -= step;
	}
	rt_lock();
	rt_stack_poll();
	rt_unlock();
}

/* a captured TCP segment */
struct seg {
	u8 tos, flags;
	u16 sport, dport;
	u32 seq, ack, len;
};

/* segments of other (closed, still retransmitting) connections are
 * skipped: only those from local port want_port (0: any) count */
static u16 want_port;

static int take(struct seg *s)
{
	struct rt_buf *b;

again:
	rt_lock();
	b = rt_bufq_dequeue(&capq);
	rt_unlock();
	if (!b)
		return 0;
	memset(s, 0, sizeof(*s));
	if (b->len >= 40 && (b->data[0] >> 4) == 4 && b->data[9] == RT_IPPROTO_TCP) {
		u32 ihl = (u32)(b->data[0] & 15) * 4, doff;
		const u8 *th = b->data + ihl;
		s->tos = b->data[1];
		s->sport = rt_get_be16(th);
		s->dport = rt_get_be16(th + 2);
		s->seq = rt_get_be32(th + 4);
		s->ack = rt_get_be32(th + 8);
		doff = (u32)(th[12] >> 4) * 4;
		s->flags = th[13];
		s->len = rt_get_be16(b->data + 2) - ihl - doff;
	}
	rt_buf_free(b);
	if (want_port && s->sport != want_port)
		goto again;
	return 1;
}

static void drain(void)
{
	struct seg s;
	while (take(&s))
		;
}

/* the last captured segment (the others dropped); 0 if none */
static int last(struct seg *s)
{
	struct seg t;
	int n = 0;
	while (take(&t)) {
		*s = t;
		n++;
	}
	return n;
}

/* a segment from the peer: flags, seq, ack, tos, len bytes of payload */
static void inject(u16 sport, u16 dport, u8 flags, u32 seq, u32 ack, u8 tos, u32 len)
{
	struct rt_iphdr ip;
	struct rt_buf *b = rt_buf_alloc(RT_BUF_HEADROOM, 24 + len);
	u8 *th;

	if (!b)
		return;
	th = rt_buf_put(b, 20 + len + ((flags & F_SYN) ? 4 : 0));
	memset(th, 0, b->len);
	rt_put_be16(th, sport);
	rt_put_be16(th + 2, dport);
	rt_put_be32(th + 4, seq);
	rt_put_be32(th + 8, ack);
	th[13] = flags;
	rt_put_be16(th + 14, 65535);
	if (flags & F_SYN) {
		/* MSS 1400 */
		th[12] = 6 << 4;
		th[20] = 2;
		th[21] = 4;
		rt_put_be16(th + 22, 1400);
	} else {
		th[12] = 5 << 4;
	}
	memset(&ip, 0, sizeof(ip));
	ip.ver_ihl = 0x45;
	ip.protocol = RT_IPPROTO_TCP;
	ip.tos = tos;
	ip.ttl = 64;
	ip.saddr = rt_htonl(PEER);
	ip.daddr = rt_htonl(ME);
	b->flags = RT_BUFF_CSUM_OK;
	rt_lock();
	rt_tcp_input(b, &ip);
	rt_unlock();
}

static struct tcp_sock *lookup(u16 lport, u16 rport)
{
	rt_in6_addr l, r;
	struct tcp_sock *tp;

	rt_in6_set_v4mapped(&l, rt_htonl(ME));
	rt_in6_set_v4mapped(&r, rt_htonl(PEER));
	rt_lock();
	tp = tcp_lookup_established(&l, rt_htons(lport), &r, rt_htons(rport));
	rt_unlock();
	return tp;
}

/* a connection to PEER:port: its SYN in *syn, the socket returned */
static int start(u16 port, struct seg *syn)
{
	struct rt_sockaddr_in d = t_sa(PEER, port);
	int fd = rt_api_socket(&ctx, RT_AF_INET, RT_SOCK_STREAM, 0);

	CHECK(fd >= 0);
	t_set_nbio(&ctx, fd, 1);
	want_port = 0;
	drain();
	CHECK(rt_api_connect(&ctx, fd, &d, sizeof(d)) == -RT_EINPROGRESS);
	advance(1);
	memset(syn, 0, sizeof(*syn));
	while (take(syn) && !(syn->dport == port && syn->flags & F_SYN))
		;
	CHECK(syn->dport == port && (syn->flags & F_SYN));
	want_port = syn->sport;
	return fd;
}

static int has_ecn(int fd)
{
	struct rt_tcp_info ti;
	t_get_info(&ctx, fd, &ti);
	return (ti.tcpi_options & 8) != 0;
}

/* SYN-ACK with synack_flags, then the peer sends nothing more */
static int open_with(u16 port, u8 synack_flags, struct seg *syn)
{
	int fd = start(port, syn);
	inject(port, syn->sport, (u8)(F_SYN | F_ACK | synack_flags), PEER_ISN, syn->seq + 1, 0, 0);
	advance(1);
	return fd;
}

static void close_fd(int fd)
{
	rt_api_close(&ctx, fd);
	advance(1);
	drain();
}

static void test_negotiation(void)
{
	struct seg syn, s;
	u32 neg0 = rt_tcpstat.TCPECNNegotiated, na0 = rt_tcpstat.TCPECNNotAgreed;
	int fd;

	/* default: request */
	fd = open_with(9001, F_ECE, &syn);
	CHECKV((syn.flags & (F_ECE | F_CWR)) == (F_ECE | F_CWR), "syn flags %x", syn.flags);
	CHECKV((syn.tos & 3) == 0, "syn tos %x", syn.tos);   /* the SYN is not ECT */
	CHECK(has_ecn(fd));
	CHECK(rt_tcpstat.TCPECNNegotiated == neg0 + 1);
	/* the ACK of the SYN-ACK: no ECT, no ECE/CWR */
	CHECK(last(&s) >= 1 && (s.flags & F_ACK) && !(s.flags & (F_ECE | F_CWR | F_SYN)) && !(s.tos & 3));
	/* new data: ECT(0) */
	CHECK(rt_api_send(&ctx, fd, "hello", 5, 0) == 5);
	advance(1);
	CHECKV(last(&s) == 1 && s.len == 5 && (s.tos & 3) == 2, "len %u tos %x", s.len, s.tos);
	CHECK(!(s.flags & (F_ECE | F_CWR)));
	/* a retransmission of it: not ECT */
	advance(1500);
	CHECKV(last(&s) >= 1 && s.len == 5 && (s.tos & 3) == 0, "rexmit len %u tos %x", s.len, s.tos);
	close_fd(fd);

	/* SYN-ACK without ECE: no ECN */
	fd = open_with(9002, 0, &syn);
	CHECK(!has_ecn(fd) && rt_tcpstat.TCPECNNotAgreed == na0 + 1);
	drain();
	CHECK(rt_api_send(&ctx, fd, "x", 1, 0) == 1);
	advance(1);
	CHECK(last(&s) == 1 && s.len == 1 && (s.tos & 3) == 0);
	close_fd(fd);

	/* ECE and CWR: a middlebox reflecting the SYN's flags */
	fd = open_with(9003, F_ECE | F_CWR, &syn);
	CHECK(!has_ecn(fd) && rt_tcpstat.TCPECNNotAgreed == na0 + 2);
	drain();
	CHECK(rt_api_send(&ctx, fd, "x", 1, 0) == 1);
	advance(1);
	CHECK(last(&s) == 1 && s.len == 1 && (s.tos & 3) == 0);
	close_fd(fd);

	/* accept only: no ECN-setup SYN */
	t_sysctl("net.inet.tcp.ecn", 2);
	fd = open_with(9004, F_ECE, &syn);
	CHECK(!(syn.flags & (F_ECE | F_CWR)) && !has_ecn(fd));
	close_fd(fd);
	t_sysctl("net.inet.tcp.ecn", 0);
	fd = open_with(9005, F_ECE, &syn);
	CHECK(!(syn.flags & (F_ECE | F_CWR)) && !has_ecn(fd));
	close_fd(fd);
	t_sysctl("net.inet.tcp.ecn", 1);
}

static void test_fallback(void)
{
	struct seg syn, s;
	u32 fb0 = rt_tcpstat.TCPECNFallback;
	int fd;

	/* the SYN is lost: the retransmission goes without ECN */
	fd = start(9011, &syn);
	CHECK((syn.flags & (F_ECE | F_CWR)) == (F_ECE | F_CWR));
	advance(1100);
	CHECKV(last(&s) >= 1 && (s.flags & F_SYN) && !(s.flags & (F_ECE | F_CWR)), "flags %x", s.flags);
	CHECK(rt_tcpstat.TCPECNFallback == fb0 + 1);
	/* even an ECN-setup SYN-ACK does not turn it on again */
	inject(9011, syn.sport, F_SYN | F_ACK | F_ECE, PEER_ISN, syn.seq + 1, 0, 0);
	advance(1);
	CHECK(!has_ecn(fd));
	close_fd(fd);

	/* net.inet.tcp.ecn_fallback=0: the retransmission keeps ECE and CWR */
	t_sysctl("net.inet.tcp.ecn_fallback", 0);
	fd = start(9012, &syn);
	advance(1100);
	CHECK(last(&s) >= 1 && (s.flags & F_SYN) && (s.flags & (F_ECE | F_CWR)) == (F_ECE | F_CWR));
	CHECK(rt_tcpstat.TCPECNFallback == fb0 + 1);
	inject(9012, syn.sport, F_SYN | F_ACK | F_ECE, PEER_ISN, syn.seq + 1, 0, 0);
	advance(1);
	CHECK(has_ecn(fd));
	close_fd(fd);
	t_sysctl("net.inet.tcp.ecn_fallback", 1);
}

/* CE on data received: ECE on every ACK until the peer's CWR */
static void test_ce_echo(void)
{
	struct seg syn, s;
	u32 marks0 = rt_tcpstat.TCPECNMarks, rseq = PEER_ISN + 1;
	char buf[64];
	int fd;

	fd = open_with(9021, F_ECE, &syn);
	drain();
	/* ECT(0) data, no CE: plain ACK */
	inject(9021, syn.sport, F_ACK | F_PSH, rseq, syn.seq + 1, 2, 10);
	rseq += 10;
	advance(300);
	CHECK(last(&s) >= 1 && (s.flags & F_ACK) && !(s.flags & F_ECE));
	/* CE: the ACK echoes it */
	inject(9021, syn.sport, F_ACK | F_PSH, rseq, syn.seq + 1, 3, 10);
	rseq += 10;
	advance(300);
	CHECK(rt_tcpstat.TCPECNMarks == marks0 + 1);
	CHECKV(last(&s) >= 1 && (s.flags & F_ECE) && !(s.tos & 3), "flags %x tos %x", s.flags, s.tos);
	/* and keeps echoing on the next ACKs */
	inject(9021, syn.sport, F_ACK | F_PSH, rseq, syn.seq + 1, 2, 10);
	rseq += 10;
	advance(300);
	CHECK(last(&s) >= 1 && (s.flags & F_ECE));
	/* the peer's CWR ends it */
	inject(9021, syn.sport, F_ACK | F_PSH | F_CWR, rseq, syn.seq + 1, 2, 10);
	rseq += 10;
	advance(300);
	CHECK(last(&s) >= 1 && !(s.flags & F_ECE));
	CHECK(rt_api_recv(&ctx, fd, buf, sizeof(buf), 0) == 40);
	close_fd(fd);
}

/* ECE received: one cwnd reduction per window, CWR on the next new data */
static void test_ece_reaction(void)
{
	struct seg syn, s;
	struct tcp_sock *tp;
	static u8 data[8 * 1400];
	u32 ece0 = rt_tcpstat.TCPECNEceRcvd, cwnd0, ssth, una;
	int fd, n;

	fd = open_with(9031, F_ECE, &syn);
	drain();
	tp = lookup(syn.sport, 9031);
	CHECK(tp != NULL);
	if (!tp)
		return;
	memset(data, 'e', sizeof(data));
	CHECK(rt_api_send(&ctx, fd, data, sizeof(data), 0) == (int)sizeof(data));
	advance(1);
	n = 0;
	while (take(&s))
		if (s.len) {
			n++;
			CHECK((s.tos & 3) == 2 && !(s.flags & F_CWR));
		}
	CHECKV(n >= 2, "segments %d", n);
	cwnd0 = tp->snd_cwnd;
	una = syn.seq + 1;
	/* ACK of the first segment with ECE */
	inject(9031, syn.sport, F_ACK | F_ECE, PEER_ISN + 1, una + 1400, 0, 0);
	advance(1);
	CHECK(rt_tcpstat.TCPECNEceRcvd == ece0 + 1);
	CHECKV(tp->snd_ssthresh < cwnd0 && tp->ca_state == TCP_CA_CWR, "cwnd %u ssthresh %u ca %u",
	       cwnd0, tp->snd_ssthresh, tp->ca_state);
	ssth = tp->snd_ssthresh;
	/* a second ECE in the same window: no second reduction */
	inject(9031, syn.sport, F_ACK | F_ECE, PEER_ISN + 1, una + 2800, 0, 0);
	advance(1);
	CHECK(tp->snd_ssthresh == ssth);
	/* the next new data carries CWR */
	drain();
	CHECK(rt_api_send(&ctx, fd, "more", 4, 0) == 4);
	inject(9031, syn.sport, F_ACK, PEER_ISN + 1, una + (u32)sizeof(data), 0, 0);
	advance(1);
	n = 0;
	while (take(&s))
		if (s.len && (s.flags & F_CWR))
			n++;
	CHECKV(n == 1, "CWR segments %d", n);
	close_fd(fd);
}

/* IP_TOS sets the DSCP; the ECN bits stay the stack's */
static void test_ip_tos(void)
{
	struct seg syn, s;
	s32 v = 0;
	u32 len = sizeof(v);
	int fd = start(9041, &syn);

	CHECK(t_set_int(&ctx, fd, RT_IPPROTO_IP, RT_IP_TOS, 0xb8 | 3) == 0);
	CHECK(rt_api_getsockopt(&ctx, fd, RT_IPPROTO_IP, RT_IP_TOS, &v, &len) == 0 && v == 0xb8);
	inject(9041, syn.sport, F_SYN | F_ACK | F_ECE, PEER_ISN, syn.seq + 1, 0, 0);
	advance(1);
	drain();
	CHECK(rt_api_send(&ctx, fd, "tos", 3, 0) == 3);
	advance(1);
	CHECKV(last(&s) == 1 && s.tos == (0xb8 | 2), "tos %x", s.tos);
	inject(9041, syn.sport, F_ACK | F_PSH, PEER_ISN + 1, syn.seq + 4, 0, 5);
	advance(300);
	CHECKV(last(&s) >= 1 && s.tos == 0xb8, "ack tos %x", s.tos);
	close_fd(fd);
}

int main(void)
{
	s32 v = 0;
	u32 len = sizeof(v);

	setvbuf(stdout, NULL, _IOLBF, 0);
	alarm(120);
	CHECK(rt_init() == 0);
	rt_bufq_init(&capq);
	memset(&tun, 0, sizeof(tun));
	rt_strlcpy(tun.name, "tun0", sizeof(tun.name));
	tun.type = RT_IFT_PTP;
	tun.flags = RT_IFF_POINTOPOINT;
	tun.mtu = 1500;
	tun.link_output = fake_output;
	rt_lock();
	CHECK(rt_netif_add(&tun) == 0);
	rt_netif_set_addr(&tun, rt_htonl(ME), rt_htonl(0xffffff00u), rt_htonl(PEER));
	rt_netif_up(&tun);
	CHECK(rt_sysctl_byname("net.inet.tcp.ecn", &v, &len, NULL, 0) == 0 && v == 1);
	rt_unlock();
	rt_ctx_init(&ctx, 16, NULL);

	test_negotiation();
	test_fallback();
	test_ce_echo();
	test_ece_reaction();
	test_ip_tos();

	printf("ecn: negotiated %u, not agreed %u, fallback %u, CE marks %u, ECE received %u\n",
	       rt_tcpstat.TCPECNNegotiated, rt_tcpstat.TCPECNNotAgreed, rt_tcpstat.TCPECNFallback,
	       rt_tcpstat.TCPECNMarks, rt_tcpstat.TCPECNEceRcvd);
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
