section .text

%define CODE_SEGMENT 0x08
%define DATA_SEGMENT 0x10

;Funciones para cargar la GDT
global gdt_flush
extern GDT_DESC
gdt_flush:
	push ebp
	mov ebp, esp
	lgdt [GDT_DESC]
	mov ax, 0x10
	mov ds, ax
	mov es, ax
	mov fs, ax
	mov es, ax
	mov gs, ax
	mov ss, ax
	jmp 0x08:_do_gdt_flush
_do_gdt_flush:
	pop ebp
	ret

;Funciones para cargar la IDT
global idt_flush
extern IDT_DESC
idt_flush:
	push ebp
	mov ebp,esp
	lidt [IDT_DESC]
	pop ebp
	ret
