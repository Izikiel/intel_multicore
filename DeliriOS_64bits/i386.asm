global getRAX
global getRBX
global getRCX
global getRDX
global getRSI
global getRDI
global getRBP
global getRSP
global getR8
global getR9
global getR10
global getR11
global getR12
global getR13
global getR14
global getR15
global getRIP
global getCS
global getDS
global getES
global getFS
global getGS
global getSS
global getCR0
global getCR2
global getCR3
global getCR4
global getRFLAGS
global haltCpu
global irq_cli
global irq_sti
global breakpoint

getRAX:;en RAX tenemos RAX
	ret

getRBX:
	mov rax, rbx
	ret

getRCX:
	mov rax, rcx
	ret

getRDX:
	mov rax, rdx
	ret

getRSI:
	mov rax, rsi
	ret

getRDI:
	mov rax, rdi
	ret

getRBP:
	mov rax, rbp
	ret

getRSP:
	mov rax, rsp
	ret

getR8:
	mov rax, r8
	ret

getR9:
	mov rax, r9
	ret

getR10:
	mov rax, r10
	ret

getR11:
	mov rax, r11
	ret

getR12:
	mov rax, r12
	ret

getR13:
	mov rax, r13
	ret

getR14:
	mov rax, r14
	ret

getR15:
	mov rax, r15
	ret

getRIP:
	mov rax, [rsp];leo la direccion de retorno
	ret

getCS:
	mov ax, cs
	ret

getDS:
	mov ax, ds
	ret

getES:
	mov ax, es
	ret

getFS:
	mov ax, fs
	ret

getGS:
	mov ax, gs
	ret

getSS:
	mov ax, ss
	ret

getCR0:
	mov rax, cr0
	ret

getCR2:
	mov rax, cr2
	ret

getCR3:
	mov rax, cr3
	ret

getCR4:
	mov rax, cr4
	ret

getCR8:
	mov rax, cr8
	ret

getRFLAGS:
	pushfq;push 64 bits rflags
	pop RAX;read them into rax
	ret

haltCpu:
	cli
	hlt
	jmp haltCpu

irq_cli:
	cli
	ret

irq_sti:
	sti
	ret

breakpoint:
	xchg bx, bx
	ret