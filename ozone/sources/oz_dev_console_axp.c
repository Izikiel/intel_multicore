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
/*  Console port driver for Alphas					*/
/*									*/
/*  It handles the physical I/O to the keyboard and screen and passes 	*/
/*  the data to the high-level console class driver			*/
/*									*/
/*  Extra parameters:							*/
/*									*/
/*	console_blank=time to wait before blanking screen		*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_dev_console_486.h"
#include "oz_dev_isa.h"
#include "oz_dev_vgavideo_486.h"
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

#define KEYBOARD_IRQ 1	/* keyboards are hard-wired to IRQ 1 */
#define NUMSCREENS 10	/* maximum number of screens we support (control-shift-0 or -~ thru control-shift-9) */

#define KB_CP 0x64	/* keyboard command port */
#define KB_DP 0x60	/* keyboard data port */

typedef struct { OZ_Devunit *devunit;			/* console_#n devunit */
                 OZ_Vctx *vctx;				/* corresponding video context */
                 OZ_Iochan *conclass_iochan;		/* the class driver I/O channel */
                 OZ_IO_comport_setup comport_setup;	/* setup parameters */
                 int suspwriteflag;			/* set to suspend writing */
               } Devex;

typedef struct { uByte class_area[1];
               } Chnex;

typedef struct { uByte class_area[1];
               } Iopex;

static uLong conport_assign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode);
static int conport_deassign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv);
static void conport_abort (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Ioop *ioop, void *iopexv, OZ_Procmode procmode);
static uLong conport_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnex, OZ_Procmode procmode, 
                                OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap);

static OZ_Devfunc conport_functable = { sizeof (Devex), sizeof (Chnex), sizeof (Iopex), 0, NULL, NULL, NULL, conport_assign, 
                                        conport_deassign, conport_abort, conport_start, NULL };

extern int oz_dev_video_pagemode;
extern uLong oz_dev_video_currow;

static OZ_Devclass *devclass;
static OZ_Devdriver *devdriver;
static Devex *devexs[NUMSCREENS];
static Devex *cdevex = NULL;

static int initialized = 0;
static OZ_Lowipl *login_lowipl = NULL;

static OZ_Dev_Isa_Irq *kb_irq;
#define smplock_kb oz_dev_keyboard_smplock
OZ_Smplock *oz_dev_keyboard_smplock = NULL;

static int videoblanked = 0;		/* 0 : screen is on; 1 : screen is off */
static OZ_Datebin blankduration;	/* delta time to leave screen on after a keystroke */
static OZ_Datebin lastkeystroke;	/* time of last keystroke */

static OZ_Datebin beepduration;		/* duration of the beep */
static OZ_Datebin beepoffwhen;		/* when to turn the beep off */
static OZ_Timer *beeptimer = NULL;	/* NULL: not initted or beep is turned on, so it is not ok to queue timer to turn it off */
					/* else: initialized and beep is turned off, so it is ok to turn it on and queue timer to turn it off */

static void conport_read_start (void *devexv, int start);
static uLong conport_disp_start (void *devexv, void *write_param, uLong size, char *buff);
static void conport_disp_suspend (void *devexv, int suspend);
static void conport_kbd_rah_full (void *devexv, int full);
static void conport_terminate (void *devexv);
static uLong conport_getsetmode (void *devexv, void *getset_param, uLong size, OZ_Console_modebuff *buff);
static void blankscreen (void *dummy, OZ_Timer *timer);
static void startbeep (void);
static void turnbeepoff (void *dummy, OZ_Timer *timer);
static void keyboard_interrupt (void *dummy, OZ_Mchargs *mchargs);
static void login_entry (void *dummy, OZ_Lowipl *lowipl);
static Devex *initdev (int vidx);

/************************************************************************/
/*									*/
/*  Boot-time initialization routine					*/
/*									*/
/************************************************************************/

void oz_dev_console_init ()

{
  const char *extra;
  Devex *devex;

  if (!initialized) {
    oz_knl_printk ("oz_dev_console_init\n");

    memset (devexs, 0, sizeof devexs);				/* aint got no devices yet */

    devclass   = oz_knl_devclass_create (OZ_IO_COMPORT_CLASSNAME, OZ_IO_COMPORT_BASE, OZ_IO_COMPORT_MASK, "oz_dev_console");
    devdriver  = oz_knl_devdriver_create (devclass, "oz_dev_console");
    kb_irq     = oz_dev_isa_irq_alloc (KEYBOARD_IRQ, keyboard_interrupt, NULL); /* set up interrupt routine */
    smplock_kb = oz_dev_isa_irq_smplock (kb_irq);
    oz_dev_keyboard_getc (1);					/* doesn't seem to interrupt unless this is here */
    oz_dev_video_pagemode = 1;					/* it's ok for pagemode to read keyboard now */

    devex = initdev (0);					/* init main console only (others on demand) */
    if (devex == NULL) return;					/* return if can't do it yet */
    conport_functable.chn_exsize += devex -> comport_setup.class_functab -> chn_exsize;
    conport_functable.iop_exsize += devex -> comport_setup.class_functab -> iop_exsize;
    login_lowipl = oz_knl_lowipl_alloc ();			/* set up a control-shift-L lowipl struct */
    cdevex = devex;						/* current screen = this one-and-only screen */
    extra = oz_knl_misc_getextra ("console_blank", "");
    if (extra[0] != 0) {
      oz_sys_datebin_encstr (4, extra, &blankduration);		/* inactivity time for blanking screen */
      lastkeystroke = oz_hw_tod_getnow ();			/* pretend key just pressed */
      oz_knl_timer_insert (oz_knl_timer_alloc (), lastkeystroke + blankduration, blankscreen, NULL);
    }
    oz_sys_datebin_encstr (3, "0.1", &beepduration);		/* set up duration of the beep */
    beeptimer   = oz_knl_timer_alloc ();			/* allocate a timer struct for turning beep off */
    initialized = 1;						/* we are now initialized */
  }
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
  Devex *devex;

  devex = devexv;
  return ((*(devex -> comport_setup.class_functab -> assign)) (devunit, devex -> comport_setup.class_devex, iochan, ((Chnex *)chnexv) -> class_area, procmode));
}

/* A channel is being deassigned from the device */

static int conport_deassign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv)

{
  Devex *devex;

  devex = devexv;
  return ((*(devex -> comport_setup.class_functab -> deassign)) (devunit, devex -> comport_setup.class_devex, iochan, ((Chnex *)chnexv) -> class_area));
}

/* Abort an I/O function */

static void conport_abort (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Ioop *ioop, void *iopexv, OZ_Procmode procmode)

{
  Devex *devex;

  devex = devexv;
  (*(devex -> comport_setup.class_functab -> abort)) (devunit, devex -> comport_setup.class_devex, iochan, ((Chnex *)chnexv) -> class_area, ioop, ((Iopex *)iopexv) -> class_area, procmode);
}

/* Start an I/O function */

static uLong conport_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode, 
                                OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap)

{
  Devex *devex;
  uLong sts;

  devex = devexv;

  switch (funcode) {

    /* Get GETSCREEN function gets processed directly */

    case OZ_IO_CONSOLE_GETSCREEN: {
      OZ_Console_screenbuff console_screenbuff;
      OZ_IO_console_getscreen console_getscreen;

      movc4 (as, ap, sizeof console_getscreen, &console_getscreen);
      sts = oz_knl_ioop_lockw (ioop, console_getscreen.size, console_getscreen.buff, NULL, NULL, NULL);
      if (sts == OZ_SUCCESS) {
        movc4 (console_getscreen.size, console_getscreen.buff, sizeof console_screenbuff, &console_screenbuff);
        if (console_screenbuff.buff != NULL) {
          sts = oz_knl_ioop_lockw (ioop, console_screenbuff.size, console_screenbuff.buff, NULL, NULL, NULL);
        }
        if (sts == OZ_SUCCESS) {
          oz_dev_video_getscreen (devex -> vctx, sizeof console_screenbuff, &console_screenbuff);
          movc4 (sizeof console_screenbuff, &console_screenbuff, console_getscreen.size, console_getscreen.buff);
        }
      }
      break;
    }

    /* All others go to the class driver */

    default: {
      sts = (*(devex -> comport_setup.class_functab -> start)) (devunit, devex -> comport_setup.class_devex, iochan, 
                                                                ((Chnex *)chnexv) -> class_area, procmode, ioop, 
                                                                ((Iopex *)iopexv) -> class_area, funcode, as, ap);
    }
  }

  return (sts);
}

/************************************************************************/
/*									*/
/*  This routine is called by the class driver when it is starting or 	*/
/*  finishing a read request						*/
/*									*/
/************************************************************************/

static void conport_read_start (void *devexv, int start)

{
  if (start) oz_dev_video_currow = 0;	/* reset any video screen line fill count */
}

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
/*	                   OZ_QUEUEFULL : can't accept new request	*/
/*									*/
/************************************************************************/

static uLong conport_disp_start (void *devexv, void *write_param, uLong size, char *buff)

{
  Devex *devex;

  devex = devexv;
  if (devex -> suspwriteflag) return (OZ_QUEUEFULL);		/* if we're suspended, don't accept anything */
  if (memchr (buff, 7, size) != NULL) startbeep ();		/* ok, maybe start beeping */
  oz_dev_video_putstring (devex -> vctx, size, buff);		/* ... and output it */
  return (OZ_SUCCESS);						/* it completed synchronously */
}

/************************************************************************/
/*									*/
/*  The class driver calls this routine when it wants us to stop 	*/
/*  displaying whatever it has told us to display, or when it wants us 	*/
/*  to resume.								*/
/*									*/
/************************************************************************/

static void conport_disp_suspend (void *devexv, int suspend)

{
  ((Devex *)devexv) -> suspwriteflag = suspend;	/* set new value for the flag */
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

static void conport_kbd_rah_full (void *devexv, int full)

{
  startbeep ();
}

/************************************************************************/
/*									*/
/*  The class driver calls this when all channels have been deassigned 	*/
/*  from the device.  We don't try to clean up, just leave stuff as is.	*/
/*									*/
/************************************************************************/

static void conport_terminate (void *devexv)

{}

/************************************************************************/
/*									*/
/*  Get / Set modes							*/
/*									*/
/************************************************************************/

static uLong conport_getsetmode (void *devexv, void *getset_param, uLong size, OZ_Console_modebuff *buff)

{
  oz_dev_video_setmode (((Devex *)devexv) -> vctx, size, buff);
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  This keyboard timer routine is called after 15mins of inactivity 	*/
/*  to blank the screen							*/
/*									*/
/************************************************************************/

static void blankscreen (void *dummy, OZ_Timer *timer)

{
  OZ_Datebin now;
  uLong kb;

  if (!oz_s_inloader) {							/* since keyboard can't wake us up in the loader */
    now = oz_hw_tod_getnow ();						/* see what time it is now */
    if (now >= lastkeystroke + blankduration) {				/* see if it is time to blank the screen */
      kb = oz_hw_smplock_wait (smplock_kb);
      if (!videoblanked) {						/* ok, blank it */
        oz_dev_vgavideo_blank (1);
        videoblanked = 1;
      }
      lastkeystroke = now;						/* pretend just got a keystroke now so we'll come back in 15 mins */
      oz_hw_smplock_clr (smplock_kb, kb);
    }
    oz_knl_timer_insert (timer, lastkeystroke + blankduration, blankscreen, NULL); /* come back 15mins after last keystroke */
  }
}

/************************************************************************/
/*									*/
/*  Start beep going for the beepduration				*/
/*  Called with the kb smplock set					*/
/*									*/
/************************************************************************/

static void startbeep (void)

{
  beepoffwhen = oz_hw_tod_getnow ();			/* - see when it should be turned off */
  OZ_HW_DATEBIN_ADD (beepoffwhen, beepoffwhen, beepduration);
  if (beeptimer != NULL) {				/* - see if the beep is already turned on */
    oz_dev_isa_outb (oz_dev_isa_inb (0x61) | 0x03, 0x61);		/* - if not, turn the beep on */
    oz_dev_isa_outb (0xb6, 0x43);
    oz_dev_isa_outb (0x36, 0x42);
    oz_dev_isa_outb (0x03, 0x42);
    oz_knl_timer_insert (beeptimer, beepoffwhen, turnbeepoff, NULL); /* say when to turn it off */
    beeptimer = NULL;					/* - remember the beep is currently on */
  }
}

/* Time to turn the beep off */

static void turnbeepoff (void *dummy, OZ_Timer *timer)

{
  OZ_Datebin now;
  uLong kb;

  now = oz_hw_tod_getnow ();				/* see what time it is now */
  kb  = oz_hw_smplock_wait (smplock_kb);		/* lock keyboard stuff (in case it is trying to extend beep time) */
  if (OZ_HW_DATEBIN_CMP (now, beepoffwhen) >= 0) {	/* make sure it is time to turn it off */
    oz_dev_isa_outb (oz_dev_isa_inb (0x61) & 0xFC, 0x61); /* ok, turn the damn thing off */
    beeptimer = timer;					/* make timer available for re-use */
  } else {
    oz_knl_timer_insert (timer, beepoffwhen, turnbeepoff, NULL); /* not time yet (got another 'bell' code while this one was going), reset for later time */
  }
  oz_hw_smplock_clr (smplock_kb, kb);			/* unlock keyboard stuff */
}

/************************************************************************/
/*									*/
/*  Keyboard scan code translation tables				*/
/*									*/
/************************************************************************/

static uLong keyboard_offs = 0;		// <7> = 0 : ctrl key released
					//       1 : ctrl key pressed
					// <8> = 0 : shift key released
					//       1 : shift key pressed

static uLong keyboard_f0 = 0;		// 0 : last keycode was not F0
					// 1 : last keycode was F0

#define RSH -1		/* right shift key */
#define LSH -1		/* left shift key (treat same as right) */
#define RCT -2		/* right ctrl key */
#define LCT -2		/* left ctrl key */
#define CSC -3		/* control-shift-C (call debugger) */
#define CSD -4		/* control-shift-D (enter diag mode) */
#define CSU -5		/* control-shift-U (scroll up a line) */
#define CSJ -6		/* control-shift-J (scroll down a line) */
#define KPS -7		/* keypad star (return PF3 multibyte) */
#define CSL -8		/* control-shift-L (login console) */
#define CSQ -9		/* control-shift-Q (enter/exit page mode) */
#define CS0 -10		/* control-shift-0 or -~ (select screen 0) */
#define CS1 -11		/* control-shift-1 (select screen 1) */
#define CS2 -12		/* control-shift-2 (select screen 2) */
#define CS3 -13		/* control-shift-3 (select screen 3) */
#define CS4 -14		/* control-shift-4 (select screen 4) */
#define CS5 -15		/* control-shift-5 (select screen 5) */
#define CS6 -16		/* control-shift-6 (select screen 6) */
#define CS7 -17		/* control-shift-7 (select screen 7) */
#define CS8 -18		/* control-shift-8 (select screen 8) */
#define CS9 -19		/* control-shift-9 (select screen 9) */
#define CSH -20		/* control-shift-H HALT */

static const Byte keyboard_table[512] = {

	/* shift up & ctrl up */

	  0,  0,  0,  0,  0,  0,  0,  0, 27,  0,  0,  0,  0,  9,'`',  0, 	// 0*
	  0,LCT,LSH,  0,  0,'q','1',  0,  0,  0,'z','s','a','w','2',  0, 	// 1*
	  0,'c','x','d','e','4','3',  0,  0,' ','v','f','t','r','5',  0, 	// 2*
	  0,'n','b','h','g','y','6',  0,  0,  0,'m','j','u','7','8',  0, 	// 3*
	  0,',','k','i','o','0','9',  0,  0,'.','/','l',';','p','-',  0, 	// 4*
	  0, 0,'\'',  0,'[','=',  0,  0,RCT,RSH, 13,']','\\', 0,  0,  0, 	// 5*
	  0,  0,  0,  0,  0,  0,127,  0,  0,  0,  0,  0,  0,  0,  0,  0, 	// 6*
	  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 	// 7*

	/* shift up & ctrl down */

	  0,  0,  0,  0,  0,  0,  0,  0, 27,  0,  0,  0,  0,  9,'`',  0, 	// 0*
	  0,LCT,LSH,  0,  0, 17,  0,  0,  0,  0, 26, 19,  1, 23,  0,  0, 	// 1*
	  0,  3, 24,  4,  5,  0,  0,  0,  0,' ', 22,  6, 20, 18,  0,  0, 	// 2*
	  0, 14,  2,  8,  7, 25,  0,  0,  0,  0, 13, 10, 21,  0,  0,  0, 	// 3*
	  0,',', 11,  9, 15,  0,  0,  0,  0,'.','/', 12,  0, 16,  0,  0, 	// 4*
	  0,  0,  0,  0, 27,'=',  0,  0,RCT,RSH, 13, 29, 28,  0,  0,  0, 	// 5*
	  0,  0,  0,  0,  0,  0,  8,  0,  0,  0,  0,  0,  0,  0,  0,  0, 	// 6*
	  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 	// 7*

	/* shift down & ctrl up */

	  0,  0,  0,  0,  0,  0,  0,  0, 27,  0,  0,  0,  0,  9,'~',  0, 	// 0*
	  0,LCT,LSH,  0,  0,'Q','!',  0,  0,  0,'Z','S','A','W','@',  0, 	// 1*
	  0,'C','X','D','E','$','#',  0,  0,' ','V','F','T','R','%',  0, 	// 2*
	  0,'N','B','H','G','Y','^',  0,  0,  0,'M','J','U','&','*',  0, 	// 3*
	  0,'<','K','I','O',')','(',  0,  0,'>','?','L',':','P','_',  0, 	// 4*
	  0,  0,'"',  0,'{','+',  0,  0,RCT,RSH, 13,'}','|',  0,  0,  0, 	// 5*
	  0,  0,  0,  0,  0,  0,127,  0,  0,  0,  0,  0,  0,  0,  0,  0, 	// 6*
	  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 	// 7*

	/* shift down & ctrl down */

	  0,  0,  0,  0,  0,  0,  0,  0, 27,  0,  0,  0,  0,  9,CS0,  0, 	// 0*
	  0,LCT,LSH,  0,  0,CSQ,CS1,  0,  0,  0,  0,  0,  0,  0,CS2,  0, 	// 1*
	  0,CSC,  0,CSD,  0,CS4,CS3,  0,  0,  0,  0,  0,  0,  0,CS5,  0, 	// 2*
	  0,  0,  0,CSH,  0,  0,CS6,  0,  0,  0,  0,CSJ,CSU,CS7,CS8,  0, 	// 3*
	  0,  0,  0,  0,  0,CS0,CS9,  0,  0,  0,  0,CSL,  0,  0,  0,  0, 	// 4*
	  0,  0,  0,  0,  0,  0,  0,  0,RCT,RSH, 13,  0,  0,  0,  0,  0, 	// 5*
	  0,  0,  0,  0,  0,  0,127,  0,  0,  0,  0,  0,  0,  0,  0,  0, 	// 6*
	  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0 	// 7*
	};

/************************************************************************/
/*									*/
/*  This routine processes keyboard interrupts				*/
/*									*/
/*  It gets called whenever there is a character to be read from the 	*/
/*  keyboard.  The keyboard smplock is already set.			*/
/*									*/
/*  It just passes the character onto the class driver for processing.	*/
/*									*/
/************************************************************************/

static void keyboard_interrupt (void *dummy, OZ_Mchargs *mchargs)

{
  char bl;

  while ((bl = oz_dev_keyboard_getc (0)) != 0) {

    /* Send 'normal' stuff to the class driver */

    if (bl > 0) {
      if (cdevex != NULL) {
        (*(cdevex -> comport_setup.class_kbd_char)) (cdevex -> comport_setup.class_param, bl);
        if (oz_dev_video_mode_local_echo) oz_dev_video_putchar (cdevex -> vctx, bl);
        if ((bl == 13) && oz_dev_video_mode_newline) {
          (*(cdevex -> comport_setup.class_kbd_char)) (cdevex -> comport_setup.class_param, 10);
          if (oz_dev_video_mode_local_echo) oz_dev_video_putchar (cdevex -> vctx, 10);
        }
      }
      continue;
    }

    /* Process special keys internally */

    switch (bl) {
      case CSC: {
        int hwi;

        keyboard_offs = 0;				/* assume they will release control and shift keys */
        hwi = oz_hw_cpu_sethwints (0);			/* inhibit hardware interrupt delivery during debug */
        oz_knl_printk ("oz_dev_console_axp: control-shift-C pressed - calling debugger\n");
        oz_knl_debug_exception (NULL, mchargs);		/* call the debugger */
        oz_hw_cpu_sethwints (hwi);
        break;
      }
      case CSD: { 
        keyboard_offs = 0;				/* assume they will release control and shift keys */
        oz_knl_printk ("oz_dev_console_axp: control-shift-D pressed - entering diag mode\n");
        oz_hw_diag ();					/* call the diagnostic routine */
        break;
      }
      case CSH: {
        keyboard_offs = 0;				/* assume they will release control and shift keys */
        oz_knl_printk ("oz_dev_console_axp: control-shift-H halt, inc PC by 4 to continue\n");
        oz_knl_printk ("oz_dev_console_axp: mchargs %p -> pc %QX, ps %LX\n", mchargs, mchargs -> pc, (uLong)(mchargs -> ps));
        OZ_HWAXP_HALT ();
        break;
      }
      case CSL: {
        OZ_Lowipl *lowipl;

        lowipl = login_lowipl;
        if (lowipl == NULL) {
          oz_knl_printk ("oz_dev_console_axp: control-shift-L pressed - system hung\n");
        } else {
          oz_knl_printk ("oz_dev_console_axp: control-shift-L pressed - logging on\n");
          login_lowipl = NULL;
          oz_knl_lowipl_call (lowipl, login_entry, NULL);
        }
        break;
      }
    }
  }
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
/*  Send a string to class driver as if it came from keyboard		*/
/*									*/
/************************************************************************/

void oz_dev_keyboard_send (void *devexv, int size, char *buff)

{
  Devex *devex;
  uLong kb;

  devex = devexv;
  if ((devex != NULL) && initialized) {
    kb = oz_hw_smplock_wait (smplock_kb);
    while (-- size >= 0) {
      (*(devex -> comport_setup.class_kbd_char)) (devex -> comport_setup.class_param, *(buff ++));
    }
    oz_hw_smplock_clr (smplock_kb, kb);
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

int oz_hw_getcon (uLong size, char *buff, uLong pmtsize, const char *pmtbuff)

{
  char c;
  uLong kb, offs;

  oz_hw_putcon (pmtsize, pmtbuff);					/* output prompt string */
  oz_dev_video_currow = 0;						/* reset any video screen line fill count */
  offs = 0;								/* start at beginning of buffer */
  while (((c = oz_dev_keyboard_getc (1)) != 4) && (c != 13) && (c != 26)) { /* get a char, stop if terminator (^d, CR, ^z) */
    if (((c == 8) || (c == 127)) && (offs > 0)) {			/* check for backspace */
      oz_hw_putcon (3, "\010 \010");					/* if so, wipe it from screen */
      -- offs;								/* ... and from buffer */
    }
    if ((c < 127) && (c >= ' ') && (offs < size - 1)) {			/* check for printable character */
      oz_hw_putcon (1, &c);						/* if so, echo to screen (with line wrap) */
      buff[offs++] = c;							/* ... and store in buffer */
    }
  }
  oz_hw_putcon (1, "\n");						/* terminated, echo newline char */
  buff[offs] = 0;							/* put null terminator in buffer */
  return (c == 13);							/* return if normal terminator or not */
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

/************************************************************************/
/*									*/
/*  Get keyboard char from interface chip				*/
/*									*/
/*    Input:								*/
/*									*/
/*	ignmb = 0 : process multibyte sequence in progress or possibly start new one
/*	        1 : flush any multibyte sequence in progress and don't start new one
/*									*/
/*    Output:								*/
/*									*/
/*	oz_dev_keyboard_getc = 0 : no character available		*/
/*	                     > 0 : ascii key code			*/
/*	                     < 0 : special key code			*/
/*									*/
/************************************************************************/

char oz_dev_keyboard_getc (int ignmb)

{
  char bl;
  int last_f0;
  uByte al;

  if (!initialized) goto use_dispatch;

  /* If someone is looking for stuff from keyboard, light up the screen */

  if (!oz_s_inloader) {				/* oz_hw_tod_getnow may not be set up yet */
    if (videoblanked) {				/* if video is blanked, turn it on */
      oz_dev_vgavideo_blank (0);
      videoblanked = 0;
    }
    lastkeystroke = oz_hw_tod_getnow ();	/* anyway, remember when last keyboard stroke was */
  }

  /* Get scan code from keyboard */

retry:
  al = oz_dev_isa_inb (KB_CP);			/* check the command port */
  if ((al & 0x21) != 0x01) return (0);		/* if no low bit, no keyboard char present */
						/* ... also make sure there is no mouse bit set */
  al = oz_dev_isa_inb (KB_DP);			/* ok, read the keyboard char */

  /* Translate scan code */

  if (al == 0xF0) {				/* this means either control or shift was released */
    keyboard_f0 = 1;				/* remember we got an F0 */
    goto retry;
  }
  last_f0 = keyboard_f0;			/* remember if last was an F0 */
  keyboard_f0 = 0;				/* and we know this one is not F0 */

  bl = keyboard_table[keyboard_offs+(al&0x7F)];	/* get code directly from table */
  if (bl == 0) goto retry;			/* ignore these codes */

  /* Process some special codes internally */

  if (bl < 0) switch (bl) {
    case LSH: {
      if (last_f0) keyboard_offs &= ~0x100;	/* shift key, flip bit 8 */
      else keyboard_offs |= 0x100;
      goto retry;
    }
    case LCT: {
      if (last_f0) keyboard_offs &= ~0x080;	/* control key, flip bit 7 */
      else keyboard_offs |= 0x080;		
      goto retry;
    }
    case CSQ: {
      oz_dev_video_pagemode = 1 - oz_dev_video_pagemode; /* control-shift-Q, toggle pagemode */
      break;					/* also return to caller in case they want to know if pagemode changed */
    }
  }

  /* Ascii or external special code, return to caller */

  return (bl);

  /* Not initialized for I/O yet, use console's dispatch routine */

use_dispatch:
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
/*  Set up new device							*/
/*									*/
/*  Devices are created on demand (either via control-shift-n or 	*/
/*  autogen) because the vctx block is such a pig			*/
/*									*/
/************************************************************************/

static Devex *initdev (int vidx)

{
  char unitdesc[20], unitname[12];
  Devex *devex;
  OZ_Devunit *devunit;
  OZ_Iochan *conclass_iochan;
  uLong dv, sts;

  /* Assign I/O channel to console class driver setup device - if not there, try later */

  sts = oz_knl_iochan_crbynm (OZ_IO_CONSOLE_SETUPDEV, OZ_LOCKMODE_CW, OZ_PROCMODE_KNL, NULL, &conclass_iochan);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_dev_console_init: error %u assigning channel to console class device '" OZ_IO_CONSOLE_SETUPDEV "'\n", sts);
    return (NULL);
  }

  /* If device already exists, just return pointer */

  dv = oz_hw_smplock_wait (&oz_s_smplock_dv);
  devex = devexs[vidx];
  if (devex != NULL) {
    oz_hw_smplock_clr (&oz_s_smplock_dv, dv);
    oz_knl_iochan_increfc (conclass_iochan, -1);
    return (devex);
  }

  /* Create the devunit */

  if (vidx == 0) {
    devunit = oz_knl_devunit_create (devdriver, "console", "keyboard and video port for 486's (control-~)", &conport_functable, 0, oz_s_secattr_sysdev);
  } else {
    oz_sys_sprintf (sizeof unitname, unitname, "console.%d", vidx);
    oz_sys_sprintf (sizeof unitdesc, unitdesc, "control-shift-%d", vidx);
    devunit = oz_knl_devunit_create (devdriver, unitname, unitdesc, &conport_functable, 0, oz_s_secattr_sysdev);
  }

  /* Set up keyboard context block */

  devexs[vidx] = devex = oz_knl_devunit_ex (devunit);
  memset (devex, 0, sizeof *devex);
  oz_hw_smplock_clr (&oz_s_smplock_dv, dv);

  devex -> devunit = devunit;
  devex -> vctx    = oz_dev_video_initctx (devex, vidx);
  devex -> conclass_iochan = conclass_iochan;

  /* Send port parameters to class driver, get class driver parameters back */

  devex -> comport_setup.port_devunit      = devunit;
  devex -> comport_setup.port_param        = devex;
  devex -> comport_setup.port_read_start   = conport_read_start;
  devex -> comport_setup.port_disp_start   = conport_disp_start;
  devex -> comport_setup.port_disp_suspend = conport_disp_suspend;
  devex -> comport_setup.port_kbd_rah_full = conport_kbd_rah_full;
  devex -> comport_setup.port_terminate    = conport_terminate;
  devex -> comport_setup.port_getsetmode   = conport_getsetmode;
  devex -> comport_setup.port_lkprm        = smplock_kb;
  devex -> comport_setup.port_lockdb       = (void *)oz_hw_smplock_wait;
  devex -> comport_setup.port_unlkdb       = (void *)oz_hw_smplock_clr;

  sts = oz_knl_io (devex -> conclass_iochan, OZ_IO_COMPORT_SETUP, sizeof devex -> comport_setup, &(devex -> comport_setup));
  if (sts != OZ_SUCCESS) oz_crash ("oz_dev_console_init: error %u setting up console device %d\n", sts, vidx);

  return (devex);
}
