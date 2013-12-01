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

# oz_params_axp.s

# Constants - must match OZ_HW_AXP.H, OZ_KNL_STATUS.H

OZ_SMPLOCK_SOFTINT = 1

OZ_SUCCESS = 1
OZ_ACCVIO = 6

# CALL_PAL codes

pal_BPT			= 0x80	# Breakpoint
pal_BUGCHK		= 0x81	# Bugcheck
pal_CFLUSH		= 0x01 	# Cache flush (R16=phypage)
pal_CHME		= 0x82	# Change mode to EXEC
pal_CHMK		= 0x83	# Change mode to KERNEL
pal_CHMS		= 0x84	# Change mode to SUPER
pal_CHMU		= 0x85	# Change mode to USER
pal_DRAINA		= 0x02 	# Drain aborts 
pal_GENTRAP		= 0xAA	# Generate Software Trap (R16=code)
pal_HALT		= 0x00 	# Halt processor
pal_IMB			= 0x86	# Instruction Memory Barrier
pal_LDQP		= 0x03 	# Load quadword physical (R16=phyaddr, R0=contents)
pal_MFPR_ASN		= 0x06	# Move from processor register ASN
pal_MFPR_ASTEN		= 0x26	# Move from processor register ASTEN
pal_MFPR_ASTSR		= 0x27	# Move from processor register ASTSR
pal_MFPR_ESP		= 0x1E 	# Move from processor register ESP 
pal_MFPR_FEN		= 0x1B 	# Move from processor register FEN
pal_MFPR_IPL		= 0x0E 	# Move from processor register IPL 
pal_MFPR_MCES		= 0x10 	# Move from processor register MCES
pal_MFPR_PCBB		= 0x12 	# Move from processor register PCBB 
pal_MFPR_PRBR		= 0x13 	# Move from processor register PRBR
pal_MFPR_PTBR		= 0x15 	# Move from processor register PTBR 
pal_MFPR_SCBB		= 0x16 	# Move from processor register SCBB
pal_MFPR_SISR		= 0x19 	# Move from processor register SISR 
pal_MFPR_SSP		= 0x20 	# Move from processor register SSP
pal_MFPR_TBCHK		= 0x1A 	# Move from processor register TBCHK (R16=vaddr)
pal_MFPR_USP		= 0x22 	# Move from processor register USP
pal_MFPR_VPTB		= 0x29 	# Move from processor register VPTB 
pal_MFPR_WHAMI		= 0x3F 	# Move from processor register WHAMI
pal_MTPR_ASTEN		= 0x07 	# Move to processor register ASTEN 
pal_MTPR_ASTSR		= 0x08 	# Move to processor register ASTSR
pal_MTPR_DATFX		= 0x2E 	# Move to processor register DATFX 
pal_MTPR_ESP		= 0x1F 	# Move to processor register ESP
pal_MTPR_FEN		= 0x0C 	# Move to processor register FEN 
pal_MTPR_IPIR		= 0x0D 	# Move to processor register IPIR
pal_MTPR_IPL		= 0x0F 	# Move to processor register IPL 
pal_MTPR_MCES		= 0x11 	# Move to processor register MCES
pal_MTPR_SCBB		= 0x17 	# Move to processor register SCBB 
pal_MTPR_SIRR		= 0x18 	# Move to processor register SIRR
pal_MTPR_SSP		= 0x21 	# Move to processor register SSP 
pal_MTPR_TBIA		= 0x1B 	# Move to processor register TBIA
pal_MTPR_TBIAP		= 0x1C 	# Move to processor register TBIAP 
pal_MTPR_TBIS		= 0x1D 	# Move to processor register TBIS
pal_MTPR_TBISD		= 0x24 	# Move to processor register TBISD 
pal_MTPR_TBISI		= 0x25 	# Move to processor register TBISI
pal_MTPR_USP		= 0x23 	# Move to processor register USP 
pal_MTPR_VPTB		= 0x2A 	# Move to processor register VPTB
pal_RD_PS		= 0x91	# Read Processor Status
pal_READ_UNQ		= 0x9E	# Read Unique Value
pal_REI			= 0x92	# Return from exception or interrupt
pal_RSCC		= 0x9D	# Read system cycle counter
pal_STQP		= 0x04 	# Store quadword physical (R16=addr, R17=data)
pal_SWPCTX		= 0x05 	# Swap privileged context
pal_WRITE_UNQ		= 0x9F	# Write Unique Value
pal_WTINT		= 0x3E	# Wait for Interrupt

# Assumed page size - must match values in oz_hw_axp.h

L2PAGESIZE_MIN = 13		# minimum permissible pagesize
L2PAGESIZE_MAX = 16		# maximum permissible pagesize

L2PAGESIZE = 13			# log2 (bytes in a page)
PAGESIZE = 1 << L2PAGESIZE	# bytes in a page

L2VASIZE = L2PAGESIZE - 3 + L2PAGESIZE - 3 + L2PAGESIZE - 3 + L2PAGESIZE	# va's have 43 bits
										# = 3 levels of tables + offset in page

L1PTSIZE = 1 << L2PAGESIZE					# number of bytes in the whole L1 pagetable (8KB)
L2PTSIZE = 1 << (L2PAGESIZE - 3 + L2PAGESIZE)			# number of bytes in the whole L2 pagetable (8MB)
L3PTSIZE = 1 << (L2PAGESIZE - 3 + L2PAGESIZE - 3 + L2PAGESIZE)	# number of bytes in the whole L3 pagetable (8GB)

# DISPATCH codes

dispatch_getc = 1
dispatch_puts = 2

# HWRPB offsets

hwrpb_q_hwrpbpa   =   0	# hwrbp's physical address
hwrpb_q_magic     =   8	# "HWRPB"
hwrpb_q_revision  =  16	# revision
hwrpb_q_hwrpbsiz  =  24	# hwrpb size (includes all variable fields)
hwrpb_q_pricpuid  =  32	# primary cpu id (its WHAMI)
hwrpb_q_pagesize  =  40	# page size
hwrpb_l_pabits    =  48	# number of physical address bits
hwrpb_l_vaexbits  =  52	# number of virt address extension bits
hwrpb_q_maxvalasn =  56	# maximum valid asn
hwrpb_q_syssernum =  64	# system serial number
hwrpb_q_systype   =  80	# system type
hwrpb_q_sysvar    =  88	# system variation
hwrpb_q_sysrevis  =  96	# system revision
hwrpb_q_intclkfrq = 104	# interval clock interrupt frequency
hwrpb_q_cycounfrq = 112	# cycle counter frequency
hwrpb_q_vptbase   = 120	# virtual page table base
hwrpb_q_tbhintofs = 136	# offset to translation buffer hint block
hwrpb_q_numprcslt = 144	# number of processor slots
hwrpb_q_cpusltsiz = 152	# per-cpu slot size
hwrpb_q_cpusltofs = 160	# offset to per-cpu slots
hwrpb_q_numofctbs = 168	# number of ctbs
hwrpb_q_sizeofctb = 176	# ctb size
hwrpb_q_ctbtoffs  = 184	# offset to console terminal block table
hwrpb_q_ccrboffs  = 192	# offset to console callback routine block
hwrpb_q_mddtoffs  = 200	# offset to memory data descriptor table
hwrpb_q_cdboffs   = 208	# offset ot configuration data block (if present)
hwrpb_q_frutblofs = 216	# offset to fru table (if present)
hwrpb_q_tssrva    = 224	# virtual address of terminal save state routine
hwrpb_q_tssrpv    = 232	# procedure value of terminal save state routine
hwrpb_q_trsrva    = 240	# virtual address of terminal restore state routine
hwrpb_q_trsrpv    = 248	# procedure value of terminal restore state routine
hwrpb_q_cpurrva   = 256	# virtual address of cpu restart routine
hwrpb_q_cpurrpv   = 264	# procedure value of cpu restart routine
hwrpb_q_checksum  = 288	# checksum of the above
hwrpb_q_rxrdybm   = 296	# rxrdy bitmask
hwrpb_q_txrdybm   = 304	# txrdy bitmask
hwrpb_q_dsrdbtofs = 312	# offset to dynamic system recognition data block table

# Hardware PCB - loaded/saved by the SWPCTX pal call
# These must be 128-byte aligned

HWPCB_Q_KSP  =   0	# kernel stack pointer
HWPCB_Q_ESP  =   8	# exec stack pointer
HWPCB_Q_SSP  =  16	# super stack pointer
HWPCB_Q_USP  =  24	# user stack pointer
HWPCB_Q_PTBR =  32	# pagetable base register (pfn of L1 page)
HWPCB_Q_ASN  =  40	# address space number (16 bits)
HWPCB_Q_AST  =  48	# ast enable and status bits
HWPCB_Q_FEN  =  56	# floating point enable, perf mon enab, data align trap
HWPCB_Q_CPC  =  64	# charged process cycles (32 bits)
HWPCB_Q_UNIQ =  72	# process unique value
HWPCB_Q8_PAL =  80	# pal scratch area
HWPCB__SIZE  = 128	# size of the HWPCB

# Per-CPU data (in oz_hwaxp_cpudb)
# Must match struct OZ_Hwaxp_Cpudb in oz_hw_axp.h

CPU_L_PRIORITY     =   0		# current smp lock level and thread priority
  CPU_B3_THREADPRI = CPU_L_PRIORITY+0	# - thread priority is low 24 bits
  CPU_B_SMPLEVEL   = CPU_L_PRIORITY+3	# - smp level is high 8 bits
CPU_L_INVLPG_CPUS  =   4		# bitmask of cpu's that haven't invalidated yet
CPU_Q_TEMPSPTE     =   8		# cpu's own spt entry for accessing physical pages
					# - there are two pages here
CPU_Q_TEMPSPVA     =  16		# virtual address mapped by CPU_L_TEMPSPTE spt's
					# - covers two pages
CPU_Q_THCTX        =  24		# current thread hardware context pointer
CPU_Q_PRCTX        =  32		# current process hardware context pointer
CPU_Q_INVLPG_VADR  =  40		# virtual address to be invalidated
CPU_Q_QUANTIM      =  48		# quantum timer expiration (absolute biased RDTSC value)
CPU_Q_RDTSCBIAS    =  56		# bias to keep RDTSC the same on all CPU's
CPU_Q_HWPCB0_VA    =  64		# virt addr of 128-byte aligned area within CPU_X_HWPCB0
CPU_Q_HWPCB1_VA    =  72		# virt addr of 128-byte aligned area within CPU_X_HWPCB1
CPU_Q_HWPCB0_PA    =  80		# phys addr of 128-byte aligned area within CPU_X_HWPCB0
CPU_Q_HWPCB1_PA    =  88		# phys addr of 128-byte aligned area within CPU_X_HWPCB1
CPU_L_ACTHWPCB     =  96		# which hwpcb is active (0 or 1)
CPU_X_HWPCB0       = 100					# first HWPCB
CPU_X_HWPCB1       = 100+HWPCB__SIZE+128			# second HWPCB
CPU__SIZE_RAW      = 100+HWPCB__SIZE+128+HWPCB__SIZE+128	# size of struct

CPU__L2SIZE = 10			# power of 2 that is >= CPU__SIZE_RAW
.if ((1<<CPU__L2SIZE)<CPU__SIZE_RAW)
  error : (1<<CPU__L2SIZE) < CPU__SIZE_RAW
.endif
CPU__SIZE = 1<<CPU__L2SIZE		# now make table entries exactly that size

# Hardware context saved in OZ_Thread block

THCTX_L_USTKSIZ  =  0	# number of pages in user stack
THCTX_L_USTKVPG  =  4	# virtual page of base of user stack
THCTX_Q_KSTACKVA =  8	# kernel stack top virtual address (initial value of ksp)
THCTX_Q_KSP      = 16	# current kernel stack pointer
THCTX_L_ASTSTATE = 24	# ast deliverable state; <0>=knl; <3>=usr

THCTX_Q_FCR =  32
THCTX_Q_F0  =  40
THCTX_Q_F1  =  48
THCTX_Q_F2  =  56
THCTX_Q_F3  =  64
THCTX_Q_F4  =  72
THCTX_Q_F5  =  80
THCTX_Q_F6  =  88
THCTX_Q_F7  =  96
THCTX_Q_F8  = 104
THCTX_Q_F9  = 112
THCTX_Q_F10 = 120
THCTX_Q_F11 = 128
THCTX_Q_F12 = 136
THCTX_Q_F13 = 144
THCTX_Q_F14 = 152
THCTX_Q_F15 = 160
THCTX_Q_F16 = 168
THCTX_Q_F17 = 176
THCTX_Q_F18 = 184
THCTX_Q_F19 = 192
THCTX_Q_F20 = 200
THCTX_Q_F21 = 208
THCTX_Q_F22 = 216
THCTX_Q_F23 = 224
THCTX_Q_F24 = 232
THCTX_Q_F25 = 240
THCTX_Q_F26 = 248
THCTX_Q_F27 = 256
THCTX_Q_F28 = 264
THCTX_Q_F29 = 272
THCTX_Q_F30 = 280

THCTX__SIZE = 288	# size of hardware thread context block
			# - must be no larger than OZ_THREAD_HW_CTX_SIZE in oz_hw_axp.h

# What oz_hw_thread_switchctx expects on the stack when it switches
# (this is pointed to by THCTX_Q_KSP when the thread is not active in a cpu)

THSAV_Q_R9  =   0
THSAV_Q_R10 =   8
THSAV_Q_R11 =  16
THSAV_Q_R12 =  24
THSAV_Q_R13 =  32
THSAV_Q_R14 =  40
THSAV_Q_R15 =  48
THSAV_Q_R26 =  56
THSAV_Q_USP =  64

THSAV_Q_AST =  72	# not used
THSAV_Q_FEN =  80
THSAV_Q_UNQ =  88

THSAV__SIZE =  96

# Machine argument list
# Must match OZ_Mchargs in oz_hw_axp.h

MCH_Q_R0  =   0
MCH_Q_R1  =   8
MCH_Q_P2  =  16
MCH_Q_P3  =  24
MCH_Q_P4  =  32
MCH_Q_P5  =  40
MCH_Q_R8  =  48
MCH_Q_R9  =  56
MCH_Q_R10 =  64
MCH_Q_R11 =  72
MCH_Q_R12 =  80
MCH_Q_R13 =  88
MCH_Q_R14 =  96
MCH_Q_R15 = 104
MCH_Q_R16 = 112
MCH_Q_R17 = 120
MCH_Q_R18 = 128
MCH_Q_R19 = 136
MCH_Q_R20 = 144
MCH_Q_R21 = 152
MCH_Q_R22 = 160
MCH_Q_R23 = 168
MCH_Q_R24 = 176
MCH_Q_R25 = 184
MCH_Q_R26 = 192
MCH_Q_R27 = 200
MCH_Q_R28 = 208
MCH_Q_R29 = 216
MCH_Q_R30 = 224
MCH_Q_UNQ = 232
MCH__REI  = 240
MCH_Q_R2  = 240
MCH_Q_R3  = 248
MCH_Q_R4  = 256
MCH_Q_R5  = 264
MCH_Q_R6  = 272
MCH_Q_R7  = 280
MCH_Q_PC  = 288
MCH_Q_PS  = 296
MCH__SIZE = 304

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
