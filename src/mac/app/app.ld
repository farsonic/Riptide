/* src/mac/app/app.ld - a Riptide application for the classic Mac OS: linked at 0, relocated by
 * itself at the start (crt0.S, tools/elf2app.py). crt0.o comes first: CODE 1's entry point is
 * the first byte. The zero-initialised data is part of the image.
 * SPDX-License-Identifier: GPL-2.0-only
 */
OUTPUT_FORMAT("elf32-m68k")
ENTRY(_start)
SECTIONS
{
	. = 0;
	.text : {
		KEEP(*crt0.o(.text))
		*(.text .text.*)
		*(.rodata .rodata.*)
	}
	.data ALIGN(4) : { *(.data .data.*) *(.sdata .sdata.*) }
	.bss ALIGN(4) : { *(.bss .bss.*) *(.sbss .sbss.*) *(COMMON) }
	_image_end = .;
	/DISCARD/ : { *(.comment) *(.note .note.*) *(.eh_frame .eh_frame_hdr) *(.gnu.attributes) }
}
