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
/*  Hardware process context routines for Alphas			*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_knl_hw.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_process.h"
#include "oz_knl_quota.h"
#include "oz_knl_sdata.h"
#include "oz_knl_section.h"
#include "oz_knl_status.h"
#include "oz_sys_condhand.h"
#include "oz_sys_pdata.h"

#define PAGESIZE (1 << OZ_HW_L2PAGESIZE)
#define PTESPERPAGE (PAGESIZE / 8)

	/* this is where the oz_sys_pdata_array gets mapped for every process */

#define OZ_SYS_PDATA_ARRAY (OZ_HW_PROCLINKBASE - sizeof oz_sys_pdata_array)

	/* this struct must match the PRCTX_... symbols in oz_kernel_axp.s */

typedef struct { OZ_Mempage l1ptpp;	/* physical page of process local L1 page (its PTBR) */
                 uLong ptmap;		/* set when pagetable section gets mapped to pagetable by first thread */
                 OZ_Section *ptsec;	/* process pagetable section */
                 Long asn;		/* assigned address space number */
					/* zero: system process */
					/* else: 1..asn_maxasn-1 */
               } Prctx;

static OZ_Mempage basepage, l1ptpage, l3ptpage, nptpages, numpages, pdata_svpage, pdata_npages, selfrefidx;
static Long asn_maxcpu = 0;
static Long asn_maxasn = 0;
static Long volatile asn_last = 0;
static Prctx **asnarray = NULL;
static Prctx *system_process_hw_ctx = NULL;

static void errcleanup (Prctx *prctx, OZ_Quota *quota);
static uLong readvirtlong (Prctx *prctx, OZ_Pointer vaddr);
static uLong mapptsec (Prctx *prctx, int newprocess);
static uLong init_pdata (void *dummy);

/************************************************************************/
/*									*/
/*  Initialize internal static data					*/
/*									*/
/************************************************************************/

void oz_hwaxp_process_init (void)

{
  /* Make sure OZ_PROCESS_HW_CTX_SIZE is big enough */

  if (sizeof (Prctx) > OZ_PROCESS_HW_CTX_SIZE) {
    oz_crash ("oz_hw_process_initctx: sizeof (Prctx) (%d) > OZ PROCESS HW CTX SIZE (%d)", sizeof (Prctx), OZ_PROCESS_HW_CTX_SIZE);
  }

  /* Define a symbol for the linker that is the address of the oz_sys_pdata_array    */
  /* The idea is that this symbol is at a constant virtual address for every process */

  asm volatile ("\n"
	"	.globl	oz_sys_pdata_array\n"
	"	.type	oz_sys_pdata_array,@object\n"
	"		oz_sys_pdata_array = %0"
		: : "i" (OZ_SYS_PDATA_ARRAY));

  /* Size and starting virtual page of pdata array */

  pdata_npages = (sizeof oz_sys_pdata_array) / PAGESIZE;
  pdata_svpage = OZ_HW_VADDRTOVPAGE (OZ_SYS_PDATA_ARRAY);

  /* Base page of virtual address space for a process */

  basepage = OZ_HW_VADDRTOVPAGE (OZ_SYS_PDATA_ARRAY);

  /* Number of pages in the per-process space */

  numpages = OZ_HW_VADDRTOVPAGE (oz_hwaxp_l1ptbase) - basepage, 

  /* Number of pages in the per-process pagetable */

  nptpages = ((oz_hwaxp_sysbasva - (OZ_Pointer)oz_hwaxp_l3ptbase) / PAGESIZE) - 1;

  /* Virtual page of the L1 and L3 pagetables */

  l1ptpage = OZ_HW_VADDRTOVPAGE (oz_hwaxp_l1ptbase);
  l3ptpage = OZ_HW_VADDRTOVPAGE (oz_hwaxp_l3ptbase);

  /* Index of L1 self-reference pte */

  selfrefidx = l1ptpage >> (2 * OZ_HW_L2PAGESIZE - 6);
}

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
  int i, j;
  Long asninc;
  OZ_Mempage phypage;
  OZ_Process *saveproc;
  OZ_Quota *quota;
  Prctx *prctx;
  uLong exitsts, pm, sts;
  uQuad pte, ptepa;

  prctx = process_hw_ctx;

  memset (prctx, 0, sizeof *prctx);

  /* The system process uses the global L1 page at oz_hwaxp_sysl1ptpp */

  if (sysproc) {
    OZ_Mempage npages, svpage;
    uLong l1index, l2index;
    uQuad *l2page, *l3page;

    /* Allocate ASN array now that we have access to NPP */

    asn_maxcpu = oz_hwaxp_hwrpb -> numprcslt;
    asn_maxasn = 1;
    if (oz_hwaxp_hwrpb -> maxvalasn != 0) asn_maxasn = oz_hwaxp_hwrpb -> maxvalasn;
    asnarray = OZ_KNL_NPPMALLOC (asn_maxcpu * asn_maxasn * sizeof *asnarray);
    memset (asnarray, 0, asn_maxcpu * asn_maxasn * sizeof *asnarray);

    /* Fill in system process context block */

    prctx -> l1ptpp = oz_hwaxp_sysl1ptpp;	/* use the global L1 pagetable page */
    prctx -> ptmap  = OZ_SUCCESS;		/* assume we'll be successful mapping section */
    prctx -> asn    = 0;			/* system process is always address space 0 */
    system_process_hw_ctx = prctx;		/* save for future reference */

    /* Create system pagetable section - covers from just past the L1 page up to but not including the Pyxis pages */
    /* This is the same range covered by oz_s_sysmem_pagtblsz and oz_s_sysmem_baseva                               */

    /* Don't include the Pyxis pages because they get handled on their own, and we don't want anything mapped there. */
    /* Don't include the L1ptpage because it's part of process address space and it's always paged in.               */

    /* - this call tells the kernel it's OK to map stuff in the given range of pages for the system process */

    npages = oz_s_sysmem_pagtblsz;
    svpage = OZ_HW_VADDRTOVPAGE (oz_s_sysmem_baseva);

    sts = oz_knl_process_createpagetable (process, OZ_SECTION_EXPUP, npages, svpage);
    if (sts != OZ_SUCCESS) oz_crash ("oz_hw_process_initctx: error %u creating %X page system pagetable at %X", sts, npages, svpage);

    /* - this call tells the kernel we have a section that is a pagetable */
    /*   it covers from just past the L1 page to the end of the L3 table  */

    npages = ((OZ_Pointer)oz_hwaxp_l3ptbase + OZ_HWAXP_L3PTSIZE - (OZ_Pointer)oz_s_sysmem_baseva) / PAGESIZE;

    sts = oz_knl_section_create (NULL, npages, 0, OZ_SECTION_TYPE_PAGTBL | OZ_SECTION_TYPE_ZEROES, NULL, &(prctx -> ptsec));
    if (sts != OZ_SUCCESS) oz_crash ("oz_hw_process_initctx: error %u creating %X system pagetable section", sts, npages);

    /* - this call tells the kernel not to map anything in the addresses where our system pagetable is  */
    /*   it also tells it to fault in demand-zero pages for stuff that's missing                        */
    /*   use OZ_MAPSECTION_SYSTEM so it won't try to set page protections (there's already stuff there) */

    sts = oz_knl_process_mapsection (prctx -> ptsec, &npages, &svpage, OZ_MAPSECTION_EXACT | OZ_MAPSECTION_SYSTEM, OZ_PROCMODE_KNL, OZ_HW_PAGEPROT_KW);
    if (sts != OZ_SUCCESS) oz_crash ("oz_hw_process_initctx: error %u mapping %X page system pagetable section at %X", sts, npages, svpage);

    /* Now we have to set reqprot=KW in all L1 and L2 pte's that are currently paged in.  oz_knl_process_mapsection  */
    /* would have done this without OZ_MAPSECTION_SYSTEM but would have puqued because there is stuff already there. */

    for (i = 0; i < PTESPERPAGE; i ++) {			// loop through whole L1 page
      pte  = oz_hwaxp_l1ptbase[i];				// get an entry
      pte &= ~(OZ_HWAXP_PTE_X_RP << OZ_HWAXP_PTE_V_RP);		// clear out any reqprot currently in it
      pte |=  (OZ_HW_PAGEPROT_KW << OZ_HWAXP_PTE_V_RP);		// set up reqprot = OZ_PROCMODE_KW
      oz_hwaxp_l1ptbase[i] = pte;
      if (pte & 1) {						// see if the L2 page exists
        for (j = 0; j < PTESPERPAGE; j ++) {			// loop through whole L2 page
          pte  = oz_hwaxp_l2ptbase[i*PTESPERPAGE+j];		// set the reqprot field to OZ_PROCMODE_KW
          pte &= ~(OZ_HWAXP_PTE_X_RP << OZ_HWAXP_PTE_V_RP);
          pte |=  (OZ_HW_PAGEPROT_KW << OZ_HWAXP_PTE_V_RP);
          oz_hwaxp_l2ptbase[i*PTESPERPAGE+j] = pte;
        }
      }
								// for any L2 pages not there, when we demand-zero page them in, 
								// ... they will get their reqprot's set to KW because the 
								// ... mapsection call declared the section mapped with KW
    }

    /* Double-map the oz_s_systempdata where its oz_sys_pdata_array[KNL] would be */
    /* We have to allocate an L2 page, then an L3 page, then copy the L3 entry    */

    if (OZ_HW_VADDRTOPOFFS (&oz_s_systempdata) != 0) {
      oz_crash ("oz_hw_process_initctx: oz_s_systempdata not page aligned %p", &oz_s_systempdata);
    }

    l1index = pdata_svpage / PTESPERPAGE / PTESPERPAGE;
    l2page  = oz_hwaxp_l2ptbase + (l1index * PTESPERPAGE);

    phypage = oz_knl_phymem_allocpagew (OZ_PHYMEM_PAGESTATE_ALLOCSECT, OZ_HW_VADDRTOVPAGE (l2page));
    oz_hwaxp_l1ptbase[l1index] = (((uQuad)phypage) << OZ_HWAXP_PTE_V_PP) + OZ_HWAXP_PTE_PKW;
    memset (l2page, 0, PAGESIZE);

    l2index = pdata_svpage / PTESPERPAGE;
    l3page  = oz_hwaxp_l3ptbase + (l2index * PTESPERPAGE);

    phypage = oz_knl_phymem_allocpagew (OZ_PHYMEM_PAGESTATE_ALLOCSECT, OZ_HW_VADDRTOVPAGE (l3page));
    oz_hwaxp_l2ptbase[l2index] = (((uQuad)phypage) << OZ_HWAXP_PTE_V_PP) + OZ_HWAXP_PTE_PKW;
    memset (l3page, 0, PAGESIZE);

    phypage = oz_hwaxp_l3ptbase[OZ_HW_VADDRTOVPAGE(&oz_s_systempdata)] >> OZ_HWAXP_PTE_V_PP;
    oz_hwaxp_l3ptbase[pdata_svpage] = (((uQuad)phypage) << OZ_HWAXP_PTE_V_PP) + OZ_HWAXP_PTE_PKW;

    return (sts);
  }

  /* Normal process
  
      Here is where we set up virtual addressing layout for the process
      Per-process space starts at 0 and goes up to the L1 pt page (inclusive)

      So we have (assuming 8K page size and 2 I/O sparse space mapping superpages):

	+---------------------------------------+
	| (2*8GB) I/O sparse space mapping	|  <- FFFF.FFFC.0000.0000
	+---------------------------------------+
	|   (8GB) Kernel			|  <- FFFF.FFFA.0000.0000
	+---------------------------------------+
	| (3*8MB) system global L3pt pages	|  <- FFFF.FFF9.FE80.0000
	+---------------------------------------+
	| (3*8KB) system global L2pt pages	|
	|         statically allocated so 	|
	|         common parts of L1pt page 	|
	|         never change			|  <- FFFF.FFF9.FE7F.C000  (system global space)
	+---------------------------------------+  - - - - - - - - - - - - - - - - - - - - - - -
	|   (8KB) L1pt page			|                          (process local space)
	|   - high part is copy of system L1pt	|
	|     page that never ever changes	|
	|     (3 entries)			|
	|   - middle is self-reference pointer	|
	|     (1 entry)				|
	|   - low part reflects the state 	|
	|     of this process' page tables	|
	|     (1020 entries)			|  <- FFFF.FFF9.FE7F.8000  (oz_hwaxp_l1ptbase)
	+---------------------------------------+
	| (1020*8KB) proc local L2pt pages	|  <- FFFF.FFF9.FE00.0000  (oz_hwaxp_l2ptbase)
	+---------------------------------------+
	| (1020*8MB) proc local L3pt pages	|  <- FFFF.FFF8.0000.0000  (oz_hwaxp_l3ptbase, ipr VPTB)
	+---------------------------------------+
	|					|
	|  upper process space (stack)		|
	|					|  <- FFFF.F800.0000.0000
	+---------------------------------------+

	   virtual address hole
						   <- 0000.0800.0000.0000
	+---------------------------------------+
	|					|
	|  lower process space (heap)		|
	|					|  <- 0000.0000.0010.0000  (OZ_HW_PROCLINKBASE)
	+---------------------------------------+
	|  per-process data, one 64K page per 	|
	|  processor access mode		|  <- 0000.0000.000E.0000  oz_sys_pdata_array (created by mapptsec routine)
	+---------------------------------------+
	|					|
	|  NO ACCESS				|
	|					|  <- 0000.0000.0000.0000
	+---------------------------------------+

  */

  prctx -> ptmap = OZ_PENDING;	/* pagetable hasn't been mapped yet */
				/* when first thread of process starts, it will map the pagetable before doing anything else */

  /* Set up address space number.  For now, just assign sequentially.  Someday, assign the least used. */

  prctx -> asn = 0;							// maybe CPU doesn't do ASN's
  if (asn_maxasn > 1) do {
    asninc = asn_last;							// it does, get last ASN assigned
    prctx -> asn = asninc + 1;						// assign the next sequential number
    if (prctx -> asn == asn_maxasn) prctx -> asn = 1;			// wrapping back to 1 (leave 0 for the system process)
  } while (!oz_hw_atomic_setif_long (&asn_last, prctx -> asn, asninc));	// update asn_last

  /* Create a section to put the pagetable pages in, up to but not including the process local L1pt page */

  sts = oz_knl_section_create (NULL, nptpages, 0, OZ_SECTION_TYPE_PAGTBL | OZ_SECTION_TYPE_ZEROES, NULL, &(prctx -> ptsec));
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_hw_process_initctx: error %u creating pagetable section\n", sts);
    return (sts);
  }

  /* Make sure there is quota for one physical memory page (for L1pt) */

  quota = oz_knl_section_getquota (prctx -> ptsec);
  if ((quota != NULL) && !oz_knl_quota_debit (quota, OZ_QUOTATYPE_PHM, 1)) {
    oz_knl_section_increfc (prctx -> ptsec, -1);
    return (OZ_EXQUOTAPHM);
  }

  /* Allocate a physical page for the L1pt and initialize it with the global L1pt contents       */
  /* Its virtual address will be oz_hwaxp_l1ptbase                                               */
  /* Initializing it with the global L1pt contents means it will map the system common area      */
  /* It is not part of a section, we must manually free this page in the process_termctx routine */

  phypage = oz_knl_phymem_allocpagew (OZ_PHYMEM_PAGESTATE_ALLOCSECT, l1ptpage);	/* allocate physical page */
  prctx -> l1ptpp = phypage;							/* save it */

  oz_hw_phys_initpage (phypage, 0);						/* fill page with zeroes */

  ptepa = phypage << OZ_HW_L2PAGESIZE;						/* get phyaddr of L1 pt page */
  i = selfrefidx;								/* get index of self reference */
  pte = (((uQuad)phypage) << OZ_HWAXP_PTE_V_PP) | OZ_HWAXP_PTE_PKW;		/* set up self reference pte */
  OZ_HWAXP_STQP (ptepa + i * 8, pte);
  while (++ i < (1 << (OZ_HW_L2PAGESIZE - 3))) {				/* fill in other global L1 ptes */
    pte = oz_hwaxp_l1ptbase[i];
    OZ_HWAXP_STQP (ptepa + i * 8, pte);
  }

  /* Create a pagetable struct to map sections that covers the whole process address space                                       */
  /* This is how we tell the kernel what range of virtual addresses the process has                                              */
  /* Exclude the L1 pt page because we don't want the pager to mess with it and we clean it up manually in oz_hw_process_termctx */
  /* Our processes only have one of these ever                                                                                   */

  sts = oz_knl_process_createpagetable (process, OZ_SECTION_EXPUP, numpages, basepage);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_hw_process_initctx: error %u creating process pagetable\n", sts);
    errcleanup (prctx, quota);
    return (sts);
  }

  /* All our processes are the same.  However, the copyprocess routine will require that the pagetable */
  /* be mapped to the newly created process without waiting for the first thread in the process to do  */
  /* it.  So to do it now, we switch to the new process' pagetables then map its pagetable section.    */

  if (copyproc) {
    saveproc = oz_knl_process_getcur ();
    oz_knl_process_setcur (process);
    sts = mapptsec (prctx, 0);
    oz_knl_process_setcur (saveproc);
    if (sts != OZ_SUCCESS) {
      oz_knl_printk ("oz_hw_process_axp: error %u mapping pagetable section\n", sts);
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
  oz_knl_phymem_freepage (prctx -> l1ptpp);		/* free the page that we allocated */
  oz_hw_smplock_clr (&oz_s_smplock_pm, pm);		/* restore smplock level */
  if (quota != NULL) oz_knl_quota_credit (quota, OZ_QUOTATYPE_PHM, 1, -1);
  oz_knl_section_increfc (prctx -> ptsec, -1);
  memset (prctx, 0, sizeof *prctx);
}

/************************************************************************/
/*									*/
/*  Switch process context on the current cpu				*/
/*									*/
/*    Input:								*/
/*									*/
/*	old_hw_ctx = pointer to old process hardware context block	*/
/*	new_hw_ctx = pointer to new process hardware context block	*/
/*	smp lock = ts							*/
/*									*/
/*    Output:								*/
/*									*/
/*	per-process memory is mapped to new_hw_ctx process		*/
/*									*/
/*    Note:								*/
/*									*/
/*	old_hw_ctx and new_hw_ctx will never be the same		*/
/*									*/
/************************************************************************/

void oz_hw_process_switchctx (void *old_hw_ctx, void *new_hw_ctx)

{
  OZ_Hwaxp_Cpudb *cpudb;
  OZ_Hwaxp_Hwpcb *newpcb, *oldpcb;
  OZ_Mempage ptbr;
  Prctx **asnelement, *newprctx, *oldprctx;
  uLong newidx, pcbidx;

  oldprctx = old_hw_ctx;
  newprctx = new_hw_ctx;

  /* Verify current PTBR is what we think it should be */

  ptbr = OZ_HWAXP_MFPR_PTBR ();
  if (ptbr != oldprctx -> l1ptpp) {
    oz_crash ("oz_hw_process_switchctx: oldprctx %p -> l1ptpp %X != ptbr %X", oldprctx, oldprctx -> l1ptpp, ptbr);
  }

  /* All we want is an MTPR_PTBR.  Since we ain't got one, fake it. */

  cpudb  = oz_hwaxp_cpudb + oz_hw_cpu_getcur ();				// point to CPU's data block
  pcbidx = cpudb -> db.acthwpcb;						// get active PCB index
  oldpcb = cpudb -> db.hwpcb_va[pcbidx];					// point to active PCB
  newidx = pcbidx ^ 1;								// get inactive PCB index
  newpcb = cpudb -> db.hwpcb_va[newidx];					// point to inactive PCB

  asm volatile ("stq $sp,%0" : "=m" (newpcb -> ksp));				// make it so the KSP won't change
  newpcb -> esp  = 0; // OZ_HWAXP_MFPR_ESP ();					// make it so the ESP won't change
  newpcb -> ssp  = 0; // OZ_HWAXP_MFPR_SSP ();					// make it so the SSP won't change
  newpcb -> usp  = OZ_HWAXP_MFPR_USP ();					// make it so the USP won't change
  newpcb -> ptbr = newprctx -> l1ptpp;						// this is the new PTBR we want
  newpcb -> asn  = newprctx -> asn;						// this is its ASN
  newpcb -> ast  = (OZ_HWAXP_MFPR_ASTSR () << 4) | OZ_HWAXP_MFPR_ASTEN ();	// make it so the ASTSR/ASTEN won't change
  newpcb -> fen  = oldpcb -> fen;						// make it so DATFX/FEN won't change
  newpcb -> uniq = OZ_HWAXP_READ_UNQ ();					// make it so the UNQ won't change

  OZ_HWAXP_SWPCTX (cpudb -> db.hwpcb_pa[newidx]);				// switch to new PCB

  cpudb -> db.acthwpcb = newidx;						// remember new active PCB

  /* Invalidate if we are re-using an ASN but this is a different process                                           */
  /* If the asnelement is NULL, means the CPU doesn't have any entries in the TLB for this ASN so just use it as is */
  /* Otherwise if it matches, means the CPU might have some ASN entries but they are for this process, so it's OK   */
  /* Otherwise, CPU might have ASN entries, but they are for another process.  Since we can't flush entries for     */
  /*   that single ASN, we flush everything and reset all the asnelements for this CPU to NULL                      */

  asnelement = asnarray + oz_hw_cpu_getcur () * asn_maxasn + newprctx -> asn;	// point to element for the process' ASN
  if (*asnelement == NULL) *asnelement = newprctx;				// if TLB is clean, just remember who is there now
  else if (*asnelement != newprctx) {						// if TLB is dirty with another process, ...
    OZ_HWAXP_MTPR_TBIAP ();							//   wipe the bad entries out
    memset (asnelement - newprctx -> asn, 0, asn_maxasn * sizeof *asnelement);	//   we just did a TBIAP on this CPU
										//   so all ASN's are reset
    *asnelement = newprctx;							//   remember which proc has TLBs loaded on this ASN
  }

  /* Flush other stuff in the CPU */

  OZ_HWAXP_IMB ();								// make sure we start executing new instructions
  OZ_HW_MB;									// might as well, anything else?

  /* Verify PTBR changed like we wanted */

  ptbr = OZ_HWAXP_MFPR_PTBR ();
  if (ptbr != newprctx -> l1ptpp) {
    oz_crash ("oz_hw_process_switchctx: newprctx %p -> l1ptpp %X != ptbr %X", newprctx, newprctx -> l1ptpp, ptbr);
  }
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
  Long cpuidx;
  OZ_Quota *quota;
  Prctx *prctx;
  uLong pm;

  prctx = process_hw_ctx;

  /* Make sure we're not trying to wipe the system process */

  if (prctx == system_process_hw_ctx) oz_crash ("oz_hw_process_termctx: trying to terminate system process");

  /* Switch to system global L1PT because we are about to wipe out this process' page tables */

  oz_knl_process_setcur (oz_s_systemproc);			// set up system process as current
  cpuidx = oz_hw_cpu_getcur ();					// too bad we can't just wipe this one process' TLBs
  asnarray[cpuidx*asn_maxasn+prctx->asn] = (void *)(-1LL);	// any new prctx will invalidate on this one

  /* Clear out the old L1PT page in case we try to use it again by accident */

  oz_hw_phys_initpage (prctx -> l1ptpp, 0);

  /* Free off the old L1PT page */

  pm = oz_hw_smplock_wait (&oz_s_smplock_pm);			// oz_knl_phymem_freepage must be called at smplock level pm
  oz_knl_phymem_freepage (prctx -> l1ptpp);			// free the physical page
  oz_hw_smplock_clr (&oz_s_smplock_pm, pm);			// restore smplock level

  /* Release quota for that physical memory page */

  quota = oz_knl_section_getquota (prctx -> ptsec);
  if (quota != NULL) oz_knl_quota_credit (quota, OZ_QUOTATYPE_PHM, 1, -1);

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

  OZ_HW_MB;
  do {
    sts = prctx -> ptmap;
    if ((sts != OZ_PENDING) && (sts != OZ_STARTED)) return (sts);
  } while (!oz_hw_atomic_setif_ulong (&(prctx -> ptmap), OZ_STARTED, OZ_PENDING));
  OZ_HW_MB;

  /* Map the pagetable section */

  npages = nptpages;										/* number of pages in the section */
  svpage = l3ptpage;										/* virtual page to start it at */
  sts = oz_knl_process_mapsection (prctx -> ptsec, &npages, &svpage, OZ_MAPSECTION_EXACT, OZ_PROCMODE_KNL, OZ_HW_PAGEPROT_KW);

  /* If this is a new process (not a copied process), create the oz_sys_pdata array and init it      */
  /* If it is a copied process, the oz_sys_pdata array will be copied because it is a normal section */

  if (newprocess && (sts == OZ_SUCCESS)) {
    npages = pdata_npages;									/* one for each mode (kernel, user) */
    svpage = pdata_svpage;									/* just below the user image spot */
    sts = oz_knl_section_create (NULL, npages, 0, OZ_SECTION_TYPE_ZEROES, NULL, &pdatasec);	/* create zero-filled section */
    if (sts == OZ_SUCCESS) {
      sts = oz_knl_process_mapsection (pdatasec, &npages, &svpage, OZ_MAPSECTION_EXACT, OZ_PROCMODE_KNL, OZ_HW_PAGEPROT_KW);
      oz_knl_section_increfc (pdatasec, -1);
      if (sts == OZ_SUCCESS) sts = oz_sys_condhand_try (init_pdata, NULL, oz_sys_condhand_rtnanysig, NULL);
      if (sts == OZ_SUCCESS) sts = oz_knl_handletbl_create ();	/* create handle table for this process */
    }
  }
  OZ_HW_MB;							/* make sure other cpu's see mapsection results before we set ptmap=status */
  prctx -> ptmap = sts;						/* tell all other cpu's the mapping was done */
  return (sts);
}

/* This is a separate routine in a 'try'/'catch' handler in case there is no memory to fault the pdata pages in */

static OZ_Hw_pageprot const rwpageprots[] = { OZ_HW_PAGEPROT_KW, OZ_HW_PAGEPROT_UW };

static uLong init_pdata (void *dummy)

{
  int i;
  uLong sts;

  for (i = 0; i <= OZ_PROCMODE_MAX - OZ_PROCMODE_MIN; i ++) {
    oz_sys_pdata_array[i].data.rwpageprot = rwpageprots[i];
    oz_sys_pdata_array[i].data.procmode   = i + OZ_PROCMODE_MIN;
    oz_sys_pdata_array[i].data.pprocmode  = i + OZ_PROCMODE_MIN;
    if (rwpageprots[i] != OZ_HW_PAGEPROT_KW) {
      sts = oz_knl_section_setpageprot ((sizeof oz_sys_pdata_array[i]) >> OZ_HW_L2PAGESIZE, pdata_svpage + i * ((sizeof oz_sys_pdata_array[i]) >> OZ_HW_L2PAGESIZE), rwpageprots[i], NULL, NULL);
      if (sts != OZ_SUCCESS) break;
    }
  }

  return (sts);
}

void oz_hw_process_validate (void *process_hw_ctx, OZ_Process *process)

{
  Prctx *prctx;

  prctx = process_hw_ctx;

  if (prctx != system_process_hw_ctx) {
    OZ_KNL_CHKOBJTYPE (prctx -> ptsec, OZ_OBJTYPE_SECTION);
  }
}
