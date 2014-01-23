ENTRY(loader)
SECTIONS
{

	. = 0x00100000;
	
	.__mbHeader : {
		start_mb_header = .;
		*(.__mbHeader)
        . = ALIGN(4096);
	}

    .text : 
    {
        start_section_text = .;
        *(.text)
        . = ALIGN(4096);
        end_section_text = .;
    }
    
    .apstartsection :
    {
        ap_startup_code_page = .;
        *(.apstartsection)
        . = ALIGN(4096);
        end_ap_startup_code_page = .;
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

    __end =.;

}