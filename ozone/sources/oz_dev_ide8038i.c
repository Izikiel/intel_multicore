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
/*  PCI IDE disk controller driver using SFF 8038i DMA standard		*/
/*  Will also process generic IDE PIO interface				*/
/*									*/
/*  Creates devices named ide_CD					*/
/*									*/
/*	C = controller: p for primary, s for secondary			*/
/*	D = drive: m for master, s for slave				*/
/*									*/
/*  For ATA devices, the created device is a raw disk drive.		*/
/*									*/
/*  For ATAPI devices, the created device appears as a scsi controller	*/
/*  either processing only scsi-id 0 (master) or 1 (slave).		*/
/*									*/
/*  Extra parameter values:						*/
/*									*/
/*	  nodma : do not use any form of DMA				*/
/*	 noudma : do not use any UDMA mode (regular DMA is ok)		*/
/*	no48bit : do not use 48-bit addressing mode			*/
/*									*/
/************************************************************************/

#include "ozone.h"

#include "oz_dev_disk.h"
#include "oz_dev_isa.h"
#include "oz_dev_pci.h"
#include "oz_dev_scsi.h"
#include "oz_dev_timer.h"
#include "oz_io_disk.h"
#include "oz_io_scsi.h"
#include "oz_knl_devio.h"
#include "oz_knl_hw.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_lowipl.h"
#include "oz_knl_misc.h"
#include "oz_knl_procmode.h"
#include "oz_knl_sdata.h"
#include "oz_knl_section.h"
#include "oz_knl_spte.h"
#include "oz_knl_status.h"
#include "oz_sys_dateconv.h"
#include "oz_sys_xprintf.h"

#define CMD_INB(reg) (*(ctrlr -> inb)) (ctrlr -> atacmd + reg)
#define CMD_INW(reg) (*(ctrlr -> inw)) (ctrlr -> atacmd + reg)
#define CMD_OUTB(val,reg) (*(ctrlr -> outb)) (val, ctrlr -> atacmd + reg)
#define CMD_OUTW(val,reg) (*(ctrlr -> outw)) (val, ctrlr -> atacmd + reg)

#define CTL_INB(reg) (*(ctrlr -> inb)) (ctrlr -> atactl + reg)
#define CTL_INW(reg) (*(ctrlr -> inw)) (ctrlr -> atactl + reg)
#define CTL_OUTB(val,reg) (*(ctrlr -> outb)) (val, ctrlr -> atactl + reg)
#define CTL_OUTW(val,reg) (*(ctrlr -> outw)) (val, ctrlr -> atactl + reg)

#define BMIBA_INB(reg) (*(ctrlr -> inb)) (ctrlr -> bmiba + reg)
#define BMIBA_OUTB(val,reg) (*(ctrlr -> outb)) (val, ctrlr -> bmiba + reg)

#ifdef OZ_HW_TYPE_486
#define DATA_INSW(nw,va) oz_hw486_insw (nw, ctrlr -> atacmd + ATACMD_W_DATA, va)
#define DATA_OUTSW(nw,va) oz_hw486_outsw (nw, va, ctrlr -> atacmd + ATACMD_W_DATA)
#else
#define DATA_INSW(nw,va)  do { Long __nw = nw; uWord *__va = (uWord *)va; while (-- __nw >= 0) *(__va ++) = CMD_INW (ATACMD_W_DATA); } while (0)
#define DATA_OUTSW(nw,va) do { Long __nw = nw; uWord *__va = (uWord *)va; while (-- __nw >= 0) CMD_OUTW (*(__va ++), ATACMD_W_DATA); } while (0)
#endif

#define DISK_BLOCK_SIZE (512)
#define BUFFER_ALIGNMENT (3)
#define L2DISK_BLOCK_SIZE (9)
#define MAX_RW_RETRIES (8)
#define ATAPI_CMDMAX (16)
#define ATA_TIMEOUT 1		/* timeout (in seconds) - don't put parentheses on this */

#define MAX_SEC_COUNT (256)	/* max sectors that the controller can transfer at a time */

#define DMATBLMAX (((MAX_SEC_COUNT*DISK_BLOCK_SIZE)>>OZ_HW_L2PAGESIZE)+1)	// max number of descriptors allowed for a DMA transfer
#define DMATBLSIZ (DMATBLMAX * sizeof (Dmatbl))					// number of bytes required for the DMA table

#define FORTYEIGHTBIT(devex) ((devex) -> totalblocks > 0x0FFFFFFF)		// non-zero if 48-bit mode required for this drive
#define LBAMODE(devex) ((devex) -> ident[ATA_IDENT_W_CAPABILITIES] & 0x200)	// non-zero if drive cabable of LBA addressing

/* "IDENTIFY DRIVE" command return buffer (just stuff that we use) */
/* Note: these are word indicies, not byte offsets                 */

#define ATA_IDENT_W_DEFCYLINDERS   (1)
#define ATA_IDENT_W_DEFTRACKS      (3)
#define ATA_IDENT_W_DEFSECTORS     (6)
#define ATA_IDENT_W20_MODEL       (27)
#define ATA_IDENT_W_RWMULTIPLE    (47)
#define ATA_IDENT_W_CAPABILITIES  (49)
#define ATA_IDENT_W_CURCYLINDERS  (54) /* only use for non-lba mode, ie, IDENT_W2_TOTALBLOCKS is zero */
#define ATA_IDENT_W_CURTRACKS     (55)
#define ATA_IDENT_W_CURSECTORS    (56)
#define ATA_IDENT_W2_TOTALBLOCKS  (60) /* only valid if drive supports LBA, zero if it doesn't */
#define ATA_IDENT_W_MULTIWORDMA   (63)
#define ATA_IDENT_W_PIOMODES      (64)
#define ATA_IDENT_W_MWORDMACYCLE  (66)
#define ATA_IDENT_W_COMMANDSETS1  (83)
#define ATA_IDENT_W_UDMAMODES     (88)
#define ATA_IDENT_W4_TOTALBLOCKS (100)
#define ATA_IDENT__SIZE          (128)

#define ATAPI_IDENT_W20_MODEL    (27)

/* Struct defs */

typedef struct Chnex Chnex;
typedef struct Ctrlr Ctrlr;
typedef struct Devex Devex;
typedef struct Iopex Iopex;

/* Channel extension area (ATAPI channels only) */

struct Chnex { char drive_id;			/* -1: not open, 0: master, 1: slave */
             };

/* Controller data (one per cable) */

struct Ctrlr { OZ_Smplock *smplock;			/* pointer to irq level smp lock */
               Iopex *iopex_qh;				/* requests waiting to be processed, NULL if none */
               Iopex **iopex_qt;
               Iopex *iopex_ip;				/* request currently being processed, NULL if none */
               OZ_Timer *timer;				/* interrupt timeout timer */
               OZ_Lowipl *lowipl;			/* used if controller is found to be busy */
               OZ_Datebin timerwhen;			/* set to date/time when we want the actual timeout */
               int timerqueued;				/* set to 1 when timer request queued, set to 0 when timer request dequeued */
               int requestcount;			/* number of iopex's queued but not yet iodone'd (master only) */
               uLong atacmd;				/* I/O address of ata command registers */
               uLong atactl;				/* I/O address of ata control registers */
               uLong bmiba;				/* Bus Master Interface Base Address */
							/* or ZERO for generic PIO-style interface */

               uByte (*inb) (uLong ioaddr);
               uWord (*inw) (uLong ioaddr);
               void (*outb) (uByte data, uLong ioaddr);
               void (*outw) (uWord data, uLong ioaddr);

               int irqlevel;				/* ISA: irq level */
							/* PCI: int pin (or 8+irq level) */
               int cablesel;				/* 0=primary; 1=secondary */
               const char *suffix;			/* "" for standard controllers, else "_<bus>_<dev>" */
               int chipidx;				/* controller chip type index */
               OZ_Dev_Pci_Conf *pciconf;		/* NULL: ISA controller */
							/* else: controller config register pci address */

               OZ_Dev_Isa_Irq *isairq;			// ISA style interrupt request block
               OZ_Dev_Pci_Irq *pciirq;			// PCI style interrupt request block
               OZ_Dev_Pci_Dma32map *pcidma;		// PCI 32-bit DMA mapping block

               char unitname[OZ_DEVUNIT_NAMESIZE];	/* unit name string temp buffer */
               const char *cable;			/* "primary" or "secondary" */
               Devex *devexs[2];			/* devices attached to controller or NULL if none there */
               uWord seccount[2], tracount[2];		/* default sector and track counts for the possible two devices */
               uWord secbuf[256];			/* temporary sector buffer used for identing drives */
             };

/* Device extension structure */

struct Devex { OZ_Devunit *devunit;		/* devunit pointer */
               const char *name;		/* device unit name (for messages) */
               Ctrlr *ctrlr;			/* pointer to controller struct */
               int usedma;			/* 0: use PIO transfer modes, 1: use DMA transfer modes */

			/* atapi device only (atapimsk != 0) */

               uLong atapimsk;			/* <0>: master is ATAPI; <1>: slave is ATAPI */
               uLong atapiopn;			/* which ATAPI device(s) are open */
               uWord atapiidentword0;		/* atapi ident word 0 */

			/* ata devices only (atapimsk == 0) */

               uLong secpertrk;			/* number of sectors in a track */
               uLong trkpercyl;			/* number of tracks per cylinder (number of heads) */
               uLong secpercyl;			/* number of sectors in a cylinder */
               uLong totalblocks;		/* total number of sectors on the drive */
               uWord ident[ATA_IDENT__SIZE];	/* 'Identify Drive' command results */
               uByte atacmd_read;		/* read/write command code bytes */
               uByte atacmd_read_inh;
               uByte atacmd_write;
               uByte multsize;			/* multiple sector transfer size (or 0 if disabled) */
               char drive_id;			/* 0: this is a master drive, 1: this is a slave drive */
             };

/* I/O operation extension structure */

struct Iopex { Iopex *next;			/* next in ctrlr -> iopex_qh/qt */
               OZ_Ioop *ioop;			/* pointer to io operation node */
               OZ_Procmode procmode;		/* requestor's processor mode */
               Devex *devex;			/* device extension data pointer */
               int writedisk;			/* 0: data transfer from device to memory; 1: data transfer from memory to device */
               uLong status;			/* completion status */
               const OZ_Mempage *phypages;	/* physical page array pointer */
               uLong byteoffs;			/* starting physical page byte offset */
               uLong amount_done;		/* amount already written to / read from disk within the whole request */
               uLong amount_xfrd;		/* amount transferred within this disk command */
               uLong amount_to_xfer;		/* total amount to transfer within this disk command */

			/* ATA style commands only */

               uByte atacmdcode;		/* ATA command code */
               uLong size;			/* buffer size */
               uLong seccount;			/* sector count for transfer */
               OZ_Dbn slbn;			/* starting logical block number */
               int timedout;			/* set if request timed out somewhere */
               int retries;			/* retry counter */
               int ix4kbuk;			/* reading an IX database 4k-byte sized bucket */

			/* ATAPI style commands only */

               OZ_IO_scsi_doiopp doiopp;	/* scsi parameter block */
               uLong dmatblmx;			// number of pages in pcidma table
               OZ_Dev_Pci_Dma32map *pcidma;	// PCI 32-bit DMA mapping block
               uByte atapicmd[ATAPI_CMDMAX];	/* scsi command buffer */
               char drive_id;			/* drive number (0 or 1) */
               uByte atapists;			/* scsi status byte (0=success, 2=error) */
             };

/* Function tables - one for ATA devices, one for ATAPI devices */

static int shutdown (OZ_Devunit *devunit, void *devexv);
static uLong ata_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode, 
                        OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap);
static const OZ_Devfunc ata_functable = { sizeof (Devex), 0, sizeof (Iopex), 0, shutdown, NULL, NULL, 
                                          NULL, NULL, NULL, ata_start, NULL };

static uLong atapi_assign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode);
static int atapi_deassign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv);
static uLong atapi_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode, 
                          OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap);
static const OZ_Devfunc atapi_functable = { sizeof (Devex), sizeof (Chnex), sizeof (Iopex), 0, shutdown, NULL, NULL, 
                                            atapi_assign, atapi_deassign, NULL, atapi_start, NULL };

/* Internal static data */

static int initialized = 0;
static OZ_Devclass  *devclass_disk,  *devclass_scsi;
static OZ_Devdriver *devdriver_disk, *devdriver_scsi;

static Devex *crash_devex = NULL;
static int crash_inprog = 0;
static Iopex crash_iopex;
static OZ_Devunit *crash_devunit = NULL;

/* A block of zeroes to use when writing less than a full block */
/* (Reading partial blocks goes to phypage zero)                */
/* Hopefully using uLong will longword align it                 */

static const uLong zeroes[DISK_BLOCK_SIZE/4];

/* Internal routines */

static int foundpci (void *ivv, uLong didvid, int func, OZ_Dev_Pci_Conf *pciconf, char const *addrsuffix, char const *addrdescrip);
static int config (uLong atacmd, uLong atactl, uLong bmiba, int irqlevel, int cablesel, const char *suffix, int chipidx, OZ_Dev_Pci_Conf *pciconf);
static void enable_drives (Ctrlr *ctrlr);
static void enable_interrupts (Ctrlr *ctrlr);
static int probe_atadrive (Ctrlr *ctrlr, int drive_id);
static void init_atadrive (Ctrlr *ctrlr, int drive_id);
static int probe_atapidrv (Ctrlr *ctrlr, int drive_id);
static void init_atapidrv (Ctrlr *ctrlr, int drive_id);
static void setup_null (Ctrlr *ctrlr);
static void setup_piix4 (Ctrlr *ctrlr);
static void setup_piix4_drive (OZ_Dev_Pci_Conf *pciconf, int cablesel, Ctrlr *ctrlr, Devex *devex);
static void setup_amd768 (Ctrlr *ctrlr);
static void setup_amd768_drive (OZ_Dev_Pci_Conf *pciconf, int cablesel, Ctrlr *ctrlr, Devex *devex);
static void setup_via686 (Ctrlr *ctrlr);
static void setup_via686_drive (OZ_Dev_Pci_Conf *pciconf, int cablesel, Ctrlr *ctrlr, Devex *devex);
static int get_drive_udma_mode (Devex *devex, int maxudmamode);
static int set_drive_udma_mode (Devex *devex, int udmamode);
static uLong waitfornotbusy (void *ctrlrv);
static uLong getidentbuff (void *ctrlrv);
static void ctrlr_fillin (Ctrlr *ctrlr);
static int shutdown (OZ_Devunit *devunit, void *devexv);
static uLong ata_crash (void *dummy, OZ_Dbn lbn, uLong size, OZ_Mempage phypage, uLong offset);
static uLong ata_queuereq (uLong size, OZ_Dbn slbn, const OZ_Mempage *phypages, uLong byteoffs, Iopex *iopex);
static uLong atapi_queuereq (Iopex *iopex);
static void queuereq (Iopex *iopex);
static void startreq (Ctrlr *ctrlr);
static void startdma (Iopex *iopex, uLong xfersize, uLong blocksize);
static void stopdma (Iopex *iopex);
static void recalibrate (Devex *devex);
static uLong checkrecaldone (void *ctrlrv);
static void ctrlrhung (void *ctrlrv, OZ_Lowipl *lowipl);
static void reqtimedout (void *ctrlrv, OZ_Timer *timer);
static void intserv (void *ctrlrv, OZ_Mchargs *mchargs);
static void dma_intserv (Ctrlr *ctrlr, Devex *devex, Iopex *iopex, uByte status, uByte bmisx);
static void pio_ata_intserv (Iopex *iopex, uByte status);
static void pio_atapi_intserv (Iopex *iopex, uByte status);
static int atapi_command_packet (Iopex *iopex, uByte status);
static void do_pio_transfer (int nbytes, Iopex *iopex);
static void ata_finish (Iopex *iopex, uByte status);
static void atapi_finish (Iopex *iopex, uByte status);
static void reqdone (Iopex *iopex);
static void atapi_reqdone (void *iopexv, int finok, uLong *status_r);
static void validaterequestcount (Ctrlr *ctrlr, int line);

/************************************************************************/
/*									*/
/*  I/O space registers							*/
/*									*/
/************************************************************************/

	/* These are biased by Ctrlr field atacmd */

#define ATACMD_W_DATA      (0)	/* RW: data */
#define ATACMD_BR_ERROR    (1)	/* RO: error */
				/*     <0> :  AMNF - address mark not found */
				/*     <1> : TK0NF - track zero not found (recal command) */
				/*     <2> :  ABRT - command aborted due to drive status error */
				/*     <3> :   MCR - media change requested */
				/*     <4> :  IDNF - requested sector's ID field cont not be found */
				/*     <5> :    MC - media changed */
				/*     <6> :   UNC - uncorrectable data error */
				/*     <7> :   BBK - bad block detected */
#define ATACMD_BW_FEATURES (1)	/* WO: features */
#define ATACMD_B_SECCOUNT  (2)	/* RW: sector count */
#define ATACMD_B_LBALOW    (3)	/* RW: sector number / LBA <00:07> */
#define ATACMD_B_SECNUM    (3)
#define ATACMD_B_LBAMID    (4)	/* RW: cylno <00:07> / LBA <08:15> */
#define ATACMD_B_CYL_LO    (4)
#define ATACMD_B_LBAHIGH   (5)	/* RW: cylno <08:15> / LBA <16:23> */
#define ATACMD_B_CYL_HI    (5)
#define ATACMD_B_DRHEAD    (6)	/* RW: */
				/*     <0:3> : head select */
				/*       <4> : drive select */
				/*       <5> : 1 */
				/*       <6> : 0 = CHS mode */
				/*             1 = LBA mode */
				/*       <7> : 1 */
#define ATACMD_BR_STATUS   (7)	/* RO: status */
				/*     <0> :  ERR - an error occurred during the command */
				/*     <1> :  IDX - set once per revolution */
				/*     <2> : CORR - correctable data error occurred */
				/*     <3> :  DRQ - ready to transfer a word of data */
				/*     <4> :  DSC - drive seek complete */
				/*     <5> :  DWF - drive write fault */
				/*     <6> : DRDY - drive ready - able to respond to a command */
				/*     <7> :  BSY - controller busy */
#define ATACMD_BW_COMMAND (7)	/* WO: command */

	/* These are biased by Ctrlr field atactl */

#define ATACTL_BR_ALTSTS (2)
#define ATACTL_BW_DEVCTL (2)
				/*     <0> : must be zero */
				/*     <1> : 0: interrupts enabled; 1: interrupts disabled */
				/*     <2> : software reset when set */
				/*     <3> : must be one */
				/*     <4> : must be zero */
				/*     <5> : must be zero */
				/*     <6> : must be zero */
				/*     <7> : must be zero */

	/* These are biased by Ctrlr field bmiba */

#define BMIBA_B_BMICX   (0)	/* bus master ide command register */
#define BMIBA_B_BMISX   (2)	/* bus master ide status register */
#define BMIBA_L_BMIDTPX (4)	/* bus master ide descriptor table pointer register */

/************************************************************************/
/*									*/
/*  Boot-time initialization routine					*/
/*									*/
/************************************************************************/

/*
  Here is how this all fits together:

  oz_dev_ide8038i_init ()
  {
    scan for PCI controller chips {
      get atacmd, atactl, bmiba from pci config regs
      config (PCI primary)
      config (PCI secondary)
    }
    config (ISA default primary)
    config (ISA default secondary)
  }

    // Check out and configure everything about a given controller and its drives

    config ()
    {
      create ctrlr struct & fill in what we can
      scan both drives on cable {
        if !probe_atadrive, try probe_atapidrv
      }
      software reset controller
      if any drive(s) found, 
        enable_drives
        enable_interrupts
      else, 
        free ctrlr struct
    }

        // See if a given drive exists and is an ATA drive

        probe_atadrive ()
        {
          software reset controller
          select drive to be identified
          send recal command to see if ATA drive is there
          if error, return failure status
          send ATA identify command
          if error, return failure status
          create and fill in devunit/devex struct, link to ctrlr struct
          finish filling in ctrlr struct
          return success status
        }

        // See if a given drive exists and is an ATAPI drive

        probe_atapidrv ()
        {
          software reset controller
          select drive to be identified
          send ATAPI identify command
          if error, return failure status
          create and fill in devunit/devex struct, link to ctrlr struct
          finish filling in ctrlr struct
          return success status
        }

        // Enable the drives that we found

        enable_drives ()
        {
          release the software reset
          call controller-specific setup routine
          for each drive found during probing {
            select it
            call either init_atadrive or init_atapidrv
          }
        }

            // Get ATA drive ready to go

            init_atadrive ()
            {
              send 'init drive params' command
              set up what types of commands to use for read/write (DMA, 48-bit, LBA, multiblock)
              set up autogen to find partitions and mount filesystem
            }

            // Get ATAPI drive ready to go

            init_atapidrv ()
            {
              send 'identify drive' command
              set up autogen to access disk via scsi
            }

        // Enable interrupts for drives on the cable

        enable_interrupts ()
        {
          for each drive found during probing {
            select it
            enable interrupt
          }
        }

*/

/* Table of chips we do special setup for (like UDMA) */

static const struct { uLong didvid;
                      int pcifunc;
                      const char *name;
                      void (*setup) (Ctrlr *ctrlr);
                    } chips[] = { 0x70108086, 1, "Intel PIIX3", setup_null, 
                                  0x71118086, 1, "Intel PIIX4", setup_piix4, 
                                  0x74411022, 1, "AMD 768",     setup_amd768, 
                                  0x05711106, 1, "VIA 686",     setup_via686, 
                                           0, 0, NULL };

typedef struct {
  int defpriused, defsecused, index;
} Initvars;

void oz_dev_ide8038i_init ()

{
  Initvars iv;

  if (initialized) return;

  oz_knl_printk ("oz_dev_ide8038i_init\n");
  initialized    = 1;
  devclass_disk  = oz_knl_devclass_create (OZ_IO_DISK_CLASSNAME, OZ_IO_DISK_BASE, OZ_IO_DISK_MASK, "ide8038i_ata");
  devdriver_disk = oz_knl_devdriver_create (devclass_disk, "ide8038i_ata");
  devclass_scsi  = oz_knl_devclass_create (OZ_IO_SCSI_CLASSNAME, OZ_IO_SCSI_BASE, OZ_IO_SCSI_MASK, "ide8038i_atapi");
  devdriver_scsi = oz_knl_devdriver_create (devclass_scsi, "ide8038i_atapi");

  /* Scan for PCI-style controller chips so we can use (U)DMA functionality */

  iv.defpriused = 0;
  iv.defsecused = 0;
  for (iv.index = 0; chips[iv.index].didvid != 0; iv.index ++) {
    oz_dev_pci_find_didvid (chips[iv.index].didvid, 
                            chips[iv.index].pcifunc, 
                            OZ_DEV_PCI_FINDFLAG_HASBASADR0 	// primary command I/O registers
                          | OZ_DEV_PCI_FINDFLAG_HASBASADR1 	// primary control I/O registers
                          | OZ_DEV_PCI_FINDFLAG_HASBASADR2 	// secondary command I/O regs
                          | OZ_DEV_PCI_FINDFLAG_HASBASADR3 	// secondary control I/O regs
                          | OZ_DEV_PCI_FINDFLAG_HASBASADR4, 	// bus master interface registers
                            foundpci, 
                            &iv);
  }

  /* If defaults not found, try generic ISA PIO-style controller */

  if (!iv.defpriused) config (0x01F0, 0x03F4, 0, 14, 0, "", -1, NULL);		// try config primary controller
  if (!iv.defsecused) config (0x0170, 0x0374, 0, 15, 1, "", -1, NULL);		// try config secondary controller
}

static int foundpci (void *ivv, uLong didvid, int func, OZ_Dev_Pci_Conf *pciconf, char const *addrsuffix, char const *addrdescrip)

{
  char const *suffix;
  Initvars *iv;
  int init, priirqnm, secirqnm, started;
  uByte progintf;
  uLong bmiba, pricmdba, prictlba, seccmdba, secctlba;

  iv = ivv;

  oz_knl_printk ("oz_dev_ide8038i: found %s ide controller at %s\n", chips[iv->index].name, addrdescrip);

  /* Get primary/secondary command/control register base addresses */

  pricmdba  = 0x01F0;								// set up default port numbers
  prictlba  = 0x03F4;
  priirqnm  = 8 + 14;								// 8+irq for oz_dev_pci_irq_alloc to indicate 
										//   specific IRQ number instead of INTPIN number
  seccmdba  = 0x0170;
  secctlba  = 0x0374;
  secirqnm  = 8 + 15;
  suffix    = "";
  progintf  = oz_dev_pci_conf_inb (pciconf, OZ_DEV_PCI_CONF_B_PI);
  if (progintf & 1) {								// see if there are explicit primary ports
										// - piix4 is hardwired to zero
										// - amd768 is programmable
    pricmdba = oz_dev_pci_conf_inl (pciconf, OZ_DEV_PCI_CONF_L_BASADR0);	// if so, read them
    prictlba = oz_dev_pci_conf_inl (pciconf, OZ_DEV_PCI_CONF_L_BASADR1);
    if (((pricmdba & 3) != 1) || ((prictlba & 3) != 1)) {			// they must be in I/O space
      oz_knl_printk ("oz_dev_ide8038i: bad primary reg bus address %X/%X\n", pricmdba, prictlba);
      return (1);
    }
    pricmdba --;								// clear the I/O space bit
    prictlba --;
    suffix = addrsuffix;							// set up suffix string
  } else {
    if (iv -> defpriused) {
      oz_knl_printk ("oz_dev_ide8038i: default primary already defined\n");
      return (1);
    }
    iv -> defpriused = 1;
  }
  if (progintf & 4) {								// see if there are explicit secondary ports
										// - piix4 is hardwired to zero
										// - amd768 is programmable
    seccmdba = oz_dev_pci_conf_inl (pciconf, OZ_DEV_PCI_CONF_L_BASADR2);	// if so, read them
    secctlba = oz_dev_pci_conf_inl (pciconf, OZ_DEV_PCI_CONF_L_BASADR3);
    if (((seccmdba & 3) != 1) || ((secctlba & 3) != 1)) {			// they must be in I/O space
      oz_knl_printk ("oz_dev_ide8038i: bad secondary reg bus address %X/%X\n", seccmdba, secctlba);
      return (1);
    }
    seccmdba --;								// clear the I/O space bit
    secctlba --;
    suffix = addrsuffix;							// set up suffix string
  } else {
    if (iv -> defsecused) {
      oz_knl_printk ("oz_dev_ide8038i: default secondary already defined\n");
      return (1);
    }
    iv -> defsecused = 1;
  }

  bmiba = oz_dev_pci_conf_inl (pciconf, OZ_DEV_PCI_CONF_L_BASADR4);		// get dma controller base I/O address
										// - same on piix4 and amd768
  if ((bmiba & 0xF) != 1) {							// verify the bits we assume to be as indicated really are
										// bmiba<00> = 1 : means the bmiba stuff is in I/O space
    oz_knl_printk ("oz_dev_ide8038i: bad bus master reg bus address %X\n", bmiba);
    return (1);
  }

  started  = config (pricmdba, prictlba, bmiba - 1, priirqnm, 0, suffix, iv -> index, pciconf);
  started |= config (seccmdba, secctlba, bmiba + 7, secirqnm, 1, suffix, iv -> index, pciconf);
  if (started) {
    oz_dev_pci_conf_outw (0x0005, pciconf, OZ_DEV_PCI_CONF_W_PCICMD);		// set bus master enable to enable dma
										// set I/O space enable to access dma registers
    oz_dev_pci_conf_outw (0x3800, pciconf, OZ_DEV_PCI_CONF_W_PCISTS);		// reset error status bits
  }

  return (1);
}

/************************************************************************/
/*									*/
/*  Try to configure drives connected to a particular cable		*/
/*									*/
/*    Input:								*/
/*									*/
/*	atacmd   = command register base address			*/
/*	atactl   = control register base address			*/
/*	bmiba    = 0 : non-DMA controller				*/
/*	        else : dma register base address			*/
/*	irqlevel = irq level number					*/
/*	cablesel = 0 : primary cable					*/
/*	           1 : secondary cable					*/
/*	suffix   = device name suffix or "" for standard device names	*/
/*	chipidx  = index in 'chips[]' array for this controller		*/
/*	pciconf  = NULL : generic non-DMA controller			*/
/*	           else : controller's PCI config space			*/
/*									*/
/************************************************************************/

static int config (uLong atacmd, uLong atactl, uLong bmiba, int irqlevel, int cablesel, const char *suffix, int chipidx, OZ_Dev_Pci_Conf *pciconf)

{
  Ctrlr *ctrlr;
  int drive_id;

  /* Create and fill in initial controller struct */

  ctrlr = OZ_KNL_NPPMALLOC (sizeof *ctrlr);
  memset (ctrlr, 0, sizeof *ctrlr);
  ctrlr -> atacmd    = atacmd;
  ctrlr -> atactl    = atactl;
  ctrlr -> bmiba     = bmiba;
  ctrlr -> irqlevel  = irqlevel;
  ctrlr -> cablesel  = cablesel;
  ctrlr -> suffix    = suffix;
  ctrlr -> chipidx   = chipidx;
  ctrlr -> pciconf   = pciconf;
  if (pciconf != NULL) {
    ctrlr -> inb     = oz_dev_pci_inb;
    ctrlr -> inw     = oz_dev_pci_inw;
    ctrlr -> outb    = oz_dev_pci_outb;
    ctrlr -> outw    = oz_dev_pci_outw;
  } else {
    ctrlr -> inb     = oz_dev_isa_inb;
    ctrlr -> inw     = oz_dev_isa_inw;
    ctrlr -> outb    = oz_dev_isa_outb;
    ctrlr -> outw    = oz_dev_isa_outw;
  }

  ctrlr -> cable     = "primary";
  if (cablesel) ctrlr -> cable = "secondary";

  oz_knl_printk ("oz_dev_ide8038i: probing %s drives (%X/%X/%X)\n", ctrlr -> cable, atacmd, atactl, bmiba);

  /* Probe master (drive_id 0) then slave (drive_id 1) drive */

  for (drive_id = 0; drive_id < 2; drive_id ++) {

    /* Make up unit name string -                */
    /* unitname = ide_<cable><drive><suffix>     */
    /*    cable = p for primary, s for secondary */
    /*    drive = m for master, s for slave      */
    /*   suffix = _bus_dev if non-standard       */

    strcpy (ctrlr -> unitname, "ide_xx");
    ctrlr -> unitname[4] = ctrlr -> cable[0];
    ctrlr -> unitname[5] = drive_id ? 's' : 'm';
    strcat (ctrlr -> unitname, suffix);

    oz_knl_printk ("oz_dev_ide8038i: identifying %s %s drive\n", ctrlr -> cable, drive_id ? "slave" : "master");

    if (!probe_atadrive (ctrlr, drive_id)) probe_atapidrv (ctrlr, drive_id);
  }

  /* If we found any drives, enable them */

  CTL_OUTB (0x0E, ATACTL_BW_DEVCTL);		// software reset the drives, disable interrupts
  if (ctrlr -> iopex_qt != NULL) {		// see if master and/or slave was found
    enable_drives (ctrlr);			// ok, enable it/them
    enable_interrupts (ctrlr);			// also enable interrupts
  } else {
    OZ_KNL_NPPFREE (ctrlr);			// no, free struct
    ctrlr = NULL;				// nothing found
  }

  return (ctrlr != NULL);			// return whether or not we found something
}

/************************************************************************/
/*									*/
/*  The controller has just been software reset.  Enable it then 	*/
/*  enable any drives that were found during probe.			*/
/*									*/
/************************************************************************/

static void enable_drives (Ctrlr *ctrlr)

{
  int drive_id;

  oz_hw_stl_nanowait (50000000);		// wait 50mS
  CTL_OUTB (0x0A, ATACTL_BW_DEVCTL);		// release the drives
  oz_hw_stl_nanowait (50000000);		// wait 50mS

  /* Set up data transfer mode and other controller stuff */

  if (ctrlr -> chipidx >= 0) (*(chips[ctrlr->chipidx].setup)) (ctrlr);

  /* Enable the drives */

  for (drive_id = 0; drive_id < 2; drive_id ++) {
    if (ctrlr -> devexs[drive_id] == NULL) continue;		// see if we found anything in the scan
    CMD_OUTB ((drive_id << 4) | 0xA0, ATACMD_B_DRHEAD);		// select the drive to be initialized
    oz_hw_stl_nanowait (400);
    CTL_OUTB (0x0A, ATACTL_BW_DEVCTL);				// disable interrupts
    if (ctrlr -> devexs[drive_id] -> atapimsk == 0) init_atadrive (ctrlr, drive_id);
                                               else init_atapidrv (ctrlr, drive_id);
  }
}

/* Enable interrupts for drives on the cable */

static void enable_interrupts (Ctrlr *ctrlr)

{
  int drive_id;

  for (drive_id = 0; drive_id < 2; drive_id ++) {
    if (ctrlr -> devexs[drive_id] == NULL) continue;		// see if we found anything in the scan
    CMD_OUTB ((drive_id << 4) | 0xA0, ATACMD_B_DRHEAD);		// select the drive to be enabled
    oz_hw_stl_nanowait (400);
    CTL_OUTB (0x08, ATACTL_BW_DEVCTL);				// enable interrupts
  }
}

/************************************************************************/
/*									*/
/*  Probe for an ATA drive						*/
/*									*/
/************************************************************************/

static int probe_atadrive (Ctrlr *ctrlr, int drive_id)

{
  char *p, unitdesc[OZ_DEVUNIT_DESCSIZE];
  const char *extra;
  Devex *devex;
  int i;
  OZ_Devunit *devunit;
  uByte status;
  uLong sts;

  /* Reset both drives on cable so they can't be screwed up by previous probing */

  CTL_OUTB (0x0E, ATACTL_BW_DEVCTL);				// software reset the drives
  oz_hw_stl_nanowait (50000000);				// wait 50mS
  CTL_OUTB (0x0A, ATACTL_BW_DEVCTL);				// release the drives
  oz_hw_stl_nanowait (50000000);				// wait 50mS
  CMD_OUTB ((drive_id << 4) | 0xA0, ATACMD_B_DRHEAD);		// select the drive to be identified
  oz_hw_stl_nanowait (400);
  CTL_OUTB (0x0A, ATACTL_BW_DEVCTL);				// disable its interrupts

  /* Recal to see if there is an ATA drive there */

  CMD_OUTB (0x10, ATACMD_BW_COMMAND);				// tell it to recalibrate
  oz_hw_stl_nanowait (400);					// give the drive 400nS to start processing the command
  sts = oz_hw_stl_microwait (50000, waitfornotbusy, ctrlr);	// give it 50mS to finish
  if (sts == 0) {
    oz_knl_printk ("oz_dev_ide8038i: - timed out recalibrating\n");
    return (0);
  }
  status = sts;							// check for error
  if ((status & 0xD9) != 0x50) {
    oz_knl_printk ("oz_dev_ide8038i: - recalibrate status 0x%X\n", status);
    if (status & 0x01) {
      status = CMD_INB (ATACMD_BR_ERROR);
      oz_knl_printk ("oz_dev_ide8038i: - error code 0x%X\n", status);
    }
    return (0);
  }

  /* Now find out about the drive */

  CMD_OUTB (0xEC, ATACMD_BW_COMMAND);				// tell it to identify itself
  oz_hw_stl_nanowait (400);					// give the drive 400nS to start processing the command
  memset (ctrlr -> secbuf, 0, sizeof ctrlr -> secbuf);		// zero fill secbuf
  sts = oz_hw_stl_microwait (50000, getidentbuff, ctrlr);	// give it 50mS to finish identifying itself
  if (sts == 0) {
    oz_knl_printk ("oz_dev_ide8038i: - timed out reading identification\n");
    return (0);
  }
  status = sts;							// check for error
  if ((status & 0xD9) != 0x50) {
    oz_knl_printk ("oz_dev_ide8038i: - identify status 0x%X\n", status);
    if (status & 0x01) {
      status = CMD_INB (ATACMD_BR_ERROR);
      oz_knl_printk ("oz_dev_ide8038i: - error code 0x%X\n", status);
    }
    return (0);
  }

  /* Fix up the model name string */

  for (i = ATA_IDENT_W20_MODEL; i < ATA_IDENT_W20_MODEL + 20; i ++) {
    ctrlr -> secbuf[i] = (ctrlr -> secbuf[i] << 8) | (ctrlr -> secbuf[i] >> 8);
  }
  for (p = (char *)(ctrlr -> secbuf + ATA_IDENT_W20_MODEL + 20); *(-- p) == ' ';) {}
  *(++ p) = 0;
  oz_knl_printk ("oz_dev_ide8038i: - model %s\n", ctrlr -> secbuf + ATA_IDENT_W20_MODEL);

  /* Get and validate drive init params */

  ctrlr -> seccount[drive_id] = ctrlr -> secbuf[ATA_IDENT_W_DEFSECTORS];
  if ((ctrlr -> seccount[drive_id] == 0) || (ctrlr -> seccount[drive_id] > 256)) {
    oz_knl_printk ("oz_dev_ide8038i: - can't init drive params because default sector count is %u\n", ctrlr -> seccount[drive_id]);
    return (1);
  }
  ctrlr -> tracount[drive_id] = ctrlr -> secbuf[ATA_IDENT_W_DEFTRACKS];
  if ((ctrlr -> tracount[drive_id] == 0) || (ctrlr -> tracount[drive_id] > 16)) {
    oz_knl_printk ("oz_dev_ide8038i: - can't init drive params because default track count is %u\n", ctrlr -> tracount[drive_id]);
    return (1);
  }

  /* Unit description is the drive model */

  strncpyz (unitdesc, (char *)(ctrlr -> secbuf + ATA_IDENT_W20_MODEL), sizeof unitdesc);

  /* Create the device unit struct */

  devunit = oz_knl_devunit_create (devdriver_disk, ctrlr -> unitname, unitdesc, &ata_functable, 0, oz_s_secattr_sysdev);
  if (devunit == NULL) return (1);

  /* Fill in the device unit extension info */

  ctrlr -> devexs[drive_id] = devex = oz_knl_devunit_ex (devunit);
  memset (devex, 0, sizeof *devex);
  ctrlr_fillin (ctrlr);
  devex -> devunit     = devunit;
  devex -> name        = oz_knl_devunit_devname (devunit);
  devex -> ctrlr       = ctrlr;
  devex -> drive_id    = drive_id;
  devex -> usedma      = (ctrlr -> bmiba != 0) && (ctrlr -> secbuf[ATA_IDENT_W_CAPABILITIES] & 0x0100);
  memcpy (devex -> ident, ctrlr -> secbuf, sizeof devex -> ident);
  devex -> secpertrk   = devex -> ident[ATA_IDENT_W_CURSECTORS];
  devex -> trkpercyl   = devex -> ident[ATA_IDENT_W_CURTRACKS];
  devex -> secpercyl   = (OZ_Dbn)(devex -> ident[ATA_IDENT_W_CURSECTORS]) * (OZ_Dbn)(devex -> ident[ATA_IDENT_W_CURTRACKS]);
  devex -> totalblocks = (OZ_Dbn)(devex -> ident[ATA_IDENT_W_CURCYLINDERS]) * devex -> secpercyl;
  if (LBAMODE (devex)) {
    devex -> totalblocks = devex -> ident[ATA_IDENT_W2_TOTALBLOCKS+0] + (devex -> ident[ATA_IDENT_W2_TOTALBLOCKS+1] << 16);
    if ((devex -> ident[ATA_IDENT_W_COMMANDSETS1] & 0xC400) == 0x4400) {
      extra = oz_knl_misc_getextra (devex -> name, "");
      if (strstr (extra, "no48bit") != NULL) {
        oz_knl_printk ("oz_dev_ide8038i: - >128GB disabled by extra parameter\n");
      } else {
        devex -> totalblocks = 0xFFFFFFFF;
        if ((devex -> ident[ATA_IDENT_W4_TOTALBLOCKS+2] == 0) && (devex -> ident[ATA_IDENT_W4_TOTALBLOCKS+3] == 0)) {
          devex -> totalblocks = devex -> ident[ATA_IDENT_W4_TOTALBLOCKS+0] + (devex -> ident[ATA_IDENT_W4_TOTALBLOCKS+1] << 16);
        }
      }
    }
  }
  oz_knl_printk ("oz_dev_ide8038i: - sec/trk %u, trk/cyl %u, totalblocks %u\n", devex -> secpertrk, devex -> trkpercyl, devex -> totalblocks);
  if (devex -> usedma) {
    extra = oz_knl_misc_getextra (devex -> name, "");
    if (strstr (extra, "nodma") != NULL) {
      devex -> usedma = 0;
      oz_knl_printk ("oz_dev_ide8038i: - DMA disabled by extra parameter\n");
    }
  }
  return (1);
}

/************************************************************************/
/*									*/
/*  Initialize ATA drive						*/
/*									*/
/************************************************************************/

static void init_atadrive (Ctrlr *ctrlr, int drive_id)

{
  Devex *devex;
  int i;
  uByte status;
  uLong sts;

  devex = ctrlr -> devexs[drive_id];

  CMD_OUTB ((drive_id << 4) | 0xA0 | (ctrlr -> tracount[drive_id] - 1), ATACMD_B_DRHEAD); // set number of heads
  CMD_OUTB (ctrlr -> seccount[drive_id], ATACMD_B_SECCOUNT);				// set number of sectors
  CMD_OUTB (0x91, ATACMD_BW_COMMAND);							// initialize drive parameters
  oz_hw_stl_nanowait (400);								// give the drive 400nS to start processing the command
  oz_hw_stl_microwait (50000, waitfornotbusy, ctrlr);					// give it 50mS to finish
  status = CMD_INB (ATACMD_BR_STATUS);
  if ((status & 0xD9) != 0x50) {							// check for error
    oz_knl_printk ("oz_dev_ide8038i: - initialize drive parameters %X %X status 0x%X\n", (drive_id << 4) | 0xA0 | (ctrlr -> tracount[drive_id] - 1), ctrlr -> seccount[drive_id], status);
    if (status & 0x01) {
      status = CMD_INB (ATACMD_BR_ERROR);
      oz_knl_printk ("oz_dev_ide8038i: - error code 0x%X\n", status);
    }
    return;
  }

  /* Maybe it is DMA */

  if (devex -> usedma) {
    devex -> atacmd_read     = 0xC8;
    devex -> atacmd_read_inh = 0xC9;
    devex -> atacmd_write    = 0xCA;
    if (FORTYEIGHTBIT (devex)) {
      devex -> atacmd_read     = 0x25;
      devex -> atacmd_read_inh = 0x25;
      devex -> atacmd_write    = 0x35;
    }
  }

  /* Else, try to set up multiple sector count = one page (for cache transfers) */

  else {
    devex -> multsize        = 0;
    devex -> atacmd_read     = 0x20;
    devex -> atacmd_read_inh = 0x21;
    devex -> atacmd_write    = 0x30;
    if (FORTYEIGHTBIT (devex)) {
      devex -> atacmd_read     = 0x24;
      devex -> atacmd_read_inh = 0x24;
      devex -> atacmd_write    = 0x34;
    }
    i = (1 << OZ_HW_L2PAGESIZE) / DISK_BLOCK_SIZE;
    if ((devex -> ident[ATA_IDENT_W_RWMULTIPLE] & 0xFF) < i) i = devex -> ident[ATA_IDENT_W_RWMULTIPLE] & 0xFF;
    if (i > 1) {
      CMD_OUTB ((drive_id << 4) | 0xA0, ATACMD_B_DRHEAD);			/* select the drive to be set up */
      CMD_OUTB ((1 << OZ_HW_L2PAGESIZE) / DISK_BLOCK_SIZE, ATACMD_B_SECCOUNT);	/* set up number of sectors we want to do at a time */
      CMD_OUTB (0xC6, ATACMD_BW_COMMAND);					/* tell it to set the multiple block count */
      oz_hw_stl_nanowait (400);							/* give the drive 400nS to start processing the command */
      sts = oz_hw_stl_microwait (50000, waitfornotbusy, ctrlr);			/* wait for it to execute (max 50mS) */
      if (sts == 0) sts = CTL_INB (ATACTL_BR_ALTSTS);
      if ((sts & 0xC9) == 0x40) {
        devex -> multsize        = (1 << OZ_HW_L2PAGESIZE) / DISK_BLOCK_SIZE;
        devex -> atacmd_read     = 0xC4;
        devex -> atacmd_read_inh = 0xC4;
        devex -> atacmd_write    = 0xC5;
        if (FORTYEIGHTBIT (devex)) {
          devex -> atacmd_read     = 0x29;
          devex -> atacmd_read_inh = 0x29;
          devex -> atacmd_write    = 0x39;
        }
      } else {
        oz_knl_printk ("oz_dev_ide8038i: - status %2.2X error %2.2X setting multiple sector transfers\n", 
		sts & 0xFF, CMD_INB (ATACMD_BR_ERROR));
      }
    }
  }

  /* Check for partitions */

  oz_knl_devunit_autogen (devex -> devunit, oz_dev_disk_auto, NULL);
}

/************************************************************************/
/*									*/
/*  Probe for an ATAPI drive						*/
/*									*/
/************************************************************************/

static int probe_atapidrv (Ctrlr *ctrlr, int drive_id)

{
  char *p, unitdesc[OZ_DEVUNIT_DESCSIZE];
  const char *extra;
  Devex *devex;
  int i;
  OZ_Devunit *devunit;
  uByte status;
  uLong sts;
  uWord ident0;

  /* Reset both drives on cable so they can't be screwed up by previous probing */

  CTL_OUTB (0x0E, ATACTL_BW_DEVCTL);				// software reset the drives
  oz_hw_stl_nanowait (50000000);				// wait 50mS
  CTL_OUTB (0x0A, ATACTL_BW_DEVCTL);				// release the drives
  oz_hw_stl_nanowait (50000000);				// wait 50mS
  CMD_OUTB ((drive_id << 4) | 0xA0, ATACMD_B_DRHEAD);		// select the drive to be identified
  oz_hw_stl_nanowait (400);
  CTL_OUTB (0x0A, ATACTL_BW_DEVCTL);				// disable its interrupts

  /* Try to tell ATAPI drive to identify itself */

  CMD_OUTB (0xA1, ATACMD_BW_COMMAND);				// tell it to identify itself
  oz_hw_stl_nanowait (400);					// give the drive 400nS to start processing the command
  memset (ctrlr -> secbuf, 0, sizeof ctrlr -> secbuf);		// zero fill secbuf
  status = oz_hw_stl_microwait (50000, getidentbuff, ctrlr);	// read it from drive
  if ((status & 0xC9) != 0x40) {
    oz_knl_printk ("oz_dev_ide8038i: - atapi ident status 0x%X\n", status);
    if (status & 0x01) oz_knl_printk ("oz_dev_ide8038i: - error code 0x%X\n", CMD_INB (ATACMD_BR_ERROR));
    return (0);
  }
  ident0 = ctrlr -> secbuf[0];
  if ((ident0 & 0xC002) != 0x8000) {
    oz_knl_printk ("oz_dev_ide8038i: - atapi ident word[0] %4.4X not supported\n", ident0);
    return (0);
  }

  /* We got something back, so assume it is an ATAPI device -         */
  /* So we set up an device that looks like an scsi controller        */
  /* but it will only process scsi id 0 or 1.  Then we call the scsi  */
  /* routines to create the appropriate class device for the unit.    */

  for (i = ATAPI_IDENT_W20_MODEL; i < ATAPI_IDENT_W20_MODEL + 20; i ++) {
    ctrlr -> secbuf[i] = (ctrlr -> secbuf[i] << 8) | (ctrlr -> secbuf[i] >> 8);
  }
  for (p = (char *)(ctrlr -> secbuf + ATAPI_IDENT_W20_MODEL + 20); *(-- p) == ' ';) {}
  *(++ p) = 0;
  oz_knl_printk ("oz_dev_ide8038i: - atapi %s\n", ctrlr -> secbuf + ATAPI_IDENT_W20_MODEL);
  oz_sys_sprintf (sizeof unitdesc, unitdesc, "access via %s.%u", ctrlr -> unitname, drive_id);
  devunit = oz_knl_devunit_create (devdriver_scsi, ctrlr -> unitname, unitdesc, &atapi_functable, 0, oz_s_secattr_sysdev);
  ctrlr -> devexs[drive_id] = devex = oz_knl_devunit_ex (devunit);
  memset (devex, 0, sizeof *devex);
  ctrlr_fillin (ctrlr);
  devex -> devunit  = devunit;
  devex -> name     = oz_knl_devunit_devname (devunit);
  devex -> ctrlr    = ctrlr;
  devex -> drive_id = drive_id;
  devex -> usedma   = (ctrlr -> bmiba != 0);
  devex -> atapimsk = 1 << drive_id;
  devex -> atapiidentword0 = ctrlr -> secbuf[0];
  if (devex -> usedma) {
    extra = oz_knl_misc_getextra (devex -> name, "");
    if (strstr (extra, "nodma") != NULL) {
      devex -> usedma = 0;
      oz_knl_printk ("oz_dev_ide8038i: - DMA disabled by extra parameter\n");
    }
  }
  return (1);
}

/************************************************************************/
/*									*/
/*  Initialize ATAPI drive						*/
/*									*/
/************************************************************************/

static void init_atapidrv (Ctrlr *ctrlr, int drive_id)

{
  Devex *devex;
  uByte status;
  uWord ident0;

  devex = ctrlr -> devexs[drive_id];

  /* Try to tell ATAPI drive to identify itself again after the reset */
  /* Otherwise, its status is funny                                   */

  CMD_OUTB (0xA1, ATACMD_BW_COMMAND);						// tell it to identify itself
  oz_hw_stl_nanowait (400);							// give the drive 400nS to start processing the command
  memset (ctrlr -> secbuf, 0, sizeof ctrlr -> secbuf);				// zero fill secbuf
  status = oz_hw_stl_microwait (50000, getidentbuff, ctrlr);			// read it from drive
  if ((status & 0xC9) != 0x40) {
    oz_knl_printk ("oz_dev_ide8038i: %s atapi ident status 0x%X\n", devex -> name, status);
    if (status & 0x01) oz_knl_printk ("oz_dev_ide8038i: - error code 0x%X\n", CMD_INB (ATACMD_BR_ERROR));
    return;
  }
  ident0 = ctrlr -> secbuf[0];
  if ((ident0 & 0xC002) != 0x8000) {
    oz_knl_printk ("oz_dev_ide8038i: %s atapi ident word[0] %4.4X not supported\n", devex -> name, ident0);
    return;
  }

  /* Set up autogen routine.  When someone tries to access the <unitname>.<driveid> device, this will create it. */
  /* Can't create it directly via oz_dev_scsi_scan1 now because the scsi class driver probably isn't there yet.  */

  oz_knl_devunit_autogen (devex -> devunit, oz_dev_scsi_auto, NULL);
}

/************************************************************************/
/*									*/
/*  Controller specific setup routines					*/
/*									*/
/************************************************************************/

static void setup_null (Ctrlr *ctrlr)

{ }

/* UDMA mode : Speed (MHz) : Cycle (nS) */
/*         0       ATA-16          120  */
/*         1       ATA-22           90  */
/*         2       ATA-33           60  */
/*         3       ATA-44           45  */
/*         4       ATA-66           30  */
/*         5       ATA-100          20  */

	/* PIIX4 */

static void setup_piix4 (Ctrlr *ctrlr)

{
  if (ctrlr -> devexs[0] != NULL) setup_piix4_drive (ctrlr -> pciconf, ctrlr -> cablesel, ctrlr, ctrlr -> devexs[0]);
  if (ctrlr -> devexs[1] != NULL) setup_piix4_drive (ctrlr -> pciconf, ctrlr -> cablesel, ctrlr, ctrlr -> devexs[1]);
}

static void setup_piix4_drive (OZ_Dev_Pci_Conf *pciconf, int cablesel, Ctrlr *ctrlr, Devex *devex)

{
  int offset, udmamode;
  uByte ub;
  uWord uw;

  offset = cablesel * 2 + devex -> drive_id;	// 0=primary_master; 1=primary_slave; 2=secondary_master; 3=secondary_slave

  /* If drive is capable of UDMA, set up UDMA mode */

  udmamode = get_drive_udma_mode (devex, 2);				// see if drive capable of UDMA mode
  if ((udmamode >= 0) && set_drive_udma_mode (devex, udmamode)) {	// put drive in UDMA mode
    uw  = oz_dev_pci_conf_inw (pciconf, 0x4A);				// set controller's UDMA mode (0, 1, 2)
    uw &= ~(3 << (offset * 4));
    uw |= udmamode << (offset * 4);
    oz_dev_pci_conf_outw (uw, pciconf, 0x4A);
    ub  = oz_dev_pci_conf_inb (pciconf, 0x48);				// set controller's UDMA enable bit
    ub |= 1 << offset;
    oz_dev_pci_conf_outb (ub, pciconf, 0x48);
  }
}

	/* AMD 768 */

static void setup_amd768 (Ctrlr *ctrlr)

{
  if (ctrlr -> devexs[0] != NULL) setup_amd768_drive (ctrlr -> pciconf, ctrlr -> cablesel, ctrlr, ctrlr -> devexs[0]);
  if (ctrlr -> devexs[1] != NULL) setup_amd768_drive (ctrlr -> pciconf, ctrlr -> cablesel, ctrlr, ctrlr -> devexs[1]);
}

static const uByte setup_amd768_udmatiming[6] = { 0xC2, 0xC1, 0xC0, 0xC4, 0xC5, 0xC6 };

static void setup_amd768_drive (OZ_Dev_Pci_Conf *pciconf, int cablesel, Ctrlr *ctrlr, Devex *devex)

{
  int offset, udmamode;
  uByte ub;

  offset = cablesel * 2 + devex -> drive_id;	// 0=primary_master; 1=primary_slave; 2=secondary_master; 3=secondary_slave

  /* If drive is capable of UDMA, set up UDMA mode */

  udmamode = get_drive_udma_mode (devex, 5);
  if (udmamode >= 0) {
    if (udmamode > 2) {							// can't do .gt. mode 2 (ATA33) with slow cable
      ub = oz_dev_pci_conf_inb (pciconf, 0x42);
      if (!(ub & (1 << offset))) {
        udmamode = 2;
        oz_knl_printk ("oz_dev_ide8038i: - but using mode 2 (ATA33) because fast cable not present\n");
      }
    }
    if (set_drive_udma_mode (devex, udmamode)) {			// put drive in UDMA mode
      oz_dev_pci_conf_outb (setup_amd768_udmatiming[udmamode], pciconf, 0x53 - offset); // ok, put controller in UDMA mode
    }
  }
}

	/* VIA 686 */

static void setup_via686 (Ctrlr *ctrlr)

{
  if (ctrlr -> devexs[0] != NULL) setup_via686_drive (ctrlr -> pciconf, ctrlr -> cablesel, ctrlr, ctrlr -> devexs[0]);
  if (ctrlr -> devexs[1] != NULL) setup_via686_drive (ctrlr -> pciconf, ctrlr -> cablesel, ctrlr, ctrlr -> devexs[1]);
}

static void setup_via686_drive (OZ_Dev_Pci_Conf *pciconf, int cablesel, Ctrlr *ctrlr, Devex *devex)

{
  int offset, udmamode;
  uByte ub;

  offset = cablesel * 2 + devex -> drive_id;	// 0=primary_master; 1=primary_slave; 2=secondary_master; 3=secondary_slave

  /* If drive is capable of UDMA, set up UDMA mode */

  udmamode = get_drive_udma_mode (devex, 2);
  if (udmamode >= 0) {
    if (set_drive_udma_mode (devex, udmamode)) {			// ok, try to put drive in UDMA mode
      ub = oz_dev_pci_conf_inb (pciconf, 0x53 - offset) & 0x18;		// success, put controller in UDMA mode
      oz_dev_pci_conf_outb (ub + 0x62 - udmamode, pciconf, 0x53 - offset);
									// <7> = 0: use bits 5:6 to enable
									//       1: enable via 'set features' (didn't work)
									// <6> = 0: UDMA disable
									//       1: UDMA enable
									// <5> = 0: use DMA/PIO
									//       1: use UDMA
									// <4> = reserved
									// <3> = 0: 33MHz pci bus (30nS pcicycletime)
									//       1: 66MHz pci bus (15nS pcicycletime)
									// <2:0> = (udmacycletime/pcicycletime)-2
    }
  }
}


/* See what UDMA mode the drive is capable of */

static int get_drive_udma_mode (Devex *devex, int maxudmamode)

{
  const char *extra;
  int udmamode;

  if (!(devex -> usedma)) return (-1);						// maybe DMA is disabled for the device
  if ((devex -> ident[ATA_IDENT_W_UDMAMODES] & 0xFF) == 0) return (-1);		// see what the drive is capable of
  for (udmamode = 8; !(devex -> ident[ATA_IDENT_W_UDMAMODES] & (1 << (-- udmamode)));) {}
  oz_knl_printk ("oz_dev_ide8038i: %s capable of UDMA mode %d\n", devex -> name, udmamode);
  extra = oz_knl_misc_getextra (devex -> name, "");				// maybe UDMA is disabled for this device
  if (strstr (extra, "noudma") != NULL) {
    oz_knl_printk ("oz_dev_ide8038i: - but UDMA disabled by extra parameter\n");
    return (-1);
  }
  if (udmamode <= maxudmamode) return (udmamode);				// make sure the controller can do it, too
  oz_knl_printk ("oz_dev_ide8038i: - but chip is only capable of UDMA mode %d\n", maxudmamode);
  return (maxudmamode);
}

/* Tell drive to enter UDMA transfer mode */

static int set_drive_udma_mode (Devex *devex, int udmamode)

{
  Ctrlr *ctrlr;
  uByte status;

  ctrlr = devex -> ctrlr;

  CMD_OUTB ((devex -> drive_id << 4) | 0xA0, ATACMD_B_DRHEAD);	// select the drive to be set
  CMD_OUTB (0x03, ATACMD_BW_FEATURES);				// set subcommand = set transfer mode
  CMD_OUTB (0x40 + udmamode, ATACMD_B_SECCOUNT);		// set parameter = UDMA mode 'udmamode'
  CMD_OUTB (0xEF, ATACMD_BW_COMMAND);				// start setting the features
  oz_hw_stl_nanowait (400);					// give the drive 400nS to start processing
  oz_hw_stl_microwait (50000, waitfornotbusy, devex -> ctrlr);	// give it 50mS to finish
  status = CMD_INB (ATACMD_BR_STATUS);				// check for error
  if (!(status & 0x81)) return (1);				// if all ok, return success status
  oz_knl_printk ("oz_dev_ide8038i: - set UDMA mode %d status 0x%X\n", udmamode, status);
  if (status & 0x01) {
    status = CMD_INB (ATACMD_BR_ERROR);
    oz_knl_printk ("oz_dev_ide8038i: - error code 0x%X\n", status);
  }
  return (0);							// some error, return failure status
}

/* Routine used by oz_hw_stl_microwait to wait for not busy */

static uLong waitfornotbusy (void *ctrlrv)

{
  Ctrlr *ctrlr;
  uByte status;

  ctrlr  = ctrlrv;
  status = CMD_INB (ATACMD_BR_STATUS);				// read status and ack interrupt
  if (status & 0x80) return (0);				// if still bussy, continue waiting
  oz_hw_stl_nanowait (400);					// give it a chance to update status
  status = CMD_INB (ATACMD_BR_STATUS);				// get updated status
  return (((uLong)status) | 0x100);				// return non-bussy status
}

/* Routine used by oz_hw_stl_microwait to read the ident buffer */

static uLong getidentbuff (void *ctrlrv)

{
  Ctrlr *ctrlr;
  uByte status;

  ctrlr = ctrlrv;

  status = CMD_INB (ATACMD_BR_STATUS);				// get current status
  if (status & 0x80) return (0);				// if busy, nothing else is valid
  oz_hw_stl_nanowait (400);					// give it a chance to update status
  status = CMD_INB (ATACMD_BR_STATUS);				// get updated status
  if (status & 0x08) {						// see if it has data for us
    DATA_INSW (256, ctrlr -> secbuf);				// if so, get it
    oz_hw_stl_nanowait (400);					// give it a chance to update status
    status = CMD_INB (ATACMD_BR_STATUS);			// get updated status
    if (status & 0x80) return (0);				// if still bussy, keep waiting
  }
  return (((uLong)status) | 0x100);				// not bussy, all done
}

/* Routine to finish filling in a ctrlr struct */

static void ctrlr_fillin (Ctrlr *ctrlr)

{
  if (ctrlr -> iopex_qt != NULL) return;					// only do it once

  ctrlr -> iopex_qt  = &(ctrlr -> iopex_qh);
  if (ctrlr -> pciconf != NULL) {						// connect to the interrupt vector
    ctrlr -> pciirq  = oz_dev_pci_irq_alloc (ctrlr -> pciconf, ctrlr -> irqlevel, intserv, ctrlr);
    ctrlr -> smplock = oz_dev_pci_irq_smplock (ctrlr -> pciirq);
  } else {
    ctrlr -> isairq  = oz_dev_isa_irq_alloc (ctrlr -> irqlevel, intserv, ctrlr);
    ctrlr -> smplock = oz_dev_isa_irq_smplock (ctrlr -> isairq);
  }
  ctrlr -> timer     = oz_knl_timer_alloc ();					// allocate a timer for interrupt timeouts
  ctrlr -> lowipl    = oz_knl_lowipl_alloc ();					// used if controller is found to be busy

  ctrlr -> pcidma    = NULL;							// don't bother with DMA crap if not a DMA controller
  if (ctrlr -> bmiba != 0) {
    ctrlr -> pcidma  = oz_dev_pci_dma32map_alloc (ctrlr -> pciconf, DMATBLMAX, OZ_DEV_PCI_DMAFLAG_64K);
    if (ctrlr -> pcidma == NULL) {
      ctrlr -> bmiba = 0;							// and we were doing so well with it!
      oz_knl_printk ("oz_dev_ide8038i: no memory for dma struct\n");
    }
  }
}

/************************************************************************/
/*									*/
/*  Shutdown device (ATA or ATAPI)					*/
/*									*/
/************************************************************************/

static int shutdown (OZ_Devunit *devunit, void *devexv)

{
  Ctrlr *ctrlr;
  Devex *devex;

  devex = devexv;
  ctrlr = devex -> ctrlr;

  if (ctrlr -> timer != NULL) oz_knl_timer_remove (ctrlr -> timer);	/* cancel any interrupt timer */
  CTL_OUTB (0x0E, ATACTL_BW_DEVCTL);					/* disable interrupts for the drives on this cable and reset drives */
  if (ctrlr -> bmiba != 0) BMIBA_OUTB (0, BMIBA_B_BMICX);		/* abort any dma transfer */
  return (1);
}

/************************************************************************/
/*									*/
/*  Start performing an ATA-style disk i/o function			*/
/*									*/
/************************************************************************/

static uLong ata_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode, 
                        OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap)

{
  Devex *devex;
  Iopex *iopex;
  uLong sts;

  devex = devexv;
  iopex = iopexv;

  iopex -> ioop     = ioop;
  iopex -> devex    = devex;
  iopex -> procmode = procmode;

  iopex -> pcidma   = NULL;
  iopex -> ix4kbuk  = 0;

  /* Process individual functions */

  switch (funcode) {

    /* Set volume valid bit one way or the other (noop for us) */

    case OZ_IO_DISK_SETVOLVALID: {
	/* ?? have it process disk changed status bit */
      return (OZ_SUCCESS);
    }

    /* Write blocks to the disk from virtual memory */

    case OZ_IO_DISK_WRITEBLOCKS: {
      const OZ_Mempage *phypages;
      OZ_IO_disk_writeblocks disk_writeblocks;
      uLong byteoffs;

      /* Lock I/O buffer in memory */

      movc4 (as, ap, sizeof disk_writeblocks, &disk_writeblocks);
      sts = oz_knl_ioop_lockr (ioop, disk_writeblocks.size, disk_writeblocks.buff, &phypages, NULL, &byteoffs);

      /* If that was successful, queue the request to the drive for processing */

      iopex -> atacmdcode = devex -> atacmd_write;
      iopex -> writedisk  = 1;
      if (sts == OZ_SUCCESS) sts = ata_queuereq (disk_writeblocks.size, disk_writeblocks.slbn, phypages, byteoffs, iopex);
      return (sts);
    }

    /* Read blocks from the disk into virtual memory */

    case OZ_IO_DISK_READBLOCKS: {
      const OZ_Mempage *phypages;
      OZ_IO_disk_readblocks disk_readblocks;
      uLong byteoffs;

      /* Lock I/O buffer in memory */

      movc4 (as, ap, sizeof disk_readblocks, &disk_readblocks);
      sts = oz_knl_ioop_lockw (ioop, disk_readblocks.size, disk_readblocks.buff, &phypages, NULL, &byteoffs);

      /* If that was successful, queue the request to the drive for processing */

      iopex -> atacmdcode = (disk_readblocks.inhretries & 1) ? devex -> atacmd_read_inh : devex -> atacmd_read;
      iopex -> writedisk  = 0;
      if (sts == OZ_SUCCESS) sts = ata_queuereq (disk_readblocks.size, disk_readblocks.slbn, phypages, byteoffs, iopex);
      return (sts);
    }

    /* Get info part 1 */

    case OZ_IO_DISK_GETINFO1: {
      OZ_IO_disk_getinfo1 disk_getinfo1;

      if (!OZ_HW_WRITABLE (as, ap, procmode)) return (OZ_ACCVIO);
      memset (&disk_getinfo1, 0, sizeof disk_getinfo1);
      disk_getinfo1.blocksize   = DISK_BLOCK_SIZE;
      disk_getinfo1.totalblocks = devex -> totalblocks;
      disk_getinfo1.secpertrk   = devex -> secpertrk;
      disk_getinfo1.trkpercyl   = devex -> trkpercyl;
      disk_getinfo1.cylinders   = devex -> totalblocks / devex -> secpercyl;
      disk_getinfo1.bufalign    = BUFFER_ALIGNMENT;
      movc4 (sizeof disk_getinfo1, &disk_getinfo1, as, ap);
      return (OZ_SUCCESS);
    }

    /* Write blocks to the disk from physical pages (kernel only) */

    case OZ_IO_DISK_WRITEPAGES: {
      OZ_IO_disk_writepages disk_writepages;

      if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);
      movc4 (as, ap, sizeof disk_writepages, &disk_writepages);

      /* Queue the request to the drive for processing */

      iopex -> atacmdcode = devex -> atacmd_write;
      iopex -> writedisk  = 1;
      sts = ata_queuereq (disk_writepages.size, disk_writepages.slbn, disk_writepages.pages, disk_writepages.offset, iopex);
      return (sts);
    }

    /* Read blocks from the disk into physical pages (kernel only) */

    case OZ_IO_DISK_READPAGES: {
      OZ_IO_disk_readpages disk_readpages;

      if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);
      movc4 (as, ap, sizeof disk_readpages, &disk_readpages);

      /* Queue the request to the drive for processing */

      iopex -> atacmdcode = (disk_readpages.inhretries & 1) ? devex -> atacmd_read_inh : devex -> atacmd_read;
      iopex -> writedisk  = 0;
      iopex -> ix4kbuk    = disk_readpages.ix4kbuk;
      sts = ata_queuereq (disk_readpages.size, disk_readpages.slbn, disk_readpages.pages, disk_readpages.offset, iopex);
      return (sts);
    }

    /* Set crash dump device */

    case OZ_IO_DISK_CRASH: {
      if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);		/* caller must be in kernel mode */
      if (crash_devunit != NULL) {					/* get rid of old crash stuff, if any */
        oz_knl_devunit_increfc (crash_devunit, -1);
        crash_devex   = NULL;
        crash_devunit = NULL;
      }
      if (ap != NULL) {
        if (as != sizeof (OZ_IO_disk_crash)) return (OZ_BADBUFFERSIZE);	/* param block must be exact size */
        ((OZ_IO_disk_crash *)ap) -> crashentry = ata_crash;		/* return pointer to crash routine */
        ((OZ_IO_disk_crash *)ap) -> crashparam = NULL;			/* we don't require a parameter */
        ((OZ_IO_disk_crash *)ap) -> blocksize  = DISK_BLOCK_SIZE;	/* tell them our blocksize */
        crash_devex   = devex;						/* save the device we will write to */
        crash_devunit = devunit;
        oz_knl_devunit_increfc (crash_devunit, 1);			/* make sure it doesn't get deleted */
      }
      return (OZ_SUCCESS);
    }

    /* Who knows what */

    default: {
      return (OZ_BADIOFUNC);
    }
  }
}

/************************************************************************/
/*									*/
/*  ATA-style crash dump routine - write logical blocks with 		*/
/*  interrupts disabled							*/
/*									*/
/*    Input:								*/
/*									*/
/*	lbn     = block to start writing at				*/
/*	size    = number of bytes to write (multiple of blocksize)	*/
/*	phypage = physical page to start writing from			*/
/*	offset  = offset in first physical page				*/
/*									*/
/*    Output:								*/
/*									*/
/*	ata_crash = OZ_SUCCESS : successful				*/
/*	                  else : error status				*/
/*									*/
/************************************************************************/

static uLong ata_crash (void *dummy, OZ_Dbn lbn, uLong size, OZ_Mempage phypage, uLong offset)

{
  Ctrlr *ctrlr;
  OZ_Datebin now;
  uByte status;
  uLong sts, wsize;

  if ((size | offset) & BUFFER_ALIGNMENT) return (OZ_UNALIGNEDBUFF);

  ctrlr = crash_devex -> ctrlr;

  if (!crash_inprog) {
    recalibrate (crash_devex);			/* reset drive */
    crash_inprog = 1;				/* ... the first time only */
  }

  ctrlr -> iopex_ip    = NULL;			/* make sure the 'inprogress' queue is empty */
  ctrlr -> iopex_qh    = NULL;			/* make sure the 'pending' queue is empty */
  ctrlr -> iopex_qt    = &(ctrlr -> iopex_qh);
  ctrlr -> smplock     = NULL;			/* we can't use the smp lock anymore */
  ctrlr -> timer       = NULL;			/* make sure we don't use the timer anymore */
  ctrlr -> timerqueued = 0;

  /* Repeat as long as there is stuff to write */

  while (size > 0) {

    /* See how much we can write (up to end of current physical page) */

    wsize = (1 << OZ_HW_L2PAGESIZE) - offset;
    if (wsize > size) wsize = size;

    /* Queue write request and start processing it - since the queue is empty, it should start right away */

    ctrlr -> requestcount  = 0;
    memset (&crash_iopex, 0, sizeof crash_iopex);
    crash_iopex.procmode   = OZ_PROCMODE_KNL;
    crash_iopex.devex      = crash_devex;
    crash_iopex.status     = OZ_PENDING;
    crash_iopex.writedisk  = 1;
    crash_iopex.atacmdcode = crash_devex -> atacmd_write;
    sts = ata_queuereq (wsize, lbn, &phypage, offset, &crash_iopex);
    if (sts == OZ_STARTED) {

      /* Now keep calling the interrupt service routine until the request completes */

      while (ctrlr -> requestcount != 0) {
        status = CMD_INB (ATACMD_BR_STATUS);	/* get drive's status byte */
        if (status & 0x80) goto itsbussy;				// - if busy set, continue waiting
        if (status & 0x01) goto itsdone;				// - if error set, it's all done
        if (!(status & 0x08)) goto itsdone;				// - if it doesn't want data, it's done
        if (crash_devex -> usedma) goto itsbussy;			// - if dma transfer, continue waiting
itsdone:
        intserv (ctrlr, NULL);						/* ... call interrupt service routine */
        continue;
itsbussy:
        now = oz_hw_tod_getnow ();					/* still busy, get current time */
        if (OZ_HW_DATEBIN_CMP (ctrlr -> timerwhen, now) <= 0) {		/* see if timer has expired */
          ctrlr -> timerqueued = 0;					/* if so, say fake timer no longer queued */
          intserv (ctrlr, NULL);					/* ... and call interrupt service routine */
        }
      }
      sts = crash_iopex.status;						/* get completion status */
    }

    /* Check the completion status */

    if (sts != OZ_SUCCESS) {
      oz_knl_printk ("oz_dev_ide8038i crash: error %u writing to lbn %u\n", sts, lbn);
      return (sts);
    }

    /* Ok, on to next physical page */

    size    -= wsize;
    offset  += wsize;
    phypage += offset >> OZ_HW_L2PAGESIZE;
    offset  &= (1 << OZ_HW_L2PAGESIZE) - 1;
    lbn     += wsize / DISK_BLOCK_SIZE;
  }

  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Queue ATA-style I/O request						*/
/*									*/
/*    Input:								*/
/*									*/
/*	size = size of transfer in bytes				*/
/*	slbn = starting logical block number				*/
/*	phypages = pointer to array of physical page numbers		*/
/*	byteoffs = byte offset in first physical page			*/
/*	iopex = iopex block to use for operation			*/
/*	iopex -> writedisk  = set if writing to disk			*/
/*	iopex -> atacmdcode = read/write ATA command code byte		*/
/*									*/
/*    Output:								*/
/*									*/
/*	ata_queuereq = OZ_STARTED : requeust queued to disk drive and 	*/
/*	                           drive started if it was idle		*/
/*	                    else : error status				*/
/*									*/
/************************************************************************/

static uLong ata_queuereq (uLong size, OZ_Dbn slbn, const OZ_Mempage *phypages, uLong byteoffs, Iopex *iopex)

{
  Ctrlr *ctrlr;
  Devex *devex;
  uLong hd;
  OZ_Dbn elbn;

  devex = iopex -> devex;
  ctrlr = devex -> ctrlr;

  /* If no buffer, instant success */

  if (size == 0) return (OZ_SUCCESS);

  /* The buffer must be long aligned for DMA since it is done a long at a time */

  if ((size | byteoffs) & BUFFER_ALIGNMENT) return (OZ_UNALIGNEDBUFF);

  /* Make sure request doesn't run off end of disk */

  elbn = (size + DISK_BLOCK_SIZE - 1) / DISK_BLOCK_SIZE + slbn;
  if (elbn < slbn) return (OZ_BADBLOCKNUMBER);
  if (elbn > devex -> totalblocks) return (OZ_BADBLOCKNUMBER);

  /* Make up a read/write request struct */

  iopex -> size        = size;				/* save buffer size */
  iopex -> slbn        = slbn;				/* save starting logical block number */
  iopex -> phypages    = phypages;			/* save physical page array pointer */
  iopex -> byteoffs    = byteoffs;			/* save starting physical page byte offset */
  iopex -> timedout    = 0;				/* hasn't timed out yet */
  iopex -> amount_done = 0;				/* nothing has been transferred yet */
  iopex -> drive_id    = devex -> drive_id;		/* set up drive id number */
  iopex -> retries     = MAX_RW_RETRIES;		/* init retry counter */
  if ((iopex -> writedisk | iopex -> atacmdcode) & 1) iopex -> retries = 1; /* (no retries for writes or if told so) */

  if (ctrlr -> smplock != NULL) {
    hd = oz_hw_smplock_wait (ctrlr -> smplock);		/* inhibit hardware interrupts */
  }
  validaterequestcount (ctrlr, __LINE__);		/* make sure queues are ok as they stand */
  ctrlr -> requestcount ++;				/* ok, there is now one more request */
  queuereq (iopex);					/* queue the request to the controller */
  validaterequestcount (ctrlr, __LINE__);		/* make sure queues are ok before releasing lock */
  if (ctrlr -> smplock != NULL) {
    oz_hw_smplock_clr (ctrlr -> smplock, hd);		/* restore hardware interrupts */
  }

  return (OZ_STARTED);					/* the request has been started */
}

/************************************************************************/
/*									*/
/*  Assign and deassign channel to ATAPI controller			*/
/*									*/
/************************************************************************/

static uLong atapi_assign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode)

{
  ((Chnex *)chnexv) -> drive_id = -1;						// init chnex saying no scsi device opened
  return (OZ_SUCCESS);								// always successful
}

static int atapi_deassign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv)

{
  char drive_id;
  Chnex *chnex;

  chnex = chnexv;

  drive_id = chnex -> drive_id;							// see if anything open on channel
  if (drive_id >= 0) {
    chnex -> drive_id = -1;							// if so, say it is closed now
    oz_hw_atomic_and_long (&(((Devex *)devexv) -> atapiopn), ~ (1 << drive_id)); // let someone else open it now
  }
  return (0);
}

/************************************************************************/
/*									*/
/*  Start performing an ATAPI i/o function				*/
/*									*/
/************************************************************************/

static uLong atapi_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode, 
                          OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap)

{
  Chnex *chnex;
  Devex *devex;
  Iopex *iopex;
  uLong sts;

  chnex = chnexv;
  devex = devexv;
  iopex = iopexv;

  iopex -> ioop     = ioop;
  iopex -> devex    = devex;
  iopex -> procmode = procmode;

  iopex -> pcidma   = NULL;

  /* Process individual functions */

  switch (funcode) {

    /* Open one of the drives on this channel */

    case OZ_IO_SCSI_OPEN: {
      Long drive_id_mask;
      OZ_IO_scsi_open scsi_open;
      uByte drive_id;

      movc4 (as, ap, sizeof scsi_open, &scsi_open);
      drive_id = scsi_open.scsi_id;					// get drive id (0=master, 1=slave)
      if (drive_id > 1) return (OZ_BADSCSIID);				// allow only 0 or 1
      drive_id_mask = (1 << drive_id);					// make a mask for it
      if (!(devex -> atapimsk & drive_id_mask)) return (OZ_BADSCSIID);	// make sure that it was found to be ATAPI in init routine
      if (chnex -> drive_id >= 0) return (OZ_FILEALREADYOPEN);		// make sure nothing already open on this channel
      if (oz_hw_atomic_or_long (&(devex -> atapiopn), drive_id_mask) & drive_id_mask) return (OZ_ACCONFLICT); // mark it open now
      chnex -> drive_id = drive_id;					// remember which one we opened on this channel
      return (OZ_SUCCESS);						// successful
    }

    /* Perform an scsi command on the drive, with virtual buffer address */

    case OZ_IO_SCSI_DOIO: {
      OZ_IO_scsi_doio scsi_doio;
      uLong sts;

      movc4 (as, ap, sizeof scsi_doio, &scsi_doio);
      sts = oz_dev_scsi_cvtdoio2pp (ioop, procmode, &scsi_doio, &(iopex -> doiopp));
      iopex -> drive_id = chnex -> drive_id;
      if (sts == OZ_SUCCESS) sts = atapi_queuereq (iopex);
      return (sts);
    }

    /* Perform an scsi command on the drive, with physical buffer address (kernel mode only) */

    case OZ_IO_SCSI_DOIOPP: {
      if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);
      movc4 (as, ap, sizeof iopex -> doiopp, &(iopex -> doiopp));
      iopex -> drive_id = chnex -> drive_id;
      sts = atapi_queuereq (iopex);
      return (sts);
    }

    /* Get scsi controller info */

    case OZ_IO_SCSI_GETINFO1: {
      OZ_IO_scsi_getinfo1 scsi_getinfo1;

      if (!OZ_HW_WRITABLE (as, ap, procmode)) return (OZ_ACCVIO);
      memset (&scsi_getinfo1, 0, sizeof scsi_getinfo1);		// clear stuff we don't know about
      scsi_getinfo1.max_scsi_id  = 2;				// allow only scsi id 0 and 1
      scsi_getinfo1.ctrl_scsi_id = devex -> atapimsk & 1;	// controller is whichever the device isn't
								//   atapimsk 1 -> ctrl_scsi_id 1
								//   atapimsk 2 -> ctrl_scsi_id 0
      scsi_getinfo1.open_scsi_id = chnex -> drive_id;		// tell caller what scsi-id is open on channel (-1 if closed)
      scsi_getinfo1.open_width   = 1;				// we're always 16-bits wide
      movc4 (sizeof scsi_getinfo1, &scsi_getinfo1, as, ap);	// copy info back to caller
      return (OZ_SUCCESS);
    }

    /* We don't support these, so let them fall through to the error */

    case OZ_IO_SCSI_RESET:
    case OZ_IO_SCSI_CRASH:

    /* Who knows what */

    default: {
      return (OZ_BADIOFUNC);
    }
  }
}

/************************************************************************/
/*									*/
/*  Queue ATAPI-style request to the controller				*/
/*									*/
/*    Input:								*/
/*									*/
/*	iopex = request to be queued					*/
/*	iopex -> ioop     = request's ioop				*/
/*	iopex -> procmode = requestor's processor mode			*/
/*	iopex -> devex    = device (atapi controller device)		*/
/*	iopex -> doiopp   = function parameter block			*/
/*	iopex -> drive_id = 0: master, 1: slave				*/
/*									*/
/*    Output:								*/
/*									*/
/*	atapi_queuereq = queuing status					*/
/*									*/
/************************************************************************/

static uLong atapi_queuereq (Iopex *iopex)

{
  Ctrlr *ctrlr;
  Devex *devex;
  OZ_Mempage ppn;
  uLong cl, hd, ppo, sts;

  devex = iopex -> devex;
  ctrlr = devex -> ctrlr;

  if (iopex -> drive_id > 1) return (OZ_BADSCSIID);

  /* Copy command into atapicmd and zero pad */

  cl = iopex -> doiopp.cmdlen;									// get length of supplied command
  hd = 12;											// maybe device takes 12 byte packets
  if (devex -> atapiidentword0 & 1) hd = 16;							// maybe device takes 16 byte packets
  if (cl > hd) return (OZ_BUFFEROVF);								// if command too long, return error
  sts = oz_knl_section_uget (iopex -> procmode, cl, iopex -> doiopp.cmdbuf, iopex -> atapicmd);	// ok, copy in the command
  if (sts != OZ_SUCCESS) return (sts);
  memset (iopex -> atapicmd + cl, 0, hd - cl);							// zero pad it

  /* The data buffer must be long aligned for DMA since it is done a long at a time */

  if ((iopex -> doiopp.datasize | iopex -> doiopp.databyteoffs) & BUFFER_ALIGNMENT) return (OZ_UNALIGNEDBUFF);

  /* If transfer request is large, maybe we need a big dmatbl */

  iopex -> doiopp.dataphypages += iopex -> doiopp.databyteoffs >> OZ_HW_L2PAGESIZE;
  iopex -> doiopp.databyteoffs %= 1 << OZ_HW_L2PAGESIZE;

  iopex -> dmatblmx = ((iopex -> doiopp.datasize + iopex -> doiopp.databyteoffs) >> OZ_HW_L2PAGESIZE) + 1;
  if (devex -> usedma && (iopex -> dmatblmx >= DMATBLMAX)) {
    iopex -> pcidma = oz_dev_pci_dma32map_alloc (ctrlr -> pciconf, iopex -> dmatblmx, OZ_DEV_PCI_DMAFLAG_64K);
    if (iopex -> pcidma == NULL) return (OZ_EXQUOTANPP);
  }

  /* Put request on queue */

  iopex -> writedisk   = ((iopex -> doiopp.optflags & OZ_IO_SCSI_OPTFLAG_WRITE) != 0);		// set whether or not we are writing to device
  iopex -> timedout    = 0;									// hasn't timed out yet
  iopex -> retries     = 0;									// ATAPI's never retry   
  iopex -> atapists    = 0;									// assume successful
  iopex -> size        = iopex -> doiopp.datasize;						// for timeout's error message

  iopex -> byteoffs    = iopex -> doiopp.databyteoffs;						// get offset in first page for data transfer
  iopex -> phypages    = iopex -> doiopp.dataphypages;						// get pointer to array of physical page numbers
  iopex -> amount_xfrd = 0;									// haven't transferred anything yet
  iopex -> amount_done = 0;
  iopex -> slbn        = 0;									// make all ATAPI request go in order received

  hd = oz_hw_smplock_wait (ctrlr -> smplock);							// inhibit hardware interrupts
  validaterequestcount (ctrlr, __LINE__);							// make sure queues are ok as they stand
  ctrlr -> requestcount ++;									// ok, there is now one more request
  queuereq (iopex);										// queue the request to the controller
  validaterequestcount (ctrlr, __LINE__);							// make sure queues are ok before releasing lock
  oz_hw_smplock_clr (ctrlr -> smplock, hd);							// restore hardware interrupts

  return (OZ_STARTED);										// the request has been started
}

/************************************************************************/
/*									*/
/*  This routine (re-)queues a request to the controller		*/
/*									*/
/*    Input:								*/
/*									*/
/*	iopex = pointer to request to be queued				*/
/*	smplock = drives irq smplock					*/
/*									*/
/*    Output:								*/
/*									*/
/*	request queued to controller					*/
/*	drive started if not already busy				*/
/*									*/
/************************************************************************/

static void queuereq (Iopex *iopex)

{
  Ctrlr *ctrlr;
  Iopex **liopex, *xiopex;

  iopex -> status = OZ_PENDING;									/* reset status (for crash routine) */

  /* Point to the controller struct */

  ctrlr = iopex -> devex -> ctrlr;

  /* Link it on to end of controller's request queue */

  *(ctrlr -> iopex_qt) = iopex;
  iopex -> next = NULL;
  ctrlr -> iopex_qt = &(iopex -> next);

  validaterequestcount (ctrlr, __LINE__);

  /* If there is no request being processed, start it going */

  if ((ctrlr -> iopex_ip == NULL) && (ctrlr -> iopex_qh == iopex)) startreq (ctrlr);
}

/************************************************************************/
/*									*/
/*  Start processing the request that's on top of queue			*/
/*									*/
/*    Input:								*/
/*									*/
/*	ctrlr = pointer to controller					*/
/*	ctrlr -> iopex_qh/qt = read/write request queue			*/
/*	ctrlr -> iopex_ip = assumed to be NULL				*/
/*									*/
/*	smplock = device's irq smp lock					*/
/*									*/
/*    Output:								*/
/*									*/
/*	ctrlr -> iopex_ip = filled in with top request			*/
/*	ctrlr -> iopex_qh/qt = top request removed			*/
/*	disk operation started						*/
/*									*/
/************************************************************************/

static void startreq (Ctrlr *ctrlr)

{
  Devex *devex;
  int i, j;
  Iopex *iopex;
  OZ_Datebin now, delta;
  OZ_Dbn lba;
  uByte status;
  uLong bytecount, seccount, sts;

  /* Dequeue the request that's on the top */

dequeue:
  validaterequestcount (ctrlr, __LINE__);
  iopex = ctrlr -> iopex_qh;							/* get pointer to top request */
  if (iopex == NULL) return;							/* just return if queue was empty */
  ctrlr -> iopex_ip = iopex;							/* got one, mark it 'in progress' */
  if ((ctrlr -> iopex_qh = iopex -> next) == NULL) ctrlr -> iopex_qt = &(ctrlr -> iopex_qh); /* unlink it from queue */
  devex = iopex -> devex;							/* get which device the request is for */
  validaterequestcount (ctrlr, __LINE__);

  /* Make sure the drive is ready to accept a new command */

  if (ctrlr -> bmiba != 0) BMIBA_OUTB (0, BMIBA_B_BMICX);			/* shut off any old dma stuff */
  CMD_OUTB ((iopex -> drive_id << 4) | 0xE0, ATACMD_B_DRHEAD);			/* select the drive and LBA mode */
  status = CMD_INB (ATACMD_BR_STATUS);						/* make sure drive is ready and not requesting data */
  if ((status & 0xC8) != 0x40) {
    oz_hw_stl_nanowait (400);
    status = CMD_INB (ATACMD_BR_STATUS);					/* give it a second chance (to clear error bits) */
    if ((status & 0xC8) != 0x40) {
      oz_knl_printk ("oz_dev_ide8038i: controller hung, status 0x%X, error 0x%X\n", status, CMD_INB (ATACMD_BR_ERROR));
      if (crash_inprog) {
        iopex -> status = OZ_IOFAILED;
        reqdone (iopex);
        goto dequeue;
      }
      CTL_OUTB (0x0E, ATACTL_BW_DEVCTL);					// start a software reset of drives on the cable
      validaterequestcount (ctrlr, __LINE__);
      iopex = ctrlr -> iopex_ip;						// push hung request back on queue so in case we ...
      ctrlr -> iopex_ip = NULL;							// ... get an int, isr won't think it's in progress
      iopex -> next = ctrlr -> iopex_qh;
      ctrlr -> iopex_qh = iopex;
      if (iopex -> next == NULL) ctrlr -> iopex_qt = &(iopex -> next);
      validaterequestcount (ctrlr, __LINE__);
      oz_knl_lowipl_call (ctrlr -> lowipl, ctrlrhung, ctrlr);			/* unstick it via lowipl routine */
      ctrlr -> lowipl = NULL;							/* don't use lowipl again until reset complete */
      return;
    }
  }

  /* If ATAPI request, do it differently */

  if (devex -> atapimsk != 0) goto start_atapi;

  /*********************/
  /* Start ATA request */
  /*********************/

  /* Determine number of sectors we have yet to transfer */

  if ((iopex -> amount_done % DISK_BLOCK_SIZE) != 0) oz_crash ("oz_dev_ide8038i startreq: amount_done %u not multiple of block size", iopex -> amount_done);
  seccount = (iopex -> size - iopex -> amount_done + DISK_BLOCK_SIZE - 1) / DISK_BLOCK_SIZE;	/* determine number of sectors to process */
  if (seccount > MAX_SEC_COUNT) seccount = MAX_SEC_COUNT;					/* max number of sectors at a time */
  iopex -> seccount = seccount;
  iopex -> amount_to_xfer = seccount * DISK_BLOCK_SIZE;

  /* Calculate starting lbn */

  lba = iopex -> slbn + iopex -> amount_done / DISK_BLOCK_SIZE;

  /* Start dma controller */

  iopex -> amount_xfrd = 0;							/* haven't transferred anything yet (in case of PIO) */
  if (devex -> usedma) startdma (iopex, iopex -> amount_to_xfer, DISK_BLOCK_SIZE); /* set up dma to transfer the whole thing */

  /* Fill in device registers to start the request */

  if (FORTYEIGHTBIT (devex)) {
    CMD_OUTB ((devex -> drive_id << 4) | 0xE0, ATACMD_B_DRHEAD);		/* select the drive, LBA mode */
    CMD_OUTB (seccount >> 8, ATACMD_B_SECCOUNT);				/* store sector count<15:08> */
    CMD_OUTB     (lba >> 24, ATACMD_B_LBALOW);					/* store lba<31:24> */
    CMD_OUTB             (0, ATACMD_B_LBAMID);					/* store lba<39:32> */
    CMD_OUTB             (0, ATACMD_B_LBAHIGH);					/* store lba<47:40> */
    CMD_OUTB      (seccount, ATACMD_B_SECCOUNT);				/* store sector count<07:00> */
    CMD_OUTB           (lba, ATACMD_B_LBALOW);					/* store lba<07:00> */
    CMD_OUTB      (lba >> 8, ATACMD_B_LBAMID);					/* store lba<15:08> */
    CMD_OUTB     (lba >> 16, ATACMD_B_LBAHIGH);					/* store lba<23:16> */
  } else if (LBAMODE (devex)) {
    CMD_OUTB ((devex -> drive_id << 4) | (lba >> 24) | 0xE0, ATACMD_B_DRHEAD);	/* select the drive, lba<27:24>, LBA mode */
    CMD_OUTB         (0, ATACMD_BW_FEATURES);					//??
    CMD_OUTB  (seccount, ATACMD_B_SECCOUNT);					/* store sector count (let it wrap to 0 for 256) */
    CMD_OUTB       (lba, ATACMD_B_LBALOW);					/* store lba<07:00> */
    CMD_OUTB  (lba >> 8, ATACMD_B_LBAMID);					/* store lba<15:08> */
    CMD_OUTB (lba >> 16, ATACMD_B_LBAHIGH);					/* store lba<23:16> */
  } else {
    uLong cylinder, sector, track;

    cylinder =  lba / devex -> secpercyl;
    track    = (lba / devex -> secpertrk) % devex -> trkpercyl;
    sector   = (lba % devex -> secpertrk) + 1;

    CMD_OUTB ((devex -> drive_id << 4) | track | 0xA0, ATACMD_B_DRHEAD);	/* select the drive, CHS mode */
    CMD_OUTB      (seccount, ATACMD_B_SECCOUNT);				/* store sector count (let it wrap to 0 for 256) */
    CMD_OUTB      (cylinder, ATACMD_B_CYL_LO);					/* store lba<07:00> */
    CMD_OUTB (cylinder >> 8, ATACMD_B_CYL_HI);					/* store lba<15:08> */
    CMD_OUTB        (sector, ATACMD_B_SECNUM);					/* store sector number */
  }
  CMD_OUTB (iopex -> atacmdcode, ATACMD_BW_COMMAND);				/* start the request */

  delta = OZ_TIMER_RESOLUTION * ATA_TIMEOUT;
  goto finishup;

  /***********************/
  /* Start ATAPI request */
  /***********************/

start_atapi:

  /* Start dma stuff going */

  iopex -> amount_to_xfer = iopex -> doiopp.datasize;
  if (devex -> usedma && (iopex -> amount_to_xfer != 0)) {
    startdma (iopex, iopex -> doiopp.datasize, 1);
    if (iopex -> amount_xfrd != iopex -> doiopp.datasize) {
      oz_knl_printk ("oz_dev_ide8038i startreq: dma can only handle %u out of %u byte ATAPI data transfer\n", iopex -> amount_xfrd, iopex -> doiopp.datasize);
      iopex -> status = OZ_BADBUFFERSIZE;
      reqdone (iopex);
      goto dequeue;
    }
  }

  /* Fill in registers to start command (drive was already selected above) */

  if (devex -> usedma) {
    CMD_OUTB (1, ATACMD_BW_FEATURES);						// we're doing DMA data transfer
    CMD_OUTB (0xFE, ATACMD_B_CYL_LO);						// these only apply for PIO mode supposedly
    CMD_OUTB (0xFF, ATACMD_B_CYL_HI);
  } else {
    bytecount = iopex -> doiopp.datasize;					// get amount to be PIO'd
    if (bytecount > 0xFFFE) bytecount = 0xFFFE;					// ... but it can only do this much at once
    CMD_OUTB (0, ATACMD_BW_FEATURES);						// we're doing PIO data transfer
    CMD_OUTB (bytecount, ATACMD_B_CYL_LO);					// set bytecount to transfer
    CMD_OUTB (bytecount >> 8, ATACMD_B_CYL_HI);
  }
  CMD_OUTB (0, ATACMD_B_SECCOUNT);						// no command queuing tag
  CMD_OUTB (0xA0, ATACMD_BW_COMMAND);						// start the ATAPI request

  /* The command packet always gets sent via PIO */

  oz_hw_stl_nanowait (400);							/* wait 400nS before checking BUSY & DRQ */

  if (!(devex -> atapiidentword0 & 0x20)) {					/* if set, it interrupts us to get command */
    j = 3000;									/* else, we have to wait up to 3mS for some old clunkers */
    if (devex -> atapiidentword0 & 0x40) j = 50;				/* wait up to 50uS for modren clunkers to be ready */
    sts = oz_hw_stl_microwait (j, waitfornotbusy, ctrlr);
    if (sts == 0) sts = CTL_INB (ATACTL_BR_ALTSTS);
    if ((sts & 0x89) != 0x08) {							/* BUSY<7> off, DRQ<3> on, ERR<0> off */
      oz_knl_printk ("oz_dev_ide8038i: %s status %2.2X (error %2.2X) waiting to send command packet\n", 
	devex -> name, (uByte)sts, CMD_INB (ATACMD_BR_ERROR));
      iopex -> status = OZ_IOFAILED;
      reqdone (iopex);
      goto dequeue;
    }

    j = CMD_INB (ATACMD_B_SECCOUNT);			/* it should have the low bit set indicating command transfer */
    if (!(j & 1)) {
      oz_knl_printk ("oz_dev_ide8038i: %s didn't enter command transfer state (seccount %2.2X)\n", devex -> name, j);
      iopex -> status = OZ_IOFAILED;
      reqdone (iopex);
      goto dequeue;
    }

    j = 6;									/* some drives take 12-byte command packets */
    if (devex -> atapiidentword0 & 1) j = 8;					/* some drives take 16-byte command packets */
    DATA_OUTSW (j, iopex -> atapicmd);						/* copy command packet out to drive */
  }

  /* Determine timeout required */

  ctrlr -> timerwhen = -1LL;
  if (iopex -> doiopp.timeout == 0) goto finishup_notimeout;
  delta = iopex -> doiopp.timeout * (OZ_TIMER_RESOLUTION / 1000);

  /*****************************************/
  /* Common (both ATA and ATAPI) finish up */
  /*****************************************/

finishup:
  now = oz_hw_tod_getnow ();							/* get current date/time */
  OZ_HW_DATEBIN_ADD (ctrlr -> timerwhen, delta, now);				/* add the timeout delta value */
  if (ctrlr -> timer != NULL) {							/* (fake it if crash dumping) */
    if (!(ctrlr -> timerqueued) || oz_knl_timer_remove (ctrlr -> timer)) {
      ctrlr -> timerqueued = 1;
      oz_knl_timer_insert (ctrlr -> timer, ctrlr -> timerwhen, reqtimedout, ctrlr); /* stick new request in queue */
    }
  }

  /* If PIO write, start by writing the first chunk */

finishup_notimeout:
  if (iopex -> writedisk && !(devex -> usedma)) {				// see if a PIO write operation
    oz_hw_stl_nanowait (400);							// wait 400nS before checking BUSY & DRQ
    if (oz_hw_stl_microwait (5, waitfornotbusy, ctrlr) != 0) {			// wait up to 5uS for it to ask for data
      intserv (ctrlr, NULL);							// write a chunk of data to the drive
    }
  }
}

/************************************************************************/
/*									*/
/*  Fill in a controller's dma table for a request or set up PIO stuff	*/
/*									*/
/*    Input:								*/
/*									*/
/*	iopex -> phypages = physical page number array			*/
/*	iopex -> byteoffs = byte offset in first physical page		*/
/*	iopex -> amount_done = amount of buffer previously completed	*/
/*	iopex -> size = total buffer length				*/
/*	iopex -> writedisk & 1 = 0 : disk-to-memory transfer		*/
/*	                         1 : memory-to-disk transfer		*/
/*	xfersize  = number of bytes to transfer this time		*/
/*	blocksize = pad to multiple of this size			*/
/*	ctrlr -> bmiba  = dma controller I/O port base address		*/
/*	ctrlr -> pcidma = dma mapping struct				*/
/*									*/
/*    Output:								*/
/*									*/
/*	*(ctrlr -> pcidma) = table all set up for transfer		*/
/*	iopex -> amount_xfrd = number of bytes set up to transfer	*/
/*	dma controller started						*/
/*									*/
/************************************************************************/

static void startdma (Iopex *iopex, uLong xfersize, uLong blocksize)

{
  const OZ_Mempage *phypages;
  Ctrlr *ctrlr;
  int i, n;
  OZ_Dev_Pci_Dma32map *pcidma;
  OZ_Ieeedma32 *mapvirtadr;
  uLong bytecount, bytelimit, byteoffs, bytesindma, mappciaddr, sts;

  ctrlr = iopex -> devex -> ctrlr;

  /* Fill in pointer table */

  pcidma = iopex -> pcidma;
  if (pcidma == NULL) pcidma = ctrlr -> pcidma;

  byteoffs  = iopex -> byteoffs;								/* this is the offset in the first phys page */
  phypages  = iopex -> phypages;								/* this is pointer to physical page array */

  byteoffs += iopex -> amount_done;								/* offset by how much we did last time through */
  phypages += byteoffs >> OZ_HW_L2PAGESIZE;
  byteoffs &= (1 << OZ_HW_L2PAGESIZE) - 1;

  bytelimit = xfersize;										/* this is the number of bytes to transfer */
  if (bytelimit > iopex -> size - iopex -> amount_done) bytelimit = iopex -> size - iopex -> amount_done; /* slightly less if partial sector transfer */
  iopex -> amount_xfrd = bytelimit;								/* save how much we are doing this time */

  n = oz_dev_pci_dma32map_start (pcidma, 							/* point to dma struct */
                                 iopex -> writedisk & 1, 					/* 0:read; 1:write */
                                 bytelimit, 							/* number of bytes */
                                 phypages, 							/* physical page number array */
                                 byteoffs, 							/* byte offset in first page */
                                 &mapvirtadr, 							/* virt addr of dma struct */
                                 &mappciaddr);							/* pci addr of dma struct */
  if (n <= 0) oz_crash ("oz_dev_8038i startdma: dma mapping error, size %u, rc %d", bytelimit, n);
  for (i = 0; i < n; i ++) {
    if (mapvirtadr[i].bytecnt > 0x10000) oz_crash ("oz_dev_ide8038i startdma: segment too large %X", mapvirtadr[i].bytecnt);
    if ((mapvirtadr[i].phyaddr & 0x0FFFF) + mapvirtadr[i].bytecnt > 0x10000) oz_crash ("oz_dev_ide8038i startdma: segment crosses 64K boundary %X+%X", mapvirtadr[i].phyaddr, mapvirtadr[i].bytecnt);
  }
  if ((((mappciaddr + (n - 1) * sizeof *mapvirtadr) ^ mappciaddr) >> 16) != 0) {
    oz_crash ("oz_dev_ide8038i startdma: dmatable %X[%d] spans 64K boundary", mappciaddr, n);
  }
  mapvirtadr[n-1].bytecnt |= 0x80000000;							/* write terminator on struct */

  /* Start the DMA controller */

  oz_dev_pci_outb (0x66, ctrlr -> bmiba + BMIBA_B_BMISX);					/* reset dma error status bits */
  oz_dev_pci_outl (mappciaddr, ctrlr -> bmiba + BMIBA_L_BMIDTPX);				/* set descriptor table base address */
  oz_dev_pci_outb ((((iopex -> writedisk & 1) ^ 1) << 3) | 0x01, ctrlr -> bmiba + BMIBA_B_BMICX); /* start the dma controller */
}

/* We're done doing transfer */

static void stopdma (Iopex *iopex)

{
  OZ_Dev_Pci_Dma32map *pcidma;

  pcidma = iopex -> pcidma;
  if (pcidma == NULL) pcidma = iopex -> devex -> ctrlr -> pcidma;
  oz_dev_pci_dma32map_stop (pcidma);
}

/* Process recalibrate command - ATA drives only */

static void recalibrate (Devex *devex)

{
  Ctrlr *ctrlr;
  uByte status;

  ctrlr = devex -> ctrlr;

  CMD_OUTB ((devex -> drive_id << 4) | 0xE0, ATACMD_B_DRHEAD); /* store drive number */

  CMD_OUTB (0x10, ATACMD_BW_COMMAND);			/* start the recalibrate */
  status = oz_hw_stl_microwait (200000, checkrecaldone, ctrlr);			/* give it up to 200mS to finish */
  if (status == 0) {
    oz_knl_printk ("oz_dev_ide8038i recalibrate: status 0x%x, error 0x%x\n", CMD_INB (ATACMD_BR_STATUS), CMD_INB (ATACMD_BR_ERROR));
  }
}

/* Check to see if recalibrate command has finished */

static uLong checkrecaldone (void *ctrlrv)

{
  Ctrlr *ctrlr;
  uByte status;

  ctrlr  = ctrlrv;
  status = CMD_INB (ATACMD_BR_STATUS);
  if ((status & 0xC0) != 0x40) status = 0;
  return (status);
}

/************************************************************************/
/*									*/
/*  This routine is called via lowipl when a request is about to be 	*/
/*  started but the controller is showing a busy status.  The caller 	*/
/*  has already initiated a software reset for the controller.		*/
/*									*/
/*    Input:								*/
/*									*/
/*	ctrlrv   = points to controller struct				*/
/*	lowipl   = points to lowipl struct to restore			*/
/*	smplevel = softint						*/
/*									*/
/************************************************************************/

static void ctrlrhung (void *ctrlrv, OZ_Lowipl *lowipl)

{
  Ctrlr *ctrlr;
  Iopex *iopex;
  uLong hd;

  ctrlr = ctrlrv;

  oz_knl_printk ("oz_dev_ide8038i: resetting %X\n", ctrlr -> atacmd);
  enable_drives (ctrlr);					// get drives enabled and ready to process requests again
  oz_knl_printk ("oz_dev_ide8038i: restarting %X\n", ctrlr -> atacmd);
  hd = oz_hw_smplock_wait (ctrlr -> smplock);
  ctrlr -> lowipl = lowipl;					// re-arm this routine
  enable_interrupts (ctrlr);					// enable interrupts on the drives
  if (ctrlr -> iopex_ip == NULL) startreq (ctrlr);		// try to start the request again
  oz_hw_smplock_clr (ctrlr -> smplock, hd);
}

/************************************************************************/
/*									*/
/*  Timer ran out waiting for interrupt					*/
/*									*/
/*  If there is a request going, call the interrupt service routine 	*/
/*  and restart timer							*/
/*									*/
/************************************************************************/

static void reqtimedout (void *ctrlrv, OZ_Timer *timer)

{
  Ctrlr *ctrlr;
  Devex *devex;
  Iopex *iopex;
  OZ_Datebin now;
  uLong hd;

  ctrlr = ctrlrv;

  hd = oz_hw_smplock_wait (ctrlr -> smplock);			/* set interrupt lock */
  ctrlr -> timerqueued = 0;					/* say the timer is no longer queued */
  iopex = ctrlr -> iopex_ip;					/* make sure a request is in progress */
  if (iopex != NULL) {
    now = oz_hw_tod_getnow ();					/* get current date/time */
    if (OZ_HW_DATEBIN_CMP (ctrlr -> timerwhen, now) <= 0) {	/* make sure it's really up (it may have been bumped since it was queued) */
      devex = iopex -> devex;
      if (devex -> atapimsk != 0) {
        oz_knl_printk ("oz_dev_ide8038i: %s atapi command %2.2X timed out, datasize %u, transferred %u\n", 
                       devex -> name, iopex -> atapicmd[0], iopex -> size, iopex -> amount_xfrd);
      } else {
        oz_knl_printk ("oz_dev_ide8038i: %s atacmd %2.2X timed out, lbn %u, size %u\n", devex -> name, iopex -> atacmdcode, iopex -> slbn, iopex -> size);
        oz_knl_printk ("            amount_done %u, amount_xfrd %u\n", iopex -> amount_done, iopex -> amount_xfrd);
      }
      iopex -> timedout = 1;					/* remember it got a timeout */
      intserv (ctrlr, NULL);					/* call the interrupt routine to process whatever is left of request */
      if (!(ctrlr -> timerqueued) && (ctrlr -> iopex_ip != NULL)) {
        ctrlr -> timerqueued = 1;				/* restart timer if there is still a request in progress */
        ctrlr -> timerwhen   = now + (OZ_TIMER_RESOLUTION * ATA_TIMEOUT);
        oz_hw_smplock_clr (ctrlr -> smplock, hd);
        oz_knl_timer_insert (ctrlr -> timer, ctrlr -> timerwhen, reqtimedout, ctrlr);
        return;
      }
    } else {
      ctrlr -> timerqueued = 1;					/* it got bumped since it was queued, re-queue at new date/time */
      oz_hw_smplock_clr (ctrlr -> smplock, hd);			/* release interrupt lock */
      oz_knl_timer_insert (ctrlr -> timer, ctrlr -> timerwhen, reqtimedout, ctrlr);
      return;
    }
  }
  oz_hw_smplock_clr (ctrlr -> smplock, hd);
}

/************************************************************************/
/*									*/
/*  Interrupt service routine entrypoint				*/
/*									*/
/*    Input:								*/
/*									*/
/*	ctrlrv = controller that caused the interrupt			*/
/*	mchargs = cpu state at point of interrupt (not used)		*/
/*									*/
/************************************************************************/

static void intserv (void *ctrlrv, OZ_Mchargs *mchargs)

{
  Ctrlr *ctrlr;
  Devex *devex;
  Iopex *iopex;
  uByte bmisx, status;

  ctrlr = ctrlrv;
  validaterequestcount (ctrlr, __LINE__);

  /* Clear the interrupt bit in the DMA controller */

  if (ctrlr -> bmiba != 0) {
    bmisx = BMIBA_INB (BMIBA_B_BMISX);
    BMIBA_OUTB (0x64, BMIBA_B_BMISX);
  }

  /* Read the status and clear interrupt */

  status = CMD_INB (ATACMD_BR_STATUS);

  /* Get current request pointer */

  iopex = ctrlr -> iopex_ip;

  /* Process interrupt */

  if (iopex != NULL) {
    devex = iopex -> devex;
    if (devex -> usedma) dma_intserv (ctrlr, devex, iopex, status, bmisx);
    else if (devex -> atapimsk == 0) pio_ata_intserv (iopex, status);
    else pio_atapi_intserv (iopex, status);
  }

  validaterequestcount (ctrlr, __LINE__);
}

/************************************************************************/
/*									*/
/*  This is the DMA interrupt routine - it is called at irq level when 	*/
/*  the drive completes an operation					*/
/*									*/
/*  It is also called when the timeout happens				*/
/*									*/
/*    Input:								*/
/*									*/
/*	ctrlrv  = pointer to controller that interrupted		*/
/*	smplock = the associated irq's smplock is set			*/
/*									*/
/************************************************************************/

static void dma_intserv (Ctrlr *ctrlr, Devex *devex, Iopex *iopex, uByte status, uByte bmisx)

{
  /* Check for ATAPI command delivery - some drives interrupt when ready to receive command packet */

  if (atapi_command_packet (iopex, status)) return;

  /* Make sure drive is no longer busy - if so and timer is still going, let it interrupt again */
  /* If timer is no longer going, it means it has timed out, so abort the request               */

  if ((status & 0x80) || (!(status & 0x01) && (status & 0x08) && !(bmisx & 0x02))) {
    oz_knl_printk ("oz_dev_ide8038i: %s busy or drq still set (status 0x%x, bmisx 0x%x)\n", devex -> name, status, bmisx);
    if (ctrlr -> timerqueued) {
      validaterequestcount (ctrlr, __LINE__);
      return;
    }
  }

  /* Make sure dma controller no longer busy */

  if (bmisx & 0x01) {
    oz_knl_printk ("oz_dev_ide8038i: %s dma busy still set (0x%x)\n", devex -> name, bmisx);
    oz_knl_printk ("oz_dev_ide8038i: - transfer size %u\n", iopex -> amount_xfrd);
    BMIBA_OUTB (0, BMIBA_B_BMICX);
  }

  /* Process the interrupt */

  if (devex -> atapimsk == 0) ata_finish (iopex, status);
  else atapi_finish (iopex, status);
}

/************************************************************************/
/*									*/
/*  This is the PIO interrupt routine - it is called at irq level when 	*/
/*  the drive completes an operation or wants to transfer data		*/
/*									*/
/*  It is also called when the timeout happens				*/
/*									*/
/*    Input:								*/
/*									*/
/*	ctrlrv  = pointer to controller that interrupted		*/
/*	smplock = the associated irq's smplock is set			*/
/*									*/
/************************************************************************/

static void pio_ata_intserv (Iopex *iopex, uByte status)

{
  Ctrlr *ctrlr;
  Devex *devex;
  int j;
  uByte error;

  devex = iopex -> devex;
  ctrlr = devex -> ctrlr;

  /* Maybe it needs some data - ATA devices get a block */

  while ((status & 0x89) == 0x08) {							/* BUSY<7> off, DRQ<3> on, ERR<0> off */

    if (((iopex -> atacmdcode & 0xFE) == 0xC4) && (devex -> multsize != 0)) {		/* ATA read/write multiple : do the group of blocks */
      j = devex -> multsize * DISK_BLOCK_SIZE;
      if (j + iopex -> amount_xfrd > iopex -> seccount * DISK_BLOCK_SIZE) {		// ... or maybe just to the end of the transfer size
        j = iopex -> seccount * DISK_BLOCK_SIZE - iopex -> amount_xfrd;
      }
    } else {
      j = DISK_BLOCK_SIZE;								/* standard ATA requests : do exactly one block */
    }
    do_pio_transfer (j, iopex);								// transfer to/from caller's buffer
    if ((iopex -> writedisk) || (iopex -> amount_xfrd < iopex -> amount_to_xfer)) return;
											/* note: we must return after transferring just one block of data */
											/*       if we try to be efficient by looping, we get timeout errors */
    CTL_INB (ATACTL_BR_ALTSTS);								/* give controller time to come up with post-transfer status */
    status = CMD_INB (ATACMD_BR_STATUS);						/* re-check the status */
    if (status & 0x80) return;								/* wait for another interrupt if it's busy again */
  }

  ata_finish (iopex, status);
}

static void pio_atapi_intserv (Iopex *iopex, uByte status)

{
  Ctrlr *ctrlr;
  int j;

  ctrlr = iopex -> devex -> ctrlr;

  /* Check for ATAPI command delivery - some drives interrupt when ready to receive command packet */

  if (atapi_command_packet (iopex, status)) {
    CTL_INB (ATACTL_BR_ALTSTS);								/* give controller time to come up with post-transfer status */
    status = CMD_INB (ATACMD_BR_STATUS);						/* re-check the status */
    if (status & 0x80) return;								/* wait for another interrupt if it's busy again */
  }

  /* Maybe it needs some data - ATAPI devices get bytecount in cyllo/cylhi registers */

  while ((status & 0x89) == 0x08) {							/* BUSY<7> off, DRQ<3> on, ERR<0> off */
    j  = CMD_INB (ATACMD_B_CYL_HI) << 8;						// get transfer bytecount
    j += CMD_INB (ATACMD_B_CYL_LO);
    do_pio_transfer (j, iopex);								// transfer to/from caller's buffer
    CTL_INB (ATACTL_BR_ALTSTS);								/* give controller time to come up with post-transfer status */
    status = CMD_INB (ATACMD_BR_STATUS);						/* re-check the status */
    if (status & 0x80) return;								/* wait for another interrupt if it's busy again */
  }

  atapi_finish (iopex, status);
}

static int atapi_command_packet (Iopex *iopex, uByte status)

{
  Ctrlr *ctrlr;
  Devex *devex;
  int cmdlen;

  devex = iopex -> devex;
  ctrlr = devex -> ctrlr;

  if ((devex -> atapiidentword0 & 0x60) != 0x20) return (0);	// only these drives interrupt for command packet transfer
  if ((status & 0x89) != 0x08) return (0);			// BUSY<7> off, DRQ<3> on, ERR<0> off
  status = CMD_INB (ATACMD_B_SECCOUNT);				// it has the low bit set indicating command transfer
  if (!(status & 1)) return (0);

  cmdlen = 6;							// some drives take 12-byte command packets
  if (devex -> atapiidentword0 & 1) cmdlen = 8;			// some drives take 16-byte command packets
  DATA_OUTSW (cmdlen, iopex -> atapicmd);			// copy command packet out to drive
  return (1);							// tell caller we sent command out
}

/************************************************************************/
/*									*/
/*  Do a PIO transfer							*/
/*									*/
/*    Input:								*/
/*									*/
/*	nbytes = number of bytes to transfer				*/
/*	iopex  = I/O request being processed				*/
/*	      -> byteoffs = byte offset in original requestor's buffer	*/
/*	      -> phypages = physical page array of original reqestor's buffer
/*	      -> amount_done = amount of whole original transfer done by previous commands
/*	      -> amount_xfrd = amount previously done for this command	*/
/*									*/
/*    Output:								*/
/*									*/
/*	data transferred						*/
/*	iopex -> amount_xfrd = incremented				*/
/*									*/
/************************************************************************/

static void do_pio_transfer (int nbytes, Iopex *iopex)

{
  const OZ_Mempage *phypages;
  Ctrlr *ctrlr;
  int i, pad;
  OZ_Pagentry oldpte;
  uLong byteoffs;
  uWord *vad;

  ctrlr = iopex -> devex -> ctrlr;

  byteoffs  = iopex -> byteoffs;					/* get offset of first byte in original page */
  phypages  = iopex -> phypages;					/* get pointer to first element of physical page array */
  byteoffs += iopex -> amount_done + iopex -> amount_xfrd;		/* increment offset in first page by the amount already done and transferred */
  phypages += (byteoffs >> OZ_HW_L2PAGESIZE);				/* normalize with physical page array pointer */
  byteoffs %= 1 << OZ_HW_L2PAGESIZE;
  vad = oz_hw_phys_mappage (*phypages, &oldpte);			/* map physical page to virtual address */
  (OZ_Pointer)vad += byteoffs;

  i    = iopex -> size - iopex -> amount_done - iopex -> amount_xfrd;	/* how many bytes left in the whole request to process */
  pad  = nbytes - i;							/* any left over is padding */
  if (pad > 0) nbytes = i;						/* just transfer this much to/from data buffer */
  pad /= 2;								/* make pad a word count */

  if (iopex -> writedisk) {
    while (1) {
      i = (1 << OZ_HW_L2PAGESIZE) - byteoffs;				/* get number of bytes to end of page */
      if (i > nbytes) i = nbytes;					/* but don't do more than drive wants */
      DATA_OUTSW (i / 2, vad);						/* give it the data */
      iopex -> amount_xfrd += i;					/* this much more has been transferred */
      nbytes -= i;							/* this much less to do */
      if (nbytes == 0) break;						/* stop if all done */
      byteoffs += i;							/* more to transfer, get offset to it */
      phypages += (byteoffs >> OZ_HW_L2PAGESIZE);			/* normalize with physical page array pointer */
      byteoffs %= 1 << OZ_HW_L2PAGESIZE;
      vad = oz_hw_phys_mappage (*phypages, NULL);			/* map physical page to virtual address */
      (OZ_Pointer)vad += byteoffs;
    }
    while (-- pad >= 0) CMD_OUTW (0, ATACMD_W_DATA);			/* zero fill padding */
  } else {
    while (1) {
      i = (1 << OZ_HW_L2PAGESIZE) - byteoffs;				/* get number of bytes to end of page */
      if (i > nbytes) i = nbytes;					/* but don't do more than drive has */
      DATA_INSW (i / 2, vad);						/* get the data from drive */
      iopex -> amount_xfrd += i;					/* this much more has been transferred */
      nbytes -= i;							/* this much less to do */
      if (nbytes == 0) break;						/* stop if all done */
      byteoffs += i;							/* more to transfer, get offset to it */
      phypages += (byteoffs >> OZ_HW_L2PAGESIZE);			/* normalize with physical page array pointer */
      byteoffs %= 1 << OZ_HW_L2PAGESIZE;
      vad = oz_hw_phys_mappage (*phypages, NULL);			/* map physical page to virtual address */
      (OZ_Pointer)vad += byteoffs;
    }
    while (-- pad >= 0) CMD_INW (ATACMD_W_DATA);			/* discard padding */
  }

  oz_hw_phys_unmappage (oldpte);					/* unmap the physical page */
}

/************************************************************************/
/*									*/
/*  Finish up an (PIO or DMA) ATA request				*/
/*									*/
/************************************************************************/

static void ata_finish (Iopex *iopex, uByte status)

{
  Ctrlr *ctrlr;
  Devex *devex;
  int j;
  uByte error;

  devex = iopex -> devex;
  ctrlr = devex -> ctrlr;

  /* Make sure drive is no longer busy - if so and timer is still going, let it interrupt again */
  /* If timer is no longer going, it means it has timed out, so abort the request               */

  if ((status & 0x80) || ((status & 0x89) == 0x08)) {
    oz_knl_printk ("oz_dev_ide8038i: %s busy or drq still set (status 0x%x)\n", devex -> name, status);
    if (ctrlr -> timerqueued) {
      validaterequestcount (ctrlr, __LINE__);
      return;
    }
  }

  /* Request is complete, stop DMA and check the status bits for error */

  if (devex -> usedma) stopdma (iopex);
  iopex -> status = OZ_SUCCESS;								// assume it was successful
  if (status & 0xA1) {									// see if any error bits are set
    iopex -> status = OZ_WRITELOCKED;							// maybe it's just writelocked
    if (status & 0x81) {								// check for any other error bits
      error  = CMD_INB (ATACMD_BR_ERROR);						// ok, get the error status
      oz_knl_printk ("oz_dev_ide8038i: %s status 0x%x, error 0x%x\n", devex -> name, status, error); // output message
      iopex -> status = OZ_IOFAILED;							// return generic error status
      if (!(status & 0x80) || (error & 0x80)) iopex -> status = OZ_BADMEDIA;		// maybe it's bad media
    }
  }

  /* If IOFAILED or BADMEDIA, maybe retry */

  if (((iopex -> status == OZ_IOFAILED) || (iopex -> status == OZ_BADMEDIA)) && (-- (iopex -> retries) > 0)) goto requeue;

  /* If successful, accumulate amount transferred so far.  Then if not complete, re-queue it. */

  if (iopex -> status == OZ_SUCCESS) {
    iopex -> amount_done += iopex -> amount_xfrd;			/* if successful, increment amount done */
    if (iopex -> amount_done > iopex -> size) iopex -> amount_done = iopex -> size; /* (padded out a short block) */
    if (iopex -> amount_done < iopex -> size) goto requeue;		/* if more to go, re-queue request to finish up */
    if (iopex -> ix4kbuk) {
      ix4kbuk_validate_phypage (iopex -> phypages, iopex -> byteoffs, __FILE__, __LINE__);
    }
  }

  /* It is complete (good or bad) */

  reqdone (iopex);							/* put completed request on completion queue */
  startreq (ctrlr);							/* start another request */
  validaterequestcount (ctrlr, __LINE__);
  return;

  /* Either continue or re-try the request */

requeue:
  ctrlr -> iopex_ip = NULL;
  queuereq (iopex);
  validaterequestcount (ctrlr, __LINE__);
}

/************************************************************************/
/*									*/
/*  Finish up an (PIO or DMA) ATAPI request				*/
/*									*/
/************************************************************************/

static void atapi_finish (Iopex *iopex, uByte status)

{
  Ctrlr *ctrlr;
  Devex *devex;
  uByte error;

  devex = iopex -> devex;
  ctrlr = devex -> ctrlr;

  /* Make sure drive is no longer busy - if so and timer is still going, let it interrupt again */
  /* If timer is no longer going, it means it has timed out, so abort the request               */

  if (status & 0x80) {
    oz_knl_printk ("oz_dev_ide8038i: %s busy (status 0x%X)\n", devex -> name, status);
    if (ctrlr -> timerqueued) {
      validaterequestcount (ctrlr, __LINE__);
      return;
    }
  }

  /* Request is complete, stop DMA and check the status bits for error */

  if (devex -> usedma && (iopex -> amount_to_xfer != 0)) stopdma (iopex);
  iopex -> atapists = 0;
  iopex -> status = OZ_SUCCESS;
  if ((status & 0xC9) != 0x40) {				// BSY=0; DRDY=1; DRQ=0; ERR=0
    if ((status & 0xC9) == 0x41) {
      error  = CMD_INB (ATACMD_BR_ERROR);
      if (error & 0x04) {
        oz_knl_printk ("oz_dev_ide8038i: %s status 0x%X, error 0x%X\n", devex -> name, status, error);
        iopex -> status = OZ_IOFAILED;
      } else {
        iopex -> atapists = error >> 4;
      }
    } else {
      oz_knl_printk ("oz_dev_ide8038i: %s status 0x%X\n", devex -> name, status);
      iopex -> status = OZ_IOFAILED;
    }
  }

  /* It is complete (good or bad) */

  reqdone (iopex);							/* put completed request on completion queue */
  startreq (ctrlr);							/* start another request */
  validaterequestcount (ctrlr, __LINE__);
}

/************************************************************************/
/*									*/
/*  An request is done							*/
/*									*/
/*    Input:								*/
/*									*/
/*	iopex   = the completed request with status filled in		*/
/*	smplock = irq level						*/
/*									*/
/*    Output:								*/
/*									*/
/*	request posted for completion					*/
/*									*/
/************************************************************************/

static void reqdone (Iopex *iopex)

{
  Ctrlr *ctrlr;
  Devex *devex;

  devex = iopex -> devex;
  ctrlr = devex -> ctrlr;
  ctrlr -> iopex_ip = NULL;
  ctrlr -> requestcount --;
  if (iopex -> timedout) oz_knl_printk ("oz_dev_ide8038i: %s request complete, status %u\n", iopex -> devex -> name, iopex -> status);
  if (iopex -> ioop != NULL) {
    if ((devex -> atapimsk != 0) && ((iopex -> doiopp.datarlen != NULL) 
                                  || (iopex -> doiopp.status != NULL) 
                                  || (iopex -> pcidma != NULL))) {
      oz_knl_iodonehi (iopex -> ioop, iopex -> status, NULL, atapi_reqdone, iopex);
    }
    else oz_knl_iodonehi (iopex -> ioop, iopex -> status, NULL, NULL, NULL);
  }
}

/* Back in requestor's memory space and requestor wants the ATAPI data result   */
/* length and/or status byte returned, or there is a temp dma table to free off */

static void atapi_reqdone (void *iopexv, int finok, uLong *status_r)

{
  Iopex *iopex;
  uLong sts;

  iopex = iopexv;

  if (iopex -> pcidma != NULL) oz_dev_pci_dma32map_free (iopex -> pcidma);

  if (finok) {
    if (iopex -> doiopp.datarlen != NULL) {
      sts = oz_knl_section_uput (iopex -> procmode, sizeof *(iopex -> doiopp.datarlen), &(iopex -> amount_xfrd), iopex -> doiopp.datarlen);
      if (*status_r == OZ_SUCCESS) *status_r = sts;
    }
    if (iopex -> doiopp.status != NULL) {
      sts = oz_knl_section_uput (iopex -> procmode, sizeof *(iopex -> doiopp.status), &(iopex -> atapists), iopex -> doiopp.status);
      if (*status_r == OZ_SUCCESS) *status_r = sts;
    }
  }
}

/************************************************************************/
/*									*/
/*  Make sure we haven't lost an request somewhere			*/
/*									*/
/*    Input:								*/
/*									*/
/*	ctrlr -> requestcount = number of pending I/O requests		*/
/*	smplevel = hd							*/
/*									*/
/************************************************************************/

static void validaterequestcount (Ctrlr *ctrlr, int line)

{
  int reqcount;
  Iopex *iopex, **liopex;

  reqcount = 0;
  if (ctrlr -> iopex_ip != NULL) reqcount ++;

  for (liopex = &(ctrlr -> iopex_qh); (iopex = *liopex) != NULL; liopex = &(iopex -> next)) reqcount ++;

  if (liopex != ctrlr -> iopex_qt) {
    oz_crash ("oz_dev_ide8038i validaterequestcount %d: liopex %p, ctrlr %p -> iopex_qt %p", line, liopex, ctrlr, ctrlr -> iopex_qt);
  }

  if (reqcount != ctrlr -> requestcount) {
    oz_crash ("oz_dev_ide8038i validaterequestcount %d: found %u, should have %u requests", line, reqcount, ctrlr -> requestcount);
  }
}
