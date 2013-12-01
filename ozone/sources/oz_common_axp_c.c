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

#include "ozone.h"
#include "oz_knl_hw.h"
#include "oz_knl_sdata.h"
#include "oz_knl_section.h"
#include "oz_knl_status.h"

#define LOG2PAGESIZE (OZ_HW_L2PAGESIZE)
#define PAGESIZE (1 << LOG2PAGESIZE)
#define PAGEMASK (PAGESIZE - 1)
#define LOG2PTESPERPAGE (LOG2PAGESIZE - 3)
#define PTESPERPAGE (1 << LOG2PTESPERPAGE)

#define PTE__NA OZ_HWAXP_PTE__NA
#define PTE__KR OZ_HWAXP_PTE__KR
#define PTE__KW OZ_HWAXP_PTE__KW
#define PTE__UR OZ_HWAXP_PTE__UR
#define PTE__UW OZ_HWAXP_PTE__UW

#define PTE_M_GBL OZ_HWAXP_PTE_M_GBL
#define PTE_V_GH OZ_HWAXP_PTE_V_GH

#define PTE_V_PS OZ_HWAXP_PTE_V_PS
#define PTE_X_PS OZ_HWAXP_PTE_X_PS
#define PTE_V_CP OZ_HWAXP_PTE_V_CP
#define PTE_X_CP OZ_HWAXP_PTE_X_CP
#define PTE_V_RP OZ_HWAXP_PTE_V_RP
#define PTE_X_RP OZ_HWAXP_PTE_X_RP
#define PTE_V_PP OZ_HWAXP_PTE_V_PP
#define PTE_X_PP OZ_HWAXP_PTE_X_PP

typedef struct { uQuad checksum;		// checksum of the rest of the struct, 
						// not including 'checksum' but including 
						// all elements of 'clusters' array
                 uQuad impdatapa;		// phyaddr of implementation specific data
                 uQuad ncluster;		// number of elements in 'clusters' array
                 struct { uQuad startpage;	// starting physical page number
                          uQuad numpages;	// number of physical pages
                          uQuad testedpages;	// number of tested pages
                          uQuad bitmap_va;	// bitmap's virtual address
                          uQuad bitmap_pa;	// bitmap's physical address
                          uQuad bitmap_cs;	// bitmap's checksum
                          uQuad memusage;	// 0: normal memory available for system use
						// 1: reserved for console use
						// 2: non-volatile memory available for system use
                        } clusters[1];
               } Mcdt;

uLong const oz_ALIGNMENT   = OZ_ALIGNMENT;		// needed by .s modules
uLong const oz_ARITHOVER   = OZ_ARITHOVER;
uLong const oz_BADPARAM    = OZ_BADPARAM;
uLong const oz_BADSYSCALL  = OZ_BADSYSCALL;
uLong const oz_BREAKPOINT  = OZ_BREAKPOINT;
uLong const oz_BUGCHECK    = OZ_BUGCHECK;
uLong const oz_FLOATPOINT  = OZ_FLOATPOINT;
uLong const oz_RESIGNAL    = OZ_RESIGNAL;
uLong const oz_RESUME      = OZ_RESUME;
uLong const oz_SUCCESS     = OZ_SUCCESS;
uLong const oz_UNDEFOPCODE = OZ_UNDEFOPCODE;

OZ_Hwaxp_Cpudb oz_hwaxp_cpudb[OZ_HW_MAXCPUS];		// per-cpu data
OZ_Mempage oz_hwaxp_sysl1ptpp;				// system level 1 pagetable's physical page number
OZ_Pagentry *oz_hwaxp_l1ptbase;				// virt address of L1 pagetable's base (maps vaddr 0)
OZ_Pagentry *oz_hwaxp_l2ptbase;				// virt address of L2 pagetable's base (maps vaddr 0)
OZ_Pagentry *oz_hwaxp_l3ptbase;				// virt address of L3 pagetable's base (maps vaddr 0)
OZ_Hwaxp_Hwrpb *oz_hwaxp_hwrpb;				// points to HWRPB set up by console before booting
uQuad oz_hwaxp_dispatchent, oz_hwaxp_dispatchr27;	// console dispatch linkage pair
OZ_Mempage oz_hwaxp_botphypage, oz_hwaxp_topphypage;	// bottom (incl) & top (excl) usable physical page numbers

static OZ_Mempage l3ptbasepage;				// base vpage of L3 pt

void oz_hwaxp_common_init (OZ_Hwaxp_Hwrpb *hwrpbva, uQuad *phymem_count_r, uQuad *phymem_start_r, uLong *maxcpus_r)

{
  int i, j;
  Mcdt *mcdt;
  OZ_Hwaxp_Hwpcb *hwcpu;
  OZ_Pointer hwpcbva;
  uLong selfrefl1idx;
  uQuad hwpcbpa, phymem_count, phymem_start, vptb;

  /* Fill in some global pointers */

  OZ_HWAXP_MTPR_DATFX (1);						// inhibit datfx (unaligned reference) reporting

  oz_hwaxp_hwrpb = hwrpbva;

  oz_hwaxp_dispatchr27 = ((uQuad *)(((uQuad)hwrpbva) + hwrpbva -> ccrboffs))[0];	// pd is first quad of the ccr
  oz_hwaxp_dispatchent = ((uQuad *)(oz_hwaxp_dispatchr27))[1];				// entry is 2nd quad of the pd

  oz_hwaxp_sysl1ptpp = OZ_HWAXP_MFPR_PTBR ();				// system level 1 pagetable's physical page number
  vptb = OZ_HWAXP_MFPR_VPTB ();						// base virtual address of L3 pagetable
  if (vptb & ((1ULL << (LOG2PAGESIZE + 2*LOG2PTESPERPAGE)) - 1)) oz_crash ("oz_hwaxp_common_init: invalid VPTB %QX", vptb);
  selfrefl1idx = OZ_HW_VADDRTOVPAGE (vptb) >> (2 * LOG2PTESPERPAGE);	// index in L1 page of self-reference

  oz_hwaxp_l3ptbase = (uQuad *)vptb;					// save L3 pagetable base virtual address
  vptb += ((uQuad)selfrefl1idx) << (LOG2PAGESIZE + LOG2PTESPERPAGE);	// for every 8GB, increment by 8MB
  oz_hwaxp_l2ptbase = (uQuad *)vptb;					// save L2 pagetable base virtual address
  vptb += ((uQuad)selfrefl1idx) << LOG2PAGESIZE;			// for every 8GB, increment by 8KB
  oz_hwaxp_l1ptbase = (uQuad *)vptb;					// save L1 pagetable base virtual address

  if ((oz_hwaxp_l1ptbase[selfrefl1idx] >> 32) != ((uQuad)oz_hwaxp_sysl1ptpp) & 0xFFFFFFFFULL) {	// make sure it came out right
    register uQuad __r16 asm ("$16") = 0x1629;
    register uQuad __r17 asm ("$17") = selfrefl1idx;
    register uQuad __r18 asm ("$18") = (uQuad)oz_hwaxp_l1ptbase;
    register uQuad __r19 asm ("$19") = oz_hwaxp_sysl1ptpp;
    register uQuad __r20 asm ("$20") = oz_hwaxp_l1ptbase[selfrefl1idx];

    asm volatile ("call_pal 0 # HALT" : : "r"(__r16), "r"(__r17), "r"(__r18), "r"(__r19), "r"(__r20));
  }

  l3ptbasepage = OZ_HW_VADDRTOVPAGE (oz_hwaxp_l3ptbase);

  if ((vptb >> 63) == 0) oz_hwaxp_sysbasva = 0;				// for loader, everything is a system address
  else oz_hwaxp_sysbasva = vptb + PAGESIZE;				// for kernel, everything after L1 page is system address

  /* Initialize CPUDB */

  memset (oz_hwaxp_cpudb, 0, sizeof oz_hwaxp_cpudb);

  hwcpu = (OZ_Hwaxp_Hwpcb *)(oz_hwaxp_hwrpb -> cpusltofs + (OZ_Pointer)oz_hwaxp_hwrpb);

  for (i = 0; (i < OZ_HW_MAXCPUS) && (i < oz_hwaxp_hwrpb -> numprcslt); i ++) {	// loop through elements

    /* Set up pointers for our two HWPCB's per CPU */

    hwpcbva = (((OZ_Pointer)oz_hwaxp_cpudb[i].db.hwpcb) + 127) & -128;		// calc 128-boundary hwpcb virt address
    for (j = 0; j < 2; j ++) {
      hwpcbpa = ((oz_hwaxp_l3ptbase[OZ_HW_VADDRTOVPAGE(hwpcbva)] >> OZ_HWAXP_PTE_V_PP) * PAGESIZE) + (hwpcbva & PAGEMASK);
      oz_hwaxp_cpudb[i].db.hwpcb_va[j] = (OZ_Hwaxp_Hwpcb *)hwpcbva;		// save virtual address
      oz_hwaxp_cpudb[i].db.hwpcb_pa[j] = hwpcbpa;				// save physical address
      hwpcbva = (hwpcbva + sizeof (OZ_Hwaxp_Hwpcb) + 127) & -128;		// increment to next hwpcb slot
    }

    /* When we get to this CPU's slot, swap it to the active [0] HWPCB.  It would be nice to have */
    /* HWPCB[0] just be the HWPCB in the HWRPB's slot for this CPU.  However, in the case of the  */
    /* Miata, the slots are not aligned, and the SWPCTX pukes switching back to it.  Idiot thing. */

    if (i == oz_hw_cpu_getcur ()) {
      OZ_Hwaxp_Hwpcb *newpcb;

      newpcb = oz_hwaxp_cpudb[i].db.hwpcb_va[0];
      asm volatile ("stq $sp,%0" : "=m" (newpcb -> ksp));	// KSP won't change
								// ESP/SSP/USP get zeroed, big deal
      newpcb -> ptbr = oz_hwaxp_sysl1ptpp;			// PTBR won't change
								// ASN gets zeroed, big deal
      newpcb -> fen  = hwcpu -> fen;				// FEN stuff won't change
								// UNIQ gets zeroed, big deal
      OZ_HWAXP_SWPCTX (oz_hwaxp_cpudb[i].db.hwpcb_pa[0]);	// switch to new *ALIGNED* PCB
    }

    /* Increment pointer to next (moronically unaligned) Hwcpu slot */

    hwcpu = (OZ_Hwaxp_Hwpcb *)(oz_hwaxp_hwrpb -> cpusltsiz + (OZ_Pointer)hwcpu);
  }

  *maxcpus_r = i;

  /* Now we can initialize our SCB */

  oz_hwaxp_scb_init ();

  /* Now the oz_knl_printk routine should work */

  oz_knl_printk ("oz_hwaxp_common_init: L1 pt base va %QX\n", oz_hwaxp_l1ptbase);
  oz_knl_printk ("oz_hwaxp_common_init: L2 pt base va %QX\n", oz_hwaxp_l2ptbase);
  oz_knl_printk ("oz_hwaxp_common_init: L3 pt base va %QX\n", oz_hwaxp_l3ptbase);

  if (oz_hwaxp_hwrpb -> pagesize != PAGESIZE) {
    oz_crash ("oz_hwaxp_common_init: oz_hw_axp.h pagesize %X .ne. hwrpb pagesize %X", PAGESIZE, oz_hwaxp_hwrpb -> pagesize);
  }

  /* Test the default SCB handlers by doing a breakpoint */

  // OZ_HWAXP_BPT ();

  /* Determine range of usable physical pages                               */
  /* We do this by finding the largest cluster of free pages in hwrpb table */

  mcdt = (Mcdt *)(((uQuad)hwrpbva) + hwrpbva -> mddtoffs);

  phymem_count = 0;
  phymem_start = 0;
  for (i = 0; i < mcdt -> ncluster; i ++) {
    oz_knl_printk ("oz_hwaxp_common_init: physical memory: %QX pages at %QX, usage %QX\n", 
	mcdt -> clusters[i].numpages, mcdt -> clusters[i].startpage, mcdt -> clusters[i].memusage);
    if (mcdt -> clusters[i].memusage == 0) {
      if (mcdt -> clusters[i].startpage == phymem_start + phymem_count) phymem_count += mcdt -> clusters[i].numpages;
      else if (mcdt -> clusters[i].numpages > phymem_count) {
        phymem_count = mcdt -> clusters[i].numpages;
        phymem_start = mcdt -> clusters[i].startpage;
      }
    }
  }
  oz_knl_printk ("oz_hwaxp_common_init: largest usable block: %QX pages at %QX\n", phymem_count, phymem_start);

  *phymem_count_r = phymem_count;
  *phymem_start_r = phymem_start;

  oz_hwaxp_botphypage = phymem_start;
  oz_hwaxp_topphypage = phymem_count + phymem_start;
}

/************************************************************************/
/*									*/
/*  Initialize non-paged pool area					*/
/*									*/
/*    Input:								*/
/*									*/
/*	*pages_required = minimum number of pages required		*/
/*									*/
/*    Output:								*/
/*									*/
/*	*pages_required = actual pages set up				*/
/*	*first_physpage = first physical page number used		*/
/*	*first_virtpage = first virtual page number used		*/
/*									*/
/************************************************************************/

void oz_hw_pool_init (OZ_Mempage *pages_required, OZ_Mempage *first_physpage, OZ_Mempage *first_virtpage)

{
  int gh;
  OZ_Mempage firstphyspage, firstvirtpage, freephyspages, i, l1idx, l2idx, l3idx, pagespergran, pagesrequired;

  pagesrequired = *pages_required;

       if (pagesrequired >= 256) gh = 3;
  else if (pagesrequired >=  32) gh = 2;
  else if (pagesrequired >=   4) gh = 1;
  else gh = 0;

  pagespergran    = 1 << (gh * 3);

  pagesrequired   = (pagesrequired + pagespergran - 1) & -pagespergran;
  *pages_required = pagesrequired;

  /* Use physical pages on the very high end of what's available */

  firstphyspage   = (oz_s_phymem_totalpages - pagesrequired) & -pagespergran;
  *first_physpage = firstphyspage;

  /* Use the lowest aligned virtual address block following the kernel */

  firstvirtpage  = OZ_HW_VADDRTOVPAGE (&oz_hwaxp_sysl1ptpp);
  firstvirtpage +=   pagespergran - 1;
  firstvirtpage &= ~(pagespergran - 1);
  for (;; firstvirtpage += pagespergran) {
    for (i = 0; i < pagesrequired; i ++) {
      l1idx = (firstvirtpage + i) >> (2 * LOG2PTESPERPAGE);
      l2idx = (firstvirtpage + i) >> LOG2PTESPERPAGE;
      l3idx =  firstvirtpage + i;
      if ((oz_hwaxp_l1ptbase[l1idx] & 1) 
       && (oz_hwaxp_l2ptbase[l2idx] & 1) 
       && (oz_hwaxp_l3ptbase[l3idx] & 1)) break;
    }
    if (i == pagesrequired) break;
  }

  *first_virtpage = firstvirtpage;

  oz_knl_printk ("oz_hw_pool_init: 0x%X pages, ppage 0x%X, vpage 0x%X\n", pagesrequired, firstphyspage, firstvirtpage);

  /* Fill in the pte's */

  freephyspages = firstphyspage;
  for (i = 0; i < pagesrequired; i ++) {
    l1idx = (firstvirtpage + i) >> (2 * LOG2PTESPERPAGE);
    l2idx = (firstvirtpage + i) >> LOG2PTESPERPAGE;
    l3idx =  firstvirtpage + i;
    if (!(oz_hwaxp_l1ptbase[l1idx] & 1)) {
      oz_hwaxp_l1ptbase[l1idx] = (((uQuad)(-- freephyspages)) << PTE_V_PP) + OZ_HWAXP_PTE_GKW;
      memset (oz_hwaxp_l2ptbase + (l1idx << LOG2PTESPERPAGE), 0, PAGESIZE);
    }
    if (!(oz_hwaxp_l2ptbase[l2idx] & 1)) {
      oz_hwaxp_l2ptbase[l2idx] = (((uQuad)(-- freephyspages)) << PTE_V_PP) + OZ_HWAXP_PTE_GKW;
      memset (oz_hwaxp_l3ptbase + (l2idx << LOG2PTESPERPAGE), 0, PAGESIZE);
    }
    oz_hwaxp_l3ptbase[l3idx] = (((uQuad)(firstphyspage + i)) << PTE_V_PP) + (gh << PTE_V_GH) + OZ_HWAXP_PTE_GKW;
  }
}

/************************************************************************/
/*									*/
/*  Initialize a physical page to known values				*/
/*									*/
/*    Input:								*/
/*									*/
/*	phypage = physical page number					*/
/*	ptvirtpage = 0 : normal data page, fill with zeroes		*/
/*	          else : will be mapped as this particular virtual 	*/
/*	                 page in current process as a pagetable page	*/
/*	                 - initialize as a page of pte's		*/
/*									*/
/*    Output:								*/
/*									*/
/*	physical page phypage initialized				*/
/*									*/
/************************************************************************/

void oz_hw_phys_initpage (OZ_Mempage phypage, OZ_Mempage ptvirtpage)

{
  OZ_Hwaxp_Cpudb *cpudb;
  OZ_Pagentry savepte;

  /* Get page mapped to a virtual address so we can access it */

  cpudb = oz_hwaxp_cpudb + oz_hw_cpu_getcur ();
  savepte = cpudb -> db.tempspte[0];					// save what is in the spte
  cpudb -> db.tempspte[0] = (((uQuad)(phypage)) << PTE_V_PP) + PTE__KW;	// map the physical page
  OZ_HWAXP_MTPR_TBIS (cpudb -> db.tempspva);

  /* Zero fill the page */

  asm volatile ("\n"
	"	.p2align 4\n"
	"1:\n"
	"	wh64	(%1)\n"
	"	stq	$31, 0-256(%1)\n"
	"	stq	$31, 8-256(%1)\n"
	"	stq	$31,16-256(%1)\n"
	"	stq	$31,24-256(%1)\n"
	"	stq	$31,32-256(%1)\n"
	"	stq	$31,40-256(%1)\n"
	"	stq	$31,48-256(%1)\n"
	"	stq	$31,56-256(%1)\n"
	"	subq	%0,1,%0\n"
	"	addq	%1,64,%1\n"
	"	bne	%0,1b\n"
	"	mov	4,%0\n"
	"	.p2align 4\n"
	"2:\n"
	"	stq	$31, 0-256(%1)\n"
	"	stq	$31, 8-256(%1)\n"
	"	stq	$31,16-256(%1)\n"
	"	stq	$31,24-256(%1)\n"
	"	stq	$31,32-256(%1)\n"
	"	stq	$31,40-256(%1)\n"
	"	stq	$31,48-256(%1)\n"
	"	stq	$31,56-256(%1)\n"
	"	subq	%0,1,%0\n"
	"	addq	%1,64,%1\n"
	"	bne	%0,2b"
	: : "r"((PAGESIZE-256)/64), "r"(cpudb -> db.tempspva + 256) : "0", "1", "memory");

  /* If this is a pagetable page, get initial 'reqprot' values from any sections that are mapped to it */

  if (ptvirtpage != 0) {
    OZ_Hw_pageprot pageprot;
    OZ_Mempage datavirtpage, i, pageoffs, npages, svpage;
    OZ_Process *process;
    OZ_Procmode procmode;
    OZ_Section *section;
    uLong mapsecflags;

    /* Get datapage mapped by first entry in this pagetable page */

    if (ptvirtpage < l3ptbasepage) oz_crash ("oz_hw_phys_initpage: ptvirtpage %X lt l3ptbasepage %X", ptvirtpage, l3ptbasepage);
    datavirtpage = (ptvirtpage - l3ptbasepage) * PTESPERPAGE;
    if (datavirtpage >= (1 << OZ_HWAXP_L2VPSIZE)) oz_crash ("oz_hw_phys_initpage: ptvirtpage %X too big (data vp %X)", ptvirtpage, datavirtpage);

    /* Get process for the data page */

    process = oz_s_systemproc;
    if (!OZ_HW_ISSYSPAGE (datavirtpage)) process = oz_knl_process_getcur ();

    /* Loop through all entries in the pagetable page */

    for (i = 0; i < PTESPERPAGE;) {

      /* See if there is a section mapped at this point or after */

      svpage = datavirtpage + i;
      npages = oz_knl_process_getsecfromvpage2 (process, &svpage, &section, &pageoffs, &pageprot, &procmode, &mapsecflags);
      if (npages == 0) break;
      i = svpage - datavirtpage;
      if (i >= PTESPERPAGE) break;

      /* Ok, fill with initial mapped protection */

      while ((npages > 0) && (i < PTESPERPAGE)) {
        -- npages;
        ((uQuad *)(cpudb -> db.tempspva))[i++] = pageprot << PTE_V_RP;
      }
    }
  }

  /* Release temp mapping */

  cpudb -> db.tempspte[0] = savepte;					// restore saved spte
  OZ_HWAXP_MTPR_TBIS (cpudb -> db.tempspva);
}

/************************************************************************/
/*									*/
/*  Map a physical page to virtual address				*/
/*									*/
/*    Input:								*/
/*									*/
/*	phypage = physical page to map to virtual address		*/
/*	smplevel >= softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_hw_phys_mappage = virtual address the page is mapped to	*/
/*	*savepte = old pte contents for that virtual address		*/
/*									*/
/*    Note:								*/
/*									*/
/*	You must not call any of the other physical page accessing 	*/
/*	routines to copy to/from the virtual address returned by this 	*/
/*	routine.  Use oz_hw_phys_movephys instead.  This is because 	*/
/*	those other routines temporarily destroy the mapping made by 	*/
/*	this routine while they are accessing their physical page(s).	*/
/*									*/
/************************************************************************/

void *oz_hw_phys_mappage (OZ_Mempage phypage, OZ_Pagentry *savepte)

{
  OZ_Hwaxp_Cpudb *cpudb;

  cpudb = oz_hwaxp_cpudb + oz_hw_cpu_getcur ();
  if (savepte != NULL) *savepte = cpudb -> db.tempspte[0];		// save what is in the spte
  cpudb -> db.tempspte[0] = (((uQuad)(phypage)) << PTE_V_PP) + PTE__KW;	// map the physical page
  OZ_HWAXP_MTPR_TBIS (cpudb -> db.tempspva);
  return (cpudb -> db.tempspva);
}

/************************************************************************/
/*									*/
/*  Unmap the page mapped by oz_hw_phys_mappage				*/
/*									*/
/*    Input:								*/
/*									*/
/*	savepte = as returned by oz_hw_phys_mappage			*/
/*									*/
/************************************************************************/

void oz_hw_phys_unmappage (OZ_Pagentry savepte)

{
  OZ_Hwaxp_Cpudb *cpudb;

  cpudb = oz_hwaxp_cpudb + oz_hw_cpu_getcur ();
  cpudb -> db.tempspte[0] = savepte;
  OZ_HWAXP_MTPR_TBIS (cpudb -> db.tempspva);
}

/************************************************************************/
/*									*/
/*  Copy data from virtual address to physical addresses		*/
/*									*/
/*    Input:								*/
/*									*/
/*	nbytes   = number of bytes to copy				*/
/*	vaddr    = virtual address to copy from				*/
/*	phypages = address of physical page numbers to copy to		*/
/*	byteoffs = byte offset in first physical page			*/
/*									*/
/*    Output:								*/
/*									*/
/*	bytes copied to the physical pages				*/
/*									*/
/************************************************************************/

void oz_hw_phys_movefromvirt (uLong nbytes, void const *vaddr, OZ_Mempage const *phypages, uLong byteoffs)

{
  OZ_Hwaxp_Cpudb *cpudb;
  OZ_Pagentry savepte;
  uLong length;

  cpudb = oz_hwaxp_cpudb + oz_hw_cpu_getcur ();

  savepte = cpudb -> db.tempspte[0];					// save what is in the spte
  while (nbytes > 0) {							// repeat until done
    phypages += byteoffs >> LOG2PAGESIZE;				// normalize byteoffs to a page
    byteoffs &= PAGEMASK;
    cpudb -> db.tempspte[0] = (((uQuad)(phypages[0])) << PTE_V_PP) + PTE__KW; // map the physical page
    OZ_HWAXP_MTPR_TBIS (cpudb -> db.tempspva);
    length = PAGESIZE - byteoffs;					// get bytes to end of physical page
    if (length > nbytes) length = nbytes;				// but only do as much as we have left
    memcpy (cpudb -> db.tempspva + byteoffs, vaddr, length);		// copy the data
    nbytes -= length;							// this much less left to do
    (OZ_Pointer)vaddr += length;					// increment input pointer
    byteoffs += length;							// increment output offset
  }
  cpudb -> db.tempspte[0] = savepte;					// restore saved spte
  OZ_HWAXP_MTPR_TBIS (cpudb -> db.tempspva);
}

/************************************************************************/
/*									*/
/*  Copy data from physical address to virtual addresses		*/
/*									*/
/*    Input:								*/
/*									*/
/*	nbytes   = number of bytes to copy				*/
/*	vaddr    = virtual address to copy to				*/
/*	phypages = address of physical page numbers to copy from	*/
/*	byteoffs = byte offset in first physical page			*/
/*									*/
/*    Output:								*/
/*									*/
/*	bytes copied from the physical pages				*/
/*									*/
/************************************************************************/

void oz_hw_phys_movetovirt (uLong nbytes, void *vaddr, const OZ_Mempage *phypages, uLong byteoffs)

{
  OZ_Hwaxp_Cpudb *cpudb;
  OZ_Pagentry savepte;
  uLong length;

  cpudb = oz_hwaxp_cpudb + oz_hw_cpu_getcur ();
  savepte = cpudb -> db.tempspte[0];					// save what is in the spte
  while (nbytes > 0) {							// repeat until done
    phypages += byteoffs >> LOG2PAGESIZE;				// normalize byteoffs to a page
    byteoffs &= PAGEMASK;
    cpudb -> db.tempspte[0] = (((uQuad)(phypages[0])) << PTE_V_PP) + PTE__KR; // map the physical page
    OZ_HWAXP_MTPR_TBIS (cpudb -> db.tempspva);
    length = PAGESIZE - byteoffs;					// get bytes to end of physical page
    if (length > nbytes) length = nbytes;				// but only do as much as we have left
    memcpy (vaddr, cpudb -> db.tempspva + byteoffs, length);		// copy the data
    nbytes -= length;							// this much less left to do
    byteoffs += length;							// increment input offset
    (OZ_Pointer)vaddr += length;					// increment output pointer
  }
  cpudb -> db.tempspte[0] = savepte;					// restore saved spte
  OZ_HWAXP_MTPR_TBIS (cpudb -> db.tempspva);
}

/************************************************************************/
/*									*/
/*  Copy data from one range of physical addresses to another		*/
/*									*/
/*    Input:								*/
/*									*/
/*	nbytes    = number of bytes to copy				*/
/*	src_pages = array of source physical page numbers		*/
/*	src_offs  = byte offset in src_pages[0] page to start copying	*/
/*	dst_pages = array of destination physical page numbers		*/
/*	dst_offs  = byte offset in dst_pages[0] page to start at	*/
/*									*/
/*    Output:								*/
/*									*/
/*	bytes copied from the physical pages				*/
/*									*/
/************************************************************************/

void oz_hw_phys_movephys (uLong nbytes, const OZ_Mempage *src_pages, uLong src_offs, const OZ_Mempage *dst_pages, uLong dst_offs)

{
  OZ_Hwaxp_Cpudb *cpudb;
  OZ_Pagentry saveptes[2];
  uLong len1, len2;

  cpudb = oz_hwaxp_cpudb + oz_hw_cpu_getcur ();
  saveptes[0] = cpudb -> db.tempspte[0];				// save what is in the sptes
  saveptes[1] = cpudb -> db.tempspte[1];
  while (nbytes > 0) {							// repeat until done
    src_pages += src_offs >> LOG2PAGESIZE;				// normalize byteoffs to a page
    src_offs  &= PAGEMASK;
    dst_pages += dst_offs >> LOG2PAGESIZE;
    dst_offs  &= PAGEMASK;
    cpudb -> db.tempspte[0] = (((uQuad)(src_pages[0])) << PTE_V_PP) + PTE__KR; // map the physical pages
    cpudb -> db.tempspte[1] = (((uQuad)(dst_pages[0])) << PTE_V_PP) + PTE__KW;
    OZ_HWAXP_MTPR_TBIS (cpudb -> db.tempspva);
    OZ_HWAXP_MTPR_TBIS (cpudb -> db.tempspva + PAGESIZE);
    len1 = PAGESIZE - src_offs;						// get bytes to end of physical pages
    len2 = PAGESIZE - dst_offs;
    if (len1 > len2) len1 = len2;
    if (len1 > nbytes) len1 = nbytes;					// but only do as much as we have left
    memcpy (cpudb -> db.tempspva + PAGESIZE + dst_offs, cpudb -> db.tempspva + src_offs, len1);
    nbytes -= len1;							// this much less left to do
    src_offs += len1;							// increment input offset
    dst_offs += len1;							// increment output offset
  }
  cpudb -> db.tempspte[0] = saveptes[0];				// restore saved sptes
  cpudb -> db.tempspte[1] = saveptes[1];
  OZ_HWAXP_MTPR_TBIS (cpudb -> db.tempspva);
  OZ_HWAXP_MTPR_TBIS (cpudb -> db.tempspva + PAGESIZE);
}

/************************************************************************/
/*									*/
/*  Write breakpoint							*/
/*									*/
/************************************************************************/

void oz_hw_debug_bpt (void)

{
  OZ_HWAXP_BPT ();
}

/************************************************************************/
/*									*/
/*  Crash the computer - does something like a breakpoint with 		*/
/*  hardware interrupts inhibited which causes a fatal kernel 		*/
/*  exception								*/
/*									*/
/************************************************************************/

void oz_hw_crash (void)

{
  oz_knl_printk ("oz_hw_crash: halting\n");
  while (1) {
    OZ_HWAXP_MTPR_IPL (31);
    OZ_HWAXP_HALT ();
  }
}

/************************************************************************/
/*									*/
/*  Reboot or Halt							*/
/*									*/
/************************************************************************/

void oz_hw_reboot (void)

{
  while (1) OZ_HWAXP_HALT ();
}

void oz_hw_halt (void)

{
  while (1) OZ_HWAXP_HALT ();
}

/************************************************************************/
/*									*/
/*  Return 1 if in kernel mode, 0 otherwise				*/
/*									*/
/************************************************************************/

int oz_hw_inknlmode (void)

{
  uQuad ps;

  ps = OZ_HWAXP_RD_PS ();
  return ((ps & 0x18) == 0);
}

/************************************************************************/
/*									*/
/*  Perform traceback of call frames					*/
/*									*/
/*    Input:								*/
/*									*/
/*	entry = entrypoint of callback routine				*/
/*	param = parameter for callback routine				*/
/*	maxframes = max number of frames (or -1 for all)		*/
/*	mchargs = initial machine args (or NULL to create own)		*/
/*	readmem = routine to read frames (or NULL for local)		*/
/*									*/
/*    Output:								*/
/*									*/
/*	Callback routine is called with parameter and machine args for 	*/
/*	each call frame.  The machine arguments are filled in as best 	*/
/*	as can be done for the calling standard in use, but at least 	*/
/*	the return and frame addresses are filled in for each frame.	*/
/*									*/
/************************************************************************/

void oz_hw_traceback (void (*entry) (void *param, OZ_Mchargs *mchargs), 
                      void *param, 
                      uLong maxframes, 
                      OZ_Mchargs *mchargs, 
                      int (*readmem) (void *param, void *buff, uLong size, void *addr));


/* Convert integer to asciz string */

void oz_hw_itoa (uLong valu, uLong size, char *buff)

{
  char temp[3*sizeof valu];
  int i;

  i = sizeof temp;
  temp[--i] = 0;
  do {
    temp[--i] = (valu % 10) + '0';
    valu /= 10;
  } while (valu != 0);
  strncpyz (buff, temp + i, size);
}

/* Convert integer to hexadecimal string */

void oz_hw_ztoa (uLong valu, uLong size, char *buff)

{
  char temp[3*sizeof valu];
  int i;

  i = sizeof temp;
  temp[--i] = 0;
  do {
    temp[--i] = (valu % 16) + '0';
    if (temp[i] > '9') temp[i] += 'A' - '9' - 1;
    valu /= 16;
  } while (valu != 0);
  strncpyz (buff, temp + i, size);
}

/* Convert string to decimal integer */

uLong oz_hw_atoi (const char *s, int *usedup)

{
  char c;
  const char *p;
  uLong accum;

  p = s;
  if ((p[0] == '0') && ((p[1] == 'x') || (p[1] == 'X'))) {
    accum = oz_hw_atoz (p + 2, usedup);
    if (usedup != NULL) *usedup += 2;
    return (accum);
  }

  accum = 0;
  for (; (c = *p) != 0; p ++) {
    if (c < '0') break;
    if (c > '9') break;
    accum = accum * 10 + c - '0';
  }

  if (usedup != NULL) *usedup = p - s;
  return (accum);
}

/* Convert string to hexadecimal integer */

uLong oz_hw_atoz (const char *s, int *usedup)

{
  char c;
  const char *p;
  uLong accum;

  accum = 0;
  for (p = s; (c = *p) != 0; p ++) {
    if ((c >= 'A') && (c <= 'F')) c -= 'A' - 10;
    else if ((c >= 'a') && (c <= 'f')) c -= 'a' - 10;
    else if ((c < '0') || (c > '9')) break;
    else c -= '0';
    accum = accum * 16 + c;
  }

  if (usedup != NULL) *usedup = p - s;
  return (accum);
}

/************************************************************************/
/*									*/
/*  Machine arg routines						*/
/*									*/
/************************************************************************/

/* Print out the machine arguments */

uLong oz_hw_mchargs_print (uLong (*entry) (void *param, const char *format, ...), void *param, int full, OZ_Mchargs *mchargs)

{
  uLong sts;

  if (full) {
    sts = (*entry) (param, "    R0=%16.16QX  R10=%16.16QX  R20=%16.16QX\n"
                           "    R1=%16.16QX  R11=%16.16QX  R21=%16.16QX\n"
                           "    R2=%16.16QX  R12=%16.16QX  R22=%16.16QX\n"
                           "    R3=%16.16QX  R13=%16.16QX  R23=%16.16QX\n"
                           "    R4=%16.16QX  R14=%16.16QX  R24=%16.16QX\n"
                           "    R5=%16.16QX  R15=%16.16QX  R25=%16.16QX\n"
                           "    R6=%16.16QX  R16=%16.16QX  R26=%16.16QX\n"
                           "    R7=%16.16QX  R17=%16.16QX  R27=%16.16QX\n"
                           "    R8=%16.16QX  R18=%16.16QX  R28=%16.16QX\n"
                           "    R9=%16.16QX  R19=%16.16QX  R29=%16.16QX\n"
                           "                                            SP/R30=%16.16QX\n"
                           "    PC=%16.16QX   P2=%16.16QX   P4=%16.16QX\n"
                           "    PS=%16.16QX   P3=%16.16QX   P5=%16.16QX\n", 
                           mchargs -> r0,  mchargs -> r10,  mchargs -> r20,
                           mchargs -> r1,  mchargs -> r11,  mchargs -> r21,
                           mchargs -> r2,  mchargs -> r12,  mchargs -> r22,
                           mchargs -> r3,  mchargs -> r13,  mchargs -> r23,
                           mchargs -> r4,  mchargs -> r14,  mchargs -> r24,
                           mchargs -> r5,  mchargs -> r15,  mchargs -> r25,
                           mchargs -> r6,  mchargs -> r16,  mchargs -> r26,
                           mchargs -> r7,  mchargs -> r17,  mchargs -> r27,
                           mchargs -> r8,  mchargs -> r18,  mchargs -> r28,
                           mchargs -> r9,  mchargs -> r19,  mchargs -> r29,
                                                            mchargs -> r30,
                           mchargs -> pc,  mchargs -> p2,   mchargs -> p4,
                           mchargs -> ps,  mchargs -> p3,   mchargs -> p5);
  } else {
    sts = (*entry) (param, "    %16.16QX  %16.16QX\n", mchargs -> pc, mchargs -> r30);
  }
  return (sts);
}

/* Machine arguments (standard and extended) descriptors (for the debugger) */

static const OZ_Mchargs *mchargs_proto;
const OZ_Debug_mchargsdes oz_hw_mchargs_des[] = {
	OZ_DEBUG_MD (mchargs_proto, "r0",  r0), 
	OZ_DEBUG_MD (mchargs_proto, "r1",  r1), 
	OZ_DEBUG_MD (mchargs_proto, "r2",  r2), 
	OZ_DEBUG_MD (mchargs_proto, "r3",  r3), 
	OZ_DEBUG_MD (mchargs_proto, "r4",  r4), 
	OZ_DEBUG_MD (mchargs_proto, "r5",  r5), 
	OZ_DEBUG_MD (mchargs_proto, "r6",  r6), 
	OZ_DEBUG_MD (mchargs_proto, "r7",  r7), 
	OZ_DEBUG_MD (mchargs_proto, "r8",  r8), 
	OZ_DEBUG_MD (mchargs_proto, "r9",  r9), 
	OZ_DEBUG_MD (mchargs_proto, "r10", r10), 
	OZ_DEBUG_MD (mchargs_proto, "r11", r11), 
	OZ_DEBUG_MD (mchargs_proto, "r12", r12), 
	OZ_DEBUG_MD (mchargs_proto, "r13", r13), 
	OZ_DEBUG_MD (mchargs_proto, "r14", r14), 
	OZ_DEBUG_MD (mchargs_proto, "r15", r15), 
	OZ_DEBUG_MD (mchargs_proto, "r16", r16), 
	OZ_DEBUG_MD (mchargs_proto, "r17", r17), 
	OZ_DEBUG_MD (mchargs_proto, "r18", r18), 
	OZ_DEBUG_MD (mchargs_proto, "r19", r19), 
	OZ_DEBUG_MD (mchargs_proto, "r20", r20), 
	OZ_DEBUG_MD (mchargs_proto, "r21", r21), 
	OZ_DEBUG_MD (mchargs_proto, "r22", r22), 
	OZ_DEBUG_MD (mchargs_proto, "r23", r23), 
	OZ_DEBUG_MD (mchargs_proto, "r24", r24), 
	OZ_DEBUG_MD (mchargs_proto, "r25", r25), 
	OZ_DEBUG_MD (mchargs_proto, "r26", r26), 
	OZ_DEBUG_MD (mchargs_proto, "r27", r27), 
	OZ_DEBUG_MD (mchargs_proto, "r28", r28), 
	OZ_DEBUG_MD (mchargs_proto, "r29", r29), 
	OZ_DEBUG_MD (mchargs_proto, "r30", r30), 
	OZ_DEBUG_MD (mchargs_proto, "pc",  pc), 
	OZ_DEBUG_MD (mchargs_proto, "ps",  ps), 
	OZ_DEBUG_MD (mchargs_proto, "p2",  p2), 
	OZ_DEBUG_MD (mchargs_proto, "p3",  p3), 
	OZ_DEBUG_MD (mchargs_proto, "p4",  p4), 
	OZ_DEBUG_MD (mchargs_proto, "p5",  p5), 
	NULL, 0, 0 };

static const OZ_Mchargx_knl *mchargx_knl_proto;
const OZ_Debug_mchargsdes oz_hw_mchargx_knl_des[] = {
	NULL, 0, 0 };

static const OZ_Mchargx_usr *mchargx_usr_proto;
const OZ_Debug_mchargsdes oz_hw_mchargx_usr_des[] = {
	NULL, 0, 0 };

/* Transfer from cpu to mchargx_knl struct */

void oz_hw_mchargx_knl_fetch (OZ_Mchargx_knl *mchargx_knl)

{
  memset (mchargx_knl, 0, sizeof *mchargx_knl);
  mchargx_knl -> asn_ro   = OZ_HWAXP_MFPR_ASN   ();
  mchargx_knl -> asten_rw = OZ_HWAXP_MFPR_ASTEN ();
  mchargx_knl -> astsr_rw = OZ_HWAXP_MFPR_ASTSR ();
  mchargx_knl -> fen_rw   = OZ_HWAXP_MFPR_FEN   ();
  mchargx_knl -> mces_rw  = OZ_HWAXP_MFPR_MCES  ();
  mchargx_knl -> pcbb_ro  = OZ_HWAXP_MFPR_PCBB  ();
  mchargx_knl -> prbr_rw  = OZ_HWAXP_MFPR_PRBR  ();
  mchargx_knl -> ptbr_ro  = OZ_HWAXP_MFPR_PTBR  ();
  mchargx_knl -> scbb_rw  = OZ_HWAXP_MFPR_SCBB  ();
  mchargx_knl -> sisr_ro  = OZ_HWAXP_MFPR_SISR  ();
  mchargx_knl -> usp_rw   = OZ_HWAXP_MFPR_USP   ();
  mchargx_knl -> vptb_rw  = OZ_HWAXP_MFPR_VPTB  ();
  mchargx_knl -> whami_ro = OZ_HWAXP_MFPR_WHAMI ();
}

/* Transfer from mchargx_knl struct to cpu */

void oz_hw_mchargx_knl_store (OZ_Mchargx_knl *mchargx_knl, OZ_Mchargx_knl *mchargx_knl_mask)

{
  if (mchargx_knl_mask -> asten_rw) OZ_HWAXP_MTPR_ASTEN (mchargx_knl -> asten_rw);
  if (mchargx_knl_mask -> astsr_rw) OZ_HWAXP_MTPR_ASTSR (mchargx_knl -> astsr_rw);
  if (mchargx_knl_mask -> datfx_wo) OZ_HWAXP_MTPR_DATFX (mchargx_knl -> datfx_wo);
  if (mchargx_knl_mask -> fen_rw)   OZ_HWAXP_MTPR_FEN   (mchargx_knl -> fen_rw);
  if (mchargx_knl_mask -> ipir_wo)  OZ_HWAXP_MTPR_IPIR  (mchargx_knl -> ipir_wo);
  if (mchargx_knl_mask -> mces_rw)  OZ_HWAXP_MTPR_MCES  (mchargx_knl -> mces_rw);
  if (mchargx_knl_mask -> prbr_rw)  OZ_HWAXP_MTPR_PRBR  (mchargx_knl -> prbr_rw);
  if (mchargx_knl_mask -> scbb_rw)  OZ_HWAXP_MTPR_SCBB  (mchargx_knl -> scbb_rw);
  if (mchargx_knl_mask -> sirr_wo)  OZ_HWAXP_MTPR_SIRR  (mchargx_knl -> sirr_wo);
  if (mchargx_knl_mask -> tbia_wo)  OZ_HWAXP_MTPR_TBIA  ();
  if (mchargx_knl_mask -> tbis_wo)  OZ_HWAXP_MTPR_TBIS  (mchargx_knl -> tbis_wo);
  if (mchargx_knl_mask -> tbisd_wo) OZ_HWAXP_MTPR_TBISD (mchargx_knl -> tbisd_wo);
  if (mchargx_knl_mask -> tbisi_wo) OZ_HWAXP_MTPR_TBISI (mchargx_knl -> tbisi_wo);
  if (mchargx_knl_mask -> usp_rw)   OZ_HWAXP_MTPR_USP   (mchargx_knl -> usp_rw);
  if (mchargx_knl_mask -> vptb_rw)  OZ_HWAXP_MTPR_VPTB  (mchargx_knl -> vptb_rw);
}

/* Transfer from cpu to mchargx_usr struct */

void oz_hw_mchargx_usr_fetch (OZ_Mchargx_usr *mchargx_usr)

{
}

/* Transfer from mchargx_usr struct to cpu */

void oz_hw_mchargx_usr_store (OZ_Mchargx_usr *mchargx_usr, OZ_Mchargx_usr *mchargx_usr_mask)

{
}

/************************************************************************/
/*									*/
/*  Check to see if the given location(s) are addressable with the 	*/
/*  given mode without any pagefaults					*/
/*									*/
/*    Input:								*/
/*									*/
/*	size  = number of bytes starting at 'adrs'			*/
/*	adrs  = starting virtual address				*/
/*	mode  = OZ_PROCMODE_KNL or OZ_PROCMODE_USR			*/
/*	write = 0 : read-only access					*/
/*	        1 : read/write access					*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_hw_probe = 0 : access would pagefault			*/
/*	              1 : access will not pagefault			*/
/*									*/
/************************************************************************/

static uQuad const probe_masks[4] = { PTE__KR, PTE__KW, PTE__UR, PTE__UW };

int oz_hw_probe (uLong size, const void *adrs, OZ_Procmode mode, int write)

{
  OZ_Pointer begaddr, endaddr;
  uQuad i, mask, npgs, page, pte, *pteva;

  if (size == 0) return (1);						// null buffers always successful

  begaddr = (OZ_Pointer)adrs;						// get beginning of buffer
  endaddr = begaddr + size - 1;						// get end (inclusive) of buffer
  if (endaddr < begaddr) {						// fail if it wraps
    oz_knl_printk ("oz_hw_probe*: %X at %p wraps\n", size, adrs);
    return (0);
  }
  if ((begaddr <  OZ_HWAXP_VAGAPEND) 					// fail if it spans the gap
   && (endaddr >= OZ_HWAXP_VAGAPBEG)) {
    oz_knl_printk ("oz_hw_probe*: %X at %p spans gap\n", size, adrs);
    return (0);
  }

  page = OZ_HW_VADDRTOVPAGE (begaddr);					// starting page to check
  npgs = OZ_HW_VADDRTOVPAGE (endaddr) - page + 1;			// number of pages to check

  mask = probe_masks[mode*2+write];					// get mask to check for
									// all bits must be set

  do {
    i = page >> (2 * LOG2PTESPERPAGE);					// make sure L1 entry shows KR access
    pteva = oz_hwaxp_l1ptbase + i;
    pte = *pteva;
    if ((pte & PTE__KR) != PTE__KR) {
      oz_knl_printk ("oz_hw_probe*: %X at %p L1PTE[%X] %p/%QX\n", size, adrs, i, pteva, pte);
      oz_hw_pte_print (pteva);
      pteva = oz_hwaxp_l3ptbase + OZ_HW_VADDRTOVPAGE (pteva);
      pte = *pteva;
      oz_knl_printk ("oz_hw_probe*:   %p/%QX\n", pteva, pte);
      return (0);
    }
    do {
      i = page >> LOG2PTESPERPAGE;					// make sure L2 entry shows KR access
      pteva = oz_hwaxp_l2ptbase + i;
      pte = *pteva;
      if ((pte & PTE__KR) != PTE__KR) {
        oz_knl_printk ("oz_hw_probe*: %X at %p L2PTE[%X] %p/%QX\n", size, adrs, i, pteva, pte);
        return (0);
      }
      do {
        pteva = oz_hwaxp_l3ptbase + page;				// fail if L3 entry doesn't have all required bits
        pte = *pteva;
        if ((pte & mask) != mask) {
          oz_knl_printk ("oz_hw_probe*: %X at %p L3PTE[%X] %p/%QX\n", size, adrs, page, pteva, pte);
          return (0);
        }
        if (-- npgs == 0) return (1);					// success if all pages checked
      } while ((++ page & (PTESPERPAGE - 1)) != 0);			// check next page
    } while ((page << (64 - 2 * LOG2PTESPERPAGE)) != 0);
  } while (1);
}

/************************************************************************/
/*									*/
/*  Read hardware pte							*/
/*									*/
/*    Input:								*/
/*									*/
/*	vpn = virtual page number to read the pte of			*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_hw_pte_read = NULL : pte read successfully			*/
/*	                 else : this vaddr must be faulted in first	*/
/*	*pagestate_r = page's software state				*/
/*	*phypage_r   = corresponding physical page number		*/
/*	*curprot_r   = page's current protection code			*/
/*	*reqprot_r   = requested page protection code			*/
/*									*/
/************************************************************************/

	/* this version functions as described above */

void *oz_hw_pte_readany (OZ_Mempage vpn, 
                         OZ_Section_pagestate *pagestate_r, 
                         OZ_Mempage *phypage_r, 
                         OZ_Hw_pageprot *curprot_r, 
                         OZ_Hw_pageprot *reqprot_r)

{
  OZ_Mempage l1idx, l2idx;
  uQuad pte;

  if (vpn >= (1 << OZ_HWAXP_L2VPSIZE)) oz_crash ("oz_hw_pte_readany: bad vpn %X", vpn);

  l1idx = vpn >> (2 * LOG2PTESPERPAGE);					// index in L1 pagetable
  l2idx = vpn >> LOG2PTESPERPAGE;					// index in L2 pagetable
  if (!(oz_hwaxp_l1ptbase[l1idx] & 1) 					// make sure L1 entry indicates L2 page is present
   || !(oz_hwaxp_l2ptbase[l2idx] & 1)) {				// make sure L2 entry indicates L3 page is present
    return (oz_hwaxp_l3ptbase + vpn);					// if not, return VA within L3 table we want to get
  }									//  - that's what we really want

  pte = oz_hwaxp_l3ptbase[vpn];						// it's there, read it
  if (pagestate_r != NULL) *pagestate_r = (pte >> PTE_V_PS) & PTE_X_PS;	// software page state (OZ_SECTION_PAGESTATE_...)
  if (phypage_r   != NULL) *phypage_r   =  pte >> PTE_V_PP;		// physical page number
  if (curprot_r   != NULL) *curprot_r   = (pte >> PTE_V_CP) & PTE_X_CP;	// current hw page protection (OZ_HW_PAGEPROT_...)
  if (reqprot_r   != NULL) *reqprot_r   = (pte >> PTE_V_RP) & PTE_X_RP;	// requested hw page protection (OZ_HW_PAGEPROT_...)
  return (NULL);							// return success status
}

	/* this one only reads system pte's, and crashes if it can't */

void oz_hw_pte_readsys (OZ_Mempage vpn,
                        OZ_Section_pagestate *pagestate_r,
                        OZ_Mempage *phypage_r,
                        OZ_Hw_pageprot *curprot_r,
                        OZ_Hw_pageprot *reqprot_r)

{
  uQuad pte;

  if (vpn >= (1 << OZ_HWAXP_L2VPSIZE)) oz_crash ("oz_hw_pte_readsys: bad vpn %X", vpn);
  if (!OZ_HW_ISSYSPAGE (vpn)) oz_crash ("oz_hw_pte_readsys: not system vpn %X", vpn);

  pte = oz_hwaxp_l3ptbase[vpn];						// blindly assume we can read it
									// if we can't, we'll pagefault here and crash
  if (pagestate_r != NULL) *pagestate_r = (pte >> PTE_V_PS) & PTE_X_PS;	// software page state (OZ_SECTION_PAGESTATE_...)
  if (phypage_r   != NULL) *phypage_r   =  pte >> PTE_V_PP;		// physical page number
  if (curprot_r   != NULL) *curprot_r   = (pte >> PTE_V_CP) & PTE_X_CP;	// current hw page protection (OZ_HW_PAGEPROT_...)
  if (reqprot_r   != NULL) *reqprot_r   = (pte >> PTE_V_RP) & PTE_X_RP;	// requested hw page protection (OZ_HW_PAGEPROT_...)
}

/************************************************************************/
/*									*/
/*  Write hardware pte							*/
/*									*/
/*    Input:								*/
/*									*/
/*	vpn = virtual page number to write the pte of			*/
/*	pagestate = software page state					*/
/*	            (just save this value and return it via oz_hw_pte_read)
/*	phypage   = physical page number				*/
/*	curprot   = protection to set page to				*/
/*	reqprot   = software's requested page protection		*/
/*	            (just save this value and return it via oz_hw_pte_read)
/*									*/
/*    Output:								*/
/*									*/
/*	pte written, cache entry invalidated				*/
/*									*/
/*    Note:								*/
/*									*/
/*	this routine does not check to see if the pte is in memory or 	*/
/*	not, so it will crash if it is above softint.  so if you want 	*/
/*	to check if it is in, you will have to set the corresponding 	*/
/*	pt lock then call oz_hw_pte_readany to see if it is there.	*/
/*									*/
/************************************************************************/

static uLong const hwprot[4] = { PTE__NA, PTE__KW, PTE__UR, PTE__UW };

/* This one just invalidates the cache on the current cpu - it is used only for upgrades in protection  */
/* and then only where the other cpu's can recover from a pagefault if they have the old value cached   */

void oz_hw_pte_writecur (OZ_Mempage vpn, 
                         OZ_Section_pagestate pagestate, 
                         OZ_Mempage phypage, 
                         OZ_Hw_pageprot curprot, 
                         OZ_Hw_pageprot reqprot)

{
  uQuad pte;

  if (vpn >= (1 << OZ_HWAXP_L2VPSIZE)) oz_crash ("oz_hw_pte_writecur: bad vpn %X", vpn);

  pte = (((uQuad)phypage) << PTE_V_PP) 			// pte<63:32> = physical page number
      + (reqprot << PTE_V_RP) 				// pte<25:24> = requested protection
      + (curprot << PTE_V_CP) 				// pte<21:20> = current protection
      + (pagestate << PTE_V_PS) 			// pte<18:16> = page state
      + hwprot[curprot];				// pte<15:00> = hardware protection code

  if (OZ_HW_ISSYSPAGE (vpn)) pte |= PTE_M_GBL;		// if system page, mark it global to all processes

  oz_hwaxp_l3ptbase[vpn] = pte;				// write hardware pagetable entry

  OZ_HWAXP_MTPR_TBIS (OZ_HW_VPAGETOVADDR (vpn));	// invalidate any old copy the CPU might have cached
}

/* This one invalidates the cache on all cpus - it is used under all other circumstances */

void oz_hw_pte_writeall (OZ_Mempage vpn, 
                         OZ_Section_pagestate pagestate, 
                         OZ_Mempage phypage, 
                         OZ_Hw_pageprot curprot, 
                         OZ_Hw_pageprot reqprot)

{
  uQuad pte;

  if (vpn >= (1 << OZ_HWAXP_L2VPSIZE)) oz_crash ("oz_hw_pte_writeall: bad vpn %X", vpn);

  pte = (((uQuad)phypage) << PTE_V_PP) 			// pte<63:32> = physical page number
      + (reqprot << PTE_V_RP) 				// pte<25:24> = requested protection
      + (curprot << PTE_V_CP) 				// pte<21:20> = current protection
      + (pagestate << PTE_V_PS) 			// pte<18:16> = page state
      + hwprot[curprot];				// pte<15:00> = hardware protection code

  if (OZ_HW_ISSYSPAGE (vpn)) pte |= PTE_M_GBL;		// if system page, mark it global to all processes

  oz_hwaxp_l3ptbase[vpn] = pte;				// write hardware pagetable entry

  OZ_HWAXP_MTPR_TBIS (OZ_HW_VPAGETOVADDR (vpn));	// invalidate any old copy the CPU might have cached

  // ???? code to flag other cpus ????
}

/************************************************************************/
/*									*/
/*  An pagetable page is about to be paged out or unmapped.  Check it 	*/
/*  to see that all pages it maps are also paged out or unmapped.  If 	*/
/*  it finds any that aren't, return the vpage of one that isn't.	*/
/*									*/
/*    Input:								*/
/*									*/
/*	vpage = virt page number of pagetable page that is about to be 	*/
/*	        paged out or unmapped					*/
/*	unmap = 0 : paging it out, just check for 			*/
/*	            pagestate=PAGEDOUT and curport=NA			*/
/*	        1 : unmapping it, also check for phypage=0		*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_hw_pte_checkptpage = 0 : all pages are ok			*/
/*	                     else : at least this page needs attention	*/
/*									*/
/************************************************************************/

OZ_Mempage oz_hw_pte_checkptpage (OZ_Mempage vpage, int unmap)

{
  int i;
  OZ_Section_pagestate pagestate;
  uQuad pte, *ptes;

  /* Make sure it's a pagetable page */

  ptes = OZ_HW_VPAGETOVADDR (vpage);
  if (ptes < oz_hwaxp_l3ptbase) return (0);
  if (ptes >= oz_hwaxp_l3ptbase + (OZ_HWAXP_L3PTSIZE / 8)) return (0);

  /* Scan it */

  for (i = 0; i < PTESPERPAGE; i ++) {
    pte = *(ptes ++);
    pagestate = (pte >> PTE_V_PS) & PTE_X_PS;;
    if (((pte >> PTE_V_CP) & PTE_X_CP) != OZ_HW_PAGEPROT_NA) goto failed;
    if (unmap) {
      if ((pagestate != OZ_SECTION_PAGESTATE_PAGEDOUT) 
       && (pagestate != OZ_SECTION_PAGESTATE_READFAILED) 
       && (pagestate != OZ_SECTION_PAGESTATE_WRITEFAILED)) goto failed;
      if ((pte >> PTE_V_PP) != 0) goto failed;
    } else {
      if (pagestate != OZ_SECTION_PAGESTATE_PAGEDOUT) goto failed;
    }
  }
  return (0);

  /* Something is referenced by the page, return the virtual page of what is mapped by the entry */

failed:
  vpage  -= OZ_HW_VADDRTOVPAGE (oz_hwaxp_l3ptbase);
  vpage <<= LOG2PTESPERPAGE;
  return (vpage + i);
}

/************************************************************************/
/*									*/
/*  Print pagetable entries corresponding to a given virtual address	*/
/*									*/
/*    Input:								*/
/*									*/
/*	vaddr = virtual address						*/
/*									*/
/************************************************************************/

void oz_hw_pte_print (void *vaddr)

{
  uQuad page, pte, ptepa, vpage;

  vpage = OZ_HW_VADDRTOVPAGE (vaddr);

  oz_knl_printk ("oz_hw_pte_print: vaddr %16.16QX : %12.12QX\n", vaddr, vpage);

  page  = OZ_HWAXP_MFPR_PTBR ();
  ptepa = (page << LOG2PAGESIZE) + (vpage >> (2 * LOG2PTESPERPAGE)) * 8;
  pte   = OZ_HWAXP_LDQP (ptepa);

  oz_knl_printk ("                    L1 pte %12.12QX / %16.16QX\n", ptepa, pte);

  if ((pte & 0x101) == 0x101) {
    page  = pte >> PTE_V_PP;
    ptepa = (page << LOG2PAGESIZE) + (((vpage >> LOG2PTESPERPAGE) * 8) & PAGEMASK);
    pte   = OZ_HWAXP_LDQP (ptepa);

    oz_knl_printk ("                    L2 pte %12.12QX / %16.16QX\n", ptepa, pte);

    if ((pte & 0x101) == 0x101) {
      page  = pte >> PTE_V_PP;
      ptepa = (page << LOG2PAGESIZE) + ((vpage * 8) & PAGEMASK);
      pte   = OZ_HWAXP_LDQP (ptepa);

      oz_knl_printk ("                    L3 pte %12.12QX / %16.16QX\n", ptepa, pte);
    }
  }
}

/************************************************************************/
/*									*/
/*  Write breakpoint to the given address, probably marked readonly	*/
/*									*/
/*    Input:								*/
/*									*/
/*	bptaddr = address to write the breakpoint to			*/
/*	opcode  = opcode to write there					*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_hw_debug_writebpt = NULL : successful			*/
/*	                       else : points to error message text	*/
/*									*/
/************************************************************************/

char const *oz_hw_debug_writebpt (OZ_Breakpoint *bptaddr, OZ_Breakpoint opcode)

{
  return ("not implemented");
}

/* Dump page tables */

void oz_hwaxp_dumppagetables (void)

{
  int i, j, k, k2;
  uQuad l1ppn, l1pte, l2ppn, l2pte, l3ppn, l3pte, l3pte2;

  l1ppn = OZ_HWAXP_MFPR_PTBR ();
  for (i = 0; i < PTESPERPAGE; i ++) {
    l1pte = OZ_HWAXP_LDQP (l1ppn * PAGESIZE + i * 8);
    if (l1pte & 1) {
      oz_knl_printk ("oz_hwaxp_dumppagetables: L1[%3X] %16.16QX\n", i, l1pte);
      l2ppn = l1pte >> PTE_V_PP;
      for (j = 0; j < PTESPERPAGE; j ++) {
        l2pte = OZ_HWAXP_LDQP (l2ppn * PAGESIZE + j * 8);
        if (l2pte & 1) {
          oz_knl_printk ("oz_hwaxp_dumppagetables: L2[%3X,%3X] %16.16QX\n", i, j, l2pte);
          l3ppn = l2pte >> PTE_V_PP;
          for (k = 0; k < PTESPERPAGE; k ++) {
            l3pte = OZ_HWAXP_LDQP (l3ppn * PAGESIZE + k * 8);
            if (l3pte & 1) {
              for (k2 = k; ++ k2 < PTESPERPAGE;) {
                l3pte2 = OZ_HWAXP_LDQP (l3ppn * PAGESIZE + k2 * 8);
                if ((l3pte2 ^ l3pte) & ((1ULL << PTE_V_PP) - 1)) break;
                if ((l3pte2 >> PTE_V_PP) - k2 != (l3pte >> PTE_V_PP) - k) break;
              }
              if (-- k2 == k) oz_knl_printk ("oz_hwaxp_dumppagetables: L3[%3X,%3X,%3X] %16.16QX\n", i, j, k, l3pte);
              else {
                l3pte2 = OZ_HWAXP_LDQP (l3ppn * PAGESIZE + k2 * 8);
                oz_knl_printk ("oz_hwaxp_dumppagetables: L3[%3X,%3X,%3X..%3X] %16.16QX..%16.16QX\n", i, j, k, k2, l3pte, l3pte2);
              }
              k = k2;
            }
          }
        }
      }
    }
  }
}

uQuad divby10 (uQuad value)

{
  return (value / 10);
}
