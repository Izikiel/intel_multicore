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
/*  Write bootblock							*/
/*									*/
/************************************************************************/

#include <stdarg.h>

#include "ozone.h"
#include "oz_io_fs.h"
#include "oz_knl_hw.h"
#include "oz_knl_sdata.h"
#include "oz_knl_status.h"
#include "oz_ldr_params.h"
#include "oz_sys_dateconv.h"
#include "oz_sys_io.h"
#include "oz_sys_io_fs.h"
#include "oz_sys_io_fs_printf.h"
#include "oz_util_start.h"

static char *pn = "writeboot";

static uLong getparams (OZ_Procmode cprocmode, void *dummy);
static void printparam (const char *format, ...);

uLong oz_util_main (int argc, char *argv[])

{
  char *filename;
  int i;
  OZ_Datebin now;
  OZ_Handle h_file;
  OZ_IO_fs_open fs_open;
  OZ_IO_fs_writeblocks fs_writeblocks;
  uLong sts;
  volatile int zero = 0;

  if (argc > 0) pn = argv[0];

  /* Get current parameters from kernel */

  sts = oz_sys_callknl (getparams, NULL);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u getting current settings from kernel\n", pn, sts);
    return (sts);
  }

  /* Process modifications from command line */

  filename = NULL;
  for (i = 1; i < argc; i ++) {
    if (strcasecmp (argv[i], "-set") == 0) {
      if (i + 2 >= argc) goto usage;
      if (!oz_ldr_set (printparam, argv[i+1], argv[i+2])) return (OZ_BADPARAM);
      i += 2;
      continue;
    }
    if (strcasecmp (argv[i], "-extra") == 0) {
      if (i + 2 >= argc) goto usage;
      if (!oz_ldr_extra (printparam, argv[i+1], argv[i+2])) return (OZ_BADPARAM);
      i += 2;
      continue;
    }
    if (argv[i][0] == '-') goto usage;
    if (filename != NULL) goto usage;
    filename = argv[i];
  }

  /* If we are writing a new block, set the signature */

  if (filename != NULL) {
    now = oz_hw_tod_getnow ();
    memset (oz_ldr_paramblock.signature, 0, sizeof oz_ldr_paramblock.signature);
    oz_sys_datebin_decstr (0, now, sizeof oz_ldr_paramblock.signature, oz_ldr_paramblock.signature);
  }

  /* Display the resulting parameters we are about to write */

  oz_ldr_show (printparam, &zero);
  oz_ldr_extras (printparam, &zero);

  /* If no filename supplied, we're all done */

  if (filename == NULL) return (OZ_SUCCESS);

  /* Open the file that contains the loader boot image */

  memset (&fs_open, 0, sizeof fs_open);
  fs_open.name = filename;
  fs_open.lockmode = OZ_LOCKMODE_PW;
  sts = oz_sys_io_fs_open (sizeof fs_open, &fs_open, 0, &h_file);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u opening file %s\n", pn, sts, filename);
    return (sts);
  }

  /* Write system parameters to loader boot file */

  memset (&fs_writeblocks, 0, sizeof fs_writeblocks);
  fs_writeblocks.size = sizeof oz_ldr_paramblock;
  fs_writeblocks.buff = &oz_ldr_paramblock;
  fs_writeblocks.svbn = OZ_LDR_PARAMS_VBN;
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_file, 0, OZ_IO_FS_WRITEBLOCKS, sizeof fs_writeblocks, &fs_writeblocks);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u writing system params to boot file\n", pn, sts);
    return (sts);
  }

  /* Write bootblock image to disk */

  sts = oz_sys_io (OZ_PROCMODE_KNL, h_file, 0, OZ_IO_FS_WRITEBOOT, 0, NULL);
  if (sts == OZ_SUCCESS) oz_sys_io_fs_printf (oz_util_h_error, "%s: bootblock written %s\n", pn, filename);
  else oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u writing bootblock %s\n", pn, sts, filename);

  return (sts);

usage:
  oz_sys_io_fs_printf (oz_util_h_error, "%s: usage: %s [-set <param> <value>] [-extra <param> <value>] [<filename>]\n", pn, pn);
  return (OZ_BADPARAM);
}

static uLong getparams (OZ_Procmode cprocmode, void *dummy)

{
  oz_ldr_paramblock = oz_s_loadparams;
  return (OZ_SUCCESS);
}

static void printparam (const char *format, ...)

{
  va_list ap;

  va_start (ap, format);
  oz_sys_io_fs_vprintf (oz_util_h_output, format, ap);
  va_end (ap);
}
