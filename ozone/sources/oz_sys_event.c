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

#include "ozone.h"
#include "oz_knl_ast.h"
#include "oz_knl_event.h"
#include "oz_knl_handle.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_procmode.h"
#include "oz_knl_quota.h"
#include "oz_knl_section.h"
#include "oz_knl_status.h"
#include "oz_sys_event.h"
#include "oz_sys_syscall.h"

static void eventastcb (void *astv, OZ_Event *event);

/************************************************************************/
/*									*/
/*  Create an event flag						*/
/*									*/
/*    Input:								*/
/*									*/
/*	procmode = processor mode					*/
/*	name = pointer to event flag name(secattr) string		*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_event_create = OZ_SUCCESS : successful			*/
/*	                            else : error status			*/
/*	*h_event_r = event flag handle					*/
/*	the event's value is initialized to zero			*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_3 (event_create, OZ_Procmode, procmode, const char *, name, OZ_Handle *, h_event_r)

{
  int name_l, si;
  uLong sts;
  OZ_Event *event;
  OZ_Secattr *secattr;
  OZ_Seclock *sl_name, *sl_h_event_r;

  if (procmode < cprocmode) procmode = cprocmode;				/* maximise processor mode */
  si  = oz_hw_cpu_setsoftint (0);						/* keep thread from being deleted */
  sts = oz_knl_section_blockz (cprocmode, OZ_EVENT_NAMESIZE, name, NULL, &sl_name); /* lock the name string in memory */
  if (sts == OZ_SUCCESS) {
    sts = oz_knl_section_blockw (cprocmode, sizeof *h_event_r, h_event_r, &sl_h_event_r);
    if (sts == OZ_SUCCESS) {
      secattr = oz_knl_thread_getdefcresecattr (NULL);				/* get its security attributes */
      sts = oz_knl_secattr_fromname (OZ_EVENT_NAMESIZE, name, &name_l, NULL, &secattr);
      if (sts == OZ_SUCCESS) {
        sts = oz_knl_event_create (name_l, name, secattr, &event);		/* create an event flag */
        if (sts == OZ_SUCCESS) {
          sts = oz_knl_handle_assign (event, procmode, h_event_r);		/* assign an handle to it */
          oz_knl_event_increfc (event, -1);					/* release event flag pointer */
        }
        oz_knl_secattr_increfc (secattr, -1);					/* release secattr pointer */
      }
      oz_knl_section_bunlock (sl_h_event_r);
    }
    oz_knl_section_bunlock (sl_name);
  }
  oz_hw_cpu_setsoftint (si);							/* restore software interrupt enable */
  return (sts);									/* return composite status */
}

/************************************************************************/
/*									*/
/*  Set event flag timer interval.  Event flag gets incremented for 	*/
/*  each interval.							*/
/*									*/
/*    Input:								*/
/*									*/
/*	h_event  = handle to event flag					*/
/*	interval = 0 : shut of interval timer				*/
/*	        else : interval to increment event flag at		*/
/*	basetime = time to start at					*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_event_setimint = OZ_SUCCESS : interval set		*/
/*	                              else : error status		*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_3 (event_setimint, OZ_Handle, h_event, OZ_Datebin, interval, OZ_Datebin, basetime)

{
  int si;
  uLong sts;
  OZ_Event *event;

  si  = oz_hw_cpu_setsoftint (0);				// inhibit thread abortions
  sts = oz_knl_handle_takeout (h_event, cprocmode, OZ_SECACCMSK_WRITE, OZ_OBJTYPE_EVENT, &event, NULL);
  if (sts == OZ_SUCCESS) {
    sts = oz_knl_event_setimint (event, interval, basetime);	// set new timer interval
    oz_knl_handle_putback (h_event);
  }
  oz_hw_cpu_setsoftint (si);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Increment event flag value						*/
/*									*/
/*    Input:								*/
/*									*/
/*	procmode = processor mode					*/
/*	h_event  = event flag handle					*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_event_inc = OZ_SUCCESS : success				*/
/*	                         else : error status			*/
/*	*value_r = previous value					*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_4 (event_inc, OZ_Procmode, procmode, OZ_Handle, h_event, Long, inc, Long *, value_r)

{
  int si;
  uLong sts;
  Long value;
  OZ_Event *event;
  OZ_Secaccmsk secaccmsk;

  if (procmode < cprocmode) procmode = cprocmode;
  secaccmsk = OZ_SECACCMSK_READ;
  if (inc != 0) secaccmsk = OZ_SECACCMSK_READ | OZ_SECACCMSK_WRITE;

  si = oz_hw_cpu_setsoftint (0);							/* inhibit thread deletion */
  if ((value_r != NULL) && !OZ_HW_WRITABLE (sizeof *value_r, value_r, procmode)) sts = OZ_ACCVIO;
  else {
    sts = oz_knl_handle_takeout (h_event, procmode, secaccmsk, OZ_OBJTYPE_EVENT, &event, NULL); /* look up the event flag */
    if (sts == OZ_SUCCESS) {
      value = oz_knl_event_inc (event, inc);						/* increment it */
      if (value_r != NULL) *value_r = value;						/* return previous count */
      oz_knl_handle_putback (h_event);							/* release event flag */
    }
  }
  oz_hw_cpu_setsoftint (si);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Set event flag value						*/
/*									*/
/*    Input:								*/
/*									*/
/*	procmode = processor mode					*/
/*	h_event  = event flag handle					*/
/*	value    = new value						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_event_inc = OZ_SUCCESS : success				*/
/*	                         else : error status			*/
/*	*value_r = previous value					*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_4 (event_set, OZ_Procmode, procmode, OZ_Handle, h_event, Long, value, Long *, value_r)

{
  int si;
  uLong sts;
  OZ_Event *event;

  if (procmode < cprocmode) procmode = cprocmode;

  si = oz_hw_cpu_setsoftint (0);							/* inhibit thread deletion */
  if ((value_r != NULL) && !OZ_HW_WRITABLE (sizeof *value_r, value_r, procmode)) sts = OZ_ACCVIO;
  else {
    sts = oz_knl_handle_takeout (h_event, procmode, OZ_SECACCMSK_WRITE, OZ_OBJTYPE_EVENT, &event, NULL); /* look up the event flag */
    if (sts == OZ_SUCCESS) {
      value = oz_knl_event_set (event, value);						/* set it */
      if (value_r != NULL) *value_r = value;						/* return previous count */
      oz_knl_handle_putback (h_event);							/* release event flag */
    }
  }
  oz_hw_cpu_setsoftint (si);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Wait for a single event flag					*/
/*									*/
/*    Input:								*/
/*									*/
/*	procmode = processor mode					*/
/*	h_event  = event flag handle to wait for			*/
/*	astloop  = 0 : return if OZ_ASTDELIVERED			*/
/*	           1 : loop if OZ_ASTDELIVERED				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_event_wait = OZ_FLAGWASSET : flag was set on entry	*/
/*                          OZ_FLAGWASCLR : flag was clear on entry	*/
/*                                   else : some status from scheduler	*/
/*                                          (like OZ_ASTDELIVERED)	*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_3 (event_wait, OZ_Procmode, procmode, OZ_Handle, h_event, int, astloop)

{
  int si;
  uLong sts;
  OZ_Eventlist eventlist;

  if (procmode < cprocmode) procmode = cprocmode;

  si = oz_hw_cpu_setsoftint (0);							// inhibit thread deletion while we have a ref count on event flag
  sts = oz_knl_handle_takeout (h_event, procmode, OZ_SECACCMSK_READ, OZ_OBJTYPE_EVENT, &eventlist.event, NULL); // look up the event flag
  if (sts == OZ_SUCCESS) {
    sts = oz_knl_event_waitlist (1, &eventlist, procmode, si);				// wait for event, maybe an ast breaks us out
    oz_knl_handle_putback (h_event);							// unlock the event flag
  }
  oz_hw_cpu_setsoftint (si);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Wait for any one of a number of events				*/
/*									*/
/*    Input:								*/
/*									*/
/*	procmode = processor mode					*/
/*	nevents  = number of elements in events				*/
/*	h_events = array of event flag handles to wait for		*/
/*	astloop  = 0 : return if OZ_ASTDELIVERED			*/
/*	           1 : loop if OZ_ASTDELIVERED				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_event_nwait = OZ_FLAGWASSET : at least one flag was set on entry
/*                           OZ_FLAGWASCLR : all flags were clear on entry
/*                                    else : some status from scheduler	*/
/*                                           (like OZ_ASTDELIVERED)	*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_4 (event_nwait, OZ_Procmode, procmode, uLong, nevents, OZ_Handle *, h_events, int, astloop)

{
  int si;
  uLong i, sts;
  OZ_Eventlist *eventlist;
  OZ_Handle h_event;
  OZ_Seclock *sectionlock;

  if (procmode < cprocmode) procmode = cprocmode;

  sectionlock = NULL;
  si  = oz_hw_cpu_setsoftint (0);								/* inhibit thread deletion */
  sts = oz_knl_section_blockr (cprocmode, nevents * sizeof *h_events, h_events, &sectionlock);	/* lock event handle array */
  if (sts == OZ_SUCCESS) {
    eventlist = OZ_KNL_NPPMALLOQ (nevents * sizeof *eventlist);					/* allocate an array for event object pointers */
    sts = OZ_EXQUOTANPP;
    if (eventlist != NULL) {
      for (i = 0; i < nevents; i ++) {
        h_event = h_events[i];									/* in case h_events[i] changes on us */
        sts = oz_knl_handle_takeout (h_event, procmode, OZ_SECACCMSK_READ, OZ_OBJTYPE_EVENT, &(eventlist[i].event), NULL); /* look up the event flag */
        if (sts != OZ_SUCCESS) break;
        oz_knl_event_increfc (eventlist[i].event, 1);
        oz_knl_handle_putback (h_event);
      }
      if (sts == OZ_SUCCESS) {
        oz_knl_section_bunlock (sectionlock);							/* unlock handle array during wait */
        sectionlock = NULL;
        sts = oz_knl_event_waitlist (nevents, eventlist, procmode, si);				/* wait for event(s), maybe an ast breaks us out */
      }
      while (i > 0) oz_knl_event_increfc (eventlist[--i].event, -1);				/* unlock the event flags */
      OZ_KNL_NPPFREE (eventlist);								/* free off eventlist array */
    }
    if (sectionlock != NULL) oz_knl_section_bunlock (sectionlock);				/* unlock handle array */
  }
  oz_hw_cpu_setsoftint (si);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Cause an ast to be called when event flag is set			*/
/*									*/
/*    Input:								*/
/*									*/
/*	procmode = ast processor mode					*/
/*	h_event  = event flag handle					*/
/*	astentry = ast routine entrypoint				*/
/*	astparam = ast routine parameter				*/
/*	express  = 0 : deliver only if ast delivery enabled		*/
/*	           1 : deliver even if ast delivery inhibited		*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_5 (event_ast, OZ_Procmode, procmode, OZ_Handle, h_event, OZ_Astentry, astentry, void *, astparam, int, express)

{
  int si;
  OZ_Ast *ast;
  OZ_Event *event;
  OZ_Thread *thread;
  uLong sts;

  if (procmode < cprocmode) procmode = cprocmode;							/* maximise processor mode */

  si = oz_hw_cpu_setsoftint (0);									/* inhibit thread deletion */
  sts = oz_knl_handle_takeout (h_event, procmode, OZ_SECACCMSK_READ, OZ_OBJTYPE_EVENT, &event, NULL);	/* look up the event flag */
  if (sts == OZ_SUCCESS) {
    thread = oz_knl_thread_getcur ();
    sts = oz_knl_ast_create (thread, procmode, astentry, astparam, express, &ast);			/* create ast object */
    if (sts == OZ_SUCCESS) {
      sts = oz_knl_event_queuecb (event, eventastcb, ast);						/* queue ast to event flag */
      if (sts == OZ_FLAGWASCLR) sts = OZ_SUCCESS;							/* if it was queued, return success status */
      else if (sts == OZ_FLAGWASSET) {									/* if not queued because event flag is set, */
        oz_knl_thread_queueast (ast, OZ_SUCCESS);							/* ... queue it directly */
        sts = OZ_SUCCESS;										/* ... and return success status */
      }
    }
    oz_knl_handle_putback (h_event);									/* release event flag */
  }
  oz_hw_cpu_setsoftint (si);
  return (sts);
}

static void eventastcb (void *astv, OZ_Event *event)

{
  oz_knl_thread_queueast (astv, (event == NULL) ? OZ_EVENTABORTED : OZ_SUCCESS);
}
