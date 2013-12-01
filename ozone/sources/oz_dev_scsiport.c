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
/*  This routine interfaces to WindoesNT SCSI HBA drivers		*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_dev_pci.h"
#include "oz_dev_scsi.h"
#include "oz_io_scsi.h"
#include "oz_knl_devio.h"
#include "oz_knl_hw.h"
#include "oz_knl_image.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_misc.h"
#include "oz_knl_phymem.h"
#include "oz_knl_status.h"
#include "oz_sys_xprintf.h"

typedef uByte  BOOLEAN;
typedef  Long  LONG;
typedef uByte *PBOOLEAN;
typedef  Byte *PCHAR;
typedef uByte *PUCHAR;
typedef uLong *PULONG;
typedef uWord *PUSHORT;
typedef  void *PVOID;
typedef uByte  UCHAR;
typedef uLong  ULONG;
typedef uWord  USHORT;
typedef  void  VOID;

#define SCSI_PHYSICAL_ADDRESS uLong

#define FAKE_IO_BASE_VA 0xFFFF0000

#define DEVEX(__HwDeviceExtension) (((Devex **)__HwDeviceExtension)[-1])

/**********************/
/* PCI specific types */
/**********************/

/* Interface types */

typedef enum _INTERFACE_TYPE {
    InterfaceTypeUndefined = -1,
    Internal,
    Isa,
    Eisa,
    MicroChannel,
    TurboChannel,
    PCIBus,
    VMEBus,
    NuBus,
    PCMCIABus,
    CBus,
    MPIBus,
    MPSABus,
    ProcessorInternal,
    InternalPowerBus,
    PNPISABus,
    PNPBus,
    MaximumInterfaceType
} INTERFACE_TYPE, *PINTERFACE_TYPE;

typedef enum _BUS_DATA_TYPE {
    ConfigurationSpaceUndefined = -1,
    Cmos,
    EisaConfiguration,
    Pos,
    CbusConfiguration,
    PCIConfiguration,
    VMEConfiguration,
    NuBusConfiguration,
    PCMCIAConfiguration,
    MPIConfiguration,
    MPSAConfiguration,
    PNPISAConfiguration,
    SgiInternalConfiguration,
    MaximumBusDataType
} BUS_DATA_TYPE, *PBUS_DATA_TYPE;

/* Interrupt modes */

typedef enum _KINTERRUPT_MODE {
    LevelSensitive,
    Latched
} KINTERRUPT_MODE;

/* Slot number */

typedef struct _PCI_SLOT_NUMBER {
  union {
    struct {
      ULONG  DeviceNumber:5;
      ULONG  FunctionNumber:3;
      ULONG  Reserved:24;
    } bits;
    ULONG  AsULONG;
  } u;
} PCI_SLOT_NUMBER, *PPCI_SLOT_NUMBER;

/* Config space data */

#define PCI_INVALID_VENDOR_ID 0xFFFF

#define PCI_TYPE0_ADDRESSES 6
#define PCI_TYPE1_ADDRESSES 2
#define PCI_TYPE2_ADDRESSES 5

typedef struct _PCI_COMMON_CONFIG {
  USHORT  VendorID;
  USHORT  DeviceID;
  USHORT  Command;
  USHORT  Status;
  UCHAR  RevisionID;
  UCHAR  ProgIf;
  UCHAR  SubClass;
  UCHAR  BaseClass;
  UCHAR  CacheLineSize;
  UCHAR  LatencyTimer;
  UCHAR  HeaderType;
  UCHAR  BIST;
 
  union {
     struct _PCI_HEADER_TYPE_0 {
       ULONG  BaseAddresses[PCI_TYPE0_ADDRESSES];
       ULONG  Reserved1[2];
       ULONG  ROMBaseAddress;
       ULONG  Reserved2[2];
       UCHAR  InterruptLine;
       UCHAR  InterruptPin;
       UCHAR  MinimumGrant;
       UCHAR  MaximumLatency;
     } type0;
   } u;
   UCHAR  DeviceSpecific[192];
} PCI_COMMON_CONFIG, *PPCI_COMMON_CONFIG;


/***********************/
/* SCSI Specific Types */
/***********************/

#define SP_UNINITIALIZED_VALUE ((ULONG) ~0)
#define SCSI_MAXIMUM_TARGETS 8
#define SCSI_MAXIMUM_LOGICAL_UNITS 8

typedef struct _PORT_CONFIGURATION_INFORMATION PORT_CONFIGURATION_INFORMATION;
typedef struct _PORT_CONFIGURATION_INFORMATION *PPORT_CONFIGURATION_INFORMATION;
typedef struct _SCSI_REQUEST_BLOCK SCSI_REQUEST_BLOCK;
typedef struct _SCSI_REQUEST_BLOCK *PSCSI_REQUEST_BLOCK;

typedef BOOLEAN (*PHW_INITIALIZE)    (PVOID HwDeviceExtension);
typedef BOOLEAN (*PHW_STARTIO)       (PVOID HwDeviceExtension, PSCSI_REQUEST_BLOCK Srb);
typedef BOOLEAN (*PHW_INTERRUPT)     (PVOID HwDeviceExtension);
typedef ULONG   (*PHW_FIND_ADAPTER)  (PVOID HwDeviceExtension, PVOID Context, PVOID BusInformation, PCHAR ArgumentString, PPORT_CONFIGURATION_INFORMATION ConfigInfo, PBOOLEAN Again);
typedef BOOLEAN (*PHW_RESET_BUS)     (PVOID HwDeviceExtension, ULONG PathId);
typedef VOID    (*PHW_DMA_STARTED)   ();
typedef VOID    (*PHW_ADAPTER_STATE) ();

typedef VOID    (*PHW_TIMER)         (PVOID DeviceExtension);

typedef struct _ACCESS_RANGE {
    SCSI_PHYSICAL_ADDRESS RangeStart;
    ULONG RangeLength;
    BOOLEAN RangeInMemory;
} ACCESS_RANGE, *PACCESS_RANGE;

/* DMA Width and Speed */

typedef enum _DMA_WIDTH {
    Width8Bits,
    Width16Bits,
    Width32Bits,
    MaximumDmaWidth
} DMA_WIDTH, *PDMA_WIDTH;

typedef enum _DMA_SPEED {
    Compatible,
    TypeA,
    TypeB,
    TypeC,
    TypeF,
    MaximumDmaSpeed
} DMA_SPEED, *PDMA_SPEED;

/* This is filled in by the HBA driver's 'DriverEntry' routine */

typedef struct _HW_INITIALIZATION_DATA {
    ULONG HwInitializationDataSize;
    INTERFACE_TYPE AdapterInterfaceType;
    PHW_INITIALIZE HwInitialize;
    PHW_STARTIO HwStartIo;
    PHW_INTERRUPT HwInterrupt;
    PHW_FIND_ADAPTER HwFindAdapter;
    PHW_RESET_BUS HwResetBus;
    PHW_DMA_STARTED HwDmaStarted;
    PHW_ADAPTER_STATE HwAdapterState;
    ULONG DeviceExtensionSize;
    ULONG SpecificLuExtensionSize;
    ULONG SrbExtensionSize;
    ULONG NumberOfAccessRanges;
    PVOID Reserved;
    BOOLEAN MapBuffers;
    BOOLEAN NeedPhysicalAddresses;
    BOOLEAN TaggedQueuing;
    BOOLEAN AutoRequestSense;
    BOOLEAN MultipleRequestPerLu;
    BOOLEAN ReceiveEvent;
    BOOLEAN RealModeInitialized;
    BOOLEAN BufferAccessScsiPortControlled;
    UCHAR   MaximumNumberOfTargets;
    UCHAR   ReservedUchars[2];
    ULONG SlotNumber;
    ULONG BusInterruptLevel2;
    ULONG BusInterruptVector2;
    KINTERRUPT_MODE InterruptMode2;
    ULONG DmaChannel2;
    ULONG DmaPort2;
    DMA_WIDTH DmaWidth2;
    DMA_SPEED DmaSpeed2;
} HW_INITIALIZATION_DATA, *PHW_INITIALIZATION_DATA;

struct _PORT_CONFIGURATION_INFORMATION {
    ULONG Length;
    ULONG SystemIoBusNumber;
    INTERFACE_TYPE AdapterInterfaceType;
    ULONG BusInterruptLevel;
    ULONG BusInterruptVector;
    KINTERRUPT_MODE InterruptMode;
    ULONG MaximumTransferLength;
    ULONG NumberOfPhysicalBreaks;
    ULONG DmaChannel;
    ULONG DmaPort;
    DMA_WIDTH DmaWidth;
    DMA_SPEED DmaSpeed;
    ULONG AlignmentMask;
    ULONG NumberOfAccessRanges;
    ACCESS_RANGE *AccessRanges;
    PVOID Reserved;
    UCHAR NumberOfBuses;
    UCHAR InitiatorBusId[8];
    BOOLEAN ScatterGather;
    BOOLEAN Master;
    BOOLEAN CachesData;
    BOOLEAN AdapterScansDown;
    BOOLEAN AtdiskPrimaryClaimed;
    BOOLEAN AtdiskSecondaryClaimed;
    BOOLEAN Dma32BitAddresses;
    BOOLEAN DemandMode;
    BOOLEAN MapBuffers;
    BOOLEAN NeedPhysicalAddresses;
    BOOLEAN TaggedQueuing;
    BOOLEAN AutoRequestSense;
    BOOLEAN MultipleRequestPerLu;
    BOOLEAN ReceiveEvent;
    BOOLEAN RealModeInitialized;
    BOOLEAN BufferAccessScsiPortControlled;
    UCHAR   MaximumNumberOfTargets;
    UCHAR   ReservedUchars[2];
    ULONG SlotNumber;
    ULONG BusInterruptLevel2;
    ULONG BusInterruptVector2;
    KINTERRUPT_MODE InterruptMode2;
    ULONG DmaChannel2;
    ULONG DmaPort2;
    DMA_WIDTH DmaWidth2;
    DMA_SPEED DmaSpeed2;

    ULONG DeviceExtensionSize;
    ULONG SpecificLuExtensionSize;
    ULONG SrbExtensionSize;

    //
    // Used to determine whether the system and/or the miniport support 
    // 64-bit physical addresses.  See SCSI_DMA64_* flags below.
    //

    UCHAR  Dma64BitAddresses;        /* New */

    //
    // Indicates that the miniport can accept a SRB_FUNCTION_RESET_DEVICE
    // to clear all requests to a particular LUN.
    //

    BOOLEAN ResetTargetSupported;       /* New */

    //
    // Indicates that the miniport can support more than 8 logical units per
    // target (maximum LUN number is one less than this field).
    //

    UCHAR MaximumNumberOfLogicalUnits;  /* New */

    //
    // Supports WMI?
    //

    BOOLEAN WmiDataProvider;
};

struct _SCSI_REQUEST_BLOCK {
    USHORT Length;			// length of the request block
    UCHAR Function;			// function to be performed
    UCHAR SrbStatus;			// completion status (set by HBA driver)
    UCHAR ScsiStatus;			// scsi status byte
    UCHAR PathId;			// scsi port or bus for the request
    UCHAR TargetId;			// target controller on the bus
    UCHAR Lun;				// logical unit
    UCHAR QueueTag;
    UCHAR QueueAction;
    UCHAR CdbLength;			// command data block length
    UCHAR SenseInfoBufferLength;	// request-sense buffer length
    ULONG SrbFlags;
    ULONG DataTransferLength;		// data transfer length, if underrun, updated with actual transfer length
    ULONG TimeOutValue;			// timeout, in seconds
    PVOID DataBuffer;			// virtual address of data buffer
    PVOID SenseInfoBuffer;		// points to request-sense buffer (physically contiguous)
    struct _SCSI_REQUEST_BLOCK *NextSrb;
    PVOID OriginalRequest;		// points to the Iopex
    PVOID SrbExtension;			// points to HBA's extension area (physically contiguous)
    ULONG QueueSortKey;
    UCHAR Cdb[16];			// command data block
};

typedef struct _SRB_IO_CONTROL {
    ULONG HeaderLength;
    UCHAR Signature[8];
    ULONG Timeout;
    ULONG ControlCode;
    ULONG ReturnCode;
    ULONG Length;
} SRB_IO_CONTROL, *PSRB_IO_CONTROL;

//
// SRB Functions
//

#define SRB_FUNCTION_EXECUTE_SCSI           0x00
#define SRB_FUNCTION_CLAIM_DEVICE           0x01
#define SRB_FUNCTION_IO_CONTROL             0x02
#define SRB_FUNCTION_RECEIVE_EVENT          0x03
#define SRB_FUNCTION_RELEASE_QUEUE          0x04
#define SRB_FUNCTION_ATTACH_DEVICE          0x05
#define SRB_FUNCTION_RELEASE_DEVICE         0x06
#define SRB_FUNCTION_SHUTDOWN               0x07
#define SRB_FUNCTION_FLUSH                  0x08
#define SRB_FUNCTION_ABORT_COMMAND          0x10
#define SRB_FUNCTION_RELEASE_RECOVERY       0x11
#define SRB_FUNCTION_RESET_BUS              0x12
#define SRB_FUNCTION_RESET_DEVICE           0x13
#define SRB_FUNCTION_TERMINATE_IO           0x14
#define SRB_FUNCTION_FLUSH_QUEUE            0x15
#define SRB_FUNCTION_REMOVE_DEVICE          0x16
#define SRB_FUNCTION_WMI                    0x17
#define SRB_FUNCTION_LOCK_QUEUE             0x18
#define SRB_FUNCTION_UNLOCK_QUEUE           0x19

//
// SRB Status
//

#define SRB_STATUS_PENDING                  0x00
#define SRB_STATUS_SUCCESS                  0x01
#define SRB_STATUS_ABORTED                  0x02
#define SRB_STATUS_ABORT_FAILED             0x03
#define SRB_STATUS_ERROR                    0x04
#define SRB_STATUS_BUSY                     0x05
#define SRB_STATUS_INVALID_REQUEST          0x06
#define SRB_STATUS_INVALID_PATH_ID          0x07
#define SRB_STATUS_NO_DEVICE                0x08
#define SRB_STATUS_TIMEOUT                  0x09
#define SRB_STATUS_SELECTION_TIMEOUT        0x0A
#define SRB_STATUS_COMMAND_TIMEOUT          0x0B
#define SRB_STATUS_MESSAGE_REJECTED         0x0D
#define SRB_STATUS_BUS_RESET                0x0E
#define SRB_STATUS_PARITY_ERROR             0x0F
#define SRB_STATUS_REQUEST_SENSE_FAILED     0x10
#define SRB_STATUS_NO_HBA                   0x11
#define SRB_STATUS_DATA_OVERRUN             0x12
#define SRB_STATUS_UNEXPECTED_BUS_FREE      0x13
#define SRB_STATUS_PHASE_SEQUENCE_FAILURE   0x14
#define SRB_STATUS_BAD_SRB_BLOCK_LENGTH     0x15
#define SRB_STATUS_REQUEST_FLUSHED          0x16
#define SRB_STATUS_INVALID_LUN              0x20
#define SRB_STATUS_INVALID_TARGET_ID        0x21
#define SRB_STATUS_BAD_FUNCTION             0x22
#define SRB_STATUS_ERROR_RECOVERY           0x23
#define SRB_STATUS_NOT_POWERED              0x24

//
// This value is used by the port driver to indicate that a non-scsi-related
// error occured.  Miniports must never return this status.
//

#define SRB_STATUS_INTERNAL_ERROR           0x30

//
// Srb status values 0x38 through 0x3f are reserved for internal port driver 
// use.
// 



//
// SRB Status Masks
//

#define SRB_STATUS_QUEUE_FROZEN             0x40
#define SRB_STATUS_AUTOSENSE_VALID          0x80

#define SRB_STATUS(Status) (Status & ~(SRB_STATUS_AUTOSENSE_VALID | SRB_STATUS_QUEUE_FROZEN))

//
// SRB Flag Bits
//

#define SRB_FLAGS_QUEUE_ACTION_ENABLE       0x00000002
#define SRB_FLAGS_DISABLE_DISCONNECT        0x00000004
#define SRB_FLAGS_DISABLE_SYNCH_TRANSFER    0x00000008
#define SRB_FLAGS_BYPASS_FROZEN_QUEUE       0x00000010
#define SRB_FLAGS_DISABLE_AUTOSENSE         0x00000020
#define SRB_FLAGS_DATA_IN                   0x00000040
#define SRB_FLAGS_DATA_OUT                  0x00000080
#define SRB_FLAGS_NO_DATA_TRANSFER          0x00000000
#define SRB_FLAGS_UNSPECIFIED_DIRECTION      (SRB_FLAGS_DATA_IN | SRB_FLAGS_DATA_OUT)
#define SRB_FLAGS_NO_QUEUE_FREEZE           0x00000100
#define SRB_FLAGS_ADAPTER_CACHE_ENABLE      0x00000200
#define SRB_FLAGS_IS_ACTIVE                 0x00010000
#define SRB_FLAGS_ALLOCATED_FROM_ZONE       0x00020000
#define SRB_FLAGS_SGLIST_FROM_POOL          0x00040000
#define SRB_FLAGS_BYPASS_LOCKED_QUEUE       0x00080000

#define SRB_FLAGS_NO_KEEP_AWAKE             0x00100000

#define SRB_FLAGS_PORT_DRIVER_RESERVED      0x0F000000
#define SRB_FLAGS_CLASS_DRIVER_RESERVED     0xF0000000

//
// Queue Action
//

#define SRB_SIMPLE_TAG_REQUEST              0x20
#define SRB_HEAD_OF_QUEUE_TAG_REQUEST       0x21
#define SRB_ORDERED_QUEUE_TAG_REQUEST       0x22

#define SRB_WMI_FLAGS_ADAPTER_REQUEST       0x01

//
// Return values for SCSI_HW_FIND_ADAPTER.
//

#define SP_RETURN_NOT_FOUND     0
#define SP_RETURN_FOUND         1
#define SP_RETURN_ERROR         2
#define SP_RETURN_BAD_CONFIG    3

//
// Notification Event Types
//

typedef enum _SCSI_NOTIFICATION_TYPE {
    RequestComplete,
    NextRequest,
    NextLuRequest,
    ResetDetected,
    CallDisableInterrupts,
    CallEnableInterrupts,
    RequestTimerCall,
    BusChangeDetected,     /* New */
    WMIEvent,
    WMIReregister
} SCSI_NOTIFICATION_TYPE, *PSCSI_NOTIFICATION_TYPE;

typedef struct Chnex Chnex;
typedef struct Devex Devex;
typedef struct Iopex Iopex;
typedef struct Lu Lu;

struct Chnex { Lu *lu;			// logical unit open on the channel, NULL if closed
               OZ_Lockmode lockmode;	// lockmode the unit was opened with
             };

struct Devex { OZ_Devunit *devunit;				// device independent data pointer
               const char *devname;				// device name string pointer
               uLong scsi_id;					// controller's scsi-id
               OZ_Smplock *smplock_dv;				// irq level smplock
               void *HwDeviceExtension;				// HBA driver's device data pointer
               PPORT_CONFIGURATION_INFORMATION ConfigInfo;	// HBA's configuration info
               int hba_ready;					// 0: controller busy, 1: controller will accept requests
               Iopex  *iopex_all_qh;				// list of waiting requests
               Iopex **iopex_all_qt;				// tail of iopex_qh
               Lu *lus;						// list of logical units defined
               PHW_INTERRUPT hba_interrupt;			// HBA interrupt routine, NULL if disabled
               int int_pending;					// set if an interrupt happened while disabled
               ULONG SpecificLuExtensionSize;			// sizeof Lu->hbaextension
               ULONG SrbExtensionSize;				// sizeof *(Iopex->Srb->SrvExtension)
               PHW_STARTIO HwStartIo;				// start I/O routine entrypoint
               OZ_Hw486_irq_many irq_many;			// interrupt block
             };

struct Iopex { Iopex  *next_all;		// next on devex -> iopex_all_qh
               Iopex **prev_all;
               Iopex  *next_lu;			// next on devex -> iopex_lu_qh
               Iopex **prev_lu;
               Devex *devex;			// corresponding device
               OZ_Ioop *ioop;			// corresponding ioop
               OZ_Procmode procmode;		// procmode of requestor
               OZ_IO_scsi_doiopp doiopp;	// i/o request parameter block
               uLong scsi_id;			// scsi-id the request is for
               SCSI_REQUEST_BLOCK Srb;		// HBA request param block
             };

struct Lu { Lu *next;				// next in devex->lus list
            UCHAR PathId;			// logical unit's path id
            UCHAR TargetId;			// logical unit's target id
            UCHAR Lun;				// lugical unit's lun
            UCHAR lu_ready;			// set if hba ready to accept a new request
            Iopex  *iopex_lu_qh;		// queue of waiting requests
            Iopex **iopex_lu_qt;
            Iopex *inprogq;			// queue of requests in progress

            Long refcount;			// channel refcounts
            Long refc_read;
            Long refc_write;
            Long deny_read;
            Long deny_write;

            UCHAR hbaextension[1];		// hba's extension area (size SpecificLuExtensionSize)
          };

/* Function table */

static int scsiport_shutdown (OZ_Devunit *devunit, void *devexv);
static uLong scsiport_assign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode);
static int scsiport_deassign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv);
static void scsiport_abort (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, 
                            OZ_Ioop *ioop, void *iopexv, OZ_Procmode procmode);
static uLong scsiport_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode,
                            OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap);

static const OZ_Devfunc scsiport_functable = { sizeof (Devex), sizeof (Chnex), sizeof (Iopex), 0,
                                               scsiport_shutdown, NULL, NULL, scsiport_assign,
                                               scsiport_deassign, scsiport_abort, scsiport_start, NULL };

/* Internal static data */

static char *dn = "oz_dev_scsiport";
static int dc = 0;

/* Internal routines */

static uLong qiopp (Chnex *chnex, Iopex *iopex);
static int got_interrupt (void *devexv, OZ_Mchargs *mchargs);
static Lu *get_lu (Devex *devex, UCHAR PathId, UCHAR TargetId, UCHAR Lun);
static void startreq (Iopex *iopex);
static void finishup (void *iopexv, int finok, uLong *status_r);

uLong _start (int argc, char *argv[], OZ_Image *image)

{
  if (argc > 1) dn = argv[0];
  if (argc != 2) {
    oz_knl_printk ("oz_dev_scsiport: usage: %s <drivername>\n", dn);
    return (OZ_BADPARAM);
  }

  dn = argv[1];						// get drivername
  dc = 0;						// haven't created any devices yet
  DriverEntry (NULL, NULL);				// scan and try to create some
  if (dc > 0) oz_knl_image_increfc (image, 1);		// if any created, lock image in memory
  return (OZ_SUCCESS);					// anyway, we're done
}

static int scsiport_shutdown (OZ_Devunit *devunit, void *devexv)

{ }

static uLong scsiport_assign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode)

{
  memset (chnexv, 0, sizeof (Chnex));
  return (OZ_SUCCESS);
}

static int scsiport_deassign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv)

{
  Chnex *chnex;
  Devex *devex;
  Lu *lu;
  OZ_Lockmode lockmode;
  uLong dv;

  chnex = chnexv;
  devex = devexv;
  dv = oz_hw_smplock_wait (devex -> smplock_dv);
  lu = chnex -> lu;
  if (lu != NULL) {
    lockmode = chnex -> lockmode;
    if (!OZ_LOCK_ALLOW_TEST (lockmode, OZ_LOCK_ALLOWS_OTHERS_READ))  lu -> deny_read  --;
    if (!OZ_LOCK_ALLOW_TEST (lockmode, OZ_LOCK_ALLOWS_OTHERS_WRITE)) lu -> deny_write --;
    if  (OZ_LOCK_ALLOW_TEST (lockmode, OZ_LOCK_ALLOWS_SELF_READ))    lu -> refc_read  --;
    if  (OZ_LOCK_ALLOW_TEST (lockmode, OZ_LOCK_ALLOWS_SELF_WRITE))   lu -> refc_write --;
    lu -> refcount --;
    chnex -> lu = NULL;
  }
  oz_hw_smplock_clr (devex -> smplock_dv, dv);

  return (0);

}

static void scsiport_abort (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv,
                            OZ_Ioop *ioop, void *iopexv, OZ_Procmode procmode)

{
  Chnex *chnex;
  Devex *devex;
  Iopex *iopex, **liopex, *xiopex;
  Lu *lu;
  uLong dv;

  chnex  = chnexv;
  devex  = devexv;
  xiopex = NULL;

  dv = oz_hw_smplock_wait (devex -> smplock_dv);

  /* See what logical unit, if any, is open on the channel */

  lu = chnex -> lu;
  if (lu != NULL) {

    /* ?? Tell HBA driver to abort any requests in progress ?? */

    /* Abort stuff that has yet to be started */

    for (liopex = &(lu -> iopex_lu_qh); (iopex = *liopex) != NULL;) {
      if (oz_knl_ioabortok (iopex -> ioop, iochan, procmode, ioop)) {
        *liopex = iopex -> next_lu;						// remove from lu->iopex_lu_q
        if (iopex -> next_lu != NULL) iopex -> next_lu -> prev_lu = liopex;

        *(iopex -> prev_all) = iopex -> next_all;				// remove from devex->iopex_all_q
        if (iopex -> next_all != NULL) iopex -> next_all -> prev_all = liopex;

        iopex -> next_lu = xiopex;						// put on xiopex list
        xiopex = iopex;
      } else {
        liopex = &(iopex -> next_lu);
      }
    }
    lu -> iopex_lu_qt = liopex;
  }

  oz_hw_smplock_clr (devex -> smplock_dv, dv);

  while ((iopex = xiopex) != NULL) {
    xiopex = iopex -> next_lu;
    if (iopex -> Srb.SrbExtension != NULL) OZ_KNL_NPPFREE (iopex -> Srb.SrbExtension);
    oz_knl_iodone (iopex -> ioop, OZ_ABORTED, NULL, NULL, NULL);
  }
}

static uLong scsiport_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode, 
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
      Lu *lu, *newlu;
      OZ_IO_scsi_open scsi_open;
      OZ_Lockmode iochlkm, lockmode;
      uLong dv, scsi_id, sts;

      /* Retrieve and validate parameters */

      movc4 (as, ap, sizeof scsi_open, &scsi_open);
      scsi_id  = scsi_open.scsi_id;
      lockmode = scsi_open.lockmode;
      if (scsi_id >= devex -> ConfigInfo -> MaximumNumberOfTargets) return (OZ_BADSCSIID);
      if (scsi_id == devex -> ConfigInfo -> InitiatorBusId[0]) return (OZ_BADSCSIID);

      iochlkm = oz_knl_iochan_getlockmode (iochan);
      if (OZ_LOCK_ALLOW_TEST (lockmode, OZ_LOCK_ALLOWS_SELF_WRITE) && !OZ_LOCK_ALLOW_TEST (iochlkm, OZ_LOCK_ALLOWS_SELF_WRITE)) {
        return (OZ_NOWRITEACCESS);
      }
      if (OZ_LOCK_ALLOW_TEST (lockmode, OZ_LOCK_ALLOWS_SELF_READ)  && !OZ_LOCK_ALLOW_TEST (iochlkm, OZ_LOCK_ALLOWS_SELF_READ)) {
        return (OZ_NOREADACCESS);
      }

      /* Mark the channel open and lock access to the logical unit */

      newlu = OZ_KNL_NPPMALLOC (devex -> SpecificLuExtensionSize + sizeof *lu);

      dv = oz_hw_smplock_wait (devex -> smplock_dv);
      if (chnex -> lu != NULL) sts = OZ_FILEALREADYOPEN;
      else {
        lu = get_lu (devex, 0, scsi_id, 0);
        if (lu == NULL) {
          lu = newlu;
          memset (lu, 0, devex -> SpecificLuExtensionSize + sizeof *lu);
          lu -> next        = devex -> lus;
          lu -> PathId      = 0;
          lu -> TargetId    = scsi_id;
          lu -> Lun         = 0;
          lu -> iopex_lu_qt = &(lu -> iopex_lu_qh);
          lu -> lu_ready    = 1; // or are we supposed to wait for an ScsiPortNotification ??
          devex -> lus      = lu;
          newlu = NULL;
        }

        if ((!OZ_LOCK_ALLOW_TEST (lockmode, OZ_LOCK_ALLOWS_OTHERS_READ)  && (lu -> refc_read  != 0)) 
         || (!OZ_LOCK_ALLOW_TEST (lockmode, OZ_LOCK_ALLOWS_OTHERS_WRITE) && (lu -> refc_write != 0)) 
         ||  (OZ_LOCK_ALLOW_TEST (lockmode, OZ_LOCK_ALLOWS_SELF_READ)    && (lu -> deny_read  != 0)) 
         ||  (OZ_LOCK_ALLOW_TEST (lockmode, OZ_LOCK_ALLOWS_SELF_WRITE)   && (lu -> deny_write != 0))) {
          sts = OZ_ACCONFLICT;
        } else {
          if (!OZ_LOCK_ALLOW_TEST (lockmode, OZ_LOCK_ALLOWS_OTHERS_READ))  lu -> deny_read  ++;
          if (!OZ_LOCK_ALLOW_TEST (lockmode, OZ_LOCK_ALLOWS_OTHERS_WRITE)) lu -> deny_write ++;
          if  (OZ_LOCK_ALLOW_TEST (lockmode, OZ_LOCK_ALLOWS_SELF_READ))    lu -> refc_read  ++;
          if  (OZ_LOCK_ALLOW_TEST (lockmode, OZ_LOCK_ALLOWS_SELF_WRITE))   lu -> refc_write ++;
          lu -> refcount ++;
          chnex -> lu = lu;
          chnex -> lockmode = lockmode;
          sts = OZ_SUCCESS;
        }
      }
      oz_hw_smplock_clr (devex -> smplock_dv, dv);
      if (newlu != NULL) OZ_KNL_NPPFREE (newlu);
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
      Lu *lu;
      OZ_IO_scsi_getinfo1 scsi_getinfo1;
      uLong sts;

      sts = oz_knl_ioop_lockw (ioop, as, ap, NULL, NULL, NULL);
      if (sts == OZ_SUCCESS) {
        memset (&scsi_getinfo1, 0, sizeof scsi_getinfo1);
        scsi_getinfo1.max_scsi_id    = devex -> ConfigInfo -> MaximumNumberOfTargets; /* max scsi id allowed on this controller+1 */
        scsi_getinfo1.ctrl_scsi_id   = devex -> ConfigInfo -> InitiatorBusId[0]; /* what the controller's scsi id is */
        scsi_getinfo1.open_scsi_id   = -1;					/* assume no scsi id open on channel */
        lu = chnex -> lu;
        if (lu != NULL) {
          scsi_getinfo1.open_scsi_id = chnex -> lu -> TargetId;			/* ok, get the open scsi id */
        }
        movc4 (sizeof scsi_getinfo1, &scsi_getinfo1, as, ap);
      }
      return (sts);
    }
  }

  return (OZ_BADIOFUNC);
}

static uLong qiopp (Chnex *chnex, Iopex *iopex)

{
  Devex *devex;
  Lu *lu;
  OZ_IO_scsi_doiopp *doiopp;
  SCSI_REQUEST_BLOCK *Srb;
  uLong dv;

  devex  = iopex -> devex;
  doiopp = &(iopex -> doiopp);

  /* Validate parameters */

  lu = chnex -> lu;
  if (lu == NULL) return (OZ_FILENOTOPEN);
  if (!OZ_LOCK_ALLOW_TEST (chnex -> lockmode, OZ_LOCK_ALLOWS_SELF_WRITE)) return (OZ_NOWRITEACCESS);
  if ((doiopp -> cmdlen == 0) || (doiopp -> cmdlen > sizeof Srb -> Cdb)) return (OZ_BADBUFFERSIZE);

  /* Fill in scsi request block */

  Srb = &(iopex -> Srb);

  memset (Srb, 0, sizeof *Srb);

  Srb -> Length          = sizeof *Srb;
  Srb -> Function        = SRB_FUNCTION_EXECUTE_SCSI;
  Srb -> SrbStatus       = SRB_STATUS_PENDING;
  Srb -> PathId          = lu -> PathId;
  Srb -> TargetId        = lu -> TargetId;
  Srb -> Lun             = lu -> Lun;
  Srb -> CdbLength       = doiopp -> cmdlen;
  Srb -> SrbFlags        = SRB_FLAGS_DISABLE_AUTOSENSE;
  if (doiopp -> datasize == 0) Srb -> SrbFlags |= SRB_FLAGS_NO_DATA_TRANSFER;
  else if (doiopp -> optflags & OZ_IO_SCSI_OPTFLAG_WRITE) Srb -> SrbFlags |= SRB_FLAGS_DATA_OUT;
  else Srb -> SrbFlags |= SRB_FLAGS_DATA_IN;
  Srb -> DataTransferLength = doiopp -> datasize;
  Srb -> TimeOutValue    = doiopp -> timeout;
  Srb -> DataBuffer      = (void *)1;
  Srb -> OriginalRequest = iopex;
  if (devex -> SrbExtensionSize != 0) {
    Srb -> SrbExtension  = OZ_KNL_PCMALLOC (devex -> SrbExtensionSize);
  }
  memcpy (Srb -> Cdb, doiopp -> cmdbuf, doiopp -> cmdlen);

  /* Queue request to controller */

  iopex -> next_all = NULL;
  iopex -> next_lu  = NULL;

  dv = oz_hw_smplock_wait (devex -> smplock_dv);		// lock the state
  if (lu -> lu_ready) {						// see if this lu is ready for requests
    lu -> lu_ready = 0;						// ok, no longer ready
    startreq (iopex);						// start this request
  } else if (devex -> hba_ready) {				// see if HBA ready for requests
    devex -> hba_ready = 0;					// ok, no longer ready
    startreq (iopex);						// start this request
  } else {
    iopex -> prev_all = devex -> iopex_all_qt;			// busy, put this request on end of queues
    iopex -> prev_lu  = lu -> iopex_lu_qt;
    *(devex -> iopex_all_qt) = iopex;
    *(lu -> iopex_lu_qt)     = iopex;
    devex -> iopex_all_qt = &(iopex -> next_all);
    lu -> iopex_lu_qt     = &(iopex -> next_lu);
  }
  oz_hw_smplock_clr (devex -> smplock_dv, dv);			// unlock state

  /* Tell caller it will complete asynchronously */

  return (OZ_STARTED);
}

/************************************************************************/
/*									*/
/*  Got an interrupt from the controller				*/
/*									*/
/************************************************************************/

static int got_interrupt (void *devexv, OZ_Mchargs *mchargs)

{
  Devex *devex;

  devex = devexv;
  if (devex -> hba_interrupt == NULL) devex -> int_pending = 1;
  else (*(devex -> hba_interrupt)) (devex -> HwDeviceExtension);

  return (0);
}

/************************************************************************/
/*									*/
/*  Completes ALL of the active requests for the specified logical 	*/
/*  unit.  It can be called to complete requests after a bus reset, 	*/
/*  a device reset, or an abort, rather than calling 			*/
/*  ScsiPortNotification for each request.				*/
/*									*/
/*    Input:								*/
/*									*/
/*	HwDeviceExtension = HBA's per-device data			*/
/*	PathId,TargetId,Lun = logical unit				*/
/*	SrbStatus = status value for each srb's SrbStatus field		*/
/*									*/
/*    Output:								*/
/*									*/
/*	all in-progress requests for the lun are completed		*/
/*									*/
/************************************************************************/


VOID ScsiPortCompleteRequest (PVOID HwDeviceExtension, 
                              UCHAR PathId, 
                              UCHAR TargetId, 
                              UCHAR Lun, 
                              UCHAR SrbStatus)

{
  Devex *devex;
  Iopex *iopex;
  Lu *lu;
  uLong dv;

  devex = DEVEX (HwDeviceExtension);			// point to my per-device data
  dv = oz_hw_smplock_wait (devex -> smplock_dv);	// lock the queues
  lu = get_lu (devex, PathId, TargetId, Lun);		// point to my per-unit data
  while ((iopex = lu -> inprogq) != NULL) {		// see if any request in prog on the unit
    lu -> inprogq = iopex -> next_lu;			// if so, unlink it
    iopex -> Srb.SrbStatus = SrbStatus;			// set the completion status
    oz_knl_iodonehi (iopex -> ioop, OZ_SUCCESS, NULL, finishup, iopex); // post it
  }							// repeat for all on the unit
  oz_hw_smplock_clr (devex -> smplock_dv, dv);		// unlock the queue
}

/************************************************************************/
/*									*/
/*  Frees a block of I/O addresses or memory space previously mapped 	*/
/*  into the system address space					*/
/*									*/
/*    Input:								*/
/*									*/
/*	HwDeviceExtension = points to HBA device extension struct	*/
/*	MappedAddress = address that device registers were mapped to	*/
/*									*/
/*    Output:								*/
/*									*/
/*	MappedAddress = no longer points to valid memory		*/
/*									*/
/************************************************************************/

VOID ScsiPortFreeDeviceBase (PVOID HwDeviceExtension, PVOID MappedAddress)

{
  OZ_Hw_pageprot pageprot;
  OZ_Mempage npages, pageoffs, vpage;
  OZ_Procmode procmode;
  OZ_Section *section;
  uLong mapsecflags;

  if ((OZ_Pointer)MappedAddress < FAKE_IO_BASE_VA) {

    /* See what section is mapped there and how big it is */

    vpage  = OZ_HW_VADDRTOVPAGE (MappedAddress);
    npages = oz_knl_process_getsecfromvpage (oz_s_systemproc, vpage, &section, &pageoffs, &pageprot, &procmode, &mapsecflags);
    if (npages == 0) oz_crash ("oz_dev_scsiport: ScsiPortFreeDeviceBase: no section at vpage %x", vpage);

    /* Back up to get the whole section */

    vpage  -= pageoffs;
    npages += pageoffs;

    /* Mark the pages invalid and free the section block so the virtual addresses are available for re-use */

    oz_knl_spte_free (npages, vpage);
  }
}

/************************************************************************/
/*									*/
/*  Gets bus-type-specific information					*/
/*									*/
/*    Input:								*/
/*									*/
/*	DeviceExtension = as passed to HwScsiFindAdapter		*/
/*	BusDataType = type of bus-specific config data to return	*/
/*	SystemIoBusNumber = as passed to HwScsiFindAdapter in 		*/
/*	                    ConfigInfo -> SystemIoBusNumber		*/
/*	SlotNumber = slot number location of the device			*/
/*	             if BusDataType == PCIConfiguration, 		*/
/*	               this param is PCI_SLOT_NUMBER type		*/
/*									*/
/*    Output:								*/
/*									*/
/*	ScsiPortGetBusData = 0 : the PCI bus does not exist		*/
/*	                     2 : there is no device in the slot		*/
/*	                         (Buffer -> VendorId == PCI_INVALID_VENDOR_ID)
/*	                  else : # of bytes of Buffer filled in		*/
/*									*/
/*    Note:								*/
/*									*/
/*	This can be called only from HBA's HwFindAdapter routine	*/
/*									*/
/************************************************************************/

ULONG ScsiPortGetBusData (PVOID DeviceExtension, 
                          ULONG BusDataType, 
                          ULONG SystemIoBusNumber, 
                          ULONG SlotNumber, 
                          PVOID Buffer, 
                          ULONG Length)

{
  OZ_Dev_pci_conf_p pciconfp;
  PCI_SLOT_NUMBER pcislotnumber;
  uLong i;
  uWord vid;

  /* We only do PCI bus */

  if (BusDataType != PCIConfiguration) return (0);

  /* Make sure PCIBIOS is present, if not, return 0 */

  if (!oz_dev_pci_present ()) return (0);

  /* Config space is at most 256 bytes int */

  if (Length > 256) Length = 256;

  /* Set up an OZONE-style PCI address block */

  pcislotnumber.u.AsULONG = SlotNumber;
  memset (&pciconfp, 0, sizeof pciconfp);
  pciconfp.pcibus  = SystemIoBusNumber;
  pciconfp.pcidev  = pcislotnumber.u.bits.DeviceNumber;
  pciconfp.pcifunc = pcislotnumber.u.bits.FunctionNumber;

  /* Make sure there is something in slot, if not, return 2 */

  vid = oz_dev_pci_conf_inw (&pciconfp, 0);
  if (vid == PCI_INVALID_VENDOR_ID) {
    i = 2;
    if (i > Length) i = Length;
    memcpy (Buffer, &vid, i);
  }

  /* Read config space into Buffer */

  else {
    i = 0;
    while (Length > 3) {
      Length -= 4;
      *((uLong *)Buffer)  = oz_dev_pci_conf_inl (&pciconfp, i);
      (OZ_Pointer)Buffer += 4;
      i += 4;
    }

    if (Length > 1) {
      Length -= 2;
      *((uWord *)Buffer)  = oz_dev_pci_conf_inw (&pciconfp, i);
      (OZ_Pointer)Buffer += 2;
      i += 2;
    }

    if (Length > 0) {
      *((uByte *)Buffer)  = oz_dev_pci_conf_inb (&pciconfp, i);
      i ++;
    }
  }

  return (i);
}

/************************************************************************/
/*									*/
/*  Returns a mapped system address that must be used to adjust device 	*/
/*  address ranges in that HBA's ACCESS_RANGE elements			*/
/*									*/
/*    Input:								*/
/*									*/
/*	HwDeviceExtension = points to HBA device extension		*/
/*	BusType = type of bus, retrieved from PORT_CONFIGURATION_INFORMATION struct
/*	SystemIoBusNumber = specifies on which bus the HBA is connected	*/
/*	                    retrieved from PORT_CONFIGURATION_INFORMATION struct
/*	IoAddress = specifies the starting physical base address for the HBA
/*	NumberOfBytes = specifies the number of bytes that the mapping covers
/*									*/
/*    Output:								*/
/*									*/
/*	ScsiPortGetDeviceBase = NULL : can't be mapped			*/
/*	                        else : virt address it is mapped to	*/
/*									*/
/************************************************************************/

PVOID ScsiPortGetDeviceBase (PVOID HwDeviceExtension, 
                             INTERFACE_TYPE BusType, 
                             ULONG SystemIoBusNumber, 
                             SCSI_PHYSICAL_ADDRESS IoAddress, 
                             ULONG NumberOfBytes, 
                             BOOLEAN InIoSpace)

{
  OZ_Mempage i, npages, phypage, sysvpage;
  uByte *vaddr;
  uLong sts;
  void *sysvaddr;

  /* If I/O space, use our fake virtual addresses for it */

  if (InIoSpace) return ((PVOID)(FAKE_IO_BASE_VA + IoAddress));

  npages = (((IoAddress & ((1 << OZ_HW_L2PAGESIZE) - 1)) + NumberOfBytes - 1) >> OZ_HW_L2PAGESIZE) + 1;

  sts = oz_knl_spte_alloc (npages, &sysvaddr, &sysvpage, NULL);
  if (sts != OZ_SUCCESS) return (NULL);

  phypage = IoAddress >> OZ_HW_L2PAGESIZE;
  vaddr   = sysvaddr;
  for (i = 0; i < npages; i ++) {
    oz_hw_map_iopage (phypage, vaddr);
    phypage ++;
    vaddr += 1 << OZ_HW_L2PAGESIZE;
  }

  vaddr  = sysvaddr;
  vaddr += IoAddress & ((1 << OZ_HW_L2PAGESIZE) - 1);
  return (vaddr);
}

/************************************************************************/
/*									*/
/*  Provides access to logical-unit-specific data for a device on the 	*/
/*  SCSI bus								*/
/*									*/
/************************************************************************/

PVOID ScsiPortGetLogicalUnit (PVOID HwDeviceExtension, UCHAR PathId, UCHAR TargetId, UCHAR Lun)

{
  Devex *devex;
  Lu *lu;
  uLong dv;

  devex = DEVEX (HwDeviceExtension);
  lu = get_lu (devex, PathId, TargetId, Lun);
  return (lu -> hbaextension);
}
  
static Lu *get_lu (Devex *devex, UCHAR PathId, UCHAR TargetId, UCHAR Lun)

{
  Lu *lu;
  uLong dv;

  dv = oz_hw_smplock_wait (devex -> smplock_dv);
  for (lu = devex -> lus; lu != NULL; lu = lu -> next) {
    if ((lu -> PathId == PathId) && (lu -> TargetId == TargetId) && (lu -> Lun == Lun)) break;
  }
  oz_hw_smplock_clr (devex -> smplock_dv, dv);

  return (lu);
}

/************************************************************************/
/*									*/
/*  Translates a virtual address range to a physical address range for 	*/
/*  a DMA operation.							*/
/*									*/
/*    Input:								*/
/*									*/
/*	Srb = scsi request block pointer				*/
/*	      NULL if VirtualAddress is a system buffer			*/
/*	VirtualAddress = virtual address to be translated		*/
/*									*/
/*    Output:								*/
/*									*/
/*	ScsiPortGetPhysicalAddress = corresponding PCI address		*/
/*	*length = number of physically contiguous bytes			*/
/*									*/
/************************************************************************/

SCSI_PHYSICAL_ADDRESS ScsiPortGetPhysicalAddress (PVOID HwDeviceExtension, 
                                                  PSCSI_REQUEST_BLOCK Srb, 
                                                  PVOID VirtualAddress, 
                                                  ULONG *length)

{
  Iopex *iopex;
  OZ_Mempage ppn;
  uLong offset, ppo;

  /* If no Srb supplied, it must be a system non-paged pool address */

  if (Srb == NULL) {
    *length = oz_knl_misc_sva2pa (VirtualAddress, &ppn, &ppo);	// get starting phys page number and offset in the page
								// return number of physically contiguous bytes starting there
  }

  /* An Srb is supplied, the VirtualAddress is part of the Srb's DataBuffer */

  iopex = Srb -> OriginalRequest;

  offset  = (uByte *)VirtualAddress - (uByte *)1;		// get offset in buffer they are requesting
  *length = 0;							// assume offset is too large
  ppn     = 0;
  ppo     = 0;
  if (offset < iopex -> doiopp.datasize) {			// see if offset is in range
    offset += iopex -> doiopp.databyteoffs;			// ok, increment for offset in first page
    ppn     = iopex -> doiopp.dataphypages[offset>>OZ_HW_L2PAGESIZE]; // get physical page number
    ppo     = offset % (1 << OZ_HW_L2PAGESIZE);			// get offset in the page
    *length = (1 << OZ_HW_L2PAGESIZE) - ppo;
    offset  = iopex -> doiopp.datasize + (uByte *)1 - (uByte *)VirtualAddress; // get number of bytes at that address
    if (*length > offset) *length = offset;			// make sure we aren't returning too big a length
  }

  return ((ppn << OZ_HW_L2PAGESIZE) + ppo);			// return the PCI bus address of the VirtualAddress
}

/************************************************************************/
/*									*/
/*  Allocates memory that can be used by both the CPU and host bus 	*/
/*  adapter for DMA or shared data.  The memory must be marked by the 	*/
/*  CPU as not being cacheable.						*/
/*									*/
/*    Input:								*/
/*									*/
/*	HwDeviceExtension = HBA driver's extension area			*/
/*	ConfigInfo = DMA config info					*/
/*	 -> DmaChannel or DmaPort = 
/*	 -> DmaWidth = 
/*	 -> DmaSpeed = 
/*	 -> MaximumTransferLength = 
/*	 -> ScatterGather = 
/*	 -> Master = TRUE
/*	 -> NumberOfPhysicalBreaks = 
/*	 -> AdapterInterfaceType = 
/*	 -> Dma32BitAddresses = 
/*	 -> SystemIoBusNumber = 
/*	 -> AutoRequestSense = 
/*	 -> SrbExtensionSize = 
/*	NumberOfBytes = number of bytes to allocate			*/
/*									*/
/*    Output:								*/
/*									*/
/*	ScsiPortGetUncachedExtension = NULL : allocation failed		*/
/*	                               else : virt address of memory	*/
/*									*/
/*    Note:								*/
/*									*/
/*	This shall be called only from HBA's HwFindAdapter routine	*/
/*									*/
/************************************************************************/

PVOID *ScsiPortGetUncachedExtension (PVOID *HwDeviceExtension, 
                                     PPORT_CONFIGURATION_INFORMATION ConfigInfo, 
                                     ULONG NumberOfBytes)

{
  OZ_Mempage i, npages, phypage, sysvpage;
  OZ_Pointer sysva;
  uLong pm, sts;
  void *sysvaddr;

  /* See how many pages are required */

  npages = (NumberOfBytes + (1 << OZ_HW_L2PAGESIZE) - 1) >> OZ_HW_L2PAGESIZE;
  if (npages == 0) return (NULL);

  /* Allocate some system pagetable entries for them */

  sts = oz_knl_spte_alloc (npages, &sysvaddr, &sysvpage, NULL);
  if (sts != OZ_SUCCESS) return (NULL);

  /* Allocate contiguous physical pages */

  pm = oz_hw_smplock_wait (&oz_s_smplock_pm);
  if (npages == 1) phypage = oz_knl_phymem_allocpage (OZ_PHYMEM_PAGESTATE_ALLOCSECT);
  else phypage = oz_knl_phymem_allocontig (npages, OZ_PHYMEM_PAGESTATE_ALLOCSECT);
  oz_hw_smplock_clr (&oz_s_smplock_pm, pm);

  if (phypage == OZ_PHYPAGE_NULL) {
    oz_knl_spte_free (npages, sysvpage);
    return (NULL);
  }

  /* Write their pagetable entries as non-cacheable */

  for (i = 0; i < npages; i ++) {
    oz_hw_map_iopage (phypage, OZ_HW_VPAGETOVADDR (sysvpage));
    phypage  ++;
    sysvpage ++;
  }

  /* Return base virtual address */

  return (sysvaddr);
}

/************************************************************************/
/*									*/
/*  Sets up system objects on behalf of the HBA miniport driver		*/
/*									*/
/*    Input:								*/
/*									*/
/*	Argument1,2 = as passed to DriverEntry routine, ie, NULL	*/
/*	HwInitializationData = stuff about the controller type		*/
/*	                       (gets wiped out on return)		*/
/*									*/
/*    Output:								*/
/*									*/
/*	OZONE devunit's created						*/
/*									*/
/************************************************************************/

ULONG ScsiPortInitialize (PVOID Argument1, PVOID Argument2, HW_INITIALIZATION_DATA *HwInitializationData, PVOID HwContext)

{
  BOOLEAN Again;
  char unitdesc[OZ_DEVUNIT_DESCSIZE], unitname[OZ_DEVUNIT_NAMESIZE];
  Devex *devex;
  OZ_Devclass *devclass;
  OZ_Devdriver *devdriver;
  OZ_Devunit *devunit;
  PCI_SLOT_NUMBER pcislotnumber;
  PPORT_CONFIGURATION_INFORMATION ConfigInfo;
  ULONG sts;
  void *BusInformation, *DeviceExtension;

  do {

    /* Mallocate the DeviceExtension used by the HBA driver */

    DeviceExtension = OZ_KNL_NPPMALLOC (HwInitializationData -> DeviceExtensionSize + sizeof devex);
    memset (DeviceExtension, 0, HwInitializationData -> DeviceExtensionSize + sizeof devex);
    (OZ_Pointer)DeviceExtension += sizeof devex;

    /* Mallocate the BusInformation and fill it in */

    /* ?? not used by ini910u driver, maybe not used by aic78xx one either ?? */

    BusInformation = NULL;

    //switch (HwInitializationData -> AdapterInterfaceType) {
    //  BusInformation = OZ_KNL_NPPMALLOC (??);
    //}

    /* Mallocate and fill in the ConfigInfo */
    /* http://www.osr.com/ddk/k306_2h4i.htm */

    ConfigInfo = OZ_KNL_NPPMALLOC (sizeof *ConfigInfo);
    memset (ConfigInfo, 0, sizeof *ConfigInfo);

    ConfigInfo -> Length = sizeof *ConfigInfo;
    ConfigInfo -> AdapterInterfaceType = HwInitializationData -> AdapterInterfaceType;
    ConfigInfo -> MaximumTransferLength = SP_UNINITIALIZED_VALUE;
    ConfigInfo -> NumberOfPhysicalBreaks = SP_UNINITIALIZED_VALUE;
    ConfigInfo -> DmaChannel = SP_UNINITIALIZED_VALUE;
    ConfigInfo -> DmaPort = SP_UNINITIALIZED_VALUE;
    ConfigInfo -> NumberOfAccessRanges = HwInitializationData -> NumberOfAccessRanges;
    if (ConfigInfo -> NumberOfAccessRanges > 0) {
      ConfigInfo -> AccessRanges = OZ_KNL_NPPMALLOC (ConfigInfo -> NumberOfAccessRanges * sizeof *(ConfigInfo -> AccessRanges));
      memset (ConfigInfo -> AccessRanges, 0, ConfigInfo -> NumberOfAccessRanges * sizeof *(ConfigInfo -> AccessRanges));
    }
    ConfigInfo -> MapBuffers = HwInitializationData -> MapBuffers;
    ConfigInfo -> NeedPhysicalAddresses = HwInitializationData -> NeedPhysicalAddresses;
    ConfigInfo -> TaggedQueuing = HwInitializationData -> TaggedQueuing;
    ConfigInfo -> AutoRequestSense = HwInitializationData -> AutoRequestSense;
    ConfigInfo -> MultipleRequestPerLu = HwInitializationData -> MultipleRequestPerLu;
    ConfigInfo -> ReceiveEvent = HwInitializationData -> ReceiveEvent;
    ConfigInfo -> MaximumNumberOfTargets = SCSI_MAXIMUM_TARGETS;
    ConfigInfo -> MaximumNumberOfLogicalUnits = SCSI_MAXIMUM_LOGICAL_UNITS;

    switch (HwInitializationData -> AdapterInterfaceType) {
      case PCIBus: {
        ConfigInfo -> SystemIoBusNumber = 0; // depends on AdapterInterfaceType;
        ConfigInfo -> InterruptMode = LevelSensitive; // LevelSensitive or Latched  (LevelSensitive for PCIBus)
        break;
      }

      default: {
        oz_knl_printk ("oz_dev_scsiport: ScsiPortInitialize unsupported AdapterInterfaceType %d", 
				HwInitializationData -> AdapterInterfaceType);
        return;
      }
    }

    /* Tell HBA driver to look for an adapter */

    Again = 0;
    sts = (*(HwInitializationData -> HwFindAdapter)) (DeviceExtension, 
                                                      HwContext, 
                                                      BusInformation, 
                                                      "", // ArgumentString
                                                      ConfigInfo, 
                                                      &Again);

    /* If error, stop scanning */

    if (sts != SP_RETURN_FOUND) {
      oz_knl_printk ("oz_dev_scsiport: HwFindAdapter error %u\n", sts);
      break;
    }

    /* An adapter was found, and the ConfigInfo struct is filled in */

    pcislotnumber.u.AsULONG = ConfigInfo -> SlotNumber;
    if (pcislotnumber.u.bits.FunctionNumber == 0) {
      oz_sys_sprintf (sizeof unitname, unitname, "%s_%u_%u", dn, ConfigInfo -> SystemIoBusNumber, 
                                                                 pcislotnumber.u.bits.DeviceNumber);
    } else {
      oz_sys_sprintf (sizeof unitname, unitname, "%s_%u_%u_%u", dn, ConfigInfo -> SystemIoBusNumber, 
                                                                    pcislotnumber.u.bits.DeviceNumber, 
                                                                    pcislotnumber.u.bits.FunctionNumber);
    }
    oz_sys_sprintf (sizeof unitdesc, unitdesc, "via oz_dev_scsiport");

    devclass  = oz_knl_devclass_create (OZ_IO_SCSI_CLASSNAME, OZ_IO_SCSI_BASE, OZ_IO_SCSI_MASK, "scsiport");
    devdriver = oz_knl_devdriver_create (devclass, "scsiport");
    devunit   = oz_knl_devunit_create (devdriver, unitname, unitdesc, &scsiport_functable, 0, oz_s_secattr_sysdev);
    devex     = oz_knl_devunit_ex (devunit);

    memset (devex, 0, sizeof *devex);
    DEVEX (DeviceExtension)          = devex;
    devex -> devunit                 = devunit;
    devex -> devname                 = oz_knl_devunit_devname (devunit);
    devex -> HwDeviceExtension       = DeviceExtension;
    devex -> ConfigInfo              = ConfigInfo;
    devex -> irq_many.entry          = got_interrupt;
    devex -> irq_many.param          = devex;
    devex -> irq_many.descr          = devex -> devname;
    devex -> smplock_dv              = oz_hw486_irq_many_add (ConfigInfo -> BusInterruptLevel, &(devex -> irq_many));
    devex -> iopex_all_qt            = &(devex -> iopex_all_qh);
    devex -> SpecificLuExtensionSize = HwInitializationData -> SpecificLuExtensionSize;
    devex -> SrbExtensionSize        = HwInitializationData -> SrbExtensionSize;
    devex -> HwStartIo               = HwInitializationData -> HwStartIo;
    OZ_HW_MB;
    devex -> hba_ready               = 1; // or are we supposed to wait for an ScsiPortNotification ??

    /* We created a device, so keep driver image locked in memory */

    dc ++;

    /* Set up an autogen routine to automatically configure devices on the bus */

    oz_knl_devunit_autogen (devunit, oz_dev_scsi_auto, NULL);

    /* Initialize HW controller */

    if ((*(HwInitializationData -> HwInitialize)) (DeviceExtension)) oz_knl_printk ("oz_dev_scsiport: %s online\n", devex -> devname);
    else oz_knl_printk ("oz_dev_scsiport: %s offline\n", devex -> devname);

    /* Repeat if there could be more of them */

  } while (Again);
}

VOID ScsiPortLogError (PVOID HwDeviceExtension, 
                       PSCSI_REQUEST_BLOCK Srb, 
                       UCHAR PathId, 
                       UCHAR TargetId, 
                       UCHAR Lun, 
                       LONG ErrorCode, 
                       LONG UniqueId)

{
  oz_knl_printk ("oz_dev_scsiport: ScsiPortLogError\n");
}

/************************************************************************/
/*									*/
/*  Notifies us of several conditions					*/
/*									*/
/************************************************************************/

VOID ScsiPortNotification (SCSI_NOTIFICATION_TYPE NotificationType, PVOID HwDeviceExtension, ...)

{
  Devex *devex;
  Iopex *iopex;
  Lu *lu;
  PSCSI_REQUEST_BLOCK Srb;
  UCHAR Lun, PathId, TargetId;
  uLong dv;
  va_list ap;

  devex = DEVEX (HwDeviceExtension);

  va_start (ap, HwDeviceExtension);

  switch (NotificationType) {

    /* Indicates the supplied Srb has finished */

    case RequestComplete: {
      Srb   = va_arg (ap, PSCSI_REQUEST_BLOCK);
      iopex = Srb -> OriginalRequest;
      oz_knl_iodonehi (iopex -> ioop, OZ_SUCCESS, NULL, finishup, iopex);
      break;
    }

    /* Indicates the HBA driver is ready for another request */

    case NextRequest: {
      dv    = oz_hw_smplock_wait (devex -> smplock_dv);			// lock state
      iopex = devex -> iopex_all_qh;					// see if any request waiting already
      if (iopex == NULL) devex -> hba_ready = 1;			// if not, say controller ready to accept incoming request
      else {
        lu -> lu_ready = 0;						// if so, say it's not ready for another request
        devex -> iopex_all_qh = iopex -> next_all;			// unlink request from queues
        if (devex -> iopex_all_qh == NULL) devex -> iopex_all_qt = &(devex -> iopex_all_qh);
        lu = get_lu (devex, iopex -> Srb.PathId, iopex -> Srb.TargetId, iopex -> Srb.Lun);
        lu -> iopex_lu_qh = iopex -> next_lu;
        if (lu -> iopex_lu_qh == NULL) lu -> iopex_lu_qt = &(lu -> iopex_lu_qh);
        startreq (iopex);						// start request
      }
      oz_hw_smplock_clr (devex -> smplock_dv, dv);			// release device state
      break;
    }

    /* Indicates the HBA driver is ready for another request on the specified logical unit */

    case NextLuRequest: {
      PathId   = va_arg (ap, UCHAR);					// point to logical unit's struct
      TargetId = va_arg (ap, UCHAR);
      Lun      = va_arg (ap, UCHAR);
      lu       = get_lu (devex, PathId, TargetId, Lun);
      dv       = oz_hw_smplock_wait (devex -> smplock_dv);		// lock device state
      iopex    = lu -> iopex_lu_qh;					// see if any request waiting already
      if (iopex == NULL) lu -> lu_ready = 1;				// if not, say lu ready to accept incoming request
      else {
        lu -> lu_ready = 0;						// if so, say it's not ready for another request
        devex -> iopex_all_qh = iopex -> next_all;			// unlink request from queues
        if (devex -> iopex_all_qh == NULL) devex -> iopex_all_qt = &(devex -> iopex_all_qh);
        lu -> iopex_lu_qh = iopex -> next_lu;
        if (lu -> iopex_lu_qh == NULL) lu -> iopex_lu_qt = &(lu -> iopex_lu_qh);
        startreq (iopex);						// start request
      }
      oz_hw_smplock_clr (devex -> smplock_dv, dv);			// release device state
      break;
    }

    /* Indicates the SCSI bus has been reset somehow */

    case ResetDetected: {
      break;
    }

    /* Indicates the HBA driver requires system interrupts to be re-disabled */

    case CallDisableInterrupts: {
      devex -> hba_interrupt = NULL;
      break;
    }

    /* Indicates the HBA driver would like system interrupts re-enabled */

    case CallEnableInterrupts: {
      dv = oz_hw_smplock_wait (devex -> smplock_dv);		// lock state
      devex -> hba_interrupt = va_arg (ap, PHW_INTERRUPT);	// get service routine address
      if (devex -> int_pending) {				// see if there is a pending interrupt
        devex -> int_pending = 0;				// if so, say no more
        (*(devex -> hba_interrupt)) (devex -> HwDeviceExtension); // process interrupt
      }
      oz_hw_smplock_clr (devex -> smplock_dv, dv);		// release device state
      break;
    }

    /* Indicates the miniport has requested a specified routine be called in the given number of microseconds */

    case RequestTimerCall: {
      PHW_TIMER entrypoint;
      ULONG timervalue;

      entrypoint = va_arg (ap, PHW_TIMER);
      timervalue = va_arg (ap, ULONG);

      oz_crash ("oz_dev_scsiport: RequestTimerCall not supported");
      break;
    }

    /* Who knows what */

    default: oz_crash ("oz_dev_scsiport: ScsiPortNotification NotificationType %d invalid", NotificationType);
  }

  va_end (ap);
}

/************************************************************************/
/*									*/
/*  Start processing I/O request, put it on 'in progress' queue		*/
/*									*/
/************************************************************************/

static void startreq (Iopex *iopex)

{
  Lu *lu;

  lu = get_lu (iopex -> devex, iopex -> Srb.PathId, iopex -> Srb.TargetId, iopex -> Srb.Lun);
  iopex -> next_lu = lu -> inprogq;
  iopex -> prev_lu = &(lu -> inprogq);
  lu -> inprogq = iopex;
  if (iopex -> next_lu != NULL) iopex -> next_lu -> prev_lu = &(iopex -> next_lu);
  (*(iopex -> devex -> HwStartIo)) (iopex -> devex -> HwDeviceExtension, &(iopex -> Srb));
}

/************************************************************************/
/*									*/
/*  We're back in requestor's process space at softint level, finish	*/
/*									*/
/************************************************************************/

static void finishup (void *iopexv, int finok, uLong *status_r)

{
  Iopex *iopex;
  uLong sts;

  iopex = iopexv;
  if (iopex -> Srb.SrbExtension != NULL) OZ_KNL_NPPFREE (iopex -> Srb.SrbExtension);

  if (finok) {

    /* Maybe caller wants scsi status byte */

    if (iopex -> doiopp.status != NULL) {
      sts = oz_knl_section_uput (iopex -> procmode, sizeof *(iopex -> doiopp.status), 
                                 &(iopex -> Srb.ScsiStatus), iopex -> doiopp.status);
      if (sts != OZ_SUCCESS) {
        oz_knl_printk ("oz_dev_scsiport: error %u writing scsi status byte to %p\n", sts, iopex -> doiopp.status);
        if (*status_r == OZ_SUCCESS) *status_r = sts;
      }
    }

    /* Maybe caller wants actual data transfer length */

    if (iopex -> doiopp.datarlen != NULL) {
      sts = oz_knl_section_uput (iopex -> procmode, sizeof *(iopex -> doiopp.datarlen), 
                         &(iopex -> Srb.DataTransferLength), iopex -> doiopp.datarlen);
      if (sts != OZ_SUCCESS) {
        oz_knl_printk ("oz_dev_scsiport: error %u writing data length to %p\n", sts, iopex -> doiopp.datarlen);
        if (*status_r == OZ_SUCCESS) *status_r = sts;
      }
    }
  }
}

/************************************************************************/
/*									*/
/*  Read from I/O port routines						*/
/*									*/
/************************************************************************/

VOID ScsiPortReadPortBufferUchar (PUCHAR Port, PUCHAR Buffer, ULONG Count)

{
  OZ_HW_MB;
  if ((ULONG)Port < FAKE_IO_BASE_VA) {
    while (Count > 0) {
      *(Buffer ++) = *Port;
      -- Count;
    }
  } else {
    while (Count > 0) {
      *(Buffer ++) = oz_hw486_inb (((ULONG) Port) - FAKE_IO_BASE_VA);
      -- Count;
    }
  }
  OZ_HW_MB;
}

VOID ScsiPortReadPortBufferUlong (PULONG Port, PULONG Buffer, ULONG Count)

{
  OZ_HW_MB;
  if ((ULONG)Port < FAKE_IO_BASE_VA) {
    while (Count > 0) {
      *(Buffer ++) = *Port;
      -- Count;
    }
  } else {
    while (Count > 0) {
      *(Buffer ++) = oz_hw486_inl (((ULONG) Port) - FAKE_IO_BASE_VA);
      -- Count;
    }
  }
  OZ_HW_MB;
}

VOID ScsiPortReadPortBufferUshort (PUSHORT Port, PUSHORT Buffer, ULONG Count)

{
  OZ_HW_MB;
  if ((ULONG)Port < FAKE_IO_BASE_VA) {
    while (Count > 0) {
      *(Buffer ++) = *Port;
      -- Count;
    }
  } else {
    while (Count > 0) {
      *(Buffer ++) = oz_hw486_inw (((ULONG) Port) - FAKE_IO_BASE_VA);
      -- Count;
    }
  }
  OZ_HW_MB;
}

UCHAR ScsiPortReadPortUchar (PUCHAR Port)

{
  UCHAR Value;

  OZ_HW_MB;
  if ((ULONG)Port < FAKE_IO_BASE_VA) Value = *Port;
  else Value = oz_hw486_inb (((ULONG) Port) - FAKE_IO_BASE_VA);
  OZ_HW_MB;
  return (Value);
}

ULONG ScsiPortReadPortUlong (PULONG Port)

{
  ULONG Value;

  OZ_HW_MB;
  if ((ULONG)Port < FAKE_IO_BASE_VA) Value = *Port;
  else Value = oz_hw486_inl (((ULONG) Port) - FAKE_IO_BASE_VA);
  OZ_HW_MB;
  return (Value);
}

USHORT ScsiPortReadPortUshort (PUSHORT Port)

{
  USHORT Value;

  OZ_HW_MB;
  if ((ULONG)Port < FAKE_IO_BASE_VA) Value = *Port;
  else Value = oz_hw486_inw (((ULONG) Port) - FAKE_IO_BASE_VA);
  OZ_HW_MB;
  return (Value);
}

/************************************************************************/
/*									*/
/*  Sets bus-type-specific information for the specified bus and slot	*/
/*									*/
/*    Input:								*/
/*									*/
/*	DeviceExtension = points to HBA device extension data		*/
/*	BusDataType = PCIConfiguration					*/
/*	SystemIoBusNumber = specifies which of the busses it is connected to
/*	SlotNumber = specifies the slot to be accessed			*/
/*	Buffer = specifies where the data comes from			*/
/*	Offset = offset in config space to start writing at		*/
/*	Length = specifies the amount of data to be stored		*/
/*									*/
/*    Output:								*/
/*									*/
/*	ScsiPortSetBusDataByOffset = 0 : parameters not valid		*/
/*	                          else : number of bytes written	*/
/*									*/
/*	http://www.osr.com/ddk/k301_2yb6.htm				*/
/*									*/
/************************************************************************/

ULONG ScsiPortSetBusDataByOffset (PVOID DeviceExtension, 
                                  ULONG BusDataType, 
                                  ULONG SystemIoBusNumber, 
                                  ULONG SlotNumber, 
                                  PVOID Buffer, 
                                  ULONG Offset, 
                                  ULONG Length)

{
  OZ_Dev_pci_conf_p pciconfp;
  PCI_SLOT_NUMBER pcislotnumber;
  uLong i;

  /* We only do PCI bus */

  if (BusDataType != PCIConfiguration) return (0);

  /* Make sure PCIBIOS is present, if not, return 0 */

  if (!oz_dev_pci_present ()) return (0);

  /* Config space is at most 256 bytes int */

  if (Offset >= 256) return (0);
  if (Length + Offset > 256) Length = 256 - Offset;

  /* Set up an OZONE-style PCI address block */

  pcislotnumber.u.AsULONG = SlotNumber;
  memset (&pciconfp, 0, sizeof pciconfp);
  pciconfp.pcibus  = SystemIoBusNumber;
  pciconfp.pcidev  = pcislotnumber.u.bits.DeviceNumber;
  pciconfp.pcifunc = pcislotnumber.u.bits.FunctionNumber;

  /* Write config space from Buffer */

  i = Length;

  switch ((i | Offset) & 3) {
    case 0: {
      while (i > 3) {
        oz_dev_pci_conf_outl (*((uLong *)Buffer), &pciconfp, Offset);
        i -= 4;
        Offset += 4;
        (OZ_Pointer)Buffer += 4;
      }
      break;
    }
    case 2: {
      while (i > 1) {
        oz_dev_pci_conf_outw (*((uWord *)Buffer), &pciconfp, Offset);
        i -= 2;
        Offset += 2;
        (OZ_Pointer)Buffer += 2;
      }
      break;
    }
    case 1:
    case 3: {
      while (i > 0) {
        oz_dev_pci_conf_outb (*((uByte *)Buffer), &pciconfp, Offset);
        i --;
        Offset ++;
        (OZ_Pointer)Buffer ++;
      }
      break;
    }
  }

  return (Length);
}

/************************************************************************/
/*									*/
/*  Delay execution the indicated number of microseconds		*/
/*									*/
/************************************************************************/

VOID ScsiPortStallExecution (ULONG Delay)

{
  oz_hw_stl_microwait (Delay, NULL, NULL);
}

/************************************************************************/
/*									*/
/*  Write to I/O port routines						*/
/*									*/
/************************************************************************/

VOID ScsiPortWritePortBufferUchar (PUCHAR Port, PUCHAR Buffer, ULONG Count)

{
  OZ_HW_MB;
  if ((ULONG)Port < FAKE_IO_BASE_VA) {
    while (Count > 0) {
      *Port = *(Buffer ++);
      -- Count;
    }
  } else {
    while (Count > 0) {
      oz_hw486_outb (*(Buffer ++), ((ULONG) Port) - FAKE_IO_BASE_VA);
      -- Count;
    }
  }
  OZ_HW_MB;
}

VOID ScsiPortWritePortBufferUlong (PULONG Port, PULONG Buffer, ULONG Count)

{
  OZ_HW_MB;
  if ((ULONG)Port < FAKE_IO_BASE_VA) {
    while (Count > 0) {
      *Port = *(Buffer ++);
      -- Count;
    }
  } else {
    while (Count > 0) {
      oz_hw486_outl (*(Buffer ++), ((ULONG) Port) - FAKE_IO_BASE_VA);
      -- Count;
    }
  }
  OZ_HW_MB;
}

VOID ScsiPortWritePortBufferUshort (PUSHORT Port, PUSHORT Buffer, ULONG Count)

{
  OZ_HW_MB;
  if ((ULONG)Port < FAKE_IO_BASE_VA) {
    while (Count > 0) {
      *Port = *(Buffer ++);
      -- Count;
    }
  } else {
    while (Count > 0) {
      oz_hw486_outw (*(Buffer ++), ((ULONG) Port) - FAKE_IO_BASE_VA);
      -- Count;
    }
  }
  OZ_HW_MB;
}

VOID ScsiPortWritePortUchar (PUCHAR Port, UCHAR Value)

{
  OZ_HW_MB;
  if ((ULONG)Port < FAKE_IO_BASE_VA) *Port = Value;
  else oz_hw486_outb (Value, ((ULONG) Port) - FAKE_IO_BASE_VA);
  OZ_HW_MB;
}

VOID ScsiPortWritePortUlong (PULONG Port, ULONG Value)

{
  OZ_HW_MB;
  if ((ULONG)Port < FAKE_IO_BASE_VA) *Port = Value;
  else oz_hw486_outl (Value, ((ULONG) Port) - FAKE_IO_BASE_VA);
  OZ_HW_MB;
}

VOID ScsiPortWritePortUshort (PUSHORT Port, USHORT Value)

{
  OZ_HW_MB;
  if ((ULONG)Port < FAKE_IO_BASE_VA) *Port = Value;
  else oz_hw486_outw (Value, ((ULONG) Port) - FAKE_IO_BASE_VA);
  OZ_HW_MB;
}
