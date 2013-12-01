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

/************************************************************************/
/*									*/
/*  Most of this driver was liberated from the Linux eepro100 driver	*/
/*									*/
/*  This device driver is designed for the Intel i82557 "Speedo3" 	*/
/*  chip, Intel's single-chip fast ethernet controller for PCI, as 	*/
/*  used on the Intel EtherExpress Pro 100 adapter.			*/
/*									*/
/*  The Speedo-3 has receive and command unit base addresses that are 	*/
/*  added to almost all descriptor pointers.  The driver sets these to 	*/
/*  zero, so that all pointer fields are absolute pci addresses.	*/
/*									*/
/*  The System Control Block (SCB) of some previous Intel chips exists 	*/
/*  on the chip in both PCI I/O and memory space.  This driver uses 	*/
/*  the I/O space registers, but might switch to memory mapped mode to 	*/
/*  better support non-x86 processors.					*/
/*									*/
/*  This driver probably could use the simplified transmit descriptor 	*/
/*  mode, but doesn't as the Linux driver didn't.  Each Tx command 	*/
/*  block (TxCB) is associated with a single, immediately appended Tx 	*/
/*  buffer descriptor (TxBD).  Whereas the Linux driver has a fixed 	*/
/*  array of descriptors, though, we just put the descriptors at the 	*/
/*  beginning of each transmit buffer.					*/
/*									*/
/*  Configuration commands are placed on the transmit queue just like 	*/
/*  any other data buffer (except the command code is different).	*/
/*									*/
/*  Commands may have bits set e.g. CmdSuspend in the command word to 	*/
/*  either suspend or stop the transmit/command unit.  This driver 	*/
/*  always flags the last command with CmdSuspend, erases the 		*/
/*  CmdSuspend in the previous command, and then unconditionally 	*/
/*  issues an CU_RESUME just in case the controller saw the previous 	*/
/*  suspend.								*/
/*									*/
/*  Receives use the simplified descriptor format.  The descriptor is 	*/
/*  in the header of each receive buffer.				*/
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
#include "oz_sys_xprintf.h"

typedef struct Chnex Chnex;
typedef struct Devex Devex;
typedef struct Iopex Iopex;
typedef struct Rxbuf Rxbuf;
typedef struct Txbuf Txbuf;

static int congenb = 0;		/* Enable congestion control in the DP83840. */
static int txfifo  = 8;		/* Tx FIFO threshold in 4 byte units, 0-15 */
static int rxfifo  = 8;		/* Rx FIFO threshold, default 32 bytes. */

/* Tx/Rx DMA burst length, 0-127, 0 == no preemption, tx==128 -> disabled. */
static int txdmacount = 128;
static int rxdmacount = 0;

/* Maximum events (Rx packets, etc.) to handle at each interrupt. */

static int max_interrupt_work = 20;

/* Size of an pre-allocated Rx buffer: <Ethernet MTU> + slack.*/

#define MAXMSGSIZE    1536

/* Time before concluding the transmitter is hung. */

#define TX_TIMEOUT  "2.4"

/* PCI device and vendor ID number for the 82557 and 82558 (same for both) */

#define DIDVID_82557 0x12298086

/* Offsets to the various registers.  All accesses need not be longword aligned. */

enum speedo_offsets {
  SCBStatus  = 0,	/* Rx/Command Unit status */ 
  SCBCmd     = 2,	/* Rx/Command Unit command */
  SCBPointer = 4,	/* General purpose pointer */
  SCBPort    = 8,	/* Misc. commands and operands */
  SCBflash   = 12,	/* flash memory control */ 
  SCBeeprom  = 14,	/* EEPROM memory control */
  SCBCtrlMDI = 16,	/* MDI interface control */
  SCBEarlyRx = 20,	/* Early receive byte count */
};

/* Commands that can be put in a command list entry. */

enum commands {
  CmdNOp           = 0, 
  CmdIASetup       = 1, 
  CmdConfigure     = 2, 
  CmdMulticastList = 3,
  CmdTx            = 4, 
  CmdTDR           = 5, 
  CmdDump          = 6, 
  CmdDiagnose      = 7,
  CmdSuspend       = 0x4000,	/* Suspend after completion. */
  CmdIntr          = 0x2000,	/* Interrupt after completion. */
  CmdTxFlex        = 0x0008,	/* Use "Flexible mode" for CmdTx command. */
};

/* The SCB accepts the following controls for the Tx and Rx units: */

#define CU_START     0x0010
#define CU_RESUME    0x0020
#define CU_STATSADDR 0x0040
#define CU_SHOWSTATS 0x0050  /* Dump statistics counters. */
#define CU_CMD_BASE  0x0060  /* Base address to add to add CU commands. */
#define CU_DUMPSTATS 0x0070  /* Dump then reset stats counters. */

#define RX_START     0x0001
#define RX_RESUME    0x0002
#define RX_ABORT     0x0004
#define RX_ADDR_LOAD 0x0006
#define RX_RESUMENR  0x0007

#define INT_MASK    0x0100
#define DRVR_INT    0x0200    /* Driver generated interrupt. */

/* The Speedo3 Rx and Tx frame/buffer descriptors. */

struct descriptor {	/* A generic descriptor. */
  uWord status;		/* Offset 0. */
  uWord command;	/* Offset 2. */
  uLong link;		/* struct descriptor *  */
  unsigned char params[0];
};

/* Receive descriptor and buffer */

#define RXDESL (20+MAXMSGSIZE)	/* length starting at status that must be physically contiguous */

struct Rxbuf {
  Rxbuf *next;			/* next in receive list */
  Long refcount;		/* ref count (number of iopex -> rxbuf's pointing to it) */

				/* status..size must be physically contiguous and longword aligned */
  uLong status;			/* first and in the middle of list have 0x00000001 */
				/* last in list has 0xC0000002 */
  uLong link;			/* pci address of next Rxbuf in list */
  uLong rx_buf_addr;		/* pci address of buf.dstaddr */
				/* - rx_buf_addr unused by i82557, consistency check only */
  uWord count;			/* initially zero */
  uWord size;			/* MAXMSGSIZE */

				/* buf must be physically contiguous */
  OZ_IO_ether_buf buf;		/* ethernet transmit data */
  uByte pad2[4];		/* four bytes for crc */
};

/* Elements of the Rxbuf.status word */

#define RX_COMPLETE 0x8000
#define RX_NOFATAL  0x2000
#define RX_OVERRUN  0x0200

/* Transmit buffer */

#define TXDESL 24		/* length starting at status that must be physically contiguous */

struct Txbuf {
  Txbuf *next;			/* next in transmit list */
  Iopex *iopex;			/* associated iopex to post on completion */

				/* status..tx_buf_size must be physically contiguous and longword aligned */
  uLong status;			/* low order word is status */
				/* high order word is command */
  uLong link;			/* pci address of next Txbuf to transmit */
  uLong tx_desc_addr;		/* pci address of tx_buf_addr contained herein */
  uLong count;			/* # of TBD (=1), Tx start thresh., etc. */
  				/* this constitutes a single "TBD" entry -- we only use one: */
  uLong tx_buf_addr;		/*  - pci address of frame to be transmitted */
  uLong tx_buf_size;		/*  - length of Tx frame */

				/* buf must be physically contiguous */
  OZ_IO_ether_buf buf;		/* ethernet transmit data */
  uByte pad2[4];		/* four bytes for crc */
};

/* Elements of the dump_statistics block. This block must be lword aligned. */

struct speedo_stats {
  uLong tx_good_frames;
  uLong tx_coll16_errs;
  uLong tx_late_colls;
  uLong tx_underruns;
  uLong tx_lost_carrier;
  uLong tx_deferred;
  uLong tx_one_colls;
  uLong tx_multi_colls;
  uLong tx_total_colls;
  uLong rx_good_frames;
  uLong rx_crc_errs;
  uLong rx_align_errs;
  uLong rx_resource_errs;
  uLong rx_overrun_errs;
  uLong rx_colls_errs;
  uLong rx_runt_errs;
  uLong done_marker;
};

/* Per-interface data */

struct enet_statistics {
  uLong tx_aborted_errors;
  uLong tx_window_errors;
  uLong tx_fifo_errors;
  uLong tx_deferred;
  uLong collisions;
  uLong rx_crc_errors;
  uLong rx_frame_errors;
  uLong rx_over_errors;
  uLong rx_fifo_errors;
  uLong rx_length_errors;
  uLong rx_errors;
};

struct Devex {
  OZ_Devunit *devunit;				/* devunit pointer (device independent info) */
  const char *name;				/* devunit name string pointer */
  uLong ioaddr;					/* base I/O address (even) */
  uByte intvec;					/* irq vector number */
  uByte pad1;
  uByte enaddr[OZ_IO_ETHER_ADDRSIZE];		/* hardware address */
  OZ_Smplock *smplock;				/* pointer to irq level smp lock */

  struct enet_statistics stats;			/* accumulated statistics counters */
  struct speedo_stats lstats;			/* counters being read from chip */

  OZ_Timer *timer;				/* media selection timer */

  Chnex *chnexs;				/* all open channels on the device */

  Long receive_buffers_queued;			/* number of buffers queued to device (not including the null terminating buffer) */
  Long receive_requests_queued;			/* number of receive I/O requeusts queued to all channels on device */

  int rcvrunning;				/* 0: receiver is known to be halted */
						/* 1: receiver is probably running, and will interrupt when it halts */
  Rxbuf *rcvbusyqf;				/* first buffer in receive queue list, NULL if empty */
  Rxbuf *rcvbusyql;				/* last buffer in receive queue list, NULL if empty */

  Txbuf *xmtbusyqf;				/* first buffer in transmit queue list (never goes empty) */
  Txbuf *xmtbusyql;				/* last buffer in transmit queue list (never goes empty) */

  int mc_setup_frm_len;				/* The length of an allocated.. */
  Txbuf *mc_setup_frm;				/* ..multicast setup frame. */

  char rx_mode;					/* Current PROMISC/ALLMULTI setting. */
  int full_duplex;				/* Full-duplex operation requested. */
  int default_port;				/* Last dev->if_port value. */
  int rx_bug;					/* Work around receiver hang errata. */
  unsigned short phy[2];			/* PHY media interfaces available. */

  long last_rx_time;				/* Last Rx, in jiffies, to handle Rx hang. */
};

struct Chnex { int dummy;
             };

struct Iopex { OZ_Ioop *ioop;
             };

/* Function table */

static int i82557_shutdown (OZ_Devunit *devunit, void *devexv);
static uLong i82557_assign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode);
static int i82557_deassign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv);
static uLong i82557_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode, 
                           OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap);

static const OZ_Devfunc i82557_functable = { sizeof (Devex), sizeof (Chnex), sizeof (Iopex), 0, 
                                             i82557_shutdown, NULL, NULL, i82557_assign, i82557_deassign, NULL, i82557_start, NULL };

/* Driver static data */

static int initialized = 0;
static OZ_Devclass  *devclass  = NULL;
static OZ_Devdriver *devdriver = NULL;
static Rxbuf *free_rxbufs = NULL;
static Txbuf *free_txbufs = NULL;

/* The parameters for a CmdConfigure operation.                               */
/* There are so many options that it would be difficult to document each bit. */
/* We mostly use the default or recommended settings.                         */

const char basic_config_cmd[22] = {
  22, 0x08, 0, 0,  0, 0x80, 0x32, 0x03,  1, /* 1=Use MII  0=Use AUI */
  0, 0x2E, 0,  0x60, 0,
  0xf2, 0x48,   0, 0x40, 0xf2, 0x80,     /* 0x40=Force full-duplex */
  0x3f, 0x05, };

/* PHY media interface chips */

static const char *phys[] = {
  "None", "i82553-A/B", "i82553-C", "i82503",
  "DP83840", "80c240", "80c24", "i82555",
  "unknown-8", "unknown-9", "DP83840A", "unknown-11",
  "unknown-12", "unknown-13", "unknown-14", "unknown-15", };

static void ctrl_init (uLong ioaddr, uByte irqlevel, char *unitname);
static uWord read_eeprom (uLong ioaddr, uLong location);
static int mdio_read(int ioaddr, int phy_id, int location);
static int mdio_write(int ioaddr, int phy_id, int location, int value);
static void speedo_timer (void *devexv);
static void speedo_queuercvbuf (Rxbuf *rxbuf, Devex *devex);
static void speedo_queuexmtdata (Txbuf *txbuf, Devex *devex);
static void speedo_queuexmtbuf (Txbuf *txbuf, uWord command, Devex *devex);
static void speedo_tx_timeout (Devex *devex);
static int speedo_close (Devex *devex);
static void timer_stats (void *devexv, OZ_Timer *timer);
static struct enet_statistics *speedo_get_stats (Devex *devex);
#if 000
static int speedo_ioctl (Devex *devex, struct ifreq *rq, int cmd);
#endif
static void set_rx_mode (Devex *devex, int new_rx_mode);
static void speedo_interrupt (void *devexv, OZ_Mchargs *mchargs);
static void speedo_rx (Devex *devex);
static void speedo_tx (Devex *devex);
static Rxbuf *allocrcvbuf (void);
static void freercvbuf (Rxbuf *rxbuf);
static Txbuf *allocxmtbuf (void);
static void freexmtbuf (Txbuf *txbuf);
static void wait_for_ready (uLong ioaddr);
static uLong virt_to_pci (uLong size, void *buff);

/************************************************************************/
/*									*/
/*  Boot time initialization routines					*/
/*									*/
/************************************************************************/

void oz_dev_i82557_init ()

{
  char unitname[OZ_DEVUNIT_NAMESIZE];
  int init;
  OZ_Dev_pci_conf_p pciconfp;
  uByte pci_irq_line, pci_latency;
  uLong didvid, pci_ioaddr;
  uWord pci_command;

  if (initialized) return;

  oz_knl_printk ("oz_dev_i82557_init\n");
  initialized = 1;

  devclass  = oz_knl_devclass_create (OZ_IO_ETHER_CLASSNAME, OZ_IO_ETHER_BASE, OZ_IO_ETHER_MASK, "i82557_486");
  devdriver = oz_knl_devdriver_create (devclass, "i82557_486");

  /* Scan pci for Speedo chips */

  for (init = 1; oz_dev_pci_conf_scan (&pciconfp, init); init = 0) {
    if (pciconfp.pcifunc != 0) continue;

    /* Make sure it is an Intel 82557 (or 82558) */

    didvid = oz_dev_pci_conf_inl (&pciconfp, OZ_DEV_PCI_CONF_L_DIDVID);
    if (didvid != DIDVID_82557) continue;

    oz_knl_printk ("oz_dev_i82557: ethernet controller found: bus/device/function %u/%u/%u\n", pciconfp.pcibus, pciconfp.pcidev, pciconfp.pcifunc);
    oz_sys_sprintf (sizeof unitname, unitname, "i82557_%u_%u", pciconfp.pcibus, pciconfp.pcidev);

    /* Get I/O base address and interrupt vector */

    /* Note: BASE_ADDRESS_0 is for memory-mapping the registers       */
    /*       BASE_ADDRESS_1 is for I/O mapped registers (what we use) */

    pci_ioaddr = oz_dev_pci_conf_inl (&pciconfp, OZ_DEV_PCI_CONF_L_BASADR1);
    if (!(pci_ioaddr & 1)) {
      oz_knl_printk ("oz_dev_i82557: io base address %x is not odd\n", pci_ioaddr);
      continue;
    }
    pci_ioaddr  &= -4;
    pci_irq_line = oz_dev_pci_conf_inb (&pciconfp, OZ_DEV_PCI_CONF_B_INTLINE);

    /* Get and check the bus-master and latency values. */

    pci_command  = oz_dev_pci_conf_inw (&pciconfp, OZ_DEV_PCI_CONF_W_PCICMD);
    pci_command |= OZ_DEV_PCI_CONF_PCICMD_ENAB_IO | OZ_DEV_PCI_CONF_PCICMD_ENAB_MAS;
    oz_dev_pci_conf_outw (pci_command, &pciconfp, OZ_DEV_PCI_CONF_W_PCICMD);

    pci_latency = oz_dev_pci_conf_inb (&pciconfp, OZ_DEV_PCI_CONF_B_LATIMER);
    if (pci_latency < 10) {
      oz_knl_printk ("oz_dev_i82557: PCI latency timer (CFLT) is unreasonably low at %u, setting to 255 clocks.\n", pci_latency);
      oz_dev_pci_conf_outb (255, &pciconfp, OZ_DEV_PCI_CONF_B_LATIMER);
    }

    /* Now initialize the controller */

    ctrl_init (pci_ioaddr, pci_irq_line, unitname);
  }
}

/* Controller initialization routine */

static void ctrl_init (uLong ioaddr, uByte irqlevel, char *unitname)

{
  char unitdesc[OZ_DEVUNIT_DESCSIZE];
  Devex *devex;
  int as, boguscnt, i, init, j;
  Long str[8];
  Long *volatile self_test_results;
  OZ_Devunit *devunit;
  OZ_IO_ether_buf *buf;
  Txbuf *txbuf;
  uByte enaddr[OZ_IO_ETHER_ADDRSIZE];
  uWord eeprom[64];
  uWord sum, value;

  static const char *connectors[] = { " RJ45", " BNC", " AUI", " MII" };

  /* Read the station address EEPROM before doing the reset */

  sum = 0;
  j   = 0;
  for (i = 0; i < 64; i ++) {
    value     = read_eeprom (ioaddr, i);
    eeprom[i] = value;
    sum      += value;
    if (j < sizeof enaddr) {
      enaddr[j++] = value;
      enaddr[j++] = value >> 8;
    }
  }
  if (sum != 0xBABA) {
    oz_knl_printk ("oz_dev_i82557: %s: invalid EEPROM checksum %#4.4x, check settings before activating this device!\n", unitname, sum);
    oz_knl_dumpmem (sizeof eeprom, eeprom);
  }

  /* Reset the chip: stop Tx and Rx processes and clear counters.               */
  /* This takes less than 10usec and will easily finish before the next action. */

  oz_hw_outl (0, ioaddr + SCBPort);

  /* Create devunit struct */

  oz_sys_sprintf (sizeof unitdesc, unitdesc, "port %x, irq %u, addr %2.2x-%2.2x-%2.2x-%2.2x-%2.2x-%2.2x",
	ioaddr, irqlevel, enaddr[0], enaddr[1], enaddr[2], enaddr[3], enaddr[4], enaddr[5]);

  devunit = oz_knl_devunit_create (devdriver, unitname, unitdesc, &i82557_functable, 0, oz_s_secattr_sysdev);
  devex   = oz_knl_devunit_ex (devunit);
  memset (devex, 0, sizeof *devex);
  devex -> devunit = devunit;
  devex -> name    = oz_knl_devunit_devname (devunit);

  oz_knl_printk ("oz_dev_i82557: - board assembly %4.4x%2.2x-%3.3d, physical connectors present:", eeprom[8], eeprom[9]>>8, eeprom[9] & 0xff);
  for (i = 0; i < 4; i++) if (eeprom[5] & (1 << i)) oz_knl_printk (connectors[i]);
  oz_knl_printk ("\n");

  oz_knl_printk ("oz_dev_i82557: - primary interface chip %s PHY #%d\n", phys[(eeprom[6]>>8)&15], eeprom[6] & 0x1f);
  if (eeprom[7] & 0x0700) oz_knl_printk ("oz_dev_i82557: - secondary interface chip %s\n", phys[(eeprom[7]>>8)&7]);

#if defined(notdef)
  /* ToDo: Read and set PHY registers through MDIO port. */
  for (i = 0; i < 2; i++) oz_knl_printk ("oz_dev_i82557*:  MDIO register %d is %4.4x.\n", i, mdio_read(ioaddr, eeprom[6] & 0x1f, i));
  for (i = 5; i < 7; i++) oz_knl_printk ("oz_dev_i82557*:  MDIO register %d is %4.4x.\n", i, mdio_read(ioaddr, eeprom[6] & 0x1f, i));
  oz_knl_printk ("oz_dev_i82557*:  MDIO register %d is %4.4x.\n", 25, mdio_read(ioaddr, eeprom[6] & 0x1f, 25));
#endif

#if 000
  if (((eeprom[6]>>8) & 0x3f) == DP83840 || ((eeprom[6]>>8) & 0x3f) == DP83840A) {
    int mdi_reg23;

    mdi_reg23 = mdio_read(ioaddr, eeprom[6] & 0x1f, 23) | 0x0422;
    if (congenb) mdi_reg23 |= 0x0100;
    oz_knl_printk ("  DP83840 specific setup, setting register 23 to %4.4x.\n", mdi_reg23);
    mdio_write(ioaddr, eeprom[6] & 0x1f, 23, mdi_reg23);
  }

  if ((options >= 0) && (options & 0x60)) {
    oz_knl_printk ("  Forcing %dMbs %s-duplex operation.\n", (options & 0x20 ? 100 : 10), (options & 0x10 ? "full" : "half"));
    mdio_write(ioaddr, eeprom[6] & 0x1f, 0, 
           ((options & 0x20) ? 0x2000 : 0) | /* 100mbps? */
           ((options & 0x10) ? 0x0100 : 0)); /* Full duplex? */
  }
#endif

  /* Perform a system self-test.  The self-test results must be paragraph (16-byte) aligned. */

  self_test_results    = (Long *) ((((OZ_Pointer) str) + 15) & ~15);
  self_test_results[0] =  0;
  self_test_results[1] = -1;
  oz_hw_outl (virt_to_pci (16, self_test_results) | 1, ioaddr + SCBPort);
  boguscnt = 16000;
  do oz_hw_stl_nanowait (10000);
  while ((self_test_results[1] == -1) && (-- boguscnt >= 0));

  if (boguscnt < 0) {    /* Test timed out */
    oz_knl_printk ("oz_dev_i82557: Self test failed, status %8.8x:\n"
           "oz_dev_i82557: Failure to initialize the i82557.\n"
           "oz_dev_i82557: Verify that the card is a bus-master capable slot.\n",
           self_test_results[1]);
  } else {
    oz_knl_printk ("oz_dev_i82557:  General self-test: %s.\n"
                   "oz_dev_i82557:  Serial sub-system self-test: %s.\n"
                   "oz_dev_i82557:  Internal registers self-test: %s.\n"
                   "oz_dev_i82557:  ROM checksum self-test: %s (%#8.8x).\n",
           self_test_results[1] & 0x1000 ? "failed" : "passed",
           self_test_results[1] & 0x0020 ? "failed" : "passed",
           self_test_results[1] & 0x0008 ? "failed" : "passed",
           self_test_results[1] & 0x0004 ? "failed" : "passed",
           self_test_results[0]);
  }

  /* Reset the chip again */

  oz_hw_outl (0, ioaddr + SCBPort);

  /* Fill in devex */

  devex -> ioaddr = ioaddr;
  devex -> intvec = irqlevel;

#if 000
  devex -> full_duplex  = options >= 0 && (options & 0x10) ? 1 : 0;
  devex -> default_port = options >= 0 ? (options & 0x0f) : 0;
#endif

  devex -> phy[0] = eeprom[6];
  devex -> phy[1] = eeprom[7];
  devex -> rx_bug = ((eeprom[3] & 0x03) != 3);
  if (devex -> rx_bug) oz_knl_printk ("oz_dev_i82557: - receiver lock-up workaround activated.\n");

  memcpy (devex -> enaddr, enaddr, sizeof devex -> enaddr);

  /* Connect to irq vector - assume we're the only one using it */

  devex -> smplock = oz_hw_irq_only (irqlevel, speedo_interrupt, devex, devex -> name);
  if (devex -> smplock == NULL) {
    oz_knl_printk ("oz_dev_i82557: irq %u already in use\n", irqlevel);
    /* ?? delete devunit by incing refcount by 0 and providing a clonedel routine */
    return;
  }

  /* Load the statistics block address */

  wait_for_ready (ioaddr);						/* wait for controller ready to accept new command */
  oz_hw_outl (virt_to_pci (sizeof devex -> lstats, &(devex -> lstats)), ioaddr + SCBPointer); /* set up pci address of lstats buffer */
  oz_hw_outw (INT_MASK | CU_STATSADDR, ioaddr + SCBCmd);		/* tell controller what we just did */
  devex -> lstats.done_marker = 0;					/* the lstats buffer has not been read into yet */

  /* Initialize receive ring and start receiver process */

  wait_for_ready (ioaddr);						/* wait for controller ready to accept new command */
  oz_hw_outl (0, ioaddr + SCBPointer);					/* base address to add to all RX pointers = zero */
  oz_hw_outw (INT_MASK | RX_ADDR_LOAD, ioaddr + SCBCmd);

  speedo_queuercvbuf (allocrcvbuf (), devex);
  speedo_queuercvbuf (allocrcvbuf (), devex);

  /* Fill the first command with our ethernet address.              */
  /* Avoid a bug(?!) here by marking the command already completed. */

  txbuf = allocxmtbuf ();

  txbuf -> status = ((CmdSuspend | CmdIASetup) << 16) | 0xa000;		/* set up the command */
  txbuf -> link   = 0;
  memcpy (&(txbuf -> tx_desc_addr), devex -> enaddr, 6);

  devex -> xmtbusyqf = txbuf;
  devex -> xmtbusyql = txbuf;

  wait_for_ready (ioaddr);						/* wait for controller ready to accept new command */
  oz_hw_outl (0, ioaddr + SCBPointer);					/* set base offset for all cmd pointers = zero */
  oz_hw_outw (INT_MASK | CU_CMD_BASE, ioaddr + SCBCmd);

  /* Start the chip's Tx process and unmask interrupts */

  wait_for_ready (ioaddr);						/* wait for controller ready to accept new command */
  oz_hw_outl (virt_to_pci (TXDESL, &(txbuf -> status)), ioaddr + SCBPointer); /* give it pointer to transmit buffer */
  oz_hw_outw (CU_START, ioaddr + SCBCmd);				/* tell it to start processing it */

  /* Setup the chip and configure the multicast list. */

  devex -> mc_setup_frm     = NULL;
  devex -> mc_setup_frm_len = 0;
  devex -> rx_mode          = -1;					/* force it to send config info */
  set_rx_mode (devex, 3);						/* ?? always use promiscuous mode for now */

  /* Set the timer.  The timer serves a dual purpose:                                                 */
  /* 1) to monitor the media interface (e.g. link beat) and perhaps switch to an alternate media type */
  /* 2) to monitor Rx activity, and restart the Rx process if the receiver hangs                      */

#if 000
  devex -> timer = oz_knl_timer_alloc ();
  oz_knl_timer_insert (devex -> timer, ??datebin, speedo_timer, devex);
#endif

  /* Start a dump-stats operation so when they are requested, they will be ready */

  wait_for_ready (ioaddr);
  oz_hw_outw (CU_DUMPSTATS, ioaddr + SCBCmd);

  oz_knl_timer_insert (oz_knl_timer_alloc (), oz_hw_timer_getnow () + 150000000, timer_stats, devex);
}

/* Serial EEPROM section. A "bit" grungy, but we work our way through bit-by-bit :->. */

/*  EEPROM_Ctrl bits. */

#define EE_SHIFT_CLK  0x01	/* EEPROM shift clock. */
#define EE_CS         0x02	/* EEPROM chip select. */
#define EE_DATA_WRITE 0x04	/* EEPROM chip data in. */
#define EE_WRITE_0    0x01
#define EE_WRITE_1    0x05
#define EE_DATA_READ  0x08	/* EEPROM chip data out. */
#define EE_ENB     (0x4800 | EE_CS)

/* Delay between EEPROM clock transitions */

#define eeprom_delay(nanosec) oz_hw_stl_nanowait (nanosec)

/* The EEPROM commands include the always-set leading bit. */

#define EE_WRITE_CMD  (5 << 6)
#define EE_READ_CMD   (6 << 6)
#define EE_ERASE_CMD  (7 << 6)

static uWord read_eeprom (uLong ioaddr, uLong location)

{
  int i;
  uLong ee_addr;
  uWord dataval, read_cmd, retval;

  ee_addr = ioaddr + SCBeeprom;					/* point to eeprom access I/O register */

  oz_hw_outw (EE_ENB & ~EE_CS, ee_addr);			/* turn off chip select to reset eeprom state */
  eeprom_delay (250);
  oz_hw_outw (EE_ENB, ee_addr);					/* turn chip select back on so we can access chip */
  eeprom_delay (250);

  /* Shift the read command bits out. */

  read_cmd = EE_READ_CMD | location;				/* set up command/address word */

  for (i = 10; i >= 0; i--) {					/* step through command/address bits */
    dataval = (read_cmd & (1 << i)) ? EE_DATA_WRITE : 0;	/* set up data write bit value */
    oz_hw_outw (EE_ENB | dataval, ee_addr);			/* output command/address bit on bus */
    eeprom_delay (100);						/* wait the setup time */
    oz_hw_outw (EE_ENB | dataval | EE_SHIFT_CLK, ee_addr);	/* raise clock strobe high */
    eeprom_delay (150);						/* wait the pulse width time */
    oz_hw_outw (EE_ENB | dataval, ee_addr);			/* lower clock strobe */
    eeprom_delay (250);						/* wait the data hold time */
  }

  oz_hw_outw (EE_ENB, ee_addr);					/* remove command/address bit from bus */
  eeprom_delay (250);

  for (i = 15; i >= 0; i--) {					/* step through data bits */
    oz_hw_outw (EE_ENB | EE_SHIFT_CLK, ee_addr);		/* strobe clock high to get a data bit */
    eeprom_delay (100);						/* wait the access time */
    retval <<= 1;						/* shift our return value */
    if (oz_hw_inw (ee_addr) & EE_DATA_READ) retval ++;		/* read the bit into bottom of shift register */
    oz_hw_outw (EE_ENB, ee_addr);				/* lower the clock */
    eeprom_delay (100);						/* wait the clock low period */
  }

  /* Terminate the EEPROM access. */

  oz_hw_outw (EE_ENB & ~EE_CS, ee_addr);			/* done, clear chip select to reset eeprom state */

  return retval;						/* return the data word */
}

static int mdio_read(int ioaddr, int phy_id, int location)

{
  int val, boguscnt = 64*4;    /* <64 usec. to complete, typ 27 ticks */
  oz_hw_outl(0x08000000 | (location<<16) | (phy_id<<21), ioaddr + SCBCtrlMDI);
  do {
    oz_hw_stl_nanowait (16000);
    val = oz_hw_inl(ioaddr + SCBCtrlMDI);
    if (--boguscnt < 0) {
      oz_knl_printk ("oz_dev_i82557:  mdio_read() timed out with val = %8.8x.\n", val);
    }
  } while (! (val & 0x10000000));
  return val & 0xffff;
}

static int mdio_write(int ioaddr, int phy_id, int location, int value)

{
  int val, boguscnt = 64*4;    /* <64 usec. to complete, typ 27 ticks */
  oz_hw_outl(0x04000000 | (location<<16) | (phy_id<<21) | value,
     ioaddr + SCBCtrlMDI);
  do {
    oz_hw_stl_nanowait (16000);
    val = oz_hw_inl(ioaddr + SCBCtrlMDI);
    if (--boguscnt < 0) {
      oz_knl_printk ("oz_dev_i82557: mdio_write() timed out with val = %8.8x.\n", val);
    }
  } while (! (val & 0x10000000));
  return val & 0xffff;
}

/************************************************************************/
/*									*/
/*  This routine is called by the system shutdown routine to reset 	*/
/*  the hardware.  It should make sure that the chip will cease all 	*/
/*  operations (ie, no more memory accesses or interrupts will be 	*/
/*  generated) before it returns.					*/
/*									*/
/************************************************************************/

static int i82557_shutdown (OZ_Devunit *devunit, void *devexv)

{
  /* Reset the chip: stop Tx and Rx processes and clear counters */

  oz_hw_outl (0, ((Devex *)devexv) -> ioaddr + SCBPort);
  return (1);
}

/************************************************************************/
/*									*/
/*  An I/O channel is being assigned to the device.  Init the chnex 	*/
/*  area.								*/
/*									*/
/************************************************************************/

static uLong i82557_assign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode)

{
  return (OZ_DEVOFFLINE);
}

/************************************************************************/
/*									*/
/*  An I/O channel is being deassigned.  Close down all operations on 	*/
/*  the channel.							*/
/*									*/
/************************************************************************/

static int i82557_deassign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv)

{
  return (0);
}

/************************************************************************/
/*									*/
/*  An new I/O operation is to be started on the device			*/
/*									*/
/************************************************************************/

static uLong i82557_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode, 
                           OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap)

{
  return (OZ_BADIOFUNC);
}

/************************************************************************/
/*									*/
/*  This routine is called every 2.4 seconds to monitor media and control
/*									*/
/************************************************************************/

#if 000
static void speedo_timer (void *devexv)

{
  Devex *devex;
  int tickssofar;

  devex = devexv;
  tickssofar = jiffies - devex -> last_rx_time;

  if (speedo_debug > 3) {
    uLong ioaddr = devex -> ioaddr;

    oz_knl_printk ("oz_dev_i82557*: %s: Media selection tick, status %4.4x.\n", devex -> name, oz_hw_inw (ioaddr + SCBStatus));
  }

  if (devex -> rx_bug) {
    if (tickssofar > 2*HZ  || sp->rx_mode < 0) {
      /* We haven't received a packet in a Long Time.  We might have been
         bitten by the receiver hang bug.  This can be cleared by sending
         a set multicast list command. */
      set_rx_mode(devex, 3);
    }
    /* We must continue to monitor the media. */
    sp->timer.expires = RUN_AT(2*HZ);       /* 2.0 sec. */
    add_timer(&sp->timer);
  }
}
#endif

/************************************************************************/
/*									*/
/*  Malloc and queue a receive buffer to the controller			*/
/*									*/
/************************************************************************/

static void speedo_queuercvbuf (Rxbuf *rxbuf, Devex *devex)

{
  Rxbuf *lrxbuf;
  uLong il, ioaddr;

  /* Prepare buffer */

  rxbuf -> next        = NULL;						/* set up the header */
  rxbuf -> refcount    = 0;

  rxbuf -> status      = 0xC0000002;

  rxbuf -> link        = 0;						/* none follow this one yet */
  rxbuf -> rx_buf_addr = virt_to_pci (RXDESL, &(rxbuf -> buf));		/* save pointer to where to save receive data */
									/* - unused by i82557, consistency check only */
  rxbuf -> count       = 0;						/* haven't received anything into it yet */
  rxbuf -> size        = MAXMSGSIZE;					/* this is the size of the area */

  /* Put it on end of receive queue */

  il = oz_hw_smplock_wait (devex -> smplock);				/* lock out interrupts */

  lrxbuf = devex -> rcvbusyql;						/* get last on queue */
  if (lrxbuf == NULL) devex -> rcvbusyqf = rxbuf;			/* - this is the only one on queue */
  else {
    lrxbuf -> next = rxbuf;						/* - link this one after the last one */
    lrxbuf -> link = virt_to_pci (RXDESL, &(rxbuf -> status));		/* - (link it for the controller to see, too) */
  }
  devex -> rcvbusyql = rxbuf;						/* either way, this one is the new last one */

  if (!(devex -> rcvrunning)) {						/* see if receiver already running */
    ioaddr = devex -> ioaddr;						/* if not, point to i/o registers */
    wait_for_ready (ioaddr);						/* wait for controller ready to accept new command */
    oz_hw_outl (virt_to_pci (RXDESL, &(rxbuf -> status)), ioaddr + SCBPointer); /* set up pci address of first receive buffer */
    oz_hw_outw (RX_START, ioaddr + SCBCmd);				/* tell controller to start receiving into it */
    devex -> rcvrunning = 1;						/* remember receiver is running */
  }

  devex -> receive_buffers_queued ++;					/* one more buffer queued to receiver */

  oz_hw_smplock_clr (devex -> smplock, il);				/* restore interrupt delivery */
}

/************************************************************************/
/*									*/
/*  Queue a buffer for transmit						*/
/*									*/
/************************************************************************/

static void speedo_queuexmtdata (Txbuf *txbuf, Devex *devex)

{
  oz_knl_printk ("oz_dev_i82557*: queuexmtdata dlen %u\n", txbuf -> buf.dlen);
  oz_knl_dumpmem (32, &(txbuf -> buf));

  txbuf -> tx_desc_addr = virt_to_pci (TXDESL, &(txbuf -> tx_buf_addr)); /* point to the descriptors */

  txbuf -> count        = 0x01208000;					/* the data region is always in one buffer descriptor, */
									/* Tx FIFO threshold of 256. */
  txbuf -> tx_buf_addr  = virt_to_pci (MAXMSGSIZE, txbuf -> buf.dstaddr); /* set up pointer to data (destination address) */
  txbuf -> tx_buf_size  = txbuf -> buf.data + txbuf -> buf.dlen - txbuf -> buf.dstaddr; /* set up length of data, including ???? */

  speedo_queuexmtbuf (txbuf, (CmdSuspend | CmdTx | CmdTxFlex | CmdIntr), devex);
}

static void speedo_queuexmtbuf (Txbuf *txbuf, uWord command, Devex *devex)

{
  Txbuf *ltxbuf;
  uLong il, ioaddr;

  ioaddr = devex -> ioaddr;

  /* Set up transmit buffer header stuff */

  txbuf -> status = command << 16;				/* set up command word */
  txbuf -> link   = 0;						/* this will be the last in the list */

  /* Prevent interrupts from changing the Tx ring from underneath us */

  il = oz_hw_smplock_wait (devex -> smplock);

  /* Put buffer on end of both our queue and the controller's queue */

  ltxbuf = devex -> xmtbusyql;						/* point to last buffer on transmit queue */
  ltxbuf -> link = virt_to_pci (TXDESL, &(txbuf -> status));		/* set link that controller uses */
  OZ_HW_MB;								/* make sure above write gets done before clearing CmdSuspend */
  ltxbuf -> status   &= ~((CmdSuspend | CmdIntr) << 16);		/* don't suspend or interrupt after previous buffer completes */
  ltxbuf -> next      = txbuf;						/* set link that we use */
  devex  -> xmtbusyql = txbuf;						/* now this one is the last in the list */

  oz_knl_printk ("oz_dev_i82557*: queuexmtbuf ltxbuf %p, txbuf %p, command %x\n", ltxbuf, txbuf, command);

  /* Trigger the command unit resume in case it had already processed the last entry */

  wait_for_ready (ioaddr);						/* wait for controller ready to accept command */
  oz_hw_outw (CU_RESUME, ioaddr + SCBCmd);				/* tell it to resume transmitting in case it suspended already */

  oz_knl_printk ("oz_dev_i82557*:  - cmdsts %8.8x\n", oz_hw_inl (ioaddr));

  /* Restore hardware interrupt delivery */

  oz_hw_smplock_clr (devex -> smplock, il);
}

static void speedo_tx_timeout (Devex *devex)

{
#if 000
  int i;
  uLong ioaddr;

  ioaddr = devex -> ioaddr;

  oz_knl_printk ("oz_dev_i82557: %s: Transmit timed out: status %4.4x command %4.4x.\n", devex -> name, oz_hw_inw(ioaddr + SCBStatus), oz_hw_inw(ioaddr + SCBCmd));

#ifndef final_version
  oz_knl_printk ("oz_i82557_dev:" "%s:  Tx timeout  fill index %d  scavenge index %d.\n", devex -> name, sp->cur_tx, sp->dirty_tx);
  oz_knl_printk ("oz_i82557_dev:" "    Tx queue ");
  for (i = 0; i < TX_RING_SIZE; i++) oz_knl_printk (" %8.8x", (int)sp->tx_ring[i].status);
  oz_knl_printk (".\n" "oz_i82557_dev:" "    Rx ring ");
  for (i = 0; i < RX_RING_SIZE; i++) oz_knl_printk (" %8.8x", (int)sp->rx_ringp[i]->status);
  oz_knl_printk (".\n");

#else
  dev->if_port ^= 1;
  oz_knl_printk ("oz_i82557_dev:" "  (Media type switching not yet implemented.)\n");
  /* Do not do 'dev->tbusy = 0;' there -- it is incorrect. */
#endif


  if (oz_hw_inw ((ioaddr + SCBStatus) & 0x00C0) != 0x0080) {
    oz_knl_printk ("oz_i82557_dev:" "%s: Trying to restart the transmitter...\n", devex -> name);
    oz_hw_outl(virt_to_pci(TXDESL, &sp->tx_ring[sp->dirty_tx % TX_RING_SIZE]), ioaddr + SCBPointer);
    oz_hw_outw(CU_START, ioaddr + SCBCmd);
  } else {
    oz_hw_outw(DRVR_INT, ioaddr + SCBCmd);
  }

  /* Reset the MII transceiver. */

  if ((sp->phy[0] & 0x8000) == 0) mdio_write (ioaddr, sp->phy[0] & 0x1f, 0, 0x8000);
  sp->stats.tx_errors++;
  dev->trans_start = jiffies;
  return;
#endif
}

/************************************************************************/
/*									*/
/*  Close (deactivate) device						*/
/*									*/
/************************************************************************/

#if 000
static int speedo_close (Devex *devex)

{
  int ioaddr = dev->base_addr;
  struct speedo_private *sp = (struct speedo_private *)dev->priv;
  int i;

  dev->start = 0;
  dev->tbusy = 1;

  if (speedo_debug > 1) oz_knl_printk ("oz_dev_i82557*: %s: Shutting down ethercard, status was %4.4x.\n", devex -> name, oz_hw_inw(ioaddr + SCBStatus));

  /* Shut off the media monitoring timer. */

  del_timer (&sp->timer);

  /* Disable interrupts, and stop the chip's Rx process. */

  oz_hw_outw (INT_MASK, ioaddr + SCBCmd);
  oz_hw_outw (INT_MASK | RX_ABORT, ioaddr + SCBCmd);

  free_irq (dev->irq, dev);

  /* Free all the skbuffs in the Rx and Tx queues. */

  for (i = 0; i < RX_RING_SIZE; i++) {
    struct sk_buff *skb = sp->rx_skbuff[i];
    sp->rx_skbuff[i] = 0;
    /* Clear the Rx descriptors. */
    if (skb) dev_kfree_skb (skb, FREE_WRITE);
  }

  for (i = 0; i < TX_RING_SIZE; i++) {
    struct sk_buff *skb = sp->tx_skbuff[i];
    sp->tx_skbuff[i] = 0;
    /* Clear the Tx descriptors. */
    if (skb) dev_kfree_skb(skb, FREE_WRITE);
  }
  if (sp->mc_setup_frm) {
    kfree(sp->mc_setup_frm);
    sp->mc_setup_frm_len = 0;
  }

  /* Print a few items for debugging. */

  if (speedo_debug > 3) {
    int phy_num = sp->phy[0] & 0x1f;
    oz_knl_printk ("oz_dev_i82557*: %s:Printing Rx ring (next to receive into %d).\n", devex -> name, sp->cur_rx);

    for (i = 0; i < RX_RING_SIZE; i++) oz_knl_printk ("oz_dev_i82557*:   Rx ring entry %d  %8.8x.\n", i, (int)sp->rx_ringp[i]->status);
    for (i = 0; i < 5; i++) oz_knl_printk ("oz_dev_i82557*:   PHY index %d register %d is %4.4x.\n", phy_num, i, mdio_read(ioaddr, phy_num, i));
    for (i = 21; i < 26; i++) oz_knl_printk ("oz_dev_i82557*:   PHY index %d register %d is %4.4x.\n", phy_num, i, mdio_read(ioaddr, phy_num, i));
  }

  return 0;
}
#endif

/************************************************************************/
/*									*/
/*  The Speedo-3 has an especially awkward and unusable method of 	*/
/*  getting statistics out of the chip.  It takes an unpredictable 	*/
/*  length of time for the dump-stats command to complete.  To avoid a 	*/
/*  busy-wait loop we update the stats with the previous dump results, 	*/
/*  and then trigger a new dump.					*/
/*									*/
/*  These problems are mitigated by the current /proc implementation, 	*/
/*  which calls this routine first to judge the output length, and 	*/
/*  then to emit the output.						*/
/*									*/
/*  Oh, and incoming frames are dropped while executing dump-stats!	*/
/*									*/
/************************************************************************/

static void timer_stats (void *devexv, OZ_Timer *timer)

{
  Devex *devex;
  Txbuf *txbuf;

  devex = devexv;

  oz_knl_printk ("oz_dev_i82557 timer_stats*: %s:\n", devex -> name);
  oz_knl_dumpmem (sizeof devex -> lstats, &(devex -> lstats));
  speedo_get_stats (devex);
  oz_knl_dumpmem (sizeof devex -> stats, &(devex -> stats));

  txbuf = allocxmtbuf ();
  txbuf -> buf.dlen = 100;
  memset (txbuf -> buf.dstaddr, -1, sizeof txbuf -> buf.dstaddr);
  memcpy (txbuf -> buf.srcaddr, devex -> enaddr, sizeof txbuf -> buf.srcaddr);
  memset (txbuf -> buf.proto, 0x69, sizeof txbuf -> buf.proto);
  speedo_queuexmtdata (txbuf, devex);

  oz_knl_timer_insert (timer, oz_hw_timer_getnow () + 150000000, timer_stats, devex);
}

static struct enet_statistics *speedo_get_stats (Devex *devex)

{
  uLong ioaddr;

  /* If the previous dump has finished, add the values to our previous totals then start a new dump                */
  /* If it hasn't finished, we just return the previous totals (it thus hasn't been that long since the last dump) */

  if (devex -> lstats.done_marker == 0xA007) {

    /* Add amounts from chips counters to our permanent counters */

    devex -> stats.tx_aborted_errors += devex -> lstats.tx_coll16_errs;
    devex -> stats.tx_window_errors  += devex -> lstats.tx_late_colls;
    devex -> stats.tx_fifo_errors    += devex -> lstats.tx_underruns;
    devex -> stats.tx_fifo_errors    += devex -> lstats.tx_lost_carrier;
    /*devex -> stats.tx_deferred     += devex -> lstats.tx_deferred;*/
    devex -> stats.collisions        += devex -> lstats.tx_total_colls;
    devex -> stats.rx_crc_errors     += devex -> lstats.rx_crc_errs;
    devex -> stats.rx_frame_errors   += devex -> lstats.rx_align_errs;
    devex -> stats.rx_over_errors    += devex -> lstats.rx_resource_errs;
    devex -> stats.rx_fifo_errors    += devex -> lstats.rx_overrun_errs;
    devex -> stats.rx_length_errors  += devex -> lstats.rx_runt_errs;

    /* Now start another dump (hopefully it will be done by the next time we're here) */

    ioaddr = devex -> ioaddr;
    devex -> lstats.done_marker = 0;					/* the lstats buffer has not been read into yet */
    wait_for_ready (ioaddr);
    oz_hw_outw (CU_DUMPSTATS, ioaddr + SCBCmd);
  }

  /* Return pointer to stats struct */

  return (&(devex -> stats));
}

/************************************************************************/
/*									*/
/*  Process I/O Control functions					*/
/*									*/
/************************************************************************/

#if 000
static int speedo_ioctl (Devex *devex, struct ifreq *rq, int cmd)

{
  int ioaddr;
  struct speedo_private *sp
  uWord *data;

  ioaddr = dev -> base_addr;
  sp     = (struct speedo_private *)(dev -> priv);
  data   = (u16 *)&(rq -> ifr_data);

  switch(cmd) {
    case SIOCDEVPRIVATE:	/* Get the address of the PHY in use. */
      data[0] = devex -> phy[0] & 0x1f;
      return 0;
    case SIOCDEVPRIVATE+1:	/* Read the specified MII register. */
      data[3] = mdio_read (ioaddr, data[0], data[1]);
      return 0;
    case SIOCDEVPRIVATE+2:	/* Write the specified MII register */
      if (!suser ()) return -EPERM;
      mdio_write (ioaddr, data[0], data[1], data[2]);
      return 0;
    default:
      return -EOPNOTSUPP;
  }
}
#endif

/************************************************************************/
/*									*/
/*  Set or clear the multicast filter for this adaptor			*/
/*									*/
/*  This is very ugly with Intel chips -- we usually have to execute 	*/
/*  an entire configuration command, plus process a multicast command.	*/
/*  This is complicated.  We must put a large configuration command 	*/
/*  and an arbitrarily-sized multicast command in the transmit list.	*/
/*  To minimize the disruption -- the previous command might have 	*/
/*  already loaded the link -- we convert the current command block, 	*/
/*  normally a Tx command, into a no-op and link it to the new command.	*/
/*									*/
/*    Input:								*/
/*									*/
/*	new_rx_mode = 0 : normal					*/
/*	              1 : all multicast addresses			*/
/*	              3 : promiscuous					*/
/*									*/
/************************************************************************/

static void set_rx_mode (Devex *devex, int new_rx_mode)

{
  int i;
  Txbuf *txbuf;
  uLong il, ioaddr;

  ioaddr = devex -> ioaddr;

  if (new_rx_mode != devex -> rx_mode) {

    /* We must change the configuration. Construct a CmdConfig frame. */

    txbuf = allocxmtbuf ();

    memcpy (&(txbuf -> tx_desc_addr), basic_config_cmd, sizeof basic_config_cmd);

    ((uByte *)&(txbuf -> tx_desc_addr))[ 1] = (txfifo << 4) | rxfifo;
    ((uByte *)&(txbuf -> tx_desc_addr))[ 4] = rxdmacount;
    ((uByte *)&(txbuf -> tx_desc_addr))[ 5] = txdmacount + 0x80;
    ((uByte *)&(txbuf -> tx_desc_addr))[15] = (new_rx_mode & 2) ? 0x49 : 0x48;
    ((uByte *)&(txbuf -> tx_desc_addr))[19] = devex -> full_duplex ? 0xC0 : 0x80;
    ((uByte *)&(txbuf -> tx_desc_addr))[21] = (new_rx_mode & 1) ? 0x0D : 0x05;
    if (devex -> phy[0] & 0x8000) {						/* Use the AUI port instead. */
      ((uByte *)&(txbuf -> tx_desc_addr))[ 8]  = 0;
      ((uByte *)&(txbuf -> tx_desc_addr))[15] |= 0x80;
    }

    oz_knl_printk ("oz_dev_i82557 set_rx_mode*: buffer:\n");
    oz_knl_dumpmem (sizeof basic_config_cmd, &(txbuf -> tx_desc_addr));

    speedo_queuexmtbuf (txbuf, CmdSuspend | CmdConfigure, devex);
  }

#if 000
  if ((new_rx_mode == 0) && (devex -> mc_count < 3)) {

    /* The simple case of 0-2 multicast list entries occurs often, and fits within one tx_ring[] entry. */

    uWord *setup_params, *eaddrs;
    struct dev_mc_list *mclist;

    txbuf = allocxmtbuf ();

    txbuf -> tx_desc_addr = 0; /* Really MC list count. */
    setup_params = (u16 *)&(txbuf -> tx_desc_addr);
    *setup_params++ = dev->mc_count*6;

    /* Fill in the multicast addresses. */

    for (i = 0, mclist = dev->mc_list; i < dev->mc_count; i++, mclist = mclist->next) {
      eaddrs = (u16 *)mclist->dmi_addr;
      *setup_params++ = *eaddrs++;
      *setup_params++ = *eaddrs++;
      *setup_params++ = *eaddrs++;
    }

    /* Queue it for transmit */

    il = oz_hw_smplock_wait (devex -> smplock);
    speedo_queuexmtbuf (txbuf, CmdSuspend | CmdMulticastList, devex);
    oz_hw_smplock_clr (devex -> smplock, il);

  } else if (new_rx_mode == 0) {

    /* This does not work correctly, but why not? */

    struct dev_mc_list *mclist;
    uWord *eaddrs;
    struct descriptor *mc_setup_frm = devex ->mc_setup_frm;
    uWord *setup_params = (u16 *)mc_setup_frm->params;
    int i;

    if (devex ->mc_setup_frm_len < 10 + dev->mc_count*6 || devex ->mc_setup_frm == NULL) {

      /* Allocate a new frame, 10bytes + addrs, with a few extra entries for growth. */

      if (devex ->mc_setup_frm) kfree(devex ->mc_setup_frm);
      devex ->mc_setup_frm_len = 10 + dev->mc_count*6 + 24;
      devex ->mc_setup_frm = kmalloc(devex ->mc_setup_frm_len, GFP_ATOMIC);
      if (devex ->mc_setup_frm == NULL) {
        oz_knl_printk ("oz_dev_i82557: %s: Failed to allocate a setup frame.\n", devex -> name);
        devex ->rx_mode = -1; /* We failed, try again. */
        return;
      }
    }
    mc_setup_frm = devex ->mc_setup_frm;

    /* Construct the new setup frame. */

    if (speedo_debug > 1) oz_knl_printk ("oz_dev_i82557*: %s: Constructing a setup frame at %p, %d bytes.\n", devex -> name, devex ->mc_setup_frm, devex ->mc_setup_frm_len);
    mc_setup_frm->status = 0;
    mc_setup_frm->command = CmdSuspend | CmdIntr | CmdMulticastList;

    /* Link set below. */

    setup_params = (u16 *)mc_setup_frm->params;
    *setup_params++ = dev->mc_count*6;

    /* Fill in the multicast addresses. */

    for (i = 0, mclist = dev->mc_list; i < dev->mc_count;
       i++, mclist = mclist->next) {
      eaddrs = (u16 *)mclist->dmi_addr;
      *setup_params++ = *eaddrs++;
      *setup_params++ = *eaddrs++;
      *setup_params++ = *eaddrs++;
    }

    /* Disable interrupts while playing with the Tx Cmd list. */

    save_flags(flags);
    cli();
    entry = devex ->cur_tx++ % TX_RING_SIZE;

    if (speedo_debug > 5) oz_knl_printk (" CmdMCSetup frame length %d in entry %d.\n", dev->mc_count, entry);

    /* Change the command to a NoOp, pointing to the CmdMulti command. */

    devex ->tx_skbuff[entry] = 0;
    devex ->tx_ring[entry].status = CmdNOp << 16;
    devex ->tx_ring[entry].link = virt_to_pci(mc_setup_frm);

    /* Set the link in the setup frame. */

    mc_setup_frm->link = virt_to_pci(&(devex ->tx_ring[devex ->cur_tx % TX_RING_SIZE]));
    devex ->last_cmd->command &= ~CmdSuspend;

    /* Immediately trigger the command unit resume. */

    wait_for_ready(ioaddr);
    oz_hw_outw(CU_RESUME, ioaddr + SCBCmd);
    devex ->last_cmd = mc_setup_frm;
    restore_flags(flags);
    if (speedo_debug > 1) oz_knl_printk ("oz_dev_i82557*: %s: Last command at %p is %4.4x.\n", devex -> name, devex ->last_cmd, devex ->last_cmd->command);
  }
#endif

  devex -> rx_mode = new_rx_mode;
}

/************************************************************************/
/*									*/
/*  The interrupt handler does all of the Rx thread work and cleans up	*/
/*  after the Tx thread.						*/
/*									*/
/************************************************************************/

static void speedo_interrupt (void *devexv, OZ_Mchargs *mchargs)

{
  Devex *devex;
  int boguscnt, i;
  Rxbuf *rxbuf;
  uLong ioaddr, txstat;
  uWord status;

  devex  = devexv;
  ioaddr = devex -> ioaddr;

  for (boguscnt = max_interrupt_work; -- boguscnt >= 0;) {

    /* Find out causes of interrupt and acknowledge them.  Exit loop if no interrupt pending. */

    status = oz_hw_inw (ioaddr + SCBStatus);
    oz_hw_outw (status & 0xfc00, ioaddr + SCBStatus);
    oz_knl_printk ("oz_dev_i82557*: %s: interrupt  status=%#4.4x.\n", devex -> name, status);
    if ((status & 0xfc00) == 0) break;

    /* Check for packet received - also do this if receiver got to end of the */
    /* list so we make sure we have processed any completed receive buffers   */

    if (status & 0x5000) speedo_rx (devex);

    /* Maybe receiver got to end of the list.  If there are buffers queued, restart receiver. */
    /* Otherwise, just leave it halted, and it will get restarted when a buffer is queued.    */

    if (status & 0x1000) {
      oz_knl_printk ("oz_dev_i82557*: receiver left the ready state, status %4.4x\n", status);
      oz_knl_printk ("oz_dev_i82557*: - rcvbusyq:");
      for (rxbuf = devex -> rcvbusyqf; rxbuf != devex -> rcvbusyql; rxbuf = rxbuf -> next) oz_knl_printk (" %p/%x", rxbuf, rxbuf -> status);
      oz_knl_printk (" %p/%x\n", rxbuf, (rxbuf == NULL) ? 0 : rxbuf -> status);

      devex -> rcvrunning = 0;								/* remember it is halted */
      rxbuf = devex -> rcvbusyqf;							/* see if any buffers on the queue */
      if (rxbuf != NULL) {
        wait_for_ready (ioaddr);							/* ok, wait for it to be ready to accept command */
        oz_hw_outl (virt_to_pci (RXDESL, &(rxbuf -> status)), ioaddr + SCBPointer);	/* set up the address of first buffer on queue */
        oz_hw_outw (RX_START, ioaddr + SCBCmd);						/* tell it to start receiving */
        devex -> rcvrunning = 1;							/* remember it is running now */
      }
    }

    /* User interrupt, Command/Tx unit interrupt, or CU not active. */

    if (status & 0xA400) speedo_tx (devex);
  }

  /* If we have done too much during this interrupt, get out */

  if (boguscnt < 0) {
    oz_knl_printk ("oz_dev_i82557: %s: Too much work at interrupt, status=0x%4.4x.\n", devex -> name, status);
    oz_hw_outl (0xfc00, ioaddr + SCBStatus);				/* clear all interrupt sources */
  }
}

/************************************************************************/
/*									*/
/*  This routine is called by the interrupt handler when a message has 	*/
/*  been received							*/
/*									*/
/************************************************************************/

static void speedo_rx (Devex *devex)

{
  Rxbuf *rxbuf;
  uLong pkt_len, status;

  oz_knl_printk ("oz_dev_i82557 speedo_rx*: devex %p\n", devex);

  /* Loop while there are completed requests */

  while ((rxbuf = devex -> rcvbusyqf) != NULL) {
    status = rxbuf -> status;
    oz_knl_printk ("oz_dev_i82557 speedo_rx*: rxbuf %p, status %x\n", rxbuf, status);
    if (!(status & RX_COMPLETE)) return;
    devex -> rcvbusyqf = rxbuf -> next;					/* completed, unlink from list */
    devex -> receive_buffers_queued --;					/* one less buffer queued to receiver */

    /* Check for buffer overrun */

    if (status & RX_OVERRUN) {
      oz_knl_printk ("oz_dev_i82557: %s: Ethernet frame overran the Rx buffer, status %8.8x!\n", devex -> name, status);
      freercvbuf (rxbuf);
    }

    /* Check for fatal error.  This *should* be impossible. */

    else if (!(status & RX_NOFATAL)) {
      devex -> stats.rx_errors ++;
      oz_knl_printk ("oz_dev_i82557: %s: Anomalous event in speedo_rx(), status %8.8x.\n", devex -> name, status);
      freercvbuf (rxbuf);
    }

    /* Message received ok */

    else {
      pkt_len = rxbuf -> count & 0x3fff;					/* get size of packet received */
      oz_knl_printk ("oz_dev_i82557*: buffer received, %u bytes\n", pkt_len);
      oz_knl_dumpmem (64, rxbuf);
      speedo_queuercvbuf (rxbuf, devex);
#if 000
      ?? process buffer ??
#endif
    }
  }

  /* Queue is empty, clear out last pointer */

  devex -> rcvbusyql = NULL;
}

/************************************************************************/
/*									*/
/*  Some transmits may have completed, so process any that have		*/
/*									*/
/*  We always leave at least one buffer on xmtbusyq so the next one 	*/
/*  will have something to queue to and the controller will have 	*/
/*  something to look at for its next pointer				*/
/*									*/
/************************************************************************/

static void speedo_tx (Devex *devex)

{
  Iopex *iopex;
  Txbuf *txbuf;
  uLong txstat;

  /* All but the last are posted and free off */

  while ((txbuf = devex -> xmtbusyqf) != devex -> xmtbusyql) {	/* check all but the last in the queue */
    txstat = txbuf -> status;					/* status of the entry */
    oz_knl_printk ("oz_dev_i82557*: txbuf %p status %4.4x\n", txbuf, txstat);
    if (!(txstat & 0x8000)) return;				/* done if it still hasn't been processed */
    devex -> xmtbusyqf = txbuf -> next;				/* finished, unlink it */
    iopex = txbuf -> iopex;					/* post io if any associated with it */
    if (iopex != NULL) oz_knl_iodonehi (iopex -> ioop, OZ_SUCCESS, NULL, NULL, NULL);
    freexmtbuf (txbuf);						/* free it off */
  }

  /* The last one is posted but not freed off (because the controller needs to have something to look at for a link to next) */

  txstat = txbuf -> status;					/* get status of last entry */
  oz_knl_printk ("oz_dev_i82557*: txbuf %p status %4.4x\n", txbuf, txstat);
  if (txstat & 0x8000) {
    iopex = txbuf -> iopex;					/* it's done, maybe post io request */
    if (iopex != NULL) {
      txbuf -> iopex = NULL;					/* remember we posted the io for this transmit */
      oz_knl_iodonehi (iopex -> ioop, OZ_SUCCESS, NULL, NULL, NULL); /* post the io request */
    }
  }
}

static Rxbuf *allocrcvbuf (void)

{
  uLong buflen, hi;
  Rxbuf *rxbuf;

  /* Maybe there is a free one we can take */

  hi = oz_hw_smplock_wait (&oz_s_smplock_hi);
  rxbuf = free_rxbufs;
  if (rxbuf != NULL) free_rxbufs = rxbuf -> next;
  oz_hw_smplock_clr (&oz_s_smplock_hi, hi);

  /* If not, allocate a physically contiguous buffer */

  if (rxbuf == NULL) rxbuf = OZ_KNL_PCMALLOC (rxbuf -> buf.data - (uByte *)rxbuf + MAXMSGSIZE + sizeof rxbuf -> pad2);

  return (rxbuf);
}

static void freercvbuf (Rxbuf *rxbuf)

{
  uLong hi;

  hi = oz_hw_smplock_wait (&oz_s_smplock_hi);
  rxbuf -> next = free_rxbufs;
  free_rxbufs = rxbuf;
  oz_hw_smplock_clr (&oz_s_smplock_hi, hi);
}

static Txbuf *allocxmtbuf (void)

{
  uLong hi;
  Txbuf *txbuf;

  /* Maybe there is a free one we can take */

  hi = oz_hw_smplock_wait (&oz_s_smplock_hi);
  txbuf = free_txbufs;
  if (txbuf != NULL) free_txbufs = txbuf -> next;
  oz_hw_smplock_clr (&oz_s_smplock_hi, hi);

  /* If not, allocate a physically contiguous buffer and get */
  /* physical addresses of the descriptor and data areas     */

  if (txbuf == NULL) {
    txbuf = OZ_KNL_PCMALLOC (txbuf -> buf.data - (uByte *)txbuf + MAXMSGSIZE + sizeof txbuf -> pad2);
  }

  txbuf -> iopex = NULL;

  return (txbuf);
}

static void freexmtbuf (Txbuf *txbuf)

{
  Iopex *iopex;
  uLong hi;

  iopex = txbuf -> iopex;

  if (iopex != NULL) oz_knl_iodonehi (iopex -> ioop, OZ_SUCCESS, NULL, NULL, NULL);

  hi = oz_hw_smplock_wait (&oz_s_smplock_hi);
  txbuf -> next = free_txbufs;
  free_txbufs   = txbuf;
  oz_hw_smplock_clr (&oz_s_smplock_hi, hi);
}

/************************************************************************/
/*									*/
/*  Wait for the command unit to be ready to accept a command.		*/
/*  Typically this takes 0 ticks.					*/
/*									*/
/************************************************************************/

static void wait_for_ready (uLong ioaddr)

{
  int wait;
  uLong cmdsts;

  for (wait = 1000; -- wait >= 0;) {
    cmdsts = oz_hw_inl (ioaddr + SCBStatus);
    if ((cmdsts & 0x00FF0000) == 0) return;
    oz_hw_stl_nanowait (100);
  }
  oz_knl_printk ("oz_dev_i82557: %x failed to be ready (command/status %8.8x)\n", ioaddr, cmdsts);
}

/************************************************************************/
/*									*/
/*  Convert virtual address of a buffer to pci address			*/
/*									*/
/*    Input:								*/
/*									*/
/*	size = size of buffer						*/
/*	buff = address of buffer					*/
/*									*/
/*    Output:								*/
/*									*/
/*	virt_to_pci = pci address of the buffer				*/
/*									*/
/************************************************************************/

static uLong virt_to_pci (uLong size, void *buff)

{
  uLong len, ppo;
  OZ_Mempage ppn;

  len = oz_knl_misc_sva2pa (buff, &ppn, &ppo);
  if (len < size) oz_crash ("oz_dev_i82557 virt_to_pci: len %u, size %u", len, size);
  return ((ppn << OZ_HW_L2PAGESIZE) + ppo);
}
