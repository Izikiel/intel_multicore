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
/*  Cache processing routines						*/
/*									*/
/************************************************************************/

#define _OZ_KNL_CACHE_C

#include "ozone.h"
#include "oz_knl_cache.h"
#include "oz_knl_hw.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_objtype.h"
#include "oz_knl_phymem.h"
#include "oz_knl_printk.h"
#include "oz_knl_sdata.h"
#include "oz_knl_status.h"

#define HASH_SIZE (256)
#define CEFS_PHYPAGE (1)
#define CEFS_EVENT (2)

#define PAGE_PHYPAGE(__page) ((((OZ_Pointer)(__page)) - (OZ_Pointer)oz_s_phymem_pages) / sizeof oz_s_phymem_pages[0])

struct OZ_Cache { OZ_Objtype objtype;			/* OZ_OBJTYPE_CACHE */
                  uLong exsize;				/* size of pagex area */
                  Long nincache;			/* total number of pages in cache */
                  OZ_Event *event;			/* wait for page lock event */
                  int (*memfullentry) (void *memfullparam);
                  void *memfullparam;
                  OZ_Cachepage *hash_table[HASH_SIZE];	/* list of blocks in the cache, regardless of state */
                  OZ_Smplock smplock_cp;		/* locks cache page hash table, page refcounts, lockcounts */
                  char name[64];			/* what the cache is for */
                };

OZ_Mempage oz_knl_cache_pagecount = 0;		/* number of pages occupied by all the caches */

static int waitingforfreepage  = 0;		/* set when someone is waiting for a free page */

#if OZ_HW_MEMORYCOLORING
static OZ_Cachepage  **oldpages = NULL;		/* list of pages starting with the page accessed the longest time ago */
static OZ_Cachepage ***oldpaget = NULL;		/* ... tail of that list */
#else
static OZ_Cachepage  *oldpages = NULL;
static OZ_Cachepage **oldpaget = &oldpages;
#endif

static OZ_Mempage free_page (OZ_Cachepage *page);
static void cache_validate (int line, OZ_Cache *cache);

/************************************************************************/
/*									*/
/*  Initiate cache processing						*/
/*									*/
/*    Input:								*/
/*									*/
/*	exsize = page extension area size				*/
/*	memfullentry = routine to call if ran out of memory		*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_cache_init = pointer to cache struct			*/
/*									*/
/*    Note:								*/
/*									*/
/*	memfullentry returns 0 if it didn't release anything, 1 if it 	*/
/*	did.								*/
/*									*/
/************************************************************************/

OZ_Cache *oz_knl_cache_init (const char *name, uLong exsize, int (*memfullentry) (void *memfullparam), void *memfullparam)

{
  OZ_Cache *cache;
  OZ_Cachepage *page, **ppage, ***pppage;
  OZ_Mempage i;
  uLong sts;

  if (exsize > sizeof oz_s_phymem_pages[0].u.c.pagex) {
    oz_crash ("oz_knl_cache_init: exsize %u too big for existing oz_s_phymem_pages array");
  }

  /* Make sure oldpages/oldpaget arrays are initialised */

#if OZ_HW_MEMORYCOLORING
  if (oldpages == NULL) {							// see if already set up
    ppage = OZ_KNL_NPPMALLOQ (oz_s_phymem_l2pages * sizeof *ppage);		// if not, malloc an array
    memset (ppage, 0, oz_s_phymem_l2pages * sizeof *ppage);			// clear it to all NULL's
    if (!oz_hw_atomic_setif_ptr ((void *volatile *)&oldpages, ppage, NULL)) {	// set array pointer
      OZ_KNL_NPPFREE (ppage);							// free if someone else just did it
    }
  }

  if (oldpaget == NULL) {							// see if already set up
    pppage = OZ_KNL_NPPMALLOQ (oz_s_phymem_l2pages * sizeof *pppage);		// if not, malloc an array
    for (i = 0; i < oz_s_phymem_l2pages; i ++) {				// init it to point to corresponding oldpages entry
      pppage[i] = oldpages + i;
    }
    if (!oz_hw_atomic_setif_ptr ((void *volatile *)&oldpaget, pppage, NULL)) {	// set array pointer
      OZ_KNL_NPPFREE (pppage);							// free if someone else just did it
    }
  }
#endif

  /* Allocate and initialise object */

  cache = OZ_KNL_NPPMALLOC (sizeof *cache);
  memset (cache, 0, sizeof *cache);
  cache -> objtype = OZ_OBJTYPE_CACHE;
  strncpyz (cache -> name, name, sizeof cache -> name);
  cache -> exsize  = exsize;
  cache -> memfullentry = memfullentry;
  cache -> memfullparam = memfullparam;
  oz_hw_smplock_init (sizeof cache -> smplock_cp, &(cache -> smplock_cp), OZ_SMPLOCK_LEVEL_CP);
  sts = oz_knl_event_create (strlen (cache -> name), cache -> name, NULL, &(cache -> event));
  if (sts != OZ_SUCCESS) oz_crash ("oz_knl_cache_init: error %u creating event flag", sts);

  /* Return object pointer */

  return (cache);
}

/************************************************************************/
/*									*/
/*  Terminate cache processing						*/
/*									*/
/*    Input:								*/
/*									*/
/*	cache = pointer to cache context				*/
/*									*/
/*    Output:								*/
/*									*/
/*	cache freed off and voided out					*/
/*									*/
/************************************************************************/

void oz_knl_cache_term (OZ_Cache *cache)

{
  int i;
  OZ_Cachepage *page;
  OZ_Mempage phypage;
  uLong pm;

  OZ_KNL_CHKOBJTYPE (cache, OZ_OBJTYPE_CACHE);

  oz_knl_event_increfc (cache -> event, -1);		/* get rid of the event flag */

  pm = oz_hw_smplock_wait (&oz_s_smplock_pm);		/* lock physical memory database */
  for (i = 0; i < HASH_SIZE; i ++) {			/* process each hash table entry */
    while ((page = cache -> hash_table[i]) != NULL) {	/* loop as long as there are pages */
      phypage = free_page (page);			/* release the npp stuff */
      oz_knl_phymem_freepage (phypage);			/* release the physical page */
    }
  }
  oz_hw_smplock_clr (&oz_s_smplock_pm, pm);		/* unlock physical memory database */

  OZ_KNL_NPPFREE (cache);				/* free off memory block */
}

/************************************************************************/
/*									*/
/*  Find a page in the cache, create one if not found			*/
/*									*/
/*    Input:								*/
/*									*/
/*	cache = cache context pointer					*/
/*	key   = key value of page to look for				*/
/*	lockmode = initial lock mode (_NL, _PR or _EX)			*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_cache_find = page context block pointer			*/
/*	*pagex_r   = points to page extension area			*/
/*	*phypage_r = physical page number				*/
/*									*/
/************************************************************************/

OZ_Cachepage *oz_knl_cache_find (OZ_Cache *cache, OZ_Cachekey key, OZ_Lockmode lockmode, void **pagex_r, OZ_Mempage *phypage_r)

{
  int cefs;
  OZ_Cachepage **lpage, *npage, *page;
  OZ_Mempage l2, phypage;
  uLong cp, hash_index, pm;

  hash_index = key % HASH_SIZE;
  cefs = 0;

scancache:
  cp = oz_hw_smplock_wait (&(cache -> smplock_cp));
  if (cp != OZ_SMPLOCK_SOFTINT) oz_crash ("oz_knl_cache findpage: called at smplevel %u (not softint)", cp);
  for (page = cache -> hash_table[hash_index]; page != NULL; page = page -> next_hash) {
    if (page -> key == key) {
      phypage = PAGE_PHYPAGE (page);
      break;
    }
  }

  /* If it wasn't found, allocate a new page.  Clear out the pagex area. */

  if (page == NULL) {
    oz_hw_smplock_clr (&(cache -> smplock_cp), cp);		/* unlock cache hash table */
    pm = oz_hw_smplock_wait (&oz_s_smplock_pm);			/* try to allocate a free physical page */
    phypage = oz_knl_phymem_allocpage (OZ_PHYMEM_PAGESTATE_ALLOCACHE, key);
    oz_hw_smplock_clr (&oz_s_smplock_pm, pm);
    if (phypage == OZ_PHYPAGE_NULL) {
      if (cache -> memfullentry != NULL) {			/* nothing available, maybe some space can be cleared up like this */
        if ((*(cache -> memfullentry)) (cache -> memfullparam)) goto scancache;
      }
      oz_knl_event_waitone (oz_s_freephypagevent);		/* wait for one to become available */
      oz_knl_event_set (oz_s_freephypagevent, 0);		/* clear flag in case we wait again */
      cefs |= CEFS_PHYPAGE;					/* remember we cleared it */
      goto scancache;						/* re-check everything (who knows, someone else could have brought page in) */
    }
    cp = oz_hw_smplock_wait (&(cache -> smplock_cp));		/* got one, lock hash tables again */
    for (page = cache -> hash_table[hash_index]; page != NULL; page = page -> next_hash) if (page -> key == key) break;
    if (page != NULL) {						/* see if someone else linked up the same page */
      oz_hw_smplock_clr (&(cache -> smplock_cp), cp);		/* if so, free off the one we just got */
      pm = oz_hw_smplock_wait (&oz_s_smplock_pm);
      oz_knl_phymem_freepage (phypage);
      oz_hw_smplock_clr (&oz_s_smplock_pm, pm);
      goto scancache;						/* ... then try search again (in case it's gone again) */
    }
    if (phypage >= oz_s_phymem_totalpages) oz_crash ("oz_knl_cache_find: bad phypage %u", phypage);
    page = &(oz_s_phymem_pages[phypage].u.c);			/* point to corresponding page state array entry */
    memset (page, 0, page -> pagex + cache -> exsize - (uByte *)page); /* clear it out */
    page -> key       = key;					/* save its key (disk lbn, etc) */
    page -> cache     = cache;					/* save the cache it belongs to */
    npage = cache -> hash_table[hash_index];			/* link it to the hash table */
    page -> next_hash = npage;					/* ... so other's won't try to read same page */
    page -> prev_hash = cache -> hash_table + hash_index;
    cache -> hash_table[hash_index] = page;			
    if (npage != NULL) npage -> prev_hash = &(page -> next_hash);
    cache -> nincache ++;					/* we are now using one more page */

#if OZ_KNL_CACHE_CHECKSUM
    {
      OZ_Pagentry savepte;
      uLong *vaddr;

      vaddr = oz_hw_phys_mappage (phypage, &savepte);
      memset (vaddr, 0, 1 << OZ_HW_L2PAGESIZE);
      oz_hw_phys_unmappage (savepte);
      page -> loaded = oz_hw_tod_getnow ();
    }
#endif
  }

#if OZ_KNL_CACHE_CHECKSUM
  if (page -> refcount == 0) {
    OZ_Pagentry savepte;
    uLong cksm, i, *vaddr;

    vaddr = oz_hw_phys_mappage (phypage, &savepte);
    cksm = 0;
    for (i = 0; i < (1 << OZ_HW_L2PAGESIZE) / 4; i ++) {
      cksm += *(vaddr ++);
    }
    if (cksm != page -> checksum) {
      oz_crash ("oz_knl_cache_find: bad checksum %s page %p -> key %X, checksum %X is %X", 
	cache -> name, page, page -> key, page -> checksum, cksm);
    }
    oz_hw_phys_unmappage (savepte);
  }

  page -> usecount ++;
#endif

  /* Keep it locked in memory */

  page -> refcount ++;

  /* Apply the lockmode to it */

  switch (lockmode) {
    case OZ_LOCKMODE_NL: break;					/* null, just leave it as is */
    case OZ_LOCKMODE_PR: {					/* read-only: */
      while (page -> lockcount < 0) {				/* see if someone has it opened read/write */
        oz_hw_smplock_clr (&(cache -> smplock_cp), cp);		/* if so, release database lock */
        cefs |= CEFS_EVENT;					/* remember we are clearing event flag */
        oz_knl_event_waitone (cache -> event);			/* wait for them to finish with it */
        oz_knl_event_set (cache -> event, 0);			/* in case we have to wait again */
        cp = oz_hw_smplock_wait (&(cache -> smplock_cp));	/* lock database */
      }
      page -> lockcount ++;					/* no one is writing it, increment read-only count */
      break;
    }
    case OZ_LOCKMODE_EX: {					/* read/write: */
      while (page -> lockcount != 0) {				/* see if someone else accessing it at all */
        oz_hw_smplock_clr (&(cache -> smplock_cp), cp);		/* if so, release database lock */
        cefs |= CEFS_EVENT;					/* remember we are clearing event flag */
        oz_knl_event_waitone (cache -> event);			/* wait for them to finish with it */
        oz_knl_event_set (cache -> event, 0);			/* in case we have to wait again */
        cp = oz_hw_smplock_wait (&(cache -> smplock_cp));	/* lock database */
      }
      page -> lockcount = -1;					/* no one is accessing it, set read/write flag */
      break;
    }
    default: oz_crash ("oz_knl_cache_find: bad lock mode %d", lockmode);
  }
  oz_hw_smplock_clr (&(cache -> smplock_cp), cp);		/* release hash table */

  /* Move to end of oldpages list so it will be the last one freed off */

#if OZ_HW_MEMORYCOLORING
  l2 = phypage & (oz_s_phymem_l2pages - 1);
#endif

  pm = oz_hw_smplock_wait (&oz_s_smplock_pm);			/* lock the old pages list */
  if (page -> prev_old == NULL) {				/* see if it's a newly allocated page */
#if OZ_HW_MEMORYCOLORING
    *(oldpaget[l2])  = page;					/* if so, link me on to end of list */
    page -> prev_old = oldpaget[l2];				/* remember who comes before me */
    oldpaget[l2] = &(page -> next_old);				/* i am now the new end of list */
#else
    *oldpaget = page;						/* if so, link me on to end of list */
    page -> prev_old = oldpaget;				/* remember who comes before me */
    oldpaget  = &(page -> next_old);				/* i am now the new end of list */
#endif
    oz_knl_cache_pagecount ++;					/* there is one more cache page now */
  } else if ((npage = page -> next_old) != NULL) {		/* don't bother if already at end of list */
    lpage  = page -> prev_old;					/* ok, point to previous in list */
    *lpage = npage;						/* unlink me from previous in list */
    npage -> prev_old = lpage;					/* unlink me from next in list */

#if OZ_HW_MEMORYCOLORING
    *(oldpaget[l2])  = page;					/* link me on to end of list */
    page -> next_old = NULL;					/* nothing follows me */
    page -> prev_old = oldpaget[l2];				/* remember who comes before me */
    oldpaget[l2] = &(page -> next_old);				/* i am now the new end of list */
#else
    *oldpaget  = page;						/* link me on to end of list */
    page -> next_old = NULL;					/* nothing follows me */
    page -> prev_old = oldpaget;				/* remember who comes before me */
    oldpaget = &(page -> next_old);				/* i am now the new end of list */
#endif
  }
  oz_hw_smplock_clr (&oz_s_smplock_pm, pm);			/* unlock the oldpages list */

  /* All done, return requested values */

  if (cefs & CEFS_PHYPAGE) oz_knl_event_set (oz_s_freephypagevent, 1); /* if we cleared this, set it in case someone is waiting for it */
  if (cefs & CEFS_EVENT) oz_knl_event_set (cache -> event, 1);	/* if we cleared this, set it in case someone is waiting for it */
  if (pagex_r   != NULL) *pagex_r   = page -> pagex;		/* return page extension area pointer */
  if (phypage_r != NULL) *phypage_r = PAGE_PHYPAGE (page);	/* return physical page number */
  return (page);						/* return pointer to npp page stuff */
}

/************************************************************************/
/*									*/
/*  IXDEBUG routine - just like oz_knl_cache_find except returns NULL 	*/
/*  pointer if page not in cache and prints out page stats		*/
/*									*/
/************************************************************************/

OZ_Cachepage *oz_knl_cache_ixdeb (OZ_Cache *cache, OZ_Cachekey key, OZ_Lockmode lockmode, void **pagex_r, OZ_Mempage *phypage_r, OZ_Handle h_output)

{
  int cefs;
  Long refcount, usecount;
  OZ_Cachepage **lpage, *npage, *page;
  OZ_Mempage phypage;
  uLong calc_checksum, cp, hash_index, page_checksum, pm;

  hash_index = key % HASH_SIZE;
  cefs = 0;

scancache:
  cp = oz_hw_smplock_wait (&(cache -> smplock_cp));
  if (cp != OZ_SMPLOCK_SOFTINT) oz_crash ("oz_knl_cache findpage: called at smplevel %u (not softint)", cp);
  for (page = cache -> hash_table[hash_index]; page != NULL; page = page -> next_hash) {
    if (page -> key == key) {
      phypage = PAGE_PHYPAGE (page);
      break;
    }
  }

  /* If it wasn't found, return NULL pointer */

  if (page == NULL) {
    oz_hw_smplock_clr (&(cache -> smplock_cp), cp);		/* unlock cache hash table */
    return (NULL);
  }

  refcount = page -> refcount;

#if OZ_KNL_CACHE_CHECKSUM
  if (page -> refcount == 0) {
    OZ_Pagentry savepte;
    uLong cksm, i, *vaddr;

    cksm = 0;
    vaddr = oz_hw_phys_mappage (phypage, &savepte);
    for (i = 0; i < (1 << OZ_HW_L2PAGESIZE) / 4; i ++) {
      cksm += *(vaddr ++);
    }
    oz_hw_phys_unmappage (savepte);
    calc_checksum = cksm;
    page_checksum = page -> checksum;
  }

  usecount = page -> usecount;
#endif

  /* Keep it locked in memory */

  page -> refcount ++;

  /* Apply the lockmode to it */

  switch (lockmode) {
    case OZ_LOCKMODE_NL: break;					/* null, just leave it as is */
    case OZ_LOCKMODE_PR: {					/* read-only: */
      while (page -> lockcount < 0) {				/* see if someone has it opened read/write */
        oz_hw_smplock_clr (&(cache -> smplock_cp), cp);		/* if so, release database lock */
        cefs |= CEFS_EVENT;					/* remember we are clearing event flag */
        oz_knl_event_waitone (cache -> event);			/* wait for them to finish with it */
        oz_knl_event_set (cache -> event, 0);			/* in case we have to wait again */
        cp = oz_hw_smplock_wait (&(cache -> smplock_cp));	/* lock database */
      }
      page -> lockcount ++;					/* no one is writing it, increment read-only count */
      break;
    }
    case OZ_LOCKMODE_EX: {					/* read/write: */
      while (page -> lockcount != 0) {				/* see if someone else accessing it at all */
        oz_hw_smplock_clr (&(cache -> smplock_cp), cp);		/* if so, release database lock */
        cefs |= CEFS_EVENT;					/* remember we are clearing event flag */
        oz_knl_event_waitone (cache -> event);			/* wait for them to finish with it */
        oz_knl_event_set (cache -> event, 0);			/* in case we have to wait again */
        cp = oz_hw_smplock_wait (&(cache -> smplock_cp));	/* lock database */
      }
      page -> lockcount = -1;					/* no one is accessing it, set read/write flag */
      break;
    }
    default: oz_crash ("oz_knl_cache_find: bad lock mode %d", lockmode);
  }
  oz_hw_smplock_clr (&(cache -> smplock_cp), cp);		/* release hash table */

#if OZ_KNL_CACHE_CHECKSUM
  oz_sys_io_fs_printf (h_output, "oz_knl_cache_ixdeb*: refcount %u, usecount %u, loaded %t\n", refcount, usecount, page -> loaded);
  if (refcount == 0) {
    if (page_checksum == calc_checksum) oz_sys_io_fs_printf (h_output, "oz_knl_cache_ixdeb*: checksum %X\n", page_checksum);
    else oz_sys_io_fs_printf (h_output, "oz_knl_cache_ixdeb*: page_checksum %X, calc_checksum %X\n", page_checksum, calc_checksum);
  }
#else
  oz_sys_io_fs_printf (h_output, "oz_knl_cache_ixdeb*: refcount %u\n", refcount);
#endif

  /* All done, return requested values */

  if (cefs & CEFS_EVENT) oz_knl_event_set (cache -> event, 1);	/* if we cleared this, set it in case someone is waiting for it */
  if (pagex_r   != NULL) *pagex_r   = page -> pagex;		/* return page extension area pointer */
  if (phypage_r != NULL) *phypage_r = PAGE_PHYPAGE (page);	/* return physical page number */
  return (page);						/* return pointer to npp page stuff */
}

/************************************************************************/
/*									*/
/*  Return various things about a page					*/
/*									*/
/************************************************************************/

void *oz_knl_cache_pagex (OZ_Cachepage *page)

{
  return (page -> pagex);
}

OZ_Mempage oz_knl_cache_phypage (OZ_Cachepage *page)

{
  return (PAGE_PHYPAGE (page));
}

OZ_Cachekey oz_knl_cache_key (OZ_Cachepage *page)

{
  return (page -> key);
}

/************************************************************************/
/*									*/
/*  Increment reference count to a cache page by 1.  This is called 	*/
/*  by the oz_knl_section_copypages routine when a cache page is 	*/
/*  directly mapped to a process.					*/
/*									*/
/*    Input:								*/
/*									*/
/*	phypage  = physical page number of cache page			*/
/*	smplevel = pt							*/
/*									*/
/*    Note:								*/
/*									*/
/*	Cache page is assumed to have been locked by oz_knl_cache_find 	*/
/*	with lockmode OZ_LOCKMODE_NL, and will be unlocked by 		*/
/*	oz_knl_cache_done with OZ_LOCKMODE_NL.				*/
/*									*/
/************************************************************************/

void oz_knl_cachepage_increfcby1 (OZ_Mempage phypage)

{
  OZ_Cache *cache;
  uLong cp;

  if (phypage >= oz_s_phymem_totalpages) oz_crash ("oz_knl_cachepage_increfby1: bad phypage %u", phypage);
  cache = oz_s_phymem_pages[phypage].u.c.cache;
  cp = oz_hw_smplock_wait (&(cache -> smplock_cp));
  oz_s_phymem_pages[phypage].u.c.refcount ++;
  oz_hw_smplock_clr (&(cache -> smplock_cp), cp);
}

/************************************************************************/
/*									*/
/*  Convert lock mode							*/
/*									*/
/*    Input:								*/
/*									*/
/*	page = page to convert						*/
/*	oldmode = old (current) lock mode (_NL, _PR, _EX)		*/
/*	newmode = new mode (_NL, _PR, _EX)				*/
/*									*/
/*    Output:								*/
/*									*/
/*	page lock mode converted (may have waited)			*/
/*									*/
/*    Note:								*/
/*									*/
/*	This routine does not handle _PR to _EX conversions as they 	*/
/*	might cause a deadlock.  Downconvert to _NL first then 		*/
/*	upconvert to _EX (of course, something else could have gotten 	*/
/*	in there in the mean time and messed things up).		*/
/*									*/
/************************************************************************/

void oz_knl_cache_conv (OZ_Cache *cache, OZ_Cachepage *page, OZ_Lockmode oldmode, OZ_Lockmode newmode)

{
  int cefs;
  uLong cp;

  if ((oldmode == OZ_LOCKMODE_PR) && (newmode == OZ_LOCKMODE_EX)) oz_crash ("oz_knl_cache_conv: cant convert PR to EX"); /* it would deadlock if two did it at same time */

  cefs = 0;
  cp   = oz_hw_smplock_wait (&(cache -> smplock_cp));

  /* Remove old lockmode from it */

  switch (oldmode) {
    case OZ_LOCKMODE_NL: break;
    case OZ_LOCKMODE_PR: {
      page -> lockcount --;
      break;
    }
    case OZ_LOCKMODE_EX: {
      page -> lockcount = 0;
      break;
    }
    default: oz_crash ("oz_knl_cache_conv: bad old mode %d", oldmode);
  }

  /* Apply the new lockmode to it */

  switch (newmode) {
    case OZ_LOCKMODE_NL: break;
    case OZ_LOCKMODE_PR: {
      while (page -> lockcount < 0) {
        oz_hw_smplock_clr (&(cache -> smplock_cp), cp);
        oz_knl_event_waitone (cache -> event);
        oz_knl_event_set (cache -> event, 0);
        cp = oz_hw_smplock_wait (&(cache -> smplock_cp));
        cefs |= CEFS_EVENT;
      }
      page -> lockcount ++;
      break;
    }
    case OZ_LOCKMODE_EX: {
      while (page -> lockcount != 0) {
        oz_hw_smplock_clr (&(cache -> smplock_cp), cp);
        oz_knl_event_waitone (cache -> event);
        oz_knl_event_set (cache -> event, 0);
        cp = oz_hw_smplock_wait (&(cache -> smplock_cp));
        cefs |= CEFS_EVENT;
      }
      page -> lockcount = -1;
      break;
    }
    default: oz_crash ("oz_knl_cache_conv: bad new mode %d", newmode);
  }

  oz_hw_smplock_clr (&(cache -> smplock_cp), cp);

  /* In case someone was waiting for us to unlock page and we down-converted, set event flag */

  if ((cefs & CEFS_EVENT) || (newmode < oldmode)) oz_knl_event_set (cache -> event, 1);
}

/************************************************************************/
/*									*/
/*  Done with cache page						*/
/*									*/
/*    Input:								*/
/*									*/
/*	page = page							*/
/*	lockmode = current lock mode (_NL, _PR, _EX)			*/
/*									*/
/*    Output:								*/
/*									*/
/*	page might be freed off						*/
/*									*/
/************************************************************************/

void oz_knl_cache_done (OZ_Cache *cache, OZ_Cachepage *page, OZ_Lockmode lockmode)

{
  int cefs;
  uLong cp;

  cefs = 0;
  cp   = oz_hw_smplock_wait (&(cache -> smplock_cp));

  /* Remove old lockmode from it */

  switch (lockmode) {
    case OZ_LOCKMODE_NL: break;
    case OZ_LOCKMODE_PR: {
      page -> lockcount --;
      break;
    }
    case OZ_LOCKMODE_EX: {
      page -> lockcount = 0;
      break;
    }
    default: oz_crash ("oz_knl_cache_done: bad lock mode %d", lockmode);
  }

  /* Release it from memory.  If no one is using it and someone is waiting for a free page, tell them one is now available. */

  page -> refcount --;

#if OZ_KNL_CACHE_CHECKSUM
  if (page -> refcount == 0) {
    OZ_Pagentry savepte;
    uLong cksm, i, *vaddr;

    vaddr = oz_hw_phys_mappage (PAGE_PHYPAGE (page), &savepte);
    cksm = 0;
    for (i = 0; i < (1 << OZ_HW_L2PAGESIZE) / 4; i ++) {
      cksm += *(vaddr ++);
    }
    page -> checksum = cksm;
    oz_hw_phys_unmappage (savepte);
  }
#endif

  if ((page -> refcount == 0) && waitingforfreepage) {
    waitingforfreepage = 0;
    cefs |= CEFS_PHYPAGE;
  }

  oz_hw_smplock_clr (&(cache -> smplock_cp), cp);

  /* In case someone was waiting for us to unlock page, set event flag */

  oz_knl_event_set (cache -> event, 1);
  if (cefs & CEFS_PHYPAGE) oz_knl_event_set (oz_s_freephypagevent, 1);
}

/************************************************************************/
/*									*/
/*  Free off an old cache page						*/
/*									*/
/*  This routine is called by oz_knl_phymem_allocpage when it needs a 	*/
/*  page (and there is nothing currently free)				*/
/*									*/
/*    Input:								*/
/*									*/
/*	smp level = pm							*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_cache_freepage = OZ_PHYPAGE_NULL : nothing available	*/
/*	                                   else : phys page number	*/
/*									*/
/************************************************************************/

OZ_Mempage oz_knl_cache_freepage (OZ_Mempage l2)

{
  OZ_Cache *lockedcache;
  OZ_Cachepage *page;
  OZ_Mempage phypage;
  uLong cp;

  lockedcache = NULL;							/* no cache list is currently locked */
#if OZ_HW_MEMORYCOLORING
  for (page = oldpages[l2]; page != NULL; page = page -> next_old) {	/* scan starting with oldest page in all caches */
#else
  for (page = oldpages; page != NULL; page = page -> next_old) {	/* scan starting with oldest page in all caches */
#endif
    if (page -> cache != lockedcache) {					/* make sure that particular cache is locked */
      if (lockedcache != NULL) oz_hw_smplock_clr (&(lockedcache -> smplock_cp), cp);
      lockedcache = page -> cache;
      cp = oz_hw_smplock_wait (&(lockedcache -> smplock_cp));
    }
    if (page -> refcount == 0) {					/* check the cache page's reference count */
      phypage = free_page (page);					/* unused, unlink it and get physical page number */
      oz_hw_smplock_clr (&(lockedcache -> smplock_cp), cp);		/* unlock the cache it was in */
      return (phypage);							/* return its physical page number */
    }
  }
  if (lockedcache != NULL) oz_hw_smplock_clr (&(lockedcache -> smplock_cp), cp); /* make sure last cache checked is unlocked */
  waitingforfreepage = 1;						/* someone is waiting for an available page */
									/* this will cause oz_s_phypage_freevent to be set when a page goes idle */
  return (OZ_PHYPAGE_NULL);						/* no idle pages, return failure status */
}

/************************************************************************/
/*									*/
/*  Return cache statistics						*/
/*									*/
/************************************************************************/

void oz_knl_cache_stats (OZ_Cache *cache, uLong *nincache_r)

{
  OZ_KNL_CHKOBJTYPE (cache, OZ_OBJTYPE_CACHE);
  *nincache_r = cache -> nincache;
}

/************************************************************************/
/*									*/
/*  Remove a page from all lists and free it off			*/
/*									*/
/*    Input:								*/
/*									*/
/*	page = pointer to cache's page node				*/
/*	smplock = pm and cp						*/
/*									*/
/*    Output:								*/
/*									*/
/*	free_page = corresponding physical page				*/
/*									*/
/************************************************************************/

static OZ_Mempage free_page (OZ_Cachepage *page)

{
  OZ_Cachepage **lpage, *npage;
  OZ_Mempage phypage;

  phypage = PAGE_PHYPAGE (page);

  /* Remove from 'hash' list */

  lpage = page -> prev_hash;
  npage = page -> next_hash;
  *lpage = npage;
  if (npage != NULL) npage -> prev_hash = lpage;
  page -> cache -> nincache --;

  /* Remove from 'oldpages' list */

  lpage = page -> prev_old;
  npage = page -> next_old;
  *lpage = npage;
  if (npage != NULL) npage -> prev_old = lpage;
#if OZ_HW_MEMORYCOLORING
  else oldpaget[phypage&(oz_s_phymem_l2pages-1)] = lpage;
#else
  else oldpaget = lpage;
#endif
  oz_knl_cache_pagecount --;

  /* Return corresponding physical page number */

  return (phypage);
}

#if 00
static void cache_validate (int line, OZ_Cache *cache)

{
  OZ_Cachepage **lpage, *npage;
  uLong i;

  /* Validate cache's hash table lists */

  for (i = 0; i < HASH_SIZE; i ++) {
    for (lpage = cache -> hash_table + i; (npage = *lpage) != NULL; lpage = &(npage -> next_hash)) {
      if (npage -> prev_hash != lpage) {
        oz_crash ("oz_knl_cache validate: %d: page prev_hash incorrect", line);
      }
    }
  }

  /* Validate oldpages lists */

#if OZ_HW_MEMOYCOLORING
  for (i = 0; i < oz_s_phymem_l2pages; i ++) {
    for (lpage = oldpages + i; (npage = *lpage) != NULL; lpage = &(npage -> next_old)) {
      if (npage -> prev_old != lpage) {
        oz_crash ("oz_knl_cache validate: %d: page prev_old incorrect", line);
      }
    }
    if (oldpaget[i] != lpage) {
      oz_crash ("oz_knl_cache validate: %d: oldpaget[%u] doesn't point to last on oldpages list", line, i);
    }
  }
#else
  for (lpage = oldpages; (npage = *lpage) != NULL; lpage = &(npage -> next_old)) {
    if (npage -> prev_old != lpage) {
      oz_crash ("oz_knl_cache validate: %d: page prev_old incorrect", line);
    }
  }
  if (oldpaget != lpage) {
    oz_crash ("oz_knl_cache validate: %d: oldpaget doesn't point to last on oldpages list", line);
  }
#endif
}
#endif

const char *oz_knl_cache_getname (OZ_Cache *cache)

{
  return (cache -> name);
}

uLong oz_knl_cache_smplock_wait (OZ_Mempage phypage)

{
  return (oz_hw_smplock_wait (&(oz_s_phymem_pages[phypage].u.c.cache -> smplock_cp)));
}

void oz_knl_cache_smplock_clr (OZ_Mempage phypage, uLong level)

{
  oz_hw_smplock_clr (&(oz_s_phymem_pages[phypage].u.c.cache -> smplock_cp), level);
}
