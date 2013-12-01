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
##  This is the main program for the loader				##
##									##
##  It is loaded into memory at 0x00400000 by oz_ldr_expand		##
##									##
##  The registers are set up:						##
##									##
##	cs  = 0x10 (GDT[2] is the code segment)				##
##	ds,es,fs,gs,ss = 0x18 (GDT[3] is the knl data segment)		##
##	 ax = boot device type ('CD', 'FD', 'HD')			##
##	ecx = lbn of the params page on boot disk			##
##	 dl = BIOS boot device number (not used)			##
##	eip = 0x00400000						##
##	esp = just below the GDT					##
##									##
##  The GDT is set up (by the preloader) at GDTBASE (7FE0) with 4 	##
##  entries:  NULL, NULL, KNLCODESEG, KNLDATASEG			##
##									##
##  The IDT is set up (by the expander) at IDTBASE (8000) with a 	##
##  handler that just prints an exception message and hangs		##
##									##
##  The alt cpu startup routine is in 1000-1FFF.			##
##  The load parameter block is at address 9000-9FFF.			##
##									##
##  This is basically what it does:					##
##									##
##   1) Set up basic pagetable and enable paging			##
##   2) Call oz_ldr_start to read the kernel into memory at 00100000	##
##   3) Enable paging							##
##   4) Jump to the kernel						##
##									##
##  It places the MPD at 2000 and the SPT starts at 3000.  Identity 	##
##  mapping is set up for 1000..FFFFF (ie, up to and including the 	##
##  memory hole.  0..FFF is set to no-access.				##
##									##
##  It sets up the pagetable as follows:				##
##									##
##	vpages		phypage	use					##
##	00000..0009F	00000	up to the hole				##
##	000A0..000FF	000A0	memory hole, cache disabled for I/O	##
##	00100..003FF	00100	no-access for kernel loading		##
##	00400..007FF	00400	identity map for this loader stuff	##
##	00800..00BFF		no-access temp sptes for drivers	##
##	00C00..00FFF	00C00	temp pages for disk cache		##
##	01000..FFFFF	01000	identity mapped for ramdisk use		##
##									##
##  Note that everything is identity mapped except for the temp spte's 	##
##  that device drivers use to point to their controller registers	##
##									##
##########################################################################

	.include "oz_params_486.s"

	.text
	.globl	_start
	.type	_start,@function
_start:
	movl	$1,oz_s_inloader		# let any module who wants to know that this is loader, not kernel
	movw	%ax,oz_hw486_bootdevtype	# save the device type we were booted from
	movl	%ecx,oz_hw486_bootparamlbn	# save the LBN that the loadparams were read from
	movl	$1024,%ecx			# save original param page read from disk
	movl	$PRMBASE,%esi
	movl	$oz_hw486_bootparams,%edi
	cld
	rep
	movsl
	movl	$1024,%ecx			# now make copy of it that can be modified by loader commands
	movl	$PRMBASE,%esi
	movl	$oz_s_loadparams,%edi
	rep
	movsl

## Make a null stack frame so frame tracer will know when to stop

	pushl	$0			# null return address
	pushl	$0			# null previous stack frame
	movl	%esp,%ebp		# set up current frame pointer

## Initialize video

	call	oz_dev_video_init

## Output a message

	pushl	$start_msg
	pushl	$start_msglen
	call	oz_hw_putcon
	addl	$8,%esp

## Test handler print routine

##	call	handler_printregs

## Initialize debugger

	leal	idt_debug,%esi		# point to debug exception fixup table
debug_fixup_loop:
	movl	0(%esi),%eax		# get vector number
	testl	%eax,%eax
	js	debug_fixup_done
	leal	IDTBASE(,%eax,8),%edi	# point to vector to be fixed up
	movl	4(%esi),%edx		# get the new entrypoint
	movw	%dx,0(%edi)		# store low order routine address
	shrl	$16,%edx
	movw	$KNLCODESEG,2(%edi)	# store code segment number
	movw	$0x8e00,4(%edi)		# set interrupt gate descriptor code
	movw	%dx,6(%edi)		# store high order routine address
	addl	$8,%esi			# increment table pointer
	jmp	debug_fixup_loop
debug_fixup_done:
	call	oz_knl_debug_init	# initialize debugger

## Set up debug registers

####	movl	$0x80004,%eax		# address to debug
####	movl	%eax,%dr0
####	movl	%dr7,%ebx		# get current dr7 contents
####	andl	$0x0000dc00,%ebx	# clear all except reserved bits
####	orl	$0x000d0202,%ebx	# enable write-only breakpoint on dr0 longword
####	movl	%ebx,%dr7
####	movl	%dr6,%ecx		# clear exception bits
####	andl	$0xffff1ff0,%ecx
####	movl	%ecx,%dr6

####	lea	IDTBASE,%edi		# point to exception table
####	lea	debug_except,%edx	# point to debug exception routine
####	movw	%dx,8(%edi)		# store low order routine address
####	shrl	$16,%edx
####	movw	$KNLCODESEG,10(%edi)	# store code segment number
####	movw	$0x8e00,12(%edi)	# set interrupt gate descriptor code
####	movw	%dx,14(%edi)		# store high order routine address

## Check various required CPU features

	xorl	%eax,%eax		# get processor name while we're at it
	cpuid
	movl	%ebx,procname+0
	movl	%edx,procname+4
	movl	%ecx,procname+8
	movl	$1,%eax			# now get type and features
	cpuid
	movl	%edx,%ebx		# save features across print
	pushl	%edx			# print it out
	pushl	%eax
	pushl	$procname
	pushl	$cpuid_msg
	call	oz_knl_printk
	addl	$12,%esp
					# <00> = built-in FPU
					# <03> = 4M pages (used by loader to identity map memory space)
					# <04> = RDTSC instruction
					# <15> = CMOV instructions (kernel emulates as needed)
	andl	$0x0019,%ebx		# mask what we require
	xorl	$0x0019,%ebx		# make sure they're all there
	je	allfeats
	pushl	%ebx			# if not, print missing bits out
	pushl	$missing_feats
	call	oz_knl_printk
missing_feats_loop:
	cli				# won't do us any good to continue
	hlt
	jmp	missing_feats_loop
allfeats:

# Count how much memory we have if we haven't been told how much there is

	movl	oz_hw_ldr_memory_megabytes_p,%esi
	movl	(%esi),%ebx		# see if we're explicitly told how much there is
	testl	%ebx,%ebx
	je	memory_count
	pushl	%ebx
	pushl	$mem_toldmsg
	call	oz_knl_printk
	addl	$8,%esp
	jmp	memory_megabytes
memory_count:
	pushl	$mem_countmsg		# display a message
	call	oz_knl_printk
	addl	$4,%esp
	xorl	%eax,%eax		# point to memory location zero
	movl	%eax,%esi
	movl	%eax,(%esi)		# clear memory location zero
	movl	$0x100000,%ebx		# start at 1MB boundary (just above the hole)
memory_loop:
	movl	%ebx,%edi		# point to location to test
	movl	%ebx,%eax		# store the memory address back in itself
	movl	%eax,(%edi)
	movl	%eax,%ecx		# the complement should also work
	notl	%ecx
	movl	%ecx,7(%edi)
	cmpl	(%edi),%eax		# see if they read back ok
	jne	memory_done
	cmpl	7(%edi),%ecx
	jne	memory_done
	cmpl	$0,(%esi)		# make sure location zero is still zero
	jne	memory_done		# if not, we wrapped around
	addl	$0x100000,%ebx		# increment by a megabyte
	jne	memory_loop
	pushl	$mem_errmsg		# wrapped all the way around, display an message
	call	oz_knl_printk
	movl	$0x1000000,%ebx		# use default of 16Meg
memory_done:
	movl	%ebx,%eax		# print out memory size
	shrl	$20,%ebx		# ... in megabytes
	pushl	%ebx
	pushl	$mem_donemsg
	call	oz_knl_printk
	addl	$8,%esp
memory_megabytes:
	shll	$8,%ebx			# save number of physical pages
	movl	%ebx,phypages
	movl	%ebx,oz_s_phymem_totalpages

# Test keyboard routine

##	subl	$64,%esp		## make a 64-byte buffer on stack
##kbtest_loop:
##	movl	$64,%ebx		## buffer size
##	movl	%esp,%edi		## buffer address
##	movl	$kbtest_pmtlen,%ecx	## prompt size
##	lea	kbtest_pmt,%esi		## prompt address
##	call	oz_hw_keyboard_getstring
##	pushl	%eax			## save eof flag
##	movl	%edx,%eax
##	call	oz_hw_print_hexw
##	movl	%edx,%ecx		## print the line back out
##	movl	%esp,%esi
##	addl	$4,%esi
##	call	oz_hw_putcon
##	popl	%eax			## see if eof
##	orl	%eax,%eax
##	jne	kbtest_loop		## repeat if normal
##	addl	$64,%esp		## wipe temp buffer from stack

## Print out boot device values

	pushl	oz_hw486_bootparamlbn
	pushl	$oz_hw486_bootdevtype
	pushl	$bootdevmsg
	call	oz_knl_printk
	addl	$12,%esp

## Initialize the spt -

##  Make spt[0] = inaccessible to catch NULL pointers
##       spt[1..HOLEPAGE-1] = kernel read/write pages that map to identical physical addresses
##       spt[HOLEPAGE..KERNELPAGE-1] = kernel read/write pages that map to identical physical addresses, 
##                                     with caching disabled (for I/O)
##       spt[KERNELPAGE..] = no-access

##  Leave variable next_spt_addr pointing to spt[KERNELPAGE] so the kernel loading routines know where they can put the kernel
##  Leave variable next_virt_addr pointing to the corresponding page (KERNELPAGE) for the kernel loading routines

	movl	$SPTBASE,%edi		# point to spt address for virtual address zero
	movl	oz_SECTION_PAGESTATE_VALID_R,%eax # set virt page zero to null page
	shll	$9,%eax
	movl	%eax,(%edi)
	addl	$4,%edi			# increment to next page table entry

	movl	oz_SECTION_PAGESTATE_VALID_W,%eax # set up valid writable pagestate
	shll	$9,%eax
	orl	$0x1000|PT_KRW,%eax	# virtual address 4k, not dirty, not accessed, not page-cache-disable, 
					# not page-write-through, no user access, writeable, present
	movl	$HOLEPAGE-1,%ecx	# fill pages up to the hole
spt_init_loop1:
	movl	%eax,(%edi)		# set up page table entry
	addl	$0x1000,%eax		# get virtual address of next page
	addl	$4,%edi			# point to next page table entry
	loop	spt_init_loop1		# loop back around to fill in all entries

	orb	$PT_NC,%al		# use page-write-through and page-cache-disable in the hole
	movl	$KERNELPAGE-HOLEPAGE,%ecx # fill pages for the hole
spt_init_loop2:
	movl	%eax,(%edi)
	addl	$0x1000,%eax
	addl	$4,%edi
	loop	spt_init_loop2

	andl	$0xFFFFF000,%eax	# mask off the low order junk bits, leaving just phys address = virt address
	movl	%edi,next_spt_addr	# save address of next available spt entry
	movl	%eax,next_virt_addr	# save address of next available virt address
					# (kernel gets loaded there, it should be KERNELPAGE*PAGESIZE)

## Zero fill the last spt page

	movl	oz_SECTION_PAGESTATE_PAGEDOUT,%eax # say they are all paged out
	shll	$9,%eax
	movl	next_spt_addr,%ebx	# point to next available spot in the spt
zero_fill_spt_loop:
	testl	$0x0FFF,%ebx		# see if on a 4k (page) boundary
	je	zero_fill_spt_done	# if so, no more filling
	movl	%ebx,%edi		# zero fill it
	movl	%eax,(%edi)
	addl	$4,%ebx			# increment spt entry pointer
	jmp	zero_fill_spt_loop	# repeat till the page of spt's is zero filled
zero_fill_spt_done:

## Fill in the master page directory to point to the spt pages that are now filled in
## %ebx points to start of page beyond the last used spt entry

	movl	$MPDBASE,%edi		# point to master page directory
	movl	$SPTBASE|PT_URW,%eax	# point to start of spt
					# set up: not dirty, not accessed, not page-cache-disable, 
					# not page-write-through, user accessible, write, present
fill_mpd_loop:
	movl	%eax,(%edi)		# set up mpd entry to point to spt base
	addl	$0x1000,%eax		# point to next spt page
	addl	$4,%edi			# increment to next mpd entry
	cmpl	%ebx,%eax		# see if we have done all the spt pages
	jc	fill_mpd_loop		# repeat while there are more spt pages to link to mpd

## Fill out the rest of the MPD for identical virtual->to->physical mapping
## This maps us for identity mapping when paging is turned on
## It is also so the phys mem copy routines can access memory for any ramdisks
## This is temporary for the loader only, kernel wipes this stuff out

zero_fill_mpd_loop:
	movl	%edi,%edx		# get the 10 bits for identity mapping
	shll	$20,%edx
	je	zero_fill_mpd_done	# if zero, we've run off the end of the MPD
	movb	$PD_4M|PT_KRW,%dl	# set: page size = 4M, supervisor RW
	movl	%edx,(%edi)		# store the MPD entry
	addl	$4,%edi			# increment to next longword in MPD
	jmp	zero_fill_mpd_loop	# repeat
zero_fill_mpd_done:

## We are going to have a 'temp' page of sptes that drivers can use in the loader to map their device registers
## We use the third spt page which follows the loader
## The spt page will start at 5000
## The associated starting vaddr is 800000, vpage 800
## The MPD entry is at 2008
## So set up the MPD pointer to that page of sptes

	temp_spt_base   = SPTBASE + 8192			# it is the third spt page
	temp_spte_pages = 1024					# it has 1024 pte's in it
	temp_spte_vpage = (temp_spt_base - SPTBASE) / 4		# this is the virt page it maps
	temp_mpd_entry  = (temp_spte_vpage / 256) + MPDBASE	# this is the directory entry that points to it

	xorl	%eax,%eax		# clear out the page of temp spte's to begin with
	movl	$1024,%ecx		# so they will accvio if not set up properly
	movl	%ebx,%edi
	cli
	rep
	stosl

	movl	$temp_spt_base|PT_URW,temp_mpd_entry

## Also give the loader some pages to use for disk cache (for stuff like copying files)
## Use the next group of pages following the temp spt pages
## These are already identity mapped by the 4M descriptors

	temp_cache_pages   = 1024
	temp_cache_phypage = temp_spte_pages + temp_spte_vpage

## Enable paging - since we are identity mapped except for where the kernel goes, we shouldn't notice anything here

	pushl	$enpag_msg
	pushl	$enpag_msglen
	call	oz_hw_putcon
	addl	$8,%esp

	movl	%cr4,%eax		# enable 4M pages for identity mapping set up in MPD
	orb	$0x10,%al
	movl	%eax,%cr4
	movl	$MPDBASE,%ebx		# point to the MPD
	movl	%ebx,%cr3		# store in CR3
	movl	%cr0,%eax		# get what's currently in CR0
	orl	$0x80010000,%eax	# set the paging enable bit
					# also set the WP bit so we can write-protect kernel pages
	movl	%eax,%cr0		# now paging is enabled

	pushl	$pagen_msg
	pushl	$pagen_msglen
	call	oz_hw_putcon
	addl	$8,%esp

## Set up interrupt handling code and enable interrupts

	movl	$IDTBASE,%edi		# point to interrupt dispatch table
	call	oz_hw486_irq_init	# initialize interrupt environment
	sti				# enable interrupts

## Call the oz_ldr_start routine to get the kernel loaded in memory
## This also fills in the spt with entries to map the kernel

	pushl	$0			# - this one gets the number of system pages
	movl	%esp,%esi
	pushl	$0			# - this one gets the kernel stack size
	movl	%esp,%edi		# point to those zeroes

	pushl	$temp_spte_vpage	# use second spt page for a page of temp sptes
	pushl	$temp_spte_pages	# ... there are 1024 of them in the page

	pushl	$temp_cache_phypage	# give it these pages to use for disk cache
	pushl	$temp_cache_pages	# ... they are identity mapped by the 4M descriptors
					#     so stuff like oz_hw_phys_movetovirt will work

	pushl	%esi			# push pointer to 'number of system pages'
	pushl	%edi			# push pointer to 'kernel stack size'

	movl	$OZ_IMAGE_NEXTADDR,%eax	# point to end of loader image
	addl	$15,%eax		# round up to 16-byte boundary
	andb	$0xF0,%al
	pushl	%eax			# push starting address of temp pool memory = follows the loader image
	pushl	$0x800000		# push size of temp memory = up to end of the 4M block
	subl	%eax,(%esp)

	pushl	phypages		# get number of physical pages

	pushl	$KERNELPAGE		# get the starting physical page to load the kernel into

	pushl	$0			# get the virtual address that the system page table entry zero maps

	pushl	$PRMBASE		# point to where we put the parameter block

	call	oz_ldr_start		# call the loader
					# eax now contains the kernel entrypoint

	addl	$48,%esp		# pop 12 parameters from the stack
					# leave the 2 output values on stack

## Disable interrupts

	cli

## Clear the entry we made in the MPD for the temp sptes because they're mapped funny

	movl	$0,temp_mpd_entry

## Now next_spt_addr and next_virt_addr point to the first page following the kernel

## Now call the kernel with (nextavailsptentry, nextavailvirtaddr, paramblockaddr, phypages, kstacksize, systempages)
## Kstacksize, systempages are still on the stack from call to oz_ldr_start

	pushl	phypages		# push number of pages of memory we have on this computer
	pushl	$PRMBASE		# push where we put the parameter block
	pushl	next_virt_addr		# push next virtual address available after kernel = next physical address available after kernel
	pushl	next_spt_addr		# push address of corresponding spt entry
	jmp	*%eax			# call it without pushing a return address on stack

##
## Data used by the above
##

	.align	4

		.globl	oz_hw486_bootparamlbn,oz_hw486_bootparams,oz_hw486_bootdevtype
oz_hw486_bootdevtype:	.long	0	# boot device type: 'CD', 'FD', 'HD', with a null terminator
oz_hw486_bootparamlbn:	.long	0	# lbn the params are in
oz_hw486_bootparams:	.space	4096	# saved original boot param page

phypages:	.long	0		# number of physical pages this computer has
next_spt_addr:	.long	0		# next available address in the spt
next_virt_addr:	.long	0		# next available virtual address
procname:	.long	0,0,0,0		# filled in with processor name string, extra 0 to null terminate

		.word	0		# align so .long IDTBASE will be aligned (ugly cpu's)
idtr_init:	.word	(8*256)-1	# size (in bytes) of idt
		.long	IDTBASE		# address of idt

idt_debug:	.long	 0,exception_DE	# divide error
		.long	 1,exception_DB	# debug (trace trap)
		.long	 3,exception_BP	# breakpoint
		.long	 4,exception_OF	# overflow (arithmetic)
		.long	 5,exception_BR	# subscript bounds
		.long	 6,exception_UD	# undefined opcode
		.long	13,exception_GP	# general protection
		.long	14,exception_PF	# page fault
		.long	-1

start_msg:	.ascii	"\noz_loader_486: loader starting\n"
	start_msglen = . - start_msg

cpuid_msg:	.string	"oz_loader_486: CPUID: <%s> version %4.4X features %4.4X\n"
missing_feats:	.string	"oz_loader_486: these required features are missing from the CPU: %4.4X\n"

mem_toldmsg:	.string	"oz_loader_486: boot param block says there are %u megabytes of memory\n"
mem_countmsg:	.string	"oz_loader_486: counting memory - "
mem_errmsg:	.string	"wrapped all the way around, using "
mem_donemsg:	.string	"%u megabytes\n"

bootdevmsg:	.string	"oz_loader_486: booted from %s at paramlbn %X\n"

temp_spt_base_bad: .string "oz_loader_486: end of used spt %X, but temp_spt_base %X"

enpag_msg:	.ascii	"oz_loader_486: enabling paging\n"
	enpag_msglen = . - enpag_msg

pagen_msg:	.ascii	"oz_loader_486: paging enabled\n"
	pagen_msglen = . - pagen_msg

##pausetest:	.byte	10
##		.ascii	"oz_loader_486: about to call oz_ldr_start> "
##		.byte	0

##kbtest_pmt:	.byte	10
##		.ascii	"oz_loader_486: kb test> "
##	kbtest_pmtlen = . - kbtest_pmt

##########################################################################
##									##
##  These exception handlers call the kernel debugger			##
##									##
##########################################################################

	.align	4
exception_DE:				# divide by zero
	pushl	%ebp
	movl	oz_DIVBYZERO,%ebp
	jmp	exception_xx

	.align	4
exception_DB:				# debug (single step, etc)
	pushl	%ebp
	movl	oz_SINGLESTEP,%ebp
	jmp	exception_xx

	.align	4
exception_BP:				# breakpoint (int3)
	pushl	%ebp
	movl	oz_BREAKPOINT,%ebp
	jmp	exception_xx

	.align	4
exception_OF:				# arithmetic overflow
	pushl	%ebp
	movl	oz_ARITHOVER,%ebp
	jmp	exception_xx

	.align	4
exception_BR:				# subscript range
	pushl	%ebp
	movl	oz_SUBSCRIPT,%ebp
	jmp	exception_xx

	.align	4
exception_UD:				# undefined opcode
	pushl	%ebp
	movl	oz_UNDEFOPCODE,%ebp
	jmp	exception_xx

	.align	4
exception_xx:
	pushal				# build rest of mchargs
	pushl	$0
	movw	$KNLDATASEG,%ax		# make sure data segment registers are ok
	movw	%ax,%ds
	movw	%ax,%es
	movw	%ax,%fs
	movw	%ax,%gs
	movl	MCH_L_EC1(%esp),%eax	# get OZ_ error status code
	movl	$0,MCH_L_EC1(%esp)	# clear out the ec1 in the mchargs

	jmp	exception_crash

	.align	4
exception_GP:				# general protection
	xchgl	%ebp,(%esp)		# swap with error code
	pushal				# build rest of mchargs
	pushl	$0
	movw	$KNLDATASEG,%ax		# make sure data segment registers are ok
	movw	%ax,%ds
	movw	%ax,%es
	movw	%ax,%fs
	movw	%ax,%gs
	movl	oz_GENERALPROT,%eax
	jmp	exception_crash

	.align	4
exception_PF:				# page fault
	xchgl	%ebp,(%esp)		# swap with error code
	pushal				# build rest of mchargs
	movl	%cr2,%eax		# (get address that caused the fault)
	pushl	%eax
	movw	$KNLDATASEG,%ax		# make sure data segment registers are ok
	movw	%ax,%ds
	movw	%ax,%es
	movw	%ax,%fs
	movw	%ax,%gs
	movl	oz_ACCVIO,%eax
	jmp	exception_crash

	.align	4

exception_crash:
	leal	MCH_L_EBP(%esp),%ebp	# set up frame pointer
	leal	MCH_L_XSP(%esp),%ebx	# calc esp at time of exception
	movl	%ebx,MCH_L_ESP(%esp)	# save esp at time of exception in the mchargs

	movl	%esp,%ebx		# save mchargs pointer

	pushl	$0			# push sigargs on stack
	pushl	%eax
	pushl	$2

	movl	%esp,%eax		# save sigargs pointer

	cli				# inhibit hw interrupt delivery during debugger

	pushl	%ebx			# call kernel debugger
	pushl	%eax
	call	oz_knl_debug_exception

	movl	%ebx,%esp		# wipe stack to machine arguments

	addl	$4,%esp			# pop ec2 from stack
	popal				# restore all the registers (except ebp gets ec1's value)
	popl	%ebp			# pop ebp from stack
	iret				# retry the faulting instruction

##########################################################################
##									##
##  Debug exception handler						##
##									##
##  Prints out message when debug exception occurs			##
##									##
##########################################################################

	.text

debug_msg1:	.ascii	"oz_loader_486 debug_except: flags %x, eip %x, addr %x, new %x"
		.byte	10,0

	.align	4
debug_except:
	pushal				# save general registers
	movl	%dr0,%eax		# get debug registers
	movl	%dr6,%ebx
	movl	32(%esp),%edx		# get saved eip
	pushl	(%eax)			# print message
	pushl	%eax
	pushl	%edx
	pushl	%ebx
	pushl	$debug_msg1
	call	oz_knl_printk
	addl	$20,%esp		# pop call args
	popal				# restore general registers
	iret				# return

##########################################################################
##
##  Read pagetable entry
##
##    Input:
##
##	 4(%esp) = virtual page to read the pte of
##	 8(%esp) = where to return the pagestate
##	12(%esp) = where to return the physical page number
##	16(%esp) = where to return the current protection
##	20(%esp) = where to return the requested protection
##
##    Output:
##
##	 *8(%esp) = OZ_SECTION_PAGESTATE_VALID_W
##	*12(%esp) = physical page number
##	*16(%esp) = taken from pte
##	*20(%esp) = taken from pte
##
##    Note:
##
##	The only time a physical page number should differ from a virtual page 
##	is for the pages mapped with oz_hw_map_iopage.  (Realtek driver uses 
##	oz_hw_pte_writeall as well).  This routine was written in case a driver 
##	calls oz_hw_map_iopage then later wants to read back what it mapped a 
##	virtual address to (like in the ScsiPort stuff).
##
##	Also, pages where the kernel hasn't been read into yet are not mapped 
##	to anything.  But when they do get mapped, they get identity mapped.
##
##	All other pages should be identity mapped, either via the SPT or via 
##	4-meg entries in the MPD.
##
	.align	4
	.globl	oz_hw_pte_readany
	.globl	oz_hw_pte_readsys
	.type	oz_hw_pte_readany,@function
	.type	oz_hw_pte_readsys,@function
oz_hw_pte_readany:
oz_hw_pte_readsys:
	pushl	%ebp
	movl	%esp,%ebp
	movl	 8(%ebp),%eax			# get virtual page number
	cmpl	$0x100000,%eax			# we only have a million of them
	jnc	pte_read_crash1
	movl	%eax,%edx			# get index into MPD page
	shrl	$10,%edx
	movl	MPDBASE(,%edx,4),%edx		# read the MPD entry
	testb	$0x80,%dl			# see if it is one of the 4M pages
	jne	pte_read_fourmeg
	movl	SPTBASE(,%eax,4),%edx		# if not, read pagetable entry using virt page number as an index
	jmp	pte_read_gotpte
pte_read_fourmeg:
	andl	$0x03FF,%eax			# if so, make something that looks like a pte
	andl	$0xFFC00FFF,%edx
	shll	$12,%eax
	orl	%eax,%edx
pte_read_gotpte:
	movl	12(%ebp),%ecx			# see if they want page state
	jecxz	pte_read_no_pagestate
	movl	oz_SECTION_PAGESTATE_VALID_W,%eax # all our pages are valid and written to
	movl	%eax,(%ecx)
pte_read_no_pagestate:
	movl	16(%ebp),%ecx
	jecxz	pte_read_no_phypage		# see if they want physical page number
	movl	%edx,%eax			# convert physical address to physical page number
	shrl	$12,%eax
	movl	%eax,(%ecx)
pte_read_no_phypage:
	movl	20(%ebp),%ecx			# see if they want current protection
	jecxz	pte_read_no_curprot
	movb	%dl,%al				# if so, extract from pte
	shrb	$1,%al
	andb	$3,%al
	movb	%al,(%ecx)
pte_read_no_curprot:
	movl	24(%ebp),%ecx			# see if they want requested protection
	jecxz	pte_read_no_reqprot
	movb	%dl,%al				# if so, extract from pte
	shrb	$1,%al
	andb	$3,%al
	movb	%al,(%ecx)
pte_read_no_reqprot:
	xorl	%eax,%eax			# we're always faulted in
	leave
	ret

pte_read_crash1:
	pushl	%eax
	pushl	$pte_read_msg1
	call	oz_crash

pte_read_msg1:	.string	"oz_loader_486 oz_hw_pte_read: invalid virtual page number %X"

##########################################################################
##									##
##  The Realtek driver writes temp pte's to map its physically contig 	##
##  ring buffer to virtual addresses					##
##									##
##########################################################################
##									##
##  Write temp spte entry						##
##									##
##    Input:								##
##									##
##	 4(%esp) = virtual page number corresponding to pte		##
##	 8(%esp) = software page state					##
##	12(%esp) = physical page number					##
##	16(%esp) = current protection to set				##
##	20(%esp) = requested protection to set (ignored)		##
##									##
##    Output:								##
##									##
##	pte written, cache entry invalidated				##
##									##
##########################################################################

## Since loader only has one CPU running, both functions are the same

	.align	4
	.globl	oz_hw_pte_writeall
	.globl	oz_hw_pte_writecur
	.type	oz_hw_pte_writeall,@function
	.type	oz_hw_pte_writecur,@function
oz_hw_pte_writeall:
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
	invlpg	(%edx)
	leave
	ret

##  Update the pte in memory

	.align	4
pte_write:
	movl	8(%ebp),%ebx			# get vpn of page to write
	subl	$temp_spte_vpage,%ebx		# make sure it's one of the temp sptes
	jc	pte_write_crash1
	cmpl	$temp_spte_pages,%ebx
	jnc	pte_write_crash1
	leal	temp_spt_base(,%ebx,4),%esi	# point to the spte

	xorl	%ebx,%ebx			# start with a null pte

	movzbl	20(%ebp),%ecx			# get new curprot
	jecxz	pte_write_nocurprot		# zero means no access, so don't bother with it
	shll	$1,%ecx				# non-zero, shift in place and set low bit
	orl	$1,%ecx
	orl	%ecx,%ebx			# ... then store in pte
pte_write_nocurprot:

	movl	16(%ebp),%ecx			# get new phypage
	shll	$12,%ecx			# shift in place
	orl	%ecx,%ebx			# store in pte

	movzbl	12(%ebp),%ecx			# get new pagestate
	shll	$9,%ecx				# shift in place
	orl	%ecx,%ebx			# store in pte

	movl	%ebx,(%esi)			# store the new pte value

	ret					# we're done

pte_write_crash1:
	addl	$temp_spte_vpage,%ebx
	pushl	%ebx
	pushl	$pte_write_msg1
	call	oz_crash

pte_write_msg1:	.string	"oz_hw_pte_write: vpage 0x%X not in temp sptes"

##########################################################################
##
##  This routine is called by oz_ldr_start when it is about to read some of the kernel into memory
##  It maps some pages at the requested virtual addresses and sets the to Kernel Read/Write access
##
##    Input:
##
##	4(%esp) = number of pages it is about to read in
##	8(%esp) = virtual page to map it at
##
	.align	4
	.globl	oz_hw_ldr_knlpage_maprw
	.type	oz_hw_ldr_knlpage_maprw,@function
oz_hw_ldr_knlpage_maprw:
	pushl	%ebp
	movl	%esp,%ebp
	pushl	%edi

	movl	 8(%ebp),%ecx			# get number of pages to be mapped
	movl	12(%ebp),%edx			# get starting virtual page number
	leal	SPTBASE(,%edx,4),%edi		# point to corresponding page table entry
	shll	$12,%edx			# get corresponding virtual address = physical address
knlpage_maprw_loop:
	bt	$0,(%edi)			# it better not already be mapped to anything
	jc	knlpage_maprw_crash1
	testl	$0x0ffc,%edi			# see if starting new page of sptes
	jne	knlpage_maprw_nonewmpde
	movl	%edx,%eax			# new page of sptes, get corresponding virtual address
	shrl	$22,%eax			# get index in mpd of the virtual address
	movl	%edi,MPDBASE(,%eax,4)		# set up physical address of spt page = its virtual address
	movb	$PT_URW,MPDBASE(,%eax,4)	# mark the entry user read/write
knlpage_maprw_nonewmpde:
	movl	oz_SECTION_PAGESTATE_VALID_W,%eax # set up read/write, kernel only, pagestate
	shll	$9,%eax				# put in its place in pte (in the AVAIL bits)
	movb	$PT_KRW,%al			# set the spt present and write-enabled bits (no user access)
	orl	%edx,%eax			# merge in physical address = virtual address
	movl	%eax,(%edi)			# store new spt entry
	invlpg	(%edx)				# invalidate the cache entry
	addl	$4,%edi				# increment spt pointer
	addl	$4096,%edx			# increment physical page address
	loop	knlpage_maprw_loop		# repeat if more spt entries to set up
	cmpl	next_virt_addr,%edx		# see if we have a new end address
	jc	knlpage_maprw_nonewend
	movl	%edi,next_spt_addr		# if so, save the new ends
	movl	%edx,next_virt_addr

knlpage_maprw_nonewend:
	popl	%edi
	leave
	ret

knlpage_maprw_crash1:
	pushl	%edx
	pushl	(%edi)
	pushl	%edi
	pushl	$knlpage_maprw_msg1
	call	oz_crash

knlpage_maprw_msg1:	.ascii	"oz_hw_ldr_knlpage_maprw: mapping a page twice, spt %p=%x, vaddr %p"
			.byte	10,0

##########################################################################
##
##  This routine is called by oz_ldr_start after it has read some kernel pages 
##  into memory and it needs them to be set read-only (the pages are currently 
##  set read/write by the above maprw routine)
##
##    Input:
##
##	4(%esp) = number of pages
##	8(%esp) = virtual page
##
	.align	4
	.globl	oz_hw_ldr_knlpage_setro
	.type	oz_hw_ldr_knlpage_setro,@function
oz_hw_ldr_knlpage_setro:
	pushl	%ebp
	movl	%esp,%ebp
	pushl	%edi

	movl	 8(%ebp),%ecx			# get number of pages
	movl	12(%ebp),%edx			# get virtual page number
	lea	SPTBASE(,%edx,4),%edi		# point to corresponding page table entry
	shll	$12,%edx			# get corresponding virtual address
	movl	oz_SECTION_PAGESTATE_VALID_R,%eax # set up read-only, user/kernel, pagestate
	shll	$9,%eax				# put in its place in pte (in the AVAIL bits)
	orl	$PT_URO,%eax			# set the spt present and user-mode bits
	orl	%eax,%edx			# merge into virtual address
knlpage_setro_loop:
	movl	%edx,(%edi)			# set up new pte
	invlpg	(%edx)				# invalidate the page
	addl	$4096,%edx			# increment to next virtual address
	addl	$4,%edi				# increment to next pte
	loop	knlpage_setro_loop		# repeat until all are set up

	popl	%edi
	leave
	ret

##
## New smp level entered - inhibit/enable hardware interrupts
##
	.align	4
	.globl	oz_hw_smplock_newlevel
	.type	oz_hw_smplock_newlevel,@function
oz_hw_smplock_newlevel:
	movl	4(%esp),%eax		# get new smp level
	cmpl	$OZ_SMPLOCK_LEVEL_IRQS,%eax # see if at or above lowest IRQ level
	jnc	newlevel_inh
	sti				# if not, enable interrupt delivery
	ret
newlevel_inh:
	cli				# if so, inhibit interrupt delivery
	ret
##
##  Print pte for a given virtual address
##
	.align	4
	.globl	oz_hw_pte_print
	.type	oz_hw_pte_print,@function
oz_hw_pte_print:
	pushl	4(%esp)
	pushl	$pte_print_msg
	call	oz_knl_printk
	addl	$8,%esp
	ret

pte_print_msg:	.string	"oz_hw_pte_print: vaddr %p\n"

##
##  Return from interrupt with mchargs on the stack
##
	.align	4
	.globl	oz_hw_iretmchargs
	.type	oz_hw_iretmchargs,@function
oz_hw_iretmchargs:
	addl	$4,%esp				# pop off EC2
	popal					# restore general registers
	popl	%ebp				# restore caller's frame pointer
	iret					# we're all done

##
##  Store long at physical address
##  Fortunately, our physical addresses = virtual address
##
	.globl	oz_hw486_phys_storelong
	.type	oz_hw486_phys_storelong,@function
oz_hw486_phys_storelong:
	movl	4(%esp),%eax
	movl	8(%esp),%edx
	movl	%eax,(%edx)
	ret
##
##  Fetch long at physical address
##
	.globl	oz_hw486_phys_fetchlong
	.type	oz_hw486_phys_fetchlong,@function
oz_hw486_phys_fetchlong:
	movl	4(%esp),%eax
	movl	(%eax),%eax
	ret
##
##  Map a physical page
##  Fortunately, our physical addresses = virtual address
##  void *oz_hw_phys_mappage (OZ_Mempage physpage, OZ_Pagentry *oldpte);
##
	.globl	oz_hw_phys_mappage
	.type	oz_hw_phys_mappage,@function
oz_hw_phys_mappage:
	movl	4(%esp),%eax
	shll	$12,%eax
	ret
##
##  void oz_hw_phys_unmappage (OZ_Pagentry oldpte);
##
	.globl	oz_hw_phys_unmappage
	.type	oz_hw_phys_unmappage,@function
oz_hw_phys_unmappage:
	ret

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

	.align	4
	.globl	oz_hw_debug_writebpt
	.type	oz_hw_debug_writebpt,@function
oz_hw_debug_writebpt:
	pushl	%ebp				# make call frame for trace
	movl	%esp,%ebp
	movl	 8(%ebp),%edx			# get the address
	movb	12(%ebp),%al			# get breakpoint instruction opcode
	movb	%al,(%edx)			# write the breakpoint value
	xorl	%eax,%eax			# successful
	leave
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
##    Note:								##
##									##
##	This version assumes processor mode is always kernel		##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_probe
	.type	oz_hw_probe,@function
oz_hw_probe:
	pushl	%ebp					# make a call frame for tracing if crash
	movl	%esp,%ebp
	pushl	%ebx					# save C register
	xorl	%eax,%eax				# assume we will fail some test
	movl	 8(%ebp),%edx				# number of bytes
	movl	12(%ebp),%ecx				# starting address
	addl	%ecx,%edx				# point to end of area
	jc	probe_rtn				# fail if wrapped around
	decl	%edx					# make end pointer inclusive
	shrl	$12,%ecx				# get starting virtual page number
	shrl	$12,%edx				# get end virtual page number (inclusive)
	movl	%edx,8(%ebp)				# save end page number
probe_loop:
##     %ecx = virtual page number to test
##  8(%ebp) = last virtual page number to test (inclusive)
## 20(%ebp) = read/write flag
	movl	%ecx,%edx				# get virtual page number
	shrl	$10,%edx				# get index in MPD that maps the virtual page
	movl	MPDBASE(,%edx,4),%ebx			# get MPD entry
	testb	$0x81,%bl				# make sure the present bit is set
	js	probe_fourmeg				# (pretend it is the pte if 4M bit is set)
	je	probe_rtn				# return failure status if MPD entry not present
	movl	SPTBASE(,%ecx,4),%ebx			# get page table entry
probe_fourkay:
	testb	$1,%bl					# present bit must be set
	je	probe_rtn				# return zero if not
	testb	$2,%bl					# see if page is writable
	jne	probe_suc				# if so, successful
	testb	$1,20(%ebp)				# page is read-only, see if they are looking for write access
	jne	probe_rtn				# if so, return failure status
probe_suc:
	incl	%ecx					# advance to next page
	cmpl	8(%ebp),%ecx				# see if more to probe
	jle	probe_loop
	incl	%eax					# success, set return value = 1
probe_rtn:
	popl	%ebx					# restore C register
	leave						# pop call frame
	ret

probe_fourmeg:
	orl	$0x03FF,%ecx				# if 4M page is ok, zip to end of it for next loop
	jmp	probe_fourkay				# pretend it was a 4K pte and check it out

##########################################################################
##									##
##  Map an I/O page (uncached)						##
##									##
##	 4(%esp) = physical page number					##
##	 8(%esp) = virt address corresponding to spte			##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_map_iopage
	.type	oz_hw_map_iopage,@function
oz_hw_map_iopage:
	pushl	%ebp
	movl	%esp,%ebp
	movl	 8(%ebp),%eax		# get physical page number
	shll	$12,%eax		# shift to get physical address
	movb	$0x1B,%al		# put in bits:
					# <0> = 1 : valid
					# <1> = 1 : write-enabled
					# <2> = 0 : kernel only
					# <3> = 1 : write-through
					# <4> = 1 : cache disabled
	movl	12(%ebp),%ecx		# get virt address
	movl	%ecx,%edx		# get virtual page number
	shrl	$12,%edx
	movl	%eax,SPTBASE(,%edx,4)	# store new page table entry
	invlpg	(%ecx)			# invalidate the cache entry
	leave
	ret

##########################################################################
##									##
##  Debugging routine to output a single character in %al, in red	##
##  Note: this routine does not scroll so it is limited in its use	##
##									##
##########################################################################

	.align	4
	.globl	oz_ldr_debchar
	.type	oz_ldr_debchar,@function
oz_ldr_debchar:
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
	popal				## restore all registers
	ret				## all done

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
	cli
	cld
	xorl	%eax,%eax
	movl	$1024,%ecx
	movl	$MPDBASE,%edi
	stosl
	movl	$MPDBASE,%eax
	movl	%eax,%cr3
	jmp	oz_hw_reboot

##########################################################################
##									##
##  This halts the cpu by by doing an hlt instruction in an infinite 	##
##  loop								##
##									##
##	oz_hw_halt ()							##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_halt
	.type	oz_hw_halt,@function
oz_hw_halt:
	cli
	hlt
	jmp	oz_hw_halt

##########################################################################
##									##
##  Flush processor cache						##
##									##
##########################################################################

	.align	4
	.globl	oz_hw486_wbinvd
	.type	oz_hw486_wbinvd,@function
oz_hw486_wbinvd:
	wbinvd
	ret

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
	movl	%cr0,%eax
	movl	%eax,MCHXK_L_CR0(%ecx)
	movl	%cr2,%eax
	movl	%eax,MCHXK_L_CR2(%ecx)
	movl	%cr3,%eax
	movl	%eax,MCHXK_L_CR3(%ecx)
	movl	%cr4,%eax
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
	movl	%eax,%cr0
	movl	MCHXK_L_CR3(%ecx),%eax
	movl	%eax,%cr3
	movl	MCHXK_L_CR4(%ecx),%eax
	movl	%eax,%cr4
	leave
	ret
