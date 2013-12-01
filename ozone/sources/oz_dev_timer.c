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
/*  This is a platform independent timer driver				*/
/*									*/
/*  It calls platform specific routines to perform the basic timing 	*/
/*  functions								*/
/*									*/
/************************************************************************/

#define _OZ_DEV_TIMER_C
#include "ozone.h"
#include "oz_dev_timer.h"
#include "oz_io_timer.h"
#include "oz_knl_hw.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_lowipl.h"
#include "oz_knl_objtype.h"
#include "oz_knl_printk.h"
#include "oz_knl_sdata.h"
#include "oz_knl_status.h"
#include "oz_knl_thread.h"

struct OZ_Timer { OZ_Objtype objtype;
                  OZ_Timer *next;
                  OZ_Datebin datebin;
                  void (*(entry)) (void *param, OZ_Timer *timer);
                  void *param;
                };

typedef struct Chnex Chnex;
typedef struct Iopex Iopex;

struct Iopex { Iopex *next;
               Iopex **prev;
               OZ_Ioop *ioop;
               OZ_Timer timer;
             };

struct Chnex { Iopex *queue;
             };
                 
static uLong oz_dev_timer_assign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode);
static void oz_dev_timer_abort (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Ioop *ioop, void *iopexv, OZ_Procmode procmode);
static uLong oz_dev_timer_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnex, OZ_Procmode procmode,
                                 OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap);

static const OZ_Devfunc oz_dev_timer_functable = { 0, sizeof (Chnex), sizeof (Iopex), 0, NULL, NULL, NULL, oz_dev_timer_assign, NULL, oz_dev_timer_abort, oz_dev_timer_start, NULL };

static int initialized = 0;
static OZ_Devclass *devclass;
static OZ_Devdriver *devdriver;
static OZ_Devunit *devunit;

static OZ_Timer *alltimers = NULL;
static OZ_Lowipl *timer_lowipl = NULL;

static Long intserv_entered = 0;
static Long intserv_exited  = 0;
static Long lowipl_entered  = 0;
static Long lowipl_exited   = 0;

static void iotimerisup (void *ioopv, OZ_Timer *timer);
static void timer_call (void *dummy, OZ_Lowipl *lowipl);

/************************************************************************/
/*									*/
/*  Boot-time initialization routine					*/
/*									*/
/************************************************************************/

void oz_dev_timer_init (void)

{
  if (!initialized) {
    oz_knl_printk ("oz_dev_timer_init\n");
    initialized  = 1;
    devclass     = oz_knl_devclass_create (OZ_IO_TIMER_CLASSNAME, OZ_IO_TIMER_BASE, OZ_IO_TIMER_MASK, "oz_dev_timer");
    devdriver    = oz_knl_devdriver_create (devclass, "oz_dev_timer");
    devunit      = oz_knl_devunit_create (devdriver, "timer", "generic timer", &oz_dev_timer_functable, 0, oz_s_secattr_tempdev);
    timer_lowipl = oz_knl_lowipl_alloc ();
    oz_knl_thread_timerinit ();
  }
}

/************************************************************************/
/*									*/
/*  An new channel is being assigned - clear the queue pointer		*/
/*									*/
/************************************************************************/

static uLong oz_dev_timer_assign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode)

{
  ((Chnex *)chnexv) -> queue = NULL;
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Abort an timer I/O request						*/
/*									*/
/************************************************************************/

static void oz_dev_timer_abort (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Ioop *ioop, void *iopexv, OZ_Procmode procmode)

{
  Chnex *chnex;
  Iopex *aborted, *iopex, **liopex, *niopex;
  uLong tm;

  chnex   = chnexv;
  aborted = NULL;
  tm = oz_hw_smplock_wait (oz_hw_smplock_tm);	/* lock database */
  for (liopex = &(chnex -> queue); (iopex = *liopex) != NULL;) {
    if (!oz_knl_ioabortok (iopex -> ioop, iochan, procmode, ioop) || !oz_knl_timer_remove (&(iopex -> timer))) {
      liopex = &(iopex -> next);		/* leave it as is, on to next */
    } else {
      niopex = iopex -> next;			/* unlink from chnex -> queue */
      if (niopex != NULL) niopex -> prev = liopex;
      *liopex = niopex;
      iopex -> next = aborted;			/* link to aborted */
      aborted = iopex;
    }
  }
  oz_hw_smplock_clr (oz_hw_smplock_tm, tm);	/* release database */

  while ((iopex = aborted) != NULL) {		/* abort all we found */
    aborted = iopex -> next;
    oz_knl_iodone (iopex -> ioop, OZ_ABORTED, NULL, NULL, NULL);
  }
}

/************************************************************************/
/*									*/
/*  Start performing a timer i/o function				*/
/*									*/
/************************************************************************/

static uLong oz_dev_timer_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode,
                                 OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap)

{
  Chnex *chnex;
  Iopex *iopex;
  uLong tm;
  OZ_IO_timer_waituntil timer_waituntil;

  chnex = chnexv;
  iopex = iopexv;

  switch (funcode) {
    case OZ_IO_TIMER_WAITUNTIL: {
      movc4 (as, ap, sizeof timer_waituntil, &timer_waituntil);
      iopex -> timer.objtype = OZ_OBJTYPE_TIMER;
      iopex -> timer.next    = &(iopex -> timer);
      iopex -> ioop = ioop;
      tm = oz_hw_smplock_wait (oz_hw_smplock_tm);
      iopex -> next = chnex -> queue;
      iopex -> prev = &(chnex -> queue);
      if (iopex -> next != NULL) iopex -> next -> prev = &(iopex -> next);
      chnex -> queue = iopex;
      oz_knl_timer_insert (&(iopex -> timer), timer_waituntil.datebin, iotimerisup, iopex);
      oz_hw_smplock_clr (oz_hw_smplock_tm, tm);
      return (OZ_STARTED);
    }

    default: {
      return (OZ_BADIOFUNC);
    }
  }
}

/* An I/O style timer has expired, call the completion routine */

static void iotimerisup (void *iopexv, OZ_Timer *timer)

{
  Iopex *iopex, *next, **prev;
  uLong tm;

  iopex = iopexv;

  tm = oz_hw_smplock_wait (oz_hw_smplock_tm);	/* lock database */
  next = iopex -> next;				/* remove from chnex -> queue */
  prev = iopex -> prev;
  if (next != NULL) next -> prev = prev;
  *prev = next;
  oz_hw_smplock_clr (oz_hw_smplock_tm, tm);	/* release database */

  oz_knl_iodone (iopex -> ioop, OZ_SUCCESS, NULL, NULL, NULL);
}

/************************************************************************/
/*									*/
/*  Allocate a timer struct for later use				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_timer_alloc = pointer to timer struct			*/
/*	              NULL = npp quota exceeded				*/
/*									*/
/************************************************************************/

OZ_Timer *oz_knl_timer_alloc (void)

{
  OZ_Timer *timer;

  /* Create a new timer queue entry */

  timer = OZ_KNL_NPPMALLOQ (sizeof *timer);
  if (timer != NULL) {
    timer -> objtype = OZ_OBJTYPE_TIMER;
    timer -> next    = timer;
    timer -> entry   = NULL;
    timer -> param   = NULL;
  }

  return (timer);
}

/************************************************************************/
/*									*/
/*  Insert a timer request in queue					*/
/*									*/
/*    Input:								*/
/*									*/
/*	timer   = timer struct allocated by oz_knl_timer_alloc		*/
/*	datebin = date/time the timer is to expire			*/
/*	entry   = callback routine entrypoint				*/
/*	param   = callback routine parameter				*/
/*									*/
/*	smplock <= tm							*/
/*									*/
/*    Output:								*/
/*									*/
/*	timer request queued, callback will happen at or just after 	*/
/*	given datebin							*/
/*									*/
/************************************************************************/

void oz_knl_timer_insert (OZ_Timer *timer, OZ_Datebin datebin, void (*entry) (void *param, OZ_Timer *timer), void *param)

{
  uLong tm;
  OZ_Timer **ltimer, *xtimer;

  OZ_KNL_CHKOBJTYPE (timer, OZ_OBJTYPE_TIMER);

  /* Lock timer queue */

  tm = oz_hw_smplock_wait (oz_hw_smplock_tm);
  oz_knl_timer_validate ();

  /* Make sure it is not in current queue */

  if (timer -> next != timer) {
    if (!oz_knl_timer_remove (timer)) oz_crash ("oz_knl_timer_insert: couldnt remove entry from queue");
  }

  /* Now that is is not in the queue, save the parameters */

  timer -> datebin = datebin;
  timer -> entry   = entry;
  timer -> param   = param;

  /* Insert new entry sorted by ascending datebin */

  for (ltimer = &alltimers; (xtimer = *ltimer) != NULL; ltimer = &(xtimer -> next)) {
    if (OZ_HW_DATEBIN_CMP (xtimer -> datebin, timer -> datebin) > 0) break;
  }
  *ltimer = timer;
  timer -> next = xtimer;

  /* If this one is the new top, set the next event's datebin = this timer's datebin */

  if (timer == alltimers) oz_hw_timer_setevent (timer -> datebin);

  /* Release lock */

  oz_knl_timer_validate ();
  oz_hw_smplock_clr (oz_hw_smplock_tm, tm);
}

/************************************************************************/
/*									*/
/*  Remove timer request from timer queue				*/
/*									*/
/*    Input:								*/
/*									*/
/*	timer = request to be removed					*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_timer_remove = 0 : request was not in queue		*/
/*	                      1 : request was in queue and removed	*/
/*									*/
/************************************************************************/

int oz_knl_timer_remove (OZ_Timer *timer)

{
  int rc;
  uLong tm;
  OZ_Timer **ltimer, *xtimer;

  OZ_KNL_CHKOBJTYPE (timer, OZ_OBJTYPE_TIMER);

  /* Lock the timer queue */

  tm = oz_hw_smplock_wait (oz_hw_smplock_tm);
  oz_knl_timer_validate ();

  /* Scan queue for given entry.  If found, remove it. */

  rc = 0;
  for (ltimer = &alltimers; (xtimer = *ltimer) != NULL; ltimer = &(xtimer -> next)) {
    if (xtimer == timer) {
      *ltimer = xtimer -> next;
      xtimer -> next = xtimer;
      rc = 1;
      break;
    }
  }

  /* Release lock and return 'removed from queue' flag */

  oz_knl_timer_validate ();
  oz_hw_smplock_clr (oz_hw_smplock_tm, tm);
  return (rc);
}

/************************************************************************/
/*									*/
/*  Free off a timer struct						*/
/*									*/
/*    Input:								*/
/*									*/
/*	timer = pointer to timer struct to free				*/
/*									*/
/************************************************************************/

void oz_knl_timer_free (OZ_Timer *timer)

{
  OZ_KNL_CHKOBJTYPE (timer, OZ_OBJTYPE_TIMER);
  oz_knl_timer_validate ();
  if (timer -> next != timer) oz_knl_timer_remove (timer);
  oz_knl_timer_validate ();
  OZ_KNL_NPPFREE (timer);
}

/************************************************************************/
/*									*/
/*  This routine is called at smplock_tm level by the hardware layer 	*/
/*  when the time is up							*/
/*									*/
/************************************************************************/

void oz_knl_timer_timeisup (void)

{
  intserv_entered ++;

  oz_knl_timer_validate ();

  /* Call the 'timer_call' routine at low ipl to process any completed requests */

  if (timer_lowipl != NULL) {
    oz_knl_lowipl_call (timer_lowipl, timer_call, NULL);
    timer_lowipl = NULL;
  }

  intserv_exited ++;
}

/* This routine is called at softint when there are completed entries to be processed. */
/* It calls the completion routine for each at softint level.                          */

static void timer_call (void *dummy, OZ_Lowipl *lowipl)

{
  uLong tm;
  OZ_Datebin now;
  OZ_Timer *timer;
  void (*entry) (void *param, OZ_Timer *timer);
  void *param;

  OZ_HW_ATOMIC_INCBY1_LONG (lowipl_entered);
  entry = NULL;							/* clear these for debugging (so the crash below will show NULL) */
  param = NULL;
  while (1) {
    tm = oz_hw_smplock_wait (oz_hw_smplock_tm);			/* acquire lock */
    if (tm != OZ_SMPLOCK_SOFTINT) oz_crash ("oz_dev_timer timer_call: smplevel %u (sb SOFTINT), last entry,param = %p,%p", tm, entry, param);
    oz_knl_timer_validate ();
    timer = alltimers;						/* see if there's anything in the queue */
    if (timer == NULL) break;
    now = oz_hw_tod_getnow ();					/* get current date/time */
    if (OZ_HW_DATEBIN_CMP (timer -> datebin, now) > 0) break;	/* see if there are any completed entries */
    alltimers = timer -> next;					/* unlink top completed timer entry */
    timer -> next = timer;					/* remember it is not in queue now */
    entry = timer -> entry;					/* save in case it gets freed the instant we release the smplock */
    param = timer -> param;
    if (lowipl != NULL) {					/* see if first time through the loop */
      if (alltimers == NULL) timer_lowipl = lowipl;		/* if there are no more timers, re-enable lowipl routine */
      else if (OZ_HW_DATEBIN_CMP (alltimers -> datebin, now) <= 0) { /* see if next timer has expired, too */
        oz_knl_lowipl_call (lowipl, timer_call, NULL);		/* if so, arm to call it (maybe on another CPU) */
								/* (also, if our 'entry' routine waits, this timer will get called */
      } else {
        timer_lowipl = lowipl;					/* not expired yet, reset the lowipl routine */
        oz_hw_timer_setevent (alltimers -> datebin);		/* set next event date/time */
      }
      lowipl = NULL;						/* only do this once as we have now disposed of lowipl */
    }
    oz_hw_smplock_clr (oz_hw_smplock_tm, tm);			/* release lock */
    (*entry) (param, timer);					/* call the completion routine */
								/* we have to be fancy above to set up all the timer stuff again before calling completion routine */
								/* ... just in case completion routine does something like start a timer of its own and wait for it */
  }

  if (lowipl != NULL) {
    timer_lowipl = lowipl;					/* make sure we do something with lowipl */
    if (alltimers != NULL) oz_hw_timer_setevent (alltimers -> datebin); /* and timer interrupt is armed */
  }
  oz_hw_smplock_clr (oz_hw_smplock_tm, tm);			/* release lock */
  OZ_HW_ATOMIC_INCBY1_LONG (lowipl_exited);
}

void oz_knl_timer_validate (void)

{
  uLong tm;
  OZ_Datebin last;
  OZ_Timer *timer;

  memset (&last, 0, sizeof last);
  tm = oz_hw_smplock_wait (oz_hw_smplock_tm);
  for (timer = alltimers; timer != NULL; timer = timer -> next) {
    if (!OZ_HW_WRITABLE (sizeof *timer, timer, OZ_PROCMODE_KNL)) {
      oz_knl_printk ("oz_knl_timer_validate: bad timer pointer %p (%p)", timer, oz_hw_getrtnadr (0));
      oz_hw_pte_print (timer);
      oz_crash ("oz_knl_timer_validate: crashing");
    }
    OZ_KNL_CHKOBJTYPE (timer, OZ_OBJTYPE_TIMER);
    if (!OZ_HW_READABLE (1, (void *)(timer -> entry), OZ_PROCMODE_KNL)) oz_crash ("oz_knl_timer_validate: timer %p bad entrypoint %p", timer, timer -> entry);
    if (timer -> param != NULL) {
      if (!OZ_HW_READABLE (1, timer -> param, OZ_PROCMODE_KNL)) oz_crash ("oz_knl_timer_validate: timer %p bad parameter %p", timer, timer -> param);
    }
    if (OZ_HW_DATEBIN_CMP (last, timer -> datebin) > 0) {
      oz_crash ("oz_knl_timer_validate: last %##t after next %##t", last, timer -> datebin);
    }
    last = timer -> datebin;
  }
  oz_hw_smplock_clr (oz_hw_smplock_tm, tm);
}

void oz_knl_timer_debug (void)

{
  uLong tm;
  OZ_Timer *timer;

  tm = oz_hw_smplock_wait (oz_hw_smplock_tm);
  oz_knl_printk ("oz_knl_timer_debug: timer_lowipl %p, intserv_entered %d, intserv_exited %d, lowipl_entered %d, lowipl_exited %d\n", 
                 timer_lowipl, intserv_entered, intserv_exited, lowipl_entered, lowipl_exited);
  for (timer = alltimers; timer != NULL; timer = timer -> next) {
    oz_knl_printk ("oz_knl_timer_debug: %p: %##t\n", timer, timer -> datebin);
  }
  oz_hw_smplock_clr (oz_hw_smplock_tm, tm);
}
