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

/************************************************************************/
/*									*/
/*  Fill a buffer with random data					*/
/*									*/
/*    Input:								*/
/*									*/
/*	 size = number of bytes to generate				*/
/*	 buff = where to put random data				*/
/*									*/
/*************************************************************************/

#include "ozone.h"
#include "oz_knl_hw.h"
#include "oz_sys_hash.h"

static struct {
  uByte lasthash[16];		// last hashing result (feedback)
  uLong rpcc;			// value from current rpcc instruction
  int int_count;		// number of interrupts received
  uByte int_rpccbytes[16];	// rpcc bytes from last 16 interrupts
} seed;

/************************************************************************/
/*									*/
/*  Fill a buffer with random data					*/
/*									*/
/************************************************************************/

void oz_hw_random_fill (uLong size, void *buff)

{
  uByte temp[16];

  while (size >= 16) {
    seed.rpcc = OZ_HWAXP_RPCC ();
    oz_sys_hash (sizeof seed, &seed, buff);
    memcpy (seed.lasthash, buff, 16);
    size -= 16;
    buff += 16;
  }
  if (size > 0) {
    seed.rpcc = OZ_HWAXP_RPCC ();
    oz_sys_hash (sizeof seed, &seed, temp);
    memcpy (seed.lasthash, temp, 16);
    memcpy (buff, temp, size);
  }
}

/************************************************************************/
/*									*/
/*  This routine should be called at the beginning of an interrupt 	*/
/*  routine.  It stores the low byte of the rpcc (cpu cycle) counter 	*/
/*  to the next slot in the int_rpccbyte array.				*/
/*									*/
/************************************************************************/

void oz_hwaxp_random_int (void)

{
  seed.int_rpccbytes[(seed.int_count++)&15] = OZ_HWAXP_RPCC ();
}
