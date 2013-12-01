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
/*  VGA video driver							*/
/*									*/
/*  This is not set up as a classic device driver.  It is just a bunch 	*/
/*  of subroutines that are called by the keyboard driver 		*/
/*  (oz_dev_console_486.c) and by the kernel.				*/
/*									*/
/************************************************************************/

#include "ozone.h"

#include "oz_dev_console_486.h"
#include "oz_dev_vgavideo_486.h"
#include "oz_knl_hw.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_sdata.h"
#include "oz_sys_dateconv.h"

#define CRTSEQ 0x03C4

#define VIDEOBASE ((uWord *)0xB8000)					/* base address of video memory */
#define VIDEOSCRL (VIDEOBASE + hwscrollindex * VIDEOCOLCOUNT)		/* base address scrolled */
#define VIDEOCOLCOUNT 80						/* number of columns per line */
#define VIDEOROWCOUNT 25						/* number of rows per page */
#define VIDEOPAGESIZE (VIDEOCOLCOUNT*VIDEOROWCOUNT)			/* number of characters per page */
#define VIDEOLINESAVE 500						/* number of lines to save off top of screen */

#define VIDEOCOLORUNLOCKED 7			/* white means not locked */
#define VIDEOCOLORMONO 7			/* color to use for monochrome mode */
#define VIDEOCOLORUNIPROC  6			/* yellow means uniprocessor or apic not set up */
static const uByte colortable[4] = { 5, 3, 4, 2 }; /* colors for cpu 0, 1, 2, and 3 */

#define FILLCHAR ' '				/* character to fill scroll lines with */
#define ESCSEQMAX 32				/* max bytes allowed in escape sequence */

/* Per-screen context block */

struct OZ_Vctx { int vidx;			/* video index number */
                 void *keyboard_devexv;		/* keyboard's devex pointer */
                 int mode_cursor_key;		/* 0: send <esc>[x, 1: send <esc>Ox for arrow keys */
                 int mode_keypad_app;		/* 0: send numeric keypad codes, 1: send escape keypad codes */
                 int mode_local_echo;		/* 0: remote echo, 1: local echo */
                 int mode_newline;		/* 0: return key sends just return and lf/ff/vt stay in same column */
						/* 1: return key sends cr and lf, and lf/ff/vt reset to first column */
                 char video_color;		/* color we are currently set to */

                 int video_cursor;		/* cursor position (word index in video memory) */

                 int escseqidx;			/* -1 : not in escape sequence, else number of chars in escseqbuf */
                 char escseqbuf[ESCSEQMAX];	/* escape sequence string, not including escape */

                 char tabstops[VIDEOCOLCOUNT];	/* tab stops: 0=no tab stop here, 1=tab stop here */
                 char lineheight[VIDEOROWCOUNT]; /* 0=single, 1=tophalf, -1=bottomhalf */
                 char linewidth[VIDEOROWCOUNT];	/* 0=single, 1=double */
                 int mode_cols_per_line;	/* number of columns per line, 80 or 132 */
                 int mode_reverse_video;	/* reverse video the whole screen */
                 int mode_origin;		/* 0: positioning absolute to screen */
						/* 1: positioning relative to scrolling region */
                 int mode_auto_wrap;		/* 0: don't wrap cursor to next line, leave it stuck at end of line */
						/* 1: let cursor wrap around to next line */
                 int mode_insertion;		/* 0: new characters overwrite what was there */
						/* 1: new characters shift rest of line over */
                 int mode_char_bold;		/* bold brightness */
                 int mode_char_underline;	/* underline characters */
                 int mode_char_blink;		/* blink characters */
                 int mode_char_reverse;		/* put chars in reverse video */
                 int scroll_top;		/* scrolling margin, top line, zero based, inclusive */
                 int scroll_bottom;		/* bottom line, zero based, exclusive */

                 int single_shift_2;
                 int single_shift_3;
                 char char_set_special[256];
                 char char_set_uk[256];
                 char char_set_us[256];
                 char *char_set_g0;
                 char *char_set_g1;

                 uLong linecount;		/* number of brand-spanking-new lines created */
                 int video_lineindx;		/* number of 'page up' lines that we are indexed by (0=normal display) */
                 int video_lineoffs;		/* word offset in video_linesave for next line to scroll off the top */
                 uWord video_pagesave[VIDEOPAGESIZE]; /* saved screen contents */
                 uWord video_linesave[VIDEOLINESAVE*VIDEOCOLCOUNT]; /* data scrolled off top of screen */
               };

/* Static data */

static Long video_lockflag  = -1;	/* set to -1 when unlocked, else the cpuid that has it locked */
static int video_03x4       = 0;	/* 0x3b4 or 0x3d4 */
static int video_03x5       = 0;	/* 0x3b5 or 0x3d5 */
static int hwscrollindex    = 0;	/* number of lines the hardware video memory is scrolled */

					/* the next 4 global vars are swapped when the current screen is swapped */
int oz_dev_video_mode_cursor_key = 0;	/* 0: send <esc>[x, 1: send <esc>Ox for arrow keys */
int oz_dev_video_mode_keypad_app = 0;	/* 0: send numeric keypad codes, 1: send escape keypad codes */
int oz_dev_video_mode_local_echo = 0;	/* 0: remote echo, 1: local echo */
int oz_dev_video_mode_newline    = 0;	/* 0: return key sends just return and lf/ff/vt stay in same column */
					/* 1: return key sends cr and lf, and lf/ff/vt reset to first column */

static uWord flagword = VIDEOCOLORUNLOCKED << 8; /* upper half of video word to write */
static uWord *lvb     = NULL;		/* points to the video buffer for the locked screen */
static OZ_Vctx vctx0;			/* context block for screen zero */
static OZ_Vctx *dvctx = &vctx0;		/* pointer to context block that's currently being displayed */
static OZ_Vctx *lvctx = NULL;		/* pointer to context block that's currently locked */
static uWord savevidxword;		/* saved video index word (what really goes where vidx char is) */

static volatile Long exclusive = -1;	/*   -1 : anyone can output */
					/* else : only this cpuid can output */

#ifdef OZ_DEBUG
#define STATUS_MAXCPUS (2)			// number of cpu linex in status display
#define STATUS_STARTCOL (67)			// starting column number (zero based)
#define STATUS_STARTROW (1)			// starting row number (zero based)
#define STATUS_COLUMNS (13)			// number of columns in status display
static uWord status_data[STATUS_MAXCPUS*STATUS_COLUMNS];
#endif

/* Internal routines */

static void init_vctx (OZ_Vctx *vctx, int vidx);
static void video_putchar (char c);
static void printable (char c);
static void escseqend (void);
static void index_down (int newline);
static void position_cursor (int line, int column);
static void video_scrollup (void);
static void video_scrolldown (void);
static void hwscrollindex_set (int scix);
static int video_lock (OZ_Vctx *vctx);
static void video_unlock (int hwi);
#ifdef OZ_DEBUG
static void statusbyte (uByte ub, uWord *vid, uWord fword);
static void flip_status_display (void);
#endif
static uByte getcpucolor (Long cpuidx);
static void setflagword (void);
static void charstring (char *ch, uLong hex);

/************************************************************************/
/*									*/
/*  Initialize video driver						*/
/*									*/
/************************************************************************/

/* This is called at the very beginning of the load process so we can use it to print stuff via oz_hw_putcon */

void oz_dev_video_init (void)

{
  int scix;

  video_03x4 = 0x03B4;					/* assume use 03b4/03b5 */
  if (oz_hw486_inb (0x3CC) & 1) video_03x4 = 0x3D4;	/* maybe use 03d4/03d5 */
  video_03x5 = video_03x4 + 1;

  oz_hw486_outb (0x0C, video_03x4);			// select high-order offset
  scix  = oz_hw486_inb (video_03x5);			// output high-order offset
  scix *= 256;
  oz_hw486_outb (0x0D, video_03x4);			// select low-order offset
  scix += oz_hw486_inb (video_03x5);			// output low-order offset

  hwscrollindex_set (scix / VIDEOCOLCOUNT);		/* set existing scrolling for video memory */

  init_vctx (&vctx0, 0);				/* initialize context block for device 0 */

							/* get the cursor ... */
  oz_hw486_outb (0x0E, video_03x4);			/* - output 0E -> 03?4 */
  dvctx -> video_cursor  = oz_hw486_inb (video_03x5);	/* - input 03?5 -> high-order cursor addr */
  dvctx -> video_cursor *= 256;
  oz_hw486_outb (0x0F, video_03x4);			/* - output 0F -> 03?4 */
  dvctx -> video_cursor += oz_hw486_inb (video_03x5);	/* - input 03?5 -> low-order cursor addr */
  dvctx -> video_cursor -= scix;			/* subtract off any scrolling */

  savevidxword = VIDEOSCRL[VIDEOCOLCOUNT-1];		/* save upper left corner char */

  oz_dev_video_putchar (NULL, '!');			/* put an '!' there */
}

/* This is called at softint level by the keyboard driver - it sets up a new screen numbered 'vidx' */

OZ_Vctx *oz_dev_video_initctx (void *devexv, int vidx)

{
  OZ_Vctx *vctx;

  if (vidx == 0) vctx = &vctx0;				/* number 0 is statically defined */
  else {
    if (oz_s_inloader) oz_crash ("oz_dev_video_initctx: only primary screen supported in loader");
    vctx = OZ_KNL_NPPMALLOC (sizeof *vctx);		/* allocate new context block */
    init_vctx (vctx, vidx);				/* initialize it */
  }
  vctx -> keyboard_devexv = devexv;			/* save for keyboard callbacks */
  return (vctx);
}

/* Initialize context block */

void init_vctx (OZ_Vctx *vctx, int vidx)

{
  int i;

  memset (vctx, 0, sizeof *vctx);					/* clear it all out to start with */

  vctx -> vidx = vidx;

  for (i = 0; i < VIDEOCOLCOUNT; i += 8) vctx -> tabstops[i] = 1;	/* set tab stops every 8 columns */

  for (i = 0; i < VIDEOPAGESIZE; i ++) {				/* so the cursor will show up on initial page */
    vctx -> video_pagesave[i] = VIDEOCOLORUNLOCKED << 8;
  }

  vctx -> video_color        = VIDEOCOLORUNLOCKED;			/* color we are currently set to */
  vctx -> escseqidx          = -1;					/* not in escape sequence */
  vctx -> mode_cols_per_line = 80;
  vctx -> scroll_bottom      = VIDEOROWCOUNT;				/* bottom line, zero based, exclusive */
  vctx -> char_set_g0        = vctx -> char_set_us;
  vctx -> char_set_g1        = vctx -> char_set_special;
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
  int hwi;
  OZ_Vctx *pvctx;

  hwi = video_lock (vctx);					/* lock database, select new screen */
  pvctx = dvctx;						/* save old screen's index */
  if (lvctx != dvctx) {						/* see if actually changing */

    /* Save current video context */

    dvctx -> mode_cursor_key = oz_dev_video_mode_cursor_key;
    dvctx -> mode_keypad_app = oz_dev_video_mode_keypad_app;
    dvctx -> mode_local_echo = oz_dev_video_mode_local_echo;
    dvctx -> mode_newline    = oz_dev_video_mode_newline;
    VIDEOSCRL[VIDEOCOLCOUNT-1] = savevidxword;			/* restore character in upper left corner */
    memcpy (dvctx -> video_pagesave, VIDEOSCRL, sizeof dvctx -> video_pagesave);

    /* Switch to make current = locked video context */

    hwscrollindex_set (0);
    dvctx = lvctx;
    oz_dev_video_mode_cursor_key = dvctx -> mode_cursor_key;
    oz_dev_video_mode_keypad_app = dvctx -> mode_keypad_app;
    oz_dev_video_mode_local_echo = dvctx -> mode_local_echo;
    oz_dev_video_mode_newline    = dvctx -> mode_newline;
    memcpy (VIDEOSCRL, dvctx -> video_pagesave, sizeof dvctx -> video_pagesave);
  }
  video_unlock (hwi);

  if (pvctx != vctx) oz_dev_video_updcursor (NULL);

  return (pvctx);
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

{
  int hwi;
  OZ_Console_modebuff cmb;

  movc4 (size, buff, sizeof cmb, &cmb);			/* expand/contract to size we know about */

  hwi = video_lock (vctx);				/* lock database, select screen context */

  if (cmb.linewrap > 0) lvctx -> mode_auto_wrap = 1;	/* maybe turn line auto wrap on */
  if (cmb.linewrap < 0) lvctx -> mode_auto_wrap = 0;	/* maybe turn line auto wrap off */

  cmb.columns  = VIDEOCOLCOUNT;				/* we can only do this many columns */
  cmb.rows     = VIDEOROWCOUNT;				/* we can only do this many rows */
  cmb.linewrap = (lvctx -> mode_auto_wrap * 2) - 1;	/* line auto wrap setting */

  video_unlock (hwi);					/* unlock database */

  movc4 (sizeof cmb, &cmb, size, buff);			/* copy result back to caller */
}

/************************************************************************/
/*									*/
/*  Called by keyboard driver to get contents of screen			*/
/*									*/
/************************************************************************/

void oz_dev_video_getscreen (OZ_Vctx *vctx, uLong size, OZ_Console_screenbuff *buff)

{
  int hwi, r, row;
  OZ_Console_screenbuff csb;
  uLong l;
  uWord *p;

  movc4 (size, buff, sizeof csb, &csb);			// expand/contract to size we know about

  csb.ncols    = VIDEOCOLCOUNT;				// number of columns in display
  csb.nrowstot = VIDEOLINESAVE + VIDEOROWCOUNT - 1;	// total number of rows possible
  csb.nrowscur = VIDEOLINESAVE + VIDEOROWCOUNT - 1;	// same number is current

  l = csb.size / sizeof *p;				// length of buffer in words
  p = csb.buff;						// point to buffer

  hwi = video_lock (vctx);				// lock database, select screen context

  csb.rowtop = VIDEOLINESAVE - 1 - lvctx -> video_lineindx;			// row currently at top of screen (inclusive)
  csb.rowbot = VIDEOLINESAVE - 1 - lvctx -> video_lineindx + VIDEOROWCOUNT - 1;	// row currently at bottom of screen (inclusive)
  csb.lastrowseq = lvctx -> linecount;			// number of lines created

  if (p != NULL) {

    /* Get stuff scrolled off top of screen */

    for (row = 1; (row < VIDEOLINESAVE - lvctx -> video_lineindx) && (l >= VIDEOCOLCOUNT); row ++) {
      r  = lvctx -> video_lineoffs / VIDEOCOLCOUNT + row + lvctx -> video_lineindx;
      r %= VIDEOLINESAVE;
      memcpy (p, lvctx -> video_linesave + r * VIDEOCOLCOUNT, VIDEOCOLCOUNT * sizeof *p);
#if 000
      p[3] = ':';
      p[2] = (r % 10) + '0';
      r   /= 10;
      p[1] = (r % 10) + '0';
      r   /= 10;
      p[0] = (r % 10) + '0';
#endif
      l -= VIDEOCOLCOUNT;
      p += VIDEOCOLCOUNT;
    }

    /* Get contents of screen */

    for (row = 0; (row < VIDEOROWCOUNT) && (l >= VIDEOCOLCOUNT); row ++) {
      memcpy (p, lvb + row * VIDEOCOLCOUNT, VIDEOCOLCOUNT * sizeof *p);
#if 000
      p[3] = ':';
      p[2] = (row % 10) + '0';
      p[1] = (row / 10) + '0';
      p[0] = '*';
#endif
      l -= VIDEOCOLCOUNT;
      p += VIDEOCOLCOUNT;
    }

    /* Get stuff scrolled off bottom of screen */

    for (row = VIDEOLINESAVE + 1 - lvctx -> video_lineindx; (row <= VIDEOLINESAVE) && (l >= VIDEOCOLCOUNT); row ++) {
      r  = lvctx -> video_lineoffs / VIDEOCOLCOUNT + row + lvctx -> video_lineindx;
      r %= VIDEOLINESAVE;
      memcpy (p, lvctx -> video_linesave + r * VIDEOCOLCOUNT, VIDEOCOLCOUNT * sizeof *p);
#if 000
      p[3] = ':';
      p[2] = (r % 10) + '0';
      r   /= 10;
      p[1] = (r % 10) + '0';
      r   /= 10;
      p[0] = (r % 10) + '0';
#endif
      l -= VIDEOCOLCOUNT;
      p += VIDEOCOLCOUNT;
    }
  }

  video_unlock (hwi);					// unlock database

  movc4 (sizeof csb, &csb, size, buff);			// copy result back to caller
}

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

{
  Long mycpuidx, newvalue, sample;

  mycpuidx = oz_hw_cpu_getcur ();
  newvalue = flag ? mycpuidx : -1;

  do sample = exclusive;
  while (((sample >= 0) && (sample != mycpuidx)) || !oz_hw_atomic_setif_long (&exclusive, mycpuidx, sample));
  if (flag) oz_dev_vgavideo_blank (0);
}

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

/* - it displays the string on the current screen, whichever that may be  */
/*   it forces auto-wrap mode so the whole line can be seen               */
/*   it also inserts a carriage return before any line feeds              */
/*   it is used by the low level output routines (like oz_knl_printk)     */

void oz_hw_putcon (uLong size, const char *buff)

{
  char c;
  int hwi, save_auto_wrap;
  uLong done;

  hwi = video_lock (NULL);			/* keep other cpu's out, stay on same screen */
  save_auto_wrap = lvctx -> mode_auto_wrap;	/* save current 'auto wrap' setting */
  lvctx -> mode_auto_wrap = 1;			/* always line-wrap for this stuff */
  for (done = 0; done < size; done ++) {	/* scan through given string */
    c = *(buff ++);
    if (c == '\n') dvctx -> video_cursor = (dvctx -> video_cursor / VIDEOCOLCOUNT) * VIDEOCOLCOUNT;
    video_putchar (c);
  }
  lvctx -> mode_auto_wrap = save_auto_wrap;	/* restore 'auto wrap' setting */
  video_unlock (hwi);				/* let other cpu's in */
  oz_dev_video_updcursor (NULL);		/* update cursor on the screen */
}

/* - it may quietly output to a screen that is not being displayed */
/*   it does not insert a carriage return before line feeds        */
/*   it is used by the class/port driver output routines           */

void oz_dev_video_putstring (OZ_Vctx *vctx, uLong size, const char *buff)

{
  int hwi;
  uLong done;

  hwi = video_lock (vctx);			/* keep other cpu's out and select screen */
  for (done = 0; done < size; done ++) {	/* scan through given string */
    video_putchar (*(buff ++));			/* output a character */
  }
  video_unlock (hwi);				/* let other cpu's in */
  if (done != 0) oz_dev_video_updcursor (vctx);	/* maybe update cursor on the screen */
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

{
  int cursor, hwi;

  hwi = video_lock (vctx);
  if (lvctx == dvctx) {
    cursor = hwscrollindex * VIDEOCOLCOUNT + lvctx -> video_cursor;
    oz_hw486_outb (0x0E, video_03x4);			/* select high-order cursor */
    oz_hw486_outb (cursor >> 8, video_03x5);		/* output high-order cursor */
    oz_hw486_outb (0x0F, video_03x4);			/* select low-order cursor */
    oz_hw486_outb (cursor, video_03x5);			/* output low-order cursor */
  }
  video_unlock (hwi);
}

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
  int hwi;

  hwi = video_lock (vctx);	/* keep other cpu's out, select the screen */
  video_putchar (c);		/* try to output the character */
  video_unlock (hwi);		/* let other cpu's in */
}

/* Same thing, except we're already locked, the character goes to the locked screen               */
/* If the locked screen also happens to be the displayed screen, the char is output to the screen */

static void video_putchar (char c)

{
  int i;

  /* If we are scrolled by keyboard, unscroll us */

  while (lvctx -> video_lineindx != 0) video_scrollup ();

  /* Determine how to output character */

  c &= 127;
  if (c >= ' ') {
    if (c != 127) {
      if (lvctx -> escseqidx < 0) printable (c);	/* if not escaping, output character to screen */
      else {
        if (lvctx -> escseqidx < sizeof lvctx -> escseqbuf) lvctx -> escseqbuf[lvctx->escseqidx++] = c;
        if (lvctx -> escseqbuf[0] != '[') {		/* check for escape sequence terminator */
          if (c >= 48) escseqend ();
        } else {
          if ((c >= 64) && (lvctx -> escseqidx > 1)) escseqend ();
        }
      }
    }
    return;						/* anyway, character was processed */
  }

  lvctx -> escseqidx = -1;				/* any control character aborts escape sequence */

  switch (c) {
    case  8: {						/* backspace */
      if (lvctx -> video_cursor % VIDEOCOLCOUNT != 0) -- (lvctx -> video_cursor);
      break;
    }
    case  9: {						/* tab */
      for (i = lvctx -> video_cursor % VIDEOCOLCOUNT; ++ i < VIDEOCOLCOUNT;) if (lvctx -> tabstops[i]) break;
      i -= lvctx -> video_cursor % VIDEOCOLCOUNT;
      if (lvctx -> mode_insertion) {
        memmove (lvb + lvctx -> video_cursor + i, lvb + lvctx -> video_cursor, VIDEOCOLCOUNT - i - (lvctx -> video_cursor % VIDEOCOLCOUNT));
      }
      while (-- i >= 0) lvb[lvctx->video_cursor++] = flagword + FILLCHAR;
      if (lvctx -> video_cursor % VIDEOCOLCOUNT == 0) lvctx -> video_cursor --;
      break;
    }
    case 10:						/* linefeed */
    case 11:						/* vertical tab */
    case 12: {						/* formfeed */
      index_down (oz_dev_video_mode_newline);
      break;
    }
    case 13: {						/* carriage-return */
      lvctx -> video_cursor = (lvctx -> video_cursor / VIDEOCOLCOUNT) * VIDEOCOLCOUNT;
      break;
    }
    case 27: {						/* escape */
      lvctx -> escseqidx = 0;
      break;
    }
  }
}

/* Output printable character at cursor position and advance cursor */

static void printable (char c)

{
  int i;

  /* If inserting, slide all characters at or after cursor on same line over to the right one position */

  if (lvctx -> mode_insertion) {
    memmove (lvb + lvctx -> video_cursor + 1, 
             lvb + lvctx -> video_cursor, 
             (VIDEOCOLCOUNT - (lvctx -> video_cursor % VIDEOCOLCOUNT) - 1) * 2);
  }

  /* Store the character on the screen and advance cursor one position to the right */

  lvb[lvctx->video_cursor++] = flagword + c;

  /* If cursor went off end of line and auto_wrap, create a new line, else back cursor up one position */

  if (lvctx -> video_cursor % VIDEOCOLCOUNT == 0) {						// see if it went to beginning of next line
    if (!(lvctx -> mode_auto_wrap)) lvctx -> video_cursor --;					// ok, if not autowrapping, leave it at end of the line
    else if (lvctx -> video_cursor == lvctx -> scroll_bottom * VIDEOCOLCOUNT) {			// autowrap, see if went past bottom of scroll region
      lvctx -> video_cursor --;									// at bottom, let index_down routine handle it
      index_down (1);
    }
    else if (lvctx -> video_cursor >= VIDEOPAGESIZE) lvctx -> video_cursor = VIDEOPAGESIZE - 1;	// don't let it go off end of screen
  }
}

/************************************************************************/
/*									*/
/*  Process command in escseqbuf					*/
/*									*/
/*    Input:								*/
/*									*/
/*	escseqbuf = escape sequence characters, not including <esc>	*/
/*	escseqidx = number of characters in escseqbuf, incl terminator	*/
/*									*/
/*    Output:								*/
/*									*/
/*	escseqidx = -1							*/
/*	operating environment modified accoring to escape sequence	*/
/*									*/
/************************************************************************/

static void escseqend (void)

{
  char termchr;

#if 00
  i = lvctx -> escseqidx + 4;
  if (i < 16) i = 16;
  vp = VIDEOSCRL + VIDEOPAGESIZE - i;
  memset (vp, 0, i * 2);
  *(vp ++) = 0;
  *(vp ++) = 0x0400 + '0' + lvctx -> escseqidx;
  for (i = 0; i < lvctx -> escseqidx; i ++) *(vp ++) = 0x2400 + lvctx -> escseqbuf[i];
  while (oz_dev_keyboard_getc (1) != ' ') {}
  *vp = 0x0400 + '.';
#endif

  termchr = lvctx -> escseqbuf[--(lvctx->escseqidx)];	/* get terminator character */
  lvctx -> escseqbuf[lvctx->escseqidx] = 0;		/* replace with terminating null */

  if (lvctx -> escseqbuf[0] != '[') switch (termchr) {

    /* Set keypad to application mode */

    case '=': {
      oz_dev_video_mode_keypad_app = 1;
      break;
    }

    /* Set keypad to numeric mode */

    case '>': {
      oz_dev_video_mode_keypad_app = 0;
      break;
    }

    /* Set lineheight attribute */

    case '3': {
      if (lvctx -> escseqbuf[0] == '#') lvctx -> lineheight[lvctx->video_cursor/VIDEOCOLCOUNT] = 1;
      break;
    }
    case '4': {
      if (lvctx -> escseqbuf[0] == '#') lvctx -> lineheight[lvctx->video_cursor/VIDEOCOLCOUNT] = -1;
      break;
    }

    /* Set linewidth attribute */

    case '5': {
      if (lvctx -> escseqbuf[0] == '#') lvctx -> linewidth[lvctx->video_cursor/VIDEOCOLCOUNT] = 0;
      break;
    }
    case '6': {
      if (lvctx -> escseqbuf[0] == '#') lvctx -> linewidth[lvctx->video_cursor/VIDEOCOLCOUNT] = 1;
      break;
    }

    /* Select character sets */

    case '0': {
      if (lvctx -> escseqbuf[0] == '(') lvctx -> char_set_g0 = lvctx -> char_set_special;
      if (lvctx -> escseqbuf[0] == ')') lvctx -> char_set_g1 = lvctx -> char_set_special;
      break;
    }
    case 'A': {
      if (lvctx -> escseqbuf[0] == '(') lvctx -> char_set_g0 = lvctx -> char_set_uk;
      if (lvctx -> escseqbuf[0] == ')') lvctx -> char_set_g1 = lvctx -> char_set_uk;
      break;
    }
    case 'B': {
      if (lvctx -> escseqbuf[0] == '(') lvctx -> char_set_g0 = lvctx -> char_set_us;
      if (lvctx -> escseqbuf[0] == ')') lvctx -> char_set_g1 = lvctx -> char_set_us;
      break;
    }

    case 'N': {
      lvctx -> single_shift_2 = 1;
      lvctx -> single_shift_3 = 0;
      break;
    }
    case 'O': {
      lvctx -> single_shift_2 = 0;
      lvctx -> single_shift_3 = 1;
      break;
    }

    /* Index down a line (just like linefeed without newline) */

    case 'D': {
      index_down (0);
      break;
    }

    /* Next line (just like linefeed with newline) */

    case 'E': {
      index_down (1);
      break;
    }

    /* Set tab stop */

    case 'H': {
      lvctx -> tabstops[lvctx->video_cursor%VIDEOCOLCOUNT] = 1;
      break;
    }

    /* Reverse index (move cursor up one line & scroll) */

    case 'M': {
      int i;

      if (lvctx -> video_cursor / VIDEOCOLCOUNT == lvctx -> scroll_top) {
        memmove (lvb + (lvctx -> scroll_top + 1) * VIDEOCOLCOUNT, 
                 lvb + lvctx -> scroll_top * VIDEOCOLCOUNT, 
                 (lvctx -> scroll_bottom - lvctx -> scroll_top - 1) * 2 * VIDEOCOLCOUNT);
        for (i = 0; i < VIDEOCOLCOUNT; i ++) lvb[lvctx->scroll_top*VIDEOCOLCOUNT+i] = flagword + FILLCHAR;
      }
      else if (lvctx -> video_cursor / VIDEOCOLCOUNT > 0) lvctx -> video_cursor -= VIDEOCOLCOUNT;
      break;
    }

    /* Z - send device attributes, ie, identify terminal type (same as <esc>[c) */

    case 'Z': {
      if ((lvctx -> escseqbuf[0] == 0) || (lvctx -> escseqbuf[0] == '0')) {
        oz_dev_keyboard_send (lvctx -> keyboard_devexv, 5, "\033[?6c");
      }
      break;
    }

  } else switch (termchr) {

    /* [ A - move cursor up */

    case 'A': {
      int nlines, usedup;

      nlines = oz_hw_atoi (lvctx -> escseqbuf + 1, &usedup);
      if (lvctx -> escseqbuf[1+usedup] == 0) {
        while (lvctx -> video_cursor / VIDEOCOLCOUNT > lvctx -> scroll_top) {
          lvctx -> video_cursor -= VIDEOCOLCOUNT;
          if (-- nlines <= 0) break;
        }
      }
      break;
    }

    /* [ B - move cursor down */

    case 'B': {
      int nlines, usedup;

      nlines = oz_hw_atoi (lvctx -> escseqbuf + 1, &usedup);
      if (lvctx -> escseqbuf[1+usedup] == 0) {
        while (lvctx -> video_cursor / VIDEOCOLCOUNT < lvctx -> scroll_bottom - 1) {
          lvctx -> video_cursor += VIDEOCOLCOUNT;
          if (-- nlines <= 0) break;
        }
      }
      break;
    }

    /* [ C - move cursor to right */

    case 'C': {
      int nchars, usedup;

      nchars = oz_hw_atoi (lvctx -> escseqbuf + 1, &usedup);
      if (lvctx -> escseqbuf[1+usedup] == 0) {
        if (nchars <= 0) nchars = 1;
        lvctx -> video_cursor += nchars;
        if (lvctx -> video_cursor % VIDEOCOLCOUNT < nchars) lvctx -> video_cursor -= (lvctx -> video_cursor % VIDEOCOLCOUNT) + 1;
      }
      break;
    }

    /* [ D - move cursor to left */

    case 'D': {
      int nchars, usedup;

      nchars = oz_hw_atoi (lvctx -> escseqbuf + 1, &usedup);
      if (lvctx -> escseqbuf[1+usedup] == 0) {
        if (nchars <= 0) nchars = 1;
        if (lvctx -> video_cursor % VIDEOCOLCOUNT < nchars) lvctx -> video_cursor -= lvctx -> video_cursor % VIDEOCOLCOUNT;
        else lvctx -> video_cursor -= nchars;
      }
      break;
    }

    /* [ K - erase in line */

    case 'K': {
      int i;
      uWord fword;

      fword = lvctx -> video_color;
      if (lvctx -> mode_reverse_video) fword = (fword << 4) | (fword >> 4);
      fword <<= 8;
      fword  += FILLCHAR;

      switch (lvctx -> escseqbuf[1]) {
        case 0:
        case '0': {
          i = lvctx -> video_cursor;
          do lvb[i++] = fword;
          while (i % VIDEOCOLCOUNT != 0);
          break;
        }
        case '1': {
          i = (lvctx -> video_cursor / VIDEOCOLCOUNT) * VIDEOCOLCOUNT;
          do lvb[i++] = fword;
          while (i <= lvctx -> video_cursor);
          break;
        }
        case '2': {
          i = (lvctx -> video_cursor / VIDEOCOLCOUNT) * VIDEOCOLCOUNT;
          do lvb[i++] = fword;
          while (i % VIDEOCOLCOUNT != 0);
          break;
        }
      }
      break;
    }

    /* [ J - erase in display */

    case 'J': {
      int i;
      uWord fword;

      fword = lvctx -> video_color;
      if (lvctx -> mode_reverse_video) fword = (fword << 4) | (fword >> 4);
      fword <<= 8;
      fword  += FILLCHAR;

      switch (lvctx -> escseqbuf[1]) {
        case 0:
        case '0': {
          for (i = lvctx -> video_cursor; i < VIDEOPAGESIZE; i ++) {
            lvb[i] = fword;
            if (i % VIDEOCOLCOUNT == 0) {
              lvctx -> linewidth[i/VIDEOCOLCOUNT]  = 0;
              lvctx -> lineheight[i/VIDEOCOLCOUNT] = 0;
            }
          }
          break;
        }
        case '1': {
          for (i = 0; i <= lvctx -> video_cursor; i ++) {
            lvb[i] = fword;
            if (i % VIDEOCOLCOUNT == 0) {
              lvctx -> linewidth[i/VIDEOCOLCOUNT]  = 0;
              lvctx -> lineheight[i/VIDEOCOLCOUNT] = 0;
            }
          }
          break;
        }
        case '2': {
          for (i = 0; i < VIDEOPAGESIZE; i ++) {
            lvb[i] = fword;
            if (i % VIDEOCOLCOUNT == 0) {
              lvctx -> linewidth[i/VIDEOCOLCOUNT]  = 0;
              lvctx -> lineheight[i/VIDEOCOLCOUNT] = 0;
            }
          }
          break;
        }
      }
      break;
    }

    /* [ L - insert line */

    case 'L': {
      int cline, i, nlines, usedup;

      nlines = oz_hw_atoi (lvctx -> escseqbuf + 1, &usedup);
      cline  = lvctx -> video_cursor / VIDEOCOLCOUNT;
      if (nlines <= 0) nlines = 1;
      if ((lvctx -> escseqbuf[1+usedup] == 0) && (cline >= lvctx -> scroll_top) && (cline < lvctx -> scroll_bottom)) {
        memmove (lvb + (cline + nlines) * VIDEOCOLCOUNT, 
                 lvb + cline * VIDEOCOLCOUNT, 
                 (lvctx -> scroll_bottom - cline - nlines) * 2 * VIDEOCOLCOUNT);
        memmove (lvctx -> lineheight + cline + nlines, lvctx -> lineheight + cline, (lvctx -> scroll_bottom - cline - nlines) * sizeof lvctx -> lineheight[0]);
        memmove (lvctx -> linewidth  + cline + nlines, lvctx -> linewidth  + cline, (lvctx -> scroll_bottom - cline - nlines) * sizeof lvctx -> linewidth[0]);
        for (i = cline * VIDEOCOLCOUNT; i < (cline + nlines) * VIDEOCOLCOUNT; i ++) {
          lvb[i] = flagword + FILLCHAR;
        }
      }
      break;
    }

    /* [ M - delete line */

    case 'M': {
      int cline, i, nlines, usedup;

      nlines = oz_hw_atoi (lvctx -> escseqbuf + 1, &usedup);
      cline  = lvctx -> video_cursor / VIDEOCOLCOUNT;
      if (nlines <= 0) nlines = 1;
      if ((lvctx -> escseqbuf[1+usedup] == 0) && (cline >= lvctx -> scroll_top) && (cline < lvctx -> scroll_bottom)) {
        memmove (lvb + cline * VIDEOCOLCOUNT, 
                 lvb + (cline + nlines) * VIDEOCOLCOUNT, 
                 (lvctx -> scroll_bottom - cline - nlines) * 2 * VIDEOCOLCOUNT);
        memmove (lvctx -> lineheight + cline, lvctx -> lineheight + cline + nlines, (lvctx -> scroll_bottom - cline - nlines) * sizeof lvctx -> lineheight[0]);
        memmove (lvctx -> linewidth  + cline, lvctx -> linewidth  + cline + nlines, (lvctx -> scroll_bottom - cline - nlines) * sizeof lvctx -> linewidth[0]);
        for (i = (lvctx -> scroll_bottom - nlines) * VIDEOCOLCOUNT; i < lvctx -> scroll_bottom * VIDEOCOLCOUNT; i ++) {
          lvb[i] = flagword + FILLCHAR;
        }
      }
      break;
    }

    /* [ P - delete characters */

    case 'P': {
      int endofline, nch, usedup;

      nch = oz_hw_atoi (lvctx -> escseqbuf + 1, &usedup);
      if ((lvctx -> escseqbuf[1+usedup] == 0) && (nch > 0)) {
        endofline = (lvctx -> video_cursor / VIDEOCOLCOUNT) * VIDEOCOLCOUNT + VIDEOCOLCOUNT - 1;
        memmove (lvb + lvctx -> video_cursor, lvb + lvctx -> video_cursor + nch, (endofline + 1 - nch - lvctx -> video_cursor) * 2);
        while (-- nch >= 0) {
          lvb[endofline-nch] = (lvb[endofline] & 0xFF00) | FILLCHAR;
        }
      }
      break;
    }

    /* [ c - send device attributes, ie, identify terminal type */

    case 'c': {
      if ((lvctx -> escseqbuf[1] == 0) || (lvctx -> escseqbuf[1] == '0')) {
        oz_dev_keyboard_send (lvctx -> keyboard_devexv, 5, "\033[?6c");
      }
      break;
    }

    /* [ H - position cursor */
    /* [ f - position cursor */

    case 'H':
    case 'f': {
      int column, line, usedup, usedup2;

      line   = 1;
      column = 1;
      if (lvctx -> escseqbuf[1] != 0) {
        line = oz_hw_atoi (lvctx -> escseqbuf + 1, &usedup);
        if (lvctx -> escseqbuf[1+usedup] != 0) {
          if (lvctx -> escseqbuf[1+usedup] != ';') break;
          column = oz_hw_atoi (lvctx -> escseqbuf + 2 + usedup, &usedup2);
          if (lvctx -> escseqbuf[2+usedup+usedup2] != 0) break;
        }
      }
      if (line   <= 0) line   = 1;
      if (column <= 0) column = 1;
      position_cursor (line, column);
      break;
    }

    /* [ g - clear tab stops */

    case 'g': {
      switch (lvctx -> escseqbuf[1]) {
        case 0:
        case '0': {
          lvctx -> tabstops[lvctx->video_cursor%VIDEOCOLCOUNT] = 1;
          break;
        }
        case '3': {
          memset (lvctx -> tabstops, 0, sizeof lvctx -> tabstops);
          break;
        }
      }
      break;
    }

    /* [ h - Set mode   */
    /* [ l - Reset mode */

    case 'h':
    case 'l': {
      int code, i, private, set, usedup;

      set = (termchr == 'h');
      i = 1;
      private = (lvctx -> escseqbuf[i] == '?');
      i += private;
      while (lvctx -> escseqbuf[i] != 0) {
        code = oz_hw_atoi (lvctx -> escseqbuf + i, &usedup);
        i += usedup;
        if ((lvctx -> escseqbuf[i] != 0) && (lvctx -> escseqbuf[i++] != ';')) break;
        if (private) switch (code) {
          case 1: {
            oz_dev_video_mode_cursor_key = set;
            break;
          }
          case 3: {
            lvctx -> mode_cols_per_line = set ? 132 : 80;
            lvctx -> scroll_top = 0;
            lvctx -> scroll_bottom = VIDEOROWCOUNT;
            for (i = 0; i < VIDEOPAGESIZE; i ++) lvb[i] = flagword + FILLCHAR;
            break;
          }
          case 5: {
            lvctx -> mode_reverse_video = set;
            break;
          }
          case 6: {
            lvctx -> mode_origin = set;
            position_cursor (1, 1);
            break;
          }
          case 7: {
            lvctx -> mode_auto_wrap = set;
            break;
          }

        } else switch (code) {
          case  4: {
            lvctx -> mode_insertion = set;
            break;
          }
          case 12: {
            oz_dev_video_mode_local_echo = set;
            break;
          }
          case 20: {
            oz_dev_video_mode_newline = set;
            break;
          }
        }
      }
      break;
    }

    /* [ m - Select graphic rendition */

    case 'm': {
      switch (lvctx -> escseqbuf[1]) {
        case 0:
        case '0': {
          lvctx -> mode_char_bold = 0;
          lvctx -> mode_char_underline = 0;
          lvctx -> mode_char_blink = 0;
          lvctx -> mode_char_reverse = 0;
          break;
        }
        case '1': {
          lvctx -> mode_char_bold = 1;
          break;
        }
        case '4': {
          lvctx -> mode_char_underline = 1;
          break;
        }
        case '5': {
          lvctx -> mode_char_blink = 1;
          break;
        }
        case '7': {
          lvctx -> mode_char_reverse = 1;
          break;
        }
      }
      setflagword ();
      break;
    }

    /* [ n - request report */

    case 'n': {
      int code, private;

      private = (lvctx -> escseqbuf[1] == '?');
      code = oz_hw_atoi (lvctx -> escseqbuf + 1 + private, NULL);
      if (private) switch (code) {
        case 15: {					/* printer status report */
          oz_dev_keyboard_send (lvctx -> keyboard_devexv, 6, "\033[?13n"); /* - always report 'not connected' */
          break;
        }
      } else switch (code) {
        case 5: {					/* device status report */
          oz_dev_keyboard_send (lvctx -> keyboard_devexv, 4, "\033[0n"); /* - always report 'Ready' */
          break;
        }
        case 6: {					/* cursor position report */
          char buf[16];
          int column, line;

          column = lvctx -> video_cursor % VIDEOCOLCOUNT;
          line   = lvctx -> video_cursor / VIDEOCOLCOUNT;
          if (lvctx -> mode_origin) line -= lvctx -> scroll_top;
          oz_sys_sprintf (sizeof buf, buf, "\033[%d;%dR", ++ line, ++ column);
          oz_dev_keyboard_send (lvctx -> keyboard_devexv, strlen (buf), buf);
          break;
        }
      }
      break;
    }

    /* [ r - set scrolling region top and bottom margins */

    case 'r': {
      int bottom, top, usedup, usedup2;

      top    = oz_hw_atoi (lvctx -> escseqbuf + 1, &usedup);
      bottom = VIDEOROWCOUNT;
      if (lvctx -> escseqbuf[1+usedup] != 0) {
        if (lvctx -> escseqbuf[1+usedup] != ';') break;
        bottom = oz_hw_atoi (lvctx -> escseqbuf + 2 + usedup, &usedup2);
        if (lvctx -> escseqbuf[2+usedup+usedup2] != 0) break;
      }
      if (top <= 0) top = 1;
      if ((bottom <= 0) || (bottom > VIDEOROWCOUNT)) {
        bottom = VIDEOROWCOUNT;
      }
      if (bottom <= top) break;
      lvctx -> scroll_top = top - 1;
      lvctx -> scroll_bottom = bottom;
      position_cursor (1, 1);
      break;
    }
  }

  lvctx -> escseqidx = -1;				/* arm to receive a new sequence */
}

/************************************************************************/
/*									*/
/*  Move the cursor down one line.  If it goes off bottom of scrolling 	*/
/*  region, scroll the region up one line instead.			*/
/*									*/
/************************************************************************/

static void index_down (int newline)

{
  int i;

  /* Move cursor down one line.  If 'newline' mode, move it to first position on newline */

  lvctx -> video_cursor += VIDEOCOLCOUNT;
  if (newline) lvctx -> video_cursor = (lvctx -> video_cursor / VIDEOCOLCOUNT) * VIDEOCOLCOUNT;

  /* If margins indicate the whole screen, use the scrolling routine that saves the top line in memory for later recall */

  if ((lvctx -> scroll_top == 0) && (lvctx -> scroll_bottom == VIDEOROWCOUNT)) {
    if (lvctx -> video_cursor >= VIDEOPAGESIZE) video_scrollup ();
  }

  /* Otherwise, any line scrolled off is discarded */

  else {
    if ((lvctx -> video_cursor / VIDEOCOLCOUNT) == lvctx -> scroll_bottom) {
      memmove (lvb + lvctx -> scroll_top * VIDEOCOLCOUNT, 
               lvb + (lvctx -> scroll_top + 1) * VIDEOCOLCOUNT, 
               (lvctx -> scroll_bottom - lvctx -> scroll_top - 1) * 2 * VIDEOCOLCOUNT);
      memmove (lvctx -> lineheight + lvctx -> scroll_top, lvctx -> lineheight + lvctx -> scroll_top + 1, (lvctx -> scroll_bottom - lvctx -> scroll_top - 1) * sizeof lvctx -> lineheight[0]);
      memmove (lvctx -> linewidth  + lvctx -> scroll_top, lvctx -> linewidth  + lvctx -> scroll_top + 1, (lvctx -> scroll_bottom - lvctx -> scroll_top - 1) * sizeof lvctx -> linewidth[0]);
      for (i = (lvctx -> scroll_bottom - 1) * VIDEOCOLCOUNT; i < lvctx -> scroll_bottom * VIDEOCOLCOUNT; i ++) {
        lvb[i] = flagword + FILLCHAR;
      }
      lvctx -> video_cursor -= VIDEOCOLCOUNT;
    }
    if (lvctx -> video_cursor >= VIDEOPAGESIZE) lvctx -> video_cursor -= VIDEOCOLCOUNT;
  }
}

/************************************************************************/
/*									*/
/*  Position cursor as indicated					*/
/*									*/
/************************************************************************/

static void position_cursor (int line, int column)

{
  if (line <= 0) line = 1;
  if (column <= 0) column = 1;

  if (column > VIDEOCOLCOUNT) column = VIDEOCOLCOUNT;

  if (lvctx -> mode_origin) {
    line += lvctx -> scroll_top;
    if (line > lvctx -> scroll_bottom) line = lvctx -> scroll_bottom;
  } else {
    if (line > VIDEOROWCOUNT) line = VIDEOROWCOUNT;
  }

  lvctx -> video_cursor = (line - 1) * VIDEOCOLCOUNT + column - 1;
}

/************************************************************************/
/*									*/
/*  Call this routine when user presses 'Page Down' key on the keyboard	*/
/*  This routine will scroll the screen up (making it more 'normal')	*/
/*									*/
/************************************************************************/

void oz_dev_video_linedn (void)

{
  int hwi, scrolled;

  hwi = video_lock (NULL);				/* keep other cpu's out, stay on current screen */
  scrolled = (lvctx -> video_lineindx > 0);
  if (scrolled) video_scrollup ();			/* shift screen contents by one line */
  video_unlock (hwi);					/* let other cpu's in */
  if (scrolled) oz_dev_video_updcursor (NULL);		/* reposition the cursor */
}

/************************************************************************/
/*									*/
/*  Scroll the whole screen up one line					*/
/*									*/
/*    Input:								*/
/*									*/
/*	video_lineindx = # of lines currently scrolled up by 'page up's on the keyboard
/*	video_lineoffs = where to store data for the line we are about to scroll off the top of the screen
/*									*/
/*    Output:								*/
/*									*/
/*	video_lineoffs = incremented by a whole line's worth of data	*/
/*	if video_lineindx was zero, it still is (and a blank line is filled in the bottom of the screen)
/*	                 otherwise, it is decremented (and the line @video_lineoffs is filled in the bottom of the screen)
/*									*/
/************************************************************************/

static void video_scrollup (void)

{
  int i;
  uWord *wp;

  /* Save top line from screen in video_linesave[video_lineoffs++] */

  memcpy (lvctx -> video_linesave + lvctx -> video_lineoffs, lvb, VIDEOCOLCOUNT * 2);
  lvctx -> video_lineoffs += VIDEOCOLCOUNT;
  if (lvctx -> video_lineoffs == VIDEOCOLCOUNT * VIDEOLINESAVE) lvctx -> video_lineoffs = 0;

  /* Move screen data up one line */

  if (lvb == VIDEOSCRL) { hwscrollindex_set (hwscrollindex + 1); lvb = VIDEOSCRL; }
  else memmove (lvb, lvb + VIDEOCOLCOUNT, (VIDEOPAGESIZE - VIDEOCOLCOUNT) * 2);

  /* If we are indexed by keyboard, restore saved line on bottom, else put in blanks */

  if (lvctx -> video_lineindx == 0) {
    wp = lvb + VIDEOPAGESIZE - VIDEOCOLCOUNT;
    for (i = VIDEOCOLCOUNT + 1; -- i > 0;) *(wp ++) = (VIDEOCOLORUNLOCKED << 8) + FILLCHAR;
    lvctx -> linecount ++;		// we just created a brand-spanking-new line, so count it
  } else {
    -- (lvctx -> video_lineindx);
    memcpy (lvb + VIDEOPAGESIZE - VIDEOCOLCOUNT, lvctx -> video_linesave + lvctx -> video_lineoffs, VIDEOCOLCOUNT * 2);
  }

  /* Shift cursor position so it points to the same character as it did on entry */

  lvctx -> video_cursor -= VIDEOCOLCOUNT;
}

/************************************************************************/
/*									*/
/*  Call this routine when user presses 'Page up' key on the keyboard	*/
/*  This routine will scroll the screen down (making it less 'normal')	*/
/*									*/
/************************************************************************/

void oz_dev_video_lineup (void)

{
  int hwi;

  hwi = video_lock (NULL);
  if (lvctx -> video_lineindx < VIDEOLINESAVE) video_scrolldown ();
  video_unlock (hwi);
}

/* Scroll the screen down one line */

static void video_scrolldown (void)

{
  /* Save line on bottom of screen in video_linesave[video_lineoffs] */

  memcpy (lvctx -> video_linesave + lvctx -> video_lineoffs, lvb + VIDEOPAGESIZE - VIDEOCOLCOUNT, VIDEOCOLCOUNT * 2);

  /* Shift screen contents down one line */

  memmove (lvb + VIDEOCOLCOUNT, lvb, (VIDEOPAGESIZE - VIDEOCOLCOUNT) * 2);

  /* Restore top screen line from video_linesave[--video_lineoffs] */

  lvctx -> video_lineoffs -= VIDEOCOLCOUNT;
  if (lvctx -> video_lineoffs < 0) lvctx -> video_lineoffs += VIDEOLINESAVE * VIDEOCOLCOUNT;
  memcpy (lvb, lvctx -> video_linesave + lvctx -> video_lineoffs, VIDEOCOLCOUNT * 2);

  /* The screen has been shifted down one more line */

  lvctx -> video_lineindx ++;

  /* Shift cursor position so it points to the same character as it did on entry */

  lvctx -> video_cursor += VIDEOCOLCOUNT;
}

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

{
  uByte al;

  oz_hw486_outb (1, CRTSEQ + 0);
  al = oz_hw486_inb (CRTSEQ + 1);
  al &= ~ 0x20;
  if (blank) al |= 0x20;
  oz_hw486_outb (al, CRTSEQ + 1);
}

/************************************************************************/
/*									*/
/*  Set hardware scrolling index					*/
/*									*/
/*    Input:								*/
/*									*/
/*	scix = number of lines to skip in video memory			*/
/*									*/
/*    Output:								*/
/*									*/
/*	hwscrollindex = updated with new scroll index			*/
/*	video registers set to corresponding value			*/
/*									*/
/************************************************************************/

static void hwscrollindex_set (int scix)

{
  hwscrollindex = scix;					// set new line index
  if (scix >= VIDEOROWCOUNT) {				// see if we've overflowed memory
    memcpy (VIDEOBASE, VIDEOSCRL, VIDEOPAGESIZE * 2);	// if so, copy the page to bottom of memory
    hwscrollindex = scix = 0;				// reset indicies
  }

  scix *= VIDEOCOLCOUNT;				// get word offset for scan memory

  oz_hw486_outb (0x0C, video_03x4);			// select high-order offset
  oz_hw486_outb (scix >> 8, video_03x5);			// output high-order offset
  oz_hw486_outb (0x0D, video_03x4);			// select low-order offset
  oz_hw486_outb (scix, video_03x5);			// output low-order offset
}

/************************************************************************/
/*									*/
/*  Make sure only one cpu is writing our data and the screen at a time	*/
/*  Also set up the color used for new characters based on the current cpu
/*  Note: these routines are not nestable				*/
/*									*/
/*    Input:								*/
/*									*/
/*	vctx  = video screen context being selected			*/
/*	        NULL: use whatever is currently being displayed (dvctx)	*/
/*	dvctx = video screen that is currently being displayed		*/
/*	video_lockflag = -1 : no one is using display database		*/
/*	               else : someone is using display database		*/
/*									*/
/*    Output:								*/
/*									*/
/*	video_lockflag = set						*/
/*	hardware interrupts inhibited					*/
/*	lvctx    = locked video context					*/
/*	lvb      = locked video buffer pointer				*/
/*	flagword = set up to current value				*/
/*	lvctx -> video_color = set to current cpu's color		*/
/*									*/
/************************************************************************/

static const char nestedmsg[] = { "Nested video lock attempt" };

static int video_lock (OZ_Vctx *vctx)

{
  int hwi;
  Long mycpuid;

  hwi = oz_hw_cpu_sethwints (-1);			/* inhibit hardware interrupts, including non-maskables */
  mycpuid = oz_hw_cpu_getcur ();
  while (exclusive >= 0) {				/* see if someone else has claimed exclusive use */
    if (mycpuid == exclusive) break;
  }

  if (video_lockflag == mycpuid) {			/* don't allow nested locking */
    flagword = (colortable[mycpuid%sizeof colortable] << 12) | 0x8000;
    for (hwi = 0; nestedmsg[hwi] != 0; hwi ++) {
      VIDEOSCRL[hwi] = flagword | nestedmsg[hwi];
    }
    while (1) {}
  }

  while (!oz_hw_atomic_setif_long (&video_lockflag, mycpuid, -1)) {} /* wait for other cpu to finish with it */

  if (vctx == NULL) vctx = dvctx;			/* maybe they want whatever is currently on display */
  lvctx = vctx;						/* select the 'locked' context block */

  if (lvctx != dvctx) lvb = lvctx -> video_pagesave;	/* if not live screen, update page save buffer */
  else {
    lvb = VIDEOSCRL;					/* live screen, update the live screen */
    if (!oz_s_inloader) VIDEOSCRL[VIDEOCOLCOUNT-1] = savevidxword; /* restore character in upper left corner */
#ifdef OZ_DEBUG
    flip_status_display ();				/* restore status char display area */
#endif
  }

  lvctx -> video_color = getcpucolor (oz_hw_cpu_getcur ()); /* assume monochrome mode */
  setflagword ();					/* set flagword for current color, etc */
  return (hwi);						/* return whether or not hw ints were enabled */
}

static void video_unlock (int hwi)

{
  lvctx -> video_color = VIDEOCOLORUNLOCKED;		/* set up the 'unlocked' color */
  flagword = VIDEOCOLORUNLOCKED << 8;			/* set flag word for unlocked color */
  if (lvctx == dvctx) {
    if (!oz_s_inloader) {				/* see if we were doing the live screen */
      savevidxword = VIDEOSCRL[VIDEOCOLCOUNT-1];	/* if so, save character in upper left corner */
      VIDEOSCRL[VIDEOCOLCOUNT-1] = lvctx -> vidx + '0' + (VIDEOCOLORUNLOCKED << 8); /* replace with screen number */
    }
#ifdef OZ_DEBUG
    flip_status_display ();				/* restore status char display area */
#endif
  }
  lvctx = NULL;						/* nothing is locked now */
  lvb   = NULL;
  OZ_HW_MB;						/* make sure all updates before the unlock happen first */
  video_lockflag = -1;					/* let other cpu's go */
  oz_hw_cpu_sethwints (hwi);				/* restore hardware interrupts */
}

/************************************************************************/
/*									*/
/*  These routines update the status values on the screen for the 	*/
/*  calling cpu								*/
/*									*/
/************************************************************************/

static const char hextabl[]  = "0123456789ABCDEF";

/* Update the values for the calling cpu */

void oz_dev_video_statusupdate (Long cpuidx, uByte cpulevel, uByte tskpri, OZ_Pointer eip)

{
#ifdef OZ_DEBUG
  int haditlocked, hwi;
  uWord *spot, fword;

  if (((uLong)cpuidx) >= STATUS_MAXCPUS) return;			/* make sure this is in range */

  fword   = getcpucolor (cpuidx);					/* figure out what color to use */
  fword <<= 8;

  hwi = oz_hw_cpu_sethwints (-1);					/* inhibit hardware interrupts, including non-maskables */
  haditlocked = (video_lockflag == cpuidx);				/* if another cpu has it locked, wait for it */
  if (!haditlocked) while (!oz_hw_atomic_setif_long (&video_lockflag, cpuidx, -1)) {} /* ... and lock it */

  spot = VIDEOSCRL + VIDEOCOLCOUNT * (STATUS_STARTROW + cpuidx) + STATUS_STARTCOL; /* this points to the spot in video memory */

#if STATUS_COLUMNS < 13
  error : code assumes STATUS COLUMNS >= 13
#endif
  statusbyte (eip >> 24, spot +  0, fword);
  statusbyte (eip >> 16, spot +  2, fword);
  statusbyte (eip >>  8, spot +  4, fword);
  statusbyte (eip,       spot +  6, fword);
                          spot[8] = fword + FILLCHAR;
  statusbyte (cpulevel,  spot +  9, fword);
  statusbyte (tskpri,    spot + 11, fword);

  if (!haditlocked) {
    OZ_HW_MB;								/* make sure all updates before the unlock happen first */
    video_lockflag = -1;						/* let other cpu's go */
    oz_hw_cpu_sethwints (hwi);						/* restore hardware interrupts */
  }
#endif
}

#ifdef OZ_DEBUG
static void statusbyte (uByte ub, uWord *vid, uWord fword)

{
  vid[0] = hextabl[ub>>4] | fword;
  vid[1] = hextabl[ub&15] | fword;
}

/* Internal routine to flip the characters to/from normal while updating regular video data */

static void flip_status_display (void)

{
  int col, i, row;
  uWord x;

  i = 0;
  for (row = 1; row < 1 + STATUS_MAXCPUS; row ++) {
    for (col = STATUS_STARTCOL; col < STATUS_STARTCOL+STATUS_COLUMNS; col ++) {
      x = status_data[i];
      status_data[i++] = VIDEOSCRL[VIDEOCOLCOUNT*row+col];
      VIDEOSCRL[VIDEOCOLCOUNT*row+col] = x;
    }
  }
}
#endif

/************************************************************************/
/*									*/
/*  Get the color code to use when displaying data from this cpu	*/
/*									*/
/************************************************************************/

static uByte getcpucolor (Long cpuidx)

{
  if (oz_s_loadparams.monochrome) return (VIDEOCOLORMONO);	// if monochrome mode, return 'white'
  if (!oz_hw486_apicmapped) return (VIDEOCOLORUNIPROC);		// if uniprocessor mode, return 'yellow'
  return (colortable[cpuidx%(sizeof colortable)]);		// otherwise, return color for the cpu
}

/************************************************************************/
/*									*/
/*  Update the 'flagword' from the currently locked video attributes	*/
/*									*/
/*    Input:								*/
/*									*/
/*	lvctx -> video_color						*/
/*	lvctx -> mode_char_reverse					*/
/*	lvctx -> mode_reverse_video					*/
/*	lvctx -> mode_char_bold						*/
/*									*/
/*    Output:								*/
/*									*/
/*	flagword = what goes in upper half of video words		*/
/*									*/
/************************************************************************/

static void setflagword (void)

{
  uByte flagbyte;

  flagbyte = lvctx -> video_color;				/* get color normal video */
  if (lvctx -> mode_char_reverse ^ lvctx -> mode_reverse_video) { /* maybe reverse video it */
    flagbyte = (flagbyte << 4) | (flagbyte >> 4);
  }
  if (lvctx -> mode_char_bold) flagbyte |= 0x08;		/* maybe brighten it */
  flagword = ((uWord)flagbyte) << 8;				/* save for upper order word */
}

/********************************************************************/
/* Display a string in reverse video in upper left corner of screen */
/* Then wait for the space bar (debugging use only)                 */
/********************************************************************/

#if 00
static void charstring (char *ch, uLong hex)

{
  char c;
  int i, j;

  for (i = 0; (c = ch[i]) != 0; i ++) {
    VIDEOSCRL[i] = 0x7000 + c;
  }
  for (j = 8; -- j >= 0;) {
    VIDEOSCRL[i+j] = 0x7000 + "0123456789ABCDEF"[hex&15];
    hex >>= 4;
  }
  while (oz_dev_keyboard_getc (1) != ' ') {}
  i += 8;
  while (i > 0) VIDEOSCRL[--i] = 0;
}
#endif
