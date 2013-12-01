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
/*  CAT utiliti								*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_crtl_malloc.h"
#include "oz_io_disk.h"
#include "oz_io_fs.h"
#include "oz_knl_lock.h"
#include "oz_knl_status.h"
#include "oz_sys_io.h"
#include "oz_sys_io_fs.h"
#include "oz_sys_io_fs_printf.h"
#include "oz_util_start.h"

static char *pn = "cat";

uLong oz_util_main (int argc, char *argv[])

{
  char buff[4096], *input_file, *output_file;
  int i;
  OZ_Handle h_in, h_out;
  OZ_IO_fs_create fs_create;
  OZ_IO_fs_open fs_open;
  OZ_IO_fs_readrec fs_readrec;
  OZ_IO_fs_writerec fs_writerec;
  uLong sts, stsw;

  if (argc > 0) pn = argv[0];

  /* If 'cat ... > file', use 'file' as the output file name, else OZ_OUTPUT */

  if ((argc > 2) && (strcmp (argv[argc-2], ">") == 0)) {
    output_file = argv[argc-1];
    argc -= 2;
    memset (&fs_create, 0, sizeof fs_create);
    fs_create.name = output_file;
    fs_create.lockmode = OZ_LOCKMODE_PW;
    sts = oz_sys_io_fs_create (sizeof fs_create, &fs_create, 0, &h_out);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u creating output file %s\n", pn, sts, output_file);
      return (sts);
    }
  } else {
    output_file = "OZ_OUTPUT";
    h_out = oz_util_h_output;
  }

  /* Set up i/o parameter blocks */

  memset (&fs_open, 0, sizeof fs_open);
  fs_open.lockmode = OZ_LOCKMODE_CR;

  memset (&fs_readrec,  0, sizeof fs_readrec);
  memset (&fs_writerec, 0, sizeof fs_writerec);
  fs_readrec.size  = sizeof buff;
  fs_readrec.buff  = buff;
  fs_readrec.rlen  = &fs_writerec.size;
  fs_writerec.buff = buff;

  /* Loop through input filenames */

  for (i = 1; i < argc; i ++) {

    /* Open input file */

    fs_open.name = argv[i];
    sts = oz_sys_io_fs_open (sizeof fs_open, &fs_open, 0, &h_in);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u opening input file %s\n", pn, sts, argv[i]);
      return (sts);
    }

    /* Copy onto end of output file */

    while (((sts = oz_sys_io (OZ_PROCMODE_KNL, h_in, 0, OZ_IO_FS_READREC, sizeof fs_readrec, &fs_readrec)) == OZ_SUCCESS) || (sts == OZ_ENDOFFILE)) {
      if (fs_writerec.size == 0) break;
      stsw = oz_sys_io (OZ_PROCMODE_KNL, h_out, 0, OZ_IO_FS_WRITEREC, sizeof fs_writerec, &fs_writerec);
      if (stsw != OZ_SUCCESS) {
        oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u writing %s\n", pn, stsw, output_file);
        return (stsw);
      }
      if (sts == OZ_ENDOFFILE) break;
    }

    /* Check read status and close input file */

    if (sts != OZ_ENDOFFILE) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u reading %s\n", pn, sts, argv[i]);
      return (sts);
    }
    oz_sys_handle_release (OZ_PROCMODE_KNL, h_in);
  }

  /* Close output file and return final status */

  sts = OZ_SUCCESS;
  if (h_out != oz_util_h_output) {
    sts = oz_sys_io (OZ_PROCMODE_KNL, h_out, 0, OZ_IO_FS_CLOSE, 0, NULL);
    if (sts != OZ_SUCCESS) oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u closing %s\n", pn, sts, output_file);
  }

  return (sts);
}
