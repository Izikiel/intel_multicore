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
/*  User mode callable condition handler routines			*/
/*									*/
/************************************************************************/

#include "ozone.h"

#include "oz_knl_hw.h"
#include "oz_knl_process.h"
#include "oz_knl_status.h"
#include "oz_knl_thread.h"
#include "oz_knl_userjob.h"
#include "oz_sys_condhand.h"
#include "oz_sys_handle_getinfo.h"
#include "oz_sys_io_fs_printf.h"
#include "oz_sys_syscall.h"
#include "oz_sys_thread.h"

#include <stdarg.h>

static void traceback (void *dummy, OZ_Mchargs *mchargs);
static uLong traceprint (void *param, const char *format, ...);

/************************************************************************/
/*									*/
/*  This routine is called by the kernel when no suitable condition 	*/
/*  handler is found.  It prints out the sigargs, mchargs and a stack 	*/
/*  trace, then exits with status = sigargs[1].				*/
/*									*/
/*    Input:								*/
/*									*/
/*	sigargs = signal argument list pointer				*/
/*	mchargs = machine argument list pointer				*/
/*									*/
/************************************************************************/

void oz_sys_condhand_default (OZ_Sigargs sigargs[], OZ_Mchargs *mchargs)

{
  char jobname[OZ_JOBNAME_MAX], processname[OZ_PROCESS_NAMESIZE], threadname[OZ_THREAD_NAMESIZE], username[OZ_USERNAME_MAX];
  int i;
  uLong sts;
  OZ_Handle_item itemlist[6];
  OZ_Processid processid;
  OZ_Threadid threadid;

  itemlist[0].code = OZ_HANDLE_CODE_THREAD_ID;
  itemlist[0].size = sizeof threadid;
  itemlist[0].buff = &threadid;
  itemlist[0].rlen = NULL;
  itemlist[1].code = OZ_HANDLE_CODE_THREAD_NAME;
  itemlist[1].size = sizeof threadname;
  itemlist[1].buff = threadname;
  itemlist[1].rlen = NULL;
  itemlist[2].code = OZ_HANDLE_CODE_PROCESS_ID;
  itemlist[2].size = sizeof processid;
  itemlist[2].buff = &processid;
  itemlist[2].rlen = NULL;
  itemlist[3].code = OZ_HANDLE_CODE_PROCESS_NAME;
  itemlist[3].size = sizeof processname;
  itemlist[3].buff = processname;
  itemlist[3].rlen = NULL;
  itemlist[4].code = OZ_HANDLE_CODE_JOB_NAME;
  itemlist[4].size = sizeof jobname;
  itemlist[4].buff = jobname;
  itemlist[4].rlen = NULL;
  itemlist[5].code = OZ_HANDLE_CODE_USER_NAME;
  itemlist[5].size = sizeof username;
  itemlist[5].buff = username;
  itemlist[5].rlen = NULL;
  sts = oz_sys_handle_getinfo (0, 6, itemlist, NULL);
  if (sts != OZ_SUCCESS) {
    oz_sys_sprintf (sizeof threadname, threadname, "<status %u>", sts);
    processname[0] = 0;
    jobname[0]     = 0;
    username[0]    = 0;
  }
  oz_sys_io_fs_printerror ("oz_sys_condhand_default: unhandled condition thread %u %s\n", threadid, threadname);
  oz_sys_io_fs_printerror ("                                            process %u %s\n", processid, processname);
  oz_sys_io_fs_printerror ("                                                job %s\n", jobname);
  oz_sys_io_fs_printerror ("                                               user %s\n", username);
  oz_sys_io_fs_printerror ("oz_sys_condhand_default:   sigargs:");
  for (i = 0; i <= sigargs[0]; i ++) oz_sys_io_fs_printerror (" %8.8X", sigargs[i]);
  oz_sys_io_fs_printerror ("\n");
  i = 1;
  oz_hw_traceback (traceback, &i, -1, mchargs, NULL);
  oz_sys_thread_exit (sigargs[1]);
}

static void traceback (void *dummy, OZ_Mchargs *mchargs)

{
  uLong sts;

  oz_hw_mchargs_print (traceprint, NULL, *(int *)dummy, mchargs);
  *(int *)dummy = 0;
}

static uLong traceprint (void *param, const char *format, ...)

{
  va_list ap;

  va_start (ap, format);
  oz_sys_io_fs_printerrorv (format, ap);
  va_end (ap);
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  This condition handler can be declared by any routine that wants 	*/
/*  any signals encountered therein to be returned as the value of the 	*/
/*  function								*/
/*									*/
/************************************************************************/

uLong oz_sys_condhand_rtnanysig (void *chparam, OZ_Sigargs sigargs[], OZ_Mchargs *mchargs)

{
  return (sigargs[1]);
}
