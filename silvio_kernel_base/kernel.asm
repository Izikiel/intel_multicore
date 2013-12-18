%include "imprimir.mac"
%include "disableCursor.asm"

global start

;; GDT
extern GDT_DESC
extern cambiarSegmentosA64Bits

;; STACK

extern kernelStackPtr

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

    imprimir_texto_mr mensaje_inicioprot_msg, mensaje_inicioprot_len, 0x07, 0, 320

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
    MOV esp, [kernelStackPtr];la pila va a partir de kernelStackPtr(expand down, OJO)
    MOV ebp, esp;pongo base y tope juntos.

    imprimir_texto_mp mensaje_ok_msg, mensaje_ok_len, 0x0A, 2, mensaje_inicioprot_len

    ; inicializar la IDT para manejar solo excepciones por ahora
    CALL idt_inicializar
    ;poner en ldtr
    LIDT [IDT_DESC]

    imprimir_texto_mp mensaje_inicio64_msg, mensaje_inicio64_len, 0x07, 3, 0    

    ;Chequeo de disponibilidad de uso de CPUID

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
    jz .NoCPUID          ; The zero flag is set, no CPUID.
    ; CPUID is available for use.

    ;Hay que chequear virtualizacion por hardware, sino crashea en mi notebook por ejemplo(Intel T4400)
    ;el SO es 64 bits pero bochs no tiene virtualizacion por hardware y no puede emular long mode
    ;esto esta en el bit 5 de cpuid en ECX

    ;Deteccion de modo 64 bits y mensaje de error sino esta disponible halteamos.
    mov eax, 0x80000000    ; Set the A-register to 0x80000000.
    cpuid                  ; CPU identification.
    cmp eax, 0x80000001    ; Compare the A-register with 0x80000001.
    jb .NoLongMode         ; It is less, there is no long mode.

    ;aca tenemos certeza de que tenemos modo de 64 bits disponible
    ;pasaje a 64 bits!

;mapeamos los primeros 2 megas con id mapping y PAE.

;PML4T - 0x40000.
;PDPT - 0x41000.
;PDT - 0x42000.
;PT - 0x43000.

    mov edi, 0x40000    ; Set the destination index to 0x40000.
    mov cr3, edi       ; Set control register 3 to the destination index.
    xor eax, eax       ; Nullify the A-register.
    mov ecx, 4096      ; Set the C-register to 4096.
    rep stosd          ; Clear the memory.
    mov edi, cr3       ; Set the destination index to control register 3.

    mov DWORD [edi], 0x41003     ; Set the double word at the destination index to 0x41003.
    add edi, 0x1000              ; Add 0x1000 to the destination index.
    mov DWORD [edi], 0x42003     ; Set the double word at the destination index to 0x42003.
    add edi, 0x1000              ; Add 0x1000 to the destination index.
    mov DWORD [edi], 0x43003     ; Set the double word at the destination index to 0x43003.
    add edi, 0x1000              ; Add 0x1000 to the destination index.

    mov ebx, 0x00000003          ; Set the B-register to 0x00000003.
    mov ecx, 512                 ; Set the C-register to 512.
     
    .SetEntry:
        mov DWORD [edi], ebx     ; Set the double word at the destination index to the B-register.
        add ebx, 0x1000          ; Add 0x1000 to the B-register.
        add edi, 8               ; Add eight to the destination index.
    loop .SetEntry               ; Set the next entry.

    mov eax, cr4                 ; Set the A-register to control register 4.
    or eax, 1 << 5               ; Set the PAE-bit, which is the 6th bit (bit 5).
    mov cr4, eax                 ; Set control register 4 to the A-register.

;fin activacion PAE y mapeo!, todo listo para levantar 64 bits modo compatibilidad con 32 bits!

    mov ecx, 0xC0000080          ; Set the C-register to 0xC0000080, which is the EFER MSR.
    rdmsr                        ; Read from the model-specific register.
    or eax, 1 << 8               ; Set the LM-bit which is the 9th bit (bit 8).
    wrmsr                        ; Write to the model-specific register.

    mov eax, cr0                 ; Set the A-register to control register 0.
    or eax, 1 << 31              ; Set the PG-bit, which is the 32nd bit (bit 31).
    mov cr0, eax                 ; Set control register 0 to the A-register.

    imprimir_texto_mp mensaje_ok_msg, mensaje_ok_len, 0x0A, 3, mensaje_inicio64_len

    imprimir_texto_mp mensaje_inicio64real_msg, mensaje_inicio64real_len, 0x07, 4, 0
    ;comienzo pasaje a 64 bits nativo

;    ;habilito los bits l de los segmentos de la GDT
;    call cambiarSegmentosA64Bits
;
;    JMP 0x08:long_mode; saltamos a modo largo, modificamos el cs con un jump y la eip(program counter)
;    ;{index:1 | gdt/ldt: 0 | rpl: 00} => 1000
;    ;aca setie el selector de segmento cs al segmento de codigo del kernel 

    HLT; DESCOMENTAR ESTO SI HACEN EL JMP A 64 BITS

.NoCPUID:
imprimir_texto_mp mensaje_cpuiderr_msg, mensaje_cpuiderr_len, 0x0C, 3, mensaje_inicio64_len
    
    CLI
    HLT
    jmp .NoCPUID

.NoLongMode:    
    imprimir_texto_mp mensaje_64bitserr_msg, mensaje_64bitserr_len, 0x0C, 3, mensaje_inicio64_len
    
    CLI
    HLT
    jmp .NoLongMode

;BITS 64
; 
;long_mode:
;    cli                           ; Clear the interrupt flag.                   OJO QUE NECESITO UNA IDT DE 64 BITS!!!    
;
;
;XOR eax, eax
;    MOV ax, 00010000b;{index:2 | gdt/ldt: 0 | rpl: 00} segmento de datos de kernel
;    MOV ds, ax;cargo como selector de segmento de datos al descriptor del indice 2 que corresponde a los datos del kernel
;    MOV es, ax;cargo tambien estos selectores auxiliares con el descriptor de datos del kernel
;    MOV fs, ax;cargo tambien estos selectores auxiliares con el descriptor de datos del kernel
;    MOV gs, ax;cargo tambien estos selectores auxiliares con el descriptor de datos del kernel    
;    
;
;    MOV ss, ax;cargo el selector de pila en el segmento de datos del kernel
;
;
;    ;setear la pila en 0x27000 para el kernel
;    MOV esp, [kernelStackPtr];la pila va a partir de kernelStackPtr(expand down, OJO)
;    MOV ebp, esp;pongo base y tope juntos.
;
;
;    mov ax, 00010000b             ;{index:2 | gdt/ldt: 0 | rpl: 00} segmento de datos de kernel - Set the A-register to the data descriptor.
;    mov ds, ax                    ; Set the data segment to the A-register.
;    mov es, ax                    ; Set the extra segment to the A-register.
;    mov fs, ax                    ; Set the F-segment to the A-register.
;    mov gs, ax                    ; Set the G-segment to the A-register.
;    
;    mov rax, 0x1F201F201F201F20   ; Set the A-register to 0x1F201F201F201F20.
;    mov ecx, 500                  ; Set the C-register to 500.
;
;    xchg bx, bx

;    hlt                           ; Halt the processor.
;
    ;fin inicio kernel!
    



;    ;configurar controlador de interrupciones enmascarables(teclado, reloj, etc)
;    CALL deshabilitar_pic
;    CALL resetear_pic
;    CALL habilitar_pic    
;
;    ;habilito las interrupciones! :D
;    STI

    ; inicializar el directorio de paginas de kernel
;    CALL mmu_inicializar_dir_kernel
;    ; habilitar paginacion
;    MOV EAX, [krnPageDir];cargar directorio de paginas del kernel
;    MOV CR3, EAX
;    MOV EAX, CR0;habilitar bit de paginacion
;    OR EAX, 0x80000000
;    MOV CR0, EAX

;; -------------------------------------------------------------------------- ;;

%include "a20.asm"
