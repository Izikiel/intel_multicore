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

#ifndef _OZ_SYS_VXPRINTF_H
#define _OZ_SYS_VXPRINTF_H

#define putint  oz_sys_vxprintf_putint
#define putsign oz_sys_vxprintf_putsign
#define putfc   oz_sys_vxprintf_putfc
#define putst   oz_sys_vxprintf_putst
#define putch   oz_sys_vxprintf_putch

/* Parameter block passed to various 'put' routines */

typedef struct { int minwidth;		/* mininum output field width */
                 int precision;		/* number of decimal places to output */
                 int negative;		/* number being output requires a '-' sign */
                 int altform;		/* output it in alternative format */
                 int zeropad;		/* right justify, zero fill */
                 int leftjust;		/* left justify, blank fill */
                 int posblank;		/* if positive, output a space for the sign */
                 int plussign;		/* if positive, output a '+' for the sign */
                 int intsize;		/* integer size: 0=int, 1=Byte, 2=Word, 4=Long, 8=Quad */
                 int longdoub;		/* floating point is an long double */

                 char *ncp;		/* start of integer string to output */
                 char *nce;		/* end of integer string to output */

                 char *ob;		/* start of output buffer */
                 char *oe;		/* end of output buffer */
                 char *op;		/* next available byte in ob */

                 uLong numout;		/* total number of characters output so far */
                 uLong (*entry) (void *param, uLong *size, char **buff); /* output callback routine */
                 void *param;		/* output callback routine parameter */                 
               } Par;

/* 'Put' routines output common stuff to the output buffer */

#define PUTLEFTFILL(__n) do { if (!(p -> leftjust) && !(p -> zeropad)) PUTFC (p -> minwidth - (__n), ' '); } while (0)
#define PUTLEFTZERO(__n) do { if (!(p -> leftjust) &&  (p -> zeropad)) PUTFC (p -> minwidth - (__n), '0'); } while (0)
#define PUTRIGHTFILL     PUTFC (p -> minwidth, ' ')

#define PUTINT(__alt)  do { sts = putint (p, __alt);   if (sts != OZ_SUCCESS) return (sts); } while (0)
#define PUTSIGN        do { sts = putsign (p);         if (sts != OZ_SUCCESS) return (sts); } while (0)
#define PUTFC(__s,__c) do { sts = putfc (p, __s, __c); if (sts != OZ_SUCCESS) return (sts); } while (0)
#define PUTST(__s,__b) do { sts = putst (p, __s, __b); if (sts != OZ_SUCCESS) return (sts); } while (0)
#define PUTCH(__c)     do { sts = putch (p, __c);      if (sts != OZ_SUCCESS) return (sts); } while (0)

/* Get short/long/int into '(un)signednum', put the sign in 'negative' */

#define GETSIGNEDNUM \
	     if (p -> intsize == 1) signednum = va_arg (ap, Byte);	\
	else if (p -> intsize == 2) signednum = va_arg (ap, Word);	\
	else if (p -> intsize == 4) signednum = va_arg (ap, Long);	\
	else if (p -> intsize == 8) signednum = va_arg (ap, Quad);	\
	else signednum = va_arg (ap, int);				\
	p -> negative = (signednum < 0);				\
	unsignednum = signednum;					\
	if (p -> negative) unsignednum = - signednum;

/* Get unsigned short/long/int into 'unsignednum' */

#define GETUNSIGNEDNUM \
	     if (p -> intsize == 1) unsignednum = va_arg (ap, uByte);	\
	else if (p -> intsize == 2) unsignednum = va_arg (ap, uWord);	\
	else if (p -> intsize == 4) unsignednum = va_arg (ap, uLong);	\
	else if (p -> intsize == 8) unsignednum = va_arg (ap, uQuad);	\
	else unsignednum = va_arg (ap, unsigned int);			\
	p -> negative = 0;

/* Get floating point into floatnum and exponent:  floatnum normalised in range 1.0 (inclusive) to 10.0 (exclusive) such that original_value = floatnum * 10 ** exponent */

#define GETFLOATNUM \
	if (p -> longdoub) floatnum = va_arg (ap, long double);	\
	else floatnum = va_arg (ap, double);			\
	p -> negative = (floatnum < 0.0);			\
	if (p -> negative) floatnum = - floatnum;		\
	exponent = 0;						\
	if (floatnum > 0.0) {					\
	  while (floatnum < 1.0) {				\
	    floatnum *= 10.0;					\
	    exponent --;					\
	  }							\
	  while (floatnum >= 10.0) {				\
	    floatnum /= 10.0;					\
	    exponent ++;					\
	  }							\
	}

/* Round normalised floating point number 'floatnum' to the given number of digits of precision following the decimal point */

#define ROUNDFLOAT(__prec) \
	roundfact = 0.5;				\
	for (i = __prec; -- i >= 0;) roundfact /= 10.0;	\
	floatnum += roundfact;				\
	if (floatnum >= 10.0) {				\
	  floatnum = 1.0;				\
	  exponent ++;					\
	}

/* Get integer in format string.  If *, use next integer argument, otherwise use decimal string */

#define GETFMTINT(__var) \
	if (*fp == '*') {				\
	  fp ++;					\
	  __var = va_arg (ap, int);			\
	} else {					\
	  __var = 0;					\
	  while (((fc = *fp) >= '0') && (fc <= '9')) {	\
	    __var *= 10;				\
	    fp ++;					\
	    __var += fc - '0';				\
	  }						\
	}

uLong oz_sys_vxprintf_fp (char fc, Par *p, va_list ap, va_list *ap_r);
uLong putint  (Par *p, char *alt);
uLong putsign (Par *p);
uLong putfc   (Par *p, int s, char c);
uLong putst   (Par *p, int s, const char *b);
uLong putch   (Par *p, char c);

#endif
