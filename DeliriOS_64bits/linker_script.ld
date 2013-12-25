OUTPUT_FORMAT("binary")
ENTRY(kernel)
/*aca hay un tema de magnitud turbia, el codigo arranca en 0x1200 y necesitamos un ap_startup_code_page alineado a 4k, */
/*sale rellenar de instrucciones nops hasta alinear?.*/
ap_startup_code_page = 0x1200;
SECTIONS
{
    . = 0x1200;

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