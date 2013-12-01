##+++2003-12-12
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
##---2003-12-12

##########################################################################
##									##
##  STRCHR - search for a character in a string				##
##									##
##    Input:								##
##									##
##	$16 = points to null-terminated string				##
##	$17 = character to scan for					##
##									##
##    Output:								##
##									##
##	$0 = 0 : char not found						##
##	  else : points to char in the $16 string			##
##									##
##########################################################################

	.set	noat
	.set	nomacro

	.text
	.p2align 6

	## Tell us which is the lowest bit set in a byte

lobitset:
	.byte	8,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0
	.byte	5,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0
	.byte	6,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0
	.byte	5,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0
	.byte	7,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0
	.byte	5,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0
	.byte	6,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0
	.byte	5,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0

	## Tell us which is the highest bit set in a byte

hibitset:
	.byte	8,0,1,1,2,2,2,2,3,3,3,3,3,3,3,3,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4
	.byte	5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5
	.byte	6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6
	.byte	6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6
	.byte	7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7
	.byte	7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7
	.byte	7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7
	.byte	7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7

	.p2align 4
	.globl	strchr
	.type	strchr,@function
x27:
strchr:
	and	$17,255,$17		# replicate search char throughout the quad
	and	$16,  7,$18		# get byte offset in first quad
	sll	$17,  8,$28
	or	$17,$28,$17
	sll	$17, 16,$28
	bic	$16,  7,$16		# quad align src pointer
	or	$17,$28,$17
	sll	$17, 32,$28
	ldq	$19,0($16)		# read first quad
	or	$17,$28,$17
	cmpbge	$31,$19,$21		# see which bytes .eq. terminator
	cmpbge	$19,$17,$20		# see which bytes .eq. search char
	cmpbge	$17,$19,$22
	and	$20,$22,$20
	srl	$20,$18,$20		# ignore low 'offset' bits
	srl	$21,$18,$21
	sll	$20,$18,$20
	sll	$21,$18,$21
	or	$20,$21,$22		# see if any set
	bne	$22,fdone		# if so, we're done
	.align	4
floop:
	ldq	$19,8($16)		# get next quad to check
	addq	$16,  8,$16		# point to the quad we just got
	cmpbge	$19,$17,$20		# see which of its bytes .eq. search char
	cmpbge	$17,$19,$21
	and	$20,$21,$20
	cmpbge	$31,$19,$21		# see which of its bytes .eq. null
	or	$20,$21,$22
	beq	$22,floop		# if none, repeat for next quad
fdone:
	addq	$20,$27,$20		# compare hit
	addq	$21,$27,$21		# null terminator
	ldbu	$20,lobitset-x27($20)	# find where in quad first compare hit was
	ldbu	$21,lobitset-x27($21)	# find where in quad first terminator was
	cmpule	$20,$21,$28		# see if hit or terminator was first
	addq	$16,$20,$0		# assume we got a hit
	cmoveq	$28,  0,$0		# but if terminator was first, return 0
	ret	$31,($26)

	.p2align 4
	.globl	strrchr
	.type	strrchr,@function
strrchr:
	lda	$27,x27-strrchr($27)

	and	$17,255,$17		# replicate search char throughout the quad
	and	$16,  7,$18		# get byte offset in first quad
	sll	$17,  8,$28
	mov	  0,$0			# assume we won't find it
	or	$17,$28,$17
	sll	$17, 16,$28
	bic	$16,  7,$16		# quad align src pointer
	or	$17,$28,$17
	sll	$17, 32,$28
	ldq	$19,0($16)		# read first quad
	or	$17,$28,$17
	cmpbge	$31,$19,$21		# see which bytes .eq. terminator
	cmpbge	$19,$17,$20		# see which bytes .eq. search char
	cmpbge	$17,$19,$22
	and	$20,$22,$20
	srl	$20,$18,$20		# ignore low 'offset' bits
	srl	$21,$18,$21
	sll	$20,$18,$20
	sll	$21,$18,$21
	or	$20,$21,$22		# see if any set
	bne	$22,rdone		# if so, we're done
	.align	4
rloop:
	ldq	$19,8($16)		# get next quad to check
	addq	$16,  8,$16		# point to the quad we just got
	cmpbge	$19,$17,$20		# see which of its bytes .eq. search char
	cmpbge	$17,$19,$21
	and	$20,$21,$20
	cmpbge	$31,$19,$21		# see which of its bytes .eq. null
	or	$20,$21,$22
	beq	$22,rloop		# if none, repeat for next quad
rdone:
	addq	$21,$27,$22		# find what is lowest terminator byte (if any) in the quad
	ldbu	$22,lobitset-x27($22)
	mov	255,$28			# mask off all that and higher bits in search hit mask
	sll	$28,$22,$22
	bic	$20,$22,$20
	addq	$20,$27,$22		# find highest hit (if any) in the quad below terminator
	ldbu	$22,hibitset-x27($22)
	addq	$16,$22,$1		# save pointer as our return value so far
	cmovne	$20, $1,$0		# ... assuming there was anything there
	beq	$21,rloop		# keep going if terminator not seen
	ret	$31,($26)
