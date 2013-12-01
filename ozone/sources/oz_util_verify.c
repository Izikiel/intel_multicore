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
/*  Verify volume command utiliti					*/
/*									*/
/*	verify [-readonly] <fsdevice>					*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_io_fs.h"
#include "oz_knl_status.h"
#include "oz_sys_handle.h"
#include "oz_sys_io.h"
#include "oz_sys_io_fs.h"
#include "oz_sys_io_fs_printf.h"
#include "oz_util_start.h"

static char *pn;

uLong oz_util_main (int argc, char *argv[])

{
  char *fsdev;
  int i;
  OZ_Handle h_iochan;
  OZ_IO_fs_verifyvol fs_verifyvol;
  uLong sts;

  pn = "verify";
  if (argc > 0) pn = argv[0];

  fsdev = NULL;
  memset (&fs_verifyvol, 0, sizeof fs_verifyvol);
  for (i = 1; i < argc; i ++) {
    if (strcmp (argv[i], "-readonly") == 0) {
      fs_verifyvol.readonly = 1;
      continue;
    }
    if (fsdev != NULL) goto usage;
    fsdev = argv[i];
  }
  if (fsdev == NULL) goto usage;

  /* Assign I/O channel to filesystem device */

  sts = oz_sys_io_assign (OZ_PROCMODE_KNL, &h_iochan, fsdev, OZ_LOCKMODE_CW);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u assigning channel to fs device %s\n", pn, sts, fsdev);
    return (sts);
  }

  /* Tell it to verify the volume structures */

  sts = oz_sys_io (OZ_PROCMODE_KNL, h_iochan, 0, OZ_IO_FS_VERIFYVOL, sizeof fs_verifyvol, &fs_verifyvol);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u verifying fs device %s\n", pn, sts, fsdev);
    return (sts);
  }

  oz_sys_io_fs_printf (oz_util_h_error, "%s: fs device %s verified\n", pn, fsdev);

  return (sts);

usage:
  oz_sys_io_fs_printf (oz_util_h_error, "%s: usage: %s [-readonly] <fs_device>\n", pn, pn);
  return (OZ_MISSINGPARAM);
}
