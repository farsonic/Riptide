/* src/mac/term/term.ld - the Riptide Terminal: src/mac/app/app.ld, with the network tools' static
 * data gathered in one place (between __tool_data and __tool_data_end, __tool_bss and
 * __tool_bss_end), which term_main.c puts back as it was before every run of a tool: on the
 * Amiga each run is a fresh program.
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
	.data ALIGN(4) : {
		__tool_data = .;
		*/amiga/cmd/*.o(.data .data.* .sdata .sdata.*)
		*/apps/speedtest/*.o(.data .data.* .sdata .sdata.*)
		. = ALIGN(4);
		__tool_data_end = .;
		*(.data .data.*) *(.sdata .sdata.*)
	}
	.bss ALIGN(4) : {
		__tool_bss = .;
		*/amiga/cmd/*.o(.bss .bss.* .sbss .sbss.* COMMON)
		*/apps/speedtest/*.o(.bss .bss.* .sbss .sbss.* COMMON)
		. = ALIGN(4);
		__tool_bss_end = .;
		*(.bss .bss.*) *(.sbss .sbss.*) *(COMMON)
	}
	_image_end = .;
	/DISCARD/ : { *(.comment) *(.note .note.*) *(.eh_frame .eh_frame_hdr) *(.gnu.attributes) }
}
