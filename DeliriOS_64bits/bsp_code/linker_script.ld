OUTPUT_FORMAT("binary")
ENTRY(protected_mode)
ap_startup_code_page = 0x2000;
SECTIONS
{
    . = 0x117000;/*esto es porque grub carga el modulo aca, super hardcode !!*/

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