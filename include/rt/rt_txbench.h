/*
 * Riptide - send path benchmark (src/core/txbench.c): the stack uploads to
 * a scripted receiver behind a fake interface in the same task. Used by
 * tests/t_tx_bench.c and "Riptide BENCH TX".
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_TXBENCH_H
#define RT_TXBENCH_H

#include "rt/rt_types.h"

struct rt_txbench_result {
	u32 bytes, segs, acks, sends, ooo, bad, csum_bad, fin, retrans;
	u32 send_us;    /* in send(): copy and the segments it sends */
	u32 ack_us;     /* input of the receiver's frames (and what they let out) */
	u32 poll_us;    /* rt_stack_poll() */
	u32 total_us;   /* all of it, the receiver included */
};

/* once after rt_init(): the fake interface bench0 (10.10.0.2/24) */
int rt_txbench_setup(void);
/* upload total bytes in sendsize send()s, an ACK every ack_every segments;
 * 0 when every byte arrived intact, else -errno */
int rt_txbench(u32 total, u32 sendsize, u32 ack_every, struct rt_txbench_result *r);

#endif
