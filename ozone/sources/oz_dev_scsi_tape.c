//+++2002-05-10
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
//---2002-05-10

/************************************************************************/
/*									*/
/*  Generic scsi tape class driver					*/
/*									*/
/*  This driver gets initialized by scsi controller drivers when they 	*/
/*  find new devices connected to their busses.				*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_dev_lio.h"
#include "oz_dev_scsi.h"
#include "oz_io_scsi.h"
#include "oz_io_tape.h"
#include "oz_knl_devio.h"
#include "oz_knl_sdata.h"
#include "oz_knl_status.h"
#include "oz_sys_xprintf.h"

#define BUF2LONG(buf) (((buf)[0] << 24) | ((buf)[1] << 16) | ((buf)[2] << 8) | ((buf)[3]))

typedef struct Devex Devex;
typedef struct Iopex Iopex;

struct Devex { const char *name;		/* tape device's unit name */
               OZ_Iochan *scsiiochan;		/* I/O channel to unit on scsi controller */
               OZ_Liod *liod;			/* layered i/o device context pointer */
               int readonly;			/* set if media is write-locked */
               int valid;
             };

struct Iopex { Devex *devex;			/* the devex this request is for */
               OZ_Ioop *ioop;			/* corresponding tape ioop */
               OZ_Procmode procmode;		/* caller's procmode */
               OZ_Lior *lior;			/* layered i/o request context pointer */
               uLong capacityrlen;		/* READ CAPACITY reply length */
               int writing;			/* 0: reading; 1: writing */
               int startstop;			/* 0: spinning down; 1: spinning up */
               int files;			/* 0: skipping blocks; 1: skipping filemarks */
               int count;			/* number of blocks/filemarks being skipped */
               uLong datasize;			/* data buffer size */
               uLong datarlen;			/* data length transferred */
               uByte tempbuff[20];		/* temp buffer used for various things */
               uByte scsists;			/* scsi status byte */
             };

static int scsi_tape_clonedel (OZ_Devunit *devunit, void *devexv, int cloned);
static void scsi_tape_abort (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Ioop *ioop, void *iopexv, OZ_Procmode);
static uLong scsi_tape_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode, 
                              OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap);

static const OZ_Devfunc scsi_tape_functable = { sizeof (Devex), 0, sizeof (Iopex), 0, NULL, NULL, scsi_tape_clonedel, 
                                                NULL, NULL, scsi_tape_abort, scsi_tape_start, NULL };

static int initialized = 0;
static OZ_Devclass *devclass;
static OZ_Devdriver *devdriver;

static void passthrucomplete (void *iopexv, uLong status);
static uLong startrw (Iopex *iopex, int writing, uLong datasize, const OZ_Mempage *dataphypages, uLong databyteoffs, OZ_Dbn slbn);
static void completedrw (void *iopexv, uLong status);
static void storerlen (void *iopexv, int finok, uLong *status_r);
static void validcomplete (void *iopexv, uLong status);
static void startreadcap (Iopex *iopex);
static void capacitycomplete (void *iopexv, uLong status);
static uLong decode_status (uLong status, Iopex *iopex, const char *funcdesc);
static void scsiio (Iopex *iopex, OZ_Procmode procmode, void (*astentry) (void *iopexv, uLong status), 
                    uLong funcode, uLong as, void *ap);

/************************************************************************/
/*									*/
/*  This routine is called by oz_dev_scsi_init when an new device has 	*/
/*  been found on an scsi bus						*/
/*									*/
/*  If the new device is a tape, it creates a device entry for it and 	*/
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
/*	oz_dev_scsi_tape_init = NULL : we can't handle it		*/
/*	                        else : pointer to created devunit	*/
/*									*/
/************************************************************************/

OZ_Devunit *oz_dev_scsi_tape_init (OZ_Scsi_init_pb *pb)

{
  Devex *devex;
  OZ_Devunit *devunit;

  /* If the first inquiry byte is one, the device is a tape */

  if (pb -> inqbuf[0] != 1) return (NULL);

  /* Ok, set up a tape device structure for it */

  if (!initialized) {
    devclass  = oz_knl_devclass_create (OZ_IO_TAPE_CLASSNAME, OZ_IO_TAPE_BASE, OZ_IO_TAPE_MASK, "scsi_tape");
    devdriver = oz_knl_devdriver_create (devclass, "scsi_tape");
    initialized = 1;
  }

  devunit = oz_knl_devunit_create (devdriver, pb -> unit_devname, pb -> unit_devdesc, &scsi_tape_functable, 0, oz_s_secattr_sysdev);
  devex   = oz_knl_devunit_ex (devunit);
  memset (devex, 0, sizeof *devex);

  devex -> name       = oz_knl_devunit_devname (devunit);			/* save pointer to tape device unitname */
  devex -> liod       = oz_dev_lio_init (pb -> ctrl_iochan);			/* initialize layered i/o routines */
  devex -> scsiiochan = pb -> ctrl_iochan;					/* save scsi i/o channel */

  /* Print a message saying we set it up */

  oz_knl_printk ("oz_dev_scsi_tape: %s (%s) defined\n", pb -> unit_devname, pb -> unit_devdesc);

#if 000 // don't have one for tapes
  /* Set up normal tape autogen on it to set up partitions and filesystems */

  oz_knl_devunit_autogen (devunit, oz_dev_tape_auto, NULL);
#endif

  return (devunit);
}

/************************************************************************/
/*									*/
/*  This routine is called when the tape is no inter required (all 	*/
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
/*	scsi_tape_clonedel = 1 : it is ok to delete the device		*/
/*									*/
/************************************************************************/

static int scsi_tape_clonedel (OZ_Devunit *devunit, void *devexv, int cloned)

{
  Devex *devex;

  devex = devexv;
  oz_dev_lio_term (devex -> liod);	/* no longer need the layered i/o routines */
  devex -> liod = NULL;			/* remember we terminated them */
  return (1);
}

/************************************************************************/
/*									*/
/*  I/O on a tape channel is being aborted, abort the corresponding 	*/
/*  I/O on the scsi controller						*/
/*									*/
/************************************************************************/

static void scsi_tape_abort (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Ioop *ioop, void *iopexv, OZ_Procmode procmode)

{
  oz_dev_lio_abort (((Devex *)devexv) -> liod, iochan, ioop, procmode);	/* perform abortions */
}

/************************************************************************/
/*									*/
/*  This routine starts a tape I/O function by translating it to the 	*/
/*  corresponding scsi I/O function and queuing it to the scsi 		*/
/*  controller								*/
/*									*/
/************************************************************************/

static uLong scsi_tape_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode, 
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

    /* Load/Unload tape */

    case OZ_IO_TAPE_SETVOLVALID: {
      OZ_IO_tape_setvolvalid tape_setvolvalid;
      OZ_IO_scsi_doio scsi_doio;
      uByte cmdbuf[6];

      movc4 (as, ap, sizeof tape_setvolvalid, &tape_setvolvalid);

      /* Mark drive no longer valid.  If we're loading, it will get marked valid   */
      /* if/when successful.  If we're unloading, we just leave it marked invalid. */

      devex -> valid       = 0;
      devex -> blocksize   = 0;
      devex -> totalblocks = 0;

      iopex -> startstop = tape_setvolvalid.valid & 1;
      iopex -> scsists   = 0;

      /* Set up the REWIND/LOAD/UNLOAD command.                                                       */
      /* The REWIND/UNLOAD commands return 'immediately', ie, before the rewind is actually complete. */
      /* If you want to wait, issue a standard REWIND command first.                                  */

      switch (((tape_setvolvalid.valid & 1) << 1) | (tape_setvolvalid.unload & 1)) {

        /* rewind without unloading */

        case 0: {
          cmdbuf[0] = 0x01;
          cmdbuf[1] = 0x01;
          cmdbuf[2] = 0;
          cmdbuf[3] = 0;
          cmdbuf[4] = 0;
          cmdbuf[5] = 0;
          break;
        }

        /* rewind and unload */

        case 1: {
          cmdbuf[0] = 0x1B;
          cmdbuf[1] = 0x01;
          cmdbuf[2] = 0;
          cmdbuf[3] = 0;
          cmdbuf[4] = 0;
          cmdbuf[5] = 0;
          break;
        }

        /* load */

        case 2:
        case 3: {
          cmdbuf[0] = 0x1B;
          cmdbuf[1] = 0;
          cmdbuf[2] = 0;
          cmdbuf[3] = 0;
          cmdbuf[4] = 0x01;
          cmdbuf[5] = 0;
          break;
        }
      }

      memset (&scsi_doio, 0, sizeof scsi_doio);
      scsi_doio.cmdlen   = sizeof cmdbuf;
      scsi_doio.cmdbuf   = cmdbuf;
      scsi_doio.status   = &(iopex -> scsists);
      scsi_doio.optflags = OZ_IO_SCSI_OPTFLAG_DISCONNECT;

      /* Queue the REWIND/LOAD/UNLOAD request to the scsi controller */

      scsiio (iopex, procmode, validcomplete, OZ_IO_SCSI_DOIO, sizeof scsi_doio, &scsi_doio);
      return (OZ_STARTED);
    }

    /* Write block to the tape */

    case OZ_IO_TAPE_WRITEBLOCK: {
      const OZ_Mempage *dataphypages;
      OZ_IO_tape_writeblock tape_writeblock;
      uLong databyteoffs;

      movc4 (as, ap, sizeof tape_writeblock, &tape_writeblock);
      sts = oz_knl_ioop_lockr (ioop, tape_writeblock.size, tape_writeblock.buff, &dataphypages, NULL, &databyteoffs);
      iopex -> callerrlen = NULL;
      if (sts == OZ_SUCCESS) sts = startrw (iopex, 1, tape_writeblock.size, dataphypages, databyteoffs, tape_writeblock.slbn);
      return (sts);
    }

    /* Read block from the tape */

    case OZ_IO_TAPE_READBLOCK: {
      const OZ_Mempage *dataphypages;
      OZ_IO_tape_readblock tape_readblock;
      uLong databyteoffs;

      movc4 (as, ap, sizeof tape_readblock, &tape_readblock);
      sts = oz_knl_ioop_lockw (ioop, tape_readblock.size, tape_readblock.buff, &dataphypages, NULL, &databyteoffs);
      iopex -> callerrlen = tape_readblock.rlen;
      if (sts == OZ_SUCCESS) sts = startrw (iopex, 0, tape_readblock.size, dataphypages, databyteoffs, tape_readblock.slbn);
      return (sts);
    }

    /* Retrieve geometry info */

    case OZ_IO_TAPE_GETINFO1: {
      OZ_IO_tape_getinfo1 tape_getinfo1;

      if (!OZ_HW_WRITABLE (as, ap, procmode)) return (OZ_ACCVIO);

      memset (&tape_getinfo1, 0, sizeof tape_getinfo1);

      tape_getinfo1.blocksize   = devex -> blocksize;
      tape_getinfo1.totalblocks = devex -> totalblocks;

      movc4 (sizeof tape_getinfo1, &tape_getinfo1, as, ap);
      return (OZ_SUCCESS);
    }

    /* Rewind tape */

    case OZ_IO_TAPE_REWIND: {
      OZ_IO_scsi_doio scsi_doio;
      OZ_IO_tape_rewind tape_rewind;
      uByte cmdbuf[6];

      if (!(devex -> valid)) return (OZ_VOLNOTVALID);

      movc4 (as, ap, sizeof tape_rewind, &tape_rewind);

      iopex -> scsists = 0;

      cmdbuf[0] = 0x01;
      cmdbuf[1] = tape_rewind.immed & 1;
      cmdbuf[2] = 0;
      cmdbuf[3] = 0;
      cmdbuf[4] = 0;
      cmdbuf[5] = 0;

      memset (&scsi_doio, 0, sizeof scsi_doio);
      scsi_doio.cmdlen   = sizeof cmdbuf;
      scsi_doio.cmdbuf   = cmdbuf;
      scsi_doio.status   = &(iopex -> scsists);
      scsi_doio.optflags = OZ_IO_SCSI_OPTFLAG_DISCONNECT;

      scsiio (iopex, procmode, rewindcomplete, OZ_IO_SCSI_DOIO, sizeof scsi_doio, &scsi_doio);
      return (OZ_STARTED);
    }

    /* Skip records/files */

    case OZ_IO_TAPE_SKIP: {
      OZ_IO_scsi_doio scsi_doio;
      OZ_IO_tape_skip tape_skip;
      uByte cmdbuf[6];

      if (!(devex -> valid)) return (OZ_VOLNOTVALID);

      movc4 (as, ap, sizeof tape_skip, &tape_skip);

      iopex -> scsists = 0;
      iopex -> files   = tape_skip.files;
      iopex -> count   = tape_skip.count;

      cmdbuf[0] = 0x11;
      cmdbuf[1] = tape_skip.files & 1;
      cmdbuf[2] = tape_skip.count >> 16;
      cmdbuf[3] = tape_skip.count >>  8;
      cmdbuf[4] = tape_skip.count;
      cmdbuf[5] = 0;

      memset (&scsi_doio, 0, sizeof scsi_doio);
      scsi_doio.cmdlen   = sizeof cmdbuf;
      scsi_doio.cmdbuf   = cmdbuf;
      scsi_doio.status   = &(iopex -> scsists);
      scsi_doio.optflags = OZ_IO_SCSI_OPTFLAG_DISCONNECT;

      scsiio (iopex, procmode, skipcomplete, OZ_IO_SCSI_DOIO, sizeof scsi_doio, &scsi_doio);
      return (OZ_STARTED);
    }

    /* Write filemark */

    case OZ_IO_TAPE_WRITEMARK: {
      OZ_IO_scsi_doio scsi_doio;
      OZ_IO_tape_writemark tape_writemark;
      uByte cmdbuf[6];

      if (!(devex -> valid)) return (OZ_VOLNOTVALID);
      if (!(devex -> readonly)) return (OZ_WRITELOCKED);

      movc4 (as, ap, sizeof tape_writemark, &tape_writemark);

      iopex -> scsists = 0;
      iopex -> count   = tape_writemark.endoftape & 1;

      cmdbuf[0] = 0x10;
      cmdbuf[1] = 0;
      cmdbuf[2] = 0;
      cmdbuf[3] = 0;
      cmdbuf[4] = 1 + iopex -> count;
      cmdbuf[5] = 0;

      memset (&scsi_doio, 0, sizeof scsi_doio);
      scsi_doio.cmdlen   = sizeof cmdbuf;
      scsi_doio.cmdbuf   = cmdbuf;
      scsi_doio.status   = &(iopex -> scsists);
      scsi_doio.optflags = OZ_IO_SCSI_OPTFLAG_DISCONNECT;

      scsiio (iopex, procmode, writemarkcomplete, OZ_IO_SCSI_DOIO, sizeof scsi_doio, &scsi_doio);
      return (OZ_STARTED);
    }

    /* Get tape position */

    case OZ_IO_TAPE_GETPOS: {
      OZ_IO_scsi_doio scsi_doio;
      OZ_IO_tape_getpos tape_getpos;
      uByte cmdbuf[10];

      if (!(devex -> valid)) return (OZ_VOLNOTVALID);

      movc4 (as, ap, sizeof tape_getpos, &tape_getpos);
      if (tape_getpos.tappossiz != 4) return (OZ_BADBUFFERSIZE);

      iopex -> scsists    = 0;
      iopex -> callerrlen = tape_getpos.tapposbuf;

      cmdbuf[0] = 0x34;
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
      scsi_doio.datasize = 20;
      scsi_doio.databuff = iopex -> tempbuff;
      scsi_doio.status   = &(iopex -> scsists);
      scsi_doio.datarlen = &(iopex -> datarlen);
      scsi_doio.optflags = OZ_IO_SCSI_OPTFLAG_DISCONNECT;

      if (sizeof iopex -> tempbuff < 20) oz_crash ("oz_dev_tape_scsi: sizeof tempbuff must be .ge. 20");

      scsiio (iopex, procmode, getposcomplete, OZ_IO_SCSI_DOIO, sizeof scsi_doio, &scsi_doio);
      return (OZ_STARTED);
    }

    /* Set tape position */

    case OZ_IO_TAPE_SETPOS: {
      OZ_IO_scsi_doio scsi_doio;
      OZ_IO_tape_setpos tape_setpos;
      uByte cmdbuf[10];

      if (!(devex -> valid)) return (OZ_VOLNOTVALID);

      movc4 (as, ap, sizeof tape_setpos, &tape_setpos);
      if (tape_setpos.tappossiz != 4) return (OZ_BADBUFFERSIZE);
      sts = oz_knl_section_uget (procmode, 4, tape_setpos.tapposbuf, cmdbuf + 3);
      if (sts != OZ_SUCCESS) return (sts);

      iopex -> scsists = 0;

      cmdbuf[0] = 0x2B;
      cmdbuf[1] = 0;
      cmdbuf[2] = 0;
      cmdbuf[7] = 0;
      cmdbuf[8] = 0;
      cmdbuf[9] = 0;

      memset (&scsi_doio, 0, sizeof scsi_doio);
      scsi_doio.cmdlen   = sizeof cmdbuf;
      scsi_doio.cmdbuf   = cmdbuf;
      scsi_doio.status   = &(iopex -> scsists);
      scsi_doio.optflags = OZ_IO_SCSI_OPTFLAG_DISCONNECT;

      scsiio (iopex, procmode, setposcomplete, OZ_IO_SCSI_DOIO, sizeof scsi_doio, &scsi_doio);
      return (OZ_STARTED);
    }

    /* Pass on SCSI DOIO request as is so they can really screw things up  */
    /* Pass on SCSI GETINFO1 request as is so they can retrieve parameters */

    case OZ_IO_SCSI_DOIO:
    case OZ_IO_SCSI_GETINFO1: {
      scsiio (iopex, procmode, passthrucomplete, funcode, as, ap);
      return (OZ_STARTED);
    }
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

static uLong startrw (Iopex *iopex, int writing, uLong datasize, const OZ_Mempage *dataphypages, uLong databyteoffs, OZ_Dbn slbn)

{
  Devex *devex;
  OZ_IO_scsi_doiopp scsi_doiopp;
  uByte cmdbuf[6];

  devex = iopex -> devex;
  iopex -> writing  = writing;
  iopex -> datasize = datasize;

  /* Make sure the tape is loaded first */

  if (!(devex -> valid)) return (OZ_VOLNOTVALID);
  if (writing && (devex -> readonly)) return (OZ_WRITELOCKED);

  /* Set up command and I/O block */

  cmdbuf[0] = writing * 2 + 0x08;	/* 08: reading, 0A: writing */
  cmdbuf[1] = 0;
  cmdbuf[2] = datasize >> 16;
  cmdbuf[3] = datasize >>  8;
  cmdbuf[4] = datasize;
  cmdbuf[5] = 0;

  memset (&scsi_doiopp, 0, sizeof scsi_doiopp);
  scsi_doiopp.cmdlen       = sizeof cmdbuf;
  scsi_doiopp.cmdbuf       = cmdbuf;
  scsi_doiopp.datasize     = datasize;
  scsi_doiopp.dataphypages = dataphypages;
  scsi_doiopp.databyteoffs = databyteoffs;
  scsi_doiopp.status       = &(iopex -> scsists);
  scsi_doiopp.datarlen     = &(iopex -> datarlen);
  scsi_doiopp.optflags     = writing ? OZ_IO_SCSI_OPTFLAG_WRITE | OZ_IO_SCSI_OPTFLAG_DISCONNECT : OZ_IO_SCSI_OPTFLAG_DISCONNECT;

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
  if ((sts == OZ_SUCCESS) && iopex -> writing && (iopex -> datarlen != iopex -> datasize)) {
    oz_knl_printk ("oz_dev_tape_scsi completedrw: only wrote %u bytes instead of %u\n", iopex -> datarlen, iopex -> datasize);
    sts = OZ_IOFAILED;
  }
  oz_dev_lio_done (iopex -> lior, sts, (!(iopex -> writing) && (iopex -> callerrlen != NULL)) ? storerlen : NULL, iopex);
}

static void storerlen (void *iopexv, int finok, uLong *status_r)

{
  Iopex *iopex;
  uLong sts;

  iopex = iopexv;
  if (finok) {
    sts = oz_knl_section_uput (iopex -> procmode, sizeof *(iopex -> callerrlen), &(iopex -> datarlen), iopex -> callerrlen);
    if (sts != OZ_SUCCESS) {
      oz_knl_printk ("oz_dev_scsi_tape: error %u storing rlen at %p\n", sts, iopex -> callerrlen);
      if (*status_r == OZ_SUCCESS) *status_r = sts;
    }
  }
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
  uLong sts;

  iopex = iopexv;
  devex = iopex -> devex;

  sts = decode_status (status, iopex, iopex -> startstop ? "spinning up" : "spinning down");
  if ((sts == OZ_SUCCESS) && iopex -> startstop) {

    /* Drive is all spun up, read its capacity (also perform width/speed negotiation) */

    startreadcap (iopex);
    return;
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
  scsi_doio.optflags = OZ_IO_SCSI_OPTFLAG_NEGOTIATE;

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
    oz_knl_printk ("oz_dev_scsi_tape: only read %u of %u bytes of %s capacity\n", 
                   iopex -> capacityrlen, sizeof iopex -> capacitybuff, devex -> name);
  }
  if (sts == OZ_SUCCESS) {
    devex -> totalblocks = BUF2LONG (iopex -> capacitybuff + 0);
    devex -> blocksize   = BUF2LONG (iopex -> capacitybuff + 4);
    if ((devex -> blocksize == 0) || (devex -> blocksize > (1 << OZ_HW_L2PAGESIZE))) {
      oz_knl_printk ("oz_dev_scsi_tape: %s blocksize %u invalid\n", devex -> name, devex -> blocksize);
      sts = OZ_BADBLOCKSIZE;
    } else {
      totalmegs   = ((uQuad)(devex -> totalblocks)) * ((uQuad)(devex -> blocksize));
      totalmegs >>= 20;
      oz_knl_printk ("oz_dev_scsi_tape: %s has %u blocks of %u bytes (%u Meg)\n", 
                     devex -> name, devex -> totalblocks, devex -> blocksize, (uLong)totalmegs);
      devex -> valid = 1;
    }
  } else if (-- (iopex -> retries) >= 0) {
    startreadcap (iopex);
    return;
  }
  oz_dev_lio_done (iopex -> lior, sts, NULL, NULL);
}

/************************************************************************/
/*									*/
/*  Miscellaneous completed request routines				*/
/*									*/
/************************************************************************/

/* This routine is called when the rewind command has completed */

static void rewindcomplete (void *iopexv, uLong status)

{
  Iopex *iopex;
  uLong sts;

  iopex = iopexv;

  sts = decode_status (status, iopex, "rewinding");
  oz_dev_lio_done (iopex -> lior, sts, NULL, NULL);
}

/* This routine is called when the skip command has completed */
/*   iopex -> files = 0: skipped blocks, 1: skipped filemarks */
/*            count = number of blocks/files requested        */

static void skipcomplete (void *iopexv, uLong status)

{
  Iopex *iopex;
  uLong sts;

  iopex = iopexv;

  sts = decode_status (status, iopex, "skipping");
  ??
  oz_dev_lio_done (iopex -> lior, sts, NULL, NULL);
}

/* This routine is called when the writemark command has completed   */
/*   iopex -> count = 0 : just a simple writemark                    */
/*                    1 : two marks written, space back between them */

static void writemarkcomplete (void *iopexv, uLong status)

{
  Iopex *iopex;
  uLong sts;

  iopex = iopexv;

  sts = decode_status (status, iopex, "writing mark");
  ??
  oz_dev_lio_done (iopex -> lior, sts, NULL, NULL);
}

/* This routine is called when a get position command has completed */
/*   iopex -> tempbuff = position data read from drive              */
/*          callerrlen = where to return it to caller               */

static void getposcomplete (void *iopexv, uLong status)

{
  Iopex *iopex;
  uLong sts;

  iopex = iopexv;

  sts = decode_status (status, iopex, "getting position");
  ??
  oz_dev_lio_done (iopex -> lior, sts, NULL, NULL);
}

/* This routine is called when the set position command has completed */

static void setposcomplete (void *iopexv, uLong status)

{
  Iopex *iopex;
  uLong sts;

  iopex = iopexv;

  sts = decode_status (status, iopex, "setting position");
  oz_dev_lio_done (iopex -> lior, sts, NULL, NULL);
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
  uByte cmdbuf[6], key, scsists, sensebuf[29], sensests;
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
        oz_knl_printk ("oz_dev_scsi_tape: scsi error 0x%2.2x %s %s\n", iopex -> scsists, funcdesc, devex -> name);
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
        scsi_doio.timeout  = 2;

        sts = oz_knl_io (devex -> scsiiochan, OZ_IO_SCSI_DOIO, sizeof scsi_doio, &scsi_doio);

        if (sts != OZ_SUCCESS) {
          oz_knl_printk ("oz_dev_scsi_tape: error %u requesting sense of %s\n", sts, devex -> name);
        } else if ((sensests & 0x3E) != 0) {
          oz_knl_printk ("oz_dev_scsi_tape: scsi sts %2.2X requesting sense of %s\n", sensests, devex -> name);
          sts = OZ_IOFAILED;
        } else if ((sensebuf[0] & 0x7E) != 0x70) {
          oz_knl_printk ("oz_dev_scsi_tape: request sense[0] %2.2X from %s\n", sensebuf[0], devex -> name);
          oz_knl_dumpmem2 (senselen, sensebuf, 0);
          sts = OZ_IOFAILED;
        } else if ((strcmp (funcdesc, "skipping") == 0) 	// see if we were skipping
                && !(iopex -> files) 				// ... records, that is
                && ((sensebuf[2] & 0x0F) == 0) 			// ... and we hit a tape mark
                && (sensebuf[12] == 0) 
                && (sensebuf[13] == 1) 
                && (sensebuf[28] == 0x32)) {
          iopex -> count -= (int)((sensebuf[3] << 24) | (sensebuf[4] << 16) | (sensebuf[5] << 8) | sensebuf[6]);
        } else if ((strcmp (funcdesc, "skipping") == 0) 	// see if we were skipping
                && ((sensebuf[2] & 0x0F) == 0) 			// ... and we hit end of data
                && (sensebuf[12] == 0) 
                && (sensebuf[13] == 5) 
                && (sensebuf[28] == 0x33)) {
          iopex -> count -= (int)((sensebuf[3] << 24) | (sensebuf[4] << 16) | (sensebuf[5] << 8) | sensebuf[6]);
        } else {
          key = sensebuf[2] & 0x0F;
          oz_knl_printk ("oz_dev_scsi_tape: %s %s %s\n", msgtable[key], funcdesc, devex -> name);
          oz_knl_printk ("oz_dev_scsi_tape: - key %X, asc %X, ascq %X, fsc %X\n", key, sensebuf[12], sensebuf[13], sensebuf[28]);
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
