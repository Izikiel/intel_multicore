; ** por compatibilidad se omiten tildes **
;==============================================================================
;TRABAJO PRACTICO 3 - System Programming - ORGANIZACION DE COMPUTADOR II - FCEN
;==============================================================================


;==============================================================================
; definicion de rutinas de atencion de interrupciones
%define breakpoint xchg bx, bx

%include "imprimir.mac"
;%include "sched.h"

%define pantalla 0xb8000
%define estado  0x2d000
%define mapa    0x2e000
%define logo    0x2f000

%define colo_tierra 00101010b
%define colo_mar 00011000b
%define color_tarea 01001111b
%define cosito_tierra "░"
%define cosito_mar "▓"
%define colorregistros 00011111b
%define TASK_ANCLA     0x40002000
%define TASK_CODE1     0x40000000
%define TASK_CODE2     0x40001000
%define AREA_MAR_INICIO 0x00100000  ;/* 1.0 MB     */
%define AREA_MAR_FIN    0x0077FFFF  ;/* 7.5 MB - 1 */

BITS 32
;==============================================================================





;==============================================================================
;; PIC
extern fin_intr_pic1

;==============================================================================
;; Scheduler
extern sched_proximo_indice
extern bandera_time
extern next_flag
extern current_flag
extern kill_task
extern get_live_tareas
extern index
extern bandera_mode
extern is_alive
;==============================================================================
;; MMU
extern directory_barco
extern mmu_mapear_pagina
;==============================================================================
;; TSS
extern setear_tss_bandera
extern reset_tarea_idle

;==============================================================================
;; Definición de MACROS
;; ----------------------------------------------------------------------------

%define buf_estado 0x0002d000

%macro saltar 2
    cmp ax, %2
    je %1
%endmacro

%macro rlt 3
    .rlt%1
    push ax
    push ecx
        xor ecx, ecx
        xor edx, edx
        add ecx, buf_estado
        add ecx, %2
        .ciclo_rlt%1%3
            mov ax, [ecx]
            mov ah, 01000000b
            mov [ecx], ax
            add ecx, 2
            inc edx
            cmp edx, 76
            jne .ciclo_rlt%1%3
    pop edx
    pop ecx
    pop ax
    jmp .fin_rlt%3
%endmacro

%macro rojear_linea_tarea 1
    push eax
    push ebx
    mov ah, 0x00
    saltar .rlt0, 0
    saltar .rlt1, 1
    saltar .rlt2, 2
    saltar .rlt3, 3
    saltar .rlt4, 4
    saltar .rlt5, 5
    saltar .rlt6, 6
    saltar .rlt7, 7
    jmp .fin_rlt%1
    rlt 0, (160*16+4), %1    ;+16*14+4)
    rlt 1, (160*17+4), %1    ;+16*14+4)
    rlt 2, (160*18+4), %1    ;+16*14+4)
    rlt 3, (160*19+4), %1    ;+16*14+4)
    rlt 4, (160*20+4), %1    ;+16*14+4)
    rlt 5, (160*21+4), %1    ;+16*14+4)
    rlt 6, (160*22+4), %1    ;+16*14+4)
    rlt 7, (160*23+4), %1    ;+16*14+4)
    .fin_rlt%1:
    pop ebx
    pop eax
%endmacro

;------------------------------------------------------------------------------
mueretareaobandera:
        xor eax, eax
        cmp byte [bandera_mode], 1
        jne .muerelatareaentarea
        mov al, [current_flag]
        jmp .muerelatareaenbandera
        .muerelatareaentarea:
        mov al, [index]
        .muerelatareaenbandera:
ret

%macro ISR 2
global _isr%1
_isr%1:
    cli
        call imprimir_reg_estado
        call mueretareaobandera
        rojear_linea_tarea al
        call mueretareaobandera
        add al, 49
        mov [el_ene_de_la_tarea], al
        imprimir_estado tarea_n, 8, 01001111b, 1, 70
        imprimir_estado msg%1, lmsg%1, 01001111b, 1, 50
        xor eax, eax
        call mueretareaobandera
        add al, 16
        imprimir_estado msg%1, lmsg%1, 01000000b, eax, 50
        call mueretareaobandera
        push eax
        call kill_task
        pop eax
        call next_flag
        mov word [selector], 0xc0   ; salto a tarea idle
        call fin_intr_pic1          ; de que sirve esto aca ?
    sti
    jmp far [offset]
;------------------------------------------------------------------------------
msg%1 db %2
lmsg%1 EQU $ - msg%1
%endmacro
tarea_n: db " tarea "
el_ene_de_la_tarea: db 0
;------------------------------------------------------------------------------
loqstaenlapantalla: db '0'
;==============================================================================

;==============================================================================
%macro imprimir_reg 5
    mov [pila1], eax
    mov [pila2], ebx
    mov [pila3], ecx
    mov ebx, %1
    mov [pila4], ebx

    mov ecx, 8
    .ciclo_imp_reg%1%5:
        mov eax, ebx
        and eax, 0x0000000F
        mov al, [hexas+eax]
        mov [string_reg+ecx-1], al
            imprimir_estado string_reg, 8, %2, %3, %4
        shr ebx, 4
        dec ecx
        jg .ciclo_imp_reg%1%5

    mov eax, [pila4]
    mov %1, eax
    mov ecx, [pila3]
    mov ebx, [pila2]
    mov eax, [pila1]
%endmacro
hexas: db '0123456789ABCDEF'
string_reg: db '--------'
pila1: dd 0x00000000
pila2: dd 0x00000000
pila3: dd 0x00000000
pila4: dd 0x00000000
;==============================================================================
imprimir_reg_estado:
%macro imp_reg_aux 4
    xor eax, eax
    mov eax, %1
    imprimir_reg eax, colorregistros, %2, %3, %4
%endmacro
    mov [maspila], eax
    imprimir_reg eax, colorregistros, 02, 55, 0
    imprimir_reg ebx, colorregistros, 03, 55, 1
    imprimir_reg ecx, colorregistros, 04, 55, 2
    imprimir_reg edx, colorregistros, 05, 55, 3
    imprimir_reg esi, colorregistros, 06, 55, 4
    imprimir_reg edi, colorregistros, 07, 55, 5
    imprimir_reg ebp, colorregistros, 08, 55, 6
    mov eax, esp
    add eax, 4
    imprimir_reg eax, colorregistros, 09, 55, 7
;    imprimir_reg eip, colorregistros, 10, 55, 8
    imprimir_reg cr0, colorregistros, 11, 55, 9
    imprimir_reg cr2, colorregistros, 12, 55, 10
    imprimir_reg cr3, colorregistros, 13, 55, 11
    imprimir_reg cr4, colorregistros, 14, 55, 12
    imp_reg_aux cs, 2, 68, 13
    imp_reg_aux ds, 3, 68, 14
    imp_reg_aux es, 4, 68, 15
    imp_reg_aux fs, 5, 68, 16
    imp_reg_aux gs, 6, 68, 17
    imp_reg_aux ss, 7, 68, 18
;    imprimir_reg eflags, colorregistros, 10, 68, 19
    mov eax, [maspila]
ret
;==============================================================================
%macro imprimo_banderas 0
    mov ebx, eax                    ;ebx = eax = *bufferbandera
    mov eax, [current_flag]         ;eax = *bandera
    cmp eax, 0
    jne .bandera2
        cmp byte [loqstaenlapantalla], 'e'
        jne .imprimo_bandera_estado1
                memcpybis 754148, ebx, 20
                add ebx, 20
                memcpybis 754308, ebx, 20
                add ebx, 20
                memcpybis 754468, ebx, 20
                add ebx, 20
                memcpybis 754628, ebx, 20
                add ebx, 20
                memcpybis 754788, ebx, 20
                sub ebx, 80
            .imprimo_bandera_estado1:
                memcpybis 184804, ebx, 20
                add ebx, 20
                memcpybis 184964, ebx, 20
                add ebx, 20
                memcpybis 185124, ebx, 20
                add ebx, 20
                memcpybis 185284, ebx, 20
                add ebx, 20
                memcpybis 185444, ebx, 20
            jmp .termine_imprimir_bandera
    .bandera2:
    cmp eax, 1
    jne .bandera3
        cmp byte [loqstaenlapantalla], "e"
        jne .imprimo_bandera_estado2
                memcpybis 754172, ebx, 20
                add ebx, 20
                memcpybis 754332, ebx, 20
                add ebx, 20
                memcpybis 754492, ebx, 20
                add ebx, 20
                memcpybis 754652, ebx, 20
                add ebx, 20
                memcpybis 754812, ebx, 20
                sub ebx, 80
            .imprimo_bandera_estado2:
                memcpybis 184828, ebx, 20
                add ebx, 20
                memcpybis 184988, ebx, 20
                add ebx, 20
                memcpybis 185148, ebx, 20
                add ebx, 20
                memcpybis 185308, ebx, 20
                add ebx, 20
                memcpybis 185468, ebx, 20
            jmp .termine_imprimir_bandera
    .bandera3:
    cmp eax, 2
    jne .bandera4
        cmp byte [loqstaenlapantalla], "e"
        jne .imprimo_bandera_estado3
                memcpybis 754196, ebx, 20
                add ebx, 20
                memcpybis 754356, ebx, 20
                add ebx, 20
                memcpybis 754516, ebx, 20
                add ebx, 20
                memcpybis 754676, ebx, 20
                add ebx, 20
                memcpybis 754836, ebx, 20
                sub ebx, 80
            .imprimo_bandera_estado3:
                memcpybis 184852, ebx, 20
                add ebx, 20
                memcpybis 185012, ebx, 20
                add ebx, 20
                memcpybis 185172, ebx, 20
                add ebx, 20
                memcpybis 185332, ebx, 20
                add ebx, 20
                memcpybis 185492, ebx, 20
            jmp .termine_imprimir_bandera
    .bandera4:
    cmp eax, 3
    jne .bandera5
        cmp byte [loqstaenlapantalla], "e"
        jne .imprimo_bandera_estado4
                memcpybis 754220, ebx, 20
                add ebx, 20
                memcpybis 754380, ebx, 20
                add ebx, 20
                memcpybis 754540, ebx, 20
                add ebx, 20
                memcpybis 754700, ebx, 20
                add ebx, 20
                memcpybis 754860, ebx, 20
                sub ebx, 80
            .imprimo_bandera_estado4:
                memcpybis 184876, ebx, 20
                add ebx, 20
                memcpybis 185036, ebx, 20
                add ebx, 20
                memcpybis 185196, ebx, 20
                add ebx, 20
                memcpybis 185356, ebx, 20
                add ebx, 20
                memcpybis 185516, ebx, 20
            jmp .termine_imprimir_bandera
    .bandera5:
    cmp eax, 4
    jne .bandera6
        cmp byte [loqstaenlapantalla], "e"
        jne .imprimo_bandera_estado5
                memcpybis 755268, ebx, 20
                add ebx, 20
                memcpybis 755428, ebx, 20
                add ebx, 20
                memcpybis 755588, ebx, 20
                add ebx, 20
                memcpybis 755748, ebx, 20
                add ebx, 20
                memcpybis 755908, ebx, 20
                sub ebx, 80
            .imprimo_bandera_estado5:
                memcpybis 185924, ebx, 20
                add ebx, 20
                memcpybis 186084, ebx, 20
                add ebx, 20
                memcpybis 186244, ebx, 20
                add ebx, 20
                memcpybis 186404, ebx, 20
                add ebx, 20
                memcpybis 186564, ebx, 20
            jmp .termine_imprimir_bandera
    .bandera6:
    cmp eax, 5
    jne .bandera7
        cmp byte [loqstaenlapantalla], "e"
        jne .imprimo_bandera_estado6
                memcpybis 755292, ebx, 20
                add ebx, 20
                memcpybis 755452, ebx, 20
                add ebx, 20
                memcpybis 755612, ebx, 20
                add ebx, 20
                memcpybis 755772, ebx, 20
                add ebx, 20
                memcpybis 755932, ebx, 20
                sub ebx, 80
            .imprimo_bandera_estado6:
                memcpybis 185948, ebx, 20
                add ebx, 20
                memcpybis 186108, ebx, 20
                add ebx, 20
                memcpybis 186268, ebx, 20
                add ebx, 20
                memcpybis 186428, ebx, 20
                add ebx, 20
                memcpybis 186588, ebx, 20
            jmp .termine_imprimir_bandera
    .bandera7:
    cmp eax, 6
    jne .bandera8
        cmp byte [loqstaenlapantalla], "e"
        jne .imprimo_bandera_estado7
                memcpybis 755316, ebx, 20
                add ebx, 20
                memcpybis 755476, ebx, 20
                add ebx, 20
                memcpybis 755636, ebx, 20
                add ebx, 20
                memcpybis 755796, ebx, 20
                add ebx, 20
                memcpybis 755956, ebx, 20
                sub ebx, 80
            .imprimo_bandera_estado7:
                memcpybis 185972, ebx, 20
                add ebx, 20
                memcpybis 186132, ebx, 20
                add ebx, 20
                memcpybis 186292, ebx, 20
                add ebx, 20
                memcpybis 186452, ebx, 20
                add ebx, 20
                memcpybis 186612, ebx, 20
            jmp .termine_imprimir_bandera
    .bandera8:
    cmp eax, 7
    jne .termine_imprimir_bandera
        cmp byte [loqstaenlapantalla], "e"
        jne .imprimo_bandera_estado8
                memcpybis 755340, ebx, 20
                add ebx, 20
                memcpybis 755500, ebx, 20
                add ebx, 20
                memcpybis 755660, ebx, 20
                add ebx, 20
                memcpybis 755820, ebx, 20
                add ebx, 20
                memcpybis 755980, ebx, 20
                sub ebx, 80
            .imprimo_bandera_estado8:
                memcpybis 185996, ebx, 20
                add ebx, 20
                memcpybis 186156, ebx, 20
                add ebx, 20
                memcpybis 186316, ebx, 20
                add ebx, 20
                memcpybis 186476, ebx, 20
                add ebx, 20
                memcpybis 186636, ebx, 20
    .termine_imprimir_bandera:
%endmacro

%macro mapear_pagina_cr3_actual 3
        mov eax, %3
        push eax

        mov eax, %2
        push eax

        mov eax, cr3
        push eax

        mov eax, %1
        push eax
        call mmu_mapear_pagina
        add esp, 4*4
%endmacro

;==============================================================================
;; Datos
;==============================================================================
maspila: dd 0



;==============================================================================
;; ----------------------------------------------------------------------------
; Scheduler
reloj_numero:           dd 0x00000000
reloj:                  db '|/-\'
reloj_time:             db '\|/-\'
offset:                 dd 0
selector:               dw 0
b_msg:                  db "bandera "
size_b_msg:             equ $-b_msg
t_msg:                  db "tarea "
size_t_msg:             equ $-t_msg


;==============================================================================



;==============================================================================
;; Rutina de atención de las EXCEPCIONES
;; ----------------------------------------------------------------------------
;==============================================================================
ISR 0, " Divide Error :(    "
ISR 1, " Reservado    :o    "
ISR 2, " Interrupt 'oh, MG' "
;==============================================================================
global _isr3
_isr3: breakpoint
iret
;==============================================================================
ISR 4, " Overflow (me pase) "
ISR 5, " Bound range exceded"
ISR 6, " Opcode inválido    "
ISR 7, " Device no esta     "
ISR 8, " Falta envido (x_x) "   ;doble foult
ISR 9, " ah ?               "
ISR 10, " Invalid TSS ????   "
ISR 11, " segmento ausente   "
ISR 12, " Stack Segment MAL  "
ISR 13, " GP (como el de F1) "
ISR 14, " Page fault (murio) "
ISR 15, " Intel no lo presta "
ISR 16, " FPU(nadie usa fpu) "
ISR 17, " Aligment Check     "
ISR 18, " Machine check      "
ISR 19, " SIMD le pifie      "
;==============================================================================

;==============================================================================
;; Rutina de atención del RELOJ
;; ----------------------------------------------------------------------------
global _isr32
_isr32:
    pushad
    call proximo_reloj
    call get_live_tareas
    cmp al, 0
    je .reset_pic

.check_bandera_mode:
    xor eax, eax
    call bandera_time
    cmp ax, 1
    jne .next_task
.check_alive_flag:
    call is_alive
    cmp al, 1
    je .change_cr3
    call next_flag
    jmp .check_alive_flag
.change_cr3:
    mov eax, [current_flag]
    push eax
    call directory_barco
    mov cr3, eax
    pop eax

.next_flag:
    call setear_tss_bandera
    mov word [selector], 0x108
    call fin_intr_pic1
    jmp far [offset]
    jmp .fin

.next_task:
    call get_live_tareas
    cmp al, 0
    je .jmp_idle
    call sched_proximo_indice
    cmp ax, 0
    ja .jmp_task
    je .reset_pic

.jmp_task:
    mov [selector], ax
    call fin_intr_pic1
    jmp far [offset]
    jmp .fin

.jmp_idle:
    ;call imprimir_globitos
    ;breakpoint
    call fin_intr_pic1
    popad
    iret
    jmp .fin

.reset_pic:
    call fin_intr_pic1
.fin:
    popad
iret
;==============================================================================
;; Rutina de atención del TECLADO
;; ----------------------------------------------------------------------------
global _isr33
_isr33:
    pushad
    xor ax, ax
    in al, 0x60
    saltar tec_m, 0x32
    saltar tec_e, 0x12
    saltar tec_r, 0x13
    saltar tec_l, 0x26
    jmp fin33
;; ----------------------------------------------------------------------------
    tec_m:
        cmp byte [loqstaenlapantalla], 'm'
        je finm33
        memcpy pantalla, mapa, 4000
        finm33:
        mov byte [loqstaenlapantalla], 'm'
        jmp fin33
;; ----------------------------------------------------------------------------
    tec_e:
        cmp byte [loqstaenlapantalla], 'e'
        je fine33
;        breakpoint
        memcpy pantalla, estado, 4000
;        breakpoint
        fine33:
        mov byte [loqstaenlapantalla], 'e'
        jmp fin33
;; ----------------------------------------------------------------------------
    tec_r:
        jmp fin33
regst: dd "000 "
;------------------------------------------------------------------------------
    tec_l:
        cmp byte [loqstaenlapantalla], 'l'
        je finl33
        memcpy pantalla, logo, 4000
        finl33:
        mov byte [loqstaenlapantalla], 'l'
        jmp fin33
;; ----------------------------------------------------------------------------
    fin33:
    call fin_intr_pic1
    popad
iret
;==============================================================================

;==============================================================================
;; Rutinas de atención de las SYSCALLS
;; ----------------------------------------------------------------------------
global _isr80   ;int 0x50
_isr80:
    cli
    cmp byte [bandera_mode], 0
    je syscall
    jmp _isr1
syscall:
    saltar ancla, 0x923
    saltar misil, 0x83A
    saltar navegar, 0xAEF
    jmp fin80
;; ----------------------------------------------------------------------------
    ancla:
        mapear_pagina_cr3_actual TASK_ANCLA, ebx, 5
        jmp fin80
;; ----------------------------------------------------------------------------
    misil:
        cmp ecx, AREA_MAR_INICIO
        jb _isr13
        memcpybis ecx, ebx, 97
        jmp fin80
;; ----------------------------------------------------------------------------
    navegar:
        memcpy ebx, TASK_CODE1, 0x1000
        mapear_pagina_cr3_actual TASK_CODE1, ebx, 5

        memcpy ecx, TASK_CODE2, 0x1000
        mapear_pagina_cr3_actual TASK_CODE2, ecx, 7

        jmp fin80
;; ----------------------------------------------------------------------------
    fin80:
    sti
iret
;; ----------------------------------------------------------------------------

global _isr102  ;int 0x66
_isr102:
    cli
    cmp byte [bandera_mode], 1
    je banderear
    jmp _isr1
banderear:
    ;buffer
    ;cambio de tarea a idle
    ;para todas las banderas
        jmp fin102
        fin102:

    imprimo_banderas
    call next_flag
    mov word [selector], 0xc0 ; salto a tarea idle
    ;call fin_intr_pic1
    sti
    jmp far [offset]
iret
;no se limpia aca el fin_intr_pic1 ?????????/??????
;==============================================================================
;; Funciones Auxiliares
;; ----------------------------------------------------------------------------
proximo_reloj:
    pushad

    imprimir_texto_mp reloj_time, 1, 0000111b, 24, 79

    mov eax, [reloj_time+1]
    mov [reloj_time], eax
    mov al, [reloj_time]
    mov [reloj_time+4], al

    popad
    ret
;==============================================================================