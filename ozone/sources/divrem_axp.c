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

/* Divide/Remainder test program */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

typedef int Long;
typedef long long Quad;
typedef unsigned int uLong;
typedef unsigned long long uQuad;

static uLong loopcount = 0;

#define TOPBIT 0x8000000000000000ULL
#define ARRAYSIZE ((1 << nmultbits) / 2)

static int nmultbits;
static uQuad *multipliers;

static uQuad calcmult (int n);
static uQuad randuquad (void);

uQuad divqu (uQuad r24, uQuad r25);
uLong divlu (uLong r24, uLong r25);
uQuad remqu (uQuad r24, uQuad r25);
uLong remlu (uLong r24, uLong r25);
Quad  divq   (Quad r24,  Quad r25);
Long  divl   (Long r24,  Long r25);
Quad  remq   (Quad r24,  Quad r25);
Long  reml   (Long r24,  Long r25);

static uQuad umulh (uQuad multiplicand, uQuad multiplier);

int main (int argc, char *argv[])

{
  int w, n;
  Long la, lb, lc, ld;
  Quad qa, qb, qc, qd;
  uLong ula, ulb, ulc, uld;
  uQuad uqa, uqb, uqc, uqd;

  if (argc != 2) {
    printf ("divrem_axp <nmultbits>\n");
    return (-1);
  }
  nmultbits = atoi (argv[1]);
  if ((nmultbits < 2) || (nmultbits > 20)) {
    printf ("bad nmultbits %d\n", nmultbits);
    return (-1);
  }

  multipliers = malloc (ARRAYSIZE * sizeof *multipliers);
  for (n = 0; n < ARRAYSIZE; n ++) {
    multipliers[n] = calcmult (n);
    if ((n % 4) == 0) printf (" .quad  ");
    printf ("0x%16.16llX", multipliers[n]);
    if ((n % 4) == 3) printf ("\n");
                 else printf (",");
  }
  fflush (stdout);

  srand (1);
  n = 0;
loop:
  ++ n;					// inc test counter
  uqa = randuquad ();			// get a random quadword dividend
  uqb = randuquad ();			// get a random quadword divisor

  w = rand () % 64;			// get random number of bits for divisor (0..63)

  la = uqa;
  lb = uqb;
  qa = uqa;
  qb = uqb;
  ula = uqa;
  ulb = uqb;

  if (w < 63) {
    uqb &= (2ULL << w) - 1;		// mask unsigned quad divisor
    if (qb < 0) qb |= (-1LL << w);	// mask signed quad divisor
           else qb &=  (1LL << w) - 1;	// ... preserving random sign bit
  }
  w %= 32;
  if (w < 31) {
    ulb &= (2U << w) - 1;		// mask unsigned long divisor
    if (lb < 0) lb |= (-1 << w);	// mask signed long divisor
           else lb &=  (1 << w) - 1;	// ... preserving random sign bit
  }

  if (uqb != 0) {
    uqc = uqa / uqb;			// calc correct answer
    uqd = divqu (uqa, uqb);		// see what we get
    if (uqc != uqd) {
      printf ("%d: %llX divqu %llX sb %llX, got %llX\n", n, uqa, uqb, uqc, uqd);
      return (0);
    }
#if 000
    uqc = uqa % uqb;			// calc correct answer
    uqd = remqu (uqa, uqb);		// see what we get
    if (uqc != uqd) {
      printf ("%d: %llX remqu %llX sb %llX, got %llX\n", n, uqa, uqb, uqc, uqd);
      return (0);
    }
#endif
  }

#if 000
  if (qb != 0) {
    qc = qa / qb;			// calc correct answer
    qd = divq (qa, qb);			// see what we get
    if (qc != qd) {
      printf ("%d: %llX divq %llX sb %llX, got %llX\n", n, qa, qb, qc, qd);
      return (0);
    }
    qc = qa % qb;			// calc correct answer
    qd = remq (qa, qb);			// see what we get
    if (qc != qd) {
      printf ("%d: %llX remq %llX sb %llX, got %llX\n", n, qa, qb, qc, qd);
      return (0);
    }
  }

  if (ulb != 0) {
    ulc = ula / ulb;			// calc correct answer
    uld = divlu (ula, ulb);		// see what we get
    if (ulc != uld) {
      printf ("%d: %llX divlu %llX sb %llX, got %llX\n", n, ula, ulb, ulc, uld);
      return (0);
    }
    ulc = ula % ulb;			// calc correct answer
    uld = remlu (ula, ulb);		// see what we get
    if (ulc != uld) {
      printf ("%d: %llX remlu %llX sb %llX, got %llX\n", n, ula, ulb, ulc, uld);
      return (0);
    }
  }

  if (lb != 0) {
    lc = la / lb;			// calc correct answer
    ld = divl (la, lb);			// see what we get
    if (lc != ld) {
      printf ("%d: %X divl %X sb %X, got %X\n", n, la, lb, lc, ld);
      return (0);
    }
    lc = la % lb;			// calc correct answer
    ld = reml (la, lb);			// see what we get
    if (lc != ld) {
      printf ("%d: %X reml %X sb %X, got %X\n", n, la, lb, lc, ld);
      return (0);
    }
  }
#endif

  if ((n % 100000) == 0) printf ("%d  %d  %7.3f\n", n, loopcount, (float)loopcount / (float)n);
  goto loop;
}

/* Calculate (TOPBIT << nmultbits) / (n + 1 + ARRAYSIZE) */

static uQuad calcmult (int n)

{
  int i;
  uLong divisor;
  uQuad dividend, quotient;

  dividend = 1 << (nmultbits - 1);
  divisor  = n + 1 + ARRAYSIZE;
  quotient = 0;

  for (i = 2; -- i >= 0;) {
    dividend <<= 32;
    quotient <<= 32;
    quotient  += dividend / divisor;
    dividend  %= divisor;
  }

  return (quotient);
}

static uQuad randuquad (void)

{
  int i;
  union { unsigned char b[8];
          uQuad q;
        } u;

  for (i = 0; i < 8; i ++) u.b[i] = rand ();

  return (u.q);
}


uQuad divqu (uQuad dividend, uQuad divisor)

{
  int shift;
  uQuad guesstimate, multiplier, product, quotient;

  if (divisor == 0) abort ();
  shift = 63;						// get bit position of most significant '1' bit
  for (product = divisor; !(product & TOPBIT); product <<= 1) -- shift;
  if (product == TOPBIT) {				// special case if product is power of two
    return (dividend >> shift);				// ... just shift dividend
  }
  multiplier = multipliers[(product>>(64-nmultbits))-ARRAYSIZE]; // get multiplier based on top nmultbits of divisor
  quotient   = 0;					// reset quotient (we haven't sub'd anything from divident yet)
  while (dividend / 2 >= divisor) {			// see if we should get 2 or greater
    guesstimate   = umulh (dividend, multiplier);	// make guesstimate of the quotient (*should* never overestimate)
    guesstimate >>= shift;
    if (guesstimate == 0) guesstimate = 1;		// maybe we got zero, so get at least 1
    product = guesstimate * divisor;			// calculate product that guesstimate would give us
    if (product > dividend) abort ();			// we should never overestimate
    quotient += guesstimate;				// add estimate to quotient
    dividend -= product;				// subtract corresponding product from dividend
    loopcount ++;					// statistics
  }
  if (dividend >= divisor) {				// see if we have enough for one more
    quotient ++;					// ok, count it
    dividend -= divisor;				// ... and take it out
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
