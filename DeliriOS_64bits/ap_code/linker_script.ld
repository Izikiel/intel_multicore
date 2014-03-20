OUTPUT_FORMAT("binary")
ENTRY(go64)
SECTIONS
{
    . = 0x12a000; /*kernel_entry_point = 0x117000, ap_start = kernel_entry_point+kernel_size+0x2000-kernel_size %% 0x1000*/

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