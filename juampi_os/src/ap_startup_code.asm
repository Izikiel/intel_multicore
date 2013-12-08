;Este codigo es el codigo de inicializacion de los Application Processors.
;Dado que inician en modo real, deben iniciar el procesador desde cero.
BITS 16

section .__apStartupCode
global mp_ap_start

mp_ap_start:
	cli
	jmp $
