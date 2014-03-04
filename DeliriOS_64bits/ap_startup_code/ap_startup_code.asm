;Este codigo es el codigo de inicializacion de los Application Processors.
;Dado que inician en modo real, deben iniciar el procesador desde cero.

global _start

%define breakpoint xchg bx, bx

BITS 16

section .text
_start:
    jmp mr_ap_start

; data_area

align 4
ap_full_code: dd 0xABBAABBA ; puntero al inicio del codigo de modulo estilo sueco

;ver gdt.c
gdt: dq 0x0

code_s32: dd 0x0000FFFF
	      dd 0x00CF9800

data_s: dd 0x0000FFFF
        dd 0x00CF9200

gdt_desc:   dw $ - gdt
            dd gdt

krnPML4T: dd 0x740000 ; segun defines.h

mr_ap_start:
	cli

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
    mov ax, 2<<3
    mov ds, ax

    ;apuntar cr3 al PML4
    mov eax, [krnPML4T]
    mov cr3, eax
    jmp [ap_full_code]

    ;falta para el modulo posta el codigo para medir tiempo
