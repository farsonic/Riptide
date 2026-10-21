/*
 * Riptide - Linux host environment (src/host/): stack thread, drivers for
 * TUN and a simulated Ethernet wire, and a link impairment layer. Used by
 * the host tests only; nothing in src/core depends on it.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_HOST_H
#define RT_HOST_H

#include "rt_types.h"
#include "rt_netif.h"

/* rt_init() and start the stack thread, which loops:
 *   lock; ms = rt_stack_poll(); unlock;
 *   poll(driver fds + kick fd, ms);
 *   for each readable driver fd: lock; rx(arg); unlock.
 * Call without the lock. Returns 0 or -errno. */
int  rt_host_start(void);
/* Stop and join the stack thread (interfaces and sockets stay). */
void rt_host_stop(void);
/* Register a file descriptor watched by the stack thread; rx(arg) is called
 * with the stack lock held whenever fd is readable (it should drain what is
 * readable without blocking). Returns 0 or -errno. Safe to call with or
 * without the lock, before or after rt_host_start(). */
int  rt_host_register_fd(int fd, void (*rx)(void *arg), void *arg);
void rt_host_unregister_fd(int fd);

/* Tests: move rt_now_ms()/rt_now_us() forward by ms (lifetimes, timers). */
void rt_host_advance_clock(u32 ms);

/* Memory accounting of rt_malloc() (outstanding blocks and bytes). */
void rt_host_mem_stats(u32 *blocks, u32 *bytes);

/* ---- drivers. All take the stack lock themselves: call WITHOUT it. ---- */

/* Open a Linux TUN device (IFF_TUN | IFF_NO_PI) named 'ifname' (may be
 * "" for a kernel chosen name) and create a matching PTP interface with
 * the same name, address 'local', peer 'peer_or_0' (0: use netmask as a
 * plain subnet) and 'netmask' (all network order). The interface is up on
 * return. The Linux side of the TUN device is NOT configured. Returns NULL
 * if /dev/net/tun is missing or not permitted. */
struct rt_netif *rt_host_tun_open(const char *ifname, u32 local, u32 peer_or_0, u32 netmask);
/* Linux name of the TUN device behind nif (valid for TUN netifs). */
const char *rt_host_tun_name(const struct rt_netif *nif);
/* Open a Linux TAP device (IFF_TAP | IFF_NO_PI, Ethernet frames) named
 * 'ifname' ("" for a kernel chosen name) as an Ethernet interface with the
 * same name and MAC address 'mac'. Like rt_host_wire_open() it is down and
 * has no address; the Linux side is not configured. NULL if /dev/net/tun
 * is missing or not permitted. */
struct rt_netif *rt_host_tap_open(const char *ifname, const u8 mac[6]);
const char *rt_host_tap_name(const struct rt_netif *nif);

/* Simulated Ethernet: frames {dst mac, src mac, ethertype(be16), payload}
 * travel as AF_UNIX SOCK_DGRAM datagrams from my_sock_path to
 * peer_sock_path, so two processes (or two threads with separate stack
 * instances in separate processes) form a wire. The interface is named
 * "eth<N>", is down and has no address: configure it with
 * rt_netif_set_addr() + rt_netif_up() under the lock. */
struct rt_netif *rt_host_wire_open(const char *my_sock_path, const char *peer_sock_path,
                                   const u8 mac[6]);

/* Transmit-side impairment of any netif (wraps nif->link_output).
 * Percentages are in units of 0.01% (10000 = 100%). A zero config turns
 * the impairment off. With seed != 0 the decisions are deterministic. */
struct rt_host_impair {
	u32 loss;            /* drop probability, 1/10000 */
	u32 reorder;         /* probability a packet is held back by reorder_ms */
	u32 duplicate;       /* probability a packet is sent twice */
	u32 delay_ms;        /* fixed one-way delay */
	u32 jitter_ms;       /* + uniform random 0..jitter_ms */
	u32 reorder_ms;      /* extra delay of reordered packets (default 10) */
	u32 rate_kbit;       /* serialisation rate limit, 0 = unlimited */
	u32 queue_limit;     /* max packets waiting (default 1000; tail drop) */
	u32 seed;            /* PRNG seed, 0 = random */
};
/* Call WITHOUT the lock. Counters of what was done are kept per netif. */
void rt_host_impair(struct rt_netif *nif, const struct rt_host_impair *cfg);
struct rt_host_impair_stats {
	u32 sent, dropped, duplicated, reordered, queue_drops;
};
void rt_host_impair_get_stats(struct rt_netif *nif, struct rt_host_impair_stats *st);

#endif
