; ** por compatibilidad se omiten tildes **
; ==============================================================================
; TRABAJO PRACTICO 3 - System Programming - ORGANIZACION DE COMPUTADOR II - FCEN
; ==============================================================================
; rutinas para habilitar y deshabilitar A20

BITS 16

section .text

habilitando: db 'Habilitando A20........'
habilitando_len equ $ - habilitando

deshabilitando: db 'Desabilitando A20......'
deshabilitando_len equ $ - deshabilitando

checkeando: db 'Checkeando A20.........'
checkeando_len equ $ - checkeando

mensajeOK: db 'OK!'
mensajeOK_len equ $ - mensajeOK

mensajeFAIL: db 'FALLO!'
mensajeFAIL_len equ $ - mensajeFAIL

contadorlineas: dw 0x0000

; Imprime un string en la seccion especificada de la memoria de Video.
; Solo funciona en modo Real.
;
; Parametros:
;       %1      Mensaje
;       %2      Longitud
;       %3      Color
;       %4      FILA Si es 0xFFFF, no aumento lineas
;       %5      COLUMNA
%macro IMPRIMIR_MODO_REAL 5
    pusha
    push    es
    mov     ax, 0xB800          ;segmento de video
    mov     es, ax
    %if %4 <> dx
    mov     dx, %4
    %endif
    cmp     dx, 0xFFFF
    je      %%sigo
    add     WORD [contadorlineas], 0x0001
    %%sigo:
    mov     ax, [contadorlineas]
    mov     bx, 80
    mul     bx
    mov     bx, ax
    %if %5 <> dx
    mov     dx, %5  ;offset
    %endif

    add     bx, dx
    shl     bx, 1
    %if %1 <> di
    mov     di, %1          ;di = puntero al mensaje
    %endif
    %if %2 <> cx
    mov     cx, %2          ;cx = contador (longitud del mensaje)
    %endif
    %if %3 <> ah
    mov     ah, %3          ;ah = color. 0x1A azul de fondo, verde brillante para el caracter
    %endif
        %%ciclo_cadena:
        mov     al, [di]            ;al = caracter.
        mov     [es:bx], ax         ;escribo en pantalla
        add     bx, 2
        inc     di
        loop    %%ciclo_cadena

    pop     es
    popa
%endmacro

deshabilitar_A20:
    pushf
    pusha
    IMPRIMIR_MODO_REAL deshabilitando, deshabilitando_len, 0x07, 0, 0
    call    a20wait
    mov     al,0xAD
    out     0x64,al
    call    a20wait
    mov     al,0xD0
    out     0x64,al
    call    a20wait2
    in      al,0x60
    push    ax
    call    a20wait
    mov     al,0xD1
    out     0x64,al
    call    a20wait
    pop     ax
    and     al,0xFD     ;deshabilito
    out     0x60,al
    call    a20wait
    mov     al,0xAE
    out     0x64,al
    call    a20wait
    IMPRIMIR_MODO_REAL mensajeOK, mensajeOK_len, 0x0A, 0xFFFF, 23
    popa
    popf
    ret


habilitar_A20:
    pushf
    pusha
    IMPRIMIR_MODO_REAL habilitando, habilitando_len, 0x07, 0, 0
    call    a20wait
    mov     al,0xAD
    out     0x64,al
    call    a20wait
    mov     al,0xD0
    out     0x64,al
    call    a20wait2
    in      al,0x60
    push    ax
    call    a20wait
    mov     al,0xD1
    out     0x64,al
    call    a20wait
    pop     ax
    or      al,2
    out     0x60,al
    call    a20wait
    mov     al,0xAE
    out     0x64,al
    call    a20wait
    IMPRIMIR_MODO_REAL mensajeOK, mensajeOK_len, 0x0A, 0xFFFF, 23
    popa
    popf
    ret

a20wait:
    in      al,0x64
    test    al,2
    jnz     a20wait
    ret

a20wait2:
    in      al,0x64
    test    al,1
    jz      a20wait2
    ret

checkear_A20:
    pushf
    push fs
    push gs
    push di
    push si
    IMPRIMIR_MODO_REAL checkeando, checkeando_len, 0x07, 0, 0
    xor ax, ax ; ax = 0
    mov fs, ax
    not ax ; ax = 0xFFFF
    mov gs, ax
    mov di, 0x0500
    mov si, 0x0510
    mov al, byte [fs:di]
    push ax
    mov al, byte [gs:si]
    push ax
    mov byte [fs:di], 0x00
    mov byte [gs:si], 0xFF
    cmp byte [fs:di], 0xFF
    je .falla
        IMPRIMIR_MODO_REAL mensajeOK, mensajeOK_len, 0x0A, 0xFFFF, 23
    jmp .sigue
    .falla:
        IMPRIMIR_MODO_REAL mensajeFAIL, mensajeFAIL_len, 0x0C, 0xFFFF, 23
    .sigue:
    pop ax
    mov byte [gs:si], al
    pop ax
    mov byte [fs:di], al
    mov ax, 0
    je check_a20__exit
    mov ax, 1

check_a20__exit:
    pop si
    pop di
    pop gs
    pop fs
    popf
    ret
