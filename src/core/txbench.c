/*
 * Riptide - send path benchmark (tests/t_tx_bench.c on the host, "Riptide
 * BENCH TX" on the Amiga): this stack uploads to a scripted TCP receiver
 * behind a fake Ethernet-like interface, in this process and task. The
 * application sends with SENDSIZE send() calls; the receiver checks every
 * byte and checksum and acknowledges every ACKEVERY segments (Linux:
 * about every second one) with a 64 KB window. The time the stack spends
 * in send(), in the input of the ACKs (which also sends the segments they
 * let out) and in its poll is measured with rt_now_us(), outside the lock
 * so the per-hold clock cache does not hide it; the receiver's own work is
 * not counted. Everything runs in one task, so OS overheads (the lock
 * against another task, task switches, the driver) are not in the numbers:
 * this is the protocol code alone, as the compiler built it.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "infra.h"
#include "rt/riptide.h"
#include "rt/rt_txbench.h"

#define TB_MY_IP   0x0a0a0002u          /* 10.10.0.2, the stack */
#define TB_PEER_IP 0x0a0a0001u          /* 10.10.0.1, the receiver */
#define TB_PORT    9100
#define TB_MSS     1460
#define TB_TXQ     64

static const u8 tb_my_mac[6] = { 0x02, 0, 0, 0, 0, 2 };
static const u8 tb_peer_mac[6] = { 0x02, 0, 0, 0, 0, 1 };

static struct {
	struct rt_netif nif;
	struct rt_buf *txq[TB_TXQ];
	int ntx, established, fin_seen;
	u32 iss, peer_snd_nxt, rcv_nxt, got, unacked, ack_every;
	u16 my_port;
	u8 frame[128];
	struct rt_txbench_result *r;
} tb;

static int tb_output(struct rt_netif *n, struct rt_buf *b, const u8 *dsthw, u16 et)
{
	if (et != RT_ETH_P_IP || tb.ntx == TB_TXQ) {
		rt_buf_free(b);
		return 0;
	}
	tb.txq[tb.ntx++] = b;
	return 0;
}

static u32 tb_sum16(const u8 *p, u32 len, u32 s)
{
	u32 i;

	for (i = 0; i + 1 < len; i += 2)
		s += (u32)(p[i] << 8 | p[i + 1]);
	if (len & 1)
		s += (u32)p[len - 1] << 8;
	return s;
}

static u16 tb_fold(u32 s)
{
	while (s >> 16)
		s = (s & 0xffff) + (s >> 16);
	return (u16)~s;
}

static u8 tb_pattern(u32 off)
{
	return (u8)(off * 7 + 3);
}

/* a frame from the receiver, delivered as a SANA-II driver would; the
 * stack's time for it is counted in r->ack_us */
static void tb_deliver(u8 flags, u32 seq, const u8 *opt, u32 optlen)
{
	u8 *f = tb.frame;
	u32 thl = 20 + optlen, len = 20 + thl, flen, s, t0;
	u8 *ip = f, *th = f + 20;
	struct rt_buf *b;

	memset(f, 0, sizeof(tb.frame));
	ip[0] = 0x45;
	rt_put_be16(ip + 2, (u16)len);
	ip[8] = 64;
	ip[9] = RT_IPPROTO_TCP;
	rt_put_be32(ip + 12, TB_PEER_IP);
	rt_put_be32(ip + 16, TB_MY_IP);
	rt_put_be16(ip + 10, tb_fold(tb_sum16(ip, 20, 0)));
	rt_put_be16(th, TB_PORT);
	rt_put_be16(th + 2, tb.my_port);
	rt_put_be32(th + 4, seq);
	rt_put_be32(th + 8, tb.rcv_nxt);
	th[12] = (u8)((thl / 4) << 4);
	th[13] = flags;
	rt_put_be16(th + 14, 65535);
	memcpy(th + 20, opt, optlen);
	s = tb_sum16(f + 12, 8, 0) + RT_IPPROTO_TCP + thl;
	rt_put_be16(th + 16, tb_fold(tb_sum16(th, thl, s)));
	flen = len < 46 ? 46 : len;          /* link padding */
	t0 = (u32)rt_now_us();
	rt_lock();
	b = rt_buf_alloc(64, 1504);
	if (b) {
		b->csum = rt_csum_copy(b->data, f, flen, 0);
		b->flags |= RT_BUFF_CSUM_PART;
		b->len = flen;
		b->netif = &tb.nif;
		rt_rx_batch_begin();
		rt_netif_input(&tb.nif, b, RT_ETH_P_IP, tb_peer_mac, 0);
		rt_rx_batch_end();
	}
	rt_unlock();
	tb.r->ack_us += (u32)rt_now_us() - t0;
	if (flags & 0x10)
		tb.r->acks++;
}

/* what the stack sent: SYN, data, FIN */
static void tb_peer(void)
{
	struct rt_buf *q[TB_TXQ];
	int i, n;

	rt_lock();
	/* a copy: the ACKs delivered below make the stack send more */
	n = tb.ntx;
	memcpy(q, tb.txq, (u32)n * sizeof(q[0]));
	tb.ntx = 0;
	for (i = 0; i < n; i++) {
		struct rt_buf *b = q[i];
		const u8 *ip = b->data, *th = ip + (ip[0] & 15) * 4;
		u8 fl = th[13];
		u32 seq = rt_get_be32(th + 4), k;
		u32 thl = (u32)(th[12] >> 4) * 4, dlen = rt_get_be16(ip + 2) - (u32)(th - ip) - thl;
		u32 s = tb_sum16(ip + 12, 8, 0) + RT_IPPROTO_TCP + thl + dlen;

		if (tb_fold(tb_sum16(th, thl + dlen, s)) != 0)
			tb.r->csum_bad++;
		if (fl & 0x02) {                     /* SYN: SYN-ACK with MSS 1460 */
			static const u8 mssopt[4] = { 2, 4, TB_MSS >> 8, TB_MSS & 255 };
			tb.my_port = rt_get_be16(th);
			tb.rcv_nxt = seq + 1;
			tb.peer_snd_nxt = tb.iss + 1;
			rt_buf_free(b);
			rt_unlock();
			tb_deliver(0x12, tb.iss, mssopt, 4);
			rt_lock();
			continue;
		}
		if (!tb.established && (fl & 0x10))
			tb.established = 1;
		if (dlen) {
			tb.r->segs++;
			if (seq != tb.rcv_nxt) {
				tb.r->ooo++;
			} else {
				for (k = 0; k < dlen; k++)
					if (th[thl + k] != tb_pattern(tb.got + k))
						tb.r->bad++;
				tb.got += dlen;
				tb.rcv_nxt += dlen;
				tb.unacked++;
			}
		}
		if ((fl & 1) && seq + dlen == tb.rcv_nxt) {
			tb.rcv_nxt++;
			tb.fin_seen = 1;
			tb.unacked = tb.ack_every;
		}
		rt_buf_free(b);
		if (tb.unacked >= tb.ack_every) {
			tb.unacked = 0;
			rt_unlock();
			tb_deliver(0x10, tb.peer_snd_nxt, NULL, 0);
			rt_lock();
		}
	}
	rt_unlock();
	if (tb.unacked) {                        /* the rest: delayed ACK */
		tb.unacked = 0;
		tb_deliver(0x10, tb.peer_snd_nxt, NULL, 0);
	}
}

static void tb_poll(void)
{
	u32 t0 = (u32)rt_now_us();

	rt_lock();
	rt_stack_poll();
	rt_unlock();
	tb.r->poll_us += (u32)rt_now_us() - t0;
	tb_peer();
}

int rt_txbench_setup(void)
{
	char err[96];
	int r;

	memset(&tb, 0, sizeof(tb));
	tb.nif.type = RT_IFT_ETHER;
	tb.nif.hwlen = 6;
	memcpy(tb.nif.hwaddr, tb_my_mac, 6);
	tb.nif.flags = RT_IFF_BROADCAST | RT_IFF_MULTICAST | RT_IFF_SIMPLEX;
	tb.nif.mtu = 1500;
	tb.nif.link_output = tb_output;
	rt_lock();
	rt_strlcpy(tb.nif.name, "bench0", sizeof(tb.nif.name));
	r = rt_netif_add(&tb.nif);
	if (r == 0) {
		rt_netif_set_addr(&tb.nif, rt_htonl(TB_MY_IP), rt_htonl(0xffffff00u), 0);
		rt_netif_up(&tb.nif);
		r = rt_config_line("arp add 10.10.0.1 02:00:00:00:00:01 dev bench0", err, sizeof(err), NULL);
	}
	rt_unlock();
	return r;
}

int rt_txbench(u32 total, u32 sendsize, u32 ack_every, struct rt_txbench_result *res)
{
	struct rt_sockctx ctx;
	struct rt_sockaddr_in a;
	u8 *buf;
	u32 sent = 0, i, t0, t;
	int fd, on = 1, r, idle = 0;

	memset(res, 0, sizeof(*res));
	tb.r = res;
	tb.iss = 5000;
	tb.ack_every = ack_every ? ack_every : 1;
	tb.established = tb.fin_seen = 0;
	tb.got = tb.unacked = tb.ntx = 0;
	buf = rt_malloc(sendsize);
	if (!buf)
		return -RT_ENOMEM;
	if (rt_ctx_init(&ctx, 8, NULL) != 0) {
		rt_free(buf);
		return -RT_ENOMEM;
	}
	fd = rt_api_socket(&ctx, RT_AF_INET, RT_SOCK_STREAM, 0);
	rt_api_ioctl(&ctx, fd, RT_FIONBIO, &on);
	memset(&a, 0, sizeof(a));
	a.sin_len = sizeof(a);
	a.sin_family = RT_AF_INET;
	a.sin_port = rt_htons(TB_PORT);
	a.sin_addr = rt_htonl(TB_PEER_IP);
	r = rt_api_connect(&ctx, fd, &a, sizeof(a));
	for (i = 0; i < 8 && !tb.established; i++)
		tb_poll();
	res->send_us = res->ack_us = res->poll_us = res->acks = 0;
	if (!tb.established) {
		r = -RT_ETIMEDOUT;
		goto out;
	}
	t0 = (u32)rt_now_us();
	while (sent < total) {
		u32 k = total - sent < sendsize ? total - sent : sendsize, j;
		for (j = 0; j < k; j++)
			buf[j] = tb_pattern(sent + j);
		t = (u32)rt_now_us();
		r = rt_api_send(&ctx, fd, buf, k, 0);
		res->send_us += (u32)rt_now_us() - t;
		res->sends++;
		if (r > 0) {
			sent += (u32)r;
			idle = 0;
		} else if (++idle > 10000) {
			break;
		}
		tb_poll();
	}
	rt_api_shutdown(&ctx, fd, 1);
	for (i = 0; i < 1000 && !(tb.fin_seen && tb.got == total); i++)
		tb_poll();
	res->total_us = (u32)rt_now_us() - t0;
	res->bytes = tb.got;
	res->fin = tb.fin_seen;
	res->retrans = rt_tcpstat.RetransSegs;
	r = tb.got == total && !res->bad && !res->csum_bad && tb.fin_seen ? 0 : -RT_EIO;
out:
	rt_api_close(&ctx, fd);
	tb_poll();
	rt_ctx_destroy(&ctx);
	rt_free(buf);
	return r;
}
