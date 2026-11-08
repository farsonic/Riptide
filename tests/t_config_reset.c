/*
 * Riptide - a restart of the configuration (rt_config_reset() and
 * rt_tcp_drop_lost(), what "Riptide RESTART" does in the core):
 *   1. what riptide.conf lines set (names, DNS, hosts, routes, sysctls,
 *      QoS, background rules, NTP, time zone, mDNS, ipv4) goes back to the
 *      state rt_init() left, and the same lines can be given again
 *   2. sockets stay: a connection whose address comes back survives, one
 *      whose address is gone is reset (ECONNRESET), a listener on
 *      INADDR_ANY keeps answering SYNs at the new address
 *   3. ten resets in a row leave nothing behind
 * The interface is a fake point-to-point one; the peer is scripted.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "t_tcp_common.h"
#include "../src/core/tcp.h"

#define ME    0x0a090001u    /* 10.9.0.1 */
#define ME2   0x0a090003u    /* 10.9.0.3: the address after the "restart" */
#define PEER  0x0a090002u    /* 10.9.0.2 */
#define PEER_ISN 0x10000000u

#define F_SYN 0x02
#define F_RST 0x04
#define F_ACK 0x10

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
	rt_host_advance_clock(ms);
	rt_lock();
	rt_stack_poll();
	rt_unlock();
}

/* the next captured TCP segment's flags, ports and seq; 0 if none */
static int take(u8 *flags, u16 *sport, u16 *dport, u32 *seq, u32 *saddr)
{
	struct rt_buf *b;

	for (;;) {
		rt_lock();
		b = rt_bufq_dequeue(&capq);
		rt_unlock();
		if (!b)
			return 0;
		if (b->len >= 40 && (b->data[0] >> 4) == 4 && b->data[9] == RT_IPPROTO_TCP) {
			const u8 *th = b->data + (b->data[0] & 15) * 4;
			*sport = rt_get_be16(th);
			*dport = rt_get_be16(th + 2);
			*seq = rt_get_be32(th + 4);
			*flags = th[13];
			*saddr = rt_get_be32(b->data + 12);
			rt_buf_free(b);
			return 1;
		}
		rt_buf_free(b);
	}
}

static void drain(void)
{
	u8 f;
	u16 s, d;
	u32 q, a;
	while (take(&f, &s, &d, &q, &a))
		;
}

static void inject(u32 to, u16 sport, u16 dport, u8 flags, u32 seq, u32 ack)
{
	struct rt_iphdr ip;
	struct rt_buf *b = rt_buf_alloc(RT_BUF_HEADROOM, 24);
	u8 *th;

	if (!b)
		return;
	th = rt_buf_put(b, 20);
	memset(th, 0, 20);
	rt_put_be16(th, sport);
	rt_put_be16(th + 2, dport);
	rt_put_be32(th + 4, seq);
	rt_put_be32(th + 8, ack);
	th[12] = 5 << 4;
	th[13] = flags;
	rt_put_be16(th + 14, 65535);
	memset(&ip, 0, sizeof(ip));
	ip.ver_ihl = 0x45;
	ip.protocol = RT_IPPROTO_TCP;
	ip.ttl = 64;
	ip.saddr = rt_htonl(PEER);
	ip.daddr = rt_htonl(to);
	b->flags = RT_BUFF_CSUM_OK;
	rt_lock();
	rt_tcp_input(b, &ip);
	rt_unlock();
}

static int conf(const char *line)
{
	char err[160];
	int r;

	rt_lock();
	r = rt_config_line(line, err, sizeof(err), NULL);
	rt_unlock();
	if (r < 0)
		fprintf(stderr, "'%s': %s\n", line, err);
	return r;
}

static s32 sysctl_int(const char *name)
{
	s32 v = -1;
	u32 len = sizeof(v);

	rt_lock();
	rt_sysctl_byname(name, &v, &len, NULL, 0);
	rt_unlock();
	return v;
}

static void if_attach(u32 addr)
{
	rt_lock();
	CHECK(rt_netif_add(&tun) == 0);
	rt_netif_set_addr(&tun, rt_htonl(addr), rt_htonl(0xffffff00u), rt_htonl(PEER));
	rt_netif_up(&tun);
	rt_unlock();
}

static void if_detach(void)
{
	rt_lock();
	rt_netif_remove(&tun);
	rt_unlock();
}

static const char *const lines[] = {
	"hostname amiga",
	"domain example.org",
	"search a.example b.example",
	"nameserver 10.9.0.53",
	"nameserver 10.9.0.54",
	"host foo 10.1.2.3",
	"route add 10.20.0.0/16 via 10.9.0.2",
	"route add 10.30.0.0/16 reject",
	"sysctl net.inet.tcp.recvspace=12345",
	"sysctl net.inet.he.enable=0",
	"dns edns_size 0",
	"background program=wget port=80",
	"qos rule proto=tcp port=22 priority=interactive",
	"ntp pool.example",
	"timezone +02:00",
	"mdns on",
	NULL
};

static int count_routes(void)
{
	struct rt_route *r;
	int n = 0;

	rt_lock();
	for (r = rt_route_list(); r; r = r->next)
		if (!(r->flags & (RT_RTF_CONNECTED | RT_RTF_PMTU)))
			n++;
	rt_unlock();
	return n;
}

static void apply_all(void)
{
	int i;

	for (i = 0; lines[i]; i++)
		CHECK(conf(lines[i]) == 0);
	CHECK(!strcmp(rt_hostname, "amiga"));
	CHECK(count_routes() == 2);
	CHECK(rt_dns_get_servers(NULL, 0) == 2);
	CHECK(rt_qos_nrules() == 1);
	CHECK(sysctl_int("net.inet.tcp.recvspace") == 12345);
	CHECK(rt_tz_configured);
}

static void check_reset(s32 recvspace0, s32 he0, s32 edns0)
{
	char buf[160];

	CHECK(rt_hostname[0] == 0 && rt_domainname[0] == 0);
	CHECK(rt_dns_search_list()[0] == 0);
	CHECK(rt_dns_get_servers(NULL, 0) == 0);
	rt_lock();
	CHECK(rt_hosts_remove("foo") == -RT_ENOENT);
	rt_unlock();
	CHECK(count_routes() == 0);
	CHECK(sysctl_int("net.inet.tcp.recvspace") == recvspace0);
	CHECK(sysctl_int("net.inet.he.enable") == he0);
	CHECK(sysctl_int("dns.edns_size") == edns0);
	CHECK(rt_qos_nrules() == 0);
	rt_lock();
	rt_tcp_bg_status(buf, sizeof(buf));
	CHECK(buf[0] == 0);
	rt_ntp_status(buf, sizeof(buf));
	CHECK(buf[0] == 0);                 /* NTP off */
	rt_mdns_status(buf, sizeof(buf));
	CHECK(!strcmp(buf, "mDNS: off"));
	rt_unlock();
	CHECK(!rt_tz_configured && rt_tz_offset == rt_tz_system);
	CHECK(rt_ip4_auto_enable);
}

static void reset(void)
{
	rt_lock();
	rt_config_reset();
	rt_unlock();
}

static int drop_lost(void)
{
	int n;

	rt_lock();
	n = rt_tcp_drop_lost();
	rt_unlock();
	return n;
}

/* a connection from ME to PEER:port, established */
static int connect_to(u16 port)
{
	struct rt_sockaddr_in d = t_sa(PEER, port);
	int fd = rt_api_socket(&ctx, RT_AF_INET, RT_SOCK_STREAM, 0);
	u8 f;
	u16 s = 0, dp = 0;
	u32 seq = 0, a;

	CHECK(fd >= 0);
	t_set_nbio(&ctx, fd, 1);
	drain();
	CHECK(rt_api_connect(&ctx, fd, &d, sizeof(d)) == -RT_EINPROGRESS);
	while (take(&f, &s, &dp, &seq, &a) && !(dp == port && (f & F_SYN)))
		;
	CHECK(dp == port);
	inject(ME, port, s, F_SYN | F_ACK, PEER_ISN, seq + 1);
	advance(1);
	drain();
	return fd;
}

static int is_established(int fd)
{
	struct rt_tcp_info ti;

	return t_get_info(&ctx, fd, &ti) == 0 && ti.tcpi_state == RT_TCPS_ESTABLISHED;
}

int main(void)
{
	struct rt_sockaddr_in any = t_sa(0, 7000);
	s32 recvspace0, he0, edns0;
	int keep, lost, lfd, ufd, i;
	char buf[16];
	u8 f;
	u16 s, d;
	u32 q, a;

	setvbuf(stdout, NULL, _IOLBF, 0);
	alarm(60);
	CHECK(rt_init() == 0);
	rt_bufq_init(&capq);
	memset(&tun, 0, sizeof(tun));
	rt_strlcpy(tun.name, "tun0", sizeof(tun.name));
	tun.type = RT_IFT_PTP;
	tun.flags = RT_IFF_POINTOPOINT;
	tun.mtu = 1500;
	tun.link_output = fake_output;
	if_attach(ME);
	rt_ctx_init(&ctx, 16, NULL);
	recvspace0 = sysctl_int("net.inet.tcp.recvspace");
	he0 = sysctl_int("net.inet.he.enable");
	edns0 = sysctl_int("dns.edns_size");
	CHECK(recvspace0 != 12345 && he0 == 1 && edns0 != 0);

	/* 1. the configuration's state goes, the same lines work again */
	apply_all();
	if_detach();
	CHECK(conf("ipv4 off") == 0 && !rt_ip4_auto_enable);
	reset();
	check_reset(recvspace0, he0, edns0);
	if_attach(ME);
	apply_all();

	/* 2. sockets across a restart */
	keep = connect_to(5001);
	lost = connect_to(5002);
	CHECK(is_established(keep) && is_established(lost));
	lfd = rt_api_socket(&ctx, RT_AF_INET, RT_SOCK_STREAM, 0);
	CHECK(lfd >= 0 && rt_api_bind(&ctx, lfd, &any, sizeof(any)) == 0 && rt_api_listen(&ctx, lfd, 4) == 0);
	t_set_nbio(&ctx, lfd, 1);
	ufd = rt_api_socket(&ctx, RT_AF_INET, RT_SOCK_DGRAM, 0);
	CHECK(ufd >= 0 && rt_api_bind(&ctx, ufd, &any, sizeof(any)) == 0);

	/* the same address back: nothing is lost */
	if_detach();
	reset();
	if_attach(ME);
	CHECK(drop_lost() == 0);
	CHECK(is_established(keep) && is_established(lost));

	/* a new address: both connections were on the old one */
	if_detach();
	reset();
	if_attach(ME2);
	CHECK(drop_lost() == 2);
	CHECK(rt_api_recv(&ctx, keep, buf, sizeof(buf), 0) == -RT_ECONNRESET);
	CHECK(rt_api_recv(&ctx, lost, buf, sizeof(buf), 0) <= 0);
	drain();
	CHECK(!take(&f, &s, &d, &q, &a));    /* no RST from an address we no longer have */
	CHECK(drop_lost() == 0);

	/* the listener answers at the new address; UDP still sends */
	CHECK(rt_api_accept(&ctx, lfd, NULL, NULL) == -RT_EAGAIN);
	inject(ME2, 40000, 7000, F_SYN, PEER_ISN, 0);
	advance(1);
	CHECK(take(&f, &s, &d, &q, &a) && (f & (F_SYN | F_ACK)) == (F_SYN | F_ACK) && s == 7000 &&
	      a == ME2);
	{
		struct rt_sockaddr_in to = t_sa(PEER, 9);
		CHECK(rt_api_sendto(&ctx, ufd, "x", 1, 0, &to, sizeof(to)) == 1);
	}

	/* 3. again and again */
	for (i = 0; i < 10; i++) {
		apply_all();
		if_detach();
		reset();
		check_reset(recvspace0, he0, edns0);
		if_attach(ME2);
	}

	rt_api_close(&ctx, keep);
	rt_api_close(&ctx, lost);
	rt_api_close(&ctx, lfd);
	rt_api_close(&ctx, ufd);
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
