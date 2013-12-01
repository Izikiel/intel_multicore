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

#include <stdarg.h>

#include "ozone.h"
#include "oz_dev_timer.h"
#include "oz_knl_hw.h"
#include "oz_knl_printk.h"
#include "oz_knl_sdata.h"
#include "oz_ldr_loader.h"

#define TEMPMEMSIZE 1024*1024
#define PAGESIZE (1 << OZ_HW_L2PAGESIZE)
#define PTESPERPAGE (PAGESIZE / 8)

OZ_Mempage oz_hwaxp_iobasevpage;
OZ_Mempage oz_hwaxp_knlbasevpage = 0;

extern OZ_Loadparams OZ_IMAGE_NEXTADDR;
static uQuad knl_phymem_count, knl_phymem_start;

static uQuad mappagekrw (uQuad ppage, uQuad vpage, int gh, uQuad phymem_count, uQuad phymem_start);
static uLong printmchargs (void *dummy, const char *format, ...);

/************************************************************************/
/*									*/
/*  This routine is called by oz_loader_axp.s after everything has 	*/
/*  be relocated from low virtual addresses to high virtual addresses	*/
/*									*/
/************************************************************************/

void *oz_ldr_init_axp (OZ_Hwaxp_Hwrpb *hwrpbva)

{
  int i, j, k;
  OZ_Hwaxp_Cpudb *cpudb;
  uByte *tempmemaddr;
  uLong kstacksize, maxcpus, systempages;
  uQuad cacheareabase, cacheareasize, phymem_count, phymem_start, next8gblock, our8gbblock, pte, *pteva;
  uQuad tempmempage, tempmempages, tempmemsize, tempsptl3page;
  void *kstart;

  /* Init a bunch of stuff and get range of usable physical memory pages */

  oz_hwaxp_common_init (hwrpbva, &phymem_count, &phymem_start, &maxcpus);

  /* Exclude the loader pages from the phymem_count/_start range so we won't try to load the kernel over it */
  /* Also, so oz_hw_pte_read... will work, set the software page status bits like it wants                  */

#define MASK (uQuad)((OZ_HWAXP_PTE_X_PS<<OZ_HWAXP_PTE_V_PS) + (OZ_HWAXP_PTE_X_CP<<OZ_HWAXP_PTE_V_CP) + (OZ_HWAXP_PTE_X_RP<<OZ_HWAXP_PTE_V_RP) + OZ_HWAXP_PTE_M_GBL)

  for (i = 0; i < PAGESIZE / 8; i ++) {								// loop through L1 pte's
    pte = *(pteva = oz_hwaxp_l1ptbase + i);							// get a L1 pte
    if (pte & 1) {										// skip if not valid
      *pteva = (pte & ~MASK) | (OZ_HWAXP_PTE_GKW & MASK);					// ok, make sure sw status & gbl set
      for (j = 0; j < PAGESIZE / 8; j ++) {							// loop through its L2 pte's
        pte = *(pteva = oz_hwaxp_l2ptbase + i * PTESPERPAGE + j);				// get a L2 pte
        if (pte & 1) {										// skip if not valid
          *pteva = (pte & ~MASK) | (OZ_HWAXP_PTE_GKW & MASK);					// ok, make sure sw status & gbl set
          for (k = 0; k < PAGESIZE / 8; k ++) {							// loop through its L3 pte's
            pte = *(pteva = oz_hwaxp_l3ptbase + (((i * PTESPERPAGE) + j) * PTESPERPAGE) + k);	// get a L3 pte
            if (pte & 1) {									// skip if not valid
              *pteva = (pte & ~MASK) | (OZ_HWAXP_PTE_GKW & MASK);				// ok, make sure sw status & gbl set
              pte  >>= OZ_HWAXP_PTE_V_PP;							// get physical page it maps
              if ((pte >= phymem_start) && (pte < phymem_count + phymem_start)) {		// see if within what we think is free
                if (pte - phymem_start > phymem_count / 2) phymem_count = pte - phymem_start;	// if so, remove from the end
                else {
                  phymem_count -= pte - phymem_start;						// ... or the beginning of range
                  phymem_start  = pte;
                }
              }
            }
          }
        }
      }
    }
  }

  /* Make up some non-paged pool at the end of physical memorie */
  /* Map it to the end of our 8GB block of virtual memorie      */

  tempmemsize   = TEMPMEMSIZE;						// temp memory
  our8gbblock   = ((uQuad)hwrpbva) & -OZ_HWAXP_L3PTSIZE;		// beginning of 8GB block we are in
  next8gblock   = our8gbblock + OZ_HWAXP_L3PTSIZE;			// end of 8GB block we are in
  tempmemaddr   = (uByte *)(next8gblock - PAGESIZE - tempmemsize);	// base address of temp memory
  tempmempages  = tempmemsize >> OZ_HW_L2PAGESIZE;			// number of pages
  phymem_count -= tempmempages;						// that many less pages availble
  tempmempage   = phymem_start + phymem_count;				// starting phypage for temp memory
  for (i = 0; i < tempmempages; i ++) {					// map them read/write by kernel mode
    phymem_count = mappagekrw (tempmempage + i, OZ_HW_VADDRTOVPAGE (tempmemaddr) + i, 0, phymem_count, phymem_start);
  }

  /* Make up an L3 page of temp SPTE's for use by drivers       */
  /* Also, the first NTEMPSPTES will be used for the temp sptes */

  pteva = (OZ_HW_VADDRTOVPAGE (oz_ldr_init_axp) >> (OZ_HW_L2PAGESIZE - 3)) + oz_hwaxp_l2ptbase;	// get L2 pte I'm mapped with
  while (*(++ pteva) != 0) {}									// find unused entry after it
  *pteva = ((phymem_start + -- phymem_count) << OZ_HWAXP_PTE_V_PP) + OZ_HWAXP_PTE_GKW;		// alloc a physical page for L3 page

  cpudb = oz_hwaxp_cpudb + oz_hw_cpu_getcur ();
  cpudb -> db.tempspte = OZ_HW_VPAGETOVADDR (pteva - oz_hwaxp_l3ptbase);
  cpudb -> db.tempspva = OZ_HW_VPAGETOVADDR (cpudb -> db.tempspte - oz_hwaxp_l3ptbase);

  memset (cpudb -> db.tempspte, 0, PAGESIZE);							// zero fill the L3 page

  /* Initialise other modules */

  phymem_count = oz_dev_pyxis_early (phymem_count, phymem_start);	// turns on I/O environment
  oz_dev_tod_init ();							// find out what time it is
  oz_hw_xxxproc_init ();						// oz_hw_(uni/sp)proc.c, turns on interrupts

  /* Call oz_ldr_start - as far as we are concerned, it loads the kernel in memory and returns with its start address */

  cacheareasize = 256;							// number of pages for temp cache

  phymem_count -= cacheareasize;
  cacheareabase = phymem_start + phymem_count;

  knl_phymem_count = phymem_count;
  knl_phymem_start = phymem_start;

  kstart = oz_ldr_start (&OZ_IMAGE_NEXTADDR, 				// paramblock = at end of my image
                         (void *)oz_hwaxp_sysbasva, 			// sysbaseva = this and above is common to all processes
                         0, 						// obsolete
                         0, 						// obsolete
                         tempmemsize, 					// tempmemsize = some non-paged pool
                         tempmemaddr, 					// tempmemaddr = some non-paged pool
                         &kstacksize, 					// kstacksize = kernel stack size
                         &systempages, 					// where to return number of system pages
                         cacheareasize, 				// cacheareasize = number of pages for temp cache
                         cacheareabase, 				// cacheareabase = start of cache pages
                         PAGESIZE / 8 - OZ_HWAXP_NTEMPSPTES, 		// n temp sptes = one page worth - NTEMPSPTES
                         OZ_HW_VADDRTOVPAGE (cpudb -> db.tempspva) + OZ_HWAXP_NTEMPSPTES); // starting virt page of temp sptes

  /* No need for interrupts from now until kernel sets up new SCB, etc */

  OZ_HWAXP_MTPR_IPL (31);

  oz_knl_printk ("oz_ldr_init_axp: kstart %p\n", kstart);
  return (kstart);
}

/************************************************************************/
/*									*/
/*  Map a page to be kernel read/write					*/
/*									*/
/*    Input:								*/
/*									*/
/*	ppage = physical page number					*/
/*	vaddr = virtual address to map it at				*/
/*	phymem_count = number of free physical pages			*/
/*	phymem_start = start of free physical pages			*/
/*									*/
/*    Output:								*/
/*									*/
/*	mappagekrw = same as phymem_count, possibly decremented		*/
/*	ppage mapped for kernel read/write access at vaddr		*/
/*									*/
/************************************************************************/

static uQuad mappagekrw (uQuad ppage, uQuad vpage, int gh, uQuad phymem_count, uQuad phymem_start)

{
  uQuad index, pte, *pteva;

  /* Make sure the gh is OK for the pages given */

  if (((ppage ^ vpage) % (1 << (3 * gh))) != 0) {
    oz_crash ("oz_ldr_init_axp mappagekrw: gh %d, ppage %QX, vpage %QX", gh, ppage, vpage);
  }

  /* Make sure there's an L2 page, if not allocate one */

  index = vpage >> (2 * OZ_HW_L2PAGESIZE - 6);					// this is index into L1 table
  pteva = oz_hwaxp_l1ptbase + index;						// point to the L1 pte
  pte   = *pteva;								// read it
  if ((pte & OZ_HWAXP_PTE__KW) != OZ_HWAXP_PTE__KW) {				// see if it's at least what we need
    if (pte != 0) oz_crash ("oz_ldr_init mappagekrw: bad L1 pte %QX", pte);
    *pteva = ((-- phymem_count + phymem_start) << 32) + OZ_HWAXP_PTE_GKW;	// if not, make one
    memset (OZ_HW_VPAGETOVADDR (pteva - oz_hwaxp_l3ptbase), 0, PAGESIZE);	// zero it out
  }

  /* Make sure there's an L3 page, if not allocate one */

  index = vpage >> (OZ_HW_L2PAGESIZE - 3);					// this is index into L2 table
  pteva = oz_hwaxp_l2ptbase + index;						// point to the L2 pte
  pte   = *pteva;								// read it
  if ((pte & OZ_HWAXP_PTE__KW) != OZ_HWAXP_PTE__KW) {				// see if it's at least what we need
    if (pte != 0) oz_crash ("oz_ldr_init mappagekrw: bad L2 pte %QX", pte);
    *pteva = ((-- phymem_count + phymem_start) << 32) + OZ_HWAXP_PTE_GKW;	// if not, make one
    memset (OZ_HW_VPAGETOVADDR (pteva - oz_hwaxp_l3ptbase), 0, PAGESIZE);	// zero it out
  }

  /* Now it is OK to map the page */

  pteva = oz_hwaxp_l3ptbase + vpage;						// point to the L3 entry
  if (*pteva != 0) oz_crash ("oz_ldr_init mappagekrw: bad L3 pte %QX", *pteva);	// make sure the entry is zero
  *pteva = (ppage << OZ_HWAXP_PTE_V_PP) + (gh << OZ_HWAXP_PTE_V_GH) + OZ_HWAXP_PTE_GKW; // write entry

  return (phymem_count);
}

OZ_Datebin oz_hw_tod_getnow (void)

{
  OZ_Iotatime iotanow;

  iotanow = oz_hw_tod_iotanow ();
  return (oz_hw_tod_aiota2sys (iotanow));
}

void oz_knl_thread_timerinit ()

{ }

/************************************************************************/
/*									*/
/*  If the kernel is linked dynamically, this will determine the 	*/
/*  actual load address.  We base it on the number of superpages 	*/
/*  needed for the I/O system.						*/
/*									*/
/*    Input:								*/
/*									*/
/*	pageprot = OZ_HW_PAGEPROT_KW or _UR				*/
/*	npagem   = number of pages being mapped				*/
/*	svpage   = starting relative virtual page number within image	*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_hw_ldr_knlpage_basevp = actual starting virtual page		*/
/*									*/
/************************************************************************/

OZ_Mempage oz_hw_ldr_knlpage_basevp (OZ_Hw_pageprot pageprot, OZ_Mempage npagem, OZ_Mempage svpage)

{
  OZ_Mempage i;

  if (oz_hwaxp_knlbasevpage == 0) {
    for (i = PTESPERPAGE; -- i > 0;) {				// see how many slots at the end of L1 are used
      if (oz_hwaxp_l1ptbase[i] == 0) break;			// (presumably by I/O system)
    }
    if (i <= oz_ldr_paramblock.system_pages + 2) {		// +2: 1 for kernel, 1 for self-mapping entry
      oz_crash ("oz_hw_ldr_knlpage_basevp: no available L1 slots for kernel");
    }
    oz_hwaxp_iobasevpage   = (i + 1) * PTESPERPAGE * PTESPERPAGE; // base vpage of the I/O area used by Pyxis or whatever
    i -= oz_ldr_paramblock.system_pages;			// also leave 'system_pages' L1 extra slots available
    oz_hwaxp_knlbasevpage  = i * PTESPERPAGE * PTESPERPAGE;	// this is the base vpage for the kernel image
    oz_hwaxp_l1ptbase[i]   = oz_hwaxp_l1ptbase[0];		// double-map the L2 page
    oz_hwaxp_l1ptbase[--i] = oz_hwaxp_l1ptbase[1];		// double-map the L1 page
    oz_knl_printk ("oz_hw_ldr_knlpage_basevp: I/O base address %p\n", OZ_HW_VPAGETOVADDR (oz_hwaxp_iobasevpage));
    oz_knl_printk ("oz_hw_ldr_knlpage_basevp: kernel base address %p\n", OZ_HW_VPAGETOVADDR (oz_hwaxp_knlbasevpage));
  }
  return (svpage + oz_hwaxp_knlbasevpage);
}

/************************************************************************/
/*									*/
/*  This routine is called by oz_ldr_start when it is about to read 	*/
/*  some of the kernel into memory.  It maps some pages at the 		*/
/*  requested virtual addresses and sets them to Kernel Read/Write 	*/
/*  access.								*/
/*									*/
/*    Input:								*/
/*									*/
/*	npagem = number of pages to map					*/
/*	svpage = starting virtual page to map				*/
/*									*/
/************************************************************************/

void oz_hw_ldr_knlpage_maprw (OZ_Mempage npagem, OZ_Mempage svpage)

{
  int gh, maxgh;
  OZ_Mempage ghsize, i, np, skip_count, skip_start, sppage, vp;

  /* Pick best granularity given the size and page */

  maxgh = 0;
  np = npagem;
  vp = svpage;
  while (np > 0) {
    for (gh = 3; gh >= 0; -- gh) {				// step thru 512,64,8,1 page blocks
      ghsize = 1 << (3 * gh);
      while (((vp & (ghsize - 1)) == 0) && (np >= ghsize)) {	// see if we have such a granule
        if (maxgh < gh) maxgh = gh;				// ok, remember that we have one that big
        np -= ghsize;						// skip over that granule
        vp += ghsize;
      }
    }
  }

  /* Find block of physical memory that aligns with the best granularity found */
  /* It must have the low 'gh*3' phypage bits identical to the virtpage bits   */

  ghsize = 1 << (maxgh * 3);							// number of pages in max gran hint we will use
  sppage = (knl_phymem_start & -ghsize) | (svpage & (ghsize - 1));		// starting phypage = top bits from knl_phymem_start
										//                  + bottom bits from svpage
  if ((knl_phymem_start & (ghsize - 1)) > (svpage & (ghsize - 1))) sppage += ghsize;
  if (sppage + npagem > knl_phymem_start + knl_phymem_count) oz_crash ("oz_hw_ldr_knlpage_maprw: not enough memory");

  /* Save what's left over for another section */

  skip_count = sppage - knl_phymem_start;					// the few pages we skip at the beginning to align
  skip_start = knl_phymem_start;						// we might be able to use them for L2/L3 pages
  knl_phymem_count = (knl_phymem_start + knl_phymem_count) - (sppage + npagem);	// how many pages are left over at the end
  knl_phymem_start = sppage + npagem;						// start of what is left over at the end

  /* Map it to virtual address space, allocating L2 and L3 pages from left over memory as necessary */

  oz_knl_printk ("oz_hw_ldr_knlpage_maprw: mapping %LX pages at %LX to %LX, maxgh %d\n", npagem, sppage, svpage, maxgh);

  while (npagem > 0) {								// repeat until none left to map
    for (gh = maxgh; gh >= 0; -- gh) {						// start with biggest granularity
      ghsize = 1 << (3 * gh);							// number of pages in the granule
      while (((svpage & (ghsize - 1)) == 0) && (npagem >= ghsize)) {		// repeat while we have granule alignment
        for (i = 0; i < ghsize; i ++) {						// map the whole granule page by page
          if (skip_count >= 2) skip_count = mappagekrw (sppage + i, svpage + i, gh, skip_count, skip_start);
          else knl_phymem_count = mappagekrw (sppage + i, svpage + i, gh, knl_phymem_count, knl_phymem_start);
        }
        npagem -= ghsize;							// decrement number of pages to do
        svpage += ghsize;							// increment starting virtual page
        sppage += ghsize;							// increment starting physical page
      }
    }
  }
}

/************************************************************************/
/*									*/
/*  This routine is called by oz_ldr_start after it has read some 	*/
/*  kernel pages into memory and it needs them to be set read-only 	*/
/*  (the pages are currently set read/write by the above maprw routine)	*/
/*									*/
/************************************************************************/

void oz_hw_ldr_knlpage_setro (OZ_Mempage npagem, OZ_Mempage svpage)

{
  int gh;
  uLong ghsize;
  uQuad pte;

  oz_knl_printk ("oz_hw_ldr_knlpage_setro: changing %LX pages at %LX to read-only\n", npagem, svpage);

  /* The first page we change should be at the start of its granularity hint range */
  /* Most likely, if there is a page immediately before it, it is remaining KW     */

  /* We crash instead of fix it because the maprw routine should have been called separately for each section */

  pte    = oz_hwaxp_l3ptbase[svpage];
  gh     = (pte >> OZ_HWAXP_PTE_V_GH) & OZ_HWAXP_PTE_X_GH;
  ghsize = 1 << (3 * gh);
  if ((svpage & (ghsize - 1)) != 0) oz_crash ("oz_hw_ldr_knlpage_setro: vpage %LX has gh %d", svpage, gh);

  /* Likewise, the last page we change should be at the end of its granularity hint range */

  pte    = oz_hwaxp_l3ptbase[svpage+npagem-1];
  gh     = (pte >> OZ_HWAXP_PTE_V_GH) & OZ_HWAXP_PTE_X_GH;
  ghsize = 1 << (3 * gh);
  if (((svpage + npagem) & (ghsize - 1)) != 0) oz_crash ("oz_hw_ldr_knlpage_setro: vpage %LX has gh %d", svpage + npagem, gh);

  /* OK, change the pages from KW to UR */
  
  while (npagem > 0) {
    -- npagem;
    pte = oz_hwaxp_l3ptbase[svpage];
    if ((pte & OZ_HWAXP_PTE__XX) != OZ_HWAXP_PTE__KW) {
      oz_crash ("oz_hw_ldr_knlpage_setro: vpage %LX was not KW (pte %QX)\n", svpage, pte);
    }
    if (((pte >> OZ_HWAXP_PTE_V_PS) & OZ_HWAXP_PTE_X_PS) != OZ_SECTION_PAGESTATE_VALID_W) {
      oz_crash ("oz_hw_ldr_knlpage_setro: vpage %LX was not VALID_W (pte %QX)\n", svpage, pte);
    }
    if (((pte >> OZ_HWAXP_PTE_V_CP) & OZ_HWAXP_PTE_X_CP) != OZ_HW_PAGEPROT_KW) {
      oz_crash ("oz_hw_ldr_knlpage_setro: vpage %LX was not curprot KW (pte %QX)\n", svpage, pte);
    }
    if (((pte >> OZ_HWAXP_PTE_V_RP) & OZ_HWAXP_PTE_X_RP) != OZ_HW_PAGEPROT_KW) {
      oz_crash ("oz_hw_ldr_knlpage_setro: vpage %LX was not reqprot KW (pte %QX)\n", svpage, pte);
    }
    oz_hwaxp_l3ptbase[svpage++] = pte ^ OZ_HWAXP_PTE__KW ^ OZ_HWAXP_PTE__UR 							// change hardware protection bits
                                      ^ ((OZ_SECTION_PAGESTATE_VALID_W ^ OZ_SECTION_PAGESTATE_VALID_R) << OZ_HWAXP_PTE_V_PS) 	// change software state
                                      ^ ((OZ_HW_PAGEPROT_KW ^ OZ_HW_PAGEPROT_UR) << OZ_HWAXP_PTE_V_CP) 				// change software current protection
                                      ^ ((OZ_HW_PAGEPROT_KW ^ OZ_HW_PAGEPROT_UR) << OZ_HWAXP_PTE_V_RP);				// change software requested protection
  }

  /* Invalidate everything (including GBL pages) */

  OZ_HWAXP_MTPR_TBIA ();
}

/* Called when there is nothing to do.  In this version, we just exit */
/* and let whoever check again, since we have nothing better to do.   */

void oz_hw_cpu_waitint (void *waitq)

{
  oz_hw_cpu_setsoftint (1);
  oz_hw_cpu_setsoftint (0);
}
