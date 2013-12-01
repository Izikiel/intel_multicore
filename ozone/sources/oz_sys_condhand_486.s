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
##  The condition handler routines look for a 'bump' in the call frame 	##
##  chain.  The bump is indicated by a saved ebp < 0x200 followed by 	##
##  the normal stuff.							##
##									##
##  If the condition handler was declared in user mode, the bump has a 	##
##  value of 0x001.  If it was declared in kernel mode, the bump has a 	##
##  value of 0x100 + smplevel.						##
##									##
##  A condition handler is used for an exception iff current level = 	##
##  level the condition handler was declared.				##
##									##
##########################################################################

	.include "oz_params_486.s"	# to get MCH_... symbols

##########################################################################
##									##
##  Try a routine and catch any generated signals			##
##									##
##    Input:								##
##									##
##	 4(%esp) = entrypoint of routine to try				##
##	 8(%esp) = parameter to pass to it				##
##	12(%esp) = entrypoint of catch routine				##
##	16(%esp) = parameter to pass to it				##
##	smplevel = anything (including user mode)			##
##									##
##    Output:								##
##									##
##	%eax = as returned by try or catch routine			##
##									##
##########################################################################

# uLong oz_sys_condhand_try (OZ_Signal_tryentry tryentry, void *tryparam, OZ_Signal_catchent catchent, void *catchprm)

	TRY_TRYENTRY = 12
	TRY_TRYPARAM = 16
	TRY_CATCHENT = 20
	TRY_CATCHPRM = 24

	.text
	.align	4
	.globl	oz_sys_condhand_try
	.type	oz_sys_condhand_try,@function
oz_sys_condhand_try:
	pushl	%ebp				# make a call frame
	pushl	$1				# with a special mark to indicate condition handler present here

	pushl	%edi				# save C registers in case of unwind
	movw	%cs,%ax
	pushl	%esi
	testb	$2,%al
	pushl	%ebx

	jne	try_user
	pushfl					# in kernel mode, 
	cli
	call	oz_hw486_getcpu
	xorl	%eax,%eax
	movb	CPU_B_SMPLEVEL(%esi),%al	# ... set flag to 0x1zz where zz is the current smplevel
	movb	$1,%ah
	popfl
	movl	%eax,12(%esp)
try_user:

	leal	12(%esp),%ebp			# point to the smplevel / flag longword
						# if the oz_sys_condhand_call routine sees ebp pointing to this longword
						# ... it will know there is a condition handler here for it to call

	movl	TRY_TRYENTRY+12(%esp),%eax	# call 'try' routine that might generate signal
	pushl	TRY_TRYPARAM+12(%esp)
	call	*%eax
	addl	$4,%esp

	popl	%ebx				# restore C registers
	popl	%esi
	popl	%edi
	addl	$4,%esp				# get rid of the condition handler flag
	popl	%ebp				# restore frame pointer
	ret					# all done

##########################################################################
##									##
##  This routine is called by the kernel when a condition is signalled	##
##									##
##    Input:								##
##									##
##	4(%esp)  = pointer to signal arguments				##
##	8(%esp)  = pointer to machine arguments				##
##	smplevel = hopefully same as corresponding call to 		##
##	           oz_sys_condhand_try, or the handler will be ingored	##
##									##
##    Output:								##
##									##
##	%eax = 1 : resume execution at point of interruption		##
##	       0 : no handler found for the smplevel, so abort		##
##	... or the stack may have been unwound and it returns to 	##
##	    caller of oz_sys_condhand_try with %eax = status from 	##
##	    condition handler						##
##									##
##########################################################################

# int oz_sys_condhand_call (OZ_Sigargs *sigargs, OZ_Mchargs *mchargs)

	PROC_SIGARGS =  8
	PROC_MCHARGS = 12

	.align	4
	.globl	oz_sys_condhand_call
	.type	oz_sys_condhand_call,@function
oz_sys_condhand_call:
	movl	%ebp,%ecx
	pushl	%ebp
	movl	%esp,%ebp
	pushl	%esi
	pushl	%ebx
	movw	%cs,%dx

call_loop:
	movl	%ecx,%ebx
	movl	(%ecx),%ecx
	jecxz	call_abort
	cmpl	$0x200,%ecx
	jnc	call_loop

	testb	$2,%dl
	movl	$1,%eax
	jne	call_user
	pushfl
	cli
	call	oz_hw486_getcpu
	xorl	%eax,%eax
	movb	CPU_B_SMPLEVEL(%esi),%al
	movb	$1,%ah
	popfl
call_user:
	cmpl	%eax,%ecx		# see if level matches
	jne	call_abort		# if not, we can't do it so abort

	movl	TRY_CATCHENT(%ebx),%esi	# matches, get pointer to condition handler routine
	testl	%esi,%esi		# - maybe it is disabled by a nested signal
	je	call_loop		#   if so, continue looking
	movl	$0,TRY_CATCHENT(%ebx)	# not disabled, disable it
	pushl	PROC_MCHARGS(%ebp)	# ... then call it
	pushl	PROC_SIGARGS(%ebp)
	pushl	TRY_CATCHPRM(%ebx)
	call	*%esi
	addl	$12,%esp
	movl	%esi,TRY_CATCHENT(%ebx)	# re-enable it

	cmpl	oz_RESIGNAL,%eax	# see if handler wants it re-signalled to an outer handler
	jne	call_processed
	movl	4(%ebp),%ecx		# if so, continue looking for next handler
	testl	%ecx,%ecx
	jne	call_loop

call_abort:
	xorl	%eax,%eax		# no handler to process the signalled condition, return 'abort' status
	popl	%ebx
	popl	%esi
	leave
	ret

call_processed:
	cmpl	oz_RESUME,%eax		# see if it wants to resume execution
	jne	call_unwind
	movl	$1,%eax			# if so, return 'resume' status
	popl	%ebx
	popl	%esi
	leave
	ret

call_unwind:
	leal	4(%ebx),%ebp		# unwind, wipe the call frame
	movl	-16(%ebp),%ebx		# restore C registers
	movl	-12(%ebp),%esi
	movl	 -8(%ebp),%edi
	leave
	ret

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

	.align	4
	.globl	oz_sys_condhand_signal
	.type	oz_sys_condhand_signal,@function
oz_sys_condhand_signal:
	pushfl				# save the eflags
	pushw	%cs			# save cs
	pushw	$0			# pad to longword boundary
	pushl	8(%esp)			# push eip = just past call to oz_sys_condhand_signal
	pushl	%ebp			# push caller's ebp
	pushal				# save the pushal stuff
	pushl	$0			# push ec2 = 0
	movl	$0,MCH_L_EC1(%esp)	# set ec1 = 0

	leal	MCH_L_EFLAGS+8(%esp),%eax # point to the sigargs
	movl	%eax,MCH_L_ESP(%esp)	# adjust saved stack pointer to be what it was just before the call to oz_sys_condhand_signal, ie, signal arguments

	pushl	%esp			# push pointer to machine arguments
	pushl	%eax			# push pointer to signal arguments
	call	oz_sys_condhand_call	# call appropriate condition handler
	testl	%eax,%eax
	jne	signal_rtn
	call	oz_sys_condhand_default	# no handler, use default
signal_rtn:
	addl	$8+MCH__SIZE,%esp	# wipe machine args from stack
	ret

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

	.align	4
	.globl	oz_sys_condhand_signalv
	.type	oz_sys_condhand_signalv,@function
oz_sys_condhand_signalv:
	pushfl				# save the eflags
	pushw	%cs			# save cs
	pushw	$0			# pad to longword boundary
	pushl	8(%esp)			# push eip = just past call to oz_sys_condhand_signal
	pushl	%ebp			# push caller's ebp
	pushal				# save the pushal stuff
	pushl	$0			# push ec2 = 0
	movl	$0,MCH_L_EC1(%esp)	# set ec1 = 0

	leal	MCH_L_EFLAGS+8(%esp),%eax # point to the sigargs pointer
	movl	%eax,MCH_L_ESP(%esp)	# adjust saved stack pointer to be what it was just before the call to oz_sys_condhand_signal, ie, signal arguments

	pushl	%esp			# push pointer to machine arguments
	pushl	(%eax)			# push pointer to signal arguments
	call	oz_sys_condhand_call	# call appropriate condition handler
	testl	%eax,%eax
	jne	signalv_rtn
	call	oz_sys_condhand_default	# no handler, use default
signalv_rtn:
	addl	$8+MCH__SIZE,%esp	# wipe machine args from stack
	ret

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

	TB_ENTRY     =  8
	TB_PARAM     = 12
	TB_MAXFRAMES = 16
	TB_MCHARGS   = 20
	TB_READMEM   = 24

	.text
	.align	4
	.globl	oz_hw_traceback
	.type	oz_hw_traceback,@function
oz_hw_traceback:
	pushl	%ebp				# make a new call frame
	movl	%esp,%ebp
	cmpl	$0,TB_READMEM(%ebp)		# see if readmem routine given
	jne	traceback_gavereadmem
	movl	$null_readmem,TB_READMEM(%ebp)	# if not, give a null one
traceback_gavereadmem:
	movl	TB_MCHARGS(%ebp),%edx		# point to given mchargs
	testl	%edx,%edx
	jne	traceback_copy
	movw	%cs,%dx				# none given, make mchargs on stack
	pushfl
	pushl	%edx
	pushl	$.
	pushal
	pushl	$0
	movl	$0,MCH_L_EC1(%esp)
	jmp	traceback_loop
traceback_copy:
	movl	$MCH__SIZE/4,%ecx		# given mchargs, copy to stack
	addl	$MCH__SIZE,%edx
traceback_push:
	subl	$4,%edx
	pushl	(%edx)
	loop	traceback_push
traceback_loop:
	subl	$1,TB_MAXFRAMES(%ebp)		# see if we have done max number of frames
	jc	traceback_done
	movl	TB_ENTRY(%ebp),%edx		# get routine entrypoint
	pushl	%esp				# push address of machine args
	pushl	TB_PARAM(%ebp)			# push routine parameter
	call	*%edx				# call routine
	addl	$8,%esp				# wipe call args from stack
	movl	MCH_L_EBP(%esp),%edx		# get current frame pointer
	testl	%edx,%edx			# done if zero
	je	traceback_done

	subl	$8,%esp				# read saved ebp and return address onto stack
	movl	TB_READMEM(%ebp),%eax
	movl	%esp,%ecx
	pushl	%edx
	pushl	$8
	pushl	%ecx
	pushl	TB_PARAM(%ebp)
	call	*%eax
	addl	$16,%esp
	testl	%eax,%eax			# done if values not readable
	je	traceback_done
	popl	%ecx				# get the saved ebp
	popl	%eax				# get the return address
	jecxz	traceback_ebpok
	cmpl	$0x200,%ecx			# see if it is a condition handler frame
	jnc	traceback_ebpok

	movl	MCH_L_EBP(%esp),%edx		# get current frame pointer
	subl	$12,%edx			# point to saved registers
	movl	TB_READMEM(%ebp),%eax		# read them onto stack
	subl	$24,%esp
	movl	%esp,%ecx
	pushl	%edx
	pushl	$24
	pushl	%ecx
	pushl	TB_PARAM(%ebp)
	call	*%eax
	addl	$16,%esp
	testl	%eax,%eax			# done if values not readable
	je	traceback_done

	leal	24(%esp),%eax			# point to mchargs on stack
	popl	MCH_L_EBX(%eax)			# pop saved registers
	popl	MCH_L_ESI(%eax)
	popl	MCH_L_EDI(%eax)
	addl	$4,%esp				# skip over flag long on stack
	popl	%ecx				# get the saved ebp
	popl	%eax				# get the return address
traceback_ebpok:
	movl	%ecx,MCH_L_EBP(%esp)		# set up new ebp
	movl	%eax,MCH_L_EIP(%esp)		# set up new return address
	jmp	traceback_loop			# repeat
traceback_done:
	leave					# pop call frame
	ret

# Read memory from local address space
#   4(%esp) = dummy
#   8(%esp) = where to write to
#  12(%esp) = how many bytes
#  16(%esp) = where to read from

null_readmem:
	pushl	%ebp
	movl	%esp,%ebp
	pushl	%edi
	pushl	%esi
	movl	16(%ebp),%ecx
	movl	20(%ebp),%esi
	movl	12(%ebp),%edi
	cld
	rep
	movsb
	popl	%esi
	popl	%edi
	popl	%ebp
	ret
