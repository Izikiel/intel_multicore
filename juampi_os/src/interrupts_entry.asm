%include "mode_switch.inc"

;Handler general
extern irq_common_handler;

;Ignorar la interrupcion
;Sirve para evitar interrupciones espurias
global _irq_ignore_handler
_irq_ignore_handler:
	iret

;Handlers de interrupcciones
;Macro que genera un handler de interrupcion. Lo que hace es pushear el codigo de la interrupcion
;antes de saltar al handler comun.
%macro IRQ 2
global _irq %+ %1
_irq %+ %1 %+ :
	pushfd
	cli
	push dword %2
	jmp _irq_common
%endmacro

IRQ 0, 32
IRQ 1, 33
IRQ 2, 34
IRQ 3, 35
IRQ 4, 36
IRQ 5, 37
IRQ 6, 38
IRQ 7, 39
IRQ 8, 40
IRQ 9, 41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

;Stub comun a todas las excepciones.
_irq_common:
	pushad
	
	KERNEL_SPACE_SWITCH
	call irq_common_handler
	USER_SPACE_SWITCH
	
	popad
	add esp,4
	popfd
	iretd
