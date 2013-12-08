;Este codigo es el codigo de inicializacion de los Application Processors.
;Dado que inician en modo real, deben iniciar el procesador desde cero.
BITS 16

section .text
global mp_ap_start

jmp mp_ap_start

iniciando_mr_msg db 'Hola! Soy un core...'
iniciando_mr_len equ    $ - iniciando_mr_msg

mp_ap_start:
	;Imprimir mensaje
	;Cuanto queda imprimir
	mov cx,iniciando_mr_len

	;Posicion en buffer
	mov edi,iniciando_mr_msg

	;Color
	mov ax, 0x200

	;Segmento de video
	mov bx, 0xb800
	mov es, bx
	
	;Posicion en memoria de video
	mov bx, 23*2*80

.imprimir:
	mov al, [di]
	mov [es:bx], ax
	add bx, 2
	inc di
	loop .imprimir

	jmp $
