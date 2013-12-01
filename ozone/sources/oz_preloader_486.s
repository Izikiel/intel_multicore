##+++2002-08-17
##    Copyright (C) 2001,2002  Mike Rieker, Beverly, MA USA
##
##    This program is free software; you can redistribute it and/or modify
##    it under the terms of the GNU General Public License as published by
##    the Free Software Foundation; version 2 of the License.
##
##    This program is distributed in the hope that it will be useful,
##    but WITHOUT ANY WARRANTY; without even the implied warranty of
##    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
##    GNU General Public License for more details.
##
##    You should have received a copy of the GNU General Public License
##    along with this program; if not, write to the Free Software
##    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
##---2002-08-17

!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
!!									!!
!!  This is the start of the loader program				!!
!!  It is read into memory by the boot block program starting at 	!!
!!  address 0x8000							!!
!!									!!
!!  Since we are still in 16-bit real mode, it has to be an as86 	!!
!!  image.  Since they do not link with the normal linker, it is 	!!
!!  simply cat'd onto the beginning of the real loader.			!!
!!									!!
!!  We give this program up to 3.5K to work with.  It is followed by 	!!
!!  the loader param block, then the fommy loader.  We must be in 	!!
!!  32-bit mode before we jump to the fommy loader.			!!
!!									!!
!!  On entry to this routine, the registers are set up like this by 	!!
!!  the boot block routine:						!!
!!									!!
!!	cs  = 0x0800							!!
!!	ax  = boot device type, 'CD', 'FD', 'HD'			!!
!!	ecx = param page starting lbn					!!
!!	dl  = boot driveno						!!
!!	eip = 0								!!
!!									!!
!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!

					! these values must match those in ozone_boot_486.s, oz_loader_486.s, oz_ldr_loader.c, oz_kernel_486.s, oz_hw_486.h
GDTBASE     = 0x7FE0			! this is where the gdt will go
PLDRSEG	    = 0x0800			! this is our start address
PRMBASE     = 0x0900			! this is where the parameter block is
LDRSEG      = 0x0A00			! this is where the 32-bit loader is
ALTCPUSEG   = 0x0100			! this is where the altcpustart routine goes

KNLCODESEG  = 16			! this is the 32-bit kernel code segment number
					! note: if this is changed, you will have to change how the gdt (in here) is built

STACKSIZE   = 0x1000			! our stack size = 4k (ie, just before the param block)
					! = (PRMBASE-PLDRSEG)*16

	.text
	.org	0			! everything is relative to zero
					! relocation is done by setting all segment registers to PLDRSEG

	.globl	_main
_main:
	mov	di,#STACKSIZE		! get stack size
	mov	bx,#PLDRSEG		! set up PLDRSEG segment number
	mov	ds,bx			! put in ds
	mov	ss,bx			! put in ss
	mov	sp,di			! put stack just below where 32-bit loader is
	mov	drivetype,ax		! save the boot device type
	mov	paramlbn,ecx		! save the param block starting lbn
	mov	driveno,dl		! save the driveno

	mov	ax,#0x0E21		! output an exclam to say we're here
	xor	bx,bx
	int	0x10
!
! Test the print_all routine
! Output line should be 0123 4567 89AB CDEF 5555 AAAA
!
!!	mov	ax,#0x5555
!!	mov	si,ax
!!	add	ax,ax
!!	mov	di,ax
!!	mov	ax,#0x0123
!!	mov	bx,#0x4567
!!	mov	cx,#0x89ab
!!	mov	dx,#0xcdef
!!	call	print_all
!!		.ascii	"oz_preloader_486: test"
!!		.byte	0
!
! Print out boot device
!
	call	print_msgi
		.byte	13,10
		.ascii	"oz_preloader_486: booted from "
		.byte	0
	mov	al,drivetype+0
	mov	ah,#0x0E
	xor	bx,bx
	int	0x10
	mov	al,drivetype+1
	mov	ah,#0x0E
	xor	bx,bx
	int	0x10
	call	print_msgi
		.ascii	" paramlbn "
		.byte	0
	mov	dx,paramlbn+2
	call	print_hex
	mov	dx,paramlbn+0
	call	print_hex
!
! Wait a few seconds for floppy motor to shut off
!
	xor	ax,ax		! get what time it is now
	int	0x1A
motorloop:
	mov	ax,#0x40	! set up for accesing BIOS data area
	mov	es,ax
	seg	es		! check floppy motor bits
	test	0x3F,#0x0F
	je	motordone	! all done if the motors are off
	push	dx		! save start time
	xor	ax,ax		! get the time again
	int	0x1A
	mov	ax,dx		! put it in ax
	pop	dx		! restore start time
	sub	ax,dx		! compute the difference
	cmp	ax,#90		! if lt 18*number_of_seconds, repeat
	jc	motorloop
motordone:
!
! Set the A20 enable line.  If this doesn't work, the memory 
! routine in the fommy loader will only find 1MB of memory.
!
	call	empty_8042
	mov	al,#0xd1
	outb	#0x64
	call	empty_8042
	mov	al,#0xdf
	outb	#0x60
	call	empty_8042

! make sure any possible coprocessor is properly reset..

	xor	ax,ax
	out	#0xf0,al
	call	iodelay
	out	#0xf1,al
	call	iodelay

!
! Set up the GDT
!
!  We use a 'flat' memory model, ie, we want to make segmentation as
!  transparent as possible.
!
!  Hardware uses GDT[0] as a NULL entry that causes access violations
!  We set up GDT[1] to be the kernel code segment that overlays all
!  4Gb.  GDT[2] is the kernel read/write segment that maps exactly
!  the same stuff.
!
	push	ds			! save data segment (PLDRSEG)
	xor	bx,bx			! set the data segment to number 0 for now
	mov	ax,#GDTBASE		! point to where gdt is going within segment 0
	mov	ds,bx
	mov	di,ax
	mov	 0(di),bx		! GDT[0] is all zeroes
	mov	 2(di),bx
	mov	 4(di),bx
	mov	 6(di),bx
	mov	 8(di),bx		! GDT[1] is all zeroes
	mov	10(di),bx
	mov	12(di),bx
	mov	14(di),bx
	mov	ax,#0xFFFF		! GDT[2] is the kernel code segment
	mov	cx,#0x9A00
	mov	dx,#0x00CF
	mov	16(di),ax
	mov	18(di),bx
	mov	20(di),cx
	mov	22(di),dx
	mov	cx,#0x9200		! GDT[3] is the kernel data segment
	mov	24(di),ax
	mov	26(di),bx
	mov	28(di),cx
	mov	30(di),dx
	pop	ds			! restore data segment (PLDRSEG)
	mov	ax,#31			! number of bytes in the GDT - 1
	push	bx			! GDT<32:47> = zeroes
	push	di			! GDT<16:31> = GDTBASE
	push	ax			! GDT<00:15> = 31
	mov	si,sp			! point to data on stack
	lgdt	(si)			! load it into GDTR

!!	call	print_msgi
!!		.byte	13,10
!!		.ascii	"oz_preloader_486: gdt set up"
!!		.byte	0

!
! Dump out the first few bytes at 9000
!
!!	xor	ax,ax
!!	mov	es,ax
!!	mov	di,#0x9000
!!	seg	es
!!	mov	ax,0(di)
!!	seg	es
!!	mov	bx,2(di)
!!	seg	es
!!	mov	cx,4(di)
!!	seg	es
!!	mov	dx,6(di)
!!	call	print_all
!!		.ascii	"oz_preloader_486: words at 9000:"
!!		.byte	0
!
! Dump out the gdtr
!
!!	push	ax
!!	push	ax
!!	push	ax
!!	push	ax
!!	mov	di,sp
!!	sgdt	(di)
!!	pop	ax
!!	pop	bx
!!	pop	cx
!!	pop	dx
!!	call	print_all
!!		.ascii	"oz_preloader_486: gdtr:"
!!		.byte	0
!
! Dump out the gdt itself
!
!!	mov	cx,#4
!!	mov	di,#GDTBASE
!!dump_gdt_loop:
!!	push	cx
!!	seg	es
!!	mov	ax,0(di)
!!	seg	es
!!	mov	bx,2(di)
!!	seg	es
!!	mov	cx,4(di)
!!	seg	es
!!	mov	dx,6(di)
!!	call	print_all
!!		.ascii	"oz_preloader_486: gdt[x]:"
!!		.byte	0
!!	add	di,#8
!!	pop	cx
!!	dec	cx
!!	jne	dump_gdt_loop

! Copy the MTRR's to the altcpustart routine so it can set 
! up other cpu's just like the bios set up the boot cpu

	mov	eax,#1			! read cpu capabilities
	.byte	0x0F,0xA2		! 'cpuid' instruction
	mov	si,#mtrrtable		! point to table of mtrr contents
	xor	ebx,ebx			! clear first long of table
	xchg	ebx,(si)		! ... and save it in ebx
	bt	edx,#12			! see if mtrr's present in cpu
	jnc	mtrr_read_done		! if not, all done (with first table entry zeroed)
	mov	(si),ebx		! has mtrr's, restore first table entry
mtrr_read_loop:
	mov	ecx,(si)		! get a mtrr register number
	or	ecx,ecx			! zero means end-of-table
	je	mtrr_read_done
	.byte	0x0F,0x32		! ok, 'rdmsr' instruction
	mov	4(si),eax		! save the contents
	mov	8(si),edx
	add	si,#12			! increment to next table entry
	jmp	mtrr_read_loop
mtrr_read_done:

!
! This is the last use of the BIOS
!

	call	print_msgi
		.byte	13,10
		.ascii	"oz_preloader_486: enabling protection"
		.byte	0

	cli				! inhibit interrupt delivery
					! yes, it makes a difference when
					! we go into protected mode because
					! the BIOS might have something going
					! so don't use the BIOS from now on

! Put the SMP CPU startup routine in place.  This routine is used in an SMP 
! environment by the oz_hw_cpu_bootalts routine to get the alternate cpu's going.

! This is kinda stuck in here because it needs to be a 16-bit 
! routine and the gas assembler doesn't do 16-bit routines

	mov	ecx,#altcpustart_end-altcpustart
	lea	si,altcpustart
	xor	ax,ax
	mov	di,ax
	mov	ax,#ALTCPUSEG
	mov	es,ax
	cld
	rep
	movsb

! Get registers set up to pass to oz_loader_486

	mov	ax,drivetype		! get the boot device type
	mov	ecx,paramlbn		! get the param block starting lbn
	mov	dl,driveno		! get the driveno

! We should now be able to turn on protected mode and access all
! of memory as a linear array and act as a genuine 32-bit processor

	mov	bx,#1			! this is the protected mode enable bit
	lmsw	bx			! load it in the machine status word

! Now jump to the 32-bit loader at 0x00009000
! Complete the change to 32-bits by using a far jump 
! that uses the new code segment number

	jmpi	LDRSEG*16,KNLCODESEG	! target address = 0x00009000
					! segment 16 = GDT[2] = the code segment

	.align	4
paramlbn:	.long	0
drivetype:	.word	0
driveno:	.byte	0

!
! This routine checks that the keyboard command queue is empty
! (after emptying the output buffers)
!
! No timeout is used - if this hangs there is something wrong with
! the machine, and we probably couldn't proceed anyway.
!
empty_8042:
	call	iodelay
	in	al,#0x64	! 8042 status port
	test	al,#1		! output buffer?
	jz	no_output
	call	iodelay
	in	al,#0x60	! read it
	jmp	empty_8042
no_output:
	test	al,#2		! is input buffer full?
	jnz	empty_8042	! yes - loop
	ret
!
iodelay:
	movb	bl,#200
iodelay_loop:
	decb	bl
	jne	iodelay_loop
	ret
!
! Print all registers
!
!   Input:
!
!	registers = to be printed
!	rtn address = null terminated prefix string
!
!   Scratch:
!
!	none
!
!!print_all:
!!	push	di			! push registers we print
!!	push	si
!!	push	dx
!!	push	cx
!!	push	bx
!!	push	ax
!!	call	print_msgi		! print a CR/LF
!!		.byte	13,10,0
!!	mov	di,sp			! point to registers on stack
!!	mov	si,12(di)		! get return address
!!	call	print_msg		! print message that follows call
!!	mov	12(di),si		! update the return address
!!	mov	cx,#6			! going to print 6 registers from stack
!!print_all_loop:
!!	push	cx			! save counter on stack
!!	mov	ax,#0x0e20		! print a space
!!	xor	bx,bx
!!	int	0x10
!!	mov	dx,(di)			! get register value word to print
!!	call	print_hex		! print out the word
!!	inc	di			! increment to next word on stack
!!	inc	di
!!	pop	cx			! get the counter
!!	dec	cx			! see if there are more to print
!!	jne	print_all_loop		! if so, continue printing
!!	pop	ax			! pop saved registers
!!	pop	bx
!!	pop	cx
!!	pop	dx
!!	pop	si
!!	pop	di
!!	ret				! all done

!
! Print inline message
!
!   Input:
!
!	null terminated message follows call inline
!
!   Scratch:
!
!	ax
!
print_msgi:
	pop	ax			! ax <- return address
	push	si			! save some registers on stack
	push	bx
	push	cx
	push	dx
	mov	si,ax			! point to the message (it follows the call instruction)
	call	print_msg		! print it out
	mov	ax,si			! save new return address (following the null byte)
	pop	dx			! restore the saved registers
	pop	cx
	pop	bx
	pop	si
	push	ax			! push the modified return address back on stack
	ret				! return to point following the null
!
! Print message pointed to by ds:si
!
!   Input:
!
!	ds:si =  points to null terminated message
!
!   Output:
!
!	si = incremented to point just past the null
!
!   Scratch:
!
!	ax,bx
!
print_msg:
	lodsb				! al <- (ds:si)+
	or	al,al			! test al
	je	print_msg_done		! done if zero
	mov	ah,#0x0e		! put 0x0e in top = print char
	xor	bx,bx			! clear page number
	int	0x10			! print it
	jmp	print_msg		! try for more
print_msg_done:
	ret
!
!  Print the word in dx in hexadecimal
!
!   Input:
!
!	dx = word to be printed
!
!   Scratch:
!
!	ax,bx,cx
!
!
print_hexb:
	mov	cx,#2		! 2 hex digits
	rol	dx,#8
	jmp	print_hexx
print_hex:
	mov	cx,#4		! 4 hex digits
print_hexx:
	xor	bx,bx		! clear page number
print_digit:
	rol	dx,#4		! rotate so that lowest 4 bits are used
	mov	ax,#0xe0f	! ah = request, al = mask for nybble
	and	al,dl
	add	al,#0x90	! convert al to ascii hex (four instructions)
	daa
	adc	al,#0x40
	daa
	int	0x10
	loop	print_digit
	ret
!
! Pause (prints a '>' and waits for the ANY key)
!
pause:
	push	bx
	push	ax
	mov	ax,#0xe3e		! output a '>'
	xor	bx,bx
	int	0x10
	xor	ax,ax			! read a keystroke
	int	0x16
	pop	ax
	pop	bx
	ret

!
! Alternate cpu startup routine
!
! This routine gets copied to location 1000 by the preloader
!
! This routine is jumped to by the alternate cpus when 
! they get activated by the oz_hw_cpu_bootalts routine
!
! All we assume is that the cpu's are in 16-bit mode
!

					! these symbols must match the same ones in oz_kernel_486.s

altcpustart_w3_gdtdes = 4082		! gdt descriptor that the cpu's load
					! set by oz_hw_cpu_bootalts before it starts the cpus

altcpustart_l_entry  = 4088		! address to jump to after in 32-bit mode
					! set by oz_hw_cpu_bootalts before it starts the cpus
					! the cpu jumps to this address with %edx = its cpu index number

altcpustart_l_cpuidx = 4092		! incremented by each alternate cpu that starts
					! oz_hw_cpu_bootalts sets this to 1 before it starts the cpus

	.align	4
altcpustart:
	cli				! just to be certain
	mov	ax,#ALTCPUSEG		! init segment registers to our segment number
	mov	ds,ax
	mov	es,ax
	mov	fs,ax
	mov	gs,ax

	mov	si,#imhere-altcpustart
	lock
	inc	(si)

! Load the mtrr's to be the same as the boot cpu.
! If the boot cpu has no mtrr's the first long of the table is zero, so we don't do anything.
! Do this here before much stuff gets in the cache (it should be disabled by default), and 
! the only thing that should be in there are bits and pieces of this page which is short-lived.

	wbinvd				! invalidate any cache we have now
	mov	si,#mtrrtable-altcpustart ! point to table of mtrr contents
mtrr_write_loop:
	mov	ecx,(si)		! get a mtrr register number
	or	ecx,ecx			! zero means end-of-table
	je	mtrr_write_done
	mov	eax,4(si)		! ok, get the contents we need
	mov	edx,8(si)
	.byte	0x0f,0x30		! 'wrmsr' instruction - write to cpu
	add	si,#12			! increment to next table entry
	jmp	mtrr_write_loop
mtrr_write_done:
	wbinvd				! invalidate cache again

	mov	eax,cr0			! turn off cache disable bits
	and	eax,#0x9fffffff
	mov	cr0,eax

! Now set up protected mode and jump to kernel (entrypoint cpu_altstart in oz_kernel_486.s)

	mov	si,#altcpustart_l_entry	! point to where the entrypoint longword is
	mov	ecx,(si)		! get the entrypoint
	mov	di,#altcpustart_entry-altcpustart ! point to where the 0x12345678 is
	mov	(di),ecx		! store the actual address we want there

	mov	di,#altcpustart_w3_gdtdes ! point to gdt descriptor
	lgdt	(di)			! load it into GDTR

	mov	ax,#1			! this is the protected mode enable bit
	lmsw	ax			! load it in the machine status word

			db	0x66	! jmpi long ptr 0x12345678,KNLCODESEG
			db	0xea
altcpustart_entry:	dd	0x12345678
			dw	KNLCODESEG

	.align	4
imhere:	.long	0x11223344

! Table of msr's to copy from boot cpu to alternate cpu's
! Note that if the boot cpu doesn't have mtrr's, it cleared the first long of the table

	! Memory Type Range Registers

	MTRR_cap           = 0x0fe
	MTRR_phys_base_0   = 0x200
	MTRR_phys_mask_0   = 0x201
	MTRR_phys_base_1   = 0x202
	MTRR_phys_mask_1   = 0x203
	MTRR_phys_base_2   = 0x204
	MTRR_phys_mask_2   = 0x205
	MTRR_phys_base_3   = 0x206
	MTRR_phys_mask_3   = 0x207
	MTRR_phys_base_4   = 0x208
	MTRR_phys_mask_4   = 0x209
	MTRR_phys_base_5   = 0x20a
	MTRR_phys_mask_5   = 0x20b
	MTRR_phys_base_6   = 0x20c
	MTRR_phys_mask_6   = 0x20d
	MTRR_phys_base_7   = 0x20e
	MTRR_phys_mask_7   = 0x20f
	MTRR_fix_64k_00000 = 0x250
	MTRR_fix_16k_80000 = 0x258
	MTRR_fix_16k_a0000 = 0x259
	MTRR_fix_4k_c0000  = 0x268
	MTRR_fix_4k_c8000  = 0x269
	MTRR_fix_4k_d0000  = 0x26a
	MTRR_fix_4k_d8000  = 0x26b
	MTRR_fix_4k_e0000  = 0x26c
	MTRR_fix_4k_e8000  = 0x26d
	MTRR_fix_4k_f0000  = 0x26e
	MTRR_fix_4k_f8000  = 0x26f
	MTRR_default_type  = 0x2ff

	.align	4
mtrrtable:
	.long	MTRR_phys_base_0,0,0	! variable ranges
	.long	MTRR_phys_mask_0,0,0
	.long	MTRR_phys_base_1,0,0
	.long	MTRR_phys_mask_1,0,0
	.long	MTRR_phys_base_2,0,0
	.long	MTRR_phys_mask_2,0,0
	.long	MTRR_phys_base_3,0,0
	.long	MTRR_phys_mask_3,0,0
	.long	MTRR_phys_base_4,0,0
	.long	MTRR_phys_mask_4,0,0
	.long	MTRR_phys_base_5,0,0
	.long	MTRR_phys_mask_5,0,0
	.long	MTRR_phys_base_6,0,0
	.long	MTRR_phys_mask_6,0,0
	.long	MTRR_phys_base_7,0,0
	.long	MTRR_phys_mask_7,0,0
	.long	MTRR_fix_64k_00000,0,0	! fixed ranges
	.long	MTRR_fix_16k_80000,0,0
	.long	MTRR_fix_16k_a0000,0,0
	.long	MTRR_fix_4k_c0000,0,0
	.long	MTRR_fix_4k_c8000,0,0
	.long	MTRR_fix_4k_d0000,0,0
	.long	MTRR_fix_4k_d8000,0,0
	.long	MTRR_fix_4k_e0000,0,0
	.long	MTRR_fix_4k_e8000,0,0
	.long	MTRR_fix_4k_f0000,0,0
	.long	MTRR_fix_4k_f8000,0,0
	.long	MTRR_default_type,0,0	! this contains the enable bit so must be last
	.long	0			! end of table

altcpustart_end:
