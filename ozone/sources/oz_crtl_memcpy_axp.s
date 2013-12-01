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
##  Prefix file for oz_crtl_copy_axp to build 'memcpy' routine		##
##									##
##########################################################################

	NULLCHK = 0	# don't treat nulls as terminators

	.text

	.globl	memcpy
	.globl	memmove
	.type	memcpy,@function
	.type	memmove,@function
	.p2align 4
memcpy:
memmove:
	.include "oz_crtl_copy_axp.s"

