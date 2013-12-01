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
/*  Mount command utiliti						*/
/*									*/
/*	mount [-nocache] [-readonly] [-verify] [-writethru]		*/
/*		<drive> <template> [<logname>:]				*/
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

static char *pn = "mount";

uLong oz_util_main (int argc, char *argv[])

{
  char fsdevname[OZ_DEVUNIT_NAMESIZE+1], *logname, *template;
  int i;
  uLong sts;
  OZ_Handle h_lognamtbl, h_tempchan;
  OZ_IO_fs_mountvol fs_mountvol;
  OZ_Logvalue logvalue;

  if (argc > 0) pn = argv[0];

  memset (&fs_mountvol, 0, sizeof fs_mountvol);
  logname  = NULL;
  template = NULL;

  for (i = 1; i < argc; i ++) {
    if (strcmp (argv[i], "-nocache") == 0) {
      fs_mountvol.mountflags |= OZ_FS_MOUNTFLAG_NOCACHE;
      continue;
    }
    if (strcmp (argv[i], "-readonly") == 0) {
      fs_mountvol.mountflags |= OZ_FS_MOUNTFLAG_READONLY;
      continue;
    }
    if (strcmp (argv[i], "-verify") == 0) {
      fs_mountvol.mountflags |= OZ_FS_MOUNTFLAG_VERIFY;
      continue;
    }
    if (strcmp (argv[i], "-writethru") == 0) {
      fs_mountvol.mountflags |= OZ_FS_MOUNTFLAG_WRITETHRU;
      continue;
    }
    if (argv[i][0] == '-') goto usage;
    if (fs_mountvol.devname == NULL) {
      fs_mountvol.devname = argv[i];
      continue;
    }
    if (template == NULL) {
      template = argv[i];
      continue;
    }
    if (logname == NULL) {
      logname = argv[i];
      continue;
    }
    goto usage;
  }
  if (template == NULL) goto usage;

  /* Assign I/O channel to filesystem template device */

  sts = oz_sys_io_assign (OZ_PROCMODE_KNL, &h_tempchan, template, OZ_LOCKMODE_PW);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u assigning channel to template %s\n", pn, sts, template);
    return (sts);
  }

  /* Tell it to mount the volume in the drive */

  sts = oz_sys_io (OZ_PROCMODE_KNL, h_tempchan, 0, OZ_IO_FS_MOUNTVOL, sizeof fs_mountvol, &fs_mountvol);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u mounting drive %s using %s\n", pn, sts, fs_mountvol.devname, template);
    return (sts);
  }

  /* Get resultant filesystem device name */

  sts = oz_sys_iochan_getunitname (h_tempchan, sizeof fsdevname - 1, fsdevname);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u getting resultant device name\n", pn, sts);
    return (sts);
  }

  oz_sys_io_fs_printf (oz_util_h_error, "%s: drive %s mounted using %s as %s\n", pn, fs_mountvol.devname, template, fsdevname);

  /* Optionally assign logical name to the resultant filesystem device */

  if (logname != NULL) {
    sts = oz_sys_logname_lookup (0, OZ_PROCMODE_USR, oz_s_logname_defaulttables, NULL, NULL, NULL, &h_lognamtbl);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "oz_cli: error %u looking up default tables (%s)\n", sts, oz_s_logname_defaulttables);
      return (sts);
    }
    logvalue.attr = OZ_LOGVALATR_TERMINAL;
    logvalue.buff = fsdevname;
    sts = strlen (logname);
    if ((sts > 0) && (logname[sts-1] == ':')) {
      logname[sts-1] = 0;
      strcat (fsdevname, ":");
    }
    sts = oz_sys_logname_create (h_lognamtbl, logname, OZ_PROCMODE_KNL, 0, 1, &logvalue, NULL);
    if (sts != OZ_SUCCESS) oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u assigning logical %s to %s\n", pn, sts, logname, fsdevname);
  }

  return (sts);

usage:
  oz_sys_io_fs_printf (oz_util_h_error, "%s: usage: %s [-nocache] [-readonly] [-verify] [-writethru] <drive> <template> [<logname>:]\n", pn, pn);
  return (OZ_BADPARAM);
}
