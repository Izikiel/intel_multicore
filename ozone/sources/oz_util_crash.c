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
/*  This utility controls the crash dump file				*/
/*									*/
/*    Analyze a crash file:						*/
/*	crash analyze <crashfile>					*/
/*									*/
/*    Create a file:							*/
/*	crash create <filename> <size>					*/
/*									*/
/*    Force a crash:							*/
/*	crash dump							*/
/*									*/
/*    Set the file:							*/
/*	crash file <filename>						*/
/*									*/
/*    Set the mode:							*/
/*	crash mode { debug | dump | reboot }				*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_io_fs.h"
#include "oz_knl_crash.h"
#include "oz_knl_devio.h"
#include "oz_knl_hw.h"
#include "oz_knl_lock.h"
#include "oz_knl_sdata.h"
#include "oz_knl_section.h"
#include "oz_knl_status.h"
#include "oz_sys_debug.h"
#include "oz_sys_handle.h"
#include "oz_sys_io.h"
#include "oz_sys_io_fs.h"
#include "oz_sys_process.h"
#include "oz_sys_section.h"
#include "oz_sys_thread.h"
#include "oz_util_start.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static char *pn;
static OZ_Handle h_iochan;

static uLong crash_analyze (int argc, char *argv[]);
static uLong crash_create (int argc, char *argv[]);
static uLong crash_dump (int argc, char *argv[]);
static uLong crash_dump_knl (OZ_Procmode cprocmode, void *dummy);
static uLong crash_file (int argc, char *argv[]);
static uLong crash_file_knl (OZ_Procmode cprocmode, void *dummy);
static uLong crash_mode (int argc, char *argv[]);
static uLong crash_mode_knl (OZ_Procmode cprocmode, void *modev);

uLong oz_util_main (int argc, char *argv[])

{
  pn = "crash";
  if (argc > 0) pn = argv[0];

  if (argc > 1) {
    if (strcasecmp (argv[1], "analyze") == 0) return (crash_analyze (argc, argv));
    if (strcasecmp (argv[1], "create")  == 0) return (crash_create (argc, argv));
    if (strcasecmp (argv[1], "dump")    == 0) return (crash_dump (argc, argv));
    if (strcasecmp (argv[1], "file")    == 0) return (crash_file (argc, argv));
    if (strcasecmp (argv[1], "mode")    == 0) return (crash_mode (argc, argv));
  }

  oz_sys_io_fs_printf (oz_util_h_error, "%s: missing or invalid sub-command\n", pn);
  oz_sys_io_fs_printf (oz_util_h_error, "usage: %s analyze ... # analyze a crash dump file\n", pn);
  oz_sys_io_fs_printf (oz_util_h_error, "       %s create ...  # create crash dump file\n", pn);
  oz_sys_io_fs_printf (oz_util_h_error, "       %s dump        # force a crash\n", pn);
  oz_sys_io_fs_printf (oz_util_h_error, "       %s file ...    # set current crash dump file\n", pn);
  oz_sys_io_fs_printf (oz_util_h_error, "       %s mode ...    # set kernel exception mode\n", pn);
  return (OZ_UNKNOWNCOMMAND);
}

/************************************************************************/
/*									*/
/*  Analyze								*/
/*									*/
/************************************************************************/

static Long activecpu;				// cpu that's processing debugger commands
static OZ_Crash_block *crasheader;		// where the crash file's header is in our memory space
static OZ_Handle h_mainthread;			// handle to main thread
static OZ_Handle h_printlock;			// print locking event flag
static OZ_Handle h_suspends[OZ_HW_MAXCPUS];	// suspend event flags
static uByte debugctx[OZ_DEBUG_SIZE];		// debugger's internal context block

static void anal_print (void *cp, char *fmt, ...);
static uLong anal_crash_altcpu (void *cpuidxv);
static int anal_readmem (void *cp, void *buff, uLong size, void *addr);

/********************************************/
/* These are the debugger callback routines */
/********************************************/

/* Get index of current 'cpu' */
/* Each cpu has a thread      */

static Long anal_getcur (void *cp)

{
  char threadname[OZ_THREAD_NAMESIZE];
  Long cpuidx;
  uLong sts;

  sts = oz_sys_thread_getname (0, sizeof threadname, threadname);
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
  cpuidx = crasheader -> cpuidx;
  if (memcmp ("crash altcpu ", threadname, 13) == 0) cpuidx = atoi (threadname + 13);

  if (h_suspends[cpuidx] == 0) {
    sts = oz_sys_event_create (OZ_PROCMODE_KNL, "other virt cpu", h_suspends + cpuidx);
    if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
  }

  return (cpuidx);
}

/* Print a message on the debugger's output channel */

static void anal_print (void *cp, char *fmt, ...)

{
  va_list ap;

  va_start (ap, fmt);
  oz_sys_io_fs_vprintf (oz_util_h_output, fmt, ap);
  va_end (ap);
}

/* Abort the current thread (error message has already been output) */

static void anal_abort (void *cp, OZ_Sigargs *sigargs, OZ_Mchargs *mchargs, void *mchargx)

{
  oz_sys_thread_abort (h_mainthread, OZ_SUCCESS);
  oz_sys_thread_exit (OZ_SUCCESS);
}

/* Read the breakpoint at a given address (not known to be readable) */

static int anal_readbpt (void *cp, OZ_Breakpoint *bptaddr, OZ_Breakpoint *oldcontents)

{
  return (anal_readmem (cp, oldcontents, sizeof *oldcontents, bptaddr));
}

/* Write a breakpoint at a given address (not known to be readable) */

static char *anal_writebpt (void *cp, OZ_Breakpoint *bptaddr, OZ_Breakpoint opcode)

{
  return ("can't write breakpoints to crash dump");
}

/* Halt all the other CPU's                                                      */
/* We halt a cpu by creating a thread, then the thread calls oz_sys_debug_halted */

static void anal_halt (void *cp)

{
  char threadname[16];
  Long cpuidx, mycpuidx;
  OZ_Handle h_thread;
  uLong sts;

  mycpuidx = anal_getcur (cp);
  for (cpuidx = 0; cpuidx < OZ_HW_MAXCPUS; cpuidx ++) {
    if (cpuidx != mycpuidx) {
      oz_sys_sprintf (sizeof threadname, threadname, "crash altcpu %d", cpuidx);
      sts = oz_sys_thread_create (OZ_PROCMODE_KNL, 0, 0, 0, 0, 0, anal_crash_altcpu, (void *)(OZ_Pointer)cpuidx, OZ_ASTMODE_ENABLE, threadname, &h_thread);
      if (sts != OZ_SUCCESS) oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u creating thread '%s'\n", pn, sts, threadname);
      else oz_sys_handle_release (OZ_PROCMODE_KNL, h_thread);
    }
  }
}

/* This routine is called on halted threads to check to see if someone is trying to alert the debugger, ie, has control-P been pressed? */

static int anal_debugchk (void *cp)

{
  return (0);
}

/* Read a command from the console input device */

static int anal_getcon (void *cp, uLong size, char *buff)

{
  char prompt[20];
  OZ_IO_fs_readrec fs_readrec;
  uLong rlen, sts;

  oz_sys_sprintf (sizeof prompt, prompt, "\n%s %u> ", pn, anal_getcur (cp));

  memset (&fs_readrec, 0, sizeof fs_readrec);
  fs_readrec.size    = size - 1;
  fs_readrec.buff    = buff;
  fs_readrec.trmsize = 1;
  fs_readrec.trmbuff = "\n";
  fs_readrec.rlen    = &rlen;
  fs_readrec.pmtsize = strlen (prompt);
  fs_readrec.pmtbuff = prompt;

  sts = oz_sys_io (OZ_PROCMODE_KNL, oz_util_h_input, 0, OZ_IO_FS_READREC, sizeof fs_readrec, &fs_readrec);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printerror ("%s anal_getcon: error %u reading command\n", pn, sts);
    buff[0] = 0;
    return (0);
  }

  buff[rlen] = 0;
  return (1);
}

/* Print out an address, symbolically if possible */

static void anal_printaddr (void *cp, void *addr)

{
  anal_print (cp, "%8.8x", addr);
}

/* Print out disassembly of instruction, return number of bytes the instruction uses up */

static int anal_printinstr (void *cp, uByte *pc)

{
  uByte opcode;
  char outbuf[128];
  int len;

  len = 1 << OZ_HW_L2PAGESIZE;						/* assume instruction is less than a page long */
  if (anal_readmem (cp, &opcode, sizeof opcode, pc) != OZ_SUCCESS) return (-1);	/* make sure initial location is readable */
  if (anal_readmem (cp, &opcode, sizeof opcode, pc + len - 1) != OZ_SUCCESS) {	/* see if byte a page later is readable */
    len -= (len - 1) && (OZ_Pointer)pc;					/* it isn't, then just use to the end of the page it is in */
    if (anal_readmem (cp, &opcode, sizeof opcode, pc + len - 1) != OZ_SUCCESS) return (-1); /* hopefully that is readable */
  }
  len = oz_sys_disassemble (len, pc, pc, sizeof outbuf, outbuf, NULL);	/* convert to string in outbuf */
  anal_print (cp, "%s", outbuf);					/* print it */
  return (len);								/* return how many bytes the opcode took up */
}

/* Convert a symbolic string to an address */

static int anal_cvtsymbol (void *cp, char *name, OZ_Pointer *symaddr, int *symsize)

{
  /* Not found, failure */

  return (0);
}

/* Read memory not known to be readable */

#if defined (OZ_HW_TYPE_486)

#define MPDBASE 0x2000

	/* read a longword at a given physical address */

static uLong readphymem_long (uLong pa)

{
  if ((pa >> crasheader -> l2pagesize) >= crasheader -> numpages) return (0xFFFFFFFF);
  if ((pa >> crasheader -> l2pagesize) >= crasheader -> holebeg) {
    if ((pa >> crasheader -> l2pagesize) < crasheader -> holeend) return (0);
    pa -= (crasheader -> holeend - crasheader -> holebeg) << crasheader -> l2pagesize;
  }
  return (*(uLong *)(((uByte *)crasheader) + crasheader -> headersize + pa));
}

	/* read some memory given CR3 */

static int readvirtmem_cr3 (void *buff, uLong size, OZ_Pointer va, uLong cr3)

{
  uLong pa, pde, pte, toeop;

  while (size > 0) {
    pde = readphymem_long ((cr3 & 0xFFFFF000) + ((va >> 20) & 0xFFC));
    if (!(pde & 1)) return (0);
    if (pde & 0x80) {
      toeop = 0x400000 - (va & 0x3FFFFF);
      pa    = (pde & 0xFFC00000) + (va & 0x3FFFFF);
    } else {
      pte = readphymem_long ((pde & 0xFFFFF000) + ((va >> 10) & 0xFFC));
      if (!(pte & 1)) return (0);
      toeop = 0x1000 - (va & 0xFFF);
      pa    = (pte & 0xFFFFF000) + (va & 0xFFF);
    }
    if (toeop > size) toeop = size;
    if ((pa >> crasheader -> l2pagesize) >= crasheader -> numpages) {
      memset (buff, 0xFF, toeop);
    } else {
      if ((pa >> crasheader -> l2pagesize) >= crasheader -> holebeg) {
        if ((pa >> crasheader -> l2pagesize) < crasheader -> holeend) return (0);
        pa -= (crasheader -> holeend - crasheader -> holebeg) << crasheader -> l2pagesize;
      }
      memcpy (buff, ((uByte *)crasheader) + crasheader -> headersize + pa, toeop);
    }
    ((uByte *)buff) += toeop;
    size -= toeop;
    va   += toeop;
  }

  return (1);
}

	/* read some system global virtual memory */

static int readvirtmem_sys (void *buff, uLong size, OZ_Pointer va)

{
  return (readvirtmem_cr3 (buff, size, va, MPDBASE));
}

	/* read virtual memory given current cpu context */

static int anal_readmem (void *cp, void *buff, uLong size, void *addr)

{
  Long cpuidx;
  OZ_Mchargx_knl mchargx_knl, *mchargxkp;

  /* Figure out which cpu is current */

  cpuidx = anal_getcur (cp);

  /* Read the pointer to the mchargx_knl struct for the current cpu */

  if (!readvirtmem_sys (&mchargxkp, sizeof mchargxkp, (OZ_Pointer)(crasheader -> mchargx_knl + cpuidx))) return (0);

  /* Read the mchargx_knl struct for the current cpu */

  if (!readvirtmem_sys (&mchargx_knl, sizeof mchargx_knl, (OZ_Pointer)mchargxkp)) return (0);

  /* Using the current cpu's CR3, read the requested virtual memory */

  return (readvirtmem_cr3 (buff, size, (OZ_Pointer)addr, mchargx_knl.cr3));
}

#elif defined (OZ_HW_TYPE_AXP)

#define AXP_VATOVP(va) (((va) >> crasheader -> l2pagesize) & ((1 << (3 * crasheader -> l2pagesize - 9)) - 1))
#define AXP_PAGMSK ((1 << crasheader -> l2pagesize) - 1)

	/* read a longword at a given physical address */

static uQuad readphymem_quad (uQuad pa)

{
  if ((pa >> crasheader -> l2pagesize) >= crasheader -> numpages) return (0);
  return (*(uQuad *)(((uByte *)crasheader) + crasheader -> headersize + pa));
}

	/* read some memory given PTBR */

static int readvirtmem_ptbr (void *buff, uLong size, OZ_Pointer va, uLong ptbr)

{
  uLong l1idx, l2idx, l3idx, toeop;
  uQuad l1pte, l2pte, l3pte, pa;

  while (size > 0) {
    l3idx  = AXP_VATOVP (va);
    l2idx  = l3idx >> (crasheader -> l2pagesize - 3);
    l1idx  = l2idx >> (crasheader -> l2pagesize - 3);
    l3idx &= AXP_PAGMSK >> 3;
    l2idx &= AXP_PAGMSK >> 3;
    l1pte  = readphymem_quad ((((uQuad)ptbr) << crasheader -> l2pagesize) + (l1idx * 8));
    if (!(l1pte & 1)) return (0);
    l2pte  = readphymem_quad (((l1pte >> 32) << crasheader -> l2pagesize) + (l2idx * 8));
    if (!(l2pte & 1)) return (0);
    l3pte  = readphymem_quad (((l2pte >> 32) << crasheader -> l2pagesize) + (l3idx * 8));
    if (!(l3pte & 1)) return (0);
    pa     = ((l3pte >> 32) << crasheader -> l2pagesize) + (va & AXP_PAGMSK);
    toeop  = (1 << crasheader -> l2pagesize) - (va & AXP_PAGMSK);
    if (toeop > size) toeop = size;

    if ((pa >> crasheader -> l2pagesize) >= crasheader -> numpages) {
      memset (buff, 0xFF, toeop);
    } else {
      memcpy (buff, ((uByte *)crasheader) + crasheader -> headersize + pa, toeop);
    }
    ((uByte *)buff) += toeop;
    size -= toeop;
    va   += toeop;
  }

  return (1);
}

	/* read some system global virtual memory */

static int readvirtmem_sys (void *buff, uLong size, OZ_Pointer va)

{
  return (readvirtmem_ptbr (buff, size, va, crasheader -> mchargx_knl_cpy.ptbr_ro));
}

	/* read virtual memory given current cpu context */

static int anal_readmem (void *cp, void *buff, uLong size, void *addr)

{
  Long cpuidx;
  OZ_Mchargx_knl mchargx_knl, *mchargxkp;

  /* Figure out which cpu is current */

  cpuidx = anal_getcur (cp);

  /* Read the pointer to the mchargx_knl struct for the current cpu */

  if (!readvirtmem_sys (&mchargxkp, sizeof mchargxkp, (OZ_Pointer)(crasheader -> mchargx_knl + cpuidx))) return (0);

  /* Read the mchargx_knl struct for the current cpu */

  if (!readvirtmem_sys (&mchargx_knl, sizeof mchargx_knl, (OZ_Pointer)mchargxkp)) return (0);

  /* Using the current cpu's PTBR, read the requested virtual memory */

  return (readvirtmem_ptbr (buff, size, (OZ_Pointer)addr, mchargx_knl.ptbr_ro));
}

#else
  error : insert code for hardware here
#endif

/* Write memory not known to be writable */

static int anal_writemem (void *cp, void *buff, uLong size, void *addr)

{
  return (0);
}

/* Lock breakpoint database */

static void anal_lockbreak (void *cp)

{ }

/* Lock print database */

static void anal_lockprint (void *cp)

{
  Long oldvalue;
  uLong sts;

  while (1) {
    sts = oz_sys_event_set (OZ_PROCMODE_KNL, h_printlock, 0, &oldvalue);
    if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
    if (oldvalue > 0) return;
    sts = oz_sys_event_wait (OZ_PROCMODE_KNL, h_printlock, 0);
    if ((sts != OZ_ASTDELIVERED) && (sts != OZ_FLAGWASSET) && (sts != OZ_FLAGWASCLR)) oz_sys_condhand_signal (2, sts, 0);
  }
}

/* Unlock breakpoint database */

static void anal_unlkbreak (void *cp)

{ }

/* Unlock print database */

static void anal_unlkprint (void *cp)

{
  uLong sts;

  sts = oz_sys_event_set (OZ_PROCMODE_KNL, h_printlock, 1, NULL);
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
}

/* Suspend execution until resumed */

static void anal_suspend (void *cp)

{
  Long cpuidx;
  uLong sts;

  cpuidx = anal_getcur (cp);

  sts = oz_sys_event_wait (OZ_PROCMODE_KNL, h_suspends[cpuidx], 0);
  if ((sts != OZ_ASTDELIVERED) && (sts != OZ_FLAGWASCLR) && (sts != OZ_FLAGWASSET)) oz_sys_condhand_signal (2, sts, 0);
  sts = oz_sys_event_set  (OZ_PROCMODE_KNL, h_suspends[cpuidx], 0, NULL);
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
}

/* Resume execution of all suspended threads */

static void anal_resume (void *cp)

{
  Long cpuidx;
  uLong sts;

  for (cpuidx = 0; cpuidx < OZ_HW_MAXCPUS; cpuidx ++) {
    if (h_suspends[cpuidx] != 0) {
      sts = oz_sys_event_set (OZ_PROCMODE_KNL, h_suspends[cpuidx], 1, NULL);
      if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
    }
  }
}

/* The callback table */

static const OZ_Debug_callback anal_callback = {
	anal_getcur, 
	anal_print, 
	anal_abort, 
	anal_readbpt, 
	anal_writebpt, 
	anal_halt, 
	anal_debugchk, 
	anal_getcon, 
	anal_printaddr, 
	anal_printinstr, 
	anal_cvtsymbol, 
	anal_readmem, 
	anal_writemem, 
	anal_lockbreak, 
	anal_lockprint, 
	anal_unlkbreak, 
	anal_unlkprint, 
	anal_suspend, 
	anal_resume, 
	oz_hw_mchargs_des, 
	oz_hw_mchargx_knl_des, 
	sizeof (OZ_Mchargx_knl)
};

/**************************************************************************/
/* This routine gets called as a thread to emulate the non-crashing CPU's */
/**************************************************************************/

static uLong anal_crash_altcpu (void *cpuidxv)

{
  char prompt[16];
  Long cpuidx;
  OZ_Mchargs *mchargs;
  OZ_Mchargx_knl *mchargx_knl;

  /* See which CPU we are emulating */

  cpuidx = (Long)(OZ_Pointer)cpuidxv;

  /* Try to read the corresponding mchargs pointer from the array */

  if (!readvirtmem_sys (&mchargs, sizeof mchargs, (OZ_Pointer)(crasheader -> mchargs + cpuidx))) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: can't read mchargs ptr[%d] from %p\n", pn, cpuidx, crasheader -> mchargs + cpuidx);
    return (OZ_FILECORRUPT);
  }

  /* If the pointer is NULL, there was no CPU there or it did not halt */

  if (mchargs == NULL) return (OZ_SUCCESS);

  /* We should be able to read the corresponding mchargx_knl pointer, too */

  if (!readvirtmem_sys (&mchargx_knl, sizeof mchargx_knl, (OZ_Pointer)(crasheader -> mchargx_knl + cpuidx))) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: can't read mchargx_knl ptr[%d] from %p\n", pn, cpuidx, crasheader -> mchargx_knl + cpuidx);
    return (OZ_FILECORRUPT);
  }

  /* OK, call the debugger so it can do this CPU, too */

  oz_sys_sprintf (sizeof prompt, prompt, "%s %d", pn, cpuidx);
  oz_sys_debug_halted (prompt, debugctx, mchargs, mchargx_knl);
  return (OZ_SUCCESS);
}

/****************************************************/
/* This is the main command line processing routine */
/****************************************************/

static uLong mchargs_print (void *dummy, const char *format, ...);

static uLong crash_analyze (int argc, char *argv[])

{
  char devname[OZ_DEVUNIT_NAMESIZE], *ermsg, *filename, *p, prompt[16], rnamebuff[OZ_FS_MAXFNLEN];
  int i, j, rc;
  Long crashcpu;
  OZ_Crash_block *crasheader2;
  OZ_Handle h_section;
  OZ_IO_fs_open fs_open;
  OZ_Mchargs *mchargs;
  OZ_Mchargx_knl *mchargx_knl;
  OZ_Mempage cdpages, npagem, svpage;
  OZ_Sigargs *sigargs, sigargs0;
  uLong sts;

  filename = NULL;

  for (i = 2; i < argc; i ++) {
    ermsg = "unknown option";
    if (argv[i][0] == '-') goto usage;
    if (filename == NULL) {
      filename = argv[i];
      continue;
    }
    ermsg = "extra parameters";
    goto usage;
  }
  ermsg = "missing file name";
  if (filename == NULL) goto usage;

  /* Open the file */

  memset (&fs_open, 0, sizeof fs_open);
  fs_open.name      = filename;
  fs_open.lockmode  = OZ_LOCKMODE_CR;
  fs_open.rnamesize = sizeof rnamebuff;
  fs_open.rnamebuff = rnamebuff;
  sts = oz_sys_io_fs_open (sizeof fs_open, &fs_open, 0, &h_iochan);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u opening file %s\n", pn, sts, filename);
    return (sts);
  }
  oz_sys_iochan_getunitname (h_iochan, (sizeof devname) - 1, devname);
  strcat (devname, ":");

  /* Map it to memorie */

  sts = oz_sys_section_create (OZ_PROCMODE_KNL, h_iochan, 0, 1, 0, &h_section);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u creating %s%s section\n", pn, sts, devname, rnamebuff);
    return (sts);
  }

  npagem = 0;
  svpage = OZ_HW_VADDRTOVPAGE (&svpage);
  sts    = oz_sys_process_mapsection (OZ_PROCMODE_KNL, h_section, &npagem, &svpage, 0, OZ_HW_PAGEPROT_UR);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u mapping %s%s to memory\n", pn, sts, devname, rnamebuff);
    return (sts);
  }

  oz_sys_io_fs_printf (oz_util_h_error, "%s: %s%s mapped, %u pages at vpage %X\n", pn, devname, rnamebuff, npagem, svpage);

  crasheader = OZ_HW_VPAGETOVADDR (svpage);
  crashcpu   = crasheader -> cpuidx;

  /* Make sure the header is valid and the dump completed */

  if (strncmp (crasheader -> magic, "oz_crash", sizeof crasheader -> magic) != 0) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: bad magic number in header\n", pn);
    goto badheader;
  }

  if (crasheader -> version != 1) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: bad version %u\n", pn, crasheader -> version);
    goto badheader;
  }

  if (crasheader -> headersize < sizeof *crasheader) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: bad header size %u\n", pn, crasheader -> headersize);
    goto badheader;
  }

  cdpages  = crasheader -> numpages + crasheader -> holebeg - crasheader -> holeend;
  cdpages += (2 * crasheader -> headersize + (1 << crasheader -> l2pagesize) - 1) >> crasheader -> l2pagesize;
  if (cdpages > npagem) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: dump length of %u pages exceeds file size of %u pages\n", cdpages, npagem);
    goto badheader;
  }

  crasheader2 = (void *)(((OZ_Pointer)crasheader) + crasheader -> headersize 
                       + ((crasheader -> numpages + crasheader -> holebeg - crasheader -> holeend) << crasheader -> l2pagesize));

  if (memcmp (crasheader, crasheader2, sizeof *crasheader) != 0) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: crash dump did not complete\n", pn);
    goto badheader;
  }

  /* Print out various things about crash */

  /* - when it happened */

  oz_sys_io_fs_printf (oz_util_h_output, "Crash date: %t\n", crasheader -> when);  

  /* - the sigargs */

  if (crasheader -> sigargs == NULL) {
    oz_sys_io_fs_printf (oz_util_h_output, "Signal args NULL\n");
  } else {
    if (!anal_readmem (crasheader, &sigargs0, sizeof sigargs0, crasheader -> sigargs)) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: can't read sigargs[0] from %p\n", pn, crasheader -> sigargs);
      goto badheader;
    }
    sigargs = malloc ((sigargs0 + 1) * sizeof *sigargs);
    if (!anal_readmem (crasheader, sigargs, (sigargs0 + 1) * sizeof *sigargs, crasheader -> sigargs)) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: can't read sigargs[0..%u] from %p\n", pn, sigargs0, crasheader -> sigargs);
      goto badheader;
    }
    oz_sys_io_fs_printf (oz_util_h_output, "Signal args (at %p):\n", crasheader -> sigargs);
    for (i = 0; i <= sigargs0; i ++) {
      oz_sys_io_fs_printf (oz_util_h_output, "  %u/%X", sigargs[i], sigargs[i]);
    }
    oz_sys_io_fs_printf (oz_util_h_output, "\n");
  }

  /* - the mchargs */

  if (!anal_readmem (crasheader, &mchargs, sizeof mchargs, crasheader -> mchargs + crashcpu)) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: can't read mchargs ptr[%d] from %p\n", pn, crashcpu, crasheader -> mchargs + crashcpu);
    goto badheader;
  }

  if (!anal_readmem (crasheader, &mchargx_knl, sizeof mchargx_knl, crasheader -> mchargx_knl + crashcpu)) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: can't read mchargx_knl ptr[%d] from %p\n", pn, crashcpu, crasheader -> mchargx_knl + crashcpu);
    goto badheader;
  }

  oz_sys_io_fs_printf (oz_util_h_output, "Machine args (at %p):\n", mchargs);
  oz_hw_mchargs_print (mchargs_print, NULL, 1, &(crasheader -> mchargs_cpy));

  /* Create printlock event flag so more than one thread won't try to write output at same time */

  sts = oz_sys_event_create (OZ_PROCMODE_KNL, "printlock", &h_printlock);
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
  sts = oz_sys_event_set (OZ_PROCMODE_KNL, h_printlock, 1, NULL);
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);

  /* Call debugger to analyze dump */

  sts = oz_sys_thread_getbyid (0, &h_mainthread);
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
  activecpu = crasheader -> cpuidx;
  oz_sys_sprintf (sizeof prompt, prompt, "%s %d", pn, crashcpu);
  rc = oz_sys_debug_init (prompt, &anal_callback, NULL, sizeof debugctx, debugctx);
  if (rc != 0) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: param block size must be at least %d\n", pn, rc);
    return (OZ_BUGCHECK);
  }
  oz_sys_debug_exception (prompt, debugctx, sigargs, mchargs, mchargx_knl);

  /* All done */

  return (OZ_SUCCESS);

  /* Something missing from command line, output usage message */

usage:
  oz_sys_io_fs_printf (oz_util_h_error, "%s: %s '%s'\n", pn, ermsg, ((i >= argc) || (argv[i] == NULL)) ? "" : argv[i]);
  oz_sys_io_fs_printf (oz_util_h_error, "usage: %s analyze <filename>\n", pn);
  return (OZ_MISSINGPARAM);

  /* Something wrong with header, dump it out and return error status */

badheader:
  oz_sys_io_fs_dumpmem (oz_util_h_error, sizeof *crasheader, crasheader);
  return (OZ_FILECORRUPT);
}

static uLong mchargs_print (void *dummy, const char *format, ...)

{
  uLong sts;
  va_list ap;

  va_start (ap, format);
  sts = oz_sys_io_fs_vprintf (oz_util_h_output, format, ap);
  va_end (ap);
  return (sts);
}

static uLong crash_create (int argc, char *argv[])

{
  char devname[OZ_DEVUNIT_NAMESIZE], *ermsg, *filename, *p, rnamebuff[OZ_FS_MAXFNLEN];
  int i, j;
  OZ_Dbn filesize;
  OZ_IO_fs_create fs_create;
  OZ_IO_fs_extend fs_extend;
  uLong sts;

  filename = NULL;
  filesize = 0;

  for (i = 2; i < argc; i ++) {
    ermsg = "unknown option";
    if (argv[i][0] == '-') goto usage;
    if (filename == NULL) {
      filename = argv[i];
      continue;
    }
    if (filesize == 0) {
      filesize = oz_hw_atoi (argv[i], &j);
      ermsg = "bad file size";
      if ((j == 0) || (argv[i][j] != 0)) goto usage;
      continue;
    }
    ermsg = "extra parameters";
    goto usage;
  }
  ermsg = "missing file name";
  if (filename == NULL) goto usage;
  ermsg = "missing file size";
  if (filesize == 0) goto usage;

  /* Create the file */

  memset (&fs_create, 0, sizeof fs_create);
  fs_create.name      = filename;
  fs_create.lockmode  = OZ_LOCKMODE_PW;
  fs_create.rnamesize = sizeof rnamebuff;
  fs_create.rnamebuff = rnamebuff;
  sts = oz_sys_io_fs_create (sizeof fs_create, &fs_create, 0, &h_iochan);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u creating file %s\n", pn, sts, filename);
    return (sts);
  }
  oz_sys_iochan_getunitname (h_iochan, (sizeof devname) - 1, devname);
  strcat (devname, ":");

  /* Extend file to desired size */

  memset (&fs_extend, 0, sizeof fs_extend);
  fs_extend.nblocks  = filesize;
  fs_extend.eofblock = filesize + 1;
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_iochan, 0, OZ_IO_FS_EXTEND, sizeof fs_extend, &fs_extend);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u extending %s%s to %u\n", pn, sts, devname, rnamebuff, filesize);
    return (sts);
  }

  /* Successful */

  oz_sys_io_fs_printf (oz_util_h_error, "%s: crash file %s%s created\n", pn, devname, rnamebuff);
  return (sts);

  /* Something missing from command line, output usage message */

usage:
  oz_sys_io_fs_printf (oz_util_h_error, "%s: %s '%s'\n", pn, ermsg, ((i >= argc) || (argv[i] == NULL)) ? "" : argv[i]);
  oz_sys_io_fs_printf (oz_util_h_error, "usage: %s create <filename> <filesize>\n", pn);
  return (OZ_MISSINGPARAM);
}

static uLong crash_dump (int argc, char *argv[])

{
  uLong sts;

  sts = oz_sys_callknl (crash_dump_knl, NULL);
  if (sts != OZ_SUCCESS) oz_sys_io_fs_printf (oz_util_h_error, "%s dump: error %u dumping\n", pn, sts);
  else oz_sys_io_fs_printf (oz_util_h_error, "%s: dump complete\n", pn);
  return (sts);
}

static uLong crash_dump_knl (OZ_Procmode cprocmode, void *dummy)

{
  oz_s_loadparams.knl_exception = 1;				/* set to do a crash dump on kernel exception */
  oz_crash ("oz_util_crash dump: operator requested crash");	/* cause a breakpoint exception */
  return (OZ_SUCCESS);						/* really? */
}

static uLong crash_file (int argc, char *argv[])

{
  char devname[OZ_DEVUNIT_NAMESIZE], *ermsg, *filename, rnamebuff[OZ_FS_MAXFNLEN];
  int i;
  OZ_IO_fs_open fs_open;
  uLong sts;

  filename = NULL;

  for (i = 2; i < argc; i ++) {
    ermsg = "unknown option";
    if (argv[i][0] == '-') goto usage;
    if (filename == NULL) {
      filename = argv[i];
      continue;
    }
    ermsg = "extra parameters";
    goto usage;
  }
  ermsg = "missing file name";
  if (filename == NULL) goto usage;

  /* Open the file */

  memset (&fs_open, 0, sizeof fs_open);
  fs_open.name      = filename;
  fs_open.lockmode  = OZ_LOCKMODE_PW;
  fs_open.rnamesize = sizeof rnamebuff;
  fs_open.rnamebuff = rnamebuff;
  sts = oz_sys_io_fs_open (sizeof fs_open, &fs_open, 0, &h_iochan);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u opening file %s\n", pn, sts, filename);
    return (sts);
  }
  oz_sys_iochan_getunitname (h_iochan, (sizeof devname) - 1, devname);
  strcat (devname, ":");

  /* Set up the file */

  sts = oz_sys_callknl (crash_file_knl, NULL);
  if (sts == OZ_SUCCESS) oz_sys_io_fs_printf (oz_util_h_error, "%s: crash file %s%s set\n", pn, devname, rnamebuff);
  else oz_sys_io_fs_printf (oz_util_h_error, "%s scan: error %u setting crash file %s%s\n", pn, sts, devname, rnamebuff);
  return (sts);

  /* Something missing from command line, output usage message */

usage:
  oz_sys_io_fs_printf (oz_util_h_error, "%s: %s '%s'\n", pn, ermsg, ((i >= argc) || (argv[i] == NULL)) ? "" : argv[i]);
  oz_sys_io_fs_printf (oz_util_h_error, "usage: %s file <filename>\n", pn);
  return (OZ_MISSINGPARAM);
}

static uLong crash_file_knl (OZ_Procmode cprocmode, void *dummy)

{
  int si;
  uLong sts;
  OZ_Iochan *iochan;

  si  = oz_hw_cpu_setsoftint (0);							/* prevent thread aborts */
  sts = oz_knl_handle_takeout (h_iochan, cprocmode, OZ_SECACCMSK_WRITE, OZ_OBJTYPE_IOCHAN, &iochan, NULL); /* convert h_iochan to iochan */
  if (sts == OZ_SUCCESS) {
    sts = oz_knl_crash_set (iochan);							/* set up the crash file */
    oz_knl_handle_putback (h_iochan);							/* all done with iochan */
  }
  oz_hw_cpu_setsoftint (1);								/* allow thread aborts now */
  return (sts);										/* return status */
}

static uLong crash_mode (int argc, char *argv[])

{
  char *ermsg;
  int i, mode;
  uLong sts;

  mode = -1;
  for (i = 2; i < argc; i ++) {
    ermsg = "unknown option";
    if (argv[i][0] == '-') goto usage;
    if (mode < 0) {
      mode = 0;
      if (strcmp (argv[i], "debug") == 0) continue;
      mode = 1;
      if (strcmp (argv[i], "dump") == 0) continue;
      mode = 2;
      if (strcmp (argv[i], "reboot") == 0) continue;
      ermsg = "unknown mode";
      goto usage;
    }
    ermsg = "extra parameters";
    goto usage;
  }
  ermsg = "missing mode";
  if (mode < 0) goto usage;

  /* Set the mode - what happens when an kernel exception occurs */
  /* - mode 0: call kernel debugger           */
  /* - mode 1: write a crash dump then reboot */
  /* - mode 2: reboot immediately             */

  sts = oz_sys_callknl (crash_mode_knl, &mode);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s mode: error %u setting mode\n", pn, sts);
  }
  ermsg = "unknown";
  if (mode == 0) ermsg = "debug";
  if (mode == 1) ermsg = "dump";
  if (mode == 2) ermsg = "reboot";
  oz_sys_io_fs_printf (oz_util_h_error, "%s: new crash mode set, old mode was %s (%d)\n", pn, ermsg, mode);
  return (sts);

  /* Something missing from command line, output usage message */

usage:
  oz_sys_io_fs_printf (oz_util_h_error, "%s: %s '%s'\n", pn, ermsg, ((i >= argc) || (argv[i] == NULL)) ? "" : argv[i]);
  oz_sys_io_fs_printf (oz_util_h_error, "usage: %s mode { debug | dump | reboot }\n", pn);
  return (OZ_MISSINGPARAM);
}

static uLong crash_mode_knl (OZ_Procmode cprocmode, void *modev)

{
  int *modep, oldmode;

  modep   = modev;
  oldmode = oz_s_loadparams.knl_exception;
  oz_s_loadparams.knl_exception = *modep;
  *modep  = oldmode;
  return (OZ_SUCCESS);
}
