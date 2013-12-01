//+++2003-03-01
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
//---2003-03-01

/************************************************************************/
/*									*/
/*  Thread locking routines						*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_knl_status.h"
#include "oz_sys_condhand.h"
#include "oz_sys_event.h"
#include "oz_sys_thread.h"
#include "oz_sys_threadlock.h"

/************************************************************************/
/*									*/
/*  Wait for a threadlock to be available then lock it			*/
/*									*/
/*  This routine is complicated to aviod touching the event flag if 	*/
/*  there is no contention.						*/
/*									*/
/************************************************************************/

int oz_sys_threadlock_wait (OZ_Threadlock *threadlock)

{
  uLong aststs, sts;
  OZ_Handle h_event;

  /* Inhibit ast delivery - if an ast tried to allocate memory, we would deadlock */

  aststs = oz_sys_thread_setast (OZ_ASTMODE_INHIBIT);
  if ((aststs != OZ_FLAGWASSET) && (aststs != OZ_FLAGWASCLR)) oz_sys_condhand_signal (2, sts, 0);

  /* Make sure we have an event flag - if not, allocate one */

  if (threadlock -> h_event == 0) {
    while (oz_hw_atomic_or_long (&(threadlock -> flag), 1) & 1) {				/* test and set flag<00> */
      if (threadlock -> h_event != 0) goto getlock;						/* we didn't get it, if someone else set up lock, do normal stuff */
    }												/* if not, try again */
    if (threadlock -> h_event == 0) {								/* see if we still don't have an event flag */
      sts = oz_sys_event_create (OZ_PROCMODE_KNL, threadlock -> name, &h_event);		/* we don't, get one */
      if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
      threadlock -> h_event = h_event;
    }
    goto locked;										/* we're done, (flag<00> is still set) */
  }

  /* Set flag<00> leaving flag<01:31> alone.  If it was previously clear, we got the lock. */

getlock:
  while (oz_hw_atomic_or_long (&(threadlock -> flag), 1) & 1) {

    /* It was already set, presumably by some other thread */

    /* Increment number of waiting threads (in flag<01:31>), while leaving flag<00> alone */
    /* This tells the unlocking routine to set the event flag                             */

    oz_hw_atomic_inc_long (&(threadlock -> flag), 2);

    /* Now clear the event flag */

    sts = oz_sys_event_set (OZ_PROCMODE_KNL, threadlock -> h_event, 0, NULL);
    if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);

    /* Test and set flag<00> again, just in case the unlock routine was called sometime before we cleared the event flag */
  
    if (!(oz_hw_atomic_or_long (&(threadlock -> flag), 1) & 1)) {
      oz_hw_atomic_inc_long (&(threadlock -> flag), -2);
      break;
    }

    /* The flag<00> was still set by the other locker, so we wait for it to set the event flag     */
    /* The unlock routine should see that flag<01:31> is non-zero and know that someone is waiting */

    oz_sys_event_wait (OZ_PROCMODE_KNL, threadlock -> h_event, 1);

    /* Decrement the waiter count in flag<01:31>, leaving flag<00> alone */

    oz_hw_atomic_inc_long (&(threadlock -> flag), -2);

    /* Now start all over */
  }

  /* Return the ast status flag */

locked:
  return (aststs == OZ_FLAGWASSET);
}

/************************************************************************/
/*									*/
/*  Release lock and restore ast state					*/
/*									*/
/************************************************************************/

void oz_sys_threadlock_clr (OZ_Threadlock *threadlock, int restore)

{
  uLong sts;

  OZ_HW_MB;									/* make sure other cpus see writes done inside lock before unlocking */
  if (oz_hw_atomic_and_long (&(threadlock -> flag), ~1) & ~1) {			/* clear flag<00>, test flag<01:31> to see if someone was waiting */
    sts = oz_sys_event_set (OZ_PROCMODE_KNL, threadlock -> h_event, 1, NULL);	/* flag<01:31> was non-zero, set the event flag */
    if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
  }
  if (restore) oz_sys_thread_setast (OZ_ASTMODE_ENABLE);			/* maybe restore ast delivery */
}
