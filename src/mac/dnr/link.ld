/* src/mac/dnr/link.ld - the 'dnrp' code resource: linked at 0, relocated by itself at the first
 * call (dnrp_entry.S, tools/mkdnr.py); the entry comes first, the BSS is part of the image.
 * SPDX-License-Identifier: GPL-2.0-only */
OUTPUT_FORMAT("elf32-m68k")
ENTRY(_start)
SECTIONS
{
	. = 0;
	.text : {
		KEEP(*dnrp_entry.o(.text))
		*(.text .text.*)
		*(.rodata .rodata.*)
	}
	.data ALIGN(4) : { *(.data .data.*) }
	.bss ALIGN(4) : { *(.bss .bss.*) *(COMMON) }
	_image_end = .;
	/DISCARD/ : { *(.comment) *(.note .note.*) *(.eh_frame .eh_frame_hdr) *(.gnu.attributes) }
}
