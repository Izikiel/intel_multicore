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
##  Condition handler routines						##
##									##
##  The condition handler routines use frames pointed to by the 	##
##  UNiQue register							##
##									##
##  A condition handler is used for an exception iff current level = 	##
##  level the condition handler was declared.				##
##									##
##########################################################################

	.include "oz_params_axp.s"

	.set	noat
	.set	nomacro

	FRAME_FP   =   0	# next outer handler (0 if none)
	FRAME_R9   =   8	# registers on call to oz_sys_condhand
	FRAME_R10  =  16
	FRAME_R11  =  24
	FRAME_R12  =  32
	FRAME_R13  =  40
	FRAME_R14  =  48
	FRAME_R15  =  56
	FRAME_R16  =  64	# (tryentry)
	FRAME_R17  =  72	# (tryparam)
	FRAME_R18  =  80	# (catchent)
	FRAME_R19  =  88	# (catchprm)
	FRAME_R26  =  96	# (return to caller of oz_sys_condhand)
	FRAME_PS   = 104	# processor status on call to oz_sys_condhand
	FRAME_SMP  = 112	# smplevel on call to oz_sys_condhand (0 if not kernel mode)
	FRAME_SIZE = 120	# size of stack frame

	.text
	.p2align 3

condhand_def_p:	.quad	oz_sys_condhand_default
cpu_smplevel_p:	.quad	oz_hw_cpu_smplevel
resignal_p:	.quad	oz_RESIGNAL
resume_p:	.quad	oz_RESUME

##########################################################################
##									##
##  Try a routine and catch any generated signals			##
##									##
##    Input:								##
##									##
##	R16 = entrypoint of routine to try				##
##	R17 = parameter to pass to it					##
##	R18 = entrypoint of catch routine				##
##	R19 = parameter to pass to it					##
##	smplevel = anything (including user mode)			##
##									##
##    Output:								##
##									##
##	R0 = as returned by try or catch routine			##
##									##
##########################################################################

# uLong oz_sys_condhand_try (OZ_Signal_tryentry tryentry, void *tryparam, OZ_Signal_catchent catchent, void *catchprm)

	.p2align 4
	.globl	oz_sys_condhand_try
	.type	oz_sys_condhand_try,@function
oz_sys_condhand_try:
	call_pal pal_READ_UNQ			# get old frame pointer into R0
	subq	$sp,FRAME_SIZE,$sp		# make new frame on stack
	stq	 $0,FRAME_FP($sp)		# save frame contents
	stq	 $9,FRAME_R9($sp)
	stq	$10,FRAME_R10($sp)
	stq	$11,FRAME_R11($sp)
	stq	$12,FRAME_R12($sp)
	stq	$13,FRAME_R13($sp)
	stq	$14,FRAME_R14($sp)
	stq	$15,FRAME_R15($sp)
	stq	$16,FRAME_R16($sp)
	stq	$17,FRAME_R17($sp)
	stq	$18,FRAME_R18($sp)
	stq	$19,FRAME_R19($sp)
	stq	$26,FRAME_R26($sp)
	mov	$27,$13				# save my base somewhere safe
	a13 = oz_sys_condhand_try
	call_pal pal_RD_PS
	and	 $0,0x18,$1
	stq	 $0,FRAME_PS($sp)
	mov	$31,$0				# assume usermode smplevel zero
	bne	 $1,condhand_try_setactive
	ldq	$27,cpu_smplevel_p-a13($13)	# kernel mode, get actual smplevel
	jsr	$26,($27)
condhand_try_setactive:
	stq	 $0,FRAME_SMP($sp)
	mov	$sp,$16				# make the frame active
	call_pal pal_WRITE_UNQ

	ldq	$27,FRAME_R16($sp)		# call the routine to try
	ldq	$16,FRAME_R17($sp)
	jsr	$26,($27)
	mov	 $0,$2				# save the return status

	ldq	$16,FRAME_FP($sp)		# pop the frame from chain
	call_pal pal_WRITE_UNQ

	ldq	$13,FRAME_R13($sp)		# restore scratch registers
	ldq	$26,FRAME_R26($sp)		# get return address
	addq	$sp,FRAME_SIZE,$sp		# pop the frame from stack

	mov	 $2,$0				# restore return status
	ret	$31,($26)			# return to caller

##########################################################################
##									##
##  This routine is called by the kernel when a condition is signalled	##
##									##
##    Input:								##
##									##
##	R16      = pointer to signal arguments				##
##	R17      = pointer to machine arguments				##
##	smplevel = hopefully same as corresponding call to 		##
##	           oz_sys_condhand_try, or the handler will be ingored	##
##									##
##    Output:								##
##									##
##	R0 = 1 : resume execution at point of interruption		##
##	     0 : no handler found for the smplevel, so abort		##
##	... or the stack may have been unwound and it returns to 	##
##	    caller of oz_sys_condhand_try with R0 = status from 	##
##	    condition handler						##
##									##
##########################################################################

# int oz_sys_condhand_call (OZ_Sigargs *sigargs, OZ_Mchargs *mchargs)

	.p2align 4
	.globl	oz_sys_condhand_call
	.type	oz_sys_condhand_call,@function
oz_sys_condhand_call:
	subq	$sp,48,$sp
	stq	 $9, 0($sp)		# save scratch registers
	stq	$10, 8($sp)
	stq	$11,16($sp)
	stq	$12,24($sp)
	stq	$13,32($sp)
	stq	$26,40($sp)
	mov	$27,$13			# use R13 for base register
	b13 = oz_sys_condhand_call
	call_pal pal_READ_UNQ		# get frame pointer into R0
	mov	 $0,$10			# save it
	call_pal pal_RD_PS		# get processor status in R0
	and	 $0,0x18,$1		# get the current mode
	mov	 $0,$11			# save it
	mov	$31,$12			# assume usermode smplevel is zero
	bne	 $1,call_loop
	ldq	$27,cpu_smplevel_p-b13($13)
	jsr	$26,($27)		# kernel mode, get actual smplevel
	mov	 $0,$12
call_loop:
	beq	$10,call_abort		# abort thread if ran out of frames
call_loop2:
	ldq	 $0,FRAME_PS($10)	# see if critical parts of PS match
	lda	 $2,0x1F18
	ldq	 $1,FRAME_SMP($10)	# make sure smplevel matches
	xor	 $0,$11,$0
	subq	 $1,$12,$1
	and	 $0,$2,$0		# ... like IPL and CM
	bne	 $1,call_next
	bne	 $0,call_next

	ldq	$27,FRAME_R18($10)	# get routine entrypoint
	ldq	$16,FRAME_R19($10)	# get routine parameter
	beq	$27,call_next		# sorry, already disabled
	stq	$31,FRAME_R18($10)	# disable in case of nested signals
	mov	$27,$9			# save it for restoration
	jsr	$26,($27)		# ok, call handler
	stq	 $9,FRAME_R18($10)	# reactivate handler

	ldq	 $1,resignal_p-b13($13)	# see if it returned OZ_RESIGNAL
	ldl	 $1,0($1)
	subl	 $1,$0,$1
	bne	 $1,call_processed	# if not, the signal was processed
					# if so, it can't do it, keep looking

call_next:
	ldq	$10,FRAME_FP($10)	# can't use this one, try next outer
	bne	$10,call_loop2
call_abort:
	mov	  0,$0			# can't find an handler, return 'abort' status
	br	$31,call_return

call_processed:
	ldq	 $1,resume_p-b13($13)	# see if it returned OZ_RESUME
	ldl	 $1,0($1)		# ... to resume execution where we left off
	subl	 $1,$0,$1
	bne	 $1,call_unwind		# if not, unwind to caller of oz_sys_condhand_try
	mov	  1,$0			# if so, return 'resume' status
call_return:
	ldq	 $9, 0($sp)		# restore scratch registers
	ldq	$10, 8($sp)
	ldq	$11,16($sp)
	ldq	$12,24($sp)
	ldq	$13,32($sp)
	ldq	$26,40($sp)
	addq	$sp,48,$sp
	ret	$31,($26)

call_unwind:
	mov	 $0,$3			# make status safe from WRITE_UNQ
	ldq	$16,FRAME_FP($10)	# pop frame from links
	call_pal pal_WRITE_UNQ
	mov	$10,$0			# restore registers
	ldq	 $9,FRAME_R9($0)
	ldq	$10,FRAME_R10($0)
	ldq	$11,FRAME_R11($0)
	ldq	$12,FRAME_R12($0)
	ldq	$13,FRAME_R13($0)
	ldq	$14,FRAME_R14($0)
	ldq	$15,FRAME_R15($0)
	ldq	$26,FRAME_R26($0)
	addq	 $0,FRAME_SIZE,$sp	# wipe stack as on entry to oz_sys_condhand_try
	mov	 $3,$0			# restore return status
	ret	$31,($26)		# return to caller of oz_sys_condhand_try

##########################################################################
##									##
##  Call the currently active condition handler with a signal array	##
##									##
##	oz_sys_condhand_signal (nargs, args, ...)			##
##									##
##  All args and nargs are of type OZ_Sigargs				##
##									##
##  This is implemented so it appears as though the call instruction 	##
##  generated a trap of the exception type given by the call args	##
##									##
##########################################################################

	.p2align 4
	.globl	oz_sys_condhand_signal
	.type	oz_sys_condhand_signal,@function
oz_sys_condhand_signal:
	lda	$sp,-MCH__SIZE-48($sp)	# combined room for machine arguments and signal array

	stq	$16,MCH__SIZE+ 0($sp)	# finish signal array
	stq	$17,MCH__SIZE+ 8($sp)
	stq	$18,MCH__SIZE+16($sp)
	stq	$19,MCH__SIZE+24($sp)
	stq	$20,MCH__SIZE+32($sp)
	stq	$21,MCH__SIZE+40($sp)

	stq	 $0,MCH_Q_R0($sp)	# create pseudo machine arguments
	stq	 $1,MCH_Q_R1($sp)
	stq	 $2,MCH_Q_R2($sp)
	stq	 $3,MCH_Q_R3($sp)
	stq	 $4,MCH_Q_R4($sp)
	stq	 $5,MCH_Q_R5($sp)
	stq	 $6,MCH_Q_R6($sp)
	stq	 $7,MCH_Q_R7($sp)
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
	stq	$29,MCH_Q_R29($sp)
	stq	$sp,MCH_Q_R30($sp)
	stq	$26,MCH_Q_PC($sp)
	mov	$27,$13
	c13 = oz_sys_condhand_signal
	call_pal pal_RD_PS
	stq	$31,MCH_Q_P2($sp)
	stq	$31,MCH_Q_P3($sp)
	stq	$31,MCH_Q_P4($sp)
	stq	$31,MCH_Q_P5($sp)
	stq	 $0,MCH_Q_PS($sp)

	lda	$27,oz_sys_condhand_call-c13($13)
	lda	$16,MCH__SIZE($sp)	# point to sigargs just past the mchargs
	lda	$17,0($sp)		# point to mchargs on the stack
	jsr	$26,($27)		# call appropriate condition handler
	bne	$0,signal_rtn
	ldq	$27,condhand_def_p-c13($13)
	jsr	$26,($27)		# none active, use default (ie, puque)
signal_rtn:
	ldq	 $9,MCH_Q_R9($sp)	# restore non-volatile registers
	ldq	$10,MCH_Q_R10($sp)
	ldq	$11,MCH_Q_R11($sp)
	ldq	$12,MCH_Q_R12($sp)
	ldq	$13,MCH_Q_R13($sp)
	ldq	$14,MCH_Q_R14($sp)
	ldq	$15,MCH_Q_R15($sp)
	ldq	$26,MCH_Q_PC($sp)	# ... and the return address
	lda	$sp,MCH__SIZE+48($sp)	# wipe from stack
	ret	$31,($26)		# return to caller

##########################################################################
##									##
##  Call the currently active condition handler with a signal array	##
##									##
##	oz_sys_condhand_signalv (sigargs[])				##
##									##
##  This is implemented so it appears as though the call instruction 	##
##  generated a trap of the exception type given by the call args	##
##									##
##########################################################################

	.p2align 4
	.globl	oz_sys_condhand_signalv
	.type	oz_sys_condhand_signalv,@function
oz_sys_condhand_signalv:
	lda	$sp,-MCH__SIZE($sp)	# room for machine arguments

	stq	 $0,MCH_Q_R0($sp)	# create pseudo machine arguments
	stq	 $1,MCH_Q_R1($sp)
	stq	 $2,MCH_Q_R2($sp)
	stq	 $3,MCH_Q_R3($sp)
	stq	 $4,MCH_Q_R4($sp)
	stq	 $5,MCH_Q_R5($sp)
	stq	 $6,MCH_Q_R6($sp)
	stq	 $7,MCH_Q_R7($sp)
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
	stq	$29,MCH_Q_R29($sp)
	stq	$sp,MCH_Q_R30($sp)
	stq	$26,MCH_Q_PC($sp)
	mov	$27,$13
	d13 = oz_sys_condhand_signal
	call_pal pal_RD_PS
	stq	$31,MCH_Q_P2($sp)
	stq	$31,MCH_Q_P3($sp)
	stq	$31,MCH_Q_P4($sp)
	stq	$31,MCH_Q_P5($sp)
	stq	 $0,MCH_Q_PS($sp)

	lda	$27,oz_sys_condhand_call-d13($13)
	ldq	$16,MCH_Q_R16($sp)	# point to sigargs that were in R16 on entry
	lda	$17,0($sp)		# point to mchargs on the stack
	jsr	$26,($27)		# call appropriate condition handler
	bne	$0,signalv_rtn
	ldq	$27,condhand_def_p-d13($13)
	jsr	$26,($27)		# none active, use default (ie, puque)
signalv_rtn:
	ldq	 $9,MCH_Q_R9($sp)	# restore non-volatile registers
	ldq	$10,MCH_Q_R10($sp)
	ldq	$11,MCH_Q_R11($sp)
	ldq	$12,MCH_Q_R12($sp)
	ldq	$13,MCH_Q_R13($sp)
	ldq	$14,MCH_Q_R14($sp)
	ldq	$15,MCH_Q_R15($sp)
	ldq	$26,MCH_Q_PC($sp)	# ... and the return address
	lda	$sp,MCH__SIZE($sp)	# wipe from stack
	ret	$31,($26)		# return to caller

##########################################################################
##									##
##  Return successive machine args for call frames			##
##									##
##    Input:								##
##									##
##	 4(%esp) = callback entrypoint					##
##	 8(%esp) = callback parameter					##
##	12(%esp) = max number of frames to trace (or -1 for all)	##
##	16(%esp) = mchargs pointer (or NULL to create own)		##
##	20(%esp) = readmem routine (or NULL for local)			##
##									##
##    Note:								##
##									##
##	callback routine is called:					##
##									##
##		entrypoint (parameter, machine_args)			##
##									##
##	readmem routine is called:					##
##									##
##		ok = readmem (parameter, buff, size, addr)		##
##									##
##########################################################################

	.text
	.align	4
	.globl	oz_hw_traceback
	.type	oz_hw_traceback,@function
oz_hw_traceback:
	ret	$31,($26)			# yeah, right
