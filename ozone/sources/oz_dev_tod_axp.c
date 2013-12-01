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
/*  Time-of-day routines						*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_dev_timer.h"
#include "oz_knl_hw.h"
#include "oz_knl_sdata.h"
#include "oz_knl_status.h"
#include "oz_sys_dateconv.h"

#define TIMER_APORT  0x70
#define TIMER_DPORT  0x71

#define TIMER_SECONDS     0x00
#define TIMER_MINUTES     0x02
#define TIMER_HOURS       0x04
#define TIMER_DAYOFWEEK   0x06
#define TIMER_DAYOFMONTH  0x07
#define TIMER_MONTH       0x08
#define TIMER_YEAR        0x09
#define TIMER_STATUS_A    0x0A
#define TIMER_STATUS_B    0x0B
#define TIMER_STATUS_C    0x0C
#define TIMER_STATUS_D    0x0D
#define TIMER_CENTURY     0x32	// seems to hold 0xFF

#define THIS_CENTURY 20	// bcdtobin (CMOS_READ (TIMER_CENTURY))

#define CMOS_READ(addr) ({ oz_dev_isa_outb ((addr), TIMER_APORT); oz_dev_isa_inb (TIMER_DPORT); })
#define CMOS_WRITE(val,addr) {  oz_dev_isa_outb ((addr), TIMER_APORT); oz_dev_isa_outb ((val), TIMER_DPORT); }

static OZ_Smplock smplock_tm;
OZ_Smplock *oz_hw_smplock_tm = &smplock_tm;

static int inbinary;				// zero=clock is in BCD, else=clock is in binary
static int initialized = 0;
static OZ_Datebin volatile basedatetime;	// datetime the cycle counter was zero
static OZ_Datebin timeisup = -1LL;		// when to call oz_knl_timer_timeisup
static uQuad cycounfrq;				// initially copied from hwrpb, adjusted since then
static uLong cycounfrq_scale;			// scale factor so cycounfrq is ok for getrate/setrate

static uQuad tod_init_begsec (void);
static uByte tod_init_readsec (void);
static uByte bcdtobin (uByte bcd);
static void timerinterrupt (void *dummy, OZ_Mchargs *mchargs);
static uQuad scale64x64s64 (uQuad n1, uQuad n2, uQuad d);

/************************************************************************/
/*									*/
/*  Initialize time-of-day routines					*/
/*									*/
/*  This is called before general OS init is performed as many of the 	*/
/*  OS init routines need to know the current time			*/
/*									*/
/************************************************************************/

void oz_dev_tod_init (void)

{
  uLong datelongs[OZ_DATELONG_ELEMENTS], day, hour, minute, month, second, year;
  uQuad ccf, scc;

  if (initialized) return;
  oz_knl_printk ("oz_dev_tod_init: initializing\n");

  memset (datelongs, 0, sizeof datelongs);

  /* Determine scaling required to allow the setrate to vary cycle counter frequency estimate by +/- 50%  */

  cycounfrq = oz_hwaxp_hwrpb -> cycounfrq;
  cycounfrq_scale = 0;
  for (ccf = cycounfrq; ccf >= 0x80000000; ccf /= 2) cycounfrq_scale ++;

  /* See if clock in BCD or binary */

  inbinary = ((CMOS_READ (TIMER_STATUS_B) & 4) != 0);

  /* Read the scc at beginning of a second according to CMOS clock */

  scc = tod_init_begsec ();

  /* Read current date/time out of CMOS clock and set boot date/time accordingly */

  year  = THIS_CENTURY * 100 + bcdtobin (CMOS_READ (TIMER_YEAR));
  month = bcdtobin (CMOS_READ (TIMER_MONTH));
  day   = bcdtobin (CMOS_READ (TIMER_DAYOFMONTH));

  oz_knl_printk ("oz_dev_tod_init*: year %u, month %u, day %u\n", year, month, day);

  datelongs[OZ_DATELONG_DAYNUMBER] = oz_sys_daynumber_encode ((year << 16) + (month << 8) + day);

  hour   = bcdtobin (CMOS_READ (TIMER_HOURS));
  minute = bcdtobin (CMOS_READ (TIMER_MINUTES));
  second = bcdtobin (CMOS_READ (TIMER_SECONDS));

  oz_knl_printk ("oz_dev_tod_init*: hour %u, minute %u, second %u\n", hour, minute, second);

  datelongs[OZ_DATELONG_SECOND] = (((hour * 60) + minute) * 60) + second;

  oz_s_boottime  = oz_sys_datebin_encode (datelongs);
  oz_s_boottime -= oz_ldr_paramblock.tz_offset_rtc * OZ_TIMER_RESOLUTION;

  /* Calculate basetime = what time it was when the SCC was zero */

  basedatetime  = oz_s_boottime;
  basedatetime -= scale64x64s64 (scc, OZ_TIMER_RESOLUTION, cycounfrq);

  oz_knl_printk ("oz_dev_tod_init*: basedatetime %##t UTC'\n", basedatetime);
  oz_knl_printk ("oz_dev_tod_init*:     boottime %##t UTC'\n", oz_s_boottime);

  /* Set up timer interrupt */

  oz_hw_smplock_init (sizeof smplock_tm, &smplock_tm, OZ_SMPLOCK_LEVEL_IPL22);
  oz_hwaxp_scb_setc (0x600, timerinterrupt, NULL, NULL, NULL);

  /* We're now initialized */

  initialized = 1;
}

/* Read system cycle counter at beginning of a second according to the RTC */

static uQuad tod_init_begsec (void)

{
  uByte sec1, sec2;
  uLong i;
  uQuad scc;

  sec1 = tod_init_readsec ();
  if (sec1 != 0xFF) {
    for (i = 100000000; i > 0; -- i) {
      sec2 = tod_init_readsec ();
      scc  = OZ_HWAXP_RSCC ();
      if (sec2 != sec1) goto done;
    }
    oz_knl_printk ("oz_hwaxp_tod_init: took too long to find beginning of second\n");
  }
  scc = OZ_HWAXP_RSCC ();
done:
  return (scc);
}

/* Read second making sure no update is in progress */

static uByte tod_init_readsec (void)

{
  Byte status;
  uByte second;
  uLong i;

  for (i = 100000000; i > 0; -- i) {
    status = CMOS_READ (TIMER_STATUS_A);
    if (status >= 0) {
      second = bcdtobin (CMOS_READ (TIMER_SECONDS));
      status = CMOS_READ (TIMER_STATUS_A);
      if (status >= 0) return (second);
    }
  }
  oz_knl_printk ("oz_hwaxp_tod_init: update bit stuck on\n");
  return (0xFF);
}

/* Convert bcd to binary */

static uByte bcdtobin (uByte bcd)

{
  if (inbinary) return (bcd);
  return (((bcd >> 4) * 10) + (bcd & 15));
}

/************************************************************************/
/*									*/
/*  Set the datebin of the next event					*/
/*  When this time is reached, call oz_knl_timer_timeisup		*/
/*									*/
/*    Input:								*/
/*									*/
/*	datebin = datebin of next event					*/
/*	smplock = tm							*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_timer_timeisup will be called at or just after the 	*/
/*	given datebin with the tm smplock set				*/
/*									*/
/************************************************************************/

void oz_hw_timer_setevent (OZ_Datebin datebin)

{
  timeisup = datebin;
}

static void timerinterrupt (void *dummy, OZ_Mchargs *mchargs)

{
  uLong tm;

  if (oz_hw_tod_getnow () >= timeisup) {	// see if the time is up
    tm = oz_hw_smplock_wait (&smplock_tm);	// if so, lock out interference
    timeisup = -1LL;				// don't call it twice
    oz_knl_timer_timeisup ();			// call it
    oz_hw_smplock_clr (&smplock_tm, tm);	// release lock
  }
}

/************************************************************************/
/*									*/
/*  Convert absolute iota time to system time				*/
/*									*/
/************************************************************************/

OZ_Datebin oz_hw_tod_aiota2sys (OZ_Iotatime iotatime)

{
  OZ_Datebin datebin;
  uQuad bdt_first, bdt_second, cyc_first, cyc_second;

  if (!initialized) return (0);

  bdt_first = basedatetime;
  OZ_HW_MB;
  cyc_first = cycounfrq;
  while (1) {
    OZ_HW_MB;
    bdt_second = basedatetime;
    OZ_HW_MB;
    cyc_second = cycounfrq;
    if ((bdt_second == bdt_first) && (cyc_second == cyc_first)) break;
    bdt_first = bdt_second;
    cyc_first = cyc_second;
  }

  datebin = scale64x64s64 (iotatime, OZ_TIMER_RESOLUTION, cyc_first) + bdt_first;
  return (datebin);
}

/************************************************************************/
/*									*/
/*  Set new current date/time						*/
/*									*/
/************************************************************************/

void oz_hw_tod_setnow (OZ_Datebin newdatebin, OZ_Datebin olddatebin)

{
  basedatetime += newdatebin - olddatebin;

  //?? update CMOS clock too
}

uLong oz_hw_tod_getrate (void)

{
  return (cycounfrq >> cycounfrq_scale);
}

void oz_hw_tod_setrate (uLong newrate)

{
  uQuad delta, newfreq, scc;

  scc = OZ_HWAXP_RSCC ();

  newfreq = newrate << cycounfrq_scale;

  delta = scale64x64s64 (scc, OZ_TIMER_RESOLUTION, cycounfrq) 
        - scale64x64s64 (scc, OZ_TIMER_RESOLUTION, newfreq);

  basedatetime += delta;
  OZ_HW_MB;
  cycounfrq = newfreq;
}

OZ_Iotatime oz_hw_tod_iotanow (void)

{
  return (OZ_HWAXP_RSCC ());
}

OZ_Datebin oz_hw_tod_diota2sys (OZ_Iotatime delta)

{
  return (scale64x64s64 (delta, OZ_TIMER_RESOLUTION, cycounfrq));
}

OZ_Iotatime oz_hw_tod_dsys2iota (OZ_Datebin delta)

{
  return (scale64x64s64 (delta, cycounfrq, OZ_TIMER_RESOLUTION));
}

/************************************************************************/
/*									*/
/*  Short waits via software timing loop				*/
/*									*/
/************************************************************************/

uLong oz_hw_stl_microwait (uLong microseconds, uLong (*entry) (void *param), void *param)

{
  uLong pcc_delta, pcc_now, pcc_start, sts;

  asm volatile ("rpcc %0" : "=r" (pcc_start));				// get current cycle count before doing anything else
  pcc_delta = microseconds * cycounfrq / 1000000;			// get the delta number of cycles to wait for
  if (entry == NULL) {
    sts = 0;
    do {
      asm volatile ("rpcc %0" : "=r" (pcc_now));			// see what cycle counter shows now
      pcc_now -= pcc_start;						// this is how many cycles since start (including wrap)
    } while (pcc_now < pcc_delta);					// see if we have waited long enough
  } else {
    do {
      sts = (*entry) (param);						// call test routine
      if (sts != 0) break;						// if it say's we're done, believe it
      asm volatile ("rpcc %0" : "=r" (pcc_now));			// see what cycle counter shows now
      pcc_now -= pcc_start;						// this is how many cycles since start (including wrap)
    } while (pcc_now < pcc_delta);					// see if we have waited long enough
  }
  return (sts);
}

void oz_hw_stl_nanowait (uLong nanoseconds)

{
  uLong pcc_delta, pcc_now, pcc_start;

  asm volatile ("rpcc %0" : "=r" (pcc_start));				// get current cycle count before doing anything else
  pcc_delta = nanoseconds * cycounfrq / 1000000000;			// get the delta number of cycles to wait for
  do {
    asm volatile ("rpcc %0" : "=r" (pcc_now));				// see what cycle counter shows now
    pcc_now -= pcc_start;						// this is how many cycles since start (including wrap)
  } while (pcc_now < pcc_delta);					// see if we have waited long enough
}

/************************************************************************/
/*									*/
/*  Scale a 64-bit unsigned by the quotient of two quads		*/
/*									*/
/*  Computes n1 * n2 / d						*/
/*									*/
/************************************************************************/

static uQuad scale64x64s64 (uQuad n1, uQuad n2, uQuad d)

{
  int i;
  uQuad hi_64, lo_64;

  asm ("umulh %1,%2,%0" : "=r" (hi_64) : "r" (n1), "r" (n2));
  asm ("mulq  %1,%2,%0" : "=r" (lo_64) : "r" (n1), "r" (n2));

  if (hi_64 >= d) OZ_HWAXP_GENTRAP (OZ_ARITHOVER);

  for (i = 64; -- i >= 0;) {
    hi_64 = (hi_64 << 1) + (lo_64 >> 63);
    lo_64 =  lo_64 << 1;
    if (hi_64 >= d) {
      hi_64 -= d;
      lo_64 ++;
    }
  }
  return (lo_64);
}
