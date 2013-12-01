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

	.text
	.align	4
	.globl	rdtsc
rdtsc:
	rdtsc
	ret

/************************************************************************/
/*									*/
/*  Generate ip-style checksum on a buffer				*/
/*									*/
/*    Input:								*/
/*									*/
/*	 4(%esp) = number of bytes to process				*/
/*	 8(%esp) = source buffer address				*/
/*	12(%esp) = starting checksum value				*/
/*									*/
/*    Output:								*/
/*									*/
/*	%ax = resulting checksum					*/
/*									*/
/*    Note:								*/
/*									*/
/*	If odd bytecount, resulting checksum will be byteswapped.  If 	*/
/*	you will be appending onto the same buffer, just continue 	*/
/*	using this checksum.  Then, when ready to send, and the total 	*/
/*	is odd, swap the checksum bytes.  If the total is even, just 	*/
/*	use it as is.							*/
/*									*/
/************************************************************************/

	.text
	.align	4
	.globl	oz_dev_ip_gencksmb
	.type	oz_dev_ip_gencksmb,@function
oz_dev_ip_gencksmb:
	pushl	%ebp			# make call frame on stack in case of error
	movl	%esp,%ebp
	pushl	%esi			# save scratch registers
	xorl	%eax,%eax		# clear top half of accumulator
	xorl	%edx,%edx		# clear all of %edx
	movl	 8(%ebp),%ecx		# get number of bytes to be checksummed
	movl	12(%ebp),%esi		# point to array of bytes to checksum
	movw	16(%ebp),%dx		# get start value
	jecxz	cksmb_null		# check for zero length
	cld				# make sure string instructions go forward
	.align	4
cksmb_loop:
	lodsb				# get byte from (%esi)+ into eax<00:07>
	bswap	%edx			# swap accumulator bytes
	subl	%eax,%edx		# subtract byte from accumulator
	loop	cksmb_loop		# repeat for more bytes
	movl	%edx,%eax		# get final end-arounds
	shrl	$16,%eax
	addw	%dx,%ax
	adcw	$0,%ax
	je	cksmb_zero
	popl	%esi
	leave
	ret
cksmb_null:
	movw	%dx,%ax			# nothing to process, just return start value as is
	popl	%esi
	leave
	ret
cksmb_zero:
	notw	%ax			# return FFFF en lieu de 0000
	popl	%esi
	leave
	ret

/************************************************************************/
/*									*/
/*  Generate ip-style checksum on a buffer and copy it			*/
/*									*/
/*    Input:								*/
/*									*/
/*	 4(%esp) = number of bytes to process				*/
/*	 8(%esp) = source buffer address				*/
/*	12(%esp) = destination buffer address				*/
/*	16(%esp) = starting checksum value				*/
/*									*/
/*    Output:								*/
/*									*/
/*	bytes copied from source to destination				*/
/*	%ax = resulting checksum					*/
/*									*/
/*    Note:								*/
/*									*/
/*	If odd bytecount, resulting checksum will be byteswapped.  If 	*/
/*	you will be appending onto the same buffer, just continue 	*/
/*	using this checksum.  Then, when ready to send, and the total 	*/
/*	is odd, swap the checksum bytes.  If the total is even, just 	*/
/*	use it as is.							*/
/*									*/
/************************************************************************/

	.text
	.align	4
	.globl	oz_dev_ip_gencksmbc
	.type	oz_dev_ip_gencksmbc,@function
oz_dev_ip_gencksmbc:
	pushl	%ebp			# make call frame on stack in case of error
	movl	%esp,%ebp
	pushl	%edi			# save scratch registers
	pushl	%esi
	xorl	%eax,%eax		# clear top half of accumulator
	xorl	%edx,%edx		# clear all of %edx
	movl	 8(%ebp),%ecx		# get number of bytes to be checksummed
	movl	12(%ebp),%esi		# point to array of bytes to checksum
	movl	16(%ebp),%edi		# point to where to copy them to
	movw	20(%ebp),%dx		# get start value
	jecxz	cksmbc_null		# check for zero length
	cld				# make sure string instructions go forward
	.align	4
cksmbc_loop:
	lodsb				# get byte from (%esi)+ into eax<00:07>
	bswap	%edx			# swap accumulator bytes
	stosb				# store byte to (%edi)+
	subl	%eax,%edx		# subtract byte from accumulator
	loop	cksmbc_loop		# repeat for more bytes
	movl	%edx,%eax		# get final end-arounds
	shrl	$16,%eax
	addw	%dx,%ax
	adcw	$0,%ax
	je	cksmbc_zero
	popl	%esi
	popl	%edi
	leave
	ret
cksmbc_null:
	movw	%dx,%ax			# nothing to process, just return start value as is
	popl	%esi
	popl	%edi
	leave
	ret
cksmbc_zero:
	notw	%ax			# return FFFF en lieu de 0000
	popl	%esi
	popl	%edi
	leave
	ret
