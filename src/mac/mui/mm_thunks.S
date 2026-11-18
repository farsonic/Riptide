| Riptide - the Mac's MUI (see mm.h): the built-in classes' dispatchers as BOOPSI calls them,
| class in a0, object in a2, message in a1 (rp_hook_call in src/amiga/prefs/mui_thunks.S), each
| calling the C function NAME(cl, obj, msg). The same macro as the editor's own classes use.
| SPDX-License-Identifier: GPL-2.0-only
	.text
	.even
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

	DISPATCH mm_notify_dispatch
	DISPATCH mm_app_dispatch
	DISPATCH mm_win_dispatch
	DISPATCH mm_area_dispatch
	DISPATCH mm_group_dispatch
	DISPATCH mm_rect_dispatch
	DISPATCH mm_image_dispatch
	DISPATCH mm_text_dispatch
	DISPATCH mm_string_dispatch
	DISPATCH mm_cycle_dispatch
	DISPATCH mm_list_dispatch
	DISPATCH mm_listview_dispatch
	DISPATCH mm_menustrip_dispatch
	DISPATCH mm_menuitem_dispatch

	.section .note.GNU-stack,"",@progbits
