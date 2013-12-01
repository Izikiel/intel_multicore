//+++2003-12-12
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
//---2003-12-12

/* Divide/Remainder table generator */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

typedef int Long;
typedef long long Quad;
typedef unsigned int uLong;
typedef unsigned long long uQuad;

#define NMULTBITS 10

#define TOPBIT 0x8000000000000000ULL
#define ARRAYSIZE ((1 << NMULTBITS) / 2)

static uQuad *multipliers;

static uQuad calcmult (int n);
static uQuad umulh (uQuad multiplicand, uQuad multiplier);

int main ()

{
  int n;
  uQuad m;

  for (n = 0; n < ARRAYSIZE; n ++) {
    m = calcmult (n);
    if ((n % 4) == 0) printf (" .quad  ");
    printf ("0x%16.16llX", m);
    if ((n % 4) == 3) printf ("\n");
                 else printf (",");
  }
  return (0);
}

/* Calculate (TOPBIT << NMULTBITS) / (n + 1 + ARRAYSIZE) */

/* Assume NMULTBITS = 8: */

/*  n    what it's for  table value          */
/* -1    div by  0x80  2.0000.0000.0000.0000 */
/* 0x3F  div by  0xC0  1.5555.5555.5555.5555 */
/* 0xFF  div by 0x100  1.0000.0000.0000.0000 */

/* calcmult(n) = ((1.0000.0000.0000.0000 << NMULTBITS) - 1) / (n + 1 + ARRAYSIZE)) */

static uQuad calcmult (int n)

{
  int i;
  uLong divisor;
  uQuad dividend, quotient;

  divisor  = n + 1 + ARRAYSIZE;		/* convert n=0x3F to d=0xC0, etc */

  dividend = (1 << NMULTBITS) - 1;
  quotient = 0;

  for (i = 2; -- i >= 0;) {
    dividend <<= 32;
    dividend  |= 0xFFFFFFFFULL;
    quotient <<= 32;
    quotient  += dividend / divisor;
    dividend  %= divisor;
  }

  return (quotient);
}

/* Compute (multiplicand*multiplier)>>64 */

static uQuad umulh (uQuad multiplicand, uQuad multiplier)

{
  int i;
  uQuad prodhi, prodlo;

  prodhi = 0;
  prodlo = 0;
  for (i = 64; -- i >= 0;) {
    prodhi += prodhi;
    if (prodlo & TOPBIT) prodhi ++;
    prodlo += prodlo;
    if (multiplier & TOPBIT) {
      prodlo += multiplicand;
      if (prodlo < multiplicand) prodhi ++;
    }
    multiplier += multiplier;
  }

  return (prodhi);
}
