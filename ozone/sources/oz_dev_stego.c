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
/*  Steganographic disk driver						*/
/*									*/
/*	init disk oz_stego "erase" - writes random pattern to disk	*/
/*	init disk.a oz_stego size - inits a partition, prompts for key	*/
/*	init "disk.a key" oz_stego size - doesn't prompt for key	*/
/*	mount disk.a oz_stego - mounts partition, prompts for key	*/
/*		resultant dev is disk.a					*/
/*	mount "disk.a key" oz_stego - doesn't prompt for key		*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_dev_disk.h"
#include "oz_io_console.h"
#include "oz_io_disk.h"
#include "oz_knl_devio.h"
#include "oz_knl_hw.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_logname.h"
#include "oz_knl_phymem.h"
#include "oz_knl_procmode.h"
#include "oz_knl_quota.h"
#include "oz_knl_sdata.h"
#include "oz_knl_section.h"
#include "oz_knl_status.h"
#include "oz_knl_thread.h"
#include "oz_knl_userjob.h"
#include "oz_sys_cipher.h"
#include "oz_sys_hash.h"
#include "oz_sys_xprintf.h"

#define MAXKEYSIZE 64

#define PRINTF oz_knl_printk ("oz_dev_stego: "

#if OZ_SYS_HASH_BINSIZE < OZ_SYS_CIPHER_KEYSIZE
  error : code assumes hash size is at least as big as cipher key size
#endif
#if OZ_SYS_HASH_BINSIZE < OZ_SYS_CIPHER_BLKSIZE
  error : code assumes hash size is at least as big as cipher block size
#endif

/* Macros for formatting header block and rotating the others */

#if OZ_SYS_HASH_BINSIZE < 14
  error : code assumes hash size is at least 14 bytes
#endif

	// get index for the two size longs in the header block - i is 0 or 1

#define DBNKEYIDX(iopex,i) (iopex -> k.keybin[i] % (iopex -> k.disk_getinfo1.blocksize / sizeof (OZ_Dbn) / 2))

	// xor value for mashing the size longs in the header (i is 0 or 1), also use for rotation factor (i is 2)

#define DBNKEYXOR(keybin,i) ((keybin[i*4+2]) | (keybin[i*4+3] << 8) | (keybin[i*4+4] << 16) | (keybin[i*4+5] << 24))

/* Device extension structure */

typedef struct Devex Devex;
typedef struct Iopex Iopex;

struct Devex { Devex *next;				// next in devexs list
               OZ_Iochan *hostiochan;			// I/O channel to host disk (NULL if not 'mounted')
               OZ_Devunit *hostdevunit;			// Host disk's device unit pointer
               int volvalid;				// volume is valid
               OZ_Dbn startlbn;				// starting lbn in the host disk (of the header block)
               OZ_Dbn lbnrotator;			// rotate lbn's by part of the key
               OZ_IO_disk_getinfo1 disk_getinfo1;	// host disk info
               OZ_Dbn partsize;				// partition size (in blocks), including header block
               Long rwinprog;				// reads/writes in progress
               Iopex *dismpend;				// pending dismount request
               void *decryptor;				// decryption context block pointer
               void *encryptor;				// encryption context block pointer
               Iopex *iopexs;				// i/o's in progress that we can abort
               OZ_Smplock smplock_vl;			// state lock
               char partchar;				// partition character ('a'..'z')
             };

#if OZ_SYS_CIPHER_BLKSIZE > OZ_SYS_HASH_BINSIZE
#define FBSIZE OZ_SYS_CIPHER_BLKSIZE
#else
#define FBSIZE OZ_SYS_HASH_BINSIZE
#endif

struct Iopex { Iopex *next, **prev;			// links on devex -> iopexs queue
               int aborted;				// set if/when aborted
               OZ_Ioop *ioop;				// iopex's ioop
               Devex *devex;				// stego's devex
               OZ_Procmode procmode;			// procmode of request
               uByte feedback[FBSIZE];			// feedback for cipher-block-chaining
               union { struct { OZ_IO_fs_initvol p;				// init param block
                                OZ_Dbn partsize;				// - size of partition (in blocks), incl hdr block
                              } initvol;
                       struct { OZ_IO_fs_mountvol p;				// mount param block
                                OZ_Iochan *stegiochan;				// - i/o chan this request was issued on
                              } mountvol;
                       struct { OZ_IO_fs_dismount p;				// dismount param block
                              } dismount;
                       struct { uLong size;					// - size of transfer
                                const OZ_Mempage *pages;			// - physical page number array
                                uLong offset;					// - offset in page
                                OZ_Dbn slbn;					// - starting logical block number
                                volatile Long status;				// - read status
                                volatile Long segments;				// - number of read segments (1 or 2)
                              } readblocks;
                       struct { volatile Long status;				// - write status
                                volatile Long segments;				// - number of write segments (1 or 2)
                                uByte *buff;					// - temp buffer
                              } writeblocks;
                     } u;
               struct { uLong keylen;						// - length of key string
                        void (*gotkey) (Iopex *iopex, uLong status);		// - what to call when we got the key
                        OZ_Iochan *hostiochan;					// - host disk i/o chan
                        OZ_Iochan *coniochan;					// - requestor's console (for key prompt)
                        OZ_IO_disk_getinfo1 disk_getinfo1;			// - get host disk geometry
                        OZ_Dbn startlbn;					// - starting lbn of the partition
                        void *decryptor;					// - decryptor context block
                        void *encryptor;					// - encryptor context block
                        uByte *stegohdr;					// - pointer to header block buffer
                        uByte keybin[OZ_SYS_HASH_BINSIZE];			// - hashed key
                        char hostdevname[OZ_DEVUNIT_NAMESIZE+MAXKEYSIZE];	// - 'hostdevname.partchar keystring'
                        char keystr[MAXKEYSIZE];				// - key string read from console
                        char keypmt[OZ_DEVUNIT_NAMESIZE+16];			// - key prompt string
                        char partchar;						// - partition char
                      } k;							// stuff for getkeystuff routine
             };

/* Function table */

static uLong stego_clonecre (OZ_Devunit *template_devunit, void *template_devex, int template_cloned, OZ_Procmode procmode, OZ_Devunit **cloned_devunit);
static int stego_clonedel (OZ_Devunit *cloned_devunit, void *devexv, int cloned);
static void stego_abort (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Ioop *ioop, void *iopexv, OZ_Procmode procmode);
static uLong stego_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode, 
                          OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap);

static const OZ_Devfunc stego_functable = { sizeof (Devex), 0, sizeof (Iopex), 0, NULL, stego_clonecre, stego_clonedel, 
                                            NULL, NULL, stego_abort, stego_start, NULL };

/* Internal static data */

static int initialized = 0;
static OZ_Devclass *devclass;
static OZ_Devdriver *devdriver;
static Devex *devexs = NULL;
static OZ_Smplock smplock_dv;

/* Internal routines */

static uLong stego_initvol (Iopex *iopex);
static void init_erase_spunup (void *iopexv, uLong status);
static void init_erase_gotinfo1 (void *iopexv, uLong status);
static void init_erase_written (void *iopexv, uLong status);
static uLong init_erase_startwrite (Iopex *iopex);
static void init_erase_done (Iopex *iopex, uLong status);
static void stego_init_gotkey (Iopex *iopex, uLong status);
static void stego_init_wrotehdrblock (void *iopexv, uLong status);
static void stego_init_done (Iopex *iopex, uLong status);
static uLong stego_mountvol (Iopex *iopex);
static void stego_mount_gotkey (Iopex *iopex, uLong status);
static void stego_mount_done (Iopex *iopex, uLong status);
static uLong getkeystuff (Iopex *iopex, const char *hostdevname, OZ_Lockmode hostiochanlockmode);
static void getkey_gotkey (void *iopexv, uLong status);
static void getkey_spunup (void *iopexv, uLong status);
static void getkey_gotinfo (void *iopexv, uLong status);
static void getkey_readhdrblock (void *iopexv, uLong status);
static uLong stego_dismount (Iopex *iopex);
static uLong stego_readblocks (Iopex *iopex, OZ_Dbn slbn, uLong size, uLong pageoffs, const OZ_Mempage *phypages);
static void readcomplete (void *iopexv, uLong status);
static uLong stego_writeblocks (Iopex *iopex, OZ_Dbn slbn, uLong size, uLong pageoffs, const OZ_Mempage *phypages);
static void writecomplete (void *iopexv, uLong status);
static uLong startrw (Devex *devex, OZ_Dbn *slbn_r, uLong size, uLong pageoffs);
static void decrwinprog (Devex *devex);
static void removefromdevexs (Devex *devex);
static void abortable (Iopex *iopex);
static void iodone (Iopex *iopex, uLong status);

/************************************************************************/
/*									*/
/*  Boot-time initialization routine					*/
/*									*/
/************************************************************************/

void oz_dev_stego_init ()

{
  OZ_Devunit *devunit;

  if (initialized) return;

  oz_knl_printk ("oz_dev_stego_init\n");
  initialized = 1;
  oz_hw_smplock_init (sizeof smplock_dv, &smplock_dv, OZ_SMPLOCK_LEVEL_DV);

  /* Create template device */

  devclass  = oz_knl_devclass_create (OZ_IO_DISK_CLASSNAME, OZ_IO_DISK_BASE, OZ_IO_DISK_MASK, "oz_stego");
  devdriver = oz_knl_devdriver_create (devclass, "oz_stego");
  devunit   = oz_knl_devunit_create (devdriver, "oz_stego", "init & mount template", &stego_functable, 0, oz_s_secattr_tempdev);
}

/************************************************************************/
/*									*/
/*  A channel was assigned to device for first time, clone if channel 	*/
/*  was assigned to the template device					*/
/*									*/
/*  Runs with dv smplock set						*/
/*									*/
/************************************************************************/

static uLong stego_clonecre (OZ_Devunit *template_devunit, void *template_devex, int template_cloned, OZ_Procmode procmode, OZ_Devunit **cloned_devunit)

{
  Devex *devex;
  char unitname[OZ_DEVUNIT_NAMESIZE];

  static uLong unitno = 0;

  if (template_cloned) {
    *cloned_devunit = template_devunit;
    oz_knl_devunit_increfc (template_devunit, 1);
  } else {
    oz_sys_sprintf (sizeof unitname, unitname, "oz_stego_%u", ++ unitno);
    *cloned_devunit = oz_knl_devunit_create (devdriver, unitname, unitname, &stego_functable, 1, oz_knl_thread_getdefcresecattr (NULL));
    devex = oz_knl_devunit_ex (*cloned_devunit);
    memset (devex, 0, sizeof *devex);
    oz_hw_smplock_init (sizeof devex -> smplock_vl, &(devex -> smplock_vl), OZ_SMPLOCK_LEVEL_VL);
  }
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Last channel was deassigned from unit.  If not template and it is 	*/
/*  not set up, delete it.						*/
/*									*/
/*  Runs with dv smplock set						*/
/*									*/
/************************************************************************/

static int stego_clonedel (OZ_Devunit *cloned_devunit, void *devexv, int cloned)

{
  Devex *devex;

  devex = devexv;

  if (cloned) {

    /* Don't delete if it is mounted.  This routine will be called again */
    /* when the dismount utility deassigns it channel to the device.     */

    if (devex -> hostiochan != NULL) cloned = 0;
  }
  return (cloned);
}

/************************************************************************/
/*									*/
/*  Abort I/O's in progress on a channel				*/
/*									*/
/************************************************************************/

static void stego_abort (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Ioop *ioop, void *iopexv, OZ_Procmode procmode)

{
  Devex *devex;
  Iopex *iopex;
  uLong vl;

  devex = devexv;

  vl = oz_hw_smplock_wait (&(devex -> smplock_vl));					// lock database
  for (iopex = devex -> iopexs; iopex != NULL; iopex = iopex -> next) {			// scan list of requests in progress
    iopex -> aborted |= oz_knl_ioabortok (iopex -> ioop, iochan, procmode, ioop);	// maybe flag it for abort
  }
  oz_hw_smplock_clr (&(devex -> smplock_vl), vl);					// unlock database
}

/************************************************************************/
/*									*/
/*  Start performing a disk i/o function				*/
/*									*/
/************************************************************************/

static uLong stego_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode, 
                          OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap)

{
  Devex *devex;
  Iopex *iopex;
  uLong sts;

  devex = devexv;
  iopex = iopexv;

  iopex -> ioop     = ioop;
  iopex -> devex    = devex;
  iopex -> procmode = procmode;
  iopex -> aborted  = 0;
  iopex -> prev     = NULL;

  /* Process individual functions */

  switch (funcode) {

    /* The INITVOL function is used to erase the disk or write partition header */

    case OZ_IO_FS_INITVOL: {
      oz_knl_process_getcur ();
      movc4 (as, ap, sizeof iopex -> u.initvol.p, &(iopex -> u.initvol.p));
      sts = stego_initvol (iopex);
      return (sts);
    }

    /* The MOUNTVOL function is used to set the stego size and allocate the memory */

    case OZ_IO_FS_MOUNTVOL: {
      movc4 (as, ap, sizeof iopex -> u.mountvol.p, &(iopex -> u.mountvol.p));
      iopex -> u.mountvol.stegiochan = iochan;
      sts = stego_mountvol (iopex);
      return (sts);
    }

    /* Dismount undoes all that mount does */

    case OZ_IO_FS_DISMOUNT: {
      movc4 (as, ap, sizeof iopex -> u.dismount.p, &(iopex -> u.dismount.p));
      sts = stego_dismount (iopex);
      return (sts);
    }

    /* Set volume valid bit one way or the other */

    case OZ_IO_DISK_SETVOLVALID: {
      OZ_IO_disk_setvolvalid disk_setvolvalid;
      uLong vl;

      movc4 (as, ap, sizeof disk_setvolvalid, &disk_setvolvalid);
      sts = OZ_DEVOFFLINE;
      vl  = oz_hw_smplock_wait (&(devex -> smplock_vl));
      if (!disk_setvolvalid.valid || ((devex -> hostiochan != NULL) && (devex -> dismpend == NULL))) {
        devex -> volvalid = disk_setvolvalid.valid;
        sts = OZ_SUCCESS;
      }
      oz_hw_smplock_clr (&(devex -> smplock_vl), vl);
      return (sts);
    }

    /* Write blocks to the disk */

    case OZ_IO_DISK_WRITEBLOCKS: {
      const OZ_Mempage *phypages;
      OZ_IO_disk_writeblocks disk_writeblocks;
      uLong pageoffs;

      /* Lock I/O buffer in memory */

      movc4 (as, ap, sizeof disk_writeblocks, &disk_writeblocks);
      sts = oz_knl_ioop_lockr (ioop, disk_writeblocks.size, disk_writeblocks.buff, &phypages, NULL, &pageoffs);

      /* If that was successful, queue the request to the drive for processing */

      if (sts == OZ_SUCCESS) sts = stego_writeblocks (iopex, disk_writeblocks.slbn, disk_writeblocks.size, pageoffs, phypages);
      return (sts);
    }

    case OZ_IO_DISK_WRITEPAGES: {
      OZ_IO_disk_writepages disk_writepages;

      if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);
      movc4 (as, ap, sizeof disk_writepages, &disk_writepages);
      sts = stego_writeblocks (iopex, disk_writepages.slbn, disk_writepages.size, disk_writepages.offset, disk_writepages.pages);
      return (sts);
    }

    /* Read blocks from the disk */

    case OZ_IO_DISK_READBLOCKS: {
      const OZ_Mempage *phypages;
      OZ_IO_disk_readblocks disk_readblocks;
      uLong pageoffs;

      /* Lock I/O buffer in memory */

      movc4 (as, ap, sizeof disk_readblocks, &disk_readblocks);
      sts = oz_knl_ioop_lockr (ioop, disk_readblocks.size, disk_readblocks.buff, &phypages, NULL, &pageoffs);

      /* If that was successful, queue the request to the drive for processing */

      if (sts == OZ_SUCCESS) sts = stego_readblocks (iopex, disk_readblocks.slbn, disk_readblocks.size, pageoffs, phypages);
      return (sts);
    }

    case OZ_IO_DISK_READPAGES: {
      OZ_IO_disk_readpages disk_readpages;

      if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);
      movc4 (as, ap, sizeof disk_readpages, &disk_readpages);
      sts = stego_readblocks (iopex, disk_readpages.slbn, disk_readpages.size, disk_readpages.offset, disk_readpages.pages);
      return (sts);
    }

    /* Get info part 1 */

    case OZ_IO_DISK_GETINFO1: {
      OZ_IO_disk_getinfo1 disk_getinfo1;
      uLong vl;

      if (!OZ_HW_WRITABLE (as, ap, procmode)) return (OZ_ACCVIO);
      memset (&disk_getinfo1, 0, sizeof disk_getinfo1);					// clear stuff we don't know/care about
      sts = OZ_VOLNOTVALID;
      vl  = oz_hw_smplock_wait (&(devex -> smplock_vl));
      if (devex -> volvalid) {
        disk_getinfo1.blocksize   = devex -> disk_getinfo1.blocksize;			// same disk block size as host
        disk_getinfo1.totalblocks = devex -> partsize - 1;				// leave out one block for the header
        disk_getinfo1.parthoststartblock = devex -> startlbn + 1;			// leave out one block for the header
        strncpyz (disk_getinfo1.parthostdevname, oz_knl_devunit_devname (devex -> hostdevunit), sizeof disk_getinfo1.parthostdevname);
        disk_getinfo1.bufalign    = devex -> disk_getinfo1.bufalign | (OZ_SYS_CIPHER_BLKSIZE - 1); // make sure always quad align
											// because encrypt/decrypt require it
        sts = OZ_SUCCESS;
      }
      oz_hw_smplock_clr (&(devex -> smplock_vl), vl);
      if (sts == OZ_SUCCESS) movc4 (sizeof disk_getinfo1, &disk_getinfo1, as, ap);	// return param block to caller
      return (sts);
    }

    /* Who knows what */

    default: {
      return (OZ_BADIOFUNC);
    }
  }
}

/************************************************************************/
/*									*/
/*  Initialize drive							*/
/*									*/
/*    Input:								*/
/*									*/
/*	volname = "erase" : erases the whole disk by filling it with 	*/
/*	                    random data					*/
/*	           "size" : creates partition <part> of size <size> 	*/
/*	                    blocks encrypted by key <key>		*/
/*	                    any earlier partitions must be mounted	*/
/*									*/
/************************************************************************/

static uLong stego_initvol (Iopex *iopex)

{
  char hostdevname[OZ_DEVUNIT_NAMESIZE], volname[16];
  int usedup;
  OZ_IO_disk_setvolvalid disk_setvolvalid;
  uLong sts;

  iopex -> k.stegohdr   = NULL;
  iopex -> k.hostiochan = NULL;

  sts = oz_knl_section_ugetz (iopex -> procmode, sizeof volname, iopex -> u.initvol.p.volname, volname, NULL);
  if (sts != OZ_SUCCESS) return (sts);

  /* Process 'erase' function - fills the whole disk with random garbage */
  /* Devname = name of disk to erase                                     */

  oz_knl_process_getcur ();
  if (strcasecmp (volname, "erase") == 0) {
    sts = oz_knl_section_ugetz (iopex -> procmode, sizeof hostdevname, iopex -> u.initvol.p.devname, hostdevname, NULL);
    if (sts != OZ_SUCCESS) return (sts);
    sts = oz_knl_iochan_crbynm (hostdevname, OZ_LOCKMODE_EX, OZ_PROCMODE_KNL, NULL, &(iopex -> k.hostiochan));
    if (sts != OZ_SUCCESS) return (sts);
    abortable (iopex);
    memset (&disk_setvolvalid, 0, sizeof disk_setvolvalid);
    disk_setvolvalid.valid = 1;
    sts = oz_knl_iostart3 (1, NULL, iopex -> k.hostiochan, OZ_PROCMODE_KNL, init_erase_spunup, iopex, NULL, NULL, NULL, NULL, 
                           OZ_IO_DISK_SETVOLVALID, sizeof disk_setvolvalid, &disk_setvolvalid);
    if (sts != OZ_STARTED) {
      init_erase_spunup (iopex, sts);
      sts = OZ_STARTED;
    }
  }

  /* Otherwise, volname="size", devname="hostdisk.partchar[ keystring]" */

  else {
    iopex -> u.initvol.partsize = oz_hw_atoi (volname, &usedup);
    if ((volname[usedup] != 0) || (iopex -> u.initvol.partsize == 0)) {
      PRINTF "bad size %s\n", volname);
      return (OZ_BADPARAM);
    }
    iopex -> k.gotkey = stego_init_gotkey;
    sts = getkeystuff (iopex, iopex -> u.initvol.p.devname, OZ_LOCKMODE_CW);
  }

  return (sts);
}

static void init_erase_spunup (void *iopexv, uLong status)

{
  Iopex *iopex;
  uLong sts;

  iopex = iopexv;

  if (status != OZ_SUCCESS) {
    PRINTF "error %u spinning disk up\n", status);
    init_erase_done (iopex, status);
    return;
  }

  memset (&(iopex -> k.disk_getinfo1), 0, sizeof iopex -> k.disk_getinfo1);
  sts = oz_knl_iostart3 (1, NULL, iopex -> k.hostiochan, OZ_PROCMODE_KNL, init_erase_gotinfo1, iopex, NULL, NULL, NULL, NULL, 
                         OZ_IO_DISK_GETINFO1, sizeof iopex -> k.disk_getinfo1, &(iopex -> k.disk_getinfo1));
  if (sts != OZ_STARTED) init_erase_gotinfo1 (iopex, sts);
}

static void init_erase_gotinfo1 (void *iopexv, uLong status)

{
  Iopex *iopex;
  OZ_IO_disk_writeblocks disk_writeblocks;
  uLong sts;

  iopex = iopexv;
  if (status != OZ_SUCCESS) {
    PRINTF "error %u getting disk geometry\n", status);
    init_erase_done (iopex, status);
    return;
  }

  iopex -> k.startlbn = 0;
  iopex -> k.stegohdr = OZ_KNL_PGPMALLOQ (1 << OZ_HW_L2PAGESIZE);
  if (iopex -> k.stegohdr == NULL) {
    init_erase_done (iopex, OZ_EXQUOTAPGP);
    return;
  }

  do {
    sts = init_erase_startwrite (iopex);
    if (sts != OZ_SUCCESS) break;
    iopex -> k.startlbn += (1 << OZ_HW_L2PAGESIZE) / iopex -> k.disk_getinfo1.blocksize;
  } while (iopex -> k.startlbn < iopex -> k.disk_getinfo1.totalblocks);
  if (sts != OZ_STARTED) init_erase_written (iopex, sts);
}

static void init_erase_written (void *iopexv, uLong status)

{
  Iopex *iopex;
  uLong sts;

  iopex = iopexv;
  sts   = status;

  while (sts == OZ_SUCCESS) {
    iopex -> k.startlbn += (1 << OZ_HW_L2PAGESIZE) / iopex -> k.disk_getinfo1.blocksize;
    if (iopex -> k.startlbn >= iopex -> k.disk_getinfo1.totalblocks) break;
    sts = init_erase_startwrite (iopex);
  }
  if (sts != OZ_STARTED) {
    if (sts != OZ_SUCCESS) PRINTF "error %u erasing disk block %u\n", sts, iopex -> k.startlbn);
    init_erase_done (iopex, sts);
  }
}

static uLong init_erase_startwrite (Iopex *iopex)

{
  OZ_IO_disk_writeblocks disk_writeblocks;
  OZ_Dbn nextlbn;
  uLong sts;

  if (iopex -> aborted) return (OZ_ABORTED);

  memset (&disk_writeblocks, 0, sizeof disk_writeblocks);
  disk_writeblocks.size = 1 << OZ_HW_L2PAGESIZE;
  disk_writeblocks.buff = iopex -> k.stegohdr;
  disk_writeblocks.slbn = iopex -> k.startlbn;

  nextlbn = iopex -> k.startlbn + (1 << OZ_HW_L2PAGESIZE) / iopex -> k.disk_getinfo1.blocksize;
  if (nextlbn > iopex -> k.disk_getinfo1.totalblocks) {
    disk_writeblocks.size = (iopex -> k.disk_getinfo1.totalblocks - iopex -> k.startlbn) * iopex -> k.disk_getinfo1.blocksize;
  }

  oz_hw_random_fill (disk_writeblocks.size, iopex -> k.stegohdr);

  sts = oz_knl_iostart3 (1, NULL, iopex -> k.hostiochan, OZ_PROCMODE_KNL, init_erase_written, iopex, NULL, NULL, NULL, NULL, 
                         OZ_IO_DISK_WRITEBLOCKS, sizeof disk_writeblocks, &disk_writeblocks);
  return (sts);
}

static void init_erase_done (Iopex *iopex, uLong status)

{
  if (iopex -> k.stegohdr   != NULL) OZ_KNL_PGPFREE (iopex -> k.stegohdr);
  if (iopex -> k.hostiochan != NULL) oz_knl_iochan_increfc (iopex -> k.hostiochan, -1);
  iodone (iopex, status);
}

static void stego_init_gotkey (Iopex *iopex, uLong status)

{
  OZ_IO_disk_writeblocks disk_writeblocks;
  uLong sts;

  if (status != OZ_SUCCESS) {
    stego_init_done (iopex, status);
    return;
  }

  /* Store size in two different places in header block.  But you  */
  /* must know the key to find them and the second copy is mashed. */

  sts  = DBNKEYIDX (iopex, 0);
  ((OZ_Dbn *)(iopex -> k.stegohdr))[sts] = iopex -> u.initvol.partsize ^ DBNKEYXOR (iopex -> k.keybin, 0);
  sts += DBNKEYIDX (iopex, 1) + 1;
  ((OZ_Dbn *)(iopex -> k.stegohdr))[sts] = iopex -> u.initvol.partsize ^ DBNKEYXOR (iopex -> k.keybin, 1);

  /* Encrypt the block and write to disk */

  oz_sys_hash (sizeof iopex -> k.startlbn, 			// size of 'text' to hash
               &(iopex -> k.startlbn), 				// text to hash = block number
               iopex -> feedback);				// where to put the hash

  oz_sys_cipher_encrypt (iopex -> k.encryptor, 			// encryption context pointer
                         iopex -> feedback, 			// cypherblock chaining feedback buffer
                         iopex -> k.disk_getinfo1.blocksize, 	// number of bytes to encrypt
                         iopex -> k.stegohdr, 			// where to get the plaintext
                         iopex -> k.stegohdr);			// where to put the cyphertext

  memset (&disk_writeblocks, 0, sizeof disk_writeblocks);
  disk_writeblocks.size = iopex -> k.disk_getinfo1.blocksize;
  disk_writeblocks.buff = iopex -> k.stegohdr;
  disk_writeblocks.slbn = iopex -> k.startlbn;
  sts = oz_knl_iostart3 (1, NULL, iopex -> k.hostiochan, OZ_PROCMODE_KNL, stego_init_wrotehdrblock, iopex, NULL, NULL, NULL, NULL, 
                         OZ_IO_DISK_WRITEBLOCKS, sizeof disk_writeblocks, &disk_writeblocks);
  if (sts != OZ_STARTED) stego_init_wrotehdrblock (iopex, sts);
}

static void stego_init_wrotehdrblock (void *iopexv, uLong status)

{
  Iopex *iopex;
  uLong sts;

  iopex = iopexv;
  if (status != OZ_SUCCESS) PRINTF "error %u writing disk block %u\n", status, iopex -> k.startlbn);
  stego_init_done (iopex, status);
}

static void stego_init_done (Iopex *iopex, uLong status)

{
  if (iopex -> k.coniochan  != NULL) oz_knl_iochan_increfc (iopex -> k.coniochan,  -1);
  if (iopex -> k.hostiochan != NULL) oz_knl_iochan_increfc (iopex -> k.hostiochan, -1);
  if (iopex -> k.decryptor  != NULL) OZ_KNL_NPPFREE (iopex -> k.decryptor);
  if (iopex -> k.encryptor  != NULL) OZ_KNL_NPPFREE (iopex -> k.encryptor);
  if (iopex -> k.stegohdr   != NULL) OZ_KNL_PGPFREE (iopex -> k.stegohdr);

  iodone (iopex, status);
}

/************************************************************************/
/*									*/
/*  Mount drive								*/
/*									*/
/*    Input:								*/
/*									*/
/*	devname = <disk>.<part>						*/
/*	          disk = host disk device name				*/
/*	          part = partition letter (a..z)			*/
/*									*/
/************************************************************************/

static uLong stego_mountvol (Iopex *iopex)

{
  uLong sts;

  iopex -> k.gotkey = stego_mount_gotkey;
  sts = getkeystuff (iopex, iopex -> u.mountvol.p.devname, 
                     (iopex -> u.mountvol.p.mountflags & OZ_FS_MOUNTFLAG_READONLY) ? OZ_LOCKMODE_CR : OZ_LOCKMODE_CW);
  return (sts);
}

/* Disk is spun up and header block is read in */

static void stego_mount_gotkey (Iopex *iopex, uLong status)

{
  char unitname[OZ_DEVUNIT_NAMESIZE];
  Devex *prevdevex, *stegodevex;
  OZ_Dbn endlbn, partsize;
  OZ_Devunit *hostdevunit, *prevdevunit, *stegodevunit;
  uLong dv, sts, vl;

  if (status != OZ_SUCCESS) {
    stego_mount_done (iopex, status);
    return;
  }

  /* Get the partition size (in blocks) from the header block */

  sts = DBNKEYIDX (iopex, 0);
  partsize = ((OZ_Dbn *)(iopex -> k.stegohdr))[sts] ^ DBNKEYXOR (iopex -> k.keybin, 0);

  /* Compare it with the mashed copy */

  sts += DBNKEYIDX (iopex, 1) + 1;
  endlbn = ((OZ_Dbn *)(iopex -> k.stegohdr))[sts] ^ DBNKEYXOR (iopex -> k.keybin, 1);
  if (endlbn != partsize) {
    stego_mount_done (iopex, OZ_BADPARAM);
    return;
  }

  /* The partition can't run off the end of the host disk */

  endlbn = partsize + iopex -> k.startlbn;
  if ((endlbn <= iopex -> k.startlbn) || (endlbn > iopex -> k.disk_getinfo1.totalblocks)) {
    stego_mount_done (iopex, OZ_BADPARAM);
    return;
  }

  /* We are ready to enable the drive */

  hostdevunit  = oz_knl_iochan_getdevunit (iopex -> k.hostiochan);
  stegodevunit = oz_knl_iochan_getdevunit (iopex -> u.mountvol.stegiochan);
  stegodevex   = oz_knl_devunit_ex (stegodevunit);

  oz_sys_sprintf (sizeof unitname, unitname, "%s.%c", oz_knl_devunit_devname (hostdevunit), iopex -> k.partchar);
  if (!oz_knl_devunit_rename (stegodevunit, unitname, NULL)) {
    stego_mount_done (iopex, OZ_ALREADYMOUNTED);
    return;
  }

  dv = oz_hw_smplock_wait (&smplock_dv);
  if (stegodevex -> hostiochan != NULL) {
    oz_hw_smplock_clr (&smplock_dv, dv);
    stego_mount_done (iopex, OZ_ALREADYMOUNTED);
    return;
  }

  stegodevex -> partchar      = iopex -> k.partchar;
  stegodevex -> hostdevunit   = hostdevunit;
  stegodevex -> startlbn      = iopex -> k.startlbn;
  stegodevex -> partsize      = partsize;
  stegodevex -> disk_getinfo1 = iopex -> k.disk_getinfo1;
  stegodevex -> decryptor     = iopex -> k.decryptor;
  stegodevex -> encryptor     = iopex -> k.encryptor;
  stegodevex -> lbnrotator    = DBNKEYXOR (iopex -> k.keybin, 2);

  stegodevex -> next = devexs;
  devexs = stegodevex;
  OZ_HW_MB;
  stegodevex -> hostiochan    = iopex -> k.hostiochan;
  oz_hw_smplock_clr (&smplock_dv, dv);

  /* Set up automount routine so the filesystem will automount on the decrypted drive */

  oz_knl_devunit_autogen (stegodevunit, oz_dev_disk_auto, NULL);

  /* Successful, but don't free off a bunch of stuff */

  iopex -> k.hostiochan = NULL;
  iopex -> k.decryptor  = NULL;
  iopex -> k.encryptor  = NULL;
  stego_mount_done (iopex, OZ_SUCCESS);
}

/* Clean up and post request's completion */

static void stego_mount_done (Iopex *iopex, uLong status)

{
  if (iopex -> k.coniochan  != NULL) oz_knl_iochan_increfc (iopex -> k.coniochan,  -1);
  if (iopex -> k.hostiochan != NULL) oz_knl_iochan_increfc (iopex -> k.hostiochan, -1);
  if (iopex -> k.decryptor  != NULL) OZ_KNL_NPPFREE (iopex -> k.decryptor);
  if (iopex -> k.encryptor  != NULL) OZ_KNL_NPPFREE (iopex -> k.encryptor);
  if (iopex -> k.stegohdr   != NULL) OZ_KNL_PGPFREE (iopex -> k.stegohdr);

  iodone (iopex, status);
}

/************************************************************************/
/*									*/
/*  Get key and related stuff						*/
/*									*/
/*    Input:								*/
/*									*/
/*	iopex = initvol or mountvol i/o request				*/
/*	iopex -> k.gotkey = points to completion routine		*/
/*	hostdevname = "<hostdisk>.<partchar>[ <keystring>]"		*/
/*	hostiochanlockmode = lock mode for the i/o channel		*/
/*									*/
/*    Output:								*/
/*									*/
/*	getkeystuff = queuing status					*/
/*	iopex -> k.decryptor = decryptor context			*/
/*	           encryptor = encryptor context			*/
/*	       disk_getinfo1 = host disk info				*/
/*	          hostiochan = host disk i/o channel			*/
/*	            startlbn = starting block number			*/
/*	            stegohdr = header block (decrypted)			*/
/*	              keybin = hashed key				*/
/*	disk spun up							*/
/*									*/
/************************************************************************/

static uLong getkeystuff (Iopex *iopex, const char *hostdevname, OZ_Lockmode hostiochanlockmode)

{
  char *keypnt;
  int l;
  OZ_IO_console_read console_read;
  OZ_Job *job;
  OZ_Logname *logname, *logtable;
  OZ_Process *process;
  OZ_Thread *thread;
  uLong sts;

  iopex -> k.coniochan = NULL;
  iopex -> k.stegohdr  = NULL;
  iopex -> k.decryptor = NULL;
  iopex -> k.encryptor = NULL;

  sts = oz_knl_section_ugetz (iopex -> procmode, sizeof iopex -> k.hostdevname, hostdevname, iopex -> k.hostdevname, NULL);
  if (sts != OZ_SUCCESS) return (sts);

  /* See if optional key is included in hostdevname */

  keypnt = strchr (iopex -> k.hostdevname, ' ');
  if (keypnt != NULL) *(keypnt ++) = 0;

  /* Hostdevname should now end in .<part>, so remove the .<part> and save in partchar */

  l = strlen (iopex -> k.hostdevname);
  if (l <= 2) return (OZ_BADDEVNAME);
  iopex -> k.partchar = iopex -> k.hostdevname[--l];
  if ((iopex -> k.partchar < 'a') || (iopex -> k.partchar > 'z') || (iopex -> k.hostdevname[--l] != '.')) return (OZ_BADDEVNAME);
  iopex -> k.hostdevname[l] = 0;

  /* Assign I/O channel to host disk */

  sts = oz_knl_iochan_crbynm (iopex -> k.hostdevname, hostiochanlockmode, OZ_PROCMODE_KNL, NULL, &(iopex -> k.hostiochan));
  if (sts != OZ_SUCCESS) return (sts);

  /* Set up cipher key context blocks */

  iopex -> k.decryptor = OZ_KNL_NPPMALLOQ (oz_sys_cipher_ctxsize);
  iopex -> k.encryptor = OZ_KNL_NPPMALLOQ (oz_sys_cipher_ctxsize);
  sts = OZ_EXQUOTANPP;
  if ((iopex -> k.decryptor == NULL) || (iopex -> k.encryptor == NULL)) goto errrtn;

  if ((keypnt != NULL) && (keypnt[0] != 0)) {
    movc4 (strlen (keypnt), keypnt, sizeof iopex -> k.keystr, iopex -> k.keystr);
    iopex -> k.keylen = strnlen (iopex -> k.keystr, sizeof iopex -> k.keystr);
    abortable (iopex);
    getkey_gotkey (iopex, OZ_SUCCESS);
    return (OZ_STARTED);
  }

  /* Need to prompt for the key */

  thread = oz_knl_ioop_getthread (iopex -> ioop);
  if (thread == NULL) {
    PRINTF "request has no thread\n");
    goto noconsole;
  }
  process = oz_knl_thread_getprocess (thread);
  if (process == NULL) {
    PRINTF "request has no process\n");
    goto noconsole;
  }
  job = oz_knl_process_getjob (process);
  if (job == NULL) {
    PRINTF "request has ho job\n");
    goto noconsole;
  }
  logtable = oz_knl_job_getlognamtbl (job);
  if (logtable == NULL) {
    PRINTF "request has no logical name table\n");
    goto noconsole;
  }

  sts = oz_knl_logname_lookup (logtable, OZ_PROCMODE_KNL, 10, "OZ_CONSOLE", NULL, NULL, NULL, NULL, &logname, NULL);
  if (sts != OZ_SUCCESS) {
    PRINTF "error %u looking up OZ_JOB_TABLE%OZ_CONSOLE\n", sts);
    goto errrtn;
  }
  sts = oz_knl_logname_getobj (logname, 0, OZ_OBJTYPE_IOCHAN, &(iopex -> k.coniochan));
  oz_knl_logname_increfc (logname, -1);
  if (sts != OZ_SUCCESS) {
    PRINTF "error %u retrieving channel from OZ_CONSOLE\n", sts);
    goto errrtn;
  }

  abortable (iopex);

  oz_sys_sprintf (sizeof iopex -> k.keypmt, iopex -> k.keypmt, "oz_dev_stego %s.%c> ", 
                  oz_knl_devunit_devname (oz_knl_iochan_getdevunit (iopex -> k.hostiochan)), iopex -> k.partchar);

  memset (&console_read, 0, sizeof console_read);
  console_read.size    = sizeof iopex -> k.keystr;
  console_read.buff    = iopex -> k.keystr;
  console_read.rlen    = &(iopex -> k.keylen);
  console_read.pmtsize = strlen (iopex -> k.keypmt);
  console_read.pmtbuff = iopex -> k.keypmt;
  console_read.noecho  = 1;

  sts = oz_knl_iostart3 (1, NULL, iopex -> k.coniochan, OZ_PROCMODE_KNL, getkey_gotkey, iopex, 
                         NULL, NULL, NULL, NULL, OZ_IO_CONSOLE_READ, sizeof console_read, &console_read);
  if (sts != OZ_STARTED) getkey_gotkey (iopex, sts);
  return (OZ_STARTED);

  /* Missing key and there is no console for this request */

noconsole:
  sts = OZ_MISSINGPARAM;
errrtn:
  oz_knl_iochan_increfc (iopex -> k.hostiochan, -1);
  if (iopex -> k.decryptor != NULL) OZ_KNL_NPPFREE (iopex -> k.decryptor);
  if (iopex -> k.encryptor != NULL) OZ_KNL_NPPFREE (iopex -> k.encryptor);
  return (sts);
}

static void getkey_gotkey (void *iopexv, uLong status)

{
  Iopex *iopex;
  OZ_IO_disk_setvolvalid disk_setvolvalid;
  uLong sts;

  iopex = iopexv;

  /* Make sure we really got it */

  if ((status == OZ_SUCCESS) && (iopex -> k.keylen == 0)) status = OZ_MISSINGPARAM;
  if (status != OZ_SUCCESS) {
    PRINTF "erorr %u reading key from console\n", status);
    (*(iopex -> k.gotkey)) (iopex, status);
    return;
  }

  /* Hash the key and set up ciphering context blocks */

  oz_sys_hash (iopex -> k.keylen, iopex -> k.keystr, iopex -> k.keybin);
  memset (iopex -> k.keystr, 0, iopex -> k.keylen);
  oz_sys_cipher_decinit (iopex -> k.keybin, iopex -> k.decryptor);
  oz_sys_cipher_encinit (iopex -> k.keybin, iopex -> k.encryptor);

  /* If this is partition 'a', start spinning the disk up */

  sts = OZ_SUCCESS;
  if (iopex -> k.partchar == 'a') {
    memset (&disk_setvolvalid, 0, sizeof disk_setvolvalid);
    disk_setvolvalid.valid = 1;
    sts = oz_knl_iostart3 (1, NULL, iopex -> k.hostiochan, OZ_PROCMODE_KNL, getkey_spunup, iopex, NULL, NULL, 
                           NULL, NULL, OZ_IO_DISK_SETVOLVALID, sizeof disk_setvolvalid, &disk_setvolvalid);
  }
  if (sts != OZ_STARTED) getkey_spunup (iopex, sts);
}

static void getkey_spunup (void *iopexv, uLong status)

{
  Iopex *iopex;
  uLong sts;

  iopex = iopexv;

  if (status != OZ_SUCCESS) {
    PRINTF "error %u spinning up disk\n", status);
    (*(iopex -> k.gotkey)) (iopex, status);
    return;
  }

  /* Start reading the host disk geometry */

  memset (&(iopex -> k.disk_getinfo1), 0, sizeof iopex -> k.disk_getinfo1);
  sts = oz_knl_iostart3 (1, NULL, iopex -> k.hostiochan, OZ_PROCMODE_KNL, getkey_gotinfo, iopex, NULL, NULL, NULL, NULL, 
                         OZ_IO_DISK_GETINFO1, sizeof iopex -> k.disk_getinfo1, &(iopex -> k.disk_getinfo1));
  if (sts != OZ_STARTED) getkey_gotinfo (iopex, sts);
}

/* Got host disk geometry info */

static void getkey_gotinfo (void *iopexv, uLong status)

{
  char unitname[OZ_DEVUNIT_NAMESIZE];
  Devex *prevdevex, *stegodevex;
  Iopex *iopex;
  OZ_Devunit *hostdevunit, *prevdevunit;
  OZ_IO_disk_readblocks disk_readblocks;
  uLong sts;

  iopex = iopexv;

  if (status != OZ_SUCCESS) {
    PRINTF "error %u getting disk %s info\n", status, iopex -> k.hostdevname);
    (*(iopex -> k.gotkey)) (iopex, status);
    return;
  }

  /* Determine where this partition starts on the disk -                            */
  /* If partition 'a', it starts at LBN 0                                           */
  /* Otherwise, the partchar-1 disk must be set up so we know where this one starts */

  hostdevunit = oz_knl_iochan_getdevunit (iopex -> k.hostiochan);

  iopex -> k.startlbn = 0;
  if (iopex -> k.partchar != 'a') {
    oz_sys_sprintf (sizeof unitname, unitname, "%s.%c", oz_knl_devunit_devname (hostdevunit), iopex -> k.partchar - 1);
    prevdevunit = oz_knl_devunit_lookup (unitname);
    if (prevdevunit == NULL) {
      (*(iopex -> k.gotkey)) (iopex, OZ_NOTMOUNTED);
      return;
    }
    prevdevex = oz_knl_devunit_ex (prevdevunit);
    iopex -> k.startlbn = prevdevex -> startlbn + prevdevex -> partsize;
    oz_knl_devunit_increfc (prevdevunit, -1);
  }

  /* Start reading the partition descriptor block */

  iopex -> k.stegohdr = OZ_KNL_PGPMALLOQ (iopex -> k.disk_getinfo1.blocksize);
  if (iopex -> k.stegohdr == NULL) {
    (*(iopex -> k.gotkey)) (iopex, OZ_EXQUOTAPGP);
    return;
  }

  memset (&disk_readblocks, 0, sizeof disk_readblocks);
  disk_readblocks.size = iopex -> k.disk_getinfo1.blocksize;
  disk_readblocks.buff = iopex -> k.stegohdr;
  disk_readblocks.slbn = iopex -> k.startlbn;
  sts = oz_knl_iostart3 (1, NULL, iopex -> k.hostiochan, OZ_PROCMODE_KNL, getkey_readhdrblock, iopex, NULL, NULL, NULL, NULL, 
                         OZ_IO_DISK_READBLOCKS, sizeof disk_readblocks, &disk_readblocks);
  if (sts != OZ_STARTED) getkey_readhdrblock (iopex, sts);
}

/* Header block has been read in from disk */

static void getkey_readhdrblock (void *iopexv, uLong status)

{
  Iopex *iopex;

  iopex = iopexv;

  if (status != OZ_SUCCESS) PRINTF "error %u reading header block\n", status);

  /* Decrypt the block */

  else {
    oz_sys_hash (sizeof iopex -> k.startlbn, 			// size of 'text' to hash
                 &(iopex -> k.startlbn), 			// text to hash = block number
                 iopex -> feedback);				// where to put the hash

    oz_sys_cipher_decrypt (iopex -> k.decryptor, 		// decryption context pointer
                           iopex -> feedback, 			// cypherblock chaining feedback buffer
                           iopex -> k.disk_getinfo1.blocksize, 	// number of bytes to decrypt
                           iopex -> k.stegohdr, 		// where to get the cyphertext
                           iopex -> k.stegohdr);		// where to put the plaintext
  }

  /* That's it for now */

  (*(iopex -> k.gotkey)) (iopex, status);
}

/************************************************************************/
/*									*/
/*  Dismount - ie, break connection to host disk and go offline		*/
/*									*/
/************************************************************************/

static uLong stego_dismount (Iopex *iopex)

{
  Devex *devex;
  OZ_Iochan *hostiochan;
  uLong sts, vl;

  devex = iopex -> devex;

  vl = oz_hw_smplock_wait (&(devex -> smplock_vl));
  if ((devex -> dismpend != NULL) || (devex -> hostiochan == NULL)) sts = OZ_DEVOFFLINE;
  else if (devex -> rwinprog != 0) {
    sts = OZ_STARTED;
    devex -> dismpend = iopex;
  } else {
    hostiochan = devex -> hostiochan;
    removefromdevexs (devex);
    sts = OZ_SUCCESS;
  }
  oz_hw_smplock_clr (&(devex -> smplock_vl), vl);
  if (sts == OZ_SUCCESS) oz_knl_iochan_increfc (hostiochan, -1);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Read and decrypt blocks						*/
/*									*/
/*    Input:								*/
/*									*/
/*	iopex    = I/O request being processed				*/
/*	slbn     = logical block number to start at			*/
/*	size     = size of buffer to read (in bytes)			*/
/*	pageoffs = offset in first page to start at			*/
/*	phypages = array of physical page numbers			*/
/*									*/
/*    Output:								*/
/*									*/
/*	stego_readblocks = queuing status				*/
/*									*/
/************************************************************************/

static uLong stego_readblocks (Iopex *iopex, OZ_Dbn slbn, uLong size, uLong pageoffs, const OZ_Mempage *phypages)

{
  Devex *devex;
  OZ_IO_disk_readpages disk_readpages;
  uLong sts;

  devex = iopex -> devex;
  sts   = startrw (devex, &slbn, size, pageoffs);
  if (sts != OZ_SUCCESS) return (sts);

  iopex -> u.readblocks.size   = size;
  iopex -> u.readblocks.pages  = phypages;
  iopex -> u.readblocks.offset = pageoffs;
  iopex -> u.readblocks.slbn   = slbn;
  iopex -> u.readblocks.status = OZ_SUCCESS;
  memset (&disk_readpages, 0, sizeof disk_readpages);

  if (slbn + (size / devex -> disk_getinfo1.blocksize) - devex -> startlbn > devex -> partsize) {
    iopex -> u.readblocks.segments = 2;
    disk_readpages.size   = (devex -> startlbn + devex -> partsize - slbn) * devex -> disk_getinfo1.blocksize;
    disk_readpages.pages  = phypages;
    disk_readpages.offset = pageoffs;
    disk_readpages.slbn   = slbn;
    sts = oz_knl_iostart3 (1, NULL, devex -> hostiochan, OZ_PROCMODE_KNL, readcomplete, iopex, NULL, NULL, NULL, NULL, 
                           OZ_IO_DISK_READPAGES, sizeof disk_readpages, &(disk_readpages));
    if (sts != OZ_STARTED) readcomplete (iopex, sts);
    disk_readpages.slbn    = devex -> startlbn + 1;
    disk_readpages.offset += disk_readpages.size;
    disk_readpages.pages  += disk_readpages.offset >> OZ_HW_L2PAGESIZE;
    disk_readpages.offset %= 1 << OZ_HW_L2PAGESIZE;
    disk_readpages.size   = size - disk_readpages.size;
    sts = oz_knl_iostart3 (1, NULL, devex -> hostiochan, OZ_PROCMODE_KNL, readcomplete, iopex, NULL, NULL, NULL, NULL, 
                           OZ_IO_DISK_READPAGES, sizeof disk_readpages, &(disk_readpages));
    if (sts != OZ_STARTED) readcomplete (iopex, sts);
  } else {
    iopex -> u.readblocks.segments = 1;
    disk_readpages.size   = size;
    disk_readpages.pages  = phypages;
    disk_readpages.offset = pageoffs;
    disk_readpages.slbn   = slbn;
    sts = oz_knl_iostart3 (1, NULL, devex -> hostiochan, OZ_PROCMODE_KNL, readcomplete, iopex, NULL, NULL, NULL, NULL, 
                           OZ_IO_DISK_READPAGES, sizeof disk_readpages, &(disk_readpages));
    if (sts != OZ_STARTED) readcomplete (iopex, sts);
  }
  return (OZ_STARTED);
}

/* The disk read has completed */

static void readcomplete (void *iopexv, uLong status)

{
  Devex *devex;
  Iopex *iopex;
  OZ_Mempage curpage;
  OZ_Pagentry savepte;
  uByte *virtaddr;
  uLong partialblock, sts, virtsize;

  iopex = iopexv;
  devex = iopex -> devex;

  do {
    sts = iopex -> u.readblocks.status;
    if (sts != OZ_SUCCESS) break;
    sts = status;
  } while (!oz_hw_atomic_setif_long (&(iopex -> u.readblocks.status), sts, OZ_SUCCESS));

  if (oz_hw_atomic_inc_long (&(iopex -> u.readblocks.segments), -1) > 0) return;

  if (sts == OZ_SUCCESS) {
    curpage = OZ_PHYPAGE_NULL;
    partialblock = 0;
    while (iopex -> u.readblocks.size > 0) {

      /* The most we can do at once is to the end of the physical page, the amount requested or to the end of the logical block */

      virtsize = (1 << OZ_HW_L2PAGESIZE) - iopex -> u.readblocks.offset;
      if (virtsize > iopex -> u.readblocks.size) virtsize = iopex -> u.readblocks.size;
      if (virtsize + partialblock > devex -> disk_getinfo1.blocksize) {
        virtsize = devex -> disk_getinfo1.blocksize - partialblock;
      }

      /* If the required page is not mapped, map it */

      if (curpage != iopex -> u.readblocks.pages[0]) {
        virtaddr = oz_hw_phys_mappage (iopex -> u.readblocks.pages[0], 			// phys page to map
                                       (curpage == OZ_PHYPAGE_NULL) ? &savepte : NULL);	// save the old pte first time
        curpage  = iopex -> u.readblocks.pages[0];					// remember what is mapped now
      }

      /* If starting a new logical block, reset decryption feedback so it matches the block */

      if (partialblock == 0) oz_sys_hash (sizeof iopex -> u.readblocks.slbn, 		// size of 'text' to hash
                                          &(iopex -> u.readblocks.slbn), 		// text to hash = block number
                                          iopex -> feedback);				// where to put the hash

      /* Decrypt the data */

      oz_sys_cipher_decrypt (devex -> decryptor, 					// decryption context pointer
                             iopex -> feedback, 					// cypherblock chaining feedback buffer
                             virtsize, 							// number of bytes to decrypt
                             virtaddr + iopex -> u.readblocks.offset, 			// where to get the cyphertext
                             virtaddr + iopex -> u.readblocks.offset);			// where to put the plaintext

      /* Increment how much we've done in the logical block.  If the whole */
      /* thing, set up for new decryption context next time thru loop.     */

      partialblock += virtsize;
      if (partialblock == devex -> disk_getinfo1.blocksize) {
        iopex -> u.readblocks.slbn ++;
        if (iopex -> u.readblocks.slbn == devex -> startlbn + devex -> partsize) iopex -> u.readblocks.slbn = devex -> startlbn + 1;
        partialblock = 0;
      }

      /* Increment buffer size and address for next time thru loop */

      iopex -> u.readblocks.size   -= virtsize;
      iopex -> u.readblocks.offset += virtsize;
      iopex -> u.readblocks.pages  += iopex -> u.readblocks.offset >> OZ_HW_L2PAGESIZE;
      iopex -> u.readblocks.offset %= (1 << OZ_HW_L2PAGESIZE);
    }

    /* Unmap user buffer */

    if (curpage != OZ_PHYPAGE_NULL) oz_hw_phys_unmappage (savepte);
  }

  /* All done */

  decrwinprog (devex);
  iodone (iopex, sts);
}

/************************************************************************/
/*									*/
/*  Encrypt and write blocks						*/
/*									*/
/*    Input:								*/
/*									*/
/*	iopex    = I/O request being processed				*/
/*	slbn     = logical block number to start at			*/
/*	size     = size of buffer to write (in bytes)			*/
/*	pageoffs = offset in first page to start at			*/
/*	phypages = array of physical page numbers			*/
/*									*/
/*    Output:								*/
/*									*/
/*	stego_writeblocks = queuing status				*/
/*									*/
/************************************************************************/

static uLong stego_writeblocks (Iopex *iopex, OZ_Dbn slbn, uLong size, uLong pageoffs, const OZ_Mempage *phypages)

{
  Devex *devex;
  OZ_IO_disk_writeblocks disk_writeblocks;
  OZ_Mempage curpage;
  OZ_Pagentry savepte;
  uByte *buff, *virtaddr;
  uLong partialblock, sts, virtsize;

  devex = iopex -> devex;

  sts = startrw (devex, &slbn, size, pageoffs);
  if (sts != OZ_SUCCESS) return (sts);

  buff = OZ_KNL_PGPMALLOQ (size);
  if (buff == NULL) return (OZ_EXQUOTAPGP);
  iopex -> u.writeblocks.buff = buff;

  memset (&disk_writeblocks, 0, sizeof disk_writeblocks);
  disk_writeblocks.size = size;
  disk_writeblocks.buff = buff;
  disk_writeblocks.slbn = slbn;

  curpage = OZ_PHYPAGE_NULL;
  partialblock = 0;
  while (size > 0) {

    /* The most we can do at once is to the end of the physical page, the amount requested or to the end of the logical block */

    virtsize = (1 << OZ_HW_L2PAGESIZE) - pageoffs;
    if (virtsize > size) virtsize = size;
    if (virtsize + partialblock > devex -> disk_getinfo1.blocksize) {
      virtsize = devex -> disk_getinfo1.blocksize - partialblock;
    }

    /* If the required page is not mapped, map it */

    if (curpage != phypages[0]) {
      virtaddr = oz_hw_phys_mappage (phypages[0], 						// phys page to map
                                     (curpage == OZ_PHYPAGE_NULL) ? &savepte : NULL);		// save the old pte first time
      curpage  = phypages[0];									// remember what is mapped now
    }

    /* If starting a new logical block, reset decryption feedback so it matches the block */

    if (partialblock == 0) oz_sys_hash (sizeof slbn, 						// size of 'text' to hash
                                        &slbn, 							// text to hash = block number
                                        iopex -> feedback);					// where to put the hash

    /* Encrypt the data */

    oz_sys_cipher_encrypt (devex -> encryptor, 			// encryption context pointer
                           iopex -> feedback, 			// cypherblock chaining feedback buffer
                           virtsize, 				// number of bytes to encrypt
                           virtaddr + pageoffs, 		// where to get the plaintext
                           buff);				// where to put the cyphertext

    /* Increment how much we've done in the logical block.  If the whole */
    /* thing, set up for new decryption context next time thru loop.     */

    partialblock += virtsize;
    if (partialblock == devex -> disk_getinfo1.blocksize) {
      slbn ++;
      if (slbn == devex -> startlbn + devex -> partsize) slbn = devex -> startlbn + 1;
      partialblock = 0;
    }

    /* Increment buffer size and address for next time thru loop */

    size     -= virtsize;
    buff     += virtsize;
    pageoffs += virtsize;
    phypages += pageoffs >> OZ_HW_L2PAGESIZE;
    pageoffs %= (1 << OZ_HW_L2PAGESIZE);
  }

  /* Unmap user buffer */

  if (curpage != OZ_PHYPAGE_NULL) oz_hw_phys_unmappage (savepte);

  /* Start writing encrypted data */

  iopex -> u.writeblocks.status = OZ_SUCCESS;

  if (disk_writeblocks.size / devex -> disk_getinfo1.blocksize + disk_writeblocks.slbn - devex -> startlbn > devex -> partsize) {
    iopex -> u.writeblocks.segments = 2;
    size = disk_writeblocks.size;
    disk_writeblocks.size = (devex -> startlbn + devex -> partsize - disk_writeblocks.slbn) * devex -> disk_getinfo1.blocksize;
    sts = oz_knl_iostart3 (1, NULL, devex -> hostiochan, OZ_PROCMODE_KNL, writecomplete, iopex, NULL, NULL, NULL, NULL, 
                           OZ_IO_DISK_WRITEBLOCKS, sizeof disk_writeblocks, &disk_writeblocks);
    if (sts != OZ_STARTED) writecomplete (iopex, sts);
    disk_writeblocks.slbn  = devex -> startlbn + 1;
    disk_writeblocks.buff += disk_writeblocks.size;
    disk_writeblocks.size  = size - disk_writeblocks.size;
    sts = oz_knl_iostart3 (1, NULL, devex -> hostiochan, OZ_PROCMODE_KNL, writecomplete, iopex, NULL, NULL, NULL, NULL, 
                           OZ_IO_DISK_WRITEBLOCKS, sizeof disk_writeblocks, &disk_writeblocks);
    if (sts != OZ_STARTED) writecomplete (iopex, sts);
  } else {
    iopex -> u.writeblocks.segments = 1;
    sts = oz_knl_iostart3 (1, NULL, devex -> hostiochan, OZ_PROCMODE_KNL, writecomplete, iopex, NULL, NULL, NULL, NULL, 
                           OZ_IO_DISK_WRITEBLOCKS, sizeof disk_writeblocks, &disk_writeblocks);
    if (sts != OZ_STARTED) writecomplete (iopex, sts);
  }
  return (OZ_STARTED);
}

/* The disk write has completed */

static void writecomplete (void *iopexv, uLong status)

{
  Iopex *iopex;
  uLong sts;

  iopex = iopexv;

  do {
    sts = iopex -> u.writeblocks.status;
    if (sts != OZ_SUCCESS) break;
    sts = status;
  } while (!oz_hw_atomic_setif_long (&(iopex -> u.writeblocks.status), sts, OZ_SUCCESS));

  if (oz_hw_atomic_inc_long (&(iopex -> u.writeblocks.segments), -1) > 0) return;

  OZ_KNL_PGPFREE (iopex -> u.writeblocks.buff);
  decrwinprog (iopex -> devex);
  iodone (iopex, sts);
}

/************************************************************************/
/*									*/
/*  Start a read/write request						*/
/*									*/
/*    Input:								*/
/*									*/
/*	devex    = stego device the request is for			*/
/*	*slbn_r  = lbn in the stego device to start at			*/
/*	size     = size (in bytes) of the transfer			*/
/*	pageoffs = offset in first physical page for data		*/
/*									*/
/*    Output:								*/
/*									*/
/*	startrw = OZ_SUCCESS : it's ok to start request			*/
/*	                else : error status				*/
/*	*slbn_r = starting block number on host disk drive		*/
/*									*/
/************************************************************************/

static uLong startrw (Devex *devex, OZ_Dbn *slbn_r, uLong size, uLong pageoffs)

{
  OZ_Dbn slbn;
  uLong sts, vl;

  slbn = *slbn_r;

  if ((size | pageoffs) & (OZ_SYS_CIPHER_BLKSIZE - 1)) return (OZ_UNALIGNEDXLEN);	// cipher blocks can't span page bundaries
  if ((size % devex -> disk_getinfo1.blocksize) != 0) return (OZ_UNALIGNEDXLEN);	// must be a multiple of disk block size

  if (slbn >= devex -> partsize) return (OZ_BADBLOCKNUMBER);				// can't start off end of partition
  if ((size / devex -> disk_getinfo1.blocksize) >= (devex -> partsize - slbn)) return (OZ_BADBLOCKNUMBER); // can't go past end of partition

  *slbn_r = ((slbn + devex -> lbnrotator) % (devex -> partsize - 1)) + devex -> startlbn + 1; // rotate and relocate the output block number

  sts = OZ_DEVOFFLINE;							// assume it's not mounted
  vl = oz_hw_smplock_wait (&(devex -> smplock_vl));			// lock database
  if ((devex -> dismpend == NULL) && (devex -> hostiochan != NULL)) {	// see if it's mounted
    sts = OZ_VOLNOTVALID;
    if (devex -> volvalid) {
      devex -> rwinprog ++;						// ok, keep it from being dismounted
      sts = OZ_SUCCESS;
    }
  }
  oz_hw_smplock_clr (&(devex -> smplock_vl), vl);			// release database
  return (sts);								// return status
}

/* Decrement reads/writes in progress and complete dismount if pending */

static void decrwinprog (Devex *devex)

{
  Iopex *dismpend;
  OZ_Iochan *hostiochan;
  uLong vl;

  dismpend   = NULL;									// haven't found pending dismount
  hostiochan = NULL;									// haven't found i/o chan to close
  vl = oz_hw_smplock_wait (&(devex -> smplock_vl));					// lock database
  if (-- (devex -> rwinprog) == 0) {							// decrement rw's in progress
    dismpend = devex -> dismpend;							// zero, see if dismount pending
    if (dismpend != NULL) {
      hostiochan = devex -> hostiochan;							// ok, get i/o channel to close
      removefromdevexs (devex);								// close out the device
    }
  }
  oz_hw_smplock_clr (&(devex -> smplock_vl), vl);					// release lock
  if (hostiochan != NULL) oz_knl_iochan_increfc (hostiochan, -1);			// close channel to host
  if (dismpend   != NULL) iodone (dismpend, OZ_SUCCESS);				// post dismount completion
}

/* Hostiochan is being closed, remove from devexs list */

static void removefromdevexs (Devex *devex)

{
  Devex **ldevex, *xdevex;
  uLong dv;

  /* Reset 'important' stuff in the devex so it could theoretically be re-used */

  devex -> hostiochan  = NULL;
  devex -> hostdevunit = NULL;
  devex -> volvalid = 0;
  devex -> rwinprog = 0;
  devex -> dismpend = NULL;
  if (devex -> decryptor != NULL) {
    OZ_KNL_NPPFREE (devex -> decryptor);
    devex -> decryptor = NULL;
  }
  if (devex -> encryptor != NULL) {
    OZ_KNL_NPPFREE (devex -> encryptor);
    devex -> encryptor = NULL;
  }

  /* Remove it from the devexs list */

  dv = oz_hw_smplock_wait (&smplock_dv);
  for (ldevex = &devexs; (xdevex = *ldevex) != devex; ldevex = &(devex -> next)) {}
  *ldevex = xdevex -> next;
  oz_hw_smplock_clr (&smplock_dv, dv);
}


/* Mark the request as abortable - Do this when it is certain that iodone */
/* will be called but make sure it is done before iodone is called        */

static void abortable (Iopex *iopex)

{
  Devex *devex;
  uLong vl;

  devex = iopex -> devex;
  vl = oz_hw_smplock_wait (&(devex -> smplock_vl));
  iopex -> next = devex -> iopexs;
  iopex -> prev = &(devex -> iopexs);
  if (iopex -> next != NULL) iopex -> next -> prev = &(iopex -> next);
  devex -> iopexs = iopex;
  oz_hw_smplock_clr (&(devex -> smplock_vl), vl);
}

/* Post I/O completion after removing request from abort queue */

static void iodone (Iopex *iopex, uLong status)

{
  uLong dv;

  if (iopex -> prev != NULL) {
    dv = oz_hw_smplock_wait (&smplock_dv);
    if ((*(iopex -> prev) = iopex -> next) != NULL) {
      iopex -> next -> prev = iopex -> prev;
    }
    oz_hw_smplock_clr (&smplock_dv, dv);
  }
  oz_knl_iodone (iopex -> ioop, status, NULL, NULL, NULL);
}
