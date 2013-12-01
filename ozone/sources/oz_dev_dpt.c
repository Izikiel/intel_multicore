//+++2003-03-01
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
//---2003-03-01

/************************************************************************/
/*									*/
/*  This is a disk pass-thru filesystem driver				*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_io_console.h"
#include "oz_io_disk.h"
#include "oz_io_fs.h"
#include "oz_knl_dcache.h"
#include "oz_knl_devio.h"
#include "oz_knl_event.h"
#include "oz_knl_hw.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_procmode.h"
#include "oz_knl_sdata.h"
#include "oz_knl_section.h"
#include "oz_knl_security.h"
#include "oz_knl_shuthand.h"
#include "oz_knl_spte.h"
#include "oz_knl_status.h"
#include "oz_knl_thread.h"
#include "oz_knl_userjob.h"
#include "oz_sys_dateconv.h"
#include "oz_sys_xprintf.h"

#define DRIVERNAME "oz_dpt"

typedef struct Chnex Chnex;
typedef struct Devex Devex;
typedef struct Iopex Iopex;


/* Device extension structure */

struct Devex { OZ_Devunit *devunit;		/* corresponding devunit */
               int shutdown;			/* device is being shut down - don't process any more requests */
               OZ_Smplock smplock_vl;		/* smp lock for iopexqh/iopexqt */
               Iopex *iopexip;			/* I/O request in progress */
               Iopex *iopexqh, **iopexqt;	/* list of I/O requests */
               OZ_Iochan *master_iochan;	/* I/O channel assigned to disk drive */
               OZ_Dcache *dcache;		/* disk cache context pointer */
               OZ_Dbn blockcount;		/* total number of blocks on disk */
               uLong blocksize;			/* disk's block size */
               uLong bufalign;			/* buffer alignment required by disk driver */
               OZ_Dbn eofblock;			/* end-of-file block number */
               uLong eofbyte;			/* end-of-file byte number */
               OZ_Shuthand *shuthand;		/* pointer to shutdown handler */
               uLong mountflags;		/* flags it was mounted with */
             };

/* Channel extension structure */

struct Chnex { OZ_Iochan *iochan;		// pointer to corresponding driver-independent channel struct
               int ignclose;			/* ignore OZ_IO_FS_CLOSE calls - only close when deassigned */
               OZ_Dbn curblock;			/* record I/O current position */
               uLong curbyte;
             };

/* I/O Operation extension structure */

struct Iopex { OZ_Ioop *ioop;			/* i/o operation struct pointer */
               Devex *devex;			/* corresponding devex struct pointer */
               Chnex *chnex;			/* corresponding chnex struct pointer */
               Iopex *next;			/* next in devex -> iopexqh/iopexqt */
               OZ_Procmode procmode;		/* requestor's processor mode */
               uLong funcode;			/* i/o function code */
               uLong (*backend) (Iopex *iopex, Chnex *chnex, Devex *devex);
               uLong as;			/* argument block size */
               void *ap;			/* argument block pointer */
               int aborted;			// flagged for abort
               union { struct { OZ_IO_fs_mountvol p;
                                OZ_Iochan *iochan;
                              } mountvol;
                       struct { OZ_IO_fs_dismount p; } dismount;
                       struct { OZ_IO_fs_writeblocks p; } writeblocks;
                       struct { OZ_IO_fs_readblocks p; } readblocks;
                       struct { OZ_IO_fs_writerec p;
                                const OZ_Mempage *phypages;	// data buffer physical page number array
                                uLong byteoffs;			// byte offset of buffer in first physical page
                                const OZ_Mempage *wlen_phypages; // 'write length' physical page number array
                                uLong wlen_byteoffs;		// byte offset of 'write length' in first phy page
                                OZ_Dbn efblk;			// end-of-file block number
                                uLong efbyt;			// end-of-file byte number
                                uLong wlen;			// length written out so far
                                uLong trmwlen;			// number of termintor bytes written out so far
                                const OZ_Mempage *trmphypages;	// terminator buffer phypage array
                                uLong trmbyteoffs;		// terminator buffer phypage offset
                                int updateof;			// eof position needs to be updated on completion
                                uByte trmdata[4];		// terminator buffer for short buffers
                                uLong status;			// completion status
                                OZ_Dcmpb dcmpb;			// oz_knl_dcache_map parameter block
                              } writerec;
                       struct { OZ_IO_fs_readrec p;		// request parameter block
                                const OZ_Mempage *phypages;	// buffer physical page array
                                uLong byteoffs;			// offset in first physical page
                                const OZ_Mempage *rlen_phypages; // rlen physical page array
                                uLong rlen_byteoffs;		// offset in first physical page
                                OZ_Dbn efblk;			// end-of-file block number
                                uLong efbyt;			// end-of-file byte number
                                uLong rlen;			// length read in so far
                                uLong trmseen;			// number of termintor bytes seen so far
                                uByte *trmbuff;			// copy of terminator buffer
                                uByte trmdata[4];		// terminator buffer for short buffers
                                uLong status;			// completion status
                                OZ_Dcmpb dcmpb;			// oz_knl_dcache_map parameter block
                              } readrec;
                       struct { OZ_IO_fs_pagewrite p; } pagewrite;
                       struct { OZ_IO_fs_pageread p; } pageread;
                       struct { OZ_IO_fs_extend p; } extend;
                       struct { OZ_IO_fs_getinfo1 p; } getinfo1;
                       struct { OZ_IO_fs_setcurpos p; } setcurpos;
                       struct { OZ_IO_fs_getinfo2 p; } getinfo2;
                       struct { OZ_IO_fs_getinfo3 p; } getinfo3;

                       struct { uLong size;
                                OZ_Dbn virtblock;
                                uLong blockoffs;
                                const OZ_Mempage *pagearray;
                                uLong pageoffs;
                                OZ_Dcmpb dcmpb;
                              } rwb;
                     } u;
             };

/* Function table */

static uLong dpt_clonecre (OZ_Devunit *template_devunit, void *template_devex, int template_cloned, OZ_Procmode procmode, OZ_Devunit **cloned_devunit);
static int dpt_clonedel (OZ_Devunit *cloned_devunit, void *devexv, int cloned);
static uLong dpt_assign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode);
static int dpt_deassign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv);
static void dpt_abort (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Ioop *ioop, void *iopexv, OZ_Procmode procmode);
static uLong dpt_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode, 
                               OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap);

static const OZ_Devfunc dpt_functable = { sizeof (Devex), sizeof (Chnex), sizeof (Iopex), 0, 
                                          NULL, dpt_clonecre, dpt_clonedel, 
                                          dpt_assign, dpt_deassign, 
                                          dpt_abort, dpt_start, NULL };

/* Driver static data */

static int initialized = 0;
static uLong clonumber = 0;
static OZ_Devclass *devclass;
static OZ_Devdriver *devdriver;
static OZ_Devunit *devunit;

static Devex *crash_devex = NULL;
static OZ_IO_disk_crash crash_disk;

/* Internal routines */

static void mount_setvolvalid_done (void *iopexv, uLong status);
static void shuthand (void *devexv);
static uLong sc_shutdown (Iopex *iopex, Chnex *chnex, Devex *devex);
static uLong sc_dismount (Iopex *iopex, Chnex *chnex, Devex *devex);
static uLong rwb_start (Iopex *iopex, int writing, int writethru, OZ_Dbn virtblock, uLong blockoffs, 
                        uLong size, const OZ_Mempage *pagearray, uLong pageoffs);
static uLong rwb_dcache (OZ_Dcmpb *dcmpb, uLong status);
static void rwb_iodone (void *iopexv, uLong status);
static uLong sc_writerec (Iopex *iopex, Chnex *chnex, Devex *devex);
static uLong sc_readrec (Iopex *iopex, Chnex *chnex, Devex *devex);
static uLong sc_crash (Iopex *iopex, Chnex *chnex, Devex *devex);
static uLong queuerecio (Iopex *iopex, Chnex *chnex, Devex *devex);
static void finishrecio (Iopex *iopex, uLong status, void (*finentry) (void *iopexv, int finok, uLong *status_r));
static uLong dismount_volume (int unload, int shutdown, Iopex *iopex);
static void dismount_unloaded (void *iopexv, uLong status);
static uLong disk_fs_crash (void *dummy, OZ_Dbn vbn, uLong size, OZ_Mempage phypage, uLong offset);
static void printk (Iopex *iopex, const char *format, ...);
static void printe (Iopex *iopex, const char *format, ...);
static uLong startprintk (void *iochanv, uLong *size, char **buff);

/************************************************************************/
/*									*/
/*  Boot-time initialization routine					*/
/*									*/
/************************************************************************/

void oz_dev_dpt_init ()

{
  Devex *devex;

  if (!initialized) {
    oz_knl_printk ("oz_dev_dpt_init\n");
    initialized = 1;

    /* Set up template device data structures */

    devclass  = oz_knl_devclass_create (OZ_IO_FS_CLASSNAME, OZ_IO_FS_BASE, OZ_IO_FS_MASK, DRIVERNAME);
    devdriver = oz_knl_devdriver_create (devclass, DRIVERNAME);
    devunit   = oz_knl_devunit_create (devdriver, DRIVERNAME, "mount template", &dpt_functable, 0, oz_s_secattr_tempdev);
    devex     = oz_knl_devunit_ex (devunit);
    memset (devex, 0, sizeof *devex);
  }
}

/************************************************************************/
/*									*/
/*  An I/O channel is being assigned and the devio routines want to 	*/
/*  know if this device is to be cloned.				*/
/*									*/
/*  In this driver, we only clone the original template device.		*/
/*									*/
/*    Input:								*/
/*									*/
/*	template_devunit = pointer to existing device unit		*/
/*	template_devex   = device extension data			*/
/*	template_cloned  = template's cloned flag			*/
/*	procmode         = processor mode doing the assign		*/
/*									*/
/*    Output:								*/
/*									*/
/*	dpt_clonecre = OZ_SUCCESS : ok to assign channel		*/
/*	                            else : error status			*/
/*	**cloned_devunit = cloned device unit				*/
/*									*/
/************************************************************************/

static uLong dpt_clonecre (OZ_Devunit *template_devunit, void *template_devex, int template_cloned, OZ_Procmode procmode, OZ_Devunit **cloned_devunit)

{
  char unitname[12+sizeof DRIVERNAME];
  Devex *devex;
  int i;
  OZ_Secattr *secattr;
  uLong sts;

  /* If this is an already cloned devunit, don't clone anymore, just use the original device */

  if (template_cloned) {
    *cloned_devunit = template_devunit;
    oz_knl_devunit_increfc (template_devunit, 1);
  }

  /* This is the original template device, clone a unit.  The next thing the caller should do is an OZ_IO_FS_MOUNTVOL call. */

  else {
    strcpy (unitname, DRIVERNAME);
    strcat (unitname, "_");
    i = strlen (unitname);
    oz_hw_itoa (++ clonumber, sizeof unitname - i, unitname + i);

    secattr = oz_knl_thread_getdefcresecattr (NULL);
    *cloned_devunit = oz_knl_devunit_create (devdriver, unitname, "Not yet mounted", &dpt_functable, 1, secattr);
    oz_knl_secattr_increfc (secattr, -1);
    devex = oz_knl_devunit_ex (*cloned_devunit);
    memset (devex, 0, sizeof *devex);
    devex -> devunit = *cloned_devunit;
    oz_hw_smplock_init (sizeof devex -> smplock_vl, &(devex -> smplock_vl), OZ_SMPLOCK_LEVEL_VL);
    devex -> iopexqt = &(devex -> iopexqh);
  }

  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  The last channel was deassigned from a devunit.  This routine is 	*/
/*  called to see if the unit should be deleted.			*/
/*									*/
/*  In this driver, we only delete devices that are not mounted.  We 	*/
/*  also never delete the template device (duh!).			*/
/*									*/
/*    Input:								*/
/*									*/
/*	cloned_devunit = cloned device's devunit struct			*/
/*	devex = the devex of cloned_devunit				*/
/*	cloned = the cloned_devunit's cloned flag			*/
/*									*/
/*    Output:								*/
/*									*/
/*	dpt_clonedel = 0 : keep device in device table			*/
/*	               1 : delete device from table			*/
/*									*/
/************************************************************************/

static int dpt_clonedel (OZ_Devunit *cloned_devunit, void *devexv, int cloned)

{
  Devex *devex;

  devex = devexv;
  return (cloned && (devex -> iopexqh == NULL) && (devex -> master_iochan == NULL));
}

/************************************************************************/
/*									*/
/*  An I/O channel was just assigned to the unit			*/
/*									*/
/*  Clear out the channel extension block				*/
/*									*/
/************************************************************************/

static uLong dpt_assign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode)

{
  Chnex *chnex;

  chnex = chnexv;
  memset (chnex, 0, sizeof *chnex);
  chnex -> iochan = iochan;
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  An I/O channel was just deassigned from a unit			*/
/*									*/
/*    Input:								*/
/*									*/
/*	devunit  = device unit that is being deassigned from		*/
/*	devexv   = corresponding devex pointer				*/
/*	iochan   = i/o channel being deassigned				*/
/*	chnexv   = corresponding chnex pointer				*/
/*									*/
/************************************************************************/

static int dpt_deassign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv)

{
  return (0);
}

/************************************************************************/
/*									*/
/*  Abort I/O request in progress					*/
/*									*/
/************************************************************************/

static void dpt_abort (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Ioop *ioop, void *iopexv, OZ_Procmode procmode)

{
  Devex *devex;
  Iopex *iopex, **liopex, *xiopex;
  uLong vl;

  devex  = devexv;
  xiopex = NULL;

  /* Remove any matching requests from request queue */

  vl = oz_hw_smplock_wait (&(devex -> smplock_vl));				// lock iopexqh/iopexqt queue
  for (liopex = &(devex -> iopexqh); (iopex = *liopex) != NULL;) {		// scan the queue
    if (oz_knl_ioabortok (iopex -> ioop, iochan, procmode, ioop)) { 		// see if this is something we should abort
      *liopex = iopex -> next;							// if so, unlink from queue
      if (devex -> iopexqh == NULL) devex -> iopexqt = &(devex -> iopexqh);
      iopex -> next = xiopex;
      xiopex = iopex;
    } else {
      liopex = &(iopex -> next);
    }
  }
  devex -> iopexqt = liopex;							// set up possibly new tail pointer
  oz_hw_smplock_clr (&(devex -> smplock_vl), vl);				// queue is clean, release lock

  /* Now that we're back at softint level, abort all the requests we found */

  while ((iopex = xiopex) != NULL) {
    xiopex = iopex -> next;
    oz_knl_iodone (iopex, OZ_ABORTED, NULL, NULL, NULL);
  }
}

/************************************************************************/
/*									*/
/*  This routine is called as a result of an oz_knl_iostart call to 	*/
/*  start performing an i/o request					*/
/*									*/
/************************************************************************/

static uLong dpt_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode,
                        OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap)

{
  Chnex *chnex;
  Devex *devex;
  Iopex *iopex;
  uLong sts, vl;

  chnex = chnexv;
  devex = devexv;
  iopex = iopexv;

  /* Maybe device is being shut down */

  if (devex -> shutdown) return (OZ_SYSHUTDOWN);

  /* Set up stuff in iopex that is common to just about all functions */

  iopex -> ioop     = ioop;
  iopex -> funcode  = funcode;
  iopex -> chnex    = chnex;
  iopex -> devex    = devex;
  iopex -> procmode = procmode;
  iopex -> as       = as;
  iopex -> ap       = ap;
  iopex -> aborted  = 0;

  movc4 (as, ap, sizeof iopex -> u, &(iopex -> u));

  sts = OZ_SUCCESS;

  switch (funcode) {

    /* Mount volume - assign I/O channel to target disk drive and spin it up */

    case OZ_IO_FS_MOUNTVOL: {
      OZ_IO_disk_setvolvalid disk_setvolvalid;
      OZ_Lockmode lockmode;

      sts = oz_knl_ioop_lockz (iopex -> ioop, -1, iopex -> u.mountvol.p.devname, NULL, NULL, NULL, NULL);
      if (sts != OZ_SUCCESS) return (sts);

      /* Assign an I/O channel to the disk drive -           */
      /* Allow others to read disk but no-one else can write */

      lockmode = OZ_LOCKMODE_PW;
      if (iopex -> u.mountvol.p.mountflags & OZ_FS_MOUNTFLAG_READONLY) lockmode = OZ_LOCKMODE_PR;
      sts = oz_knl_iochan_crbynm (iopex -> u.mountvol.p.devname, lockmode, OZ_PROCMODE_KNL, NULL, &(iopex -> u.mountvol.iochan));
      if (sts != OZ_SUCCESS) return (sts);

      /* Start setting volume valid flag */

      memset (&disk_setvolvalid, 0, sizeof disk_setvolvalid);
      disk_setvolvalid.valid = 1;
      sts = oz_knl_iostart3 (1, NULL, iopex -> u.mountvol.iochan, OZ_PROCMODE_KNL, 
                             mount_setvolvalid_done, iopex, NULL, NULL, NULL, NULL, 
                             OZ_IO_DISK_SETVOLVALID, sizeof disk_setvolvalid, &disk_setvolvalid);
      if (sts != OZ_STARTED) mount_setvolvalid_done (iopex, sts);
      return (OZ_STARTED);
    }

    /* Convert these directly to equivalent cache or disk I/O requests */

    case OZ_IO_FS_WRITEBLOCKS: {
      const OZ_Mempage *pagearray;
      uLong pageoffs;

      sts = oz_knl_ioop_lockr (iopex -> ioop, iopex -> u.writeblocks.p.size, iopex -> u.writeblocks.p.buff, 
                               &pagearray, NULL, &pageoffs);
      if (sts == OZ_SUCCESS) sts = rwb_start (iopex, 1, iopex -> u.writeblocks.p.writethru, 
                                              iopex -> u.writeblocks.p.svbn, iopex -> u.writeblocks.p.offs, 
                                              iopex -> u.writeblocks.p.size, pagearray, pageoffs);
      return (sts);
    }

    case OZ_IO_FS_READBLOCKS: {
      const OZ_Mempage *pagearray;
      uLong pageoffs;

      sts = oz_knl_ioop_lockw (iopex -> ioop, iopex -> u.readblocks.p.size, iopex -> u.readblocks.p.buff, 
                               &pagearray, NULL, &pageoffs);
      if (sts == OZ_SUCCESS) sts = rwb_start (iopex, 0, 0, 
                                              iopex -> u.readblocks.p.svbn, iopex -> u.readblocks.p.offs, 
                                              iopex -> u.readblocks.p.size, pagearray, pageoffs);
      return (sts);
    }

    case OZ_IO_FS_PAGEWRITE: {
      if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);
      sts = rwb_start (iopex, 1, iopex -> u.pagewrite.p.writethru, 
                       iopex -> u.pagewrite.p.startblock, 0, 
                       iopex -> u.pagewrite.p.pagecount << OZ_HW_L2PAGESIZE, 
                       iopex -> u.pagewrite.p.pagearray, 0);
      return (sts);
    }

    case OZ_IO_FS_PAGEREAD: {
      if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);
      sts = rwb_start (iopex, 0, 0, 
                       iopex -> u.pageread.p.startblock, 0, 
                       iopex -> u.pageread.p.pagecount << OZ_HW_L2PAGESIZE, 
                       iopex -> u.pageread.p.pagearray, 0);
      return (sts);
    }

    /* Record I/O requests can only be done one at a time because they share the current and end-of-file pointers */

    case OZ_IO_FS_WRITEREC: {

      // Lock the data buffer in memory

      sts = oz_knl_ioop_lockr (iopex -> ioop, iopex -> u.writerec.p.size, iopex -> u.writerec.p.buff, 
                               &(iopex -> u.writerec.phypages), NULL, &(iopex -> u.writerec.byteoffs));

      // Lock return length in memory and clear it

      if ((sts == OZ_SUCCESS) && (iopex -> u.writerec.p.wlen != NULL)) {
        sts = oz_knl_ioop_lockw (iopex -> ioop, sizeof *(iopex -> u.writerec.p.wlen), iopex -> u.writerec.p.wlen, 
                                 &(iopex -> u.writerec.wlen_phypages), NULL, &(iopex -> u.writerec.wlen_byteoffs));
        if (sts == OZ_SUCCESS) *(iopex -> u.writerec.p.wlen) = 0;
      }

      // Lock the terminator buffer in memory (or copy small ones to trmdata)

      if (sts == OZ_SUCCESS) {
        if (iopex -> u.writerec.p.trmsize > sizeof iopex -> u.writerec.trmdata) {
          sts = oz_knl_ioop_lockr (iopex -> ioop, iopex -> u.writerec.p.trmsize, iopex -> u.writerec.p.trmbuff, 
                                   &(iopex -> u.writerec.trmphypages), NULL, &(iopex -> u.writerec.trmbyteoffs));
        } else {
          sts = oz_knl_section_uget (procmode, iopex -> u.writerec.p.trmsize, iopex -> u.writerec.p.trmbuff, 
                                     iopex -> u.writerec.trmdata);
        }
      }

      // Queue the request for processing

      if (sts == OZ_SUCCESS) {
        iopex -> backend = sc_writerec;
        sts = queuerecio (iopex, chnex, devex);
      }
      return (sts);
    }

    case OZ_IO_FS_READREC: {

      // Lock the data buffer in memory

      sts = oz_knl_ioop_lockw (iopex -> ioop, iopex -> u.readrec.p.size, iopex -> u.readrec.p.buff, 
                               &(iopex -> u.readrec.phypages), NULL, &(iopex -> u.readrec.byteoffs));

      // Lock return length in memory and clear it

      if ((sts == OZ_SUCCESS) && (iopex -> u.readrec.p.rlen != NULL)) {
        sts = oz_knl_ioop_lockw (iopex -> ioop, sizeof *(iopex -> u.readrec.p.rlen), iopex -> u.readrec.p.rlen, 
                                 &(iopex -> u.readrec.rlen_phypages), NULL, &(iopex -> u.readrec.rlen_byteoffs));
        if (sts == OZ_SUCCESS) *(iopex -> u.readrec.p.rlen) = 0;
      }

      // Copy the terminator buffer in to temp buffer

      iopex -> u.readrec.trmbuff = NULL;
      if ((sts == OZ_SUCCESS) && (iopex -> u.readrec.p.trmsize != 0)) {
        iopex -> u.readrec.trmbuff = iopex -> u.readrec.trmdata;
        if (iopex -> u.readrec.p.trmsize > sizeof iopex -> u.readrec.trmdata) {
          iopex -> u.readrec.trmbuff = OZ_KNL_PGPMALLOQ (iopex -> u.readrec.p.trmsize);
          if (iopex -> u.readrec.trmbuff != NULL) return (OZ_EXQUOTAPGP);
        }
        sts = oz_knl_section_uget (procmode, iopex -> u.readrec.p.trmsize, iopex -> u.readrec.p.trmbuff, iopex -> u.readrec.trmbuff);
        if ((sts != OZ_SUCCESS) && (iopex -> u.readrec.p.trmsize > sizeof iopex -> u.readrec.trmdata)) OZ_KNL_PGPFREE (iopex -> u.readrec.trmbuff);
      }

      // Queue the request for processing

      if (sts == OZ_SUCCESS) {
        iopex -> backend = sc_readrec;
        sts = queuerecio (iopex, chnex, devex);
      }
      return (sts);
    }

    case OZ_IO_FS_DISMOUNT: {
      iopex -> backend = sc_dismount;
      sts = queuerecio (iopex, chnex, devex);
      return (sts);
    }

    case OZ_IO_FS_CREATE: {
      chnex -> curblock = 1;
      chnex -> curbyte  = 0;
      devex -> eofblock = 1;
      devex -> eofbyte  = 0;
      return (OZ_SUCCESS);
    }

    case OZ_IO_FS_OPEN: {
      chnex -> curblock = 1;
      chnex -> curbyte  = 0;
      return (OZ_SUCCESS);
    }

    case OZ_IO_FS_CLOSE: {
      return (OZ_SUCCESS);
    }

    case OZ_IO_FS_EXTEND: {
      if (iopex -> u.extend.p.nblocks > devex -> blockcount) return (OZ_DEVICEFULL);
      return (OZ_SUCCESS);
    }

    case OZ_IO_FS_GETINFO1: {
      sts = oz_knl_ioop_lockw (iopex -> ioop, iopex -> as, iopex -> ap, NULL, NULL, NULL);
      if (sts == OZ_SUCCESS) {
        iopex -> u.getinfo1.p.blocksize = devex -> blocksize;
        iopex -> u.getinfo1.p.eofblock  = devex -> eofblock;
        iopex -> u.getinfo1.p.eofbyte   = devex -> eofbyte;
        iopex -> u.getinfo1.p.hiblock   = devex -> blockcount;
        iopex -> u.getinfo1.p.curblock  = chnex -> curblock;
        iopex -> u.getinfo1.p.curbyte   = chnex -> curbyte;
        movc4 (sizeof iopex -> u.getinfo1.p, &(iopex -> u.getinfo1.p), iopex -> as, iopex -> ap);
      }
      return (sts);
    }

    case OZ_IO_FS_SETCURPOS: {
      chnex -> curblock = iopex -> u.setcurpos.p.atblock;
      chnex -> curbyte  = iopex -> u.setcurpos.p.atbyte;
      return (OZ_SUCCESS);
    }

    case OZ_IO_FS_GETINFO3: {
      sts = oz_knl_ioop_lockw (iopex -> ioop, iopex -> as, iopex -> ap, NULL, NULL, NULL);
      if ((sts == OZ_SUCCESS) && (iopex -> u.getinfo3.p.undersize != 0)) {
        sts = oz_knl_ioop_lockw (iopex -> ioop, iopex -> u.getinfo3.p.undersize, iopex -> u.getinfo3.p.underbuff, NULL, NULL, NULL);
      }
      if (sts == OZ_SUCCESS) {
        iopex -> u.getinfo3.p.blocksize     = devex -> blocksize;
        iopex -> u.getinfo3.p.clusterfactor = 1;
        iopex -> u.getinfo3.p.clustertotal  = devex -> blockcount;
        iopex -> u.getinfo3.p.mountflags    = devex -> mountflags;
        if (devex -> dcache != NULL) oz_knl_dcache_stats (devex -> dcache, 
                                                          &(iopex -> u.getinfo3.p.nincache), 
                                                          &(iopex -> u.getinfo3.p.ndirties), 
                                                          &(iopex -> u.getinfo3.p.dirty_interval), 
                                                          &(iopex -> u.getinfo3.p.avgwriterate));
        if (iopex -> u.getinfo3.p.undersize != 0) strncpyz (iopex -> u.getinfo3.p.underbuff, 
                                                            oz_knl_devunit_devname (oz_knl_iochan_getdevunit (devex -> master_iochan)), 
                                                            iopex -> u.getinfo3.p.undersize);
        movc4 (sizeof (iopex -> u.getinfo3.p), &(iopex -> u.getinfo3.p), iopex -> as, iopex -> ap);
      }
      return (sts);
    }

    case OZ_IO_FS_SHUTDOWN: {
      if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);	/* can only be called from kernel mode */
      devex -> shutdown = 1;					/* don't ever let any more requests queue */
      iopex -> backend  = sc_shutdown;				/* queue to mount thread */
      sts = queuerecio (iopex, chnex, devex);
      return (sts);
    }

    case OZ_IO_FS_CRASH: {
      if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);
      iopex -> backend = sc_crash;
      sts = queuerecio (iopex, chnex, devex);
      return (sts);
    }
  }

  /* Don't know what the function is */

  return (OZ_BADIOFUNC);
}

/************************************************************************/
/*									*/
/*  The 'set volume valid' I/O to the disk has completed		*/
/*									*/
/************************************************************************/

static void mount_setvolvalid_done (void *iopexv, uLong status)

{
  char unitdesc[OZ_DEVUNIT_DESCSIZE], unitname[OZ_DEVUNIT_NAMESIZE];
  Chnex *chnex;
  Devex *devex;
  Iopex *iopex;
  OZ_Devunit *devunit;
  OZ_IO_disk_getinfo1 disk_getinfo1;
  OZ_Secattr *secattr;
  uLong sts;

  iopex = iopexv;
  chnex = iopex -> chnex;
  devex = iopex -> devex;

  /* Check the 'spin up' status */

  sts = status;
  if (sts != OZ_SUCCESS) {
    printk (iopex, "oz_dev_dpt mount: error %u setting volume valid bit\n", sts);
    goto rtnerr;
  }

  /* Get disk geometry */

  memset (&disk_getinfo1, 0, sizeof disk_getinfo1);

  sts = oz_knl_io (iopex -> u.mountvol.iochan, OZ_IO_DISK_GETINFO1, sizeof disk_getinfo1, &disk_getinfo1);
  if (sts != OZ_SUCCESS) {
    printk (iopex, "oz_dev_dpt mount_volume: error %u getting disk information\n", sts);
    goto rtnerr;
  }
  devex -> blockcount = disk_getinfo1.totalblocks;
  devex -> blocksize  = disk_getinfo1.blocksize;
  devex -> bufalign   = disk_getinfo1.bufalign;
  devex -> mountflags = iopex -> u.mountvol.p.mountflags;

  devex -> eofblock   = devex -> blockcount + 1;
  devex -> eofbyte    = 0;

  /* Mount successful, rename the unit to something predictable */
  /* Unit name is <disk_unitname>.<template_unitname>           */
  /* Unit description is Volume <volume_name>                   */

  devunit = oz_knl_iochan_getdevunit (chnex -> iochan);
  oz_sys_sprintf (sizeof unitname, unitname, "%s.%s", oz_knl_devunit_devname (oz_knl_iochan_getdevunit (iopex -> u.mountvol.iochan)), DRIVERNAME);
  oz_sys_sprintf (sizeof unitdesc, unitdesc, "fs access to raw disk");
  oz_knl_devunit_rename (devunit, unitname, unitdesc);

  /* If the caller supplied any security attributes, apply them to the device now */

  if (iopex -> u.mountvol.p.secattrsize != 0) {
    sts = oz_knl_secattr_create (iopex -> u.mountvol.p.secattrsize, iopex -> u.mountvol.p.secattrbuff, NULL, &secattr);
    if (sts != OZ_SUCCESS) {
      printk (iopex, "error %u creating mount security attributes\n", sts);
      goto rtnerr;
    }
    oz_knl_devunit_setsecattr (devunit, secattr);
    oz_knl_secattr_increfc (secattr, -1);
  }

  /* Activate disk cache routines */

  if (!(devex -> mountflags & OZ_FS_MOUNTFLAG_NOCACHE)) {
    devex -> dcache = oz_knl_dcache_init (iopex -> u.mountvol.iochan, devex -> blocksize, NULL, NULL);
  }

  /* Mark the volume as being mounted by setting the I/O channel in the devex */

  if (oz_hw_atomic_setif_ptr (&(devex -> master_iochan), iopex -> u.mountvol.iochan, NULL)) {
    oz_knl_iodone (iopex -> ioop, OZ_SUCCESS, NULL, NULL, NULL);
    return;
  }
  oz_knl_dcache_term (devex -> dcache, 0);
  devex -> dcache = NULL;
  sts = OZ_ALREADYMOUNTED;

rtnerr:
  oz_knl_iochan_increfc (iopex -> u.mountvol.iochan, -1);
  oz_knl_iodone (iopex -> ioop, sts, NULL, NULL, NULL);
}

/************************************************************************/
/*									*/
/*  The system is being shut down - terminate all activity now		*/
/*									*/
/*    Input:								*/
/*									*/
/*	devex = device to shut down					*/
/*	smplevel = softint						*/
/*									*/
/************************************************************************/

static void shuthand (void *devexv)

{
  Devex *devex;
  uLong sts;
  OZ_Iochan *iochan;

  devex = devexv;

  /* The shutdown handler is no longer queued */

  devex -> shuthand = NULL;

  /* Use an I/O function code so it gets synchronized */

  sts = oz_knl_iochan_create (devex -> devunit, OZ_LOCKMODE_NL, OZ_PROCMODE_KNL, NULL, &iochan);
  if (sts != OZ_SUCCESS) oz_crash ("dpt_shutdown: error %u assigning I/O channel", sts);
  sts = oz_knl_io (iochan, OZ_IO_FS_SHUTDOWN, 0, NULL);
  if ((sts != OZ_SUCCESS) && (sts != OZ_NOTMOUNTED)) oz_crash ("dpt_shutdown: error %u shutting down", sts);
  oz_knl_iochan_increfc (iochan, -1);
}

/************************************************************************/
/*									*/
/*  System is being shutdown - close all files and dismount volume	*/
/*									*/
/************************************************************************/

static uLong sc_shutdown (Iopex *iopex, Chnex *chnex, Devex *devex)

{
  uLong sts;

  oz_knl_printk ("oz_dev_dpt: shutting down %s\n", oz_knl_devunit_devname (devex -> devunit));
  sts = dismount_volume (0, 1, iopex);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Dismount volume							*/
/*									*/
/************************************************************************/

static uLong sc_dismount (Iopex *iopex, Chnex *chnex, Devex *devex)

{
  uLong sts;

  sts = dismount_volume (iopex -> u.dismount.p.unload, 0, iopex);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Start Read/Write blocks operation					*/
/*									*/
/*    Input:								*/
/*									*/
/*	iopex     = fs iopex						*/
/*	writing   = 0 : it is a read operation				*/
/*	            1 : it is a write operation				*/
/*	writethru = 0 : writeback cache					*/
/*	            1 : writethru cache					*/
/*	virtblock = starting virtual block number			*/
/*	blockoffs = byte offset within first block			*/
/*	size      = number of bytes					*/
/*	pagearray = array of physical page numbers			*/
/*	pageoffs  = byte offset within first physical page		*/
/*	smplevel  = softint						*/
/*									*/
/************************************************************************/

static uLong rwb_start (Iopex *iopex, int writing, int writethru, OZ_Dbn virtblock, uLong blockoffs, 
                        uLong size, const OZ_Mempage *pagearray, uLong pageoffs)

{
  Devex *devex;
  OZ_IO_disk_readpages disk_readpages;
  OZ_IO_disk_writepages disk_writepages;
  uLong sts;

  devex = iopex -> devex;

  virtblock += blockoffs / devex -> blocksize;
  blockoffs %= devex -> blocksize;

  if (virtblock == 0) return (OZ_VBNZERO);
  if (virtblock > devex -> blockcount) return (OZ_ENDOFFILE);

  /* If cache enabled, do it all via the cache routines */

  if (devex -> dcache != NULL) {
    iopex -> u.rwb.size      = size;
    iopex -> u.rwb.virtblock = virtblock;
    iopex -> u.rwb.blockoffs = blockoffs;
    iopex -> u.rwb.pagearray = pagearray;
    iopex -> u.rwb.pageoffs  = pageoffs;

    iopex -> u.rwb.dcmpb.dcache    = devex -> dcache;
    iopex -> u.rwb.dcmpb.writing   = writing;
    iopex -> u.rwb.dcmpb.nbytes    = size;
    iopex -> u.rwb.dcmpb.logblock  = virtblock - 1;
    iopex -> u.rwb.dcmpb.virtblock = virtblock;
    iopex -> u.rwb.dcmpb.blockoffs = blockoffs;
    iopex -> u.rwb.dcmpb.entry     = rwb_dcache;
    iopex -> u.rwb.dcmpb.param     = iopex;
    iopex -> u.rwb.dcmpb.writethru = writethru || (devex -> mountflags & OZ_FS_MOUNTFLAG_WRITETHRU);
    iopex -> u.rwb.dcmpb.ix4kbuk   = 0;
    sts = oz_knl_dcache_map (&(iopex -> u.rwb.dcmpb));
    return (sts);
  }

  /* Don't support non-block sized transfers without the cache */

  if (blockoffs != 0) return (OZ_UNALIGNEDBUFF);
  if (size % devex -> blocksize != 0) return (OZ_UNALIGNEDXLEN);

  /* Convert to equivalent disk I/O request */

  if (writing) {
    memset (&disk_writepages, 0, sizeof disk_writepages);
    disk_writepages.size   = size;
    disk_writepages.pages  = pagearray;
    disk_writepages.offset = pageoffs;
    disk_writepages.slbn   = virtblock - 1;
    sts = oz_knl_iostart3 (1, NULL, devex -> master_iochan, OZ_PROCMODE_KNL, rwb_iodone, iopex, 
                           NULL, NULL, NULL, NULL, OZ_IO_DISK_WRITEPAGES, sizeof disk_writepages, &disk_writepages);
  } else {
    memset (&disk_readpages, 0, sizeof disk_readpages);
    disk_readpages.size   = size;
    disk_readpages.pages  = pagearray;
    disk_readpages.offset = pageoffs;
    disk_readpages.slbn   = virtblock - 1;
    sts = oz_knl_iostart3 (1, NULL, devex -> master_iochan, OZ_PROCMODE_KNL, rwb_iodone, iopex, 
                           NULL, NULL, NULL, NULL, OZ_IO_DISK_READPAGES, sizeof disk_readpages, &disk_readpages);
  }
  return (sts);
}

/************************************************************************/
/*									*/
/*  The disk cache system calls this routine when it has access to the 	*/
/*  cache blocks for the disk						*/
/*									*/
/************************************************************************/

static uLong rwb_dcache (OZ_Dcmpb *dcmpb, uLong status)

{
  const OZ_Mempage *phypages;
  Iopex *iopex;
  uLong size, skip;

  iopex = dcmpb -> param;
  size  = 0;

  /* Maybe the read or write request is complete */

  if (status != OZ_PENDING) oz_knl_iodone (iopex -> ioop, status, NULL, NULL, NULL);

  /* Copy to/from cache page from/to user buffer and increment parameters for next transfer                 */
  /* If new nbytes is zero, means we're done, and cache system will call us back when it's ok to free dcmpb */

  else {
    skip = (dcmpb -> virtblock - iopex -> u.rwb.virtblock) * iopex -> devex -> blocksize 
         + (dcmpb -> blockoffs - iopex -> u.rwb.blockoffs);
    size = iopex -> u.rwb.size - skip;
    if (size > dcmpb -> nbytes) size = dcmpb -> nbytes;
    if (dcmpb -> writing) oz_hw_phys_movephys (size, iopex -> u.rwb.pagearray, skip + iopex -> u.rwb.pageoffs, &(dcmpb -> phypage), dcmpb -> pageoffs);
    else oz_hw_phys_movephys (size, &(dcmpb -> phypage), dcmpb -> pageoffs, iopex -> u.rwb.pagearray, skip + iopex -> u.rwb.pageoffs);
    skip += size;
    dcmpb -> virtblock = iopex -> u.rwb.virtblock;
    dcmpb -> nbytes    = iopex -> u.rwb.size - skip;
    dcmpb -> blockoffs = iopex -> u.rwb.blockoffs + skip;
    dcmpb -> logblock  = iopex -> u.rwb.virtblock - 1;

    if (!(dcmpb -> writing)) size = 0;
  }

  /* Return number of bytes we modified */

  return (size);
}

/************************************************************************/
/*									*/
/*  A direct read/write blocks has completed on the disk		*/
/*									*/
/************************************************************************/

static void rwb_iodone (void *iopexv, uLong status)

{
  Iopex *iopex;

  iopex = iopexv;
  oz_knl_iodone (iopex -> ioop, status, NULL, NULL, NULL);
}

/************************************************************************/
/*									*/
/*  Write record							*/
/*									*/
/*  This is a shortcut routine, but there is only one request active 	*/
/*  per file at a time, as they go through file -> recio_q.		*/
/*									*/
/************************************************************************/

static uLong dc_writerec (OZ_Dcmpb *dcmpb, uLong status);

static uLong sc_writerec (Iopex *iopex, Chnex *chnex, Devex *devex)

{
  OZ_Dbn efblk, ewblk;
  uLong blocksize, efbyt, ewbyt, sts, vl;

  blocksize = devex -> blocksize;

  // Maybe file attributes force write-thru mode

  if (devex -> mountflags & OZ_FS_MOUNTFLAG_WRITETHRU) iopex -> u.writerec.p.writethru = 1;

  /* If atblock specified, position there */

  if (iopex -> u.writerec.p.atblock != 0) {
    chnex -> curblock = iopex -> u.writerec.p.atblock;
    chnex -> curbyte = iopex -> u.writerec.p.atbyte;
  }
  chnex -> curblock += chnex -> curbyte / blocksize;
  chnex -> curbyte %= blocksize;

  /* Maybe position to or set the end-of-file marker for the file */

  iopex -> u.writerec.updateof = 0;					// so far, we don't update the eof position

  if (iopex -> u.writerec.p.append) {					// if we're appending, position to the end-of-file
    vl = oz_hw_smplock_wait (&(devex -> smplock_vl));
    chnex -> curblock = devex -> eofblock;
    chnex -> curbyte = devex -> eofbyte;
    oz_hw_smplock_clr (&(devex -> smplock_vl), vl);
  }

  ewblk  = chnex -> curblock;						// calculate where our write will end
  if (ewblk == 0) return (OZ_VBNZERO);
  ewbyt  = chnex -> curbyte;
  ewbyt += iopex -> u.writerec.p.size + iopex -> u.writerec.p.trmsize;
  ewblk += ewbyt / blocksize;
  ewbyt %= blocksize;

  if (ewblk > devex -> blockcount) return (OZ_DISKISFULL);		// see if write will go beyond end of allocated space

  /* Write as much as we can to current position from caller's buffer */

  iopex -> u.writerec.wlen    = 0;					// nothing written from user's buffer yet
  iopex -> u.writerec.trmwlen = 0;					// nothing written from terminator yet

  iopex -> u.writerec.dcmpb.dcache    = devex -> dcache;
  iopex -> u.writerec.dcmpb.writing   = 1;				// this is a disk write operation
  iopex -> u.writerec.dcmpb.nbytes    = iopex -> u.writerec.p.size + iopex -> u.writerec.p.trmsize;
  iopex -> u.writerec.dcmpb.virtblock = chnex -> curblock;		// virtual block we want to start writing at
  iopex -> u.writerec.dcmpb.logblock  = chnex -> curblock - 1;		// equivalent logical block
  iopex -> u.writerec.dcmpb.blockoffs = chnex -> curbyte;		// byte we want to start writing at
  iopex -> u.writerec.dcmpb.entry     = dc_writerec;			// write routine entrypoint
  iopex -> u.writerec.dcmpb.param     = iopex;				// write routine parameter
  iopex -> u.writerec.dcmpb.writethru = iopex -> u.writerec.p.writethru; // set up writethru mode flag
  iopex -> u.writerec.dcmpb.ix4kbuk   = 0;
  iopex -> u.writerec.status          = OZ_SUCCESS;			// in case of zero bytes

  sts = oz_knl_dcache_map (&(iopex -> u.writerec.dcmpb));		// process the request
  if (sts != OZ_STARTED) dc_writerec (&(iopex -> u.writerec.dcmpb), sts);
  return (OZ_STARTED);
}

/************************************************************************/
/*									*/
/*  The disk cache system calls this routine when it has access to the 	*/
/*  cache blocks for the disk						*/
/*									*/
/************************************************************************/

static uLong dc_writerec (OZ_Dcmpb *dcmpb, uLong status)

{
  Chnex *chnex;
  Devex *devex;
  const OZ_Mempage *phypages;
  Iopex *iopex;
  Long alf;
  uLong blocksize, modified, skip, size, vl;

  iopex = dcmpb -> param;
  chnex = iopex -> chnex;
  devex = iopex -> devex;
  modified = 0;

  /* Maybe we are all finished up */

  if (status != OZ_PENDING) {
    if (status == OZ_SUCCESS) status = iopex -> u.writerec.status;

    /* Ok, update the current and end-of-file pointers */

    if (status == OZ_SUCCESS) {
      blocksize = devex -> blocksize;
      chnex -> curbyte  += iopex -> u.writerec.p.size + iopex -> u.writerec.p.trmsize;
      chnex -> curblock += chnex -> curbyte / blocksize;
      chnex -> curbyte  %= blocksize;
      vl = oz_hw_smplock_wait (&(devex -> smplock_vl));
      if (iopex -> u.writerec.p.truncate || (chnex -> curblock > devex -> eofblock) || ((chnex -> curblock == devex -> eofblock) && (chnex -> curbyte > devex -> eofbyte))) {
        devex -> eofblock = chnex -> curblock;
        devex -> eofbyte  = chnex -> curbyte;
      }
      oz_hw_smplock_clr (&(devex -> smplock_vl), vl);
    }

    /* Copy the written length back to user's buffer */

    if (iopex -> u.writerec.p.wlen != NULL) oz_hw_phys_movefromvirt (sizeof *(iopex -> u.writerec.p.wlen), 
                                                                     &(iopex -> u.writerec.wlen), 
                                                                     iopex -> u.writerec.wlen_phypages, 
                                                                     iopex -> u.writerec.wlen_byteoffs);

    /* Post I/O request's completion and maybe start another recio on the file */

    finishrecio (iopex, status, NULL);
    return (0);
  }

  /* Copy unwritten data bytes from user's buffer to cache page */

  size = iopex -> u.writerec.p.size - iopex -> u.writerec.wlen;		// this is how much of the user buffer we have yet to put
  if (size > dcmpb -> nbytes) size = dcmpb -> nbytes;			// can't put more than cache is making available
  if (size > 0) {
    oz_hw_phys_movephys (size, iopex -> u.writerec.phypages, iopex -> u.writerec.byteoffs + iopex -> u.writerec.wlen, &(dcmpb -> phypage), dcmpb -> pageoffs);
    iopex -> u.writerec.wlen += size;					// that much more has been written to cache
    dcmpb -> nbytes   -= size;						// remove from what cache has made available to us
    dcmpb -> pageoffs += size;
    modified = size;
  }

  /* Copy unwritten terminator bytes to cache page */

  size = iopex -> u.writerec.p.trmsize - iopex -> u.writerec.trmwlen;	// this is how much of the terminator buffer we have yet to put
  if (size > dcmpb -> nbytes) size = dcmpb -> nbytes;			// can't put more than cache is making available
  if (size > 0) {
    if (iopex -> u.writerec.p.trmsize > sizeof iopex -> u.writerec.trmdata) {
      oz_hw_phys_movephys (size, iopex -> u.writerec.trmphypages,  iopex -> u.writerec.trmbyteoffs + iopex -> u.writerec.trmwlen, &(dcmpb -> phypage), dcmpb -> pageoffs);
    } else {
      oz_hw_phys_movefromvirt (size, iopex -> u.writerec.trmdata + iopex -> u.writerec.trmwlen, &(dcmpb -> phypage), dcmpb -> pageoffs);
    }
    iopex -> u.writerec.trmwlen += size;				// that much more has been written to cache
    dcmpb -> nbytes   -= size;						// remove from what cache has made available to us
    dcmpb -> pageoffs += size;
    modified += size;
  }

  /* Get more from cache if we haven't written it all yet.  Else set nbytes=0 so oz_knl_dcache_map will know we're all done. */

  size = iopex -> u.writerec.p.size + iopex -> u.writerec.p.trmsize;
  skip = iopex -> u.writerec.wlen   + iopex -> u.writerec.trmwlen;

  iopex -> u.writerec.dcmpb.nbytes    = size - skip;
  iopex -> u.writerec.dcmpb.virtblock = chnex -> curblock;
  iopex -> u.writerec.dcmpb.blockoffs = chnex -> curbyte + skip;
  iopex -> u.writerec.dcmpb.logblock  = chnex -> curblock - 1;

  /* Return number of bytes we modified in the cache page */

  return (modified);
}

/************************************************************************/
/*									*/
/*  Read record								*/
/*									*/
/*  This is a shortcut routine, but there is only one request active 	*/
/*  per file at a time, as they go through file -> recio_q.		*/
/*									*/
/************************************************************************/

static uLong dc_readrec (OZ_Dcmpb *dcmpb, uLong status);

static uLong sc_readrec (Iopex *iopex, Chnex *chnex, Devex *devex)

{
  uLong blocksize, dcsize, sts, vl;

  /* If atblock specified, position there */

  blocksize = devex -> blocksize;
  if (iopex -> u.readrec.p.atblock != 0) {
    chnex -> curblock = iopex -> u.readrec.p.atblock;
    chnex -> curbyte  = iopex -> u.readrec.p.atbyte;
  }
  chnex -> curblock += chnex -> curbyte / blocksize;
  chnex -> curbyte  %= blocksize;

  /* If we're at or past the eof, return eof status */

  vl = oz_hw_smplock_wait (&(devex -> smplock_vl));
  iopex -> u.readrec.efblk = devex -> eofblock;
  iopex -> u.readrec.efbyt = devex -> eofbyte;
  oz_hw_smplock_clr (&(devex -> smplock_vl), vl);

  sts = OZ_ENDOFFILE;
  if (chnex -> curblock > iopex -> u.readrec.efblk) goto rtnerr;
  if ((chnex -> curblock == iopex -> u.readrec.efblk) && (chnex -> curbyte >= iopex -> u.readrec.efbyt)) goto rtnerr;

  /* Read as much as we can from current position into caller's buffer */

  iopex -> u.readrec.rlen    = 0;					// nothing read into user's buffer yet
  iopex -> u.readrec.trmseen = 0;					// haven't seen any of the terminator yet

  iopex -> u.readrec.dcmpb.dcache    = devex -> dcache;
  iopex -> u.readrec.dcmpb.writing   = 0;				// this is a disk read operation
  iopex -> u.readrec.dcmpb.nbytes    = iopex -> u.readrec.p.size + iopex -> u.readrec.p.trmsize;
  iopex -> u.readrec.dcmpb.virtblock = chnex -> curblock;		// virtual block we want to start reading at
  iopex -> u.readrec.dcmpb.blockoffs = chnex -> curbyte;		// byte we want to start reading at
  iopex -> u.readrec.dcmpb.entry     = dc_readrec;			// read routine entrypoint
  iopex -> u.readrec.dcmpb.param     = iopex;				// read routine parameter
  iopex -> u.readrec.dcmpb.logblock  = chnex -> curblock - 1;		// corresponding logical block
  iopex -> u.readrec.dcmpb.ix4kbuk   = 0;
  iopex -> u.readrec.status          = OZ_SUCCESS;			// in case nbytes is zero

  sts = oz_knl_dcache_map (&(iopex -> u.readrec.dcmpb));		// process the request
  if (sts != OZ_STARTED) dc_readrec (&(iopex -> u.readrec.dcmpb), sts);
  return (OZ_STARTED);

rtnerr:
  if (iopex -> u.readrec.p.trmsize > sizeof iopex -> u.readrec.trmdata) OZ_KNL_PGPFREE (iopex -> u.readrec.trmbuff);
  return (sts);
}

/************************************************************************/
/*									*/
/*  The disk cache system calls this routine when it has access to the 	*/
/*  cache blocks for the disk						*/
/*									*/
/************************************************************************/

static uLong dc_readrec (OZ_Dcmpb *dcmpb, uLong status)

{
  Chnex *chnex;
  const OZ_Mempage *phypages;
  Iopex *iopex;
  OZ_Dbn efblk, epblk;
  OZ_Pagentry savepte;
  uByte *dcbuff, *p;
  uLong blocksize, efbyt, epbyt, skip, size, sts;

  iopex = dcmpb -> param;
  chnex = iopex -> chnex;
  blocksize = iopex -> devex -> blocksize;

  /* Maybe oz_knl_dcache_map is all done with dcmpb.  If so, post request completion. */

  if (status != OZ_PENDING) {
    if (status == OZ_SUCCESS) status = iopex -> u.readrec.status;
    chnex -> curbyte += iopex -> u.readrec.rlen + iopex -> u.readrec.trmseen;
    chnex -> curblock += chnex -> curbyte / blocksize;
    chnex -> curbyte %= blocksize;

    if (iopex -> u.readrec.p.trmsize > sizeof iopex -> u.readrec.trmdata) OZ_KNL_PGPFREE (iopex -> u.readrec.trmbuff);

    if (iopex -> u.readrec.p.rlen != NULL) oz_hw_phys_movefromvirt (sizeof *(iopex -> u.readrec.p.rlen), 
                                                                    &(iopex -> u.readrec.rlen), 
                                                                    iopex -> u.readrec.rlen_phypages, 
                                                                    iopex -> u.readrec.rlen_byteoffs);

    finishrecio (iopex, status, NULL);
    return (0);
  }

  /* Chop nbytes off at the end-of-file */

  skip   = iopex -> u.readrec.rlen - iopex -> u.readrec.trmseen;	// calculate where nbytes of the cache would put us
  epblk  = chnex -> curblock;
  epbyt  = chnex -> curbyte + skip + dcmpb -> nbytes;
  epblk += epbyt / blocksize;
  epbyt %= blocksize;
  efblk  = iopex -> u.readrec.efblk;					// get end-of-file pointer
  efbyt  = iopex -> u.readrec.efbyt;
  if ((epblk > efblk) || ((epblk == efblk) && (epbyt > efbyt))) {	// if nbytes goes beyond eof ...
    dcmpb -> nbytes = (efblk - chnex -> curblock) * blocksize + efbyt - chnex -> curbyte - skip; // just read up to the eof
    if (dcmpb -> nbytes == 0) {
      skip     = iopex -> u.readrec.rlen + iopex -> u.readrec.byteoffs;
      phypages = iopex -> u.readrec.phypages;
      if (iopex -> u.readrec.rlen + iopex -> u.readrec.trmseen <= iopex -> u.readrec.p.size) {
        iopex -> u.readrec.rlen += iopex -> u.readrec.trmseen;
        iopex -> u.readrec.status = OZ_ENDOFFILE;			// return end-of-file as there is nothing more to read
      } else {
        iopex -> u.readrec.trmseen = iopex ->  u.readrec.p.size - iopex -> u.readrec.rlen;
        iopex -> u.readrec.rlen = iopex -> u.readrec.p.size;		// return success so they come back and 
									// get the rest of partial terminator
      }
      oz_hw_phys_movefromvirt (iopex -> u.readrec.trmseen, iopex -> u.readrec.trmbuff, phypages, skip);
      iopex -> u.readrec.trmseen = 0;					// partial terminator was put in data buffer
      goto rtn;
    }
  }

  /* Maybe this is a continuation of a multi-byte terminator sequence       */
  /* iopex -> u.readrec.trmseen has how many bytes have been matched so far */

  if (iopex -> u.readrec.trmseen != 0) {				// see if part of terminator seen last time
    size = iopex -> u.readrec.p.trmsize - iopex -> u.readrec.trmseen;	// get how much of the terminator we have yet to find
    if (size > dcmpb -> nbytes) size = dcmpb -> nbytes;			// ... but we won't find more than what cache has given us
    dcbuff  = oz_hw_phys_mappage (dcmpb -> phypage, &savepte);		// point to cache buffer data
    dcbuff += dcmpb -> pageoffs;					// offset to the byte we want to start with
    p = iopex -> u.readrec.trmbuff + iopex -> u.readrec.trmseen;	// point to what's left of terminator to match
    for (sts = 0; sts < size; sts ++) if (*(dcbuff ++) != *(p ++)) break; // see how much of it matches
    oz_hw_phys_unmappage (savepte);
    iopex -> u.readrec.trmseen += sts;					// this much more of the terminator has been matched up
    if (iopex -> u.readrec.trmseen == iopex -> u.readrec.p.trmsize) {	// read is complete if whole terminator found
      iopex -> u.readrec.status = OZ_SUCCESS;
      goto readdone;
    }
    if (sts < size) {
      iopex -> u.readrec.trmseen = 0;					// terminator broken, forget about it
      skip      = iopex -> u.readrec.rlen + iopex -> u.readrec.byteoffs;
      phypages  = iopex -> u.readrec.phypages;
      oz_hw_phys_movefromvirt (1, iopex -> u.readrec.trmbuff, phypages, skip); // but copy first byte to data buffer
      iopex -> u.readrec.rlen ++;
    }
    goto continuereading;
  }

  /* See how much of what it gave will fit in user data buffer */

  size = iopex -> u.readrec.p.size - iopex -> u.readrec.rlen;
  if (size > dcmpb -> nbytes) size = dcmpb -> nbytes;

  /* If no terminator specified, copy all of it to user buffer */

  if (iopex -> u.readrec.p.trmsize == 0) {
    oz_hw_phys_movephys (size, &(dcmpb -> phypage), dcmpb -> pageoffs, iopex -> u.readrec.phypages, iopex -> u.readrec.byteoffs + iopex -> u.readrec.rlen);
    iopex -> u.readrec.rlen += size;					// accumulate whatever was read in
    if (iopex -> u.readrec.rlen < iopex -> u.readrec.p.size) goto continuereading; // continue reading if we haven't filled buffer
  }

  /* Terminator processing */

  else {
    dcbuff  = oz_hw_phys_mappage (dcmpb -> phypage, &savepte);		// point to cache buffer data we just copied from
    dcbuff += dcmpb -> pageoffs;
    p = memchr (dcbuff, iopex -> u.readrec.trmbuff[0], size);		// search for the first byte of the terminator

    while (p != NULL) {
      sts = dcbuff + dcmpb -> nbytes - p;				// this is how much remains at first terminator byte
      if (sts > iopex -> u.readrec.p.trmsize) sts = iopex -> u.readrec.p.trmsize; // only match up to whole terminator
      if (memcmp (p, iopex -> u.readrec.trmbuff, sts) == 0) break;	// stop if successful comparison
      p = memchr (p + 1, iopex -> u.readrec.trmbuff[0], sts - 1);	// mismatch, look for first byte again
    }

    oz_hw_phys_unmappage (savepte);					// unmap cache page

    if (p == NULL) {
      oz_hw_phys_movephys (size, &(dcmpb -> phypage), dcmpb -> pageoffs, iopex -> u.readrec.phypages, iopex -> u.readrec.byteoffs + iopex -> u.readrec.rlen);
      iopex -> u.readrec.rlen += size;					// no match found, so it is all data
      if (iopex -> u.readrec.rlen < iopex -> u.readrec.p.size) goto continuereading; // continue reading
      iopex -> u.readrec.status = OZ_NOTERMINATOR;			// user buffer filled with no terminator, return semi-error status
      goto readdone;
    }
    size = p - dcbuff;							// length up to but not including start of terminator
    oz_hw_phys_movephys (size, &(dcmpb -> phypage), dcmpb -> pageoffs, iopex -> u.readrec.phypages, iopex -> u.readrec.byteoffs + iopex -> u.readrec.rlen);
    iopex -> u.readrec.rlen   += size;
    iopex -> u.readrec.trmseen = sts;					// set the size matched so far
    if (sts < iopex -> u.readrec.p.trmsize) goto continuereading;	// haven't found the whole thing, continue reading
  }

  iopex -> u.readrec.status = OZ_SUCCESS;
  goto readdone;

  /* Request requires more data to complete */

continuereading:
  size = iopex -> u.readrec.p.size + iopex -> u.readrec.p.trmsize;
  skip = iopex -> u.readrec.rlen   + iopex -> u.readrec.trmseen;

  dcmpb -> nbytes = size - skip;
  dcmpb -> virtblock = chnex -> curblock;
  dcmpb -> blockoffs = chnex -> curbyte + skip;
  dcmpb -> logblock  = chnex -> curblock - 1;
  goto rtn;

  /* Read request is complete */

readdone:
  dcmpb -> nbytes = 0;
rtn:
  return (0);
}

/************************************************************************/
/*									*/
/*  Set up crash dump file						*/
/*									*/
/************************************************************************/

static uLong sc_crash (Iopex *iopex, Chnex *chnex, Devex *devex)

{
  OZ_Dbn efblk;
  uLong sts;

  sts = OZ_SUCCESS;

  if (crash_devex != NULL) crash_devex = NULL;

  if (iopex -> ap != NULL) {
    if (iopex -> as != sizeof (OZ_IO_fs_crash)) return (OZ_BADBUFFERSIZE);

    /* Get disk drive crash info */

    memset (&crash_disk, 0, sizeof crash_disk);
    sts = oz_knl_io (devex -> master_iochan, OZ_IO_DISK_CRASH, sizeof crash_disk, &crash_disk);
    if (sts != OZ_SUCCESS) {
      printk (iopex, "oz_dev_dpt: error %u enabling crash disk\n", sts);
      return (sts);
    }
    crash_devex = devex;

    /* Return file info to caller */

    efblk = devex -> eofblock;
    if (devex -> eofbyte == 0) efblk --;

    ((OZ_IO_fs_crash *)(iopex -> ap)) -> crashentry = disk_fs_crash;
    ((OZ_IO_fs_crash *)(iopex -> ap)) -> blocksize  = devex -> blocksize;
    ((OZ_IO_fs_crash *)(iopex -> ap)) -> filesize   = efblk;
  }

  return (sts);
}

/************************************************************************/
/*									*/
/*  Enqueue an record I/O request to channel and start it when ready	*/
/*									*/
/************************************************************************/

static uLong queuerecio (Iopex *iopex, Chnex *chnex, Devex *devex)

{
  uLong sts, vl;

  iopex -> next = NULL;						// it will go on end of the queue
  vl = oz_hw_smplock_wait (&(devex -> smplock_vl));		// lock the drive's record-io request queue
  if (devex -> iopexip == NULL) {				// see if there is a record-io request already in progress on the channel
    devex -> iopexip = iopex;					// if not, make this one the 'in progress' request
  } else {
    *(devex -> iopexqt) = iopex;				// if so, put this request on the end of the queue
    devex -> iopexqt = &(iopex -> next);
    iopex = NULL;
  }
  oz_hw_smplock_clr (&(devex -> smplock_vl), vl);		// unlock the queue
  if (iopex != NULL) {
    sts = (*(iopex -> backend)) (iopex, chnex, devex);		// we marked this one 'in progress' so start processing it
    if (sts != OZ_STARTED) {
      finishrecio (iopex, sts, NULL);				// maybe it finished synchronously
      sts = OZ_STARTED;
    }
  }
  return (sts);
}

/************************************************************************/
/*									*/
/*  Finish the 'in progress' recio requeust for the channel		*/
/*									*/
/************************************************************************/

static void finishrecio (Iopex *iopex, uLong status, void (*finentry) (void *iopexv, int finok, uLong *status_r))

{
  Chnex *chnex;
  Devex *devex;
  Iopex *newiopex;
  uLong sts, vl;

  chnex = iopex -> chnex;
  devex = iopex -> devex;
  sts   = status;

  // Complete the 'in progress' request and try to dequeue the next one

loop:
  vl = oz_hw_smplock_wait (&(devex -> smplock_vl));
  if (devex -> iopexip != iopex) oz_crash ("oz_dev_dpt finishrecio: request %p not in progress (%p is)", iopex, devex -> iopexip);
  newiopex = devex -> iopexqh;
  if (newiopex != NULL) {
    if ((devex -> iopexqh = newiopex -> next) == NULL) devex -> iopexqt = &(devex -> iopexqh);
  }
  devex -> iopexip = newiopex;
  oz_hw_smplock_clr (&(devex -> smplock_vl), vl);

  oz_knl_iodone (iopex -> ioop, sts, NULL, finentry, iopex);
  finentry = NULL;

  // See if there are any waiting in the queue

  iopex = newiopex;

  // If so, call its routine to start it going.  If it completes synchronously, then finish it off, too.

  if (iopex != NULL) {
    chnex = iopex -> chnex;
    sts   = (*(iopex -> backend)) (iopex, chnex, devex);
    if (sts != OZ_STARTED) goto loop;
  }
}

/************************************************************************/
/*									*/
/*  Dismount volume							*/
/*									*/
/*    Input:								*/
/*									*/
/*	volume = volume to be dismounted				*/
/*	unload = 0 : leave volume online				*/
/*	         1 : unload volume (if possible)			*/
/*									*/
/*    Output:								*/
/*									*/
/*	volume dismounted						*/
/*									*/
/************************************************************************/

static uLong dismount_volume (int unload, int shutdown, Iopex *iopex)

{
  Devex *devex;
  OZ_IO_disk_setvolvalid disk_setvolvalid;
  uLong sts, vl;

  devex = iopex -> devex;

  /* Flush and deactivate cache */

  if (devex -> dcache != NULL) {
    oz_knl_dcache_term (devex -> dcache, 0);
    devex -> dcache = NULL;
  }

  /* Clear volume valid flag and maybe unload volume */

  memset (&disk_setvolvalid, 0, sizeof disk_setvolvalid);
  disk_setvolvalid.unload = unload;
  sts = oz_knl_iostart3 (1, NULL, devex -> master_iochan, OZ_PROCMODE_KNL, dismount_unloaded, iopex, 
                         NULL, NULL, NULL, NULL, OZ_IO_DISK_SETVOLVALID, sizeof disk_setvolvalid, &disk_setvolvalid);
  if (sts != OZ_STARTED) dismount_unloaded (iopex, sts);
  return (OZ_STARTED);
}

static void dismount_unloaded (void *iopexv, uLong status)

{
  Devex *devex;
  Iopex *iopex;

  iopex = iopexv;
  devex = iopex -> devex;

  oz_knl_iochan_increfc (devex -> master_iochan, -1);
  devex -> master_iochan = NULL;
  oz_knl_iodone (iopex -> ioop, status, NULL, NULL, NULL);
}

/************************************************************************/
/*									*/
/*  Write segment of crash dump file					*/
/*									*/
/************************************************************************/

static uLong disk_fs_crash (void *dummy, OZ_Dbn vbn, uLong size, OZ_Mempage phypage, uLong offset)

{
  OZ_Dbn lbn, nblocks;
  uLong blocksize, sts, wsize;

  if (crash_devex == NULL) return (OZ_FILENOTOPEN);
  if (vbn == 0) return (OZ_VBNZERO);
  lbn = vbn - 1;
  if (lbn >= crash_devex -> blockcount) return (OZ_ENDOFFILE);
  if (lbn + (wsize / crash_devex -> blockcount) >= crash_devex -> blockcount) return (OZ_ENDOFFILE);

  blocksize = crash_disk.blocksize;					/* get file = disk's block size */
  nblocks   = crash_devex -> blockcount - lbn;				/* this is how many blocks left */
  while (size > 0) {							/* repeat as long as there's stuff to do */
    wsize = size;							/* try to write all that's left to do */
    if (wsize > nblocks * blocksize) wsize = nblocks * blocksize;	/* but don't write more than what we have left */
    sts = (*(crash_disk.crashentry)) (crash_disk.crashparam, lbn, wsize, phypage, offset); /* write to disk */
    if (sts != OZ_SUCCESS) {
      oz_knl_printk ("oz_dev_dpt: error %u writing crash file vbn %u\n", sts, vbn);
      return (sts);
    }
    size    -= wsize;							/* subtract what was written */
    offset  += wsize;							/* increment page offset */
    phypage += offset >> OZ_HW_L2PAGESIZE;				/* add overflow to page number */
    offset  &= (1 << OZ_HW_L2PAGESIZE) - 1;				/* get offset within the page */

    wsize   /= blocksize;						/* see how many blocks were written */
    nblocks -= wsize;							/* decrement number of blocks mapped */
    lbn     += wsize;							/* increment the next lbn */
    vbn     += wsize;							/* increment the next vbn */
  }

  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Print error message to both OZ_ERROR and the kernel console		*/
/*									*/
/************************************************************************/

static void printk (Iopex *iopex, const char *format, ...)

{
  char buff[256];
  uLong sts;
  OZ_Iochan *iochan;
  OZ_Logname *logname, *logtable;
  OZ_Process *process;
  OZ_Thread *thread;
  va_list ap;

  va_start (ap, format);

  /* Print to console */

  oz_knl_printkv (format, ap);

  /* Get process table that the I/O request came from */

  thread = oz_knl_ioop_getthread (iopex -> ioop);
  if (thread == NULL) return;
  process = oz_knl_thread_getprocess (thread);
  if (process == NULL) return;
  logtable = oz_knl_process_getlognamtbl (process);
  if (logtable == NULL) return;

  /* Find OZ_ERROR logical name */

  sts = oz_knl_logname_lookup (logtable, OZ_PROCMODE_KNL, 8, "OZ_ERROR", NULL, NULL, NULL, NULL, &logname, NULL);
  if (sts != OZ_SUCCESS) {
    if (sts != OZ_NOLOGNAME) oz_knl_printk ("oz_dev_dpt printk: error %u looking up OZ_ERROR\n", sts);
    return;
  }

  /* Get I/O channel assigned to OZ_ERROR */

  sts = oz_knl_logname_getobj (logname, 0, OZ_OBJTYPE_IOCHAN, &iochan);
  oz_knl_logname_increfc (logname, -1);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_dev_dpt printk: error %u converting OZ_ERROR to io channel\n", sts);
    return;
  }

  /* Output message to OZ_ERROR console */

  oz_sys_vxprintf (startprintk, iochan, sizeof buff, buff, NULL, format, ap);
  oz_knl_iochan_increfc (iochan, -1);

  va_end (ap);
}

/* Output to OZ_ERROR only */

static void printe (Iopex *iopex, const char *format, ...)

{
  char buff[256];
  uLong sts;
  OZ_Iochan *iochan;
  OZ_Logname *logname, *logtable;
  OZ_Process *process;
  OZ_Thread *thread;
  va_list ap;

  va_start (ap, format);

  /* Get process table that the I/O request came from */

  thread = oz_knl_ioop_getthread (iopex -> ioop);
  if (thread == NULL) return;
  process = oz_knl_thread_getprocess (thread);
  if (process == NULL) return;
  logtable = oz_knl_process_getlognamtbl (process);
  if (logtable == NULL) return;

  /* Find OZ_ERROR logical name */

  sts = oz_knl_logname_lookup (logtable, OZ_PROCMODE_KNL, 8, "OZ_ERROR", NULL, NULL, NULL, NULL, &logname, NULL);
  if (sts != OZ_SUCCESS) {
    if (sts != OZ_NOLOGNAME) oz_knl_printk ("oz_dev_dpt printk: error %u looking up OZ_ERROR\n", sts);
    return;
  }

  /* Get I/O channel assigned to OZ_ERROR */

  sts = oz_knl_logname_getobj (logname, 0, OZ_OBJTYPE_IOCHAN, &iochan);
  oz_knl_logname_increfc (logname, -1);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_dev_dpt printk: error %u converting OZ_ERROR to io channel\n", sts);
    return;
  }

  /* Output message to OZ_ERROR console */

  oz_sys_vxprintf (startprintk, iochan, sizeof buff, buff, NULL, format, ap);
  oz_knl_iochan_increfc (iochan, -1);

  va_end (ap);
}

static uLong startprintk (void *iochanv, uLong *size, char **buff)

{
  uLong sts;
  OZ_IO_console_write console_write;

  /* Use CONSOLE_WRITE function so we can't possibly be writing to ourself */

  memset (&console_write, 0, sizeof console_write);
  console_write.size = *size;
  console_write.buff = *buff;
  sts = oz_knl_io (iochanv, OZ_IO_CONSOLE_WRITE, sizeof console_write, &console_write);
  if (sts != OZ_SUCCESS) oz_knl_printk ("oz_dev_dpt printk: error %u writing to OZ_ERROR\n", sts);
  return (sts);
}
