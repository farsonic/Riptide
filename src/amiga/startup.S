| Riptide - program entry. Must be the first code in the image (offset 0):
| AmigaOS starts a CLI command at the beginning of its first hunk with
| a0 = argument string, d0 = its length, and 4.w = SysBase.
| SPDX-License-Identifier: GPL-2.0-only
	.section .text.amiga_entry,"ax"
	.globl	_start
_start:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)
	move.l	%a0,%a2
	move.l	%d0,%d2
	| clear bss (LoadSeg zero-fills the extra hunk space on Kickstart 2.0+,
	| but do not rely on it)
	lea	__bss_start,%a0
	lea	__bss_end,%a1
1:	cmp.l	%a1,%a0
	bcc.s	2f
	clr.l	(%a0)+
	bra.s	1b
2:	move.l	4.w,%a6
	move.l	%a6,SysBase
	move.l	%d2,-(%sp)
	move.l	%a2,-(%sp)
	jsr	amiga_main
	addq.l	#8,%sp
	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts
	.section .note.GNU-stack,"",@progbits
