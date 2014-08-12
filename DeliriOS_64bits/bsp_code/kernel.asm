%include "../macros/asm_screen_utils.mac"

global protected_mode

;; GDT
extern GDT_DESC

;; IDT
extern IDT_DESC
extern idt_inicializar

;; STACK
extern kernelStackPtrBSP
extern core_stack_ptrs

;; PIC
extern deshabilitar_pic
extern resetear_pic
extern habilitar_pic

;;paginacion
extern krnPML4T
extern krnPDPT
extern krnPDT
extern krnPTT

;; consola
extern console_setYCursor
extern console_setXCursor

;; startup
extern startKernel64_BSPMODE
extern initialize_timer
extern multiprocessor_init

;; tests
extern test_1_core
extern test_2_cores
extern test_ipi_cores
extern test_sum_vector1
extern test_sum_vector2
extern test_mem_sync
extern test_sync_ipi_cores
;test basico para ver q andan las ipis
extern make_ap_jump


extern sin

;test fft
extern test_fft_mono
extern test_fft_dual_mem
extern test_fft_dual_ipi
extern test_half_fft

;Screen
extern clear_screen

;Ap stage2
global apStartupPtr

;grub info
global grubInfoStruct

%macro get_lapic_id32 0  ; para distinguir los procesadores entre si
    ;xor rax, rax ; por si las moscas
    ;mov eax, 0xb ; Manual de intel capitulo 8.4.5 Read 32-bit APIC ID from CPUID leaf 0BH
    ;cpuid
    ;mov eax, edx ; esta version describe topologia, capaz varia con una maquina real
    xor eax, eax
    mov eax, 0xfee00020
    mov eax, [eax]
    shr eax, 24
    and eax, 0xFF
%endmacro

%macro get_lapic_id 0
    xor rax, rax
    mov eax, 0xfee00020
    mov eax, [eax]
    shr eax, 24
    and eax, 0xFF
%endmacro
;; Saltear seccion de datos(para que no se ejecute)

%define breakpoint xchg bx, bx
%define sleep_ap 0x200080 ;; definido en defines.h tambien
%define static_variable_area 0x200000
%define number_of_cores     0x200004

BITS 32
JMP protected_mode

local: dq 0x0
;;
;; Seccion de datos
;; -------------------------------------------------------------------------- ;;
mensaje_inicio64_msg:     db '[BSP]Starting up in long mode(IA32e compatibility mode)...'
mensaje_inicio64_len      equ $ - mensaje_inicio64_msg

mensaje_inicio64real_msg:     db '[BSP]Starting up in full long mode...'
mensaje_inicio64real_len      equ $ - mensaje_inicio64real_msg

mensaje_64bitserr_msg:     db '[BSP]FAIL! 64 bits mode unavailable! -> Kernel Halted.'
mensaje_64bitserr_len      equ $ - mensaje_64bitserr_msg

mensaje_cpuiderr_msg:     db '[BSP]FAIL! CPUID unavailable! -> Kernel Halted.'
mensaje_cpuiderr_len        equ $ - mensaje_cpuiderr_msg

mensaje_paging4g_msg:             db '[BSP]Configuring PAE paging up to 4GB...'
mensaje_paging4g_len              equ $ - mensaje_paging4g_msg

mensaje_paging64g_msg:             db '[BSP]Extending paging up to 64GB...'
mensaje_paging64g_len              equ $ - mensaje_paging64g_msg

mensaje_interrupt_msg:             db '[BSP]Initializing interrupt handling...'
mensaje_interrupt_len              equ $ - mensaje_interrupt_msg

mensaje_timer_msg:             db '[BSP]Configuring timer...'
mensaje_timer_len              equ $ - mensaje_timer_msg

mensaje_multicore_msg:             db '[BSP]Starting up multicore...'
mensaje_multicore_len              equ $ - mensaje_multicore_msg

mensaje_ok_msg:             db 'OK!'
mensaje_ok_len              equ $ - mensaje_ok_msg

mensaje_fail_msg:             db 'FAIL!'
mensaje_fail_len              equ $ - mensaje_fail_msg

apStartupPtr: resb 4;reservo 4 bytes(32 bits)
grubInfoStruct: resb 4;reservo 4 bytes(32 bits)

;------------------------------------------------------------------------------------------------------
;------------------------------- comienzo modo protegido ----------------------------------------------
;------------------------------------------------------------------------------------------------------

BITS 32
;Pasaje de parametros:
;   - en eax viene el puntero en donde grub comenzo a cargar el modulo
;   - en ebx viene el puntero a la informacion de grub multiboot_info_t* (ver multiboot.h)
;   - en ecx viene el puntero al codigo de los aps
;
;Notas:
;   - El comienzo del modulo en memoria por grub puede cambiar, pero SIEMPRE va a ser por encima del primer mega
;       con lo cual todas las estructuras, incluidas GDT, IDT, etc estan por encima del mega, en modo protegido
;       no hay problema para cargarla, pero es bueno anotar esto.
protected_mode:
    ; recargar la GDT -> vengo del entorno de grub donde la GDT puede ser basura
    ; ver http://www.gnu.org/software/grub/manual/multiboot/multiboot.html#Machine-state
    lgdt [GDT_DESC]

    mov [grubInfoStruct], ebx;copio el ptr a la info de grub
    mov [apStartupPtr], ecx;copio el ptr startup del ap

    ;limpio la pantalla(macro en asm_screen_utils.mac)
    limpiar_pantalla_mp

    ;limpio los registros para generar un entorno mas estable
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
    mov ax, 3<<3;{index:3 | gdt/ldt: 0 | rpl: 00} segmento de datos de kernel
    mov ds, ax;cargo como selector de segmento de datos al descriptor del indice 2 que corresponde a los datos del kernel
    mov es, ax;cargo tambien estos selectores auxiliares con el descriptor de datos del kernel
    mov fs, ax;cargo tambien estos selectores auxiliares con el descriptor de datos del kernel
    mov gs, ax;cargo tambien estos selectores auxiliares con el descriptor de datos del kernel
    mov ss, ax;cargo el selector de pila en el segmento de datos del kernel
    get_lapic_id32
    mov esp, [core_stack_ptrs + eax * 8];la pila va a partir de kernelStackPtrBSP(expand down, OJO)
    mov ebp, esp;pongo base y tope juntos.

    ; Chequeo de disponibilidad de uso de CPUID
    ; tomado de http://wiki.osdev.org/User:Stephanvanschaik/Setting_Up_Long_Mode#Detection_of_CPUID
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

;;inicializo paginacion 2mb
;    imprimir_texto_mp mensaje_paging4g_msg, mensaje_paging4g_len, 0x0F, 0, 0
;
;;    Estoy usando paginacion IA-32e => bits CR0.PG=1 + CR4.PAE=1 + EFER.LME=1
;;    paginas de 2mb
;
;;symbol containing krnPML4T pointer is krnPML4T
;;symbol containing krnPDPT pointer is krnPDPT
;;symbol containing krnPDT pointer is krnPDT
;
;; Mapeo 4GB con paginas de 2MB
;; la estructura PML4 esta en krnPML4T, creamos la primer entrada aca
;    cld                     ;limpia el direction flag -> http://en.wikipedia.org/wiki/Direction_flag
;    mov edi, [krnPML4T]
;    mov eax, [krnPDPT]
;    or eax, 0x7; attributes nibble
;    stosd
;    xor eax, eax
;    stosd
;
;    ;Nota http://en.wikipedia.org/wiki/X86_instruction_listings
;    ;stosd es equivalente a *ES:EDI = EAX; => store string double word
;
;; creo las entradas en PDP
;; la estructura PDP esta en krnPDPT, creamos las entradas desde este lugar
;    mov ecx, 64             ; hago 64 PDPE entries cada una mapea 1gb de memoria -> mas abajo las mapeo en x64 mode
;    mov edi, [krnPDPT]
;    mov eax, [krnPDT]
;    or eax, 0x7; attributes nibble
;crear_pdpentry:
;    stosd
;    push eax
;    xor eax, eax
;    stosd
;    pop eax
;    add eax, 0x00001000     ;avanzo 4k
;    dec ecx
;    cmp ecx, 0
;    jne crear_pdpentry
;
;; Crear las entradas en PD
;    mov edi, [krnPDT]
;    mov eax, 0x0000008F     ; para activar paginas de 2MB seteamos el bit 7
;    xor ecx, ecx
;pd_loop:
;    stosd
;    push eax
;    xor eax, eax
;    stosd
;    pop eax
;    add eax, 0x00200000     ;incremento 2 MB
;    inc ecx
;    cmp ecx, 2048
;    jne pd_loop            ; Create 2048 2 MiB page maps.
;
;    ; apunto cr3 al PML4
;    mov eax, [krnPML4T]
;    mov cr3, eax
;
;    imprimir_texto_mp mensaje_ok_msg, mensaje_ok_len, 0x02, 0, mensaje_paging4g_len

;inicializo paginacion 4k
    imprimir_texto_mp mensaje_paging4g_msg, mensaje_paging4g_len, 0x0F, 0, 0

;    Estoy usando paginacion IA-32e => bits CR0.PG=1 + CR4.PAE=1 + EFER.LME=1
;    paginas de 4kb

;symbol containing krnPML4T pointer is krnPML4T
;symbol containing krnPDPT pointer is krnPDPT
;symbol containing krnPDT pointer is krnPDT
;symbol containing krnPTT pointer is krnPTT

; Mapeo 4GB con paginas de 4kb
; la estructura PML4 esta en krnPML4T, creamos la primer entrada aca
    cld                     ;limpia el direction flag -> http://en.wikipedia.org/wiki/Direction_flag
    mov edi, [krnPML4T]
    mov eax, [krnPDPT]
    or eax, 0x7; attributes nibble
    stosd
    xor eax, eax
    stosd

    ;Nota http://en.wikipedia.org/wiki/X86_instruction_listings
    ;stosd es equivalente a *ES:EDI = EAX; => store string double word

; creo las entradas en PDP
; la estructura PDP esta en krnPDPT, creamos las entradas desde este lugar
    mov ecx, 64             ; hago 64 PDPE entries cada una mapea 1gb de memoria -> mas abajo las mapeo en x64 mode
    mov edi, [krnPDPT]
    mov eax, [krnPDT]
    or eax, 0x7; attributes nibble
crear_pdpentry:
    stosd
    push eax
    xor eax, eax
    stosd
    pop eax
    add eax, 0x00001000     ;avanzo 4k
    dec ecx
    cmp ecx, 0
    jne crear_pdpentry

; Crear las entradas en PD que apuntan a PTTs
    mov edi, [krnPDT]
    mov eax, [krnPTT]
    or eax, 0x7; attributes nibble
    xor ecx, ecx
pd_loop:
    stosd
    push eax
    xor eax, eax
    stosd
    pop eax
    add eax, 0x00001000    ;incremento 4kb
    inc ecx
    cmp ecx, 2048
    jne pd_loop            ; Create 2048 4 kb ptt entries.

; Crear las entradas en PT que apuntan a paginas de 4k
    mov edi, [krnPTT]
    mov eax, 0x7; attributes nibble
    xor ecx, ecx
pt_loop:
    stosd
    push eax
    xor eax, eax
    stosd
    pop eax
    add eax, 0x00001000    ;incremento 4kb
    inc ecx
    cmp ecx, 512*2048    ;512 entries por ptt(2048 PTTEs)
    jne pt_loop


    ; apunto cr3 al PML4
    mov eax, [krnPML4T]
    mov cr3, eax

    imprimir_texto_mp mensaje_ok_msg, mensaje_ok_len, 0x02, 0, mensaje_paging4g_len
    ;comienzo a inicializar 64 bits
    imprimir_texto_mp mensaje_inicio64_msg, mensaje_inicio64_len, 0x0F, 1, 0

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

    imprimir_texto_mp mensaje_ok_msg, mensaje_ok_len, 0x02, 1, mensaje_inicio64_len

    imprimir_texto_mp mensaje_inicio64real_msg, mensaje_inicio64real_len, 0x0F, 2, 0

    ;estamos en modo ia32e compatibilidad con 32 bits
    ;comienzo pasaje a 64 bits puro

    jmp 2<<3:long_mode; saltamos a modo largo, modificamos el cs con un jump y la eip(program counter)
    ;{index:2 | gdt/ldt: 0 | rpl: 00} => 00010000
    ;aca setie el selector de segmento cs al segmento de codigo del kernel

; Funciones auxiliares en 32 bits!
CPUIDNoDisponible:
imprimir_texto_mp mensaje_cpuiderr_msg, mensaje_cpuiderr_len, 0x0C, 0, mensaje_inicio64_len

    cli
    hlt
    jmp CPUIDNoDisponible

ModoLargoNoDisp:
    imprimir_texto_mp mensaje_64bitserr_msg, mensaje_64bitserr_len, 0x0C, 0, mensaje_inicio64_len

    cli
    hlt
    jmp ModoLargoNoDisp


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
    MOV ax, 3<<3;{index:3 | gdt/ldt: 0 | rpl: 00} segmento de datos de kernel
    MOV ds, ax;cargo como selector de segmento de datos al descriptor del indice 2 que corresponde a los datos del kernel
    MOV es, ax;cargo tambien estos selectores auxiliares con el descriptor de datos del kernel
    MOV fs, ax;cargo tambien estos selectores auxiliares con el descriptor de datos del kernel
    MOV gs, ax;cargo tambien estos selectores auxiliares con el descriptor de datos del kernel

    ;cargo el selector de pila en el segmento de datos del kernel
    MOV ss, ax

    ;setear la pila en para el kernel
    get_lapic_id
    mov esp, [core_stack_ptrs + eax * 8]
    ;MOV rsp, [kernelStackPtrBSP];la pila va a partir de kernelStackPtrBSP(expand down, OJO)
    MOV rbp, rsp;pongo base y tope juntos.
    imprimir_texto_ml mensaje_ok_msg, mensaje_ok_len, 0x02, 2, mensaje_inicio64real_len

    ;levanto la IDT de 64 bits
    lidt [IDT_DESC]
    call idt_inicializar

;    imprimir_texto_ml mensaje_paging64g_msg, mensaje_paging64g_len, 0x0F, 3, 0

;    ;genero el resto de las estructuras de paginacion para mapear 64 gb
;    ;lo tengo que hacer aca porque la direccion es de 64 bits, en modo protegido de 32 no podia
;    mov rcx, 0x0000000000000000
;    mov rax, 0x000000010000008F
;    mov rdi, [krnPDT]
;    add rdi, 0x4000;los primeros 4gb ya los tengo bien mapeados
;loop_64g_structure:
;    stosq
;    add rax, 0x0000000000200000
;    add rcx, 1
;    cmp rcx, 30720         ; 32768 - 2048 (ya mapeamos 2048*2mb)
;    jne loop_64g_structure
;
;    imprimir_texto_ml mensaje_ok_msg, mensaje_ok_len, 0x02, 3, mensaje_paging64g_len

    imprimir_texto_ml mensaje_interrupt_msg, mensaje_interrupt_len, 0x0F, 4, 0

    ;inicializo la consola

    mov rdi, 7
    call console_setYCursor
    mov rdi, 0
    call console_setXCursor

    ;configurar controlador de interrupciones
    call deshabilitar_pic
    call resetear_pic
    call habilitar_pic

    ;habilito las interrupciones! necesario para timer y core_sleep
    imprimir_texto_ml mensaje_ok_msg, mensaje_ok_len, 0x02, 4, mensaje_interrupt_len

    ;initialize_timer -> para multicore init es necesario por los core_sleep
    imprimir_texto_ml mensaje_timer_msg, mensaje_timer_len, 0x0F, 5, 0
    call initialize_timer
    imprimir_texto_ml mensaje_ok_msg, mensaje_ok_len, 0x02, 5, mensaje_timer_len
    ;inicializamos multicore

    ; inicializamos a 0 variables de multicore
clean_static_variable_area:
    mov rcx, 0x8000>>3 ; divido por 8
    mov rax, static_variable_area
    xor rdx, rdx
    clean_variables:
        mov [rax], rdx
        lea rax, [rax + 8]
        loop clean_variables

    inc byte [number_of_cores]

enable_sse: ;Taken from osdev
    mov rax, cr0
    and ax, 0xFFFB      ;clear coprocessor emulation CR0.EM
    or ax, 0x2          ;set coprocessor monitoring  CR0.MP
    mov cr0, rax
    mov rax, cr4
    or ax, 3 << 9       ;set CR4.OSFXSR and CR4.OSXMMEXCPT at the same time
    mov cr4, rax

    sti

    imprimir_texto_ml mensaje_multicore_msg, mensaje_multicore_len, 0x0F, 6, 0
    call multiprocessor_init
    imprimir_texto_ml mensaje_ok_msg, mensaje_ok_len, 0x02, 6, mensaje_multicore_len

    call deshabilitar_pic ;es esto o no tener interrupciones

    ;fin inicio kernel para BSP en 64 bits!
    ;arrancan las pruebas!
tests:
%define SYNC
;%define FFT
%ifdef  SYNC
    call test_mem_sync
   ; call test_sync_ipi_cores
%elifdef FFT
    call test_fft_dual_mem
    call test_fft_mono
    call test_fft_dual_ipi
%else
    call test_1_core
    call test_2_cores
    call test_ipi_cores
%endif
    ;call test_sum_vector1
    ;call test_sum_vector2
;
    ;mov byte [sleep_ap], 1
    ;call make_ap_jump


sleep_bsp:
    hlt
    jmp sleep_bsp



;; -------------------------------------------------------------------------- ;;
