/*
 * Riptide test helper - clkjump SECS OFFSET: wait SECS seconds, then move
 * the system time by OFFSET seconds with timer.device TR_SETSYSTIME, as
 * Riptide's NTP client does (tests/amiga/run_fsuae_speedtest.sh).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <exec/types.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/timer.h>
#include <stdlib.h>
struct Device *TimerBase;
int main(int argc, char **argv)
{
	struct MsgPort *p = CreateMsgPort();
	struct timerequest *t = (struct timerequest *)CreateIORequest(p, sizeof(*t));
	struct timeval tv;
	long wait = argc > 1 ? atol(argv[1]) : 4, off = argc > 2 ? atol(argv[2]) : -3600;
	if (OpenDevice("timer.device", UNIT_MICROHZ, (struct IORequest *)t, 0))
		return 20;
	TimerBase = t->tr_node.io_Device;
	t->tr_node.io_Command = TR_ADDREQUEST;
	t->tr_time.tv_secs = wait;
	t->tr_time.tv_micro = 0;
	DoIO((struct IORequest *)t);
	GetSysTime(&tv);
	tv.tv_secs += off;
	t->tr_node.io_Command = TR_SETSYSTIME;
	t->tr_time = tv;
	DoIO((struct IORequest *)t);
	CloseDevice((struct IORequest *)t);
	DeleteIORequest((struct IORequest *)t);
	DeleteMsgPort(p);
	return 0;
}
