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
##  This is the main program for the kernel				##
##									##
##  It is loaded into memory at 0x00100000 by the loader		##
##  The processor is in 32-bit mode with paging enabled			##
##									##
##    The call parameters are:						##
##									##
##	 0(%esp) = address of next free spt entry			##
##	 4(%esp) = virt address mapped by next free spt entry		##
##	 8(%esp) = loadparams block pointer				##
##	12(%esp) = number of physical pages in this computer		##
##	16(%esp) = executive stack size (from param block)		##
##	20(%esp) = system pages (from param block)			##
##									##
##    Current memory layout:						##
##									##
##	Page 0 is a no-access page					##
##									##
##	There is an IDT at 7E00 with pushb/jmp's that call a default 	##
##	exception handler in the loader.				##
##									##
##	The 24-byte GDT is at 7DE8, and the executive stack is 		##
##	currently just below that.					##
##									##
##	The master page directory is at 2000 and the system page 	##
##	tables directly follow it starting at 3000.			##
##									##
##	The load params block is at 8E00.				##
##									##
##	All the memory at 9000 and above was the loader and can be 	##
##	re-used for non-paged pool or whatever desired.			##
##									##
##									##
##	|				| <- 4(%esp)			##
##	|   ^  ^  ^  ^  ^  ^  ^  ^  ^	|				##
##	|				|				##
##	|  kernel			| <- 00100000			##
##	+-------------------------------+				##
##	|				|				##
##	|				|				##
##	|  memory hole			| <- 000A0000			##
##	+-------------------------------+				##
##	|				|				##
##									##
##	|   ^  ^  ^  ^  ^  ^  ^  ^  ^	|				##
##	|				|				##
##	|  loader			| <- 00009000			##
##	+-------------------------------+				##
##	|  load params block		| <- 00008E00 <- 8(%esp)	##
##	+-------------------------------+				##
##	|  initial IDT + pushb/jmp's	| <- 00007E00			##
##	+-------------------------------+				##
##	|  initial GDT			| <- 00007DE8			##
##	+-------------------------------+				##
##	|  kernel stack			|				##
##	|				|				##
##	|   .  .  .  .  .  .  .  .  .	| <- %esp			##
##	|				| 				##
##									##
##	|				| <- 0(%esp)			##
##	|   ^  ^  ^  ^  ^  ^  ^  ^  ^	|				##
##	|				|				##
##	|  SPT				| <- 00003000 = SPTBASE		##
##	+-------------------------------+				##
##	|  MPD				| <- 00002000 = MPDBASE		##
##	+-------------------------------+				##
##	|  ALT cpu startup		| <- 00001000 = ALTCPUBASE	##
##	+-------------------------------+				##
##	|  no access			| <- 00000000			##
##	+-------------------------------+				##
##									##
##########################################################################

	.include "oz_params_486.s"

	PARANOID_MPD = 0		# 0: use MPD in per-process address space
					# 1: use MPD in the statically mapped area for debugging
					#    MPD's are located just below memory hole and never move

	SYSENTER_CS_MSR  = 0x174
	SYSENTER_ESP_MSR = 0x175
	SYSENTER_EIP_MSR = 0x176
.macro	sysenter
	.byte	0x0F,0x34
	.endm	sysenter
.macro	sysexit
	.byte	0x0F,0x35
	.endm	sysexit

	.data
	.align	4096

	.globl	oz_s_systempdata	# system process' OZ_Pdata struct
	.type	oz_s_systempdata,@object
oz_s_systempdata:	.space	4096	# - mapped here always 
					# - also mapped at same address as per-process oz_sys_pdata 
					#   kernel struct when the system process is current on the cpu

	.globl	oz_hw486_activecpus
	.type	oz_hw486_activecpus,@object
oz_hw486_activecpus:	.long	0	# bitmask of active cpus

## The following stuff is linked in the read-only area of the kernel
## When we first start up, we write-enable the pages that contain this stuff
## Then, before enabling interrupts, we write-protect it as it should never change
## The area is delineated by the prot_start and prot_end symbols

	.text
	.align	4096
prot_start:

syspdataptpage:		.space	4096	# pagetable page that maps oz_s_systempdata page 
					# to per-process oz_sys_pdata[kernel] address

			.globl	oz_hw486_estacksize
			.type	oz_hw486_estacksize,@object
oz_hw486_estacksize:	.long	0	# number of bytes to allocate for executive stacks

static_spt_end:		.long	0	# pointer to end of static spt
spt_end_pointer:	.long	0	# pointer to end of spt
spt_end_virtaddr:	.long	0	# address mapped by end of spt
			.globl	oz_hw486_dynphypagbeg
			.type	oz_hw486_dynphypagbeg,@object
oz_hw486_dynphypagbeg:	.long	0	# page number of first dynamic physical page
					# - all pages below this cannot be mapped to any pte's except their primary static ones
srp_table:		.long	0	# address of system reqprot table

cpu_sptentry:		.long	0	# address of spt entry for cpu 0
cpu_sptvaddr:		.long	0	# corresponding virtual address

			.globl	oz_hw486_sysentsysex
			.type	oz_hw486_sysentsysex,@object
oz_hw486_sysentsysex:	.long	0	# gets set to 1 if cpu's support sysenter/sysexit

			.globl	oz_hw486_hiestgvadp1
			.type	oz_hw486_hiestgvadp1,@object
oz_hw486_hiestgvadp1:	.long	0		# highest global page virtual address, plus 1
						# (or perhaps better, lowest non-global virtual address)
cr4_gblbit:		.long	0		# set to 0x80 if cpu supports global pages, else value is zeroes
cr4_osfxsr_bit:		.long	0		# set to 0x200 if cpu supports fxsave/fxrstor
pte_gbl:		.long	0		# pte bits for GLOBAL
pte_validw_gbl_krw:	.long	PT_KRW		# pte bits for OZ_SECTION_PAGESTATE_VALID_W, GLOBAL, KERNEL READ/WRITE
pte_validw_gbl_krw_nc:	.long	PT_KRW|PT_NC	# pte bits for OZ_SECTION_PAGESTATE_VALID_W, GLOBAL, KERNEL READ/WRITE, NO CACHE
pte_validw_gbl_na:	.long	0		# pte bits for OZ_SECTION_PAGESTATE_VALID_W, GLOBAL, NO ACCESS

## This is the 'permanent' IDT

		.align	8
		.globl	oz_hw486_kernel_idt
		.globl	oz_hw486_kernel_idtr
		.type	oz_hw486_kernel_idt,@object
		.type	oz_hw486_kernel_idtr,@object
oz_hw486_kernel_idt:	.space	256*16			# *16 correct, see oz_hw_idt_init

oz_hw486_kernel_idtr:	.word	(8*256)-1
			.long	oz_hw486_kernel_idt

## This is the 'permanent' GDT.  We do not have any LDT's.

		.align	8
kernel_gdt:	.word	0,0,0,0			# gdt[0] = null (by hw definition)

		.word	-1,0,0x9F00,0x00CF	# gdt[1] = conforming code descriptor
						# - used by the exception handlers like "divide by zero" 
						#   to remain on the original stack

		.word	-1,0,0x9B00,0x00CF	# gdt[2] = kernel code descriptor
						# - used when executing code in kernel mode

		.word	-1,0,0x9300,0x00CF	# gdt[3] = kernel data descriptor
						# - used for kernel stack

		.word	-1,0,0xFB00,0x00CF	# gdt[4] = user code descriptor
						# - used when executing code in user mode

		.word	-1,0,0xF300,0x00CF	# gdt[5] = user data descriptor
						# - used by kernel, executive and user modes to access data

		.word	-1,0,0xBB00,0x00CF	# gdt[6] = executive code descriptor
						# - used when executing code in executive mode

		.word	-1,0,0xB300,0x00CF	# gdt[7] = executive data descriptor
						# - used for executive stack

kernel_gdt_tss:	.space	8*MAXCPUS		# gdt[8] = task state descriptors (one per cpu)

			.globl	oz_hw486_kernel_gdtr
			.type	oz_hw486_kernel_gdtr,@object
oz_hw486_kernel_gdtr:	.word	.-kernel_gdt-1
			.long	kernel_gdt

prot_end:

	.align	4

# Fixup table for IDT - fill in stuff for exception and 'int' instruction handlers
# Note:  For handlers that execute in executive mode with hardware interrupts enabled, 
#        they must check for usermode ast's before iretting to user mode

			#  0x00 : interrupt gate - inhibit hardware interrupts
			#  0x01 : trap gate - leave hardware interrupts as they are
			#  0x00 : user and exec cannot call with an INT instruction
			#  0x20 : executive can call with an INT instruction
			#  0x60 : users can call with an INT instruction
			#       CONCODESEG : executes in caller's mode
			#       EXECODESEG : executes in executive mode
			#                    (every EXECODESEG type handler must break stack frame chain if prev mode not executive)
			#                    (or any exception in executive mode may call a user mode handler in executive mode)
			#	KNLCODESEG : executes in kernel mode
kernel_idt_fix:	.long	 0,0x01,CONCODESEG,exception_DE				# divide error
	####	.long	 1,0x01,CONCODESEG,exception_DB				# debug - call condition handler
		.long	 1,0x00,EXECODESEG,exception_DB_exe			# debug - go to kernel debugger
		.long	 3,0x61,CONCODESEG,exception_BP				# breakpoint - call condition handler
	####	.long	 3,0x60,EXECODESEG,exception_BP_exe			# breakpoint - go to kernel debugger
		.long	 4,0x61,CONCODESEG,exception_OF				# overflow (arithmetic)
		.long	 5,0x01,CONCODESEG,exception_BR				# subscript bounds
		.long	 6,0x01,CONCODESEG,exception_UD				# undefined opcode
		.long	 7,0x00,EXECODESEG,exception_NM				# device (fpu) disabled
	####	.long	 8,0x00,KNLCODESEG,exception_DF_knl			# double fault
		.long	13,0x01,CONCODESEG,exception_GP				# general protection 
										# - fix seg regs or call condition handler
	####	.long	13,0x00,EXECODESEG,exception_GP_exe			# general protection - go to kernel debugger
		.long	14,0x00,EXECODESEG,exception_PF				# page fault
		.long	15,0x00,EXECODESEG,exception_15				# reserved
		.long	16,0x01,CONCODESEG,exception_MF				# floating-point error
		.long	17,0x01,CONCODESEG,exception_AC				# alignment check
		.long	19,0x01,CONCODESEG,exception_MF				# sse instruction error
		.long	INT_SYSCALL,0x61,EXECODESEG,except_syscall		# system call
		.long	INT_PRINTKP,0x60,EXECODESEG,except_printkp		# printkp call (temporary)
		.long	INT_LINUX,0x61,CONCODESEG,except_linux			# old linux calls still in libraries
		.long	INT_NEWUSRSTK,0x61,EXECODESEG,procnewusrstk		# there is a new user stack for this thread
		.long	INT_CHECKASTS,0x61,CONCODESEG,except_checkasts		# check for ast's
.if EXECODESEG <> KNLCODESEG
		.long	INT_INVLPG_EDX,0x20,KNLCODESEG,except_invlpg_edx	# invalidate possibly global page
		.long	INT_INVLPG_EDI,0x20,KNLCODESEG,except_invlpg_edi	# invalidate possibly global page
		.long	INT_INVLPG_EDXNG,0x20,KNLCODESEG,except_invlpg_edxng	# invalidate non-global page
		.long	INT_INVLPG_EDING,0x20,KNLCODESEG,except_invlpg_eding	# invalidate non-global page
		.long	INT_INVLPG_ALLNG,0x20,KNLCODESEG,except_invlpg_allng	# invalidate all non-global pages
		.long	INT_CLRTS,0x20,KNLCODESEG,except_clrts			# enable fpu access
		.long	INT_SETTS,0x20,KNLCODESEG,except_setts			# disable fpu access
		.long	INT_MOVL_CR0_EAX,0x60,KNLCODESEG,except_movl_cr0_eax	# get misc ctl bits (callable from user mode)
		.long	INT_MOVL_CR2_EAX,0x20,KNLCODESEG,except_movl_cr2_eax	# get pagefault address
		.long	INT_MOVL_CR3_EAX,0x20,KNLCODESEG,except_movl_cr3_eax	# get mpd phys address
		.long	INT_MOVL_CR4_EAX,0x20,KNLCODESEG,except_movl_cr4_eax
		.long	INT_MOVL_EAX_CR0,0x20,KNLCODESEG,except_movl_eax_cr0
.if !PARANOID_MPD
		.long	INT_MOVL_EAX_CR3,0x20,KNLCODESEG,except_movl_eax_cr3	# set mpd phys address
.endif
		.long	INT_MOVL_EAX_CR4,0x20,KNLCODESEG,except_movl_eax_cr4
		.long	INT_WBINVLD,0x20,KNLCODESEG,except_wbinvld		# flush write cache
.endif
		.long	INT_HALT,0x20,KNLCODESEG,except_halt			# halt the cpu permanently
.if !PARANOID_MPD
		.long	INT_REBOOT,0x20,KNLCODESEG,except_reboot		# reboot the cpu
.endif
		.long	-1

##  Init messages

init_msg:		.ascii	"oz_kernel_486: starting\n"
			init_msglen = .-init_msg
init_msg1:		.ascii	"oz_kernel_486: exec stacksize %u, totalpages %u\n"
			.string	"oz_kernel_486: kernel end vaddr %x, kernel spt end %x\n"
init_msg2:		.string	"oz_kernel_486: srp table end vaddr %x, spt end %x\n"
init_msg3:		.string	"oz_kernel_486: static sys pagetable end %x, static sys virtaddr end %x\n"
kexp_msg1:		.string	"oz_kernel_486: expanding kernel from %u pages to %u\n"
dont_have_min_syspages: .string "oz_kernel_486: only have room for %d system pages, need at least %d"
init_spt_toobig:	.string	"oz_kernel_486: initial spt ending at %p is too big"
pagedoutnonzero:	.string	"oz_kernel_486: OZ_SECTION_PAGESTATE_PAGEDOUT is nonzero"
nosysenter_sysexit_msg:	.string	"oz_kernel_486: cpu's don't support sysenter/sysexit (eax=%3.3X edx=%8.8X)\n"
sysenter_sysexit_msg:	.string	"oz_kernel_486: cpu's support sysenter/sysexit (eax=%3.3X edx=%8.8X)\n"

# Used to patch over fxsave with fnsave and fxrstor with frstor
# Code assumes the are 4 bytes each

fnsave_mmx:	fxsave	THCTX_X_FPSAVE+16(%eax)
frstor_mmx:	fxrstor	THCTX_X_FPSAVE+16(%eax)

# Used to decode memory cache descriptor bytes from CPUID instruction
#  .byte  bytefromCPUID,numofpagesperL1cachearray,numofpagesperL2cachearray

cpumemcachetable:
	.byte	0x06,1,0	# 1st-level instr cache: 8k bytes, 4-way set assoc, 32 byte line
	.byte	0x08,1,0	# 1st-level instr cache: 16k bytes, 4-way set assoc, 32 byte line
	.byte	0x0A,1,0	# 1st-level data cache: 8k bytes, 2-way set assoc, 32 byte line
	.byte	0x0C,1,0	# 1st-level data cache: 16k bytes, 4-way set assoc, 32 byte line
	.byte	0x41,0,8	# 2nd-level cache: 128k bytes, 4-way set assoc, 32 byte line
	.byte	0x42,0,16	# 2nd-level cache: 256k bytes, 4-way set assoc, 32 byte line
	.byte	0x43,0,32	# 2nd-level cache, 512k bytes, 4-way set assoc, 32 byte line
	.byte	0x44,0,64	# 2nd-level cache, 1M bytes, 4-way set assoc, 32 byte line
	.byte	0x45,0,128	# 2nd-level cache, 2M bytes, 4-way set assoc, 32 byte line
	.byte	0x66,1,0	# 1st-level data cache: 8k bytes, 4-way set assoc, 64 byte line
	.byte	0x67,1,0	# 1st-level data cache: 16k bytes, 4-way set assoc, 64 byte line
	.byte	0x68,2,0	# 1st-level data cache: 32k bytes, 4-way set assoc, 64 byte line
	.byte	0x79,0,4	# 2nd-level cache: 128KB, 8-way set assoc, sectored, 64 byte line
	.byte	0x7A,0,8	# 2nd-level cache: 256KB, 8-way set assoc, sectored, 64 byte line
	.byte	0x7B,0,16	# 2nd-level cache: 512KB, 8-way set assoc, sectored, 64 byte line
	.byte	0x7C,0,32	# 2nd-level cache: 1MB, 8-way set assoc, sectored, 64 byte line
	.byte	0x82,0,8	# 2nd-level cache: 256KB, 8-way set assoc, 32 byte line
	.byte	0x84,0,32	# 2nd-level cache: 1MB, 8-way set assoc, 32 byte line
	.byte	0x85,0,64	# 2nd-level cache: 2MB, 8-way set assoc, 32 byte line
	.byte	0,0,0

##########################################################################
##									##
##  Initialization routine						##
##									##
##########################################################################

	.text

	.align	4
	.globl	_start
	.type	_start,@function
_start:

## Pop parameters from stack into permanent locations

	movl	%cr0,%eax		# turn off WP bit so we can write the prot_start..prot_end values
	andl	$0xFFFEFFFF,%eax
	movl	%eax,%cr0

	popl	spt_end_pointer		# this is address of next free slot in spt
	popl	spt_end_virtaddr	# this is the corresponding virtual address

	popl	%esi			# point to current location of the parameter block
	leal	oz_s_loadparams,%edi	# point to where we want it to go
	movl	oz_LDR_PARAMS_SIZ,%ecx	# this is its size in bytes
	cld				# copy it
	rep
	movsb

	popl	oz_s_phymem_totalpages	# save total number of physical memory pages this computer has
	popl	%eax			# save desired executive stack size
	cmpl	$KSTACKMIN,%eax
	jnc	kstackbigenuf
	movl	$KSTACKMIN,%eax
kstackbigenuf:
	addl	$4095,%eax		# make sure it is an exact multiple of pagesize
	andl	$-4096,%eax		# ... so page-length allocation can be done
	movl	%eax,oz_hw486_estacksize # ... and so spt size will be an exact number of pages
	popl	oz_s_sysmem_pagtblsz	# save desired number of extra system pages (re-calculated later)

## Clear 'in loader' flag

	movl	$0,oz_s_inloader

## Determine highest global page vaddr+1.  If the cpu supports global pages, it is PROCBASVADDR.  Otherwise, 0.
## Then, any invalidate at or above this will not require that global pages be shut off in order to invalidate.

	movl	oz_SECTION_PAGESTATE_VALID_W,%eax	# set up 'VALID_W' stuff in template pte's anyway
	shll	$9,%eax
	orl	%eax,pte_validw_gbl_krw
	orl	%eax,pte_validw_gbl_krw_nc
	orl	%eax,pte_validw_gbl_na
	movl	$1,%eax					# see if this CPU supports global pages
	cpuid
	bt	$13,%edx
	jnc	nogblpages
	movl	$PROCBASVADDR,oz_hw486_hiestgvadp1	# it does, so any pages in kernel common area are assumed to be global
	movb	$0x80,cr4_gblbit			# allow cpu init routine to set CR4's global-page-enable bit
	movl	$PT_G,%eax				# set the GLOBAL bit in our template PTE's
	orl	%eax,pte_gbl
	orl	%eax,pte_validw_gbl_krw
	orl	%eax,pte_validw_gbl_krw_nc
	orl	%eax,pte_validw_gbl_na
nogblpages:

## Initialize video driver and print message

	call	oz_dev_video_init
	pushl	$init_msg
	pushl	$init_msglen
	call	oz_hw_putcon
	addl	$8,%esp

## Fill in IDT with entries that call default handler.
## These handlers run in true kernel mode, all they do 
## is print the registers, a traceback, then hang.

	leal	oz_hw486_kernel_idt,%edi	# point to table to fill in
	call	oz_hw_idt_init			# fill it in

## Set up permanent IDT and GDT

	lgdt	oz_hw486_kernel_gdtr
	lidt	oz_hw486_kernel_idtr

## Load code and data segment registers using the new GDT
## Theoretically, we never touch %ds/%ef/%fs/%gs from now on
## If the user corrupts them, we should get a GP fault and then we fix them

	ljmp	$KNLCODESEG,$new_cs
new_cs:
	movw	$USRDATASEG,%ax
	movw	$KNLDATASEG,%bx
	movw	%ax,%ds
	movw	%ax,%es
	movw	%ax,%fs
	movw	%ax,%gs
	movw	%bx,%ss

## Set kernel stack pointer to just below the memory hole for now
## Later, we move it to HOLEPAGE*4096-<oz_hw486_estacksize+8192>*cpuindex

	movl	$HOLEPAGE*4096,%esp

## Print out stuff

	pushl	spt_end_pointer
	pushl	spt_end_virtaddr
	pushl	oz_s_phymem_totalpages
	pushl	oz_hw486_estacksize
	pushl	$init_msg1
	call	oz_knl_printk
	addl	$20,%esp

## If we have SYSENTER/SYSEXIT, set stuff up to use them

.if EXECODESEG == KNLCODESEG
	movl	$1,%eax				# see if CPU has SYSENTER/SYSEXIT
	cpuid
	bt	$11,%edx
	jnc	no_sysenter_sysexit
	andw	$0x0FFF,%ax			# - but it only works in these models
	cmpw	$0x0633,%ax
	jc	no_sysenter_sysexit
	pushl	%edx				# ok, print a message
	pushl	%eax
	pushl	$sysenter_sysexit_msg
	call	oz_knl_printk
	addl	$12,%esp
	movl	$1,oz_hw486_sysentsysex		# remember we are doing it
	movb	$0x0F,syscall_patch+0		# patch in the sysenter instruction
	movb	$0x34,syscall_patch+1
	jmp	end_sysenter_sysexit
no_sysenter_sysexit:
	pushl	%edx
	pushl	%eax
	pushl	$nosysenter_sysexit_msg
	call	oz_knl_printk
	addl	$12,%esp
	movb	$0x90,%al			# no sysenter/sysexit, NOP some stuff in thread_switchctx routine
	movl	$thread_switchctx_wipeend-thread_switchctx_wipebeg,%ecx
	movl	$thread_switchctx_wipebeg,%edi
	cld
	rep
	stosb
end_sysenter_sysexit:
.endif

## See if we have FXSAVE/FXRSTOR instructions

	movl	$1,%eax				# see if CPU has FXSAVE/FXRSTOR
	cpuid
	bt	$24,%edx
	jnc	cpu_has_fxsave_fxrstor
	movl	fnsave_mmx,%eax			# if not, patch fnsave/nop with fxsave
	movl	frstor_mmx,%edx			# ... and patch frstor/nop with fxrstor
	movl	%eax,fnsave_pat
	movl	%edx,frstor_pat
	movl	$0x200,cr4_osfxsr_bit		# then set OSFXSR bit on all cpus
cpu_has_fxsave_fxrstor:

## Map the local APIC to virtual address, overlaying the altcpu startup page

	call	oz_hw486_apic_map

## The SPT will fill up to the bottom of the executive stacks
##   oz_s_sysmem_pagtblsz = (lowest_stack_address - SPTBASE) / 4
##   where lowest_stack_address = HOLEPAGE*4096-<oz_hw486_estacksize+8K>*MAXCPUS
## Unlike everything else up to now, these pages will not initially be mapped to anything
## As time goes on, they will map non-paged pool, paged pool, loadable system images, etc

	movl	oz_hw486_estacksize,%eax	# calc how many bytes are used by executive stacks
	movl	$MAXCPUS,%ecx
	addl	$8192,%eax			# (8K includes a kernel stack page and a guard page)
.if PARANOID_MPD
	addl	$4096,%eax			# (an add'l 4K for per-cpu MPD)
.endif
	mull	%ecx
	movl	%eax,%ecx
	movl	$(HOLEPAGE*4096)-SPTBASE,%ebx	# calc how many bytes are available for SPT
	subl	%ecx,%ebx
	andl	$0xFFFFF000,%ebx		# round down to page boundary
	shrl	$2,%ebx				# calc how many sptes will fit (at 4bytes per spte)

	cmpl	$PROCBASVPAGE,%ebx		# make sure we don't overflow into per-process addresses
	jc	dont_have_max_syspages
	movl	$PROCBASVPAGE,%ebx
dont_have_max_syspages:

	movl	spt_end_pointer,%eax		# see how many pointers are used up already
	subl	$SPTBASE,%eax
	shrl	$2,%eax

	subl	%eax,%ebx			# see how many pointers we will be adding
	cmpl	$MINSYSPAGES,%ebx		# make sure we will be adding enough
	jge	have_min_syspages
	pushl	$MINSYSPAGES
	pushl	%ebx
	pushl	$dont_have_min_syspages
	call	oz_crash
have_min_syspages:

	addl	%eax,%ebx			# restore total number of pointers
	movl	%ebx,oz_s_sysmem_pagtblsz	# that's how much we will have

	pushl	%ebx
	pushl	%eax
	pushl	$kexp_msg1
	call	oz_knl_printk
	addl	$12,%esp

## Create the srp_table (table of reqprot bytes used for system page table)
## It goes immediately after the loaded kernel image

	movl	%ebx,%edx			# this is how many '2-bits' we need
	addl	$0x3FFF,%edx			# round up to a page worth of bytes
	shrl	$14,%edx			# get the number of pages we will need

	movl	spt_end_pointer,%esi		# get the corresponding spte pointer to map the pages
	movl	spt_end_virtaddr,%edi		# get the starting virtual = physical address = end of kernel image
	movl	%edi,srp_table			# save starting address of the srp_table
make_srp_table:
	movl	pte_validw_gbl_krw,%eax		# mark all pages as VALID, GLOBAL and KERNEL WRITABLE
	orl	%edi,%eax
	movl	%eax,(%esi)
	invlpg	(%edi)
	xorl	%eax,%eax			# clear the page of rpe's
	movl	$1024,%ecx
	cld
	rep
	stosl
	addl	$4,%esi				# increment to next spte
	decl	%edx				# repeat if there are more to do
	jne	make_srp_table

	movl	%esi,spt_end_pointer		# save end of system page table we have used up
	movl	%edi,spt_end_virtaddr		# save end of virtual addresses we have used up

	pushl	%esi
	pushl	%edi
	pushl	$init_msg2
	call	oz_knl_printk
	addl	$12,%esp

	shrl	$12,%edi
	movl	%edi,oz_hw486_dynphypagbeg

## Zero fill the as-of-yet unused entries of the spt

	movl	%esi,%edi			# point to first unused spte
	movl	%ebx,%ecx			# get total number of spte's

	movl	%esi,%eax			# calculate how many spte's are already filled in
	subl	$SPTBASE,%eax
	shrl	$2,%eax

	subl	%eax,%ecx			# this is how many we have to fill in

	movl	oz_SECTION_PAGESTATE_PAGEDOUT,%eax # mark all entries as being paged out
	shll	$9,%eax
	cld
	rep
	stosl

## Re-fill the MPD so that it points to all the possible spt pages

	movl	$MPDBASE,%edi			# point to beginning of the MPD
	movl	oz_s_sysmem_pagtblsz,%ecx	# get how many pages are mapped by the spt when fully utilised
	shrl	$10,%ecx			# get how many pages the spt takes up
	movl	$SPTBASE|MPD_BITS,%ebx		# get mpd entry for first spt page
refill_mpd:
	movl	%ebx,(%edi)			# store mpd entry
	addl	$0x1000,%ebx			# increment to next spt page
	addl	$4,%edi				# increment mpd entry pointer
	loop	refill_mpd			# repeat if more to do
refill_mpd_zero:
	movl	%ecx,(%edi)			# zero out the rest of it
	addl	$4,%edi
	cmpl	$MPDBASE+0x1000,%edi
	jc	refill_mpd_zero

## Set up double mapping of oz_s_systempdata page so that it will have same virtual address as per-process 
## oz_sys_pdata_array[kernel] when the system process is current.  This eliminates the oz_sys_pdata_pointer 
## routine having to test to see if the current process is the system process.

	movl	$oz_sys_pdata_array,%eax	# this is virt address we want to double-map it at
	movl	%eax,%edx

	shrl	$22,%eax			# get index in MPD page
	movl	$syspdataptpage+PT_KRW,MPDBASE(,%eax,4) # set up double map (its physaddr = its virtaddr)

	shrl	$12,%edx			# get index in syspdataptpage page
	andl	$1023,%edx
	movl	$oz_s_systempdata+PT_KRW,syspdataptpage(,%edx,4) # set up double map (its physaddr = its virtaddr)

## Round end of virtual addresses to end of the corresponding spt page
## Later we will write-protect those spt pages as they should never change
## We don't consume any physical pages here, just spt entries

	movl	spt_end_pointer,%edx		# round this up to the end of the static spt page(s)
	movl	oz_SECTION_PAGESTATE_VALID_R,%eax # mark all entries as VALID so boot routine won't try to re-use them
	shll	$9,%eax
round_spt_loop:
	testl	$0x00000FFF,%edx
	je	round_spt_done
	movl	%eax,(%edx)
	addl	$4,%edx
	addl	$4096,spt_end_virtaddr
	jmp	round_spt_loop
round_spt_done:
	movl	%edx,spt_end_pointer
	movl	%edx,static_spt_end

	pushl	spt_end_virtaddr
	pushl	spt_end_pointer
	pushl	$init_msg3
	call	oz_knl_printk
	addl	$12,%esp

## Allocate an double spt entry for each possible cpu to use to address physical memory
## The virtual addresses for these pages are at the beginning of the dynamic spt area so they can be written to
## We do not consume any physical pages doing this
## Set the pages to software state VALID_W so the oz_knl_boot_firstcpu routine will block them off from general use
## The oz_hw_irq_init routine uses these to access the IOAPIC, so this has to be done before it is called

	movl	spt_end_pointer,%eax
	movl	spt_end_virtaddr,%ebx
	movl	%eax,cpu_sptentry
	movl	%ebx,cpu_sptvaddr
	addl	$MAXCPUS*2*4096,spt_end_virtaddr
	movl	$MAXCPUS*2,%ecx
	movl	oz_SECTION_PAGESTATE_VALID_W,%edx
	shll	$9,%edx
cpu_spt_init:
	movl	%edx,(%eax)
	addl	$4,%eax
	loop	cpu_spt_init
	movl	%eax,spt_end_pointer

##########################################################################
##									##
##  System Global Memory is all set up in its permanent form:		##
##									##
##					  <- oz_s_sysmem_pagtblsz*4k	##
##	+-------------------------------+				##
##	|				|				##
##	|  available (set no-access)	| <- spt_end_virtaddr		##
##	+-------------------------------+				##
##	|				|				##
##	|  cpu pages			| <- cpu_sptvaddr		##
##	+-------------------------------+-------------- everything below here is statically mapped
##	|				|               ie, the spte's should never change
##	|  round to 4M boundary		|				##
##	+-------------------------------+-------------- end of consumed physical memory
##	|				|				##
##	|  system reqprot table		| <- srp_table			##
##	+-------------------------------+				##
##	|				|				##
##	|  kernel			| <- 00100000			##
##	+-------------------------------+				##
##	|				|				##
##	|				|				##
##	|  memory hole			| <- 000A0000			##
##	+-------------------------------+				##
##	|  kernel stack			| <- %esp			##
##	|   .  .  .  .  .  .  .  .  .	|				##
##	|				| <- SPTBASE + oz_s_sysmem_pagtblsz*4
##	+-------------------------------+				##
##	|				|				##
##	|  available (set no-access)	| <- spt_end_pointer		##
##	|  (cpu reserved entries)	| <- cpu_sptentry		##
##	+ - - - - - - - - - - - - - - - +page boundary			##
##	|  (pad to page boundary)	|				##
##	|  SPT (read-only)		| <- 00003000 = SPTBASE		##
##	+-------------------------------+				##
##	|  MPD (read-only)		| <- 00002000 = MPDBASE		##
##	+-------------------------------+				##
##	|  PHYS: alt cpu startup	| <- 00001000 = ALTCPUBASE	##
##	|  VIRT: apic (or no-access)	| <- 00001000 = APICBASE	##
##	+-------------------------------+				##
##	|  no access			| <- 00000000			##
##	+-------------------------------+				##
##									##
##########################################################################

## Fill in IDT with special handlers for some exceptions

	movl	$kernel_idt_fix,%esi	# point to fixup table
idt_fix_loop:
	movl	(%esi),%edi		# get vector number
	shll	$3,%edi			# multiply by 8 because each entry is 8 bytes
	js	idt_fix_done		# - done if negative
	addl	$oz_hw486_kernel_idt,%edi # point to entry to modify
	movw	$0x8E00,%ax		# get base seg flag bits
	orb	4(%esi),%ah		# or in any modifications
	movw	8(%esi),%bx		# copy in the new code segment
	movw	12(%esi),%cx		# copy in low-order entrypoint
	movw	14(%esi),%dx		# copy in high-order entrypoint
	movw	%cx,0(%edi)
	movw	%bx,2(%edi)
	movw	%ax,4(%edi)
	movw	%dx,6(%edi)
	addl	$16,%esi		# increment to next fixup table entry
	jmp	idt_fix_loop
idt_fix_done:

## Set up the stack pointer (based on cpuid) and cpu database
## This also switches the cpu to executive mode
## It also makes a guard page for the exec stack

	leal	firststart_inirtn,%edi
	jmp	initcpudb
firststart_inirtn:

## Set up other system parameters in oz_knl_sdata.c

	movl	oz_hw486_maxcpus,%eax		# this is the max number of cpus we can handle
	movl	%eax,oz_s_cpucount		# =1 for uniprocessor; =MAXCPUS for multiprocessor
	movl	$0,oz_s_sysmem_baseva		# first entry of spt maps virt address zero

## Set up cache sizes

	movl	$1,oz_s_phymem_l1pages		# effectively disable memoury colouring
	movl	$1,oz_s_phymem_l2pages

## - Intel style

	xorl	%eax,%eax			# see if CPU will tell us about its caches
	cpuid
	cmpl	$2,%eax
	jc	nocpumemcacheintel
	cmpl	$0x756E6547,%ebx
	jne	nocpumemcacheintel
	cmpl	$0x6C65746E,%ecx
	jne	nocpumemcacheintel
	cmpl	$0x49656E69,%edx
	jne	nocpumemcacheintel
	movl	$2,%eax				# ok, ask it
	cpuid
	xorb	%al,%al				# low byte of %eax is garbage
	testl	%eax,%eax			# if top bit of register is set, 
	jns	cpumemcacheaxok
	xorl	%eax,%eax			# ... that means to ignore it
cpumemcacheaxok:
	testl	%ebx,%ebx
	jns	cpumemcachebxok
	xorl	%ebx,%ebx
cpumemcachebxok:
	testl	%ecx,%ecx
	jns	cpumemcachecxok
	xorl	%ecx,%ecx
cpumemcachecxok:
	testl	%edx,%edx
	jns	cpumemcachedxok
	xorl	%edx,%edx
cpumemcachedxok:
	pushl	%edx				# make an byte array on the stack
	pushl	%ecx
	pushl	%ebx
	pushl	%eax
	movl	$16,%ecx			# we just pushed 16 bytes on the stack
cpumemcacheloop:
	xorl	%ebx,%ebx			# get a cache descriptor byte
	movb	(%esp),%bl
	incl	%esp
	movl	$cpumemcachetable,%esi		# loop though decoding table
cpumemcachetableloop:
	cmpb	%bl,(%esi)			# see if the entry matches
	jne	cpumemcachetablenext
	movzbl	1(%esi),%eax			# ok, get L1's pages/cache
	movzbl	2(%esi),%edx			# and get L2's pages/cache
	cmpl	%eax,oz_s_phymem_l1pages	# only set L1 value
	jnc	cpumemcachenol1
	movl	%eax,oz_s_phymem_l1pages	# ... if table is .gt. what was there
cpumemcachenol1:
	cmpl	%edx,oz_s_phymem_l2pages	# same with L2 value
	jnc	cpumemcachenol2
	movl	%edx,oz_s_phymem_l2pages
cpumemcachenol2:
	jmp	cpumemcachenext
cpumemcachetablenext:
	addl	$3,%esi				# didn't match, check next table entry
	cmpb	$0,(%esi)
	jne	cpumemcachetableloop
cpumemcachenext:
	loop	cpumemcacheloop			# repeat to process next cache descriptor
	jmp	cpumemcacheend
nocpumemcacheintel:

## - AMD style

	movl	$0x80000000,%eax		# check for AMD style
	cpuid
	cmpl	$0x80000006,%eax
	jc	nocpumemcacheamd
	cmpl	$0x68747541,%ebx
	jne	nocpumemcacheamd
	cmpl	$0x444D4163,%ecx
	jne	nocpumemcacheamd
	cmpl	$0x69746E65,%edx
	jne	nocpumemcacheamd
	movl	$0x80000005,%eax		# get L1 info
	cpuid
	call	decodeamdl1			# decode %ecx -> put in oz_s_phymem_l1pages if bigger
	movl	%edx,%ecx			# decode %edx -> put in oz_s_phymem_l1pages if bigger
	call	decodeamdl1
	movl	$0x80000006,%eax		# get L2 info
	cpuid
	movl	%ecx,%eax			# get cache size in KB -> %eax
	shrl	$16,%eax
	shrl	$12,%ecx			# get associativity -> %cl
	andb	$0x0F,%cl
	je	amdnol2				#   0: no L2 cache present
	decb	%cl
	je	amdgotl2			#   1: direct size
	shrl	$1,%eax
	decb	%cl				#   2: 2-way assoc
	je	amdgotl2
	shrl	$1,%eax
	subb	$2,%cl				#   4: 4-way assoc
	je	amdgotl2
	shrl	$1,%eax
	subb	$2,%cl				#   6: 8-way assoc
	je	amdgotl2
	shrl	$1,%eax
	subb	$2,%cl				#   8: 16-way assoc
	jne	amdnol2
amdgotl2:
	shrl	$2,%eax				# convert KB to 4KB page count
	cmpl	oz_s_phymem_l2pages,%eax
	jc	amdnol2
	movl	%eax,oz_s_phymem_l2pages	# set L2 cache page modulus
amdnol2:
	jmp	nocpumemcacheamd
decodeamdl1:
	shrl	$16,%ecx			# get cl=assoc, ch=sizeinkb
	testb	%cl,%cl				# if zero, assume it's not there
	je	noamdl1
	xorl	%eax,%eax			# get size in kb
	movb	%ch,%al
	divb	%cl				# div by associativity factor
	shrb	$2,%al				# convert from kb to num pages
	cmpb	oz_s_phymem_l1pages,%al		# see if bigger than what we have
	jc	noamdl1
	movb	%al,oz_s_phymem_l1pages		# save as factor
noamdl1:
	ret
nocpumemcacheamd:
cpumemcacheend:

## Init interrupt system, fill in IDT as needed for interrupt processing

	leal	oz_hw486_kernel_idt,%edi
	call	oz_hw486_irq_init

## Write-lock the protected data, they should never change from now on

	MOVL_CR0_EAX				# turn on WP bit so we can't write the prot_start..prot_end values
	orl	$0x00010000,%eax
	MOVL_EAX_CR0

## The MPD should never change from now on, so write-protect the virtual page

	movl	$MPDBASE,%eax
	call	writeprotsptpage

## Set all the G bits in the static SPTE's - they are common to all processes
## This will keep them from being invalidated whenever there is a process switch

	movl	$SPTBASE,%edi			# point to the base of the SPT
	movl	pte_gbl,%eax			# we have the g-bit or we have a 0 if cpu can't do it
set_g_bits:
	orl	%eax,(%edi)			# set the G bit in the pte (valid or not)
	addl	$4,%edi				# increment on to next entry
	cmpl	static_spt_end,%edi		# see if we reached the end of the static area
	jc	set_g_bits			# repeat if there is more to do

## Write protect the static page(s) of the SPT, they should never change from now on

	movl	$SPTBASE,%eax			# start at base of spt
spt_prot_loop:
	call	writeprotsptpage		# write protect the page of the spt
	addl	$4096,%eax			# increment to next page of spt
	cmpl	static_spt_end,%eax		# repeat until done
	jc	spt_prot_loop

## Copy all the curprot bits from the spte's that we have set up to the corresponding reqprot bits in the rpte's

	movl	$SPTBASE,%esi			# source is the spt
	movl	srp_table,%edi			# dest is the rpt
cpu_rpt_init:
	movb	  (%esi),%al			# get curprot bits in al<1:2>
	andb	$6,%al
	shrb	$1,%al				# put them in al<0:1>
	movb	 4(%esi),%dl			# get next curprot bits in dl<1:2>
	andb	$6,%dl
	shlb	$1,%dl				# shift to dl<2:3>
	orb	%dl,%al				# merge into al
	movb	 8(%esi),%dl			# get next curprot bits in dl<1:2>
	andb	$6,%dl
	shlb	$3,%dl				# shift to dl<4:5>
	orb	%dl,%al				# merge into al
	movb	12(%esi),%dl			# get next curprot bits in dl<1:2>
	andb	$6,%dl
	shlb	$5,%dl				# shift to dl<6:7>
	orb	%dl,%al				# merge into al
	movb	%al,(%edi)			# store in reqprot byte
	addl	$16,%esi			# increment past the four spte's
	incl	%edi				# increment to the next reqprot byte
	cmpl	spt_end_pointer,%esi		# repeat if there are more entries to process
	jc	cpu_rpt_init

## Finally, we can enable hardware interrupts

	sti

## Call the boot routine for the first cpu to set it up
## call it thusly: oz_knl_boot_firstcpu (first_free_virt_page, first_free_phys_page)

	movl	spt_end_virtaddr,%ebx
	shrl	$12,%ebx
	pushl	oz_hw486_dynphypagbeg		# first free physical page
	pushl	%ebx				# first free virtual page
	call	oz_knl_boot_firstcpu		# call the routine

	####	call	oz_knl_halt		#### halt this cpu so it doesn't do anything anymore

## When it returns, we just loop in hlt instructions forever
## Alternatively, we could call oz_knl_thread_exit in which case the scheduler would go into a tight loop looking for threads to process

firstcpu_loop:
	pushl	oz_SUCCESS			# hlt instr doesn't work in executive mode
	call	oz_knl_thread_exit
	hlt
	jmp	firstcpu_loop

##########################################################################
#
#  Write protect the MPD/SPT page whose virt address is in %eax
#  %eax assumed to be on 4K boundary
#  Scratch: %edx
#
writeprotsptpage:
	movl	%eax,%edx			# get offset SPT entry
	shrl	$10,%edx
	andb	$0xFD,SPTBASE(%edx)		# clear the write-enabled bit
	orb	$0x04,SPTBASE(%edx)		# make it user-read, something that the kernel will understand
						# and not think the page is not mapped to anything, who cares 
						# if user-mode programs actually read the mpd/spt, no secrets here
	movl	%eax,%edx			# invalidate the virtual page so cpu gets new descriptor
	INVLPG_EDX
	ret

##########################################################################
##									##
##  Symmetric multiprocessing						##
##									##
##########################################################################

##########################################################################
##
##  This is jumped to by the cpu's when they each start up
##  At this point, they are in 32-bit mode but paging is not enabled
##  Also, the mtrr's have been copied from the boot cpu
##
	.align	4
	.globl	oz_hw486_cpu_bootalt
	.type	oz_hw486_cpu_bootalt,@function
oz_hw486_cpu_bootalt:

	####	cpu_bootalt_loop:
	####		jmp	cpu_bootalt_loop

## Set up the cpu's segment registers

	movw	$USRDATASEG,%bx
	movw	%bx,%ds
	movw	%bx,%es
	movw	%bx,%fs
	movw	%bx,%gs
	movw	$KNLDATASEG,%bx
	movw	%bx,%ss

## Set up the idt (gdt is already set up)

	lidt	oz_hw486_kernel_idtr

## Enable paging

	wbinvd
	movl	$MPDBASE,%eax			# point to system global MPD
	movl	%eax,%cr3			# store in CR3
	movl	%cr0,%eax			# get what's currently in CR0
	orl	$0x80000000,%eax		# set the paging enable bit
	movl	%eax,%cr0			# now paging is enabled
	wbinvd

## Set up the stack pointer and cpu database
## This also switches us to executive mode

	leal	altstart_inirtn,%edi
	jmp	initcpudb
altstart_inirtn:

	MOVL_CR0_EAX				# turn on WP bit so we can't write the prot_start..prot_end values
	orl	$0x00010000,%eax
	MOVL_EAX_CR0

## Get current time to help synchronize RDTSC
## Boot processor just called oz_hw_tod_iotanow

	call	oz_hw_tod_iotanow

## Inhibit software interrupts as required by oz_knl_boot_anothercpu

	pushl	$0
	call	oz_hw_cpu_setsoftint
	addl	$4,%esp

## Enable hardware interrupts as required by oz_knl_boot_anothercpu

	sti

## Call the kernel's alt cpu boot routine

	call	oz_knl_boot_anothercpu

## When it returns, we just loop in hlt instructions forever
## Alternatively, we could call oz_knl_thread_exit in which case the scheduler would go into a tight loop looking for threads to process

altcpu_loop:
	pushl	oz_SUCCESS		# hlt instr doesn't work in executive mode
	call	oz_knl_thread_exit
	hlt
	jmp	altcpu_loop

##########################################################################
##									##
##  Initialize cpu database for current cpu				##
##									##
##    Input:								##
##									##
##	edi = return address						##
##	WP bit is turned off						##
##	cpu in kernel mode						##
##									##
##    Output:								##
##									##
##	stack and cpu data set up					##
##	WP bit is still turned off					##
##	cpu might be in exec mode					##
##									##
##########################################################################

	.align	4
initcpudb:

## Reset the FPU

	clts					# enable access to fpu
	fninit					# initialize the fpu
	movl	%cr0,%eax			# set the TS bit to inhibit access to fpu
	orb	$8,%al
	movl	%eax,%cr0

## Set up the local APIC, get my cpuid in %eax

	leal	apci_setup_rtn,%ecx
	jmp	oz_hw486_apic_setup
apci_setup_rtn:

## Set up a stack based on cpuid (left in %eax by oz_hw486_apic_setup)
## oz_hw486_apic_setup has already validated the value in %eax

	movl	$HOLEPAGE*4096,%esp		# point to top of CPU 0's stack
	movl	oz_hw486_estacksize,%ecx	# get stack size without the kernel stack page and guard page
	addl	$8192,%ecx			# add room for kernel stack page and guard page
.if PARANOID_MPD
	addl	$4096,%ecx			# add room for per-cpu MPD
.endif
	mull	%ecx				# get displacement for this cpu's stack
	subl	%eax,%esp			# move the stack pointer

## If we are doing SYSENTER/SYSEXIT, set up the MSR's for it

.if EXECODESEG == KNLCODESEG
	cmpl	$0,oz_hw486_sysentsysex		# make sure cpu's support it
	je	no_init_sysenter_sysexit
	movl	$KNLCODESEG,%eax		# tell CPU what to use for kernel code segment (doesn't ever change)
	xorl	%edx,%edx
	movl	$SYSENTER_CS_MSR,%ecx
	wrmsr
	movl	$syscall_sep_enter,%eax		# tell CPU where our service routine is (doesn't ever change)
	xorl	%edx,%edx
	movl	$SYSENTER_EIP_MSR,%ecx
	wrmsr
	movl	%esp,%eax			# set up stack pointer for now (gets switched on thread switch, though)
	xorl	%edx,%edx
	movl	$SYSENTER_ESP_MSR,%ecx
	wrmsr
no_init_sysenter_sysexit:
.endif

## Set up the per-cpu copy of the MPD

	movl	%edi,%edx			# save the return address
.if PARANOID_MPD
	subl	$4096,%esp			# hop over where our per-cpu copy of MPD will go
	movl	$1024,%ecx			# get number of longs to copy
	movl	$MPDBASE,%esi			# this is where they come from
	movl	%esp,%edi			# this is where they are going
	cld
	rep
	movsl
	movl	%esp,%eax			# now set our MPD pointer, never to change again!
	movl	%eax,%cr3			# (for these pages, phys addr = virt addr)
						# we haven't set the global page enable bit yet
.endif

## Make sure RDTSC will work from any mode
## Also enable the global-page bit (pte<08>)
## And enable the 4M page bit (for non-paged pool)
## And enable FXSAVE/FXRSTOR instructions

	movl	%cr4,%eax
	andb	$0xFB,%al
	orb	cr4_gblbit,%al
	orb	$0x10,%al
	orl	cr4_osfxsr_bit,%eax
	movl	%eax,%cr4

## Put a guard page at bottom of the stack

	movl	%esp,%eax			# copy the stack pointer
	subl	oz_hw486_estacksize,%eax
	subl	$8192,%eax			# this is the lowest address in the guard page
	movl	%eax,%esi			# save the base address
	shrl	$12,%eax			# this is the virtual page number of the guard page
	movl	pte_validw_gbl_na,%ebx		# set it to valid, global, no access, phypage 0
	movl	%ebx,SPTBASE(,%eax,4)
	invlpg	(%esi)				# invalidate the pagetable entry

##  Set up a TSS in the CPU_... struct - each cpu has its own TSS.
##  The TSS is only used by the cpu when it switches from an outer mode to an 
##  inner mode (for whatever reason).  It fetches the new stack pointer and 
##  new stack segment from the TSS.  Note that the idiot cpus do not store 
##  the KSP/ESP and KSS/ESS back in the TSS when they switch from inner to 
##  outer mode nor do they just use the values they have internally, thus the 
##  operation is not symmetric, and we have to kludge around to make it work 
##  nicely.  So every time the ESP is swapped, the TSS must also be updated for 
##  the cpu.

	pushl	%edx				# save return address

	call	oz_hw486_getcpu			# get cpu number and database pointer
	pushl	%eax				# save cpu number

	xorl	%eax,%eax			# get some zeroes
	movl	$CPU__SIZE/4,%ecx		# clear out the CPU_... struct
	movl	%esi,%edi
	cld
	rep
	stosl

.if PARANOID_MPD
	movl	%cr3,%eax			# save per-cpu MPD base
	movl	%eax,CPU_L_MPDBASE(%esi)
.endif

	popl	%eax				# restore my cpu number

	movl	$-1,CPU_Q_QUANTIM+0(%esi)	# say the quantum is a long way off
	movl	$-1,CPU_Q_QUANTIM+4(%esi)

	movw	$KNLDATASEG,CPU_X_TSS+TSS_W_SS0(%esi) # set up kernel stack segment
	movl	%esp,CPU_X_TSS+TSS_L_ESP0(%esi)	# set up kernel stack pointer
	addl	$4,CPU_X_TSS+TSS_L_ESP0(%esi)	# (the return address is still on stack)
.if EXEDATASEG <> KNLDATASEG
	movw	$EXEDATASEG,CPU_X_TSS+TSS_W_SS1(%esi) # set up executive stack segment
	movl	%esp,CPU_X_TSS+TSS_L_ESP1(%esi)	# set up executive stack pointer
	subl	$4092,CPU_X_TSS+TSS_L_ESP1(%esi) # (it starts one page below the kernel stack)
.endif

	movl	%eax,%edi			# point to where the tss' gdt entry goes
	shll	$3,%edi
	addl	$kernel_gdt_tss,%edi
	leal	CPU_X_TSS(%esi),%ecx		# base address of the tss in the CPU_... struct
	movl	%ecx,%ebx			# copy the address
	roll	$16,%ecx			# swap the words around
	movl	%ecx,%edx			# save the swapped words
	movw	$TSS__SIZE,%cx			# put tss size in ecx<00:15>, low order address is in ecx<16:31>
	movl	%ecx,0(%edi)
	andl	$0xFF000000,%ebx		# tss address<24:31> is in ebx<24:31>
	andl	$0x000000FF,%edx		# tss address<16:23> is in edx<00:07>
	orl	%ebx,%edx			# put them together
	orl	$0x00008900,%edx		# or in the flag bits
	movl	%edx,4(%edi)			# store it
	movl	%eax,%ebx			# get the corresponding segment number
	shll	$3,%ebx
	addl	$TASKSEG,%ebx
	ltr	%bx				# load the descriptor

	popl	%edi				# restore return address

## Set up debug registers to protect first MPD entry for all cpu's
## Although the detection is after the fact, the TLB cache is probably still good

.if PARANOID_MPD
	movl	$(HOLEPAGE-1)*4096,%ebx		# point to first entry of CPU 0's MPD
	movl	%ebx,%dr0
	subl	oz_hw486_estacksize,%ebx	# move to first entry of CPU 1's MPD
	subl	$3*4096,%ebx			# - skip CPU 0's kernel stack, guard and CPU 1's MPD
	movl	%ebx,%dr1

	movl	$0x00DD060A,%ebx
	movl	%ebx,%dr7
.else
##	movl	$MPDBASE,%ebx			# point to first entry of system MPD
##	movl	%ebx,%dr0
##	movl	$PROCMPDVADDR,%ebx		# point to first entry of per-process MPD (when it's mapped)
##	movl	%ebx,%dr1
.endif

## Set up the rest of the cpu data

	movl	$0,CPU_L_PRIORITY(%esi)		# make us the lowest priority for now
	movb	$1,CPU_B_SMPLEVEL(%esi)		# we're at smp level 1 = softint delivery inhibited
	movl	%eax,%ebx			# calculate address of my own spte pair that is used for accessing physical pages
	shll	$3,%ebx
	addl	cpu_sptentry,%ebx
	movl	%ebx,CPU_L_SPTENTRY(%esi)
	movl	%eax,%ebx			# calculate corresponding virtual address
	shll	$13,%ebx
	addl	cpu_sptvaddr,%ebx
	movl	%ebx,CPU_L_SPTVADDR(%esi)
	lock					# set the active cpu bit
	bts	%eax,oz_hw486_activecpus

## Switch to executive mode

.if EXECODESEG <> KNLCODESEG
	pushl	$EXEDATASEG			# set up operands on kernel stack for IRET instruction
	pushl	CPU_X_TSS+TSS_L_ESP1(%esi)	# - its executive mode stack pointer
	pushl	$0x1000				# - its EFLAGS = interrupts disabled, iopl = executive mode
	pushl	$EXECODESEG			# - its code segment
	pushl	$cpudbinit_execmode		# - its instruction pointer
	iret					# iret with 32-bit operands
						# the cpu's kernel stack should now be completely empty
cpudbinit_execmode:
.endif

## Push a null stack frame for condition handlers to have something to terminate on

	pushl	$0				# null return address
	pushl	$0				# null previous frame
	movl	%esp,%ebp			# set up frame pointer

## Return

	jmp	*%edi

##########################################################################
##									##
##  Processes								##
##									##
##########################################################################

	## These offsets must match the Prctx struct in oz_hw_process_486.c

	PRCTX_L_MPDPA =  0			# physical address of process local mpd

	## This defines the address of the oz_sys_pdata_array for processes
	## It puts the array just below the reqprot/pagetable pages
	## oz_hw_process_486.c sets them up this way

	.globl	oz_sys_pdata_array
	.type	oz_sys_pdata_array,@object
	oz_sys_pdata_array = PROCPTRPBVADDR - 8192

	.text

##########################################################################
##									##
##  Switch process hardware context, ie, access new process'		##
##  pagetables								##
##									##
##	 4(%esp) = old process hardware context block address		##
##	 8(%esp) = new process hardware context block address		##
##	smplevel = ts							##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_process_switchctx
	.type	oz_hw_process_switchctx,@function
oz_hw_process_switchctx:
	pushl	%ebp				# make stack frame for tracing errors
	movl	%esp,%ebp
	pushl	%esi				# save C register
	call	oz_hw486_getcpu			# get current cpu index in %eax, database in %esi
	movl	12(%ebp),%edx			# point to new process context
	movl	%edx,CPU_L_PRCTX(%esi)		# save new process context

.if PARANOID_MPD
						# don't switch actual MPD's
						# - just copy the new process' per-process MPDE's
						# - leave the system MPDE's intact
	pushl	%edi				# save other C registers
	pushl	%ebx
	movl	%esi,%ebx			# keep safe from movsl
	movl	CPU_L_SPTENTRY(%ebx),%ecx	# save old mapping at that address
	pushl	(%ecx)
	movl	PRCTX_L_MPDPA(%edx),%eax	# get this process' MPD physical address (page aligned)
	movb	$PT_KRO,%al			# we only need to read the data
	movl	%eax,(%ecx)
	movl	CPU_L_SPTVADDR(%ebx),%edx	# tell CPU it's all new mapping for that virt address
	INVLPG_EDXNG
	movl	$1024-(PROCBASVPAGE/1024),%ecx	# number of MPD entries to copy (each entry covers 4M)
	movl	%edx,%esi			# point to base of per-process MPD we just mapped
	addl	$PROCBASVPAGE/256,%esi		# offset to the first per-process MPD entry
	movl	CPU_L_MPDBASE(%ebx),%edi	# point to base of per-CPU MPD
	addl	$PROCBASVPAGE/256,%edi		# offset to the first per-process MPD entry
	cld					# copy per-process MPD entries from per-process MPD to per-CPU MPD
	rep
	movsl
	movl	CPU_L_SPTENTRY(%ebx),%ecx	# restore old mapping at that address
	popl	(%ecx)
	INVLPG_ALLNG				# invalidate all non-global translation cache entries
	popl	%ebx				# restore C registers
	popl	%edi
.else
	movl	PRCTX_L_MPDPA(%edx),%eax	# get this process' MPD physical address (page aligned)
.if 0000
	call	check_new_mpd			# sanity check, no double-fault handler can fix a bad mpd!!
.endif
	MOVL_EAX_CR3				# point to it (we're now using that process' address space)
.endif
	popl	%esi				# restore C register
	leave					# restore frame pointer
	ret

#  Simple test to see if new MPD will be valid

.if 0000
	.align	4
check_new_mpd:
	pushal
	cmpl	$MPDBASE,%eax
	je	check_new_mpd_ok
	testl	$0x0FFF,%eax			# low 12 bits should be clear
	jne	check_new_mpd_bad
	movl	%eax,%edi			# read first long of new MPD
	call	fetch_phys_long
	cmpl	%eax,MPDBASE			# it should match global MPD
	je	check_new_mpd_ok		# - so we know mapping of first 4M is ok
	pushl	MPDBASE
	pushl	%eax
	pushl	$check_new_mpd_msg1
	call	oz_crash
check_new_mpd_bad:
	pushl	%eax
	pushl	$check_new_mpd_msg2
	call	oz_crash
check_new_mpd_ok:
	popal
	ret

check_new_mpd_msg1:	.string "oz_hw_process_switchctx: setting bad mpd - first long %x should be %x"
check_new_mpd_msg2:	.string "oz_hw_process_switchctx: low 12 bits mpd phyaddr %x non-zero"
.endif

##########################################################################
##									##
##  Scheduled threads							##
##									##
##########################################################################

##########################################################################
##									##
##  Initialize thread hardware context block				##
##									##
##    Input:								##
##									##
##	 4(%esp) = pointer to hardware context block			##
##	 8(%esp) = number of user stack pages				##
##	           0 if executive-mode only thread			##
##	12(%esp) = thread routine entrypoint				##
##	           0 if initializing cpu as a thread for the first time	##
##	16(%esp) = thread routine parameter				##
##	20(%esp) = process hardware context block pointer		##
##	24(%esp) = thread software context block pointer		##
##									##
##	smplock  = softint						##
##									##
##    Output:								##
##									##
##	thread hardware context block = initialized so that call to 	##
##	oz_hw_thread_switchctx will set up to call the thread routine	##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_thread_initctx
	.type	oz_hw_thread_initctx,@function
oz_hw_thread_initctx:
	pushl	%ebp					# make call frame
	movl	%esp,%ebp
	pushl	%edi					# save C registers
	pushl	%esi
	pushl	%ebx

##  Initialize hardware context area to zeroes

	xorl	%eax,%eax
	movl	$THCTX__SIZE/4,%ecx
	movl	8(%ebp),%edi
	cld
	rep
	stosl
	movl	8(%ebp),%edi				# point to hardware context area

##  If we're initializing this cpu as a thread, set it as current on the cpu and skip the rest

	cmpl	$0,16(%ebp)				# see if initializing cpu for the first time
	jne	thread_initctx_other
	call	oz_hw486_getcpu				# ok, get cpudb pointer
	movl	%edi,CPU_L_THCTX(%esi)			# save current thread hwctx pointer
	jmp	thread_initctx_rtnsuc
thread_initctx_other:

##  Set up user stack parameters - stack gets created when thread starts

	movl	12(%ebp),%eax				# save number of user stack pages
	movl	$PROCPTRPBVPAGE,THCTX_L_USTKVPG(%edi)	# set up user stack page number
	movl	%eax,THCTX_L_USTKSIZ(%edi)

##  Allocate an executive stack

	leal	THCTX_L_ESTACKVA(%edi),%eax		# set up executive stack memory
	pushl	%eax
	pushl	%edi
	call	oz_hw486_kstack_create
	addl	$8,%esp
	cmpl	oz_SUCCESS,%eax
	jne	thread_initctx_rtn
	movl	THCTX_L_ESTACKVA(%edi),%eax

		####	pushl	%eax
		####	pushl	%edi
		####	pushl	$thread_init_deb2
		####	call	oz_knl_printk
		####	addl	$12,%esp
		####	movl	THCTX_L_ESTACKVA(%edi),%eax

##  Set up what we want to happen when the 'RET' of the oz_hw_thread_switchctx routine executes

	leal	-THSAV__SIZE-20(%eax),%esi		# make room for 11 longs on new stack
	movl	%eax,THSAV_L_ES0(%esi)			# tss' esp = the very top of my stack
	pushfl						# set eflags = my eflags
.if EXECODESEG <> KNLCODESEG
	andb	$0xDF,1(%esp)				# ... with IOPL=1
.else
	andb	$0xCF,1(%esp)				# ... with IOPL=0
.endif
	popl	THSAV_L_EFL(%esi)
	movl	$0,THSAV_L_EBP(%esi)			# saved ebp = zeroes
	movl	$thread_init,THSAV_L_EIP(%esi)		# return address = thread_init routine
	movl	16(%ebp),%eax				# thread routine entrypoint
	movl	20(%ebp),%ebx				# thread routine parameter
	movl	24(%ebp),%ecx				# process hardware context
	movl	28(%ebp),%edx				# thread software context
	movl	%edx,THSAV__SIZE+ 0(%esi)		# thread software context
	movl	%edi,THSAV__SIZE+ 4(%esi)		# thread hardware context pointer
	movl	%eax,THSAV__SIZE+ 8(%esi)		# thread routine entrypoint
	movl	%ebx,THSAV__SIZE+12(%esi)		# thread routine parameter
	movl	%ecx,THSAV__SIZE+16(%esi)		# process hardware context pointer
	movl	%esi,THCTX_L_EXESP(%edi)		# save initial stack pointer

		####	pushl	%esi
		####	pushl	$THSAV__SIZE+16
		####	call	oz_knl_dumpmem
		####	addl	$8,%esp

thread_initctx_rtnsuc:
	movl	oz_SUCCESS,%eax				# set up success status
thread_initctx_rtn:
	popl	%ebx					# pop call frame
	popl	%esi
	popl	%edi
	leave
	ret

##########################################################################
##									##
##  The oz_hw_thread_switchctx routine just executed its RET		##
##  We are now in the target thread's context				##
##									##
##    Input:								##
##									##
##	 0(%esp) = thread software context block pointer		##
##	 4(%esp) = thread hardware context block pointer		##
##	 8(%esp) = thread routine entrypoint				##
##	12(%esp) = thread routine parameter				##
##	16(%esp) = process hardware context block pointer		##
##	smplevel = (see oz_knl_thread_start)				##
##									##
##########################################################################

	.align	4
thread_init:

		####	pushl	%esp
		####	pushl	$thread_init_deb3
		####	call	oz_knl_printk
		####	addl	$8,%esp
		####	pushl	%esp
		####	pushl	$20
		####	call	oz_knl_dumpmem
		####	addl	$8,%esp

##  Start thread

	call	oz_knl_thread_start		# do executive mode thread startup
						# comes back at smplock_null level
	addl	$4,%esp				# pop thread software context block pointer
	pushl	12(%esp)			# maybe the pagetable needs to be mapped
	call	oz_hw_process_firsthread
	addl	$4,%esp
	popl	%esi				# point to thread hardware context block pointer
	movl	THCTX_L_USTKSIZ(%esi),%ecx	# see if any user stack requested
	jecxz	thread_init_executive		# if not, it executes in executive mode
	leal	THCTX_L_USTKVPG(%esi),%edx	# point to where to return base virtual page number
	pushl	%edx				# create the stack section and map it
	pushl	%ecx
	call	oz_hw486_ustack_create
	addl	$8,%esp
	popl	%edi				# get user mode thread routine entrypoint
	popl	%edx				# get user mode thread routine parameter
	addl	$4,%esp				# get rid of process hw ctx

		####	pushl	%edx
		####	pushl	%edi
		####	pushl	%eax
		####	movl	$thread_init_either,%eax
		####	invlpg	(%eax)
		####	call	pte_print
		####	movl	(%esp),%eax
		####	invlpg	(%eax)
		####	call	pte_print
		####	popl	%eax
		####	popl	%edi
		####	popl	%edx

	pushl	$USRDATASEG			# set up operands on executive stack for IRET instruction
	pushl	%eax				# - its user mode stack pointer
	pushl	$0x200				# - its EFLAGS = interrupts enabled
	pushl	$USRCODESEG			# - its code segment
	pushl	$thread_init_either		# - its instruction pointer

		####	pushl	%edx
		####	pushl	%edi
		####	pushl	%esp
		####	pushl	$28
		####	call	oz_knl_dumpmem
		####	pushl	$thread_init_deb4
		####	call	oz_hw_pause
		####	addl	$12,%esp
		####	popl	%edi
		####	popl	%edx

	iret					# iret with 32-bit operands
						# the thread's executive stack should now be completely empty
						# - ready to accept interrupts, syscalls, pagefaults, etc
						# - the oz_hw_thread_switchctx routine set up tss' ksp for this stack

##########################################################################
##									##
##  This is the thread for threads that run in executive mode		##
##  Softint delivery is enabled						##
##									##
##########################################################################

	.align	4
thread_init_executive:
	popl	%edi				# get routine's entrypoint
	popl	%edx				# get routine's parameter
	popl	%eax				# pop and ignore process hw ctx pointer

		####	pushl	%edx
		####	pushl	%edi
		####	pushl	%edx
		####	pushl	%edi
		####	pushl	%esp
		####	pushl	$8
		####	call	oz_knl_dumpmem
		####	pushl	$thread_init_deb5
		####	call	oz_hw_pause
		####	addl	$20,%esp
		####	popl	%edi
		####	popl	%edx

##########################################################################
##									##
##  We are now in user mode for the first time				##
##  (This routine is also used for executive mode threads)		##
##									##
##    Input:								##
##									##
##	edi = thread entrypoint						##
##	edx = thread parameter						##
##	esp = very top of the stack					##
##									##
##########################################################################

thread_init_either:
	pushl	$0				# make a null call frame
						# the above push is the thread's first user-mode pagefault
	pushl	$0
	movl	%esp,%ebp			# point to the null stack frame

		####	pushl	%edi
		####	pushl	%edx
		####	pushl	%edi
		####	pushl	%edx
		####	pushl	$thread_init_deb6
		####	call	oz_knl_printk
		####	addl	$12,%esp
		####	popl	%edx
		####	popl	%edi

	pushl	%edx				# push the parameter for the thread routine
	call	*%edi				# call the thread routine

		####	pushl	%eax
		####	leal	thread_init_deb7,%esi
		####	movl	%esp,%edi
		####	movl	$0,%eax
		####	int	$INT_PRINTKP
		####	popl	%eax

	pushl	%eax				# force call to oz_sys_thread_exit
	call	oz_sys_thread_exit
	pushl	%eax
	pushl	$thread_init_msg1
	call	oz_crash

##thread_init_deb1:	.string	"oz_hw_thread_initctx*: user stack pages 0x%x, virt page 0x%x\n"
##thread_init_deb2:	.string	"oz_hw_thread_initctx*: thctx %p executive stack top %p\n"
##thread_init_deb3:	.string	"oz_hw_thread_initctx*: initial ksp %p\n"
##thread_init_deb4:	.string	"oz_hw_thread_initctx*: about to iret to user mode> "
##thread_init_deb5:	.string	"oz_hw_thread_initctx*: about to start knl thread> "
##thread_init_deb6:	.string	"oz_hw_thread_initctx*: now in user mode, param %p, entry %p\n"
##thread_init_deb7:	.string	"oz_hw_thread_initctx*: status %u\n"
##thread_init_deb8:	.string	"oz_hw_thread_initctx*: creating %u page user stack\n"
##thread_init_deb9:	.string	"oz_hw_thread_initctx*: proc %p ptsec %p\n"

thread_init_msg1:	.string	"oz_hw_thread_initctx: returned from oz_sys_thread_exit with status %u"

##########################################################################
##									##
##  Determine how much room is left on current kernel stack		##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_thread_kstackleft
	.type	oz_hw_thread_kstackleft,@function
oz_hw_thread_kstackleft:
	pushl	%ebp				# make a call frame
	movl	%esp,%ebp

	call	oz_knl_thread_getcur		# get pointer to current thread struct
	pushl	%eax
	call	oz_knl_thread_gethwctx		# get corresponding hardware context

	movl	THCTX_L_ESTACKVA(%eax),%edx	# get the top of the stack
	movl	%esp,%eax			# get current stack pointer
	subl	oz_hw486_estacksize,%edx	# calc the bottom of the stack
	subl	%edx,%eax			# calc how much room is left

	leave
	ret

##########################################################################
##									##
##  The thread has changed user stacks					##
##									##
##  This unmaps the old user stack section and sets up the new one to 	##
##  be unmapped when the thread exits.					##
##									##
##########################################################################

	.align	4
	.globl	oz_sys_thread_newusrstk
	.type	oz_sys_thread_newusrstk,@function
oz_sys_thread_newusrstk:
	pushl	%ebp
	pushl	%edi			# save scratch register
	int	$INT_NEWUSRSTK		# do the dirty work in exec mode
	popl	%edi			# restore scratch register
	popl	%ebp
	ret

#  0(%esp) = return address (just past the int instruction)
#  4(%esp) = caller's code segment
#  8(%esp) = caller's eflags
# 12(%esp) = caller's stack pointer
# 16(%esp) = caller's stack segment

	.align	4
procnewusrstk:
	pushl	$0			# make a stack frame again, but terminate chain here
					# - because we don't want an exception handler searching user stack frames
	movl	%esp,%ebp		# now point to my kernel stack frame

	call	oz_knl_thread_getcur	# get pointer to current thread struct
	pushl	%eax
	call	oz_knl_thread_gethwctx	# get corresponding hardware context
	movl	%eax,%edi

	pushl	THCTX_L_USTKVPG(%edi)	# unmap the old section
	call	oz_hw486_ustack_delete

	movl	16(%ebp),%eax		# get caller's (user-mode) stack pointer
	shrl	$12,%eax
	movl	%eax,THCTX_L_USTKVPG(%edi)

	movl	oz_SUCCESS,%eax		# always successful
	leave
	iret

##########################################################################
##									##
##  Terminate as much as possible about the thread while still in its 	##
##  context								##
##									##
##    Input:								##
##									##
##	4(%esp) = pointer to thread hardware context block		##
##									##
##	smplock = softint delivery inhibited				##
##									##
##    Output:								##
##									##
##	everything possible cleared out while in thread context		##
##									##
##########################################################################

	.align	4
	.global	oz_hw_thread_exited
	.type	oz_hw_thread_exited,@function
oz_hw_thread_exited:
	pushl	%ebp
	movl	%esp,%ebp

## Unmap and delete the user stack section, oz_hw_thread_termctx deletes executive stack

	movl	8(%ebp),%ecx			# point to hardware context block
	cmpl	$0,THCTX_L_USTKSIZ(%ecx)	# see if a user stack was created
	je	thread_exited_rtn		# all done if there wasn't one created
	pushl	THCTX_L_USTKVPG(%ecx)		# ok, unmap the section
	movl	$0,THCTX_L_USTKSIZ(%ecx)
	movl	$0,THCTX_L_USTKVPG(%ecx)
	call	oz_hw486_ustack_delete
thread_exited_rtn:

	leave
	ret

##########################################################################
##									##
##  Terminate thread hardware context					##
##									##
##    Input:								##
##									##
##	4(%esp) = pointer to thread hardware context block		##
##	8(%esp) = pointer to process hardware context block		##
##	smplevel <= ts							##
##									##
##    Output:								##
##									##
##	@4(%esp) = voided out						##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_thread_termctx
	.type	oz_hw_thread_termctx,@function
oz_hw_thread_termctx:
	pushl	%ebp
	movl	%esp,%ebp

##  Free memory used for its executive stack
##  If ESTACKVA is zero, it means this is the cpu's initial thread (?? should we free it to non-paged pool ??)

	movl	8(%ebp),%edx			# point to thread's hw context block
	movl	THCTX_L_ESTACKVA(%edx),%ecx	# get top address of executive stack
	jecxz	thread_termctx_rtn
	movl	$0,THCTX_L_ESTACKVA(%edx)	# clear it out now that we're freeing it (paranoia)
	pushl	%ecx				# free off the memory pages and the spte's
	pushl	%edx
	call	oz_hw486_kstack_delete
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
	.type	oz_hw_thread_switchctx,@function
oz_hw_thread_switchctx:

	# save general registers
	# if you change the order pushed below, you must change THSAV_... symbols

	pushl	%ebp				# make a stack frame
	movl	%esp,%ebp
	pushfl					# save eflags (in case IOPL is different)
	pushl	%edi				# save C registers
	pushl	%esi
	pushl	%ebx

	call	oz_hw486_getcpu			# this is safe to do because we are at or above softint
.if EXECODESEG == KNLCODESEG
	pushl	CPU_X_TSS+TSS_L_ESP0(%esi)	# save the tss' kernel stack pointer
.else
	pushl	CPU_X_TSS+TSS_L_ESP1(%esi)	# save the tss' executive stack pointer
						# - the kernel stack is only used as a last-chance crash 
						#   handler and therefore there is not switched per task
.endif

	# %esp now points to old THSAV_...

	movl	 8(%ebp),%edi			# get old thread's hardware context block
	movl	12(%ebp),%ebx			# get new thread's hardware context block

		####	pushl	%ebx
		####	pushl	$thread_switchctx_deb1
		####	call	oz_knl_printk
		####	pushl	THCTX_L_EXESP(%ebx)
		####	pushl	$THSAV__SIZE
		####	call	oz_knl_dumpmem
		####	addl	$16,%esp

	# save fpu/mmx context

	movl	%edi,%eax
	btr	$0,THCTX_L_FPUSED(%edi)		# see if old thread accessed its fp regs this last time around
	jnc	thread_switchctx_nofpsave	# if not, don't bother (TS bit should be set)
	andb	$0xF0,%al			# fxsave requires 16-byte alignment
fnsave_pat:			# fnsave/nop gets patched to fxsave if cpu is mmx
	fnsave	THCTX_X_FPSAVE+16(%eax)		# if so, save fp registers and re-init fpu (TS bit should be clear)
	nop
	SETTS					# set the TS bit to inhibit access to fpu
thread_switchctx_nofpsave:

	# make some basic checks before switching

.if 0000
	pushl	$1				# make sure some of the new stack is writable
	pushl	oz_PROCMODE_KNL
	pushl	THCTX_L_EXESP(%ebx)
	subl	$32,(%esp)
	pushl	$32+THSAV__SIZE
	call	oz_hw_probe
	testl	%eax,%eax
	je	thread_switchctx_crash3
	addl	$16,%esp

	movl	THCTX_L_EXESP(%ebx),%eax	# make sure some of the new instruction stream is readable
	pushl	$0
	pushl	oz_PROCMODE_KNL
	pushl	THSAV_L_EIP(%eax)
	pushl	$1
	call	oz_hw_probe
	testl	%eax,%eax
	je	thread_switchctx_crash2
	addl	$16,%esp
.endif

	# switch stacks

	movl	%esp,THCTX_L_EXESP(%edi)	# save current executive stack pointer
	movl	THCTX_L_EXESP(%ebx),%esp	# we're now on the new thread's executive stack

	# %esp now points to new THSAV_...

	cmpl	%edi,CPU_L_THCTX(%esi)		# make sure old pointer is ok
	jne	thread_switchctx_crash1
	movl	%ebx,CPU_L_THCTX(%esi)		# save new thread context pointer

	movl	$0x96696996,THCTX_L_EXESP(%ebx)	# wipe it until we save it again

	# restore general registers
	# if you change the order popped below, you must change THSAV_... symbols

.if EXECODESEG == KNLCODESEG
thread_switchctx_wipebeg:			# gets nop'd if we don't support sysenter/sysexit
	movl	(%esp),%eax			# set up the esp that will be used for sysenter instruction
	xorl	%edx,%edx			# (= same as will be used for any user to kernel mode switch)
	movl	$SYSENTER_ESP_MSR,%ecx
	wrmsr
thread_switchctx_wipeend:			# end of what gets nop'd
	popl	CPU_X_TSS+TSS_L_ESP0(%esi)	# set up the esp that will be used when switching from user to kernel mode
.else
	popl	CPU_X_TSS+TSS_L_ESP1(%esi)	# set up the esp that will be used when switching from user to executive mode
.endif
	popl	%ebx				# restore C registers
	popl	%esi
	popl	%edi
	popfl					# restore eflags (in case IOPL is different)
						# (only works if running in true kernel mode)
	popl	%ebp				# restore new thread's frame pointer
	ret					# return to new thread

thread_switchctx_crash3:
	pushl	THCTX_L_EXESP(%ebx)
	pushl	$thread_switchctx_msg3
	call	oz_crash

thread_switchctx_crash2:
	movl	THCTX_L_EXESP(%ebx),%eax
	pushl	THSAV_L_EIP(%eax)
	pushl	$thread_switchctx_msg2
	call	oz_crash

thread_switchctx_crash1:
	pushl	%ebx
	pushl	%edi
	pushl	CPU_L_THCTX(%esi)
	pushl	$thread_switchctx_msg1
	call	oz_crash

thread_switchctx_msg1:	.string	"oz_hw_thread_switchctx: old CPU_L_THCTX %p, old hwctx %p, new hwctx %p"
thread_switchctx_msg2:	.string	"oz_hw_thread_switchctx: new instrs %p not readable"
thread_switchctx_msg3:	.string	"oz_hw_thread_switchctx: new stack %p not writable"

thread_switchctx_deb1:	.string	"oz_hw_thread_switchctx*: new hwctx %p\n"

##########################################################################
##									##
##  Set thread ast state						##
##									##
##  It's a nop for us because we poll on return to outer modes		##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_thread_aststate
	.type	oz_hw_thread_aststate,@function
oz_hw_thread_aststate:
	ret

##########################################################################
##									##
##  Thread Trace Dump routine						##
##									##
##    Input:								##
##									##
##	4(%esp) = pointer to hwctx block				##
##	smplock = ts							##
##	threadstate not currently loaded in any cpu			##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_thread_tracedump
	.type	oz_hw_thread_tracedump,@function
oz_hw_thread_tracedump:
	pushl	%ebp
	movl	%esp,%ebp
	pushl	%ebx

.if THSAV_L_EIP<>THSAV_L_EBP+4
	error : code assumes THSAV_L_EIP comes right after THSAV_L_EBP
.endif
	movl	8(%ebp),%ebx			# point to THCTX block
	movl	THCTX_L_EXESP(%ebx),%eax	# get thread's stack pointer
	leal	THSAV_L_EBP(%eax),%ebx		# get thread's frame pointer
	pushl	%ebx				# print stack and frame pointer
	pushl	%eax
	pushl	$thread_tracedump_msg1
	call	oz_knl_printk
	addl	$12,%esp
thread_tracedump_loop:
	pushl	$1				# 8 bytes at its ebp should be writable
	pushl	$0
	pushl	%ebx
	pushl	$8
	call	oz_hw_probe
	addl	$16,%esp
	testl	%eax,%eax
	je	thread_tracedump_done		# if not, we're all done
	pushl	%ebx				# ok, print out the frame
	pushl	 (%ebx)
	pushl	4(%ebx)
	pushl	$thread_tracedump_msg2
	call	oz_knl_printk
	pushl	4(%ebx)
	call	oz_knl_printkaddr
	addl	$20,%esp
	movl	(%ebx),%ebx			# check out next frame
	jmp	thread_tracedump_loop
thread_tracedump_done:
	pushl	$thread_tracedump_msg3		# all done
	call	oz_knl_printk
	addl	$4,%esp

	popl	%ebx
	leave
	ret

thread_tracedump_msg1:	.string	"oz_hw_thread_tracedump: esp %X, ebp %X"
thread_tracedump_msg2:	.string	"\n   %8.8X  %8.8X : %8.8X : "
thread_tracedump_msg3:	.string	"\n"

##########################################################################
##									##
##  Pagetable processing						##
##									##
##########################################################################

	# calculate address of pte that maps the first pagetable page of the process
	# this should come out to be 0xFFFFC118 (given process base of 0x20000000 and mpd at 0xFFFFE000)

page_of_pte_that_maps_first_pt_page = (((((PROCPTRPBVPAGE - PROCBASVPAGE) / 1024) * 17) / 16) + PROCPTRPBVPAGE) * 4096
pte_that_maps_first_pt_page = page_of_pte_that_maps_first_pt_page + (((PROCPTRPBVPAGE - PROCBASVPAGE) & 1023) * 4)

	# calculate address of the MPD entry corresponding to process virtual addresses
	# this should come out to be 0xFFFFE200

mpde_for_first_proc_page = PROCMPDVADDR + (PROCBASVPAGE / 1024) * 4

##########################################################################
##									##
##  Init non-paged pool memory						##
##									##
##    Input:								##
##									##
##	 4(%esp) = pointer to number of pages required			##
##	 8(%esp) = pointer to first physical page			##
##	12(%esp) = pointer to first virtual page			##
##	oz_s_phymem_totalpages = total number of physical pages		##
##									##
##    Output:								##
##									##
##	 *4(%esp) = actual number of pages allocated			##
##	 *8(%esp) = actual base physical page				##
##	*12(%esp) = actual base virtual page				##
##									##
##    Note:								##
##									##
##	This routine sets up 4Meg descriptors for the pool pages.  It 	##
##	also sets up the corresponding SPTE's to look like they are 	##
##	mapping the pages, so the pte_read and probe routines don't 	##
##	have to do anything special.					##
##									##
##	The OS may read the pte's but it will never write them.  So we 	##
##	write-protect the fake SPT pages to make sure it doesn't try.	##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_pool_init
	.type	oz_hw_pool_init,@function
oz_hw_pool_init:
	pushl	%ebp
	movl	%esp,%ebp
	pushl	%ebx

	movl	 8(%ebp),%eax					# get number of pages wanted
	movl	(%eax),%ebx
	addl	$0x000003FF,%ebx				# round up to next 4Meg boundary
	andl	$0xFFFFFC00,%ebx
	movl	%ebx,(%eax)					# return to caller
	movl	oz_s_phymem_totalpages,%eax			# point to end of physical memory
	subl	%ebx,%eax					# get first physical page we will use
	movl	12(%ebp),%ecx					# return it to caller
	movl	%eax,(%ecx)
	movl	$PROCBASVPAGE-0x400,%edx			# point to end of system virtual memory plus leave a 4M 
								# ... hole as a guard between system and process space
	subl	%ebx,%edx					# get first virtual page we will use
	movl	16(%ebp),%eax
	movl	%edx,(%eax)

	MOVL_CR0_EAX						# clear the WP bit so the MPD will be writable
	pushl	%eax
	andl	$0xFFFEFFFF,%eax
	MOVL_EAX_CR0

	movl	(%ecx),%eax					# get first physical page again
	shrl	$10,%edx					# get MPD index for the virtual addresses
	shll	$12,%eax					# get starting physical address
	orl	pte_validw_gbl_krw,%eax				# put in PT_KRW, OZ_SECTION_PAGESTATE_VALID_W and maybe GLOBAL bit
	shrl	$10,%ebx					# get number of 4Meg pages to set up
pool_init_outerloop:
	movl	%eax,MPDBASE(,%edx,4)				# set up the 4Meg entry
	orb	$PD_4M,MPDBASE(,%edx,4)

	addb	$PT_URO-PT_KRW,SPTPAGE*4+SPTBASE(,%edx,4)	# write protect the fake spt page we are going to set up

	shll	$10,%edx					# get corresponding spt index

	pushl	%edx
	leal	SPTBASE(,%edx,4),%edx				# flush out the old read/write spte for the page
	INVLPG_EDX						# (we can still write it though because WP bit is off for now)
	popl	%edx

	movl	$1024,%ecx					# fill the corresponding spt page so pte_read and 
								# ... probe don't have to do anything special
pool_init_innerloop:
	movl	%eax,SPTBASE(,%edx,4)
	addl	$0x1000,%eax
	incl	%edx
	loop	pool_init_innerloop

	shrl	$10,%edx					# get mpd index back

	decl	%ebx						# repeat for more 4M pages to set up
	jne	pool_init_outerloop

	popl	%eax						# turn WP back on to write protect the MPD
	MOVL_EAX_CR0

	popl	%ebx
	leave
	ret

##########################################################################
##									##
##  Read a pte of the current active process				##
##									##
##    Input:								##
##									##
##	4(%esp) = current process' virtual page number of pte to read	##
##									##
##    Output:								##
##									##
##	%eax = 0 : successful						##
##	    else : vaddr that needs to be faulted in			##
##	 @8(%esp) = software page state					##
##	@12(%esp) = physical page number				##
##	@16(%esp) = current page protection				##
##	@20(%esp) = requested page protection				##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_pte_readsys		# - read system pte, crash if not valid
	.type	oz_hw_pte_readsys,@function
oz_hw_pte_readsys:
	pushl	%ebp				# make stack frame for trace
	movl	%esp,%ebp
	pushl	%ebx
	jmp	pte_read_system			# go read system pte

	.align	4
	.globl	oz_hw_pte_readany		# - read any pte, return pte's va if not valid
	.type	oz_hw_pte_readany,@function
oz_hw_pte_readany:
	pushl	%ebp				# make stack frame for trace
	movl	%esp,%ebp
	pushl	%ebx

	movl	8(%ebp),%ebx			# get vpn of page to read
	subl	$PROCBASVPAGE,%ebx		# see if it is a per-process page
	jc	pte_read_system

## Reading a per-process pagetable entry

	movl	24(%ebp),%ecx			# see if they want requested protection
	jecxz	pte_read_process_noreqprot
	movl	%ebx,%eax			# point to beginning of page that has requested protection in it
	shrl	$14,%eax
	movl	%eax,%edx
	shll	$4,%eax
	addl	%edx,%eax
	addl	$PROCPTRPBVPAGE+16,%eax
	shll	$12,%eax
	call	pte_read_iseaxreadable		# make sure the page is paged in
	movl	%ebx,%edx
	jnc	pte_read_process_rtn		# if not, tell caller they need to fault it in
	andl	$0x3FFC,%edx			# ok, get the byte within the page that we want
	shrl	$2,%edx
	movb	(%eax,%edx,1),%al
	movb	%bl,%cl				# see which two bits within that byte we should get
	andb	$3,%cl
	shrb	%cl,%al				# shift to get those two bits
	shrb	%cl,%al
	movl	24(%ebp),%edx			# return the two bits to caller
	andb	$3,%al
	movb	%al,(%edx)
pte_read_process_noreqprot:
	movl	12(%ebp),%eax			# see if caller wants anything from hw pte
	orl	16(%ebp),%eax
	orl	20(%ebp),%eax
	je	pte_read_process_rtn		# return with success status in %eax if not
	movl	%ebx,%eax			# point to page that contains the pte
	shrl	$10,%eax
	movl	%eax,%ecx
	shrl	$4,%eax
	addl	%ecx,%eax
	andl	$0x3FF,%ebx			# point to longword within that page
	shll	$12,%eax
	leal	PROCPTRPBVADDR(%eax,%ebx,4),%eax
	call	pte_read_iseaxreadable		# make sure it is readable
	jnc	pte_read_process_rtn
	movl	(%eax),%eax			# ok, read it in
	jmp	pte_read_rtnvalues		# ... then go return values from it
pte_read_process_rtn:
	popl	%ebx
	leave
	ret

## Reading a system pte - since the spt is always all paged in, we don't have to check accessibilities

pte_read_system:
	movl	 8(%ebp),%ebx			# get vpn of system page to read = index in spt
	cmpl	oz_s_sysmem_pagtblsz,%ebx	# make sure desired entry is in range
	ja	pte_read_crash1
	movl	24(%ebp),%edx			# see if they want reqprot
	testl	%edx,%edx
	je	pte_read_system_noreqprot
	movl	%ebx,%eax			# if so, get the byte with the bits we want
	shrl	$2,%eax
	addl	srp_table,%eax
	movb	(%eax),%al
	movb	%bl,%cl				# see which two bits within that byte we should get
	andb	$3,%cl
	shrb	%cl,%al				# shift to get those two bits
	shrb	%cl,%al
	andb	$3,%al				# return the two bits to caller
	movb	%al,(%edx)
pte_read_system_noreqprot:
	movl	SPTBASE(,%ebx,4),%eax		# read spte corresponding to the virtual page

## Pte contents in %eax, return requested values

pte_read_rtnvalues:
	movl	20(%ebp),%ecx			# see if they want curprot
	jecxz	pte_read_nocurprot
	movb	%al,%dl				# if so, extract from pte<01:02>
	shrb	$1,%dl				# 00=NA, 01=KW, 10=UR, 11=UW
	andb	$3,%dl
	movb	%dl,(%ecx)
pte_read_nocurprot:
	movl	16(%ebp),%ecx			# see if they want phypage
	jecxz	pte_read_nophypage
	movl	%eax,%edx			# if so, extract from pte<12:31>
	shrl	$12,%edx
	movl	%edx,(%ecx)
pte_read_nophypage:
	movl	12(%ebp),%ecx			# see if they want pagestate
	jecxz	pte_read_nopagestate
	shrl	$9,%eax				# if so, extract from pte<09:11>
	andl	$7,%eax
	movl	%eax,(%ecx)
pte_read_nopagestate:
	xorl	%eax,%eax			# successful
	popl	%ebx
	leave
	ret

pte_read_crash1:
	pushl	%ebx
	pushl	$pte_read_msg1
	call	oz_crash

# Determine if %eax points to a readable virtual address
# Assume %eax is a per-process address (ie, .ge. PROCBASVADDR)
# Return c-set if readable, c-clear if not readable
# Uses %ecx for scratch, all others preserved

pte_read_iseaxreadable:
	pushl	%eax				# save original virtual address
	shrl	$12,%eax			# get the corresponding vpage

	movl	%eax,%ecx			# make a copy of the vpage number
	shrl	$10,%ecx			# get index into mpd corresponding to that vpage
	bt	$0,PROCMPDVADDR(,%ecx,4)	# see if the page is present
	jnc	pte_read_eaxnotreadable		# if not, the %eax location is not readable
						# otherwise, assume it is readable by the kernel

	subl	$PROCBASVPAGE,%eax		# mpd entry is readable, get index into ppt for the vpage
	jc	pte_read_crash2
	movl	%eax,%ecx			# (have to skip over the reqprot pages)
	shrl	$14,%ecx			# (there is one for every 16k virtual pages)
	shll	$12,%ecx			# (and the reqprot pages are 4kbytes each)
	bt	$0,PROCPTRPBVADDR(%ecx,%eax,4)	# test to see if the page is present
						# if not, the %eax location is not readable
						# otherwise, assume it is readable by the kernel

pte_read_eaxnotreadable:
	popl	%eax				# restore original %eax
	ret					# return with carry bit set accordingly

pte_read_crash2:
	addl	$PROCBASVPAGE,%eax
	pushl	%eax
	pushl	$pte_read_msg2
	call	oz_crash

pte_read_msg1:	.string	"oz_hw_pte_read: system page %x beyond end of spt"
pte_read_msg2:	.string	"oz_hw_pte_read: virt page %x not in process"

##########################################################################
##									##
##  Write pte of process active on this cpu				##
##									##
##    Input:								##
##									##
##	 4(%esp) = virtual page number corresponding to pte		##
##	 8(%esp) = software page state					##
##	12(%esp) = physical page number					##
##	16(%esp) = current protection to set				##
##	20(%esp) = requested protection to set				##
##									##
##    Output:								##
##									##
##	pte written, cache entry invalidated				##
##									##
##########################################################################

## This one just invalidates the cache entry on the current cpu - it is used only for upgrades in protection 
## and then only where the other cpu's can recover from a pagefault if they have the old value cached

	.align	4
	.globl	oz_hw_pte_writecur
	.type	oz_hw_pte_writecur,@function
oz_hw_pte_writecur:
	pushl	%ebp				# make stack frame for trace
	movl	%esp,%ebp
	pushl	%esi
	pushl	%ebx
	call	pte_write			# write the pte
	movl	8(%ebp),%edx			# get vpage again
	popl	%ebx
	shll	$12,%edx			# get corresponding virt address
	popl	%esi
	INVLPG_EDX				# invalidate pte on this cpu
	leave
	ret

## This one invalidates the entry on all cpus - it is used under all other circumstances

	.align	4
	.globl	oz_hw_pte_writeall
	.type	oz_hw_pte_writeall,@function
oz_hw_pte_writeall:
	pushl	%ebp				# make stack frame for trace
	movl	%esp,%ebp
	pushl	%esi
	pushl	%ebx
	call	pte_write			# write the pte
	movl	8(%ebp),%edx			# get vpage again
	shll	$12,%edx			# get corresponding virt address
	INVLPG_EDX				# invalidate pte on this cpu
	call	oz_hw486_apic_invlpgedx		# invalidate it on other cpu's, too
	popl	%ebx
	popl	%esi
	leave
	ret

##  Update the pte in memory

	.align	4
pte_write:
	movl	20(%ebp),%ecx			# see what protection they're setting
	jecxz	pte_write_noaccess
	movl	16(%ebp),%eax			# if accessible, don't allow access (dual mapping) to static pages
	cmpl	oz_hw486_dynphypagbeg,%eax
	jc	pte_write_crash2
pte_write_noaccess:
	movl	8(%ebp),%ebx			# get vpn of page to write
	subl	$PROCBASVPAGE,%ebx		# see if it is a per-process page
	jc	pte_write_system

##  Writing a process pte

	movl	%ebx,%eax			# point to beginning of page that has requested protection in it
	shrl	$14,%eax
	movl	%eax,%edx
	shll	$4,%eax
	addl	%edx,%eax
	shll	$12,%eax
	movl	%ebx,%edx
	leal	PROCPTRPBVADDR+16*4096(,%eax,1),%eax
	andl	$0x3FFC,%edx			# point to the byte within the page that we want
	shrl	$2,%edx
	addl	%eax,%edx
	movb	%bl,%cl				# see which two bits within that byte we should get
	andb	$3,%cl
	pushl	%ebx
	shlb	$1,%cl
	movb	(%edx),%al			# get byte contents
pte_write_reqprot_loop:
	movb	%al,%bh				# work on contents in %bh
	shrw	%cl,%bx				# shift to get those two bits, saving lower bits in %bl
	andb	$0xFC,%bh			# take out the old bits
	orb	24(%ebp),%bh			# put in the new bits
	shlw	%cl,%bx				# shift back in place, restoring lower bits from %bl
	lock					# in case another processor is writing this byte, too
	cmpxchgb %bh,(%edx)			# if (%edx) .eq. %al, then store %bh in (%edx), else store (%edx) in %al
	jne	pte_write_reqprot_loop		# repeat if failure (someone else modified byte on us)
	popl	%ebx				# restore vpage offset in page table

	movl	%ebx,%eax			# point to page that contains the pte
	shrl	$10,%eax
	movl	%eax,%ecx
	shrl	$4,%eax
	addl	%ecx,%eax
	andl	$0x3FF,%ebx			# point to longword within that page
	shll	$12,%eax
	leal	PROCPTRPBVADDR(%eax,%ebx,4),%esi

	xorl	%ebx,%ebx			# start with a blank pte

	movzbl	20(%ebp),%ecx			# get new curprot
	jecxz	pte_write_proc_nocurprot	# zero means no access, so don't bother with it
	shll	$1,%ecx				# non-zero, shift in place and set low bit
	orl	$1,%ecx
	orl	%ecx,%ebx			# ... then store on stack with possible G bit
pte_write_proc_nocurprot:

	movl	16(%ebp),%ecx			# get new phypage
	shll	$12,%ecx			# shift in place
	orl	%ecx,%ebx			# store in pte

	movzbl	12(%ebp),%ecx			# get new pagestate
	shll	$9,%ecx				# shift in place
	orl	%ecx,%ebx			# store in pte

		####	pushl	%esi
		####	pushl	%ebx
		####	pushl	$pte_write_deb1
		####	call	oz_knl_printk
		####	addl	$12,%esp

	movl	%ebx,(%esi)			# store the new pte value

	subl	$pte_that_maps_first_pt_page,%esi # see if we just updated a pointer to a pagetable page
	jnc	pte_write_proc_ptpage		# ie, caller just faulted in/out pagetable page

	ret					# if not, we're done as is

##  Writing a system pte - the system pagetable and reqprot tables are not interleaved like the process ones are

pte_write_system:
	addl	$PROCBASVPAGE,%ebx		# get the page within the system page table
	cmpl	oz_s_sysmem_pagtblsz,%ebx	# make sure page number is within range
	jnc	pte_write_crash1
	movl	%ebx,%esi			# point to reqprot byte
	shrl	$2,%esi
	addl	srp_table,%esi
	movb	%bl,%cl
	andb	$3,%cl
	shlb	$1,%cl
	movb	(%esi),%al			# get initial contents of byte in %al
pte_write_sys_reqprot_loop:
	movb	%al,%dh				# work on contents in %dh
	shrw	%cl,%dx				# shift to get those two bits, saving lower bits in %dl
	andb	$0xFC,%dh			# take out the old bits
	orb	24(%ebp),%dh			# put in the new bits
	shlw	%cl,%dx				# shift back in place, restoring lower bits from %dl
	lock					# in case another processor is writing this byte, too
	cmpxchgb %dh,(%esi)			# if (%esi) .eq. %al, then store %dh in (%esi), else store (%esi) in %al
	jne	pte_write_reqprot_loop		# repeat if failure (someone else modified byte on us)

	leal	SPTBASE(,%ebx,4),%esi		# point to the spte

	movl	pte_gbl,%ebx			# start with a pte that has the G-bit set

	movzbl	20(%ebp),%ecx			# get new curprot
	jecxz	pte_write_sys_nocurprot		# zero means no access, so don't bother with it
	shll	$1,%ecx				# non-zero, shift in place and set low bit
	orl	$1,%ecx
	orl	%ecx,%ebx			# ... then store in pte
pte_write_sys_nocurprot:

	movl	16(%ebp),%ecx			# get new phypage
	shll	$12,%ecx			# shift in place
	orl	%ecx,%ebx			# store in pte

	movzbl	12(%ebp),%ecx			# get new pagestate
	shll	$9,%ecx				# shift in place
	orl	%ecx,%ebx			# store in pte

	movl	%ebx,(%esi)			# store the new pte value

	ret					# we're done

pte_write_crash1:
	pushl	%ebx
	pushl	$pte_write_msg1
	call	oz_crash
pte_write_crash2:
	pushl	%ecx
	pushl	%eax
	pushl	$pte_write_msg2
	call	oz_crash

## We probably just updated a pagetable's pte (ie, the virt address mapped by 
## the pte probably is a pagetable page), update the corresponding MPD entry

##  %ebx = contents of the pte that was just written
##  %esi = address of pte that was just written

pte_write_proc_ptpage:
	movl	%esi,%eax			# we have to splice out reqprot pages
	xorl	%edx,%edx
	movl	$17*4,%ecx
	divl	%ecx
	cmpl	$16*4,%edx			# see if it was a reqprot page and not a pagetable page that was faulted
	je	pte_write_rtn			# if so, don't update mpd
	shll	$4+2,%eax			# ok, get mpd offset put back together with reqprot entries spliced out
	addl	%edx,%eax
	orb	$6,%bl				# mpd entries always are kernel and user writable, leave P (present) bit as caller wants

		####	pushl	%eax
		####	leal	mpde_for_first_proc_page(%eax),%eax
		####	pushl	%eax
		####	pushl	%ebx
		####	pushl	$pte_write_deb2
		####	call	oz_knl_printk
		####	addl	$12,%esp
		####	popl	%eax

	movl	%ebx,mpde_for_first_proc_page(%eax) # write the corresponding MPD entry
						# note that if %eax is out-of-range, it should barf trying to write the FFFFF000 page
.if PARANOID_MPD
	movl	%eax,%ecx			# also update our active copy
	call	oz_hw486_getcpu
	addl	CPU_L_MPDBASE(%esi),%ecx
	movl	%ebx,PROCBASVPAGE/256(%ecx)
.endif
	INVLPG_ALLNG				# invalidate everything (except system pages which should all have the G bit set)
						# ?? don't know if this is necessary, maybe simple invalidate page also invalidates corresponding MPD entry
						# ?? the book weakly implies that it invalidates the MPD entry when the PT entry is invalidated via INVLPG 
						# ?? but we do this just to be sure
pte_write_rtn:
	ret

pte_write_msg1:	.string	"oz_hw_pte_write: vpage 0x%X beyond end of spt"
pte_write_msg2:	.string	"oz_hw_pte_write: attempting to map static page 0x%X prot %u"

	####	pte_write_deb1:	.string	"oz_hw_pte_write*: writing pte %X to %X\n"
	####	pte_write_deb2:	.string	"oz_hw_pte_write*: writing mpde %X to %X\n"

##########################################################################
##									##
##  A page is about to be paged out that may be part of a pagetable	##
##  Check it to see if all entries it contains are paged out		##
##									##
##    Input:								##
##									##
##	 4(%esp) = vpage of possible pagetable page			##
##	 8(%esp) = 0 : it is being paged out, just make sure all its 	##
##	               pages are paged out (pagestate PAGEDOUT, curprot NA)
##	           1 : it is being unmapped, make sure all the pages 	##
##	               are unmapped 					##
##	               (pagestate PAGEDOUT/READFAILED/WRITEFAILED, 	##
##	                curprot NA, phypage 0)				##
##									##
##    Output:								##
##									##
##	%eax = 0 : it is not a pagetable page 				##
##	           or all its pages are out / unmapped			##
##	    else : this particular vpage is paged in			##
##									##
##    Note:								##
##									##
##	The kernel only calls this routine for sections marked PAGTBL, 	##
##	so this should crash for any non-per-process pt/rp pages.	##
##	But the oz_sys_pdata_array is part of that section for us, so 	##
##	don't crash for those pages.					##
##									##
##	The spt is just a plain section and should never be paged out.	##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_pte_checkptpage
	.type	oz_hw_pte_checkptpage,@function
oz_hw_pte_checkptpage:
	pushl	%ebp
	movl	%esp,%ebp
	pushl	%ebx
	movl	8(%ebp),%eax				# get vpage of the ptpage to check out
	cmpl	$PROCMPDVPAGE,%eax			# if it is at or after the process' mpd page, can't be a pt page
	jnc	pte_checkptpage_crash
	subl	$PROCPTRPBVPAGE,%eax
	jc	pte_checkptpage_pdata
	xorl	%edx,%edx				# see which 17-page pt/rp page group it is in
	movl	$17,%ecx
	divl	%ecx
	cmpl	$16,%edx				# if it is the rp page within those 17, can't be a pt page
	je	pte_checkptpage_none
	shll	$4,%eax					# not the rp page, must be a pt page
	addl	%edx,%eax				# so get the pt page number (don't count the interleaved rp pages)
	addl	$PROCBASVPAGE/1024,%eax			# add the offset for the beginning of the per-process pagetable
pte_checkptpage:
	shll	$10,%eax				# get the vpage number mapped by the first entry of the pt page
	movl	$1024,%ecx				# there are 1024 entries in the pt page to scan
	movl	8(%ebp),%edx				# get pointer to beginning of pt page to scan
	shll	$12,%edx
	movl	$0xE01,%ebx				# always make sure pagestate=0 (PAGESTATE_PAGEDOUT), present=0 (PAGEPROT_NA)
	bt	$0,12(%ebp)
	jnc	pte_checkptpage_loop
	movl	$0xFFFFFE01,%ebx			# unmapping, also make sure phypage=0
pte_checkptpage_loop:
	testl	%ebx,(%edx)				# see if page is paged out
	jne	pte_checkptpage_done			# if not, tell caller they have to page out the %eax vpage
	addl	$4,%edx					# it is paged out, increment pointer
	incl	%eax					# increment vpage number
	loop	pte_checkptpage_loop			# repeat if there are more to check
pte_checkptpage_none:
	xorl	%eax,%eax				# they are all paged out (or it's not a pt page), return 0
pte_checkptpage_done:
	popl	%ebx
	leave
	ret

pte_checkptpage_pdata:
	addl	$2,%eax					# maybe it's one of the two oz_sys_pdata_array pages
	jc	pte_checkptpage_none			# just tell caller it's not really a pt page
pte_checkptpage_crash:
	pushl	8(%ebp)					# shouldn't have called us for this page at all, puque
	pushl	$pte_checkptpage_msg
	call	oz_crash

pte_checkptpage_msg:	.string	"oz_hw_pte_checkptpage: non per-process pt page %x"

##########################################################################
##									##
##  Prints the pte for virtual address in 4(%esp) or %eax		##
##									##
##########################################################################

pte_print_msg1:	.string	"oz_hw_pte_print: vaddr %8.8X\n"
pte_print_msg2:	.string	"                   cr3 %8.8X\n"
pte_print_msg3:	.string	"                   pde %8.8X"
pte_print_msg4:	.string	": %8.8X\n"
pte_print_msg5:	.string	"                   pte %8.8X"

	.align	4
	.globl	oz_hw_pte_print
	.type	oz_hw_pte_print,@function
oz_hw_pte_print:
	movl	4(%esp),%eax		# get virtual address from call arg
pte_print:
	pushl	%ebp			# we might get a fault in here, so set up a stack frame to get a trace
	movl	%esp,%ebp
	pushl	%edi			# save C registers
	pushl	%esi
	pushl	%ebx
	pushfl				# make sure any abort really aborts
	cli				# ... by inhibiting interrupt delivery
	pushl	%eax			# save vaddr we care about
	pushl	%eax			# print out the vaddr
	pushl	$pte_print_msg1
	call	oz_knl_printk
	addl	$8,%esp
	MOVL_CR3_EAX			# print out cr3
	pushl	%eax
	pushl	%eax
	pushl	$pte_print_msg2
	call	oz_knl_printk
	addl	$8,%esp
	popl	%eax			# get mpd base physical address
	andl	$0xFFFFF000,%eax	# and off the stuff we don't care about here
	movl	(%esp),%ebx		# get virt address in question
	shrl	$22,%ebx		# get mpd index
	leal	(%eax,%ebx,4),%edi	# get physical address of mpd entry
	pushl	%edi			# save it
	pushl	%edi			# print it
	pushl	$pte_print_msg3
	call	oz_knl_printk
	addl	$8,%esp
	popl	%edi			# read the mpd entry
	call	fetch_phys_long
	pushl	%eax			# save it on stack
	pushl	%eax			# print it out
	pushl	$pte_print_msg4
	call	oz_knl_printk
	addl	$8,%esp
	popl	%eax			# get phys address of pte page
	popl	%ebx			# get virt address in question
	bt	$0,%eax			# test the mpd's pt page present bit
	jnc	pte_print_done		# all done if not set
	andl	$0xFFFFF000,%eax	# and off the stuff we don't care about here
	andl	$0x003FF000,%ebx	# get index into pt page
	shrl	$12,%ebx
	leal	(%eax,%ebx,4),%edi	# get physical address of pt entry
	pushl	%edi			# save it on stack
	pushl	%edi			# print it out
	pushl	$pte_print_msg5
	call	oz_knl_printk
	addl	$8,%esp
	popl	%edi			# read the pt entry
	call	fetch_phys_long
	pushl	%eax			# print it out
	pushl	$pte_print_msg4
	call	oz_knl_printk
	addl	$8,%esp
pte_print_done:
	popfl
	popl	%ebx
	popl	%esi
	popl	%edi
	popl	%ebp
	ret

##########################################################################
##									##
##  Determine if a particular null terminated string is readable 	##
##  in the current memory mapping by the given processor mode 		##
##									##
##    Input:								##
##									##
##	 4(%esp) = address of area					##
##	 8(%esp) = processor mode					##
##									##
##    Output:								##
##									##
##	%eax = 0 : string is not accessible				##
##	       1 : string is accessible					##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_prober_strz
	.type	oz_hw_prober_strz,@function
oz_hw_prober_strz:
	pushl	%ebp			# make a call frame
	movl	%esp,%ebp
	movl	 8(%ebp),%eax		# get address of null terminated string
	movl	12(%ebp),%edx		# get processor mode to check it for
	pushl	$0			# check for read access
	pushl	%ebx			# processor mode
	pushl	%edx			# address
	pushl	$1			# just check the first byte
	call	oz_hw_probe		# check out the first byte of the string
	addl	$16,%esp		# wipe call args
	bt	$0,%eax			# see if the first byte is readable
	jnc	prober_strz_done	# if not, we're all done
	movl	8(%ebp),%edx		# if so, point to first byte of string
prober_strz_loop:
	movb	(%edx),%cl		# read the byte
	orb	%cl,%cl			# check for null terminator
	je	prober_strz_done	# if null, return with 1 still in %eax
	incl	%edx			# not null, increment to next byte
	testw	$0x0fff,%dx		# see if still in same memory page
	jne	prober_strz_loop	# if so, repeat without probing
	movl	%edx,8(%ebp)		# new page, change string pointer
	jmp	oz_hw_prober_strz	# repeat until error or a null
prober_strz_done:
	leave				# pop call frame
	ret

##########################################################################
##									##
##  Determine if a particular range of addesses is accessible 		##
##  in the current memory mapping by the given processor mode 		##
##									##
##    Input:								##
##									##
##	 4(%esp) = size of area						##
##	 8(%esp) = address of area					##
##	12(%esp) = processor mode					##
##	16(%esp) = 0 : validate for read				##
##	           1 : validate for write				##
##									##
##    Output:								##
##									##
##	%eax = 0 : location is not accessible				##
##	       1 : location is accessible				##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_probe
	.type	oz_hw_probe,@function
oz_hw_probe:
	pushl	%ebp					# make a call frame
	movl	%esp,%ebp
.if PARANOID_MPD
	pushl	%esi
.endif
	pushl	%ebx

	xorl	%edx,%edx				# start out requiring nothing
	bt	$0,16(%ebp)				# set carry iff user access required
	adcl	%edx,%edx				# - it will end up being our 'U'-bit-required flag
	bt	$0,20(%ebp)				# set carry iff write access required
	adcl	%edx,%edx				# - it will end up being our 'W'-bit-required flag
	addl	%edx,%edx				# we always need the 'P'-bit
	incl	%edx

	movl	 8(%ebp),%ecx				# number of bytes
	jecxz	probe_null				# success if null buffer
	movl	12(%ebp),%ebx				# get starting address
	addl	%ebx,%ecx				# point to end of area
	jc	probe_fail				# (some crazy wrap-around, fail)
	decl	%ecx					# make end pointer inclusive
	shrl	$12,%ebx				# get starting virtual page number
	shrl	$12,%ecx				# get end virtual page number (inclusive)
	subl	%ebx,%ecx				# get number of pages to test
	incl	%ecx
	subl	$PROCBASVPAGE,%ebx			# see if a process address
	jc	probe_system
	MOVL_CR3_EAX					# process, get pointer to this cpu's current MPD
.if PARANOID_MPD
	leal	PROCBASVPAGE/256(%eax),%esi		# point to entry for PROCBASVPAGE
							# (paranoid MPD virt addr = phys addr)
.else
	cmpl	$MPDBASE,%eax				# see if we're on system MPD
	je	probe_fail				# if so, page can't be addressible because we
							# ... can't even access the per-process MPD!
.endif
probe_process_loop:
## %ebx = virtual page number within process space
## %ecx = number of pages to test
## %edx = required bits in the pte
	movl	%ebx,%eax				# get index in the MPD for the page being checked
	shrl	$10,%eax
.if PARANOID_MPD
	bt	$0,(%esi,%eax,4)			# (look in paranoid MPD in case per-process MPD not addressible)
.else
	bt	$0,mpde_for_first_proc_page(,%eax,4)	# (look in per-process MPD)
.endif
	jnc	probe_fail				# fail if MPD says the pt page is not present
	shrl	$4,%eax					# this is how many reqprot pages we need to skip
	shll	$12,%eax				# this is how many reqprot longs we need to skip
	movl	PROCPTRPBVADDR(%eax,%ebx,4),%eax	# get the pte
	notl	%eax					# see if it has all the required bits set
	testl	%edx,%eax
	jne	probe_fail				# if not, it fails
	incl	%ebx					# check out the next page
	loop	probe_process_loop
probe_null:
	movl	$1,%eax					# no more pages, we're successful
	popl	%ebx
.if PARANOID_MPD
	popl	%esi
.endif
	leave
	ret

probe_fail:
	xorl	%eax,%eax
	popl	%ebx
.if PARANOID_MPD
	popl	%esi
.endif
	leave
	ret

probe_system:
	addl	$PROCBASVPAGE,%ebx			# restore starting page number
	leal	(%ebx,%ecx),%eax			# get end page number (exclusive)
	cmpl	oz_s_sysmem_pagtblsz,%eax		# see if we will go beyond end of system table
	ja	probe_fail				# fail a probe that goes from system into process area
probe_system_loop:
	movl	SPTBASE(,%ebx,4),%eax			# get the pte
	notl	%eax					# see if it has all the required bits set
	testl	%edx,%eax
	jne	probe_fail				# if not, it fails
	incl	%ebx					# check out the next page
	loop	probe_system_loop
	movl	$1,%eax					# no more pages, we're successful
	popl	%ebx
	leave
	ret

##########################################################################
##									##
##  Kernel debugger support routines					##
##									##
##########################################################################

##########################################################################
##									##
##  Write a breakpoint at a read-only location				##
##									##
##    Input:								##
##									##
##	4(%esp) = address to write the breakpoint			##
##	8(%esp) = breakpoint opcode (one byte)				##
##									##
##    Output:								##
##									##
##	%eax = 0 : successful						##
##	    else : points to error message				##
##									##
##########################################################################

writebptfail_system:	.asciz	"not a system address"
writebptfail_present:	.asciz	"page not present in memory"

	.align	4
	.globl	oz_hw_debug_writebpt
	.type	oz_hw_debug_writebpt,@function
oz_hw_debug_writebpt:
	pushl	%ebp				# make stack frame for trace
	movl	%esp,%ebp
	pushl	%edi				# save C register
	movl	8(%ebp),%edi			# must be a system address
	movl	$writebptfail_system,%eax
	cmpl	$PROCBASVADDR,%edi
	jnc	writebptfail
	movl	%edi,%edx			# get page number
	shrl	$12,%edx
	leal	SPTBASE(,%edx,4),%edx		# point to PTE
	movl	(%edx),%ecx			# get PTE
	movl	$writebptfail_present,%eax
	bt	$0,%ecx				# page must be 'present'
	jnc	writebptfail
	MOVL_CR0_EAX				# clear the WP bit so the page will be writable
	pushl	%eax
	andl	$0xFFFEFFFF,%eax
	MOVL_EAX_CR0
	movb	12(%ebp),%al			# get breakpoint instruction opcode
	movb	%al,(%edi)			# write the breakpoint value
	popl	%eax				# restore WP bit so page will be read-only again
	MOVL_EAX_CR0
	xorl	%eax,%eax			# successful
writebptfail:
	popl	%edi
	leave
	ret

##########################################################################
##									##
##  User mode callable oz_knl_printkp - called via INT_PRINTKP		##
##  It will also work if called from executive mode via INT_PRINTKP	##
##									##
##    Input:								##
##									##
##	 4(%esp) = 0 : do not pause for <enter> key			##
##	           1 : pause for <enter> key				##
##	 8(%esp) = points to format string				##
##	12(%esp) = points to argument list				##
##									##
##    Note:								##
##									##
##	This routine is for debugging only and should be disabled in 	##
##	a production system by commenting out the entry in the idt	##
##									##
##########################################################################

except_printkp_nullpmt:	.byte	0

	.align	4
	.globl	oz_sys_printkp
	.type	oz_sys_printkp,@function
oz_sys_printkp:					# callable, oz_sys_printkp (pause_flag, format, args...)
	pushl	%ebp
	movl	%esp,%ebp

	pushl	12(%ebp)			# make sure format string is faulted in
	call	strlen

	movl	 8(%ebp),%eax			# get pause flag
	movl	12(%ebp),%ecx			# get format string pointer
	leal	16(%ebp),%edx			# get arg list pointer
	int	$INT_PRINTKP			# do the printing in executive mode

	addl	$4,%esp				# wipe format string pointer
	popl	%ebp				# restore frame pointer
	ret					# return to caller

	.align	4
except_printkp:
	testb	$2,4(%esp)			# see if previous executive mode
	je	except_printkp_exe
	xorl	%ebp,%ebp			# if not, zero the ebp in case of exception during print
						#         so we dont try to call a user mode condition handler
except_printkp_exe:
	pushl	%ebp				# make a stack frame in case of crash
	movl	%esp,%ebp
	pushl	%eax				# save the prompt enable flag
	pushl	%edx				# push address of format arg list
	pushl	%ecx				# push address of format string
	call	oz_knl_printkv			# print the formatted string
	addl	$8,%esp				# pop oz_knl_printkv call args
	popl	%ecx				# restore the prompt enable flag
	jecxz	except_printkp_rtn		# all done if zero
	pushl	$except_printkp_nullpmt		# non-zero, push null prompt string address
	call	oz_hw_pause			# wait for the <enter> key
except_printkp_rtn:
	leave					# pop saved stack frame pointer (might be zero)
	iret					# all done

##########################################################################
##									##
##  System calls							##
##									##
##    Input:								##
##									##
##	%ecx = call arg bytecount					##
##	%edx = syscall index						##
##	 (%ebp) = saved %ebp						##
##	4(%ebp) = return to caller of oz_sys_... routine		##
##	8(%ebp) = first call argument to oz_sys_... routine		##
##	CPU is in user mode						##
##									##
##    Output:								##
##									##
##	%eax = status							##
##									##
##########################################################################

	MAXARGBYTES = 64	# allows up to 16 long args to oz_sys_... routines

	.align	4
	.globl	oz_hw486_syscall
	.type	oz_hw486_syscall,@function
oz_hw486_syscall:
	movl	%ebp,%esp			# make sure GNUC didn't put anything funny on stack
syscall_patch:			# the next two bytes of this get patched
				# with a 'sysenter' if the cpu supports SYSENTER/SYSEXIT
	int	$INT_SYSCALL			# do it
	popl	%ebp				# restore caller's stack frame
	jecxz	syscall_checkasts		# jump if pending ast's
	ret					# all done

	.align	4
syscall_sep_rtnwithast:
	popl	%ebp				# back in user mode, pop saved %ebp
syscall_checkasts:
	popl	%edx				# pop return address
	int	$INT_CHECKASTS			# check for ast's without instruction tracing, etc

	.align	4
except_checkasts:				# (still in user mode, but tracing turned off)
	movl	%edx,(%esp)			# replace return address on stack with return to caller to oz_hw_syscall
						# finish mchargs on stack
	pushl	%ebp				# - ebp
	pushal					# - eax,ecx,edx,ebx,esp,ebp/ec1,esi,edi
	pushl	$0				# - ec2
	leal	MCH_L_EBP(%esp),%ebp		# set up new frame pointer
	leal	MCH__SIZE(%esp),%eax		# this is where stack pointer was before exception
	movl	$0,MCH_L_EC1(%esp)		# clear out the ec1
	movl	%eax,MCH_L_ESP(%esp)		# save stack pointer at time of exception
	pushl	%esp				# make pointer to mchargs - they point just past the call to oz_hw_syscall
	call	oz_sys_thread_checkast		# check for ast's now deliverable and process them
	addl	$8,%esp				# wipe mcharg pointer and dummy ec2 from stack
	popal					# restore registers from mchargs on stack
	popl	%ebp
	iret

## Input is as follows:
##
##       %ecx = call arg bytecount
##       %edx = call index number
##       %ebp = points to saved frame and return address that called oz_sys_... routine
##    0(%esp) = points just past the int instruction
##    4(%esp) = caller's code segment (user mode)
##    8(%esp) = eflags (ignored)

	.align	4
except_syscall:
	pushl	%ebp				# save caller's frame pointer
	pushl	%ebx				# mark where we are in kernel stack
	movl	%esp,%ebx
	cmpl	$MAXARGBYTES+1,%ecx		# check for bad arg bytecount
	jnc	except_syscall_bad		# - must be .le. MAXARGBYTES
	cmpl	oz_s_syscallmax,%edx		# see if call index exceeds maximum
	jnc	except_syscall_bad
	subl	$MAXARGBYTES,%esp		# make room on kernel stack
	movl	%esp,%eax			# - always use MAXARGBYTES in case caller gave us bad count
	pushl	%edx				# push the validated call index
	pushl	%eax				# copy arg list from user stack to kernel stack
	leal	8(%ebp),%eax
	pushl	%eax
	pushl	%ecx
	pushl	oz_PROCMODE_USR
	call	oz_knl_section_uget
	addl	$16,%esp
	cmpl	oz_SUCCESS,%eax
	jne	except_syscall_rtnsts

	popl	%edx				# get validated call index back
	pushl	$0				# dummy saved eip
	pushl	$0				# dummy saved ebp
	movl	%esp,%ebp			# point to dummy call frame so exception handlers won't try usermode handlers
	pushl	oz_PROCMODE_USR			# push caller's processor mode
	movl	oz_s_syscalltbl(,%edx,4),%eax	# get routine entrypoint
	call	*%eax				# process the system call

except_syscall_rtnsts:
	movl	%ebx,%esp			# reset kernel stack
	movl	%eax,%ebx			# save return status
	cli					# inhibit hw ints so we can't get softinted after calling chkpendast
						# ... that could result in an ast getting queued that we miss seeing
	pushl	oz_PROCMODE_USR			# we are about to return to user mode
	call	oz_knl_thread_chkpendast	# ... see if there will be any deliverable ast's for that mode
	movl	$1,%ecx
	addl	$4,%esp
	subl	%eax,%ecx			# setup ecx = 0 : there are pending ast's; ecx = 1 : no pending ast's
	movl	%ebx,%eax			# restore return status
	popl	%ebx				# restore saved %ebx
	popl	%ebp				# restore saved %ebp
	iret					# return with status in %eax, restore interrupt delivery flag bit
						# %ecx holds pending ast flag

except_syscall_bad:
	movl	oz_BADSYSCALL,%eax
	jmp	except_syscall_rtnsts

# This is jumped to by the SYSENTER instruction to service a system call from user mode
#
#    Input:
#
#	%ebp = points to user stack
#	        0(%ebp) = saved %ebp
#	        4(%ebp) = return from oz_hw_syscall
#	        8(%ebp) = remainder of args to oz_hw_syscall
#	%ecx = arg list byte count
#	%edx = system call number
#	kernel stack is empty
#	hardware interrupt delivery inhibited

syscall_sep_enter:
	cmpl	$MAXARGBYTES+1,%ecx		# see if arg bytecount exceeds maximum
	jnc	syscall_sep_bad
	cmpl	oz_s_syscallmax,%edx		# see if call index exceeds maximum
	jnc	syscall_sep_bad
	sti					# allow interrupts (now we have to check for ast's on exit)
	pushl	%ebp				# save user's call frame pointer
	pushl	%ebx				# mark where we are in kernel stack
	movl	%esp,%ebx
	subl	$MAXARGBYTES,%esp		# make room on kernel stack for call args
	movl	%esp,%eax			# - always MAXARGBYTES in case given bad count
	pushl	%edx				# push the validated call index
	pushl	%eax				# copy arg list from user stack to kernel stack
	leal	8(%ebp),%eax
	pushl	%eax
	pushl	%ecx
	pushl	oz_PROCMODE_USR
	call	oz_knl_section_uget
	addl	$16,%esp
	cmpl	oz_SUCCESS,%eax
	jne	syscall_sep_usermode_rtnsts
	popl	%edx				# get validated call index back
	pushl	$0				# dummy saved eip
	pushl	$0				# dummy saved ebp
	movl	%esp,%ebp			# point to dummy call frame so exception handlers won't try usermode handlers
	pushl	oz_PROCMODE_USR			# push caller's processor mode
	movl	oz_s_syscalltbl(,%edx,4),%eax	# get routine entrypoint
	call	*%eax				# process the system call

syscall_sep_usermode_rtnsts:
	movl	%ebx,%esp			# reset kernel stack
	movl	%eax,%ebx			# save return status
	cli					# inhibit hw ints so we can't get softinted after calling chkpendast
						# ... that could result in an ast getting queued that we miss seeing
	pushl	oz_PROCMODE_USR			# we are about to return to user mode
	call	oz_knl_thread_chkpendast	# ... see if there will be any deliverable ast's for that mode
	addl	$4,%esp
	movl	$syscall_sep_rtn,%edx		# assume no deliverable ast's
	testl	%eax,%eax
	movl	$syscall_sep_rtnwithast,%eax
	cmovnel	%eax,%edx			# there are ast's so process them on return
	movl	%ebx,%eax			# restore system call status
	popl	%ebx				# restore saved %ebx
	popl	%ecx				# restore user frame pointer = its stack pointer
	movl	%ecx,%ebp
	sti					# wait until now to enable interrupts in case another ast tries to slip in
	sysexit					# this should execute with interrupts inhibited, then enables them when complete

# Bad system call number

syscall_sep_bad:
	movl	oz_BADSYSCALL,%eax		# set up error status code
	movl	%ebp,%ecx			# user stack pointer
	movl	$syscall_sep_rtn,%edx		# user instruction pointer
	sti					# wait until now to enable interrupts in case an ast tries to slip in
	sysexit					# this should execute with interrupts inhibited, then enables them when complete

	.align	4
syscall_sep_rtn:
	popl	%ebp				# back in user mode, pop saved %ebp
	ret					# status is in %eax

##########################################################################
##									##
##  Exception handlers							##
##									##
##########################################################################

##########################################################################
##									##
##  These exception handlers are called in the mode of the instruction 	##
##  that caused the exception - they don't have an error code		##
##									##
##########################################################################

	.align	4
exception_UD:				# 6 - undefined opcode
	pushl	%ebp
	movl	4(%esp),%ebp		# point to faulting instruction
	cmpb	$0x0F,(%ebp)
	jne	exception_UD_real
	cmpb	$0x43,1(%ebp)
	jne	exception_UD_real
	cmpb	$0xC8,2(%ebp)
	je	exception_UD_cmovnc_eax_ecx
	cmpb	$0xCA,2(%ebp)
	je	exception_UD_cmovnc_edx_ecx
exception_UD_real:
	movl	oz_UNDEFOPCODE,%ebp
	jmp	exception_xx

exception_UD_cmovnc_eax_ecx:		# emulate cmovnc %eax,%ecx
	testb	$0x01,12(%esp)
	jne	exception_UD_cmovnc_eax_ecx_rtn
	movl	%eax,%ecx
exception_UD_cmovnc_eax_ecx_rtn:
	popl	%ebp
	addl	$3,(%esp)
	iret

exception_UD_cmovnc_edx_ecx:		# emulate cmovnc %edx,%ecx
	testb	$0x01,12(%esp)
	jne	exception_UD_cmovnc_edx_ecx_rtn
	movl	%edx,%ecx
exception_UD_cmovnc_edx_ecx_rtn:
	popl	%ebp
	addl	$3,(%esp)
	iret

	.align	4
exception_DE:				# 0 - divide error
	pushl	%ebp
	movl	oz_DIVBYZERO,%ebp
	jmp	exception_xx

	.align	4
exception_DB:				# 1 - debug exception
	pushl	%ebp
	movl	oz_SINGLESTEP,%ebp
	jmp	exception_xx

	.align	4
exception_BP:				# 3 - int3 breakpoint
	pushl	%ebp
	movl	oz_BREAKPOINT,%ebp
	jmp	exception_xx

	.align	4
exception_OF:				# 4 - arithmetic overflow
	pushl	%ebp
	movl	oz_ARITHOVER,%ebp
	jmp	exception_xx

	.align	4
exception_BR:				# 5 - subscript out of bounds
	pushl	%ebp
	movl	oz_SUBSCRIPT,%ebp
	jmp	exception_xx

	.align	4
exception_MF:				# 16 - floating point exception
	pushl	%ebp
	movl	oz_FLOATPOINT,%ebp
	jmp	exception_xx

	.align	4
exception_AC:				# 17 - alignment check
	pushl	%ebp
	movl	oz_ALIGNMENT,%ebp
##	jmp	exception_xx

	.align	4
exception_xx:
	pushal				# save all registers on stack (exception code is in ebp and will go into EC1)
	pushl	$0			# push dummy error code 2
					# now mchargs are complete on the stack
	leal	MCH_L_EBP(%esp),%ebp	# set up frame pointer
	movl	MCH_L_EC1(%esp),%eax	# get OZ_ error status code
	leal	MCH_L_XSP(%esp),%ebx	# calc esp at time of exception
	movl	$0,MCH_L_EC1(%esp)	# clear out the ec1 in the mchargs
	movl	%ebx,MCH_L_ESP(%esp)	# save esp at time of exception in the mchargs
	cmpl	oz_FLOATPOINT,%eax	# exec/kernel, check for fpu exception
	jne	exception_push		# if not, jump to common exception processing
					# - mchargs are on the stack
					# - %eax holds the error status code
	testb	$2,MCH_W_CS(%esp)	# see if in user mode
	jne	exception_MF_push
	movl	%eax,%ecx		####
	MOVL_CR0_EAX			#### see if the fpu is even enabled
	testb	$8,%al			####
	jne	exception_MF_nofpu	####
	movl	%ecx,%eax		####
exception_MF_push:
	subl	$28,%esp		# floating point exception, save state
	fnstenv	(%esp)			# ... as signal arguments
	pushl	$7
	pushl	%eax
	pushl	$9
	jmp	exception_test

	.align	4			####
exception_MF_nofpu:			####
	movl	%esp,%eax		#### fp exceptoin with fpu disabled
	pushl	MCH_W_CS(%eax)		#### print a message
	pushl	MCH_L_EIP(%eax)		####
	pushl	$exception_MF_msg1	####
	pushl	$0			####
	call	oz_sys_printkp		####
	addl	$16,%esp		####
	jmp	exception_rtn		#### then ignore it

exception_MF_msg1:	.string	"oz_kernel_486 exception_MF: fpu is disabled, eip %8.8x, cs %4.4x\n"

##########################################################################
##									##
##  These exception handlers are called in the mode of the instruction 	##
##  that caused the exception - with an error code (ec1) value already 	##
##  on stack								##
##									##
##########################################################################

	UDSUDS = (USRDATASEG << 16) + USRDATASEG

	.align	4
exception_GP:				# 13
	pushw	%es			# see if %ds and %es are ok
	pushw	%ds
	cmpl	$UDSUDS,(%esp)
	jne	fix_es_ds		# ... if not, go fix them
	pushw	%gs			# see if %fs and %gs are ok
	pushw	%fs
	cmpl	$UDSUDS,(%esp)
	jne	fix_gs_fs		# ... if not, go fix them
	addl	$8,%esp			# that's not the problem
	xchgl	%ebp,(%esp)		# save ebp, get ec1 in ebp
	pushal				# save registers on stack
.if EXECODESEG <> KNLCODESEG
	testb	$2,40(%esp)		# check for was in exec mode
	jne	gp_not_exe_hlt
	movl	36(%esp),%edx		# see if stopped on an HLT instruction
	cmpl	$OZ_IMAGE_BASEADDR,%edx
	jc	gp_not_exe_hlt
	cmpl	$OZ_IMAGE_NEXTADDR,%edx
	jnc	gp_not_exe_hlt
	cmpb	$0xF4,(%edx)
	je	gp_exe_hlt_fix
	cmpb	$0x90,(%edx)		# (maybe another CPU just patched it)
	jne	gp_not_exe_hlt
	popal				# ok, just redo it ...
	popl	%ebp
	incl	(%esp)			# ... but inc past it so we can't get 2 in a row
	iret
gp_not_exe_hlt:
.endif
	movl	oz_GENERALPROT,%eax	# get OZ_... code in eax
	jmp	exception_xx_ec		# jump to common processing routine

	# if any of the dataseg registers are bad, fix them
	# the user should have no reason to change them
	# but if they did, we want to fix them for kernel stuff

fix_es_ds:
	pushl	%edx			# save scratch wiped by oz_knl_printk
	pushl	%ecx
	pushl	%eax
	movw	$USRDATASEG,%ax		# get new contents
	movw	%ax,%ds
	movw	%ax,%es

	movl	%esp,%edx		# print out a message
	pushl	24(%edx)
	pushl	20(%edx)
	movzwl	14(%edx),%eax
	pushl	%eax
	movzwl	12(%edx),%eax
	pushl	%eax
	pushl	$fixed_es_ds
	call	oz_knl_printk
	addl	$20,%esp

	popl	%eax			# restore scratch regs
	popl	%ecx
	popl	%edx
	addl	$8,%esp			# wipe old ds/es and error code
	iret				# resume where we left off

fix_gs_fs:
	pushl	%edx			# save scratch wiped by oz_knl_printk
	pushl	%ecx
	pushl	%eax
	movw	$USRDATASEG,%ax		# get new contents
	movw	%ax,%fs
	movw	%ax,%gs

	movl	%esp,%edx		# print out a message
	pushl	28(%edx)
	pushl	24(%edx)
	movzwl	14(%edx),%eax
	pushl	%eax
	movzwl	12(%edx),%eax
	pushl	%eax
	pushl	$fixed_gs_fs
	call	oz_knl_printk
	addl	$20,%esp

	popl	%eax			# restore scratch regs
	popl	%ecx
	popl	%edx
	addl	$12,%esp		# wipe old ds/es/fs/gs and error code
	iret				# resume where we left off

	# there was an HLT instruction in the kernel but we are operating in executive mode
	# so just patch the HLT over with an NOP and resume

.if EXECODESEG <> KNLCODESEG
gp_exe_hlt_fix:
	MOVL_CR0_EAX			# clear the WP bit so the instruction will be writable
	pushl	%eax
	andl	$0xFFFEFFFF,%eax
	MOVL_EAX_CR0
	movb	$0x90,(%edx)		# change the HLT to a NOP
	popl	%eax			# restore WP bit
	MOVL_EAX_CR0
	pushl	%edx			# output a message
	pushl	$gp_exe_hlt_msg
	call	oz_knl_printk
	addl	$8,%esp
	popal				# restore registers
	popl	%ebp			# ... including ebp
	iret				# resume (at the NOP)
.endif

fixed_es_ds:	.string	"oz_kernel_486 exception_GP: fixed ds %X, es %X, at eip %X, cs %X\n"
fixed_gs_fs:	.string	"oz_kernel_486 exception_GP: fixed fs %X, gs %X, at eip %X, cs %X\n"
.if EXECODESEG <> KNLCODESEG
gp_exe_hlt_msg:	.string	"oz_kernel_486 exception_GP: patched HLT at %X to a NOP\n"
.endif

	.align	4
except_linux:				# 0x80 - old linux call
	pushl	%ebp
	movl	%eax,%ebp		# (well, error code wasn't on stack, so we put the syscall number there like it is an error code)
	pushal				# save registers on stack
	movl	oz_OLDLINUXCALL,%eax	# get OZ_... code in eax
##	jmp	exception_xx_ec

	.align	4
exception_xx_ec:
	pushl	$0			# push dummy ec2 error code
					# mchargs now complete on stack
	leal	MCH_L_EBP(%esp),%ebp	# point to stack frame
	movl	MCH_L_XSP(%esp),%ebx	# calc esp at time of exception
	movl	%ebx,MCH_L_ESP(%esp)	# save esp at time of exception in the mchargs
	jmp	exception_push		# jump to common exception processing
					# - mchargs are on the stack
					# - %eax holds the error status code

##########################################################################
##									##
##  Misc specially handled exceptions					##
##									##
##########################################################################

##########################################################################
##
##  FPU access disabled exception
##
##  This happens when a thread uses the FPU for the first time after it has been (re-)activated
##
##  This exception handler runs in executive mode with interrupt delivery inhibited.  It could run 
##  with interrupts enabled, but then we would have to check for user mode ast's before returning.
##  Plus we would have to inhibit softints to stay on the same cpu/fpu, so it's probably not 
##  worth the effort.

	.align	4
exception_NM:
	pushl	%esi				# save the registers we use
	pushl	%eax
	call	oz_hw486_getcpu			# get pointer to cpudb for this cpu
	CLRTS					# clear the TS bit from CR0 to enable access to the fpu
	movl	CPU_L_THCTX(%esi),%eax		# get the current thread hardware context pointer
	fninit					# initialize fpu
	testb	$2,THCTX_L_FPUSED(%eax)		# see if it has been initialized for this thread once upon a time
	movl	$3,THCTX_L_FPUSED(%eax)		# set bit 0 saying that this thread has used fpu since last oz_hw_thread_switchctx
						# set bit 1 saying that this thread has used fpu in its lifetime
	je	exception_NM_done
	andb	$0xF0,%al			# fxrstor requires 16-byte alignment
frstor_pat:			# frstor/nop gets patched to fxrstor if cpu is mmx
	frstor	THCTX_X_FPSAVE+16(%eax)		# if so, restore prior fpu state
	nop
exception_NM_done:
	popl	%eax				# restore registers
	popl	%esi
	iret					# re-try the floating point instruction

##########################################################################
##
##  Reserved fault - this happens via the APIC and we just ignore it
##

	.align	4
exception_15:
	iret

##########################################################################
##
##  Double fault - usually due to bad user stack
##
##  This executes in the executive code segment
##
##    0(%esp) = error code, always 0
##    4(%esp) = old EIP
##    8(%esp) = old CS
##   12(%esp) = old EFLAGS
##
	.align	4
exception_DF:				# 8
	pushl	$exception_df_msg1	# output a message to console
	call	oz_knl_printk
	pushl	oz_DOUBLEFAULT		# abort the offending thread
	call	oz_knl_thread_exit
	pushl	$exception_df_msg2	# shouldn't return
	call	oz_crash

exception_df_msg1:	.ascii	"oz_kernel_486 exception_DF: double-fault error %x, EIP %x, CS %x, EFLAGS %x, esp %x, ss %x"
			.byte	10,0
exception_df_msg2:	.ascii	"oz_kernel_486 exception_DF: return from oz_knl_thread_exit"
			.byte	10,0

##########################################################################
##
##  These modified handlers run in executive mode with interrupt delivery 
##  inhibited.  They are not for production use, but for kernel debugging.
##
##  They simply call the executive debugger with the associated exception

#  General protection

	.align	4
exception_GP_exe:
	xchgl	%ebp,(%esp)		# swap with error code
	pushal				# save registers on stack
	movw	$USRDATASEG,%ax		# set segment registers
	movw	%ax,%ds
	movw	%ax,%es
	movw	%ax,%fs
	movw	%ax,%gs
	pushl	$0			# push dummy ec2 error code
	call	oz_hw_fixkmexceptebp	# point to stack frame
	call	oz_hw_fixkmmchargesp	# calc esp at time of exception in the mchargs
	pushl	$0
	pushl	oz_GENERALPROT
	pushl	$2
	jmp	exception_crash		# just call the debugger

#  Debug (trace trap)

	.align	4
exception_DB_exe:
	pushl	%ebp			# save frame pointer	
	pushal				# save registers on stack
	pushl	$0			# push dummy ec2 error code
	movl	$0,MCH_L_EC1(%esp)	# clear ec1
	call	oz_hw_fixkmexceptebp	# point to stack frame
	call	oz_hw_fixkmmchargesp	# calc esp at time of exception in the mchargs
	pushl	$0
	pushl	oz_SINGLESTEP
	pushl	$2
	jmp	exception_crash		# just call the debugger

#  Breakpoint

	.align	4
exception_BP_exe:
	pushl	%ebp			# save frame pointer	
	pushal				# save registers on stack
	pushl	$0			# push dummy ec2 error code
	movl	$0,MCH_L_EC1(%esp)	# clear ec1
	call	oz_hw_fixkmexceptebp	# point to stack frame
	call	oz_hw_fixkmmchargesp	# calc esp at time of exception in the mchargs
	pushl	$0
	pushl	oz_BREAKPOINT
	pushl	$2
	jmp	exception_crash		# just call the debugger

##########################################################################
##									##
##  Page fault								##
##									##
##  This executes in the executive code segment (just in case it is 	##
##  the user stack that caused the fault)				##
##									##
##    Input:								##
##									##
##	 0(%esp) = error code						##
##	 4(%esp) = old EIP						##
##	 8(%esp) = old CS						##
##	12(%esp) = old EFLAGS						##
##									##
##  This handler is entered with hw ints inhibited so the cpu doesn't 	##
##  switch on us before we retrieve the faulting address from CR2	##
##									##
##########################################################################

####	except_pf_deb1:	.string	"oz_kernel_486 exception_PF: pagefault> "
####	except_pf_deb2:	.string	"oz_kernel_486 exception_PF: oz_knl_section_faultpage sts %u> "
####	except_pf_deb3:	.string	"oz_kernel_486 exception_PF: pagefault va 0x%X, ec 0x%X, eip 0x%X, cs 0x%X > "

	.align	4
exception_PF:
					# it has an error code aleady on the stack (where we want to save %ebp)
	pushal				# save all registers on stack
	MOVL_CR2_EAX			# get address that caused the fault
	pushl	%eax			# push ec2 = virtual address that caused the fault
	movl	MCH_L_EBP(%esp),%eax	# get the error code from stack
	movl	%ebp,MCH_L_EBP(%esp)	# save the %ebp there
	movl	%eax,MCH_L_EC1(%esp)	# put the error code where it beints
	call	oz_hw_fixkmexceptebp	# point to stack frame
	call	oz_hw_fixkmmchargesp	# calc esp at time of exception in the mchargs

		####	movl	%esp,%eax
		####	pushl	MCH_W_CS(%eax)
		####	pushl	MCH_L_EIP(%eax)
		####	pushl	MCH_L_EC1(%eax)
		####	pushl	MCH_L_EC2(%eax)
		####	pushl	$except_pf_deb3
		####	call	oz_knl_printkp
		####	addl	$20,%esp

		####	call	pte_print
		####	pushl	%esp
		####	pushl	$MCH__SIZE+8
		####	call	oz_knl_dumpmem
		####	pushl	$except_pf_deb1
		####	call	oz_hw_pause
		####	addl	$12,%esp

	bt	$9,MCH_L_EFLAGS(%esp)	# see if interrupt delivery inhibited
	movl	oz_ACCVIO,%ebx		# assume we will signal the pagefault as an executive mode access violation
	jnc	except_pf_signal	# if so, signal the pagefault as an executive mode access violation
	call	oz_hw486_getcpu		# see which cpu we're on
	cmpb	$2,CPU_B_SMPLEVEL(%esi)	# see if above softint
	sti				# restore interrupt delivery flag
	jnc	except_pf_signal	# if above softint, signal the pagefault as an executive mode access violation
	movl	MCH_L_EC2(%esp),%eax	# softint or below, get virtual address that caused fault
	movl	MCH_L_EC1(%esp),%ebx	# get reason for pagefault
	xorl	%ecx,%ecx		# assume they were trying to read
	bt	$1,%ebx			# maybe they were trying to write
	adcb	$0,%cl
	shrl	$12,%eax		# calculate vpage that caused fault
	movl	oz_PROCMODE_KNL,%edx	# assume access was from executive mode
	bt	$2,%ebx
	jnc	except_pf_exe
	movl	oz_PROCMODE_USR,%edx	# - it was from user mode
except_pf_exe:
	movl	CPU_L_THCTX(%esi),%edi	# don't allow access to lowest user stack page
	cmpl	%eax,THCTX_L_USTKVPG(%edi)
	je	except_pf_lowustkpg
	pushl	%ecx			# push read/write flag that caused fault
	pushl	%eax			# push vpage that caused fault
	pushl	%edx			# push access mode that caused fault
	call	oz_knl_section_faultpage # try to read in page
	addl	$12,%esp		# wipe call args from stack

		####	leal	MCH_L_EIP(%esp),%esi
		####	pushl	%eax
		####	pushl	%esi
		####	pushl	$20
		####	call	oz_knl_dumpmem
		####	movl	MCH_L_EC2+12(%esp),%eax
		####	call	pte_print
		####	pushl	8(%esp)
		####	pushl	$except_pf_deb2
		####	call	oz_knl_printkp
		####	addl	$16,%esp
		####	popl	%eax

	testb	$2,MCH_W_CS(%esp)	# see if caller was user mode
	movl	%eax,%ebx		# save page read status code
	je	except_pf_retrychk

## We are returning to user mode, possibly with pending user mode ast's

	pushl	oz_PROCMODE_USR		# returning to user mode, check for pending user mode ast's
	cli				# ... with hw ints inhibited so we can't be in COM state if we get new ast's
	call	oz_knl_thread_chkpendast
	addl	$4,%esp
	testl	%eax,%eax
	jne	except_pf_userastpend
	cmpl	oz_SUCCESS,%ebx		# no pending ast's, maybe we retry the faulting instruction now
	je	exception_rtn		# if so, go do it (leave hw ints inhibited so we can't be in COM state if we get new ast's)
except_pf_userastpend:
	sti				# well we have at least one, doesn't matter if we get any more
	xorl	%eax,%eax		# no extra words on stack to move to user stack, just the mchargs
	call	oz_hw486_movemchargstocallerstack # try to move mchargs onto user stack and switch to user mode
	cmpl	oz_SUCCESS,%eax
	movl	%esp,%esi		# point to mchargs
	jne	except_pf_baduserstack	# - didn't make it, abort thread
	pushl	%esi			# process any queued user mode ast's
	call	oz_sys_thread_checkast
	addl	$4,%esp

## On caller's stack with mchargs on stack, %ebx = page read status

except_pf_retrychk:
	cmpl	oz_SUCCESS,%ebx		# now see if we should retry the faulting instruction
	je	exception_rtn		# if so, go do it

## Page read or other fatal error, signal it

except_pf_signal:
	movl	%esp,%esi		# point to mchargs
	pushl	MCH_L_EC1(%esi)		# signal the fault
	pushl	MCH_L_EC2(%esi)
	pushl	$2
	pushl	%ebx
	pushl	$4
	jmp	exception_test

## User stack cannot hold mchargs, so we abort the thread (we are still on executive stack)

except_pf_baduserstack:
	pushl	MCH_L_ESP(%esi)		# output message to console indicating why we are aborting it
	pushl	%ebx
	pushl	$except_pf_msg1
	call	oz_knl_printk
	pushl	%ebx			# exit thread with original oz_knl_section_faultpage error code as the exit status
	jmp	except_pf_usertrace

## Lowest user stack page is a guard, print error message and exit

except_pf_lowustkpg:
	movl	%esp,%esi		# point to mchargs on stack
	pushl	%eax			# output message to console indicating why we are aborting it
	pushl	$except_pf_msg2
	call	oz_knl_printk
	pushl	oz_USERSTACKOVF		# push thread exit status on stack

## Try to trace a few call frames before exiting

except_pf_usertrace:
	pushl	$0			# push current thread's name
	call	oz_knl_thread_getname
	movl	%eax,(%esp)
	pushl	$0			# push current thread's id
	call	oz_knl_thread_getid
	movl	%eax,(%esp)
	pushl	$except_pf_msg3		# print them out
	call	oz_knl_printk
	pushl	%esi			# print out the mchargs
	pushl	$MCH__SIZE
	call	oz_knl_dumpmem
	addl	$20,%esp		# wipe call params so exit status is on top of stack
	movl	MCH_L_EBP(%esi),%edi	# try to do a traceback of a few frames
	movl	$8,%ebx
except_pf_usertraceloop:
	pushl	$0			# - make sure we can read the 8 bytes at %edi
	pushl	$0
	pushl	%edi
	pushl	$8
	call	oz_hw_probe
	addl	$16,%esp
	testl	%eax,%eax
	je	except_pf_usertracedone	# - if not, stop tracing
	pushl	%edi			# - ok, print out a call frame
	pushl	(%edi)
	pushl	4(%edi)
	pushl	$except_pf_msg4
	call	oz_knl_printk
	addl	$16,%esp
	movl	(%edi),%edi		# - link to next frame in chain
	decl	%ebx			# - but only do this many so we dont go banannas
	jne	except_pf_usertraceloop
except_pf_usertracedone:
	call	oz_knl_thread_exit	# finally, exit with status on stack
	jmp	except_pf_usertracedone

except_pf_msg1:	.string	"oz_kernel_486 exception_PF: error %u locking user stack (below %p) in memory\n"
except_pf_msg2:	.string	"oz_kernel_486 exception_PF: user stack overflow, vpage 0x%X\n"
except_pf_msg3:	.string	"oz_kernel_486 exception_PF: - thread id %u, name %s, mchargs:\n"
except_pf_msg4:	.string	"oz_kernel_486 exception_PF:   rtn addr %8.8X  next ebp %8.8X : %8.8X\n"

##########################################################################
##									##
##  Move the machine args to the caller's stack				##
##									##
##    Input:								##
##									##
##	%eax = number of additional lw's to copy to outer stack		##
##	4(%esp) = additional lw's followed by mchargs			##
##									##
##    Output:								##
##									##
##	%eax = OZ_SUCCESS : successfully moved				##
##	             else : error code					##
##									##
##    Scratch:								##
##									##
##	%ecx, %edx, %esi, %edi						##
##									##
##    Pagefault stuff depends on it preserving %ebx			##
##									##
##########################################################################

	.align	4
	.globl	oz_hw486_movemchargstocallerstack
	.type	oz_hw486_movemchargstocallerstack,@function
oz_hw486_movemchargstocallerstack:
	leal	4(%esp,%eax,4),%esi	# point to mchargs
	testb	$2,MCH_W_CS(%esi)	# see if it was from executive code, ie, it was on kernel stack
	je	oncallerstack		# if so, the mchargs are already on the correct stack

## It was on user stack, copy mchargs to user stack and make like a GP exception
## Use oz_knl_section_uput to copy so it can't page out or move on us

	movl	MCH_L_XSP(%esi),%edi	# this is the user's stack pointer
	leal	MCH__SIZE(,%eax,4),%eax	# calculate number of bytes to move
	subl	%eax,%edi		# this is the start of where to put sigargs and mchargs on user stack
	movl	%edi,MCH_L_XSP(%esi)	# put new stack pointer back where iret will see it
	leal	4(%esp),%esi		# point to start of data to move

	pushl	%eax			# save bytecount being copied

	pushl	%edi			# - copy to this address
	pushl	%esi			# - copy from this address
	pushl	%eax			# - copy this how many bytes to copy
	pushl	oz_PROCMODE_USR		# - destination is in user mode
	call	oz_knl_section_uput
	addl	$16,%esp

	popl	%ecx			# restore bytecount that was hopefully copied

	cmpl	oz_SUCCESS,%eax		# hopefully the oz_knl_section_uput succeeded
	jne	callerstackbad		# abort thread if it doesn't have any more user stack

	popl	%edx			# get my return address
	leal	MCH_L_EIP-MCH__SIZE(%esp,%ecx),%esp # wipe extra longwords and machine args from executive stack, but leave EIP, etc on executive stack
	movl	%edx,(%esp)		# change EIP to my return address
	movl	12(%esp),%ebp		# point to user stack area (but don't touch - it could be faulted back out)
	andb	$0xFE,9(%esp)		# make sure the trace (T) bit is clear in eflags
					# (so we dont trace the rest of the exception code or softint handler)
					# (the T bit is preserved in the mchargs that was moved to the user stack)
	leal	MCH_L_EBP-MCH__SIZE(%ebp,%ecx),%ebp # set up ebp to point to MCH_L_EBP in the machine args on the user stack
					# (just as it would be if we were all on executive stack returning via oncallerstack)
	iret				# return to caller in user mode with everything now on user stack

	.align	4
oncallerstack:
	movl	oz_SUCCESS,%eax		# already on correct stack, return success status
callerstackbad:
	ret

##########################################################################
##									##
##  Common exception handling routines					##
##									##
##########################################################################

##########################################################################
##									##
##  Exception handling - calls executive debugger if above softint 	##
##  level, otherwise the exception is signalled				##
##									##
##    Input:								##
##									##
##	processor stack = same as that of instruction causing exception	##
##	eax = error code (OZ_...)					##
##	esp = points to mchargs						##
##									##
##########################################################################

	.align	4
exception_push:
	pushl	$0			# push sigargs on stack
	pushl	%eax
	pushl	$2

# sigargs on stack, followed directly by mchargs
# if we are in user mode, call the condition handler
# else if hardware interrupt delivery inhibited, call the executive debugger
# else call the condition handler

exception_test:
	movl	(%esp),%edx		# get number of longwords in sigargs
	leal	4(%esp,%edx,4),%edi	# point to machine args
	testb	$2,MCH_W_CS(%edi)	# see if in executive mode
	jne	exception_signal	# if not, go signal the exception
	bt	$9,MCH_L_EFLAGS(%edi)	# see if interrupt delivery enabled
	jnc	exception_crash		# if not, call the debugger
	movl	4(%esp),%eax		# restore signal code
	cmpl	oz_SINGLESTEP,%eax	# call executive debugger if one of these exceptions
	je	exception_crash		# ... without checking for exception handler first
	cmpl	oz_BREAKPOINT,%eax
	je	exception_crash

## call condition handler

exception_signal:
	movl	%esp,%esi		# point to sigargs
	pushl	%edi			# push mchargs pointer
	pushl	%esi			# push sigargs pointer
	call	oz_sys_condhand_call	# call condition handler, may unwind or return with:
					#  %eax = 0 : no handler present, use default
					#      else : resume execution
	testl	%eax,%eax		# see if it was able to process it
	je	exception_default
	movl	%edi,%esp		# if so, resume execution by first pointing stack pointer at mch args

## stack is assumed to contain the machine args

exception_rtn:
	addl	$4,%esp			# pop ec2 from stack
	popal				# restore all the registers (except ebp gets ec1's value)
	popl	%ebp			# pop ebp from stack
	iret				# retry the faulting instruction

## no suitable condition handler active, so either call the default handler (for user mode) or trap to the debugger

exception_default:
	movl	%esi,%esp		# wipe junk from stack, point it at sigargs
	testb	$2,MCH_W_CS(%edi)	# see if in executive mode
	je	exception_crash		# if so, go to debugger
	pushl	%edi			# user mode, push mchargs pointer
	pushl	%esi			# push sigargs pointer
	call	oz_sys_condhand_default	# print error message and exit

##########################################################################
##									##
##  Exception occurred in executive mode with no suitable handler, so 	##
##  call the debugger							##
##									##
##    Input:								##
##									##
##	stack contains sigargs followed by mchargs			##
##									##
##########################################################################

	.align	4
exception_crash:
		####	movl	%esp,%eax
		####	pushl	4(%eax)
		####	pushl	%eax
		####	pushl	$exception_crash_msg1
		####	call	oz_knl_printk
		####	addl	$12,%esp
	movl	(%esp),%ebx		# get # of longwords to sigargs, not counting this longword
	movl	%esp,%eax		# point to sigargs
	leal	4(%esp,%ebx,4),%ebx	# point to mchargs
	pushfl				# make sure hw interrupt delivery inhibited for debugger
	cli
	pushl	%ebx			# call executive debugger
	pushl	%eax
	call	oz_knl_debug_exception
	addl	$8,%esp
	popfl				# restore hw interrupt delivery
	movl	%ebx,%esp		# wipe stack up to the machine arguments
	testl	%eax,%eax
	jne	exception_rtn		# continue, restore registers from the mchargs on stack
exception_hang:
	jmp	exception_hang

####	exception_crash_msg1:	.string	"oz_kernel_486 exception_crash*: sigargs %p, [1] %u\n"

##########################################################################
##									##
##  Physical page access						##
##									##
##  These routines must be called at or above softint level because 	##
##  they all use the cpu-specific spt entry and thus cannot have the 	##
##  cpu switched out from under them					##
##									##
##########################################################################

##########################################################################
##									##
##  Fill a page with zeroes (demand zero page)				##
##									##
##    Input:								##
##									##
##	4(%esp) = physical page number to fill				##
##	8(%esp) = 0 : normal data page, initialize to zeroes		##
##	       else : pt page, check for reqprot's			##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_phys_initpage
	.type	oz_hw_phys_initpage,@function
oz_hw_phys_initpage:
	pushl	%ebp				# make call frame in case of crash
	movl	%esp,%ebp
	pushl	%edi				# save scratch registers
	pushl	%esi
	pushl	%ebx
	call	oz_hw486_getcpu			# see which cpu we're on
	cmpb	$0,CPU_B_SMPLEVEL(%esi)		# make sure at softint or above
	je	phys_crash1
	movl	CPU_L_SPTENTRY(%esi),%edx	# get the spt entry this cpu can use
	pushl	(%edx)				# save the prior contents
	movl	8(%ebp),%eax			# get physical page number to be zeroed
	cmpl	oz_hw486_dynphypagbeg,%eax	# make sure it's not a static page
	jc	phys_crash2_eax
	shll	$12,%eax			# get physical address of page to fill
	movb	$PT_KRW,%al			# set the present and write-enabled bits
	movl	%eax,(%edx)			# map the physical page
	movl	CPU_L_SPTVADDR(%esi),%edi	# point to its virtual address
	INVLPG_EDING				# flush the non-global cache entry for the page
	xorl	%eax,%eax			# fill page with zeroes
	movl	$1024,%ecx			# number of longs in a page
	cld					# fill the page
	rep
	stosl
	subl	$4096,%edi			# point to beginning of page again
	movl	12(%ebp),%eax			# see if pagetable page
	testl	%eax,%eax
	je	page_zeropage_rtn

	subl	$PROCPTRPBVPAGE,%eax		# this is page within the pagetable
	jc	page_zeropage_rtn		# - should never be .lt.

	pushl	%edx				# save pointer to temp spte
	xorl	%edx,%edx
	movl	$17,%ecx
	divl	%ecx
	cmpl	$16,%edx			# see if this is a reqprot page
	jne	page_zeropage_rpdone
	shll	$14,%eax			# this is vpage represented by first two bits of the reqprot page
	addl	$PROCBASVPAGE,%eax
	movl	%eax,%ebx
page_zeropage_rploop1:

	# %ebx = vpage to start scanning at
	# %edi = base address of reqprot page

	pushl	%ebx				# vpage
	movl	%esp,%edx
	pushl	$0				# pageprot
	movl	%esp,%ecx
	leal	8(%ebp),%eax			# a dummy location
	pushl	%eax				# - &mapsecflags
	pushl	%eax				# - &procmode
	pushl	%ecx				# - &pageprot
	pushl	%eax				# - &pageoffs
	pushl	%eax				# - &section
	pushl	%edx				# - &svpage
	call	oz_knl_process_getcur		# process
	pushl	%eax
	call	oz_knl_process_getsecfromvpage2	# find section mapped to svpage or after
	addl	$28,%esp			# wipe call args from stack
	popl	%ecx				# pop resulting pageprot
	popl	%edx				# pop updated svpage
	testl	%eax,%eax			# see if anything found
	je	page_zeropage_rpdone		# if not, we're done

	# %eax = number of pages starting at %edx to mark with reqprot
	# %ebx = initial vpage
	# %ecx = resultant reqprot (zero filled)
	# %edx = updated vpage = max (start of section, initial vpage)
	# %edi = base address of reqprot page

	movl	%edx,%esi			# see if beyond our reqprot page
	xorl	%ebx,%esi
	shrl	$14,%esi
	jne	page_zeropage_rpdone		# if so, we're done
	movl	%edx,%ebx			# this is the page we will start on
	movl	%ecx,%edx			# save reqprot here
	movl	%eax,%esi			# save count here

	# %ebx = vpage at beginning of section
	# %edx = reqprot bits (zero filled)
	# %esi = number of pages in section
	# %edi = base address of reqprot page

page_zeropage_rploop2:
	movb	%bl,%cl				# shift reqprot bits in place
	andb	$3,%cl
	addb	%cl,%cl
	movb	%dl,%al
	shlb	%cl,%al
	movl	%ebx,%ecx			# get pointer to byte holding reqprot bits
	shrl	$2,%ecx
	andl	$4095,%ecx
	orb	%al,(%edi,%ecx)			# store reqprot bits in byte
	incl	%ebx				# increment vpage
	testl	$16383,%ebx			# all done if off the reqprot page
	je	page_zeropage_rpdone
	decl	%esi				# see if end of this section
	jne	page_zeropage_rploop2
	jmp	page_zeropage_rploop1		# end of section, see if any more sections
page_zeropage_rpdone:
	popl	%edx				# restore pointer to temp spte

page_zeropage_rtn:
	popl	(%edx)				# restore the temp spte
	INVLPG_EDING
	popl	%ebx				# restore scratch registers
	popl	%esi
	popl	%edi
	popl	%ebp				# all done
	ret

##########################################################################
##									##
##  Fill a physical page with a given value				##
##									##
##    Input:								##
##									##
##	4(%esp) = longword fill value					##
##	8(%esp) = physical address (page aligned)			##
##									##
##########################################################################

	.align	4
	.globl	oz_hw486_phys_filllong
	.type	oz_hw486_phys_filllong,@function
oz_hw486_phys_filllong:
	pushl	%ebp
	movl	%esp,%ebp
	pushl	%edi
	pushl	%esi

	call	oz_hw486_getcpu			# see which cpu we're on
	cmpb	$0,CPU_B_SMPLEVEL(%esi)		# make sure at softint or above
	je	phys_crash1
	movl	CPU_L_SPTENTRY(%esi),%edi	# get the spt entry this cpu can use
	movl	12(%ebp),%eax			# get physical address
	movl	%eax,%ecx			# make sure it's not in static pages
	shrl	$12,%ecx
	cmpl	oz_hw486_dynphypagbeg,%ecx
	jc	phys_crash2_ecx
	movb	$PT_KRW,%al			# set the present and write-enabled bits
	movl	%eax,(%edi)			# map the physical page
	movl	CPU_L_SPTVADDR(%esi),%edi	# point to its virtual address
	INVLPG_EDING				# flush the non-global cache entry for the page
	movl	$1024,%ecx			# number of longs in a page
	movl	 8(%ebp),%eax			# get the fill pattern
	cld					# fill the page
	rep
	stosl
	movl	CPU_L_SPTENTRY(%esi),%edi	# clear the spt entry
	movl	%ecx,(%edi)			# ... so we would accvio
	movl	CPU_L_SPTVADDR(%esi),%edi
	INVLPG_EDING

	popl	%esi
	popl	%edi
	leave					# pop call frame and return
	ret

##########################################################################
##
##  Store a longword in a physical address
##
##    Input:
##
##	4(%esp) = data
##	8(%esp) = physical address
##
##    Note:
##
##	This routine assumes the address does not cross a page boundary

	.align	4
	.globl	oz_hw486_phys_storelong
	.type	oz_hw486_phys_storelong,@function
oz_hw486_phys_storelong:
	pushl	%ebp				# make a call frame in case of crash
	movl	%esp,%ebp
	pushl	%esi				# save scratch register
	call	oz_hw486_getcpu			# see which cpu we're on
	cmpb	$0,CPU_B_SMPLEVEL(%esi)		# make sure at softint or above
	je	phys_crash1
	movl	CPU_L_SPTVADDR(%esi),%edx	# get the spt entry's base virtual address
	movl	CPU_L_SPTENTRY(%esi),%esi	# get the spt entry this cpu can use
	movl	12(%ebp),%eax			# get physical address
	shrl	$12,%eax			# get physical page number
	cmpl	oz_hw486_dynphypagbeg,%eax	# make sure it's not in static pages
	jc	phys_crash2_eax
	shll	$12,%eax
	movl	(%esi),%ecx			# save previous pte contents
	movb	$PT_KRW,%al			# set the present and read/write bits
	andb	$0xF0,%ah			# page align physical address
	movl	%eax,(%esi)			# map the physical page
	INVLPG_EDXNG				# flush the non-global cache entry for the page
	movl	12(%ebp),%eax			# get physical address again
	andl	$0x0FFF,%eax			# get offset in page
	addl	%eax,%edx			# point to the longword
	movl	8(%ebp),%eax			# get the longword
	movl	%eax,(%edx)			# store it in page
	movl	%ecx,(%esi)			# restore the spt
	INVLPG_EDXNG
	popl	%esi
	leave
	ret

##########################################################################
##
##  Fetch a longword at a physical address
##
##    Input:
##
##	4(%esp) = physical address
##
##    Output:
##
##	%eax = contents of that physical address
##
##    Note:
##
##	This routine assumes the address does not cross a page boundary

	.align	4
	.globl	oz_hw486_phys_fetchlong
	.type	oz_hw486_phys_fetchlong,@function
oz_hw486_phys_fetchlong:
	pushl	%ebp				# make a call frame in case of crash
	movl	%esp,%ebp
	pushl	%esi				# save scratch register
	call	oz_hw486_getcpu			# see which cpu we're on
	cmpb	$0,CPU_B_SMPLEVEL(%esi)		# make sure at softint or above
	je	phys_crash1
	movl	CPU_L_SPTVADDR(%esi),%edx	# get the spt entry's base virtual address
	movl	CPU_L_SPTENTRY(%esi),%esi	# get the spt entry this cpu can use
	movl	8(%ebp),%eax			# get physical address
	movl	(%esi),%ecx			# save previous pte contents
	movb	$PT_KRO,%al			# set the present and read-only bits
	andb	$0xF0,%ah			# page align physical address
	movl	%eax,(%esi)			# map the physical page
	INVLPG_EDXNG				# flush the non-global cache entry for the page
	movl	8(%ebp),%eax			# get physical address again
	andl	$0x0FFF,%eax			# get offset in page
	movl	(%edx,%eax),%eax		# fetch the long from the physical page
	movl	%ecx,(%esi)			# restore the spt
	INVLPG_EDXNG
	popl	%esi
	leave
	ret

#   input: edi = phys addr
#  output: eax = contents
# scratch: esi

	.align	4
fetch_phys_long:
	pushl	%edi				# save physical address
	call	oz_hw486_getcpu			# see which cpu we're on
	cmpb	$0,CPU_B_SMPLEVEL(%esi)		# make sure at softint or above
	je	phys_crash1
	movl	CPU_L_SPTENTRY(%esi),%edi	# get the spt entry this cpu can use
	movl	(%esp),%eax			# get physical address
	pushl	(%edi)				# save previous pte contents
	movb	$PT_KRO,%al			# set the present and read-only bits
	andb	$0xF0,%ah			# page align it
	movl	%eax,(%edi)			# map the physical page
	movl	CPU_L_SPTVADDR(%esi),%edi	# point to its base virtual address
	INVLPG_EDING				# flush the non-global cache entry for the page
	movl	4(%esp),%eax			# get physical address again
	andl	$0x0FFF,%eax			# get offset in page
	movl	(%edi,%eax),%eax		# fetch the long from the physical page
	movl	CPU_L_SPTENTRY(%esi),%edi	# restore the spt
	popl	(%edi)
	movl	CPU_L_SPTVADDR(%esi),%edi
	INVLPG_EDING
	popl	%edi				# pop physical address
	ret

##########################################################################
##
##  General virtual-to-physical copy
##
##    Input:
##
##	 4(%esp)  = number of bytes to copy
##	 8(%esp)  = virtual address of source data
##	12(%esp)  = pointer to array of physical page numbers
##	16(%esp)  = byte offset in first physical page
##	smplevel >= softint (so cpu doesn't switch on us)
##
	.align	4
	.globl	oz_hw_phys_movefromvirt
	.type	oz_hw_phys_movefromvirt,@function
oz_hw_phys_movefromvirt:
	pushl	%ebp				# make a call frame for tracing
	movl	%esp,%ebp
	pushl	%edi				# save C registers
	pushl	%esi
	pushl	%ebx

	cmpl	$0,8(%ebp)			# see if anything to copy
	je	phys_copyfr_null		# don't bother if not

	call	oz_hw486_getcpu			# point to my cpu database
	cmpb	$0,CPU_B_SMPLEVEL(%esi)		# make sure at softint or above
	je	phys_crash1
	movl	%esi,%ebx			# keep pointer safe from movsb
	movl	CPU_L_SPTENTRY(%esi),%eax	# save old spte contents
	pushl	(%eax)

	movl	20(%ebp),%eax			# get offset in page
	andl	$0x0FFF,20(%ebp)		# normalize to a page size
	shrl	$12,%eax			# adjust physical page number array pointer accordingly
	shll	$2,%eax
	addl	%eax,16(%ebp)

	cld
	movl	12(%ebp),%esi			# point to source string buffer
phys_copyfr_loop:
	movl	16(%ebp),%eax			# point to physical page number array
	addl	$4,16(%ebp)			# increment for next time through loop
	movl	(%eax),%eax			# get physical page number
	cmpl	oz_hw486_dynphypagbeg,%eax	# crash if accessing a static page this way
	jc	phys_crash2_eax
	movl	CPU_L_SPTENTRY(%ebx),%ecx	# point to my temp spte
	shll	$12,%eax			# make a pte
	movb	$PT_KRW,%al
	movl	%eax,(%ecx)			# store in pagetable
	movl	CPU_L_SPTVADDR(%ebx),%edi	# get corresponding virtual address
	INVLPG_EDING				# invalidate the non-global spte
	addl	20(%ebp),%edi			# add page offset
	movl	$4096,%ecx			# get number of bytes left in page
	movl	 8(%ebp),%eax			# get how many bytes left to copy
	subl	20(%ebp),%ecx
	movl	$0,20(%ebp)			# clear page offset for next time through loop
	cmpl	%eax,%ecx			# if not enough left in page to do it all, chop off the copy for now
	.byte	0x0F,0x43,0xC8 # cmovnc %eax,%ecx
	subl	%ecx,8(%ebp)			# that fewer bytes next time to copy
	rep					# copy the bytes
	movsb
	cmpl	$0,8(%ebp)			# see if more to copy
	jne	phys_copyfr_loop

	movl	CPU_L_SPTENTRY(%ebx),%ecx	# point to my temp spte
	popl	(%ecx)				# restore to original contents
	movl	CPU_L_SPTVADDR(%ebx),%edi	# get corresponding virtual address
	INVLPG_EDING				# invalidate the non-global spte

phys_copyfr_null:
	popl	%ebx				# restore C registers
	popl	%esi
	popl	%edi
	leave					# pop stack frame
	ret

##########################################################################
##
##  General physical-to-virtual copy
##
##    Input:
##
##	 4(%esp) = number of bytes to copy
##	 8(%esp) = virtual address of destination data
##	12(%esp) = pointer to array of physical page numbers
##	16(%esp) = byte offset in first physical page
##
	.align	4
	.globl	oz_hw_phys_movetovirt
	.type	oz_hw_phys_movetovirt,@function
oz_hw_phys_movetovirt:
	pushl	%ebp				# make a call frame for tracing
	movl	%esp,%ebp
	pushl	%edi				# save C registers
	pushl	%esi
	pushl	%ebx

	cmpl	$0,8(%ebp)			# see if anything to copy
	je	phys_copyto_null		# don't bother if not

	call	oz_hw486_getcpu			# point to my cpu database
	cmpb	$0,CPU_B_SMPLEVEL(%esi)		# make sure at softint or above
	je	phys_crash1
	movl	%esi,%ebx			# keep pointer safe from movsb
	movl	CPU_L_SPTENTRY(%esi),%eax	# save old spte contents
	pushl	(%eax)

	movl	20(%ebp),%eax			# get offset in page
	andl	$0x0FFF,20(%ebp)		# normalize to a page size
	shrl	$12,%eax			# adjust physical page number array pointer accordingly
	shll	$2,%eax
	addl	%eax,16(%ebp)

	cld
	movl	12(%ebp),%edi			# point to destination string buffer
phys_copyto_loop:
	movl	16(%ebp),%eax			# point to physical page number array
	addl	$4,16(%ebp)			# increment for next time through loop
	movl	(%eax),%eax			# get physical page number
  ##	cmpl	oz_hw486_dynphypagbeg,%eax	## crash if accessing a static page this way
  ##	jc	phys_crash2_eax			## it's ok to copy FROM static pages (for dumppmem.oz)
	movl	CPU_L_SPTENTRY(%ebx),%ecx	# point to my temp spte
	shll	$12,%eax			# make a pte
	movb	$PT_KRO,%al			# (we only need to read the page)
	movl	%eax,(%ecx)			# store in pagetable
	movl	CPU_L_SPTVADDR(%ebx),%edx	# get corresponding virtual address
	INVLPG_EDXNG				# invalidate the non-global spte
	addl	20(%ebp),%edx			# add page offset
	movl	$4096,%ecx			# get number of bytes left in page
	movl	 8(%ebp),%eax			# get how many bytes left to copy
	subl	20(%ebp),%ecx
	movl	$0,20(%ebp)			# clear page offset for next time through loop
	cmpl	%eax,%ecx			# if not enough left in page to do it all, chop off the copy for now
	.byte	0x0F,0x43,0xC8 # cmovnc %eax,%ecx
	subl	%ecx,8(%ebp)			# that fewer bytes next time to copy
	movl	%edx,%esi			# set up source pointer
	rep					# copy the bytes
	movsb
	cmpl	$0,8(%ebp)			# see if more to copy
	jne	phys_copyto_loop

	movl	CPU_L_SPTENTRY(%ebx),%ecx	# point to my temp spte
	popl	(%ecx)				# restore to original contents
	movl	CPU_L_SPTVADDR(%ebx),%edx	# get corresponding virtual address
	INVLPG_EDXNG				# invalidate the non-global spte

phys_copyto_null:
	popl	%ebx				# restore C registers
	popl	%esi
	popl	%edi
	leave					# pop stack frame
	ret

##########################################################################
##
##  C-Callable map physical page
##
##    Input:
##
##	4(%esp) = physical page number
##	8(%esp) = where to return prior pte contents (or NULL)
##
##    Output:
##
##	%eax = virtual address it is mapped to

	.align	4
	.globl	oz_hw_phys_mappage
	.type	oz_hw_phys_mappage,@function
oz_hw_phys_mappage:
	pushl	%ebp
	movl	%esp,%ebp
	pushl	%esi

	call	oz_hw486_getcpu			# point to this cpu's data
	cmpb	$0,CPU_B_SMPLEVEL(%esi)		# make sure at softint or above
	je	phys_crash1
	movl	 8(%ebp),%eax			# get physical page number
	cmpl	oz_hw486_dynphypagbeg,%eax	# read-only for static pages
	jc	mappage_static
	shll	$12,%eax			# shift over
	movb	$PT_KRW,%al			# write enable for executive mode
mappage_ok:
	movl	CPU_L_SPTENTRY(%esi),%ecx	# get spt entry we can use
	xchgl	%eax,(%ecx)			# map the physical page, save prior pte contents
	movl	CPU_L_SPTVADDR(%esi),%edx	# get its base virtual address
	movl	12(%ebp),%ecx			# get where to return prior contents
	INVLPG_EDXNG				# flush non-global cache entry for the page
	jecxz	mappage_nortn
	movl	%eax,(%ecx)			# return prior contents
mappage_nortn:
	movl	%edx,%eax			# return virtual address the page is mapped at
	popl	%esi
	leave
	ret

mappage_static:					# - needed for crash dump routine via oz_dev_ide_486.c driver
	shll	$12,%eax			# shift over
	movb	$PT_KRO,%al			# just do read-only access
	jmp	mappage_ok

##	4(%esp) = prior pte contents

	.align	4
	.globl	oz_hw_phys_unmappage
	.type	oz_hw_phys_unmappage,@function
oz_hw_phys_unmappage:
	pushl	%ebp
	movl	%esp,%ebp
	pushl	%esi

	call	oz_hw486_getcpu			# point to this cpu's data
	cmpb	$0,CPU_B_SMPLEVEL(%esi)		# make sure at softint or above
	je	phys_crash1
	movl	CPU_L_SPTENTRY(%esi),%edx	# point to the spt entry
	movl	8(%ebp),%ecx			# get the old contents
	movl	%ecx,(%edx)			# update the pte
	movl	CPU_L_SPTVADDR(%esi),%edx	# get the corresponding virt address
	INVLPG_EDXNG				# invalidate non-global cache entry

	popl	%esi
	leave
	ret

##########################################################################
##
##  General physical-to-physical copy
##
##    Input:
##
##	 4(%esp) = number of bytes to copy
##	 8(%esp) = pointer to array of source physical page numbers
##	12(%esp) = byte offset in first source physical page
##	16(%esp) = pointer to array of destination physical page numbers
##	20(%esp) = byte offset in first destination physical page
##	smplock >= softint (so cpu doesn't switch on us)
##
	.align	4
	.globl	oz_hw_phys_movephys
	.type	oz_hw_phys_movephys,@function
oz_hw_phys_movephys:
	pushl	%ebp				# make a call frame for tracing
	movl	%esp,%ebp
	pushl	%edi				# save C registers
	pushl	%esi
	pushl	%ebx

	cmpl	$0,8(%ebp)			# see if anything to copy
	je	phys_copyphys_done		# don't bother if not

	call	oz_hw486_getcpu			# point to cpu database
	cmpb	$0,CPU_B_SMPLEVEL(%esi)		# make sure at softint or above
	je	phys_crash1
	movl	%esi,%ebx			# save pointer to be safe from movsb instruction
	movl	CPU_L_SPTENTRY(%ebx),%edi	# point to the two spte's we will use
	pushl	4(%edi)				# save their current contents
	pushl	 (%edi)				# ... in case someone else we interrupted was using them

	cld					# tell the movsb to autoincrement
phys_copyphys_loop:
	movl	$4095,%ecx
	movl	16(%ebp),%eax			# normalize source byte offset
	movl	24(%ebp),%edx			# normalize destination byte offset
	andl	%ecx,16(%ebp)
	andl	%ecx,24(%ebp)
	shrl	$12,%eax
	shrl	$12,%edx
	shll	$2,%eax
	shll	$2,%edx
	addl	12(%ebp),%eax
	addl	20(%ebp),%edx
	movl	%eax,12(%ebp)
	movl	%edx,20(%ebp)
	movl	CPU_L_SPTENTRY(%ebx),%edi	# get pointer to spte's to use
	movl	(%eax),%eax			# get source physical page number
	movl	(%edx),%edx			# get destination physical page number
  ##	cmpl	oz_hw486_dynphypagbeg,%eax	## crash if accessing a static page this way
  ##	jc	phys_crash2_eax			## it's ok to copy FROM static page (why waste time checking)
	cmpl	oz_hw486_dynphypagbeg,%edx	# crash if writing a static page this way
	jc	phys_crash2_edx
	shll	$12,%eax			# make pte contents
	shll	$12,%edx			# make pte contents
	movb	$PT_KRO,%al			# - kernel read-only access
	movb	$PT_KRW,%dl			# - kernel read/write access
	movl	%eax, (%edi)			# write the source spte
	movl	%edx,4(%edi)			# write destination spte
	movl	CPU_L_SPTVADDR(%ebx),%edi	# invalidate both non-global cache entries
	movl	$4096,%eax
	INVLPG_EDING
	movl	%edi,%esi			# (save source page pointer)
	addl	%eax,%edi			# (set up dest page pointer)
	INVLPG_EDING
	movl	8(%ebp),%ecx			# see how much the caller wants copied
	movl	%eax,%edx
	subl	16(%ebp),%eax			# see how much is left in source page to copy
	subl	24(%ebp),%edx			# see how much is left in destination page to copy
	cmpl	%eax,%ecx			# if not enough left in source page to do it all, chop off the copy for now
	.byte	0x0F,0x43,0xC8 # cmovnc %eax,%ecx
	addl	16(%ebp),%esi			# add in source page offset
	cmpl	%edx,%ecx			# if not enough left in dest page to do it all, chop off the copy for now
	.byte	0x0F,0x43,0xCA # cmovnc %edx,%ecx
	addl	24(%ebp),%edi			# add in destination page offset
	subl	%ecx, 8(%ebp)			# decrement remaining byte count for next loop
	addl	%ecx,16(%ebp)			# increment offsets in case we need to loop back
	addl	%ecx,24(%ebp)
	rep					# copy the bytes
	movsb
	cmpl	$0,8(%ebp)			# see if anything more to copy
	jne	phys_copyphys_loop		# repeat if so

	movl	CPU_L_SPTENTRY(%ebx),%edi	# get pointer to spte's we used
	popl	 (%edi)				# restore original contents
	popl	4(%edi)
	movl	CPU_L_SPTVADDR(%ebx),%edx	# invalidate both non-global cache entries
	INVLPG_EDXNG
	addl	$4096,%edx
	INVLPG_EDXNG

phys_copyphys_done:
	popl	%ebx				# restore C registers
	popl	%esi
	popl	%edi
	leave					# pop stack frame
	ret

##########################################################################
##
##  Crash routine if called with softint delivery enabled or mapping static page
##

phys_crash1:
	pushl	4(%ebp)
	pushl	$phys_msg1
	call	oz_crash

phys_crash2_eax:
	pushl	%eax
	jmp	phys_crash2
phys_crash2_ecx:
	pushl	%ecx
	jmp	phys_crash2
phys_crash2_edx:
	pushl	%edx
	jmp	phys_crash2
phys_crash2_edi:
	pushl	%edi
phys_crash2:
	pushl	$phys_msg2
	call	oz_crash

phys_msg1:	.string	"oz_hw_phys: called from %p with softint delivery enabled"
phys_msg2:	.string	"oz_hw_phys: mapping static page 0x%X"

##########################################################################
##									##
##  Debugging routine to output a single character in %al, in red	##
##  Note: this routine does not scroll so it is limited in its use	##
##									##
##########################################################################

	.align	4
	.globl	oz_knl_debcharp
	.globl	oz_knl_debchar
	.type	oz_knl_debcharp,@function
	.type	oz_knl_debchar,@function
oz_knl_debcharp:
	movl	4(%esp),%eax
oz_knl_debchar:
	pushal				## save all registers
	pushl	%eax			## save character where it's easy to get
	movw	$0x03cc,%dx		## read the misc output register
	inb	%dx
	IODELAY
	movw	$0x03b4,%dx		## assume use 03b4/03b5
	and	$1,%al
	je	dc_video_init_useit
	movw	$0x03d4,%dx		## if misc<0> set, use 03d4/03d5
dc_video_init_useit:
	xorl	%ecx,%ecx		## read cursor into %ecx...
	movb	$0x0e,%al		## ... output 0e -> 03?4
	outb	%dx
	IODELAY
	incw	%dx			## ... input 03?5 -> al
	inb	%dx
	IODELAY
	movb	%al,%ch			## ... that's high order cursor addr
	decw	%dx			## ... output 0f -> 03?4
	movb	$0x0f,%al
	outb	%dx
	IODELAY
	incw	%dx			## ... input 03?5 -> al
	inb	%dx
	IODELAY
	movb	%al,%cl			## ... that's low order cursor addr
	movl	$0xb8000,%edi		## point to base of video memory
	addl	%ecx,%edi		## add twice the cursor offset
	addl	%ecx,%edi
	popl	%eax			## get character to display
	movb	$4,%ah			## make the colour be red
	movw	%ax,(%edi)		## display it
	incw	%cx			## increment cursor position
	decw	%dx			## output 0e -> 03?4
	movb	$0x0e,%al
	outb	%dx
	IODELAY
	incw	%dx			## output %ch -> 03?5
	movb	%ch,%al
	outb	%dx
	IODELAY
	decw	%dx			## output 0f -> 03?4
	movb	$0x0f,%al
	outb	%dx
	IODELAY
	incw	%dx			## output %cl -> 03?5
	movb	%cl,%al
	outb	%dx

getchar_wait:
	pushl	$1
	call	oz_dev_keyboard_getc
	addl	$4,%esp
	cmpb	$32,%al
	jne	getchar_wait

	popal				## restore all registers
	ret				## all done

##########################################################################
##									##
##  Map an I/O page (uncached)						##
##									##
##	 4(%esp) = physical page number					##
##	 8(%esp) = system virt address corresponding to spte		##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_map_iopage
	.type	oz_hw_map_iopage,@function
oz_hw_map_iopage:
	pushl	%ebp
	movl	%esp,%ebp
	pushl	%esi
	pushl	%ebx
	movl	12(%ebp),%edx		# get system virt address where the iopage will be mapped

	movl	oz_s_sysmem_pagtblsz,%eax
	movl	%edx,%ecx		# get corresponding system page number
	shrl	$12,%ecx
	cmpl	%eax,%ecx
	jnc	map_iopage_crash1

	movl	8(%ebp),%eax		# get physical page number
	movl	%eax,%ebx
	shll	$12,%eax		# shift to get physical address
	je	map_iopage_set		# if zero, shut it off
	orl	pte_validw_gbl_krw_nc,%eax # VALID_W, GLOBAL, KERNEL READ/WRITE, NO CACHE
	cmpl	$0x0A0,%ebx		# don't allow them to map anything below hole so they can't wipe it out
	jc	map_iopage_crash2
	cmpl	$0x100,%ebx		# but allow them to map something in the hole so they can get their own BIOS
	jc	map_iopage_set
	cmpl	oz_hw486_dynphypagbeg,%ebx # don't allow access to any of the static pages above the hole so they can't wipe them
	jc	map_iopage_crash2
map_iopage_set:
	movl	%eax,SPTBASE(,%ecx,4)	# store in page table entry
	INVLPG_EDX			# invalidate the cache entry on this cpu
	call	oz_hw486_apic_invlpgedx	# invalidate it on other cpu's, too, so the pagefault 
					# ... handler will leave the no-cache bits alone
	popl	%ebx
	popl	%esi
	leave
	ret

map_iopage_crash1:
	pushl	%eax
	pushl	%ecx
	pushl	12(%ebp)
	pushl	 8(%ebp)
	pushl	$map_iopage_msg1
	call	oz_crash

map_iopage_crash2:
	pushl	oz_hw486_dynphypagbeg
	pushl	%ebx
	pushl	12(%ebp)
	pushl	 8(%ebp)
	pushl	$map_iopage_msg2
	call	oz_crash

map_iopage_msg1:	.string	"oz_hw_map_iopage (0x%X, 0x%X): system page %X out of range of %X"
map_iopage_msg2:	.string	"oz_hw_map_iopage (0x%X, 0x%X): cannot map static page %X (oz_hw486_dynphypagbeg=%X)"

##########################################################################
##									##
##  Fetch and store extended machine arguments				##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_mchargx_knl_fetch
	.type	oz_hw_mchargx_knl_fetch,@function
oz_hw_mchargx_knl_fetch:
	pushl	%ebp
	movl	%esp,%ebp
	movl	8(%ebp),%ecx
	movw	%ds,%ax
	movw	%ax,MCHXK_W_DS(%ecx)
	movw	%es,%ax
	movw	%ax,MCHXK_W_ES(%ecx)
	movw	%fs,%ax
	movw	%ax,MCHXK_W_FS(%ecx)
	movw	%gs,%ax
	movw	%ax,MCHXK_W_GS(%ecx)
	movw	%ss,%ax
	movw	%ax,MCHXK_W_SS(%ecx)
	MOVL_CR0_EAX
	movl	%eax,MCHXK_L_CR0(%ecx)
	MOVL_CR2_EAX
	movl	%eax,MCHXK_L_CR2(%ecx)
	MOVL_CR3_EAX
	movl	%eax,MCHXK_L_CR3(%ecx)
	MOVL_CR4_EAX
	movl	%eax,MCHXK_L_CR4(%ecx)
	leave
	ret

	.align	4
	.globl	oz_hw_mchargx_knl_store
	.type	oz_hw_mchargx_knl_store,@function
oz_hw_mchargx_knl_store:
	pushl	%ebp
	movl	%esp,%ebp
	movl	8(%ebp),%ecx
	movw	MCHXK_W_DS(%ecx),%ax
	movw	%ax,%ds
	movw	MCHXK_W_ES(%ecx),%ax
	movw	%ax,%es
	movw	MCHXK_W_FS(%ecx),%ax
	movw	%ax,%fs
	movw	MCHXK_W_GS(%ecx),%ax
	movw	%ax,%gs
	movw	MCHXK_W_SS(%ecx),%ax
	movw	%ax,%ss
	movl	MCHXK_L_CR0(%ecx),%eax
	MOVL_EAX_CR0
.if !PARANOID_MPD
	movl	MCHXK_L_CR3(%ecx),%eax
	MOVL_EAX_CR3
.endif
	movl	MCHXK_L_CR4(%ecx),%eax
	MOVL_EAX_CR4
	leave
	ret

	.align	4
	.globl	oz_hw_mchargx_usr_fetch
	.type	oz_hw_mchargx_usr_fetch,@function
oz_hw_mchargx_usr_fetch:
	pushl	%ebp
	movl	%esp,%ebp
	movl	8(%ebp),%ecx
	movw	%ds,%ax
	movw	%ax,MCHXU_W_DS(%ecx)
	movw	%es,%ax
	movw	%ax,MCHXU_W_ES(%ecx)
	movw	%fs,%ax
	movw	%ax,MCHXU_W_FS(%ecx)
	movw	%gs,%ax
	movw	%ax,MCHXU_W_GS(%ecx)
	movw	%ss,%ax
	movw	%ax,MCHXU_W_SS(%ecx)
	leave
	ret

	.align	4
	.globl	oz_hw_mchargx_usr_store
	.type	oz_hw_mchargx_usr_store,@function
oz_hw_mchargx_usr_store:
	pushl	%ebp
	movl	%esp,%ebp
	movl	8(%ebp),%ecx
	movw	MCHXU_W_DS(%ecx),%ax
	movw	%ax,%ds
	movw	MCHXU_W_ES(%ecx),%ax
	movw	%ax,%es
	movw	MCHXU_W_FS(%ecx),%ax
	movw	%ax,%fs
	movw	MCHXU_W_GS(%ecx),%ax
	movw	%ax,%gs
	movw	MCHXU_W_SS(%ecx),%ax
	movw	%ax,%ss
	leave
	ret

##########################################################################
##									##
##  This reboots the system by wiping the MPD				##
##									##
##	oz_hw_reboot ()							##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_reboot
	.type	oz_hw_reboot,@function
oz_hw_reboot:
.if PARANOID_MPD
	pushl	$reboot_msg1
	call	oz_crash
reboot_msg1:	.string	"oz_hw_reboot: too paranoid to reboot"
.else
	int	$INT_REBOOT
	jmp	oz_hw_reboot
.endif

##########################################################################
##									##
##  This halts the cpu by by doing an hlt instruction in an infinite 	##
##  loop with all interrupt delivery disabled				##
##									##
##	oz_hw_halt ()							##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_halt
	.type	oz_hw_halt,@function
oz_hw_halt:
	call	oz_hw486_getcpu		# get my cpu index number in %eax
	lock				# clear the active cpu bit
	btr	%eax,oz_hw486_activecpus
	call	oz_hw486_apic_shut	# shut off the apic interrupts
	int	$INT_HALT		# do the real halting here

##########################################################################
##									##
##  Flush processor cache						##
##									##
##########################################################################

	.align	4
	.globl	oz_hw486_wbinvd
	.type	oz_hw486_wbinvd,@function
oz_hw486_wbinvd:
	WBINVLD
	ret

##########################################################################
##									##
##  These int calls are made by executive level code to do things 	##
##  that can only be done in kernel mode				##
##									##
##########################################################################

.if EXECODESEG <> KNLCODESEG

## Invalidate page pointed to by a register
## If it's part of the SPT, clear CR4<PGE> so it really gets invalidated

	.align	4
except_invlpg_edx:
	cmpl	oz_hw486_hiestgvadp1,%edx
	jc	except_invlpg_edx_g
	invlpg	(%edx)
	iret
except_invlpg_edx_g:
	pushl	%ecx
	movl	%cr4,%ecx
	andb	$0x7F,%cl
	movl	%ecx,%cr4
	invlpg	(%edx)
	orb	$0x80,%cl
	movl	%ecx,%cr4
	popl	%ecx
	iret

	.align	4
except_invlpg_edi:
	cmpl	oz_hw486_hiestgvadp1,%edi
	jc	except_invlpg_edi_g
	invlpg	(%edi)
	iret
except_invlpg_edi_g:
	pushl	%ecx
	movl	%cr4,%ecx
	andb	$0x7F,%cl
	movl	%ecx,%cr4
	invlpg	(%edi)
	orb	$0x80,%cl
	movl	%ecx,%cr4
	popl	%ecx
	iret

	.align	4
except_invlpg_edxng:		# (%edx) known to not be mapped by a global pte
	invlpg	(%edx)
	iret

	.align	4
except_invlpg_eding:		# (%edi) known to not be mapped by a global pte
	invlpg	(%edi)
	iret

# Invalidate all non-global pages

	.align	4
except_invlpg_allng:
	movl	%cr3,%eax
	movl	%eax,%cr3
	iret

# Clear TS bit to enable FPU access

	.align	4
except_clrts:
	clts
	iret

# Set TS bit to cause FPU to generate an exception_NM call

	.align	4
except_setts:
	movl	%cr0,%eax
	orb	$8,%al
	movl	%eax,%cr0
	iret

# Load EAX from CR0 (misc control bits)

	.align	4
except_movl_cr0_eax:
	movl	%cr0,%eax
	iret

# Load EAX from CR2 (gets pagefault's virtual address)

	.align	4
except_movl_cr2_eax:
	movl	%cr2,%eax
	iret

# Load EAX from CR3 (gets phys address of MPD)

	.align	4
except_movl_cr3_eax:
	movl	%cr3,%eax
	iret

# Store EAX in CR3 (sets phys address of MPD)

.if !PARANOID_MPD
	.align	4
except_movl_eax_cr3:
	movl	%eax,%cr3
	iret
.endif

# Flush write cache

	.align	4
except_wbinvld:
	wbinvd
	iret

# Load EAX from CR4

	.align	4
except_movl_cr4_eax:
	movl	%cr4,%eax
	iret

# Store EAX in CR0

	.align	4
except_movl_eax_cr0:
	movl	%eax,%cr0
	iret

# Store EAX in CR4

	.align	4
except_movl_eax_cr4:
	movl	%eax,%cr4
	iret

.endif

# Halt the CPU permanently

	.align	4
except_halt:
	hlt
	jmp	except_halt

# Reboot the system

except_reboot:
	cli				# inhibit interrupt delivery
	movl	%cr4,%eax		# turn off global pages
	andb	$0x7F,%al
	movl	%eax,%cr4
	movl	%cr0,%eax		# turn off write-protect of MPD, etc
	andl	$0xFFFEFFFF,%eax
	movl	%eax,%cr0
	xorl	%eax,%eax		# clear out the mpd
	movl	$1024,%ecx
	movl	$MPDBASE,%edi
	cld
	rep
	stosl
	movl	$MPDBASE,%eax		# set the new mpd base address
	movl	%eax,%cr3		# whump!!
	iret

