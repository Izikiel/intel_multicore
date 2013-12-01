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
/*  Generic scsi disk class driver					*/
/*									*/
/*  This driver gets initialized by scsi controller drivers when they 	*/
/*  find new devices connected to their busses.				*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_dev_disk.h"
#include "oz_dev_lio.h"
#include "oz_dev_scsi.h"
#include "oz_io_disk.h"
#include "oz_io_scsi.h"
#include "oz_knl_devio.h"
#include "oz_knl_sdata.h"
#include "oz_knl_status.h"
#include "oz_sys_xprintf.h"

#define BUF2LONG(buf) (((buf)[0] << 24) | ((buf)[1] << 16) | ((buf)[2] << 8) | ((buf)[3]))

#define IOTIMEOUT 15000		// timeout (mS) for a general disk I/O
#define SENSETMOUT 2000		// timeout (mS) for a 'request sense' I/O

typedef struct Devex Devex;
typedef struct Iopex Iopex;

struct Devex { const char *name;		/* disk device's unit name */
               OZ_Iochan *scsiiochan;		/* I/O channel to unit on scsi controller */
               OZ_Liod *liod;			/* layered i/o device context pointer */
               OZ_Dbn totalblocks;		/* capacity of the disk in blocks */
               uLong blocksize;			/* size of disk block in bytes (usually 512 for disk, 2048 for cdrom) */
               int readonly;			/* 0: read/write; 1: readonly */
               int removable;			/* 0: fixed; 1: removable */
               int valid;
               uLong negotiate;			/* optional OZ_IO_SCSI_OPTFLAG_NEGO_WIDTH,_SYNCH flags */
               OZ_IO_scsi_getinfo1 scsi_getinfo1;
             };

struct Iopex { Devex *devex;			/* the devex this request is for */
               OZ_Ioop *ioop;			/* corresponding disk ioop */
               OZ_Procmode procmode;		/* caller's procmode */
               OZ_Lior *lior;			/* layered i/o request context pointer */
               uLong capacityrlen;		/* READ CAPACITY reply length */
               int writethru;			/* 0: write-back; 1: write-thru */
               int writing;			/* 0: reading; 1: writing */
               int startstop;			/* 0: spinning down; 1: spinning up */
               int retries;			/* remaining retries */
               uLong datasize;			/* data buffer size */
               uLong datarlen;			/* data length transferred */
               uByte capacitybuff[8];		/* READ CAPACITY reply data */
               uByte scsists;			/* scsi status byte */
             };

static int scsi_disk_clonedel (OZ_Devunit *devunit, void *devexv, int cloned);
static void scsi_disk_abort (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Ioop *ioop, void *iopexv, OZ_Procmode);
static uLong scsi_disk_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode, 
                              OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap);

static const OZ_Devfunc scsi_disk_functable = { sizeof (Devex), 0, sizeof (Iopex), 0, NULL, NULL, scsi_disk_clonedel, 
                                                NULL, NULL, scsi_disk_abort, scsi_disk_start, NULL };

static int initialized = 0;
static OZ_Devclass *devclass;
static OZ_Devdriver *devdriver;

static Devex *crash_devex = NULL;
static OZ_Devunit *crash_devunit = NULL;
static OZ_IO_scsi_crash crash_scsi;

static OZ_Devunit *scsi_disk_init (void *dummy, OZ_Scsi_init_pb *pb);
static void passthrucomplete (void *iopexv, uLong status);
static uLong startrw (Iopex *iopex, int writing, int writethru, uLong datasize, const OZ_Mempage *dataphypages, uLong databyteoffs, OZ_Dbn slbn);
static void completedrw (void *iopexv, uLong status);
static void validcomplete (void *iopexv, uLong status);
static void startreadcap (Iopex *iopex);
static void capacitycomplete (void *iopexv, uLong status);
static void getinfo1complete (void *iopexv, uLong status);
static uLong decode_status (uLong status, Iopex *iopex, const char *funcdesc);
static void scsiio (Iopex *iopex, OZ_Procmode procmode, void (*astentry) (void *iopexv, uLong status), 
                    uLong funcode, uLong as, void *ap);
static uLong scsi_disk_crash (void *dummy, OZ_Dbn lbn, uLong size, OZ_Mempage phypage, uLong offset);

/************************************************************************/
/*									*/
/*  This routine is called by oz_dev_scsi_init when an new device has 	*/
/*  been found on an scsi bus						*/
/*									*/
/*  If the new device is a disk, it creates a device entry for it and 	*/
/*  enables processing for it						*/
/*									*/
/*    Input:								*/
/*									*/
/*	iochan = i/o channel assigned to controller's device (CW mode)	*/
/*	         it is open on the particular scsi_id (PW mode)		*/
/*	inqlen = length of data in inqbuf				*/
/*	inqbuf = results from inquiry command (0x12)			*/
/*	unit_devname = suggested device unit name			*/
/*	unit_devdesc = suggested device unit description		*/
/*	smp level    = softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	scsi_disk_init = NULL : we can't handle it			*/
/*	                 else : pointer to created devunit		*/
/*									*/
/************************************************************************/

void oz_dev_scsi_disk_init (void)

{
  if (!initialized) {
    initialized = 1;
    devclass  = oz_knl_devclass_create (OZ_IO_DISK_CLASSNAME, OZ_IO_DISK_BASE, OZ_IO_DISK_MASK, "scsi_disk");
    devdriver = oz_knl_devdriver_create (devclass, "scsi_disk");
    oz_dev_scsi_class_add (scsi_disk_init, NULL);
  }
}

static OZ_Devunit *scsi_disk_init (void *dummy, OZ_Scsi_init_pb *pb)

{
  Devex *devex;
  OZ_Devunit *devunit;

  /* If the first inquiry byte is zero, the device is a disk  */
  /* If the first inquiry byte is five, the device is a cdrom */

  if ((pb -> inqbuf[0] != 0) && (pb -> inqbuf[0] != 5)) return (NULL);

  /* Ok, set up a disk device structure for it */

  devunit = oz_knl_devunit_create (devdriver, pb -> unit_devname, pb -> unit_devdesc, &scsi_disk_functable, 0, oz_s_secattr_sysdev);
  devex   = oz_knl_devunit_ex (devunit);
  memset (devex, 0, sizeof *devex);

  devex -> name       = oz_knl_devunit_devname (devunit);			/* save pointer to disk device unitname */
  devex -> liod       = oz_dev_lio_init (pb -> ctrl_iochan);			/* initialize layered i/o routines */
  devex -> scsiiochan = pb -> ctrl_iochan;					/* save scsi i/o channel */
  devex -> readonly   = pb -> inqbuf[0] & 1;					/* save the readonly flag (if cdrom) */
  devex -> removable  = ((pb -> inqbuf[1] & 0x80) != 0);			/* save the removable flag */
  if (pb -> inqbuf[7] & 0x60) devex -> negotiate |= OZ_IO_SCSI_OPTFLAG_NEGO_WIDTH; /* maybe drive can negotiate width */
  if (pb -> inqbuf[7] & 0x10) devex -> negotiate |= OZ_IO_SCSI_OPTFLAG_NEGO_SYNCH; /* maybe drive can negotiate synchronous */

  /* Print a message saying we set it up */

  oz_knl_printk ("oz_dev_scsi_disk: %s %s %s (%s) defined\n", 
	devex -> removable ? "removable" : "fixed", 
	devex -> readonly ? "cdrom" : "disk", 
	pb -> unit_devname, pb -> unit_devdesc);

  /* Set up normal disk autogen on it to set up partitions and filesystems */

  oz_knl_devunit_autogen (devunit, oz_dev_disk_auto, NULL);

  return (devunit);
}

/************************************************************************/
/*									*/
/*  This routine is called when the disk is no inter required (all 	*/
/*  channels have been deassigned).  It releases all resources 		*/
/*  referenced by the devex struct.					*/
/*									*/
/*    Input:								*/
/*									*/
/*	devunit  = disk's devunit					*/
/*	devexv   = devex pointer					*/
/*	cloned   = the cloned flag passed to devunit_create		*/
/*	smplevel = dv							*/
/*									*/
/*    Output:								*/
/*									*/
/*	scsi_disk_clonedel = 1 : it is ok to delete the device		*/
/*									*/
/************************************************************************/

static int scsi_disk_clonedel (OZ_Devunit *devunit, void *devexv, int cloned)

{
  Devex *devex;

  devex = devexv;
  oz_dev_lio_term (devex -> liod);	/* no longer need the layered i/o routines */
  devex -> liod = NULL;			/* remember we terminated them */
  return (1);
}

/************************************************************************/
/*									*/
/*  I/O on a disk channel is being aborted, abort the corresponding 	*/
/*  I/O on the scsi controller						*/
/*									*/
/************************************************************************/

static void scsi_disk_abort (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Ioop *ioop, void *iopexv, OZ_Procmode procmode)

{
  oz_dev_lio_abort (((Devex *)devexv) -> liod, iochan, ioop, procmode);	/* perform abortions */
}

/************************************************************************/
/*									*/
/*  This routine starts a disk I/O function by translating it to the 	*/
/*  corresponding scsi I/O function and queuing it to the scsi 		*/
/*  controller								*/
/*									*/
/************************************************************************/

static uLong scsi_disk_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode, 
                              OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap)

{
  Devex *devex;
  Iopex *iopex;
  uLong sts;

  devex = devexv;
  iopex = iopexv;

  iopex -> devex    = devex;
  iopex -> ioop     = ioop;
  iopex -> procmode = procmode;
  iopex -> lior     = NULL;

  switch (funcode) {

    /* Spin drive up or down */

    case OZ_IO_DISK_SETVOLVALID: {
      OZ_IO_disk_setvolvalid disk_setvolvalid;
      OZ_IO_scsi_doio scsi_doio;
      uByte cmdbuf[6];

      movc4 (as, ap, sizeof disk_setvolvalid, &disk_setvolvalid);

      /* Mark drive no longer valid.  If we're spinning up, it will get marked valid   */
      /* if/when successful.  If we're spinning down, we just leave it marked invalid. */

      devex -> valid       = 0;
      devex -> blocksize   = 0;
      devex -> totalblocks = 0;

      iopex -> startstop = disk_setvolvalid.valid & 1;
      iopex -> scsists   = 0;
      iopex -> retries   = 2;

      /* Removable disks, set up the START STOP UNIT command */

      if (devex -> removable) {
        cmdbuf[0] = 0x1B;
        cmdbuf[1] = 0;
        cmdbuf[2] = 0;
        cmdbuf[3] = 0;
        cmdbuf[4] = (disk_setvolvalid.valid & 1) | ((disk_setvolvalid.unload & 1) << 1);
        cmdbuf[5] = 0;

        memset (&scsi_doio, 0, sizeof scsi_doio);
        scsi_doio.cmdlen   = sizeof cmdbuf;
        scsi_doio.cmdbuf   = cmdbuf;
        scsi_doio.status   = &(iopex -> scsists);
        scsi_doio.optflags = OZ_IO_SCSI_OPTFLAG_DISCONNECT;
        scsi_doio.timeout  = IOTIMEOUT;

        /* Queue the START STOP UNIT request to the scsi controller */

        scsiio (iopex, procmode, validcomplete, OZ_IO_SCSI_DOIO, sizeof scsi_doio, &scsi_doio);
        return (OZ_STARTED);
      }

      /* Fixed disks, if spinning up, read the capacity */

      if (iopex -> startstop) {
        startreadcap (iopex);
        return (OZ_STARTED);
      }

      /* Spinning down a fixed disk, just return as is */

      return (OZ_SUCCESS);
    }

    /* Write blocks to the disk */

    case OZ_IO_DISK_WRITEBLOCKS: {
      const OZ_Mempage *dataphypages;
      OZ_IO_disk_writeblocks disk_writeblocks;
      uLong databyteoffs;

      movc4 (as, ap, sizeof disk_writeblocks, &disk_writeblocks);
      sts = oz_knl_ioop_lockr (ioop, disk_writeblocks.size, disk_writeblocks.buff, &dataphypages, NULL, &databyteoffs);
      if (sts == OZ_SUCCESS) sts = startrw (iopex, 1, disk_writeblocks.writethru & 1, disk_writeblocks.size, dataphypages, databyteoffs, disk_writeblocks.slbn);
      return (sts);
    }

    /* Read blocks from the disk */

    case OZ_IO_DISK_READBLOCKS: {
      const OZ_Mempage *dataphypages;
      OZ_IO_disk_readblocks disk_readblocks;
      uLong databyteoffs;

      movc4 (as, ap, sizeof disk_readblocks, &disk_readblocks);
      sts = oz_knl_ioop_lockw (ioop, disk_readblocks.size, disk_readblocks.buff, &dataphypages, NULL, &databyteoffs);
      if (sts == OZ_SUCCESS) sts = startrw (iopex, 0, 0, disk_readblocks.size, dataphypages, databyteoffs, disk_readblocks.slbn);
      return (sts);
    }

    /* Write physical pages to blocks on the disk */

    case OZ_IO_DISK_WRITEPAGES: {
      OZ_IO_disk_writepages disk_writepages;

      if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);
      movc4 (as, ap, sizeof disk_writepages, &disk_writepages);
      sts = startrw (iopex, 1, disk_writepages.writethru & 1, disk_writepages.size, disk_writepages.pages, disk_writepages.offset, disk_writepages.slbn);
      return (sts);
    }

    /* Read physical pages from blocks on the disk */

    case OZ_IO_DISK_READPAGES: {
      OZ_IO_disk_readpages disk_readpages;

      if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);
      movc4 (as, ap, sizeof disk_readpages, &disk_readpages);
      sts = startrw (iopex, 0, 0, disk_readpages.size, disk_readpages.pages, disk_readpages.offset, disk_readpages.slbn);
      return (sts);
    }

    /* Retrieve geometry info */

    case OZ_IO_DISK_GETINFO1: {
      OZ_IO_disk_getinfo1 disk_getinfo1;

      if (!OZ_HW_WRITABLE (as, ap, procmode)) return (OZ_ACCVIO);

      memset (&disk_getinfo1, 0, sizeof disk_getinfo1);

      disk_getinfo1.blocksize   = devex -> blocksize;
      disk_getinfo1.totalblocks = devex -> totalblocks;

      movc4 (sizeof disk_getinfo1, &disk_getinfo1, as, ap);
      return (OZ_SUCCESS);
    }

    /* Set up crash dump disk */

    case OZ_IO_DISK_CRASH: {
      if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);

      /* Close out any old stuff */

      if (crash_devunit != NULL) {
        oz_knl_devunit_increfc (crash_devunit, -1);
        crash_devunit = NULL;
        crash_devex   = NULL;
      }

      /* Open new stuff */

      if (ap != NULL) {
        if (as != sizeof (OZ_IO_disk_crash)) return (OZ_BADBUFFERSIZE);
        memset (&crash_scsi, 0, sizeof crash_scsi);
        sts = oz_knl_io (devex -> scsiiochan, OZ_IO_SCSI_CRASH, sizeof crash_scsi, &crash_scsi);
        if (sts != OZ_SUCCESS) return (sts);
        crash_devex   = devex;
        crash_devunit = devunit;
        oz_knl_devunit_increfc (crash_devunit, 1);
        ((OZ_IO_disk_crash *)ap) -> crashentry = scsi_disk_crash;
        ((OZ_IO_disk_crash *)ap) -> crashparam = NULL;
        ((OZ_IO_disk_crash *)ap) -> blocksize  = devex -> blocksize;
      }
      return (OZ_SUCCESS);
    }

    /* Pass on SCSI DOIO request as is so they can really screw things up  */
    /* Pass on SCSI GETINFO1 request as is so they can retrieve parameters */

    /* ** DONT DO THIS AS IT CAN LEAD PROGRAMS TO BELIEVE THIS IS A SCSI CONTROLLER ** */

#if 0
    case OZ_IO_SCSI_DOIO:
    case OZ_IO_SCSI_GETINFO1: {
      scsiio (iopex, procmode, passthrucomplete, funcode, as, ap);
      return (OZ_STARTED);
    }
#endif
  }

  return (OZ_BADIOFUNC);
}

/* A passed thru function has completed, just post the original request for completion */

static void passthrucomplete (void *iopexv, uLong status)

{
  oz_dev_lio_done (((Iopex *)iopexv) -> lior, status, NULL, NULL);
}

/************************************************************************/
/*									*/
/*  This routine starts a read or write request				*/
/*									*/
/*    Input:								*/
/*									*/
/*	iopex = i/o request being processed				*/
/*	writing = 0 : this is a read request				*/
/*	          1 : this is a write request				*/
/*	writethru = 0 : disk may perform write to media at any time	*/
/*	            1 : disk shall write to media before returning status
/*	datasize = size of data being read or written			*/
/*	dataphypages = physical page array of data buffer		*/
/*	databyteoffs = byte offset in first physical page		*/
/*	slbn = starting logical block number				*/
/*									*/
/*    Output:								*/
/*									*/
/*	startrw = OZ_STARTED : request successfully queued		*/
/*	                else : error status				*/
/*									*/
/************************************************************************/

static uLong startrw (Iopex *iopex, int writing, int writethru, uLong datasize, const OZ_Mempage *dataphypages, uLong databyteoffs, OZ_Dbn slbn)

{
  Devex *devex;
  OZ_IO_scsi_doiopp scsi_doiopp;
  uByte cmdbuf[10];
  uLong nblocks;

  devex = iopex -> devex;
  iopex -> writethru = writethru;
  iopex -> writing   = writing;
  iopex -> datasize  = datasize;

  if (!(devex -> valid)) return (OZ_VOLNOTVALID);
  if (writing && (devex -> readonly)) return (OZ_WRITELOCKED);

  /* Enforce SCSI restrictions - must do multiples of a blocksize, max of 65535 blocks at a time */

  if (datasize % devex -> blocksize != 0) return (OZ_BADBUFFERSIZE);
  nblocks = datasize / devex -> blocksize;
  if (nblocks > 65535) return (OZ_BADBUFFERSIZE);

  /* General restriction is that we can't go beyond end of disk */

  if (slbn >= devex -> totalblocks) return (OZ_BADBLOCKNUMBER);
  if (slbn + nblocks > devex -> totalblocks) return (OZ_BADBLOCKNUMBER);

  /* Make sure the drive is spun up */

  if (!(devex -> valid)) return (OZ_VOLNOTVALID);

  /* Set up command and I/O block */

  cmdbuf[0] = writing * 2 + 0x28;	/* 28: reading, 2A: writing */
  cmdbuf[1] = writethru * 8;
  cmdbuf[2] = slbn >> 24;
  cmdbuf[3] = slbn >> 16;
  cmdbuf[4] = slbn >>  8;
  cmdbuf[5] = slbn;
  cmdbuf[6] = 0;
  cmdbuf[7] = nblocks >> 8;
  cmdbuf[8] = nblocks;
  cmdbuf[9] = 0;

  memset (&scsi_doiopp, 0, sizeof scsi_doiopp);
  scsi_doiopp.cmdlen       = sizeof cmdbuf;
  scsi_doiopp.cmdbuf       = cmdbuf;
  scsi_doiopp.datasize     = datasize;
  scsi_doiopp.dataphypages = dataphypages;
  scsi_doiopp.databyteoffs = databyteoffs;
  scsi_doiopp.status       = &(iopex -> scsists);
  scsi_doiopp.datarlen     = &(iopex -> datarlen);
  scsi_doiopp.optflags     = writing ? (OZ_IO_SCSI_OPTFLAG_WRITE | OZ_IO_SCSI_OPTFLAG_DISCONNECT) : OZ_IO_SCSI_OPTFLAG_DISCONNECT;
  scsi_doiopp.timeout      = IOTIMEOUT;

  /* Queue the request to the scsi controller */

  scsiio (iopex, OZ_PROCMODE_KNL, completedrw, OZ_IO_SCSI_DOIOPP, sizeof scsi_doiopp, &scsi_doiopp);
  return (OZ_STARTED);
}

/* This routine is called (at softint) when the read/write command has completed */

static void completedrw (void *iopexv, uLong status)

{
  Iopex *iopex;
  uLong sts;

  iopex = iopexv;

  sts = decode_status (status, iopex, iopex -> writing ? "writing" : "reading");
  if ((sts == OZ_SUCCESS) && (iopex -> datarlen != iopex -> datasize)) {
    oz_knl_printk ("oz_dev_disk_scsi completedrw: only %s %u bytes instead of %u\n", 
	iopex -> writing ? "wrote" : "read", iopex -> datarlen, iopex -> datasize);
    sts = OZ_IOFAILED;
  }
  oz_dev_lio_done (iopex -> lior, sts, NULL, NULL);
}

/************************************************************************/
/*									*/
/*  These routines finish up setvolvalid requests			*/
/*									*/
/************************************************************************/

/* This routine is called (at softint) when the START STOP command has completed */

static void validcomplete (void *iopexv, uLong status)

{
  Devex *devex;
  Iopex *iopex;
  OZ_IO_scsi_doio scsi_doio;
  uByte cmdbuf[6];
  uLong sts;

  iopex = iopexv;
  devex = iopex -> devex;

  sts = decode_status (status, iopex, iopex -> startstop ? "spinning up" : "spinning down");
  if (iopex -> startstop) {
    if (sts == OZ_SUCCESS) {

      /* Drive is all spun up, read its capacity (also perform width/speed negotiation) */

      startreadcap (iopex);
      return;
    }
    if (-- (iopex -> retries) >= 0) {
      cmdbuf[0] = 0x1B;
      cmdbuf[1] = 0;
      cmdbuf[2] = 0;
      cmdbuf[3] = 0;
      cmdbuf[4] = 1;
      cmdbuf[5] = 0;

      memset (&scsi_doio, 0, sizeof scsi_doio);
      scsi_doio.cmdlen   = sizeof cmdbuf;
      scsi_doio.cmdbuf   = cmdbuf;
      scsi_doio.status   = &(iopex -> scsists);
      scsi_doio.optflags = OZ_IO_SCSI_OPTFLAG_DISCONNECT;
      scsi_doio.timeout  = IOTIMEOUT;

      /* Queue the START STOP UNIT request to the scsi controller */

      scsiio (iopex, iopex -> procmode, validcomplete, OZ_IO_SCSI_DOIO, sizeof scsi_doio, &scsi_doio);
      return;
    }
  }
  oz_dev_lio_done (iopex -> lior, sts, NULL, NULL);
}

/* Start reading capacity of drive */

static void startreadcap (Iopex *iopex)

{
  Devex *devex;
  OZ_IO_scsi_doio scsi_doio;
  uByte cmdbuf[10];

  devex = iopex -> devex;

  cmdbuf[0] = 0x25;
  cmdbuf[1] = 0;
  cmdbuf[2] = 0;
  cmdbuf[3] = 0;
  cmdbuf[4] = 0;
  cmdbuf[5] = 0;
  cmdbuf[6] = 0;
  cmdbuf[7] = 0;
  cmdbuf[8] = 0;
  cmdbuf[9] = 0;

  memset (&scsi_doio, 0, sizeof scsi_doio);
  scsi_doio.cmdlen   = sizeof cmdbuf;
  scsi_doio.cmdbuf   = cmdbuf;
  scsi_doio.datasize = sizeof iopex -> capacitybuff;
  scsi_doio.databuff = iopex -> capacitybuff;
  scsi_doio.status   = &(iopex -> scsists);
  scsi_doio.datarlen = &(iopex -> capacityrlen);
  scsi_doio.optflags = OZ_IO_SCSI_OPTFLAG_DISCONNECT | devex -> negotiate;
  scsi_doio.timeout  = IOTIMEOUT;

  scsiio (iopex, OZ_PROCMODE_KNL, capacitycomplete, OZ_IO_SCSI_DOIO, sizeof scsi_doio, &scsi_doio);
}

/* This routine is called (at softint) when the READ CAPACITY command has completed */

static void capacitycomplete (void *iopexv, uLong status)

{
  Devex *devex;
  Iopex *iopex;
  uLong sts;
  uQuad totalmegs;

  iopex = iopexv;
  devex = iopex -> devex;

  sts = decode_status (status, iopex, "reading capacity");
  if ((sts == OZ_SUCCESS) && (iopex -> capacityrlen != sizeof iopex -> capacitybuff)) {
    sts = OZ_IOFAILED;
    oz_knl_printk ("oz_dev_scsi_disk: only read %u of %u bytes of %s capacity\n", 
                   iopex -> capacityrlen, sizeof iopex -> capacitybuff, devex -> name);
  }
  if (sts == OZ_SUCCESS) {
    devex -> totalblocks = BUF2LONG (iopex -> capacitybuff + 0);
    devex -> blocksize   = BUF2LONG (iopex -> capacitybuff + 4);
    if (devex -> blocksize == 2352) {
      oz_knl_printk ("oz_dev_scsi_disk: %s reports blocksize 2352, but using 2048\n", devex -> name);
      devex -> blocksize = 2048; // hack for some CDROM drives that report audio block size
    }
    if ((devex -> blocksize == 0) || (devex -> blocksize > (1 << OZ_HW_L2PAGESIZE)) || (((1 << OZ_HW_L2PAGESIZE) % devex -> blocksize) != 0)) {
      oz_knl_printk ("oz_dev_scsi_disk: %s blocksize %u invalid\n", devex -> name, devex -> blocksize);
      sts = OZ_BADBLOCKSIZE;
    } else {
      totalmegs   = ((uQuad)(devex -> totalblocks)) * ((uQuad)(devex -> blocksize));
      totalmegs >>= 20;
      oz_knl_printk ("oz_dev_scsi_disk: %s has %u blocks of %u bytes (%u Meg)\n", 
                     devex -> name, devex -> totalblocks, devex -> blocksize, (uLong)totalmegs);

      memset (&(devex -> scsi_getinfo1), 0, sizeof devex -> scsi_getinfo1);
      scsiio (iopex, OZ_PROCMODE_KNL, getinfo1complete, OZ_IO_SCSI_GETINFO1, 
              sizeof devex -> scsi_getinfo1, &(devex -> scsi_getinfo1));
      return;
    }
  } else if (-- (iopex -> retries) >= 0) {
    startreadcap (iopex);
    return;
  }
  oz_dev_lio_done (iopex -> lior, sts, NULL, NULL);
}

/* This routine is called (at softint) when the SCSI_GETINFO1 has completed */

static void getinfo1complete (void *iopexv, uLong status)

{
  char msgbuf[64];
  Devex *devex;
  int i;
  Iopex *iopex;

  iopex = iopexv;
  devex = iopex -> devex;

  if (status != OZ_SUCCESS) oz_knl_printk ("oz_dev_scsi_disk: %s error %u getting width/speed\n", devex -> name, status);
  else {
    oz_sys_sprintf (sizeof msgbuf, msgbuf, "width %u bits", 8 << devex -> scsi_getinfo1.open_width);
    if (devex -> scsi_getinfo1.open_speed != 0) {
      i = strlen (msgbuf);
      oz_sys_sprintf (sizeof msgbuf - i, msgbuf + i, ", speed %u Mbytes/sec", (250 / devex -> scsi_getinfo1.open_speed) << devex -> scsi_getinfo1.open_width);
    }
    if (devex -> scsi_getinfo1.open_raofs == 0) strcat (msgbuf, ", async");
    else {
      i = strlen (msgbuf);
      oz_sys_sprintf (sizeof msgbuf - i, msgbuf + i, ", sync %u", devex -> scsi_getinfo1.open_raofs);
    }
    oz_knl_printk ("oz_dev_scsi_disk: %s %s\n", devex -> name, msgbuf);
  }

  devex -> valid = 1;
  oz_dev_lio_done (iopex -> lior, OZ_SUCCESS, NULL, NULL);
}

/************************************************************************/
/*									*/
/*  Decode status from the operation, doing a request sense if needed	*/
/*									*/
/*    Input:								*/
/*									*/
/*	status = I/O completion status					*/
/*	iopex -> scsists = scsi operation status byte			*/
/*	funcdesc = string describing function that was just done	*/
/*									*/
/*    Output:								*/
/*									*/
/*	decode_status = possibly modified status			*/
/*									*/
/************************************************************************/

static uLong decode_status (uLong status, Iopex *iopex, const char *funcdesc)

{
  Devex *devex;
  OZ_IO_scsi_doio scsi_doio;
  uByte cmdbuf[6], key, scsists, sensebuf[24], sensests;
  uLong senselen, sts;

  static const char *msgtable[16] = { "no error", "recovered error", "not ready", "medium error", "hardware error", 
                                      "illegal request", "unit attention", "data protect", "blank check", 
                                      "vendor specific condition", "copy aborted", "aborted command", "obsolete condition", 
                                      "volume overflow", "miscompare", "reserved condition" };
  static const uLong ststable[16] = {  OZ_SUCCESS, OZ_SUCCESS,        OZ_VOLNOTVALID, OZ_BADMEDIA, OZ_IOFAILED, 
                                       OZ_IOFAILED,       OZ_VOLNOTVALID,   OZ_WRITELOCKED, OZ_WRITELOCKED, 
                                       OZ_IOFAILED,                 OZ_IOFAILED,    OZ_IOFAILED,       OZ_IOFAILED, 
                                       OZ_IOFAILED,       OZ_BADMEDIA,  OZ_IOFAILED };

  sts = status;
  if (sts == OZ_SUCCESS) {
    scsists = iopex -> scsists & 0x3E;
    if (scsists != 0x00) {
      devex = iopex -> devex;
      if (scsists != 0x02) {
        sts = OZ_IOFAILED;
        oz_knl_printk ("oz_dev_scsi_disk: scsi error 0x%2.2x %s %s\n", iopex -> scsists, funcdesc, devex -> name);
      } else {
        memset (sensebuf, 0, sizeof sensebuf);
        cmdbuf[0] = 0x03;
        cmdbuf[1] = 0;
        cmdbuf[2] = 0;
        cmdbuf[3] = 0;
        cmdbuf[4] = sizeof sensebuf;
        cmdbuf[5] = 0;

        memset (&scsi_doio, 0, sizeof scsi_doio);
        scsi_doio.cmdlen   = sizeof cmdbuf;
        scsi_doio.cmdbuf   = cmdbuf;
        scsi_doio.datasize = sizeof sensebuf;
        scsi_doio.databuff = sensebuf;
        scsi_doio.datarlen = &senselen;
        scsi_doio.status   = &sensests;
        scsi_doio.timeout  = SENSETMOUT;

        sts = oz_knl_io (devex -> scsiiochan, OZ_IO_SCSI_DOIO, sizeof scsi_doio, &scsi_doio);

        if (sts != OZ_SUCCESS) {
          oz_knl_printk ("oz_dev_scsi_disk: error %u requesting sense of %s\n", sts, devex -> name);
        } else if ((sensests & 0x3E) != 0) {
          oz_knl_printk ("oz_dev_scsi_disk: scsi sts %2.2x requesting sense of %s\n", sensests, devex -> name);
          sts = OZ_IOFAILED;
        } else if ((sensebuf[0] & 0x7E) != 0x70) {
          oz_knl_printk ("oz_dev_scsi_disk: request sense[0] %2.2x from %x\n", sensebuf[0], devex -> name);
          oz_knl_dumpmem (senselen, sensebuf);
          sts = OZ_IOFAILED;
        } else {
          key = sensebuf[2] & 0x0F;
          oz_knl_printk ("oz_dev_scsi_disk: %s %s %s\n", msgtable[key], funcdesc, devex -> name);
          oz_knl_printk ("oz_dev_scsi_disk: - key %x, asc %x, ascq %x\n", key, sensebuf[12], sensebuf[13]);
          sts = ststable[key];
          if (sts == OZ_VOLNOTVALID) devex -> valid = 0;
        }
      }
    }
  }
  return (sts);
}

/************************************************************************/
/*									*/
/*  Start an I/O on the scsi controller					*/
/*									*/
/*    Input:								*/
/*									*/
/*	iopex = pointer to disk's iopex					*/
/*	procmode = processor mode of the request			*/
/*	astentry = completion routine to be called			*/
/*	funcode = function code to be performed				*/
/*	as,ap = arg block size and pointer				*/
/*									*/
/*    Output:								*/
/*									*/
/*	astentry will be called when operation completes		*/
/*									*/
/************************************************************************/

static void scsiio (Iopex *iopex, OZ_Procmode procmode, void (*astentry) (void *iopexv, uLong status), 
                    uLong funcode, uLong as, void *ap)

{
  /* Make sure there is a 'lior' context set up for this request */

  if (iopex -> lior == NULL) iopex -> lior = oz_dev_lio_start (iopex -> devex -> liod, iopex -> ioop, iopex -> procmode);

  /* Start the i/o on the scsi controller */

  oz_dev_lio_io (iopex -> lior, procmode, astentry, iopex, funcode, as, ap);
}

/************************************************************************/
/*									*/
/*  Crash dump routine - write logical blocks with interrupts disabled	*/
/*									*/
/*    Input:								*/
/*									*/
/*	lbn = block to start writing at					*/
/*	size = number of bytes to write (multiple of blocksize)		*/
/*	phypage = physical page to start writing from			*/
/*	offset = offset in first physical page				*/
/*									*/
/*    Output:								*/
/*									*/
/*	scsi_disk_crash = OZ_SUCCESS : successful			*/
/*	                        else : error status			*/
/*									*/
/************************************************************************/

static uLong scsi_disk_crash (void *dummy, OZ_Dbn lbn, uLong size, OZ_Mempage phypage, uLong offset)

{
  int retries;
  OZ_IO_scsi_doiopp scsi_doiopp;
  OZ_Mempage senphypages[2];
  uByte cmdbuf[10], scsists, senbuf[24];
  uLong datarlen, nblocks, sts;

  /* Data size must be a multiple of block size */

  if (size % crash_devex -> blocksize != 0) {
    oz_knl_printk ("oz_dev_scsi_disk crash: buffer size %u not multiple of block size %u\n", size, crash_devex -> blocksize);
    return (OZ_BADBUFFERSIZE);
  }

  /* Also, buffer must be contained within a single physical page */

  if (offset + size > (1 << OZ_HW_L2PAGESIZE)) {
    oz_knl_printk ("oz_dev_scsi_disk crash: buffer size %u at offset %u overflows page\n", size, offset);
    return (OZ_BADBUFFERSIZE);
  }

  /* Make sure the drive is spun up */

  if (!(crash_devex -> valid)) return (OZ_VOLNOTVALID);

  /* Set up command and I/O block */

  nblocks = size / crash_devex -> blocksize;
  retries = 2;

retry:
  cmdbuf[0] = 0x2A;
  cmdbuf[1] = 0;
  cmdbuf[2] = lbn >> 24;
  cmdbuf[3] = lbn >> 16;
  cmdbuf[4] = lbn >>  8;
  cmdbuf[5] = lbn;
  cmdbuf[6] = 0;
  cmdbuf[7] = nblocks >> 8;
  cmdbuf[8] = nblocks;
  cmdbuf[9] = 0;

  memset (&scsi_doiopp, 0, sizeof scsi_doiopp);
  scsi_doiopp.cmdlen       = 10;
  scsi_doiopp.cmdbuf       = cmdbuf;
  scsi_doiopp.datasize     = size;
  scsi_doiopp.dataphypages = &phypage;
  scsi_doiopp.databyteoffs = offset;
  scsi_doiopp.status       = &scsists;
  scsi_doiopp.datarlen     = &datarlen;
  scsi_doiopp.optflags     = OZ_IO_SCSI_OPTFLAG_WRITE;
  scsi_doiopp.timeout      = IOTIMEOUT;

  /* Tell scsi controller to do it and return status */

  sts = (*(crash_scsi.crashentry)) (crash_scsi.crashparam, &scsi_doiopp);

  /* Analyse scsi status byte - we shouldn't get things like unit attn because */
  /* the drive was online with crash file open, etc, so we know it was spun up */

  if ((sts == OZ_SUCCESS) && ((scsists & 0x3E) != 0)) {
    oz_knl_printk ("oz_dev_scsi_disk crash: scsi status 0x%x writing lbn %u\n", scsists, lbn);
    if ((scsists & 0x3E) == 0x02) {
      cmdbuf[0] = 0x03;
      cmdbuf[1] = 0;
      cmdbuf[2] = 0;
      cmdbuf[3] = 0;
      cmdbuf[4] = sizeof senbuf;
      cmdbuf[5] = 0;
      memset (&scsi_doiopp, 0, sizeof scsi_doiopp);
      scsi_doiopp.cmdlen       = 6;
      scsi_doiopp.cmdbuf       = cmdbuf;
      scsi_doiopp.datasize     = sizeof senbuf;
      scsi_doiopp.status       = &scsists;
      scsi_doiopp.datarlen     = &datarlen;
      scsi_doiopp.dataphypages = senphypages;
      scsi_doiopp.timeout      = SENSETMOUT;
      sts = oz_knl_misc_sva2pa (senbuf, senphypages + 0, &scsi_doiopp.databyteoffs);
      if (sts < 24) oz_knl_misc_sva2pa (senbuf + sts, senphypages + 1, NULL);
      sts = (*(crash_scsi.crashentry)) (crash_scsi.crashparam, &scsi_doiopp);
      if (sts != OZ_SUCCESS) {
        oz_knl_printk ("oz_dev_scsi_disk crash: error %u requesting sense\n", sts);
        crash_devex -> valid = 0;
        return (sts);
      }
      oz_knl_printk ("oz_dev_scsi_disk crash: - key %x, asc %x, ascq %x\n", senbuf[2] & 0x0F, senbuf[12], senbuf[13]);
      if (-- retries >= 0) goto retry;
    }
    crash_devex -> valid = 0;
    sts = OZ_IOFAILED;
  }

  /* Make sure it wrote the whole thing */

  if ((sts == OZ_SUCCESS) && (datarlen != size)) {
    oz_knl_printk ("oz_dev_scsi_disk crash: only wrote %u bytes of %u to lbn %u\n", datarlen, size, lbn);
    crash_devex -> valid = 0;
    sts = OZ_IOFAILED;
  }

  return (sts);
}
