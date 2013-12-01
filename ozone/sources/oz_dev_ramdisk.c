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
/*  Ramdisk driver							*/
/*									*/
/*  To create a ramdisk device using any available physical pages, use 	*/
/*  the mount command like this:					*/
/*									*/
/*	mount <size_wanted> ramdisk					*/
/*	- you will be charged 'physical memory' quota for the pages 	*/
/*	  used								*/
/*	- the resulting devices are called ramdisk_1, _2, etc.		*/
/*	- the data is lost when device is deleted via dismounting	*/
/*									*/
/*  Or to create one in a specific spot in physical memory:		*/
/*									*/
/*	mount <size_wanted>@<physical_address> ramdisk			*/
/*	- but memory size must have been constrained below the 		*/
/*	  requested address with the 'memory_megabytes' parameter in 	*/
/*	  the boot block						*/
/*	- the resulting devices are called ramdisk.<size_wanted>@<physical_address>
/*	  and they can be autogen'd					*/
/*	- the data is preserved in the physical memory after deleting 	*/
/*	  the device via dismounting					*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_dev_disk.h"
#include "oz_io_disk.h"
#include "oz_knl_dcache.h"
#include "oz_knl_devio.h"
#include "oz_knl_hw.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_phymem.h"
#include "oz_knl_procmode.h"
#include "oz_knl_quota.h"
#include "oz_knl_sdata.h"
#include "oz_knl_section.h"
#include "oz_knl_spte.h"
#include "oz_knl_status.h"
#include "oz_knl_thread.h"
#include "oz_sys_xprintf.h"

#define L2DISK_BLOCK_SIZE (9)
#define DISK_BLOCK_SIZE (1<<L2DISK_BLOCK_SIZE)

/* Device extension structure */

typedef struct Devex Devex;

struct Devex { const char *magic;		/* points to our magic string */
               OZ_Mempage *map_sysvaddr;	/* virtual address of map array */
               OZ_Mempage map_pages;		/* number of map sptes */
               OZ_Dbn diskblocks;		/* number of disk blocks */
               OZ_Mempage diskpages;		/* number of pages needed for disk */
               OZ_Quota *quota;			/* quota that was charged for the diskpages and map_pages */
               Long quotapages;			/* number of pages charged to quota */
               int volvalid;			/* volume is valid */
             };

/* Function table */

static uLong ramdisk_clonecre (OZ_Devunit *template_devunit, void *template_devex, int template_cloned, OZ_Procmode procmode, OZ_Devunit **cloned_devunit);
static int ramdisk_clonedel (OZ_Devunit *cloned_devunit, void *devexv, int cloned);
static uLong ramdisk_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode, 
                            OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap);

static const OZ_Devfunc ramdisk_functable = { sizeof (Devex), 0, 0, 0, NULL, ramdisk_clonecre, ramdisk_clonedel, NULL, NULL, NULL, ramdisk_start, NULL };

/* Internal static data */

static const char ramdisk_magic[] = "*oz_dev_ramdisk**magic*";
static int initialized = 0;
static OZ_Devclass *devclass;
static OZ_Devdriver *devdriver;

/* Internal routines */

static OZ_Devunit *ramdisk_autogen (void *dummy, OZ_Devunit *host_devunit, const char *devname, const char *suffix);
static uLong activate (OZ_Devunit *devunit, Devex *devex, const char *descr);
static uLong atomemsize (const char *string, uLong *usedup_r);
static uLong writeblocks (uLong size, const void *buff, OZ_Dbn slbn, Devex *devex);
static uLong writepages (uLong size, const OZ_Mempage *pages, uLong offset, OZ_Dbn slbn, Devex *devex);
static uLong readblocks (uLong size, void *buff, OZ_Dbn slbn, Devex *devex);
static uLong readpages (uLong size, const OZ_Mempage *pages, uLong offset, OZ_Dbn slbn, Devex *devex);
static uLong ramdisk_map (OZ_Iochan *iochan, OZ_Dcmpb *dcmpb);
static uLong ramdisk_pfmap (OZ_Iochan *iochan, OZ_Dbn logblock, OZ_Mempage *phypage_r);

/************************************************************************/
/*									*/
/*  Boot-time initialization routine					*/
/*									*/
/************************************************************************/

void oz_dev_ramdisk_init ()

{
  OZ_Devunit *devunit;

  if (initialized) return;

  oz_knl_printk ("oz_dev_ramdisk_init\n");
  initialized = 1;

  /* Create template device and call autogen routine to automatically create ramdisk.<size>@<addr> devices when called for */

  devclass  = oz_knl_devclass_create (OZ_IO_DISK_CLASSNAME, OZ_IO_DISK_BASE, OZ_IO_DISK_MASK, "ramdisk");
  devdriver = oz_knl_devdriver_create (devclass, "ramdisk");
  devunit   = oz_knl_devunit_create (devdriver, "ramdisk", "mount <size_in_bytes>[K/M]", &ramdisk_functable, 0, oz_s_secattr_tempdev);
  oz_knl_devunit_autogen (devunit, ramdisk_autogen, NULL);
}

/************************************************************************/
/*									*/
/*  This routine is called to autogen a unit				*/
/*									*/
/*    Input:								*/
/*									*/
/*	host_devunit = template device unit pointer			*/
/*	devname      = points to ramdisk.<size>@<addr> string		*/
/*	suffix       = points to <size>@<addr> string within devname	*/
/*	smplevel     = softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	ramdisk_autogen = NULL : failed					*/
/*	                  else : points to created devunit		*/
/*									*/
/************************************************************************/

static OZ_Devunit *ramdisk_autogen (void *dummy, OZ_Devunit *host_devunit, const char *devname, const char *suffix)

{
  char unitname[OZ_DEVUNIT_NAMESIZE];
  const char *p, *q;
  Devex *devex;
  OZ_Devunit *cloned_devunit;
  OZ_Seckeys *seckeys;
  uLong sts;

  p = strchr (suffix, '.');					// <size>@<adrs> might be followed by .oz_dfs or .partition or something
  if (p == NULL) p = suffix + strlen (suffix);
  q = strchr (suffix, '@');					// can only autogen if they tell us where in physical memory it is
  if ((q == NULL) || (q > p)) return (NULL);			// (or we'd have two devices named something like ramdisk.2m)

  if (p - devname >= OZ_DEVUNIT_NAMESIZE) {			// see if string is too long
    oz_knl_printk ("oz_dev_ramdisk autogen: devname %s too long\n", devname);
    return (NULL);
  }
  memcpy (unitname, devname, p - devname);			// ok, get name string
  unitname[p-devname] = 0;

  cloned_devunit = oz_knl_devunit_create (devdriver, unitname, unitname, &ramdisk_functable, 1, oz_knl_thread_getdefcresecattr (NULL));
  if (cloned_devunit == NULL) return (NULL);			// (might already have one there)
  devex = oz_knl_devunit_ex (cloned_devunit);			// create the requested device
  memset (devex, 0, sizeof *devex);
  devex -> magic = ramdisk_magic;

  sts = activate (cloned_devunit, devex, unitname + (suffix - devname)); // activate it
  if (sts == OZ_SUCCESS) return (cloned_devunit);		// if successful, return cloned device's devunit pointer

  oz_knl_devunit_increfc (cloned_devunit, 0);			// else, delete cloned device
  oz_knl_printk ("oz_dev_ramdisk autogen: error %u activating %s\n", sts, unitname);
  return (NULL);						// ... and return failure status
}

/************************************************************************/
/*									*/
/*  A channel was assigned to device for first time, clone if channel 	*/
/*  was assigned to the template device					*/
/*									*/
/*  Runs with dv smplock set						*/
/*									*/
/************************************************************************/

static uLong ramdisk_clonecre (OZ_Devunit *template_devunit, void *template_devex, int template_cloned, OZ_Procmode procmode, OZ_Devunit **cloned_devunit)

{
  Devex *devex;
  char unitname[OZ_DEVUNIT_NAMESIZE];

  static uLong unitno = 0;

  if (template_cloned) {
    *cloned_devunit = template_devunit;
    oz_knl_devunit_increfc (template_devunit, 1);
  } else {
    oz_sys_sprintf (sizeof unitname, unitname, "ramdisk_%u", ++ unitno);
    *cloned_devunit = oz_knl_devunit_create (devdriver, unitname, unitname, &ramdisk_functable, 1, oz_knl_thread_getdefcresecattr (NULL));
    devex = oz_knl_devunit_ex (*cloned_devunit);
    memset (devex, 0, sizeof *devex);
    devex -> magic = ramdisk_magic;
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

static int ramdisk_clonedel (OZ_Devunit *cloned_devunit, void *devexv, int cloned)

{
  Devex *devex;

  devex = devexv;

  if (cloned) {

    /* Don't delete if it is mounted.  This routine will be called again */
    /* when the dismount utility deassigns it channel to the device.     */

    if (devex -> map_sysvaddr != NULL) cloned = 0;
  }
  return (cloned);
}

/************************************************************************/
/*									*/
/*  Start performing a disk i/o function				*/
/*									*/
/************************************************************************/

static uLong ramdisk_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode, 
                            OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap)

{
  Devex *devex;
  uLong sts;

  devex = devexv;

  /* Process individual functions */

  switch (funcode) {

    /* The MOUNTVOL function is used to set the ramdisk size and allocate the memory */

    case OZ_IO_FS_MOUNTVOL: {
      OZ_IO_fs_mountvol fs_mountvol;

      movc4 (as, ap, sizeof fs_mountvol, &fs_mountvol);
      sts = activate (devunit, devex, fs_mountvol.devname);
      return (sts);
    }

    /* Dismount undoes all that mount does */

    case OZ_IO_FS_DISMOUNT: {
      Long quotapages;
      OZ_Mempage diskpages, i, mappages, phypage;
      OZ_Quota *quota;
      OZ_Section_pagestate pagestate;
      uLong pm;
      void *sysvaddr;

      /* Lock memory allocation and make sure device is set up */

      pm = oz_hw_smplock_wait (&oz_s_smplock_pm);
      if (devex -> map_sysvaddr == NULL) {
        oz_hw_smplock_clr (&oz_s_smplock_pm, pm);
        return (OZ_NOTMOUNTED);
      }

      diskpages = devex -> diskpages;
      mappages  = devex -> map_pages;
      sysvaddr  = devex -> map_sysvaddr;

      if (devex -> map_sysvaddr[0] < oz_s_phymem_totalpages) {
        for (i = diskpages; i > 0;) {
          phypage = devex -> map_sysvaddr[--i];
          oz_knl_phymem_freepage (phypage);
        }
      }
      for (i = mappages; i > 0;) {
        -- i;
        oz_hw_pte_readsys (OZ_HW_VADDRTOVPAGE (devex -> map_sysvaddr) + i, NULL, &phypage, NULL, NULL);
        oz_knl_phymem_freepage (phypage);
      }
      quota = devex -> quota;
      quotapages = devex -> quotapages;
      devex -> map_sysvaddr  = NULL;
      devex -> diskpages     = 0;
      devex -> map_pages     = 0;
      devex -> volvalid      = 0;
      devex -> quota         = NULL;
      devex -> quotapages    = 0;
      oz_hw_smplock_clr (&oz_s_smplock_pm, pm);
      if (oz_s_inloader) OZ_KNL_NPPFREE (sysvaddr);
      else oz_knl_spte_free (mappages, OZ_HW_VADDRTOVPAGE (sysvaddr));
      if (quota != NULL) oz_knl_quota_credit (devex -> quota, OZ_QUOTATYPE_PHM, quotapages, -1);
      return (OZ_SUCCESS);
    }

    /* Set volume valid bit one way or the other (noop for us) */

    case OZ_IO_DISK_SETVOLVALID: {
      OZ_IO_disk_setvolvalid disk_setvolvalid;

      if (devex -> map_sysvaddr == NULL) return (OZ_DEVOFFLINE);
      movc4 (as, ap, sizeof disk_setvolvalid, &disk_setvolvalid);
      devex -> volvalid = disk_setvolvalid.valid;
      return (OZ_SUCCESS);
    }

    /* Write blocks to the disk */

    case OZ_IO_DISK_WRITEBLOCKS: {
      OZ_IO_disk_writeblocks disk_writeblocks;

      if (!(devex -> volvalid)) return (OZ_VOLNOTVALID);

      /* Lock I/O buffer in memory */

      movc4 (as, ap, sizeof disk_writeblocks, &disk_writeblocks);
      sts = oz_knl_ioop_lockr (ioop, disk_writeblocks.size, disk_writeblocks.buff, NULL, NULL, NULL);

      /* If that was successful, queue the request to the drive for processing */

      if (sts == OZ_SUCCESS) {
        sts = writeblocks (disk_writeblocks.size, 
                           disk_writeblocks.buff, 
                           disk_writeblocks.slbn, 
                           devex);
      }
      return (sts);
    }

    /* Read blocks from the disk */

    case OZ_IO_DISK_READBLOCKS: {
      OZ_IO_disk_readblocks disk_readblocks;

      if (!(devex -> volvalid)) return (OZ_VOLNOTVALID);

      /* Lock I/O buffer in memory */

      movc4 (as, ap, sizeof disk_readblocks, &disk_readblocks);
      sts = oz_knl_ioop_lockw (ioop, disk_readblocks.size, disk_readblocks.buff, NULL, NULL, NULL);

      /* If that was successful, queue the request to the drive for processing */

      if (sts == OZ_SUCCESS) {
        sts = readblocks (disk_readblocks.size, 
                          disk_readblocks.buff, 
                          disk_readblocks.slbn, 
                          devex);
      }
      return (sts);
    }

    /* Get info part 1 */

    case OZ_IO_DISK_GETINFO1: {
      OZ_IO_disk_getinfo1 disk_getinfo1;

      if (!OZ_HW_WRITABLE (as, ap, procmode)) return (OZ_ACCVIO);
      memset (&disk_getinfo1, 0, sizeof disk_getinfo1);		// clear stuff we don't know/care about
      disk_getinfo1.blocksize     = DISK_BLOCK_SIZE;		// return the block size we're using
      disk_getinfo1.totalblocks   = devex -> diskblocks;	// return the number of blocks
      disk_getinfo1.ramdisk_map   = ramdisk_map;		// tell caller we're a cpu memory based ramdisk
      disk_getinfo1.ramdisk_pfmap = ramdisk_pfmap;
      movc4 (sizeof disk_getinfo1, &disk_getinfo1, as, ap);	// return param block to caller
      return (OZ_SUCCESS);
    }

    /* WRITEPAGES and READPAGES must be from kernel mode */

    case OZ_IO_DISK_WRITEPAGES: {
      OZ_IO_disk_writepages disk_writepages;

      if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);
      if (!(devex -> volvalid)) return (OZ_VOLNOTVALID);

      movc4 (as, ap, sizeof disk_writepages, &disk_writepages);

      sts = writepages (disk_writepages.size, 
                        disk_writepages.pages, 
                        disk_writepages.offset, 
                        disk_writepages.slbn, 
                        devex);
      return (sts);
    }

    case OZ_IO_DISK_READPAGES: {
      OZ_IO_disk_readpages disk_readpages;

      if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);
      if (!(devex -> volvalid)) return (OZ_VOLNOTVALID);

      movc4 (as, ap, sizeof disk_readpages, &disk_readpages);

      sts = readpages (disk_readpages.size, 
                       disk_readpages.pages, 
                       disk_readpages.offset, 
                       disk_readpages.slbn, 
                       devex);
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
/*  Activate a ramdisk device						*/
/*									*/
/*    Input:								*/
/*									*/
/*	devunit, devex = ramdisk to activate				*/
/*	descr = descriptor string					*/
/*	smplevel = softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	activate = OZ_SUCCESS : successful				*/
/*	                 else : error status				*/
/*									*/
/************************************************************************/

static uLong activate (OZ_Devunit *devunit, Devex *devex, const char *descr)

{
  int usedup, usedup2;
  Long quotapages;
  OZ_Dbn diskblocks;
  OZ_IO_fs_mountvol fs_mountvol;
  OZ_Mempage diskpages, i, mappages, phypage, sysvpage;
  OZ_Quota *quota;
  OZ_Seckeys *seckeys;
  OZ_Section_pagestate pagestate;
  uLong diskaddr, disksize, pm, sts;
  void *sysvaddr;

  /* The 'descr' is the number of bytes to allocate for disk, optionally followed by physical address */

  disksize = atomemsize (descr, &usedup);					// get size of disk to create (in bytes)
  diskaddr = 0;									// assume we find main memory pages for it
  if (descr[usedup] == '@') {							// maybe caller is telling us where the pages are
    seckeys = oz_knl_thread_getseckeys (NULL);					// if so, make sure they are a super-duper user
    if (seckeys != NULL) {
      oz_knl_seckeys_increfc (seckeys, -1);
      return (OZ_SECACCDENIED);
    }
    diskaddr = atomemsize (descr + (++ usedup), &usedup2);			// ok, get the physical address of the pages
    usedup  += usedup2;
    if ((diskaddr % (1 << OZ_HW_L2PAGESIZE)) != 0) {
      oz_knl_printk ("oz_dev_ramdisk: disk physical address %X not on page boundary\n", diskaddr);
      return (OZ_BADPARAM);							// must be on a page boundary
    }
    if ((diskaddr >> OZ_HW_L2PAGESIZE) < oz_s_phymem_totalpages) {
      oz_knl_printk ("oz_dev_ramdisk: disk physical address %X within normal system memory\n", diskaddr);
      return (OZ_BADPARAM);							// must be beyond what rest of os is using
    }
  }
  if (descr[usedup] != 0) {
    oz_knl_printk ("oz_dev_ramdisk: bad string '%s' at offset %d, use 'bytesize[@physaddr]'\n", descr, usedup);
    return (OZ_BADPARAM);							// must not be any junk left over on string
  }

  /* Calculate number of blocks and number of pages that correspond */

  diskpages  = (disksize + (1 << OZ_HW_L2PAGESIZE) - 1) >> OZ_HW_L2PAGESIZE;	// get number of pages
  if (diskpages == 0) {
    oz_knl_printk ("oz_dev_ramdisk: bytesize 0x%X must be at least one page\n", disksize);
    return (OZ_BADPARAM);							// must have at least one page
  }
  diskblocks = diskpages * ((1 << OZ_HW_L2PAGESIZE) / DISK_BLOCK_SIZE);		// get corresponding number of blocks
  disksize   = diskpages << OZ_HW_L2PAGESIZE;					// re-calc size for any rounding that happened
  if ((disksize + diskaddr) < disksize) {					// check for overflow/wrap-around
    oz_knl_printk ("oz_dev_ramdisk: bytesize 0x%X @ physaddr 0x%X wraps\n", disksize, diskaddr);
    return (OZ_BADPARAM);
  }

  /* Calculate number of map pages we will need.  Be careful to check for all kinds of overflows. */

  mappages   = diskpages * sizeof (OZ_Mempage);					// number of bytes needed for map array
  if (mappages / sizeof (OZ_Mempage) != diskpages) {				// check for overflow
    oz_knl_printk ("oz_dev_ramdisk: diskpages 0x%X overflows mappages\n", diskpages);
    return (OZ_BADPARAM);
  }
  mappages  += (1 << OZ_HW_L2PAGESIZE) - 1;					// only allocate whole pages
  if (mappages < (1 << OZ_HW_L2PAGESIZE)) {					// check for overflow
    oz_knl_printk ("oz_dev_ramdisk: diskpages 0x%X overflows mappages after rounding\n", diskpages);
    return (OZ_BADPARAM);
  }
  mappages >>= OZ_HW_L2PAGESIZE;						// (Long)mappages should be > 0, needed below

  /* Make sure they have sufficient phymem quota for all physical memory pages we will allocate */

  quotapages = mappages;							// well, maybe just the mappages
  if (diskaddr != 0) quotapages += diskpages;					// but diskpages too, if we have to alloc from main memory
  if (quotapages < (Long)mappages) {						// make sure it didn't overflow
    oz_knl_printk ("oz_dev_ramdisk: mappages 0x%X + diskpages 0x%X overflows quota\n", mappages, diskpages);
    return (OZ_BADPARAM);
  }
  quota = OZ_KNL_QUOTA_DEFAULT;
  if ((quota != NULL) && !oz_knl_quota_debit (quota, OZ_QUOTATYPE_PHM, quotapages)) return (OZ_EXQUOTAPHM);

  /* Allocate some spte's to use for the map array pages to system virtual addresses */
  /* But if in loader, don't bother as we just use a chunk of npp for the map array  */

  if (!oz_s_inloader) {
    sts = oz_knl_spte_alloc (mappages, &sysvaddr, NULL, NULL);
    if (sts != OZ_SUCCESS) goto cred_quota;
    sysvpage = OZ_HW_VADDRTOVPAGE (sysvaddr);
  }

  /* Lock physical memory allocation and make sure device not already set up */

  pm  = oz_hw_smplock_wait (&oz_s_smplock_pm);
  sts = OZ_ALREADYMOUNTED;
  if (devex -> map_sysvaddr != NULL) goto free_sptes;

  /* Allocate the map array pages.  If we're in the loader, just use NPP, because phymem_allocpage doesn't work. */

  if (oz_s_inloader) {
    sts = OZ_EXQUOTANPP;
    sysvaddr = OZ_KNL_NPPMALLOQ (mappages << OZ_HW_L2PAGESIZE);
    if (sysvaddr == NULL) goto free_sptes;
  } else {
    sts = OZ_NOMEMORY;
    for (i = 0; i < mappages; i ++) {

      /* Allocate a physical page */

      phypage = oz_knl_phymem_allocpage (OZ_PHYMEM_PAGESTATE_ALLOCSECT, sysvpage + i);
      if (phypage == OZ_PHYPAGE_NULL) {
        mappages = i;
        goto free_mappages;
      }

      /* Just write spte on this cpu.  Other cpu's will pagefault (as the pages used to be 'no access') and see */
      /* the new valid descriptor and use it.  This prevents a flurry of unnecessary interprocessor interrupts. */
      /* Set the 'reqprot' to NA so oz_knl_spte_free won't puke on them when we free them off.                  */

      oz_hw_pte_writecur (sysvpage + i, OZ_SECTION_PAGESTATE_VALID_W, phypage, OZ_HW_PAGEPROT_KW, OZ_HW_PAGEPROT_NA);
    }
  }

  /* Now allocate the disk pages and save entries in map array */

  for (i = 0; i < diskpages; i ++) {
    if (diskaddr != 0) phypage = (diskaddr >> OZ_HW_L2PAGESIZE) + i;
    else {
      phypage = oz_knl_phymem_allocpage (OZ_PHYMEM_PAGESTATE_ALLOCSECT, i);
      if (phypage == OZ_PHYPAGE_NULL) {
        diskpages = i;
        goto free_diskpages;
      }
      if (phypage >= oz_s_phymem_totalpages) {	// besides being 'impossible', it would look like we were given 'diskaddr' param
        oz_crash ("oz_dev_ramdisk: got phypage %x .ge. oz_s_phymem_totalpages %x", phypage, oz_s_phymem_totalpages);
      }
    }
    ((OZ_Mempage *)sysvaddr)[i] = phypage;
  }

  /* Successful, save values in devex, marking the device as 'mounted' */

  devex -> map_sysvaddr  = sysvaddr;
  devex -> map_pages     = mappages;
  devex -> diskblocks    = diskblocks;
  devex -> diskpages     = diskpages;
  devex -> volvalid      = 0;
  devex -> quota         = quota;
  devex -> quotapages    = quotapages;
  oz_hw_smplock_clr (&oz_s_smplock_pm, pm);

  /* Set the description to the size, and call the partition/automount autogen routine */

  oz_knl_devunit_rename (devunit, NULL, descr);
  oz_knl_devunit_autogen (devunit, oz_dev_disk_auto, NULL);
  return (OZ_SUCCESS);

  /* Return with error status after cleaning up */

free_diskpages:
  while (diskpages > 0) {
    -- diskpages;
    phypage = ((OZ_Mempage *)sysvaddr)[diskpages];
    oz_knl_phymem_freepage (phypage);
  }
free_mappages:
  if (oz_s_inloader) OZ_KNL_NPPFREE (sysvaddr);
  else while (mappages > 0) {
    -- mappages;
    oz_hw_pte_readsys (OZ_HW_VADDRTOVPAGE (sysvaddr) + i, NULL, &phypage, NULL, NULL);
    oz_knl_phymem_freepage (phypage);
  }
free_sptes:
  oz_hw_smplock_clr (&oz_s_smplock_pm, pm);
  oz_knl_spte_free (mappages, OZ_HW_VADDRTOVPAGE (sysvaddr));
cred_quota:
  if (quota != NULL) oz_knl_quota_credit (quota, OZ_QUOTATYPE_PHM, quotapages, -1);
  return (sts);
}

static uLong atomemsize (const char *string, uLong *usedup_r)

{
  uLong memsize, usedup;

  memsize = oz_hw_atoi (string, &usedup);
  if ((string[usedup] == 'K') || (string[usedup] == 'k')) {
    memsize <<= 10;
    usedup ++;
  }
  if ((string[usedup] == 'M') || (string[usedup] == 'm')) {
    memsize <<= 20;
    usedup ++;
  }
  if ((string[usedup] == 'G') || (string[usedup] == 'g')) {
    memsize <<= 30;
    usedup ++;
  }
  *usedup_r = usedup;
  return (memsize);
}

/************************************************************************/
/*									*/
/*  Write and Read logical blocks on the volume				*/
/*									*/
/************************************************************************/

static uLong writeblocks (uLong size, const void *buff, OZ_Dbn slbn, Devex *devex)

{
  uLong diskaddrstart, pageindex, pageoffset, pagesize;
  OZ_Dbn elbn;

  if (size != 0) {
    elbn = slbn + (size - 1) / DISK_BLOCK_SIZE;
    if (elbn < slbn) return (OZ_BADBLOCKNUMBER);
    if (elbn >= devex -> diskblocks) return (OZ_BADBLOCKNUMBER);

    pagesize = 1 << OZ_HW_L2PAGESIZE;
    diskaddrstart = slbn * DISK_BLOCK_SIZE;
    pageoffset = diskaddrstart & (pagesize - 1);
    pageindex  = diskaddrstart >> OZ_HW_L2PAGESIZE;

    oz_hw_phys_movefromvirt (size, buff, devex -> map_sysvaddr + pageindex, pageoffset);
  }
  return (OZ_SUCCESS);
}

static uLong writepages (uLong size, const OZ_Mempage *pages, uLong offset, OZ_Dbn slbn, Devex *devex)

{
  uLong diskaddrstart, pageindex, pageoffset, pagesize;
  OZ_Dbn elbn;

  if (size != 0) {
    elbn = slbn + (size - 1) / DISK_BLOCK_SIZE;
    if (elbn < slbn) return (OZ_BADBLOCKNUMBER);
    if (elbn >= devex -> diskblocks) return (OZ_BADBLOCKNUMBER);

    pagesize = 1 << OZ_HW_L2PAGESIZE;
    diskaddrstart = slbn * DISK_BLOCK_SIZE;
    pageoffset = diskaddrstart & (pagesize - 1);
    pageindex  = diskaddrstart >> OZ_HW_L2PAGESIZE;

    oz_hw_phys_movephys (size, pages, offset, devex -> map_sysvaddr + pageindex, pageoffset);
  }
  return (OZ_SUCCESS);
}

static uLong readblocks (uLong size, void *buff, OZ_Dbn slbn, Devex *devex)

{
  uLong diskaddrstart, pageindex, pageoffset, pagesize;
  OZ_Dbn elbn;

  if (size != 0) {
    elbn = slbn + (size - 1) / DISK_BLOCK_SIZE;
    if (elbn < slbn) return (OZ_BADBLOCKNUMBER);
    if (elbn >= devex -> diskblocks) return (OZ_BADBLOCKNUMBER);

    pagesize = 1 << OZ_HW_L2PAGESIZE;
    diskaddrstart = slbn * DISK_BLOCK_SIZE;
    pageoffset = diskaddrstart & (pagesize - 1);
    pageindex  = diskaddrstart >> OZ_HW_L2PAGESIZE;

    oz_hw_phys_movetovirt (size, buff, devex -> map_sysvaddr + pageindex, pageoffset);
  }
  return (OZ_SUCCESS);
}

static uLong readpages (uLong size, const OZ_Mempage *pages, uLong offset, OZ_Dbn slbn, Devex *devex)

{
  uLong diskaddrstart, pageindex, pageoffset, pagesize;
  OZ_Dbn elbn;

  if (size != 0) {
    elbn = slbn + (size - 1) / DISK_BLOCK_SIZE;
    if (elbn < slbn) return (OZ_BADBLOCKNUMBER);
    if (elbn >= devex -> diskblocks) return (OZ_BADBLOCKNUMBER);

    pagesize = 1 << OZ_HW_L2PAGESIZE;
    diskaddrstart = slbn * DISK_BLOCK_SIZE;
    pageoffset = diskaddrstart & (pagesize - 1);
    pageindex  = diskaddrstart >> OZ_HW_L2PAGESIZE;

    oz_hw_phys_movephys (size, devex -> map_sysvaddr + pageindex, pageoffset, pages, offset);
  }
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Direct access to ramdisk page					*/
/*									*/
/*    Input:								*/
/*									*/
/*	iochan = I/O channel assigned to ramdisk device			*/
/*	dcmpb -> virtblock = starting virtual block number		*/
/*	dcmpb -> nbytes    = number of bytes requested			*/
/*	dcmpb -> loglbock  = starting logical block number		*/
/*	dcmpb -> blockoffs = starting offset in logical block		*/
/*	dcmpb -> entry     = points to processing routine		*/
/*	dcmpb -> param     = parameter to pass to completion routine	*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_dev_ramdisk_map = OZ_SUCCESS : successful completion		*/
/*	                  OZ_BADDEVUNIT : non-ramdisk I/O channel	*/
/*	              OZ_BADBLOCKNUMBER : invalid logical block number	*/
/*									*/
/************************************************************************/

static uLong ramdisk_map (OZ_Iochan *iochan, OZ_Dcmpb *dcmpb)

{
  Devex *devex;
  OZ_Dbn slbn, elbn;

  /* Point to the ramdisk device's device extension structure and make sure it's one of ours */

  devex = oz_knl_devunit_ex (oz_knl_iochan_getdevunit (iochan));
  if (devex -> magic != ramdisk_magic) return (OZ_BADDEVUNIT);
  if (!(devex -> volvalid)) return (OZ_VOLNOTVALID);

  /* Repeat while there is something to do */

  while (dcmpb -> nbytes > 0) {

    /* Normalise blockoffs to the simulated DISK_BLOCK_SIZE */

    dcmpb -> virtblock += dcmpb -> blockoffs >> L2DISK_BLOCK_SIZE;
    dcmpb -> logblock  += dcmpb -> blockoffs >> L2DISK_BLOCK_SIZE;
    dcmpb -> blockoffs %= DISK_BLOCK_SIZE;

    /* Make sure the last block they want to access is within range */

    slbn = dcmpb -> logblock;
    elbn = slbn + (dcmpb -> nbytes + dcmpb -> blockoffs - 1) / DISK_BLOCK_SIZE;
    if (elbn < slbn) return (OZ_BADBLOCKNUMBER);
    if (elbn >= devex -> diskblocks) return (OZ_BADBLOCKNUMBER);

    /* Point to the physical page and the offset within the page */

    dcmpb -> phypage  = devex -> map_sysvaddr[slbn>>(OZ_HW_L2PAGESIZE-L2DISK_BLOCK_SIZE)];
    dcmpb -> pageoffs = ((slbn % (1 << (OZ_HW_L2PAGESIZE - L2DISK_BLOCK_SIZE))) << L2DISK_BLOCK_SIZE) + dcmpb -> blockoffs;

    /* Chop the number of bytes available off at the end of the page */

    if (dcmpb -> pageoffs + dcmpb -> nbytes > (1 << OZ_HW_L2PAGESIZE)) {
      dcmpb -> nbytes = (1 << OZ_HW_L2PAGESIZE) - dcmpb -> pageoffs;
    }

    /* Call the processing routine.  We don't care if it modifies anything or not. */

    (*(dcmpb -> entry)) (dcmpb, OZ_PENDING);
  }

  /* We always complete synchronously, a priori */

  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Direct access to ramdisk page					*/
/*									*/
/*    Input:								*/
/*									*/
/*	iochan   = I/O channel assigned to ramdisk device		*/
/*	loglbock = starting logical block number			*/
/*	           must be on page boundary				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_dev_ramdisk_pfmap = OZ_SUCCESS : successful completion	*/
/*	                    OZ_BADDEVUNIT : non-ramdisk I/O channel	*/
/*	                OZ_BADBLOCKNUMBER : invalid logical block number
/*	*phypage_r = physical page containing logblock			*/
/*									*/
/************************************************************************/

static uLong ramdisk_pfmap (OZ_Iochan *iochan, OZ_Dbn logblock, OZ_Mempage *phypage_r)

{
  Devex *devex;
  OZ_Dbn slbn, elbn;

  /* Point to the ramdisk device's device extension structure and make sure it's one of ours */

  devex = oz_knl_devunit_ex (oz_knl_iochan_getdevunit (iochan));
  if (devex -> magic != ramdisk_magic) return (OZ_BADDEVUNIT);
  if (!(devex -> volvalid)) return (OZ_VOLNOTVALID);

  /* Make sure the last block they want to access is within range */

  slbn = logblock;
  elbn = slbn + ((1 << OZ_HW_L2PAGESIZE) - 1) / DISK_BLOCK_SIZE;
  if (elbn < slbn) return (OZ_BADBLOCKNUMBER);
  if (elbn >= devex -> diskblocks) return (OZ_BADBLOCKNUMBER);

  /* Make sure they are starting on a page boundary */

  if (((slbn % (1 << (OZ_HW_L2PAGESIZE - L2DISK_BLOCK_SIZE))) << L2DISK_BLOCK_SIZE) != 0) return (OZ_BADBLOCKNUMBER);

  /* Point to the physical page and the offset within the page */

  *phypage_r = devex -> map_sysvaddr[slbn>>(OZ_HW_L2PAGESIZE-L2DISK_BLOCK_SIZE)];
  return (OZ_SUCCESS);
}
