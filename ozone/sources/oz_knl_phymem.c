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
/*  Physical memory allocation routines					*/
/*									*/
/************************************************************************/

#define _OZ_KNL_PHYMEM_C
#include "ozone.h"

#include "oz_knl_cache.h"
#include "oz_knl_hw.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_phymem.h"
#include "oz_knl_sdata.h"
#include "oz_knl_status.h"

#define VALIDATEFREEPAGES /* validatefreepages () */

static int freepagevent = 1;		/* copy of free page event flag state */

#if OZ_HW_MEMORYCOLORING
static int l1sinl2;			/* number of L1 cache's in L2 cache (l2size/l1size) */
static OZ_Mempage *phymem_freepages;	/* list of free pages */
static OZ_Mempage l1size, l2sizem1;	/* size of processor caches */

uLong oz_s_phymem_stat_freel2   = 0;	// number found free with L2 alignment
uLong oz_s_phymem_stat_freel1   = 0;	// number found free with L1 alignment
uLong oz_s_phymem_stat_freeany  = 0;	// number found free with no alignment
uLong oz_s_phymem_stat_cachel2  = 0;	// number found in cache with L2 alignment
uLong oz_s_phymem_stat_cachel1  = 0;	// number found in cache with L1 alignment
uLong oz_s_phymem_stat_cacheany = 0;	// number found in cache with no alignment
#else
static OZ_Mempage phymem_freepages;	/* list of free pages */
#endif

/************************************************************************/
/*									*/
/*  Initialize physical memory static data and non-paged pool		*/
/*									*/
/*    Input:								*/
/*									*/
/*	nppsize = number of bytes to allocate to non-paged pool		*/
/*	ffvirtpage = first free virt page in system space		*/
/*	ffphyspage = first free physical page				*/
/*									*/
/************************************************************************/

void oz_knl_phymem_init (uLong nppsize, OZ_Mempage ffvirtpage, OZ_Mempage ffphyspage)

{
  uLong bytes_per_page, bytes_required;
  OZ_Mempage first_physpage, first_virtpage, i, j, pages_required, phypage;
  OZ_Section_pagestate pagestate;
  uLong sts;
  void *nppbase;

#if OZ_HW_MEMORYCOLORING

  // the oz_s_phymem_l*pages vars are filled in by hardware 
  // ... layer before it calls oz_knl_boot_firstcpu

  oz_knl_printk ("oz_knl_phymem_init: cache modulo: L1 %u page%s (%u K), L2 %u page%s (%u K)\n", 
	oz_s_phymem_l1pages, (oz_s_phymem_l1pages == 1) ? "" : "s", oz_s_phymem_l1pages << (OZ_HW_L2PAGESIZE - 10), 
	oz_s_phymem_l2pages, (oz_s_phymem_l2pages == 1) ? "" : "s", oz_s_phymem_l2pages << (OZ_HW_L2PAGESIZE - 10));

  if (oz_s_phymem_l2pages < oz_s_phymem_l1pages) oz_s_phymem_l2pages = oz_s_phymem_l1pages; // code assumes L2 modulus is at least as big as L1

  l1size   = oz_s_phymem_l1pages;		// number of OZ_HW_L2PAGESIZE pages in L1 cache
  l2sizem1 = oz_s_phymem_l2pages;		// number of OZ_HW_L2PAGESIZE pages in L2 cache

  l1sinl2 = l2sizem1 / l1size;			// calc number of L1 caches that fit in L2 cache
  -- l2sizem1;					// make a mask out of this
#endif

  bytes_per_page = 1 << OZ_HW_L2PAGESIZE;

  bytes_required = oz_s_phymem_totalpages * sizeof *oz_s_phymem_pages + nppsize;
  pages_required = (bytes_required + bytes_per_page - 1) >> OZ_HW_L2PAGESIZE;

  oz_knl_printk ("oz_knl_phymem_init: %u pages required for phys mem state table and non-paged pool\n", pages_required);

  /* Get at least the required number of pages mapped to virtual addresses, accessible by kernel mode for read/write */
  /* These pages may have their pte's read, but they will never be written                                           */

  first_physpage = ffphyspage;
  first_virtpage = ffvirtpage;
  oz_hw_pool_init (&pages_required, &first_physpage, &first_virtpage);

  /* Calculate the virtual address those pages are at now and zero-fill the page state table */

  oz_s_phymem_pages = OZ_HW_VPAGETOVADDR (first_virtpage);
  oz_knl_printk ("oz_knl_phymem_init: physical memory state array at vaddr %p, phypage 0x%X\n", oz_s_phymem_pages, first_physpage);
  memset (oz_s_phymem_pages, 0, oz_s_phymem_totalpages * sizeof oz_s_phymem_pages[0]);
  for (j = 0; j < oz_s_phymem_totalpages; j ++) oz_s_phymem_pages[j].state = OZ_PHYMEM_PAGESTATE_FREE;

  /* Non-paged pool starts immediately following the physical page state table through to end of block */

  nppbase = oz_s_phymem_pages + j;
  nppsize = (OZ_Pointer)(OZ_HW_VPAGETOVADDR (first_virtpage + pages_required)) - (OZ_Pointer)nppbase;
  oz_knl_printk ("oz_knl_phymem_init: initial non-paged pool size %u (%u K), base %p\n", nppsize, nppsize >> 10, nppbase);
  OZ_KNL_NPPFREESIZ (nppsize, nppbase);

  /* Mark all physical pages that are being used by scanning the spt for valid entries        */
  /* Note that the spt may contain physical pages that are out of range for mapping i/o space */

  for (i = 0; i < oz_s_sysmem_pagtblsz; i ++) {
    if (oz_hw_pte_readany (OZ_HW_VADDRTOVPAGE (oz_s_sysmem_baseva) + i, &pagestate, &phypage, NULL, NULL) != NULL) continue;
    if (phypage >= oz_s_phymem_totalpages) continue;
    switch (pagestate) {
      case OZ_SECTION_PAGESTATE_VALID_R:
      case OZ_SECTION_PAGESTATE_VALID_W: {
        if ((oz_s_phymem_pages[phypage].state != OZ_PHYMEM_PAGESTATE_FREE) && (phypage != OZ_PHYPAGE_NULL)) {
          oz_crash ("oz_knl_phymem_init: phys page 0x%X is mapped by spt more than once", phypage);
        }
        oz_s_phymem_pages[phypage].state = OZ_PHYMEM_PAGESTATE_ALLOCPERM;
        break;
      }
      case OZ_SECTION_PAGESTATE_PAGEDOUT: {
        break;
      }
      default: {
        oz_crash ("oz_knl_phymem_init: unknown state %u in spt entry number 0x%X", pagestate, i);
      }
    }
  }

  /* Make sure we have physical page 0 marked as used as we don't ever want to assign it to anything */

  if ((OZ_PHYPAGE_NULL >= 0) && (OZ_PHYPAGE_NULL < oz_s_phymem_totalpages)) {
    oz_s_phymem_pages[OZ_PHYPAGE_NULL].state = OZ_PHYMEM_PAGESTATE_ALLOCPERM;
  }

  /* Make sure we have the first ffphyspage pages marked as used in case they aren't mapped by anything */

  for (i = 0; i < ffphyspage; i ++) {
    oz_s_phymem_pages[i].state = OZ_PHYMEM_PAGESTATE_ALLOCPERM;
  }

  /* Now make linked lists of free pages */

#if OZ_HW_MEMORYCOLORING
  phymem_freepages = OZ_KNL_NPPMALLOC ((l2sizem1 + 1) * sizeof *phymem_freepages);
  for (i = 0; i <= l2sizem1; i ++) phymem_freepages[i] = OZ_PHYPAGE_NULL;
#else
  phymem_freepages = OZ_PHYPAGE_NULL;
#endif

  oz_s_phymem_freepages = 0;
  while (j > 0) {
    if (oz_s_phymem_pages[--j].state == OZ_PHYMEM_PAGESTATE_FREE) {
#if OZ_HW_MEMORYCOLORING
      oz_s_phymem_pages[j].u.f.nextpage = phymem_freepages[j&l2sizem1];
      phymem_freepages[j&l2sizem1] = j;
#else
      oz_s_phymem_pages[j].u.f.nextpage = phymem_freepages;
      phymem_freepages = j;
#endif
      oz_s_phymem_freepages ++;
    }
  }
  oz_knl_printk ("oz_knl_phymem_init: there are 0x%X free pages left (%u Meg)\n", 
			oz_s_phymem_freepages, oz_s_phymem_freepages >> (20 - OZ_HW_L2PAGESIZE));

  /* Create frephypagevent flag and set it indicating there are free pages */

  sts = oz_knl_event_create (20, "oz_s_freephypagevent", NULL, &oz_s_freephypagevent);
  if (sts != OZ_SUCCESS) oz_crash ("oz_knl_phymem_init: error %u creating oz_s_freephypagevent", sts);
  oz_knl_event_set (oz_s_freephypagevent, 1);
}

/************************************************************************/
/*									*/
/*  Allocate physical page of memory					*/
/*									*/
/*    Input:								*/
/*									*/
/*	pagestate = OZ_PHYMEM_PAGESTATE_ALLOCSECT or _ALLOCACHE		*/
/*	virtpage  = virtual page number it will be mapped to		*/
/*	smp lock  = pm							*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_phymem_allocpage = OZ_PHYPAGE_NULL : no memory available	*/
/*	                                     else : physical page number
/*									*/
/*    Note:								*/
/*									*/
/*	if OZ_PHYPAGE_NULL returned, caller can use event 		*/
/*	'oz_s_freephypagevent' to wait for a free page			*/
/*									*/
/************************************************************************/

	/* waits, called at null or softint level */

OZ_Mempage oz_knl_phymem_allocpagew (OZ_Phymem_pagestate pagestate, OZ_Mempage virtpage)

{
  OZ_Mempage p;
  uLong pm;

  pm = oz_hw_smplock_wait (&oz_s_smplock_pm);

  while ((p = oz_knl_phymem_allocpage (pagestate, virtpage)) == OZ_PHYPAGE_NULL) {
    oz_hw_smplock_clr (&oz_s_smplock_pm, pm);
    oz_knl_event_waitone (oz_s_freephypagevent);
    pm = oz_hw_smplock_wait (&oz_s_smplock_pm);
    oz_knl_event_set (oz_s_freephypagevent, 0);
    freepagevent = 0;
  }
  oz_hw_smplock_clr (&oz_s_smplock_pm, pm);

  return (p);
}

	/* no waiting, called at level PM */

OZ_Mempage oz_knl_phymem_allocpage (OZ_Phymem_pagestate pagestate, OZ_Mempage virtpage)

{
  int i;
  OZ_Mempage l2, p;

  if ((pagestate != OZ_PHYMEM_PAGESTATE_ALLOCSECT) && (pagestate != OZ_PHYMEM_PAGESTATE_ALLOCACHE)) {
    oz_crash ("oz_knl_phymem_allocpage: pagestate %d", pagestate);
  }

#if OZ_HW_MEMORYCOLORING

  /* Colored, try to get a free page from free page list keeping it tidy for L2 cache */

  l2 = virtpage & l2sizem1;
  p  = phymem_freepages[l2];
  if (p != OZ_PHYPAGE_NULL) {
    oz_s_phymem_stat_freel2 ++;
    goto gotfreepage;
  }

  /* Try getting one from the disk cache tidy for L2 cache */

  p = oz_knl_cache_freepage (l2);
  if (p != OZ_PHYPAGE_NULL) {
    oz_s_phymem_stat_cachel2 ++;
    goto gotcachepage;
  }

  /* Try one from free list that's tidy for L1 cache */

  for (i = l1sinl2; -- i >= 0;) {
    l2 = (l2 + l1size) & l2sizem1;
    p  = phymem_freepages[l2];
    if (p != OZ_PHYPAGE_NULL) {
      oz_s_phymem_stat_freel1 ++;
      goto gotfreepage;
    }
  }

  /* Try one from disk cache that's tidy for L1 cache */

  for (i = l1sinl2; -- i >= 0;) {
    l2 = (l2 + l1size) & l2sizem1;
    p  = oz_knl_cache_freepage (l2);
    if (p != OZ_PHYPAGE_NULL) {
      oz_s_phymem_stat_cachel1 ++;
      goto gotcachepage;
    }
  }

  /* Try one from any free list */

  for (i = l2sizem1; i >= 0; -- i) {
    l2 = (l2 + 1) & l2sizem1;
    p  = phymem_freepages[l2];
    if (p != OZ_PHYPAGE_NULL) {
      oz_s_phymem_stat_freeany ++;
      goto gotfreepage;
    }
  }

  /* Try one from disk cache with any alignment */

  for (i = l2sizem1; i >= 0; -- i) {
    l2 = (l2 + 1) & l2sizem1;
    p  = oz_knl_cache_freepage (l2);
    if (p != OZ_PHYPAGE_NULL) {
      oz_s_phymem_stat_cacheany ++;
      goto gotcachepage;
    }
  }

#else

  /* Not colored, just get a free page */

  p = phymem_freepages;
  if (p != OZ_PHYPAGE_NULL) goto gotfreepage;

  /* Try getting one from the disk cache tidy for L2 cache */

  p = oz_knl_cache_freepage (0);
  if (p != OZ_PHYPAGE_NULL) goto gotcachepage;

#endif

  /* If none available, clear event flag so someone can wait for it */

  if (freepagevent) oz_knl_event_set (oz_s_freephypagevent, 0);
  freepagevent = 0;
  return (OZ_PHYPAGE_NULL);

  /* Got one, mark it allocated and return page number */

gotfreepage:
  if (p >= oz_s_phymem_totalpages) oz_crash ("oz_knl_phymem_allocpage: bad phypage %u", p);
  if (oz_s_phymem_pages[p].state != OZ_PHYMEM_PAGESTATE_FREE) oz_crash ("oz_knl_phymem_allocpage: non-free page on free page list");
#if OZ_HW_MEMORYCOLORING
  phymem_freepages[l2] = oz_s_phymem_pages[p].u.f.nextpage;
#else
  phymem_freepages = oz_s_phymem_pages[p].u.f.nextpage;
#endif
  oz_s_phymem_freepages --;
gotcachepage:
  oz_s_phymem_pages[p].state = pagestate;
  return (p);
}

/************************************************************************/
/*									*/
/*  Allocate physical contiguous pages of memory			*/
/*									*/
/*    Input:								*/
/*									*/
/*	count     = number of pages to allocate				*/
/*	pagestate = OZ_PHYMEM_PAGESTATE_ALLOCSECT or _ALLOCACHE		*/
/*	smp lock  = pm							*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_phymem_allocontig = OZ_PHYPAGE_NULL : no memory available
/*	                                      else : physical page number
/*									*/
/*    Note:								*/
/*									*/
/*	if OZ_PHYPAGE_NULL returned, caller can use event 		*/
/*	'oz_s_freephypagevent' to wait for a free page			*/
/*									*/
/************************************************************************/

OZ_Mempage oz_knl_phymem_allocontig (OZ_Mempage count, OZ_Phymem_pagestate pagestate, OZ_Mempage virtpage)

{
  int i;
  OZ_Mempage l2, *lp1, p0, p1;

  if ((pagestate != OZ_PHYMEM_PAGESTATE_ALLOCSECT) && (pagestate != OZ_PHYMEM_PAGESTATE_ALLOCACHE)) {
    oz_crash ("oz_knl_phymem_allocpage: pagestate %d", pagestate);
  }

  VALIDATEFREEPAGES;

#if OZ_HW_MEMORYCOLORING

  l2 = virtpage & l2sizem1;

  /* Colored, try friendly to L2 cache */

  for (p0 = phymem_freepages[l2]; p0 != OZ_PHYPAGE_NULL; p0 = oz_s_phymem_pages[p0].u.f.nextpage) {
    for (p1 = 0; p1 < count; p1 ++) {
      if (oz_s_phymem_pages[p1+p0].state != OZ_PHYMEM_PAGESTATE_FREE) break;
    }
    if (p1 == count) goto found;
  }

  /* Try friendly to L1 cache */

  for (i = l1sinl2; -- i >= 0;) {
    l2 = (l2 + l1size) & l2sizem1;
    for (p0 = phymem_freepages[l2]; p0 != OZ_PHYPAGE_NULL; p0 = oz_s_phymem_pages[p0].u.f.nextpage) {
      for (p1 = 0; p1 < count; p1 ++) {
        if (oz_s_phymem_pages[p1+p0].state != OZ_PHYMEM_PAGESTATE_FREE) break;
      }
      if (p1 == count) goto found;
    }
  }

  /* Try anything */

  for (i = l2sizem1; i >= 0; -- i) {
    l2 = (l2 + 1) & l2sizem1;
    for (p0 = phymem_freepages[l2]; p0 != OZ_PHYPAGE_NULL; p0 = oz_s_phymem_pages[p0].u.f.nextpage) {
      for (p1 = 0; p1 < count; p1 ++) {
        if (oz_s_phymem_pages[p1+p0].state != OZ_PHYMEM_PAGESTATE_FREE) break;
      }
      if (p1 == count) goto found;
    }
  }

#else

  /* Not colored, try the only thing we have */

  for (p0 = phymem_freepages; p0 != OZ_PHYPAGE_NULL; p0 = oz_s_phymem_pages[p0].u.f.nextpage) {
    for (p1 = 0; p1 < count; p1 ++) {
      if (oz_s_phymem_pages[p1+p0].state != OZ_PHYMEM_PAGESTATE_FREE) break;
    }
    if (p1 == count) goto found;
  }

#endif

  /* Eh? Oh, Well! */

  return (OZ_PHYPAGE_NULL);

  /* 'count' contig pages starting at 'p0' have been found, mark them all allocated */

found:
  if ((p0 >= oz_s_phymem_totalpages) || (p0 + count > oz_s_phymem_totalpages)) {
    oz_crash ("oz_knl_phymem_allocontig: bad phypage %u..%u", p0, p0 + count - 1);
  }
  oz_s_phymem_freepages -= count;
#if OZ_HW_MEMORYCOLORING
  p0 += count;
  for (; count > 0; -- count) {
    for (lp1 = phymem_freepages + ((-- p0) & l2sizem1); (p1 = *lp1) != p0; lp1 = &(oz_s_phymem_pages[p1].u.f.nextpage)) {
      if (p1 == OZ_PHYPAGE_NULL) oz_crash ("oz_knl_phymem_allocontig: page %u not on free list", p0);
    }
    *lp1 = oz_s_phymem_pages[p1].u.f.nextpage;
    oz_s_phymem_pages[p1].state = pagestate;
  }
#else
  for (lp1 = phymem_freepages; (p1 = *lp1) != OZ_PHYPAGE_NULL;) {
    if ((p1 >= p0) && (p1 < p0 + count)) {
      *lp1 = oz_s_phymem_pages[p1].u.f.nextpage;
      oz_s_phymem_pages[p1].state = pagestate;
    } else {
      lp1 = &(oz_s_phymem_pages[p1].u.f.nextpage);
    }
  }
  for (p1 = 0; p1 < count; p1 ++) {
    if (oz_s_phymem_pages[p0+p1] == OZ_PHYMEM_PAGESTATE_FREE) {
      oz_crash ("oz_knl_phymem_allocontig: page %u not on free list", p0 + p1);
    }
  }
#endif

  return (p0);
}

/************************************************************************/
/*									*/
/*  Free physical page of memory					*/
/*									*/
/*    Input:								*/
/*									*/
/*	phypage  = physical page number to deallocate			*/
/*	smp lock = pm							*/
/*									*/
/*    Output:								*/
/*									*/
/*	physical page is added to the free page list			*/
/*									*/
/************************************************************************/

void oz_knl_phymem_freepage (OZ_Mempage phypage)

{
  OZ_Mempage l2, p;

  p = phypage;

  if (p > oz_s_phymem_totalpages) oz_crash ("oz_knl_phymem_freepage: bad phypage %u", p);
  if ((oz_s_phymem_pages[p].state != OZ_PHYMEM_PAGESTATE_ALLOCSECT) 
   && (oz_s_phymem_pages[p].state != OZ_PHYMEM_PAGESTATE_ALLOCACHE)) oz_crash ("oz_knl_phymem_freepage: freeing non-alloc page");

  VALIDATEFREEPAGES;

  /* Link page to free page list */

  oz_s_phymem_pages[p].state = OZ_PHYMEM_PAGESTATE_FREE;
#if OZ_HW_MEMORYCOLORING
  l2 = p & l2sizem1;
  oz_s_phymem_pages[p].u.f.nextpage = phymem_freepages[l2];
  phymem_freepages[l2] = p;
#else
  oz_s_phymem_pages[p].u.f.nextpage = phymem_freepages;
  phymem_freepages = p;
#endif

  oz_s_phymem_freepages ++;

  VALIDATEFREEPAGES;

  /* Say there is some physical memory available now */

  if (!freepagevent) {
    oz_knl_event_set (oz_s_freephypagevent, 1);
    freepagevent = 1;
  }
}

static void validatefreepages ()

{
  OZ_Mempage l2, n, p;
  OZ_Phymem_pagestate s;

  OZ_KNL_CHKSMPLEVEL (OZ_SMPLOCK_LEVEL_PM);

  n = 0;
#if OZ_HW_MEMORYCOLORING
  for (l2 = 0; l2 <= l2sizem1; l2 ++) {
    for (p = phymem_freepages[l2]; p != OZ_PHYPAGE_NULL; p = oz_s_phymem_pages[p].u.f.nextpage) {
      s = oz_s_phymem_pages[p].state;
      if (s != OZ_PHYMEM_PAGESTATE_FREE) oz_crash ("oz_knl_phymem validatefreepages: page %u state %d", p, s);
      n ++;
    }
  }
#else
  for (p = phymem_freepages; p != OZ_PHYPAGE_NULL; p = oz_s_phymem_pages[p].u.f.nextpage) {
    s = oz_s_phymem_pages[p].state;
    if (s != OZ_PHYMEM_PAGESTATE_FREE) oz_crash ("oz_knl_phymem validatefreepages: page %u state %d", p, s);
    n ++;
  }
#endif

  if (n != oz_s_phymem_freepages) oz_crash ("oz_knl_phymem validatefreepages: should be %u free, there are %u free", oz_s_phymem_freepages, n);
}
