| src/mac/app/crt0.S - the start of a Riptide application for the classic Mac OS (RiptidePrefs).
| The program is linked at 0 (app.ld) and packed by tools/elf2app.py into CODE 1, whose entry
| point is the first byte here. At the start it adds its load address to every 32-bit absolute
| field (the table elf2app.py puts after the image), flushes the instruction cache, makes the
| stack APP_STACK bytes (the MUI editor's layout recursion wants more than the 24 KB default),
| initialises the Toolbox managers and calls main(); main's return quits. The Segment Loader keeps
| CODE 1 locked while the program runs and nothing calls UnloadSeg, so the fixed-up image stays.
| SPDX-License-Identifier: GPL-2.0-only
#ifndef APP_STACK
#define APP_STACK 98304
#endif
	.text
	.globl	_start
	.globl	_reloc_off
_start:
	bra.s	1f
_reloc_off:
	.long	0			| filled in by elf2app.py: the offset of the relocation table
1:	lea	_start(%pc),%a3
	move.l	_reloc_off(%pc),%d2
	lea	(%a3,%d2.l),%a4
	move.l	(%a4)+,%d3
	move.l	%a3,%d4
	bra.s	3f
2:	move.l	(%a4)+,%d2
	add.l	%d4,(%a3,%d2.l)
3:	subq.l	#1,%d3
	bcc.s	2b
	cmp.w	#0x0605,0x15A		| SysVersion: FlushInstructionCache (HWPriv 1) from 6.0.5
	bcs.s	4f
	moveq	#1,%d0
	.short	0xA198			| _HWPriv
4:	move.l	0x908,%a0		| CurStackBase
	sub.l	#APP_STACK,%a0
	cmp.l	0x130,%a0		| ApplLimit: only ever lower it
	bcc.s	5f
	.short	0xA02D			| _SetApplLimit
5:	.short	0xA063			| _MaxApplZone
	moveq	#15,%d7
6:	.short	0xA036			| _MoreMasters
	dbf	%d7,6b
	pea	-4(%a5)
	.short	0xA86E			| _InitGraf
	.short	0xA8FE			| _InitFonts
	move.l	#0x0000FFFF,%d0
	.short	0xA032			| _FlushEvents (all of them)
	.short	0xA912			| _InitWindows
	.short	0xA930			| _InitMenus
	.short	0xA9CC			| _TEInit
	clr.l	-(%sp)
	.short	0xA97B			| _InitDialogs
	.short	0xA850			| _InitCursor
	bsr.l	main
	.short	0xA9F4			| _ExitToShell
	.section .note.GNU-stack,"",@progbits
