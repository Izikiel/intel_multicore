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
/*  Create directory utiliti						*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_io_fs.h"
#include "oz_knl_lock.h"
#include "oz_knl_status.h"
#include "oz_sys_io.h"
#include "oz_sys_io_fs.h"
#include "oz_sys_io_fs_printf.h"
#include "oz_util_start.h"

static char *pn;

uLong oz_util_main (int argc, char *argv[])

{
  uLong sts;
  OZ_Handle h_out;
  OZ_IO_fs_create fs_create;

  pn = "credir";
  if (argc > 0) pn = argv[0];

  if (argc != 2) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: usage: %s <directory>\n", pn, pn);
    return (OZ_MISSINGPARAM);
  }

  memset (&fs_create, 0, sizeof fs_create);
  fs_create.name = argv[1];
  fs_create.lockmode = OZ_LOCKMODE_PW;
  fs_create.filattrflags = OZ_FS_FILATTRFLAG_DIRECTORY;
  sts = oz_sys_io_fs_create (sizeof fs_create, &fs_create, 0, &h_out);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u creating directory %s\n", pn, sts, argv[1]);
  }
  return (sts);
}
