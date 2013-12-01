
SECTIONS
	{
		kernel_readonly 0x100000:
			{
				../objects/oz_kernel_486.o (.text)
				* (.text)
				* (.rodata)
			}

		kernel_readwrite BLOCK(0x1000):
			{
				* (.data)
				* (.bss)
				* (COMMON)
			}
	}
