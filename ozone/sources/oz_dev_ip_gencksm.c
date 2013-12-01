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

#include "ozone.h"
#include "oz_io_ip.h"

/************************************************************************/
/*									*/
/*  Generate IP-style checksum for a list of network byte order words	*/
/*  Crude but effective							*/
/*									*/
/************************************************************************/

uWord oz_dev_ip_gencksm (uLong nwords, const void *words, uWord start)

{
  const uByte *r2;
  uLong r0, r1;

  r0 = 0xffff & ~ start;		/* get one's comp of start value */
  r2 = words;				/* point to array of words in network byte order */
  for (r1 = nwords; r1 != 0; -- r1) {	/* repeat as long as there is more to do */
    r0 += *(r2 ++) << 8;		/* add in high order byte */
    r0 += *(r2 ++);			/* add in low order byte */
  }
  while ((r1 = r0 >> 16) != 0) {	/* get end-around carries */
    r0 = (r0 & 0xffff) + r1;		/* add them back around */
  }					/* should only happen a total of up to 2 times */
  r0 = 0xffff & ~ r0;			/* get one's comp of result */
  if (r0 == 0) r0 = 0xffff;		/* if zero, return 0xffff (neg zero) */
  return (r0);
}
