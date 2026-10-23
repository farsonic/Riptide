/*
 * Riptide - send path benchmark on the host (src/core/txbench.c; on the
 * Amiga "Riptide BENCH TX"): the stack uploads to a scripted receiver,
 * which checks every byte and checksum. Single threaded, so the
 * instruction count per segment is reproducible (valgrind
 * --tool=callgrind; for 68k instructions build with CROSS=m68k-linux-gnu-
 * and run under qemu-m68k -one-insn-per-tb -d exec,nochain).
 *
 *   t_tx_bench [BYTES] [SENDSIZE] [ACKEVERY]
 * defaults 1 MB, 16384 (netbench's send size), 2 (segments per ACK).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <stdlib.h>

#include "rt/riptide.h"
#include "rt/rt_txbench.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(int argc, char **argv)
{
	struct rt_txbench_result r;
	u32 total = argc > 1 ? (u32)strtoul(argv[1], NULL, 0) : 1u << 20;
	u32 sendsize = argc > 2 ? (u32)strtoul(argv[2], NULL, 0) : 16384;
	u32 ack_every = argc > 3 ? (u32)strtoul(argv[3], NULL, 0) : 2;

	CHECK(rt_init() == 0);
	CHECK(rt_txbench_setup() == 0);
	CHECK(rt_txbench(total, sendsize, ack_every, &r) == 0);
	CHECK(r.bytes == total && r.bad == 0 && r.csum_bad == 0 && r.fin);
	CHECK(r.retrans == 0 && r.ooo == 0);
	printf("t_tx_bench: %lu bytes in %lu segments, %lu ACKs, %lu send() calls; "
	       "us: send %lu, ACK input %lu, poll %lu, total %lu\n",
	       (unsigned long)r.bytes, (unsigned long)r.segs, (unsigned long)r.acks, (unsigned long)r.sends,
	       (unsigned long)r.send_us, (unsigned long)r.ack_us, (unsigned long)r.poll_us,
	       (unsigned long)r.total_us);
	printf("t_tx_bench: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
