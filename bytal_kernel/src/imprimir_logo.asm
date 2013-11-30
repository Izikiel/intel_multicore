
%define breakpoint xchg bx, bx
%include "imprimir.mac"


;===============================================================================
global inicializo_estado
inicializo_estado:
    xor edx, edx
    .ciclo_estadof1:
        imprimir_estado espacio, 1,0x00, 0, edx
        inc edx
        cmp edx, 80
        jne .ciclo_estadof1
    imprimir_estado nombre_grupo, tamanio_nombre, 0x05, 0, 2
    .ciclo_estadof2:
        imprimir_estado espacio, 1, 0xFF, 0, edx
        inc edx
        cmp edx, 1280
        jne .ciclo_estadof2
    .ciclo_estadof3:
        imprimir_estado espacio, 1, 0x10, 0, edx
        inc edx
        cmp edx, 1920
        jne .ciclo_estadof3
    .ciclo_estadof4:
        imprimir_estado espacio, 1, 0x00, 0, edx
        inc edx
        cmp edx, 2000
        jne .ciclo_estadof4

    mov edx, 1
    .ciclo_estadoc1:
        imprimir_estado espacio, 1, 0x00, edx, 0
        imprimir_estado espacio, 1, 0x00, edx, 79
        inc edx
        cmp edx, 25
        jne .ciclo_estadoc1

    mov edx, 16
    .ciclo_estadoc2:
        mov al, [numero]
        inc al
        mov [numero], al
        imprimir_estado numero, 1, 01110000b, edx, 1
        inc edx
        cmp edx, 24
        jne .ciclo_estadoc2
        imprimir_estado navio_l1, tamanio_msgnavio, 0x70, 2, 1
        imprimir_estado navio_l2, tamanio_msgnavio, 0x70, 9, 1

    mov ecx, 2
    .ciclo_estadoc3:
        imprimir_estado espacios28, 28, 00011111b, ecx, 50
        inc ecx
        cmp ecx, 15
        jne .ciclo_estadoc3

    mov ecx, 15
    .ciclo_estadoc4:
        imprimir_estado espacio, 1, 01111111b, ecx, 78
        inc ecx
        cmp ecx, 24
        jne .ciclo_estadoc4


    %define colorregistros 00011111b

%macro iniReg 3
        mov dword [regst], %1
        imprimir_estado regst, 4, colorregistros, %2, %3
        imprimir_estado ochoceros, 8, colorregistros, %2, %3+4
%endmacro
        iniReg "eax ", 02, 51
        iniReg "ebx ", 03, 51
        iniReg "ecx ", 04, 51
        iniReg "edx ", 05, 51
        iniReg "esi ", 06, 51
        iniReg "edi ", 07, 51
        iniReg "ebp ", 08, 51
        iniReg "esp ", 09, 51
        iniReg "eip ", 10, 51
        iniReg "cr0 ", 11, 51
        iniReg "cr2 ", 12, 51
        iniReg "cr3 ", 13, 51
        iniReg "cr4 ", 14, 51
        iniReg " cs ", 02, 64
        iniReg " ds ", 03, 64
        iniReg " es ", 04, 64
        iniReg " fs ", 05, 64
        iniReg " gs ", 06, 64
        iniReg " ss ", 07, 64
        imprimir_estado eflagsstring, 6, colorregistros, 09, 65
        imprimir_estado ochoceros, 8, colorregistros, 10, 68

    mov ebx, 16
    .ciclo_estadop1:
        imprimir_estado pn, 13, colorregistros, ebx, 3
        inc ebx
        cmp ebx, 24
        jne .ciclo_estadop1
    mov al, [pn+1]
    inc al
    mov [pn+1], al
    mov ebx, 16
    .ciclo_estadop2:
        imprimir_estado pn, 13, colorregistros, ebx, 17
        inc ebx
        cmp ebx, 24
        jne .ciclo_estadop2
    mov al, [pn+1]
    inc al
    mov [pn+1], al
    mov ebx, 16
    .ciclo_estadop3:
        imprimir_estado pn, 13, colorregistros, ebx, 31
        inc ebx
        cmp ebx, 24
        jne .ciclo_estadop3
%macro cuadrado 2
    xor eax, eax
    mov ebx, %1
    .ciclo_cuadrado%1%2
        imprimir_estado banda, 10, 0x00, ebx, %2
        inc eax
        inc ebx
        cmp eax, 5
        jne .ciclo_cuadrado%1%2
%endmacro
        cuadrado 3, 2
        cuadrado 3, 14
        cuadrado 3, 26
        cuadrado 3, 38
        cuadrado 10, 2
        cuadrado 10, 14
        cuadrado 10, 26
        cuadrado 10, 38
ret
banda: db "          "
nombre_grupo: db "Sambayon/Freddo"
tamanio_nombre: equ $ - nombre_grupo
navio_l1: db "    NAVIO 1     NAVIO 2     NAVIO 3     NAVIO 4"
navio_l2: db "    NAVIO 5     NAVIO 6     NAVIO 7     NAVIO 8"
tamanio_msgnavio: equ $ - navio_l2
numero: db "0"
espacios28: db "                            "
ochoceros: db "00000000"
regst: dd "000 "
eflagsstring: db "eflags"
pn: db "P0:00000000  "
;===============================================================================


;===============================================================================
global inicializo_mapa
%define color_tierra 00101010b
%define color_mar 00011001b
%define color_tarea 01001111b
inicializo_mapa:
 xor ecx, ecx
    .ciclo_imtierra:
        imprimir_mapa cosito_tierra, 1, color_tierra, 0, ecx
         inc ecx
         cmp ecx, 256
         jne .ciclo_imtierra
    .ciclo_immar:
        imprimir_mapa cosito_mar, 1, color_mar, 0, ecx
         inc ecx
         cmp ecx, 1920
     jne .ciclo_immar
    .ciclo_imlimpio:
        imprimir_mapa espacio, 1, 0x00, 0, ecx
         inc ecx
         cmp ecx, 2000
     jne .ciclo_imlimpio
ret
cosito_tierra: db 176
cosito_mar: db 178
espacio: db " "
;===============================================================================



;===============================================================================
;   aca imprimo la pantallita linda
global ImprimirLogo

ImprimirLogo:

    mov ax, 0xb0 ; 22<<3 segmento de video
    mov fs, ax

    mov ebx, 0
    xor ecx, ecx
    mov ah, 00001111b
    mov al, 32
    ciclol0:
        mov [fs:ebx], ax
        add ebx, 2
        inc ecx
        cmp ecx, 240
        jne ciclol0

    xor ecx, ecx
    mov al, 220
    ciclol1:
        mov [fs:ebx], ax
        add ebx, 2
        inc ecx
        cmp ecx, 80
        jne ciclol1

    xor ecx, ecx
    cicloc0:
        mov al, 32
        mov [fs:ebx], ax
        add ebx, 158
        mov [fs:ebx], ax
        add ebx, 2*2
        inc ecx
        cmp ecx, 18
        jne cicloc0

    xor ecx, ecx
    mov ebx, 2084
    mov ah, 01001111b
    mov al, 220
    ciclol2:
        mov [fs:ebx], ax
        add ebx, 2
        inc ecx
        cmp ecx, 76
        jne ciclol2

    xor ecx, ecx
    mov ebx, 642
    cicloc1:
        mov al, 219
        mov [fs:ebx], ax
        add ebx, 154
        mov [fs:ebx], ax
        add ebx, 2*3
        inc ecx
        cmp ecx, 19
        jne cicloc1

    mov ebx, 3524
    mov ah, 00011111b
    mov al, 220
    mov [fs:ebx], ax
    add ebx, 150
    mov [fs:ebx], ax

    xor ecx, ecx
    mov ebx, 3684
    mov ah, 00001111b
    mov al, 223
    ciclol3:
        mov [fs:ebx], ax
        add ebx, 2
        inc ecx
        cmp ecx, 35
        jne ciclol3
    mov ah, 00011111b
    mov al, 219
    mov [fs:ebx], ax
    add ebx, 2
    mov al, 220
    mov [fs:ebx], ax
    add ebx, 2
    mov ah, 00011001b
    mov al, 176
    mov [fs:ebx], ax
    add ebx, 2
    mov [fs:ebx], ax
    add ebx, 2
    mov ah, 00011111b
    mov al, 220
    mov [fs:ebx], ax
    add ebx, 2
    mov al, 219
    mov [fs:ebx], ax
    add ebx, 2
    xor ecx, ecx
    mov ah, 00001111b
    mov al, 223
    ciclol4:
        mov [fs:ebx], ax
        add ebx, 2
        inc ecx
        cmp ecx, 35
        jne ciclol4

    mov ebx, 3916
    mov ah, 00001111b
    mov al, 223
    mov [fs:ebx], ax
    add ebx, 2
    mov al, 219
    mov [fs:ebx], ax
    add ebx, 2
    mov [fs:ebx], ax
    add ebx, 2
    mov al, 223
    mov [fs:ebx], ax
    add ebx, 2

    xor ecx, ecx
    mov ah, 01001100b
    mov al, 176
    mov ebx, 644
    ciclor1:
        xor edx, edx
        ciclor2:
            mov [fs:ebx], ax
            add ebx, 2
            inc edx
            cmp edx, 76
            jne ciclor2
        add ebx, 8
        inc ecx
        cmp ecx, 9
        jne ciclor1

    xor ecx, ecx
    mov ah, 00011001b
    mov al, 176
    mov ebx, 2244
    ciclor3:
        xor edx, edx
        ciclor4:
            mov [fs:ebx], ax
            add ebx, 2
            inc edx
            cmp edx, 76
            jne ciclor4
        add ebx, 8
        inc ecx
        cmp ecx, 8
        jne ciclor3

    add ebx, 2
    xor ecx, ecx
    ciclor5:
        mov [fs:ebx], ax
        add ebx, 2
        inc ecx
        cmp ecx, 74
        jne ciclor5

    mov ebx, 2408
    mov ah, 00001111b
    mov al, 219
    letras1:
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*6
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*4
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*6
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*3
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*3
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax

    mov ebx, 2568
    letras2:
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*5
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*4
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*3
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*5
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*6
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*4
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*6
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*6
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*3
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*3
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*3
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*3
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*3
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax


    mov ebx, 2728
    letras3:
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*5
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*3
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*5
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*4
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*6
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*4
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*6
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*6
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*6
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*3
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*3
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*3
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*3
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax

    mov ebx, 2888
    letras4:
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*4
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*4
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*6
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*4
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*6
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*5
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*3
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*3
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*3
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax


    mov ebx, 3048
    letras5:
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*5
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*3
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*5
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*4
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*6
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*4
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*6
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*10
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*3
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*3
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*3
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax

    mov ebx, 3208
    letras6:
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*5
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*3
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*5
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*4
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*6
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*4
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*6
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*6
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*3
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*3
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*3
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*3
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax

    mov ebx, 3368
    letras7:
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*4
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*5
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*4
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*6
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*4
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*3
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*3
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*3
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2*2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax

    barquito:
        mov ah, 01000000b

        mov ebx, 1240
        mov al, 219
        mov [fs:ebx], ax

        mov ebx, 1398
        mov al, 219
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 10
        mov [fs:ebx], ax
        sub ebx, 2
        mov al, 220
        mov [fs:ebx], ax

        mov ebx, 1528
        mov al, 223
        mov [fs:ebx], ax
        add ebx, 2
        mov al, 219
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov al, 220
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov al, 219
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov al, 220
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov al, 219
        mov [fs:ebx], ax
        add ebx, 2

        mov ebx, 1694
        mov al, 223
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2
        mov [fs:ebx], ax
        add ebx, 2

    xor ax, ax
    mov fs, ax
ret