; ** por compatibilidad se omiten tildes **
; ==============================================================================
; TRABAJO PRACTICO 3 - System Programming - ORGANIZACION DE COMPUTADOR II - FCEN
; ==============================================================================
; definicion de rutinas de atencion de interrupciones

%include "imprimir.mac"
extern flushEstadoBufferToScreen
extern flushMapaBufferToScreen
extern impl_syscall80_handler

extern notificarExcepcion
extern notificarRelojTick
extern notificarCambioPantalla
extern notificarTeclaNumerica
extern isEstadoTarea
extern getTareaActual
extern nextTaskToExecute
extern jmpToTask
extern notificarRelojIdle
extern dibujarBandera
extern setEjecutandoBandera
extern isEjecutandoBandera

BITS 32

;; PIC
extern fin_intr_pic1

;;
;; Definición de MACROS
;; -------------------------------------------------------------------------- ;;

;%macro ISR 1
;global _isr%1
;
;_isr%1:
;        jmp $
;%endmacro


%macro ISR_GENERIC_HANDLER 2
        global _isr%1

        %defstr intr_itoa_%1 %1
        interrupt_base_msg%1: db  "Interruption #", intr_itoa_%1, " - ", %2, " has ocurred"
        interrupt_base_len%1 equ $ - interrupt_base_msg%1
        
        _isr%1:
            imprimir_texto_mp interrupt_base_msg%1, interrupt_base_len%1, 0x4F, 0, 80-interrupt_base_len%1    
            ;EIP esta pusheado.
            pushad;Push all double-word (32-bit) registers onto stack
            pushfd;Push EFLAGS register onto stack       
            ;notificarExcepcion(unsigned int errorCode, unsigned int taskNumber, unsigned int EFLAGS, unsigned int EDI, unsigned int ESI, unsigned int EBP, unsigned int ESP, unsigned int EBX, unsigned int EDX, unsigned int ECX, unsigned int EAX, unsigned int EIP);
            ;pushad is equivalent to 
            ;PUSH EAX
            ;PUSH ECX
            ;PUSH EDX
            ;PUSH EBX
            ;PUSH ESP
            ;PUSH EBP
            ;PUSH ESI
            ;PUSH EDI
            call getTareaActual
            ;en eax tengo la taskNumber actual
            push eax;task number
            push %1;error code
            call notificarExcepcion
            add esp, 8;desapilo dos parametros extra

            ;salto a siguiente tarea porque esta murio y la desaloje de la estructura del scheduler, si hago iret BOOM

;   salto a la proxima tarea
        call nextTaskToExecute
        ;en eax tengo el selector para la gdt ya con el rpl piola
        push eax
        call jmpToTask
        add esp, 4

            popfd
            popad
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
ISR_GENERIC_HANDLER  8, '#DF Double Fault'
ISR_GENERIC_HANDLER  9, 'Coprocessor Segment Overrun (reserved)'
ISR_GENERIC_HANDLER 10, '#TS Invalid TSS'
ISR_GENERIC_HANDLER 11, '#NP Segment Not Present'
ISR_GENERIC_HANDLER 12, '#SS Stack-Segment Fault'
ISR_GENERIC_HANDLER 13, '#GP General Protection'
ISR_GENERIC_HANDLER 14, '#PF Page Fault'
ISR_GENERIC_HANDLER 15, '(Intel reserved. Do not use.)'
ISR_GENERIC_HANDLER 16, '#MF x87 FPU Floating-Point Error (Math Fault)'
ISR_GENERIC_HANDLER 17, '#AC Alignment Check'
ISR_GENERIC_HANDLER 18, '#MC Machine Check'
ISR_GENERIC_HANDLER 19, '#XM SIMD Floating-Point Exception'
ISR_GENERIC_HANDLER 20, '#VE Virtualization Exception'
;ISR_GENERIC_HANDLER 20//Reserved -> intel use only
;...//Reserved -> intel use only
;ISR_GENERIC_HANDLER 31//Reserved -> intel use only
;...user defined interrupts
ISR_GENERIC_HANDLER 1000, '#Int 0x66 durante contexto tareas'

;;
;; variables
;; -------------------------------------------------------------------------- ;;
rand_number:   dd 0x00000000
rand_limit:   dd 100

;;
;; Rutina de atención del RELOJ
;; -------------------------------------------------------------------------- ;;
global _isr32
_isr32:
        pushfd;Push EFLAGS register onto stack
        pushad;Push all double-word (32-bit) registers onto stack
        call fin_intr_pic1;comunicarle al al pic que ya se atendio la interrupción
        
        call proximo_rand
        ;void notificarRelojTick()
        call notificarRelojTick
        ;fijarme si estoy en la idle y notificarRelojIdle
        str ax
        cmp ax, 0xC0;(GDT_IDX_IDLE_TASK_DESC << 3 /*RPL 0*/)
        jne seguirIsr32
        call notificarRelojIdle
seguirIsr32:
        call fin_intr_pic1;comunicarle al al pic que ya se atendio la interrupción
        call isEstadoTarea
        cmp eax, 1;ignoro el check si estoy en modo sched de tareas :)        
        je noHayBanderaCorriendo
        call isEjecutandoBandera
        cmp eax, 1;pregunto si hay una bandera corriendo.
        jne noHayBanderaCorriendo
        ;en eax tengo el resultado booleano. si da true => desaoljar
        ;llamo a notificarExcepcion
        ;notificarExcepcion(unsigned int errorCode, unsigned int taskNumber, unsigned int EFLAGS, unsigned int EDI, unsigned int ESI, unsigned int EBP, unsigned int ESP, unsigned int EBX, unsigned int EDX, unsigned int ECX, unsigned int EAX, unsigned int EIP);
        ;pushad is equivalent to 
        ;PUSH EAX
        ;PUSH ECX
        ;PUSH EDX
        ;PUSH EBX
        ;PUSH ESP
        ;PUSH EBP
        ;PUSH ESI
        ;PUSH EDI
        call getTareaActual
        ;en eax tengo la taskNumber actual
        push eax;task number
        push 201;error code
        call notificarExcepcion
        add esp, 8;desapilo dos parametros extra
noHayBanderaCorriendo:
;salto a la proxima tarea
        call nextTaskToExecute
        ;en eax tengo el selector para la gdt ya con el rpl piola
        push eax
        call jmpToTask
        add esp, 4

        popad
        popfd
    iret

proximo_rand:;devuelve el numero en eax
        pushad
        inc DWORD [rand_number]
        mov ebx, [rand_number]
        cmp ebx, [rand_limit]
        jl .ok
                mov DWORD [rand_number], 0x0
                mov ebx, 0
        .ok:
        popad
        mov eax, [rand_number];devolucion por convencion C
        ret

;;
;; Rutina de atención del TECLADO
;; -------------------------------------------------------------------------- ;;

%macro numpadKeyListener 1
        %defstr numStr%1 %1
        numero%1Msg: db numStr%1        
        numKey%1Listener:
            push ebx
            mov ebx, [rand_number]
            cmp ebx, 0x07
            jg skipResetEBX_%1
            mov ebx, 0x4
skipResetEBX_%1:   
            sal ebx, 4
            add ebx, 0x04

            ;void notificarTeclaNumerica(unsigned char number, unsigned char format);
            push ebx
            push %1
            add word [esp], 48;paso a char
            call notificarTeclaNumerica
            add esp, 8;desapilo parametros

            pop ebx
        jmp _finisr33
%endmacro

numpadKeyListener 0
numpadKeyListener 1
numpadKeyListener 2
numpadKeyListener 3
numpadKeyListener 4
numpadKeyListener 5
numpadKeyListener 6
numpadKeyListener 7
numpadKeyListener 8
numpadKeyListener 9

global _isr33
_isr33:
        pushfd;Push EFLAGS register onto stack
        pushad;Push all double-word (32-bit) registers onto stack
        call fin_intr_pic1;comunicarle al al pic que ya se atendio la interrupción

        ;obtenemos el scan code
        in al, 0x60

        ;pregunto por teclas de pantallas
        cmp al, 0x32;pregunto si se presiono la tecla M        
        je mKeyListener

        cmp al, 0x12;pregunto si se presiono la tecla E
        je eKeyListener

;        cmp al, 0x24;pregunto si se presiono la tecla J
;        je test_fondear
;
;        cmp al, 0x30;pregunto si se presiono la tecla B
;        je test_navegar
;
;        cmp al, 0x23;pregunto si se presiono la tecla H
;        je test_canonear

;        cmp al, 0x1F;pregunto si se presiono la tecla S
;        je test_excepcion

        ;pregunto por numeros presionados
        cmp al, 0x52;0
        je numKey0Listener     
        
        cmp al, 0x4F;1
        je numKey1Listener
        
        cmp al, 0x50;2
        je numKey2Listener
        
        cmp al, 0x51;3
        je numKey3Listener
        
        cmp al, 0x4B;4
        je numKey4Listener
        
        cmp al, 0x4C;5
        je numKey5Listener
        
        cmp al, 0x4D;6
        je numKey6Listener
        
        cmp al, 0x47;7
        je numKey7Listener
        
        cmp al, 0x48;8
        je numKey8Listener

        cmp al, 0x49;9
        je numKey9Listener

_finisr33:
        popad
        popfd
    iret

;numerosarasa: dd 0
;test_fondear:
;    mov eax, 0x923;syscall code
;    inc dword [numerosarasa]
;    mov ebx, [numerosarasa]
;    sal ebx, 12
;    cmp ebx, 0xFF000
;    jle valorValido
;    mov dword [numerosarasa], 0
;valorValido:
;    int 0x50
;    jmp _finisr33

;test_navegar:
;    mov esi, [rand_number]
;    sal esi, 12
;    mov eax, 0xAEF;syscall code
;    mov ebx, 0x400000
;    mov ecx, 0x401000
;    add ebx, esi
;    add ecx, esi
;    int 0x50
;    jmp _finisr33

;test_excepcion:
;    int 17
;    jmp _finisr33

;test_canonear:
;    mov eax, 0x083A;syscall code
;    mov ebx, 0x200000;target    
;    mov esi, [rand_number]
;    sal esi, 12
;    add ebx, esi
;    mov ecx, 0x0;offset adentro de la tarea a copiar(97 bytes, ojo con el caso borde)
;    int 0x50
;    jmp _finisr33


mKeyListener:
                ;void notificarCambioPantalla(int screenIdSelected)
                push 0;screenId 0
                call notificarCambioPantalla
                add esp, 4; desapilo parametro
                jmp _finisr33


eKeyListener:
                ;void notificarCambioPantalla(int screenIdSelected)
                push 1;screenId 1
                call notificarCambioPantalla
                add esp, 4; desapilo parametro
                jmp _finisr33

;;
;; Rutinas de atención de las SYSCALLS
;; Voy a usar two level C-ASM ISR wrapping, para invocar la implementacion de las syscalls en C
;; -------------------------------------------------------------------------- ;;
global _isr80
_isr80: 
        pushfd;Push EFLAGS register onto stack => alineada
        pushad;Push all double-word (32-bit) registers onto stack -> numero par de pushes. => no me afecta el alineamiento

        ;;Una funcion para cumplir con la convencion debe:
        ;;Preservar EBX, ESI Y EDI
        ;;Retornar el resultado en EAX
        ;;No romper la pila
        ;;Antes de hacer un llamado, tenemos que tener la pila alineada a 4 Bytes
        ;;uso esi y edi como auxiliares, en ebx ya tengo un dato

        mov esi, eax;protejo por convencion c antes de la llamada a fin_intr_pic1
        mov edi, ecx;protejo por convencion c antes de la llamada a fin_intr_pic1
        ;ebx esta protegido por la convencion
        ;pila alineada a 4 bytes(todos los pushs son de 4 bytes)
        call fin_intr_pic1;comunicarle al al pic que ya se atendio la interrupción

        ;llamo a la funcion manejadora de interrupciones de sistema
        ;void impl_syscall80_handler(unsigned int eax, unsigned int ebx, unsigned int ecx)
        push edi;parametro ecx -> protegido mas arriba en edi
        push ebx;parametro ebx -> protegido mas arriba por la convencion C
        push esi;parametro eax -> protegido mas arriba en esi        
        call impl_syscall80_handler
        add esp, 12;deshago el pasaje de parametros(3 params * 4 bytes cada uno)

        ;saltar a tarea idle
        push 0xC0;(GDT_IDX_IDLE_TASK_DESC << 3 /*RPL 0*/)
        call jmpToTask; pasar a la idle
        add esp, 4

        ;mov eax, 0x42 ;ejercicio 5d

_finisr80:
        popad
        popfd
    iret

;;
;; Bandera syscall
;; ---------------------------------------------------------------------------- ;;

global _isr102
_isr102:

;mecanismo para ver si cae un clock en el medio de una bandera
;en contextManager variable global ejecutandoBandera booleana, cuando nextTask devuelve una bandera la pone en true
;y la int 0x66 la pone en false. si en una isr de clock esta activa esta variable ejecutandoBandera desalojamos a la tarea actual
;poner getters y setters en contextManager.

        ;mov eax, 0x42
        pushfd;Push EFLAGS register onto stack
        pushad;Push all double-word (32-bit) registers onto stack
        mov esi, eax;protejo por convencion c antes de la llamada a fin_intr_pic1
        call fin_intr_pic1;comunicarle al al pic que ya se atendio la interrupción

        ;validacion de contexto de banderas
        ;unsigned int isEstadoTarea();//1 es tarea; 0 es bandera
        call isEstadoTarea
        cmp eax, 0x0
        je validacionContextoOk 
        ;esta siendo llamada la interrupcion desde contexto de tareas, DEBE MORIR.
        ;llamo a notificarExcepcion
        ;notificarExcepcion(unsigned int errorCode, unsigned int taskNumber, unsigned int EFLAGS, unsigned int EDI, unsigned int ESI, unsigned int EBP, unsigned int ESP, unsigned int EBX, unsigned int EDX, unsigned int ECX, unsigned int EAX, unsigned int EIP);
        ;pushad is equivalent to 
        ;PUSH EAX
        ;PUSH ECX
        ;PUSH EDX
        ;PUSH EBX
        ;PUSH ESP
        ;PUSH EBP
        ;PUSH ESI
        ;PUSH EDI
        call getTareaActual
        ;en eax tengo la taskNumber actual
        push eax;task number
        push 200;error code
        call notificarExcepcion
        add esp, 8;desapilo dos parametros extra

        jmp _finisr102
validacionContextoOk:
        ;esta siendo llamada la interrupcion desde contexto de banderas, esta todo piola.

        ;en BANDERA_BUFFER 0x40001000 tengo el buffer armado de 50 caracteres de 2 bytes cada uno(100 bytes total)
        push esi;esta es virtual! hay que pasarla a fisica...
        call getTareaActual
        push eax
        ;dibujarBandera(flagNumber, getBandera(flagNumber));
        call dibujarBandera
        add esp, 8

        ;avisar que termino la bandera
        push 0
        call setEjecutandoBandera
        add esp, 4
        call getTareaActual
        ;saltar a tarea idle para completar el quantum de la bandera actual
        push 0xC0;(GDT_IDX_IDLE_TASK_DESC << 3 /*RPL 0*/)
        call jmpToTask; pasar a la idle
        add esp, 4
_finisr102:        
        popad
        popfd
    iret

;;
;; Funciones Auxiliares
;; -------------------------------------------------------------------------- ;;