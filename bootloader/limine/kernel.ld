ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 0xffffffff80000000;

    .text : ALIGN(4K)
    {
        __kernel_text_start = .;
        *(.text .text.*)
        . = ALIGN(4K);
        __kernel_text_end = .;
    } :text

    .rodata : ALIGN(4K)
    {
        __kernel_rodata_start = .;
        *(.rodata .rodata.*)
        *(.eh_frame_hdr .eh_frame)
        . = ALIGN(4K);
        __kernel_rodata_end = .;
    } :rodata

    .data : ALIGN(4K)
    {
        __kernel_data_start = .;
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
        *(.data .data.*)
        *(.got .got.*)
    } :data

    .bss : ALIGN(4K)
    {
        *(COMMON)
        *(.bss .bss.*)
        . = ALIGN(4K);
        __kernel_image_end = .;
    } :data
}
