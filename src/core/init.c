/*
 * Riptide - stack initialisation and the stack thread's poll entry.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "infra.h"

/* Provided by the sockets team (socket.c, sysctl.c, resolv.c). */
extern void rt_socket_init(void);
extern void rt_sysctl_init(void);
extern void rt_resolv_init(void);

static int rt_initialised;

int rt_init(void)
{
	int r;

	if (rt_initialised)
		return 0;
	/* Called without the lock and before the stack thread runs, so no
	 * one else can be inside the stack yet. Module inits that register
	 * sysctls need rt_sysctl_init() first; the resolver comes last
	 * because it may open a socket. */
	rt_sysctl_init();
	rt_arch_init();
	rt_netif_init();
	rt_route_init();
	rt_arp_init();
	rt_ipv4ll_init();
	rt_ip_init();
	rt_ip6_init();
	rt_inet_init();
	rt_icmp_init();
	rt_udp_init();
	rt_igmp_init();
	rt_raw_init();
	rt_tcp_init();
	rt_socket_init();
	rt_ctl_init();
	rt_qos_init();
	rt_he_init();
	rt_diag_init();
	rt_lock();
	r = rt_loopback_init();
	rt_unlock();
	if (r < 0)
		return r;
	rt_resolv_init();
	rt_netsvc_init();
	rt_initialised = 1;
	return 0;
}

u32 rt_stack_poll(void)
{
	u32 now = rt_now_ms(), next;
	int more = rt_loopback_poll();

	RT_PROF_ENTER(RT_PROF_TIMERS);
	next = rt_timers_run(now);
	RT_PROF_EXIT(RT_PROF_TIMERS);
	if (more || rt_loopback_poll())
		return 0;
	return next;
}
