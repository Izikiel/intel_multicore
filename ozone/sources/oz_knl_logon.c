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

/************************************************************************/
/*									*/
/*  This routine is called by a console driver to start the logon 	*/
/*  process going							*/
/*									*/
/*    Input:								*/
/*									*/
/*	smplock = softint level						*/
/*									*/
/*    Output:								*/
/*									*/
/*	logon process started						*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_knl_devio.h"
#include "oz_knl_handle.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_logon.h"
#include "oz_knl_sdata.h"
#include "oz_knl_status.h"
#include "oz_knl_thread.h"
#include "oz_sys_spawn.h"
#include "oz_sys_thread.h"

typedef struct { OZ_Iochan *iochan;
                 OZ_Logname *logname;
                 uLong nvalues;
                 const OZ_Logvalue *values;
               } Lpb;

static const char logon_logical[] = "OZ_LOGON_IMAGE";

static uLong logon_thread (void *lpbv);

/* This entrypoint takes the console's devunit as the parameter and assigns a channel of its own */

void oz_knl_logon_devunit (OZ_Devunit *devunit)

{
  char unitname[OZ_DEVUNIT_NAMESIZE];
  uLong sts;
  OZ_Iochan *iochan;

  sts = oz_knl_iochan_create (devunit, OZ_LOCKMODE_EX, OZ_PROCMODE_USR, NULL, &iochan);
  if (sts == OZ_SUCCESS) {
    oz_knl_logon_iochan (iochan);
    oz_knl_iochan_increfc (iochan, -1);
  } else {
    oz_knl_printk ("oz_knl_logon_devunit: error %u assigning channel to %s\n", sts, oz_knl_devunit_devname (devunit));
  }
}

/* This entrypoint takes an I/O channel assigned in OZ_LOCKMODE_EX (for security purposes only, it could actually be just OZ_LOCKMODE_CW) */
/* Note:  This routine incs the iochan ref count as needed.  Caller must dec the ref count when done with channel.                        */

void oz_knl_logon_iochan (OZ_Iochan *iochan)

{
  char threadname[6+OZ_DEVUNIT_NAMESIZE];
  const OZ_Logvalue *values;
  Lpb *lpb;
  OZ_Devunit *devunit;
  OZ_Logname *logname;
  OZ_Thread *thread;
  uLong nvalues, priority, sts;

  /* Make string "logon <unitname>" */

  strcpy (threadname + 0, "logon ");
  devunit = oz_knl_iochan_getdevunit (iochan);
  strncpyz (threadname + 6, oz_knl_devunit_devname (devunit), sizeof threadname - 6);

  /* Make sure OZ_LOGON_IMAGE logical is defined in the system table, if not, don't bother doing anything */
  /* This way, we don't try to start anything until the startup process is complete                       */

  sts = oz_knl_logname_lookup (oz_s_systemtable, OZ_PROCMODE_KNL, sizeof logon_logical, logon_logical, 
                               NULL, NULL, &nvalues, &values, &logname, NULL);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_knl_logon: error %u looking up kernel mode logical %s in system table for %s\n", sts, logon_logical, threadname + 6);
    return;
  }

  /* Increment channel ref count, we decrement it after an handle is assigned */
  /* This prevents it from being deleted until we are done with it            */

  oz_knl_iochan_increfc (iochan, 1);

  /* Print message saying we're starting up */

  oz_knl_printk ("oz_knl_logon: creating thread for %s\n", threadname + 6);

  /* Create a thread in the system process that runs in kernel mode */

  lpb = OZ_KNL_NPPMALLOC (sizeof *lpb);
  lpb -> iochan  = iochan;
  lpb -> logname = logname;
  lpb -> nvalues = nvalues;
  lpb -> values  = values;

  priority = oz_knl_user_getmaxbasepri (oz_s_systemuser);
  sts = oz_knl_thread_create (oz_s_systemproc, priority, NULL, NULL, NULL, 0, logon_thread, lpb, OZ_ASTMODE_INHIBIT, strlen (threadname), threadname, NULL, &thread);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_knl_logon: error %u creating thread for %s\n", sts, threadname + 6);
    oz_knl_iochan_increfc (iochan, -1);
    oz_knl_logname_increfc (logname, -1);
    OZ_KNL_NPPFREE (lpb);
  }

  /* We don't do anything with the thread pointer so release it */

  else oz_knl_thread_increfc (thread, -1);
}

/* This thread executes in kernel mode as part of the system process */

static uLong logon_thread (void *lpbv)

{
  const char **argv, *unitname;
  int argc;
  Lpb *lpb;
  uLong sts;
  OZ_Devunit *devunit;
  OZ_Handle h_iochan, h_thread;
  OZ_Iochan *iochan;

  lpb = lpbv;

  oz_hw_cpu_setsoftint (0);	// inhib softints as required by many routines below

  /* Get the console device unit name for error messages */

  devunit  = oz_knl_iochan_getdevunit (lpb -> iochan);
  unitname = oz_knl_devunit_devname (devunit);

  /* Try to assign an handle to the console device */

  sts = oz_knl_handle_assign (lpb -> iochan, OZ_PROCMODE_KNL, &h_iochan);

  /* If couldn't assign handle, print error message and exit */

  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_knl_logon: error %u assigning handle to %s\n", sts, unitname);
    goto rtnsts;
  }

  /* Handle assigned, spawn a process (in the current job) that runs the logon image */

  argv = OZ_KNL_PGPMALLOC (lpb -> nvalues * sizeof argv[0]);
  for (argc = 0; argc < lpb -> nvalues; argc ++) {
    argv[argc] = lpb -> values[argc].buff;
  }
  sts = oz_sys_spawn (0, argv[0], h_iochan, h_iochan, h_iochan, 0, 0, NULL, 
                      argc, argv, oz_knl_thread_getname (oz_knl_thread_getcur ()), &h_thread, NULL);
  OZ_KNL_PGPFREE (argv);

  /* Release the console handle */

  oz_knl_handle_release (h_iochan, OZ_PROCMODE_KNL);

  /* If failed to spawn, output error message */

  if (sts != OZ_SUCCESS) oz_knl_printk ("oz_knl_logon: error %u spawning logon process for %s\n", sts, unitname);

  /* Otherwise, orphan the spawned thread or it'll get killed when this thread exits */

  else {
    oz_sys_thread_orphan (h_thread);
    oz_knl_handle_release (h_thread, OZ_PROCMODE_KNL);
  }

  /* Return completion status (as if anyone cared) and exit */

rtnsts:
  oz_knl_iochan_increfc (lpb -> iochan, -1);
  oz_knl_logname_increfc (lpb -> logname, -1);
  OZ_KNL_NPPFREE (lpb);
  oz_hw_cpu_setsoftint (1);
  return (sts);
}
