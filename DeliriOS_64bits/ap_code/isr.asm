%include "../macros/long_mode_macros.mac"
%include "../macros/asm_screen_utils.mac"
BITS 64

;; PIC
extern fin_intr_pic1

;; contextManager
extern notificarRelojTick
extern notificarTecla

;; kmain64
extern kernel_panic

;; sorting
extern sort_ap_int
extern merge_ap_int
extern copy_ap_int

extern ap_jump

%define eoi_register_apic 0xFEE000B0
%macro interrupt_finished 0
    push rax
    mov rax, eoi_register_apic
    mov [rax], eax
    pop rax
%endmacro

%macro user_interrupt 1
global _isr%1
    _isr%1:
    interrupt_finished
    iretq
%endmacro

%macro set_user_interrupts 2
    %assign j %1
    %rep %2-%1
    user_interrupt j
    %assign j j+1
    %endrep
%endmacro

%macro ISR_GENERIC_HANDLER_ERR_CODE 2
        global _isr%1

        %defstr intr_itoa_%1 %1
        interrupt_base_msg%1: db  "KERNEL PANIC - Exception #", intr_itoa_%1, " : ", %2, " has ocurred", 0
        interrupt_base_len%1 equ $ - interrupt_base_msg%1

        _isr%1:
            ;xchg bx, bx
            add rsp, 8;desapilo (e ignoro) el error code.
            pushaq
            imprimir_texto_ml interrupt_base_msg%1, interrupt_base_len%1, 0x4F, 0, 80-interrupt_base_len%1

            xchg bx, bx ;nota para mi yo del futuro: es una buena idea parar aca
            ;y debugear el iretq y revisar si es trap , fault o interrupt para que no lopee en la instr que explota
            popaq
        iretq
%endmacro


%macro ISR_GENERIC_HANDLER 2
        global _isr%1

        %defstr intr_itoa_%1 %1
        interrupt_base_msg%1: db  "KERNEL PANIC - Exception #", intr_itoa_%1, " : ", %2, " has ocurred", 0
        interrupt_base_len%1 equ $ - interrupt_base_msg%1

        _isr%1:
        xchg bx, bx
            pushaq
            imprimir_texto_ml interrupt_base_msg%1, interrupt_base_len%1, 0x4F, 0, 80-interrupt_base_len%1

            xchg bx, bx ;nota para mi yo del futuro: es una buena idea parar aca
            ;y debugear el iretq y revisar si es trap , fault o interrupt para que no lopee en la instr que explota
        popaq
        iretq
%endmacro

;;
;; Rutinas de atención de las EXCEPCIONES
;;
ISR_GENERIC_HANDLER  0, '#DE Divide Error'
ISR_GENERIC_HANDLER  1, '#DB RESERVED'
ISR_GENERIC_HANDLER  2, 'NMI Interrupt'
ISR_GENERIC_HANDLER  3, '#BP Breakpoint'
ISR_GENERIC_HANDLER  4, '#OF Overflow'
ISR_GENERIC_HANDLER  5, '#BR BOUND Range Exceeded'
ISR_GENERIC_HANDLER  6, '#UD Invalid Opcode (Undefined Opcode)'
ISR_GENERIC_HANDLER  7, '#NM Device Not Available (No Math Coprocessor)'
ISR_GENERIC_HANDLER_ERR_CODE  8, '#DF Double Fault'
ISR_GENERIC_HANDLER_ERR_CODE  9, 'Coprocessor Segment Overrun (reserved)'; --> desde 386 no se produce esta excepcion
ISR_GENERIC_HANDLER_ERR_CODE 10, '#TS Invalid TSS'
ISR_GENERIC_HANDLER_ERR_CODE 11, '#NP Segment Not Present'
ISR_GENERIC_HANDLER_ERR_CODE 12, '#SS Stack-Segment Fault'
ISR_GENERIC_HANDLER_ERR_CODE 13, '#GP General Protection'
ISR_GENERIC_HANDLER_ERR_CODE 14, '#PF Page Fault'
ISR_GENERIC_HANDLER 15, '(Intel reserved. Do not use.)'
ISR_GENERIC_HANDLER 16, '#MF x87 FPU Floating-Point Error (Math Fault)'
ISR_GENERIC_HANDLER_ERR_CODE 17, '#AC Alignment Check'
ISR_GENERIC_HANDLER 18, '#MC Machine Check'
ISR_GENERIC_HANDLER 19, '#XM SIMD Floating-Point Exception'
ISR_GENERIC_HANDLER 20, '#VE Virtualization Exception'
;ISR_GENERIC_HANDLER 20//Reserved -> intel use only
;...//Reserved -> intel use only
;ISR_GENERIC_HANDLER 31//Reserved -> intel use only

;...user defined data

;...user defined interrupts


set_user_interrupts 21, 39
set_user_interrupts 43,143
set_user_interrupts 144,256

global _isr39
_isr39:
    pushaq
    call ap_jump
    popaq
    interrupt_finished
    iretq


global _isr40
_isr40:
    pushaq
    mov rax, 40
    call sort_ap_int
    popaq
    interrupt_finished
    iretq

global _isr41
_isr41:
    pushaq
    mov rax, 41
    call merge_ap_int
    popaq
    interrupt_finished
    iretq

global _isr42
_isr42:
    pushaq
    mov rax, 42
    call copy_ap_int
    popaq
    interrupt_finished
    iretq

;Ignorar la interrupcion
;Sirve para evitar interrupciones espurias
global _isr_spurious
_isr_spurious:
    xchg bx, bx
    interrupt_finished
    iretq