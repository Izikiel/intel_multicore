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
/*  oz_hw_getcon routine for 486's for use in the loader		*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_dev_console_486.h"
#include "oz_dev_vgavideo_486.h"
#include "oz_knl_debug.h"
#include "oz_knl_devio.h"
#include "oz_knl_hw.h"
#include "oz_knl_procmode.h"

#define KB_CP 0x64	/* keyboard command port */
#define KB_DP 0x60	/* keyboard data port */

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

  offs = 0;								/* start at beginning of buffer */
  oz_hw_putcon (pmtsize, pmtbuff);					/* output prompt string */
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
/*  Send a string to class driver as if it came from keyboard		*/
/*									*/
/************************************************************************/

void oz_dev_keyboard_send (void *devexv, int size, char *buff)

{ }

/************************************************************************/
/*									*/
/*  This routine is called by the debugger on the primary cpu when it 	*/
/*  is waiting for another cpu to execute.  It checks to see if 	*/
/*  control-shift-C has been pressed.					*/
/*									*/
/************************************************************************/

int oz_knl_console_debugchk (void)

{
  return (0);
}
