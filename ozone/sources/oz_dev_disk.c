//+++2001-10-24
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
//---2001-10-24

/************************************************************************/
/*									*/
/*  Disk driver utility routines					*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_dev_disk.h"
#include "oz_dev_dpar.h"
#include "oz_io_fs.h"
#include "oz_knl_devio.h"
#include "oz_knl_lock.h"
#include "oz_knl_procmode.h"
#include "oz_knl_sdata.h"
#include "oz_knl_status.h"

/************************************************************************/
/*									*/
/*  This is an autogen routine that disk drivers can use.  		*/
/*									*/
/*  If the sub-device is numeric, it assumes it is a partition and 	*/
/*  tries the partition driver.  Otherwise, it assumes the sub-device 	*/
/*  is a filesystem template device and it tries to mount the disk 	*/
/*  using that template device.						*/
/*									*/
/*    Input:								*/
/*									*/
/*	host_devunit = points to disk's devunit				*/
/*	devname = device name that is to be created			*/
/*	suffix = part of devname that follows disk name			*/
/*	         for our purposes, it points at the fstemplate name	*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_dev_disk_auto = NULL : no device created			*/
/*	                   else : pointer to created device		*/
/*									*/
/*    Note:								*/
/*									*/
/*	disk driver calls ...						*/
/*	  oz_knl_devunit_autogen (devunit, oz_dev_disk_auto, NULL)	*/
/*	... in its init routine to enable				*/
/*									*/
/************************************************************************/

OZ_Devunit *oz_dev_disk_auto (void *dummy, OZ_Devunit *host_devunit, const char *devname, const char *suffix)

{
  int i;
  OZ_Devunit *devunit;
  OZ_IO_fs_mountvol fs_mountvol;
  OZ_Iochan *iochan;
  uLong partno, sts;

  /* If sub-device is numeric, try the partition driver */

  partno = oz_hw_atoi (suffix, &i);				/* try to decode partition number */
  if ((i > 0) && ((suffix[i] == 0) || (suffix[i] == '.'))) {	/* it must end at end of string or at a dot */
    devunit = oz_dev_dpar_init (host_devunit, partno);		/* ok, try to configure partition device */
    if (devunit != NULL) {
      oz_knl_printk ("oz_dev_disk_auto: partition %u found on %s\n", partno, oz_knl_devunit_devname (host_devunit));
    }
    return (devunit);						/* return pointer (or NULL if failure) */
  }

  /* Not numeric, assign channel to template device (eg, oz_dfs) */

  sts = oz_knl_iochan_crbynm (suffix, OZ_LOCKMODE_CW, OZ_PROCMODE_KNL, NULL, &iochan);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_dev_disk_auto: error %u assigning channel to %s for %s\n", sts, suffix, devname);
    return (NULL);
  }

  /* Try mounting the host device using the template device */

  memset (&fs_mountvol, 0, sizeof fs_mountvol);
  if (oz_s_inloader) fs_mountvol.mountflags = OZ_FS_MOUNTFLAG_NOCACHE;
  fs_mountvol.devname     = oz_knl_devunit_devname (host_devunit);
  fs_mountvol.secattrsize = oz_knl_secattr_getsize (oz_s_secattr_tempdev);
  fs_mountvol.secattrbuff = oz_knl_secattr_getbuff (oz_s_secattr_tempdev);
  sts = oz_knl_io (iochan, OZ_IO_FS_MOUNTVOL, sizeof fs_mountvol, &fs_mountvol);

  /* If we failed, close channel return NULL pointer */

  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_dev_disk_auto: error %u mounting drive %s using %s\n", sts, fs_mountvol.devname, suffix);
    oz_knl_iochan_increfc (iochan, -1);
    return (NULL);
  }

  /* Ok, return pointer to resulting fs device and close channel to it */

  devunit = oz_knl_iochan_getdevunit (iochan);
  oz_knl_devunit_increfc (devunit, 1);
  oz_knl_iochan_increfc (iochan, -1);
  oz_knl_printk ("oz_dev_disk_auto: filesystem mounted for %s\n", devname);
  return (devunit);
}
