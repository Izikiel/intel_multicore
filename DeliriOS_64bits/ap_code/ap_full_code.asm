section .text

;------------------------------------------------------------------------------------------------------
;------------------------------- comienzo modo protegido ----------------------------------------------
;------------------------------------------------------------------------------------------------------

%macro get_lapic_id 0  ; para distinguir los procesadores entre si
    xor eax, eax       ; manual de intel capitulo 10 tabla 10-1 Local APIC Register Address Map
    mov eax, 0xfee00020 ;si no lo hago asi en 64 bochs me enchufa 0xffffffff adelante y boom!
    mov eax, [eax]
    shr eax, 24
    and eax, 0xFF
%endmacro

BITS 32

jmp ap_protected_mode

krnPML4T: dq 0xb0b
stack_pointer_table: times 16 dd 0x0 ; defino tabla donde guardar los stack pointers iniciales

ap_protected_mode:
    limpio los registros
    xor eax, eax
    xor ebx, ebx
    xor ecx, ecx
    xor edx, edx
    xor esi, esi
    xor edi, edi
    xor ebp, ebp
    xor esp, esp

    get_lapic_id

    mov esp, [stack_pointer_table + eax * 4];la pila va a partir de kernelStackPtrBSP(expand down, OJO)
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

    jmp (1<<2):long_mode; saltamos a modo largo, modificamos el cs con un jump y la eip(program counter)
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
    MOV ax, 3 << 3;{index:3 | gdt/ldt: 0 | rpl: 00} segmento de datos de kernel
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

    ;imprimir mensaje en pantalla
    imprimir_texto_ml mensaje_ap_started_msg, mensaje_ap_started_len, 0x0F, 10, 0

    ;llamo al entrypoint en kmain64
    ;call startKernel64_APMODE


    xchg bx, bx
    jmp $



mensaje_ap_started_msg: db "AP en 64 :D"
mensaje_ap_started_len: equ $ - mensaje_ap_started_msg
    ;fin inicio kernel para AP en 64 bits!

;    start_sort:
;        cmp byte [start], 0
;        je start_sort
;        xor rsi, rsi
;        xor rdi, rdi
;        xor rdx, rdx
;
;        mov esi, [start_point]
;        mov rdi, array_global
;        mov rdx, 1
;
;        add rdi, rsi
;
;        call mergesort
;        mov byte [done], 1
;        imprimir_texto_ml array_global, 52, 0x02, 11, 0
;
;    start_merge_:
;        call do_reverse_merge
;
;
;    haltApCore:
;        imprimir_texto_ml array_global, 52, 0x02, 11, 0
;        jmp haltApCore

; -------------------------------------------------------------------------- ;;