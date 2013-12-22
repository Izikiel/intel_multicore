global cmos_writeb

cmos_writeb:
	push ebp
	mov ebp, esp
	cli
	;Obtengo el puerto y lo selecciono desde comandos (0x70)
	mov eax, [ebp+8]
	out 0x70, al
	;Obtengo el valor y lo mando al puerto de datos (0x70)
	mov eax, [ebp+12]
	out 0x71, al
	sti
	pop ebp
	ret
