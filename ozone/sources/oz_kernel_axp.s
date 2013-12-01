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
##  Main kernel module for the OS.  It is jumped to by the loader.  It 	##
##  also contains some utility routines.				##
##									##
##  It gets loaded at the bottom of the L1 block that the loader is in.	##
##									##
##########################################################################

	.title	oz_kernel_axp

	.include "oz_params_axp.s"

	.set	noat
	.set	nomacro

	.data
	.p2align 4
db:

phymem_count:		.quad	0		# number of physical pages available
phymem_start:		.quad	0		# starting page number
maxcpus:		.quad	0		# number of elements in oz_hwaxp_cpudb array
firstfreeva:		.quad	0		# filled in with VA following temp sptes
knlbasevpage:		.quad	0		# vpage at beginning of kernel superpage(s)
iobasevpage:		.quad	0		# vpage at beginning of I/O area superpage(s)

	.globl kmarker
kmarker:		.quad	0

	.p2align 4
			kstacksize = 16384
kernelstack:		.space	kstacksize

	.text
	.p2align 4
tb:

aiota2sys_p:		.quad	oz_hw_tod_aiota2sys
alignment_p:		.quad	oz_ALIGNMENT
arithover_p:		.quad	oz_ARITHOVER
badparam_p:		.quad	oz_BADPARAM
badsyscall_p:		.quad	oz_BADSYSCALL
boot_firstcpu_p:	.quad	oz_knl_boot_firstcpu
breakpoint_p:		.quad	oz_BREAKPOINT
bughceck_p:		.quad	oz_BUGCHECK
checkknlastq_p:		.quad	oz_knl_thread_checkknlastq
common_init_p:		.quad	oz_hwaxp_common_init
condhand_call_p:	.quad	oz_sys_condhand_call
condhand_default_p:	.quad	oz_sys_condhand_default
cpu_sethwints_p:	.quad	oz_hw_cpu_sethwints
cpu_smplevel_p:		.quad	oz_hw_cpu_smplevel
cpudb_p:		.quad	oz_hwaxp_cpudb
crash_p:		.quad	oz_crash
db_p:			.quad	db
debug_exception_p:	.quad	oz_knl_debug_exception
dumpmem_p:		.quad	oz_knl_dumpmem
dumppagetables_p:	.quad	oz_hwaxp_dumppagetables
floatpoint_p:		.quad	oz_FLOATPOINT
fpdis_p:		.quad	oz_hwaxp_fpdis
l1ptbase_p:		.quad	oz_hwaxp_l1ptbase
l2ptbase_p:		.quad	oz_hwaxp_l2ptbase
l3ptbase_p:		.quad	oz_hwaxp_l3ptbase
loadparams_p:		.quad	oz_s_loadparams
memcpy_p:		.quad	memcpy
phymem_l1pages_p:	.quad	oz_s_phymem_l1pages
phymem_l2pages_p:	.quad	oz_s_phymem_l2pages
printk_p:		.quad	oz_knl_printk
printkv_p:		.quad	oz_knl_printkv
process_init_p:		.quad	oz_hwaxp_process_init
pte_print_p:		.quad	oz_hw_pte_print
pyxis_early_p:		.quad	oz_dev_pyxis_early
reidump_p:		.quad	oz_hwaxp_reidump
scb_p:			.quad	oz_hwaxp_scb
section_faultpage_p:	.quad	oz_knl_section_faultpage
section_uput_p:		.quad	oz_knl_section_uput
success_p:		.quad	oz_SUCCESS
sys_checkast_p:		.quad	oz_sys_thread_checkast
syscalltbl_p:		.quad	oz_s_syscalltbl
syscallmax_p:		.quad	oz_s_syscallmax
sysmem_baseva_p:	.quad	oz_s_sysmem_baseva
sysmem_pagtblsz_p:	.quad	oz_s_sysmem_pagtblsz
thread_exit_p:		.quad	oz_knl_thread_exit
thread_getcur_p:	.quad	oz_knl_thread_getcur
thread_gethwctx_p:	.quad	oz_knl_thread_gethwctx
thread_getname_p:	.quad	oz_knl_thread_getname
thread_ustack_delete_p:	.quad	oz_hw486_ustack_delete
tod_init_p:		.quad	oz_dev_tod_init
totalpages_p:		.quad	oz_s_phymem_totalpages
undefopcode_p:		.quad	oz_UNDEFOPCODE
xxxproc_init_p:		.quad	oz_hw_xxxproc_init

chmk_neg_table:
	.quad	procgetnow	# -3
	.quad	procprintkv	# -2
	.quad	procnewusrstk	# -1
	chmk_neg_offset = (. - chmk_neg_table) / 8

l1zeroscan_bad_msg:		.string	"oz_kernel_axp: L1 pte %QX/%Q16.16X non-zero\r\n"
l1freescan_ourl1pte_bad_msg:	.string	"oz_kernel_axp: our L1 pte %QX/%Q16.16X invalid\r\n"
l1selfref_bad_msg:		.string	"oz_kernel_axp: L1 self reference %QX/%Q16.16X bad\r\n"
movemchargs_badstack_msg:	.string	"oz_kernel_axp: error %u pushing mchargs to outer mode stack\r\n"
pagesizebad_msg:		.string	"oz_kernel_axp: oz_params_axp.s pagesize %X .ne. hwrpb pagesize %X"

accvio_deb1:			.string	"oz_kernel_axp accvio*: usp %QX %4.4X (%QX %s)\n"
procchmk_deb1:			.string "oz_kernel_axp chmk*: sts %u\n"
procast_deb1:			.string	"oz_kernel_axp procast*: p3 %X\n"

##########################################################################
##									##
##  This is jumped to by the loader					##
##									##
##    Input:								##
##									##
##	R16 = points to loader's param block				##
##	R17 = points to hwrpb						##
##	R18 = kernel base virtual page (at beg of superpage)		##
##	R19 = I/O base virtual page (at beg of superpage)		##
##	R27 = entrypoint address (_start)				##
##	IPL = 31							##
##									##
##########################################################################

	.p2align 4
	.globl	_start
	.type	_start,@function
_start:
	lda	$13,tb-_start($27)
	ldq	$14,db_p-tb($13)

	stq	$18,knlbasevpage-db($14)
	stq	$19,iobasevpage-db($14)

	mov	$16,$10				# paramblock pointer
	mov	$17,$9				# hwrpb pointer

	## Copy in param block from loader

	ldq	$27,memcpy_p-tb($13)		# point to memcpy routine
	ldq	$16,loadparams_p-tb($13)	# point to where we put it
	mov	$10,$17				# point to loader's copy
	lda	$18,4096			# it's always 4K
	jsr	$26,($27)			# copy

	## Switch to our own stack so when we wipe loader out we still have a stack

	lda	$sp,kernelstack+kstacksize-db($14)

	## Initialize a bunch of stuff (including pagetable base addresses)

	ldq	$27,common_init_p-tb($13)
	mov	 $9,$16				# - hwrpbva
	lda	$17,phymem_count-db($14)	# - phymem_count_r
	lda	$18,phymem_start-db($14)	# - phymem_start_r
	lda	$19,maxcpus-db($14)		# - maxcpus_r
	jsr	$26,($27)

	## Make sure pagesize is correct

	ldq	$18,hwrpb_q_pagesize($9)
	lda	$17,PAGESIZE
	subq	$18,$17,$2
	beq	 $2,pagesizeok
	ldq	$27,crash_p-tb($13)
	lda	$16,pagesizebad_msg-tb($13)
	jsr	$26,($27)
pagesizeok:

	## phymem_count/_start include pages for the loader and kernel
	## When oz_knl_boot_firstcpu scans the pagetables, it will mark them in use

	## Unmap all loader and pyxis crap
	## The only L2 slots left should be for kernel and palcode
	##    Kernel is loaded at XXXX.XXXX.0000.0000
	##   Palcode is loaded at XXXX.XXXX.1000.0000
	##    Loader is loaded at XXXX.XXXX.2000.0000
	## No matter what the PAGESIZE is, these are all in same L1 pte
	## So the only thing left in L1 page is the self-reference and the L2 page pointer

	call_pal pal_MFPR_PTBR			# get L1 page physical page number
	mov	$0,$4				# save physical page number

	ldq	$2,l3ptbase_p-tb($13)		# see how many L1 pte's until the self reference
	ldq	$2,0($2)
	sll	$2,64-L2VASIZE,$2
	srl	$2,64-L2VASIZE+(3*L2PAGESIZE-6),$2

	sll	$0,L2PAGESIZE,$3		# get L1 page physical address
l1zeroscan_loop:
	mov	$3,$16				# read an L1 pte
	call_pal pal_LDQP
	bne	$0,l1zeroscan_bad		# should be zero
	subq	$2,1,$2
	addq	$3,8,$3
	bne	$2,l1zeroscan_loop

	mov	$3,$16				# next entry should be self-reference
	call_pal pal_LDQP
	blbc	$0,l1selfref_bad
	srl	$0,32,$1
	subl	$1,$4,$1
	bne	$1,l1selfref_bad
	mov	$3,$16				# make sure it doesn't have global or granularity bits set
	bic	$0,0x70,$17
	call_pal pal_STQP
	addq	$3,8,$3				# other than that, leave it alone

	sll	$13,64-L2VASIZE,$2		# get our L1 pte index
	srl	$2,64-L2VASIZE+(3*L2PAGESIZE-6),$2
	sll	$4,L2PAGESIZE,$0
	s8addq	$2,$0,$2			# this is phyaddr of our L1 pte
l1freescan_loop:
	mov	$3,$16				# read L1 pte
	call_pal pal_LDQP
	subq	$3,$2,$1			# branch out if it's our L1 pte
	beq	$1,l1freescan_ourl1pte
	mov	1,$4				# not ours, it came from an L1 page
	bsr	$26,freeptpage
	mov	$3,$16				# clear out L1 pte
	mov	0,$17
	call_pal pal_STQP
l1freescan_next:
	addq	$3,8,$3				# on to next L1 pte
	sll	$3,64-L2PAGESIZE,$0
	bne	$0,l1freescan_loop		# ... if any

	call_pal pal_MTPR_TBIA			# I mean it!!
						# invalidates global pages, too

	## Initialize global variables

	ldq	$0,sysmem_baseva_p-tb($13)	# base of system global memory = just after the L1 page
	ldq	$1,l1ptbase_p-tb($13)
	ldq	$2,0($1)
	lda	$2,PAGESIZE($2)
	stq	$2,0($0)

	ldq	$0,sysmem_pagtblsz_p-tb($13)	# number of system pagetable entries = number of pages from there 
	ldq	$1,iobasevpage-db($14)		# ... to, but not including, the I/O pages
	sll	$2,64-L2VASIZE,$3		# (kernel doesn't need to know about I/O pages, and don't map anything over them)
	srl	$3,64-L2VASIZE+L2PAGESIZE,$3
	subq	$1,$3,$1
	stl	$1,0($0)

	ldq	$0,phymem_l1pages_p-tb($13)	# size of l1,l2 cache = 1 for now
	mov	1,$1
	stl	$1,0($0)
	ldq	$0,phymem_l2pages_p-tb($13)
	stl	$1,0($0)

	## Make up temp sptes for cpudb array

	ldq	 $2,maxcpus-db($14)		# max cpus we handle
	ldq	 $3,cpudb_p-tb($13)		# point to cpudb array
	mov	$13,$4				# get kernel's virt address
	sll	 $4,64-L2VASIZE,$5		# calc corresponding virt page number
	srl	 $5,64-L2VASIZE+L2PAGESIZE,$5
	sll	 $5,3,$5			# calc index in L3 pagetable for its PTE
	ldq	 $0,l3ptbase_p-tb($13)		# get virt address of L3 pt base
	ldq	 $0,0($0)
	addq	 $5,$0,$5			# get virt address of temp SPTE
tempspteinit_find:
	ldq	 $0,0($5)			# see if the SPTE is in use
	blbc	 $0,tempspteinit_loop		# if not, start with it
	lda	 $4,PAGESIZE($4)		# if so, increment to next
	lda	 $5,8($5)
	br	$31,tempspteinit_find
tempspteinit_loop:
	stq	 $4,CPU_Q_TEMPSPVA($3)		# this is virt address mapped by temp SPTE
	stq	 $5,CPU_Q_TEMPSPTE($3)		# this is virt address of the temp SPTE
	subq	 $2,1,$2
	lda	 $4,PAGESIZE*2($4)		# on to next pair of temp SPTE's
	lda	 $5,16($5)
	bne	 $2,tempspteinit_loop		# repeat if more to fill in
	stq	 $4,firstfreeva-db($14)		# save end of used virt address space

	## Finish initialization

	ldq	$27,pyxis_early_p-tb($13)	# oz_dev_pyxis_early to set up I/O environment
	ldl	$16,phymem_count-db($14)	# - number of free physical pages
	ldl	$17,phymem_start-db($14)	# - start of free physical pages
	jsr	$26,($27)
	stl	 $0,phymem_count-db($14)	# - it sucks up some pages off the end
	ldl	 $1,phymem_start-db($14)
	ldq	 $2,totalpages_p-tb($13)
	addl	 $0,$1,$0			# this is total number of pages 
	stl	 $0,0($2)			# ... we let the kernel know about

	ldq	$11,scb_p-tb($13)		# point to the SCB

	ldq	$0,fpdis_p-tb($13)		# set up FPU disabled handler
	stq	$0,0x010($11)

	lda	$0,except_accvio-tb($13)	# set up ACCVIO handler
	stq	$0,0x080($11)

	lda	$0,except_arith-tb($13)		# set up arithmetic trap handler
	stq	$0,0x200($11)

	lda	 $0,procast-tb($13)		# set up ast handler
	mov	 14,$1
	stq	 $0,0x240($11)			# - for kernel mode
	stq	 $1,0x248($11)			#   param is 'clear bit 0 of ASTSR'
	mov	  7,$1
	stq	 $0,0x270($11)			# - for user mode
	stq	 $1,0x278($11)			#   param is 'clear bit 3 of ASTSR'
	mov	0x99,$16			# enable kernel and user ast interrupts
	call_pal pal_MTPR_ASTEN

	lda	 $0,except_alignment-tb($13)	# set up data alignment trap handler
	stq	 $0,0x280($11)

	lda	 $0,except_generic-tb($13)	# set up generic trap handler
	ldq	 $1,breakpoint_p-tb($13)	# - breakpoint
	ldl	 $1,0($1)
	stq	 $0,0x400($11)
	stq	 $1,0x408($11)
	ldq	 $1,bughceck_p-tb($13)		# - bugcheck
	ldl	 $1,0($1)
	stq	 $0,0x410($11)
	stq	 $1,0x418($11)
	ldq	 $1,undefopcode_p-tb($13)	# - illegal instruction
	ldl	 $1,0($1)
	stq	 $0,0x420($11)
	stq	 $1,0x428($11)
	ldq	 $1,badparam_p-tb($13)		# - illegal operand
	ldl	 $1,0($1)
	stq	 $0,0x430($11)
	stq	 $1,0x438($11)

	lda	 $0,except_gentrap-tb($13)	# - generate trap
	stq	 $0,0x440($11)

	lda	$0,procchmk-tb($13)		# set up CHMK handler
	stq	$0,0x480($11)
	lda	$0,procchmx-tb($13)
	stq	$0,0x490($11)			# dummy out CHME
	stq	$0,0x4A0($11)			# dummy out CHMS
	stq	$0,0x4B0($11)			# dummy out CHMU

	ldq	$27,tod_init_p-tb($13)		# initialize time-of-day now as
	jsr	$26,($27)			# ... many startup routines need it

	ldq	$27,xxxproc_init_p-tb($13)	# oz_hw_(uni/sm)proc.c
	jsr	$26,($27)			# this is what enables interrupts
						# we are now at softint level

	ldq	$27,process_init_p-tb($13)	# initialize process module
	jsr	$26,($27)

	ldq	$27,boot_firstcpu_p-tb($13)	# call oz_knl_boot_firstcpu
	ldq	$16,firstfreeva-db($14)		# - first free virtual page
	sll	$16,64-L2VASIZE,$16
	srl	$16,64-L2VASIZE+L2PAGESIZE,$16
	ldq	$17,phymem_start-db($14)	# - first free physical page
	jsr	$26,($27)

.if 000 # doesn't work on Miata, ill instr trap on WTINT
	.p2align 4
waitloop:
	mov	0,$16				# wait here forever
	call_pal pal_WTINT
	br	$31,waitloop
.else
	ldq	$27,thread_exit_p-tb($13)
	mov	OZ_SUCCESS,$16
	jsr	$26,($27)
	call_pal pal_HALT
.endif

	## We found our L1 pte
	## Free off any L2 crap except what maps the kernel and palcode
	##    Kernel is loaded at XXXX.XXXX.0000.0000
	##   Palcode is loaded at XXXX.XXXX.1000.0000
	## Number of entries depends on the PAGESIZE
	## The only registers we need to preserve are R2,R3,R13,R14,R15

l1freescan_ourl1pte:
	blbc	$0,l1freescan_ourl1pte_bad	# we've lost our mind
	srl	$0,32,$0			# get the L2 phypage
	sll	$0,L2PAGESIZE,$0		# get the L2 phyaddr
	lda	$1,(0x20000000+(PAGESIZE<<(L2PAGESIZE-3))-1)>>(2*L2PAGESIZE-3) # get number of entries to leave alone
	s8addq	$1,$0,$5			# set R5 = phyaddr of first L2 pte to wipe
	mov	2,$4				# all these pte's come from an L2 page
l2freescan_loop:
	mov	$5,$16				# get an L2 pte
	call_pal pal_LDQP
	bsr	$26,freeptpage			# wipe out anything it points to
	mov	$5,$16				# clear the pte out
	mov	0,$17
	call_pal pal_STQP
	addq	$5,8,$5				# on to next entry in L2 page
	sll	$5,64-L2PAGESIZE,$0
	bne	$0,l2freescan_loop		# ... if any
	br	$31,l1freescan_next		# at end, resume L1 scanning

	## Crashes

l1zeroscan_bad:
	ldq	$27,printk_p-tb($13)
	mov	$0,$18				# pte value
	mov	$3,$17				# pte phyaddr
	lda	$16,l1zeroscan_bad_msg-tb($13)
	jsr	$26,($27)
	call_pal pal_HALT

l1freescan_ourl1pte_bad:
	ldq	$27,printk_p-tb($13)
	mov	$0,$18
	mov	$3,$17
	lda	$16,l1freescan_ourl1pte_bad_msg-tb($13)
	jsr	$26,($27)
	call_pal pal_HALT

l1selfref_bad:
	ldq	$27,printk_p-tb($13)
	mov	$0,$18
	mov	$3,$17
	lda	$16,l1selfref_bad_msg-tb($13)
	jsr	$26,($27)
	call_pal pal_HALT

##########################################################################
##									##
##  Free physical page pointed to by a pte and any sub-pages		##
##									##
##    Input:								##
##									##
##	R0  = pte pointing to page to free				##
##	R4  = pagetable level the R0 pte came from (1..3)		##
##	R14 = read/write base (db)					##
##	R26 = return address						##
##									##
##    Scratch:								##
##									##
##	R0,R1,R16							##
##									##
##########################################################################

freeptpage:
	blbc	$0,freeptpage_ret	# if pte not valid, ignore it
	subq	$sp,24,$sp		# save scratch regs
	stq	$10, 0($sp)
	stq	$11, 8($sp)
	stq	$26,16($sp)
	srl	$0,32,$0		# get the physical page number

	sll	$0,L2PAGESIZE,$11	# get page's physical address
	cmpeq	$4,3,$1			# see if it came from an L3 page
	bne	$1,freeptpage_done	# if so, it has no sub-pages to free
	addq	$4,1,$4			# all these pages come from next level
freeptpage_loop:
	mov	$11,$16			# read a pte
	call_pal pal_LDQP
	bsr	$26,freeptpage		# free off phypage it points to, if any
	addq	$11,8,$11		# increment on to next pte
	sll	$11,64-L2PAGESIZE,$0
	bne	$0,freeptpage_loop	# repeat if we haven't done whole page yet
	subq	$4,1,$4			# restore level counter
freeptpage_done:
	ldq	$10, 0($sp)
	ldq	$11, 8($sp)
	ldq	$26,16($sp)
	addq	$sp,24,$sp
freeptpage_ret:
	ret	$31,($26)

##########################################################################
##									##
##  Jumped to by hardware on a CHMK instruction				##
##									##
##    Standard syscall input (R27>=0):					##
##									##
##	R16..R23 = first 8 args						##
##	R0,R1 = args 9 & 10						##
##	(KSP) = args 11..16 (in saved R2..R7 by palcode)		##
##	R26 = outer mode return address					##
##	R27 = syscall index						##
##									##
##    Standard syscall routine called:					##
##									##
##	R16..R20 = args 1..5 as on input				##
##	R21 = (now arg 6) caller's procmode (0=kernel, 1=user)		##
##	(KSP) = args 6..16 (now args 7..17)				##
##	R26 = returns back here to clean up				##
##	R27 = routine's entrypoint					##
##									##
##    Internal input (R27<0):						##
##									##
##	R27 = chmk_neg_table index					##
##									##
##    Neg index routine is called:					##
##									##
##	R2..R5 = messed up						##
##	R26 = returns to a pal_REI instruction				##
##	R27 = its entrypoint						##
##	all others unchanged						##
##									##
##########################################################################

	.p2align 4
procchmk:
	blt	$27,chmk_negative
	ldq	 $3,syscallmax_p-procchmk($2)
	stq	$26,48($sp)		# save outer mode return address over saved PC
	ldl	 $3,0($3)
	subq	$sp,40,$sp		# make room for args 6..10
	cmpult	$27,$3,$3		# make sure index is ok
	stq	$21, 0($sp)		# save possible arg 6 on stack
	beq	 $3,bad_index
	stq	$22, 8($sp)		# save possible arg 7 on stack
	ldq	 $3,syscalltbl_p-procchmk($2)
	stq	$23,16($sp)		# save possible arg 8 on stack
	stq	 $0,24($sp)		# save possible arg 9 on stack
	s8addq	$27,$3,$27
	ldq	$21,40+56($sp)		# get caller's PS
	stq	 $1,32($sp)		# save possible arg 10 on stack
	and	$21,0x18,$21		# get current mode field from caller's PS
	ldq	$27,0($27)		# get oz_syscall_... entrypoint from oz_s_syscalltbl
	cmovne	$21,1,$21		# we want 0=kernel, 1=not kernel (ie, user)
					# - this is 'cprocmode' arg (#6) to oz_syscall_... routines
	stq	$27,40+40($sp)		#### save entrypoint over arg 16 which no one uses anyway
	jsr	$26,($27)		# call the oz_syscall_... routine

	ldq	$26,40+48($sp)		# call standard requires return via R26
	addq	$sp,40,$sp		# wipe extra args from stack
	call_pal pal_REI		# return to outermode caller

bad_index:
	ldq	 $0,badsyscall_p-procchmk($2)
	ldq	 $0,0($0)
	ldq	$26,40+48($sp)		# call standard requires return via R26
	addq	$sp,40,$sp		# wipe extra args from stack
	call_pal pal_REI		# return to outermode caller

	.p2align 4
chmk_negative:
	addq	$27,chmk_neg_offset,$27
	lda	 $3,chmk_neg_table-procchmk($2)
	blt	$27,bad_neg_index
	s8addq	$27,$3,$27
	ldq	$27,0($27)
	jsr	$26,($27)
	call_pal pal_REI		# return just past the CHMK instruction

bad_neg_index:
	ldq	 $0,badsyscall_p-procchmk($2)
	ldq	 $0,0($0)
	call_pal pal_REI		# return just past the CHMK instruction

# If anyone tries a CHME, CHMS, CHMU, just return as is

	.p2align 4
procchmx:
	call_pal pal_REI

##########################################################################
##									##
##  The thread has changed user stacks					##
##									##
##  This unmaps the old user stack section and sets up the new one to 	##
##  be unmapped when the thread exits.					##
##									##
##    Input:								##
##									##
##	usp = new user stack section					##
##	PS<CM> = user							##
##									##
##    Output:								##
##									##
##	thread's old user stack unmapped				##
##	section that usp is in is marked for delete on thread exit	##
##									##
##########################################################################

	.align	4
	.globl	oz_sys_thread_newusrstk
	.type	oz_sys_thread_newusrstk,@function
oz_sys_thread_newusrstk:
	mov	$26,$8			# save return address where CHMK won't mess wit it
	lda	$27,-1			# set up index to procnewusrstk routine
	call_pal pal_CHMK		# call it

	.p2align 4
procnewusrstk:
	stq	 $8,48($sp)		# make pal_REI return to oz_sys_thread_newusrstk's caller
	subq	$sp,16,$sp		# save scratch registers
	stq	$12, 0($sp)
	stq	$13, 8($sp)
	mov	$27,$13			# set up base register R13 = procnewusrstk

	ldq	$27,thread_getcur_p-procnewusrstk($13)
	jsr	$26,($27)		# get pointer to current thread struct
	mov	 $0,$16			# get corresponding hardware context
	ldq	$27,thread_gethwctx_p-procnewusrstk($13)
	jsr	$26,($27)
	mov	 $0,$12
	ldl	$16,THCTX_L_USTKVPG($12) # unmap the old section
	ldq	$27,thread_ustack_delete_p-procnewusrstk($13)
	jsr	$26,($27)

	call_pal pal_MFPR_USP		# get user mode stack pointer
	sll	 $0,64-L2VASIZE,$0	# convert to virtual page number
	srl	 $0,64-L2VASIZE+L2PAGESIZE,$0
	stl	 $0,THCTX_L_USTKVPG($12) # save it away to be unmapped on thread exit

	ldq	 $0,success_p-procnewusrstk($13)
	ldq	 $0,0($0)		# always successful
	ldq	$12, 0($sp)
	ldq	$13, 8($sp)
	addq	$sp,16,$sp
	ldq	$26,48($sp)		# calling standard requires
	call_pal pal_REI

##########################################################################
##									##
##  Usermode callable debug print routine				##
##									##
##  *NOT* for production system						##
##									##
##    Input:								##
##									##
##	R16 = 0 : just continue						##
##	      1 : pause after printing					##
##	R17 = points to format string					##
##	R18..21,0..32(SP) = print args					##
##									##
##########################################################################

	.p2align 4
	.globl	oz_sys_printkp
	.type	oz_sys_printkp,@function
oz_sys_printkp:
	subq	$sp,8,$sp	# make room on stack for return address
	stq	$26,0($sp)	# save return address
	mov	$17,$16		# shuffle args down one
	mov	$18,$17
	mov	$19,$18
	mov	$20,$19
	mov	$21,$20
	ldq	$21, 8($sp)	# - this better be all they're giving
	ldq	$22,16($sp)
	ldq	$23,24($sp)
	ldq	$24,32($sp)
	ldq	$25,40($sp)
	lda	$27,-2		# get printkv code
	call_pal pal_CHMK	# do it in kernel mode
	ldq	$26,0($sp)	# restore return address
	addq	$sp,8,$sp	# wipe temp stack space
	ret	$31,($26)	# return to caller

	.p2align 4
procprintkv:
	subq	$sp,32,$sp
	stq	$22, 0($sp)
	stq	$23, 8($sp)
	stq	$24,16($sp)
	stq	$25,24($sp)
	ldq	$27,printk_p-procprintkv($27)
	jsr	$26,($27)	# call oz_knl_printkv
	addq	$sp,32,$sp
	call_pal pal_REI	# return just past CHMK call

##########################################################################
##									##
##  Get current date/time						##
##  Callable from either kernel or user mode				##
##									##
##    Output:								##
##									##
##	R0 = date/time							##
##									##
##########################################################################

	.p2align 4
	.globl	oz_hw_tod_getnow
	.type	oz_hw_tod_getnow,@function
oz_hw_tod_getnow:
	mov	$26,$8		# pass in return address this way
	lda	$27,-3		# tell it we want procgetnow
	call_pal pal_CHMK

procgetnow:
	stq	$8,48($sp)	# save final return address
	call_pal pal_RSCC	# get current iota time
	ldq	$27,aiota2sys_p-procgetnow($27)
	mov	$0,$16		# convert to system time
	jsr	$26,($27)
	ldq	$26,48($sp)
	call_pal pal_REI

##########################################################################
##									##
##  Maximize processor mode with current mode				##
##									##
##    Input:								##
##									##
##	R16 = mode to maximize						##
##									##
##    Output:								##
##									##
##	R0 = R16 maximized with current mode				##
##									##
##########################################################################

	.p2align 4
	.globl	oz_hw_procmode_max
	.type	oz_hw_procmode_max,@function
oz_hw_procmode_max:
	mov	$16,$2		# make sure it's safe from RD_PS
	call_pal pal_RD_PS	# read processor status into R0
	and	$0,0x18,$0	# mask to get current mode in <04:03>
	cmpeq	$0,0,$0		# map any non-zero value to 1
	xor	$0,1,$0
	or	$0,$2,$0	# if input arg is 1, force a 1 on output
	ret	$31,($26)	# return with maximized mode

##########################################################################
##									##
##  Process ast deliveries						##
##									##
##    Input:								##
##									##
##	R2 = address of procast						##
##	R3 = 0xE for kernel, 0x7 for user				##
##	(SP) = mchargs from MCH__REI to the end				##
##	PS<CM> = kernel							##
##	PS<IPL> = 2							##
##									##
##########################################################################

	.p2align 4
procast:
	bsr	 $6,pushmchargs			# push mchargs on kernel stack
	lda	$13,tb-.($6)			# set up R13 text base register
	mov	 $3,$16				# clear bit in ASTSR
	call_pal pal_MTPR_ASTSR
	mov	  0,$16				# now we can lower IPL back to zero
	call_pal pal_MTPR_IPL
	blbc	 $3,procast_kernel		# check for kernel ast's
	bsr	$10,movemchargs			# move to outer mode stack
	ldq	$27,sys_checkast_p-tb($13)	# call oz_sys_thread_checkast (mchargs)
	mov	$12,$16				# ... which calls oz_hw_thread_aststate to update THCTX_L_ASTSR
	lda	$26,exception_rtn-tb($13)
	jsr	$31,($27)

	.p2align 4
procast_kernel:
	call_pal pal_MFPR_WHAMI			# we can do kernel ast's a lot quicker this way
	ldq	$27,checkknlastq_p-tb($13)	# ... which also updates THCTX_L_ASTSR
	mov	 $0,$16
	mov	$12,$17
	lda	$26,exception_rtn-tb($13)
	jsr	$31,($27)

##########################################################################
##									##
##  This routine gets called when there is an access violation		##
##  Note that this is the pagefault code.  We don't use the 		##
##  translation-not-valid mechanism because it's not portable, and the 	##
##  only thing that gets slowed down is true access violation fault 	##
##  handling.  The oz_knl_section_pagefault routine is smart enough to 	##
##  not fault in the page if the faulter does not have the required 	##
##  access.								##
##									##
##  A side effect is that it leaves the translation-not-valid vector 	##
##  available for use by other routines, eg, the Pyxis driver uses it 	##
##  to dynamically map sparse I/O space.				##
##									##
##    Input:								##
##									##
##	($sp) = mchargs from MCH__REI to the end			##
##	kernel mode, same IPL as caller					##
##									##
##########################################################################

	.p2align 4
except_accvio:
	bsr	 $6,pushmchargs			# sets up R12=mchargs
except_accvio_x6:
	ldq	$27,cpu_smplevel_p-except_accvio_x6($6)
	lda	$13,tb-except_accvio_x6($6)	# set up R13 text base register
	jsr	$26,($27)			# get our smplevel
	cmpule	 $0,OZ_SMPLOCK_SOFTINT,$1	# see if SOFTINT or below
	mov	OZ_ACCVIO,$0
	beq	 $1,except_accvio_bad		# if not, can't fault anything in
	ldq	$27,section_faultpage_p-tb($13)
	ldq	$16,MCH_Q_PS($12)		# ok, get kernel/user flag
	ldq	$17,MCH_Q_P4($12)		# get virtual page
	ldq	$18,MCH_Q_P5($12)		# get write bit
	and	$16,0x18,$16			# 0=kernel, else user
	sll	$17,64-L2VASIZE,$17
	srl	$18,63,$18
	cmpeq	$16,0,$16			# 1=kernel, 0=user
	srl	$17,64-L2VASIZE+L2PAGESIZE,$17
	xor	$16,1,$16			# 0=kernel, 1=user
	jsr	$26,($27)			# call oz_knl_section_faultpage
	subl	 $0,OZ_SUCCESS,$1		# see if successful
	beq	 $1,exception_rtn		# return to retry faulting instruction

## Failed to load the required page (R0=status)

except_accvio_bad:
	mov	 $0,$9				# save error status
	bsr	$10,movemchargs			# switch to outermode stack
	ldq	 $2,MCH_Q_P4($12)		# get virtual address
	ldq	 $3,MCH_Q_P5($12)		# get access type
	subq	$sp,40,$sp			# make room for sigargs
	mov	  4,$0				# fill in sigargs
	mov	  2,$1
	stq	 $0, 0($sp)
	stq	 $9, 8($sp)
	stq	 $1,16($sp)
	stq	 $2,24($sp)
	stq	 $3,32($sp)
	br	$31,exception_signal		# go signal it

##########################################################################
##									##
##  Other miscellaneous exceptions					##
##									##
##  These are called in kernel mode with IPL unchanged			##
##  All we do is signal the error					##
##									##
##########################################################################

	## R4=register write mask; R5=exception summary

	.p2align 4
except_arith:
	bsr	 $6,pushmchargs			# sets up SP=R12=mchargs
	lda	$13,tb-.($6)			# set up R13 text base register
	mov	 $4,$14				# save register write mask
	mov	 $5,$15				# save exception summary
	bsr	$10,movemchargs			# move to outermode stack
	subq	$sp,40,$sp			# push sigargs on stack
	mov	  4,$0
	mov	  2,$1
	stq	 $0, 0($sp)
	stq	 $1,16($sp)
	stq	$14,24($sp)
	stq	$15,32($sp)
	and	$15,0x40,$1			# check for integer overflow
	ldq	 $0,arithover_p-tb($13)
	bne	 $1,except_arith_push
	ldq	 $0,floatpoint_p-tb($13)	# if not, assume floatingpoint error
except_arith_push:
	ldl	 $0,0($0)
	stq	 $0,8($sp)			# store error code in sigargs
	br	$31,exception_signal		# go signal it

	## R4=unaligned address; R5=0: read, 1: write

	.p2align 4
except_alignment:
	ldq	 $2,56($sp)			# see if kernel mode
	and	 $2,0x18,$2
	bne	 $2,except_alignment_outer
	call_pal pal_REI			# if kernel mode, just resume

	.p2align 4
except_alignment_outer:
	bsr	 $6,pushmchargs			# outer mode, sets up SP=R12=mchargs
	lda	$13,tb-.($6)			# set up R13 text base register
	mov	 $4,$14				# save va
	mov	 $5,$15				# save wf
	bsr	$10,movemchargs			# move to outermode stack
	ldq	 $1,alignment_p-tb($13)
	subq	$sp,40,$sp			# push sigargs on stack
	mov	  4,$0
	ldl	 $1,0($1)
	mov	  2,$2
	stq	 $0, 0($sp)
	stq	 $1, 8($sp)
	stq	 $2,16($sp)
	stq	$14,24($sp)
	stq	$15,32($sp)
	br	$31,exception_signal		# go signal it

	## R16 = OZ_... error status

	.p2align 4
except_gentrap:
	mov	$16,$3				# shuffle over for except_generic handler

	## R3 = OZ_... error status

	.p2align 4
except_generic:
	bsr	 $6,pushmchargs			# sets up SP=R12=mchargs
	lda	$13,tb-.($6)			# set up R13 text base register
	mov	 $3,$9				# save error status
	bsr	$10,movemchargs			# switch to outermode stack
	mov	  2,$0				# fill in sigargs
	subq	$sp,24,$sp
	stq	 $0, 0($sp)
	stq	 $9, 8($sp)
	stq	$31,16($sp)

## Signal the exception.  On outermode stack with SP=sigargs, R12=mchargs, R13=textbase

	.p2align 4
exception_signal:
	ldq	$27,condhand_call_p-tb($13)
	mov	$sp,$16				# point to sigargs
	mov	$12,$17				# point to mchargs
	jsr	$26,($27)			# call oz_sys_condhand_call
	bne	 $0,exception_rtn		# retry faulting instruction
	ldq	 $0,MCH_Q_PS($12)		# no handler found, see if exception happened in kernel mode
	and	 $0,0x18,$0
	beq	 $0,exception_crash
	mov	$sp,$16				# if not, call oz_sys_condhand_default (sigargs, mchargs)
	mov	$12,$17
	ldq	$27,condhand_default_p-tb($13)
	jsr	$26,($27)
	call_pal pal_HALT			# it should never return
exception_crash:
	ldq	$27,cpu_sethwints_p-tb($13)	# inhibit hw int delivery during debugger
	mov	  0,$16
	jsr	$26,($27)
	mov	 $0,$9
	ldq	$27,debug_exception_p-tb($13)	# call oz_knl_debug_exception (sigargs, mchargs)
	mov	$sp,$16
	mov	$12,$17
	jsr	$26,($27)
	mov	 $0,$10
	ldq	$27,cpu_sethwints_p-tb($13)	# restore hw int delivery
	mov	 $9,$16
	jsr	$26,($27)
	bne	$10,exception_crash

## Return to where we left off (R12=mchargs on stack)

	.p2align 4
exception_rtn:
	mov	$12,$sp				# wipe sigargs from stack
	ldq	$16,MCH_Q_UNQ($12)		# restore state from mchargs
	call_pal pal_WRITE_UNQ
	ldq	 $0,MCH_Q_R0($sp)
	ldq	 $1,MCH_Q_R1($sp)
	ldq	 $8,MCH_Q_R8($sp)
	ldq	 $9,MCH_Q_R9($sp)
	ldq	$10,MCH_Q_R10($sp)
	ldq	$11,MCH_Q_R11($sp)
	ldq	$12,MCH_Q_R12($sp)
	ldq	$13,MCH_Q_R13($sp)
	ldq	$14,MCH_Q_R14($sp)
	ldq	$15,MCH_Q_R15($sp)
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
	lda	$sp,MCH__REI($sp)
	call_pal pal_REI

##########################################################################
##									##
##  Push the mchargs on the stack					##
##									##
##    Input:								##
##									##
##	just as on entry to exception handler				##
##	R6 = return address (not R26)					##
##									##
##    Output:								##
##									##
##	R12 = points to mchargs on stack				##
##									##
##########################################################################

	.p2align 4
pushmchargs:
	lda	$sp,-MCH__REI($sp)		# make room on stack for all mchargs
	stq	 $0,MCH_Q_R0($sp)		# save registers
	stq	 $1,MCH_Q_R1($sp)
	stq	 $2,MCH_Q_P2($sp)
	stq	 $3,MCH_Q_P3($sp)
	stq	 $4,MCH_Q_P4($sp)
	stq	 $5,MCH_Q_P5($sp)
	stq	 $8,MCH_Q_R8($sp)
	stq	 $9,MCH_Q_R9($sp)
	stq	$10,MCH_Q_R10($sp)
	stq	$11,MCH_Q_R11($sp)
	stq	$12,MCH_Q_R12($sp)
	stq	$13,MCH_Q_R13($sp)
	stq	$14,MCH_Q_R14($sp)
	stq	$15,MCH_Q_R15($sp)
	stq	$16,MCH_Q_R16($sp)
	stq	$17,MCH_Q_R17($sp)
	stq	$18,MCH_Q_R18($sp)
	stq	$19,MCH_Q_R19($sp)
	stq	$20,MCH_Q_R20($sp)
	stq	$21,MCH_Q_R21($sp)
	stq	$22,MCH_Q_R22($sp)
	stq	$23,MCH_Q_R23($sp)
	ldq	$12,MCH_Q_PS($sp)
	stq	$24,MCH_Q_R24($sp)
	stq	$25,MCH_Q_R25($sp)
	stq	$26,MCH_Q_R26($sp)
	srl	$12,56,$0
	stq	$27,MCH_Q_R27($sp)
	lda	 $0,MCH__SIZE($0)
	stq	$28,MCH_Q_R28($sp)
	addq	 $0,$sp,$0
	stq	$29,MCH_Q_R29($sp)
	stq	 $0,MCH_Q_R30($sp)		# (inner mode) stack pointer at time of exception
						# gets changed to outermode stack pointer by movemchargs
	call_pal pal_READ_UNQ			# save UNQ (used for frame unwinding)
	and	$12,0x18,$1
	stq	 $0,MCH_Q_UNQ($sp)
	bne	 $1,pushmchargs_ret
	mov	  0,$16				# clear if caller not kernel mode so kernel 
	call_pal pal_WRITE_UNQ			# ... exceptions don't call usermode handlers
pushmchargs_ret:
	mov	$sp,$12				# point to the mchargs
	ret	$31,($6)

##########################################################################
##									##
##  Move mchargs to outermode stack					##
##									##
##    Input:								##
##									##
##	R10 = return address (not R26)					##
##	R12 = points to mchargs on kernel stack				##
##	R13 = text base register (tb)					##
##									##
##    Output:								##
##									##
##	SP = R12 = mchargs (on outer mode stack)			##
##									##
##    Scratch:								##
##									##
##	R0-R8,R11,R16-R29						##
##									##
##    Note:								##
##									##
##	Thread aborted if move fails					##
##									##
##########################################################################

	.p2align 4
movemchargs:
	ldq	 $0,MCH_Q_PS($12)	# instant success if outermode stack is kernel
	mov	$12,$sp			# wipe any crap off kernel stack
	and	 $0,0x18,$0
	beq	 $0,movemchargs_success

## It was on user stack, copy mchargs to user stack
## Use oz_knl_section_uput to copy so it can't page out or move on us

	call_pal pal_MFPR_USP		# get usermode stack pointer
	bic	 $0,63,$1		# 64-byte align it
	mov	 $0,$11			# save original for moved mchargs
	lda	$16,-MCH__SIZE($1)
	lda	$19,-MCH__SIZE($1)	# - copy to the user stack
	call_pal pal_MTPR_USP		# (update usermode stack pointer)
	ldq	$27,section_uput_p-tb($13)
	mov	  1,$16			# - destination is in user mode
	lda	$17,MCH__SIZE		# - how many bytes to copy
	mov	$12,$18			# - copy from the kernel stack
	jsr	$26,($27)		# - call oz_knl_section_uput
	cmpeq	$0,OZ_SUCCESS,$1
	lda	 $2,movemchargs_alignfixup-tb($13)
	beq	 $1,movemchargs_failed
	stq	 $2,MCH_Q_PC($sp)	# return to caller in outer mode
	lda	$sp,MCH__REI($sp)	# leave just REI stuff on stack
	call_pal pal_REI

	.p2align 4
movemchargs_alignfixup:
	and	$11,63,$1		# mask to get alignment bits
	stq	$11,MCH_Q_R30($sp)	# outer mode stack pointer at time of exception
	stb	 $1,MCH_Q_PS+7($sp)	# save user stack alignment bits
	mov	$sp,$12			# set up mchargs pointer
	ret	$31,($10)		# return just past call to movemchargs in outer mode

	.p2align 4
movemchargs_success:
	ret	$31,($10)

movemchargs_failed:
	mov	 $0,$10			# outermode stack bad, save status somewhere safe
	ldq	$27,printk_p-tb($13)	# print message
	lda	$16,movemchargs_badstack_msg-tb($13)
	mov	$10,$17
	jsr	$26,($27)
	ldq	$27,thread_exit_p-tb($13) # exit thread
	mov	$10,$16
	jsr	$26,($27)
	call_pal pal_HALT

