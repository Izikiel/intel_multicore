extern switch_kernel_mode
extern switch_user_mode

%macro KERNEL_SPACE_SWITCH 0
	;Pasamos el procesador al control del kernel	
	mov bx, ds
	mov ax, 0x10 ;Segmento de datos del kernel
	mov ds,ax
	mov es,ax
	mov fs,ax
	mov gs,ax	
%endmacro

%assign _jmp_i 0
%macro USER_SPACE_SWITCH 0
	;Volvemos el procesador a como estaba antes
	
	cmp bx, 0x23
	jne .was_kernel %+ _jmp_i
	
.was_kernel %+ _jmp_i %+:
	mov ds, bx
	mov es, bx
	mov fs, bx
	mov gs, bx

%assign _jmp_i _jmp_i+1
%endmacro
