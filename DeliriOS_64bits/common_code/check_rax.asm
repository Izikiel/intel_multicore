global check_rax

%define done_address 		0x200700

check_rax:
	xor rax, rax
	mov byte [done_address], 1
	.loop:
		hlt
		cmp rax, 1
		jne .loop
	ret