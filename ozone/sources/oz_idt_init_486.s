##+++2002-08-17
##    Copyright (C) 2001,2002  Mike Rieker, Beverly, MA USA
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
##---2002-08-17

##########################################################################
##									##
##  Routines common to the loader and the kernel			##
##									##
##########################################################################

	.include "oz_params_486.s"


##########################################################################
##									##
##  Default exception / interrupt handler				##
##									##
##  It just prints out the registers then hangs				##
##									##
##  These are called with an extra byte on the stack containing the 	##
##  vector number							##
##									##
##########################################################################

	.text

handler_except_msg:
	.byte	13,10
	.ascii	"oz_common_486: exception "
	.byte	-1
	.byte	0

handler_regs_msg:
	.byte	13,10
	.ascii	"  eax="
	.byte	-4
	.ascii	"  ebx="
	.byte	-4
	.ascii	"  ecx="
	.byte	-4
	.ascii	"  edx="
	.byte	-4
	.byte	13,10
	.ascii	"  esi="
	.byte	-4
	.ascii	"  edi="
	.byte	-4
	.ascii	"  ebp="
	.byte	-4
	.ascii	"  esp="
	.byte	-4
	.byte	13,10
	.ascii	"  ds="
	.byte	-2
	.ascii	"  es="
	.byte	-2
	.ascii	"  fs="
	.byte	-2
	.ascii	"  gs="
	.byte	-2
	.byte	0

handler_PF_msg:
	.byte	13,10
	.ascii	"  cr2="
	.byte	-4
	.ascii	"  ec="
	.byte	-4
	.byte	0

handler_ec_msg:
	.byte	13,10
	.ascii	"  ec="
	.byte	-4
	.byte	0

handler_from_msg_knl:
	.byte	13,10
	.ascii	"  eip="
	.byte	-4
	.ascii	"  cs="
	.byte	-4
	.ascii	"  ef="
	.byte	-4
	.byte	0

handler_from_msg_outer:
	.byte	13,10
	.ascii	"  eip="
	.byte	-4
	.ascii	"  cs="
	.byte	-4
	.ascii	"  ef="
	.byte	-4
	.ascii	"  xsp="
	.byte	-4
	.ascii	"  xss="
	.byte	-4
	.byte	0

handler_tracemsg:
	.byte	13,10
	.ascii	"  "
	.byte	-4
	.ascii	": "
	.byte	-4
	.ascii	"  "
	.byte	-4
	.byte	0

handler_dumpmem_msg:
	.byte	13,10
	.ascii	"  "
	.byte	-4
	.ascii	" "
	.byte	-4
	.ascii	" "
	.byte	-4
	.ascii	" "
	.byte	-4
	.ascii	" : "
	.byte	-4
	.ascii	" > "
	.byte	0

##########################################################################
##									##
##  Fill in the IDT with default handlers				##
##									##
##  These handlers execute in kernel mode with interrupts disabled	##
##  They dump out the registers and a traceback then hang in a loop 	##
##  forever								##
##									##
##    Input:								##
##									##
##	edi = points to 256*16 byte area for idt and pushb/jmp's	##
##									##
##    Scratch:								##
##									##
##	eax,ebx,ecx,esi,edi						##
##									##
##########################################################################

	.align	4
	.globl	oz_hw_idt_init
	.type	oz_hw_idt_init,@function
oz_hw_idt_init:
.if 000 ## BOCHS
	xorl	%eax,%eax
	movl	$256*4,%ecx
	cld
	rep
	stosl
.else
	movl	$KNLCODESEG*65536,%eax	# put KNLCODESEG in top of eax
	movl	$256,%ecx		# get number of entries to fill in
	leal	256*8(%edi),%esi	# point to where the pushb/jmp's will go
idt_init_loop1:
	movw	%si,%ax			# %eax<00:15> = low 16 bits of pushb/jmp vector entry
	movl	%esi,%ebx		# %ebx<16:31> = high 16 bits of pushb/jmp vector entry
	movw	$0x8e00,%bx		# %ebx<00:15> = vector type : kernel mode, 32-bit, interrupts disabled
	movl	%eax,0(%edi)		# store in table
	movl	%ebx,4(%edi)
	addl	$8,%edi			# increment pointers
	addl	$8,%esi
	loop	idt_init_loop1		# repeat if more to do

	leal	handler,%esi		# point to the handler
	xorb	%al,%al			# start with vector number zero
	movl	$-7,%ebx		# make displacement for first jmp
	subl	%edi,%ebx
	addl	%esi,%ebx
	movl	$256,%ecx		# get number of IDT vectors = number of pushb/jmp's to fill in
	movl	%edi,%esi		# save beginning of where the pushb/jmp's are
idt_init_loop2:
	movb	$0x6A,0(%edi)		# store pushb imm8 opcode
	movb	%al,1(%edi)		# store imm8 value = vector number
	movb	$0xE9,2(%edi)		# store jmp rel32 opcode
	movl	%ebx,3(%edi)		# store displacement from '7(%edi)' to 'handler'
	movb	$0,7(%edi)		# clear out the unused byte
	incb	%al			# increment vector number
	subl	$8,%ebx			# decrement offset from pushb/jmp to 'handler' entrypoint
	addl	$8,%edi			# increment pointer for next pushb/jmp
	loop	idt_init_loop2		# repeat for all 256 pushb/jmp's to fill in

	movl	$handler_ec-handler,%ecx # get displacement from one entrypoint to the other
	addl	%ecx, 8*8+3(%esi)	# modify vector  8 for error-code style handler
	addl	%ecx,10*8+3(%esi)	# modify vector 10 for error-code style handler
	addl	%ecx,11*8+3(%esi)	# modify vector 11 for error-code style handler
	addl	%ecx,12*8+3(%esi)	# modify vector 12 for error-code style handler
	addl	%ecx,13*8+3(%esi)	# modify vector 13 for error-code style handler
	addl	$handler_PF-handler,14*8+3(%esi) # modify vector 14 for pagefault style handler
	addl	%ecx,17*8+3(%esi)	# modify vector 17 for error-code style handler
	addl	%ecx,18*8+3(%esi)	# modify vector 18 for error-code style handler
.endif
	ret

##########################################################################
##									##
##  pagefault exception							##
##  different from the others only to print out cr2			##
##									##
##########################################################################

handler_PF:
	decb	oz_hw486_idt_lastflag		# hang if error printing error message
handler_PF_hang:
	js	handler_PF_hang
	call	handler_printregs		# print out the registers
	movl	%cr2,%eax			# get virtual address
	movl	%eax,(%esp)			# store over vector number
	leal	handler_PF_msg,%esi		# print out the virtual address and error code
	movl	%esp,%edi
	call	handler_print
	pop	%edx				# wipe error code from stack
	jmp	handler_printfrom		# print where the error came from

##########################################################################
##									##
##  exception with error code						##
##									##
##########################################################################

handler_ec:					# exception with error code
	decb	oz_hw486_idt_lastflag		# hang if error printing error message
handler_ec_hang:
	js	handler_ec_hang
	call	handler_printregs		# print out the registers
	leal	handler_ec_msg,%esi		# print out the error code
	leal	4(%esp),%edi
	call	handler_print
	pop	%edx				# wipe error code from stack
	jmp	handler_printfrom		# print where the error came from

##########################################################################
##									##
##  exception without error code					##
##									##
##########################################################################

handler:
	decb	oz_hw486_idt_lastflag		# hang if error printing error message
handler_hang:
	js	handler_hang
	call	handler_printregs		# print out the registers

#
#  Common handler routine
#
handler_printfrom:
	addl	$4,%esp				# pop exception number from stack to be nice
	leal	handler_from_msg_knl,%esi	# point to message for EIP, etc
	testb	$3,4(%esp)			# see if faulting from kernel mode
	je	handler_printfromprint
	leal	handler_from_msg_outer,%esi	# point to message for EIP, etc, include outer esp, ss
handler_printfromprint:
	movl	%esp,%edi			# point to the EIP, etc
	call	handler_print			# print it
	pushl	%ebp				# save ebp
	movl	$12,%ecx			# print at most this many frames
handler_printtrace:
	movl	%ebp,%eax			# see if end of trace dump
	testl	%eax,%eax
	je	handler_printtrace_done
	leal	handler_tracemsg,%esi		# if not, trace a frame
	pushl	%ecx
	pushl	4(%ebp)
	pushl	0(%ebp)
	pushl	%ebp
	movl	%esp,%edi
	call	handler_print
	addl	$12,%esp
	popl	%ecx
	movl	(%ebp),%ebp			# point to next frame
	loop	handler_printtrace		# maybe go dump it out
handler_printtrace_done:
	popl	%ebp				# restore ebp
handler_dumpmem:
	pushl	%ebp				# dump 16 bytes @%ebp
	pushl	  (%ebp)
	pushl	 4(%ebp)
	pushl	 8(%ebp)
	pushl	12(%ebp)
	leal	handler_dumpmem_msg,%esi
	movl	%esp,%edi
	call	handler_print
	addl	$20,%esp
	addl	$16,%ebp			# increment %ebp to next 16
handler_stop:
	inb	$0x64				# see if keyboard char present
	IODELAY
	testb	$1,%al
	je	handler_stop
	inb	$0x60				# if so, read it
	IODELAY
	jmp	handler_dumpmem			# then dump another line
##
##  Print out all the registers - be sure to leave %ebp intact
##
handler_printregs:
	pushw	%gs				# push all registers on stack so we can print them
	pushw	%fs
	pushw	%es
	pushw	%ds
	pushl	%esp
	pushl	%ebp
	pushl	%edi
	pushl	%esi
	pushl	%edx
	pushl	%ecx
	pushl	%ebx
	pushl	%eax
	movw	$KNLDATASEG,%ax			# in case data segment register got wacked
	movw	%ax,%ds
	pushl	$1				# lock everyone else out of video
	call	oz_dev_video_exclusive
	addl	$4,%esp
	leal	handler_except_msg,%esi		# print exception message
	leal	44(%esp),%edi			# - including exception number
	call	handler_print
	leal	handler_regs_msg,%esi		# point to beginning of text string
	movl	%esp,%edi			# point to where the registers are stored
	call	handler_print			# print out registers
	movl	24(%esp),%ebp			# done, restore ebp
	addl	$40,%esp			# wipe values from stack
	ret
##
##  Print out a list of registers with format string
##
##    Input:
##
##	esi = format string pointer
##	edi = register list pointer
##
handler_print:
	movb	(%esi),%al			# get a byte from the text string
	incl	%esi				# increment text string pointer
	testb	%al,%al				# test the text string byte
	js	handler_print_value		# negative means output value from stack
	je	handler_print_done		# zero means we're done
	pushl	%eax				# positive, print out the character
	pushl	$0
	call	oz_dev_video_putchar
	addl	$8,%esp
	jmp	handler_print			# repeat until end of text string
handler_print_value:
	orl	$0xffffff00,%eax		# extend al to eax
	subl	%eax,%edi			# this really increments edi to next value on stack
	movl	%eax,%ecx			# move it here out of the way
	pushl	%edi				# save address of next value
handler_print_value_loop:
	decl	%edi				# get (next) most significant byte from value on stack
	movb	(%edi),%al			# print out the byte
	call	oz_hw_print_hexb
	incb	%cl				# see if there are more bytes in the value
	jne	handler_print_value_loop	# repeat if more to do
	popl	%edi				# pop pointer to next value on stack
	jmp	handler_print			# resume processing text string
handler_print_done:
	ret

##########################################################################
##									##
##  Print hex values in eax - save and restore all scratch registers	##
##									##
##########################################################################

##
## Print a long value in hex
##
##   Input:
##
##	eax = long to print in hex
##
	.globl	oz_hw_print_hexl
	.type	oz_hw_print_hexl,@function

oz_hw_print_hexl:
	roll	$16,%eax
	call	oz_hw_print_hexw
	roll	$16,%eax
##
## Print a word value in hex
##
##   Input:
##
##	ax = word to print in hex
##
	.globl	oz_hw_print_hexw
	.type	oz_hw_print_hexw,@function

oz_hw_print_hexw:
	rolw	$8,%ax
	call	oz_hw_print_hexb
	rolw	$8,%ax
##
## Print a byte value in hex
##
##   Input:
##
##	al = byte to print in hex
##
	.globl	oz_hw_print_hexb
	.type	oz_hw_print_hexb,@function

oz_hw_print_hexb:
	pushl	%edi		# save everything that gets zapped
	pushl	%esi
	pushl	%edx
	pushl	%ecx
	pushl	%ebx
	movl	$2,%ecx		# print 2 digits
print_hexb_loop:
	rolb	$4,%al		# get digit to print in ax<0:3>
	pushl	%ecx		# save counter and value on stack
	pushl	%eax
	andb	$0x0f,%al	# and out the flags
	addb	$0x90,%al	# convert to ascii
	daa
	adcb	$0x40,%al
	daa
	pushl	%eax		# output to screen
	pushl	$0
	call	oz_dev_video_putchar
	addl	$8,%esp
	popl	%eax		# restore value and counter from stack
	popl	%ecx
	loop	print_hexb_loop	# repeat if more digits to print
	popl	%ebx		# restore the stuff we saved
	popl	%ecx
	popl	%edx
	popl	%esi
	popl	%edi
	ret
