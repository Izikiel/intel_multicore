//+++2001-10-06
//    Copyright (C) 2001, Mike Rieker, Beverly, MA USA
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
//---2001-10-06

/************************************************************************/
/*									*/
/*  Replace the 'dirent' routines					*/
/*									*/
/************************************************************************/

#define _OZ_CRTL_DIRIO_C

#include "ozone.h"
#include "oz_knl_status.h"
#include "oz_crtl_dirio.h"
#include "oz_crtl_malloc.h"
#include "oz_io_fs.h"
#include "oz_sys_handle.h"
#include "oz_sys_io_fs.h"

struct DIR { OZ_Handle h_dir;
             OZ_IO_fs_readdir fs_readdir;
             struct dirent de;
             char filenambuff[OZ_FS_MAXFNLEN];
           };

#include <errno.h>

extern uLong errno_ozsts;

DIR *opendir (const char *name)

{
  DIR *dir;
  uLong sts;
  OZ_Handle h_dir;
  OZ_IO_fs_open fs_open;

  memset (&fs_open, 0, sizeof fs_open);
  fs_open.name = name;
  fs_open.lockmode = OZ_LOCKMODE_CR;
  sts = oz_sys_io_fs_open (sizeof fs_open, &fs_open, 0, &h_dir);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printerror ("oz_crtl_dir opendir: error %u opening directory %s\n", sts, name);
    errno = 65535;
    errno_ozsts = sts;
    return (NULL);
  }

  dir = malloc (sizeof *dir);
  dir -> h_dir = h_dir;
  memset (&(dir -> fs_readdir), 0, sizeof dir -> fs_readdir);
  memset (&(dir -> de), 0, sizeof dir -> de);
  dir -> fs_readdir.filenamsize = sizeof dir -> filenambuff;
  dir -> fs_readdir.filenambuff = dir -> filenambuff;
  dir -> de.d_name = dir -> filenambuff;
  return (dir);
}

struct dirent *readdir (DIR *dir)

{
  uLong sts;

  sts = oz_sys_io (OZ_PROCMODE_KNL, dir -> h_dir, 0, OZ_IO_FS_READDIR, 
                   sizeof dir -> fs_readdir, &(dir -> fs_readdir));
  if (sts == OZ_ENDOFFILE) return (NULL);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printerror ("oz_crtl_dir readdir: error %u reading directory\n", sts);
    errno = 65535;
    errno_ozsts = sts;
    return (NULL);
  }

  return (&(dir -> de));
}

int closedir (DIR *dir)

{
  oz_sys_handle_release (OZ_PROCMODE_KNL, dir -> h_dir);
  free (dir);
  return (0);
}
