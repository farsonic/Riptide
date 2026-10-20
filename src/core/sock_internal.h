/*
 * Riptide - private interfaces of the socket layer (socket.c, socket_api.c,
 * resolv.c, dhcp.c, sysctl.c, config.c). Not part of the contract.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_SOCK_INTERNAL_H
#define RT_SOCK_INTERNAL_H

#include "rt/riptide.h"

/* Private so->state bits, kept well above the public RT_SS_* range. */
#define SOCK_SS_DEAD    0x00010000u /* closed; freed by the last waiter */
#define SOCK_SS_UPCALL  0x00020000u /* kernel socket: so->owner is a sock_upcall */

/* Kernel sockets (no descriptor, used inside the stack thread, e.g. by the
 * DHCP client): rt_sock_wakeup() calls fn instead of rt_sock_async_hook.
 * fn runs with the lock held, possibly deep inside protocol input, so it
 * should only schedule work (e.g. arm a 0 ms timer). */
struct sock_upcall {
	void (*fn)(struct rt_socket *so, u32 events, void *arg);
	void *arg;
};

/* Tunables (sysctl), defined in socket.c */
extern s32 sock_tcp_sendspace, sock_tcp_recvspace;
extern s32 sock_udp_sendspace, sock_udp_recvspace;
extern s32 sock_raw_sendspace, sock_raw_recvspace;
extern s32 sock_maxsockbuf, sock_somaxconn;

/* Test hook: if set and it returns non-NULL, used instead of the built-in
 * protocol switch by socket creation. */
extern const struct rt_proto_ops *(*rt_sock_proto_hook)(int type, int protocol);

/* socket.c internals */
const struct rt_proto_ops *sock_proto_lookup(int type, int *protocol);
struct rt_socket *sock_alloc(int type, int protocol, const struct rt_proto_ops *ops);
void sock_free(struct rt_socket *so);
/* The last descriptor reference went away: close the protocol, abort
 * queued children, and free (or leave to the last waiter). */
void sock_close(struct rt_socket *so);
/* A waiter removed its entry from so->wq: free a dead socket if it was the
 * last one. */
void sock_waiter_done(struct rt_socket *so);
/* Create/close a kernel socket (no fd). Lock held. */
int  sock_kernel_create(int type, int protocol, struct sock_upcall *up,
                        struct rt_socket **out);
void sock_kernel_close(struct rt_socket *so);
/* readiness for select: RT_POLL* bits including socket-layer state */
u32  sock_poll(struct rt_socket *so);

/* init hooks called by rt_init() (init.c declares them itself) */
void rt_socket_init(void);
void rt_sysctl_init(void);
void rt_resolv_init(void);

/* resolv.c */
void rt_dns_remove_server(u32 addr);

#endif
