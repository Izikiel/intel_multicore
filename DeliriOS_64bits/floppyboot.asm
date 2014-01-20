%include "bios.mac" 	;Macros de la bios, para la pantalla mas que nada.
%include "fat12.mac"	;Macros de fat12 con definiciones de la estructura
%include "definiciones.mac" ; Definiciones de direcciones de memoria.

; Posiciones donde se cargan las cosas


BITS 16
ORG ADDR_BOOT
BOOT:
	cli
	jmp short start 	;Salto sobre la estructura de FAT12
	
TIMES 0Bh - ($ - $$) DB 0	; primeros 11 bytes que FAT12 ignora
DW 200h				; sectores de 512 bytes
DB  1h				; 1 sector por cluster
DW 1h				; cantidad de sectores reservados 1
DB  2h				; cantidad de FATs
DW 0E0h				; cantidad de entradas en Root Directory 224
DW 0B40h			; cantidad de sectores totales en el disco 2880
DB 0F0h				; modelo de disco 3.5" 1.44MB
DW 9h				; cantidad de sectores por FAT
DW 12h				; cantidad de sectores por cabeza 18
DW 2h				; cantidad de cabezas por cilindro 2
DD 0				; sectores ocultos 0
DD 0				; cantidad de sectores si fuera FAT32, va en 0
DB 0				; número de unidad
DB 0				; flags
DB 29h				; firma
DD 0FFFFFFFFh			; número de serie
DB 'ORGA2       '		; etiqueta, 12 bytes relleno con espacios
DB 'FAT12   '			; formato, 8 bytes relleno con espacios

LOADING: DB "Loading ["
disk_err_reset:
	; Reset Floppy A
	xor dx, dx
	xor ah, ah
	int 0x13
	ret
TIMES 11 - ($ - disk_err_reset) DB " "
	DB "]"
LOADING_LEN equ $-LOADING

start:
	call    a20wait
    mov     al,0xAD
    out     0x64,al
    call    a20wait
    mov     al,0xD0
    out     0x64,al
    call    a20wait2
    in      al,0x60
    push    ax
    call    a20wait
    mov     al,0xD1
    out     0x64,al
    call    a20wait
    pop     ax
    and     al,0xFD		;deshabilito
    out     0x60,al
    call    a20wait
    mov     al,0xAE
    out     0x64,al
    call    a20wait

	; Copia el boot sector a un lugar sensato y conocido
	mov cx, 512
	mov si, ADDR_BOOTSEC
	mov di, ADDR_BOOT
	rep movsb
	jmp main-(ADDR_BOOTSEC-ADDR_BOOT)	;salto al nuevo lugar

main:
	bios_clrscr 0	;pantalla en negro

	;imprimo por patanlla el mensajito de LOADING
	bios_write LOADING, LOADING_LEN, 0x07, 0, 0
	bios_write BOOT+FAT12_VolumeLabel, 11, 0x0f, 9, 0

	; Armo una pila de 512 bytes en un lugar que no jode
	mov ax, SEG_DATA
	mov es, ax
	mov ss, ax
	mov sp, ADDR_STACK - 4

	; Calcula el espacio ocupado por las FATs
	mov ax, WORD [BOOT+FAT12_SectorsPerFAT]
	mul BYTE [BOOT+FAT12_NumberOfFATs]
	push ax; Salva el tamaño de las FATs

	; Calcula la cantidad de sectores en el Root dir
	mov cx, [BOOT+FAT12_RootEntries]
	shr cx, 4 ; Cada entry ocupa 32 bytes, entonces hay 16 entries por sector

	add cx, ax
	mov bl, cl

	push cx ; Numero de sectores a leer
	mov bp, 20; Numero de intentos de lectura
	; Levanta las dos FATs a memoria y el root directory completo ((9+9+14)*512 = 16KB)
_load_FAT:
	pop ax
	push ax

	mov cx, 2 ; Sector 2, Cilindro 0, Cabeza 0, floppy 0
	xor dx, dx
	mov ah, 0x02
	xor bx, bx ; Levanta en la dirección es:bx
	int 0x13

	jnc _load_FAT_ok
	; falla de disco, reseteo la unidad y vuelvo a leer
	call disk_err_reset
	dec bp
	jnz _load_FAT
_load_FAT_fail:
	; Bocha errores de disco, no da mas. BSoD
	mov si, msg_disk_error
	mov	cx, msg_disk_error_len
	jmp blue_screen
msg_disk_error: db "FDD error"
msg_disk_error_len equ $ - msg_disk_error
KERNEL: db "KERNEL  BIN"  ; Archivo que estamos buscando

_load_FAT_ok:
	pop ax ; Cargo basura que quedo en la pila.

	cld
	; Busca el kernel.bin en el root directory
	pop bx ; recupera el tamaño de las FATs
	shl bx, 9 ; 512 bytes por sector
	
	mov dx, [BOOT+FAT12_RootEntries]
	;ahora busco el archivo KERNEL.BIN
_file_search_loop:
	mov si, KERNEL
	mov di, bx
	cmp BYTE [es:di], 0x00 
	je _file_not_found ; Si es cero entonces todas las entries de ahí pa abajo son "free"
	mov cx, 11
	repe cmpsb
	je _file_found
	
	add bx, FAT_DE__ ; Salteo una entrada
	dec dx
	jnz _file_search_loop
	jmp _file_not_found
	
	;No encontre el archivo, a la BSoD
_file_not_found:
	bios_disk_park 0x00
	;xchg bx, bx ;bochs debugger. Descomentar si queremos un breakpoint aca
	mov si, msg_notfound
	mov cx, msg_notfound_len
	jmp blue_screen

_file_found: ; Encontre el archivo en la direccion BX
	mov ax, [es:bx+FAT_DE_FirstCluster]
	push ax
	mov di, ADDR_KERN >> 4
	mov es, di
	; Destino: 0x0000:ADDR_KERN

_kern_load_loop_err:
	call disk_err_reset
	pop ax
_kern_load_loop:
	push ax
	add ax, 0x1f ; Offset que saltea los sectores con headers
	
;%macro bios_disk_read 3 ; LBA sector, es:dest_addr, n_sectors
	bios_disk_read ax, 0, 1
	jc _kern_load_loop_err
	add di, 0x200 >> 4
	mov es, di
	pop bx
	mov cx, bx
	shr bx, 1
	add bx, cx
	mov ax, [ss:bx]
	and cl, 0x01
	jz _kern_load_noshift
	shr ax, 4
_kern_load_noshift:
	and ah, 0x0F
	cmp ax, 0x0FF8
	jb _kern_load_loop

_kern_load_end:
	;xchg bx, bx ;bochs debugger. Descomentar si queremos un breakpoint aca
	jmp ADDR_KERN

;msg_notfound: db "File KERNEL.BIN not found! Panic Attack!",0
msg_notfound: db "KERNEL.BIN not found. Panic Attack!"
msg_notfound_len equ $ - msg_notfound
%define scr_color 0x1f

;Nuestra BSoD, porque no teniamos que ser menos que microsoft...
blue_screen: ; Recive un string en si y su longitud en cx
	push cx
	push si
	bios_clrscr scr_color
	pop si
	pop cx
	;xchg	bx, bx
	;mov si, di
	; strlen
	;xor ax, ax
	;mov es, ax
	;xor cx, cx
	;dec cx
	;repnz scasb
	;not cx
	;dec cx
	;mov cx, ax

	;push cx
	; Dibujo un box (5 x (cx+2))

	xor dx, dx
	mov ax, SEG_VIDEO
	mov es, ax
	xor di, di
	mov di, (10 * 80) * 2
	mov dl, 80
	;add	cl, 2
	sub dl, cl
	;mov bx, dx
	and dl, -2
	add di, dx

	;sub bl, 2
	;shl bl, 1
	;mov ax, (scr_color << 8) | 201
	;stosw
	;mov al, 205
	;rep stosw
	;mov al, 187
	;stosw
	;add di, bx
	;mov al, 186
	;stosw
	;inc di
	;inc di
	;pop cx
	;mov dx, cx
	;inc dl
	;inc dl
	mov ah, scr_color | 0x80
_blue_loop:
	lodsb
	stosw
	loop _blue_loop
	;mov ah, scr_color
	;inc di
	;inc di
	;mov al, 186
	;stosw
	;add di, bx
	;mov cx, dx
	;mov al, 200
	;stosw
	;mov al, 205
	;rep stosw
	;mov al, 188
	;stosw
	jmp $
	
a20wait:
        in      al,0x64
        test    al,2
        jnz     a20wait
        ret
 
a20wait2:
        in      al,0x64
        test    al,1
        jz      a20wait2
        ret
        	
TIMES 0200h - 2 - ($ - $$) DB 0 ;Lleno con ceros hasta 510 bytes

DW 0xAA55 ;Boot Sector signature

