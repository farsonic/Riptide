/*
 * Riptide - 802.1Q VLAN interface against the Linux kernel's 802.1Q
 * framing over a TAP device. The container kernel may lack VLAN
 * interfaces (8021q), so the Linux side is a packet socket on the TAP:
 * it sends tagged frames and reads Riptide's replies, with the tag taken
 * from the frame or, where the kernel moved it to the packet's metadata on
 * receive, from PACKET_AUXDATA. Riptide: TAP "rtvN" 10.90.1.2/24 untagged,
 * VLAN 10 (10.90.10.2/24, PCP 5) on it. Checks: tagged ARP and ping on
 * VLAN 10 answered with VID 10 and PCP 5, a ping that fills the VLAN MTU
 * (a 1518 byte frame), untagged ARP answered untagged, IPv6 neighbour
 * discovery of the VLAN interface sent tagged, a VLAN Riptide lacks
 * ignored. Needs root and /dev/net/tun.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>

#include "rt/riptide.h"
#include "rt/rt_host.h"
#include "t_infra_stubs.h"

/* <net/if.h> clashes with rt_bsd.h (ifr_name ...) */
unsigned int if_nametoindex(const char *ifname);
#ifndef IFNAMSIZ
#define IFNAMSIZ 16
#endif

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fflush(stderr); } } while (0)

static const u8 rmac[6] = { 0x02, 0x00, 0x5e, 0x10, 0x90, 0x02 };   /* Riptide */
static const u8 lmac[6] = { 0x02, 0x00, 0x5e, 0x10, 0x90, 0x01 };   /* the Linux side */
static const u8 bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static char tap_name[IFNAMSIZ];
static int ps, ifindex;

static int sh(const char *fmt, ...)
{
	char cmd[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);
	return system(cmd);
}

static int config(const char *fmt, ...)
{
	char line[256], err[256];
	va_list ap;
	int r;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	rt_lock();
	r = rt_config_line(line, err, sizeof(err), NULL);
	rt_unlock();
	if (r < 0)
		fprintf(stderr, "config '%s': %s\n", line, err);
	return r;
}

static long ms_now(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000L + tv.tv_usec / 1000;
}

static u16 csum(const u8 *p, u32 len)
{
	u32 s = 0;
	while (len > 1) {
		s += (u32)(p[0] << 8 | p[1]);
		p += 2;
		len -= 2;
	}
	if (len)
		s += (u32)p[0] << 8;
	while (s >> 16)
		s = (s & 0xffff) + (s >> 16);
	return (u16)~s;
}

/* frame: dst src [8100 tci] type payload */
static int send_frame(const u8 *dst, int vid, int pcp, u16 type, const u8 *pl, u32 len)
{
	u8 f[1600];
	u32 o = 12;
	struct sockaddr_ll ll;

	memcpy(f, dst, 6);
	memcpy(f + 6, lmac, 6);
	if (vid >= 0) {
		rt_put_be16(f + o, 0x8100);
		rt_put_be16(f + o + 2, (u16)((pcp << 13) | vid));
		o += 4;
	}
	rt_put_be16(f + o, type);
	o += 2;
	memcpy(f + o, pl, len);
	memset(&ll, 0, sizeof(ll));
	ll.sll_family = AF_PACKET;
	ll.sll_ifindex = ifindex;
	ll.sll_halen = 6;
	memcpy(ll.sll_addr, dst, 6);
	return (int)sendto(ps, f, o + len, 0, (struct sockaddr *)&ll, sizeof(ll));
}

struct rx {
	int vid, pcp;            /* -1: untagged */
	u16 type;
	u32 len;
	u8 data[2048];           /* payload after the (inner) ethertype */
	u8 src[6];
};

/* next frame from Riptide (source rmac) within ms; 0 or -1 */
static int recv_frame(struct rx *r, long ms)
{
	long end = ms_now() + ms;
	u8 f[2048], cbuf[CMSG_SPACE(sizeof(struct tpacket_auxdata))];

	while (ms_now() < end) {
		struct pollfd pfd = { ps, POLLIN, 0 };
		struct iovec iov = { f, sizeof(f) };
		struct msghdr mh;
		struct cmsghdr *cm;
		ssize_t n;
		u32 o = 12;

		if (poll(&pfd, 1, (int)(end - ms_now())) <= 0)
			continue;
		memset(&mh, 0, sizeof(mh));
		mh.msg_iov = &iov;
		mh.msg_iovlen = 1;
		mh.msg_control = cbuf;
		mh.msg_controllen = sizeof(cbuf);
		n = recvmsg(ps, &mh, 0);
		if (n < 14 || memcmp(f + 6, rmac, 6))
			continue;
		r->vid = r->pcp = -1;
		for (cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm)) {
			struct tpacket_auxdata *ad = (void *)CMSG_DATA(cm);
			if (cm->cmsg_level == SOL_PACKET && cm->cmsg_type == PACKET_AUXDATA &&
			    (ad->tp_status & TP_STATUS_VLAN_VALID)) {
				r->vid = ad->tp_vlan_tci & 0xfff;
				r->pcp = ad->tp_vlan_tci >> 13;
			}
		}
		if (rt_get_be16(f + 12) == 0x8100) {
			u16 tci = rt_get_be16(f + 14);
			r->vid = tci & 0xfff;
			r->pcp = tci >> 13;
			o += 4;
		}
		r->type = rt_get_be16(f + o);
		o += 2;
		r->len = (u32)n - o;
		memcpy(r->data, f + o, r->len);
		memcpy(r->src, f + 6, 6);
		return 0;
	}
	return -1;
}

static void arp_req(u8 *p, const char *sip, const char *tip)
{
	u32 s = inet_addr(sip), t = inet_addr(tip);
	rt_put_be16(p, 1);
	rt_put_be16(p + 2, 0x0800);
	p[4] = 6;
	p[5] = 4;
	rt_put_be16(p + 6, 1);
	memcpy(p + 8, lmac, 6);
	memcpy(p + 14, &s, 4);
	memset(p + 18, 0, 6);
	memcpy(p + 24, &t, 4);
}

/* an ARP reply for 'who' with VID vid (-1 untagged) within ms */
static int arp_reply_seen(int vid, const char *who, int *pcp, long ms)
{
	struct rx r;
	u32 a = inet_addr(who);
	long end = ms_now() + ms;

	while (ms_now() < end && recv_frame(&r, end - ms_now()) == 0) {
		if (r.type == 0x0806 && r.vid == vid && rt_get_be16(r.data + 6) == 2 &&
		    !memcmp(r.data + 14, &a, 4) && !memcmp(r.data + 18, lmac, 6)) {
			if (pcp)
				*pcp = r.pcp;
			return 1;
		}
	}
	return 0;
}

static u32 echo_req(u8 *p, const char *src, const char *dst, u16 seq, u32 datalen)
{
	u32 s = inet_addr(src), d = inet_addr(dst), tot = 20 + 8 + datalen;
	memset(p, 0, tot);
	p[0] = 0x45;
	rt_put_be16(p + 2, (u16)tot);
	rt_put_be16(p + 4, seq);
	p[8] = 64;
	p[9] = 1;
	memcpy(p + 12, &s, 4);
	memcpy(p + 16, &d, 4);
	rt_put_be16(p + 10, csum(p, 20));
	p[20] = 8;
	rt_put_be16(p + 24, 0x1234);
	rt_put_be16(p + 26, seq);
	memset(p + 28, 0xa5, datalen);
	rt_put_be16(p + 22, csum(p + 20, 8 + datalen));
	return tot;
}

static int echo_reply_seen(int vid, u16 seq, u32 tot, struct rx *out, long ms)
{
	long end = ms_now() + ms;

	while (ms_now() < end && recv_frame(out, end - ms_now()) == 0)
		if (out->type == 0x0800 && out->vid == vid && out->len >= tot && out->data[9] == 1 &&
		    out->data[20] == 0 && rt_get_be16(out->data + 26) == seq && rt_get_be16(out->data + 2) == tot)
			return 1;
	return 0;
}

int main(void)
{
	struct rt_netif *tap, *v10;
	struct sockaddr_ll ll;
	struct rx r;
	u8 pl[1600];
	int one = 1, pcp = -2, seen6 = 0;
	u32 n;
	long end;

	setvbuf(stdout, NULL, _IOLBF, 0);
	if (access("/dev/net/tun", R_OK | W_OK) != 0 || geteuid() != 0 || system("ip -V >/dev/null 2>&1") != 0) {
		printf("t_vlan_tap: SKIP (needs root, /dev/net/tun and iproute2)\n");
		return 77;
	}
	alarm(120);
	if (rt_host_start() != 0)
		return 1;
	snprintf(tap_name, sizeof(tap_name), "rtv%d", (int)(getpid() % 100000));
	tap = rt_host_tap_open(tap_name, rmac);
	if (!tap) {
		printf("t_vlan_tap: SKIP (cannot create TAP device)\n");
		return 77;
	}
	if (sh("ip link set %s address 02:00:5e:10:90:01 up", tap_name)) {
		printf("t_vlan_tap: SKIP (cannot configure %s)\n", tap_name);
		return 77;
	}
	ifindex = (int)if_nametoindex(tap_name);
	ps = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
	CHECK(ps >= 0);
	setsockopt(ps, SOL_PACKET, PACKET_AUXDATA, &one, sizeof(one));
	memset(&ll, 0, sizeof(ll));
	ll.sll_family = AF_PACKET;
	ll.sll_protocol = htons(ETH_P_ALL);
	ll.sll_ifindex = ifindex;
	CHECK(bind(ps, (struct sockaddr *)&ll, sizeof(ll)) == 0);

	CHECK(config("ifconfig %s 10.90.1.2/24 up", tap_name) == 0);
	CHECK(config("interface %s.10 vlan=10 parent=%s pcp=5", tap_name, tap_name) == 0);
	CHECK(config("ifconfig %s.10 10.90.10.2/24 up", tap_name) == 0);
	{
		char nm[IFNAMSIZ + 4];
		snprintf(nm, sizeof(nm), "%s.10", tap_name);
		v10 = rt_netif_find(nm);
	}
	CHECK(v10 && v10->mtu == 1496);

	/* IPv6 of the VLAN interface: DAD/RS from its link-local, tagged 10 */
	end = ms_now() + 4000;
	while (ms_now() < end && !seen6)
		if (recv_frame(&r, end - ms_now()) == 0 && r.type == 0x86dd && r.vid == 10 &&
		    r.data[8] == 0xfe && r.data[24] != 0)
			seen6 = 1;
	CHECK(seen6);

	/* tagged ARP on VLAN 10: answered tagged 10, PCP 5 */
	arp_req(pl, "10.90.10.1", "10.90.10.2");
	CHECK(send_frame(bcast, 10, 0, 0x0806, pl, 28) > 0);
	CHECK(arp_reply_seen(10, "10.90.10.2", &pcp, 2000));
	CHECK(pcp == 5);
	/* untagged ARP for the parent's address: answered untagged */
	arp_req(pl, "10.90.1.1", "10.90.1.2");
	CHECK(send_frame(bcast, -1, 0, 0x0806, pl, 28) > 0);
	CHECK(arp_reply_seen(-1, "10.90.1.2", NULL, 2000));
	/* ping on VLAN 10 */
	n = echo_req(pl, "10.90.10.1", "10.90.10.2", 1, 56);
	CHECK(send_frame(rmac, 10, 0, 0x0800, pl, n) > 0);
	CHECK(echo_reply_seen(10, 1, n, &r, 2000));
	CHECK(r.pcp == 5);
	/* one that fills the VLAN MTU: 1496 bytes of IP, a 1518 byte frame */
	n = echo_req(pl, "10.90.10.1", "10.90.10.2", 2, 1496 - 28);
	CHECK(send_frame(rmac, 10, 0, 0x0800, pl, n) > 0);
	CHECK(echo_reply_seen(10, 2, n, &r, 2000));
	/* VLAN 11 is not configured: nothing comes back */
	arp_req(pl, "10.90.10.1", "10.90.10.2");
	CHECK(send_frame(bcast, 11, 0, 0x0806, pl, 28) > 0);
	CHECK(!arp_reply_seen(11, "10.90.10.2", NULL, 800));
	CHECK(tap->stats.noproto > 0);
	printf("VLAN 10 on %s: %u frames in, %u out\n", tap_name, v10 ? v10->stats.ipackets : 0,
	       v10 ? v10->stats.opackets : 0);

	close(ps);
	rt_host_stop();
	printf("t_vlan_tap: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
