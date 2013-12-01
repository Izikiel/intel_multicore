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
/*  These routines lock a resource for exclusive or protected-read 	*/
/*  modes using an event flag and an atomic long.  They are written 	*/
/*  to use the event flag only in cases where there is contention 	*/
/*  for efficiency.							*/
/*									*/
/************************************************************************/

#define _OZ_KNL_THREADLOCK_C

#include "ozone.h"
#include "oz_knl_event.h"
#include "oz_knl_hw.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_objtype.h"
#include "oz_knl_status.h"
#include "oz_knl_threadlock.h"

struct OZ_Threadlock { OZ_Objtype objtype;	/* OZ_OBJTYPE_THREADLOCK */
                       volatile Long flag;	/* see FLAG_... below */
                       volatile Long check;	/* <00> = set if there is a writer */
						/* <01:30> = number of readers */
						/* (debugging double-check only) */
                       OZ_Event *event;		/* event flag used for waiting */
                     };

#define FLAG_V_READC (0x00000001)	/* the number of threads that have READ access to the resource */
#define FLAG_M_READC (0x00007FFF)

#define FLAG_M_EXBIT (0x00008000)	/* if bit is set, some thread has EXclusive access to the resource */

#define FLAG_V_WAITC (0x00010000)	/* number of threads that are waiting for event flag */
#define FLAG_M_WAITC (0xFFFF0000)

/************************************************************************/
/*									*/
/*  Create a thread lock						*/
/*									*/
/************************************************************************/

uLong oz_knl_threadlock_create (const char *name, OZ_Threadlock **threadlock_r)

{
  OZ_Threadlock *threadlock;
  uLong sts;

  threadlock = OZ_KNL_NPPMALLOQ (sizeof *threadlock);
  if (threadlock == NULL) return (OZ_EXQUOTANPP);
  threadlock -> objtype = OZ_OBJTYPE_THREADLOCK;
  threadlock -> flag    = 0;
  threadlock -> check   = 0;
  sts = oz_knl_event_create (strlen (name), name, NULL, &(threadlock -> event));
  if (sts != OZ_SUCCESS) OZ_KNL_NPPFREE (threadlock);
  else *threadlock_r = threadlock;
  return (sts);
}

/************************************************************************/
/*									*/
/*  Delete threadlock							*/
/*									*/
/************************************************************************/

void oz_knl_threadlock_delete (OZ_Threadlock *threadlock)

{
  OZ_KNL_CHKOBJTYPE (threadlock, OZ_OBJTYPE_THREADLOCK);
  oz_knl_event_increfc (threadlock -> event, -1);
  OZ_KNL_NPPFREE (threadlock);
}

/************************************************************************/
/*									*/
/*  Lock resource for read/write access.  This basically sets 		*/
/*  flag<EXBIT> making sure it and flag<READC> were both clear.  	*/
/*									*/
/*  Having EXBIT set will prevent READC from being incremented (by the 	*/
/*  lock_pr routine).							*/
/*									*/
/************************************************************************/

void oz_knl_threadlock_ex (OZ_Threadlock *threadlock)

{
  Long oldflag, waitinc;

  OZ_KNL_CHKOBJTYPE (threadlock, OZ_OBJTYPE_THREADLOCK);

  waitinc = 0;												// haven't waited yet
top:
  oldflag = threadlock -> flag;										// sample flag
  if ((oldflag & (FLAG_M_EXBIT | FLAG_M_READC)) != 0) {							// see if it's in use
    oz_knl_event_set (threadlock -> event, 0);								// if so, prepare to wait
    if (!oz_hw_atomic_setif_long (&(threadlock -> flag), oldflag - waitinc + FLAG_V_WAITC, oldflag)) goto top; // say we're waiting
    waitinc = FLAG_V_WAITC;										// remember we waited
    oz_knl_event_waitone (threadlock -> event);								// wait
    goto top;												// try it all again
  }
  if (!oz_hw_atomic_setif_long (&(threadlock -> flag), oldflag - waitinc + FLAG_M_EXBIT, oldflag)) goto top; // not in use, set EX
}

/* Convert from exclusive to protected-read mode */

void oz_knl_threadlock_ex_pr (OZ_Threadlock *threadlock)

{
  Long oldflag;

  OZ_KNL_CHKOBJTYPE (threadlock, OZ_OBJTYPE_THREADLOCK);
  do {
    oldflag = threadlock -> flag;
    if (!(oldflag & FLAG_M_EXBIT)) oz_crash ("oz_knl_threadlock_ex_pr: exbit not set");
  } while (!oz_hw_atomic_setif_long (&(threadlock -> flag), oldflag - FLAG_M_EXBIT + FLAG_V_READC, oldflag));
  if (oldflag & FLAG_M_WAITC) oz_knl_event_set (threadlock -> event, 1);
}

/* Release exclusive mode */

void oz_knl_threadunlk_ex (OZ_Threadlock *threadlock)

{
  Long oldflag;

  OZ_KNL_CHKOBJTYPE (threadlock, OZ_OBJTYPE_THREADLOCK);
  do {
    oldflag = threadlock -> flag;
    if (!(oldflag & FLAG_M_EXBIT)) oz_crash ("oz_knl_threadunlk_ex: exbit not set");
  } while (!oz_hw_atomic_setif_long (&(threadlock -> flag), oldflag - FLAG_M_EXBIT, oldflag));
  if (oldflag & FLAG_M_WAITC) oz_knl_event_set (threadlock -> event, 1);
}

/************************************************************************/
/*									*/
/*  Lock resource for read-only access.  This basically increments 	*/
/*  flag<READC> and makes sure flag<EXBIT> was clear.			*/
/*									*/
/*  Having a non-zero READC will prevent EXBIT from being set (by the 	*/
/*  lock_ex routine).							*/
/*									*/
/************************************************************************/

void oz_knl_threadlock_pr (OZ_Threadlock *threadlock)

{
  Long oldflag, waitinc;

  OZ_KNL_CHKOBJTYPE (threadlock, OZ_OBJTYPE_THREADLOCK);

  waitinc = 0;												// haven't waited yet
top:
  oldflag = threadlock -> flag;										// sample flag
  if ((oldflag & FLAG_M_EXBIT) || ((oldflag & FLAG_M_READC) == FLAG_M_READC)) {				// see if it's in use
    oz_knl_event_set (threadlock -> event, 0);								// if so, prepare to wait
    if (!oz_hw_atomic_setif_long (&(threadlock -> flag), oldflag - waitinc + FLAG_V_WAITC, oldflag)) goto top; // say we're waiting
    waitinc = FLAG_V_WAITC;										// remember we waited
    oz_knl_event_waitone (threadlock -> event);								// wait
    goto top;												// try it all again
  }
  if (!oz_hw_atomic_setif_long (&(threadlock -> flag), oldflag - waitinc + FLAG_V_READC, oldflag)) goto top; // not in use, inc PR
}

/* Release protected-read mode */

void oz_knl_threadunlk_pr (OZ_Threadlock *threadlock)

{
  Long oldflag;

  OZ_KNL_CHKOBJTYPE (threadlock, OZ_OBJTYPE_THREADLOCK);
  do {
    oldflag = threadlock -> flag;
    if ((oldflag & FLAG_M_READC) == 0) oz_crash ("oz_knl_threadunlk_ex: pr count zero");
  } while (!oz_hw_atomic_setif_long (&(threadlock -> flag), oldflag - FLAG_V_READC, oldflag));
  if (oldflag & FLAG_M_WAITC) oz_knl_event_set (threadlock -> event, 1);
}
