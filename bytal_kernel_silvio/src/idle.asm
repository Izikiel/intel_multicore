; ==============================================================================
; TRABAJO PRACTICO 3 - System Programming - ORGANIZACION DE COMPUTADOR II - FCEN
; ==============================================================================

ORG 0x40000000

BITS 32

;%include "imprimir.mac"

idle:
    .loopear:
;        inc dword [numero]
;        cmp dword [numero], 0x4
;        jb .imprimir
;
;    .reset_contador:
;        mov dword [numero], 0x0
;
;    .imprimir:
;        ; Imprimir 'reloj'
;        mov ebx, dword [numero]
;        add ebx, message1
;        imprimir_texto_mp ebx, 1, 0xf0, 24, 0
;
    jmp .loopear

;numero:   dd 0x00000000
;
;message1: db '\'
;message2: db '-'
;message3: db '/'
;message4: db '|'
