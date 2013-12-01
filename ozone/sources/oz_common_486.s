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
##  Routines common to the loader and the kernel			##
##									##
##########################################################################

	.include "oz_params_486.s"

	.data
	.align	4
	.globl	oz_hw486_idt_lastflag
	.type	oz_hw486_idt_lastflag,@object
oz_hw486_idt_lastflag:	.long	1

	.text

##########################################################################
##									##
##  Delay a short bit after an in/out instruction			##
##  Use macro IODELAY in oz_params_486.s to call it so we can nop it 	##
##  out									##
##									##
##########################################################################

	.align	4
	.globl	oz_hw486_iodelay
	.type	oz_hw486_iodelay,@function
oz_hw486_iodelay:
	jmp	iodelay1
iodelay1:
	jmp	iodelay2
iodelay2:
	ret


##########################################################################
##									##
##  Fix the current ebp given machine args on the stack			##
##									##
##    Input:								##
##									##
##	4(%esp) = machine arguments					##
##									##
##    Output:								##
##									##
##	ebp = fixed							##
##									##
##    Scratch:								##
##									##
##	nothing								##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_fixkmexceptebp
	.type	oz_hw_fixkmexceptebp,@function
oz_hw_fixkmexceptebp:
	xorl	%ebp,%ebp			# assume previous outer mode, break call frame chain
	testb	$2,4+MCH_W_CS(%esp)		# see if previous exec mode
	jne	fixkmexceptebp_wasntknl
	leal	4+MCH_L_EBP(%esp),%ebp		# if so, keep the call frame chain intact
fixkmexceptebp_wasntknl:
	ret

##########################################################################
##									##
##  Fix the esp saved in the kernel machine arguments			##
##									##
##    Input:								##
##									##
##	4(%esp) = machine arguments					##
##									##
##    Output:								##
##									##
##	4+MCH_L_ESP(%esp) = fixed					##
##									##
##    Scratch:								##
##									##
##	%edx								##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_fixkmmchargesp
	.type	oz_hw_fixkmmchargesp,@function
oz_hw_fixkmmchargesp:
	leal	4+MCH_L_XSP(%esp),%edx		# this is where exec stack pointer was before exception
	testb	$2,4+MCH_W_CS(%esp)		# see if previous exec mode
	jne	fixkmmchargesp_outer
	movl	%edx,4+MCH_L_ESP(%esp)		# save stack pointer at time of exception
	ret
fixkmmchargesp_outer:
	movl	(%edx),%edx			# if not, get outer mode stack pointer
	movl	%edx,4+MCH_L_ESP(%esp)
	ret

##########################################################################
##									##
##  If given procmode is .lt. current procmode, return current 		##
##  procmode, else return given procmode.				##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_procmode_max
	.type	oz_hw_procmode_max,@function
oz_hw_procmode_max:
	movl	4(%esp),%eax		# get the given procmode
	movw	%cs,%cx			# get %cs to see what mode we're in now
	andl	$1,%eax			# should give us only 0 (knl) or 1 (usr)
	testb	$2,%cl			# see if currently user mode
	je	procmode_max_knl	# if not, return given arg as is
	movl	$1,%eax			# if so, force user mode for arg
procmode_max_knl:
	ret

##########################################################################
##									##
##  Set up debug to watch a location					##
##									##
##    Input:								##
##									##
##	4(%esp) = address of location to watch				##
##									##
##########################################################################

	.align	4
	.global	oz_hw_debug_watch
	.type	oz_hw_debug_watch,@function
oz_hw_debug_watch:
	pushl	%edi			# save C registers that we use
	pushl	%ebx
	movl	12(%esp),%eax		# address to debug
	movl	%eax,%dr0
	movl	%dr7,%ebx		# get current dr7 contents
	andl	$0x0000dc00,%ebx	# clear all except reserved bits
	orl	$0x000d0202,%ebx	# enable write-only breakpoint on dr0 longword
	movl	%ebx,%dr7
	movl	%dr6,%ecx		# clear exception bits
	andl	$0xffff1ff0,%ecx
	movl	%ecx,%dr6

	movl	%cr0,%eax		# turn off WP bit so we can write IDT
	pushl	%eax
	andl	$0xFFFEFFFF,%eax
	movl	%eax,%cr0

	subl	$8,%esp			# get idtr on stack
	sidt	(%esp)
	movl	2(%esp),%edi		# point to exception table
	addl	$8,%esp
	leal	debug_except,%edx	# point to debug exception routine
	movw	%dx,8(%edi)		# store low order routine address
	shrl	$16,%edx
	movw	%cs,10(%edi)		# store code segment number
	movw	$0x8e00,12(%edi)	# set interrupt gate descriptor code
	movw	%dx,14(%edi)		# store high order routine address

	popl	%eax			# turn WP back on
	movl	%eax,%cr0

	popl	%ebx
	popl	%edi
	ret

##########################################################################
##									##
##  Debug exception handler						##
##									##
##  Prints out message when debug exception occurs			##
##									##
##########################################################################

	.text

debug_msg1:	.ascii	"oz_common_486 debug_except: flags %x, addr %x, new %8.8x\n    eip %x"
		.byte	0
debug_msg2:	.ascii	",%x"
		.byte	0
debug_msg3:	.byte	10,0

	.align	4
debug_except:
	pushal				# save general registers
	movl	%dr0,%eax		# get debug registers
	movl	%dr6,%ebx
	movl	32(%esp),%edx		# get saved eip
	pushl	%edx			# print message
	pushl	(%eax)
	pushl	%eax
	pushl	%ebx
	pushl	$debug_msg1
	call	oz_knl_printk
	addl	$20,%esp		# pop call args
	movl	%ebp,%edx		# point to last frame pointer
	movl	$8,%ecx			# do a max of 8 frames
debug_loop:
	testl	%edx,%edx		# stop if no more frames
	je	debug_done
	bt	$31,%edx		# don't do any user mode frames
	jc	debug_done
	pushl	%ecx			# save number of frams to go
	pushl	(%edx)			# save pointer to next frame
	pushl	4(%edx)			# print out the corresponding return address
	pushl	$debug_msg2
	call	oz_knl_printk
	addl	$8,%esp
	popl	%edx			# point to next frame
	popl	%ecx			# see if we should do any more
	loop	debug_loop
debug_done:
	pushl	$debug_msg3
	call	oz_knl_printk
	addl	$4,%esp

			####	pushl	$irq_smplocks	# dump out irq smplocks table
			####	pushl	$32
			####	call	oz_knl_dumpmem
			####	leal	8(%esp),%eax	# dump out pushal registers
			####	pushl	%eax
			####	pushl	$32
			####	call	oz_knl_dumpmem
			####	addl	$16,%esp

	popal				# restore general registers
	iret				# return

##########################################################################
##									##
##  Routines we need to provide to the C stuff in loader and kernel	##
##									##
##########################################################################

##########################################################################
##									##
##  Get return address of a particular frame				##
##									##
##    Input:								##
##									##
##	4(%esp) = frame index						##
##	          0 returns the rtn address of call to oz_hw_getrtnadr	##
##									##
##    Output:								##
##									##
##	%eax = return address of that frame, 0 if no more frames	##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_getrtnadr
	.type	oz_hw_getrtnadr,@function
oz_hw_getrtnadr:
	pushl	%ebp			# make a stack frame
	movl	%esp,%ebp
	movl	8(%ebp),%ecx		# get frame count
	movl	%ebp,%eax		# start with my return address as frame zero
	jecxz	getrtnadr_done		# all done if frame count is zero
getrtnadr_loop:
	movl	(%eax),%eax		# non-zero param, link up a frame
	testl	%eax,%eax		# stop if ran out of frames
	loopnz	getrtnadr_loop		# more frames, see if count still says go up more
	je	getrtnadr_exit
getrtnadr_done:
	movl	4(%eax),%eax		# get return address in that frame
getrtnadr_exit:
	leave
	ret

##########################################################################
##									##
##  Increment a long, even in an SMP					##
##									##
##    Input:								##
##									##
##	4(%esp) = pointer to longword					##
##	8(%esp) = value							##
##									##
##    Output:								##
##									##
##	@4(%esp) = modified						##
##	%eax = new value						##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_atomic_inc_long
	.type	oz_hw_atomic_inc_long,@function
oz_hw_atomic_inc_long:
	movl	4(%esp),%edx			# point to destination memory location
	movl	8(%esp),%eax			# get value to be added to memory location
	lock					# lock others out
	xaddl	%eax,(%edx)			# add %eax to (%edi), get old (%edi) value in %eax
	addl	8(%esp),%eax			# get new value in %eax
	ret

##########################################################################
##									##
##  OR a long, even in an SMP						##
##									##
##    Input:								##
##									##
##	4(%esp) = pointer to longword					##
##	8(%esp) = value							##
##									##
##    Output:								##
##									##
##	@4(%esp) = modified						##
##	%eax = old value						##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_atomic_or_long
	.type	oz_hw_atomic_or_long,@function
oz_hw_atomic_or_long:
	movl	4(%esp),%edx			# point to destination memory location
	movl	(%edx),%eax			# get old value
atomic_or_loop:
	movl	8(%esp),%ecx			# get mask to or in
	orl	%eax,%ecx			# or the old and new values
	lock					# lock others out
	cmpxchgl %ecx,(%edx)			# if (%edx) == %eax, then store %ecx in (%edx)
	jne	atomic_or_loop			# repeat if failure
	ret

##########################################################################
##									##
##  AND a long, even in an SMP						##
##									##
##    Input:								##
##									##
##	4(%esp) = pointer to longword					##
##	8(%esp) = value							##
##									##
##    Output:								##
##									##
##	@4(%esp) = modified						##
##	%eax = old value						##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_atomic_and_long
	.type	oz_hw_atomic_and_long,@function
oz_hw_atomic_and_long:
	movl	4(%esp),%edx			# point to destination memory location
	movl	(%edx),%eax			# get old value
atomic_and_loop:
	movl	8(%esp),%ecx			# get mask to and in
	andl	%eax,%ecx			# and the old and new values
	lock					# lock others out
	cmpxchgl %ecx,(%edx)			# if (%edx) == %eax, then store %ecx in (%edx)
	jne	atomic_and_loop			# repeat if failure
	ret

##########################################################################
##									##
##  Set a long, even in an SMP						##
##									##
##    Input:								##
##									##
##	4(%esp) = pointer to longword					##
##	8(%esp) = value							##
##									##
##    Output:								##
##									##
##	@4(%esp) = modified						##
##	%eax = old value						##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_atomic_set_long
	.type	oz_hw_atomic_set_long,@function
oz_hw_atomic_set_long:
	movl	4(%esp),%edx			# point to destination memory location
	movl	8(%esp),%eax			# get value to be stored in memory location
	lock					# lock others out
	xchgl	%eax,(%edx)			# store %eax in (%edi), get old (%edi) value in %eax
	ret

##########################################################################
##									##
##  Set a long conditionally, even in an SMP				##
##									##
##    Input:								##
##									##
##	 4(%esp) = pointer to longword					##
##	 8(%esp) = new value						##
##	12(%esp) = old value						##
##									##
##    Output:								##
##									##
##	@4(%esp) = modified iff it was 12(%esp)				##
##	%eax = 0 : it was different than 12(%esp) so it was not set	##
##	       1 : it was the same as 12(%esp) so it was set to 8(%esp)	##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_atomic_setif_long
	.globl	oz_hw_atomic_setif_ptr
	.type	oz_hw_atomic_setif_long,@function
	.type	oz_hw_atomic_setif_ptr,@function
oz_hw_atomic_setif_long:
oz_hw_atomic_setif_ptr:
	movl	 4(%esp),%edx			# point to destination memory location
	movl	 8(%esp),%ecx			# get value to be stored in memory location
	movl	12(%esp),%eax			# get expected old contents of memory location
	lock					# lock others out
	cmpxchgl %ecx,(%edx)			# if (%edx) == %eax, then store %ecx in (%edx)
	setz	%al				# set %al 0 if it failed, 1 if it succeeded
	movzbl	%al,%eax			# clear uppper bits of %eax
	ret

##########################################################################
##									##
##  This crashes the system by inhibiting hardware interrupts, doing a 	##
##  breakpoint trap							##
##									##
##	oz_hw_crash ()							##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_crash
	.type	oz_hw_crash,@function
oz_hw_crash:
	cli
	int3
	jmp	oz_hw_crash

##########################################################################
##									##
##  Get a page that can be mapped to a device's registers		##
##									##
##    Output:								##
##									##
##	%eax = physical page number that isn't mapped to anything	##
##									##
##########################################################################

	.data
	.align	4
last_used_page:	.long	0

	.text
	.align	4
	.globl	oz_hw_get_iopage
	.type	oz_hw_get_iopage,@function
oz_hw_get_iopage:
	pushl	%ebp
	movl	%esp,%ebp
get_unused_iopage_loop:
	movl	last_used_page,%eax		# get old value
	leal	1(%eax),%ecx			# get new value in ecx
	testl	%eax,%eax
	jne	get_unused_iopage_ok
	movl	oz_s_phymem_totalpages,%ecx
get_unused_iopage_ok:
	lock					# lock others out
	cmpxchgl %ecx,last_used_page		# if last_used_page == %eax, then store %ecx in last_used_page
	jne	get_unused_iopage_loop		# repeat if failure
	movl	%ecx,%eax			# return allocate page number
	leave
	ret

##  Check to see if hardware interrupt delivery is inhibited, crash if not

	.align	4
	.globl	oz_hw_cpu_chkhwinhib
	.type	oz_hw_cpu_chkhwinhib,@function
oz_hw_cpu_chkhwinhib:
	pushl	%ebp
	movl	%esp,%ebp
	pushfl
	cli
	bt	$9,(%esp)
	jc	chkhwinhib_crash1
	leave
	ret
chkhwinhib_crash1:
	movl	8(%ebp),%edx
	call	beep
chkhwinhib_hang:
	jmp	chkhwinhib_hang

chkhwinhib_msg1:	.string	"oz_hw_cpu_chkhwinhib: eflags %8.8X"

##
## Output a long beep followed by the given number of short beeps
##
##   Input:
##
##	dx = number of short beeps
##
##   Scratch:
##
##	eax,ebx,ecx,edx
##
beep:
	movl	$100000000,%ecx		# output a long beep and a short pause
	call	beeptimed
beep_count_loop:
	movl	$30000000,%ecx		# output a short beep and a short pause
	call	beeptimed
	decw	%dx			# decrement beep counter
	jne	beep_count_loop		# loop back if more beeps to go
	call	beepshortdelay		# an extra short pause when done
	ret
##
beeptimed:
	inb	$0x61		# turn the beep on
	IODELAY
	orb	$3,%al
	outb	$0x61
	IODELAY
	movb	$0xb6,%al
	outb	$0x43
	IODELAY
	movb	$0x36,%al
	outb	$0x42
	IODELAY
	movb	$0x03,%al
	outb	$0x42
	IODELAY
##
	call	beepdelay	# leave it on for time in ecx
##
	inb	$0x61		# turn the beep off
	IODELAY
	andb	$0xfc,%al
	outb	$0x61
	IODELAY
##
beepshortdelay:
	movl	$20000000,%ecx	# follow the turn-off by a short pause
beepdelay:
	decl	%ecx
	jne	beepdelay
	ret

##########################################################################
##									##
##  Call a subroutine given a pointer to its arg list			##
##									##
##    Input:								##
##									##
##	 4(%esp) = entrypoint						##
##	 8(%esp) = number of arguments (longwords in this case)		##
##	12(%esp) = pointer to arg list					##
##									##
##    Output:								##
##									##
##	as defined by subroutine being called				##
##									##
##########################################################################

	.align	4
	.globl	oz_sys_call
	.type	oz_sys_call,@function
oz_sys_call:
	pushl	%ebp		# make a call frame
	movl	%esp,%ebp
	pushl	%edi		# save scratch registers
	pushl	%esi
	movl	12(%ebp),%ecx	# get number of longwords in arg list
	cld			# make the copy go forward
	movl	%ecx,%eax	# get number of longwords again
	movl	16(%ebp),%esi	# point to given arg list
	shll	$2,%eax		# calculate number of bytes in arg list
	movl	8(%ebp),%edx	# get subroutine's entrypoint
	subl	%eax,%esp	# make room for arg list on stack
	movl	%esp,%edi	# point to stack's copy of arg list
	rep			# copy arg list to stack
	movsl
	call	*%edx		# call the routine
	leal	-8(%ebp),%esp	# pop arg list from stack
	popl	%esi		# restore scratch registers
	popl	%edi
	leave			# pop call frame and return
	ret

##########################################################################
##									##
##  Take a breakpoint here						##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_debug_bpt
	.type	oz_hw_debug_bpt,@function
oz_hw_debug_bpt:
	pushl	%ebp
	movl	%esp,%ebp
	int3
	leave
	ret

##########################################################################
##									##
##  Determine whether or not we are in kernel mode			##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_inknlmode
	.type	oz_hw_inknlmode,@function
oz_hw_inknlmode:
	movw	%cs,%dx		# get the code segment we're in
	xorl	%eax,%eax	# assume not in kernel mode
	testb	$2,%dl		# check low 2 bits of code seg
	jne	inknlmode_rtn	# if non-zero, we're not in kernel mode
	incl	%eax		# they're zero, we are in kernel mode
inknlmode_rtn:
	ret

##########################################################################
##									##
##  Make sure other processors see any writes done before this call 	##
##  before they see writes done after this call				##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_mb
	.type	oz_hw_mb,@function
oz_hw_mb:
	xorl	%eax,%eax
	pushl	%ebx
	cpuid
	popl	%ebx
	ret

##########################################################################
##									##
##  Kernel profiling data						##
##									##
##########################################################################

	.globl	oz_hw_profile_enable
	.globl	oz_hw_profile_array
	.globl	oz_hw_profile_base
	.globl	oz_hw_profile_count
	.globl	oz_hw_profile_lock
	.globl	oz_hw_profile_scale
	.globl	oz_hw_profile_size
	.globl	oz_hw_profile_tick
	.type	oz_hw_profile_enable,@object
	.type	oz_hw_profile_array,@object
	.type	oz_hw_profile_base,@object
	.type	oz_hw_profile_count,@object
	.type	oz_hw_profile_lock,@object
	.type	oz_hw_profile_scale,@object
	.type	oz_hw_profile_size,@object
	.type	oz_hw_profile_tick,@function

	.data
	.align	4
oz_hw_profile_enable:	.long	1	# (0 here disables profiling, else it is enabled)
					# code can turn on and off to selectively profile stuff
oz_hw_profile_array:	.long	0	# base address of longword array
					# (0 here disables profiling)
oz_hw_profile_base:	.long	0	# base code address to profile
oz_hw_profile_count:	.long	0	# total number of timer counts
oz_hw_profile_lock:	.long	0	# pointer to profile data smplock
oz_hw_profile_scale:	.long	0	# scale code offset to array longword index
oz_hw_profile_size:	.long	0	# size of code block to profile

##  Call this routine at regular intervals to take a snapshot of the call stack
##
##    Input:
##
##	%edx = pointer to mchargs at point of interrupt
##
##    Scratch:
##
##	%eax,%ebx,%ecx,%edx,%edi

	.text
	.align	4

oz_hw_profile_tick:
	cmpl	$0,oz_hw_profile_enable		# make sure it's enabled
	je	profile_done
	movl	oz_hw_profile_array,%edi	# point to profile array
	testl	%edi,%edi
	je	profile_done
	movl	MCH_L_EBP(%edx),%ecx		# get the ebp
	movl	MCH_L_EIP(%edx),%eax		# get the eip
	incl	oz_hw_profile_count		# increment total count
	leal	MCH_L_EBP(%edx),%ebx
profile_scan:
	subl	oz_hw_profile_base,%eax		# see if within range
	jc	profile_next
	cmpl	oz_hw_profile_size,%eax
	jnc	profile_next
	xorl	%edx,%edx			# found, get the array index
	divl	oz_hw_profile_scale
	incl	(%edi,%eax,4)			# increment the counter
profile_next:
	cmpl	%ecx,%ebx			# see if frame pointer good
	jnc	profile_done			# (it must be .gt. last one)
	cmpl	$PROCBASVADDR,%ecx		# (and it must be in system space)
	jnc	profile_done
	movl	%ecx,%ebx			# ok, save last one
	movl	4(%ecx),%eax			# get the next outer eip
	movl	(%ecx),%ecx			# ... and the next outer ebp
	jmp	profile_scan			# check out that last eip
profile_done:
	ret

	TRACE_Q_WHEN  =  0
	TRACE_L_RTN   =  8
	TRACE_L_P1    = 12
	TRACE_L_P2    = 16
	TRACE__SIZE   = 32
	TRACE__L2SIZE =  5

	TRACE__L2MAX  = 7
	TRACE__MAX    = (1<<TRACE__L2MAX)

	.data
	.align	4
	.globl	oz_hw486_trace_buf
	.globl	oz_hw486_trace_idx
	.type	oz_hw486_trace_buf,@object
	.type	oz_hw486_trace_idx,@object
oz_hw486_trace_buf:	.space	TRACE__SIZE*TRACE__MAX
oz_hw486_trace_idx:	.long	0

	.text

# void oz_hw486_trace_ini (void)

	.align	4
	.globl	oz_hw486_trace_ini
	.type	oz_hw486_trace_ini,@function
oz_hw486_trace_ini:
	movl	$0,oz_hw486_trace_idx
	ret

# void oz_hw486_trace_add (uLong p1, uLong p2)

	.align	4
	.globl	oz_hw486_trace_add
	.type	oz_hw486_trace_add,@function
oz_hw486_trace_add:
	pushfl
	cli
	call	oz_hw_tod_getnow
	movzbl	oz_hw486_trace_idx,%ecx
	andb	$TRACE__MAX-1,%cl
	shll	$TRACE__L2SIZE,%ecx
	addl	$oz_hw486_trace_buf,%ecx
	movl	%eax,TRACE_Q_WHEN+0(%ecx)
	movl	%edx,TRACE_Q_WHEN+4(%ecx)
	movl	4(%esp),%eax
	movl	%eax,TRACE_L_RTN(%ecx)
	movl	8(%esp),%eax
	movl	%eax,TRACE_L_P1(%ecx)
	movl	12(%esp),%eax
	movl	%eax,TRACE_L_P2(%ecx)
	incl	oz_hw486_trace_idx
	popfl
	ret

# void oz_hw486_trace_dmp (void)

	.align	4
	.globl	oz_hw486_trace_dmp
	.type	oz_hw486_trace_dmp,@function
oz_hw486_trace_dmp:
	pushl	%ebp			# make a call frame on stack
	movl	%esp,%ebp
	pushl	%esi			# save scratch registers
	pushl	%ebx
	pushfl				# don't allow isr's to mod oz_hw486_trace_buf
	cli
	pushl	oz_hw486_trace_idx	# print 1st message
	pushl	$tracemsg1
	call	oz_knl_printk
	addl	$8,%esp
	movl	oz_hw486_trace_idx,%ebx	# get ending index
	subl	$TRACE__MAX,%ebx	# change it to starting index
	jnc	trace_dmp_loop
	xorl	%ebx,%ebx		# if it hasn't wrapped yet, start at zero
trace_dmp_loop:
	movl	%ebx,%esi		# get index
	andl	$TRACE__MAX-1,%esi	# wrap it
	shll	$TRACE__L2SIZE,%esi	# get offset in array
	addl	$oz_hw486_trace_buf,%esi # point to the entry
	pushl	TRACE_L_P2(%esi)	# print out the entry
	pushl	TRACE_L_P1(%esi)
	pushl	TRACE_Q_WHEN+4(%esi)
	pushl	TRACE_Q_WHEN+0(%esi)
	pushl	$tracemsg2
	call	oz_knl_printk
	pushl	TRACE_L_RTN(%esi)	# print the return address symbolically
	call	oz_knl_printkaddr
	pushl	$tracemsg3		# newline
	call	oz_knl_printk
	addl	$28,%esp		# wipe pushed args from stack
	incl	%ebx			# increment index for next entry
	cmpl	oz_hw486_trace_idx,%ebx	# repeat if more to print
	jne	trace_dmp_loop
##	pushl	$tracemsg4		# done, wait for a <return> key
##	call	oz_hw_pause
##	addl	$4,%esp
	popfl				# allow interrupt delivery
	popl	%ebx			# restore scratch regs
	popl	%esi
	leave				# remove call frame from stack
	ret				# return

tracemsg1:	.string	"oz_hw486_trace_dmp: count 0x%X\n"
tracemsg2:	.string	"%t %8.8X %8.8X "
tracemsg3:	.string	"\n"
tracemsg4:	.string	"oz_hw486_trace_dmp: > "

##########################################################################
##									##
##  Check to make sure hardware interrupt delivery is enabled		##
##									##
##    Input:								##
##									##
##	4(%esp) = string name of routine being called from		##
##	8(%esp) = instance number within that routine			##
##									##
##########################################################################

	.align	4
	.globl	oz_hw486_chkhwien
	.type	oz_hw486_chkhwien,@function
oz_hw486_chkhwien:
	pushl	%ebp			# make call frame for error message
	movl	%esp,%ebp
	pushfl				# get current interrupt enable flag
	bt	$9,(%esp)		# test it
	jnc	chkhwien_bad
	leave				# it is set, so that's good
	ret

chkhwien_bad:
	pushl	12(%ebp)		# it's clear, print out error message
	pushl	8(%ebp)
	pushl	$chkhwien_msg1
	call	oz_knl_printk
	movl	%ebp,%ebx		# print out a trace of return addresses (system seems to hang on crash)
chkhwien_bad_loop:
	pushl	$chkhwien_msg2
	call	oz_knl_printk
	pushl	4(%ebx)
	call	oz_knl_printkaddr
	addl	$8,%esp
	movl	(%ebx),%ebx
	testl	%ebx,%ebx
	jne	chkhwien_bad_loop
	pushl	$chkhwien_msg3
	call	oz_crash

chkhwien_msg1:	.string	"oz_hw486_chkhwien: hw ints inhibited (%s %d), eflags %x"
chkhwien_msg2:	.string	"\n    "
chkhwien_msg3:	.string	"\n  crashing..."

