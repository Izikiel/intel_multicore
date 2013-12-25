%include "macros/asm_screen_utils.mac"
%include "macros/real_mode_macros.mac"

global kernel

;; GDT
extern GDT_DESC

;; IDT
extern IDT_DESC
extern idt_inicializar

;; STACK
extern kernelStackPtr

;; PIC
extern deshabilitar_pic
extern resetear_pic
extern habilitar_pic

;;paginacion
extern krnPML4T

;; startup
extern startKernel64

;; Saltear seccion de datos(para que no se ejecute)
BITS 16
JMP kernel

;;
;; Seccion de datos
;; -------------------------------------------------------------------------- ;;
mensaje_inicioprot_msg:     db 'Starting up in protected mode...'
mensaje_inicioprot_len      equ $ - mensaje_inicioprot_msg

mensaje_inicio64_msg:     db 'Starting up in long mode(IA32e compatibility mode)...'
mensaje_inicio64_len      equ $ - mensaje_inicio64_msg

mensaje_inicio64real_msg:     db 'Starting up in full long mode...'
mensaje_inicio64real_len      equ $ - mensaje_inicio64real_msg

mensaje_64bitserr_msg:     db 'FAIL! 64 bits mode unavailable! -> Kernel Halted.'
mensaje_64bitserr_len      equ $ - mensaje_64bitserr_msg

mensaje_cpuiderr_msg:     db 'FAIL! CPUID unavailable! -> Kernel Halted.'
mensaje_cpuiderr_len        equ $ - mensaje_cpuiderr_msg

mensaje_ok_msg:             db 'OK!'
mensaje_ok_len              equ $ - mensaje_ok_msg

;;
;; Seccion de código.
;; -------------------------------------------------------------------------- ;;

;------------------------------------------------------------------------------------------------------
;------------------------------- comienzo modo real ---------------------------------------------------
;------------------------------------------------------------------------------------------------------
BITS 16
kernel:
    ; Deshabilitar interrupciones
    cli

    ; habilitar A20    
    call habilitar_A20

    ;desaparecer cursor en pantalla
    mov BL, 0
    dec BL
    mov BH, 0
    dec BH
    set_cursor

    ; cargar la GDT        
    lgdt [GDT_DESC]

    imprimir_texto_mr mensaje_inicioprot_msg, mensaje_inicioprot_len, 0x0F, 0, 320

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
    ;cargo los selectores de segmento de modo protegido
    xor eax, eax
    mov ax, 00011000b;{index:3 | gdt/ldt: 0 | rpl: 00} segmento de datos de kernel
    mov ds, ax;cargo como selector de segmento de datos al descriptor del indice 2 que corresponde a los datos del kernel
    mov es, ax;cargo tambien estos selectores auxiliares con el descriptor de datos del kernel
    mov fs, ax;cargo tambien estos selectores auxiliares con el descriptor de datos del kernel
    mov gs, ax;cargo tambien estos selectores auxiliares con el descriptor de datos del kernel    
    mov ss, ax;cargo el selector de pila en el segmento de datos del kernel
    ;setear la pila en 0x27000 para el kernel
    mov esp, [kernelStackPtr];la pila va a partir de kernelStackPtr(expand down, OJO)
    mov ebp, esp;pongo base y tope juntos.

    imprimir_texto_mp mensaje_ok_msg, mensaje_ok_len, 0x02, 2, mensaje_inicioprot_len

    imprimir_texto_mp mensaje_inicio64_msg, mensaje_inicio64_len, 0x0F, 3, 0    

    ; Chequeo de disponibilidad de uso de CPUID

    pushfd               ; Store the FLAGS-register.
    pop eax              ; Restore the A-register.
    mov ecx, eax         ; Set the C-register to the A-register.
    xor eax, 1 << 21     ; Flip the ID-bit, which is bit 21.
    push eax             ; Store the A-register.
    popfd                ; Restore the FLAGS-register.
    pushfd               ; Store the FLAGS-register.
    pop eax              ; Restore the A-register.
    push ecx             ; Store the C-register.
    popfd                ; Restore the FLAGS-register.
    xor eax, ecx         ; Do a XOR-operation on the A-register and the C-register.
    jz CPUIDNoDisponible; The zero flag is set, no CPUID.
    ; here CPUID is available for use.

    ;Deteccion de modo 64 bits y mensaje de error sino esta disponible halteamos.
    mov eax, 0x80000000    ; pasamos parametros 0x80000000.
    cpuid                  
    cmp eax, 0x80000001    ; 0x80000001 significa que esta habilitado.
    jb ModoLargoNoDisp    ; si es menor, modo largo no esta disponible

    ;aca tenemos certeza de que tenemos modo de 64 bits disponible

                ;------------------ Hardcode --------------------------

;;PML4T - 0x40000.
;;PDPT - 0x41000.
;;PDT - 0x42000.
;;PT - 0x43000.

; Clear memory for the Page Descriptor Entries (0x10000 - 0x4FFFF)
;    mov edi, 0x00010000
;    mov ecx, 65536
;    rep stosd

; Create the Level 4 Page Map. (Maps 4GBs of 2MB pages)
; First create a PML4 entry.
; PML4 is stored at 0x0000000000040000, create the first entry there
; A single PML4 entry can map 512GB with 2MB pages.
    cld
    mov edi, 0x00040000     ; Create a PML4 entry for the first 4GB of RAM
    mov eax, 0x00041007
    stosd
    xor eax, eax
    stosd

; Create the PDP entries.
; The first PDP is stored at 0x0000000000041000, create the first entries there
; A single PDP entry can map 1GB with 2MB pages
    mov ecx, 64         ; number of PDPE's to make.. each PDPE maps 1GB of physical memory
    mov edi, 0x00041000
    mov eax, 0x00050007     ; location of first PD
create_pdpe:
    stosd
    push eax
    xor eax, eax
    stosd
    pop eax
    add eax, 0x00001000     ; 4K later (512 records x 8 bytes)
    dec ecx
    cmp ecx, 0
    jne create_pdpe

; Create the PD entries.
; PD entries are stored starting at 0x0000000000050000 and ending at 0x000000000009FFFF (256 KiB)
; This gives us room to map 64 GiB with 2 MiB pages
    mov edi, 0x00050000
    mov eax, 0x0000008F     ; Bit 7 must be set to 1 as we have 2 MiB pages
    xor ecx, ecx
pd_again:               ; Create a 2 MiB page
    stosd
    push eax
    xor eax, eax
    stosd
    pop eax
    add eax, 0x00200000
    inc ecx
    cmp ecx, 2048
    jne pd_again            ; Create 2048 2 MiB page maps.

; Point cr3 at PML4
    mov eax, 0x00040000     ; Write-thru (Bit 3)
    mov cr3, eax

    mov eax, cr4                 ; Set the A-register to control register 4.
    or eax, 1 << 5               ; Set the PAE-bit, which is the 6th bit (bit 5).
    mov cr4, eax                 ; Set control register 4 to the A-register.
    

                                ;--------------- Fin Hardcode --------------------------

;fin activacion PAE y mapeo!, todo listo para levantar 64 bits modo compatibilidad con 32 bits!

    mov ecx, 0xC0000080          ; Seleccionamos EFER MSR poniendo 0xC0000080 en ECX
    rdmsr                        ; Leemos el registro en EDX:EAX.
    or eax, 1 << 8               ; Seteamos el bit de modo largo que es el noveno bit (contando desde 0) osea el bit 8.
    wrmsr                        ; Escribimos nuevamente al registro.

    mov eax, cr0                 ; Obtenemos el registro de control 0 actual.
    or eax, 1 << 31              ; Habilitamos el bit de Paginacion que es el 32vo bit (contando desde 0) osea el bit 31
    mov cr0, eax                 ; escribimos el nuevo valor sobre el registro de control

    imprimir_texto_mp mensaje_ok_msg, mensaje_ok_len, 0x02, 3, mensaje_inicio64_len

    imprimir_texto_mp mensaje_inicio64real_msg, mensaje_inicio64real_len, 0x0F, 4, 0
    
    ;estamos en modo ia32e compatibilidad con 32 bits
    ;comienzo pasaje a 64 bits puro

    jmp 00010000b:long_mode; saltamos a modo largo, modificamos el cs con un jump y la eip(program counter)
    ;{index:2 | gdt/ldt: 0 | rpl: 00} => 00010000
    ;aca setie el selector de segmento cs al segmento de codigo del kernel 

; Funciones auxiliares en 32 bits!
CPUIDNoDisponible:
imprimir_texto_mp mensaje_cpuiderr_msg, mensaje_cpuiderr_len, 0x0C, 3, mensaje_inicio64_len
    
    cli
    hlt
    jmp CPUIDNoDisponible

ModoLargoNoDisp:    
    imprimir_texto_mp mensaje_64bitserr_msg, mensaje_64bitserr_len, 0x0C, 3, mensaje_inicio64_len
    
    cli
    hlt
    jmp ModoLargoNoDisp


;------------------------------------------------------------------------------------------------------
;------------------------------- comienzo modo largo --------------------------------------------------
;------------------------------------------------------------------------------------------------------

BITS 64
long_mode:
    ;levanto segmentos con valores iniciales
    XOR eax, eax
    MOV ax, 00011000b;{index:3 | gdt/ldt: 0 | rpl: 00} segmento de datos de kernel
    MOV ds, ax;cargo como selector de segmento de datos al descriptor del indice 2 que corresponde a los datos del kernel
    MOV es, ax;cargo tambien estos selectores auxiliares con el descriptor de datos del kernel
    MOV fs, ax;cargo tambien estos selectores auxiliares con el descriptor de datos del kernel
    MOV gs, ax;cargo tambien estos selectores auxiliares con el descriptor de datos del kernel    
    
    ;cargo el selector de pila en el segmento de datos del kernel
    MOV ss, ax

    ;setear la pila en para el kernel
    MOV rsp, [kernelStackPtr];la pila va a partir de kernelStackPtr(expand down, OJO)
    MOV rbp, rsp;pongo base y tope juntos.
    
    imprimir_texto_ml mensaje_ok_msg, mensaje_ok_len, 0x02, 4, mensaje_inicio64real_len

    ;levanto la IDT de 64 bits
    lidt [IDT_DESC]
    call idt_inicializar

    ;configurar controlador de interrupciones
    CALL deshabilitar_pic
    CALL resetear_pic
    CALL habilitar_pic  

    ;habilito las interrupciones! :D
    STI

    ;llamo al entrypoint en kmain64
    call startKernel64

    ;fin inicio kernel en 64 bits!
    halt: hlt
        jmp halt

;; -------------------------------------------------------------------------- ;;

%include "a20.asm"
