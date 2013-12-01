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

	.text

	.p2align 4
tb:

cpudb_p:		.quad	oz_hwaxp_cpudb
crash_p:		.quad	oz_crash
firsthread_p:		.quad	oz_hw_process_firsthread
gethwctx_p:		.quad	oz_knl_thread_gethwctx
initial_fen:		.quad	0x8000000000000000
kstack_create_p:	.quad	oz_hw486_kstack_create
kstack_delete_p:	.quad	oz_hw486_kstack_delete
nppmallo_p:		.quad	oz_knl_nppmallo
nppfree_p:		.quad	oz_knl_nppfree
printk_p:		.quad	oz_knl_printk
thread_exit_p:		.quad	oz_sys_thread_exit
thstart_p:		.quad	oz_knl_thread_start
updaststate_p:		.quad	oz_hwaxp_updaststate
ustack_create_p:	.quad	oz_hw486_ustack_create
ustack_delete_p:	.quad	oz_hw486_ustack_delete
ustackvpage:		.quad	1<<(L2VASIZE-L2PAGESIZE-1)

thread_init_either_msg1: .string "oz_hw_thread_initctx: oz_sys_thread returned status %u"
thread_switchctx_msg1:	.string	"oz_hw_thread_switchctx: old ctx %p, cpudb ctx %p"

initctx_deb1:		.string	"oz_hw_thread_initctx*: initial thctx %p\n"
initctx_deb2:		.string	"oz_hw_thread_initctx*: created thctx %p\n"
switchctx_deb1:		.string	"oz_hw_thread_switchctx*: old %p, new %p\n"
aststate_deb1:		.string	"oz_hw_thread_aststate*: procmode %u, state %u, cpu %d, aststate %X\n"

tracedump_msg1:		.string	"oz_hw_thread_tracedump: ustksiz %Lu, ustkvpg %LX, kstackva %QX, ksp %QX\n"
tracedump_msg2:		.string	"oz_hw_thread_tracedump: usp %QX, fen %QX, unq %QX\n"

fpdis_kernel_msg:	.string	"oz_hwaxp_fpdis: kernel use of FPU at PC %QX"

##########################################################################
##									##
##  Initialize thread hardware context block				##
##									##
##    Input:								##
##									##
##	R16 = pointer to hardware context block				##
##	R17 = number of user stack pages				##
##	      0 if kernel-mode only thread				##
##	R18 = entrypoint, or 0 if initializing on cpu			##
##	R19 = parameter							##
##	R20 = process hardware context block pointer			##
##	R21 = thread software context block pointer			##
##									##
##	smplock = ts							##
##									##
##    Output:								##
##									##
##	R0 = OZ_SUCCESS : successful					##
##	           else : error status					##
##	thread hardware context block = initialized so that call to 	##
##	oz_hw_thread_switchctx will set up to call the thread routine	##
##									##
##########################################################################

	.p2align 4
	.globl	oz_hw_thread_initctx
	.type	oz_hw_thread_initctx,@function
oz_hw_thread_initctx:
	subq	$sp,32,$sp
	stq	$10, 0($sp)
	stq	$11, 8($sp)
	stq	$13,16($sp)
	stq	$26,24($sp)
	lda	$13,tb-oz_hw_thread_initctx($27)

	mov	$16,$10					# save hw ctx pointer
	mov	$21,$11					# save sw ctx pointer

##  Initialize hardware context area to zeroes

	mov	THCTX__SIZE/8,$0			# get size of hardware context block (in quads)
	mov	$16,$1					# point to hardware context block
thread_initctx_clr:
	stq	$31,0($1)				# zero it out
	subq	 $0,1,$0
	addq	 $1,8,$1
	bne	 $0,thread_initctx_clr

##  If we're initializing this cpu as a thread, just skip the rest

	beq	$18,thread_initctx_cpu

##  Set up user stack parameters - stack gets created when thread starts

	stl	$17,THCTX_L_USTKSIZ($10)		# number of pages in user stack, 0 for kernel only

##  Allocate a kernel stack

	subq	$sp,24,$sp
	stq	$18, 0($sp)
	stq	$19, 8($sp)
	stq	$20,16($sp)

	ldq	$27,kstack_create_p-tb($13)		# set up kernel stack memorie
	mov	$10,$16					# - thctx pointer
	lda	$17,THCTX_Q_KSTACKVA($10)		# - stack virtual address (top)
	jsr	$26,($27)				# attempt to allocate

	ldq	$18, 0($sp)
	ldq	$19, 8($sp)
	ldq	$20,16($sp)
	addq	$sp,24,$sp

	subl	 $0,OZ_SUCCESS,$1
	bne	 $1,thread_initctx_rtn
	ldq	 $0,THCTX_Q_KSTACKVA($10)		# get top of stack

##  Set up what we want to happen when the 'RET' of the oz_hw_thread_switchctx routine executes

	lda	 $0,-THSAV__SIZE($0)			# make room for thread save block
	lda	 $1,thread_start-tb($13)		# return address = thread_start routine
	ldq	 $2,initial_fen-tb($13)			# initial fen value

	mov	  3,$3					# get what we want for FPCR<DYN> bits, according to arch ref man
	stq	$11,THSAV_Q_R9($0)			# set  R9 = thread software context pointer
	stq	$10,THSAV_Q_R10($0)			# set R10 = thread hardware context pointer
	stq	$18,THSAV_Q_R11($0)			# set R11 = entrypoint
	stq	$19,THSAV_Q_R12($0)			# set R12 = parameter
	stq	$20,THSAV_Q_R13($0)			# set R13 = process hardware context block pointer
							# R14,R15 = garbage
	sll	 $3,58,$3				# slide over the FPCR<DYN> bits into place
	stq	 $1,THSAV_Q_R26($0)			# set R26 = where to return to
	stq	$31,THSAV_Q_USP($0)			# clear out user stack pointer for now
	stq	 $2,THSAV_Q_FEN($0)			# set FEN = disable DATFX reporting
	stq	$31,THSAV_Q_UNQ($0)			# clear out 'unique' value (exception frame pointer)

	stq	 $3,THCTX_Q_FCR($10)			# save initial FPCR value (FPCR<DYN> bits=11)
	stq	 $0,THCTX_Q_KSP($10)			# save initial stack pointer

		####	lda	$16,initctx_deb2-tb($13)
		####	mov	$10,$17
		####	ldq	$27,printk_p-tb($13)
		####	jsr	$26,($27)

thread_initctx_rtnsuc:
	mov	OZ_SUCCESS,$0
thread_initctx_rtn:
	ldq	$10, 0($sp)
	ldq	$11, 8($sp)
	ldq	$13,16($sp)
	ldq	$26,24($sp)
	addq	$sp,32,$sp
	ret	$31,($26)

thread_initctx_cpu:
	ldq	$1,cpudb_p-tb($13)			 # save initial hardware context pointer
	stq	$10,CPU_Q_THCTX($1)

		####	lda	$16,initctx_deb1-tb($13)
		####	mov	$10,$17
		####	ldq	$27,printk_p-tb($13)
		####	jsr	$26,($27)

	br	thread_initctx_rtnsuc

##########################################################################
##									##
##  The oz_hw_thread_switchctx has just executed its 'RET' instruction	##
##  We are now in the new thread's context in kernel mode		##
##									##
##    Input:								##
##									##
##	 $9 = thread software context block pointer			##
##	$10 = thread hardware context block pointer			##
##	$11 = thread routine entrypoint					##
##	$12 = thread routine parameter					##
##	$13 = process hardware context block pointer			##
##	$26 = address of thread_start					##
##	$sp = stack completely empty					##
##									##
##########################################################################

	.p2align 4
thread_start:
	lda	$15,tb-thread_start($26)

	## Do kernel mode thread startup, comes back at smplock_null level

	ldq	$27,thstart_p-tb($15)
	mov	 $9,$16				# - thread software context block pointer
	jsr	$26,($27)			# call oz_knl_thread_start

	## Maybe pagetable needs to be mapped

	ldq	$27,firsthread_p-tb($15)
	mov	$13,$16				# - process hardware context block pointer
	jsr	$26,($27)			# call oz_hw_process_firsthread

	## Create user stack

	ldq	 $0,ustackvpage-tb($15)		# (suggest somewhere at end of upper VA range)
	ldq	$27,ustack_create_p-tb($15)
	ldl	$16,THCTX_L_USTKSIZ($10)	# get number of pages for user stack
	lda	$17,THCTX_L_USTKVPG($10)	# this is where to put base virtual page number
	stl	 $0,0($17)			# (we need to suggest a page)
	beq	$16,thread_init_either
	jsr	$26,($27)			# call oz_hw486_ustack_create

	mov	 $0,$16				# set it up as my user stack pointer
	call_pal pal_MTPR_USP

	## Return out to user mode

	and	$sp,63,$0			# save low 6 bits of KSP
	bic	$sp,63,$sp			# 64-byte align KSP
	lda	$27,thread_init_either-tb($15)	# this is where to jump to
	sll	 $0,56,$0			# put KSP align bits in place
	subq	$sp,64,$sp			# make room for REI frame on kernel stack
	or	 $0,0x18,$0			# here's where we set it to user mode
	stq	$27,48($sp)			# this is the saved PC
	stq	 $0,56($sp)			# this is the saved PS
	call_pal pal_REI			# jumps to thread_init_either in user mode

##########################################################################
##									##
##  We are now in user mode for the first time				##
##  (This routine is also used for kernel mode threads)			##
##									##
##    Input:								##
##									##
##	$11 = thread routine entrypoint					##
##	$12 = thread routine parameter					##
##	$15 = text base (tb)						##
##	$27 = address of thread_init_either				##
##	$sp = stack completely empty					##
##									##
##########################################################################

	.p2align 4
thread_init_either:
	mov	$11,$27			# set up thread routine entrypoint
	mov	$12,$16			# set up thread routine parameter
	jsr	$26,($27)		# call thread routine

	ldq	$27,thread_exit_p-tb($15)
	mov	 $0,$16			# get exit status from return value
	jsr	$26,($27)		# call oz_sys_thread_exit

	ldq	$27,crash_p-tb($15)	# it shouldn't return
	lda	$16,thread_init_either_msg1-tb($15)
	mov	 $0,$17
	jsr	$26,($27)
	call_pal pal_HALT


##########################################################################
##									##
##  Terminate as much as possible about the thread while still in its 	##
##  context								##
##									##
##    Input:								##
##									##
##	R16 = pointer to thread hardware context block			##
##	smplock = softint						##
##									##
##    Output:								##
##									##
##	everything possible cleared out while in thread context		##
##									##
##########################################################################

	.p2align 4
	.globl	oz_hw_thread_exited
	.type	oz_hw_thread_exited,@function
oz_hw_thread_exited:

	## Unmap and delete the user stack section, oz_hw_thread_termctx deletes kernel stack

	ldl	$0,THCTX_L_USTKSIZ($16)		# see if a user stack was created
	ldl	$16,THCTX_L_USTKVPG($16)	# ok, unmap the section
	beq	$0,thread_exited_rtn		# all done if there wasn't one created
	ldq	$27,ustack_delete_p-oz_hw_thread_exited($27)
	zapnot	$16,15,$16
	jmp	$31,($27)
thread_exited_rtn:
	ret	$31,($26)

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
	.type	oz_hw_thread_termctx,@function
oz_hw_thread_termctx:
	c27 = oz_hw_thread_termctx
	subq	$sp,8,$sp
	stq	$26,0($sp)

##  Free memory used for its kernel stack
##  If KSTACKVA is zero, it means this is the cpu's initial thread

	ldq	$17,THCTX_Q_KSTACKVA($16)	# get vaddr of top of kernel stack
	ldq	$27,kstack_delete_p-c27($27)
	beq	$17,thread_termctx_rtn		# if zero, it's a nop for us
	stq	$31,THCTX_Q_KSTACKVA($16)	# zero it out
	jsr	$26,($27)			# call oz_hw486_kstack_delete (r16=thctx, r17=kstackva)
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
##	smplevel = tp (old thread locked)				##
##									##
##########################################################################

	.p2align 4
	.globl	oz_hw_thread_switchctx
	.type	oz_hw_thread_switchctx,@function
oz_hw_thread_switchctx:
	d27 = oz_hw_thread_switchctx
	mov	$16,$6				# safe from MFPR/MTPR
	mov	$17,$7
	lda	$sp,-THSAV__SIZE($sp)
	stq	 $9,THSAV_Q_R9($sp)		# save C registers
	stq	$10,THSAV_Q_R10($sp)
	stq	$11,THSAV_Q_R11($sp)
	stq	$12,THSAV_Q_R12($sp)
	stq	$13,THSAV_Q_R13($sp)
	stq	$14,THSAV_Q_R14($sp)
	stq	$15,THSAV_Q_R15($sp)
	stq	$26,THSAV_Q_R26($sp)		# ... and the return address
	call_pal pal_MFPR_USP			# save user stack pointer
	stq	 $0,THSAV_Q_USP($sp)
	call_pal pal_READ_UNQ			# save 'unique' (exception frame pointer)
	stq	 $0,THSAV_Q_UNQ($sp)

	call_pal pal_MFPR_WHAMI			# point to cpudb for this CPU
	ldq	 $2,cpudb_p-d27($27)
	sll	 $0,CPU__L2SIZE,$0
	addq	 $2,$0,$2
	ldl	 $3,CPU_L_ACTHWPCB($2)		# see which HWPCB is active
	s8addq	 $3,$2,$3			# point to the active HWPCB
	ldq	 $3,CPU_Q_HWPCB0_VA($3)

	ldq	$18,CPU_Q_THCTX($2)		# make sure old context matches
	subq	$18,$6,$0
	bne	 $0,thread_switchctx_crash1

	mov	$sp,$9
	stq	$sp,THCTX_Q_KSP($6)		# save current kernel stack pointer
	ldq	$sp,THCTX_Q_KSP($7)		# we're now on the new thread's kernel stack

	ldq	$11,HWPCB_Q_FEN($3)		# get CPU's current FEN bits
	ldq	$10,THSAV_Q_FEN($sp)		# get new FEN bits
	ldl	 $5,THCTX_L_ASTSTATE($7)	# get new AST pending bits
	blbs	$11,thread_switchctx_savefp	# save fp context if floatingpoint was enabled
thread_switchctx_fpsaved:			# comes back here with R11<00> cleared
	stq	$11,THSAV_Q_FEN($9)		# save current FEN bits in old thread
	xor	$10,$11,$12			# see what needs setting
	stq	 $7,CPU_Q_THCTX($2)		# save new context block pointer as current on the CPU
	bne	$12,thread_switchctx_setfen	# if new is different, go do the required MTPR's
thread_switchctx_fenok:

	sll	 $5,4,$16			# set up new AST pending bits
	call_pal pal_MTPR_ASTSR
	mov	0x99,$16			# always set AST interrupt enables
	call_pal pal_MTPR_ASTEN

	ldq	$16,THSAV_Q_UNQ($sp)		# restore 'unique' (exception frame pointer)
	call_pal pal_WRITE_UNQ
	ldq	$16,THSAV_Q_USP($sp)		# restore user stack pointer
	call_pal pal_MTPR_USP
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

	## Thread was using floatingpoint, save state and clear enabled bit for new thread

	##  R2 = CPUDB pointer
	##  R3 = HWPCB pointer
	##  R6 = old THCTX pointer
	##  R7 = new THCTX pointer
	##  R9 = old THSAV pointer
	## R10 = new bits (from new THCTX)
	## R11 = old bits (from HWPCB)
	##  SP = new THSAV pointer
	## (must also preserve R5)

	.p2align 4
thread_switchctx_savefp:
	stt	 $f0,THCTX_Q_F0($6)		# save FP registers
	stt	 $f1,THCTX_Q_F1($6)
	stt	 $f2,THCTX_Q_F2($6)
	stt	 $f3,THCTX_Q_F3($6)
	stt	 $f4,THCTX_Q_F4($6)
	stt	 $f5,THCTX_Q_F5($6)
	stt	 $f6,THCTX_Q_F6($6)
	stt	 $f7,THCTX_Q_F7($6)
	stt	 $f8,THCTX_Q_F8($6)
	stt	 $f9,THCTX_Q_F9($6)
	stt	$f10,THCTX_Q_F20($6)
	stt	$f11,THCTX_Q_F21($6)
	stt	$f12,THCTX_Q_F22($6)
	stt	$f13,THCTX_Q_F23($6)
	stt	$f14,THCTX_Q_F24($6)
	stt	$f15,THCTX_Q_F25($6)
	stt	$f16,THCTX_Q_F26($6)
	stt	$f17,THCTX_Q_F27($6)
	stt	$f18,THCTX_Q_F28($6)
	stt	$f19,THCTX_Q_F29($6)
	stt	$f20,THCTX_Q_F20($6)
	stt	$f21,THCTX_Q_F21($6)
	stt	$f22,THCTX_Q_F22($6)
	stt	$f23,THCTX_Q_F23($6)
	stt	$f24,THCTX_Q_F24($6)
	stt	$f25,THCTX_Q_F25($6)
	stt	$f26,THCTX_Q_F26($6)
	stt	$f27,THCTX_Q_F27($6)
	stt	$f28,THCTX_Q_F28($6)
	stt	$f29,THCTX_Q_F29($6)
	stt	$f30,THCTX_Q_F30($6)
	mf_fpcr	 $f0
	stt	 $f0,THCTX_Q_FCR($6)
	bic	$11,1,$11			# clear FEN bit in copy from HWPCB_Q_FEN
	mov	  0,$16				# disable floatingpoint
	call_pal pal_MTPR_FEN			# this also updates HWPCB_Q_FEN($3)
	br	$31,thread_switchctx_fpsaved

	## Some bit(s) in the FEN quad have changed

	##  R2 = CPUDB pointer
	##  R3 = HWPCB pointer
	##  R6 = old THCTX pointer
	##  R7 = new THCTX pointer
	##  R9 = old THSAV pointer
	## R10 = new bits (from new THCTX)
	## R11 = old bits (from HWPCB)
	## R12 = what's different
	##  SP = new THSAV pointer
	## (must also preserve R5)

	.p2align 4
thread_switchctx_setfen:
	bge	$12,thread_switchctx_fenok	# see if DATFX changed
	srl	$10,63,$16			# if so, write new value
	call_pal pal_MTPR_DATFX			# this also updates HWPCB_Q_FEN($3)
	br	$31,thread_switchctx_fenok

##########################################################################
##									##
##  This routine is called as a result of a floating-point-unit 	##
##  disabled fault							##
##									##
##    Input:								##
##									##
##	R2 = address of oz_hwaxp_fpdis					##
##	(KSP) = set up for an REI					##
##									##
##########################################################################

	.p2align 4
	.globl	oz_hwaxp_fpdis
	.type	oz_hwaxp_fpdis,@function
oz_hwaxp_fpdis:
	ldq	 $3,56($sp)			# can't have FP instructions in kernel as interrupt 
	subq	$sp,MCH__SIZE-MCH__REI,$sp
	and	 $3,0x18,$3
	stq	 $0,MCH_Q_R0($sp)		# save registers for MTPR_FEN and oz_knl_thread_gethwctx calls
	stq	 $1,MCH_Q_R1($sp)
	stq	 $8,MCH_Q_R8($sp)
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
	beq	 $3,fpdis_kernel		# can't use FPU in kernel mode
	mov	  1,$16				# enable the FPU
	call_pal pal_MTPR_FEN

	ldq	$27,gethwctx_p-oz_hwaxp_fpdis($2)
	mov	  0,$16				# get thread HW context block pointer
	jsr	$26,($27)			# ... into R0

	ldt	 $f0,THCTX_Q_FCR($0)		# restore FPU context
	mt_fpcr	 $f0
	ldt	 $f0,THCTX_Q_F0($0)
	ldt	 $f1,THCTX_Q_F1($0)
	ldt	 $f2,THCTX_Q_F2($0)
	ldt	 $f3,THCTX_Q_F3($0)
	ldt	 $f4,THCTX_Q_F4($0)
	ldt	 $f5,THCTX_Q_F5($0)
	ldt	 $f6,THCTX_Q_F6($0)
	ldt	 $f7,THCTX_Q_F7($0)
	ldt	 $f8,THCTX_Q_F8($0)
	ldt	 $f9,THCTX_Q_F9($0)
	ldt	$f10,THCTX_Q_F10($0)
	ldt	$f11,THCTX_Q_F11($0)
	ldt	$f12,THCTX_Q_F12($0)
	ldt	$f13,THCTX_Q_F13($0)
	ldt	$f14,THCTX_Q_F14($0)
	ldt	$f15,THCTX_Q_F15($0)
	ldt	$f16,THCTX_Q_F16($0)
	ldt	$f17,THCTX_Q_F17($0)
	ldt	$f18,THCTX_Q_F18($0)
	ldt	$f19,THCTX_Q_F19($0)
	ldt	$f20,THCTX_Q_F20($0)
	ldt	$f21,THCTX_Q_F21($0)
	ldt	$f22,THCTX_Q_F22($0)
	ldt	$f23,THCTX_Q_F23($0)
	ldt	$f24,THCTX_Q_F24($0)
	ldt	$f25,THCTX_Q_F25($0)
	ldt	$f26,THCTX_Q_F26($0)
	ldt	$f27,THCTX_Q_F27($0)
	ldt	$f28,THCTX_Q_F28($0)
	ldt	$f29,THCTX_Q_F29($0)
	ldt	$f30,THCTX_Q_F30($0)

	ldq	 $0,MCH_Q_R0($sp)		# restore scratch that MTPR_FEN and oz_knl_thread_gethwctx might have used
	ldq	 $1,MCH_Q_R1($sp)
	ldq	 $8,MCH_Q_R8($sp)
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
	addq	$sp,MCH__SIZE-MCH__REI,$sp

	call_pal pal_REI			# that's it!

fpdis_kernel:
	ldq	$27,crash_p-oz_hwaxp_fpdis($2)
	lda	$16,fpdis_kernel_msg-oz_hwaxp_fpdis($2)
	ldq	$17,MCH_Q_PC($sp)
	jsr	$26,($27)
	call_pal pal_HALT

##########################################################################
##									##
##  The state of an AST changed for the given thread			##
##									##
##    Input:								##
##									##
##	R16 = thread hardware context block pointer			##
##	R17 = processor mode (OZ_PROCMODE_KNL or _USR)			##
##	R18 = 0 : no ast pending					##
##	      1 : ast pending						##
##	R19 = -1 : thread is not active on any CPU			##
##	    else : thread is active on the indicated CPU		##
##	smplevel = tp							##
##									##
##  This writes the bit to the ASTSR.  It is assumed that all ASTEN 	##
##  bits are 'permanently' set.  If the thread is not active, only 	##
##  its THCTX block is updated.  It is up to the kernel to get the 	##
##  thread activated which will load ASTSR from THCTX.  If it is 	##
##  active on another CPU, then the smp routine is called which will 	##
##  update that CPU's ASTSR from the THCTX block.			##
##									##
##########################################################################

	.p2align 4
	.globl	oz_hw_thread_aststate
	.type	oz_hw_thread_aststate,@function
oz_hw_thread_aststate:
	addl	$17,$17,$0			# always update state bits in THCTX block
	mov	  1,$20
	addl	$17,$0,$17			# OZ_PROCMODE_KNL->0; OZ_PROCMODE_USR->3
	sll	$20,$17,$20
	sll	$18,$17,$18
	mb
aststate_sample:
	ldl_l	 $0,THCTX_L_ASTSTATE($16)	# sample current ASTSTATE value
	bic	 $0,$20,$1			# always clear the bit we are going to change
	or	 $1,$18,$1			# put in the new value for the bit
	cmpeq	 $0,$1,$0			# see if there was a change
	mov	 $1,$3				# meanwhile, save new value
	blbs	 $0,aststate_return		# if not, don't bother doing anything
	stl_c	 $1,THCTX_L_ASTSTATE($16)	# if so, write back to ASTSTATE
	beq	 $1,aststate_updfailed		# repeat if failed
	mb

		####	subq	$sp,32,$sp
		####	stq	 $3, 0($sp)
		####	stq	$19, 8($sp)
		####	stq	$26,16($sp)
		####	stq	$27,24($sp)
		####	lda	$16,aststate_deb1-oz_hw_thread_aststate($27)
		####	ldq	$27,printk_p-oz_hw_thread_aststate($27)
		####	mov	 $3,$20
		####	jsr	$26,($27)
		####	ldq	 $3, 0($sp)
		####	ldq	$19, 8($sp)
		####	ldq	$26,16($sp)
		####	ldq	$27,24($sp)
		####	addq	$sp,32,$sp

	blt	$19,aststate_return		# if not active on any cpu, just let kernel get it going
	call_pal pal_MFPR_WHAMI
	subl	 $0,$19,$0			# see if active on my cpu
	bne	 $0,aststate_othercpu
	sll	 $3,4,$16			# my cpu, update current ASTSR
	addq	$16,15,$16
	call_pal pal_MTPR_ASTSR
aststate_return:
	ret	$31,($26)
aststate_updfailed:
	br	$31,aststate_sample
aststate_othercpu:
	ldq	$27,updaststate_p-oz_hw_thread_aststate($27)
	mov	$19,$16				# update this CPU
	mov	 $3,$17				# ... with these ASTSR bits
	jmp	$31,($27)

	.p2align 4
	.globl	oz_hw_thread_tracedump
	.type	oz_hw_thread_tracedump,@function
oz_hw_thread_tracedump:
	subq	$sp,24,$sp
	stq	$12, 0($sp)
	stq	$13, 8($sp)
	stq	$26,16($sp)

	lda	$13,tb-oz_hw_thread_tracedump($27)
	mov	$16,$12

	ldq	$27,printk_p-tb($13)
	lda	$16,tracedump_msg1-tb($13)
	ldl	$17,THCTX_L_USTKSIZ($12)
	ldl	$18,THCTX_L_USTKVPG($12)
	zapnot	$17,15,$17
	zapnot	$18,15,$18
	ldq	$19,THCTX_Q_KSTACKVA($12)
	ldq	$20,THCTX_Q_KSP($12)
	jsr	$26,($27)

	ldq	 $2,THCTX_Q_KSP($12)
	ldq	$27,printk_p-tb($13)
	lda	$16,tracedump_msg2-tb($13)
	ldq	$17,THSAV_Q_USP($2)
	ldq	$18,THSAV_Q_FEN($2)
	ldq	$19,THSAV_Q_UNQ($2)
	jsr	$26,($27)

	ldq	$12, 0($sp)
	ldq	$13, 8($sp)
	ldq	$26,16($sp)
	addq	$sp,24,$sp
	ret	$31,($26)

