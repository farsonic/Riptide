/*
 * Riptide - unit tests of the Berkeley Packet Filter (src/core/bpf.c),
 * the packet filter of the Roadshow bsdsocket.library API: the filter
 * validator and machine (the BPF manual's example programs), channel
 * allocation and ownership, the ioctls, frames seen on the way in and out
 * of an Ethernet interface (rebuilt link header) and of lo0 (DLT_NULL),
 * the store/hold buffers, drops, read timeouts, writes, promiscuous mode
 * and interface removal. Single threaded, no stack thread.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt/riptide.h"
#include "rt/rt_host.h"
#include "rt/rt_bpf.h"
#include "../src/core/infra.h"

#include "t_infra_stubs.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define STMT(c, k)        { (u16)(c), 0, 0, (u32)(k) }
#define JUMP(c, k, t, f)  { (u16)(c), (u8)(t), (u8)(f), (u32)(k) }

static struct { u16 et; u32 len; u8 dst[6]; int bcast; u8 data[1600]; } rec[8];
static int nrec;
static u16 drv_flags;

static int fake_output(struct rt_netif *nif, struct rt_buf *b, const u8 *dsthw, u16 et)
{
	if (nrec < 8) {
		rec[nrec].et = et;
		rec[nrec].len = b->len;
		rec[nrec].bcast = !dsthw;
		if (dsthw)
			memcpy(rec[nrec].dst, dsthw, 6);
		memcpy(rec[nrec].data, b->data, b->len < 1600 ? b->len : 1600);
		nrec++;
	}
	rt_buf_free(b);
	return 0;
}

static void fake_flags(struct rt_netif *nif, u16 nf)
{
	drv_flags = nf;
}

static struct rt_netif eth0;
static const u8 mac0[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
static const u8 peer[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x99 };
static int owner_a, owner_b;
#define A (&owner_a)
#define B (&owner_b)

static u32 get32(const u8 *p)
{
	u32 v;
	memcpy(&v, p, 4);
	return v;
}

static u16 get16(const u8 *p)
{
	u16 v;
	memcpy(&v, p, 2);
	return v;
}

/* a frame of 'len' payload bytes with ethertype et received on nif */
static void rx(struct rt_netif *nif, u16 et, u32 len, u8 fill, const u8 *src, u16 flags)
{
	struct rt_buf *b = rt_buf_alloc(RT_BUF_HEADROOM, len);
	memset(rt_buf_put(b, len), fill, len);
	rt_netif_input(nif, b, et, src, flags);
}

static void test_machine(void)
{
	/* the BPF manual's examples */
	struct rt_bpf_insn rarp[] = {
		STMT(RT_BPF_LD | RT_BPF_H | RT_BPF_ABS, 12),
		JUMP(RT_BPF_JMP | RT_BPF_JEQ | RT_BPF_K, 0x8035, 0, 3),
		STMT(RT_BPF_LD | RT_BPF_H | RT_BPF_ABS, 20),
		JUMP(RT_BPF_JMP | RT_BPF_JEQ | RT_BPF_K, 3, 0, 1),
		STMT(RT_BPF_RET | RT_BPF_K, 42),
		STMT(RT_BPF_RET | RT_BPF_K, 0),
	};
	struct rt_bpf_insn finger[] = {
		STMT(RT_BPF_LD | RT_BPF_H | RT_BPF_ABS, 12),
		JUMP(RT_BPF_JMP | RT_BPF_JEQ | RT_BPF_K, 0x0800, 0, 10),
		STMT(RT_BPF_LD | RT_BPF_B | RT_BPF_ABS, 23),
		JUMP(RT_BPF_JMP | RT_BPF_JEQ | RT_BPF_K, 6, 0, 8),
		STMT(RT_BPF_LD | RT_BPF_H | RT_BPF_ABS, 20),
		JUMP(RT_BPF_JMP | RT_BPF_JSET | RT_BPF_K, 0x1fff, 6, 0),
		STMT(RT_BPF_LDX | RT_BPF_B | RT_BPF_MSH, 14),
		STMT(RT_BPF_LD | RT_BPF_H | RT_BPF_IND, 14),
		JUMP(RT_BPF_JMP | RT_BPF_JEQ | RT_BPF_K, 79, 2, 0),
		STMT(RT_BPF_LD | RT_BPF_H | RT_BPF_IND, 16),
		JUMP(RT_BPF_JMP | RT_BPF_JEQ | RT_BPF_K, 79, 0, 1),
		STMT(RT_BPF_RET | RT_BPF_K, (u32)-1),
		STMT(RT_BPF_RET | RT_BPF_K, 0),
	};
	struct rt_bpf_insn alu[] = {
		STMT(RT_BPF_LD | RT_BPF_W | RT_BPF_LEN, 0),     /* A = len */
		STMT(RT_BPF_ST, 3),                           /* M[3] = len */
		STMT(RT_BPF_LDX | RT_BPF_IMM, 3),             /* X = 3 */
		STMT(RT_BPF_ALU | RT_BPF_MUL | RT_BPF_X, 0),  /* A = 3 len */
		STMT(RT_BPF_ALU | RT_BPF_SUB | RT_BPF_K, 1),
		STMT(RT_BPF_ALU | RT_BPF_DIV | RT_BPF_K, 2),
		STMT(RT_BPF_ALU | RT_BPF_LSH | RT_BPF_K, 4),
		STMT(RT_BPF_ALU | RT_BPF_RSH | RT_BPF_K, 3),  /* ((3 len - 1) / 2) * 2 */
		STMT(RT_BPF_MISC | RT_BPF_TAX, 0),
		STMT(RT_BPF_LD | RT_BPF_MEM, 3),
		STMT(RT_BPF_ALU | RT_BPF_ADD | RT_BPF_X, 0),
		STMT(RT_BPF_ALU | RT_BPF_NEG, 0),
		STMT(RT_BPF_ALU | RT_BPF_NEG, 0),
		STMT(RT_BPF_RET | RT_BPF_A, 0),
	};
	struct rt_bpf_insn bad_jump[] = {
		JUMP(RT_BPF_JMP | RT_BPF_JEQ | RT_BPF_K, 0, 5, 0),
		STMT(RT_BPF_RET | RT_BPF_K, 0),
	};
	struct rt_bpf_insn no_ret[] = { STMT(RT_BPF_LD | RT_BPF_IMM, 1) };
	struct rt_bpf_insn div0[] = { STMT(RT_BPF_ALU | RT_BPF_DIV | RT_BPF_K, 0), STMT(RT_BPF_RET | RT_BPF_A, 0) };
	struct rt_bpf_insn mem[] = { STMT(RT_BPF_ST, RT_BPF_MEMWORDS), STMT(RT_BPF_RET | RT_BPF_A, 0) };
	struct rt_bpf_insn ja[] = { STMT(RT_BPF_JMP | RT_BPF_JA, 1), STMT(RT_BPF_RET | RT_BPF_K, 1),
	                            STMT(RT_BPF_RET | RT_BPF_K, 2) };
	u8 pkt[64];

	CHECK(rt_bpf_validate(rarp, 6) == 0);
	CHECK(rt_bpf_validate(finger, 13) == 0);
	CHECK(rt_bpf_validate(alu, 14) == 0);
	CHECK(rt_bpf_validate(ja, 3) == 0);
	CHECK(rt_bpf_validate(bad_jump, 2) == -RT_EINVAL);
	CHECK(rt_bpf_validate(no_ret, 1) == -RT_EINVAL);
	CHECK(rt_bpf_validate(div0, 2) == -RT_EINVAL);
	CHECK(rt_bpf_validate(mem, 2) == -RT_EINVAL);
	CHECK(rt_bpf_validate(rarp, 0) == -RT_EINVAL);
	CHECK(rt_bpf_validate(NULL, 1) == -RT_EINVAL);

	memset(pkt, 0, sizeof(pkt));
	rt_put_be16(pkt + 12, 0x8035);
	rt_put_be16(pkt + 20, 3);
	CHECK(rt_bpf_filter(rarp, pkt, 60, 60) == 42);
	rt_put_be16(pkt + 20, 4);
	CHECK(rt_bpf_filter(rarp, pkt, 60, 60) == 0);
	CHECK(rt_bpf_filter(rarp, pkt, 60, 16) == 0);      /* load beyond the buffer rejects */

	/* TCP to port 79 */
	memset(pkt, 0, sizeof(pkt));
	rt_put_be16(pkt + 12, 0x0800);
	pkt[14] = 0x45;
	pkt[23] = 6;
	rt_put_be16(pkt + 34, 1234);
	rt_put_be16(pkt + 36, 79);
	CHECK(rt_bpf_filter(finger, pkt, 64, 64) == (u32)-1);
	rt_put_be16(pkt + 36, 80);
	CHECK(rt_bpf_filter(finger, pkt, 64, 64) == 0);
	rt_put_be16(pkt + 34, 79);                        /* from finger */
	CHECK(rt_bpf_filter(finger, pkt, 64, 64) == (u32)-1);
	pkt[14] = 0x46;                                   /* IP options: header 24 bytes */
	rt_put_be16(pkt + 38, 79);
	rt_put_be16(pkt + 34 + 4, 79);
	CHECK(rt_bpf_filter(finger, pkt, 64, 64) == (u32)-1);
	pkt[14] = 0x45;
	rt_put_be16(pkt + 20, 0x0010);                    /* a later fragment */
	CHECK(rt_bpf_filter(finger, pkt, 64, 64) == 0);

	CHECK(rt_bpf_filter(alu, pkt, 10, 10) == ((3 * 10 - 1) / 2 * 2) + 10);
	CHECK(rt_bpf_filter(ja, pkt, 10, 10) == 2);
	CHECK(rt_bpf_filter(NULL, pkt, 10, 10) == (u32)-1);
}

static void test_channels(void)
{
	CHECK(rt_bpf_open(0, A) == 0);
	CHECK(rt_bpf_open(0, B) == -RT_EBUSY);
	CHECK(rt_bpf_open(-1, B) == 1);
	CHECK(rt_bpf_open(RT_BPF_NCHAN, A) == -RT_ENXIO);
	CHECK(rt_bpf_close(1, A) == -RT_EPERM);
	CHECK(rt_bpf_waiting(1, A) == -RT_EPERM);
	CHECK(rt_bpf_close(1, B) == 0);
	CHECK(rt_bpf_close(1, B) == -RT_ENXIO);
	CHECK(rt_bpf_close(5, A) == -RT_ENXIO);
	CHECK(rt_bpf_close(0, A) == 0);
	/* all channels taken */
	{
		int i;
		for (i = 0; i < RT_BPF_NCHAN; i++)
			CHECK(rt_bpf_open(-1, A) == i);
		CHECK(rt_bpf_open(-1, B) == -RT_EBUSY);
		rt_bpf_close_owner(A);
		CHECK(rt_bpf_open(-1, B) == 0);
		CHECK(rt_bpf_close(0, B) == 0);
	}
}

static int setif(int ch, void *owner, const char *name)
{
	char ifr[32];

	memset(ifr, 0, sizeof(ifr));
	strncpy(ifr, name, 16);
	return rt_bpf_ioctl(ch, owner, RT_BIOCSETIF, ifr);
}

static void test_ioctl_and_capture(void)
{
	u8 buf[RT_BPF_MAXBUFSIZE];
	u32 v, intr = 0;
	char ifr[32];
	struct rt_bpf_version bv;
	struct rt_bpf_stat st;
	struct rt_bpf_timeval tv;
	struct rt_sockaddr_in sin;
	int ch = rt_bpf_open(-1, A), n, hl;

	CHECK(ch == 0);
	CHECK(rt_bpf_ioctl(ch, A, RT_BIOCGBLEN, &v) == 0 && v == RT_BPF_BUFSIZE);
	v = 10;
	CHECK(rt_bpf_ioctl(ch, A, RT_BIOCSBLEN, &v) == 0 && v == RT_BPF_MINBUFSIZE);
	v = 1u << 20;
	CHECK(rt_bpf_ioctl(ch, A, RT_BIOCSBLEN, &v) == 0 && v == RT_BPF_MAXBUFSIZE);
	v = 2048;
	CHECK(rt_bpf_ioctl(ch, A, RT_BIOCSBLEN, &v) == 0 && v == 2048);
	CHECK(rt_bpf_ioctl(ch, A, RT_BIOCGDLT, &v) == -RT_EINVAL);   /* not attached */
	CHECK(rt_bpf_read(ch, A, buf, 2048, &intr) == -RT_EINVAL);
	CHECK(rt_bpf_ioctl(ch, A, RT_BIOCVERSION, &bv) == 0 && bv.bv_major == 1 && bv.bv_minor == 1);
	CHECK(setif(ch, A, "nosuch0") == -RT_ENXIO);
	CHECK(setif(ch, A, "eth0") == 0);
	CHECK(rt_bpf_on);
	CHECK(rt_bpf_ioctl(ch, A, RT_BIOCSBLEN, &v) == -RT_EINVAL);  /* attached */
	CHECK(rt_bpf_ioctl(ch, A, RT_BIOCGDLT, &v) == 0 && v == RT_DLT_EN10MB);
	memset(ifr, 'x', sizeof(ifr));
	CHECK(rt_bpf_ioctl(ch, A, RT_BIOCGETIF, ifr) == 0 && !strcmp(ifr, "eth0"));
	CHECK(rt_bpf_ioctl(ch, A, RT_SIOCGIFADDR, &sin) == 0 && sin.sin_family == RT_AF_INET &&
	      sin.sin_addr == eth0.addr);
	CHECK(rt_bpf_ioctl(ch, A, 0x12345678, &v) == -RT_EINVAL);
	CHECK(rt_bpf_ioctl(ch, B, RT_BIOCGDLT, &v) == -RT_EPERM);
	v = 1;
	CHECK(rt_bpf_ioctl(ch, A, RT_BIOCIMMEDIATE, &v) == 0);
	CHECK(rt_bpf_waiting(ch, A) == 0);

	/* received: our MAC is the destination, the peer the source */
	rx(&eth0, 0x88b5, 100, 0xab, peer, 0);
	CHECK(rt_bpf_waiting(ch, A) == 1);
	CHECK(rt_bpf_ioctl(ch, A, RT_FIONREAD, &v) == 0 && v == 18 + 14 + 100);
	CHECK(rt_bpf_read(ch, A, buf, 100, &intr) == -RT_EINVAL);
	n = rt_bpf_read(ch, A, buf, 2048, &intr);
	CHECK(n == 18 + 14 + 100);
	hl = get16(buf + 16);
	CHECK(hl == 18);
	CHECK(get32(buf + 8) == 114 && get32(buf + 12) == 114);
	CHECK(!memcmp(buf + hl, mac0, 6) && !memcmp(buf + hl + 6, peer, 6));
	CHECK(rt_get_be16(buf + hl + 12) == 0x88b5 && buf[hl + 14] == 0xab && buf[hl + 113] == 0xab);
	CHECK(rt_bpf_waiting(ch, A) == 0);

	/* broadcast, and multicast to 224.0.0.251 */
	rx(&eth0, 0x88b5, 10, 1, peer, RT_BUFF_BCAST);
	n = rt_bpf_read(ch, A, buf, 2048, &intr);
	CHECK(n == 18 + 24 && buf[18] == 0xff && buf[23] == 0xff);
	{
		struct rt_buf *b = rt_buf_alloc(RT_BUF_HEADROOM, 20);
		u8 *p = rt_buf_put(b, 20);
		memset(p, 0, 20);
		p[0] = 0x45;
		p[16] = 224; p[17] = 0; p[18] = 0; p[19] = 251;
		p[9] = 253;                     /* nobody's protocol */
		rt_netif_input(&eth0, b, RT_ETH_P_IP, peer, RT_BUFF_MCAST);
	}
	n = rt_bpf_read(ch, A, buf, 2048, &intr);
	CHECK(n > 0 && buf[18] == 0x01 && buf[19] == 0x00 && buf[20] == 0x5e && buf[23] == 251);

	/* sent: the destination given to the driver, our MAC the source */
	{
		struct rt_buf *b = rt_buf_alloc(RT_BUF_HEADROOM, 30);
		memset(rt_buf_put(b, 30), 0xcd, 30);
		nrec = 0;
		CHECK(rt_netif_xmit(&eth0, b, peer, 0x88b5) == 0);
		CHECK(nrec == 1);
	}
	n = rt_bpf_read(ch, A, buf, 2048, &intr);
	CHECK(n == 18 + 14 + 30);
	CHECK(!memcmp(buf + 18, peer, 6) && !memcmp(buf + 24, mac0, 6) && buf[18 + 14] == 0xcd);

	/* a filter: only ethertype 0x88b6, 20 bytes of it */
	{
		struct rt_bpf_insn f[] = {
			STMT(RT_BPF_LD | RT_BPF_H | RT_BPF_ABS, 12),
			JUMP(RT_BPF_JMP | RT_BPF_JEQ | RT_BPF_K, 0x88b6, 0, 1),
			STMT(RT_BPF_RET | RT_BPF_K, 20),
			STMT(RT_BPF_RET | RT_BPF_K, 0),
		};
		struct rt_bpf_insn bad[] = { STMT(RT_BPF_LD | RT_BPF_IMM, 0) };
		struct rt_bpf_program pr = { 4, f }, prb = { 1, bad };
		CHECK(rt_bpf_ioctl(ch, A, RT_BIOCSETF, &prb) == -RT_EINVAL);
		CHECK(rt_bpf_ioctl(ch, A, RT_BIOCSETF, &pr) == 0);
		rx(&eth0, 0x88b5, 50, 0, peer, 0);
		CHECK(rt_bpf_waiting(ch, A) == 0);
		rx(&eth0, 0x88b6, 50, 7, peer, 0);
		n = rt_bpf_read(ch, A, buf, 2048, &intr);
		CHECK(n == 18 + 20 && get32(buf + 8) == 20 && get32(buf + 12) == 64);
		CHECK(rt_bpf_ioctl(ch, A, RT_BIOCGSTATS, &st) == 0 && st.bs_recv == 2 && st.bs_drop == 0);
		pr.bf_insns = NULL;
		pr.bf_len = 0;
		CHECK(rt_bpf_ioctl(ch, A, RT_BIOCSETF, &pr) == 0);   /* back to everything */
	}

	/* buffers full: the store and hold buffers fill, then drops */
	v = 0;
	CHECK(rt_bpf_ioctl(ch, A, RT_BIOCIMMEDIATE, &v) == 0);
	CHECK(rt_bpf_ioctl(ch, A, RT_BIOCFLUSH, NULL) == 0);
	{
		int i;
		for (i = 0; i < 40; i++)
			rx(&eth0, 0x88b5, 200, (u8)i, peer, 0);
	}
	CHECK(rt_bpf_ioctl(ch, A, RT_BIOCGSTATS, &st) == 0 && st.bs_recv == 40 && st.bs_drop > 0);
	CHECK(rt_bpf_waiting(ch, A) == 1);               /* the hold buffer is full */
	n = rt_bpf_read(ch, A, buf, 2048, &intr);
	CHECK(n == 8 * 232 && buf[18 + 14] == 0);       /* the oldest packets first */
	CHECK(rt_bpf_waiting(ch, A) == 0);               /* the store buffer waits */

	/* read timeout with partial data (non-immediate) and with none */
	tv.tv_sec = 0;
	tv.tv_usec = 50000;
	CHECK(rt_bpf_ioctl(ch, A, RT_BIOCSRTIMEOUT, &tv) == 0);
	memset(&tv, 0, sizeof(tv));
	CHECK(rt_bpf_ioctl(ch, A, RT_BIOCGRTIMEOUT, &tv) == 0 && tv.tv_sec == 0 && tv.tv_usec == 50000);
	CHECK(rt_bpf_ioctl(ch, A, RT_BIOCFLUSH, NULL) == 0);
	rx(&eth0, 0x88b5, 10, 3, peer, 0);
	CHECK(rt_bpf_waiting(ch, A) == 0);
	rt_lock();
	n = rt_bpf_read(ch, A, buf, 2048, &intr);
	rt_unlock();
	CHECK(n == 18 + 24);
	rt_lock();
	n = rt_bpf_read(ch, A, buf, 2048, &intr);
	rt_unlock();
	CHECK(n == 0);

	/* write: one Ethernet frame to the driver */
	{
		u8 fr[60];
		memset(fr, 0x5a, sizeof(fr));
		memcpy(fr, peer, 6);
		memcpy(fr + 6, mac0, 6);
		rt_put_be16(fr + 12, 0x88b5);
		nrec = 0;
		CHECK(rt_bpf_write(ch, A, fr, 60) == 60);
		CHECK(nrec == 1 && rec[0].et == 0x88b5 && rec[0].len == 46 && !memcmp(rec[0].dst, peer, 6));
		memset(fr, 0xff, 6);
		CHECK(rt_bpf_write(ch, A, fr, 60) == 60 && nrec == 2 && rec[1].bcast);
		CHECK(rt_bpf_write(ch, A, fr, 10) == -RT_EINVAL);
		CHECK(rt_bpf_write(ch, A, buf, 14 + 1600) == -RT_EMSGSIZE);
		CHECK(rt_bpf_write(ch, B, fr, 60) == -RT_EPERM);
	}

	/* promiscuous mode until the last channel that wanted it goes */
	{
		int ch2 = rt_bpf_open(-1, B);
		CHECK(ch2 == 1 && setif(ch2, B, "eth0") == 0);
		CHECK(rt_bpf_ioctl(ch, A, RT_BIOCPROMISC, NULL) == 0);
		CHECK(rt_bpf_ioctl(ch2, B, RT_BIOCPROMISC, NULL) == 0);
		CHECK((eth0.flags & RT_IFF_PROMISC) && (drv_flags & RT_IFF_PROMISC));
		CHECK(rt_bpf_close(ch2, B) == 0);
		CHECK(eth0.flags & RT_IFF_PROMISC);
		CHECK(rt_bpf_close(ch, A) == 0);
		CHECK(!(eth0.flags & RT_IFF_PROMISC) && !(drv_flags & RT_IFF_PROMISC));
		CHECK(!rt_bpf_on);
	}
}

static void test_loopback_and_removal(void)
{
	u8 buf[4096];
	u32 v = 1, intr = 0, af;
	int ch = rt_bpf_open(3, A), n;

	CHECK(ch == 3);
	CHECK(setif(ch, A, "lo0") == 0);
	CHECK(rt_bpf_ioctl(ch, A, RT_BIOCGDLT, &v) == 0 && v == RT_DLT_NULL);
	v = 1;
	CHECK(rt_bpf_ioctl(ch, A, RT_BIOCIMMEDIATE, &v) == 0);
	{
		struct rt_buf *b = rt_buf_alloc(RT_BUF_HEADROOM, 20);
		u8 *p = rt_buf_put(b, 20);
		memset(p, 0, 20);
		p[0] = 0x45;
		rt_netif_input(rt_loopback_if, b, RT_ETH_P_IP, NULL, 0);
	}
	n = rt_bpf_read(ch, A, buf, RT_BPF_BUFSIZE, &intr);
	CHECK(n == 20 + 4 + 20);                 /* header padded to 20: the IP header aligned */
	CHECK(get16(buf + 16) == 20 && get32(buf + 12) == 24);
	af = get32(buf + 20);
	CHECK(af == RT_AF_INET && buf[24] == 0x45);
	/* sending on lo0 is not seen twice */
	{
		struct rt_buf *b = rt_buf_alloc(RT_BUF_HEADROOM, 20);
		memset(rt_buf_put(b, 20), 0, 20);
		rt_bpf_tap(rt_loopback_if, b, 1, RT_ETH_P_IP, NULL, NULL, 0);
		rt_buf_free(b);
	}
	CHECK(rt_bpf_waiting(ch, A) == 0);
	CHECK(rt_bpf_close(ch, A) == 0);

	/* the interface goes away: its channels are detached */
	ch = rt_bpf_open(-1, A);
	CHECK(setif(ch, A, "eth0") == 0 && rt_bpf_on);
	rt_netif_remove(&eth0);
	CHECK(!rt_bpf_on);
	CHECK(rt_bpf_ioctl(ch, A, RT_BIOCGDLT, &v) == -RT_EINVAL);
	CHECK(rt_bpf_read(ch, A, buf, RT_BPF_BUFSIZE, &intr) == -RT_EINVAL);
	rt_bpf_close_owner(A);
	CHECK(rt_bpf_close(ch, A) == -RT_ENXIO);
}

int main(void)
{
	CHECK(rt_init() == 0);
	rt_log_level = 0;
	rt_strlcpy(eth0.name, "eth0", sizeof(eth0.name));
	eth0.type = RT_IFT_ETHER;
	eth0.hwlen = 6;
	memcpy(eth0.hwaddr, mac0, 6);
	eth0.flags = RT_IFF_BROADCAST | RT_IFF_MULTICAST | RT_IFF_SIMPLEX;
	eth0.mtu = 1500;
	eth0.link_output = fake_output;
	eth0.ioctl_flags = fake_flags;
	CHECK(rt_netif_add(&eth0) == 0);
	CHECK(rt_netif_set_addr(&eth0, rt_htonl(0xc0a80a02u), rt_htonl(0xffffff00u), 0) == 0);
	rt_netif_up(&eth0);
	test_machine();
	test_channels();
	test_ioctl_and_capture();
	test_loopback_and_removal();
	printf("t_bpf_unit: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
