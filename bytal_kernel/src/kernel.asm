;==============================================================================
;==============================================================================
;TRABAJO PRACTICO 3 - System Programming - ORGANIZACION DE COMPUTADOR II - FCEN
;==============================================================================
;==============================================================================





;==============================================================================
%define breakpoint xchg bx, bx

%include "imprimir.mac"
;==============================================================================





;==============================================================================
global start
;------------------------------------------------------------------------------
;; GDT
extern GDT_DESC
;------------------------------------------------------------------------------
;; IDT
extern IDT_DESC
extern idt_inicializar
;------------------------------------------------------------------------------
;; PIC
extern deshabilitar_pic
extern resetear_pic
extern habilitar_pic
;------------------------------------------------------------------------------
;; Paginacion
extern mmu_inicializar_dir_kernel
extern mmu_inicializar
extern mmu_mapear_pagina
extern mmu_unmapear_pagina
extern pagina_barco
extern ImprimirLogo
extern inicializo_mapa
extern inicializo_estado
;==============================================================================
;; TSS
extern tss_inicializar
;; SCHEDULER
extern sched_inicializar

;==============================================================================
;; Saltear seccion de datos
jmp start
;==============================================================================



;==============================================================================
;; Seccion de datos.
%include "a20.asm"
;------------------------------------------------------------------------------
iniciando_mr_msg db     'Iniciando kernel (Modo Real)...'
iniciando_mr_len equ    $ - iniciando_mr_msg
;------------------------------------------------------------------------------
iniciando_mp_msg db     'Iniciando kernel (Modo Protegido)...'
iniciando_mp_len equ    $ - iniciando_mp_msg
;------------------------------------------------------------------------------
;==============================================================================




;==============================================================================
;------------------------------------------------------------------------------
;; Seccion de código.
;------------------------------------------------------------------------------
;; Punto de entrada del kernel.
BITS 16
start:
;------------------------------------------------------------------------------
    ; Deshabilitar interrupciones
    cli
;------------------------------------------------------------------------------
    ; Imprimir mensaje de bienvenida
    imprimir_texto_mr iniciando_mr_msg, iniciando_mr_len, 0x07, 0, 0
;------------------------------------------------------------------------------
    ; habilitar A20
    call habilitar_A20
;------------------------------------------------------------------------------
    ; cargar la GDT
load_gdt:
    lgdt [GDT_DESC]
;------------------------------------------------------------------------------
    ;   Seteo el bit 0 de cr0
enable_protected_mode:
    mov eax, cr0
    bts eax, 0
    mov cr0, eax
;------------------------------------------------------------------------------
    ;   Salto a modo protegido
    jmp 0x90:protected_mode
;------------------------------------------------------------------------------
BITS 32
    ;   Modo protegido
protected_mode:
    xor eax, eax
    ; el segmento de codigo se setea con el salto a modo protegido
    mov ax, 0xa0   ;datos  kernel 20 << 3
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    imprimir_texto_mp iniciando_mp_msg, iniciando_mp_len, 0x7, 0, 0
;------------------------------------------------------------------------------
    ;   Seteo el stack
set_stack:
    mov ss, ax
    mov esp, 0x27000 ; ver enunciado
    mov ebp, esp
;------------------------------------------------------------------------------
    ;   pintar pantalla
pintoPantalla:
    call ImprimirLogo
    call inicializo_mapa
    call inicializo_estado
    memcpy 0x2F000,0xb8000, 4000
;------------------------------------------------------------------------------
    ; inicializar el manejador de memoria

;------------------------------------------------------------------------------
    ; inicializar el directorio de paginas
load_page_dir:
    mov eax, 0x27000
    mov cr3, eax
;------------------------------------------------------------------------------
    ; inicializar memoria de tareas

;------------------------------------------------------------------------------
    ; habilitar paginacion
enable_paging:
    call mmu_inicializar_dir_kernel
    mov eax, cr0
    bts eax, 31
    mov cr0, eax

load_tasks:
    call mmu_inicializar
;------------------------------------------------------------------------------
    ; inicializar tarea idle

;------------------------------------------------------------------------------
    ; inicializar todas las tsss

;------------------------------------------------------------------------------
    ; inicializar entradas de la gdt de las tsss

;------------------------------------------------------------------------------
    ; inicializar el scheduler

;------------------------------------------------------------------------------
    ;   deshabilito el PIC
    call deshabilitar_pic
    call resetear_pic
    call habilitar_pic
;------------------------------------------------------------------------------
    ; inicializar la IDT
    lidt [IDT_DESC]
    call idt_inicializar
;------------------------------------------------------------------------------
    ; configurar controlador de interrupciones
    sti
;------------------------------------------------------------------------------
    ; cargar la tarea inicial

;------------------------------------------------------------------------------
    ; saltar a la primer tarea

;------------------------------------------------------------------------------
    ; Ciclar infinitamente (por si algo sale mal...)
tarea_inicial:
    call sched_inicializar
    call tss_inicializar
    mov ax, 0xb8
    ltr ax
tarea_idle:
    jmp 0xc0:0
tarea1:
; con el scheduler recien se podra hacer el salto de modo con iret
;    breakpoint
    call imprimo_paginas_en_buffer
    jmp 0xc0:0
fin:
;    breakpoint
    jmp fin
;; ----------------------------------------------------------------------------
;==============================================================================


%macro imprimir_dire_pagina 5
    push eax
    push ebx
    push ecx
    mov ebx, %1
    push ebx

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

    pop eax
    mov %1, eax
    pop ecx
    pop ebx
    pop eax
%endmacro
hexas: db '0123456789ABCDEF'
string_reg: db '--------'
pila1: dd 0x00000000
pila2: dd 0x00000000
pila3: dd 0x00000000
pila4: dd 0x00000000
;==============================================================================
;   unsigned int pagina_barco(unsigned int barco, unsigned int pagina);
imprimo_paginas_en_buffer:

    mov byte [loqueimprimo], 49
    push 0
    push 0
        call pagina_barco
        imprimir_dire_pagina eax, 00011111b, 16, 06, 00
        shr eax, 12
        imprimir_mapa loqueimprimo, 1, 01000000b, 0, eax
    pop eax
    pop eax
    push 1
    push 0
        call pagina_barco
        imprimir_dire_pagina eax, 00011111b, 16, 20, 01
        shr eax, 12
        imprimir_mapa loqueimprimo, 1, 01000000b, 0, eax
    pop eax
    pop eax
    push 2
    push 0
        call pagina_barco
        imprimir_dire_pagina eax, 00011111b, 16, 34, 02
        shr eax, 12
        imprimir_mapa loqueimprimo, 1, 01000000b, 0, eax
    add ebp, 8
    pop eax
    pop eax

    mov byte [loqueimprimo], 50
    push 0
    push 1
        call pagina_barco
        imprimir_dire_pagina eax, 00011111b, 17, 06, 10
                shr eax, 12
        imprimir_mapa loqueimprimo, 1, 01000000b, 0, eax
    pop eax
    pop eax
    push 1
    push 1
        call pagina_barco
        imprimir_dire_pagina eax, 00011111b, 17, 20, 11
                shr eax, 12
        imprimir_mapa loqueimprimo, 1, 01000000b, 0, eax
    pop eax
    pop eax
    push 2
    push 1
        call pagina_barco
        imprimir_dire_pagina eax, 00011111b, 17, 34, 12
                shr eax, 12
        imprimir_mapa loqueimprimo, 1, 01000000b, 0, eax
    add ebp, 8
    pop eax
    pop eax

    mov byte [loqueimprimo], 51
    push 0
    push 2
        call pagina_barco
        imprimir_dire_pagina eax, 00011111b, 18, 06, 20
                shr eax, 12
        imprimir_mapa loqueimprimo, 1, 01000000b, 0, eax
    pop eax
    pop eax
    push 1
    push 2
        call pagina_barco
        imprimir_dire_pagina eax, 00011111b, 18, 20, 21
                shr eax, 12
        imprimir_mapa loqueimprimo, 1, 01000000b, 0, eax
    pop eax
    pop eax
    push 2
    push 2
        call pagina_barco
        imprimir_dire_pagina eax, 00011111b, 18, 34, 22
                shr eax, 12
        imprimir_mapa loqueimprimo, 1, 01000000b, 0, eax
    add ebp, 8
    pop eax
    pop eax

    mov byte [loqueimprimo], 52
    push 0
    push 3
        call pagina_barco
        imprimir_dire_pagina eax, 00011111b, 19, 06, 30
                shr eax, 12
        imprimir_mapa loqueimprimo, 1, 01000000b, 0, eax
    pop eax
    pop eax
    push 1
    push 3
        call pagina_barco
        imprimir_dire_pagina eax, 00011111b, 19, 20, 31
                shr eax, 12
        imprimir_mapa loqueimprimo, 1, 01000000b, 0, eax
    pop eax
    pop eax
    push 2
    push 3
        call pagina_barco
        imprimir_dire_pagina eax, 00011111b, 19, 34, 32
                shr eax, 12
        imprimir_mapa loqueimprimo, 1, 01000000b, 0, eax
    add ebp, 8
    pop eax
    pop eax

    mov byte [loqueimprimo], 53
    push 0
    push 4
        call pagina_barco
        imprimir_dire_pagina eax, 00011111b, 20, 06, 40
                shr eax, 12
        imprimir_mapa loqueimprimo, 1, 01000000b, 0, eax
    pop eax
    pop eax
    push 1
    push 4
        call pagina_barco
        imprimir_dire_pagina eax, 00011111b, 20, 20, 41
                shr eax, 12
        imprimir_mapa loqueimprimo, 1, 01000000b, 0, eax
    pop eax
    pop eax
    push 2
    push 4
        call pagina_barco
        imprimir_dire_pagina eax, 00011111b, 20, 34, 42
                shr eax, 12
        imprimir_mapa loqueimprimo, 1, 01000000b, 0, eax
    add ebp, 8
    pop eax
    pop eax

    mov byte [loqueimprimo], 54
    push 0
    push 5
        call pagina_barco
        imprimir_dire_pagina eax, 00011111b, 21, 06, 50
                shr eax, 12
        imprimir_mapa loqueimprimo, 1, 01000000b, 0, eax
    pop eax
    pop eax
    push 1
    push 5
    call pagina_barco
        imprimir_dire_pagina eax, 00011111b, 21, 20, 51
                shr eax, 12
        imprimir_mapa loqueimprimo, 1, 01000000b, 0, eax
    pop eax
    pop eax
    push 2
    push 5
        call pagina_barco
        imprimir_dire_pagina eax, 00011111b, 21, 34, 52
                shr eax, 12
        imprimir_mapa loqueimprimo, 1, 01000000b, 0, eax
    add ebp, 8
    pop eax
    pop eax

    mov byte [loqueimprimo], 55
    push 0
    push 6
        call pagina_barco
        imprimir_dire_pagina eax, 00011111b, 22, 06, 60
                shr eax, 12
        imprimir_mapa loqueimprimo, 1, 01000000b, 0, eax
    pop eax
    pop eax
    push 1
    push 6
        call pagina_barco
        imprimir_dire_pagina eax, 00011111b, 22, 20, 61
                shr eax, 12
        imprimir_mapa loqueimprimo, 1, 01000000b, 0, eax
    pop eax
    pop eax
    push 2
    push 6
        call pagina_barco
        imprimir_dire_pagina eax, 00011111b, 22, 34, 62
                shr eax, 12
        imprimir_mapa loqueimprimo, 1, 01000000b, 0, eax
    add ebp, 8
    pop eax
    pop eax

    mov byte [loqueimprimo], 56
    push 0
    push 7
        call pagina_barco
        imprimir_dire_pagina eax, 00011111b, 23, 06, 70
                shr eax, 12
        imprimir_mapa loqueimprimo, 1, 01000000b, 0, eax
    pop eax
    pop eax
    push 1
    push 7
        call pagina_barco
        imprimir_dire_pagina eax, 00011111b, 23, 20, 71
                shr eax, 12
        imprimir_mapa loqueimprimo, 1, 01000000b, 0, eax
    pop eax
    pop eax
    push 2
    push 7
        call pagina_barco
        imprimir_dire_pagina eax, 00011111b, 23, 34, 72
                shr eax, 12
        imprimir_mapa loqueimprimo, 1, 01000000b, 0, eax
    add ebp, 8
    pop eax
    pop eax
ret
char_nums: db '012345678'
loqueimprimo: db 01000000b
;==============================================================================
