/*
 * Riptide test helper - execrace DIR SECS [quiet]: exec only, no TCP/IP.
 *
 * A task "pong" (priority 2) and this process (priority 0) wake each
 * other with Signal()/Wait() as fast as they can for SECS seconds; this
 * process appends a line to DIR/execrace.log every 50 rounds, and a task
 * "noise" (unless "quiet") keeps timer.device busy with 3 ms requests.
 * At the end DIR/execrace.done holds the round counts.
 *
 * On FS-UAE 3.2.35 with the AROS Kickstart ROM, under host load, one of
 * the two tasks ends up in state TS_READY but in neither exec's TaskReady
 * nor TaskWait list, and never runs again: the run hangs. Riptide's stack
 * process (woken by Signal() for every packet) was lost the same way in
 * rare bsdsocktest runs; see run_fsuae_execrace.sh and taskprobe.c.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <exec/types.h>
#include <exec/tasks.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <devices/timer.h>
#include <clib/alib_protos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct Task *main_task, *pong_task;
static volatile ULONG pong_count;
static BYTE pong_bit = -1, back_bit = -1;

static void __saveds pong(void)
{
	pong_bit = AllocSignal(-1);
	Signal(main_task, SIGBREAKF_CTRL_F);
	for (;;) {
		Wait(1UL << pong_bit);
		pong_count++;
		Signal(main_task, 1UL << back_bit);
	}
}

static void __saveds noise(void)
{
	for (;;) {
		struct MsgPort *p = CreateMsgPort();
		struct timerequest *t = p ? (struct timerequest *)CreateIORequest(p, sizeof(*t)) : NULL;
		if (t && OpenDevice((STRPTR)"timer.device", UNIT_MICROHZ, (struct IORequest *)t, 0) == 0) {
			int i;
			for (i = 0; i < 100; i++) {
				t->tr_node.io_Command = TR_ADDREQUEST;
				t->tr_time.tv_secs = 0;
				t->tr_time.tv_micro = 3000;
				DoIO((struct IORequest *)t);
			}
			CloseDevice((struct IORequest *)t);
		}
		if (t)
			DeleteIORequest((struct IORequest *)t);
		if (p)
			DeleteMsgPort(p);
	}
}

static long seconds_since(const struct DateStamp *a)
{
	struct DateStamp b;

	DateStamp(&b);
	return (b.ds_Days - a->ds_Days) * 86400 + (b.ds_Minute - a->ds_Minute) * 60 + (b.ds_Tick - a->ds_Tick) / 50;
}

int main(int argc, char **argv)
{
	char path[200], line[64];
	long secs = argc > 2 ? atol(argv[2]) : 240;
	ULONG rounds = 0;
	struct DateStamp d0;
	BPTR log;

	if (argc < 2) {
		printf("usage: execrace DIR SECS [quiet]\n");
		return 20;
	}
	main_task = FindTask(NULL);
	back_bit = AllocSignal(-1);
	SetSignal(0, SIGBREAKF_CTRL_F);
	pong_task = CreateTask((STRPTR)"pong", 2, (APTR)pong, 4096);
	if (!pong_task || back_bit < 0)
		return 20;
	Wait(SIGBREAKF_CTRL_F);
	if (argc < 4 || strcmp(argv[3], "quiet"))
		CreateTask((STRPTR)"noise", 0, (APTR)noise, 4096);
	sprintf(path, "%s/execrace.log", argv[1]);
	log = Open((STRPTR)path, MODE_NEWFILE);
	DateStamp(&d0);
	while (seconds_since(&d0) < secs) {
		Signal(pong_task, 1UL << pong_bit);
		Wait((1UL << back_bit) | SIGBREAKF_CTRL_C);
		if (SetSignal(0, 0) & SIGBREAKF_CTRL_C)
			break;
		if (++rounds % 50 == 0 && log) {
			sprintf(line, "round %lu\n", (unsigned long)rounds);
			Write(log, line, strlen(line));
		}
	}
	if (log)
		Close(log);
	sprintf(path, "%s/execrace.done", argv[1]);
	log = Open((STRPTR)path, MODE_NEWFILE);
	if (log) {
		sprintf(line, "rounds %lu pong %lu\n", (unsigned long)rounds, (unsigned long)pong_count);
		Write(log, line, strlen(line));
		Close(log);
	}
	/* the helper tasks live on until reboot: the test ends the emulator */
	return 0;
}
