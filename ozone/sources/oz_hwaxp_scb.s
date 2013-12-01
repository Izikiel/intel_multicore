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
##  Default exception / interrupt handler				##
##  It just prints out the registers then halts				##
##									##
##########################################################################

	.include "oz_params_axp.s"

	.set	noat
	.set	nomacro

	.data
	.p2align L2PAGESIZE
	.globl	oz_hwaxp_scb
	.type	oz_hwaxp_scb,@object	# MUST be page aligned and physically contiguous
oz_hwaxp_scb:	.space	16*2048		# pair of quadwords for each possible vector
					#   .quad entrypoint -> R2 on entry
					#   .quad parameter  -> R3 on entry
	scbsize  = .-oz_hwaxp_scb
	scbpages = scbsize>>L2PAGESIZE

setctbl:	.space	16*2048		# pair of quadwords for each possible vector
					#   .quad entrypoint
					#   .quad parameter

db:

	OZ_HWAXP_REIMAX = 16		# number of entries
	OZ_HWAXP_REISIZ = 48		# size of each entry
	.globl	oz_hwaxp_reitbl
	.globl	oz_hwaxp_reiofs
	.type	oz_hwaxp_reitbl,@object
	.type	oz_hwaxp_reiofs,@object
oz_hwaxp_reitbl: .space	OZ_HWAXP_REISIZ*OZ_HWAXP_REIMAX # .quad RSCC,PCFROM,PCTO,PSTO,unused,PTBR
oz_hwaxp_reiofs: .quad	0

	.text

	.p2align 3
tb:

db_p:		.quad	db
scb_p:		.quad	oz_hwaxp_scb
setctbl_p:	.quad	setctbl
scbvbomask:	.quad	0xFFFFFFFFFFFF800F
oz_crash_p:	.quad	oz_crash
printk_p:	.quad	oz_knl_printk
reitbl_p:	.quad	oz_hwaxp_reitbl

dispatch_pd:	.quad	oz_hwaxp_dispatchr27

regtable:
	.word	  0 ; .ascii " 0"
	.word	  8 ; .ascii " 1"
	.word	264 ; .ascii " 2"
	.word	272 ; .ascii " 3"
	.word	280 ; .ascii " 4"
	.word	288 ; .ascii " 5"
	.word	296 ; .ascii " 6"
	.word	304 ; .ascii " 7"
	.word	 48 ; .ascii " 8"
	.word	 56 ; .ascii " 9"
	.word	 64 ; .ascii "10"
	.word	 72 ; .ascii "11"
	.word	 80 ; .ascii "12"
	.word	 88 ; .ascii "13"
	.word	 96 ; .ascii "14"
	.word	104 ; .ascii "15"
	.word	112 ; .ascii "16"
	.word	120 ; .ascii "17"
	.word	128 ; .ascii "18"
	.word	136 ; .ascii "19"
	.word	144 ; .ascii "20"
	.word	152 ; .ascii "21"
	.word	160 ; .ascii "22"
	.word	168 ; .ascii "23"
	.word	176 ; .ascii "24"
	.word	184 ; .ascii "25"
	.word	192 ; .ascii "26"
	.word	200 ; .ascii "27"
	.word	208 ; .ascii "28"
	.word	216 ; .ascii "29"
	.word	312 ; .ascii "PC"
	.word	320 ; .ascii "PS"
	.word	224 ; .ascii "SP"
	.word	 24 ; .ascii "P3"
	.word	 32 ; .ascii "P4"
	.word	 40 ; .ascii "P5"
	regtablelen = .-regtable

hextable:	.ascii	"0123456789ABCDEF"
msg1:		.ascii	"oz_hwaxp_scb"
colon:		.ascii	":"
space:		.ascii	" "
		.ascii	"exception"
crlf:		.byte	13,10
		msg1len = .-msg1
msg2:		.ascii	"oz_hwaxp_scb: C to continue, H to halt> "
		msg2len = .-msg2
scb_setc_msg1:	.string	"oz_hwaxp_scb_setc: bad vector %X"
scb_setraw_msg1: .string "oz_hwaxp_scb_setraw: bad vector %X"
bad_scb_msg:	.ascii	"oz_hwaxp_scb_init: unaligned or non-contiguous scb"
		.byte	13,10
		.ascii	"	va %p, basepp %X, pp[%d] %X"
		.byte	13,10,0
reidump_fmt:	.string	"oz_hwaxp_reidump: [%3X] %QX %QX %16.16QX %4.4X %4.4X\n"

##########################################################################
##									##
##  Fill in the SCB with default handlers				##
##									##
##  These handlers execute in kernel mode with interrupts disabled	##
##  They dump out the registers and a traceback then hang in a loop 	##
##  forever								##
##									##
##    Input:								##
##									##
##	$18 = dispatch routine entrypoint				##
##	$19 = dispatch routine proc descr				##
##	$26 = return address						##
##	$27 = address of oz_hwaxp_scb_init				##
##									##
##########################################################################

	.p2align 4
	.globl	oz_hwaxp_scb_init
	.type	oz_hwaxp_scb_init,@function
oz_hwaxp_scb_init:
	a27 = oz_hwaxp_scb_init
	ldq	$21,scb_p-a27($27)	# point to scb
	ldq	$23,setctbl_p-a27($27)	# point to setctbl
	mov	$31,$18			# start with zero vector number
	mov	$21,$19			# get table pointer
	lda	$20,defaulthandler-a27($27) # point to default handler
	lda	$22,defaultchandler-a27($27)
init_fill_loop:
	stq	$20,0($19)		# store default handler va
	stq	$18,8($19)		# store vector number
	stq	$22,0($23)
	stq	$18,8($23)
	addq	$18,1,$18		# increment vector number
	addq	$19,16,$19		# increment table pointer
	srl	$18,11,$0		# check for 2048 vectors processed
	beq	 $0,init_fill_loop	# repeat if more to go

	sll	$21,64-L2PAGESIZE,$1	# make sure scb is page aligned
	bne	 $1,bad_scb
	call_pal pal_MFPR_VPTB		# point R0 to base of L3 pagetables
	sll	$21,64-L2VASIZE,$1	# get vpage for the scb
	srl	 $1,64-L2VASIZE+L2PAGESIZE,$1
	s8addq	 $1,$0,$2		# point to the scb's first pte
	ldl	$16,4($2)		# get the page number for MTPR SCBB
	mov	  0,$0			# check to make sure it's physically contiguous
init_page_loop:
	ldl	$17,4($2)		#   read page number from the pte
	addl	$16,$0,$18		#   this is the page number it should have
	subl	$18,$17,$18		#   make sure they match
	bne	$18,bad_scb		#   if not, puque
	addq	 $0,1,$0		#   one more page has been done
	addq	 $2,8,$2		#   increment to next pte
	subq	 $0,scbpages,$1		#   see if any more to do
	blt	 $1,init_page_loop	#   repeat until all have been checked
	call_pal pal_MTPR_SCBB		# it's ok, tell hardware where new scb is (first phypage still in R16)
	ret	$31,($26)		# all done

bad_scb:
	mov	$17,$20			# phypage that's bad
	mov	 $0,$19			# phypage index that's bad
	mov	$16,$18			# starting phypage
	mov	$21,$17			# base virt address
	lda	$16,bad_scb_msg-a27($27)
	ldq	$27,printk_p-a27($27)
	jsr	$26,($27)
bad_scb_hang:
	call_pal pal_HALT
	br	$31,bad_scb_hang

##########################################################################
##									##
##  The handler is called:						##
##									##
##	R2 = entrypoint "defaulthandler"				##
##	R3 = vector number						##
##	R4,R5 = exception dependent info				##
##									##
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
defaultchandler:					# called with R16=vector, R17=mchargs, R27=entrypoint
	lda	 $2,defaulthandler-defaultchandler($27)	# R2=default scb routine's entrypoint
	mov	$16,$3					# R3=vector number
	mov	$17,$sp					# wipe stack to mchargs
	ldq	 $0,MCH_Q_R0($sp)
	ldq	 $1,MCH_Q_R1($sp)
	ldq	 $4,MCH_Q_P4($sp)
	ldq	 $5,MCH_Q_P5($sp)
	ldq	 $8,MCH_Q_R8($sp)
	ldq	 $9,MCH_Q_R9($sp)
	ldq	$10,MCH_Q_R10($sp)
	ldq	$11,MCH_Q_R11($sp)
	ldq	$12,MCH_Q_R12($sp)
	ldq	$13,MCH_Q_R13($sp)
	ldq	$14,MCH_Q_R14($sp)
	ldq	$15,MCH_Q_R15($sp)
	ldq	$16,MCH_Q_R16($sp)
	ldq	$17,MCH_Q_R17($sp)
	ldq	$18,MCH_Q_R18($sp)
	ldq	$19,MCH_Q_R19($sp)
	ldq	$20,MCH_Q_R20($sp)
	ldq	$21,MCH_Q_R21($sp)
	ldq	$22,MCH_Q_R22($sp)
	ldq	$23,MCH_Q_R23($sp)
	ldq	$24,MCH_Q_R24($sp)
	ldq	$25,MCH_Q_R25($sp)
	ldq	$26,MCH_Q_R26($sp)
	ldq	$27,MCH_Q_R27($sp)
	ldq	$28,MCH_Q_R28($sp)
	ldq	$29,MCH_Q_R29($sp)
	addq	$sp,MCH__REI,$sp			# wipe stack to point of REI

	.p2align 4
defaulthandler:						# called directly from SCB, R2=entrypoint, R3=vector
	lda	$sp,-264($sp)				# save all the other registers
	stq	 $0,  0($sp)
	stq	 $1,  8($sp)
	stq	 $2, 16($sp)
	stq	 $3, 24($sp)
	stq	 $4, 32($sp)
	stq	 $5, 40($sp)
	stq	 $8, 48($sp)
	stq	 $9, 56($sp)
	stq	$10, 64($sp)
	stq	$11, 72($sp)
	stq	$12, 80($sp)
	stq	$13, 88($sp)
	stq	$14, 96($sp)
	stq	$15,104($sp)
	stq	$16,112($sp)
	stq	$17,120($sp)
	stq	$18,128($sp)
	stq	$19,136($sp)
	stq	$20,144($sp)
	stq	$21,152($sp)
	stq	$22,160($sp)
	stq	$23,168($sp)
	stq	$24,176($sp)
	stq	$25,184($sp)
	stq	$26,192($sp)
	stq	$27,200($sp)
	stq	$28,208($sp)
	stq	$29,216($sp)
	lda	$28,264+64($sp)				# this is what the SP was at time of exception
	stq	$28,224($sp)
	mov	31,$16					# make sure all interrupts are inhibited
	call_pal pal_MTPR_IPL
	stq	$0,232($sp)				# save previous IPL
	mov	$2,$9					# set up R9 as a base register
	x9 = defaulthandler
	mov	msg1len,$16				# output initial message
	lda	$17,msg1-x9($9)
	bsr	$26,print_strc
	mov	$31,$10					# reset loop index
	lda	$11,regtable-x9($9)			# point to register offset table
handler_loop:
	ldwu	$18,2($11)				# output 2-char register name from table
	mov	16,$0
	ldwu	$16,0($11)
	ldah	$18,':'($18)				# followed by a colon
	lda	$1,259($sp)				# followed by register contents in hex
	addq	$16,$sp,$16
	stl	$18,240($sp)
	ldq	$16,0($16)
handler_hex:
	and	$16,15,$17
	srl	$16,4,$16
	addq	$17,$9,$17
	subq	$1,1,$1
	ldbu	$17,hextable-x9($17)
	subq	$0,1,$0
	stb	$17,0($1)
	bne	$0,handler_hex
	addq	$10,4,$10				# increment index to next table entry
	and	$10,15,$1				# see if we've done 4 on this line
	mov	' ',$0
	mov	20,$16
	stb	$0,259($sp)
	bne	$1,handler_next
	mov	0x0D,$0
	mov	0x0A,$1
	mov	21,$16
	stb	$0,259($sp)
	stb	$1,260($sp)
handler_next:
	lda	$17,240($sp)
	bsr	$26,print_strc
	cmpult	$10,regtablelen,$0			# see if we've done all registers
	addq	$11,4,$11
	bne	$0,handler_loop				# repeat if more to do

	mov	msg2len,$16
	lda	$17,msg2-x9($9)
	bsr	$26,print_strc
handler_wait:
	bsr	$26,keyboard_getc
	cmpeq	$0,'C',$1
	blbs	$1,handler_cont
	cmpeq	$0,'H',$1
	blbc	$1,handler_wait

	call_pal pal_HALT				# all done, halt

handler_cont:
	mov	2,$16
	lda	$17,crlf-x9($9)
	bsr	$26,print_strc

	ldq	 $0,  0($sp)				# restore registers
	ldq	 $1,  8($sp)
	ldq	 $8, 48($sp)
	ldq	 $9, 56($sp)
	ldq	$10, 64($sp)
	ldq	$11, 72($sp)
	ldq	$12, 80($sp)
	ldq	$13, 88($sp)
	ldq	$14, 96($sp)
	ldq	$15,104($sp)
	ldq	$16,112($sp)
	ldq	$17,120($sp)
	ldq	$18,128($sp)
	ldq	$19,136($sp)
	ldq	$20,144($sp)
	ldq	$21,152($sp)
	ldq	$22,160($sp)
	ldq	$23,168($sp)
	ldq	$24,176($sp)
	ldq	$25,184($sp)
	ldq	$26,192($sp)
	ldq	$27,200($sp)
	ldq	$28,208($sp)
	ldq	$29,216($sp)
	lda	$sp,264($sp)
	call_pal pal_REI				# return where we left off

##
##  Print counted string on console
##
##    Input:
##
##	 $9 = base register (x9)
##	$16 = string length
##	$17 = string address
##	$26 = return address
##
##    Scratch:
##
##	$16,$17,$18,$19
##
print_strc:
	subq	$sp,24,$sp		# make temp stack space
	stq	$2,  0($sp)		# save scratch registers
	stq	$3,  8($sp)
	stq	$26,16($sp)		# save return address
	mov	$16,$2			# save string length
	mov	$17,$3			# save string address
	beq	$16,2000f		# maybe we're done already
1000:
	ldq	$27,dispatch_pd-x9($9)	# $27 = vaddr of dispatch routine proc descr
	mov	dispatch_puts,$16	# get function code for PUTS
	ldq	$27,0($27)
	mov	0,$17			# terminal unit number
	ldq	$26,8($27)		# $26 = dispatch routine entrypoint
	mov	$3,$18			# virtual address of message
	mov	$2,$19			# length of message
	jsr	$26,($26)
	srl	$0,62,$1		# check error status
	bne	$1,3000f
	zapnot	$0,15,$0		# bytecount in $0<31:00>
	subq	$2,$0,$2		# that much less to do now
	addq	$3,$0,$3
	bne	$2,1000b		# repeat if there's more to do
2000:
	ldq	$2,  0($sp)		# restore scratch registers
	ldq	$3,  8($sp)
	ldq	$26,16($sp)		# restore return address
	addq	$sp,24,$sp		# wipe stack
	ret	$31,($26)		# return
3000:
	call_pal pal_HALT		# can't even print, just halt!
	br	3000b
##
##  Get character from keyboard
##
##    Input:
##
##	 $9 = base register (x9)
##	$26 = return address
##
##    Scratch:
##
##	$16,$17,$18,$19
##
keyboard_getc:
	subq	$sp,24,$sp		# make temp stack space
	stq	$2,  0($sp)		# save scratch registers
	stq	$3,  8($sp)
	stq	$26,16($sp)		# save return address
1000:
	ldq	$27,dispatch_pd-x9($9)	# $27 = vaddr of dispatch routine proc descr
	mov	dispatch_getc,$16	# get function code for GETC
	ldq	$27,0($27)
	mov	0,$17			# terminal unit number
	ldq	$26,8($27)		# $26 = dispatch routine entrypoint
	jsr	$26,($26)
	srl	$0,62,$1		# check status
	bne	$1,1000b		# repeat if no character yet
	and	$0,0x7F,$0		# return character read
	ldq	$2,  0($sp)		# restore scratch registers
	ldq	$3,  8($sp)
	ldq	$26,16($sp)		# restore return address
	addq	$sp,24,$sp		# wipe stack
	ret	$31,($26)		# return

##########################################################################
##									##
##  Set up a "C" routine to be called by one of the vectors		##
##									##
##    Input:								##
##									##
##	R16 = vector byte offset in SCB					##
##	R17 = C routine entrypoint					##
##	R18 = parameter to pass to C routine				##
##									##
##    Output:								##
##									##
##	*R19 = previous entrypoint					##
##	*R20 = previous parameter					##
##									##
##    C routine is called:						##
##									##
##	void (*entrypoint) (void *parameter, OZ_Mchargs *mchargs)	##
##									##
##########################################################################

	.p2align 4
	.globl	oz_hwaxp_scb_setc
	.type	oz_hwaxp_scb_setc,@function
oz_hwaxp_scb_setc:
	b27 = oz_hwaxp_scb_setc
	ldq	$21,scbvbomask-b27($27)	# make sure vector byte offset is sane
	and	$21,$16,$21
	bne	$21,scb_setc_crash1
	ldq	$21,setctbl_p-b27($27)	# point to 32K internal table
	addq	$21,$16,$21		# point to entry we care about
	ldq	 $0,0($21)		# get previous entrypoint
	ldq	 $1,8($21)		# get previous parameter
	beq	$19,scb_setc_noprevent
	stq	 $0,0($19)
scb_setc_noprevent:
	beq	$20,scb_setc_noprevprm
	stq	 $1,0($20)
scb_setc_noprevprm:
	stq	$17,0($21)		# save the new entrypoint
	stq	$18,8($21)		# save the new parameter
	ldq	$22,scb_p-b27($27)	# point to hardware SCB
	addq	$22,$16,$22		# point to the SCB entry we care about
	lda	$0,scb_setc_handler-b27($27)
	stq	$21,8($22)		# its parameter = our internal table entry's address
	stq	$0,0($22)		# its entrypoint = our internal routine
	ret	$31,($26)

scb_setc_crash1:
	mov	$16,$17
	lda	$16,scb_setc_msg1-b27($27)
	ldq	$27,oz_crash_p-b27($27)
	jsr	$26,($27)

##########################################################################
##									##
##  Handler to call a "C" routine that handles the exception		##
##									##
##    Input:								##
##									##
##	R2 = address of scb_setc_handler				##
##	R3 = points to entry in setctbl for this vector			##
##	R4,R5 = dependent parameters					##
##									##
##########################################################################

	.p2align 4
scb_setc_handler:			# R2-R7,PC,PS already on stack
	subq	$sp,MCH__REI,$sp	# finish the mchargs on the stack
	stq	 $0,MCH_Q_R0($sp)
	stq	 $1,MCH_Q_R1($sp)
	stq	 $2,MCH_Q_P2($sp)
	stq	 $3,MCH_Q_P3($sp)
	stq	 $4,MCH_Q_P4($sp)
	stq	 $5,MCH_Q_P5($sp)
	stq	 $8,MCH_Q_R8($sp)
	stq	 $9,MCH_Q_R9($sp)
	stq	$10,MCH_Q_R10($sp)
	stq	$11,MCH_Q_R11($sp)
	stq	$12,MCH_Q_R12($sp)
	stq	$13,MCH_Q_R13($sp)
	stq	$14,MCH_Q_R14($sp)
	stq	$15,MCH_Q_R15($sp)
	stq	$16,MCH_Q_R16($sp)
	stq	$17,MCH_Q_R17($sp)
	stq	$18,MCH_Q_R18($sp)
	stq	$19,MCH_Q_R19($sp)
	stq	$20,MCH_Q_R20($sp)
	stq	$21,MCH_Q_R21($sp)
	stq	$22,MCH_Q_R22($sp)
	stq	$23,MCH_Q_R23($sp)
	stq	$24,MCH_Q_R24($sp)
	stq	$25,MCH_Q_R25($sp)
	stq	$26,MCH_Q_R26($sp)
	stq	$27,MCH_Q_R27($sp)
	stq	$28,MCH_Q_R28($sp)
	lda	$28,MCH__SIZE($sp)	# (stack pointer at time of exception)
	stq	$29,MCH_Q_R29($sp)
	stq	$28,MCH_Q_R30($sp)
	call_pal pal_READ_UNQ
	stq	 $0,MCH_Q_UNQ($sp)
	bne	 $0,setc_check
setc_continue:
	ldq	$27,0($3)		# get entrypoint to C routine
	ldq	$16,8($3)		# get 'parameter' to pass to C routine
	mov	$sp,$17			# point to mchargs on stack
	jsr	$26,($27)		# call the C routine to process it
	ldq	$16,MCH_Q_UNQ($sp)	# restore everything
	call_pal pal_WRITE_UNQ
	ldq	 $0,MCH_Q_R0($sp)
	ldq	 $1,MCH_Q_R1($sp)
	ldq	 $8,MCH_Q_R8($sp)
	ldq	 $9,MCH_Q_R9($sp)
	ldq	$10,MCH_Q_R10($sp)
	ldq	$11,MCH_Q_R11($sp)
	ldq	$12,MCH_Q_R12($sp)
	ldq	$13,MCH_Q_R13($sp)
	ldq	$14,MCH_Q_R14($sp)
	ldq	$15,MCH_Q_R15($sp)
	ldq	$16,MCH_Q_R16($sp)
	ldq	$17,MCH_Q_R17($sp)
	ldq	$18,MCH_Q_R18($sp)
	ldq	$19,MCH_Q_R19($sp)
	ldq	$20,MCH_Q_R20($sp)
	ldq	$21,MCH_Q_R21($sp)
	ldq	$22,MCH_Q_R22($sp)
	ldq	$23,MCH_Q_R23($sp)
	ldq	$24,MCH_Q_R24($sp)
	ldq	$25,MCH_Q_R25($sp)
	ldq	$26,MCH_Q_R26($sp)
	ldq	$27,MCH_Q_R27($sp)
	ldq	$28,MCH_Q_R28($sp)
	ldq	$29,MCH_Q_R29($sp)
	addq	$sp,MCH__REI,$sp	# wipe stack to point of REI
	call_pal pal_REI		# resume where we left off

setc_check:
	ldq	 $2,MCH_Q_PS($sp)	# get caller's processor status
	call_pal pal_RD_PS		# get current processor status
	xor	 $0,$2,$0		# compare them
	and	 $0,0x18,$0		# we just care about current mode bits
	mov	  0,$16			# in case they're different
	beq	 $0,setc_continue	# leave condition handler in place if they're the same
					# caller was different mode, reset unique so we
	call_pal pal_WRITE_UNQ		# ... don't call outermode condition handler 
	br	$31,setc_continue	# ... if there's a innermode exception

##########################################################################
##									##
##  This routine saves a mark for each REI for debugging		##
##									##
##    Instead of 'call_pal pal_REI', do 'bsr $2,oz_hwaxp_rei'		##
##									##
##########################################################################

	.p2align 4
	.globl	oz_hwaxp_rei
	.type	oz_hwaxp_rei,@function
oz_hwaxp_rei:
	mov	$0,$6					# save this contents from wrath of RD_PS and RSCC
	mov	$1,$7					# ... and this too just in case
	call_pal pal_RD_PS				# see if we're in kernel mode
	and	$0,0x18,$0
	bne	$0,rei_doit				# if not, don't try any of this
	ldq	$0,56($sp)				# also get PS we're returning out to
	and	$0,0x18,$0				# see if it's kernel mode
	beq	$0,rei_doit				# if so, don't bother logging it
	call_pal pal_RSCC				# kernel mode to user mode, get what time it is
	bsr	$3,x3					# set up read-only base register
x3:
	ldq	$4,reitbl_p-x3($3)			# point to oz_hwaxp_reitbl
	ldq	$5,oz_hwaxp_reiofs-oz_hwaxp_reitbl($4)	# get oz_hwaxp_reiofs (offset in table for next datum)
	addq	$5,$4,$5				# add offset to table pointer
	stq	$0, 0($5)				# store cycle counter
	stq	$2, 8($5)				# store where the bsr $2,oz_hwaxp_rei was from
	ldq	$0,48($sp)				# copy the destination PC & PS
	ldq	$1,56($sp)
	stq	$0,16($5)
	stq	$1,24($5)
	call_pal pal_MFPR_PTBR
	stq	$0,40($5)

	lda	$2,OZ_HWAXP_REIMAX*OZ_HWAXP_REISIZ	# get total bytes in array
	subq	$5,$4,$5				# get offset again
	addq	$5,OZ_HWAXP_REISIZ,$5			# increment to next entry
	subq	$2,$5,$2				# see if reached end of array
	cmoveq	$2,0,$5					# if so, reset to beginning
	stq	$5,oz_hwaxp_reiofs-oz_hwaxp_reitbl($4)

rei_doit:
	mov	$6,$0					# restore R0 and R1
	mov	$7,$1
	call_pal pal_REI				# off we go

	.p2align 4
	.globl	oz_hwaxp_reidump
	.type	oz_hwaxp_reidump,@function
oz_hwaxp_reidump:
	subq	$sp,32,$sp
	stq	 $9, 0($sp)
	stq	$13, 8($sp)
	stq	$14,16($sp)
	stq	$26,24($sp)

	lda	$13,tb-oz_hwaxp_reidump($27)
	ldq	$14,db_p-tb($13)
	ldq	 $9,oz_hwaxp_reiofs-db($14)
	subq	$sp,24,$sp
reidump_loop:
	lda	 $0,OZ_HWAXP_REIMAX*OZ_HWAXP_REISIZ
	cmoveq	 $9,$0,$9
	subq	 $9,OZ_HWAXP_REISIZ,$9
	addq	 $9,$14,$0
	lda	$16,reidump_fmt-tb($13)
	mov	 $9,$17
	ldq	$27,printk_p-tb($13)
	ldq	 $1,oz_hwaxp_reitbl+40-db($0)	# ptbr
	ldq	$18,oz_hwaxp_reitbl+ 0-db($0)	# cycle counter
	ldq	$19,oz_hwaxp_reitbl+ 8-db($0)	# pc of the bsr oz_hwaxp_rei
	ldq	$20,oz_hwaxp_reitbl+16-db($0)	# pc being returned to
	ldl	$21,oz_hwaxp_reitbl+24-db($0)	# ps being returned to
	stq	 $1, 0($sp)
	jsr	$26,($27)
	ldq	 $0,oz_hwaxp_reiofs-db($14)
	subq	 $0,$9,$0
	bne	 $0,reidump_loop
	addq	$sp,24,$sp

	ldq	 $9, 0($sp)
	ldq	$13, 8($sp)
	ldq	$14,16($sp)
	ldq	$26,24($sp)
	addq	$sp,32,$sp
	ret	$31,($26)
