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

#include "ozone.h"
#include "oz_knl_hw.h"
#include "oz_knl_lowipl.h"
#include "oz_knl_sdata.h"

#define IPL_RESCHED 4
#define IPL_LOWIPL 5
#define IPL_DIAG 6

#define IPL_SOFTINT 7

static int hwintsenabled = 1;
static uLong currentsmplevel = 0;

static void diaginthand (void *dummy, OZ_Mchargs *mchargs);
static void lowiplinthand (void *dummy, OZ_Mchargs *mchargs);
static void reschedinthand (void *dummy, OZ_Mchargs *mchargs);
static uQuad getnewipl (uLong newlevel);
static void setnewipl (uLong newlevel);
static void diaginthand (void *dummy, OZ_Mchargs *mchargs);

/************************************************************************/
/*									*/
/*  Module initialization routine					*/
/*									*/
/*  On input, all interrupts are blocked				*/
/*  On return, only softints are blocked				*/
/*									*/
/************************************************************************/

void oz_hw_xxxproc_init (void)

{
  oz_s_cpucount  = 1;									// we only have one cpu
  oz_s_cpusavail = 1;									// and its number is zero
  oz_hwaxp_scb_setc (0x500 + IPL_RESCHED * 16, reschedinthand, NULL, NULL, NULL);	// set up resched handler
  oz_hwaxp_scb_setc (0x500 + IPL_LOWIPL  * 16, lowiplinthand,  NULL, NULL, NULL);	// set up lowipl handler
  oz_hwaxp_scb_setc (0x500 + IPL_DIAG    * 16, diaginthand,    NULL, NULL, NULL);	// set up diag mode handler
  currentsmplevel = OZ_SMPLOCK_SOFTINT;							// we are going to be at softint level
  OZ_HWAXP_MTPR_IPL (IPL_SOFTINT);							// we are now at softint level
}

void oz_hw_cpu_bootalts (Long cpucount)

{ }

/************************************************************************/
/*									*/
/*  Enable/Inhibit software interrupt delivery on the calling CPU	*/
/*									*/
/************************************************************************/

int oz_hw_cpu_setsoftint (int enb)

{
  uLong oldlevel;

  oldlevel = currentsmplevel;
  if (oldlevel > OZ_SMPLOCK_SOFTINT) oz_crash ("oz_hw_cpu_setsoftint: smplevel %u", oldlevel);

  if (enb) {
    if (oldlevel != OZ_SMPLOCK_NULL) {
      currentsmplevel = OZ_SMPLOCK_NULL;
      OZ_HWAXP_MTPR_IPL (0);
    }
  } else {
    if (oldlevel == OZ_SMPLOCK_NULL) {
      OZ_HWAXP_MTPR_IPL (IPL_SOFTINT);
      currentsmplevel = OZ_SMPLOCK_SOFTINT;
    }
  }

  return (oldlevel == OZ_SMPLOCK_NULL);
}

/************************************************************************/
/*									*/
/*  Return the SMP level of the calling CPU				*/
/*									*/
/************************************************************************/

uLong oz_hw_cpu_smplevel (void)

{
  return (currentsmplevel);
}

/************************************************************************/
/*									*/
/*  This routine may be called at any smplevel to cause the 		*/
/*  oz_knl_lowipl_handleint routine to be called on the given CPU	*/
/*  at softint level							*/
/*									*/
/************************************************************************/

void oz_hw_cpu_lowiplint (Long cpuidx)

{
  if (cpuidx != 0) oz_crash ("oz_hw_cpu_lowiplint: bad cpu number %d at %p", cpuidx, __builtin_return_address (0));
  OZ_HWAXP_MTPR_SIRR (IPL_LOWIPL);
}

static void lowiplinthand (void *dummy, OZ_Mchargs *mchargs)

{
  OZ_HWAXP_MTPR_IPL (IPL_SOFTINT);
  if (currentsmplevel != OZ_SMPLOCK_NULL) oz_crash ("oz_hw_cpu lowiplinthand: smplevel %u", currentsmplevel);
  currentsmplevel = OZ_SMPLOCK_SOFTINT;
  oz_knl_lowipl_handleint ();
  currentsmplevel = OZ_SMPLOCK_NULL;
}

/************************************************************************/
/*									*/
/*  This routine may be called at any smplevel to cause the 		*/
/*  oz_knl_thread_handleint routine to be called on the given CPU	*/
/*  at softint level							*/
/*									*/
/************************************************************************/

void oz_hw_cpu_reschedint (Long cpuidx)

{
  if (cpuidx != 0) oz_crash ("oz_hw_cpu_reschedint: bad cpu number %d at %p", cpuidx, __builtin_return_address (0));
  OZ_HWAXP_MTPR_SIRR (IPL_RESCHED);
}

static void reschedinthand (void *dummy, OZ_Mchargs *mchargs)

{
  OZ_HWAXP_MTPR_IPL (IPL_SOFTINT);
  if (currentsmplevel != OZ_SMPLOCK_NULL) oz_crash ("oz_hw_cpu lowiplinthand: smplevel %u", currentsmplevel);
  currentsmplevel = OZ_SMPLOCK_SOFTINT;
  oz_knl_thread_handleint ();
  currentsmplevel = OZ_SMPLOCK_NULL;
}

/************************************************************************/
/*									*/
/*  SMP lock handling routines						*/
/*									*/
/************************************************************************/

void oz_hw_smplock_init (int smplocksize, OZ_Smplock *smplock, uLong level)

{
  if (smplocksize != sizeof *smplock) oz_crash ("oz_hw_smplock_init: bad smplocksize %d", smplocksize);
  if (level > 0xFF) oz_crash ("oz_hw_smplock_init: bad smplocklevel 0x%X", level);
  smplock -> opaque = level + 0xFFFFFF00;		// opaque<07:00> = level
							// opaque<31:08> = cpu that has it locked
}

/* Lock an smplock, increasing level and IPL */

uLong oz_hw_smplock_wait (OZ_Smplock *smplock)

{
  uLong newlevel, oldconts, oldlevel;

  oldconts = smplock -> opaque;
  oldlevel = currentsmplevel;
  newlevel = oldconts & 0xFF;

  /* We can't be decreasing lock level */

  if (newlevel < oldlevel) {
    oz_crash ("oz_hw_smplock_wait: locking to a lower level: %2.2X -> %2.2X", oldlevel, newlevel);
  }

  /* If increasing level, new lock must not already be owned */

  if (newlevel > oldlevel) {
    if ((oldconts & 0xFFFFFF00) == 0) oz_crash ("oz_hw_smplock_wait: locking to higher level but already owned");
    setnewipl (newlevel);
    smplock -> opaque = currentsmplevel = newlevel;
  }

  /* If staying at same level, we must already own lock (no lateral locking) */

  else if ((oldconts & 0xFFFFFF00) != 0) oz_crash ("oz_hw_smplock_wait: relocking unowned lock");

  /* Return previous level */

  return (oldlevel);
}

/* Same thing, but we must already be at exactly the correct IPL and level must be increasing */

uLong oz_hwaxp_smplock_wait_atipl (OZ_Smplock *smplock)

{
  uLong newlevel, oldconts, oldlevel;

  oldconts = smplock -> opaque;
  oldlevel = currentsmplevel;
  newlevel = oldconts & 0xFF;

  /* We must be increasing lock level */

  if (newlevel <= oldlevel) {
    oz_crash ("oz_hw_smplock_wait_atipl: locking to same or lower level: %2.2X -> %2.2X", oldlevel, newlevel);
  }

  /* New lock must not already be owned */

  if ((oldconts & 0xFFFFFF00) == 0) oz_crash ("oz_hw_smplock_wait_atipl: lock already owned");
  if (getnewipl (newlevel) != OZ_HWAXP_MFPR_IPL ()) oz_crash ("oz_hw_smplock_wait_atipl: bad ipl");
  smplock -> opaque = currentsmplevel = newlevel;

  return (oldlevel);
}

/* Release an smplock, restoring previous level and IPL */

void oz_hw_smplock_clr (OZ_Smplock *smplock, uLong old)

{
  uLong oldconts;

  oldconts = smplock -> opaque;
  if ((oldconts & 0xFFFFFF00) != 0) oz_crash ("oz_hw_smplock_clr: releasing unowned lock");
  if ((oldconts & 0xFF) != currentsmplevel) oz_crash ("oz_hw_smplock_clr: releasing out of order");
  if (old > (oldconts & 0xFF)) oz_crash ("oz_hw_smplock_clr: releasing to higher level");
  if (old < (oldconts & 0xFF)) {
    smplock -> opaque = oldconts | 0xFFFFFF00;
    currentsmplevel = old;
    setnewipl (old);
  }
}

/* Same, but must be at exact IPL and we remain there on return */

void oz_hwaxp_smplock_clr_atipl (OZ_Smplock *smplock, uLong old)

{
  uLong oldconts;

  oldconts = smplock -> opaque;
  if ((oldconts & 0xFFFFFF00) != 0) oz_crash ("oz_hwaxp_smplock_clr_atipl: releasing unowned lock");
  if ((oldconts & 0xFF) != currentsmplevel) oz_crash ("oz_hwaxp_smplock_clr_atipl: releasing out of order");
  if (old >= (oldconts & 0xFF)) oz_crash ("oz_hwaxp_smplock_clr_atipl: releasing to higher or same level");
  if (getnewipl (currentsmplevel) != OZ_HWAXP_MFPR_IPL ()) oz_crash ("oz_hwaxp_smplock_clr_atipl: bad ipl");
  smplock -> opaque = oldconts | 0xFFFFFF00;
  currentsmplevel = old;
}

/* Get a lock's smp level */

uLong oz_hw_smplock_level (OZ_Smplock *smplock)

{
  return (smplock -> opaque & 0xFF);
}

/* Get CPU that owns a lock */

Long oz_hw_smplock_cpu (OZ_Smplock *smplock)

{
  return (smplock -> opaque >> 8);
}

/********************************************************/
/* Set hardware IPL based on the SMP lock level         */
/* Any low-level smplock blocks any softints            */
/* The IPL smplocks block their corresponding int level */
/* Any high-level smplocks block all interrupts         */
/********************************************************/

static uQuad getnewipl (uLong newlevel)

{
  uQuad newipl;

       if (!hwintsenabled) newipl = 31;
  else if (newlevel == OZ_SMPLOCK_NULL) newipl = 0;						// anything goes
  else if (newlevel <= OZ_SMPLOCK_LEVEL_IPLS + IPL_SOFTINT) newipl = IPL_SOFTINT;		// inhibit softints
  else if (newlevel <= OZ_SMPLOCK_LEVEL_IPLS + 31) newipl = newlevel - OZ_SMPLOCK_LEVEL_IPLS;	// inhibit some hw ints
  else newipl = 31;										// inhibit all hw ints

  return (newipl);
}

static void setnewipl (uLong newlevel)

{
  OZ_HWAXP_MTPR_IPL (getnewipl (newlevel));
}

/************************************************************************/
/*									*/
/*  Waitloop routines							*/
/*									*/
/************************************************************************/

void oz_hw_waitloop_init (void)

{
  OZ_HWAXP_MTPR_IPL (31);
  hwintsenabled = 0;
  currentsmplevel = OZ_SMPLOCK_NULL;
}

void oz_hw_waitloop_body (void)

{
  hwintsenabled = 1;
  OZ_HWAXP_MTPR_IPL (0);
  //?? get ill instr trap here?? // OZ_HWAXP_WTINT (0);
  OZ_HW_MB; //?? so just do this instead for a NOP
  OZ_HWAXP_MTPR_IPL (31);
  hwintsenabled = 0;
}

void oz_hw_waitloop_term ()

{
  currentsmplevel = OZ_SMPLOCK_SOFTINT;
  hwintsenabled = 1;
  OZ_HWAXP_MTPR_IPL (IPL_SOFTINT);
}

void oz_hw_waitloop_wake (Long cpuidx)

{ }

/************************************************************************/
/*									*/
/*  Enable/Disable hardware interrupt delivery				*/
/*									*/
/************************************************************************/

int oz_hw_cpu_sethwints (int enable)

{
  int wasenabled;

  wasenabled = hwintsenabled;

  if (enable != wasenabled) {				// see if making a change
    if (enable) {
      hwintsenabled = 1;				// enabling, say we're enabled
      OZ_HWAXP_MTPR_IPL (getnewipl (currentsmplevel));	// ... then lower ipl according to smplevel
    } else {
      OZ_HWAXP_MTPR_IPL (31);				// disabling, disable them
      hwintsenabled = 0;				// remember we're disabled in case an smplock is released
    }
  }

  return (wasenabled);					// anyway, return prior state
}

/************************************************************************/
/*									*/
/*  Update ASTSR on another CPU						*/
/*									*/
/*    Input:								*/
/*									*/
/*	R16 = the other CPU index					*/
/*	R17 = new ASTSR value						*/
/*									*/
/************************************************************************/

void oz_hwaxp_updaststate (Long cpuidx, uLong astsr)

{
  oz_crash ("oz_hwaxp_updaststate: uniprocessor only");
}

/************************************************************************/
/*									*/
/*  Halt the other cpu's, cause them to call oz_knl_debug_halted no 	*/
/*  matter what they are currently doing				*/
/*									*/
/************************************************************************/

void oz_hw_debug_halt (void)

{ }

void oz_hw_debug_watch (void *address)

{ }

/************************************************************************/
/*									*/
/*  Cause all cpus to enter diag mode					*/
/*									*/
/*  This routine is called at high ipl when control-shift-D is pressed.	*/
/*  It sets the softintpend<2> bit then softints the cpu.  The softint 	*/
/*  interrupt routine calls oz_knl_diag for every cpu as long as the 	*/
/*  diagmode flag is set.						*/
/*									*/
/************************************************************************/

void oz_hw_diag (void)

{
  OZ_HWAXP_MTPR_SIRR (IPL_DIAG);
}

static void diaginthand (void *dummy, OZ_Mchargs *mchargs)

{
  OZ_HWAXP_MTPR_IPL (IPL_SOFTINT);
  if (currentsmplevel != OZ_SMPLOCK_NULL) oz_crash ("oz_hw_cpu diaginthand: smplevel %u", currentsmplevel);
  currentsmplevel = OZ_SMPLOCK_SOFTINT;
  oz_knl_diag (0, 1, mchargs);
  currentsmplevel = OZ_SMPLOCK_NULL;
}
