##+++2003-11-18
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
##---2003-11-18

##########################################################################
##									##
##  Implement watchpoints via fault-on-read/write mechanism		##
##									##
##########################################################################

	.title	oz_hwaxp_watch

	.include "oz_params_axp.s"

	.set	noat
	.set	nomacro

	NWATCHPOINTS = 8

	TYPE_READ  = 2
	TYPE_WRITE = 4

	WP_L_TYPE    =  0
	WP_L_PAGE    =  4
	WP_Q_BEGADDR =  8
	WP_Q_ENDADDR = 16
	WP__SIZE     = 24

	.data
	.p2align 4
mydata:

l3ptbase:	.quad	0			# holds VA of base of level-3 pagetable
lockflag:	.quad	0			# holds VA locked by most recent LDL_L/LDQ_L
						# cleared by any other memory operation
watchpoints:	.space	NWATCHPOINTS*WP__SIZE	# table of watchpoints
nsetup:		.long	0			# number of entries in 'watchpoints'

	.text
	.p2align 4

crash_p:	.quad	oz_crash
l3ptbase_p:	.quad	oz_hwaxp_l3ptbase
mydata_p:	.quad	mydata
printk_p:	.quad	oz_knl_printk
scb_p:		.quad	oz_hwaxp_scb

                     ## 0,1,2,3,4,5,6,7,8,9,A,B,C,D,E,F,0,1,2,3,4,5,6,7,8,9,A,B,C,D,E,F
readsizes:	.byte	0,0,0,0,0,0,0,0,0,0,1,8,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
		.byte	0,0,0,0,0,0,0,0,4,8,4,8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
writesizes:	.byte	0,0,0,0,0,0,0,0,0,0,0,0,0,2,1,8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
		.byte	0,0,0,0,0,0,0,0,0,0,0,0,4,8,4,8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0

msg1:	.string	"oz_hwaxp_watch_set: %p: address %p must be aligned"
msg2:	.string	"oz_hwaxp_watch_set: %p: max watchpoints reached"

read_msg1:	.string	"oz_hwaxp_watch: don't know why %8.8LX at %QX faulted-on-read"
read_byte_msg:	.ascii	"oz_hwaxp_watch: %16.16QX: %16.16QX -> %2.2BX"   ; .byte 10,0
read_word_msg:	.ascii	"oz_hwaxp_watch: %16.16QX: %16.16QX -> %4.4WX"   ; .byte 10,0
read_long_msg:	.ascii	"oz_hwaxp_watch: %16.16QX: %16.16QX -> %8.8LX"   ; .byte 10,0
read_quad_msg:	.ascii	"oz_hwaxp_watch: %16.16QX: %16.16QX -> %16.16QX" ; .byte 10,0

write_msg1:	.string	"oz_hwaxp_watch: don't know why %8.8LX at %QX faulted-on-write"
write_byte_msg:	.ascii	"oz_hwaxp_watch: %16.16QX: %16.16QX <- %2.2BX"   ; .byte 10,0
write_word_msg:	.ascii	"oz_hwaxp_watch: %16.16QX: %16.16QX <- %4.4WX"   ; .byte 10,0
write_long_msg:	.ascii	"oz_hwaxp_watch: %16.16QX: %16.16QX <- %8.8LX"   ; .byte 10,0
write_quad_msg:	.ascii	"oz_hwaxp_watch: %16.16QX: %16.16QX <- %16.16QX" ; .byte 10,0

##########################################################################
##									##
##  Set a watchpoint							##
##									##
##	void oz_hwaxp_watch_set (int type, int size, void *addr)	##
##									##
##    Input:								##
##									##
##	R16 = type (2=read, 4=write, 6=either)				##
##	R17 = size of data (1,2,4,8)					##
##	R18 = address (aligned)						##
##									##
##########################################################################

	.p2align 4
	.globl	oz_hwaxp_watch_set
	.type	oz_hwaxp_watch_set,@function
oz_hwaxp_watch_set:
	subq	$sp,24,$sp		# save scratch regs
	stq	$13, 0($sp)
	stq	$14, 8($sp)
	stq	$26,16($sp)
	mov	$27,$13
	a13 = oz_hwaxp_watch_set	# RO base register = R13
	ldq	$14,mydata_p-a13($13)
	a14 = mydata			# RW base register = R14
	subq	$17,1,$2		# make sure address given is aligned
	ldl	$0,nsetup-a14($14)	# get number of watchpoints already set up
	and	$2,$18,$2
	ldq	$4,l3ptbase-a14($14)	# point to base of L3 pagetable
	subl	$0,NWATCHPOINTS,$1	# make sure we have room for another
	bne	$2,set_maligned
	beq	$4,set_firstime		# maybe this is very first time
	beq	$1,set_isfull		# maybe there is no more room
set_setup:
	addl	$0,1,$1			# increment nsetup
	stl	$1,nsetup-a14($14)
	sll	$18,64-L2VASIZE,$1	# get virtual page number
	srl	$1,64-L2VASIZE+L2PAGESIZE,$1
	mulq	$0,WP__SIZE,$0		# get table offset
	lda	$2,watchpoints-a14($14)	# point to table entry
	addq	$2,$0,$2
	stl	$16,WP_L_TYPE($2)	# save watchpoint type
	stl	$1,WP_L_PAGE($2)	# save watchpoint page
	addq	$18,$17,$17		# save ending address
	stq	$18,WP_Q_BEGADDR($2)	# save beginning address
	stq	$17,WP_Q_ENDADDR($2)

	s8addq	$1,$4,$0		# point to pte
	ldq	$1,0($0)		# set the fault-on-read&write bits
	or	$1,6,$1
	stq	$1,0($0)

	mov	$18,$16			# invalidate it
	call_pal pal_MTPR_TBIS

	ldq	$13, 0($sp)
	ldq	$14, 8($sp)
	ldq	$26,16($sp)
	addq	$sp,24,$sp
	ret	$31,($26)		# all done

set_maligned:
	lda	$16,msg1-a13($13)
	mov	$26,$17
	ldq	$27,crash_p-a13($13)
	jsr	$26,($27)

set_isfull:
	lda	$16,msg2-a13($13)
	mov	$26,$17
	ldq	$27,crash_p-a13($13)
	jsr	$26,($27)

	# Out-of-line code for first-time setup

set_firstime:
	ldq	 $2,scb_p-a13($13)	# set up raw SCB vectors
	lda	 $0,faultonread-a13($13)
	ldq	 $1,mydata_p-a13($13)
	stq	 $0,0x0A0($2)
	stq	 $1,0x0A8($2)
	lda	 $0,faultonwrite-a13($13)
	ldq	 $1,mydata_p-a13($13)
	stq	 $0,0x0B0($2)
	stq	 $1,0x0B8($2)
	ldq	 $4,l3ptbase_p-a13($13)	# get L3 pagetable base
	ldq	 $4,0($4)
	stq	 $4,l3ptbase-a14($14)
	mov	  0,$0			# get nsetup again
	br	$31,set_setup

##########################################################################
##									##
##  Clear a watchpoint							##
##									##
##	void oz_hwaxp_watch_set (int type, int size, void *addr)	##
##									##
##    Input:								##
##									##
##	R16 = type (2=read, 4=write, 6=either)				##
##	R17 = size of data (1,2,4,8)					##
##	R18 = address (aligned)						##
##									##
##########################################################################

	.p2align 4
	.globl	oz_hwaxp_watch_clr
	.type	oz_hwaxp_watch_clr,@function
oz_hwaxp_watch_clr:
	subq	$sp,24,$sp		# save scratch regs
	stq	$13, 0($sp)
	stq	$14, 8($sp)
	stq	$26,16($sp)
	mov	$27,$13
	b13 = oz_hwaxp_watch_clr	# RO base register = R13
	ldq	$14,mydata_p-b13($13)
	b14 = mydata			# RW base register = R14

	# Remove matching entry(s) from table

	ldl	$0,nsetup-b14($14)	# get number of watchpoints set up
	lda	$1,watchpoints-b14($14)	# point to the table
	addq	$17,$18,$19		# create 'endaddr'
	sll	$18,64-L2VASIZE,$20	# create page number
	mov	1,$7			# assume we will disable the fault-on-* bits in pte
	srl	$20,64-L2VASIZE+L2PAGESIZE,$20
	beq	$0,clr_done
clr_loop:
	ldl	$21,WP_L_TYPE($1)	# see if table entry matches
	ldl	$22,WP_L_PAGE($1)
	ldq	$23,WP_Q_BEGADDR($1)
	ldq	$24,WP_Q_ENDADDR($1)
	subl	$21,$16,$21		# - matching type
	subl	$22,$20,$22		# - matching page
	subq	$23,$18,$23		# - matching begaddr
	subq	$24,$19,$24		# - matching endaddr
	or	$21,$22,$21
	or	$23,$24,$23
	or	$23,$21,$23
	beq	$23,clr_match
	cmoveq	$22,0,$7		# no match, but if page matches, keep the fault-on-* pte bits
clr_next:
	subl	$0,1,$0
	addq	$1,WP__SIZE,$1
	bne	$0,clr_loop
clr_done:

	# Unless there's a referencing entry, 
	# clear the fault-on-* bits in the pte
	#  R18 = vaddr
	#  R20 = vpage

	beq	$7,clr_return
	ldq	$0,l3ptbase-b14($14)	# point to pagetable
	s8addq	$20,$0,$0		# point to pte
	ldq	$1,0($0)		# clear the fault-on-read&write bits
	bic	$1,6,$1
	stq	$1,0($0)
	mov	$18,$16			# invalidate it
	call_pal pal_MTPR_TBIS

clr_return:
	ldq	$13, 0($sp)
	ldq	$14, 8($sp)
	ldq	$26,16($sp)
	addq	$sp,24,$sp
	ret	$31,($26)		# all done

	# Out-of-line code to smash out the entry pointed to by R1

clr_match:
	ldl	$3,nsetup-b14($14)	# decrement nsetup
	subl	$0,1,$2			# and one less will follow this one
	subl	$3,1,$3
	stl	$3,nsetup-b14($14)
	beq	$2,clr_done		# if none follow, we just squashed the last entry in table
	mov	$1,$3			# point to one to smash out
clr_smash:
	ldl	$21,WP__SIZE+WP_L_TYPE($3) # smash out the entry
	ldl	$22,WP__SIZE+WP_L_PAGE($3)
	ldq	$23,WP__SIZE+WP_Q_BEGADDR($3)
	ldq	$24,WP__SIZE+WP_Q_ENDADDR($3)
	stl	$21,WP_L_TYPE($3)
	stl	$22,WP_L_PAGE($3)
	stq	$23,WP_Q_BEGADDR($3)
	stq	$24,WP_Q_ENDADDR($3)
	subl	$2,1,$2			# one less to smash
	addq	$3,WP__SIZE,$3		# point to next entry to smash
	bne	$2,clr_smash		# repeat if any more to smash
	br	$31,clr_next

##########################################################################
##									##
##  Fault-on-Read handler						##
##									##
##    Input:								##
##									##
##	R2 = faultonread entrypoint address				##
##	R3 = mydata address						##
##	R4 = virtual address						##
##	 0($sp) = saved R2						##
##	 8($sp) = saved R3						##
##	16($sp) = saved R4						##
##	24($sp) = saved R5						##
##	32($sp) = saved R6						##
##	40($sp) = saved R7						##
##	48($sp) = saved PC						##
##	56($sp) = saved PS						##
##									##
##########################################################################

	.p2align 4
faultonread:
	c2 = faultonread
	c3 = mydata
	subq	$sp,208,$sp		# save rest of registers on stack
	stq	 $8,  0($sp)
	stq	 $9,  8($sp)
	stq	$10, 16($sp)
	stq	$11, 24($sp)
	stq	$12, 32($sp)
	stq	$13, 40($sp)
	stq	$14, 48($sp)
	stq	$15, 56($sp)
	stq	$16, 64($sp)
	stq	$17, 72($sp)
	stq	$18, 80($sp)
	stq	$19, 88($sp)
	stq	$20, 96($sp)
	stq	$21,104($sp)
	stq	$22,112($sp)
	stq	$23,120($sp)
	stq	$24,128($sp)
	stq	$25,136($sp)
	stq	$26,144($sp)
	stq	$27,152($sp)
	stq	$28,160($sp)
	stq	$29,168($sp)
	stq	 $0,192($sp)
	stq	 $1,200($sp)

	# now stack contains:
	#   R8..R31,R0..R7,PC,PS

####	lda	$16,0x1601
####	call_pal pal_HALT

	sll	$4,64-L2VASIZE,$6	# get virtual page number
	ldq	$12,l3ptbase-c3($3)	# point to pagetable entry
	srl	$6,64-L2VASIZE+L2PAGESIZE,$6
	ldq	$8,256($sp)		# get saved PC
	s8addq	$6,$12,$12
	addq	$8,4,$1			# increment over faulting instruction
	ldq	$13,0($12)		# clear fault-on-* bits
	ldl	$14,0($8)		# get faulting instruction
	bic	$13,6,$0
	stq	$1,256($sp)
	stq	$0,0($12)
	mov	$4,$16			# and invalidate TB entry
	call_pal pal_MTPR_TBIS

	srl	$14,26,$10		# get (load) opcode that caused fault
	srl	$14,21-3,$11		# get its 'ra', ie, register being loaded * 8
	and	$10,0x3F,$10
	cmpeq	$11,30,$15		# special stuff at end if loading the stack pointer
	subl	$11,8*8,$11		# R8->offset 0; R9->offset 8, etc.
	and	$10,0x3E,$1		# check for LDL_L/LDQ_L
	addq	$10,$2,$9
	cmpeq	$1,0x2A,$1		# set R1=one if LDL_L/LDQ_L; else set R1=zero
	and	$11,0xF8,$11		# make it wrap, now we have offset on stack
	cmovne	$1,$4,$1		# set R1=va if LDL_L/LDQ_L; else leave R1=zero
	ldbu	$9,readsizes-c2($9)	# get size of operand (in bytes)
	addq	$11,$sp,$11		# now we point to register on stack
	subl	$10,0x0B,$0		# see if its an LDQ_U opcode
	bic	$4,7,$5			# get quad aligned address in case of LDQ_U
	beq	$9,read_badopcode	# - not a 'load' opcode
	cmoveq	$0,$5,$4		# if LDQ_U, quad align virtual address
	stq	$1,lockflag-c3($3)	# if LDL_L/LDQ_L, set lockflag to virt addr, else clear it
	addq	$4,$9,$5		# get end virtual address (exclusive)

####	lda	$16,0x1602
####	call_pal pal_HALT

	#  R2 = read-only base register (c2)
	#  R3 = read/write base register (c3)
	#  R4 = faulting virtual address (begaddr)
	#  R5 = va + size of operand (endaddr)
	#  R6 = virtual page number
	#  R8 = faulting instruction's address (PC)
	#  R9 = size (1,2,4,8)
	# R10 = opcode in bits <05:00>
	# R11 = points to 'ra' on stack
	# R12 = pte address
	# R13 = original pte contents (with fault enables on)
	# R14 = faulting instruction longword
	# R15 = 0: 'ra' isn't SP; 1: 'ra' is the SP

	ldl	$0,nsetup-c3($3)	# get number of watchpoints
	lda	$1,watchpoints-c3($3)	# point to watchpoint table
	mov	0,$7			# haven't found a hit yet
	beq	$0,read_scandone	# - table is empty
read_scanloop:
	ldl	$21,WP_L_TYPE($1)
	ldq	$22,WP_Q_BEGADDR($1)
	and	$21,TYPE_READ,$21
	ldq	$23,WP_Q_ENDADDR($1)
	xor	$21,TYPE_READ,$21
	cmpule	$5,$22,$22		# if fault's end address <= entry's beg address, no match
	cmpule	$23,$4,$23		# if entry's end address <= fault's beg address, no match
	or	$21,$22,$21
	subl	$0,1,$0			# decrement loop counter
	or	$21,$23,$21
	addq	$1,WP__SIZE,$1		# increment pointer to next entry
	cmoveq	$21,0,$0		# stop loop if we got a match
	cmoveq	$21,1,$7		# set hit flag if we got a match
	bne	$0,read_scanloop	# loop for more to check
read_scandone:

####	lda	$16,0x1603
####	call_pal pal_HALT

	# R7 = hit flag (0=no hit; 1=had a hit)

	# Perform load operation

	srl	$9,1,$0
	blbs	$9,read_a_byte
	srl	$0,1,$1
	blbs	$0,read_a_word
	blbs	$1,read_a_long
	ldq	$19,0($4)
	lda	$16,read_quad_msg-c2($2)
	br	$31,read_the_data
read_a_byte:
	ldbu	$19,0($4)
	lda	$16,read_byte_msg-c2($2)
	br	$31,read_the_data
read_a_word:
	ldwu	$19,0($4)
	lda	$16,read_word_msg-c2($2)
	br	$31,read_the_data
read_a_long:
	ldl	$19,0($4)
	lda	$16,read_long_msg-c2($2)
read_the_data:
	stq	$19,0($11)

####	lda	$16,0x1604
####	call_pal pal_HALT

	# R16 = format message
	# R19 = data

	# Maybe print out message

	bne	$7,read_printhitmsg
read_nohitmsg:

####	lda	$16,0x1605
####	call_pal pal_HALT

	# Turn the faulting back on

	stq	$13,0($12)		# restore PTE contents
	mov	$4,$16			# invalidate TB entry
	call_pal pal_MTPR_TBIS

	# If they just loaded the stack pointer, we have a big mess to clean up
	# There are 256 bytes of registers, plus the PC and PS, to move

	bne	$15,read_loadedsp

	# Stack not changed (or has been fixed), restore and return

read_return:

####	lda	$16,0x1606
####	call_pal pal_HALT

	ldq	 $8,  0($sp)
	ldq	 $9,  8($sp)	
	ldq	$10, 16($sp)
	ldq	$11, 24($sp)
	ldq	$12, 32($sp)
	ldq	$13, 40($sp)
	ldq	$14, 48($sp)
	ldq	$15, 56($sp)
	ldq	$16, 64($sp)
	ldq	$17, 72($sp)
	ldq	$18, 80($sp)
	ldq	$19, 88($sp)
	ldq	$20, 96($sp)
	ldq	$21,104($sp)
	ldq	$22,112($sp)
	ldq	$23,120($sp)
	ldq	$24,128($sp)
	ldq	$25,136($sp)
	ldq	$26,144($sp)
	ldq	$27,152($sp)
	ldq	$28,160($sp)
	ldq	$29,168($sp)
	ldq	 $0,192($sp)
	ldq	 $1,200($sp)
	addq	$sp,208,$sp
	call_pal pal_REI

	# Bad opcode (not a load)

read_badopcode:
####	lda	$16,0x1621
####	call_pal pal_HALT
	ldq	$27,crash_p-d2($2)
	lda	$16,read_msg1-d2($2)
	mov	$14,$17
	mov	$8,$18
	jsr	$26,($27)

	# Out-of-line code to print hit message

read_printhitmsg:
####	lda	$16,0x1622
####	call_pal pal_HALT
	mov	$4,$11			# save this where it will survive the printk call
	mov	$8,$17			# ok, set up arg2=faulting pc
	mov	$4,$18			# set up arg3=virtual address
	ldq	$27,printk_p-c2($2)	# call oz_knl_printk
	jsr	$26,($27)
					# (only R9..R15 preserved)
	mov	$11,$4			# restore virtual address
	br	$31,read_nohitmsg

	# Out-of-line code to handle case where SP was loaded with new value (like for a thread switch)
	# The value on the stack is the 'pre-exception' value, ie, what SP must be after the 'call_pal pal_REI'

read_loadedsp:
####	lda	$16,0x1623
####	call_pal pal_HALT
	lda	$26,272($sp)		# get old pre-exception stackpointer value, 64-byte aligned
	ldq	$27,176($sp)		# get new pre-exception stackpointer value, unaligned
	and	$27,63,$0		# save low 6 bits of new SP in top of saved PS
	stb	$0,271($sp)
	bic	$27,63,$27		# 64-byte align new pre-exception stackpointer value
	cmpult	$26,$27,$0		# see if old .gt. new
	bne	$0,read_movestackup
	mov	$sp,$29			# new .le. old, move stack downward
	lda	$sp,-272($27)		# (set up new value first in case of exception)
	ldq	 $0,  0($29)	# R8
	ldq	 $1,  8($29)	# R9
	ldq	 $2, 16($29)	# R10
	ldq	 $3, 24($29)	# R11
	ldq	 $4, 32($29)	# R12
	ldq	 $5, 40($29)	# R13
	ldq	 $6, 48($29)	# R14
	ldq	 $7, 56($29)	# R15
	ldq	 $8, 64($29)	# R16
	ldq	 $9, 72($29)	# R17
	ldq	$10, 80($29)	# R18
	ldq	$11, 88($29)	# R19
	ldq	$12, 96($29)	# R20
	ldq	$13,104($29)	# R21
	ldq	$14,112($29)	# R22
	ldq	$15,120($29)	# R23
	ldq	$16,128($29)	# R24
	stq	 $0,  0($sp)	# R8
	stq	 $1,  8($sp)	# R9
	stq	 $2, 16($sp)	# R10
	stq	 $3, 24($sp)	# R11
	stq	 $4, 32($sp)	# R12
	stq	 $5, 40($sp)	# R13
	stq	 $6, 48($sp)	# R14
	stq	 $7, 56($sp)	# R15
	stq	 $8, 64($sp)	# R16
	stq	 $9, 72($sp)	# R17
	stq	$10, 80($sp)	# R18
	stq	$11, 88($sp)	# R19
	stq	$12, 96($sp)	# R20
	stq	$13,104($sp)	# R21
	stq	$14,112($sp)	# R22
	stq	$15,120($sp)	# R23
	stq	$16,128($sp)	# R24
	ldq	 $0,136($29)	# R25
	ldq	 $1,144($29)	# R26
	ldq	 $2,152($29)	# R27
	ldq	 $3,160($29)	# R28
	ldq	 $4,168($29)	# R29
##	ldq	 $5,176($29)	# R30
##	ldq	 $6,184($29)	# R31
	ldq	 $7,192($29)	# R0
	ldq	 $8,200($29)	# R1
	ldq	 $9,208($29)	# R2
	ldq	$10,216($29)	# R3
	ldq	$11,224($29)	# R4
	ldq	$12,232($29)	# R5
	ldq	$13,240($29)	# R6
	ldq	$14,248($29)	# R7
	ldq	$15,256($29)	# PC
	ldq	$16,264($29)	# PS
	stq	 $0,136($sp)	# R25
	stq	 $1,144($sp)	# R26
	stq	 $2,152($sp)	# R27
	stq	 $3,160($sp)	# R28
	stq	 $4,168($sp)	# R29
##	stq	 $5,176($sp)	# R30
##	stq	 $6,184($sp)	# R31
	stq	 $7,192($sp)	# R0
	stq	 $8,200($sp)	# R1
	stq	 $9,208($sp)	# R2
	stq	$10,216($sp)	# R3
	stq	$11,224($sp)	# R4
	stq	$12,232($sp)	# R5
	stq	$13,240($sp)	# R6
	stq	$14,248($sp)	# R7
	stq	$15,256($sp)	# PC
	stq	$16,264($sp)	# PS
	br	$31,read_return
read_movestackup:
	lda	$29,-272($27)		# new .gt. old, move stack upward
	ldq	 $0,136($sp)	# R25
	ldq	 $1,144($sp)	# R26
	ldq	 $2,152($sp)	# R27
	ldq	 $3,160($sp)	# R28
	ldq	 $4,168($sp)	# R29
##	ldq	 $5,176($sp)	# R30
##	ldq	 $6,184($sp)	# R31
	ldq	 $7,192($sp)	# R0
	ldq	 $8,200($sp)	# R1
	ldq	 $9,208($sp)	# R2
	ldq	$10,216($sp)	# R3
	ldq	$11,224($sp)	# R4
	ldq	$12,232($sp)	# R5
	ldq	$13,240($sp)	# R6
	ldq	$14,248($sp)	# R7
	ldq	$15,256($sp)	# PC
	ldq	$16,264($sp)	# PS
	stq	 $0,136($29)	# R25
	stq	 $1,144($29)	# R26
	stq	 $2,152($29)	# R27
	stq	 $3,160($29)	# R28
	stq	 $4,168($29)	# R29
##	stq	 $5,176($29)	# R30
##	stq	 $6,184($29)	# R31
	stq	 $7,192($29)	# R0
	stq	 $8,200($29)	# R1
	stq	 $9,208($29)	# R2
	stq	$10,216($29)	# R3
	stq	$11,224($29)	# R4
	stq	$12,232($29)	# R5
	stq	$13,240($29)	# R6
	stq	$14,248($29)	# R7
	stq	$15,256($29)	# PC
	stq	$16,264($29)	# PS
	ldq	 $0,  0($sp)	# R8
	ldq	 $1,  8($sp)	# R9
	ldq	 $2, 16($sp)	# R10
	ldq	 $3, 24($sp)	# R11
	ldq	 $4, 32($sp)	# R12
	ldq	 $5, 40($sp)	# R13
	ldq	 $6, 48($sp)	# R14
	ldq	 $7, 56($sp)	# R15
	ldq	 $8, 64($sp)	# R16
	ldq	 $9, 72($sp)	# R17
	ldq	$10, 80($sp)	# R18
	ldq	$11, 88($sp)	# R19
	ldq	$12, 96($sp)	# R20
	ldq	$13,104($sp)	# R21
	ldq	$14,112($sp)	# R22
	ldq	$15,120($sp)	# R23
	ldq	$16,128($sp)	# R24
	stq	 $0,  0($29)	# R8
	stq	 $1,  8($29)	# R9
	stq	 $2, 16($29)	# R10
	stq	 $3, 24($29)	# R11
	stq	 $4, 32($29)	# R12
	stq	 $5, 40($29)	# R13
	stq	 $6, 48($29)	# R14
	stq	 $7, 56($29)	# R15
	stq	 $8, 64($29)	# R16
	stq	 $9, 72($29)	# R17
	stq	$10, 80($29)	# R18
	stq	$11, 88($29)	# R19
	stq	$12, 96($29)	# R20
	stq	$13,104($29)	# R21
	stq	$14,112($29)	# R22
	stq	$15,120($29)	# R23
	stq	$16,128($29)	# R24
	mov	$29,$sp			# point to new stack
	br	$31,read_return		# all done

##########################################################################
##									##
##  Fault-on-Write handler						##
##									##
##    Input:								##
##									##
##	R2 = faultonwrite entrypoint address				##
##	R3 = mydata address						##
##	R4 = virtual address						##
##	 0($sp) = saved R2						##
##	 8($sp) = saved R3						##
##	16($sp) = saved R4						##
##	24($sp) = saved R5						##
##	32($sp) = saved R6						##
##	40($sp) = saved R7						##
##	48($sp) = saved PC						##
##	56($sp) = saved PS						##
##									##
##########################################################################

	.p2align 4
faultonwrite:
	d2 = faultonwrite
	d3 = mydata
	subq	$sp,208,$sp		# save rest of registers on stack
	stq	 $8,  0($sp)
	stq	 $9,  8($sp)
	stq	$10, 16($sp)
	stq	$11, 24($sp)
	stq	$12, 32($sp)
	stq	$13, 40($sp)
	stq	$14, 48($sp)
	stq	$15, 56($sp)
	stq	$16, 64($sp)
	stq	$17, 72($sp)
	stq	$18, 80($sp)
	stq	$19, 88($sp)
	stq	$20, 96($sp)
	stq	$21,104($sp)
	stq	$22,112($sp)
	stq	$23,120($sp)
	stq	$24,128($sp)
	stq	$25,136($sp)
	stq	$26,144($sp)
	stq	$27,152($sp)
	stq	$28,160($sp)
	lda	$27,272($sp)		# (pre-exception value of SP)
	stq	$29,168($sp)
	stq	$27,176($sp)		# (in case doing an STx SP,...)
	stq	$31,184($sp)		# (in case doing an STx R31,...)
	stq	 $0,192($sp)
	stq	 $1,200($sp)

####	lda	$16,0x1651
####	call_pal pal_HALT

	# now stack contains:
	#   R8..R31,R0..R7,PC,PS

	sll	$4,64-L2VASIZE,$6	# get virtual page number
	ldq	$12,l3ptbase-c3($3)	# point to pagetable entry
	srl	$6,64-L2VASIZE+L2PAGESIZE,$6
	ldq	$8,256($sp)		# get saved PC
	s8addq	$6,$12,$12
	addq	$8,4,$1			# increment over faulting instruction
	ldq	$13,0($12)		# clear fault-on-* bits
	ldl	$14,0($8)		# get faulting instruction
	bic	$13,6,$0
	stq	$1,256($sp)
	stq	$0,0($12)
	mov	$4,$16			# and invalidate TB entry
	call_pal pal_MTPR_TBIS

####	lda	$16,0x1652
####	call_pal pal_HALT

	srl	$14,21-3,$11		# get its 'ra', ie, register being loaded * 8
	srl	$14,26,$10		# get (store) opcode that caused fault
	subl	$11,8*8,$11		# R8->offset 0; R9->offset 8, etc.
	and	$10,0x3F,$10
	and	$11,0xF8,$11		# make it wrap, now we have offset on stack
	and	$10,0x3E,$15		# special stuff later if STL_C/STQ_C instruction
	addq	$10,$2,$9
	addq	$11,$sp,$11		# now we point to register on stack
	ldbu	$9,writesizes-d2($9)	# get size of operand (in bytes)
	subl	$10,0x0F,$0		# see if its an STQ_U opcode
	bic	$4,7,$5			# get quad aligned address in case of STQ_U
	beq	$9,write_badopcode	# - not a 'store' opcode
	cmoveq	$0,$5,$4		# if STQ_U, quad align virt address
	cmpeq	$15,0x2E,$15		# finish checking for STL_C/STQ_C
	addq	$4,$9,$5		# get end virtual address (exclusive)

####	lda	$16,0x1653
####	call_pal pal_HALT

	#  R2 = read-only base register (d2)
	#  R3 = read/write base register (d3)
	#  R4 = faulting virtual address
	#  R5 = va + size of operand
	#  R6 = virtual page number
	#  R8 = faulting instruction's address (PC)
	#  R9 = size (1,2,4,8)
	# R10 = opcode in bits <05:00>
	# R11 = points to 'ra' on stack
	# R12 = pte address
	# R13 = original pte contents (with fault enables on)
	# R14 = faulting instruction longword
	# R15 = 0: isn't an STL_C/STQ_C; 1: it is an STL_C/STQ_C

	ldl	$0,nsetup-d3($3)	# get number of watchpoints
	lda	$1,watchpoints-d3($3)	# point to watchpoint table
	mov	0,$7			# haven't found a hit yet
	beq	$0,write_scandone	# - table is empty
write_scanloop:
	ldl	$21,WP_L_TYPE($1)
	ldq	$22,WP_Q_BEGADDR($1)
	and	$21,TYPE_WRITE,$21
	ldq	$23,WP_Q_ENDADDR($1)
	xor	$21,TYPE_WRITE,$21
	cmpule	$5,$22,$22		# if fault's end address <= entry's beg address, no match
	cmpule	$23,$4,$23		# if entry's end address <= fault's beg address, no match
	or	$21,$22,$21
	subl	$0,1,$0			# decrement loop counter
	or	$21,$23,$21
	addq	$1,WP__SIZE,$1		# increment pointer to next entry
	cmoveq	$21,0,$0		# stop loop if we got a match
	cmoveq	$21,1,$7		# set hit flag if we got a match
	bne	$0,write_scanloop	# loop for more to check
write_scandone:

####	lda	$16,0x1654
####	call_pal pal_HALT

	# R7 = hit flag (0=no hit; 1=had a hit)

	# Perform store operation

	ldq	$19,0($11)		# get contents of the register
	bne	$15,write_got_stlstq_c	# special stuff if STL_C/STQ_C
write_the_data:
	stl	$31,lockflag-d3($3)	# anyway, clear lockflag

	srl	$9,1,$0
	blbs	$9,write_a_byte
	srl	$0,1,$1
	blbs	$0,write_a_word
	blbs	$1,write_a_long
	stq	$19,0($4)
	lda	$16,write_quad_msg-d2($2)
	br	$31,wrote_the_data
write_a_byte:
	stb	$19,0($4)
	lda	$16,write_byte_msg-d2($2)
	br	$31,wrote_the_data
write_a_word:
	stw	$19,0($4)
	lda	$16,write_word_msg-d2($2)
	br	$31,wrote_the_data
write_a_long:
	stl	$19,0($4)
	lda	$16,write_long_msg-d2($2)
wrote_the_data:

####	lda	$16,0x1655
####	call_pal pal_HALT

	# R16 = format message
	# R19 = data

	# Maybe print out message

	bne	$7,write_printhitmsg
write_nohitmsg:

####	lda	$16,0x1656
####	call_pal pal_HALT

	# Turn the faulting back on

	stq	$13,0($12)		# restore PTE contents
	mov	$4,$16			# invalidate TB entry
	call_pal pal_MTPR_TBIS

	# Restore and return

####	lda	$16,0x1657
####	call_pal pal_HALT

	ldq	 $8,  0($sp)
	ldq	 $9,  8($sp)	
	ldq	$10, 16($sp)
	ldq	$11, 24($sp)
	ldq	$12, 32($sp)
	ldq	$13, 40($sp)
	ldq	$14, 48($sp)
	ldq	$15, 56($sp)
	ldq	$16, 64($sp)
	ldq	$17, 72($sp)
	ldq	$18, 80($sp)
	ldq	$19, 88($sp)
	ldq	$20, 96($sp)
	ldq	$21,104($sp)
	ldq	$22,112($sp)
	ldq	$23,120($sp)
	ldq	$24,128($sp)
	ldq	$25,136($sp)
	ldq	$26,144($sp)
	ldq	$27,152($sp)
	ldq	$28,160($sp)
	ldq	$29,168($sp)
	ldq	 $0,192($sp)
	ldq	 $1,200($sp)
	addq	$sp,208,$sp
	call_pal pal_REI

	# Bad opcode (not a store)

write_badopcode:
	ldq	$27,crash_p-d2($2)
	lda	$16,write_msg1-d2($2)
	mov	$14,$17
	mov	$8,$18
	jsr	$26,($27)

	# Out-of-line code to handle STL/STQ_C

write_got_stlstq_c:
	ldq	$0,lockflag-d3($3)	# get lock flag
	cmpeq	$0,$4,$0		# see if eq to the virtual address
	stq	$0,0($11)		# store 'eq' flag value back in register
	bne	$0,write_the_data	# if flag still set, write data to memory
	mov	0,$7			# if flag clear, don't print out a hit message
	br	$31,wrote_the_data	# ... and skip over the actual write

	# Out-of-line code to print hit message

write_printhitmsg:
	mov	$4,$11			# save this where it will survive the printk call
	mov	$8,$17			# ok, set up arg2=faulting pc
	mov	$4,$18			# set up arg3=virtual address
	ldq	$27,printk_p-d2($2)	# call oz_knl_printk
	jsr	$26,($27)
					# (only R9..R15 preserved)
	mov	$11,$4			# restore virtual address
	br	$31,write_nohitmsg
