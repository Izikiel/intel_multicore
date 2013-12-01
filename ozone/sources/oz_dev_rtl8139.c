//+++2003-12-12
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
//---2003-12-12

/************************************************************************/
/*									*/
/*  Realtek 8139 ethernet driver					*/
/*									*/
/************************************************************************/

#include "ozone.h"

#include "oz_dev_pci.h"
#include "oz_dev_timer.h"
#include "oz_knl_devio.h"
#include "oz_knl_event.h"
#include "oz_knl_hw.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_misc.h"
#include "oz_knl_phymem.h"
#include "oz_knl_procmode.h"
#include "oz_knl_sdata.h"
#include "oz_knl_section.h"
#include "oz_knl_spte.h"
#include "oz_knl_status.h"
#include "oz_io_ether.h"
#include "oz_sys_dateconv.h"
#include "oz_sys_xprintf.h"

#define ARPHWTYPE 1
#define ADDRSIZE 6
#define PROTOSIZE 2
#define DATASIZE 1500
#define BUFFSIZE (2*ADDRSIZE+PROTOSIZE+DATASIZE+4)
#define USESIOPORTS 1

#define CEQENADDR(__ea1,__ea2) ((*((uLong *)(__ea1)) == *((uLong *)(__ea2))) && (*((uWord *)(__ea1 + 4)) == *((uWord *)(__ea2 + 4))))

typedef struct Chnex Chnex;
typedef struct Devex Devex;
typedef struct Iopex Iopex;
typedef struct Rxbuf Rxbuf;

#define ETH_ZLEN (64)			/* smallest number of data bytes to transmit */

enum pci_flags_bit { PCI_USES_IO=1, PCI_USES_MEM=2, PCI_USES_MASTER=4, PCI_ADDR0=0x10<<0, PCI_ADDR1=0x10<<1, PCI_ADDR2=0x10<<2, PCI_ADDR3=0x10<<3 };

/* PCI Tuning Parameters - */

#define TX_FIFO_THRESH 256		/* Threshold is bytes transferred to chip before transmission starts. */
					/* In bytes, rounded down to 32 byte units. */

					/* The following settings are log_2(bytes)-4:  0 == 16 bytes .. 6==1024. */
#define RX_FIFO_THRESH  4		/* Rx buffer level before first PCI xfer.  */
#define RX_DMA_BURST    6		/* Maximum PCI burst, '4' is 256 bytes */
#define TX_DMA_BURST    6		/* Calculate as 16<<val.  Chip doc recommends 1024 bytes = 6. */

/* The receive buffers are placed in physically contiguous memory that is also mapped to contiguous system virtual addresses */

#define RX_RING_TOTAL_IDX 2				/* 0==8K, 1==16K, 2==32K, 3==64K */
#define RX_RING_TOTAL (8192 << RX_RING_TOTAL_IDX)	/* total size of receive ring area */

enum RxStatusBits { RxMulticast=0x8000, RxPhysical=0x4000, RxBroadcast=0x2000, RxBadSymbol=0x0020, RxRunt=0x0010, RxTooLong=0x0008, RxCRCErr=0x0004, RxBadAlign=0x0002, RxStatusOK=0x0001 };

/* The transmit buffers are allocated in non-paged pool as needed and queued to the controller as */
/* fast as it can do them.  The controller can process up to four transmit requests at a time.    */

#define TX_DATA_SIZE BUFFSIZE			/* size of each transmit buffer (the largest a transmit buffer can be) */
#define NUM_TX_DESC 4				/* number of transmit buffers (this is set by hardware design - do not change it) */

/* Format of ethernet buffers */

typedef struct { uByte dstaddr[ADDRSIZE];
                 uByte srcaddr[ADDRSIZE];
                 uByte proto[PROTOSIZE];
                 uByte data[DATASIZE];
                 uByte crc[4];
               } Ether_buf;

/* Format of receive buffers passed via rcvfre, etc */

struct Rxbuf { Rxbuf *next;		/* next in receive list */
               uLong dlen;		/* length of received data (not incl header or crc) */
               Ether_buf buf;		/* ethernet receive data */
             };

/* Format of transmit data buffers */

#define Txbuf Ether_buf

/* Device extension structure */

struct Devex { OZ_Devunit *devunit;		/* devunit pointer */
               const char *name;		/* devunit name string pointer */
               uLong iobase;			/* base I/O address (if odd, use I/O space, if even, use memory space) */
               uByte enaddr[ADDRSIZE];		/* hardware address */
               uByte intvec;			/* irq vector */
               uByte old_msr;			/* previously reported media status bits */
               uWord old_bmcr;
               uWord old_bmsr;
               OZ_Smplock *smplock;		/* pointer to irq level smp lock */

               Chnex *chnexs;			/* all open channels on the device */
               uLong promiscuous;		/* >0: promiscuous mode; =0: normal */

               uLong rxring_pciad;		/* receive ring base pci address (RX_RING_TOTAL bytes) */
               uByte *rxring_vaddr;		/* receive ring base virtual address */
               OZ_Mempage npages;		/* number of physical pages allocated */
               OZ_Mempage phypage;		/* first physical page number */
               OZ_Mempage syspage;		/* first system virtual page */

               uLong rxring_offs;		/* currently received buffer ring offset */

               Iopex *txreqh;			/* waiting transmit requests */
               Iopex **txreqt;
               int txfree;			/* count of free transmit slots (in txreqip array) */
               int txnexti;			/* transmit round-robin index (in txreqip array) */
               Iopex *txreqip[NUM_TX_DESC];	/* array of transmits in progress */
						/* if non-NULL, corresponding xmtpcidma is busy */

               OZ_Dev_Pci_Irq *pci_irq;		/* interrupt block */
               OZ_Dev_Pci_Dma32map *rcvpcidma;	/* receive dma mapping - always 'busy' */
               OZ_Dev_Pci_Dma32map *xmtpcidma[NUM_TX_DESC]; /* only busy when corresponding txreqip entry non-NULL */
             };

/* Channel extension structure */

struct Chnex { Chnex *next;			/* next in devex->chnexs list */
               OZ_IO_ether_open ether_open;	/* open parameters */

               /* Receive related data */

               Iopex *rxreqh;			/* list of pending receive requests */
               Iopex **rxreqt;			/* (NULL: closed; else: open) */
               uLong rcvmissed;			/* number of packets that didn't have a receive waiting */
             };

/* I/O extension structure */

struct Iopex { Iopex *next;				/* next in list of requests */
               OZ_Ioop *ioop;				/* I/O operation block pointer */
               OZ_Procmode procmode;			/* processor mode of request */
               Devex *devex;				/* ethernet device pointer */

               /* Receive related data */

               OZ_IO_ether_receive ether_receive;	/* receive request I/O parameters */
               Rxbuf *rxbuf;				/* buffer to receive into */

               /* Transmit related data */

               OZ_IO_ether_transmit ether_transmit;	/* transmit request I/O parameters */
               Txbuf *txbuf;				/* transmit buffer virtual address */
               OZ_Mempage txbufpp;			/* transmit buffer physical page */
               uLong txbufof;				/* transmit buffer page offset */
             };

/* Function table */

static int rtl8139_shutdown (OZ_Devunit *devunit, void *devex);
static uLong rtl8139_assign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode);
static int rtl8139_deassign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv);
static uLong rtl8139_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode, 
                            OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap);

static const OZ_Devfunc rtl8139_functable = { sizeof (Devex), sizeof (Chnex), sizeof (Iopex), 0, rtl8139_shutdown, NULL, 
                                              NULL, rtl8139_assign, rtl8139_deassign, NULL, rtl8139_start, NULL };

/* Internal static data */

static int initialized = 0;
static OZ_Devclass *devclass;
static OZ_Devdriver *devdriver;
static uByte broadcast[ADDRSIZE];

/* Internal routines */

static int foundone (void *dummy, uLong didvid, int func, OZ_Dev_Pci_Conf *pciconf, char const *addrsuffix, char const *addrdescrip);
static uLong ether_enable (Devex *devex);
static int close_channel (Devex *devex, Chnex *chnexx);
static void transmit_start (Devex *devex);
static int ether_interrupt (void *devexv, OZ_Mchargs *mchargs);

static void rtl_init (Devex *rtl);
static unsigned rtl_eeprom (Devex *rtl, unsigned location);
static void rtl_interrupt (void *rtlv, OZ_Mchargs *mchargs);
static void receive_done (Devex *rtl);
static void message_received (Devex *devex, uLong length, uByte *ringdata);
static void receive_finish (void *iopexv, int finok, uLong *status_r);
static void transmit_done (Devex *devex);
static void transmit_finish (void *iopexv, int finok, uLong *status_r);
static int rtl_reset (Devex *rtl);
static uLong rtl_reset_done (void *rtlv);
static void linkstatuschange (Devex *rtl, int all);

/************************************************************************/
/*									*/
/*  I/O space registers							*/
/*									*/
/************************************************************************/

#define IO_L_IDR0_3  0x00	/* id register 0-3 (ethernet hardware address, part 1) */
#define IO_L_IDR4_5  0x04	/* id register 4-5 (ethernet hardware address, part 2) */
#define IO_L_MAR0_3  0x08	/* multicast register 0-3 */
#define IO_L_MAR4_7  0x0C	/* multicast register 4-7 */
#define IO_L_TSD0    0x10	/* transmist status of descriptor 0 */
#define IO_L_TSD1    0x14	/* transmist status of descriptor 1 */
#define IO_L_TSD2    0x18	/* transmist status of descriptor 2 */
#define IO_L_TSD3    0x1C	/* transmist status of descriptor 3 */
	enum TxStatusBits { TxHostOwns=0x2000, TxUnderrun=0x4000, TxStatOK=0x8000, TxOutOfWindow=0x20000000, TxAborted=0x40000000, TxCarrierLost=0x80000000 };
#define IO_L_TSAD0   0x20	/* transmit start address of descriptor 0 */
#define IO_L_TSAD1   0x24	/* transmit start address of descriptor 1 */
#define IO_L_TSAD2   0x28	/* transmit start address of descriptor 2 */
#define IO_L_TSAD3   0x2C	/* transmit start address of descriptor 3 */
#define IO_L_RBSTART 0x30	/* receive buffer start address */
#define IO_W_ERBCR   0x34	/* early receive byte count register */
#define IO_B_ERSR    0x36	/* early rx status register */
#define IO_B_CR      0x37	/* command register */
	enum ChipCmdBits { CmdReset=0x10, CmdRxEnb=0x08, CmdTxEnb=0x04, RxBufEmpty=0x01 };
#define IO_W_CAPR    0x38	/* current address of packet read */
#define IO_W_CBA     0x3A	/* current buffer address : the initial value is 0 - it reflects total recived byte count in the rx buffer */
#define IO_W_IMR     0x3C	/* interrupt mask register */
#define IO_W_ISR     0x3E	/* interrupt status register */
	enum IntrStatusBits { PCIErr=0x8000, PCSTimeout=0x4000, CableLenChange=0x2000, RxFIFOOver=0x40, LinkChg=0x20, RxOverflow=0x10, TxErr=0x08, TxOK=0x04, RxErr=0x02, RxOK=0x01 };
#define IO_L_TCR     0x40	/* transmit configuration register */
#define TCR_CLRABT 1
#define IO_L_RCR     0x44	/* receive configuration register */
	enum rx_mode_bits { RxCfgWrap=0x80, AcceptErr=0x20, AcceptRunt=0x10, AcceptBroadcast=0x08, AcceptMulticast=0x04, AcceptMyPhys=0x02, AcceptAllPhys=0x01 };
#define IO_L_TCTR    0x48	/* timer count register : this register contains a 32bit general-purpose timer - writing any value will reset the timer to zero */
#define IO_L_MPC     0x4C	/* missed packet counter (24bit) : indicates the number of packets discarded due to rx fifo overflow - any write resets to zero */
#define IO_B_9346CR  0x50	/* 93c46 command register */
#define IO_B_CONFIG0 0x51	/* rtl8139 config0 register */
#define IO_B_CONFIG1 0x52	/* rtl8139 config1 register */
#define IO_B_MSR     0x58	/* media status register */
#define IO_B_HLTCLK  0x5B	/* halt clock register : it is referenced by 25MHz */
#define IO_W_MULTINT 0x5C	/* multiple interrupt */
#define IO_W_RERID   0x5E	/* PCI revision ID (0x10) */
#define IO_W_TSAD    0x60	/* transmit status of all descriptors : the TOK, TUN, TABT and OWN bits of all descriptors */
#define IO_W_BMCR    0x62	/* basic mode control register */
#define IO_W_BMSR    0x64	/* basic mode status register */
#define IO_W_ANAR    0x66	/* auto-negotiation advertisement register */
#define IO_W_ANLPAR  0x68	/* auto-negotiation link partner ability register */
#define IO_W_ANER    0x6A	/* auto-negotiation expansion register */
#define IO_W_DIS     0x6C	/* disconnect counter */
#define IO_W_FCSC    0x6E	/* false carrior sense counter */
#define IO_W_NWAYTR  0x70	/* nway test register */
#define IO_W_REC     0x72	/* rx_er counter */
#define IO_W_CSCR    0x74	/* cs configuration register */
#define IO_L_PARA78  0x78
#define IO_L_PARA7C  0x7C

/************************************************************************/
/*									*/
/*  Access ethernet card's I/O registers				*/
/*									*/
/************************************************************************/

#if USESIOPORTS

#define ether_inb(iobase,offset) oz_dev_pci_inb (iobase - 1 + offset)
#define ether_inw(iobase,offset) oz_dev_pci_inw (iobase - 1 + offset)
#define ether_inl(iobase,offset) oz_dev_pci_inl (iobase - 1 + offset)

#define ether_outb(value,iobase,offset) oz_dev_pci_outb (value, iobase - 1 + offset)
#define ether_outw(value,iobase,offset) oz_dev_pci_outw (value, iobase - 1 + offset)
#define ether_outl(value,iobase,offset) oz_dev_pci_outl (value, iobase - 1 + offset)

#else

#define ether_inb(iobase,offset) oz_dev_pci_rdb (iobase + offset)
#define ether_inw(iobase,offset) oz_dev_pci_rdw (iobase + offset)
#define ether_inl(iobase,offset) oz_dev_pci_rdl (iobase + offset)

#define ether_outb(value,iobase,offset) oz_dev_pci_wtb (value, iobase + offset)
#define ether_outw(value,iobase,offset) oz_dev_pci_wtw (value, iobase + offset)
#define ether_outl(value,iobase,offset) oz_dev_pci_wtl (value, iobase + offset)

#endif

/************************************************************************/
/*									*/
/*  Boot-time initialization routine					*/
/*									*/
/************************************************************************/

void oz_dev_rtl8139_init ()

{
  if (initialized) return;

  oz_knl_printk ("oz_dev_rtl8139_init\n");
  initialized = 1;

  memset (broadcast, -1, sizeof broadcast);

  devclass  = oz_knl_devclass_create (OZ_IO_ETHER_CLASSNAME, OZ_IO_ETHER_BASE, OZ_IO_ETHER_MASK, "rtl8139");
  devdriver = oz_knl_devdriver_create (devclass, "rtl8139");

  oz_dev_pci_find_didvid (0x813910EC, 0, OZ_DEV_PCI_FINDFLAG_HASBASADR0, foundone, NULL);
  oz_dev_pci_find_didvid (0x12111113, 0, OZ_DEV_PCI_FINDFLAG_HASBASADR0, foundone, NULL);
}

static int foundone (void *dummy, uLong didvid, int func, OZ_Dev_Pci_Conf *pciconf, char const *addrsuffix, char const *addrdescrip)

{
  uByte enaddr[ADDRSIZE], intvec;
  char unitdesc[OZ_DEVUNIT_DESCSIZE], unitname[OZ_DEVUNIT_NAMESIZE];
  Devex *devex;
  int i, j;
  uLong iobase, sts;
  OZ_Devunit *devunit;
  uWord pcicmd;

  oz_knl_printk ("oz_dev_rtl8139: ethernet controller found: %s\n", addrdescrip);

  /* Set chip enable bits */

  pcicmd  = oz_dev_pci_conf_inw (pciconf, OZ_DEV_PCI_CONF_W_PCICMD);
  pcicmd |= PCI_USES_IO | PCI_USES_MASTER;
  oz_dev_pci_conf_outw (pcicmd, pciconf, OZ_DEV_PCI_CONF_W_PCICMD);

  /* Create device unit struct and fill it in */

  iobase = oz_dev_pci_conf_inl (pciconf, OZ_DEV_PCI_CONF_L_BASADR0);
  intvec = oz_dev_pci_conf_inb (pciconf, OZ_DEV_PCI_CONF_B_INTLINE);

  if ((iobase ^ USESIOPORTS) & 1) {
    oz_knl_printk ("oz_dev_rtl8139:   but uses %s ports and driver configed for %s ports\n", 
		(iobase & 1) ? "IO" : "mem", (USESIOPORTS & 1) ? "IO" : "mem");
    return (1);
  }

  *(uLong *)(enaddr + 0) = ether_inl (iobase, IO_L_IDR0_3);
  *(uWord *)(enaddr + 4) = ether_inl (iobase, IO_L_IDR4_5);

  oz_sys_sprintf (sizeof unitdesc, unitdesc, "Realtek 8139 port %X, irq %u, addr %2.2X-%2.2X-%2.2X-%2.2X-%2.2X-%2.2X", 
	iobase, intvec, enaddr[0], enaddr[1], enaddr[2], enaddr[3], enaddr[4], enaddr[5]);

  oz_sys_sprintf (sizeof unitname, unitname, "rtl8139_%s", addrsuffix);

  devunit = oz_knl_devunit_create (devdriver, unitname, unitdesc, &rtl8139_functable, 0, oz_s_secattr_sysdev);
  devex   = oz_knl_devunit_ex (devunit);

  devex -> devunit = devunit;
  devex -> name    = oz_knl_devunit_devname (devunit);
  devex -> iobase  = oz_dev_pci_conf_inl (pciconf, OZ_DEV_PCI_CONF_L_BASADR0);
  devex -> intvec  = oz_dev_pci_conf_inb (pciconf, OZ_DEV_PCI_CONF_B_INTLINE);
  devex -> pci_irq = oz_dev_pci_irq_alloc (pciconf, 0, rtl_interrupt, devex);
  devex -> promiscuous = 0;

  memcpy (devex -> enaddr, enaddr, sizeof devex -> enaddr);

  /* Allocate dma mapping buffers -          */
  /* One big one for the receive ring buffer */
  /* One little one for each transmit buffer */

  devex -> rcvpcidma = oz_dev_pci_dma32map_alloc (pciconf, RX_RING_TOTAL >> OZ_HW_L2PAGESIZE, 0);
  for (i = 0; i < NUM_TX_DESC; i ++) {
    devex -> xmtpcidma[i] = oz_dev_pci_dma32map_alloc (pciconf, 1, 0);
  }

  /* Start hardware going */

  ether_enable (devex);

  return (1);
}

/************************************************************************/
/*									*/
/*  Shutdown device							*/
/*									*/
/************************************************************************/

static int rtl8139_shutdown (OZ_Devunit *devunit, void *devexv)

{
  Devex *devex;

  devex = devexv;
  ether_outb (CmdReset, devex -> iobase, IO_B_CR);  /* software reset the chip */
  return (1);
}

/************************************************************************/
/*									*/
/*  A new channel was assigned to the device				*/
/*  This routine initializes the chnex area				*/
/*									*/
/************************************************************************/

static uLong rtl8139_assign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode)

{
  memset (chnexv, 0, sizeof (Chnex));
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  A channel is about to be deassigned from a device			*/
/*  Here we do a close if it is open					*/
/*									*/
/************************************************************************/

static int rtl8139_deassign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv)

{
  Chnex *chnex;

  chnex = chnexv;

  if (chnex -> rxreqt != NULL) {
    close_channel (devexv, chnex);
    if (chnex -> rxreqt != NULL) oz_crash ("oz_dev_rtl8139 deassign: channel still open after close");
  }

  return (0);
}

/************************************************************************/
/*									*/
/*  Start performing an ethernet i/o function				*/
/*									*/
/************************************************************************/

static uLong rtl8139_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode, 
                            OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap)

{
  Chnex *chnex;
  Devex *devex;
  Iopex *iopex, **liopex;
  uLong dv, sts;

  chnex = chnexv;
  devex = devexv;
  iopex = iopexv;

  iopex -> ioop     = ioop;
  iopex -> next     = NULL;
  iopex -> procmode = procmode;
  iopex -> devex    = devex;

  switch (funcode) {

    /* Open - associates a protocol number with a channel and starts reception */

    case OZ_IO_ETHER_OPEN: {
      uLong rcr;

      /* Make sure not already open */

      dv = oz_hw_smplock_wait (devex -> smplock);
      if (chnex -> rxreqt != NULL) {
        oz_hw_smplock_clr (devex -> smplock, dv);
        return (OZ_FILEALREADYOPEN);
      }

      /* Copy the arg list to chnex for future reference */

      movc4 (as, ap, sizeof chnex -> ether_open, &(chnex -> ether_open));

      /* Put channel on list of open channels - the interrupt routine will now see it */

      if (chnex -> rxreqh != NULL) oz_crash ("oz_dev_rtl8139: chnex rxreq was not empty on open (%p)", chnex -> rxreqh);
      chnex -> rxreqt = &(chnex -> rxreqh);
      chnex -> next   = devex -> chnexs;
      devex -> chnexs = chnex;
      if (chnex -> ether_open.promis) {
        devex -> promiscuous ++;
        rcr = ether_inl (devex -> iobase, IO_L_RCR);
        if ((rcr & 0xF) != 0xF) {
          oz_knl_printk ("oz_dev_rtl8139: %s enabling promiscuous mode\n", devex -> name);
          ether_outl (rcr | 0xF, devex -> iobase, IO_L_RCR);
        }
      }
      oz_hw_smplock_clr (devex -> smplock, dv);
      return (OZ_SUCCESS);
    }

    /* Disassociates a protocol with a channel and stops reception */

    case OZ_IO_ETHER_CLOSE: {
      return (close_channel (devex, chnex) ? OZ_SUCCESS : OZ_FILENOTOPEN);
    }

    /* Receive a message */

    case OZ_IO_ETHER_RECEIVE: {
      Rxbuf *rxbuf;

      /* Get parameter block into iopex for future reference */

      movc4 (as, ap, sizeof iopex -> ether_receive, &(iopex -> ether_receive));

      /* If any of the rcv... parameters are filled in, it must be called from kernel mode */

      if ((iopex -> ether_receive.rcvfre   != NULL) 
       || (iopex -> ether_receive.rcvdrv_r != NULL) 
       || (iopex -> ether_receive.rcveth_r != NULL)) {
        if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);
      }

      /* Set up the request parameters and queue request so the interrupt routine can fill the buffer with an incoming message */

      rxbuf = iopex -> ether_receive.rcvfre;		/* maybe caller supplied the kernel-mode buffer */
      if (rxbuf == NULL) {
        rxbuf = OZ_KNL_NPPMALLOQ (sizeof *rxbuf);	/* if not, allocate a new one */
        if (rxbuf == NULL) return (OZ_EXQUOTANPP);
      }
      iopex -> rxbuf = rxbuf;				/* either way, save pointer to kernel-mode buffer */

      dv = oz_hw_smplock_wait (devex -> smplock);	/* lock database */
      liopex = chnex -> rxreqt;
      if (liopex == NULL) {				/* make sure channel is still open */
        oz_hw_smplock_clr (devex -> smplock, dv);
        OZ_KNL_NPPFREE (rxbuf);
        return (OZ_FILENOTOPEN);
      }
      *liopex = iopex;					/* put reqeuest on end of queue - interrupt routine can now see it */
      chnex -> rxreqt = &(iopex -> next);
      oz_hw_smplock_clr (devex -> smplock, dv);		/* unlock database */
      return (OZ_STARTED);
    }

    /* Free a receive buffer */

    case OZ_IO_ETHER_RECEIVEFREE: {
      OZ_IO_ether_receivefree ether_receivefree;

      if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);			/* can only be called from kernel mode */
      movc4 (as, ap, sizeof ether_receivefree, &ether_receivefree);		/* get the parameters */
      OZ_KNL_NPPFREE (ether_receivefree.rcvfre);				/* free off the given buffer */
      return (OZ_SUCCESS);
    }

    /* Allocate a send buffer */

    case OZ_IO_ETHER_TRANSMITALLOC: {
      OZ_IO_ether_transmitalloc ether_transmitalloc;
      Txbuf *txbuf;

      if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);					/* can only be called from kernel mode */
      movc4 (as, ap, sizeof ether_transmitalloc, &ether_transmitalloc);				/* get the parameters */
      txbuf = OZ_KNL_PCMALLOQ (sizeof *txbuf);							/* allocate a transmit buffer */
      if (txbuf == NULL) return (OZ_EXQUOTANPP);
      if (ether_transmitalloc.xmtsiz_r != NULL) *(ether_transmitalloc.xmtsiz_r) = DATASIZE;	/* this is size of data it can handle */
      if (ether_transmitalloc.xmtdrv_r != NULL) *(ether_transmitalloc.xmtdrv_r) = txbuf;	/* this is the pointer we want returned in ether_transmit.xmtdrv */
      if (ether_transmitalloc.xmteth_r != NULL) *(ether_transmitalloc.xmteth_r) = (void *)txbuf; /* this is where they put the ethernet packet to be transmitted */
      return (OZ_SUCCESS);
    }

    /* Transmit a message */

    case OZ_IO_ETHER_TRANSMIT: {
      Txbuf *txbuf;

      /* Get parameter block into iopex for future reference */

      movc4 (as, ap, sizeof iopex -> ether_transmit, &(iopex -> ether_transmit));

      /* Make sure buffer not too big */

      if (iopex -> ether_transmit.size > sizeof *txbuf) return (OZ_BUFFEROVF);
      if (iopex -> ether_transmit.dlen > DATASIZE) return (OZ_BUFFEROVF);

      /* If any xmt... params given, caller must be in kernel mode */

      if ((iopex -> ether_transmit.xmtdrv   != NULL) 
       || (iopex -> ether_transmit.xmtsiz_r != NULL) 
       || (iopex -> ether_transmit.xmtdrv_r != NULL) 
       || (iopex -> ether_transmit.xmteth_r != NULL)) {
        if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);
      }

      /* If caller supplied kernel-mode physically contiguous txbuf, use it, otherwise allocate one */

      txbuf = iopex -> ether_transmit.xmtdrv;
      if (txbuf == NULL) {
        if (iopex -> ether_transmit.dlen + 14 > iopex -> ether_transmit.size) return (OZ_UNALIGNEDBUFF);
        txbuf = OZ_KNL_PCMALLOQ (sizeof *txbuf);
        if (txbuf == NULL) return (OZ_EXQUOTANPP);
      }

      /* Get buffer's physical address */

      sts = oz_knl_misc_sva2pa (txbuf, &(iopex -> txbufpp), &(iopex -> txbufof));
      if (sts < sizeof *txbuf) oz_crash ("oz_dev_rtl8139 transmit: txbuf %p not physically contiguous", txbuf);

      /* Copy in any supplied data */

      if (iopex -> ether_transmit.buff != NULL) {
        sts = oz_knl_section_uget (procmode, iopex -> ether_transmit.size, iopex -> ether_transmit.buff, txbuf);
        if (sts != OZ_SUCCESS) {
          OZ_KNL_NPPFREE (txbuf);
          return (sts);
        }
      }

      /* Queue it for processing */

      iopex -> txbuf = txbuf;

      dv = oz_hw_smplock_wait (devex -> smplock);	/* lock database */

      *(devex -> txreqt) = iopex;			/* queue the request */
      devex -> txreqt = &(iopex -> next);

      if (devex -> txfree != 0) {			/* see if any free slots available */
        transmit_start (devex);				/* if so, start transmitting */
      }

      oz_hw_smplock_clr (devex -> smplock, dv);		/* unlock database */

      return (OZ_STARTED);
    }

    /* Get info - part 1 */

    case OZ_IO_ETHER_GETINFO1: {
      OZ_IO_ether_getinfo1 ether_getinfo1;

      movc4 (as, ap, sizeof ether_getinfo1, &ether_getinfo1);
      if (ether_getinfo1.enaddrbuff != NULL) {
        if (ether_getinfo1.enaddrsize > ADDRSIZE) ether_getinfo1.enaddrsize = ADDRSIZE;
        sts = oz_knl_section_uput (procmode, ether_getinfo1.enaddrsize, devex -> enaddr, ether_getinfo1.enaddrbuff);
        if (sts != OZ_SUCCESS) return (sts);
      }
      ether_getinfo1.datasize   = DATASIZE;					// max length of data portion of message
      ether_getinfo1.buffsize   = BUFFSIZE;					// max length of whole message (header, data, crc)
      ether_getinfo1.dstaddrof  = 0;						// offset of dest address in packet
      ether_getinfo1.srcaddrof  = 0 + ADDRSIZE;					// offset of source address in packet
      ether_getinfo1.protooffs  = 0 + 2 * ADDRSIZE;				// offset of protocol in packet
      ether_getinfo1.dataoffset = 0 + 2 * ADDRSIZE + PROTOSIZE;			// offset of data in packet
      ether_getinfo1.arphwtype  = ARPHWTYPE;					// ARP hardware type
      ether_getinfo1.addrsize   = ADDRSIZE;					// size of each address field
      ether_getinfo1.protosize  = PROTOSIZE;					// size of protocol field
      ether_getinfo1.rcvmissed  = chnex -> rcvmissed;				// number of receives that missed a request
      if (as > sizeof ether_getinfo1) as = sizeof ether_getinfo1;
      sts = oz_knl_section_uput (procmode, as, &ether_getinfo1, ap);
      return (sts);
    }
  }

  return (OZ_BADIOFUNC);
}

/************************************************************************/
/*									*/
/*  Enable the device							*/
/*									*/
/************************************************************************/

static uLong ether_enable (Devex *devex)

{
  uByte *physaddr, *virtaddr;
  OZ_Ieeedma32 *ieeedma32;
  OZ_Mempage ipage, npages, phypages, sysvpage;
  Txbuf *txbuf;
  uLong dv, i, pm, sts;
  void *sysvaddr;

  /* Software reset the chip.  Chip clears the CmdReset bit when reset is complete. */

  ether_outb (CmdReset, devex -> iobase, IO_B_CR);

  /* Allocate contiguous physical memory for receive buffers and map it to contiguous virtual addresses */
  /* Well, at least we don't have to PIO the data out of the stupid thing                               */

#if RX_RING_TOTAL % (1 << OZ_HW_L2PAGESIZE)
  error : code assumes RX RING TOTAL is a multiple of page size because first phypage is mapped on the beginning and on the end
#endif
#if BUFFSIZE > (1 << OZ_HW_L2PAGESIZE)
  error : code assumes BUFF SIZE is smaller than a page because we only double-map the first phypage
#endif

  npages = RX_RING_TOTAL >> OZ_HW_L2PAGESIZE;						/* number of pages those bytes take */
  sts = oz_knl_spte_alloc (npages + 1, &sysvaddr, &sysvpage, NULL);			/* allocate some contiguous sptes to map the pages with */
  if (sts != OZ_SUCCESS) {								/* one extra spte so we can double-map the wrapped page */
    oz_knl_printk ("oz_dev_rtl8139: %s error %u allocating %u sptes\n", devex -> name, sts, npages + 1);
    return (sts);
  }

  pm = oz_hw_smplock_wait (&oz_s_smplock_pm);						/* allocate some contiguous physical pages */
  phypages = oz_knl_phymem_allocontig (npages, OZ_PHYMEM_PAGESTATE_ALLOCSECT, sysvpage);
  oz_hw_smplock_clr (&oz_s_smplock_pm, pm);
  if (phypages == OZ_PHYPAGE_NULL) {
    oz_knl_printk ("oz_dev_rtl8193: %s unable to allocate %u physical pages\n", devex -> name, npages);
    oz_knl_spte_free (npages + 1, sysvpage);
    return (OZ_NOMEMORY);
  }

  for (ipage = 0; ipage < npages; ipage ++) {						/* map the physical pages to system virtual memory */
    oz_hw_pte_writeall (sysvpage + ipage, 
                        OZ_SECTION_PAGESTATE_VALID_W, 
                        phypages + ipage, 
                        OZ_HW_PAGEPROT_KW, 
                        OZ_HW_PAGEPROT_NA);
  }
  oz_hw_pte_writeall (sysvpage + ipage, 						/* map first page on end of last page */
                      OZ_SECTION_PAGESTATE_VALID_W, 					/* so we don't have to do wrap to copy */
                      phypages, 
                      OZ_HW_PAGEPROT_KW, 
                      OZ_HW_PAGEPROT_NA);

  devex -> npages  = npages;								// remember how many pages we allocated
  devex -> phypage = phypages;								// remember the starting physical page number
  devex -> syspage = sysvpage;								// remember the starting virtual page number

  /* Get PCI and virtual starting addresses */

  if (oz_dev_pci_dma32map_start (devex -> rcvpcidma, 0, npages << OZ_HW_L2PAGESIZE, &phypages, 0, &ieeedma32, NULL) < 0) {
    oz_crash ("oz_dev_rtl8139 ether_enable: error mapping %u page receive buffer for DMA");
  }
  if (ieeedma32[0].bytecnt != (npages << OZ_HW_L2PAGESIZE)) {
    oz_crash ("oz_dev_rtl8139 ether_enable: bad dma bytcount %u", ieeedma32[0].bytecnt);
  }
  devex -> rxring_pciad = ieeedma32[0].phyaddr;
  devex -> rxring_vaddr = OZ_HW_VPAGETOVADDR (sysvpage);

  oz_knl_printk ("oz_dev_rtl8139: %s rcv ring pci addr %X, virt addr %p\n", 
	devex -> name, devex -> rxring_pciad, devex -> rxring_vaddr);

  /* Fill in other devex stuff */

  devex -> chnexs      = NULL;						// no channels assigned to it yet
  devex -> rxring_offs = 0;						// first rcv packet comes in at beg of ring buffer

  devex -> txreqh      = NULL;						// no transmit requests pending
  devex -> txreqt      = &(devex -> txreqh);
  devex -> txfree      = NUM_TX_DESC;					// chip has 4 available transmit slots
  devex -> txnexti     = 0;						// start scanning at slot 0
  for (i = 0; i < NUM_TX_DESC; i ++) devex -> txreqip[i] = NULL;	// all slots are empty

  /* Turn on hardware */

  rtl_init (devex);
  dv = oz_hw_smplock_wait (devex -> smplock);				// inhibit interrupt delivery
  i = rtl_reset (devex);						// turn the chip on
  oz_hw_smplock_clr (devex -> smplock, dv);				// enable interrupt delivery
  return (i ? OZ_SUCCESS : OZ_IOFAILED);
}

/************************************************************************/
/*									*/
/*  Close an open channel						*/
/*									*/
/*    Input:								*/
/*									*/
/*	chnexx   = channel to be closed					*/
/*	smplevel = softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	close_channel = 0 : channel wasn't open				*/
/*	                1 : channel was open, now closed		*/
/*									*/
/************************************************************************/

static int close_channel (Devex *devex, Chnex *chnexx)

{
  Chnex *chnex, **lchnex;
  Iopex *iopex;
  uLong dv, rcr;

  dv = oz_hw_smplock_wait (devex -> smplock);

  /* Remove from list of open channels - this stops the interrupt routine from processing requests */

  for (lchnex = &(devex -> chnexs); (chnex = *lchnex) != chnexx; lchnex = &(chnex -> next)) {
    if (chnex == NULL) {
      oz_hw_smplock_clr (devex -> smplock, dv);
      return (0);
    }
  }
  *lchnex = chnex -> next;

  /* Decrement promiscuous count */

  if (chnex -> ether_open.promis && (-- (devex -> promiscuous) == 0)) {
    rcr = ether_inl (devex -> iobase, IO_L_RCR);
    if ((rcr & 0xF) != 0xA) {
      oz_knl_printk ("oz_dev_rtl8139: %s disabling promiscuous mode\n", devex -> name);
      rcr &= ~0xF;
      ether_outl (rcr | 0xA, devex -> iobase, IO_L_RCR);
    }
    chnex -> ether_open.promis = 0;
  }

  /* Abort all pending receive requests and don't let any more queue */

  chnex -> rxreqt = NULL;					/* block any more receive requests from queueing */
  while ((iopex = chnex -> rxreqh) != NULL) {			/* abort any receive requests we may have */
    chnex -> rxreqh = iopex -> next;
    oz_knl_iodonehi (iopex -> ioop, OZ_ABORTED, NULL, NULL, NULL);
  }
  oz_hw_smplock_clr (devex -> smplock, dv);
  return (1);
}

/************************************************************************/
/*									*/
/*  Start transmitting next packet if possible				*/
/*									*/
/*    Input:								*/
/*									*/
/*	devex    = device to start transmitting on			*/
/*	smplevel = devex -> smplock (interrupt level)			*/
/*									*/
/*    Output:								*/
/*									*/
/*	any requests on devexv -> txreqs processed			*/
/*									*/
/************************************************************************/

static void transmit_start (Devex *devex)

{
  Iopex *txreq;
  OZ_Ieeedma32 *ieeedma32;
  Txbuf *txbuf;
  uLong i, size;

  while (devex -> txfree != 0) {							/* repeat while transmitter can do more */
    txreq = devex -> txreqh;								/* see if there are any waiting for free transmit buffs */
    if (txreq == NULL) break;								/* if we don't have both, all done */
    devex -> txreqh = txreq -> next;							/* we have both, unlink the top request */
    if (devex -> txreqh == NULL) devex -> txreqt = &(devex -> txreqh);
    for (i = devex -> txnexti; devex -> txreqip[i] != NULL;) {				/* look for a free slot */
      if (++ i == NUM_TX_DESC) i = 0;
      if (i == devex -> txnexti) oz_crash ("oz_dev_rtl8139 transmit_start: bad txfree count %u", devex -> txfree);
    }
    devex -> txreqip[i] = txreq;							/* mark slot no inter free */
    devex -> txfree --;									/* one less slot free */
    size  = txreq -> ether_transmit.dlen;						/* get size of user's data */
    size += 14;										/* add on dstaddr, srcaddr and proto, but not crc */
    if (size > TX_DATA_SIZE) size = TX_DATA_SIZE;					/* must transmit at most this many bytes */
    if (size < ETH_ZLEN) size = ETH_ZLEN;						/* must transmit at least this many bytes */
    if (oz_dev_pci_dma32map_start (devex -> xmtpcidma[i], 1, size, &(txreq -> txbufpp), txreq -> txbufof, &ieeedma32, NULL) < 0) { /* map it */
      oz_crash ("oz_dev_rtl8139 transmit_start: error mapping %u byte buffer for DMA", size);
    }
    if (ieeedma32[0].bytecnt != size) {
      oz_crash ("oz_dev_rtl8139 transmit_start: only mapped %u bytes of %u", ieeedma32[0].bytecnt, size);
    }
    ether_outl (ieeedma32[0].phyaddr, devex -> iobase, IO_L_TSAD0 + (i * 4));		/* start transmitting */
    ether_outl (((TX_FIFO_THRESH << 11) & 0x3F0000) + size, devex -> iobase, IO_L_TSD0 + (i * 4));
    devex -> txnexti = (++ i) % NUM_TX_DESC;						/* set up which one to use next time */
  }
}

/* Contributed by Tim Robinson */

/*--beg of OZONE added stuff--*/

#define in(port) oz_dev_pci_inb (port - 1)
#define in16(port) oz_dev_pci_inw (port - 1)
#define in32(port) oz_dev_pci_inl (port - 1)
#define out(port,data) oz_dev_pci_outb (data, port - 1)
#define out16(port,data) oz_dev_pci_outw (data, port - 1)
#define out32(port,data) oz_dev_pci_outl (data, port - 1)

/*--end of OZONE added stuff--*/

/* Symbolic offsets to registers. */
enum RTL8139_registers
{
    MAC0=0,             /* Ethernet hardware address. */
    MAR0=8,             /* Multicast filter. */
    TxStatus0=0x10,     /* Transmit status (four 32bit registers). */
    TxAddr0=0x20,       /* Tx descriptors (also four 32bit). */
    RxBuf=0x30, RxEarlyCnt=0x34, RxEarlyStatus=0x36,
    ChipCmd=0x37, RxBufPtr=0x38, RxBufAddr=0x3A,
    IntrMask=0x3C, IntrStatus=0x3E,
    TxConfig=0x40, RxConfig=0x44,
    Timer=0x48,         /* general-purpose counter. */
    RxMissed=0x4C,      /* 24 bits valid, write clears. */
    Cfg9346=0x50, Config0=0x51, Config1=0x52,
    TimerIntrReg=0x54,  /* intr if gp counter reaches this value */
    MediaStatus=0x58,
    Config3=0x59,
    MultiIntr=0x5C,
    RevisionID=0x5E,    /* revision of the RTL8139 chip */
    TxSummary=0x60,
    MII_BMCR=0x62, MII_BMSR=0x64, NWayAdvert=0x66, NWayLPAR=0x68,
    NWayExpansion=0x6A,
    DisconnectCnt=0x6C, FalseCarrierCnt=0x6E,
    NWayTestReg=0x70,
    RxCnt=0x72,         /* packet received counter */
    CSCR=0x74,          /* chip status and configuration register */
    PhyParm1=0x78,TwisterParm=0x7c,PhyParm2=0x80,   /* undocumented */
    /* from 0x84 onwards are a number of power management/wakeup frame
     * definitions we will probably never need to know about.  */
};



enum MediaStatusBits
{
    MSRTxFlowEnable=0x80, MSRRxFlowEnable=0x40, MSRSpeed10=0x08,
    MSRLinkFail=0x04, MSRRxPauseFlag=0x02, MSRTxPauseFlag=0x01,
};

enum MIIBMCRBits
{
    BMCRReset=0x8000, BMCRSpeed100=0x2000, BMCRNWayEnable=0x1000,
    BMCRRestartNWay=0x0200, BMCRDuplex=0x0100,
};

enum CSCRBits
{
    CSCR_LinkOKBit=0x0400, CSCR_LinkChangeBit=0x0800,
    CSCR_LinkStatusBits=0x0f000, CSCR_LinkDownOffCmd=0x003c0,
    CSCR_LinkDownCmd=0x0f3c0,
};

/************************************************************************/
/*									*/
/*  Boot-time initialization routine - read EEPROM and put chip in 	*/
/*  running state							*/
/*									*/
/************************************************************************/

static void rtl_init (Devex *rtl)

{
  unsigned i, sl;

  /* Bring the chip out of low-power mode */

  out (rtl -> iobase + Config1, 0x00);

  /* Get ethernet address from eeprom */

  if (rtl_eeprom (rtl, 0) != 0xFFFF) {
    unsigned short *ap;

    ap = (unsigned short *)(rtl -> enaddr);
    for (i = 0; i < 3; i++) *(ap ++) = rtl_eeprom (rtl, i + 7);
  } else {
    unsigned char *ap;

    ap = (unsigned char *)(rtl -> enaddr);
    for (i = 0; i < 6; i++) *(ap ++) = in (rtl -> iobase + MAC0 + i);
  }

  oz_knl_printk ("oz_dev_rtl8139: %s link status:\n", rtl -> name);
  linkstatuschange (rtl, 1);
}


/* Serial EEPROM section. */

/*  EEPROM_Ctrl bits. */
#define EE_SHIFT_CLK    0x04    /* EEPROM shift clock. */
#define EE_CS           0x08    /* EEPROM chip select. */
#define EE_DATA_WRITE   0x02    /* EEPROM chip data in. */
#define EE_WRITE_0      0x00
#define EE_WRITE_1      0x02
#define EE_DATA_READ    0x01    /* EEPROM chip data out. */
#define EE_ENB          (0x80 | EE_CS)

/*
    Delay between EEPROM clock transitions.
    No extra delay is needed with 33Mhz PCI, but 66Mhz may change this.
*/

#define eeprom_delay()  in32(ee_addr)

/* The EEPROM commands include the alway-set leading bit. */
#define EE_WRITE_CMD    (5 << 6)
#define EE_READ_CMD     (6 << 6)
#define EE_ERASE_CMD    (7 << 6)

static unsigned rtl_eeprom(Devex *rtl, unsigned location)

{
    int i;
    unsigned int retval = 0;
    long ee_addr = rtl -> iobase + Cfg9346;
    int read_cmd = location | EE_READ_CMD;

    out(ee_addr, EE_ENB & ~EE_CS);
    out(ee_addr, EE_ENB);

    /* Shift the read command bits out. */
    for (i = 10; i >= 0; i--)
    {
        int dataval = (read_cmd & (1 << i)) ? EE_DATA_WRITE : 0;
        out(ee_addr, EE_ENB | dataval);
        eeprom_delay();
        out(ee_addr, EE_ENB | dataval | EE_SHIFT_CLK);
        eeprom_delay();
    }

    out(ee_addr, EE_ENB);
    eeprom_delay();

    for (i = 16; i > 0; i--)
    {
        out(ee_addr, EE_ENB | EE_SHIFT_CLK);
        eeprom_delay();
        retval = (retval << 1) | ((in(ee_addr) & EE_DATA_READ) ? 1 : 0);
        out(ee_addr, EE_ENB);
        eeprom_delay();
    }

    /* Terminate the EEPROM access. */
    out(ee_addr, ~EE_CS);
    return retval;
}

/************************************************************************/
/*									*/
/*  Interrupt service routine						*/
/*									*/
/************************************************************************/

#define IntMask (LinkChg | TxOK | TxErr | RxOK | RxErr | RxOverflow | RxFIFOOver)

static void rtl_interrupt (void *rtlv, OZ_Mchargs *mchargs)

{
  Devex *rtl;
  uWord status;

  rtl = rtlv;

  while ((status = ether_inw (rtl -> iobase, IO_W_ISR) & IntMask) != 0) {
    if (status & LinkChg) {
      oz_knl_printk ("oz_dev_rtl8139: %s link status change:\n", rtl -> name);
      linkstatuschange (rtl, 0);
      ether_outw (LinkChg, rtl -> iobase, IO_W_ISR);
    }
    if (status & TxOK) {
      transmit_done (rtl);
      ether_outw (TxOK, rtl -> iobase, IO_W_ISR);
    }
    if (status & TxErr) {
      oz_knl_printk ("oz_dev_rtl8139: %s transmit error\n", rtl -> name);
      ether_outl (TCR_CLRABT, rtl -> iobase, IO_L_TCR);
      ether_outw (TxErr, rtl -> iobase, IO_W_ISR);
    }
    if ((status & (RxOK | RxErr | RxOverflow | RxFIFOOver)) != 0) {
      if (status & RxOK) receive_done (rtl);
      if (status & RxErr) oz_knl_printk ("oz_dev_rtl8139: %s receive error\n", rtl -> name);
      if (status & RxOverflow) oz_knl_printk ("oz_dev_rtl8139: %s receive ring overflow\n", rtl -> name);
      if (status & RxFIFOOver) oz_knl_printk ("oz_dev_rtl8139: %s receive fifo overflow\n", rtl -> name);
      ether_outw (status & (RxOK | RxErr | RxOverflow | RxFIFOOver), rtl -> iobase, IO_W_ISR);
    }
  }
}

/************************************************************************/
/*									*/
/*  Process received messages						*/
/*									*/
/************************************************************************/

static void receive_done (Devex *rtl)

{
  uLong ring_offs, rx_size, rx_status;

  ring_offs  = rtl -> rxring_offs;					// get offset of message in ring buffer

  while (!(ether_inb (rtl -> iobase, IO_B_CR) & 1)) {			// see if a message is ready to process
    OZ_HW_MB;
    rx_status  = *(uLong *) (rtl -> rxring_vaddr + ring_offs);		// ok, get the status/size
    rx_size    = rx_status >> 16;					// split out the size

    if ((rx_status & (RxBadSymbol | RxRunt | RxTooLong | RxCRCErr | RxBadAlign)) ||
        (rx_size < ETH_ZLEN) || (rx_size > BUFFSIZE + 4)) {		// check for nasty error
      oz_knl_printk ("oz_dev_rtl8139: %s rx error 0x%X, len %u\n", rtl -> name, rx_status & 0xFFFF, rx_size);
      rtl_reset (rtl);							// reset chip
      return;
    }

    /* Received a good packet */

    message_received (rtl, rx_size - 4, rtl -> rxring_vaddr + ring_offs + 4);

    /* Remove message from ring buffer */

    ring_offs  = (ring_offs + rx_size + 4 + 3) & ~3;			// skip 8139's header then longword align
    out16 (rtl -> iobase + RxBufPtr, ring_offs - 16);			// tell chip we processed message
    ring_offs %= RX_RING_TOTAL;						// wrap it for next packet
  }

  rtl -> rxring_offs = ring_offs;					// remember offset for next time
}

/************************************************************************/
/*									*/
/*  This routine is called by the interrupt service routine when a 	*/
/*  message has been received						*/
/*									*/
/*    Input:								*/
/*									*/
/*	devex    = device the message was received on			*/
/*	length   = length of message					*/
/*	ringdata = pointer to message					*/
/*	smplevel = device interrupt level				*/
/*									*/
/************************************************************************/

static void message_received (Devex *devex, uLong length, uByte *ringdata)

{
  Chnex *chnex;
  Iopex *iopex;
  Rxbuf *rxbuf;
  uByte *dstad;
  uWord chproto, proto;

  dstad = ringdata + 0;							/* point to destination ethernet address */
  proto = *(uWord *)(ringdata + 12);					/* get message's ethernet protocol number */
  for (chnex = devex -> chnexs; chnex != NULL; chnex = chnex -> next) {	/* scan the open channels */
    chproto = *(uWord *)(chnex -> ether_open.proto);			/* get protocol open on the channel */
    if ((chproto != 0) && (chproto != proto)) continue;			/* find one with matching protocol number */
    if (!(chnex -> ether_open.promis) && !CEQENADDR (dstad, devex -> enaddr) && !CEQENADDR (dstad, broadcast)) continue; /* matching enaddr */
    iopex = chnex -> rxreqh;						/* see if it has a receive buffer available */
    if (iopex == NULL) {
      chnex -> rcvmissed ++;
      continue;								/* if not, skip it */
    }
    rxbuf = iopex -> rxbuf;						/* ok, get receive buffer pointer */
    rxbuf -> dlen = length - 14;					/* save length of data (no header or crc) */
    memcpy (&(rxbuf -> buf), ringdata, length);				/* copy to kernel buffer */
									/* first page of ring buffer is double-mapped on */
									/* the end so we don't have to do wrapped copy */
    chnex -> rxreqh = iopex -> next;					/* unlink request from pending list */
    if (chnex -> rxreqh == NULL) chnex -> rxreqt = &(chnex -> rxreqh);
    oz_knl_iodonehi (iopex -> ioop, OZ_SUCCESS, NULL, receive_finish, iopex); /* finish it off */
  }
}

/************************************************************************/
/*									*/
/*  This routine is called at softint level in requestor's address 	*/
/*  space when a buffer has been received on the ethernet		*/
/*									*/
/*    Input:								*/
/*									*/
/*	iopex   = request that message was received for			*/
/*	smplock = softint						*/
/*									*/
/************************************************************************/

static void receive_finish (void *iopexv, int finok, uLong *status_r)

{
  Iopex *iopex;
  Rxbuf *rxbuf;
  uLong size, sts;

  iopex = iopexv;
  rxbuf = iopex -> rxbuf;
  if (finok && (*status_r == OZ_SUCCESS)) {

    /* Maybe copy packet from kernel buffer to user buffer */

    if (iopex -> ether_receive.buff != NULL) {
      size = rxbuf -> buf.data + rxbuf -> dlen - (uByte *)&(rxbuf -> buf);
      if (size > iopex -> ether_receive.size) size = iopex -> ether_receive.size;
      sts = oz_knl_section_uput (iopex -> procmode, size, &(rxbuf -> buf), iopex -> ether_receive.buff);
      if (*status_r == OZ_SUCCESS) *status_r = sts;
    }

    /* Maybe return length of data in packet (not incl header or crc) */

    if (iopex -> ether_receive.dlen != NULL) {
      sts = oz_knl_section_uput (iopex -> procmode, sizeof *(iopex -> ether_receive.dlen), &(rxbuf -> dlen), iopex -> ether_receive.dlen);
      if (*status_r == OZ_SUCCESS) *status_r = sts;
    }

    /* Maybe return pointer to kernel packet */

    if (iopex -> ether_receive.rcveth_r != NULL) *(iopex -> ether_receive.rcveth_r) = (uByte *)&(rxbuf -> buf);

    /* Maybe return pointer to temp kernel buffer so caller can re-use it */

    if (iopex -> ether_receive.rcvdrv_r != NULL) {
      *(iopex -> ether_receive.rcvdrv_r) = rxbuf;
      rxbuf = NULL;
    }
  }

  /* If we didn't pass kernel buffer back to caller for re-use, free it off */

  if (rxbuf != NULL) OZ_KNL_NPPFREE (rxbuf);
}

/************************************************************************/
/*									*/
/*  This routine is called at interrupt level when a transmission has 	*/
/*  completed								*/
/*									*/
/************************************************************************/

static void transmit_done (Devex *devex)

{
  Iopex *iopex;
  uLong i, sts, tsd;

  /* Check all transmitters that are marked busy */

  if (devex -> txfree < NUM_TX_DESC) {				/* see if anything is busy */
    for (i = 0; i < NUM_TX_DESC; i ++) {			/* loop through each transmit slot */
      iopex = devex -> txreqip[i];				/* see if slot is bussy */
      if (iopex != NULL) {
        tsd = ether_inl (devex -> iobase, IO_L_TSD0 + (i * 4)); /* ok, read the transmit status */
        if (tsd & (TxStatOK | TxUnderrun | TxAborted | TxOutOfWindow | TxCarrierLost)) {	/* see if it is done */
          sts = OZ_SUCCESS;					/* if so, check status */
          if (!(tsd & TxHostOwns)) {
            oz_knl_printk ("oz_dev_rtl8139 %s transmit_done: buffer %u done but not free\n", devex -> name, i);
            rtl_reset (devex);					/* reset chip and abort transmits */
            break;
          }
          if (!(tsd & TxStatOK)) {
            oz_knl_printk ("oz_dev_rtl8139: %s transmit error 0x%X\n", devex -> name, tsd);
            sts = OZ_IOFAILED;
          }
          oz_dev_pci_dma32map_stop (devex -> xmtpcidma[i]);	/* dma is finished */
          oz_knl_iodonehi (iopex -> ioop, sts, NULL, transmit_finish, iopex); /* transmit is now complete */
          devex -> txreqip[i] = NULL;				/* say slot is available for use */
          devex -> txfree ++;					/* one more free slot */
        }
      }
    }
  }

  /* If there are transmit requests waiting for free slot, start them */

  if ((devex -> txfree != 0) && (devex -> txreqh != NULL)) {	/* see if any free slots and anyone waiting for one */
    transmit_start (devex);					/* if so, start a new transmit */
  }
}

/************************************************************************/
/*									*/
/*  This routine is called at softint level in requestor's address 	*/
/*  space when a buffer has been transmitted on the ethernet		*/
/*									*/
/*    Input:								*/
/*									*/
/*	iopex   = transmit request that has completed			*/
/*	smplock = softint						*/
/*									*/
/************************************************************************/

static void transmit_finish (void *iopexv, int finok, uLong *status_r)

{
  Iopex *iopex;
  Txbuf *txbuf;

  iopex = iopexv;
  txbuf = iopex -> txbuf;

  if (!finok || (*status_r != OZ_SUCCESS) || (iopex -> ether_transmit.xmtdrv_r == NULL)) OZ_KNL_NPPFREE (txbuf);
  else {
    *(iopex -> ether_transmit.xmtdrv_r) = txbuf;							/* this is the pointer we want returned in ether_transmit.xmtdrv */
    if (iopex -> ether_transmit.xmtsiz_r != NULL) *(iopex -> ether_transmit.xmtsiz_r) = DATASIZE;	/* this is size of data it can handle */
    if (iopex -> ether_transmit.xmteth_r != NULL) *(iopex -> ether_transmit.xmteth_r) = (void *)txbuf;	/* this is where they put the ethernet packet to be transmitted */
  }
}

/************************************************************************/
/*									*/
/*  Reset chip then initialize it to running state			*/
/*									*/
/*    Input:								*/
/*									*/
/*	smplevel = device smplevel (interrupt level)			*/
/*									*/
/*    Output:								*/
/*									*/
/*	rtl_reset = 0 : failure						*/
/*	            1 : success						*/
/*									*/
/************************************************************************/

static int rtl_reset (Devex *rtl)

{
  int i;

  /* Start software reset going on chip */

  out (rtl -> iobase + ChipCmd, CmdReset);

  /* Reset receive ring pointers and abort any transmits that were in progress */

  rtl -> rxring_offs = 0;				// reset next received message offset in ring buffer
  rtl -> txfree      = NUM_TX_DESC;			// all transmit slots shall soon be available
  rtl -> txnexti     = 0;				// next transmit goes to slot 0
  for (i = 0; i < NUM_TX_DESC; i ++) {			// scan through transmit slots
    if (rtl -> txreqip[i] != NULL) {			// see if request was in progress
      oz_dev_pci_dma32map_stop (rtl -> xmtpcidma[i]);	// if so, dma is finished
      oz_knl_iodonehi (rtl -> txreqip[i] -> ioop, OZ_ABORTED, NULL, NULL, NULL); // abort request
      rtl -> txreqip[i] = NULL;				// ... and mark the slot available
    }
  }

  /* Give the chip up to 10ms to finish the reset. */

  if (!oz_hw_stl_microwait (10000, rtl_reset_done, rtl)) return (0);

  /* Set up the ethernet address */

  for (i = 0; i < sizeof rtl -> enaddr; i++) {
    out (rtl -> iobase + MAC0 + i, rtl -> enaddr[i]);
  }

  /* Must enable Tx/Rx before setting transfer thresholds! */

  out (rtl -> iobase + ChipCmd, CmdRxEnb | CmdTxEnb);
  out32 (rtl -> iobase + RxConfig, (RX_FIFO_THRESH<<13) | (RX_RING_TOTAL_IDX<<11) | (RX_DMA_BURST<<8)); /* accept no frames yet!  */
  out32 (rtl -> iobase + TxConfig, (TX_DMA_BURST<<8)|0x03000000);

  /* The Linux driver changes Config1 here to use a different LED pattern
   * for half duplex or full/autodetect duplex (for full/autodetect, the
   * outputs are TX/RX, Link10/100, FULL, while for half duplex it uses
   * TX/RX, Link100, Link10).  This is messy, because it doesn't match
   * the inscription on the mounting bracket.  It should not be changed
   * from the configuration EEPROM default, because the card manufacturer
   * should have set that to match the card.  */

  out32 (rtl -> iobase + RxBuf, rtl -> rxring_pciad);

  /* Start the chip's Tx and Rx process. */

  out32 (rtl -> iobase + RxMissed, 0);

  /* Set rx_mode */

  out (rtl -> iobase + RxConfig, AcceptBroadcast|AcceptMyPhys);

  /* If we add multicast support, the MAR0 register would have to be
   * initialized to 0xffffffffffffffff (two 32 bit accesses).  Etherboot
   * only needs broadcast (for ARP/RARP/BOOTP/DHCP) and unicast.  */

  out (rtl -> iobase + ChipCmd, CmdRxEnb | CmdTxEnb);

  /* Enable the interrupts we want */

  out16 (rtl -> iobase + IntrMask, IntMask);

  return (1);
}

static uLong rtl_reset_done (void *rtlv)

{
  return ((in (((Devex *)rtlv) -> iobase + ChipCmd) & CmdReset) == 0);
}

/************************************************************************/
/*									*/
/*  Print out changed link status bits					*/
/*									*/
/************************************************************************/

typedef struct Lsbits { uLong mask; const char *prefix; const char *set; const char *clr; const char *suffix; } Lsbits;

static const Lsbits lsbits_msr[] = {
	0x80, "tx flow control ", "en", "dis", "abled", 
	0x40, "rx flow control ", "en", "dis", "abled", 
	0x10, "aux power ", "pre", "ab", "sent", 
	0x08, "speed ", "10", "100", "Mbps", 
	0x04, "link ", "Fail", "OK", "", 
	0x02, "send ", "pause", "done", " packet", 
	0x01, "receive ", "paus", "resum", "ed", 
	0, NULL, NULL, NULL, NULL };

static const Lsbits lsbits_bmcr[] = {
	0x8000, "", "reset", "running", "", 
	0x2000, "speed ", "10", "100", "Mbps", 
	0x1000, "autonegotiation ", "en", "dis", "abled", 
	0x0200, "autonegotiation ", "started", "finished", "", 
	0x0100, "", "full", "half", " duplex", 
	0, NULL, NULL, NULL, NULL };

static const Lsbits lsbits_bmsr[] = {
	0x8000, "100Base-T4 support ", "en", "dis", "abled", 
	0x4000, "100Base-TX fdx support ", "en", "dis", "abled", 
	0x2000, "100Base-TX hdx support ", "en", "dis", "abled", 
	0x1000, "10Base-T fdx support ", "en", "dis", "abled", 
	0x0800, "10Base-T hdx support ", "en", "dis", "abled", 
	0x0020, "autonegotiation ", "complete", "in progress", "", 
	0x0010, "", "", "no ", "remote fault", 
	0x0008, "link ", "had not ", "", "experienced fail state", 
	0x0004, "valid link ", "", "not ", "established", 
	0x0002, "", "", "no ", "jabber condition detected", 
	0x0001, "", "extended", "basic", " register capability", 
	0, NULL, NULL, NULL, NULL };

static uLong linkstatuschange1 (Devex *rtl, int all, const char *label, const Lsbits *lsbits, uLong msold, uLong msnew);

static void linkstatuschange (Devex *rtl, int all)

{
  rtl -> old_msr  = linkstatuschange1 (rtl, all, "MSR",  lsbits_msr,  rtl -> old_msr,  ether_inb (rtl -> iobase, IO_B_MSR));
  rtl -> old_bmcr = linkstatuschange1 (rtl, all, "BMCR", lsbits_bmcr, rtl -> old_bmcr, ether_inw (rtl -> iobase, IO_W_BMCR));
  rtl -> old_bmsr = linkstatuschange1 (rtl, all, "BMSR", lsbits_bmsr, rtl -> old_bmsr, ether_inw (rtl -> iobase, IO_W_BMSR));
}

static uLong linkstatuschange1 (Devex *rtl, int all, const char *label, const Lsbits *lsbits, uLong msold, uLong msnew)

{
  int i;
  uLong mask, mschg;

  mschg = all ? -1 : msnew ^ msold;

  for (i = 0; (mask = lsbits[i].mask) != 0; i ++) {
    if (mask & mschg) oz_knl_printk ("oz_dev_rtl8139:   %s %s%s%s\n", 
                                      label, 
                                      lsbits[i].prefix, 
                                      (msnew & mask) ? lsbits[i].set : lsbits[i].clr, 
                                      lsbits[i].suffix);
  }

  return (msnew);
}
