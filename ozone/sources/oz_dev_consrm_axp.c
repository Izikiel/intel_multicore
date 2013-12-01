//+++2003-12-12
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
//---2003-12-12

/************************************************************************/
/*									*/
/*  This driver uses the SRM console routines to access the system 	*/
/*  console.  It 'should' work with any type of Alpha Console.		*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_dev_isa.h"
#include "oz_dev_timer.h"
#include "oz_io_comport.h"
#include "oz_io_console.h"
#include "oz_io_fs.h"
#include "oz_knl_debug.h"
#include "oz_knl_devio.h"
#include "oz_knl_hw.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_logon.h"
#include "oz_knl_lowipl.h"
#include "oz_knl_misc.h"
#include "oz_knl_procmode.h"
#include "oz_knl_sdata.h"
#include "oz_knl_status.h"

#define DELAY 0 // fraction of a second to delay after <CR>
                // eg, 16 for 1/16th second
                // zero for no delay

static uLong conport_assign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode);
static int conport_deassign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv);
static void conport_abort (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Ioop *ioop, void *iopexv, OZ_Procmode procmode);
static uLong conport_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnex, OZ_Procmode procmode, 
                                OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap);

static OZ_Devfunc conport_functable = { 0, 0, 0, 0, NULL, NULL, NULL, conport_assign, 
                                        conport_deassign, conport_abort, conport_start, NULL };

static OZ_Devunit *devunit;			/* console devunit */
static OZ_Iochan *conclass_iochan;		/* the class driver I/O channel */
static OZ_IO_comport_setup comport_setup;	/* setup parameters */
static int suspwriteflag;			/* set to suspend writing */
static uLong writesize;				/* number of bytes remaining to be written */
static char const *writebuff;			/* points to next character to be written */
static void *writeparam;			/* class driver's write parameter */
static int sendbeep;				/* send a beep (bell) iff set */
               
static char const beepbuff = 7;
static int initialized = 0;
static int timeripl;
static OZ_Devclass *devclass;
static OZ_Devdriver *devdriver;
static OZ_Lowipl *login_lowipl;
static OZ_Smplock smplock_kb;
static uQuad lastcrlf, lastoutput;
static void (*oldtimerent) (void *oldtimerprm, OZ_Mchargs *mchargs);
static void *oldtimerprm;

static void conport_read_start (void *dummy, int start);
static uLong conport_disp_start (void *dummy, void *write_param, uLong size, char *buff);
static void conport_disp_suspend (void *dummy, int suspend);
static void conport_kbd_rah_full (void *dummy, int full);
static void conport_terminate (void *dummy);
static uLong conport_getsetmode (void *dummy, void *getset_param, uLong size, OZ_Console_modebuff *buff);

static uLong checktimeripl (void *dummy);
static void timerinterrupt (void *dummy, OZ_Mchargs *mchargs);
static void login_entry (void *dummy, OZ_Lowipl *lowipl);
static void startwrite (void);
static char readchar (void);
static uLong writestring (uLong size, char const *buff);

/************************************************************************/
/*									*/
/*  Boot-time initialization routine					*/
/*									*/
/************************************************************************/

void oz_dev_consrm_init ()

{
  uLong sts;

  if (!initialized) {
    oz_knl_printk ("oz_dev_consrm_init\n");

    /* Assign I/O channel to console class driver setup device - if not there, try later */

    sts = oz_knl_iochan_crbynm (OZ_IO_CONSOLE_SETUPDEV, OZ_LOCKMODE_CW, OZ_PROCMODE_KNL, NULL, &conclass_iochan);
    if (sts != OZ_SUCCESS) {
      oz_knl_printk ("oz_dev_consrm_init: error %u assigning channel to console class device '" OZ_IO_CONSOLE_SETUPDEV "'\n", sts);
      return;
    }

    /* Set up timer interrupt to poll because we don't know what IRQ the console operates on */

    timeripl = 0;
    oz_hwaxp_scb_setc (0x600, timerinterrupt, NULL, &oldtimerent, &oldtimerprm);

    /* Wait for a timer interrupt to get the IPL it operates at and set up smplock */

    oz_hw_stl_microwait ((3 * 64 * oz_hwaxp_hwrpb -> intclkfrq / 1000) * 64, checktimeripl, NULL);	// wait up to 3 ticks
    if (timeripl == 0) oz_crash ("oz_dev_consrm_axp: couldn't get timer's IPL");			// won't get far without console
    oz_hw_smplock_init (sizeof smplock_kb, &smplock_kb, OZ_SMPLOCK_LEVEL_IPLS + timeripl);		// set up smplock at that IPL

    /* Create the devunit */

    devclass  = oz_knl_devclass_create (OZ_IO_COMPORT_CLASSNAME, OZ_IO_COMPORT_BASE, OZ_IO_COMPORT_MASK, "oz_dev_consrm");
    devdriver = oz_knl_devdriver_create (devclass, "oz_dev_consrm");
    devunit   = oz_knl_devunit_create (devdriver, "console", "alpha srm console", &conport_functable, 0, oz_s_secattr_sysdev);

    /* Send port parameters to class driver, get class driver parameters back */

    memset (&comport_setup, 0, sizeof comport_setup);
    comport_setup.port_devunit      = devunit;
    comport_setup.port_read_start   = conport_read_start;
    comport_setup.port_disp_start   = conport_disp_start;
    comport_setup.port_disp_suspend = conport_disp_suspend;
    comport_setup.port_kbd_rah_full = conport_kbd_rah_full;
    comport_setup.port_terminate    = conport_terminate;
    comport_setup.port_getsetmode   = conport_getsetmode;
    comport_setup.port_lkprm        = &smplock_kb;
    comport_setup.port_lockdb       = (void *)oz_hw_smplock_wait;
    comport_setup.port_unlkdb       = (void *)oz_hw_smplock_clr;

    sts = oz_knl_io (conclass_iochan, OZ_IO_COMPORT_SETUP, sizeof comport_setup, &comport_setup);
    if (sts != OZ_SUCCESS) oz_crash ("oz_dev_consrm_init: error %u setting up console device\n", sts);

    conport_functable.chn_exsize = comport_setup.class_functab -> chn_exsize;
    conport_functable.iop_exsize = comport_setup.class_functab -> iop_exsize;

    login_lowipl = oz_knl_lowipl_alloc ();
    initialized  = 1;
  }
}

static uLong checktimeripl (void *dummy)

{
  return (timeripl);
}

/************************************************************************/
/*									*/
/*  We just pass all the functab calls directly to the class driver	*/
/*									*/
/*  Class drivers do not have clonecre/clonedel.  They use 		*/
/*  OZ_IO_COMPORT_SETUP in place of clonecre, and deassigning that 	*/
/*  channel takes the place of clonedel.				*/
/*									*/
/************************************************************************/

/* A channel is being assigned to the device */

static uLong conport_assign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode)

{
  return ((*(comport_setup.class_functab -> assign)) (devunit, comport_setup.class_devex, iochan, chnexv, procmode));
}

/* A channel is being deassigned from the device */

static int conport_deassign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv)

{
  return ((*(comport_setup.class_functab -> deassign)) (devunit, comport_setup.class_devex, iochan, chnexv));
}

/* Abort an I/O function */

static void conport_abort (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Ioop *ioop, void *iopexv, OZ_Procmode procmode)

{
  (*(comport_setup.class_functab -> abort)) (devunit, comport_setup.class_devex, iochan, chnexv, ioop, iopexv, procmode);
}

/* Start an I/O function */

static uLong conport_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode, 
                            OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap)

{
  return ((*(comport_setup.class_functab -> start)) (devunit, comport_setup.class_devex, iochan, chnexv, procmode, 
                                                     ioop, iopexv, funcode, as, ap));
}

/************************************************************************/
/*									*/
/*  This routine is called by the class driver when it is starting or 	*/
/*  finishing a read request						*/
/*									*/
/************************************************************************/

static void conport_read_start (void *dummy, int start)

{ }

/************************************************************************/
/*									*/
/*  This routine is called by the class driver when it wants to 	*/
/*  display something.							*/
/*									*/
/*    Input:								*/
/*									*/
/*	size = size of message to display				*/
/*	buff = address of message to display				*/
/*									*/
/*    Output:								*/
/*									*/
/*	conport_disp_start = OZ_SUCCESS : completed synchronously	*/
/*	                     OZ_STARTED : will or did complete asyncly	*/
/*	                   OZ_QUEUEFULL : can't accept new request	*/
/*									*/
/************************************************************************/

static uLong conport_disp_start (void *dummy, void *write_param, uLong size, char *buff)

{
  if (suspwriteflag) return (OZ_QUEUEFULL);		/* if we're suspended, don't accept anything */
  if (writesize != 0) return (OZ_QUEUEFULL);		/* if we're already busy, don't accept anything */
  if (size == 0) return (OZ_SUCCESS);			/* null buffer = instant success */
  writesize  = size;					/* get the size of stuff to write */
  writebuff  = buff;					/* get pointer to stuff to write */
  writeparam = write_param;
  startwrite ();					/* start writing it */
  return (OZ_STARTED);					/* we will call back when write is complete */
}

/************************************************************************/
/*									*/
/*  The class driver calls this routine when it wants us to stop 	*/
/*  displaying whatever it has told us to display, or when it wants us 	*/
/*  to resume.								*/
/*									*/
/************************************************************************/

static void conport_disp_suspend (void *dummy, int suspend)

{
  suspwriteflag = suspend;				/* set new value for the flag */
  if (!suspend && (writesize != 0)) startwrite ();	/* maybe restart writing */
}

/************************************************************************/
/*									*/
/*  The class driver calls this routine when its read-ahead buffer is 	*/
/*  full.								*/
/*									*/
/*    Input:								*/
/*									*/
/*	full = 0 : read-ahead buffer is no longer full			*/
/*	       1 : read-ahead buffer is full				*/
/*									*/
/*  Does a beep if full=1.						*/
/*									*/
/************************************************************************/

static void conport_kbd_rah_full (void *dummy, int full)

{
  if (full) {
    sendbeep = 1;
    startwrite ();
  }
}

/************************************************************************/
/*									*/
/*  The class driver calls this when all channels have been deassigned 	*/
/*  from the device.  We don't try to clean up, just leave stuff as is.	*/
/*									*/
/************************************************************************/

static void conport_terminate (void *dummy)

{ }

/************************************************************************/
/*									*/
/*  Get / Set modes							*/
/*									*/
/************************************************************************/

static uLong conport_getsetmode (void *dummy, void *getset_param, uLong size, OZ_Console_modebuff *buff)

{
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  This routine processes timer interrupts				*/
/*									*/
/************************************************************************/

static void timerinterrupt (void *dummy, OZ_Mchargs *mchargs)

{
  char bl, tmp[2];
  uLong kb;

  if (initialized) {

    /* We have to do spinlock manually because this is a direct SCB handler */
    /* This smplock_wait routine assumes we're already at correct IPL       */

    kb = oz_hwaxp_smplock_wait_atipl (&smplock_kb);

    /* Process any keyboard data waiting for us */

    while ((bl = readchar ()) != 0) {

      /* Send 'normal' stuff to the class driver */

      if (bl != ']' - '@') (*(comport_setup.class_kbd_char)) (comport_setup.class_param, bl);

      /* Control-], special prompt */

      else {
        oz_knl_printk ("\r\noz_dev_consrm: C=debugger, D=diag, H=halt, L=login, R=resume\r\n");
ctrlbr_prompt:
        if (!oz_hw_getcon (sizeof tmp, tmp, 16, "oz_dev_consrm:> ")) goto ctrlbr_prompt;
        switch (tmp[0]) {
          case 'C': {
            int hwi;

            hwi = oz_hw_cpu_sethwints (0);			/* inhibit hardware interrupt delivery during debug */
            oz_knl_printk ("oz_dev_consrm_axp: calling debugger\r\n");
            oz_knl_debug_exception (NULL, mchargs);		/* call the debugger */
            oz_hw_cpu_sethwints (hwi);
            break;
          }
          case 'D': { 
            oz_knl_printk ("oz_dev_consrm_axp: entering diag mode\r\n");
            oz_hw_diag ();					/* call the diagnostic routine */
            break;
          }
          case 'H': {
            oz_knl_printk ("oz_dev_consrm_axp: halting, inc PC by 4 to continue\r\n");
            oz_knl_printk ("oz_dev_consrm_axp: mchargs %p -> pc %QX, ps %LX\r\n", mchargs, mchargs -> pc, (uLong)(mchargs -> ps));
            OZ_HWAXP_HALT ();
            break;
          }
          case 'L': {
            OZ_Lowipl *lowipl;

            lowipl = login_lowipl;
            if (lowipl == NULL) {
              oz_knl_printk ("oz_dev_consrm_axp: system hung, try diag mode\r\n");
              goto ctrlbr_prompt;
            }
            oz_knl_printk ("oz_dev_consrm_axp: logging on\r\n");
            login_lowipl = NULL;
            oz_knl_lowipl_call (lowipl, login_entry, NULL);
            break;
          }
          case 'R': {
            oz_knl_printk ("oz_dev_consrm_axp: resuming\r\n");
            break;
          }
          default: goto ctrlbr_prompt;
        }
      }
    }

    /* Maybe there's stuff to display */

    startwrite ();

    /* Release spinlock, remain at IPL */

    oz_hwaxp_smplock_clr_atipl (&smplock_kb, kb);
  }

  /* Maybe init routine is waiting for us to tell it the timer's IPL */

  else if (timeripl == 0) timeripl = OZ_HWAXP_MFPR_IPL ();

  /* Always call the old timer interrupt routine */

  (*oldtimerent) (oldtimerprm, mchargs);
}

/************************************************************************/
/*									*/
/*  Lowipl routine to start the login process on the console.  The 	*/
/*  normal <CR> won't work because the console already has a channel 	*/
/*  (oz_s_coniochan) assigned to it.					*/
/*									*/
/************************************************************************/

static void login_entry (void *dummy, OZ_Lowipl *lowipl)

{
  oz_knl_logon_iochan (oz_s_coniochan);		/* start the logon image */
  login_lowipl = lowipl;			/* re-arm control-shift-L */
}

/************************************************************************/
/*									*/
/*  Start writing data indicated by writesize/buff.			*/
/*  Call completion routine when done.					*/
/*									*/
/************************************************************************/

static void startwrite (void)

{
  uLong done;

  while (sendbeep || ((writesize > 0) && !suspwriteflag)) {
    if (sendbeep) {						// see if we need to send a beep
      done = writestring (1, &beepbuff);			// if so, send it
      if (done == 0) break;					// if can't, try again later
      sendbeep = 0;						// ok, don't send another
    } else {
      done = writestring (writesize, writebuff);		// no beep, try to write something
      if (done == 0) break;					// if can't, try again later
      writesize -= done;					// that much less to do
      writebuff += done;
      if (writesize == 0) {					// maybe tell class driver we're all done
        (*(comport_setup.class_displayed)) (comport_setup.class_param, writeparam, OZ_SUCCESS);
      }
    }
  }
}

/************************************************************************/
/*									*/
/*  This routine reads a line from the keyboard with interrupts 	*/
/*  disabled (used for kernel debugging)				*/
/*									*/
/*    Input:								*/
/*									*/
/*	size = max length to read from keyboard				*/
/*	buff = buffer to read them into					*/
/*	pmtsize = prompt string size					*/
/*	pmtbuff = prompt string buffer					*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_hw_getcon = 0 : end-of-file terminator			*/
/*	               1 : normal terminator				*/
/*	*buff = filled with null-terminated string			*/
/*									*/
/*    Note:								*/
/*									*/
/*	The current screen is always used, no matter which one it 	*/
/*	happens to be							*/
/*									*/
/************************************************************************/

int oz_hw_getcon (uLong size, char *buff, uLong pmtsize, char const *pmtbuff)

{
  char c;
  uLong kb, offs;

  oz_hw_putcon (pmtsize, pmtbuff);					/* output prompt string */
  offs = 0;								/* start at beginning of buffer */
  while (((c = readchar ()) != 4) && (c != 13) && (c != 26)) {		/* get a char, stop if terminator (^d, CR, ^z) */
    if (((c == 8) || (c == 127)) && (offs > 0)) {			/* check for backspace */
      oz_hw_putcon (3, "\010 \010");					/* if so, wipe it from screen */
      -- offs;								/* ... and from buffer */
    }
    if ((c < 127) && (c >= ' ') && (offs < size - 1)) {			/* check for printable character */
      oz_hw_putcon (1, &c);						/* if so, echo to screen (with line wrap) */
      buff[offs++] = c;							/* ... and store in buffer */
    }
  }
  oz_hw_putcon (2, "\r\n");						/* terminated, echo newline char */
  buff[offs] = 0;							/* put null terminator in buffer */
  return (c == 13);							/* return if normal terminator or not */
}

/************************************************************************/
/*									*/
/*  Put a string on the screen						*/
/*  Loop until all is output						*/
/*  Edit in CR before LF						*/
/*									*/
/*    Input:								*/
/*									*/
/*	size = number of characters to output				*/
/*	buff = characters to output					*/
/*									*/
/*    Output:								*/
/*									*/
/*	string output to screen						*/
/*									*/
/************************************************************************/

void oz_hw_putcon (uLong size, char const *buff)

{
  char const *anlf;
  uLong done;
  uQuad saveipl;

  saveipl = OZ_HWAXP_MFPR_IPL ();
  if (saveipl < timeripl) OZ_HWAXP_MTPR_IPL (timeripl);						// keep timer from mixing output
  while (size > 0) {										// repeat while there's stuff to do
    for (anlf = buff; (anlf = memchr (anlf, '\n', buff + size - anlf)) != NULL; anlf ++) {	// find an LF in there somewhere
      if ((anlf == buff) || (anlf[-1] != '\r')) break;						// that doesn't have a CR before it
    }
    if (anlf == buff) {										// see if LF at beginning
      for (anlf = "\r\n"; *anlf != 0; anlf += done) done = writestring (strlen (anlf), anlf);	// if so, output CRLF
      -- size;											// skip over it
      buff ++;
    } else {
      if (anlf == NULL) anlf = buff + size;							// if not, output up to LF or end
      while (buff < anlf) {
        done  = writestring (anlf - buff, buff);
        size -= done;
        buff += done;
      }
    }
  }
  if (saveipl < timeripl) OZ_HWAXP_MTPR_IPL (saveipl);
}

/************************************************************************/
/*									*/
/*  Get keyboard char from console					*/
/*									*/
/*    Output:								*/
/*									*/
/*	readchar = 0 : no character available				*/
/*	        else : ascii key code					*/
/*									*/
/************************************************************************/

static char readchar (void)

{
  char bl;

  bl = 0;
  {
    register uQuad __r0  asm ("$0");
    register uQuad __r16 asm ("$16") = OZ_HWAXP_DISPATCH_GETC;
    register uQuad __r17 asm ("$17") = 0;
    register uQuad __r27 asm ("$27") = oz_hwaxp_dispatchr27;

    asm volatile ("jsr $26,(%1)" : "=r"(__r0) 
                                 : "r"(oz_hwaxp_dispatchent), "r"(__r16), "r"(__r17), "r"(__r27) 
                                 : "$1", "$16", "$17", "$18", "$19", "$20", "$21", "$22", "$23", "$24", "$25", "$26", "$27", "$28");
    if ((__r0 >> 62) == 0) bl = __r0;
  }

  return (bl);
}

/************************************************************************/
/*									*/
/*  Output what we can without stalling					*/
/*									*/
/*    Input:								*/
/*									*/
/*	size = number of chars to output				*/
/*	buff = points to chars to output				*/
/*									*/
/*    Output:								*/
/*									*/
/*	writestring = number of chars actually output			*/
/*									*/
/************************************************************************/

static uLong writestring (uLong size, char const *buff)

{
  uLong ancrlf;
  uQuad savedipl;

  if (size == 0) return (0);

  /* Insert a delay after outputting a CR or an LF */

#if DELAY > 0
  if (lastcrlf != 0) {
    if (OZ_HWAXP_RSCC () < lastcrlf) return (0);
    lastcrlf = 0;
  }
#endif

  /* Stop before a CR or the stupid thing will mix up the following LF with the next char */

  for (ancrlf = 0; ancrlf < size; ancrlf ++) {
    if (buff[ancrlf] == 13) break;
  }

  /* But if that's what we got at beginning of buffer, output it after we're sure it's been a delay since last output */

  if (ancrlf == 0) {
#if DELAY > 0
    if (OZ_HWAXP_RSCC () < lastoutput + oz_hwaxp_hwrpb -> cycounfrq / DELAY) return (0);
#endif
    ancrlf = 1;
  }

  /* Output something from ancrlf/buff */

  savedipl = OZ_HWAXP_MTPR_IPL (31);					// lock out all ints, we don't know console IPL
  {
    register uQuad __r0  asm ("$0");
    register uQuad __r16 asm ("$16") = OZ_HWAXP_DISPATCH_PUTS;
    register uQuad __r17 asm ("$17") = 0;
    register uQuad __r18 asm ("$18") = (OZ_Pointer)buff;
    register uQuad __r19 asm ("$19") = ancrlf;
    register uQuad __r27 asm ("$27") = oz_hwaxp_dispatchr27;

    asm volatile ("jsr $26,(%1)" : "=r"(__r0) 
                                 : "r"(oz_hwaxp_dispatchent), "r"(__r16), "r"(__r17), "r"(__r18), "r"(__r19), "r"(__r27)
                                 : "$1", "$16", "$17", "$18", "$19", "$20", "$21", "$22", "$23", "$24", "$25", "$26", "$27", "$28");
    if ((__r0 >> 62) != 0) OZ_HWAXP_HALT ();				// if can't even print, just halt
    ancrlf = __r0;							// get how much we did
  }
  OZ_HWAXP_MTPR_IPL (savedipl);						// lower IPL so we don't hog the CPU too long

  /* If we output something, remember when it was.  If we output a CR, don't output anything else until after delay. */

#if DELAY > 0
  if (ancrlf > 0) {
    lastoutput =  OZ_HWAXP_RSCC ();
    if (buff[ancrlf-1] == 13) {
      lastcrlf = OZ_HWAXP_RSCC () + oz_hwaxp_hwrpb -> cycounfrq / DELAY;
    }
  }
#endif

  /* Return how much we actually output */

  return (ancrlf);
}

/************************************************************************/
/*									*/
/*  This routine is called by the debugger on the primary cpu when it 	*/
/*  is waiting for another cpu to execute.  It checks to see if 	*/
/*  control-shift-C has been pressed.					*/
/*									*/
/************************************************************************/

int oz_knl_console_debugchk (void)

{
  return (0);					/* no char present, return false */
}
