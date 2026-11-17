/* src/mac/init.S - Riptide on the classic Mac OS: the startup INIT. It loads the stack's 'DRVR'
 * 128 from the Riptide file into the system heap, installs it in a free unit table entry as
 * ".IPP" and opens it, which starts the stack (main_mac.c). If a .IPP driver is already in the
 * unit table (MacTCP's, or a second copy of this file), it does nothing: two stacks cannot share
 * the Ethernet protocol handlers.
 * Position independent, no relocations: assembled and copied as is into 'INIT' 128.
 * SPDX-License-Identifier: GPL-2.0-only
 */
	.equ	UTableBase,	0x11C
	.equ	UnitNtryCnt,	0x1D2
	.equ	_GetResource,	0xA9A0
	.equ	_DetachResource, 0xA992
	.equ	_HLock,		0xA029
	.equ	_DrvrInstall,	0xA03D
	.equ	_Open,		0xA000

#ifdef AURORA68_LOG
	/* development on Aurora68: milestones to the kernel log through the slot-9 mailbox */
	.macro	log text
	movem.l	%d0-%d1/%a0-%a1,-(%sp)
	lea	9f(%pc),%a1
	bsr	a68_log
	bra.s	8f
9:	.ascii	"\text"
	.byte	0
	.even
8:	movem.l	(%sp)+,%d0-%d1/%a0-%a1
	.endm
#else
	.macro	log text
	.endm
#endif

	.text
	.globl	init_start
init_start:
	movem.l	%d1-%d7/%a0-%a5,-(%sp)
	link	%a6,#-64
	log	"Riptide INIT: start"
	bsr	find_ipp
	beq	0f
	log	"Riptide INIT: a .IPP driver is already installed (MacTCP?): Riptide stays off"
	bra	done
0:
	subq.l	#4,%sp
	move.l	#0x44525652,-(%sp)	| 'DRVR'
	move.w	#128,-(%sp)
	.short	_GetResource
	move.l	(%sp)+,%d7
	bne	0f
	log	"Riptide INIT: DRVR 128 did not load (system heap full?)"
	bra	done
0:	move.l	%d7,%a0
	.short	_HLock
	move.l	%d7,-(%sp)
	.short	_DetachResource
	/* a free unit from 48 up (the ROM, AppleTalk and the slots use the low ones) */
	move.l	UTableBase,%a1
	moveq	#48,%d6
1:	cmp.w	UnitNtryCnt,%d6
	bge	done
	tst.l	(%a1,%d6.w*4)
	beq.s	2f
	addq.w	#1,%d6
	bra.s	1b
2:	move.w	%d6,%d0
	addq.w	#1,%d0
	neg.w	%d0			| refNum = -(unit + 1)
	move.l	%d7,%a0
	move.l	(%a0),%a0		| the driver itself (locked, detached)
	.short	_DrvrInstall
	tst.w	%d0
	beq	0f
	log	"Riptide INIT: DrvrInstall failed"
	bra	done
0:	/* _DrvrInstall only makes the device control entry: the driver and its header's settings go
	   in by hand (dCtlDriver, dCtlFlags without dRAMBased: a pointer, dCtlDelay/EMask/Menu) */
	move.l	UTableBase,%a1
	move.l	(%a1,%d6.w*4),%a1	| DCE handle
	move.l	(%a1),%a1		| DCE
	move.l	%d7,%a0
	move.l	(%a0),%a0		| driver
	move.l	%a0,(%a1)		| dCtlDriver
	move.w	(%a0),%d0
	and.w	#0xFF00,%d0
	move.w	%d0,4(%a1)		| dCtlFlags: the header's, closed, pointer-based
	move.w	2(%a0),34(%a1)		| dCtlDelay
	move.w	4(%a0),36(%a1)		| dCtlEMask
	move.w	6(%a0),38(%a1)		| dCtlMenu
	log	"Riptide INIT: driver installed, opening .IPP"
	lea	-64(%a6),%a0
	moveq	#15,%d0
3:	clr.l	(%a0)+
	dbf	%d0,3b
	lea	-64(%a6),%a0
	lea	ipp_name(%pc),%a1
	move.l	%a1,18(%a0)		| ioNamePtr
	.short	_Open			| starts the stack
	tst.w	%d0
	beq	0f
#ifdef AURORA68_LOG
	lea	errmsg_num(%pc),%a0	| "Riptide INIT: Open .IPP failed: XXXX" with D0 in hex
	moveq	#3,%d2
4:	move.w	%d0,%d1
	and.w	#15,%d1
	move.b	hexdig(%pc,%d1.w),(%a0,%d2.w)
	lsr.w	#4,%d0
	dbf	%d2,4b
	lea	errmsg(%pc),%a1
	bsr	a68_log
#endif
	bra	done
0:	log	"Riptide INIT: .IPP open"
done:
	unlk	%a6
	movem.l	(%sp)+,%d1-%d7/%a0-%a5
	rts

/* Z clear (bne) when a driver named .IPP is in the unit table */
find_ipp:
	movem.l	%d0-%d2/%a0-%a2,-(%sp)
	move.l	UTableBase,%a1
	moveq	#0,%d1
1:	cmp.w	UnitNtryCnt,%d1
	bge.s	8f
	move.l	(%a1,%d1.w*4),%d0	| DCE handle
	beq.s	7f
	move.l	%d0,%a0
	move.l	(%a0),%d0
	beq.s	7f
	move.l	%d0,%a0			| DCE
	move.l	(%a0),%d0		| dCtlDriver
	beq.s	7f
	btst	#6,5(%a0)		| dRAMBased: a handle
	beq.s	2f
	move.l	%d0,%a2
	move.l	(%a2),%d0
	beq.s	7f
2:	move.l	%d0,%a2
	lea	18(%a2),%a2		| drvrName
	lea	ipp_name(%pc),%a0
	moveq	#4,%d2			| length byte + ".IPP"
3:	cmpm.b	(%a0)+,(%a2)+
	bne.s	7f
	dbf	%d2,3b
	moveq	#1,%d0			| found: Z clear
	movem.l	(%sp)+,%d0-%d2/%a0-%a2
	moveq	#1,%d0
	rts
7:	addq.w	#1,%d1
	bra.s	1b
8:	movem.l	(%sp)+,%d0-%d2/%a0-%a2
	moveq	#0,%d0			| not found: Z set
	rts

#ifdef AURORA68_LOG
a68_log:				| a1 = C string
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
5:	rts
#endif
#ifdef AURORA68_LOG
hexdig:	.ascii	"0123456789ABCDEF"
errmsg:	.ascii	"Riptide INIT: Open .IPP failed: $"
errmsg_num:
	.ascii	"0000"
	.byte	0
	.even
#endif
ipp_name:
	.byte	4
	.ascii	".IPP"
	.even
