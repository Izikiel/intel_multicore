%include "imprimir.mac"
%include "disableCursor.asm"

global start

;; GDT
extern GDT_DESC

;; IDT
extern IDT_DESC
extern idt_inicializar

;; PIC
extern deshabilitar_pic
extern resetear_pic
extern habilitar_pic

;;paginacion
extern krnPageDir
extern mmu_inicializar_dir_kernel

;; Saltear seccion de datos(para que no se ejecute supongo)
JMP start

;;
;; Seccion de datos.
;; -------------------------------------------------------------------------- ;;
mensaje_inicio_msg:     db 'Starting up in protected mode...'
mensaje_inicio_len      equ $ - mensaje_inicio_msg

;;
;; Seccion de código.
;; -------------------------------------------------------------------------- ;;

;; Punto de entrada del kernel.
BITS 16
start:
    ; Deshabilitar interrupciones
    CLI

    ; habilitar A20    
    call habilitar_A20

    ;desaparecer cursor en pantalla
    mov BL, 0
    DEC BL
    mov BH, 0
    DEC BH
    set_cursor

    ; cargar la GDT    
    LGDT [GDT_DESC];cargo posicion de la gdt en el registro

    ; setear el bit PE del registro CR0
    MOV EAX, CR0;levanto registro CR0 para pasar a modo protegido
    OR EAX, 1;hago un or con una mascara de 0...1 para setear el bit de modo protegido
    MOV CR0, EAX
    ; pasar a modo protegido

    JMP 0x08:protected_mode; saltamos a modo protegido, modificamos el cs con un jump y la eip(program counter)
    ;{index:1 | gdt/ldt: 0 | rpl: 00} => 1000
    ;aca setie el selector de segmento cs al segmento de codigo del kernel

BITS 32;modo de programacion en 32 bits(compila en 32 bits)
protected_mode:    
    ;cargo los selectores de segmento de modo protegido
    XOR eax, eax
    MOV ax, 00010000b;{index:2 | gdt/ldt: 0 | rpl: 00} segmento de datos de kernel
    MOV ds, ax;cargo como selector de segmento de datos al descriptor del indice 2 que corresponde a los datos del kernel
    MOV es, ax;cargo tambien estos selectores auxiliares con el descriptor de datos del kernel
    MOV fs, ax;cargo tambien estos selectores auxiliares con el descriptor de datos del kernel
    MOV gs, ax;cargo tambien estos selectores auxiliares con el descriptor de datos del kernel    
    MOV ss, ax;cargo el selector de pila en el segmento de datos del kernel
    ;setear la pila en 0x27000 para el kernel
    MOV esp, 0x27000;la pila va a partir de 0x27000(expand down, OJO)
    MOV ebp, esp;pongo base y tope juntos.

    imprimir_texto_mp mensaje_inicio_msg, mensaje_inicio_len, 0x07, 2, 0

    ; inicializar la IDT para manejar solo excepciones por ahora
    CALL idt_inicializar
    ;poner en ldtr
    LIDT [IDT_DESC]

    ; inicializar el directorio de paginas de kernel
    CALL mmu_inicializar_dir_kernel
    ; habilitar paginacion
    MOV EAX, [krnPageDir];cargar directorio de paginas del kernel
    MOV CR3, EAX
    MOV EAX, CR0;habilitar bit de paginacion
    OR EAX, 0x80000000
    MOV CR0, EAX

    ;configurar controlador de interrupciones enmascarables(teclado, reloj, etc)
    CALL deshabilitar_pic
    CALL resetear_pic
    CALL habilitar_pic    

    ;habilito las interrupciones! :D
    STI

    JMP $;fin de inicio de kernel

;; -------------------------------------------------------------------------- ;;

%include "a20.asm"
