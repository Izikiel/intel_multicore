##+++2003-12-02
##    Copyright (C) 2001,2002,2003  Mike Rieker, Beverly, MA USA
##
##    This program is free software# you can redistribute it and/or modify
##    it under the terms of the GNU General Public License as published by
##    the Free Software Foundation# version 2 of the License.
##
##    This program is distributed in the hope that it will be useful,
##    but WITHOUT ANY WARRANTY# without even the implied warranty of
##    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
##    GNU General Public License for more details.
##
##    You should have received a copy of the GNU General Public License
##    along with this program# if not, write to the Free Software
##    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
##---2003-12-02

##########################################################################
##									##
##  MEMCHR - search for a character in a block of memory		##
##									##
##    Input:								##
##									##
##	$16 = points to block of memory					##
##	$17 = character to scan for					##
##	$18 = size of memory block					##
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
	.globl	memchr
	.type	memchr,@function
memchr:
	beq	$18,eqnone		# if zero length, not found
	and	$17,255,$17		# replicate search char throughout the quad
	and	$16,  7,$22		# get byte offset in first quad
	sll	$17,  8,$28
	or	$17,$28,$17
	sll	$17, 16,$28
	or	$17,$28,$17
	sll	$17, 32,$28
	ldq_u	$19,0($16)		# read first quad
	or	$17,$28,$17
	cmpbge	$19,$17,$20		# see which bytes .eq. search char
	cmpbge	$17,$19,$21
	and	$20,$21,$20
	mov	  8,$28			# calc how many bytes just checked
	srl	$20,$22,$20		# ignore low 'offset' bits
	subq	$28,$22,$22
	bne	$20,eqdone		# if any set, we're done
	cmpule	$18,$22,$28
	bic	$16,  7,$16		# quad align src pointer
	subq	$18,$22,$18		# subtract out bytes just checked
	blbs	$28,eqnone		# stop if no more to check
eqloop:
	ldq	$19,8($16)		# get next quad to check
	addq	$16,  8,$16		# point to the quad we just got
	cmpule	$18,  8,$28		# see if there will be more quads to check
	cmpbge	$19,$17,$20		# see which of its bytes .eq. search char
	cmpbge	$17,$19,$21
	and	$20,$21,$20
	bne	$20,eqdone		# if any set, we're done
	subq	$18,  8,$18		# not there, see if any more to check
	blbc	$28,eqloop
eqnone:
	mov	  0,$0			# character not found, return 0
	ret	$31,($26)
eqdone:
	addq	$20,$27,$20		# find where in quad first hit was
	ldbu	$20,lobitset-memchr($20)
	cmpult	$20,$18,$28		# see if hit or end of string is first
	addq	$16,$20,$0		# assume we got a hit
	cmoveq	$28,  0,$0		# but if terminator was first, return 0
	ret	$31,($26)

	.p2align 4
	.globl	memchrnot
	.type	memchrnot,@function
memchrnot:
	beq	$18,eqnone		# if zero length, not found
	and	$17,255,$17		# replicate search char throughout the quad
	and	$16,  7,$22		# get byte offset in first quad
	sll	$17,  8,$28
	or	$17,$28,$17
	sll	$17, 16,$28
	or	$17,$28,$17
	sll	$17, 32,$28
	ldq_u	$19,0($16)		# read first quad
	or	$17,$28,$17
	cmpbge	$19,$17,$20		# see which bytes .ne. search char
	cmpbge	$17,$19,$21
	and	$20,$21,$20
	xor	$20,255,$20
	mov	  8,$28			# calc how many bytes just checked
	srl	$20,$22,$20		# ignore low 'offset' bits
	subq	$28,$22,$22
	bne	$20,nedone		# if any set, we're done
	cmpule	$18,$22,$28
	bic	$16,  7,$16		# quad align src pointer
	subq	$18,$22,$18		# subtract out bytes just checked
	blbs	$28,nenone		# stop if no more to check
neloop:
	ldq	$19,8($16)		# get next quad to check
	addq	$16,  8,$16		# point to the quad we just got
	cmpule	$18,  8,$28		# see if there will be more quads to check
	cmpbge	$19,$17,$20		# see which of its bytes .ne. search char
	cmpbge	$17,$19,$21
	and	$20,$21,$20
	xor	$20,255,$20
	bne	$20,nedone		# if any set, we're done
	subq	$18,  8,$18		# not there, see if any more to check
	blbc	$28,neloop
nenone:
	mov	  0,$0			# character not found, return 0
	ret	$31,($26)
nedone:
	addq	$20,$27,$20		# find where in quad first hit was
	ldbu	$20,lobitset-memchrnot($20)
	cmpult	$20,$18,$28		# see if hit or end of string is first
	addq	$16,$20,$0		# assume we got a hit
	cmoveq	$28,  0,$0		# but if terminator was first, return 0
	ret	$31,($26)
