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
/*  User (and kernel) callable thread routines				*/
/*									*/
/************************************************************************/

#include "ozone.h"

#include "oz_dev_timer.h"
#include "oz_knl_ast.h"
#include "oz_knl_handle.h"
#include "oz_knl_hw.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_sdata.h"
#include "oz_knl_section.h"
#include "oz_knl_status.h"
#include "oz_knl_thread.h"
#include "oz_knl_userjob.h"
#include "oz_sys_condhand.h"
#include "oz_sys_dateconv.h"
#include "oz_sys_syscall.h"
#include "oz_sys_thread.h"

static void thread_abort_usr (void *dummy, uLong status, OZ_Mchargs *mchargs);
static void thread_abort_tmo (void *tav, OZ_Timer *timer);

/************************************************************************/
/*									*/
/*  Create a thread							*/
/*									*/
/*    Input:								*/
/*									*/
/*	process    = pointer to process that thread belongs to		*/
/*	priority   = thread's execution priority			*/
/*	initevent  = wait for this flag to set on initialize		*/
/*	             (or NULL if not to wait)				*/
/*	exitevent  = event flag to set on exit				*/
/*	             (or NULL if none wanted)				*/
/*	stacksize  = number of pages to allocate for its user stack	*/
/*	             (or 0 for kernel mode only)			*/
/*	thentry    = entrypoint for thread routine			*/
/*	thparam    = parameter to pass to thread routine		*/
/*	knlastmode = initial kernel ast mode 				*/
/*	             (OZ_ASTMODE_INHIBIT or OZ_ASTMODE_ENABLE)		*/
/*	name       = thread name(secattr) string pointer		*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_thread_create = OZ_SUCCESS : successfully created	*/
/*	                             else : error status		*/
/*	*h_thread_r = thread handle					*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_11 (thread_create, OZ_Procmode, procmode, OZ_Handle, h_process, uLong, priority, OZ_Handle, h_initevent, OZ_Handle, h_exitevent, OZ_Mempage, stacksize, OZ_Thread_entry, thentry, void *, thparam, OZ_Astmode,  knlastmode, const char *, name, OZ_Handle *, h_thread_r)

{
  int name_l, si;
  uLong sts;
  OZ_Event *exitevent, *initevent;
  OZ_Process *process;
  OZ_Secattr *secattr;
  OZ_Seckeys *seckeys;
  OZ_Thread *thread;

  if (procmode < cprocmode) procmode = cprocmode;
  if ((h_thread_r != NULL) && !OZ_HW_WRITABLE (sizeof *h_thread_r, h_thread_r, procmode)) return (OZ_ACCVIO);

  exitevent = NULL;
  initevent = NULL;
  process   = NULL;
  secattr   = NULL;
  seckeys   = NULL;
  thread    = NULL;

  /* If stack size zero, set it to default */

  if (stacksize == 0) stacksize = oz_s_loadparams.def_user_stack_size >> OZ_HW_L2PAGESIZE;

  /* If priority is zero, use current thread's priority */

  if (priority == 0) priority = oz_knl_thread_getbasepri (oz_knl_thread_getcur ());

  si = oz_hw_cpu_setsoftint (0);

  /* Get process pointer that thread goes into */

  if (h_process != 0) {
    sts = oz_knl_handle_takeout (h_process, procmode, OZ_SECACCMSK_WRITE, OZ_OBJTYPE_PROCESS, &process, NULL);
    if (sts != OZ_SUCCESS) goto rtnsts;
  } else {
    process = oz_knl_thread_getprocesscur ();
  }

  /* Get initialization event flag */

  if (h_initevent != 0) {
    sts = oz_knl_handle_takeout (h_initevent, procmode, OZ_SECACCMSK_READ, OZ_OBJTYPE_EVENT, &initevent, NULL);
    if (sts != OZ_SUCCESS) goto rtnsts;
  }

  /* Get exit event flag */

  if (h_exitevent != 0) {
    sts = oz_knl_handle_takeout (h_exitevent, procmode, OZ_SECACCMSK_WRITE, OZ_OBJTYPE_EVENT, &exitevent, NULL);
    if (sts != OZ_SUCCESS) goto rtnsts;
  }

  /* Get security attributes for the thread */

  secattr = oz_knl_thread_getdefcresecattr (NULL);
  sts = oz_knl_secattr_fromname (OZ_THREAD_NAMESIZE, name, &name_l, NULL, &secattr);
  if (sts != OZ_SUCCESS) goto rtnsts;

  /* Create the thread */

  seckeys = oz_knl_thread_getseckeys (NULL);
  sts     = oz_knl_thread_create (process, priority, seckeys, initevent, exitevent, stacksize, 
                                  (void *)thentry, thparam, knlastmode, name_l, name, secattr, &thread);
  oz_knl_seckeys_increfc (seckeys, -1);

  /* Assign an handle to the thread */

  if (sts == OZ_SUCCESS) {
    if (h_thread_r != NULL) {
      sts = oz_knl_handle_assign (thread, cprocmode, h_thread_r);
      if (sts != OZ_SUCCESS) oz_knl_thread_abort (thread, sts);
    }
    oz_knl_thread_increfc (thread, -1);
  }

  /* Release stuff and return status */

rtnsts:
  if (exitevent != NULL) oz_knl_handle_putback (h_exitevent);
  if (initevent != NULL) oz_knl_handle_putback (h_initevent);
  if ((process  != NULL) && (h_process != 0)) oz_knl_handle_putback (h_process);
  if (secattr   != NULL) oz_knl_secattr_increfc (secattr, -1);
  oz_hw_cpu_setsoftint (si);
  return (sts);
}

/************************************************************************/
/*									*/
/*  This routine is called by the hardware layer whenever the 		*/
/*  oz_hw_syscall routine returns to the outer mode before actually 	*/
/*  returning to the caller.  It checks for queued ast's and calls the 	*/
/*  ast routines if there are any queued.				*/
/*									*/
/************************************************************************/

typedef struct { int state;
                 uLong aststs;
                 OZ_Astentry astentry;
                 void *astparam;
               } Capb;

void oz_sys_thread_checkast (OZ_Mchargs *mchargs)

{
  Capb capb;
  uLong sts;

  capb.state = 0;
  while ((sts = oz_sys_thread_deqast (&capb)) == OZ_FLAGWASSET) {
    (*(capb.astentry)) (capb.astparam, capb.aststs, mchargs);		// if there, call it with delivery inhibited
  }									// repeat to process all ast's
  if (sts != OZ_FLAGWASCLR) oz_sys_condhand_signal (2, sts, 0);		// signal all other errors
}

/********************************************************************************************************************************/
/*																*/
/*  Dequeue an deliverable ast													*/
/*																*/
/*    Input:															*/
/*																*/
/*	*state = 0 : initialize													*/
/*	      else : as returned by last call											*/
/*																*/
/*    Ouput:															*/
/*																*/
/*	oz_syscall_thread_deqast = OZ_FLAGWASCLR : no deliverable ast exists, ast mode restored to original value		*/
/*	                           OZ_FLAGWASSET : deliverable ast dequeued, ast mode set to inhibit further asts at that level	*/
/*	                                           *astentry, *astparam, *aststs = ast values					*/
/*	                                    else : error status									*/
/*	*state = 0 : express ast delivery was inhibited on input and still is, and thus there are no deliverable asts		*/
/*	         1 : express ast delivery was enabled and normal ast delivery was inhibited, maybe an express ast was dequeued	*/
/*	         2 : express and normal ast delivery was enabled, and an express ast was dequeued				*/
/*	         3 : express and normal ast delivery was enabled, maybe an normal ast was dequeued				*/
/*																*/
/********************************************************************************************************************************/

OZ_HW_SYSCALL_DEF_1 (thread_deqast, void *, capbv)

{
  Capb *capb;
  int si;
  OZ_Astmode astmode;
  OZ_Seclock *seclock;
  uLong sts;

  capb = capbv;
  si   = oz_hw_cpu_setsoftint (0);
  sts  = oz_knl_section_iolock (cprocmode, sizeof *capb, capb, 1, &seclock, NULL, NULL, NULL);
  if (sts != OZ_SUCCESS) goto rtn_si;

  /* Process state variable */

switch_state:
  switch (capb -> state) {

    /* This is where we start */

    case 0: {
      astmode = oz_knl_thread_setast (cprocmode, OZ_ASTMODE_INHEXP);	/* inhibit express ast delivery -- this should be the only place this is done */
      if (astmode == OZ_ASTMODE_INHIBIT) capb -> state = 1;		/* if normal ast's were inhibited, enter state 1 */
      if (astmode == OZ_ASTMODE_ENABLE)  capb -> state = 2;		/* if normal ast's were enabled, enter state 2 */
      if (capb -> state > 0) goto switch_state;
      if (astmode != OZ_ASTMODE_INHEXP) oz_crash ("oz_sys_thread_checkast: bad ast mode %d", astmode);
      sts = OZ_FLAGWASCLR;						/* express ast's already inhibited, just return -- can't delivery anything */
      break;
    }

    /* Process express ast's when normal ast delivery was inhibited */

    case 1: {
      sts = oz_knl_thread_deqast (cprocmode, 1, &(capb -> astentry), &(capb -> astparam), &(capb -> aststs)); /* normal ast's were inhibited, dequeue an express ast */
      if (sts == OZ_FLAGWASCLR) oz_knl_thread_setast (cprocmode, OZ_ASTMODE_INHIBIT); /* if none found, just inhibit normal ast delivery */
      break;
    }

    /* Process express ast's when normal ast delivery was enabled */

    case 2: {
      sts = oz_knl_thread_deqast (cprocmode, 1, &(capb -> astentry), &(capb -> astparam), &(capb -> aststs)); /* normal ast's were enabled, dequeue an express ast */
      if (sts != OZ_FLAGWASCLR) break;					/* if found, process it then come back for another */
      oz_knl_thread_setast (cprocmode, OZ_ASTMODE_INHIBIT);		/* if none found, just inhibit normal ast delivery */
      capb -> state = 3;						/* ... then start processing normal ast's */
									/* (fall through to state 3 processing) */
    }

    /* Process normal ast's until there are no more */

    case 3: {
      sts = oz_knl_thread_deqast (cprocmode, 0, &(capb -> astentry), &(capb -> astparam), &(capb -> aststs)); /* normal ast's were enabled, dequeue an normal ast */
      if (sts == OZ_FLAGWASCLR) oz_knl_thread_setast (cprocmode, OZ_ASTMODE_ENABLE); /* if none found, enable all ast delivery */
      break;
    }

    /* Who knows */

    default: {
      sts = OZ_BUGCHECK;
    }
  }

  oz_knl_section_iounlk (seclock);
rtn_si:
  oz_hw_cpu_setsoftint (si);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Set the ast enable mode for the caller's processor mode		*/
/*									*/
/*    Input:								*/
/*									*/
/*	cprocmode = caller's processor mode				*/
/*	astmode = new ast mode						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_syscall_thread_setast = OZ_FLAGWASCLR : previously inhibited	*/
/*	                           OZ_FLAGWASSET : previously enabled	*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_1 (thread_setast, OZ_Astmode, astmode)

{
  uLong sts;
  OZ_Astmode oldmode;

  if ((astmode != OZ_ASTMODE_RDONLY) && (astmode != OZ_ASTMODE_ENABLE) && (astmode != OZ_ASTMODE_INHIBIT)) return (OZ_BADPARAM);
  oldmode = oz_knl_thread_setast (cprocmode, astmode);
  return ((oldmode == OZ_ASTMODE_ENABLE) ? OZ_FLAGWASSET : OZ_FLAGWASCLR);
}

/************************************************************************/
/*									*/
/*  Exit thread with given status					*/
/*									*/
/************************************************************************/

uLong oz_sys_thread_exit (uLong status)

{
  uLong sts;
  OZ_Exhand_entry exhentry;
  void *exhparam;

  while ((sts = oz_sys_thread_exiteh (status, &exhentry, &exhparam)) == OZ_SUCCESS) {
    (*exhentry) (exhparam, status);
  }

  return (sts);
}

OZ_HW_SYSCALL_DEF_3 (thread_exiteh, uLong, status, OZ_Exhand_entry *, exhentry_r, void **, exhparam_r)

{
  int si;
  OZ_Thread *thread;

  /* If other than kernel mode, dequeue and process any exit handlers then call the iorundown routine then release handles */

  if (cprocmode != OZ_PROCMODE_KNL) {
    si = oz_hw_cpu_setsoftint (0);					// inhib softints as required by exhand_dequeue
    if (oz_knl_exhand_dequeue (cprocmode, exhentry_r, exhparam_r)) {	// see if there are any exit handlers
      oz_hw_cpu_setsoftint (si);					// ok, restore softints
      return (OZ_SUCCESS);						// tell caller to process exit handler
    }
    thread = oz_knl_thread_getcur ();					// no exit handlers, get thread pointer
    oz_knl_iorundown (thread, cprocmode);				// rundown the I/O started by thread at this proc mode
    oz_knl_handle_release_all (thread, cprocmode);			// release all handles opened by thread at the proc mode
    oz_hw_cpu_setsoftint (si);						// restore softint delivery
  }

  /* Kernel mode, call the kernel exit routine - note that it calls the kernel mode exit handlers, etc, itself */

  while (1) oz_knl_thread_exit (status);
}

/************************************************************************/
/*									*/
/*  Abort a thread							*/
/*									*/
/*    Input:								*/
/*									*/
/*	h_thread = handle to thread that is to be aborted		*/
/*	abortsts = status the aborted thread is to exit with		*/
/*									*/
/************************************************************************/

typedef struct { OZ_Thread *thread;
                 uLong status;
               } Ta;

OZ_HW_SYSCALL_DEF_2 (thread_abort, OZ_Handle, h_thread, uLong, abortsts)

{
  int si;
  uLong sts;
  OZ_Ast *ast;
  OZ_Datebin when;
  OZ_Thread *thread;
  OZ_Timer *timer;
  Ta *ta;

  si = oz_hw_cpu_setsoftint (0);							/* keep from being aborted */
  sts = oz_knl_handle_takeout (h_thread, cprocmode, OZ_SECACCMSK_WRITE, OZ_OBJTYPE_THREAD, &thread, NULL); /* translate handle to object pointer */
  if (sts == OZ_SUCCESS) {
    oz_knl_thread_increfc (thread, 1);
    oz_knl_handle_putback (h_thread);
    if (cprocmode != OZ_PROCMODE_KNL) {
      sts = oz_knl_ast_create (thread, cprocmode, thread_abort_usr, NULL, 1, &ast);
      if (sts == OZ_SUCCESS) {
        ta = OZ_KNL_PGPMALLOC (sizeof *ta);
        ta -> thread = thread;
        ta -> status = abortsts;
        when  = oz_hw_tod_getnow ();
        when += OZ_TIMER_RESOLUTION * 5;
        timer = oz_knl_timer_alloc ();
        oz_knl_timer_insert (timer, when, thread_abort_tmo, ta);
        oz_knl_thread_queueast (ast, abortsts);
      } else {
        oz_knl_thread_increfc (thread, -1);
      }
    } else {
      oz_knl_thread_abort (thread, abortsts);						/* queue abort ast to the thread */
      oz_knl_thread_increfc (thread, -1);						/* release the handle */
    }
  }
  oz_hw_cpu_setsoftint (si);								/* restore software interrupts */
  return (sts);										/* return handle translate status */
}

static void thread_abort_usr (void *dummy, uLong status, OZ_Mchargs *mchargs)

{
  oz_sys_thread_exit (status);
}

static void thread_abort_tmo (void *tav, OZ_Timer *timer)

{
  Ta *ta;

  ta = tav;
  oz_knl_timer_free (timer);
  oz_knl_thread_abort (ta -> thread, ta -> status);
  oz_knl_thread_increfc (ta -> thread, -1);
  OZ_KNL_PGPFREE (ta);
}

/************************************************************************/
/*									*/
/*  Set thread's security keys						*/
/*									*/
/*    Input:								*/
/*									*/
/*	h_thread = thread to set the keys of				*/
/*	seckeyssize = size of security keys				*/
/*	seckeysbuff = address of security keys				*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_3 (thread_setseckeys, OZ_Handle, h_thread, uLong, seckeyssize, const void *, seckeysbuff)

{
  int si;
  OZ_Seckeys *useckeys, *xseckeys;
  OZ_Thread *thread;
  uLong sts;

  si  = oz_hw_cpu_setsoftint (0);					/* keep from being aborted while we have the handle accessed */
  sts = OZ_SUCCESS;							/* default to current thread */
  thread = oz_knl_thread_getcur ();
  if (h_thread != 0) sts = oz_knl_handle_takeout (h_thread, cprocmode, OZ_SECACCMSK_WRITE, OZ_OBJTYPE_THREAD, &thread, NULL);
  if (sts == OZ_SUCCESS) {
    useckeys = oz_knl_user_getseckeys (NULL);				/* get keys the caller is allowed to give */
    sts = oz_knl_seckeys_create (seckeyssize, seckeysbuff, useckeys, &xseckeys); /* create new seckeys struct */
    if (sts == OZ_SUCCESS) {
      oz_knl_thread_setseckeys (thread, xseckeys);			/* set the target thread's security keys */
      oz_knl_seckeys_increfc (xseckeys, -1);				/* done with them */
    }
    oz_knl_seckeys_increfc (useckeys, -1);				/* release caller's authorized keys */
    if (h_thread != 0) oz_knl_handle_putback (h_thread);		/* release the thread */
  }
  oz_hw_cpu_setsoftint (si);						/* restore software interrupts */
  return (sts);								/* return handle translate status */
}

/************************************************************************/
/*									*/
/*  Set thread's default creation security attributes (who can access 	*/
/*  what the thread creates)						*/
/*									*/
/*    Input:								*/
/*									*/
/*	h_thread = thread to set the keys of				*/
/*	secattrsize = size of security attributes			*/
/*	secattrbuff = address of security attributes			*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_3 (thread_setdefcresecattr, OZ_Handle, h_thread, uLong, secattrsize, const void *, secattrbuff)

{
  int si;
  OZ_Secattr *secattr;
  OZ_Thread *thread;
  uLong sts;

  si  = oz_hw_cpu_setsoftint (0);							/* keep from being aborted while we have the handle accessed */
  sts = OZ_SUCCESS;									/* default to current thread */
  thread = oz_knl_thread_getcur ();
  if (h_thread != 0) sts = oz_knl_handle_takeout (h_thread, cprocmode, OZ_SECACCMSK_WRITE, OZ_OBJTYPE_THREAD, &thread, NULL); /* translate handle to object pointer */
  if (sts == OZ_SUCCESS) {
    sts = oz_knl_secattr_create (secattrsize, secattrbuff, NULL, &secattr);		/* copy what the caller gave us */
    if (sts == OZ_SUCCESS) {
      oz_knl_thread_setdefcresecattr (thread, secattr);					/* set the target thread's security attr */
      oz_knl_secattr_increfc (secattr, -1);						/* done with them */
    }
    if (h_thread != 0) oz_knl_handle_putback (h_thread);				/* release the thread */
  }
  oz_hw_cpu_setsoftint (si);								/* restore software interrupts */
  return (sts);										/* return status */
}

/************************************************************************/
/*									*/
/*  Set thread's security attributes (who can access the thread)	*/
/*									*/
/*    Input:								*/
/*									*/
/*	h_thread = thread to set the keys of				*/
/*	secattrsize = size of security attributes			*/
/*	secattrbuff = address of security attributes			*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_3 (thread_setsecattr, OZ_Handle, h_thread, uLong, secattrsize, const void *, secattrbuff)

{
  int si;
  OZ_Secattr *secattr;
  OZ_Thread *thread;
  uLong sts;

  si  = oz_hw_cpu_setsoftint (0);							/* keep from being aborted while we have the handle accessed */
  sts = OZ_SUCCESS;									/* default to current thread */
  thread = oz_knl_thread_getcur ();
  if (h_thread != 0) sts = oz_knl_handle_takeout (h_thread, cprocmode, OZ_SECACCMSK_WRITE, OZ_OBJTYPE_THREAD, &thread, NULL); /* translate handle to object pointer */
  if (sts == OZ_SUCCESS) {
    sts = oz_knl_secattr_create (secattrsize, secattrbuff, NULL, &secattr);		/* copy what the caller gave us */
    if (sts == OZ_SUCCESS) {
      oz_knl_thread_setsecattr (thread, secattr);					/* set the target thread's security attr */
      oz_knl_secattr_increfc (secattr, -1);						/* done with them */
    }
    if (h_thread != 0) oz_knl_handle_putback (h_thread);				/* release the thread */
  }
  oz_hw_cpu_setsoftint (si);								/* restore software interrupts */
  return (sts);										/* return status */
}

/************************************************************************/
/*									*/
/*  Set thread's base priority						*/
/*									*/
/*    Input:								*/
/*									*/
/*	h_thread = thread to set the keys of				*/
/*	basepri  = new base priority					*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_2 (thread_setbasepri, OZ_Handle, h_thread, uLong, basepri)

{
  int si;
  OZ_Thread *thread;
  uLong maxbasepri, sts;

  si  = oz_hw_cpu_setsoftint (0);							/* keep from being aborted while we have the handle accessed */
  sts = OZ_SUCCESS;									/* default to current thread */
  thread = oz_knl_thread_getcur ();
  maxbasepri = oz_knl_user_getmaxbasepri (NULL);					/* my max base prio is the most I can set */
  if (basepri > maxbasepri) basepri = maxbasepri;					/* limit what we're doing to that */
  if (h_thread != 0) sts = oz_knl_handle_takeout (h_thread, cprocmode, OZ_SECACCMSK_WRITE, OZ_OBJTYPE_THREAD, &thread, NULL); /* translate handle to object pointer */
  if (sts == OZ_SUCCESS) {
    oz_knl_thread_setbasepri (thread, basepri);						/* set the thread's base priority and also */
    oz_knl_thread_setcurprio (thread, basepri);						/* ... set current priority so it will reschedule */
    if (h_thread != 0) oz_knl_handle_putback (h_thread);				/* release the thread */
  }
  oz_hw_cpu_setsoftint (si);								/* restore software interrupts */
  return (sts);										/* return status */
}

/************************************************************************/
/*									*/
/*  Get thread's name string						*/
/*									*/
/*    Input:								*/
/*									*/
/*	h_thread = handle to thread to get name of			*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_thread_getname = OZ_SUCCESS : successful			*/
/*	                              else : error status		*/
/*	*buff = name string						*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_3 (thread_getname, OZ_Handle, h_thread, uLong, size, char *, buff)

{
  const char *name;
  int si;
  uLong sts;
  OZ_Thread *thread;

  si = oz_hw_cpu_setsoftint (0);							/* keep from being aborted while we have the handle accessed */
  sts = OZ_SUCCESS;									/* default to current thread */
  thread = oz_knl_thread_getcur ();
  if (h_thread != 0) sts = oz_knl_handle_takeout (h_thread, cprocmode, OZ_SECACCMSK_LOOK, OZ_OBJTYPE_THREAD, &thread, NULL); /* translate handle to object pointer */
  if (sts == OZ_SUCCESS) {
    name = oz_knl_thread_getname (thread);						/* point to thread name string */
    sts = strlen (name) + 1;								/* get length of name string including terminating null */
    if (sts < size) size = sts;								/* maximise with buffer size */
    sts = oz_knl_section_uput (cprocmode, size, name, buff);				/* copy out to user buffer */
    if (h_thread != 0) oz_knl_handle_putback (h_thread);				/* release the thread */
  }
  oz_hw_cpu_setsoftint (si);								/* restore software interrupts */
  return (sts);										/* return handle translate status */
}

/************************************************************************/
/*									*/
/*  Get thread's exit status						*/
/*									*/
/*    Input:								*/
/*									*/
/*	h_thread = handle to thread that is to be aborted		*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_thread_getexitsts = OZ_SUCCESS : successful		*/
/*	                        OZ_FLAGWASCLR : thread hasn't exited	*/
/*	                                 else : error status		*/
/*	*exitsts_r = exit status					*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_2 (thread_getexitsts, OZ_Handle, h_thread, uLong *, exitsts_r)

{
  int si;
  uLong sts;
  OZ_Thread *thread;

  si = oz_hw_cpu_setsoftint (0);							/* keep from being aborted */
  /* ?? make sure exitsts_r writable by cprocmode ?? */
  sts = oz_knl_handle_takeout (h_thread, cprocmode, OZ_SECACCMSK_READ, OZ_OBJTYPE_THREAD, &thread, NULL); /* translate handle to object pointer */
  if (sts == OZ_SUCCESS) {
    sts = oz_knl_thread_getexitsts (thread, exitsts_r);					/* get thread's exit status */
    oz_knl_handle_putback (h_thread);							/* release the handle */
  }
  oz_hw_cpu_setsoftint (si);								/* restore software interrupts */
  return (sts);										/* return handle translate status */
}

/************************************************************************/
/*									*/
/*  Get thread's exit event flag handle					*/
/*									*/
/*    Input:								*/
/*									*/
/*	h_thread = handle to thread					*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_thread_getexitevent = OZ_SUCCESS : successful		*/
/*	                                   else : error status		*/
/*	*h_event_r = 0 : there is no exit event flag for the thread	*/
/*	          else : exit event flag handle				*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_2 (thread_getexitevent, OZ_Handle, h_thread, OZ_Handle *, h_event_r)

{
  int si;
  uLong sts;
  OZ_Event *event;
  OZ_Thread *thread;

  si = oz_hw_cpu_setsoftint (0);							/* keep from being aborted */
  /* ?? make sure h_event_r writable by cprocmode ?? */
  *h_event_r = 0;
  sts = oz_knl_handle_takeout (h_thread, cprocmode, OZ_SECACCMSK_READ, OZ_OBJTYPE_THREAD, &thread, NULL); /* translate handle to object pointer */
  if (sts == OZ_SUCCESS) {
    event = oz_knl_thread_getexitevent (thread);					/* get thread's exit event flag pointer */
    if (event != NULL) sts = oz_knl_handle_assign (event, cprocmode, h_event_r);	/* got it, assign an handle to it */
    oz_knl_handle_putback (h_thread);							/* release the thread */
  }
  oz_hw_cpu_setsoftint (si);								/* restore software interrupts */
  return (sts);										/* return composite status */
}

/************************************************************************/
/*									*/
/*  Orphan a thread							*/
/*									*/
/*    Input:								*/
/*									*/
/*	h_thread = thread to be orphaned (or 0 for self)		*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_thread_orphan = OZ_SUCCESS : thread is now an orphan	*/
/*	                             else : error status		*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_1 (thread_orphan, OZ_Handle, h_thread)

{
  int si;
  uLong sts;
  OZ_Thread *thread;

  si = oz_hw_cpu_setsoftint (0);							/* keep from being aborted */

  sts = OZ_SUCCESS;
  if (h_thread == 0) thread = oz_knl_thread_getcur ();					/* maybe orphaning self */
  else sts = oz_knl_handle_takeout (h_thread, cprocmode, OZ_SECACCMSK_WRITE, OZ_OBJTYPE_THREAD, &thread, NULL); /* translate handle to object pointer */
  if (sts == OZ_SUCCESS) {
    oz_knl_thread_orphan (thread);							/* orphan thread */
    if (h_thread != 0) oz_knl_handle_putback (h_thread);				/* release the thread */
  }
  oz_hw_cpu_setsoftint (si);								/* restore software interrupts */
  return (sts);										/* return composite status */
}

/************************************************************************/
/*									*/
/*  Suspend a thread							*/
/*									*/
/*    Input:								*/
/*									*/
/*	h_thread = thread to be suspended				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_thread_suspend = OZ_FLAGWASSET : thread was already suspended
/*	                        OZ_FLAGWASCLR : thread was not already suspended
/*	                                 else : error status		*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_1 (thread_suspend, OZ_Handle, h_thread)

{
  int si, wassusp;
  uLong sts;
  OZ_Thread *thread;

  si = oz_hw_cpu_setsoftint (0);							/* keep from being aborted */

  sts = oz_knl_handle_takeout (h_thread, cprocmode, OZ_SECACCMSK_WRITE, OZ_OBJTYPE_THREAD, &thread, NULL); /* translate handle to object pointer */
  if (sts == OZ_SUCCESS) {
    wassusp = oz_knl_thread_suspend (thread);						/* suspend thread */
    sts = wassusp ? OZ_FLAGWASSET : OZ_FLAGWASCLR;
    oz_knl_handle_putback (h_thread);							/* release the thread */
  }
  oz_hw_cpu_setsoftint (si);								/* restore software interrupts */
  return (sts);										/* return composite status */
}

/************************************************************************/
/*									*/
/*  Resume a thread							*/
/*									*/
/*    Input:								*/
/*									*/
/*	h_thread = thread to be resumed					*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_thread_resume = OZ_FLAGWASSET : thread was suspended	*/
/*	                       OZ_FLAGWASCLR : thread was not suspended	*/
/*	                                else : error status		*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_1 (thread_resume, OZ_Handle, h_thread)

{
  int si, wassusp;
  uLong sts;
  OZ_Thread *thread;

  si = oz_hw_cpu_setsoftint (0);							/* keep from being aborted */

  sts = oz_knl_handle_takeout (h_thread, cprocmode, OZ_SECACCMSK_WRITE, OZ_OBJTYPE_THREAD, &thread, NULL); /* translate handle to object pointer */
  if (sts == OZ_SUCCESS) {
    wassusp = oz_knl_thread_resume (thread);						/* resume thread */
    sts = wassusp ? OZ_FLAGWASSET : OZ_FLAGWASCLR;
    oz_knl_handle_putback (h_thread);							/* release the thread */
  }
  oz_hw_cpu_setsoftint (si);								/* restore software interrupts */
  return (sts);										/* return composite status */
}

/************************************************************************/
/*									*/
/*  Get an handle to a thread given its id number			*/
/*									*/
/*    Input:								*/
/*									*/
/*	threadid = thread's id number					*/
/*	           (0 for current thread)				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_thread_getbyid = OZ_SUCCESS : successfully retrieved	*/
/*	                   OZ_NOSUCHTHREAD : there is no such thread	*/
/*	                              else : error assigning handle	*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_2 (thread_getbyid, OZ_Threadid, threadid, OZ_Handle *, h_thread_r)

{
  int si;
  uLong sts;
  OZ_Thread *thread;

  si = oz_hw_cpu_setsoftint (0);					/* keep from being aborted in here */
  thread = oz_knl_thread_getbyid (threadid);				/* find the thread given its id number */
  sts = OZ_NOSUCHTHREAD;						/* assume it wasn't found */
  if (thread != NULL) {							/* see if it was found */
    sts = oz_knl_handle_assign (thread, cprocmode, h_thread_r);		/* got it, assign an handle to it */
    oz_knl_thread_increfc (thread, -1);					/* ... release thread pointer */
  }
  oz_hw_cpu_setsoftint (si);						/* restore software interrupts */
  return (sts);								/* return final status */
}

/************************************************************************/
/*									*/
/*  Queue an ast to a thread						*/
/*									*/
/*    Input:								*/
/*									*/
/*	procmode = processor mode for the ast				*/
/*	h_thread = thread to queue it to (or 0 for self)		*/
/*	astentry = ast entrypoint					*/
/*	astparam = ast parameter					*/
/*	express  = 0 : normal ast					*/
/*	           1 : express ast					*/
/*	status   = status value to pass to ast				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_thread_queueast = OZ_SUCCESS : successfully queued	*/
/*	                               else : error status		*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_6 (thread_queueast, OZ_Procmode, procmode, OZ_Handle, h_thread, OZ_Astentry, astentry, void *, astparam, int, express, uLong, status)

{
  int si;
  OZ_Ast *ast;
  OZ_Thread *thread;
  uLong sts;

  if (procmode < cprocmode) procmode = cprocmode;
  si = oz_hw_cpu_setsoftint (0);							/* keep from being aborted */
  if (h_thread == 0) { thread = oz_knl_thread_getcur (); sts = OZ_SUCCESS; }		/* get thread pointer */
  else sts = oz_knl_handle_takeout (h_thread, cprocmode, OZ_SECACCMSK_WRITE, OZ_OBJTYPE_THREAD, &thread, NULL);
  if (sts == OZ_SUCCESS) {
    sts = oz_knl_ast_create (thread, procmode, astentry, astparam, express, &ast);	/* create an ast for it */
    if (sts == OZ_SUCCESS) oz_knl_thread_queueast (ast, status);			/* queue the ast to it */
    if (h_thread != 0) oz_knl_handle_putback (h_thread);				/* all done with thread pointer */
  }
  oz_hw_cpu_setsoftint (si);								/* restore software interrupts */
  return (sts);										/* return handle translate status */
}
