/*
 * Riptide - UDP and raw socket semantics over lo0 through the descriptor
 * API, with the host stack thread running (no root needed).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <string.h>

#include "rt/riptide.h"
#include "rt/rt_host.h"
#include "t_infra_stubs.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static struct rt_sockaddr_in sa(u32 host_order_addr, u16 port)
{
	struct rt_sockaddr_in s;

	memset(&s, 0, sizeof(s));
	s.sin_len = sizeof(s);
	s.sin_family = RT_AF_INET;
	s.sin_addr = rt_htonl(host_order_addr);
	s.sin_port = rt_htons(port);
	return s;
}

static void set_timeout(struct rt_sockctx *c, int fd, u32 ms)
{
	struct rt_timeval tv;

	tv.tv_secs = ms / 1000;
	tv.tv_micro = (ms % 1000) * 1000;
	rt_api_setsockopt(c, fd, RT_SOL_SOCKET, RT_SO_RCVTIMEO, &tv, sizeof(tv));
}

int main(void)
{
	struct rt_sockctx c;
	struct rt_sockaddr_in a, from;
	u32 alen, flen;
	char buf[2048];
	int s1, s2, s3, r, one = 1, i;
	u32 v;

	CHECK(rt_host_start() == 0);
	CHECK(rt_ctx_init(&c, 64, NULL) == 0);

	s1 = rt_api_socket(&c, RT_AF_INET, RT_SOCK_DGRAM, 0);
	s2 = rt_api_socket(&c, RT_AF_INET, RT_SOCK_DGRAM, 0);
	CHECK(s1 >= 0 && s2 >= 0);
	set_timeout(&c, s1, 2000);
	set_timeout(&c, s2, 2000);
	a = sa(0x7f000001, 5001);
	CHECK(rt_api_bind(&c, s1, &a, sizeof(a)) == 0);
	/* same port again without reuse options: EADDRINUSE */
	CHECK(rt_api_bind(&c, s2, &a, sizeof(a)) == -RT_EADDRINUSE);
	a = sa(0x0a0b0c0d, 5002);            /* not one of our addresses */
	CHECK(rt_api_bind(&c, s2, &a, sizeof(a)) == -RT_EADDRNOTAVAIL);

	/* unbound sender gets an ephemeral port in 49152..65535 */
	a = sa(0x7f000001, 5001);
	CHECK(rt_api_sendto(&c, s2, "hello", 5, 0, &a, sizeof(a)) == 5);
	flen = sizeof(from);
	r = rt_api_recvfrom(&c, s1, buf, sizeof(buf), 0, &from, &flen);
	CHECK(r == 5 && memcmp(buf, "hello", 5) == 0);
	CHECK(from.sin_family == RT_AF_INET && from.sin_addr == rt_htonl(0x7f000001) &&
	      rt_ntohs(from.sin_port) >= 49152);
	alen = sizeof(a);
	CHECK(rt_api_getsockname(&c, s2, &a, &alen) == 0 && a.sin_port == from.sin_port);

	/* reply to the sender's address */
	CHECK(rt_api_sendto(&c, s1, "world!", 6, 0, &from, sizeof(from)) == 6);
	r = rt_api_recv(&c, s2, buf, sizeof(buf), 0);
	CHECK(r == 6 && memcmp(buf, "world!", 6) == 0);

	/* MSG_PEEK, FIONREAD, truncation (MSG_TRUNC via recvmsg) */
	a = sa(0x7f000001, 5001);
	CHECK(rt_api_sendto(&c, s2, "0123456789", 10, 0, &a, sizeof(a)) == 10);
	CHECK(rt_api_sendto(&c, s2, "abc", 3, 0, &a, sizeof(a)) == 3);
	rt_api_recv(&c, s1, buf, 0, RT_MSG_PEEK);   /* wait for arrival */
	for (i = 0; i < 100; i++) {
		v = 0;
		rt_api_ioctl(&c, s1, RT_FIONREAD, &v);
		if (v == 13)
			break;
		rt_api_recv(&c, s1, buf, 0, RT_MSG_PEEK | RT_MSG_DONTWAIT);
	}
	CHECK(v == 13);
	CHECK(rt_api_recv(&c, s1, buf, 4, RT_MSG_PEEK) == 4 && memcmp(buf, "0123", 4) == 0);
	{
		struct rt_iov iov = { buf, 4 };
		int mf = 0;
		flen = sizeof(from);
		r = rt_api_recvmsg(&c, s1, &iov, 1, 0, &from, &flen, &mf);
		CHECK(r == 4 && (mf & RT_MSG_TRUNC));
	}
	CHECK(rt_api_recv(&c, s1, buf, sizeof(buf), 0) == 3 && memcmp(buf, "abc", 3) == 0);
	CHECK(rt_api_recv(&c, s1, buf, sizeof(buf), RT_MSG_DONTWAIT) == -RT_EWOULDBLOCK);

	/* connected socket + closed port -> ICMP port unreachable -> ECONNREFUSED */
	a = sa(0x7f000001, 5999);
	CHECK(rt_api_connect(&c, s2, &a, sizeof(a)) == 0);
	CHECK(rt_api_sendto(&c, s2, "x", 1, 0, &a, sizeof(a)) == -RT_EISCONN);
	CHECK(rt_api_send(&c, s2, "x", 1, 0) == 1);
	r = rt_api_recv(&c, s2, buf, sizeof(buf), 0);
	CHECK(r == -RT_ECONNREFUSED);
	alen = sizeof(a);
	CHECK(rt_api_getpeername(&c, s2, &a, &alen) == 0 && a.sin_port == rt_htons(5999));
	/* unconnected send without address */
	s3 = rt_api_socket(&c, RT_AF_INET, RT_SOCK_DGRAM, 0);
	CHECK(rt_api_send(&c, s3, "x", 1, 0) == -RT_ENOTCONN);
	/* datagram larger than the send buffer (BSD: 9216) */
	a = sa(0x7f000001, 5001);
	CHECK(rt_api_sendto(&c, s3, buf, 9217 > sizeof(buf) ? 2048 : 9217, 0, &a, sizeof(a)) > 0);
	{
		static char big[10000];
		CHECK(rt_api_sendto(&c, s3, big, sizeof(big), 0, &a, sizeof(a)) == -RT_EMSGSIZE);
		CHECK(rt_api_sendto(&c, s3, big, 9000, 0, &a, sizeof(a)) == 9000);
		set_timeout(&c, s1, 2000);
		CHECK(rt_api_recv(&c, s1, big, sizeof(big), 0) == 2048);
		CHECK(rt_api_recv(&c, s1, big, sizeof(big), 0) == 9000);
	}
	/* broadcast needs SO_BROADCAST */
	a = sa(0xffffffff, 5001);
	CHECK(rt_api_sendto(&c, s3, "b", 1, 0, &a, sizeof(a)) == -RT_EACCES);
	CHECK(rt_api_setsockopt(&c, s3, RT_SOL_SOCKET, RT_SO_BROADCAST, &one, sizeof(one)) == 0);
	r = rt_api_sendto(&c, s3, "b", 1, 0, &a, sizeof(a));
	CHECK(r == 1 || r == -RT_ENETUNREACH);    /* no broadcast interface on a bare host */
	/* IP_TTL option round trip */
	{
		int ttl = 7, got = 0;
		u32 gl = sizeof(got);
		CHECK(rt_api_setsockopt(&c, s3, RT_IPPROTO_IP, RT_IP_TTL, &ttl, sizeof(ttl)) == 0);
		CHECK(rt_api_getsockopt(&c, s3, RT_IPPROTO_IP, RT_IP_TTL, &got, &gl) == 0 && got == 7);
		ttl = 300;
		CHECK(rt_api_setsockopt(&c, s3, RT_IPPROTO_IP, RT_IP_TTL, &ttl, sizeof(ttl)) == -RT_EINVAL);
	}
	/* SO_REUSEADDR on both: shared port; unicast goes to one of them */
	{
		int r1 = rt_api_socket(&c, RT_AF_INET, RT_SOCK_DGRAM, 0);
		int r2 = rt_api_socket(&c, RT_AF_INET, RT_SOCK_DGRAM, 0);
		rt_api_setsockopt(&c, r1, RT_SOL_SOCKET, RT_SO_REUSEADDR, &one, sizeof(one));
		rt_api_setsockopt(&c, r2, RT_SOL_SOCKET, RT_SO_REUSEADDR, &one, sizeof(one));
		a = sa(0, 5050);
		CHECK(rt_api_bind(&c, r1, &a, sizeof(a)) == 0);
		CHECK(rt_api_bind(&c, r2, &a, sizeof(a)) == 0);
		rt_api_close(&c, r1);
		rt_api_close(&c, r2);
	}

	/* raw ICMP socket: ping 127.0.0.1, the kernel answers, raw gets the reply */
	{
		int rs = rt_api_socket(&c, RT_AF_INET, RT_SOCK_RAW, RT_IPPROTO_ICMP);
		u8 pkt[16] = { 8, 0, 0, 0, 0x42, 0x42, 0, 1, 'r', 'i', 'p', 't', 'i', 'd', 'e', '!' };
		u16 ck = rt_inet_csum(pkt, sizeof(pkt));
		int got_reply = 0;

		CHECK(rs >= 0);
		set_timeout(&c, rs, 2000);
		memcpy(pkt + 2, &ck, 2);
		a = sa(0x7f000001, 0);
		CHECK(rt_api_sendto(&c, rs, pkt, sizeof(pkt), 0, &a, sizeof(a)) == sizeof(pkt));
		for (i = 0; i < 4 && !got_reply; i++) {
			r = rt_api_recv(&c, rs, buf, sizeof(buf), 0);
			if (r == 20 + (int)sizeof(pkt) && (buf[0] & 0xf0) == 0x40 && (u8)buf[20] == 0 &&
			    memcmp(buf + 28, "riptide!", 8) == 0)
				got_reply = 1;
		}
		CHECK(got_reply);
		rt_api_close(&c, rs);
	}

	rt_api_close(&c, s1);
	rt_api_close(&c, s2);
	rt_api_close(&c, s3);
	{
		struct rt_pcb_info info[8];
		rt_lock();
		r = rt_udp_list(info, 8);
		rt_unlock();
		CHECK(r == 0);
	}
	rt_ctx_destroy(&c);
	rt_host_stop();
	printf("t_udp_loop: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
