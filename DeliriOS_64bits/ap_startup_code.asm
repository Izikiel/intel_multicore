;Este codigo es el codigo de inicializacion de los Application Processors.
;Dado que inician en modo real, deben iniciar el procesador desde cero.
%include "macros/asm_screen_utils.mac"
BITS 16

section .apstartsection
global mp_ap_start

jmp mp_ap_start

iniciando_ap_msg db '[CPU1] * Core AP iniciado...'
iniciando_ap_len equ    $ - iniciando_ap_msg

mp_ap_start:
	;Imprimir mensaje
	imprimir_texto_mr iniciando_ap_msg, iniciando_ap_len, 0x0A, 0, 11*80*2 + 8
	
	cli
	hlt
