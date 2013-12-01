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
##  Memory set (fill) routine						##
##									##
##    Input:								##
##									##
##	R16 = destination address					##
##	R17 = fill byte							##
##	R18 = length							##
##									##
##    Output:								##
##									##
##	R0 = what R16 was on input					##
##									##
##########################################################################

	.set	noat
	.set	nomacro

	.text

	# r16 = dst	--> r0 = dst
	# r17 = fill byte
	# r18 = len

	.p2align 4
	.globl	memset
	.type	memset,@function
memset:
	mov	$16,$0			# set up return value (code below doesn't use r0 for anything)
	beq	$18,return		# nop if length is zero

	zapnot	$17,  1,$17		# make a full quad of fill data
	sll	$17,  8,$19
	or	$17,$19,$17
	sll	$17, 16,$19
	or	$17,$19,$17
	sll	$17, 32,$19
	or	$17,$19,$17

	and	$16,  7,$19		# get offset in first quad to start at
	s8addq	$19,$31,$20		# get bit number in first quad to start at
	bic	$16,  7,$16		# quad align dst addr

	## Special case if output restrained to a single quad so we can do single LDQ_L/STQ_C

	addq	$19,$18,$28		# see if length+offset .le. 8
	cmpule	$28,  8,$28
	blbc	$28,bigfill
	cmpeq	$18,  8,$28		# ok, see if we're doing the whole quad
	blbc	$28,tinyfill
	stq	$17,0($16)		# if so, write the whole quad
	ret	$31,($26)		# ... and that's it
tinyfill:
	mov	  1,$28			# partial quad, make mask of bytes to write
	sll	$28,$18,$28
	subq	$28,  1,$28
	sll	$28,$19,$28
	zapnot	$17,$28,$17		# mask out bytes not being written
tinyfilltry:
	ldq_l	$25,0($16)		# get the single quad
	zap	$25,$28,$25		# clear out bytes being written
	or	$25,$17,$25		# insert bytes being written
	stq_c	$25,0($16)		# store result back
	blbc	$25,tinyfillfail
	ret	$31,($26)		# all done

	## Fill the first partial quad

bigfill:
	beq	$19,fillquads		# maybe we start on a quad boundary
	mov	  1,$28			# if not, make mask of bits to preserve in dst quad
	sll	$28,$19,$28
	subq	$28,  1,$28
	zap	$17,$28,$21		# wipe junk bits from src quad
firstfilltry:
	ldq_l	$25,0($16)
	zapnot	$25,$28,$25
	or	$25,$21,$25
	stq_c	$25,0($16)
	blbc	$25,firstfillfail
	subq	$18,  8,$18		# this is how much is left to fill
	addq	$18,$19,$18
	addq	$16,  8,$16		# this is where to put next quad

	## Fill full quads

fillquads:
	cmpult	$18,  8,$28
	blbs	$28,fillquadsdone
fillquadsloop:
	subq	$18,  8,$18
	stq	$17,0($16)
	cmpult	$18,  8,$28
	addq	$16,  8,$16
	blbc	$28,fillquadsloop
fillquadsdone:

	## Fill partial quad on the end

	beq	$18,return		# maybe there's nothing left
	mov	  1,$28			# ok, make mask of bytes to be written
	sll	$28,$18,$28
	subq	$28,  1,$28
	zapnot	$17,$28,$17		# clear out bytes not being written
lastfilltry:
	ldq_l	$25,0($16)
	zap	$25,$28,$25
	or	$25,$17,$25
	stq_c	$25,0($16)
	blbc	$25,lastfillfail
return:
	ret	$31,($26)

	## LDQ_L/STQ_C failure branches

tinyfillfail:
	br	$31,tinyfilltry
firstfillfail:
	br	$31,firstfilltry
lastfillfail:
	br	$31,lastfilltry

