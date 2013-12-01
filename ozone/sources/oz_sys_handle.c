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
/*  User callable handle routines					*/
/*									*/
/************************************************************************/

#include "oz_sys_syscall.h"

#include "oz_sys_handle.h"

#include "oz_knl_handle.h"
#include "oz_knl_procmode.h"
#include "oz_knl_status.h"

/************************************************************************/
/*									*/
/*  Get next handle in process						*/
/*									*/
/*    Input:								*/
/*									*/
/*	procmode = processor mode					*/
/*	handle = last handle (or 0 to start at beginning)		*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_handle_next = OZ_SUCCESS : successful			*/
/*	                           else : error status			*/
/*	*handle_r = new handle or 0 if at end of list			*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_3 (handle_next, OZ_Procmode, procmode, OZ_Handle, handle, OZ_Handle *, handle_r)

{
  if (procmode < cprocmode) procmode = cprocmode;		/* maximise processor mode */
  *handle_r = oz_knl_handle_next (handle, procmode);		/* get next handle at that or outer mode */
  return (OZ_SUCCESS);						/* return success status */
}

/************************************************************************/

OZ_HW_SYSCALL_DEF_3 (handle_setthread, OZ_Procmode, procmode, OZ_Handle, h, OZ_Handle, h_thread)

{
  int si;
  OZ_Thread *thread;
  uLong sts;

  if (procmode < cprocmode) procmode = cprocmode;		/* maximise processor mode */
  thread = NULL;						/* assume h_thread is zero */
  si = oz_hw_cpu_setsoftint (0);				/* keep from being aborted */
  if (h_thread != 0) {
    sts = oz_knl_handle_takeout (h_thread, procmode, OZ_SECACCMSK_LOOK, OZ_OBJTYPE_THREAD, &thread, NULL); /* lookup thread */
    if (sts != OZ_SUCCESS) goto rtnsts;
    sts = OZ_THREADNOTINPROC;					/* make sure it is in our process */
    if (oz_knl_thread_getprocess (thread) != oz_knl_thread_getprocesscur ()) goto rtnststh;
  }
  sts = oz_knl_handle_setthread (h, procmode, thread);		/* set handle's thread to the given value */
rtnststh:
  if (thread != NULL) oz_knl_handle_putback (h_thread);		/* release thread pointer */
rtnsts:
  oz_hw_cpu_setsoftint (si);					/* restore softints */
  return (sts);							/* return final status */
}

/************************************************************************/
/*									*/
/*  Release (close) an handle						*/
/*									*/
/*    Input:								*/
/*									*/
/*	procmode = processor mode					*/
/*	h = handle to release						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_handle_release = OZ_SUCCESS : successful			*/
/*	                              else : error status		*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_2 (handle_release, OZ_Procmode, procmode, OZ_Handle, h)

{
  int si;
  uLong sts;

  if (procmode < cprocmode) procmode = cprocmode;	/* maximise processor mode */
  si = oz_hw_cpu_setsoftint (0);
  sts = oz_knl_handle_release (h, procmode);		/* release handle */
  oz_hw_cpu_setsoftint (si);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Retrieve an handle from another process				*/
/*									*/
/*    Input:								*/
/*									*/
/*	h_remote  = remote handle to retrieve				*/
/*	h_process = process to retrieve handle from			*/
/*	secaccmsk = access required of handle in remote process		*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_handle_retrieve = OZ_SUCCESS : successful		*/
/*	                               else : error			*/
/*	*h_local_r = local handle pointing to same object		*/
/*									*/
/*    Note:								*/
/*									*/
/*	Caller must have write access to the target process.		*/
/*									*/
/*	Local handle is created with access as the caller is able to 	*/
/*	get on the object.  Use secaccmsk bits to check to see if 	*/
/*	remote process had the required access.				*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_4 (handle_retrieve, OZ_Handle, h_remote, OZ_Handle, h_process, uLong, secaccmsk, OZ_Handle *, h_local_r)

{
  int si;
  OZ_Handle h_local;
  OZ_Process *process, *saveprocess;
  uLong sts;
  void *object;

  object = NULL;
  si = oz_hw_cpu_setsoftint (0);
  sts = oz_knl_handle_takeout (h_process, cprocmode, OZ_SECACCMSK_WRITE, 		// get target process pointer
                               OZ_OBJTYPE_PROCESS, &process, NULL);			// (must have write access)
  if (sts == OZ_SUCCESS) {
    saveprocess = oz_knl_process_getcur ();						// save my process
    oz_knl_process_setcur (process);							// address target process
    sts = oz_knl_handle_takeout (h_remote, cprocmode, secaccmsk, OZ_OBJTYPE_UNKNOWN, &object, NULL); // get handle in target context
    if (sts == OZ_SUCCESS) {
      oz_knl_handle_objincrefc (OZ_KNL_GETOBJTYPE (object), object, 1);			// inc object's refcount so it can't die
      oz_knl_handle_putback (h_remote);							// release target handle
    }
    oz_knl_process_setcur (saveprocess);						// restore my process
    oz_knl_handle_putback (h_process);							// release target process handle
    if (sts == OZ_SUCCESS) {
      sts = oz_knl_handle_assign (object, cprocmode, &h_local);				// assign equivalent handle in my process
      oz_knl_handle_objincrefc (OZ_KNL_GETOBJTYPE (object), object, -1);		// release object pointer
      if (sts == OZ_SUCCESS) {
        sts = oz_knl_section_uput (cprocmode, sizeof *h_local_r, &h_local, h_local_r);	// return local handle to caller
        if (sts != OZ_SUCCESS) oz_knl_handle_release (h_local, cprocmode);		// failed, release local handle
      }
    }
  }
  oz_hw_cpu_setsoftint (si);
  return (sts);
}
