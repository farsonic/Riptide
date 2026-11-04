; Riptide - hand-tuned 680x0 checksum and copy routines (Motorola syntax,
; vasm: vasmm68k_mot -m68020 -Fhunk for vbcc, -Felf for the GCC builds).
; SPDX-License-Identifier: GPL-2.0-only
;
; C prototypes (include/rt/rt_arch.h):
;   u32  rt_csum_020(const void *data, u32 len, u32 sum)
;   u32  rt_csum_copy_020(void *dst, const void *src, u32 len, u32 sum)
;   void rt_copy_020(void *dst, const void *src, u32 len)
;   void rt_copy_040(void *dst, const void *src, u32 len)    68040/68060 only
;   void rt_copy_020m(void *dst, const void *src, u32 len)   movem.l variant
;
; Calling convention: arguments on the stack, d0/d1/a0/a1 scratch, result
; in d0. vbcc (+aos68k) and m68k-linux-gnu-gcc both use it. Every symbol is
; exported with and without the leading underscore vbcc adds to C names.
;
; The routines are reentrant: they use only registers and the stack, so
; the SANA-II buffer callbacks may call them from interrupts. They use no
; 64-bit mulu.l/divu.l (68060 traps those) and no FPU.
;
; Checksums are 32-bit partial one's complement sums with the semantics of
; rt_csum_partial(): 16-bit big-endian words paired from the start of the
; buffer, an odd trailing byte is the high byte of a word, then 'sum' is
; added with end-around carry. The 32-bit result may differ from the C
; version but is congruent modulo 0xffff (equal after rt_csum_fold()).
; A buffer at an odd address is summed from the next even address and the
; result rotated by 8 bits (RFC 1071 byte-swap property).

	section	.text,code

	xdef	_rt_csum_020
	xdef	rt_csum_020
	xdef	_rt_csum_copy_020
	xdef	rt_csum_copy_020
	xdef	_rt_copy_020
	xdef	rt_copy_020
	xdef	_rt_copy_040
	xdef	rt_copy_040
	xdef	_rt_copy_020m
	xdef	rt_copy_020m
	xdef	_rt_cpu_vbl
	xdef	rt_cpu_vbl

; ---------------------------------------------------------------------------
; u32 rt_csum_020(const void *data, u32 len, u32 sum)
; 16 x (move.l / addx.l) per iteration, entered by a computed jump for the
; remainder (Duff's device); dbra/swap keep the X carry chain intact.
	cnop	0,4
_rt_csum_020:
rt_csum_020:
	movem.l	d2-d4,-(sp)		; args: data 16(sp), len 20(sp), sum 24(sp)
	move.l	16(sp),a0
	move.l	20(sp),d1
	moveq	#0,d0			; accumulator
	moveq	#0,d4			; 1 = started on an odd address
	tst.l	d1
	beq	.done
	move.w	a0,d2
	btst	#0,d2
	beq.s	.even
	move.b	(a0)+,d0		; low byte of the shifted pairing
	subq.l	#1,d1
	moveq	#1,d4
.even:
	cmp.l	#2,d1
	blo	.tail
	move.w	a0,d2
	btst	#1,d2
	beq.s	.al4
	moveq	#0,d2
	move.w	(a0)+,d2
	add.l	d2,d0			; cannot carry
	subq.l	#2,d1
.al4:
	move.l	d1,d2
	lsr.l	#2,d2			; longs
	beq	.tail
	moveq	#15,d3
	and.l	d2,d3			; longs of the partial block
	lsr.l	#4,d2			; further full blocks (dbra count)
	sub.w	#16,d3
	neg.w	d3
	add.w	d3,d3
	add.w	d3,d3			; (16 - partial) * 4 bytes of code to skip
	move	#0,ccr			; X = 0
	jmp	.blk(pc,d3.w)
.blk:
	rept	16
	move.l	(a0)+,d3
	addx.l	d3,d0
	endr
	dbra	d2,.blk
	swap	d2
	dbra	d2,.more
	bra.s	.lend
.more:
	swap	d2
	bra.s	.blk
.lend:
	moveq	#0,d3
	addx.l	d3,d0			; last carry
.tail:
	btst	#1,d1
	beq.s	.nw
	moveq	#0,d2
	move.w	(a0)+,d2
	add.l	d2,d0
	moveq	#0,d2
	addx.l	d2,d0
.nw:
	btst	#0,d1
	beq.s	.fold
	moveq	#0,d2
	move.b	(a0),d2
	lsl.w	#8,d2			; odd trailing byte: high byte of a word
	add.l	d2,d0
	moveq	#0,d2
	addx.l	d2,d0
.fold:
	tst.w	d4
	beq.s	.done
	ror.l	#8,d0			; undo the one byte shift of the pairing
.done:
	add.l	24(sp),d0
	moveq	#0,d2
	addx.l	d2,d0
	movem.l	(sp)+,d2-d4
	rts

; ---------------------------------------------------------------------------
; u32 rt_csum_copy_020(void *dst, const void *src, u32 len, u32 sum)
; Copies len bytes and returns rt_csum_020(src, len, sum) in one pass. The
; source is aligned for the long reads; the destination may be misaligned
; (allowed on the 68020+, aligned in the common rt_buf to rt_buf case).
;
; The bulk moves 56 bytes per pass: movem.l loads seven longwords at a
; time into d1-d7, and each is stored and folded into the sum by one
; addx.l, the carry riding in X from one to the next. Two instructions and
; a seventh of a movem per longword instead of three (load, store, addx):
; a 68020 pays mostly for the instructions it fetches. Nothing between two
; addx may change X: movem, move, lea, cmpa and the branches leave it be,
; so the loops end on cmpa against precomputed end pointers.
	cnop	0,4
_rt_csum_copy_020:
rt_csum_copy_020:
	movem.l	d2-d7/a2-a3,-(sp)	; dst 36(sp), src 40(sp), len 44(sp), sum 48(sp)
	move.l	36(sp),a1
	move.l	40(sp),a0
	move.l	44(sp),d1
	moveq	#0,d0
	moveq	#0,d4
	tst.l	d1
	beq	.done
	move.w	a0,d2
	btst	#0,d2
	beq.s	.even
	move.b	(a0)+,d0
	move.b	d0,(a1)+
	subq.l	#1,d1
	moveq	#1,d4
.even:
	cmp.l	#2,d1
	blo	.tail
	move.w	a0,d2
	btst	#1,d2
	beq.s	.al4
	moveq	#0,d2
	move.w	(a0)+,d2
	move.w	d2,(a1)+
	add.l	d2,d0
	subq.l	#2,d1
.al4:
	move.l	d1,d2
	lsr.l	#2,d2
	beq	.tail
	move.l	d1,-(sp)		; the length (its low bits: the tail) and
	move.l	d4,-(sp)		; the odd start, while d1-d7 carry data
	lsl.l	#2,d2			; bytes in whole longwords
	lea	(a0,d2.l),a3		; end of the longwords
	moveq	#56,d3
	sub.l	d3,d2
	lea	(a0,d2.l),a2		; the last place a 56 byte pass may start
	move	#0,ccr			; X = 0
	bra.s	.bchk
.blk:
	rept	2
	movem.l	(a0)+,d1-d7
	move.l	d1,(a1)+		; move leaves X alone
	addx.l	d1,d0
	move.l	d2,(a1)+
	addx.l	d2,d0
	move.l	d3,(a1)+
	addx.l	d3,d0
	move.l	d4,(a1)+
	addx.l	d4,d0
	move.l	d5,(a1)+
	addx.l	d5,d0
	move.l	d6,(a1)+
	addx.l	d6,d0
	move.l	d7,(a1)+
	addx.l	d7,d0
	endr
.bchk:
	cmpa.l	a2,a0
	bls.s	.blk			; a0 <= a2: another 56 bytes
	bra.s	.lchk
.lng:
	move.l	(a0)+,d1		; the last 0-13 longwords
	move.l	d1,(a1)+
	addx.l	d1,d0
.lchk:
	cmpa.l	a3,a0
	blo.s	.lng
	moveq	#0,d1
	addx.l	d1,d0			; the last carry; all ones + X carries
	addx.l	d1,d0			; once more, and then no more
	move.l	(sp)+,d4
	move.l	(sp)+,d1
.tail:
	btst	#1,d1
	beq.s	.nw
	moveq	#0,d2
	move.w	(a0)+,d2
	move.w	d2,(a1)+
	add.l	d2,d0
	moveq	#0,d2
	addx.l	d2,d0
.nw:
	btst	#0,d1
	beq.s	.fold
	moveq	#0,d2
	move.b	(a0),d2
	move.b	d2,(a1)
	lsl.w	#8,d2
	add.l	d2,d0
	moveq	#0,d2
	addx.l	d2,d0
.fold:
	tst.w	d4
	beq.s	.done
	ror.l	#8,d0
.done:
	add.l	48(sp),d0
	moveq	#0,d2
	addx.l	d2,d0
	movem.l	(sp)+,d2-d7/a2-a3
	rts

; ---------------------------------------------------------------------------
; void rt_copy_020(void *dst, const void *src, u32 len)
; Aligns the destination to 4 bytes, then 16 x move.l (a0)+,(a1)+ per
; iteration (computed jump for the remainder). The 68020+ reads misaligned
; longs from the source at a small cost.
	cnop	0,4
_rt_copy_020:
rt_copy_020:
	move.l	d2,-(sp)		; dst 8(sp), src 12(sp), len 16(sp)
	move.l	8(sp),a1
	move.l	12(sp),a0
	move.l	16(sp),d0
	cmp.l	#16,d0
	blo.s	.small
	move.w	a1,d1
	btst	#0,d1
	beq.s	.d2
	move.b	(a0)+,(a1)+
	subq.l	#1,d0
.d2:
	move.w	a1,d1
	btst	#1,d1
	beq.s	.d4
	move.w	(a0)+,(a1)+
	subq.l	#2,d0
.d4:
	move.l	d0,d1
	lsr.l	#2,d1			; longs (>= 3)
	moveq	#15,d2
	and.l	d1,d2
	lsr.l	#4,d1
	sub.w	#16,d2
	neg.w	d2
	add.w	d2,d2			; (16 - partial) * 2 bytes of code
	jmp	.blk(pc,d2.w)
.blk:
	rept	16
	move.l	(a0)+,(a1)+
	endr
	dbra	d1,.blk
	swap	d1
	dbra	d1,.more
	bra.s	.tail
.more:
	swap	d1
	bra.s	.blk
.tail:
	btst	#1,d0
	beq.s	.nw
	move.w	(a0)+,(a1)+
.nw:
	btst	#0,d0
	beq.s	.ret
	move.b	(a0),(a1)
.ret:
	move.l	(sp)+,d2
	rts
.small:
	subq.l	#1,d0
	bcs.s	.ret
.sl:
	move.b	(a0)+,(a1)+
	dbra	d0,.sl
	bra.s	.ret

; ---------------------------------------------------------------------------
; void rt_copy_040(void *dst, const void *src, u32 len)        68040/68060
; move16 for the 16-byte lines when src and dst have the same alignment
; modulo 16 and len >= 80; everything else goes to rt_copy_020. move16
; bypasses the caches' allocation logic and bursts whole lines: use it only
; on Fast RAM that supports bursts (rt_arch.copy_buf: Riptide's own packet
; buffers; kill switch: sysctl riptide.cpu.move16=0).
	cnop	0,4
_rt_copy_040:
rt_copy_040:
	move.l	12(sp),d0		; dst 4(sp), src 8(sp), len 12(sp)
	cmp.l	#80,d0
	blo	rt_copy_020
	move.l	8(sp),d1
	sub.l	4(sp),d1
	and.w	#15,d1
	bne	rt_copy_020		; different line offsets: no move16
	move.l	4(sp),a1
	move.l	8(sp),a0
	move.w	a1,d1			; align dst (and so src) to 16
	btst	#0,d1
	beq.s	.a2
	move.b	(a0)+,(a1)+
	subq.l	#1,d0
.a2:
	move.w	a1,d1
	btst	#1,d1
	beq.s	.a4
	move.w	(a0)+,(a1)+
	subq.l	#2,d0
.a4:
	move.w	a1,d1
	btst	#2,d1
	beq.s	.a8
	move.l	(a0)+,(a1)+
	subq.l	#4,d0
.a8:
	move.w	a1,d1
	btst	#3,d1
	beq.s	.a16
	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+
	subq.l	#8,d0
.a16:					; d0 >= 65
	move.l	d0,d1
	lsr.l	#6,d1			; 64-byte blocks (>= 1)
	machine	68040
.l64:
	move16	(a0)+,(a1)+
	move16	(a0)+,(a1)+
	move16	(a0)+,(a1)+
	move16	(a0)+,(a1)+
	subq.l	#1,d1
	bne.s	.l64
	btst	#5,d0
	beq.s	.n32
	move16	(a0)+,(a1)+
	move16	(a0)+,(a1)+
.n32:
	btst	#4,d0
	beq.s	.n16
	move16	(a0)+,(a1)+
.n16:
	machine	68020
	btst	#3,d0
	beq.s	.n8
	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+
.n8:
	btst	#2,d0
	beq.s	.n4
	move.l	(a0)+,(a1)+
.n4:
	btst	#1,d0
	beq.s	.n2
	move.w	(a0)+,(a1)+
.n2:
	btst	#0,d0
	beq.s	.n1
	move.b	(a0),(a1)
.n1:
	rts

; ---------------------------------------------------------------------------
; void rt_copy_020m(void *dst, const void *src, u32 len)
; As rt_copy_020, but the aligned middle moves 32 bytes per iteration with
; two movem.l over eight registers: a third of the instructions (and of the
; instruction fetches, which a 68020's 256-byte cache does not hold) of
; eight move.l (a0)+,(a1)+. Copies under 96 bytes go to rt_copy_020.
	cnop	0,4
_rt_copy_020m:
rt_copy_020m:
	move.l	12(sp),d0		; dst 4(sp), src 8(sp), len 12(sp)
	cmp.l	#96,d0
	blo	rt_copy_020
	movem.l	d2-d7/a2-a3,-(sp)	; 8 registers: dst 36(sp), src 40(sp), len 44(sp)
	move.l	36(sp),a1
	move.l	40(sp),a0
	move.l	44(sp),d0
	move.w	a1,d1			; align the destination to 4
	btst	#0,d1
	beq.s	.d2
	move.b	(a0)+,(a1)+
	subq.l	#1,d0
.d2:
	move.w	a1,d1
	btst	#1,d1
	beq.s	.d4
	move.w	(a0)+,(a1)+
	subq.l	#2,d0
.d4:
	move.l	d0,d1
	lsr.l	#5,d1			; 32-byte blocks (>= 2)
.blk:
	movem.l	(a0)+,d2-d7/a2-a3
	movem.l	d2-d7/a2-a3,(a1)
	lea	32(a1),a1
	subq.l	#1,d1
	bne.s	.blk
	btst	#4,d0			; the rest: 16, 8, 4, 2, 1 bytes
	beq.s	.n16
	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+
.n16:
	btst	#3,d0
	beq.s	.n8
	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+
.n8:
	btst	#2,d0
	beq.s	.n4
	move.l	(a0)+,(a1)+
.n4:
	btst	#1,d0
	beq.s	.n2
	move.w	(a0)+,(a1)+
.n2:
	btst	#0,d0
	beq.s	.n1
	move.b	(a0),(a1)
.n1:
	movem.l	(sp)+,d2-d7/a2-a3
	rts

; rt_cpu_vbl: an AmigaOS vertical blank interrupt server (exec calls it
; with a1 = is_Data) for Riptide's CPU meter. is_Data points to
; struct rt_cpu_samples { struct Task *task; u32 ticks; u32 waiting; }:
; each call counts a tick, and a waiting one when the task's tc_State
; (offset 15) is TS_WAIT (4). Returns with Z set so the chain goes on.
_rt_cpu_vbl:
rt_cpu_vbl:
	move.l	(a1),a0
	addq.l	#1,4(a1)
	cmp.b	#4,15(a0)
	bne.s	.busy
	addq.l	#1,8(a1)
.busy:
	moveq	#0,d0
	rts
