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
/*  Console port driver for 486's					*/
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

static uLong conport_486_assign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode);
static int conport_486_deassign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv);
static void conport_486_abort (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Ioop *ioop, void *iopexv, OZ_Procmode procmode);
static uLong conport_486_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnex, OZ_Procmode procmode, 
                                OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap);

static OZ_Devfunc conport_486_functable = { sizeof (Devex), sizeof (Chnex), sizeof (Iopex), 0, NULL, NULL, NULL, conport_486_assign, 
                                            conport_486_deassign, conport_486_abort, conport_486_start, NULL };

static OZ_Devclass *devclass;
static OZ_Devdriver *devdriver;
static Devex *devexs[NUMSCREENS];
static Devex *cdevex = NULL;

static int initialized = 0;
static OZ_Lowipl *login_lowipl = NULL;

static OZ_Lowipl *setnewvctx_lowipl = NULL;
static int setnewvctx_vidx = 0;

static OZ_Hw486_irq_many kb_irq_many;
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
static int keyboard_interrupt (void *dummy, OZ_Mchargs *mchargs);
static void login_entry (void *dummy, OZ_Lowipl *lowipl);
static void setnewvctx (void *dummy, OZ_Lowipl *lowipl);
static OZ_Devunit *autogen (void *dummy, OZ_Devunit *host_devunit, const char *devname, const char *suffix);
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
    oz_knl_printk ("oz_dev_console_486_init\n");

    memset (devexs, 0, sizeof devexs);				/* aint got no devices yet */

    devclass   = oz_knl_devclass_create (OZ_IO_COMPORT_CLASSNAME, OZ_IO_COMPORT_BASE, OZ_IO_COMPORT_MASK, "oz_dev_console_486");
    devdriver  = oz_knl_devdriver_create (devclass, "oz_dev_console_486");
    kb_irq_many.entry = keyboard_interrupt;			/* set up interrupt routine */
    kb_irq_many.param = NULL;
    kb_irq_many.descr = "console keyboard";
    smplock_kb = oz_hw486_irq_many_add (KEYBOARD_IRQ, &kb_irq_many);
    oz_dev_keyboard_getc (1);					/* doesn't seem to interrupt unless this is here */

    devex = initdev (0);					/* init main console only (others on demand) */
    if (devex == NULL) return;					/* return if can't do it yet */
    conport_486_functable.chn_exsize += devex -> comport_setup.class_functab -> chn_exsize;
    conport_486_functable.iop_exsize += devex -> comport_setup.class_functab -> iop_exsize;
    login_lowipl = oz_knl_lowipl_alloc ();			/* set up a control-shift-L lowipl struct */
								/* (main console only, others just use <CR> to access) */
    cdevex = devex;						/* current screen = this one-and-only screen */
    if (!oz_s_inloader) {					/* no screen switching in loader */
      setnewvctx_lowipl = oz_knl_lowipl_alloc ();		/* arm routine to switch screens */
      oz_knl_devunit_autogen (devex -> devunit, autogen, NULL);	/* set up autogen to create other units on demand */
    }
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

static uLong conport_486_assign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode)

{
  Devex *devex;

  devex = devexv;
  return ((*(devex -> comport_setup.class_functab -> assign)) (devunit, devex -> comport_setup.class_devex, iochan, ((Chnex *)chnexv) -> class_area, procmode));
}

/* A channel is being deassigned from the device */

static int conport_486_deassign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv)

{
  Devex *devex;

  devex = devexv;
  return ((*(devex -> comport_setup.class_functab -> deassign)) (devunit, devex -> comport_setup.class_devex, iochan, ((Chnex *)chnexv) -> class_area));
}

/* Abort an I/O function */

static void conport_486_abort (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Ioop *ioop, void *iopexv, OZ_Procmode procmode)

{
  Devex *devex;

  devex = devexv;
  (*(devex -> comport_setup.class_functab -> abort)) (devunit, devex -> comport_setup.class_devex, iochan, ((Chnex *)chnexv) -> class_area, ioop, ((Iopex *)iopexv) -> class_area, procmode);
}

/* Start an I/O function */

static uLong conport_486_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode, 
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
    oz_hw486_outb (oz_hw486_inb (0x61) | 0x03, 0x61);		/* - if not, turn the beep on */
    oz_hw486_outb (0xb6, 0x43);
    oz_hw486_outb (0x36, 0x42);
    oz_hw486_outb (0x03, 0x42);
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
    oz_hw486_outb (oz_hw486_inb (0x61) & 0xfc, 0x61);		/* ok, turn the damn thing off */
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

static uByte keyboard_offs = 0;		/* <6> = 0 : ctrl key released */
					/*       1 : ctrl key pressed */
					/* <7> = 0 : shift key released */
					/*       1 : shift key pressed */

static uByte keyboard_e0 = 0;		/* 0 : last keycode was not E0 */
					/* 1 : last keycode was E0 */

/* Single character scancode translation */

#define RSH -1		/* right shift key */
#define LSH -1		/* left shift key (treat same as right) */
#define LAL  0		/* left alt key (ignore) */
#define CL   0		/* caps lock (ignore) */
#define CTR -2		/* ctrl key */
#define CSC -3		/* control-shift-C (call debugger) */
#define CSD -4		/* control-shift-D (enter diag mode) */
#define CSU -5		/* control-shift-U (scroll up a line) */
#define CSJ -6		/* control-shift-J (scroll down a line) */
#define KPS -7		/* keypad star (return PF3 multibyte) */
#define CSL -8		/* control-shift-L (login console) */
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

	/*0  x1  x2  x3  x4  x5  x6  x7  x8  x9  xA  xB  xC  xD  xE  xF*/

static const Byte keyboard_table[256] = {

	/* shift up & ctrl up */

	  0, 27,'1','2','3','4','5','6','7','8','9','0','-','=',127,  9,		/* 0x                                                      */
	'q','w','e','r','t','y','u','i','o','p','[',']', 13,CTR,'a','s',		/* 1x  cr=carriage return; ctr=ctrl-key                    */
	'd','f','g','h','j','k','l',';', 39,'`',LSH,92, 'z','x','c','v',		/* 2x  lsh=left-shift-key; 39=apostrophe, 92=backslash     */
	'b','n','m',',','.','/',RSH,KPS,LAL,' ', CL, 0,   0,  0,  0,  0,		/* 3x  rsh=right-shift-key; cl=caps lock; lal=left-alt-key */

	/* shift up & ctrl down */

	  0, 27,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  8,  9,		/* 0x                                                      */
	 17, 23,  5, 18, 20, 25, 21,  9, 15, 16, 27, 29, 13,CTR,  1, 19,		/* 1x  cr=carriage return; ctr=ctrl-key                    */
	  4,  6,  7,  8, 10, 11, 12,  0,  0,  0,LSH, 28, 26, 24,  3, 22,		/* 2x  lsh=left-shift-key                                  */
	  2, 14, 13,',','.','/',RSH,KPS,LAL,  0, CL,  0,  0,  0,  0,  0,		/* 3x  rsh=right-shift-key; cl=caps lock; lal=left-alt-key */

	/* shift down & ctrl up */

	  0, 27,'!','@','#','$','%','^','&','*','(',')','_','+',127,  9,		/* 0x                                                      */
	'Q','W','E','R','T','Y','U','I','O','P','{','}', 13,CTR,'A','S',		/* 1x  return; ctr=ctrl-key                                */
	'D','F','G','H','J','K','L',':','"','~',LSH,'|','Z','X','C','V',		/* 2x  lsh=left-shift-key                                  */
	'B','N','M','<','>','?',RSH,KPS,LAL,' ', CL,  0,  0,  0,  0,  0,		/* 3x  rsh=right-shift-key; cl=caps lock; lal=left-alt-key */

	/* shift down & ctrl down */

	  0,  0,CS1,CS2,CS3,CS4,CS5,CS6,CS7,CS8,CS9,CS0,  0,  0,  8,  9,		/* 0x                                                      */
	  0,  0,  0,  0,  0,  0,CSU,  0,  0,  0,  0,  0,  0,CTR,  0,  0,		/* 1x  return; ctr=ctrl-key                                */
	CSD,  0,  0,  0,CSJ,  0,CSL,  0,  0,CS0,LSH,  0,  0,  0,CSC,  0,		/* 2x  lsh=left-shift-key; csc=control-shift-C             */
	  0,  0,  0,  0,  0,  0,RSH,KPS,LAL,  0, CL,  0,  0,  0,  0,  0			/* 3x  rsh=right-shift-key; cl=caps lock; lal=left-alt-key */
	};

/* Multibyte scancode translation */
/* - the first byte, if not <esc> is used only if oz_dev_video_mode_keypad_app is clear or ignmb is set */
/*   otherwise, the first byte is skipped and the escape string is used                                 */
/* - if the first byte is <esc>, the key is ignored if ignmb is set                                     */
/*   otherwise, the escape string is returned, regardless of oz_dev_video_mode_keypad_app               */

#define PF1 "\033OP"
#define PF2 "/\033OQ"
#define PF3 "*\033OR"
#define PF4 "-\033OS"

#define KP0 "0\033Op"
#define KP1 "1\033Oq"
#define KP2 "2\033Or"
#define KP3 "3\033Os"
#define KP4 "4\033Ot"
#define KP5 "5\033Ou"
#define KP6 "6\033Ov"
#define KP7 "7\033Ow"
#define KP8 "8\033Ox"
#define KP9 "9\033Oy"

#define KPDP ".\033On"
#define KPEN "\015\033OM"
#define KPPL "+\033Ol"	/* vt100's ',' code ('-' is Om) */

#define U_A "\033[A"	/* brackets get changed to O's if oz_dev_video_mode_cursor_key is set */
#define D_A "\033[B"
#define R_A "\033[C"
#define L_A "\033[D"

/* Used for scancodes 40-7F (in the keypad area) (ctrl & shift ignored) */

static const char *const kbkeypad_table[64] = {
	NULL, NULL, NULL, NULL, NULL, PF1,  NULL, KP7,    KP8,  KP9,  PF4,  KP4,  KP5,  KP6,  KPPL, KP1,  /* 40-4F */
	KP2,  KP3,  KP0,  KPDP, NULL, NULL, NULL, NULL,   NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, /* 50-5F */
	NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,   NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, /* 60-6F */
	NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,   NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL  /* 70-7F */
};

/* Used when the last scancode was E0 (ctrl & shift ignored) */

static const char *const kbe0_table[128] = {
	NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,   NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, /* 00-0F */
	NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,   NULL, NULL, NULL, NULL, KPEN, NULL, NULL, NULL, /* 10-1F */
	NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,   NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, /* 30-2F */
	NULL, NULL, NULL, NULL, NULL, PF2,  NULL, NULL,   NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, /* 30-3F */
	NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,   U_A,  NULL, NULL, L_A,  NULL, R_A,  NULL, NULL, /* 40-4F */
	D_A,  NULL, NULL, NULL, NULL, NULL, NULL, NULL,   NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, /* 50-5F */
	NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,   NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, /* 60-6F */
	NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,   NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL  /* 70-7F */
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

static int keyboard_interrupt (void *dummy, OZ_Mchargs *mchargs)

{
  Byte bl;
  int hwi, newvidx;
  OZ_Lowipl *lowipl;

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

    newvidx = 0;
    switch (bl) {
      case CSC: {
        keyboard_offs = 0;				/* assume they will release control and shift keys */
        hwi = oz_hw_cpu_sethwints (0);			/* inhibit hardware interrupt delivery during debug */
        oz_knl_printk ("oz_dev_console_486: control-shift-C pressed - calling debugger\n");
        oz_knl_debug_exception (NULL, mchargs);		/* call the debugger */
        oz_hw_cpu_sethwints (hwi);
        break;
      }
      case CSD: { 
        keyboard_offs = 0;				/* assume they will release control and shift keys */
        oz_knl_printk ("oz_dev_console_486: control-shift-D pressed - entering diag mode\n");
        oz_hw_diag ();					/* call the diagnostic routine */
        break;
      }
      case CSL: {
        OZ_Lowipl *lowipl;

        lowipl = login_lowipl;
        if (lowipl == NULL) {
          oz_knl_printk ("oz_dev_console_486: control-shift-L pressed - system hung\n");
        } else {
          oz_knl_printk ("oz_dev_console_486: control-shift-L pressed - logging on\n");
          login_lowipl = NULL;
          oz_knl_lowipl_call (lowipl, login_entry, NULL);
        }
        break;
      }

      case CS9: newvidx ++;
      case CS8: newvidx ++;
      case CS7: newvidx ++;
      case CS6: newvidx ++;
      case CS5: newvidx ++;
      case CS4: newvidx ++;
      case CS3: newvidx ++;
      case CS2: newvidx ++;
      case CS1: newvidx ++;
      case CS0: {
        lowipl = setnewvctx_lowipl;				/* control-shift-<n>, see if switch already in progress */
        if (lowipl != NULL) {					/* if so, ignore it */
          setnewvctx_lowipl = NULL;				/* if not, mark switch in progress */
          setnewvctx_vidx = newvidx;				/* save the 'n' being switched to */
          cdevex = NULL;					/* don't process any more console input until switch completes */
          oz_knl_lowipl_call (lowipl, setnewvctx, NULL);	/* start switching via lowipl routine */
        }
        break;
      }
    }
  }

  return (0);
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
/*  Lowipl routine to perform screen switch				*/
/*  This is done at low ipl in case a new device needs to be made	*/
/*									*/
/************************************************************************/

static void setnewvctx (void *dummy, OZ_Lowipl *lowipl)

{
  Devex *devex;

  devex = devexs[setnewvctx_vidx];				/* point to its context block */
  if (devex == NULL) devex = initdev (setnewvctx_vidx);		/* create if it doesn't exist */
  if (devex != NULL) {
    oz_dev_video_switch (devex -> vctx);			/* switch to it */
    cdevex = devex;						/* turn keyboard interrupt processing back on */
  }
  setnewvctx_lowipl = lowipl;					/* re-arm routine to switch again */
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

#if 000 // cant because we wont be able to switch cpu's in an SMP when it gets in debugger via ctrl-shift-C - so what do we break??
  if (initialized) {
    kb = oz_hw_cpu_smplevel ();
    if (kb < oz_hw_smplock_level (smplock_kb)) oz_hw_smplock_wait (smplock_kb); /* lock others out of keyboard */
  }
#endif
  oz_hw_putcon (pmtsize, pmtbuff);					/* output prompt string */
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
#if 000
  if (initialized && (kb < oz_hw_smplock_level (smplock_kb))) oz_hw_smplock_clr (smplock_kb, kb); /* give others access to keyboard */
#endif
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
  Byte bl;

  while ((bl = oz_dev_keyboard_getc (1)) != 0) { /* see if keyboard character waiting */
    if (bl == CSC) {				/* check for control-shift-C */
      keyboard_offs = 0;			/* assume they will release control and shift keys */
      return (1);				/* this is what we have been looking for */
    }
  }

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
  Byte bl;
  uByte al;

  static const char *multibyte = NULL;

  /* If someone is looking for stuff from keyboard, light up the screen */

  if (!oz_s_inloader) {				/* oz_hw_tod_getnow may not be set up yet */
    if (videoblanked) {				/* if video is blanked, turn it on */
      oz_dev_vgavideo_blank (0);
      videoblanked = 0;
    }
    lastkeystroke = oz_hw_tod_getnow ();	/* anyway, remember when last keyboard stroke was */
  }

  /* If ignoring mulibyte sequences, cancel any that may be in progress         */
  /* Otherwise, if one is in progress and it is not exhausted, return next char */

  if (ignmb) multibyte = NULL;			/* if ignoring multibyte, flush any that might be there */
  else if (multibyte != NULL) {			/* if multibyte in progress, ... */
    bl = *(multibyte ++);			/* ... get next ascii character in sequence */
    if ((bl == '[') && oz_dev_video_mode_cursor_key) bl = 'O';
    if (bl != 0) return (bl);			/* ... return it if haven't reached the end */
    multibyte = NULL;				/* ... otherwise, we're done with it */
  }

  /* No multibyte in progress, get scan code from keyboard */

retry:
  al = oz_hw486_inb (KB_CP);			/* check the command port */
  if ((al & 0x21) != 0x01) return (0);		/* if no low bit, no keyboard char present */
						/* ... also make sure there is no mouse bit set */
  al = oz_hw486_inb (KB_DP);			/* ok, read the keyboard char */

  /* If code is E0, remember it and apply it to next scancode we get */

  if (al == 0xe0) {				/* check for 'E0' code */
    keyboard_e0 = 1;				/* remember that we got an 'E0' code */
    goto retry;					/* ... then try to see what follows it */
  }

  /* If last code was E0 use multibyte translation tables */

  if (keyboard_e0) {				/* see if last scancode was E0 */
    keyboard_e0 = 0;				/* if so, it only has effect on this code */
    if (al & 0x80) goto retry;			/* ignore any key-up codes */
    multibyte = kbe0_table[al];			/* return pointer for E0-xx table entry */
    goto chkmb;
  }

  /* If bit <6> is set, use multibyte translation table */

  if (al & 0x40) {				/* check for keypad area of keyboard */
    if (al & 0x80) goto retry;			/* ignore key-up sequences */
    multibyte = kbkeypad_table[al&0x3f];	/* return pointer to corresponding string */
    goto chkmb;
  }

  /* Use single character translation table */

  bl = keyboard_table[(al&0x3f)|keyboard_offs];	/* ok, translate given current shift/ctrl state */
  if (bl == 0) goto retry;			/* if 0 entry, ignore it & try again */
  if (bl < 0) goto special;			/* if neg, it is a special code */
  if (al & 0x80) goto retry;			/* if key-up, ignore and try again */
  return (bl);					/* ok, just return the character itself */

special:
  switch (bl) {
    case KPS: {						/* keypad star */
      if (al & 0x80) goto retry;			/* ignore key-up */
      multibyte = PF3;					/* use PF3 multibyte */
      goto chkmb;
    }
    case CSJ: {						/* control-shift-J */
      if (al & 0x80) goto retry;			/* ignore key-up */
      oz_dev_video_linedn ();				/* scroll line down one line */
      break;
    }
    case CSU: {						/* control-shift-U */
      if (al & 0x80) goto retry;			/* ignore key-up */
      oz_dev_video_lineup ();				/* scroll line up one line */
      break;
    }
    case LSH: {
      al = (al & 0x80) ^ 0x80;				/* shift key, mask and invert the key-up bit */
      keyboard_offs = (keyboard_offs & 0x7f) | al;	/* store in keyboard_offs<7> */
      break;
    }
    case CTR: {
      al = (al & 0x80) ^ 0x80;				/* control key, mask and invert the key-up bit */
      keyboard_offs = (keyboard_offs & 0xbf) | (al >> 1); /* store in keyboard_offs<6> */
      break;
    }
    default: { /* control-shift-C, -D, -L, etc */
      if (al & 0x80) goto retry;			/* ignore key-up */
      return (bl);					/* return the code to caller */
    }
  }
  goto retry;

  /* Just starting a multibyte string of some sort */

chkmb:
  if (multibyte == NULL) goto retry;		/* if the entry is NULL, just ignore it */
  bl = *multibyte;				/* get the first byte of the string */

  /* - ignmb is set by the internal read routines like oz_hw_getcon */
  /*   under no circumstances should a multibyte string be returned */

  if (ignmb) {					/* see if caller wants us to ignore multibyte sequences */
    multibyte = NULL;				/* if so, forget about it */
    if (bl == 27) goto retry;			/* if there is no single byte equivalent, get another scancode */
    return (bl);				/* return the single byte equivalent */
  }

  /* - oz_dev_video_mode_keypad_app is set by the application to return escape sequences for the numeric keypad keys                                   */
  /*   if set, send the escape sequences for all the keys                                                                                              */
  /*   if clear, send the single byte character for those that have them (like the numbers), and escape sequences for all others (like the arrow keys) */

  if (oz_dev_video_mode_keypad_app) {		/* see if the keypad multibyte codes are enabled by the application */
    if (bl != 27) multibyte ++;			/* if so, skip over any possible single byte equivalent */
    return (*(multibyte ++));			/* return the initial escape character */
  }
  if (bl != 27) multibyte = NULL;		/* otherwise, if single byte equivalend, cancel multibyte string */
  else multibyte ++;				/* if no single byte equivalent, return the multibyte string anyway */
  return (bl);
}

/************************************************************************/
/*									*/
/*  Autogen a new device						*/
/*									*/
/*  This routine is called at softint level when a reference is made 	*/
/*  to a non-existant device that begins with 'console.', in the hopes 	*/
/*  that this routine will be able to create it.			*/
/*									*/
/*  This routine will create devices 'console.1' through 'console.9'.	*/
/*									*/
/************************************************************************/

static OZ_Devunit *autogen (void *dummy, OZ_Devunit *host_devunit, const char *devname, const char *suffix)

{
  Devex *devex;
  int usedup, vidx;

  /* Decode vidx from suffix.  It must not be null, it must end at end of string, it must not be zero (that */
  /* device exists and is called 'console'), and must not be greater that what the devexs array can handle. */

  vidx = oz_hw_atoi (suffix, &usedup);
  if ((usedup == 0) || (suffix[usedup] != 0) || (vidx == 0) || (vidx >= NUMSCREENS)) return (NULL);

  /* Set it up */

  devex = initdev (vidx);			/* try to create console.<vidx> */
  if (devex == NULL) return (NULL);		/* if failure, return NULL */
  return (devex -> devunit);			/* ok, return devunit pointer */
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
    oz_knl_printk ("oz_dev_console_init: error %u assigning channel to console class device 'conclass'\n", sts);
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
    devunit = oz_knl_devunit_create (devdriver, "console", "keyboard and video port for 486's (control-~)", &conport_486_functable, 0, oz_s_secattr_sysdev);
  } else {
    oz_sys_sprintf (sizeof unitname, unitname, "console.%d", vidx);
    oz_sys_sprintf (sizeof unitdesc, unitdesc, "control-shift-%d", vidx);
    devunit = oz_knl_devunit_create (devdriver, unitname, unitdesc, &conport_486_functable, 0, oz_s_secattr_sysdev);
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
