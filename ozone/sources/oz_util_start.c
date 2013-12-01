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
/*  This routine is the start program for a C program			*/
/*  It opens the input, output and error streams from OZ_INPUT, etc.	*/
/*  It sets up argc/argv from the OZ_PARAMS logical			*/
/*									*/
/*  It calls a subroutine called 'main' with the oz_h_input, _error, 	*/
/*  _output, _console handles set up.  It will also start the debugger 	*/
/*  if an OZ_DEBUG logical is defined with an odd value.		*/
/*									*/
/************************************************************************/

#include "ozone.h"

#include "oz_crtl_malloc.h"
#include "oz_io_fs.h"
#include "oz_knl_hw.h"
#include "oz_knl_lock.h"
#include "oz_knl_logname.h"
#include "oz_knl_sdata.h"
#include "oz_knl_status.h"
#include "oz_sys_handle.h"
#include "oz_sys_logname.h"
#include "oz_sys_io_fs.h"
#include "oz_sys_io_fs_printf.h"
#include "oz_usr_debug.h"
#include "oz_usr_profile.h"
#include "oz_util_start.h"

typedef struct { uLong argc;
                 char **argv;
               } Mp;

OZ_Handle oz_util_h_console, oz_util_h_error, oz_util_h_input, oz_util_h_output;

static uLong me (void *mpv);

int _start ()

{
  char debugb[4];
  Mp mp;
  OZ_Handle h_logname, h_table;
  OZ_IO_fs_create fs_create;
  OZ_IO_fs_open fs_open;
  uLong argi, debugc, rlen, sts;

  /* Open input, output and error streams */

  memset (&fs_open, 0, sizeof fs_open);
  fs_open.lockmode = OZ_LOCKMODE_CR;
  memset (&fs_create, 0, sizeof fs_create);
  fs_create.lockmode = OZ_LOCKMODE_CW;

  fs_create.name = "OZ_ERROR";
  sts = oz_sys_io_fs_create (sizeof fs_create, &fs_create, 0, &oz_util_h_error);
  if (sts != OZ_SUCCESS) return (sts);

  fs_create.name = "OZ_OUTPUT";
  sts = oz_sys_io_fs_create (sizeof fs_create, &fs_create, 0, &oz_util_h_output);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "oz_util_start: error %u creating OZ_OUTPUT\n", sts);
    return (sts);
  }

  fs_open.name = "OZ_INPUT";
  sts = oz_sys_io_fs_open (sizeof fs_open, &fs_open, 0, &oz_util_h_input);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "oz_util_start: error %u opening OZ_INPUT\n", sts);
    return (sts);
  }

  /* Get console I/O channel */

  oz_util_h_console = 0;

  sts = oz_sys_logname_lookup (0, OZ_PROCMODE_USR, oz_s_logname_defaulttables, NULL, NULL, NULL, &h_table);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "oz_util_start: error %u looking up default tables (%s)\n", sts, oz_s_logname_defaulttables);
    return (sts);
  }

  sts = oz_sys_logname_lookup (h_table, OZ_PROCMODE_USR, "OZ_CONSOLE", NULL, NULL, &rlen, &h_logname);
  if (sts != OZ_NOLOGNAME) {
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "oz_util_start: error %u looking up OZ_CONSOLE\n", sts);
      return (sts);
    }

    sts = oz_sys_logname_getval (h_logname, 0, NULL, 0, NULL, &rlen, &oz_util_h_console, OZ_OBJTYPE_IOCHAN, NULL);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "oz_util_start: error %u getting OZ_CONSOLE\n", sts);
      return (sts);
    }

    oz_sys_handle_release (OZ_PROCMODE_KNL, h_logname);
  }

  /* Put logical OZ_PARAMS in mp.argv array */

  sts = oz_sys_logname_lookup (h_table, OZ_PROCMODE_USR, "OZ_PARAMS", NULL, NULL, &mp.argc, &h_logname);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "oz_util_start: error %u looking up OZ_PARAMS\n", sts);
    return (sts);
  }

  mp.argv = malloc ((mp.argc + 1) * sizeof mp.argv[0]);

  for (argi = 0; argi < mp.argc; argi ++) {
    sts = oz_sys_logname_getval (h_logname, argi, NULL, 0, NULL, &rlen, NULL, 0, NULL);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "oz_util_start: error %u getting OZ_PARAMS[%u] string\n", sts, argi);
      return (sts);
    }
    mp.argv[argi] = malloc (rlen + 1);
    sts = oz_sys_logname_getval (h_logname, argi, NULL, rlen + 1, mp.argv[argi], NULL, NULL, 0, NULL);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "oz_util_start: error %u getting OZ_PARAMS[%u] string\n", sts, argi);
      return (sts);
    }
  }

  oz_sys_handle_release (OZ_PROCMODE_KNL, h_logname);

  /* Get OZ_DEBUG to see if the debugger should be used */

  debugb[0] = 0;

  sts = oz_sys_logname_lookup (h_table, OZ_PROCMODE_USR, "OZ_DEBUG", NULL, NULL, &debugc, &h_logname);
  if ((sts != OZ_SUCCESS) && (sts != OZ_NOLOGNAME)) {
    oz_sys_io_fs_printf (oz_util_h_error, "oz_util_start: error %u looking up OZ_DEBUG\n", sts);
    return (sts);
  }

  if (sts == OZ_SUCCESS) {
    if (debugc > 0) {
      sts = oz_sys_logname_getval (h_logname, 0, NULL, sizeof debugb, debugb, NULL, NULL, 0, NULL);
      if (sts != OZ_SUCCESS) {
        oz_sys_io_fs_printf (oz_util_h_error, "oz_util_start: error %u getting OZ_DEBUG string\n", sts);
        return (sts);
      }
    }
    oz_sys_handle_release (OZ_PROCMODE_KNL, h_logname);
  }

  oz_sys_handle_release (OZ_PROCMODE_KNL, h_table);

  /* Maybe start profiler */

#if !defined (OZ_HW_TYPE_AXP)
  oz_usr_profile_start (mp.argc, mp.argv);
#endif

  /* Call the main program */

  if (debugb[0] & 1) {
    sts = oz_usr_debug_init (me, &mp);
  } else {
    sts = oz_util_main (mp.argc, mp.argv);
  }

  return (sts);
}

static uLong me (void *mpv)

{
  uLong sts;
  Mp *mp;

  mp = mpv;
  sts = oz_util_main (mp -> argc, mp -> argv);
  return (sts);
}
