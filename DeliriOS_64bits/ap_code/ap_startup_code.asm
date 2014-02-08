;Este codigo es el codigo de inicializacion de los Application Processors.
;Dado que inician en modo real, deben iniciar el procesador desde cero.
BITS 16

section .text

jmp mr_ap_start

; data_area
; ver como carajo setear ! :D

align_var: dw 0xb0b0
ap_full_code: dd 0xABBAABBA ; puntero al inicio del codigo de modulo estilo sueco


gdt: dq 0x0

code_s32: dd 0x0000FFFF
	      dd 0x00CF9800

code_s64: dd 0x0000FFFF
		  dd 0x00AF9800 ; ver gdt.c en bsp_code

data_s: dd 0x0000FFFF
	    dd 0x00CF9200

gdt_desc:   dw $ - gdt
            dd gdt

mr_ap_start:
	cli
    ;hlt

    ; A20 YA ESTA HABILITADO POR EL BSP

    ; cargar la GDT;
    lgdt [gdt_desc]

    ; setear el bit PE del registro CR0
    mov eax, CR0;levanto registro CR0 para pasar a modo protegido
    or eax, 1;hago un or con una mascara de 0...1 para setear el bit de modo protegido
    mov CR0, eax

    ; pasar a modo protegido
    jmp (1<<3):f_mp_ap_start; saltamos a modo protegido, modificamos el cs con un jump y la eip(program counter)
    ;{index:1 | gdt/ldt: 0 | rpl: 00} => 1000
    ;aca setie el selector de segmento cs al segmento de codigo del kernel

BITS 32

f_mp_ap_start:
	mov ax, 3<<3 ; 3 << 3, segmento de datos
	; cargo selectores de segmento
	mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov ss, ax

    xor eax, eax
    xor ebx, ebx

    jmp $

	jmp [ap_full_code]

    ;falta para el modulo posta el codigo para medir tiempo
