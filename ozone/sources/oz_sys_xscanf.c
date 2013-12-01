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
/*  Scan input stream							*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_knl_hw.h"
#include "oz_knl_status.h"
#include "oz_sys_pdata.h"
#include "oz_sys_xscanf.h"

#ifndef EOF
#define EOF (-1)
#endif

#define ISSPACE(__c) ((__c <= ' ') || (__c == 127))
#define ISDIGIT(__c) ((__c >= '0') && (__c <= '9'))

typedef struct { uLong (*entry) (void *param, uLong *size, const char **buff);
                 void *param;
                 uLong index;
                 uLong isize;
                 const char *ibuff;
                 uLong sts;
                 uLong total;
                 int argpos;
                 va_list ap;
                 int numargs;
                 int argsize;
                 void **arglist;
               } Pblk;

#define GETCH getch (&pblk)
#define UNGETCH ungetch (&pblk)

#define ARG(__type) (__type)getarg (&pblk, sizeof (__type), #__type)

/* These are flags in the conversion format */
# define LONG           0x001   /* l: long or double */
# define LONGDBL        0x002   /* L: long long or long double */
# define SHORT          0x004   /* h: short */
# define SUPPRESS       0x008   /* *: suppress assignment */
# define TYPEMOD        (LONG|LONGDBL|SHORT)

static int getch (Pblk *pblk);
static void ungetch (Pblk *pblk);
static void *getarg (Pblk *pblk, int size, const char *name);

uLong oz_sys_xscanf (uLong (*entry) (void *param, uLong *size, const char **buff), void *param, uLong *nargs, uLong *uncnv, const char *format, ...)

{
  uLong sts;
  va_list ap;

  va_start (ap, format);
  sts = oz_sys_vxscanf (entry, param, nargs, uncnv, format, ap);
  va_end (ap);
  return (sts);
}

uLong oz_sys_vxscanf (uLong (*entry) (void *param, uLong *size, const char **buff), void *param, uLong *nargs, uLong *uncnv, const char *format, va_list ap)

{
  char fmtc, *str;
  const char *fmt, *savef;
  int base, c, done, flags, numargs, number_signed, skip_space, strsize, width;
  Pblk pblk;

  pblk.entry   = entry;
  pblk.param   = param;
  pblk.isize   = 0;
  pblk.ibuff   = NULL;
  pblk.total   = 0;
  pblk.sts     = OZ_SUCCESS;

  pblk.ap      = ap;			/* points to subroutine arglist */
  pblk.numargs = 0;			/* number of args fetched (elements valid in pblk.arglist) */
  pblk.argsize = 0;			/* size allocated to pblk.arglist */
  pblk.arglist = NULL;			/* assume we won't need this crap */

  for (fmt = format; (fmtc = *(fmt ++)) != 0;) {
    if (fmtc == '%') {			/* scan for <N>$ following a % */
      do fmtc = *(fmt ++);
      while (ISDIGIT (fmtc));
      if (fmtc == '$') break;		/* if so, we need the pblk.arglist crap */
    }
  }
  if (fmtc != 0) {
    pblk.argsize = 16;			/* size allocated to pblk.arglist */
    pblk.arglist = oz_sys_pdata_malloc (OZ_PROCMODE_KNL, 16 * sizeof *pblk.arglist);
  }

  skip_space = 0;			/* haven't seen any space chars in format string yet */
  done = 0;				/* haven't converted any output values yet */
  fmt = format;				/* point to format string */

  while ((fmtc = *(fmt ++)) != 0) {

    /* All non-conversion characters must match exactly */

    if (fmtc != '%') {
      if (ISSPACE (fmtc)) {		/* see if it is whitespace */
        skip_space = 1;			/* if so, say we're skipping whitespace */
        continue;
      }
      c = GETCH;			/* not a space, get input character */
      if (c == EOF) goto input_error;	/* stop if eof */
      if (skip_space) {
        while (ISSPACE (c)) {
          c = GETCH;
          if (c == EOF) goto input_error;
        }
        skip_space = 0;
      }
      if (c != fmtc) {			/* see if characters match */
        UNGETCH;			/* if not, undo it */
        goto conv_error;		/* return error status */
      }
      continue;
    }

    /* This is the start of the conversion string */

    flags  = 0;
    pblk.argpos = 0;

    fmtc = *fmt;

    /* Argument position given by <N>$ */

    savef = fmt;
    if (ISDIGIT (fmtc)) {
      do {
        pblk.argpos = pblk.argpos * 10 + (fmtc - '0');
        fmtc = *(++ fmt);
      } while (ISDIGIT (fmtc));
      if (fmtc == '$') {
        fmtc = *(++ fmt);			/* it must end in $ */
      } else {
        fmt = savef;				/* otherwise, restore format pointer */
        pblk.argpos = 0;			/* pretend like we never saw it */
        fmtc = *fmt;				/* retrieve the first digit */
      }
    }

    /* Check for the assignment-suppressing flag */

    if (fmtc == '*') {
      flags |= SUPPRESS;
      fmtc = *(++ fmt);
    }

    /* Process width */

    width = 0;
    while (ISDIGIT (fmtc)) {
      width = width * 10 + fmtc - '0';
      fmtc = *(++ fmt);
    }
    if (width == 0) width = -1;

    /* Check for type modifiers */

    while (fmtc == 'h' || fmtc == 'l' || fmtc == 'L' || fmtc == 'a' || fmtc == 'q') {
      switch (fmtc) {
        case 'h': {
          if (flags & TYPEMOD) goto conv_error;
          flags |= SHORT;
          break;
        }
        case 'l': {
          if (flags & (SHORT | LONGDBL)) goto conv_error;
          if (flags & LONG) {
            flags &= ~LONG;
            flags |= LONGDBL;
          } else {
            flags |= LONG;
          }
          break;
        }
        case 'q':
        case 'L': {
          if (flags & TYPEMOD) goto conv_error;
          flags |= LONGDBL;
          break;
        }
      }
      fmtc = *(++ fmt);
    }

    /* End of format string is an error */

    if (fmtc == 0) goto conv_error;

    /* Maybe skip leading spaces in input */

    if (skip_space || (fmtc != 'c' && fmtc != 'C' && fmtc != 'n')) {
      do {
        c = GETCH;
        if (c == EOF) goto input_error;
      } while (ISSPACE (c));
      UNGETCH;
      skip_space = 0;
    }

    /* Dispatch on format char */

    fmt ++;

    switch (fmtc) {

      /* Must match a literal '%' */

      case '%': {
        c = GETCH;
        if (c != '%') {
          UNGETCH;
          goto conv_error;
        }
        break;
      }

      /* Return number of characters used from input so far */

      case 'n': {
        if (!(flags & SUPPRESS)) {
          if (flags & LONGDBL) *ARG (long long int *) = pblk.total;
          else if (flags & LONG) *ARG (long int *) = pblk.total;
          else if (flags & SHORT) *ARG (short int *) = pblk.total;
          else *ARG (int *) = pblk.total;
        }
        break;
      }

      /* Match characters */

      case 'c': {
        str = NULL;
        if (!(flags & SUPPRESS)) {
          str = ARG (char *);			/* get pointer where to return characters */
          if (str == NULL) goto conv_error;
        }
        if (width == -1) width = 1;		/* default to retrieving one character */
        while (-- width >= 0) {			/* repeat while there's room for more */
          c == GETCH;				/* retrieve input character */
          if (c == EOF) goto input_error;	/* abort if EOF */
          if (str != NULL) *(str ++) = c;	/* store in output buffer */
        }
        if (!(flags & SUPPRESS)) done ++;
        break;
      }

      /* Hexadecimal integer */

      case 'x':
      case 'X': {
        base = 16;
        number_signed = 0;
        goto number;
      }

      /* Octal integer */

      case 'o': {
        base = 8;
        number_signed = 0;
        goto number;
      }

      /* Unsigned decimal integer */

      case 'u': {
        base = 10;
        number_signed = 0;
        goto number;
      }

      /* Signed decimal integer */

      case 'd': {
        base = 10;
        number_signed = 1;
        goto number;
      }

      /* Pointer (same size as long int) */

      case 'p': {
        base = 16;
        flags &= ~(SHORT|LONGDBL);
        flags |= LONG;
        number_signed = 0;
        goto number;
      }

      /* Generic number */

      case 'i': {
        base = 0;
        number_signed = 1;
      }

      number: {
        int negative, wpsize;
        long long workspace;

        c = GETCH;
        if (c == EOF) goto input_error;

        /* Check for a sign */

        negative = 0;
        if ((c == '-') || (c == '+')) {
          if (width > 0) -- width;
          if (c == '-') negative = 1;
          c = GETCH;
        }

        /* Look for a leading indication of base */

        if ((width != 0) && (c == '0')) {
          if (width > 0) -- width;
          c = GETCH;
          if ((width != 0) && ((c == 'x') || (c == 'X'))) {
            if (base == 0) base = 16;
            if (base == 16) {
              if (width > 0) -- width;
              c = GETCH;
            }
          } else if (memchr ("0123456789aAbBcCdDeEfFgGhHiIjJkKlLmMnNoOpPqQrRsStTuUvVwWxXyYzZ", c, base + (base > 10 ? base - 10 : 0)) == NULL) {
            if (c != EOF) UNGETCH;
            goto gotnumber;
          } else {
            if (base == 0) base = 8;
          }
        }

        /* Default base is 10 */

        if (base == 0) base = 10;

        /* Read the number into workspace */

        workspace = 0;
        wpsize = 0;
        while ((c != EOF) && (width != 0)) {
          if ((c >= '0') && (c <= '9') && (c < '0' + base)) {
            workspace = workspace * base + c - '0';
          } else if ((base > 10) && (c >= 'A') && (c < 'A' + base - 10)) {
            workspace = workspace * base + c - 'A' + 10;
          } else if ((base > 10) && (c >= 'a') && (c < 'a' + base - 10)) {
            workspace = workspace * base + c - 'a' + 10;
          } else {
            break;
          }
          wpsize ++;
          if (width > 0) -- width;
          c = GETCH;
        }
        if (c != EOF) UNGETCH;
        if (wpsize == 0) goto conv_error;

        /* Return result to caller */

      gotnumber:
        if (!(flags & SUPPRESS)) {
          if (!number_signed) {
            if (flags & LONGDBL) *ARG (unsigned long long int *) = workspace;
            else if (flags & LONG) *ARG (unsigned long int *) = workspace;
            else if (flags & SHORT) *ARG (unsigned short int *) = workspace;
            else *ARG (unsigned int *) = workspace;
          } else if (negative) {
            if (flags & LONGDBL) *ARG (long long int *) = - workspace;
            else if (flags & LONG) *ARG (long int *) = - workspace;
            else if (flags & SHORT) *ARG (short int *) = - workspace;
            else *ARG (int *) = - workspace;
          } else {
            if (flags & LONGDBL) *ARG (long long int *) = workspace;
            else if (flags & LONG) *ARG (long int *) = workspace;
            else if (flags & SHORT) *ARG (short int *) = workspace;
            else *ARG (int *) = workspace;
          }
          done ++;
        }
        break;
      }

      /* String conversion */

      case 's': {
        if (!(flags & SUPPRESS)) {
          str = ARG (char *);						/* get pointer to buffer */
          if (str == NULL) goto conv_error;
        }

        c = GETCH;							/* must be able to get at least one character */
        if (c == EOF) goto input_error;

        do {
          if (ISSPACE (c)) {						/* stop on a whitespace */
            UNGETCH;
            break;
          }
          if (!(flags & SUPPRESS)) {
            *(str ++) = c;						/* store char in output buffer */
          }
        } while ((width < 0 || -- width > 0) && ((c = GETCH) != EOF));	/* repeat while width left and while input left */
        if (!(flags & SUPPRESS)) {
          *str = 0;							/* null terminate output buffer */
          done ++;							/* one more parameter processed */
        }
        break;
      }

      /* Floating point conversions */

      case 'e':
      case 'E':
      case 'f':
      case 'g':
      case 'G': {
        int decimals, exponneg, exponval, got_dot, got_e, lastwase, negative, wpsize;
        long double doubnum;

        c = GETCH;
        if (c == EOF) goto input_error;
        negative = 0;				/* don't have negative sign yet */
        got_dot  = 0;				/* haven't seen a decimal point yet */
        got_e    = 0;				/* haven't seen an exponent 'e' yet */
        wpsize   = 0;				/* havent' processed any digits yet */
        doubnum  = 0.0;				/* start out with a value of zero */
        exponval = 0;				/* haven't seen any exponent digits yet */
        exponneg = 0;				/* assume the exponent will be positive */
        decimals = 0;				/* haven't seen any digits after decimal point yet */
        lastwase = 0;				/* last char was not the 'e' for exponent */

        if ((c == '-') || (c == '+')) {		/* check for overall sign character */
          if (c == '-') negative = 1;		/* if negative, set flag */
          if (width > 0) -- width;
          if (width == 0) goto conv_error;
          c = GETCH;				/* anyway, get next character */
        }

        while (c != EOF) {							/* repeat as long as there is input to process */
          if (ISDIGIT (c)) {							/* see if we have a digit */
            c -= '0';								/* ok, convert to integer */
            if (got_e) exponval = exponval * 10 + c;				/* maybe it's part of the exponent */
            else {
              doubnum = doubnum * 10.0 + c;					/* it's part of the number itself */
              if (got_dot) decimals ++;						/* if after decimal point, count it */
            }
            wpsize ++;								/* anyway, get processed a digit */
            lastwase = 0;							/* this char was not the 'e' */
          } else if (wpsize > 0 && !got_e && (c == 'e' || c == 'E')) {		/* check for the 'e' */
            lastwase = got_e = got_dot = 1;					/* ok, set all these flags */
          } else if (lastwase && (c == '+' || c == '-')) {			/* check for sign char following the 'e' */
            if (c == '-') exponneg = 1;						/* maybe it makes the exponent negative */
            lastwase = 0;							/* this char was not the 'e' */
          } else if ((c == '.') && !got_dot) {					/* check for decimal point */
            wpsize ++;								/* it counts as a digit */
            got_dot = 1;							/* remember we found a decimal point */
            lastwase = 0;							/* this char was not the 'e' */
          } else {
            UNGETCH;								/* something else, undo the getc */
            break;								/* break out of loop */
          }
          if (width > 0) -- width;						/* decrement input width to process */
          if (width == 0) break;						/* stop if no more input to process */
          c = GETCH;								/* otherwise check out next character */
        }

        /* Must have had some digits */

        if (wpsize == 0) goto conv_error;

        /* Put decimals and exponent in mantissa */

        if (exponneg) exponval = - exponval;					/* put sign in exponent value */
        exponval -= decimals;							/* subtract number of digits after decimal point from exponent value */
        while (exponval > 0) {							/* doubnum = doubnum * 10 ** exponval */
          doubnum *= 10.0;
          exponval --;
        }
        while (exponval < 0) {
          doubnum /= 10.0;
          exponval ++;
        }

        /* Maybe it is negative */

        if (negative) doubnum = - doubnum;

        /* Return result */

        if (!(flags & SUPPRESS)) {
          if (flags & LONGDBL) *ARG (long double *) = doubnum;
          else if (flags & LONG) *ARG (double *) = doubnum;
          else *ARG (float *) = doubnum;
          done ++;
        }
        break;
      }

      /* Don't know what kind of conversion it is */

      default: goto conv_error;
    }
  }

  /* Maybe we should skip trailing spaces in input */

  if (skip_space) {
    do {
      c = GETCH;
      if (c == EOF) goto input_error;
    } while (ISSPACE (c));
    UNGETCH;
  }

  /* All done, return number of conversions performed, number of unconverted chars in input and status */

input_error:
  oz_sys_pdata_free (OZ_PROCMODE_KNL, pblk.arglist);
  if (nargs != NULL) *nargs = done;
  if (uncnv != NULL) *uncnv = pblk.isize;
  return (pblk.sts);

  /* Bad conversion specification in input string */

conv_error:
  pblk.sts = OZ_BADPARAM;
  goto input_error;
}

/* Get next input character */

static int getch (Pblk *pblk)

{
  uLong sts;

  /* If input buffer empty, read another */

  while (pblk -> isize == 0) {
    pblk -> sts = (*(pblk -> entry)) (pblk -> param, &(pblk -> isize), &(pblk -> ibuff));
    if (pblk -> sts != OZ_SUCCESS) return (EOF);
  }

  /* Return the next char in buffer */

  pblk -> isize --;					/* there will be one less char in there */
  pblk -> total ++;					/* one more input character processed */
  return ((unsigned char)(*(pblk -> ibuff ++)));	/* return char and increment pointer */
}

/* Put the character back */

static void ungetch (Pblk *pblk)

{
  pblk -> ibuff --;	/* back up input pointer */
  pblk -> isize ++;	/* increment number of chars remaining in input buffer */
  pblk -> total --;	/* one less total character processed */
}

/* Get argument */

static void *getarg (Pblk *pblk, int size, const char *name)

{
  void **newarglist;

  if (size != sizeof (void *)) oz_crash ("oz_sys_xscanf: too stupid to get type %s", name);

  /* Argpos zero means get the next arg from the list */

  if (pblk -> argpos == 0) {
    if (pblk -> arglist == NULL) return (va_arg (pblk -> ap, void *));	// if no arglist crap, just return directly from args
    if (pblk -> numargs >= pblk -> argsize) {				// see if there is room to save it
      pblk -> argsize += 16;						// if not, realloc with 16 more elements
      newarglist = oz_sys_pdata_malloc (OZ_PROCMODE_KNL, pblk -> argsize * sizeof *(pblk -> arglist));
      memcpy (newarglist, pblk -> arglist, pblk -> numargs * sizeof *(pblk -> arglist));
      oz_sys_pdata_free (OZ_PROCMODE_KNL, pblk -> arglist);
      pblk -> arglist = newarglist;
    }
    pblk -> arglist[pblk->numargs] = va_arg (pblk -> ap, void *);	// get next arg from list
    return (pblk -> arglist[pblk->numargs++]);				// return it to caller
  }

  /* Otherwise, get argpos'th arg from the list */

  if (pblk -> argpos >= pblk -> argsize) {				// see if there is room to save it
    pblk -> argsize = (pblk -> argpos + 16) & -16;			// if not, realloc to next 16 multiple
    newarglist = oz_sys_pdata_malloc (OZ_PROCMODE_KNL, pblk -> argsize * sizeof *(pblk -> arglist));
    memcpy (newarglist, pblk -> arglist, pblk -> numargs * sizeof *(pblk -> arglist));
    oz_sys_pdata_free (OZ_PROCMODE_KNL, pblk -> arglist);
    pblk -> arglist = newarglist;
  }
  while (pblk -> numargs < pblk -> argpos) {				// see if we already have it
    pblk -> arglist[pblk->numargs++] = va_arg (pblk -> ap, void *);	// if not, keep fetching until we do
  }
  return (pblk -> arglist[pblk->argpos-1]);				// return the one asked for
}
