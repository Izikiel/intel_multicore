global cmos_writeb

cmos_writeb:
	cli
	;Obtengo el puerto y lo selecciono desde comandos (0x70)
	mov rax, rdi
	out 0x70, al
	;Obtengo el valor y lo mando al puerto de datos (0x71)
	mov rax, rsi
	out 0x71, al
	sti
	ret
