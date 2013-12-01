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
/*  Hardware process context routines for 486's				*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_knl_hw.h"
#include "oz_knl_process.h"
#include "oz_knl_quota.h"
#include "oz_knl_sdata.h"
#include "oz_knl_section.h"
#include "oz_knl_status.h"
#include "oz_params_486.h"
#include "oz_sys_condhand.h"
#include "oz_sys_pdata.h"

	/* this struct must match the PRCTX_... symbols in oz_kernel_486.s */

typedef struct { OZ_Phyaddr mpdpa;	/* physical address of process local mpd */
                 OZ_Section *ptsec;	/* process pagetable section */
                 OZ_Phyaddr pt1pa;	/* physical address of the pagetable page allocated by init routine */
                 Long ptmap;		/* set when pagetable section gets mapped to pagetable by first thread */
                 OZ_Phyaddr rp1pa;	/* requested protection page allocated by init routine */
               } Prctx;

static Prctx *system_process_hw_ctx = NULL;

static void errcleanup (Prctx *prctx, OZ_Quota *quota);
static void checkptes (Prctx *prctx);
static void checkpte (Prctx *prctx, OZ_Mempage vpage, OZ_Phyaddr paddr);
static uLong readvirtlong (Prctx *prctx, OZ_Pointer vaddr);
static uLong mapptsec (Prctx *prctx, int newprocess);
static uLong init_pdata (void *dummy);

/************************************************************************/
/*									*/
/*  Initialize process hardware context block				*/
/*									*/
/*    Input:								*/
/*									*/
/*	process_hw_ctx = hardware context pointer (process -> hw_ctx)	*/
/*	process  = software context pointer				*/
/*	sysproc  = 0 : other than oz_s_systemproc process		*/
/*	           1 : creating oz_s_systemproc process			*/
/*	copyproc = 0 : create empty process				*/
/*	           1 : create process with pagetables equal to 		*/
/*	               current process					*/
/*	smplock level = softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_hw_process_initctx = OZ_SUCCESS : successful			*/
/*	                              else : error status		*/
/*	*process_hw_ctx = filled in					*/
/*									*/
/************************************************************************/

uLong oz_hw_process_initctx (void *process_hw_ctx, OZ_Process *process, int sysproc, int copyproc)

{
  int i;
  OZ_Mempage phypage;
  OZ_Process *saveproc;
  OZ_Quota *quota;
  Prctx *prctx;
  uLong exitsts, pm, sts;

  prctx = process_hw_ctx;

  /* Hopefully the compiler is smart enough to optimize this out */

  if (sizeof *prctx > OZ_PROCESS_HW_CTX_SIZE) {
    oz_crash ("oz_hw_process_initctx: sizeof *prctx (%d) > OZ PROCESS HW CTX SIZE (%d)", sizeof *prctx, OZ_PROCESS_HW_CTX_SIZE);
  }

  memset (prctx, 0, sizeof *prctx);

  /* The system process uses the global MPD based at address MPDBASE           */
  /* It just maps the addresses that are common to the system (ie, < 20000000) */

  if (sysproc) {
    prctx -> mpdpa = MPDBASE;		/* use the global MPD at MPDBASE ... */
					/* virt and phys addresses are the same */
    prctx -> ptmap = OZ_SUCCESS;	/* pagetable section does not need to be mapped */
    prctx -> ptsec = NULL;		/* ... because it doesn't have a section */
    sts = oz_knl_process_createpagetable (process, OZ_SECTION_EXPUP, oz_s_sysmem_pagtblsz, 0);
    if (sts != OZ_SUCCESS) oz_knl_printk ("oz_hw_process_initctx: error %u creating process pagetable\n", sts);
    system_process_hw_ctx = prctx;	/* save for future reference */
    return (sts);
  }

  /* Normal process
  
      Here is where we set up virtual addressing layout for the process as follows:
  
  	PROCVIRTBASE is a multiple of 4M so it starts its own page on the MPD (doesn't share a page of pte's with system common area)
  	MAXPROCPAGES is a multiple of 1023 to make PROCVIRTBASE + 4100 * MAXPROCPAGES + 4096, so we use all of the process page table pages
  	(we don't want to leave part of a page unused, and we want the pte for the MPD and the last pte page to be in the same pte page, not split)
  

  4m boundary
     indicator
	+---------------------------------------+
	|  no-access to catch bad pointers	|  <- 0xFFFFF000
	+---------------------------------------+
	|  process copy of MPD			|  <- 0xFFFFE000  PROCMPDVADDR	<- physical address PRCTX_L_MPDPA
	|   - first part is copy of system MPD	|
	|     that never ever changes		|
	|   - the rest reflects the state of 	|
	|     this process' page tables		|
	+---------------------------------------+
	|  last RP page				|  <- 0xFFFFD000		<- physical address PRCTX_L_RP1PA
	|  last PT page				|  <- 0xFFFFC000		<- physical address PRCTX_L_PT1PA
	|					|
	|  process page table pages		|				\  private section 'PRCTX_L_PTSEC'
	|  enough to map from PROCBASVADDR up 	|				|  - doesn't include MPD page
	|  to and including PROCMPDVADDR	|				|  - doesn't include last PT page
	|					|  <- PROCPTRPBVADDR		|  - doesn't include last RP page
	+---------------------------------------+				/
	|  per-process data, one page per 	|
	|  processor access mode		|
	|					|  <- oz_sys_pdata_array (created by mapptsec routine)
	+---------------------------------------+
	|					|
	|  user process space			|
	|  (MAXPROCPAGES*4096 bytes)		|
  4m	|					|  <- PROCBASVADDR
	+---------------------------------------+
	|					|
	|    (unused)				|
	|					|  <- oz_s_sysmem_pagtblsz * 4096
	+---------------------------------------+
	|					|
	|  system global memory			|
	|					|  <- 0x00001000
	+---------------------------------------+
	|  no-access to catch null pointers	|  <- 0x00000000
	+---------------------------------------+

    Note:

	The pages starting at PROCPTRPBVADDR are the pagetable pages.  However, they are 
	not all contiguous.  There are 16 PT pages, followed by 1 RP page, then 16 more 
	PT pages followed by another RP page, etc, etc.  They are interleaved like this 
	so it will put all the statically allocated physical pages at the end of the 
	virtual addresses, which makes it easy to create the pagetable section.

	To calculate the pagetable page for a given virtual page, use:

		ptpage = (((vpage - PROCBASVPAGE) /  1024) * 17 / 16) + PROCPTRPBVPAGE

	To calculate the reqprot page for a given virutal page, use:

		rppage = (((vpage - PROCBASVPAGE) / 16384) * 17) + 16 + PROCPTRPBVPAGE

  */

  prctx -> ptmap = OZ_PENDING;	/* pagetable hasn't been mapped yet */
				/* when first thread of process starts, it will map the pagetable before doing anything else */

  /* Create a section to put the pagetable and reqprot pages in (all except for the last two, */
  /* because they contain the pte and reqprot bytes that maps the process' copy of the MPD)   */

  sts = oz_knl_section_create (NULL, ONEMEG-PROCPTRPBVPAGE-4, 0, OZ_SECTION_TYPE_PAGTBL | OZ_SECTION_TYPE_ZEROES, NULL, &(prctx -> ptsec));
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_hw_process_initctx: error %u creating pagetable section\n", sts);
    return (sts);
  }

  /* Make sure there is quota for three physical memory pages.  One is for the MPD, one is  */
  /* for the PT page that maps the MPD, one is for RP page that hold bits for last pt page. */

  quota = oz_knl_section_getquota (prctx -> ptsec);
  if ((quota != NULL) && !oz_knl_quota_debit (quota, OZ_QUOTATYPE_PHM, 3)) {
    oz_knl_section_increfc (prctx -> ptsec, -1);
    return (OZ_EXQUOTAPHM);
  }

  /* Allocate a physical page for the MPD and initialize it with the global MPD contents         */
  /* Its virtual address will be 0xFFFFE000                                                      */
  /* Initializing it with the global MPD contents means it will map the system common area       */
  /* It is not part of a section, we must manually free this page in the process_termctx routine */

  phypage = oz_knl_phymem_allocpagew (OZ_PHYMEM_PAGESTATE_ALLOCSECT, 0xFFFFE);	/* allocate physical page */
  prctx -> mpdpa = phypage << 12;						/* save corresponding physical address */
  oz_hw_phys_movefromvirt (4096, (void *)MPDBASE, &phypage, 0);			/* copy in global mpd contents */
  sts = oz_hw486_phys_fetchlong (prctx -> mpdpa);				/* should now have same first long anyway */
  if (sts != *(uLong *)MPDBASE) oz_crash ("oz_hw_process_initctx: first mpd long %x", sts);

  /* Allocate a physical page for the reqprot page that maps the MPD and itself                  */
  /* Its virtual address will be 0xFFFFD000                                                      */
  /* Its first entry will map virtual address 0xFC000xxx                                         */
  /* Its last entry will map virtual address 0xFFFFFxxx                                          */
  /* It is not part of a section, we must manually free this page in the process_termctx routine */

  phypage = oz_knl_phymem_allocpagew (OZ_PHYMEM_PAGESTATE_ALLOCSECT, 0xFFFFD);	/* allocate physical page */
  prctx -> rp1pa = phypage << 12;						/* save corresponding physical address */
#if OZ_HW_PAGEPROT_NA != 0
  error : code assumes OZ HW PAGEPROT NA is zero
#endif
  oz_hw486_phys_filllong (0, prctx -> rp1pa);					/* fill with OZ_HW_PAGEPROT_NA entries for all pages */
  oz_hw486_phys_storelong (OZ_HW_PAGEPROT_KW * 0x15000000, prctx -> rp1pa + 0xFFC); /* set last three to KW and very last to NA */

  /* Allocate a physical page for the page table page that maps the MPD and itself               */
  /* Its virtual address will be 0xFFFFC000                                                      */
  /* Its first entry will map virtual address 0xFFC00xxx                                         */
  /* Its last entry will map virtual address 0xFFFFFxxx                                          */
  /* It is not part of a section, we must manually free this page in the process_termctx routine */

  phypage = oz_knl_phymem_allocpagew (OZ_PHYMEM_PAGESTATE_ALLOCSECT, 0xFFFFC);	/* allocate physical page */
  prctx -> pt1pa = phypage << 12;						/* save corresponding physical address */
  oz_hw486_phys_storelong (prctx -> pt1pa | (OZ_SECTION_PAGESTATE_VALID_W << 9) | MPD_BITS, prctx -> mpdpa + 0xFFC); /* map it in the MPD */

  /* Map the process' mpd, rp1 and pt1 pages to the process address space by filling the appropriate pt1 entries */

  oz_hw486_phys_filllong (OZ_SECTION_PAGESTATE_PAGEDOUT << 9, prctx -> pt1pa);	/* fill with pagedout entries for all pages */

  oz_hw486_phys_storelong (prctx -> mpdpa | (OZ_SECTION_PAGESTATE_VALID_W << 9) | PT_KRW, prctx -> pt1pa + ((PROCMPDVPAGE-0)*4&0xFFF));
  oz_hw486_phys_storelong (prctx -> rp1pa | (OZ_SECTION_PAGESTATE_VALID_W << 9) | PT_KRW, prctx -> pt1pa + ((PROCMPDVPAGE-1)*4&0xFFF));
  oz_hw486_phys_storelong (prctx -> pt1pa | (OZ_SECTION_PAGESTATE_VALID_W << 9) | PT_KRW, prctx -> pt1pa + ((PROCMPDVPAGE-2)*4&0xFFF));

  /* Create a pagetable struct to map sections to that covers the whole process address space */
  /* This is how we tell the kernel what range of virtual addresses the process has           */
  /* Exclude the last four pages of virtual address space:                                    */
  /*   1) Exclude the hole page because we never use it for anything anyway                   */
  /*   2) Exclude the MPD page because we don't want the pager to mess with it                */
  /*      and we clean it up manually in oz_hw_process_termctx                                */
  /*   3) Exclude the rp1 and pt1 pages for same reasons as (2)                               */
  /* Our processes only have one of these ever                                                */
  /* Everything after this must succeed as we have no routine to undo this                    */

  sts = oz_knl_process_createpagetable (process, OZ_SECTION_EXPUP, ONEMEG-PROCBASVPAGE-4, PROCBASVPAGE);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_hw_process_initctx: error %u creating process pagetable\n", sts);
    errcleanup (prctx, quota);
    return (sts);
  }

  /* Validate the pte's that we explicitly set up */

  checkptes (prctx);

  /* All our processes are the same.  However, the copyprocess routine will require that the pagetable */
  /* be mapped to the newly created process without waiting for the first thread in the process to do  */
  /* it.  So to do it now, we switch to the new process' pagetables then map its pagetable section.    */

  if (copyproc) {
    saveproc = oz_knl_process_getcur ();
    oz_knl_process_setcur (process);
    sts = mapptsec (prctx, 0);
    oz_knl_process_setcur (saveproc);
    if (sts != OZ_SUCCESS) {
      oz_knl_printk ("oz_hw_process_486: error %u mapping pagetable section\n", sts);
      errcleanup (prctx, quota);
      return (sts);
    }
  }

  /* Successful */

  return (OZ_SUCCESS);
}

static void errcleanup (Prctx *prctx, OZ_Quota *quota)

{
  uLong pm;

  pm = oz_hw_smplock_wait (&oz_s_smplock_pm);		/* oz_knl_phymem_freepage must be called at smplock level pm */
  oz_knl_phymem_freepage (prctx -> pt1pa >> 12);	/* free the three physical pages that we allocated */
  oz_knl_phymem_freepage (prctx -> rp1pa >> 12);
  oz_knl_phymem_freepage (prctx -> mpdpa >> 12);
  oz_hw_smplock_clr (&oz_s_smplock_pm, pm);		/* restore smplock level */
  if (quota != NULL) oz_knl_quota_credit (quota, OZ_QUOTATYPE_PHM, 3, -1);
  oz_knl_section_increfc (prctx -> ptsec, -1);
  memset (prctx, 0, sizeof *prctx);
}

static void checkptes (Prctx *prctx)

{
  checkpte (prctx, PROCMPDVPAGE-0, prctx -> mpdpa);	/* the mpd page should be addressible at vaddr 0xFFFFE000 */
  checkpte (prctx, PROCMPDVPAGE-1, prctx -> rp1pa);	/* the rp1 page should be addressible at vaddr 0xFFFFD000 */
  checkpte (prctx, PROCMPDVPAGE-2, prctx -> pt1pa);	/* the pt1 page should be addressible at vaddr 0xFFFFC000 */
}

/* Validate the pte for a given virtual page to make sure it points to the correct physical address */

static void checkpte (Prctx *prctx, OZ_Mempage vpage, OZ_Phyaddr paddr)

{
  uLong mpdentry, ptentry, ptpagevaddr, rpentry, rppagevaddr;

  /* paddr should be page aligned and within range of RAM */

  if ((paddr & 0xFFF) || ((paddr >> 12) >= oz_s_phymem_totalpages)) {
    oz_crash ("oz_hw_process checkpte: bad paddr %x", paddr);
  }

  /* Read MPD entry from the mpd page just like the cpu would */

  mpdentry = oz_hw486_phys_fetchlong (prctx -> mpdpa + ((vpage >> 10) << 2));

  /* It should indicate a kernel and user writable page */

  if ((mpdentry & 7) != 7) oz_crash ("oz_hw_process_initctx checkpte: vpage %x, mpdentry %x", vpage, mpdentry);

  /* Now get the pagetable entry from the mpd entry just like the cpu would */

  ptentry = oz_hw486_phys_fetchlong ((mpdentry & 0xFFFFF000) | ((vpage & 0x3FF) << 2));

  /* Make sure it is what it should be */

  if (((ptentry & 7) != 3) 						/* kernel writable, user no access */
   || ((ptentry & 0xE00) != (OZ_SECTION_PAGESTATE_VALID_W << 9)) 	/* pagestate indicates write-enabled page that is dirty */
   || ((ptentry & 0xFFFFF000) != paddr)) {				/* should point to correct physical page */
    oz_crash ("oz_hw_process_initctx checkpte: vpage %x, ptentry %x (mpdentry %x, mpdpa %x)", vpage, ptentry, mpdentry, prctx -> mpdpa);
  }

  /* Read the longword that contains the requested protection bits just like oz_hw_pte_read would */

  rppagevaddr = (((((vpage - PROCBASVPAGE) / 16384) * 17) + 16) << 12) + PROCPTRPBVADDR;
  rpentry     = readvirtlong (prctx, rppagevaddr + (((vpage >> 4) & 0x3FF) << 2));
  rpentry   >>= (vpage & 15) * 2;
  rpentry    &= 3;
  if (rpentry != OZ_HW_PAGEPROT_KW) {
    oz_crash ("oz_hw_process_initctx checkpte: vpage %x, rppagevaddr %x, rpentry bits %x", vpage, rppagevaddr, rpentry);
  }

  /* Read the pte longword just like oz_hw_pte_read would */

  ptpagevaddr = (((((vpage - PROCBASVPAGE) /  1024) * 17) / 16) << 12) + PROCPTRPBVADDR;
  ptentry     = readvirtlong (prctx, ptpagevaddr + ((vpage & 0x3FF) << 2));

  /* Validate its contents just like we did above */

  if (((ptentry & 7) != 3) 						/* kernel writable, user no access */
   || ((ptentry & 0xE00) != (OZ_SECTION_PAGESTATE_VALID_W << 9)) 	/* pagestate indicates write-enabled page that is dirty */
   || ((ptentry & 0xFFFFF000) != paddr)) {				/* should point to correct physical page */
    oz_crash ("oz_hw_process_initctx checkpte: vpage %x, ptpagevaddr %x, ptentry %x", vpage, ptpagevaddr, ptentry);
  }
}

/* Read a virtual memory location given the hardware process context block address */

static uLong readvirtlong (Prctx *prctx, OZ_Pointer vaddr)

{
  OZ_Mempage vpage;
  uLong mpdentry, ptentry, virtentry;

  if (vaddr & 3) oz_crash ("oz_hw_process_initctx readvirtlong: vaddr %x not long aligned", vaddr);
  vpage = vaddr >> 12;

  /* Read MPD entry from the mpd page just like the cpu would */

  mpdentry = oz_hw486_phys_fetchlong (prctx -> mpdpa + ((vpage >> 10) << 2));

  /* It should indicate a kernel and user writable page */

  if ((mpdentry & 7) != 7) oz_crash ("oz_hw_process_initctx checkpte: vpage %x, mpdentry %x", vpage, mpdentry);

  /* Now get the pagetable entry from the mpd entry just like the cpu would */

  ptentry = oz_hw486_phys_fetchlong ((mpdentry & 0xFFFFF000) | ((vpage & 0x3FF) << 2));

  /* It should indicate a kernel writable page */

  if ((ptentry & 7) != 3) oz_crash ("oz_hw_process_initctx readvirtlong: vaddr %x, ptentry %x", vaddr, ptentry);

  /* Now read the requested memory location using the pagetable entry just like the cpu would */

  virtentry = oz_hw486_phys_fetchlong ((ptentry & 0xFFFFF000) | (vaddr & 0xFFC));

  return (virtentry);
}

/************************************************************************/
/*									*/
/*  Terminate current normal process hardware context block		*/
/*  System process hardware context block is never terminated		*/
/*									*/
/*  Note that the final thread (routine cleanupproc in 			*/
/*  oz_knl_process_increfc) is still active in the process		*/
/*									*/
/*    Input:								*/
/*									*/
/*	process_hw_ctx = pointer to process hardware context block	*/
/*	process = pointer to process block				*/
/*									*/
/*	ipl = softint							*/
/*									*/
/************************************************************************/

void oz_hw_process_termctx (void *process_hw_ctx, OZ_Process *process)

{
  OZ_Quota *quota;
  Prctx *prctx;
  uLong pm;

  prctx = process_hw_ctx;

  /* Make sure we're not trying to wipe the system process */

  if (prctx == system_process_hw_ctx) oz_crash ("oz_hw_process_termctx: trying to terminate system process");

  /* Switch to system global MPD because we are about to wipe out this process' page tables */

  oz_hw_process_switchctx (prctx, system_process_hw_ctx);

  /* Clear out the mpd and pt1 pages in case we try to use them again by accident */
  /* - but not even a double-fault handler will save us from an wiped MPD page    */
  /* Don't bother filling the rp1 page                                            */

  oz_hw486_phys_filllong (0x99996666, prctx -> pt1pa);	/* use even number so the 'page-present' bit will be zero */
  oz_hw486_phys_filllong (0x99669966, prctx -> mpdpa);

  /* Free off all three pages */

  pm = oz_hw_smplock_wait (&oz_s_smplock_pm);		/* oz_knl_phymem_freepage must be called at smplock level pm */
  oz_knl_phymem_freepage (prctx -> pt1pa >> 12);	/* free the three physical pages */
  oz_knl_phymem_freepage (prctx -> rp1pa >> 12);
  oz_knl_phymem_freepage (prctx -> mpdpa >> 12);
  oz_hw_smplock_clr (&oz_s_smplock_pm, pm);		/* restore smplock level */

  /* Release quota for those three physical memory pages */

  quota = oz_knl_section_getquota (prctx -> ptsec);
  if (quota != NULL) oz_knl_quota_credit (quota, OZ_QUOTATYPE_PHM, 3, -1);

  /* Decrement pagetable section reference count - this undoes the refcount from when it was created in oz_hw_process_initctx */

  oz_knl_section_increfc (prctx -> ptsec, -1);

  /* Garbage fill the closed out prctx */

  memset (prctx, 0x96, sizeof *prctx);
}

/************************************************************************/
/*									*/
/*  This routine is called when a thread first starts			*/
/*  If it is the very first thread of a process, it maps the pagetable 	*/
/*  to the virtual address space of the process				*/
/*									*/
/*    Input:								*/
/*									*/
/*	process_hw_ctx = pointer to process' hw ctx block		*/
/*	smplevel = null							*/
/*									*/
/************************************************************************/

void oz_hw_process_firsthread (void *process_hw_ctx)

{
  int si;
  uLong sts;

  si = oz_hw_cpu_setsoftint (0);
  sts = mapptsec (process_hw_ctx, 1);
  oz_hw_cpu_setsoftint (si);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_hw_process_firsthread: error %u mapping pagetable section\n", sts);
    oz_knl_thread_exit (sts);
    oz_crash ("oz_hw_process_firsthread: returned from oz_knl_thread_exit");
  }
}

/* Map a process' pagetable section to its virtual address space */

static uLong mapptsec (Prctx *prctx, int newprocess)

{
  OZ_Mempage npages, svpage;
  OZ_Section *pdatasec;
  uLong sts;

  /* Make sure some other CPU isn't trying to do this */

  /* ptmap = OZ_PENDING : hasn't been done yet at all */
  /*         OZ_STARTED : started on another cpu      */
  /*         OZ_SUCCESS : completed successfully      */
  /*               else : error status                */

  do {
    sts = prctx -> ptmap;
    if ((sts != OZ_PENDING) && (sts != OZ_STARTED)) return (sts);
  } while (!oz_hw_atomic_setif_long (&(prctx -> ptmap), OZ_STARTED, OZ_PENDING));

  /* Map the pagetable section */

  npages = ONEMEG-PROCPTRPBVPAGE-4;				/* number of pages in the section */
  svpage = PROCPTRPBVPAGE;					/* virtual page to start it at */
  sts = oz_knl_process_mapsection (prctx -> ptsec, &npages, &svpage, OZ_MAPSECTION_EXACT, OZ_PROCMODE_KNL, OZ_HW_PAGEPROT_KW);

  /* If this is a new process (not a copied process), create the oz_sys_pdata array and init it      */
  /* If it is a copied process, the oz_sys_pdata array will be copied because it is a normal section */

  if (newprocess && (sts == OZ_SUCCESS)) {
    npages = 2;							/* one for kernel, one for user mode */
    svpage = PROCPTRPBVPAGE-2;					/* just below the pagetable section */
    sts = oz_knl_section_create (NULL, 2, 0, OZ_SECTION_TYPE_ZEROES, NULL, &pdatasec);
    if (sts == OZ_SUCCESS) {
      sts = oz_knl_process_mapsection (pdatasec, &npages, &svpage, OZ_MAPSECTION_EXACT, OZ_PROCMODE_KNL, OZ_HW_PAGEPROT_KW);
      oz_knl_section_increfc (pdatasec, -1);
      if (sts == OZ_SUCCESS) sts = oz_sys_condhand_try (init_pdata, NULL, oz_sys_condhand_rtnanysig, NULL);
      if (sts == OZ_SUCCESS) sts = oz_knl_section_setpageprot (1, PROCPTRPBVPAGE-1, OZ_HW_PAGEPROT_UW, NULL, NULL);
      if (sts == OZ_SUCCESS) sts = oz_knl_handletbl_create ();	/* create handle table for this process */
    }
  }
  OZ_HW_MB;							/* make sure other cpu's see mapsection results before we set ptmap=status */
  prctx -> ptmap = sts;						/* tell all other cpu's the mapping was done */
  return (sts);
}

/* This is a separate routine in a 'try'/'catch' handler in case there is no memory to fault the pages in */

static uLong init_pdata (void *dummy)

{
  oz_sys_pdata_array[OZ_PROCMODE_KNL-OZ_PROCMODE_MIN].data.rwpageprot = OZ_HW_PAGEPROT_KW;	// read/write by kernel only
  oz_sys_pdata_array[OZ_PROCMODE_KNL-OZ_PROCMODE_MIN].data.procmode   = OZ_PROCMODE_KNL;	// owned by kernel mode
  oz_sys_pdata_array[OZ_PROCMODE_KNL-OZ_PROCMODE_MIN].data.pprocmode  = OZ_PROCMODE_KNL;	// this is the per-process knl data

  oz_sys_pdata_array[OZ_PROCMODE_USR-OZ_PROCMODE_MIN].data.rwpageprot = OZ_HW_PAGEPROT_UW;	// read/write by any mode
  oz_sys_pdata_array[OZ_PROCMODE_USR-OZ_PROCMODE_MIN].data.procmode   = OZ_PROCMODE_USR;	// owned by user mode
  oz_sys_pdata_array[OZ_PROCMODE_USR-OZ_PROCMODE_MIN].data.pprocmode  = OZ_PROCMODE_USR;	// this is the per-process user data

  return (OZ_SUCCESS);										// sucessfully faulted and initted those pages
}

void oz_hw_process_validate (void *process_hw_ctx, OZ_Process *process)

{
  Prctx *prctx;

  prctx = process_hw_ctx;

  if (prctx != system_process_hw_ctx) {
    checkptes (prctx);
    OZ_KNL_CHKOBJTYPE (prctx -> ptsec, OZ_OBJTYPE_SECTION);
  }
}
