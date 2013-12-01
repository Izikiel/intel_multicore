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
/*  Event flags are integer values that a thread can wait for a 	*/
/*  positive value (ie, thread will wait for value > 0)			*/
/*									*/
/************************************************************************/

#define _OZ_KNL_EVENT_C

#include "ozone.h"
#include "oz_dev_timer.h"
#include "oz_knl_event.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_lowipl.h"
#include "oz_knl_objtype.h"
#include "oz_knl_sdata.h"
#include "oz_knl_security.h"
#include "oz_knl_status.h"
#include "oz_knl_thread.h"

/* Timer states */

#define EVENT_TIMERSTATE_DEAD 0
#define EVENT_TIMERSTATE_IDLE 1
#define EVENT_TIMERSTATE_QUED 2
#define EVENT_TIMERSTATE_KILL 3

/* Callback blocks */

typedef struct Evcb Evcb;
struct Evcb { Evcb *next;
              void (*entry) (void *param, OZ_Event *event);
              void *param;
            };

/* Event block */

struct OZ_Event { OZ_Objtype objtype;		/* OZ_OBJTYPE_EVENT */
                  Long value;			/* <=0 : wait; >0 : don't wait */
                  Long refcount;		/* reference count = incremented for each pointer to the event */
                  Evcb *evcbs;			/* callback queue, locked by smplock_ev */
                  OZ_Secattr *secattr;		/* who can access this event flag */
                  OZ_Eventlist *eventlists;	/* list of threads that are waiting for this event flag */
                  OZ_Event *hinxt;		/* pointer to next on highiplq */
                  int onhiq;			/* set if currently linked to highiplq */
                  volatile Long timerstate;	/* state of interval timer */
                  OZ_Timer *timer;		/* pointer to interval timer struct */
                  OZ_Datebin interval;		/* interval */
                  OZ_Datebin nextwhen;		/* when the timer is going to expire next */
                  OZ_Smplock smplock_ev;	/* event flag's state lock */
                  char name[OZ_EVENT_NAMESIZE]; /* null-terminated name string */
                };

static OZ_Event  *highiplq_head;		/* list of all events set at high ipl */
static OZ_Event **highiplq_tail;
static OZ_Lowipl *highiplq_lowipl;

static void event_timer (void *eventv, OZ_Timer *timer);
static void wakethreads (OZ_Event *event);
static void put_on_highiplq (OZ_Event *event);
static void highiplq_proc (void *dummy, OZ_Lowipl *lowipl);
static void callcallbacks (OZ_Event *event);

/************************************************************************/
/*									*/
/*  Initialize event flag module					*/
/*									*/
/************************************************************************/

void oz_knl_event_init (void)

{
  highiplq_head   = NULL;
  highiplq_tail   = &highiplq_head;
  highiplq_lowipl = oz_knl_lowipl_alloc ();
}

/************************************************************************/
/*									*/
/*  Create an event flag						*/
/*									*/
/*    Input:								*/
/*									*/
/*	name = name string						*/
/*	secattr = event flag's security attributes			*/
/*	          controls what threads can access the event flag	*/
/*	          NULL : only accessible from kernel mode		*/
/*	smp lock <= np							*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_event_create = OZ_SUCCESS : event flag created		*/
/*	                            else : error status			*/
/*	*event_r = pointer to event flag object				*/
/*									*/
/*    Note:								*/
/*									*/
/*	the caller must call oz_knl_event_increfc (event, -1) when 	*/
/*	done with the event block pointer				*/
/*									*/
/*	the event's value is initialized to zero			*/
/*									*/
/*	the secattr's reference count is incremented			*/
/*									*/
/************************************************************************/

uLong oz_knl_event_create (int name_l, const char *name, OZ_Secattr *secattr, OZ_Event **event_r)

{
  OZ_Event *event;

  event = OZ_KNL_NPPMALLOQ (sizeof *event);					/* allocate some memory for it */
  if (event == NULL) return (OZ_EXQUOTANPP);					/* return failure status */
  memset (event, 0, sizeof *event);
  event -> objtype  = OZ_OBJTYPE_EVENT;						/* set up object type */
  event -> refcount = 1;							/* only the caller refs it initially */
  event -> secattr  = secattr;							/* save its security attributes */
  oz_knl_secattr_increfc (secattr, 1);						/* secattrs are referenced by one more thing now */
  oz_hw_smplock_init (sizeof event -> smplock_ev, &(event -> smplock_ev), OZ_SMPLOCK_LEVEL_EV); /* init the smplock */
  if (name_l > sizeof event -> name - 1) name_l = sizeof event -> name - 1;	/* make sure name string isn't too long */
  strncpy (event -> name, name, name_l);

  *event_r = event;
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Rename an event flag						*/
/*									*/
/************************************************************************/

void oz_knl_event_rename (OZ_Event *event, int name_l, const char *name)

{
  OZ_KNL_CHKOBJTYPE (event, OZ_OBJTYPE_EVENT);
  if (name_l >= sizeof event -> name) name_l = sizeof event -> name - 1;
  name_l = strnlen (name, name_l);
  movc4 (name_l, name, sizeof event -> name, event -> name);
}

/************************************************************************/
/*									*/
/*  Set event flag timer interval.  Event flag gets incremented for 	*/
/*  each interval.							*/
/*									*/
/*    Input:								*/
/*									*/
/*	event = pointer to event flag					*/
/*	interval = timer interval					*/
/*	           or 0 to shut it off					*/
/*	basetime = time to start intervals at				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_event_setimint = OZ_SUCCESS : interval set		*/
/*	                              else : error status		*/
/*									*/
/************************************************************************/

uLong oz_knl_event_setimint (OZ_Event *event, OZ_Datebin interval, OZ_Datebin basetime)

{
  Long oldstate;
  OZ_Datebin now, when;
  OZ_Timer *timer;

  OZ_KNL_CHKOBJTYPE (event, OZ_OBJTYPE_EVENT);

  while ((timer = event -> timer) == NULL) {				// see if event flag already has a timer defined
    if (interval == 0) return (OZ_SUCCESS);				// no, and we're clearing it, we're done
    timer = oz_knl_timer_alloc ();					// ok, allocate a new one
    if (timer == NULL) return (OZ_EXQUOTANPP);				// barf if no pool
    if (oz_hw_atomic_setif_ptr (&(event -> timer), timer, NULL)) break;	// save the pointer
    oz_knl_timer_free (timer);						// repeat if someone else beat us to it
  }

  event -> interval = interval;						// save the new interval
  event -> nextwhen = interval;						// this is when the next expiration happens
  if (interval != 0) {

    // Set state indicating timer is queued

    oldstate = oz_hw_atomic_set_long (&(event -> timerstate), EVENT_TIMERSTATE_QUED);

    // Shouldn't be in KILL state because that only happens when refcount goes zero

    if ((oldstate != EVENT_TIMERSTATE_DEAD) && (oldstate != EVENT_TIMERSTATE_IDLE) && (oldstate != EVENT_TIMERSTATE_QUED)) {
      oz_crash ("oz_knl_event_setimint: bad event %p -> timerstate %d", event, event -> timerstate);
    }

    // Qeueue timer

    event -> nextwhen += basetime;
    oz_knl_timer_insert (timer, event -> nextwhen, event_timer, event);
  }

  return (OZ_SUCCESS);
}

static void event_timer (void *eventv, OZ_Timer *timer)

{
  OZ_Datebin now;
  OZ_Event *event;
  uLong inc;

  event = eventv;
again:
  switch (event -> timerstate) {
    case EVENT_TIMERSTATE_QUED: {					// normal running state:
      if (event -> interval != 0) {
        now = oz_hw_tod_getnow ();					// see how many intervals have elapsed
        inc = (now + event -> interval - event -> nextwhen) / event -> interval;
        oz_knl_event_inc (event, inc);					// increment event flag
        event -> nextwhen += inc * event -> interval;			// increment interval for next one
        oz_knl_timer_insert (timer, event -> nextwhen, event_timer, event);
      } else {
        if (!oz_hw_atomic_setif_long (&(event -> timerstate), EVENT_TIMERSTATE_IDLE, EVENT_TIMERSTATE_QUED)) goto again;
      }
      break;
    }
    case EVENT_TIMERSTATE_KILL: {					// event refcount zero:
      oz_knl_timer_free (timer);					// free off timer
      OZ_KNL_NPPFREE (event);						// free off event flag
      break;
    }

    // Shouldn't ever be IDLE because that means the timer is not queued
    // Shouldn't ever be DEAD because that means no timer was ever even allocated

    default: oz_crash ("oz_knl_event timer_ast: bad event %p -> timerstate %d", event, event -> timerstate);
  }
}

/* Get what the current timer interval is */

OZ_Datebin oz_knl_event_getimint (OZ_Event *event)

{
  return (event -> interval);
}

/* Get when the timer will next expire */

OZ_Datebin oz_knl_event_getimnxt (OZ_Event *event)

{
  return (event -> nextwhen);
}

/************************************************************************/
/*									*/
/*  Increment event flag value						*/
/*									*/
/*    Input:								*/
/*									*/
/*	event = pointer to event flag to set				*/
/*	value = value to incremnt the event flag by			*/
/*									*/
/*	smplock level <= hi						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_event_inc = previous value					*/
/*	if value > 0, threads waiting for the flag have been woken	*/
/*									*/
/************************************************************************/

Long oz_knl_event_inc (OZ_Event *event, Long value)

{
  Long new, old;

  OZ_KNL_CHKOBJTYPE (event, OZ_OBJTYPE_EVENT);

  new = oz_hw_atomic_inc_long (&(event -> value), value);	/* increment event flag value */
  old = new - value;						/* compute what the old value was */
  if ((new > 0) && (old <= 0) && ((event -> eventlists != NULL) || (event -> evcbs != NULL))) wakethreads (event);
  return (old);							/* return old value */
}

/************************************************************************/
/*									*/
/*  Set event flag value						*/
/*									*/
/*    Input:								*/
/*									*/
/*	event = pointer to event flag to set				*/
/*	value = new value for the event flag				*/
/*									*/
/*	smplock level <= hi						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_event_set = previous value					*/
/*	if value > 0, threads waiting for the flag have been woken	*/
/*									*/
/************************************************************************/

Long oz_knl_event_set (OZ_Event *event, Long value)

{
  Long old;

  OZ_KNL_CHKOBJTYPE (event, OZ_OBJTYPE_EVENT);

  old = oz_hw_atomic_set_long (&(event -> value), value);	/* set new event flag value */
  if ((value > 0) && (old <= 0) && ((event -> eventlists != NULL) || (event -> evcbs != NULL))) wakethreads (event);
  return (old);							/* return old value */
}

/************************************************************************/
/*									*/
/*  An event flag has transitioned from .le. 0 to .gt. 0, and so any 	*/
/*  threads waiting for it need to be woken.  Also call callbacks 	*/
/*  that are queued to the event flag.					*/
/*									*/
/*    Input:								*/
/*									*/
/*	event = event flag that got set (it may be clear again, but we 	*/
/*	        don't care, we will wake the threads anyway)		*/
/*	smplevel <= hi							*/
/*									*/
/*    Output:								*/
/*									*/
/*	all threads that were waiting are woken and all callbacks are 	*/
/*	called								*/
/*									*/
/************************************************************************/

static void wakethreads (OZ_Event *event)

{
  OZ_Eventlist *eventlist;
  uLong ev;

  /* If we're above event smplevel, use an lowipl routine to set the flag when smplevel lowers               */
  /* Also, if we're at event smplevel, assume it's because some other event is locked, so do it the hard way */
  /* And if there are any callbacks queued, we have to be at softint level to call them                      */

  ev = oz_hw_cpu_smplevel ();						// see if smplevel is too high to process directly
  if ((ev >= OZ_SMPLOCK_LEVEL_EV) || ((event -> evcbs != NULL) && (ev != OZ_SMPLOCK_SOFTINT))) {
    put_on_highiplq (event);
  }

  /* It's ok to process it directly */

  else {

    /* Wake threads waiting for the event flag to be set */

    ev = oz_hw_smplock_wait (&(event -> smplock_ev));			// lock list of threads waiting for this event flag
    for (eventlist = event -> eventlists; eventlist != NULL; eventlist = eventlist -> neventlist) { // loop through the list
      oz_knl_thread_wakewev (eventlist -> thread, OZ_FLAGWASCLR);	// wake the thread if not already
    }
    oz_hw_smplock_clr (&(event -> smplock_ev), ev);			// unlock the list, the woken threads can remove themselves now

    /* Dequeue all callbacks.  If we're not at softint and there are callbacks queued, it must be because they were */
    /* queued after we did the above check.  So if the event flag is still set, the queuing routine should see this */
    /* and do its own dequeue.  If it is now clear, then the callbacks will have to wait until it gets set again.   */

    if (ev == OZ_SMPLOCK_SOFTINT) callcallbacks (event);		// have to be at softint to call the callbacks
  }
}

/* Event needs to be processed at softint level */

static void put_on_highiplq (OZ_Event *event)

{
  uLong hi;

  hi = oz_hw_smplock_wait (&oz_s_smplock_hi);				// lock the high-ipl queue
  if (!(event -> onhiq)) {						// see if this event flag is already on the queue
    event -> hinxt = NULL;						// if not, link it on the queue
    *highiplq_tail = event;
    highiplq_tail  = &(event -> hinxt);
    event -> onhiq = 1;							// ... and remember it is on the queue now
    OZ_HW_ATOMIC_INCBY1_LONG (event -> refcount);			// don't let it free off while it's on the queue
    if (highiplq_lowipl != NULL) {					// see if lowipl routine is running somewhere
      oz_knl_lowipl_call (highiplq_lowipl, highiplq_proc, NULL);	// if not, start it going
      highiplq_lowipl = NULL;						// ... and remember it is running somewhere now
    }
  }
  oz_hw_smplock_clr (&oz_s_smplock_hi, hi);				// release high-ipl queue
}

/* Wakethreads was called at too high an smplevel, so this lowipl routine calls it at softint level */

static void highiplq_proc (void *dummy, OZ_Lowipl *lowipl)

{
  OZ_Event *event, *hievents;
  uLong hi;

  hi = oz_hw_smplock_wait (&oz_s_smplock_hi);	// lock the queue of high-ipl events
  hievents = highiplq_head;			// get them all
  highiplq_head   = NULL;			// clear the queue
  highiplq_tail   = &highiplq_head;
  highiplq_lowipl = lowipl;			// re-enable being called in case more queue
  oz_hw_smplock_clr (&oz_s_smplock_hi, hi);	// release the queue

  while ((event = hievents) != NULL) {		// see if there are any (more) to process
    hievents = event -> hinxt;			// ok, unlink it
    event -> onhiq = 0;				// enable it to be queued to us again
    wakethreads (event);			// wake any threads that are waiting for it
    oz_knl_event_increfc (event, -1);		// we're done with it now (refcount was inc'd when put on queue)
  }
}

/************************************************************************/
/*									*/
/*  Increment event reference count					*/
/*									*/
/*    Input:								*/
/*									*/
/*	event = pointer to event					*/
/*	inc   = 1 : increment reference count				*/
/*	       -1 : decrement reference count				*/
/*	        0 : no-op						*/
/*									*/
/*	smp lock <= np							*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_event_increfc = new ref count				*/
/*	if new ref count == 0, event block is freed off			*/
/*									*/
/************************************************************************/

Long oz_knl_event_increfc (OZ_Event *event, Long inc)

{
  Evcb *evcb, *evcbs;
  Long refc;
  uLong ev;

  OZ_KNL_CHKOBJTYPE (event, OZ_OBJTYPE_EVENT);

inc_again:
  do {
    refc = event -> refcount;
    if (refc + inc <= 0) goto going_le_zero;
  } while (!oz_hw_atomic_setif_long (&(event -> refcount), refc + inc, refc));
  return (refc + inc);

going_le_zero:
  if (refc + inc < 0) oz_crash ("oz_knl_event_increfc: %p refcount %d+%d", event, refc, inc);
  ev = oz_hw_smplock_wait (&(event -> smplock_ev));		// block changes to evcbs list
  if (!oz_hw_atomic_setif_long (&(event -> refcount), 0, refc)) { // try to zero the refcount
    oz_hw_smplock_clr (&(event -> smplock_ev), ev);		// failed, release evcbs list and try again
    goto inc_again;
  }
  if (event -> onhiq) oz_crash ("oz_knl_event_increfc: event %p still on highiplq", event);
  evcbs = event -> evcbs;					// see if there are any pending callbacks
  if (evcbs != NULL) {
    if (ev != OZ_SMPLOCK_SOFTINT) {				// ok, see if we can call them
      put_on_highiplq (event);					// if not, queue it to highipl routine
      oz_hw_smplock_clr (&(event -> smplock_ev), ev);		// ... so it will call us back at softint level
      return (1);
    }
    event -> evcbs = NULL;					// if so, clear the queue out
  }
  oz_hw_smplock_clr (&(event -> smplock_ev), ev);		// release evcbs queue

  event -> interval = 0;					// in case timer ast is about to run
  oz_knl_secattr_increfc (event -> secattr, -1);		// done with security attributes
  event -> secattr = NULL;

  while ((evcb = evcbs) != NULL) {				// repeat as long as there are callbacks to do
    evcbs = evcb -> next;					// unlink it
    (*(evcb -> entry)) (evcb -> param, NULL);			// call it with NULL event pointer so it knows event is dead
    OZ_KNL_NPPFREE (evcb);					// free it
  }

timer_again:
  switch (event -> timerstate) {

    /* No timer was ever allocated */

    case EVENT_TIMERSTATE_DEAD: break;

    /* Timer is allocated but not queued, just free it off */

    case EVENT_TIMERSTATE_IDLE: {
      oz_knl_timer_free (event -> timer);
      break;
    }

    /* Timer is in queue */

    case EVENT_TIMERSTATE_QUED: {
      if (oz_knl_timer_remove (event -> timer)) {		// try to rip it out of queue
        oz_knl_timer_free (event -> timer);			// if we did, free it off here
        break;							// ... and free event flag struct
      }
								// otherwise, tell timer ast to do it
      if (!oz_hw_atomic_setif_long (&(event -> timerstate), EVENT_TIMERSTATE_KILL, EVENT_TIMERSTATE_QUED)) goto timer_again;
      return (0);						// timer ast will free timer and event struct
    }

    /* The only way it gets in KILL is when the refcount goes zero */

    default: oz_crash ("oz_knl_event_increfc: bad event %p -> timerstate %d", event, event -> timerstate);
  }

  OZ_KNL_NPPFREE (event);					// free off the event block
  return (0);
}

/************************************************************************/
/*									*/
/*  Queue an callback to happen when event flag is set			*/
/*									*/
/*    Input:								*/
/*									*/
/*	event = pointer to event block					*/
/*	entry = entrypoint to call when event flag is set		*/
/*	param = parameter to pass to entry routine			*/
/*									*/
/*	smp lock = softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_event_queuecb = OZ_FLAGWASSET : flag was set upon entry	*/
/*	                                       callback will not happen	*/
/*	                       OZ_FLAGWASCLR : flag was clear on entry	*/
/*	                                       callback will happen	*/
/*	                                else : error status		*/
/*									*/
/*    Note:								*/
/*									*/
/*	Callback routine gets called at softint level.  If event is 	*/
/*	NULL, it means the refcount on the event went zero.		*/
/*									*/
/************************************************************************/

uLong oz_knl_event_queuecb (OZ_Event *event, void (*entry) (void *param, OZ_Event *event), void *param)

{
  Evcb *evcb;
  uLong ev;

  OZ_KNL_CHKOBJTYPE (event, OZ_OBJTYPE_EVENT);

  if (event -> value > 0) return (OZ_FLAGWASSET);			// if event flag is already set, just return status

  evcb = OZ_KNL_NPPMALLOC (sizeof *evcb);				// clear, allocate callback block
  if (evcb == NULL) return (OZ_EXQUOTANPP);
  evcb -> entry = entry;						// save entrypoint and parameter
  evcb -> param = param;

  ev = oz_hw_smplock_wait (&(event -> smplock_ev));
  evcb -> next = event -> evcbs;					// queue callback to event flag
  event -> evcbs = evcb;
  oz_hw_smplock_clr (&(event -> smplock_ev), ev);

  if (event -> value > 0) callcallbacks (event);			// maybe it got set just before we queued the evcb

  return (OZ_FLAGWASCLR);						// return status saying callback will be (or was) called
}

/* An event flag was just set, call any queued callbacks */
/* This routine is assumed to be called at softint level */

static void callcallbacks (OZ_Event *event)

{
  Evcb *evcb, *evcbs;
  uLong ev;

  if (event -> evcbs == NULL) return;					// quick unlocked test as it is very common
  ev = oz_hw_smplock_wait (&(event -> smplock_ev));			// something there, lock the queue
  evcbs = event -> evcbs;						// get pointer to the whole list
  if (evcbs == NULL) {
    oz_hw_smplock_clr (&(event -> smplock_ev), ev);			// stuff disappeared, there's nothing to do
    return;
  }
  event -> evcbs = NULL;						// got something, clear the list
  oz_knl_event_increfc (event, 1);					// make sure event doesn't free off in loop
  oz_hw_smplock_clr (&(event -> smplock_ev), ev);			// lock can be released now

  while ((evcb = evcbs) != NULL) {					// repeat as long as there are some to do
    evcbs = evcb -> next;						// unlink it
    (*(evcb -> entry)) (evcb -> param, event);				// call it with non-NULL event pointer
    OZ_KNL_NPPFREE (evcb);						// free it
  }
  oz_knl_event_increfc (event, -1);					// now maybe free off event flag
}

/************************************************************************/
/*									*/
/*  Wait for a single event flag to be set				*/
/*									*/
/*  This routine is just a convenience for a common case of waiting 	*/
/*  for a single event flag.  It just calls the general event_waitlist 	*/
/*  routine with a single element array.				*/
/*									*/
/*  ?? someday optimise as it is a very common case ??			*/
/*									*/
/*    Input:								*/
/*									*/
/*	event = pointer to event flag to wait for			*/
/*	smplevel = softint delivery inhibited				*/
/*									*/
/************************************************************************/

uLong oz_knl_event_waitone (OZ_Event *event)

{
  OZ_Eventlist eventlist[1];

  eventlist[0].event = event;
  return (oz_knl_event_waitlist (1, eventlist, OZ_PROCMODE_KNL, 0));
}

/************************************************************************/
/*									*/
/*  Wait for any one of a list of events				*/
/*									*/
/*    Input:								*/
/*									*/
/*	nevents   = number of elements in events			*/
/*	eventlist = list of events to wait for				*/
/*	procmode  = procmode that wait is being done for		*/
/*	si        = 0 : caller won't enable softints before looping back
/*	            1 : caller will enable softints on return		*/
/*									*/
/*	smplock level = softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_event_wait = OZ_FLAGWASSET : at least one flag was set on entry
/*	                    OZ_FLAGWASCLR : all flags were clear on entry
/*	                  OZ_ASTDELIVERED : all flags were clear on entry 
/*	                                    and they still are		*/
/*									*/
/*    Note:								*/
/*									*/
/*	The 'events' array must remain valid until this routine 	*/
/*	returns.  It is assumed the caller has a ref count out on each 	*/
/*	event flag, so this routine doesn't bother inc'ing the event 	*/
/*	flags' reference counts.					*/
/*									*/
/************************************************************************/

uLong oz_knl_event_waitlist (uLong nevents, OZ_Eventlist *eventlist, OZ_Procmode procmode, int si)

{
  OZ_Event *event;
  OZ_Eventlist *neventlist, **peventlist;
  OZ_Procmode pm;
  OZ_Thread *thread;
  uLong ev, i, sts;

  if (oz_hw_cpu_smplevel () != OZ_SMPLOCK_SOFTINT) oz_crash ("oz_knl_event_waitlist: called at smplevel %X", oz_hw_cpu_smplevel ());

  /* If no events specified, return error status, as there would be no flag to wake the thread up */

  if (nevents == 0) return (OZ_MISSINGPARAM);

  /* Set current thread state to WEV, so that if an event should */
  /* set while we're in here we will immediately be woken.       */

  thread = oz_knl_thread_setcurstate (OZ_THREAD_STATE_WEV, nevents, eventlist);

  /* Link to all event flags we are waiting for.  If any  */
  /* one is set though, get out as we don't need to wait. */

  sts = OZ_FLAGWASSET;
  for (i = 0; i < nevents; i ++) {
    event = eventlist[i].event;						// point to an event flag to be tested
    OZ_KNL_CHKOBJTYPE (event, OZ_OBJTYPE_EVENT);			// make sure it is an event flag
    ev = oz_hw_smplock_wait (&(event -> smplock_ev));			// lock the event flag's state
    if (event -> value > 0) goto stayawake;				// see if it is already set
    eventlist[i].neventlist = neventlist = event -> eventlists;		// if not, link this thread to be woken when it gets set
    eventlist[i].peventlist = &(event -> eventlists);
    eventlist[i].thread = thread;
    event -> eventlists = eventlist + i;
    if (neventlist != NULL) neventlist -> peventlist = &(eventlist[i].neventlist);
    oz_hw_smplock_clr (&(event -> smplock_ev), ev);			// unlock event flag's state
  }

  /* An ast might have queued just before we changed the state to WEV above                                      */
  /* But do this after checking the event flags as we prefer to return OZ_FLAGWASSET if both conditions are true */

  /* Don't do this, tho, if the caller had softint delivery inhibited, as that implies */
  /* the caller won't let the ast execute until the wait condition is satisfied.  For  */
  /* example, a suspend ast gets queued while waiting for a pagefault I/O to complete. */

  if (si) {
    sts = OZ_ASTDELIVERED;
    for (pm = OZ_PROCMODE_MIN; pm <= procmode; pm ++) {
      if (oz_knl_thread_chkpendast (pm)) goto stayawake2;
    }
  }

  /* This will return when the current thread state changes to RUN.  If one of those event flags got set since */
  /* we tested it, our state will have already been changed to RUN and the wait call will return immediately.  */
  /* Same is true if an ast gets queued.                                                                       */

  sts = oz_knl_thread_wait ();

  /* Unlink from all the event flags */

  for (i = 0; i < nevents; i ++) {
    event = eventlist[i].event;
    OZ_KNL_CHKOBJTYPE (event, OZ_OBJTYPE_EVENT);
    ev = oz_hw_smplock_wait (&(event -> smplock_ev));
    neventlist = eventlist[i].neventlist;
    peventlist = eventlist[i].peventlist;
    *peventlist = neventlist;
    if (neventlist != NULL) neventlist -> peventlist = peventlist;
    oz_hw_smplock_clr (&(event -> smplock_ev), ev);
  }

  /* Return the wake status to caller (either OZ_FLAGWASCLR or OZ_ASTDELIVERED) */

  return (sts);

  /* An event flag was already set on input, unlink for all events that we linked to so far */

stayawake:
  oz_hw_smplock_clr (&(event -> smplock_ev), ev);		// unlock the event flag we found was set
stayawake2:
  oz_knl_thread_setcurstate (OZ_THREAD_STATE_RUN, 0, NULL);	// change us back to RUNning
  while (i > 0) {
    event = eventlist[--i].event;				// get an event we already linked to
    OZ_KNL_CHKOBJTYPE (event, OZ_OBJTYPE_EVENT);
    ev = oz_hw_smplock_wait (&(event -> smplock_ev));		// unlink this thread from it
    neventlist = eventlist[i].neventlist;
    peventlist = eventlist[i].peventlist;
    *peventlist = neventlist;
    if (neventlist != NULL) neventlist -> peventlist = peventlist;
    oz_hw_smplock_clr (&(event -> smplock_ev), ev);
  }
  return (sts);							// return status saying we didn't wait
}

/************************************************************************/
/*									*/
/*  Get event flag's security attributes				*/
/*									*/
/*    Input:								*/
/*									*/
/*	event = event flag in question					*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_event_getsecattr = secattrs (ref count incremented)	*/
/*									*/
/************************************************************************/

OZ_Secattr *oz_knl_event_getsecattr (OZ_Event *event)

{
  OZ_KNL_CHKOBJTYPE (event, OZ_OBJTYPE_EVENT);
  oz_knl_secattr_increfc (event -> secattr, 1);
  return (event -> secattr);
}

/************************************************************************/
/*									*/
/*  Get event flag's name						*/
/*									*/
/*    Input:								*/
/*									*/
/*	event = pointer to event flag					*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_event_getname = pointer to null-terminate name string	*/
/*									*/
/************************************************************************/

const char *oz_knl_event_getname (OZ_Event *event)

{
  OZ_KNL_CHKOBJTYPE (event, OZ_OBJTYPE_EVENT);
  return (event -> name);
}

/************************************************************************/
/*									*/
/*  Dump out event list							*/
/*									*/
/************************************************************************/

void oz_knl_event_dump (void)

{
  oz_knl_printk ("oz_knl_event_dump: not supported anymore\n");
}
