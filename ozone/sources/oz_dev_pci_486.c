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
/*  PCI library routines for 486					*/
/*									*/
/************************************************************************/

#define _OZ_DEV_PCI_C

#include "ozone.h"

#include "oz_dev_pci.h"
#include "oz_knl_hw.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_objtype.h"
#include "oz_knl_printk.h"

#define PAGESIZE (1<<OZ_HW_L2PAGESIZE)

#define MAX_PCI_SLOTS 256

#define PCI_FUNCTION_ID 0xB1
#define PCI_BIOS_PRESENT 0x01
#define FIND_PCI_DEVICE 0x02
#define FIND_PCI_CLASS_CODE 0x03
#define GENERATE_SPECIAL_CYCLE 0x06
#define READ_CONFIG_BYTE 0x08
#define READ_CONFIG_WORD 0x09
#define READ_CONFIG_LONG 0x0A
#define WRITE_CONFIG_BYTE 0x0B
#define WRITE_CONFIG_WORD 0x0C
#define WRITE_CONFIG_LONG 0x0D
#define GET_IRQ_ROUTING_OPTIONS 0x0E
#define SET_PCI_IRQ 0x0F

#define SUCCESSFUL 0x00
#define FUNC_NOT_SUPPORTED 0x81
#define BAD_VENDOR_ID 0x83
#define DEVICE_NOT_FOUND 0x86
#define BAD_REGISTER_NUMBER 0x87
#define SET_FAILED 0x88
#define BUFFER_TOO_SMALL 0x89

typedef struct { uByte pci_bus_number;
                 uByte pci_dev_number;
                 uByte link_int_a;
                 uByte irq_bitmap_lo_a;
                 uByte irq_bitmap_hi_a;
                 uByte link_int_b;
                 uByte irq_bitmap_lo_b;
                 uByte irq_bitmap_hi_b;
                 uByte link_int_c;
                 uByte irq_bitmap_lo_c;
                 uByte irq_bitmap_hi_c;
                 uByte link_int_d;
                 uByte irq_bitmap_lo_d;
                 uByte irq_bitmap_hi_d;
                 uByte slot_number;
                 uByte unused;
               } Routentry;

typedef struct { uLong signature;
                 void *bsdentry;
                 uByte version;
                 uByte length;
                 uByte checksum;
                 uByte pad1[5];
               } Sdep;

/* PCI config addressing struct */

#define CONFADDR(bus,dev,func) (((bus) << 8) | ((dev) << 3) | (func))

#define CONFBUS(addr) (((addr) >> 8) & 0xFF)
#define CONFDEV(addr) (((addr) >> 3) & 0x1F)
#define CONFFUNC(addr) ((addr) & 7)

struct OZ_Dev_Pci_Conf { OZ_Objtype objtype;		// OZ_OBJTYPE_PCICONF
                         OZ_Dev_Pci_Conf *next;		// next in pciconfs list
                         uLong confaddr;		// bus,dev,func in same format as PCIBIOS:
							//   <15:08> = bus
							//   <07:03> = device
							//   <02:00> = function
                         int accepted;			// set if accepted by a driver
                         char addrsuffix[16];		// address suffix string, _<bus>_<dev>_<func>
                         char addrdescrip[32];		// address description, bus/dev/func <bus>/<dev>/<func>
                       };

/* PCI Interrupt request struct */

struct OZ_Dev_Pci_Irq { OZ_Objtype objtype;		// OZ_OBJTYPE_PCIIRQ
                        void (*entry) (void *param, OZ_Mchargs *mchargs);
                        void *param;
                        OZ_Smplock *smplock;
                        OZ_Hw486_irq_many irq_many;
                        uByte intline;
                      };

/* DMA mapping context struct */

struct OZ_Dev_Pci_Dma32map {
  OZ_Objtype objtype;		// OZ_OBJTYPE_PCIDMA32MAP
  uLong npages;			// number of pages we can map
				// mapped: number of map registers starting at basepciaddr
				// unmapped: number of elements in 'ieeedma32' array
  uLong basepciaddr;		// mapped: start of allocated map registers
				// unmapped: not used
  uLong mappciaddr;		// pci address of the ieeedma32 array
  uLong flags;			// flags from oz_dev_pci_dma32map_alloc call
  Long volatile busy;		// 0: available; 1: busy
  OZ_Ieeedma32 ieeedma32[1];	// ieeedma32 array to give to the controller
};

static Long volatile initinprog = 0;
static OZ_Dev_Pci_Conf *pciconfs = NULL;	// list of configured devices
static OZ_Smplock smplock_dv;			// used to lock 'pciconfs' list
static Routentry route_table[MAX_PCI_SLOTS];
static uLong volatile pcibios_entry = 0;	// 0: uninitted; 1: no PCI BIOS present; else: PCI BIOS entrypoint
static uLong pcislots = 0;

static int initroutetable (void);
static void initialize (void);
static int pci_interrupt (void *pciirqv, OZ_Mchargs *mchargs);

/************************************************************************/
/*									*/
/*  Determine whether or not PCI bus is present				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_dev_pci_present = 0 : no PCI bus				*/
/*	                     1 : has PCI bus				*/
/*									*/
/************************************************************************/

int oz_dev_pci_present (void)

{
  if (pcibios_entry == 0) initialize ();	// if not initialized, initialize
  return (pcibios_entry != 1);			// if init failed, return 'not present'
						// initialized ok, return 'success'
}

/************************************************************************/
/*									*/
/*  Get PCI interrupt redirection assignments made by the BIOS.		*/
/*									*/
/*  This routine gets called by oz_hw_smproc_486's oz_hw486_irq_init 	*/
/*  routine when it is ready to program the IOAPIC.			*/
/*									*/
/*    Output:								*/
/*									*/
/*	%eax<00:07> = irq for PCI-A interrupt				*/
/*	    <08:15> = irq for PCI-B interrupt				*/
/*	    <16:23> = irq for PCI-C interrupt				*/
/*	    <24:31> = irq for PCI-D interrupt				*/
/*									*/
/*    Note:								*/
/*									*/
/*	Returned byte will be 0x80 if the PCI interrupt is not 		*/
/*	assigned to any IRQ						*/
/*									*/
/************************************************************************/

uLong oz_hw486_getpciirqs (void)

{
  uLong irqnumber, pciintletter;
  uLong i, pciirqs;
  OZ_Dev_Pci_Conf pciconf;

  if ((pcislots == 0) && !initroutetable ()) return (0x80808080);

  pciirqs = 0x80808080;

  memset (&pciconf, 0, sizeof pciconf);
  pciconf.objtype = OZ_OBJTYPE_PCICONF;

  for (i = 0; i < pcislots; i ++) {

    /* See what interrupt mapping the BIOS set up for the device in this slot */
    /* If no device in the slot, we'll either get all 0's or all 1's          */
    /* Also, for devs that don't use interrupts, we get 0's                   */

    pciconf.confaddr = CONFADDR (route_table[i].pci_bus_number, 
                                 route_table[i].pci_dev_number >> 3, 
                                 route_table[i].pci_dev_number & 7);

    irqnumber    = oz_dev_pci_conf_inb (&pciconf, OZ_DEV_PCI_CONF_B_INTLINE);	// irq BIOS set the device to
    pciintletter = oz_dev_pci_conf_inb (&pciconf, OZ_DEV_PCI_CONF_B_INTPIN);	// controller interrupt pin
										// - 1=INTA, 2=INTB, 3=INTC, 4=INTD

    if ((irqnumber > 0) && (irqnumber < 16)) {

      /* Translate interrupt pin coming out of controller to corresponding interrupt controller pin */

      /* ?? The PCI BIOS spec says the route_table[i].link_int_x entries can have anything so long as the    */
      /* ?? numbers match with other slots so as to indicate which lines are connected together.  But as far */
      /* ?? as interpreting the number itself, that is undefined.  But it seems to work to interpret them    */
      /* ?? in the 'sane' way, ie, 1=INTA, 2=INTB, 3=INTC, 4=INTD (at least on the Asus P2B-D and MSI K7D).  */

      switch (pciintletter) {
        case 1:  pciintletter = route_table[i].link_int_a; break;
        case 2:  pciintletter = route_table[i].link_int_b; break;
        case 3:  pciintletter = route_table[i].link_int_c; break;
        case 4:  pciintletter = route_table[i].link_int_d; break;
        default: pciintletter = 0;
      }

      /* Store the irqnumber in the corresponding byte of pciirqs */

      switch (pciintletter) {
        case 1: pciirqs = (pciirqs & 0xFFFFFF00) |  irqnumber;        break;
        case 2: pciirqs = (pciirqs & 0xFFFF00FF) | (irqnumber <<  8); break;
        case 3: pciirqs = (pciirqs & 0xFF00FFFF) | (irqnumber << 16); break;
        case 4: pciirqs = (pciirqs & 0x00FFFFFF) | (irqnumber << 24); break;
      }
    }
  }

  oz_knl_printk ("oz_hw486_getpciirqs: mapping %8.8X\n", pciirqs);

  return (pciirqs);
}

/************************************************************************/
/*									*/
/*  Find all devices with a given didvid				*/
/*									*/
/*    Input:								*/
/*									*/
/*	didvid = didvid to search for					*/
/*	func < 0 : can be any func					*/
/*	    else : must be this func					*/
/*	entry = entrypoint to call back when one found			*/
/*	param = parameter to pass to 'entry' routine			*/
/*	smplevel = softint						*/
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
  int i, index;
  OZ_Dev_Pci_Conf pciconf, *pciconfn, *pciconfx;
  uLong basadr, confaddr, dv, return_code;

  if (pcibios_entry == 0) initialize ();	// if not initialized, initialize
  if (pcibios_entry == 1) return;		// if init failed, return without doing anything

  memset (&pciconf, 0, sizeof pciconf);
  pciconf.objtype = OZ_OBJTYPE_PCICONF;

  for (index = 0;; index ++) {

    /* Find a device from the PCI BIOS */

    asm ("pushl %%cs\n"
         "call *%%ebx\n"
         : "=a" (return_code), 			// where to put return code
           "=b" (confaddr)			// where to put composite pci address
         : "a" ((PCI_FUNCTION_ID << 8) | FIND_PCI_DEVICE), 
           "b" (pcibios_entry), 		// entrypoint to pci bios routine
           "c" (didvid >> 16), 			// device-id word
           "d" (didvid & 0xFFFF), 		// vendor-id word
           "S" (index));			// index number

    if ((return_code & 0xFF00) == (DEVICE_NOT_FOUND << 8)) break;
    if ((return_code & 0xFF00) != (SUCCESSFUL << 8)) oz_crash ("oz_dev_pci_find_didvid: return code %X", return_code);

    if ((func < 0) || (CONFFUNC (confaddr) == func)) {

      /* Allocate and fill in a new pciconf struct */

      pciconfn = OZ_KNL_NPPMALLOC (sizeof *pciconfn);
      memset (pciconfn, 0, sizeof *pciconfn);
      pciconfn -> objtype  = OZ_OBJTYPE_PCICONF;
      pciconfn -> confaddr = confaddr;
      pciconfn -> accepted = 1;

      /* Make sure we haven't done this same device before (like for a different driver) */
      /* If we haven't, link it up so we won't do it again                               */

      dv = oz_hw_smplock_wait (&smplock_dv);
      for (pciconfx = pciconfs; pciconfx != NULL; pciconfx = pciconfx -> next) {
        if (pciconfx -> confaddr == pciconfn -> confaddr) break;
      }
      if (pciconfx == NULL) {
        pciconfn -> next = pciconfs;
        pciconfs = pciconfn;
      }
      oz_hw_smplock_clr (&smplock_dv, dv);

      /* If done before, free it off */

      if (pciconfx != NULL) {
        OZ_KNL_NPPFREE (pciconfn);
        if (pciconfx -> accepted) continue;
        pciconfn = pciconfx;
      }

      /* Otherwise, set up its addressing registers */

      else {

        /* Make up address suffix and description strings */

        if (func >= 0) oz_sys_sprintf (sizeof pciconfn -> addrsuffix, pciconfn -> addrsuffix, "%u_%u", 
		CONFBUS (confaddr), CONFDEV (confaddr));
        else oz_sys_sprintf (sizeof pciconfn -> addrsuffix, pciconfn -> addrsuffix, "%u_%u_%u", 
		CONFBUS (confaddr), CONFDEV (confaddr), CONFFUNC (confaddr));

        oz_sys_sprintf (sizeof pciconfn -> addrdescrip, pciconfn -> addrdescrip, "bus/dev/func %u/%u/%u", 
		CONFBUS (confaddr), CONFDEV (confaddr), CONFFUNC (confaddr));

        /* These CPU's can only handle 16-bit I/O addresses */

        for (i = 0; i < 6; i ++) {
          if (flags & (1 << i)) {
            basadr = oz_dev_pci_conf_inl (pciconfn, OZ_DEV_PCI_CONF_L_BASADR0 + i * 4);
            if ((basadr & 1) && (basadr > 0xFFFF)) {
              oz_knl_printk ("oz_dev_pci_find_didvid: %s basadr[%d] %8.8X out of range\n", 
			pciconfn -> addrdescrip, didvid, i, basadr);
              break;
            }
          }
        }
        if (i < 6) continue;
      }

      /* Call the driver */

      pciconfn -> accepted = (*entry) (param, didvid, CONFFUNC (confaddr), pciconfn, pciconfn -> addrsuffix, pciconfn -> addrdescrip);
    }
  }
}

/*****************************/
/* Scan configuration tables */
/*****************************/

#if 000

int oz_dev_pci_conf_scan_classcode (OZ_Dev_Pci_Conf *pciconf, int first, uLong classcode)

{
  uLong pci_address, return_code;

  oz_knl_printk ("oz_dev_pci_conf_scan: scanning for pci classcode %6.6X\n", classcode);

  if (pcibios_entry == 0) initialize ();	// if not initialized, initialize
  if (pcibios_entry == 1) return (0);		// if init failed, return 'no such device'

  if (first) pciconf -> index = 0;		// find first device
  else pciconf -> index ++;			// find next device

  asm ("pushl %%cs\n"
       "call *%%ebx\n"
       : "=a" (return_code), 			// where to put return code
         "=b" (pci_address)			// where to put composite pci address
       : "a" ((PCI_FUNCTION_ID << 8) | FIND_PCI_CLASS_CODE), 
         "b" (pcibios_entry), 			// entrypoint to pci bios routine
         "c" (classcode), 			// class code (24 bits)
         "S" (pciconf -> index));		// index number

  if ((return_code & 0xFF00) == (SUCCESSFUL << 8)) {
    pciconf -> confaddr = pci_address;
    return (1);
  }
  if ((return_code & 0xFF00) == (DEVICE_NOT_FOUND << 8)) return (0);
  oz_crash ("oz_dev_pci_conf_scan_didvid: return code %X", return_code);
}
#endif

/**************************************************************/
/* Initialization routine to read PCI interrupt routing table */
/**************************************************************/

static int initroutetable (void)

{
  OZ_Dev_Pci_Conf pciconf;
  struct { uWord w1, w2, w3, w4; } route_table_descr;
  uByte intline, intpin;
  uLong didvid, i, irqs_available, return_code, slots;

  if (pcibios_entry == 0) initialize ();	// if not initialized, initialize
  if (pcibios_entry == 1) return (0);		// if init failed, return 'no such device'

  for (slots = 0; ++ slots < MAX_PCI_SLOTS;) {
    route_table_descr.w1 = slots * sizeof *route_table;
    route_table_descr.w2 = (uWord)((OZ_Pointer)route_table);
    route_table_descr.w3 = ((OZ_Pointer)route_table) >> 16;
    route_table_descr.w4 = 0;

    memset (route_table, 0, route_table_descr.w1);

    asm ("xorl  %%ebx,%%ebx\n"
         "pushl %%cs\n"
         "call *%%ecx\n"
         : "=a" (return_code), 			// where to put return code
           "=b" (irqs_available)		// where to put available irq bitmap
         : "a" ((PCI_FUNCTION_ID << 8) | GET_IRQ_ROUTING_OPTIONS), 
           "c" (pcibios_entry), 		// entrypoint to pci bios routine
           "D" (&route_table_descr)		// pointer to route table descriptor
         : "memory");

    if ((return_code & 0xFF00) != (BUFFER_TOO_SMALL << 8)) break;
  }

  if ((return_code & 0xFF00) != (SUCCESSFUL << 8)) {
    oz_knl_printk ("oz_dev_pci irqroute: return_code %X", return_code);
    return (0);
  }

  oz_knl_printk ("oz_dev_pci irqroute: slots %u\n", slots);

  memset (&pciconf, 0, sizeof pciconf);
  pciconf.objtype = OZ_OBJTYPE_PCICONF;

  for (i = 0; i < slots; i ++) {

    pciconf.confaddr = CONFADDR (route_table[i].pci_bus_number, 
                                 route_table[i].pci_dev_number >> 3, 
                                 route_table[i].pci_dev_number & 7);

    didvid  = oz_dev_pci_conf_inl (&pciconf, OZ_DEV_PCI_CONF_L_DIDVID);
    intline = oz_dev_pci_conf_inb (&pciconf, OZ_DEV_PCI_CONF_B_INTLINE);
    intpin  = oz_dev_pci_conf_inb (&pciconf, OZ_DEV_PCI_CONF_B_INTPIN);

    oz_knl_printk (" [%2u] %2u/%2u/%u  A:%2.2X %2.2X%2.2X B:%2.2X %2.2X%2.2X C:%2.2X %2.2X%2.2X D:%2.2X %2.2X%2.2X  %8.8X %3u/%u\n", 
	i, 
	CONFBUS (pciconf.confaddr), CONFDEV (pciconf.confaddr), CONFFUNC (pciconf.confaddr), 
	route_table[i].link_int_a, 
	route_table[i].irq_bitmap_hi_a, 
	route_table[i].irq_bitmap_lo_a, 
	route_table[i].link_int_b, 
	route_table[i].irq_bitmap_hi_b, 
	route_table[i].irq_bitmap_lo_b, 
	route_table[i].link_int_c, 
	route_table[i].irq_bitmap_hi_c, 
	route_table[i].irq_bitmap_lo_c, 
	route_table[i].link_int_d, 
	route_table[i].irq_bitmap_hi_d, 
	route_table[i].irq_bitmap_lo_d, 
	didvid, intline, intpin);
  }

  oz_knl_printk ("oz_dev_pci irqroute: irqs_available %4.4X\n", irqs_available);	// ?? comes out zero on Asus P2B-D
											// ?? seems ok on MSI K7D

  pcislots = slots;

  return (1);
}

/******************************************************/
/* Initialization routine to find pci bios entrypoint */
/******************************************************/

static void initialize (void)

{
  int i;
  Sdep *sdep;
  uByte cksum, return_code;
  uLong pcibios_base, pcibios_offset, pcibios_size;

  /* Require softint level so 'initinprog' lock doesn't have to block out all interrupts */

  if (oz_hw_cpu_smplevel () != OZ_SMPLOCK_SOFTINT) oz_crash ("oz_dev_pci initialize: must be at softint level");

  /* Make sure another CPU isn't doing this same thing */
  /* If it is, wait for it then assume it finished     */

  if (oz_hw_atomic_set_long (&initinprog, 1)) {
    while (initinprog) {}
    return;
  }

  /* Initialize some static data */

  oz_hw_smplock_init (sizeof smplock_dv, &smplock_dv, OZ_SMPLOCK_LEVEL_DV);
  pciconfs = NULL;
  pcibios_entry = 1;

  /* Locate the Sdep struct in memory.  It is on a 16-byte boundary and begins with '_32_'. */

  oz_knl_printk ("oz_dev_pci_init: scanning for 32-bit bios ...\n");
  for (sdep = (Sdep *)0x0E0000; sdep < (Sdep *)0x100000; sdep ++) {
    if (sdep -> signature == 0x5F32335F) {
      if (sdep -> version != 0) oz_knl_printk ("oz_dev_pci_init: found signature at %p but version is %u\n", sdep, sdep -> version);
      else {
        cksum = 0;
        for (i = 0; i < sdep -> length * 16; i ++) cksum += ((uByte *)sdep)[i];
        if (cksum == 0) goto foundit;
        oz_knl_printk ("oz_dev_pci_init: found signature at %p but checksum is bad\n", sdep);
      }
    }
  }
  oz_knl_printk ("oz_dev_pci_init: 32-bit bios not found\n");
  goto rtn;

foundit:
  oz_knl_printk ("oz_dev_pci_init: 32-bit bios found at %X\n", sdep);
  asm ("movl  $0x49435024,%%eax\n"	// put '$PCI' in %eax
       "xorl  %%ebx,%%ebx\n"		// clear %ebx
       "pushl %%cs\n"			// make a 'far' call 
       "call  *%%ecx\n"			// ... to 'sdep -> bsdentry' routine
       : "=a" (return_code), 		// put the return code here
         "=b" (pcibios_base), 		// put the pci bios base address here
         "=c" (pcibios_size), 		// put the pci bios size here
         "=d" (pcibios_offset)		// put the pci bios offset here
       : "c" (sdep -> bsdentry));	// get the entrypoint here
  if (return_code != 0) {
    oz_knl_printk ("oz_dev_pci_init: 'find pci bios' return code %X\n", return_code);
  } else {
    pcibios_entry = pcibios_base + pcibios_offset;
    oz_knl_printk ("oz_dev_pci_init: pci bios entrypoint at %X\n", pcibios_entry);
  }

  /* Tell other CPU's we're done */

rtn:
  initinprog = 0;
}

/************************************************************************/
/*									*/
/*  DMA mapping routines -						*/
/*									*/
/*    Since our memory addresses = PCI addresses, these are a NOP	*/
/*    We also don't even bother to define the 64-bit routines, since 	*/
/*    these CPU's don't have 64-bit PCI busses				*/
/*									*/
/************************************************************************/

/************************************************************************/
/*									*/
/*  Allocate mapping for DMA						*/
/*									*/
/*    Input:								*/
/*									*/
/*	pciconf = device the mapping is for				*/
/*	npages  = number of pages needed				*/
/*	flags   = OZ_DEV_PCI_DMAFLAG_64K : don't cross 64K boundary	*/
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

  OZ_KNL_CHKOBJTYPE (pciconf, OZ_OBJTYPE_PCICONF);

  if (flags & ~OZ_DEV_PCI_DMAFLAG_64K) {
    oz_knl_printk ("oz_dev_pci_dma32map_alloc: no support for flags %X\n", flags & ~OZ_DEV_PCI_DMAFLAG_64K);
    return (NULL);
  }

  dma32map = OZ_KNL_PCMALLOQ ((npages * sizeof dma32map -> ieeedma32[0]) + sizeof *dma32map);
  if (dma32map != NULL) {
    dma32map -> objtype = OZ_OBJTYPE_PCIDMA32MAP;
    dma32map -> flags   = flags;
    dma32map -> npages  = npages;
    dma32map -> busy    = 0;
    size = oz_knl_misc_sva2pa (dma32map -> ieeedma32, &ppn, &ppo);
    if (size < npages * sizeof dma32map -> ieeedma32[0]) oz_crash ("oz_dev_pci_dma32map_alloc: bad ieeedma32 buffer");
    dma32map -> mappciaddr = (ppn << OZ_HW_L2PAGESIZE) + ppo;
    if (dma32map -> flags & OZ_DEV_PCI_DMAFLAG_64K) {		// see if descriptor can't cross 64K boundary
      if (npages * sizeof dma32map -> ieeedma32[0] > 65536 - (dma32map -> mappciaddr & 65535)) {
        oz_crash ("oz_dev_dma32map_alloc: descriptor array crosses 64K boundary"); // (it *should* fit in a single page)
      }
    }
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
    phyaddr   = (*phypages << OZ_HW_L2PAGESIZE) + offset;	// get phys addr of transfer = pci address under direct mapping
    bytecnt   = PAGESIZE - offset;				// get bytecount to end of page
    if (bytecnt > size) bytecnt = size;				// ... but stop at end of transfer
    if (phyaddr != endaddr) goto newentry;			// see if it starts where last entry left off
    if (!(dma32map -> flags & OZ_DEV_PCI_DMAFLAG_64K)) goto mergelast;
    if ((endaddr & 65535) == 0) goto newentry;
mergelast:
    dma32map -> ieeedma32[--i].bytecnt += bytecnt;		// if so, merge with last entry
    goto nextseg;
newentry:
    if (i == dma32map -> npages) {
      OZ_HW_MB;
      dma32map -> busy = 0;
      oz_knl_printk ("oz_dev_pci_dma32map_start: too many pages\n");
      return (-1);
    }
    dma32map -> ieeedma32[i].phyaddr = phyaddr;			// if not, make a new entry
    dma32map -> ieeedma32[i].bytecnt = bytecnt;
nextseg:
    if (dma32map -> flags & OZ_DEV_PCI_DMAFLAG_64K) {		// see if segment can't cross 64K boundary
      endaddr = 65536 - (dma32map -> ieeedma32[i].phyaddr & 65535); // calc bytes to end of 64K block
      if (dma32map -> ieeedma32[i].bytecnt > endaddr) {		// see if we're trying to go past end of 64K block
        bytecnt -= dma32map -> ieeedma32[i].bytecnt - endaddr;	// if so, subtract off how much we overflowed by
        dma32map -> ieeedma32[i].bytecnt = endaddr;		// ... and just go to end of the 64K block
      }
    }
    i ++;
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
  OZ_Dev_Pci_Irq *pciirq;

  pciirq = OZ_KNL_NPPMALLOC (sizeof *pciirq);
  pciirq -> objtype = OZ_OBJTYPE_PCIIRQ;
  pciirq -> entry   = entry;
  pciirq -> param   = param;
  pciirq -> irq_many.entry = pci_interrupt;
  pciirq -> irq_many.param = pciirq;
  pciirq -> irq_many.descr = "pci_interrupt";

  if (intpin >= 8) pciirq -> intline = intpin - 8;
  else pciirq -> intline = oz_dev_pci_conf_inb (pciconf, OZ_DEV_PCI_CONF_B_INTLINE);
  pciirq -> smplock = oz_hw486_irq_many_add (pciirq -> intline, &(pciirq -> irq_many));

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
  return (pciirq -> smplock);
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

  ll = oz_hw_smplock_wait (pciirq -> smplock);
  pciirq -> entry = entry;
  pciirq -> param = param;
  oz_hw_smplock_clr (pciirq -> smplock, ll);
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
  OZ_KNL_CHKOBJTYPE (pciirq, OZ_OBJTYPE_PCIIRQ);
  oz_hw486_irq_many_rem (pciirq -> intline, &(pciirq -> irq_many));
  OZ_KNL_NPPFREE (pciirq);
}

/************************************************************************/
/*									*/
/*  Internal PCI interrupt wrapper routine				*/
/*									*/
/************************************************************************/

static int pci_interrupt (void *pciirqv, OZ_Mchargs *mchargs)

{
  OZ_Dev_Pci_Irq *pciirq;

  pciirq = pciirqv;
  OZ_KNL_CHKOBJTYPE (pciirq, OZ_OBJTYPE_PCIIRQ);
  (*(pciirq -> entry)) (pciirq -> param, mchargs);
  return (0);
}

/************************************************/
/* Read and Write configuration space registers */
/************************************************/

/* Range of pcibus is from 0 to 255 */
/* Range of pcidev when pcibus is 0 : from 0 to 20 */
/*                             else : from 0 to 31 */
/* Range of pcifunc is from 0 to 7 */

uByte oz_dev_pci_conf_inb (OZ_Dev_Pci_Conf *pciconf, uByte confadd)

{
  uLong return_code, return_value;

  OZ_KNL_CHKOBJTYPE (pciconf, OZ_OBJTYPE_PCICONF);

  asm ("pushl %%cs\n"						// do a 'far' call
       "call *%%edx\n"
       : "=a" (return_code), 
         "=c" (return_value)
       : "a" ((PCI_FUNCTION_ID << 8) + READ_CONFIG_BYTE), 	// get PCI_FUNCTION_ID, READ_CONFIG_BYTE in %eax
         "b" (pciconf -> confaddr), 				// get pci bus, dev, func in %ebx
         "d" (pcibios_entry), 					// get pcibios_entry in %edx
         "D" ((uLong)confadd));					// get conf register number in %edi

  if ((return_code & 0xFF00) != 0) oz_crash ("oz_dev_pci_conf_inb: error code %X", return_code);

  return (return_value);
}

uWord oz_dev_pci_conf_inw (OZ_Dev_Pci_Conf *pciconf, uByte confadd)

{
  uLong return_code, return_value;

  OZ_KNL_CHKOBJTYPE (pciconf, OZ_OBJTYPE_PCICONF);

  if (confadd & 1) oz_crash ("oz_dev_pci_conf_inw: unaligned address %X", confadd);

  asm ("pushl %%cs\n"						// do a 'far' call
       "call *%%edx\n"
       : "=a" (return_code), 
         "=c" (return_value)
       : "a" ((PCI_FUNCTION_ID << 8) + READ_CONFIG_WORD), 	// get PCI_FUNCTION_ID, READ_CONFIG_WORD in %eax
         "b" (pciconf -> confaddr), 				// get pci bus, dev, func in %ebx
         "d" (pcibios_entry), 					// get pcibios_entry in %edx
         "D" ((uLong)confadd));					// get conf register number in %edi

  if ((return_code & 0xFF00) != 0) oz_crash ("oz_dev_pci_conf_inw: error code %X", return_code);

  return (return_value);
}

uLong oz_dev_pci_conf_inl (OZ_Dev_Pci_Conf *pciconf, uByte confadd)

{
  uLong return_code, return_value;

  OZ_KNL_CHKOBJTYPE (pciconf, OZ_OBJTYPE_PCICONF);

  if (confadd & 3) oz_crash ("oz_dev_pci_conf_inl: unaligned address %X", confadd);

  asm ("pushl %%cs\n"						// do a 'far' call
       "call *%%edx\n"
       : "=a" (return_code), 
         "=c" (return_value)
       : "a" ((PCI_FUNCTION_ID << 8) + READ_CONFIG_LONG), 	// get PCI_FUNCTION_ID, READ_CONFIG_LONG in %eax
         "b" (pciconf -> confaddr), 				// get pci bus, dev, func in %ebx
         "d" (pcibios_entry), 					// get pcibios_entry in %edx
         "D" ((uLong)confadd));					// get conf register number in %edi

  if ((return_code & 0xFF00) != 0) oz_crash ("oz_dev_pci_conf_inl: error code %X", return_code);

  return (return_value);
}

void oz_dev_pci_conf_outb (uByte value, OZ_Dev_Pci_Conf *pciconf, uByte confadd)

{
  uLong return_code;

  OZ_KNL_CHKOBJTYPE (pciconf, OZ_OBJTYPE_PCICONF);

  asm ("pushl %%cs\n"		// do a 'far' call
       "call *%%edx\n"
       : "=a" (return_code)
       : "a" ((PCI_FUNCTION_ID << 8) + WRITE_CONFIG_BYTE), 	// get PCI_FUNCTION_ID, WRITE_CONFIG_BYTE into %eax
         "b" (pciconf -> confaddr), 				// get pciconf -> confaddr into %ebx
         "c" ((uLong)value), 					// get value into %ecx
         "d" (pcibios_entry), 					// get pci bios entrypoint into %edx
         "D" ((uLong)confadd));					// get config register number into %edi

  if ((return_code & 0xFF00) != 0) oz_crash ("oz_dev_pci_conf_outb: error code %X", return_code);
}

void oz_dev_pci_conf_outw (uWord value, OZ_Dev_Pci_Conf *pciconf, uByte confadd)

{
  uLong return_code;

  OZ_KNL_CHKOBJTYPE (pciconf, OZ_OBJTYPE_PCICONF);

  if (confadd & 1) oz_crash ("oz_dev_pci_conf_outw: unaligned address %X", confadd);

  asm ("pushl %%cs\n"		// do a 'far' call
       "call *%%edx\n"
       : "=a" (return_code)
       : "a" ((PCI_FUNCTION_ID << 8) + WRITE_CONFIG_WORD), 	// get PCI_FUNCTION_ID, WRITE_CONFIG_WORD into %eax
         "b" (pciconf -> confaddr), 				// get pciconf -> confaddr into %ebx
         "c" ((uLong)value), 					// get value into %ecx
         "d" (pcibios_entry), 					// get pci bios entrypoint into %edx
         "D" ((uLong)confadd));					// get config register number into %edi

  if ((return_code & 0xFF00) != 0) oz_crash ("oz_dev_pci_conf_outw: error code %X", return_code);
}

void oz_dev_pci_conf_outl (uLong value, OZ_Dev_Pci_Conf *pciconf, uByte confadd)

{
  uLong return_code;

  OZ_KNL_CHKOBJTYPE (pciconf, OZ_OBJTYPE_PCICONF);

  if (confadd & 3) oz_crash ("oz_dev_pci_conf_outl: unaligned address %X", confadd);

  asm ("pushl %%cs\n"		// do a 'far' call
       "call *%%edx\n"
       : "=a" (return_code)
       : "a" ((PCI_FUNCTION_ID << 8) + WRITE_CONFIG_LONG), 	// get PCI_FUNCTION_ID, WRITE_CONFIG_LONG into %eax
         "b" (pciconf -> confaddr), 				// get pciconf -> confaddr into %ebx
         "c" ((uLong)value), 					// get value into %ecx
         "d" (pcibios_entry), 					// get pci bios entrypoint into %edx
         "D" ((uLong)confadd));					// get config register number into %edi

  if ((return_code & 0xFF00) != 0) oz_crash ("oz_dev_pci_conf_outl: error code %X", return_code);
}
