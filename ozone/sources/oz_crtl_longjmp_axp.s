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

	JMPBUF_R9  =  0
	JMPBUF_R10 =  8
	JMPBUF_R11 = 16
	JMPBUF_R12 = 24
	JMPBUF_R13 = 32
	JMPBUF_R14 = 40
	JMPBUF_R15 = 48
	JMPBUF_PC  = 56
	JMPBUF_SP  = 64

	.text

	.globl	setjmp
	.type	setjmp,@function
	.p2align 4
setjmp:
	stq	 $9,JMPBUF_R9($16)
	stq	$10,JMPBUF_R10($16)
	stq	$11,JMPBUF_R11($16)
	stq	$12,JMPBUF_R12($16)
	stq	$13,JMPBUF_R13($16)
	stq	$14,JMPBUF_R14($16)
	stq	$15,JMPBUF_R15($16)
	stq	$26,JMPBUF_PC($16)
	stq	$sp,JMPBUF_SP($16)
	mov	0,$0
	ret	$31,($26)

	.globl	longjmp
	.type	longjmp,@function
	.p2align 4
longjmp:
	cmoveq	$17,1,$17
	ldq	 $9,JMPBUF_R9($16)
	ldq	$10,JMPBUF_R10($16)
	ldq	$11,JMPBUF_R11($16)
	ldq	$12,JMPBUF_R12($16)
	ldq	$13,JMPBUF_R13($16)
	ldq	$14,JMPBUF_R14($16)
	ldq	$15,JMPBUF_R15($16)
	ldq	$26,JMPBUF_PC($16)
	ldq	$sp,JMPBUF_SP($16)
	mov	$17,$0
	ret	$31,($26)

