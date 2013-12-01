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
/*  Floatingpoint conversions for oz_sys_vxprintf			*/
/*									*/
/*  In a different module so oz_sys_xprintf.c can be compiled without 	*/
/*  references to floating point data					*/
/*									*/
/************************************************************************/

#include <stdarg.h>
#include "ozone.h"
#include "oz_knl_status.h"
#include "oz_sys_vxprintf.h"

uLong oz_sys_vxprintf_fp (char fc, Par *p, va_list ap, va_list *ap_r)

{
  int exponent, i, numsize;
  long double floatnum, roundfact;
  long long int signednum;
  uLong sts;

  switch (fc) {
    case 'G':
    case 'g': {					/* - either 'e'/'E' or 'f', depending on value */
      GETFLOATNUM;							/* get floating point number to be converted */
      if (p -> precision < 0) p -> precision = 6;			/* fix up the given precision */
      if (p -> precision == 0) p -> precision = 1;
      ROUNDFLOAT (p -> precision - 1);					/* round to given precision */
      if ((exponent < -4) || (exponent >= p -> precision)) {		/* use 'e'/'E' format if exponent < -4 or >= precision */
        -- (p -> precision);
        fc -= 'g' - 'e';
        goto format_e;
      }
      p -> precision -= exponent + 1;					/* otherwise, use 'f' format */
      goto format_f;
    }

    case 'E':
    case 'e': {					/* - floating point exponent notation */
      GETFLOATNUM;							/* get floating point number to be converted */
      if (p -> precision < 0) p -> precision = 6;			/* default of 6 places after decimal point */
      ROUNDFLOAT (p -> precision);					/* round to given precision */
format_e:
      numsize = p -> precision;						/* size of number = number of digits after decimal point */
      if ((numsize > 0) || p -> altform) numsize ++;			/*                + maybe it includes the decimal point */
      numsize ++;							/*                + always includes digit before decimal point */
      if (p -> negative || p -> plussign || p -> posblank) numsize ++;	/*               + maybe it includes room for the sign */
      numsize += 3;							/*                + e/E and two exponent digits */
      if ((exponent < 0) || p -> plussign || p -> posblank) numsize ++;	/*              + maybe it includes room for exponent sign */
      PUTLEFTFILL (numsize);						/* output leading space padding */
      PUTSIGN;								/* output the sign character, if any */
      PUTLEFTZERO (numsize);						/* output leading zero padding */
      i = floatnum;							/* get single digit in front of decimal point */
      PUTCH (i + '0');							/* output it */
      if (p -> altform || (p -> precision > 0)) PUTCH ('.');		/* decimal point if altform or any digits after decimal pt */
      while (p -> precision > 0) {					/* keep cranking out digits as long as precision says to */
        floatnum -= i;
        floatnum *= 10.0;
        -- (p -> precision);
        i = floatnum;
        PUTCH (i + '0');
      }
      PUTCH (fc);							/* output the 'e' or 'E' */
      p -> negative = (exponent < 0);					/* get exponent sign */
      if (p -> negative) exponent = - exponent;				/* make exponent positive */
      PUTSIGN;								/* output the exponent sign character, if any */
      PUTCH (((exponent / 10) % 10) + '0');				/* output exponent tens digit */
      PUTCH ((exponent % 10) + '0');					/* output exponent units digit */
      PUTRIGHTFILL;							/* pad on right with spaces */
      break;
    }

    case 'f': {					/* - floating point standard notation */
      GETFLOATNUM;							/* get floating point number to be converted */
      if (p -> precision < 0) p -> precision = 6;			/* default of 6 places after decimal point */
      ROUNDFLOAT (p -> precision + exponent);				/* round to given precision */
format_f:
      numsize = p -> precision;						/* size of number = number of digits after decimal point */
      if ((numsize > 0) || p -> altform) numsize ++;			/*                + maybe it includes the decimal point */
      numsize ++;							/*                + always includes digit before decimal point */
      if (exponent > 0) numsize += exponent;				/*                + this many more before the decimal point */
      if (p -> negative || p -> plussign || p -> posblank) numsize ++;	/*               + maybe it includes room for the sign */
      PUTLEFTFILL (numsize);						/* output leading space padding */
      PUTSIGN;								/* output the sign character, if any */
      PUTLEFTZERO (numsize);						/* output leading zero padding */
      while (exponent >= 0) {
        i = floatnum;							/* get digit in front of decimal point */
        -- exponent;							/* one less digit in front of decimal point to do */
        floatnum -= i;							/* remove from number */
        PUTCH (i + '0');						/* output it */
        floatnum *= 10.0;						/* shift to get next digit */
      }
      if (p -> altform || (p -> precision > 0)) PUTCH ('.');		/* decimal point if altform or any digits after decimal pt */
      while (p -> precision > 0) {					/* keep cranking out digits as long as precision says to */
        i = floatnum;
        -- (p -> precision);
        floatnum -= i;
        PUTCH (i + '0');
        floatnum *= 10.0;
      }
      PUTRIGHTFILL;							/* pad on right with spaces */
      break;
    }
  }

  *ap_r = ap;
  return (OZ_SUCCESS);
}
