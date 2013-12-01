//+++2002-08-17
//    Copyright (C) 2001,2002  Mike Rieker, Beverly, MA USA
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
//---2002-08-17

/************************************************************************/
/*									*/
/*  TAIL -F utiliti							*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_io_console.h"
#include "oz_io_fs.h"
#include "oz_knl_lock.h"
#include "oz_knl_status.h"
#include "oz_sys_io.h"
#include "oz_sys_io_fs.h"
#include "oz_sys_io_fs_printf.h"
#include "oz_util_start.h"

static char *pn = "tailf";

uLong oz_util_main (int argc, char *argv[])

{
  char buff[4096], *input_file;
  OZ_Handle h_in;
  OZ_IO_console_ctrlchar console_ctrlchar;
  OZ_IO_fs_open fs_open;
  OZ_IO_fs_readrec fs_readrec;
  OZ_IO_fs_writerec fs_writerec;
  uLong sts, stsw;
  volatile uLong ctrlcsts;

  if (argc > 0) pn = argv[0];
  if (argc != 2) goto usage;

  /* Open input file */

  memset (&fs_open, 0, sizeof fs_open);
  fs_open.name = argv[1];
  fs_open.lockmode = OZ_LOCKMODE_CR;
  sts = oz_sys_io_fs_open (sizeof fs_open, &fs_open, 0, &h_in);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u opening input file %s\n", pn, sts, argv[1]);
    return (sts);
  }

  /* Set up control-C to exit */

  memset (&console_ctrlchar, 0, sizeof console_ctrlchar);
  console_ctrlchar.mask[0]  = 8;
  console_ctrlchar.terminal = 1;
  ctrlcsts = OZ_PENDING;
  if (oz_util_h_console != 0) {
    sts = oz_sys_io_start (OZ_PROCMODE_KNL, oz_util_h_console, &ctrlcsts, 0, NULL, NULL, 
                           OZ_IO_CONSOLE_CTRLCHAR, sizeof console_ctrlchar, &console_ctrlchar);
    if (sts != OZ_STARTED) oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u enabling control-C exit\n", pn, sts);
  }

  /* Read it and write to output repeatedly */

  memset (&fs_readrec,  0, sizeof fs_readrec);
  memset (&fs_writerec, 0, sizeof fs_writerec);
  fs_readrec.size  = sizeof buff;
  fs_readrec.buff  = buff;
  fs_readrec.rlen  = &fs_writerec.size;
  fs_writerec.buff = buff;

  while ((sts = ctrlcsts) == OZ_PENDING) {
    while (((sts = oz_sys_io (OZ_PROCMODE_KNL, h_in, 0, OZ_IO_FS_READREC, sizeof fs_readrec, &fs_readrec)) == OZ_SUCCESS) || (sts == OZ_ENDOFFILE)) {
      if (fs_writerec.size == 0) break;
      stsw = oz_sys_io (OZ_PROCMODE_KNL, oz_util_h_output, 0, OZ_IO_FS_WRITEREC, sizeof fs_writerec, &fs_writerec);
      if (stsw != OZ_SUCCESS) {
        oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u writing OZ_OUTPUT\n", pn, stsw);
        return (stsw);
      }
      if (sts == OZ_ENDOFFILE) break;
    }
    if (sts != OZ_ENDOFFILE) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u reading %s\n", pn, sts, argv[1]);
      return (sts);
    }
    sleep (1);
  }
  return (sts);

usage:
  oz_sys_io_fs_printf (oz_util_h_error, "usage: %s <filename>\n", pn);
  return (OZ_MISSINGPARAM);
}
