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
/*  User mode debugger							*/
/*									*/
/*  (It assumes express ast's can be delivered, so it won't run above 	*/
/*   softint level, and it assumes that it runs as scheduled threads)	*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_io_console.h"
#include "oz_knl_hw.h"
#include "oz_knl_procmode.h"
#include "oz_knl_status.h"
#include "oz_sys_condhand.h"
#include "oz_sys_debug.h"
#include "oz_sys_disassemble.h"
#include "oz_sys_event.h"
#include "oz_sys_handle.h"
#include "oz_sys_io_fs_printf.h"
#include "oz_sys_misc.h"
#include "oz_sys_process.h"
#include "oz_sys_section.h"
#include "oz_sys_thread.h"
#include "oz_sys_xprintf.h"
#include "oz_usr_debug.h"

#define ALERTCHAR 'P'
#define MAXTHREADS 32
#define PROMPTSIZE (32+OZ_THREAD_NAMESIZE)

typedef struct {
        uLong (*entry) (void *param);
        void *param;
	OZ_Handle h_breaklock;			/* event flag for breakpoint database lock */
	OZ_Handle h_printlock;			/* event flag for printing lock */
	OZ_Handle h_threadlock;			/* event flag for thread database lock */
	OZ_Handle h_input;			/* iochan for input */
	OZ_Handle h_output;			/* iochan for output */
	OZ_Handle h_alert;			/* event flag signifying alert event (control-P) */
        OZ_Handle threadsusp[MAXTHREADS];	/* event flags for udb_suspend/_resume routine */
        OZ_Handle h_threads[MAXTHREADS];	/* thread handles */
        volatile uLong alertsts;		/* I/O status for alert ast */
        void *threads[MAXTHREADS];		/* thread object pointers */
	uByte dc[OZ_DEBUG_SIZE];		/* oz_sys_ debugger context */
} Udb;

typedef struct { void *buff;
                 uLong size;
                 void *addr;
               } Rwmpb;

static uLong readmem (void *buff, uLong size, void *addr);
static uLong readmem_try (void *rwmpbv);
static uLong writemem (void *buff, uLong size, void *addr);
static uLong writemem_try (void *rwmpbv);
static uLong getchan (char *dname, char *name, OZ_Handle *h_r);
static void armalert (Udb *udb);
static void alertast (void *cp, uLong status, OZ_Mchargs *mchargs);
static uLong tryit (void *cp);
static uLong exception (OZ_Chparam dc, OZ_Sigargs *sigargs, OZ_Mchargs *mchargs);
static void haltast (void *cp, uLong status, OZ_Mchargs *mchargs);
static void *getthreadaddr (OZ_Handle h_thread);
static void getprompt (void *cp, char *buff);

void oz_usr_debug_top (void)
{}

/************************************************************************/
/*									*/
/*  Callback routines							*/
/*									*/
/************************************************************************/

static void udb_print (void *cp, char *fmt, ...);
static int udb_readmem (void *cp, void *buff, uLong size, void *addr);

/* Get index of current thread -                  */
/*  Returns a number 0..31 assigned to the thread */

static Long udb_getcur (void *cp)

{
  Long i, j, oldvalue;
  uLong exitsts, sts;
  OZ_Handle h_thisthread;
  OZ_Handle_item items[1];
  Udb *udb;
  void *thisthread;

  udb = cp;

  /* Assign handle to current thread and get its kernel object address */

  items[0].size = sizeof h_thisthread;
  items[0].buff = &h_thisthread;
  items[0].code = OZ_HANDLE_CODE_THREAD_HANDLE;
  items[0].rlen = NULL;
  sts = oz_sys_handle_getinfo (0, 1, items, NULL);
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);

  thisthread = getthreadaddr (h_thisthread);

  /* Lock thread database */

  while (1) {
    sts = oz_sys_event_set (OZ_PROCMODE_KNL, udb -> h_threadlock, 0, &oldvalue);
    if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
    if (oldvalue > 0) break;
    sts = oz_sys_event_wait (OZ_PROCMODE_KNL, udb -> h_threadlock, 0);
    if ((sts != OZ_ASTDELIVERED) && (sts != OZ_FLAGWASSET) && (sts != OZ_FLAGWASCLR)) oz_sys_condhand_signal (2, sts, 0);
  }

  /* See if this thread already in table.  If so, return its index. */

  j = -1;
  for (i = 0; i < MAXTHREADS; i ++) {					/* scan the list */
    if (udb -> threads[i] == thisthread) {				/* see if it is this thread */
      oz_sys_handle_release (OZ_PROCMODE_KNL, h_thisthread);		/* it is this one, release redundant handle */
      goto rtn;								/* return the index 'i' */
    }
    if (udb -> threads[i] != NULL) {
      sts = oz_sys_thread_getexitsts (udb -> h_threads[i], &exitsts);	/* not this one, see if it is still alive */
      if (sts == OZ_SUCCESS) {
        oz_sys_handle_release (OZ_PROCMODE_KNL, udb -> h_threads[i]);	/* it has exited, clear out it's table entry */
        udb -> h_threads[i] = 0;
        udb -> threads[i]   = NULL;
      }
    }
    if ((udb -> threads[i] == NULL) && (j < 0)) j = i;			/* see if a free one was found */
  }

  /* If not, look for an empty spot.  Fill it, and create a suspend event flag. */

  if (j < 0) {
    udb_print (udb, "oz_usr_debug udb_getcur: maximum threads exceeded, %d allowed\n", MAXTHREADS);
    oz_sys_thread_exit (OZ_MAXTHREADS);
  }

  i = j;
  udb -> threads[i]   = thisthread;							/* save kernel object pointer */
  udb -> h_threads[i] = h_thisthread;							/* save handle */
  if (udb -> threadsusp[i] == 0) {
    sts = oz_sys_event_create (OZ_PROCMODE_KNL, "udb suspend", udb -> threadsusp + i);	/* create a 'suspend' event flag */
    if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
  }
  sts = oz_sys_event_set (OZ_PROCMODE_KNL, udb -> threadsusp[i], 1, NULL);		/* set the 'suspend' event flag, ie, we are 'resumed' */
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);

  /* Unlock and return index in i */

rtn:
  sts = oz_sys_event_set (OZ_PROCMODE_KNL, udb -> h_threadlock, 1, &oldvalue);
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
  return (i);
}

/* Print a message on the debugger's output channel */

static void udb_print (void *cp, char *fmt, ...)

{
  Udb *udb;
  va_list ap;

  udb = cp;
  va_start (ap, fmt);
  oz_sys_io_fs_vprintf (udb -> h_output, fmt, ap);
  va_end (ap);
}

/* Abort the current thread (error message has already been output) */

static void udb_abort (void *cp, OZ_Sigargs *sigargs, OZ_Mchargs *mchargs, void *mchargx)

{
  oz_sys_thread_exit ((sigargs != NULL) ? sigargs[1] : OZ_ABORTED);
}

/* Read the breakpoint at a given address (not known to be readable) */

static int udb_readbpt (void *cp, OZ_Breakpoint *bptaddr, OZ_Breakpoint *oldcontents)

{
  return (udb_readmem (cp, oldcontents, sizeof *oldcontents, bptaddr));
}

/* Write a breakpoint at a given address (not known to be readable) */

static char *udb_writebpt (void *cp, OZ_Breakpoint *bptaddr, OZ_Breakpoint opcode)

{
  char *rc;
  uLong sts;
  OZ_Mempage vpage;
  OZ_Hw_pageprot oldprot;
  Udb *udb;

  udb = cp;

  vpage = OZ_HW_VADDRTOVPAGE (bptaddr);						/* get the vpage the breakpoint is in */
  sts   = oz_sys_section_setpageprot (1, vpage, OZ_HW_PAGEPROT_UW, &oldprot);	/* make the page writable */
  rc    = "error setting section page protection";
  if (sts == OZ_SUCCESS) {
    sts = writemem (&opcode, sizeof *bptaddr, bptaddr);				/* write the breakpoint */
    rc  = "error writing breakpoint";
    if (sts == OZ_SUCCESS) rc = NULL;
    if (oldprot != OZ_HW_PAGEPROT_UW) oz_sys_section_setpageprot (1, vpage, oldprot, NULL); /* restore page protection */
  }
  else oz_sys_printkp (0, "oz_usr_debug*: udb_writebpt %p setpageprot %X sts %u\n", bptaddr, vpage, sts);

  return (rc);
}

/* Halt all the other threads in the process - we do this by queuing express 'haltast's to them, causing them to call oz_sys_debug_halted */
/* We cannot simply loop through udb->threads because there may be new threads we don't know about yet                                    */

static void udb_halt (void *cp)

{
  uLong sts;
  OZ_Handle h_thread, h_thread_next;
  OZ_Handle_item items[2];
  void *thisthread, *threadaddr;

  items[0].size = sizeof h_thread_next;					/* this gets an handle to the next thread in the process */
  items[0].buff = &h_thread_next;
  items[0].code = OZ_HANDLE_CODE_THREAD_FIRST;
  items[0].rlen = NULL;
  items[1].size = sizeof threadaddr;					/* this is the address of the kernel mode object */
  items[1].buff = &threadaddr;
  items[1].code = OZ_HANDLE_CODE_OBJADDR;
  items[1].rlen = NULL;

  thisthread = getthreadaddr (0);					/* get address of kernel mode object for this thread */

  sts = oz_sys_handle_getinfo (0, 1, items, NULL);			/* get handle to first thread in process */
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
  items[0].code = OZ_HANDLE_CODE_THREAD_NEXT;

  while ((h_thread = h_thread_next) != 0) {
    sts = oz_sys_handle_getinfo (h_thread, 2, items, NULL);		/* get kernel mode address of thread object, get handle to next thread in process */
    if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
    if (threadaddr != thisthread) {					/* don't wack this thread */
      sts = oz_sys_thread_queueast (OZ_PROCMODE_KNL, h_thread, haltast, cp, 1, OZ_SUCCESS); /* some other thread, wack it */
      if (sts != OZ_SUCCESS) udb_print (cp, "oz_usr_debug udb_halt: error %u queuing ast to thread\n", sts);
    }
    oz_sys_handle_release (OZ_PROCMODE_KNL, h_thread);			/* release handle to old thread */
  }
}

/* This routine is called on halted threads to check to see if someone is trying to alert the debugger, ie, has control-P been pressed? */

static int udb_debugchk (void *cp)

{
  Long oldvalue;
  uLong sts;
  Udb *udb;

  udb = cp;

  /* Read the event flag.  Oldvalue will be 0 if ^P not seen, and it will be 1 if it has. */

  sts = oz_sys_event_inc (OZ_PROCMODE_KNL, udb -> h_alert, 0, &oldvalue);
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
  return (oldvalue);
}

/* Read a command from the console input device */

static int udb_getcon (void *cp, uLong size, char *buff)

{
  char prompt[PROMPTSIZE+4];
  uLong rlen, sts;
  OZ_IO_fs_readrec fs_readrec;
  Udb *udb;

  udb = cp;

  prompt[0] = '\n';
  getprompt (udb, prompt + 1);
  strcat (prompt, "> ");

  memset (&fs_readrec, 0, sizeof fs_readrec);
  fs_readrec.size    = size - 1;
  fs_readrec.buff    = buff;
  fs_readrec.trmsize = 1;
  fs_readrec.trmbuff = "\n";
  fs_readrec.rlen    = &rlen;
  fs_readrec.pmtsize = strlen (prompt);
  fs_readrec.pmtbuff = prompt;

  sts = oz_sys_io (OZ_PROCMODE_KNL, udb -> h_input, 0, OZ_IO_FS_READREC, sizeof fs_readrec, &fs_readrec);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printerror ("oz_usr_debug udb_getcon: error %u reading command\n", sts);
    buff[0] = 0;
    return (0);
  }

  buff[rlen] = 0;
  return (1);
}

/* Print out an address, symbolically if possible */

static void udb_printaddr (void *cp, void *addr)

{
  udb_print (cp, "%8.8x", addr);
}

/* Print out disassembly of instruction, return number of bytes the instruction uses up */

static int udb_printinstr (void *cp, uByte *pc)

{
  uByte opcode;
  char outbuf[128];
  int len;
  Udb *udb;

  udb = cp;

  len = 1 << OZ_HW_L2PAGESIZE;						/* assume instruction is less than a page long */
  if (readmem (&opcode, sizeof opcode, pc) != OZ_SUCCESS) return (-1);	/* make sure initial location is readable */
  if (readmem (&opcode, sizeof opcode, pc + len - 1) != OZ_SUCCESS) {	/* see if byte a page later is readable */
    len -= (len - 1) && (OZ_Pointer)pc;					/* it isn't, then just use to the end of the page it is in */
    if (readmem (&opcode, sizeof opcode, pc + len - 1) != OZ_SUCCESS) return (-1); /* hopefully that is readable */
  }
  len = oz_sys_disassemble (len, pc, pc, sizeof outbuf, outbuf, NULL);	/* convert to string in outbuf */
  udb_print (cp, "%s", outbuf);						/* print it */
  return (len);								/* return how many bytes the opcode took up */
}

/* Convert a symbolic string to an address */

static int udb_cvtsymbol (void *cp, char *name, OZ_Pointer *symaddr, int *symsize)

{
  /* Not found, failure */

  return (0);
}

/* Read memory not known to be readable */

static int udb_readmem (void *cp, void *buff, uLong size, void *addr)

{
  uLong sts;

  sts = readmem (buff, size, addr);
  return (sts == OZ_SUCCESS);
}

static uLong readmem (void *buff, uLong size, void *addr)

{
  Rwmpb rwmpb;

  rwmpb.buff = buff;
  rwmpb.size = size;
  rwmpb.addr = addr;

  return (oz_sys_condhand_try (readmem_try, &rwmpb, oz_sys_condhand_rtnanysig, NULL));
}

static uLong readmem_try (void *rwmpbv)

{
  Rwmpb *rwmpb;

  rwmpb = rwmpbv;
  memcpy (rwmpb -> buff, rwmpb -> addr, rwmpb -> size);
  return (OZ_SUCCESS);
}

/* Write memory not known to be writable */

static int udb_writemem (void *cp, void *buff, uLong size, void *addr)

{
  uLong sts;

  sts = writemem (buff, size, addr);
  return (sts == OZ_SUCCESS);
}

static uLong writemem (void *buff, uLong size, void *addr)

{
  Rwmpb rwmpb;

  rwmpb.buff = buff;
  rwmpb.size = size;
  rwmpb.addr = addr;

  return (oz_sys_condhand_try (writemem_try, &rwmpb, oz_sys_condhand_rtnanysig, NULL));
}

static uLong writemem_try (void *rwmpbv)

{
  Rwmpb *rwmpb;

  rwmpb = rwmpbv;
  memcpy (rwmpb -> addr, rwmpb -> buff, rwmpb -> size);
  return (OZ_SUCCESS);
}

/* Lock breakpoint database */

static void udb_lockbreak (void *cp)

{
  Long oldvalue;
  uLong sts;
  Udb *udb;

  udb = cp;

  while (1) {
    sts = oz_sys_event_set (OZ_PROCMODE_KNL, udb -> h_breaklock, 0, &oldvalue);
    if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
    if (oldvalue > 0) return;
    sts = oz_sys_event_wait (OZ_PROCMODE_KNL, udb -> h_breaklock, 0);
    if ((sts != OZ_ASTDELIVERED) && (sts != OZ_FLAGWASSET) && (sts != OZ_FLAGWASCLR)) oz_sys_condhand_signal (2, sts, 0);
  }
}

/* Lock print database */

static void udb_lockprint (void *cp)

{
  Long oldvalue;
  uLong sts;
  Udb *udb;

  udb = cp;

  while (1) {
    sts = oz_sys_event_set (OZ_PROCMODE_KNL, udb -> h_printlock, 0, &oldvalue);
    if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
    if (oldvalue > 0) return;
    sts = oz_sys_event_wait (OZ_PROCMODE_KNL, udb -> h_printlock, 0);
    if ((sts != OZ_ASTDELIVERED) && (sts != OZ_FLAGWASSET) && (sts != OZ_FLAGWASCLR)) oz_sys_condhand_signal (2, sts, 0);
  }
}

/* Unlock breakpoint database */

static void udb_unlkbreak (void *cp)

{
  uLong sts;
  Udb *udb;

  udb = cp;

  sts = oz_sys_event_set (OZ_PROCMODE_KNL, udb -> h_breaklock, 1, NULL);
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
}

/* Unlock print database */

static void udb_unlkprint (void *cp)

{
  uLong sts;
  Udb *udb;

  udb = cp;

  sts = oz_sys_event_set (OZ_PROCMODE_KNL, udb -> h_printlock, 1, NULL);
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
}

/* Suspend execution until resumed */

static void udb_suspend (void *cp)

{
  Long i;
  uLong sts;
  Udb *udb;

  udb = cp;

  i = udb_getcur (cp);

  sts = oz_sys_event_wait (OZ_PROCMODE_KNL, udb -> threadsusp[i], 0);
  if ((sts != OZ_ASTDELIVERED) && (sts != OZ_FLAGWASCLR) && (sts != OZ_FLAGWASSET)) oz_sys_condhand_signal (2, sts, 0);
  sts = oz_sys_event_set  (OZ_PROCMODE_KNL, udb -> threadsusp[i], 0, NULL);
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
}

/* Resume execution of all suspended threads */

static void udb_resume (void *cp)

{
  Long i;
  uLong sts;
  Udb *udb;

  udb = cp;

  for (i = 0; i < MAXTHREADS; i ++) {
    if (udb -> threadsusp[i] == 0) continue;
    sts = oz_sys_event_set (OZ_PROCMODE_KNL, udb -> threadsusp[i], 1, NULL);
    if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
  }
}

/* The callback table */

static const OZ_Debug_callback cb = {
	udb_getcur, 
	udb_print, 
	udb_abort, 
	udb_readbpt, 
	udb_writebpt, 
	udb_halt, 
	udb_debugchk, 
	udb_getcon, 
	udb_printaddr, 
	udb_printinstr, 
	udb_cvtsymbol, 
	udb_readmem, 
	udb_writemem, 
	udb_lockbreak, 
	udb_lockprint, 
	udb_unlkbreak, 
	udb_unlkprint, 
	udb_suspend, 
	udb_resume, 
	oz_hw_mchargs_des, 
	oz_hw_mchargx_usr_des, 
	sizeof (OZ_Mchargx_usr)
};

/************************************************************************/
/*									*/
/*  Initialization routine						*/
/*									*/
/*    Input:								*/
/*									*/
/*	entry = program's entrypoint					*/
/*	param = parameter to pass to program				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_usr_debug_init = program's status value			*/
/*									*/
/*    Note:								*/
/*									*/
/*	Input is taken from OZ_DEBUG_INPUT and output goes to 		*/
/*	OZ_DEBUG_OUTPUT.  If OZ_DEBUG_INPUT is not defined, OZ_INPUT 	*/
/*	is used.  If OZ_DEBUG_OUTPUT is not defined, OZ_OUTPUT is 	*/
/*	used.								*/
/*									*/
/************************************************************************/

uLong oz_usr_debug_init (uLong (*entry) (void *param), void *param)

{
  char *lnm, prompt[PROMPTSIZE];
  int i, rc;
  uLong sts;
  Udb udb;

  memset (&udb, 0, sizeof udb);

  /* Get the breakpoint, print data and thread lock event flags and mark them unlocked */

  sts = oz_sys_event_create (OZ_PROCMODE_KNL, "udb breaklock", &(udb.h_breaklock));
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
  sts = oz_sys_event_create (OZ_PROCMODE_KNL, "udb printlock", &(udb.h_printlock));
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
  udb_unlkbreak (&udb);
  udb_unlkprint (&udb);

  sts = oz_sys_event_create (OZ_PROCMODE_KNL, "udb threadlock", &udb.h_threadlock);
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
  sts = oz_sys_event_set (OZ_PROCMODE_KNL, udb.h_threadlock, 1, NULL);
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);

  /* Get handles to input and output streams */

  sts = getchan ("OZ_DEBUG_INPUT", "OZ_INPUT", &udb.h_input);
  if (sts != OZ_SUCCESS) return (sts);
  sts = getchan ("OZ_DEBUG_OUTPUT", "OZ_ERROR", &udb.h_output);
  if (sts != OZ_SUCCESS) return (sts);

  /* Put this thread in the thread database */

  udb_getcur (&udb);

  /* Arm the 'alert' event flag and ast */

  sts = oz_sys_event_create (OZ_PROCMODE_KNL, "udb alert", &udb.h_alert);
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
  armalert (&udb);

  /* Initialize debugger routines and run the program */

  getprompt (&udb, prompt);
  rc = oz_sys_debug_init (prompt, &cb, &udb, sizeof udb.dc, udb.dc);
  sts = OZ_BADBUFFERSIZE;
  if (rc != 0) oz_sys_io_fs_printerror ("oz_usr_debug_init: dc size is %d, should be %d\n", sizeof udb.dc, rc);
  else {
    udb.entry = entry;
    udb.param = param;
    sts = oz_sys_condhand_try (tryit, &udb, exception, &udb);
  }

  /* Release all handles we have set up */

  oz_sys_handle_release (OZ_PROCMODE_KNL, udb.h_breaklock);
  oz_sys_handle_release (OZ_PROCMODE_KNL, udb.h_printlock);
  oz_sys_handle_release (OZ_PROCMODE_KNL, udb.h_input);
  oz_sys_handle_release (OZ_PROCMODE_KNL, udb.h_output);
  oz_sys_handle_release (OZ_PROCMODE_KNL, udb.h_alert);
  oz_sys_handle_release (OZ_PROCMODE_KNL, udb.h_threadlock);

  for (i = 0; i < MAXTHREADS; i ++) {
    if (udb.h_threads[i]  != 0) oz_sys_handle_release (OZ_PROCMODE_KNL, udb.h_threads[i]);
    if (udb.threadsusp[i] != 0) oz_sys_handle_release (OZ_PROCMODE_KNL, udb.threadsusp[i]);
  }

  /* Return with program's status */

  return (sts);
}

/* Get an I/O channel for debugger input or output */

static uLong getchan (char *dname, char *name, OZ_Handle *h_r)

{
  char *lnm;
  uLong sts;
  OZ_Handle h_defaulttbl, h_logname;

  /* Lookup logical name default table */

  sts = oz_sys_logname_lookup (0, OZ_PROCMODE_USR, "OZ_DEFAULT_TBL", NULL, NULL, NULL, &h_defaulttbl);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printerror ("oz_usr_debug_init: error %u looking up OZ_DEFAULT_TBL\n", sts);
    return (sts);
  }

  /* Look up first the dname, then the name if dname was not found */

  lnm = dname;
  sts = oz_sys_logname_lookup (h_defaulttbl, OZ_PROCMODE_USR, lnm, NULL, NULL, NULL, &h_logname);
  if (sts == OZ_NOLOGNAME) {
    lnm = name;
    sts = oz_sys_logname_lookup (h_defaulttbl, OZ_PROCMODE_USR, lnm, NULL, NULL, NULL, &h_logname);
  }
  oz_sys_handle_release (OZ_PROCMODE_KNL, h_defaulttbl);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printerror ("oz_usr_debug_init: error %u looking up %s\n", sts, lnm);
    return (sts);
  }

  /* Now get the I/O channel from the logical name and assign an handle to it */

  sts = oz_sys_logname_getval (h_logname, 0, NULL, 0, NULL, NULL, h_r, OZ_OBJTYPE_IOCHAN, NULL);
  oz_sys_handle_release (OZ_PROCMODE_KNL, h_logname);
  if (sts != OZ_SUCCESS) oz_sys_io_fs_printerror ("oz_usr_debug_init: error %u getting %s channel\n", sts, lnm);

  return (sts);
}

/* This routine establishes a control-P ast on the input channel */

static void armalert (Udb *udb)

{
  uLong sts;
  OZ_IO_console_ctrlchar console_ctrlchar;

  sts = oz_sys_event_set (OZ_PROCMODE_KNL, udb -> h_alert, 0, NULL);		/* clear the event flag saying 'no alarm pending' */
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);

  sts = oz_sys_event_ast (OZ_PROCMODE_KNL, udb -> h_alert, alertast, udb, 1);	/* arm an express ast to happen when event flag gets set */
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);

  memset (&console_ctrlchar, 0, sizeof console_ctrlchar);			/* set up control block to set event when control-P is detected */
  console_ctrlchar.mask[0]   = 1 << (ALERTCHAR & 31);
  console_ctrlchar.wiperah   = 1;
  console_ctrlchar.terminal  = 1;
  console_ctrlchar.abortread = 1;

  sts = oz_sys_io_start (OZ_PROCMODE_KNL, udb -> h_input, &(udb -> alertsts), udb -> h_alert, NULL, NULL, OZ_IO_CONSOLE_CTRLCHAR, sizeof console_ctrlchar, &console_ctrlchar);
  if ((sts != OZ_STARTED) && (sts != OZ_BADIOFUNC)) udb_print (udb, "oz_usr_debug armalert: error %u arming alert ast\n", sts);
}

/* This express ast routine is called when a control-P ast is received.  This is how the main thread gets interrupted */
/* by control-P.  Then when it calls oz_sys_debug_exception, that will call udb_halt to interrupt the other threads.  */

static void alertast (void *cp, uLong status, OZ_Mchargs *mchargs)

{
  char prompt[PROMPTSIZE];
  OZ_Mchargx_usr mchargx_usr;
  Udb *udb;

  udb = cp;

  if ((status != OZ_SUCCESS) && (status != OZ_FLAGWASCLR) && (status != OZ_FLAGWASSET)) udb_print (udb, "oz_usr_debug alertast: error %u\n", status);
  else {
    oz_hw_mchargx_usr_fetch (&mchargx_usr);
    getprompt (udb, prompt);
    oz_sys_debug_exception (prompt, udb -> dc, NULL, mchargs, &mchargx_usr);
    armalert (udb);
    oz_hw_mchargx_usr_store (&mchargx_usr, NULL);
  }
}

static uLong tryit (void *cp)

{
  Udb *udb;
  uLong sts;

  udb = cp;

  oz_hw_debug_bpt ();							/* do a breakpoint to call debugger initially */
  sts = (*(udb -> entry)) (udb -> param);				/* now call the program */
  return (sts);
}

/* This routine is called as the result of an exception in any thread.  So it will call oz_sys_debug_exception which */
/* will make it the 'command' cpu, and it will call udb_halt to interrupt all the other threads in the process.      */

static uLong exception (void *cp, OZ_Sigargs *sigargs, OZ_Mchargs *mchargs)

{
  char prompt[PROMPTSIZE];
  int rc;
  OZ_Mchargx_usr mchargx_usr;
  Udb *udb;

  udb = cp;

  /**udb_print (cp, "udb exception: sigargs[1]=%u, mchargs->eip=%p\n", sigargs[1], mchargs -> eip);**/

  oz_hw_mchargx_usr_fetch (&mchargx_usr);
  getprompt (udb, prompt);
  rc = oz_sys_debug_exception (prompt, udb -> dc, sigargs, mchargs, &mchargx_usr);
  oz_hw_mchargx_usr_store (&mchargx_usr, NULL);
  return (rc ? OZ_RESUME : OZ_RESIGNAL);
}

/* This express ast routine is called when the threads are halted via udb_halt call - */
/* It calls the debugger to process it - they will typically wait for                 */
/* either an 'x' command or for the user to resume execution on all threads           */

static void haltast (void *cp, uLong status, OZ_Mchargs *mchargs)

{
  char prompt[PROMPTSIZE];
  OZ_Mchargx_usr mchargx_usr;
  Udb *udb;

  udb = cp;
  oz_hw_mchargx_usr_fetch (&mchargx_usr);
  getprompt (udb, prompt);
  oz_sys_debug_halted (prompt, udb -> dc, mchargs, &mchargx_usr);
  oz_hw_mchargx_usr_store (&mchargx_usr, NULL);
}

/************************************************************************/
/*									*/
/*  Get kernel object address of current thread.  This is used to 	*/
/*  compare two handles to see if they point to the same thread.	*/
/*									*/
/************************************************************************/

static void *getthreadaddr (OZ_Handle h_thread)

{
  uLong sts;
  OZ_Handle h_t;
  OZ_Handle_item items[1];
  void *threadaddr;

  /* If supplied handle is zero, assign an handle to current thread */

  if ((h_t = h_thread) == 0) {
    items[0].size = sizeof h_thread;
    items[0].buff = &h_thread;
    items[0].code = OZ_HANDLE_CODE_THREAD_HANDLE;
    items[0].rlen = NULL;
    sts = oz_sys_handle_getinfo (0, 1, items, NULL);
    if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
  }

  /* Get the thread object's kernel mode address */

  items[0].size = sizeof threadaddr;
  items[0].buff = &threadaddr;
  items[0].code = OZ_HANDLE_CODE_OBJADDR;
  items[0].rlen = NULL;
  sts = oz_sys_handle_getinfo (h_t, 1, items, NULL);
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);

  /* Release thread handle */

  if (h_thread == 0) oz_sys_handle_release (OZ_PROCMODE_KNL, h_t);

  /* Return thread address */

  return (threadaddr);
}

/************************************************************************/
/*									*/
/*  Get prompt string for this thread					*/
/*									*/
/************************************************************************/

static void getprompt (void *cp, char *buff)

{
  char threadname[OZ_THREAD_NAMESIZE];
  uLong sts;
  OZ_Handle_item items[1];

  items[0].size = sizeof threadname;
  items[0].buff = threadname;
  items[0].code = OZ_HANDLE_CODE_THREAD_NAME;
  items[0].rlen = NULL;
  sts = oz_sys_handle_getinfo (0, 1, items, NULL);
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);

  oz_sys_sprintf (PROMPTSIZE, buff, "oz_usr_debug %d (%s)", udb_getcur (cp), threadname);
}
