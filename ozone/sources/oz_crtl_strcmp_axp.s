##+++2003-11-26
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
##---2003-11-26

##########################################################################
##									##
##  Prefix file for oz_crtl_compare_axp to build 'strcmp' routine	##
##									##
##########################################################################

	IGNCASE = 0	# don't convert upper to lower case when comparing
	NULLCHK = 1	# treat nulls as terminators

	.text

	.globl	strcmp
	.globl	strncmp
	.type	strcmp,@function
	.type	strncmp,@function
	.p2align 4
strcmp:
	subq	$31,9,$18
	lda	$27,strncmp-strcmp($27)
strncmp:
	.include "oz_crtl_compare_axp.s"

