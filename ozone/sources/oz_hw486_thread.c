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
/*  Hardware thread routines for 486's					*/
/*  These routines are called by oz_kernel_486.s routines		*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_knl_hw.h"
#include "oz_knl_process.h"
#include "oz_knl_sdata.h"
#include "oz_knl_section.h"
#include "oz_knl_status.h"

#define kstacksize oz_s_loadparams.kernel_stack_size

#define KSTACKGUARDS 1	/* number of guard pages below stack */


/************************************************************************/
/*									*/
/*  This routine creates a kernel stack					*/
/*									*/
/*    Input:								*/
/*									*/
/*	smplock = softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_hw486_kstack_create = OZ_SUCCESS : successful		*/
/*	                               else : error status		*/
/*	*sysvaddr_r = initial stack pointer value			*/
/*									*/
/************************************************************************/

uLong oz_hw486_kstack_create (void *thctx, void **sysvaddr_r)

{
#if OZ_HW_KSTACKINTHCTX
  *sysvaddr_r  = thctx + OZ_THREAD_HW_CTX_SIZE;		// kernel stack on end of hardware context block
  memset (*sysvaddr_r, 0x69, kstacksize);		// garbage fill it to start with
  *sysvaddr_r += kstacksize;				// i386 stacks grow downward
  return (OZ_SUCCESS);					// always successful
#else
  OZ_Mempage i, npages, phypage, sysvpage;
  OZ_Quota *quota;
  OZ_Section *section;
  OZ_Section_pagestate pagestate;
  uLong pm, sts;
  void *sysvaddr;

  /* The kstacksize parameter should be a multiple of the page size */

  if (kstacksize & ((1 << OZ_HW_L2PAGESIZE) - 1)) oz_crash ("oz_hw_kstack_create: kstacksize %u not multiple of page size", kstacksize);
  npages = kstacksize >> OZ_HW_L2PAGESIZE;

  /* Allocate some spte's for the stack - this gives the stack its virtual address */
  /* Allocate extra pages for guard pages                                          */

  sts = oz_knl_spte_alloc (npages + KSTACKGUARDS, &sysvaddr, &sysvpage, &section);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_hw_kstack_create: error %u allocating %u sptes for kernel stack\n", sts, npages + KSTACKGUARDS);
    return (sts);
  }

  /* Make sure caller has enough physical memory quota left */

  quota = oz_knl_section_getquota (section);
  if (!oz_knl_quota_debit (quota, OZ_QUOTATYPE_PHM, npages)) {
    oz_knl_printk ("oz_hw_kstack_create: no phymem quota for %u kernel stack pages\n", npages);
    oz_knl_section_increfc (section, -1);
    return (OZ_EXQUOTAPHM);
  }

  /* Write the lowest NGUARD pages with a no-access page - set VALID_W just to match the others */
  /* Kernel should crash if it tries to write to this page (overflows the stack)                */

  for (i = 0; i < KSTACKGUARDS; i ++) {
    oz_hw_pte_writeall (sysvpage + i, OZ_SECTION_PAGESTATE_VALID_W, OZ_PHYPAGE_NULL, OZ_HW_PAGEPROT_NA, OZ_HW_PAGEPROT_NA);
  }

  /* Fill the rest of the spte's with genuine physical pages and set them to kernel read/write access */

  pm = oz_hw_smplock_wait (&oz_s_smplock_pm);
  for (; i < npages + KSTACKGUARDS; i ++) {
    phypage = oz_knl_phymem_allocpage (OZ_PHYMEM_PAGESTATE_ALLOCSECT, sysvpage + i);
    if (phypage == OZ_PHYPAGE_NULL) goto nomemory;
    oz_hw_pte_writeall (sysvpage + i, OZ_SECTION_PAGESTATE_VALID_W, phypage, OZ_HW_PAGEPROT_KW, OZ_HW_PAGEPROT_NA);
  }
  oz_hw_smplock_clr (&oz_s_smplock_pm, pm);

  /* Finish up */

  oz_knl_section_increfc (section, -1);					/* get rid of that stupid section pointer */
  (OZ_Pointer)sysvaddr += KSTACKGUARDS << OZ_HW_L2PAGESIZE;		/* point to lowest address that has pages */
  memset (sysvaddr, 0x69, kstacksize);					/* fill with garbage */
  *sysvaddr_r = (void *)((OZ_Pointer)sysvaddr + kstacksize);		/* return pointer to end of stack area */
  return (OZ_SUCCESS);							/* success */

  /* Ran out of physical pages */

nomemory:
  oz_knl_printk ("oz_hw_kstack_create: no physical memory for kernel stack\n");
  while (-- i >= KSTACKGUARDS) {					/* free off any we did so far */
    oz_hw_pte_readsys (sysvpage + i, &pagestate, &phypage, NULL, NULL);
    if (pagestate != OZ_SECTION_PAGESTATE_VALID_W) {
      oz_crash ("oz_hw_kstack_create: kstack vpage %x pagestate %u", OZ_HW_VADDRTOVPAGE (sysvaddr) + i, pagestate);
    }
    oz_knl_phymem_freepage (phypage);
  }
  oz_hw_smplock_clr (&oz_s_smplock_pm, pm);				/* restore smp level */
  oz_knl_quota_credit (quota, OZ_QUOTATYPE_PHM, npages, -1);		/* credit back the pages */
  oz_knl_spte_free (npages + KSTACKGUARDS, sysvpage);			/* release the spte's */
  oz_knl_section_increfc (section, -1);					/* get rid of the section */
  return (OZ_NOMEMORY);							/* return error status */
#endif
}

/************************************************************************/
/*									*/
/*  This routine deletes a kernel stack					*/
/*									*/
/*    Input:								*/
/*									*/
/*	sysvaddr = as returned by oz_hw_kstack_create			*/
/*	           (which is just beyond highest address)		*/
/*	smplock  = softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	kernel stack no longer usable					*/
/*									*/
/************************************************************************/

void oz_hw486_kstack_delete (void *thctx, void *sysvaddr)

{
  OZ_Hw_pageprot pageprot;
  OZ_Mempage i, npages, pageoffs, phypage, svpage;
  OZ_Procmode procmode;
  OZ_Quota *quota;
  OZ_Section *section;
  OZ_Section_pagestate pagestate;
  uLong depth, mapsecflags, pm, *scan, sts;

  static uLong oldepth = 0;

  scan = (uLong *)(((uByte *)sysvaddr) - kstacksize);				/* point to lowest usable address on stack */
  for (depth = kstacksize - oldepth; depth > 0; depth -= 4) {			/* scan it */
    if (*(scan ++) != 0x69696969) break;					/* stop if it has been used for anything */
  }
  if (depth > 0) {
    oldepth += depth;
    oz_knl_printk ("oz_hw_kstack_delete*: kernel stack depth %u (0x%x)\n", oldepth, oldepth);
  }

#if !OZ_HW_KSTACKINTHCTX
  npages = kstacksize >> OZ_HW_L2PAGESIZE;					/* get number of pages, not including guard pages */
  svpage = OZ_HW_VADDRTOVPAGE (sysvaddr) - npages - KSTACKGUARDS;		/* get system page number of first guard page */

  /* Make sure the guard pages are still guard pages and mark them paged out */

  for (i = 0; i < KSTACKGUARDS; i ++) {
    oz_hw_pte_readsys (svpage + i, &pagestate, &phypage, NULL, NULL);
    if ((pagestate != OZ_SECTION_PAGESTATE_VALID_W) || (phypage != OZ_PHYPAGE_NULL)) {
      oz_crash ("oz_hw_kstack_delete: kernel stack guard vpage %x has pagestate %d, phypage %u", svpage + i, pagestate, phypage);
    }
    oz_hw_pte_writeall (svpage + i, OZ_SECTION_PAGESTATE_PAGEDOUT, OZ_PHYPAGE_NULL, OZ_HW_PAGEPROT_NA, OZ_HW_PAGEPROT_NA);
  }

  /* Decrement the I/O ref count on all the pages and free the pages off.  Hopefully */
  /* the count is zero, kernel routines shouldn't leave I/O going on their stacks.   */

  pm = oz_hw_smplock_wait (&oz_s_smplock_pm);
  for (; i < npages + KSTACKGUARDS; i ++) {
    oz_hw_pte_readsys (svpage + i, &pagestate, &phypage, NULL, NULL);
    if (pagestate != OZ_SECTION_PAGESTATE_VALID_W) {
      oz_crash ("oz_hw_kstack_delete: kernel stack vpage %x has pagestate %d, phypage %u", svpage + i, pagestate, phypage);
    }
    oz_hw_pte_writeall (svpage + i, OZ_SECTION_PAGESTATE_PAGEDOUT, OZ_PHYPAGE_NULL, OZ_HW_PAGEPROT_NA, OZ_HW_PAGEPROT_NA);
    oz_knl_phymem_freepage (phypage);
  }
  oz_hw_smplock_clr (&oz_s_smplock_pm, pm);

  /* Credit the quota back for the pages */

  if (!oz_knl_process_getsecfromvpage (oz_s_systemproc, svpage, &section, &pageoffs, &pageprot, &procmode, &mapsecflags)) {
    oz_crash ("oz_hw_kstack_delete: cannot find stack section vpage %x", svpage);
  }
  quota = oz_knl_section_getquota (section);
  oz_knl_quota_credit (quota, OZ_QUOTATYPE_PHM, npages, -1);

  /* Unmap the section */

  sts = oz_knl_process_unmapsec (svpage);
  if (sts != OZ_SUCCESS) oz_crash ("oz_hw_kstack_delete: error %u unmapping kernel stack section", sts);
#endif
}

/************************************************************************/
/*									*/
/*  Create and map an user mode stack section for the current thread 	*/
/*									*/
/*    Input:								*/
/*									*/
/*	stacksizeinpages = stack size in pages				*/
/*	smplevel = softints enabled					*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_hw486_ustack_create = initial value for user stack pointer	*/
/*	*stackvirtpage = base virt page of user stack			*/
/*									*/
/************************************************************************/

void *oz_hw486_ustack_create (OZ_Mempage stacksizeinpages, OZ_Mempage *stackvirtpage)

{
  int si;
  OZ_Section *section;
  uLong sts;
  void *stackvirtaddr;

  si = oz_hw_cpu_setsoftint (0);

  /* Create a section of 'zeroes' */

  sts = oz_knl_section_create (NULL, stacksizeinpages, 0, OZ_SECTION_TYPE_ZEROES, NULL, &section);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_hw486_ustack_create: error %u creating user stack section\n", sts);
    oz_hw_cpu_setsoftint (si);
    oz_knl_thread_exit (sts);
  }

  /* Map it to the highest available address (let usermode unmap it if it wants to) */

  *stackvirtpage = OZ_HW_VADDRTOVPAGE (OZ_HW_DVA_USRSTACK_VA);
  sts = oz_knl_process_mapsection (section, &stacksizeinpages, stackvirtpage, OZ_HW_DVA_USRSTACK_AT, OZ_PROCMODE_USR, OZ_HW_PAGEPROT_UW);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_hw486_ustack_create: error %u mapping user stack section\n", sts);
    oz_knl_section_increfc (section, -1);
    oz_hw_cpu_setsoftint (si);
    oz_knl_thread_exit (sts);
  }

  /* Get rid of section pointer so just umapping the section will delete it */

  oz_knl_section_increfc (section, -1);

  /* Return initial stack pointer = very end of the section */

  oz_hw_cpu_setsoftint (si);
  stackvirtaddr = OZ_HW_VPAGETOVADDR (stacksizeinpages + *stackvirtpage);
  return (stackvirtaddr);
}

/************************************************************************/
/*									*/
/*  Thread has exited, delete the user stack				*/
/*									*/
/************************************************************************/

void oz_hw486_ustack_delete (OZ_Mempage stackvirtpage)

{
  oz_knl_process_unmapsec (stackvirtpage);
}
