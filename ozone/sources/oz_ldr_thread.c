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
/*  Thread routines for the loader					*/
/*									*/
/*    They ...								*/
/*									*/
/*      1) are called just like the normal kernel routines		*/
/*      2) are non-pre-emptive, have no priority, round-robin scheduled	*/
/*      3) run in kernel mode only with softint delivery inhibited	*/
/*									*/
/************************************************************************/

#define _OZ_KNL_THREAD_C

#include <stdarg.h>

#include "ozone.h"
#include "oz_knl_devio.h"
#include "oz_knl_event.h"
#include "oz_knl_hw.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_objtype.h"
#include "oz_knl_sdata.h"
#include "oz_knl_status.h"
#include "oz_knl_thread.h"

struct OZ_Thread { OZ_Objtype objtype;				// OZ_OBJTYPE_THREAD
                   OZ_Thread *next;				// next in allthreads list
                   Long refcount;				// number of refs (delete when it goes zero)
                   OZ_Thread_state state;			// current state (COM,RUN,WEV,ZOM, we don't use INI)
                   OZ_Event *exitevent;				// exit event flag (or NULL if none)
                   uLong nevents;				// number of elements in eventlist array
                   OZ_Eventlist *eventlist;			// event flags being waited for (WEV state only)
                   uLong (*thentry) (void *thparam);		// thread routine entrypoint
                   void *thparam;				// thread routine parameter
                   uLong waitsts;				// status to return from wait call
                   uLong numios;				// number of I/O's started by this thread
                   OZ_Ioop *ioopq;				// list of io's in progress
                   int tisinitted;				// set when tis gets initialized
                   OZ_Datebin timeinstate[OZ_THREAD_STATE_MAX];	// time in each state
                   char name[OZ_THREAD_NAMESIZE];		// thread's name
                   uByte hwctx[OZ_THREAD_HW_CTX_SIZE];		// hardware context (registers)
                 };

static OZ_Smplock smplock_tc;		// database lock
static OZ_Thread *allthreads = NULL;	// list of all thread's
static OZ_Thread *curthread  = NULL;	// which thread is currently executing
static OZ_Thread *newthread  = NULL;	// new thread to start
static OZ_Thread *oldthread  = NULL;	// old thread that was current

static void changestate (OZ_Thread *thread, OZ_Thread_state newstate);

/************************************************************************/
/*									*/
/*  Create main thread for the loader					*/
/*									*/
/************************************************************************/

void oz_knl_thread_cpuinit (void)

{
  OZ_Datebin now;
  OZ_Thread *thread;

  oz_hw_smplock_init (sizeof smplock_tc, &smplock_tc, OZ_SMPLOCK_LEVEL_TC);

  thread = OZ_KNL_NPPMALLOC (sizeof *thread);
  memset (thread, 0, sizeof *thread);
  thread -> objtype  = OZ_OBJTYPE_THREAD;
  thread -> refcount = 2;
  thread -> state    = OZ_THREAD_STATE_RUN;
  now = oz_hw_tod_getnow ();
  if (now != 0) {
    thread -> tisinitted = 1;
    thread -> timeinstate[OZ_THREAD_STATE_RUN] -= now;
  }
  strncpyz (thread -> name, "main", sizeof thread -> name);
  oz_hw_thread_initctx (thread -> hwctx, 0, NULL, NULL, NULL, thread);

  thread -> next = NULL;
  allthreads = thread;

  curthread = thread;
}

/************************************************************************/
/*									*/
/*  Create thread							*/
/*									*/
/*    Note: Loader version must always return OZ_SUCCESS		*/
/*									*/
/************************************************************************/

uLong oz_knl_thread_create (OZ_Process *process, 
                            uLong priority, 
                            OZ_Seckeys *seckeys, 
                            OZ_Event *initevent, 
                            OZ_Event *exitevent, 
                            OZ_Mempage stacksize, 
                            uLong (*thentry) (void *thparam), 
                            void *thparam, 
                            OZ_Astmode knlastmode, 
                            int name_l, 
                            const char *name, 
                            OZ_Secattr *secattr, 
                            OZ_Thread **thread_r)

{
  OZ_Datebin now;
  OZ_Thread *thread;
  uLong ts;

  if (initevent != NULL) oz_crash ("oz_knl_thread_create: loader doesn't support initevent");
  if (stacksize != 0)    oz_crash ("oz_knl_thread_create: loader doesn't support user stack");

  thread = OZ_KNL_NPPMALLOC (sizeof *thread);				/* allocate a new block */
  memset (thread, 0, sizeof *thread);
  thread -> objtype    = OZ_OBJTYPE_THREAD;				/* set up the object type */
  thread -> refcount   = 2;						/* one for caller, one for the cpu */
  thread -> exitevent  = exitevent;					/* exit event flag */
  if (exitevent != NULL) oz_knl_event_increfc (exitevent, 1);
  thread -> thentry    = thentry;					/* save thread routine entrypoint */
  thread -> thparam    = thparam;					/* save thread routine parameter */
  thread -> state      = OZ_THREAD_STATE_COM;				/* set state = 'waiting for a cpu' */
  thread -> tisinitted = 1;						/* assume oz_hw_tod_getnow is working */
  now = oz_hw_tod_getnow ();
  thread -> timeinstate[OZ_THREAD_STATE_COM] -= now;
  if (name_l > sizeof thread -> name - 1) name_l = sizeof thread -> name - 1;
  movc4 (name_l, name, sizeof thread -> name, thread -> name);

  ts = oz_hw_smplock_wait (&smplock_tc);				/* lock database */

  thread -> next = allthreads;						/* link it to list of all threads */
  allthreads = thread;

  newthread = thread;							/* set this up as the new thread */
  oldthread = curthread;						/* current thread is the 'old' one */
  oz_hw_thread_initctx (thread -> hwctx, 0, thentry, thparam, NULL, thread); /* set up hwctx to call oz_knl_thread_start when loaded */

  oz_hw_smplock_clr (&smplock_tc, ts);					/* release database */

  *thread_r = thread;
  return (OZ_SUCCESS);
}

void oz_knl_thread_start (OZ_Thread *thread)

{
  uLong sts;

  OZ_KNL_CHKOBJTYPE (curthread, OZ_OBJTYPE_THREAD);
  OZ_KNL_CHKOBJTYPE (oldthread, OZ_OBJTYPE_THREAD);
  if (thread != curthread) oz_crash ("oz_knl_thread_start: thread %p .ne. curthread %p", thread, curthread);
  if (oldthread -> state == OZ_THREAD_STATE_ZOM) oz_knl_thread_increfc (oldthread, -1);	/* if old one was zombie, get rid of it now */
  oz_hw_smplock_clr (&smplock_tc, OZ_SMPLOCK_SOFTINT);					/* release ts smp lock */
  sts = (*(curthread -> thentry)) (curthread -> thparam);				/* call the thread routine */
  while (1) oz_knl_thread_exit (sts);
}

/************************************************************************/
/*									*/
/*  Set the current thread's state					*/
/*									*/
/*    Input:								*/
/*									*/
/*	state = new state to set (WEV or RUN)				*/
/*	nevents,eventlist = put these in thread under lock		*/
/*	softint <= smplevel <= tc					*/
/*									*/
/************************************************************************/

OZ_Thread *oz_knl_thread_setcurstate (OZ_Thread_state state, uLong nevents, OZ_Eventlist *eventlist)

{
  OZ_Thread *thread;
  uLong tc;

  if ((state != OZ_THREAD_STATE_RUN) && (state != OZ_THREAD_STATE_WEV)) {
    oz_crash ("oz_knl_thread_setcurstate: new state %d unsupported", thread -> state);
  }

  tc = oz_hw_smplock_wait (&smplock_tc);
  thread = curthread;
  OZ_KNL_CHKOBJTYPE (thread, OZ_OBJTYPE_THREAD);
  switch (thread -> state) {
    case OZ_THREAD_STATE_COM: {
      break;
    }
    case OZ_THREAD_STATE_RUN:
    case OZ_THREAD_STATE_WEV:
    case OZ_THREAD_STATE_ZOM: {
      changestate (thread, state);
      thread -> nevents   = nevents;
      thread -> eventlist = eventlist;
      break;
    }
    default: oz_crash ("oz_knl_thread_setcurstate: old state %d unsupported", thread -> state);
  }
  oz_hw_smplock_clr (&smplock_tc, tc);
  return (thread);
}

/************************************************************************/
/*									*/
/*  Wake a thread that might be in WEV state				*/
/*									*/
/*    Input:								*/
/*									*/
/*	thread  = thread to wake					*/
/*	wakests = status to wake it with				*/
/*	smplevel <= tc							*/
/*									*/
/************************************************************************/

void oz_knl_thread_wakewev (OZ_Thread *thread, uLong wakests)

{
  uLong tc;

  OZ_KNL_CHKOBJTYPE (thread, OZ_OBJTYPE_THREAD);
  tc = oz_hw_smplock_wait (&smplock_tc);
  if (thread -> state == OZ_THREAD_STATE_WEV) {
    thread -> waitsts = wakests;
    changestate (thread, OZ_THREAD_STATE_COM);
  }
  oz_hw_smplock_clr (&smplock_tc, tc);
}

/************************************************************************/
/*									*/
/*  Make current thread wait and start a computable one in its place	*/
/*									*/
/*    Input:								*/
/*									*/
/*	curthread = current thread to make wait				*/
/*	smplevel  = softint						*/
/*									*/
/************************************************************************/

uLong oz_knl_thread_wait (void)

{
  int si;
  OZ_Thread **lthread, *thread;
  uLong tc;

get_something:
  tc = oz_hw_smplock_wait (&smplock_tc);
  if (tc != OZ_SMPLOCK_SOFTINT) oz_crash ("oz_knl_thread_wait: called at smplevel %u", tc);

  /* Do any pending lowipls */

  if (oz_knl_lowipl_lowipls != NULL) {
    oz_hw_smplock_clr (&smplock_tc, OZ_SMPLOCK_SOFTINT);
    oz_knl_lowipl_handleint ();
    goto get_something;
  }

  /* the original thread might have been put */
  /* in run state by an lowipl routine that */
  /* waited for an event flag and was */
  /* subsequently resumed, then it returned */
  /* to the wait loop here, well the thread */
  /* state will now be 'RUN' */

  OZ_KNL_CHKOBJTYPE (curthread, OZ_OBJTYPE_THREAD);
  if (curthread -> state == OZ_THREAD_STATE_RUN) {
    oz_hw_smplock_clr (&smplock_tc, OZ_SMPLOCK_SOFTINT);
    return;
  }

  /* Find a computable thread and start it */

  for (lthread = &allthreads; (thread = *lthread) != NULL; lthread = &(thread -> next)) {
    OZ_KNL_CHKOBJTYPE (thread, OZ_OBJTYPE_THREAD);
    if (thread -> state == OZ_THREAD_STATE_COM) break;
  }

  /* No thread is computable, wait for an interrupt then check again */

  if (thread == NULL) {
    oz_hw_smplock_clr (&smplock_tc, OZ_SMPLOCK_SOFTINT);
    oz_hw_cpu_waitint (NULL);
    goto get_something;
  }

  /* Have a computable thread, make it current and move to end of 'allthreads' list */

  newthread = thread;							/* save pointer in static variable */
  thread = thread -> next;						/* unlink from 'allthreads' list */
  *lthread = thread;
  while ((thread = *lthread) != NULL) lthread = &(thread -> next);	/* skip to end of 'allthreads' list */
  *lthread = newthread;							/* put new current thread on end of 'allthreads' list */
  newthread -> next  = NULL;
  changestate (newthread, OZ_THREAD_STATE_RUN);				/* set new state to 'running' */
  oldthread = curthread;						/* save old thread pointer */
  curthread = newthread;						/* set up new current thread */
  oz_hw_thread_switchctx (oldthread -> hwctx, newthread -> hwctx);	/* switch stacks */
  if (oldthread -> state == OZ_THREAD_STATE_ZOM) oz_knl_thread_increfc (oldthread, -1); /* if old one was zombie, get rid of it now */
  oz_hw_smplock_clr (&smplock_tc, OZ_SMPLOCK_SOFTINT);
}

/************************************************************************/
/*									*/
/*  Increment thread reference count					*/
/*									*/
/************************************************************************/

Long oz_knl_thread_increfc (OZ_Thread *thread, Long inc)

{
  Long refc;
  uLong tc;
  OZ_Thread **lthread, *nthread;

  tc = oz_hw_smplock_wait (&smplock_tc);
  thread -> refcount += inc;
  refc = thread -> refcount;
  if (refc < 0) oz_crash ("oz_knl_thread_increfc: ref count went negative");
  if (refc == 0) {
    if (thread -> ioopq != NULL) oz_crash ("oz_knl_thread_increfc: io still in progress");
    if (thread -> state != OZ_THREAD_STATE_ZOM) oz_crash ("oz_knl_thread_increfc: thread refcount 0 but state not ZOM (%d)", thread -> state);
    for (lthread = &allthreads; (nthread = *lthread) != thread; lthread = &(nthread -> next)) {
      if (nthread == NULL) oz_crash ("oz_knl_thread_increfc: zombie thread not found on allthreads list");
    }
    *lthread = nthread -> next;
    oz_hw_thread_termctx (thread -> hwctx, NULL);
    if (thread -> exitevent != NULL) oz_knl_event_increfc (thread -> exitevent, -1);
    OZ_KNL_NPPFREE (thread);
  }
  oz_hw_smplock_clr (&smplock_tc, tc);
  return (refc);
}

/************************************************************************/
/*									*/
/*  Change thread's state						*/
/*									*/
/*  This routine updates the thread's state variable and updates the 	*/
/*  time in state element.  It does not do anything with the queues.	*/
/*									*/
/*  smplevel = tc							*/
/*									*/
/************************************************************************/

static void changestate (OZ_Thread *thread, OZ_Thread_state newstate)

{
  OZ_Datebin now;
  OZ_Thread_state oldstate;
  uLong newprio;

  oldstate = thread -> state;
  thread -> state = newstate;

  /* Maybe main's time-in-state-run did not get initialized */

  now = oz_hw_tod_getnow ();
  if (!(thread -> tisinitted)) {
    thread -> timeinstate[OZ_THREAD_STATE_RUN] -= now;
    thread -> tisinitted = 1;
  }

  /* Stop counting time in the old state */

  thread -> timeinstate[oldstate] += now;

  /* Start counting time in new state */

  thread -> timeinstate[newstate] -= now;
}

/************************************************************************/
/*									*/
/*  Get thread's ioopq pointer						*/
/*									*/
/************************************************************************/

OZ_Ioop **oz_knl_thread_getioopqp (OZ_Thread *thread)

{
  return (&(thread -> ioopq));
}

/************************************************************************/
/*									*/
/*  Change process that the current thread belongs to			*/
/*									*/
/*  It is a no-op in the loader because there are no processes		*/
/*									*/
/************************************************************************/

void oz_knl_thread_changeproc (OZ_Process *newprocess)

{ }

/************************************************************************/
/*									*/
/*  Queue an ast to a thread						*/
/*									*/
/*  We don't do ast's as all threads run at softint or above, so no 	*/
/*  ast could possibly be delivered					*/
/*									*/
/************************************************************************/

void oz_knl_thread_queueast (OZ_Ast *ast, uLong aststs)

{
  oz_crash ("oz_knl_thread_queueast: cannot queue ast's in loader");
}

/************************************************************************/
/*									*/
/*  Returns pointer to process that owns a thread			*/
/*									*/
/************************************************************************/

OZ_Process *oz_knl_thread_getprocess (OZ_Thread *thread)

{
  return (NULL);
}

OZ_Process *oz_knl_thread_getprocesscur (void)

{
  return (NULL);
}

/************************************************************************/
/*									*/
/*  Returns pointer to thread block currently executing on the current 	*/
/*  cpu									*/
/*									*/
/************************************************************************/

OZ_Thread *oz_knl_thread_getcur (void)

{
  return (curthread);
}

/************************************************************************/
/*									*/
/*  Returns pointer to thread's security keys (just return a NULL to 	*/
/*  get access to everything)						*/
/*									*/
/************************************************************************/

OZ_Seckeys *oz_knl_thread_getseckeys (OZ_Thread *thread)

{
  return (NULL);
}

/************************************************************************/
/*									*/
/*  Returns thread's exit status					*/
/*									*/
/************************************************************************/

uLong oz_knl_thread_getexitsts (OZ_Thread *thread, uLong *exitsts_r)

{
  if (thread -> state != OZ_THREAD_STATE_ZOM) return (OZ_FLAGWASCLR);
  *exitsts_r = thread -> waitsts;
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Get thread's name string pointer					*/
/*									*/
/************************************************************************/

const char *oz_knl_thread_getname (OZ_Thread *thread)

{
  return (thread -> name);
}

/************************************************************************/
/*									*/
/*  Get thread's default create security attributes			*/
/*									*/
/************************************************************************/

OZ_Secattr *oz_knl_thread_getdefcresecattr (OZ_Thread *thread)

{
  oz_knl_secattr_increfc (oz_s_secattr_syslogname, 1);
  return (oz_s_secattr_syslogname);	/* allow everyone to read what I create, only system can write it */
}

/************************************************************************/
/*									*/
/*  Called by hardware layer in response to an softint			*/
/*									*/
/************************************************************************/

void oz_knl_thread_handleint (void)

{ }

/************************************************************************/
/*									*/
/*  Called by hardware layer when quantum timer expires			*/
/*									*/
/************************************************************************/

void oz_knl_thread_quantimex (Long cpuidx)

{ }

/************************************************************************/
/*									*/
/*  Called by hardware layer to process kernel mode asts		*/
/*  We don't do ast's, so there's nothing to check for			*/
/*									*/
/************************************************************************/

void oz_knl_thread_checkknlastq (Long cpuidx, OZ_Mchargs *mchargs)

{ }

/************************************************************************/
/*									*/
/*  Called by hardware layer to check for pending outermode asts	*/
/*									*/
/************************************************************************/

int oz_knl_thread_chkpendast (OZ_Procmode procmode)

{
  return (0);
}

/************************************************************************/
/*									*/
/*  Orphan a thread (well, all our threads are orphans)			*/
/*									*/
/************************************************************************/

void oz_knl_thread_orphan (OZ_Thread *thread)

{ }

/************************************************************************/
/*									*/
/*  Called when a thread exits						*/
/*									*/
/************************************************************************/

void oz_knl_thread_exit (uLong status)

{
  OZ_KNL_CHKOBJTYPE (curthread, OZ_OBJTYPE_THREAD);
  oz_knl_iorundown (curthread, OZ_PROCMODE_KNL);	// do I/O rundown
  OZ_KNL_CHKOBJTYPE (curthread, OZ_OBJTYPE_THREAD);
  curthread -> waitsts = status;			// save its exit status
  while (1) {
    changestate (curthread, OZ_THREAD_STATE_ZOM);	// kill it off
    if (curthread -> exitevent != NULL) {		// maybe someone wants to know that we've exited
      oz_knl_event_inc (curthread -> exitevent, 1);
    }
    oz_knl_thread_wait ();				// loop in case some lowipl routine puts it in run state
  }
}

/************************************************************************/
/*									*/
/*  Called by hardware layer to dequeue outermode ast's			*/
/*									*/
/************************************************************************/

void oz_sys_thread_checkast (OZ_Mchargs *mchargs)

{ }

/************************************************************************/
/*									*/
/*  Get thread's time in a particular state				*/
/*									*/
/************************************************************************/

OZ_Datebin oz_knl_thread_gettis (OZ_Thread *thread, OZ_Thread_state state)

{
  OZ_Datebin now, tis;
  OZ_Thread_state curstate;
  uLong ts;

  OZ_KNL_CHKOBJTYPE (thread, OZ_OBJTYPE_THREAD);	/* make sure they passed a thread */
  ts  = oz_hw_smplock_wait (&smplock_tc);		/* lock out any changes in state */
  tis = thread -> timeinstate[state];			/* get time in requested state */
  curstate = thread -> state;				/* get thread's current state */
  oz_hw_smplock_clr (&smplock_tc, ts);			/* release threadstate smp lock */
  if (curstate == state) {				/* see if they're asking for time in current state */
    now  = oz_hw_tod_getnow ();				/* if so, get current date/time */
    tis += now;						/* ... and add to time in state */
  }
  return (tis);						/* return time in state */
}

/************************************************************************/
/*									*/
/*  Dump thread list							*/
/*									*/
/************************************************************************/

void oz_ldr_thread_dump (void)

{
  char *statestr;
  int i;
  OZ_Datebin tisrun;
  OZ_Event *event;
  OZ_Thread *thread;
  uLong ts;

  ts = oz_hw_smplock_wait (&smplock_tc);
  oz_knl_printk ("\n");
  for (thread = allthreads; thread != NULL; thread = thread -> next) {
    statestr = NULL;
    if (thread -> state == OZ_THREAD_STATE_COM) statestr = "COM";
    if (thread -> state == OZ_THREAD_STATE_RUN) statestr = "RUN";
    if (thread -> state == OZ_THREAD_STATE_WEV) statestr = "WEV";
    if (thread -> state == OZ_THREAD_STATE_ZOM) statestr = "ZOM";
    tisrun = oz_knl_thread_gettis (thread, OZ_THREAD_STATE_RUN);
    if (statestr != NULL) oz_knl_printk ("thread:  %p  %s  %#t  %u  %s\n", thread, statestr, tisrun, thread -> numios, thread -> name);
    else oz_knl_printk ("thread:  %p  %3d  %#t  %u  %s\n", thread, thread -> state, tisrun, thread -> numios, thread -> name);
    if (thread -> state == OZ_THREAD_STATE_WEV) {
      for (i = 0; i < thread -> nevents; i ++) {
        event = thread -> eventlist[i].event;
        oz_knl_printk (" event:            %p  %3d  %s\n", event, oz_knl_event_inc (event, 0), oz_knl_event_getname (event));
      }
    }
  }

  oz_hw_smplock_clr (&smplock_tc, ts);
}


OZ_Secattr *oz_knl_thread_getsecattr (OZ_Thread *thread)

{
  return (NULL);
}

OZ_Threadid oz_knl_thread_getid (OZ_Thread *thread)

{
  return (0);
}

OZ_Devunit **oz_knl_thread_getdevalloc (OZ_Thread *thread)

{
  oz_crash ("oz_knl_thread_getdevalloc: not available in loader");
}

uLong oz_knl_thread_getbasepri (OZ_Thread *thread)

{
  return (1);
}

uLong oz_knl_thread_incios (OZ_Thread *thread, uLong inc)

{
  if (thread == NULL) thread = oz_knl_thread_getcur ();
  thread -> numios += inc;
  return (thread -> numios);
}
