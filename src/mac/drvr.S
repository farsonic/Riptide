/* src/mac/drvr.S - Riptide on the classic Mac OS: the .IPP driver's header and entry points, its
 * self-relocation, and the assembly glue C cannot express (trap calls, the deferred task, the
 * Time Manager task, the .ENET protocol handler, completion and notification calls).
 *
 * The image is linked at address 0 and loaded wherever the Resource Manager puts the 'DRVR'
 * resource; tools/elf2mac.py appends the list of 32-bit absolute fields and stores its offset in
 * _reloc_off, and the first Open adds the load address to each before any C runs.
 *
 * Calling conventions: the C side is GCC's m68k SysV ABI (arguments on the stack, results in D0
 * and, for pointers, A0; D2-D7/A2-A6 preserved). The Mac side is register-based (Device Manager
 * entries: A0 = parameter block, A1 = DCE) or pascal (notification procedures).
 * SPDX-License-Identifier: GPL-2.0-only
 */
	.section .text.drvr,"ax"
	.globl	_drvr_start
_drvr_start:
	.short	0x4C00			| dNeedLock, dStatEnable, dCtlEnable
	.short	0			| drvrDelay
	.short	0			| drvrEMask
	.short	0			| drvrMenu
	.short	drv_open-_drvr_start
	.short	drv_prime-_drvr_start
	.short	drv_control-_drvr_start
	.short	drv_status-_drvr_start
	.short	drv_close-_drvr_start
	.byte	4
	.ascii	".IPP"
	.even
	.short	0x0100			| version 1.0 (after the name, word aligned)
	.globl	_reloc_off
_reloc_off:
	.long	0			| tools/elf2mac.py: offset of the relocation table
_relocated:
	.long	0

/* ---- Device Manager entries ------------------------------------------------------------- */
drv_open:
	movem.l	%d1-%d7/%a0-%a6,-(%sp)
#ifdef AURORA68_LOG
	lea	open_msg(%pc),%a1
	bsr	a68_log_asm
#endif
	bsr	relocate
	move.l	%a1,-(%sp)
	move.l	%a0,-(%sp)
	jsr	rt_mac_open
	addq.l	#8,%sp
	movem.l	(%sp)+,%d1-%d7/%a0-%a6
	rts

drv_close:
	moveq	#-24,%d0		| closErr: the stack stays (MacTCP's .IPP is never closed either)
	rts

drv_prime:
	moveq	#-17,%d0		| controlErr: no Read/Write on .IPP
	bra.s	iodone

drv_status:
	movem.l	%d1-%d7/%a0-%a6,-(%sp)
	move.l	%a1,-(%sp)
	move.l	%a0,-(%sp)
	jsr	rt_mac_status
	addq.l	#8,%sp
	movem.l	(%sp)+,%d1-%d7/%a0-%a6
	bra.s	iodone

drv_control:
	movem.l	%d1-%d7/%a0-%a6,-(%sp)
	move.l	%a1,-(%sp)
	move.l	%a0,-(%sp)
	jsr	rt_mac_control		| 1 = in progress (completed later by mac_complete), else the result
	addq.l	#8,%sp
	movem.l	(%sp)+,%d1-%d7/%a0-%a6
	cmp.l	#1,%d0
	beq.s	pending
iodone:
	move.w	6(%a0),%d1		| ioTrap: an immediate call returns straight away
	btst	#9,%d1
	bne.s	1f
	move.l	0x8FC,-(%sp)		| JIODone
1:	rts
pending:
	/* A concurrent driver (Inside Macintosh: Devices, "Writing a Device Driver"): the request
	   leaves our queue and the driver is marked idle, so the Device Manager hands us the next
	   one; ioResult stays 1 until mac_complete sets it and calls ioCompletion. */
	movem.l	%a0-%a1,-(%sp)
	lea	6(%a1),%a1		| dCtlQHdr
	.short	0xA96E			| _Dequeue: A0 = element, A1 = queue
	movem.l	(%sp)+,%a0-%a1
	bclr	#7,5(%a1)		| drvrActive
	moveq	#0,%d0
	rts

#ifdef AURORA68_LOG
open_msg:
	.ascii	"Riptide: driver Open entered"
	.byte	0
	.even
a68_log_asm:				| a1 = C string; position independent, before relocation
	movem.l	%d0-%d1/%a0,-(%sp)
	lea	0xF9000000,%a0
	tst.b	0x0CB2
	bne.s	1f
	lea	0x00900000,%a0
1:	lea	0x1000(%a0),%a0
	moveq	#0,%d1
2:	tst.b	(%a1,%d1.w)
	beq.s	3f
	addq.w	#1,%d1
	bra.s	2b
3:	move.w	#2,8(%a0)
	move.l	%a1,28(%a0)
	move.l	%d1,32(%a0)
	move.l	(%a0),%d0
	addq.l	#1,%d0
	move.l	%d0,(%a0)
	move.l	#200000,%d1
4:	cmp.l	4(%a0),%d0
	beq.s	5f
	subq.l	#1,%d1
	bne.s	4b
5:	movem.l	(%sp)+,%d0-%d1/%a0
	rts
#endif

/* ---- relocation: once, before any C ----------------------------------------------------- */
relocate:
	lea	_relocated(%pc),%a2
	tst.l	(%a2)
	bne.s	9f
	lea	_drvr_start(%pc),%a3
	move.l	_reloc_off(%pc),%d2
	lea	(%a3,%d2.l),%a4
	move.l	(%a4)+,%d3
	move.l	%a3,%d4
	bra.s	2f
1:	move.l	(%a4)+,%d2
	add.l	%d4,(%a3,%d2.l)
2:	subq.l	#1,%d3
	bcc.s	1b
	move.l	#1,(%a2)
	/* the fixed-up code must reach the instruction cache: HWPriv FlushInstructionCache,
	   System 7 and later (68020/030 caches are write-through and hold nothing of ours yet) */
	cmp.w	#0x0700,0x15A		| SysVersion
	bcs.s	9f
	moveq	#1,%d0
	.short	0xA198			| _HWPriv
9:	rts

/* ---- trap glue for C ---------------------------------------------------------------------- */
	.text
	.globl	mac_newptr_sys
mac_newptr_sys:				| void *(u32 size): NewPtrSysClear, NULL on failure
	move.l	4(%sp),%d0
	.short	0xA71E
	tst.w	%d0
	beq.s	1f
	sub.l	%a0,%a0
1:	move.l	%a0,%d0
	rts

	.globl	mac_microseconds
mac_microseconds:			| u64 (void): Microseconds, System 7 (D0:D1 = hi:lo)
	.short	0xA193
	move.l	%d0,%d1
	move.l	%a0,%d0
	rts

	.globl	mac_pbcontrol
mac_pbcontrol:				| int (void *pb, int async)
	move.l	4(%sp),%a0
	tst.l	8(%sp)
	bne.s	1f
	.short	0xA004
	ext.l	%d0
	rts
1:	.short	0xA404
	ext.l	%d0
	rts

	.globl	mac_pbopen
mac_pbopen:				| int (void *pb, int slot): _Open, or _OpenSlot
	move.l	4(%sp),%a0
	tst.l	8(%sp)
	bne.s	1f
	.short	0xA000
	ext.l	%d0
	rts
1:	.short	0xA200
	ext.l	%d0
	rts

	.globl	mac_pb
mac_pb:					| short (void *pb, long trap): a synchronous File Manager call
	move.l	4(%sp),%a0		| (_HOpen A200, _Read A002, _Close A001, _GetEOF A011, _HDelete A209);
	move.l	8(%sp),%d0		| the trap word is written into the code below, the cache flushed
	lea	2f(%pc),%a1
	move.w	%d0,(%a1)
	move.l	%a0,-(%sp)
	moveq	#1,%d0
	.short	0xA198			| _HWPriv: FlushInstructionCache
	move.l	(%sp)+,%a0
	nop
2:	.short	0xA000
	ext.l	%d0
	rts

	.globl	mac_findfolder
mac_findfolder:				| short (int vref, long type, int create, short *vrefout, long *dirout)
	link	%a6,#0
	move.l	%d2,-(%sp)
	clr.w	-(%sp)
	move.w	10(%a6),-(%sp)
	move.l	12(%a6),-(%sp)
	move.b	19(%a6),-(%sp)
	move.l	20(%a6),-(%sp)
	move.l	24(%a6),-(%sp)
	moveq	#0,%d0
	.short	0xA823			| _FindFolder
	move.w	(%sp)+,%d0
	ext.l	%d0
	move.l	-4(%a6),%d2
	unlk	%a6
	rts

	.globl	mac_slotmanager
mac_slotmanager:			| int (void *spblock, int selector)
	move.l	4(%sp),%a0
	move.l	8(%sp),%d0
	.short	0xA06E
	ext.l	%d0
	rts

	.globl	mac_dtinstall
mac_dtinstall:				| int (void *deferred task)
	move.l	4(%sp),%a0
	.short	0xA082
	ext.l	%d0
	rts

	.globl	mac_insxtime
mac_insxtime:				| void (void *tmtask)
	move.l	4(%sp),%a0
	.short	0xA458
	rts

	.globl	mac_primetime
mac_primetime:				| void (void *tmtask, s32 ms)
	move.l	4(%sp),%a0
	move.l	8(%sp),%d0
	.short	0xA05A
	rts

	.globl	mac_getresource
mac_getresource:			| void **(u32 type, int id): GetResource (toolbox, pascal)
	move.l	4(%sp),%d0
	move.l	8(%sp),%d1
	subq.l	#4,%sp
	move.l	%d0,-(%sp)
	move.w	%d1,-(%sp)
	.short	0xA9A0
	move.l	(%sp)+,%d0
	move.l	%d0,%a0
	rts

	.globl	mac_releaseresource
mac_releaseresource:			| void (void **handle)
	move.l	4(%sp),-(%sp)
	.short	0xA9A3
	rts

/* ---- completions -------------------------------------------------------------------------- */
	.globl	mac_complete
mac_complete:				| void (void *pb, int result): ioResult, then ioCompletion (A0 = pb, D0)
	movem.l	%d2-%d7/%a2-%a6,-(%sp)
	move.l	44+4(%sp),%a0
	move.l	44+8(%sp),%d0
	move.w	%d0,16(%a0)
	move.l	12(%a0),%d1
	beq.s	1f
	move.l	%d1,%a1
	ext.l	%d0
	jsr	(%a1)
1:	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts

	.globl	mac_call_pascal1
mac_call_pascal1:			| void (void *proc, u32 arg, void *a0): pascal proc(arg), A0 set too
	movem.l	%d2-%d7/%a2-%a6,-(%sp)
	move.l	44+4(%sp),%a1
	move.l	44+8(%sp),%d1
	move.l	44+12(%sp),%a0
	move.l	%sp,%a4			| whatever the routine does with its argument, the stack comes back
	move.l	%d1,-(%sp)
	jsr	(%a1)
	move.l	%a4,%sp
	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts

	.globl	mac_call_asr
mac_call_asr:				| void (proc, stream, event, udata, reason, icmp, tcp): a MacTCP notification
	movem.l	%d2-%d7/%a2-%a6,-(%sp)	| routine, pascal: TCP (stream, event, userData, terminReason, icmpMsg),
	move.l	%sp,%a4			| UDP (stream, event, userData, icmpMsg); the stack comes back whatever it does
	lea	44+4(%sp),%a3		| the C arguments
	move.l	(%a3),%a1		| proc
	move.l	4(%a3),-(%sp)		| stream
	move.w	8+2(%a3),-(%sp)		| eventCode
	move.l	12(%a3),-(%sp)		| userDataPtr
	tst.l	24(%a3)
	beq.s	1f
	move.w	16+2(%a3),-(%sp)	| terminReason (TCP only)
1:	move.l	20(%a3),-(%sp)		| icmpMsg
	jsr	(%a1)
	move.l	%a4,%sp
	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts

/* ---- the stack's context: a deferred task on a stack of its own --------------------------- */
	.globl	mac_dt_entry
mac_dt_entry:				| A1 = dtParam
	movem.l	%d0-%d7/%a0-%a6,-(%sp)
	move.l	%sp,%d7
	move.l	mac_stack_top,%d0
	beq.s	1f
	move.l	%d0,%sp
1:	jsr	mac_stack_run
	move.l	%d7,%sp
	movem.l	(%sp)+,%d0-%d7/%a0-%a6
	rts

	.globl	mac_tm_entry
mac_tm_entry:				| A1 = the TMTask: ask for the deferred task
	movem.l	%d0-%d2/%a0-%a1,-(%sp)
	jsr	mac_schedule
	movem.l	(%sp)+,%d0-%d2/%a0-%a1
	rts

/* ---- the .ENET protocol handler (Inside Macintosh: Networking, "Ethernet Protocol Handlers") --
   On entry: A3 -> the first byte after the 14-byte header (which is at -14(A3)), D1 = bytes left
   to read, A4 -> ReadPacket, 2(A4) = ReadRest; A0 and A1 belong to the driver until ReadRest.
   The frame is copied into the next slot of the receive ring (C: net_mac.c), which the stack's
   deferred task drains; a full ring drops the frame (ReadRest with an empty buffer). */
	.equ	RX_SLOT, 1536
	.equ	RX_DATA, 1500
	.globl	mac_enet_ph
mac_enet_ph:
	movem.l	%d4-%d7/%a2/%a5/%a6,-(%sp)
	move.l	mac_rx_ring,%d4
	beq.s	drop
	move.w	mac_rx_head,%d4
	move.w	%d4,%d5
	sub.w	mac_rx_tail,%d5
	cmp.w	mac_rx_slots,%d5
	bcc.s	drop
	and.w	mac_rx_mask,%d4
	mulu.w	#RX_SLOT,%d4
	move.l	mac_rx_ring,%a2
	add.l	%d4,%a2			| slot: length word, the 14-byte header, the data
	lea	-14(%a3),%a5
	lea	2(%a2),%a6
	moveq	#13,%d5
1:	move.b	(%a5)+,(%a6)+
	dbf	%d5,1b
	move.w	%d1,%d6
	cmp.w	#RX_DATA,%d6
	bls.s	2f
	move.w	#RX_DATA,%d6
2:	moveq	#0,%d3
	move.w	%d6,%d3
	lea	16(%a2),%a3
	jsr	2(%a4)			| ReadRest: A3 = buffer, D3 = its size
	add.w	#14,%d6
	move.w	%d6,(%a2)
	addq.w	#1,mac_rx_head
	jsr	mac_schedule		| A0/A1 are free after ReadRest
	movem.l	(%sp)+,%d4-%d7/%a2/%a5/%a6
	rts
drop:
	addq.l	#1,mac_rx_dropped
	moveq	#0,%d3
	jsr	2(%a4)
	movem.l	(%sp)+,%d4-%d7/%a2/%a5/%a6
	rts

	.globl	mac_tx_done
mac_tx_done:				| an EWrite's ioCompletion (A0 = its parameter block): the slot is free
	movem.l	%d0-%d1/%a0-%a1,-(%sp)
	move.l	%a0,-(%sp)
	jsr	net_mac_tx_done
	addq.l	#4,%sp
	movem.l	(%sp)+,%d0-%d1/%a0-%a1
	rts
