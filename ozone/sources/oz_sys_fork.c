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
/*  This routine creates a new process and thread that is a duplicate 	*/
/*  of the caller.							*/
/*									*/
/*    Input:								*/
/*									*/
/*	priority = 0 : new thread has same priority as calling thread	*/
/*	        else : run new thread at the given priority		*/
/*	h_initevent = 0 : new thread will not wait on startup		*/
/*	           else : new thread waits for this event flag 		*/
/*	h_exitevent = 0 : new thread won't set any event flag on exit	*/
/*	           else : new thread sets the event flag on exit	*/
/*	name = what to name the new process and thread			*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_fork = OZ_SUCCESS : this is the original process/thread	*/
/*	           OZ_SUBPROCESS : this is the new forked process/thread
/*	                    else : error status				*/
/*	*h_thread_r = handle to new thread (original thread only)	*/
/*	              0 (new forked thread)				*/
/*									*/
/*    Note:								*/
/*									*/
/*	This routine must be called only from user mode.  This is 	*/
/*	because the setjmp would save the kernel stack context, then 	*/
/*	the longjmp, being in user mode, would try to jump to it and 	*/
/*	would barf.							*/
/*									*/
/*	If there is more than one thread executing when this routine 	*/
/*	is called, their stack contents are copied to the new process 	*/
/*	but the new process only gets a copy of the calling thread.	*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_knl_hw.h"
#include "oz_knl_status.h"
#include "oz_sys_fork.h"
#include "oz_sys_process.h"
#include "oz_sys_thread.h"

static uLong forked_thread (void *jmpbufv);

uLong oz_sys_fork (uLong priority, OZ_Handle h_initevent, OZ_Handle h_exitevent, const char *name, OZ_Handle *h_thread_r)

{
  OZ_Handle h_process, h_thread;
  OZ_Hw_jmpbuf jmpbuf;
  uLong sts;

  if (h_thread_r != NULL) *h_thread_r = 0;

  /* Save current stack context.  If it returns zero, it means it is the original thread  */
  /* returning from the setjmp call.  Otherwise, it is the new thread in the new process. */

  if (setjmp (&jmpbuf)) {
    oz_sys_thread_newusrstk ();		/* unmap the new thread's old stack, set up new thread's new stack to be unmapped when thread exits */
    return (OZ_SUBPROCESS);		/* return to caller saying this is the subprocess thread */
  }

  /* Make a copy of the current process (copy pagetable(s), logical names, handles, etc) */

  sts = oz_sys_process_makecopy (name, &h_process);
  if (sts != OZ_SUCCESS) return (sts);

  /* Create a thread in the new process.  It starts in 'forked_thread' which 'longjmps' back to the 'setjmp' call. */

  sts = oz_sys_thread_create (OZ_PROCMODE_KNL, h_process, priority, h_initevent, h_exitevent, 
                              0, forked_thread, &jmpbuf, OZ_ASTMODE_ENABLE, name, &h_thread);
  oz_sys_handle_release (OZ_PROCMODE_KNL, h_process);

  /* Maybe return handle to new thread */

  if (sts == OZ_SUCCESS) {
    if (h_thread_r != NULL) *h_thread_r = h_thread;
    else oz_sys_handle_release (OZ_PROCMODE_KNL, h_thread);
  }
  return (sts);
}

/* This routine executes in the new process and thread */

static uLong forked_thread (void *jmpbufv)

{
  longjmp (jmpbufv, 1);		/* restore to old stack */
  return (OZ_SUCCESS);		/* never get here, but keep the compiler happy */
}
