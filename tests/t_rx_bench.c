/*
 * Riptide - receive path benchmark: one stack, one Ethernet-like interface
 * whose "wire" is a scripted TCP sender in this process. The sender sends
 * full-sized segments as the advertised window allows; every frame reaches
 * the stack the way a SANA-II driver delivers it (a fresh MTU-sized buffer,
 * the frame copied in and summed in the same pass (RT_BUFF_CSUM_PART, as
 * sana2.c's CopyToBuff), short frames padded to the Ethernet minimum with
 * junk, rt_netif_input). One corrupted copy of a segment goes in first and
 * must be caught by the checksum. The application reads with 2 KB
 * recv() calls, like nc, and checks every byte. Single threaded: the test
 * drives rt_stack_poll() itself, so the instruction count per segment is
 * reproducible (qemu-m68k -one-insn-per-tb -d exec gives a profile).
 *
 *   t_rx_bench [BYTES] [RECVSIZE] [SUM] [BATCH]
 * defaults 1 MB, 2048, 1, 4. SUM 0: a plain copy, the stack sums. BATCH:
 * frames input per receive batch (rt_rx_batch_begin/end, as a driver's
 * poll does with the reads that completed together); 0: no batches.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rt/riptide.h"
#include "../src/core/infra.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define MY_IP   0x0a090002u          /* 10.9.0.2, the stack */
#define PEER_IP 0x0a090001u          /* 10.9.0.1, the scripted sender */
#define PORT    80
#define MSS     1460

static const u8 my_mac[6] = { 0x02, 0, 0, 0, 0, 2 };
static const u8 peer_mac[6] = { 0x02, 0, 0, 0, 0, 1 };
static struct rt_netif nif;

/* the sender's TCP state */
static u32 iss = 1000, snd_una, snd_nxt, snd_wnd, total, sent_data;
static u32 peer_isn, rcv_nxt;        /* the stack's sequence numbers */
static u16 my_port;
static int established, fin_sent, fin_acked;
static u32 acks_seen, segs_sent;
static int drv_sum = 1, corrupt_one = 1;
static u32 batch_max = 4, in_batch;

/* frames the stack sent, processed after each poll */
#define TXQ 256
static struct rt_buf *txq[TXQ];
static int ntx;

static int bench_output(struct rt_netif *n, struct rt_buf *b, const u8 *dsthw, u16 et)
{
	(void)n;
	(void)dsthw;
	if (et != RT_ETH_P_IP || ntx == TXQ) {
		rt_buf_free(b);
		return 0;
	}
	txq[ntx++] = b;
	return 0;
}

static u32 sum16(const u8 *p, u32 len, u32 s)
{
	u32 i;

	for (i = 0; i + 1 < len; i += 2)
		s += (u32)(p[i] << 8 | p[i + 1]);
	if (len & 1)
		s += (u32)p[len - 1] << 8;
	return s;
}

static u16 fold(u32 s)
{
	while (s >> 16)
		s = (s & 0xffff) + (s >> 16);
	return (u16)~s;
}

static u8 pattern(u32 off)
{
	return (u8)(off * 7 + 3);
}

/* a frame from the sender, delivered as a SANA-II driver would */
static void deliver(u8 flags, u32 seq, const u8 *opt, u32 optlen, u32 dlen)
{
	static u8 f[1600];
	u32 thl = 20 + optlen, len = 20 + thl + dlen, flen, i, s;
	u8 *ip = f, *th = f + 20, *d = th + thl;
	struct rt_buf *b;

	memset(f, 0, 20 + thl);
	ip[0] = 0x45;
	rt_put_be16(ip + 2, (u16)len);
	ip[8] = 64;
	ip[9] = RT_IPPROTO_TCP;
	rt_put_be32(ip + 12, PEER_IP);
	rt_put_be32(ip + 16, MY_IP);
	rt_put_be16(ip + 10, fold(sum16(ip, 20, 0)));
	rt_put_be16(th, PORT);
	rt_put_be16(th + 2, my_port);
	rt_put_be32(th + 4, seq);
	rt_put_be32(th + 8, (flags & 0x10) ? rcv_nxt : 0);
	th[12] = (u8)((thl / 4) << 4);
	th[13] = flags;
	rt_put_be16(th + 14, 65535);
	memcpy(th + 20, opt, optlen);
	for (i = 0; i < dlen; i++)
		d[i] = pattern(seq - iss - 1 + i);
	s = sum16(f + 12, 8, 0) + RT_IPPROTO_TCP + thl + dlen;
	rt_put_be16(th + 16, fold(sum16(th, thl + dlen, s)));
	for (flen = len; flen < 46; flen++)  /* link padding, not zero */
		f[flen] = 0xa5;

	b = rt_buf_alloc(64, 1504);          /* as sana2.c: S2_RX_HEADROOM, MTU + 4 */
	if (!b)
		return;
	if (corrupt_one && dlen) {           /* a damaged copy first */
		struct rt_buf *c = rt_buf_alloc(64, 1504);
		corrupt_one = 0;
		if (c) {
			f[len - 1] ^= 0x10;
			c->csum = rt_csum_copy(c->data, f, flen, 0);
			c->flags |= RT_BUFF_CSUM_PART;
			c->len = flen;
			c->netif = &nif;
			rt_netif_input(&nif, c, RT_ETH_P_IP, peer_mac, 0);
			f[len - 1] ^= 0x10;
		}
	}
	if (drv_sum) {                       /* the driver's CopyToBuff */
		b->csum = rt_csum_copy(b->data, f, flen, 0);
		b->flags |= RT_BUFF_CSUM_PART;
	} else {
		rt_copy(b->data, f, flen);
	}
	b->len = flen;
	b->netif = &nif;
	if (batch_max && !in_batch++)
		rt_rx_batch_begin();
	rt_netif_input(&nif, b, RT_ETH_P_IP, peer_mac, 0);
	if (in_batch && in_batch == batch_max) {
		in_batch = 0;
		rt_rx_batch_end();
	}
	segs_sent++;
}

static void send_data(void)
{
	while (established && sent_data < total) {
		u32 n = total - sent_data;
		if (n > MSS)
			n = MSS;
		if (snd_nxt + n - snd_una > snd_wnd)
			break;
		deliver(0x18, snd_nxt, NULL, 0, n);     /* ACK|PSH */
		snd_nxt += n;
		sent_data += n;
	}
	if (established && sent_data == total && !fin_sent) {
		deliver(0x11, snd_nxt, NULL, 0, 0);     /* FIN|ACK */
		snd_nxt++;
		fin_sent = 1;
	}
	if (in_batch) {                          /* the wire went quiet */
		in_batch = 0;
		rt_rx_batch_end();
	}
}

/* what the stack sent: SYN, ACKs, window updates */
static void peer_process(void)
{
	int i, n = ntx;

	ntx = 0;
	for (i = 0; i < n; i++) {
		struct rt_buf *b = txq[i];
		const u8 *ip = b->data, *th = ip + (ip[0] & 15) * 4;
		u8 fl = th[13];
		u32 seq = rt_get_be32(th + 4), ack = rt_get_be32(th + 8);
		u32 thl = (u32)(th[12] >> 4) * 4, dlen = rt_get_be16(ip + 2) - (u32)(th - ip) - thl;

		if (fl & 0x02) {                     /* SYN: answer SYN-ACK, MSS 1460 */
			static const u8 mssopt[4] = { 2, 4, MSS >> 8, MSS & 255 };
			my_port = rt_get_be16(th);
			peer_isn = seq;
			rcv_nxt = seq + 1;
			snd_una = iss;
			snd_nxt = iss + 1;
			deliver(0x12, iss, mssopt, 4, 0);
		} else if (fl & 0x10) {
			acks_seen++;
			rcv_nxt = seq + dlen + ((fl & 1) ? 1 : 0);
			if ((s32)(ack - snd_una) > 0)
				snd_una = ack;
			snd_wnd = rt_get_be16(th + 14);   /* no window scaling offered */
			if (!established && ack == iss + 1)
				established = 1;
			if (fin_sent && ack == snd_nxt)
				fin_acked = 1;
		}
		rt_buf_free(b);
	}
	send_data();
}

static void poll_once(void)
{
	rt_lock();
	rt_stack_poll();
	rt_unlock();
	peer_process();
}

int main(int argc, char **argv)
{
	struct rt_sockctx ctx;
	struct rt_sockaddr_in a;
	char err[128];
	u8 *buf;
	u32 recvsize, got = 0, bad = 0, i;
	int fd, on = 1, r, idle = 0;
	clock_t t0;
	double secs;

	total = argc > 1 ? (u32)strtoul(argv[1], NULL, 0) : 1u << 20;
	recvsize = argc > 2 ? (u32)strtoul(argv[2], NULL, 0) : 2048;
	drv_sum = argc > 3 ? atoi(argv[3]) : 1;
	batch_max = argc > 4 ? (u32)strtoul(argv[4], NULL, 0) : 4;
	buf = malloc(recvsize);
	CHECK(rt_init() == 0);
	nif.type = RT_IFT_ETHER;
	nif.hwlen = 6;
	memcpy(nif.hwaddr, my_mac, 6);
	nif.flags = RT_IFF_BROADCAST | RT_IFF_MULTICAST | RT_IFF_SIMPLEX;
	nif.mtu = 1500;
	nif.link_output = bench_output;
	rt_lock();
	rt_strlcpy(nif.name, "bench0", sizeof(nif.name));
	CHECK(rt_netif_add(&nif) == 0);
	rt_netif_set_addr(&nif, rt_htonl(MY_IP), rt_htonl(0xffffff00u), 0);
	rt_netif_up(&nif);
	CHECK(rt_config_line("arp add 10.9.0.1 02:00:00:00:00:01 dev bench0", err, sizeof(err), NULL) == 0);
	rt_unlock();

	CHECK(rt_ctx_init(&ctx, 8, NULL) == 0);
	fd = rt_api_socket(&ctx, RT_AF_INET, RT_SOCK_STREAM, 0);
	CHECK(rt_api_ioctl(&ctx, fd, RT_FIONBIO, &on) == 0);
	memset(&a, 0, sizeof(a));
	a.sin_len = sizeof(a);
	a.sin_family = RT_AF_INET;
	a.sin_port = rt_htons(PORT);
	a.sin_addr = rt_htonl(PEER_IP);
	r = rt_api_connect(&ctx, fd, &a, sizeof(a));
	CHECK(r == 0 || r == -RT_EINPROGRESS);

	t0 = clock();
	while (!fin_acked || got < total) {
		poll_once();
		r = rt_api_recv(&ctx, fd, buf, recvsize, 0);
		if (r > 0) {
			for (i = 0; i < (u32)r; i++)
				if (buf[i] != pattern(got + i))
					bad++;
			got += (u32)r;
			idle = 0;
		} else if (r == 0 && got == total) {
			poll_once();                     /* the ACK of the FIN */
			if (fin_acked)
				break;
		} else if (++idle > 100000) {
			fprintf(stderr, "stalled at %lu of %lu bytes\n", (unsigned long)got, (unsigned long)total);
			break;
		}
	}
	secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
	CHECK(got == total && bad == 0);
	CHECK(rt_tcpstat.RetransSegs == 0);
	CHECK(rt_tcpstat.InCsumErrors == 1);    /* the damaged copy, only */
	printf("t_rx_bench: %lu bytes in %lu segments, %lu ACKs from the stack (%.2f per segment), "
	       "%.3f s CPU\n", (unsigned long)got, (unsigned long)segs_sent, (unsigned long)acks_seen,
	       segs_sent ? (double)acks_seen / segs_sent : 0.0, secs);
	rt_api_close(&ctx, fd);
	poll_once();
	rt_ctx_destroy(&ctx);
	free(buf);
	printf("t_rx_bench: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
