; ** por compatibilidad se omiten tildes **
; ==============================================================================
; TRABAJO PRACTICO 3 - System Programming - ORGANIZACION DE COMPUTADOR II - FCEN
; ==============================================================================

%include "imprimir.mac"
%include "disableCursor.asm"

global start

;; GDT
extern GDT_DESC
extern inicializar_gdt_tareas

;; TSS
extern tss_inicializar

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
extern mmu_inicializar_dir_tareas

;; pantalla
extern inicializarBuffersMapa
extern dibujarBarraRelojes
extern pintarPantallaEstado
extern pintarPantallaMapa
extern notificarCambioPantalla

;; sched
extern inicializar_sched
extern jmpToTask

;; Saltear seccion de datos(para que no se ejecute supongo)
JMP start

;;
;; Seccion de datos.
;; -------------------------------------------------------------------------- ;;
mensaje_inicio_msg:     db 'How is the kid doing? -The kid is fine. Presionar M/E para ver las pantallas'
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
    JMP 0x90:protected_mode; saltamos a modo protegido, modificamos el cs con un jump y la eip(program counter)
    ;0x90 = 0001 0010 bin , notar que es la forma en la que indexo la gdt(los primeros 3 bits son flags)
    ;esto lo calcule como indice 18 => 12 en hexa, 0x12 << 3 por los 3 bits de flags => 0x90 en hexa
    ;{index:18 | gdt/ldt: 0 | rpl: 00}
    ;aca setie el selector de segmento cs al segmento de codigo del kernel

BITS 32;modo de programacion en 32 bits(compila en 32 bits)
protected_mode:
    ;cargo los selectores de segmento
    XOR eax, eax
    MOV ax, 10110000b;{index:22 | gdt/ldt: 0 | rpl: 00} 
    MOV fs, ax;utilizamos el selector de segmento de indice 22 para usar memoria de video desde el kernel
    MOV ax, 10011000b;{index:19 | gdt/ldt: 0 | rpl: 00}
    MOV ds, ax;cargo como selector de segmento de datos al descriptor del indice 19 que corresponde a los datos del kernel
    MOV es, ax;cargo tambien estos selectores auxiliares con el descriptor de datos del kernel
    MOV gs, ax;cargo tambien estos selectores auxiliares con el descriptor de datos del kernel
    ;MOV ax, 10111000b;{index:23 | gdt/ldt: 0 | rpl: 00};aca me habia armado un descriptor para la pila del kernel pero alto yao, ver sig linea
    MOV ss, ax;cargo el selector de pila en el segmento de datos del kernel
    ; setear la pila en 0x27000 para el kernel
    MOV esp, 0x27000;la pila va a partir de 0x27000
    MOV ebp, esp

    imprimir_texto_mp mensaje_inicio_msg, mensaje_inicio_len, 0x4F, 24, 0   

    ; inicializar la IDT para manejar solo excepciones por ahora
    CALL idt_inicializar
    ;poner en ldti
    LIDT [IDT_DESC]

    ; inicializar el directorio de paginas
    CALL mmu_inicializar_dir_kernel
    ; habilitar paginacion
    MOV EAX, [krnPageDir];cargar directorio de paginas del kernel
    MOV CR3, EAX
    MOV EAX, CR0;habilitar bit de paginacion
    OR EAX, 0x80000000
    MOV CR0, EAX

    ;ejercicio 1d: usar la memoria de video desde el segmento de kernel de la gtd, cargado en fs, las rutinas estan en imprimir.mac:
    ;limpiar_pantalla_mp
    ;pintar_primera_ultima_linea_mp

    ;inicializar interfaz grafica
    CALL inicializarBuffersMapa

    ;crear interfaz grafica inicial con datos dummy
    call pintarPantallaEstado
    call pintarPantallaMapa
    call dibujarBarraRelojes

    ;inicializar memoria de tareas, paginacion, etc
    CALL mmu_inicializar_dir_tareas

    ;configurar controlador de interrupciones
    CALL deshabilitar_pic
    CALL resetear_pic
    CALL habilitar_pic    

    ; inicializar todas las tsss
    call tss_inicializar

    ; inicializar entradas de la gdt de las tsss
    call inicializar_gdt_tareas

    ; inicializar tarea inicial -> pongo esto para que en un cambio de tarea, se guarde el contexto de kernel sobre tarea_inicial
    mov ax, 0xB8 ; esto es 23d = 17h => 17h << 3 = B8h
    ltr ax

    ; inicializar el scheduler
    call inicializar_sched

    ;voy a la pantalla estado para mostrar algo lindo inicialmente
    ;void notificarCambioPantalla(int screenIdSelected)
    push 1;screenId 1
    call notificarCambioPantalla
    add esp, 4; desapilo parametro

    ;habilito las interrupciones! :D
    ;las habilito aca porque sino, puede hacer una int de reloj que arranca el sched y todavia no termine de inicializar el kernel. UN HORROR. alta race condition papurri.
    ;asumo que un push y el call salen como piña en un tiempo de reloj
    STI
    
    ;pasar a idle(por enunciado)
    push 0xC0;(GDT_IDX_IDLE_TASK_DESC << 3 /*RPL 0*/)
    call jmpToTask; pasar a la idle
    add esp, 4

    ;en el reloj se schedulean las tareas.!

    JMP $; termino de iniciar el kernel

;; -------------------------------------------------------------------------- ;;

%include "a20.asm"
