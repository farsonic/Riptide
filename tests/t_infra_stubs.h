/*
 * Riptide - weak stand-ins for the TCP module, so infra/UDP/ICMP tests link
 * and run whether or not src/core/tcp*.c is in the library yet. When the
 * real TCP objects are linked their strong definitions take precedence.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef T_INFRA_STUBS_H
#define T_INFRA_STUBS_H

#include "rt/riptide.h"

__attribute__((weak)) void rt_tcp_init(void) {}
__attribute__((weak)) void rt_tcp_input(struct rt_buf *b, const struct rt_iphdr *ip)
{
	(void)ip;
	rt_buf_free(b);
}
__attribute__((weak)) void rt_tcp_icmp_error(u8 t, u8 c, u32 i, const struct rt_iphdr *in, u32 l)
{
	(void)t; (void)c; (void)i; (void)in; (void)l;
}
__attribute__((weak)) int rt_tcp_list(struct rt_pcb_info *o, int m) { (void)o; (void)m; return 0; }
__attribute__((weak)) struct rt_tcp_stats rt_tcpstat;
__attribute__((weak)) const struct rt_proto_ops rt_tcp_ops;

#endif
