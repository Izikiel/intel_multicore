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
/*  Console class driver						*/
/*									*/
/*  This driver does not access any hardware directly, it uses 		*/
/*  'console port' devices for that					*/
/*									*/
/*  The port driver initialization routine creates the device that 	*/
/*  applications access (eg, com1:, console:).  Then the port driver 	*/
/*  assigns an I/O channel to 'conclass' and does an 			*/
/*  OZ_IO_COMPORT_SETUP function to set up the linkage between the two 	*/
/*  drivers.  Then, when the port driver gets an I/O function from an 	*/
/*  application that it does not understand, it passes it along to the 	*/
/*  class driver.							*/
/*									*/
/*  A possible future example of a different class driver might be a 	*/
/*  PPP driver.  It would take ethernet-like functions (because that 	*/
/*  is what the ip driver generates) and convert them to the 		*/
/*  appropriate port driver calls.  The port driver, upon seeing an 	*/
/*  ethernet function code, and not understanding how to process it, 	*/
/*  would pass it on to the class driver.  The PPP class driver would 	*/
/*  then process the ethernet function and call the port driver as 	*/
/*  needed.  Perhaps it would be called 'pppclass' instead of 		*/
/*  'conclass'.								*/
/*									*/
/************************************************************************/

#include <stdarg.h>

#include "ozone.h"
#include "oz_dev_timer.h"
#include "oz_io_comport.h"
#include "oz_io_conpseudo.h"
#include "oz_io_console.h"
#include "oz_io_fs.h"
#include "oz_knl_debug.h"
#include "oz_knl_devio.h"
#include "oz_knl_hw.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_logon.h"
#include "oz_knl_lowipl.h"
#include "oz_knl_printk.h"
#include "oz_knl_procmode.h"
#include "oz_knl_sdata.h"
#include "oz_knl_status.h"
#include "oz_sys_dateconv.h"
#include "oz_sys_xprintf.h"

#define ESC_EREOS "[J"	/* erase from cursor to the end of the screen */
#define ESC_CURUP "[%uA"	/* move cursor up %d lines */
#define ESC_CURDN "[%uB"	/* move cursor down %d lines */
#define ESC_CURFW "[%uC"	/* move cursor forward (right) %d chars */
#define ESC_CURBK "[%uD"	/* move cursor backward (left) %d chars */

#define RHISTSIZ 1024		/* read history buffer size */

#define whilst while

typedef struct Adevex Adevex;
typedef struct Achnex Achnex;
typedef struct Aiopex Aiopex;
typedef struct Mchnex Mchnex;

/* These are actually part of the Mchnex - */
/* They are the class driver's extension area for the application device */

struct Adevex { Long chansassnd;		/* count of channels assigned */
                OZ_Lowipl *lowipl;		/* lowipl routine to call */

                OZ_Devunit *port_devunit;
                void *port_param;
                void (*port_terminate) (void *port_param);
                void (*port_read_start) (void *port_param, int start);
                uLong (*port_disp_start) (void *port_param, void *write_param, uLong size, char *buff);
                void (*port_disp_suspend) (void *port_param, int suspend);
                void (*port_kbd_rah_full) (void *port_param, int full);
                uLong (*port_getsetmode) (void *port_param, void *getset_param, uLong size, OZ_Console_modebuff *buff);

                void *lkprm;
                uLong (*lockdb) (void *lkprm);
                void (*unlkdb) (void *lkprm, uLong iplsav);

                Aiopex *ctrlchars;		/* list of pending OZ_IO_CONSOLE_CTRLCHAR iopex's */
                Aiopex *kb_top;			/* top read iopex waiting for processing */
                Aiopex **kb_bot;		/* pointer last iopex's next field */
                int kb_echo;			/* set to enable echoing of input characters */
                uLong kb_len;			/* length of data in kb_buf */
                uLong kb_ins;			/* offset in kb_buf to insert next char */
                uLong kb_siz;			/* total size of kb_buf */
                uByte *kb_buf;			/* non-paged pool temporary buffer for current request */
                uByte *kb_upd;			/* extension onto end of kb_buf for doing screen updates */
                uLong kb_sts;			/* status of request just completed */
                OZ_Timer *kb_timer;		/* read request timer */
                int kb_timer_pending;		/* set when timer is queued */
						/* cleared when either the timer expires or the timer entry is removed from the queue */
                int kb_timer_expired;		/* cleared when the timer is queued */
						/* set by the timer expired ast routine */
                int inkbdloop;			/* keep from nesting calls to console_kbd_char during echo */

                int rahin;			/* offset for chars going in to rahbuf */
                int rahout;			/* offset for chars coming out of rahbuf */
                char rahbuf[256];		/* read-ahead buffer */

                void *quotaobj;			/* points to quota object for current operation */
                int output_suspend;		/* when set, queue no more display requests to the port device, including prompts and keyboard echo */
                int tt_status;			/* OZ_PENDING : a write is in progress */
						/* OZ_SUCCESS : last write succeeded */
						/*       else : last write failed */
                Aiopex *tt_top, **tt_bot;	/* queue of pending requests */
                Aiopex *tt_done;		/* queue of completed requests */
                int wbhin;			/* offset for chars going in to wbhbuf */
                int wbhout;			/* offset for chars coming out of wbhbuf */
                char wbhbuf[256];		/* write-behind buffer */

                int online;			/* set if initialized and running */
						/* clear if terminating (port driver hasn't set it up yet or has closed channel to main device) */
                OZ_Iochan *closing;		/* main_deassign sets this when it has incremented the iochan's ref count */
						/* lowiplroutine clears it when it decrements the iochan's ref count */
                Long lowiplinprog;		/* 0 : lowipl routine is not executing at all */
						/* 1 : lowipl routine is executing, and hasn't found anything to do so far */
						/* 2 : lowipl routine is executing, and needs to re-execute the loop until it finds nothing to do */

                /* Formatting data */

                int output_bare_lf;		/* 0 : output \r\n for an lf */
						/* 1 : output \n for an lf */
                int hw_wrapping;		/* 0 : hardware does not wrap */
						/* 1 : hardware does wrap */
                int sw_wrapping;		/* 0 : make it look like hardware does not wrap */
						/* 1 : make it look like hardware wraps */
                uLong screenwidth;		/* number of characters on a line */
                uLong screenlength;		/* number of lines on the screen (or page) */
						/* - not used for any processing, just passed back and forth by getmode/setmode */
                uLong linenumber;		/* current line, reset to 0 at start of formatted reads */
                uLong columnumber;		/* current column, 1..screenwidth */
                uLong readcolno;		/* starting columnumber for current read (where first char gets echoed) */
                uLong blankspot;		/* we know stuff starting here to end of screen is blank */
						/* = (linenumber * screenwidth) + columnumber */

                uLong new_kb_ins;		/* saved values for echo_update */
                uLong new_kb_len;
                uLong save_same;

                char escbuf[32];		/* temp buffer for escape sequences */
              };

struct Achnex { Adevex *app_devex;

                /* Read history data */
                /* Entries are formatted: .word length, .ascii text, .even, .word length */

                int rhistsiz;			/* size of rhistbuf */
                int rhistin;			/* points to free space (where next one gets stored) */
                int rhistout;			/* points to occupied space (when oldest one is) */
                int rhistcur;			/* points to current one on screen */
                char *rhistbuf;
              };

struct Aiopex { Aiopex *next;
                OZ_Ioop *ioop;
                uLong funcode;
                OZ_Procmode procmode;
                Achnex *app_chnex;

                uLong write_done;				/* how much of 'write_buff' has been done */
                uLong write_size;				/* total size of 'write_buff' string */
                uByte *write_buff;				/* unformatted data string */
                int write_format;				/* 0 : do not apply formatting; 1 : apply formatting */

                OZ_Quota *quotanpp;				/* set to quota block npp quota was debited from */
                Long quotasiz;					/* how much quota was debited */

                union { struct { int satisfied;			/* 0 : still waiting for it; 1 : got it */
                                 OZ_IO_console_ctrlchar p;	/* call parameters */
                                 uByte ctrlchar;		/* control character that did it */
                               } ctrlchar;
                        struct { uLong size;			/* read buffer size */
                                 uByte *userbuff;		/* users read buffer pointer */
                                 uByte *tempbuff;		/* temporary system read buffer pointer */
                                 uLong *rlen;			/* where to return actual length read */
                                 uLong timeout;			/* timeout (milliseconds) */
                                 int echo;			/* set to enable echoing */
                                 OZ_Datebin timeoutat;		/* when it will time out */
                                 uLong pmtsize;			/* prompt string length */
                                 uByte *pmtbuff;		/* prompt string buffer (system copy) */
                               } read;
                        struct { uLong *wlen;			/* where to return written length */
                                 uLong status;			/* completion status */
                               } write;
                        struct { uLong status;			/* completion status */
                                 OZ_Console_modebuff modebuff;	/* internal copy of mode buffer */
                                 uLong size;			/* user buffer size */
                                 OZ_Console_modebuff *buff;	/* user buffer address */
                               } gsmode;
                      } u;
             };

static uLong app_assign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode);
static int app_deassign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv);
static void app_abort (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Ioop *ioop, void *iopexv, OZ_Procmode procmode);
static uLong app_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnex, OZ_Procmode procmode, 
                        OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap);

static const OZ_Devfunc app_functable = { sizeof (Adevex), sizeof (Achnex), sizeof (Aiopex), 0, NULL, NULL, NULL, 
                                          app_assign, app_deassign, app_abort, app_start, NULL };

/* These exist on the main device (conclass:) only */

struct Mchnex { Adevex app_devex;	/* corresponding application level devex */
              };

static uLong main_assign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode);
static int main_deassign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv);
static uLong main_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnex, OZ_Procmode procmode, 
                         OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap);

static const OZ_Devfunc main_functable = { 0, sizeof (Mchnex), 0, 0, NULL, NULL, NULL, main_assign, main_deassign, NULL, main_start, NULL };

/* Static data and internal functions */

static int initialized = 0;
static OZ_Devclass *devclass;
static OZ_Devdriver *devdriver;
static OZ_Devunit *main_devunit;

static int aborteverything (Adevex *app_devex, uLong kb);
static uLong conwrite (Adevex *app_devex, Aiopex *app_iopex, uLong size, const void *buff, uLong *wlen, uLong trmsize, const void *trmbuff, int format);
static uLong conread (Adevex *app_devex, Aiopex *app_iopex, uLong size, void *buff, uLong *rlen, uLong pmtsize, const void *pmtbuff, uLong timeout, int echo);
static void lowipl_hi (Adevex *app_devex);
static void lowiplroutine (void *app_devexv, OZ_Lowipl *lowipl);
static void readtimedout (void *app_devexv, OZ_Timer *timer);
static void iodone (Aiopex *app_iopex, uLong status, void (*finentry) (void *finparam, int finok, uLong *status_r), void *finparam);
static void ctrlcharfin (void *ccv, int finok, uLong *status_r);
static void write_iodone (void *app_iopexv, int finok, uLong *status_r);
static void read_iodone (void *app_iopexv, int finok, uLong *status_r);
static void console_kbd_char (void *app_devexv, char bl);
static int echo_update (Adevex *app_devex, uLong same, uLong new_kb_ins, uLong new_kb_len);
static int setcurpos (Adevex *app_devex, const char *initstr, uLong newrow, uLong newcol);
static void fakeformat (uLong size, uLong *col_r, uLong *row_r, Adevex *app_devex);
static uLong resume_write (Adevex *app_devex, Aiopex *app_iopex);
static uLong disp_start_escbuf (Adevex *app_devex, const char *format, ...);
static uLong disp_start_string (Adevex *app_devex, uLong size, const void *buff);
static uLong disp_start_flush (Adevex *app_devex);
static void console_displayed (void *app_devexv, void *write_param, uLong status);
static void console_gotsetmode (void *app_devexv, void *app_iopexv, uLong status);
static void fin_gotsetmode (void *app_iopexv, int finok, uLong *status_r);
static uLong console_setmode (void *app_devexv, OZ_IO_conpseudo_setmode *conpseudo_setmode);
static uLong lockdb (Adevex *app_devex, void *quotaobj);
static void unlkdb (Adevex *app_devex, uLong kb);

/************************************************************************/
/*									*/
/*  Boot-time initialization routine					*/
/*									*/
/************************************************************************/

void oz_dev_conclass_init ()

{
  if (!initialized) {
    oz_knl_printk ("oz_dev_conclass_init\n");
    initialized = 1;

    /* Create template device for port drivers to connect to */

    devclass     = oz_knl_devclass_create (OZ_IO_CONSOLE_CLASSNAME, OZ_IO_CONSOLE_BASE, OZ_IO_CONSOLE_MASK, "oz_dev_conclass");
    devdriver    = oz_knl_devdriver_create (devclass, "oz_dev_conclass");
    main_devunit = oz_knl_devunit_create (devdriver, OZ_IO_CONSOLE_SETUPDEV, "console class setup", &main_functable, 0, oz_s_secattr_tempdev);
  }
}

/************************************************************************/
/*									*/
/*  Port driver is assigning channel to main device			*/
/*									*/
/*  We clear out the app_devex area so it starts in a known state	*/
/*									*/
/************************************************************************/

static uLong main_assign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode)

{
  Mchnex *main_chnex;

  main_chnex = chnexv;
  if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);
  memset (&(main_chnex -> app_devex), 0, sizeof main_chnex -> app_devex);
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Port driver is deassigning channel to main device - terminate 	*/
/*  corresponding application level device - when this routine returns 	*/
/*  the main_chnex -> app_devex area should be completely cleaned out	*/
/*									*/
/************************************************************************/

static int main_deassign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv)

{
  Adevex *app_devex;
  int repeat;
  uLong kb;
  Mchnex *main_chnex;

  main_chnex = chnexv;
  app_devex  = &(main_chnex -> app_devex);

  app_devex -> online = 0;					/* don't let start routine queue any more requests */
  kb = lockdb (app_devex, NULL);				/* lock database */
  repeat = aborteverything (app_devex, kb);			/* try to abort everything queued to the device */
  unlkdb (app_devex, kb);					/* unlock database */
  if (repeat) {							/* if it is idle, return telling it we're all done */
    lowiplroutine (app_devex, NULL);				/* call lowiplroutine to try to finish off the aborts */
    oz_knl_iochan_increfc (iochan, 1);				/* increment iochan ref count */
    kb = lockdb (app_devex, NULL);				/* lock database */
    repeat = aborteverything (app_devex, kb);			/* scan the queues again */
    if (repeat) {
      app_devex -> closing = iochan;				/* if still busy, tell lowiplroutine to dec iochan ref count */
      unlkdb (app_devex, kb);					/* unlock database */
      lowiplroutine (app_devex, NULL);				/* call lowiplroutine to try to finish off the aborts */
    } else {
      unlkdb (app_devex, kb);					/* it's idle now, release lock */
      oz_knl_iochan_increfc (iochan, -1);			/* decrement it back */
    }
  }
  if (!repeat && (app_devex -> lowiplinprog != 0)) oz_crash ("oz_dev_conclass main_deassign: lowiplinprog stuck on"); /* ?? maybe a short timed wait is all we need ?? */
  return (repeat);						/* returning with 0 means we're all done */
								/* returning with 1 means stuff is still busy and oz_knl_iochan_increfc will be called */
								/* to decrement the channel's ref count when it is time to be called back again        */
}

/* Try to abort everything on the device.  Return 0 if successful, return 1 if there is stuff to wait for. */

static int aborteverything (Adevex *app_devex, uLong kb)

{
  int repeat;
  Aiopex *iopex, **liopex;

  repeat = 0;											/* we have not found anything we have to wait for */

  /* Abort all I/O in progress for the device */

  whilst ((iopex = app_devex -> ctrlchars) != NULL) {						/* scan through queue */
    app_devex -> ctrlchars = iopex -> next;							/* unlink from queue */
    unlkdb (app_devex, kb);									/* release the lock */
    iodone (iopex, OZ_ABORTED, NULL, NULL);							/* complete the request with aborted status */
    kb = lockdb (app_devex, NULL);								/* lock the queue */
  }

  whilst (((iopex = app_devex -> kb_top) != NULL) && ((iopex = iopex -> next) != NULL)) {	/* see if anything on queue after the first */
    app_devex -> kb_top -> next = iopex -> next;						/* unlink from queue */
    unlkdb (app_devex, kb);									/* release the queue lock */
    if (iopex -> u.read.pmtbuff != NULL) OZ_KNL_NPPFREE (iopex -> u.read.pmtbuff);		/* free any prompt buffer */
    iodone (iopex, OZ_ABORTED, NULL, NULL);							/* post request as completed */
    kb = lockdb (app_devex, NULL);								/* lock the queue */
  }
  if (app_devex -> kb_sts == OZ_PENDING) {
    app_devex -> kb_sts = OZ_ABORTED;								/* mark the first aborted */
    (*(app_devex -> port_read_start)) (app_devex -> port_param, 0);				/* tell port driver read is finished */
    repeat = 1;											/* we have to repeat while it aborts */
  }

writescan:
  for (liopex = &(app_devex -> tt_top); (iopex = *liopex) != NULL; liopex = &(iopex -> next)) {
    *liopex = iopex -> next;
    if (*liopex == NULL) app_devex -> tt_bot = liopex;
    unlkdb (app_devex, kb);									/* release the queue lock */
    OZ_KNL_NPPFREE (iopex -> write_buff);							/* free temp buffer */
    iodone (iopex, OZ_ABORTED, NULL, NULL);							/* post request as completed */
    kb = lockdb (app_devex, NULL);								/* lock the queue */
    repeat = 1;
    goto writescan;
  }
  if (app_devex -> tt_status == OZ_PENDING) {							/* see if transmitter is all clogged */
    // cant call after it has been terminated ?? (*(app_devex -> port_disp_suspend)) (app_devex -> port_param, -1); /* if so, abort output in progress */
    repeat = 1;											/* we have to repeat while it aborts */
  }

  return (repeat);
}

/************************************************************************/
/*									*/
/*  An I/O channel is being assigned to the application level device	*/
/*									*/
/************************************************************************/

static uLong app_assign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode)

{
  Achnex *app_chnex;

  app_chnex = chnexv;
  memset (app_chnex, 0, sizeof *app_chnex);				/* clear out the channel block */
  app_chnex -> app_devex = devexv;					/* save pointer to the device */
  app_chnex -> rhistsiz  = RHISTSIZ;					/* save size of read history buffer */
  app_chnex -> rhistbuf  = OZ_KNL_NPPMALLOQ (app_chnex -> rhistsiz);	/* try to allocate read history buffer */
  if (app_chnex -> rhistbuf == NULL) return (OZ_EXQUOTANPP);
  OZ_HW_ATOMIC_INCBY1_LONG (((Adevex *)devexv) -> chansassnd);		/* ok, inc number of channels on the device */
  return (OZ_SUCCESS);							/* successful */
}

/************************************************************************/
/*									*/
/*  An I/O channel is being deassigned from the app level device	*/
/*									*/
/*  If count goes to zero, call the terminate routine to let the port 	*/
/*  driver know that no one is using the device anymore.		*/
/*									*/
/************************************************************************/

static int app_deassign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv)

{
  Achnex *app_chnex;
  Adevex *app_devex;
  uLong kb;

  app_chnex = chnexv;
  app_devex = devexv;

  /* Free off any associated read history buffer */

  if (app_chnex -> rhistbuf != NULL) {
    OZ_KNL_NPPFREE (app_chnex -> rhistbuf);
    app_chnex -> rhistsiz = 0;
    app_chnex -> rhistbuf = NULL;
  }

  /* If this is the last channel, call the port driver's terminate routine */

  if (oz_hw_atomic_inc_long (&(app_devex -> chansassnd), -1) == 0) {
    kb = lockdb (app_devex, NULL);
    (*(app_devex -> port_terminate)) (app_devex -> port_param);
    unlkdb (app_devex, kb);
  }
  return (0);
}

/************************************************************************/
/*									*/
/*  Abort an application level console i/o function			*/
/*									*/
/*  This routine gets called by the port driver's abort i/o routine	*/
/*									*/
/*    Input:								*/
/*									*/
/*	devunit  = port device unit					*/
/*	devexv   = my Adevex extensions					*/
/*	iochan   = the iochannel assigned by application program	*/
/*	chnexv   = my Achnex extensions					*/
/*	ioop     = i/o request						*/
/*	iopexv   = my Aiopex extensions					*/
/*	procmode = processor mode being aborted				*/
/*									*/
/************************************************************************/

static void app_abort (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Ioop *ioop, void *iopexv, OZ_Procmode procmode)

{
  Adevex *app_devex;
  Aiopex *iopex, **liopex;
  uLong kb;

  app_devex = devexv;

abort_ctrlchars:
  kb = lockdb (app_devex, NULL);								/* lock the queue */
  for (liopex = &(app_devex -> ctrlchars); (iopex = *liopex) != NULL; liopex = &(iopex -> next)) { /* scan through queue */
    if (iopex -> u.ctrlchar.satisfied) continue;						/* skip satisfied requests */
    if (!oz_knl_ioabortok (iopex -> ioop, iochan, procmode, ioop)) continue;			/* skip entry if not to be aborted */
    *liopex = iopex -> next;									/* abort, unlink from queue */
    unlkdb (app_devex, kb);									/* release the lock */
    iodone (iopex, OZ_ABORTED, NULL, NULL);							/* complete the request with aborted status */
    goto abort_ctrlchars;									/* see if any more to abort */
  }
  unlkdb (app_devex, kb);									/* none to be aborted, release lock */

abort_reads:
  kb = lockdb (app_devex, NULL);								/* lock the queue */
  if (app_devex -> kb_top != NULL) {								/* see if anything on queue */
    for (liopex = &(app_devex -> kb_top -> next); (iopex = *liopex) != NULL; liopex = &(iopex -> next)) { /* loop through read requests, except for the top one */
      if (!oz_knl_ioabortok (iopex -> ioop, iochan, procmode, ioop)) continue;			/* skip if not to be aborted */
      *liopex = iopex -> next;									/* abort, unlink from queue */
      if (*liopex == NULL) app_devex -> kb_bot = liopex;
      unlkdb (app_devex, kb);									/* release the queue lock */
      if (iopex -> u.read.pmtbuff != NULL) OZ_KNL_NPPFREE (iopex -> u.read.pmtbuff);		/* free any prompt buffer */
      iodone (iopex, OZ_ABORTED, NULL, NULL);							/* post request as completed */
      goto abort_reads;										/* check for more in queue */
    }
    if (oz_knl_ioabortok (app_devex -> kb_top -> ioop, iochan, procmode, ioop)) {		/* none to be aborted after first, see if first to be aborted */
      if (app_devex -> kb_sts == OZ_PENDING) {
        app_devex -> kb_sts = OZ_ABORTED;							/* if so, mark it aborted */
        (*(app_devex -> port_read_start)) (app_devex -> port_param, 0);				/* tell port driver read is finished */
      }
    }
  }
  unlkdb (app_devex, kb);									/* release lock */

abort_writes:
  kb = lockdb (app_devex, NULL);								/* lock the queue */
  if (app_devex -> tt_top != NULL) {								/* see if anything on queue */
    for (liopex = &(app_devex -> tt_top); (iopex = *liopex) != NULL; liopex = &(iopex -> next)) { /* loop through write requests */
      if (!oz_knl_ioabortok (iopex -> ioop, iochan, procmode, ioop)) continue;			/* skip if not to be aborted */
      *liopex = iopex -> next;									/* abort, unlink from queue */
      if (*liopex == NULL) app_devex -> tt_bot = liopex;
      unlkdb (app_devex, kb);									/* release the queue lock */
      OZ_KNL_NPPFREE (iopex -> write_buff);							/* free temp buffer */
      iodone (iopex, OZ_ABORTED, NULL, NULL);							/* post request as completed */
      goto abort_writes;									/* check for more in queue */
    }
  }
  unlkdb (app_devex, kb);									/* release lock */

  lowiplroutine (app_devex, NULL);								/* clean up aborted requests */
}

/************************************************************************/
/*									*/
/*  Start performing a main device console i/o function			*/
/*									*/
/************************************************************************/

static uLong main_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode, 
                         OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap)

{
  Adevex *app_devex;
  uLong dv;
  Mchnex *main_chnex;

  main_chnex = chnexv;

  switch (funcode) {

    /* A port driver calls this function to create a console device that is accessible to applications programs.          */
    /* This function should only be executed in kernel mode as the interface params are accessible only from kernel mode. */

    case OZ_IO_COMPORT_SETUP: {
      OZ_IO_comport_setup comport_setup;

      if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);

      app_devex = &(main_chnex -> app_devex);
      dv = oz_hw_smplock_wait (&oz_s_smplock_dv);
      if (app_devex -> online) {
        oz_hw_smplock_clr (&oz_s_smplock_dv, dv);
        return (OZ_FILEALREADYOPEN);
      }

      movc4 (as, ap, sizeof comport_setup, &comport_setup);

      /* Initialize app_devex structure */

      memset (app_devex, 0, sizeof *app_devex);

      app_devex -> port_devunit      = comport_setup.port_devunit;		/* port device unit pointer */
      app_devex -> port_param        = comport_setup.port_param;		/* parameter that the port routines want us to pass to them */
      app_devex -> port_terminate    = comport_setup.port_terminate;		/* termination notification routine (may be null if permanent device) */
      app_devex -> port_read_start   = comport_setup.port_read_start;		/* port routine to call when starting/finishing a read */
      app_devex -> port_disp_start   = comport_setup.port_disp_start;		/* port routine to call when we have something to display */
      app_devex -> port_disp_suspend = comport_setup.port_disp_suspend;		/* port routine to call when we want to suspend/resume display */
      app_devex -> port_kbd_rah_full = comport_setup.port_kbd_rah_full;		/* port routine to call when the read-ahead buffer is full or is empty */
      app_devex -> port_getsetmode   = comport_setup.port_getsetmode;		/* port routine to call to get/set modes */
      app_devex -> lkprm             = comport_setup.port_lkprm;		/* parameter to pass to lockdb/unlkdb routines */
      app_devex -> lockdb            = comport_setup.port_lockdb;		/* lock database routine */
      app_devex -> unlkdb            = comport_setup.port_unlkdb;		/* unlock database routine */

      app_devex -> tt_bot    = &(app_devex -> tt_top);				/* display request queue is empty */
      app_devex -> kb_sts    = OZ_SUCCESS;					/* no keyboard request in progress */
      app_devex -> kb_bot    = &(app_devex -> kb_top);
      app_devex -> kb_timer  = oz_knl_timer_alloc ();				/* allocate a timeout timer */
      if (app_devex -> kb_timer == NULL) {
        oz_hw_smplock_clr (&oz_s_smplock_dv, dv);
        return (OZ_EXQUOTANPP);
      }
      app_devex -> tt_status = OZ_SUCCESS;					/* pretend last write was successful */

      /* Allocate a lowipl struct to process requests and mark device online */

      app_devex -> lowipl = oz_knl_lowipl_alloc ();
      app_devex -> online = 1;
      oz_hw_smplock_clr (&oz_s_smplock_dv, dv);

      /* Return parameters to port driver */

      comport_setup.class_functab    = &app_functable;				/* port driver calls this function table with i/o's it can't handle directly */
      comport_setup.class_devex      = app_devex;				/* ... and it passes them this devex structure instead of its own */
      comport_setup.class_param      = app_devex;				/* port driver must pass this parameter when it calls us */
      comport_setup.class_kbd_char   = console_kbd_char;			/* port driver calls this when it has a keyboard character for us to process */
      comport_setup.class_displayed  = console_displayed;			/* port driver calls this when it has finished displaying something */
      comport_setup.class_gotsetmode = console_gotsetmode;			/* port driver calls this when it has finished get/set modes */
      comport_setup.class_setmode    = console_setmode;				/* port driver calls this when it wants to tell us the modes */
      movc4 (sizeof comport_setup, &comport_setup, as, ap);			/* copy the stuff back out to port driver */

      /* Butcher these in - port driver should call console_setmode to fix them */

      app_devex -> hw_wrapping  =  0;
      app_devex -> sw_wrapping  =  1;
      app_devex -> screenwidth  = 80;
      app_devex -> screenlength = 24;
      app_devex -> columnumber  =  1;
      app_devex -> linenumber   =  0;
      app_devex -> blankspot    = -1;

      return (OZ_SUCCESS);
    }

    /* Who knows what */

    default: {
      return (OZ_BADIOFUNC);
    }
  }
}

/************************************************************************/
/*									*/
/*  Start performing an applications device I/O function		*/
/*									*/
/*  This gets called by the port driver when it gets an i/o function 	*/
/*  code it does not know how to process				*/
/*									*/
/*    Input:								*/
/*									*/
/*	devunit  = port device						*/
/*	devexv   = my Adevex area of the port device			*/
/*	iochan   = the I/O channel assigned to port device by application
/*	chnexv   = my Achnex area of the channel			*/
/*	procmode = processor mode of the I/O request			*/
/*	ioop     = i/o operation struct					*/
/*	iopexv   = my Aiopex area of the i/o operation			*/
/*	funcode  = function code					*/
/*	as, ap   = function arguments					*/
/*									*/
/************************************************************************/

static uLong app_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode, 
                        OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap)

{
  Adevex *app_devex;
  Aiopex *app_iopex;
  uLong kb, sts;

  app_devex = devexv;
  app_iopex = iopexv;

  app_iopex -> next      = NULL;
  app_iopex -> ioop      = ioop;
  app_iopex -> funcode   = funcode;
  app_iopex -> procmode  = procmode;
  app_iopex -> app_chnex = chnexv;
  app_iopex -> quotanpp  = NULL;

  switch (funcode) {

    /* Write a line to the console */

    case OZ_IO_CONSOLE_WRITE: {
      OZ_IO_console_write console_write;

      movc4 (as, ap, sizeof console_write, &console_write);
      sts = conwrite (app_devex, app_iopex, console_write.size, console_write.buff, NULL, console_write.trmsize, console_write.trmbuff, 1);
      return (sts);
    }

    case OZ_IO_CONSOLE_PUTDAT: {
      OZ_IO_console_putdat console_putdat;

      movc4 (as, ap, sizeof console_putdat, &console_putdat);
      sts = conwrite (app_devex, app_iopex, console_putdat.size, console_putdat.buff, NULL, 0, NULL, 0);
      return (sts);
    }

    case OZ_IO_FS_WRITEREC: {
      OZ_IO_fs_writerec fs_writerec;

      movc4 (as, ap, sizeof fs_writerec, &fs_writerec);
      sts = conwrite (app_devex, app_iopex, fs_writerec.size, fs_writerec.buff, fs_writerec.wlen, fs_writerec.trmsize, fs_writerec.trmbuff, 1);
      return (sts);
    }

    /* Read a line from the console */

    case OZ_IO_CONSOLE_READ: {
      OZ_IO_console_read console_read;

      movc4 (as, ap, sizeof console_read, &console_read);
      sts = conread (app_devex, app_iopex, console_read.size, console_read.buff, console_read.rlen, 
                     console_read.pmtsize, console_read.pmtbuff, console_read.timeout, !console_read.noecho);
      return (sts);
    }

    case OZ_IO_FS_READREC: {
      OZ_IO_fs_readrec fs_readrec;

      movc4 (as, ap, sizeof fs_readrec, &fs_readrec);
      sts = conread (app_devex, app_iopex, fs_readrec.size, fs_readrec.buff, fs_readrec.rlen, fs_readrec.pmtsize, fs_readrec.pmtbuff, 0, 1);
      return (sts);
    }

    /* Wait for a control character from keyboard */

    case OZ_IO_CONSOLE_CTRLCHAR: {
      movc4 (as, ap, sizeof app_iopex -> u.ctrlchar.p, &(app_iopex -> u.ctrlchar.p));
      app_iopex -> u.ctrlchar.satisfied = 0;			/* not yet satisfied */
      kb  = lockdb (app_devex, ioop);				/* get access to interrupt level variables */
      sts = OZ_DEVOFFLINE;
      if (app_devex -> online) {				/* make sure device is online */
        app_iopex -> next = app_devex -> ctrlchars;		/* put this request on ctrlchars list, last-in/first-out order */
        app_devex -> ctrlchars = app_iopex;
        sts = OZ_STARTED;
      }
      unlkdb (app_devex, kb);
      return (sts);
    }

    /* Get raw keyboard data from the port driver */

    case OZ_IO_CONSOLE_GETDAT: {
      OZ_IO_console_getdat  console_getdat;

      movc4 (as, ap, sizeof console_getdat, &console_getdat);
      sts = oz_knl_ioop_lock (ioop, console_getdat.size, console_getdat.buff, 1, NULL, NULL, NULL);
      if ((sts == OZ_SUCCESS) && (console_getdat.rlen != NULL)) {
        sts = oz_knl_ioop_lock (ioop, sizeof *console_getdat.rlen, console_getdat.rlen, 1, NULL, NULL, NULL);
      }
      if ((sts == OZ_SUCCESS) && (console_getdat.rlen != NULL)) *console_getdat.rlen = 0;
      if ((sts == OZ_SUCCESS) && (console_getdat.size > 0)) {
        app_iopex -> u.read.pmtsize   = 0;			/* no prompt buffer */
        app_iopex -> u.read.pmtbuff   = NULL;
        app_iopex -> write_size       = 0;
        app_iopex -> write_buff       = NULL;
        app_iopex -> u.read.size      = console_getdat.size;	/* save params in app_iopex area */
        app_iopex -> u.read.userbuff  = console_getdat.buff;
        app_iopex -> u.read.rlen      = console_getdat.rlen;
        app_iopex -> u.read.tempbuff  = NULL;
        app_iopex -> u.read.timeout   = 0;
        app_iopex -> u.read.echo      = 0;
        kb  = lockdb (app_devex, app_iopex -> ioop);		/* get access to interrupt level variables */
        sts = OZ_DEVOFFLINE;
        if (app_devex -> online) {				/* make sure device is online */
          *(app_devex -> kb_bot) = app_iopex;			/* put the request on end of the queue */
          app_devex -> kb_bot = &(app_iopex -> next);
          sts = OZ_STARTED;
        }
        unlkdb (app_devex, kb);
        lowiplroutine (app_devex, NULL);			/* start processing request */
      }
      return (sts);
    }

    /* Get / Set modes */

    case OZ_IO_CONSOLE_GETMODE:
    case OZ_IO_CONSOLE_SETMODE: {
      OZ_IO_console_getmode console_getmode;
      OZ_IO_console_setmode console_setmode;

      if (funcode == OZ_IO_CONSOLE_GETMODE) {
        movc4 (as, ap, sizeof console_getmode, &console_getmode);
        memset (&(app_iopex -> u.gsmode.modebuff), 0, sizeof app_iopex -> u.gsmode.modebuff);
        app_iopex -> u.gsmode.size = console_getmode.size;
        app_iopex -> u.gsmode.buff = console_getmode.buff;
      }
      if (funcode == OZ_IO_CONSOLE_SETMODE) {
        movc4 (as, ap, sizeof console_setmode, &console_setmode);
        app_iopex -> u.gsmode.size = console_setmode.size;
        app_iopex -> u.gsmode.buff = console_setmode.buff;
      }
      sts = oz_knl_ioop_lockw (ioop, app_iopex -> u.gsmode.size, app_iopex -> u.gsmode.buff, NULL, NULL, NULL);
      if (sts == OZ_SUCCESS) {
        if (funcode == OZ_IO_CONSOLE_SETMODE) {			/* if setmode, get user's values */
          movc4 (app_iopex -> u.gsmode.size, app_iopex -> u.gsmode.buff, sizeof app_iopex -> u.gsmode.modebuff, &(app_iopex -> u.gsmode.modebuff));
          if (app_iopex -> u.gsmode.modebuff.columns  != 0) app_devex -> screenwidth  = app_iopex -> u.gsmode.modebuff.columns;
          if (app_iopex -> u.gsmode.modebuff.rows     != 0) app_devex -> screenlength = app_iopex -> u.gsmode.modebuff.rows;
          if (app_iopex -> u.gsmode.modebuff.linewrap != 0) app_devex -> sw_wrapping  = (app_iopex -> u.gsmode.modebuff.linewrap > 0);
        }
        kb  = lockdb (app_devex, app_iopex -> ioop);		/* get access to interrupt level variables */
        sts = (*(app_devex -> port_getsetmode)) (app_devex -> port_param, app_iopex, sizeof app_iopex -> u.gsmode.modebuff, &(app_iopex -> u.gsmode.modebuff));
        unlkdb (app_devex, kb);					/* release interrupt level variables */
        if (sts != OZ_STARTED) fin_gotsetmode (app_iopex, 1, &sts); /* if synchronous completion, copy out result */
      }
      return (sts);
    }

    /* No-op fs functions */

    case OZ_IO_FS_CREATE:
    case OZ_IO_FS_OPEN:
    case OZ_IO_FS_CLOSE: {
      return (OZ_SUCCESS);
    }

    /* Who knows what */

    default: {
      return (OZ_BADIOFUNC);
    }
  }
}

/************************************************************************/
/*									*/
/*  Qeueue write request						*/
/*									*/
/************************************************************************/

static uLong conwrite (Adevex *app_devex, Aiopex *app_iopex, uLong size, const void *buff, uLong *wlen, uLong trmsize, const void *trmbuff, int format)

{
  uLong kb, sts;

  sts = oz_knl_ioop_lock (app_iopex -> ioop, size, buff, 0, NULL, NULL, NULL);
  if ((sts == OZ_SUCCESS) && (wlen != NULL)) sts = oz_knl_ioop_lock (app_iopex -> ioop, sizeof wlen, wlen, 1, NULL, NULL, NULL);
  if (sts == OZ_SUCCESS) sts = oz_knl_ioop_lock (app_iopex -> ioop, trmsize, trmbuff, 0, NULL, NULL, NULL);
  if ((sts == OZ_SUCCESS) && (size + trmsize != 0)) {
    app_iopex -> write_done     = 0;				/* nothing has been written yet */
    app_iopex -> write_size     = size + trmsize;		/* save total size to write */
    app_iopex -> write_buff     = OZ_KNL_NPPMALLOQ (size + trmsize); /* allocate a temp non-paged buffer */
    if (app_iopex -> write_buff == NULL) sts = OZ_EXQUOTANPP;
    else {
      app_iopex -> u.write.status = OZ_PENDING;			/* it is not done yet */
      app_iopex -> write_format   = format;			/* copy the 'apply formatting' parameter */
      memcpy (app_iopex -> write_buff, buff, size);		/* copy in main string */
      memcpy (app_iopex -> write_buff + size, trmbuff, trmsize); /* copy in terminator string */
      app_iopex -> u.write.wlen = wlen;				/* save where to return write length */
      kb  = lockdb (app_devex, app_iopex -> ioop);		/* access interrupt level variables */
      sts = OZ_DEVOFFLINE;
      if (app_devex -> online) {				/* make sure device is online */
        *(app_devex -> tt_bot) = app_iopex;			/* queue the request */
        app_devex -> tt_bot = &(app_iopex -> next);
        unlkdb (app_devex, kb);					/* release interrupt level variables */
        lowiplroutine (app_devex, NULL);			/* maybe start outputting */
        sts = OZ_STARTED;					/* asynchronous completion */
      } else {
        unlkdb (app_devex, kb);					/* release interrupt level variables */
        OZ_KNL_NPPFREE (app_iopex -> write_buff);		/* free temp buffer */
        sts = OZ_DEVOFFLINE;					/* return error status */
      }
    }
  }
  return (sts);
}

/************************************************************************/
/*									*/
/*  Queue read request							*/
/*									*/
/************************************************************************/

static uLong conread (Adevex *app_devex, Aiopex *app_iopex, uLong size, void *buff, uLong *rlen, uLong pmtsize, const void *pmtbuff, uLong timeout, int echo)

{
  OZ_Quota *quota;
  uLong kb, sts;

  sts = oz_knl_ioop_lock (app_iopex -> ioop, size, buff, 1, NULL, NULL, NULL);
  if (sts == OZ_SUCCESS) sts = oz_knl_ioop_lock (app_iopex -> ioop, pmtsize, pmtbuff, 0, NULL, NULL, NULL);
  if ((sts == OZ_SUCCESS) && (rlen != NULL)) sts = oz_knl_ioop_lock (app_iopex -> ioop, sizeof *rlen, rlen, 1, NULL, NULL, NULL);
  if ((sts == OZ_SUCCESS) && (rlen != NULL)) *rlen = 0;
  if ((sts == OZ_SUCCESS) && (size > 0)) {
    quota = OZ_KNL_QUOTA_DEFAULT;
    if (quota != NULL) {
      if (!oz_knl_quota_debit (quota, OZ_QUOTATYPE_NPP, size * 2 + pmtsize)) sts = OZ_EXQUOTANPP;
      else {
        app_iopex -> quotanpp = quota;
        app_iopex -> quotasiz = size * 2 + pmtsize;
      }
    }
  }
  if ((sts == OZ_SUCCESS) && (size > 0)) {
    app_iopex -> write_done       = 0;					/* buffers successfully locked, save params in iopex area */
    app_iopex -> write_size       = 0;
    app_iopex -> write_buff       = NULL;
    app_iopex -> write_format     = 1;
    app_iopex -> u.read.pmtsize   = pmtsize;
    app_iopex -> u.read.pmtbuff   = NULL;
    if (pmtsize != 0) {
      app_iopex -> u.read.pmtbuff = OZ_KNL_NPPMALLOC (pmtsize);
      memcpy (app_iopex -> u.read.pmtbuff, pmtbuff, pmtsize);
      app_iopex -> write_size = pmtsize;				/* set this up now so partial write can be restarted */
      app_iopex -> write_buff = app_iopex -> u.read.pmtbuff;
    }
    if (sts == OZ_SUCCESS) {
      app_iopex -> u.read.size      = size;
      app_iopex -> u.read.userbuff  = buff;
      app_iopex -> u.read.rlen      = rlen;
      app_iopex -> u.read.tempbuff  = NULL;
      app_iopex -> u.read.timeout   = timeout;
      app_iopex -> u.read.echo      = echo;
      kb = lockdb (app_devex, app_iopex -> ioop);			/* get access to interrupt level variables */
      if (app_devex -> online) {					/* make sure device is online */
        *(app_devex -> kb_bot) = app_iopex;				/* put the request on end of the queue */
        app_devex -> kb_bot = &(app_iopex -> next);
        unlkdb (app_devex, kb);
        lowiplroutine (app_devex, NULL);				/* maybe start processing request */
        sts = OZ_STARTED;						/* it will complete asynchronously */
      } else {
        unlkdb (app_devex, kb);
        if (app_iopex -> write_buff != NULL) OZ_KNL_NPPFREE (app_iopex -> write_buff);
        sts = OZ_DEVOFFLINE;
      }
    }
  }
  return (sts);
}

/************************************************************************/
/*									*/
/*  This routine is called from high ipl to call the 'lowiplroutine'	*/
/*									*/
/*    Input:								*/
/*									*/
/*	smplevel = kb lock set						*/
/*									*/
/************************************************************************/

static void lowipl_hi (Adevex *app_devex)

{
  OZ_Lowipl *lowipl;

  lowipl = app_devex -> lowipl;					/* see if the lowipl struct is in use */
  if (lowipl != NULL) {
    app_devex -> lowipl = NULL;					/* if not, mark it in use */
    oz_knl_lowipl_call (lowipl, lowiplroutine, app_devex);	/* ... and queue it so lowiplroutine will be called */
  }
}

/************************************************************************/
/*									*/
/*  This lowipl routine processes I/O requests for the applications 	*/
/*  level device							*/
/*									*/
/*    Input:								*/
/*									*/
/*	app_devexv = device to be processed				*/
/*	lowipl = NULL : don't touch app_devexv -> lowipl		*/
/*	         else : restore app_devexv -> lowipl			*/
/*	smplock = softint						*/
/*									*/
/************************************************************************/

static void lowiplroutine (void *app_devexv, OZ_Lowipl *lowipl)

{
  Achnex *app_chnex;
  Adevex *app_devex;
  Aiopex *cc, *iopex, **lcc;
  int called_iodone;
  uLong kb, sts;
  OZ_Datebin delta;
  OZ_Iochan *iochan;

  app_devex = app_devexv;
  if (lowipl != NULL) app_devex -> lowipl = lowipl;
  called_iodone = 0;

  /* If the flag is non-zero, it means the routine is already running somewhere (like on another cpu) */
  /* But set it to 2 so that the other cpu will re-run the whole loop                                 */

dequeue:
  if (oz_hw_atomic_set_long (&(app_devex -> lowiplinprog), 2) != 0) return;

  /* The flag was zero and we are the first to set it to a non-zero value */

  /* Set the flag to 1 because we are about to check everything out.                                                              */
  /* If someone tries to re-enter the routine while we're running, they set the flag to 2 which will cause us to repeat the loop. */
  /* If the lowiplinprog flag survives the whole loop set at 1, the loop will exit at the bottom.                                 */

dequeue2:
  app_devex -> lowiplinprog = 1;

  /* Process any waiting write requests if any and we're not suspended and the transmitter is not overflowed */

  if (!(app_devex -> output_suspend) && (app_devex -> tt_status != OZ_PENDING) && (app_devex -> tt_top != NULL)) {
    kb = lockdb (app_devex, NULL);							/* lock database */
    iopex = app_devex -> tt_top;							/* see if anything to process */
    if ((iopex != NULL) && !(app_devex -> output_suspend) && (app_devex -> tt_status != OZ_PENDING)) {
      app_devex -> tt_top = iopex -> next;						/* if so, dequeue request */
      if (app_devex -> tt_top == NULL) app_devex -> tt_bot = &(app_devex -> tt_top);
      sts = OZ_DEVOFFLINE;								/* start it */
      if (app_devex -> online) sts = resume_write (app_devex, iopex);
      if (sts == OZ_STARTED) {								/* if transmitter's much too busy, */
        iopex -> next = app_devex -> tt_top;						/* ... put it back on queue */
        app_devex -> tt_top = iopex;							/* ... (we'll finish it later) */
        if (iopex -> next == NULL) app_devex -> tt_bot = &(iopex -> next);
      } else {
        iopex -> u.write.status = sts;							/* otherwise it finished already */
        iopex -> next = app_devex -> tt_done;						/* ... link it to 'done' queue */
        app_devex -> tt_done = iopex;
      }
      app_devex -> lowiplinprog = 2;
    }
    unlkdb (app_devex, kb);								/* release database */
  }

  /* Post completed write requests */

  if (app_devex -> tt_done != NULL) {							/* see if any write request completed */
    kb = lockdb (app_devex, NULL);							/* lock database */
    iopex = app_devex -> tt_done;							/* get top request on queue */
    if (iopex != NULL) {								/* make sure something still there */
      app_devex -> tt_done = iopex -> next;						/* ok, unlink from list */
      unlkdb (app_devex, kb);								/* release database */
      OZ_KNL_NPPFREE (iopex -> write_buff);						/* free temp buffer */
      iodone (iopex, iopex -> u.write.status, write_iodone, iopex);			/* complete the write request */
      app_devex -> lowiplinprog = 2;
      called_iodone = 1;
    }
    else unlkdb (app_devex, kb);							/* release database */
  }

  /* If no channels assigned and read-ahead buffer is not empty, start logon process */

  if ((app_devex -> chansassnd == 0) && (app_devex -> rahin != app_devex -> rahout)) {
    app_devex -> rahin = app_devex -> rahout;						/* empty read-ahead buffer first */
    oz_knl_logon_devunit (app_devex -> port_devunit);					/* start logon process */
    app_devex -> lowiplinprog = 2;							/* check for other stuff to do */
  }

  /* Start a new read request if the queue has something on it and there isn't already one in progress */
  /* But we can't start it if it has a prompt string and output is currently suspended                 */

  iopex = app_devex -> kb_top;
  if ((iopex != NULL) 									/* - there must be a read on queue */
   && (app_devex -> kb_siz == 0) 							/* - it must not be in progress already */
   && ((iopex -> write_size == iopex -> write_done) 					/* - and there must be no more prompt */
    || !(app_devex -> output_suspend || (app_devex -> tt_status == OZ_PENDING)))) {	/* -     or we must be able to output it */
    app_devex -> kb_echo   = iopex -> u.read.echo;					/* set up echo flag */
    app_devex -> kb_len    = 0;								/* set the data length received so far */
    app_devex -> kb_ins    = 0;								/* set the insertion point */
    app_devex -> kb_buf    = OZ_KNL_NPPMALLOC (iopex -> u.read.size * 2);		/* set the buffer address = a non-paged buffer */
    app_devex -> kb_upd    = app_devex -> kb_buf + iopex -> u.read.size;
    app_devex -> readcolno = 0;								/* don't know where it will echo yet */
    if (iopex -> u.read.timeout != 0) {
      delta = iopex -> u.read.timeout * (OZ_TIMER_RESOLUTION / 1000);			/* convert timeout in milliseconds to datebin delta */
      iopex -> u.read.timeoutat = oz_hw_tod_getnow ();					/* add current time to the delta time */
      OZ_HW_DATEBIN_ADD (iopex -> u.read.timeoutat, delta, iopex -> u.read.timeoutat);
    }
    kb = lockdb (app_devex, iopex -> ioop);						/* get exclusive access to interrupt variables */
    app_devex -> kb_siz = iopex -> u.read.size;						/* set the total buffer size (marks it 'in progress') */
    app_devex -> kb_sts = OZ_DEVOFFLINE;						/* (assume device is offline - port driver closed channel) */
    if (app_devex -> online) {
      app_devex -> kb_sts = OZ_PENDING;							/* request is being started */
      if (iopex -> write_done < iopex -> write_size) {					/* see if there is more prompt to do */
        sts = resume_write (app_devex, iopex);						/* continue writing prompt */
        if (sts == OZ_STARTED) {							/* if transmitter's much too busy, undo everything ... */
          app_devex -> kb_siz = 0;							/* ... read no longer in progress */
          unlkdb (app_devex, kb);							/* ... release interrupt variables */
          OZ_KNL_NPPFREE (app_devex -> kb_buf);						/* ... free off temp buffer */
          app_devex -> kb_buf = NULL;
          app_devex -> kb_upd = NULL;
          goto no_kb_req;
        }
      }
      app_chnex = iopex -> app_chnex;							/* reset read history pointer ... */
      app_chnex -> rhistcur = app_chnex -> rhistin;					/* to point past last line saved */
      (*(app_devex -> port_read_start)) (app_devex -> port_param, 1);			/* tell port driver we are starting a read */
      console_kbd_char (app_devex, 0);							/* maybe there is stuff in the read-ahead buffer */
    }
    unlkdb (app_devex, kb);								/* release interrupt variables */
    app_devex -> kb_timer_expired = 0;							/* say timer has not expired yet */
    if (iopex -> u.read.timeout != 0) {
      if (app_devex -> kb_timer_pending) oz_crash ("oz_dev_conclass lowiplroutine: kb_timer_pending was already set before starting timer");
      app_devex -> kb_timer_pending = 1;						/* start the timeout timer */
      oz_knl_timer_insert (app_devex -> kb_timer, iopex -> u.read.timeoutat, readtimedout, app_devex);
    }
    app_devex -> lowiplinprog = 2;							/* repeat whole loop (because we changed something) */
no_kb_req:
  }

  /* Process any satisfied ctrlchar requests */

ctrlcharck:
  kb = lockdb (app_devex, NULL);							/* get exclusive access to interrupt variables */
  for (lcc = &(app_devex -> ctrlchars); (cc = *lcc) != NULL;) {				/* scan the ctrlchar request list */
    if (!(cc -> u.ctrlchar.satisfied)) {						/* skip it if not yet satisfied */
      lcc = &(cc -> next);
    } else {
      *lcc = cc -> next;								/* satisfied, unlink from list */
      if ((cc -> u.ctrlchar.p.abortread) && (app_devex -> kb_sts == OZ_PENDING)) {	/* maybe abort read request in progress */
        app_devex -> kb_sts = OZ_CTRLCHARABORT;
        (*(app_devex -> port_read_start)) (app_devex -> port_param, 0);			/* tell port driver read is finished */
      }
      unlkdb (app_devex, kb);								/* release queue lock */
      iodone (cc, OZ_SUCCESS, (cc -> u.ctrlchar.p.ctrlchar != NULL) ? ctrlcharfin : NULL, cc); /* post it */
      app_devex -> lowiplinprog = 2;							/* repeat the whole loop */
      called_iodone = 1;
      goto ctrlcharck;									/* go check for more */
    }
  }
  unlkdb (app_devex, kb);								/* release queue lock */

  /* If the keyboard timer has expired, it is no longer pending.  Do this here instead of in the    */
  /* readtimedout routine so that this operation is synchronized with the oz_knl_timer_remove call. */

  if (app_devex -> kb_timer_expired) {
    app_devex -> kb_timer_pending = 0;
    kb = lockdb (app_devex, NULL);
    if (app_devex -> kb_sts == OZ_PENDING) {
      app_devex -> kb_sts = OZ_TIMEDOUT;
      (*(app_devex -> port_read_start)) (app_devex -> port_param, 0);
    }
    unlkdb (app_devex, kb);
  }

  /* Process completed read request if there is something on queue and it is finished */

  iopex = app_devex -> kb_top;
  if ((iopex != NULL) 									/* there has to be a current request */
   && (app_devex -> kb_siz != 0) 							/* ... that has a temp buffer (ie, it has been started) */
   && (app_devex -> kb_sts != OZ_PENDING)) {						/* ... and it must have completed or timed out */
    if (app_devex -> kb_timer_pending && oz_knl_timer_remove (app_devex -> kb_timer)) {	/* (try to kill timer) */
      app_devex -> kb_timer_pending = 0;
    }
    if (!(app_devex -> kb_timer_pending)) {						/* ... and there must be no pending timer */
											/*     (if we weren't able to kill the timer, the readtimedout routine will */
											/*      set the kb_timer_expired flag and call us back, then the above "if */
											/*      kb_timer_expired ..." will clear the kb_timer_pending flag */
											/*     (we must account for the timer before posting the read request so the */
											/*      device doesn't get deleted out from under the readtimedout routine) */
											/* ok, it has completed */
      if (iopex -> u.read.pmtbuff != NULL) {
        OZ_KNL_NPPFREE (iopex -> u.read.pmtbuff);					/* free off prompt buffer */
        iopex -> u.read.pmtbuff = NULL;
      }
      iopex -> u.read.size     = app_devex -> kb_len;					/* save size actually read in */
      iopex -> u.read.tempbuff = app_devex -> kb_buf;					/* save temp buffer pointer */
      kb = lockdb (app_devex, NULL);							/* get exclusive access to interrupt variables */
      sts = app_devex -> kb_sts;							/* get read status */
      app_devex -> kb_siz = 0;								/* no more read request in progress */
      app_devex -> kb_buf = NULL;							/* no more temp buffer */
      app_devex -> kb_upd = NULL;
      app_devex -> kb_top = iopex -> next;						/* unlink the request */
      if (app_devex -> kb_top == NULL) app_devex -> kb_bot = &(app_devex -> kb_top);
      unlkdb (app_devex, kb);								/* release interrupt variables */
      iodone (iopex, sts, read_iodone, iopex);						/* that request is complete */
      called_iodone = 1;

      if (sts == OZ_CTRLCHARABORT) {							/* if that request was aborted by a control character, ... */
        whilst ((iopex = app_devex -> kb_top) != NULL) {				/* ... abort all pending read requests */
          kb = lockdb (app_devex, NULL);
          app_devex -> kb_top = iopex -> next;						/* unlink from queue */
          if (app_devex -> kb_top == NULL) app_devex -> kb_bot = &(app_devex -> kb_top);
          unlkdb (app_devex, kb);
          iodone (iopex, OZ_CTRLCHARABORT, NULL, NULL);					/* that request is complete */
          called_iodone = 1;
        }
      }

      app_devex -> lowiplinprog = 2;							/* loop back to check for more to do */
    }
  }

  /* If we called iodone in here, get the 'closing' flag */

  iochan = NULL;
  if (called_iodone) {
    kb = lockdb (app_devex, NULL);							/* set the database lock */
    iochan = app_devex -> closing;							/* see if the closing flag is set */
    app_devex -> closing = NULL;							/* clear it out, anyway */
    unlkdb (app_devex, kb);								/* release the database lock */
  }

  /* If lowiplinprog is 1, it means we did nothing significant in the loop and no-one else tried to enter this routine, so just set the flag to 0 and exit */
  /* If lowiplinprog is 2, it means we did something significant or someone else tried to enter this routine, so repeat the whole process                  */

  if (app_devex -> lowiplinprog == 2) goto dequeue2;

  /* We're about to leave, maybe for quite a while, so flush the write-behind buffer */

  kb  = lockdb (app_devex, NULL);
  sts = disp_start_flush (app_devex);
  if ((sts != OZ_STARTED) && (app_devex -> tt_status == OZ_SUCCESS)) app_devex -> tt_status = sts;
  unlkdb (app_devex, kb);

  /* Now clear lowiplinprog in an interlocked fashion, retesting it for a 2. */

  if (oz_hw_atomic_set_long (&(app_devex -> lowiplinprog), 0) == 2) goto dequeue;

  /* The lowiplinprog flag is now zero so someone can enter through the top */

  /* If closing flag was set, decrement the iochan's ref count.  This will cause main_deassign to */
  /* be called again, and hopefully it will find that all I/O on this device has been completed.  */

  /* This has to be done after lowiplinprog is cleared, so the main_deassign routine will not think that we are still doing something */

  if (iochan != NULL) oz_knl_iochan_increfc (iochan, -1);
}

/************************************************************************/
/*									*/
/*  This routine gets called at softint level when the kb_timer 	*/
/*  expires.  Note that the read request will not complete until 	*/
/*  either this routine has set kb_timer_expired or the lowiplroutine 	*/
/*  was able to remove the timer entry from the queue.  This prevents 	*/
/*  the device from being deleted out from under us.			*/
/*									*/
/************************************************************************/

static void readtimedout (void *app_devexv, OZ_Timer *timer)

{
  Adevex *app_devex;

  app_devex = app_devexv;
  if (!(app_devex -> kb_timer_pending)) oz_crash ("oz_dev_conclass readtimedout: kb_timer_pending was not set");
  app_devex -> kb_timer_expired = 1;	/* tell lowiplroutine that the timer has expired */
  lowiplroutine (app_devex, NULL);	/* call lowiplroutine to complete the read request */
}

/************************************************************************/
/*									*/
/*  This routine is called to complete an I/O request rather than the 	*/
/*  normal oz_knl_iodone.  This routine credits any quota back that 	*/
/*  was manually debited by the request.				*/
/*									*/
/************************************************************************/

static void iodone (Aiopex *app_iopex, uLong status, void (*finentry) (void *finparam, int finok, uLong *status_r), void *finparam)

{
  if ((app_iopex -> quotanpp != NULL) && (finentry != read_iodone)) {
    oz_knl_quota_credit (app_iopex -> quotanpp, OZ_QUOTATYPE_NPP, app_iopex -> quotasiz, -1);
  }
  oz_knl_iodone (app_iopex -> ioop, status, NULL, finentry, finparam);
}

/************************************************************************/
/*									*/
/*  This routine is called back in the requestor's address space to 	*/
/*  store the control char in user's buffer				*/
/*									*/
/************************************************************************/

static void ctrlcharfin (void *ccv, int finok, uLong *status_r)

{
  Aiopex *cc;
  uLong sts;

  cc = ccv;

  if (finok) {
    sts = oz_knl_section_uput (cc -> procmode, sizeof *(cc -> u.ctrlchar.p.ctrlchar), &(cc -> u.ctrlchar.ctrlchar), cc -> u.ctrlchar.p.ctrlchar);
    if (sts != OZ_SUCCESS) {
      oz_knl_printk ("oz_dev_conclass: error %u storing ctrlchar at %p\n", sts, cc -> u.ctrlchar.p.ctrlchar);
      if (*status_r == OZ_SUCCESS) *status_r = sts;
    }
  }
}

/************************************************************************/
/*									*/
/*  This routine is called when we are back in the originating 		*/
/*  thread's address space and a write has completed			*/
/*									*/
/************************************************************************/

static void write_iodone (void *app_iopexv, int finok, uLong *status_r)

{
  Aiopex *iopex;

  iopex = app_iopexv;

  /* Return the actual number of bytes written */

  if (finok && (iopex -> u.write.wlen != NULL)) *(iopex -> u.write.wlen) = iopex -> write_done;
}

/************************************************************************/
/*									*/
/*  This routine is called when we are back in the originating 		*/
/*  thread's address space and a read has completed			*/
/*									*/
/************************************************************************/

static void read_iodone (void *app_iopexv, int finok, uLong *status_r)

{
  Aiopex *iopex;
  uLong size;

  iopex = app_iopexv;

  if (finok) {
    size = iopex -> u.read.size;						/* get size of string read, not including terminator */
    if (iopex -> u.read.rlen != NULL) *(iopex -> u.read.rlen) = size;		/* return actual length read, not including terminator */
    if ((*status_r == OZ_SUCCESS) || (*status_r == OZ_ENDOFFILE)) size ++;	/* include the terminator if it is there */
    memcpy (iopex -> u.read.userbuff, iopex -> u.read.tempbuff, size);		/* return data read to caller's buffer, including terminator */
  }

  OZ_KNL_NPPFREE (iopex -> u.read.tempbuff);					/* free off non-paged buffer */
  if (iopex -> quotanpp != NULL) {
    oz_knl_quota_credit (iopex -> quotanpp, OZ_QUOTATYPE_NPP, iopex -> quotasiz, -1);
  }
}

/************************************************************************/
/*									*/
/*  This routine is called by the port driver when it has a character 	*/
/*  that came from the keyboard.					*/
/*									*/
/*    Input:								*/
/*									*/
/*	app_devexv = the class device devex pointer (as passed in portinfo.class_param)
/*	bl = 0 : no new char, just process any read-ahead we may have	*/
/*	  else : new character to be processed				*/
/*	smplevel = database locked					*/
/*									*/
/*    Output:								*/
/*									*/
/*	character processed						*/
/*									*/
/************************************************************************/

static const char wordchars[] = "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz0123456789_";

static void console_kbd_char (void *app_devexv, char bl)

{
  Achnex *app_chnex;
  Adevex *app_devex;
  Aiopex *cc;
  int i, num_chars, room_left;
  uLong sts;

  app_devex = app_devexv;

  /* We get called with a null just to process anything that may currently be in the read-ahead buffer */

  if (bl == 0) {
    if (app_devex -> rahin == app_devex -> rahout) return;
    goto check_rah;
  }

  /* Control-S and Control-Q are video output control characters */

  if (bl == 19) {								/* check for ctrl-s */
    app_devex -> output_suspend = 1;						/* if so, set output suspend flag to block future output */
    (*(app_devex -> port_disp_suspend)) (app_devex -> port_param, 1);		/* tell port driver to suspend any output that is in progress */
    goto rtn;
  }
  if (bl == 17) {								/* check for ctrl-q */
    app_devex -> output_suspend = 0;						/* if so, clear output suspend flag to allow future output */
    (*(app_devex -> port_disp_suspend)) (app_devex -> port_param, 0);		/* tell port driver to resume any output that is in progress */
    lowipl_hi (app_devex);							/* resume any suspended output requests */
    if (app_devex -> rahin == app_devex -> rahout) goto rtn;			/* see if any read-ahead, if not we're done */
    goto check_rah;								/* there is read-ahead, maybe resume a suspended input request */
  }

  /* See if control character in the list */

  for (cc = app_devex -> ctrlchars; cc != NULL; cc = cc -> next) {
    if (cc -> u.ctrlchar.satisfied) continue;					/* if already satisfied, ignore it */
    if (!(cc -> u.ctrlchar.p.mask[((uByte)bl)>>5] & (1 << (bl & 31)))) continue; /* if not masked, ignore it */
    cc -> u.ctrlchar.satisfied = 1;						/* ok, mark it satisfied */
    cc -> u.ctrlchar.ctrlchar  = bl;						/* save the character that did it */
    if (cc -> u.ctrlchar.p.wiperah) app_devex -> rahin = app_devex -> rahout = 0; /* maybe wipe read-ahead buffer */
    lowipl_hi (app_devex);							/* complete the request */
    if (cc -> u.ctrlchar.p.terminal) goto rtn;					/* if 'terminal', stop processing this character */
  }

  /* Store the char in the read-ahead buffer.  If read-ahead buffer is already full, ignore it. */

  i = app_devex -> rahin;
  app_devex -> rahbuf[i++] = bl;
  if (i == sizeof app_devex -> rahbuf) i = 0;
  if (i != app_devex -> rahout) app_devex -> rahin = i;
check_rah:

  /* We can assume from this point on that there is at least one char in the read-ahead buffer */

  /* If no channels assigned to device, start login process */

  if (!(app_devex -> chansassnd)) {
    lowipl_hi (app_devex);
    goto rtn;
  }

  /* If the current function is a console_getdat, get everything we can from the read-ahead buffer */

  if ((app_devex -> kb_siz != 0) && (app_devex -> kb_sts == OZ_PENDING) && (app_devex -> kb_top -> funcode == OZ_IO_CONSOLE_GETDAT)) {
    whilst (1) {
      num_chars = (sizeof app_devex -> rahbuf) - app_devex -> rahout;			/* get number of chars in read-ahead */
      if (app_devex -> rahin >= app_devex -> rahout) num_chars = app_devex -> rahin - app_devex -> rahout;
      room_left = app_devex -> kb_siz - app_devex -> kb_len;				/* get room left in caller's buffer */
      if (num_chars > room_left) num_chars = room_left;
      if (num_chars == 0) break;							/* if nothing left to copy, exit loop */
      memcpy (app_devex -> kb_buf + app_devex -> kb_len, app_devex -> rahbuf + app_devex -> rahout, num_chars);
      app_devex -> rahout += num_chars;
      app_devex -> kb_len += num_chars;
      if (app_devex -> rahout == sizeof app_devex -> rahbuf) app_devex -> rahout = 0;
    }
    app_devex -> kb_sts = OZ_SUCCESS;
    (*(app_devex -> port_read_start)) (app_devex -> port_param, 0);			/* finished a request */
    lowipl_hi (app_devex);
    goto rtn;
  }

  /* Process any currently active read request */

  if (app_devex -> inkbdloop) return;					/* in case one of the echoing calls below tries to call us back */
  app_devex -> inkbdloop = 1;						/* keep echoing call from calling us back */

  whilst ((app_devex -> kb_siz != 0) 					/* make sure there is a read request in progress */
      && (app_devex -> kb_sts == OZ_PENDING) 				/* make sure that read request is still pending */
      && (app_devex -> rahout != app_devex -> rahin)) {
    char *ctlx;

    if (app_devex -> rahout < app_devex -> rahin) {
      ctlx = memchr (app_devex -> rahbuf + app_devex -> rahout + 1, 24, app_devex -> rahin - app_devex -> rahout - 1);
    } else {
      ctlx = memchr (app_devex -> rahbuf + app_devex -> rahout + 1, 24, sizeof app_devex -> rahbuf - app_devex -> rahout - 1);
      if (ctlx == NULL) ctlx = memchr (app_devex -> rahbuf, 24, app_devex -> rahin);
    }
    if (ctlx == NULL) break;
    app_devex -> rahout = ctlx - app_devex -> rahbuf + 1;
    if (app_devex -> rahout == sizeof app_devex -> rahbuf) app_devex -> rahout = 0;
  }

  whilst (!(app_devex -> kb_echo && app_devex -> output_suspend) 	/* make sure output not suspended (via such as ^s) */
      && !(app_devex -> kb_echo && (app_devex -> tt_status == OZ_PENDING)) /* make sure transmitter is able to process echos */
      && (app_devex -> kb_siz != 0) 					/* make sure there is a read request in progress */
      && (app_devex -> kb_sts == OZ_PENDING) 				/* make sure that read request is still pending */
      && (app_devex -> rahout != app_devex -> rahin)) {			/* make sure there is something in read-ahead buffer */

    bl = app_devex -> rahbuf[app_devex->rahout];			/* get a character from read-ahead buffer */

    /* Process character for CONSOLE_READ function */

    switch (bl) {

      /* Null - finish an update.  Note that this assumes a null will   */
      /* not be placed in read-ahead by the above normal input means.   */
      /* Here, the kb_upd is already filled in, kb_buf may be partially */
      /* updated, and new_kb_ins and new_kb_len have the saved values.  */

      case 0: {
        if (!echo_update (app_devex, app_devex -> save_same, app_devex -> new_kb_ins, app_devex -> new_kb_len)) goto rtn;
        if (app_devex -> kb_len == app_devex -> kb_siz) {	/* see if buffer is completely full */
          app_devex -> kb_sts = OZ_NOTERMINATOR;		/* if so, request is complete */
          (*(app_devex -> port_read_start)) (app_devex -> port_param, 0);
          lowipl_hi (app_devex);
        }
        break;
      }

      /* Control-B - back up one entry in read history */

      case 2: {
        uWord len, lenr;

        app_chnex = app_devex -> kb_top -> app_chnex;
        if (app_devex -> kb_echo && (app_chnex -> rhistcur != app_chnex -> rhistout)) {		/* make sure there's something to back up to */
          if (app_chnex -> rhistcur == 0) app_chnex -> rhistcur = app_chnex -> rhistsiz;	/* maybe have to wrap to get length */
          app_chnex -> rhistcur -= sizeof len;							/* point to the ending length word */
          len  = *((uWord *)(app_chnex -> rhistbuf + app_chnex -> rhistcur));			/* retrieve it */
          lenr = (len + sizeof len - 1) & - (sizeof len);					/* get rounded length */
          if (app_chnex -> rhistcur <= lenr) app_chnex -> rhistcur += app_chnex -> rhistsiz; 	/* maybe wrap to get beg length word */
          app_chnex -> rhistcur -= lenr + sizeof len;						/* point to beg length word */
          if (len > app_devex -> kb_siz - 1) len = app_devex -> kb_siz - 1;			/* chop saved string to fit in current buffer */
          if (app_chnex -> rhistcur + len + sizeof len <= app_chnex -> rhistsiz) {		/* see if it is all together */
            memcpy (app_devex -> kb_upd, app_chnex -> rhistbuf + app_chnex -> rhistcur + sizeof len, len); /* if so, copy at once */
          } else {
            lenr = app_chnex -> rhistsiz - app_chnex -> rhistcur - sizeof len;			/* two pieces, get len of first */
            memcpy (app_devex -> kb_upd, app_chnex -> rhistbuf + app_chnex -> rhistcur + sizeof len, lenr); /* copy first piece */
            memcpy (app_devex -> kb_upd + lenr, app_chnex -> rhistbuf, len - lenr);		/* copy second piece (wrapped) */
          }
          if (!echo_update (app_devex, 0, len, len)) goto rtn;					/* anway, update screen */
        }
        break;
      }

      /* Control-E - move to end of line (leave data intact) */

      case 5: {
        if (app_devex -> kb_ins != app_devex -> kb_len) {
          if (!echo_update (app_devex, app_devex -> kb_len, app_devex -> kb_len, app_devex -> kb_len)) goto rtn;
        }
        break;
      }

      /* Control-F - go forward one entry in read history */

      case 6: {
        uWord len, lenr;

        app_chnex = app_devex -> kb_top -> app_chnex;
        if (app_devex -> kb_echo && (app_chnex -> rhistcur != app_chnex -> rhistin)) {		/* make sure there's something to go forward to */
          len  = *((uWord *)(app_chnex -> rhistbuf + app_chnex -> rhistcur));			/* retrieve beginning length word */
          lenr = (len + sizeof len - 1) & - (sizeof len);					/* get rounded length */
          app_chnex -> rhistcur += lenr + 2 * sizeof len;					/* skip forward over current entry */
          if (app_chnex -> rhistcur >= app_chnex -> rhistsiz) app_chnex -> rhistcur -= app_chnex -> rhistsiz;
          len = 0;										/* assume we're at very end now */
          if (app_chnex -> rhistcur != app_chnex -> rhistin) {					/* make sure something's there */
            len = *((uWord *)(app_chnex -> rhistbuf + app_chnex -> rhistcur));			/* retrieve its length word */
            if (len > app_devex -> kb_siz - 1) len = app_devex -> kb_siz - 1;			/* chop saved string to fit in current buffer */
            if (app_chnex -> rhistcur + len + sizeof len <= app_chnex -> rhistsiz) {		/* see if it is all together */
              memcpy (app_devex -> kb_upd, app_chnex -> rhistbuf + app_chnex -> rhistcur + sizeof len, len); /* if so, copy at once */
            } else {
              lenr = app_chnex -> rhistsiz - app_chnex -> rhistcur - sizeof len;		/* two pieces, get len of first */
              memcpy (app_devex -> kb_upd, app_chnex -> rhistbuf + app_chnex -> rhistcur + sizeof len, lenr); /* copy first piece */
              memcpy (app_devex -> kb_upd + lenr, app_chnex -> rhistbuf, len - lenr);		/* copy second piece (wrapped) */
            }
          }
          if (!echo_update (app_devex, 0, len, len)) goto rtn;					/* anway, update screen */
        }
        break;
      }

      /* Backspace - move to beginning of line (leave data intact) */

      case 8: {
        if (app_devex -> kb_ins != 0) {
          if (!echo_update (app_devex, app_devex -> kb_len, 0, app_devex -> kb_len)) goto rtn;
        }
        break;
      }

      /* Control-J - delete word to left of cursor */

      case 10: {
        uLong new_kb_ins;

        new_kb_ins = app_devex -> kb_ins;
        if (new_kb_ins > 0) {
          do bl = app_devex -> kb_buf[--new_kb_ins];
          while ((new_kb_ins > 0) && ((bl == ' ') || (strchr (wordchars, app_devex -> kb_buf[new_kb_ins-1]) != NULL)));
          memcpy (app_devex -> kb_upd + new_kb_ins, app_devex -> kb_buf + app_devex -> kb_ins, app_devex -> kb_len - app_devex -> kb_ins);
          if (!echo_update (app_devex, new_kb_ins, new_kb_ins, new_kb_ins + app_devex -> kb_len - app_devex -> kb_ins)) goto rtn;
        }
        break;
      }

      /* Control-L - move left one char (leave data intact) */

      case 12: {
        if (app_devex -> kb_ins > 0) {
          if (!echo_update (app_devex, app_devex -> kb_len, app_devex -> kb_ins - 1, app_devex -> kb_len)) goto rtn;
        }
        break;
      }

      /* Control-R - move right one char (leave data intact) */

      case 18: {
        if (app_devex -> kb_ins < app_devex -> kb_len) {
          if (!echo_update (app_devex, app_devex -> kb_len, app_devex -> kb_ins + 1, app_devex -> kb_len)) goto rtn;
        }
        break;
      }

      /* Control-U / Control-X - wipe all data to left of cursor */

      case 21:
      case 24: {
        if (app_devex -> kb_ins != 0) {
          memcpy (app_devex -> kb_upd, 
                  app_devex -> kb_buf + app_devex -> kb_ins, 
                  app_devex -> kb_len - app_devex -> kb_ins);
          if (!echo_update (app_devex, 0, 0, app_devex -> kb_len - app_devex -> kb_ins)) goto rtn;
        }
        break;
      }

      /* Rubout - wipe one character to left of cursor */

      case 127: {
        if (app_devex -> kb_ins != 0) {				/* see if there is something to rub out */
          memcpy (app_devex -> kb_upd + app_devex -> kb_ins - 1, 
                  app_devex -> kb_buf + app_devex -> kb_ins, 
                  app_devex -> kb_len - app_devex -> kb_ins);
          if (!echo_update (app_devex, app_devex -> kb_ins - 1, app_devex -> kb_ins - 1, app_devex -> kb_len - 1)) goto rtn;
        }
        break;
      }

      /* Check for terminators (control-D, carriage return, control-Z, escape) */

      case  4:
      case 13:
      case 26:
      case 27: {
        uLong endcol, endrow;

        if (app_devex -> kb_echo) {

          /* Maybe echo terminator.  If transmitter too busy, we'll come back later when it isn't busy and re-process terminator. */
          /* To echo terminator, we place cursor just past last char input (if not there already), then we output a <CR><LF>.     */

          if (app_devex -> kb_len == 0) {					/* see if there's anything of the read on screen */
            app_devex -> readcolno  = app_devex -> columnumber;			/* if not, read starts here */
            app_devex -> blankspot -= app_devex -> linenumber * app_devex -> screenwidth;
            app_devex -> linenumber = 0;
          }
          fakeformat (app_devex -> kb_len, &endcol, &endrow, app_devex);	/* figure out where the terminator char goes */
          if ((app_devex -> kb_len != 0) && (endrow > app_devex -> linenumber)) { /* see if we are already on that line */
            sts = disp_start_escbuf (app_devex, ESC_CURDN "\r\n", endrow - app_devex -> linenumber); /* if not, get there */
          } else {
            sts = disp_start_string (app_devex, 2, "\r\n");			/* if so, get to beg of next line */
          }
          if (sts == OZ_STARTED) goto rtn;
          app_devex -> columnumber = 1;						/* either way, we're now in first column */
          app_devex -> linenumber  = endrow + 1;				/* ... of line following last char input */

          /* If it will fit, store data in the read history for later retrieval via ctl-B and -F */
          /* Don't bother if it's a blank line or the same as last line in history.              */

          app_chnex = app_devex -> kb_top -> app_chnex;
          if ((app_devex -> kb_len > 0) && (app_devex -> kb_len < app_chnex -> rhistsiz - 3 * sizeof (uWord))) {
            uWord in, len, lenr, room;

            if (app_chnex -> rhistin != app_chnex -> rhistout) {				/* see if anything in history */
              in = app_chnex -> rhistin;							/* point to end of last entry */
              if (in == 0) in = app_chnex -> rhistsiz;
              in  -= sizeof len;								/* point to end length of last entry */
              len  = *((uWord *)(app_chnex -> rhistbuf + in));					/* get length of last entry */
              lenr = (len + sizeof len - 1) & - (sizeof len);					/* round it up */
              if (len == app_devex -> kb_len) {							/* see if length matches */
                if (in >= lenr) {								/* ok, see if it's all together */
                  if (memcmp (app_chnex -> rhistbuf + in - lenr, app_devex -> kb_buf, len) == 0) goto rhistdontbother;
                } else {
                  if ((memcmp (app_chnex -> rhistbuf + app_chnex -> rhistsiz + in - lenr, app_devex -> kb_buf, lenr - in) == 0) 
                   && (memcmp (app_chnex -> rhistbuf, app_devex -> kb_buf + lenr - in, in + len - lenr) == 0)) goto rhistdontbother;
                }
              }
            }

            while (1) {
              room = app_chnex -> rhistout - app_chnex -> rhistin;				/* see how much room is avail */
              if (app_chnex -> rhistout <= app_chnex -> rhistin) room += app_chnex -> rhistsiz;
              if (room >= app_devex -> kb_len + 3 * sizeof len) break;				/* stop if we have enough */
              len  = *((uWord *)(app_chnex -> rhistbuf + app_chnex -> rhistout));		/* no room, remove an entry */
              lenr = (len + sizeof len - 1) & - (sizeof len);
              app_chnex -> rhistout += lenr + 2 * sizeof len;
              if (app_chnex -> rhistout >= app_chnex -> rhistsiz) app_chnex -> rhistout -= app_chnex -> rhistsiz;
            }
            len  = app_devex -> kb_len;								/* get text length to be stored */
            lenr = (len + sizeof len - 1) & - (sizeof len);					/* get rounded length */
            *((uWord *)(app_chnex -> rhistbuf + app_chnex -> rhistin)) = len;			/* store beginning length */
            app_chnex -> rhistin += sizeof len;
            if (app_chnex -> rhistin == app_chnex -> rhistsiz) app_chnex -> rhistin = 0;
            room = app_chnex -> rhistsiz - app_chnex -> rhistin;				/* get room following length */
            if (room >= len) {									/* see if text will fit */
              memcpy (app_chnex -> rhistbuf + app_chnex -> rhistin, app_devex -> kb_buf, len);	/* ok, stuff it all at once */
            } else {
              memcpy (app_chnex -> rhistbuf + app_chnex -> rhistin, app_devex -> kb_buf, room);	/* needs two pieces */
              memcpy (app_chnex -> rhistbuf, app_devex -> kb_buf + room, len - room);
            }
            app_chnex -> rhistin += lenr;							/* inc to end length spot */
            if (app_chnex -> rhistin >= app_chnex -> rhistsiz) app_chnex -> rhistin -= app_chnex -> rhistsiz;
            *((uWord *)(app_chnex -> rhistbuf + app_chnex -> rhistin)) = len;			/* store end length */
            app_chnex -> rhistin += sizeof len;
            if (app_chnex -> rhistin == app_chnex -> rhistsiz) app_chnex -> rhistin = 0;
          }
rhistdontbother:;
        }

        /* Anyway, finish off the read */

        app_devex -> kb_buf[app_devex->kb_len] = bl;				/* save the terminator */
        app_devex -> kb_sts = OZ_ENDOFFILE;					/* assume an end-of-file terminator */
        if (bl & 1) app_devex -> kb_sts = OZ_SUCCESS;				/* success terminator if cr or esc */
        (*(app_devex -> port_read_start)) (app_devex -> port_param, 0);		/* tell port driver we're done */
        lowipl_hi (app_devex);							/* complete request */
        break;
      }

      /* Process printables if room in buffer for them, ignore all other junk */

      default: {
        int filled;
        uLong new_kb_ins, same;

        if (((bl >= ' ') || (bl == 9)) && (app_devex -> kb_len < app_devex -> kb_siz)) {
          same = app_devex -> kb_ins;
          app_devex -> kb_upd[same] = bl;				/* insert character into line */
          memcpy (app_devex -> kb_upd + same + 1, app_devex -> kb_buf + same, app_devex -> kb_len - same);
          filled = (app_devex -> kb_len == app_devex -> kb_siz);	/* see if buffer is completely full */
          if (!filled) new_kb_ins = same + 1;				/* if not, cursor goes over one to right */
          else new_kb_ins = app_devex -> kb_len;			/* if so, position cursor to end of buffer */
          if (!echo_update (app_devex, same, new_kb_ins, app_devex -> kb_len + 1)) goto rtn;
          if (filled) {						
            app_devex -> kb_sts = OZ_NOTERMINATOR;			/* full, request is complete */
            (*(app_devex -> port_read_start)) (app_devex -> port_param, 0);
            lowipl_hi (app_devex);
          }
        }
      }
    }

    /* Now that keyboard character has been processed, remove it from read-ahead buffer */

    app_devex -> rahout ++;
    if (app_devex -> rahout == sizeof app_devex -> rahbuf) app_devex -> rahout = 0;

    /* Repeat for more characters in read-ahead buffer */
  }

  /* We may be gone for a while, so start flushing transmit buffer */

rtn:
  app_devex -> inkbdloop = 0;
  sts = disp_start_flush (app_devex);
  if ((sts != OZ_STARTED) && (app_devex -> tt_status == OZ_SUCCESS)) app_devex -> tt_status = sts;
}

/************************************************************************/
/*									*/
/*  Echo the update to screen						*/
/*									*/
/*    Input:								*/
/*									*/
/*	kb_buf = old contents						*/
/*	kb_upd = new contents						*/
/*	same = number of chars at beg of kb_buf that are staying the 	*/
/*	       same (they were not copied to kb_upd but pretend they 	*/
/*	       were)							*/
/*	new_kb_ins = new value for kb_ins				*/
/*	new_kb_len = new value for kb_len				*/
/*									*/
/*    Output:								*/
/*									*/
/*	echo_update = 0 : update not complete, call back to reprocess 	*/
/*	                  when transmitter no longer busy		*/
/*	              1 : update complete				*/
/*	                  kb_ins  = new_kb_ins				*/
/*	                  kb_len  = new_kb_len				*/
/*	                  *kb_buf = *kb_upd				*/
/*									*/
/************************************************************************/

static int echo_update (Adevex *app_devex, uLong same, uLong new_kb_ins, uLong new_kb_len)

{
  Aiopex *app_iopex;
  uLong endcol, endrow, newcol, newrow, sts;

  app_iopex = app_devex -> kb_top;

  /* If not echoing, just apply update and return success */

  if (!(app_devex -> kb_echo)) {
    app_devex -> kb_ins = new_kb_ins;
    app_devex -> kb_len = new_kb_len;
    memcpy (app_devex -> kb_buf + same, app_devex -> kb_upd + same, new_kb_len - same);
    return (1);
  }

  /* See how much more of the two buffers is the same - we won't re-output this much */

  for (; (same < app_devex -> kb_len) && (same < new_kb_len); same ++) {
    if (app_devex -> kb_buf[same] != app_devex -> kb_upd[same]) break;
  }

  /* See where cursor has to be for the stuff that's different to be displayed */

  if (app_devex -> kb_len == 0) {			/* see if there's anything of the read on screen */
    app_devex -> readcolno  = app_devex -> columnumber;	/* if not, read starts here */
    app_devex -> blankspot -= app_devex -> linenumber * app_devex -> screenwidth;
    app_devex -> linenumber = 0;
  }
  fakeformat (same, &newcol, &newrow, app_devex);

  /* If cursor isn't there, and we have something to do there (like output data or erase), put it there */

  if ((newcol != app_devex -> columnumber) || (newrow != app_devex -> linenumber)) {
    if ((same < new_kb_len) || (app_devex -> blankspot > newrow * app_devex -> screenwidth + newcol)) {
      if (!setcurpos (app_devex, "", newrow, newcol)) goto busy;
    }
  }

  /* Now output data that goes at the new cursor's spot, then update kb_buf with updated data.  If transmitter */
  /* is too busy to finsh it, just update what it did do then come back later when it can finish up.           */
  /* Hopefully each time we come back, 'same' will be a little greater and eventually we should finish.        */

  endcol = newcol;							/* this is the end of everything so far */
  endrow = newrow;
  if (same < new_kb_len) {						/* well maybe there's some more to output */
    app_iopex -> write_format = 1;
    app_iopex -> write_done   = 0;
    app_iopex -> write_size   = new_kb_len - same;
    app_iopex -> write_buff   = app_devex -> kb_upd + same;
    sts = resume_write (app_devex, app_iopex);
    memcpy (app_devex -> kb_buf + same, app_devex -> kb_upd + same, app_iopex -> write_done);
    same += app_iopex -> write_done;					/* that much more is now the same */
    if (app_devex -> kb_len < same) app_devex -> kb_len = same;
    if (sts == OZ_STARTED) goto busy;
    endcol = app_devex -> columnumber;					/* now this is the end of everything */
    endrow = app_devex -> linenumber;
  }

  /* Finally erase to end of screen then put cursor at new insertion spot */

  fakeformat (new_kb_ins, &newcol, &newrow, app_devex);			/* find new insertion point */
  if (endrow * app_devex -> screenwidth + endcol >= app_devex -> blankspot) {
    if (!setcurpos (app_devex, "", newrow, newcol)) goto busy;
  } else {
    if (!setcurpos (app_devex, ESC_EREOS, newrow, newcol)) goto busy;
    app_devex -> blankspot = endrow * app_devex -> screenwidth + endcol;
  }
  app_devex -> kb_ins = new_kb_ins;					/* save new insertion point */
  app_devex -> kb_len = new_kb_len;					/* save new length */
  return (1);								/* successful */

  /* Transmitter is too busy to process echoing.  So stuff a null back in read-ahead so we will be called back. */

busy:
  app_devex -> new_kb_ins = new_kb_ins;					/* save in devex for now */
  app_devex -> new_kb_len = new_kb_len;
  app_devex -> save_same  = same;
  app_devex -> rahbuf[app_devex->rahout] = 0;				/* replace update char with a null so we get called back */
  return (0);
}

/************************************************************************/
/*									*/
/*  Set new cursor position (after doing the initstr)			*/
/*									*/
/*    Input:								*/
/*									*/
/*	initstr = small string to output first				*/
/*	newrow  = new row to position to				*/
/*	newcol  = new column to position to				*/
/*									*/
/*    Output:								*/
/*									*/
/*	setcurpos = 0 : escbuf or transmitter busy, try again later	*/
/*	         else : update complete					*/
/*	app_devex -> columnumber = newcol				*/
/*	app_devex -> linenumber  = newrow				*/
/*									*/
/************************************************************************/

static int setcurpos (Adevex *app_devex, const char *initstr, uLong newrow, uLong newcol)

{
  char *p;
  uLong n, sts;

  /* Fill in escbuf with the required string to move cursor relative to where it is now */

  p  = app_devex -> escbuf;
  strcpy (p, initstr);
  p += strlen (p);
  n  = app_devex -> columnumber;
  if (newcol > n) oz_sys_sprintf (10, p, ESC_CURFW, newcol - n);
  if (n > newcol) oz_sys_sprintf (10, p, ESC_CURBK, n - newcol);
  p += strlen (p);
  n  = app_devex -> linenumber;
  if (newrow > n) oz_sys_sprintf (10, p, ESC_CURDN, newrow - n);
  if (n > newrow) oz_sys_sprintf (10, p, ESC_CURUP, n - newrow);

  /* Copy to write-behind buffer.  If too busy, return failure. */

  sts = disp_start_escbuf (app_devex, NULL);
  if (sts == OZ_STARTED) return (0);

  /* Ok, update current column number and return success */

  app_devex -> columnumber = newcol;
  app_devex -> linenumber  = newrow;
  return (1);
}

/************************************************************************/
/*									*/
/*  Advance *col_r and *row_r as if the kb_buf were output		*/
/*									*/
/*    Input:								*/
/*									*/
/*	size = number of chars in kb_buf to process			*/
/*	app_devex = device						*/
/*									*/
/*    Output:								*/
/*									*/
/*	*col_r = where to put kb_buf[size] character			*/
/*	*row_r = where to put kb_buf[size] character			*/
/*									*/
/************************************************************************/

static void fakeformat (uLong size, uLong *col_r, uLong *row_r, Adevex *app_devex)

{
  uByte *buff;
  uLong colno, rowno, width;

  colno = app_devex -> readcolno;			/* point to where first char is / goes */
  rowno = 0;

  width = app_devex -> screenwidth;
  buff  = app_devex -> kb_buf;

  /* Scan the buffer and count the columns */

  whilst (size > 0) {
    -- size;
    switch (*(buff ++)) {

      /* Ignore these control characters and rubouts */

      case   0:    case   1:    case   2:    case   3:    case   4:    case   5:    case   6:
      case   7:    case  11:    case  12:    case  14:    case  15:    case  16:    case  17:
      case  18:    case  19:    case  20:    case  21:    case  22:    case  23:    case  24:
      case  25:    case  26:    case  27:    case  28:    case  29:    case  30:    case  31:
      case 127: {
        break;
      }

      /* Backspace - ingnore if already at beginning of line */

      case  8: {
        if (colno > 1) colno --;
        break;
      }

      /* Linefeed - just advance to same or first column in next line */

      case 10: {
        rowno ++;
        if (!(app_devex -> output_bare_lf)) colno = 1;
        break;
      }

      /* Carriage return */

      case 13: {
        colno = 1;
        break;
      }

      /* Tab - up to 8 spaces but don't wrap */

      case 9: {
        int nspaces;

        nspaces = 8 - ((colno - 1) & 7);				/* see how many spaces we need */
        colno  += nspaces;						/* advance column number by that much */
        if (colno > width) colno = width;				/* but don't go past last column on line */
        break;
      }

      /* Otherwise assume it's a printable */

      default: {
        if (colno < width) colno ++;					/* if we're not at right margin, char moves us one column */
        else if (app_devex -> sw_wrapping) {				/* otherwise, see if software wrapping is enabled */
          colno = 1;							/* if so, the char will put us in first column ... */
          rowno ++;							/* ... of the next line */
        }
      }
    }
  }

  *col_r = colno;
  *row_r = rowno;
}

/************************************************************************/
/*									*/
/*  Resume processing the write_done/_size/_buff portion of iopex	*/
/*									*/
/*    Input:								*/
/*									*/
/*	app_devex = device the i/o is going to				*/
/*	app_iopex = request being processed				*/
/*	          -> write_format = do we format the data		*/
/*	          -> write_done   = amount in write_buff already done	*/
/*	          -> write_size   = total amount of data in write_buff	*/
/*	          -> write_buff   = points to beginning of data		*/
/*									*/
/*    Output:								*/
/*									*/
/*	resume_write = OZ_STARTED : can't output it all now, flush started
/*	               OZ_SUCCESS : completed				*/
/*	                     else : error status			*/
/*									*/
/************************************************************************/

static uLong resume_write (Adevex *app_devex, Aiopex *app_iopex)

{
  uByte c;
  uLong colno, done, linno, sts, width;

  colno = app_devex -> columnumber;
  linno = app_devex -> linenumber;
  width = app_devex -> screenwidth;

  sts = OZ_SUCCESS;

  for (done = app_iopex -> write_done; done < app_iopex -> write_size; done ++) {
    c = app_iopex -> write_buff[done];		/* get the character */
    if ((app_iopex -> write_format) && (colno != 0)) {
      switch (c) {

        /* Ignore these control characters and rubouts                   */
        /* Note that we get here for backspace when we're at beg of line */

        case   0:    case   1:    case   2:    case   3:    case   4:    case   5:    case   6:
        case  11:    case  12:    case  14:    case  15:
        case  16:    case  17:    case  18:    case  19:    case  20:    case  21:    case  22:
        case  23:    case  24:    case  25:    case  26:    case  27:    case  28:    case  29:
        case  30:    case  31:    case 127: {
          break;
        }

        /* Bell - pass on to output but don't count any columns */

        case 7: {
          sts = disp_start_string (app_devex, 1, &c);
          break;
        }

        /* Backspace - backs up one column to the left if not in left column */

        case 8: {
          if (colno > 1) {
            colno --;
            sts = disp_start_string (app_devex, 1, &c);
          }
          break;
        }

        /* Tab - output spaces to next multiple of 8 but don't wrap (just get stuck at right margin) */

        case 9: {
          int nspaces;

          nspaces = 8 - ((colno - 1) & 7);					/* see how many spaces to next tab stop */
          if (colno + nspaces <= width) {					/* see if it takes us to wrap point */
            sts = disp_start_string (app_devex, nspaces, "        ");
            colno += nspaces;						/* accepted, update current column number */
          } else {
            nspaces = width - colno;					/* it would wrap, erase then skip to last column */
            if (linno * width + colno >= app_devex -> blankspot) {
              if (nspaces != 0) sts = disp_start_escbuf (app_devex, ESC_CURFW, nspaces);
            } else {
              if (nspaces == 0) sts = disp_start_escbuf (app_devex, ESC_EREOS);
              else sts = disp_start_escbuf (app_devex, ESC_EREOS ESC_CURFW, nspaces);
              if (sts == OZ_SUCCESS) app_devex -> blankspot = linno * width + colno;
            }
            colno = width;							/* we are now at the right margin */
          }
          break;
        }

        /* Linefeed - goes to same column in next line */

        case 10: {
          if (app_devex -> output_bare_lf) {
            sts = disp_start_string (app_devex, 1, "\n");
          } else {
            sts = disp_start_string (app_devex, 2, "\r\n");
            colno = 1;
          }
          linno ++;
          break;
        }

        /* Carriage return - goes to column 1 of current line */

        case 13: {
          colno = 1;
          sts = disp_start_string (app_devex, 1, "\r");
          break;
        }

        /* Otherwise assume it's a printable */

        default: {
          if (colno < width) {
            sts = disp_start_string (app_devex, 1, &c);
            colno ++;
          }
          else switch (((app_devex -> hw_wrapping) << 1) + app_devex -> sw_wrapping) {
            case 0: {		/* !hw_wrapping && !sw_wrapping */
              sts = disp_start_string (app_devex, 1, &c);
              break;
            }
            case 1: {		/* !hw_wrapping &&  sw_wrapping */
              sts = disp_start_escbuf (app_devex, "%c\r\n", c);
              colno = 1;
              linno ++;
              break;
            }
            case 2: {		/*  hw_wrapping && !sw_wrapping */
              sts = disp_start_escbuf (app_devex, "%c" ESC_CURUP ESC_CURFW, c, 1, width - 1);
              break;
            }
            case 3: {		/*  hw_wrapping &&  sw_wrapping */
              sts = disp_start_string (app_devex, 1, &c);
              colno = 1;
              linno ++;
              break;
            }
          }
          if ((sts == OZ_SUCCESS) && (c > ' ')) {
            uLong spot;

            spot = linno * width + colno;
            if (spot > app_devex -> blankspot) app_devex -> blankspot = spot;
          }
          break;
        }
      }

      /* If unable to output the data, stop here and return status */

      if (sts != OZ_SUCCESS) break;

      /* Data was output ok, update column and line numbers */

      app_devex -> columnumber = colno;
      app_devex -> linenumber  = linno;
    }

    /* Unformatted output - just copy to wbhbuf */

    else {
      sts = disp_start_string (app_devex, 1, &c);
      if (sts != OZ_SUCCESS) break;
    }
  }

  /* Either error or all done, save how much was done and return status */

  app_iopex -> write_done = done;
  return (sts);
}

/************************************************************************/
/*									*/
/*  Format string into escbuf and start displaying it			*/
/*									*/
/*    Input:								*/
/*									*/
/*	app_devex  = device it is to be displayed on			*/
/*	format ... = formatting arguments				*/
/*	             if NULL, escbuf is already filled in		*/
/*									*/
/*    Output:								*/
/*									*/
/*	disp_start_escbuf = OZ_SUCCESS : successfully completed		*/
/*	                    OZ_STARTED : not done, flush started	*/
/*	                          else : error status			*/
/*									*/
/************************************************************************/

static uLong disp_start_escbuf (Adevex *app_devex, const char *format, ...)

{
  uLong sts;
  va_list ap;

  if (format != NULL) {					/* maybe escbuf is already filled in */
    va_start (ap, format);				/* if not, format string to be displayed */
    oz_sys_vsprintf (sizeof app_devex -> escbuf, app_devex -> escbuf, format, ap);
    va_end (ap);
  }
  sts = disp_start_string (app_devex, strlen (app_devex -> escbuf), app_devex -> escbuf);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Copy string to the wbhbuf						*/
/*									*/
/*    Input:								*/
/*									*/
/*	size = number of chars to copy					*/
/*	buff = where the chars are					*/
/*									*/
/*    Output:								*/
/*									*/
/*	disp_start_string = OZ_SUCCESS : successfully copied		*/
/*	                    OZ_STARTED : not enough room, flush started	*/
/*	                          else : flush error status		*/
/*									*/
/************************************************************************/

static uLong disp_start_string (Adevex *app_devex, uLong size, const void *buff)

{
  uLong room, sts;

  /* See if there is enough room for the whole thing in the wbhbuf. */
  /* If not, start flushing it and return OZ_STARTED.               */

  if (size >= sizeof app_devex -> wbhbuf) oz_crash ("oz_dev_conclass: write size %u too big for wbhbuf", size);

checkroom:
  room = app_devex -> wbhout - app_devex -> wbhin;
  if (room == 0) app_devex -> wbhin = app_devex -> wbhout = 0;
  if (app_devex -> wbhout <= app_devex -> wbhin) room += sizeof app_devex -> wbhbuf;
  room --;
  if (size > room) {
    sts = disp_start_flush (app_devex);
    if (sts == OZ_SUCCESS) goto checkroom;
    return (sts);
  }

  /* Ok, copy string to the wbhbuf and increment wbhin */

  whilst (size > 0) {
    room = sizeof app_devex -> wbhbuf - app_devex -> wbhin;
    if (room > size) room = size;
    memcpy (app_devex -> wbhbuf + app_devex -> wbhin, buff, room);
    size -= room;
    ((const uByte *)buff) += room;
    app_devex -> wbhin += room;
    if (app_devex -> wbhin == sizeof app_devex -> wbhbuf) app_devex -> wbhin = 0;
  }

  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Flush the wbhbuf to the port driver					*/
/*									*/
/*    Output:								*/
/*									*/
/*	disp_start_flush = OZ_SUCCESS : flush completed			*/
/*	                   OZ_STARTED : flush started			*/
/*	                         else : error status			*/
/*									*/
/************************************************************************/

static uLong disp_start_flush (Adevex *app_devex)

{
  char *buff;
  uLong size, sts;

  /* Check current transmit status.  If something is going, lie and say we started this (we will when console_displayed gets called) */
  /* Otherwise, if a previous transmit failed, return the error status but reset to success so we only return the error code once */

  sts = app_devex -> tt_status;
  if (sts == OZ_PENDING) return (OZ_STARTED);
  if (sts != OZ_SUCCESS) {
    app_devex -> tt_status = OZ_SUCCESS;
    return (sts);
  }

  /* See how much there is to do, if nothing, return success status */

  size = app_devex -> wbhin - app_devex -> wbhout;
  if (size == 0) return (OZ_SUCCESS);
  if (app_devex -> wbhin < app_devex -> wbhout) size = sizeof app_devex -> wbhbuf - app_devex -> wbhout;

  /* Mark the transmitter busy and start outputting the buffer */

  app_devex -> tt_status = OZ_PENDING;
  sts = (*(app_devex -> port_disp_start)) (app_devex -> port_param, 
                                           (void *)(OZ_Pointer)size, 
                                           size, 
                                           app_devex -> wbhbuf + app_devex -> wbhout);

  /* If it finished writing it already, mark that portion of buffer as done and flag transmitter idle   */
  /* OZ_STARTED means it is actually outputting it and will call console_displayed when it has finished */
  /* We should never get OZ_QUEUEFULL because we only do one write at a time                            */

  if (sts != OZ_STARTED) {
    app_devex -> wbhout += size;
    if (app_devex -> wbhout == sizeof app_devex -> wbhbuf) app_devex -> wbhout = 0;
    app_devex -> tt_status = OZ_SUCCESS;
  }

  return (sts);
}

/************************************************************************/
/*									*/
/*  This routine is called by the port driver when a write completes	*/
/*									*/
/*    Input:								*/
/*									*/
/*	app_devexv  = application device pointer			*/
/*	write_param = as passed to port_disp_start			*/
/*	              (number of bytes that were written)		*/
/*	status  = write status						*/
/*	smplock = kb lock set						*/
/*									*/
/************************************************************************/

static void console_displayed (void *app_devexv, void *write_param, uLong status)

{
  Adevex *app_devex;
  uLong sts;

  app_devex = app_devexv;

  /* Increment wbhout by size indicating those chars have been processed and that part */
  /* of wbhbuf can be used again.  If there's stuff still in the buffer, start it.     */

  app_devex -> wbhout += (OZ_Pointer)write_param;
  if (app_devex -> wbhout == sizeof app_devex -> wbhbuf) app_devex -> wbhout = 0;
  app_devex -> tt_status = status;
  sts = disp_start_flush (app_devex);
  if ((sts != OZ_STARTED) && (app_devex -> tt_status == OZ_SUCCESS)) app_devex -> tt_status = sts;

  /* Maybe there is something to do that will fill wbhbuf */

  lowipl_hi (app_devex);			/* resume any write request or any read prompting that needs doing */
  console_kbd_char (app_devex, 0);		/* ... or maybe there's some keyboard echoing that needs doing */
}

/************************************************************************/
/*									*/
/*  This routine is called by the port driver when it has finished 	*/
/*  getting or setting the modes.  It is called with the db locked.	*/
/*									*/
/************************************************************************/

static void console_gotsetmode (void *app_devexv, void *app_iopexv, uLong status)

{
  if (((Aiopex *)app_iopexv) -> quotanpp != NULL) oz_crash ("oz_dev_conclass console_gotsetmode: app_quotanpp not null");
  oz_knl_iodonehi (((Aiopex *)app_iopexv) -> ioop, status, NULL, fin_gotsetmode, app_iopexv);
}

/* This routine is called at softint level back in the originator's address space */
/* It copies what was returned in the app_iopex -> modebuff to the user's buffer  */

static void fin_gotsetmode (void *app_iopexv, int finok, uLong *status_r)

{
  Adevex *app_devex;
  Aiopex *app_iopex;

  app_iopex = app_iopexv;
  app_devex = app_iopex -> app_chnex -> app_devex;

  /* Intercept what the hardware does and save it */

  if (*status_r == OZ_SUCCESS) {
    if (app_iopex -> u.gsmode.modebuff.columns  != 0) app_devex -> screenwidth  = app_iopex -> u.gsmode.modebuff.columns;
    if (app_iopex -> u.gsmode.modebuff.rows     != 0) app_devex -> screenlength = app_iopex -> u.gsmode.modebuff.rows;
    if (app_iopex -> u.gsmode.modebuff.linewrap != 0) app_devex -> hw_wrapping  = (app_iopex -> u.gsmode.modebuff.linewrap > 0);
  }

  /* Copy result back to requestor's buffer */

  if (finok) movc4 (sizeof app_iopex -> u.gsmode.modebuff, &(app_iopex -> u.gsmode.modebuff), 
                    app_iopex -> u.gsmode.size, app_iopex -> u.gsmode.buff);
}

/************************************************************************/
/*									*/
/*  This routine is called by the port driver when it wants to tell us 	*/
/*  the modes the terminal hardware is in				*/
/*									*/
/************************************************************************/

static uLong console_setmode (void *app_devexv, OZ_IO_conpseudo_setmode *conpseudo_setmode)

{
  Adevex *app_devex;
  OZ_Console_modebuff modebuff;

  app_devex = app_devexv;

  movc4 (conpseudo_setmode -> size, conpseudo_setmode -> buff, sizeof modebuff, &modebuff);
  if (modebuff.columns  != 0) app_devex -> screenwidth  = modebuff.columns;
  if (modebuff.rows     != 0) app_devex -> screenlength = modebuff.rows;
  if (modebuff.linewrap != 0) app_devex -> hw_wrapping  = (modebuff.linewrap > 0);

  modebuff.columns  = app_devex -> screenwidth;
  modebuff.rows     = app_devex -> screenlength;
  modebuff.linewrap = app_devex -> hw_wrapping ? 1 : -1;
  movc4 (sizeof modebuff, &modebuff, conpseudo_setmode -> size, conpseudo_setmode -> buff);

  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Routines to lock and unlock the database				*/
/*									*/
/************************************************************************/

static uLong lockdb (Adevex *app_devex, void *quotaobj)

{
  uLong kb;

  kb = (*(app_devex -> lockdb)) (app_devex -> lkprm);
  app_devex -> quotaobj = quotaobj;
  return (kb);
}

static void unlkdb (Adevex *app_devex, uLong kb)

{
  app_devex -> quotaobj = NULL;
  (*(app_devex -> unlkdb)) (app_devex -> lkprm, kb);
}
