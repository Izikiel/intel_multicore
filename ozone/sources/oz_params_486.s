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

ENABLE_PROFILING = 1		# enable kernel profiling
MAXCPUS = 4			# maximum number of cpu's to support

## these values must match what is in ozone_boot_486.s, oz_hw_486.h, oz_params_486.h, oz_preloader_486.s

LDRBASE = 0xA000		# loader only - base address of this routine
PRMBASE = 0x9000		# loader only - where the parameter block got read into
IDTBASE = PRMBASE - 0x1000	# loader only - base address of the IDT (includes IDT itself and the pushb/jmp's)
GDTSIZE = 32			# loader only - number of bytes in the GDT
GDTBASE = IDTBASE - GDTSIZE	# loader only - base address of the GDT

ALTCPUBASE = 0x00001000		# base address of alternate cpu startup routine
ALTCPUPAGE = ALTCPUBASE/4096	# ... corresponding page number
  APICBASE = ALTCPUBASE		# - note: the ALTCPU page gets overlayed by the APIC when the kernel boots
				#         the physical page remains intact but the virtual address gets 
				#         mapped to point to the APIC registers
MPDBASE = ALTCPUBASE+0x1000	# base address of the master page directory
SPTBASE = MPDBASE+0x1000	# base address of the system page table
SPTPAGE = SPTBASE/4096		# page number of the SPT

CONCODESEG = 0x08		# [1] conforming code selector
				#     the KNLCODESEG,KNLDATASEG,USRCODESEG,USRDATASEG segments must remain
				#     in the order shown so the sysenter/sysexit instructions will work
KNLCODESEG = 0x10		# [2] segment selector value for kernel code segments
KNLDATASEG = 0x18		# [3] segment selector value for kernel data segments (used only for the kernel stack)
USRCODESEG = 0x23		# [4] user code selector
USRDATASEG = 0x2B		# [5] user data selector (used for kernel, executive and user data, and user stack)
EXECODESEG = KNLCODESEG # 0x31	# [6] executive code selector
EXEDATASEG = KNLDATASEG # 0x39	# [7] executive data selector (used for executive stack)
TASKSEG    = 0x40		# [8...] task segment selectors, one per cpu

HOLEPAGE    = 0x0A0		# page number of the hole (640k)
KERNELPAGE  = 0x100		# load kernel starting at this page = just after the hole (1M)

MINSYSPAGES = 1024/4		# needs a minimum of an extra 1M of memory beyond end of kernel
MAXSYSPAGES = 512*1024/4	# max possible total number of system pages 
				# = 512*17/16Kb of spt (so it all fits below hole) 
				# = 128K pages = 512Mbytes

KSTACKMIN = 8192		# minimum kernel stack size

## MPD entry protection bits

MPD_BITS = 7	# any mode has r/w access to the pages, let the individual entries restrict as needed

## Pagetable entry protection bits

PT_NUL = 0	# no access
PT_KRO = 1	# kernel can read, user cant access at all
PT_KRW = 3	# kernel can read and write, user cant access at all
PT_URO = 5	# kernel and user can read, nobody can write
PT_URW = 7	# kernel and user can read and write

PT_NC = 0x18	# disable caching this page (used for accessing I/O pages)
PT_G  = 0x100	# global bit, doesn't flush from cache when MPD base changed

PD_4M = 0x80	# 4meg page mode

## These parameters determine process memory allocation

PROCMPDVPAGE = 0x000FFFFE					# page of the process' own MPD
PROCMPDVADDR = 0xFFFFE000					# address of the process' own MPD
								# first PROCVIRTBASE/4M entries are exact copy system MPD
								# the rest are pointers to the PROCPTBVADDR pages

onemeg = 1024*1024
pages_left_after_maxsyspages = (onemeg-MAXSYSPAGES)&(-16384)
ptrp_bytes_for_pages_left_after_maxsyspages = (pages_left_after_maxsyspages*17/16*4)
PROCPTRPBVADDR = PROCMPDVADDR-ptrp_bytes_for_pages_left_after_maxsyspages

PROCPTRPBVPAGE = (PROCPTRPBVADDR/4096)&0x000FFFFF		# virtual address of the process' own page tables 

PROCPTRPPAGES  = PROCMPDVPAGE-PROCPTRPBVPAGE			# number of pages that the process page table occupies

PROCBASVPAGE = onemeg-pages_left_after_maxsyspages		# starting virtual page of a process (0x20000)
PROCBASVADDR = PROCBASVPAGE*4096				# corresponding virtual address (0x20000000)

	# miscellaneous interrupt vectors

	INT_LOWIPL       = 0x20		# low priority for apic tskpri
					# (must be same block of 16 as INT_QUANTIM so inhibiting softint delivery inhibits INT_QUANTIM interrupts)
	  L2_INT_SOFTINT = 5		# log2 (INT_SOFTINT)

	INT_SYSCALL      = 0x21		# system calls
	INT_PRINTKP      = 0x22		# temp call to access oz_knl_printk(p)
	INT_GETNOW       = 0x23		# get current date/time in edx:eax (must match oz_common_486.s)
	INT_QUANTIM      = 0x24		# quantum timer (must be same block of 16 as INT_SOFTINT)
	INT_CHECKASTS    = 0x25		# check for outer mode ast's
	INT_DIAGMODE     = 0x26		# put all processors in diag mode (via APIC - see oz_hw_diag)
	INT_RESCHED      = 0x27		# cause a reschedule interrupt on the cpu
	INT_NEWUSRSTK    = 0x28		# there is a new user stack for this thread
	INT_FAKENMI      = 0x29		# fake an nmi (oz_hw_smproc_486.s only)
	INT_WAITLOOPWAKE = 0x2A		# wake from the waitloop's HLT (oz_hw_smproc_486.s only)
	INT_DOIRQEAX     = 0x2B		# do irq given by %eax (oz_hw_uniproc_486.s only)
	INT_IRQBASE      = 0x30		# irq interrupt vector base (16 vectors) (oz_hw_uniproc_486.s only)
	INT_INVLPG_EDX   = 0x40		# executive mode calls these to perform the given instruction in kernel mode
	INT_INVLPG_EDI   = 0x41
	INT_INVLPG_EDXNG = 0x42
	INT_INVLPG_EDING = 0x43
	INT_CLRTS        = 0x44
	INT_SETTS        = 0x45
	INT_MOVL_CR0_EAX = 0x46
	INT_MOVL_CR2_EAX = 0x47
	INT_MOVL_CR3_EAX = 0x48
	INT_MOVL_EAX_CR3 = 0x49
	INT_WBINVLD      = 0x4A
	INT_MOVL_CR4_EAX = 0x4B
	INT_MOVL_EAX_CR4 = 0x4C
	INT_HALT         = 0x4D
	INT_INVLPG_ALLNG = 0x4E
	INT_REBOOT       = 0x4F
	INT_MOVL_EAX_CR0 = 0x50

	INT_LINUX        = 0x80		# old left-over Linux calls
					# see file glibc-2.0.6/sysdeps/unix/sysv/linux/i386/sysdep.h
	INT_IOAPIC_0     = 0x74		# IOAPIC interrupt vectors (oz_hw_smproc_486.s only)
	INT_IOAPIC_1     = 0x7C
	INT_IOAPIC_2     = 0x84
	INT_IOAPIC_3     = 0x8C
	INT_IOAPIC_4     = 0x94
	INT_IOAPIC_5     = 0x9C
	INT_IOAPIC_6     = 0xA4
	INT_IOAPIC_7     = 0xAC
	INT_IOAPIC_8     = 0xB4
	INT_IOAPIC_9     = 0xBC
	INT_IOAPIC_10    = 0xC4
	INT_IOAPIC_11    = 0xCC
	INT_IOAPIC_12    = 0xD4
	INT_IOAPIC_13    = 0xDC
	INT_IOAPIC_14    = 0xE4
	INT_IOAPIC_15    = 0xEC
	INT_INTERVAL     = 0xEE		# interval timer (oz_hw_smproc_486.s only)
	INT_INVLPG_SMP   = 0xFE		# invalidate pages in smp system

	# Advanced Programmable Interrupt Controller registers

	APIC_L_LCLID   = APICBASE+0x0020
	APIC_L_LCLVER  = APICBASE+0x0030
	APIC_L_TSKPRI  = APICBASE+0x0080
	APIC_L_ARBPRI  = APICBASE+0x0090
	APIC_L_PROCPRI = APICBASE+0x00A0
	APIC_L_EOI     = APICBASE+0x00B0
	APIC_L_LOGDST  = APICBASE+0x00D0
	APIC_L_DSTFMT  = APICBASE+0x00E0
	APIC_L_SPURINT = APICBASE+0x00F0
	APIC_L_ISR0    = APICBASE+0x0100
	APIC_L_TMR0    = APICBASE+0x0180
	APIC_L_IRR0    = APICBASE+0x0200
	APIC_L_ERRSTAT = APICBASE+0x0280
	APIC_L_ICREG0  = APICBASE+0x0300
	APIC_L_ICREG1  = APICBASE+0x0310
	APIC_L_LCLVTBL = APICBASE+0x0320
	APIC_L_PERFCNT = APICBASE+0x0340
	APIC_L_LINT0   = APICBASE+0x0350
	APIC_L_LINT1   = APICBASE+0x0360
	APIC_L_LERROR  = APICBASE+0x0370
	APIC_L_TIMINIT = APICBASE+0x0380
	APIC_L_TIMCURC = APICBASE+0x0390
	APIC_L_TIMDVCR = APICBASE+0x03E0

	# SMP locks

	SL_B_LEVEL  =  0			# smp lock level - this value is never modified
	SL_B_CPUID  =  1			# cpu index that has it locked
						# -1 if not locked
	SL__SIZE    =  2			# size of struct
						# must match oz_common_486.s
						# must be no larger than OZ_SMPLOCK_SIZE

	OZ_SMPLOCK_LEVEL_IRQS = 0xE0		# SMP levels used for IRQ levels - must match oz_hw_486.h

	# TSS structure (as defined by hardware)
	# - this is where the cpu gets the kernel or executive stack pointer
	#   when it switches from an outer to an inner mode (like from an interrupt or exception)
	# - there is one of these per cpu

	TSS_L_ESP0 =   4			# offset of kernel stack pointer
	TSS_W_SS0  =   8			# offset of kernel stack segment
	TSS_L_ESP1 =  12			# offset of executive stack pointer
	TSS_W_SS1  =  16			# offset of executive stack segment
						# the rest of the TSS is zeroed out
	TSS__SIZE  = 104			# minimum size that a TSS can be

	# Per-CPU data (in oz_hw486_cpudb)

	CPU_L_PRIORITY     =  0			# current smp lock level and thread priority
	  CPU_B3_THREADPRI = CPU_L_PRIORITY+0	# - thread priority is low 24 bits
	  CPU_B_SMPLEVEL   = CPU_L_PRIORITY+3	# - smp level is high 8 bits
	CPU_L_SPTENTRY     =  4			# cpu's own spt entry for accessing physical pages
						# - there are two pages here
	CPU_L_SPTVADDR     =  8			# virtual address mapped by CPU_L_SPTENTRY spt
						# - covers two pages
	CPU_L_THCTX        = 12			# current thread hardware context pointer
	CPU_L_PRCTX        = 16			# current process hardware context pointer
	CPU_L_INVLPG_VADR  = 20			# virtual address to be invalidated
	CPU_L_INVLPG_CPUS  = 24			# bitmask of cpu's that haven't invalidated yet
	CPU_Q_QUANTIM      = 28			# quantum timer expiration (absolute biased RDTSC value)
	CPU_L_MPDBASE      = 36			# per cpu MPD base (PARANOID_MPD mode only)
	CPU_Q_RDTSCBIAS    = 40			# bias to keep RDTSC the same on all CPU's
	CPU_X_TSS          = 48			# TSS struct for the cpu
	CPU__SIZE          = 48+TSS__SIZE	# size of struct

	CPU__L2SIZE = 8				# power of 2 that is >= CPU__SIZE
.if ((1<<CPU__L2SIZE)<CPU__SIZE)
	error : (1<<CPU__L2SIZE) < CPU__SIZE
.endif
	CPU__SIZE = 1<<CPU__L2SIZE		# now make table entries exactly that size

	# Hardware context saved in OZ_Thread block

	THCTX_L_USTKSIZ  =   0		# number of pages in user stack
	THCTX_L_USTKVPG  =   4		# virtual page of base of user stack
	THCTX_L_ESTACKVA =   8		# executive stack top virtual address (initial value of esp)
	THCTX_L_EXESP    =  12		# saved executive stack pointer at context switch
	THCTX_L_FPUSED   =  16		# <00> = 0 : fp not used last time around
					#        1 : fp was used last time around
					# <01> = 0 : THCTX_X_FPSAVE has not been initialized
					#        1 : THCTX_X_FPSAVE has been initialized
	THCTX_X_FPSAVE   =  32		# floating point save area (512+16 bytes)
					# (extra 16 bytes are for 16-byte alignment)
	THCTX__SIZE      =  32+16+512	# size of hardware thread context block
					# - must be no larger than OZ_THREAD_HW_CTX_SIZE

	# What oz_hw_thread_switchctx expects on the stack when it switches
	# (this is pointed to by THCTX_L_EXESP when the thread is not active in a cpu)

	THSAV_L_ES0 =  0		# top of executive stack (for TSS block)
	THSAV_L_EBX =  4		# saved C registers
	THSAV_L_ESI =  8
	THSAV_L_EDI = 12
        THSAV_L_EFL = 16		# eflags (in case IOPL is different)
	THSAV_L_EBP = 20		# saved frame pointer
	THSAV_L_EIP = 24		# saved instruction pointer
	THSAV__SIZE = 28

	# Machine argument list, OZ_Mchargs in oz_hw_486.h must match this

	MCH_L_EC2     =  0
	MCH_L8_PUSHAL =  4
	MCH_L_EDI     =  4
	MCH_L_ESI     =  8
	MCH_L_EC1     = 12
	MCH_L_ESP     = 16
	MCH_L_EBX     = 20
	MCH_L_EDX     = 24
	MCH_L_ECX     = 28
	MCH_L_EAX     = 32
	MCH_L_EBP     = 36
	MCH_L_EIP     = 40
	MCH_W_CS      = 44
	MCH_W_PAD1    = 46
	MCH_L_EFLAGS  = 48
	MCH__SIZE     = 52

	MCH_L_XSP     = 52	# not part of mchargs returned to user, but this is where the outer mode stack pointer is in an exception handler
	MCH_W_XSS     = 56	# ditto for the outer mode stack segment register

	# Extended machine arguments for kernel and user modes (must match oz_hw_486.h OZ_Mchargx_knl and OZ_Mchargx_usr structs)

	MCHXK_W_DS    =  0
	MCHXK_W_ES    =  2
	MCHXK_W_FS    =  4
	MCHXK_W_GS    =  6
	MCHXK_W_SS    =  8
	MCHXK_W_PAD1  = 10
	MCHXK_L_CR0   = 12
	MCHXK_L_CR2   = 16
	MCHXK_L_CR3   = 20
	MCHXK_L_CR4   = 24

	MCHXU_W_DS    =  0
	MCHXU_W_ES    =  2
	MCHXU_W_FS    =  4
	MCHXU_W_GS    =  6
	MCHXU_W_SS    =  8
	MCHXU_W_PAD1  = 10

# IRQ_MANY struct - MUST match what is in oz_hw_486.h

	IRQ_MANY_NEXT  =  0	# pointer to next in list
	IRQ_MANY_ENTRY =  4	# handler's entrypoint
	IRQ_MANY_PARAM =  8	# handler's parameter
	IRQ_MANY_DESCR = 12	# pointer to description string
	IRQ_MANY_SMPLK = 16	# handler's smp lock
				# (two unused bytes)
	IRQ_MANY_SIZE  = 20	# size of the block

# Macro to insert short delay after an I/O instruction

.macro	IODELAY
	## call oz_hw486_iodelay
	.endm

# Macros that either do the instruction directly or do it via an int call
# If the EXECODESEG is the same as the KNLCODESEG, it means the kernel is running in real kernel mode, so do the instruction directly
# Otherwise, it means the kernel is really running in executive mode, so do the instruction via an int call

.if EXECODESEG == KNLCODESEG

.macro	INVLPG_EDX
	cmpl	oz_hw486_hiestgvadp1,%edx
	jc	except_invlpg_edx_g_\@
	invlpg	(%edx)
	jmp	except_invlpg_edx_\@
except_invlpg_edx_g_\@:
	pushl	%ecx
	movl	%cr4,%ecx
	andb	$0x7F,%cl
	movl	%ecx,%cr4
	invlpg	(%edx)
	orb	$0x80,%cl
	movl	%ecx,%cr4
	popl	%ecx
except_invlpg_edx_\@:
	.endm

.macro	INVLPG_EDI
	cmpl	oz_hw486_hiestgvadp1,%edi
	jc	except_invlpg_edi_g_\@
	invlpg	(%edi)
	jmp	except_invlpg_edi_\@
except_invlpg_edi_g_\@:
	pushl	%ecx
	movl	%cr4,%ecx
	andb	$0x7F,%cl
	movl	%ecx,%cr4
	invlpg	(%edi)
	orb	$0x80,%cl
	movl	%ecx,%cr4
	popl	%ecx
except_invlpg_edi_\@:
	.endm

.macro	INVLPG_EDXNG
	invlpg	(%edx)
	.endm

.macro	INVLPG_EDING
	invlpg	(%edi)
	.endm

.macro	CLRTS
	clts
	.endm

.macro	SETTS
	movl	%cr0,%eax
	orb	$8,%al
	movl	%eax,%cr0
	.endm

.macro	MOVL_CR0_EAX
	movl	%cr0,%eax
	.endm

.macro	MOVL_CR2_EAX
	movl	%cr2,%eax
	.endm

.macro	MOVL_CR3_EAX
	movl	%cr3,%eax
	.endm

.macro	MOVL_EAX_CR3
	movl	%eax,%cr3
	.endm

.macro	WBINVLD
	wbinvd
	.endm

.macro	MOVL_CR4_EAX
	movl	%cr4,%eax
	.endm

.macro	MOVL_EAX_CR4
	movl	%eax,%cr4
	.endm

.macro	MOVL_EAX_CR0
	movl	%eax,%cr0
	.endm

.macro	INVLPG_ALLNG
	movl	%cr3,%eax
	movl	%eax,%cr3
	.endm

.else

.macro	INVLPG_EDX
	int	$INT_INVLPG_EDX
	.endm

.macro	INVLPG_EDI
	int	$INT_INVLPG_EDI
	.endm

.macro	INVLPG_EDXNG
	int	$INT_INVLPG_EDXNG
	.endm

.macro	INVLPG_EDING
	int	$INT_INVLPG_EDING
	.endm

.macro	CLRTS
	int	$INT_CLRTS
	.endm

.macro	SETTS
	int	$INT_SETTS
	.endm

.macro	MOVL_CR0_EAX
	int	$INT_MOVL_CR0_EAX
	.endm

.macro	MOVL_CR2_EAX
	int	$INT_MOVL_CR2_EAX
	.endm

.macro	MOVL_CR3_EAX
	int	$INT_MOVL_CR3_EAX
	.endm

.macro	MOVL_EAX_CR3
	int	$INT_MOVL_EAX_CR3
	.endm

.macro	WBINVLD
	int	$INT_WBINVLD
	.endm

.macro	MOVL_CR4_EAX
	int	$INT_MOVL_CR4_EAX
	.endm

.macro	MOVL_EAX_CR4
	int	$INT_MOVL_EAX_CR4
	.endm

.macro	MOVL_EAX_CR0
	int	$INT_MOVL_EAX_CR0
	.endm

.macro	INVLPG_ALLNG
	int	$INT_INVLPG_ALLNG
	.endm

.endif
