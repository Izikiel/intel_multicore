//+++2001-10-06
//    Copyright (C) 2001, Mike Rieker, Beverly, MA USA
//
//    This program is free software; you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation; version 2 of the License.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with this program; if not, write to the Free Software
//    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//---2001-10-06

/************************************************************************/
/*									*/
/*  Print out an instruction						*/
/*									*/
/*    Input:								*/
/*									*/
/*	il = max length of bytes pointed to by ob			*/
/*	ib = pointer to instruction buffer that actually holds the bytes*/
/*	pc = address of this instruction, it will be used to print the 	*/
/*	     target address if this is a relative jump or call		*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_print_insn_x86 = number of bytes for instruction		*/
/*	instruction printed via oz_knl_printk				*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_knl_printk.h"
#include "oz_sys_disassemble.h"

int oz_knl_printinstr (int il, const uByte *ib, const uByte *pc)

{
  char outbuf[128];
  int rc;

  rc = oz_sys_disassemble (il, ib, pc, sizeof outbuf, outbuf, NULL);
  oz_knl_printk ("%s", outbuf);
  return (rc);
}
