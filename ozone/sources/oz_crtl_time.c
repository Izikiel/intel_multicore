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
/*  Time related CRTL routines						*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_io_console.h"
#include "oz_io_timer.h"
#include "oz_knl_hw.h"
#include "oz_knl_sdata.h"
#include "oz_knl_status.h"
#include "oz_sys_condhand.h"
#include "oz_sys_dateconv.h"
#include "oz_sys_handle.h"
#include "oz_sys_io_fs.h"
#include "oz_sys_logname.h"
#include "oz_sys_process.h"

#include <errno.h>
#include <time.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/utsname.h>

#define EOZSYSERR 65535

#ifdef errno
#undef errno
#endif

globalref int errno;
globalref uLong errno_ozsts;

#define ALIAS(x) asm (" .globl __" #x "\n __" #x "=" #x );

static uLong basedaynumber = 0;

time_t time (time_t *t)

{
  uLong nowlongs[OZ_DATELONG_ELEMENTS];
  OZ_Datebin nowbin;
  time_t nowtime;

  if (basedaynumber == 0) basedaynumber = oz_sys_daynumber_encode ((1970 << 16) | (1 << 8) | 1);

  nowbin = oz_hw_tod_getnow ();
  oz_sys_datebin_decode (nowbin, nowlongs);
  nowlongs[OZ_DATELONG_DAYNUMBER] -= basedaynumber;
  nowtime = nowlongs[OZ_DATELONG_DAYNUMBER] * 86400 + nowlongs[OZ_DATELONG_SECOND];
  if (t != NULL) *t = nowtime;
  return (nowtime);
}

struct tm *localtime (const time_t *timep)

{
  uLong daynumber, yyyymmdd;
  time_t when;

  static struct tm xtm;

  if (basedaynumber == 0) basedaynumber = oz_sys_daynumber_encode ((1970 << 16) | (1 << 8) | 1);

  when = *timep;					/* get input value */
  memset (&xtm, 0, sizeof xtm);				/* clear result array */

  xtm.tm_sec  = when % 60; when /= 60;			/* get second number and remove it */
  xtm.tm_min  = when % 60; when /= 60;			/* get minitue number and remove it */
  xtm.tm_hour = when % 24; when /= 24;			/* get hour number and remove it */

  daynumber = when + basedaynumber;			/* get day number since our beginning of time */
  yyyymmdd  = oz_sys_daynumber_decode (daynumber);	/* convert to year, month, day */

  xtm.tm_mday =   yyyymmdd & 0xff;			/* get day of the month */
  xtm.tm_mon  = ((yyyymmdd >>  8) & 0xff) - 1;		/* get month of the year */
  xtm.tm_year =  (yyyymmdd >> 16) - 1900;		/* get year since 1900 */

  xtm.tm_wday = oz_sys_daynumber_weekday (daynumber);	/* get day of the week (0=Sunday, 1=Monday, ...) */

  yyyymmdd    = (yyyymmdd & 0xffff0000) + 0x00000101;	/* get daynumber at beginning of the year */
  daynumber  -= oz_sys_daynumber_encode (yyyymmdd);	/* get number of days since beginning of year (0=Jan 1) */
  xtm.tm_yday = daynumber;

  return (&xtm);
}

ALIAS (time)

/************************************************************************/
/*									*/
/*  Sleep for the given number of seconds				*/
/*									*/
/************************************************************************/

unsigned int sleep (unsigned int seconds)

{
  uLong datelongs[OZ_DATELONG_ELEMENTS], sts;
  OZ_Datebin delta, now;
  OZ_Handle h_timer;
  OZ_IO_timer_waituntil timer_waituntil;

  if (seconds == 0) return (0);

  sts = oz_sys_io_assign (OZ_PROCMODE_KNL, &h_timer, "timer", OZ_LOCKMODE_CW);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printerror ("oz_crtl_misc sleep: error %u assigning channel to timer\n", sts);
    return (seconds);
  }

  memset (datelongs, 0, sizeof datelongs);
  memset (&timer_waituntil, 0, sizeof timer_waituntil);
  datelongs[OZ_DATELONG_SECOND] = seconds;
  now = oz_hw_tod_getnow ();
  delta = oz_sys_datebin_encode (datelongs);
  OZ_HW_DATEBIN_ADD (timer_waituntil.datebin, now, delta);
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_timer, 0, OZ_IO_TIMER_WAITUNTIL, sizeof timer_waituntil, &timer_waituntil);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printerror ("oz_crtl_misc_sleep: error %u waiting for timer\n", sts);
    return (seconds);
  }

  oz_sys_handle_release (OZ_PROCMODE_KNL, h_timer);
  return (0);
}

void usleep (unsigned int usec)

{
  uLong datelongs[OZ_DATELONG_ELEMENTS], sts;
  OZ_Datebin delta, now;
  OZ_Handle h_timer;
  OZ_IO_timer_waituntil timer_waituntil;

  now = oz_hw_tod_getnow ();
  if (usec == 0) return;

  sts = oz_sys_io_assign (OZ_PROCMODE_KNL, &h_timer, "timer", OZ_LOCKMODE_CW);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printerror ("oz_crtl_misc usleep: error %u assigning channel to timer\n", sts);
    return;
  }

  memset (datelongs, 0, sizeof datelongs);
  memset (&timer_waituntil, 0, sizeof timer_waituntil);
#if (OZ_TIMER_RESOLUTION % 1000000) != 0
  error : statement below assumes OZ TIMER RESOLUTION is a multiple of 1000000
#endif
  datelongs[OZ_DATELONG_FRACTION] = (usec % 1000000) * (OZ_TIMER_RESOLUTION / 1000000);
  datelongs[OZ_DATELONG_SECOND]   = usec / 1000000;
  delta = oz_sys_datebin_encode (datelongs);
  OZ_HW_DATEBIN_ADD (timer_waituntil.datebin, now, delta);
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_timer, 0, OZ_IO_TIMER_WAITUNTIL, sizeof timer_waituntil, &timer_waituntil);
  oz_sys_handle_release (OZ_PROCMODE_KNL, h_timer);
  if (sts != OZ_SUCCESS) oz_sys_io_fs_printerror ("oz_crtl_misc usleep: error %u waiting for timer\n", sts);
}

int gettimeofday (struct timeval *tv, struct timezone *tz)

{
  uLong nowlongs[OZ_DATELONG_ELEMENTS];
  OZ_Datebin nowbin;
  time_t nowtime;

  if (basedaynumber == 0) basedaynumber = oz_sys_daynumber_encode ((1970 << 16) | (1 << 8) | 1);

  nowbin = oz_hw_tod_getnow ();
  oz_sys_datebin_decode (nowbin, nowlongs);
  nowlongs[OZ_DATELONG_DAYNUMBER] -= basedaynumber;
  nowtime = nowlongs[OZ_DATELONG_DAYNUMBER] * 86400 + nowlongs[OZ_DATELONG_SECOND];

  if (tv != NULL) {
    tv -> tv_sec = nowtime;
#if (OZ_TIMER_RESOLUTION % 1000000) != 0
    error : code assumes OZ TIMER RESOLUTION is multiple of 1000000
#endif
    tv -> tv_usec = nowlongs[OZ_DATELONG_FRACTION] / (OZ_TIMER_RESOLUTION / 1000000);
  }
  if (tz != NULL) memset (tz, 0, sizeof *tz);
  return (0);
}

struct tm *gmtime_r (const time_t *timep, struct tm *tp)

{
  uLong daynumber, yyyymmdd;
  time_t when;

  if (basedaynumber == 0) basedaynumber = oz_sys_daynumber_encode ((1970 << 16) | (1 << 8) | 1);

  when = *timep;					/* get input value */
  memset (tp, 0, sizeof *tp);				/* clear result array */

  tp -> tm_sec  = when % 60; when /= 60;		/* get second number and remove it */
  tp -> tm_min  = when % 60; when /= 60;		/* get minitue number and remove it */
  tp -> tm_hour = when % 24; when /= 24;		/* get hour number and remove it */

  daynumber = when + basedaynumber;			/* get day number since our beginning of time */
  yyyymmdd  = oz_sys_daynumber_decode (daynumber);	/* convert to year, month, day */

  tp -> tm_mday =   yyyymmdd & 0xff;			/* get day of the month */
  tp -> tm_mon  = ((yyyymmdd >>  8) & 0xff) - 1;	/* get month of the year */
  tp -> tm_year =  (yyyymmdd >> 16) - 1900;		/* get year since 1900 */

  tp -> tm_wday = oz_sys_daynumber_weekday (daynumber);	/* get day of the week (0=Sunday, 1=Monday, ...) */

  yyyymmdd    = (yyyymmdd & 0xffff0000) + 0x00000101;	/* get daynumber at beginning of the year */
  daynumber  -= oz_sys_daynumber_encode (yyyymmdd);	/* get number of days since beginning of year (0=Jan 1) */
  tp -> tm_yday = daynumber;

  return (tp);
}
