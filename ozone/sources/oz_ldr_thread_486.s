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

	.include "oz_params_486.s"

	KERNEL_STACK_SIZE = 8192

	.text

##########################################################################
##									##
##  Initialize thread hardware context block				##
##									##
##    Input:								##
##									##
##	 4(%esp) = pointer to hardware context block			##
##	 8(%esp) = number of user stack pages				##
##	           0 if kernel-mode only thread				##
##	12(%esp) = thread routine entrypoint				##
##	           0 if initializing cpu as a thread for the first time	##
##	16(%esp) = thread routine parameter				##
##	20(%esp) = process hardware context block pointer		##
##	24(%esp) = thread software context block pointer		##
##									##
##	smplock = ts							##
##									##
##    Output:								##
##									##
##	thread hardware context block = initialized so that call to 	##
##	oz_hw_thread_switchctx will set up to call the thread routine	##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_thread_initctx
oz_hw_thread_initctx:
	pushl	%ebp
	movl	%esp,%ebp
	pushl	%edi
	pushl	%esi

##  Initialize hardware context area to zeroes

	xorl	%eax,%eax
	movl	$THCTX__SIZE/4,%ecx
	movl	8(%ebp),%edi				# point to hardware context block
	cld
	rep
	stosl
	movl	8(%ebp),%edi

##  If we're initializing this cpu as a thread, just skip the rest

	cmpl	$0,16(%ebp)				# see if initializing cpu for the first time
	je	thread_initctx_cpu

##  Allocate a kernel stack

	pushl	$0					# allocate non-paged pool for kernel stack
	pushl	$KERNEL_STACK_SIZE
	call	oz_knl_nppmallo
	addl	$8,%esp					# pop call args from stack
	movl	%eax,THCTX_L_ESTACKVA(%edi)		# save base address of stack
	addl	$KERNEL_STACK_SIZE,%eax			# get top address of stack

##  Set up what we want to happen when the 'RET' of the oz_hw_thread_switchctx routine executes

	movl	28(%ebp),%edx				# get pointer to thread software context block
	leal	-28(%eax),%esi				# make room for 7 longs on new stack
							# leave ebx, esi, edi as garbage
	movl	$0,12(%esi)				# saved ebp = zeroes
	movl	$oz_knl_thread_start,16(%esi)		# saved eip = oz_knl_thread_start routine
	movl	$0,20(%esi)				# oz_knl_thread_start's rtn addr = 0
	movl	%edx,24(%esi)				# param to pass to oz_knl_thread_start = thread sw ctx ptr
	movl	%esi,THCTX_L_EXESP(%edi)		# save initial stack pointer

		####	pushl	%edx
		####	pushl	%esi
		####	pushl	%edi
		####	pushl	$thread_initctx_deb1
		####	call	oz_knl_printk
		####	addl	$16,%esp

thread_initctx_rtn:
	popl	%esi
	popl	%edi
	leave
	ret

thread_initctx_cpu:
	movl	%edi,oz_hw486_cpudb+CPU_L_THCTX		# save initial hardware context pointer
	jmp	thread_initctx_rtn

		####	thread_initctx_deb1:	.string	"oz_hw_thread_initctx: hwctx %p, stack %p, swctx %p\n"

##########################################################################
##									##
##  Terminate thread hardware context					##
##									##
##    Input:								##
##									##
##	4(%esp) = pointer to thread hardware context block		##
##	8(%esp) = pointer to process hardware context block		##
##									##
##    Output:								##
##									##
##	@4(%esp) = voided out						##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_thread_termctx
oz_hw_thread_termctx:
	pushl	%ebp
	movl	%esp,%ebp

##  Free memory used for its kernel stack
##  If ESTACKVA is zero, it means this is the cpu's initial thread

	movl	8(%ebp),%edx
	movl	THCTX_L_ESTACKVA(%edx),%eax
	orl	%eax,%eax
	je	thread_termctx_rtn
	movl	$0,THCTX_L_ESTACKVA(%edx)
	pushl	%eax
	call	oz_knl_nppfree
thread_termctx_rtn:
	leave
	ret

##########################################################################
##									##
##  Switch thread hardware context					##
##									##
##	 4(%esp) = old thread hardware context block address		##
##	 8(%esp) = new thread hardware context block address		##
##	smplevel = ts							##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_thread_switchctx
oz_hw_thread_switchctx:
	pushl	%ebp				# save old thread's frame pointer on old thread's stack
	movl	%esp,%ebp
	pushl	%edi				# save C registers
	pushl	%esi
	pushl	%ebx

	movl	 8(%ebp),%esi			# get old thread hardware context block
	movl	12(%ebp),%edi			# get new thread's hardware context block

	cmpl	oz_hw486_cpudb+CPU_L_THCTX,%esi	# make sure old context matches
	jne	thread_switchctx_crash1

	movl	%esp,THCTX_L_EXESP(%esi)	# save current kernel stack pointer
	movl	THCTX_L_EXESP(%edi),%esp	# we're now on the new thread's kernel stack

	movl	%edi,oz_hw486_cpudb+CPU_L_THCTX	# save new context block pointer

	popl	%ebx				# restore C registers
	popl	%esi
	popl	%edi
	popl	%ebp				# restore new thread's frame pointer
	ret					# return to new thread

thread_switchctx_crash1:
	pushl	oz_hw486_cpudb+CPU_L_THCTX
	pushl	%esi
	pushl	$thread_switchctx_msg1
	call	oz_crash

thread_switchctx_msg1:	.string	"oz_ldr_thread_486 switchctx: cpudb ctx %p, old ctx %p"
