//+++2003-11-18
//    Copyright (C) 2001,2002,2003  Mike Rieker, Beverly, MA USA
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
//---2003-11-18

#include "ozone.h"
#include "oz_sys_disassemble.h"
#include "oz_sys_xprintf.h"

/************************************************************************/
/*									*/
/*  Disassemble an instruction						*/
/*									*/
/*    Input:								*/
/*									*/
/*	il = max length of bytes pointed to by ob			*/
/*	ib = pointer to instruction buffer that actually holds the bytes*/
/*	pc = address of this instruction, it will be used to print the 	*/
/*	     target address if this is a relative jump or call		*/
/*	ol = output buffer length					*/
/*	ob = output buffer address					*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_disassemble = number of bytes for instruction		*/
/*	*ob = filled in (null terminated string)			*/
/*	*into = -1 : steps into but clears Tracing flag			*/
/*	         0 : doesn't step into anything				*/
/*	         1 : steps into, leaves Tracing flag alone		*/
/*									*/
/************************************************************************/

int oz_sys_disassemble (int il, const uByte *ib, const uByte *pc, int ol, char *ob, int *into)

{
  uLong opcode;

  if (into != NULL) *into = 0;
  if (il < 4) {
    oz_sys_sprintf (ol, ob, "  length %u", il);
    return (il);
  }
  opcode = ib[0] + (ib[1] << 8) + (ib[2] << 16) + (ib[3] << 24);
  oz_sys_sprintf (ol, ob, "  inst %8.8LX", opcode);
  return (4);
}
