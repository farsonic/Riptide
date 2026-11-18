| src/mac/term/term_thunks.S - the Riptide Terminal's functions the driver calls back while a
| socket call waits (src/mac/api_mac.c): yield and break_pending. The driver's code uses A5 as an
| ordinary register, and the Toolbox (WaitNextEvent, QuickDraw) needs the program's A5 world, so
| each thunk puts the program's A5 back for the call and the driver's after it.
| SPDX-License-Identifier: GPL-2.0-only
	.text
	.globl	term_yield_thunk
	.globl	term_break_thunk
	.globl	term_a5
term_yield_thunk:
	move.l	%a5,-(%sp)
	move.l	term_a5,%a5
	move.l	8(%sp),-(%sp)
	jsr	term_yield
	addq.l	#4,%sp
	move.l	(%sp)+,%a5
	rts
term_break_thunk:
	move.l	%a5,-(%sp)
	move.l	term_a5,%a5
	move.l	8(%sp),-(%sp)
	jsr	term_break_pending
	addq.l	#4,%sp
	move.l	(%sp)+,%a5
	rts
	.data
	.align	2
term_a5:
	.long	0
	.section .note.GNU-stack,"",@progbits
