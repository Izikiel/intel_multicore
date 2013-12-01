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
##  This program gets loaded into memory by the console boot command	##
##									##
##  Paging is already enabled						##
##  The HWRPB is at VA 10000000						##
##  We are loaded at VA 20000000					##
##  Interrupts disabled (IPL = 31)					##
##  L1PTE[0] = the one-and-only L2 page					##
##       [1] = the L1 page itself					##
##									##
##  The I/O driver (eg, oz_dev_pyxis_axp.c) requires some unused L1 	##
##  pte's at the end of the L1 page.  So it will mark them as needed.  	##
##  Then the loader will load the kernel at the beginning of the 	##
##  superpage just below that and relocate the pagetable superpage 	##
##  just below the kernel.						##
##									##
##########################################################################

	.title	oz_loader_axp

	.set	noat	# allows use of $28
	.set	nomacro	# don't allow it to make up funny instrs for us

	.include "oz_params_axp.s"

	.text
	.globl	_start
_start:
	nop					# to put 'base13' on quad boundary
	br	$13,begin			# jump to beginning of code
baser13:					# ... also sets up $13 as base register

	.p2align 3
dotten:		.quad	0x999999999999999A
linkedaddress:	.quad	_start			# address we are linked at
hwrpbva:	.quad	0x10000000		# the console creates the hwrpb and maps it at va 0x10000000
phonyl2va:	.quad	0x40000000		# there's a stupid mapping of L2 pagetable here, we wipe it out
ldr_init_p:	.quad	oz_ldr_init_axp
fourgb:		.quad	0x100000000
ldrparamblock_p: .quad	oz_ldr_paramblock
						# linkage pair for dispatch routine:
dispatch_ep:	.quad	0			# - entrypoint
dispatch_pd:	.quad	0			# - procedure descriptor
iobasevpage:	.quad	oz_hwaxp_iobasevpage	# page at base of I/O area superpage
kernelbase:	.quad	0			# kernel base address (at beginning of its superpage)
knlbasevpage:	.quad	oz_hwaxp_knlbasevpage	# page at base of kernel area superpage
kstart:		.quad	0			# kernel's _start address
l2pagesize:	.quad	0			# LOG2 (pagesize)
l2ptesperpage:	.quad	0			# LOG2 (ptesperpage)
l2vasize:	.quad	0			# number of bits in a virtual address (including 'sign' bit)
midpagesize:	.quad	0			# number of bytes mapped by an L2 entry
newl1ptbase:	.quad	0			# new base address of L1 pagetable
newl2ptbase:	.quad	0			# new base address of L2 pagetable
newl3ptbase:	.quad	0			# new base address of L3 pagetable
oldl1ptbase:	.quad	0			# entry of old L1 pt that maps vaddr 0
pagesize:	.quad	0			# page size in bytes (from HWRPB)
ptesperpage:	.quad	0			# number of pagetable entries in a page
superpagesize:	.quad	0			# number of bytes mapped by an L1 entry
vamaskbits:	.quad	0			# mask for the virtual address bits (including 'sign' bit)

hwrpbmagic:	.ascii	"HWRPB"
		.byte	0,0,0
hextable:	.ascii	"0123456789ABCDEF"

spaces:		.ascii	"        "
crlf:		.byte	13,10
brcolsp:	.ascii	"]: "
dotdot:		.ascii	".."

bwxbad:		.ascii	"oz_loader_axp: I'm tOO stOOpid to run without BWX\r\n"
		bwxbadlen = .-bwxbad

	.p2align 3
begin:
	x13 = baser13

	ldq	$3,hwrpbva-x13($13)		# get hwrpb pointer set up
	ldq	$0,hwrpb_q_magic($3)		# make sure the magic is there
	ldq	$1,hwrpbmagic-x13($13)
	subq	$0,$1,$0
	beq	$0,hwrpbmagicok
	lda	$16,0x1694			# if not, all we can do is halt thusly
	call_pal pal_HALT
	br	begin
hwrpbmagicok:

	ldq	$4,hwrpb_q_ctbtoffs($3)		# point to ctb table
	addq	$4,$3,$4

	ldq	$5,hwrpb_q_ccrboffs($3)		# point to crb
	addq	$5,$3,$5

	ldq	$27,0($5)			# $27 = vaddr of dispatch routine proc descr
	ldq	$26,8($27)			# $26 = vaddr of dispatch routine entrypoint
	stq	$27,dispatch_pd-x13($13)
	stq	$26,dispatch_ep-x13($13)

	amask	1,$0				# make sure we have BWX extensions
	beq	$0,bwxok
	lda	$16,bwxbad-x13($13)
	mov	bwxbadlen,$17
	bsr	$27,print_strc
	br	$31,hang
bwxok:

	bsr	$26,print_stri			# test the print_stri routine
		.string	"oz_loader_axp: starting...\r\n"
		.p2align 2

	ldl	$0,hwrpb_l_vaexbits($3)		# make sure L0 pte bits not turned on
	beq	$0,l0bitsok
	bsr	$26,print_stri
		.string	"oz_loader_axp: can only work with three-level pagetables, not four\r\n"
		.p2align 2
	br	$31,hang
l0bitsok:

	ldq	$6,hwrpb_q_pagesize($3)		# get this CPU's page size
	stq	$6,pagesize-x13($13)		# save it where it's easy to get
	mov	0,$7
	mov	1,$2
pagesizeloop:
	cmpeq	$2,$6,$3
	blbs	$3,pagesizeok
	addq	$7,1,$7
	addq	$2,$2,$2
	bne	$2,pagesizeloop
pagesizebad:
	bsr	$26,print_stri
		.string	"oz_loader_axp: bad page size 0x"
		.p2align 2
	ldq	$16,hwrpb_q_pagesize($3)
	mov	16,$17
	bsr	$26,print_hex
	bsr	$26,print_stri
		.string	" in HWRPB\r\n"
		.p2align 2
	br	$31,hang
pagesizeok:
	cmpult	$7,L2PAGESIZE_MIN,$0		# no less than 13 (8K) to accomodate all boot stuff in one L2 page
	blbs	$0,pagesizebad			#                      ... including L2 self ref at 4000.0000
	cmpule	$7,L2PAGESIZE_MAX,$0		# no more than 16 (64K) so L2 self ref can fit at 4000.0000
	blbc	$0,pagesizebad
	stq	$7,l2pagesize-x13($13)		# save LOG2 (pagesize)
	srl	$6,3,$4
	subq	$7,3,$5
	stq	$4,ptesperpage-x13($13)		# save ptes-per-page
	stq	$5,l2ptesperpage-x13($13)

	bsr	$26,print_stri
		.string	"oz_loader_axp: pagesize "
		.p2align 2
	srl	$6,10,$16
	bsr	$26,print_dec
	bsr	$26,print_stri
		.string	"K\r\n"
		.p2align 2

	# R4 = ptesperpage
	# R5 = l2ptesperpage
	# R6 = pagesize
	# R7 = l2pagesize

	addq	$7,$5,$0			# calc number of bits in a virtual address
	addq	$0,$5,$0			# (we assume standard 3-level pagetable format)
	addq	$0,$5,$0
	stq	$0,l2vasize-x13($13)
	mov	1,$1				# make a mask for them
	sll	$1,$0,$1
	subq	$1,1,$1
	stq	$1,vamaskbits-x13($13)

	sll	$6,$5,$0			# calc size mapped by an L2 entry
	stq	$0,midpagesize-x13($13)
	sll	$0,$5,$0			# calc size mapped by an L1 entry
	stq	$0,superpagesize-x13($13)

	ldq	$0,linkedaddress-x13($13)	# make sure we're linked at the correct address
	lda	$2,_start-x13($13)		# - should be what we're currently executing at
	cmpeq	$2,$0,$1
	blbs	$1,linkedok
	bsr	$26,print_stri
		.string	"oz_loader_axp: linked at wrong address, try "
		.p2align 2
	lda	$16,_start-x13($13)
	mov	16,$17
	bsr	$26,print_hex
	bsr	$26,print_stri
		.string	"\r\n"
		.p2align 2
	br	$31,hang
linkedok:

## Verify that L1[1] = the L1 page itself

	ldq	$3,hwrpbva-x13($13)		# point to the hwrpb
	ldq	$0,superpagesize-x13($13)	# get what it should have for old base
	ldq	$2,hwrpb_q_vptbase($3)
	subq	$0,$2,$0			# see how far off we are
	beq	$0,hwrpbvptbaseok
	bsr	$26,print_stri
		.string	"oz_loader_axp: bad hwrpb_b_vptbase contents\r\n"
		.p2align 2
	br	$31,hang
hwrpbvptbaseok:

	ldq	$10,superpagesize-x13($13)	# calculate old l1 pagetable base
	ldq	$1,midpagesize-x13($13)
	ldq	$2,pagesize-x13($13)
	addq	$10,$1,$10
	addq	$10,$2,$10
	stq	$10,oldl1ptbase-x13($13)

	call_pal pal_MFPR_PTBR			# get phys page of L1 pt page into R0
	ldl	$1,12($10)			# make sure entry [1] points to the L1 page itself
	subl	$1,$0,$1
	beq	$1,l1selfrefok
	bsr	$26,print_stri
		.string	"oz_loader_axp: L1 page [1] doesn't point to itself\r\n"
		.p2align 2
	br	$31,hang
l1selfrefok:
	mov	16,$1				# except for the first 2 entries, it should be zeroes
l1pagecheck:
	addq	$10,$1,$2			# point to the entry we want to check
	ldq	$3,0($2)			# get the value in there
	beq	$3,l1pageok
	bsr	$26,print_stri			# it's non-zero, output error message
		.string	"oz_loader_axp: L1 page has non-zero pointers\r\n"
		.p2align 2
	br	$31,hang			# ... then hang
l1pageok:
	addq	$1,8,$1				# it's ok, increment page offset
	ldq	$2,l2pagesize-x13($13)
	srl	$1,$2,$2			# see if we have checked a whole page
	beq	$2,l1pagecheck			# repeat if more to do

##  Wipe out mapping of the L2 page at vaddr 0x0.4000.0000
##  It still should remain mapped at 0x2.0080.0000 (for 8K pagesize)

	ldq	$5,l2ptesperpage-x13($13)	# point to the stupid mapping pointer
	ldq	$0,phonyl2va-x13($13)
	srl	$0,$5,$0
	srl	$0,$5,$0
	ldq	$1,superpagesize-x13($13)
	ldq	$2,midpagesize-x13($13)
	addq	$0,$1,$0
	addq	$0,$2,$0

	ldq	$1,oldl1ptbase-x13($13)		# point to the good mapping pointer

	ldq	$20,0($0)			# read them
	ldq	$21,0($1)
	subq	$20,$21,$22			# they should be the same
	beq	$22,phonyl2ok
	bsr	$26,print_stri
		.string	"oz_loader_axp: phony L2 page pointer bad\r\n"
		.p2align 2
	br	$31,hang
phonyl2ok:
	stq	$31,0($0)			# they are, wipe the stupid one
	call_pal pal_MTPR_TBIA			# invalidate everything just to be sure

##  Get kernel loaded in memory - it gets mapped to a high-end (FFFFFF...) virtual address
##  This also returns with us double-mapped in the L1 pagetable page so kernel is also at VA 0

	ldq	$27,ldr_init_p-x13($13)
	ldq	$16,hwrpbva-x13($13)		# get pointer to HWRPB
	jsr	$26,($27)			# call oz_ldr_init_axp
	stq	$0,kstart-x13($13)		# returns kernel's entrypoint in R0

	ldq	$1,superpagesize-x13($13)	# get kernel's superpage base
	subq	$1,1,$1
	bic	$0,$1,$0
	stq	$0,kernelbase-x13($13)

##  Figure out where the new pagetables are

	addq	$1,1,$1				# L3 pagetable is superpage just before kernel
	subq	$0,$1,$0
	stq	$0,newl3ptbase-x13($13)

	ldq	$1,vamaskbits-x13($13)		# get masked virtual address
	and	$0,$1,$2

	ldq	$1,l2ptesperpage-x13($13)
	srl	$2,$1,$2			# this is offset of L2 pagetable
	addq	$0,$2,$0			# this is address of L2 pagetable within L3 pagetable
	stq	$0,newl2ptbase-x13($13)

	srl	$2,$1,$2			# this is offset of L1 pagetable
	addq	$0,$2,$0			# this is address of L1 pagetable within L2 pagetable
	stq	$0,newl1ptbase-x13($13)

	bsr	$26,print_stri
		.string	"oz_loader_axp: new L1 pt base "
		.p2align 2
	ldq	$16,newl1ptbase-x13($13)
	mov	 16,$17
	bsr	$26,print_hex
	bsr	$26,print_stri
		.string	"\r\noz_loader_axp: new L2 pt base "
		.p2align 2
	ldq	$16,newl2ptbase-x13($13)
	mov	 16,$17
	bsr	$26,print_hex
	bsr	$26,print_stri
		.string	"\r\noz_loader_axp: new L3 pt base "
		.p2align 2
	ldq	$16,newl3ptbase-x13($13)
	mov	 16,$17
	bsr	$26,print_hex
	bsr	$26,print_stri
		.string	"\r\n"
		.p2align 2

##  Now tell the console routines that we want it to move to the high-end virtual address range

	ldq	$9,kernelbase-x13($13)		# this is how far we are moving stuff
	ldq	$3,hwrpbva-x13($13)		# get HWRPB pointer
	ldq	$5,hwrpb_q_ccrboffs($3)		# point to existing CRB
	addq	$5,$3,$5
	ldq	$0,48($5)			# get old console base virt addr
	addq	$0,$9,$16			# this is new console base virt addr
	addq	$3,$9,$17			# this is new vaddr of HWRPB
	ldq	$27,16($5)			# get vaddr of fixup proc descr
	ldq	$26,8($27)			# get vaddr of fixup entrypoint
	jsr	$26,($26)			# tell console to relocate itself
	beq	$0,fixupsuccess
	lda	$16,0x1696
	call_pal pal_HALT
fixupsuccess:

##  Set up new VPTB register

	ldq	$3,hwrpbva-x13($13)		# get HWRPB pointer
	ldq	$16,newl3ptbase-x13($13)	# get new vaddr of L3 pagetable base
	stq	$16,hwrpb_q_vptbase($3)		# store in HWRPB
	mov	hwrpb_q_checksum/8,$0		# re-checksum the HWRPB
	mov	$3,$1
	mov	0,$2
hwrbp_checksum:
	ldq	$4,0($1)
	subq	$0,1,$0
	addq	$1,8,$1
	addq	$2,$4,$2
	bne	$0,hwrbp_checksum
	stq	$2,0($1)
	call_pal pal_MTPR_VPTB			# ... and inform PAL it is moved (R16 = new vaddr)

##  Now relocate all the entries in the CRB

	ldq	$9,kernelbase-x13($13)		# this is how far we are moving stuff
	ldq	$5,hwrpb_q_ccrboffs($3)		# point to existing CRB
	addq	$5,$3,$5
	ldq	$0,0($5)			# relocate 'dispatch' entry of CRB
	ldq	$1,8($5)			# get corresponding phys addr of dispatch proc des
	addq	$0,$9,$0
	mov	1,$2
	stq	$0,0($5)
	bsr	$26,relocrbentry
	ldq	$0,16($5)			# same for 'fixup' entry of CRB
	ldq	$1,24($5)
	addq	$0,$9,$0
	mov	1,$2
	stq	$0,16($5)
	bsr	$26,relocrbentry
	ldq	$6,32($5)			# number of remaining entries
	lda	$7,48($5)
	beq	$6,crbrelocdone
	ldq	$0,0($7)			# get old console base address
	addq	$0,$9,$0			# relocate it
crbrelocloop:
	stq	$0,0($7)			# store new virtual address
	ldq	$1,8($7)			# get physical address
	ldq	$2,16($7)			# get number of pages
	bsr	$26,relocrbentry
	subq	$6,1,$6				# one less entry to do
	addq	$7,24,$7			# point to next entry
	bne	$6,crbrelocloop			# repeat until it's done
crbrelocdone:

##  Relocate my stuff

	addq	$13,$9,$13			# relocate my base register
	addq	$sp,$9,$sp			# relocate my stack pointer
	lda	$1,relocpc-x13($13)		# relocate the PC (uses new $13 contents)
	jmp	$31,($1)
relocpc:
	ldq	$3,hwrpbva-x13($13)		# get hwrpb pointer set up
	addq	$3,$9,$3			# set up new value
	stq	$3,hwrpbva-x13($13)

	ldq	$4,newl1ptbase-x13($13)		# unmap the old stuff
	stq	$31,0($4)
	stq	$31,8($4)
	call_pal pal_MTPR_TBIA

	ldq	$5,hwrpb_q_ccrboffs($3)		# point to crb
	addq	$5,$3,$5
	ldq	$27,0($5)			# $27 = vaddr of dispatch routine proc descr
	ldq	$26,8($27)			# $26 = vaddr of dispatch routine entrypoint
	stq	$27,dispatch_pd-x13($13)
	stq	$26,dispatch_ep-x13($13)

	bsr	$26,print_stri
		.string	"oz_loader_axp: relocation complete\r\n"
		.p2align 2

##  Dump out the page tables

####	bsr	$26,dump_pagetables

##  We are now completely at the high end of virtual memory
##  And the stuff at the low end has been unmapped

	bsr	$26,print_stri
		.string	"oz_loader_axp: jumping to kernel\r\n"
		.p2align 2

	ldq	 $0,kernelbase-x13($13)
	ldq	$27,kstart-x13($13)
	ldq	$16,ldrparamblock_p-x13($13)
	addq	$16,$0,$16
	ldq	$17,hwrpbva-x13($13)
	ldq	$18,knlbasevpage-x13($13)
	addq	$18,$0,$18
	ldl	$18,0($18)
	zapnot	$18,15,$18
	ldq	$19,iobasevpage-x13($13)
	addq	$19,$0,$19
	ldl	$19,0($19)
	zapnot	$19,15,$19
	jsr	$26,($27)			# oz_kernel_axp.s: _start (&oz_ldr_paramblock, hwrpbva, knlbasevpage, iobasevpage)

##  Something went wrong, all we can do is halt

hang:
	lda	$16,0x1697
	call_pal pal_HALT			# exit back to console
	br	$31,hang			# hang here

##
##  Relocate a single CRB entry
##
##    Input:
##
##	 $0 = new virtual address
##	 $1 = corresponding physical address
##	 $2 = number of physical pages
##	$13 = base register
##	$26 = return address
##
##    Output:
##
##	$0 = incremented to next block
##
##    Scratch:
##
##	$1,$2,$16,$17,$18
##
##    Note:
##
##	This routine simply verifies the pagetable entries.
##	Everything should already be re-mapped.
##	If a bad entry is found, this routine halts.
##
relocrbentry:
	beq	$2,relocrbentrydone
	ldq	$18,pagesize-x13($13)
	subq	$18,1,$18
	xor	$0,$1,$16			# the low 13 bits must match
	and	$16,$18,$16
	beq	$16,relocrbentryloop
	lda	$16,0x1698			# if not, halt
	call_pal pal_HALT
	br	relocrbentry
relocrbentryloop:
	ldq	$18,l2pagesize-x13($13)
	ldq	$16,vamaskbits-x13($13)		# get index into L3 pagetable
	and	$16,$0,$16
	srl	$16,$18,$16
	ldq	$17,newl3ptbase-x13($13)	# point to the L3 pagetable entry
	s8addq	$16,$17,$16
	ldl	$17,4($16)			# get the physical page number
	srl	$1,$18,$18			# get what it should be
	subl	$18,$17,$18
	beq	$18,relocrbentryok
	lda	$16,0x1699			# bad, just halt
	call_pal pal_HALT			# R0=vaddr, R1=paddr, R16=pte vaddr, R17=pfn from pte
	br	relocrbentryloop
relocrbentryok:
	ldq	$16,pagesize-x13($13)
	subq	$2,1,$2				# decrement number of pages to do
	addq	$0,$16,$0			# increment virtual address
	addq	$1,$16,$1			# increment physical address
	bne	$2,relocrbentryloop		# repeat if more pages to do
relocrbentrydone:
	ret	$31,($26)

##
##  Dump pagetables
##
##    Input:
##
##	$13 = base register
##	$26 = return address
##
dump_pagetables:
	subq	$sp,88,$sp
	stq	 $2, 0($sp)
	stq	 $3, 8($sp)
	stq	 $4,16($sp)
	stq	 $5,24($sp)
	stq	 $6,32($sp)
	stq	 $7,40($sp)
	stq	 $8,48($sp)
	stq	 $9,56($sp)
	stq	$10,64($sp)
	stq	$11,72($sp)
	stq	$26,80($sp)

	mov	0,$2				# index in L1 page table
l1dump_loop:
	mov	1,$6				# print L1 page table entry
	mov	$2,$7
	call_pal pal_MFPR_PTBR
	ldq	$1,l2pagesize-x13($13)
	sll	$0,$1,$8
	bsr	$9,print_pte
	blbs	$0,l1dump_next			# don't print corresponding L2 page if not valid
	mov	$0,$10				# save phys addr of L2 page
	mov	0,$3				# init index in L2 page
l2dump_loop:
	mov	2,$6				# print L2 page table entry
	mov	$3,$7
	mov	$10,$8
	bsr	$9,print_pte
	blbs	$0,l2dump_next			# don't print corresponding L3 page if not valid
	mov	$0,$11				# save phys addr of L3 page
	mov	0,$4				# init index in L3 page
l3dump_loop:
	mov	3,$6				# print L3 page table entry
	mov	$4,$7
	mov	$11,$8
	bsr	$9,print_pte
	addq	$4,1,$4				# increment L3 index
	ldq	$0,l2ptesperpage-x13($13)
	srl	$4,$0,$0			# see if reached end of page
	beq	$0,l3dump_loop			# repeat if more to do
l2dump_next:
	addq	$3,1,$3				# increment L2 index
	ldq	$0,l2ptesperpage-x13($13)
	srl	$3,$0,$0			# see if reached end of page
	beq	$0,l2dump_loop			# repeat if more to do
l1dump_next:
	addq	$2,1,$2				# increment L1 index
	ldq	$0,l2ptesperpage-x13($13)
	srl	$2,$0,$0			# see if reached end of page
	beq	$0,l1dump_loop			# repeat if more to do

	ldq	 $2, 0($sp)
	ldq	 $3, 8($sp)
	ldq	 $4,16($sp)
	ldq	 $5,24($sp)
	ldq	 $6,32($sp)
	ldq	 $7,40($sp)
	ldq	 $8,48($sp)
	ldq	 $9,56($sp)
	ldq	$10,64($sp)
	ldq	$11,72($sp)
	ldq	$26,80($sp)
	addq	$sp,88,$sp
	ret	$31,($26)

##
##  Print pagetable entry
##
##    Input:
##
##	 $6 = level (1, 2, 3)
##	 $7 = index of entry in pt page
##	 $8 = base phys addr of page
##	 $9 = return address
##	$13 = base register
##
##    Output:
##
##	$0<0> = 1 : entry not valid
##	     else : base physaddr mapped
##
print_pte:
	subq	$sp,8,$sp
	s8addq	$7,$8,$16			# read pte[$7]
	call_pal pal_LDQP
	stq	$0,0($sp)			# save result on stack
	srl	$0,8,$1				# see if KRE (kernel read enabled)
	blbc	$0,4000f			# skip if page not valid
	blbc	$1,4000f			# skip if not enabled for kernel read
	lda	$18,spaces-x13($13)		# ok, output some spaces
	addq	$6,$6,$19			# ... to indent for the level
	bsr	$26,print_strc
	mov	'L',$16				# 'L' then the level number
	bsr	$26,print_char
	addq	$6,'0',$16
	bsr	$26,print_char
	mov	'[',$16				# [indexinpage]: space
	bsr	$26,print_char
	mov	$7,$16
	mov	3,$17
	bsr	$26,print_hex
	lda	$18,brcolsp-x13($13)
	mov	3,$19
	bsr	$26,print_strc
	ldq	$16,0($sp)			# contents of the pte
	mov	16,$17
	bsr	$26,print_hex
	subq	$6,3,$0				# see if this is an L3 pte
	bne	$0,3000f
	ldq	$6,0($sp)			# ok, get the L3 pte contents
1000:
	ldq	$0,l2ptesperpage-x13($13)
	addq	$7,1,$7				# increment index to next L3 pte
	srl	$7,$0,$0			# see if reached end of L3 page
	bne	$0,2000f
	s8addq	$7,$8,$16			# if not, read pte[$7]
	call_pal pal_LDQP
	ldq	$1,fourgb-x13($13)		# increment last pte contents
	addq	$6,$1,$6
	subq	$6,$0,$0			# see if same as new pte contents
	beq	$0,1000b			# if so, check out next entry
	subq	$6,$1,$6			# if not, back up to last matching entry
2000:
	ldq	$0,0($sp)			# end or different, see if we did any at all
	subq	$0,$6,$0
	beq	$0,3000f
	lda	$18,dotdot-x13($13)
	mov	2,$19
	bsr	$26,print_strc
	mov	$6,$16				# contents of the ending pte
	mov	16,$17
	bsr	$26,print_hex
	subq	$7,1,$4				# tell outside loop not to do these again
3000:
	lda	$18,crlf-x13($13)
	mov	2,$19
	bsr	$26,print_strc
	ldq	$0,0($sp)			# return phys addr of page mapped
	ldq	$19,l2pagesize-x13($13)
	srl	$0,32,$0			# ... by getting phys page number
	sll	$0,$19,$0			# ... then shifting over
	addq	$sp,8,$sp
	ret	$31,($9)
4000:
	mov	1,$0				# page not valid or not readable
	addq	$sp,8,$sp
	ret	$31,($9)

##
##  Print hex data
##
##    Input:
##
##	$13 = base register
##	$16 = data (in low order bits)
##	$17 = number of digits in $16 to print (1..16)
##	$26 = return address
##
print_hex:
	subq	$sp,24,$sp			# make spare stack space
	stq	$26,16($sp)			# save return address
	mov	$17,$19				# get number of chars to output
	lda	$20,hextable-x13($13)		# point to translation table

	addq	$sp,$19,$18			# point to end of string
1000:
	and	$16,15,$28			# get low order digit
	srl	$16,4,$16			# shift for next time
	addq	$20,$28,$28			# point to corresponding char in table
	subq	$18,1,$18			# adjust output pointer
	ldbu	$28,0($28)			# get char from table
	subq	$17,1,$17			# one less digit to process
	stb	$28,0($18)			# store char in buffer
	bne	$17,1000b			# repeat if more to process

	bsr	$26,print_strc			# print the counted string ($18=addr, $19=size)

	ldq	$26,16($sp)			# get return address
	addq	$sp,24,$sp			# wipe spare stack space
	ret	$31,($26)			# return

##
##  Print decimal data
##
##    Input:
##
##	$13 = base register
##	$16 = data (in low order bits)
##	$26 = return address
##
print_dec:
	subq	$sp,32,$sp			# make spare stack space
	ldq	$1,dotten-x13($13)
	stq	$26,24($sp)			# save return address
	lda	$19,24($sp)			# point to end of temp stack space
	lda	$18,24($sp)
1000:
	umulh	$16,$1,$17			# divide R16 by ten -> R17
	zap	$16,0x0F,$28
	zapnot	$16,0x0F,$27
	srl	$28,4,$28
	addq	$17,$27,$17
	srl	$17,4,$17
	addq	$17,$28,$17
	mulq	$17,10,$20			# get remainder in R20
	subq	$16,$20,$20
	addq	$20,'0',$20			# convert to ascii digit
	subq	$18,1,$18			# decrement output buffer pointer
	stb	$20,0($18)			# store digit in buffer
	mov	$17,$16				# process any remaining digits
	bne	$17,1000b
	subq	$19,$18,$19			# print the counted string ($18=addr, $19=size)
	bsr	$26,print_strc
	ldq	$26,24($sp)			# get return address
	addq	$sp,32,$sp			# wipe spare stack space
	ret	$31,($26)			# return

##
##  Print char
##
##    Input:
##
##	$13 = base register
##	$16 = char to print
##	$26 = return address
##
print_char:
	subq	$sp,16,$sp			# get stack space
	stq	$16,0($sp)			# save char to output
	stq	$26,8($sp)			# save return address
	mov	$sp,$18				# point to char
	mov	1,$19				# just one of them
	bsr	$26,print_strc			# output it
	ldq	$26,8($sp)			# get return address
	addq	$sp,16,$sp			# wipe stack
	ret	$31,($26)			# return

##
##  Print immediate string on console
##
##    Input:
##
##	$13 = base register
##	$26 = return address / string address
##
print_stri:
	mov	$26,$18			# save pointer to string
print_stri_loop:
	ldbu	$19,0($26)		# get a byte of string
	addq	$26,1,$26		# increment past it
	bne	$19,print_stri_loop	# repeat if non-null
	subq	$26,$18,$19		# get number of chars including null
	addq	$26,3,$26		# round up return address to next long
	subq	$19,1,$19		# exclude the null from string length
	bic	$26,3,$26		# finish rounding return address
##
##  Print counted string on console
##
##    Input:
##
##	$13 = base register
##	$18 = string address
##	$19 = string length
##	$26 = return address
##
##    Scratch:
##
##	$16,$17,$18,$19,$27,$28
##
print_strc:
	subq	$sp,24,$sp			# make temp stack space
	stq	$2,  0($sp)			# save scratch registers
	stq	$3,  8($sp)
	stq	$26,16($sp)			# save return address
	mov	$19,$2				# save string length
	mov	$18,$3				# save string address
	beq	$19,2000f			# maybe we're done already
1000:
	mov	dispatch_puts,$16		# get function code for PUTS
	mov	0,$17				# terminal unit number
	mov	$3,$18				# virtual address of message
	mov	$2,$19				# length of message
	ldq	$26,dispatch_ep-x13($13)	# $26 = vaddr of dispatch routine entrypoint
	ldq	$27,dispatch_pd-x13($13)	# $27 = vaddr of dispatch routine proc descr
	jsr	$26,($26)
	srl	$0,62,$1			# check error status
	bne	$1,3000f
	zapnot	$0,15,$0			# bytecount in $0<31:00>
	subq	$2,$0,$2			# that much less to do now
	addq	$3,$0,$3
	bne	$2,1000b			# repeat if there's more to do
2000:
	ldq	$2,  0($sp)			# restore scratch registers
	ldq	$3,  8($sp)
	ldq	$26,16($sp)			# restore return address
	addq	$sp,24,$sp			# wipe stack
	ret	$31,($26)			# return
3000:
	lda	$16,0x1695			# can't even print, just halt!
	call_pal pal_HALT
	br	3000b
