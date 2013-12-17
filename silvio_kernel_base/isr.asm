%include "imprimir.mac"
BITS 32

;; PIC
extern fin_intr_pic1

;; contextManager
extern notificarExcepcion
extern notificarRelojTick
extern notificarTecla


%macro ISR_GENERIC_HANDLER_ERR_CODE 2
        global _isr%1

        %defstr intr_itoa_%1 %1
        interrupt_base_msg%1: db  "KERNEL PANIC - Exception #", intr_itoa_%1, " : ", %2, " has ocurred"
        interrupt_base_len%1 equ $ - interrupt_base_msg%1
        
        _isr%1:
            add esp, 4;desapilo (e ignoro)serror code.        
            imprimir_texto_mp interrupt_base_msg%1, interrupt_base_len%1, 0x4F, 0, 80-interrupt_base_len%1    
            ;wrapper en contextManager
            ;notificarExcepcion(unsigned int errorCode, unsigned int EFLAGS, unsigned int EDI,
            ; unsigned int ESI, unsigned int EBP, unsigned int ESP, unsigned int EBX, 
            ; unsigned int EDX, unsigned int ECX, unsigned int EAX, unsigned int EIP);
            ;pasaje de parametros
            ;EIP esta pusheado por convencion.
            pushad;Push all double-word (32-bit) registers onto stack
            pushfd;Push EFLAGS register onto stack       
            ;pushad is equivalent to 
            ;PUSH EAX
            ;PUSH ECX
            ;PUSH EDX
            ;PUSH EBX
            ;PUSH ESP
            ;PUSH EBP
            ;PUSH ESI
            ;PUSH EDI
            push %1;error code
            call notificarExcepcion
            add esp, 4;desapilo parametro extra            
            
            popfd
            popad
            xchg bx, bx ;nota para mi yo del futuro: es una buena idea parar aca
            ;y debugear el iret y revisar si es trap , fault o interrupt para que no lopee en la instr que explota
        iret
%endmacro


%macro ISR_GENERIC_HANDLER 2
        global _isr%1

        %defstr intr_itoa_%1 %1
        interrupt_base_msg%1: db  "KERNEL PANIC - Exception #", intr_itoa_%1, " : ", %2, " has ocurred"
        interrupt_base_len%1 equ $ - interrupt_base_msg%1
        
        _isr%1:
            imprimir_texto_mp interrupt_base_msg%1, interrupt_base_len%1, 0x4F, 0, 80-interrupt_base_len%1    
            ;wrapper en contextManager
            ;notificarExcepcion(unsigned int errorCode, unsigned int EFLAGS, unsigned int EDI,
            ; unsigned int ESI, unsigned int EBP, unsigned int ESP, unsigned int EBX, 
            ; unsigned int EDX, unsigned int ECX, unsigned int EAX, unsigned int EIP);
            ;pasaje de parametros
            ;EIP esta pusheado por convencion.
            pushad;Push all double-word (32-bit) registers onto stack
            pushfd;Push EFLAGS register onto stack       
            ;pushad is equivalent to 
            ;PUSH EAX
            ;PUSH ECX
            ;PUSH EDX
            ;PUSH EBX
            ;PUSH ESP
            ;PUSH EBP
            ;PUSH ESI
            ;PUSH EDI
            push %1;error code
            call notificarExcepcion
            add esp, 4;desapilo parametro extra            
            
            popfd
            popad
            xchg bx, bx ;nota para mi yo del futuro: es una buena idea parar
            ;aca y debugear el iret y revisar si es trap , fault o interrupt para que no lopee en la instr que explota
        iret
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
;...user defined interrupts

;;
;; Rutina de atención del RELOJ
;; -------------------------------------------------------------------------- ;;
global _isr32
_isr32:
        pushfd;Push EFLAGS register onto stack
        pushad;Push all double-word (32-bit) registers onto stack
        call fin_intr_pic1;comunicarle al al pic que ya se atendio la interrupción        
        ;wrapper en contextManager
        ;void notificarRelojTick()
        call notificarRelojTick
        popad
        popfd
    iret

;;
;; Rutina de atención del TECLADO
;; -------------------------------------------------------------------------- ;;

global _isr33
_isr33:
        pushfd;Push EFLAGS register onto stack
        pushad;Push all double-word (32-bit) registers onto stack
        call fin_intr_pic1;comunicarle al al pic que ya se atendio la interrupción
        ;obtenemos el scan code
        in al, 0x60

        ;wrapper en contextManager
        ;void notificarTecla(unsigned char keyCode);
        push ax;8 bits(unsigned char) --> no me deja pushear al
        call notificarTecla
        pop ax
        
        popad
        popfd
    iret