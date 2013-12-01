##+++2003-11-23
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
##---2003-11-23

##########################################################################
##									##
##  String length routine						##
##									##
##    Input:								##
##									##
##	$16 = string address						##
##	$17 = maximum length allowed (strnlen only)			##
##									##
##    Output:								##
##									##
##	$0 = length, not including null					##
##									##
##########################################################################

	.set	noat
	.set	nomacro

	.text

	.p2align 4
	.globl	strlen
	.globl	strnlen
	.type	strlen,@function
	.type	strnlen,@function
strlen:
	subq	$31,  8,$17		# set up huge default length if none given
strnlen:
	mov	$17,$0			# save original length
	mov	$16,$1			# save original address
	beq	$17,notfound		# if null string, didn't find terminator, return 0

	and	$16,  7,$19		# get offset in first quad to start at
	bic	$16,  7,$16		# quad align input pointer

	addq	$17,$19,$28		# see if scan limited to first quad
	ldq	$25,0($16)		# (read in first quad)
	cmpule	$28,  8,$28
	mov	  1,$20			# (needed for masking)
	blbs	$28,smallscan

	sll	$20,$19,$20		# multiple quads to scan, set up mask for the ingore bits, if any
	subq	$17,  8,$17		# we will have this much less to do
	subq	$20,  1,$20
	addq	$17,$19,$17		# (well, ok, this much less to do)
	cmpbge	$31,$25,$28		# check for zeroes
	bic	$28,$20,$28		# clear out the bits we don't care about, if any
	bne	$28,foundzero
	addq	$16,  8,$16		# none seen, increment to next quad

fullquads:
	cmpult	$17,  8,$28		# see if there is a full quad left to check
	blbs	$28,fullquadsdone
	.align	4
fullquadsloop:
	ldq	$25,0($16)		# if so, read it
	subq	$17,  8,$17		# this many less bytes to scan after
	cmpbge	$31,$25,$28		# check it
	cmpult	$17,  8,$20		# see if we will have another full quad to check
	bne	$28,foundzero		# if zero found, we're done scanning
	addq	$16,  8,$16		# no zero, increment pointer for next loop
	blbc	$20,fullquadsloop	# repeat if another full quad to check
	nop
fullquadsdone:
	beq	$17,notfound		# if nothing left to scan, it's not found
	ldq	$25,0($16)		# ok, get last partial quad
	mov	  1,$20			# ... but we only care about 'length' data bytes
	sll	$20,$17,$20
	subq	$20,  1,$20
	cmpbge	$31,$25,$28		# see if any zero bytes therein
	and	$28,$20,$28		# mask off bits we don't care about
	bne	$28,foundzero
notfound:
	ret	$31,($26)		# not found, return original given length

	## We just have to scan within the first quad

smallscan:
	sll	$20,$17,$20		# make mask for bits of cmpbge result we care about
	subq	$20,  1,$20
	sll	$20,$19,$20
	cmpbge	$31,$25,$28		# see if a zero byte found within
	and	$28,$20,$28		# ... that we care about
	bne	$28,foundzero
	ret	$31,($26)

## $1  = original string start address
## $16 = beginning of quad that has a zero byte
## $28 = result of cmpbge (known to have at least one non-zero bit in <07:00>)

	.align	4
foundzero:
	subq	$16, $1,$0		# calc length to beginning of quad

				# we know that one of the low 8 bits of $28 is set indicating the zero terminator

	and	$28, 15,$25		# see if low 4 bits indicate the first zero data byte
	srl	$28,  4,$20		# speculate that they don't and shift out the 4 low bits
	addq	 $0,  4,$1		# speculate that they don't and prepare to assume string is 4 bytes longer
	cmoveq	$25,$20,$28		# if no zero data byte in low 4 bytes, shift out the 4 bits
	cmoveq	$25, $1,$0		# if no zero data byte in low 4 bytes, increment length by 4

				# we know that one of the low 4 bits of $28 is set indicating the zero terminator

	and	$28,  3,$25		# see if low 2 bits indicate the first zero data byte
	srl	$28,  2,$20		# speculate that they don't and shift out the 2 low bits
	addq	 $0,  2,$1		# speculate that they don't and prepare to assume string is 2 bytes longer
	cmoveq	$25,$20,$28		# if no zero data byte in low 2 bytes, shift out the 4 bits
	cmoveq	$25, $1,$0		# if no zero data byte in low 2 bytes, increment length by 2

				# we know that one of the low 2 bits of $28 is set indicating the zero terminator

	and	$28,  1,$25		# see if low bit indicates the first zero data byte
	addq	 $0,  1,$1		# speculate that it doesn't and prepare to assume string is 1 byte longer
	cmoveq	$25, $1,$0		# if no zero data byte in low byte, increment length by 1

	ret	$31,($26)
