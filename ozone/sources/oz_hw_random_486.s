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
##  Fill a buffer with random data					##
##									##
##    Input:								##
##									##
##	 4(%esp) = number of bytes to generate				##
##	 8(%esp) = where to put random data				##
##									##
##########################################################################

	.data
	.align	4
	.globl	oz_hw486_random_seed
	.type	oz_hw486_random_seed,@object
oz_hw486_random_seed:
seedbuf:
lasthash:		.long	0,0,0,0	# last hashing result (feedback)
rdtscvalue:		.long	0	# value from current rdtsc instruction
int_rdtscbyte:		.long	0,0,0,0	# rdtsc bytes from last 16 interrupts
int_count:		.long	0	# number of interrupts received
	seedlen = . - seedbuf

	.text

# This routine should be called at the beginning of an interrupt routine
# It stores the low byte of the rdtsc (cpu cycle) counter to the next slot in the int_rdtscbyte array

	.align	4
	.globl	oz_hw486_random_int
	.type	oz_hw486_random_int,@function
oz_hw486_random_int:
	rdtsc				# get some 'random' bits
	movl	int_count,%edx		# see which byte to store them in
	andl	$15,%edx		# ... wrapped at 16 bytes
	movb	%al,int_rdtscbyte(%edx)	# store them in a byte
	incl	int_count		# increment number of interrupts
	ret

# This routine is called to fill a buffer with a sequence of random numbers

	.align	4
	.globl	oz_hw_random_fill
	.type	oz_hw_random_fill,@function
oz_hw_random_fill:
	pushl	%ebp			# make call frame
	movl	%esp,%ebp
	pushl	%edi			# save scratch registers
	pushl	%esi
	pushl	%ebx

	movl	 8(%ebp),%ebx		# get number of bytes to generate
	movl	$seedbuf,%esi		# point to saved random seed data
	movl	12(%ebp),%edi		# point to where output goes
fill_loop:
	subl	$16,%ebx		# see if there are 16 bytes to go
	jc	fill_done
	call	randomise		# ok, output 16 bytes
	addl	$16,%edi		# increment and repeat
	jmp	fill_loop
fill_done:
	addl	$16,%ebx		# last bit, see if anything left to do
	je	fill_rtn
	subl	$16,%esp		# ok, make 16 bytes on stack
	pushl	%edi
	movl	%esp,%edi
	call	randomise
	popl	%edi			# copy as much as the caller wants
	movl	%esp,%esi
	movl	%ebx,%ecx
	cld
	rep
	movsb
fill_rtn:
	leal	-12(%ebp),%esp		# point to register save area
	popl	%ebx			# restore scratch
	popl	%esi
	popl	%edi
	leave				# pop call frame
	ret				# return

# Generate 16 bytes (128 bits) of random data

#  Input:
#   %esi = points to seedbuf
#   %edi = where to put output

#  Output:
#   *%esi = modified with feedback
#   *%edi = filled in
#   %eax,%edx,%ecx = scratch

	.align	4
randomise:
	subl	$16,%esp				# this is where temp output goes
	rdtsc						# get a 'random' number
	movl	%eax,rdtscvalue-seedbuf(%esi)		# store it in seed buffer
	pushl	%esp					# hash all the seed stuff
	pushl	%esi					# - into temp output
	pushl	$seedlen
	call	oz_sys_hash
	addl	$12,%esp
	popl	%eax					# output 8 bytes of it
	popl	%edx
	movl	%eax,  (%edi)
	movl	%edx, 4(%edi)
	movl	%eax,lasthash   -seedbuf(%esi)		# - and save in seed for next time
	movl	%edx,lasthash+ 4-seedbuf(%esi)
	popl	%eax					# do the last 8 bytes
	popl	%edx
	movl	%eax, 8(%edi)
	movl	%edx,12(%edi)
	movl	%eax,lasthash+ 8-seedbuf(%esi)
	movl	%edx,lasthash+12-seedbuf(%esi)
	ret
