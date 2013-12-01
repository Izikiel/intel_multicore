//+++2003-03-01
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
//---2003-03-01

/************************************************************************/
/*									*/
/*  This routine generates a one-way hash value given an arbitrary 	*/
/*  string								*/
/*									*/
/************************************************************************/

#include <string.h>

#include "ozone.h"
#include "oz_sys_hash.h"

typedef struct { uLong d[5];		/* current hashing data */
                 uLong count;		/* total bytecount so far */
                 uLong ll;		/* length in 'lb' */
                 uByte lb[64];		/* leftover buffer */
               } Hctx;

/* The SHS f()-functions */

#define F1(x,y,z) ((x & y) | (~x & z))		/* rounds  0-19 */
#define F2(x,y,z) (x ^ y ^ z)			/* rounds 20-39 */
#define F3(x,y,z) ((x & y) | (x & z) | (y & z))	/* rounds 40-59 */
#define F4(x,y,z) (x ^ y ^ z)			/* rounds 60-79 */

/* The SHS Mysterious constants */

#define K1 0x5a827999
#define K2 0x6ed9eba1
#define K3 0x8f1bbcdc
#define K4 0xca62c1d6

/* SHS initial values */

#define H0INIT 0x67452301
#define H1INIT 0xefcdab89
#define H2INIT 0x98badcfe
#define H3INIT 0x10325476
#define H4INIT 0xc3d2e1f0

/* 32-bit rotate - kludged with shifts */

#define ROT32(n,x) ((x << n) | (x >> (32 - n)))

/************************************************************************/
/*									*/
/*  Internal routine to perofrm the SHS transformation			*/
/*									*/
/************************************************************************/

static void transform (const uByte buff[64], uLong digest[5])

{
  const uByte *ip;
  uLong a, b, c, d, e, t, w[80];
  int i;

  /* Make 16 longs out of 64 bytes (endian-independent) */

  ip = buff;
  for (i = 0; i < 16; i ++) {
    w[i] = *(ip ++);
    w[i] = (w[i] << 8) + *(ip ++);
    w[i] = (w[i] << 8) + *(ip ++);
    w[i] = (w[i] << 8) + *(ip ++);
  }

  /* Expand the 16 longs into 64 more longs */

  for (; i < 80; i ++) w[i] = w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16];

  /* Set up first buffer */

  a = digest[0];
  b = digest[1];
  c = digest[2];
  d = digest[3];
  e = digest[4];

  /* Serious mangling, divided into four sub-rounds */

  for (i = 0; i < 20; i ++) {
    t = ROT32 (5, a) + F1 (b, c, d) + e + w[i] + K1;
    e = d; d = c; c = ROT32 (30, b); b = a; a = t;
  }

  for (; i < 40; i ++) {
    t = ROT32 (5, a) + F2 (b, c, d) + e + w[i] + K2;
    e = d; d = c; c = ROT32 (30, b); b = a; a = t;
  }

  for (; i < 60; i ++) {
    t = ROT32 (5, a) + F3 (b, c, d) + e + w[i] + K3;
    e = d; d = c; c = ROT32 (30, b); b = a; a = t;
  }

  for (; i < 80; i ++) {
    t = ROT32 (5, a) + F4 (b, c, d) + e + w[i] + K4;
    e = d; d = c; c = ROT32 (30, b); b = a; a = t;
  }

  /* Build message digest */

  digest[0] += a;
  digest[1] += b;
  digest[2] += c;
  digest[3] += d;
  digest[4] += e;
}

/************************************************************************/
/*									*/
/*  This is the global routine to do the hashing			*/
/*									*/
/*    Input:								*/
/*									*/
/*	count   = number of input bytes					*/
/*	*buffer = input bytes						*/
/*									*/
/*    Output:								*/
/*									*/
/*	*digest = output bytes						*/
/*									*/
/************************************************************************/

void oz_sys_hash_init (void *hctxv)

{
  Hctx *hctx;

  if (OZ_SYS_HASH_CTXSIZE < sizeof *hctx) oz_crash ("oz_sys_hash_init: these routines require OZ SYS HASH CTXSIZE >= %d", sizeof *hctx);

  hctx = hctxv;

  /* Set the digest vars to their initial values */

  hctx -> d[0] = H0INIT;
  hctx -> d[1] = H1INIT;
  hctx -> d[2] = H2INIT;
  hctx -> d[3] = H3INIT;
  hctx -> d[4] = H4INIT;

  /* No "left over" data */

  hctx -> ll    = 0;
  hctx -> count = 0;
}

void oz_sys_hash_data (void *hctxv, int count, const void *buffer)

{
  const uByte *ip;
  Hctx *hctx;
  uLong i, j;

  hctx = hctxv;
  i    = count;
  ip   = buffer;

  hctx -> count += count;

  /* If less than 64 to do, just put in leftover buffer and do it later */

  if (hctx -> ll + i < 64) {
    memcpy (hctx -> lb + hctx -> ll, ip, i);
    hctx -> ll += i;
    return;
  }

  /* Process any left-over data with enough input to fill to 64 bytes */

  if (hctx -> ll != 0) {
    j = 64 - hctx -> ll;
    memcpy (hctx -> lb + hctx -> ll, ip, j);
    transform (hctx -> lb, hctx -> d);
    i  -= j;
    ip += j;
  }

  /* Process remaining input data in 64 byte chunks */

  while (i >= 64) {
    transform (ip, hctx -> d);
    i  -= 64;
    ip += 64;
  }

  /* Put the rest of the input in the leftover buffer, if any */

  hctx -> ll = i;
  memcpy (hctx -> lb, ip, i);
}

void oz_sys_hash_term (void *hctxv, uByte digest[16])

{
  uByte *op;
  Hctx *hctx;
  uLong i;

  if (OZ_SYS_HASH_BINSIZE < 16) oz_crash ("oz_sys_hash_init: these routines require OZ SYS HASH BINSIZE >= 16");

  hctx = hctxv;
  i = hctx -> ll;

  /* Set the first char of padding to 0x80.  This is    */
  /* safe since there is always at least one byte free. */

  hctx -> lb[i++] = 0x80;

  /* If block has more than 60, pad with zeroes and process it */

  if (i > 60) {
    memset (hctx -> lb + i, 0, 64 - i);
    transform (hctx -> lb, hctx -> d);
    i = 0;
  }

  /* Pad block with zeroes to 60 bytes */

  memset (hctx -> lb + i, 0, 60 - i);

  /* Append length and process */

  hctx -> lb[60] = (uByte)(hctx -> count >> 24);
  hctx -> lb[61] = (uByte)(hctx -> count >> 16);
  hctx -> lb[62] = (uByte)(hctx -> count >>  8);
  hctx -> lb[63] = (uByte)(hctx -> count);

  transform (hctx -> lb, hctx -> d);

  /* Change 160-bit key to 128-bits */

  i = hctx -> d[4];
  hctx -> d[0] += i & K1;
  hctx -> d[1] += i & K2;
  hctx -> d[2] += i & K3;
  hctx -> d[3] += i & K4;

  /* Copy to output (endian-independent) */

  op = digest;
  for (i = 0; i < 4; i ++) {
    *(op ++) = (uByte)(hctx -> d[i] >> 24);
    *(op ++) = (uByte)(hctx -> d[i] >> 16);
    *(op ++) = (uByte)(hctx -> d[i] >>  8);
    *(op ++) = (uByte)(hctx -> d[i]);
  }
}

void oz_sys_hash (int count, const void *buffer, uByte digest[16])

{
  Hctx hctx;

  oz_sys_hash_init (&hctx);
  oz_sys_hash_data (&hctx, count, buffer);
  oz_sys_hash_term (&hctx, digest);
}

/************************************************************************/
/*									*/
/*  These routines either generate a printable hash string from a 	*/
/*  binary hash or decode a binary hash from a printable string		*/
/*									*/
/************************************************************************/

static const char trans32[33] = "ab0cd1ef2gh3jk4mn5pr6st7uv8wx9yz";

#if OZ_SYS_HASH_BINSIZE != 16
  error : these routines require OZ SYS HASH BINSIZE == 16
#endif

#if OZ_SYS_HASH_STRSIZE != 36
  error : these routines require OZ SYS HASH STRSIZE == 36
#endif

/************************************************************************/
/*									*/
/*  Convert a binary hash buffer to string				*/
/*									*/
/*    Input:								*/
/*									*/
/*	hash = binary hash buffer					*/
/*	buff = address of string buffer					*/
/*									*/
/*   Output:								*/
/*									*/
/*	*buff = filled in with null-terminated string			*/
/*									*/
/************************************************************************/

void oz_sys_hash_bin2str (uByte hash[16], char buff[36])

{
  char *b;
  unsigned char hashbin[16+4];
  int i, j;
  unsigned int longy;

  j = 0;					/* clear checksum */
  for (i = 0; i < 16; i ++) {			/* copy bytes and checksum them */
    hashbin[i] = hash[i];
    j += hash[i];
  }
  hashbin[i++] = j >> 8;			/* store checksum & zero pad */
  hashbin[i++] = j;
  hashbin[i++] = 0;
  hashbin[i++] = 0;

  b = buff;					/* point to output buffer */

  for (i = 0; i < 16 + 2; i += 3) {		/* take input 24 bits at a time */
    longy = hashbin[i] + (hashbin[i+1] << 8) + (hashbin[i+2] << 16);
    if (i != 0) *(b ++) = '-';			/* put in an hyphen */
    for (j = 0; j < 24; j += 5) {		/* output it 5 bits at a time */
      *(b ++) = trans32[longy&31];		/* ... resulting in 5 chars */
      longy >>= 5;
    }
  }

  *b = 0;					/* null terminate output */
}

/************************************************************************/
/*									*/
/*  Convert a string hash buffer to binary				*/
/*									*/
/*    Input:								*/
/*									*/
/*	buff = address of string buffer					*/
/*	hash = binary hash buffer					*/
/*									*/
/*   Output:								*/
/*									*/
/*	sc_str2hash = 1 : successful conversion				*/
/*	              0 : conversion error				*/
/*									*/
/************************************************************************/

#define lc(c) (((c >= 'A') && (c <= 'Z')) ? (c + 'a' - 'A') : c)

int oz_sys_hash_str2bin (const char buff[36], uByte hash[16])

{
  char c, hashstr[30];
  unsigned char hashbin[16+4];
  int i, j, k;
  unsigned int longy;

  /* Convert from ascii to 5-bit binary bytes */

  k = 0;
  for (i = 0; i < 35; i ++) {
    c = lc (buff[i]);
    if (i % 6 == 5) {
      if (c != '-') goto badkeygiven;
      continue;
    }
    for (j = 0; j < 32; j ++) if (trans32[j] == c) {
      hashstr[k++] = j;
      break;
    }
  }
  if (k != 30) goto badkeygiven;

  /* Convert from 5-bit binary bytes to 8 bit bytes */

  k = 0;
  for (i = 0; i < 16 + 2; i += 3) {
    longy = 0;
    for (j = 0; j < 24; j += 5) longy |= (unsigned int) (hashstr[k++]) << j;
    hashbin[i]   = longy;
    hashbin[i+1] = longy >>  8;
    hashbin[i+2] = longy >> 16;
  }

  /* Make sure the checksum is valid (so they typed it in correctly) */

  k = 0;
  for (i = 0; i < 16; i ++) k += hashbin[i];
  j  = (unsigned int) (hashbin[i++]) << 8;
  j += hashbin[i];
  if (j != k) goto badkeygiven;

  memcpy (hash, hashbin, 16);
  return (1);

badkeygiven:
  return (0);
}
