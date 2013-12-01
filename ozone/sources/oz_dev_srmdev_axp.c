//+++2004-01-03
//    Copyright (C) 2001,2002,2003,2004  Mike Rieker, Beverly, MA USA
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
//---2004-01-03

/************************************************************************/
/*									*/
/*  This driver uses the SRM console routines to access system 		*/
/*  devices.  Although it is not efficient, it should work for any 	*/
/*  SRM-accessible device, but we currently only support disks.		*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_io_disk.h"
#include "oz_knl_devio.h"
#include "oz_knl_hw.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_misc.h"
#include "oz_knl_procmode.h"
#include "oz_knl_sdata.h"
#include "oz_knl_status.h"

#define L2BLOCKSIZE 9
#define BLOCKSIZE (1 << L2BLOCKSIZE)
#define PAGESIZE (1 << OZ_HW_L2PAGESIZE)
#define BUFFALIGN 7

typedef struct { OZ_Dbn totalblocks;
                 uLong channel;
                 int valid;
                 char suffix[20];
               } Ddevex;

static uLong null_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnex, OZ_Procmode procmode, 
                         OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap);

static OZ_Devfunc null_functable = { 0, 0, 0, 0, NULL, NULL, NULL, NULL, NULL, NULL, null_start, NULL };

static int disk_clonedel (OZ_Devunit *devunit, void *devexv, int cloned);
static uLong disk_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnex, OZ_Procmode procmode, 
                         OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap);

static OZ_Devfunc disk_functable = { sizeof (Ddevex), 0, 0, 0, NULL, NULL, disk_clonedel, NULL, NULL, NULL, disk_start, NULL };

static int initialized = 0;
static OZ_Devunit *null_devunit;
static OZ_Devclass *disk_devclass, *null_devclass;
static OZ_Devdriver *disk_devdriver, *null_devdriver;

static OZ_Devunit *srmdev_auto (void *dummy, OZ_Devunit *host_devunit, char const *devname, char const *suffix);
static uLong readwritep (Ddevex *ddevex, uQuad func, uLong size, OZ_Mempage const *pages, uLong offset, OZ_Dbn slbn);
static uLong readwritev (Ddevex *ddevex, uQuad func, uLong size, void const *buff, OZ_Dbn slbn);

/************************************************************************/
/*									*/
/*  Boot-time initialization routine					*/
/*									*/
/************************************************************************/

void oz_dev_srmdev_init ()

{
  uLong sts;

  if (!initialized) {
    oz_knl_printk ("oz_dev_srmdev_init\n");

    null_devclass  = oz_knl_devclass_create ("null", 0, 0, "oz_dev_srmdev");
    null_devdriver = oz_knl_devdriver_create (null_devclass, "oz_dev_srmdev");
    null_devunit   = oz_knl_devunit_create (null_devdriver, "srm", "alpha srm devs, eg, srm.dka0", &null_functable, 0, oz_s_secattr_sysdev);

    disk_devclass  = oz_knl_devclass_create (OZ_IO_DISK_CLASSNAME, OZ_IO_DISK_BASE, OZ_IO_DISK_MASK, "oz_dev_srmdev");
    disk_devdriver = oz_knl_devdriver_create (disk_devclass, "oz_dev_srmdev");

    oz_knl_devunit_autogen (null_devunit, srmdev_auto, NULL);

    initialized  = 1;
  }
}

/************************************************************************/
/*									*/
/*  This routine is called when an access is made to a non-existant 	*/
/*  unit, eg, the first access to srm.dka0 or srm.dka400, etc.		*/
/*									*/
/*    Input:								*/
/*									*/
/*	dummy = NULL							*/
/*	host_devunit = the 'srm' device unit				*/
/*	devname = the whole desired string, eg, srm.dka0		*/
/*	suffix = portion of devname to create, eg, dka0			*/
/*	smplevel = softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	srmdev_auto = NULL : unable to create device			*/
/*	              else : points to created device			*/
/*									*/
/************************************************************************/

static OZ_Devunit *srmdev_auto (void *dummy, OZ_Devunit *host_devunit, char const *devname, char const *suffix)

{
  char unitdesc[OZ_DEVUNIT_DESCSIZE];
  Ddevex *ddevex;
  OZ_Dbn block, hiblock;
  OZ_Devunit *devunit;
  uLong channel;
  uQuad blockbuff[BLOCKSIZE/8], rq;

  /* All we do are disks */

  if (suffix[0] != 'd') {
    oz_knl_printk ("oz_dev_srmdev: can't do %s, only smart enough to do disks\n", suffix);
    return (NULL);
  }

  rq = strlen (suffix);
  if (rq >= sizeof ddevex -> suffix) {
    oz_knl_printk ("oz_dev_srmdev: suffix string %s too int\n", suffix);
    return (NULL);
  }

  /* Open SRM channel to disk */

  {
    register uQuad __r0  asm ("$0");
    register uQuad __r16 asm ("$16") = 0x10;	// OPEN
    register uQuad __r17 asm ("$17") = (uQuad)suffix;
    register uQuad __r18 asm ("$18") = rq;
    register uQuad __r27 asm ("$27") = oz_hwaxp_dispatchr27;

    asm volatile ("jsr $26,(%1)" : "=r"(__r0)
                                 : "r"(oz_hwaxp_dispatchent), "r"(__r16), "r"(__r17), "r"(__r18), "r"(__r27)
                                 : "$1", "$16", "$17", "$18", "$19", "$20", "$21", "$22", "$23", "$24", "$25", "$26", "$27", "$28");
    rq = __r0;
  }
  channel = (uLong)rq;

  if ((rq >> 62) != 0) {
    oz_knl_printk ("oz_dev_srmdev: error %QX opening %s\n");
    return (NULL);
  }

  /* Determine total number of blocks on the disk */
  /* (Do we want to do this each setvalid for CD's of different sizes??) */

  hiblock = 0;
  for (block = 0x80000000U; block != 0; block >>= 1) {
    hiblock += block;
    {
      register uQuad __r0  asm ("$0");
      register uQuad __r16 asm ("$16") = 0x13;
      register uQuad __r17 asm ("$17") = channel;
      register uQuad __r18 asm ("$18") = BLOCKSIZE;
      register uQuad __r19 asm ("$19") = (uQuad)blockbuff;
      register uQuad __r20 asm ("$20") = hiblock;
      register uQuad __r27 asm ("$27") = oz_hwaxp_dispatchr27;

      asm volatile ("jsr $26,(%1)" : "=r"(__r0)
                                   : "r"(oz_hwaxp_dispatchent), "r"(__r16), "r"(__r17), "r"(__r18), "r"(__r19), "r"(__r20), "r"(__r27)
                                   : "$1", "$16", "$17", "$18", "$19", "$20", "$21", "$22", "$23", "$24", "$25", "$26", "$27", "$28");
      rq = __r0;
    }
    if ((rq & 0xC0000000FFFFFFFFULL) != BLOCKSIZE) hiblock -= block;
  }

  /* Create disk structure */

  oz_sys_sprintf (sizeof unitdesc, unitdesc, "%u blocks", ++ hiblock);
  devunit = oz_knl_devunit_create (disk_devdriver, devname, unitdesc, &disk_functable, 0, oz_s_secattr_sysdev);
  ddevex  = oz_knl_devunit_ex (devunit);
  memset (ddevex, 0, sizeof *ddevex);
  ddevex -> totalblocks = hiblock;
  ddevex -> channel = channel;
  strncpyz (ddevex -> suffix, suffix, sizeof ddevex -> suffix);

  return (devunit);
}

/************************************************************************/
/*									*/
/*  Called when the last channel is deassigned from the unit		*/
/*									*/
/*    Input:								*/
/*									*/
/*	devex = ddevex being closed out					*/
/*	smplevel = dv spinlock						*/
/*									*/
/*    Output:								*/
/*									*/
/*	disk_clonedel = 1 : it's OK to delete the unit			*/
/*									*/
/************************************************************************/

static int disk_clonedel (OZ_Devunit *devunit, void *devexv, int cloned)

{
  Ddevex *ddevex;

  ddevex = devexv;

  {
    register uQuad __r0  asm ("$0");
    register uQuad __r16 asm ("$16") = 0x11;	// CLOSE
    register uQuad __r17 asm ("$17") = ddevex -> channel;
    register uQuad __r27 asm ("$27") = oz_hwaxp_dispatchr27;

    asm volatile ("jsr $26,(%1)" : "=r"(__r0)
                                 : "r"(oz_hwaxp_dispatchent), "r"(__r16), "r"(__r17), "r"(__r27)
                                 : "$1", "$16", "$17", "$18", "$19", "$20", "$21", "$22", "$23", "$24", "$25", "$26", "$27", "$28");
  }

  ddevex -> channel = 0;
  return (1);
}

/************************************************************************/
/*									*/
/*  Perform I/O function						*/
/*									*/
/*  We are at SOFTINT level.  All I/O is performed synchronously.	*/
/*									*/
/************************************************************************/

static uLong null_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode, 
                         OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap)

{
  return (OZ_BADIOFUNC);
}

static uLong disk_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode, 
                         OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap)

{
  Ddevex *ddevex;
  uLong sts;

  ddevex = devexv;

  /* Process individual functions */

  switch (funcode) {

    /* Set volume valid bit one way or the other */

    case OZ_IO_DISK_SETVOLVALID: {
      OZ_IO_disk_setvolvalid disk_setvolvalid;

      movc4 (as, ap, sizeof disk_setvolvalid, &disk_setvolvalid);
      ddevex -> valid = disk_setvolvalid.valid & 1;
      return (OZ_SUCCESS);
    }

    /* Write blocks to the disk from virtual memory */

    case OZ_IO_DISK_WRITEBLOCKS: {
      OZ_IO_disk_writeblocks disk_writeblocks;

      movc4 (as, ap, sizeof disk_writeblocks, &disk_writeblocks);
      sts = oz_knl_ioop_lockr (ioop, disk_writeblocks.size, disk_writeblocks.buff, NULL, NULL, NULL);
      if (sts == OZ_SUCCESS) sts = readwritev (ddevex, 0x14, disk_writeblocks.size, disk_writeblocks.buff, disk_writeblocks.slbn);
      return (sts);
    }

    /* Read blocks from the disk into virtual memory */

    case OZ_IO_DISK_READBLOCKS: {
      OZ_IO_disk_readblocks disk_readblocks;

      movc4 (as, ap, sizeof disk_readblocks, &disk_readblocks);
      sts = oz_knl_ioop_lockw (ioop, disk_readblocks.size, disk_readblocks.buff, NULL, NULL, NULL);
      if (sts == OZ_SUCCESS) sts = readwritev (ddevex, 0x13, disk_readblocks.size, disk_readblocks.buff, disk_readblocks.slbn);
      return (sts);
    }

    /* Get info part 1 */

    case OZ_IO_DISK_GETINFO1: {
      OZ_IO_disk_getinfo1 disk_getinfo1;

      if (!OZ_HW_WRITABLE (as, ap, procmode)) return (OZ_ACCVIO);			// make sure block is writable
      memset (&disk_getinfo1, 0, sizeof disk_getinfo1);					// zero it all out
      disk_getinfo1.blocksize   = BLOCKSIZE;						// return the block size
      disk_getinfo1.totalblocks = ddevex -> totalblocks;				// return number of blocks
      disk_getinfo1.bufalign    = BUFFALIGN;						// everything must be quad aligned
      movc4 (sizeof disk_getinfo1, &disk_getinfo1, as, ap);				// return the info
      return (OZ_SUCCESS);
    }

    /* Write blocks to the disk from physical pages (kernel only) */

    case OZ_IO_DISK_WRITEPAGES: {
      OZ_IO_disk_writepages disk_writepages;

      if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);
      movc4 (as, ap, sizeof disk_writepages, &disk_writepages);
      if (sts == OZ_SUCCESS) sts = readwritep (ddevex, 0x14, disk_writepages.size, disk_writepages.pages, disk_writepages.offset, disk_writepages.slbn);
      return (sts);
    }

    /* Read blocks from the disk into physical pages (kernel only) */

    case OZ_IO_DISK_READPAGES: {
      OZ_IO_disk_readpages disk_readpages;

      if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);
      movc4 (as, ap, sizeof disk_readpages, &disk_readpages);
      if (sts == OZ_SUCCESS) sts = readwritep (ddevex, 0x13, disk_readpages.size, disk_readpages.pages, disk_readpages.offset, disk_readpages.slbn);
      return (sts);
    }

    /* Set crash dump device */

#if 000
    case OZ_IO_DISK_CRASH: {
      if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);		/* caller must be in kernel mode */
      if (crash_devunit != NULL) {					/* get rid of old crash stuff, if any */
        oz_knl_devunit_increfc (crash_devunit, -1);
        crash_devex   = NULL;
        crash_devunit = NULL;
      }
      if (ap != NULL) {
        if (as != sizeof (OZ_IO_disk_crash)) return (OZ_BADBUFFERSIZE);	/* param block must be exact size */
        ((OZ_IO_disk_crash *)ap) -> crashentry = ata_crash;		/* return pointer to crash routine */
        ((OZ_IO_disk_crash *)ap) -> crashparam = NULL;			/* we don't require a parameter */
        ((OZ_IO_disk_crash *)ap) -> blocksize  = DISK_BLOCK_SIZE;	/* tell them our blocksize */
        crash_devex   = devex;						/* save the device we will write to */
        crash_devunit = devunit;
        oz_knl_devunit_increfc (crash_devunit, 1);			/* make sure it doesn't get deleted */
      }
      return (OZ_SUCCESS);
    }
#endif

    /* Who knows what */

    default: {
      return (OZ_BADIOFUNC);
    }
  }
}

/************************************************************************/
/*									*/
/*  Perform Read/Write operation					*/
/*									*/
/*    Input:								*/
/*									*/
/*	ddevex = device to do read/write to				*/
/*	func = 0x13 : perform read					*/
/*	       0x14 : perform write					*/
/*	size = number of bytes to transfer				*/
/*	slbn = starting block number					*/
/*	readwritep:							*/
/*	  pages  = points to physical page number array			*/
/*	  offset = offset in first physical page			*/
/*	readwritev:							*/
/*	  buff = virtual address of buffer				*/
/*									*/
/*    Output:								*/
/*									*/
/*	readwritep/v = OZ_SUCCESS : successful				*/
/*	                     else : error status			*/
/*									*/
/************************************************************************/

static uLong readwritep (Ddevex *ddevex, uQuad func, uLong size, OZ_Mempage const *pages, uLong offset, OZ_Dbn slbn)

{
  OZ_Pagentry savepte;
  uByte *va;
  uLong len, sts;
  uQuad blockbuff[BLOCKSIZE/8];

  oz_hw_phys_mappage (0, &savepte);

  while (size > 0) {
    pages  += offset >> OZ_HW_L2PAGESIZE;						// normalize pages/offset
    offset &= PAGESIZE - 1;								// ... so offset < PAGESIZE
    len = PAGESIZE - offset;								// get length to end of page

    /* If buffer finishes by end of page, do it directly from the page and exit */

    if (len >= size) {									// see if that covers what's left to do
      va  = oz_hw_phys_mappage (*pages, NULL);						// ok, map the page
      va += offset;									// point to buffer within page
      sts = readwritev (ddevex, func, size, va, slbn);					// do read/write
      break;										// ... and we're done (good or bad)
    }

    /* If there is at least a block to end of page, do the blocks directly from the page */

    if (len >= BLOCKSIZE) {								// see if at least a block to end of page
      len = (len >> L2BLOCKSIZE) << L2BLOCKSIZE;					// ok, do as many blocks as we can
      va  = oz_hw_phys_mappage (*pages, NULL);						// map the page
      va += offset;									// point to buffer within page
      sts = readwritev (ddevex, func, len, va, slbn);					// do read/write
      if (sts != OZ_SUCCESS) return (sts);						// stop if error
    }

    /* Otherwise, do a single block split between pages via temp buffer */

    else {
      len = size;									// see how much left to do
      if (len > BLOCKSIZE) len = BLOCKSIZE;						// but not more than a block
      if (func == 0x14) oz_hw_phys_movetovirt (len, blockbuff, pages, offset);		// write: copy from phypages to buffer
      sts = readwritev (ddevex, func, len, blockbuff, slbn);				// do read/write
      if (sts != OZ_SUCCESS) break;							// stop if error
      if (func == 0x13) oz_hw_phys_movefromvirt (len, blockbuff, pages, offset);	// read: copy from buffer to phypages
    }

    /* Compute how much is left to do and where it is */

    size -= len;
    slbn += len >> L2BLOCKSIZE;
    offset += len;
  }

  oz_hw_phys_unmappage (savepte);
  return (sts);
}

static uLong readwritev (Ddevex *ddevex, uQuad func, uLong size, void const *buff, OZ_Dbn slbn)

{
  uQuad rq;

  if (!(ddevex -> valid)) return (OZ_VOLNOTVALID);

  {
    register uQuad __r0  asm ("$0");
    register uQuad __r16 asm ("$16") = func;	// 0x13=READ; 0x14=WRITE
    register uQuad __r17 asm ("$17") = ddevex -> channel;
    register uQuad __r18 asm ("$18") = size;
    register uQuad __r19 asm ("$19") = (uQuad)buff;
    register uQuad __r20 asm ("$20") = slbn;
    register uQuad __r27 asm ("$27") = oz_hwaxp_dispatchr27;

    asm volatile ("jsr $26,(%1)" : "=r"(__r0)
                                 : "r"(oz_hwaxp_dispatchent), "r"(__r16), "r"(__r17), "r"(__r18), "r"(__r19), "r"(__r20), "r"(__r27)
                                 : "$1", "$16", "$17", "$18", "$19", "$20", "$21", "$22", "$23", "$24", "$25", "$26", "$27", "$28");
    rq = __r0;
  }

  if ((rq >> 63) == 0) return (OZ_SUCCESS);
  oz_knl_printk ("oz_dev_srmdev: %s error %QX, func %X, size %X, buff %X, slbn %X\n", ddevex -> suffix, rq, func, size, buff, slbn);
  return (OZ_IOFAILED);
}
