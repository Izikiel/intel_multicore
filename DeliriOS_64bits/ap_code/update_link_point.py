#!/usr/bin/python

import os

def modify_linking_point():
	link_script = "linker_script.ld"
	kernel_bin_path = "../bsp_code/kernel.bin"

	kernel_size = os.stat(kernel_bin_path).st_size
	kernel_start = 0x117000
	kernel_end = kernel_size + kernel_start
	ap_full_code_start = hex(kernel_end + 0x2000 - kernel_end%0x1000)

	flag = ". = 0x"
	line_to_write = "    . = %s; "%ap_full_code_start
	comment = "/*kernel_entry_point = 0x117000, ap_start = kernel_entry_point+kernel_size+0x2000-kernel_size %% 0x1000*/\n"
	line_to_write += comment
	lines = []
	with open(link_script, "r") as f:
		lines = f.readlines()

	with open(link_script, "w") as out:
		for l in lines:
			if flag in l:
				l = line_to_write
			out.write(l)

	return






modify_linking_point()