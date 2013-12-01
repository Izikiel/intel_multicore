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
/*  Disk cache processing routines					*/
/*									*/
/*  These routines are called by filesystem drivers rather than 	*/
/*  accessing the disk directly						*/
/*									*/
/*  They store the disk blocks in unused physical memory pages, giving 	*/
/*  them up as needed.							*/
/*									*/
/************************************************************************/

#define _OZ_KNL_DCACHE_C

#include "ozone.h"
#include "oz_dev_timer.h"
#include "oz_io_disk.h"
#include "oz_knl_cache.h"
#include "oz_knl_dcache.h"
#include "oz_knl_devio.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_objtype.h"
#include "oz_knl_phymem.h"
#include "oz_knl_sdata.h"
#include "oz_knl_status.h"
#include "oz_sys_dateconv.h"

#define FLUSH_INTERVAL OZ_TIMER_RESOLUTION
#define INITIAL_WRITERATE 50

#define DISK_BLOCK_SIZE (dcache -> getinfo1.blocksize)
#define BLOCKS_PER_PAGE (OZ_KNL_CACHE_PAGESIZE / DISK_BLOCK_SIZE)

typedef struct Pagex Pagex;

struct OZ_Dcache { OZ_Objtype objtype;			/* OZ_OBJTYPE_DCACHE */
                   OZ_Iochan *iochan;			/* I/O channel to disk drive */
                   OZ_IO_disk_getinfo1 getinfo1;	/* disk information */
                   OZ_Event *event;			/* event flag */
                   OZ_Cache *cache;			/* cache context pointer */
                   volatile Long rdniopend;		/* number of read I/O's that are in progress */
                   OZ_Cachepage *dirty_qh;		/* dirty pages list */
                   OZ_Cachepage **dirty_qt;
                   volatile Long ndirties;		/* - number of pages on dirties list */
                   volatile Long wtniopend;		/* number of write I/O's we're waiting for */
                   OZ_IO_disk_writepages dwppb;		/* disk write pages param block */
                   OZ_Timer *volatile flush_timer;	/* flush interval timer */
                   OZ_Timer *spread_timer;		/* spread interval timer */
                   OZ_Datebin spread_nextwrite;		/* when to do the next write */
                   OZ_Datebin spread_interval;		/* current spread interval */
                   int spread_collision;		/* spread writing in prog when flush_timer went off */
                   volatile int terminate;		/* terminate flag */
                   uLong (*reval_entry) (void *reval_param, OZ_Dcache *dcache); /* volume re-validation routine entrypoint */
                   void *reval_param;			/* volume re-validation routine parameter */
                   uLong totalnumwrites;		/* total number of writes performed */
                   OZ_Datebin totalwritetime;		/* total time spent writing */
                   uLong avgwriterate;			/* average number of writes per second we can do */
                   OZ_Smplock smplock_vl;		/* dirty pages list lock */
                 };

struct Pagex { OZ_Dcache *dcache;			/* pointer to dcache */
               OZ_Cachepage *next_dirty;		/* next in the dcache -> dirties list */
               OZ_Mempage phypage;			/* corresponding physical page number */
               volatile Long statevalid;		/* see macros/routines below */
             };

static uLong read_page (OZ_Dcache *dcache, Pagex *pagex, OZ_Cachekey key, OZ_Mempage phypage);
static uLong read_page_async (OZ_Dcache *dcache, Pagex *pagex, OZ_Cachekey key, 
                              int ix4kbuk, void (*entry) (void *param, uLong status), void *param);
static void mark_page_dirty (OZ_Dcache *dcache, OZ_Cachepage *page, Pagex *pagex, uLong written);
static void decrdniopend (OZ_Dcache *dcache);
static void flush_timer_expired (void *dcachev, OZ_Timer *flush_timer);
static void startwriting (OZ_Dcache *dcache);
static void start_write (void *pagev, OZ_Timer *spread_timer);
static void write_done (void *dcachev, uLong status);
static void putondirtiesq (OZ_Cachepage *page, OZ_Dcache *dcache);
static void restart_flush_timer (OZ_Dcache *dcache);
static int memfull (void *dcachev);

/* Manipulate the statevalid variable */
/* Bits <00:01> are the page's state: 0=clean and not being written; 1=dirty, not being written; 2=clean, being written; 3=dirty, being written */
/* Bits <02:31> are the number of valid longs (always a multiple of the blocksize) */

#define GETSTATE(__pagex) ((__pagex) -> statevalid & 3)
#define SETSTATE(__pagex,__state) setstate (__pagex, __state)
#define GETVALID(__pagex) ((__pagex) -> statevalid & 0xFFFFFFFC)
#define SETVALID(__pagex,__valid) setvalid (__pagex, __valid)

static void setstate (Pagex *pagex, uByte state)

{
  uLong newstatevalid, oldstatevalid;

  if (state > 3) oz_crash ("oz_knl_dcache: invalid state %X", state);

  do {
    oldstatevalid = pagex -> statevalid;
    newstatevalid = (oldstatevalid & 0xFFFFFFFC) | state;
  } while (!oz_hw_atomic_setif_long (&(pagex -> statevalid), newstatevalid, oldstatevalid));
}

static void setvalid (Pagex *pagex, uLong valid)

{
  uLong newstatevalid, oldstatevalid;

  if (valid & 3) oz_crash ("oz_knl_dcache setvalid: invalid 'valid' of %X", valid);

  do {
    oldstatevalid = pagex -> statevalid;
    newstatevalid = (oldstatevalid & 3) | valid;
  } while (!oz_hw_atomic_setif_long (&(pagex -> statevalid), newstatevalid, oldstatevalid));
}

/************************************************************************/
/*									*/
/*  Initiate cache processing for a disk				*/
/*									*/
/*    Input:								*/
/*									*/
/*	iochan      = I/O channel assigned to disk			*/
/*	blocksize   = disk's block size					*/
/*	reval_entry = routine to call if media changed			*/
/*	reval_param = parameter to pass to reval_entry			*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_dcache_init = pointer to dcache struct			*/
/*									*/
/************************************************************************/

OZ_Dcache *oz_knl_dcache_init (OZ_Iochan *iochan, uLong blocksize, uLong (*reval_entry) (void *reval_param, OZ_Dcache *dcache), void *reval_param)

{
  OZ_Datebin when;
  OZ_Dcache *dcache;
  uLong sts;

  /* Allocate and initialise object */

  oz_knl_iochan_increfc (iochan, 1);

  dcache = OZ_KNL_NPPMALLOC (sizeof *dcache);
  memset (dcache, 0, sizeof *dcache);
  dcache -> objtype        = OZ_OBJTYPE_DCACHE;
  dcache -> iochan         = iochan;
  dcache -> dirty_qt       = &(dcache -> dirty_qh);
  dcache -> reval_entry    = reval_entry;
  dcache -> reval_param    = reval_param;
  dcache -> cache          = oz_knl_cache_init (oz_knl_devunit_devname (oz_knl_iochan_getdevunit (iochan)), 
                                                sizeof (Pagex), 
                                                memfull, 
                                                dcache);
  dcache -> avgwriterate   = INITIAL_WRITERATE;

  sts = oz_knl_event_create (6, "dcache", NULL, &(dcache -> event));
  if (sts != OZ_SUCCESS) oz_crash ("oz_knl_dcache_init: error %u creating event flag", sts);
  oz_hw_smplock_init (sizeof dcache -> smplock_vl, &(dcache -> smplock_vl), OZ_SMPLOCK_LEVEL_VL);

  memset (&(dcache -> getinfo1), 0, sizeof dcache -> getinfo1);
  sts = oz_knl_io (dcache -> iochan, OZ_IO_DISK_GETINFO1, sizeof dcache -> getinfo1, &(dcache -> getinfo1));
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_knl_dcache_init: %s does not support OZ IO DISK GETINFO1, status %u\n", 
                   oz_knl_devunit_devname (oz_knl_iochan_getdevunit (iochan)), sts);
    memset (&(dcache -> getinfo1), 0, sizeof dcache -> getinfo1);
    dcache -> getinfo1.blocksize = blocksize;
  }

  /* If not a ramdisk, then start the flush timer going.  Also allocate the spread timer. */

  if (dcache -> getinfo1.ramdisk_map == NULL) {
    dcache -> spread_timer = oz_knl_timer_alloc ();
    dcache -> flush_timer  = oz_knl_timer_alloc ();
    restart_flush_timer (dcache);
  }

  /* Return object pointer */

  return (dcache);
}

/************************************************************************/
/*									*/
/*  Terminate cache processing						*/
/*									*/
/*    Input:								*/
/*									*/
/*	dcache = pointer to dcache context				*/
/*	abort  = 0 : flush pending writes and wait for I/O's to 	*/
/*	             complete normally					*/
/*	         1 : abort any I/O's and don't bother writing		*/
/*									*/
/*    Output:								*/
/*									*/
/*	dcache freed off and voided out					*/
/*									*/
/************************************************************************/

void oz_knl_dcache_term (OZ_Dcache *dcache, int abort)

{
  uLong vl;

  OZ_KNL_CHKOBJTYPE (dcache, OZ_OBJTYPE_DCACHE);

  /* Only need to clean up after non-ramdisks */

  if (dcache -> getinfo1.ramdisk_map == NULL) {

    /* Set terminate flag in cache block so the flush_timer_expired routine can see it */
    /* Clear the event flag first, though, so if write_timer sets it, we will not wait */

    oz_knl_event_set (dcache -> event, 0);
    dcache -> terminate = 1;

    /* Try to remove flush_timer entry.  If successful, call the flush_timer_expired */
    /* routine to finish terminating.  If not successful, it is already running      */
    /* somewhere and won't requeue itself because it sees the terminate flag is set. */

waitfortimer:
    vl = oz_hw_smplock_wait (&(dcache -> smplock_vl));				// lock database
    if ((dcache -> flush_timer != NULL) && !oz_knl_timer_remove (dcache -> flush_timer)) { // if timer is queued and we can't remove it, ...
      oz_hw_smplock_clr (&(dcache -> smplock_vl), vl);				// ... release lock
      oz_knl_event_waitone (dcache -> event);					// ... wait for it to finish executing
      oz_knl_event_set (dcache -> event, 0);					// ... in case it hasn't terminated yet
      goto waitfortimer;							// ... then go check again
    }
    oz_hw_smplock_clr (&(dcache -> smplock_vl), vl);				// timer is no longer queued, release lock

    /* Double the write rate for finishing up as we won't have any reads going on */

    dcache -> totalnumwrites  = -1;						// so it won't get recalculated
    dcache -> avgwriterate   *= 2;						// double the write rate
    dcache -> spread_interval = FLUSH_INTERVAL / (dcache -> avgwriterate + 2);	// halve the interval between writes

    /* If caller wants us to abort any I/O's we have going, abort them and wait for them to complete */

    if (abort) {
      oz_knl_ioabort (dcache -> iochan, OZ_PROCMODE_KNL);
      while ((dcache -> wtniopend != 0) || (dcache -> rdniopend != 0)) {
        oz_knl_event_waitone (dcache -> event);
        oz_knl_event_set (dcache -> event, 0);
      }
    }

    /* Shouldn't be any reads going now */

    if (dcache -> rdniopend != 0) oz_crash ("oz_knl_dcache_term: dcache %p -> rdniopend %d", dcache, dcache -> rdniopend);

    /* If not aborting, queue writes for any last dirty pages */

    dcache -> spread_interval = 0;
    if (!abort) startwriting (dcache);

    /* Wait for all writes to complete */

    vl = oz_hw_smplock_wait (&(dcache -> smplock_vl));
    while ((dcache -> wtniopend != 0) || (dcache -> spread_timer == NULL) || (!abort && (dcache -> ndirties != 0))) {		// repeat while writes are in progress
																//           or spread timer is in use
																//           or there are dirty pages
      if (!abort && ((dcache -> spread_timer != NULL) && (dcache -> dirty_qh != NULL))) {					// if not writing and there are dirty pages, start writing
        oz_hw_smplock_clr (&(dcache -> smplock_vl), vl);
        startwriting (dcache);
      }
      else oz_hw_smplock_clr (&(dcache -> smplock_vl), vl);
      oz_knl_event_waitone (dcache -> event);											// wait for the writes to complete
      oz_knl_event_set (dcache -> event, 0);											// in case we have to wait again
      vl = oz_hw_smplock_wait (&(dcache -> smplock_vl));
    }
    oz_hw_smplock_clr (&(dcache -> smplock_vl), vl);
  }

  /* Free everything off */

  oz_knl_iochan_increfc (dcache -> iochan, -1);
  oz_knl_event_increfc  (dcache -> event,  -1);
  if (dcache -> flush_timer  != NULL) oz_knl_timer_free (dcache -> flush_timer);
  if (dcache -> spread_timer != NULL) oz_knl_timer_free (dcache -> spread_timer);
  oz_knl_cache_term (dcache -> cache);
  OZ_KNL_NPPFREE (dcache);
}

/************************************************************************/
/*									*/
/*  Read from the disk/cache into user's buffer, waiting if necessary	*/
/*									*/
/*    Input:								*/
/*									*/
/*	dcache = disk cache						*/
/*	size = size (in bytes) of transfer				*/
/*	buff = where to put the data					*/
/*	slbn = starting lbn of transfer					*/
/*	offs = offset in starting lbn					*/
/*	smp level = softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_dcache_readw = OZ_SUCCESS : completed successfully	*/
/*	                            else : error status			*/
/*									*/
/************************************************************************/

static uLong ramdisk_readw (OZ_Dcmpb *dcmpb, uLong status);

uLong oz_knl_dcache_readw (OZ_Dcache *dcache, uLong size, void *buff, OZ_Dbn slbn, uLong offs)

{
  OZ_Cachekey key;
  OZ_Cachepage *page;
  OZ_Dcmpb dcmpb;
  OZ_Mempage phypage;
  Pagex *pagex;
  uByte *ubuff;
  uLong page_offset, page_size, sts;

  OZ_KNL_CHKOBJTYPE (dcache, OZ_OBJTYPE_DCACHE);

  ubuff = buff;

  /* Handle ramdisk differently.  We just use its pages directly, no need for cache pages. */

  if (dcache -> getinfo1.ramdisk_map != NULL) {
    memset (&dcmpb, 0, sizeof dcmpb);				// set up param block for oz_dev_ramdisk_map routine
    dcmpb.entry = ramdisk_readw;
    dcmpb.param = ubuff;					// start with buffer here
    while (size > 0) {
      dcmpb.nbytes    = size;					// this is how many bytes we have yet to do
      dcmpb.logblock  = slbn;					// start at this logical block number
      dcmpb.blockoffs = offs;					// start at this offset in the logical block
      sts = (*(dcache -> getinfo1.ramdisk_map)) (dcache -> iochan, &dcmpb); // copy what is left in the ramdisk page
      if (sts != OZ_SUCCESS) return (sts);			// abort if ramdisk error
      size -= ((uByte *)dcmpb.param) - ubuff;			// reduce size that we have yet to copy
      offs += ((uByte *)dcmpb.param) - ubuff;			// increment offset in starting logical block
      ubuff = dcmpb.param;					// point to where rest of data goes
    }
  }

  /* Magnetic disk */

  else while (size > 0) {

    /* Make sure 'offs' is offset into block and adjust slbn accordingly */

    slbn += offs / DISK_BLOCK_SIZE;
    offs %= DISK_BLOCK_SIZE;

    /* Figure out how much of that page we want */

    page_offset = (slbn % BLOCKS_PER_PAGE) * DISK_BLOCK_SIZE + offs;			/* get byte offset into the page we will start at */
    page_size   = OZ_KNL_CACHE_PAGESIZE - page_offset;					/* get number of bytes in page starting at that point */
    if (page_size > size) page_size = size;						/* ... but make sure it's no more than caller wants */

    /* Find the page in the cache, create one if it isn't there.  Allow other readers but no other writers. */

    key  = slbn / BLOCKS_PER_PAGE;
    page = oz_knl_cache_find (dcache -> cache, key, OZ_LOCKMODE_PR, (void **)&pagex, &phypage);
    pagex -> dcache  = dcache;
    pagex -> phypage = phypage;

    /* See if valid portion covers what I want.  If not, read in from disk. */

    if (page_offset + page_size > GETVALID (pagex)) {
      oz_knl_cache_conv (dcache -> cache, page, OZ_LOCKMODE_PR, OZ_LOCKMODE_NL);	/* get exclusive access to the page so others can't try reading, too */
      oz_knl_cache_conv (dcache -> cache, page, OZ_LOCKMODE_NL, OZ_LOCKMODE_EX);	/* (cache routines don't support _PR to _EX conversion) */
      if (page_offset + page_size > GETVALID (pagex)) {					/* re-check page valid pointer in case it changed while at _NL */
        sts = read_page (dcache, pagex, key, phypage);					/* read rest of page from disk */
        if (sts != OZ_SUCCESS) {
          oz_knl_cache_done (dcache -> cache, page, OZ_LOCKMODE_EX);			/* read error, release page */
          return (sts);									/* return error status */
        }
      }
      oz_knl_cache_conv (dcache -> cache, page, OZ_LOCKMODE_EX, OZ_LOCKMODE_PR);	/* allow others to read the page now */
    }

    /* Copy data in from cache page */

    oz_hw_phys_movetovirt (page_size, ubuff, &phypage, page_offset);			/* copy from phypage+page_offset to ubuff */
    oz_knl_cache_done (dcache -> cache, page, OZ_LOCKMODE_PR);				/* all done with cache page */
    size  -= page_size;									/* we have this fewer bytes to do */
    ubuff += page_size;									/* and they will go here */
    offs  += page_size;									/* where to start in next block */

    /* Repeat if caller wants some from the next page on disk */
  }

  /* All done */

  return (OZ_SUCCESS);
}

/* This routine copies the data from the ramdisk page to the caller's buffer */

static uLong ramdisk_readw (OZ_Dcmpb *dcmpb, uLong status)

{
  oz_hw_phys_movetovirt (dcmpb -> nbytes, dcmpb -> param, &(dcmpb -> phypage), dcmpb -> pageoffs);
  (OZ_Pointer)(dcmpb -> param) += dcmpb -> nbytes;
  dcmpb -> nbytes = 0;
  return (0);
}

OZ_Cachepage *oz_knl_cache_ixdeb ();

OZ_Dbn oz_knl_dcache_ixdeb (OZ_Dcache *dcache, OZ_Dbn nblocks, OZ_Dbn slbn, OZ_Handle h_output)

{
  OZ_Cachekey key;
  OZ_Cachepage *page;
  OZ_Dcmpb dcmpb;
  OZ_Mempage phypage;
  OZ_Pagentry savepte;
  Pagex *pagex;
  uByte *tempb, *ubuff;
  uLong page_offset, page_size, sts, valid;

  OZ_KNL_CHKOBJTYPE (dcache, OZ_OBJTYPE_DCACHE);

  /* Handle ramdisk differently.  We just use its pages directly, no need for cache pages. */

  if (dcache -> getinfo1.ramdisk_map != NULL) {
    oz_sys_io_fs_printf (h_output, "oz_knl_dcache_ixdeb*: ramdisk\n");
    return (nblocks);
  }

  /* Magnetic disk */

  /* Figure out how much of that page we want */

  page_offset = (slbn % BLOCKS_PER_PAGE) * DISK_BLOCK_SIZE;				/* get byte offset into the page we will start at */
  page_size   = OZ_KNL_CACHE_PAGESIZE - page_offset;					/* get number of bytes in page starting at that point */
  if (page_size / DISK_BLOCK_SIZE > nblocks) page_size = nblocks * DISK_BLOCK_SIZE;
  else nblocks = page_size / DISK_BLOCK_SIZE;

  /* Find the page in the cache */

  key  = slbn / BLOCKS_PER_PAGE;
  page = oz_knl_cache_ixdeb (dcache -> cache, key, OZ_LOCKMODE_PR, (void **)&pagex, &phypage, h_output);
  if (page == NULL) {
    oz_sys_io_fs_printf (h_output, "oz_knl_dcache_ixdeb*: page not in cache\n");
  } else if (page_offset >= (valid = GETVALID (pagex))) {
    oz_sys_io_fs_printf (h_output, "oz_knl_dcache_ixdeb*: valid %u, page_offset %u\n", valid, page_offset);
    oz_knl_cache_done (dcache -> cache, page, OZ_LOCKMODE_PR);
  } else {
    
    if (page_size + page_offset > valid) page_size = valid - page_offset;

    oz_sys_io_fs_printf (h_output, "oz_knl_dcache_ixdeb*: size %u, state %u, phyaddr %X\n", 
	page_size, GETSTATE (pagex), (phypage << OZ_HW_L2PAGESIZE) + page_offset);
    tempb = OZ_KNL_NPPMALLOC (page_size);
    ubuff = oz_hw_phys_mappage (phypage, &savepte);
    memcpy (tempb, ubuff + page_offset, page_size);
    oz_hw_phys_unmappage (savepte);
    oz_sys_io_fs_dumpmem (h_output, page_size, tempb);
    OZ_KNL_NPPFREE (tempb);

    /* All done with cache page */

    oz_knl_cache_done (dcache -> cache, page, OZ_LOCKMODE_PR);
  }
  return (nblocks);
}

/************************************************************************/
/*									*/
/*  Write to the disk/cache from user's buffer, waiting if necessary	*/
/*									*/
/*    Input:								*/
/*									*/
/*	dcache = disk cache						*/
/*	size = size (in bytes) of transfer				*/
/*	buff = where to get the data					*/
/*	slbn = starting lbn of transfer					*/
/*	offs = offset in starting lbn					*/
/*	writethru = if set, write through to magnetic media immediately	*/
/*	smp level = softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_dcache_writew = OZ_SUCCESS : completed successfully	*/
/*	                             else : error status		*/
/*									*/
/************************************************************************/

static uLong ramdisk_writew (OZ_Dcmpb *dcmpb, uLong status);

uLong oz_knl_dcache_writew (OZ_Dcache *dcache, uLong size, const void *buff, OZ_Dbn slbn, uLong offs, int writethru)

{
  const uByte *ubuff;
  int waited;
  OZ_Cachekey key;
  OZ_Cachepage *page;
  OZ_Dcmpb dcmpb;
  OZ_IO_disk_writepages disk_writepages;
  OZ_Mempage phypage;
  Pagex *pagex;
  uLong page_offset, page_size, sts, valid, vl;
  volatile uLong status;

  OZ_KNL_CHKOBJTYPE (dcache, OZ_OBJTYPE_DCACHE);

  ubuff = buff;

  /* Handle ramdisk differently.  We just use its pages directly, no need for cache pages. */

  if (dcache -> getinfo1.ramdisk_map != NULL) {
    memset (&dcmpb, 0, sizeof dcmpb);				// set up param block for ramdisk_map routine
    dcmpb.entry = ramdisk_writew;
    dcmpb.param = (void *)ubuff;				// start with buffer here
    while (size > 0) {
      dcmpb.nbytes    = size;					// this is how many bytes we have yet to do
      dcmpb.logblock  = slbn;					// start at this logical block number
      dcmpb.blockoffs = offs;					// start at this offset in the logical block
      sts = (*(dcache -> getinfo1.ramdisk_map)) (dcache -> iochan, &dcmpb); // copy what is left in the ramdisk page
      if (sts != OZ_SUCCESS) return (sts);			// abort if ramdisk error
      size -= ((const uByte *)dcmpb.param) - ubuff;		// reduce size that we have yet to copy
      offs += ((const uByte *)dcmpb.param) - ubuff;		// increment offset in starting logical block
      ubuff = (const uByte *)dcmpb.param;			// point to where rest of data goes
    }
  }

  /* Magnetic disk */

  else while (size > 0) {

    /* Make sure 'offs' is offset into block and adjust slbn accordingly */

    slbn += offs / DISK_BLOCK_SIZE;
    offs %= DISK_BLOCK_SIZE;

    /* Figure out how much of that page we want */

    page_offset = (slbn % BLOCKS_PER_PAGE) * DISK_BLOCK_SIZE + offs;			/* get byte offset into the page we will start at */
    page_size   = OZ_KNL_CACHE_PAGESIZE - page_offset;					/* get number of bytes in page starting at that point */
    if (page_size > size) page_size = size;						/* ... but make sure it's no more than caller wants */

    /* Find the page in the cache, create one if it isn't there.  Allow no other accessors whilst we are modifying it. */

    key  = slbn / BLOCKS_PER_PAGE;
    page = oz_knl_cache_find (dcache -> cache, key, OZ_LOCKMODE_EX, (void **)&pagex, &phypage);
    pagex -> dcache  = dcache;
    pagex -> phypage = phypage;

    /* If the valid portion doesn't cover what we want, read in from disk */
    /* - we don't need to read if valid covers thru the end of what we're writing */
    /* - we don't need to read if we are starting within valid data and are ending on a block boundary */

    valid = GETVALID (pagex);
    if ((valid < (page_size + page_offset)) && ((page_offset > valid) || (((page_size + page_offset) % DISK_BLOCK_SIZE) != 0))) {
      sts = read_page (dcache, pagex, key, phypage);					/* read rest of page from disk */
      if (sts != OZ_SUCCESS) {
        oz_knl_cache_done (dcache -> cache, page, OZ_LOCKMODE_EX);
        return (sts);
      }
    }

    /* Copy from user buffer to cache page */

    oz_hw_phys_movefromvirt (page_size, ubuff, &phypage, page_offset);	// copy from user buffer to cache page

    size  -= page_size;							// we have this fewer bytes to do
    ubuff += page_size;							// and they will come from here
    offs  += page_size;							// where to start in next block

    /* If write-thru mode, write to disk immediately and release it */

    page_offset += page_size;						// increment past written data

    if (writethru) {
      if (GETVALID (pagex) < page_offset) SETVALID (pagex, page_offset);
      memset (&disk_writepages, 0, sizeof disk_writepages);		// set up write parameters
      disk_writepages.size  = GETVALID (pagex);
      disk_writepages.pages = &phypage;
      disk_writepages.slbn  = (key * BLOCKS_PER_PAGE);
      disk_writepages.writethru = 1;
      OZ_HW_ATOMIC_INCBY1_LONG (dcache -> wtniopend);			// increment number of writes in progress
      status = OZ_PENDING;
      sts = oz_knl_iostart3 (1, NULL, dcache -> iochan, OZ_PROCMODE_KNL, NULL, NULL, &status, dcache -> event, 
                             NULL, NULL, OZ_IO_DISK_WRITEPAGES, sizeof disk_writepages, &disk_writepages);
      if (sts == OZ_STARTED) {						// wait for write to complete
        waited = 0;
        while ((sts = status) == OZ_PENDING) {
          oz_knl_event_waitone (dcache -> event);
          oz_knl_event_set (dcache -> event, 0);
          waited = 1;
        }
        if (waited) oz_knl_event_set (dcache -> event, 1);
      }
      OZ_HW_ATOMIC_DECBY1_LONG (dcache -> wtniopend);			// decrement number of writes in progress
      oz_knl_cache_done (dcache -> cache, page, OZ_LOCKMODE_EX);	// release cache page (we don't have to mark it dirty)
      if (sts != OZ_SUCCESS) return (sts);				// if write error, return error status
    }

    /* Otherwise, put page on disk's 'dirty page' list and release it */

    else mark_page_dirty (dcache, page, pagex, page_offset);		// this also updates the 'valid' pointer

    /* Repeat if caller has some for the next page on disk */
  }

  /* All done */

  return (OZ_SUCCESS);
}

/* This routine copies the data to the ramdisk page from the caller's buffer */

static uLong ramdisk_writew (OZ_Dcmpb *dcmpb, uLong status)

{
  oz_hw_phys_movefromvirt (dcmpb -> nbytes, dcmpb -> param, &(dcmpb -> phypage), dcmpb -> pageoffs);
  (OZ_Pointer)(dcmpb -> param) += dcmpb -> nbytes;
  dcmpb -> nbytes = 0;
  return (0);
}

/************************************************************************/
/*									*/
/*  Map cache page for access and call a routine			*/
/*									*/
/*    Input:								*/
/*									*/
/*	dcmpb = disk cache map parameter block pointer			*/
/*	dcmpb -> dcache    = pointer to disk cache context		*/
/*	dcmpb -> writing   = 0 : completion routine will not modify data
/*	                     1 : completion routine may modify data	*/
/*	dcmpb -> nbytes    = number of bytes requested starting at logblock.blockoffs
/*	dcmpb -> logblock  = starting logical block number		*/
/*	dcmpb -> virtblock = corresponding virtual block number		*/
/*	dcmpb -> blockoffs = byte offset in starting logical block	*/
/*	dcmpb -> entry     = completion routine entrypoint		*/
/*	dcmpb -> param     = completion routine parameter		*/
/*	dcmpb -> writethru = 0 : normal writeback mode			*/
/*	                     1 : data gets immediately written to disk	*/
/*	smp level = softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_dcache_map = OZ_SUCCESS : completed synchronously	*/
/*	                    OZ_STARTED : will complete asynchronously	*/
/*	                          else : error status			*/
/*									*/
/*    Note:								*/
/*									*/
/*	If this routine returns OZ_STARTED status indicating 		*/
/*	asynchronous completion, it will call (*entry) with 		*/
/*	OZ_SUCCESS to indicate when it is all done with dcmpb.		*/
/*									*/
/*  The completion routine is called:					*/
/*									*/
/*	(*entry) (dcmpb, status)					*/
/*									*/
/*    Input:								*/
/*									*/
/*	if (status == OZ_PENDING) {					*/
/*	  dcmpb -> nbytes    = how many bytes are available at phypage.pageoffs
/*	                       (but no more that originally requested)	*/
/*	  dcmpb -> blockoffs = byte offset in logblock, normalized to disk block size
/*	  dcmpb -> virtblock = normalized correspondingly		*/
/*	  dcmpb -> logblock  = normalized	correspondingly		*/
/*	  dcmpb -> phypage   = physical page number of disk cache page	*/
/*	  dcmpb -> pageoffs  = byte offset in phypage for logblock.blockoffs byte
/*	  dcmpb -> cachepage,cachepagex,cachepagelm = internal use	*/
/*	  dcmpb -> everything else = unchanged				*/
/*	} else {							*/
/*	  request is complete (possibly in error)			*/
/*	}								*/
/*	smp level = softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	if (status == OZ_PENDING) {					*/
/*	  (*entry) = 0 : the cache page was not modified		*/
/*	          else : the number of bytes, starting at pageoffs, 	*/
/*	                 that were modified				*/
/*	  dcmpb -> cachepage,cachepagex,cachepagelm = unchanged		*/
/*	  dcmpb -> nbytes = 0 : no new data wanted			*/
/*	                 else : new data wanted, then all else as 	*/
/*	                        specified to oz_knl_dcache_map		*/
/*	} else {							*/
/*	  don't care							*/
/*	}								*/
/*									*/
/************************************************************************/

static void map_readin (void *dcmpbv, uLong status);
static uLong map_process (OZ_Dcmpb *dcmpb);
static void map_writtenthru (void *dcmpbv, uLong status);

uLong oz_knl_dcache_map (OZ_Dcmpb *dcmpb)

{
  OZ_Dcache *dcache;
  OZ_Cachekey key;
  Pagex *pagex;
  uLong page_size, page_offs, sts, valid;

  dcache = dcmpb -> dcache;
  OZ_KNL_CHKOBJTYPE (dcache, OZ_OBJTYPE_DCACHE);

  /* Handle ramdisk differently.  We just use its pages directly, no need for cache pages. */

  if (dcache -> getinfo1.ramdisk_map != NULL) sts = (*(dcache -> getinfo1.ramdisk_map)) (dcache -> iochan, dcmpb);

  /* Magnetic disk */

  else while (dcmpb -> nbytes > 0) {

    /* Normalize blockoffs to DISK_BLOCK_SIZE */

    dcmpb -> logblock  += dcmpb -> blockoffs / DISK_BLOCK_SIZE;
    dcmpb -> virtblock += dcmpb -> blockoffs / DISK_BLOCK_SIZE;
    dcmpb -> blockoffs %= DISK_BLOCK_SIZE;

    /* Figure out how much of the page we can do at once */

    dcmpb -> pageoffs  = (dcmpb -> logblock % BLOCKS_PER_PAGE) * DISK_BLOCK_SIZE;	// get byte offset into the page we will start at
    dcmpb -> pageoffs += dcmpb -> blockoffs;
    page_size = OZ_KNL_CACHE_PAGESIZE - dcmpb -> pageoffs;				// get number of bytes left in page starting at that point
    if (dcmpb -> nbytes > page_size) dcmpb -> nbytes = page_size;			// but no more than caller wants

    /* Find the page in the cache, create one if it isn't there.  If reading, allow */
    /* others to read.  Else, don't allow anyone else access whilst we modify it.   */

    dcmpb -> cachepagelm = OZ_LOCKMODE_PR;
    if (dcmpb -> writing) dcmpb -> cachepagelm = OZ_LOCKMODE_EX;
    key = dcmpb -> logblock / BLOCKS_PER_PAGE;
    dcmpb -> cachepage = oz_knl_cache_find (dcache -> cache, key, dcmpb -> cachepagelm, &(dcmpb -> cachepagex), &(dcmpb -> phypage));
    pagex = dcmpb -> cachepagex;
    pagex -> dcache  = dcache;
    pagex -> phypage = dcmpb -> phypage;

    /* See if valid portion covers what I want.  If so, we're all done. */

    valid = GETVALID (pagex);
    if (valid >= dcmpb -> pageoffs + dcmpb -> nbytes) goto syncompletion;		// if it completely covers requested region, we're good
    if (dcmpb -> writing 								// if it covers up to where we start writing, 
     && (valid >= dcmpb -> pageoffs) 							// and we end right on a block boundary, we're good
     && ((dcmpb -> pageoffs +  dcmpb -> nbytes) % DISK_BLOCK_SIZE == 0)) goto syncompletion;

    /* If reading, convert lock to EX mode whilst we modify the page (ie, read it in from disk) */

    if (!(dcmpb -> writing)) {
      oz_knl_cache_conv (dcache -> cache, dcmpb -> cachepage, OZ_LOCKMODE_PR, OZ_LOCKMODE_NL); // get exclusive access to the page so others can't try reading it in from disk, too
      oz_knl_cache_conv (dcache -> cache, dcmpb -> cachepage, OZ_LOCKMODE_NL, OZ_LOCKMODE_EX); // (cache routines don't support _PR to _EX conversion)
      dcmpb -> cachepagelm = OZ_LOCKMODE_EX;
      if (GETVALID (pagex) >=  dcmpb -> pageoffs + dcmpb -> nbytes) goto syncompletion;	// re-check page valid pointer in case it changed while at _NL
    }

    /* Start reading from disk into cache page */

    sts = read_page_async (dcache, pagex, key, dcmpb -> ix4kbuk, map_readin, dcmpb);	// read rest of page from disk
    if (sts == OZ_SUCCESS) goto syncompletion;						// read completed synchronously
    if (sts != OZ_STARTED) oz_knl_cache_done (dcache -> cache, dcmpb -> cachepage, dcmpb -> cachepagelm); // read error, release page
    return (sts);									// return status (OZ_STARTED or error)

    /* Data is available immediately - call completion routine then release cache block */

syncompletion:
    sts = map_process (dcmpb);								// let requestor process the data
    if (sts != OZ_SUCCESS) return (sts);						// if async or failure, return status
  }

  /* Requestor has indicated there is nothing more to do.  Return successful synchronous completion status. */

  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  The page has been read in from disk.  So we call the requestor's 	*/
/*  processing routine to process it, then act on the return values.	*/
/*									*/
/************************************************************************/

static void map_readin (void *dcmpbv, uLong status)

{
  OZ_Cachekey key;
  OZ_Dcmpb *dcmpb;
  OZ_Mempage mempage;
  uLong mod_size, page_offs;

  dcmpb = dcmpbv;

  if (status == OZ_SUCCESS) {
    status = map_process (dcmpb);					// let requestor process the data
    if (status == OZ_SUCCESS) status = oz_knl_dcache_map (dcmpb);	// attempt to process next request
									// if status is OZ_STARTED, it means a disk read was started and map_done will be called back when the disk read completes
									// if status is OZ_SUCCESS, it means the request is complete and we need to call the entry routine with a zero size
									// any other status indicates some kind of read error, that we will pass back to the entry routine
  }

  if (status != OZ_STARTED) {						// see if oz_knl_dcache_map is all done
    (*(dcmpb -> entry)) (dcmpb, status);				// ok, tell requestor we're done with dcmpb
  }
}

/************************************************************************/
/*									*/
/*  This routine calls the requestor's processing routine to process 	*/
/*  the cache page.  Then it acts on the values returned, which can be 	*/
/*  to do a 'writethru' to the disk, and can be to access another page 	*/
/*  of the disk.							*/
/*									*/
/*    Input:								*/
/*									*/
/*	dcmpb = parameters for next segment of the request		*/
/*	cache page locked						*/
/*									*/
/*    Output:								*/
/*									*/
/*	map_process = OZ_STARTED : will complete asynchronously		*/
/*	              OZ_SUCCESS : successful synchronous completion	*/
/*	                    else : synchronous completion with error	*/
/*	cache page released						*/
/*									*/
/************************************************************************/

static uLong map_process (OZ_Dcmpb *dcmpb)

{
  OZ_Cachekey key;
  OZ_Dcache *dcache;
  OZ_IO_disk_writepages disk_writepages;
  OZ_Mempage phypage;
  Pagex *pagex;
  uLong mod_offs, page_offs, sts;

  sts = OZ_SUCCESS;

  dcache = dcmpb -> dcache;
  pagex  = dcmpb -> cachepagex;

  page_offs = dcmpb -> pageoffs;							// save in case processing routine modifies them
  key       = dcmpb -> logblock / BLOCKS_PER_PAGE;
  phypage   = dcmpb -> phypage;

  mod_offs = (*(dcmpb -> entry)) (dcmpb, OZ_PENDING);					// let requestor process the data

  if (mod_offs == 0) {									// see if requestor modified any data
    oz_knl_cache_done (dcache -> cache, dcmpb -> cachepage, dcmpb -> cachepagelm);	// if not, just release the cache page
  } else if (!(dcmpb -> writethru)) {							// ok, see if writethru mode
    mark_page_dirty (dcache, dcmpb -> cachepage, pagex, page_offs + mod_offs);		// writeback mode, mark page dirty 
											// ... and queue for writing 'whenever'
  } else {
    mod_offs += page_offs;								// set new valid size
    if (GETVALID (pagex) < mod_offs) SETVALID (pagex, mod_offs);
    dcmpb -> phypage = phypage;								// restore physical page number
    memset (&disk_writepages, 0, sizeof disk_writepages);				// set up write parameters
    disk_writepages.size  = GETVALID (pagex);
    disk_writepages.pages = &(dcmpb -> phypage);
    disk_writepages.slbn  = key * BLOCKS_PER_PAGE;
    disk_writepages.writethru = 1;
    OZ_HW_ATOMIC_INCBY1_LONG (dcache -> wtniopend);					// increment number of writes in progress
    sts = oz_knl_iostart3 (1, NULL, dcache -> iochan, OZ_PROCMODE_KNL, map_writtenthru, dcmpb, NULL, NULL, 
                               NULL, NULL, OZ_IO_DISK_WRITEPAGES, sizeof disk_writepages, &disk_writepages);
    if (sts != OZ_STARTED) {
      OZ_HW_ATOMIC_DECBY1_LONG (dcache -> wtniopend);					// sync completion, dec number of writes
      oz_knl_cache_done (dcache -> cache, dcmpb -> cachepage, dcmpb -> cachepagelm);	// ... and release cache page
    }
  }
  return (sts);
}

/************************************************************************/
/*									*/
/*  The page has been written back out to disk.  If successful, start 	*/
/*  processing the next segment of the request as indicated by the 	*/
/*  values left in dcmpb.						*/
/*									*/
/************************************************************************/

static void map_writtenthru (void *dcmpbv, uLong status)

{
  OZ_Dcache *dcache;
  OZ_Dcmpb *dcmpb;

  dcmpb = dcmpbv;
  dcache = dcmpb -> dcache;

  OZ_HW_ATOMIC_DECBY1_LONG (dcache -> wtniopend);					// dec number of pending writes
  oz_knl_cache_done (dcache -> cache, dcmpb -> cachepage, dcmpb -> cachepagelm);	// release the old cache page

  if (status == OZ_SUCCESS) {								// see if the write was successful
    status = oz_knl_dcache_map (dcmpb);							// if so, attempt to process next segment
  }

  if (status != OZ_STARTED) {								// see if all done
    (*(dcmpb -> entry)) (dcmpb, status);						// ok, tell requestor we're done with dcmpb
  }
}

/************************************************************************/
/*									*/
/*  Start prefetch of block into cache					*/
/*									*/
/*    Input:								*/
/*									*/
/*	dcache    = pointer to disk cache context			*/
/*	logblock  = disk block (page) to be prefetched			*/
/*	smp level = softint						*/
/*									*/
/************************************************************************/

static void prefetch_readin (void *pagev, uLong status);

uLong oz_knl_dcache_prefetch (OZ_Dcache *dcache, OZ_Dbn logblock, int ix4kbuk)

{
  OZ_Cachekey key;
  OZ_Cachepage *page;
  OZ_Lockmode lockmode;
  OZ_Mempage phypage;
  Pagex *pagex;
  uLong sts;

  OZ_KNL_CHKOBJTYPE (dcache, OZ_OBJTYPE_DCACHE);

  /* If ramdisk, do nothing (it's all in memory a priori) */

  if (dcache -> getinfo1.ramdisk_map != NULL) return;

  /* Find the page in the cache, create one if it isn't there.  Don't block anyone else from reading or writing it. */

  key  = logblock / BLOCKS_PER_PAGE;
  page = oz_knl_cache_find (dcache -> cache, key, OZ_LOCKMODE_NL, (void **)&pagex, &phypage);
  pagex -> dcache  = dcache;
  pagex -> phypage = phypage;
  lockmode = OZ_LOCKMODE_NL;

  /* See if valid portion covers the whole page */

  if (GETVALID (pagex) < OZ_KNL_CACHE_PAGESIZE) {

    /* Convert lock to EX mode whilst we modify the page (ie, read it in from disk) */

    oz_knl_cache_conv (dcache -> cache, page, OZ_LOCKMODE_NL, OZ_LOCKMODE_EX);
    lockmode = OZ_LOCKMODE_EX;
    if (GETVALID (pagex) < OZ_KNL_CACHE_PAGESIZE) {

      /* Start reading from disk into cache page */

      sts = read_page_async (dcache, pagex, key, ix4kbuk, prefetch_readin, page);
      if (sts == OZ_STARTED) return;
    }
  }

  /* Page is in, release it, hopefully it will still be there when caller wants it */

  oz_knl_cache_done (dcache -> cache, page, lockmode);
}

/************************************************************************/
/*									*/
/*  The page has been read in from disk.  So we just release it.	*/
/*									*/
/************************************************************************/

static void prefetch_readin (void *pagev, uLong status)

{
  Pagex *pagex;

  pagex = oz_knl_cache_pagex (pagev);
  oz_knl_cache_done (pagex -> dcache -> cache, pagev, OZ_LOCKMODE_EX);
}

/************************************************************************/
/*									*/
/*  Map cache page for direct access					*/
/*									*/
/*    Input:								*/
/*									*/
/*	dcache    = pointer to disk cache context			*/
/*	logblock  = starting logical block number			*/
/*	            must be on page boundary				*/
/*	smp level = softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_dcache_pfmap = OZ_SUCCESS : completed			*/
/*	                            else : error status			*/
/*	*phypage_r = physical page containing the block			*/
/*									*/
/*    Note:								*/
/*									*/
/*	Caller must call oz_knl_dcache_pfrel when done with page	*/
/*									*/
/************************************************************************/

uLong oz_knl_dcache_pfmap (OZ_Dcache *dcache, OZ_Dbn logblock, OZ_Mempage *phypage_r)

{
  OZ_Cachekey key;
  OZ_Cachepage *page;
  OZ_Mempage phypage;
  Pagex *pagex;
  uLong sts, valid;

  OZ_KNL_CHKOBJTYPE (dcache, OZ_OBJTYPE_DCACHE);

  if ((logblock % BLOCKS_PER_PAGE) != 0) return (OZ_BADBLOCKNUMBER);

  /* Handle ramdisk differently.  We just use its pages directly, no need for cache pages. */

  if (dcache -> getinfo1.ramdisk_pfmap != NULL) {
    return ((*(dcache -> getinfo1.ramdisk_pfmap)) (dcache -> iochan, logblock, phypage_r));
  }

  /* Magnetic disk - find the page in the cache, create one if it isn't there */

  key  = logblock / BLOCKS_PER_PAGE;
  page = oz_knl_cache_find (dcache -> cache, key, OZ_LOCKMODE_NL, (void **)&pagex, &phypage);
  if (phypage >= oz_s_phymem_totalpages) oz_crash ("oz_knl_dcache_pfmap: bad phypage %u", phypage);
  if (page != &(oz_s_phymem_pages[phypage].u.c)) oz_crash ("oz_knl_dcache_pfmap: breaks assumption make by pfupd and pfrel");
  pagex -> dcache  = dcache;
  pagex -> phypage = phypage;

  /* We only do whole pages, so make sure it is all read in from disk */

  if (GETVALID (pagex) < OZ_KNL_CACHE_PAGESIZE) {
    oz_knl_cache_conv (dcache -> cache, page, OZ_LOCKMODE_NL, OZ_LOCKMODE_EX);	// get exclusive access to the page so others can't try reading it in from disk, too
    if (GETVALID (pagex) <  OZ_KNL_CACHE_PAGESIZE) {				// re-check page valid pointer in case it changed while at _NL
      sts = read_page (dcache, pagex, key, phypage);				// read rest of page from disk
      if (sts != OZ_SUCCESS) return (sts);					// read failed
    }
    oz_knl_cache_conv (dcache -> cache, page, OZ_LOCKMODE_EX, OZ_LOCKMODE_NL);	// allow any access by others
  }

  *phypage_r = phypage;
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Apply updates made to page acquired via oz_knl_dcache_pfmap		*/
/*									*/
/************************************************************************/

uLong oz_knl_dcache_pfupd (OZ_Dcache *dcache, OZ_Dbn logblock, OZ_Mempage phypage, int writethru)

{
  OZ_Cachepage *page;
  OZ_IO_disk_writepages disk_writepages;
  Pagex *pagex;
  uLong sts;
  volatile uLong status;

  OZ_KNL_CHKOBJTYPE (dcache, OZ_OBJTYPE_DCACHE);

  if (dcache -> getinfo1.ramdisk_pfmap != NULL) return (OZ_SUCCESS);			// ramdisk is a nop because the cache 
											// ... page IS the disk media

  if (phypage >= oz_s_phymem_totalpages) oz_crash ("oz_knl_dcache_pfupd: bad phypage %u", phypage);
  page  = &(oz_s_phymem_pages[phypage].u.c);
  pagex = (Pagex *)(page -> pagex);
  if (GETVALID (pagex) != OZ_KNL_CACHE_PAGESIZE) oz_crash ("oz_knl_dcache_pfupd: valid size not a whole page");

  if (!writethru) {									// ok, see if writethru mode
    mark_page_dirty (dcache, page, pagex, OZ_KNL_CACHE_PAGESIZE);			// writeback mode, mark page dirty 
    sts = OZ_SUCCESS;									// ... and queue for writing 'whenever'
  } else {
    memset (&disk_writepages, 0, sizeof disk_writepages);				// set up write parameters
    disk_writepages.size  = OZ_KNL_CACHE_PAGESIZE;
    disk_writepages.pages = &phypage;
    disk_writepages.slbn  = logblock;
    disk_writepages.writethru = 1;
    OZ_HW_ATOMIC_INCBY1_LONG (dcache -> wtniopend);					// increment number of writes in progress
    status = OZ_PENDING;								// start writing page to disk
    sts = oz_knl_iostart3 (1, NULL, dcache -> iochan, OZ_PROCMODE_KNL, NULL, NULL, &status, dcache -> event, 
                           NULL, NULL, OZ_IO_DISK_WRITEPAGES, sizeof disk_writepages, &disk_writepages);
    if (sts == OZ_STARTED) {
      while ((sts = status) == OZ_PENDING) {
        oz_knl_event_waitone (dcache -> event);						// wait for write to complete
        oz_knl_event_set (dcache -> event, 0);
      }
      oz_knl_event_set (dcache -> event, 1);
    }
    OZ_HW_ATOMIC_DECBY1_LONG (dcache -> wtniopend);					// decrement number of writes in progress
  }
  return (sts);
}

/************************************************************************/
/*									*/
/*  Release page to cache acquired via oz_knl_dcache_pfmap		*/
/*									*/
/************************************************************************/

void oz_knl_dcache_pfrel (OZ_Dcache *dcache, OZ_Mempage phypage)

{
  OZ_KNL_CHKOBJTYPE (dcache, OZ_OBJTYPE_DCACHE);

  if (dcache -> getinfo1.ramdisk_pfmap == NULL) {
    if (phypage >= oz_s_phymem_totalpages) oz_crash ("oz_knl_dcache_pfrel: bad phypage %u", phypage);
    oz_knl_cache_done (dcache -> cache, &(oz_s_phymem_pages[phypage].u.c), OZ_LOCKMODE_NL);
  }
}

/************************************************************************/
/*									*/
/*  Read a page into cache from the disk				*/
/*									*/
/*    Input:								*/
/*									*/
/*	dcache   = dcache pointer					*/
/*	pagex    = page extension area pointer				*/
/*	key      = (lbn / BLOCKS_PER_PAGE)				*/
/*	phypage  = physical page to read into				*/
/*	smplevel = softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	read_page = OZ_SUCCESS : successful				*/
/*	                  else : error status				*/
/*	GETVALID (pagex) = the whole page				*/
/*									*/
/************************************************************************/

static uLong read_page (OZ_Dcache *dcache, Pagex *pagex, OZ_Cachekey key, OZ_Mempage phypage)

{
  int waited;
  OZ_IO_disk_readpages disk_readpages;
  uLong sts, valid, vl;
  volatile uLong status;

  valid = GETVALID (pagex);
  if ((valid % DISK_BLOCK_SIZE) != 0) {
    oz_crash ("oz_knl_dcache read_page: pagex -> valid %u not on %u block boundary", valid, DISK_BLOCK_SIZE);
  }

  memset (&disk_readpages, 0, sizeof disk_readpages);			/* start reading the page following what's already valid */
  disk_readpages.size   = OZ_KNL_CACHE_PAGESIZE - valid;		/* ... read the rest of the page after what's already valid */
  disk_readpages.pages  = &phypage;					/* ... just one physical page number */
  disk_readpages.offset = valid;					/* ... starting at this offset in the memory page */
  disk_readpages.slbn   = (valid / DISK_BLOCK_SIZE) + (key * BLOCKS_PER_PAGE); /* ... start reading here on the disk */

  OZ_HW_ATOMIC_INCBY1_LONG (dcache -> rdniopend);
reread:
  status = OZ_PENDING;
  sts = oz_knl_iostart3 (1, NULL, dcache -> iochan, OZ_PROCMODE_KNL, NULL, NULL, &status, dcache -> event, 
                         NULL, NULL, OZ_IO_DISK_READPAGES, sizeof disk_readpages, &disk_readpages);
  if (sts == OZ_STARTED) {						/* see if read completed synchronously */
    waited = 0;
    while ((sts = status) == OZ_PENDING) {				/* if not, see if completed asynchronously */
      oz_knl_event_waitone (dcache -> event);				/* neither, wait for async completion */
      oz_knl_event_set (dcache -> event, 0);				/* clear flag in case we have to wait again */
      waited = 1;							/* remember we cleared it */
    }
    if (waited) oz_knl_event_set (dcache -> event, 1);			/* set in case someone else was waiting */
  }
  if ((sts == OZ_VOLNOTVALID) && (dcache -> reval_entry != NULL)) {
    sts = (*(dcache -> reval_entry)) (dcache -> reval_param, dcache);	/* volume is not valid, try to turn it back online */
    if (sts == OZ_SUCCESS) goto reread;					/* if successful, re-try the read */
  }
  if (sts == OZ_SUCCESS) SETVALID (pagex, OZ_KNL_CACHE_PAGESIZE);	/* if successful, the whole page is valid */
  decrdniopend (dcache);						/* disk might be idle, maybe start writing a dirty page */
  return (sts);
}

/************************************************************************/
/*									*/
/*  Read a page into cache from the disk with async completion		*/
/*									*/
/*    Input:								*/
/*									*/
/*	dcache    = dcache pointer					*/
/*	pagex     = page extension area pointer				*/
/*	key       = (lbn / BLOCKS_PER_PAGE)				*/
/*	entry     = completion routine					*/
/*	param     = completion routine parameter			*/
/*	smplevel  = softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	read_page_async = OZ_SUCCESS : successful synchronous completion
/*	                  OZ_STARTED : read started, will complete asyncly
/*	                        else : error status			*/
/*	GETVALID (pagex) = the whole page				*/
/*									*/
/************************************************************************/

typedef struct Rpap Rpap;

struct Rpap { Pagex *pagex;
              OZ_Dcache *dcache;
              void (*entry) (void *param, uLong status);
              void *param;
            };

static void read_async_done (void *rpapv, uLong status);

static uLong read_page_async (OZ_Dcache *dcache, Pagex *pagex, OZ_Cachekey key, 
                              int ix4kbuk, void (*entry) (void *param, uLong status), void *param)

{
  int waited;
  OZ_IO_disk_readpages disk_readpages;
  Rpap *rpap;
  uLong sts, valid, vl;
  volatile uLong status;

  valid = GETVALID (pagex);
  if ((valid % DISK_BLOCK_SIZE) != 0) {
    oz_crash ("oz_knl_dcache read_page: pagex -> valid %u not on %u block boundary", valid, DISK_BLOCK_SIZE);
  }

  rpap = OZ_KNL_NPPMALLOQ (sizeof *rpap);
  if (rpap == NULL) return (OZ_EXQUOTANPP);

  OZ_HW_ATOMIC_INCBY1_LONG (dcache -> rdniopend);

  rpap -> pagex  = pagex;
  rpap -> dcache = dcache;
  rpap -> entry  = entry;
  rpap -> param  = param;

  memset (&disk_readpages, 0, sizeof disk_readpages);			/* start reading the page following what's already valid */
  disk_readpages.size    = OZ_KNL_CACHE_PAGESIZE - valid;		/* ... read the rest of the page after what's already valid */
  disk_readpages.pages   = &(pagex -> phypage);				/* ... just one physical page number */
  disk_readpages.offset  = valid;					/* ... starting at this offset in the memory page */
  disk_readpages.slbn    = (valid / DISK_BLOCK_SIZE) + (key * BLOCKS_PER_PAGE); /* ... start reading here on the disk */
  disk_readpages.ix4kbuk = ix4kbuk;
  sts = oz_knl_iostart3 (1, NULL, dcache -> iochan, OZ_PROCMODE_KNL, read_async_done, rpap, NULL, NULL, NULL, NULL, 
                         OZ_IO_DISK_READPAGES, sizeof disk_readpages, &disk_readpages);
  if (sts != OZ_STARTED) {
    if (sts == OZ_SUCCESS) {
      SETVALID (pagex, OZ_KNL_CACHE_PAGESIZE);				/* if successful, the whole page is valid */
      if (ix4kbuk) ix4kbuk_validate_phypage (&(pagex -> phypage), 0, __FILE__, __LINE__);
    }
    OZ_KNL_NPPFREE (rpap);						/* if sync completion, free async param block */
    decrdniopend (dcache);						/* maybe disk is idle now */
  }
  return (sts);
}

static void read_async_done (void *rpapv, uLong status)

{
  Rpap *rpap;

  rpap = rpapv;

  if (status == OZ_SUCCESS) SETVALID (rpap -> pagex, OZ_KNL_CACHE_PAGESIZE);	/* if successful, the whole page is valid */
  (*(rpap -> entry)) (rpap -> param, status);					/* now call completion routine */
  decrdniopend (rpap -> dcache);						/* maybe disk is idle now */
  OZ_KNL_NPPFREE (rpap);							/* free off temp async param block */
}

/************************************************************************/
/*									*/
/*  Return cache statistics						*/
/*									*/
/************************************************************************/

void oz_knl_dcache_stats (OZ_Dcache *dcache, uLong *nincache_r, uLong *ndirties_r, OZ_Datebin *dirty_interval_r, uLong *avgwriterate_r)

{
  OZ_KNL_CHKOBJTYPE (dcache, OZ_OBJTYPE_DCACHE);
  oz_knl_cache_stats (dcache -> cache, nincache_r);
  *ndirties_r       = dcache -> ndirties;
  *dirty_interval_r = dcache -> spread_interval;
  *avgwriterate_r   = dcache -> avgwriterate;
}

/************************************************************************/
/*									*/
/*  Increase valid pointer, mark page dirty and release it		*/
/*									*/
/************************************************************************/

static void mark_page_dirty (OZ_Dcache *dcache, OZ_Cachepage *page, Pagex *pagex, uLong written)

{
  uLong vl;

  if (written > OZ_KNL_CACHE_PAGESIZE) {
    oz_crash ("oz_knl_dcache mark_page_dirty: written %u larger than a %u byte page", written, OZ_KNL_CACHE_PAGESIZE);
  }
  if (GETVALID (pagex) < written) SETVALID (pagex, written);

  vl = oz_hw_smplock_wait (&(dcache -> smplock_vl));				/* lock dirties list */
  if (dcache -> terminate != 0) oz_crash ("oz_knl_dcache mark_page_dirty: terminated %d", dcache -> terminate);
  switch (GETSTATE (pagex)) {
    case 0: {									/* see if not on queue nor being written */
      putondirtiesq (page, dcache);						/* ok, queue it to be written */
      oz_hw_smplock_clr (&(dcache -> smplock_vl), vl);
      oz_knl_cache_conv (dcache -> cache, page, OZ_LOCKMODE_EX, OZ_LOCKMODE_NL); /* allow others total access but keep refcount incremented */
      break;
    }
    case 2: {									/* see if currently being written to disk */
      SETSTATE (pagex, 3);							/* if so, say it needs to be re-written */
      oz_hw_smplock_clr (&(dcache -> smplock_vl), vl);
      oz_knl_cache_conv (dcache -> cache, page, OZ_LOCKMODE_EX, OZ_LOCKMODE_NL); /* allow others total access but keep refcount incremented */
      break;
    }
    default: {
      oz_hw_smplock_clr (&(dcache -> smplock_vl), vl);				/* already on queue, don't re-queue it */
      oz_knl_cache_done (dcache -> cache, page, OZ_LOCKMODE_EX);		/* don't keep refcount incremented on the page */
    }										/* (we already have a refcount from when it was put on dirties list) */
  }
}

/************************************************************************/
/*									*/
/*  Decrement the number of reads pending on the disk.  If it is 	*/
/*  becomes zero and there are no writes, start writing a dirty page.	*/
/*									*/
/************************************************************************/

static void decrdniopend (OZ_Dcache *dcache)

{
  OZ_HW_ATOMIC_DECBY1_LONG (dcache -> rdniopend);
}

/************************************************************************/
/*									*/
/*  Flush timer routine - this routine is called every FLUSH_INTERVAL 	*/
/*  to start writing dirty pages out to the disk			*/
/*									*/
/*    Input:								*/
/*									*/
/*	dcache -> dirties = list of pages to write to disk		*/
/*	smp level = softint						*/
/*									*/
/************************************************************************/

static void flush_timer_expired (void *dcachev, OZ_Timer *timer)

{
  OZ_Dcache *dcache;

  dcache = dcachev;
  if (dcache -> terminate) {
    dcache -> flush_timer = NULL;						// timer request is no longer queued
    oz_knl_timer_free (timer);							// free off the timer struct
    oz_knl_event_set (dcache -> event, 1);					// set event in case it's waiting for timer
  } else {
    startwriting (dcache);							// not terminating, start writing top dirty block
    restart_flush_timer (dcache);						// ... and restart flush timer
  }
}

/* Start writing the dirty pages out to disk */

static void startwriting (OZ_Dcache *dcache)

{
  Long ndirties;
  OZ_Cachekey dirty_key;
  OZ_Cachepage *dirties, *dirty, **ldirty, *ndirty;
  OZ_Timer *spread_timer;
  Pagex *pagex;
  uLong n, vl;

  /* Take the spread_timer so in case we are still writing when the next FLUSH_INTERVAL comes along, we won't collide. */
  /* If it is not there, then it is busy so we leave the dirties alone to be done at the next FLUSH_INTERVAL.          */

  do {
    spread_timer = dcache -> spread_timer;						// see if timer in use
    if (spread_timer == NULL) {
      dcache -> spread_collision = 1;							// if so, there's a collision
      return;										// so let it restart when done with current dirties
    }
  } while (!oz_hw_atomic_setif_ptr (&(dcache -> spread_timer), NULL, spread_timer));	// if not, mark the timer in use

  /* Get pages on dirty list.  Get at most 'avgwriterate' as that is now many the disk is capable of doing. */

  dirties = NULL;							// we haven't popped any yet
  vl = oz_hw_smplock_wait (&(dcache -> smplock_vl));			// lock the list
  for (n = 0; n < dcache -> avgwriterate; n ++) {			// pop only as many as we can write
    dirty = dcache -> dirty_qh;						// see who's on top of the list
    if (dirty == NULL) break;
    dirty_key = oz_knl_cache_key (dirty);
    for (ldirty = &dirties; (ndirty = *ldirty) != NULL; ldirty = &(pagex -> next_dirty)) { // find insert spot by key number
      pagex = oz_knl_cache_pagex (ndirty);
      if (oz_knl_cache_key (ndirty) > dirty_key) break;
    }
    pagex = oz_knl_cache_pagex (dirty);					// remove from dcache -> dirty_qh
    if ((dcache -> dirty_qh = pagex -> next_dirty) == NULL) dcache -> dirty_qt = &(dcache -> dirty_qh);
    *ldirty = dirty;							// insert on dirties queue by key number (lbn)
    pagex -> next_dirty = ndirty;
  }
  oz_hw_smplock_clr (&(dcache -> smplock_vl), vl);

  /* Start writing the first one out to disk immediately                                         */
  /* Then spread out the others evenly throughout the coming second to allow reads to take place */

  if (dirties != NULL) {
    dcache -> spread_interval  = FLUSH_INTERVAL / (n + 1);		// spread them evenly throughout the FLUSH_INTERVAL
    dcache -> spread_nextwrite = oz_hw_tod_getnow ();			// start this write right away
    start_write (dirties, spread_timer);				// start writing this page
  }

  /* Nothing found, say spread_timer is no longer in use and wake terminate routine, as we're all done */

  else {
    dcache -> spread_timer = spread_timer;
    if (dcache -> terminate) oz_knl_event_set (dcache -> event, 1);
  }
}

/* Start writing the page to disk, then queue a timer to start the next one in the list */

static void start_write (void *pagev, OZ_Timer *spread_timer)

{
  OZ_Cachepage *next_dirty, *page;
  OZ_Dcache *dcache;
  Pagex *pagex;
  uLong sts, vl;

  page = pagev;

  /* Start writing the page out to disk */

start_it:
  pagex  = oz_knl_cache_pagex (page);					// get page extension area pointer
  dcache = pagex -> dcache;						// get dcache context pointer
  OZ_KNL_CHKOBJTYPE (dcache, OZ_OBJTYPE_DCACHE);
  OZ_KNL_CHKOBJTYPE (dcache -> iochan, OZ_OBJTYPE_IOCHAN);
  next_dirty = pagex -> next_dirty;					// unlink page from remaining dirties
  if (next_dirty == page) oz_crash ("oz_knl_dcache start_write: circular list");
  SETSTATE (pagex, 2);							// set state to indicate a write is in progress

  OZ_HW_ATOMIC_INCBY1_LONG (dcache -> wtniopend);			// increment number of writes in progress
  vl = oz_hw_smplock_wait (&(dcache -> smplock_vl));
  dcache -> totalwritetime -= oz_hw_tod_getnow ();			// start timing the write
  oz_hw_smplock_clr (&(dcache -> smplock_vl), vl);

  dcache -> dwppb.size  = GETVALID (pagex);				// ... write whatever is valid
  dcache -> dwppb.pages = &(pagex -> phypage);				// ... just one physical page number
  dcache -> dwppb.slbn  = oz_knl_cache_key (page) * BLOCKS_PER_PAGE;	// ... start writing here on the disk
  sts = oz_knl_iostart3 (1, NULL, dcache -> iochan, OZ_PROCMODE_KNL, write_done, page, NULL, NULL, 
                         NULL, NULL, OZ_IO_DISK_WRITEPAGES, sizeof dcache -> dwppb, &(dcache -> dwppb));
  if (sts != OZ_STARTED) write_done (page, sts);			// maybe write completed synchronously

  /* If there are any following it, start a timer to write the next one out to disk */

  if (next_dirty != NULL) {
    dcache -> spread_nextwrite += dcache -> spread_interval;		// calc when to do the next write
    if (dcache -> spread_nextwrite <= oz_hw_tod_getnow ()) {		// if we're there already
      page = next_dirty;						// ... don't bother with the timer
      goto start_it;
    }
    oz_knl_timer_insert (spread_timer, dcache -> spread_nextwrite, start_write, next_dirty);
  }

  /* Nothing more to start, let next FLUSH_INTERVAL have the spread_timer */

  else {
    dcache -> spread_timer = spread_timer;				// we are no longer spreading out writes
    if (dcache -> terminate) oz_knl_event_set (dcache -> event, 1);	// maybe termination routine is waiting for us
    if (dcache -> spread_collision) {					// see if flush_timer went off while writing
      dcache -> spread_collision = 0;					// if so, start writing immediately
      startwriting (dcache);
    }
  }
}

/* A write to the disk has completed */

static void write_done (void *pagev, uLong status)

{
  Long wtniopend;
  OZ_Cachepage *page;
  OZ_Dcache *dcache;
  Pagex *pagex;
  uLong newwriterate, vl;

  page   = pagev;
  pagex  = oz_knl_cache_pagex (page);
  dcache = pagex -> dcache;

  /* Check I/O status */

  if (status != OZ_SUCCESS) {
    oz_knl_printk ("oz_knl_dcache: error %u writing to lbn %u of %s\n", status, 
                   oz_knl_cache_key (page) * BLOCKS_PER_PAGE, 
                   oz_knl_devunit_devname (oz_knl_iochan_getdevunit (dcache -> iochan)));
  }

  /* If page got dirty again, re-queue it */

  vl = oz_hw_smplock_wait (&(dcache -> smplock_vl));		/* lock dirties list */
  switch (GETSTATE (pagex)) {
    case 2: {
      pagex -> next_dirty = (void *)0xDEADB0EF;			/* done with it, release it */
      SETSTATE (pagex, 0);
      oz_hw_smplock_clr (&(dcache -> smplock_vl), vl);
      oz_knl_cache_done (dcache -> cache, page, OZ_LOCKMODE_NL);
      vl = oz_hw_smplock_wait (&(dcache -> smplock_vl));
      break;
    }
    case 3: {
      putondirtiesq (page, dcache);				/* put it back on dirties list */
      break;							/* so it will get processed again sometime */
    }
    default: oz_crash ("oz_knl_dcache write_done: bad %p -> state %u", pagex, GETSTATE (pagex));
  }

  /* Decrement number of writes pending */

  wtniopend = oz_hw_atomic_inc_long (&(dcache -> wtniopend), -1);		/* one less write pending */

  /* If all writes are done now, re-calculate average write rate */

  if (dcache -> totalnumwrites < 1000000) {					/* see if write counter overflowed */
    dcache -> totalnumwrites ++;						/* if not, inc total number of writes we've done */
    dcache -> totalwritetime += oz_hw_tod_getnow ();				/* stop timing the write */
    if (wtniopend == 0) {							/* see if all writes done (so totalwritetime is valid) */
      newwriterate = ((OZ_Datebin)(dcache -> totalnumwrites) * FLUSH_INTERVAL / dcache -> totalwritetime) + 1;
      dcache -> avgwriterate = ((dcache -> avgwriterate + newwriterate) / 2) + 1;
    }
  }

  /* Set event flag in case someone is waiting for number of dirty pages < average write rate */

  if (-- (dcache -> ndirties) < dcache -> avgwriterate) oz_knl_event_set (dcache -> event, 1);
  oz_hw_smplock_clr (&(dcache -> smplock_vl), vl);
}

/* Put page on end of dirties queue */

static void putondirtiesq (OZ_Cachepage *page, OZ_Dcache *dcache)

{
  Pagex *pagex;

  pagex = oz_knl_cache_pagex (page);			// point to page extension area
  pagex  -> next_dirty  = NULL;				// it will be the last on the dirty queue
  *(dcache -> dirty_qt) = page;				// link it on the end of the dirty queue
  dcache -> dirty_qt    = &(pagex -> next_dirty);
  SETSTATE (pagex, 1);					// change page's state to indicate it is on dirty page list
  dcache -> ndirties ++;				// one more page on dirty list
}

/* Restart the flush timer */

static void restart_flush_timer (OZ_Dcache *dcache)

{
  OZ_Datebin when;

  when  = oz_hw_tod_getnow ();								// get what time it is now
  when += FLUSH_INTERVAL;								// add the interval to it
  oz_knl_timer_insert (dcache -> flush_timer, when, flush_timer_expired, dcache);	// queue the timer
}

/************************************************************************/
/*									*/
/*  This routine is called by oz_knl_cache_find when it can't get a 	*/
/*  memory page.							*/
/*									*/
/*    Input:								*/
/*									*/
/*	dcachev = disk cache context we're trying to get a page for	*/
/*	smplevel = softint						*/
/*									*/
/************************************************************************/

static int memfull (void *dcachev)

{
  return (0);										// we didn't release anything, until the write completes
}
