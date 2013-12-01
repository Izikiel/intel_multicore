##+++2003-12-12
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
##---2003-12-12

##########################################################################
##									##
##  SMP routines							##
##									##
##  These routines require each cpu to have a built-in local APIC and 	##
##  that the pci bus have an PIIX4/IOAPIC combination.			##
##									##
##########################################################################

	.include "oz_params_486.s"

	MAST_8259_APORT = 0x20			# 8259 controllers' I/O ports
	MAST_8259_DPORT = 0x21
	SLAV_8259_APORT = 0xA0
	SLAV_8259_DPORT = 0xA1

	IOAPIC_PHYPAGE = 0xFEC00		# IOAPIC's physical page

LOWEST_IRQ_SMPLEVEL  = OZ_SMPLOCK_LEVEL_IRQS
SMPLEVEL_TM = OZ_SMPLOCK_LEVEL_IRQS+16

.macro	UPDATEVIDEOSTATUS theeip
##	pushl	theeip
##	call	updatevideostatus
	.endm

		.data
		.align	4

			.globl	oz_hw486_apicmapped
			.type	oz_hw486_apicmapped,@object
oz_hw486_apicmapped:	.long	0		# set to 1 when the apic is mapped to virtual memory
			.globl	oz_hw486_irq_hits_lock
			.type	oz_hw486_irq_hits_lock,@object
oz_hw486_irq_hits_lock:	.long	0		# zero: no one is using oz_hw486_irq_hits_mask; else: someone is using it
			.globl	oz_hw486_irq_hits_mask
			.type	oz_hw486_irq_hits_mask,@object
oz_hw486_irq_hits_mask:	.long	0		# bitmask of irqs that have been detected
			.globl	oz_hw486_irq_counts
			.type	oz_hw486_irq_counts,@object
oz_hw486_irq_counts:	.space	MAXCPUS*4	#### count of irq's on a cpu
			.globl	oz_hw486_irq_masks
			.type	oz_hw486_irq_masks,@object
oz_hw486_irq_masks:	.space	MAXCPUS*4	#### irq's that have happened on a cpu
			.globl	oz_hw486_smplev_clash
			.type	oz_hw486_smplev_clash,@object
oz_hw486_smplev_clash:	.fill	256*8,1,0	#### how many times clashed on an smp level

enabled_nmis:	.long	0			# mask of cpu's with enabled NMI's
pending_nmis:	.long	0			# mask of cpu's with pending NMI's
debughaltmode:	.long	0			# set if the NMI is for entering debug halt mode
diagmodeflag:	.long	0			# set on entry to diagmode to indicate the first cpu to recognize it
apic_second:	.long	0			# number of local apic timer counts per second (bus speed)
rdtsc_per_apic_16: .long 0			# rdtsc's per apic timer count, shifted left by 16
timer_event_when: .long	-1,-1			# when to call oz_knl_timer_timeisup (RDTSC value)
timer_event_cpuid: .long -1			# what cpu is currently timing it

		.globl	oz_hw486_cpudb		# (no one really uses it, just for debugger reference)
		.type	oz_hw486_cpudb,@object
oz_hw486_cpudb:	.space	MAXCPUS*CPU__SIZE	# enough room for all cpudb structs
cpudb_end:

int_count_time:	.long	0,0
int_count_irqs:	.long	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0

# Table of interrupt call blocks, indexed by irq level

irq_list_heads:	.long	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
irq_list_lock:	.long	0			# .eq. 0 : irq_list_heads is unlocked
						# .lt. 0 : irq_list_heads is being modified
						# .gt. 0 : irq_list_heads is being scanned

# Timer smplock

smplock_tm:	.space	SL__SIZE

# Profiling smplock

.if ENABLE_PROFILING
profile_lock:	.space	SL__SIZE
.endif

	.text

	.align	4

# Number of elements in oz_hw486_cpudb array

		.globl	oz_hw486_maxcpus
		.type	oz_hw486_maxcpus,@object
oz_hw486_maxcpus: .long	MAXCPUS

# Pointer to timer smplock

		.globl	oz_hw_smplock_tm
		.type	oz_hw_smplock_tm,@object
oz_hw_smplock_tm: .long	smplock_tm

# Translate (smp lock level - LOWEST_IRQ_SMPLEVEL) to irq number

smplevel_to_irq: .byte	7,6,5,4,3,15,14,13,12,11,10,9,8,2,1,0

# Translate irq number to smplevel

irq_to_smplevel: .byte	LOWEST_IRQ_SMPLEVEL+15,LOWEST_IRQ_SMPLEVEL+14,LOWEST_IRQ_SMPLEVEL+13,LOWEST_IRQ_SMPLEVEL+4
		.byte	LOWEST_IRQ_SMPLEVEL+3, LOWEST_IRQ_SMPLEVEL+2, LOWEST_IRQ_SMPLEVEL+1, LOWEST_IRQ_SMPLEVEL+0
		.byte	LOWEST_IRQ_SMPLEVEL+12,LOWEST_IRQ_SMPLEVEL+11,LOWEST_IRQ_SMPLEVEL+10,LOWEST_IRQ_SMPLEVEL+9
		.byte	LOWEST_IRQ_SMPLEVEL+8, LOWEST_IRQ_SMPLEVEL+7, LOWEST_IRQ_SMPLEVEL+6, LOWEST_IRQ_SMPLEVEL+5

# Translate smplevel to local apic's tskpri

smplevel_to_tskpri:
		.byte	0							# smplevel 0 -> tskpri 0, ie, allow all interrupts
		.fill	LOWEST_IRQ_SMPLEVEL-1,1,INT_LOWIPL			# all below lowest IRQ just inhibits softint delivery
		.byte	INT_IOAPIC_0, INT_IOAPIC_1, INT_IOAPIC_2, INT_IOAPIC_3	# irq smplevels -> corresponding int vector
		.byte	INT_IOAPIC_4, INT_IOAPIC_5, INT_IOAPIC_6, INT_IOAPIC_7	# ... to inhibit that interrupt and all lower ones
		.byte	INT_IOAPIC_8, INT_IOAPIC_9, INT_IOAPIC_10,INT_IOAPIC_11
		.byte	INT_IOAPIC_12,INT_IOAPIC_13,INT_IOAPIC_14,INT_IOAPIC_15
		.byte	INT_INTERVAL				# interval timer
		.fill	256-LOWEST_IRQ_SMPLEVEL-17,1,-1		# all above highest irq and timer -> FF, ie, inhibit all interrupts

# Translate irq number to interrupt vector

irq_to_intvec:	.byte	INT_IOAPIC_15,INT_IOAPIC_14,INT_IOAPIC_13,INT_IOAPIC_4
		.byte	INT_IOAPIC_3, INT_IOAPIC_2, INT_IOAPIC_1, INT_IOAPIC_0
		.byte	INT_IOAPIC_12,INT_IOAPIC_11,INT_IOAPIC_10,INT_IOAPIC_9
		.byte	INT_IOAPIC_8, INT_IOAPIC_7, INT_IOAPIC_6, INT_IOAPIC_5

# Entrypoints for each irq handling routine to be copied to IDT on initialization

idt_irq_fix:	.long	 irq_0_entry, irq_1_entry, irq_2_entry, irq_3_entry
		.long	 irq_4_entry, irq_5_entry, irq_6_entry, irq_7_entry
		.long	 irq_8_entry, irq_9_entry,irq_10_entry,irq_11_entry
		.long	irq_12_entry,irq_13_entry,irq_14_entry,irq_15_entry

##########################################################################
##									##
##  Map the local APIC to overlay the ALTCPUPAGE.  The altcpu's will 	##
##  still see the phys page when they start up as they don't start 	##
##  with paging on.  Then, when they enable paging, they will see their ##
##  own local APIC at these same virtual addresses.			##
##									##
##########################################################################

					# these symbols must match the same ones in oz_preloader_486.s
altcpustart_w3_gdtdes = 4082		# gdt descriptor that the cpu's load
					# set by oz_hw_cpu_bootalts before it starts the cpus
altcpustart_l_entry   = 4088		# address to jump to after in 32-bit mode
					# set by oz_hw_cpu_bootalts before it starts the cpus
					# the cpu jumps to this address with %edx = its cpu index number
altcpustart_l_cpuidx  = 4092		# incremented by each alternate cpu that starts
					# oz_hw_cpu_bootalts sets this to 1 before it starts the cpus

	.align	4
	.globl	oz_hw486_apic_map
	.type	oz_hw486_apic_map,@function
oz_hw486_apic_map:

# Since we overlay the ALTCPUSTART page with the local APIC, first fix up the ALTCPUSTART page

	movl	$ALTCPUBASE,%edi		# point to alt cpu startup page
						# it has been set up by the preloader

	movw	oz_hw486_kernel_gdtr+0,%ax	# this is hideous
	movl	oz_hw486_kernel_gdtr+2,%ebx	# who invented these computers anyway
	movw	%ax,altcpustart_w3_gdtdes+0(%edi)
	movl	%ebx,altcpustart_w3_gdtdes+2(%edi)
	movl	$oz_hw486_cpu_bootalt,altcpustart_l_entry(%edi)
	movl	$1,altcpustart_l_cpuidx(%edi)

# Now map the local APIC over it

	movl	$1,%eax				# see if this cpu has rdmsr/wrmsr instructions
	cpuid
	bt	$9,%edx				# see if this chip has an local apic
	jnc	no_apic_map			# if not, can't use apic
	movl	$0xFEE00000,%eax		# set up default local apic address
	bt	$5,%edx				# check for rdmsr instruction capability
	jnc	apic_map_default
	movl	$27,%ecx			# ok, read the apic physical base address
	rdmsr					# ... into eax
	orl	$0x00000800,%eax		# set the global enable bit
	wrmsr
	andl	$0xFFFFF000,%eax		# get rid of junk from bottom bits
apic_map_default:
	movl	oz_SECTION_PAGESTATE_VALID_W,%edx # set it to valid, kernel read/write, cache disabled
	shll	$9,%edx
	orl	$0x118|PT_KRW,%edx
	orl	%eax,%edx
	movl	%edx,SPTBASE+(APICBASE/1024)
	invlpg	APICBASE
	movl	$INT_LOWIPL,APIC_L_TSKPRI	# inhibit softint delivery so oz_hw_cpu_getcur will work
	movl	$1,oz_hw486_apicmapped		# let anyone who cares know it is mapped now
	pushl	%eax				# this should print in the cpu's colour
	pushl	$apic_msg1
	call	oz_knl_printk
	addl	$8,%esp
	pushl	APIC_L_LCLVER
	pushl	APIC_L_LCLID
	pushl	$apic_msg2
	call	oz_knl_printk
	addl	$12,%esp

	ret

no_apic_map:
	pushl	%edx
	pushl	$apic_msg0			# these routines require an local apic
	call	oz_crash

apic_msg0:	.string	"oz_hw_smproc_486: this kernel requires local apic's (CPUID features=0x%X)"
apic_msg1:	.string	"oz_hw_smproc_486: apic mapped, phys address %x\n"
apic_msg2:	.string	"oz_hw_smproc_486: apic lclid %x, lclver %x\n"

##########################################################################
##									##
##  Start up the alternate cpus, cause them to call 			##
##  oz_knl_boot_othercpu						##
##									##
##  This routine is not called if the uniprocessor sysgen param is set	##
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
	pushl	%ebp				# make a call frame
	movl	%esp,%ebp
	pushl	%edi
	pushl	%esi
	pushl	%ebx

	pushfl					# prevent isr's from accessing local APIC
	cli

	movl	APIC_L_ICREG1,%edi		# get current contents of apic interrupt control register
	movl	APIC_L_ICREG0,%esi
	andl	$0x00FFFFFF,%edi		# mask off everything except the reserved bits
	andl	$0xFFF32000,%esi

	movl	%edi,APIC_L_ICREG1		# clear destination field bits (though it probably really doesn't matter)

	# Intel chips require only the START message (at least on Asus P2B-D mobo)
	# AMD chips require both INIT's then the START (at least on MSI K7D mobo)

	movl	%esi,%eax			# set up 'INIT assert, all excl self'
	orl	$0x000C4500,%eax
	movl	%eax,APIC_L_ICREG0
	pushl	$100000000			# wait 100mS
	call	oz_hw_stl_nanowait
	addl	$4,%esp

	movl	%esi,%eax			# set up 'INIT deassert, all excl self'
	orl	$0x000C0500,%eax
	movl	%eax,APIC_L_ICREG0
	pushl	$100000000			# wait 100mS
	call	oz_hw_stl_nanowait
	addl	$4,%esp

	call	oz_hw_tod_iotanow		# get current time to help synchronize rdtsc's

	movl	%esi,%eax			# set up 'start up everything except myself' message
	orl	$0x000C4600+ALTCPUPAGE,%eax
	movl	%eax,APIC_L_ICREG0		# store the command back in apic, this should start the other cpus

	popfl					# restore hardware interrupt delivery
	popl	%ebx				# restore scratch regs and return
	popl	%esi
	popl	%edi
	popl	%ebp
	ret

##########################################################################
##									##
##  Set up the per-cpu apic register contents				##
##									##
##    Input:								##
##									##
##	%ecx = return address						##
##	hardware interrupt delivery inhibited				##
##									##
##    Output:								##
##									##
##	%eax = cpu id number						##
##									##
##    Note:								##
##									##
##	assume there is no stack					##
##									##
##########################################################################

	.align	4
	.globl	oz_hw486_apic_setup
	.type	oz_hw486_apic_setup,@function
oz_hw486_apic_setup:

	movl	$INT_LOWIPL,APIC_L_TSKPRI	# task priority = 0x20 to inhibit softint delivery
						# (so oz_hw_cpu_getcur will work)
	movl	$0x10000,APIC_L_LCLVTBL		# mask off the timer interrupt
	movl	$0x08700,APIC_L_LINT0		# set up the ExtINT stuff to allow I/O interrupts
	movl	$0x00400,APIC_L_LINT1		# allow NMI's
	movl	$0x10000,APIC_L_PERFCNT		# mask off performance interrupt
	movl	$0x10000,APIC_L_LERROR		# mask off error interrupt
	movl	$0x0010F,APIC_L_SPURINT		# enable the local APIC
	movl	$0x08700,APIC_L_LINT0		# set up the ExtINT stuff to allow I/O interrupts
	movl	$0x00400,APIC_L_LINT1		# allow NMI's

	movl	APIC_L_LCLID,%eax		# get my physical id
	shrl	$24,%eax			# shift to eax<0..3>
	andl	$15,%eax			# mask off the bits
	cmpl	$MAXCPUS,%eax			# make sure it is not too big
	jnc	apic_setup_cpuidbad

.if MAXCPUS>8
  error : this code assumes MAXCPUS .le. 8
.endif
	movl	%ecx,%edx
	movl	%eax,%ecx
	movl	$0x01000000,%eax		# make up a logical destination bit
	shll	%cl,%eax			# - used by oz_hw486_apic_invlpgedx
	movl	%eax,APIC_L_LOGDST
	movl	$-1,APIC_L_DSTFMT		# - use the 'flat model'
	movl	%ecx,%eax			# return processor number in %eax
	lock					# enable the nmi routine for this processor
	bts	%ecx,enabled_nmis
	jmp	*%edx				# return

apic_setup_cpuidbad:
	cli					# inhibit interrupt delivery
	movl	$0x10000,APIC_L_LINT1		# turn off NMI delivery
	hlt					# don't crash, just hang here forever doing nothing
	jmp	apic_setup_cpuidbad		# - not much we can do without a stack, anyway

##########################################################################
##									##
##  Shut down APIC interrupts in preparation for halting, 		##
##  return my cpu index in %eax						##
##									##
##########################################################################

	.globl	oz_hw486_apic_shut
	.type	oz_hw486_apic_shut,@function
oz_hw486_apic_shut:
	movl	$0,APIC_L_LOGDST		# this LOGDST is never used by IOAPIC to send interrupts
	movl	$0xFF,APIC_L_TSKPRI		# make this cpu have maximum priority so it can't take any more interrupts
	movl	$0x10000,APIC_L_LINT1		# disable NMI processing
	movl	APIC_L_LCLID,%eax		# get local processor's id in eax<24..27>
	shrl	$24,%eax			# shift to get cpu index
	andl	$15,%eax			# mask off reserved bits
	lock					# disable the nmi routine for this processor
	btr	%eax,enabled_nmis
	movl	$0x10000,APIC_L_LINT0		# disable I/O interrupt processing
	movl	$MAXCPUS,%ecx			# make sure no one is waiting for us to invalidate an pte
	movl	$oz_hw486_cpudb,%esi
apic_shut_loop:
	lock
	btr	%eax,CPU_L_INVLPG_CPUS(%esi)
	addl	$CPU__SIZE,%esi
	loop	apic_shut_loop
	ret

##########################################################################
##									##
##  Invalidate the other cpu's page pointed to by %edx			##
##									##
##    Input:								##
##									##
##	%edx = virt address of page to invalidate			##
##	smplevel >= softint						##
##									##
##    Scratch:								##
##									##
##	%eax,%ebx,%ecx,%edx,%esi					##
##									##
##########################################################################

	.align	4
	.globl	oz_hw486_apic_invlpgedx
	.type	oz_hw486_apic_invlpgedx,@function
oz_hw486_apic_invlpgedx:
	call	oz_hw486_getcpu			# get my cpudb entry pointer (preserves %edx)
	movl	%edx,CPU_L_INVLPG_VADR(%esi)	# save the virtual address to be invalidated
	movl	oz_hw486_activecpus,%ecx	# get mask of all cpu's (including self)
	cmpl	$PROCBASVADDR,%edx		# see if it's a process or system address
	jc	invlpgedx_start			# if system address, do them all
	pushl	%eax				# process, only do those mapped to same process as me
	movl	CPU_L_PRCTX(%esi),%eax		#   see what process I have loaded now
	movl	$MAXCPUS-1,%edx			#   get number of other cpu's to scan
	movl	$cpudb_end,%ebx			#   point to the end of the list of cpu's
invlpgedx_cpuloop:
	subl	$CPU__SIZE,%ebx			#   point to next table entry
	cmpl	CPU_L_PRCTX(%ebx),%eax		#   see if it has same process as I do
	je	invlpgedx_cpuskip
	btr	%edx,%ecx			#   if not, clear its bit so I won't wack it
invlpgedx_cpuskip:
	decl	%edx				#   see if any more entries to check
	jnc	invlpgedx_cpuloop		#   repeat if so
	popl	%eax				#   restore my cpu number
invlpgedx_start:
	btr	%eax,%ecx			# reset my cpu's bit in the mask (because we are only doing other cpu's)
	jecxz	invlpgedx_done			# all done if this is the only cpu
	movl	%ecx,%eax
	movl	%eax,CPU_L_INVLPG_CPUS(%esi)	# set mask of cpu's that haven't updated yet
	xorl	%ecx,%ecx			# max time to wait for completion by others
invlpgedx_wack:
	testw	%cx,%cx				# wack cpu's every 64K times around loop
	jne	invlpgedx_test
	shll	$24,%eax			# make mask of LOGDST's to interrupt = those that haven't responded yet
	pushfl					# inhibit hardware interrupt delivery
	cli					# ... so ISR's can't mess with APIC on us
	movl	%eax,APIC_L_ICREG1		# set mask of LOGDST's to interrupt
	movl	$0x0800|INT_INVLPG_SMP,APIC_L_ICREG0 # set the interrupt vector, tell it 'Fixed, Edge-triggered, Logical-destination'
	popfl					# restore hardware interrupt delivery while waiting
invlpgedx_test:
	movl	CPU_L_INVLPG_CPUS(%esi),%eax	# see who has yet to respond
	testl	%eax,%eax
	loopne	invlpgedx_wack			# if not all done, wait some more
	jne	invlpgedx_crash1		# if still not all done, crash
invlpgedx_done:
	ret

invlpgedx_crash1:
	pushl	%eax
	pushl	$invlpgedx_msg1
	call	oz_crash

invlpgedx_msg1:	.string	"oz_hw486_apic_invlpgedx: cpu's 0x%8.8x failed to invalidate"

##########################################################################
##									##
##  This routine is called as a result of receiving an INT_INVLPG_SMP	##
##  interrupt initiated via another cpu's local apic.			##
##									##
##  It scans the cpudb entries to look for pages that it should 	##
##  invalidate.								##
##									##
##  This is the highest priority interrupt.  We don't use the NMI, 	##
##  because that would interrupt one of the INT_INVLPG_... type 	##
##  routines, which execute in kernel mode.  Since this routine runs 	##
##  in executive mode, that would be a mess.  What would happen is 	##
##  this routine runs in executive mode (even though it interrupted a 	##
##  kernel routine), then it returns to executive mode instead of 	##
##  kernel mode, so the routine that was supposed to run in kernel 	##
##  mode ends up running in executive mode and it pukes.		##
##									##
##########################################################################

	.align	4
proc_invlpg_smp:

## Build mchargs on stack

	pushl	%ebp				# make a call frame
	pushal					# save registers
	pushl	$0				# push an dummy ec2
	movl	$0,MCH_L_EC1(%esp)		# clear out the ec1
	call	oz_hw_fixkmexceptebp		# point to stack frame

## Acknowledge interrupt out of the APIC

	movl	$0,APIC_L_EOI			# tell apic end-of-interrupt so it will queue another one

## Scan for TLB cache entries to invalidate

	call	oz_hw486_getcpu			# see which cpu we're on, get cpu index in %eax
invlpg_smp_repeat:
	movl	$MAXCPUS,%ecx			# get total number of possible cpu's
	movl	$oz_hw486_cpudb,%edi		# point to base of data array
	xorl	%ebx,%ebx			# haven't seen anything yet
invlpg_smp_loop:
	movl	CPU_L_INVLPG_VADR(%edi),%edx	# get virtual address to be invalidated before we clear the flag bit
	lock					# see if this cpu has yet to invalidate the entry
	btr	%eax,CPU_L_INVLPG_CPUS(%edi)	# ... and clear the flag, thus indicating we have invalidated it
	jnc	invlpg_smp_next			# if so, don't do it again
	INVLPG_EDX				# ok, invalidate the entry
	incl	%ebx
invlpg_smp_next:
	addl	$CPU__SIZE,%edi			# anyway, point to next cpudb entry
	loop	invlpg_smp_loop			# repeat to process that entry
	testl	%ebx,%ebx			# repeat until we don't do any
	jne	invlpg_smp_repeat

## All done, return

	addl	$4,%esp				# pop the dummy ec2
	popal
	pop	%ebp
	iret

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
	movl	$1,debughaltmode		# set the flag to get cpu's to call the oz_knl_debug_halted routine
	movl	$0x000C0402,APIC_L_ICREG0	# set the NMI vector (2), tell it 'NMI, Edge-triggered, All-Excp-Self'
	ret

##########################################################################
##
##  This is from an INT_FAKENMI to do an nmi thing.  It doesn't do an APIC_L_EOI.
##

	.align	4
process_fakenmi:

## Build mchargs on stack

	pushl	%ebp				# make a call frame
	pushal					# save registers
	pushl	$0				# push an dummy ec2
	movl	$0,MCH_L_EC1(%esp)		# clear out the ec1
	call	oz_hw_fixkmexceptebp		# point to stack frame
	jmp	process_nmi			# go process then nmi

##########################################################################
##
## NMI handler from the APIC - note that oz_knl_debug_halted does its own recursion testing
## This handler is called in executive mode with interrupt delivery inhibited
##
	.align	4
process_apicnmi:

## Build mchargs on stack

	pushl	%ebp				# make a call frame
	pushal					# save registers
	pushl	$0				# push an dummy ec2
	movl	$0,MCH_L_EC1(%esp)		# clear out the ec1
	call	oz_hw_fixkmexceptebp		# point to stack frame

## Acknowledge interrupt out of the APIC

	movl	$0,APIC_L_EOI			# tell apic end-of-interrupt so it will queue another nmi

## See if nmi's are enabled for this cpu
## They are typically inhibited while we're in the video routines as it wouldn't do 
##   any good to call the debugger and hang when it tries to output to the screen

	call	oz_hw_cpu_getcur
	bt	%eax,enabled_nmis
	jnc	nmi_pending

## Clear the pending_nmis bit for this cpu

	lock
	btr	%eax,pending_nmis

## If the debug halt flag is set, call the debugger

process_nmi:
	cmpl	$0,debughaltmode		# see if being halted for debug
	je	nmi_return
	pushl	%esp				# point to the mchargs
	call	oz_knl_debug_halted		# this cpu is now halted
	addl	$4,%esp				# pop mchargs pointer
	movl	$0,debughaltmode		# when one of them returns, it is ok to let them all go

## All done, return

nmi_return:
	addl	$4,%esp				# pop the dummy ec2
	popal
	pop	%ebp
	iret

## NMI delivery is inhibited, set the pending bit and return as if nothing happened

nmi_pending:
	lock
	bts	%eax,pending_nmis
	jmp	nmi_return

##########################################################################
##									##
##  Timer routines							##
##									##
##  The local APIC timer is used for two things:			##
##									##
##	1) Event timer							##
##	2) Quantum timer						##
##									##
##  Each cpu is responsible for its own quantum timer.  The event 	##
##  timer duties are shared among the processors.  Namely, whoever 	##
##  sets the event time value, their local APIC will be used to 	##
##  process it.								##
##									##
##########################################################################

##########################################################################
##									##
##  Set the datebin of the next event					##
##  When this time is reached, call oz_knl_timer_timeisup		##
##									##
##    Input:								##
##									##
##	8:4(%esp) = datebin of next event				##
##	smplock = tm							##
##									##
##########################################################################

	####	setevent_deb1:	.string	"oz_hw_timer_setevent*: event %8.8X:%8.8X, rdtsc %8.8X:%8.8X\n"

	.align	4
	.globl	oz_hw_timer_setevent
	.type	oz_hw_timer_setevent,@function
oz_hw_timer_setevent:
	pushl	%ebp
	movl	%esp,%ebp
	pushl	%edi
	pushl	%esi
	pushl	%ebx

	movl	 8(%ebp),%eax			# get absolute system time of the event
	movl	12(%ebp),%edx
	pushfl
	cli
	call	oz_hw486_asys2iota		# ... 0 if underflow, 0xFFFFFFFFFFFFFFFF if overflow
	popfl
	movl	%eax,timer_event_when+0
	movl	%edx,timer_event_when+4
	movl	$-1,timer_event_cpuid		# say it is not running on any cpu so far

	####		rdtsc
	####		pushl	%eax
	####		pushl	%edx
	####		pushl	timer_event_when+0
	####		pushl	timer_event_when+4
	####		pushl	$setevent_deb1
	####		call	oz_knl_printk
	####		addl	$20,%esp

	call	timer_setup			# start the timer going if not already

	popl	%ebx				# all done
	popl	%esi
	popl	%edi
	leave
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
##	 4(%esp) = delta iotatime from now to expire (<= 0.1 sec)	##
##	12(%esp) = current iota time					##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_quantimer_start
	.type	oz_hw_quantimer_start,@function
oz_hw_quantimer_start:
	####		ret	####

	pushl	%ebp
	movl	%esp,%ebp
	pushl	%edi
	pushl	%esi
	pushl	%ebx
	call	oz_hw486_getcpu			# get cpu database pointer in %esi
	movl	 8(%ebp),%eax			# get absolute expiration time in %edx:%eax
	movl	12(%ebp),%edx
	addl	16(%ebp),%eax
	adcl	20(%ebp),%edx
	movl	%eax,CPU_Q_QUANTIM+0(%esi)	# save the expiration time in cpu struct
	movl	%edx,CPU_Q_QUANTIM+4(%esi)	# = RDTSC when quantum expires

	pushl	$smplock_tm			# set the timer smplock
	call	oz_hw_smplock_wait
	pushl	%eax
	call	timer_setup			# start the timer going
	pushl	$smplock_tm			# clear the timer smplock
	call	oz_hw_smplock_clr
	addl	$12,%esp
	popl	%ebx
	popl	%esi
	popl	%edi
	leave
	ret

##########################################################################
##
##  Set timer to expire at either the event timer or quantum timer, 
##  which ever is sooner.  If any of those have already expired, call 
##  the appropriate processing routine.
##
##    Input:
##
##	timer_event_when   = when to expire (absolute RDTSC value)
##	CPU_Q_QUANTIM(cpu) = absolute quantum expiration (absolute RDTSC value)
##	smplock = tm
##
##    Scratch:
##
##	%eax,%ebx,%ecx,%edx,%esi,%edi

timer_setup:
	pushl	%ebp
	movl	%esp,%ebp

	call	oz_hw_tod_iotanow		# RDTSC, cpu independent
	pushl	%edx				# save it at -4:-8(%ebp)
	pushl	%eax
	call	oz_hw486_getcpu			# get this cpu's index (%eax) and database pointer (%esi)
	pushl	%eax				# save index at -12(%ebp)

timer_setup_loop:
	movl	timer_event_when+0,%eax		# see if event timer has expired
	movl	timer_event_when+4,%ebx
	subl	-8(%ebp),%eax
	sbbl	-4(%ebp),%ebx
	jc	timer_setup_eventexp
	movl	CPU_Q_QUANTIM+0(%esi),%ecx	# see if quantum timer has expired
	movl	CPU_Q_QUANTIM+4(%esi),%edx
	subl	-8(%ebp),%ecx
	sbbl	-4(%ebp),%edx
	jc	timer_setup_quantexp
	movl	timer_event_cpuid,%edi		# neither expired, see which happens earlier
	testl	%edi,%edi
	js	timer_setup_compare
	cmpl	-12(%ebp),%edi
	jne	timer_setup_quantum		# (force quantum if someone else is already doing event timer)
timer_setup_compare:
	subl	%eax,%ecx
	sbbl	%ebx,%edx
	jnc	timer_setup_event
	movl	$-1,timer_event_cpuid		# - quantum, if we were doing event timer, we aren't anymore
	addl	%eax,%ecx			#   restore delta quantum in %edx:%ecx
	adcl	%ebx,%edx
timer_setup_quantum:
	movl	%ecx,%eax			#   get delta quantum in %ebx:%eax
	movl	%edx,%ebx
	jmp	timer_setup_delta
timer_setup_event:
	movl	-12(%ebp),%ecx			# - event, mark it in progress by this cpu
	movl	%ecx,timer_event_cpuid		#   (delta event already in %ebx:%eax)
timer_setup_delta:
	####		pushl	%eax
	####		pushl	%eax
	####		pushl	%ebx
	####		pushl	$timer_setup_deb1
	####		call	oz_knl_printk
	####		addl	$12,%esp
	####		popl	%eax
	movl	$-1,%edi			# (assume it will take a long time)
	roll	$16,%ebx			# convert delta RDTSC in %ebx:%eax
	testw	%bx,%bx
	jne	timer_setup_start
	roll	$16,%eax
	xchgl	%eax,%ebx
	movw	%bx,%ax
	xorl	%edx,%edx
	divl	rdtsc_per_apic_16
	testl	%eax,%eax
	jne	timer_setup_start
	xorw	%bx,%bx
	movl	%ebx,%eax
	divl	rdtsc_per_apic_16
	testl	%eax,%eax
	movl	%eax,%edi			# ... to delta APIC in %edi
	jne	timer_setup_start		# make sure at least 1 so apic doesn't
	incl	%edi				# ... think we're shutting timer off
timer_setup_start:
	####		rdtsc
	####		pushl	%eax
	####		pushl	%edx
	####		pushl	%edi
	####		pushl	$timer_setup_deb2
	####		call	oz_knl_printk
	####		addl	$16,%esp
	movl	$0x10000,APIC_L_LCLVTBL		# mask off the timer interrupt
	movl	$0xB,APIC_L_TIMDVCR		# init the divisor to 1 (ie, clock speed = bus speed / 1)
	movl	%edi,APIC_L_TIMINIT		# set up count & start timing
	movl	$INT_INTERVAL,APIC_L_LCLVTBL	# enable timer interrupt

	leave
	ret

## Event time has arrived, mark it expired then call the kernel's routine

timer_setup_eventexp:
	movl	$-1,timer_event_when+0		# say the timer has expired, don't bother doing it anymore
	movl	$-1,timer_event_when+4
	movl	$-1,timer_event_cpuid
	call	oz_knl_timer_timeisup		# call the kernel's high-level routine
	jmp	timer_setup_loop		# repeat if there is anything more to do

## Quantum time has arrived, mark it expired then queue softint to the local processor

timer_setup_quantexp:
	movl	$-1,CPU_Q_QUANTIM+0(%esi)
	movl	$-1,CPU_Q_QUANTIM+4(%esi)
	movl	APIC_L_LCLID,%eax		# get local processor's id in eax<24..27>
	andl	$0xFF000000,%eax		# mask off any junk bits
	pushfl					# inhibit hardware interrupt delivery
	cli					# - so no one else can access APIC regs while we are
	movl	%eax,APIC_L_ICREG1		# set the cpuid to interrupt
	movl	$INT_QUANTIM,APIC_L_ICREG0	# set the interrupt vector, tell it 'Fixed, Edge-triggered'
	popfl					# restore hardware interrupt delivery
	jmp	timer_setup_loop		# see if there is anything more to do

	####	timer_setup_deb1:	.string	"timer_setup*: delta rdtsc %8.8X:%8.8X\n"
	####	timer_setup_deb2:	.string	"timer_setup*: delta apic %8.8X at rdtsc %8.8X:%8.8X\n"

##########################################################################
##
##  This high-priority interrupt routine is called when the timer expires
##

	####	procinterval_deb1:	.string	"procinterval*: rdtsc %8.8X:%8.8X\n"

	.align	4
procinterval:
	pushl	%ebp
	pushal
	pushl	$0
	call	oz_hw_fixkmexceptebp

## Lock the timer database

	pushl	$smplock_tm
	call	oz_hw_smplock_wait
	movl	$0,APIC_L_EOI
	pushl	%eax

	####		rdtsc
	####		pushl	%eax
	####		pushl	%edx
	####		pushl	$procinterval_deb1
	####		call	oz_knl_printk
	####		addl	$12,%esp

## Process timer stuff

	call	timer_setup

## Unlock timer and return where we left off

	cli
	pushl	$smplock_tm
	call	oz_hw_smplock_clr
	addl	$12,%esp

	addl	$4,%esp
	popal
	popl	%ebp
	iret

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
	pushl	%ebp				# make a call frame for tracing
	movl	%esp,%ebp
	pushl	%esi				# save C registers
	pushfl					# inhibit hardware interrupt delivery if not already
	cli					# - so cpu doesn't get switched out from under us
						# - and so no one else messes with local APIC
	call	oz_hw486_getcpu			# point %esi at current cpu data
	movzbl	CPU_B_SMPLEVEL(%esi),%eax	# get current smp lock level
	movl	$1,%edx
	cmpl	$2,%eax				# must be at level 0 or 1
	movl	APIC_L_TSKPRI,%ecx		# get the reserved bits
	jnc	setsoftint_crash1		# barf if not at smplevel 0 or 1
	xorb	%cl,%cl				# assume softints being enabled (set task priority=0)
	subl	8(%ebp),%edx			# get new smplock level
	jc	setsoftint_crash2		# (must be a 0 or 1)
	jz	setsoftint_enable
	movb	$INT_LOWIPL,%cl			# softints being inhibited, set task priority=INT_LOWIPL
						# - this also inhibits INT_QUANTIM, INT_DIAGMODE and INT_RESCHED
						#   because they are in the same block of 16
setsoftint_enable:
	movb	%dl,CPU_B_SMPLEVEL(%esi)	# store corresponding smp lock level
	movl	%ecx,APIC_L_TSKPRI		# store new apic task priority
	UPDATEVIDEOSTATUS 4(%ebp)
	popfl					# restore hardware interrupt enable
	xorl	$1,%eax				# return prior enable state
	popl	%esi				# restore C registers
	leave					# pop call frame
	ret

setsoftint_crash1:
	pushl	%ebx
	pushl	$setsoftint_msg1
	call	oz_crash

setsoftint_crash2:
	pushl	8(%ebp)
	pushl	$setsoftint_msg2
	call	oz_crash

setsoftint_msg1:	.string	"oz_hw_cpu_setsoftint: called at smplevel %x"
setsoftint_msg2:	.string	"oz_hw_cpu_setsoftint: invalid argument %u"

##########################################################################
##									##
##  This routine is called by the scheduler to simply wake another cpu	##
##  out of the HLT instruction in the OZ_HW_WAITLOOP_WAIT macro		##
##									##
##  All it does is do an IPI and that CPU just does an iret		##
##									##
##    Input:								##
##									##
##	4(%esp) = cpuidx to interrupt					##
##	smplevel >= softint						##
##									##
##########################################################################

	.align	4
	.globl	oz_hw486_waitloop_wake
	.type	oz_hw486_waitloop_wake,@function
oz_hw486_waitloop_wake:
	movl	APIC_L_LCLID,%eax		# get local processor's id in eax<24..27>
	shrl	$24,%eax			# shift to get cpu id
	andl	$15,%eax			# mask off reserved bits
	movl	4(%esp),%edx			# don't wack myself
	cmpl	%edx,%eax
	je	waitloop_wake_rtn
	shll	$24,%edx			# shift cpu index number over for ICREG1
	pushfl					# inhibit hardware interrupt delivery
	cli					# - so no one else can access APIC regs while we are
	movl	%edx,APIC_L_ICREG1		# set the cpuid to interrupt
	movl	$INT_WAITLOOPWAKE,APIC_L_ICREG0	# set the interrupt vector, tell it 'Fixed, Edge-triggered'
	popfl					# restore hardware interrupt delivery
waitloop_wake_rtn:
	ret

	.align	4
waitloopwake:					# interrupted the HLT instruction
	movl	$0,APIC_L_EOI			# acknowledge the interrupt then
	iret					# return to the CLI instruction

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
	.globl	oz_hw_cpu_lowiplint
	.type	oz_hw_cpu_lowiplint,@function
oz_hw_cpu_lowiplint:				# - cause oz_knl_lowipl_handleint to be called
	pushl	%ebp
	movl	%esp,%ebp
	movl	8(%ebp),%eax			# get cpuid to be interrupted
	cmpl	$MAXCPUS,%eax			# make sure cpuid is in range
	jnc	softint_crash1
	shll	$24,%eax			# shift over for APIC_L_ICREG1
	pushfl					# inhibit hardware interrupt delivery
	cli					# - so no one else can access APIC regs while we are
	movl	%eax,APIC_L_ICREG1		# set the cpuid to interrupt
	movl	$INT_LOWIPL,APIC_L_ICREG0	# set the interrupt vector, tell it 'Fixed, Edge-triggered'
	popfl					# restore hardware interrupt delivery
	leave					# remove stack frame
	ret

	.align	4
	.globl	oz_hw_cpu_reschedint
	.type	oz_hw_cpu_reschedint,@function
oz_hw_cpu_reschedint:				# - cause oz_knl_thread_handleint to be called
	pushl	%ebp
	movl	%esp,%ebp
	movl	8(%ebp),%eax			# get cpuid to be interrupted
	cmpl	$MAXCPUS,%eax			# make sure cpuid is in range
	jnc	softint_crash1
	shll	$24,%eax			# shift over for APIC_L_ICREG1
	pushfl					# inhibit hardware interrupt delivery
	cli					# - so no one else can access APIC regs while we are
	movl	%eax,APIC_L_ICREG1		# set the cpuid to interrupt
	movl	$INT_RESCHED,APIC_L_ICREG0	# set the interrupt vector, tell it 'Fixed, Edge-triggered'
	popfl					# restore hardware interrupt delivery
	leave					# remove stack frame
	ret

softint_crash1:
	pushl	%eax
	pushl	$softint_msg1
	call	oz_crash

softint_msg1:	.string	"oz_hw_cpu_softint: cpu id %u out of range"

##########################################################################
##									##
##  Cause all cpus to enter diag mode					##
##									##
##  This routine is called at high ipl when control-shift-D is pressed.	##
##  It interrupts all cpu's to call the procdiagmode handler when they 	##
##  have softints enabled.						##
##									##
##########################################################################

	.align	4
	.global	oz_hw_diag
	.type	oz_hw_diag,@function
oz_hw_diag:
	movl	$1,diagmodeflag				# set the first cpu flag for the procdiagmode routine
	movl	$0x00080000|INT_DIAGMODE,APIC_L_ICREG0	# set the interrupt vector, tell it 'Fixed, Edge-triggered, All-Incl-Self'
	ret

	.align	4
procdiagmode:
	call	softintsetup

	xorl	%edx,%edx			# fetch and clear 'diagmodeflag'
	lock
	xchgl	%edx,diagmodeflag

	pushl	%esp				# point to machine args on stack
	pushl	%edx				# push first cpu flag on stack
	pushl	%eax				# push cpu number (returned by softintsetup)
	call	oz_knl_diag			# call the diagnostic routine (cpu_index, first_cpu_flag, mchargs)
	addl	$12,%esp

	jmp	softintiret			# all done processing the software interrupt

##########################################################################
##									##
##  Get current cpu index and point to cpudb struct			##
##									##
##    Input:								##
##									##
##	apic = cpu specific registers					##
##	smplevel = at least softint (or the cpu might get switched)	##
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
	xorl	%eax,%eax			# return a zero if we haven't mapped apic yet
	cmpb	%al,oz_hw486_apicmapped
	jz	cpu_getcur_rtn
	movl	APIC_L_PROCPRI,%eax		# make sure softint delivery is inhibited
	cmpb	$INT_LOWIPL&0xF0,%al		# ... or our return value is meaningless
	jc	cpu_getcur_chkhwi		# ... as the cpu might get switched from under us
cpu_getcur_ok:
	movl	APIC_L_LCLID,%eax		# get local processor's id in eax<24..27>
	shrl	$24,%eax			# shift to get cpu id
	andl	$15,%eax			# mask off reserved bits
cpu_getcur_rtn:
	ret

cpu_getcur_chkhwi:
	pushfl					# also allow if hardware interrupts are inhibited
	popl	%ecx
	testb	$2,%ch
	je	cpu_getcur_ok
	pushl	%ebp				# too bad, crash
	movl	%esp,%ebp
	pushl	%ecx
	pushl	%eax
	pushl	4(%ebp)
	pushl	$cpu_getcur_msg1
	call	oz_crash
cpu_getcur_msg1: .string "oz_hw_cpu_getcur: called from %p with APIC_L_PROCPRI of %X, eflags %X"

	.align	4
	.globl	oz_hw486_getcpu
	.type	oz_hw486_getcpu,@function
oz_hw486_getcpu:				# assembler routine sets up %esi as well as %eax ...
	xorl	%eax,%eax			# return a zero if we haven't mapped apic yet
	movl	$oz_hw486_cpudb,%esi
	cmpb	%al,oz_hw486_apicmapped
	jz	getcpu_rtn
	movl	APIC_L_PROCPRI,%eax		# make sure softint delivery is inhibited
	cmpb	$INT_LOWIPL&0xF0,%al		# ... or our return value is meaningless
	jc	getcpu_chkhwi			# ... as the cpu might get switched from under us
getcpu_ok:
	movl	APIC_L_LCLID,%eax		# get local processor's id in eax<24..27>
	shrl	$24-CPU__L2SIZE,%eax		# shift to get cpudb entry offset
	andl	$15*CPU__SIZE,%eax		# mask off reserved bits
	addl	%eax,%esi			# point to cpu specific cpudb entry
	shrl	$CPU__L2SIZE,%eax		# finish shifting cpuid into eax<0..3>
getcpu_rtn:
	ret

getcpu_chkhwi:
	pushfl					# also allow if hardware interrupts are inhibited
	popl	%eax
	testb	$2,%ah
	je	getcpu_ok
	pushl	%ebp				# too bad, crash
	movl	%esp,%ebp
	pushl	%eax
	pushl	APIC_L_PROCPRI
	pushl	$cpu_getcur_msg1
	call	oz_crash
getcpu_msg1: .string "oz_hw486_getcpu: called with APIC_L_PROCPRI of %X, eflags %X"

##########################################################################
##									##
##  Get current cpu's lock level					##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_cpu_smplevel
	.type	oz_hw_cpu_smplevel,@function
oz_hw_cpu_smplevel:
	pushl	%esi				# save scratch register
	pushfl					# inhibit hardware interrupts
	cli					# ... so cpu doesn't switch on us
	call	oz_hw486_getcpu			# point to this cpu's data
	movzbl	CPU_B_SMPLEVEL(%esi),%eax	# get its smp level
	popfl					# restore hardware interrupts
	popl	%esi				# restore scratch register
	ret

##########################################################################
##									##
##  Set hardware interrupt enable bit on calling cpu			##
##									##
##    Input:								##
##									##
##	4(%esp) = 0 : inhibit hardware interrupt delivery		##
##	          1 : enable hardware interrupt delivery		##
##	         -1 : inhibit hw ints and nmi ints			##
##									##
##    Output:								##
##									##
##	%eax = 0 : interrupt delivery was previously inhibited		##
##	       1 : interrupt delivery was previously enabled		##
##	      -1 : maskable and non-maskable ints previous inhibited	##
##									##
##########################################################################

	.align	4
	.global	oz_hw_cpu_sethwints
	.type	oz_hw_cpu_sethwints,@function
oz_hw_cpu_sethwints:
	pushl	%ebp			# make call frame in case of crash
	movl	%esp,%ebp
	pushfl				# get copy of current eflags
	cli				# unconditionally make sure hw ints are inhibited for now
	popl	%edx			# save current setting
	shrl	$9,%edx			# get previous setting bit in place
	andl	$1,%edx			# mask out any junk in old value
	call	oz_hw_cpu_getcur	# get the cpu number we're on in %eax
	bt	%eax,enabled_nmis	# see if nmi's were enabled
	jc	sethwints_setnew
	testl	%edx,%edx		# nmi's inhibited, maskables should have been inhibited
	jne	sethwints_crash2
	decl	%edx			# ok, return both were inhibited
sethwints_setnew:
	movl	8(%ebp),%ecx		# get new setting
	testl	%ecx,%ecx
	jl	sethwints_veryoff	# - everything gets shut off
	lock				# set the nmi enabled bit for this cpu
	bts	%eax,enabled_nmis
	lock				# see if an nmi was pending for this cpu
	btr	%eax,pending_nmis
	jnc	sethwints_maybesti
	int	$INT_FAKENMI		# if so, call the nmi routine
sethwints_maybesti:
	jecxz	sethwints_rtn		#  0: hw ints off, nmi ints on
	sti				#  1: hw ints on, nmi ints on
	jmp	sethwints_rtn
sethwints_veryoff:
	lock				# -1: hw ints off, nmi ints off
	btr	%eax,enabled_nmis
sethwints_rtn:
	movl	%edx,%eax
	leave
	ret

sethwints_crash2:
	pushl	$sethwints_msg2
	call	oz_crash

sethwints_msg2:	.string	"oz_hw_cpu_sethwints: nmi's inhibited but maskables were enabled"

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
##	interrupt and any lower level irq's, as the local APIC 		##
##	interrupt mask will be programmed accordingly.			##
##									##
##	This routine preserves the sti/cli interrupt enable bit in the 	##
##	processor.  So it may be called with either state of the bit.	##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_smplock_wait
	.type	oz_hw_smplock_wait,@function
oz_hw_smplock_wait:
	pushl	%ebp				# make a call frame
	movl	%esp,%ebp
	pushl	%esi				# save scratch register
	pushl	$0				# a place for return value
	pushfl					# inhibit hardware interrupts
	cli					# ... so cpu doesn't switch from under us
	call	oz_hw486_getcpu			# get my cpu data
	movb	%al,%ah				# save my cpu index here
	movl	8(%ebp),%ecx			# point to lock block
	movzbl	CPU_B_SMPLEVEL(%esi),%edx	# get existing smplevel here
	movl	%edx,4(%esp)			# save existing smplevel for return value
	movb	SL_B_LEVEL(%ecx),%dh		# get new smplevel here
# %ah  = cpu index
# %ecx = smplock pointer
# %dl  = old smp level
# %dh  = new smp level
# %esi = cpu data pointer
	cmpb	%dl,%dh				# compare new level : current level
	jc	smplock_wait_crash1		# crash if new level < current level
	je	smplock_wait_same		# no-op if new level == current level
	cmpb	SL_B_CPUID(%ecx),%ah		# level is being increased, i better not already own this lock
	je	smplock_wait_crash2

# Lock is at higher level than this cpu is at, acquire it

	movb	%dh,CPU_B_SMPLEVEL(%esi)	# ok, increase cpu's smplock level

	movzbl	%dh,%esi			# put the smplevel here for now
	movl	APIC_L_TSKPRI,%edx		# get the reserved bits of TSKPRI
	movb	smplevel_to_tskpri(%esi),%dl	# get task priority value corresponding to new smp level
	movl	%edx,APIC_L_TSKPRI		# store back in TSKPRI register
	UPDATEVIDEOSTATUS 4(%ebp)
	popfl					# ... then restore hardware interrupts
						# - hardware interrupts on this cpu at or below the level of the 
						#   smp lock will be blocked because the local APIC's TSKPRI is high
						# - so interrupts above this new smplock are ok (even during the wait loop)
						#   because they should not try to lock this lock or any lower lock
smplock_wait_loop:
	movb	$-1,%al				# this is the flag value in SL_B_CPUID indicating that the lock is unowned
	lock
	cmpxchgb %ah,SL_B_CPUID(%ecx)		# store my cpu index in there iff it was unowned
	jne	smplock_wait_clash		# repeat if lock was owned (presumably by someone else)
	popl	%eax				# return previous smp level
	popl	%esi
	leave
	ret

# CPU is already at current level, just return with current level

smplock_wait_same:
	cmpb	SL_B_CPUID(%ecx),%ah		# level staying the same, i better already own this lock
	jne	smplock_wait_crash3		# (if not, might have two locks at the same level)
	popfl					# restore hardware interrupt delivery
	popl	%eax				# return smp level
	popl	%esi
	leave
	ret

# Clashed, increment statistics counter and try again

smplock_wait_clash:
	lock
	incl	oz_hw486_smplev_clash+0(,%esi,8)
	jne	smplock_wait_loop
	lock
	incl	oz_hw486_smplev_clash+4(,%esi,8)
	jmp	smplock_wait_loop

# Crashes

smplock_wait_crash1:
	pushl	$0
	movb	%dl,(%esp)
	pushl	$0
	movb	%dh,(%esp)
	pushl	$smplock_wait_msg1
	call	oz_crash

smplock_wait_crash2:
	pushl	$0
	movb	%dl,(%esp)
	pushl	$0
	movb	%dh,(%esp)
	pushl	%ecx
	pushl	$smplock_wait_msg2
	call	oz_crash

smplock_wait_crash3:
	pushl	%ecx
	pushl	$0
	movb	%dh,(%esp)
	pushl	$smplock_wait_msg3
	call	oz_crash

smplock_wait_msg1:	.string	"oz_hw_smplock_wait: new level %x .lt. current level %x"
smplock_wait_msg2:	.string	"oz_hw_smplock_wait: already own lock %p at level %x, but at level %x"
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
##	sw ints enabled if now at level 0				##
##									##
##    Note:								##
##									##
##	This routine does not touch the sti/cli interrupt enable bit 	##
##	in the processor.  So it may be called with either state of 	##
##	the bit.							##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_smplock_clr
	.type	oz_hw_smplock_clr,@function
oz_hw_smplock_clr:
	pushl	%ebp				# make a call frame
	movl	%esp,%ebp
	pushl	%edi				# save scratch registers
	pushl	%esi
	pushl	%ebx
	call	oz_hw486_getcpu			# get my cpu data
	movl	 8(%ebp),%edi			# point to lock block
	movzbl	12(%ebp),%edx			# get level we're restoring to

#  %eax = my cpu index
#  %edx = level we're returning to
#  %esi = cpu data
#  %edi = lock block being released

	cmpb	SL_B_CPUID(%edi),%al		# we better own the lock being released
	jne	smplock_clr_crash1

	cmpb	CPU_B_SMPLEVEL(%esi),%dl	# compare new level : old level
	je	smplock_clr_rtn			# if the same, don't change a thing
	jnc	smplock_clr_crash2		# we can't lower the level to an higher value

# smplevel is being lowered

	xorl	%eax,%eax			# make sure other cpu's see writes done with lock
	cpuid					# ... before they see us release the lock

	movzbl	12(%ebp),%edx			# get level we're restoring to

	pushfl					# don't let isr see inconsistent state
	cli

	movb	$-1,SL_B_CPUID(%edi)		# mark the lock being unowned by anyone (before allowing any more interrupts)
						# (some other cpu might take it now)

	movb	%dl,CPU_B_SMPLEVEL(%esi)	# mark this cpu's smplevel as being lower now

	movl	APIC_L_TSKPRI,%eax		# get reserved bits
	movb	smplevel_to_tskpri(%edx),%al	# get (lower) tskpri corresponding to new lock level
	movl	%eax,APIC_L_TSKPRI		# set new tskpri value allowing more interrupts
	UPDATEVIDEOSTATUS 4(%ebp)

	popfl					# hardware interrupts ok now

smplock_clr_rtn:
	popl	%ebx
	popl	%esi
	popl	%edi
	leave
	ret

# Crashes

smplock_clr_crash1:
	pushl	%eax
	movzbl	SL_B_CPUID(%edi),%eax
	pushl	%eax
	pushl	%edi
	pushl	$smplock_clr_msg1
	call	oz_crash

smplock_clr_crash2:
	pushl	%edx
	movzbl	CPU_B_SMPLEVEL(%esi),%eax
	pushl	%eax
	pushl	%edi
	pushl	$smplock_clr_msg2
	call	oz_crash

smplock_clr_msg1:	.string	"oz_hw_smplock_clr: lock %p owned by %x, this cpu %x"
smplock_clr_msg2:	.string	"oz_hw_smplock_clr: lock %p lowering from %x to %x"

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

# Figure out codesegment to use = caller's codesegment

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

# Fill in the NMI vector

	leal	8*2(%edi),%eax			# point to the vector
	movl	$process_apicnmi,%edx		# get entrypoint
	movw	%dx,0(%eax)			# low order entrypoint
	movw	%cx,2(%eax)			# code segment
	shrl	$16,%edx			# get high order entrypoint in %dx
	movb	$0x8E,5(%eax)			# inhibit hardware interrups on entry
	movw	%dx,6(%eax)			# high order entrypoint

# Fill in the 'int $INT_FAKENMI' vector

	leal	8*INT_FAKENMI(%edi),%eax	# point to the vector
	movl	$process_fakenmi,%edx		# get entrypoint
	movw	%dx,0(%eax)			# low order entrypoint
	movw	%cx,2(%eax)			# code segment
	shrl	$16,%edx			# get high order entrypoint in %dx
	movb	$0xAE,5(%eax)			# make it accessible from exec mode 'int $INT_FAKENMI' calls
						# hardware interrupts inhibited on entry
	movw	%dx,6(%eax)			# high order entrypoint

# Fill in the 'int $INT_GETNOW' vector

	leal	8*INT_GETNOW(%edi),%eax		# point to the vector
	movl	$oz_hw_except_getnow,%edx	# get entrypoint
	movw	%dx,0(%eax)			# low order entrypoint
	movw	%cx,2(%eax)			# code segment
	shrl	$16,%edx			# get high order entrypoint in %dx
	movb	$0xEE,5(%eax)			# make it accessible from user mode 'int $INT_GETNOW' calls
						# hardware interrupts inhibited on entry
	movw	%dx,6(%eax)			# high order entrypoint

# Initialize INT_LOWIPL interrupt vector

	leal	8*INT_LOWIPL(%edi),%eax		# point to the vector
	movl	$proclowiplint,%edx		# get entrypoint
	movw	%dx,0(%eax)			# low order entrypoint
	movw	%cx,2(%eax)			# code segment
	shrl	$16,%edx			# get high order entrypoint in %dx
	movb	$0x8F,5(%eax)			# enable hardware interrups on entry
	movw	%dx,6(%eax)			# high order entrypoint

# Initialize INT_DIAGMODE interrupt vector

	leal	8*INT_DIAGMODE(%edi),%eax	# point to the vector
	movl	$procdiagmode,%edx		# get entrypoint
	movw	%dx,0(%eax)			# low order entrypoint
	movw	%cx,2(%eax)			# code segment
	shrl	$16,%edx			# get high order entrypoint in %dx
	movb	$0x8F,5(%eax)			# enable hardware interrups on entry
	movw	%dx,6(%eax)			# high order entrypoint

# Initialize INT_QUANTIM interrupt vector

	leal	8*INT_QUANTIM(%edi),%eax	# point to the vector
	movl	$procquantim,%edx		# get entrypoint
	movw	%dx,0(%eax)			# low order entrypoint
	movw	%cx,2(%eax)			# code segment
	shrl	$16,%edx			# get high order entrypoint in %dx
	movb	$0x8F,5(%eax)			# enable hardware interrups on entry
	movw	%dx,6(%eax)			# high order entrypoint

# Initialize INT_RESCHED interrupt vector

	leal	8*INT_RESCHED(%edi),%eax	# point to the vector
	movl	$procreschedint,%edx		# get entrypoint
	movw	%dx,0(%eax)			# low order entrypoint
	movw	%cx,2(%eax)			# code segment
	shrl	$16,%edx			# get high order entrypoint in %dx
	movb	$0x8F,5(%eax)			# enable hardware interrups on entry
	movw	%dx,6(%eax)			# high order entrypoint

# Initialize the INT_INTERVAL interrupt vector

	leal	8*INT_INTERVAL(%edi),%eax	# point to the vector
	mov	$procinterval,%edx		# get entrypoint
	movw	%dx,0(%eax)			# low order entrypoint
	movw	%cx,2(%eax)			# code segment
	shrl	$16,%edx			# get high order entrypoint in %dx
	movb	$0x8F,5(%eax)			# enable hardware interrups on entry
	movw	%dx,6(%eax)			# high order entrypoint

# Initialize the INT_WAITLOOPWAKE interrupt vector

	leal	8*INT_WAITLOOPWAKE(%edi),%eax	# point to the vector
	mov	$waitloopwake,%edx		# get entrypoint
	movw	%dx,0(%eax)			# low order entrypoint
	movw	%cx,2(%eax)			# code segment
	shrl	$16,%edx			# get high order entrypoint in %dx
	movb	$0x8E,5(%eax)			# inhibit hardware interrups on entry
	movw	%dx,6(%eax)			# high order entrypoint

# Initialize the INT_INVLPG_SMP interrupt vector

	leal	8*INT_INVLPG_SMP(%edi),%eax	# point to the vector
	movl	$proc_invlpg_smp,%edx		# get entrypoint
	movw	%dx,0(%eax)			# low order entrypoint
	movw	%cx,2(%eax)			# code segment
	shrl	$16,%edx			# get high order entrypoint in %dx
	movb	$0x8E,5(%eax)			# inhibit hardware interrups on entry
	movw	%dx,6(%eax)			# high order entrypoint

# Initialize vector E0 - we get these every now and then for no particular reason

	leal	8*0xE0(%edi),%eax		# point to the vector
	movl	$exception_E0,%edx		# get entrypoint
	movw	%dx,0(%eax)			# low order entrypoint
	movw	%cx,2(%eax)			# code segment
	shrl	$16,%edx			# get high order entrypoint in %dx
	movb	$0x8E,5(%eax)			# inhibit hardware interrups on entry
	movw	%dx,6(%eax)			# high order entrypoint

# Set up the interrupt vectors for the local apic

	xorl	%esi,%esi			# start with irq 0
idt_irq_fix_loop:
	movzbl	irq_to_intvec(%esi),%ebx	# get interrupt vector number that IOAPIC will use
	shll	$3,%ebx				# make idt entry offset
	addl	%edi,%ebx			# make idt entry pointer
	movl	%esi,%edx			# now get irq processing routine entrypoint
	shll	$2,%edx
	movw	idt_irq_fix+0(%edx),%ax		# copy in low-order entrypoint
	movw	idt_irq_fix+2(%edx),%dx		# copy in high-order entrypoint
	movw	%ax,0(%ebx)
	movw	%cx,2(%ebx)			# code segment
	movb	$0x8F,5(%ebx)			# enable hardware interrups on entry
	movw	%dx,6(%ebx)
	incl	%esi				# increment irq number
	cmpl	$16,%esi			# repeat if more to go
	jc	idt_irq_fix_loop

# Set up 18.21Hz clock interrupt on IOAPIC's IRQ 2 vector (for oz_s_quickies counter and profiling)

	leal	8*INT_IOAPIC_13(%edi),%eax	# point to the vector for IOAPIC's IRQ 2
	movl	$profile_timer,%edx		# get entrypoint
	movw	%dx,0(%eax)			# low order entrypoint
	movw	%cx,2(%eax)			# code segment
	shrl	$16,%edx			# get high order entrypoint in %dx
	movb	$0x8E,5(%eax)			# inhibit hardware interrups on entry
	movw	%dx,6(%eax)			# high order entrypoint

.if ENABLE_PROFILING
	pushl	$LOWEST_IRQ_SMPLEVEL+13		# set up smplock for profile timer
	pushl	$profile_lock
	pushl	$SL__SIZE
	call	oz_hw_smplock_init
	addl	$12,%esp
	movl	$profile_lock,oz_hw_profile_lock
.endif

# Disable the 8259's because we are going to use the IOAPIC for interrupt processing

	movb	$0xFF,%al			# OCW1 master: inhibit all interrupts
	outb	$MAST_8259_DPORT
	IODELAY
	movb	$0xFF,%al			# OCW1 slave: inhibit all interrupts
	outb	$SLAV_8259_DPORT
	IODELAY

# Reprogram the IOAPIC to route IRQ0-IRQ15 to interrupt vectors that we can mask off with local APIC's TSKPRI register
# Also set it to route the PCI interrupts (16..19) to the IRQ's set up by the BIOS
# Finally, tell it to ignore interrupts 20..23 (who knows what they're connected to)

	pushl	$0				# - where to return old pte contents
	pushl	%esp				# map the IOAPIC to temp phys page
	pushl	$IOAPIC_PHYPAGE
	call	oz_hw_phys_mappage
	addl	$8,%esp				# - leave old pte contents on stack
	movl	%eax,%edi			# save virt address in a permanent place

	movl	$2,(%edi)			# read IOAPIC ARB register
	pushl	16(%edi)
	movl	$1,(%edi)			# read IOAPIC VER register
	pushl	16(%edi)
	movl	$0,(%edi)			# read IOAPIC ID register
	pushl	16(%edi)
	pushl	$ioapic_msg1			# print them all out
	call	oz_knl_printk
	addl	$16,%esp

	xorl	%ecx,%ecx			# first IOAPIC register is for IRQ 0
	movl	$0x10,%edx			# point to IOAPIC vector register for IRQ 0
ioapic_setup_loop_isa:
	movl	%edx,(%edi)			# write lower half to disable the entry
	movl	$0x10000,16(%edi)		# ... before we write the upper half

	incl	%edx				# write the upper half first so it will be valid when we write lower half ...
	movl	%edx,(%edi)
	movl	$0xFF000000,16(%edi)		# <31:24> = logical destination = any of the 8 processors

	decl	%edx				# now that upper half is set, write the lower half ...
	movl	%edx,(%edi)
	movl	$0x0900,%eax			# <15> = 0 : edge triggered
						# <13> = 0 : active high
						# <11> = 1 : logical destination mode
						# <10:08> = 001 : deliver to lowest prio cpu
	movb	irq_to_intvec(%ecx),%al		# put interrupt vector number in <07:00>
	incl	%ecx				# increment irq number for next time through
	movl	%eax,16(%edi)
	addl	$2,%edx				# point to next IOAPIC table entry
	cmpb	$0x30,%dl			# repeat for all sixteen table entries
	jne	ioapic_setup_loop_isa

	call	oz_hw486_getpciirqs		# read PCI-INT-A..D irq assignment numbers from PIIX4 chip
	pushl	%eax
	movl	$0x30,%edx			# point to IOAPIC vector register for PCI-INT-A
ioapic_setup_loop_pci:
	movzbl	(%esp),%ecx			# get an irq number to map
	incl	%esp				# pop it off the stack
	movl	%edx,(%edi)			# write the lower half ...
	movl	$0x10000,16(%edi)		# (disable it before setting upper half)
						# assume it will be disabled
	cmpb	$16,%cl				# - indicated by a 0x80 for the irq number
	jnc	ioapic_setup_loop_pci_next	#   (so disable if it's anything out-of-range)
	incl	%edx				# write the upper half first so it will be valid ...
	movl	%edx,(%edi)
	movl	$0xFF000000,16(%edi)		# <31:24> = logical destination = any of the 8 processors
	decl	%edx				# now it's ok to write lower half ...
	movl	%edx,(%edi)
	movl	$0xA900,%eax			# <15> = 1 : level triggered
						# <13> = 1 : active low
						# <11> = 1 : logical destination mode
						# <10:08> = 001 : deliver to lowest prio cpu
	movb	irq_to_intvec(%ecx),%al		# put interrupt vector number in <07:00>
	movl	%eax,16(%edi)
ioapic_setup_loop_pci_next:
	addl	$2,%edx				# increment to next IOAPIC table entry
	cmpb	$0x38,%dl			# repeat for all four table entries
	jne	ioapic_setup_loop_pci

ioapic_ignore_loop:
	movl	%edx,(%edi)			# mask off interrupts 20..23
	addl	$2,%edx
	movl	$0x10000,16(%edi)
	cmpb	$0x40,%dl
	jne	ioapic_ignore_loop

	call	oz_hw_phys_unmappage		# unmap the IOAPIC page
	addl	$4,%esp

	movb	$0x70,%al			# tell the IMCR to route interrupts through the IOAPIC
	outb	$0x22				# (if the system doesn't have an IMCR,)
	IODELAY					# (... this stuff will be ignored)
	movb	$0x01,%al
	outb	$0x23
	IODELAY

# Initialize time-of-day

	call	oz_hw486_tod_init

	pushl	$SMPLEVEL_TM
	pushl	$smplock_tm
	pushl	$SL__SIZE
	call	oz_hw_smplock_init
	addl	$12,%esp

## Now wait a second to count the bus (apic) speed, or just wait an eighth then multiply by eight
## The apic timer is used for quantum and interval timing in an smp environment

	movl	$0x10000,APIC_L_LCLVTBL		# mask off the timer interrupt
	movl	$0xB,APIC_L_TIMDVCR		# init the divisor to 1 (ie, clock speed = bus speed / 1)
	pushl	$125000000			# we wait 125mS (125,000,000nS)
	movl	$-1,APIC_L_TIMINIT		# set up count & start timing
	call	oz_hw_stl_nanowait		# wait the 125mS via software timing loop
	movl	APIC_L_TIMCURC,%eax		# get current count
	xorl	$-1,%eax			# get how much it counted down during that 125mS
	shll	$3,%eax				# this is now much it would do in a full second
	movl	%eax,apic_second		# how many counts it gets in a second
	pushl	%eax
	pushl	$apic_msg3
	call	oz_knl_printk
	addl	$12,%esp

	movl	oz_hw486_rdtscpersec,%eax	# calculate rdtsc's per apic (cpu-to-bus ratio)
	movl	%eax,%edx			# ... shifted over 16 in case of fraction (like 3.5)
	shll	$16,%eax
	shrl	$16,%edx
	divl	apic_second
	movl	%eax,rdtsc_per_apic_16

# Test the dump_apics routine

	call	dump_apics

	ret

ioapic_msg1:	.string	"oz_hw_smproc_486: ioapic ID %X, VER %X, ARB %X\n"
apic_msg3:	.string	"oz_hw_smproc_486: local apic (bus) speed %u\n"

##########################################################################
##									##
##  This declares an handler as one of many for the IRQ level		##
##  It must not be called from an interrupt service routine		##
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
	pushl	%ebx
	movl	8(%ebp),%ebx				# get IRQ level
	cmpl	$16,%ebx
	jnc	irq_many_add_crash1

	pushfl						# inhibit ints
	cli						# so an isr won't hang with list locked
irq_many_add_lock:
	xorl	%eax,%eax				# we are waiting for it to be zero
	movl	$-1,%ecx				# then we set it to a negative number
	lock
	cmpxchgl %ecx,irq_list_lock
	jne	irq_many_add_lock			# repeat if it was not zero

	movb	irq_to_smplevel(%ebx),%al		# get smplevel for the irq
	movl	12(%ebp),%ecx				# point to the new block
	movb	$-1,IRQ_MANY_SMPLK+SL_B_CPUID(%ecx)	# its smplock is not owned by any cpu
	movb	%al,IRQ_MANY_SMPLK+SL_B_LEVEL(%ecx)	# set its level = level for the irq

	movl	irq_list_heads(,%ebx,4),%eax		# save previous top of list
	movl	%ecx,irq_list_heads(,%ebx,4)		# set new top of list
	movl	%eax,IRQ_MANY_NEXT(%ecx)		# set link to previous top
	xorl	%eax,%eax				# serialize updates before releasing lock
	cpuid
	movl	$0,irq_list_lock			# release lock
	popfl						# and we can allow interrupts now
	movl	12(%ebp),%eax				# point to the struct again
	addl	$IRQ_MANY_SMPLK,%eax			# return pointer to smplock therein
	popl	%ebx					# restore scratch registers
	leave						# all done
	ret

irq_many_add_crash1:
	pushl	%ebx
	pushl	$irq_many_add_msg1
	call	oz_crash

irq_many_add_msg1: .string "oz_hw486_irq_many_add: irq %u out of range"

##########################################################################
##									##
##  Remove an handler from an IRQ level					##
##  It must not be called from an interrupt service routine		##
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
	pushl	%ebx					# save scratch registers
	movl	8(%ebp),%ebx				# get IRQ level
	cmpl	$16,%ebx
	jnc	irq_many_rem_crash1

	pushfl						# inhibit ints
	cli						# so an isr won't hang with list locked
irq_many_rem_lock:
	xorl	%eax,%eax				# we are waiting for it to be zero
	movl	$-1,%ecx				# then we set it to a negative number
	lock
	cmpxchgl %ecx,irq_list_lock
	jne	irq_many_rem_lock			# repeat if it was not zero

	leal	irq_list_heads(,%ebx,4),%edx		# point to listhead
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
	xorl	%eax,%eax				# serialize updates before unlocking
	cpuid
	movl	$0,irq_list_lock			# release irq list
	popfl						# restore hw ints now that we're unlocked
	popl	%ebx					# restore scratch registers
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
#									#
#########################################################################

	.align	4
irq_1_entry:
	pushl	%ebp
	movl	 $1,%ebp
	jmp	irq_entry

	.align	4
irq_2_entry:			# not really used
	pushl	%ebp		# 18.21Hz timer goes to 'profile_timer'
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

	.align	4
irq_0_entry:			# not really used
	pushl	%ebp		# 18.21Hz timer goes to IRQ 2
	xorl	%ebp,%ebp

	# %ebp     = irq number, 0..15
	# hardints = enabled, but local APIC is blocking this level and any lower levels until we do an EOI

	.align	4
irq_entry:

# Finish making the mchargs on the stack

	pushal						# push registers (irq number ends up in MCH_L_EC1)
	movl	%ebp,%ebx				# get the irq number

	call	oz_hw486_random_int			# increment random seed counter

	call	oz_hw486_getcpu				# point to cpu's data block
	incl	oz_hw486_irq_counts(,%eax,4)		# count the irq's on this cpu
	bts	%ebx,oz_hw486_irq_masks(,%eax,4)	# remember which irq's we got on this cpu
	lock						# set bit in general 'got this irq' mask
	bts	%ebx,oz_hw486_irq_hits_mask

	movl	APIC_L_TSKPRI,%edx			# stack current TSKPRI and SMPLEVEL as the EC2
	movb	CPU_B_SMPLEVEL(%esi),%dh
	pushl	%edx
	movl	%eax,%edi				# save cpu id number

	call	oz_hw_fixkmexceptebp			# fix up the saved ebp
	call	oz_hw_fixkmmchargesp			# fix up the saved esp

# Block changes to irq handler lists

irq_entry_list_loop1:
	movl	irq_list_lock,%eax			# get current lock value
irq_entry_list_loop2:
	testl	%eax,%eax				# see if top bit <31> is set meaning list is being modified
	js	irq_entry_list_loop1			# if so, wait for it to clear
	leal	1(%eax),%ecx				# if not, prepare to increment it to block modifications
	lock						# attempt to increment
	cmpxchgl %ecx,irq_list_lock
	jne	irq_entry_list_loop2			# repeat if it changed since we started

# Raise the cpu's smplevel to that of the irq

							# not having done the APIC_L_EOI yet should keep the APIC 
							# from delivering this level again and any lower priority 
							# interrupts

	movzbl	CPU_B_SMPLEVEL(%esi),%edx		# get cpu's smplevel
	movzbl	irq_to_smplevel(%ebx),%ecx		# get irq's smplevel
	cmpl	%ecx,%edx				# make sure it should have allowed us in
	jnc	irq_entry_crash1			# ie, new level .gt. old level, else crash
	movzbl	smplevel_to_tskpri(%ecx),%eax
	movb	%cl,CPU_B_SMPLEVEL(%esi)		# ok, set up new smp level in the cpu
							#     so it will match the irq handler smplocks
	movl	%eax,APIC_L_TSKPRI			# set new TSKPRI to block this and lower interrupt levels
	movl	$0,APIC_L_EOI				# clear this interrupt out of LAPIC so it can queue another

# Call the interrupt service routines for the level

	pushl	%esi					# save cpu database pointer
	movl	irq_list_heads(,%ebx,4),%esi		# point to top routine for the level
	testl	%esi,%esi				# see if there are any handlers defined for this level at all
	je	irq_entry_call_done			# if not, ignore it
irq_entry_call_loop:
	movl	%edi,%ecx				# get my cpu number
	leal	4(%esp),%edx				# point to mchargs
	cmpb	%cl,IRQ_MANY_SMPLK+SL_B_CPUID(%esi)	# this cpu can't alreay own the lock
	movl	IRQ_MANY_ENTRY(%esi),%ebx		# get interrupt routine entrypoint
	je	irq_entry_crash2
irq_entry_call_lock:
	movb	$-1,%al					# set the handler's smplock
	lock
	cmpxchgb %cl,IRQ_MANY_SMPLK+SL_B_CPUID(%esi)
	jne	irq_entry_call_locked
	pushl	%edx					# push mchargs pointer
	pushl	IRQ_MANY_PARAM(%esi)			# push interrupt routine parameter
	call	*%ebx					# call interrupt routine
	xorl	%eax,%eax				# serialize updates before releasing smplock
	cpuid
	movb	$-1,IRQ_MANY_SMPLK+SL_B_CPUID(%esi)	# release the handler's smplock
	movl	IRQ_MANY_NEXT(%esi),%esi		# get pointer to next in list
	addl	$8,%esp					# pop parameter & mchargs pointer
	testl	%esi,%esi				# see if ther are more handlers
	jne	irq_entry_call_loop			# repeat if more handlers
irq_entry_call_done:

	lock						# unblock irq handler list
	decl	irq_list_lock

	popl	%esi					# get cpu database pointer
	popl	%edx					# get the TSKPRI / cpu's old smplevel (from EC2)
	cli						# don't let nested int see mixed up TSKPRI/cpu smplevel
	movb	%dh,CPU_B_SMPLEVEL(%esi)		# restore cpu's smplevel
	xorb	%dh,%dh
	movl	%edx,APIC_L_TSKPRI			# restore task priority
	popal						# restore general registers
	popl	%ebp					# restore ebp
	iret

irq_entry_call_locked:
	movzbl	IRQ_MANY_SMPLK+SL_B_LEVEL(%esi),%eax	# increment clash counter
	lock
	incl	oz_hw486_smplev_clash+0(,%eax,8)
	jne	irq_entry_call_lock
	lock
	incl	oz_hw486_smplev_clash+4(,%eax,8)
	jmp	irq_entry_call_lock			# ... then try again

# Crashes

irq_entry_crash1:
	pushl	APIC_L_TSKPRI
	pushl	%ecx
	pushl	%ebx
	pushl	%edx
	pushl	%edi
	pushl	$irq_entry_msg1
	call	oz_crash

irq_entry_crash2:
	pushl	APIC_L_TSKPRI
	pushl	%ecx
	pushl	%ebx
	pushl	$irq_entry_msg2
	call	oz_crash

irq_entry_msg1:	.string	"oz_hw_smproc_486 irq_entry: cpu %u at level %x at or above irq %u lock level %x, tskpri %x"
irq_entry_msg2:	.string	"oz_hw_smproc_486 irq_entry: irq %u lock already owned by cpu %u, tskpri %x"

##########################################################################
##									##
##  Process an quantum timer software interrupt				##
##									##
##  This routine gets called as a result of the APIC interrupting from 	##
##  an INT_QUANTIM interrupt.  It should only honour this interrupt 	##
##  when software interrupt delivery is enabled.			##
##									##
##  This interrupt routine should be set up so it only triggers when 	##
##  soft interrupts are enabled.  This is accomplished by making its 	##
##  vector in the same group of 16 as the softint vector.		##
##									##
##########################################################################

	.align	4
procquantim:
	call	softintsetup
	pushl	%eax			# put cpu index as call arg to routine
	call	oz_knl_thread_quantimex	# call the routine
	addl	$4,%esp
	jmp	softintiret

##########################################################################
##									##
##  Process an lowipl software interrupt, call oz_knl_lowipl_handleint	##
##									##
##  This routine gets called as a result of the APIC interrupting from 	##
##  an INT_LOWIPL interrupt.  It should only honour this interrupt 	##
##  when software interrupt delivery is enabled.			##
##									##
##  This interrupt routine should be set up so it only triggers when 	##
##  soft interrupts are enabled.  This is accomplished by making its 	##
##  vector in the same group of 16 as the softint vector.		##
##									##
##########################################################################

	.align	4
proclowiplint:
	call	softintsetup
	call	oz_knl_lowipl_handleint
	jmp	softintiret

##########################################################################
##
##  General softint interrupt handler setup routine
##
##    Input:
##
##	Called with hardware interrupts enabled
##	APIC EOI stuff has further softint delivery inhibited
##	CPU_B_SMPLEVEL is still zero
##	return address is where MCH_L_EBP goes
##
##    Output:
##
##	%eax = my cpu index number
##	%esi = my cpudb struct pointer
##	Mchargs pushed on stack
##	APIC TSKPRI set to inhibit softint delivery, but EOI has been given
##	CPU_B_SMPLEVEL now set to one to indicate softint delivery inhibited
##

	.align	4
softintsetup:
	pushal					# save registers
	pushl	$0				# dummy ec2
	movl	$0,MCH_L_EC1(%esp)		# dummy ec1
	movl	MCH_L_EBP(%esp),%ebx		# get the return address
	movl	%ebp,MCH_L_EBP(%esp)		# save frame pointer
	call	oz_hw_fixkmexceptebp		# point to stack frame

	call	oz_hw486_getcpu			# make sure softints were enabled
	movzbl	CPU_B_SMPLEVEL(%esi),%ecx
	testl	%ecx,%ecx
	jne	softintsetup_crash1
	movb	$1,CPU_B_SMPLEVEL(%esi)		# say softints aren't enabled anymore
	movl	$INT_LOWIPL,APIC_L_TSKPRI	# inhibit further softint deliveries
	UPDATEVIDEOSTATUS %ebx
	movl	$0,APIC_L_EOI			# clear this interrupt from the apic so it can queue more
	jmp	*%ebx				# return to caller

# Softints were inhibited

softintsetup_crash1:
	pushl	%ebx
	pushl	APIC_L_TSKPRI
	pushl	%ecx
	pushl	$softintsetup_msg1
	call	oz_crash

softintsetup_msg1:	.string	"oz_hw_smproc_486 softintsetup: called at smplevel %X, TSKPRI %X, from %X"

##########################################################################
##									##
##  Process an resched software interrupt, call oz_knl_thread_handleint	##
##									##
##  This routine gets called as a result of the APIC interrupting from 	##
##  an INT_RESCHED interrupt.  It should only honour this interrupt 	##
##  when software interrupt delivery is enabled.			##
##									##
##########################################################################

	.align	4
procreschedint:
	call	softintsetup
	call	oz_knl_thread_handleint

##########################################################################
##
##  Return from an softint-handling routine
##  At this point, hardware interrupts are enabled and/but software interrupts are inhibited
##

softintiret:
	cli						# inhibit hw interrupts so cpu doesn't switch on us
							# this also prevents softints from nesting on us 
							# ... until after the iret executes
	call	oz_hw486_getcpu				# (maybe we're on a different cpu now)
	movb	$0,CPU_B_SMPLEVEL(%esi)			# enable softint delivery
	movl	$0,APIC_L_TSKPRI
	UPDATEVIDEOSTATUS $softintiret

# About to do an iret with mchargs on stack.  Check for pending ast's before returning.  This 
# routine assumes the eflags on the stack indicate hardint delivery is to be enabled on return.

	pushl	%esp					# push mchargs pointer
	pushl	%eax					# push cpu index number
	call	oz_knl_thread_checkknlastq		# see if there are any deliverable kernel ast's
	addl	$8,%esp					# pop cpu index and mchargs pointer
	testb	$2,MCH_W_CS(%esp)			# see if returning to user mode
	je	softintiret_rtn				# if not, just return (kernel ast's are handled by oz_knl_thread_handleint)
	pushl	oz_PROCMODE_USR				# we are about to return to user mode
	call	oz_knl_thread_chkpendast		# ... see if there will be any deliverable ast's for that mode
	addl	$4,%esp
	testl	%eax,%eax
	je	softintiret_rtn				# if not, we can just return to user mode as is
	sti						# deliverable usermode ast's, enable hw ints so we can access user stack
							# - if other ast's get queued to us, well we're about to do them anyway
							# - if the ast's get cancelled or delivered on us, 
							#   oz_sys_thread_checkast will just have nothing to do
	xorl	%eax,%eax				# no additional longs to copy to user stack, just copy the mchargs
	call	oz_hw486_movemchargstocallerstack
	cmpl	oz_SUCCESS,%eax				# hopefully it copied ok
	jne	softintiret_baduserstack
	pushl	%esp					# we're in user mode now, push pointer to mchargs
	call	oz_sys_thread_checkast			# process any user mode ast's that may be queued
	addl	$4,%esp					# (the iret below will be from user mode to user mode)
softintiret_rtn:
	addl	$4,%esp					# wipe the dummy ec2 from stack
	popal						# restore general registers
	popl	%ebp					# restore ebp
	iret

# Unable to copy mchargs to user stack in preparation for delivering an ast
# Print an error message on console then exit the thread

softintiret_baduserstack:
	pushl	%eax
	pushl	MCH_L_XSP+4(%esp)
	pushl	%eax
	pushl	$softintiret_msg1
	call	oz_knl_printk
	addl	$12,%esp
softintiret_baduserexit:
	call	oz_knl_thread_exit
	jmp	softintiret_baduserexit

softintiret_msg1:	.string	"oz_hw_smproc_486: error %u pushing to user stack pointer %p\n"

#
#  We seem to get these from time to time
#
	.align	4
exception_E0:
	pushl	%ebp			# build mchargs on stack
	pushal
	pushl	$0			# ... including dummy ec2

	pushl	MCH_L_EIP(%esp)		# print a message
	pushl	$exception_E0_msg1
	call	oz_knl_printk
	addl	$8,%esp

		####	call	dump_apics # dump the local apic and ioapic

	popl	APIC_L_EOI		# pop dummy ec2 / ack the interrupt
	popal
	popl	%ebp
	iret

exception_E0_msg1:	.string	"oz_hw_smproc_486: exception E0 at %p\n"

# Dump out the contents of the local APIC and the IOAPIC
#  Scratch: %eax,%ebx,%ecx,%edx,%edi

	.align	4
dump_apics:
	pushl	%ebp
	movl	%esp,%ebp

	pushl	$0				# - where to return old pte contents
	pushl	%esp				# map the IOAPIC to temp phys page
	pushl	$IOAPIC_PHYPAGE
	call	oz_hw_phys_mappage
	addl	$8,%esp				# - leave old pte contents on stack
	movl	%eax,%edi			# save virt address in a permanent place

	movl	$0x40,%ecx			# push IOAPIC's vector table registers on stack
dump_ioapic_table_loop:
	decl	%ecx
	movl	%ecx,(%edi)
	movl	16(%edi),%edx
	andl	$0xFF000000,%edx		# - whittle the 64 bits down to 32
	decl	%ecx
	movl	%ecx,(%edi)
	movl	16(%edi),%eax
	andl	$0x00FFFFFF,%eax		# - as most of the bits are unused
	orl	%edx,%eax
	pushl	%eax
	cmpb	$0x10,%cl
	jne	dump_ioapic_table_loop
	pushl	$0				# a dummy zero to fill the line
	movl	$2,(%edi)			# push the whole control registers
	pushl	16(%edi)
	movl	$1,(%edi)
	pushl	16(%edi)
	movl	$0,(%edi)
	pushl	16(%edi)

	movl	$dump_local_apic_table_offsets,%edi	# get local apic pushed on stack quickly
dump_apics_loop_offsets:
	movl	(%edi),%ecx
	addl	$4,%edi
	jecxz	dump_apics_done_offsets
	pushl	(%ecx)
	jmp	dump_apics_loop_offsets
dump_apics_done_offsets:
	xorl	%ebx,%ebx				# reset column counter
dump_apics_loop_names:
	pushl	%edi					# push address of 8-byte name string
	pushl	8(%edi)					# push pointer to format string
	call	oz_knl_printk				# print the register(s)
	movl	12(%edi),%eax				# see how many values it output
	addl	%eax,%ebx				# add that to our 'column counter'
	addl	$8,%esp					# wipe format string and name string pointer
	shll	$2,%eax					# wipe values from stack
	addl	%eax,%esp
	testl	$3,%ebx					# see if at end of line
	jne	dump_apics_next_names
	pushl	$dump_apics_newline			# if so, print a newline
	call	oz_knl_printk
	addl	$4,%esp
dump_apics_next_names:
	addl	$16,%edi				# point to next table entry
	cmpl	$0,(%edi)				# see if we're at the end
	jne	dump_apics_loop_names
	testl	$3,%ebx					# see if we need a final newline
	je	dump_apics_rtn
	pushl	$dump_apics_newline			# ok, print it
	call	oz_knl_printk
	addl	$4,%esp
dump_apics_rtn:
	call	oz_hw_phys_unmappage			# restore temp spte
	leave
	ret

dump_ioapic_table_offsets:
	.long	
dump_local_apic_table_offsets:
	.long	APIC_L_LCLID   
	.long	APIC_L_LCLVER  
	.long	APIC_L_TSKPRI  
	.long	APIC_L_ARBPRI  
	.long	APIC_L_PROCPRI 
	.long	APIC_L_EOI     
	.long	APIC_L_LOGDST  
	.long	APIC_L_DSTFMT  
	.long	APIC_L_ISR0+0x70,APIC_L_ISR0+0x60,APIC_L_ISR0+0x50,APIC_L_ISR0+0x40
	.long	APIC_L_ISR0+0x30,APIC_L_ISR0+0x20,APIC_L_ISR0+0x10,APIC_L_ISR0+0x00
	.long	APIC_L_TMR0+0x70,APIC_L_TMR0+0x60,APIC_L_TMR0+0x50,APIC_L_TMR0+0x40
	.long	APIC_L_TMR0+0x30,APIC_L_TMR0+0x20,APIC_L_TMR0+0x10,APIC_L_TMR0+0x00
	.long	APIC_L_IRR0+0x70,APIC_L_IRR0+0x60,APIC_L_IRR0+0x50,APIC_L_IRR0+0x40
	.long	APIC_L_IRR0+0x30,APIC_L_IRR0+0x20,APIC_L_IRR0+0x10,APIC_L_IRR0+0x00
	.long	APIC_L_SPURINT 
	.long	APIC_L_ERRSTAT 
	.long	APIC_L_ICREG0  
	.long	APIC_L_ICREG1  
	.long	APIC_L_LCLVTBL 
	.long	APIC_L_PERFCNT 
	.long	APIC_L_LINT0   
	.long	APIC_L_LINT1   
	.long	APIC_L_LERROR  
	.long	APIC_L_TIMINIT 
	.long	APIC_L_TIMCURC 
	.long	APIC_L_TIMDVCR 
	.long	0

	.string	"TIMDVCR"
	.long	dump_apics_message_1,1
	.string	"TIMCURC"
	.long	dump_apics_message_1,1
	.string	"TIMINIT"
	.long	dump_apics_message_1,1
	.string	"LERROR "
	.long	dump_apics_message_1,1

	.string	"LINT1  "
	.long	dump_apics_message_1,1
	.string	"LINT0  "
	.long	dump_apics_message_1,1
	.string	"PERFCNT"
	.long	dump_apics_message_1,1
	.string	"LCLVTBL"
	.long	dump_apics_message_1,1

	.string	"ICREG1 "
	.long	dump_apics_message_1,1
	.string	"ICREG0 "
	.long	dump_apics_message_1,1
	.string	"ERRSTAT"
	.long	dump_apics_message_1,1
	.string	"SPURINT"
	.long	dump_apics_message_1,1

	.string	"IRR"
	.long	0,dump_apics_message_8,8
	.string	"TMR"
	.long	0,dump_apics_message_8,8
	.string	"ISR"
	.long	0,dump_apics_message_8,8

	.string	"DSTFMT "
	.long	dump_apics_message_1,1
	.string	"LOGDST "
	.long	dump_apics_message_1,1
	.string	"EOI    "
	.long	dump_apics_message_1,1
	.string	"PROCPRI"
	.long	dump_apics_message_1,1

	.string	"ARBPRI "
	.long	dump_apics_message_1,1
	.string	"TSKPRI "
	.long	dump_apics_message_1,1
	.string	"LCLVER "
	.long	dump_apics_message_1,1
	.string	"LCLID  "
	.long	dump_apics_message_1,1

	.string	"IOAPID "
	.long	dump_apics_message_1,1
	.string	"IOAPVR "
	.long	dump_apics_message_1,1
	.string	"IOAPARB"
	.long	dump_apics_message_1,1
	.string	"       "
	.long	dump_apics_message_0,1

	.string	"IV0"
	.long	0,dump_apics_message_8,8
	.string	"IV8"
	.long	0,dump_apics_message_8,8
	.string	"IVx"
	.long	0,dump_apics_message_8,8

	.long	0,0,0,0

dump_apics_message_1:	.string	"  %s=%8.8X"
dump_apics_message_8:	.string	"  %s=%8.8X %8.8X %8.8X %8.8X %8.8X %8.8X %8.8X %8.8X"
dump_apics_message_0:	.string	""
dump_apics_newline:	.string	"\n"

##########################################################################
##									##
##  Interrupt handler runs at 18.21Hz to increment profile counters	##
##									##
##    Input:								##
##									##
##	direct irq 2 interrupt, hw interrupt delivery inhibited		##
##									##
##########################################################################

	.align	4
profile_timer:
.if ENABLE_PROFILING
	pushl	%ebp			# push mchargs on stack
	pushal
	pushl	$0
	call	oz_hw_fixkmexceptebp
	call	oz_hw_fixkmmchargesp
	pushl	$profile_lock		# set profiling data lock
	call	oz_hw_smplock_wait
	pushl	%eax
	leal	8(%esp),%edx		# get pointer to mchargs
	call	oz_hw_profile_tick	# increment profile counters
	incl	oz_s_quickies		# increment the quickies counter
	pushl	$profile_lock		# release profiling lock
	call	oz_hw_smplock_clr
	movl	$0,APIC_L_EOI		# ack the interrupt out of the APIC
	addl	$16,%esp		# pop call params and mchargs from stack
	popal
	popl	%ebp
.else
	incl	oz_s_quickies		# increment the quickies counter
	movl	$0,APIC_L_EOI		# ack the interrupt out of the APIC
.endif
	iret				# all done

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
	call	oz_hw486_getcpu
	movzbl	CPU_B_SMPLEVEL(%esi),%ecx
	pushl	8(%ebp)
	pushl	APIC_L_TSKPRI
	pushl	%ecx
	pushl	%eax
	call	oz_dev_video_statusupdate
	addl	$16,%esp
	popfl
	popl	%eax
	popl	%ecx
	popl	%edx
	popl	%esi
	leave
	ret	$4
