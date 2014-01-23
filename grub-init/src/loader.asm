%include "src/asm_screen_utils.mac"
;La idea es bootear con GRUB.
global loader                          
extern kmain                            ; kmain es el punto de entrada al kernel posta, esta en un archivo aparte
 
;Header multiboot de GRUB
MODULEALIGN equ  1<<0                   ; align loaded modules on page boundaries
MEMINFO     equ  1<<1                   ; provide memory map
FLAGS       equ  MODULEALIGN | MEMINFO  ; this is the Multiboot 'flag' field
MAGIC       equ    0x1BADB002           ; 'magic number' lets bootloader find the header
CHECKSUM    equ -(MAGIC + FLAGS)        ; checksum required

section .data
mensaje_ok_msg:             db 'Grub loader OK!'
mensaje_ok_len              equ $ - mensaje_ok_msg

section .__mbHeader 
;AOUT KLUDGE 
align 4
	dd MAGIC
	dd FLAGS
	dd CHECKSUM
 
section .text

;El codigo genuino del loader
loader:
	mov  esp, stack + STACKSIZE         ; Ponemos la pila
	mov  ebp, stack + STACKSIZE	    ; Ponemos el piso de la pila

	imprimir_texto_mp mensaje_ok_msg, mensaje_ok_len, 0x02, 9, 0


	push eax                            ; Pusheamos el magic number.
	push ebx                            ; Pusheamos la informacion. Esto sirve para por ejemplo obtener cuanta ram tenemos.

	cli
	call kmain					        ; Llamamos al kernel posta
	
	add esp, 8;desapilo parametros a kmain

	cli
.hang:
	hlt                                 ; Halt por si no funca kmain (no deberia).
	jmp  .hang
 

section .bss

;Pila
;Reservamos un espacio de pila inicial, 64K
STACKSIZE equ 0x10000                    
align 4
stack:
	resb STACKSIZE                      ; Reservamos la cantidad de la pila que queremos
