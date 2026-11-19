| src/mac/dnr/dnrp_entry.S - Riptide on the classic Mac OS: the start of the 'dnrp' code resource
| (the resolver MacTCP programs reach through their DNR glue, dnr.c). The glue loads 'dnrp' 0 from
| the Riptide DNR file, locks and detaches it, and calls it as a C function: a long selector and
| its arguments on the stack, the result in D0. The first call adds the code's address to every
| absolute field (the table tools/mkdnr.py puts after the image) and flushes the instruction
| cache; every call then goes on to dnrp_main with the stack as the caller left it.
| SPDX-License-Identifier: GPL-2.0-only
	.text
	.globl	_start
	.globl	_reloc_off
_start:
	bra.s	1f
_reloc_off:
	.long	0
relocated:
	.long	0
1:	lea	relocated(%pc),%a0
	tst.l	(%a0)
	bne.s	3f
	movem.l	%d2-%d4/%a2-%a4,-(%sp)
	lea	_start(%pc),%a3
	move.l	_reloc_off(%pc),%d2
	lea	(%a3,%d2.l),%a4
	move.l	(%a4)+,%d3
	move.l	%a3,%d4
	bra.s	4f
2:	move.l	(%a4)+,%d2
	add.l	%d4,(%a3,%d2.l)
4:	subq.l	#1,%d3
	bcc.s	2b
	lea	relocated(%pc),%a0
	move.l	#1,(%a0)
	moveq	#1,%d0
	.short	0xA198			| _HWPriv: FlushInstructionCache
	movem.l	(%sp)+,%d2-%d4/%a2-%a4
3:	lea	dnrp_caller_a5(%pc),%a0	| the calling program's A5: its result procedure gets it back
	move.l	%a5,(%a0)
	bra.l	dnrp_main

	.globl	dnrp_caller_a5
dnrp_caller_a5:
	.long	0

| The completion routine of a lookup's _Control: A0 = the parameter block, interrupt time.
	.globl	dnrp_done
dnrp_done:
	movem.l	%d0-%d7/%a0-%a6,-(%sp)
	move.l	%a0,-(%sp)
	bsr.l	dnrp_complete
	addq.l	#4,%sp
	movem.l	(%sp)+,%d0-%d7/%a0-%a6
	rts

| void dnrp_call_result(void *proc, void *hostinfo, void *udata, void *a5): PROCEDURE ResultProc(hostInfoPtr;
| userDataPtr), with A5 the program's (it runs at deferred time, whichever program is in front)
	.globl	dnrp_call_result
dnrp_call_result:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)
	move.l	%sp,%a4
	move.l	44+4(%sp),%a1
	move.l	44+16(%sp),%d0
	beq.s	1f
	move.l	%d0,%a5
1:	move.l	44+8(%sp),-(%sp)
	move.l	44+16(%sp),-(%sp)
	jsr	(%a1)
	move.l	%a4,%sp
	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts

| short dnrp_pb(void *pb, long trap): _Open A000, _Control A004, _Control async A404
	.globl	dnrp_pb
dnrp_pb:
	move.l	4(%sp),%a0
	move.l	8(%sp),%d0
	cmp.w	#0xA000,%d0
	beq.s	1f
	cmp.w	#0xA004,%d0
	beq.s	2f
	.short	0xA404
	ext.l	%d0
	rts
1:	.short	0xA000
	ext.l	%d0
	rts
2:	.short	0xA004
	ext.l	%d0
	rts
	.section .note.GNU-stack,"",@progbits
