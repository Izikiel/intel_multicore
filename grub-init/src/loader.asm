;3.2 Machine state
;
;When the boot loader invokes the 32-bit operating system, the machine must have the following state:
;
;‘EAX’
;    Must contain the magic value ‘0x2BADB002’; the presence of this value indicates to the operating system ;that it was loaded by a Multiboot-compliant boot loader (e.g. as opposed to another type of boot loader that ;the operating system can also be loaded from).
;‘EBX’
;    Must contain the 32-bit physical address of the Multiboot information structure provided by the boot loader (see Boot information format).
;‘CS’
;   Must be a 32-bit read/execute code segment with an offset of ‘0’ and a limit of ‘0xFFFFFFFF’. The exact value is undefined.
;‘DS’
;‘ES’
;‘FS’
;‘GS’
;‘SS’
;    Must be a 32-bit read/write data segment with an offset of ‘0’ and a limit of ‘0xFFFFFFFF’. The exact values are all undefined.
;‘A20 gate’
;    Must be enabled.
;‘CR0’
;    Bit 31 (PG) must be cleared. Bit 0 (PE) must be set. Other bits are all undefined.
;‘EFLAGS’
;    Bit 17 (VM) must be cleared. Bit 9 (IF) must be cleared. Other bits are all undefined. 
;
;All other processor registers and flag bits are undefined. This includes, in particular:
;
;‘ESP’
;    The OS image must create its own stack as soon as it needs one.
;‘GDTR’
;    Even though the segment registers are set up as described above, the ‘GDTR’ may be invalid, so the OS image ;must not load any segment registers (even just reloading the same values!) until it sets up its own ‘GDT’.
;‘IDTR’
;    The OS image must leave interrupts disabled until it sets up its own IDT. 

;However, other machine state should be left by the boot loader in normal working order, i.e. as initialized by ;the bios (or DOS, if that's what the boot loader runs from). In other words, the operating system should be ;able to make bios calls and such after being loaded, as long as it does not overwrite the bios data structures ;before doing so. Also, the boot loader must leave the pic programmed with the normal bios/DOS values, even if ;it changed them during the switch to 32-bit mode.

%include "src/asm_screen_utils.mac"
BITS 32
global loader                          
global apStartupCode
extern kmain                            ; kmain es el punto de entrada al kernel posta, esta en un archivo aparte

section .data
	;Header multiboot de GRUB
	MODULEALIGN equ  1<<0                   ; align loaded modules on page boundaries
	MEMINFO     equ  1<<1                   ; provide memory map
	FLAGS       equ  MODULEALIGN | MEMINFO  ; this is the Multiboot 'flag' field
	MAGIC       equ    0x1BADB002           ; 'magic number' lets bootloader find the header
	CHECKSUM    equ -(MAGIC + FLAGS)        ; checksum required

	mensaje_ok_msg:             db 'Grub loader OK!'
	mensaje_ok_len              equ $ - mensaje_ok_msg	

section .__mbHeader
	align 4
	multiboot_header: 
		dd MAGIC
		dd FLAGS
		dd CHECKSUM
 
section .text

	;El codigo genuino del loader
	loader:
		;ver arriba state machine para ver como esta la pc en este lugar
		mov  esp, stack + STACKSIZE         ; Ponemos la pila
		mov  ebp, stack + STACKSIZE	    ; Ponemos el piso de la pila

		imprimir_texto_mp mensaje_ok_msg, mensaje_ok_len, 0x02, 9, 0

		push eax                            ; Pusheamos el magic number.
		push ebx                            ; Pusheamos la informacion. Esto sirve para por ejemplo obtener cuanta ram tenemos.

		call kmain					        ; Llamamos al kernel posta

		;restauro el ptr al multiboot_header
		pop ebx
		;desapilo el magic number luego del llamado
		add esp, 4

		;kmain me devolvio el puntero al entrypoint de DeliriOS o NULL si hubo error

		;en ecx dejo el puntero al comienzo del stage2 del loader de los ap o NULL si hubo error
		mov ecx, [apStartupCode]
		;la idea es que un stage1 de los ap ocupe lo menos posible y haga jmp a memoria alta
		;porque tengo que pisar grub para poner el ip en memoria baja y comenzar en modo real en los aps...
		;ps. el stage1 del loader de los ap va si o si abajo del mega, pisamos grub, ver en
		;linker script de DeliriOS donde esta pegoteado eso, seguramente en 0x2000, pero revisar
		;por si en el futuro se cambia...		
		cmp eax, 0x0;NULL
		je hang
		;si no es null paso por ebx el puntero a la informacion de grub multiboot_info_t* 
		;(ver multiboot.h) y salta al entrypoint de DeliriOS
		jmp eax

	hang:
		xchg bx, bx
		hlt                                 ; Halt por si no funca kmain (no deberia).
		jmp  hang
	 
section .bss

	;Pila
	;Reservamos un espacio de pila inicial del loader, 64K
	STACKSIZE equ 0x10000                    
	align 4
	stack:
		resb STACKSIZE                      ; Reservamos la cantidad de la pila que queremos

	apStartupCode: resb 4;reservo memoria 32 bits