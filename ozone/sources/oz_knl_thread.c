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
/*  This module contains the thread object handling routines		*/
/*									*/
/*    Thread states:							*/
/*									*/
/*	INI: just set up by thread_create, cpuidx = -1			*/
/*	RUN: currently executing on a cpu, cpuidx >= 0			*/
/*	COM: waiting for a cpu to execute on or about to		*/
/*	WEV: waiting for an event flag or about to			*/
/*	ZOM: waiting for refcount to go zero before getting freed off	*/
/*									*/
/************************************************************************/

#define _OZ_KNL_THREAD_C
#include "ozone.h"

#include "oz_dev_timer.h"
#include "oz_knl_ast.h"
#include "oz_knl_devio.h"
#include "oz_knl_event.h"
#include "oz_knl_handle.h"
#include "oz_knl_hw.h"
#include "oz_knl_idno.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_objtype.h"
#include "oz_knl_process.h"
#include "oz_knl_quota.h"
#include "oz_knl_sdata.h"
#include "oz_knl_security.h"
#include "oz_knl_status.h"
#include "oz_knl_thread.h"
#include "oz_sys_dateconv.h"
#include "oz_sys_thread.h"
#include "oz_sys_xprintf.h"

#define DELTARUNQUANT (OZ_TIMER_RESOLUTION / 10)	// how often to decrement RUN thread's priority
#define DELTACOMQUANT (OZ_TIMER_RESOLUTION / 3)		// how often to increment COM thread's priority
#define DELTAWEVQUANT (OZ_TIMER_RESOLUTION / 10)	// how often to increment WEV thread's priority

#define REMTHREADFROMQ(__thread) do { \
	OZ_Thread *__x, **__y; \
	__y = __thread -> stateprev; \
	if (__y == NULL) oz_crash ("oz_knl_thread remthreadromq: thread not in a queue"); \
	*__y = __x = __thread -> statenext; \
	if (__x != NULL) __x -> stateprev = __y; \
	__thread -> statenext = NULL; \
	__thread -> stateprev = NULL; \
} while (0)

#define INSTHREADINTOQ(__thread,__pred) do { \
	OZ_Thread *__x; \
	if (__thread -> stateprev != NULL) oz_crash ("oz_knl_thread insthreadintoq: thread aready in a queue"); \
	__thread -> statenext = __x = *(__pred); \
	__thread -> stateprev = __pred; \
	if (__x != NULL) __x -> stateprev = &(__thread -> statenext); \
	*(__pred) = __thread; \
} while (0)

/* Exit handler block */

struct OZ_Exhand { OZ_Objtype objtype;				/* OZ_OBJTYPE_EXHAND */
                   OZ_Thread *thread;				/* pointer to thread it is queued to */
                   OZ_Procmode procmode;			/* processor mode */
                   OZ_Exhand *next;				/* next in thread -> exhandq list */
                   OZ_Exhand_entry exhentry;			/* exit handler entrypoint */
                   void *exhparam;				/* exit handler parameter */
                 };

/* Thread block */

struct OZ_Thread { OZ_Objtype objtype;				/* OZ_OBJTYPE_THREAD */
                   volatile OZ_Thread_state state;		/* thread's state */
								/* - change this value only with the changestate */
								/*   routine so the timeinstate gets updated properly */
                   int knlexit;					/* zero : normal */
								/*   +1 : exited, but let ast's still queue */
								/*   -1 : exited, and don't let any ast's queue */
                   OZ_Thread *statenext;			/* next in threadq_* lists */
                   OZ_Thread **stateprev;			/* previous in threadq_* lists */
                   OZ_Threadid threadid;			/* this thread's id */
                   OZ_Thread *parent;				/* parent thread, NULL if orphaned */
                   OZ_Thread *children;				/* list of children */
                   OZ_Thread *siblingnext;			/* next in list of siblings */
                   OZ_Thread **siblingprev;			/* previous in list of siblings */
                   OZ_Iotatime nextrunquant;			/* amount of run time (timeinstate[OZ_THREAD_STATE_RUN]) for next quantum interrupt */
								/* - gets reset to a new value when it expires */
								/* - gets reset to a new value when thread enters OZ_THREAD_STATE_WEV */
								/* - does not get reset when thread enters OZ_THREAD_STATE_COM */
                   OZ_Iotatime nextcomquant;			/* amount of com time (timeinstate[OZ_THREAD_STATE_COM]) for priority boost */
                   OZ_Iotatime nextwevquant;			/* timeinstate[OZ_THREAD_STATE_WEV] when quantum will run out */
                   OZ_Iotatime timeinstate[OZ_THREAD_STATE_MAX]; /* time in each state - */
								/* - the entry for OZ_THREAD_STATE_INI has the absolute time the thread was started */
								/* - the entry for the current state must have the current date/time added to it to get time in state */
								/* - all other entries have the actual amount of time spent in that state */
								/* - INI: absolute time the thread was started */
								/* - RUN: actual cpu time used by the thread */
								/* - COM: time spent waiting for an available cpu */
								/* - WEV: time spent waiting for events */
								/* - ZOM: elapsed time since exiting */
                   uLong basepri;				/* base execution priority (the bigger, the higher the priority) */
                   uLong curprio;				/* current execution priority (might be .lt. basepri if it hogged cpu, but never .gt. basepri) */
                   OZ_Process *process;				/* what process this thread belongs to */
                   OZ_Thread *proc_next;			/* next thread in process -> threadq */
                   OZ_Thread **proc_prev;			/* pointer to prev thread's proc_next pointer */
                   uLong nevents;				/* number of events being waited for - only valid during OZ_THREAD_STATE_WEV */
                   OZ_Eventlist *eventlist;			/* pointer to array of event pointers - only valid during OZ_THREAD_STATE_WEV */
                   OZ_Astmode astmode[OZ_PROCMODE_MAX-OZ_PROCMODE_MIN+1]; /* ast delivery modes per processor mode */
								/* astmode is not protected by any smplock, so only change by the thread only */
                   OZ_Ast  *astexpqh[OZ_PROCMODE_MAX-OZ_PROCMODE_MIN+1]; /* express ast block queues per processor mode */
                   OZ_Ast **astexpqt[OZ_PROCMODE_MAX-OZ_PROCMODE_MIN+1]; /* express ast block queues per processor mode */
                   OZ_Ast  *astnorqh[OZ_PROCMODE_MAX-OZ_PROCMODE_MIN+1]; /* normal ast block queues per processor mode */
                   OZ_Ast **astnorqt[OZ_PROCMODE_MAX-OZ_PROCMODE_MIN+1]; /* normal ast block queues per processor mode */
                   OZ_Exhand *exhandq[OZ_PROCMODE_MAX-OZ_PROCMODE_MIN+1]; /* exit handler queue per processor mode */
                   Long cpuidx;					/* if >= 0, cpu that holds the contents of hw_ctx */
								/* if < 0, hw_ctx stored in hw_ctx is valid (not in any cpu) */
                   OZ_Event *initevent;				/* wait for this flag before running */
                   OZ_Event *exitevent;				/* exit event flag */
                   OZ_Event *suspevent;				/* suspend event flag */
                   OZ_Event *parexevent;			/* event flag to set if parent is waiting for this thread to orphan from it */
                   OZ_Event *ioevent;				/* I/O event flag for oz_sys_io routine */
                   uLong wakests;				/* reason woken from WEV state (OZ_FLAGWASCLR or OZ_ASTDELIVERED) */
                   uLong exitsts;				/* exit status */
                   volatile Long refcount;			/* reference count */
								/* ... initially set to 2, decremented by oz_knl_thread_wait when context is wiped from last cpu to use it */
								/*                         and decremented when creator finishes with thread block pointer */
								/* ... incremented for each handle assigned to it */
								/* ... incremented for being on process's threadq list */
								/* ... incremented for each ast block that references it, no matter where the ast block is */
                   OZ_Seckeys *seckeys;	             		/* security keys - what I can access (if NULL, anything!) */
                   OZ_Secattr *secattr;				/* security attributes - who can access me */
                   OZ_Secattr *defcresecattr;			/* default create security attributes */
								/* - who can access the things I create */
                   uLong validseq;				/* used by oz_knl_thread_validate */
                   OZ_Devunit *devalloc;			/* list of allocated devices */
                   OZ_Ioop *ioopq;				/* list of ioops started by this thread */
                   volatile Long handlingint;			/* number of times entered oz_knl_thread_handleint - number of times left */
                   uLong volatile numios;			/* number of I/O's started by this thread */
                   uLong volatile numpfs;			/* number of pagefaults by this thread */
                   char name[OZ_THREAD_NAMESIZE];		/* thread name string */
                   OZ_Smplock smplock_tp;			/* thread private smplock */
                   int abortpend;				/* set if an abort ast has been queued */
                   int inwaitloop;				/* set if thread is in wait loop */
                   uLong hw_ctx[OZ_THREAD_HW_CTX_SIZE/sizeof(uLong)]; /* hardware context data */
								/* valid only if cpuidx < 0 - otherwise it is currently in cpu[cpuidx] */
                 };

/* Internal static data */

typedef OZ_Thread *Threadp;

static OZ_Smplock smplock_tc;			/* threadq_com lock */
static OZ_Smplock smplock_tf;			/* thread 'family' smp lock (orphans/parents/siblings/children) */
static OZ_Thread *curthreads[OZ_HW_MAXCPUS];	/* array[oz_s_cpucount] of threads mapped to the given cpu */
						/* - element accessed only by corresponding cpu as it is unlocked */
static uLong curprios[OZ_HW_MAXCPUS];		/* array[oz_s_cpucount] of thread priorities */
						/* - not locked, but who cares if we're a little wrong sometimes */
static OZ_Thread *orphans = NULL;		/* list of threads with parent = NULL */
						/* - locked by smplock_tf */
static uLong cpu_round_robin = 0;		/* last cpu assigned to a thread */
						/* - locked by smplock_tc */
static volatile Threadp threadq_com = NULL;	/* list of threads in OZ_THREAD_STATE_COM (sorted by curprio) */
						/* - locked by smplock_tc */

static OZ_Iotatime deltacomquant, deltarunquant, deltawevquant;

/* Internal routines */

static void thread_timer_check (void *dummy, OZ_Timer *thread_timer);
static uLong add_thread_to_process (OZ_Thread *thread, OZ_Process *process, int ifnormal);
static void add_thread_to_parent (OZ_Thread *thread, OZ_Thread *parent);
static OZ_Thread *nextsibling (OZ_Thread *thread, OZ_Thread *parent);
static void suspend_ast (void *threadv, uLong status, OZ_Mchargs *mchargs);
static void thread_abort (void *dummy, uLong abortsts, OZ_Mchargs *mchargs);
static OZ_Thread *woken_part1 (OZ_Thread *thread);
static void woken_part2 (OZ_Thread *thread, OZ_Thread *zthread, OZ_Process *process);
static void makecom (OZ_Thread *thread, int softint);
static void start_quantum (OZ_Thread *thread);
static void setcurprio (OZ_Thread *thread, uLong newprio);
static void checkknlastq (OZ_Thread *thread, OZ_Mchargs *mchargs);
static void changestate (OZ_Thread *thread, OZ_Thread_state newstate);
static void validate_process_threadq (OZ_Thread **threadqp, OZ_Process *process, int verbose);
static void valthread (OZ_Thread *thread);
static void validate_com_queue ();

/************************************************************************/
/*									*/
/*  Initialize thread processing					*/
/*									*/
/*    Output:								*/
/*									*/
/*	static variables initialized					*/
/*									*/
/************************************************************************/

void oz_knl_thread_init (void)

{
  memset (curthreads, 0, sizeof curthreads);
  memset (curprios,   0, sizeof curprios);

  oz_hw_smplock_init (sizeof smplock_tc, &smplock_tc, OZ_SMPLOCK_LEVEL_TC);
  oz_hw_smplock_init (sizeof smplock_tf, &smplock_tf, OZ_SMPLOCK_LEVEL_TF);

  deltacomquant = oz_hw_tod_dsys2iota (DELTACOMQUANT);
  deltarunquant = oz_hw_tod_dsys2iota (DELTARUNQUANT);
  deltawevquant = oz_hw_tod_dsys2iota (DELTAWEVQUANT);
}

void oz_knl_thread_timerinit (void)

{ }

/************************************************************************/
/*									*/
/*  Initialize thread processing on a cpu				*/
/*									*/
/*    Input:								*/
/*									*/
/*	called by the boot routines to make this cpu available for 	*/
/*	thread processing						*/
/*	smplevel = softints inhibited					*/
/*									*/
/*    Output:								*/
/*									*/
/*	What is happening now on current cpu is now a thread.		*/
/*									*/
/*    Note:								*/
/*									*/
/*	This routine should be called for each cpu on the system, 	*/
/*	then each caller should then either call oz_knl_thread_exit 	*/
/*	or go into an infinite loop of 'wait-for-interrupt' 		*/
/*	instructions.							*/
/*									*/
/*	The thread gets set to priority 0 which is the very lowest.  	*/
/*	This is ok because we have softints inhibited so we won't 	*/
/*	switch anyway, and we do this so when the boot routines 	*/
/*	return, the 'wait-for-interrupt' loops will be easily 		*/
/*	interrupted.							*/
/*									*/
/************************************************************************/

void oz_knl_thread_cpuinit (void)

{
  int j;
  Long cpuidx;
  OZ_Iotatime iotanow;
  OZ_Thread *thread;
  uLong sts, ts;

  /* Get my cpu index.  It should be < the number of cpu's. */

  cpuidx = oz_hw_cpu_getcur ();					/* get this cpu's index */
  if (cpuidx < 0) oz_crash ("oz_knl_thread_init: cpu index %d is negative", cpuidx);
  if (cpuidx >= oz_s_cpucount) oz_crash ("oz_knl_thread_init: cpu index %d, max is %d", cpuidx, oz_s_cpucount);

  /* Make what is happening into a thread - then when initializing routine */
  /* calls oz_knl_thread_exit, this cpu will be used to process threads    */

  thread = OZ_KNL_NPPMALLOC (sizeof *thread);			/* allocate a block of non-paged pool storage */
  memset (thread, 0, sizeof *thread);				/* clear everything */
  thread -> objtype       = OZ_OBJTYPE_THREAD;			/* - object type = thread */
  thread -> refcount      = 1;					/* - refcount set to 1: gets decremented when context is unloaded from the cpu for the last time */
  iotanow = oz_hw_tod_iotanow ();				/* - time in state */
  thread -> timeinstate[OZ_THREAD_STATE_INI]  = iotanow;	/* - (this one holds the thread's start time) */
  thread -> timeinstate[OZ_THREAD_STATE_RUN] -= iotanow;
  thread -> nextrunquant  = deltarunquant;			/* - let it run a whole quantum to start */
  thread -> nextcomquant  = deltacomquant;
  thread -> state         = OZ_THREAD_STATE_RUN;		/* - it is currently running on a cpu */
  for (j = 0; j <= OZ_PROCMODE_MAX - OZ_PROCMODE_MIN; j ++) {
    thread -> astmode[j]  = OZ_ASTMODE_ENABLE;			/* - there are no asts queued, and normal ast delivery is enabled */
    thread -> astexpqt[j] = thread -> astexpqh + j;
    thread -> astnorqt[j] = thread -> astnorqh + j;
  }
  thread -> cpuidx        = cpuidx;				/* - this is the cpu that it is running on */
  thread -> exitsts       = OZ_PENDING;				/* - it has not exited yet */
  thread -> defcresecattr = oz_s_secattr_syslogname;		/* - let others read what I create */
  oz_knl_secattr_increfc (oz_s_secattr_syslogname, 1);
  oz_sys_sprintf (sizeof thread -> name, thread -> name, "cpu init %d", cpuidx);

  sts = oz_knl_event_create (9, "oz_sys_io", NULL, &(thread -> ioevent));
  if (sts != OZ_SUCCESS) oz_crash ("oz_knl_thread_cpuinit: error %u creating I/O event flag", sts);

  sts = oz_hw_thread_initctx (thread -> hw_ctx, 0, NULL, NULL, oz_knl_process_gethwctx (oz_s_systemproc), thread); /* init hardware ctx block */
  if (sts != OZ_SUCCESS) oz_crash ("oz_knl_thread_cpuinit: error %u initializing hardware context", sts);

  oz_hw_smplock_init (sizeof thread -> smplock_tp, &(thread -> smplock_tp), OZ_SMPLOCK_LEVEL_TP);

  sts = add_thread_to_process (thread, oz_s_systemproc, 0);	/* make it belong to the system process */
  if (sts != OZ_SUCCESS) oz_crash ("oz_knl_thread_cpuinit: error %u adding thread to system process", sts);
  add_thread_to_parent (thread, NULL);				/* make it an orphan */
  curthreads[cpuidx] = thread;					/* save the pointer that says what thread is running on this cpu */
  oz_hw_atomic_or_long (&oz_s_cpusavail, 1 << cpuidx);		/* the cpu is available for executing threads now */

  oz_knl_printk ("oz_knl_thread_cpuinit: cpu %d initialization complete\n", cpuidx);
}

/************************************************************************/
/*									*/
/*  Create a thread							*/
/*									*/
/*    Input:								*/
/*									*/
/*	process    = pointer to process that thread belongs to		*/
/*	priority   = thread's execution priority			*/
/*	seckeys    = security keys pointer				*/
/*	             (or NULL to access anything)			*/
/*	secattr    = security attributes pointer			*/
/*	             (or NULL for kernel-only access)			*/
/*	initevent  = wait for this flag to set on initialize		*/
/*	             (or NULL if not to wait)				*/
/*	exitevent  = event flag to increment on exit			*/
/*	             (or NULL if none wanted)				*/
/*	stacksize  = number of pages to allocate for its user stack	*/
/*	             (or 0 for kernel mode only)			*/
/*	thentry    = entrypoint for thread routine			*/
/*	thparam    = parameter to pass to thread routine		*/
/*	knlastmode = initial kernel ast mode 				*/
/*	             (OZ_ASTMODE_INHIBIT or OZ_ASTMODE_ENABLE)		*/
/*	name       = thread name string					*/
/*									*/
/*	smplevel = softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_thread_create = OZ_SUCCESS : successful			*/
/*	                             else : error status		*/
/*	*thread_r = pointer to thread block				*/
/*									*/
/*    Note:								*/
/*									*/
/*	caller must call oz_knl_thread_increfc (thread, -1) when done 	*/
/*	with thread block pointer					*/
/*									*/
/************************************************************************/

uLong oz_knl_thread_create (OZ_Process *process, 
                            uLong priority, 
                            OZ_Seckeys *seckeys, 
                            OZ_Event *initevent, 
                            OZ_Event *exitevent, 
                            OZ_Mempage stacksize, 
                            OZ_Thread_entry thentry, 
                            void *thparam, 
                            OZ_Astmode knlastmode, 
                            int name_l, 
                            const char *name, 
                            OZ_Secattr *secattr, 
                            OZ_Thread **thread_r)

{
  char suspeventname[32];
  int j;
  OZ_Thread *parent, *thread;
  uLong sts, tp;

  OZ_KNL_CHKOBJTYPE (process,   OZ_OBJTYPE_PROCESS);
  OZ_KNL_CHKOBJTYPE (seckeys,   OZ_OBJTYPE_SECKEYS);
  OZ_KNL_CHKOBJTYPE (initevent, OZ_OBJTYPE_EVENT);
  OZ_KNL_CHKOBJTYPE (exitevent, OZ_OBJTYPE_EVENT);

  /* Can't allow null thentry because oz_hw_thread_initctx interprets that specially (besides it just being dumb) */

  if (thentry == NULL) {
    oz_knl_printk ("oz_knl_thread_create: null thentry parameter\n");
    return (OZ_BADPARAM);
  }

  /* Create a new thread block and fill it in */

  thread = OZ_KNL_NPPMALLOQ (sizeof *thread + OZ_HW_KSTACKINTHCTX * oz_s_loadparams.kernel_stack_size);
  if (thread == NULL) return (OZ_EXQUOTANPP);
  memset (thread, 0, sizeof *thread);
  thread -> objtype      = OZ_OBJTYPE_THREAD;
  thread -> refcount     = 2;				/* one count is undone by oz_knl_thread_wait when it wipes context from last CPU to have it */
							/* other count is undone when the caller calls oz_knl_thread_increfc (thread, -1) */
  thread -> basepri      = priority;			/* save base priority */
  thread -> curprio      = priority;			/* set current priority to be same as base priority to start with */
  thread -> nextrunquant = deltarunquant;		/* let it run a whole quantum to start */
  thread -> nextcomquant = deltacomquant;
  thread -> state        = OZ_THREAD_STATE_INI;		/* anything that makecom doesn't do special with */
  thread -> cpuidx       = -1;				/* we are about to set up the hw_ctx block so it will be valid */

  sts = oz_knl_event_create (16, "thread suspended", NULL, &(thread -> suspevent));
  if (sts != OZ_SUCCESS) {
    OZ_KNL_NPPFREE (thread);
    return (sts);
  }
  oz_knl_event_set (thread -> suspevent, 1);		/* put it in 'running' status to begin with */

  sts = oz_knl_event_create (9, "oz_sys_io", NULL, &(thread -> ioevent));
  if (sts != OZ_SUCCESS) {
    oz_knl_event_increfc (thread -> suspevent, -1);
    OZ_KNL_NPPFREE (thread);
    return (sts);
  }

  sts = oz_hw_thread_initctx (thread -> hw_ctx, stacksize, thentry, thparam, oz_knl_process_gethwctx (process), thread); /* inititalize the hw_ctx */
  if (sts != OZ_SUCCESS) {
    oz_knl_event_increfc (thread -> ioevent, -1);
    oz_knl_event_increfc (thread -> suspevent, -1);
    OZ_KNL_NPPFREE (thread);
    return (sts);
  }

  thread -> seckeys = seckeys;
  oz_knl_seckeys_increfc (thread -> seckeys, 1);

  thread -> secattr = secattr;
  thread -> defcresecattr = secattr;
  if (thread -> secattr != NULL) oz_knl_secattr_increfc (thread -> secattr, 2);

  for (j = 0; j <= OZ_PROCMODE_MAX - OZ_PROCMODE_MIN; j ++) {
    thread -> astmode[j]  = OZ_ASTMODE_ENABLE;		/* there are no asts queued, and normal ast delivery is enabled */
    thread -> astexpqt[j] = thread -> astexpqh + j;
    thread -> astnorqt[j] = thread -> astnorqh + j;
  }
  thread -> astmode[OZ_PROCMODE_KNL-OZ_PROCMODE_MIN] = knlastmode; /* set up kernel ast delivery mode */

  thread -> initevent = initevent;			/* set up initialization event flag */
  if (thread -> initevent != NULL) oz_knl_event_increfc (thread -> initevent, 1);

  thread -> exitevent = exitevent;			/* set up exit event flag */
  if (thread -> exitevent != NULL) oz_knl_event_increfc (thread -> exitevent, 1);

  if (name_l > sizeof thread -> name - 1) name_l = sizeof thread -> name - 1;
  movc4 (name_l, name, sizeof thread -> name, thread -> name);

  oz_hw_smplock_init (sizeof thread -> smplock_tp, &(thread -> smplock_tp), OZ_SMPLOCK_LEVEL_TP);

  /* Link it to process */

  sts = add_thread_to_process (thread, process, 1);	/* link thread to process' list of threads */
  if (sts != OZ_SUCCESS) {				/* maybe process is dead */
    if (thread -> ioevent   != NULL) oz_knl_event_increfc (thread -> ioevent,   -1);
    if (thread -> suspevent != NULL) oz_knl_event_increfc (thread -> suspevent, -1);
    if (thread -> initevent != NULL) oz_knl_event_increfc (thread -> initevent, -1);
    if (thread -> exitevent != NULL) oz_knl_event_increfc (thread -> exitevent, -1);
    if (thread -> seckeys   != NULL) oz_knl_seckeys_increfc (thread -> seckeys, -1);
    oz_hw_thread_termctx (thread -> hw_ctx, oz_knl_process_gethwctx (process));
    oz_knl_devunit_dallocall (&(thread -> devalloc));
    OZ_KNL_NPPFREE (thread);
    return (sts);
  }

  /* Make it a child of the current thread */

  parent = curthreads[oz_hw_cpu_getcur()];		// parent will be the current thread
  if (parent -> suspevent == NULL) parent = NULL;	// ... but orphan it if current thread is one of the 'cpu init' threads
  add_thread_to_parent (thread, parent);		// add thread to the parent

  /* Rename suspend event flag to something meaningful                            */
  /* A child might get suspended by this event flag so this makes it easy to tell */

  strcpy (suspeventname, "threadid ");
  oz_hw_itoa (thread -> threadid, 12, suspeventname + 9);
  strcat (suspeventname, " suspended");
  oz_knl_event_rename (thread -> suspevent, strlen (suspeventname), suspeventname);

  /* Make the thread computable, ie, let it go get a cpu to run on */

  tp = oz_hw_smplock_wait (&(thread -> smplock_tp));
  makecom (thread, 1);
  oz_hw_smplock_clr (&(thread -> smplock_tp), tp);

  *thread_r = thread;
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Add newly created thread to a process's threadq list		*/
/*									*/
/*    Input:								*/
/*									*/
/*	thread  = thread to be added					*/
/*	process = process to be added to				*/
/*									*/
/*	smplock = softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	thread -> process = set to the process				*/
/*	thread added to process -> threadq				*/
/*	process reference count incremented				*/
/*									*/
/************************************************************************/

static uLong add_thread_to_process (OZ_Thread *thread, OZ_Process *process, int ifnormal)

{
  OZ_Thread *oldtopthread, **threadqp;
  uLong ps;

  /* Make sure the process is accepting new threads */

  ps = oz_knl_process_lockps (process);						/* lock process state */
  threadqp = oz_knl_process_getthreadqp (process, ifnormal);			/* get threadqp = &(process -> threadq) */
  if (threadqp == NULL) {
    oz_knl_process_unlkps (process, ps);
    return (OZ_PROCESSCLOSED);							/* ... but only if process is in NORMAL state */
  }

  /* Link the thread to the process */

  thread -> process = process;							/* store process pointer in thread block */
  oz_knl_process_increfc (process, 1);						/* increment number of things that reference the process */
  oldtopthread = *threadqp;							/* get pointer to the old top thread */
  thread -> proc_next = oldtopthread;						/* make it follow this new one */
  thread -> proc_prev = threadqp;						/* prior to me is the listhead itself */
  if (oldtopthread != NULL) oldtopthread -> proc_prev = &(thread -> proc_next);	/* if there was an old top, make me the predecessor to it */
  *threadqp = thread;								/* the new one is on top now */
  oz_knl_process_unlkps (process, ps);
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Add newly created thread to it's parents list of children		*/
/*  Also assign an unique thread id number to the thread		*/
/*									*/
/*    Input:								*/
/*									*/
/*	thread = thread to be added					*/
/*	parent = parent to be added to (NULL to make it an orphan)	*/
/*									*/
/*	smplock <= tf							*/
/*									*/
/*    Output:								*/
/*									*/
/*	thread -> parent = set to the parent				*/
/*	thread -> siblingnext/prev = linked to parents other children	*/
/*	thread -> threadid = a unique number				*/
/*									*/
/************************************************************************/

static void add_thread_to_parent (OZ_Thread *thread, OZ_Thread *parent)

{
  OZ_Thread *sibling;
  uLong tf;

  /* Assign an unique thread-id to the thread */

  tf = oz_hw_smplock_wait (&smplock_tf);				// lock thread family database
  if (thread -> threadid == 0) thread -> threadid = oz_knl_idno_alloc (thread); // allocate an id number

  /* Link it to parent's list of children */

  thread -> parent = parent;						// save pointer to parent thread
  if (parent == NULL) {							// see if it is an orphan
    thread -> siblingnext = sibling = orphans;				// ok, make it a sibling of other orphans
    thread -> siblingprev = &orphans;
    orphans               = thread;
  } else {
    thread -> siblingnext = sibling = parent -> children;		// no, make it a sibling of parent's other children
    thread -> siblingprev = &(parent -> children);
    parent -> children    = thread;
  }
  if (sibling != NULL) sibling -> siblingprev = &(thread -> siblingnext); // if there is a sibling, link it back to this one

  oz_hw_smplock_clr (&smplock_tf, tf);
}

/************************************************************************/
/*									*/
/*  Find next sibling in a list, and find sub-siblings, too		*/
/*									*/
/*    Input:								*/
/*									*/
/*	thread = thread to find next (sub-)sibling of			*/
/*	parent = parent of top-level children (or NULL for orphans list)
/*	smplevel = tf							*/
/*									*/
/*    Output:								*/
/*									*/
/*	nextsibling = NULL : end of (sub-)siblings			*/
/*	              else : next (sub-)sibling				*/
/*									*/
/************************************************************************/

static OZ_Thread *nextsibling (OZ_Thread *thread, OZ_Thread *parent)

{
  OZ_Thread *nexthread;

  nexthread = thread -> children;			// see if it has any children
  if (nexthread != NULL) return (nexthread);		// if so, do the first one next
  do {
    nexthread = thread -> siblingnext;			// if not, see if it has a next sibling
    if (nexthread != NULL) return (nexthread);		// if so, to it next
    thread = thread -> parent;				// if not, see who its parent is
  } while (thread != parent);				// repeat if there is a parent
  return (NULL);					// no sibling or parent, it was the last orphan, so we're done
}

/************************************************************************/
/*									*/
/*  The oz_hw_thread_initctx sets up this routine to be called when a 	*/
/*  thread first runs.  It first runs when oz_knl_thread_wait calls 	*/
/*  oz_hw_thread_switchctx, which, instead of returning to 		*/
/*  oz_knl_thread_wait, returns to this routine.			*/
/*									*/
/*  The oz_knl_thread_start routine must undo everything		*/
/*  the oz_knl_thread_wait routine undoes before returning.		*/
/*  Those things have been put in woken_part1 and woken_part2.		*/
/*									*/
/*    Input:								*/
/*									*/
/*	thread = new thread						*/
/*	curthreads[this cpu] = old thread				*/
/*	old thread's smplock_tp set					*/
/*	new thread in COM state but removed from the COM queue		*/
/*									*/
/*    Output:								*/
/*									*/
/*	smp locks = none						*/
/*									*/
/************************************************************************/

void oz_knl_thread_start (OZ_Thread *thread)

{
  uLong sts, tp;
  OZ_Quota *quota;
  OZ_Thread *zthread;

  OZ_KNL_CHKOBJTYPE (thread, OZ_OBJTYPE_THREAD);

  /* It is as if oz_hw_thread_switchctx () was called here - so we must do */
  /* everything oz_knl_thread_wait routine does after it calls switchctx   */

  zthread = woken_part1 (thread);			// complete transition to RUN state
  woken_part2 (thread, zthread, thread -> process);	// finish up various things

  /* Ok, that's everything oz_knl_thread_wait undoes after it calls switchctx, so we can do anything we want to now */

  quota = oz_knl_quota_fromobj (thread);		// get thread's quota block pointer and inc ref count so it stays till we exit
  oz_knl_quota_setcpudef (quota);			// set this cpu's default quota block pointer

  oz_hw_cpu_setsoftint (1);				// enable softints because that's what we want threads to start at
							// - from this point on, thread can be aborted
  checkknlastq (thread, NULL);				// in case any kernel ast's are queued

  /* If there was an initevent defined, wait for it to be set if not already set.  Note */
  /* that kernel ast's can be delivered and the thread can be aborted during the wait.  */

  if (thread -> initevent != NULL) {				/* see if an initial event flag was specified for the thread */
    do sts = oz_knl_event_waitone (thread -> initevent);	/* if so, wait for it */
    while (sts != OZ_FLAGWASSET);				/* make sure it is really set */
    oz_hw_cpu_setsoftint (0);					/* in case an abort queues here, we don't want to */
								/* ... screw up init event flag's ref count */
    oz_knl_event_increfc (thread -> initevent, -1);		/* decrement init event flag reference count */
    thread -> initevent = NULL;					/* forget about it */
    oz_hw_cpu_setsoftint (1);
  }

  /* The oz_hw_thread_initctx routine should have set stuff up so this returns to a routine that: */
  /*  1) Switches to user mode (if a non-zero user stack size was given)                          */
  /*  2) Calls the thread's main routine at the given entrypoint with the given parameter         */
  /*  3) Calls oz_sys_thread_exit with the return value from the thread's main routine            */
  /*  4) Crashes if oz_sys_thread_exit returns                                                    */
}

/************************************************************************/
/*									*/
/*  Create an exit handler entry for a thread				*/
/*									*/
/*    Input:								*/
/*									*/
/*	exhentry = entry point for routine				*/
/*	exhparam = parameter to pass to exhentry routine		*/
/*	procmode = processor mode					*/
/*	thread = NULL : current thread					*/
/*	         else : the given thread				*/
/*	smplevel = softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	exit handler put on thread queue (LIFO)				*/
/*									*/
/************************************************************************/

uLong oz_knl_exhand_create (void (*exhentry) (void *exhparam, uLong status), void *exhparam, OZ_Procmode procmode, OZ_Thread *thread, OZ_Exhand **exhand_r)

{
  OZ_Exhand *exhand;
  uLong tp;

  OZ_KNL_CHKOBJTYPE (thread, OZ_OBJTYPE_THREAD);

  if (thread == NULL) thread = curthreads[oz_hw_cpu_getcur()];

  exhand = OZ_KNL_NPPMALLOQ (sizeof *exhand);
  if (exhand == NULL) return (OZ_EXQUOTANPP);
  exhand -> objtype  = OZ_OBJTYPE_EXHAND;
  exhand -> thread   = thread;
  exhand -> procmode = procmode;
  exhand -> exhentry = exhentry;
  exhand -> exhparam = exhparam;

  tp = oz_hw_smplock_wait (&(thread -> smplock_tp));
  exhand -> next = thread -> exhandq[procmode];
  thread -> exhandq[procmode] = exhand;
  oz_hw_smplock_clr (&(thread -> smplock_tp), tp);

  *exhand_r = exhand;
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Dequeue and delete an exit handler entry for the current thread	*/
/*									*/
/*    Input:								*/
/*									*/
/*	procmode = processor mode					*/
/*	smplevel = softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_exhand_dequeue = 0 : no exit handler defined		*/
/*	                        1 : exit handler found			*/
/*	*exhentry_r = entrypoint					*/
/*	*exhparam_r = parameter						*/
/*									*/
/************************************************************************/

int oz_knl_exhand_dequeue (OZ_Procmode procmode, OZ_Exhand_entry *exhentry_r, void **exhparam_r)

{
  OZ_Exhand *exhand;
  OZ_Thread *thread;
  uLong tp;

  thread = curthreads[oz_hw_cpu_getcur()];		/* get current thread pointer */
  tp = oz_hw_smplock_wait (&(thread -> smplock_tp));	/* lock thread database */
  exhand = thread -> exhandq[procmode];			/* get top exit handler for the processor mode */
  if (exhand != NULL) thread -> exhandq[procmode] = exhand -> next; /* if one found, unlink it */
  oz_hw_smplock_clr (&(thread -> smplock_tp), tp);	/* unlock thread database */
  if (exhand != NULL) {
    *exhentry_r = exhand -> exhentry;			/* return the entrypoint */
    *exhparam_r = exhand -> exhparam;			/* return the parameter */
    OZ_KNL_NPPFREE (exhand);				/* free it off */
  }
  return (exhand != NULL);				/* all done */
}

/************************************************************************/
/*									*/
/*  Delete an exit handler from a thread				*/
/*									*/
/*    Input:								*/
/*									*/
/*	smplevel = softint						*/
/*									*/
/************************************************************************/

void oz_knl_exhand_delete (OZ_Exhand *exh)

{
  OZ_Exhand *exhand, **lexhand;
  OZ_Thread *thread;
  uLong tp;

  OZ_KNL_CHKOBJTYPE (exh, OZ_OBJTYPE_EXHAND);

  thread = exh -> thread;
  tp = oz_hw_smplock_wait (&(thread -> smplock_tp));
  for (lexhand = &(thread -> exhandq[exh->procmode]); (exhand = *lexhand) != exh; lexhand = &(exhand -> next)) {
    if (exhand == NULL) oz_crash ("oz_knl_exhand_delete: couldn't find exit handler on thread's exit handler list");
  }
  *lexhand = exhand -> next;
  oz_hw_smplock_clr (&(thread -> smplock_tp), tp);

  OZ_KNL_NPPFREE (exhand);
}

/************************************************************************/
/*									*/
/*  Orphan a thread							*/
/*									*/
/*    Input:								*/
/*									*/
/*	thread = pointer to thread to orphan				*/
/*	smplevel <= tf							*/
/*									*/
/************************************************************************/

void oz_knl_thread_orphan (OZ_Thread *thread)

{
  OZ_Thread *sibling;
  uLong tf;

  OZ_KNL_CHKOBJTYPE (thread, OZ_OBJTYPE_THREAD);			// make sure they gave us a thread
  tf = oz_hw_smplock_wait (&smplock_tf);				// lock the thread family lists
  if (thread -> parent != NULL) {					// see if this thread has a parent
    *(thread -> siblingprev) = sibling = thread -> siblingnext;		// ok, remove it from parent's list of children
    if (sibling != NULL) sibling -> siblingprev = thread -> siblingprev;
    add_thread_to_parent (thread, NULL);				// ... then make it a child of the orphan list
    if (thread -> parexevent != NULL) {					// see if parent is waiting for us to orphan from it
      oz_knl_event_set (thread -> parexevent, 1);			// ... if so, set the event flag
      thread -> parexevent = NULL;					// ... and wipe pointer because the parent can't anymore
    }
  }
  oz_hw_smplock_clr (&smplock_tf, tf);					// release thread family lists
}

/************************************************************************/
/*									*/
/*  Suspend a thread and all its (sub-)children				*/
/*									*/
/*    Input:								*/
/*									*/
/*	thread = pointer to thread to suspend				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_thread_suspend = 0 : thread was not already marked for suspend
/*	                        1 : thread was already marked for suspend or is exiting
/*									*/
/************************************************************************/

int oz_knl_thread_suspend (OZ_Thread *thread)

{
  int wassusp;
  Long oldsusp;
  OZ_Ast *ast;
  OZ_Thread *child;
  uLong sts, tf;

  OZ_KNL_CHKOBJTYPE (thread, OZ_OBJTYPE_THREAD);

  wassusp = 1;											// assume it has exited
  tf = oz_hw_smplock_wait (&smplock_tf);
  if (thread -> suspevent != NULL) {								// see if it has
    oldsusp = oz_knl_event_set (thread -> suspevent, 0);					// no, clear suspending event flag
    if (oldsusp > 0) {
      sts = oz_knl_ast_create (thread, OZ_PROCMODE_KNL, suspend_ast, thread, 0, &ast);		// queue suspend ast
      if (sts == OZ_SUCCESS) oz_knl_thread_queueast (ast, OZ_SUCCESS);
      for (child = thread -> children; child != NULL; child = nextsibling (child, thread)) {	// suspend all its children
        sts = oz_knl_ast_create (child, OZ_PROCMODE_KNL, suspend_ast, thread, 0, &ast);		// ... and their children, etc
        if (sts == OZ_SUCCESS) oz_knl_thread_queueast (ast, OZ_SUCCESS);
      }
      wassusp = 0;										// return that is was not already suspended
    }
  }
  oz_hw_smplock_clr (&smplock_tf, tf);

  return (wassusp);
}

/************************************************************************/
/*									*/
/*  This routine runs in the target thread as a normal kernel ast.  It 	*/
/*  waits for the 'suspend' event flag.  Note that (sub-)children 	*/
/*  threads wait for the top parent's suspend event flag.  If this 	*/
/*  thread was the direct target of an oz_knl_thread_suspend call, 	*/
/*  then threadv will point to this thread.  If a parent was the 	*/
/*  target of the oz_knl_thread_suspend call, then threadv will point 	*/
/*  to that parent thread.						*/
/*									*/
/************************************************************************/

static void suspend_ast (void *threadv, uLong status, OZ_Mchargs *mchargs)

{
  OZ_Eventlist eventlist[1];
  OZ_Thread *thread;
  uLong tf;

  thread = threadv;

  while (1) {
    tf = oz_hw_smplock_wait (&smplock_tf);			// lock thread family database
    if (tf != OZ_SMPLOCK_NULL) oz_crash ("oz_knl_thread suspend_ast: called at smplevel %X", tf);
    eventlist[0].event = thread -> suspevent;			// see if (parent) thread still has a suspend event flag
    if (eventlist[0].event == NULL) break;			// if not, we don't have anything to wait for
    if (oz_knl_event_inc (eventlist[0].event, 0) > 0) break;	// if flag is set, we don't have anything to wait for
    oz_knl_event_increfc (eventlist[0].event, 1);		// flag is clear, make sure it doesn't get freed on us while we wait
    oz_hw_smplock_clr (&smplock_tf, OZ_SMPLOCK_SOFTINT);	// release thread family database
    oz_knl_event_waitlist (1, eventlist, OZ_PROCMODE_KNL, 1);	// wait for it to be set
    oz_knl_event_increfc (eventlist[0].event, -1);		// release it
    oz_hw_cpu_setsoftint (1);					// in case there are softints waiting
  }								// go test again
  oz_hw_smplock_clr (&smplock_tf, OZ_SMPLOCK_NULL);
}

/************************************************************************/
/*									*/
/*  Resume a suspended thread and all its (sub-)children		*/
/*									*/
/*    Input:								*/
/*									*/
/*	thread = pointer to thread to resume				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_thread_resume = 0 : thread was not marked for suspend or is exiting
/*	                       1 : thread was marked for suspend	*/
/*									*/
/************************************************************************/

int oz_knl_thread_resume (OZ_Thread *thread)

{
  int wassusp;
  Long oldsusp;
  uLong tf;

  OZ_KNL_CHKOBJTYPE (thread, OZ_OBJTYPE_THREAD);

  wassusp = 0;
  tf = oz_hw_smplock_wait (&smplock_tf);
  if (thread -> suspevent != NULL) {
    oldsusp = oz_knl_event_set (thread -> suspevent, 1);	// resume it by setting event flag
    if (oldsusp <= 0) wassusp = 1;				// return whether it was suspended
  }
  oz_hw_smplock_clr (&smplock_tf, tf);

  return (wassusp);
}

/************************************************************************/
/*									*/
/*  Abort a thread							*/
/*									*/
/*    Input:								*/
/*									*/
/*	thread   = pointer to thread block				*/
/*	abortsts = what to make the exit status				*/
/*									*/
/*	smplock <= tp							*/
/*									*/
/*    Output:								*/
/*									*/
/*	the thread will soon exit					*/
/*									*/
/************************************************************************/

void oz_knl_thread_abort (OZ_Thread *thread, uLong abortsts)

{
  OZ_Ast *ast;
  OZ_Quota *quota;
  uLong sts, tf, tp;

  OZ_KNL_CHKOBJTYPE (thread, OZ_OBJTYPE_THREAD);

  tp = oz_hw_smplock_wait (&(thread -> smplock_tp));			/* set thread state smp lock */
  if ((thread -> knlexit == 0) && !(thread -> abortpend)) {		/* don't bother trying if it's already exiting */
    quota = oz_knl_quota_setcpudef (NULL);				/* turn off quotas for abort ast's */
    sts = oz_knl_ast_create (thread, OZ_PROCMODE_KNL, thread_abort, NULL, 0, &ast); /* queue an normal ast to the thread */
									/* don't make this an express ast as routines depend */
									/* on the fact that inhibiting kernel mode ast       */
									/* delivery will prevent the thread from aborting    */
    if (sts != OZ_SUCCESS) oz_crash ("oz_knl_thread_abort: error %u creating ast", sts);
    oz_knl_thread_queueast (ast, abortsts);
    oz_knl_quota_setcpudef (quota);
    thread -> abortpend = 1;						/* remember an abort has been queued */
  }
  oz_hw_smplock_clr (&(thread -> smplock_tp), tp);			/* release thread state */

  tf = oz_hw_smplock_wait (&smplock_tf);				/* make sure it is not directly suspended */
  if (thread -> suspevent != NULL) oz_knl_event_set (thread -> suspevent, 1);
  oz_hw_smplock_clr (&smplock_tf, tf);					/* note that if it is suspended because a   */
									/* parent is suspended, the abort will have */
									/* to wait until that parent gets resumed   */
}

/* Kernel mode ast routine called in the target thread context - it just exits with the abort status */

static void thread_abort (void *dummy, uLong abortsts, OZ_Mchargs *mchargs)

{
  oz_knl_thread_exit (abortsts);
}

/************************************************************************/
/*									*/
/*  This routine is called by a thread when it wants to exit		*/
/*									*/
/*    Input:								*/
/*									*/
/*	status    = exit status						*/
/*	astmode   = unknown (ENABLE or INHIBIT)				*/
/*	smp locks = none						*/
/*	ipl       = none						*/
/*									*/
/*    Output:								*/
/*									*/
/*	thread exit event flag is set					*/
/*	thread state eventually set to OZ_THREAD_STATE_ZOM		*/
/*									*/
/*	this routine does not return to caller except if it has been 	*/
/*	called already for this thread, like when a second 		*/
/*	oz_knl_thread_abort call has been made for the same thread	*/
/*									*/
/************************************************************************/

void oz_knl_thread_exit (uLong status)

{
  OZ_Ast *ast;
  OZ_Event *event;
  OZ_Exhand *exhand;
  OZ_Process *process;
  OZ_Procmode pm;
  OZ_Quota *quota;
  OZ_Thread *thread, *zthread;
  uLong tf, tp;
  void (*exhentry) (void *exhparam, uLong status);
  void *exhparam;

  /* Only come through here once per thread - this avoids the subsequent */
  /* rundown stuff from being aborted which can mess things up.          */

  if (!oz_hw_cpu_setsoftint (0)) {				/* inhib softints so we get correct current thread */
    oz_crash ("oz_knl_thread_exit: called with softint delivery inhibited");
  }
  thread = curthreads[oz_hw_cpu_getcur()];			/* get my thread pointer */
  oz_hw_smplock_wait (&(thread -> smplock_tp));			/* lock thread state */
  process = oz_knl_process_getcur ();				/* see what process is mapped to memory */
  if (thread -> process != process) {				/* they had better match up so we clean up the right process */
    oz_crash ("oz_knl_thread_exit: current process %p doesn't match current thread %p -> process %p", process, thread, thread -> process);
  }
  if (thread -> knlexit != 0) {					/* ok, see if i've been here before on this thread */
    oz_hw_smplock_clr (&(thread -> smplock_tp), OZ_SMPLOCK_NULL); /* release thread state */
    return;							/* if not first time on this thread, return to caller */
  }
  thread -> exitsts = status;					/* if not, save the exit status */
  thread -> knlexit = 1;					/* set the i've been here before flag */
  oz_hw_smplock_clr (&(thread -> smplock_tp), OZ_SMPLOCK_SOFTINT); /* release thread state */

  /* From now on, with knlexit set, if an abort ast gets queued, the exit will just return and we will keep going */

  /* Clear the current cpu's quota block pointer.                  */
  /*  1) this is how we release this thread's grip on the quota    */
  /*     block (its refcount was inc'd in oz_knl_thread_start)     */
  /*  2) we do it here so the rundown stuff won't be subject to    */
  /*     quota limits, after all, we are trying to kill things off */

  quota = oz_knl_quota_setcpudef (NULL);			/* wipe thread's quota block (as set up in thread_start routine) */
  if (quota != NULL) oz_knl_quota_increfc (quota, -1);		/* free off the reference to the quota block */

  /* Let any waiting kernel mode ast's execute */

  oz_hw_cpu_setsoftint (1);					// kernel mode ast's (and all ast's for that matter) execute with softint delivery enabled
  checkknlastq (thread, NULL);					// execute them

  /* Free off left-over outer mode exit handlers */

  oz_hw_smplock_wait (&(thread -> smplock_tp));			/* set smp lock to lock the exit handler lists */
  for (pm = OZ_PROCMODE_MIN; pm <= OZ_PROCMODE_MAX; pm ++) {
    if (pm == OZ_PROCMODE_KNL) continue;			/* skip kernel list for now */
    while ((exhand = thread -> exhandq[pm-OZ_PROCMODE_MIN]) != NULL) { /* see if anything on list */
      thread -> exhandq[pm-OZ_PROCMODE_MIN] = exhand -> next;	/* if so, unlink the top one */
      OZ_KNL_NPPFREE (exhand);					/* free off exit handler block */
    }								/* repeat to get more */
  }

  /* Call kernel mode exit handlers */

  while ((exhand = thread -> exhandq[OZ_PROCMODE_KNL-OZ_PROCMODE_MIN]) != NULL) { /* see if anything on list */
    thread -> exhandq[OZ_PROCMODE_KNL-OZ_PROCMODE_MIN] = exhand -> next; /* if so, unlink the top one */
    exhentry = exhand -> exhentry;				/* get the entrypoint and parameter */
    exhparam = exhand -> exhparam;
    OZ_KNL_NPPFREE (exhand);					/* free off exit handler block */
    oz_hw_smplock_clr (&(thread -> smplock_tp), OZ_SMPLOCK_NULL); /* release smp lock, allow softints in exit handler */
    (*exhentry) (exhparam, status);				/* call the routine */
    oz_hw_smplock_wait (&(thread -> smplock_tp));		/* get smp lock back and re-check list */
  }
  oz_hw_smplock_clr (&(thread -> smplock_tp), OZ_SMPLOCK_SOFTINT); /* exit handler lists empty, release smp lock, inhib softints */

  /* Abort all children and wait for them to exit */

  tf = oz_hw_smplock_wait (&smplock_tf);			// lock children list
  event = thread -> suspevent;					// steal the suspension event flag
  if (event == NULL) {						// cpuinit threads don't have suspension event flag
    while ((zthread = thread -> children) != NULL) oz_knl_thread_orphan (zthread); // for cpuinit threads, orphan them all
  } else {
    thread -> suspevent = NULL;					// let children know not to suspend on this anymore
    for (zthread = thread -> children; zthread != NULL; zthread = zthread -> siblingnext) {
      oz_knl_thread_abort (zthread, OZ_PARENTEXITED);		// queue aborts to all direct children
    }
    oz_knl_event_set (event, 1);				// set event in case some child was suspended by it
    while ((zthread = thread -> children) != NULL) {		// see if there (still) are any children to wait for
      zthread -> parexevent = event;				// ok, tell it we are waiting for it to orphan from us
      oz_hw_smplock_clr (&smplock_tf, tf);			// release the lock
      do oz_knl_event_waitone (event);				// wait for it to orphan from us (as part of its exit sequence)
      while (oz_knl_event_set (event, 0) == 0);
      tf = oz_hw_smplock_wait (&smplock_tf);			// lock list again
    }
    oz_knl_event_increfc (event, -1);				// release suspension event flag
  }
  oz_hw_smplock_clr (&smplock_tf, OZ_SMPLOCK_SOFTINT);		// unlock children list, keep softints inhibited

  /* Abort all I/O started by this thread that hasn't completed yet */

  oz_knl_iorundown (thread, OZ_PROCMODE_KNL);

  /* Close all handles opened by this thread */

  oz_knl_handle_release_all (thread, OZ_PROCMODE_KNL);

  /* Don't let any more ast's (especially kernel mode) queue */

  thread -> knlexit = -1;
  OZ_HW_MB;

  /* Tell the hardware we're done with everything (like the user mode stack) (except the stack we're on) */

  oz_hw_thread_exited (thread -> hw_ctx);

  /* Clean up everything we can about the thread */

  /* - orphan from parent so it won't bother aborting me when it exits -- i'm already on the way out */
  /*   this also sets the parexevent if the parent is waiting for us to orphan since it is exiting   */

  oz_knl_thread_orphan (thread);

  /* - increment the exit event flag (don't unlink in case someone calls oz_knl_thread_getexitevent) */

  if (thread -> exitevent != NULL) oz_knl_event_inc (thread -> exitevent, 1);

  /* - unlink from init event flag if not already */

  if (thread -> initevent != NULL) {
    oz_knl_event_increfc (thread -> initevent, -1);
    thread -> initevent = NULL;
  }

  /* - get rid of I/O event flag */

  oz_knl_event_increfc (thread -> ioevent, -1);
  thread -> ioevent = NULL;

  /* - delete all left-over ast's -                    */
  /*   with the knlexit flag set, no more should queue */

  tp = oz_hw_smplock_wait (&(thread -> smplock_tp));
  for (pm = OZ_PROCMODE_MIN; pm <= OZ_PROCMODE_MAX; pm ++) {
    while ((ast = thread -> astexpqh[pm-OZ_PROCMODE_MIN]) != NULL) {
      oz_knl_ast_remove (ast);
      if (thread -> astexpqh[pm-OZ_PROCMODE_MIN] == NULL) thread -> astexpqt[pm-OZ_PROCMODE_MIN] = &(thread -> astexpqh[pm-OZ_PROCMODE_MIN]);
      oz_knl_ast_delete (ast, NULL, NULL, NULL);
    }
    while ((ast = thread -> astnorqh[pm-OZ_PROCMODE_MIN]) != NULL) {
      oz_knl_ast_remove (ast);
      if (thread -> astnorqh[pm-OZ_PROCMODE_MIN] == NULL) thread -> astnorqt[pm-OZ_PROCMODE_MIN] = &(thread -> astnorqh[pm-OZ_PROCMODE_MIN]);
      oz_knl_ast_delete (ast, NULL, NULL, NULL);
    }
  }
  oz_hw_smplock_clr (&(thread -> smplock_tp), tp);

  /* - delay getting rid of security attributes until the thread block is deleted so we can let others get our exit status */

  /* Get rid of process mapping in case another cpu/thread wipes our real process context */

  oz_knl_process_setcur (oz_s_systemproc);

  /* We must delay deleting the thread block itself until the cpu has released its context.  This */
  /* is the purpose of the OZ_THREAD_STATE_ZOM state.  When another thread comes along that wants */
  /* to run on this cpu, this cpu will release the exiting thread's context to load the new       */
  /* thread's context.  This is where we delete the exiting thread's block and finish with it.    */

  while (1) {

    /* If all other threads of the process are zombied, do process cleanup before zombiing this one */
    /* Initiating process cleanup will prevent any more threads from attaching to the process       */

    oz_knl_process_lockps (process);							/* lock process state */
    for (zthread = *oz_knl_process_getthreadqp (process, 0); zthread != NULL; zthread = zthread -> proc_next) { /* loop through the list of threads */
      if (zthread == thread) continue;							/* don't count this thread */
      if (zthread -> state != OZ_THREAD_STATE_ZOM) break;				/* stop if one found that isn't a ZOMbie */
    }
    if (zthread == NULL) oz_knl_process_cleanup (process);				/* everyone zombied, clean up process */
    else oz_knl_process_unlkps (process, OZ_SMPLOCK_SOFTINT);				/* non-zombie found, let it clean up */

    /* Zombie the exited thread */

    do {
      changestate (thread, OZ_THREAD_STATE_ZOM);					/* make it zombie */	
      oz_knl_thread_wait ();								/* it returns if, while waiting on the cpu, a lowipl routine */
											/* came along and changed the current thread's state to RUN */
    } while (zthread == NULL);								/* don't try cleanup again if we tried it once already, just re-zombie */
  }
}

/************************************************************************/
/*									*/
/*  Get thread's default I/O event flag					*/
/*									*/
/*    Input:								*/
/*									*/
/*	procmode  = processor mode for the event flag			*/
/*	smplevel >= softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_thread_ioevent = the event flag				*/
/*									*/
/************************************************************************/

OZ_Event *oz_knl_thread_ioevent (void)

{
  OZ_Thread *thread;

  thread = curthreads[oz_hw_cpu_getcur()];	/* get current thread on the cpu */
  return (thread -> ioevent);			/* return its I/O event flag */
}

/************************************************************************/
/*									*/
/*  Increment thread's IO or Pagefount count				*/
/*									*/
/************************************************************************/

uLong oz_knl_thread_incios (OZ_Thread *thread, uLong inc)

{
  if (thread == NULL) thread = curthreads[oz_hw_cpu_getcur()];
  return (oz_hw_atomic_inc_ulong (&(thread -> numios), inc));
}

uLong oz_knl_thread_incpfs (OZ_Thread *thread, uLong inc)

{
  if (thread == NULL) thread = oz_knl_thread_getcur ();		// we might get called at smplevel 0
  if (thread == NULL) return (0);				// maybe there aren't any threads yet
  return (oz_hw_atomic_inc_ulong (&(thread -> numpfs), inc));	// ok, increment pagefault count
}

/************************************************************************/
/*									*/
/*  Get thread's id number						*/
/*									*/
/*    Input:								*/
/*									*/
/*	thread = thread to get id of					*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_thread_getid = thread id number				*/
/*									*/
/************************************************************************/

OZ_Threadid oz_knl_thread_getid (OZ_Thread *thread)

{
  if (thread == NULL) thread = oz_knl_thread_getcur ();
  OZ_KNL_CHKOBJTYPE (thread, OZ_OBJTYPE_THREAD);	/* make sure they passed a thread */
  return (thread -> threadid);				/* return id number */
}

/************************************************************************/
/*									*/
/*  Get thread's name string						*/
/*									*/
/*    Input:								*/
/*									*/
/*	thread = thread to get name of					*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_thread_getname = pointer to thread name string		*/
/*									*/
/************************************************************************/

const char *oz_knl_thread_getname (OZ_Thread *thread)

{
  if (thread == NULL) thread = oz_knl_thread_getcur ();
  OZ_KNL_CHKOBJTYPE (thread, OZ_OBJTYPE_THREAD);	/* make sure they passed a thread */
  return (thread -> name);				/* return pointer to name string */
}

/************************************************************************/
/*									*/
/*  Get thread's parent thread						*/
/*									*/
/*    Input:								*/
/*									*/
/*	thread = thread to get parent of				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_thread_getparent = NULL : thread is an orphan		*/
/*	                          else : pointer to parent thread	*/
/*	                                 (ref count incremented)	*/
/*									*/
/************************************************************************/

OZ_Thread *oz_knl_thread_getparent (OZ_Thread *thread)

{
  OZ_Thread *parent;
  uLong tf;

  if (thread == NULL) thread = oz_knl_thread_getcur ();
  OZ_KNL_CHKOBJTYPE (thread, OZ_OBJTYPE_THREAD);		/* make sure they passed a thread */

  tf = oz_hw_smplock_wait (&smplock_tf);			/* don't let it orphan on us */
  parent = thread -> parent;					/* see what its parent is */
  if (parent != NULL) oz_knl_thread_increfc (parent, 1);	/* inc parent's ref count (in case thread orphans on us) */
  oz_hw_smplock_clr (&smplock_tf, tf);				/* let it orphan */
  return (parent);						/* return pointer to name string */
}

/************************************************************************/
/*									*/
/*  Get thread's exit status						*/
/*									*/
/*    Input:								*/
/*									*/
/*	thread = thread to get status of				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_thread_getstatus = OZ_SUCCESS : successful completion	*/
/*	                       OZ_FLAGWASCLR : thread has not exited	*/
/*	*exitsts_r = exit status					*/
/*									*/
/************************************************************************/

uLong oz_knl_thread_getexitsts (OZ_Thread *thread, uLong *exitsts_r)

{
  OZ_KNL_CHKOBJTYPE (thread, OZ_OBJTYPE_THREAD);	/* make sure they passed a thread */
  if (thread -> knlexit == 0) return (OZ_FLAGWASCLR);	/* make sure the thread has exited */
  *exitsts_r = thread -> exitsts;			/* it has exited, return exit status */
  return (OZ_SUCCESS);					/* return success status */
}

/************************************************************************/
/*									*/
/*  Get thread's exit event flag					*/
/*									*/
/*    Input:								*/
/*									*/
/*	thread = thread to get event flag of				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_thread_getexitevent = event flag pointer (or NULL if none)
/*									*/
/************************************************************************/

OZ_Event *oz_knl_thread_getexitevent (OZ_Thread *thread)

{
  OZ_KNL_CHKOBJTYPE (thread, OZ_OBJTYPE_THREAD);	/* make sure they passed a thread */
  return (thread -> exitevent);				/* return exit event flag pointer */
}

/************************************************************************/
/*									*/
/*  Get an event that the thread is waiting for				*/
/*									*/
/*    Input:								*/
/*									*/
/*	thread = thread to get event flag for				*/
/*	index  = event flag array index to get				*/
/*	smplevel <= tp							*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_thread_getwevent = NULL : thread is not waiting for event flag
/*	                                 or index is out of range	*/
/*	                          else : pointer to event flag		*/
/*									*/
/*    Note:								*/
/*									*/
/*	Caller must decrement event flag ref count when done with 	*/
/*	pointer								*/
/*									*/
/************************************************************************/

OZ_Event *oz_knl_thread_getwevent (OZ_Thread *thread, uLong index)

{
  OZ_Event *event;
  uLong tp;

  if (thread == NULL) thread = oz_knl_thread_getcur ();
  OZ_KNL_CHKOBJTYPE (thread, OZ_OBJTYPE_THREAD);				/* make sure they passed a thread */
  event = NULL;									/* assume we won't return anything */
  tp = oz_hw_smplock_wait (&(thread -> smplock_tp));				/* lock thread's database */
  if ((thread -> state == OZ_THREAD_STATE_WEV) && (index < thread -> nevents)) { /* see if thread is waiting and index is in range */
    event = thread -> eventlist[index].event;					/* if so, get pointer to the event flag */
    oz_knl_event_increfc (event, 1);						/* inc its ref count so it won't be deleted */
										/* caller will have to decrement it when done with pointer */
  }
  oz_hw_smplock_clr (&(thread -> smplock_tp), tp);				/* anyway, release lock */
  return (event);								/* return NULL or pointer to event flag */
}

/************************************************************************/
/*									*/
/*  Increment thread reference count					*/
/*									*/
/*    Input:								*/
/*									*/
/*	thread = pointer to thread					*/
/*	inc    = 1 : increment reference count				*/
/*	        -1 : decrement reference count				*/
/*	         0 : no-op						*/
/*									*/
/*	smplock : if refcount stays above 0, anything			*/
/*	          if refcount might go zero, <= ps			*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_thread_increfc = new ref count				*/
/*	if new ref count == 0, thread block is freed off		*/
/*									*/
/************************************************************************/

Long oz_knl_thread_increfc (OZ_Thread *thread, Long inc)

{
  int pm;
  Long refc;
  OZ_Process *process;
  OZ_Thread *nextthread, **prevthread, *sibling, **threadqp;
  uLong ps, tf;

  OZ_KNL_CHKOBJTYPE (thread, OZ_OBJTYPE_THREAD);

  /* Increment refcount atomically.  But if going zero, get the ps (process state) and */
  /* tf (thread family) locks first so we can remove it from lists before freeing it.  */

again:
  do {
    refc = thread -> refcount;							// sample the refcount
    if (refc <= 0) oz_crash ("oz_knl_thread_increfc: %p refcount was %d", thread, refc);
    if (refc + inc <= 0) goto going_le_zero;					// see if it is going zero
  } while (!oz_hw_atomic_setif_long (&(thread -> refcount), refc + inc, refc));	// if not, write it
  return (refc + inc);								// ... and return the new value

going_le_zero:
  if (refc + inc < 0) oz_crash ("oz_knl_thread_increfc: %p new refcount %d+%d negative", thread, refc, inc);
  process = thread -> process;							// get what process it belongs to
  ps = oz_knl_process_lockps (process);						// lock process state
  tf = oz_hw_smplock_wait (&smplock_tf);					// lock thread family
  if (!oz_hw_atomic_setif_long (&(thread -> refcount), 0, refc)) {		// write new zero refcount
    oz_hw_smplock_clr (&smplock_tf, tf);					// refcount changed on us
    oz_knl_process_unlkps (process, ps);					// ... go do it all again
    goto again;
  }

  /* Thread ref count now zero, make sure everything is what we expect */

  if (thread -> state != OZ_THREAD_STATE_ZOM) oz_crash ("oz_knl_thread_increfc: %p ref count zero but not zombie (state %d)", thread, thread -> state);
  if (thread -> cpuidx >= 0) oz_crash ("oz_knl_thread_increfc: %p ref count zero but cpuidx not neg, still %d", thread, thread -> cpuidx);
  for (pm = 0; pm <= OZ_PROCMODE_MAX - OZ_PROCMODE_MIN; pm ++) {
    if (thread -> astexpqh[pm] != NULL) oz_crash ("oz_knl_thread_increfc: %p ref count zero but astexpq[%d] not empty", thread, pm);
    if (thread -> astnorqh[pm] != NULL) oz_crash ("oz_knl_thread_increfc: %p ref count zero but astnorq[%d] not empty", thread, pm);
  }
  if (thread -> initevent  != NULL) oz_crash ("oz_knl_thread_increfc: %p ref count zero but has init event", thread);
  if (thread -> suspevent  != NULL) oz_crash ("oz_knl_thread_increfc: %p ref count zero but has suspension event", thread);
  if (thread -> ioopq      != NULL) oz_crash ("oz_knl_thread_increfc: %p ref count zero but has i/o operations", thread);
  if (thread -> parent     != NULL) oz_crash ("oz_knl_thread_increfc: %p ref count zero but has a parent", thread);
  if (thread -> parexevent != NULL) oz_crash ("oz_knl_thread_increfc: %p ref count zero but has a parent exit event", thread);
  if (thread -> children   != NULL) oz_crash ("oz_knl_thread_increfc: %p ref count zero but has children", thread);

  /* Ok, finish cleaning up and delete thread block */

  *(thread -> siblingprev) = sibling = thread -> siblingnext;	/* remove it from orphan list */
  if (sibling != NULL) sibling -> siblingprev = thread -> siblingprev;
  oz_knl_idno_free (thread -> threadid);			/* release the id number */
  oz_hw_smplock_clr (&smplock_tf, tf);				/* release thread family smp lock */

  threadqp = oz_knl_process_getthreadqp (process, 0);		/* remove it from process' list of threads */
  thread -> process = NULL;
  nextthread = thread -> proc_next;
  prevthread = thread -> proc_prev;
  *prevthread = nextthread;
  if (nextthread != NULL) nextthread -> proc_prev = prevthread;
  oz_knl_process_unlkps (process, ps);				/* unlock process state */

  oz_hw_thread_termctx (thread -> hw_ctx, oz_knl_process_gethwctx (process)); /* terminate hardware context block */
  oz_knl_process_increfc (process, -1);				/* decrement the process ref count */
  oz_knl_seckeys_increfc (thread -> seckeys, -1);		/* decrement security keys ref count */
  if (thread -> exitevent != NULL) oz_knl_event_increfc (thread -> exitevent, -1); /* dec exit event ref count */
  oz_knl_devunit_dallocall (&(thread -> devalloc));		/* deallocate any devices */
  OZ_KNL_NPPFREE (thread);					/* free off thread block */
  return (0);							/* return new ref count */
}

/************************************************************************/
/*									*/
/*  Queue an ast to a thread						*/
/*									*/
/*    Input:								*/
/*									*/
/*	ast    = pointer to ast block					*/
/*	aststs = status to deliver to ast routine			*/
/*									*/
/*	smplock <= tp							*/
/*									*/
/*    Output:								*/
/*									*/
/*	ast queued to thread						*/
/*	thread made computable if not already				*/
/*	thread interrupted to execute ast				*/
/*									*/
/************************************************************************/

void oz_knl_thread_queueast (OZ_Ast *ast, uLong aststs)

{
  int express, wasempty;
  OZ_Procmode procmode;
  OZ_Thread *thread;
  uLong tp;

  OZ_KNL_CHKOBJTYPE (ast, OZ_OBJTYPE_AST);

  thread = oz_knl_ast_getthread (ast);
  tp = oz_hw_smplock_wait (&(thread -> smplock_tp));

  /* If the thread has died, just free off the ast */

  if (thread -> knlexit < 0) {
    oz_hw_smplock_clr (&(thread -> smplock_tp), tp);
    oz_knl_ast_delete (ast, NULL, NULL, NULL);
    return;
  }

  /* Link ast block to end of thread's appropriate ast queue */

  oz_knl_ast_setstatus (ast, aststs);
  procmode = oz_knl_ast_getprocmode (ast);
  express  = oz_knl_ast_getexpress  (ast);
  if (express) {
    wasempty = (thread -> astexpqh[procmode-OZ_PROCMODE_MIN] == NULL);
    thread -> astexpqt[procmode-OZ_PROCMODE_MIN] = oz_knl_ast_insert (ast, thread -> astexpqt[procmode-OZ_PROCMODE_MIN]);
  } else {
    wasempty = (thread -> astnorqh[procmode-OZ_PROCMODE_MIN] == NULL);
    thread -> astnorqt[procmode-OZ_PROCMODE_MIN] = oz_knl_ast_insert (ast, thread -> astnorqt[procmode-OZ_PROCMODE_MIN]);
  }
#if OZ_DEBUG
  oz_knl_ast_setqinfo (ast, thread -> state, thread -> astmode[procmode-OZ_PROCMODE_MIN], thread -> cpuidx, thread -> hw_ctx, wasempty);
#endif

  /* Make thread executable and tell it there is an ast to be executed */
  /* Also tell hardware that an ast is now deliverable for the thread  */

  if (wasempty && (thread -> astmode[procmode-OZ_PROCMODE_MIN] != OZ_ASTMODE_INHEXP) && (express || (thread -> astmode[procmode-OZ_PROCMODE_MIN] == OZ_ASTMODE_ENABLE))) {
    oz_hw_thread_aststate (thread -> hw_ctx, procmode, 1, thread -> cpuidx);
    switch (thread -> state) {

      /* If it is already executing on a cpu (including this one), tell it something */
      /* has happened.  This basically takes it out of RUN state, puts it briefly    */
      /* in COM, then back in RUN state, but calls any pending ast routines.         */

      /* This causes routine oz_knl_thread_handleint to be executed on the target cpu */

      case OZ_THREAD_STATE_RUN: {
        oz_hw_cpu_reschedint (thread -> cpuidx);
        break;
      }

      /* If it is waiting for an event, make it computable so it will */
      /* execute the ast.  Note that wakewev will interrupt to make a */
      /* cpu execute the thread if a cpu is available for use.        */

      case OZ_THREAD_STATE_WEV: {
        oz_knl_thread_wakewev (thread, OZ_ASTDELIVERED);	// thread is being woken because ast was delivered, 
        break;							// ... not because event flag was set
      }

      /* If state is OZ_THREAD_STATE_COM, just leave it as is       */
      /* It will get a cpu to execute the ast when one is available */

      case OZ_THREAD_STATE_COM: {
        break;
      }

      /* We should never see the ZOM state, as the knlexit flag gets set long before state gets set to ZOM */

      default: {
        oz_crash ("oz_knl_thread_queueast: unknown thread state %d", thread -> state);
      }
    }
  }

  oz_hw_smplock_clr (&(thread -> smplock_tp), tp);
}

/************************************************************************/
/*									*/
/*  This routine is called by the hardware layer routines when the 	*/
/*  quantum timer started by the oz_hw_quantimer_start routine runs 	*/
/*  out.								*/
/*									*/
/*    Input:								*/
/*									*/
/*	cpuidx = cpu index of timer that ran out			*/
/*	         (does not have to be the current cpu)			*/
/*	OZ_SMPLOCK_SOFTINT <= smplock <= ts				*/
/*									*/
/************************************************************************/

void oz_knl_thread_quantimex (Long cpuidx)

{
  OZ_Iotatime iotanow, timeincom;
  OZ_Thread **loldcomthread, *nexthread, *oldcomthreads, *thread;
  uLong tc;

  /* Increment the priority of any threads sitting in COM too long (so they don't get choked out of cpu time) */

  iotanow = oz_hw_tod_iotanow ();
  tc = oz_hw_smplock_wait (&smplock_tc);				// lock the threadq_com queue
  validate_com_queue ();
  loldcomthread = &oldcomthreads;					// this is where we build list of threads we change
  for (thread = threadq_com; thread != NULL; thread = nexthread) {	// loop through the existing COM queue
    nexthread = thread -> statenext;					// save pointer to next in COM queue
    timeincom = thread -> timeinstate[OZ_THREAD_STATE_COM] + iotanow;	// get how much total time thread has spent in COM state
    if ((timeincom >= thread -> nextcomquant) && (thread -> curprio < thread -> basepri)) { // see if it needs a boost
      REMTHREADFROMQ (thread);						// ok, remove it from com queue
      thread -> curprio ++;						// increment its prioriti
      thread -> nextcomquant += deltacomquant;				// increment how much total COM time it needs for another boost
      *loldcomthread = thread;						// link it to end of temp queue, keeping it in order with others
      loldcomthread  = &(thread -> statenext);
    }
  }
  *loldcomthread = NULL;						// null terminate the temp queue

  loldcomthread = (OZ_Thread **)&threadq_com;
  while ((thread = oldcomthreads) != NULL) {				// get highest priority item from temp queue
    oldcomthreads = thread -> statenext;				// unlink it from temp queue
    while ((nexthread = *loldcomthread) != NULL) {			// skip through real COM queue
      if (nexthread -> curprio < thread -> curprio) break;		// find insertion point
      loldcomthread = &(nexthread -> statenext);
    }
    INSTHREADINTOQ (thread, loldcomthread);				// insert just before one of lower priority
    loldcomthread = &(thread -> statenext);				// bump live COM queue pointer just past inserted one
  }
  validate_com_queue ();
  oz_hw_smplock_clr (&smplock_tc, tc);					// all done with COM queue

  /* Now resched the thread who's quantum ran out                                       */
  /* Do it via softint in case we're buried in an lowipl routine or something like that */

  oz_hw_cpu_reschedint (cpuidx);
}

/************************************************************************/
/*									*/
/*  This routine is called by the hardware routines as a result of an 	*/
/*  oz_hw_cpu_reschedint call.  Its delivery must be blocked if ANY 	*/
/*  smplocks are set by the target cpu until the target cpu unlocks 	*/
/*  all smplocks.							*/
/*									*/
/*    Input:								*/
/*									*/
/*	smp locks = none						*/
/*	ipl = softint							*/
/*									*/
/*    Note:								*/
/*									*/
/*	This routine may lower the ipl to null, but it always returns 	*/
/*	at softint level.						*/
/*									*/
/*	This routine is invoked under the following circumstances:	*/
/*									*/
/*	 1) Target cpu is idle and a new thread has been placed in the 	*/
/*	    OZ_THREAD_STATE_COM state, ie, it needs a cpu to run on	*/
/*									*/
/*	 2) Target cpu is running a thread, but a higher priority 	*/
/*	    thread has just been placed in the OZ_THREAD_STATE_COM 	*/
/*	    state, so it will replace the current thread		*/
/*									*/
/*	 3) Target cpu is running a thread, and a new ast has been 	*/
/*	    queued to the thread.  Here, the thread is recycled 	*/
/*	    through the OZ_THREAD_STATE_COM state, and placed right 	*/
/*	    back in OZ_THREAD_STATE_RUN, but a check is made for 	*/
/*	    kernel mode ast's.						*/
/*									*/
/*	The caller must check for outermode deliverable ast's.		*/
/*									*/
/************************************************************************/

void oz_knl_thread_handleint (void)

{
  int resched;
  Long curcpu;
  OZ_Iotatime timeinrun;
  OZ_Procmode procmode;
  OZ_Thread *thread, *topcom;
  uLong tc, tp;

  /* See what thread is current on this cpu */

  curcpu = oz_hw_cpu_getcur ();
  thread = curthreads[curcpu];
  if (thread -> cpuidx != curcpu) oz_crash ("oz_knl_thread_handleint: thread %p -> cpuidx %d != %d", thread, thread -> cpuidx, curcpu);

  /* There must not be any smp locks set by this cpu as this is a     */
  /* requirement of the oz_knl_thread_wait routine - and we certainly */
  /* don't want to interrupt a routine that has its tp lock set       */

  tp = oz_hw_smplock_wait (&(thread -> smplock_tp));				// lock the thread's state
  if (tp != OZ_SMPLOCK_SOFTINT) oz_crash ("oz_knl_thread_handleint: called at smplevel %u", tp);

  /* If the thread's quantum ran out, decrement its prioriti */

  resched = 0;
  if (thread -> state == OZ_THREAD_STATE_RUN) {
    timeinrun  = oz_hw_tod_iotanow ();						// calculate thread's total time in RUN state
    timeinrun += thread -> timeinstate[OZ_THREAD_STATE_RUN];
    while (timeinrun >= thread -> nextrunquant) {				// see if thread ran its quantum out
      if (thread -> curprio > 1) thread -> curprio --;				// decrement priority, but not to 0 (0 is reserved for the 'cpu idle' threads)
      thread -> nextrunquant += deltarunquant;					// reset quantum for next time
      thread -> nextwevquant  = thread -> timeinstate[OZ_THREAD_STATE_WEV] + deltawevquant; // it will have to accumulate quanta of wev time since now to get its priority boosted back up
      resched = 1;
    }
    if (thread -> cpuidx < 0) oz_crash ("oz_knl_thread_handleint: thread %p -> cpuidx %d", thread, thread -> cpuidx);
    curprios[curcpu] = thread -> curprio;					// reset cpu's current execution priority
  }

  /* If the thread's quantum ran out, cycle it through the computable queue.  This will cause */
  /* threads of equal priority to share the cpu.  A recursion might happen if event flag      */
  /* routines (via a lowipl) sneak in during wait's wait loop and try to wake the thread.     */

  tc = oz_hw_smplock_wait (&smplock_tc);
  topcom = threadq_com;
  if (topcom == NULL) resched = 0;								// if nothing else to run, don't reschedule
  else if (topcom -> curprio < thread -> curprio) resched = 0;					// if only lower prio stuff, don't reschedule
  else if (topcom -> curprio > thread -> curprio) resched = 1;					// if something higher prio to run, force reschedule
  oz_hw_smplock_clr (&smplock_tc, tc);
  if (resched) {
    if (++ (thread -> handlingint) == 1) {							// skip this if recursive
      do {
        thread -> handlingint = 1;								// reset recursion counter
        if (thread -> state == OZ_THREAD_STATE_RUN) makecom (thread, 0);			// change its state to COM and put on COM queue
        oz_hw_smplock_clr (&(thread -> smplock_tp), OZ_SMPLOCK_SOFTINT);			// return to softint level
        oz_knl_thread_wait ();									// wait for it to return to RUN state
        tp = oz_hw_smplock_wait (&(thread -> smplock_tp));					// lock thread state again
        curcpu = oz_hw_cpu_getcur ();
        if (thread != curthreads[curcpu]) oz_crash ("oz_knl_thread_handleint: wait returned with wrong thread");
        if (thread -> state != OZ_THREAD_STATE_RUN) oz_crash ("oz_knl_thread_handleint: wait returned thread in state %d", thread -> state);
        if (tp != OZ_SMPLOCK_SOFTINT) oz_crash ("oz_knl_thread_handleint: wait returned at smplevel %u", tp);
      } while (-- (thread -> handlingint) > 0);							// repeat if recursion attempted during wait
      if (thread -> handlingint < 0) oz_crash ("oz_knl_thread_handleint: handlingint went negative (%d)", thread -> handlingint);
    }
  }

  /* If the thread is waiting for an event flag and has pending ast's,     */
  /* wake it up - this happens when the caller of oz_knl_event_wait has    */
  /* softint delivery inhibited, an ast gets queued via another cpu, then  */
  /* the oz_knl_event_wait routine locks smplock_tp and puts the thread in */
  /* the WEV state.                                                        */

  /* Note that it would be nice if we could check for ast's that are */
  /* actually deliverable instead of just checking for ast's, but    */
  /* that would be very difficult for the outer mode ast levels.     */

  else if (thread -> state == OZ_THREAD_STATE_WEV) {
    for (procmode = OZ_PROCMODE_MIN; procmode <= OZ_PROCMODE_MAX; procmode ++) {
      if (((thread -> astexpqh[procmode-OZ_PROCMODE_MIN] != NULL) && (thread -> astmode[procmode-OZ_PROCMODE_MIN] != OZ_ASTMODE_INHEXP)) 
       || ((thread -> astnorqh[procmode-OZ_PROCMODE_MIN] != NULL) && (thread -> astmode[procmode-OZ_PROCMODE_MIN] == OZ_ASTMODE_ENABLE))) {
        thread -> wakests = OZ_ASTDELIVERED;		// set the wake status to indicate it was woken because of ast delivery (not because the event flag was set)
        changestate (thread, OZ_THREAD_STATE_RUN); 	// mark it as running now, so oz_knl_thread_wait's idle loop will exit and the thread will continue
							// if the thread doesn't return out far enough to dequeue the ast's, well we tried, and it will eventually do them
        break;
      }
    }
  }

  oz_hw_smplock_clr (&(thread -> smplock_tp), OZ_SMPLOCK_SOFTINT);
}

/************************************************************************/
/*									*/
/*  Wake a thread that might be in WEV state				*/
/*									*/
/*    Input:								*/
/*									*/
/*	thread  = thread to wake					*/
/*	wakests = status to wake it with				*/
/*	smplevel <= tp							*/
/*									*/
/************************************************************************/

void oz_knl_thread_wakewev (OZ_Thread *thread, uLong wakests)

{
  uLong tp;

  OZ_KNL_CHKOBJTYPE (thread, OZ_OBJTYPE_THREAD);

  tp = oz_hw_smplock_wait (&(thread -> smplock_tp));			// lock target thread's state
  if (thread -> state == OZ_THREAD_STATE_WEV) {				// see if it is waiting for event flags
    thread -> wakests = wakests;					// save the wake status
    if (thread -> cpuidx >= 0) {					// if its context is in a cpu ...
      changestate (thread, OZ_THREAD_STATE_RUN);			// ... just mark it as running now
									// ... so oz_knl_thread_wait will skip right on through
      OZ_HW_WAITLOOP_WAKE (thread -> cpuidx);				// ... (wack it in case it's on the HLT instruction)
    }
    else makecom (thread, 1);						// not in a cpu, say it is waiting for a cpu now
									// ... and softint a cpu (maybe me) to run it
  }
  oz_hw_smplock_clr (&(thread -> smplock_tp), tp);			// release thread's state
}

/************************************************************************/
/*									*/
/*  Cause the current thread to wait until its state is restored to 	*/
/*  'RUN' (by oz_knl_thread_wake)					*/
/*									*/
/*    Input:								*/
/*									*/
/*	nevents, eventlist = if WEV, the event list descriptor		*/
/*	smplevel = softint delivery inhibited				*/
/*	thread in RUN,COM,WEV,ZOM state					*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_thread_wait = status passed to oz_knl_thread_wakewev	*/
/*	smplevel = softint delivery inhibited				*/
/*	thread in RUN state						*/
/*									*/
/*    Note:								*/
/*									*/
/*	Conceptually an thread that enters here in ZOM state will 	*/
/*	never return, it will be freed off via oz_knl_thread_increfc.  	*/
/*	However, it is possible that an lowipl routine could come in 	*/
/*	here and do an oz_knl_event_wait* call, which changes the 	*/
/*	state from ZOM to WEV, and when the lowipl routine gets woken, 	*/
/*	the state would get changed to RUN.  Then, when the lowipl 	*/
/*	routine returns back here to the idle loop, the idle loop sees 	*/
/*	its state is now RUN and will resume processing the thread.	*/
/*									*/
/************************************************************************/

uLong oz_knl_thread_wait (void)

{
  Long cpuidx;
  OZ_Process *process;
  OZ_Thread *nthread, *thread, *zthread;
  uLong sts, tc, tp;

  cpuidx  = oz_hw_cpu_getcur ();					// get the current thread pointer
  thread  = curthreads[cpuidx];
  process = oz_knl_process_getcur ();					// get what process it has mapped to memory
  zthread = NULL;							// assume there will be no thread to zombie test

  /* We assume that this 'while' loop isn't constantly generating bus cycles as the    */
  /* processor should have all the variables in cache.  We mark all the necessary      */
  /* variables as 'volatile' to the compiler so it won't try to put them in registers. */

  /* Note that we are testing variables without having the lock, but this is ok because:                    */
  /*   1) If we get a false positive, we re-check it under lock and loop back if not true                   */
  /*   2) If we get a false negative, the next time through the while loop will pick up the positive result */

  /* If the startup threads (in oz_knl_boot.c) terminate in an infinite loop of 'wait-for-interrupt' */
  /* instructions, there should always be something in the threadq_com available for a cpu to chew   */
  /* away on, and thus this loop will see that and immediately exit.                                 */

waitloop:
  thread -> inwaitloop ++;						// make sure we don't do any kernel ast delivery
									// - it's possible the OZ_HW_WAITLOOP_BODY will get an 
									//   softint to deliver something like the abort ast to 
									//   this thread.  Well, if the thread has softint delivery 
									//   inhibited, the ast should not be delivered.  So we 
									//   block ast delivery here until the thread enables 
									//   softints
									// - use a counter in case, whilst in OZ_HW_WAITLOOP_BODY, 
									//   we get a softint routine that waits and wakes, we 
									//   still want a non-zero inwaitloop variable when it 
									//   returns back out
									// - it doesn't have to be atomic as it is only modified by 
									//   the thread itself when it is the current thread
  OZ_HW_WAITLOOP_INIT;							// inhibit all interrupt delivery
  while ((thread -> state != OZ_THREAD_STATE_RUN) 			// maybe a hero will come along and put us right back in RUN
      && (((nthread = threadq_com) == NULL) 				// otherwise, keep looping if the COM queue is empty
       || ((nthread -> cpuidx >= 0) 					// (... or some other cpu already/still has the top entry, 
        && (nthread -> cpuidx != cpuidx)))) {				//  hope they dequeue it very soon)
    OZ_HW_WAITLOOP_BODY;						// enable all ints, wait for interrupt, inhib all ints
    if (oz_knl_lowipl_lowipls != NULL) {				// meanwhile, if there are any pending lowipl routines ...
      OZ_HW_WAITLOOP_TERM;						// ... enable hw interrupts, inhibit softint delivery
      oz_knl_lowipl_handleint ();					//     process them
      OZ_HW_WAITLOOP_INIT;						//     inhibit all interrupt delivery
    }
  }
  OZ_HW_WAITLOOP_TERM;							// enable hw interrupts, inhibit softint delivery
  cpuidx = oz_hw_cpu_getcur ();						// see what cpu we're on now
  if (-- (thread -> inwaitloop) < 0) oz_crash ("oz_knl_thread_wait: %p -> inwaitloop neg", thread); // allow kernel ast delivery after softint delivery enabled

  /* Now we see if there really is something to do with locking.  If not, we jump back to the wait loop. */
  /* We don't lock and unlock in the waitloop itself, as it might generate some sort of bus cycles.      */

  tp = oz_hw_smplock_wait (&(thread -> smplock_tp));			// lock existing thread state
  if (tp != OZ_SMPLOCK_SOFTINT) oz_crash ("oz_knl_thread_wait: waitloop exited at smplevel %u", tp);
  if (thread -> state != OZ_THREAD_STATE_RUN) {				// see if the existing thread is runnable
    tc = oz_hw_smplock_wait (&smplock_tc);				// if not, lock the COM queue
    validate_com_queue ();
    nthread = threadq_com;						// see if anything is waiting for a cpu
    if ((nthread == NULL) || ((nthread -> cpuidx >= 0) && (nthread -> cpuidx != cpuidx))) {
      oz_hw_smplock_clr (&smplock_tc, tc);				// if not, release both locks
      oz_hw_smplock_clr (&(thread -> smplock_tp), tp);
      goto waitloop;							// ... then wait some more
    }
    if (curthreads[cpuidx] != thread) oz_crash ("oz_knl_thread_wait: old thread %p not still current", thread);
    REMTHREADFROMQ (nthread);						// remove new thread from the COM queue
    validate_com_queue ();
    oz_hw_smplock_clr (&smplock_tc, tc);				// release the COM queue
									// keep lock on old thread because it's state is not stable
									// removing the new thread from COM queue is ok as no cpu can get it now
    if (nthread == thread) {						// see if it's the same thread we cam in with
      if (thread -> cpuidx != cpuidx) oz_crash ("oz_knl_thread_wait: same thread but cpuidx is %d", thread -> cpuidx); // ok, we should have its context
      changestate (thread, OZ_THREAD_STATE_RUN);			// ... just change the state from COM to RUN
    } else {

      /* Temporally speaking, it will either return right back here, but with a different stack, or it may */
      /* 'call' oz_knl_thread_start with the new thread.  Contextually speaking, it always simply returns, */
      /* unless our state is ZOM, and all the stack variables are the same as they were on entry.          */

      oz_hw_thread_switchctx (thread -> hw_ctx, nthread -> hw_ctx);	// switch stacks
									// now: thread = the new thread
									//     nthread = garbage
									//      cpuidx = garbage
									//   curthreads[current_cpu] = the old thread
									// we still have the old thread's smplock so no other cpu 
									//   can get to it -- we don't have its context anymore, 
									//   but we haven't set cpuidx=-1 yet
									// the new thread is still in COM state but removed from 
									//   the COM queue, so no other cpu will try to run it
      zthread = woken_part1 (thread);					// complete new thread transition to RUN state
    }
  }
  sts = thread -> wakests;						// get status it was woken with
  woken_part2 (thread, zthread, process);				// finish up various things
  thread -> nevents   = 0;						// we're not waiting for event flags anymore
  thread -> eventlist = NULL;
  return (sts);								// return the wake status
}

static OZ_Thread *woken_part1 (OZ_Thread *thread)

{
  int oldwazombie;
  Long cpuidx;
  OZ_Thread *othread, *zthread;

  cpuidx  = oz_hw_cpu_getcur ();					// get pointer to old thread
  othread = curthreads[cpuidx];
  othread -> cpuidx = -1;						// its context is no longer in any cpu
  zthread = NULL;							// assume we're not releasing an zombied thread
  if (othread -> state == OZ_THREAD_STATE_ZOM) zthread = othread;	// remember if it was an zombie we just unloaded
  oz_hw_smplock_clr (&(othread -> smplock_tp), OZ_SMPLOCK_SOFTINT);	// release the old thread
  oz_hw_smplock_wait (&(thread -> smplock_tp));				// lock the new thread's state
  if ((thread -> state != OZ_THREAD_STATE_COM) || (thread -> cpuidx >= 0)) { // it should be waiting for us to finish setting it up
    oz_crash ("oz_knl_thread_wait: thread %p -> state %d, cpuidx %d", thread, thread -> state, thread -> cpuidx);
  }
  changestate (thread, OZ_THREAD_STATE_RUN);				// mark it as RUNning now
  thread -> cpuidx   = cpuidx;						// ... on this cpu
  curthreads[cpuidx] = thread;

  return (zthread);
}

static void woken_part2 (OZ_Thread *thread, OZ_Thread *zthread, OZ_Process *process)

{
  start_quantum (thread);						// don't let it hog the cpu for too long
  oz_hw_smplock_clr (&(thread -> smplock_tp), OZ_SMPLOCK_SOFTINT);	// release thread state
  oz_knl_process_setcur (process);					// restore the process context
  if (zthread != NULL) oz_knl_thread_increfc (zthread, -1);		// maybe finish off a zombie thread
}

/************************************************************************/
/*									*/
/*  Set a thread's security keys (what it can access)			*/
/*									*/
/*    Input:								*/
/*									*/
/*	thread  = thread to set (or NULL for current)			*/
/*	seckeys = new security keys (or NULL to access all threads)	*/
/*									*/
/************************************************************************/

void oz_knl_thread_setseckeys (OZ_Thread *thread, OZ_Seckeys *seckeys)

{
  OZ_Seckeys *oldkeys;
  uLong se;

  OZ_KNL_CHKOBJTYPE (thread,  OZ_OBJTYPE_THREAD);		/* validate parameters */
  OZ_KNL_CHKOBJTYPE (seckeys, OZ_OBJTYPE_SECKEYS);
  if (thread == NULL) thread = oz_knl_thread_getcur ();		/* maybe use current thread */
  se = oz_hw_smplock_wait (&oz_s_smplock_se);
  oz_knl_seckeys_increfc (seckeys, 1);				/* inc new key's ref count */
  oldkeys = thread -> seckeys;					/* save pointer to old keys */
  thread -> seckeys = seckeys;					/* set up new keys */
  oz_knl_seckeys_increfc (oldkeys, -1);				/* no longer referencing old keys */
  oz_hw_smplock_clr (&oz_s_smplock_se, se);
}

/************************************************************************/
/*									*/
/*  Set a thread's default creation security attributes (who can 	*/
/*  access what it creates)						*/
/*									*/
/*    Input:								*/
/*									*/
/*	thread  = thread to set (or NULL for current)			*/
/*	secattr = new security attributes (or NULL for kernel access only)
/*									*/
/************************************************************************/

void oz_knl_thread_setdefcresecattr (OZ_Thread *thread, OZ_Secattr *secattr)

{
  OZ_Secattr *oldattr;
  uLong se;

  OZ_KNL_CHKOBJTYPE (thread,  OZ_OBJTYPE_THREAD);		/* validate parameters */
  OZ_KNL_CHKOBJTYPE (secattr, OZ_OBJTYPE_SECATTR);
  if (thread == NULL) thread = oz_knl_thread_getcur ();		/* maybe use current thread */
  se = oz_hw_smplock_wait (&oz_s_smplock_se);
  oz_knl_secattr_increfc (secattr, 1);				/* inc new attr's ref count */
  oldattr = thread -> defcresecattr;				/* save pointer to old attr */
  thread -> defcresecattr = secattr;				/* set up new attr */
  oz_knl_secattr_increfc (oldattr, -1);				/* no longer referencing old attr */
  oz_hw_smplock_clr (&oz_s_smplock_se, se);
}

/************************************************************************/
/*									*/
/*  Set a thread's security attributes (who can access it)		*/
/*									*/
/*    Input:								*/
/*									*/
/*	thread  = thread to set (or NULL for current)			*/
/*	secattr = new security attributes (or NULL for kernel access only)
/*									*/
/************************************************************************/

void oz_knl_thread_setsecattr (OZ_Thread *thread, OZ_Secattr *secattr)

{
  OZ_Secattr *oldattr;
  uLong se;

  OZ_KNL_CHKOBJTYPE (thread,  OZ_OBJTYPE_THREAD);		/* validate parameters */
  OZ_KNL_CHKOBJTYPE (secattr, OZ_OBJTYPE_SECATTR);
  if (thread == NULL) thread = oz_knl_thread_getcur ();		/* maybe use current thread */
  se = oz_hw_smplock_wait (&oz_s_smplock_se);
  oz_knl_secattr_increfc (secattr, 1);				/* inc new attr's ref count */
  oldattr = thread -> secattr;					/* save pointer to old attr */
  thread -> secattr = secattr;					/* set up new attr */
  oz_knl_secattr_increfc (oldattr, -1);				/* no longer referencing old attr */
  oz_hw_smplock_clr (&oz_s_smplock_se, se);
}

/************************************************************************/
/*									*/
/*  Returns pointer to threads security keys				*/
/*									*/
/*    Input:								*/
/*									*/
/*	thread = pointer to thread block or NULL for current		*/
/*									*/
/*	smplock <= ts							*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_thread_getseckeys = pointer to security keys block	*/
/*									*/
/*    Note:								*/
/*									*/
/*	Caller must decrement ref count when done			*/
/*									*/
/************************************************************************/

OZ_Seckeys *oz_knl_thread_getseckeys (OZ_Thread *thread)

{
  OZ_Seckeys *seckeys;
  uLong se;

  OZ_KNL_CHKOBJTYPE (thread, OZ_OBJTYPE_THREAD);
  if (thread == NULL) thread = oz_knl_thread_getcur ();
  seckeys = NULL;
  if (thread != NULL) {
    se = oz_hw_smplock_wait (&oz_s_smplock_se);
    seckeys = thread -> seckeys;
    oz_knl_seckeys_increfc (seckeys, 1);
    oz_hw_smplock_clr (&oz_s_smplock_se, se);
  }
  return (seckeys);
}

/************************************************************************/
/*									*/
/*  Returns pointer to thread's security attributes			*/
/*									*/
/*    Input:								*/
/*									*/
/*	thread = pointer to thread block or NULL for current		*/
/*									*/
/*	smplock <= ts							*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_thread_getsecattr = pointer to security attributes block	*/
/*									*/
/*    Note:								*/
/*									*/
/*	Caller must decrement ref count when done			*/
/*									*/
/************************************************************************/

OZ_Secattr *oz_knl_thread_getsecattr (OZ_Thread *thread)

{
  OZ_Secattr *secattr;
  uLong se;

  OZ_KNL_CHKOBJTYPE (thread, OZ_OBJTYPE_THREAD);
  if (thread == NULL) thread = oz_knl_thread_getcur ();
  secattr = NULL;
  if (thread != NULL) {
    se = oz_hw_smplock_wait (&oz_s_smplock_se);
    secattr = thread -> secattr;
    if (secattr != NULL) oz_knl_secattr_increfc (secattr, 1);
    oz_hw_smplock_clr (&oz_s_smplock_se, se);
  }
  return (secattr);
}

/************************************************************************/
/*									*/
/*  Returns pointer to thread's defult create security attributes	*/
/*									*/
/*    Input:								*/
/*									*/
/*	thread = pointer to thread block or NULL for current		*/
/*									*/
/*	smplock <= ts							*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_thread_getdefcresecattr = pointer to default create 	*/
/*	                                 security attributes block	*/
/*									*/
/*    Note:								*/
/*									*/
/*	Caller must decrement ref count when done			*/
/*									*/
/************************************************************************/

OZ_Secattr *oz_knl_thread_getdefcresecattr (OZ_Thread *thread)

{
  OZ_Secattr *secattr;
  uLong se;

  OZ_KNL_CHKOBJTYPE (thread, OZ_OBJTYPE_THREAD);
  if (thread == NULL) thread = oz_knl_thread_getcur ();
  secattr = NULL;
  if (thread != NULL) {
    se = oz_hw_smplock_wait (&oz_s_smplock_se);
    secattr = thread -> defcresecattr;
    oz_knl_secattr_increfc (secattr, 1);
    oz_hw_smplock_clr (&oz_s_smplock_se, se);
  }
  return (secattr);
}

/************************************************************************/
/*									*/
/*  Returns pointer to process that owns a thread			*/
/*									*/
/*    Input:								*/
/*									*/
/*	thread = pointer to thread block or NULL for current		*/
/*									*/
/*	smplock <= ts							*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_thread_getprocess = pointer to process block		*/
/*									*/
/************************************************************************/

OZ_Process *oz_knl_thread_getprocess (OZ_Thread *thread)

{
  OZ_KNL_CHKOBJTYPE (thread, OZ_OBJTYPE_THREAD);
  if (thread == NULL) thread = oz_knl_thread_getcur ();
  if (thread == NULL) return (NULL);
  return (thread -> process);
}

/* Get for the current thread - optimized because it is called a lot */
/* This routine is assumed to be called at or above softint level    */

OZ_Process *oz_knl_thread_getprocesscur (void)

{
  OZ_Thread *thread;

  thread = curthreads[oz_hw_cpu_getcur()];
  return (thread -> process);
}

/************************************************************************/
/*									*/
/*  Get pointer to hardware context block				*/
/*									*/
/************************************************************************/

void *oz_knl_thread_gethwctx (OZ_Thread *thread)

{
  OZ_KNL_CHKOBJTYPE (thread, OZ_OBJTYPE_THREAD);
  if (thread == NULL) thread = oz_knl_thread_getcur ();
  if (thread == NULL) return (NULL);
  return (thread -> hw_ctx);
}

/************************************************************************/
/*									*/
/*  Returns pointer to thread block currently executing on the current 	*/
/*  cpu									*/
/*									*/
/*    Input:								*/
/*									*/
/*	smplock = anything						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_thread_getcur = NULL : threads not initialized yet	*/
/*	                       else : pointer to thread block		*/
/*									*/
/************************************************************************/

OZ_Thread *oz_knl_thread_getcur (void)

{
  int si;
  OZ_Thread *thread;

  si = 0;					/* keep from switching cpu's */
  if (oz_hw_cpu_smplevel () == OZ_SMPLOCK_NULL) si = oz_hw_cpu_setsoftint (0);
  thread = curthreads[oz_hw_cpu_getcur()];	/* get current thread on the cpu */
  if (si) oz_hw_cpu_setsoftint (1);		/* let it switch cpu's now if it wants - our thread pointer will remain the same no matter what */
  return (thread);				/* return current thread pointer */
}

/************************************************************************/
/*									*/
/*  This makes a non-running thread computable				*/
/*									*/
/*  It interrupts a cpu that is either idle or is running a lower 	*/
/*  priority thread to make it start executing this one			*/
/*									*/
/*    Input:								*/
/*									*/
/*	thread = thread to make computable				*/
/*	         (assumed to already be removed from old queue)		*/
/*	softint = 0 : don't softint the current cpu			*/
/*	              (to keep it from infinite looping through 	*/
/*	               the oz_knl_thread_handleint routine)		*/
/*	          1 : softint even the current cpu			*/
/*	thread -> smplock_tp = set					*/
/*									*/
/*    Output:								*/
/*									*/
/*	thread inserted on computable queue				*/
/*	this or other cpu interrupted					*/
/*									*/
/************************************************************************/

static void makecom (OZ_Thread *thread, int softint)

{
  Long i, lowerpriocpu;
  OZ_Thread **lthread, *nthread;
  uLong j, tc;

  OZ_KNL_CHKOBJTYPE (thread, OZ_OBJTYPE_THREAD);

  /* If thread state was ZOM, barf */

  if (thread -> state == OZ_THREAD_STATE_ZOM) oz_crash ("oz_knl_thread makecom: trying to wake a zombie thread");

  /* Make state computable and insert by priorty in queue */

  changestate (thread, OZ_THREAD_STATE_COM);
  tc = oz_hw_smplock_wait (&smplock_tc);		/* lock the queue */
  validate_com_queue ();				/* make sure it isn't messed up */
  for (lthread = (OZ_Thread **)(&threadq_com); (nthread = *lthread) != NULL; lthread = &(nthread -> statenext)) {
    if (thread -> curprio > nthread -> curprio) break;	/* find insertion point based on current priority */
  }
  INSTHREADINTOQ (thread, lthread);			/* stick thread in the queue */

  /* If there are any cpu's in the wait loop, they will exit because the threadq_com has something in it now   */
  /* But in case there isn't, we softint the cpu running the lowest priority thread that's lower than this one */

  if (thread == threadq_com) {				/* see if the new one is top priority */
							/* - if not, don't bother doing anything, */
							/*   because if the top priority one doesn't have a cpu to run on */
							/*   this lower priority one won't have a cpu to run on either */
    lowerpriocpu = thread -> cpuidx;			/* if so, see if some cpu already has thread's context */
							/* - note that only that cpu can execute it at this point, */
							/*   because thread -> hw_ctx is not valid */
    if (lowerpriocpu < 0) {				/* see if on any cpu at all */
      i = cpu_round_robin;				/* if not, loop through all available cpu's */
      do {
        if (++ i == oz_s_cpucount) i = 0;		/* increment the cpu number (with wrap) */
        if (!(oz_s_cpusavail & (1 << i))) continue;	/* skip if the cpu is not online */
        j = curprios[i];				/* get prio of thread it is currently executing (or 0 if none) */
        if (j >= thread -> curprio) continue;		/* if it's executing at higher or same priority, skip over it */
        if ((lowerpriocpu < 0) 				/* if it's the lowest prio cpu so far, save it */
         || (curprios[lowerpriocpu] > j)) lowerpriocpu = i;
      } while (i != cpu_round_robin);
    }

    if (lowerpriocpu >= 0) {				/* if we found a suitable cpu, ... */
      if (softint || (lowerpriocpu != oz_hw_cpu_getcur ())) {
        oz_hw_cpu_reschedint (lowerpriocpu);		/* ... interrupt it so it will execute highest priority thread */
      }
      cpu_round_robin = lowerpriocpu;			/* save which cpu we used here so we use them evenly */
    }
  }

  validate_com_queue ();				/* make sure we didn't mess up the queue */
  oz_hw_smplock_clr (&smplock_tc, tc);			/* release the queue's lock */
}

/************************************************************************/
/*									*/
/*  Start thread's quantum timer					*/
/*									*/
/*    Input:								*/
/*									*/
/*	thread = pointer to thread to reset quantum on			*/
/*	(thread is assumed to be in RUN state on current cpu)		*/
/*	smplock level = tp						*/
/*									*/
/*   Note:								*/
/*									*/
/*	This causes the oz_knl_thread_quantimex routine to be called 	*/
/*	when the thread's quantum is up					*/
/*									*/
/*	The timer is not started if the basepri is zero, ie, this is 	*/
/*	one of the 'idle cpu' threads.  After all, why bother?		*/
/*									*/
/************************************************************************/

static void start_quantum (OZ_Thread *thread)

{
  OZ_Iotatime iotanow, quantum, timeinrun;

  if (thread -> basepri == 0) return;					/* no quantum timer on 'idle' threads */

  iotanow   = oz_hw_tod_iotanow ();					/* get current date/time */
  timeinrun = thread -> timeinstate[OZ_THREAD_STATE_RUN] + iotanow;	/* calculate thread's total time in RUN state */

  if (thread -> nextrunquant > timeinrun) {
    quantum = thread -> nextrunquant - timeinrun;			/* see how far in future the remaining quantum expires */
    oz_hw_quantimer_start (quantum, iotanow);				/* set timer to expire that amount from now */
  } else {
    oz_hw_cpu_reschedint (oz_hw_cpu_getcur ());				/* already ran out, reschedule it */
  }
}

/************************************************************************/
/*									*/
/*  Set the ast mode of the current thread				*/
/*									*/
/*    Input:								*/
/*									*/
/*	procmode = processor mode					*/
/*	astmode  = new ast mode (OZ_ASTMODE_INHEXP, _INHIBIT or _ENABLE)*/
/*	           OZ_ASTMODE_RDONLY just to read and not change it	*/
/*									*/
/*	smp lock = none							*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_thread_setast = previous mode				*/
/*									*/
/************************************************************************/

OZ_Astmode oz_knl_thread_setast (OZ_Procmode procmode, OZ_Astmode astmode)

{
  int deliverable;
  OZ_Astmode oldmode;
  OZ_Thread *thread;

  deliverable = 0;
  thread  = oz_knl_thread_getcur ();						/* get current thread pointer */
  oldmode = thread -> astmode[procmode];					/* save old ast mode */
  if ((astmode != OZ_ASTMODE_RDONLY) && (astmode != oldmode)) {
    thread -> astmode[procmode] = astmode;					/* set new ast mode */
    deliverable = ((oldmode == OZ_ASTMODE_INHEXP) && (thread -> astexpqh[procmode] != NULL)) 
               || ((astmode == OZ_ASTMODE_ENABLE) && (thread -> astnorqh[procmode] != NULL));
    oz_hw_thread_aststate (thread -> hw_ctx, procmode, deliverable, thread -> cpuidx);
  }

  /* If kernel ast mode went from inhibited to enabled, check for deliverable kernel mode ast's */

  if ((procmode == OZ_PROCMODE_KNL) && deliverable) checkknlastq (thread, NULL);

  /* Anyway, return old state */

  return (oldmode);
}

/************************************************************************/
/*									*/
/*  Set a thread's current priority					*/
/*									*/
/*    Input:								*/
/*									*/
/*	thread   = thread to set current priority of			*/
/*	newprio  = new current priority					*/
/*	softint <= smplevel <= tp					*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_thread_setcurprio = previous current priority		*/
/*									*/
/************************************************************************/

uLong oz_knl_thread_getbasepri (OZ_Thread *thread)

{
  OZ_KNL_CHKOBJTYPE (thread, OZ_OBJTYPE_THREAD);
  if (thread == NULL) thread = oz_knl_thread_getcur ();
  return (thread -> basepri);
}

uLong oz_knl_thread_setbasepri (OZ_Thread *thread, uLong newprio)

{
  uLong oldprio, tp;

  OZ_KNL_CHKOBJTYPE (thread, OZ_OBJTYPE_THREAD);
  tp = oz_hw_smplock_wait (&(thread -> smplock_tp));
  oldprio = thread -> basepri;
  thread -> basepri = newprio;
  oz_hw_smplock_clr (&(thread -> smplock_tp), tp);
  return (oldprio);
}

uLong oz_knl_thread_getcurprio (OZ_Thread *thread)

{
  uLong tp;

  if (thread == NULL) thread = curthreads[oz_hw_cpu_getcur()];				/* get current thread on the cpu */
  tp = oz_hw_smplock_wait (&(thread -> smplock_tp));					/* lock out changes by others */
  if (thread -> state == OZ_THREAD_STATE_WEV) changestate (thread, OZ_THREAD_STATE_WEV); /* maybe boost curprio if it's wev */
											/* (so we get a true picture of curprio) */
  oz_hw_smplock_clr (&(thread -> smplock_tp), tp);					/* allow changes by others now */
  return (thread -> curprio);								/* return what we got for it */
}

uLong oz_knl_thread_inccurprio (OZ_Thread *thread, Long incprio)

{
  uLong newprio, oldprio, tp;

  OZ_KNL_CHKOBJTYPE (thread, OZ_OBJTYPE_THREAD);

  tp = oz_hw_smplock_wait (&(thread -> smplock_tp));
  oldprio = thread -> curprio;			/* get old priority */
  newprio = oldprio + incprio;			/* compute new priority */
  if (incprio < 0) {
    if (oldprio <= -incprio) newprio = 1;	/* don't let it go below 1 */
  } else {
    if (newprio < oldprio) newprio = -1;	/* don't let it go above FFFFFFFF */
  }
  setcurprio (thread, newprio);			/* set it */
  oz_hw_smplock_clr (&(thread -> smplock_tp), tp); /* release lock */
  return (oldprio);				/* return previous priority */
}

uLong oz_knl_thread_setcurprio (OZ_Thread *thread, uLong newprio)

{
  uLong oldprio, tp;

  OZ_KNL_CHKOBJTYPE (thread, OZ_OBJTYPE_THREAD);

  tp = oz_hw_smplock_wait (&(thread -> smplock_tp));
  oldprio = thread -> curprio;
  setcurprio (thread, newprio);
  oz_hw_smplock_clr (&(thread -> smplock_tp), tp);
  return (oldprio);
}

static void setcurprio (OZ_Thread *thread, uLong newprio)

{
  uLong oldprio, tc;

  oldprio = thread -> curprio;
  thread -> curprio = newprio;
  switch (thread -> state) {

    /* Currently executing on a cpu somewhere.  If the priority was just */
    /* lowered and there are equal or higher priority threads waiting    */
    /* for a cpu, softint the thread's cpu to force it to reschedule.    */

    case OZ_THREAD_STATE_RUN: {
      tc = oz_hw_smplock_wait (&smplock_tc);
      if (thread -> cpuidx < 0) oz_crash ("oz_knl_thread_handleint: thread %p -> cpuidx %d", thread, thread -> cpuidx);
      curprios[thread->cpuidx] = thread -> curprio;
      validate_com_queue ();
      if ((newprio < oldprio) && (threadq_com != NULL) && (threadq_com -> curprio >= newprio)) {
        oz_hw_cpu_reschedint (thread -> cpuidx);
      }
      oz_hw_smplock_clr (&smplock_tc, tc);
      break;
    }

    /* Currently waiting for a cpu.  Remove from the 'waiting */
    /* for a cpu' queue and re-insert based on its new        */
    /* prioirty and softint a cpu if is now runnable.         */

    case OZ_THREAD_STATE_COM: {
      tc = oz_hw_smplock_wait (&smplock_tc);
      REMTHREADFROMQ (thread);
      oz_hw_smplock_clr (&smplock_tc, tc);
      makecom (thread, 1);
      break;
    }

    /* Other states (ZOM and WEV) we don't care about, just let the thread continue waiting */
  }
}

/************************************************************************/
/*									*/
/*  This routine is called when kernel mode ast delivery is enabled to 	*/
/*  check for deliverable kernel mode ast's.  It is also called when 	*/
/*  threads have been switched to check for deliverable kernel ast's.	*/
/*									*/
/*    Input:								*/
/*									*/
/*	thread = current thread						*/
/*	expressonly = 0 : check both normal and express kernel ast's	*/
/*	              1 : check only express ast's			*/
/*	smplevel = none							*/
/*									*/
/************************************************************************/

	/* This is called by the hardware layer when it is about to return with softint delivery enabled */
	/* It is called with hardware interrupt delivery inhibited, but softint delivery is enabled      */
	/* It does not enable hardware interrupt delivery unless there actually is an ast to deliver     */
	/* Keeping hwints inhibited will prevent an ast from queuing that we won't see (if an ast        */
        /* queues to this thread via another CPU, it will give us a reschedule interrupt)                */

void oz_knl_thread_checkknlastq (Long cpuidx, OZ_Mchargs *mchargs)

{
  OZ_Thread *thread;

  if (cpuidx >= 0) thread = curthreads[cpuidx];		// if caller knows what cpu it is on, get current thread the easy way
  else thread = oz_knl_thread_getcur ();		// if not, get current thread the hard way
  if (thread -> inwaitloop == 0) {			// anyway, make sure it's not in the wait loop
							// - the thread conceptually has softint delivery inhibited but the cpu 
							//   actually has them enabled so lowipl's and resched's will run in the 
							//   OZ_HW_WAITLOOP_BODY (which may possibly queue an ast to the thread)
    if (thread -> state != OZ_THREAD_STATE_RUN) {	// if we're not in waitloop and have softints enabled, thread must be in run state
      oz_crash ("oz_knl_thread_checkknlastq: thread %p not in wait loop but state %d", thread, thread -> state);
    }
    checkknlastq (thread, mchargs);
  }
}

static void checkknlastq (OZ_Thread *thread, OZ_Mchargs *mchargs)

{
  int hi, unlocked;
  OZ_Astentry astentry;
  OZ_Astmode astmode;
  uLong aststs;
  void *astparam;

  /* Scan for deliverable kernel ast's */

  astmode = thread -> astmode[OZ_PROCMODE_KNL-OZ_PROCMODE_MIN];
  do {
    unlocked = 0;

    /* Express ast delivery */

    if ((astmode != OZ_ASTMODE_INHEXP) && (thread -> astexpqh[OZ_PROCMODE_KNL-OZ_PROCMODE_MIN] != NULL)) {
      thread -> astmode[OZ_PROCMODE_KNL-OZ_PROCMODE_MIN] = OZ_ASTMODE_INHEXP;		/* inhibit nested ast delivery during delivery */
      hi = oz_hw_cpu_sethwints (1);							/* make sure hw int delivery enabled */
      unlocked |= (hi <= 0);								/* set flag iff it was inhibited */
      while (oz_knl_thread_deqast (OZ_PROCMODE_KNL, 1, &astentry, &astparam, &aststs) == OZ_FLAGWASSET) { /* dequeue an express ast */
        (*astentry) (astparam, aststs, mchargs);           				/* if there, call it */
      }											/* repeat to get them all */
      oz_hw_cpu_sethwints (hi);								/* restore hw int delivery mode */
      thread -> astmode[OZ_PROCMODE_KNL-OZ_PROCMODE_MIN] = astmode;			/* re-enable ast delivery */
    }

    /* Normal ast delivery */

    if ((astmode == OZ_ASTMODE_ENABLE) && (thread -> astnorqh[OZ_PROCMODE_KNL-OZ_PROCMODE_MIN] != NULL)) {
      thread -> astmode[OZ_PROCMODE_KNL-OZ_PROCMODE_MIN] = OZ_ASTMODE_INHIBIT;		/* inhibit nested ast delivery during delivery */
      hi = oz_hw_cpu_sethwints (1);							/* make sure hw int delivery enabled */
      unlocked |= (hi <= 0);								/* set flag iff it was inhibited */
      while (oz_knl_thread_deqast (OZ_PROCMODE_KNL, 0, &astentry, &astparam, &aststs) == OZ_FLAGWASSET) { /* dequeue an normal ast */
        (*astentry) (astparam, aststs, mchargs);					/* if there, call it */
        if (oz_hw_cpu_smplevel () != 0) oz_crash ("oz_knl_thread checkknlastq: ast %p left smplevel %u", astentry, oz_hw_cpu_smplevel ());
      }											/* repeat to get them all */
      oz_hw_cpu_sethwints (hi);								/* restore hw int delivery mode */
      thread -> astmode[OZ_PROCMODE_KNL-OZ_PROCMODE_MIN] = OZ_ASTMODE_ENABLE;		/* re-enable ast delivery */
    }

    /* Repeat if hwints were inhibited by caller but we released them to make sure we get everything */

  } while (unlocked);
}

/************************************************************************/
/*									*/
/*  Return whether or not there are any ast's waiting for the given 	*/
/*  processor mode in the current thread				*/
/*									*/
/*    Input:								*/
/*									*/
/*	procmode = OZ_PROCMODE_USR or OZ_PROCMODE_KNL			*/
/*	software interrupt delivery inhibited				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_thread_chkpendast = 0 : no ast's pending for procmode	*/
/*	                           1 : there are pending ast's		*/
/*									*/
/*    Note:								*/
/*									*/
/*	This routine is typically called by the hardware layer's 	*/
/*	softint and syscall handlers when they are about to return to 	*/
/*	an outer mode.  They can call this routine when still in 	*/
/*	kernel mode to find out whether or not they should call 	*/
/*	oz_sys_thread_checkast after returning to the outer mode.	*/
/*									*/
/*	Because this routine is not interlocked in any way, two things 	*/
/*	can happen:							*/
/*									*/
/*	  false positive - in which case oz_sys_thread_checkast will 	*/
/*	    not find any ast to dequeue					*/
/*	  false negative - in which case there should be a pending 	*/
/*	    hardware softint which will cause the softint handler to 	*/
/*	    be called which will process the ast			*/
/*									*/
/************************************************************************/

int oz_knl_thread_chkpendast (OZ_Procmode procmode)

{
  int rc;
  OZ_Thread *thread;

  thread = curthreads[oz_hw_cpu_getcur()];	/* get current thread pointer */
  rc = ((thread -> astexpqh[procmode-OZ_PROCMODE_MIN] != NULL) && (thread -> astmode[procmode-OZ_PROCMODE_MIN] != OZ_ASTMODE_INHEXP)) /* see if anything is pending */
    || ((thread -> astnorqh[procmode-OZ_PROCMODE_MIN] != NULL) && (thread -> astmode[procmode-OZ_PROCMODE_MIN] == OZ_ASTMODE_ENABLE));
  return (rc);					/* return pending flag */
}

/************************************************************************/
/*									*/
/*  Dequeue an ast block from the current thread of the given mode	*/
/*									*/
/*    Input:								*/
/*									*/
/*	procmode = processor mode to check for ast's			*/
/*	express  = express ast flag					*/
/*									*/
/*	smp lock <= tp							*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_thread_deqast = OZ_FLAGWASCLR : no ast was in queue	*/
/*	                       OZ_FLAGWASSET : ast was dequeued		*/
/*	                                       *myast = copy of ast block
/*	                                       thread refcount decremented
/*									*/
/************************************************************************/

uLong oz_knl_thread_deqast (OZ_Procmode procmode, int express, OZ_Astentry *astentry, void **astparam, uLong *aststs)

{
  OZ_Ast *ast, **astnp, **astqh;
  OZ_Thread *thread;
  uLong tp;

  thread = oz_knl_thread_getcur ();				/* get current thread pointer */
  astqh  = (express ? thread -> astexpqh : thread -> astnorqh) + procmode - OZ_PROCMODE_MIN; /* point to queue header */
  tp     = oz_hw_smplock_wait (&(thread -> smplock_tp));	/* set thread state smplock */
  ast    = *astqh;						/* see if any ast's in queue */
  if (ast != NULL) {
    oz_knl_ast_remove (ast);					/* got one, unlink it */
    if (*astqh == NULL) (express ? thread -> astexpqt : thread -> astnorqt)[procmode-OZ_PROCMODE_MIN] = astqh;
    oz_knl_ast_delete (ast, astentry, astparam, aststs); 	/* free ast block and get parameters */
  }
  oz_hw_smplock_clr (&(thread -> smplock_tp), tp);		/* release smplock */

  oz_hw_thread_aststate (thread -> hw_ctx, 			/* update what hardware thinks about ast state now */
                         procmode, 
                         ((thread -> astmode[procmode-OZ_PROCMODE_MIN] != OZ_ASTMODE_INHEXP) && (thread -> astexpqh[procmode-OZ_PROCMODE_MIN] != NULL)) 
                      || ((thread -> astmode[procmode-OZ_PROCMODE_MIN] == OZ_ASTMODE_ENABLE) && (thread -> astnorqh[procmode-OZ_PROCMODE_MIN] != NULL)), 
                         thread -> cpuidx);

  return ((ast == NULL) ? OZ_FLAGWASCLR : OZ_FLAGWASSET);	/* return (not)empty status */
}

/************************************************************************/
/*									*/
/*  Set the current thread's state					*/
/*									*/
/*    Input:								*/
/*									*/
/*	state = new state to set (WEV or RUN)				*/
/*	nevents,eventlist = put these in thread under lock		*/
/*	softint <= smplevel <= tp					*/
/*									*/
/************************************************************************/

OZ_Thread *oz_knl_thread_setcurstate (OZ_Thread_state state, uLong nevents, OZ_Eventlist *eventlist)

{
  OZ_Thread *thread;
  uLong tp;

  if ((state != OZ_THREAD_STATE_RUN) && (state != OZ_THREAD_STATE_WEV)) {
    oz_crash ("oz_knl_thread_setcurstate: new state %d unsupported", thread -> state);
  }

  thread = curthreads[oz_hw_cpu_getcur()];		// see what thread is current on this cpu
  tp = oz_hw_smplock_wait (&(thread -> smplock_tp));	// lock its state
  switch (thread -> state) {
    case OZ_THREAD_STATE_COM: {				// if in COM state, just ignore request
      break;						// ... wait till it loops through to complete what it's doing
    }
    case OZ_THREAD_STATE_RUN:
    case OZ_THREAD_STATE_WEV:
    case OZ_THREAD_STATE_ZOM: {
      thread -> nevents   = nevents;			// save these for info display purposes
      thread -> eventlist = eventlist;
      changestate (thread, state);			// change the thread's state
      break;
    }
    default: oz_crash ("oz_knl_thread_setcurstate: old state %d unsupported", thread -> state);
  }
  oz_hw_smplock_clr (&(thread -> smplock_tp), tp);	// unlock it
  return (thread);					// return current thread (for convenience)
}

/************************************************************************/
/*									*/
/*  Change thread's state						*/
/*									*/
/*  This routine updates the thread's state variable and updates the 	*/
/*  time in state element.  It does not do anything with the queues.	*/
/*  It does boost the curprio back up if it was in wev state long 	*/
/*  enough.								*/
/*									*/
/*  smplevel = tp							*/
/*									*/
/************************************************************************/

static void changestate (OZ_Thread *thread, OZ_Thread_state newstate)

{
  Long cpuidx;
  OZ_Iotatime iotanow;
  OZ_Thread_state oldstate;
  uLong newprio;

  oldstate = thread -> state;
  if ((newstate != oldstate) || (newstate == OZ_THREAD_STATE_WEV)) {

    /* New state different than old state, store new state                                                */
    /* We also go thru this for WEV-to-WEV (it is getcurprio trying to update the current priority field) */

    thread -> state = newstate;

    /* Stop counting time in the old state */

    iotanow = oz_hw_tod_iotanow ();
    thread -> timeinstate[oldstate] += iotanow;

    /* If the old state was WEV, and we have accumulated a quantum of time in WEV state, boost priority back up */

    if (oldstate == OZ_THREAD_STATE_WEV) {
      while (thread -> timeinstate[OZ_THREAD_STATE_WEV] >= thread -> nextwevquant) {
        if (thread -> curprio < thread -> basepri) {	/* maybe boost its priority back up some  */
          newprio  = thread -> basepri - thread -> curprio;
          newprio /= 3;					/*  (do it the hard way to avoid arith overflow) */
          thread -> curprio += ++ newprio;
        }
        thread -> nextwevquant += deltawevquant;
      }
    }

    /* Anyway, start counting time in new state */

    thread -> timeinstate[newstate] -= iotanow;

    /* Also set curprios to new priority if the thread is loaded in a cpu.  If new state is  */
    /* RUN, set it to actual prio.  Else use 0 so it can be interrupted by any other thread. */

    if ((cpuidx = thread -> cpuidx) >= 0) {
      if (newstate != OZ_THREAD_STATE_RUN) curprios[cpuidx] = 0;
      else curprios[cpuidx] = thread -> curprio;
    }
  }
}

/************************************************************************/
/*									*/
/*  Validate the process threadq					*/
/*									*/
/************************************************************************/

static void validate_process_threadq (OZ_Thread **threadqp, OZ_Process *process, int verbose)

{
  OZ_Thread *thread;

  if (verbose) {
    oz_knl_printk ("oz_knl_thread validate_process_threadq (%p, %p, %d)\n", threadqp, process, verbose);
  }

  if (!OZ_HW_READABLE (sizeof *threadqp, threadqp, OZ_PROCMODE_KNL)) {
    oz_crash ("oz_knl_thread validate_process_threadq: initial threadqp %p not readable", threadqp);
  }

  while ((thread = *threadqp) != NULL) {
    if (verbose) {
      oz_knl_printk ("oz_knl_thread validate_process_threadq:  threadqp %p points to thread %p\n", threadqp, thread);
    }
    if (!OZ_HW_READABLE (sizeof *thread, thread, OZ_PROCMODE_KNL)) {
      oz_crash ("oz_knl_thread validate_process_threadq: thread %p not readable", thread);
    }
    if (verbose) {
      oz_knl_printk ("oz_knl_thread validate_process_threadq:  thread -> process %p, proc_prev %p, proc_next %p\n", 
		thread -> process, thread -> proc_prev, thread -> proc_next);
    }
    if (thread -> process != process) {
      oz_crash ("oz_knl_thread validate_process_threadq: thread %p, thread -> process %p, process %p", thread, thread -> process, process);
    }
    if (thread -> proc_prev != threadqp) {
      oz_crash ("oz_knl_thread validate_process_threadq: thread %p, thread -> proc_prev %p, threadqp %p", thread, thread -> proc_prev, threadqp);
    }
    threadqp = &(thread -> proc_next);
  }
}

/************************************************************************/
/*									*/
/*  Get address of ioop queue listhead					*/
/*									*/
/************************************************************************/

OZ_Ioop **oz_knl_thread_getioopqp (OZ_Thread *thread)

{
  OZ_KNL_CHKOBJTYPE (thread, OZ_OBJTYPE_THREAD);
  return (&(thread -> ioopq));
}

/************************************************************************/
/*									*/
/*  Get thread state							*/
/*									*/
/************************************************************************/

OZ_Thread_state oz_knl_thread_getstate (OZ_Thread *thread)

{
  OZ_KNL_CHKOBJTYPE (thread, OZ_OBJTYPE_THREAD);
  return (thread -> state);
}

/************************************************************************/
/*									*/
/*  Get thread's time in a particular state				*/
/*									*/
/************************************************************************/

OZ_Datebin oz_knl_thread_gettis (OZ_Thread *thread, OZ_Thread_state state)

{
  OZ_Datebin tis;
  OZ_Iotatime iotatime;
  OZ_Thread_state curstate;
  uLong tp;

  OZ_KNL_CHKOBJTYPE (thread, OZ_OBJTYPE_THREAD);	/* make sure they passed a thread */
  tp = oz_hw_smplock_wait (&(thread -> smplock_tp));	/* lock out any changes in state */
  iotatime = thread -> timeinstate[state];		/* get time in requested state */
  curstate = thread -> state;				/* get thread's current state */
  oz_hw_smplock_clr (&(thread -> smplock_tp), tp);	/* release threadstate smp lock */
  if (curstate == state) {				/* see if they're asking for time in current state */
    iotatime += oz_hw_tod_iotanow ();			/* if so, add current time to time in state */
  }
  tis = oz_hw_tod_diota2sys (iotatime);			/* convert iota time in state to system time in state */
  return (tis);						/* return system time in state */
}

/************************************************************************/
/*									*/
/*  See whether an abort ast has been queued to a thread		*/
/*									*/
/************************************************************************/

int oz_knl_thread_abortpend (OZ_Thread *thread)

{
  OZ_KNL_CHKOBJTYPE (thread, OZ_OBJTYPE_THREAD);	/* make sure they passed a thread */
  if (thread == NULL) thread = oz_knl_thread_getcur ();	/* use current thread if they gave NULL */
  return (thread -> abortpend);				/* return flag */
}

/************************************************************************/
/*									*/
/*  Get next thread in a process					*/
/*									*/
/*    Input:								*/
/*									*/
/*	lastthread = last thread that we got info for			*/
/*	process = process to get threads of				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_thread_getnext = new thread (or NULL if end of list)	*/
/*									*/
/************************************************************************/

OZ_Thread *oz_knl_thread_getnext (OZ_Thread *lastthread, OZ_Process *process)

{
  OZ_Thread *thread;
  uLong ps;

  thread = lastthread;
  if (process == NULL) process = thread -> process;
  ps = oz_knl_process_lockps (process);
  if (thread == NULL) thread = *oz_knl_process_getthreadqp (process, 0);
  else thread = thread -> proc_next;
  if (thread != NULL) oz_knl_thread_increfc (thread, 1);
  oz_knl_process_unlkps (process, ps);
  return (thread);
}

/************************************************************************/
/*									*/
/*  Count the number of threads in a process				*/
/*									*/
/*    Input:								*/
/*									*/
/*	threadq = pointer to process -> threadq				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_thread_count = number of threads on that queue		*/
/*									*/
/************************************************************************/

uLong oz_knl_thread_count (OZ_Process *process)

{
  OZ_Thread *thread;
  uLong count, ps;

  count = 0;
  ps = oz_knl_process_lockps (process);
  for (thread = *oz_knl_process_getthreadqp (process, 0); thread != NULL; thread = thread -> proc_next) count ++;
  oz_knl_process_unlkps (process, ps);
  return (count);
}

/************************************************************************/
/*									*/
/*  Find a thread by its id number					*/
/*									*/
/*    Input:								*/
/*									*/
/*	threadid = id of thread to find					*/
/*	           (0 for current thread)				*/
/*	smp level <= tf							*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_thread_getbyid = NULL : no such thread			*/
/*	                        else : pointer to thread with 		*/
/*	                               ref count incremented		*/
/*									*/
/************************************************************************/

OZ_Thread *oz_knl_thread_getbyid (OZ_Threadid threadid)

{
  OZ_Thread *thread;
  uLong tf;

  if (threadid == 0) {						// id 0 is the current thread
    thread = oz_knl_thread_getcur ();
    oz_knl_thread_increfc (thread, 1);
  } else {
    tf = oz_hw_smplock_wait (&smplock_tf);			// lock thread id numbers
    thread = oz_knl_idno_find (threadid, OZ_OBJTYPE_THREAD);	// look it up in id number list
    if (thread != NULL) oz_knl_thread_increfc (thread, 1);	// if found, inc its ref count so it doesn't go away on us
    oz_hw_smplock_clr (&smplock_tf, tf);			// release thread id numbers
  }
  return (thread);						// return pointer (or NULL if not found)
}

/************************************************************************/
/*									*/
/*  Get allocated device list head pointer				*/
/*									*/
/************************************************************************/

OZ_Devunit **oz_knl_thread_getdevalloc (OZ_Thread *thread)

{
  OZ_KNL_CHKOBJTYPE (thread, OZ_OBJTYPE_THREAD);
  return (&(thread -> devalloc));
}

/************************************************************************/
/*									*/
/*  Dump threads							*/
/*									*/
/************************************************************************/

void oz_knl_thread_dump_all (void)

{
  Long cpuidx;
  OZ_Thread *thread;
  uLong tc, tf;

  tf = oz_hw_smplock_wait (&smplock_tf);
  tc = oz_hw_smplock_wait (&smplock_tc);
  oz_knl_printk ("oz_knl_thread_dump_all: all threads:\n");
  for (thread = orphans; thread != NULL; thread = nextsibling (thread, NULL)) oz_knl_thread_dump (thread);
  oz_knl_printk ("oz_knl_thread_dump_all: COM threads:\n");
  for (thread = threadq_com; thread != NULL; thread = thread -> statenext) oz_knl_thread_dump (thread);
  oz_knl_printk ("oz_knl_thread_dump_all: RUN threads:\n");
  for (cpuidx = 0; cpuidx < oz_s_cpucount; cpuidx ++) {
    if (oz_s_cpusavail & (1 << cpuidx)) {
      oz_knl_printk ("[%d] %u:", cpuidx, curprios[cpuidx]);
      oz_knl_thread_dump (curthreads[cpuidx]);
    }
  }
  oz_hw_smplock_clr (&smplock_tc, tc);
  oz_hw_smplock_clr (&smplock_tf, tf);
}

void oz_knl_thread_dump_process (OZ_Process *process)

{
  OZ_Thread *thread;
  uLong ps;

  ps = oz_knl_process_lockps (process);
  for (thread = *oz_knl_process_getthreadqp (process, 0); thread != NULL; thread = thread -> proc_next) oz_knl_thread_dump (thread);
  oz_knl_process_unlkps (process, ps);
}

void oz_knl_thread_dump (OZ_Thread *thread)

{
  char statebuf[4];

  /* Fill in statebuf with thread state */

  switch (thread -> state) {
    case OZ_THREAD_STATE_WEV: { strcpy (statebuf, "WEV"); break; }
    case OZ_THREAD_STATE_COM: { strcpy (statebuf, "COM"); break; }
    case OZ_THREAD_STATE_ZOM: { strcpy (statebuf, "ZOM"); break; }
    case OZ_THREAD_STATE_RUN: { strcpy (statebuf, "RUN"); break; }
    default: { oz_sys_sprintf (sizeof statebuf, statebuf, "?%2d", thread -> state); break; }
  }

  /* Output the line */

  oz_knl_printk ("  %p %6u %s %2d %8u %s\n", 
	thread, thread -> threadid, statebuf, thread -> cpuidx, thread -> curprio, thread -> name);
}

void oz_knl_thread_tracedump (OZ_Thread *thread)

{
  uLong tp;

  tp = oz_hw_smplock_wait (&(thread -> smplock_tp));
  oz_knl_thread_dump (thread);
  if (thread -> cpuidx >= 0) oz_knl_printk ("oz_knl_thread_tracedump: loaded in cpu %d, can't trace\n", thread -> cpuidx);
  else oz_hw_thread_tracedump (thread -> hw_ctx);
  oz_hw_smplock_clr (&(thread -> smplock_tp), tp);
}

/************************************************************************/
/*									*/
/*  Validate the thread queues						*/
/*									*/
/*    Input:								*/
/*									*/
/*	all thread state info						*/
/*	smp level = ts							*/
/*									*/
/************************************************************************/

void oz_knl_thread_validate (void)

{
#if 000 // OZ_DEBUG
  Long i;
  uLong numfound;
  OZ_Thread *thread;
  volatile Threadp *lthread;

  static uLong runseq = 0;

  /* Make sure we can find exactly 'numthreads' threads in the state queues */

  numfound = 0;

  while (++ runseq == 0) {}

  for (lthread = &threadq_com; (thread = *lthread) != NULL; lthread = &(thread -> statenext)) {
    if (thread -> state != OZ_THREAD_STATE_COM) oz_crash ("oz_knl_thread_validate: thread in threadq_com state is %d", thread -> state);
    if (thread -> validseq == runseq) oz_crash ("oz_knl_thread_validate: thread found twice");
    if (thread -> stateprev != lthread) oz_crash ("oz_knl_thread_validate: thread in threadq_com has bad stateprev pointer");
    thread -> validseq = runseq;
    valthread (thread);
    numfound ++;
  }

  for (lthread = &threadq_wev; (thread = *lthread) != NULL; lthread = &(thread -> statenext)) {
    if (thread -> state != OZ_THREAD_STATE_WEV) oz_crash ("oz_knl_thread_validate: thread in threadq_wev state is %d", thread -> state);
    if (thread -> validseq == runseq) oz_crash ("oz_knl_thread_validate: thread found twice");
    if (thread -> stateprev != lthread) oz_crash ("oz_knl_thread_validate: thread in threadq_wev has bad stateprev pointer");
    thread -> validseq = runseq;
    valthread (thread);
    numfound ++;
  }

  for (lthread = &threadq_zom; (thread = *lthread) != NULL; lthread = &(thread -> statenext)) {
    if (thread -> state != OZ_THREAD_STATE_ZOM) oz_crash ("oz_knl_thread_validate: thread in threadq_zom state is %d", thread -> state);
    if (thread -> validseq == runseq) oz_crash ("oz_knl_thread_validate: thread found twice");
    if (thread -> stateprev != lthread) oz_crash ("oz_knl_thread_validate: thread in threadq_zom has bad stateprev pointer");
    thread -> validseq = runseq;
    valthread (thread);
    numfound ++;
  }

  for (i = 0; i < oz_s_cpucount; i ++) {
    thread = curthreads[i];
    if (thread != NULL) {
      if (thread -> cpuidx != i) oz_crash ("oz_knl_thread_validate: thread cpuidx %d not eq curthread array index %d", thread -> cpuidx, i);
      if (thread -> state == OZ_THREAD_STATE_RUN) {
        if (thread -> validseq == runseq) oz_crash ("oz_knl_thread_validate: thread found twice");
        thread -> validseq = runseq;
        numfound ++;
      } else {
        if (thread -> validseq != runseq) oz_crash ("oz_knl_thread_validate: curthread in state %d is not in its queue", thread -> state);
      }
      valthread (thread);
    }
  }

  if (numfound != numthreads) oz_crash ("oz_knl_thread_validate: there are %d threads but %d were found in state queues", numthreads, numfound);

  /* Should be able to find them all by scanning orphans list */

  numfound = 0;
  for (thread = orphans; thread != NULL; thread = nextsibling (thread, NULL)) numfound ++;
  if (numfound != numthreads) oz_crash ("oz_knl_thread_validate: there are %d threads but %d were found in sibling queues", numthreads, numfound);
#endif
}

/* Check out all we can about an individual thread */

static void valthread (OZ_Thread *thread)

{
  int astex, j;
  OZ_Ast *ast;
  OZ_Procmode astpm;
  OZ_Thread *astth;

  for (j = 0; j <= OZ_PROCMODE_MAX - OZ_PROCMODE_MIN; j ++) {
    oz_knl_ast_validateq (thread -> astexpqh + j, thread -> astexpqt[j]);
    for (ast = thread -> astexpqh[j]; ast != NULL; ast = oz_knl_ast_getnext (ast)) {
      astth = oz_knl_ast_getthread (ast);
      astpm = oz_knl_ast_getprocmode (ast);
      astex = oz_knl_ast_getexpress (ast);
      if (astth != thread) oz_crash ("oz_knl_thread_validate: ast thread %p, thread %p", astth, thread);
      if (astpm != j + OZ_PROCMODE_MIN) oz_crash ("oz_knl_thread_validate: ast pm %d, queue pm %d", astpm, j + OZ_PROCMODE_MIN);
      if (astex == 0) oz_crash ("oz_knl_thread_validate: normal ast on express queue");
    }

    oz_knl_ast_validateq (thread -> astnorqh + j, thread -> astnorqt[j]);
    for (ast = thread -> astnorqh[j]; ast != NULL; ast = oz_knl_ast_getnext (ast)) {
      astth = oz_knl_ast_getthread (ast);
      astpm = oz_knl_ast_getprocmode (ast);
      astex = oz_knl_ast_getexpress (ast);
      if (astth != thread) oz_crash ("oz_knl_thread_validate: ast thread %p, thread %p", astth, thread);
      if (astpm != j + OZ_PROCMODE_MIN) oz_crash ("oz_knl_thread_validate: ast pm %d, queue pm %d", astpm, j + OZ_PROCMODE_MIN);
      if (astex != 0) oz_crash ("oz_knl_thread_validate: express ast on normal queue");
    }
  }
}

/************************************************************************/
/*									*/
/*  Validate COM queue just after smplock_tc set or just before 	*/
/*  releasing it							*/
/*									*/
/************************************************************************/

static void validate_com_queue ()

{
#if OZ_DEBUG
  OZ_Thread **lthread, *thread;

  for (lthread = (OZ_Thread **)&threadq_com; (thread = *lthread) != NULL; lthread = &(thread -> statenext)) {
    if (lthread != thread -> stateprev) oz_crash ("oz_knl_thread validate_com_queue: link broken");
    if (thread -> state != OZ_THREAD_STATE_COM) oz_crash ("oz_knl_thread validate_com_queue: state %d", thread -> state);
  }
#endif
}
