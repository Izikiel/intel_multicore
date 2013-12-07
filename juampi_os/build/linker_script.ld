ENTRY(loader)
SECTIONS
{
	. = 0x2000;
	ap_startup_code_page = .;

	. = 0x00100000;
	.__mbHeader : {
		*(.__mbHeader)
	}

	.text ALIGN (0x1000) :
	{
		*(.text)

		__TEXT_END__ = .;
	}	

	.rodata ALIGN (0x1000) :
	{
		*(.rodata*)

		__RODATA_END__ = .;
	}
	

	.data ALIGN (0x1000) :
	{	
		kernel_code_end = .;	
		*(.data)

		__DATA_END__ = .;
	}

	.bss :
	{
		sbss = .;
		*(COMMON)
		*(.bss)
		ebss = .;
	}

	kernel_end = .; 
}
