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
/*  Date conversion routines						*/
/*  (The datebin <-> datelongs conversion is done by the hw routines)	*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_knl_hw.h"
#include "oz_knl_status.h"
#include "oz_sys_dateconv.h"

typedef const char *Ccharp;

/* Total days in a century */
#define CENTURYDAYS ((100*365)+(100/4)-(100/100))

/* Average quarter days per century */
#define QDAYSPCENT ((((100*365)+(100/4)-(100/100))*4)+(400/400))

/* Average quarter days per year */
#define QDAYSPYEAR ((365*4)+1)

/* Total days in a quadricentury */
#define QUADRIDAYS ((400*365)+(400/4)-(400/100)+(400/400))

/* Total days in a quadyear */
#define QUADYEARDAYS ((365*4)+1)

/* Offset in days from 1-JAN-1501 to 1-JAN-1601 */
#define TIMOFF1 (((1601-1501)*365)+((1601-1501)/4)-((1601-1501)/100)+((1601-1501)/400))

/* Days in month */

static const uLong daysinmonth[12] = { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

/* Internal routines */

static char *ultoa (uLong value, int size, char *buff);
static uLong atoul (int *size, Ccharp *buff);

/************************************************************************/
/*									*/
/*  Convert a daynumber to century, year-in-century, month-in-year, 	*/
/*  day-in-month							*/
/*									*/
/*    Input:								*/
/*									*/
/*	daynumber = number of days since epoch				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_daynumber_decode = decoded daynumber			*/
/*		oz_sys_daynumber_decode<00:07> = day in month (starts at 1)
/*		oz_sys_daynumber_decode<08:15> = month in year (starts at 1)
/*		oz_sys_daynumber_decode<16:31> = year			*/
/*									*/
/************************************************************************/

uLong oz_sys_daynumber_decode (uLong daynumber)

{
  uLong r1, r2, r3, r4;
  uLong yyyymmdd;

  r1  = daynumber;

  r1 += TIMOFF1;		/* add time offset so that day is relative to 1-JAN-1501 */

  r2  = r1 % QUADRIDAYS;
  r1  = r1 / QUADRIDAYS;	/* calculate number of quadricenturies that have passed since 1501 */

  /* r1 contains the number of quadricenturies and r2 contains the number of */
  /* days into the next quadricentury.  Calculate the number of centuries by */
  /* converting to quarter days into next quadricentury and then dividing by */
  /* the average number of quarter days in a century.                        */

  r2 *= 4;			/* calculate number of quarter days */
  r3  = r2 % QDAYSPCENT;
  r2  = r2 / QDAYSPCENT;	/* calculate number of centuries */

  /* r2 contains the number of centuries and r3 contains the number of quarter days into the next century. */

  /* Calculate years by discarding any fraction of a day, adding 3/4'ths of a */
  /* day, and dividing by the average number of days in a year.  The leap day */
  /* of each four year cycle is forced into the fourth year.                  */

  r3 |= 3;
  r4  = r3 % QDAYSPYEAR;
  r3  = r3 / QDAYSPYEAR;
  r4 /= 4;
  r4 ++;

  /* r1 contains number of quadricenturies. */
  /* r2 contains number of centuries.       */
  /* r3 contains number of years.           */
  /* r4 contains julian day of year.        */

  /* Calculate actual year. */

  r1  = r2 + 4 * r1;		/* combine centuries and quadricenturies */
  r1 *= 50;			/* calculate number of double centuries */
  r1  = 1501 + r3 + 2 * r1;	/* calculate actual year */

  yyyymmdd = r1 << 16;

  r2  = r1 % 100;		/* calculate century and year in century */
  r1 /= 100;

  /* Test for nonleap year and bias day if after 28-FEB */

  if (!(r2 & 3)) {		/* no leap year if year not multiple of 4 */
    if (r2 != 0) goto leapyear;	/* it is a leapyear if year not multiple of 100 */
    if (!(r1 & 3)) goto leapyear; /* it is a leapyear if year multiple of 400 */
  }
  if (r4 > 31+28) r4 ++;	/* after 28-FEB? yes, adjust for table bias (FEB listed as having 29 days) */
leapyear:

  /* Now convert biased julian day to month number and day number */

  for (r1 = 1; r1 <= 12; r1 ++) { /* loop through all months */
    r2  = daysinmonth[r1-1];	/* get number of days in month */
    if (r4 <= r2) break;	/* if we're there, stop looping */
    r4 -= r2;			/* subtract from julian day */
  }
  yyyymmdd += r1 << 8;		/* store month (starts at 1) */
  yyyymmdd += r4;		/* store day (starts at 1) */

  return (yyyymmdd);
}

/************************************************************************/
/*									*/
/*  Convert a daynumber to day-of-the-week number			*/
/*									*/
/*    Input:								*/
/*									*/
/*	daynumber = number of days since epoch				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_daynumber_weekday = day of week number			*/
/*		0 = Sunday						*/
/*		1 = Monday, etc.					*/
/*									*/
/************************************************************************/

uLong oz_sys_daynumber_weekday (uLong daynumber)

{
  return ((daynumber + 0) % 7);
}

/************************************************************************/
/*									*/
/*  Convert century, year-in-century, month-in-year, day-in-month to a 	*/
/*  daynumber								*/
/*									*/
/*    Input:								*/
/*									*/
/*	yyyymmdd = decoded date						*/
/*		yyyymmdd<00:07> = day in month (starts at 1)		*/
/*		yyyymmdd<08:15> = month in year (starts at 1)		*/
/*		yyyymmdd<16:31> = year (minimum 1601)			*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_daynumber_encode = number of days since epoch		*/
/*									*/
/************************************************************************/

uLong oz_sys_daynumber_encode (uLong yyyymmdd)

{
  uLong year, month, day;
  uLong r0, r1, r2, r3, r4, r5, r6;

  year = yyyymmdd >> 16;

  month = (yyyymmdd >> 8) & 255;
  day = yyyymmdd & 255;

  if (year < 1601) return (0);
  if ((month < 1) || (month > 12)) return (0);
  if (day < 1) return (0);

  /* Convert years to quadricenturies, centuries, quadyears, years */

  r0 = year - 1601;
  r1 = r0 % 400;		/* calculate quadricenturies */
  r0 = r0 / 400;

  r2 = r1 % 100;		/* calculate centuries */
  r1 = r1 / 100;

  r3 = r2 % 4;			/* calculate quadyears and years */
  r2 = r2 / 4;

  /* Convert quadricenturies, centuries, quadyears, years to days */

  r3 *= 365;			/* calculate number of days past leap year */
  r2  = r2 * QUADYEARDAYS + r3; /* calculate number of quadyear days and sum */
  r1 *= CENTURYDAYS;		/* calculate number of century days */
  r5  = r0 * QUADRIDAYS + r1;	/* calculate number of quadridays and sum */
  r0  = 0;			/* clear initial loop index */
  r6  = month;
x10:
  r5 += r2;			/* accumulate total days */
  r2 = daysinmonth[r0];		/* get number of days in month */
  if (r0 != 1) goto x30;	/* second month of year? */
  r3 = year;
  if (r3 & 3) goto x20;
  r4 = r3 % 100;
  r3 = r3 / 100;
  if (r4 != 0) goto x30;
  if (!(r3 & 3)) goto x30;
x20:
  r2 --;			/* reduce number of days in month */
x30:
  r0 ++;			/* increment month index */
  if (r0 < r6) goto x10;	/* repeat if more days to accumulate */
  if (day > r2) return (0);
  r5 += day;

  return (r5);
}

/************************************************************************/
/*									*/
/*  Decode a quadword date into a printable string			*/
/*									*/
/*    Input:								*/
/*									*/
/*	delta = 0 : output yyyy-mm-dd@hh:mm:ss.fffffff			*/
/*	        1 : output ddddddd@hh:mm:ss.fffffff			*/
/*	datebin = quadword date						*/
/*	size = size of buffer						*/
/*	buff = address of buffer					*/
/*									*/
/*    Output:								*/
/*									*/
/*	*buff = filled in with string, null terminated			*/
/*	oz_sys_datebin_decstr = length filled in			*/
/*									*/
/************************************************************************/

int oz_sys_datebin_decstr (int delta, OZ_Datebin datebin, int size, char *buff)

{
  char *p, tmpbuf[32];
  uLong datelongs[OZ_DATELONG_ELEMENTS], yyyymmdd;

  p = tmpbuf;
  oz_sys_datebin_decode (datebin, datelongs);					/* convert quad to longwords */
  if (delta) {
    p = ultoa (datelongs[OZ_DATELONG_DAYNUMBER], 7, p);				/* convert daynumber to ddddddd */
  } else {
    yyyymmdd = oz_sys_daynumber_decode (datelongs[OZ_DATELONG_DAYNUMBER]);	/* convert daynumber to yyyymmdd */
    p = ultoa (yyyymmdd >> 16, 4, p);						/* put year in temp buffer */
    *(p ++) = '-';
    p = ultoa ((yyyymmdd >> 8 & 255), 2, p);					/* put month in temp buffer */
    *(p ++) = '-';
    p = ultoa (yyyymmdd & 255, 2, p);						/* put day in temp buffer */
  }
  *(p ++) = '@';
  p = ultoa (datelongs[OZ_DATELONG_SECOND] / 3600, 2, p);			/* put hour in temp buffer */
  *(p ++) = ':';
  p = ultoa ((datelongs[OZ_DATELONG_SECOND] / 60) % 60, 2, p);			/* put minute in temp buffer */
  *(p ++) = ':';
  p = ultoa (datelongs[OZ_DATELONG_SECOND] % 60, 2, p);				/* put second in temp buffer */
  *(p ++) = '.';
#if OZ_TIMER_RESOLUTION == 10000000
  p = ultoa (datelongs[OZ_DATELONG_FRACTION], 7, p);				/* put fraction in temp buffer */
#else
  error : bad OZ TIMER RESOLUTION
#endif
  *p = 0;

  strncpyz (buff, tmpbuf, size);
  return (strlen (buff));
}

static char *ultoa (uLong value, int size, char *buff)

{
  int s;

  for (s = size; s > 0;) {
    buff[--s] = value % 10 + '0';
    value /= 10;
  }
  if (value != 0) memset (buff, '*', size);

  return (size + buff);
}

/************************************************************************/
/*									*/
/*  Encode a quadword date from a printable string			*/
/*									*/
/*    Input:								*/
/*									*/
/*	size = size of string						*/
/*	buff = address of string					*/
/*	now  = 0 : don't allow any named times				*/
/*	    else : allow named times, and consider this the current 	*/
/*	           time to base them on					*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_datebin_encstr = 0 : conversion failure			*/
/*	                        1 : successful absolute conversion	*/
/*	                       -1 : successful delta conversion		*/
/*	*datebin_r = quadword date					*/
/*									*/
/*    Note:								*/
/*									*/
/*	string is in absolute form yyyy-mm-dd@hh:mm:ss.fffffff 		*/
/*	or name@hh:mm:ss.fffffff, or in delta form 			*/
/*	ddddddd@hh:mm:ss.fffffff					*/
/*									*/
/************************************************************************/

int oz_sys_datebin_encstr (int size, const char *buff, OZ_Datebin *datebin_r)

{
  return (oz_sys_datebin_encstr2 (size, buff, datebin_r, 0));
}

int oz_sys_datebin_encstr2 (int size, const char *buff, OZ_Datebin *datebin_r, OZ_Datebin now)

{
  char separators[7];
  const char *fracbuff;
  int fsiz, n, named, rc;
  uLong datelongs[OZ_DATELONG_ELEMENTS], numbers[8];
  uLong hour, min, sec, frac, fracfact;

  named = 0;
  if (now != 0) {

    /* Maybe it is just 'now' */

    if ((size >= 3) && (strncasecmp (buff, "now", 3) == 0)) {
      if ((size > 3) && (buff[3] != 0)) return (0);
      *datebin_r = now;
      return (1);
    }

    /* Maybe it begins with 'today', 'yesterday' or 'tomorrow' */

    if ((size >= 5) && (strncasecmp (buff, "today", 5) == 0)) {
      size -= 5;
      buff += 5;
      oz_sys_datebin_decode (now, datelongs);
      named = 1;
    } else if ((size >= 9) && (strncasecmp (buff, "yesterday", 9) == 0)) {
      size -= 9;
      buff += 9;
      oz_sys_datebin_decode (now, datelongs);
      datelongs[OZ_DATELONG_DAYNUMBER] -= 1;
      named = 1;
    } else if ((size >= 8) && (strncasecmp (buff, "tomorrow", 8) == 0)) {
      size -= 8;
      buff += 8;
      oz_sys_datebin_decode (now, datelongs);
      datelongs[OZ_DATELONG_DAYNUMBER] += 1;
      named = 1;
    }

    if (named) {
      datelongs[OZ_DATELONG_FRACTION] = 0;		// it might be just plain 'today', 'tomorrow' or 'yesterday'
      datelongs[OZ_DATELONG_SECOND]   = 0;
      rc = 1;
      if ((size == 0) || (*buff == 0)) goto encode;	// if so, just go with the daynumber, leave seconds and fractions zero
      if (*(buff ++) != '@') return (0);		// something more, must be followed by an '@', fail if not
      -- size;
    }
  }

  /* We don't do null strings */

  if ((size == 0) || (*buff == 0)) return (0);

  /* Put all the numbers in the numbers array and the separators in the separators array */

  for (n = 0;; n ++) {			/* this is the number index */
    fracbuff = buff;			/* save beginning of number */
    numbers[n] = atoul (&size, &buff);	/* convert a number, decrement size, increment buff */
    fsiz = buff - fracbuff;		/* save how many digits were converted */
    if (size == 0) break;		/* stop if reached end of the input string */
    if (*buff == 0) break;
    if (n == 7) return (0);		/* abort if we converted 7 numbers already and we haven't reached end of string */
    separators[n] = *buff;		/* ok, save separator character */
    -- size;				/* ... and skip over it */
    buff ++;
  }
  separators[n] = 0;			/* end of string, end the separators */

  /* Figure out what to do depending on the separators that were found */

  rc = 1;
  if (named) {

    if (strcmp (separators, "::.") == 0) goto nam_ymdhmsf;	/* name@hh:mm:ss.ff */
    if (strcmp (separators, "::")  == 0) goto nam_ymdhms;	/* name@hh:mm:ss */
    if (strcmp (separators, ":")   == 0) goto nam_ymdhm;	/* name@hh:mm */

  } else {

    if (strcmp (separators, "--@::.") == 0) goto abs_ymdhmsf;	/* yyyy-mm-dd@hh:mm:ss.ff */
    if (strcmp (separators, "--@::")  == 0) goto abs_ymdhms;	/* yyyy-mm-dd@hh:mm:ss */
    if (strcmp (separators, "--@:")   == 0) goto abs_ymdhm;	/* yyyy-mm-dd@hh:mm */
    if (strcmp (separators, "--")     == 0) goto abs_ymd;	/* yyyy-mm-dd */

    rc = -1;
    if (strcmp (separators, "@::.") == 0) goto del_dhmsf;	/* dd@hh:mm:ss.ff */
    if (strcmp (separators, "@::")  == 0) goto del_dhms;	/* dd@hh:mm:ss */
    if (strcmp (separators, "@:")   == 0) goto del_dhm;		/* dd@hh:mm */
    if (strcmp (separators,  "::.") == 0) goto del_hmsf;	/* hh:mm:ss.ff */
    if (strcmp (separators,  "::")  == 0) goto del_hms;		/* hh:mm:ss */
    if (strcmp (separators,  ":.")  == 0) goto del_msf;		/* mm:ss.ff */
    if (strcmp (separators,  ":")   == 0) goto del_ms;		/* mm:ss */
    if (strcmp (separators,  ".")   == 0) goto del_sf;		/* ss.ff */
    if (strcmp (separators,  "")    == 0) goto del_s;		/* ss */
  }

  /* Bad format, return error status */

  return (0);

  /* Named format date conversions */

nam_ymdhm:	/* name@hh:mm */
  numbers[2] = 0;
nam_ymdhms:	/* name@hh:mm:ss */
  numbers[3] = 0;
nam_ymdhmsf:	/* name@hh:mm:ss.ff */
  hour = numbers[0];
  min  = numbers[1];
  sec  = numbers[2];
  frac = numbers[3];
  goto conv;

  /* Absolute format date conversions */

abs_ymd:	/* yyyy-mm-dd */
  numbers[3] = 0;
  numbers[4] = 0;
abs_ymdhm:	/* yyyy-mm-dd@hh:mm */
  numbers[5] = 0;
abs_ymdhms:	/* yyyy-mm-dd@hh:mm:ss */
  numbers[6] = 0;
abs_ymdhmsf:	/* yyyy-mm-dd@hh:mm:ss.ff */
  if (numbers[0] > 9999) return (0);
  if (numbers[1] > 12) return (0);
  if (numbers[2] > 31) return (0);
  datelongs[OZ_DATELONG_DAYNUMBER] = oz_sys_daynumber_encode ((((numbers[0] << 8) + numbers[1]) << 8) + numbers[2]);
  if (datelongs[OZ_DATELONG_DAYNUMBER] == 0) return (0);
  hour = numbers[3];
  min  = numbers[4];
  sec  = numbers[5];
  frac = numbers[6];
  goto conv;

  /* Delta format date conversions */

del_dhm:	/* dd@hh:mm */
  numbers[3] = 0;
del_dhms:	/* dd@hh:mm:ss */
  numbers[4] = 0;
del_dhmsf:	/* dd@hh:mm:ss.ff */
  datelongs[OZ_DATELONG_DAYNUMBER] = numbers[0];
  hour = numbers[1];
  min  = numbers[2];
  sec  = numbers[3];
  frac = numbers[4];
  goto conv;

del_hms:	/* hh:mm:ss */
  numbers[3] = 0;
del_hmsf:	/* hh:mm:ss.ff */
  datelongs[OZ_DATELONG_DAYNUMBER] = 0;
  hour = numbers[0];
  min  = numbers[1];
  sec  = numbers[2];
  frac = numbers[3];
  goto conv;

del_ms:		/* mm:ss */
  numbers[2] = 0;
del_msf:	/* mm:ss.ff */
  datelongs[OZ_DATELONG_DAYNUMBER] = 0;
  hour = 0;
  min  = numbers[0];
  sec  = numbers[1];
  frac = numbers[2];
  goto conv;

del_s:		/* ss */
  numbers[1] = 0;
del_sf:		/* ss.ff */
  datelongs[OZ_DATELONG_DAYNUMBER] = 0;
  hour = 0;
  min  = 0;
  sec  = numbers[0];
  frac = numbers[1];

  /* Convert datelongs[OZ_DATELONG_DAYNUMBER], hour, min, sec, frac to binary quadword */

conv:
  if (frac != 0) {					/* see if any fraction specified */
    if (frac >= OZ_TIMER_RESOLUTION) return (0);	/* must be less than resolution (ie, fraction/resolution < 1.0) */
    fracfact = OZ_TIMER_RESOLUTION;			/* scale it up for each missing digit */
    while (-- fsiz >= 0) fracfact /= 10;
    frac *= fracfact;
  }

  datelongs[OZ_DATELONG_FRACTION]  = frac;		/* set up datelongs */
  datelongs[OZ_DATELONG_SECOND]    = (hour * 60 + min) * 60 + sec;
encode:
  *datebin_r = oz_sys_datebin_encode (datelongs);	/* convert datelongs to quadword */
  return (rc);						/* successful */
}

static uLong atoul (int *size, Ccharp *buff)

{
  const char *b;
  char c;
  int s;
  uLong value;

  value = 0;
  s = *size;
  b = *buff;

  while (s > 0) {
    c = *b;
    if (c < '0') break;
    if (c > '9') break;
    value = value * 10 + c - '0';
    -- s;
    ++ b;
  }

  *size = s;
  *buff = b;
  return (value);
}

/*********************************************/
/* Decode date quadword into three longwords */
/*********************************************/

void oz_sys_datebin_decode (OZ_Datebin datebin, uLong datelongs[OZ_DATELONG_ELEMENTS])

{
  datelongs[OZ_DATELONG_FRACTION]  = datebin % OZ_TIMER_RESOLUTION;
  datebin /= OZ_TIMER_RESOLUTION;
  datelongs[OZ_DATELONG_SECOND]    = datebin % 86400;
  datebin /= 86400;
  datelongs[OZ_DATELONG_DAYNUMBER] = datebin;
}

/***********************************************/
/* Encode three longwords into a date quadword */
/***********************************************/

OZ_Datebin oz_sys_datebin_encode (const uLong datelongs[OZ_DATELONG_ELEMENTS])

{
  OZ_Datebin datebin;

  datebin  = datelongs[OZ_DATELONG_DAYNUMBER];
  datebin *= 86400;
  datebin += datelongs[OZ_DATELONG_SECOND];
  datebin *= OZ_TIMER_RESOLUTION;
  datebin += datelongs[OZ_DATELONG_FRACTION];
  return (datebin);
}

/*******************************/
/* Perform timezone conversion */
/*******************************/

OZ_Datebin oz_sys_datebin_tzconv (OZ_Datebin datebin, OZ_Datebin_tzconv tzconv, Long offset)

{
  uLong sts;

  switch (tzconv) {
    case OZ_DATEBIN_TZCONV_UTC2LCL: {
      oz_sys_tzconv (OZ_DATEBIN_TZCONV_UTC2LCL, datebin, 0, &datebin, 0, NULL);
      return (datebin);
    }
    case OZ_DATEBIN_TZCONV_LCL2UTC: {
      oz_sys_tzconv (OZ_DATEBIN_TZCONV_LCL2UTC, datebin, 0, &datebin, 0, NULL);
      return (datebin);
    }
    case OZ_DATEBIN_TZCONV_UTC2OFS: {
      return (datebin + OZ_TIMER_RESOLUTION * (Quad)offset);
    }
    case OZ_DATEBIN_TZCONV_OFS2UTC: {
      return (datebin - OZ_TIMER_RESOLUTION * (Quad)offset);
    }
    default: oz_crash ("oz_sys_datebin_tzconv: bad tzconv %d", tzconv);
  }
}

#if 0

int main ()

{
  uLong yyyymmdd, daynumber, i, j, sts, vmsdate[2];
  uWord numtim[7];

  vmsdate[0] = 0;
  vmsdate[1] = 0;

  for (i = 0; i < 0xffffffff; i += 20) {
    yyyymmdd = oz_sys_daynumber_decode (i);
#if 0
    if (i != 0) {
      sts = sys$numtim (numtim, vmsdate);
      if (!(sts & 1)) lib$stop (sts);
      if ((((yyyymmdd >> 24) * 100 + ((yyyymmdd >> 16) & 255)) != numtim[0]) 
       || (((yyyymmdd >> 8) & 255) != numtim[1]) 
       || ((yyyymmdd & 255) != numtim[2])) {
        printf ("i = %u, numtim = 0x%2.2x%2.2x%2.2x%2.2x, yyyymmdd = 0x%x\n", i, numtim[0] / 100, numtim[0] % 100, numtim[1], numtim[2], yyyymmdd);
        return (0);
      }
    }
#endif
    daynumber = oz_sys_daynumber_encode (yyyymmdd);
    if (daynumber != i) {
      printf ("i = %u, daynumber = %u, yyyymmdd = 0x%x\n", i, daynumber, yyyymmdd);
      return (0);
    }
    if (i % 10000 == 0) {
      printf ("%u:  %4.4u-%2.2u-%2.2u\n", daynumber, yyyymmdd >> 16, (yyyymmdd >> 8) & 255, yyyymmdd & 255);
    }
#if 0
    for (j = 0; j < 864*20; j ++) {
      vmsdate[0] += 1000000000;
      if (vmsdate[0] < 1000000000) vmsdate[1] ++;
    }
#endif
  }
  return (0);
}
#endif
