/* src/mac/link.ld - Riptide on the classic Mac OS: the .IPP driver image, linked at 0 and
 * relocated by itself at its first Open (drvr.S, tools/elf2mac.py). The driver header must come
 * first; the zero-initialised data is part of the image (a 'DRVR' resource has no BSS).
 * SPDX-License-Identifier: GPL-2.0-only
 */
OUTPUT_FORMAT("elf32-m68k")
ENTRY(_drvr_start)
SECTIONS
{
	. = 0;
	.text : {
		KEEP(*(.text.drvr))
		*(.text .text.*)
		*(.rodata .rodata.*)
	}
	.data ALIGN(4) : { *(.data .data.*) *(.sdata .sdata.*) }
	.bss ALIGN(4) : { *(.bss .bss.*) *(.sbss .sbss.*) *(COMMON) }
	_image_end = .;
	/DISCARD/ : { *(.comment) *(.note .note.*) *(.eh_frame .eh_frame_hdr) *(.gnu.attributes) }
}
