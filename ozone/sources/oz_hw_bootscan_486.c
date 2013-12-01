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
/*  Routine to scan the devices in search of the boot volume		*/
/*									*/
/*    Input:								*/
/*									*/
/*	oz_hw486_bootdevtype  = 'CD', 'FD', 'HD'			*/
/*	oz_hw486_bootparamlbn = lbn of block that contains loader param page
/*	oz_hw486_bootparams   = original loader param page loaded by boot block
/*	oz_ldr_paramblock.load_device = assumed to be null string	*/
/*	oz_ldr_paramblock.load_fstemp = must be filled in with boot vol's fs template device
/*									*/
/*	*abortflag = gets set to 1 externally to abort the scan		*/
/*	abortevent = gets set externally when *abortflag gets set	*/
/*	verbose    = print debug info					*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_hw_bootscan = 0 : failed to find boot volume			*/
/*	                 1 : boot vol found, oz_ldr_paramblock.load_device filled in
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_dev_dpar.h"
#include "oz_dev_scsi.h"
#include "oz_io_disk.h"
#include "oz_io_fs.h"
#include "oz_io_scsi.h"
#include "oz_knl_devio.h"
#include "oz_knl_event.h"
#include "oz_knl_hw.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_sdata.h"
#include "oz_knl_status.h"
#include "oz_ldr_params.h"

#define BLOCKSIZE 512
#define DT_CD ('C' + ('D' << 8))
#define DT_FD ('F' + ('D' << 8))
#define DT_HD ('H' + ('D' << 8))

#define PRINTK(args) if (debugit) oz_knl_printk args

extern const uWord oz_hw486_bootdevtype;
extern const OZ_Dbn oz_hw486_bootparamlbn;
extern const OZ_Loadparams oz_hw486_bootparams;

#pragma pack(1)
typedef struct { uByte code[BLOCKSIZE-2-64-32];
                 struct { char string[8]; } partnames[4];
                 struct { uByte flag;
                          uByte fill1[3];
                          uByte ptype;
                          uByte fill2[3];
                          uLong start;
                          uLong count;
                        } partitions[4];
                 uWord magic;
               } Partition;
#pragma pack(8)

typedef struct Diskctx Diskctx;

struct Diskctx { Diskctx *next;
                 const char *devname;
                 OZ_Iochan *diskiochan;
                 OZ_Iochan *fsiochan;
                 OZ_IO_scsi_getinfo1 scsi_getinfo1;
                 uQuad align1;
                 OZ_Loadparams parambuff;
                 uQuad align2;
                 Partition partblockbuff;
               };

static Diskctx *volatile diskctxs;
static int debugit;
static OZ_Event *eventflag;
static volatile int *aborted;
static volatile Long numstarted, wefoundit;

static void startadevunit (OZ_Devunit *devunit);
static uLong scanscsibus (void *diskctxv);
static void spindiskup (Diskctx *diskctx);
static void volnowvalid (void *diskctxv, uLong status);
static void paramblockread (void *diskctxv, uLong status);
static void fdhdpartblockread (void *diskctxv, uLong status);
static void trymountingit (Diskctx *diskctx);
static void volmounted (void *diskctxv, uLong status);
static void dismounted (void *diskctxv, uLong status);
static void spindiskdown (Diskctx *diskctx);
static void diskspundown (void *diskctxv, uLong status);
static void killdiskctx (Diskctx *diskctx);

int oz_hw_bootscan (volatile int *abortflag, OZ_Event *abortevent, int verbose)

{
  Diskctx *diskctx;
  OZ_Devunit *devunit, *lastdevunit;
  uLong sts;

  /* Boot block better have given us something we can understand */

  if ((oz_hw486_bootdevtype != DT_CD)		// we can scan a CDROM
   && (oz_hw486_bootdevtype != DT_FD)		// we can scan a floppy disk
   && (oz_hw486_bootdevtype != DT_HD)) {	// we can scan a hard drive
    oz_knl_printk ("oz_hw_bootscan: don't know how to scan device type %c%c\n", 
	(char)oz_hw486_bootdevtype, (char)(oz_hw486_bootdevtype >> 8));
    return (0);
  }

  /* We have to know what filesystem it is */

  if (oz_ldr_paramblock.load_fstemp[0] == 0) {
    oz_knl_printk ("oz_hw_bootscan: must set load_fstemplate to indicate what fs the boot volume is\n");
    return (0);
  }

  /* Use the abort event flag to synchronize */

  aborted   = abortflag;
  eventflag = abortevent;
  debugit   = verbose;

  /* Scan through all devices in the system */

  oz_knl_printk ("oz_hw_bootscan: scanning for %c%c boot volume\n", 
	(char)oz_hw486_bootdevtype, (char)(oz_hw486_bootdevtype >> 8));

  wefoundit  = 0;
  numstarted = 1;
  diskctxs   = NULL;
  for (lastdevunit = NULL; (devunit = oz_knl_devunit_getnext (lastdevunit)) != NULL; lastdevunit = devunit) {
    if (lastdevunit != NULL) oz_knl_devunit_increfc (lastdevunit, -1);
    startadevunit (devunit);
  }
  -- numstarted;

  /* Wait for them to complete */

  while (numstarted > 0) {
    if (*abortflag || wefoundit) {
      PRINTK (("oz_hw_bootscan: aborting (%d still in progress)\n", numstarted));
      for (diskctx = diskctxs; diskctx != NULL; diskctx = diskctx -> next) {
        if (diskctx -> diskiochan != NULL) oz_knl_ioabort (diskctx -> diskiochan, OZ_PROCMODE_KNL);
        if (diskctx -> fsiochan   != NULL) oz_knl_ioabort (diskctx -> fsiochan,   OZ_PROCMODE_KNL);
      }
    }
    oz_knl_event_waitone (eventflag);
    oz_knl_event_set (eventflag, 0);
  }

  /* Free them all off */

  while ((diskctx = diskctxs) != NULL) {
    diskctxs = diskctx -> next;
    if (diskctx -> diskiochan != NULL) {
      PRINTK (("oz_hw_bootscan: %s deassigning disk channel\n", diskctx -> devname));
      oz_knl_iochan_increfc (diskctx -> diskiochan, -1);
    }
    if (diskctx -> fsiochan   != NULL) {
      PRINTK (("oz_hw_bootscan: %s deassigning fs channel\n", diskctx -> devname));
      oz_knl_iochan_increfc (diskctx -> fsiochan,   -1);
    }
    OZ_KNL_NPPFREE (diskctx);
  }

  /* Return whether or not we found it */

  return (wefoundit);
}

/************************************************************************/
/*									*/
/*  Start processing the given device					*/
/*									*/
/*    Input:								*/
/*									*/
/*	devunit = possible disk or scsi controller device		*/
/*									*/
/*    Output:								*/
/*									*/
/*	processing started on devunit					*/
/*	caller must dec devunit refcount				*/
/*									*/
/************************************************************************/

static void startadevunit (OZ_Devunit *devunit)

{
  Diskctx *diskctx;
  OZ_Thread *scsithread;
  uLong sts;

  /* Alloc context struct and assign channel to device */
  /* There shouldn't be anyone trying to write to it   */

  diskctx = OZ_KNL_NPPMALLOC (sizeof *diskctx);
  diskctx -> devname    = oz_knl_devunit_devname (devunit);
  diskctx -> diskiochan = NULL;
  diskctx -> fsiochan   = NULL;
  sts = oz_knl_iochan_create (devunit, OZ_LOCKMODE_PR, OZ_PROCMODE_KNL, NULL, &(diskctx -> diskiochan));
  if (sts != OZ_SUCCESS) {
    if (debugit || (sts != OZ_ACCONFLICT)) {
      oz_knl_printk ("oz_hw_bootscan: error %u assigning channel to device %s\n", sts, diskctx -> devname);
    }
    OZ_KNL_NPPFREE (diskctx);
    return;
  }

  /* Link to list of active requests */

  numstarted ++;
  diskctx -> next = diskctxs;
  diskctxs = diskctx;

  /* See if it is a SCSI controller by trying to sense the scsi controller's ID */

  memset (&(diskctx -> scsi_getinfo1), 0, sizeof diskctx -> scsi_getinfo1);
  sts = oz_knl_io (diskctx -> diskiochan, OZ_IO_SCSI_GETINFO1, sizeof diskctx -> scsi_getinfo1, &(diskctx -> scsi_getinfo1));
  if (sts == OZ_SUCCESS) {
    PRINTK (("oz_hw_bootscan: %s starting scsi controller scan\n", diskctx -> devname));
    sts = oz_knl_thread_create (oz_s_systemproc, -1, NULL, NULL, NULL, 0, scanscsibus, diskctx, OZ_ASTMODE_INHIBIT, 
                                strlen (diskctx -> devname), diskctx -> devname, NULL, &scsithread);
    if (sts == OZ_SUCCESS) oz_knl_thread_increfc (scsithread, -1);
    else {
      oz_knl_printk ("oz_hw_bootscan: error %u creating thread to scan %s\n", sts, diskctx -> devname);
      killdiskctx (diskctx);
    }
    return;
  }

  /* Make sure driver tells us that it's possible for it to be a 486-style boot device of type given by oz_hw486_bootdevtype */

//  sts = oz_knl_io (diskctx -> diskiochan, OZ_IO_486_CHECKBOOTTYPE, 0, NULL);
//  if (sts != OZ_SUCCESS) {
//    killdiskctx (diskctx);
//    return;
//  }

  /* Start spinning the volume up.  It is probably already physically spinning, but the driver might not know it. */

  spindiskup (diskctx);
}

/************************************************************************/
/*									*/
/*  Thread to scan a scsi bus						*/
/*									*/
/*    Input:								*/
/*									*/
/*	diskctx = pointer to context block with channel assigned to a 	*/
/*	          scsi controller					*/
/*									*/
/*    Output:								*/
/*									*/
/*	controller's scsi bus scanned, for any devices found, try to 	*/
/*	find the boot block on them					*/
/*									*/
/************************************************************************/

static uLong scanscsibus (void *diskctxv)

{
  Diskctx *diskctx;
  OZ_Devunit *ctrl_devunit, *disk_devunit;
  uLong scsi_id;

  diskctx = diskctxv;

  ctrl_devunit = oz_knl_iochan_getdevunit (diskctx -> diskiochan);			// get scsi controller's devunit
  oz_knl_devunit_increfc (ctrl_devunit, 1);
  oz_knl_iochan_increfc (diskctx -> diskiochan, -1);					// deassign channel so oz_dev_scsi_scan1
  diskctx -> diskiochan = NULL;								// ... won't die with acconflict error

  for (scsi_id = 0; scsi_id < diskctx -> scsi_getinfo1.max_scsi_id; scsi_id ++) {	// loop through each possible scsi-id
    if (wefoundit || *aborted) break;							// stop scanning if boot dev has been found
											// ... or we've been aborted
    if (scsi_id != diskctx -> scsi_getinfo1.ctrl_scsi_id) {				// skip the controller's scsi-id
      PRINTK (("oz_hw_bootscan: %s checking scsi-id %u\n", diskctx -> devname, scsi_id));
      disk_devunit = oz_dev_scsi_scan1 (ctrl_devunit, scsi_id);				// see what's at that id number
      if (disk_devunit != NULL) {
        startadevunit (disk_devunit);							// something there, start processing it
        oz_knl_devunit_increfc (disk_devunit, -1);					// anyway, we're done with this pointer
      }
    }
  }

  oz_knl_devunit_increfc (ctrl_devunit, -1);						// we're done scanning the controller
  killdiskctx (diskctx);
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Start spinning disk up						*/
/*									*/
/************************************************************************/

static void spindiskup (Diskctx *diskctx)

{
  OZ_IO_disk_setvolvalid disk_setvolvalid;
  uLong sts;

  PRINTK (("oz_hw_bootscan: %s spinning up\n", diskctx -> devname));
  memset (&disk_setvolvalid, 0, sizeof disk_setvolvalid);
  disk_setvolvalid.valid = 1;
  sts = oz_knl_iostart3 (1, NULL, diskctx -> diskiochan, OZ_PROCMODE_KNL, volnowvalid, diskctx, NULL, NULL, NULL, NULL, 
                         OZ_IO_DISK_SETVOLVALID, sizeof disk_setvolvalid, &disk_setvolvalid);
  if (sts != OZ_STARTED) volnowvalid (diskctx, sts);
}

/************************************************************************/
/*									*/
/*  Volume is spun up							*/
/*									*/
/************************************************************************/

static void volnowvalid (void *diskctxv, uLong status)

{
  Diskctx *diskctx;
  OZ_IO_disk_readblocks disk_readblocks;
  uLong sts;

  diskctx = diskctxv;

  /* Make sure it actually spun up (maybe there isn't even a disk in the drive) */

  if (status != OZ_SUCCESS) {
    PRINTK (("oz_hw_bootscan: %s error %u spinning up\n", diskctx -> devname, status));
    killdiskctx (diskctx);
    return;
  }

  /* Ok, start reading the params block */

  PRINTK (("oz_hw_bootscan: %s reading params block %p %u at %p\n", diskctx -> devname, diskctx, sizeof diskctx -> parambuff, &(diskctx -> parambuff)));
  memset (&disk_readblocks, 0, sizeof disk_readblocks);
  disk_readblocks.size = sizeof diskctx -> parambuff;
  disk_readblocks.buff = &(diskctx -> parambuff);
  disk_readblocks.slbn = oz_hw486_bootparamlbn;
  sts = oz_knl_iostart3 (1, NULL, diskctx -> diskiochan, OZ_PROCMODE_KNL, paramblockread, diskctx, NULL, NULL, NULL, NULL, 
                         OZ_IO_DISK_READBLOCKS, sizeof disk_readblocks, &disk_readblocks);
  if (sts != OZ_STARTED) paramblockread (diskctx, sts);
}

/************************************************************************/
/*									*/
/*  The boot params block has been read from the disk			*/
/*									*/
/************************************************************************/

static void paramblockread (void *diskctxv, uLong status)

{
  Diskctx *diskctx;
  OZ_IO_disk_readblocks disk_readblocks;
  uLong sts;

  diskctx = diskctxv;

  /* Make sure the read was successful - maybe it doesn't even have that many blocks */

  if (status != OZ_SUCCESS) {
    PRINTK (("oz_hw_bootscan: %s error %u reading params block\n", diskctx -> devname, status));
    spindiskdown (diskctx);
    return;
  }

  /* It must match the original params block read by the bootblock routine */
  /* If not, this is not the boot disk                                     */

  if (memcmp (&(diskctx -> parambuff), &oz_hw486_bootparams, sizeof oz_hw486_bootparams) != 0) {
    PRINTK (("oz_hw_bootscan: %s not the right params block\n", diskctx -> devname));
    spindiskdown (diskctx);
    return;
  }
  PRINTK (("oz_hw_bootscan: %s params block matches\n", diskctx -> devname));

  /* Process differently based on drive type provided by boot block */

  switch (oz_hw486_bootdevtype) {

    /* CD's don't have partitions as such (as far as El Torito is concerned, anyway) */

    case DT_CD: {
      trymountingit (diskctx);
      return;
    }

    /* I don't think a BIOS would boot a partitioned floppy, but we might as well allow for it anyway */

    case DT_FD:
    case DT_HD: {
      PRINTK (("oz_hw_bootscan: %s reading partition block\n", diskctx -> devname));
      memset (&disk_readblocks, 0, sizeof disk_readblocks);
      disk_readblocks.size = sizeof diskctx -> partblockbuff;
      disk_readblocks.buff = &(diskctx -> partblockbuff);
      sts = oz_knl_iostart3 (1, NULL, diskctx -> diskiochan, OZ_PROCMODE_KNL, fdhdpartblockread, diskctx, NULL, NULL, NULL, NULL, 
                             OZ_IO_DISK_READBLOCKS, sizeof disk_readblocks, &disk_readblocks);
      if (sts != OZ_STARTED) fdhdpartblockread (diskctx, sts);
      return;
    }
  }

  /* Shouldn't have changed since we checked it earlier */

  oz_crash ("oz_hw_bootscan: bad device type %c%c", 
	(char)oz_hw486_bootdevtype, (char)(oz_hw486_bootdevtype >> 8));
}

/************************************************************************/
/*									*/
/*  Partition block has been read from the floppy or hard disk		*/
/*									*/
/************************************************************************/

static void fdhdpartblockread (void *diskctxv, uLong status)

{
  Diskctx *diskctx;
  int i;
  OZ_Devunit *part_devunit;
  uLong sts;

  diskctx = diskctxv;

  /* Make sure we were actually able to read the partition block */

  if (status != OZ_SUCCESS) {
    PRINTK (("oz_hw_bootscan: %s error %u reading partition block\n", diskctx -> devname, status));
    goto killit;
  }

  /* If it doesn't have the magic word, it's not even bootable, let alone has partitions */

  if (diskctx -> partblockbuff.magic != 0xAA55) {
    PRINTK (("oz_hw_bootscan: %s has no magic word\n", diskctx -> devname));
    goto killit;
  }

  /* See if we have a partition that has the given logical block number */

  for (i = 0; i < 4; i ++) {
    if ((diskctx -> partblockbuff.partitions[i].flag & 0x7F) != 0) continue;
    if (diskctx -> partblockbuff.partitions[i].start > oz_hw486_bootparamlbn) continue;
    if (diskctx -> partblockbuff.partitions[i].start + diskctx -> partblockbuff.partitions[i].count > oz_hw486_bootparamlbn) goto partitioned;
  }

  /* Not partitioned, just try mounting it as is */

  goto mountit;

  /* Found a partition that holds the boot file, create partition device and point to it */

partitioned:
  PRINTK (("oz_hw_bootscan: %s partition %d found\n", diskctx -> devname, i + 1));
  part_devunit = oz_dev_dpar_init (oz_knl_iochan_getdevunit (diskctx -> diskiochan), i + 1);
  if (part_devunit != NULL) {
    oz_knl_iochan_increfc (diskctx -> diskiochan, -1);
    diskctx -> diskiochan = NULL;
    diskctx -> devname = oz_knl_devunit_devname (part_devunit);
    sts = oz_knl_iochan_create (part_devunit, OZ_LOCKMODE_PR, OZ_PROCMODE_KNL, NULL, &(diskctx -> diskiochan));
    oz_knl_devunit_increfc (part_devunit, -1);
    if (sts != OZ_SUCCESS) {
      PRINTK (("oz_hw_bootscan: %s error %u assigning channel to device\n", diskctx -> devname, sts));
      goto killit;
    }
  } else {
    PRINTK (("oz_hw_bootscan: %s unable to create %d partition device\n", diskctx -> devname, i + 1));
  }

mountit:
  trymountingit (diskctx);
  return;

killit:
  killdiskctx (diskctx);
}

/************************************************************************/
/*									*/
/*  Try mounting the disk						*/
/*									*/
/************************************************************************/

static void trymountingit (Diskctx *diskctx)

{
  OZ_IO_fs_mountvol fs_mountvol;
  uLong sts;

  /* Assign I/O channel to fs template device */

  sts = oz_knl_iochan_crbynm (oz_ldr_paramblock.load_fstemp, OZ_LOCKMODE_CR, OZ_PROCMODE_KNL, NULL, &(diskctx -> fsiochan));
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_hw_bootscan: error %u assigning channel to fstemplate %s\n", sts, oz_ldr_paramblock.load_fstemp);
    killdiskctx (diskctx);
    return;
  }

  /* Start mount request - READONLY and NOCACHE because we just dismount it right away anyway */

  PRINTK (("oz_hw_bootscan: %s starting to mount via %s\n", diskctx -> devname, oz_ldr_paramblock.load_fstemp));
  memset (&fs_mountvol, 0, sizeof fs_mountvol);
  fs_mountvol.devname    = diskctx -> devname;
  fs_mountvol.mountflags = OZ_FS_MOUNTFLAG_READONLY | OZ_FS_MOUNTFLAG_NOCACHE;
  sts = oz_knl_iostart3 (1, NULL, diskctx -> fsiochan, OZ_PROCMODE_KNL, volmounted, diskctx, NULL, NULL, NULL, NULL, 
                         OZ_IO_FS_MOUNTVOL, sizeof fs_mountvol, &fs_mountvol);
  if (sts != OZ_STARTED) volmounted (diskctx, sts);
}

/************************************************************************/
/*									*/
/*  Volume has been mounted (or failed to mount)			*/
/*									*/
/************************************************************************/

static void volmounted (void *diskctxv, uLong status)

{
  Diskctx *diskctx;
  uLong sts;

  diskctx = diskctxv;

  if (status != OZ_SUCCESS) {
    PRINTK (("oz_hw_bootscan: %s error %u mounting\n", diskctx -> devname, status));
    killdiskctx (diskctx);
    return;
  }
  PRINTK (("oz_hw_bootscan: %s successfully mounted\n", diskctx -> devname));

  /* Dismount it and save the device name as our boot device */

  sts = oz_knl_iostart3 (1, NULL, diskctx -> fsiochan, OZ_PROCMODE_KNL, dismounted, diskctx, NULL, NULL, NULL, NULL, 
                         OZ_IO_FS_DISMOUNT, 0, NULL);
  if (sts != OZ_STARTED) dismounted (diskctx, sts);
}

static void dismounted (void *diskctxv, uLong status)

{
  Diskctx *diskctx;

  diskctx = diskctxv;
  movc4 (strlen (diskctx -> devname), diskctx -> devname, sizeof oz_ldr_paramblock.load_device, oz_ldr_paramblock.load_device);
  oz_knl_printk ("oz_hw_bootscan: found boot device %s\n", oz_ldr_paramblock.load_device);
  OZ_HW_MB;
  wefoundit = 1;
  killdiskctx (diskctx);
}

/************************************************************************/
/*									*/
/*  Spin disk down and forget about it					*/
/*									*/
/************************************************************************/

static void spindiskdown (Diskctx *diskctx)

{
  uLong sts;

  PRINTK (("oz_hw_bootscan: %s spinning down\n", diskctx -> devname));
  sts = oz_knl_iostart3 (1, NULL, diskctx -> diskiochan, OZ_PROCMODE_KNL, diskspundown, diskctx, NULL, NULL, NULL, NULL, 
                         OZ_IO_DISK_SETVOLVALID, 0, NULL);
  if (sts != OZ_STARTED) diskspundown (diskctx, sts);
}

static void diskspundown (void *diskctxv, uLong status)

{
  PRINTK (("oz_hw_bootscan: %s spun down\n", ((Diskctx *)diskctxv) -> devname));
  killdiskctx (diskctxv);
}

/************************************************************************/
/*									*/
/*  All done with disk context block, free it off and wake main thread	*/
/*									*/
/************************************************************************/

static void killdiskctx (Diskctx *diskctx)

{
  -- numstarted;
  oz_knl_event_set (eventflag, 1);
}
