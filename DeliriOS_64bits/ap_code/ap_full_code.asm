;ORG 0x121000

section .text

global _start

%include "../macros/asm_screen_utils.mac"

;; IDT
extern IDT_DESC

;; STACK
extern core_stack_ptrs

%macro get_lapic_id 0  ; para distinguir los procesadores entre si
    xor rax, rax ; por si las moscas
    mov eax, 0xb ; Manual de intel capitulo 8.4.5 Read 32-bit APIC ID from CPUID leaf 0BH
    cpuid
    mov eax, edx
%endmacro

%define breakpoint xchg bx, bx

BITS 32
_start:
    jmp (2<<3):long_mode


;------------------------------------------------------------------------------------------------------
;------------------------------- comienzo modo largo --------------------------------------------------
;------------------------------------------------------------------------------------------------------


BITS 64
long_mode:
    ;limpio los registros
    xor rax, rax
    xor rbx, rbx
    xor rcx, rcx
    xor rdx, rdx
    xor rsi, rsi
    xor rdi, rdi
    xor rbp, rbp
    xor rsp, rsp
    xor r8, r8
    xor r9, r9
    xor r10, r10
    xor r11, r11
    xor r12, r12
    xor r13, r13
    xor r14, r14
    xor r15, r15


    ;setear la pila en para el kernel
    get_lapic_id
    
    ;xor rax, rax
    ;mov eax, 0xfee00020
    ;mov eax, [eax]
    ;shr eax, 24
    ;and eax, 0xFF
    ;breakpoint
    ; deberia ser asi, pero ta reventando en 64 leyendo fruta :(


    mov esp, [core_stack_ptrs + eax * 4];la pila va a partir de kernelStackPtrBSP(expand down, OJO)
    

    
    MOV rbp, rsp<;pongo base y tope juntos.

    xchg bx, bx
    ;levanto la IDT de 64 bits, es unica para todos los cores
    lidt [IDT_DESC]
    ;la IDT esta inicializada por el BSP

    ;el controlador de interrupciones ya esta inicializado por el BSP

    ;imprimir mensaje en pantalla
    imprimir_texto_ml mensaje_ap_started_msg, mensaje_ap_started_len, 0x0F, 10, 0

    ;llamo al entrypoint en kmain64
    ;call startKernel64_APMODE


    xchg bx, bx
    jmp $



mensaje_ap_started_msg: db "AP en 64 :D"
mensaje_ap_started_len: equ $ - mensaje_ap_started_msg
    ;fin inicio kernel para AP en 64 bits!

;    start_sort:
;        cmp byte [start], 0
;        je start_sort
;        xor rsi, rsi
;        xor rdi, rdi
;        xor rdx, rdx
;
;        mov esi, [start_point]
;        mov rdi, array_global
;        mov rdx, 1
;
;        add rdi, rsi
;
;        call mergesort
;        mov byte [done], 1
;        imprimir_texto_ml array_global, 52, 0x02, 11, 0
;
;    start_merge_:
;        call do_reverse_merge
;
;
;    haltApCore:
;        imprimir_texto_ml array_global, 52, 0x02, 11, 0
;        jmp haltApCore

; -------------------------------------------------------------------------- ;;