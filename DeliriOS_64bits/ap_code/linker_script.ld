OUTPUT_FORMAT("binary")
ENTRY(go64)
SECTIONS
{
    . = 0x126000; /* hardcodeo a lo silvio, me fije donde grub lo pone */

    .text :
    {
        start_section_text = .;
        *(.text)
        . = ALIGN(4096);
        end_section_text = .;
    }

    .data :
    {
        start_section_data = .;
        *(.data)
        . = ALIGN(4096);
        end_section_data = .;
    }

    .rodata :
    {
        start_section_rodata = .;
        *(.rodata)
        . = ALIGN(4096);
        end_section_rodata = .;
    }

    .bss :
    {
        start_section_bss = .;
        *(.bss)
        . = ALIGN(4096);
        end_section_bss = .;
    }

}