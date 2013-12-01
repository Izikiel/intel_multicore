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
/*  Init command utiliti						*/
/*									*/
/*	init <drive> <template>						*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_io_fs.h"
#include "oz_knl_devio.h"
#include "oz_knl_lock.h"
#include "oz_knl_status.h"
#include "oz_sys_handle.h"
#include "oz_sys_io.h"
#include "oz_sys_io_fs.h"
#include "oz_sys_io_fs_printf.h"
#include "oz_util_start.h"

static char *pn;

uLong oz_util_main (int argc, char *argv[])

{
  char fsdevname[OZ_DEVUNIT_NAMESIZE], *p, *template;
  int i, usedup;
  uLong sts;
  OZ_Handle h_tempchan;
  OZ_IO_fs_initvol fs_initvol;

  pn = "init";
  if (argc > 0) pn = argv[0];

  template = NULL;
  memset (&fs_initvol, 0, sizeof fs_initvol);

  for (i = 1; i < argc; i ++) {
    if (strcmp (argv[i], "-clusterfactor") == 0) {
      if (++i >= argc) goto usage;
      fs_initvol.clusterfactor = oz_hw_atoi (argv[i], &usedup);
      if (argv[i][usedup] != 0) goto usage;
      continue;
    }
    if (strcmp (argv[i], "-writethru") == 0) {
      fs_initvol.initflags |= OZ_FS_INITFLAG_WRITETHRU;
      continue;
    }
    if (fs_initvol.devname == NULL) {
      fs_initvol.devname = argv[i];
      continue;
    }
    if (template == NULL) {
      template = argv[i];
      continue;
    }
    if (fs_initvol.volname == NULL) {
      fs_initvol.volname = argv[i];
      continue;
    }
    goto usage;
  }
  if (fs_initvol.volname == NULL) goto usage;

  /* Assign I/O channel to filesystem template device */

  sts = oz_sys_io_assign (OZ_PROCMODE_KNL, &h_tempchan, template, OZ_LOCKMODE_CW);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u assigning channel to template %s\n", pn, sts, template);
    return (sts);
  }

  /* Tell it to init the volume in the drive */

  sts = oz_sys_io (OZ_PROCMODE_KNL, h_tempchan, 0, OZ_IO_FS_INITVOL, sizeof fs_initvol, &fs_initvol);
  if (sts != OZ_SUCCESS) oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u initting drive %s using %s\n", pn, sts, fs_initvol.devname, template);
  else oz_sys_io_fs_printf (oz_util_h_error, "%s: drive %s initted using %s\n", pn, fs_initvol.devname, template);

  return (sts);

usage:
  oz_sys_io_fs_printf (oz_util_h_error, "usage: %s [-clusterfactor <n>] [-writethru] <devname> <template> <volname>\n", pn);
  return (OZ_BADPARAM);
}
