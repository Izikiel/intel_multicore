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
/*  Disk format command utiliti						*/
/*									*/
/*	diskfmt <drive> <format_parameter_string>			*/
/*									*/
/*	see driver doc for 'format_parameter_string' description	*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_io_disk.h"
#include "oz_knl_lock.h"
#include "oz_knl_status.h"
#include "oz_sys_handle.h"
#include "oz_sys_io.h"
#include "oz_sys_io_fs.h"
#include "oz_sys_io_fs_printf.h"
#include "oz_util_start.h"

static char *pn = "diskfmt";

uLong oz_util_main (int argc, char *argv[])

{
  char *template;
  uLong sts;
  OZ_Handle h_tempchan;
  OZ_IO_disk_format disk_format;

  if (argc > 0) pn = argv[0];
  if (argc != 3) goto usage;
  template = argv[1];

  /* Assign I/O channel to disk drive device */

  sts = oz_sys_io_assign (OZ_PROCMODE_KNL, &h_tempchan, template, OZ_LOCKMODE_EX);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u assigning channel to disk drive device %s\n", pn, sts, template);
    return (sts);
  }

  /* Tell it to format the media */

  memset (&disk_format, 0, sizeof disk_format);
  disk_format.paramstr = argv[2];
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_tempchan, 0, OZ_IO_DISK_FORMAT, sizeof disk_format, &disk_format);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u formatting disk drive %s using %s\n", pn, sts, template, disk_format.paramstr);
  }

  return (sts);

usage:
  oz_sys_io_fs_printf (oz_util_h_error, "%s: usage: %s <drive> <format_parameter_string>\n", pn, pn);
  return (OZ_BADPARAM);
}
