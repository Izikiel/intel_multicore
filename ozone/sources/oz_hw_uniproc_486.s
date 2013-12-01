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
##  Uniprocessor routines						##
##									##
##########################################################################

	.include "oz_params_486.s"

	MAST_8259_APORT = 0x20
	MAST_8259_DPORT = 0x21
	SLAV_8259_APORT = 0xA0
	SLAV_8259_DPORT = 0xA1

	TIMER_IRQ = 0

LOWEST_IRQ_SMPLEVEL  = OZ_SMPLOCK_LEVEL_IRQS	# inclusive
HIGHEST_IRQ_SMPLEVEL = LOWEST_IRQ_SMPLEVEL+15	# inclusive

.macro	UPDATEVIDEOSTATUS theeip
##	pushl	theeip
##	call	updatevideostatus
	.endm

		.data
		.align	4

softintpend:	.long	0		# set indicates that a software interrupt is pending
					# - to be processed when smplevel is 0 and hw ints are enabled
					#   <0> : set to indicate oz_knl_thread_handleint needs to be called
					#   <1> : set to indicate oz_knl_lowipl_handleint needs to be called
					#   <2> : set to indicate oz_knl_diag needs to be called
blocked_irqs:	.long	0		# bit for the irq gets set when cpu's current smplevel is at or above an irq level
					# and the interrupt happens.  bit gets cleared when the smplevel is lowered enough 
					# to process the interrupt.

	.globl	oz_hw486_cpudb
	.type	oz_hw486_cpudb,@object
oz_hw486_cpudb:	.space	CPU__SIZE	# we only have one cpu
quantum_timer:	.long	0		# OZ_Timer quantum timer struct

last_oz_hw_cpu_softint:	.long	0,0,0,0	# last place oz_hw_cpu_softint was called

# the following oz_hw486_irq_hits_... must only be used at softint level
			.globl	oz_hw486_irq_hits_lock
			.type	oz_hw486_irq_hits_lock,@object
oz_hw486_irq_hits_lock:	.long	0	# zero: no one is using oz_hw486_irq_hits_mask; else: someone is using it
			.globl	oz_hw486_irq_hits_mask
			.type	oz_hw486_irq_hits_mask,@object
oz_hw486_irq_hits_mask:	.long	0	# bitmask of irqs that have been detected

# Table of routine entrypoints and parameters, indexed by irq number
# The 'descriptors' table is a table of strings supplied by whoever registered for the irq, used for debugging

irq_entrypoints: .long	irq_many_handler,irq_many_handler,irq_many_handler,irq_many_handler
		 .long	irq_many_handler,irq_many_handler,irq_many_handler,irq_many_handler
		 .long	irq_many_handler,irq_many_handler,irq_many_handler,irq_many_handler
		 .long	irq_many_handler,irq_many_handler,irq_many_handler,irq_many_handler
irq_parameters:	 .long	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
irq_descriptors: .long	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0

irq_smplocks:	.space	SL__SIZE*16		# there is one smplock per irq, indexed by irq number

	.globl	oz_hw486_smplev_clash
	.type	oz_hw486_smplev_clash,@object
oz_hw486_smplev_clash:	.fill	256*8,1,0

	.text

	.align	4

	.globl	oz_hw486_apicmapped
	.type	oz_hw486_apicmapped,@object
oz_hw486_apicmapped:	.long	0	# these routines never map the apic
	.globl	oz_hw486_maxcpus
	.type	oz_hw486_maxcpus,@object
oz_hw486_maxcpus:	.long	1	# number of elements in oz_hw486_cpudb array

# Translate (smp lock level - LOWEST_IRQ_SMPLEVEL) to irq number

smplevel_to_irq: .byte	7,6,5,4,3,15,14,13,12,11,10,9,8,2,1,0

# Translate irq number to smplevel

irq_to_smplevel: .byte	LOWEST_IRQ_SMPLEVEL+15,LOWEST_IRQ_SMPLEVEL+14,LOWEST_IRQ_SMPLEVEL+13,LOWEST_IRQ_SMPLEVEL+4
		.byte	LOWEST_IRQ_SMPLEVEL+3, LOWEST_IRQ_SMPLEVEL+2, LOWEST_IRQ_SMPLEVEL+1, LOWEST_IRQ_SMPLEVEL+0
		.byte	LOWEST_IRQ_SMPLEVEL+12,LOWEST_IRQ_SMPLEVEL+11,LOWEST_IRQ_SMPLEVEL+10,LOWEST_IRQ_SMPLEVEL+9
		.byte	LOWEST_IRQ_SMPLEVEL+8, LOWEST_IRQ_SMPLEVEL+7, LOWEST_IRQ_SMPLEVEL+6, LOWEST_IRQ_SMPLEVEL+5

# Entrypoints for each irq handling routine to be copied to IDT on initialization

idt_irq_fix:	.long	 irq_0_entry, irq_1_entry, irq_2_entry, irq_3_entry
		.long	 irq_4_entry, irq_5_entry, irq_6_entry, irq_7_entry
		.long	 irq_8_entry, irq_9_entry,irq_10_entry,irq_11_entry
		.long	irq_12_entry,irq_13_entry,irq_14_entry,irq_15_entry

##########################################################################
##									##
##  Since we don't have an local APIC, map the page to no-access.	##
##									##
##########################################################################

	.align	4
	.globl	oz_hw486_apic_map
	.type	oz_hw486_apic_map,@function
oz_hw486_apic_map:
	movl	oz_SECTION_PAGESTATE_VALID_W,%eax # set it to valid
	shll	$9,%eax				# ... but with no access
	movl	%eax,SPTBASE+(APICBASE/1024)	# ... so oz_knl_boot will not try to map it to anything
	invlpg	APICBASE
	pushl	$apic_null			# print message indicating no apic
	call	oz_knl_printk
	addl	$4,%esp
	ret

apic_null:	.string	"oz_hw_uniproc_486: uniprocessor kernel\n"

##########################################################################
##									##
##  Start up the alternate cpus, cause them to call 			##
##  oz_knl_boot_othercpu						##
##									##
##    Input:								##
##									##
##	4(%esp) = oz_s_cpucount						##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_cpu_bootalts
	.type	oz_hw_cpu_bootalts,@function
oz_hw_cpu_bootalts:
	ret

##########################################################################
##									##
##  Set up the per-cpu apic register contents				##
##									##
##    Input:								##
##									##
##	%ecx = return address						##
##									##
##    Output:								##
##									##
##	%eax = cpu id number						##
##									##
##########################################################################

	.globl	oz_hw486_apic_setup
	.type	oz_hw486_apic_setup,@function
oz_hw486_apic_setup:
	xorl	%eax,%eax
	jmp	*%ecx

##########################################################################
##									##
##  Shut down APIC interrupts in preparation for halting, 		##
##  return my cpu index in %eax						##
##									##
##########################################################################

	.globl	oz_hw486_apic_shut
	.type	oz_hw486_apic_shut,@function
oz_hw486_apic_shut:
	movb	$-1,%al				# OCW1 master: inhibit all interrupts
	outb	$MAST_8259_DPORT
	IODELAY
	movb	$-1,%al				# OCW1 slave: inhibit all interrupts
	outb	$SLAV_8259_DPORT
	IODELAY
	xorl	%eax,%eax
	ret

##########################################################################
##									##
##  Invalidate the other cpu's page pointed to by %edx			##
##									##
##    Input:								##
##									##
##	%edx = virt address of page to invalidate			##
##									##
##    Scratch:								##
##									##
##	%eax,%ecx,%esi							##
##									##
##########################################################################

	.align	4
	.globl	oz_hw486_apic_invlpgedx
	.type	oz_hw486_apic_invlpgedx,@function
oz_hw486_apic_invlpgedx:
	ret

##########################################################################
##									##
##  Halt the other cpu's, cause them to call oz_knl_debug_halted no 	##
##  matter what they are currently doing				##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_debug_halt
	.type	oz_hw_debug_halt,@function
oz_hw_debug_halt:
	ret

##########################################################################
##									##
##  Quantum timer							##
##									##
##  These routines use the system timer queue to control cpu time 	##
##  usage by scheduled threads.						##
##									##
##  When the kernel loads a threads context up, it calls 		##
##  oz_hw_quantimer_start.  It expects to be called back at 		##
##  oz_knl_thread_quantimex when the timer runs out.			##
##									##
##    Input:								##
##									##
##	 4(%esp) = delta time from now to expire (<= 0.1 sec)		##
##	12(%esp) = current date/time					##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_quantimer_start
	.type	oz_hw_quantimer_start,@function
oz_hw_quantimer_start:
	pushl	%ebp
	movl	%esp,%ebp
	pushl	%edi
	pushl	%esi
	pushl	%ebx
	movl	16(%ebp),%eax			# get current iota time in %edx:%eax
	movl	20(%ebp),%edx
	addl	 8(%ebp),%eax			# add the given delta to it (<= 0.1 sec)
	adcl	12(%ebp),%edx
	call	oz_hw486_aiota2sys		# convert to system time
	movl	quantum_timer,%ecx
	pushl	$0				# parameter to oz_knl_thread_quantimex = cpuid 0
	pushl	$oz_knl_thread_quantimex	# entrypoint to call at softint level when time is up
	pushl	%edx				# when time is up
	pushl	%eax
	jecxz	quantimer_getone_uni
quantimer_haveone_uni:
	pushl	%ecx				# address of the timer struct
	call	oz_knl_timer_insert		# insert into queue (or remove and reinsert if already there)
	addl	$20,%esp
	popl	%ebx
	popl	%esi
	popl	%edi
	popl	%ebp
	ret

quantimer_getone_uni:
	pushl	$0				# allocate a timer struct
	call	oz_knl_timer_alloc
	addl	$4,%esp
	movl	%eax,%ecx
	movl	%eax,quantum_timer		# save it for repeated re-use
	jmp	quantimer_haveone_uni

##########################################################################
##									##
##  Enable/Inhibit softint delivery on this cpu				##
##									##
##    Input:								##
##									##
##	4(%esp) = 0 : ihnibit softint delivery				##
##	          1 : enable softint delivery				##
##									##
##    Output:								##
##									##
##	oz_hw_cpu_setsoftint = 0 : softint delivery was inhibited	##
##	                       1 : softint delivery was enabled		##
##	softint delivery mode set as given				##
##									##
##    Note:								##
##									##
##	No smplocks can be held by this cpu				##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_cpu_setsoftint
	.type	oz_hw_cpu_setsoftint,@function
oz_hw_cpu_setsoftint:
	pushl	%ebp
	movl	%esp,%ebp
	movzbl	oz_hw486_cpudb+CPU_B_SMPLEVEL,%eax # get cpu's current smp level
	cmpb	$2,%al				# make sure smp level not too high
	jnc	setsoftint_crash1
	movl	$1,%ecx				# get new smp level
	subl	8(%ebp),%ecx
	jc	setsoftint_crash2
	movb	%cl,oz_hw486_cpudb+CPU_B_SMPLEVEL # set the new level
	UPDATEVIDEOSTATUS 4(%ebp)
	jnz	setsoftint_rtn			# if softints now inhibited, don't check for pending softints
	cmpb	$0,softintpend			# enabled, see if there are any pending softints
	jz	setsoftint_rtn
	int	$INT_LOWIPL			# ... go process it
setsoftint_rtn:
	xorb	$1,%al				# flip smplevel<->softint enabled value
	popl	%ebp
	ret

setsoftint_crash1:
	pushl	%eax
	pushl	$setsoftint_msg1
	call	oz_crash

setsoftint_crash2:
	pushl	8(%ebp)
	pushl	$setsoftint_msg2
	call	oz_crash

setsoftint_msg1:	.string	"oz_hw_cpu_setsoftint: called at smplevel %u"
setsoftint_msg2:	.string	"oz_hw_cpu_setsoftint: invalid argument %u"

##########################################################################
##									##
##  This routine is called by the scheduler to simply wake another cpu	##
##  out of the HLT instruction in the OZ_HW_WAITLOOP_WAIT macro		##
##									##
##  For uniprocessor, it is a NOP as there is no other cpu to wake	##
##									##
##########################################################################

	.align	4
	.globl	oz_hw486_waitloop_wake
	.type	oz_hw486_waitloop_wake,@function
oz_hw486_waitloop_wake:
	ret

##########################################################################
##									##
##  Set this cpu's thread execution priority				##
##									##
##    Input:								##
##									##
##	4(%esp) = new thread execution priority for the current cpu	##
##	smplevel = at least softint					##
##									##
##    Note:								##
##									##
##	In this implementation, we don't use it for anything, so we 	##
##	don't bother saving it						##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_cpu_sethreadpri
	.type	oz_hw_cpu_sethreadpri,@function
oz_hw_cpu_sethreadpri:
	ret

##########################################################################
##									##
##  Sofint this or another cpu						##
##									##
##    Input:								##
##									##
##	4(%esp)  = cpu index number					##
##	smplevel = any							##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_cpu_reschedint
	.type	oz_hw_cpu_reschedint,@function
oz_hw_cpu_reschedint:
	pushl	%ebp
	movl	%esp,%ebp
	pushl	%ebx
	movb	$1,%bl
	jmp	softint

	.align	4
	.globl	oz_hw_cpu_lowiplint
	.type	oz_hw_cpu_lowiplint,@function
oz_hw_cpu_lowiplint:
	pushl	%ebp
	movl	%esp,%ebp
	pushl	%ebx
	movb	$2,%bl

softint:
	cmpl	$0,8(%ebp)				# we only do #0
	jne	softint_crash1

	pushfl
	cli
	movl	%ebp,%ecx				# get frame pointer
	xorl	%edx,%edx				# clear index
save_last_cpu_softint_loop:
	movl	4(%ecx),%eax				# get return address
	movl	 (%ecx),%ecx				# pop off a frame
	movl	%eax,last_oz_hw_cpu_softint(%edx)	# store return address
	addl	$4,%edx					# increment index
	jecxz	save_last_cpu_softint_done		# stop if no more frames to scan
	cmpl	$16,%edx				# see if we have four return addresses
	jne	save_last_cpu_softint_loop		# if not, keep processing
save_last_cpu_softint_done:
	cmpl	$16,%edx				# see if there is room left over
	je	save_last_cpu_softint_full
	movl	$0,last_oz_hw_cpu_softint(%edx)		# if so, put a zero to terminate
save_last_cpu_softint_full:
	popfl

	orb	%bl,softintpend				# say it is pending
	cmpb	$0,oz_hw486_cpudb+CPU_B_SMPLEVEL	# see if softint delivery is enabled
	jne	softint_rtn
	int	$INT_LOWIPL				# if so, process it immediately
softint_rtn:
	popl	%ebx
	leave
	ret

softint_crash1:
	pushl	4(%esp)
	pushl	$softint_msg1
	call	oz_crash

softint_msg1:	.string	"oz_hw_cpu_softint: cpu id %d out of range"

##########################################################################
##									##
##  Cause all cpus to enter diag mode					##
##									##
##  This routine is called at high ipl when control-shift-D is pressed.	##
##  It sets the softintpend<2> bit then softints the cpu.  The softint 	##
##  interrupt routine calls oz_knl_diag for every cpu as long as the 	##
##  diagmode flag is set.						##
##									##
##########################################################################

	.align	4
	.global	oz_hw_diag
	.type	oz_hw_diag,@function
oz_hw_diag:
	orb	$4,softintpend				# set the flag for the procsoftint routine
	cmpb	$0,oz_hw486_cpudb+CPU_B_SMPLEVEL	# see if softint delivery enabled
	jne	diag_ret
	int	$INT_LOWIPL				# if so, process it
diag_ret:
	ret

##########################################################################
##									##
##  Get current cpu index and point to cpudb struct			##
##									##
##    Input:								##
##									##
##	apic = cpu specific registers					##
##									##
##    Output:								##
##									##
##	eax = cpu index (starts at zero)				##
##	esi = points to cpu data					##
##									##
##    Preserved:							##
##									##
##	edi, edx (used by the thread_init and pte_writeact stuff)	##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_cpu_getcur
	.type	oz_hw_cpu_getcur,@function
oz_hw_cpu_getcur:				# C callable routine just sets up %eax ...
	xorl	%eax,%eax
	ret

	.align	4
	.globl	oz_hw486_getcpu
	.type	oz_hw486_getcpu,@function
oz_hw486_getcpu:				# assembler routine sets up %esi as well as %eax ...
	xorl	%eax,%eax
	leal	oz_hw486_cpudb,%esi
	ret

##########################################################################
##									##
##  Get current cpu's lock level					##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_cpu_smplevel
	.type	oz_hw_cpu_smplevel,@function
oz_hw_cpu_smplevel:
	movzbl	oz_hw486_cpudb+CPU_B_SMPLEVEL,%eax
	ret

##########################################################################
##									##
##  Set hardware interrupt enable bit on calling cpu			##
##									##
##    Input:								##
##									##
##	4(%esp) = 0 : inhibit hardware interrupt delivery		##
##	          1 : enable hardware interrupt delivery		##
##	         -1 : inhibit nmi's also				##
##									##
##    Output:								##
##									##
##	%eax = 0 : interrupt delivery was previously inhibited		##
##	       1 : interrupt delivery was previously enabled		##
##									##
##########################################################################

	.align	4
	.global	oz_hw_cpu_sethwints
	.type	oz_hw_cpu_sethwints,@function
oz_hw_cpu_sethwints:
	movl	4(%esp),%ecx	# get new setting
	pushfl			# get copy of current eflags
	testl	%ecx,%ecx
	jl	sethwints_both
	andl	$1,%ecx		# mask any junk from new setting
sethwints:
	movb	1(%esp),%al	# save current setting
	addl	%ecx,%ecx	# shift new bit over one bit
	andb	$0xFD,1(%esp)	# clear the enable bit on stack
	shrl	$1,%eax		# get previous setting bit in place
	orb	%cl,1(%esp)	# store new bit in stack's copy of eflags
	andl	$1,%eax		# mask out any junk in old value
	popfl			# store new eflags setting in eflags
	ret			# all done
sethwints_both:
	xorl	%ecx,%ecx	# we don't use nmi's for anything
	jmp	sethwints

##########################################################################
##									##
##  Initialize an smp lock						##
##									##
##    Input:								##
##									##
##	 4(%esp) = size of smplock struct				##
##	 8(%esp) = address of smplock struct				##
##	12(%esp) = priority to initialize it to				##
##									##
##    Output:								##
##									##
##	smp lock struct initialized, unowned				##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_smplock_init
	.type	oz_hw_smplock_init,@function
oz_hw_smplock_init:
	pushl	%ebp
	movl	%esp,%ebp
	cmpl	$SL__SIZE,8(%ebp)			# make sure they have enough room for the struct we use
	jc	smplock_init_crash1
	movl	12(%ebp),%eax				# ok, point to it
	movb	$-1,SL_B_CPUID(%eax)			# mark it unowned
	movl	16(%ebp),%edx				# get the priority they want it to be
	cmpl	$256,%edx				# make sure it will fit in smplevel field
	jnc	smplock_init_crash2
	movb	%dl,SL_B_LEVEL(%eax)			# ok, set up its priority
	leave						# all done
	ret

smplock_init_crash1:
	pushl	$SL__SIZE
	pushl	8(%ebp)
	pushl	$smplock_init_msg1
	call	oz_crash

smplock_init_crash2:
	pushl	%edx
	pushl	$smplock_init_msg2
	call	oz_crash

smplock_init_msg1:	.string	"oz_hw_smplock_init: smp lock size %u must be at least %u"
smplock_init_msg2:	.string	"oz_hw_smplock_init: smp lock level %u must be less than 256"

##########################################################################
##									##
##  Get an smp lock's level						##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_smplock_level
	.type	oz_hw_smplock_level,@function
oz_hw_smplock_level:
	movl	4(%esp),%eax			# point to smplock structure
	movzbl	SL_B_LEVEL(%eax),%eax		# get its level
	ret

##########################################################################
##									##
##  Get the cpu that owns an smplock					##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_smplock_cpu
	.type	oz_hw_smplock_cpu,@function
oz_hw_smplock_cpu:
	movl	4(%esp),%eax
	movzbl	SL_B_CPUID(%eax),%eax
	testb	%al,%al
	jns	smplock_cpu_rtn
	subl	$0x100,%eax
smplock_cpu_rtn:
	ret

##########################################################################
##									##
##  Wait for an smp lock to become available and lock it		##
##									##
##    Input:								##
##									##
##	4(%esp) = pointer to smplock struct to lock			##
##									##
##    Output:								##
##									##
##	%eax = prior smp lock level for this cpu			##
##	sw ints always inhibited					##
##	hw ints inhibited iff at or above highest irq level		##
##	cpu smp level = that of the new lock				##
##									##
##    Note:								##
##									##
##	you can only lock:						##
##	 1) the same lock as the last one that was locked by this cpu	##
##	 2) a lock defined at a higher level than the last one locked by this cpu
##	these rules prevent deadlock situations				##
##									##
##	Locking an irq's smplock will inhibit delivery of that irq's 	##
##	interrupt and any lower level irq's				##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_smplock_wait
	.type	oz_hw_smplock_wait,@function
oz_hw_smplock_wait:
	pushl	%ebp				# make a call frame
	movl	%esp,%ebp
	movzbl	oz_hw486_cpudb+CPU_B_SMPLEVEL,%eax # get existing smplevel here
	movl	8(%ebp),%ecx			# point to lock block
	movzbl	SL_B_LEVEL(%ecx),%edx		# get new smplevel here
# %ecx = smplock pointer
# %edx = new smp level
# %eax = old smp level
	cmpb	%al,%dl				# compare new level : current level
	jc	smplock_wait_crash1		# crash if new level < current level
	je	smplock_wait_same		# no-op if new level == current level

# Lock is at higher level than this cpu is at, acquire it

	movb	%dl,oz_hw486_cpudb+CPU_B_SMPLEVEL # increase cpu's smplock level
	UPDATEVIDEOSTATUS 4(%ebp)
	incb	SL_B_CPUID(%ecx)		# increment ownership from -1 to 0
	jne	smplock_wait_crash2		# crash if it was not -1 indicating it was unowned
	popl	%ebp
	ret

# Lock is at current level, just return with current level

smplock_wait_same:
	cmpb	$0,SL_B_CPUID(%ecx)		# level staying the same, i better already own this lock
	jne	smplock_wait_crash3		# (if not, might have two locks at the same level)
	popl	%ebp
	ret

# Crashes

smplock_wait_crash1:
	pushl	%eax
	pushl	%edx
	pushl	$smplock_wait_msg1
	call	oz_crash

smplock_wait_crash2:
	pushl	%edx
	pushl	%ecx
	pushl	$smplock_wait_msg2
	call	oz_crash

smplock_wait_crash3:
	pushl	%ecx
	pushl	%edx
	pushl	$smplock_wait_msg3
	call	oz_crash

smplock_wait_msg1:	.string	"oz_hw_smplock_wait: new level %x .lt. current level %x"
smplock_wait_msg2:	.string	"oz_hw_smplock_wait: already own lock %p at level %x"
smplock_wait_msg3:	.string	"oz_hw_smplock_wait: re-locking level %x but dont own lock %p"

##########################################################################
##									##
##  Clear an owned lock, return cpu to a previous level			##
##									##
##    Input:								##
##									##
##	4(%esp)  = pointer to lock being released			##
##	8(%esp)  = previous smp level to return to			##
##	smplevel = that exact same level as lock being released		##
##									##
##    Output:								##
##									##
##	cpu level returned to 8(%esp)					##
##	smp lock released if now below its level			##
##	hw ints enabled if now below highest irq level			##
##	sw ints enabled if now at level 0				##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_smplock_clr
	.type	oz_hw_smplock_clr,@function
oz_hw_smplock_clr:
	pushl	%ebp				# make a call frame
	movl	%esp,%ebp
	pushl	%edi				# save scratch registers
	movl	 8(%ebp),%edi			# point to lock block
	movzbl	12(%ebp),%edx			# get level we're restoring to
	movzbl	oz_hw486_cpudb+CPU_B_SMPLEVEL,%ecx # get existing smplevel here
	cmpb	SL_B_LEVEL(%edi),%cl		# we must be exactly at lock's level
	jne	smplockclr_crash1
	cmpb	$0,SL_B_CPUID(%edi)		# we must own the lock being released
	jne	smplockclr_crash2
	cmpb	%dl,%cl				# compare current level : level we're restoring to
	jc	smplockclr_crash3		# crash if restoring to higher level than current level
	je	smplockclr_ret			# nop if staying at same level (keep lock)

# Lock level is being lowered from level %cl to level %dl, so release the lock

	movb	$-1,SL_B_CPUID(%edi)		# release the lock so others can use it now
	movb	%dl,oz_hw486_cpudb+CPU_B_SMPLEVEL # set my cpu's new level
	UPDATEVIDEOSTATUS 4(%ebp)

# If some blocked interrupts happened whilst smplevel was elevated, process them now

	cmpl	$0,blocked_irqs
	jne	proc_blocked_irqs

smplockclr_ret:
	popl	%edi
	popl	%ebp
	ret

# Process blocked interrupts
# blocked_irqs = bitmask of irq's to process
# %ecx = high smplevel that blocked the interrupts
# %edx = current lowered smplevel
# We want to scan from %ecx (inclusive) (what used to be blocked) 
#              down to %edx (exclusive) (what is still blocked)

proc_blocked_irqs:
	cmpb	$HIGHEST_IRQ_SMPLEVEL,%cl			# limit high end to the highest priority irq
	jc	proc_blocked_irqs_clok
	movb	$HIGHEST_IRQ_SMPLEVEL,%cl			# - this is the highest irq smplevel
proc_blocked_irqs_clok:
	cmpb	$LOWEST_IRQ_SMPLEVEL-1,%dl			# limit other end to the lowest priority irq
	jnc	proc_blocked_irqs_dlok
	movb	$LOWEST_IRQ_SMPLEVEL-1,%dl			# - this is just below the lowest irq smplevel
proc_blocked_irqs_dlok:
	cmpb	%cl,%dl						# make sure %cl is above %dl
	jnc	smplockclr_ret					# if not, we're done scanning
proc_blocked_irqs_loop:
	movzbl	smplevel_to_irq-LOWEST_IRQ_SMPLEVEL(%ecx),%eax	# get irq number
	btr	%eax,blocked_irqs				# see if it needs to be processed
	jnc	proc_blocked_irqs_next
	int	$INT_DOIRQEAX					# if so, process it
proc_blocked_irqs_next:
	decb	%cl						# anyway, decrement to next lower smplevel
	cmpb	%cl,%dl						# see if %cl is still above %dl
	jc	proc_blocked_irqs_loop				# if so, process it
	jmp	smplockclr_ret					# all done

# Crashes

smplockclr_crash1:
	movzbl	%cl,%ecx
	movzbl	SL_B_LEVEL(%edi),%eax
	pushl	%ecx
	pushl	%eax
	pushl	$smplockclr_msg1
	call	oz_crash

smplockclr_crash2:
	movzbl	SL_B_CPUID(%edi),%eax
	pushl	%eax
	pushl	%ecx
	pushl	$smplockclr_msg2
	call	oz_crash

smplockclr_crash3:
	pushl	%ecx
	pushl	%edx
	pushl	$smplockclr_msg3
	call	oz_crash

smplockclr_msg1: .string "oz_hw_smplock_clr: releasing level %x while at %x"
smplockclr_msg2: .string "oz_hw_smplock_clr: releasing level %x owned by %d"
smplockclr_msg3: .string "oz_hw_smplock_clr: returning from level %x to %x"

##########################################################################
##									##
##  Initialize interrupt processing					##
##									##
##    Input:								##
##									##
##	%cs  = codesegment to use for interrupt processing		##
##	%edi = base of IDT						##
##	hardints inhibited						##
##									##
##    Output:								##
##									##
##	various IDT entries filled in					##
##	interrupt controllers initialized				##
##	some basic devices initialized					##
##									##
##    Scratch:								##
##									##
##	%eax,%ebx,%ecx,%edx,%esi,%edi					##
##									##
##########################################################################

	.align	4
	.globl	oz_hw486_irq_init
	.type	oz_hw486_irq_init,@function
oz_hw486_irq_init:

# Make sure we are using the patched assembler that will handle SL__SIZE as a scale arg for leal instruction

	pushl	%edi				# save idt pointer
	movl	$3,%ebx				# use a fancy index number
	leal	irq_smplocks(,%ebx,SL__SIZE),%edi # try the leal, broken gas uses scale factor 1
	movl	$SL__SIZE,%ecx			# multiply SL__SIZE by three
	addl	$SL__SIZE,%ecx
	addl	$SL__SIZE,%ecx
	addl	$irq_smplocks,%ecx		# add the base we always use
	cmpl	%ecx,%edi			# hopefully got the same answer
	je	leal_scale_ok
	pushl	%ecx
	pushl	%edi
	pushl	$leal_scale_bad
	call	oz_crash
leal_scale_ok:
	popl	%edi				# restore idt pointer

## Figure out codesegment to use = caller's codesegment

	movw	%cs,%cx

# IDT bytes 1:0 : low 16-bits of entrypoint
# IDT bytes 3:2 : new code segment selector
# IDT byte 4: zero
# IDT byte 5:
#     <0> = 0 : inhibit interrupt delivery on entry
#           1 : interrupt delivery unchanged
#   <4:1> = 0111
#   <6:5> = dpl (privilege level to access descriptor)
#     <7> = 1
# IDT bytes 6:7 : high 16-bits of entrypoint

# Fill in the 'int $INT_GETNOW' vector

	leal	8*INT_GETNOW(%edi),%eax		# point to the vector
	movl	$oz_hw_except_getnow,%edx	# get entrypoint
	movw	%dx,0(%eax)			# low order entrypoint
	movw	%cx,2(%eax)			# code segment
	shrl	$16,%edx			# get high order entrypoint in %dx
	movb	$0xEE,5(%eax)			# make it accessible from user mode 'int $INT_GETNOW' calls
						# inhibit hardware interrupts on entry
	movw	%dx,6(%eax)			# high order entrypoint

# Initialize 'int $INT_LOWIPL' interrupt vector

	leal	8*INT_LOWIPL(%edi),%eax		# point to the vector
	movl	$procsoftint,%edx		# get entrypoint
	movw	%dx,0(%eax)			# low order entrypoint
	movw	%cx,2(%eax)			# code segment
	shrl	$16,%edx			# get high order entrypoint in %dx
	movb	$0xAE,5(%eax)			# make it accessible from executive mode 'int $INT_LOWIPL' calls
						# - and inhibit hardware interrupts on entry
	movw	%dx,6(%eax)			# high order entrypoint

# Initialize 'int $INT_DOIRQEAX' interrupt vector

	leal	8*INT_DOIRQEAX(%edi),%eax	# point to the vector
	movl	$doirqeax,%edx			# get entrypoint
	movw	%dx,0(%eax)			# low order entrypoint
	movw	%cx,2(%eax)			# code segment
	shrl	$16,%edx			# get high order entrypoint in %dx
	movb	$0xAE,5(%eax)			# make it accessible from executive mode 'int $INT_LOWIPL' calls
						# - and inhibit hw interrupt delivery on entry
	movw	%dx,6(%eax)			# high order entrypoint

# Fill in IDT with IRQ interrupt vectors

	movl	$16,%ebx			# number of entries
	movl	$idt_irq_fix,%esi		# point to fixup table
	addl	$8*INT_IRQBASE,%edi		# point to irq vectors in table
idt_irq_fix_loop:
	movw	0(%esi),%ax			# copy in low-order entrypoint
	movw	2(%esi),%dx			# copy in high-order entrypoint
	movw	%ax,0(%edi)
	movw	%cx,2(%edi)			# code segment
	movb	$0x8E,5(%edi)			# inhibit hw interrupt delivery on entry
	movw	%dx,6(%edi)
	addl	$4,%esi				# increment to next fixup table entry
	addl	$8,%edi				# increment to next irq table entry
	decl	%ebx
	jne	idt_irq_fix_loop

# Reprogram the 8259's to not overlay the cpu vector table
# Use vectors 0x30-0x3F for IRQ0-IRQ15

	movb	$0x11,%al			# ICW1: initialization sequence
						#  <0> = 1 : ICW4 will be sent
						#  <1> = 0 : cascade mode
						#  <2> = 0 : not used for 8086 mode ?
						#  <3> = 0 : edge triggered mode
						#  <4> = 1 : must be 1
	outb	$MAST_8259_APORT		# send it to master
	IODELAY
	outb	$SLAV_8259_APORT		# send it to slave
	IODELAY
	movb	$INT_IRQBASE+0,%al		# ICW2 master: start of hardware int's (0x30)
	outb	$MAST_8259_DPORT
	IODELAY
	movb	$INT_IRQBASE+8,%al		# ICW2 slave: start of hardware int's (0x38)
	outb	$SLAV_8259_DPORT
	IODELAY
	movb	$0x04,%al			# ICW3 master: slave is connected to IR2
	outb	$MAST_8259_DPORT
	IODELAY
	movb	$0x02,%al			# ICW3 slave: slave is on master's IR2
	outb	$SLAV_8259_DPORT
	IODELAY
	movb	$0x01,%al			# ICW4: 8086 and non-AEOI modes for both
						#  <0> = 1 : 8086 mode
						#  <1> = 0 : no auto EOI mode
						#  <2> = x
						#  <3> = 0 : non-buffered mode
						#  <4> = not special fully nested mode
	outb	$MAST_8259_DPORT
	IODELAY
	outb	$SLAV_8259_DPORT
	IODELAY
	xorb	%al,%al				# OCW1 master: enable all interrupts
	outb	$MAST_8259_DPORT
	IODELAY
	outb	$SLAV_8259_DPORT		# OCW1 slave: enable all interrupts
	IODELAY

# Initialize the irq smp locks

	movl	$16,%ebx			# number of irqs
	movl	$irq_to_smplevel,%esi		# point to table of smp levels
	movl	$irq_smplocks,%edi		# point to table of locks
irq_init_smplock:
	movzbl	(%esi),%eax			# get level
	pushl	%eax				# initialize the lock
	pushl	%edi
	pushl	$SL__SIZE
	call	oz_hw_smplock_init
	addl	$12,%esp
	incl	%esi				# increment to next irq
	addl	$SL__SIZE,%edi
	decl	%ebx
	jne	irq_init_smplock

# Initialize timer

	call	oz_hw486_tod_init

	pushl	$timer_description
	pushl	$0				# dummy parameter
	pushl	$timer_interrupt		# interrupt routine entrypoint
	pushl	$TIMER_IRQ			# the irq level
	call	oz_hw_irq_only			# it is the only one at this level
	addl	$16,%esp
	movl	%eax,oz_hw_smplock_tm		# save pointer to smp lock
.if ENABLE_PROFILING
	movl	%eax,oz_hw_profile_lock		# same smp lock used for profile data
.endif

	ret

leal_scale_bad:	.string	"oz_hw_smproc_486: leal irq_smplocks(,%%ebx,SL__SIZE) gave %X, should be %X"
timer_description:	.string	"oz_hw_timer"

##########################################################################
##									##
##  Declare an interrupt service routine as the sole processor of the 	##
##  level								##
##									##
##    Input:								##
##									##
##	 4(%esp) = irq level						##
##	 8(%esp) = routine's entrypoint					##
##	12(%esp) = parameter to pass to routine				##
##	16(%esp) = device descriptor string pointer			##
##	smplevel = .le. the corresponding irq smplevel			##
##									##
##    Output:								##
##									##
##	oz_hw_irq_only = pointer to corresponding smplock		##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_irq_only
	.type	oz_hw_irq_only,@function
oz_hw_irq_only:
	pushl	%ebp
	movl	%esp,%ebp
	pushl	%edi
	pushl	%ebx
	movl	 8(%ebp),%ebx					# get irq level
	leal	irq_smplocks(,%ebx,SL__SIZE),%edi		# point to corresponding smp lock
	pushl	%edi						# lock the interrupt level so no one is in there
	call	oz_hw_smplock_wait
	movl	%eax,(%esp)					# save caller's smplock level (probably softint)
	movl	12(%ebp),%edx					# get entrypoint
	movl	16(%ebp),%ecx					# get parameter
	cmpl	$irq_many_handler,irq_entrypoints(,%ebx,4)	# make sure not already in use
	jne	irq_only_error1					# error if already in use
	cmpl	$0,irq_parameters(,%ebx,4)
	jne	irq_only_error1
	movl	%edx,irq_entrypoints(,%ebx,4)			# ok, save entrypoint
	movl	%ecx,irq_parameters(,%ebx,4)			# save parameter
	movl	20(%ebp),%edx					# save descr string
	movl	%edx,irq_descriptors(,%ebx,4)
	pushl	%edi						# release irq's smp lock
irq_only_return:
	call	oz_hw_smplock_clr
	addl	$8,%esp
	movl	%edi,%eax					# return pointer to smplock (or NULL if failed)
	popl	%ebx
	popl	%edi
	leave
	ret

irq_only_error1:
	pushl	irq_descriptors(,%eax,4)
	pushl	20(%ebp)
	pushl	irq_entrypoints(,%eax,4)
	pushl	%edx
	pushl	%eax
	pushl	$irq_only_msg1
	call	oz_knl_printk
	pushl	%edi						# release irq's smp lock
	xorl	%edi,%edi					# ... but return a null pointer
	jmp	irq_only_return

irq_only_msg1:	.string	"oz_hw_irq_only: redeclaring %u as %p when already %p, for %s by %s\n"

##########################################################################
##									##
##  This declares an handler as one of many for the IRQ level		##
##									##
##    Input:								##
##									##
##	 4(%esp) = irq level number (0-15)				##
##	 8(%esp) = pointer to OZ_Hw486_irq_many struct			##
##									##
##########################################################################

	.align	4
	.globl	oz_hw486_irq_many_add
	.type	oz_hw486_irq_many_add,@function
oz_hw486_irq_many_add:
	pushl	%ebp					# make a call frame for error tracing
	movl	%esp,%ebp
	pushl	%edi					# save scratch registers
	pushl	%ebx
	movl	8(%ebp),%ebx				# get IRQ level
	cmpl	$16,%ebx
	jnc	irq_many_add_crash1
	leal	irq_smplocks(,%ebx,SL__SIZE),%edi	# point to corresponding smp lock
	pushl	%edi					# lock the interrupt level so no one is in there
	call	oz_hw_smplock_wait
	movl	%eax,(%esp)				# save caller's smplock level (probably softint)
	cmpl	$irq_many_handler,irq_entrypoints(,%ebx,4) # make sure not in use by an irq_only routine
	jne	irq_many_add_inuse
	movl	12(%ebp),%ecx				# point to the new block
	movl	irq_parameters(,%ebx,4),%eax		# link it in to the list
	movl	%ecx,irq_parameters(,%ebx,4)
	movl	%eax,IRQ_MANY_NEXT(%ecx)
	pushl	%edi					# release the smp lock
irq_many_add_return:
	call	oz_hw_smplock_clr
	addl	$8,%esp
	movl	%edi,%eax				# return pointer to smp lock
	popl	%ebx					# restore scratch registers
	popl	%edi
	leave						# all done
	ret

irq_many_add_crash1:
	pushl	%ebx
	pushl	$irq_many_add_msg1
	call	oz_crash

irq_many_add_inuse:
	movl	12(%ebp),%ecx
	pushl	irq_descriptors(,%ebx,4)		# this is who is using it
	pushl	IRQ_MANY_DESCR(%ecx)			# this is who wants it
	pushl	%ebx					# this is the irq number
	pushl	$irq_many_add_msg2			# output message
	call	oz_knl_printk
	addl	$16,%esp
	pushl	%edi					# release irq's smp lock
	xorl	%edi,%edi				# ... but return a null pointer
	jmp	irq_many_add_return

irq_many_add_msg1: .string "oz_hw486_irq_many_add: irq %u out of range"
irq_many_add_msg2: .string "oz_hw486_irq_many_add: redeclaring %u as %s when already %s\n"

##########################################################################
##									##
##  Remove an handler from an IRQ level					##
##									##
##    Input:								##
##									##
##	 4(%esp) = irq level number (0-15)				##
##	 8(%esp) = pointer to OZ_Hw486_irq_many struct			##
##									##
##########################################################################

	.align	4
	.globl	oz_hw486_irq_many_rem
	.type	oz_hw486_irq_many_rem,@function
oz_hw486_irq_many_rem:
	pushl	%ebp					# make a call frame for error tracing
	movl	%esp,%ebp
	pushl	%edi					# save scratch registers
	pushl	%ebx
	movl	8(%ebp),%ebx				# get IRQ level
	cmpl	$16,%ebx
	jnc	irq_many_rem_crash1
	leal	irq_smplocks(,%ebx,SL__SIZE),%edi	# point to corresponding smp lock
	pushl	%edi					# lock the interrupt level so no one is in there
	call	oz_hw_smplock_wait
	movl	%eax,(%esp)				# save caller's smplock level (probably softint)
	leal	irq_parameters(,%ebx,4),%edx		# point to listhead
	movl	12(%ebp),%eax				# get pointer to block being removed
irq_many_rem_loop:
	movl	(%edx),%ecx				# get next element in list
	jecxz	irq_many_rem_crash2			# crash if reached the end of the list
	cmpl	%ecx,%eax				# see if it matches what we were given
	je	irq_many_rem_found
	leal	IRQ_MANY_NEXT(%ecx),%edx		# if not, check out the next in the list
	jmp	irq_many_rem_loop
irq_many_rem_found:
	xchgl	IRQ_MANY_NEXT(%eax),%eax		# found, unlink it from list
	movl	%eax,(%edx)
	pushl	%edi					# release the smp lock
	call	oz_hw_smplock_clr
	addl	$8,%esp
	popl	%ebx					# restore scratch registers
	popl	%edi
	leave						# all done
	ret

irq_many_rem_crash1:
	pushl	%ebx
	pushl	$irq_many_rem_msg1
	call	oz_crash

irq_many_rem_crash2:
	pushl	%ebx
	pushl	IRQ_MANY_DESCR(%eax)
	pushl	$irq_many_rem_msg2
	call	oz_crash

irq_many_rem_msg1: .string "oz_hw486_irq_many_rem: irq %d out of range"
irq_many_rem_msg2: .string "oz_hw486_irq_many_rem: couldn't find %s handler in irq %d"

#########################################################################
#									#
#  Process IRQ's							#
#  These routines are called as a result of hardware IRQ's		#
#  They are entered with harware interrupt delivery inhibited		#
#									#
#########################################################################

	.align	4
irq_0_entry:
	pushl	%ebp
	xorl	%ebp,%ebp
	jmp	irq_entry

	.align	4
irq_1_entry:
	pushl	%ebp
	movl	 $1,%ebp
	jmp	irq_entry

	.align	4
irq_2_entry:
	pushl	%ebp
	movl	 $2,%ebp
	jmp	irq_entry

	.align	4
irq_3_entry:
	pushl	%ebp
	movl	 $3,%ebp
	jmp	irq_entry

	.align	4
irq_4_entry:
	pushl	%ebp
	movl	 $4,%ebp
	jmp	irq_entry

	.align	4
irq_5_entry:
	pushl	%ebp
	movl	 $5,%ebp
	jmp	irq_entry

	.align	4
irq_6_entry:
	pushl	%ebp
	movl	 $6,%ebp
	jmp	irq_entry

	.align	4
irq_7_entry:
	pushl	%ebp
	movl	 $7,%ebp
	jmp	irq_entry

	.align	4
irq_8_entry:
	pushl	%ebp
	movl	 $8,%ebp
	jmp	irq_entry

	.align	4
irq_9_entry:
	pushl	%ebp
	movl	 $9,%ebp
	jmp	irq_entry

	.align	4
irq_10_entry:
	pushl	%ebp
	movl	$10,%ebp
	jmp	irq_entry

	.align	4
irq_11_entry:
	pushl	%ebp
	movl	$11,%ebp
	jmp	irq_entry

	.align	4
irq_12_entry:
	pushl	%ebp
	movl	$12,%ebp
	jmp	irq_entry

	.align	4
irq_13_entry:
	pushl	%ebp
	movl	$13,%ebp
	jmp	irq_entry

	.align	4
irq_14_entry:
	pushl	%ebp
	movl	$14,%ebp
	jmp	irq_entry

	.align	4
irq_15_entry:
	pushl	%ebp
	movl	$15,%ebp
	jmp	irq_entry

	# This gets called via 'int $INT_DOIRQEAX' instruction
	# %eax = irq number to be processed

	.align	4
doirqeax:
	pushl	%ebp
	movl	%eax,%ebp

	# %ebp     = irq number, 0..15
	# hardints = inhibited

	.align	4
irq_entry:

# Finish making the mchargs on the stack

	pushal						# push registers (irq number ends up in MCH_L_EC1)
	pushl	$0					# push a dummy EC2
	call	oz_hw_fixkmexceptebp			# fix up the saved ebp
	call	oz_hw_fixkmmchargesp			# fix up the saved esp
	movl	MCH_L_EC1(%esp),%ebx			# get the irq number
	bt	$9,MCH_L_EFLAGS(%esp)			# check for hw ints enabled
	jnc	irq_crash1				# (from DOIRQEAX probably)

	call	oz_hw486_random_int			# increment random seed counter

	bts	%ebx,oz_hw486_irq_hits_mask		# set bit in general 'got this irq' mask

# If cpu is at or above the smplevel for the irq, this interrupt is being blocked

	movb	irq_to_smplevel(%ebx),%cl		# get irq's smp level
	movb	oz_hw486_cpudb+CPU_B_SMPLEVEL,%ch	# get cpu's smp level
	cmpb	%cl,%ch					# see if cpu's smplevel >= irq's smplevel
	jnc	irq_is_blocked				# if so, the interrupt is blocked

# Raise the cpu's smplevel to that of the irq, set the smplock
# The 8259 masks should not need to be reprogrammed as they are supposed to block this and lower irq's until they get the EOI
# IRQ's at higher levels may occur, but they should be processed ok

	leal	irq_smplocks(,%ebx,SL__SIZE),%edi	# point to corresponding smp lock
	movb	%cl,oz_hw486_cpudb+CPU_B_SMPLEVEL	# set cpu's new smp level
	incb	SL_B_CPUID(%edi)			# mark it owned by this cpu
	jne	irq_crash2				# - it should have been -1 and now 0
	UPDATEVIDEOSTATUS $irq_entry

# Call the interrupt service routine

	sti						# allow nested interrupts now that smplevel is all updated
							# - we can't do sti before updating smplevel in case we get a
							#   nested interrupt (such as IRQ 0), it might think it's ok 
							#   to switch kernel stacks if the old smplevel is 0

	movl	irq_entrypoints(,%ebx,4),%eax		# get irq service routine entrypoint
	pushl	%esp					# - pass mchargs pointer
	pushl	irq_parameters(,%ebx,4)			# - pass parameter
	movb	%ch,%bh					# save old cpu smplevel in %bh
	call	*%eax					# call irq service routine
	addl	$8,%esp					# pop parameter and mchargs pointer
							# smplock pointer still in %edi
							# irq number still in %bl
							# old cpu smplevel still in %bh

	cli						# so we don't nest on this same exact level after eoi-ing

# Release the smplock for the irq level

# We shouldn't need to check blocked_irqs bits here as the 8259 chips are 
# supposed to be blocking all interrupts at or below the current level

	movb	$-1,SL_B_CPUID(%edi)			# mark smp lock as unowned
	movb	%bh,oz_hw486_cpudb+CPU_B_SMPLEVEL	# restore cpu's smp level
	UPDATEVIDEOSTATUS $irq_entry_rtn

# Ack the interrupt out of the 8259's.  This will allow them to process new requests at and below this level.

	movb	%bl,%al					# ack the specific irq we just processed
	andb	$0x07,%al
	orb	$0x60,%al
	testb	$0x08,%bl				# see if master or slave irq
	je	ack_8259_master
	outb	$SLAV_8259_APORT			# slave, acknowledge this irq in both slave and master
	IODELAY
	movb	$0x62,%al				# (it shows up as IRQ2 in master)
ack_8259_master:
	outb	$MAST_8259_APORT			# acknowledge this irq in master
	IODELAY
	jmp	irq_entry_rtn

# CPU's smplevel is at or above the level for the IRQ
# This means the interrupt is currently being blocked
# We leave the interrupt un-EOI'd so it can't interrupt any more and return back out
# When interrupted routine lowers the CPU's smplevel, it will re-process the interrupt

irq_is_blocked:
	bts	%ebx,blocked_irqs			# mark the irq as being blocked

# About to do an iret with mchargs on stack.  Check for pending softints or ast's before returning.
# This routine assumes the eflags on the stack indicate hardint delivery is to be enabled on return.
# Hardware interrupt delivery is inhibited.

irq_entry_rtn:
iretwithmchargs:
	cmpb	$0,oz_hw486_cpudb+CPU_B_SMPLEVEL	# see if softints enabled
	jne	iretmchargs_rtn				# if not, can't deliver anything
	cmpb	$0,softintpend				# see if softint pending
	jne	procsoftint_doit			# if so, go back to process it
	pushl	%esp					# push pointer to mchargs
	pushl	$0					# we're always cpu #0
	call	oz_knl_thread_checkknlastq		# process kernel mode ast's
	addl	$8,%esp					# pop cpu index and mchargs pointer
	testb	$2,MCH_W_CS(%esp)			# see if returning to user mode
	je	iretmchargs_rtn				# if not, just return
	pushl	oz_PROCMODE_USR				# we are about to return to user mode
	call	oz_knl_thread_chkpendast		# ... see if there will be any deliverable ast's for that mode
	addl	$4,%esp
	testl	%eax,%eax
	je	iretmchargs_rtn				# if not, we can just return to user mode as is
	sti						# deliverable usermode ast's, enable hw ints so we can access user stack
							# - if other ast's get queued to us, well we're about to do them anyway
							# - if the ast's get cancelled or delivered on us, oz_sys_thread_checkast will just have nothing to do
	xorl	%eax,%eax				# no additional longs to copy to user stack, just copy the mchargs
	call	oz_hw486_movemchargstocallerstack
	cmpl	oz_SUCCESS,%eax				# hopefully it copied ok
	jne	iretmchargs_baduserstack
	pushl	%esp					# we're in user mode now, push pointer to mchargs
	call	oz_sys_thread_checkast			# process any user mode ast's that may be queued
	addl	$4,%esp					# (the iret below will be from user mode to user mode)
iretmchargs_rtn:
	addl	$4,%esp					# wipe the dummy ec2 from stack
	popal						# restore general registers
	popl	%ebp					# restore ebp
	iret

# Unable to copy mchargs to user stack in preparation for delivering an ast
# Print an error message on console then exit the thread

iretmchargs_baduserstack:
	pushl	%eax
	pushl	MCH_L_XSP+4(%esp)
	pushl	%eax
	pushl	$iretmchargs_msg1
	call	oz_knl_printk
	addl	$12,%esp
iretmchargs_baduserexit:
	call	oz_knl_thread_exit
	jmp	iretmchargs_baduserexit

# Crashes

irq_crash1:
	pushl	%ebx
	pushl	$irq_msg1
	call	oz_crash

irq_crash2:
	pushl	%ebx
	pushl	$irq_msg2
	call	oz_crash

iretmchargs_msg1:	.string	"oz_hw_uniproc_486: error %u pushing to user stack pointer %p\n"
irq_msg1:		.string	"oz_hw_uniproc_486: irq %u with hw interrupt delivery inhibited"
irq_msg2:		.string	"oz_hw_uniproc_486: irq %u smplock was not unowned"

##########################################################################
##									##
##  This interrupt routine handles the 'oz_hw486_irq_many' list for 	##
##  the level.  The parameter is the first in the list.			##
##									##
##    Input:								##
##									##
##	%bl = irq level number						##
##	8(%esp) = pointer to mchargs					##
##									##
##########################################################################

	.align	4
irq_many_handler:
	pushl	%edi
	pushl	%esi
	pushl	%ebx
	movzbl	%bl,%ebx
irq_many_handler_repeat:
	movl	irq_parameters(,%ebx,4),%edi	# point to first struct in the list
	testl	%edi,%edi
	je	irq_many_handler_done		# all done if none
	xorl	%esi,%esi			# haven't seen any interrupt active flags yet
irq_many_handler_loop:
	pushl	20(%esp)			# push mchargs pointer
	movl	IRQ_MANY_ENTRY(%edi),%eax	# get routine's entrypoint
	pushl	IRQ_MANY_PARAM(%edi)		# push routine's parameter
	movl	IRQ_MANY_NEXT(%edi),%edi	# save pointer to next in list
						# (in case handler removes itself)
	call	*%eax				# call the routine
	addl	$8,%esp				# wipe params from stack
	orl	%eax,%esi			# save the 'interrupt was active' flag
	testl	%edi,%edi
	jne	irq_many_handler_loop		# loop back if next one found
	testl	%esi,%esi			# see if any interrupts were active
	jne	irq_many_handler_repeat		# if so, repeat until there are none
irq_many_handler_done:
	popl	%ebx
	popl	%esi
	popl	%edi
	ret

##########################################################################
##									##
##  Process an software interrupt					##
##									##
##  Since we are assuming there is no local APIC, the only way this 	##
##  gets called is via 'int $INT_LOWIPL'.				##
##									##
##########################################################################

	.align	4
procsoftint:
	bt	$9,8(%esp)			# see if hwints were enabled
	jnc	procsoftint_iret		# - if not, we can't do softints
	pushl	%ebp				# make a call frame
	pushal					# save registers
	pushl	$0				# dummy ec2
	movl	$0,MCH_L_EC1(%esp)		# dummy ec1
	call	oz_hw_fixkmexceptebp		# point to stack frame

	cmpb	$0,oz_hw486_cpudb+CPU_B_SMPLEVEL # make sure softints were enabled
	jne	procsoftint_crash1
procsoftint_doit:
	movb	$1,oz_hw486_cpudb+CPU_B_SMPLEVEL # say softints aren't enabled anymore
	UPDATEVIDEOSTATUS $procsoftint
	movb	softintpend,%bl			# see what kind of softints are pending
	movb	$0,softintpend			# sofints no longer pending as we are about to process them
	sti					# allow hardware interrupts, softints are inhibited
	testb	$4,%bl				# see if in diag mode
	jz	procsoftint_nodiagmode
	pushl	%esp				# point to machine args on stack
	pushl	$1				# push first cpu flag on stack
	pushl	$0				# push cpu number
	call	oz_knl_diag			# call the diagnostic routine (cpu_index, first_cpu_flag, mchargs)
	addl	$12,%esp			# wipe call args from stack
procsoftint_nodiagmode:
	testb	$2,%bl				# see if lowipl delivery pending
	jz	procsoftint_nolowipl
	call	oz_knl_lowipl_handleint		# process stuff with softints inhibited
procsoftint_nolowipl:
	testb	$1,%bl				# see if reschedule pending
	jz	procsoftint_noresched
	call	oz_knl_thread_handleint		# process stuff with softints enabled
procsoftint_noresched:
	cli					# inhibit hw interrupts so an interrupt routine can't change stuff on us
	movb	$0,oz_hw486_cpudb+CPU_B_SMPLEVEL # enable software interrupt delivery
	UPDATEVIDEOSTATUS $procsoftint_iret
	jmp	iretwithmchargs			# if no nested reschedule, do an iret with mchargs on stack, but check for ast's, etc

procsoftint_iret:
	iret					# hw ints were inhibited, so we can't deliver softint now

# Softints were inhibited

procsoftint_crash1:
	movzbl	oz_hw486_cpudb+CPU_B_SMPLEVEL,%eax
	pushl	%eax
	pushl	$procsoftint_msg1
	call	oz_crash

procsoftint_msg1:	.string	"oz_hw_uniproc_486 procsoftint: called at smplevel %x"

##########################################################################
##									##
##  Set the datebin of the next event					##
##  When this time is reached, call oz_knl_timer_timeisup		##
##									##
##    Input:								##
##									##
##	8:4(%esp) = datebin of next event				##
##									##
##	smplock = tm							##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_timer_setevent
	.type	oz_hw_timer_setevent,@function
oz_hw_timer_setevent:
	movl	4(%esp),%eax
	movl	8(%esp),%edx
	pushl	%ebx
	pushfl
	cli
	call	oz_hw486_asys2iota
	movl	%eax,timer_event+0
	movl	%edx,timer_event+4
	popfl
	popl	%ebx
	ret

##########################################################################
##									##
##  This interrupt routine is called approx every 54.9247mS		##
##									##
##  This routine assumes the clock is the only thing connected to IRQ0	##
##									##
##########################################################################

	.data

	.globl	oz_hw_smplock_tm
	.type	oz_hw_smplock_tm,@object
oz_hw_smplock_tm:	.long	0		# points to timer smplock
timer_event:		.long	-1,-1		# rdtsc to call oz_knl_timer_timeisup

	.text

	.align	4
timer_interrupt:
	pushl	%edi
	pushl	%esi
	pushl	%ebx

.if ENABLE_PROFILING
	movl	20(%esp),%edx			# point to mchargs on stack
	call	oz_hw_profile_tick		# update kernel profile data
.endif

## Increment the quicky counter

	incl	oz_s_quickies

## Get current time from rdtsc timer into %edx:%eax
## Since there is only one CPU, a simple rdtsc will do

	rdtsc

## See if it is time for the next event

	subl	timer_event+0,%eax		# subtract next event's time
	sbbl	timer_event+4,%edx
	jc	timer_interrupt_rtn		# just return if not there yet
	movl	$-1,timer_event+4		# ok, set flag saying don't call it twice
	call	oz_knl_timer_timeisup		# call the timeisup routine
timer_interrupt_rtn:
	popl	%ebx
	popl	%esi
	popl	%edi
	ret					# return to irq_entry routine to clean up

##########################################################################
##									##
##  Update status line on the screen for the current cpu		##
##  Called via macro UPDATEVIDEOSTATUS so it can be nopped		##
##									##
##    Input:								##
##									##
##	4(%esp) = eip associated with change				##
##									##
##    Output:								##
##									##
##	all registers preserved						##
##	eflags preserved						##
##	one arg popped from stack					##
##									##
##########################################################################

	.align	4
updatevideostatus:
	pushl	%ebp
	movl	%esp,%ebp
	pushl	%esi
	pushl	%edx
	pushl	%ecx
	pushl	%eax
	pushfl
	cli

	movb	oz_hw486_cpudb+CPU_B_SMPLEVEL,%al
	cmpb	$0,enab_smplevel
	je	updatevideostatus_skip
	cmpb	$0xEF,last_smplevel
	je	updatevideostatus_skip
	testb	%al,%al
	jne	updatevideostatus_skip
	pushl	8(%ebp)
	pushl	last_smplevel
	pushl	$zero_smplevel_msg
	call	oz_knl_printk
	addl	$12,%esp
	xorb	%al,%al
updatevideostatus_skip:
	movb	%al,last_smplevel

.if 000
	movzbl	oz_hw486_cpudb+CPU_B_SMPLEVEL,%ecx
	pushl	8(%ebp)
	pushl	$0
	pushl	%ecx
	pushl	$0
	call	oz_dev_video_statusupdate
	addl	$16,%esp
.endif

	popfl
	popl	%eax
	popl	%ecx
	popl	%edx
	popl	%esi
	leave
	ret	$4

zero_smplevel_msg:	.string	"oz_hw_uniproc*: smplevel %X -> 0 at %X\n"

	.data
	.align	4
####	.globl	enab_smplevel
enab_smplevel:	.long	0
last_smplevel:	.long	0
