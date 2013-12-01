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

##########################################################################
##									##
##  C Runtime library routines that are part of the kernel		##
##									##
##########################################################################

	.text

#  4(%esp) = src
#  8(%esp) = dest
# 12(%esp) = len

	.align	4
	.globl	bcopy
	.type	bcopy,@function
bcopy:
	pushl	%ebp		# set up call frame in case of accvio
	movl	%esp,%ebp
	pushl	%edi		# save C registers
	pushl	%esi
	movl	 8(%ebp),%esi	# get input buffer address
	movl	12(%ebp),%edi	# get output buffer address
	movl	16(%ebp),%ecx	# number of bytes to copy
	movl	%esi,%eax	# (compute esi ...)
	cld			# copy forward
	subl	%edi,%eax	# (... minus edi)
	jecxz	bcopy_null	# nothing to copy
	jnc	bcopy_copy	# (if esi < edi, copy in reverse)
	std
	leal	-1(%esi,%ecx),%esi
	leal	-1(%edi,%ecx),%edi
bcopy_copy:
	rep			# repeat as long as ecx is non-zero
	movsb			# copy string
bcopy_null:
	popl	%esi		# restore registers
	popl	%edi
	leave
	ret

#  4(%esp) = dest address
#  8(%esp) = fill count

	.align	4
	.globl	bzero
	.globl	__bzero
	.type	bzero,@function
	.type	__bzero,@function
bzero:
__bzero:
	pushl	%ebp		# set up call frame in case of accvio
	movl	%esp,%ebp
	pushl	%edi		# save C registers
	movl	 8(%ebp),%edi	# output pointer
	xorl	%eax,%eax	# fill character
	movl	12(%ebp),%ecx	# byte count
	cld			# fill the buffer
	jecxz	bzero_null
	rep
	stosb
bzero_null:
	popl	%edi		# restore registers
	leave
	ret

#  4(%esp) = value

	.align	4
	.globl	ffs
	.type	ffs,@function
ffs:
	bsf	4(%esp),%eax
	je	ffs_zero
	incl	%eax
	ret
ffs_zero:
	xorl	%eax,%eax
	ret

#  4(%esp) = jmpbuf
#  8(%esp) = val

	JMP_L_EBX =  0	# in same order as glibc jmp_buf definition
	JMP_L_ESI =  4	# ... and must match OZ_Hw_jmpbuf def in oz_hw_486.h
	JMP_L_EDI =  8
	JMP_L_EBP = 12
	JMP_L_ESP = 16
	JMP_L_EIP = 20
	JMP__SIZE = 24

	.align	4
	.globl	longjmp
	.type	longjmp,@function
longjmp:
	addl	$4,%esp			# wipe return address
	popl	%edx			# get jmpbuf pointer
	popl	%eax			# get val to return
	movl	JMP_L_EBP(%edx),%ebp	# restore ebp
	movl	JMP_L_ESP(%edx),%esp	# restore esp
	movl	JMP_L_EIP(%edx),%ecx	# get the eip
	movl	JMP_L_EBX(%edx),%ebx	# restore ebx
	movl	JMP_L_ESI(%edx),%esi	# restore esi
	movl	JMP_L_EDI(%edx),%edi	# restore edi
	testl	%eax,%eax		# prevent returning zero
	jz	longjmp_zero
	jmp	*%ecx			# return to caller of setjmp
longjmp_zero:
	incl	%eax			# idiot gave us zero, convert to a one
	jmp	*%ecx			# return to caller of setjmp

#  4(%esp) = buf
#  8(%esp) = char
# 12(%esp) = len

	.align	4
	.globl	memchr
	.type	memchr,@function
memchr:
	pushl	%ebp
	movl	%esp,%ebp
	movl	 8(%ebp),%eax	# point to buffer
	movl	16(%ebp),%ecx	# get character count
	movb	12(%ebp),%dl	# get character to scan for
	jecxz	memchr_notfound	# if len is zero, char was not found
memchr_loop:
	cmpb	(%eax),%dl	# see if char matches
	je	memchr_found	# done scanning if so
	incl	%eax		# else increment pointer
	loop	memchr_loop	# repeat if more to scan
memchr_notfound:
	xorl	%eax,%eax	# not found, return NULL pointer
memchr_found:
	leave
	ret

#  4(%esp) = buf
#  8(%esp) = char
# 12(%esp) = len

	.align	4
	.globl	memchrnot
	.type	memchrnot,@function
memchrnot:
	pushl	%ebp
	movl	%esp,%ebp
	movl	 8(%ebp),%eax		# point to buffer
	movl	16(%ebp),%ecx		# get character count
	movb	12(%ebp),%dl		# get character to scan for
	jecxz	memchrnot_notfound	# if len is zero, char was not found
memchrnot_loop:
	cmpb	(%eax),%dl		# see if char matches
	jne	memchrnot_found		# done scanning if not
	incl	%eax			# else increment pointer
	loop	memchrnot_loop		# repeat if more to scan
memchrnot_notfound:
	xorl	%eax,%eax		# not found, return NULL pointer
memchrnot_found:
	leave
	ret

#  4(%esp) = buf1
#  8(%esp) = buf2
# 12(%esp) = len

	.align	4
	.globl	bcmp
	.globl	memcmp
	.type	bcmp,@function
	.type	memcmp,@function
bcmp:
memcmp:
	pushl	%ebp		# set up call frame in case of accvio
	movl	%esp,%ebp
	pushl	%edi		# save C registers
	pushl	%esi
	movl	 8(%ebp),%edi	# get buffer 1 address
	movl	12(%ebp),%esi	# get buffer 2 address
	movl	16(%ebp),%ecx	# number of bytes to compare
	xorl	%eax,%eax
	xorl	%edx,%edx
	cld			# compare forward
	jecxz	memcmp_null	# nothing to compare
	repe			# repeat as long as bytes equal
	cmpsb			# compare bytes
	movb	-1(%edi),%al
	movb	-1(%esi),%dl
	subl	%edx,%eax
memcmp_null:
	popl	%esi		# restore registers
	popl	%edi
	leave
	ret

#  4(%esp) = dest
#  8(%esp) = src
# 12(%esp) = len

	.align	4
	.globl	memcpy
	.type	memcpy,@function
memcpy:
	pushl	%ebp		# set up call frame in case of accvio
	movl	%esp,%ebp
	pushl	%edi		# save C registers
	pushl	%esi
	movl	 8(%ebp),%edi	# get output buffer address
	movl	12(%ebp),%esi	# get input buffer address
	movl	16(%ebp),%ecx	# number of bytes to copy
	cld			# copy forward
	jecxz	memcpy_null	# nothing to copy
	rep			# repeat as long as ecx is non-zero
	movsb			# copy string
memcpy_null:
	movl	8(%ebp),%eax	# return output buffer address
	popl	%esi		# restore registers
	popl	%edi
	leave
	ret

#  4(%esp) = dest
#  8(%esp) = src
# 12(%esp) = len

	.align	4
	.globl	memmove
	.type	memmove,@function
memmove:
	pushl	%ebp		# set up call frame in case of accvio
	movl	%esp,%ebp
	pushl	%edi		# save C registers
	pushl	%esi
	movl	 8(%ebp),%edi	# get output buffer address
	movl	12(%ebp),%esi	# get input buffer address
	movl	16(%ebp),%ecx	# number of bytes to copy
	movl	%esi,%eax	# (compute esi ...)
	cld			# copy forward
	subl	%edi,%eax	# (... minus edi)
	jecxz	memmove_null	# nothing to copy
	jz	memmove_null	# (if esi == edi, don't bother copying)
	jnc	memmove_copy	# (if esi < edi, copy in reverse)
	std
	leal	-1(%esi,%ecx),%esi
	leal	-1(%edi,%ecx),%edi
memmove_copy:
	rep			# repeat as long as ecx is non-zero
	movsb			# copy string
memmove_null:
	movl	8(%ebp),%eax	# return output buffer address
	popl	%esi		# restore registers
	popl	%edi
	leave
	ret

#  4(%esp) = dest address
#  8(%esp) = fill character
# 12(%esp) = fill count

	.align	4
	.globl	memset
	.type	memset,@function
memset:
	pushl	%ebp		# set up call frame in case of accvio
	movl	%esp,%ebp
	pushl	%edi		# save C registers
	movl	 8(%ebp),%edi	# output pointer
	movl	12(%ebp),%eax	# fill character
	movl	16(%ebp),%ecx	# byte count
	cld			# fill the buffer
	jecxz	memset_null
	rep
	stosb
memset_null:
	movl	8(%ebp),%eax
	popl	%edi		# restore registers
	leave
	ret

#  4(%esp) = source length
#  8(%esp) = source address
# 12(%esp) = destination length
# 16(%esp) = destination address

	.align	4
	.globl	movc4
	.type	movc4,@function
movc4:
	pushl	%ebp		# set up call frame in case of accvio
	movl	%esp,%ebp
	pushl	%edi		# save C registers
	movl	 8(%ebp),%edi	# get source length
	movl	16(%ebp),%eax	# get destination length
	cmpl	%edi,%eax	# compare with source length
	ja	movc4_withpad
	pushl	%eax		# source can fill destination, so copy the whole thing
	pushl	12(%ebp)
	pushl	20(%ebp)
	call	memmove
	movl	-4(%ebp),%edi
	leave
	ret
movc4_withpad:
	pushl	%edi		# destination will need to be padded, copy all of source
	pushl	12(%ebp)
	pushl	20(%ebp)
	call	memmove
	movl	16(%ebp),%ecx	# get destination string length
	subl	%edi,%ecx	# calculate number of bytes to pad
	addl	20(%ebp),%edi	# point to where padding goes
	xorl	%eax,%eax	# pad with zeroes
	cld
	rep
	stosb
	movl	-4(%ebp),%edi	# restore C register
	leave			# pop call frame
	ret

#  4(%esp) = jmpbuf
#  8(%esp) = save sigmask flag

	.align	4
	.globl	__sigsetjmp
	.type	__sigsetjmp,@function
__sigsetjmp:
	movl	8(%esp),%ecx		# check for a zero flag
	jecxz	common_setjmp
	pushl	%ebp			# we don't do non-zero flags
	movl	%esp,%ebp		# so make a call frame for the crash
	int3				# crash the program

#  4(%esp) = jmpbuf

	.align	4
	.globl	setjmp
	.globl	_setjmp
	.globl	__setjmp
	.type	setjmp,@function
	.type	_setjmp,@function
	.type	__setjmp,@function
common_setjmp:
setjmp:
_setjmp:
__setjmp:
	movl	4(%esp),%edx		# get jmpbuf pointer
	popl	%ecx			# get return address
	movl	%ebp,JMP_L_EBP(%edx)	# save frame pointer
	movl	%esp,JMP_L_ESP(%edx)	# save stack pointer
	movl	%ecx,JMP_L_EIP(%edx)	# save return address
	movl	%ebx,JMP_L_EBX(%edx)	# save C registers
	movl	%esi,JMP_L_ESI(%edx)
	movl	%edi,JMP_L_EDI(%edx)
	xorl	%eax,%eax		# return a zero result
	jmp	*%ecx			# return

#  4(%esp) = destination
#  8(%esp) = source

	.align	4
	.globl	stpcpy
	.globl	__stpcpy
	.type	stpcpy,@function
	.type	__stpcpy,@function
stpcpy:
__stpcpy:
	pushl	%ebp		# set up call frame in case of accvio
	movl	%esp,%ebp
	pushl	%edi		# save C registers
	pushl	%esi
	movl	12(%ebp),%esi	# get input buffer address
	movl	 8(%ebp),%edi	# get output buffer address
	cld			# copy forward
stpcpy_loop:
	lodsb			# get char from (%esi)++ to %al
	testb	%al,%al		# see if it is zero
	stosb			# store char from %al to (%edi++)
	jne	stpcpy_loop	# repeat as long as non-zero
	leal	 -1(%edi),%eax	# return end of output string
	popl	%esi		# restore registers
	popl	%edi
	leave
	ret

#  4(%esp) = string1
#  8(%esp) = string2

	.align	4
	.globl	strcasecmp
	.type	strcasecmp,@function
strcasecmp:
	pushl	%ebp		# set up call frame in case of accvio
	movl	%esp,%ebp
	pushl	%esi		# save C registers
	movl	12(%ebp),%edx	# point to string2
	movl	 8(%ebp),%esi	# point to string1
	xorl	%ecx,%ecx	# clear out the whole ecx
	xorl	%eax,%eax	# clear out the whole eax
	cld			# make lodsb increment %esi
strcasecmp_loop:
	movb	(%edx),%cl	# get string2 byte
	lodsb			# get string1 byte from (%esi++) to %al
	incl	%edx		# increment string2 pointer
	cmpb	$'A',%cl	# make sure byte2 is lowercase
	jc	strcasecmp_cl_ok
	cmpb	$'Z'+1,%cl
	jnc	strcasecmp_cl_ok
	addb	$'a'-'A',%cl
strcasecmp_cl_ok:
	cmpb	$'A',%al	# make sure byte1 is lowercase
	jc	strcasecmp_al_ok
	cmpb	$'Z'+1,%al
	jnc	strcasecmp_al_ok
	addb	$'a'-'A',%al
strcasecmp_al_ok:
	cmpb	%cl,%al		# see if characters match
	jecxz	strcasecmp_done	# stop if string2 byte is zero
	je	strcasecmp_loop	# otherwise, repeat as long as they match
strcasecmp_done:
	subl	%ecx,%eax	# return result of string1-string2
	popl	%esi		# restore registers
	leave
	ret

#  4(%esp) = dest
#  8(%esp) = src

	.align	4
	.globl	strcat
	.type	strcat,@function
strcat:
	pushl	%ebp			# make call frame
	movl	%esp,%ebp
	pushl	%edi			# save C registers
	pushl	%esi
	xorl	%eax,%eax		# we're looking for a null byte
	movl	 8(%ebp),%edi		# get starting address to scan at
	movl	$0x7fffffff,%ecx	# get maximum number of bytes to scan
	cld				# scan forward
	repne				# repeat while byte (%edi)+ != %al
	scasb
	movl	12(%ebp),%esi		# get input buffer address
	decl	%edi			# back up over the null byte
strcat_loop:
	lodsb				# get byte from (%esi)++ to %al
	testb	%al,%al			# test it for zero
	stosb				# store byte from %al to (%edi)++
	loopnz	strcat_loop		# repeat as long as byte is not zero
	movl	 8(%ebp),%eax		# return output buffer address
	popl	%esi			# restore registers
	popl	%edi
	leave
	ret

#  4(%esp) = string pointer
#  8(%esp) = character

	.align	4
	.globl	index
	.globl	__rawmemchr		# used by assinine libbfd and ar
	.globl	strchr
	.type	index,@function
	.type	__rawmemchr,@function
	.type	strchr,@function
index:
__rawmemchr:
strchr:
	pushl	%ebp			# make call frame
	movl	%esp,%ebp
	movl	 8(%ebp),%eax		# get starting address to scan at
	movb	12(%ebp),%dl		# we're looking for this byte
	xorl	%ecx,%ecx		# zero the whole ecx
strchr_loop:
	movb	(%eax),%cl		# get a byte from string
	incl	%eax			# increment string pointer
	cmpb	%cl,%dl			# see if it matches
	jecxz	strchr_abort		# stop if reached end of string
	jne	strchr_loop		# repeat if haven't found character yet
	decl	%eax			# found it, back up pointer
	leave
	ret
strchr_abort:
	je	strchr_null		# they were looking for a null, so return end-of-string pointer
	xorl	%eax,%eax		# hit null, return null pointer
	leave
	ret
strchr_null:
	decl	%eax			# found it, back up pointer
	leave
	ret

#  4(%esp) = string1
#  8(%esp) = string2

	.align	4
	.globl	strcmp
	.type	strcmp,@function
strcmp:
	pushl	%ebp			# set up call frame in case of accvio
	movl	%esp,%ebp
	pushl	%esi			# save C registers
	movl	12(%ebp),%edx		# point to string2
	movl	 8(%ebp),%esi		# point to string1
	xorl	%ecx,%ecx		# clear out the whole ecx
	xorl	%eax,%eax		# clear out the whole eax
	cld				# make lodsb increment %esi
strcmp_loop:
	movb	(%edx),%cl		# get string2 byte
	lodsb				# get string1 byte from (%esi)++ to %al
	incl	%edx			# increment string2 pointer
	cmpb	%cl,%al			# see if the bytes match
	jecxz	strcmp_done		# stop if string2 byte is zero
	je	strcmp_loop		# otherwise, repeat as long as they match
strcmp_done:
	subl	%ecx,%eax		# return result of string1-string2
	popl	%esi			# restore registers
	leave
	ret

#  4(%esp) = dest
#  8(%esp) = src

	.align	4
	.globl	strcpy
	.type	strcpy,@function
strcpy:
	pushl	%ebp		# set up call frame in case of accvio
	movl	%esp,%ebp
	pushl	%edi		# save C registers
	pushl	%esi
	movl	12(%ebp),%esi	# get input buffer address
	movl	 8(%ebp),%edi	# get output buffer address
	cld			# copy forward
strcpy_loop:
	lodsb			# get char from (%esi)++ to %al
	testb	%al,%al		# see if it is zero
	stosb			# store char from %al to (%edi++)
	jne	strcpy_loop	# repeat as long as non-zero
	movl	 8(%ebp),%eax	# return output buffer address
	popl	%esi		# restore registers
	popl	%edi
	leave
	ret

#  4(%esp) = src address

	.align	4
	.globl	strlen
	.type	strlen,@function
strlen:
	pushl	%ebp			# make call frame
	movl	%esp,%ebp
	pushl	%edi			# save C registers
	movl	8(%ebp),%edi		# get starting address to scan at
	movl	$0x7fffffff,%ecx	# get maximum number of bytes to scan
	xorl	%eax,%eax		# we're looking for a null byte
	cld				# scan forward
	repne				# repeat while byte (%edi)+ != %al
	scasb
	decl	%edi			# exclude the null from the count
	movl	%edi,%eax
	subl	8(%ebp),%eax
	popl	%edi			# restore and return
	leave
	ret

#  4(%esp) = string1
#  8(%esp) = string2
# 12(%esp) = length

	.align	4
	.globl	strncasecmp
	.type	strncasecmp,@function
strncasecmp:
	pushl	%ebp		# set up call frame in case of accvio
	movl	%esp,%ebp
	pushl	%edi		# save C registers
	pushl	%esi
	movl	12(%ebp),%edi	# point to string2
	movl	 8(%ebp),%esi	# point to string1
	movl	16(%ebp),%ecx	# get maximum length to compare
	xorl	%edx,%edx	# clear out the whole edx
	xorl	%eax,%eax	# clear out the whole eax
	jecxz	strncasecmp_done # maybe the length is zero
	cld			# make lodsb increment %esi
strncasecmp_loop:
	movb	(%edi),%dl	# get string2 byte
	lodsb			# get string1 byte from (%esi)++ to %al
	incl	%edi		# increment string2 pointer
	cmpb	$'A',%dl	# make sure byte2 is lowercase
	jc	strncasecmp_dl_ok
	cmpb	$'Z'+1,%dl
	jnc	strncasecmp_dl_ok
	addb	$'a'-'A',%dl
strncasecmp_dl_ok:
	cmpb	$'A',%al	# make sure byte1 is lowercase
	jc	strncasecmp_al_ok
	cmpb	$'Z'+1,%al
	jnc	strncasecmp_al_ok
	addb	$'a'-'A',%al
strncasecmp_al_ok:
	orb	%dl,%dl		# stop if string2 byte is zero
	je	strncasecmp_done
	cmpb	%dl,%al		# compare bytes
	loope	strncasecmp_loop # repeat as long as equal and count allows it
strncasecmp_done:
	subl	%edx,%eax	# return result of string1-string2
	popl	%esi		# restore registers
	popl	%edi
	leave
	ret

#  4(%esp) = dst
#  8(%esp) = src
# 12(%esp) = cnt : max bytes of src to copy, not including null

	.align	4
	.globl	strncat
	.type	strncat,@function
strncat:
	pushl	%ebp			# make call frame
	movl	%esp,%ebp
	pushl	%edi			# save C registers
	pushl	%esi
	xorl	%eax,%eax		# we're looking for a null byte
	movl	 8(%ebp),%edi		# get starting address to scan at
	movl	$0x7fffffff,%ecx	# get maximum number of bytes to scan
	cld				# scan forward
	repne				# repeat while byte (%edi)+ != %al
	scasb
	movl	12(%ebp),%esi		# get input buffer address
	movl	16(%ebp),%ecx		# get max number of input bytes to copy
	decl	%edi			# back up over the null byte
strncat_loop:
	lodsb				# get byte from (%esi)++ to %al
	testb	%al,%al			# test it for zero
	stosb				# store byte from %al to (%edi)++
	loopnz	strncat_loop		# repeat as long as byte is not zero
	jz	strncat_done
	movb	$0,(%edi)		# make sure we have a null terminator
strncat_done:
	movl	8(%ebp),%eax		# return output buffer address
	popl	%esi			# restore registers
	popl	%edi
	leave
	ret

#  4(%esp) = string1
#  8(%esp) = string2
# 12(%esp) = length

	.align	4
	.globl	strncmp
	.type	strncmp,@function
strncmp:
	pushl	%ebp		# set up call frame in case of accvio
	movl	%esp,%ebp
	pushl	%edi		# save C registers
	pushl	%esi
	movl	12(%ebp),%edi	# point to string2
	movl	 8(%ebp),%esi	# point to string1
	movl	16(%ebp),%ecx	# get maximum length to compare
	xorl	%edx,%edx	# clear out the whole edx
	xorl	%eax,%eax	# clear out the whole eax
	jecxz	strncmp_done	# maybe the length is zero
	cld			# make lodsb increment %esi
strncmp_loop:
	movb	(%edi),%dl	# get string2 byte
	lodsb			# get string1 byte from (%esi)++ to %al
	incl	%edi		# increment string2 pointer
	orb	%dl,%dl		# stop if string2 byte is zero
	je	strncmp_done
	cmpb	%dl,%al		# compare bytes
	loope	strncmp_loop	# repeat as long as bytes equal and count allows it
strncmp_done:
	subl	%edx,%eax	# return result of string1-string2
	popl	%esi		# restore registers
	popl	%edi
	leave
	ret

#  4(%esp) = dest
#  8(%esp) = src
# 12(%esp) = n

	.align	4
	.globl	strncpy
	.type	strncpy,@function
strncpy:
	pushl	%ebp		# set up call frame in case of accvio
	movl	%esp,%ebp
	pushl	%edi		# save C registers
	pushl	%esi
	movl	16(%ebp),%ecx	# get output buffer size
	movl	 8(%ebp),%edi	# get output buffer address
	jecxz	strncpy_done	# all done if output buffer size zero
	movl	12(%ebp),%esi	# get input buffer address
	cld			# copy forward
strncpy_loop:
	lodsb			# get byte from (%esi)++ to %al
	testb	%al,%al		# see if null byte
	stosb			# store byte from %al to (%edi)++
	loopnz	strncpy_loop	# decrement %ecx, then loop if %ecx and %al are both nonzero
	jecxz	strncpy_done	# done if output buffer filled up
	rep			# now pad the output buffer with nulls
	stosb
strncpy_done:
	movl	8(%ebp),%eax	# return output buffer address
	popl	%esi		# restore registers
	popl	%edi
	leave
	ret

# just like strncpy, but makes sure there is at least one null byte in output
#  4(%esp) = dest
#  8(%esp) = src
# 12(%esp) = n

	.align	4
	.globl	strncpyz
	.type	strncpyz,@function
strncpyz:
	pushl	%ebp		# set up call frame in case of accvio
	movl	%esp,%ebp
	pushl	%edi		# save C registers
	pushl	%esi
	movl	16(%ebp),%ecx	# get output buffer size
	movl	 8(%ebp),%edi	# get output buffer address
	jecxz	strncpyz_done	# all done if output buffer size zero
	decl	%ecx		# reserve last output buffer byte
	movl	12(%ebp),%esi	# get input buffer address
	cld			# copy forward
	jecxz	strncpyz_skip
strncpyz_loop:
	lodsb			# get byte from (%esi)++ to %al
	testb	%al,%al		# see if null byte
	stosb			# store byte from %al to (%edi)++
	loopnz	strncpyz_loop	# decrement %ecx, then loop if %ecx and %al are both nonzero
strncpyz_skip:
	incl	%ecx		# now pad the output buffer with nulls
	xorl	%eax,%eax
	rep
	stosb
strncpyz_done:
	movl	8(%ebp),%eax	# return output buffer address
	popl	%esi		# restore registers
	popl	%edi
	leave
	ret

#  4(%esp) = src address
#  8(%esp) = src length

	.align	4
	.globl	strnlen
	.type	strnlen,@function
strnlen:
	pushl	%ebp			# make call frame
	movl	%esp,%ebp
	pushl	%edi			# save C registers
	movl	 8(%ebp),%edi		# get starting address to scan at
	movl	12(%ebp),%ecx		# get maximum number of bytes to scan
	xorl	%eax,%eax		# we're looking for a null byte
	jecxz	strnlen_done		# done if zero length input
	cld				# scan forward
	repne				# repeat while byte (%edi)++ != %al
	scasb				# test by subtracting %al - (%edi)++
	movl	%edi,%eax		# point just past last byte tested
	cmc				# this sets carry if hit null, clears it otherwise
	sbbl	8(%ebp),%eax		# get number of characters scanned
strnlen_done:
	popl	%edi			# restore and return
	leave
	ret

#  4(%esp) = string pointer
#  8(%esp) = character

	.align	4
	.globl	rindex
	.globl	strrchr
	.type	rindex,@function
	.type	strrchr,@function
rindex:
strrchr:
	pushl	%ebp			# make call frame
	movl	%esp,%ebp
	pushl	%esi			# save C registers
	movl	 8(%ebp),%esi		# get starting address to scan at
	movb	12(%ebp),%dl		# we're looking for this byte
	xorl	%eax,%eax		# zero the whole eax
	xorl	%ecx,%ecx		# zero the whole ecx
	cld				# scan forward
strrchr_loop:
	movb	(%esi),%cl		# get a byte from string
	incl	%esi			# increment string pointer
	cmpb	%cl,%dl			# see if it matches
	jecxz	strrchr_done		# stop if reached end of string
	jne	strrchr_loop		# repeat if haven't found character yet
	leal	-1(%esi),%eax		# found it, save pointer to it
	jmp	strrchr_loop		# continue scanning in case there are others
strrchr_done:
	je	strrchr_null
	popl	%esi
	leave
	ret
strrchr_null:
	leal	-1(%esi),%eax		# they're looking for the null the hard way
	popl	%esi
	leave
	ret

#  4(%esp) = haystack
#  8(%esp) = needle

	.align	4
	.globl	strstr
	.type	strstr,@function
strstr:
	pushl	%ebp
	movl	%esp,%ebp
	pushl	%edi
	pushl	%esi
	pushl	%ebx
	movl	 8(%ebp),%edi		# point to haystack
	movl	12(%ebp),%ebx		# point to needle
	xorl	%ecx,%ecx		# zero the whole ecx
	cld				# make lodsb increment
strstr_loop1:
	movb	(%ebx),%al		# get first char of needle
strstr_loop3:
	movb	(%edi),%cl		# get a byte from haystack
	incl	%edi			# increment string pointer
	cmpb	%cl,%al			# see if it matches first char of needle
	jecxz	strstr_abort		# stop if reached end of haystack
	jne	strstr_loop3		# repeat if haven't found character yet
	leal	1(%ebx),%edx		# point to needle + 1
	movl	%edi,%esi		# point to haystack + 1
	xorl	%eax,%eax		# clear out the whole eax
strstr_loop2:
	movb	(%edx),%cl		# get needle byte
	lodsb				# get haystack byte from (%esi)++ to %al
	incl	%edx			# increment needle pointer
	cmpb	%cl,%al			# see if the bytes match
	jecxz	strstr_found		# success if needle byte is zero
	je	strstr_loop2		# otherwise, repeat as long as they match
	jmp	strstr_loop1		# mismatch, resume scanning for first char of needle in haystack
strstr_abort:
	movl	$1,%edi			# can't find it, return a 0
strstr_found:
	leal	-1(%edi),%eax		# found, return pointer to start of needle in haystack
strstr_done:
	popl	%ebx
	popl	%esi
	popl	%edi
	leave
	ret

	.align	4
	.globl	tolower
	.type	tolower,@function
tolower:
	movl	4(%esp),%eax
	cmpl	$0x41,%eax
	jc	tolower_rtn
	cmpl	$0x41+26,%eax
	jnc	tolower_rtn
	addl	$0x20,%eax
tolower_rtn:
	ret

	.align	4
	.globl	toupper
	.type	toupper,@function
toupper:
	movl	4(%esp),%eax
	cmpl	$0x61,%eax
	jc	toupper_rtn
	cmpl	$0x61+26,%eax
	jnc	toupper_rtn
	subl	$0x20,%eax
toupper_rtn:
	ret


## %edx:%eax = 8:4(%esp) / 16:12(%esp)

	.align	4
	.globl	__divdi3
	.type	__divdi3,@function
__divdi3:
	pushl	%ebp			# push call frame
	movl	%esp,%ebp
	movl	20(%ebp),%edx		# get divisor
	movl	16(%ebp),%eax
	testl	%edx,%edx
	js	divdi3_divisorneg	# get out if negative
	pushl	%edx			# positive, save it on stack
	pushl	%eax
	movl	12(%ebp),%edx		# get dividend
	movl	 8(%ebp),%eax
	testl	%edx,%edx
	jns	divdi3_bothpositive	# - both positive, use divdi3 as is
	notl	%edx
	notl	%eax
	addl	$1,%eax
	adcl	$0,%edx
divdi3_onlyoneisneg:
	pushl	%edx
	pushl	%eax
	call	__udivdi3
	notl	%edx
	notl	%eax
	addl	$1,%eax
	adcl	$0,%edx
	leave
	ret

divdi3_divisorneg:
	notl	%edx
	notl	%eax
	addl	$1,%eax
	adcl	$0,%edx
	pushl	%edx
	pushl	%eax
	movl	12(%ebp),%edx		# get dividend
	movl	 8(%ebp),%eax
	testl	%edx,%edx
	jns	divdi3_onlyoneisneg	# if positive, divide then negate result
	notl	%edx
	notl	%eax
	addl	$1,%eax
	adcl	$0,%edx
	pushl	%edx
	pushl	%eax
	call	__udivdi3		# both negative, divide the positives
	leave				# ... then use result as is
	ret

divdi3_bothpositive:
	leave
	jmp	__udivdi3

## %edx:%eax = 8:4(%esp) / 16:12(%esp)

	.align	4
	.globl	__udivdi3
	.type	__udivdi3,@function
__udivdi3:
	pushl	%ebp			# push call frame
	movl	%esp,%ebp
	pushl	%edi			# save C registers
	pushl	%esi
	pushl	%ebx
	movl	 8(%ebp),%eax		# get dividend
	movl	12(%ebp),%edx
	xorl	%ebx,%ebx		# ... as an 128-bit number
	xorl	%ecx,%ecx		# ... in %ecx:%ebx:%edx:%eax
	pushl	$64			# push loop counter
udivdi3_loop:
	addl	%eax,%eax		# shift dividend over one bit (and make room for quotient bit in the bottom)
	adcl	%edx,%edx
	adcl	%ebx,%ebx
	adcl	%ecx,%ecx
	movl	%ebx,%edi		# temporarily subtract divisor from high 64 bits of dividend
	movl	%ecx,%esi
	subl	16(%ebp),%edi
	sbbl	20(%ebp),%esi
	jc	udivdi3_next
	movl	%edi,%ebx		# if no borrow produced, make the subtraction permanent
	movl	%esi,%ecx
	incl	%eax			# ... and set the quotient bit
udivdi3_next:
	decl	(%esp)			# repeat if not finsihed
	jnz	udivdi3_loop
	addl	$4,%esp			# pop loop counter
					# %edx:%eax = quotient
					# %ecx:%ebx = remainder
	popl	%ebx			# restore C registers
	popl	%esi
	popl	%edi
	leave				# pop call frame
	ret

## %edx:%eax = 8:4(%esp) % 16:12(%esp)

	.align	4
	.globl	__moddi3
	.type	__moddi3,@function
__moddi3:
	pushl	%ebp			# push call frame
	movl	%esp,%ebp
	movl	20(%ebp),%edx		# get divisor
	movl	16(%ebp),%eax
	testl	%edx,%edx
	js	moddi3_divisorneg	# get out if negative
	pushl	%edx			# positive, save it on stack
	pushl	%eax
	movl	12(%ebp),%edx		# get dividend
	movl	 8(%ebp),%eax
	testl	%edx,%edx
	jns	moddi3_bothpositive	# - both positive, use moddi3 as is
moddi3_dividendisneg:
	notl	%edx
	notl	%eax
	addl	$1,%eax
	adcl	$0,%edx
	pushl	%edx
	pushl	%eax
	call	__umoddi3
	notl	%edx
	notl	%eax
	addl	$1,%eax
	adcl	$0,%edx
	leave
	ret

moddi3_divisorneg:
	notl	%edx
	notl	%eax
	addl	$1,%eax
	adcl	$0,%edx
	pushl	%edx
	pushl	%eax
	movl	12(%ebp),%edx		# get dividend
	movl	 8(%ebp),%eax
	testl	%edx,%edx
	js	moddi3_dividendisneg	# if negative, mod then negate result
	pushl	%edx
	pushl	%eax
	call	__umoddi3		# dividend positive, mod the positives
	leave				# ... then use result as is
	ret

moddi3_bothpositive:
	leave
	jmp	__umoddi3

## %edx:%eax = 8:4(%esp) % 16:12(%esp)

	.align	4
	.globl	__umoddi3
	.type	__umoddi3,@function
__umoddi3:
	pushl	%ebp			# push call frame
	movl	%esp,%ebp
	pushl	%edi			# save C registers
	pushl	%esi
	pushl	%ebx
	movl	 8(%ebp),%ebx		# get dividend
	movl	12(%ebp),%ecx
	xorl	%eax,%eax		# ... as an 128-bit number
	xorl	%edx,%edx		# ... in %edx:%eax:%ecx:%ebx
	pushl	$64			# push loop counter
umoddi3_loop:
	addl	%ebx,%ebx		# shift dividend over one bit (and make room for quotient bit in the bottom)
	adcl	%ecx,%ecx
	adcl	%eax,%eax
	adcl	%edx,%edx
	movl	%eax,%edi		# temporarily subtract divisor from high 64 bits of dividend
	movl	%edx,%esi
	subl	16(%ebp),%edi
	sbbl	20(%ebp),%esi
	jc	umoddi3_next
	movl	%edi,%eax		# if no borrow produced, make the subtraction permanent
	movl	%esi,%edx
	incl	%ebx			# ... and set the quotient bit
umoddi3_next:
	decl	(%esp)			# repeat if not finsihed
	jnz	umoddi3_loop
	addl	$4,%esp			# pop loop counter
					# %ecx:%ebx = quotient
					# %edx:%eax = remainder
	popl	%ebx			# restore C registers
	popl	%esi
	popl	%edi
	leave				# pop call frame
	ret

