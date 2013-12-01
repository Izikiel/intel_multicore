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
/*  This module implements the IDEA (International Data Encryption 	*/
/*  Algorithm)								*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_knl_hw.h"
#include "oz_sys_cipher.h"

#define ROUNDS 8			/* don't change this value, should be 8 */
#define KEYLEN (6*ROUNDS+4)		/* length of key schedule */

const int oz_sys_cipher_ctxsize = KEYLEN * sizeof (uWord);

#define low16(x) ((x) & 0xffff)		/* get low-order 16 bits */

#ifdef _GNUC_
/* __const__ simply means there are not side effects for this function, */
/* which is useful info for the gcc compiler */
#define CONST __const__
#elif !defined (CONST)
#define CONST
#endif

#if defined (OSF1) || defined (AIX) || defined (SOL) || defined (SUN) || defined (DEC)
#define AVOID_JUMPS
#endif

/************************************************************************/
/*									*/
/*  Multiply two numbers, modulo 0x10001.  Requires two temps, 		*/
/*  t16 and t32.  x must be a side-effect-free lvalue.  y may be 	*/
/*  anything, but unlike x, must be strictly 16 bits even if low16 ()	*/
/*  is #defined.							*/
/*									*/
/*  All of these are equivalent - see which is faster on your machine	*/
/*									*/
/************************************************************************/

#ifdef SMALL_CACHE

#ifdef VAX
#pragma inline (mul)
#endif

#define MUL(x,y) (x = mul (low16 (x), y))

CONST static uWord mul (register uWord a, register uWord b)

{
  register uLong p;

  if (a) {
    if (b) {
      p = (uLong) a * b;
      b = low16 (p);
      a = p >> 16;
      return (b - a + (b < a));
    }
    else return (1 - a);
  }
  else return (1 - b);
}

#elif defined (AVOID_JUMPS)
#define MUL(x,y) (x   = low16 (x - 1), \
                  t16 = low16 ((y) - 1), \
                  t32 = (uLong) x * t16 + x + t16 + 1, \
                  x   = low16 (t32), \
                  t16 = t32 >> 16, \
                  x   = x - t16 + (x < t16))
#else
#define MUL(x,y) ((t16 = (y)) ? (x = low16 (x)) ? \
                  t32 = (uLong) x * t16, x = low16 (t32), t16 = t32 >> 16, \
                  x = x - t16 + (x < t16) : (x = 1 - t16) : (x = 1 - x))
#endif

/************************************************************************/
/*									*/
/*  Compute multiplicative inverse of x, modulo (2**16) + 1		*/
/*  using Euclid's GCD algorithm.  It is unrolled twice to 		*/
/*  avoid swapping the meaning of the registers each iteration, 	*/
/*  and some subtractions of t have been changed to adds.		*/
/*									*/
/************************************************************************/

CONST static uWord inv (uWord v)

{
  uWord q, x, y;
  uWord t0, t1;

  x = v;				/* 0 and 1 are self-inverse */
  if (x <= 1) return (x);

  t1 = 0x10001 / x;			/* since 2 <= x <= 65535, ... */
  y  = 0x10001 % x;			/* these both fit into 16 bits */

  t0 = 1;
  while (y != 1) {
    q = x / y;
    x = x % y;
    t0 += q * t1;
    if (x == 1) return (t0);
    q = y / x;
    y = y % x;
    t1 += q * t0;
  }

  return (1 - t1);
}

/************************************************************************/
/*									*/
/*  Compute IDEA encryption subkeys					*/
/*									*/
/*    Input:								*/
/*									*/
/*	uk  = pointer to unsigned byte array containing 128-bit key	*/
/*	ekv = malloc (oz_sys_cipher_ctxsize ())				*/
/*									*/
/*    Output:								*/
/*									*/
/*	*ekv = filled in with encryption key				*/
/*									*/
/************************************************************************/

void oz_sys_cipher_encinit (uByte uk[16], void *ekv)

{
  int i, j;
  uWord *ek;

  ek = ekv;

  ek[0]  = ((uWord) *(uk ++)) << 8;
  ek[1]  = ((uWord) *(uk ++)) << 8;
  ek[2]  = ((uWord) *(uk ++)) << 8;
  ek[3]  = ((uWord) *(uk ++)) << 8;
  ek[4]  = ((uWord) *(uk ++)) << 8;
  ek[5]  = ((uWord) *(uk ++)) << 8;
  ek[6]  = ((uWord) *(uk ++)) << 8;
  ek[7]  = ((uWord) *(uk ++)) << 8;

  ek[0] |= *(uk ++);
  ek[1] |= *(uk ++);
  ek[2] |= *(uk ++);
  ek[3] |= *(uk ++);
  ek[4] |= *(uk ++);
  ek[5] |= *(uk ++);
  ek[6] |= *(uk ++);
  ek[7] |= *(uk ++);

  i = 0;
  for (j = 8; j < KEYLEN; j ++) {
    i ++;
    ek[i+7] = ek[i&7] << 9 | ek[i+1&7] >> 7;
    ek += i & 8;
    i &= 7;
  }
}

/************************************************************************/
/*									*/
/*  Compute IDEA decryption subkeys					*/
/*									*/
/*    Input:								*/
/*									*/
/*	uk  = pointer to unsigned byte array containing 128-bit key	*/
/*	dkv = malloc (oz_sys_cipher_ctxsize ())				*/
/*									*/
/*    Output:								*/
/*									*/
/*	*dkv = filled in with decryption key				*/
/*									*/
/************************************************************************/

void oz_sys_cipher_decinit (uByte uk[16], void *dkv)

{
  int j;
  uLong t1, t2, t3;
  uWord *dk, *ek, *p;
  uWord eka[KEYLEN];

  oz_sys_cipher_encinit (uk, eka);
  ek = eka;

  dk = dkv;

  p = dk + KEYLEN;

  t1 = inv (*(ek ++));
  t2 = -(*(ek ++));
  t3 = -(*(ek ++));
  *(-- p) = inv (*(ek ++));
  *(-- p) = t3;
  *(-- p) = t2;
  *(-- p) = t1;

  for (j = 1; j < ROUNDS; j ++) {
    t1 = *(ek ++);
    *(-- p) = *(ek ++);
    *(-- p) = t1;

    t1 = inv (*(ek ++));
    t2 = -(*(ek ++));
    t3 = -(*(ek ++));
    *(-- p) = inv (*(ek ++));
    *(-- p) = t2;
    *(-- p) = t3;
    *(-- p) = t1;
  }

  t1 = *(ek ++);
  *(-- p) = *(ek ++);
  *(-- p) = t1;

  t1 = inv (*(ek ++));
  t2 = -(*(ek ++));
  t3 = -(*(ek ++));
  *(-- p) = inv (*(ek ++));
  *(-- p) = t3;
  *(-- p) = t2;
  *(-- p) = t1;
}

/************************************************************************/
/*									*/
/*  IDEA encryption / decryption algorithm				*/
/*									*/
/*    Input:								*/
/*									*/
/*	ekv  = pointer to encryption sub-keys				*/
/*	len  = number of unsigned bytes (must be multiple of 8)		*/
/*	*in  = input data to be encrypted or decrypted			*/
/*									*/
/*    Output:								*/
/*									*/
/*	*out = output data						*/
/*									*/
/*    Note that in and out can be the same buffer			*/
/*									*/
/************************************************************************/

void oz_sys_cipher_encrypt (void *ekv, uByte feedback[8], int len, void *in, void *out)

{
  register uLong x1, x2, x3, x4, t1, t2;
  register uLong t16, t32;
  register uWord *z;
  uByte *b, *ii, *oo, isav[8], tempfeedback[8];
  int i, r;

  i  = len / 8;
  ii = in;
  oo = out;

  if (feedback == NULL) {
    feedback = tempfeedback;
    memset (tempfeedback, 0, sizeof tempfeedback);
  }

  while (-- i >= 0) {
    b = feedback;

    x1 = *(ii ++) ^ *(b ++);
    x2 = *(ii ++) ^ *(b ++);
    x3 = *(ii ++) ^ *(b ++);
    x4 = *(ii ++) ^ *(b ++);

    x1 |= ((uLong) (*(ii ++) ^ *(b ++))) << 8;
    x2 |= ((uLong) (*(ii ++) ^ *(b ++))) << 8;
    x3 |= ((uLong) (*(ii ++) ^ *(b ++))) << 8;
    x4 |= ((uLong) (*(ii ++) ^ *(b ++))) << 8;

    z = ekv;

    for (r = ROUNDS; r > 0; -- r) {
      MUL (x1, *(z ++));
      x2 += *(z ++);
      x3 += *(z ++);
      MUL (x4, *(z ++));

      t2 = x1 ^ x3;
      MUL (t2, *(z ++));
      t1 = t2 + (x2 ^ x4);
      MUL (t1, *(z ++));
      t2 = t1 + t2;

      x1 ^= t1;
      x4 ^= t2;

      t2 ^= x2;
      x2  = x3 ^ t1;
      x3  = t2;
    }

    MUL (x1, *(z ++));
    x3 += *(z ++);
    x2 += *(z ++);
    MUL (x4, *z);

    b = feedback;
    *(oo ++) = *(b ++) = x1;
    *(oo ++) = *(b ++) = x3;
    *(oo ++) = *(b ++) = x2;
    *(oo ++) = *(b ++) = x4;

    *(oo ++) = *(b ++) = x1 >> 8;
    *(oo ++) = *(b ++) = x3 >> 8;
    *(oo ++) = *(b ++) = x2 >> 8;
    *(oo ++) = *(b ++) = x4 >> 8;
  }
}

/************************************************************************/
/*									*/
/*  IDEA encryption / decryption algorithm				*/
/*									*/
/*    Input:								*/
/*									*/
/*	dkv  = pointer to decryption sub-keys				*/
/*	len  = number of unsigned bytes (must be multiple of 8)		*/
/*	*in  = input data to be encrypted or decrypted			*/
/*									*/
/*    Output:								*/
/*									*/
/*	*out = output data						*/
/*									*/
/*  Note that in and out can be the same buffer				*/
/*									*/
/************************************************************************/

void oz_sys_cipher_decrypt (void *dkv, uByte feedback[8], int len, void *in, void *out)

{
  register uLong x1, x2, x3, x4, t1, t2;
  register uLong t16, t32;
  register uWord *z;
  uByte *b, *ii, isav[8], *oo, tempfeedback[8];
  int i, r;

  i  = len / 8;
  ii = in;
  oo = out;

  if (feedback == NULL) {
    feedback = tempfeedback;
    memset (tempfeedback, 0, sizeof tempfeedback);
  }

  while (-- i >= 0) {
    memcpy (isav, ii, 8);

    x1 = *(ii ++);
    x2 = *(ii ++);
    x3 = *(ii ++);
    x4 = *(ii ++);

    x1 |= ((uLong) *(ii ++)) << 8;
    x2 |= ((uLong) *(ii ++)) << 8;
    x3 |= ((uLong) *(ii ++)) << 8;
    x4 |= ((uLong) *(ii ++)) << 8;

    z = dkv;

    for (r = ROUNDS; r > 0; -- r) {
      MUL (x1, *(z ++));
      x2 += *(z ++);
      x3 += *(z ++);
      MUL (x4, *(z ++));

      t2 = x1 ^ x3;
      MUL (t2, *(z ++));
      t1 = t2 + (x2 ^ x4);
      MUL (t1, *(z ++));
      t2 = t1 + t2;

      x1 ^= t1;
      x4 ^= t2;

      t2 ^= x2;
      x2  = x3 ^ t1;
      x3  = t2;
    }

    MUL (x1, *(z ++));
    x3 += *(z ++);
    x2 += *(z ++);
    MUL (x4, *z);

    b = feedback;
    *(oo ++) = *(b ++) ^ x1;
    *(oo ++) = *(b ++) ^ x3;
    *(oo ++) = *(b ++) ^ x2;
    *(oo ++) = *(b ++) ^ x4;

    *(oo ++) = *(b ++) ^ (x1 >> 8);
    *(oo ++) = *(b ++) ^ (x3 >> 8);
    *(oo ++) = *(b ++) ^ (x2 >> 8);
    *(oo ++) = *(b ++) ^ (x4 >> 8);

    memcpy (b - 8, isav, 8);
  }
}
