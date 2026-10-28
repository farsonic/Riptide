| Riptide - RiptidePrefs (MUI), GCC build: BOOPSI calls in registers.
| BOOPSI calls a class dispatcher with a0 = class, a2 = object and
| a1 = message; GCC passes arguments on the stack. vbcc needs none of
| this (its dispatchers take __reg() arguments, amiga.lib does the rest).
| SPDX-License-Identifier: GPL-2.0-only
	.text
	.even

| ULONG rp_hook_call(struct Hook *h, Object *obj, Msg msg): jsr h->h_Entry
	.globl	rp_hook_call
rp_hook_call:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)
	move.l	48(%sp),%a0
	move.l	52(%sp),%a2
	move.l	56(%sp),%a1
	move.l	8(%a0),%a3
	jsr	(%a3)
	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts

| NAME_thunk: the entry MUI calls; calls the C function NAME(cl, obj, msg)
	.macro	DISPATCH name
	.globl	\name\()_thunk
\name\()_thunk:
	move.l	%a1,-(%sp)
	move.l	%a2,-(%sp)
	move.l	%a0,-(%sp)
	jsr	\name
	lea	12(%sp),%sp
	rts
	.endm

	DISPATCH nav_dispatch
	DISPATCH head_dispatch
	DISPATCH qbutton_dispatch
	DISPATCH qlabel_dispatch
	DISPATCH status_dispatch
	DISPATCH line_dispatch

	.section .note.GNU-stack,"",@progbits
