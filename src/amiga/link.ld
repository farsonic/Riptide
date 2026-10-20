/*
 * Riptide - link script for the AmigaOS image. Everything goes into one
 * image based at address 0: text, rodata and data first (they become the
 * single HUNK_CODE), bss last (the hunk's allocation covers it and the
 * startup code clears it). tools/elf2hunk.py turns the R_68K_32
 * relocations kept by --emit-relocs into HUNK_RELOC32.
 * SPDX-License-Identifier: GPL-2.0-only
 */
OUTPUT_FORMAT("elf32-m68k")
OUTPUT_ARCH(m68k)
ENTRY(_start)
SECTIONS
{
	. = 0;
	.text : {
		KEEP(*(.text.amiga_entry))
		*(.text .text.*)
	}
	.rodata : ALIGN(4) { *(.rodata .rodata.*) }
	.data : ALIGN(4) { *(.data .data.*) }
	. = ALIGN(4);
	__bss_start = .;
	.bss : { *(.bss .bss.*) *(COMMON) }
	. = ALIGN(4);
	__bss_end = .;
	/DISCARD/ : { *(.comment) *(.note .note.*) *(.eh_frame .eh_frame_hdr) *(.gnu.attributes) }
}
