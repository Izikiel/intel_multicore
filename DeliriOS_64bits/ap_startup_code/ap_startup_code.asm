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
;mini gdt para mp
gdt: dq 0x0

code_s32: dd 0x0000FFFF
	      dd 0x00CF9800

code_s64: dd 0x0000FFFF
          dd 0x00AF9800

data_s: dd 0x0000FFFF
        dd 0x00CF9200

gdt_desc:   dw $ - gdt
            dd gdt

krnPML4T: dd 0x740000 ; segun defines.h

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
    mov ax, 3<<3
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ;apuntar cr3 al PML4
    mov eax, [krnPML4T]
    mov cr3, eax

    ;prender el bit 5(6th bit) para activar PAE
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    mov ecx, 0xC0000080          ; Seleccionamos EFER MSR poniendo 0xC0000080 en ECX
    rdmsr                        ; Leemos el registro en EDX:EAX.
    or eax, 1 << 8               ; Seteamos el bit de modo largo que es el noveno bit (contando desde 0) osea el bit 8.
    wrmsr                        ; Escribimos nuevamente al registro.

    mov eax, cr0                 ; Obtenemos el registro de control 0 actual.
    or eax, 1 << 31              ; Habilitamos el bit de Paginacion que es el 32vo bit (contando desde 0) osea el bit 31
    mov cr0, eax                 ; escribimos el nuevo valor sobre el registro de control

    jmp [ap_full_code]

    ;falta para el modulo posta el codigo para medir tiempo
