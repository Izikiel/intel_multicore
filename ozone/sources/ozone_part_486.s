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
!  This partition boot block is loaded into 7C00 by the BIOS
!
!  It does the following:
!
!  Sets all the segment registers to 07c0 since everything here thinks it is based at location 0
!  Moves itself to 6000
!  Sets all the segment registers to 0600
!  Scans the partition table contained herein for an 'active' partition
!  Reads the first block of the partition into 7C00
!  Jumps to it
!

BOOTSEG   = 0x07C0	! original segment we're loaded in
MOVESEG   = 0x0600	! segment to move this block to
STACKSIZE = 0x0400	! stack size in bytes

bootblsig = 0x1DFE	! where boot block signature word ends up
			! = (BOOTSEG-MOVESEG)*16+BLOCKSIZE-2

BLOCKSIZE = 512

	.text
	.org	0

	.globl	_main
_main:
	jmpi	go7C-_main,BOOTSEG	! set code segment register to BOOTSEG
go7C:
	mov	ax,#BOOTSEG		! set up BOOTSEG segment number
	mov	ds,ax			! put in ds
	mov	ss,ax			! put in ss
	mov	sp,#STACKSIZE		! put stack just below where loader goes
	mov	driveno,dl		! save boot drive number
!
!  Copy this whole thing to 6000
!
	xor	ax,ax
	mov	bx,#MOVESEG
	mov	cx,#BLOCKSIZE/2
	mov	si,ax
	mov	di,ax
	mov	es,bx
	cld
	rep
	movs
!
!  Now switch all segment registers to 0600
!
	jmpi	go60-_main,MOVESEG	! set code segment register to MOVESEG
go60:
	mov	ds,bx			! put MOVESEG in ds
	mov	ss,bx			! put MOVESEG in ss
!
!  Scan for the active partition table entry
!
	xor	cx,cx
	lea	si,partitions
scanloop:
	bt	(si),#7			! if this bit is set, it's active
	jc	scandone
	inc	cx
	add	si,#16
	cmp	cx,#4
	jne	scanloop
	jmp	promptkb		! none found, go prompt
!
!  We found an active partition, print the digit then wait a couple seconds for keyboard override
!
scandone:
	push	cx			! save parition number
	mov	ax,#0x0e31		! print out the digit found
	add	al,cl
	xor	bx,bx
	int	0x10
	mov	ah,#0			! read current time
	int	0x1a			! (18.206Hz counter to cx:dx)
	mov	ax,dx			! save low-order count
scanreadl:
	push	ax
	mov	ah,#1			! see if any keyboard char
	int	0x16
	jne	readkb			! ok, go process it
	mov	ah,#0			! no, read timer again
	int	0x1a
	pop	ax			! subtract saved count
	sub	dx,ax
	sub	dx,#54			! check for 3 seconds (3*18.206)
	jc	scanreadl		! if not up yet, keep looping
	pop	cx			! get table index of active partition
	jmp	selected		! go boot it
!
!  Read partition number from keyboard
!
promptkb:
	call	print_msgi
		.byte	13,10
		.ascii	"Partition?"
		.byte	0
readkb:
	mov	ah,#0			! read char into al, wait if nothing there
	int	0x16
	sub	al,#0x31		! must be digit 1..4
	test	al,#252
	je	readkbok
printnames:
	xor	cx,cx			! if not, print partition names
	lea	si,partnames
readprint:
	call	print_msgi		! cr/lf/space/space
		.byte	13,10
		.ascii	"  "
		.byte	0
	mov	ax,cx			! partition number digit
	add	ax,#0x0e31
	xor	bx,bx
	int	0x10
	mov	ax,#0x0e20		! a space
	xor	bx,bx
	int	0x10
	push	si			! partition name
	call	print_msg
	pop	si
	inc	cx			! increment partion number
	add	si,#8			! ... and name string pointer
	cmp	cl,#4
	jne	readprint
	jmp	promptkb
printnames2: jmp printnames
readkbok:
	xor	ah,ah			! save partition number
	push	ax
	add	ax,#0x0e31		! print it out
	xor	bx,bx
	int	0x10
	pop	cx			! get parittion number in cx
!
!  Boot partition number cx
!
selected:
	call	print_msgi		! print a space
		.byte	32,0
	add	cx,cx			! print partition name
	add	cx,cx
	add	cx,cx
	lea	si,partnames
	add	si,cx
	call	print_msg
	add	cx,cx			! point to partition table entry
	lea	si,partitions
	add	si,cx
!
!  Read the selected partition's boot block into BOOTSEG
!
	mov	dl,driveno		! get disk geometry from BIOS
	mov	ah,#8
	int	0x13
	jnc	driveprmok
	call	print_msgi
		.byte	13,10
		.ascii	"int 13.8 err"
		.byte	0
hang1:	jmp	hang1
printnames3: jmp printnames2
driveprmok:
					! CH = number of cylinders <0:7>
					! CL<0:5> = sectors per track
					! CL<6:7> = number of cyls <8:9>
					! DH = tracks per cylinder - 1
					! DL = number of drives
	and	cl,#0x3f		! mask out high cyl number bits
!
! Determine cylinder, track and sector to read
!
	mov	al,dh			! calculate number of sectors per cylinder
	mul	cl			! (calculate ax = al * cl)
	add	al,cl			! add one of these to result because we used tpc-1
	adc	ah,#0			! ... to get sectors per cylinder
	mov	bx,ax			! save result in bx
	mov	ax, 8(si)		! get starting block number in dx:ax
	mov	dx,10(si)
	div	bx			! divide block number by sectors per cylinder to get cylinder number
					! ax = quotient  = cylinder number
					! dx = remainder = sector within cylinder
	mov	bx,ax			! save cylinder number in bx
	mov	ax,dx			! put sector within cylinder in ax
	div	cl			! divide sector within cylinder by sectors per track to get track number within cylinder
					! al = quotient  = track number in cylinder
					! ah = remainder = sector number in track
	mov	dh,al			! move track number where it will be needed for the int 13 call
!
! ah = sector number within the track (0..62)
! bx = cylinder number (0..1023)
! cl = sectors per track (1..63)
! dh = track number within the cylinder (0..255)
!
! Now finally read the sector into memory
!
	mov	cl,bh			! get cylinder bits <8..15>
	shl	cl,#6			! put cylinder bits <8..9> in cl bits <6..7>
	or	cl,ah			! get sector number in cl <0..5> (assumes sector < 63)
	inc	cl			! offset sector number by 1
	mov	ch,bl			! get clyinder bits <0..7> in ch bits <0..7>
					! track number already in dh <0..7> (assumes track < 256)
	mov	dl,driveno		! get drive number in dl
	mov	ax,#0x0201		! get read function code in ah, one sector in al
	mov	bx,#BOOTSEG		! where to read into
	mov	es,bx
	xor	bx,bx			! zero offset in es segment
	mov	bootblsig,bx		! (clear signature word)
	int	0x13			! read disk
	jnc	read_ok
	call	print_msgi
		.ascii	" read error"
		.byte	13,10,0
printnames4: jmp printnames3
read_ok:
	mov	dx,bootblsig		! make sure it ends with AA55
	cmp	dx,#0xAA55
	je	bootblkok
	call	print_msgi
		.ascii	" not bootable"
		.byte	0
	jmp	printnames4
bootblkok:
	call	print_msgi
		.ascii	" ok"
		.byte	13,10,0
	mov	dl,driveno		! restore drive number
	jmpi	0,BOOTSEG		! jump to it

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
	test	al,al			! test al
	je	print_msg_done		! done if zero
	mov	ah,#0x0e		! put 0x0e in top = print char
	xor	bx,bx			! clear page number
	int	0x10			! print it
	jmp	print_msg		! try for more
print_msg_done:
	ret

	.org	_main+512-2-64-32-1
!
driveno:	.byte	0		! drive number as given in dl on entry		!! 413
!
! Partition names (up to 7 chars followed by a null)
!
partnames:	.long	0,0								!! 414
		.long	0,0
		.long	0,0
		.long	0,0
!
! This is the partition table itself
!
partitions:
partition1:	.byte	0x80		! 80 = bootable, 00 = not bootable		!! 446
		.byte	0x01		! NOT USED - beginning track
		.byte	0x01		! NOT USED - beginning sector
		.byte	0x00		! NOT USED - beginning cylinder
		.byte	0x83		! filesystem type id
		.byte	0xFE		! NOT USED - ending track
		.byte	0x3F		! NOT USED - ending sector
		.byte	0x3F		! NOT USED - ending cylinder
		.long	0x0000003F	! beginning block number
		.long	0x000FB001	! number of blocks
partition2:	.long	0,0,0,0								!! 462
partition3:	.long	0,0,0,0								!! 478
partition4:	.long	0,0,0,0								!! 494
!
! This number is required by the bios to be the last word in the bootblock
!
flagword:	.word	0xAA55		! magic number for bios				!! 510
