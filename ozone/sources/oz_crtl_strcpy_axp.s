##+++2003-12-12
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
##---2003-12-12

##########################################################################
##									##
##  Prefix file for oz_crtl_copy_axp to build 'strcpy' routine		##
##									##
##########################################################################

	NULLCHK = 1	# treat nulls as terminators

	.text

	.globl	strcpy
	.globl	strncpy
	.globl	strncpyz
	.type	strcpy,@function
	.type	strncpy,@function
	.type	strncpyz,@function

	.p2align 4
strcpy:						# no length limit given
	subq	$31,  9,$18			#   make up an huge length
	lda	$27,strncpy-strcpy($27)		#   set up base register
	br	$31,strncpy
	nop
strncpyz:					# guarantees a null terminator
	addq	$16,$18,$19			#   point past end of buffer
	subq	$18,  1,$18			#   decrement length for copy operation
	stb	$31,-1($19)			#   put null terminator on end
	lda	$27,strncpy-strncpyz($27)	#   set up base register
strncpy:					# doesn't guarantee a null terminator
	.include "oz_crtl_copy_axp.s"

