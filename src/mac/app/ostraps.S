| src/mac/app/ostraps.S - register-based Mac OS traps for Riptide's applications (the Memory,
| File and Device Managers, Gestalt), C-callable: arguments on the stack, the result in D0 (and
| A0 for pointers). Each keeps D2 for GCC. Parameter blocks are passed whole (Inside Macintosh:
| Files, Devices).
| SPDX-License-Identifier: GPL-2.0-only
	.text

| void *mac_newptr(long size): NewPtrClear, NULL when there is no room
	.globl	mac_newptr
mac_newptr:
	move.l	4(%sp),%d0
	.short	0xA31E
	move.l	%a0,%d0
	rts
| void mac_disposeptr(void *p)
	.globl	mac_disposeptr
mac_disposeptr:
	move.l	4(%sp),%a0
	.short	0xA01F
	rts
| void **mac_newhandle(long size): NewHandleClear
	.globl	mac_newhandle
mac_newhandle:
	move.l	4(%sp),%d0
	.short	0xA322
	move.l	%a0,%d0
	rts
| void mac_disposehandle(void **h)
	.globl	mac_disposehandle
mac_disposehandle:
	move.l	4(%sp),%a0
	.short	0xA023
	rts
| long mac_gethandlesize(void **h)
	.globl	mac_gethandlesize
mac_gethandlesize:
	move.l	4(%sp),%a0
	.short	0xA025
	tst.l	%d0
	bpl.s	1f
	moveq	#0,%d0
1:	rts
| void mac_hlock(void **h) / mac_hunlock
	.globl	mac_hlock
mac_hlock:
	move.l	4(%sp),%a0
	.short	0xA029
	rts
	.globl	mac_hunlock
mac_hunlock:
	move.l	4(%sp),%a0
	.short	0xA02A
	rts

| short mac_pb(void *pb, int trap): a parameter-block call, synchronous: _Open A000, _Close A001,
| _Read A002, _Write A003, _Control A004, _Status A005, _GetEOF A011, _SetEOF A012, _SetFPos A044,
| the HFS forms with bit 9 (_HOpen A200, _HCreate A208, _HDelete A209), _HFSDispatch A260 with
| the selector in the trap's high word (mac_pb(pb, 0x0009A260): GetCatInfo). Returns ioResult.
	.globl	mac_pb
mac_pb:
	move.l	4(%sp),%a0
	move.l	8(%sp),%d0
	move.w	%d0,1f			| the trap word, written into the code below
	swap	%d0			| _HFSDispatch's selector in D0
	and.l	#0xFFFF,%d0
	cmp.w	#0x0605,0x15A		| the cache holds the old word: flush it (System 6.0.5 on)
	bcs.s	2f
	movem.l	%d0/%a0,-(%sp)
	moveq	#1,%d0
	.short	0xA198
	movem.l	(%sp)+,%d0/%a0
2:	nop
1:	.short	0xA000
	ext.l	%d0
	rts

| long mac_gestalt(long selector, long *response): Gestalt, the OSErr
	.globl	mac_gestalt
mac_gestalt:
	move.l	4(%sp),%d0
	.short	0xA1AD
	move.l	8(%sp),%a1
	move.l	%a0,(%a1)
	ext.l	%d0
	rts

| short mac_findfolder(int vref, long type, int create, short *foundVRef, long *foundDir):
| FindFolder (_AliasDispatch, selector 0). Arguments at 8(a6), 12, 16, 20, 24.
	.globl	mac_findfolder
mac_findfolder:
	link	%a6,#0
	move.l	%d2,-(%sp)
	clr.w	-(%sp)
	move.w	10(%a6),-(%sp)		| vRefNum
	move.l	12(%a6),-(%sp)		| folderType
	move.b	19(%a6),-(%sp)		| createFolder
	move.l	20(%a6),-(%sp)		| VAR foundVRefNum
	move.l	24(%a6),-(%sp)		| VAR foundDirID
	moveq	#0,%d0
	.short	0xA823
	move.w	(%sp)+,%d0
	ext.l	%d0
	move.l	-4(%a6),%d2
	unlk	%a6
	rts

| Standard File (System 7, Pack3): void mac_stdputfile(const unsigned char *prompt,
| const unsigned char *name, void *reply) and void mac_stdgetfile(long ntypes, const long *types,
| void *reply), reply a StandardFileReply
	.globl	mac_stdputfile
mac_stdputfile:
	link	%a6,#0
	move.l	%d2,-(%sp)
	move.l	8(%a6),-(%sp)		| prompt
	move.l	12(%a6),-(%sp)		| defaultName
	move.l	16(%a6),-(%sp)		| VAR reply
	move.w	#5,-(%sp)
	.short	0xA9EA
	move.l	-4(%a6),%d2
	unlk	%a6
	rts
	.globl	mac_stdgetfile
mac_stdgetfile:
	link	%a6,#0
	move.l	%d2,-(%sp)
	clr.l	-(%sp)			| no file filter
	move.w	10(%a6),-(%sp)		| numTypes
	move.l	12(%a6),-(%sp)		| typeList
	move.l	16(%a6),-(%sp)		| VAR reply
	move.w	#6,-(%sp)
	.short	0xA9EA
	move.l	-4(%a6),%d2
	unlk	%a6
	rts

| unsigned long long mac_us(void): Microseconds (System 7), the high long in A0, the low in D0
	.globl	mac_us
mac_us:
	.short	0xA193
	move.l	%d0,%d1
	move.l	%a0,%d0
	rts

| Pascal callbacks into C: a routine the Toolbox calls with Pascal arguments, which it pops itself.
| pascal void (ControlHandle ctl, short part) -> void c_ctl_action(void *ctl, int part)
| pascal void (DialogPtr dlg, short item) -> void c_useritem(void *dlg, int item): a user item's
| drawing procedure (mac_alert.c: the default button's outline)
	.globl	pas_useritem
pas_useritem:
	move.w	4(%sp),%d0
	ext.l	%d0
	move.l	6(%sp),%a0
	move.l	%d0,-(%sp)
	move.l	%a0,-(%sp)
	bsr.l	c_useritem
	addq.l	#8,%sp
	move.l	(%sp)+,%a0
	addq.l	#6,%sp
	jmp	(%a0)

| pascal Boolean (DialogPtr dlg, EventRecord *ev, short *item) -> int c_dlgfilter(dlg, ev, item):
| ModalDialog's filter (mac_alert.c: Return, Enter, Escape, Command-period, the windows behind)
	.globl	pas_dlgfilter
pas_dlgfilter:
	move.l	4(%sp),-(%sp)
	move.l	12(%sp),-(%sp)
	move.l	20(%sp),-(%sp)
	bsr.l	c_dlgfilter
	lea	12(%sp),%sp
	move.l	(%sp)+,%a0
	lea	12(%sp),%sp
	tst.l	%d0
	sne	%d0
	and.b	#1,%d0
	move.b	%d0,(%sp)		| the BOOLEAN: the high byte of the result word
	clr.b	1(%sp)
	jmp	(%a0)

	.globl	pas_ctl_action
pas_ctl_action:
	move.w	4(%sp),%d0
	ext.l	%d0
	move.l	6(%sp),%a0
	move.l	%d0,-(%sp)
	move.l	%a0,-(%sp)
	bsr.l	c_ctl_action
	addq.l	#8,%sp
	move.l	(%sp)+,%a0
	addq.l	#6,%sp
	jmp	(%a0)
	.section .note.GNU-stack,"",@progbits
