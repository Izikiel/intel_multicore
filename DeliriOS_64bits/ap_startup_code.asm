;Este codigo es el codigo de inicializacion de los Application Processors.
;Dado que inician en modo real, deben iniciar el procesador desde cero.
BITS 16
%include "macros/asm_screen_utils.mac"
%include "macros/real_mode_macros.mac"

section .apstartsection

;; GDT
extern GDT_DESC

;; IDT
extern IDT_DESC

;; STACK
extern kernelStackPtrAP1; OJO , OJO , OJO CON ESTO, LAS PILAS DEBEN SER DIFERENTES ENTRE LOS CORES!!!

;;paginacion
extern krnPML4T

;; startup
extern startKernel64_APMODE

;; Saltear seccion de datos(para que no se ejecute)
jmp mr_ap_start

;------------------------------------------------------------------------------------------------------
;------------------------------- comienzo modo real ---------------------------------------------------
;------------------------------------------------------------------------------------------------------
mr_ap_start:
    ; Deshabilitar interrupciones
    cli

    ; A20 YA ESTA HABILITADO POR EL BSP    

    ; cargar la GDT; ES UNICA PARA TODOS LOS CORES        
    lgdt [GDT_DESC]

    ; setear el bit PE del registro CR0
    mov EAX, CR0;levanto registro CR0 para pasar a modo protegido
    or EAX, 1;hago un or con una mascara de 0...1 para setear el bit de modo protegido
    mov CR0, EAX

    ; pasar a modo protegido
    jmp 00001000b:protected_mode; saltamos a modo protegido, modificamos el cs con un jump y la eip(program counter)
    ;{index:1 | gdt/ldt: 0 | rpl: 00} => 1000
    ;aca setie el selector de segmento cs al segmento de codigo del kernel

;------------------------------------------------------------------------------------------------------
;------------------------------- comienzo modo protegido ----------------------------------------------
;------------------------------------------------------------------------------------------------------

BITS 32
protected_mode:    
    ;limpio los registros
    xor eax, eax
    xor ebx, ebx
    xor ecx, ecx
    xor edx, edx
    xor esi, esi
    xor edi, edi
    xor ebp, ebp
    xor esp, esp

    ;cargo los selectores de segmento de modo protegido
    xor eax, eax
    mov ax, 00011000b;{index:3 | gdt/ldt: 0 | rpl: 00} segmento de datos de kernel
    mov ds, ax;cargo como selector de segmento de datos al descriptor del indice 2 que corresponde a los datos del kernel
    mov es, ax;cargo tambien estos selectores auxiliares con el descriptor de datos del kernel
    mov fs, ax;cargo tambien estos selectores auxiliares con el descriptor de datos del kernel
    mov gs, ax;cargo tambien estos selectores auxiliares con el descriptor de datos del kernel    
    mov ss, ax;cargo el selector de pila en el segmento de datos del kernel
    mov esp, [kernelStackPtrAP1];la pila va a partir de kernelStackPtrBSP(expand down, OJO)
    mov ebp, esp;pongo base y tope juntos.

    ;Los chequeos de cpuid y disponibilidad de modo x64 ya fueron hechos por el
    ;BSP, de forma que en este punto deberiamos tener x64 en todos los cores
	;Las estructuras de paginacion ya fueron inicializadas por el BSP

	;apuntar cr3 al PML4
    mov eax, [krnPML4T]
    mov cr3, eax

    ;prender el bit 5(6th bit) para activar PAE
    mov eax, cr4                 
    or eax, 1 << 5               
    mov cr4, eax                 

    mov ecx, 0xC0000080          ; Seleccionamos EFER MSR poniendo 0xC0000080 en ECX
    rdmsr                        ; Leemos el registro en EDX:EAX.
    or eax, 1 << 8               ; Seteamos el bit de modo largo que es el noveno bit (contando desde 0) osea el bit 8.
    wrmsr                        ; Escribimos nuevamente al registro.

    mov eax, cr0                 ; Obtenemos el registro de control 0 actual.
    or eax, 1 << 31              ; Habilitamos el bit de Paginacion que es el 32vo bit (contando desde 0) osea el bit 31
    mov cr0, eax                 ; escribimos el nuevo valor sobre el registro de control

    ;estamos en modo ia32e compatibilidad con 32 bits
    ;comienzo pasaje a 64 bits puro

    jmp 00010000b:long_mode; saltamos a modo largo, modificamos el cs con un jump y la eip(program counter)
    ;{index:2 | gdt/ldt: 0 | rpl: 00} => 00010000
    ;aca setie el selector de segmento cs al segmento de codigo del kernel 

;------------------------------------------------------------------------------------------------------
;------------------------------- comienzo modo largo --------------------------------------------------
;------------------------------------------------------------------------------------------------------

BITS 64
long_mode:
    ;limpio los registros
    xor rax, rax
    xor rbx, rbx
    xor rcx, rcx
    xor rdx, rdx
    xor rsi, rsi
    xor rdi, rdi
    xor rbp, rbp
    xor rsp, rsp
    xor r8, r8
    xor r9, r9
    xor r10, r10
    xor r11, r11
    xor r12, r12
    xor r13, r13
    xor r14, r14
    xor r15, r15

    ;levanto segmentos con valores iniciales
    XOR rax, rax
    MOV ax, 00011000b;{index:3 | gdt/ldt: 0 | rpl: 00} segmento de datos de kernel
    MOV ds, ax;cargo como selector de segmento de datos al descriptor del indice 2 que corresponde a los datos del kernel
    MOV es, ax;cargo tambien estos selectores auxiliares con el descriptor de datos del kernel
    MOV fs, ax;cargo tambien estos selectores auxiliares con el descriptor de datos del kernel
    MOV gs, ax;cargo tambien estos selectores auxiliares con el descriptor de datos del kernel    
    
    ;cargo el selector de pila en el segmento de datos del kernel
    MOV ss, ax

    ;setear la pila en para el kernel
    MOV rsp, [kernelStackPtrAP1];la pila va a partir de kernelStackPtrBSP(expand down, OJO)
    MOV rbp, rsp;pongo base y tope juntos.

    ;levanto la IDT de 64 bits, es unica para todos los cores
    lidt [IDT_DESC]
    ;la IDT esta inicializada por el BSP

    ;el controlador de interrupciones ya esta inicializado por el BSP

    ;llamo al entrypoint en kmain64
    call startKernel64_APMODE

    ;fin inicio kernel para AP en 64 bits!
    halt: hlt
        jmp halt

; -------------------------------------------------------------------------- ;;