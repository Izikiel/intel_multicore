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
/*  Generic output conversion routines					*/
/*									*/
/*	'Extra' format types:						*/
/*									*/
/*	  %t - date/time in format yyyy-mm-dd@hh:mm:ss.ccccccc		*/
/*		- arg is one OZ_Datebin, convert to local time		*/
/*		  (this form is used for printing absolute values)	*/
/*									*/
/*	  %#t - days/time - in format dddddd@hh:mm:ss.ccccccc		*/
/*		- arg is one OZ_Datebin					*/
/*		  (this form is used for printing delta values)		*/
/*									*/
/*	  %##t - date/time in format yyyy-mm-dd@hh:mm:ss.ccccccc	*/
/*	         - arg is one OZ_Datebin, leave it as UTC		*/
/*	           (this form is used for printing absolute values)	*/
/*									*/
/*	  %S - printable string - arg is one char *			*/
/*									*/
/*	Extra modifiers for %d, %o, %u, %x:				*/
/*									*/
/*	  B - datatype is (u)Byte (8 bits)				*/
/*	  L - datatype is (u)Long (32 bits)				*/
/*	  P - datatype is OZ_Pointer (32 or 64 bits)			*/
/*	  Q - datatype is (u)Quad (64 bits)				*/
/*	  W - datatype is (u)Word (16 bits)				*/
/*									*/
/************************************************************************/

#include <stdarg.h>
#include "ozone.h"
#include "oz_knl_hw.h"
#include "oz_knl_status.h"
#include "oz_sys_dateconv.h"
#include "oz_sys_misc.h"
#include "oz_sys_tzconv.h"
#include "oz_sys_vxprintf.h"
#include "oz_sys_xprintf.h"

/************************************************************************/
/*									*/
/*  These routines place output in a fixed caller supplied buffer	*/
/*									*/
/*    Input:								*/
/*									*/
/*	size   = size of caller supplied buffer				*/
/*	buff   = address of caller supplied buffer			*/
/*	format = pointer to null terminated format string		*/
/*	ap     = conversion argument list pointer			*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_sprintf = OZ_SUCCESS : successful conversion		*/
/*	               OZ_BUFFEROVF : buffer overflowed			*/
/*	*buff = filled with null terminated buffer			*/
/*									*/
/************************************************************************/

static uLong inmemory (void *param, uLong *size, char **buff);

uLong oz_sys_sprintf (uLong size, char *buff, const char *format, ...)

{
  uLong sts;
  va_list ap;

  va_start (ap, format);
  sts = oz_sys_vsprintf (size, buff, format, ap);
  va_end (ap);
  return (sts);
}

uLong oz_sys_vsprintf (uLong size, char *buff, const char *format, va_list ap)

{
  uLong origsize;

  origsize = size;
  return (oz_sys_vxprintf (inmemory, &origsize, size, buff, NULL, format, ap));
}

static uLong inmemory (void *param, uLong *size, char **buff)

{
  *(uLong *)param -= *size;				/* decrement size of remaining area */
  *buff += *size;					/* increment pointer to remaining area */
  if (*(uLong *)param == 0) return (OZ_BUFFEROVF);	/* if buffer is filled completely, it is overflowed */
  **buff = 0;						/* null terminate the string (assuming there won't be any more) */
  return (OZ_SUCCESS);					/* successful */
}

/************************************************************************/
/*									*/
/*    Input:								*/
/*									*/
/*	entry  = routine to call with output data			*/
/*	param  = parameter to pass to 'entry' routine			*/
/*	size   = size of 'buff'						*/
/*	buff   = address of temporary buffer				*/
/*	format = pointer to null terminated format string		*/
/*	ap     = conversion argument list pointer			*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_xprintf = OZ_SUCCESS : successful conversion		*/
/*	                       else : as returned by 'entry'		*/
/*	*rlen = number of chars written to output			*/
/*									*/
/*    Note:								*/
/*									*/
/*	'entry' routine is called whenever the buff fills.  If it 	*/
/*	wants, it can alter the size and address of the buffer before 	*/
/*	it returns.							*/
/*									*/
/*	'Extra' format types:						*/
/*									*/
/*	  %t - date/time - arg is one OZ_Datebin			*/
/*	  %S - printable string - arg is one char *			*/
/*									*/
/************************************************************************/

uLong oz_sys_xprintf (uLong (*entry) (void *param, uLong *size, char **buff), void *param, uLong size, char *buff, uLong *rlen, const char *format, ...)

{
  uLong sts;
  va_list ap;

  va_start (ap, format);
  sts = oz_sys_vxprintf (entry, param, size, buff, rlen, format, ap);
  va_end (ap);
  return (sts);
}

uLong oz_sys_vxprintf (uLong (*entry) (void *param, uLong *size, char **buff), void *param, uLong size, char *buff, uLong *rlen, const char *format, va_list ap)

{
  char *cp, fc, ncb[32];
  const char *fp, *sp;
  int exponent, i, numsize;
  long long int signednum;
  uLong siz, sts;
  OZ_Datebin datebin;
  Par par, *p;
  unsigned long long int unsignednum;

  if (rlen != NULL) *rlen = 0;

  p = &par;						/* point to param block */
  memset (p, 0, sizeof *p);				/* clear out param block */

  p -> entry = entry;
  p -> param = param;

  p -> op = p -> ob = buff;				/* point to beginning of block buffer */
  p -> oe = buff + size;				/* point to end of block buffer */

  if (p -> op == p -> oe) {				/* see if buffer is full */
    siz = p -> oe - p -> ob;				/* number of bytes being written = the whole buffer */
    sts = (*(p -> entry)) (p -> param, &siz, &(p -> ob)); /* if so, flush it */
    if (sts != OZ_SUCCESS) return (sts);		/* return any error status */
    p -> op = p -> ob;					/* reset buffer pointer */
    p -> oe = p -> ob + siz;
  }

  for (fp = format; (fc = *fp) != 0;) {			/* scan each character in the format string */
    if (fc != '%') {					/* if not a percent, copy as is to output buffer */
      sp = strchr (fp, '%');
      if (sp == NULL) i = strlen (fp);
      else i = sp - fp;
      PUTST (i, fp);
      fp += i;
      continue;
    }

    p -> altform   = 0;					/* percent, initialize param block values */
    p -> zeropad   = 0;
    p -> leftjust  = 0;
    p -> posblank  = 0;
    p -> plussign  = 0;
    p -> minwidth  = 0;
    p -> precision = -1;
    p -> intsize   = 0;

    fp ++;						/* skip over the percent */

    /* Process format item prefix characters */

    while ((fc = *fp) != 0) {
      fp ++;
      switch (fc) {
        case '#': p -> altform ++; break;				/* 'alternate form' */
        case '0': if (!p -> leftjust) p -> zeropad  = 1; break;		/* zero pad the numeric value (ignore precision) */
        case '-': p -> leftjust = 1;  p -> zeropad  = 0; break;		/* left justify space fill (otherwise it is right justified) */
        case ' ': if (!p -> plussign) p -> posblank = 1; break;		/* leave a space for a plus sign if value is positive */
        case '+': p -> plussign = 1;  p -> posblank = 0; break;		/* include a '+' if value is positive */
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9': 
        case '*': -- fp; GETFMTINT (p -> minwidth); break;		/* minimum field width */
        case '.': GETFMTINT (p -> precision); break;			/* precision */
        case 'h': p -> intsize  = sizeof (short); break;		/* value is a 'short int' */
        case 'l': p -> intsize += sizeof (long); break; 		/* value is a 'long int' or 'long long int' */
        case 'B': p -> intsize  = sizeof (Byte); break;			/* value is a 'Byte' */
        case 'L': p -> intsize  = sizeof (Long); p -> longdoub = 1; break; /* value is a 'Long' or a 'long double' */
        case 'P': p -> intsize  = sizeof (OZ_Pointer); break;		/* value is a 'OZ_Pointer' */
        case 'Q': p -> intsize  = sizeof (Quad); break;			/* value is a 'Quad' */
        case 'W': p -> intsize  = sizeof (Word); break;			/* value is a 'Word' */

        default: goto gotfinal;						/* unknown, assume it's the final character */
      }
    }
gotfinal:

    /* Process final format character using those prefixes */

    switch (fc) {

      /* Character and string convertions */

      case 'c': {					/* - single character */
        fc = va_arg (ap, int);						/* get character to be output */
        if (!(p -> leftjust)) PUTFC (p -> minwidth - 1, ' ');		/* output leading padding */
        PUTCH (fc);							/* output the character */
        PUTRIGHTFILL;							/* output trailing padding */
        break;
      }

      case 's': {					/* - character string */
        sp = va_arg (ap, const char *);					/* point to string to be output */
        if (sp == NULL) sp = "(nil)";
        i = strlen (sp);						/* get length of the string */
        if ((p -> precision < 0) || (p -> precision > i)) p -> precision = i; /* get length to be output */
        if (!(p -> leftjust)) PUTFC (p -> minwidth - p -> precision, ' '); /* output leading padding */
        PUTST (p -> precision, sp);					/* output the string itself */
        PUTRIGHTFILL;							/* output trailing padding */
        break;
      }

      case 'S': {					/* - printable character string */
        sp = va_arg (ap, const char *);					/* point to string to be output */
        if (sp == NULL) sp = "(nil)";
        i = strlen (sp);						/* get length of the string */
        if ((p -> precision < 0) || (p -> precision > i)) p -> precision = i; /* get length to be output */
        if (!(p -> leftjust)) PUTFC (p -> minwidth - p -> precision, ' '); /* output leading padding */
        while ((p -> precision > 0) && ((fc = *(sp ++)) != 0)) {
          p -> precision --;
          if ((fc == 0x7f) || (fc & 0x80) || (fc < ' ')) fc = '.';	/* convert anything funky to a dot */
          PUTCH (fc);							/* output the character */
        }
        PUTRIGHTFILL;							/* output trailing padding */
        break;
      }

      /* Integer convertions */

      case 'D': p -> intsize = 4;
      case 'd':
      case 'i': {					/* - signed decimal */
        GETSIGNEDNUM;							/* get value into 'signednum', with sign in 'negative' */
        p -> ncp = p -> nce = ncb + sizeof ncb;				/* point to end of numeric conversion buffer */
        do {								/* convert to decimal string in end of ncb */
          *(-- (p -> ncp)) = (unsignednum % 10) + '0';
          unsignednum /= 10;
        } while (unsignednum != 0);
        PUTINT ("");
        break;
      }

      case 'O': p -> intsize = 4;
      case 'o': {					/* - unsigned octal */
        GETUNSIGNEDNUM;							/* get value into 'unsignednum' */
        p -> ncp = p -> nce = ncb + sizeof ncb;				/* point to end of numeric conversion buffer */
        do {								/* convert to octal string in end of ncb */
          *(-- (p -> ncp)) = (unsignednum & 7) + '0';
          unsignednum >>= 3;
        } while (unsignednum != 0);
        PUTINT ("0");
        break;
      }

      case 'U': p -> intsize = 4;
      case 'u': {					/* - unsigned decimal */
        GETUNSIGNEDNUM;							/* get value into 'unsignednum' */
        p -> ncp = p -> nce = ncb + sizeof ncb;				/* point to end of numeric conversion buffer */
        do {								/* convert to decimal string in end of ncb */
          *(-- (p -> ncp)) = (unsignednum % 10) + '0';
          unsignednum /= 10;
        } while (unsignednum != 0);
        PUTINT ("");
        break;
      }

      case 'x': {					/* - unsigned hexadecimal (lower case) */
        GETUNSIGNEDNUM;							/* get value into 'unsignednum' */
        p -> ncp = p -> nce = ncb + sizeof ncb;				/* point to end of numeric conversion buffer */
        do {								/* convert to decimal string in end of ncb */
          *(-- (p -> ncp)) = "0123456789abcdef"[unsignednum&15];
          unsignednum >>= 4;
        } while (unsignednum != 0);
        PUTINT ("0x");
        break;
      }

      case 'X': {					/* - unsigned hexadecimal (caps) */
        GETUNSIGNEDNUM;							/* get value into 'unsignednum' */
        p -> ncp = p -> nce = ncb + sizeof ncb;				/* point to end of numeric conversion buffer */
        do {								/* convert to decimal string in end of ncb */
          *(-- (p -> ncp)) = "0123456789ABCDEF"[unsignednum&15];
          unsignednum >>= 4;
        } while (unsignednum != 0);
        PUTINT ("0X");
        break;
      }

      /* Floating point convertions - in a different module so this one can be compiled without floating point */

      case 'E':
      case 'G':
      case 'e':
      case 'f':
      case 'g': {
        va_list ap_r;

        sts = oz_sys_vxprintf_fp (fc, p, ap, &ap_r);
        if (sts != OZ_SUCCESS) return (sts);
        ap = ap_r;
        break;
      }

      /* Misc convertions */

      case 'p': {					/* - pointer */
        unsignednum = (unsigned long) va_arg (ap, void *);		/* get value */
        p -> negative = 0;						/* it's never negative by definition */
        p -> ncp = p -> nce = ncb + sizeof ncb;				/* point to end of numeric conversion buffer */
        do {								/* convert to decimal string in end of ncb */
          *(-- (p -> ncp)) = "0123456789abcdef"[unsignednum&15];
          unsignednum /= 16;
        } while (unsignednum != 0);
        p -> altform = 1;						/* always include the 0x */
        PUTINT ("0x");
        break;
      }

      case 'n': {					/* - number of output characters so far */
        if (p -> intsize == 2) *(va_arg (ap, short int *)) = p -> numout;
        else if (p -> intsize == 4) *(va_arg (ap, long int *)) = p -> numout;
        else *(va_arg (ap, int *)) = p -> numout;
        break;
      }

      case 't': {					/* - date/time */
        int zflag;

        zflag = 0;
        datebin = va_arg (ap, OZ_Datebin);				/* get date to be converted */
        if (p -> altform == 0) {
          if (oz_hw_inknlmode () && (oz_hw_cpu_smplevel () > OZ_SMPLOCK_SOFTINT)) {
            zflag = 1;							/* can't convert timezone above softint, so stab in a Z */
          } else {
            sts = oz_sys_tzconv ( OZ_DATEBIN_TZCONV_UTC2LCL, datebin, 0, &datebin, 0, NULL); /* convert time zone */
            if (sts != OZ_SUCCESS) zflag = 1;				/* if failure, stab in a Z and just use UTC */
          }
        }
        oz_sys_datebin_decstr (p -> altform & 1, datebin, sizeof ncb, ncb); /* convert to a string (if 'altform', use delta format) */
        if (p -> altform & 1) {
          cp = strchr (ncb, '.');					/* find the dot for the fraction of a second */
									/* precision indicates how many places after it we show */
									/* minwidth indicates total number of characters we show */
          i = strlen (cp + 1);						/* get number of digits there actually are after the dot */
          if ((p -> precision >= 0) && (p -> precision < i)) {
            i = p -> precision;						/* get number of digits to output after the decimal point */
            if (i > 0) i ++;						/* include the . in the number of chars to leave standing */
            cp[i] = 0;							/* truncate the string there */
          }
          i = strlen (ncb);						/* get length so far */
          cp = ncb;
          while ((i > p -> minwidth) || (*cp < '0') || (*cp > '9')) {	/* see if minwidth says to lop zeroes off the front */
									/* (always lop off leading separators) */
            if ((*cp >= '1') && (*cp <= '9')) break;			/* stop if non-zero digit */
            if (cp[1] == 0) break;					/* stop if end-of-string coming up (make sure we output at least one digit) */
            if (cp[1] == '.') break;					/* stop if decimal point coming up (make sure we output at least one digit before '.') */
            i --;							/* separator or unwanted zero, skip over it */
            cp ++;
          }
          if (!(p -> leftjust)) PUTFC (p -> minwidth - i, ' ');		/* output leading padding */
          PUTST (i, cp);						/* output the string itself */
        } else {
          i = strlen (ncb);						/* get length of the string */
          if ((p -> precision >= 0) && (p -> precision <= i)) {		/* see if string overflows output */
            if (zflag) ncb[p->precision-1] = 'z';			/* if so, make sure the Z can be seen */
          } else {
            if (zflag) ncb[i++] = 'z';					/* we got room, tack Z on the end */
            p -> precision = i;						/* get length to be output */
          }
          if (!(p -> leftjust)) PUTFC (p -> minwidth - p -> precision, ' '); /* output leading padding */
          PUTST (p -> precision, ncb);					/* output the string itself */
        }
        PUTRIGHTFILL;							/* output trailing padding */
        break;
      }

      /* Don't know what it is, just output it as is (maybe it was '%%') */

      default: {
        PUTCH (fc);
        break;
      }
    }
  }

  if (rlen != NULL) *rlen = p -> numout;
  siz = p -> op - p -> ob;
  return ((*(p -> entry)) (p -> param, &siz, &(p -> ob)));
}

/* Put the integer in p -> ncp..p -> nce, optionally prepending the alternate string and padding as required */

uLong putint (Par *p, char *alt)

{
  int altneeded, numdigs, numzeroes, signneeded;
  uLong sts;

  numdigs = p -> nce - p -> ncp;					/* get number of digits in p -> ncp..p -> nce string */

  signneeded = (p -> negative || p -> plussign || p -> posblank);	/* maybe another char will be required for sign position */

  altneeded = 0;							/* maybe need the alternate string */
  if (p -> altform) altneeded = strlen (alt);

  numzeroes = 0;  							/* maybe it needs zeroes on front for precision padding */
  if (numdigs < p -> precision) numzeroes = p -> precision - numdigs;

  PUTLEFTFILL (signneeded + altneeded + numzeroes + numdigs);		/* output any left blank filling */
  PUTSIGN;								/* output the sign character, if any */
  PUTST (altneeded, alt);						/* output the alternate string, if any */
  PUTLEFTZERO (numzeroes + numdigs);					/* put left zero padding (for minimum width) */
  PUTFC (numzeroes, '0');						/* put left zeroes for precision */
  PUTST (numdigs, p -> ncp);						/* output the digits as a string */
  PUTRIGHTFILL;								/* pad on the right with blanks if needed (for minimum width) */

  return (OZ_SUCCESS);
}

/* Output sign character, given p -> negative, plussign, posblank */

uLong putsign (Par *p)

{
  uLong sts;

  if (p -> negative) PUTCH ('-');
  else if (p -> plussign) PUTCH ('+');
  else if (p -> posblank) PUTCH (' ');

  return (OZ_SUCCESS);
}

/* Output fill characters, flush if full */

uLong putfc (Par *p, int s, char c)

{
  uLong r, siz, sts;

  if (s > 0) {
    p -> numout   += s;							/* this much more will have been output */
    p -> minwidth -= s;							/* this fewer chars needed to fill output field minimum width */

    while (s > 0) {
      r = p -> oe - p -> op;						/* see how much room is left */
      if (r > s) break;							/* stop if enough room left and leave at least a byte */
      memset (p -> op, c, r);						/* fill it with parameter */
      s -= r;								/* update remaining size */
      siz = p -> oe - p -> ob;						/* number of bytes being written = the whole buffer */
      sts = (*(p -> entry)) (p -> param, &siz, &(p -> ob));		/* write it out to file */
      if (sts != OZ_SUCCESS) return (sts);				/* return any error status */
      p -> op = p -> ob;						/* reset buffer pointer */
      p -> oe = p -> ob + siz;
    }

    memset (p -> op, c, s);						/* fill last part */
    p -> op += s;
  }

  return (OZ_SUCCESS);							/* successful */
}

/* Output string, flush if full */

uLong putst (Par *p, int s, const char *b)

{
  uLong r, siz, sts;

  if (s > 0) {
    p -> numout   += s;							/* this much more will have been output */
    p -> minwidth -= s;							/* this fewer chars needed to fill output field minimum width */

    while (s > 0) {
      r = p -> oe - p -> op;						/* see how much room is left */
      if (r > s) break;							/* stop if enough room left and leave at least a byte */
      memcpy (p -> op, b, r);						/* fill it with parameter */
      s -= r;								/* update remaining size */
      b += r;								/* update remaining address */
      siz = p -> oe - p -> ob;						/* number of bytes being written = the whole buffer */
      sts = (*(p -> entry)) (p -> param, &siz, &(p -> ob));		/* write it out to file */
      if (sts != OZ_SUCCESS) return (sts);				/* return any error status */
      p -> op = p -> ob;						/* reset buffer pointer */
      p -> oe = p -> ob + siz;
    }

    memcpy (p -> op, b, s);						/* copy last part */
    p -> op += s;
  }

  return (OZ_SUCCESS);							/* successful */
}

/* Output single character, flush if full */

uLong putch (Par *p, char c)

{
  uLong siz, sts;

  *(p -> op ++) = c;							/* store character in buffer */
  p -> numout ++;							/* one more char has been output */
  p -> minwidth --;							/* one less char needed to fill output field minimum width */
  if (p -> op == p -> oe) {						/* see if buffer is full */
    siz = p -> oe - p -> ob;						/* number of bytes being written = the whole buffer */
    sts = (*(p -> entry)) (p -> param, &siz, &(p -> ob));		/* if so, flush it */
    if (sts != OZ_SUCCESS) return (sts);				/* return any error status */
    p -> op = p -> ob;							/* reset buffer pointer */
    p -> oe = p -> ob + siz;
  }

  return (OZ_SUCCESS);							/* successful */
}
