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

# In the loader, oz_sys_pdata_array and oz_s_systempdata are the same
# because there is only one process, and oz_sys_pdata_array only 
# needs the kernel mode element because there is no user mode

	.data
	.align	4
	.globl	oz_s_systempdata
	.globl	oz_sys_pdata_array
	.type	oz_s_systempdata,@object
	.type	oz_sys_pdata_array,@object
oz_s_systempdata:
oz_sys_pdata_array:
	.space	4096

