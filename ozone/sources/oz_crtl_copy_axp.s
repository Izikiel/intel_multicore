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
##  This is my attempt at an memcpy/memmove routine for the Alpha	##
##  It does the copy by quadwords					##
##  It uses atomic LDQ_L/STQ_C on the ends				##
##  It's about 3-5x faster than a byte-by-byte copy			##
##									##
##  The Dec routine takes about 80% of the time as this one		##
##  VMS's MAC/OPT doesn't seem to make a difference			##
##									##
##  This file must be prefixed by oz_crtl_memcpy/strcpy_axp.s		##
##									##
##  Symbol NULLCHK = 0 : don't terminate on null			##
##                   1 : terminate copy on null				##
##									##
##    Input:								##
##									##
##	$16 = destination address					##
##	$17 = source address						##
##	$18 = length							##
##	$26 = return address						##
##									##
##    Output:								##
##									##
##	$0 = original destination address				##
##	$19..$23,$28 = scratch						##
##									##
##########################################################################

	.set	noat
	.set	nomacro

## EXTQH: shift left by 8-rb bytes
## EXTQL: shift right by rb bytes
## INSQH: shift right by 8-rb bytes
## INSQL: shift left by rb bytes
## MSKQL: leave low rb bytes intact, clear the upper
## MSKQH: clear low rb bytes, leave the upper intact

	## $19 = dstoffs
	## $20 = srcoffs
	## $21 = srcquad1
	## $22 = srcquad2
	## $23 = dstquad

	## $28 = temp

x27:
	mov	$16,$0			# set up return value = destination address
	cmpeq	$16,$17,$28		# see if srcaddr .eq. dstaddr
	beq	$18,f_return		# we're a nop if length .eq. zero
	blbs	$28,f_return		# also nop if srcaddr .eq. dstaddr

	and	$16,  7,$19		# get destination alignment bits
	and	$17,  7,$20		# get source alignment bits

	## Special case if output restrained to a single quad so we can do single LDQ_L/STQ_C
	## This works for any overlapped case as we do all reading before any writing

srccheck:
	addq	$19,$18,$28		# see if copy finishes by end of first dst quad
	cmpule	$28,  8,$28
	blbc	$28,mtonedstquad
	ldq_u	$21,0($17)		# ok, get first source quad
.if NULLCHK
	cmpbge	$31,$21,$24		# check for null terminator
	srl	$24,$20,$24		# we don't care about low 'srcoffs' bits
	beq	$24,nosrcterm
	addq	$24,$27,$24
	ldbu	$24,lowbitsetp1-x27($24)
	cmpult	$24,$18,$28		# if found, chop length off just after it
	cmovne	$28,$24,$18
nosrcterm:
.endif
	extql	$21,$20,$21		# remove useless stuff from bottom of srcquad
	addq	$20,$18,$28		# see if we need a second src quad
	cmpule	$28,  8,$28
	blbs	$28,justonesrcquad
	ldq_u	$22,8($17)		# if so, read it
	extqh	$22,$20,$22		#        shift it
	or	$21,$22,$21		#        merge it
.if NULLCHK
	cmpbge	$31,$21,$24		# check for null terminator
	addq	$24,$27,$24
	ldbu	$24,lowbitsetp1-x27($24)
	cmpult	$24,$18,$28		# if found, chop length off just after it
	cmovne	$28,$24,$18
.endif
justonesrcquad:
	cmpeq	$18,  8,$28		# see if we're writing whole quad
	blbs	$28,storeonlyquadwhole
	mov	  1,$28			# if not, make mask for bytes being transferred
	sll	$28,$18,$28
	subq	$28,  1,$28
	sll	$28,$19,$28
	insql	$21,$19,$21		#         shift src bits in place
	zapnot	$21,$28,$21		#         mask top junk bits off src
	bic	$16,  7,$16		#         quad align address
storeonlyquadtry:
	ldq_l	$23,0($16)		#         read existing quad
	zap	$23,$28,$23		#         mask off what we want to change
	or	$23,$21,$23		#         insert new bits
	stq_c	$23,0($16)		#         store it back
	blbc	$23,storeonlyquadfail
	ret	$31,($26)
storeonlyquadwhole:
	stq	$21,0($16)		# if so, write the whole quad
	ret	$31,($26)
mtonedstquad:

	## If srcaddr < dstaddr, copy from end to beginning

.if !NULLCHK
	cmpult	$17,$16,$28
	blbs	$28,copybackwards
.endif

	## Special case if src and dst are equally aligned

	cmpeq	$19,$20,$28		# see if srcoffs .eq. dstoffs
	blbc	$28,f_notaligned
	beq	$19,f_alignedtest
	ldq_u	$21,0($17)		# ok, get first source quad
.if NULLCHK
	cmpbge	$31,$21,$24		#     check for null terminator
	srl	$24,$20,$24		#     we don't care about low 'srcoffs' bits
	beq	$24,nosrcterm2
	addq	$24,$27,$24
	ldbu	$24,lowbitsetp1-x27($24)
	cmpult	$24,$18,$28		#     if found, chop length off just after it
	cmovne	$28,$24,$18
	blbs	$28,srccheck		#     maybe we only do partial first quad now
nosrcterm2:
.endif
	addq	$17,  8,$17		#     increment past it
	mskqh	$21,$20,$21		#     zap the junk bytes from bottom of src quad
	bic	$16,  7,$16		#     quad align dstaddr for LDQ_L/STQ_C
f_oddalignedtry:
	ldq_l	$23,0($16)		#     get existing dst quad
	mskql	$23,$20,$23		#     zap out the bytes we want to write
	or	$23,$21,$23		#     insert the bytes we want to write
	stq_c	$23,0($16)		#     store back modified value
	blbc	$23,f_oddalignedfail
	addq	$18,$20,$18		# subtract bytes just done from length
	subq	$18,  8,$18
	addq	$16,  8,$16		# this is where to store next dst quad (aligned)
f_alignedtest:
	cmpult	$18,  8,$28		# see if there is at least one full quad left to copy
	blbs	$28,f_aligneddone
	.align	4
f_alignedloop:
	ldq_u	$23,0($17)		# get a quad from source
	addq	$17,  8,$17		# increment address for next quad
.if NULLCHK
	cmpbge	$31,$23,$24		# check for null terminator
	bne	$24,f_alignedterm
.endif
	subq	$18,  8,$18		# decrement remaining length
	stq	$23,0($16)		# store quad in destination
	cmpult	$18,  8,$28		# see if there are more full quads to do
	addq	$16,  8,$16		# increment address for next quad
	blbc	$28,f_alignedloop
	.align	4
f_aligneddone:
	beq	$18,f_alignedret	# see if there is a partial quad at the end
	ldq_u	$21,0($17)		# if so, get last src quad
.if NULLCHK
	cmpbge	$31,$21,$24		# check for null terminator
	addq	$24,$27,$24
	ldbu	$24,lowbitsetp1-x27($24)
	cmpult	$24,$18,$28		# chop length just past it
	cmovne	$28,$24,$18
.endif
	mskql	$21,$18,$21		# clear out junk bytes above 'length'
f_alignedlasttry:
	ldq_l	$23,0($16)		# read existing quad
	mskqh	$23,$18,$23		# clear out bytes we want to write
	or	$23,$21,$23		# insert bytes we want to write
	stq_c	$23,0($16)		# store modified quad back out
	blbc	$23,f_alignedlastfail
f_alignedret:
	ret	$31,($26)
.if NULLCHK
f_alignedterm:
	addq	$24,$27,$24		# terminator found in loop
	ldbu	$18,lowbitsetp1-x27($24) # chop length off just after it
	cmpult	$18,  8,$28		# see if there still is one full quad left to copy
	mskql	$23,$18,$21		# clear out junk bytes above 'length'
	blbs	$28,f_alignedlasttry	# go back to store last partial quad (incl terminator)
	stq	$23,0($16)		# store last full quad (incl terminator)
	ret	$31,($26)
.endif
f_notaligned:

	## Copy data upto first dst quad aligned boundary, so we can use LDQ_L/STQ_C.  Leave remaining src bytes in srcquad1.

	ldq_u	$21,0($17)		# get first source quad
.if NULLCHK
	cmpbge	$31,$21,$24		# check for null terminator
	srl	$24,$20,$24		# ignore low 'srcoffs' bits
	beq	$24,f_nosrcterm4
	addq	$24,$27,$24
	ldbu	$24,lowbitsetp1-x27($24)
	cmpult	$24,$18,$28		# chop length off just past terminator
	cmovne	$28,$24,$18
	blbs	$28,srccheck		# maybe we only do one dst quad now
f_nosrcterm4:
.endif
	addq	$17,  8,$17		# increment past it
	beq	$19,f_dstisaligned
	cmpule	$20,$19,$28		# see if srcoffs > dstoffs
	blbs	$28,f_needonesrcquad
	mov	$21,$22			# if so, we need a second src quad to fill dst quad
	ldq_u	$21,0($17)
	extql	$22,$20,$22		#        shift first quad bits to get rid of junk
	extqh	$21,$20,$28		#        shift second quad bits in place
	addq	$17,  8,$17
	or	$22,$28,$22		#        merge in with first quad
.if NULLCHK
	cmpbge	$31,$22,$24		#        check for null terminator
	beq	$24,f_storefirstdstquad
	addq	$24,$27,$24
	ldbu	$24,lowbitsetp1-x27($24)
	cmpult	$24,$18,$28		#        chop length off just past terminator
	cmovne	$28,$24,$18
	subq	$17, 16,$17		#        (in case we br to srccheck, point to first src quad)
	blbs	$28,srccheck		#        maybe we only do one dst quad now
	addq	$17, 16,$17		#        (we didn't, so point to third src quad)
.endif
	br	$31,f_storefirstdstquad
f_needonesrcquad:
	extql	$21,$20,$22		# if not, use bytes from the one quad we have
	addq	$20,  8,$20
f_storefirstdstquad:
	subq	$20,$19,$20		# subtract dstoffs from srcoffs ...
					# so we know what of srcquad1 has yet to be copied
	insql	$22,$19,$22		# shift bytes into top of $22
	bic	$16,  7,$16		# quad align dst address
f_storefirstdstquadtry:
	ldq_l	$23,0($16)		# get existing dst quad
	mskql	$23,$19,$23		# mask out the bytes we want to write
	or	$23,$22,$23		# insert the bytes we want to write
	stq_c	$23,0($16)		# write the quad back
	blbc	$23,f_storefirstdstquadfail
	subq	$18,  8,$18		# adjust length for how much there is left to write
	addq	$16,  8,$16		# increment dst addr to next quad
	addq	$18,$19,$18
f_dstisaligned:

	## $18 length   = number of bytes yet to be written starting at dstaddr
	## $16 dstaddr  = aligned to quad boundary
	## $19 dstoffs  = we just don't care
	## $17 srcaddr  = where to fetch next src quad from (unaligned)
	## $20 srcoffs  = starting byte offset in srcquad1 to be stored in next dst quad
	## $21 srcquad1 = data left over that has yet to be written

	## Copy via shift and merge

.if NULLCHK
	cmpbge	$31,$21,$24		# see if null terminator in what's left over
	srl	$24,$20,$24
	beq	$24,f_srcunalignedtest
	addq	$24,$27,$24		# if so, chop length off just after it
	ldbu	$24,lowbitsetp1-x27($24)
	cmpult	$24,$18,$28
	cmovne	$28,$24,$18
f_srcunalignedtest:
.endif
	cmpult	$18,  8,$28		# see if there is a whole quad to write
	blbs	$28,f_srcunaligneddone
	.align	4
f_srcunalignedloop:
	ldq_u	$22,0($17)		# read next source quad for top bytes
	addq	$17,  8,$17		# increment address for next read
.if NULLCHK
	cmpbge	$31,$22,$24		# check for null terminator therein
	bne	$24,f_srcunalignedterm	# break out of loop if found
f_srcunalignedmerge:
.endif
	extql	$21,$20,$23		# fill in bottom bytes
	extqh	$22,$20,$28		# fill in top bytes
	subq	$18,  8,$18		# there are 8 bytes less to do now
	or	$23,$28,$23		# merge together
	cmpult	$18,  8,$28		# see if there is another whole quad to write
	stq	$23,0($16)		# write it out
	addq	$16,  8,$16		# increment address for next write
	mov	$22,$21			# shift next src quad down for processing
	blbc	$28,f_srcunalignedloop
	.align	4
f_srcunaligneddone:

	## Finish off last dst quad (length in range 0..7 at this point)

	beq	$18,f_return		# all done if nothing more to do
.if NULLCHK
	cmpbge	$31,$21,$24		# check for null terminator
	srl	$24,$20,$24		# ignore low 'srcoffs' bytes
	beq	$24,f_lastnoterm
	addq	$24,$27,$24
	ldbu	$24,lowbitsetp1-x27($24)
	cmpult	$24,$18,$28		# chop length off just after terminator
	cmovne	$28,$24,$18
f_lastnoterm:
.endif
	addq	$18,$20,$28		# see if we need bytes from next source quad to finish last dst quad
	extql	$21,$20,$23		# get residual src bytes from copy loop
	cmpule	$28,  8,$28
	blbs	$28,f_gotwhatweneed
	ldq_u	$22,0($17)		# if so, read them
	extqh	$22,$20,$22		#        and merge them in
	or	$23,$22,$23
.if NULLCHK
	cmpbge	$31,$23,$24		# check for null terminator
	addq	$24,$27,$24
	ldbu	$24,lowbitsetp1-x27($24)
	cmpult	$24,$18,$28		# chop length off just after terminator
	cmovne	$28,$24,$18
.endif
f_gotwhatweneed:
	mskql	$23,$18,$23		# mask bytes to be written
f_storelastdsttry:
	ldq_l	$22,0($16)		# read last dst quad
	mskqh	$22,$18,$22		# zap out bytes we want to write
	or	$22,$23,$22		# insert bytes we want to write
	stq_c	$22,0($16)		# store modified value
	blbc	$22,f_storelastdstfail
f_return:
	ret	$31,($26)

	## Hit terminator in loop, finish off the last parts in $21 and $22

.if NULLCHK
f_srcunalignedterm:
	addq	$24,$27,$24		# found terminator in loop
	ldbu	$24,lowbitsetp1-x27($24) # this is how many bytes in $22 to copy (incl terminator)
	addq	$24,  8,$24		# calculate new length yet to write incl terminator
	subq	$24,$20,$24		# includes good bytes still in top of $21
	cmpult	$24,$18,$28		# chop length off there
	cmovne	$28,$24,$18
	extql	$21,$20,$23		# get data to be written
	extqh	$22,$20,$28		# ... from both src quads
	or	$23,$28,$23
	cmpult	$18,  8,$28		# see if at least a quad to write
	blbs	$28,f_gotwhatweneed	# if not, write it out in last dst & return
	stq	$23,0($16)		# if so, write it out as is
	subq	$18,  8,$18		# one less quad to write
	addq	$16,  8,$16		# increment to next dst quad
	extql	$22,$20,$23		# extract what's left, if anything
	bne	$18,f_gotwhatweneed	# but maybe we're all done
	ret	$31,($26)
.endif

	######################################################
	## Copy Backwards - Same Stuff, Different Direction ##
	######################################################

.if !NULLCHK

copybackwards:
	addq	$17,$18,$17		# point srcaddr past end of source data
	addq	$16,$18,$16		# point dstaddr past end of dst buffer
	and	$17,  7,$20		# recompute srcoffs
	and	$16,  7,$19		# recompute dstoffs

	## Special case if src and dst are equally aligned

	cmpeq	$20,$19,$28
	blbc	$28,b_notaligned
	beq	$20,b_aligned
	ldq_u	$21,0($17)		# read first src quad
	mskql	$21,$20,$21		# mask junk out of top of src quad
	bic	$16,  7,$16		# quad align address for LDQ_L/STQ_C
b_firstalignedtry:
	ldq_l	$23,0($16)		# read existing quad
	mskqh	$23,$20,$23		# clear out bytes in bottom we want to write
	or	$23,$21,$23		# put in bytes to be written
	stq_c	$23,0($16)
	blbc	$23,b_firstalignedfail
	subq	$18,$20,$18		# that many less bytes to do
b_aligned:
	cmpult	$18,  8,$28
	blbs	$28,b_aligneddone
	.align	4
b_alignedloop:
	ldq_u	$21,-8($17)		# copy as many full quads as we can
	subq	$18,  8,$18
	subq	$16,  8,$16
	cmpult	$18,  8,$28
	subq	$17,  8,$17
	stq	$21, 0($16)
	blbc	$28,b_alignedloop
	nop
b_aligneddone:
	beq	$18,b_alignedret
	mov	  8,$28			# this is how many bytes to preserve in last dst quad
	ldq_u	$21,-8($17)		# get last src quad
	subq	$28,$18,$28
	mskqh	$21,$28,$21		# mask out any junk out of bottom
b_lastalignedtry:
	ldq_l	$23,-8($16)		# read existing last quad
	mskql	$23,$28,$23		# mask out stuff we want to change
	or	$23,$21,$23		# merge in data to be written
	stq_c	$23,-8($16)		# store it back out
	blbc	$23,b_lastalignedfail
b_alignedret:
	ret	$31,($26)		# all done
b_notaligned:

	## Copy first quad out to align dst

	ldq_u	$21,-1($17)		# get first src quad
	subq	$17,  8,$17
	cmoveq	$20,  8,$20		# this is how many bytes of it are data

	beq	$19,b_dstaligned	# skip this if we're already at dst quad boundary
	cmpult	$20,$19,$28		# see if we have enough in $21 for first dst quad
	blbc	$28,b_alreadyhavenuf
	subq	$19,$20,$28		# if not, shift what we got in place for dst
	insql	$21,$28,$22
	ldq_u	$21,-1($17)		#         read the second src quad
	subq	$17,  8,$17
	insqh	$21,$28,$28		#         and merge in with the first src quad
	or	$22,$28,$22
	addq	$20,  8,$20		#         compensate for reading extra src quad
	br	$31,b_nowtheresenuf
b_alreadyhavenuf:
	subq	$20,$19,$28		# if so, shift in place for dst
	extql	$21,$28,$22
b_nowtheresenuf:
	bic	$16,  7,$16		# quad align dst pointer
	mskql	$22,$19,$22		# clear out junk from top of src
b_firstunaligntry:
	ldq_l	$23,0($16)		# read existing quad
	mskqh	$23,$19,$23		# clear space for new data
	or	$23,$22,$23		# insert new data
	stq_c	$23,0($16)		# store new quad
	blbc	$23,b_firstunalignfail
	subq	$18,$19,$18		# this is how many bytes we have left to write out
	subq	$20,$19,$20		# this is how many bytes in bottom of srcquad1 still need to be copied
b_dstaligned:

	## Do the bulk of the copying

	cmpult	$18,  8,$28		# see if there is at least a whole quad to copy
	blbs	$28,b_bulkdone
	.align	4
b_bulkloop:
	ldq_u	$22,-1($17)		# get next src quad
	subq	$17,  8,$17		# point to next src quad
	extqh	$21,$20,$21		# merge the two quads
	extql	$22,$20,$23
	subq	$18,  8,$18		# there is one less quad to copy
	or	$23,$21,$23
	cmpult	$18,  8,$28		# repeat if more whole quads to copy
	stq	$23,-8($16)		# store dst quad
	subq	$16,  8,$16
	mov	$22,$21			# shift what's left for next loop
	blbc	$28,b_bulkloop
	nop
b_bulkdone:

	## Maybe there's a partial quad on the end

	beq	$18,b_return		# see if anything left to copy
	extqh	$21,$20,$21		# put good data in top of srcquad1
	cmpult	$20,$18,$28		# see if we have enough for last dst quad
	blbc	$28,b_haveenuflast
	ldq_u	$22,-1($17)		# if not, read from last src quad
	extql	$22,$20,$22		#         and merge it in
	or	$21,$22,$21
b_haveenuflast:
	mov	  8,$28			# make count of what to preserve out of last quad
	subq	$28,$18,$28
	mskqh	$21,$28,$21		# clear junk bytes from bottom of last src quad
b_lastunaligntry:
	ldq_l	$23,-8($16)		# read last dst quad
	mskql	$23,$28,$23		# mask out stuff we want to write
	or	$23,$21,$23		# insert stuff we want to write
	stq_c	$23,-8($16)		# write it back out
	blbc	$23,b_lastunalignfail
b_return:
	ret	$31,($26)

.endif

	#################################
	## LDQ_L/STQ_C failure retries ##
	#################################

storeonlyquadfail:
	br	$31,storeonlyquadtry

f_storefirstdstquadfail:
	br	$31,f_storefirstdstquadtry
f_oddalignedfail:
	br	$31,f_oddalignedtry
f_alignedlastfail:
	br	$31,f_alignedlasttry
f_storelastdstfail:
	br	$31,f_storelastdsttry

.if !NULLCHK
b_firstalignedfail:
	br	$31,b_firstalignedtry
b_lastalignedfail:
	br	$31,b_lastalignedtry
b_firstunalignfail:
	br	$31,b_firstunaligntry
b_lastunalignfail:
	br	$31,b_lastunaligntry
.endif

.if NULLCHK

	.p2align 6

	## Tells us the lowest bit set in a byte, plus 1

lowbitsetp1:
	.byte	9,1,2,1,3,1,2,1,4,1,2,1,3,1,2,1
	.byte	5,1,2,1,3,1,2,1,4,1,2,1,3,1,2,1
	.byte	6,1,2,1,3,1,2,1,4,1,2,1,3,1,2,1
	.byte	5,1,2,1,3,1,2,1,4,1,2,1,3,1,2,1
	.byte	7,1,2,1,3,1,2,1,4,1,2,1,3,1,2,1
	.byte	5,1,2,1,3,1,2,1,4,1,2,1,3,1,2,1
	.byte	6,1,2,1,3,1,2,1,4,1,2,1,3,1,2,1
	.byte	5,1,2,1,3,1,2,1,4,1,2,1,3,1,2,1
	.byte	8,1,2,1,3,1,2,1,4,1,2,1,3,1,2,1
	.byte	5,1,2,1,3,1,2,1,4,1,2,1,3,1,2,1
	.byte	6,1,2,1,3,1,2,1,4,1,2,1,3,1,2,1
	.byte	5,1,2,1,3,1,2,1,4,1,2,1,3,1,2,1
	.byte	7,1,2,1,3,1,2,1,4,1,2,1,3,1,2,1
	.byte	5,1,2,1,3,1,2,1,4,1,2,1,3,1,2,1
	.byte	6,1,2,1,3,1,2,1,4,1,2,1,3,1,2,1
	.byte	5,1,2,1,3,1,2,1,4,1,2,1,3,1,2,1
.endif
