/*
 * Riptide - packet capture: the "capture" command, the pcap file it
 * writes (LINKTYPE_LINUX_SLL), filters, stop-when-full and wrap modes, on
 * loopback (one stack) and on the simulated Ethernet wire (two stack
 * processes). Files are parsed back here and, when tcpdump is installed,
 * read with tcpdump too.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <signal.h>
#include <sys/wait.h>
#include "t_tcp_common.h"
#include "rt/rt_capture.h"

#define LO   0x7f000001u
#define A_IP 0xc0a85901u     /* 192.168.89.1 */
#define B_IP 0xc0a85902u     /* 192.168.89.2 */
#define PORT 7100

static const u8 mac_a[6] = { 0x02, 0x52, 0x54, 0, 2, 0x0a };
static const u8 mac_b[6] = { 0x02, 0x52, 0x54, 0, 2, 0x0b };

static struct rt_sockctx cs, ss;
static char out[2048];

static int cmd(const char *line)
{
	int r;

	rt_lock();
	r = rt_ctl_command(line, out, sizeof(out), NULL);
	rt_unlock();
	return r;
}

/* a number from the status line: "capture: running, N frames (...), N seen, N dropped, N overwritten" */
static unsigned long stat_field(const char *word)
{
	const char *p;

	cmd("capture status");
	p = strstr(out, word);
	if (!p)
		return ~0ul;
	while (p > out && p[-1] == ' ')
		p--;
	while (p > out && p[-1] >= '0' && p[-1] <= '9')
		p--;
	return strtoul(p, NULL, 10);
}

/* ---- the pcap file ---- */

struct rec {
	u32 sec, usec, incl, orig;
	u16 pkttype, hatype, halen, proto;
	u8  addr[8];
	const u8 *l3;             /* incl - 16 bytes */
};

struct pcap {
	u8  *data;
	long len;
	u32 snaplen, linktype, n;
	struct rec r[20000];
};

static struct pcap pc;

static int load(const char *path)
{
	FILE *f = fopen(path, "rb");
	long off;

	free(pc.data);
	memset(&pc, 0, sizeof(pc) - sizeof(pc.r));
	if (!f)
		return -1;
	fseek(f, 0, SEEK_END);
	pc.len = ftell(f);
	fseek(f, 0, SEEK_SET);
	pc.data = malloc(pc.len + 1);
	if (fread(pc.data, 1, pc.len, f) != (size_t)pc.len) {
		fclose(f);
		return -1;
	}
	fclose(f);
	if (pc.len < 24 || *(u32 *)pc.data != 0xa1b2c3d4u || ((u16 *)pc.data)[2] != 2 || ((u16 *)pc.data)[3] != 4)
		return -2;
	pc.snaplen = ((u32 *)pc.data)[4];
	pc.linktype = ((u32 *)pc.data)[5];
	for (off = 24; off < pc.len; ) {
		struct rec *r = &pc.r[pc.n];
		const u8 *p = pc.data + off;
		if (off + 16 > pc.len || pc.n >= RT_ARRAY_SIZE(pc.r))
			return -3;
		memcpy(&r->sec, p, 16);
		if (r->incl < 16 || r->incl > r->orig || r->incl > pc.snaplen || off + 16 + r->incl > pc.len)
			return -4;
		p += 16;
		r->pkttype = rt_get_be16(p);
		r->hatype = rt_get_be16(p + 2);
		r->halen = rt_get_be16(p + 4);
		memcpy(r->addr, p + 6, 8);
		r->proto = rt_get_be16(p + 14);
		r->l3 = p + 16;
		off += 16 + r->incl;
		pc.n++;
	}
	return 0;
}

/* timestamps never go backwards */
static int stamps_ordered(void)
{
	u32 i;

	for (i = 1; i < pc.n; i++)
		if (pc.r[i].sec < pc.r[i - 1].sec ||
		    (pc.r[i].sec == pc.r[i - 1].sec && pc.r[i].usec < pc.r[i - 1].usec) || pc.r[i].usec >= 1000000u)
			return 0;
	return 1;
}

static int is_tcp4(const struct rec *r)
{
	return r->proto == RT_ETH_P_IP && r->incl >= 16 + 40 && r->l3[9] == 6;
}

static u16 tcp_port(const struct rec *r, int dst)
{
	return rt_get_be16(r->l3 + ((r->l3[0] & 15) << 2) + (dst ? 2 : 0));
}

/* Data segments from sport cover the stream without gaps (a
 * retransmission may go back; no segment may skip ahead). Returns the
 * number of data segments; *bytes is the length of the stream covered. */
static u32 check_seq(u16 sport, u32 *bytes, int *ordered)
{
	u32 i, n = 0, first = 0, next = 0;

	*ordered = 1;
	for (i = 0; i < pc.n; i++) {
		const struct rec *r = &pc.r[i];
		u32 ihl, thl, len, seq;
		if (!is_tcp4(r) || tcp_port(r, 0) != sport)
			continue;
		ihl = (u32)(r->l3[0] & 15) << 2;
		thl = (u32)(r->l3[ihl + 12] >> 4) << 2;
		len = rt_get_be16(r->l3 + 2) - ihl - thl;
		seq = rt_get_be32(r->l3 + ihl + 4);
		if (!len)
			continue;
		if (!n)
			first = next = seq;
		if ((s32)(seq - next) > 0)
			*ordered = 0;
		if ((s32)(seq + len - next) > 0)
			next = seq + len;
		n++;
	}
	*bytes = next - first;
	return n;
}

/* source port of the client that connected to dport */
static u16 client_port(u16 dport)
{
	u32 i;

	for (i = 0; i < pc.n; i++)
		if (is_tcp4(&pc.r[i]) && tcp_port(&pc.r[i], 1) == dport)
			return tcp_port(&pc.r[i], 0);
	return 0;
}

/* tcpdump -r must read it and print one line per record */
static void check_tcpdump(const char *path, u32 nrec)
{
	char line[512];
	FILE *p;
	u32 n = 0;
	int st;

	if (system("command -v tcpdump >/dev/null 2>&1") != 0) {
		printf("  (tcpdump not installed: skipped)\n");
		return;
	}
	snprintf(line, sizeof(line), "tcpdump -nn -r %s 2>/dev/null", path);
	p = popen(line, "r");
	CHECK(p != NULL);
	if (!p)
		return;
	while (fgets(line, sizeof(line), p))
		n++;
	st = pclose(p);
	CHECKV(st == 0 && n == nrec, "tcpdump status %d, %u lines, %u records", st, n, nrec);
}

/* ---- loopback ---- */

struct bulk {
	int lfd;
	u64 total, got, bad;
};

static void *sink(void *p)
{
	struct bulk *a = p;
	int fd = rt_api_accept(&ss, a->lfd, NULL, NULL);

	if (fd < 0)
		return NULL;
	a->got = t_recv_pattern(&ss, fd, 7, &a->bad, 32768);
	rt_api_close(&ss, fd);
	return NULL;
}

static void loop_transfer(u16 port, u64 total)
{
	struct rt_sockaddr_in a = t_sa(0, port), d = t_sa(LO, port);
	struct bulk b;
	pthread_t th;
	int fd;

	memset(&b, 0, sizeof(b));
	b.total = total;
	b.lfd = rt_api_socket(&ss, RT_AF_INET, RT_SOCK_STREAM, 0);
	t_set_int(&ss, b.lfd, RT_SOL_SOCKET, RT_SO_REUSEADDR, 1);
	CHECK(rt_api_bind(&ss, b.lfd, &a, sizeof(a)) == 0 && rt_api_listen(&ss, b.lfd, 2) == 0);
	pthread_create(&th, NULL, sink, &b);
	fd = rt_api_socket(&cs, RT_AF_INET, RT_SOCK_STREAM, 0);
	CHECK(rt_api_connect(&cs, fd, &d, sizeof(d)) == 0);
	CHECK(t_send_pattern(&cs, fd, total, 7, 8192) == total);
	rt_api_close(&cs, fd);
	pthread_join(th, NULL);
	rt_api_close(&ss, b.lfd);
	CHECK(b.got == total && b.bad == 0);
}

static void test_loopback6(void)
{
	struct rt_sockaddr_in6 a;
	struct bulk b;
	pthread_t th;
	u32 i;
	int fd;

	printf("loopback IPv6, host ::1 filter\n");
	CHECK(cmd("capture start host=::1 proto=tcp port=5306") == 0);
	memset(&a, 0, sizeof(a));
	a.sin6_len = sizeof(a);
	a.sin6_family = RT_AF_INET6;
	a.sin6_port = rt_htons(5306);
	memset(&b, 0, sizeof(b));
	b.lfd = rt_api_socket(&ss, RT_AF_INET6, RT_SOCK_STREAM, 0);
	t_set_int(&ss, b.lfd, RT_SOL_SOCKET, RT_SO_REUSEADDR, 1);
	CHECK(rt_api_bind(&ss, b.lfd, &a, sizeof(a)) == 0 && rt_api_listen(&ss, b.lfd, 2) == 0);
	pthread_create(&th, NULL, sink, &b);
	rt_inet6_pton("::1", &a.sin6_addr);
	fd = rt_api_socket(&cs, RT_AF_INET6, RT_SOCK_STREAM, 0);
	CHECK(rt_api_connect(&cs, fd, &a, sizeof(a)) == 0);
	CHECK(t_send_pattern(&cs, fd, 30000, 7, 8192) == 30000);
	rt_api_close(&cs, fd);
	pthread_join(th, NULL);
	rt_api_close(&ss, b.lfd);
	CHECK(b.got == 30000 && b.bad == 0);
	loop_transfer(5306, 30000);                   /* IPv4: filtered out */
	CHECK(cmd("capture stop") == 0 && cmd("capture save /tmp/rt_capture_lo6.pcap") == 0);
	CHECK(load("/tmp/rt_capture_lo6.pcap") == 0 && pc.n >= 6);
	for (i = 0; i < pc.n; i++)
		CHECK(pc.r[i].proto == RT_ETH_P_IPV6 && pc.r[i].l3[6] == 6 &&
		      (rt_get_be16(pc.r[i].l3 + 40) == 5306 || rt_get_be16(pc.r[i].l3 + 42) == 5306));
	check_tcpdump("/tmp/rt_capture_lo6.pcap", pc.n);
	unlink("/tmp/rt_capture_lo6.pcap");
	CHECK(cmd("capture clear") == 0);
}

static void test_commands(void)
{
	printf("commands\n");
	CHECK(cmd("capture") == 0 && strstr(out, "capture: off"));
	CHECK(cmd("capture save /tmp/x") == -RT_ENOENT);
	CHECK(cmd("capture start iface=nope") == -RT_ENXIO && strstr(out, "no interface"));
	CHECK(cmd("capture start proto=sctp") == -RT_EINVAL && strstr(out, "bad protocol"));
	CHECK(cmd("capture start port=0") == -RT_EINVAL);
	CHECK(cmd("capture start host=1.2.3") == -RT_EINVAL);
	CHECK(cmd("capture start size=8") == -RT_EINVAL);
	CHECK(cmd("capture start bogus") == -RT_EINVAL && strstr(out, "usage"));
	CHECK(cmd("capture frobnicate") == -RT_EINVAL);
	CHECK(cmd("capture start iface=lo0 proto=tcp port=5 host=127.0.0.1 snaplen=96 size=32 wrap") == 0);
	CHECKV(strstr(out, "running") && strstr(out, "snaplen 96") && strstr(out, "wrap") &&
	       strstr(out, "iface lo0 proto tcp port 5 host 127.0.0.1") && strstr(out, "of 32 KB"), "%s", out);
	CHECK(cmd("capture stop") == 0 && strstr(out, "stopped"));
	CHECK(cmd("capture clear") == 0 && strstr(out, "capture: off"));
	CHECK(!rt_capture_on);
	/* config file syntax works too */
	CHECK(rt_config_line("capture start size=16", NULL, 0, NULL) == 0 && rt_capture_on);
	CHECK(cmd("capture clear") == 0);
}

static void test_loopback(void)
{
	const char *f = "/tmp/rt_capture_lo.pcap";
	u32 bytes, nseg, i, syn = 0, ackers = 0;
	unsigned long frames;
	int ordered;

	printf("loopback transfer, stop mode\n");
	CHECK(cmd("capture start size=2048 file=/tmp/rt_capture_lo.pcap") == 0);
	loop_transfer(5301, 300000);
	frames = stat_field("frames");
	CHECK(frames > 15 && stat_field("dropped") == 0);
	CHECK(cmd("capture stop") == 0);             /* saves to file= */
	CHECKV(strstr(out, "saved to /tmp/rt_capture_lo.pcap"), "%s", out);
	CHECK(load(f) == 0);
	CHECKV(pc.n == frames, "%u records, %lu frames", pc.n, frames);
	CHECK(pc.linktype == 113 && pc.snaplen == 16 + 128);
	CHECK(stamps_ordered());
	for (i = 0; i < pc.n; i++) {
		const struct rec *r = &pc.r[i];
		if (!is_tcp4(r))
			continue;
		CHECK(r->hatype == 772 && r->pkttype == 0 && r->halen == 0);
		CHECK(r->incl <= 16 + 128 && r->orig >= r->incl);
		if (tcp_port(r, 1) == 5301 && (r->l3[((r->l3[0] & 15) << 2) + 13] & 0x02))
			syn++;
		if (tcp_port(r, 0) == 5301)
			ackers++;
	}
	CHECK(syn == 1 && ackers > 0);                 /* each lo0 packet once */
	nseg = check_seq(client_port(5301), &bytes, &ordered);
	CHECKV(nseg > 10 && bytes == 300000 && ordered, "%u segments, %u bytes, ordered %d", nseg, bytes, ordered);
	check_tcpdump(f, pc.n);
	/* save again by name, then filters */
	CHECK(cmd("capture save /tmp/rt_capture_lo2.pcap") == 0 && load("/tmp/rt_capture_lo2.pcap") == 0 && pc.n == frames);
	unlink("/tmp/rt_capture_lo2.pcap");

	printf("loopback, filter port and snaplen\n");
	CHECK(cmd("capture start proto=tcp port=5303 snaplen=60") == 0);
	loop_transfer(5302, 50000);
	loop_transfer(5303, 50000);
	CHECK(cmd("capture stop") == 0 && cmd("capture save /tmp/rt_capture_lo.pcap") == 0 && load(f) == 0);
	CHECK(pc.n > 4 && pc.snaplen == 16 + 60);
	for (i = 0; i < pc.n; i++)
		CHECK(is_tcp4(&pc.r[i]) && (tcp_port(&pc.r[i], 0) == 5303 || tcp_port(&pc.r[i], 1) == 5303) &&
		      pc.r[i].incl <= 16 + 60);

	printf("loopback, small ring: stop when full, then wrap\n");
	CHECK(cmd("capture start size=16") == 0);
	loop_transfer(5304, 3000000);
	frames = stat_field("frames");
	CHECK(stat_field("dropped") > 0 && stat_field("overwritten") == 0);
	CHECK(cmd("capture save /tmp/rt_capture_lo.pcap") == 0 && load(f) == 0 && pc.n == frames && stamps_ordered());
	/* the start of the connection is kept */
	CHECK(pc.n && is_tcp4(&pc.r[0]) && (pc.r[0].l3[33] & 0x02));
	CHECK(cmd("capture start size=16 wrap") == 0);
	loop_transfer(5305, 3000000);
	CHECK(stat_field("dropped") == 0 && stat_field("overwritten") > 0);
	frames = stat_field("frames");
	/* saving pauses recording, then it resumes */
	CHECK(cmd("capture save /tmp/rt_capture_lo.pcap") == 0 && load(f) == 0 && pc.n == frames && stamps_ordered());
	CHECK(rt_capture_on);
	/* the end is kept: the close is there, the SYN was overwritten */
	{
		int fin = 0;
		for (i = 0; i < pc.n; i++)
			if (is_tcp4(&pc.r[i]) && (pc.r[i].l3[33] & 0x01))
				fin++;
		CHECK(fin >= 1);          /* the client's; the server's may still be queued */
		CHECK(!(pc.r[0].l3[33] & 0x02));
	}
	check_tcpdump(f, pc.n);
	CHECK(cmd("capture clear") == 0);
	unlink(f);
}

/* ---- wire: two processes ---- */

static struct rt_netif *bring_up(const char *me, const char *peer, const u8 *mac, u32 ip)
{
	struct rt_netif *n;

	if (rt_host_start() != 0)
		return NULL;
	n = rt_host_wire_open(me, peer, mac);
	if (!n)
		return NULL;
	rt_lock();
	rt_netif_set_addr(n, rt_htonl(ip), rt_htonl(0xffffff00u), 0);
	rt_netif_up(n);
	rt_unlock();
	return n;
}

static int run_b(const char *pa, const char *pb)
{
	struct rt_sockaddr_in a = t_sa(0, PORT);
	int lfd, n;

	alarm(300);
	if (!bring_up(pb, pa, mac_b, B_IP))
		return 2;
	rt_ctx_init(&ss, 16, NULL);
	lfd = rt_api_socket(&ss, RT_AF_INET, RT_SOCK_STREAM, 0);
	t_set_int(&ss, lfd, RT_SOL_SOCKET, RT_SO_REUSEADDR, 1);
	if (rt_api_bind(&ss, lfd, &a, sizeof(a)) || rt_api_listen(&ss, lfd, 4))
		return 3;
	for (n = 0; n < 2; n++) {
		u64 bad;
		int fd = rt_api_accept(&ss, lfd, NULL, NULL);
		if (fd < 0)
			return 4;
		t_recv_pattern(&ss, fd, 7, &bad, 32768);
		rt_api_close(&ss, fd);
	}
	sleep(1);
	return 0;
}

static void wire_send(u64 total)
{
	struct rt_sockaddr_in d = t_sa(B_IP, PORT);
	int fd = rt_api_socket(&cs, RT_AF_INET, RT_SOCK_STREAM, 0), i;

	for (i = 0; i < 50 && rt_api_connect(&cs, fd, &d, sizeof(d)) != 0; i++) {
		rt_api_close(&cs, fd);
		usleep(100000);
		fd = rt_api_socket(&cs, RT_AF_INET, RT_SOCK_STREAM, 0);
	}
	CHECK(t_send_pattern(&cs, fd, total, 7, 8192) == total);
	rt_api_close(&cs, fd);
	/* close() returns with data still queued: wait for the wire to go quiet */
	{
		unsigned long last = ~0ul, now;
		for (i = 0; i < 100; i++) {
			usleep(200000);
			now = stat_field("frames");
			if (now == last)
				break;
			last = now;
		}
	}
}

static void test_wire(void)
{
	const char *f = "/tmp/rt_capture_wire.pcap";
	char pa[64], pb[64], line[128];
	struct rt_netif *n;
	u32 i, arp = 0, in = 0, outn = 0, bytes;
	int status = 0, ordered;
	pid_t child;

	printf("simulated Ethernet wire\n");
	snprintf(pa, sizeof(pa), "/tmp/rt_cap_%d_a", (int)getpid());
	snprintf(pb, sizeof(pb), "/tmp/rt_cap_%d_b", (int)getpid());
	child = fork();
	if (child == 0)
		_exit(run_b(pa, pb));
	n = bring_up(pa, pb, mac_a, A_IP);
	CHECK(n != NULL);
	if (!n)
		goto out;
	snprintf(line, sizeof(line), "capture start iface=%s", n->name);
	CHECK(cmd(line) == 0);
	wire_send(200000);
	CHECK(cmd("capture stop") == 0 && cmd("capture save /tmp/rt_capture_wire.pcap") == 0 && load(f) == 0);
	for (i = 0; i < pc.n; i++) {
		const struct rec *r = &pc.r[i];
		CHECK(r->hatype == 1 && r->halen == 6);
		if (r->proto == RT_ETH_P_ARP)
			arp++;
		if (r->pkttype == 4) {
			CHECK(!memcmp(r->addr, mac_a, 6));
			outn++;
		} else {
			CHECK(!memcmp(r->addr, mac_b, 6) && r->pkttype <= 2);
			in++;
		}
	}
	CHECKV(arp >= 2 && in > 5 && outn > 20, "arp %u in %u out %u", arp, in, outn);
	CHECK(stamps_ordered());
	check_seq(client_port(PORT), &bytes, &ordered);
	CHECKV(bytes == 200000 && ordered, "%u bytes, ordered %d", bytes, ordered);
	check_tcpdump(f, pc.n);

	printf("wire, host and port filter\n");
	CHECK(cmd("capture start proto=tcp host=192.168.89.2 port=7100") == 0);
	wire_send(50000);
	CHECK(cmd("capture stop") == 0 && cmd("capture save /tmp/rt_capture_wire.pcap") == 0 && load(f) == 0);
	CHECK(pc.n > 4);
	for (i = 0; i < pc.n; i++)
		CHECK(is_tcp4(&pc.r[i]) && (tcp_port(&pc.r[i], 0) == PORT || tcp_port(&pc.r[i], 1) == PORT) &&
		      (rt_get_be32(pc.r[i].l3 + 12) == B_IP || rt_get_be32(pc.r[i].l3 + 16) == B_IP));
	CHECK(cmd("capture start host=192.168.89.77") == 0);
	wire_send(10000);
	CHECK(stat_field("frames") == 0);
	CHECK(cmd("capture clear") == 0);
	unlink(f);
out:
	waitpid(child, &status, 0);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int main(void)
{
	alarm(600);
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (rt_host_start() != 0) {
		printf("cannot start stack\n");
		return 1;
	}
	rt_ctx_init(&cs, 32, NULL);
	rt_ctx_init(&ss, 32, NULL);
	test_commands();
	test_loopback();
	test_loopback6();
	rt_host_stop();
	test_wire();
	free(pc.data);
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
