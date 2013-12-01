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
/*  This module contains routines that a disk driver can use to 	*/
/*  implement disk partitioning.  They are called by the 		*/
/*  oz_dev_disk_auto autogen routine.					*/
/*									*/
/************************************************************************/

#include "ozone.h"

#include "oz_dev_disk.h"
#include "oz_dev_dpar.h"
#include "oz_io_disk.h"
#include "oz_knl_devio.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_lock.h"
#include "oz_knl_objtype.h"
#include "oz_knl_sdata.h"
#include "oz_knl_status.h"

#define PAR_TABLE_BLK_NUM (0)			/* logical block number of partition table */
#define PAR_TABLE_BLK_SIZ (512)			/* number of bytes in partition table */
#define NUM_PARTS (4)				/* number of partition table entries */

typedef struct Chnex Chnex;
typedef struct Devex Devex;

	/* disk partition block layout */

typedef struct { uByte bootable;	/* 0x00 = not bootable, 0x80 = bootable */
                 uByte beg_trk;		/* beginning track/sector/cylinder */
                 uByte beg_sec;
                 uByte beg_cyl;
                 uByte fs_type;		/* filesystem type id */
                 uByte end_trk;		/* ending track/sector/cylinder */
                 uByte end_sec;
                 uByte end_cyl;
                 uLong beg_blk;		/* beginning block number */
                 uLong num_blk;		/* number of blocks */
               } Partition;

#pragma pack (1)
typedef struct { uByte code[512-(NUM_PARTS * sizeof (Partition))-2];	/* partition boot block code and padding */
                 Partition partitions[NUM_PARTS];			/* partition table entries */
                 uWord flagword;					/* 0xAA55 */
               } Parblock;
#pragma nopack

	/* one of these per I/O channel assigned to partition disk */

struct Chnex { OZ_Iochan *hostiochan;		/* corresponding I/O channel assigned to host disk device */
               void *hostiochnex;		/* host I/O channel extension pointer */
             };

	/* one of these per partition */

struct Devex { OZ_Devunit *pardevunit;		/* partition device unit */
               OZ_Dbn psiz;			/* number of disk blocks in partition */
               OZ_Dbn plbn;			/* starting logical block number of partition */
               uLong pnum;			/* partition number */
               int valid;			/* partition is valid flag */
               OZ_Devunit *hostdevunit;		/* host device unit */
               void *hostdevex;			/* host device extension area pointer */
               const OZ_Devfunc *hostfunctab;	/* host disk device function table */
               uLong blocksize;			/* host disk block size */
               uLong bufalign;			/* host disk driver buffer alignment */
             };

static int dpar_clonedel (OZ_Devunit *cloned_devunit, void *devexv, int cloned);
static uLong dpar_assign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode);
static int dpar_deassign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv);
static void dpar_abort (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Ioop *ioop, void *iopexv, OZ_Procmode procmode);
static uLong dpar_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode, 
                         OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap);

/* func table needs to be read/write because iop_exsize and sel_exsize are modified by oz_dev_dpar_init routine */

static OZ_Devfunc dpar_functable = { sizeof (Devex), sizeof (Chnex), 0, 0, NULL, NULL, dpar_clonedel, dpar_assign, dpar_deassign, dpar_abort, dpar_start, NULL };

static OZ_Devclass  *devclass  = NULL;
static OZ_Devdriver *devdriver = NULL;

static Devex *crash_devex = NULL;		/* partition that crash dump will be written to */
static OZ_Devunit *crash_devunit = NULL;	/* partition that crash dump will be written to */
static OZ_IO_disk_crash crash_disk;		/* physical drive crash dump writing routine */

static uLong dpar_crash (void *dummy, OZ_Dbn lbn, uLong size, OZ_Mempage phypage, uLong offset);
static uLong reloc (Devex *devex, uLong size, OZ_Dbn *slbn);

/************************************************************************/
/*									*/
/*  This routine is called by the disk autogen routine when it finds 	*/
/*  an numeric sub-device name.  It reads the partition definition 	*/
/*  block on the host disk and creates a partition drive.		*/
/*									*/
/*    Input:								*/
/*									*/
/*	hostdevunit = host disk's device unit pointer			*/
/*	partno      = partition number to bring online			*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_dev_dpar_init = NULL : unable to bring online		*/
/*	                   else : devunit of partition device		*/
/*									*/
/************************************************************************/

OZ_Devunit *oz_dev_dpar_init (OZ_Devunit *hostdevunit, uLong partno)

{
  char unitdesc[OZ_DEVUNIT_DESCSIZE], unitname[OZ_DEVUNIT_NAMESIZE];
  const char *hostdevname;
  const OZ_Devfunc *hostfunctab;
  Devex *devex;
  int i;
  OZ_Dbn pend, plbn, psiz;
  OZ_Devunit *pardevunit;
  OZ_IO_disk_getinfo1 disk_getinfo1;
  OZ_IO_disk_readblocks disk_readblocks;
  OZ_IO_disk_setvolvalid disk_setvolvalid;
  OZ_Iochan *hostiochan;
  Parblock parblock;
  uLong sts;
  void *hostdevex;

  hostdevname = oz_knl_devunit_devname (hostdevunit);
  hostdevex   = oz_knl_devunit_ex (hostdevunit);
  hostfunctab = oz_knl_devunit_functable (hostdevunit);

  pardevunit = NULL;
  hostiochan = NULL;  

  /* Make sure partition number is valid */

  if ((partno == 0) || (partno > NUM_PARTS)) {
    oz_knl_printk ("oz_dev_dpar_init: invalid partition number %u for %s\n", partno, hostdevname);
    return (NULL);
  }

  /* Make sure we have devclass and devdriver entries */

  if (devdriver == NULL) {
    devclass  = oz_knl_devclass_create (OZ_IO_DISK_CLASSNAME, OZ_IO_DISK_BASE, OZ_IO_DISK_MASK, "oz_dpar");
    devdriver = oz_knl_devdriver_create (devclass, "oz_dpar");
  }

  /* Make sure any iopex's queued to me have iopex's of at least the size required by host driver. */
  /* Likewise for any selects.                                                                     */

  if (dpar_functable.iop_exsize < hostfunctab -> iop_exsize) dpar_functable.iop_exsize = hostfunctab -> iop_exsize;
  if (dpar_functable.sel_exsize < hostfunctab -> sel_exsize) dpar_functable.sel_exsize = hostfunctab -> sel_exsize;

  /* Open an I/O channel to the host disk so we can read its partition table block */

  sts = oz_knl_iochan_create (hostdevunit, OZ_LOCKMODE_CR, OZ_PROCMODE_KNL, NULL, &hostiochan);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_dev_dpar_init: error %u assigning channel to %s\n", sts, hostdevname);
    goto rtn;
  }

  /* Make sure volume valid is set so we can read from the disk */

  memset (&disk_setvolvalid, 0, sizeof disk_setvolvalid);
  disk_setvolvalid.valid = 1;
  sts = oz_knl_io (hostiochan, OZ_IO_DISK_SETVOLVALID, sizeof disk_setvolvalid, &disk_setvolvalid);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_dev_dpar_init: error %u setting %s volume valid\n", sts, hostdevname);
    goto rtn;
  }

  /* Get the disk's info */

  memset (&disk_getinfo1, 0, sizeof disk_getinfo1);
  sts = oz_knl_io (hostiochan, OZ_IO_DISK_GETINFO1, sizeof disk_getinfo1, &disk_getinfo1);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_dev_dpar_init: error %u getting %s info\n", sts, hostdevname);
    goto rtn;
  }

  /* Read the partition table block from the host disk */

  memset (&disk_readblocks, 0, sizeof disk_readblocks);
  disk_readblocks.size = sizeof parblock;
  disk_readblocks.buff = &parblock;
  disk_readblocks.slbn = PAR_TABLE_BLK_NUM;
  sts = oz_knl_io (hostiochan, OZ_IO_DISK_READBLOCKS, sizeof disk_readblocks, &disk_readblocks);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_dev_dpar_init: error %u reading %s partition block\n", sts, hostdevname);
    goto rtn;
  }

  /* Make sure it is a partition block */

  if (parblock.flagword != 0xAA55) goto rtn;
  for (i = 0; i < NUM_PARTS; i ++) {
    if (parblock.partitions[i].bootable & 0x7f) goto rtn;
  }

  /* Read ok, process partition */

  psiz = parblock.partitions[partno-1].num_blk;
  plbn = parblock.partitions[partno-1].beg_blk;
  if (psiz == 0) goto rtn;

  oz_knl_printk ("oz_dev_dpar_init: partition %u found on device %s\n", partno, hostdevname);

  pend = psiz + plbn;
  if ((pend < plbn) || (pend > disk_getinfo1.totalblocks)) {
    oz_knl_printk ("oz_dev_dpar_init: partition %u: size %u, block %u goes off end of %u block disk %s\n", 
                   partno, psiz, plbn, disk_getinfo1.totalblocks, hostdevname);
    goto rtn;
  }

  /* Partition found, create disk device table entry for it */

  oz_sys_sprintf (sizeof unitname, unitname, "%s.%u", hostdevname, partno);		/* unitname = <host_devunit_name>.<partition_number> */
  oz_sys_sprintf (sizeof unitdesc, unitdesc, "partition %u: %u blocks at %u, type %2.2X", /* unitdesc = something descriptive */
                  partno, psiz, plbn, parblock.partitions[partno-1].fs_type);

  pardevunit = oz_knl_devunit_create (devdriver, unitname, unitdesc, &dpar_functable, 	/* create pseudo device table entry */
                                      0, oz_knl_devunit_getsecattr (hostdevunit));
  if (pardevunit == NULL) goto rtn;							/* skip it if any failure (like duplicate device) */
  devex = oz_knl_devunit_ex (pardevunit);						/* ok, get pointer to extension */
  memset (devex, 0, sizeof *devex);							/* clear it all out */
  devex -> pardevunit  = pardevunit;							/* save partition device unit pointer */
  devex -> pnum        = partno;							/* partition number */
  devex -> psiz        = psiz;								/* partition size (in blocks) */
  devex -> plbn        = plbn;								/* partition starting block number */
  devex -> hostdevunit = hostdevunit;							/* save the host it is on */
  devex -> hostdevex   = oz_knl_devunit_ex (hostdevunit);				/* save host device extension area */
  devex -> hostfunctab = hostfunctab;							/* remember where host dev functab is */
  devex -> blocksize   = disk_getinfo1.blocksize;					/* copy host's block size */
  devex -> bufalign    = disk_getinfo1.bufalign;					/* copy host's buffer alignment */
  oz_knl_devunit_increfc (hostdevunit, 1);						/* make sure hostdev stays around */

  oz_knl_devunit_autogen (pardevunit, oz_dev_disk_auto, NULL);

rtn:
  if (hostiochan != NULL) oz_knl_iochan_increfc (hostiochan, -1);
  return (pardevunit);
}

/************************************************************************/
/*									*/
/*  The last I/O channel was just deassigned from the partition device	*/
/*									*/
/*  It returns a non-zero status indicating that it is ok to delete 	*/
/*  the devunit entry - the partition device is now gone		*/
/*									*/
/************************************************************************/

static int dpar_clonedel (OZ_Devunit *cloned_devunit, void *devexv, int cloned)

{
  Devex *devex;

  devex = devexv;

  /* Disconnect the host by closing the channel */

  oz_knl_devunit_increfc (devex -> hostdevunit, -1);
  devex -> hostdevunit = NULL;

  return (1);
}

/************************************************************************/
/*									*/
/*  An new I/O channel was just assigned to the partition device, 	*/
/*  assign a corresponding I/O channel to the host device		*/
/*									*/
/************************************************************************/

static uLong dpar_assign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode)

{
  Chnex *chnex;
  Devex *devex;
  uLong sts;
  OZ_Lockmode hostlockmode, parlockmode;

  chnex = chnexv;
  devex = devexv;

  memset (chnex, 0, sizeof *chnex);

  /* Convert lock mode to equivalent that allows sharing, as the host disk device is shared amongst the partitions */

  parlockmode = oz_knl_iochan_getlockmode (iochan);							/* get partition I/O channel's lock mode */
  if (OZ_LOCK_ALLOW_TEST (parlockmode, OZ_LOCK_ALLOWS_SELF_WRITE)) hostlockmode = OZ_LOCKMODE_CW;	/* if it allows writing, get lockmode that allows writing */
  else if (OZ_LOCK_ALLOW_TEST (parlockmode, OZ_LOCK_ALLOWS_SELF_READ)) hostlockmode = OZ_LOCKMODE_CR;	/* if it allows only reads, get lockmode that allows only reads */
  else hostlockmode = OZ_LOCKMODE_NL;									/* if it allows nothing, get lockmode that allows nothing */

  /* Create I/O channel to host disk device */

  sts = oz_knl_iochan_create (devex -> hostdevunit, hostlockmode, procmode, NULL, &(chnex -> hostiochan));
  if (sts == OZ_SUCCESS) chnex -> hostiochnex = oz_knl_iochan_ex (chnex -> hostiochan);
  return (sts);
}

/************************************************************************/
/*									*/
/*  An I/O channel was just deassigned from the partition device.  	*/
/*  De-assign the corresponding I/O channel from the host device.	*/
/*									*/
/************************************************************************/

static int dpar_deassign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv)

{
  Chnex *chnex;
  OZ_Iochan *hostiochan;

  chnex = chnexv;

  hostiochan = chnex -> hostiochan;		/* get pointer to host I/O channel */
  if (hostiochan != NULL) {
    chnex -> hostiochan  = NULL;		/* clear pointers to cause a fault if accessed */
    chnex -> hostiochnex = NULL;
    oz_knl_iochan_increfc (hostiochan, -1);	/* release the host I/O channel */
  }

  return (0);
}

/************************************************************************/
/*									*/
/*  I/O on a partition channel is to be aborted.  Abort the I/O on the 	*/
/*  corresponding host I/O channel.					*/
/*									*/
/************************************************************************/

static void dpar_abort (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Ioop *ioop, void *iopexv, OZ_Procmode procmode)

{
  oz_knl_ioabort (((Chnex *)chnexv) -> hostiochan, procmode);
}

/************************************************************************/
/*									*/
/*  An I/O request is being started on the disk partition		*/
/*									*/
/************************************************************************/

static uLong dpar_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode, 
                         OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap)

{
  Chnex *chnex;
  Devex *devex;
  uLong sts;

  chnex = chnexv;
  devex = devexv;

  switch (funcode) {

    /* Set volume valid - the partition is always online as long as host disk is */

    case OZ_IO_DISK_SETVOLVALID: {
      OZ_IO_disk_setvolvalid disk_setvolvalid;

      movc4 (as, ap, sizeof disk_setvolvalid, &disk_setvolvalid);
      devex -> valid = disk_setvolvalid.valid;
      return (OZ_SUCCESS);
    }

    /* Read and Write Blocks - relocate the starting block number of the request then pass it on to the host driver's start routine */
    /* Here is where we require the size of our chnexv to be at least as big as any host, and likewise for iopexv                   */

    case OZ_IO_DISK_WRITEBLOCKS: {
      OZ_IO_disk_writeblocks disk_writeblocks;

      if (!(devex -> valid)) return (OZ_VOLNOTVALID);
      movc4 (as, ap, sizeof disk_writeblocks, &disk_writeblocks);
      sts = reloc (devex, disk_writeblocks.size, &disk_writeblocks.slbn);
      if (sts == OZ_SUCCESS) sts = (*(devex -> hostfunctab -> start)) (devex -> hostdevunit, 
                                                                       devex -> hostdevex, 
                                                                       chnex -> hostiochan, 
                                                                       chnex -> hostiochnex, 
                                                                       procmode, ioop, iopexv, funcode, 
                                                                       sizeof disk_writeblocks, &disk_writeblocks);
      return (sts);
    }

    case OZ_IO_DISK_READBLOCKS: {
      OZ_IO_disk_readblocks  disk_readblocks;

      if (!(devex -> valid)) return (OZ_VOLNOTVALID);
      movc4 (as, ap, sizeof disk_readblocks, &disk_readblocks);
      sts = reloc (devex, disk_readblocks.size, &disk_readblocks.slbn);
      if (sts == OZ_SUCCESS) sts = (*(devex -> hostfunctab -> start)) (devex -> hostdevunit, 
                                                                       devex -> hostdevex, 
                                                                       chnex -> hostiochan, 
                                                                       chnex -> hostiochnex, 
                                                                       procmode, ioop, iopexv, funcode, 
                                                                       sizeof disk_readblocks, &disk_readblocks);
      return (sts);
    }

    case OZ_IO_DISK_WRITEPAGES: {
      OZ_IO_disk_writepages disk_writepages;

      if (!(devex -> valid)) return (OZ_VOLNOTVALID);
      movc4 (as, ap, sizeof disk_writepages, &disk_writepages);
      sts = reloc (devex, disk_writepages.size, &disk_writepages.slbn);
      if (sts == OZ_SUCCESS) sts = (*(devex -> hostfunctab -> start)) (devex -> hostdevunit, 
                                                                       devex -> hostdevex, 
                                                                       chnex -> hostiochan, 
                                                                       chnex -> hostiochnex, 
                                                                       procmode, ioop, iopexv, funcode, 
                                                                       sizeof disk_writepages, &disk_writepages);
      return (sts);
    }
    case OZ_IO_DISK_READPAGES: {
      OZ_IO_disk_readpages disk_readpages;

      if (!(devex -> valid)) return (OZ_VOLNOTVALID);
      movc4 (as, ap, sizeof disk_readpages, &disk_readpages);
      sts = reloc (devex, disk_readpages.size, &disk_readpages.slbn);
      if (sts == OZ_SUCCESS) sts = (*(devex -> hostfunctab -> start)) (devex -> hostdevunit, 
                                                                       devex -> hostdevex, 
                                                                       chnex -> hostiochan, 
                                                                       chnex -> hostiochnex, 
                                                                       procmode, ioop, iopexv, funcode, 
                                                                       sizeof disk_readpages, &disk_readpages);
      return (sts);
    }

    /* Get disk info, part 1 */

    case OZ_IO_DISK_GETINFO1: {
      OZ_IO_disk_getinfo1 disk_getinfo1;

      if (!OZ_HW_WRITABLE (as, ap, procmode)) return (OZ_ACCVIO);
      memset (&disk_getinfo1, 0, sizeof disk_getinfo1);
      disk_getinfo1.blocksize          = devex -> blocksize;	/* size of each block in bytes */
      disk_getinfo1.totalblocks        = devex -> psiz;		/* number of blocks in the partition */
      disk_getinfo1.parthoststartblock = devex -> plbn;		/* starting lbn of partition on host disk */
      strncpyz (disk_getinfo1.parthostdevname, oz_knl_devunit_devname (devex -> hostdevunit), sizeof disk_getinfo1.parthostdevname);
      disk_getinfo1.bufalign           = devex -> bufalign;	/* buffer alignment */
      movc4 (sizeof disk_getinfo1, &disk_getinfo1, as, ap);
      return (OZ_SUCCESS);
    }

    /* Select crash device */

    case OZ_IO_DISK_CRASH: {
      if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);
      if (crash_devex != NULL) {
        oz_knl_devunit_increfc (crash_devunit, -1);
        crash_devex   = NULL;
        crash_devunit = NULL;
      }
      if (ap != NULL) {
        if (as != sizeof (OZ_IO_disk_crash)) return (OZ_BADBUFFERSIZE);
        sts = oz_knl_io (chnex -> hostiochan, OZ_IO_DISK_CRASH, sizeof crash_disk, &crash_disk);
        if (sts != OZ_SUCCESS) {
          oz_knl_printk ("oz_dev_dpar_start crash: error %u getting disk info\n", sts);
          return (sts);
        }
        ((OZ_IO_disk_crash *)ap) -> crashentry = dpar_crash;
        ((OZ_IO_disk_crash *)ap) -> crashparam = NULL;
        ((OZ_IO_disk_crash *)ap) -> blocksize  = devex -> blocksize;
        crash_devex   = devex;
        crash_devunit = devunit;
        oz_knl_devunit_increfc (devunit, 1);
      }
      return (OZ_SUCCESS);
    }

    /* Unknown function code */

    default: {
      return (OZ_BADIOFUNC);
    }
  }
}

/* Write crash dump blocks to partition */

static uLong dpar_crash (void *dummy, OZ_Dbn lbn, uLong size, OZ_Mempage phypage, uLong offset)

{
  uLong sts;

  lbn += crash_devex -> plbn;
  sts  = (*crash_disk.crashentry) (crash_disk.crashparam, lbn, size, phypage, offset);
  return (sts);
}

/* Validate and relocate starting block number according to partitions parameters */

static uLong reloc (Devex *devex, uLong size, OZ_Dbn *slbn)

{
  OZ_Dbn elbn;

  elbn = (size + devex -> blocksize - 1) / devex -> blocksize + *slbn;	/* get ending block number within partition */
  if (elbn < *slbn) return (OZ_BADBLOCKNUMBER);				/* fail if wrapped around */
  if (*slbn >= devex -> psiz) return (OZ_BADBLOCKNUMBER);		/* fail if off end of partition */
  *slbn += devex -> plbn;						/* ok, relocate block number by start of partition */
  return (OZ_SUCCESS);							/* successful */
}
