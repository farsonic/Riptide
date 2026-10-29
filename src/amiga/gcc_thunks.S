| Riptide - register-argument thunks for the GCC build (vbcc declares
| these entry points in C with __reg()).
| SPDX-License-Identifier: GPL-2.0-only
	.text
	.even

| SANA-II buffer management: BOOL fn(a0 to, a1 from, d0 n)
	.globl	s2_copy_to_buff_thunk
s2_copy_to_buff_thunk:
	move.l	%d0,-(%sp)
	move.l	%a1,-(%sp)
	move.l	%a0,-(%sp)
	jsr	s2_copy_to_buff
	lea	12(%sp),%sp
	rts

	.globl	s2_copy_from_buff_thunk
s2_copy_from_buff_thunk:
	move.l	%d0,-(%sp)
	move.l	%a1,-(%sp)
	move.l	%a0,-(%sp)
	jsr	s2_copy_from_buff
	lea	12(%sp),%sp
	rts

| SANA-II R3 DMA: ULONG fn(a0 cookie), the buffer address or 0 in d0
	.globl	s2_dma_to_buff_thunk
s2_dma_to_buff_thunk:
	move.l	%a0,-(%sp)
	jsr	s2_dma_to_buff
	addq.l	#4,%sp
	rts

	.globl	s2_dma_from_buff_thunk
s2_dma_from_buff_thunk:
	move.l	%a0,-(%sp)
	jsr	s2_dma_from_buff
	addq.l	#4,%sp
	rts

| LONG amiga_call_d0d1(APTR fn, LONG d0, LONG d1): call fn with d0/d1
	.globl	amiga_call_d0d1
amiga_call_d0d1:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)
	move.l	48(%sp),%a0
	move.l	52(%sp),%d0
	move.l	56(%sp),%d1
	jsr	(%a0)
	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts

| ULONG amiga_call_hook(APTR hook, APTR obj, APTR msg): jsr hook->h_Entry
| with a0 = hook, a2 = object, a1 = message
	.globl	amiga_call_hook
amiga_call_hook:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)
	move.l	48(%sp),%a0
	move.l	52(%sp),%a2
	move.l	56(%sp),%a1
	move.l	8(%a0),%a3
	jsr	(%a3)
	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts

	.section .note.GNU-stack,"",@progbits
