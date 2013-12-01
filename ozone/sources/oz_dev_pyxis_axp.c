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

/************************************************************************/
/*									*/
/*  Routines to access I/O via the Pyxis chipset (21174)		*/
/*									*/
/************************************************************************/

#define _OZ_DEV_PCI_C
#define _OZ_DEV_ISA_C

#include "ozone.h"
#include "oz_dev_isa.h"
#include "oz_dev_pci.h"
#include "oz_knl_hw.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_objtype.h"
#include "oz_knl_phymem.h"
#include "oz_knl_sdata.h"
#include "oz_knl_section.h"
#include "oz_knl_status.h"
#include "oz_sys_xprintf.h"

#define VIRTUAL_MAP 10			// zero: statically map entire pyxis_pci struct
					// else: number of pages to preallocate for 
					//       mapping based on translation-not-valid faults

typedef struct Pcibus Pcibus;

#define MAX_PCI_BUSSES 4		// max number of busses (bridges + 1) that we can handle

#define IOSIZE 0x100000			// assign block of 1M I/O addresses for each bus
#define MEMSIZE 0x1000000		// assign block of 16M mem addresses for each bus

#define PAGESIZE (1 << OZ_HW_L2PAGESIZE)
#define PTESPERPAGE (PAGESIZE / sizeof (OZ_Pagentry))

#define MAST_8259_APORT 0x20
#define MAST_8259_DPORT 0x21
#define SLAV_8259_APORT 0xA0
#define SLAV_8259_DPORT 0xA1

#define PCI_FUNC_MAX 8	// 0..7

#define ISA_IPL 21			// CPU interrupt priority level for all ISA interrupts
#define PCI_IPL 21			// CPU interrupt priority level for all PCI interrupts

#define ISA_IRQ_MAX 16	// 0..15	// PC-style ISA bus interrupts just like PC IRQ's
#define PCI_IRQ_MAX 32	// 0..31	// one interrupt per bit in the PYXIS_INT_MASK

#define PYXIS_IRQ_BASE ISA_IRQ_MAX	// leave room for ISA IRQ's
					// this is also how the interrupt vectors are set up in the SCB
					// 0x800..0x8F0 are for the ISA interrupts
					// 0x900..0xAF0 are for the PCI interrupts

#define PCI_SIZE_BYTE 0x00
#define PCI_SIZE_WORD 0x08
#define PCI_SIZE_LONG 0x18

#define pyxis_ctrl  *((uLong volatile *)(pyxis_pci -> cfg_21174_csr + 0x0100))
#define   FILL_ERR_EN 0x200
#define hae_mem_csr *((uLong volatile *)(pyxis_pci -> cfg_21174_csr + 0x0400))
#define hae_io_csr  *((uLong volatile *)(pyxis_pci -> cfg_21174_csr + 0x0440))
#define hae_cfg_csr *((uLong volatile *)(pyxis_pci -> cfg_21174_csr + 0x0480))

#define pyxis_err      *((uLong volatile *)(pyxis_pci -> cfg_21174_csr + 0x8200))
#define pyxis_stat     *((uLong volatile *)(pyxis_pci -> cfg_21174_csr + 0x8240))
#define err_mask       *((uLong volatile *)(pyxis_pci -> cfg_21174_csr + 0x8280))
#define pyxis_syn      *((uLong volatile *)(pyxis_pci -> cfg_21174_csr + 0x8300))
#define pyxis_err_data *((uLong volatile *)(pyxis_pci -> cfg_21174_csr + 0x8308))
#define mear           *((uLong volatile *)(pyxis_pci -> cfg_21174_csr + 0x8400))
#define mesr           *((uLong volatile *)(pyxis_pci -> cfg_21174_csr + 0x8440))
#define pci_err0       *((uLong volatile *)(pyxis_pci -> cfg_21174_csr + 0x8800))
#define pci_err1       *((uLong volatile *)(pyxis_pci -> cfg_21174_csr + 0x8840))
#define pci_err2       *((uLong volatile *)(pyxis_pci -> cfg_21174_csr + 0x8880))

#define w0_base *((uLong volatile *)(pyxis_pci -> cfg_addrxlate + 0x0400))	// base PCI address for the window
#define w1_base *((uLong volatile *)(pyxis_pci -> cfg_addrxlate + 0x0500))
#define w2_base *((uLong volatile *)(pyxis_pci -> cfg_addrxlate + 0x0600))
#define w3_base *((uLong volatile *)(pyxis_pci -> cfg_addrxlate + 0x0700))
#define   W_EN 0x1								// - set: enables window
#define   W_BASE_SG 0x2								// - set: scatter/gather; clr: direct map
#define   MEMCS_EN 0x4
#define   DAC_ENABLE 0x8
#define w0_mask *((uLong volatile *)(pyxis_pci -> cfg_addrxlate + 0x0440))	// comparing masks bits
#define w1_mask *((uLong volatile *)(pyxis_pci -> cfg_addrxlate + 0x0540))	// - bit clr: compare; set: ignore
#define w2_mask *((uLong volatile *)(pyxis_pci -> cfg_addrxlate + 0x0640))
#define w3_mask *((uLong volatile *)(pyxis_pci -> cfg_addrxlate + 0x0740))
#define t0_base *((uLong volatile *)(pyxis_pci -> cfg_addrxlate + 0x0480))	// base memory address for the window >> 2
#define t1_base *((uLong volatile *)(pyxis_pci -> cfg_addrxlate + 0x0580))
#define t2_base *((uLong volatile *)(pyxis_pci -> cfg_addrxlate + 0x0680))
#define t3_base *((uLong volatile *)(pyxis_pci -> cfg_addrxlate + 0x0780))

/* DMA mapping context struct */

struct OZ_Dev_Pci_Dma32map {
  OZ_Objtype objtype;			// OZ_OBJTYPE_PCIDMA32MAP
  uLong npages;				// number of pages we can map
					// scatter/gather: number of map registers starting at basepciaddr
					// direct: number of elements in 'ieeedma32' array
  uLong basepciaddr;			// scatter/gather: start of allocated map registers
					// direct: not used
  uLong mappciaddr;			// pci address of the ieeedma32 array
  Long volatile busy;			// 0: available; 1: busy
  OZ_Ieeedma32 ieeedma32[1];		// ieeedma32 array to give to the controller
};

/* This 32GB struct is mapped at the very top of VA's */
/* We don't use the first 20GB, nor the last 256MB    */
/* so we require only 12GB of L1 pt space             */

#define PCI_DATA_PHYADDR 0x8500000000

static struct {
//	uByte volatile pci_sparse_mem_1[1UL<<34];	// PA 80.0000.0000
//	uByte volatile pci_sparse_mem_2[1UL<<32];	// PA 84.0000.0000
	uByte volatile pci_sparse_mem_3[1UL<<31];	// PA 85.0000.0000  VA FFFF.FFFC.0000.0000
	uByte volatile pci_sparse_io_A[1UL<<30];	// PA 85.8000.0000  VA FFFF.FFFC.8000.0000
	uByte volatile pci_sparse_io_B[1UL<<30];	// PA 85.C000.0000  VA FFFF.FFFC.C000.0000
	uByte volatile pci_dense_mem[1UL<<32];		// PA 86.0000.0000  VA FFFF.FFFD.0000.0000
	uByte volatile cfg_space[1UL<<29];		// PA 87.0000.0000  VA FFFF.FFFE.0000.0000
	uByte volatile cfg_iack[1UL<<29];		// PA 87.2000.0000  VA FFFF.FFFE.2000.0000
	uByte volatile cfg_21174_csr[1UL<<28];		// PA 87.4000.0000  VA FFFF.FFFE.4000.0000
	uByte volatile cfg_memctlcsr[1UL<<28];		// PA 87.5000.0000  VA FFFF.FFFE.5000.0000
	uByte volatile cfg_addrxlate[1UL<<28];		// PA 87.6000.0000  VA FFFF.FFFE.6000.0000
//	uByte volatile filler[1<<28];			// PA 87.7000.0000
} *pyxis_pci;					// virtual address it is mapped to (FFFF.FFFC.0000.0000)

#define NDATAPAGES ((PAGESIZE - 1 + sizeof *pyxis_pci) / PAGESIZE)
#define NL3PTPAGES ((PTESPERPAGE - 1 + NDATAPAGES) / PTESPERPAGE)
#define NL2PTPAGES ((PTESPERPAGE - 1 + NL3PTPAGES) / PTESPERPAGE)

/* And of course it has some wacko registers off somewhere else - just use OZ_HWAXP_LDQP/OZ_HWAXP_STQP to access */

#define PYXIS_INTCTL_PHYPAGE (0x87A0000000ULL >> OZ_HW_L2PAGESIZE)

#define PYXIS_INT_REQ   0x87A0000000	// interrupt request
#define PYXIS_INT_MASK  0x87A0000040	// interrupt mask
#define PYXIS_INT_HILO  0x87A00000C0	// interrupt high/low select
#define PYXIS_INT_ROUTE 0x87A0000140	// interrupt routine select
#define PYXIS_INT_GPO   0x87A0000180	// general purpose output
#define PYXIS_INT_CNFG  0x87A00001C0	// interrupt configuration
#define PYXIS_RT_COUNT  0x87A0000200	// realtime counter
#define PYXIS_INT_TIME  0x87A0000240	// interrupt time
#define PYXIS_IIC_CTRL  0x87A00002C0	// i2c control

/* These keep track of L3 pagetable pages used to map pyxis_pci struct */

typedef struct Ioptcb Ioptcb;
struct Ioptcb { Ioptcb *next;
                uQuad *mappedat;	// points to beginning of L3 pagetable page
                uQuad *mappedby;	// points to single L2 pagetable entry
                OZ_Mempage phypage;
              };

/* We use pci_sparse_mem_3 space to access memory in the I/O cards */
/* We cram everything in the upper 64MB of PCI space so we don't   */
/* have to keep changing the hae_mem_csr contents                  */

#define PCI_SPARSE_MEM_3_HAEBITS 26

/* Interrupt structs */

struct OZ_Dev_Pci_Irq { OZ_Objtype objtype;		// OZ_OBJTYPE_PCIIRQ
                        OZ_Dev_Pci_Irq *next;
                        void (*entry) (void *param, OZ_Mchargs *mchargs);
                        void *param;
                        int irq;
                        OZ_Smplock smplock;
                      };

struct OZ_Dev_Isa_Irq { OZ_Objtype objtype;		// OZ_OBJTYPE_ISAIRQ
                        OZ_Dev_Isa_Irq *next;
                        void (*entry) (void *param, OZ_Mchargs *mchargs);
                        void *param;
                        uLong irq;
                        OZ_Smplock smplock;
                      };

/* PCI Config struct */

#define BASADRJUNK 15	// junk bits on bottom of base address registers

struct OZ_Dev_Pci_Conf { OZ_Objtype objtype;			// OZ_OBJTYPE_PCICONF
                         OZ_Dev_Pci_Conf *next;			// next in pcibusses[busidx].pciconfs list
                         uLong findflags;			// flags from oz_dev_pci_find_...
                         int busidx;				// pcibusses element
                         uLong pcidev, pcifunc;			// addressing
                         int accepted;				// 0: driver init rejected it
								// 1: driver init accepted it
                         char addrsuffix[16], addrdescrip[32];
                         struct { uLong size, addr; } basadr[6];
                       };


/* Internal data and routines */

static int initialized = 0;

static OZ_Dev_Isa_Irq *isairqlists[ISA_IRQ_MAX];
static OZ_Dev_Pci_Irq *pciirqlists[PCI_IRQ_MAX];

#if VIRTUAL_MAP
static Ioptcb  *ioptcb_free  = NULL;
static Ioptcb  *ioptcb_l3_qh = NULL;
static Ioptcb **ioptcb_l3_qt = &ioptcb_l3_qh;
static Ioptcb   ioptcb_prealloc[VIRTUAL_MAP];
#endif

static OZ_Mempage pci_data_vpage = 0;		// vpage for the PCI data space
static OZ_Mempage pci_l3pt_vpage = 0;		// vpage for the L3 mapping pages
static OZ_Mempage pci_l2pt_vpage = 0;		// vpage for the L2 mapping pages
static OZ_Mempage const pci_data_ppage = PCI_DATA_PHYADDR >> OZ_HW_L2PAGESIZE;

static uLong directoffs;			// pci dma map offset in bytes (pciaddr=phyaddr+directoffs)
static uLong topphyaddr;			// top legal physical address (exclusive)
static uQuad volatile int_mask_copy;		// internal copy of PYXIS_INT_MASK

struct Pcibus { OZ_Dev_Pci_Conf *bridge;		// bridge device, NULL for primary bus
                OZ_Dev_Pci_Conf *pciconfs;		// devices configured on this bus
                OZ_Dev_Pci_Conf loaddr;			// lowest addresses to assign to devs on the bus (inclusive)
							// - .basadr[0].addr is for memory
							// - .basadr[1].addr is for io (and it has low bit set)
                uLong hiaddrs[2];			// highest addresses to assign to devs on the bus (exclusive)
							// - hiaddrs[0] is for memory
							// - hiaddrs[1] is for io (low bit is not set)
                uLong haecfg, pcibus, devmax;		// device numbers to use to address the bus
                uLong iobas, iolim, membas, memlim;	// io/mem register limits for the bus (both limits inclusive)
              };
static int numpcibusses = 0;				// number of pcibusses elements filled in
static Pcibus pcibusses[MAX_PCI_BUSSES];

static void (*old_tnv_ent) (void *param, OZ_Mchargs *mchargs);
static void *old_tnv_prm;

static int found_21152 (void *dummy, uLong didvid, int func, OZ_Dev_Pci_Conf *pciconf, char const *addrsuffix, char const *addrdescrip);
static uLong errorsoff (int busidx);
static void errorson (int busidx, uLong saverr);
static void founddev (OZ_Dev_Pci_Conf *pciconf, 
                      uLong didvid, 
                      int func, 
                      int (*entry) (void *param, 
                                    uLong didvid, 
                                    int func, 
                                    OZ_Dev_Pci_Conf *pciconf, 
                                    char const *addrsuffix, 
                                    char const *addrdescrip), 
                      void *param);
static void pci_interrupt (void *irqv, OZ_Mchargs *mchargs);
static void isa_interrupt (void *irqv, OZ_Mchargs *mchargs);
static void pyxis_mapiopage (void *dummy, OZ_Mchargs *mchargs);
static Ioptcb *get_ioptcb (OZ_Mempage vpage);
static int miata_pci_irq (OZ_Dev_Pci_Conf *pciconf, uByte intpin);

#if 000
static void mcheckhandler (void *dummy, OZ_Mchargs *mchargs)

{
  uQuad whami;
  uQuad *cpuslot, logoutpa, logoutq0, logoutq1, logoutq2, logoutsz;

  oz_knl_printk ("oz_dev_pyxis mcheckhandler*: PC %QX, P4 %QX\n", mchargs -> pc, mchargs -> p4);

  whami = OZ_HWAXP_MFPR_WHAMI ();
  cpuslot = (uQuad *)((whami * oz_hwaxp_hwrpb -> cpusltsiz) + oz_hwaxp_hwrpb -> cpusltofs + (OZ_Pointer)oz_hwaxp_hwrpb);
  oz_knl_printk ("oz_dev_pyxis mcheckhandler*: whami %u, cpuslot %p\n", whami, cpuslot);

  logoutsz  = cpuslot[224/8];
  logoutpa  = cpuslot[216/8];
  oz_knl_printk ("oz_dev_pyxis mcheckhandler*: logoutsz %QX, logoutpa %QX\n", logoutsz, logoutpa);

  logoutpa += mchargs -> p4;
  logoutq0  = OZ_HWAXP_LDQP (logoutpa +  0);
  logoutq1  = OZ_HWAXP_LDQP (logoutpa +  8);
  logoutq2  = OZ_HWAXP_LDQP (logoutpa + 16);
  oz_knl_printk ("  %QX: %QX %QX %QX\n", logoutpa, logoutq0, logoutq1, logoutq2);

  // machine check code 20F: pyxis master abort

  if ((logoutq2 == 0x20F) && ((logoutq1 >> 32) == 0x1A0)) {
    logoutq0 = OZ_HWAXP_LDQP (logoutpa + 0x1B0);
    logoutq1 = OZ_HWAXP_LDQP (logoutpa + 0x1B8);
    logoutq2 = OZ_HWAXP_LDQP (logoutpa + 0x1C0);
    oz_knl_printk ("  pyxis_err: %LX, pyxis_stat %LX, err_mask %LX\n", (uLong)logoutq0, (uLong)logoutq1, (uLong)logoutq2);
    logoutq0 = OZ_HWAXP_LDQP (logoutpa + 0x1E0);
    logoutq1 = OZ_HWAXP_LDQP (logoutpa + 0x1E8);
    logoutq2 = OZ_HWAXP_LDQP (logoutpa + 0x1F8);
    oz_knl_printk ("  pci_err0: %LX, pci_err1 %LX, pci_err2 %LX\n", (uLong)logoutq0, (uLong)logoutq1, (uLong)logoutq2);
  }

  while (1) {}
  OZ_HWAXP_HALT ();
}
#endif

/************************************************************************/
/*									*/
/*  Called by early bootcode to set up I/O environment			*/
/*									*/
/*    Input:								*/
/*									*/
/*	phymem_count = count of free physical memory pages		*/
/*	phymem_start = starting physical memory page number		*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_dev_pyxis_early = count of remaining free physical memory pages
/*									*/
/************************************************************************/

OZ_Mempage oz_dev_pyxis_early (OZ_Mempage phymem_count, OZ_Mempage phymem_start)

{
  int i, j, n;
  Ioptcb *ioptcb;
  OZ_Mempage phypage, svpage;
  OZ_Pagentry *pte, savepte;
  OZ_Pointer irq;
  uByte mdport, sdport, reg, *vaddr;
  uLong base, mask, sts, top;
  uQuad intmask, topva, totalmembytes;

  oz_knl_printk ("oz_dev_pyxis_early: initializing\n");

#if 000
  oz_hwaxp_scb_setc (0x660, mcheckhandler, NULL, NULL, NULL);
#endif

  /* Save mapping addresses = top 12GB of virt address space, rounded down to L1 granularity (eg, 8GB) */

  topva  = -sizeof *pyxis_pci;				// this is how much we need at top of memory
  topva &= -PAGESIZE * PTESPERPAGE * PTESPERPAGE;	// round it down to an L1 pte boundary
  pyxis_pci = (void *)topva;

  pci_data_vpage = OZ_HW_VADDRTOVPAGE (pyxis_pci);
  pci_l3pt_vpage = (pci_data_vpage / PTESPERPAGE) + OZ_HW_VADDRTOVPAGE (oz_hwaxp_l3ptbase);
  pci_l2pt_vpage = (pci_l3pt_vpage / PTESPERPAGE) + OZ_HW_VADDRTOVPAGE (oz_hwaxp_l3ptbase);

  /* Allocate and initialize L2 pagetable pages */

  phymem_count -= NL2PTPAGES;
  phypage = phymem_count + phymem_start;
  for (i = 0; i < NL2PTPAGES; i ++) {
    j = (pci_data_vpage / PTESPERPAGE / PTESPERPAGE) + i;
    if ((oz_hwaxp_l1ptbase[j] & ~(uQuad)(OZ_HWAXP_PTE_X_RP << OZ_HWAXP_PTE_V_RP)) != 0) {
      oz_crash ("oz_dev_pyxis_init: oz_hwaxp_l1ptbase[%X] is non-zero (%QX)", j, oz_hwaxp_l1ptbase[j]);
    }
    oz_hwaxp_l1ptbase[j] = (((uQuad)(phypage + i)) << 32) + 0x1111; // KW, global, active
  }

#if VIRTUAL_MAP

  /* Set the L2 pages to have all 'not active' entries that are KW protection */
  /* This will give us 'translation not valid' faults on all accesses         */

  pte = OZ_HW_VPAGETOVADDR (pci_l2pt_vpage);
  memset (pte, 0, NL2PTPAGES * PAGESIZE);
  for (i = 0; i < NL3PTPAGES; i ++) {
    pte[i] = 0x1110; // KW, global, inactive
  }

  /* Preallocate some pages for L3 pagetable */

  for (i = 0; i < VIRTUAL_MAP; i ++) {
    ioptcb = ioptcb_prealloc + i;
    ioptcb -> next = ioptcb_free;
    ioptcb -> phypage = -- phymem_count + phymem_start;
    ioptcb_free = ioptcb;
  }

  /* Set up translation-not-valid handler to dynamically map the I/O pages */

  oz_hwaxp_scb_setc (0x090, pyxis_mapiopage, NULL, &old_tnv_ent, &old_tnv_prm);

#else

  /* Now fill the L2 pages with L3 pages */

  pte = OZ_HW_VPAGETOVADDR (pci_l2pt_vpage);
  memset (pte, 0, NL2PTPAGES * PAGESIZE);

  for (i = 0; i < NL3PTPAGES; i ++) {
    phypage = -- phymem_count + phymem_start;
    pte[i] = (((uQuad)phypage) << 32) + 0x1111; // KW, global, active
  }

  /* Fill the L3 pages with mapping to the Pyxis registers */

  pte = OZ_HW_VPAGETOVADDR (pci_l3pt_vpage);
  memset (pte, 0, NL3PTPAGES * PAGESIZE);

  for (i = 0; i < NDATAPAGES; i ++) {
    pte[i] = ((((uQuad)pci_data_ppage) + i) << 32) + 0x1171; // KW, global, active, max granularity
  }

#endif

  /* Enable Pyxis error interrupts */

  oz_knl_printk ("oz_dev_pyxis_init*: original err_mask %LX\n", err_mask);

#if 000
  OZ_HW_MB;
  err_mask = 0;
  OZ_HW_MB;
  OZ_HW_MB;
  err_mask;
  OZ_HW_MB;
#endif

  /* Programming hae_mem_csr with all 1's will fill the top bits with ones */

  top = -1 << PCI_SPARSE_MEM_3_HAEBITS;
  hae_mem_csr = -1;
  OZ_HW_MB;
  OZ_HW_MB;
  hae_mem_csr;
  OZ_HW_MB;

  /* Set up DMA mapping windows -                                       */
  /* We try to keep away from the bottom most and top most addresses in */
  /* case there's something funky there (some dev that can't be moved)  */

  totalmembytes = ((uQuad)(phymem_start + phymem_count)) << OZ_HW_L2PAGESIZE;

  t0_base = w0_mask = w0_base = 0;
  t1_base = w1_mask = w1_base = 0;
  t2_base = w2_mask = w2_base = 0;
  t3_base = w3_mask = w3_base = 0;

  /* 1GB and less is real easy, just map it at PCI address 2GB thru 3GB */

  if (totalmembytes <= (1ULL << 30)) {
    directoffs = 2 << 30;
    t0_base = 0;			// window[0]: phyaddr[0..1G] <-> pciaddr[2G..3G]
    w0_mask = (1 << 30) - 1;
    w0_base = (2 << 30) + W_EN;
  }

  /* 2GB and less is almost as easy.  Use 2 windows and map it at PCI address 1GB thru 3GB */

  else if (totalmembytes <= (2ULL << 30)) {
    directoffs = 1 << 30;
    t0_base = 0;			// window[0]: phyaddr[0..1G] <-> pciaddr[1G..2G]
    w0_mask = (1 << 30) - 1;
    w0_base = (1 << 30) + W_EN;
    t1_base = (1 << 30) >> 2;		// window[1]: phyaddr[1G..2G] <-> pciaddr[2G..3G]
    w1_mask = (1 << 30) - 1;
    w1_base = (2 << 30) + W_EN;
  }

  /* That's all we know how to do */

  else oz_crash ("oz_dev_pyxis_init: too stupid to handle %Lu gigabytes", (uLong)(totalmembytes >> 30));

  /* Set up PCI interrupt routine */

  memset (pciirqlists, 0, sizeof pciirqlists);					// all lists start out empty
  int_mask_copy = OZ_HWAXP_LDQP (PYXIS_INT_MASK);				// see what's enabled to start
  for (irq = PYXIS_IRQ_BASE; irq < PYXIS_IRQ_BASE + PCI_IRQ_MAX; irq ++) {	// set up interrupt routines in SCB
    oz_hwaxp_scb_setc (0x800 + irq * 16, pci_interrupt, (void *)irq, NULL, NULL);
  }

  /* Also set up ISA interrupt routine */

  memset (isairqlists, 0, sizeof isairqlists);
  for (irq = 0; irq < ISA_IRQ_MAX; irq ++) {
    oz_hwaxp_scb_setc (0x800 + irq * 16, isa_interrupt, (void *)irq, NULL, NULL);
  }

  /* Set up parameters for the primary bus */

  memset (pcibusses, 0, sizeof pcibusses);
  pcibusses[0].haecfg = 0;					// pyxis chip uses type 0 addresses for directly connected devs
  pcibusses[0].pcibus = 0;					// pyxis chip uses bus 0 to indicate directly connected devs
  pcibusses[0].devmax = 21;					// pyxis chip handles dev 0..20 directly connected to pci bus
  pcibusses[0].iobas  = 0x10000;				// leave the first 64K for legacy ISA devices, start with 0x10000 for us
  pcibusses[0].iolim  = 0x10000 + IOSIZE - 1;			// give us a whole block of addresses
  pcibusses[0].membas = -1 << PCI_SPARSE_MEM_3_HAEBITS;		// keep memory in single PCI sparse space so we don't have to change HAE
  pcibusses[0].memlim = pcibusses[0].membas + MEMSIZE - 1;	// give us a whole block of addresses

  pcibusses[0].loaddr.objtype = OZ_OBJTYPE_PCICONF;
  pcibusses[0].loaddr.basadr[0].addr = pcibusses[0].membas;	// lowest assignable memory address
  pcibusses[0].loaddr.basadr[1].addr = pcibusses[0].iobas | 1;	// lowest assignable io address
  pcibusses[0].hiaddrs[0] = pcibusses[0].iolim + 1;		// highest assignable memory address (exclusive)
  pcibusses[0].hiaddrs[1] = pcibusses[0].memlim + 1;		// highest assignable io address (exclusive)

  pcibusses[0].pciconfs = &(pcibusses[0].loaddr);		// link so find routines will see it

  numpcibusses = 1;

  /* Return number of pages that are left */

  topphyaddr = oz_hwaxp_topphypage << OZ_HW_L2PAGESIZE;		// but allow for I/O in all physical memory

  return (phymem_count);
}

/************************************************************************/
/*									*/
/*  Boot-time initialization routine					*/
/*									*/
/************************************************************************/

void oz_dev_pyxis_init (void)

{
  if (initialized) return;

  /* Setting this flag also lets the VIRTUAL_MAP routines know */
  /* it is OK now to allocate new physical pages for mapping   */

  initialized = 1;
  oz_knl_printk ("oz_dev_pyxis_init: initializing\n");

  /* Scan for bridges */

  oz_dev_pci_find_didvid (0x00241011, 0, 0, found_21152, NULL);
}

/************************************************************************/
/*									*/
/*  Found a 21152 bridge						*/
/*									*/
/*    Input:								*/
/*									*/
/*	didvid  = didvid of the 21152					*/
/*	func    = 0							*/
/*	pciconf = points to the 21152's primary interface registers	*/
/*									*/
/*    Output:								*/
/*									*/
/*	found_21152 = 1 : we always accept the device			*/
/*	21152 set up and added to the 'pcibusses' array			*/
/*									*/
/************************************************************************/

static int found_21152 (void *dummy, uLong didvid, int func, OZ_Dev_Pci_Conf *pciconf, char const *addrsuffix, char const *addrdescrip)

{
  uWord saved_cr;

  oz_knl_printk ("oz_dev_pyxis_init: 21152 bridge %s\n", addrdescrip);

  saved_cr = oz_dev_pci_conf_inw (pciconf, OZ_DEV_PCI_CONF_W_PCICMD);		// save current command settings
  oz_dev_pci_conf_outw (0, pciconf, OZ_DEV_PCI_CONF_W_PCICMD);			// disable 21152 sensing of any transactions during setup
  oz_dev_pci_conf_outw (0xFFFF, pciconf, OZ_DEV_PCI_CONF_W_PCISTS);		// clear out any error bits

  if (numpcibusses == MAX_PCI_BUSSES) {
    oz_knl_printk ("oz_dev_pyxis_init: - max bridges already defined\n");
    return (1);
  }

  pcibusses[numpcibusses].bridge = pciconf;					// remember where the bridge for this bus is
  pcibusses[numpcibusses].haecfg = 1;						// send out type 1 transactions to get to secondary devices
  pcibusses[numpcibusses].pcibus = numpcibusses;				// use this bus number to get to secondary devices
  pcibusses[numpcibusses].devmax = 16;						// 21152's only do dev 0..15 on secondary bus
  pcibusses[numpcibusses].iobas  = pcibusses[numpcibusses-1].iolim + 1;		// all secondary devs must use io regs in this range
  pcibusses[numpcibusses].iolim  = pcibusses[numpcibusses].iobas + IOSIZE - 1;
  pcibusses[numpcibusses].membas = pcibusses[numpcibusses-1].memlim + 1;	// all secondary devs must use mem regs in this range
  pcibusses[numpcibusses].memlim = pcibusses[numpcibusses].membas + MEMSIZE - 1;

  if (pcibusses[numpcibusses].iolim >= 0x02000000) {				// we're restricting to sparse I/O A space for efficiency
    oz_knl_printk ("oz_dev_pyxis_init: - bridge would overflow I/O space\n");
    return (1);
  }
  if (pcibusses[numpcibusses].memlim >= (uLong)(-MEMSIZE)) {			// make sure we don't come near wrapping around
    oz_knl_printk ("oz_dev_pyxis_init: - bridge would overflow mem space\n");
    return (1);
  }

										// 0C(b): cache line size, leave as is
										// 0D(b): primary latency, leave as is
  oz_dev_pci_conf_outb (pcibusses[pciconf->busidx].pcibus, pciconf, 0x18);	// 18(b): primary bus number
										// - used by devs on secondary bus to access primary
  oz_dev_pci_conf_outb (pcibusses[numpcibusses].pcibus, pciconf, 0x19);		// 19(b): secondary bus number
										// - this is bus number of devs directly connected to secondary bus
  oz_dev_pci_conf_outb (pcibusses[numpcibusses].pcibus, pciconf, 0x1A);		// 1A(b): we aren't supporting any tertiary busses
										// 1B(b): secondary latency, leave as is
  oz_dev_pci_conf_outw (0xFFFF, pciconf, 0x1E);					// 1E(w): secondary status, clear error bits
  oz_knl_printk ("oz_dev_pyxis found_21152*: 3E: %X\n", oz_dev_pci_conf_inw (pciconf, 0x3E));
#if 000
  oz_dev_pci_conf_outw (0x0C23, pciconf, 0x3E);					// 3E(w): forward secondary errors to primary bus
#endif

#if IOSIZE % 4096 != 0
  error : IOSIZE must be multiple of 4K
#endif

  oz_dev_pci_conf_outb (pcibusses[numpcibusses].iobas >>  8, pciconf, 0x1C);	// io base<15:12>  -> conf_1C<07:04>
  oz_dev_pci_conf_outw (pcibusses[numpcibusses].iobas >> 16, pciconf, 0x30);	// io base<31:16>  -> conf_30<15:00>
  oz_dev_pci_conf_outb (pcibusses[numpcibusses].iolim >>  8, pciconf, 0x1D);	// io limit<15:12> -> conf_1D<07:04>
  oz_dev_pci_conf_outw (pcibusses[numpcibusses].iolim >> 16, pciconf, 0x32);	// io limit<31:16> -> conf_30<15:00>

#if MEMSIZE % 1024*1024 != 0
  error MEMSIZE must be multiple of 1M
#endif

  oz_dev_pci_conf_outw (pcibusses[numpcibusses].membas >> 16, pciconf, 0x20);	// mem base<31:20>  -> conf_20<15:04>
  oz_dev_pci_conf_outw (pcibusses[numpcibusses].memlim >> 16, pciconf, 0x22);	// mem limit<31:20> -> conf_22<15:04>

						// by setting base gt limit, we say there is no prefetchable memory on sub-bus
  oz_dev_pci_conf_outw (0xDEAD, pciconf, 0x24);					// prefetch base<31:20>  -> conf_24<15:04>
  oz_dev_pci_conf_outl (0xDEAD, pciconf, 0x28);					// prefetch base<63:32>  -> conf_28<15:00>
  oz_dev_pci_conf_outw (0xBEEF, pciconf, 0x26);					// prefetch limit<31:20> -> conf_26<15:04>
  oz_dev_pci_conf_outl (0xBEEF, pciconf, 0x2C);					// prefetch limit<63:32> -> conf_28<15:00>

  saved_cr |= 7;								// force I/O, mem and dma enables on
  oz_dev_pci_conf_outw (saved_cr, pciconf, OZ_DEV_PCI_CONF_W_PCICMD);		// restore command settings

  pcibusses[numpcibusses].loaddr.objtype = OZ_OBJTYPE_PCICONF;
  pcibusses[numpcibusses].loaddr.busidx  = numpcibusses;			// set address limits to assign to controller registers
  pcibusses[numpcibusses].loaddr.basadr[0].addr = pcibusses[numpcibusses].membas;
  pcibusses[numpcibusses].loaddr.basadr[1].addr = pcibusses[numpcibusses].iobas | 1;
  pcibusses[numpcibusses].hiaddrs[0] = pcibusses[numpcibusses].memlim + 1;
  pcibusses[numpcibusses].hiaddrs[1] = pcibusses[numpcibusses].iolim + 1;
  pcibusses[numpcibusses].pciconfs = &(pcibusses[numpcibusses].loaddr);

  oz_knl_printk ("oz_dev_pyxis_init: - pirmary bus %u, secondary bus %u\n", 
	pcibusses[pciconf->busidx].pcibus, pcibusses[numpcibusses].pcibus);

  oz_knl_printk ("oz_dev_pyxis_init: - io %X..%X, mem %X..%X\n", 
	pcibusses[numpcibusses].iobas, pcibusses[numpcibusses].iolim, 
	pcibusses[numpcibusses].membas, pcibusses[numpcibusses].memlim);

  numpcibusses ++;								// one more element of pcibusses is filled in

  return (1);
}

/************************************************************************/
/*									*/
/*  Determine if a PCI bus is present					*/
/*									*/
/************************************************************************/

int oz_dev_pci_present (void)

{
  return (1);
}

/************************************************************************/
/*									*/
/*  Scan for a given vendor-id/device-id				*/
/*									*/
/*    Input:								*/
/*									*/
/*	didvid = pci device and vendor id to search for			*/
/*	func   = bits describing which bar's are valid			*/
/*	entry  = callback routine entrypoint				*/
/*	param  = callback routine parameter				*/
/*									*/
/*	smplevel = softint						*/
/*									*/
/*    Note:								*/
/*									*/
/*	The callbacks are always synchronous in this version but might 	*/
/*	not always be.							*/
/*									*/
/************************************************************************/

void oz_dev_pci_find_didvid (uLong didvid, 
                             int func, 
                             uLong flags, 
                             int (*entry) (void *param, 
                                           uLong didvid, 
                                           int func, 
                                           OZ_Dev_Pci_Conf *pciconf, 
                                           char const *addrsuffix, 
                                           char const *addrdescrip), 
                             void *param)

{
  int busidx, funcmax, funcmin;
  OZ_Dev_Pci_Conf pciconf;
  uLong saverr;

  funcmin = 0;
  funcmax = PCI_FUNC_MAX;
  if (func >= 0) {
    funcmin = func;
    funcmax = func + 1;
  }

  memset (&pciconf, 0, sizeof pciconf);
  pciconf.objtype = OZ_OBJTYPE_PCICONF;
  pciconf.findflags = flags;
  for (busidx = 0; busidx < numpcibusses; busidx ++) {
    pciconf.busidx = busidx;
    saverr = errorsoff (busidx);
    for (pciconf.pcidev = 0; pciconf.pcidev < pcibusses[busidx].devmax; pciconf.pcidev ++) {
      for (pciconf.pcifunc = funcmin; pciconf.pcifunc < funcmax; pciconf.pcifunc ++) {
        if (oz_dev_pci_conf_inl (&pciconf, OZ_DEV_PCI_CONF_L_DIDVID) == didvid) {
          founddev (&pciconf, didvid, func, entry, param);
        }
      }
    }
    errorson (busidx, saverr);
  }
}

static uLong errorsoff (int busidx)

{
  uLong saverr;

  OZ_HW_MB;
  if (pcibusses[busidx].bridge == NULL) {
    saverr = pyxis_ctrl;
    OZ_HW_MB;
    pyxis_ctrl &= ~FILL_ERR_EN;
    OZ_HW_MB;
    OZ_HW_MB;
    pyxis_ctrl;
    OZ_HW_MB;
  } else {
    saverr = oz_dev_pci_conf_inw (pcibusses[busidx].bridge, 0x3E);
    oz_dev_pci_conf_outw (0, pcibusses[busidx].bridge, 0x3E);
  }
  OZ_HW_MB;
  return (saverr);
}

static void errorson (int busidx, uLong saverr)

{
  OZ_HW_MB;
  if (pcibusses[busidx].bridge == NULL) {
    pyxis_ctrl = saverr;
    OZ_HW_MB;
    OZ_HW_MB;
    pyxis_ctrl;
    OZ_HW_MB;
  } else {
    oz_dev_pci_conf_outw (0xFFFF, pcibusses[busidx].bridge, 0x1E);
    oz_dev_pci_conf_outw (saverr, pcibusses[busidx].bridge, 0x3E);
  }
  OZ_HW_MB;
}

static void founddev (OZ_Dev_Pci_Conf *pciconf, 
                      uLong didvid, 
                      int func, 
                      int (*entry) (void *param, 
                                    uLong didvid, 
                                    int func, 
                                    OZ_Dev_Pci_Conf *pciconf, 
                                    char const *addrsuffix, 
                                    char const *addrdescrip), 
                      void *param)

{
  int busidx, i, j, k;
  OZ_Dev_Pci_Conf *pciconfn, *pciconfx, *pciconfy;
  uLong basadrend, basadrofs, basadrone, basadrsav, basadrsiz, basadrzer, hiaddr, basadrtst;

  oz_knl_printk ("oz_dev_pci_find: found %8.8X at bus/dev/func %u/%u/%u\n", 
	didvid, pcibusses[pciconf->busidx].pcibus, pciconf -> pcidev, pciconf -> pcifunc);

  busidx = pciconf -> busidx;

  /* See if we found it before (maybe a second driver is calling for same didvid).  If so, don't configure it twice. */

  for (pciconfn = pcibusses[busidx].pciconfs; pciconfn != NULL; pciconfn = pciconfn -> next) {
    if ((pciconfn -> busidx  == pciconf -> busidx) 
     && (pciconfn -> pcidev  == pciconf -> pcidev) 
     && (pciconfn -> pcifunc == pciconf -> pcifunc)) {
       if (!(pciconfn -> accepted)) goto calldriverinit;
       oz_knl_printk ("oz_dev_pci_find: - already configured\n");
       return;
     }
  }

  /* Set up a new pciconf struct for the device and link to list */

  pciconfn  = OZ_KNL_NPPMALLOC (sizeof *pciconfn);					// malloc a new struct
  *pciconfn = *pciconf;									// copy temp into it
  pciconfn -> accepted = 1;								// mark accepted for now so we won't recurse on it

  if (func >= 0) oz_sys_sprintf (sizeof pciconfn -> addrsuffix, pciconfn -> addrsuffix, "%u_%u", 
	pcibusses[pciconf->busidx].pcibus, pciconf -> pcidev);
  else oz_sys_sprintf (sizeof pciconfn -> addrsuffix, pciconfn -> addrsuffix, "%u_%u_%u", 
	pcibusses[pciconf->busidx].pcibus, pciconf -> pcidev, pciconf -> pcifunc);

  oz_sys_sprintf (sizeof pciconfn -> addrdescrip, pciconfn -> addrdescrip, "bus/dev/func %u/%u/%u", 
	pcibusses[pciconf->busidx].pcibus, pciconf -> pcidev, pciconf -> pcifunc);

  pciconfn -> next = pcibusses[busidx].pciconfs;					// link it to list of all pciconf's
  pcibusses[busidx].pciconfs = pciconfn;

  /* Assign addresses for its registers in the range we want */

  for (i = 0; i < 6; i ++) {								// scan through all base address registers
    if (!(pciconf -> findflags & (1 << i))) continue;					// skip if it doesn't have one
    basadrofs = OZ_DEV_PCI_CONF_L_BASADR0 + i * 4;					// get offset in pciconf space for basadr register
    basadrsav = oz_dev_pci_conf_inl (pciconf, basadrofs);				// save original contents
    oz_dev_pci_conf_outl (basadrsav & BASADRJUNK, pciconf, basadrofs);			// write zeroes to significant bits
    basadrzer = oz_dev_pci_conf_inl (pciconf, basadrofs);				// read what we get
    oz_dev_pci_conf_outl (~BASADRJUNK | (basadrsav & BASADRJUNK), pciconf, basadrofs);	// write ones to sig bits
    basadrone = oz_dev_pci_conf_inl (pciconf, basadrofs);				// read what we get
    oz_dev_pci_conf_outl (basadrsav, pciconf, basadrofs);				// restore in case we barf
    basadrsiz = basadrzer - basadrone;							// zeroes - ones = number of bytes required
    for (basadrtst = BASADRJUNK + 1; basadrtst != 0; basadrtst *= 2) {			// make sure it's a proper power of 2
      if (basadrsiz == basadrtst) goto basadrsiz_ok;
    }
    oz_knl_printk ("oz_dev_pci_find: - bad basadrsiz %LX\n", basadrsiz);
    goto free_pciconfn;
basadrsiz_ok:

    hiaddr = pcibusses[busidx].hiaddrs[basadrsav&1];					// high end of assignable addresses

    for (basadrtst = basadrsiz; basadrtst != 0; basadrtst *= 2) {			// scan for nice place to put it
      for (pciconfx = pcibusses[busidx].pciconfs; pciconfx != NULL; pciconfx = pciconfx -> next) { // scan through existing devices, including self
        for (j = 0; j < 6; j ++) {							// scan their assigned address blocks
          basadrend  = pciconfx -> basadr[j].size + pciconfx -> basadr[j].addr;		// see where its block ends (includes junk bits)
          if ((basadrend ^ basadrsav) & 1) continue;					// see if its IO/MEM flag matches
          basadrend &= ~BASADRJUNK;							// mask off junk bits
          if (!(basadrend & basadrtst)) continue;					// see if it's the multiple boundary we're looking for
          if (basadrend & (basadrtst - 1)) continue;
          if (basadrend >= hiaddr) continue;						// ok, skip if it ends at high address end
          if (basadrend > hiaddr - basadrsiz) continue;					// skip if we would go past high address end using it
          for (pciconfy = pcibusses[busidx].pciconfs; pciconfy != NULL; pciconfy = pciconfy -> next) { // rescan to see if basadrend..basadrend+basadrsiz-1 in use
            for (k = 0; k < 6; k ++) {
              if ((pciconfy -> basadr[k].addr & ~BASADRJUNK) >= basadrend + basadrsiz) continue;
              if ((pciconfy -> basadr[k].addr & ~BASADRJUNK) + pciconfy -> basadr[k].size <= basadrend) continue;
              goto basadr_in_use;
            }
          }
          pciconfn -> basadr[i].size = basadrsiz;					// not in use, this is where we go
          pciconfn -> basadr[i].addr = basadrend | (basadrsav & BASADRJUNK);
          oz_dev_pci_conf_outl (pciconfn -> basadr[i].addr, pciconf, basadrofs);
          goto basadr_assigned;
basadr_in_use:;
        }
      }
    }
    oz_knl_printk ("oz_dev_pci_find: - no place for registers\n");
free_pciconfn:
    pcibusses[busidx].pciconfs = pciconfn -> next;
    OZ_KNL_NPPFREE (pciconfn);
    return;
basadr_assigned:;
  }

  /* Call driver-specific initialization routine               */
  /* If driver doesn't accept it, maybe some other driver will */

calldriverinit:
  pciconf -> accepted = (*entry) (param, didvid, pciconfn -> pcifunc, pciconfn, pciconfn -> addrsuffix, pciconfn -> addrdescrip);
}

/************************************************************************/
/*									*/
/*  Allocate mapping for DMA						*/
/*									*/
/*    Input:								*/
/*									*/
/*	pciconf = device the mapping is for				*/
/*	npages  = number of pages needed				*/
/*									*/
/*	smplevel = softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_dev_pci_dma~~map_alloc = NULL : no resources			*/
/*	                            else : mapping context block	*/
/*									*/
/************************************************************************/

OZ_Dev_Pci_Dma32map *oz_dev_pci_dma32map_alloc (OZ_Dev_Pci_Conf *pciconf, uLong npages, uLong flags)

{
  OZ_Dev_Pci_Dma32map *dma32map;
  OZ_Mempage ppn;
  uLong ppo, size;

  if (flags & ~0) {
    oz_knl_printk ("oz_dev_pci_dma32map_alloc: no support for flags %X\n", flags & ~0);
    return (NULL);
  }

  dma32map = OZ_KNL_PCMALLOQ ((npages * sizeof dma32map -> ieeedma32[0]) + sizeof *dma32map);
  if (dma32map != NULL) {
    dma32map -> objtype = OZ_OBJTYPE_PCIDMA32MAP;
    dma32map -> npages  = npages;
    size = oz_knl_misc_sva2pa (dma32map -> ieeedma32, &ppn, &ppo);
    if (size < npages * sizeof dma32map -> ieeedma32[0]) oz_crash ("oz_dev_pci_dma32map_alloc: bad ieeedma32 buffer");
    dma32map -> mappciaddr = (ppn << OZ_HW_L2PAGESIZE) + ppo + directoffs;
    dma32map -> busy       = 0;
  }
  return (dma32map);
}

/************************************************************************/
/*									*/
/*  Start a DMA transaction						*/
/*									*/
/*    Input:								*/
/*									*/
/*	dma~~map = as returned by oz_dev_pci_dma~~map_alloc		*/
/*	memtodev = 0 : device-to-memory transfer			*/
/*	           1 : memory-to-device transfer			*/
/*	size = total number of bytes being transferred			*/
/*	phypages = physical page number array				*/
/*	offset = offset in first physical page				*/
/*									*/
/*	smplevel = any							*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_dev_pci_dma32map_start < 0 : couldn't map transfer		*/
/*	                         else : number of elements		*/
/*	*mapvirtadr = virtual address of dma mapping array		*/
/*	*mappciaddr = pci address of dma mapping array			*/
/*									*/
/************************************************************************/

int oz_dev_pci_dma32map_start (OZ_Dev_Pci_Dma32map *dma32map, int memtodev, uLong size, const OZ_Mempage *phypages, uLong offset, OZ_Ieeedma32 **mapvirtadr, uLong *mappciaddr)

{
  int i;
  uLong bytecnt, endaddr, phyaddr;

  OZ_KNL_CHKOBJTYPE (dma32map, OZ_OBJTYPE_PCIDMA32MAP);

  if (oz_hw_atomic_set_long (&(dma32map -> busy), 1) != 0) oz_crash ("oz_dev_pci_dma32map_start: already busy");

  i = 0;							// index in ieeedma32 array
  endaddr = 0xFFFFFFFF;
  while (size > 0) {						// repeat while transfer to process
    phypages += offset >> OZ_HW_L2PAGESIZE;			// normalize offset to a page
    offset   &= PAGESIZE - 1;
    phyaddr   = *phypages;					// get phys addr of transfer = pci address under direct mapping
    if ((phyaddr  < oz_hwaxp_botphypage) || (phyaddr >= oz_hwaxp_topphypage)) { // make sure it is in usable physical memory
      oz_crash ("oz_dev_pci_dma32map_start: bad phypage %LX", phyaddr);
    }
    phyaddr <<= OZ_HW_L2PAGESIZE;
    phyaddr  += offset;
    bytecnt   = PAGESIZE - offset;				// get bytecount to end of page
    if (bytecnt > size) bytecnt = size;				// ... but stop at end of transfer
    if (phyaddr + bytecnt > topphyaddr) oz_crash ("oz_dev_pci_dma32map_start: %LX @ %LX goes off end of mem", bytecnt, phyaddr);
    if (phyaddr == endaddr) {					// see if it starts where last entry left off
      dma32map -> ieeedma32[i-1].bytecnt += bytecnt;		// if so, merge with last entry
    } else {
      if (i == dma32map -> npages) {
        OZ_HW_MB;
        dma32map -> busy = 0;
        oz_knl_printk ("oz_dev_pci_dma32map_start: too many pages\n");
        return (-1);
      }
      dma32map -> ieeedma32[i].phyaddr = phyaddr + directoffs;	// if not, make a new entry
      dma32map -> ieeedma32[i].bytecnt = bytecnt;
      i ++;
    }
    endaddr = phyaddr + bytecnt;				// remember where last left off
    size   -= bytecnt;						// this much less to do
    offset += bytecnt;						// ... starting at this offset in page
  }
  OZ_HW_MB;
  if (mapvirtadr != NULL) *mapvirtadr = dma32map -> ieeedma32;	// return virtual addr of the ieeedma32 array
  if (mappciaddr != NULL) *mappciaddr = dma32map -> mappciaddr;	// return pci address of the ieeedma32 array
  return (i);							// return number of elements filled
}

/************************************************************************/
/*									*/
/*  Stop a DMA transaction						*/
/*									*/
/*    Input:								*/
/*									*/
/*	dma~~map = as returned by oz_dev_pci_dma~~map_alloc		*/
/*									*/
/*    Output:								*/
/*									*/
/*	mapping set up by oz_dev_pci_dma32map_start invalidated		*/
/*									*/
/************************************************************************/

void oz_dev_pci_dma32map_stop (OZ_Dev_Pci_Dma32map *dma32map)

{
  OZ_KNL_CHKOBJTYPE (dma32map, OZ_OBJTYPE_PCIDMA32MAP);
  if (!(dma32map -> busy)) oz_crash ("oz_dev_pci_dma32map_stop: not busy");
  dma32map -> busy = 0;
}

/************************************************************************/
/*									*/
/*  Close out DMA mapping						*/
/*									*/
/*    Input:								*/
/*									*/
/*	dma~~map = as returned by *oz_dev_pci_dma~~map_alloc		*/
/*									*/
/*	smplevel = softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	dma~~map = no longer valid					*/
/*									*/
/************************************************************************/

void oz_dev_pci_dma32map_free (OZ_Dev_Pci_Dma32map *dma32map)

{
  OZ_KNL_CHKOBJTYPE (dma32map, OZ_OBJTYPE_PCIDMA32MAP);
  if (dma32map -> busy) oz_crash ("oz_dev_pci_dma32map_free: busy");
  OZ_KNL_NPPFREE (dma32map);
}

/************************************************************************/
/*									*/
/*  Access PCI Config space						*/
/*									*/
/************************************************************************/

static inline uLong pci_conf_in (OZ_Dev_Pci_Conf *pciconf, uByte confadd, int pci_size)

{
  uLong addr, data, ipl;

  OZ_KNL_CHKOBJTYPE (pciconf, OZ_OBJTYPE_PCICONF);

  addr = (pcibusses[pciconf->busidx].pcibus << 21) 	// <28:21> = bus
       + (pciconf -> pcidev  << 16) 			// <20:16> = device
       + (pciconf -> pcifunc << 13) 			// <15:13> = function
       + (confadd << 5) 				// <12:05> = register
       + pci_size;					// <04:03> = size

  ipl = OZ_HWAXP_MTPR_IPL (31);		// inhib interrupts so it can't interfere with hae_cfg_csr
  hae_cfg_csr = pcibusses[pciconf->busidx].haecfg;
  OZ_HW_MB;				// make sure hae_cfg_csr gets updated before reading data
  hae_cfg_csr;				// re-read it to make sure it gets written
  OZ_HW_MB;
  data = *(uLong *)(pyxis_pci -> cfg_space + addr);
  OZ_HW_MB;				// make sure data gets read before releasing hae_cfg_csr
  OZ_HW_MB;				// Linux code says use two MB's
  OZ_HWAXP_MTPR_IPL (ipl);		// restore interrupt delivery

  return (data);
}

uByte oz_dev_pci_conf_inb (OZ_Dev_Pci_Conf *pciconf, uByte confadd)

{
  return (pci_conf_in (pciconf, confadd, PCI_SIZE_BYTE) >> ((confadd & 3) << 3));
}

uWord oz_dev_pci_conf_inw (OZ_Dev_Pci_Conf *pciconf, uByte confadd)

{
  return (pci_conf_in (pciconf, confadd, PCI_SIZE_WORD) >> ((confadd & 2) << 3));
}

uLong oz_dev_pci_conf_inl (OZ_Dev_Pci_Conf *pciconf, uByte confadd)

{
  return (pci_conf_in (pciconf, confadd, PCI_SIZE_LONG));
}

static inline void pci_conf_out (uLong data, OZ_Dev_Pci_Conf *pciconf, uByte confadd, int pci_size)

{
  int i;
  uLong addr, ipl;

  OZ_KNL_CHKOBJTYPE (pciconf, OZ_OBJTYPE_PCICONF);

  addr = (pcibusses[pciconf->busidx].pcibus << 21) 	// <28:21> = bus
       + (pciconf -> pcidev  << 16) 			// <20:16> = device
       + (pciconf -> pcifunc << 13) 			// <15:13> = function
       + (confadd << 5) 				// <12:05> = register
       + pci_size;					// <04:03> = size

  ipl = OZ_HWAXP_MTPR_IPL (31);			// inhib interrupts so it can't interfere with hae_cfg_csr
  hae_cfg_csr = pcibusses[pciconf->busidx].haecfg;
  OZ_HW_MB;					// make sure hae_cfg_csr gets updated before writing data
  hae_cfg_csr;					// re-read it to make sure it gets written
  OZ_HW_MB;
  *(uLong *)(pyxis_pci -> cfg_space + addr) = data;
  OZ_HW_MB;					// make sure data gets written before restoring hae_cfg_csr
  OZ_HW_MB;					// Linux code says use two MB's
  *(uLong *)(pyxis_pci -> cfg_space + addr);	// re-read to make sure it gets written
  OZ_HW_MB;
  OZ_HWAXP_MTPR_IPL (ipl);
}

void oz_dev_pci_conf_outb (uByte data, OZ_Dev_Pci_Conf *pciconf, uByte confadd)

{
  pci_conf_out (((uLong)data) << ((confadd & 3) << 3), pciconf, confadd, PCI_SIZE_BYTE);
}

void oz_dev_pci_conf_outw (uWord data, OZ_Dev_Pci_Conf *pciconf, uByte confadd)

{
  pci_conf_out (((uLong)data) << ((confadd & 2) << 3), pciconf, confadd, PCI_SIZE_WORD);
}

void oz_dev_pci_conf_outl (uLong data, OZ_Dev_Pci_Conf *pciconf, uByte confadd)

{
  pci_conf_out (data, pciconf, confadd, PCI_SIZE_LONG);
}

/************************************************************************/
/*									*/
/*  Access PCI I/O space						*/
/*									*/
/************************************************************************/

static inline uLong pci_in (uLong ioaddr, int pci_size)

{
  uLong data;

  if (ioaddr < 0x02000000) {
    OZ_HW_MB;
    data = *(uLong *)(pyxis_pci -> pci_sparse_io_A + (ioaddr << 5) + pci_size);
    OZ_HW_MB;
  } else {
    uLong ipl;

    ipl = OZ_HWAXP_MTPR_IPL (31);
    hae_io_csr = ioaddr;
    OZ_HW_MB;
    hae_io_csr;
    OZ_HW_MB;
    data = *(uLong *)(pyxis_pci -> pci_sparse_io_B + ((ioaddr & 0x01FFFFFF) << 5) + pci_size);
    OZ_HW_MB;
    OZ_HW_MB;
    OZ_HWAXP_MTPR_IPL (ipl);
  }
  return (data);
}

uByte oz_dev_pci_inb (uLong ioaddr)

{
  return (pci_in (ioaddr, PCI_SIZE_BYTE) >> ((ioaddr & 3) << 3));
}

uWord oz_dev_pci_inw (uLong ioaddr)

{
  return (pci_in (ioaddr, PCI_SIZE_WORD) >> ((ioaddr & 2) << 3));
}

uLong oz_dev_pci_inl (uLong ioaddr)

{
  return (pci_in (ioaddr, PCI_SIZE_LONG));
}

static inline void pci_out (uLong data, uLong ioaddr, int pci_size)

{
  if (ioaddr < 0x02000000) {
    OZ_HW_MB;
    *(uLong *)(pyxis_pci -> pci_sparse_io_A + (ioaddr << 5) + pci_size) = data;
    OZ_HW_MB;
  } else {
    uLong ipl;

    ipl = OZ_HWAXP_MTPR_IPL (31);
    hae_io_csr = ioaddr;
    OZ_HW_MB;
    hae_io_csr;
    OZ_HW_MB;
    *(uLong *)(pyxis_pci -> pci_sparse_io_B + ((ioaddr & 0x01FFFFFF) << 5) + pci_size) = data;
    OZ_HW_MB;
    OZ_HW_MB;
    OZ_HWAXP_MTPR_IPL (ipl);
  }
}

void oz_dev_pci_outb (uByte data, uLong ioaddr)

{
  pci_out (((uLong)data) << ((ioaddr & 3) << 3), ioaddr, PCI_SIZE_BYTE);
}

void oz_dev_pci_outw (uWord data, uLong ioaddr)

{
  pci_out (((uLong)data) << ((ioaddr & 2) << 3), ioaddr, PCI_SIZE_WORD);
}

void oz_dev_pci_outl (uLong data, uLong ioaddr)

{
  pci_out (data, ioaddr, PCI_SIZE_LONG);
}

/************************************************************************/
/*									*/
/*  Access PCI Memory space						*/
/*									*/
/************************************************************************/

static inline uLong pci_rd (uLong memaddr, int pci_size)

{
  uLong data;

  /* It must have the top address bits set to match what's in the hae_mem_csr */

  if (memaddr < (uLong)(-1 << PCI_SPARSE_MEM_3_HAEBITS)) oz_crash ("oz_dev_pyxis pci_rd: invalid memaddr %LX", memaddr);

  /* Read the memory location from the PCI card */

  OZ_HW_MB;
  data = *(uLong *)(pyxis_pci -> pci_sparse_mem_3 + ((memaddr & ((1 << PCI_SPARSE_MEM_3_HAEBITS) - 1)) << 5) + pci_size);
  OZ_HW_MB;

  return (data);
}

uByte oz_dev_pci_rdb (uLong memaddr)

{
  return (pci_rd (memaddr, PCI_SIZE_BYTE) >> ((memaddr & 3) << 3));
}

uWord oz_dev_pci_rdw (uLong memaddr)

{
  return (pci_rd (memaddr, PCI_SIZE_WORD) >> ((memaddr & 2) << 3));
}

uLong oz_dev_pci_rdl (uLong memaddr)

{
  return (pci_rd (memaddr, PCI_SIZE_LONG));
}

static inline void pci_wt (uLong data, uLong memaddr, int pci_size)

{
  /* It must have the top address bits set to match what's in the hae_mem_csr */

  if (memaddr < (uLong)(-1 << PCI_SPARSE_MEM_3_HAEBITS)) oz_crash ("oz_dev_pyxis pci_wt: invalid memaddr %LX", memaddr);

  /* Write to the memory location on the PCI card */

  OZ_HW_MB;
  *(uLong *)(pyxis_pci -> pci_sparse_mem_3 + ((memaddr & ((1 << PCI_SPARSE_MEM_3_HAEBITS) - 1)) << 5) + pci_size) = data;
  OZ_HW_MB;
}

void oz_dev_pci_wtb (uByte data, uLong memaddr)

{
  pci_wt (((uLong)data) << ((memaddr & 3) << 3), memaddr, PCI_SIZE_BYTE);
}

void oz_dev_pci_wtw (uWord data, uLong memaddr)

{
  pci_wt (((uLong)data) << ((memaddr & 2) << 3), memaddr, PCI_SIZE_WORD);
}

void oz_dev_pci_wtl (uLong data, uLong memaddr)

{
  pci_wt (data, memaddr, PCI_SIZE_LONG);
}

/************************************************************************/
/*									*/
/*  PCI Interrupt processing						*/
/*									*/
/************************************************************************/

/************************************************************************/
/*									*/
/*  Allocate an IRQ for a PCI device					*/
/*									*/
/*    Input:								*/
/*									*/
/*	pciconf = points to PCI address struct				*/
/*	intpin  = 0 : get from config space				*/
/*	       else : 1=INTA, 2=INTB, 3=INTC, 4=INTD			*/
/*	entry   = entrypoint of routine to call upon interrupt		*/
/*	param   = parameter to pass to 'entry' routine			*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_dev_pci_irq_alloc = interrupt struct pointer			*/
/*									*/
/************************************************************************/

OZ_Dev_Pci_Irq *oz_dev_pci_irq_alloc (OZ_Dev_Pci_Conf *pciconf, uByte intpin, void (*entry) (void *param, OZ_Mchargs *mchargs), void *param)

{
  int irq;
  OZ_Dev_Pci_Irq *pciirq;
  uQuad int_mask_bit;

  OZ_KNL_CHKOBJTYPE (pciconf, OZ_OBJTYPE_PCICONF);

  /* Determine the IRQ for the device */

  irq = miata_pci_irq (pciconf, intpin);
  if ((irq < PYXIS_IRQ_BASE) || (irq >= PYXIS_IRQ_BASE + PCI_IRQ_MAX)) {
    oz_knl_printk ("oz_dev_pci_irq_alloc: irq %d out of range\n", irq);
    return (NULL);
  }

  /* Fill in a pciirq struct */

  pciirq = OZ_KNL_NPPMALLOC (sizeof *pciirq);
  pciirq -> objtype = OZ_OBJTYPE_PCIIRQ;
  pciirq -> entry   = entry;
  pciirq -> param   = param;
  pciirq -> irq     = irq;
  oz_hw_smplock_init (sizeof pciirq -> smplock, &(pciirq -> smplock), OZ_SMPLOCK_LEVEL_IPLS + PCI_IPL);

  /* Link the pciirq struct up so it will be seen by interrupt routine */

  pciirq -> next = pciirqlists[irq-PYXIS_IRQ_BASE];
  pciirqlists[irq-PYXIS_IRQ_BASE] = pciirq;

  /* Make sure that interrupt is enabled */

  int_mask_bit = 1ULL << (irq - PYXIS_IRQ_BASE + 8);
  if (!(int_mask_copy & int_mask_bit)) {
    int_mask_copy |= int_mask_bit;
    oz_knl_printk ("oz_dev_pci_irq_alloc: enabling pyxis pci irq %d, mask %8.8QX\n", irq, int_mask_copy);
    OZ_HW_MB;
    OZ_HWAXP_STQP (PYXIS_INT_MASK, int_mask_copy);
    OZ_HW_MB;
    OZ_HW_MB;
    OZ_HWAXP_LDQP (PYXIS_INT_MASK);
    OZ_HW_MB;
  }

  /* Return pointer to struct */

  return (pciirq);
}

/************************************************************************/
/*									*/
/*  Get PCI device's smplock						*/
/*									*/
/*    Input:								*/
/*									*/
/*	pciirq = as returned by oz_dev_pci_irq_alloc			*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_dev_pci_irq_smplock = points to corresponding smplock	*/
/*									*/
/************************************************************************/

OZ_Smplock *oz_dev_pci_irq_smplock (OZ_Dev_Pci_Irq *pciirq)

{
  OZ_KNL_CHKOBJTYPE (pciirq, OZ_OBJTYPE_PCIIRQ);
  return (&(pciirq -> smplock));
}

/************************************************************************/
/*									*/
/*  Set new entry/param for a PCI interrupt				*/
/*									*/
/*    Input:								*/
/*									*/
/*	pciirq = as returned by oz_dev_pci_irq_alloc			*/
/*	entry  = entrypoint of routine to call upon interrupt		*/
/*	param  = parameter to pass to 'entry' routine			*/
/*									*/
/************************************************************************/

void oz_dev_pci_irq_reset (OZ_Dev_Pci_Irq *pciirq, void (*entry) (void *param, OZ_Mchargs *mchargs), void *param)

{
  uLong ll;

  OZ_KNL_CHKOBJTYPE (pciirq, OZ_OBJTYPE_PCIIRQ);

  ll = oz_hw_smplock_wait (&(pciirq -> smplock));	// set smplock so isr gets consistent values for entry, param
  pciirq -> entry = entry;				// set new values
  pciirq -> param = param;
  oz_hw_smplock_clr (&(pciirq -> smplock), ll);		// release smplock
}

/************************************************************************/
/*									*/
/*  Free off an PCI interrupt block					*/
/*									*/
/*    Input:								*/
/*									*/
/*	pciirq = as returned by oz_dev_pci_irq_alloc			*/
/*									*/
/*    Output:								*/
/*									*/
/*	pciirq no longer valid						*/
/*									*/
/************************************************************************/

void oz_dev_pci_irq_free (OZ_Dev_Pci_Irq *pciirq)

{
  OZ_Dev_Pci_Irq **lpciirq, *xpciirq;

  OZ_KNL_CHKOBJTYPE (pciirq, OZ_OBJTYPE_PCIIRQ);

  for (lpciirq = pciirqlists + pciirq -> irq - PYXIS_IRQ_BASE; (xpciirq = *lpciirq) != pciirq; lpciirq = &(xpciirq -> next)) {}
  *lpciirq = pciirq -> next;

  OZ_KNL_NPPFREE (pciirq);
}

/************************************************************************/
/*									*/
/*  Interrupt routine							*/
/*									*/
/*    Input:								*/
/*									*/
/*	(uQuad)irqv = irq PYXIS_IRQ_BASE..PYXIS_IRQ_BASE+PCI_IRQ_MAX-1	*/
/*	CPU's IPL   = that of the interrupt, apparently always 21	*/
/*									*/
/************************************************************************/

static void pci_interrupt (void *irqv, OZ_Mchargs *mchargs)

{
  OZ_Dev_Pci_Irq *pciirq;
  uLong ll;
  uQuad irq;

  irq = (uQuad)irqv;
  if ((irq < PYXIS_IRQ_BASE) || (irq >= PYXIS_IRQ_BASE+PCI_IRQ_MAX)) oz_crash ("oz_dev_pyxis pci_interrupt: bad irq %Qu", irq);

  oz_hwaxp_random_int ();

  /* Ack the interrupt so we can get more of them     */
  /* Doesn't seem we need to, firmware already did it */

#if 000
  OZ_HW_MB;
  int_req = pyxis_intctl -> int_req;
  OZ_HW_MB;

  int_bit = 1 << (irq - PYXIS_IRQ_BASE + 8);
  int_mask_copy &= ~int_bit;
  OZ_HW_MB;
  pyxis_intctl -> int_mask = int_mask_copy;
  OZ_HW_MB;
  pyxis_intctl -> int_req = int_bit;
  OZ_HW_MB;
  pyxis_intctl -> int_mask;
  OZ_HW_MB;
#endif
  
  /* Scan the list calling all routines for this irq */

  for (pciirq = pciirqlists[irq-PYXIS_IRQ_BASE]; pciirq != NULL; pciirq = pciirq -> next) { // scan list for this irq
    ll = oz_hwaxp_smplock_wait_atipl (&(pciirq -> smplock));	// set smplock, already at correct ipl
    (*(pciirq -> entry)) (pciirq -> param, mchargs);		// process interrupt
    oz_hwaxp_smplock_clr_atipl (&(pciirq -> smplock), ll);	// release smplock, remaining at ipl
  }

#if 000
  int_mask_copy |= int_bit;
  OZ_HW_MB;
  pyxis_intctl -> int_mask = int_mask_copy;
  OZ_HW_MB;
  pyxis_intctl -> int_mask;
  OZ_HW_MB;
#endif
}

/************************************************************************/
/*									*/
/*  Pyxis chip also does ISA bus I/O space				*/
/*									*/
/************************************************************************/

asm ("\n"
	"	.globl	oz_dev_isa_inb\n"
	"	.globl	oz_dev_isa_inw\n"
	"	.globl	oz_dev_isa_inl\n"
	"	.globl	oz_dev_isa_outb\n"
	"	.globl	oz_dev_isa_outw\n"
	"	.globl	oz_dev_isa_outl\n"
	"	oz_dev_isa_inb  = oz_dev_pci_inb\n"
	"	oz_dev_isa_inw  = oz_dev_pci_inw\n"
	"	oz_dev_isa_inl  = oz_dev_pci_inl\n"
	"	oz_dev_isa_outb = oz_dev_pci_outb\n"
	"	oz_dev_isa_outw = oz_dev_pci_outw\n"
	"	oz_dev_isa_outl = oz_dev_pci_outl\n"
    );

/************************************************************************/
/*									*/
/*  We also handle ISA bus interrupts					*/
/*									*/
/************************************************************************/

OZ_Dev_Isa_Irq *oz_dev_isa_irq_alloc (uLong irq, void (*entry) (void *param, OZ_Mchargs *mchargs), void *param)

{
  OZ_Dev_Isa_Irq *isairq;
  uByte dport;

  if (irq >= ISA_IRQ_MAX) oz_crash ("oz_dev_isa_irq_alloc: bad irq %u", irq);

  isairq = OZ_KNL_NPPMALLOC (sizeof *isairq);
  isairq -> objtype = OZ_OBJTYPE_ISAIRQ;
  isairq -> entry   = entry;
  isairq -> param   = param;
  isairq -> irq     = irq;
  oz_hw_smplock_init (sizeof isairq -> smplock, &(isairq -> smplock), OZ_SMPLOCK_LEVEL_IPLS + ISA_IPL);

  isairq -> next    = isairqlists[irq];
  isairqlists[irq]  = isairq;

  if (irq >= 8) {
    irq -= 8;
    dport = oz_dev_isa_inb (SLAV_8259_DPORT);
    if (dport & (1 << irq)) {
      oz_knl_printk ("oz_dev_isa_irq_alloc: enabling irq %u\n", irq + 8);
      dport &= ~(1 << irq);
      oz_dev_isa_outb (dport, SLAV_8259_DPORT);
    }
    irq = 2;
  }
  dport = oz_dev_isa_inb (MAST_8259_DPORT);
  if (dport & (1 << irq)) {
    oz_knl_printk ("oz_dev_isa_irq_alloc: enabling irq %u\n", irq);
    dport &= ~(1 << irq);
    oz_dev_isa_outb (dport, MAST_8259_DPORT);
  }

  return (isairq);
}

OZ_Smplock *oz_dev_isa_irq_smplock (OZ_Dev_Isa_Irq *isairq)

{
  OZ_KNL_CHKOBJTYPE (isairq, OZ_OBJTYPE_ISAIRQ);
  return (&(isairq -> smplock));
}

void oz_dev_isa_irq_reset (OZ_Dev_Isa_Irq *isairq, void (*entry) (void *param, OZ_Mchargs *mchargs), void *param)

{
  uLong ll;

  OZ_KNL_CHKOBJTYPE (isairq, OZ_OBJTYPE_ISAIRQ);

  ll = oz_hw_smplock_wait (&(isairq -> smplock));	// set smplock so isr gets consistent values for entry, param
  isairq -> entry = entry;				// set new values
  isairq -> param = param;
  oz_hw_smplock_clr (&(isairq -> smplock), ll);		// release smplock
}

void oz_dev_isa_irq_free (OZ_Dev_Isa_Irq *isairq)

{
  OZ_Dev_Isa_Irq **lisairq, *xisairq;

  OZ_KNL_CHKOBJTYPE (isairq, OZ_OBJTYPE_ISAIRQ);

  for (lisairq = isairqlists + isairq -> irq; (xisairq = *lisairq) != isairq; lisairq = &(xisairq -> next)) {}
  *lisairq = isairq -> next;

  OZ_KNL_NPPFREE (isairq);
}

/************************************************************************/
/*									*/
/*  Interrupt routine							*/
/*									*/
/*    Input:								*/
/*									*/
/*	(uQuad)irqv = irq 0..ISA_IRQ_MAX-1				*/
/*	CPU's IPL   = that of the interrupt (always 21)			*/
/*									*/
/************************************************************************/

static void isa_interrupt (void *irqv, OZ_Mchargs *mchargs)

{
  OZ_Dev_Isa_Irq *isairq;
  uLong ll;
  uQuad irq;

  irq = (uQuad)irqv;
  if (irq >= ISA_IRQ_MAX) oz_crash ("oz_dev_pyxis isa_interrupt: bad irq %u", irq);

  oz_hwaxp_random_int ();

  /* Scan the list calling all routines */

  for (isairq = isairqlists[irq]; isairq != NULL; isairq = isairq -> next) { // scan list for this irq
    ll = oz_hwaxp_smplock_wait_atipl (&(isairq -> smplock));	// set smplock, already at correct ipl
    (*(isairq -> entry)) (isairq -> param, mchargs);		// process interrupt
    oz_hwaxp_smplock_clr_atipl (&(isairq -> smplock), ll);	// release smplock, remaining at ipl
  }

  /* Ack the interrupt so we can get more of them */

  if (irq >= 8) {
    oz_dev_isa_outb (0x60 + irq - 8, SLAV_8259_APORT);
    irq = 2;
  }
  oz_dev_isa_outb (0x60 + irq, MAST_8259_APORT);
}

/************************************************************************/
/*									*/
/*  This routine is called by a translation-not-valid fault.  These 	*/
/*  do not happen by normal pagefaults (they get access violations).  	*/
/*									*/
/*	1) we don't ever have to invalidate L3 TB entries as the 	*/
/*	   virtual->physical translation is always the same		*/
/*	2) we know the contents of the L3 pages so we can fill an L3 	*/
/*	   page completely when we map it in				*/
/*	3) we must operate at any IPL, so we work with a small set of 	*/
/*	   physical pages						*/
/*									*/
/************************************************************************/

/************************************************************************/
/*									*/
/*    Suppose:								*/
/*									*/
/*	vaddr 0's oz_hwaxp_l3ptbase is at FFFF.FFFC.0000.0000		*/
/*	vaddr 0's oz_hwaxp_l2ptbase is at FFFF.FFFD.FF00.0000		*/
/*	vaddr 0's oz_hwaxp_l1ptbase is at FFFF.FFFD.FF7F.C000		*/
/*									*/
/*	and pyxis_pci ends up being at FFFF.FFE0.0000.0000		*/
/*									*/
/*    So that gives us:							*/
/*									*/
/*	oz_hwaxp_l3ptbase vpage = 3FE0.0000 (1024*1024 pages)		*/
/*	oz_hwaxp_l2ptbase vpage = 3FEF.F800 (1024 pages)		*/
/*	oz_hwaxp_l1ptbase vpage = 3FEF.FBFE (just 1 page)		*/
/*									*/
/*	pci_data_vpage = 3F00.0000					*/
/*	pci_l3pt_vpage = 3FEF.C000 = 3FE0.0000 + 000F.C000		*/
/*	pci_l2pt_vpage = 3FEF.FBF0 = 3FE0.0000 + 000F.FBF0		*/
/*									*/
/*    A routine wants to access physaddr 81.2345.6789			*/
/*    That ends up being virtaddr FFFF.FFE1.2345.6789			*/
/*    The corresponding data vpage is 3F09.1A2B				*/
/*    The corresponding l3pte va is FFFF.FFFD.F848.D158			*/
/*    The corresponding l3pte vpage is 3FEF.C246			*/
/*    The corresponding l2pte va is FFFF.FFFD.FF7E.1230			*/
/*    The corresponding l2pte vpage is 3FEF.FBF0			*/
/*    The corresponding l1pte va is FFFF.FFFD.FF7F.DF80			*/
/*									*/
/************************************************************************/

#ifdef VIRTUAL_MAP

static void pyxis_mapiopage (void *dummy, OZ_Mchargs *mchargs)

{
  OZ_Mempage vpage;

  oz_knl_printk ("oz_dev_pyxis mapiopage*: ps %QX, p4 %QX\n", mchargs -> ps, mchargs -> p4);

  if ((mchargs -> ps & 0x18) != 0) goto notus;
  vpage = OZ_HW_VADDRTOVPAGE (mchargs -> p4);

  /* See if the fault happened as a direct result of accessing an I/O data page                 */
  /* All we have to do is read the L3 pte, and it will fault in an L3 page full of good entries */
  /* So we just test it for the bits it should have and puke if it's bad                        */

  /* Using the example problem:                          */
  /*    oz_hwaxp_l3ptbase = FFFF.FFFC.0000.0000          */
  /*   datavpage = 3F09.1A2B                             */
  /*   datappage = 0409.1A2B                             */
  /* The l3pte we access is at vaddr FFFF.FFFD.F848.D158 */

  if ((vpage >= pci_data_vpage) && (vpage < pci_data_vpage + NDATAPAGES)) {
    uQuad l3pte, ppage;

    l3pte = oz_hwaxp_l3ptbase[vpage];
    ppage = vpage - pci_data_vpage + pci_data_ppage;
    if (l3pte != (ppage << 32) + 0x1171) oz_crash ("pyxis_pci_iopf: L3 pte[0x%LX]=%QX != %QX", vpage, l3pte, (ppage << 32) + 0x1171);
    return;
  }

  /* See if the fault happened because there isn't an L3 pagetable page there.               */
  /* We allocate a physical page and fill it in with the constant virtual->physical mapping. */
  /* Also set up the L2 pointer to the allocated page.                                       */

  /* Using the example problem:                    */
  /*    oz_hwaxp_l3ptbase = FFFF.FFFC.0000.0000    */
  /*   l3ptvpage = 3FEF.C246                       */
  /* So the l2pte we access is FFFF.FFFD.FF7E.1230 */
  /* The VA of the new L3 page FFFF.FFFD.F848.C000 */

  /* The datavaddr at the beginning of this page is FFFF.FFE1.2300.0000, vpage 3F09.1800 */
  /* The datapaddr at the beginning of this page is        81.2300.0000, ppage 0409.1800 */

  if ((vpage >= pci_l3pt_vpage) && (vpage < pci_l3pt_vpage + NL3PTPAGES)) {
    int i;
    Ioptcb *ioptcb;
    uLong datappage, datavpage;

    ioptcb = get_ioptcb (vpage);					// get a physical page to use
    ioptcb -> mappedat = OZ_HW_VPAGETOVADDR (vpage);			// virtual addr it gets mapped at within the L3 table
    ioptcb -> mappedby = oz_hwaxp_l3ptbase + vpage;			// get the va of the L2 pointer
    *(ioptcb -> mappedby) = (((uQuad)(ioptcb -> phypage)) << 32) + 0x1111; // set up L2 pointer, global, no granularity hint

    datavpage = ioptcb -> mappedat - oz_hwaxp_l3ptbase;			// this is the data vpage at beg of the L3 page
    datappage = datavpage - pci_data_vpage + pci_data_ppage;		// this is the corresponding physical page number
    for (i = PTESPERPAGE; -- i >= 0;) {					// fill with consecutive physical page numbers
      (ioptcb -> mappedat)[i] = (((uQuad)(datappage + i)) << 32) + 0x1171; // set up PTE, global, max granularity
    }
    *ioptcb_l3_qt = ioptcb;						// link it up for possible re-use
    ioptcb -> next = NULL;
    ioptcb_l3_qt = &(ioptcb -> next);
    return;
  }

  /* See if the fault happened because there isn't an L2 pagetable page there */
  /* If so, allocate a page and fill it with zeroes.  Fill in the L1 pointer. */

  /* Using the example problem:                       */
  /* The l2pte's va from above is FFFF.FFFD.FF7E.1230 */
  /* That makes the l2ptvpage  = 3FEF.FBF0            */
  /* So the l1pte we access va = FFFF.FFFD.FF7F.DF80  */
  /* And the l1index = 3F0                            */

#if 000 // we preallocate and initialize all L2 pages at boot time so we don't waste time here
  if ((vpage >= pci_l2pt_vpage) && (vpage < pci_l2pt_vpage + NL2PTPAGES)) {
    int l1index;
    Ioptcb *ioptcb;

    l1index = oz_hwaxp_l3ptbase + vpage - oz_hwaxp_l1ptbase;		// get page's index in L1 table
    if (oz_hwaxp_masterl1ptva[l1index] != 0) {				// see if the master L1 table has it
      oz_hwaxp_l3ptbase[vpage] = oz_hwaxp_masterl1ptva[l1index];	// if so, just copy to this process' table
    } else {
      ioptcb = get_ioptcb (vpage);					// get a physical page to use
      ioptcb -> next = ioptcb;						// never re-use an L2 page
      ioptcb -> mappedat = OZ_HW_VPAGETOVADDR (vpage);
      ioptcb -> mappedby = oz_hwaxp_l3ptbase + vpage;			// get the va of the L1 pointer
      memset (ioptcb -> mappedat, 0, PAGESIZE);				// zero out the L2 page
      oz_hwaxp_masterl1ptva[l1index] = *(ioptcb -> mappedby) = (ioptcb -> phypage << 32) + 0x1111; // no granularity, but mark it global
    }
    return;
  }
#endif

  /* SOL */

notus:
  (*old_tnv_ent) (old_tnv_prm, mchargs);				// call the other handler
}

static Ioptcb *get_ioptcb (OZ_Mempage vpage)

{
  Ioptcb *ioptcb;
  OZ_Mempage ppage;

  ioptcb = ioptcb_free;						// see if there's a free preallocated one
  if (ioptcb != NULL) {
    ioptcb_free = ioptcb -> next;				// if so, unlink it
  } else if (initialized && (oz_hw_cpu_smplevel () < OZ_SMPLOCK_LEVEL_PM)) { // if not, see if we can allocate a general page
    uLong pm;

    pm = oz_hw_smplock_wait (&oz_s_smplock_pm);			// ok, lock physical memory database
    ppage = oz_knl_phymem_allocpage (OZ_PHYMEM_PAGESTATE_ALLOCSECT, vpage); // try to allocate a page
    oz_hw_smplock_clr (&oz_s_smplock_pm, pm);			// release physical memory database
    if (ppage != OZ_PHYPAGE_NULL) {
      ioptcb = OZ_KNL_NPPMALLOC (sizeof *ioptcb);		// got one, alloc the corresponding control block
      ioptcb -> phypage = ppage;
    }
  }
  if (ioptcb == NULL) {
    ioptcb = ioptcb_l3_qh;					// re-use a currently mapped L3 page
    if ((ioptcb_l3_qh = ioptcb -> next) == NULL) ioptcb_l3_qt = &ioptcb_l3_qh;
    *(ioptcb -> mappedby) = 0x1110;				// mark page inactive, but still KW and global
    OZ_HWAXP_MTPR_TBISD (ioptcb -> mappedat);			// ... and invalidate it
  }

  return (ioptcb);
}

#endif

/************************************************************************/
/*									*/
/*  Determine the 'irq' for a given PCI device				*/
/*									*/
/*  The SCB is set up with the PCI interrupts at vectors 0x900..0xAF0	*/
/*  This makes it easy for us to hand out irq numbers 16..47, as the 	*/
/*  ISA stuff is at 0x800..0x8F0 and uses irq numbers 0..15.		*/
/*									*/
/*  This routine was mostly copied from Linux				*/
/*									*/
/*  For int A of Slot 1, we return 28 because that's what interrupt 	*/
/*  vector the device interrupts on.  Similar for all the other bits.	*/
/*  Like for the built-in ethernet, we return 16.			*/
/*									*/
/************************************************************************/

/*
 * PCI Fixup configuration.
 *
 * Summary @ int_mask_register:
 * Bit      Meaning                           IRQ

 * 0        Fan Fault                          -1
 * 1        NMI                                -1
 * 2        Halt/Reset switch                  -1
 * 3        none                               -1

 * 4        CID0 (Riser ID)                    -1
 * 5        CID1 (Riser ID)                    -1
 * 6        Interval timer                     -1
 * 7        PCI-ISA Bridge                     -1

 * 8        Ethernet                           16
 * 9        EIDE (deprecated, ISA 14/15 used)  -1
 *10        none                               -1
 *11        USB                                -1

 *12        Interrupt Line A from slot 4       20
 *13        Interrupt Line B from slot 4       21
 *14        Interrupt Line C from slot 4       22
 *15        Interrupt Line D from slot 4       23
 *16        Interrupt Line A from slot 5       24
 *17        Interrupt line B from slot 5       25
 *18        Interrupt Line C from slot 5       26
 *19        Interrupt Line D from slot 5       27
 *20        Interrupt Line A from slot 1       28
 *21        Interrupt Line B from slot 1       29
 *22        Interrupt Line C from slot 1       30
 *23        Interrupt Line D from slot 1       31
 *24        Interrupt Line A from slot 2       32
 *25        Interrupt Line B from slot 2       33
 *26        Interrupt Line C from slot 2       34
 *27        Interrupt Line D from slot 2       35
 *27        Interrupt Line A from slot 3       36
 *29        Interrupt Line B from slot 3       37
 *30        Interrupt Line C from slot 3       38
 *31        Interrupt Line D from slot 3       39
 *
 * The device to slot mapping looks like:
 * (from console 'SHOW CONFIG' command)
 *
 * Directly on Pyxis' bus:  HAECFG=0; PCIBUS=0
 *
 * PCIDEV   Description                      IRQ
 *  3       DC21142 Ethernet                 16
 *  4       EIDE CMD646                      uses ISA ints 14&15
 *  7       PCI-ISA bridge                   none
 * 11       PCI on board slot 4 (SBU Riser)  20..23
 * 12       PCI on board slot 5 (SBU Riser)  24..27
 * 20       DC21152 PCI-PCI Bridge           none
 *
 * Behind the bridge:  HAECFG=1; PCIBUS=1
 *
 *  8       PCI on board slot 1 (SBU Riser)  28..31
 *  9       PCI on board slot 2 (SBU Riser)  32..35
 * 10       PCI on board slot 3 (SBU Riser)  36..40
 */

static int const irqtable[] = { PYXIS_IRQ_BASE+11,PYXIS_IRQ_BASE+15,PYXIS_IRQ_BASE+19,PYXIS_IRQ_BASE+3,PYXIS_IRQ_BASE+7 };

static int miata_pci_irq (OZ_Dev_Pci_Conf *pciconf, uByte intpin)

{
  uLong pcibus;

  pcibus = pcibusses[pciconf->busidx].pcibus;

  /* We only process bus 0 or 1 */

  if (pcibus >= 2) {
    oz_knl_printk ("oz_dev_pyxis miata_pci_irq: we don't do bus %u\n", pcibus);
    return (-1);
  }

  if (intpin > 4) {
    oz_knl_printk ("oz_dev_pyxis miata_pci_irq: we don't do intpin %u\n", intpin);
    return (-1);
  }

  /* Built-in ethernet is wired to bit 8 of Pyxis interrupt registers */

  if ((pcibus == 0) && (pciconf -> pcidev == 3)) return (PYXIS_IRQ_BASE + 0);

  /* See if it's one of the slots (they use the intpin) */

  if ((pcibus == 0) && (pciconf -> pcidev >= 11) && (pciconf -> pcidev <= 12)) goto slotok;
  if ((pcibus == 1) && (pciconf -> pcidev >=  8) && (pciconf -> pcidev <= 10)) goto slotok;
  oz_knl_printk ("oz_dev_pyxis miata_pci_irq: we don't do %u/%u\n", pcibus, pciconf -> pcidev);
  return (-1);
slotok:

  /* If not supplied, get intpin from the config space data for the device (1=INTA, 2=INTB, 3=INTC, 4=INTD) */

  if (intpin == 0) {
    intpin = oz_dev_pci_conf_inb (pciconf, OZ_DEV_PCI_CONF_B_INTPIN);
    if ((intpin < 1) || (intpin > 4)) {
      oz_knl_printk ("oz_dev_pyxis miata_pci_irq: %s has invalid intpin %u\n", pciconf -> addrdescrip, intpin);
      return (-1);
    }
  }

  /* Calculate IRQ based on dev and intpin               */
  /* We don't need bus because dev numbers don't overlap */

  /* pcibus pcidev slot irq=16+    */
  /*    1      8     1  20-8..23-8 */
  /*    1      9     2  24-8..27-8 */
  /*    0     10     3  28-8..31-8 */
  /*    0     11     4  12-8..15-8 */
  /*    0     12     5  16-8..19-8 */

  return (irqtable[pciconf->pcidev-8] + intpin);
}

#if 000
        static char irq_tab[18][5] __initdata = {
		/*INT    INTA   INTB   INTC   INTD */
		{16+ 8, 16+ 8, 16+ 8, 16+ 8, 16+ 8},  /* Dev 3, IdSel 14,  DC21142 */
		{   -1,    -1,    -1,    -1,    -1},  /* Dev 4, IdSel 15,  EIDE    */
		{   -1,    -1,    -1,    -1,    -1},  /* Dev 5, IdSel 16,  none    */
		{   -1,    -1,    -1,    -1,    -1},  /* Dev 6, IdSel 17,  none    */
		{   -1,    -1,    -1,    -1,    -1},  /* Dev 7, IdSel 18,  PCI-ISA */
		{   -1,    -1,    -1,    -1,    -1},  /* Dev 8, IdSel 19,  PCI-PCI */
		{   -1,    -1,    -1,    -1,    -1},  /* Dev 9, IdSel 20,  none    */
		{   -1,    -1,    -1,    -1,    -1},  /* Dev 10, IdSel 21,  none    */
		{16+12, 16+12, 16+13, 16+14, 16+15},  /* Dev 11, IdSel 22,  slot 4  */
		{16+16, 16+16, 16+17, 16+18, 16+19},  /* Dev 12, IdSel 23,  slot 5  */
		/* the next 7 are actually on PCI bus 1, across the bridge */
		{16+11, 16+11, 16+11, 16+11, 16+11},  /* IdSel 24,  QLISP/GL*/
		{   -1,    -1,    -1,    -1,    -1},  /* IdSel 25,  none    */
		{   -1,    -1,    -1,    -1,    -1},  /* IdSel 26,  none    */
		{   -1,    -1,    -1,    -1,    -1},  /* IdSel 27,  none    */
		{16+20, 16+20, 16+21, 16+22, 16+23},  /* IdSel 28,  slot 1  */
		{16+24, 16+24, 16+25, 16+26, 16+27},  /* IdSel 29,  slot 2  */
		{16+28, 16+28, 16+29, 16+30, 16+31},  /* IdSel 30,  slot 3  */
		/* This bridge is on the main bus of the later orig MIATA */
		{   -1,    -1,    -1,    -1,    -1},  /* IdSel 31,  PCI-PCI */
        };

	const long min_idsel = 3, max_idsel = 20, irqs_per_slot = 5;

	long _ctl_ = -1;
#endif
