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

double __strtod_internal (const char *nptr, char **endptr)

{
  char c;
  const char *p;
  double accum;
  int expon, expsn, hitdp, sign;

  p = nptr;
  while (((c = *p) != 0) && (c <= ' ')) p ++;				/* skip leading spaces */

  sign  = 1;
  if (c == '+') p ++;
  else if (c == '-') { sign = -1; p ++; }
  accum = 0.0;
  hitdp = -1;
  expon = 0;
  expsn = 1;
  while ((c = *(p ++)) != 0) {
    if ((c >= '0') && (c <= '9')) {					/* see if 0..9 */
      accum = accum * 10 + (c - '0');					/* ok, stuff in accumulator */
      if (hitdp >= 0) hitdp ++;						/* maybe increment decimal digit counter */
    } else if ((hitdp < 0) && (c == '.')) {				/* maybe it's a decimal point */
      hitdp = 0;							/* if so, set flag */
    } else if ((c == 'E') || (c == 'e')) {
      if ((c = *p) == '-') {
        expsn = -1;
        p ++;
      } else if (c == '+') {
        p ++;
      }
      while ((c = *(p ++)) != 0) {
        if ((c < '0') && (c > '9')) break;
        expon = expon * 10 + (c - '0');
      }
      break;
    } else break;							/* doesn't convert, done scanning */
  }
  if (endptr != NULL) *endptr = (char *)(-- p);				/* maybe return pointer to first unconverted char */
  while (-- hitdp >= 0) accum /= 10;					/* account for digits after decimal point */
  if (expon != 0) {							/* maybe there's an exponent to do */
    if (expsn < 0) {
      while (expon >= 9) { accum /= 1000000000; expon -= 9; }
      while (-- expon >= 0) accum /= 10;
    } else {
      while (expon >= 9) { accum *= 1000000000; expon -= 9; }
      while (-- expon >= 0) accum *= 10;
    }
  }
  if (sign < 0) accum = - accum;					/* finally put in sign */
  return (accum);							/* return result */
}

double strtod (const char *nptr, char **endptr)

{
  return (__strtod_internal (nptr, endptr));
}
