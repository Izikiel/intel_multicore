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
/*  TOP command								*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_io_console.h"
#include "oz_knl_process.h"
#include "oz_knl_status.h"
#include "oz_knl_thread.h"
#include "oz_knl_userjob.h"
#include "oz_sys_dateconv.h"
#include "oz_sys_event.h"
#include "oz_sys_handle_getinfo.h"
#include "oz_sys_xprintf.h"
#include "oz_util_start.h"

#define MAXTHREADS 1000

typedef struct Threadata Threadata;

struct Threadata { OZ_Threadid threadid;
                   uLong curprio;
                   OZ_Datebin cputime, deltacpu;
                   char username[OZ_USERNAME_MAX];
                   char jobname[OZ_JOBNAME_MAX];
                   char processname[OZ_PROCESS_NAMESIZE];
                   char threadname[OZ_THREAD_NAMESIZE];
                 };

void qsort ();
void *bsearch ();

static char *pn = "top";

static int by_threadid (const void *v1, const void *v2);
static int by_deltacpu (const void *v1, const void *v2);
static uLong rawrite (void *dummy, uLong *size, char **buff);

uLong oz_util_main (int argc, char *argv[])

{
  char rawbuff[256];
  int i, index, jobname_w, newcount, oldcount, processname_w, threadname_w, username_w;
  Long oldevent;
  OZ_IO_console_ctrlchar console_ctrlchar;
  OZ_Datebin cpupercent, elapsedtime, newtime, oldtime;
  OZ_Handle h_event;
  OZ_Handle h_nextjob, h_nextprocess, h_nextthread, h_nextuser;
  OZ_Handle h_thisjob, h_thisprocess, h_thisthread, h_thisuser;
  OZ_Threadid threadid;
  Threadata githread, *newthreads, *oldthread, *oldthreads, ta1[MAXTHREADS], ta2[MAXTHREADS];
  uLong deltacpupercent, sts;
  volatile uLong aborted;

  OZ_Handle_item userfirst = { OZ_HANDLE_CODE_USER_FIRST, sizeof h_nextuser, &h_nextuser, NULL };
  OZ_Handle_item useritems[] = { OZ_HANDLE_CODE_USER_NEXT, sizeof h_nextuser, &h_nextuser, NULL, 
                                 OZ_HANDLE_CODE_USER_NAME, sizeof githread.username, githread.username, NULL };

  OZ_Handle_item jobfirst = { OZ_HANDLE_CODE_JOB_FIRST, sizeof h_nextjob, &h_nextjob, NULL };
  OZ_Handle_item jobitems[] = { OZ_HANDLE_CODE_JOB_NEXT, sizeof h_nextjob, &h_nextjob, NULL, 
                                OZ_HANDLE_CODE_JOB_NAME, sizeof githread.jobname, githread.jobname, NULL };

  OZ_Handle_item processfirst = { OZ_HANDLE_CODE_PROCESS_FIRST, sizeof h_nextprocess, &h_nextprocess, NULL };
  OZ_Handle_item processitems[] = { OZ_HANDLE_CODE_PROCESS_NEXT, sizeof h_nextprocess, &h_nextprocess, NULL, 
                                    OZ_HANDLE_CODE_PROCESS_NAME, sizeof githread.processname, githread.processname, NULL };

  OZ_Handle_item threadfirst = { OZ_HANDLE_CODE_THREAD_FIRST, sizeof h_nextthread, &h_nextthread, NULL };
  OZ_Handle_item threaditems[] = { OZ_HANDLE_CODE_THREAD_NEXT,    sizeof h_nextthread,       &h_nextthread,        NULL, 
                                   OZ_HANDLE_CODE_THREAD_ID,      sizeof githread.threadid,  &githread.threadid,   NULL, 
                                   OZ_HANDLE_CODE_THREAD_NAME,    sizeof githread.threadname, githread.threadname, NULL, 
                                   OZ_HANDLE_CODE_THREAD_CURPRIO, sizeof githread.curprio,   &githread.curprio,    NULL, 
                                   OZ_HANDLE_CODE_THREAD_TIS_RUN, sizeof githread.cputime,   &githread.cputime,    NULL };

  if (argc > 0) pn = argv[0];

  oldcount = 0;
  oldtime  = 0;

  oldthreads = ta1;
  newthreads = ta2;

  sts = oz_sys_event_create (OZ_PROCMODE_KNL, "loop timer", &h_event);
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
  newtime = oz_hw_tod_getnow ();
  sts = oz_sys_event_setimint (h_event, (OZ_Datebin)OZ_TIMER_RESOLUTION, newtime);
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);

  aborted = OZ_PENDING;
  memset (&console_ctrlchar, 0, sizeof console_ctrlchar);
  console_ctrlchar.mask[0]  = (1 << ('C' - '@')) | (1 << ('Z' - '@'));
  console_ctrlchar.terminal = 1;
  sts = oz_sys_io_start (OZ_PROCMODE_KNL, oz_util_h_console, &aborted, 0, NULL, NULL, OZ_IO_CONSOLE_CTRLCHAR, sizeof console_ctrlchar, &console_ctrlchar);
  if (sts != OZ_STARTED) oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u enabling control char abort\n", pn, sts);
  else oz_sys_io_fs_printf (oz_util_h_console, "%s: use control-C or control-Z to exit\n", pn);

loop:
  if (aborted != OZ_PENDING) return (OZ_SUCCESS);
  newtime = oz_hw_tod_getnow ();

  /* Get list of all threads in 'newthreads' */

  newcount      = 0;
  username_w    = 4;	/* width of string "User" */
  jobname_w     = 3;	/* width of string "Job" */
  processname_w = 7;	/* width of string "Process" */
  threadname_w  = 6;	/* width of string "Thread" */
  sts = oz_sys_handle_getinfo (0, 1, &userfirst, &index);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u getting first user\n", pn, sts);
    return (sts);
  }
  while ((h_thisuser = h_nextuser) != 0) {
    sts = oz_sys_handle_getinfo (h_thisuser, sizeof useritems / sizeof useritems[0], useritems, &index);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u getting useritems[%d]\n", pn, sts, index);
      return (sts);
    }
    i = strlen (githread.username);
    if (username_w < i) username_w = i;
    sts = oz_sys_handle_getinfo (h_thisuser, 1, &jobfirst, &index);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u getting first job, user %s\n", pn, sts, githread.username);
      return (sts);
    }
    while ((h_thisjob = h_nextjob) != 0) {
      sts = oz_sys_handle_getinfo (h_thisjob, sizeof jobitems / sizeof jobitems[0], jobitems, &index);
      if (sts != OZ_SUCCESS) {
        oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u getting jobitems[%d]\n", pn, sts, index);
        return (sts);
      }
      i = strlen (githread.jobname);
      if (jobname_w < i) jobname_w = i;
      sts = oz_sys_handle_getinfo (h_thisjob, 1, &processfirst, &index);
      if (sts != OZ_SUCCESS) {
        oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u getting first process, user %s, job %s\n", pn, sts, githread.username, githread.jobname);
        return (sts);
      }
      while ((h_thisprocess = h_nextprocess) != 0) {
        sts = oz_sys_handle_getinfo (h_thisprocess, sizeof processitems / sizeof processitems[0], processitems, &index);
        if (sts != OZ_SUCCESS) {
          oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u getting processitems[%d]\n", pn, sts, index);
          return (sts);
        }
        i = strlen (githread.processname);
        if (processname_w < i) processname_w = i;
        sts = oz_sys_handle_getinfo (h_thisprocess, 1, &threadfirst, &index);
        if (sts != OZ_SUCCESS) {
          oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u getting first thread, user %s, job %s, process %s\n", pn, sts, githread.username, githread.jobname, githread.processname);
          return (sts);
        }
        while ((h_thisthread = h_nextthread) != 0) {
          sts = oz_sys_handle_getinfo (h_thisthread, sizeof threaditems / sizeof threaditems[0], threaditems, &index);
          if (sts != OZ_SUCCESS) {
            oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u getting threaditmes[%d]\n", pn, sts, index);
            return (sts);
          }
          i = strlen (githread.threadname);
          if (threadname_w < i) threadname_w = i;
          newthreads[newcount++] = githread;
          oz_sys_handle_release (OZ_PROCMODE_KNL, h_thisthread);
          if (newcount == MAXTHREADS) break;
        }
        oz_sys_handle_release (OZ_PROCMODE_KNL, h_thisprocess);
        if (newcount == MAXTHREADS) break;
      }
      oz_sys_handle_release (OZ_PROCMODE_KNL, h_thisjob);
      if (newcount == MAXTHREADS) break;
    }
    oz_sys_handle_release (OZ_PROCMODE_KNL, h_thisuser);
    if (newcount == MAXTHREADS) break;
  }

  /* If this is the first scan, go wait out the interval before displaying anything */

  if (oldtime == 0) goto wait;

  /* Calculate elapsed cputime during the interval by subtracting previous values */

  if (oldcount > 2) qsort (oldthreads, oldcount, sizeof oldthreads[0], by_threadid);
  for (i = 0; i < newcount; i ++) {
    newthreads[i].deltacpu = (OZ_Datebin)(-1);
    if (oldcount > 0) {
      oldthread = bsearch (newthreads + i, oldthreads, oldcount, sizeof oldthreads[0], by_threadid);
      if (oldthread != NULL) newthreads[i].deltacpu = newthreads[i].cputime - oldthread -> cputime;
    }
  }

  /* Now sort by elapsed cpu time to get those with the most cpu time */

  qsort (newthreads, newcount, sizeof newthreads[0], by_deltacpu);

  /* Display them */

  elapsedtime = newtime - oldtime;
  oz_sys_xprintf (rawrite, NULL, sizeof rawbuff, rawbuff, NULL, "[H");
  oz_sys_xprintf (rawrite, NULL, sizeof rawbuff, rawbuff, NULL, "%8s  %8s   %%  %8s  %-*s  %-*s[K\r\n", "Threadid", "Cpu Time", "Priority", 
      username_w, "User", threadname_w, "Thread");
  for (i = 0; (i < 22) && (i < newcount); i ++) {
    if (newthreads[i].deltacpu == (OZ_Datebin)(-1)) continue;
    deltacpupercent = newthreads[i].deltacpu * 100 / elapsedtime;
    oz_sys_xprintf (rawrite, NULL, sizeof rawbuff, rawbuff, NULL, "%8u  %8.6#t %3u  %8u  %-*s  %-*s[K\r\n", 
	newthreads[i].threadid, newthreads[i].deltacpu, deltacpupercent, 
	newthreads[i].curprio, username_w, newthreads[i].username, threadname_w, newthreads[i].threadname);
  }
  oz_sys_xprintf (rawrite, NULL, sizeof rawbuff, rawbuff, NULL, "[J");

  /* Now wait for a second before doing it again */

wait:
  do {
    oz_sys_event_wait (OZ_PROCMODE_KNL, h_event, 0);
    oz_sys_event_set (OZ_PROCMODE_KNL, h_event, 0, &oldevent);
  } while (oldevent == 0);
  oldtime  = newtime;
  oldcount = newcount;
  oldthreads = ta1 - oldthreads + ta2;
  newthreads = ta2 - newthreads + ta1;
  goto loop;
}

static int by_threadid (const void *v1, const void *v2)

{
  if (((Threadata *)v1) -> threadid < ((Threadata *)v2) -> threadid) return (-1);
  if (((Threadata *)v1) -> threadid > ((Threadata *)v2) -> threadid) return (1);
  return (0);
}

static int by_deltacpu (const void *v1, const void *v2)

{
  if (((Threadata *)v1) -> deltacpu > ((Threadata *)v2) -> deltacpu) return (-1);
  if (((Threadata *)v1) -> deltacpu < ((Threadata *)v2) -> deltacpu) return (1);
  return (0);
}

static uLong rawrite (void *dummy, uLong *size, char **buff)

{
  uLong sts;
  OZ_IO_console_putdat console_putdat;

  memset (&console_putdat, 0, sizeof console_putdat);
  console_putdat.size = *size;
  console_putdat.buff = *buff;
  sts = oz_sys_io (OZ_PROCMODE_KNL, oz_util_h_console, 0, OZ_IO_CONSOLE_PUTDAT, sizeof console_putdat, &console_putdat);
  return (sts);
}
