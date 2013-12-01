##+++2004-01-03
##    Copyright (C) 2001,2002,2003,2004  Mike Rieker, Beverly, MA USA
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
##---2004-01-03

##########################################################################
##									##
##  String compare routines						##
##									##
##  This file must be prefixed by 					##
##  oz_crtl_memcmp/strcasecmp/strcmp_axp.s				##
##									##
##  Symbol IGNCASE = 0 : preserve case during compare			##
##                   1 : convert upper case to lower case on compare	##
##         NULLCHK = 0 : don't terminate on null			##
##                   1 : terminate compare on null			##
##									##
##########################################################################

	.set	noat
	.set	nomacro

## Convert RX to lower case
##   Input:
##	$2 = ^x2020202020202020 ('aaaaaaaa'^'AAAAAAAA')
##	$4 = ^x4141414141414141 ('AAAAAAAA')
##	$5 = ^x5A5A5A5A5A5A5A5A ('ZZZZZZZZ')
##	RX = data to test
##   Output:
##	$6,$7 = scratch
##	RX = possibly modified

.macro	lowcase	rx
.if IGNCASE
	cmpbge	\rx,$4,$6	# see what bytes are .ge. 'AAAAAAAA'
	cmpbge	$5,\rx,$7	# see what bytes are .le. 'ZZZZZZZZ'
	and	$6,$7,$6	# see what bytes are both
	zapnot	$2,$6,$7	# only change bytes that are both
	xor	\rx,$7,\rx	# change the bytes from upper to lower case
.endif
	.endm	lowcase

# This symbol must be the entrypoint to the routine
# and pointed to by R27 on entry

x27:
	beq	$18,equal

.if IGNCASE
	ldq	 $2,r2init-x27($27)
	ldq	 $4,r4init-x27($27)
	ldq	 $5,r5init-x27($27)
.endif

	mov	  1,$0		# assume s1 .gt. s2
	and	$16,  7,$20	# get offset of s1 in the quad
	and	$17,  7,$21	# get offset of s2 in the quad

	## If s1offs .lt. s2offs, flip the strings around

	cmpult	$20,$21,$28
	blbc	$28,s1offsges2offs
	subq	$31,  1,$0	# ok, assume s2 .gt. s1
	mov	$16,$28		# switch s1addr and s2addr
	mov	$17,$16
	mov	$28,$17
	and	$16,  7,$20	# recalc offsets
	and	$17,  7,$21
s1offsges2offs:

	## See if compare is confined to first quad of s1
	## (and therefore also first quad of s2 since s1offs .ge. s2offs)

	addq	$18,$20,$28	# number of bytes from beg of first quad to end of s1
	cmpule	$28,  8,$28	# see if .le. 8
	blbc	$28,bigcompare	# if not, big compare
	ldq_u	$22,0($16)	# get s1 quad
	ldq_u	$23,0($17)	# get s2 quad
	lowcase	$22
	lowcase	$23
	extql	$22,$20,$24	# shift out garbage bytes from bottom of s1 data
	extql	$23,$21,$25	# shift out garbage bytes from bottom of s2 data
	mov	255,$28		# chop them both off at length
	sll	$28,$18,$28
	zap	$24,$28,$24
	zap	$25,$28,$25
.if NULLCHK
	cmpbge	$31,$24,$28	# check for null terminator in s1 temp
	cmpbge	$31,$25,$19	# check for null terminator in s2 temp
	or	$19,$28,$19	# lowest in either will do
	addq	$19,$27,$19	# smear lowest set bit up through the top
	ldbu	$19,zerozaps-x27($19)
	sll	$19,  1,$19	# shift to leave actual terminator in place
				# ... and therefore the corresponding byte in other string
	zap	$24,$19,$24
	zap	$25,$19,$25
.endif
	cmpeq	$24,$25,$28
	blbc	$28,noteq
	mov	  0,$0
	ret	$31,($26)

	## Maybe the strings are mutually aligned

bigcompare:
	cmpeq	$20,$21,$28
	blbs	$28,aligned

	## Compare parts of first quads to get us on s1 quad boundary
	## We assume s1offs .gt. s2offs

	ldq_u	$22,0($16)		# get first quad of string 1
	addq	$16,  8,$16
	ldq_u	$23,0($17)		# get first quad of string 2
	addq	$17,  8,$17
	lowcase	$22
	lowcase	$23
	extql	$22,$20,$24		# shift to put LSB in bottom
	extql	$23,$21,$25
	mov	255,$1			# mask off extra stuff from string 2
	srl	 $1,$20,$1
	zapnot	$25, $1,$25
.if NULLCHK
	cmpbge	$31,$24,$28		# check for nulls in s1temp
	cmpbge	$31,$25,$19		# check for nulls in s2temp
	or	$19,$28,$19
	and	$19, $1,$19		#   but only up to what we got
	addq	$19,$27,$19		# smear lowest set bit up to top
	ldbu	$19,zerozaps-x27($19)
	sll	$19,  1,$19
	zap	$24,$19,$24		# chop both quads off just after earliest one
	zap	$25,$19,$25		# ... to leave corresponding byte in other one
.endif
	cmpeq	$24,$25,$28		# stop if not equal
	blbc	$28,noteq
.if NULLCHK
	bne	$19,equal		# stop if null terminated
.endif
	subq	$18,  8,$18		# calc how many bytes left of s1 to process
	addq	$18,$20,$18

	## s1 is on a quad boundary
	## s2quad has s1offs-s2offs uncompared bytes in the top
	## length = number of uncompared s1 bytes

	addq	$21,  8,$1		# calc offset to uncompared bytes in s2quad
	subq	 $1,$20,$1
	cmpult	$18,  8,$28		# see if there's a whole quad of s1 left
	blbs	$28,unaligndone
unalignloop:
	ldq_u	$24,0($16)		# get an s1 quad
	addq	$16,  8,$16
	extql	$23, $1,$25		# shift old s2quad stuff down
	ldq_u	$23,0($17)		# get a new quad from s2
	addq	$17,  8,$17
	lowcase	$24
	lowcase	$23
	extqh	$23, $1,$28		# shift new s2 stuff up and merge with old s2 stuff
	or	$25,$28,$25
.if NULLCHK
	cmpbge	$31,$24,$28		# check for nulls in either quad
	cmpbge	$31,$25,$19
	or	$19,$28,$19
	addq	$19,$27,$19
	ldbu	$19,zerozaps-x27($19)
	sll	$19,  1,$19		# chop both quads off just after earliest one
	zap	$24,$19,$24		# ... so we compare the null to the byte in other string
	zap	$25,$19,$25
.endif
	cmpeq	$24,$25,$28		# see if equal
	blbc	$28,noteq		# stop comparing if not
.if NULLCHK
	bne	$19,equal		# stop if null terminator
.endif
	subq	$18,  8,$18		# we processed a quad of s1
	cmpult	$18,  8,$28		# repeat if another quad of s1 to do
	blbc	$28,unalignloop
unaligndone:

	## Process any last s1 bytes

	beq	$18,equal		# string equal if we hit the end
	ldq_u	$22,0($16)		# get last s1 quad
	lowcase	$22
	mskql	$22,$18,$24		# mask out junk past the length
	extql	$23, $1,$25		# shift s2quad stuff down
	subq	$20,$21,$28		# see if we need another s2 quad
	cmpule	$18,$28,$28
	blbs	$28,gotlastquad
	ldq_u	$23,0($17)		# ok, get the last s1 quad
	lowcase	$23
	extqh	$23, $1,$28		# shift new s2 stuff up and merge with old s2 stuff
	or	$25,$28,$25
gotlastquad:
	mskql	$25,$18,$25		# mask out junk past the length
.if NULLCHK
	cmpbge	$31,$24,$28		# check for nulls in either quad
	cmpbge	$31,$25,$19
	or	$19,$28,$19
	addq	$19,$27,$19		# chop both quads off just after earliest one
	ldbu	$19,zerozaps-x27($19)
	sll	$19,  1,$19
	zap	$24,$19,$24
	zap	$25,$19,$25
.endif
	cmpeq	$24,$25,$28		# compare
	blbc	$28,noteq
	mov	  0,$0
	ret	$31,($26)

	## The strings are mutually aligned

aligned:
	beq	$20,alignedquad
	ldq_u	$24,0($16)
	addq	$16,  8,$16
	ldq_u	$25,0($17)
	addq	$17,  8,$17
	lowcase	$24
	lowcase	$25
	extql	$24,$20,$24
	extql	$25,$21,$25
.if NULLCHK
	cmpbge	$31,$24,$28		# check for nulls
	cmpbge	$31,$25,$19
	or	$19,$28,$19
	mov	255,$28			# but only up to what we got
	srl	$28,$20,$28
	and	$19,$28,$19
	addq	$19,$27,$19
	ldbu	$19,zerozaps-x27($19)
	sll	$19,  1,$19		# chop both quads off just after earliest one
	zap	$24,$19,$24
	zap	$25,$19,$25
.endif
	cmpeq	$24,$25,$28		# compare data quads
	blbc	$28,noteq		# stop comparing if .ne.
.if NULLCHK
	bne	$19,equal		# stop comparing if hit terminator
.endif
	subq	$18,  8,$18		# subtract off how many bytes are done
	addq	$18,$20,$18
alignedquad:

	## Both s1 and s2 are quad aligned

	cmpult	$18,  8,$28
	blbs	$28,aligneddone
alignedloop:
	ldq_u	$24,0($16)
	addq	$16,  8,$16
	ldq_u	$25,0($17)
	addq	$17,  8,$17
	lowcase	$24
	lowcase	$25
.if NULLCHK
	cmpbge	$31,$24,$28		# check for null terminator in both quads
	cmpbge	$31,$25,$19
	or	$19,$28,$19
	addq	$19,$27,$19
	ldbu	$19,zerozaps-x27($19)
	sll	$19,  1,$19		# chop both quads off just after earliest null
	zap	$24,$19,$24
	zap	$25,$19,$25
.endif
	cmpeq	$24,$25,$28
	blbc	$28,noteq
.if NULLCHK
	bne	$19,equal
.endif
	subq	$18,  8,$18		# a quad has been compared
	cmpult	$18,  8,$28		# see if there is another quad to compare
	blbc	$28,alignedloop
aligneddone:

	## Compare any left over bytes

	beq	$18,equal
	ldq_u	$22,0($16)		# ok, read the last quads
	ldq_u	$23,0($17)
	lowcase	$22
	lowcase	$23
	mskql	$22,$18,$24		# chop them off at length
	mskql	$23,$18,$25
.if NULLCHK
	cmpbge	$31,$24,$28		# check for nulls in either quad
	cmpbge	$31,$25,$19
	or	$19,$28,$19
	addq	$19,$27,$19		# chop both quads off just after earliest one
	ldbu	$19,zerozaps-x27($19)
	sll	$19,  1,$19
	zap	$24,$19,$24
	zap	$25,$19,$25
.endif
	cmpeq	$24,$25,$28		# compare
	blbc	$28,noteq
equal:
	mov	  0,$0
	ret	$31,($26)

	## The strings aren't equal
	## $24,$25 contain the unequal quads
	## Return $0 as is if s1>s2, else return -$0

noteq:
	cmpbge	$24,$25,$20		# clears bit for each byte of s1 .lt. s2
	cmpbge	$25,$24,$21		# clears bit for each byte of s2 .lt. s1
	addq	$20,$27,$20		# first byte of s1 that's .lt. s2
	ldbu	$20,lowbitclr-x27($20)
	addq	$21,$27,$21		# first byte of s2 that's .lt. s1
	ldbu	$21,lowbitclr-x27($21)
	subq	$31, $0,$1		# speculate that $20 .lt. $21
	cmpult	$20,$21,$28		# see if $20 .lt. $21
	cmovne	$28, $1,$0		# if so, return -$0
	ret	$31,($26)

	.p2align 6

## Smears the lowest set bit of the given byte up through the top

zerozaps:
	.byte	0x00,0xFF,0xFE,0xFF,0xFC,0xFF,0xFE,0xFF,0xF8,0xFF,0xFE,0xFF,0xFC,0xFF,0xFE,0xFF
	.byte	0xF0,0xFF,0xFE,0xFF,0xFC,0xFF,0xFE,0xFF,0xF8,0xFF,0xFE,0xFF,0xFC,0xFF,0xFE,0xFF
	.byte	0xE0,0xFF,0xFE,0xFF,0xFC,0xFF,0xFE,0xFF,0xF8,0xFF,0xFE,0xFF,0xFC,0xFF,0xFE,0xFF
	.byte	0xF0,0xFF,0xFE,0xFF,0xFC,0xFF,0xFE,0xFF,0xF8,0xFF,0xFE,0xFF,0xFC,0xFF,0xFE,0xFF
	.byte	0xC0,0xFF,0xFE,0xFF,0xFC,0xFF,0xFE,0xFF,0xF8,0xFF,0xFE,0xFF,0xFC,0xFF,0xFE,0xFF
	.byte	0xF0,0xFF,0xFE,0xFF,0xFC,0xFF,0xFE,0xFF,0xF8,0xFF,0xFE,0xFF,0xFC,0xFF,0xFE,0xFF
	.byte	0xE0,0xFF,0xFE,0xFF,0xFC,0xFF,0xFE,0xFF,0xF8,0xFF,0xFE,0xFF,0xFC,0xFF,0xFE,0xFF
	.byte	0xF0,0xFF,0xFE,0xFF,0xFC,0xFF,0xFE,0xFF,0xF8,0xFF,0xFE,0xFF,0xFC,0xFF,0xFE,0xFF
	.byte	0x80,0xFF,0xFE,0xFF,0xFC,0xFF,0xFE,0xFF,0xF8,0xFF,0xFE,0xFF,0xFC,0xFF,0xFE,0xFF
	.byte	0xF0,0xFF,0xFE,0xFF,0xFC,0xFF,0xFE,0xFF,0xF8,0xFF,0xFE,0xFF,0xFC,0xFF,0xFE,0xFF
	.byte	0xE0,0xFF,0xFE,0xFF,0xFC,0xFF,0xFE,0xFF,0xF8,0xFF,0xFE,0xFF,0xFC,0xFF,0xFE,0xFF
	.byte	0xF0,0xFF,0xFE,0xFF,0xFC,0xFF,0xFE,0xFF,0xF8,0xFF,0xFE,0xFF,0xFC,0xFF,0xFE,0xFF
	.byte	0xC0,0xFF,0xFE,0xFF,0xFC,0xFF,0xFE,0xFF,0xF8,0xFF,0xFE,0xFF,0xFC,0xFF,0xFE,0xFF
	.byte	0xF0,0xFF,0xFE,0xFF,0xFC,0xFF,0xFE,0xFF,0xF8,0xFF,0xFE,0xFF,0xFC,0xFF,0xFE,0xFF
	.byte	0xE0,0xFF,0xFE,0xFF,0xFC,0xFF,0xFE,0xFF,0xF8,0xFF,0xFE,0xFF,0xFC,0xFF,0xFE,0xFF
	.byte	0xF0,0xFF,0xFE,0xFF,0xFC,0xFF,0xFE,0xFF,0xF8,0xFF,0xFE,0xFF,0xFC,0xFF,0xFE,0xFF

## Given a byte, tells which is the lowest cleared bit

lowbitclr:
	.byte	0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,5
	.byte	0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,6
	.byte	0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,5
	.byte	0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,7
	.byte	0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,5
	.byte	0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,6
	.byte	0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,5
	.byte	0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,8

.if IGNCASE
r2init:	.quad	0x2020202020202020
r4init:	.quad	0x4141414141414141
r5init:	.quad	0x5A5A5A5A5A5A5A5A
.endif
