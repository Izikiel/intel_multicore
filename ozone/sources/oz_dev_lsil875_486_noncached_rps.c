//+++2002-05-10
//    Copyright (C) 2001,2002  Mike Rieker, Beverly, MA USA
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
//---2002-05-10

/************************************************************************/
/*									*/
/*  LSI Logic 53C875 SCSI controller chip driver			*/
/*									*/
/************************************************************************/

#include "ozone.h"

#include "oz_dev_pci.h"
#include "oz_dev_scsi.h"
#include "oz_dev_timer.h"
#include "oz_io_scsi.h"
#include "oz_knl_devio.h"
#include "oz_knl_event.h"
#include "oz_knl_hw.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_logname.h"
#include "oz_knl_malloc.h"
#include "oz_knl_misc.h"
#include "oz_knl_phymem.h"
#include "oz_knl_procmode.h"
#include "oz_knl_sdata.h"
#include "oz_knl_section.h"
#include "oz_knl_spte.h"
#include "oz_knl_status.h"
#include "oz_sys_dateconv.h"
#include "oz_sys_xprintf.h"

typedef struct Chnex Chnex;
typedef struct Devex Devex;
typedef struct Iopex Iopex;
typedef struct Regs Regs;
typedef struct Rp Rp;

/* Misc defines */

#define UNITNAME_PREFIX "lsil875"		/* controller device names begin with this */
#define STARTUPDELAY 2				/* number of seconds for startup delay */

#define ONEATATIME 0 // 1			/* 0: queue multiple requests (per scsi-id) at a time */
						/* 1: queue one request (per scsi-id) at a time */
						/* Setting to 0 causes many INT 0x96's for stretches of time */
						/* ... so sometime this should be fixed.  For now, set to 1 */

#define CRASH_NUMPAGES (2)			/* max number of physical pages for a single crash-dump transfer */
						/* 2 should be adequate because the crash routine writes at most */
						/* one physical page at a time so we need 2 in case the stuff is */
						/* split across a physical page boundary */

#define MAX_SCSI_ID 16				/* maximum scsi id permitted (exclusive) */
#define MAX_CMDLEN 16				/* maximum command length */

#define DCMD_CHMOV_WITH_DATA_OUT 0x00000000	/* data from memory to device, put odd byte in SWIDE */
#define DCMD_CHMOV_WITH_DATA_IN  0x01000000	/* data from device to memory */
#define DCMD_MOVE_WITH_DATA_OUT  0x08000000	/* data from memory to device, transfer last odd byte to device */
#define DCMD_JUMP                0x80080000	/* jump to given address */

				/* these values are maintained in SCRATCHJ1 by the SCRIPTS program */
				/* they are used to indicate to the host CPU that an expected exception may occurr */
				/* ... and it is ok for the host CPU to do its recovery processing                 */
#define SCRATCHJ1_CHMOVS 1			/* currently doing the data CHMOV/MOVE's */
#define SCRATCHJ1_SELECT 2			/* currently doing the device SELECT */

#define RP_NPAGES (16384>>OZ_HW_L2PAGESIZE)	/* alloc 16K non-cached memory for request packets */

/* State values kept in SCRATCHJ0 for debugging */

#define STATE_IDLE                 0
#define STATE_GETTING_MESSAGE      1
#define STATE_GETTING_STATUS       2
#define STATE_MESSAGE_OUT          3
#define STATE_CHECKING_TARGET      4
#define STATE_SELECTED             5
#define STATE_SELECTING            6
#define STATE_SENDING_COMMAND      7
#define STATE_TRANSFERRING_DATA    8
#define STATE_WAITING_FOR_RESELECT 9

/* Scsi ID table entries - these are located at the very beginning of the SCRIPTS ram */
/* The scsi id table has 16 entries, one for each possible scsi id on the controller  */

typedef struct { volatile uLong se_queue_head;		/* pci address of first request_packet for the device */
							/* - when queuing a new request, you must set the low bit = 1 */
							/*   the scripts program clears it when it starts processing the request */
                 volatile uByte se0_sequence;		/* sequence of current request */
                 volatile uByte se1_sxfer;		/* current negotiated transfer speed */
                 volatile uByte se2_scsi_id;		/* corresponding scsi_id (used by SELECT instruction) */
                 volatile uByte se3_scntl3;		/* current negotiated transfer width */
                 volatile uLong se_saved_dsp;		/* saved value of rp_datamov_pa */
							/*   <00> = 0 : no restore required */
							/*          1 : data moved, restore required */
                 volatile uLong se_saved_dbc;		/* saved dcmd/dbc at rp_datamov_pa */
               } Se;

/* Channel extension structure */

struct Chnex { int open;			/* 0: not open; 1: open (scsi_id is valid) */
               uLong scsi_id;			/* scsi_id that this channel is open on */
               OZ_Lockmode lockmode;		/* lockmode that this channel has on the scsi_id */
             };

/* Device extension structure */

#define DEVSTATE_OFFLINE 0	// chip is halted and reset
#define DEVSTATE_STARTING 1	// chip started, but waiting two seconds
#define DEVSTATE_ONLINE 2	// chip is ready to process requests

struct Devex { OZ_Devunit *devunit;		/* devunit pointer */
               const char *name;		/* devunit name string pointer */
               Regs *regs;			/* controller chip's registers (in memory) (virt addr) */
               void *sram_va;			/* SCRIPTS RAM (virt addr) */
               uLong sram_pa;			/* SCRIPTS RAM (pci addr) */
               Se *se_va;			/* virt address of scsi_id_table */
               OZ_Hw486_irq_many irq_many;	/* shared irq link struct */
               OZ_Smplock *smplock;		/* pointer to irq level smp lock */
               OZ_Timer *startuptimer;		/* startup timer */
               OZ_Timer *timeouttimer;		/* timeout timer */
               OZ_Datebin nextimeout;		/* when the timer goes off next or 0 if not currently queued */

               Rp *firstrp[MAX_SCSI_ID];	/* first rp queued to the scsi device (virt addr) */
               Rp *lastrp[MAX_SCSI_ID];		/* last rp queued to the scsi device (virt addr) */

               Long refc_read[MAX_SCSI_ID];	/* number of opened channels that allow reading */
               Long refc_write[MAX_SCSI_ID];	/* number of opened channels that allow writing */
               Long deny_read[MAX_SCSI_ID];	/* number of opened channels that deny reading */
               Long deny_write[MAX_SCSI_ID];	/* number of opened channels that deny writing */
               uLong queueseq[MAX_SCSI_ID];	/* queuing sequence */

               Rp *startingqh;			/* requests holding for two second startup delay */
               Rp **startingqt;

               uLong resumedsp;			/* dsp to resume processing at */

               uByte intvec;			/* irq vector */
               uByte scsi_id;			/* this controller's scsi id */
               uByte devstate;			/* device state */
             };

/* I/O extension structure */

struct Iopex { OZ_Ioop *ioop;			/* I/O operation block pointer */
               OZ_Procmode procmode;		/* processor mode of request */
               Devex *devex;			/* pointer to device */
               uLong datanumpages;		/* number of entries in dataphypages array */
               OZ_IO_scsi_doiopp doiopp;	/* doiopp structure from user */
               uByte scsi_id;			/* scsi-id of the request */
               uByte queueflg;			/* (debugging) how it was queued */
             };

/* Request packets queued to controller */

#define RP_FLAG_NEEDTOIDENT 0x01		/* need to send 'identify' message */
#define RP_FLAG_NEGWIDTH    0x02		/* re-negotiate transfer width */
#define RP_FLAG_NEGSYNCH    0x04		/* re-negotiate transfer speed */
#define RP_FLAG_GOTSTATUS   0x08		/* the status byte was received */
#define RP_FLAG_DONE        0x10		/* the request is complete */
#define RP_FLAG_ABORTED     0x20		/* the 'abort task' message was sent to target */
#define RP_FLAG_DISCONNECT  0x40		/* allow target to disconnect during command */

				/* the next code is the only one used by SCRIPTS and can't be changed without changing the SCRIPTS */
#define RP_ABORT_BUFFEROVF 1			/* device wanted to transfer more data than buffer allowed */
				/* the rest are only used internally and can be changed as needed */
#define RP_ABORT_FATLERR   2			/* some fatal error accessing device */
#define RP_ABORT_IOABORT   3			/* an call to oz_knl_ioabort was made */
#define RP_ABORT_TIMEOUT   4			/* the timeout elapsed */
#define RP_ABORT_SELECT    5			/* the scsi SELECT phase timed out */
#define RP_ABORT_CTRLERR   6			/* some fatal controller error happened */

static const uLong abortstatus[] = { OZ_SCSISTSMISSING, OZ_BUFFEROVF, OZ_FATALDEVERR, OZ_ABORTED, OZ_TIMEDOUT, OZ_SCSISELECTERR, OZ_FATALCTRLERR };

struct Rp { /* These items are also defined in the SCRIPTS program so must not be moved */

            void *rp_this_va;			/* virtual address of this packet */
            uLong rp_next_pa;			/* pci address of next request_packet for the device */
						/* - you must set the low bit = 1 when linking these */
            uLong rp_datamov_pa;		/* pci address of data transfer CHMOV/MOVE's */
						/* - set to list of CHMOV/MOVE's that will transfer data */
						/*   the list must be terminated by a 'JUMP transfer_data_done' */
						/*   SCRIPTS will point it to an internal JUMP if/when they complete */
            volatile uByte rp0_flags;		/* flag bits */
						/* - new requests must have RP_FLAG_NEEDTOIDENT set */
						/*   they may also have _NEGWIDTH, _NEGSYNCH and/or _DISCONNECT set */
						/*   everything else must be clear to start */
            uByte rp1_abort;			/* zero: ok to process request */
						/* else: abort asap (& unlink & set RP_FLAG_DONE) */
            uByte rp2_seqsts;			/* input: sequence number (must match se0_sequence) */
						/* output: final scsi status byte (when RP_FLAG_GOTSTATUS is set) */
            uByte rp3_cmdlen;			/* length of command (1..sizeof rp_command) */
            uByte rp_command[MAX_CMDLEN];	/* command buffer */

            /* These items are only used in the driver so can be changed at will */

            Rp *rp_next_va;			/* virtual address of next request on devex -> firstrp */
            Iopex *rp_iopex;			/* iopex pointer */
            OZ_Datebin rp_timesout;		/* when the request times out or 0 if never */

            /* The driver places the CHMOV/MOVE/JUMP instructions here at the end of the rp (long aligned).   */
            /* Initially, the driver points rp_datamov_pa at the first element.  Then if the target           */
            /* disconnects in the middle of a transfer, rp_datamov_pa points to the interrupted instruction   */
            /* and that instruction is modified to include only the untransferred portion of the data buffer. */

            struct { uLong dcmd_dbc;		/* dma command is <24:31>, dma bytecount in <00:23> */
                     uLong dnad;		/* dma address is 32 bits */
                   } rp_datamovs[1];
          };

/* Function table */

static int lsil875_shutdown (OZ_Devunit *devunit, void *devexv);
static uLong lsil875_assign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode);
static int lsil875_deassign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv);
static void lsil875_abort (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, 
                           OZ_Ioop *ioop, void *iopexv, OZ_Procmode procmode);
static uLong lsil875_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode, 
                            OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap);

static const OZ_Devfunc lsil875_functable = { sizeof (Devex), sizeof (Chnex), sizeof (Iopex), 0, 
                                              lsil875_shutdown, NULL, NULL, lsil875_assign, 
                                              lsil875_deassign, lsil875_abort, lsil875_start, NULL };

/* Internal static data */

static int initialized = 0;
static OZ_Devclass  *devclass;
static OZ_Devdriver *devdriver;
static OZ_Memlist   *rpmemlist = NULL;
static OZ_Smplock    rpmemlist_smplock;
static volatile Long rpmemlist_inuse = 0;
static volatile Long rpmemlist_total = 0;

static OZ_Iochan *crash_iochan  = NULL;	/* io channel to controller the crash was set up on */
static Devex     *crash_devex   = NULL;	/* controller device the crash dump goes to */
static int        crash_inprog  = 0;	/* 0: normal operation; 1: crash dump in progress */
static Iopex      crash_iopex;		/* an iopex used for crash dump requests */
static Rp        *crash_rp      = NULL;	/* pointer to rp used for crash dump requests */
static uLong      crash_scsi_id = -1;	/* the scsi-id of the device crash dump goes to */

/* Internal routines */

static uLong lsil875_crash (void *dummy, OZ_IO_scsi_doiopp *scsi_doiopp);
static uLong qiopp (Chnex *chnex, Iopex *iopex);
static Rp *malloc_rp (uLong datanumpages);
static void fill_in_rp (Rp *rp, Iopex *iopex);
static void free_rp (Rp *rp);
static void queue_request (Rp *rp);
static uLong check_sem_bit (void *regsv);
static void timeout (void *devexv, OZ_Timer *timer);
static int chip_interrupt (void *devexv, OZ_Mchargs *mchargs);
static void done_interrupt (Devex *devex);
#if ONEATATIME
static void start_request (Devex *devex, uLong scsi_id);
#endif
static void dma_interrupt (Devex *devex, uByte istat, uByte dstat);
static void scsi_interrupt (Devex *devex, uByte istat, uByte sist0, uByte sist1);
static void setupdatamovs (Rp *rp);
static void chip_abort (Devex *devex, int alreadyhalted);
static uLong chip_abort_haltcheck (void *regsv);
static void chip_dump (Devex *devex);
static void abort_requests (Devex *devex, uLong scsi_id, uByte rp_abort_code);
static void postreq (Rp *rp);
static void finishup (void *rpv, int finok, uLong *status_r);
static void chip_enable (Devex *devex);
static void chip_started (void *devexv, OZ_Timer *timer);
static void chip_reset (Devex *devex);
static uLong npp_virt_to_phys (uLong size, void *addr);
static OZ_Mempage calc_datanumpages (OZ_IO_scsi_doiopp *doiopp);
static uLong calc_datarlen (Rp *rp);
static uLong calc_status (Rp *rp);

/* Chip's registers */

struct Regs { volatile uByte scntl0;	/* 00 */
              volatile uByte scntl1;	/* 01 */
              volatile uByte scntl2;	/* 02 */
              volatile uByte scntl3;	/* 03 */
              volatile uByte scid;	/* 04 */
              volatile uByte sxfer;	/* 05 */
              volatile uByte sdid;	/* 06 */
              volatile uByte gpreg;	/* 07 */
              volatile uByte sfbr;	/* 08 */
              volatile uByte socl;	/* 09 */
              volatile uByte ssid;	/* 0A */
              volatile uByte sbcl;	/* 0B */
              volatile uByte dstat;	/* 0C */
              volatile uByte sstat0;	/* 0D */
              volatile uByte sstat1;	/* 0E */
              volatile uByte sstat2;	/* 0F */
              volatile uLong dsa;	/* 10 */
              volatile uByte istat;	/* 14 */
              volatile uByte pad1[3];
              volatile uByte ctest0;	/* 18 */
              volatile uByte ctest1;	/* 19 */
              volatile uByte ctest2;	/* 1A */
              volatile uByte ctest3;	/* 1B */
              volatile uLong temp;	/* 1C */
              volatile uByte dfifo;	/* 20 */
              volatile uByte ctest4;	/* 21 */
              volatile uByte ctest5;	/* 22 */
              volatile uByte ctest6;	/* 23 */
              volatile uLong dcmd_dbc;	/* 24 */
              volatile uLong dnad;	/* 28 */
              volatile uLong dsp;	/* 2C */
              volatile uLong dsps;	/* 30 */
              volatile uByte scratcha0;	/* 34 */
              volatile uByte scratcha1;	/* 35 */
              volatile uByte scratcha2;	/* 36 */
              volatile uByte scratcha3;	/* 37 */
              volatile uByte dmode;	/* 38 */
              volatile uByte dien;	/* 39 */
              volatile uByte sbr;	/* 3A */
              volatile uByte dcntl;	/* 3B */
              volatile uLong adder;	/* 3C */
              volatile uByte sien0;	/* 40 */
              volatile uByte sien1;	/* 41 */
              volatile uByte sist0;	/* 42 */
              volatile uByte sist1;	/* 43 */
              volatile uByte slpar;	/* 44 */
              volatile uByte swide;	/* 45 */
              volatile uByte macntl;	/* 46 */
              volatile uByte gpcntl;	/* 47 */
              volatile uByte stime0;	/* 48 */
              volatile uByte stime1;	/* 49 */
              volatile uByte respid0;	/* 4A */
              volatile uByte respid1;	/* 4B */
              volatile uByte stest0;	/* 4C */
              volatile uByte stest1;	/* 4D */
              volatile uByte stest2;	/* 4E */
              volatile uByte stest3;	/* 4F */
              volatile uLong sidl;	/* 50 */
              volatile uLong sodl;	/* 54 */
              volatile uLong sbdl;	/* 58 */
              volatile uLong scratchb;	/* 5C */
              volatile uLong scratchc;	/* 60 */
              volatile uLong scratchd;	/* 64 */
              volatile uLong scratche;	/* 68 */
              volatile uLong scratchf;	/* 6C */
              volatile uLong scratchg;	/* 70 *//* pci addr of current scsi_id_table entry */
              volatile uLong scratchh;	/* 74 *//* host virt addr of current request packet */
              volatile uLong scratchi;	/* 78 *//* pci addr of current request packet */
              volatile uByte scratchj0;	/* 7C *//* state */
              volatile uByte scratchj1;	/* 7D *//* non-zero if CHMOV/MOVE's being executed */
              volatile uByte scratchj2;	/* 7E *//* scsi index being processed */
              volatile uByte scratchj3;	/* 7F *//* next scsi index to be checked */
            };

/* SCRIPTS code */

#include "oz_dev_lsil875_486.out"

/************************************************************************/
/*									*/
/*  Boot-time initialization routine					*/
/*									*/
/************************************************************************/

void oz_dev_lsil875_init ()

{
  char unitdesc[OZ_DEVUNIT_DESCSIZE], unitname[OZ_DEVUNIT_NAMESIZE];
  Devex *devex;
  int init;
  OZ_Dev_pci_conf_p pciconfp;
  OZ_Devunit *devunit;
  OZ_Mempage ram_page, reg_page;
  uByte intvec, my_scsi_id, *ram_va, *reg_va;
  uLong didvid, ram_pciaddr, reg_pciaddr, sts;
  void *svadr;

  if (initialized) return;

  oz_knl_printk ("oz_dev_lsil875_init\n");
  initialized = 1;

  devclass  = oz_knl_devclass_create (OZ_IO_SCSI_CLASSNAME, OZ_IO_SCSI_BASE, OZ_IO_SCSI_MASK, "lsil875_486");
  devdriver = oz_knl_devdriver_create (devclass, "lsil875_486");

  oz_hw_smplock_init (sizeof rpmemlist_smplock, &rpmemlist_smplock, OZ_SMPLOCK_LEVEL_NP);

  /* Scan pci for lsil53c875 chips */

  for (init = 1; oz_dev_pci_conf_scan_didvid (&pciconfp, init, 0x000F1000); init = 0) {
    if (pciconfp.pcifunc != 0) continue;

    oz_knl_printk ("oz_dev_lsil875: scsi controller found: bus/device/function %u/%u/%u\n", pciconfp.pcibus, pciconfp.pcidev, pciconfp.pcifunc);

#if 00
    oz_knl_printk ("oz_dev_lsil875*: original pci regs:\n");
    for (i = 0; i < 0x80; i += 0x10) {
      for (j = 16; j > 0;) {
        j -= 4;
        oz_knl_printk (" %8.8x", oz_dev_pci_conf_inl (&pciconfp, i + j));
      }
      oz_knl_printk (" : %2.2x\n", i);
    }
#endif

    /* Set chip enable bits - enable memory access, enable bus mastering, enable cache line size, enable parity checking */

    oz_dev_pci_conf_outw (0x0156, &pciconfp, OZ_DEV_PCI_CONF_W_PCICMD);

    /* Create device unit struct and fill it in */

#if OZ_HW_L2PAGESIZE != 12
    error : code assumes 4k page size
#endif

    sts = oz_knl_spte_alloc (2, &svadr, NULL, NULL);
    if (sts != OZ_SUCCESS) oz_crash ("oz_dev_lsil875 init: error %u allocating 2 sys virt pages", sts);

    reg_pciaddr = oz_dev_pci_conf_inl (&pciconfp, OZ_DEV_PCI_CONF_L_BASADR1);
    reg_page    = reg_pciaddr >> OZ_HW_L2PAGESIZE;
    reg_va      = svadr;
    reg_va     += reg_pciaddr & ((1 << OZ_HW_L2PAGESIZE) - 1);
    oz_hw_map_iopage (reg_page, reg_va);

    ram_pciaddr = oz_dev_pci_conf_inl (&pciconfp, OZ_DEV_PCI_CONF_L_BASADR2);
    ram_page    = ram_pciaddr >> OZ_HW_L2PAGESIZE;
    ram_va      = svadr;
    ram_va     += 1 << OZ_HW_L2PAGESIZE;
    oz_hw_map_iopage (ram_page, ram_va);

    intvec = oz_dev_pci_conf_inb (&pciconfp, OZ_DEV_PCI_CONF_B_INTLINE);	/* get irq number assigned by bios */
    my_scsi_id = ((Regs *)reg_va) -> scid & 0x0F;				/* get scsi-id assigned by bios */

    oz_knl_printk ("oz_dev_lsil875: - reg pa %8.8x, va %8.8p\n", reg_pciaddr, reg_va);
    oz_knl_printk ("oz_dev_lsil875: - ram pa %8.8x, va %8.8p\n", ram_pciaddr, ram_va);
    oz_knl_printk ("oz_dev_lsil875: - irq vector %u, controller's scsi id %u\n", intvec, my_scsi_id);

    oz_sys_sprintf (sizeof unitname, unitname, UNITNAME_PREFIX "_%u_%u", pciconfp.pcibus, pciconfp.pcidev);
    oz_sys_sprintf (sizeof unitdesc, unitdesc, "53C875 irq %u, scsi_id %u, reg_pa %x, (va %p)", 
                                                intvec, my_scsi_id, reg_pciaddr, reg_va);

    devunit = oz_knl_devunit_create (devdriver, unitname, unitdesc, &lsil875_functable, 0, oz_s_secattr_sysdev);
    devex   = oz_knl_devunit_ex (devunit);					/* get pointer to extension area */
    memset (devex, 0, sizeof *devex);						/* clear it out */
    devex -> devunit = devunit;							/* save devunit pointer */
    devex -> name    = oz_knl_devunit_devname (devunit);			/* save devname string pointer */
    devex -> intvec  = intvec;							/* save IRQ number */
    devex -> scsi_id = my_scsi_id;						/* save controller's scsi id */
    devex -> irq_many.entry = chip_interrupt;					/* link to shared IRQ vector */
    devex -> irq_many.param = devex;
    devex -> irq_many.descr = devex -> name;
    devex -> smplock = oz_hw486_irq_many_add (devex -> intvec, &(devex -> irq_many));
    devex -> regs    = (Regs *)reg_va;						/* virtual address the registers are at */
    devex -> sram_va = ram_va;							/* virtual address the internal ram is at */
    devex -> sram_pa = ram_pciaddr;						/* pci address of the internal ram */
    devex -> se_va   = (Se *)(ram_va + Ent_scsi_id_table);			/* scsi-id table address in the ram */
    devex -> timeouttimer = oz_knl_timer_alloc ();				/* allocate a timer struct for timeouts */
    devex -> startuptimer = oz_knl_timer_alloc ();				/* allocate a timer struct for startupdelay */
    devex -> startingqt   = &(devex -> startingqh);				/* init startup delay queue */

    /* Start controller going - normally it is running.  It only halts   */
    /* on error conditions but then this driver starts it right back up. */

#if 00
    oz_knl_printk ("oz_dev_lsil875*: original registers:\n");
    oz_knl_dumpmem (sizeof *(devex -> regs), devex -> regs);
#endif
    chip_enable (devex);

    /* Finally, set up an autogen routine to automatically configure devices on the bus */

    oz_knl_devunit_autogen (devunit, oz_dev_scsi_auto, NULL);
  }
}

/************************************************************************/
/*									*/
/*  Shutdown device - system is about to be rebooted			*/
/*									*/
/************************************************************************/

static int lsil875_shutdown (OZ_Devunit *devunit, void *devexv)

{
  chip_reset (devexv);			/* do software reset followed by scsi reset */
  return (1);
}

/************************************************************************/
/*									*/
/*  A new channel was assigned to the device				*/
/*									*/
/************************************************************************/

static uLong lsil875_assign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode)

{
  Devex *devex;
  uLong dv;

  devex = devexv;

  /* Make sure the intvec connected ok */

  if (devex -> smplock == NULL) {
    oz_knl_printk ("oz_dev_lsil875 assign: irq conflict\n");
    return (OZ_DEVOFFLINE);
  }

  /* Turn chip back online if it crashed */

  if (devex -> devstate == DEVSTATE_OFFLINE) {
    dv = oz_hw_smplock_wait (devex -> smplock);
    if (devex -> devstate == DEVSTATE_OFFLINE) chip_enable (devex);
    oz_hw_smplock_clr (devex -> smplock, dv);
  }

  /* Clear chnex area and return successful status */

  memset (chnexv, 0, sizeof (Chnex));
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  A channel, possibly open, is being deassigned			*/
/*  We can safely assume there is no I/O going on for the scsi_id	*/
/*									*/
/************************************************************************/

static int lsil875_deassign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv)

{
  Chnex *chnex;
  Devex *devex;
  OZ_Lockmode lockmode;
  uLong dv, scsi_id;

  chnex = chnexv;
  devex = devexv;
  dv = oz_hw_smplock_wait (devex -> smplock);
  if (chnex -> open) {
    scsi_id  = chnex -> scsi_id;
    lockmode = chnex -> lockmode;
    if (!OZ_LOCK_ALLOW_TEST (lockmode, OZ_LOCK_ALLOWS_OTHERS_READ))  devex -> deny_read[scsi_id]  --;
    if (!OZ_LOCK_ALLOW_TEST (lockmode, OZ_LOCK_ALLOWS_OTHERS_WRITE)) devex -> deny_write[scsi_id] --;
    if  (OZ_LOCK_ALLOW_TEST (lockmode, OZ_LOCK_ALLOWS_SELF_READ))    devex -> refc_read[scsi_id]  --;
    if  (OZ_LOCK_ALLOW_TEST (lockmode, OZ_LOCK_ALLOWS_SELF_WRITE))   devex -> refc_write[scsi_id] --;
    chnex -> open = 0;
  }
  oz_hw_smplock_clr (devex -> smplock, dv);

  return (0);
}

/************************************************************************/
/*									*/
/*  Abort all requests on the given channel				*/
/*									*/
/************************************************************************/

static void lsil875_abort (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, 
                           OZ_Ioop *ioop, void *iopexv, OZ_Procmode procmode)

{
  Chnex *chnex;
  Devex *devex;
  int kickit;
  Rp **lrp, *rp, *xrp;
  uLong dv, scsi_id;

  chnex  = chnexv;
  devex  = devexv;
  kickit = 0;

  dv = oz_hw_smplock_wait (devex -> smplock);					/* inhibit interrupt delivery */

  /* Remove any from the startingq */

  xrp = NULL;
  for (lrp = &(devex -> startingqh); (rp = *lrp) != NULL;) {
    if (oz_knl_ioabortok (rp -> rp_iopex -> ioop, iochan, procmode, ioop)) {	/* see if this request should be aborted */
      *lrp = rp -> rp_next_va;
      rp -> rp_next_va = xrp;
      xrp = rp;
    } else {
      lrp = &(rp -> rp_next_va);
    }
  }
  devex -> startingqt = lrp;

  /* If any are queued to the controller, just tag them for abort and let the contoller cycle through it. */
  /* Do it this way in case the controller is working on it already or is just about to start on it.      */

  if (chnex -> open) {								/* see if anything open on channel */
    scsi_id = chnex -> scsi_id;							/* ok, so it has a scsi_id that is valid */
    for (rp = devex -> firstrp[scsi_id]; rp != NULL; rp = rp -> rp_next_va) {	/* scan through the scsi_id's requeust queue */
      if (oz_knl_ioabortok (rp -> rp_iopex -> ioop, iochan, procmode, ioop)) {	/* see if this request should be aborted */
        if (rp -> rp1_abort == 0) {						/* if so, see if it already has been aborted */
          rp -> rp1_abort = RP_ABORT_IOABORT;					/* if not, flag it to be aborted */
          kickit = 1;								/* remember to set SIGP when we're done */
        }
      }
    }
    if (kickit) devex -> regs -> istat = 0x20;					/* if we aborted anything, set SIGP ... */
										/* ... to wake the scripts processor up */
  }

  oz_hw_smplock_clr (devex -> smplock, dv);					/* enable interrupt delivery */

  /* Post aborted requests from startingq */

  while ((rp = xrp) != NULL) {
    xrp = rp -> rp_next_va;
    oz_knl_iodone (rp -> rp_iopex -> ioop, OZ_ABORTED, NULL, NULL, NULL);
    free_rp (rp);
  }
}

/************************************************************************/
/*									*/
/*  Start performing an scsi I/O function				*/
/*									*/
/************************************************************************/

static uLong lsil875_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode, 
                            OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap)

{
  Chnex *chnex;
  Devex *devex;
  Iopex *iopex;

  chnex = chnexv;
  devex = devexv;
  iopex = iopexv;

  iopex -> devex    = devex;
  iopex -> ioop     = ioop;
  iopex -> procmode = procmode;

  switch (funcode) {

    /* Declare what scsi_id and what lockmode is to be associated with the channel */

    case OZ_IO_SCSI_OPEN: {
      OZ_IO_scsi_open scsi_open;
      OZ_Lockmode iochlkm, lockmode;
      uLong dv, scsi_id, sts;

      /* Retrieve and validate parameters */

      movc4 (as, ap, sizeof scsi_open, &scsi_open);
      scsi_id  = scsi_open.scsi_id;
      lockmode = scsi_open.lockmode;
      if (scsi_id >= MAX_SCSI_ID) return (OZ_BADSCSIID);
      if (scsi_id == devex -> scsi_id) return (OZ_BADSCSIID);

      iochlkm = oz_knl_iochan_getlockmode (iochan);
      if (OZ_LOCK_ALLOW_TEST (lockmode, OZ_LOCK_ALLOWS_SELF_WRITE) 
       && !OZ_LOCK_ALLOW_TEST (iochlkm, OZ_LOCK_ALLOWS_SELF_WRITE)) 
        return (OZ_NOWRITEACCESS);
      if (OZ_LOCK_ALLOW_TEST (lockmode, OZ_LOCK_ALLOWS_SELF_READ) 
       && !OZ_LOCK_ALLOW_TEST (iochlkm, OZ_LOCK_ALLOWS_SELF_READ)) 
        return (OZ_NOREADACCESS);

      /* Make sure the chip hasn't crashed */

      if (devex -> devstate == DEVSTATE_OFFLINE) return (OZ_DEVOFFLINE);

      /* Mark the channel open and lock access to the scsi-id */

      dv = oz_hw_smplock_wait (devex -> smplock);
      if (chnex -> open) sts = OZ_FILEALREADYOPEN;
      else {
        if ((!OZ_LOCK_ALLOW_TEST (lockmode, OZ_LOCK_ALLOWS_OTHERS_READ)  && (devex -> refc_read[scsi_id]  != 0)) 
         || (!OZ_LOCK_ALLOW_TEST (lockmode, OZ_LOCK_ALLOWS_OTHERS_WRITE) && (devex -> refc_write[scsi_id] != 0)) 
         ||  (OZ_LOCK_ALLOW_TEST (lockmode, OZ_LOCK_ALLOWS_SELF_READ)    && (devex -> deny_read[scsi_id]  != 0)) 
         ||  (OZ_LOCK_ALLOW_TEST (lockmode, OZ_LOCK_ALLOWS_SELF_WRITE)   && (devex -> deny_write[scsi_id] != 0))) {
          sts = OZ_ACCONFLICT;
        } else {
          if (!OZ_LOCK_ALLOW_TEST (lockmode, OZ_LOCK_ALLOWS_OTHERS_READ))  devex -> deny_read[scsi_id]  ++;
          if (!OZ_LOCK_ALLOW_TEST (lockmode, OZ_LOCK_ALLOWS_OTHERS_WRITE)) devex -> deny_write[scsi_id] ++;
          if  (OZ_LOCK_ALLOW_TEST (lockmode, OZ_LOCK_ALLOWS_SELF_READ))    devex -> refc_read[scsi_id]  ++;
          if  (OZ_LOCK_ALLOW_TEST (lockmode, OZ_LOCK_ALLOWS_SELF_WRITE))   devex -> refc_write[scsi_id] ++;
          chnex -> scsi_id  = scsi_open.scsi_id;
          chnex -> lockmode = scsi_open.lockmode;
          chnex -> open = 1;
          sts = OZ_SUCCESS;
        }
      }
      oz_hw_smplock_clr (devex -> smplock, dv);
      return (sts);
    }

    /* Queue an I/O request to the scsi_id open on the channel */

    case OZ_IO_SCSI_DOIO: {
      OZ_IO_scsi_doio scsi_doio;
      uLong sts;

      movc4 (as, ap, sizeof scsi_doio, &scsi_doio);
      sts = oz_dev_scsi_cvtdoio2pp (ioop, procmode, &scsi_doio, &(iopex -> doiopp));
      if (sts == OZ_SUCCESS) sts = qiopp (chnex, iopex);
      return (sts);
    }

    /* - This one already has the buffers locked in memory */

    case OZ_IO_SCSI_DOIOPP: {
      if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);
      movc4 (as, ap, sizeof iopex -> doiopp, &(iopex -> doiopp));
      return (qiopp (chnex, iopex));
    }

    /* Get info, part 1 */

    case OZ_IO_SCSI_GETINFO1: {
      OZ_IO_scsi_getinfo1 scsi_getinfo1;
      Se *se;
      uLong sts;

      sts = oz_knl_ioop_lockw (ioop, as, ap, NULL, NULL, NULL);
      if (sts == OZ_SUCCESS) {
        memset (&scsi_getinfo1, 0, sizeof scsi_getinfo1);
        scsi_getinfo1.max_scsi_id    = MAX_SCSI_ID;				/* max scsi id allowed on this controller */
        scsi_getinfo1.ctrl_scsi_id   = devex -> scsi_id;			/* what the controller's scsi id is */
        scsi_getinfo1.open_scsi_id   = -1;					/* assume no scsi id open on channel */
        if (chnex -> open) {
          scsi_getinfo1.open_scsi_id = chnex -> scsi_id;			/* ok, get the open scsi id */
          se = devex -> se_va + scsi_getinfo1.open_scsi_id;			/* point to its scsi_id_table entry */
          scsi_getinfo1.open_width   = (se -> se3_scntl3 >> 3) & 1;		/* width: 0=8-bit, 1=16-bit */
          scsi_getinfo1.open_speed   = (25 << ((se -> se3_scntl3 >> 5) & 3)) / 2; /* speed: 12=50nS, 25=100nS, 50=200nS */
          scsi_getinfo1.open_raofs   = se -> se1_sxfer & 0x1F;			/* raofs: 0=async, else # of bytes/words */
        }
        movc4 (sizeof scsi_getinfo1, &scsi_getinfo1, as, ap);
      }
      return (sts);
    }

    case OZ_IO_SCSI_RESET: {
      uLong dv;

      oz_knl_printk ("oz_dev_lsil875: resetting %s\n", devex -> name);
      dv = oz_hw_smplock_wait (devex -> smplock);
      chip_abort (devex, 0);
      chip_enable (devex);
      oz_hw_smplock_clr (devex -> smplock, dv);
      return (OZ_SUCCESS);
    }

    /* Set this controller up for crash dumping */

    case OZ_IO_SCSI_CRASH: {
      if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);
      if (crash_iochan != NULL) {
        oz_knl_devunit_increfc (crash_iochan, -1);
        crash_iochan  = NULL;
        crash_devex   = NULL;
        crash_scsi_id = -1;
      }
      if (ap != NULL) {
        if (as != sizeof (OZ_IO_scsi_crash)) return (OZ_BADBUFFERSIZE);
        if (crash_rp == NULL) {
          crash_rp = malloc_rp (CRASH_NUMPAGES);
          if (crash_rp == NULL) {
            oz_knl_printk ("oz_dev_lsil875 crash: can't pre-allocate request packet - CRASH NUMPAGES too big\n");
            return (OZ_BADBUFFERSIZE);
          }
        }
        crash_iochan  = iochan;
        crash_devex   = devex;
        crash_scsi_id = chnex -> scsi_id;
        oz_knl_iochan_increfc (crash_iochan, 1);
        ((OZ_IO_scsi_crash *)ap) -> crashentry = lsil875_crash;
        ((OZ_IO_scsi_crash *)ap) -> crashparam = NULL;
      }
      return (OZ_SUCCESS);
    }
  }

  return (OZ_BADIOFUNC);
}

/************************************************************************/
/*									*/
/*  Crash dump routine - performs the indicated scsi function with 	*/
/*  interrupts disabled - any I/O's that may be going on at the time 	*/
/*  are lost.								*/
/*									*/
/************************************************************************/

static uLong lsil875_crash (void *dummy, OZ_IO_scsi_doiopp *doiopp)

{
  OZ_Datebin now;
  uLong sts;

  /* First time through (or controller not online), reset and restart the controller */

  if (!crash_inprog || (crash_devex -> devstate == DEVSTATE_OFFLINE)) {
    crash_inprog = 1;
    chip_enable (crash_devex);
  }

  /* Now that the controller is idle, clear out all internal queues */

  crash_devex -> smplock      = NULL;					/* can't use smplocks anymore */
  crash_devex -> timeouttimer = NULL;					/* can't use timers anymore */
  crash_devex -> startuptimer = NULL;
  memset (crash_devex -> firstrp, 0, sizeof crash_devex -> firstrp);	/* get rid of anything laying around in here */
  memset (crash_devex -> lastrp,  0, sizeof crash_devex -> lastrp);	/* get rid of anything laying around in here */

  /* Fill in our request param block and queue to controller */

  memset (&crash_iopex, 0, sizeof crash_iopex);
  crash_iopex.devex        = crash_devex;
  crash_iopex.doiopp       = *doiopp;
  crash_iopex.datanumpages = calc_datanumpages (doiopp);
  crash_iopex.scsi_id      = crash_scsi_id;
  if (crash_iopex.datanumpages > CRASH_NUMPAGES) {
    oz_knl_printk ("oz_dev_lsil875_crash: transfer requires %u pages, max %u allowed\n", crash_iopex.datanumpages, CRASH_NUMPAGES);
    return (OZ_BADBUFFERSIZE);
  }
  fill_in_rp (crash_rp, &crash_iopex);
  queue_request (crash_rp);

  /* Repeatedly call interrupt routine until the request completes or times out */

  while (crash_rp -> rp_next_va != crash_rp) {
    oz_hw_stl_nanowait (5000);								/* not too fast or chip pukes */
    if (OZ_HW_DATEBIN_TST (crash_rp -> rp_timesout)) {					/* see if timeout parameter given */
      now = oz_hw_timer_getnow ();							/* ok, see what time it is now */
      if (OZ_HW_DATEBIN_CMP (crash_rp -> rp_timesout, now) <= 0) {			/* see if it timed out */
        if (crash_rp -> rp1_abort == 0) crash_rp -> rp1_abort = RP_ABORT_TIMEOUT;	/* it timed out, mark it so */
        oz_knl_printk ("oz_dev_lsil875_crash: request timed out\n");
        chip_reset (crash_devex);							/* reset the controller */
        break;										/* ... and the request is done now */
      }
    }
    chip_interrupt (crash_devex, NULL);							/* not timed out, see if chip is done */
  }

  /* Return status byte and data length */

  if (doiopp -> status   != NULL) *(doiopp -> status)   = crash_rp -> rp2_seqsts;
  if (doiopp -> datarlen != NULL) *(doiopp -> datarlen) = calc_datarlen (crash_rp);

  /* Return completion status */

  sts = calc_status (crash_rp);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Prepare request packet and queue it to controller			*/
/*									*/
/************************************************************************/

static uLong qiopp (Chnex *chnex, Iopex *iopex)

{
  Devex *devex;
  Rp *rp;
  OZ_IO_scsi_doiopp *doiopp;
  uLong dv;

  devex  = iopex -> devex;
  doiopp = &(iopex -> doiopp);

  /* Validate parameters */

  if (!(chnex -> open)) return (OZ_FILENOTOPEN);
  if (!OZ_LOCK_ALLOW_TEST (chnex -> lockmode, OZ_LOCK_ALLOWS_SELF_WRITE)) return (OZ_NOWRITEACCESS);

  if ((doiopp -> cmdlen == 0) || (doiopp -> cmdlen > sizeof rp -> rp_command)) return (OZ_BADBUFFERSIZE);
  if (doiopp -> datasize >= (1 << 24)) return (OZ_BADBUFFERSIZE);

  /* Save I/O request's scsi-id */

  iopex -> scsi_id = chnex -> scsi_id;

  /* Allocate a physically contiguous request packet */

  iopex -> datanumpages = calc_datanumpages (doiopp);
  rp = malloc_rp (iopex -> datanumpages);
  if (rp == NULL) return (OZ_BADBUFFERSIZE);

  /* Fill in the request packet */

  fill_in_rp (rp, iopex);

  /* Queue the request packet to the controller for processing */

  dv = oz_hw_smplock_wait (devex -> smplock);
  switch (devex -> devstate) {

    /* Controller is halted, free request packet and return error status */

    case DEVSTATE_OFFLINE: {
      oz_hw_smplock_clr (devex -> smplock, dv);
      free_rp (rp);
      return (OZ_DEVOFFLINE);
    }

    /* Waiting for the two seconds to expire, put request in a holding queue */

    case DEVSTATE_STARTING: {
      *(devex -> startingqt) = rp;
      devex -> startingqt = &(rp -> rp_next_va);
      break;
    }

    /* Chip is processing requests, queue request to chip for processing */

    case DEVSTATE_ONLINE: {
      queue_request (rp);
      break;
    }

    /* How did we get in this state? */

    default: oz_crash ("oz_dev_lsil875: invalid devstate %u", devex -> devstate);
  }
  oz_hw_smplock_clr (devex -> smplock, dv);
  return (OZ_STARTED);
}

/************************************************************************/
/*									*/
/*  Allocate physically contiguous request packet			*/
/*									*/
/*  If needed, someday expand to handle a fractured rp by chaining the 	*/
/*  data CHMOV/MOVE instructions with intervening JUMPs			*/
/*									*/
/************************************************************************/

static Rp *malloc_rp (uLong datanumpages)

{
  int i;
  OZ_Mempage phypage, sysvpage;
  OZ_Memsize rpblocksize, rsize;
  Rp *rp;
  uLong np, pm, sts;
  void *sysvaddr;

  rpblocksize = datanumpages * (sizeof rp -> rp_datamovs[0]) + (sizeof *rp);
  rp = oz_malloc (rpmemlist, rpblocksize, &rsize, 0);
  if (rp == NULL) {

    /* Allocate som spte's for it to have virtual addresses */

    sts = oz_knl_spte_alloc (RP_NPAGES, &sysvaddr, &sysvpage, NULL);
    if (sts != OZ_SUCCESS) {
      oz_knl_printk ("oz_dev_lsil875 malloc_rp: error %u allocating %u page spte block\n", sts, RP_NPAGES);
      return (NULL);
    }

    /* Allocate physically contiguous pages */

    pm = oz_hw_smplock_wait (&oz_s_smplock_pm);
    phypage = oz_knl_phymem_allocontig (RP_NPAGES, OZ_PHYMEM_PAGESTATE_ALLOCSECT);
    oz_hw_smplock_clr (&oz_s_smplock_pm, pm);
    if (phypage == OZ_PHYPAGE_NULL) {
      oz_knl_printk ("oz_dev_lsil875 malloc_rp: no physical memory left\n");
      oz_knl_spte_free (RP_NPAGES, sysvpage);
      return (NULL);
    }

    /* Map those pages to the allocated virtual addresses, but with caching disabled */

    for (i = 0; i < RP_NPAGES; i ++) {
      oz_hw_map_iopage (phypage + i, ((uByte *)sysvaddr) + (i << OZ_HW_L2PAGESIZE));
    }

    /* Link those new pages to the existing free memory list */

    np = oz_hw_smplock_wait (&rpmemlist_smplock);
    rpmemlist_total += RP_NPAGES << OZ_HW_L2PAGESIZE;
    oz_knl_printk ("oz_dev_lsil875 malloc_rp*: new va %p, phypage %X, total %uK, inuse %uK+%u\n", 
	sysvaddr, phypage, rpmemlist_total >> 10, rpmemlist_inuse >> 10, rpblocksize);
    rpmemlist = oz_freesiz (rpmemlist, 
                            RP_NPAGES << OZ_HW_L2PAGESIZE, 
                            sysvaddr, 
                            (void *)oz_hw_smplock_wait, 
                            (void *)oz_hw_smplock_clr, 
                            &rpmemlist_smplock);
    oz_hw_smplock_clr (&rpmemlist_smplock, np);

    /* Try to allocate.  If we fail, assume it's because they are asking for too big of a block */

    rp = oz_malloc (rpmemlist, rpblocksize, &rsize, 0);
  }

  if (rp != NULL) oz_hw_atomic_inc_long (&rpmemlist_inuse, rsize);

  return (rp);
}

/************************************************************************/
/*									*/
/*  Fill in request packet from iopex -> doiopp stuff			*/
/*									*/
/************************************************************************/

static void fill_in_rp (Rp *rp, Iopex *iopex)

{
  OZ_IO_scsi_doiopp *doiopp;

  doiopp = &(iopex -> doiopp);

  rp -> rp_this_va  = rp;					/* save its virtual address */
  rp -> rp_next_pa  = 0;					/* clear link to next in se_queue_head list */
  rp -> rp_next_va  = NULL;					/* clear link to next in devex -> startingqh or firstrp list */
  rp -> rp0_flags   = RP_FLAG_NEEDTOIDENT;			/* set up flags byte */
  if (doiopp -> optflags & OZ_IO_SCSI_OPTFLAG_NEGO_WIDTH) rp -> rp0_flags |= RP_FLAG_NEGWIDTH;
  if (doiopp -> optflags & OZ_IO_SCSI_OPTFLAG_NEGO_SYNCH) rp -> rp0_flags |= RP_FLAG_NEGSYNCH;
  if (doiopp -> optflags & OZ_IO_SCSI_OPTFLAG_DISCONNECT) rp -> rp0_flags |= RP_FLAG_DISCONNECT;
  rp -> rp1_abort   = 0;					/* we haven't aborted it yet */
  rp -> rp3_cmdlen  = doiopp -> cmdlen;				/* save command length and copy in command */
  memcpy (rp -> rp_command, doiopp -> cmdbuf, doiopp -> cmdlen);
  rp -> rp_iopex    = iopex;					/* save pointer back to iopex */

  setupdatamovs (rp);						/* set up data CHMOV/MOVE instrs */

  rp -> rp_timesout = 0;					/* fill in time-out time */
  if (doiopp -> timeout != 0) {
    rp -> rp_timesout  = oz_hw_timer_getnow ();
    rp -> rp_timesout += doiopp -> timeout * (OZ_TIMER_RESOLUTION / 1000);
  }
}

/************************************************************************/
/*									*/
/*  Free request packet							*/
/*									*/
/************************************************************************/

static void free_rp (Rp *rp)

{
  OZ_Memsize rsize;

  rsize = oz_free (rpmemlist, rp);
  oz_hw_atomic_inc_long (&rpmemlist_inuse, -rsize);
}

/************************************************************************/
/*									*/
/*  Queue request							*/
/*									*/
/*    Input:								*/
/*									*/
/*	devex = device extension struct pointer				*/
/*	rp = request packet (rp0_flags already set up)			*/
/*	smplevel = device interrupts inhibited				*/
/*									*/
/************************************************************************/

static void queue_request (Rp *rp)

{
  Devex *devex;
  Iopex *iopex;
  Regs *regs;
  Rp *lastrp;
  Se *se;
  uLong rp_pa1, scsi_id;

  iopex   = rp -> rp_iopex;
  devex   = iopex -> devex;
  scsi_id = iopex -> scsi_id;

  iopex -> queueflg = 0;

  regs = devex -> regs;					/* point to controller's registers */

  rp -> rp2_seqsts = (devex -> queueseq[scsi_id]) ++;	/* assign its sequence */
  lastrp = devex -> lastrp[scsi_id];			/* get last rp queued to device */
  devex -> lastrp[scsi_id] = rp;			/* make this new one the last one now */
  if (lastrp != NULL) lastrp -> rp_next_va = rp;
#if ONEATATIME
  else {
    iopex -> queueflg = 1;
    devex -> firstrp[scsi_id] = rp;
    start_request (devex, scsi_id);
  }
#else
  else devex -> firstrp[scsi_id] = rp;

  rp_pa1 = npp_virt_to_phys (sizeof *rp, rp) | 1;	/* get pci address of request packet with low bit set */
							/* low bit set indicates it is a new request */
  se = devex -> se_va + scsi_id;			/* get virt address of scsi_id_table entry */
  iopex -> queueflg ++;	// 0x01
  OZ_HW_MB;
  if (se -> se_queue_head == 0) goto queue_empty;	/* see if the queue head is empty */
							/* that means controller isn't doing anything with this device */
							/* so we can simply stick it on the head of the queue */

  if (lastrp == NULL) {					/* controller is busy doing something, there better be a 'last request' */
    oz_knl_printk ("oz_dev_lsil875: %s[%u] controller busy but no requests queued\n", devex -> name, scsi_id);
    chip_abort (devex, 0);
    return;
  }
  OZ_HW_MB;
  lastrp -> rp_next_pa = rp_pa1;			/* ok, link me on to end of it */
  OZ_HW_MB;
  iopex -> queueflg ++;	// 0x02
  if (se -> se_queue_head != 0) goto success;		/* if controller is still busy, nothing more to do */
							/* (it is still either doing an old request or this new one) */
							/* controller is no longer busy, either it didn't see this request at */
							/* all or it saw it and has finished it already */
  OZ_HW_MB;
  if (regs -> istat & 0x10) {				/* wait for SEM flag to clear so we can see a proper RP_FLAG_DONE bit */
							/* (it is possible that controller just finished the new */
							/*  request and has cleared se_queue_head but hasn't set the */
							/*  RP_FLAG_DONE bit yet) */
    if (!oz_hw_stl_microwait (100, check_sem_bit, regs)) { /* but only wait a max of 100uS */
      oz_knl_printk ("oz_dev_lsil875: %s hung with SEM bit set\n", devex -> name);
      chip_abort (devex, 0);
      return;
    }
    iopex -> queueflg += 0x10;
  }

  iopex -> queueflg ++;	// 0x03 or 0x13
  OZ_HW_MB;
  if (rp -> rp0_flags & RP_FLAG_DONE) goto success;	/* it's idle, if it finished me off, nothing more to do */
							/* otherwise, it went idle just as new request was being queued */
							/* ... so just queue it to the controller */
  iopex -> queueflg ++;	// 0x04 or 0x14
queue_empty:
  OZ_HW_MB;
  se -> se_queue_head = rp_pa1;				/* if so, link to top of queue */
  OZ_HW_MB;
  regs -> istat = 0x20;					/* ... and tell it something needs to be started */
success:
#endif
  if ((rp -> rp_timesout != 0) && !crash_inprog) {	/* see if request has a timeout setting */
    if ((devex -> nextimeout == 0) || (rp -> rp_timesout < devex -> nextimeout)) { /* see if timer needs to be (re)set */
      devex -> nextimeout = rp -> rp_timesout;		/* if so, remember what we are setting it to */
      oz_knl_timer_insert (devex -> timeouttimer, rp -> rp_timesout, timeout, devex); /* (re)set the timer */
    }
  }
}

/* This routine returns 0 if SEM bit is still set, non-zero if SEM bit is clear */

static uLong check_sem_bit (void *regsv)

{
  return (!(((Regs *)regsv) -> istat & 0x10));
}

/************************************************************************/
/*									*/
/*  Timeout routine - this routine is called at softint level when the 	*/
/*  devex's timer expires.						*/
/*									*/
/*  It scans through all queues for requests that are timed out.  For 	*/
/*  those that it finds, it just marks the request packets with 	*/
/*  RP_ABORT_TIMEOUT.  Then it sets SIGP to wake the controller.  The 	*/
/*  controller must go through its paces to see requests flagged this 	*/
/*  way and complete them as soon as it can.				*/
/*									*/
/************************************************************************/

static void timeout (void *devexv, OZ_Timer *timer)

{
  Devex *devex;
  int kickit;
  OZ_Datebin nextimeout, now;
  Rp *rp;
  uLong dv, scsi_id;

  OZ_HW_DATEBIN_CLR (nextimeout);
  devex  = devexv;
  kickit = 0;
  now    = oz_hw_timer_getnow ();

  dv = oz_hw_smplock_wait (devex -> smplock);					/* inhibit interrupt delivery */
  for (scsi_id = 0; scsi_id < MAX_SCSI_ID; scsi_id ++) {			/* scan through all scsi-id queues */
    for (rp = devex -> firstrp[scsi_id]; rp != NULL; rp = rp -> rp_next_va) {	/* scan through all pending requests */
      if ((rp -> rp1_abort == 0) && OZ_HW_DATEBIN_TST (rp -> rp_timesout)) {
        if (OZ_HW_DATEBIN_CMP (rp -> rp_timesout, now) <= 0) {
          rp -> rp1_abort = RP_ABORT_TIMEOUT;					/* if it timed out, mark it so */
          kickit = 1;								/* remember to wake controller */
        } else if (!OZ_HW_DATEBIN_TST (nextimeout) || (OZ_HW_DATEBIN_CMP (rp -> rp_timesout, nextimeout) < 0)) {
          nextimeout = rp -> rp_timesout;					/* it has yet to timeout, remember when */
        }
      }
    }
  }
  if (kickit) devex -> regs -> istat = 0x20;					/* if we aborted anything, wake controller */

  devex -> nextimeout = nextimeout;						/* see if there is anything yet to timeout */
  if (OZ_HW_DATEBIN_TST (nextimeout)) {
    oz_knl_timer_insert (devex -> timeouttimer, nextimeout, timeout, devex);	/* if so, start the timer for them */
  }

  oz_hw_smplock_clr (devex -> smplock, dv);					/* enable interrupt delivery */
}

/************************************************************************/
/*									*/
/*  Interrupt routine							*/
/*									*/
/************************************************************************/

static int chip_interrupt (void *devexv, OZ_Mchargs *mchargs)

{
  uByte dstat, istat, sist0, sist1;
  Devex *devex;
  Regs *regs;

  devex = devexv;
  regs  = devex -> regs;

  /* The book says:                          */
  /*   1) read istat                         */
  /*   2) process intfly if indicated        */
  /*   3) if sip set, read sist0 and sist1   */
  /*   4) if dip set, read dstat             */
  /*   5) if dip set, process dma interrupt  */
  /*   6) if sip set, process scsi interrupt */
  /* must wait 12 clock periods between      */
  /* reading status bit registers            */

  istat = regs -> istat;		/* read the interrupt status register */

  if (istat & 0x04) {			/* maybe an interrupt-on-the-fly happened */
    oz_hw_stl_nanowait (300);		/* wait 12 clock periods (12 @ 40 MHz = 300nS) */
    regs -> istat = 0x04;		/* if so, clear it */
    done_interrupt (devex);		/* ... then process it (a scsi op completed somewhere) */
  }

  devex -> resumedsp = 0;
  switch (istat & 0x03) {

    /* No interrupt, just get out */

    case 0: return (0);

    /* DMA interrupt only */

    case 1: {
      oz_hw_stl_nanowait (300);		/* wait 12 clock periods (12 @ 40 MHz = 300nS) */
      dstat = regs -> dstat;
      dma_interrupt (devex, istat, dstat);
      break;
    }

    /* SCSI interrupt only */

    case 2: {
      oz_hw_stl_nanowait (300);		/* wait 12 clock periods (12 @ 40 MHz = 300nS) */
      sist0 = regs -> sist0;
      oz_hw_stl_nanowait (300);		/* wait 12 clock periods (12 @ 40 MHz = 300nS) */
      sist1 = regs -> sist1;

      if ((istat == 0x2A) && (sist0 == 0xFF) && (sist1 == 0xFF)) {
        oz_knl_printk ("oz_dev_lsil875: %s istat 2A, sist0 FF, sist1 FF\n", devex -> name);
        chip_abort (devex, 0);
        return (0);
      }
      scsi_interrupt (devex, istat, sist0, sist1);
      break;
    }

    /* DMA and SCSI interrupt */

    case 3: {
      oz_hw_stl_nanowait (300);		/* wait 12 clock periods (12 @ 40 MHz = 300nS) */
      sist0 = regs -> sist0;
      oz_hw_stl_nanowait (300);		/* wait 12 clock periods (12 @ 40 MHz = 300nS) */
      sist1 = regs -> sist1;
      oz_hw_stl_nanowait (300);		/* wait 12 clock periods (12 @ 40 MHz = 300nS) */
      dstat = regs -> dstat;
      dma_interrupt (devex, istat, dstat);
      scsi_interrupt (devex, istat, sist0, sist1);
      break;
    }
  }

  /* Start chip going if dma_interrupt/scsi_interrupt want it resumed */

  if (devex -> resumedsp != 0) {
    OZ_HW_MB;
    regs -> dsp = devex -> resumedsp;
  }

  /* The book says the chip will interrupt if it has more for us to do */

  return (0);
}

/*******************************************************/
/* Interrupt-on-the-fly:  An I/O request has completed */
/*******************************************************/

static void done_interrupt (Devex *devex)

{
  Rp *rp;
  uLong scsi_id;

  for (scsi_id = 0; scsi_id < MAX_SCSI_ID; scsi_id ++) {			/* check each possible scsi device */
    while ((rp = devex -> firstrp[scsi_id]) != NULL) {				/* see if anything queued to it */
      if (!(rp -> rp0_flags & RP_FLAG_DONE)) break;				/* forget it if still being worked on */
      devex -> firstrp[scsi_id] = rp -> rp_next_va;				/* ok, unlink from pending queue */
#if ONEATATIME
      if (devex -> firstrp[scsi_id] != NULL) start_request (devex, scsi_id);	/* start next request, if any */
#endif
      postreq (rp);								/* post completion */
    }
    if (rp == NULL) devex -> lastrp[scsi_id] = NULL;
  }
}

/************************************************************************/
/*									*/
/*  Start the controller processing the top request on the queue	*/
/*  This routine assumes the controller is known to be idle for the 	*/
/*  particular scsi-id							*/
/*									*/
/************************************************************************/

#if ONEATATIME
static void start_request (Devex *devex, uLong scsi_id)

{
  Rp *rp;
  Se *se;
  uLong rp_pa1;

  se = devex -> se_va + scsi_id;
  OZ_HW_MB;						/* don't use a speculative se_queue_head that was read before rp0_flags */
  if (se -> se_queue_head != 0) {			/* make sure scsi-id's queue is empty */
    oz_knl_printk ("oz_dev_lsil875: %s[%u] queue busy\n", devex -> name, scsi_id);
    chip_abort (devex, 0);
  } else {
    rp = devex -> firstrp[scsi_id];			/* get top request on queue */
    rp_pa1 = npp_virt_to_phys (sizeof *rp, rp) | 1;	/* get pci address of request packet with low bit set */
							/* low bit set indicates it is a new request */
    se -> se_queue_head = rp_pa1;			/* link to top of queue */
    OZ_HW_MB;						/* make sure se_queue_head gets written before istat */
    devex -> regs -> istat = 0x20;			/* ... and tell it something needs to be started */
  }
}
#endif

/***************************************************************************/
/* Something went wrong with the DMA stuff (like memory parity error, etc) */
/***************************************************************************/

static void dma_interrupt (Devex *devex, uByte istat, uByte dstat)

{
  Regs *regs;
  Rp *rp;
  Se *se;
  uByte queueflg;
  uLong dcmd_dbc, *mov_va, rp_pa, scsi_id;

  regs = devex -> regs;

  /* Check for INT 0x69 instruction indicating that the SCRIPTS is asking us to restore a saved data pointer */

  if ((dstat & 0x04) && (regs -> dsps == 0x00000069)) {
    scsi_id  = regs -> scratchj2;				/* get scsi_id in question */
    se       = devex -> se_va + scsi_id;			/* point to scsi_id_table entry */
    rp_pa    = regs -> scratchi;				/* get phys addr of rp */
    rp       = (Rp *)(regs -> scratchh);			/* get virt addr of rp */
    dcmd_dbc = se -> se_saved_dbc;				/* get saved instruction */
#if 111
    oz_knl_printk ("oz_dev_lsil875*: %s[%u] int 69: rp_pa %x, rp %p\n", devex -> name, scsi_id, rp_pa, rp);
    oz_knl_printk ("oz_dev_lsil875*: %s[%u]   saved_dsp %x, saved_dbc %x\n", devex -> name, scsi_id, se -> se_saved_dsp, dcmd_dbc);
#endif
    setupdatamovs (rp);						/* restore data chmovs to original state */
    se -> se_saved_dsp &= -2;					/* clear low bit of saved pointer */
    rp -> rp_datamov_pa = se -> se_saved_dsp;			/* restore pointer */
    if ((dcmd_dbc & 0xF6000000) == 0) {				/* only restore CHMOV/MOVE instrs */
      mov_va = (uLong *)(((uByte *)rp) + rp -> rp_datamov_pa - rp_pa); /* get virt address of saved chmov instr */
#if 111
      oz_knl_printk ("oz_dev_lsil875*: %s[%u]   original: %8.8x %8.8x\n", devex -> name, scsi_id, mov_va[0], mov_va[1]);
#endif
      mov_va[1] += mov_va[0] - dcmd_dbc;			/* update the buffer address */
      mov_va[0]  = dcmd_dbc;					/* restore the buffer size */
#if 111
      oz_knl_printk ("oz_dev_lsil875*: %s[%u]   restored: %8.8x %8.8x\n", devex -> name, scsi_id, mov_va[0], mov_va[1]);
#endif
    }
    devex -> resumedsp = regs -> dsp;				/* resume scripts processor where it left off */
  }

  /* Check for INT 0x96 instruction indicating that the queue is out-of-order                */
  /* These seem to happen for a few seconds then quit for a quite a while every now and then */

#if !ONEATATIME
  if ((dstat & 0x04) && (regs -> dsps == 0x00000096)) {
    scsi_id = regs -> scratchj2;				/* get scsi_id in question */
    se      = devex -> se_va + scsi_id;				/* point to scsi_id_table entry */

#if 000
    rp_pa = se -> se_queue_head & -4;
    rp = (Rp *) oz_hw486_phys_fetchlong (rp_pa);
    queueflg = -1;
    if (OZ_HW_WRITABLE (sizeof *rp, rp, OZ_PROCMODE_KNL)) queueflg = rp -> rp_iopex -> queueflg;
    oz_knl_printk ("oz_dev_lsil875*: %t %s[%u]\n		has seq %2.2X (%2.2X), expects %2.2X", 
	oz_hw_timer_getnow (), devex -> name, scsi_id, regs -> scratcha0 ^ regs -> scratcha2, queueflg, regs -> scratcha0);
#endif

    done_interrupt (devex);					/* remove all from queue that are done */
    se -> se_queue_head = 0;					/* clear controller's queue in case no more requests to process */
    rp = devex -> firstrp[scsi_id];				/* see if there is any request to process */
    if (rp != NULL) {
#if 000
      oz_knl_printk (" (%2.2X)\n", rp -> rp_iopex -> queueflg);
#endif
      if (rp -> rp2_seqsts != se -> se0_sequence) {		/* make sure it is what the controller expects, or something is fu'd */
        oz_knl_printk ("oz_dev_lsil875: %s[%u] controller seq %u, top request %u\n", devex -> name, scsi_id, se -> se0_sequence, rp -> rp2_seqsts);
        chip_abort (devex, 1);
        return;
      }
      rp_pa = npp_virt_to_phys (sizeof *rp, rp);		/* get request packet's pci address */
      se -> se_queue_head = rp_pa | 1;				/* queue it, low bit indicates it is a new request */
    }
#if 000
    else oz_knl_printk (" (none)\n");
#endif
    devex -> resumedsp  = Ent_queue_fixed + devex -> sram_pa;	/* restart controller */
  }
#endif

  /* Everything else is fatal */

  else {
    oz_knl_printk ("oz_dev_lsil875: %s dma_interrupt istat %2.2X, dstat %2.2X\n", devex -> name, istat, dstat);
    chip_abort (devex, 1);
  }
}

/**************************************************************************/
/* Something went wrong with the SCSI stuff (like timeout or phase error) */
/**************************************************************************/

static uLong check_fifo_empty (void *regsv);
static uLong check_clf_clear (void *regsv);
static uLong check_csf_clear (void *regsv);

static void scsi_interrupt (Devex *devex, uByte istat, uByte sist0, uByte sist1)

{
  Regs *regs;
  Rp *rp;
  Se *se;
  uByte ctest5, dcmd, dfifo, scratchj1, scsi_id, sstat0, sstat1, sstat2;
  uLong dbc, fifobytecount, mov_pa, *mov_va, memorybytestransferred, rp_pa, scsibytestransferred;

  regs = devex -> regs;
  scsi_id = regs -> scratchj2;				/* this holds the scsi_id being worked on */
  if (scsi_id >= MAX_SCSI_ID) {
    oz_knl_printk ("oz_dev_lsil875: %s bad interrupt scsi-id %u\n", devex -> name, scsi_id);
    oz_knl_printk ("oz_dev_lsil875: %s istat %2.2X, sist0 %2.2X, sist1 %2.2X\n", devex -> name, istat, sist0, sist1);
    chip_abort (devex, 1);
    return;
  }
  se = devex -> se_va + scsi_id;			/* point to scsi_id_table entry for this id */

  /* Phase mismatch - this happens when the target is in a different scsi phase than what we expected      */
  /* ... this happens 'expectedly' during a data transfer when the target wants to disconnect or something */
  /* So to distinguish a fatal one from an expected one, the SCRIPTS uses SCRATCHJ1 = 1 to indicate such.  */
  /* The 'expected' ones should only happen during the CHMOV/MOVE's of an rp_datamov_pa list.              */

  if (sist0 & 0x80) {
    scratchj1 = regs -> scratchj1;			/* this holds the 'doing chmov/move's' flag */
    if (scratchj1 == SCRATCHJ1_CHMOVS) {
      dbc  = regs -> dcmd_dbc;				/* get opcode that aborted */
      dcmd = dbc >> 24;					/* opcode is in top 8 bits */
      dbc &= 0x00FFFFFF;				/* low 24 bits contains residual byte count */
      if ((dcmd & 0xF6) == 0x00) {			/* only allow CHMOV/MOVE ... WHEN DATA_IN/DATA_OUT */

        /* If something like a disk read was going, but the fifo's aren't empty, flush them to memory */

        if ((dcmd & 0x01) && !(regs -> dstat & 0x80)) {	/* check for dev-to-mem (eg, disk read) but the fifo is not empty */
          regs -> ctest3 |= 0x08;			/* if not empty, tell it to flush
          oz_hw_stl_microwait (1000, check_fifo_empty, regs); /* wait up to 1mS for it to flush */
          regs -> ctest3 &= 0xF7;			/* turn off flush operation */
          if (!(regs -> dstat & 0x80)) {		/* the fifo should be empty now */
            oz_knl_printk ("oz_dev_lsil875: %s[%u] fifo didn't flush after phase mismatch\n", devex -> name, scsi_id);
            chip_abort (devex, 1);
            return;
          }
        }

        /* Get current state of transfer to determine how much was transferred before phase mismatch happened */

        rp_pa  = regs -> scratchi;			/* get what the chip has for current request packet pci address */
        rp     = (Rp *)(regs -> scratchh);		/* get what it has for the request's rp_this_va */
        mov_pa = regs -> dsp - 8;			/* get pci address of failed CHMOV/MOVE instruction */
        mov_va = (uLong *)(((uByte *)rp) + mov_pa - rp_pa); /* get virt address of failed CHMOV/MOVE instruction */

        memorybytestransferred = (mov_va[0] & 0x00FFFFFF) - dbc; /* get how many bytes were transferred to/from memory */

        ctest5 = regs -> ctest5;			/* get dma fifo size */
        dfifo  = regs -> dfifo;				/* get dma scsi-side ring counter */
        fifobytecount = ((ctest5 << 8) + dfifo - dbc) & 0x3FF; /* assume a large fifo size */
        if (!(ctest5 & 0x20)) fifobytecount &= 0x7F;	/* maybe it is small */
        sstat0 = regs -> sstat0;			/* get various scsi status bytes */
        sstat1 = regs -> sstat1;
        sstat2 = regs -> sstat2;

#if 000
        oz_knl_printk ("oz_dev_lsil875*: %s[%u] phase mismatch sstat1 %2.2X\n", devex -> name, scsi_id, sstat1);
        oz_knl_printk ("         istat %2.2x -> %2.2x\n", istat, regs -> istat);
        oz_knl_printk ("         sist0 %2.2x\n", sist0);
        oz_knl_printk ("         sist1 %2.2x\n", sist1);
        oz_knl_printk ("          dcmd %2.2x\n", dcmd);
        oz_knl_printk ("         rp_pa %8.8x\n", rp_pa);
        oz_knl_printk ("        mov_pa %8.8x\n", mov_pa);
        oz_knl_printk ("         rp_va %8.8x\n", rp);
        oz_knl_printk ("        mov_va %8.8x\n", mov_va);
        oz_knl_printk ("     mov_va[0] %8.8x\n", mov_va[0]);
        oz_knl_printk ("           dbc %8.8x\n", dbc);
        oz_knl_printk ("  membytesxfrd %8.8x\n", memorybytestransferred);
        oz_knl_printk ("     fifobytes %4.4x\n", fifobytecount);
        oz_knl_printk ("         dfifo %2.2x\n", dfifo);
        oz_knl_printk ("        ctest5 %2.2x\n", ctest5);
        oz_knl_printk ("        sstat0 %2.2x\n", sstat0);
        oz_knl_printk ("        sstat1 %2.2x\n", sstat1);
        oz_knl_printk ("        sstat2 %2.2x\n", sstat2);
#endif

        if ((sstat1 & 0x06) == 0x00) {			/* can't recover if target wants a data phase */
							/* it 'must be' asking for data in the other direction */
          oz_knl_printk ("oz_dev_lsil875: %s[%u] target wants data phase %u when request expects phase %u\n", 
                          devex -> name, scsi_id, sstat1 & 0x07, dcmd & 0x07);
          chip_abort (devex, 1);
          return;
        }

        /* See what's in fifo to modify the bytecount transferred to/from the device                               */
        /* For device-to-memory operations, this difference should be zero as the fifo should be flushed to memory */
        /* For memory-to-device operations, device_bytecount = memory_bytecount - bytes_in_fifo                    */

        if (!(dcmd & 0x01)) {				/* check for CHMOV/MOVE ... WITH DATA_OUT (mem-to-dev) */
          if (sstat0 & 0x20) fifobytecount ++;		/* maybe there is a byte in SODL least significant */
          if (sstat2 & 0x20) fifobytecount ++;		/* maybe there is a byte in SODL most significant */
          if ((regs -> sxfer & 0x1F) != 0) {		/* if synchronous transfer mode ... */
            if (sstat0 & 0x40) fifobytecount ++;	/* ... maybe there is a byte in SODR least significant */
            if (sstat2 & 0x40) fifobytecount ++;	/* ... maybe there is a byte in SODR most significant */
          }
          scsibytestransferred = memorybytestransferred - fifobytecount;
        } else {					/* else it's a CHMOV/MOVE ... WITH DATA_IN (dev-to-mem) */
          if ((regs -> sxfer & 0x1F) != 0) {		/* if synchronous transfer mode ... */
            fifobytecount += sstat1 >> 4;		/* ... get fifo bytes from sstat1<4:7> (scsi fifo count<0:3>) */
            fifobytecount += sstat2 & 0x10;		/* ... and from sstat1<4> (scsi fifo count<4>) */
          } else {					/* otherwise, asynchronous mode ... */
            if (sstat0 & 0x80) fifobytecount ++;	/* ... maybe there is a byte in SIDL least significant */
            if (sstat2 & 0x80) fifobytecount ++;	/* ... maybe there is a byte in SIDL most significant */
          }
          fifobytecount += regs -> scntl2 & 0x01;	/* maybe there's a wide residue byte */
          if (fifobytecount != 0) {			/* the chip should have flushed it all to memory before halting */
            oz_knl_printk ("oz_dev_lsil875: %s[%u] read left %u bytes in fifo\n", devex -> name, scsi_id, fifobytecount);
            chip_abort (devex, 1);
            return;
          }
          scsibytestransferred = memorybytestransferred;
        }

        /* Update the failed CHMOV/MOVE to do what's remaining, ie, skip over what was transferred to/from the device. */

        if ((mov_va[0] & 0x00FFFFFF) <= scsibytestransferred) {
          oz_knl_printk ("oz_dev_lsil875: %s[%u] mov_va count 0x%x, scsibytestransferred 0x%x\n", 
                         devex -> name, scsi_id, mov_va[0], scsibytestransferred);
          chip_abort (devex, 1);
          return;
        }
        mov_va[0] -= scsibytestransferred;		/* decrement remaining data bytecount */
        mov_va[1] += scsibytestransferred;		/* increment remaining data address */

        /* Adjust the rp_datamov_pa pointer to point to the failed CHMOV/MOVE */

        rp -> rp_datamov_pa = mov_pa;

        /* Clear any stuff out of dma and scsi fifos.  There should only be something there for memory-to-device operations, */
        /* ie, the device cut the transfer off while there was stuff in the fifos waiting to go out to the device.           */

        regs -> ctest3 |= 0x04;				/* tell chip to reset dma fifo */
        oz_hw_stl_microwait (1000, check_clf_clear, regs);
        regs -> stest3 |= 0x02;				/* tell chip to reset scsi fifo */
        oz_hw_stl_microwait (1000, check_csf_clear, regs);
        if ((regs -> ctest3 & 0x04) || (regs -> stest3 & 0x02)) {
          oz_knl_printk ("oz_dev_lsil875: %s[%u] fifo's failed to reset after phase mismatch\n", devex -> name, scsi_id);
          chip_abort (devex, 1);
          return;
        }

        /* Resume the chip at the 'transfer_data_mismatch' entrypoint */

        devex -> resumedsp = Ent_transfer_data_mismatch + devex -> sram_pa;
        return;
      }
    }
  }

  /* Check for an 'unexpected disconnect'.  We recover from these if the controller was just polling the target's phase. */

  if ((sist0 & 0x04) && (regs -> scratchj0 == STATE_CHECKING_TARGET)) {
    oz_knl_printk ("oz_dev_lsil875: %s[%u] unexpected disconnect, recovering\n", devex -> name, scsi_id);
    regs -> scratchj0 = ~ STATE_CHECKING_TARGET;
    devex -> resumedsp = Ent_disconnecting + devex -> sram_pa;
    return;
  }

  /* Check for Selection time-out - this happens when a target does not        */
  /* respond to a selection (presumably it is not connected or turned on)      */
  /* Here, we abort all requests for the drive and jump to 'selection_timeout' */

  if (sist1 & 0x04) {
    oz_knl_printk ("oz_dev_lsil875: %s[%u] selection timeout\n", devex -> name, scsi_id);
#if 000 /* we get interrupt in the wrong place but it doesn't seem to hurt - see scripts for details */
    scratchj1 = regs -> scratchj1;			/* this holds the 'doing select' flag */
    if (scratchj1 != SCRATCHJ1_SELECT) {
      oz_knl_printk ("oz_dev_lsil875: --> scratchj1 = %2.2x, dsp = %8.8x\n", scratchj1, regs -> dsp);
    }
#endif
    regs -> scratchj1 = 0;				/* if so, clear the SCRATCHJ1 register */
    memcpy (se, ((Se *)SCRIPT) + scsi_id, sizeof *se);	/* clear out the queue */
    devex -> resumedsp = Ent_select_timedout + devex -> sram_pa; /* tell chip to jump to 'select_timedout' */
    abort_requests (devex, scsi_id, RP_ABORT_SELECT);	/* abort all requests for the failing device */
    return;
  }

  /* Check for Handshake time-out - this happens when a target does */
  /* not handshake a transfer (it hung or something like that)      */

#if 00
  if (sist1 & 0x01) {
    oz_knl_printk ("oz_dev_lsil875: %s[%u] handshake timeout\n", devex -> name, scsi_id);
    if (scsi_id < MAX_SCSI_ID) {
      memset (se, 0, sizeof *se);			/* if so, clear out the queue head */
      devex -> resumedsp = Ent_handshake_timeout + devex -> sram_pa; /* tell chip to jump to 'handshake_to' */
      abort_requests (devex, scsi_id, RP_ABORT_FATLERR); /* abort all requests for the device */
      return;
    }
  }
#endif

  /* Don't know what it wants so reset it all */

  oz_knl_printk ("oz_dev_lsil875: %s scsi_interrupt istat %2.2x, sist0 %2.2x, sist1 %2.2x\n", devex -> name, istat, sist0, sist1);
  chip_abort (devex, 1);
}

static uLong check_fifo_empty (void *regsv)

{
  return (((Regs *)regsv) -> dstat & 0x80);
}

static uLong check_clf_clear (void *regsv)

{
  return (!(((Regs *)regsv) -> ctest3 & 0x04));
}

static uLong check_csf_clear (void *regsv)

{
  return (!(((Regs *)regsv) -> stest3 & 0x02));
}

/************************************************************************/
/*									*/
/*  This routine sets up the data CHMOV/MOVE/JUMP instructions		*/
/*									*/
/************************************************************************/

static void setupdatamovs (Rp *rp)

{
  Iopex *iopex;
  OZ_IO_scsi_doiopp *doiopp;
  OZ_Mempage i, j;
  uLong bytesinpage, databyteoffs, dcmd, pagesize, totalbytesleft;

  iopex    = rp -> rp_iopex;
  doiopp   = &(iopex -> doiopp);
  pagesize = 1 << OZ_HW_L2PAGESIZE;

  rp -> rp_datamov_pa = npp_virt_to_phys ((iopex -> datanumpages + 1) * sizeof rp -> rp_datamovs[0], rp -> rp_datamovs); /* ok, save pci address of first CHMOV/MOVE instruction */

  totalbytesleft = doiopp -> datasize;				/* total amount of data to be done */
  databyteoffs   = doiopp -> databyteoffs;			/* offset in first physical page */
  dcmd = DCMD_CHMOV_WITH_DATA_IN;				/* assume a 'read' type operation */
  if (doiopp -> optflags & OZ_IO_SCSI_OPTFLAG_WRITE) dcmd = DCMD_CHMOV_WITH_DATA_OUT; /* maybe not */
  j = 0;
  for (i = 0; i < iopex -> datanumpages; i ++) {		/* set up CHMOV's to transfer the data */
    bytesinpage = pagesize - databyteoffs;			/* see how many bytes left in the page */
    if (bytesinpage > totalbytesleft) bytesinpage = totalbytesleft; /* not more than we have left to transfer */
    if ((j > 0) && (doiopp -> dataphypages[i] == doiopp -> dataphypages[i-1] + 1)) { /* see if physically contiguous with last page */
      rp -> rp_datamovs[j-1].dcmd_dbc += bytesinpage;		/* if so, just add onto last transfer's size */
    } else {							/* not phys contig, start a new one */
      rp -> rp_datamovs[j].dcmd_dbc = dcmd | bytesinpage;	/* that's the size we want to do */
      rp -> rp_datamovs[j++].dnad   = (doiopp -> dataphypages[i] << OZ_HW_L2PAGESIZE) + databyteoffs; /* store the starting pci address */
    }
    totalbytesleft -= bytesinpage;				/* this much less left to do */
    databyteoffs    = 0;					/* start right at beginning of next page */
  }
  if ((j > 0) && (doiopp -> optflags & OZ_IO_SCSI_OPTFLAG_WRITE)) { /* change last opcode of a write to MOVE ... */
    rp -> rp_datamovs[j-1].dcmd_dbc ^= DCMD_MOVE_WITH_DATA_OUT ^ DCMD_CHMOV_WITH_DATA_OUT; /* ... to flush odd byte */
  }
  rp -> rp_datamovs[j].dcmd_dbc = DCMD_JUMP;			/* terminate with a 'JUMP transfer_data_done' */
  rp -> rp_datamovs[j].dnad = Ent_transfer_data_done + iopex -> devex -> sram_pa;
}

/************************************************************************/
/*									*/
/*  Some bad error happened with the chip - so abort everything and 	*/
/*  reset								*/
/*									*/
/*    Input:								*/
/*									*/
/*	alreadyhalted = 0 : chip is not known to be halted		*/
/*	                1 : chip is known to be halted			*/
/*									*/
/************************************************************************/

static void chip_abort (Devex *devex, int alreadyhalted)

{
  int i;
  Regs *regs;
  Rp *rp;

  devex -> devstate = DEVSTATE_OFFLINE;			/* prevent further requests from queuing */
  regs = devex -> regs;

  oz_knl_printk ("oz_dev_lsil875: %s chip_abort ISTAT %2.2X\n", devex -> name, regs -> istat);

  /* If chip isn't already halted, halt it */

  if (!alreadyhalted) {
    regs -> istat = 0x80;
    if (!oz_hw_stl_microwait (1000, NULL /*chip_abort_haltcheck*/, regs)) {
      oz_knl_printk ("oz_dev_lsil875: %s: - failed to halt, ISTAT %2.2x\n", devex -> name, regs -> istat);
    }
  }

  /* Now print out all the registers and I/O requests */

  chip_dump (devex);

  /* Reset the chip - it will get turned back on next time someone tries to use it */

  oz_knl_printk ("oz_dev_lsil875: %s turned offline\n", devex -> name);
  chip_reset (devex);

  /* Abort all pending I/O requests */

  for (i = 0; i < MAX_SCSI_ID; i ++) {			/* loop through all possible scsi-id's */
    while ((rp = devex -> firstrp[i]) != NULL) {	/* repeat as long as there are packets queued */
      devex -> firstrp[i] = rp -> rp_next_va;		/* unlink the packet */
      rp -> rp1_abort     = RP_ABORT_CTRLERR;		/* mark it aborted with fatal controller error */
      postreq (rp);					/* post request completion */
    }
    devex -> lastrp[i] = NULL;				/* queue is empty */
  }
}

static uLong chip_abort_haltcheck (void *regsv)

{
  return (((Regs *)regsv) -> istat & 0x03);
}

/************************************************************************/
/*									*/
/*  Dump out chip registers (chip is assumed to be halted)		*/
/*									*/
/************************************************************************/

static void chip_dump (Devex *devex)

{
  int i, j;
  Iopex *iopex;
  Regs *regs;
  Rp *rp;

  regs = devex -> regs;

  /* Print out all the registers */

  oz_knl_dumpmem (sizeof *regs, regs);

  /* Dump pending I/O requests */

  for (i = 0; i < MAX_SCSI_ID; i ++) {					/* loop through all possible scsi-id's */
    if ((devex -> firstrp[i] == NULL) && (devex -> se_va[i].se_queue_head == 0)) continue;
    oz_knl_printk ("oz_dev_lsil875: %s[%u] queue:\n", devex -> name, i);
    oz_knl_dumpmem (sizeof *(devex -> se_va), devex -> se_va + i);	/* dump scsi_id_table entry */
    j = -3;								/* only dump out the first three requests per device */
    for (rp = devex -> firstrp[i]; rp != NULL; rp = rp -> rp_next_va) {
      if (++ j <= 0) {
        oz_knl_printk ("oz_dev_lsil875: - va %p, pa 0x%X\n", rp, npp_virt_to_phys (sizeof *rp, rp));
        iopex = rp -> rp_iopex;
        oz_knl_dumpmem ((OZ_Pointer)(rp -> rp_datamovs + iopex -> datanumpages + 1) - (OZ_Pointer)rp, rp);
        oz_knl_dumpmem (sizeof *iopex, iopex);
        oz_knl_dumpmem (iopex -> datanumpages * sizeof iopex -> doiopp.dataphypages[0], iopex -> doiopp.dataphypages);
      }
    }
    if (j > 0)  oz_knl_printk ("oz_dev_lsil875: - plus %d other requests\n", j);
  }
}

/************************************************************************/
/*									*/
/*  Abort all requests queued to a scsi_id				*/
/*									*/
/*    Input:								*/
/*									*/
/*	devex = controller device struct				*/
/*	scsi_id = scsi-id of target device being aborted		*/
/*	rp_abort_code = reason for abort				*/
/*									*/
/*    Output:								*/
/*									*/
/*	software queue (devex->firstrp[scsi_id]) cleared out		*/
/*									*/
/************************************************************************/

static void abort_requests (Devex *devex, uLong scsi_id, uByte rp_abort_code)

{
  Rp *rp;

  while ((rp = devex -> firstrp[scsi_id]) != NULL) {	/* repeat as long as there are packets queued */
    devex -> firstrp[scsi_id] = rp -> rp_next_va;	/* unlink the packet */
    rp -> rp1_abort = rp_abort_code;
    postreq (rp);					/* post completion */
  }
  devex -> lastrp[scsi_id] = NULL;			/* queue is empty */
}

/************************************************************************/
/*									*/
/* Post request's completion (we're at high ipl)			*/
/*									*/
/************************************************************************/

static void postreq (Rp *rp)

{
  OZ_Ioop *ioop;
  uLong sts;

  rp -> rp_next_va = rp;						/* so crash routine will see it is done */
  ioop = rp -> rp_iopex -> ioop;					/* point to associated ioop (NULL for crash routine) */
  if (!(rp -> rp0_flags & RP_FLAG_GOTSTATUS)) rp -> rp2_seqsts = 0xFF;	/* if we didn't get status back, return 0xFF */
  if (ioop != NULL) {							/* see if crash routine */
    sts = calc_status (rp);						/* if not, calc completion status */
    oz_knl_iodonehi (ioop, sts, NULL, finishup, rp);			/* post for completion */
  }
}

/************************************************************************/
/*									*/
/* Now we're in requestor's memory space context still at softint level */
/*									*/
/************************************************************************/

static void finishup (void *rpv, int finok, uLong *status_r)

{
  Devex *devex;
  Iopex *iopex;
  Rp *rp;
  uLong datarlen, sts;

  rp = rpv;

  if (finok) {
    iopex = rp -> rp_iopex;
    devex = iopex -> devex;

    /* Maybe caller wants scsi status byte */

    if (iopex -> doiopp.status != NULL) {
      sts = oz_knl_section_uput (iopex -> procmode, sizeof *(iopex -> doiopp.status), &(rp -> rp2_seqsts), iopex -> doiopp.status);
      if (sts != OZ_SUCCESS) {
        oz_knl_printk ("oz_dev_lsil875: error %u writing scsi status byte to %p\n", sts, iopex -> doiopp.status);
        if (*status_r == OZ_SUCCESS) *status_r = sts;
      }
    }

    /* Maybe caller wants actual data transfer length */

    if (iopex -> doiopp.datarlen != NULL) {
      datarlen = calc_datarlen (rp);
      sts = oz_knl_section_uput (iopex -> procmode, sizeof *(iopex -> doiopp.datarlen), &datarlen, iopex -> doiopp.datarlen);
      if (sts != OZ_SUCCESS) {
        oz_knl_printk ("oz_dev_lsil875: error %u writing data length to %p\n", sts, iopex -> doiopp.datarlen);
        if (*status_r == OZ_SUCCESS) *status_r = sts;
      }
    }
  }

  free_rp (rp);
}

/************************************************************************/
/*									*/
/*  Start a dead scsi controller from scratch				*/
/*									*/
/*    Input:								*/
/*									*/
/*	devex = scsi controller to be enabled				*/
/*	smplevel = dv							*/
/*									*/
/*    Output:								*/
/*									*/
/*	controller initialized and running				*/
/*	devex -> devstate = DEVSTATE_STARTING, eventually _ONLINE	*/
/*									*/
/************************************************************************/

static void chip_enable (Devex *devex)

{
  int i;
  OZ_Datebin when;
  Regs *regs;

  oz_knl_printk ("oz_dev_lsil875: starting %s\n", devex -> name);

  /* Software reset the chip and hardware reset the SCSI bus */

  chip_reset (devex);

  /* Reset the queuing sequence numbers */

  memset (devex -> queueseq, 0, sizeof devex -> queueseq);

  /* Turn on hardware */

  regs = devex -> regs;

						/* miscellaneous enables */
  regs -> scntl0  = 0xCA;
  regs -> scntl3  = ((Se *)SCRIPT) -> se3_scntl3;
  regs -> scid    = 0x40 | devex -> scsi_id;
  regs -> sxfer   = ((Se *)SCRIPT) -> se1_sxfer;
  regs -> ctest3  = 0x01;
  regs -> ctest5  = 0x24;
  regs -> dmode   = 0x8E;
  regs -> dcntl   = 0xA1;
  regs -> stime0  = 0xDD;
  regs -> respid0 = (1 << devex -> scsi_id);
  regs -> respid1 = (1 << devex -> scsi_id) >> 8;

						/* enable the 'clock doubler' */
  regs -> stest1  = 0x08;
  oz_hw_stl_microwait (20, NULL, NULL);
  regs -> stest3  = 0xB0;
  regs -> scntl3  = ((Se *)SCRIPT) -> se3_scntl3;
  regs -> stest1  = 0x0C;
  regs -> stest3  = 0x90;

  /* Download SCRIPTS to the chip's internal ram */

  if (sizeof SCRIPT > 4096) oz_crash ("oz_dev_lsil875: sizeof SCRIPT is %u, max 4096", sizeof SCRIPT);
  for (i = 0; i < sizeof SCRIPT / sizeof SCRIPT[0]; i ++) {
    ((uLong *)(devex -> sram_va))[i] = SCRIPT[i];
  }
  while (i < 1024) ((uLong *)(devex -> sram_va))[i++] = 0xFEEBDAED;
  for (i = 0; i < sizeof LABELPATCHES / sizeof LABELPATCHES[0]; i ++) {
    ((uLong *)(devex -> sram_va))[LABELPATCHES[i]] += devex -> sram_pa;
  }

  /* Enable interrupts and start processor */

  regs -> dien  = 0x7F;
  regs -> sien0 = 0x8F;
  regs -> sien1 = 0x05;

  regs -> dsp = Ent_startup + devex -> sram_pa;	/* tell it to start at entrypoint 'startup' */
						/* I PUKE so discreetly ... */

  /* Change controller state so it can queue requests now                                  */
  /* If we're in crash dump mode, use a software timing loop to wait for the startup delay */
  /* Otherwise, use a normal timer for the startup delay                                   */
  /* Without this startup delay, an scsi boot disk will not be ready for selection         */

  oz_knl_printk ("oz_dev_lsil875: %s started\n", devex -> name);
  if (!crash_inprog) {
    devex -> devstate = DEVSTATE_STARTING;				// allow requests to queue to startingq while waiting
    when  = oz_hw_timer_getnow ();					// see what time it is right now
    when += OZ_TIMER_RESOLUTION * STARTUPDELAY;				// calc when we want to start processing requests
    oz_knl_timer_insert (devex -> startuptimer, when, chip_started, devex); // start the timer
  } else {
    oz_hw_stl_microwait (1000000 * STARTUPDELAY, NULL, NULL);		// just loop here for the startup delay
    oz_knl_printk ("oz_dev_lsil875: %s online\n", devex -> name);	// output a message saying we're finally online
    devex -> devstate = DEVSTATE_ONLINE;				// set state to indicate online mode
  }
}

/************************************************************************/
/*									*/
/*  This routine is called at softint level when the startup delay 	*/
/*  timer has expired.  It transitions the state to ONLINE, and queues 	*/
/*  and requests in the startingq to the controller for processing.	*/
/*									*/
/************************************************************************/

static void chip_started (void *devexv, OZ_Timer *timer)

{
  Devex *devex;
  Rp *rp, *xrp;
  uLong dv;

  devex = devexv;
  dv = oz_hw_smplock_wait (devex -> smplock);				// lock the state and the queues
  if (devex -> devstate != DEVSTATE_STARTING) {				// maybe the chip aborted since we started it up
    rp = devex -> startingqh;						// if so, set up to abort all waiting requests
    devex -> startingqh = NULL;
  } else {
    oz_knl_printk ("oz_dev_lsil875: %s online\n", devex -> name);	// output a message saying we're finally online
    devex -> devstate = DEVSTATE_ONLINE;				// set state to indicate online mode
    while ((rp = devex -> startingqh) != NULL) {			// see if there are any requests waiting
      devex -> startingqh = rp -> rp_next_va;				// ok, dequeue top request
      rp -> rp_next_va = NULL;						// clear link as expected by queue_request routine
      queue_request (rp);						// queue it to controller for processing
    }									// repeat for any more waiting requests
  }
  devex -> startingqt = &(devex -> startingqh);				// either way, reset queue tail pointer
  oz_hw_smplock_clr (devex -> smplock, dv);				// release state and the queues

  while (rp != NULL) {
    xrp = rp -> rp_next_va;
    oz_knl_iodone (rp -> rp_iopex -> ioop, OZ_DEVOFFLINE, NULL, NULL, NULL);
    free_rp (rp);
    rp  = xrp;
  }
}

/************************************************************************/
/*									*/
/*  Reset the chip and the scsi bus					*/
/*									*/
/************************************************************************/

static void chip_reset (Devex *devex)

{
  Regs *regs;

  devex -> devstate = DEVSTATE_OFFLINE;		/* abort any requests that try to queue */
  regs = devex -> regs;
  regs -> istat  = 0x40;			/* turn on software reset bit */
  regs -> scntl1 = 0x08;			/* turn on SCSI bus reset bit */
						/* (this may be ignored when software reset is on) */
  oz_hw_stl_microwait (2, NULL, NULL);		/* wait 50 PCI cycles (assume worst case 25MHz bus = 2uS) */
  regs -> istat  = 0;				/* turn off software reset bit */
  oz_hw_stl_microwait (2, NULL, NULL);		/* wait 50 PCI cycles (assume worst case 25MHz bus = 2uS) */
  regs -> scntl1 = 0x08;			/* turn on SCSI bus reset bit */
  oz_hw_stl_microwait (30, NULL, NULL);		/* wait 30uS (SCSI spec says minimum 25uS) */
  regs -> scntl1 = 0;				/* turn off SCSI bus reset bit */

  /* Now do another software reset to get rid of the 'SCSI reset' interrupt */

  oz_hw_stl_microwait (2, NULL, NULL);		/* wait 50 PCI cycles (assume worst case 25MHz bus = 2uS) */
  regs -> istat  = 0x40;			/* turn on software reset bit */
  oz_hw_stl_microwait (2, NULL, NULL);		/* wait 50 PCI cycles (assume worst case 25MHz bus = 2uS) */
  regs -> istat  = 0;				/* turn off software reset bit */
  oz_hw_stl_microwait (2, NULL, NULL);		/* wait 50 PCI cycles (assume worst case 25MHz bus = 2uS) */

  /* Re-program in the controller's scsi-id in case this is the loader doing a shutdown, we want the kernel to see the scsi-id */

  regs -> scid = devex -> scsi_id;
}

/************************************************************************/
/*									*/
/*  Convert npp virtual address to physical address - also check for 	*/
/*  longword alignment							*/
/*									*/
/*    Input:								*/
/*									*/
/*	size = size of buffer area					*/
/*	addr = address of buffer area					*/
/*									*/
/*    Output:								*/
/*									*/
/*	npp_virt_to_phys = physical address of 'addr'			*/
/*									*/
/************************************************************************/

static uLong npp_virt_to_phys (uLong size, void *addr)

{
  OZ_Mempage phypage;
  uLong length, offset;

  length = oz_knl_misc_sva2pa (addr, &phypage, &offset);
  if (length < size) {
    oz_crash ("oz_dev_lsil875 npp_virt_to_phys: buf %p size %u not physically contiguous", addr, size);
  }

  if (offset & 3) oz_crash ("oz_dev_lsil875 npp_virt_to_phys: buf %p not longword aligned", addr);

  return ((phypage << OZ_HW_L2PAGESIZE) + offset);
}

/************************************************************************/
/*									*/
/*  Calculate number of physical pages required for a data transfer	*/
/*									*/
/************************************************************************/

static OZ_Mempage calc_datanumpages (OZ_IO_scsi_doiopp *doiopp)

{
  if (doiopp -> datasize == 0) return (0);
  return (((doiopp -> databyteoffs + doiopp -> datasize - 1) >> OZ_HW_L2PAGESIZE) + 1);
}

/************************************************************************/
/*									*/
/*  Calculate returned data length and completion status		*/
/*									*/
/************************************************************************/

static uLong calc_datarlen (Rp *rp)

{
  Devex *devex;
  int i;
  Iopex *iopex;
  uLong datarlen, datamov_pa, dcmd_dbc;

  iopex = rp -> rp_iopex;
  devex = iopex -> devex;

  datarlen   = iopex -> doiopp.datasize;			/* assume the whole thing was done */
  datamov_pa = rp -> rp_datamov_pa;				/* point to possible residual CHMOV/MOVE's */
  if ((datamov_pa ^ devex -> sram_pa) & ~4095) {		/* if it still points to CHMOV/MOVES's, count residuals */
    i = (datamov_pa - npp_virt_to_phys (1, rp -> rp_datamovs)) / (sizeof rp -> rp_datamovs[0]);
    while ((dcmd_dbc = rp -> rp_datamovs[i++].dcmd_dbc) != DCMD_JUMP) datarlen -= dcmd_dbc & 0x00FFFFFF; /* subtract residuals */
  }
  return (datarlen);
}

static uLong calc_status (Rp *rp)

{
  uLong status;

  status = OZ_SUCCESS;
  if (!(rp -> rp0_flags & RP_FLAG_GOTSTATUS)) status = abortstatus[rp->rp1_abort];
  return (status);
}
