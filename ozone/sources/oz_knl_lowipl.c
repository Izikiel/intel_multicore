//+++2002-08-17
//    Copyright (C) 2001,2002  Mike Rieker, Beverly, MA USA
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
//---2002-08-17

/************************************************************************/
/*									*/
/*  These routines are used by device driver routines to process stuff 	*/
/*  at low ipl.  Typically, they are called from interrupt service 	*/
/*  routines at high ipl.						*/
/*									*/
/************************************************************************/

#define _OZ_KNL_LOWIPL_C

#include "ozone.h"

#include "oz_knl_kmalloc.h"
#include "oz_knl_lowipl.h"
#include "oz_knl_objtype.h"
#include "oz_knl_printk.h"
#include "oz_knl_quota.h"
#include "oz_knl_sdata.h"

struct OZ_Lowipl { OZ_Objtype objtype;			/* OZ_OBJTYPE_LOWIPL */
                   struct OZ_Lowipl *next;		/* next in lowipls list */
                   void (*entry) (void *param, struct OZ_Lowipl *lowipl); /* routine entrypoint */
                   void *param;				/* routine parameter */
                 };

static Long volatile cpu_round_robin = 0;			/* last cpu used */
globaldef OZ_Lowipl *volatile oz_knl_lowipl_lowipls = NULL;	/* list of stuff to execute at low ipl */

static void softint_some_cpu (int thisoneok);

/************************************************************************/
/*									*/
/*  Allocate a 'lower ipl' struct for use later				*/
/*									*/
/*    Input:								*/
/*									*/
/*	smplock <= np							*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_lowipl_alloc = pointer to OZ_Lowipl struct		*/
/*									*/
/*    Note:								*/
/*									*/
/*	This routine is typically called in a device driver's start 	*/
/*	i/o routine when it knows it will be processing at interrupt 	*/
/*	level.  Then its interrupt routine can call oz_knl_lowipl_call 	*/
/*	to lower the ipl back when it wants to get ipl back down.	*/
/*									*/
/************************************************************************/

OZ_Lowipl *oz_knl_lowipl_alloc (void)

{
  OZ_Lowipl *lowipl;

  lowipl = OZ_KNL_NPPMALLOC (sizeof *lowipl);
  lowipl -> objtype = OZ_OBJTYPE_LOWIPL;
  return (lowipl);
}

/************************************************************************/
/*									*/
/*  Set up a routine to be called at a low ipl				*/
/*									*/
/*    Input:								*/
/*									*/
/*	lowipl = struct initialized by oz_knl_lowipl_alloc		*/
/*	entry  = entrypoint of routine to call				*/
/*	param  = parameter to pass to the routine			*/
/*	smplevel >= softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	routine will be called when ipl lowers below softint		*/
/*									*/
/*    Note:								*/
/*									*/
/*	this does not result in the freeing of the lowipl struct, 	*/
/*	making it available for re-use when the entry routine is 	*/
/*	finally called							*/
/*									*/
/************************************************************************/

void oz_knl_lowipl_call (OZ_Lowipl *lowipl, void (*entry) (void *param, OZ_Lowipl *lowipl), void *param)

{
  OZ_KNL_CHKOBJTYPE (lowipl, OZ_OBJTYPE_LOWIPL);	/* make sure we got what we expect */

  lowipl -> entry = entry;				/* save entrypoint and parameter */
  lowipl -> param = param;

  do lowipl -> next = oz_knl_lowipl_lowipls;		/* link it to list */
  while (!oz_hw_atomic_setif_ptr ((void *volatile *)&oz_knl_lowipl_lowipls, lowipl, lowipl -> next));
  softint_some_cpu (1);					/* interrupt some cpu to process it, even this one */
}

/************************************************************************/
/*									*/
/*  Lowipl struct is no longer needed					*/
/*									*/
/************************************************************************/

void oz_knl_lowipl_free (OZ_Lowipl *lowipl)

{
  OZ_KNL_CHKOBJTYPE (lowipl, OZ_OBJTYPE_LOWIPL);	/* make sure we got what we expect */
  OZ_KNL_NPPFREE (lowipl);				/* free it off */
}

/************************************************************************/
/*									*/
/*  This routine is called by the hardware layer as a result of a 	*/
/*  softint.								*/
/*									*/
/*    Input:								*/
/*									*/
/*	lowipls = list of routines to call				*/
/*									*/
/*	smplocks = none							*/
/*	softints = inhibited						*/
/*									*/
/*    Output:								*/
/*									*/
/*	all routines on lowipls list called				*/
/*									*/
/************************************************************************/

void oz_knl_lowipl_handleint (void)

{
  uLong hi;
  OZ_Lowipl *lowipl;
  OZ_Quota *quota;
  void (*entry) (void *param, OZ_Lowipl *lowipl);
  void *param;

  /* Stack and clear current thread's quota pointer          */
  /* Lowipl routine will have to set its own if it wants one */

  quota = oz_knl_quota_setcpudef (NULL);

  /* Check for stuff on the lowipls list */

  entry = NULL;
  param = NULL;
  while (1) {
    do lowipl = oz_knl_lowipl_lowipls;
    while ((lowipl != NULL) && !oz_hw_atomic_setif_ptr ((void *volatile *)&oz_knl_lowipl_lowipls, lowipl -> next, lowipl));
    if (lowipl == NULL) break;				/* stop if nothing there */
    entry = lowipl -> entry;
    param = lowipl -> param;
    if (oz_knl_lowipl_lowipls != NULL) softint_some_cpu (0); /* and if there is other stuff, maybe another cpu can do it */
    (*entry) (param, lowipl);				/* call it */
  }

  /* Restore thread's quota pointer */

  oz_knl_quota_setcpudef (quota);
}

/************************************************************************/
/*									*/
/*  There is some stuff on oz_knl_lowipl_lowipls to process, so 	*/
/*  softint some cpu to process it.  This causes some cpu to call 	*/
/*  oz_knl_lowipl_handleint at softint level.				*/
/*									*/
/*    Input:								*/
/*									*/
/*	thisoneok = 0 : don't softint this cpu, though			*/
/*	            1 : i'ts ok to sofint this cpu			*/
/*	smplevel >= softint						*/
/*									*/
/************************************************************************/

static void softint_some_cpu (int thisoneok)

{
  Long cpuidx, crr, thiscpuidx;

  if (thisoneok) {
    do {
      cpuidx = crr = cpu_round_robin;					// sample last cpu triggered by someone
      do if (-- cpuidx < 0) cpuidx = oz_s_cpucount - 1;			// decrement it with wrap
      while (!(oz_s_cpusavail & (1 << cpuidx)));			// repeat until we find a usable number
    } while (!oz_hw_atomic_setif_long (&cpu_round_robin, cpuidx, crr));	// write new value, but repeat if another cpu beat us to it
  } else {
    thiscpuidx = oz_hw_cpu_getcur ();					// don't do this cpu
    do {
      cpuidx = crr = cpu_round_robin;					// sample last cpu triggered by someone
      do {
        if (-- cpuidx < 0) cpuidx = oz_s_cpucount - 1;			// excrement wit rap
        if ((cpuidx != thiscpuidx) && (oz_s_cpusavail & (1 << cpuidx))) goto foundone;
      } while (cpuidx != crr);						// stop if we've tried them all
      return;
foundone:;
    } while (!oz_hw_atomic_setif_long (&cpu_round_robin, cpuidx, crr));	// write new value, but repeat if another cpu beat us to it
  }
  oz_hw_cpu_lowiplint (cpuidx);						// found a wackable cpu, wack it
}
