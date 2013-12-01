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
##  This routine is loaded into memory at 0xA000 by the bootblock	##
##  The processor is in 32-bit mode as set up by the preloader		##
##									##
##  The registers are set up:						##
##									##
##	cs  = KNLCODESEG = 16 (GDT[2] is the code segment)		##
##	eip = 0xA000							##
##									##
##########################################################################

	.include "oz_params_486.s"

	.text
	.globl	_start
	.type	_start,@function
_start:

# Set up the stack and data segment registers

	movl	%eax,%edi
	movw	$KNLDATASEG,%ax		# get data segment selector (16) in ax
	movw	%ax,%ds			# set all the data related segment registers
	movw	%ax,%es			# - now we can forget all about segment registers
	movw	%ax,%fs			# - it's beginning to look like a real 32-bit processor
	movw	%ax,%gs
	movw	%ax,%ss
	movl	$GDTBASE,%esp		# set up the stack pointer to put stack just below GDT

	movl	%edi,save_eax
	movl	%ebx,save_ebx
	movl	%ecx,save_ecx
	movl	%edx,save_edx

# Now our 32-bit environment is complete

	movw	$0x03cc,%dx		# read the misc output register
	inb	%dx
	IODELAY
	movw	$0x03b4,%dx		# assume use 03b4/03b5
	and	$1,%al
	je	video_init_useit
	movw	$0x03d4,%dx		# if misc<0> set, use 03d4/03d5
video_init_useit:
	movw	%dx,video_port
	movb	$0x2a,%al		# store an '*' there in red
	movb	$4,%ah
	call	putcharw

# Output a message

	pushl	$start_msg
	pushl	$start_msglen
	call	oz_hw_putcon
	addl	$8,%esp

# Test handler print routine

#	call	handler_printregs

# Make a null stack frame so frame tracer will know when to stop

	pushl	$0			# null return address
	pushl	$0			# null previous stack frame
	movl	%esp,%ebp		# set up current frame pointer

# Fill in the IDT with default handler

	movl	$IDTBASE,%edi		# point to where we will put IDT
	call	oz_hw_idt_init		# initialize the IDT

	lidt	idtr_init		# enable it

##	movw	$KNLCODESEG,%ax		## force an exception for testing ...
##	movw	%ax,%ds			## by writing to code segment
##	movl	$0xdeadbeef,%esi
##	incb	(%esi)

# GUNZIP the loader stored after this image (at OZ_IMAGE_NEXTADDR=0x11000) and put in 0x400000

	pushl	$exp_start
	pushl	$exp_start_len
	call	oz_hw_putcon

	movl	$OZ_IMAGE_NEXTADDR,%eax
	movl	%eax,readnext
	movl	$0xA0000,%edx
	subl	%eax,%edx
	movl	%edx,readsize

	pushl	$0			# complevel = 0
	pushl	$2			# funcode = GZIP_FUNC_EXPAND
	pushl	$0			# callparam = NULL
	pushl	$freeroutine		# memory free routine
	pushl	$mallocroutine		# memory allocate routine
	pushl	$errorroutine		# error message routine
	pushl	$writeroutine		# write output routine
	pushl	$readroutine		# read input routine
	call	gzip			# GUNZIP it

	pushl	$exp_done
	pushl	$exp_done_len
	call	oz_hw_putcon

	movl	save_eax,%eax
	movl	save_ebx,%ebx
	movl	save_ecx,%ecx
	movl	save_edx,%edx

	movl	$0x400000,%edi		# jmp to the expanded image
	jmp	*%edi

# int (*readroutine) (void *param, int siz, char *buf, int *len, char **pnt)

readroutine:
	pushl	%ebp
	movl	%esp,%ebp

	movb	$'r',%al
	call	putchar
##	movl	readnext,%eax
##	call	oz_hw_print_hexl
##	movb	$':',%al
##	call	putchar
##	movl	readnext,%eax
##	movl	(%eax),%eax
##	call	oz_hw_print_hexl
##	movb	$0x0A,%al
##	call	putchar

	movl	12(%ebp),%ecx		# get the size the caller wants
	cmpl	readsize,%ecx		# make sure we have that much to give
	jc	readroutine_sizeok
	movl	readsize,%ecx		# if not, just give what we got left
readroutine_sizeok:
	subl	%ecx,readsize		# subtract it from what's remaining for next time
	movl	20(%ebp),%edx		# return it to caller in *len
	movl	%ecx,(%edx)
	movl	readnext,%eax		# get pointer to where the data is
	movl	24(%ebp),%edx		# return it to caller in *pnt
	movl	%eax,(%edx)
	addl	%ecx,readnext		# increment pointer for next time
	movl	$1,%eax			# return success status
	leave
	ret

# int (*writeroutine) (void *param, int siz, char *buf)

writeroutine:
	pushl	%ebp
	movl	%esp,%ebp
	pushl	%edi
	pushl	%esi

	movb	$'w',%al
	call	putchar
##	movl	writenext,%eax
##	call	oz_hw_print_hexl
##	movb	$0x0A,%al
##	call	putchar

	movl	12(%ebp),%ecx	# get size of expanded data
	movl	16(%ebp),%esi	# get pointer to expanded data
	movl	writenext,%edi	# this is where to store it
	addl	%ecx,writenext	# this is where next stuff goes
	cld			# copy it
	rep
	movsb
	movl	$1,%eax		# return success status
	popl	%esi
	popl	%edi
	leave
	ret

# void (*errorroutine) (void *param, int code, char *msg)

errorroutine:
	pushl	$exp_error
	pushl	$exp_error_len
	call	oz_hw_putcon
	movl	20(%esp),%ebx
	pushl	%ebx
	call	strlen
	pushl	%ebx
	pushl	%eax
	call	oz_hw_putcon
errorroutine_hang: jmp errorroutine_hang

# void *(*mallocroutine) (void *param, int size)

mallocroutine:
	movl	nextfreemem,%eax	# get address of free memory
	movl	8(%esp),%edx		# get the length wanted
	addl	$15,%edx		# make next address on 16-byte boundary
	andb	$0xF0,%dl
	addl	%edx,nextfreemem	# increment to end for next time
	ret

# void (*freeroutine) (void *param, void *buff)

freeroutine:
	ret

# Data for the above

	.align	4

save_eax:	.long	0
save_ebx:	.long	0
save_ecx:	.long	0
save_edx:	.long	0

readsize:	.long	0		# this is the size of the compressed loader image (to the hole)
readnext:	.long	0		# this is where we get the compressed image from
writenext:	.long	0x400000	# this is where we put the expanded loader image
nextfreemem:	.long	0x800000	# free memory pool starts at 8M boundary
video_port:	.word	0		# 0x3B4 or 0x3D4
idtr_init:	.word	(8*256)-1	# size (in bytes) of idt
		.long	IDTBASE		# address of idt

		.globl	oz_hw486_idt_lastflag
		.type	oz_hw486_idt_lastflag,@object
oz_hw486_idt_lastflag:	.long	1	# only allow last chance handler to execute once

start_msg:	.ascii	"\noz_ldr_expand_486: now in 32-bit mode\n"
	start_msglen = . - start_msg

exp_start:	.ascii	"oz_ldr_expand_486: expanding loader... "
	exp_start_len = . - exp_start
exp_done:	.ascii	"\noz_ldr_expand_486: loader expanded"
	exp_done_len = . - exp_done
exp_error:	.ascii	"\noz_ldr_expand_486: gunzip loader error: "
	exp_error_len = . - exp_error

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
##  Output string to screen						##
##									##
##    Input:								##
##									##
##	 4(%esp) = length of string					##
##	 8(%esp) = address of string					##
##									##
##    Scratch:								##
##									##
##	%eax, %ecx, %edx						##
##									##
##########################################################################

	.globl	oz_hw_putcon
	.type	oz_hw_putcon,@function
oz_hw_putcon:
	pushl	%ebp		# make call frame for tracing
	movl	%esp,%ebp
	pushl	%esi		# save scratch C registers
	pushl	%ebx
	movl	 8(%ebp),%ebx	# get number of chars to output
	movl	12(%ebp),%esi	# get where they are
putcon_loop:
	movb	(%esi),%al	# get a character
	incl	%esi		# increment pointer
	call	putchar		# output it to screen
	decl	%ebx		# see if any more to do
	jne	putcon_loop	# repeat if so
	popl	%ebx		# restore scratch registers
	popl	%esi
	leave			# pop call frame
	ret			# all done

# Output character in %al
# Scratch: %ecx,%edx

	.globl	oz_dev_video_putchar
	.type	oz_dev_video_putchar,@function
oz_dev_video_putchar:
	xorl	%eax,%eax
	movb	8(%esp),%al
putchar:
	movb	$0x07,%ah		# set up white char on black background
putcharw:
	pushl	%eax			# save char
	call	getcursor		# read cursor into %ecx
	popl	%eax			# restore char
	cmpb	$0x0A,%al		# see if a 'newline' char
	je	putchar_newline		# if so, go on to new line
	cmpb	$0x20,%al		# ignore all other control chars
	jc	putchar_ignore
	movl	$0xb8000,%edx		# point to base of video memory
	addl	%ecx,%edx		# add twice the cursor offset
	addl	%ecx,%edx
	movw	%ax,(%edx)		# store char and color there
	incl	%ecx			# increment cursor position
	call	putcursor		# write cursor from %ecx
putchar_ignore:
	ret

putchar_newline:
	movl	%ecx,%eax		# see how far we are into line
	xorl	%edx,%edx
	divl	eighty
	subl	eighty,%edx		# get neg (chars to end of line)
	subl	%edx,%ecx		# increment to beg of next line
	call	putcursor		# that's where the new cursor goes
	ret

eighty:	.long	80

# Read cursor into %ecx
# Scratch: %eax,%edx

getcursor:
	movzwl	video_port,%edx
	xorl	%ecx,%ecx		# clear upper bits of %ecx
	movb	$0x0E,%al		# ... output 0E -> 03?4
	outb	%dx
	IODELAY
	incw	%dx			# ... input 03?5 -> al
	inb	%dx
	IODELAY
	movb	%al,%ch			# ... that's high order cursor addr
	decw	%dx			# ... output 0F -> 03?4
	movb	$0x0F,%al
	outb	%dx
	IODELAY
	incw	%dx			# ... input 03?5 -> al
	inb	%dx
	IODELAY
	movb	%al,%cl			# ... that's low order cursor addr
	ret

# Write cursor from %ecx, scrolling if it overflowed
# Scratch: %eax,%edx

putcursor:
	cmpl	$80*25,%ecx		# check for scroll required
	jnc	putcursor_scroll
	movzwl	video_port,%edx
	movb	$0x0E,%al		# output 0E -> 03?4
	outb	%dx
	IODELAY
	incw	%dx			# output %ch -> 03?5
	movb	%ch,%al
	outb	%dx
	IODELAY
	decw	%dx			# output 0F -> 03?4
	movb	$0x0F,%al
	outb	%dx
	IODELAY
	incw	%dx			# output %cl -> 03?5
	movb	%cl,%al
	outb	%dx
	ret
putcursor_scroll:
	pushl	%edi			# save scratch registers
	pushl	%esi
	pushl	%ecx
	movl	$80*24,%ecx		# we're moving 24 lines of stuff
	movl	$0xB8000+160,%esi	# starting with the second line
	movl	$0xB8000,%edi		# put it on top of the old first line
	cld
	rep
	movsw
	movl	$80,%ecx		# blank fill the new last line
	movw	$0x0720,%ax
	rep
	stosw
	popl	%ecx			# restore scratch registers
	popl	%esi
	popl	%edi
	subl	$80,%ecx		# back cursor up one line
	jmp	putcursor		# go see if we need to scroll more

# Needed by ide last chance handler

	.globl	oz_dev_video_exclusive
	.type	oz_dev_video_exclusive,@function
oz_dev_video_exclusive:
	ret

