//+++2003-03-01
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
//---2003-03-01

/************************************************************************/
/*									*/
/*  CONSOLE GETSCREEN command						*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_io_console.h"
#include "oz_knl_status.h"
#include "oz_sys_io.h"
#include "oz_sys_xprintf.h"
#include "oz_util_start.h"

void *malloc (int size);

static char *pn = "console getscreen";

static void outputescape (const char *escapestring);

uLong oz_util_main (int argc, char *argv[])

{
  char *consolename, *linebuff;
  int enabled, i, tailf;
  OZ_IO_console_ctrlchar console_ctrlchar;
  OZ_Console_screenbuff console_screenbuff;
  OZ_IO_console_getscreen console_getscreen;
  OZ_Handle h_console;
  uLong col, row, seq, sts;
  volatile uLong ctrlcsts;

  if (argc > 0) pn = argv[0];

  consolename = NULL;
  tailf       = 0;
  for (i = 1; i < argc; i ++) {
    if (argv[i][0] == '-') {
      if (strcasecmp (argv[i], "-tailf") == 0) {
        tailf = 1;
        continue;
      }
      goto usage;
    }
    if (consolename != NULL) goto usage;
    consolename = argv[i];
  }
  if (consolename == NULL) goto usage;

  /* Assign channel to console to be monitored */

  sts = oz_sys_io_assign (OZ_PROCMODE_KNL, &h_console, consolename, OZ_LOCKMODE_NL);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u assigning channel to %s\n", pn, sts, consolename);
    return (sts);
  }

  /* Get the console's screen and buffer sizes */

  memset (&console_getscreen, 0, sizeof console_getscreen);
  console_getscreen.size = sizeof console_screenbuff;
  console_getscreen.buff = &console_screenbuff;

  memset (&console_screenbuff, 0, sizeof console_screenbuff);

  sts = oz_sys_io (OZ_PROCMODE_KNL, h_console, 0, OZ_IO_CONSOLE_GETSCREEN, sizeof console_getscreen, &console_getscreen);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u getting screen parameters\n", pn, sts);
    return (sts);
  }

  /* Set up buffers for processing */

  console_screenbuff.size = console_screenbuff.nrowstot * console_screenbuff.ncols * sizeof *console_screenbuff.buff;
  console_screenbuff.buff = malloc (console_screenbuff.size);

  linebuff = malloc (console_screenbuff.ncols + 1);

  /* If -tailf, set up control-C to exit */

  memset (&console_ctrlchar, 0, sizeof console_ctrlchar);
  console_ctrlchar.mask[0]  = 8;
  console_ctrlchar.terminal = 1;
  ctrlcsts = OZ_PENDING;
  if (tailf && (oz_util_h_console != 0)) {
    sts = oz_sys_io_start (OZ_PROCMODE_KNL, oz_util_h_console, &ctrlcsts, 0, NULL, NULL, 
                           OZ_IO_CONSOLE_CTRLCHAR, sizeof console_ctrlchar, &console_ctrlchar);
    if (sts != OZ_STARTED) oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u enabling control-C exit\n", pn, sts);
  }

  /* Get and display data */

  seq = 0;									// haven't scanned anything yet
  enabled = 0;									// skip all blank lines at the top

  while (ctrlcsts == OZ_PENDING) {
    sts = oz_sys_io (OZ_PROCMODE_KNL, h_console, 0, OZ_IO_CONSOLE_GETSCREEN, sizeof console_getscreen, &console_getscreen);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u getting screen contents\n", pn, sts);
      return (sts);
    }

    row = 0;									// assume we display all lines given
    if (console_screenbuff.nrowscur - tailf > (console_screenbuff.lastrowseq - seq)) {	// skip those we showed last time through
      row = console_screenbuff.nrowscur - tailf - (console_screenbuff.lastrowseq - seq);
    }

    for (; row < console_screenbuff.nrowscur - tailf; row ++) {			// loop through the lines to be displayed
										// don't do very bottom row in tailf mode
										// ... because it is not done yet
      for (col = 0; col < console_screenbuff.ncols; col ++) {			// convert the word-oriented array to bytes
        linebuff[col] = console_screenbuff.buff[row*console_screenbuff.ncols+col];
        if (linebuff[col] < ' ') linebuff[col] = ' ';
      }
      while ((col > 0) && (linebuff[col-1] == ' ')) col --;
      linebuff[col] = 0;
      if (col != 0) enabled = 1;						// found a non-blank line, enable output
      if (enabled) {
        if (!tailf && (row == console_screenbuff.rowtop)) outputescape ("[1m"); // turn bold on for part on screen
        oz_sys_io_fs_printf (oz_util_h_output, "%s\n", linebuff);
        if (!tailf && (row == console_screenbuff.rowbot)) outputescape ("[m"); // turn bold off 
      }
    }
    if (!tailf) break;
    seq = console_screenbuff.lastrowseq;					// remember what we've done so far
    sleep (1);									// wait a second
  }

  return (OZ_SUCCESS);

usage:
  oz_sys_io_fs_printf (oz_util_h_error, "usage: %s [-tailf] <console_device_name>\n", pn);
  return (OZ_MISSINGPARAM);
}

static void outputescape (const char *escapestring)

{
  OZ_IO_console_putdat console_putdat;

  memset (&console_putdat, 0, sizeof console_putdat);
  console_putdat.size = strlen (escapestring);
  console_putdat.buff = escapestring;
  oz_sys_io (OZ_PROCMODE_KNL, oz_util_h_output, 0, OZ_IO_CONSOLE_PUTDAT, sizeof console_putdat, &console_putdat);
}
