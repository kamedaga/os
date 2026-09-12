/* SPDX-License-Identifier: MIT */
/* Machine fixture only: no writable static state may hide outside its stack. */
ENTRY(_start)
SECTIONS {
    . = 0x44000000;
    .text : { *(.text.entry) *(.text .text.*) }
    . = ALIGN(4096);
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) }
    .bss : { *(.bss .bss.*) *(COMMON) }
    ASSERT(SIZEOF(.data) == 0, "IPC child has writable static data")
    ASSERT(SIZEOF(.bss) == 0, "IPC child has unbacked BSS")
    /DISCARD/ : { *(.eh_frame*) *(.comment) *(.note*) }
}
