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
/*  SRM console video driver						*/
/*									*/
/*  This is not set up as a classic device driver.  It is just a bunch 	*/
/*  of subroutines that are called by the keyboard driver 		*/
/*  (oz_dev_console_axp.c) and by the kernel.				*/
/*									*/
/************************************************************************/

#include "ozone.h"

#include "oz_dev_console_486.h"
#include "oz_dev_vgavideo_486.h"
#include "oz_knl_hw.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_sdata.h"
#include "oz_knl_status.h"
#include "oz_sys_dateconv.h"

#include <stdarg.h>

#define NCOL 80
#define NROW 25

/* Per-screen context block */

struct OZ_Vctx { int vidx;			/* video index number */
                 void *keyboard_devexv;		/* keyboard's devex pointer */
               };

/* Static data */

					/* the next 4 global vars are swapped when the current screen is swapped */
int oz_dev_video_mode_cursor_key = 0;	/* 0: send <esc>[x, 1: send <esc>Ox for arrow keys */
int oz_dev_video_mode_keypad_app = 0;	/* 0: send numeric keypad codes, 1: send escape keypad codes */
int oz_dev_video_mode_local_echo = 0;	/* 0: remote echo, 1: local echo */
int oz_dev_video_mode_newline    = 0;	/* 0: return key sends just return and lf/ff/vt stay in same column */
					/* 1: return key sends cr and lf, and lf/ff/vt reset to first column */

int oz_dev_video_pagemode = 1;		/* 0: don't pause at end-of-page */
					/* 1: pause at each end-of-page */
					/* toggled by control-shift-Q in oz_dev_keyboard_getc routine */

static uLong curcol = 0;		/* current column on screen (0..79) */
uLong oz_dev_video_currow = NROW;	/* rows since last pause */
					/* setting to NROW causes a pause as soon as it is enabled for the first time */

static OZ_Vctx vctx0;			/* context block for screen zero */

static int escnum (uLong size, const char *buff, int *number_r);
static void inccurrow (void);
static void putf (const char *format, ...);
static uLong putf_helper (void *dummy, uLong *size, char **buff);
static void putstring (uLong size, const char *buff);

/************************************************************************/
/*									*/
/*  Initialize video driver						*/
/*									*/
/************************************************************************/

/* This is called at the very beginning of the load process so we can use it to print stuff via oz_hw_putcon */

void oz_dev_video_init (void)

{ }

/* This is called at softint level by the keyboard driver - it sets up a new screen numbered 'vidx' */

OZ_Vctx *oz_dev_video_initctx (void *devexv, int vidx)

{
  return (&vctx0);
}

/************************************************************************/
/*									*/
/*  Switch currently displayed context blocks				*/
/*									*/
/*    Input:								*/
/*									*/
/*	vctx = new video context to be displayed			*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_dev_video_switch = old video context				*/
/*	dvctx = new video screen					*/
/*	video screen and cursor updated with switched contents		*/
/*									*/
/************************************************************************/

OZ_Vctx *oz_dev_video_switch (OZ_Vctx *vctx)

{
  return (&vctx0);
}

/************************************************************************/
/*									*/
/*  Set modes								*/
/*									*/
/*  This routine gets called by the keyboard driver to set the display 	*/
/*  modes, as a result of an OZ_IO_CONSOLE_SETMODE or _GETMODE.		*/
/*									*/
/*    Input:								*/
/*									*/
/*	vctx  = which screen the settings are for			*/
/*	size  = size of caller's mode buffer				*/
/*	buff  = address of caller's mode buffer				*/
/*	*buff = new settings requested					*/
/*									*/
/*    Output:								*/
/*									*/
/*	*buff = new settings realized					*/
/*									*/
/************************************************************************/

void oz_dev_video_setmode (OZ_Vctx *vctx, uLong size, OZ_Console_modebuff *buff)

{ }

/************************************************************************/
/*									*/
/*  Called by keyboard driver to get contents of screen			*/
/*									*/
/************************************************************************/

void oz_dev_video_getscreen (OZ_Vctx *vctx, uLong size, OZ_Console_screenbuff *buff)

{ }

/************************************************************************/
/*									*/
/*  Called by default fatal exception handler to lock all other cpus 	*/
/*  out of the video system while it dumps stuff			*/
/*									*/
/*    Input:								*/
/*									*/
/*	flag = 0 : release exclusive access (not used)			*/
/*	       1 : acquire exclusive access				*/
/*									*/
/*    Output:								*/
/*									*/
/*	exclusive = -1 : exclusive access released			*/
/*	          else : exclusive access acquired by this cpu		*/
/*									*/
/*    Note:								*/
/*									*/
/*	This routine hangs in a spinlock type wait if another cpu has 	*/
/*	the exclusive lock						*/
/*									*/
/************************************************************************/

void oz_dev_video_exclusive (int flag)

{ }

/************************************************************************/
/*									*/
/*  Put a string on the screen						*/
/*									*/
/*    Input:								*/
/*									*/
/*	vctx = which screen the string is for				*/
/*	       NULL: output to whatever is currently being displayed	*/
/*	size = number of characters to output				*/
/*	buff = characters to output					*/
/*									*/
/*    Output:								*/
/*									*/
/*	string output to screen						*/
/*									*/
/************************************************************************/

void oz_hw_putcon (uLong size, const char *buff)

{
  uLong i;

  while (size > 0) {
    for (i = 0; i < size; i ++) if (buff[i] == '\n') break;		// check for an LF
    if ((i < size) && ((i == 0) || (buff[i-1] != '\r'))) {		// ... that doesn't have a preceding CR
      if (i > 0) {
        oz_dev_video_putstring (NULL, i, buff);				// ok, output up to but not including LF
        size -= i;							// skip over that much
        buff += i;
      }
      oz_dev_video_putstring (NULL, 2, "\r\n");				// ... and print a CRLF
      -- size;								// skip over the LF in buffer
      buff ++;
    } else {
      oz_dev_video_putstring (NULL, size, buff);
      break;
    }
  }
}

/* - it may quietly output to a screen that is not being displayed */
/*   it does not insert a carriage return before line feeds        */
/*   it is used by the class/port driver output routines           */

void oz_dev_video_putstring (OZ_Vctx *vctx, uLong size, const char *buff)

{
  char c;
  uLong i;

  while (size > 0) {
    for (i = 0; i < size; i ++) if (((c = buff[i]) < ' ') || (c >= 127)) break;
    if (i > 0) {							// see if any printables at beginning of buffer
      if (curcol >= NCOL) {						// maybe we have to wrap to start
        putstring (2, "\r\n");
        curcol = 0;
        inccurrow ();
      }
      if (curcol + i > NCOL) i = NCOL - curcol;				// maybe it would go off end
      putstring (i, buff);						// output what we can
      curcol += i;
      size   -= i;							// that much less to do now
      buff   += i;
    } else {
      switch (c) {							// control char
        case  8: {							// - BS
          if (curcol > 0) {
            putstring (1, buff);
            -- curcol;
          }
          break;
        }
        case  9: {							// - TAB
          i = (curcol & -8) + 8;					//   advance to next 8-stop
          oz_dev_video_putstring (NULL, i - curcol, "        ");	//   ... with wrap
          break;
        }
        case 10: {							// - LF
          putstring (1, "\n");
          inccurrow ();							//   on to next line
          break;
        }
        case 13: {							// - CR
          if ((size > 1) && (buff[1] == '\n')) {
            putstring (2, "\r\n");
            curcol = 0;
            inccurrow ();
            -- size;
            buff ++;
          } else {
            putstring (1, "\r");
            curcol = 0;							//   beg of current line
          }
          break;
        }
        case 27: {							// ESC
          int digits, number;

          if ((size >= 3) && (buff[1] == '[') && (buff[2] == 'J')) {	// - erase to end of screen
            putstring (3, buff);
            size -= 2;
            buff += 2;
          } else if ((size >= 3) 					// - move cursor around
                  && (buff[1] == '[') 
                  && ((digits = escnum (size - 2, buff + 2, &number)) >= 0) 
                  && (buff[digits+2] >= 'A') 
                  && (buff[digits+2] <= 'D')) {
            putstring (digits + 3, buff);
            switch (buff[digits+2]) {
              case 'A': {
                if (oz_dev_video_currow < number) oz_dev_video_currow = 0;
                else oz_dev_video_currow -= number;
                break;
              }
              case 'B': {
                oz_dev_video_currow += number;
                if (oz_dev_video_currow >= NROW) oz_dev_video_currow = NROW - 1;
                break;
              }
              case 'C': {
                curcol += number;
                if (curcol >= NCOL) curcol = NCOL - 1;
                break;
              }
              case 'D': {
                if (curcol < number) curcol = 0;
                else curcol -= number;
                break;
              }
            }
            size -= digits + 2;
            buff += digits + 2;
          } else {
            putstring (5, "<ESC>");
            curcol += 5;
          }
        }
      }
      -- size;
      buff ++;
    }
  }
}

static int escnum (uLong size, const char *buff, int *number_r)

{
  char c;
  int digits, number;

  number = 0;
  for (digits = 0; digits < size; digits ++) {
    c = buff[digits];
    if (c < '0') break;
    if (c > '9') break;
    number = number * 10 + c - '0';
  }
  if (digits == 0) number = 1;
  *number_r = number;
  return (digits);
}

/* Increment current row, pausing if screen filled */

static void inccurrow (void)

{
  char kbchar;

  oz_dev_video_currow ++;
  if (oz_dev_video_pagemode && (oz_dev_video_currow >= NROW - 1)) {
    putstring (80, "\r           (Space new page / Enter new line / Control-Shift-Q toggle)          ");
    do {
      kbchar = oz_dev_keyboard_getc (1);
      if ((kbchar == 4) || (kbchar == 26)) OZ_HWAXP_HALT ();
    } while (oz_dev_video_pagemode && (kbchar != 32) && (kbchar != 13));
    oz_dev_video_currow = 0;
    if (kbchar == 13) oz_dev_video_currow = NROW - 2;
    putstring (71 + curcol, "\r                                                                     "
                            "\r                                                                                ");
  }
}

/* Output formatted (debug) string */

static void putf (const char *format, ...)

{
  char buff[256];
  va_list ap;

  va_start (ap, format);
  oz_sys_vxprintf (putf_helper, NULL, sizeof buff, buff, NULL, format, ap);
  va_end (ap);
}

static uLong putf_helper (void *dummy, uLong *size, char **buff)

{
  putstring (*size, *buff);
  return (OZ_SUCCESS);
}

/* Just output string as is, with no editing or counting */
/* Can be called at any IPL or smplevel                  */

static void putstring (uLong size, const char *buff)

{
  uQuad savedipl;

  while (size > 0) {
    savedipl = OZ_HWAXP_MTPR_IPL (31);					// lock out all ints, we don't know console IPL
    {
      register uQuad __r0  asm ("$0");
      register uQuad __r16 asm ("$16") = OZ_HWAXP_DISPATCH_PUTS;
      register uQuad __r17 asm ("$17") = 0;
      register uQuad __r18 asm ("$18") = (OZ_Pointer)buff;
      register uQuad __r19 asm ("$19") = size;
      register uQuad __r27 asm ("$27") = oz_hwaxp_dispatchr27;

      asm volatile ("jsr $26,(%1)" : "=r"(__r0) 
                                   : "r"(oz_hwaxp_dispatchent), "r"(__r16), "r"(__r17), "r"(__r18), "r"(__r19), "r"(__r27)
                                   : "$1", "$16", "$17", "$18", "$19", "$20", "$21", "$22", "$23", "$24", "$25", "$26", "$27", "$28");
      if ((__r0 >> 62) != 0) OZ_HWAXP_HALT ();				// if can't even print, just halt
      size -= (uLong)__r0;						// this much less to do
      buff += (uLong)__r0;
    }
    OZ_HWAXP_MTPR_IPL (savedipl);					// lower IPL so we don't hog the CPU too long
  }
}

/************************************************************************/
/*									*/
/*  Update hardware screen cursor					*/
/*									*/
/*    Input:								*/
/*									*/
/*	vctx = which screen to update the cursor on			*/
/*	       NULL: the current display screen				*/
/*									*/
/************************************************************************/

void oz_dev_video_updcursor (OZ_Vctx *vctx)

{ }

/************************************************************************/
/*									*/
/*  Put a character on the screen					*/
/*  Caller must call oz_dev_video_updcursor when all done with screen	*/
/*									*/
/*    Input:								*/
/*									*/
/*	vctx = screen to put character on				*/
/*	c = character to put						*/
/*									*/
/*    Output:								*/
/*									*/
/*	character output to screen					*/
/*									*/
/************************************************************************/

void oz_dev_video_putchar (OZ_Vctx *vctx, char c)

{
  oz_dev_video_putstring (vctx, 1, &c);
}

/************************************************************************/
/*									*/
/*  Call this routine when user presses 'Page Down' key on the keyboard	*/
/*  This routine will scroll the screen up (making it more 'normal')	*/
/*									*/
/************************************************************************/

void oz_dev_video_linedn (void)

{ }

/************************************************************************/
/*									*/
/*  Call this routine when user presses 'Page up' key on the keyboard	*/
/*  This routine will scroll the screen down (making it less 'normal')	*/
/*									*/
/************************************************************************/

void oz_dev_video_lineup (void)

{ }

/************************************************************************/
/*									*/
/*  Blank or unblank screen.  Keyboard driver calls this routine to 	*/
/*  blank the screen after a period of unactivity and calls it when 	*/
/*  activity resumes.							*/
/*									*/
/*    Input:								*/
/*									*/
/*	blank = 0 : restore screen contents				*/
/*	        1 : blank screen					*/
/*									*/
/************************************************************************/

void oz_dev_vgavideo_blank (int blank)

{ }

/************************************************************************/
/*									*/
/*  These routines update the status values on the screen for the 	*/
/*  calling cpu								*/
/*									*/
/************************************************************************/

/* Update the values for the calling cpu */

void oz_dev_video_statusupdate (Long cpuidx, uByte cpulevel, uByte tskpri, OZ_Pointer eip)

{ }
