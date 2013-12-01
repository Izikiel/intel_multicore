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
/*  Dismount command utiliti						*/
/*									*/
/*	mount <fsdevice>						*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_io_fs.h"
#include "oz_knl_devio.h"
#include "oz_knl_lock.h"
#include "oz_knl_sdata.h"
#include "oz_knl_status.h"
#include "oz_sys_handle.h"
#include "oz_sys_io.h"
#include "oz_sys_io_fs.h"
#include "oz_sys_io_fs_printf.h"
#include "oz_sys_logname.h"
#include "oz_util_start.h"

static char *pn;

uLong oz_util_main (int argc, char *argv[])

{
  uLong sts;
  OZ_Handle h_tempchan;
  OZ_IO_fs_dismount fs_dismount;

  pn = "dismount";
  if (argc > 0) pn = argv[0];

  if (argc != 2) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: usage: %s <fs_device>\n", pn, pn);
    return (OZ_MISSINGPARAM);
  }

  /* Assign I/O channel to filesystem device */

  sts = oz_sys_io_assign (OZ_PROCMODE_KNL, &h_tempchan, argv[1], OZ_LOCKMODE_CW);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u assigning channel to fs device %s\n", pn, sts, argv[1]);
    return (sts);
  }

  /* Tell it to dismount the volume in the drive */

  memset (&fs_dismount, 0, sizeof fs_dismount);

  sts = oz_sys_io (OZ_PROCMODE_KNL, h_tempchan, 0, OZ_IO_FS_DISMOUNT, sizeof fs_dismount, &fs_dismount);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u dismounting fs device %s\n", pn, sts, argv[1]);
    return (sts);
  }

  oz_sys_io_fs_printf (oz_util_h_error, "%s: fs device %s dismounted\n", pn, argv[1]);

  return (sts);
}
