##+++2003-03-01
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
##---2003-03-01

##########################################################################
##
##  Set usermode IOPL
##
##	oldiopl = oz_hw486_setuseriopl (newiopl)
##
##    Input:
##
##	newiopl = -1 : just return it, don't modify
##	        else : new iopl (0 for kernel, 1 for user)
##
##    Output:
##
##	oldiopl = OZ_FLAGWASSET : iopl used to indicate user mode
##	          OZ_FLAGWASCLR : iopl used to indicate kernel mode
##	                   else : error status (no priv to access)
##
##    Note:
##
##	This routine is only available when the kernel is built to 
##	operate in true kernel mode.  It can't work in exec mode because 
##	on thread switch, the iret back to user mode of another thread 
##	would leave the IOPL set to the previous thread's state.

	.include "oz_params_486.s"

	.text
	.align	4
	.globl	oz_hw486_setuseriopl
	.type	oz_hw486_setuseriopl,@function
oz_hw486_setuseriopl:
	pushl	%ebp
	movl	%esp,%ebp
	pushl	8(%ebp)				# get new value
	pushl	$setuseriopl_callknl		# do it in kernel mode
	call	oz_sys_callknl			# ... via oz_sys_callknl to check caller's privs
	movl	%ebp,%esp			# all done
	popl	%ebp
	ret

# Runs in kernel mode with hardware and software interrupt delivery enabled
#  4(%esp) = caller's procmode
#  8(%esp) = contains new iopl value (-1=nochange, 0=kernel, 1=user)

	.align	4
setuseriopl_callknl:
	pushl	%ebp				# make call frame on stack
	movl	%esp,%ebp
	pushl	%esi				# save scratch register
	movl	oz_BADSYSCALL,%eax		# in case we're not in real kernel mode
	movw	%cs,%cx
	testb	$3,%cl
	jne	viaintinstr_rtn

	cmpb	$0,oz_hw486_sysentsysex		# see if we are here via sysenter/sysexit
	jne	sysentsysex

# Got here via 'int' instruction, so we should have the following on the kernel stack:
#   -4(%esi) = saved user ss (USRDATASEG)
#   -8(%esi) = saved user esp
#  -12(%esi) = saved user eflags
#  -16(%esi) = saved user cs (USRCODESEG or CONCODESEG|3)
#  -20(%esi) = saved user eip

	cli					# so cpu doesn't switch on us
	call	oz_hw486_getcpu			# get cpu number and database pointer
	movl	CPU_L_THCTX(%esi),%eax		# get thread hardware context pointer
	movl	THCTX_L_ESTACKVA(%eax),%esi	# get top of this thread's kernel stack
	sti					# now it's ok for cpu to switch all it wants, our stack doesn't move
	movl	%esi,%eax			# make sure current esp is in that stack
	subl	%esp,%eax
	cmpl	oz_hw486_estacksize,%eax
	jnc	setuseriopl_crash1
	cmpl	$USRDATASEG,-4(%esi)		# make sure there is a user ss/esp pair pushed on top of stack
	jne	setuseriopl_crash2
	cmpl	$USRCODESEG,-16(%esi)		# make sure there is a user cs/eip pair below that
	je	viaintinstr_csok
	cmpl	$CONCODESEG|3,-16(%esi)
	jne	setuseriopl_crash3
viaintinstr_csok:

# OK, user mode eflags is -12(%esi)

	movl	oz_FLAGWASCLR,%eax		# assume flag used to be clear (kernel)
	bt	$13,-12(%esi)			# check it out
	jnc	viaintinstr_gotold
	movl	oz_FLAGWASSET,%eax		# it used to be set (user)
viaintinstr_gotold:
	movl	12(%ebp),%ecx			# get new iopl mode
	testl	%ecx,%ecx			# see if caller wants to modify them
	js	viaintinstr_rtn			# if not, just return current setting
	cmpl	$2,%ecx				# otherwise, only allow 0 or 1
	jnc	viaintinstr_bad
	leal	(%ecx,%ecx,2),%ecx		# map 0->0 and 1->3
	shll	$12,%ecx			# put in %ecx<13:12>
	andl	$0xFFFFCFFF,-12(%esi)		# clear the old bits
	orl	%ecx,-12(%esi)			# set new bits
viaintinstr_rtn:
	popl	%esi				# restore scratch register
	popl	%ebp				# wipe call frame from stack
	ret					# return to caller
viaintinstr_bad:
	movl	oz_BADPARAM,%eax
	jmp	viaintinstr_rtn

# Got here via sysenter/sysexit
# Current eflags IOPL field is same as user as it is not affected by sysenter/sysexit

sysentsysex:
	pushfl					# get current elfags on stack
	movl	oz_FLAGWASCLR,%eax		# assume flag used to be clear (kernel)
	bt	$13,(%esp)			# check it out
	jnc	sysentsysex_gotold
	movl	oz_FLAGWASSET,%eax		# it used to be set (user)
sysentsysex_gotold:
	movl	12(%ebp),%ecx			# get new iopl mode
	testl	%ecx,%ecx			# see if caller wants to modify them
	js	sysentsysex_rtn			# if not, just return current setting
	cmpl	$2,%ecx				# otherwise, only allow 0 or 1
	jnc	sysentsysex_bad
	leal	(%ecx,%ecx,2),%ecx		# map 0->0 and 1->3
	shll	$12,%ecx			# put in %ecx<13:12>
	andl	$0xFFFFCFFF,(%esp)		# clear the old bits
	orl	%ecx,(%esp)			# set new bits
sysentsysex_rtn:
	popfl					# get new eflags
	popl	%esi				# restore scratch register
	popl	%ebp				# wipe call frame from stack
	ret					# return to caller
sysentsysex_bad:
	movl	oz_BADPARAM,%eax		# parameter invalid
	jmp	sysentsysex_rtn

# Crashes

setuseriopl_crash1:
	pushl	%esp
	pushl	oz_hw486_estacksize
	pushl	%esi
	pushl	$setuseriopl_msg1
	call	oz_crash

setuseriopl_crash2:
	pushl	-4(%esi)
	pushl	%esi
	pushl	$setuseriopl_msg2
	call	oz_crash

setuseriopl_crash3:
	pushl	-16(%esi)
	pushl	%esi
	pushl	$setuseriopl_msg3
	call	oz_crash

setuseriopl_msg1: .asciz "oz_hw486_setuseriopl: stack top %X, size %X, current %X"
setuseriopl_msg2: .asciz "oz_hw486_setuseriopl: stack top %X, user ss %X"
setuseriopl_msg3: .asciz "oz_hw486_setuseriopl: stack top %X, user cs %X"
