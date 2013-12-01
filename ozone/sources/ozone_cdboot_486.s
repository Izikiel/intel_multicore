##+++2003-03-01
##    Copyright (C) 2001,2002,2003  Mike Rieker, Beverly, MA USA
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
##---2003-03-01

!
!  The CDROM boot block is loaded into 7C00 by the BIOS
!
!  It does the following:
!
!  Prints out a message
!  Reads in the oz_loader_486.bb image starting at 8000 (skipping the first block which is a copy of the boot block)
!  Jumps to the oz_loader_486.bb image
!
!  Originally, I was going to have the boot catalog just load the preloader directly starting at 8000 along with the rest of it 
!  all.  But it seems BIOS's really can't do that, even though El Torito spec says they should be able to do 0xFFFF sectors, and 
!  we only need to do at most 0x130.  So I have this block read into the default 7C00 location, then it reads the CDROM headers 
!  and loads the rest of the boot image.  Since the boot catalog shows a size of just one 2K block (4 512 byte blocks), this 
!  routine assumes the rest of the boot image is the maximum possible of 0x130 2K blocks which fills to the memory hole.
!

! These must match oz_loader_486.s, oz_preloader_486.s, oz_ldr_loader.c, oz_kernel_486.s

LB        = 0x7C00			! load base (address we're loaded at)
LOADSEG   = 0x0800			! segment to put preloader followed by everything else
LOADADR   = 0x8000			! corresponding address relative to our datasegment register

BLOCKSIZE = 2048			! bytes per sector
L2BLOCKSIZE = 11			! log2 (BLOCKSIZE)

!
! ld86 requires an entry symbol. This may as well be the usual one.
!
	.text
	.org	0			! everything is relative to zero

	.globl	_main
_main:
	jmpi	go-_main+LB,0		! set code segment register to 0
go:
	mov	ax,cs			! set all segment registers same as code segment = 0
	mov	ds,ax			! put in ds
	mov	es,ax			! put in es
	mov	ss,ax			! put in ss
	mov	sp,#LB			! put stack just below _main
	mov	driveno+LB,dl		! save boot drive number

!!	mov	ax,#0x0E2E		! output a dot
!!	xor	bx,bx
!!	int	0x10
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
!!	mov	cx,#0x89AB
!!	mov	dx,#0xCDEF
!!	call	print_all
!!		.ascii	"test"
!!		.byte	0
!
! Print out the driveno
!
	call	print_msgi
		.byte	13,10
		.ascii	"drive "
		.byte	0
	mov	dl,driveno+LB
	call	print_hexb
!
! Find and read boot catalogue
!
	mov	readparams+ 2+LB,#2	! read primary and boot volume descriptors
	mov	readparams+ 4+LB,#0
	mov	readparams+ 6+LB,#LOADSEG
	mov	readparams+ 8+LB,#16
	mov	readparams+10+LB,#0
	call	readcdrom
	mov	di,#LOADADR		! point to what was read
	mov	si,#privoldes+LB	! check it out
	mov	cx,#7
	cld
	repe
	cmpsb
	je	privoldesok
	call	print_msgi
		.ascii	"-bad primary volume descriptor"
		.byte	0
privoldeshang:
	jmp	privoldeshang
privoldesok:
	mov	di,#LOADADR+BLOCKSIZE	! point to boot volume descriptor
	mov	si,#bootvoldes+LB	! check it out
	mov	cx,#7
	cld
	repe
	cmpsb
	je	bootvoldesok
	call	print_msgi
		.ascii	"-bad boot volume descriptor"
		.byte	0
bootvoldeshang:
	jmp	bootvoldeshang
bootvoldesok:
	mov	ax,LOADADR+80		! get total blocks on volume
	mov	dx,LOADADR+82
	mov	volsize+0+LB,ax
	mov	volsize+2+LB,dx
	call	print_msgi
		.ascii	"-volsize "
		.byte	0
	mov	dx,volsize+2+LB
	call	print_hex
	mov	dx,volsize+0+LB
	call	print_hex
	mov	ax,LOADADR+BLOCKSIZE+0x47 ! get boot catalog sector number
	mov	dx,LOADADR+BLOCKSIZE+0x49
	mov	readparams+ 2+LB,#1	! read boot catalog
	mov	readparams+ 4+LB,#0
	mov	readparams+ 6+LB,#LOADSEG
	mov	readparams+ 8+LB,ax
	mov	readparams+10+LB,dx
	call	readcdrom
	xor	bx,bx
	mov	ax,LOADADR+0x00		! [00] = 01; [01] = 00 for 80x86
	cmp	ax,#0x0001
	jne	bootcatbad
	inc	bx
	mov	ax,LOADADR+0x1E		! [1E] = 55; [1F] = AA
	cmp	ax,#0xAA55
	jne	bootcatbad
	inc	bx
	mov	ax,LOADADR+0x20		! [20] = 88 bootable; [21] = 00 no emulation
	cmp	ax,#0x0088
	jne	bootcatbad
	inc	bx
	mov	ax,LOADADR+0x22		! [22] = 0 load segment = default
	cmp	ax,#0
	je	bootcatsegok
	cmp	ax,#0x7C0		!        or 7C0, same difference
	jne	bootcatbad
bootcatsegok:
	inc	bx
	mov	ax,LOADADR+0x26		! [26] = 4 sector count
	cmp	ax,#4
	jne	bootcatbad
	inc	bx
	mov	si,#LOADADR		! checksum should be zero
	xor	dx,dx
	mov	cx,#16
bootcatcksm:
	add	dx,(si)
	add	si,#2
	loop	bootcatcksm
	test	dx,dx
	je	bootcatok
bootcatbad:
	push	ax
	push	bx
	call	print_msgi
		.ascii	"-bad boot catalog, code "
		.byte	0
	pop	dx
	call	print_hexb
	call	print_msgi
		.ascii	" "
		.byte	0
	pop	dx
	call	print_hex
bootcathang:
	jmp	bootcathang
bootcatok:
!
!  Read starting with the preloader into 0x8000
!  We have no way of knowing the size, so read up to the memory hole
!  The readcdrom routine will chop us off at end of CD volume if need be
!  We only read 62K at a time because of shitbox BIOS's
!
	mov	ax,LOADADR+0x28		! get this boot block's lbn
	mov	dx,LOADADR+0x2A
	add	ax,#1			! start reading from next block
	adc	dx,#0
	mov	startlbn+0+LB,ax
	mov	startlbn+2+LB,dx
	add	ax,#2			! (the param page starts 2 blocks after the preloader)
	adc	dx,#0
	mov	paramlbn+0+LB,ax
	mov	paramlbn+2+LB,dx
	mov	ax,#LOADSEG		! set segment to start putting preloader at
	mov	segment+LB,ax
readloop:
	mov	ax,startlbn+0+LB	! get block to read
	mov	dx,startlbn+2+LB
	mov	cx,segment+LB		! get segment number
	mov	readparams+ 2+LB,#0x1F
	mov	readparams+ 4+LB,#0
	mov	readparams+ 6+LB,cx
	mov	readparams+ 8+LB,ax
	mov	readparams+10+LB,dx
	add	ax,#0x1F		! increment block for next time
	adc	dx,#0
	add	cx,#BLOCKSIZE/16*0x1F	! increment segment for next time
	mov	startlbn+0+LB,ax
	mov	startlbn+2+LB,dx
	mov	segment+LB,cx
	call	readcdrom		! read block from CD
	mov	cx,segment+LB		! get where next one starts
	add	cx,#BLOCKSIZE/16*0x1F	! see where it would end
	sub	cx,#0x9FFF		! see if we've reached the hole
	jc	readloop		! repeat if not

	mov	ax,startlbn+0+LB	! get block to read
	mov	dx,startlbn+2+LB
	mov	cx,segment+LB		! get segment number
	mov	bx,#0x9FFF		! see how many segments left to the hole
	sub	bx,cx
	jc	readdone
	shr	bx,#L2BLOCKSIZE-4	! convert to a block count
	je	readdone
	mov	readparams+ 2+LB,bx	! read up to the hole
	mov	readparams+ 4+LB,#0
	mov	readparams+ 6+LB,cx
	mov	readparams+ 8+LB,ax
	mov	readparams+10+LB,dx
	call	readcdrom
readdone:
!
! Print a load complete message
!
	call	print_msgi
		.byte	13,10
		.ascii	" done"
		.byte	0

!
! Dump out 1st 16 bytes
!
	mov	si,#LOADADR
	mov	di,#16
dump_loop:
	mov	ax,#0x0E20		! output a space
	xor	bx,bx
	int	0x10
	mov	dl,(si)			! output a byte of data
	inc	si
	call	print_hexb
	dec	di
	jne	dump_loop
dump_done:
!
! Jump to the 16-bit pre-loader
!
! All it assumes we do is pass drivetype in ax and driveno in dl,
! set the cs to LOADSEG (and thus the IP is zero)
!
	mov	dl,driveno+LB	! get driveno in dl for the pre-loader
	mov	ecx,paramlbn+LB	! get param page starting lbn
	mov	al,#'C		! tell it this is a CDROM
	mov	ah,#'D
	jmpi	0,LOADSEG	! jump to pre-loader

!
!  Read cdrom given readparams
!
!    readparams+2.W = number of 2K blocks, .le. 0xFF
!    readparams+4.W = memory offset
!    readparams+6.W = memory segment
!    readparams+8.Q = starting logical block number
!
!    size is truncated to not read off end of volume
!
readcdrom:
	mov	ax,volsize+0+LB		! get size of volume
	mov	dx,volsize+2+LB
	sub	ax,readparams+ 8+LB	! subtract starting LBN
	sbb	dx,readparams+10+LB
	jc	readcdromrtn		! if we start after volume size, nothing to read
	test	dx,dx			! if more than 64K block to end of volume, size we got is ok
	jne	readcdromsizeok
	sub	ax,readparams+2+LB	! see if we have enough blocks to end of volume
	jnc	readcdromsizeok
	add	ax,readparams+2+LB	! if not, read just to end of volume
	mov	readparams+2+LB,ax
readcdromsizeok:
	call	print_msgi		! print number of blocks
		.byte	13,10
		.ascii	" read "
		.byte	0
	mov	dl,readparams+2+LB
	call	print_hexb
	call	print_msgi		! print starting block number
		.ascii	" blks at "
		.byte	0
	mov	dx,readparams+10+LB
	call	print_hex
	mov	dx,readparams+ 8+LB
	call	print_hex
	call	print_msgi		! print memory segment
		.ascii	" into seg "
		.byte	0
	mov	dx,readparams+6+LB
	call	print_hex
	call	print_msgi		! print memory offset
		.ascii	" ofs "
		.byte	0
	mov	dx,readparams+4+LB
	call	print_hex

	mov	al,#0x69		! dummy so we know if error code is good
	mov	ah,#0x42		! function to read from CDROM
	mov	dl,driveno+LB		! cdrom drive number
	mov	si,#readparams+LB	! point to parameter block
	int	0x13			! read from CDROM
	jc	readerr
readcdromrtn:
	ret
readerr:
	push	ax			! print error code
	call	print_msgi
		.ascii	"-read error "
		.byte	0
	pop	dx
	call	print_hex
readhang:
	jmp	readhang

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
!!	mov	ax,#0x0E20		! print a space
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
!	none
!
print_msgi:
	pusha				! save all registers
	mov	di,sp			! point to stack
	mov	si,16(di)		! si = return address = message pointer
	call	print_msg		! print it out
	mov	16(di),si		! save new return address (following the null byte)
	popa				! restore all registers
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
	mov	ah,#0x0E		! put 0x0E in top = print char
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
	mov	ax,#0x0E0F	! ah = request, al = mask for nybble
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
!!pause:
!!	push	bx
!!	push	ax
!!	mov	ax,#0xe3e		! output a '>'
!!	xor	bx,bx
!!	int	0x10
!!	xor	ax,ax			! read a keystroke
!!	int	0x16
!!	pop	ax
!!	pop	bx
!!	ret

		.align	4

driveno:	.long	0		! drive number of CDROM being booted
paramlbn:	.long	0		! param page starting lbn
startlbn:	.long	0		! this block lbn
volsize:	.long	32		! size of volume in 2K blocks
					! - init big enough to read volume descriptor blocks
segment:	.long	0		! segment to read into next

readparams:	.byte	0x10		! 00: size of parameter block
		.byte	0
		.word	0		! 02: number of blocks to read (max 0xFF)
		.word	0,0		! 04: where to read it (offset,segment)
		.long	0,0		! 08: starting block number (quadword)

privoldes:	.byte	1		! - primary volume descriptor
		.ascii	"CD001"		! - volume descriptor
		.byte	1		! - primary volume descr version
bootvoldes:	.byte	0		! - boot volume descriptor
		.ascii	"CD001"		! - volume descriptor
		.byte	1		! - boot volume descr version

		.org	2040		! pad out to a 2K block
		.ascii	"OZONE CD"
