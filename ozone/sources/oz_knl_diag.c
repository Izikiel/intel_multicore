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
/*  Diag module - invoked by pressing control-shift-D			*/
/*									*/
/************************************************************************/

#include "ozone.h"

#include "oz_knl_boot.h"
#include "oz_knl_debug.h"
#include "oz_knl_devio.h"
#include "oz_knl_event.h"
#include "oz_knl_hw.h"
#include "oz_knl_image.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_lock.h"
#include "oz_knl_logname.h"
#include "oz_knl_logon.h"
#include "oz_knl_malloc.h"
#include "oz_knl_printk.h"
#include "oz_knl_process.h"
#include "oz_knl_procmode.h"
#include "oz_knl_sdata.h"
#include "oz_knl_status.h"
#include "oz_knl_thread.h"

#include "oz_io_console.h"
#include "oz_io_fs.h"
#include "oz_io_timer.h"

#include "oz_sys_callknl.h"
#include "oz_sys_condhand.h"
#include "oz_sys_dateconv.h"
#include "oz_sys_image.h"

#include "oz_usr_debug.h"

#define USER_STACK_SIZE (1024*1024 >> OZ_HW_L2PAGESIZE)

typedef struct { uLong addr, data;
                 int io, sz;
               } Pp;

static volatile int halted = 0;
static Long donthaltme = -1;
static char imagename[64];

static void softint_level (OZ_Mchargs *mchargs);
static uLong thread_level (void *dummy);
static OZ_Thread *assignthread (int consiz, char *conbuf);
static uLong ztoul (char *b, char **p);
static uLong peek_try (void *ppv);
static uLong poke_try (void *ppv);
static void readcons (int size, char *buff, char *prompt);
static OZ_Iochan *openfile (char *filename, OZ_Lockmode lockmode, int verbose);
static OZ_Iochan *createfile (char *filename, uLong filattrflags);
static uLong thread_init (void *dummy);

/************************************************************************/
/*									*/
/*  This routine is called by the hardware layer at softint level on 	*/
/*  all cpu's as a result of a control-shift-D				*/
/*									*/
/*    Input:								*/
/*									*/
/*	cpuidx = current cpu index number				*/
/*	first  = 1 : this is the first cpu to respond			*/
/*	         0 : this isn't the first cpu to respond		*/
/*	mchargs = point of interrupt					*/
/*									*/
/************************************************************************/

void oz_knl_diag (Long cpuidx, int first, OZ_Mchargs *mchargs)

{
  uLong ts;

  /* If called because we're shutting down, print message and hang forever */

  if (oz_s_shutdown >= 0) {
    if (cpuidx == oz_s_shutdown) return;			/* don't shutdown the cpu that's doing the shutdown */
    oz_knl_halt ();						/* halt */
  }

  /* Set the 'halted' flag no matter which cpu we are         */
  /* This will cause the 'while halted' loop to hang for sure */

  halted = 1;
  OZ_HW_MB;

  /* This is the first cpu to get here, use it for processing        */
  /* Clear the halted flag when done so as to release the other cpus */

  if (first) {
    donthaltme = cpuidx;
    oz_knl_printk ("oz_knl_diag: activated on cpu %d\n", cpuidx);
    softint_level (mchargs);
    donthaltme = -1;
    halted = 0;
    oz_knl_printk ("oz_knl_diag: cpu %d resuming\n", cpuidx);
  }

  /* If this isn't the first cpu to come here, just  */
  /* wait for the first cpu to clear the halted flag */

  else if (cpuidx != donthaltme) {
    oz_knl_printk ("oz_knl_diag: cpu %d halted\n", cpuidx);
    while (halted) {}
    oz_knl_printk ("oz_knl_diag: cpu %d resuming\n", cpuidx);
  }
}

/************************************************************************/
/*									*/
/*  This is the main processing routine - it runs on the first cpu to 	*/
/*  respond at softint level						*/
/*									*/
/************************************************************************/

static void softint_level (OZ_Mchargs *mchargs)

{
  char conbuf[64], *p;
  int hwi, i, usedup;
  OZ_Datebin now;
  OZ_Devunit *devunit;
  OZ_Thread *thread;
  uLong sts;

  /* Read command number from console */

read_command:
  readcons (sizeof conbuf, conbuf, 
	"\n  1=ShowProcs     10=ShowDate      15=ShowDevs       20=ThreadTrace"
	"\n  2=ShowThreads   11=PeekPoke      16=AbortThread"
	"\n                  12=Exit          17=SuspendThread"
	"\n  4=ThreadLevel   13=DumpNpp       18=ResumeThread"
        "\n  5=Logon         14=DumpEvents    19=CallDebugger"
	"\noz_knl_diag# ");

  /* Show processes and threads */

  if (strcmp (conbuf, "1") == 0) {
    oz_knl_printk ("Process list:\n");
    oz_knl_process_dump_all ();
    oz_knl_printkp ("done");
    goto read_command;
  }

  if (strcmp (conbuf, "2") == 0) {
    oz_knl_printk ("Thread list:\n");
    oz_knl_thread_dump_all ();
    oz_knl_printkp ("done");
    goto read_command;
  }

  /* Enter thread level */

  if (strcmp (conbuf, "4") == 0) {
    oz_knl_printk ("creating thread\n");
    sts = oz_knl_thread_create (oz_s_systemproc, -1, NULL, NULL, NULL, 
                                0, thread_level, NULL, OZ_ASTMODE_ENABLE, 
                                24, "oz_knl_diag thread_level", NULL, &thread);
    if (sts != OZ_SUCCESS) oz_knl_printk ("error %u creating thread\n", sts);
    else {
      oz_knl_thread_orphan (thread);
      oz_knl_printk ("thread %p created - you must exit diag mode for it to start\n", thread);
      oz_knl_thread_increfc (thread, -1);
    }
    goto read_command;
  }

  /* Start logon thread */

  if (strcmp (conbuf, "5") == 0) {
    readcons (sizeof conbuf, conbuf, "Enter terminal device name [console] > ");
    if ((conbuf[0] == 0) || (strcmp (conbuf, "console") == 0)) {
      oz_knl_printk ("oz_knl_diag: starting console logon - exit diag mode to complete\n", conbuf);
      oz_knl_logon_iochan (oz_s_coniochan);
    } else {
      devunit = oz_knl_devunit_lookup (conbuf);
      if (devunit == NULL) oz_knl_printk ("oz_knl_diag: unable to find device unit %s\n", conbuf);
      else {
        oz_knl_printk ("oz_knl_diag: starting logon on %s - exit diag mode to complete\n", conbuf);
        oz_knl_logon_devunit (devunit);
        oz_knl_devunit_increfc (devunit, -1);
      }
    }
    goto read_command;
  }

  /* Show current date/time */

  if (strcmp (conbuf, "10") == 0) {
    oz_knl_printk ("oz_knl_diag: calling oz_hw_tod_getnow\n");
    now = oz_hw_tod_getnow ();
    oz_knl_printk ("datebin 0x%x:0x%x\n", (uLong)(now >> 32), (uLong)now);
    oz_sys_datebin_decstr (0, now, sizeof conbuf, conbuf);
    oz_knl_printk ("  %s\n", conbuf);
    goto read_command;
  }

  /* Peek and Poke - platform dependent */

#if defined (OZ_HW_TYPE_486)
  if (strcmp (conbuf, "11") == 0) {
    Pp pp;

pp_read:
    readcons (sizeof conbuf, conbuf, "\n  > ");				/* read address [ mode ] [ size ] [ = data ] */
    if (conbuf[0] == 0) goto read_command;				/* if blank line, all done */
    if (conbuf[0] == '?') goto pp_help;					/* ? print help line */
    pp.addr = ztoul (conbuf, &p);					/* convert hex to address */
    pp.io = 0;								/* default is memory */
    pp.sz = 0;								/* default size is byte */
    while (1) {
      while (*p == ' ') p ++;						/* skip spaces */
      if (*p == 0) break;						/* done if end of line */
      switch (*(p ++)) {
        case 'I': case 'i': pp.io = 1; break;				/* I/O */
        case 'M': case 'm': pp.io = 0; break;				/* MEMORY */
        case 'B': case 'b': pp.sz = 0; break;				/* BYTE */
        case 'W': case 'w': pp.sz = 1; break;				/* WORD */
        case 'L': case 'l': pp.sz = 2; break;				/* LONG */
        case '=': {
          pp.data = ztoul (p, &p);					/* convert hex to data */
          while (*p == ' ') p ++;
          if (*p != 0) goto pp_help;
          sts = oz_sys_condhand_try (poke_try, &pp, oz_sys_condhand_rtnanysig, NULL);
          if (sts != OZ_SUCCESS) oz_knl_printk (" - status %u\n", sts);
          goto pp_read;
        }
        default: goto pp_help;
      }
    }
    sts = oz_sys_condhand_try (peek_try, &pp, oz_sys_condhand_rtnanysig, NULL);
    if (sts != OZ_SUCCESS) oz_knl_printk (" - status %u\n", sts);
    goto pp_read;
pp_help:
    oz_knl_printk ("\n  <address> {Io Mem} {Byte Word Long} [ = value ]\n");
    goto pp_read;
  }
#endif

  /* Exit */

  if (strcmp (conbuf, "12") == 0) return;

  /* Dump Non-paged pool usage */

  if (strcmp (conbuf, "13") == 0) {
    oz_knl_npp_dump ();
    oz_knl_printkp ("  done> ");
    goto read_command;
  }

  /* Dump Events */

  if (strcmp (conbuf, "14") == 0) {
    oz_knl_event_dump ();
    goto read_command;
  }

  /* Show Devices */

  if (strcmp (conbuf, "15") == 0) {
    oz_knl_devdump (1);
    goto read_command;
  }

  /* Abort thread */

  if (strcmp (conbuf, "16") == 0) {
    thread = assignthread (sizeof conbuf, conbuf);
    if (thread == NULL) goto read_command;
    readcons (sizeof conbuf, conbuf, "Enter abort status [ABORTEDBYCLI] > ");
    sts = OZ_ABORTEDBYCLI;
    if (conbuf[0] != 0) {
      sts = oz_hw_atoi (conbuf, &usedup);
      if (conbuf[usedup] != 0) {
        oz_knl_printk ("oz_knl_diag: invalid status value %s\n", conbuf);
        oz_knl_thread_increfc (thread, -1);
        goto read_command;
      }
    }
    oz_knl_printk ("oz_knl_diag: aborting thread %p with %u\n", thread, sts);
    oz_knl_thread_abort (thread, sts);
    oz_knl_thread_increfc (thread, -1);
    goto read_command;
  }

  /* Suspend thread */

  if (strcmp (conbuf, "17") == 0) {
    thread = assignthread (sizeof conbuf, conbuf);
    if (thread == NULL) goto read_command;
    oz_knl_printk ("oz_knl_diag: suspend thread %p\n", thread);
    i = oz_knl_thread_suspend (thread);
    if (i) oz_knl_printk ("oz_knl_diag: thread was already marked for suspension\n");
    else oz_knl_printk ("oz_knl_diag: thread is now marked for suspension\n");
    oz_knl_thread_increfc (thread, -1);
    goto read_command;
  }

  /* Resume thread */

  if (strcmp (conbuf, "18") == 0) {
    thread = assignthread (sizeof conbuf, conbuf);
    if (thread == NULL) goto read_command;
    oz_knl_printk ("oz_knl_diag: resume thread %p\n", thread);
    i = oz_knl_thread_resume (thread);
    if (i) oz_knl_printk ("oz_knl_diag: thread was marked for suspension\n");
    else oz_knl_printk ("oz_knl_diag: thread was not marked for suspension\n");
    oz_knl_thread_increfc (thread, -1);
    goto read_command;
  }

  /* Call debugger */

  if (strcmp (conbuf, "19") == 0) {
    hwi = oz_hw_cpu_sethwints (0);
    oz_knl_debug_exception (NULL, mchargs);
    oz_hw_cpu_sethwints (hwi);
    goto read_command;
  }

  /* Thread trace dump */

  if (strcmp (conbuf, "20") == 0) {
    thread = assignthread (sizeof conbuf, conbuf);
    if (thread == NULL) goto read_command;
    oz_knl_thread_tracedump (thread);
    oz_knl_thread_increfc (thread, -1);
    goto read_command;
  }

  oz_knl_printk ("unknown command code %s\n", conbuf);
  goto read_command;
}

/************************************************************************/
/*									*/
/*  This routine executes as a thread of the system process in kernel 	*/
/*  mode.  It can do things that cause pagefaults, however the other 	*/
/*  cpu's are running and so is the scheduler.				*/
/*									*/
/************************************************************************/

static uLong thread_level (void *dummy)

{
  char conbuf[64], diskdevname[256], filbuf[1024], *fileidstrbf, *p;
  char tempdevname[64], volname[32];
  int i;
  OZ_IO_fs_getinfo1 fs_getinfo1;
  OZ_IO_fs_getinfo3 fs_getinfo3;
  OZ_IO_fs_initvol fs_initvol;
  OZ_IO_fs_mountvol fs_mountvol;
  OZ_IO_fs_open fs_open;
  OZ_IO_fs_readdir fs_readdir;
  OZ_IO_fs_readrec fs_readrec;
  OZ_IO_fs_writerec fs_writerec;
  OZ_Iochan *filioch, *tempiochan;
  OZ_Logname *logname, *lognamtbl;
  OZ_Logvalue lnmvals[16];
  OZ_Process *process;
  OZ_Thread *thread;
  uLong rlen, sts;
  void *fileidbuff;

  static const char trmbuff[1] = { '\n' };

  /* Most of these routines assume software interrupt delivery is inhibited */
  /* (Like oz_knl_process_create barfs if it isn't)                         */

  oz_hw_cpu_setsoftint (0);

  /* Read command number from console */

read_command:
  readcons (sizeof conbuf, conbuf, 
	"\n  1=SoftintLevel  5=DisplayFile   9=CreateDir  13=Reboot"
        "\n  2=RunImage      6=InitVol      10=CopyFile"
	"\n  3=ShowLogical   7=MountVol     11=DismVol"
	"\n  4=CreateLnm     8=ListDir      12=Exit"
	"\noz_knl_diag> ");

  /* Return to softint level */

  if (strcmp (conbuf, "1") == 0) {
    oz_knl_printk ("oz_knl_diag: returning to softint level\n");
    oz_hw_diag ();
    oz_hw_cpu_setsoftint (1);
    return (OZ_SUCCESS);
  }

  /* Run an executable */

  if (strcmp (conbuf, "2") == 0) {
    readcons (sizeof imagename, imagename, "Enter image name to run > ");
    if (imagename[0] == 0) goto read_command;

    oz_knl_printk ("oz_knl_diag: creating process\n");
    sts = oz_knl_process_create (oz_s_systemjob, 0, 0, strlen (imagename), imagename, NULL, &process);
    if (sts != OZ_SUCCESS) {
      oz_knl_printk ("oz_knl_diag: error %u creating process\n", sts);
      goto read_command;
    }

    oz_knl_printk ("oz_knl_diag: creating input, output and error logical names\n");
    lognamtbl = oz_knl_process_getlognamtbl (process);
    sts = oz_knl_logname_creobj (lognamtbl, OZ_PROCMODE_KNL, NULL, oz_s_secattr_syslogname, 0, 8, "OZ_ERROR", oz_s_coniochan, NULL);
    if (sts != OZ_SUCCESS) oz_knl_printk ("oz_knl_diag: error %u creating logical OZ_ERROR\n", sts);
    sts = oz_knl_logname_creobj (lognamtbl, OZ_PROCMODE_KNL, NULL, oz_s_secattr_syslogname, 0, 8, "OZ_INPUT", oz_s_coniochan, NULL);
    if (sts != OZ_SUCCESS) oz_knl_printk ("oz_knl_diag: error %u creating logical OZ_INPUT\n", sts);
    sts = oz_knl_logname_creobj (lognamtbl, OZ_PROCMODE_KNL, NULL, oz_s_secattr_syslogname, 0, 9, "OZ_OUTPUT", oz_s_coniochan, NULL);
    if (sts != OZ_SUCCESS) oz_knl_printk ("oz_knl_diag: error %u creating logical OZ_OUTPUT\n", sts);
    sts = oz_knl_logname_crestr (lognamtbl, OZ_PROCMODE_KNL, NULL, oz_s_secattr_syslogname, 0, 8, "OZ_IMAGE", imagename, NULL);
    if (sts != OZ_SUCCESS) oz_knl_printk ("oz_knl_diag: error %u creating logical OZ_IMAGE\n", sts);
    sts = oz_knl_logname_create (lognamtbl, OZ_PROCMODE_KNL, NULL, oz_s_secattr_syslogname, 0, 9, "OZ_PARAMS", 0, NULL, NULL);
    if (sts != OZ_SUCCESS) oz_knl_printk ("oz_knl_diag: error %u creating logical OZ_PARAMS\n", sts);

    oz_knl_printk ("oz_knl_diag: creating thread\n");
    sts = oz_knl_thread_create (process, -2, NULL, NULL, NULL, USER_STACK_SIZE, thread_init, NULL, 
                                OZ_ASTMODE_ENABLE, strlen (imagename), imagename, NULL, &thread);
    if (sts != OZ_SUCCESS) oz_knl_printk ("error %u creating thread\n", sts);
    else {
      oz_knl_thread_orphan (thread);
      oz_knl_printk ("oz_knl_diag: thread %p created\n", thread);
      oz_knl_thread_increfc (thread, -1);
    }
    oz_knl_process_increfc (process, -1);
    goto read_command;
  }

  /* Show Logicals */

  if (strcmp (conbuf, "3") == 0) {
    readcons (sizeof tempdevname, tempdevname, "Enter logical name [OZ_SYSTEM_DIRECTORY] > ");
    if (tempdevname[0] == 0) {
      oz_knl_logname_dump (0, oz_s_systemdirectory);
    } else {
      sts = oz_knl_logname_lookup (oz_s_systemdirectory, OZ_PROCMODE_USR, 
                                   strlen (tempdevname), tempdevname, NULL, NULL, NULL, NULL, 
                                   &logname, NULL);
      if (sts != OZ_SUCCESS) {
        oz_knl_printk ("oz_knl_diag: error %u looking up %s\n", sts, tempdevname);
      } else {
        oz_knl_logname_dump (0, logname);
        oz_knl_logname_increfc (logname, -1);
      }
    }
    goto read_command;
  }

  /* Create logical name */

  if (strcmp (conbuf, "4") == 0) {
    readcons (sizeof tempdevname, tempdevname, "Enter logical name > ");
    if (tempdevname[0] == 0) goto read_command;
    for (i = 0; i < sizeof lnmvals / sizeof lnmvals[0]; i ++) {
      oz_sys_sprintf (sizeof conbuf, conbuf, "Enter value[%d] > ", i);
      readcons (sizeof diskdevname, diskdevname, conbuf);
      if (diskdevname[0] == 0) break;
      lnmvals[i].attr = OZ_LOGVALATR_TERMINAL;
      lnmvals[i].buff = OZ_KNL_PGPMALLOC (strlen (diskdevname) + 1);
      strcpy (lnmvals[i].buff, diskdevname);
    }
    if (i == 0) goto read_command;
    sts = oz_knl_logname_create (oz_s_systemtable, OZ_PROCMODE_KNL, NULL, oz_s_secattr_syslogname, 0, strlen (tempdevname), tempdevname, i, lnmvals, NULL);
    if (sts == OZ_SUCCESS) oz_knl_printk ("oz_knl_diag: logical %s created\n", tempdevname);
    else if (sts == OZ_SUPERSEDED) oz_knl_printk ("oz_knl_diag: logical %s superseded\n", tempdevname);
    else oz_knl_printk ("oz_knl_diag: error %u creating logical %s\n", tempdevname);
    while (-- i >= 0) OZ_KNL_PGPFREE (lnmvals[i].buff);
    goto read_command;
  }

  /* Display a file */

  if (strcmp (conbuf, "5") == 0) {
    readcons (sizeof conbuf, conbuf, "Enter disk:file to display > ");
    oz_knl_printk ("oz_knl_diag: going to display file %s\n", conbuf);

    /* Assign a channel to the disk drive */

    filioch = openfile (conbuf, OZ_LOCKMODE_CR, 1);
    if (filioch == NULL) goto read_command;

    /* Read a record from the file */

display_loop:
    memset (&fs_readrec, 0, sizeof fs_readrec);
    fs_readrec.size = sizeof filbuf - 1;
    fs_readrec.buff = filbuf;
    fs_readrec.trmsize = 1;
    fs_readrec.trmbuff = trmbuff;
    fs_readrec.rlen = &rlen;
    sts = oz_knl_io (filioch, OZ_IO_FS_READREC, sizeof fs_readrec, &fs_readrec);
    if (sts != OZ_SUCCESS) {
      if (sts == OZ_ENDOFFILE) oz_knl_printk ("oz_knl_diag: end of input file\n");
      else oz_knl_printk ("oz_knl_diag: error %u reading file\n", sts);
      goto display_done;
    }
    filbuf[rlen] = 0;

    /* Write it to console */

    oz_knl_printk ("%s\n", filbuf);
    goto display_loop;

display_done:
    oz_knl_iochan_increfc (filioch, -1);
    filioch = NULL;
    goto read_command;
  }

  /* Initialize volume */

  if (strcmp (conbuf, "6") == 0) {
    readcons (sizeof tempdevname, tempdevname, "Enter fs template device name > ");
    readcons (sizeof diskdevname, diskdevname, "Enter disk device name > ");
    readcons (sizeof volname, volname, "Enter volume name > ");
    readcons (sizeof conbuf, conbuf, "Enter cluster factor > ");
    sts = oz_knl_iochan_crbynm (tempdevname, OZ_LOCKMODE_PW, OZ_PROCMODE_KNL, NULL, &tempiochan);
    if (sts != OZ_SUCCESS) {
      oz_knl_printk ("oz_knl_diag: error %u assigning channel to %s\n", sts, tempdevname);
      goto read_command;
    }
    memset (&fs_initvol, 0, sizeof fs_initvol);
    fs_initvol.devname = diskdevname;
    fs_initvol.volname = volname;
    fs_initvol.clusterfactor = oz_hw_atoi (conbuf, NULL);
    sts = oz_knl_io (tempiochan, OZ_IO_FS_INITVOL, sizeof fs_initvol, &fs_initvol);
    if (sts == OZ_SUCCESS) oz_knl_printk ("oz_knl_diag: volume initialized\n");
    else oz_knl_printk ("oz_knl_diag: error %u initializing volume\n", sts);
    oz_knl_iochan_increfc (tempiochan, -1);
    goto read_command;
  }

  /* Mount volume */

  if (strcmp (conbuf, "7") == 0) {
    readcons (sizeof tempdevname, tempdevname, "Enter fs template device name > ");
    readcons (sizeof diskdevname, diskdevname, "Enter disk device name > ");
    sts = oz_knl_iochan_crbynm (tempdevname, OZ_LOCKMODE_PW, OZ_PROCMODE_KNL, NULL, &tempiochan);
    if (sts != OZ_SUCCESS) {
      oz_knl_printk ("oz_knl_diag: error %u assigning channel to %s\n", sts, tempdevname);
      goto read_command;
    }
    memset (&fs_mountvol, 0, sizeof fs_mountvol);
    fs_mountvol.devname = diskdevname;
    sts = oz_knl_io (tempiochan, OZ_IO_FS_MOUNTVOL, sizeof fs_mountvol, &fs_mountvol);
    if (sts != OZ_SUCCESS) {
      oz_knl_printk ("oz_knl_diag: error %u mounting volume\n", sts);
    } else {
      oz_knl_printk ("oz_knl_diag: volume mounted on device %s\n", oz_knl_devunit_devname (oz_knl_iochan_getdevunit (tempiochan)));
    }
    oz_knl_iochan_increfc (tempiochan, -1);
    goto read_command;
  }

  /* List Directory */

  if (strcmp (conbuf, "8") == 0) {
    readcons (sizeof filbuf, filbuf, "Enter fs device name:directory > ");
    tempiochan = openfile (filbuf, OZ_LOCKMODE_CR, 1);
    if (tempiochan == NULL) goto read_command;
    p = filbuf + strlen (filbuf);
    memset (&fs_getinfo3, 0, sizeof fs_getinfo3);
    sts = oz_knl_io (tempiochan, OZ_IO_FS_GETINFO3, sizeof fs_getinfo3, &fs_getinfo3);
    if ((sts != OZ_SUCCESS) && (sts != OZ_BADIOFUNC)) {
      oz_knl_printk ("oz_knl_diag: error %u getting volume info\n");
    }
    if ((sts == OZ_SUCCESS) && (fs_getinfo3.fileidsize != 0) && (fs_getinfo3.fileidstrsz != 0) && (fs_getinfo3.fidtoa != NULL)) {
      fileidbuff  = OZ_KNL_PGPMALLOC (fs_getinfo3.fileidsize);
      fileidstrbf = OZ_KNL_PGPMALLOC (fs_getinfo3.fileidstrsz);
    } else {
      fs_getinfo3.fileidsize  = 0;
      fs_getinfo3.fileidstrsz = 0;
      fs_getinfo3.fidtoa      = NULL;
      fileidbuff  = NULL;
      fileidstrbf = NULL;
    }

readdir_loop:
    memset (&fs_readdir, 0, sizeof fs_readdir);
    fs_readdir.filenamsize = filbuf - p + sizeof filbuf;
    fs_readdir.filenambuff = p;
    fs_readdir.fileidsize  = fs_getinfo3.fileidsize;
    fs_readdir.fileidbuff  = fileidbuff;
    sts = oz_knl_io (tempiochan, OZ_IO_FS_READDIR, sizeof fs_readdir, &fs_readdir);
    if (sts != OZ_SUCCESS) {
      if (sts == OZ_ENDOFFILE) oz_knl_printk ("oz_knl_diag: end of directory\n");
      else oz_knl_printk ("oz_knl_diag: error %u reading directory\n", sts);
      goto readdir_done;
    }
    if (fs_getinfo3.fidtoa == NULL) oz_knl_printk ("	%s\n", p);
    else {
      (*(fs_getinfo3.fidtoa)) (fileidbuff, fs_getinfo3.fileidstrsz, fileidstrbf);
      oz_knl_printk ("	%s  %s\n", p, fileidstrbf);
    }

    filioch = openfile (filbuf, OZ_LOCKMODE_NL, 0);
    if (filioch == NULL) goto readdir_loop;
    memset (&fs_getinfo1, 0, sizeof fs_getinfo1);
    sts = oz_knl_io (filioch, OZ_IO_FS_GETINFO1, sizeof fs_getinfo1, &fs_getinfo1);
    if (sts != OZ_SUCCESS) {
      oz_knl_printk ("				error %u getting info for file %s\n", sts, fs_open.name);
      oz_knl_iochan_increfc (filioch, -1);
      p[i] = 0;
      goto readdir_loop;
    }
    oz_knl_printk ("                         eof: %u.%u  all: %u  flags: %x\n", fs_getinfo1.eofblock, fs_getinfo1.eofbyte, fs_getinfo1.hiblock, fs_getinfo1.filattrflags);
    oz_knl_printk ("                     created: %t\n", fs_getinfo1.create_date);
    oz_knl_printk ("                     changed: %t\n", fs_getinfo1.change_date);
    oz_knl_printk ("                    accessed: %t\n", fs_getinfo1.access_date);
    oz_knl_printk ("                    modified: %t\n", fs_getinfo1.modify_date);
    oz_knl_iochan_increfc (filioch, -1);
    goto readdir_loop;

readdir_done:
    oz_knl_iochan_increfc (tempiochan, -1);
    if (fileidbuff  != NULL) OZ_KNL_PGPFREE (fileidbuff);
    if (fileidstrbf != NULL) OZ_KNL_PGPFREE (fileidstrbf);
    goto read_command;
  }

  /* Create Directory */

  if (strcmp (conbuf, "9") == 0) {
    readcons (sizeof tempdevname, tempdevname, "Enter fs disk:directory > ");
    tempiochan = createfile (tempdevname, OZ_FS_FILATTRFLAG_DIRECTORY);
    if (tempiochan != NULL) oz_knl_iochan_increfc (tempiochan, -1);
    goto read_command;
  }

  /* Copy file */

  if (strcmp (conbuf, "10") == 0) {
    readcons (sizeof tempdevname, tempdevname, "From > ");
    if (tempdevname[0] == 0) goto read_command;
    tempiochan = openfile (tempdevname, OZ_LOCKMODE_CR, 1);
    if (tempiochan == NULL) goto read_command;

    readcons (sizeof diskdevname, diskdevname, "To > ");
    if (diskdevname[0] == 0) goto copy_doney;
    filioch = createfile (diskdevname, 0);
    if (filioch == NULL) goto copy_doney;

    memset (&fs_readrec, 0, sizeof fs_readrec);
    fs_readrec.size    = sizeof conbuf;
    fs_readrec.buff    = conbuf;
    fs_readrec.trmsize = 1;
    fs_readrec.trmbuff = trmbuff;
    fs_readrec.pmtsize = 1;
    fs_readrec.pmtbuff = ">";
    fs_readrec.rlen    = &fs_writerec.size;

    memset (&fs_writerec, 0, sizeof fs_writerec);
    fs_writerec.buff    = conbuf;
    fs_writerec.trmbuff = trmbuff;
    fs_writerec.append  = 1;

    rlen = 0;

copy_loop:
    sts = oz_knl_io (tempiochan, OZ_IO_FS_READREC, sizeof fs_readrec, &fs_readrec);
    if ((sts != OZ_SUCCESS) && (sts != OZ_NOTERMINATOR)) {
      if (sts == OZ_ENDOFFILE) oz_knl_printk ("oz_knl_diag: end of input file\n");
      else oz_knl_printk ("oz_knl_diag: error %u reading input file\n", sts);
      goto copy_done;
    }
    fs_writerec.trmsize = (sts == OZ_SUCCESS);
    rlen += fs_writerec.size + fs_writerec.trmsize;
    sts = oz_knl_io (filioch, OZ_IO_FS_WRITEREC, sizeof fs_writerec, &fs_writerec);
    if (sts != OZ_SUCCESS) {
      oz_knl_printk ("oz_knl_diag: error %u writing output file\n", sts);
      goto copy_done;
    }
    goto copy_loop;

copy_done:
    sts = oz_knl_io (filioch, OZ_IO_FS_CLOSE, 0, NULL);
    if (sts != OZ_SUCCESS) {
      oz_knl_printk ("oz_knl_diag: error %u closing output file\n", sts);
    }
    oz_knl_printk ("oz_knl_diag: %u bytes copied\n", rlen);
    oz_knl_iochan_increfc (filioch, -1);
copy_doney:
    oz_knl_iochan_increfc (tempiochan, -1);
    filioch = NULL;
    tempiochan = NULL;
    goto read_command;
  }

  /* Dismount volume */

  if (strcmp (conbuf, "11") == 0) {
    readcons (sizeof tempdevname, tempdevname, "Enter fs device name > ");
    sts = oz_knl_iochan_crbynm (tempdevname, OZ_LOCKMODE_PW, OZ_PROCMODE_KNL, NULL, &tempiochan);
    if (sts != OZ_SUCCESS) {
      oz_knl_printk ("oz_knl_diag: error %u assigning channel to %s\n", sts, tempdevname);
      goto read_command;
    }
    sts = oz_knl_io (tempiochan, OZ_IO_FS_DISMOUNT, 0, NULL);
    if (sts == OZ_SUCCESS) oz_knl_printk ("volume dismounted\n");
    else oz_knl_printk ("oz_knl_diag: error %u dismounting volume\n", sts);
    oz_knl_iochan_increfc (tempiochan, -1);
    goto read_command;
  }

  /* Exit */

  if (strcmp (conbuf, "12") == 0) {
    oz_knl_printk ("oz_knl_diag: exiting\n");
    oz_hw_cpu_setsoftint (1);
    return (OZ_SUCCESS);
  }

  /* Reboot */

  if (strcmp (conbuf, "13") == 0) {
    oz_knl_shutdown ();
    oz_knl_printk ("Rebooting...\n");
    oz_hw_stl_microwait (1000000, NULL, NULL);
    oz_hw_reboot ();
  }

  oz_knl_printk ("unknown command code %s\n", conbuf);
  goto read_command;
}

/************************************************************************/
/*									*/
/*  Select and assign handle to thread					*/
/*									*/
/************************************************************************/

static OZ_Thread *assignthread (int consiz, char *conbuf)

{
  OZ_Process *lastprocess, *process;
  OZ_Thread *lastthread, *thread;

  lastprocess = NULL;
  while (1) {
    process = oz_knl_process_getnext (lastprocess, NULL);
    if (lastprocess != NULL) oz_knl_process_increfc (lastprocess, -1);
    if (process == NULL) break;
    oz_knl_process_dump (process, 0);
    lastthread = NULL;
    while (1) {
      thread = oz_knl_thread_getnext (lastthread, process);
      if (lastthread != NULL) oz_knl_thread_increfc (lastthread, -1);
      if (thread == NULL) break;
      oz_knl_thread_dump (thread);
      do {
        readcons (consiz, conbuf, "yes, [no], quit? ");
        if (strcmp (conbuf, "quit") == 0) {
          oz_knl_process_increfc (process, -1);
          oz_knl_thread_increfc (thread, -1);
          return (NULL);
        }
        if (strcmp (conbuf, "yes") == 0) {
          oz_knl_process_increfc (process, -1);
          return (thread);
        }
      } while ((strcmp (conbuf, "no") != 0) && (conbuf[0] != 0));
      lastthread = thread;
    }
    lastprocess = process;
  }
  return (NULL);
}

/************************************************************************/
/*									*/
/*  Utility routines for peek/poke					*/
/*									*/
/************************************************************************/

#if defined (OZ_HW_TYPE_486)

/* Convert hexadecimal ascii string to binary */

static uLong ztoul (char *b, char **p)

{
  char c;
  uLong value;

  while (*b == ' ') b ++;

  value = 0;
  while (1) {
    c = *b;
    if ((c >= 'a') && (c <= 'f')) c -= 'a' - 'A';
    if ((c >= '0') && (c <= '9')) value = value * 16 + c - '0';
    else if ((c >= 'A') && (c <= 'F')) value = value * 16 + c - 'A' + 10;
    else break;
    b ++;
  }
  *p = b;
  return (value);
}

static const char *const pptypes[6] = { "MB", "IB", "MW", "IW", "ML", "IL" };

static uLong peek_try (void *ppv)

{
  Pp *pp;
  uLong pp_data;

  pp = ppv;

  oz_knl_printk ("  %X %s / ", pp -> addr, pptypes[pp->io+pp->sz*2]);
  switch (pp -> io + pp -> sz * 2) {
    case 0: pp_data = *(uByte *)(pp -> addr); break;
    case 1: pp_data = oz_hw486_inb (pp -> addr); break;
    case 2: pp_data = *(uWord *)(pp -> addr); break;
    case 3: pp_data = oz_hw486_inw (pp -> addr); break;
    case 4: pp_data = *(uLong *)(pp -> addr); break;
    case 5: pp_data = oz_hw486_inl (pp -> addr); break;
  }
  oz_knl_printk ("%*.*X  ok\n", 2 << pp -> sz, 2 << pp -> sz, pp_data);
  return (OZ_SUCCESS);
}

static uLong poke_try (void *ppv)

{
  Pp *pp;

  pp = ppv;

  oz_knl_printk ("  %X %s = %*.*X  ", pp -> addr, pptypes[pp->io+pp->sz*2], 2 << pp -> sz, 2 << pp -> sz, pp -> data);
  switch (pp -> io + pp -> sz * 2) {
    case 0: *(uByte *)(pp -> addr) = pp -> data; break;
    case 1: oz_hw486_outb (pp -> data, pp -> addr); break;
    case 2: *(uWord *)(pp -> addr) = pp -> data; break;
    case 3: oz_hw486_outw (pp -> data, pp -> addr); break;
    case 4: *(uLong *)(pp -> addr) = pp -> data; break;
    case 5: oz_hw486_outl (pp -> data, pp -> addr); break;
  }
  oz_knl_printk ("ok\n");
  return (OZ_SUCCESS);
}

#endif

/************************************************************************/
/*									*/
/*  Read command line from console					*/
/*									*/
/************************************************************************/

static void readcons (int size, char *buff, char *prompt)

{
  int hwi;

  hwi = oz_hw_cpu_sethwints (0);
  oz_hw_getcon (size, buff, strlen (prompt), prompt);
  oz_hw_cpu_sethwints (hwi);
}

/************************************************************************/
/*									*/
/*  Used by thread-level routine to open a file				*/
/*									*/
/************************************************************************/

static OZ_Iochan *openfile (char *filename, OZ_Lockmode lockmode, int verbose)

{
  char *p;
  uLong sts;
  OZ_IO_fs_open fs_open;
  OZ_Iochan *iochan;

  /* Assign a channel to the disk drive */
  
  p = strchr (filename, ':');
  if (p == NULL) {
    oz_knl_printk ("oz_knl_diag: missing : in %s\n", filename);
    return (NULL);
  }
  *p = 0;
  if (verbose) oz_knl_printk ("oz_knl_diag: assigning channel to device %s\n", filename);
  sts = oz_knl_iochan_crbynm (filename, OZ_LOCKMODE_CR, OZ_PROCMODE_KNL, NULL, &iochan);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_knl_diag: error %u assigning channel to %s\n", sts, filename);
    *(p ++) = ':';
    return (NULL);
  }
  *(p ++) = ':';

  /* Open the file */

  memset (&fs_open, 0, sizeof fs_open);
  fs_open.name = p;
  fs_open.lockmode = lockmode;
  if (verbose) oz_knl_printk ("oz_knl_diag: opening file %s\n", fs_open.name);
  sts = oz_knl_io (iochan, OZ_IO_FS_OPEN, sizeof fs_open, &fs_open);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_knl_diag: error %u opening file %s\n", sts, p);
    oz_knl_iochan_increfc (iochan, -1);
    return (NULL);
  }

  if (verbose) oz_knl_printk ("oz_knl_diag: open complete\n");

  return (iochan);
}

static OZ_Iochan *createfile (char *filename, uLong filattrflags)

{
  char *p;
  uLong sts;
  OZ_IO_fs_create fs_create;
  OZ_Iochan *iochan;

  /* Assign a channel to the disk drive */
  
  p = strchr (filename, ':');
  if (p == NULL) {
    oz_knl_printk ("oz_knl_diag: missing : in %s\n", filename);
    return (NULL);
  }
  *p = 0;
  oz_knl_printk ("oz_knl_diag: assigning channel to device %s\n", filename);
  sts = oz_knl_iochan_crbynm (filename, OZ_LOCKMODE_CW, OZ_PROCMODE_KNL, NULL, &iochan);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_knl_diag: error %u assigning channel to %s\n", sts, filename);
    *(p ++) = ':';
    return (NULL);
  }
  *(p ++) = ':';

  /* Create the file */

  memset (&fs_create, 0, sizeof fs_create);
  fs_create.name = p;
  fs_create.lockmode = OZ_LOCKMODE_CW;
  fs_create.filattrflags = filattrflags;
  oz_knl_printk ("oz_knl_diag: creating file %s\n", fs_create.name);
  sts = oz_knl_io (iochan, OZ_IO_FS_CREATE, sizeof fs_create, &fs_create);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_knl_diag: error %u creating file %s\n", sts, p);
    oz_knl_iochan_increfc (iochan, -1);
    return (NULL);
  }

  oz_knl_printk ("oz_knl_diag: create complete\n");

  return (iochan);
}

/************************************************************************/
/*									*/
/*  This is used by the run command - it is the thread that loads and 	*/
/*  runs the image							*/
/*									*/
/*  Note that the thread_init routine runs in user mode			*/
/*									*/
/************************************************************************/

static uLong getimagename (OZ_Procmode cprocmode, void *uimagename);
static uLong dumpthreadstuff (OZ_Procmode cprocmode, void *baseaddr);

static uLong thread_init (void *dummy)

{
  char uimagename[64];
  uLong exsts, sts;
  OZ_Handle h_image;
  void *baseaddr, *startaddr;

  sts = oz_sys_callknl (getimagename, uimagename);
  if (sts != OZ_SUCCESS) {
    oz_sys_printkp (0, "oz_knl_diag thread_init: error %u getting imagename to %p\n", sts, uimagename);
    return (sts);
  }

  oz_sys_printkp (0, "oz_knl_diag thread_init: loading image %s ...\n", uimagename);
  sts = oz_sys_image_load (OZ_PROCMODE_KNL, uimagename, 0, &baseaddr, &startaddr, &h_image);
  if (sts != OZ_SUCCESS) {
    oz_sys_printkp (0, "oz_knl_diag thread_init: error %u loading image %s\n", sts, uimagename);
    return (sts);
  }
  oz_sys_printkp (0, "oz_knl_diag thread_init: image loaded at address %p, handle %x\n", baseaddr, h_image);
  oz_sys_printkp (0, "oz_knl_diag thread_init: entrypoint at %p (%X)\n", startaddr, *(uLong *)startaddr);
  oz_sys_callknl (dumpthreadstuff, baseaddr);
  oz_sys_callknl (dumpthreadstuff, startaddr);

  oz_sys_printkp (0, "oz_knl_diag thread_init: starting debugger\n");
  exsts = oz_usr_debug_init (startaddr, NULL);
  oz_sys_printkp (0, "oz_knl_diag thread_init:  - routine returned status %u\n", exsts);

  oz_sys_printkp (0, "oz_knl_diag thread_init: unloading image ...\n");
  sts = oz_sys_handle_release (OZ_PROCMODE_KNL, h_image);
  oz_sys_printkp (0, "oz_knl_diag thread_init:  - unload status %u\n", sts);

  return (exsts);
}

static uLong getimagename (OZ_Procmode cprocmode, void *uimagename)

{
  memcpy (uimagename, imagename, 64);
  return (OZ_SUCCESS);
}

static uLong dumpthreadstuff (OZ_Procmode cprocmode, void *baseaddr)

{
  int si;

  si = oz_hw_cpu_setsoftint (0);
  oz_hw_pte_print (baseaddr);
  if (OZ_HW_READABLE (32, baseaddr, cprocmode)) oz_knl_dumpmem (32, baseaddr);
  else oz_knl_printk ("oz_knl_diag thread_init: ... not readable\n");
  oz_hw_cpu_setsoftint (si);

  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Halt the cpu so it looks like it was never here			*/
/*									*/
/************************************************************************/

oz_knl_halt ()

{
  Long cpuidx;

  cpuidx = oz_hw_cpu_getcur ();
  oz_knl_printk ("oz_knl_halt: cpu %d halting\n", cpuidx);	// print a message
  oz_hw_atomic_and_long (&oz_s_cpusavail, ~(1 << cpuidx));	// say I'm no longer available for anything
  oz_hw_halt ();						// call the hardware layer halt routine
}
