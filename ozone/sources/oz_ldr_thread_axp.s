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
##  Scheduled threads							##
##									##
##########################################################################

	.include "oz_params_axp.s"

	.set	noat
	.set	nomacro

	KERNEL_STACK_SIZE = 16384

	.text

	.p2align 4
crash_p:	.quad	oz_crash
cpudb_p:	.quad	oz_hwaxp_cpudb
nppmallo_p:	.quad	oz_knl_nppmallo
nppfree_p:	.quad	oz_knl_nppfree
thstart_p:	.quad	oz_knl_thread_start
printk_p:	.quad	oz_knl_printk

thread_switchctx_msg1:	.string	"oz_ldr_thread_axp switchctx: old ctx %p, cpudb ctx %p"

initctx_deb1:		.string	"oz_ldr_thread_axp initctx: initial thctx %p\n"
initctx_deb2:		.string	"oz_ldr_thread_axp initctx: created thctx %p\n"
switchctx_deb1:		.string	"oz_ldr_thread_axp switchctx: old %p, new %p\n"

##########################################################################
##									##
##  Initialize thread hardware context block				##
##									##
##    Input:								##
##									##
##	$16 = pointer to hardware context block				##
##	$17 = always 0 in loader					##
##	$18 = entrypoint, or 0 if initializing on cpu			##
##	$19 = parameter							##
##	$20 = always 0 in loader					##
##	$21 = thread software context block pointer			##
##									##
##	smplock = ts							##
##									##
##    Output:								##
##									##
##	thread hardware context block = initialized so that call to 	##
##	oz_hw_thread_switchctx will set up to call the thread routine	##
##									##
##########################################################################

	.p2align 4
	.globl	oz_hw_thread_initctx
oz_hw_thread_initctx:
	subq	$sp,32,$sp
	stq	$10, 0($sp)
	stq	$11, 8($sp)
	stq	$13,16($sp)
	stq	$26,24($sp)
	mov	$27,$13
	a13 = oz_hw_thread_initctx

	mov	$16,$10					# save hw ctx pointer
	mov	$21,$11					# save sw ctx pointer

##  Initialize hardware context area to zeroes

	mov	THCTX__SIZE/8,$0			# get size of hardware context block (in quads)
	mov	$16,$1					# point to hardware context block
thread_initctx_clr:
	stq	$31,0($1)				# zero it out
	subq	$0,1,$0
	addq	$1,8,$1
	bne	$0,thread_initctx_clr

##  If we're initializing this cpu as a thread, just skip the rest

	beq	$18,thread_initctx_cpu

##  Allocate a kernel stack

	ldq	$27,nppmallo_p-a13($13)
	lda	$16,KERNEL_STACK_SIZE			# allocate non-paged pool for kernel stack
	mov	0,$17
	jsr	$26,($27)
	stq	$0,THCTX_Q_KSTACKVA($10)		# save base address of stack

##  Set up what we want to happen when the 'RET' of the oz_hw_thread_switchctx routine executes

####	lda	$0,KERNEL_STACK_SIZE-THSAV__SIZE($0)	# get top address of stack
####							# ... but leave room for 8 quads

	lda	$0,KERNEL_STACK_SIZE($0)	####	# get top of stack
	srl	$0,13,$0			####	# round down to page boundary
	sll	$0,13,$0			####	# ... so it won't get watched
	lda	$0,-THSAV__SIZE($0)		####	# leave room for 8 quads at top

	stq	$11,THSAV_Q_R9($0)			# set R9 = thread sw ctx ptr
							# leave R10..R15 as garbage
	lda	$1,thread_start-a13($13)		# return address = thread_start routine
	stq	$1,THSAV_Q_R26($0)

	stq	$0,THCTX_Q_KSP($10)			# save initial stack pointer

			lda	$16,initctx_deb2-a13($13)
			mov	$10,$17
			ldq	$27,printk_p-a13($13)
			jsr	$26,($27)

thread_initctx_rtn:
	ldq	$10, 0($sp)
	ldq	$11, 8($sp)
	ldq	$13,16($sp)
	ldq	$26,24($sp)
	addq	$sp,32,$sp
	ret	$31,($26)

thread_initctx_cpu:
	ldq	$1,cpudb_p-a13($13)			 # save initial hardware context pointer
	stq	$10,CPU_Q_THCTX($1)

			lda	$16,initctx_deb1-a13($13)
			mov	$10,$17
			ldq	$27,printk_p-a13($13)
			jsr	$26,($27)

	br	thread_initctx_rtn

#
#  Jumped to by oz_hw_thread_switchctx to start a new thread
#
#    Input:
#
#	 $9 = thread software context block pointer
#	$26 = address of thread_start
#	$sp = stack completely empty
#
thread_start:
	b26 = thread_start
	ldq	$27,thstart_p-b26($26)
	mov	$9,$16		# arg 1 = thread software context block pointer
	jsr	$26,($27)	# call 'oz_knl_thread_start', it does not return

##########################################################################
##									##
##  Terminate thread hardware context					##
##									##
##    Input:								##
##									##
##	$16 = pointer to thread hardware context block			##
##	$17 = pointer to process hardware context block			##
##									##
##    Output:								##
##									##
##	@$16 = voided out						##
##									##
##########################################################################

	.p2align 4
	.globl	oz_hw_thread_termctx
oz_hw_thread_termctx:
	c27 = oz_hw_thread_termctx
	subq	$sp,8,$sp
	stq	$26,0($sp)

##  Free memory used for its kernel stack
##  If KSTACKVA is zero, it means this is the cpu's initial thread

	ldq	$0,THCTX_Q_KSTACKVA($16)
	beq	$0,thread_termctx_rtn
	stq	$31,THCTX_Q_KSTACKVA($16)
	ldq	$27,nppfree_p-c27($27)
	mov	$0,$16
	jsr	$26,($27)
thread_termctx_rtn:
	ldq	$26,0($sp)
	addq	$sp,8,$sp
	ret	$31,($26)

##########################################################################
##									##
##  Switch thread hardware context					##
##									##
##	$16 = old thread hardware context block address			##
##	$17 = new thread hardware context block address			##
##	smplevel = ts							##
##									##
##########################################################################

	.p2align 4
	.globl	oz_hw_thread_switchctx
oz_hw_thread_switchctx:
	d27 = oz_hw_thread_switchctx
	lda	$sp,-THSAV__SIZE($sp)
	stq	 $9,THSAV_Q_R9($sp)		# save C registers
	stq	$10,THSAV_Q_R10($sp)
	stq	$11,THSAV_Q_R11($sp)
	stq	$12,THSAV_Q_R12($sp)
	stq	$13,THSAV_Q_R13($sp)
	stq	$14,THSAV_Q_R14($sp)
	stq	$15,THSAV_Q_R15($sp)
	stq	$26,THSAV_Q_R26($sp)		# ... and the return address

		####	mov	$16,$10
		####	mov	$17,$11
		####	mov	$27,$12

		####	lda	$16,switchctx_deb1-d27($27)
		####	mov	$10,$17
		####	mov	$11,$18
		####	ldq	$27,printk_p-d27($27)
		####	jsr	$26,($27)

		####	mov	$10,$16
		####	mov	$11,$17
		####	mov	$12,$27

	ldq	$1,cpudb_p-d27($27)		# make sure old context matches
	ldq	$18,CPU_Q_THCTX($1)
	subq	$18,$16,$0
	bne	$0,thread_switchctx_crash1

	stq	$sp,THCTX_Q_KSP($16)		# save current kernel stack pointer
	ldq	$sp,THCTX_Q_KSP($17)		# we're now on the new thread's kernel stack

	stq	$17,CPU_Q_THCTX($1)		# save new context block pointer

	ldq	 $9,THSAV_Q_R9($sp)		# restore C registers
	ldq	$10,THSAV_Q_R10($sp)
	ldq	$11,THSAV_Q_R11($sp)
	ldq	$12,THSAV_Q_R12($sp)
	ldq	$13,THSAV_Q_R13($sp)
	ldq	$14,THSAV_Q_R14($sp)
	ldq	$15,THSAV_Q_R15($sp)
	ldq	$26,THSAV_Q_R26($sp)		# ... and the return address
	lda	$sp,THSAV__SIZE($sp)
	ret	$31,($26)			# return to new thread

thread_switchctx_crash1:
	mov	$16,$17
	lda	$16,thread_switchctx_msg1-d27($27)
	ldq	$27,crash_p-d27($27)
	jsr	$26,($27)
