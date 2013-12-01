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

#include "oz_sys_syscall.h"
#include "oz_sys_io.h"

#include "oz_knl_devio.h"
#include "oz_knl_event.h"
#include "oz_knl_handle.h"
#include "oz_knl_lock.h"
#include "oz_knl_procmode.h"
#include "oz_knl_section.h"
#include "oz_knl_security.h"
#include "oz_knl_status.h"

#include "oz_sys_event.h"

/************************************************************************/
/*									*/
/*  Allocate device for exclusive use of a given user, job, process or 	*/
/*  thread								*/
/*									*/
/*    Input:								*/
/*									*/
/*	devname = name of device to allocate				*/
/*	h_alloc = 0 : alloc to caller's user/job/process/thread		*/
/*	       else : alloc to this user/job/process/thread		*/
/*	objtype = type of 'h_alloc' object				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_io_alloc = OZ_SUCCESS : successful			*/
/*	             OZ_DEVALLOCATED : was allocd to someone else	*/
/*	               OZ_DEVICEBUSY : had non-null channels assigned	*/
/*	               OZ_INVOBJTYPE : not an user/job/process/thread	*/
/*									*/
/************************************************************************/

static uLong io_alloc (OZ_Procmode cprocmode, const char *devname, OZ_Handle h_alloc, OZ_Objtype objtype, 
                       uLong (*op) (OZ_Devunit *devunit, void *alloc_obj));

OZ_HW_SYSCALL_DEF_3 (io_alloc, const char *, devname, OZ_Handle, h_alloc, OZ_Objtype, objtype)

{
  return (io_alloc (cprocmode, devname, h_alloc, objtype, oz_knl_devunit_alloc));
}

/************************************************************************/
/*									*/
/*  Re-allocate device for exclusive use of a given user, job, process 	*/
/*  or thread								*/
/*									*/
/*    Input:								*/
/*									*/
/*	devname = name of device to allocate				*/
/*	h_alloc = 0 : re-alloc to caller's user/job/process/thread	*/
/*	       else : re-alloc to this user/job/process/thread		*/
/*	objtype = type of 'h_alloc' object				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_io_realloc = OZ_SUCCESS : successful			*/
/*	               OZ_DEVALLOCATED : was allocd to someone else	*/
/*	                 OZ_DEVICEBUSY : had non-null channels assigned	*/
/*	                 OZ_INVOBJTYPE : not an user/job/process/thread	*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_3 (io_realloc, const char *, devname, OZ_Handle, h_alloc, OZ_Objtype, objtype)

{
  return (io_alloc (cprocmode, devname, h_alloc, objtype, oz_knl_devunit_realloc));
}

/* Do the real work here */

static uLong io_alloc (OZ_Procmode cprocmode, const char *devname, OZ_Handle h_alloc, OZ_Objtype objtype, 
                       uLong (*op) (OZ_Devunit *devunit, void *alloc_obj))

{
  char unitname[OZ_DEVUNIT_NAMESIZE];
  int si;
  OZ_Devunit *devunit;
  uLong sts;
  void *alloc_obj;

  devunit   = NULL;
  alloc_obj = NULL;

  si  = oz_hw_cpu_setsoftint (0);							// keep thread from being deleted
  sts = oz_knl_section_ugetz (cprocmode, sizeof unitname, devname, unitname, NULL);	// get device name string
  if (sts != OZ_SUCCESS) goto rtn;
  devunit = oz_knl_devunit_lookup (unitname);						// find the device
  sts = OZ_BADDEVNAME;
  if (devunit == NULL) goto rtn;

  if (h_alloc != 0) {									// see if explicit user/job/process/thread
    sts = oz_knl_handle_takeout (h_alloc, cprocmode, OZ_SECACCMSK_WRITE, objtype, &alloc_obj, NULL); // ok, get object pointer
    if (sts != OZ_SUCCESS) goto rtn;
    oz_knl_handle_objincrefc (objtype, alloc_obj, 1);
    oz_knl_handle_putback (h_alloc);
  } else {
    switch (objtype) {									// implicit, get caller's user/job/process/thread
      case OZ_OBJTYPE_USER: {
        alloc_obj = oz_knl_job_getuser (NULL);
        oz_knl_user_increfc (alloc_obj, 1);
        break;
      }
      case OZ_OBJTYPE_JOB: {
        alloc_obj = oz_knl_process_getjob (NULL);
        oz_knl_job_increfc (alloc_obj, 1);
        break;
      }
      case OZ_OBJTYPE_PROCESS: {
        alloc_obj = oz_knl_thread_getprocess (NULL);
        oz_knl_process_increfc (alloc_obj, 1);
        break;
      }
      case OZ_OBJTYPE_THREAD: {
        alloc_obj = oz_knl_thread_getcur ();
        oz_knl_thread_increfc (alloc_obj, 1);
        break;
      }
      default: {
        sts = OZ_INVOBJTYPE;
        goto rtn;
      }
    }
  }

  sts = (*op) (devunit, alloc_obj);							// try to re-allocate device

rtn:
  if (devunit != NULL) oz_knl_devunit_increfc (devunit, -1);				// release device refcount
  if (alloc_obj != NULL) oz_knl_handle_objincrefc (objtype, alloc_obj, -1);		// release user/job/process/thread refcount
  oz_hw_cpu_setsoftint (si);								// allow abortions
  return (sts);
}

/************************************************************************/
/*									*/
/*  Deallocate a device							*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_1 (io_dealloc, const char *, devname)

{
  char unitname[OZ_DEVUNIT_NAMESIZE];
  int si;
  OZ_Devunit *devunit;
  uLong sts;

  devunit = NULL;

  si  = oz_hw_cpu_setsoftint (0);							// keep thread from being deleted
  sts = oz_knl_section_ugetz (cprocmode, sizeof unitname, devname, unitname, NULL);	// get device name string
  if (sts != OZ_SUCCESS) goto rtn;
  devunit = oz_knl_devunit_lookup (unitname);						// find the device
  sts = OZ_BADDEVNAME;
  if (devunit == NULL) goto rtn;

  sts = oz_knl_devunit_dealloc (devunit, NULL);						// try to de-allocate device

rtn:
  if (devunit != NULL) oz_knl_devunit_increfc (devunit, -1);				// release device refcount
  oz_hw_cpu_setsoftint (si);								// allow abortions
  return (sts);
}

/************************************************************************/
/*									*/
/*  Assign an io channel to a device					*/
/*									*/
/*    Input:								*/
/*									*/
/*	procmode = processor mode to assign channel at			*/
/*	devname  = pointer to device_name(chan_secattrs) string		*/
/*	lockmode = lock mode to assign channel at			*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_io_assign = OZ_SUCCESS : successful				*/
/*	                     else : error status			*/
/*	*h_iochan_r = i/o channel handle				*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_4 (io_assign, OZ_Procmode, procmode, OZ_Handle *, h_iochan_r, const char *, devname, OZ_Lockmode, lockmode)

{
  char unitname[OZ_DEVUNIT_NAMESIZE];
  int si, unitname_l;
  uLong sts;
  OZ_Devunit *devunit;
  OZ_Iochan *iochan;
  OZ_Secaccmsk secaccmsk;
  OZ_Secattr *chan_secattr, *secattr;
  OZ_Seckeys *seckeys;
  OZ_Seclock *sl_devname, *sl_h_iochan_r;

  sl_devname    = NULL;
  sl_h_iochan_r = NULL;

  if (procmode < cprocmode) procmode = cprocmode;

  secaccmsk = OZ_SECACCMSK_LOOK;
  if (OZ_LOCK_ALLOW_TEST (lockmode, OZ_LOCK_ALLOWS_SELF_READ))  secaccmsk |= OZ_SECACCMSK_READ;
  if (OZ_LOCK_ALLOW_TEST (lockmode, OZ_LOCK_ALLOWS_SELF_WRITE)) secaccmsk |= OZ_SECACCMSK_WRITE;

  si  = oz_hw_cpu_setsoftint (0);						/* keep thread from being deleted */
  sts = oz_knl_section_blockz (cprocmode, OZ_DEVUNIT_NAMESIZE, devname, NULL, &sl_devname); /* lock parameters */
  if (sts != OZ_SUCCESS) goto rtn;
  sts = oz_knl_section_blockw (cprocmode, sizeof *h_iochan_r, h_iochan_r, &sl_h_iochan_r);
  if (sts != OZ_SUCCESS) goto rtn;
  chan_secattr = oz_knl_thread_getdefcresecattr (NULL);				/* get secattrs for channel */
  sts = oz_knl_secattr_fromname (sizeof unitname, devname, &unitname_l, NULL, &chan_secattr);
  if (sts != OZ_SUCCESS) goto rtn;
  movc4 (unitname_l, devname, sizeof unitname, unitname);
  devunit = oz_knl_devunit_lookup (unitname);					/* look up the device unit */
  if (devunit == NULL) sts = OZ_BADDEVNAME;
  else {
    secattr = oz_knl_devunit_getsecattr (devunit);				/* make sure we can access the device unit */
    seckeys = oz_knl_thread_getseckeys (NULL);
    sts = oz_knl_security_check (secaccmsk, seckeys, secattr);
    oz_knl_secattr_increfc (secattr, -1);
    oz_knl_seckeys_increfc (seckeys, -1);
    if (sts == OZ_SUCCESS) sts = oz_knl_iochan_create (devunit, lockmode, procmode, chan_secattr, &iochan); /* create an i/o channel object and point it to the device */
    oz_knl_devunit_increfc (devunit, -1);					/* channel assigned or not, release our devunit pointer */
    if (sts == OZ_SUCCESS) {
      sts = oz_knl_handle_assign (iochan, procmode, h_iochan_r);		/* if successful, assign an handle to it */
      oz_knl_iochan_increfc (iochan, -1);					/* release iochan pointer */
    }
  }

rtn:
  oz_knl_secattr_increfc (chan_secattr, -1);
  oz_knl_section_bunlock (sl_devname);						/* unlock parameters */
  oz_knl_section_bunlock (sl_h_iochan_r);
  oz_hw_cpu_setsoftint (si);							/* restore software interrupt enable */
  return (sts);									/* return composite status */
}

/************************************************************************/
/*									*/
/*  Copy an I/O channel							*/
/*									*/
/*    Input:								*/
/*									*/
/*	procmode = processor mode to assign channel at			*/
/*	h_iochan = old I/O channel's handle				*/
/*	lockmode = lock mode to assign new channel at			*/
/*	           or OZ_LOCKMODE_XX for same				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_io_chancopy = OZ_SUCCESS : successful			*/
/*	                       else : error status			*/
/*	*h_iochan_r = new i/o channel handle				*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_4 (io_chancopy, OZ_Procmode, procmode, OZ_Handle, h_iochan, OZ_Lockmode, lockmode, OZ_Handle *, h_iochan_r)

{
  int si;
  OZ_Iochan *newiochan, *oldiochan;
  OZ_Seclock *sl_h_iochan_r;
  uLong sts;

  if (procmode < cprocmode) procmode = cprocmode;

  si  = oz_hw_cpu_setsoftint (0);						/* keep thread from being deleted */
  sts = oz_knl_section_blockw (cprocmode, sizeof *h_iochan_r, h_iochan_r, &sl_h_iochan_r);
  if (sts != OZ_SUCCESS) {
    sts = oz_knl_handle_takeout (h_iochan, procmode, 0, OZ_OBJTYPE_IOCHAN, &oldiochan, NULL);
    if (sts == OZ_SUCCESS) {
      sts = oz_knl_iochan_copy (oldiochan, lockmode, procmode, &newiochan);	/* copy, making access checks */
      oz_knl_handle_putback (h_iochan);						/* release old i/o channel */
      if (sts == OZ_SUCCESS) {
        sts = oz_knl_handle_assign (newiochan, procmode, h_iochan_r);		/* if successful, assign an handle to it */
        oz_knl_iochan_increfc (newiochan, -1);					/* release iochan pointer */
      }
    }
    oz_knl_section_bunlock (sl_h_iochan_r);
  }
  oz_hw_cpu_setsoftint (si);							/* restore software interrupt enable */
  return (sts);									/* return composite status */
}

/************************************************************************/
/*									*/
/*  Abort the i/o on a channel						*/
/*									*/
/*    Input:								*/
/*									*/
/*	procmode = processor mode to abort the i/o for			*/
/*	h_iochan = i/o channel handle					*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_io_abort = OZ_SUCCESS : success				*/
/*	                    else : error status				*/
/*									*/
/*    Note:								*/
/*									*/
/*	In general, the call completes immediately without actually 	*/
/*	having aborted the i/o requests.  You must still wait for the 	*/
/*	requests to complete to be sure they have aborted.  Some 	*/
/*	drivers may treat this as a no-op (like reading blocks from a 	*/
/*	physically connected disk drive).				*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_2 (io_abort, OZ_Procmode, procmode, OZ_Handle, h_iochan)

{
  int si;
  uLong sts;
  OZ_Iochan *iochan;

  if (procmode < cprocmode) procmode = cprocmode;

  si = oz_hw_cpu_setsoftint (0);							/* inhibit thread deletion */
  sts = oz_knl_handle_takeout (h_iochan, procmode, OZ_SECACCMSK_WRITE, OZ_OBJTYPE_IOCHAN, &iochan, NULL); /* look up the i/o channel */
  if (sts == OZ_SUCCESS) {
    oz_knl_ioabort (iochan, procmode);							/* abort the i/o */
    oz_knl_handle_putback (h_iochan);							/* release i/o channel */
  }
  oz_hw_cpu_setsoftint (si);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Start I/O request and wait for completion				*/
/*									*/
/*    Input:								*/
/*									*/
/*	procmode = processor mode to perform i/o for			*/
/*	h_iochan = i/o channel handle					*/
/*	h_event  = (ignored)						*/
/*	funcode  = i/o function code					*/
/*	as       = i/o argument block size				*/
/*	ap       = i/o argument block pointer				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_io = queueing or completion status			*/
/*									*/
/************************************************************************/

uLong oz_sys_io (OZ_Procmode procmode, OZ_Handle h_iochan, OZ_Handle h_event, uLong funcode, uLong as, void *ap)

{
  uLong sts;
  volatile uLong status;

  /* Set async completion status to 'pending' to start with */

  status = OZ_PENDING;

  /* Start the i/o request and wait for something */

  sts = oz_sys_io_wait (procmode, h_iochan, &status, funcode, as, ap);

  /* If successfully started, wait for the status to change from pending to something else */

  if (sts == OZ_STARTED) {					// make sure driver intends to store status and set event flag
    while ((sts = status) == OZ_PENDING) {			// see if status has been set by driver
      oz_sys_io_waitsetef (0);					// clear in case we wait again
      if ((sts = status) != OZ_PENDING) {			// maybe it completed just before clearing event flag
        oz_sys_io_waitsetef (1);				// set the flag in case this is an ast routine that interrupted 
        break;							// ... just as the main program was just about to wait
      }
      oz_sys_io_waitagain ();					// wait for event flag to be set by driver
								// ... or an ast to be delivered
    }
  }

  return (sts);
}

/* Start an I/O and if async completion, wait for completion or an ast delivery */

OZ_HW_SYSCALL_DEF_6 (io_wait, OZ_Procmode, procmode, OZ_Handle, h_iochan, volatile uLong *, status_r, uLong, funcode, uLong, as, void *, ap)

{
  int si;
  uLong sts;
  OZ_Event *event;
  OZ_Eventlist eventlist[1];
  OZ_Iochan *iochan;

  if (procmode < cprocmode) procmode = cprocmode;

  /* Inhibit softint delivery so we can't be aborted with refcounts out on the iochan */

  si = oz_hw_cpu_setsoftint (0);

  /* Lookup the i/o channel in the handle table.  Write access is required (we are going to be 'writing' I/O requests to the channel). */

  sts = oz_knl_handle_takeout (h_iochan, procmode, OZ_SECACCMSK_WRITE, OZ_OBJTYPE_IOCHAN, &iochan, NULL);
  if (sts != OZ_SUCCESS) goto rtn;

  /* Lookup the event flag in the handle table and clear it just before starting the I/O */

  event = oz_knl_thread_ioevent ();
  oz_knl_event_set (event, 0);

  /* Start the I/O request */

  sts = oz_knl_iostart3 (0, NULL, iochan, procmode, NULL, NULL, status_r, event, NULL, NULL, funcode, as, ap);

  /* We're done with I/O channel handle */

  oz_knl_handle_putback (h_iochan);

  /* Wait for I/O to complete or an ast to be delivered */

  if (sts == OZ_STARTED) {
    eventlist[0].event = event;					// async, wait for the I/O to complete
    oz_knl_event_waitlist (1, eventlist, cprocmode, si);	// ... or maybe an ast delivery or something
  }
  oz_knl_event_set (event, 1);

  /* Restore software interrupt delivery and return queuing status */

rtn:
  oz_hw_cpu_setsoftint (si);
  return (sts);
}

/* Wait for I/O or an ast delivery */

OZ_HW_SYSCALL_DEF_0 (io_waitagain)

{
  int si;
  OZ_Eventlist eventlist[1];

  si = oz_hw_cpu_setsoftint (0);				// keep us from being deleted
  eventlist[0].event = oz_knl_thread_ioevent ();		// get the thread's I/O event flag
  oz_knl_event_waitlist (1, eventlist, cprocmode, si);		// wait for the I/O to complete or an ast to be delivered
  oz_knl_event_set (eventlist[0].event, 1);			// set in case we're an ast interrupting an outer wait loop
  oz_hw_cpu_setsoftint (si);					// restore softint delivery
  return (OZ_SUCCESS);						// always successful
}

/* Set the I/O event flag to a particular value */

OZ_HW_SYSCALL_DEF_1 (io_waitsetef, Long, value)

{
  int si;
  OZ_Event *event;

  si = oz_hw_cpu_setsoftint (0);				// keep us from switching cpu's (as req'd by oz_knl_thread_ioevent)
  event = oz_knl_thread_ioevent ();				// get the thread's I/O event flag
  oz_knl_event_set (event, value);				// set it to requested value
  oz_hw_cpu_setsoftint (si);					// restore softint delivery
  return (OZ_SUCCESS);						// always successful
}

/************************************************************************/
/*									*/
/*  Start I/O request but do not wait for completion			*/
/*									*/
/*    Input:								*/
/*									*/
/*	procmode = processor mode to perform i/o for			*/
/*	h_iochan = i/o channel handle					*/
/*	status_r = where to return i/o completion status		*/
/*	h_event  = event flag handle to increment when complete		*/
/*	astentry = ast routine to call when complete			*/
/*	astparam = ast parameter					*/
/*	funcode  = i/o function code					*/
/*	as       = i/o argument block size				*/
/*	ap       = i/o argument block pointer				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_io_start = OZ_STARTED : requeust started			*/
/*	                    else : as returned by driver start routine	*/
/*	if oz_io_start == OZ_STARTED) {					*/
/*	  when request completes ...					*/
/*	  *status_r = filled in by driver				*/
/*	  h_event   = incremented by 1 when request completes		*/
/*	  astentry  = called iff oz_io == OZ_STARTED		*/
/*	}								*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_9 (io_start, OZ_Procmode, procmode, OZ_Handle, h_iochan, volatile uLong *, status_r, OZ_Handle, h_event, OZ_Astentry, astentry, void *, astparam, uLong, funcode, uLong, as, void *, ap)

{
  int si;
  uLong sts;
  OZ_Event *event;
  OZ_Iochan *iochan;

  if (procmode < cprocmode) procmode = cprocmode;

  /* Inhibit softint delivery so we can't be aborted with refcounts out on the iochan and event flag */

  si = oz_hw_cpu_setsoftint (0);

  /* Lookup the i/o channel in the handle table.  Write access is required (we are going to be 'writing' I/O requests to the channel). */

  sts = oz_knl_handle_takeout (h_iochan, procmode, OZ_SECACCMSK_WRITE, OZ_OBJTYPE_IOCHAN, &iochan, NULL);
  if (sts != OZ_SUCCESS) goto rtn;

  /* Lookup the event flag in the handle table */

  event = NULL;
  if (h_event != 0) {
    sts = oz_knl_handle_takeout (h_event, procmode, OZ_SECACCMSK_WRITE, OZ_OBJTYPE_EVENT, &event, NULL);
    if (sts != OZ_SUCCESS) goto rtn_iochan;
  }

  /* Start the I/O request */

  sts = oz_knl_iostart3 (0, NULL, iochan, procmode, NULL, NULL, status_r, event, astentry, astparam, funcode, as, ap);

  /* We're done with the handles */

  if (event != NULL) oz_knl_handle_putback (h_event);
rtn_iochan:
  oz_knl_handle_putback (h_iochan);
rtn:

  /* Restore interrupt delivery and return status */

  oz_hw_cpu_setsoftint (si);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Get device's unit name						*/
/*									*/
/*    Input:								*/
/*									*/
/*	h_iochan = i/o channel handle assigned to device		*/
/*	size     = size of buffer					*/
/*	buff     = address of buffer					*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_iochan_getunitname = OZ_SUCCESS : success		*/
/*	                                  else : error status		*/
/*	*buff = filled in with null terminated device unitname string	*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_3 (iochan_getunitname, OZ_Handle, h_iochan, uLong, size, char *, buff)

{
  int si;
  uLong sts;
  OZ_Devunit *devunit;
  OZ_Iochan *iochan;

  si = oz_hw_cpu_setsoftint (0);

  /* Lookup the i/o channel in the handle table */

  sts = oz_knl_handle_takeout (h_iochan, cprocmode, OZ_SECACCMSK_LOOK, OZ_OBJTYPE_IOCHAN, &iochan, NULL);

  /* Get unit name */

  if (sts == OZ_SUCCESS) {
    devunit = oz_knl_iochan_getdevunit (iochan);
    strncpyz (buff, oz_knl_devunit_devname (devunit), size);
    oz_knl_handle_putback (h_iochan);
  }

  oz_hw_cpu_setsoftint (si);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Get device's driver class name					*/
/*									*/
/*    Input:								*/
/*									*/
/*	h_iochan = i/o channel handle assigned to device		*/
/*	size     = size of buffer					*/
/*	buff     = address of buffer					*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_iochan_getclassname = OZ_SUCCESS : success		*/
/*	                                   else : error status		*/
/*	*buff = filled in with null terminated driver classname string	*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_3 (iochan_getclassname, OZ_Handle, h_iochan, uLong, size, char *, buff)

{
  int si;
  uLong sts;
  OZ_Devunit *devunit;
  OZ_Iochan *iochan;

  si = oz_hw_cpu_setsoftint (0);

  /* Lookup the i/o channel in the handle table */

  sts = oz_knl_handle_takeout (h_iochan, cprocmode, OZ_SECACCMSK_LOOK, OZ_OBJTYPE_IOCHAN, &iochan, NULL);

  /* Get class name */

  if (sts == OZ_SUCCESS) {
    devunit = oz_knl_iochan_getdevunit (iochan);
    strncpyz (buff, oz_knl_devunit_classname (devunit), size);
    oz_knl_handle_putback (h_iochan);
  }

rtn:
  oz_hw_cpu_setsoftint (si);
  return (sts);
}

uLong oz_syscall_iosel_start (OZ_Procmode cprocmode, OZ_Procmode procmode, uLong numchans, OZ_Handle *h_iochans, uLong *selcodes, uLong *senses,
                              OZ_Handle h_event, OZ_Astentry astentry, void *astparam)

{
  return (OZ_DEVNOTSELECTABLE);
}
