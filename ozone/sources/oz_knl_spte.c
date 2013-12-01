//+++2001-10-06
//    Copyright (C) 2001, Mike Rieker, Beverly, MA USA
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
//---2001-10-06

#define _OZ_KNL_SPTE_C

#include "ozone.h"
#include "oz_knl_hw.h"
#include "oz_knl_sdata.h"
#include "oz_knl_spte.h"
#include "oz_knl_status.h"

/************************************************************************/
/*									*/
/*  Allocate some contiguous system page table entries			*/
/*									*/
/*    Input:								*/
/*									*/
/*	count = number of entries to allocate				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_spte_alloc = OZ_SUCCESS : successful			*/
/*	                OZ_NOFREESPTES : not enough spts available	*/
/*	*sysvaddr  = virt address mapped by that spte			*/
/*	*sysvpage  = virt page number mapped by that spte		*/
/*	*section_r = corresponding section				*/
/*	             (note: loader version returns NULL here)		*/
/*	smplock = softint delivery inhibited				*/
/*									*/
/*    Note:								*/
/*									*/
/*	This in essence just creates a demand-zero private section 	*/
/*	that is mapped to system address space and the system process.	*/
/*	All pages initially have the 'PAGEDOUT' state and are set to 	*/
/*	no-access.  Setting the reqprot to no-access will cause any 	*/
/*	pagefaults to abort.  The caller must therefore set the pages 	*/
/*	to the desired curprot and point to the desired phypage.	*/
/*									*/
/************************************************************************/

uLong oz_knl_spte_alloc (OZ_Mempage count, void **sysvaddr, OZ_Mempage *sysvpage, OZ_Section **section_r)

{
  uLong sts;
  OZ_Mempage npagem;
  OZ_Mempage basevpage, i, phypage, svpage;
  OZ_Section *section;
  OZ_Section_pagestate pagestate;
  void *systemprochwctx;

  basevpage = OZ_HW_VADDRTOVPAGE (oz_s_sysmem_baseva);					/* get virt page # of base of system area (what is mapped by spte[0]) */

  npagem = count;									/* get number of desired contiguous spt entries */
  svpage = basevpage;									/* this is some page number anywhere in system area */

  sts = oz_knl_section_create (NULL, npagem, 0, OZ_SECTION_TYPE_ZEROES, NULL, &section); /* create a private section for it */
  if (sts != OZ_SUCCESS) oz_crash ("oz_knl_spte_alloc: error %u creating section", sts); /* section must be private so caller can manipulate page states just by writing */
											/*  spte's - it gets mapped to system space so everyone can access it anyway */
  sts = oz_knl_process_mapsection (section, &npagem, &svpage, 0, OZ_PROCMODE_KNL, OZ_HW_PAGEPROT_NA); /* map that section to the system page table and the system process */
  if (section_r != NULL) *section_r = section;						/* maybe return section pointer to caller */
  else oz_knl_section_increfc (section, -1);						/* now that it is mapped, dec ref count so I don't have to keep track of pointer */
  if (sts == OZ_ADDRSPACEFULL) sts = OZ_NOFREESPTES;					/* change this status to say all spte's are being used */
  else if (sts != OZ_SUCCESS) {
    oz_crash ("oz_knl_spte_alloc: error %u mapping section", sts);			/* only other acceptable status is success, crash otherwise */
  } else {
    if (sysvaddr  != NULL) *sysvaddr = OZ_HW_VPAGETOVADDR (svpage);			/* return virt address of the area pointed to by that spte */
    if (sysvpage  != NULL) *sysvpage = svpage;						/* return virt page number of that area */
    for (i = 0; i < count; i ++) {
      oz_hw_pte_readsys (svpage + i, &pagestate, &phypage, NULL, NULL);			/* read the spte */
      if (pagestate != OZ_SECTION_PAGESTATE_PAGEDOUT) {					/* make sure it is not mapped to anything */
        oz_crash ("oz_knl_spte_alloc: vpage %x page state is %u", svpage + i, pagestate);
      }
    }
  }
  return (sts);
}

/************************************************************************/
/*									*/
/*  Free those contiguous system page table entries			*/
/*									*/
/*    Input:								*/
/*									*/
/*	count = number of pages that were allocated			*/
/*	svpage = starting system virtual page number that was allocd	*/
/*									*/
/************************************************************************/

void oz_knl_spte_free (OZ_Mempage count, OZ_Mempage svpage)

{
  OZ_Hw_pageprot reqprot;
  OZ_Mempage i;
  uLong sts;

  /* Mark the pages as paged out so unmapsec won't do anything with them.  Also check them to be sure the reqprot is still NA.  This is how we check to see if the */
  /* pager might have some state in there, because the pager will never do anything with a page that has reqprot NA (it will always return an OZ_ACCVIO status).   */

  for (i = 0; i < count; i ++) {
    oz_hw_pte_readsys (svpage + i, NULL, NULL, NULL, &reqprot);
    if (reqprot != OZ_HW_PAGEPROT_NA) oz_crash ("oz_knl_spte_free: svpage %x reqprot is %d", svpage + i, reqprot);
    oz_hw_pte_writeall (svpage + i, OZ_SECTION_PAGESTATE_PAGEDOUT, OZ_PHYPAGE_NULL, OZ_HW_PAGEPROT_NA, OZ_HW_PAGEPROT_NA);
  }

  /* Now unmap the section */

  sts = oz_knl_process_unmapsec (svpage);
  if (sts != OZ_SUCCESS) oz_crash ("oz_knl_spte_free: error %u freeing %u pages at %x", sts, count, svpage);
}
