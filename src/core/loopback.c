/*
 * Riptide - loopback interface lo0 (127.0.0.1/8).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Packets sent to lo0 (or to any of our own addresses, which route via
 * lo0) are not delivered recursively: they go on a deferred queue that
 * rt_stack_poll() drains. This keeps the call depth of a send() bounded
 * (Amiga client stacks are small) and avoids re-entering a protocol that
 * is in the middle of transmitting.
 */
#include "infra.h"

#define LOOP_MTU      16384     /* as 4.4BSD */
#define LOOP_QLEN_MAX 512

struct rt_netif *rt_loopback_if;

static struct rt_netif lo_if;
static struct rt_bufq lo_queue;

static int lo_output(struct rt_netif *nif, struct rt_buf *b, const u8 *dsthw, u16 ethertype)
{
	if (lo_queue.qlen >= LOOP_QLEN_MAX) {
		nif->stats.iqdrops++;
		rt_buf_free(b);
		return -RT_ENOBUFS;
	}
	b->l3_proto = ethertype;
	/* Locally generated: transport checksums were computed by us. */
	b->flags = RT_BUFF_LOOP | RT_BUFF_CSUM_OK;
	rt_bufq_tail(&lo_queue, b);
	rt_kick();
	return 0;
}

int rt_loopback_poll(void)
{
	/* Only what is queued now: replies generated while delivering wait for
	 * the next round, so a ping-pong cannot starve the timers. */
	u32 n = lo_queue.qlen;

	while (n-- > 0) {
		struct rt_buf *b = rt_bufq_dequeue(&lo_queue);
		if (!b)
			break;
		rt_netif_input(&lo_if, b, b->l3_proto, NULL, 0);
	}
	return !rt_bufq_empty(&lo_queue);
}

int rt_loopback_init(void)
{
	int r;

	rt_bufq_purge(&lo_queue);
	rt_bufq_init(&lo_queue);
	memset(&lo_if, 0, sizeof(lo_if));
	rt_strlcpy(lo_if.name, "lo0", sizeof(lo_if.name));
	lo_if.type = RT_IFT_LOOP;
	lo_if.flags = RT_IFF_LOOPBACK | RT_IFF_MULTICAST;
	lo_if.mtu = LOOP_MTU;
	lo_if.link_output = lo_output;
	r = rt_netif_add(&lo_if);
	if (r < 0)
		return r;
	rt_loopback_if = &lo_if;
	rt_netif_set_addr(&lo_if, RT_INADDR_LOOPBACK, rt_htonl(0xff000000u), 0);
	rt_netif_up(&lo_if);
	return 0;
}
