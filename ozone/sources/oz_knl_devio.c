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
/*  Device and i/o driver support routines				*/
/*									*/
/*  Drivers should be called oz_dev_<drivername>.c			*/
/*									*/
/*  The file oz_dev_inits.c should contain a call to <drivername>_init 	*/
/*  that will be called at boot time (at softint level) to initialize 	*/
/*  the driver.								*/
/*									*/
/*  Each <drivername>_init routine might be called more than once.  	*/
/*  Each time, it should make attempts to locate more of its devices, 	*/
/*  as other drivers it depends on may have declared some dependent 	*/
/*  resources.								*/
/*									*/
/*  For example, a scsi disk driver is dependent on scsi controller 	*/
/*  drivers.  So whenever a scsi controller comes online, the scsi 	*/
/*  disk driver should scan the scsi bus to see if there are any disk 	*/
/*  drives there for it to process.					*/
/*									*/
/*  The <drivername>_init routine should call:				*/
/*									*/
/*	devclass = oz_dev_devclass_create ("<classname>");		*/
/*	devdriver = oz_dev_devdriver_create (devclass, 			*/
/*	                                     "<drivername>");		*/
/*									*/
/*  Then for every unit it processes, call:				*/
/*									*/
/*	devunit = oz_dev_devunit_create (devdriver, 			*/
/*	                                 "<unitdesc>", 			*/
/*	                                 functable); 			*/
/*	ex = oz_dev_devunit_ex (devunit);				*/
/*									*/
/*  <classname> : defines the basic functions available			*/
/*									*/
/*	 'scsi' : used by all scsi controllers				*/
/*	'ether' : used by all ethernet controllers			*/
/*	 'disk' : used by all disk drivers				*/
/*	   'fs' : used by all file system drivers			*/
/*									*/
/*  <drivername> : simply identifies the driver by a name		*/
/*									*/
/*	for hardware controllers (like scsi or ether class), it can be 	*/
/*	the name of the controller (like 'adaptec 2940' or 'smc pci')	*/
/*									*/
/*	for things like file systems, it can be 'ext2', 'msdos', etc	*/
/*									*/
/*  <unitname> : specifies the name of the unit suitable for passing 	*/
/*               to oz_knl_iochan_create.  End users will see this 	*/
/*               string.  Comparisons and searches are 			*/
/*               case-insensitive.					*/
/*									*/
/*	Allowed chars are a-z, A-Z, 0-9, _, -, . and @.  The name 	*/
/*	should be based as much as possible on some physical attribute 	*/
/*	of the device.  For instance, an scsi controller in pci slot 	*/
/*	zero might call itself scsi_pci0.  Now the . should be 		*/
/*	reserved for indicating hierarchy (sort of like internet host 	*/
/*	names).  For example, if a disk is scsi id 5 of scsi 		*/
/*	controller in pci slot 0, it might be called scsi_pci0.disk5.  	*/
/*	If the disk has 4 partitions, the device representing partion 	*/
/*	2 might be named scsi_pci0.disk5.part2.				*/
/*									*/
/*	The dot is also used by the 'autogen' system.  Say, for 	*/
/*	instance, an user calls for device ide_pm.3.  This says they 	*/
/*	want partition '3' of the 'ide_pm' disk.  Now suppose that the 	*/
/*	partition table hasn't been scanned yet.  So all that will 	*/
/*	exist in the system is device ide_pm.  So the autogen stuff 	*/
/*	will say, ok, there is no ide_pm.3 but there is an ide_pm 	*/
/*	device and it has declared an autogen routine.  So call 	*/
/*	ide_pm's autogen routine and see if it is able to come up 	*/
/*	with .3.  So ide_pm's autogen routine knows that it can come 	*/
/*	up with partitions and it sees the .3 and knows it refers 	*/
/*	to partition three.  So ide_pm's autogen routine scans 		*/
/*	ide_pm's parition table and gets the definition for partition 	*/
/*	3.  If it is successful, it creates device ide_pm.3, and the 	*/
/*	user now has access.  The autogen mechanism can also be used 	*/
/*	to automatically 'mount' a volume on a filesystem.		*/
/*									*/
/*	Unit names must not begin with an _, as the system creates 	*/
/*	'alias names' for the devices.  The alias name consists of an 	*/
/*	underscore, followed by the class name, followed by an unique 	*/
/*	sequentially-assigned integer.  So, for instance, file systems 	*/
/*	(class fs) will have aliases of _fs1, _fs2, etc.  Ethernets 	*/
/*	will have aliases of _ether1, _ether2, etc.  These alias names 	*/
/*	can be used wherever a normal device name can be used to refer 	*/
/*	to a device.							*/
/*									*/
/*  <unitdesc> : describes the unit in a human readable form		*/
/*									*/
/*	the string is not intended to be parsed by any routine, just 	*/
/*	printed out							*/
/*									*/
/*	for scsi controllers, it could be like 'scsi in pci slot 0'	*/
/*	for disk controllers, it could be like 'lun 5 of scsi1'		*/
/*	for file systems, it could be 'volume abc on disk5'		*/
/*									*/
/*  All drivers of the same class should have identical functions.  	*/
/*  For example, an Intel ethernet driver should have functions 	*/
/*  identical to an SMC ethernet driver.  An IDE disk driver should 	*/
/*  have functions identical to an SCSI disk driver.  An ext2 file 	*/
/*  system driver should have functions identical to an msdos file 	*/
/*  system driver.							*/
/*									*/
/*  Some groups of classes can be thought of as having an hierarchy.  	*/
/*  For example, file system drivers are the highest in the hierarchy, 	*/
/*  followed by disk drivers then scsi drivers.  Another example, nfs, 	*/
/*  then tcp, then ip, then ethernet.					*/
/*									*/
/*  The 'devex' area's use is defined by the driver itself.  The 	*/
/*  driver specifies the size of this area when it calls 		*/
/*  oz_knl_devunit_create.  This area becomes part of the device unit 	*/
/*  definition and can be used by the driver for whatever it wants 	*/
/*  until the device unit is deleted.  Typical use for disk drives 	*/
/*  could be number of blocks, what devunit has the scsi controller, 	*/
/*  what its logical unit number on that controller is, etc.		*/
/*									*/
/*  Likewise, the 'iopex' area's use is also defined by the 		*/
/*  driver.  The driver specifies the size to allocate to this area 	*/
/*  when it calls oz_knl_devunit_create.  This area is tacked on the 	*/
/*  end of the ioop and can be used by the driver for whatever it 	*/
/*  wants until it calls the oz_knl_iodone routine for that ioop 	*/
/*  packet.  Typical use for a read function could be the block number 	*/
/*  to start reading at, the address and size of the buffer, etc.	*/
/*									*/
/*  The driver must define the maximum possible size needed for the 	*/
/*  iopex area for all possible function codes, as it is not 		*/
/*  possible to vary the size of the ex area based on function 		*/
/*  code.								*/
/*									*/
/************************************************************************/

#define _OZ_KNL_DEVIO_C

#include "ozone.h"

#include "oz_knl_ast.h"
#include "oz_knl_devio.h"
#include "oz_knl_event.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_lock.h"
#include "oz_knl_lowipl.h"
#include "oz_knl_objtype.h"
#include "oz_knl_printk.h"
#include "oz_knl_process.h"
#include "oz_knl_sdata.h"
#include "oz_knl_status.h"
#include "oz_knl_thread.h"
#include "oz_knl_userjob.h"

#define MAXIOOPLOCKS (16)

#define LEGALUNITNAMECHAR(c) (((c >= '0') && (c <= '9')) || ((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) || (c == '_') || (c == '-') || (c == '.') || (c == '@'))

	/* These are the I/O channels. */

struct OZ_Iochan { OZ_Objtype objtype;				/* object type OZ_OBJTYPE_IOCHAN */
                   OZ_Iochan *next;				/* links for devunit -> iochans */
                   OZ_Iochan **prev;
                   OZ_Devunit *devunit;				/* device unit assigned to this i/o channel */
                   OZ_Procmode procmode;			/* processor mode associated with channel */
                   OZ_Lockmode lockmode;			/* lock mode associated with channel */
                   OZ_Threadid lastiotid;			/* thread-id of last I/O queued */
                   volatile Long refcount;			/* reference count (ioop's, iosel's, general ref count) */
                   int calldeas;				/* 0 : assign failed, don't call deassign routine */
								/* 1 : assign succeeded, call deassign routine */
                   OZ_Secattr *secattr;				/* who can access me */
                   volatile uLong readcount, writecount;	/* number of read I/O's started, write I/O's started */
                   uByte chnex[1];				/* channel ex area (for use by the device driver) */
                 };

	/* There is one of these for each device unit activated by a device driver */

struct OZ_Devunit { OZ_Objtype objtype;				/* object type OZ_OBJTYPE_DEVUNIT */
                    OZ_Devunit *next;				/* next in devdriver -> devunits list */
                    OZ_Devdriver *devdriver;			/* pointer to devdriver struct */
                    uLong aliasno;				/* alias number */
                    void *alloc_obj;				/* NULL: available */
								/* else: allocated to this user, job, process, thread */
                    OZ_Devunit  *alloc_nxt;			/* next devunit allocated to the user/job/process/thread */
                    OZ_Devunit **alloc_prv;			/* prev devunit allocated to the user/job/process/thread */
                    OZ_Iochan *iochans;				/* list of channels assigned to devunit */
                    OZ_Secattr *secattr;			/* pointer to security attributes struct */
                    Long refcount;				/* reference count (number of assigned channels) */
                    Long refc_read, refc_write;			/* channel lock mode ref counts */
                    Long deny_read, deny_write;
                    OZ_Devunit *(*auto_entry) (void *auto_param, /* autogen routine entrypoint */
                                               OZ_Devunit *host_devunit, 
                                               const char *devname, 
                                               const char *suffix);
                    void *auto_param;				/* autogen routine parameter */
                    int unitname_l;				/* strlen (unitname) */
                    char unitname[OZ_DEVUNIT_NAMESIZE];		/* unit name (for passing to oz_knl_iochan_create) */
                    char unitdesc[OZ_DEVUNIT_DESCSIZE];		/* unit description (like "IDE PRIMARY MASTER DISK") */
                    int cloned;					/* created via cloning flag */
                    volatile uLong opcount;			/* incremented for each iostart and ioselect attempted */
                    Long ioopendcount;				/* count of ioop's pending */
                    const OZ_Devfunc *functable;		/* pointer to function table */
                    uByte devex[1];				/* devex area (for use by the device driver) */
                  };

	/* There is one of these per device driver */

struct OZ_Devdriver { OZ_Objtype objtype;			/* object type OZ_OBJTYPE_DEVDRIVER */
                      struct OZ_Devdriver *next;		/* next in devclass -> devdrivers list */
                      struct OZ_Devclass *devclass;		/* pointer to devclass struct */
                      char drivername[OZ_DEVDRIVER_NAMESIZE];	/* driver name (like "SMC_ETHER") */
                      OZ_Devunit *devunits;			/* list of device units that this devdriver handles */
                    };

	/* There is one of these per device class */

struct OZ_Devclass { OZ_Objtype objtype;			/* object type OZ_OBJTYPE_DEVCLASS */
                     OZ_Devclass *next;				/* next in devclasses list */
                     char classname[OZ_DEVCLASS_NAMESIZE];	/* device class name (like "SCSI") */
                     uLong aliasno;				/* last alias number assigned */
                     OZ_Devdriver *devdrivers;			/* list of devdrivers for this class */
                   };

	/* There is one of these per I/O operation started with the iostart routine */

struct OZ_Ioop { OZ_Objtype objtype;				/* object type OZ_OBJTYPE_IOOP */
                 volatile Long refcount;			/* reference count (ioop gets freed when this goes zero) */
                 volatile Long state;				/* state (IOOP_STATE_...) */
                 OZ_Iochan *iochan;				/* I/O channel pointer used for the i/o */
                 OZ_Procmode procmode;				/* processor mode associated with i/o operation */
                 OZ_Thread *thread;				/* issuing thread */
                 OZ_Process *process;				/* process that was mapped when request was issued */
                 OZ_Ioop *tq_next;				/* next in thread -> ioopq */
                 OZ_Ioop **tq_prev;				/* previous ioop's tq_next */
                 OZ_Event *abortevent;				/* abort event flag pointer */
								/* NULL if rundown not yet called */
                 void (*iopostent) (void *iopostpar, uLong status); /* post-processing entrypoint */
                 void *iopostpar;				/* post-processing parameter */
                 volatile uLong *status_r;			/* completion status location */
                 OZ_Event *event;				/* event flag to set when complete */
                 OZ_Astentry astentry;				/* ast routine entrypoint */
                 void *astparam;				/* ast parameter */
                 OZ_Ioop *donenext;				/* next on iodonehiqh/t */
                 uLong status;					/* completion status */
                 void (*finentry) (void *finparam, int finok, uLong *status_r);
                 void *finparam;
                 Long nseclocks;				/* number of entries used in seclocks table */
                 OZ_Seclock *seclocks[MAXIOOPLOCKS];		/* list of buffers locked by driver's start routine */
                 uByte iopex[1];				/* iopex area (for use by the device driver) */
               };

	// Ioop states valid transitions:

	//	INITING -> (ioop gets completely filled in and all ref counts incremented, etc) -> STARTED
	//	STARTED -> (driver start routine returns <> OZ_STARTED) -> SYNCMPL
	//	        -> (driver start routine calls iodone and returns OZ_STARTED) -> PSYNCMP
	//	        -> (driver start routine returns OZ_STARTED but caller wants oz_knl_iostart to wait) -> WAITING
	//	        -> (driver start routine returns OZ_STARTED and caller doesn't want to wait) -> INPROGR
	//	SYNCMPL -> terminal, synchronous completion status returned to caller
	//	INPROGR -> (interrupt routine calls iodone) -> ASYNCMP
	//	WAITING -> (interrupt routine calls iodone) -> PSYNCMP
	//	PSYNCMP -> terminal, synchronous completion status returned to caller
	//	ASYNCMP -> terminal, completion status stored in status block, event flag set, ast routine called

#define IOOP_STATE_INITING 0	// initializing the ioop struct
#define IOOP_STATE_STARTED 1	// driver's start routine has been called
#define IOOP_STATE_SYNCMPL 2	// synchronous completion, driver returned <> OZ_STARTED
#define IOOP_STATE_INPROGR 3	// driver returned OZ_STARTED and oz_knl_iostart returned OZ_STARTED to caller
#define IOOP_STATE_WAITING 4	// driver returned OZ_STARTED and oz_knl_iostart is waiting for completion
#define IOOP_STATE_PSYNCMP 5	// pseudo-synchronous completion, driver start routine called iodone then returned OZ_STARTED
#define IOOP_STATE_ASYNCMP 6	// asynchronous completion, driver returned OZ_STARTED, then later called iodone

	/* One of these per ioselect routine request */

struct OZ_Ioselect { OZ_Objtype objtype;			/* object type OZ_OBJTYPE_IOSELECT */
                     OZ_Ioselect *nexthi;			/* next on ioseldoneq */
                     int sysio;					/* 0: can be in process address space */
								/* 1: all params in system addr space */
                     OZ_Thread *thread;				/* requesting thread */
                     OZ_Process *process;			/* requesting process */
                     uLong numchans;				/* number of channels being scanned */
                     OZ_Procmode procmode;			/* processor mode of requestor */
                     OZ_Iochan **iochans;			/* I/O channels being selected on */
                     uLong *ioselcodes;				/* what is being selected for */
                     uLong *senses;				/* pointer to return array */
                     Long state;
                     Long pending;				/* number of selects actually started - those that have finished */
                     Long hicount;				/* number of times ioseldonehi was called */
                     void (*selpostent) (void *selpostpar, 	/* select post-processing routine entrypoint */
                                         uLong numchans, 
                                         OZ_Iochan **iochans, 
                                         uLong *ioselcodes, 
                                         uLong *senses, 
                                         OZ_Procmode procmode);
                     void *selpostpar;				/* select post-processing routine parameter */
                     uLong active[1];				/* bit array of 'active' selects, followed by selex's */
                   };

#define IOSEL_STATE_STARTUP 1		// in startup loop, nothing finished yet
#define IOSEL_STATE_ASYNINSTART 2	// in startup loop, something finished async'ly
#define IOSEL_STATE_INPROGRESS 3	// out of startup loop, nothing finished yet
#define IOSEL_STATE_FINISHED 4		// something has finished

	/* Static data */

static int devdbchange;			/* set when a device class, devdriver, unit is created */
static OZ_Devclass *devclasses;		/* list of all device classes */
static OZ_Ioop *iodonehiqh;		/* list of ioop's queued from iodonehi routine */
static OZ_Ioop **iodonehiqt;
static OZ_Ioselect *ioseldoneqh;	/* list of ioselect's queued from ioseldonehi routine */
static OZ_Ioselect **ioseldoneqt;
static OZ_Lowipl *iodonehi_lowipl;	/* used in oz_knl_iodonehi routine */
static OZ_Lowipl *iosellowipl;		/* used in oz_knl_ioseldonehi routine */
static uLong devunit_count;		/* total # of devunits in system */

	/* Internal routines */

static OZ_Devunit **getdevalloc (void *alloc_obj);
static uLong checkalloc (OZ_Devunit *devunit);
static uLong assign_channel (OZ_Devunit *devunit, OZ_Lockmode cklockmode, OZ_Lockmode asnlockmode, OZ_Procmode procmode, OZ_Secattr *secattr, uLong dv, OZ_Iochan **iochan_r);
static void iodonehi_proc (void *dummy, OZ_Lowipl *lowipl);
static void iodone (OZ_Ioop *ioop);
static void ioseldonelo (void *dummy, OZ_Lowipl *lowipl);
static int ioseldone (OZ_Ioselect *ioselect, int fin);

/************************************************************************/
/*									*/
/*  This routine is called at boot time to initialize the devices	*/
/*									*/
/*  Maybe have this create a kernel thread which loops every once-in-	*/
/*  a-while to scan for new devices that just came online.		*/
/*									*/
/*    Input:								*/
/*									*/
/*	smplock = softint						*/
/*									*/
/************************************************************************/

void oz_knl_devinit (void)

{
  devclasses      = NULL;	/* start with nothing */
  devunit_count   = 0;
  iodonehi_lowipl = oz_knl_lowipl_alloc ();
  iodonehiqh      = NULL;
  iodonehiqt      = &iodonehiqh;
  iosellowipl     = oz_knl_lowipl_alloc ();
  ioseldoneqh     = NULL;
  ioseldoneqt     = &ioseldoneqh;
  oz_dev_timer_init ();		/* initialize the timer driver */

  do {
    devdbchange = 0;		/* say nothing has changed */
    oz_dev_inits ();		/* call all driver initialization routines */
  } while (devdbchange);	/* repeat if there was a change */
}

/************************************************************************/
/*									*/
/*  Shutdown routine to neutralize all drivers				*/
/*									*/
/*    Input:								*/
/*									*/
/*	smp level = softint						*/
/*	all other cpus halted						*/
/*									*/
/*    Output:								*/
/*									*/
/*	all drivers shut down (may have waited)				*/
/*									*/
/************************************************************************/

void oz_knl_devshut (void)

{
  int repeat;
  int (*shutdown) (OZ_Devunit *devunit, void *devexv);
  const OZ_Devfunc *devfunc;
  OZ_Devunit *devunit, *lastdevunit;

  do {
    repeat = 0;
    for (lastdevunit = NULL; (devunit = oz_knl_devunit_getnext (lastdevunit)) != NULL; lastdevunit = devunit) {
      if (lastdevunit != NULL) oz_knl_devunit_increfc (lastdevunit, -1);
      devfunc = devunit -> functable;							/* get pointer to function table */
      if (devfunc == NULL) continue;							/* skip if shutdown previously succeeded */
      shutdown = devfunc -> shutdown;							/* get pointer to shutdown routine */
      if (shutdown == NULL) devunit -> functable = NULL;				/* if none, assume it is shutdown then */
      else if ((*shutdown) (devunit, devunit -> devex)) devunit -> functable = NULL;	/* else, call shutdown routine */
      else repeat = 1;									/* didn't shutdown yet, repeat the loop */
    }
    if (lastdevunit != NULL) oz_knl_devunit_increfc (lastdevunit, -1);
  } while (repeat);
}

/************************************************************************/
/*									*/
/*  This routine is called by a driver's initialization routine to 	*/
/*  create or find its devclass struct					*/
/*									*/
/*    Input:								*/
/*									*/
/*	classname  = null terminated device class name string		*/
/*	funcodbase = deprecated						*/
/*	funcodmask = deprecated						*/
/*									*/
/*	smplock <= dv							*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_devclass_create = NULL : class name string invalid	*/
/*	                         else : pointer to devclass struct	*/
/*									*/
/************************************************************************/

OZ_Devclass *oz_knl_devclass_create (const char *classname, uLong funcodbase, uLong funcodmask, const char *drivername)

{
  char c;
  int i;
  uLong dv;
  OZ_Devclass *devclass, **ldevclass, *xdevclass;

  dv = oz_hw_smplock_wait (&oz_s_smplock_dv);

  /* If it already exists, just return pointer to it */

  for (devclass = devclasses; devclass != NULL; devclass = devclass -> next) {
    if (strcmp (classname, devclass -> classname) == 0) {
      oz_hw_smplock_clr (&oz_s_smplock_dv, dv);
      return (devclass);
    }
  }

  /* Not there, make sure the class name is legal -    */
  /* We allow only alphabetics and simple separators   */
  /* so people don't make crazy names that may be hard */
  /* for parsers to handle.  We do not allow numerics  */
  /* because they are used to represent unit numbers.  */

  for (i = 0; (c = classname[i]) != 0; i ++) {
    if ((c >= 'a') && (c <= 'z')) continue;
    if ((c >= 'A') && (c <= 'Z')) continue;
    if ((c == '_') || (c == '-')) continue;
    oz_crash ("oz_knl_devclass_create: device class name %s contains invalid character (only a-z, A-Z, _, - allowed)\n", classname);
  }
  if (i >= sizeof devclass -> classname) {
    oz_crash ("oz_knl_devclass_create: device class name %s is longer than %d characters\n", classname, sizeof devclass -> classname - 1);
  }

  /* Create a new devclass structure */

  devclass = OZ_KNL_NPPMALLOC (sizeof *devclass);
  devclass -> objtype = OZ_OBJTYPE_DEVCLASS;
  strncpyz (devclass -> classname, classname, sizeof devclass -> classname);
  devclass -> devdrivers = NULL;
  devclass -> aliasno    = 0;

  /* Insert in list in alphabetical order */

  for (ldevclass = &devclasses; (xdevclass = *ldevclass) != NULL; ldevclass = &(xdevclass -> next)) {
    if (strcmp (xdevclass -> classname, devclass -> classname) > 0) break;
  }
  *ldevclass = devclass;
  devclass -> next = xdevclass;

  devdbchange = 1;

  oz_hw_smplock_clr (&oz_s_smplock_dv, dv);
  return (devclass);
}

/************************************************************************/
/*									*/
/*  Create a device driver entry					*/
/*									*/
/*    Input:								*/
/*									*/
/*	devclass = device class that driver belongs to			*/
/*	drivername = name of the device driver to create		*/
/*									*/
/*	smplock <= dv							*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_devdriver_create = NULL : invalid drivername		*/
/*	                          else : pointer to devdriver struct	*/
/*									*/
/************************************************************************/

OZ_Devdriver *oz_knl_devdriver_create (OZ_Devclass *devclass, const char *drivername)

{
  char c;
  int i;
  uLong dv;
  OZ_Devdriver *devdriver, **ldevdriver, *xdevdriver;

  OZ_KNL_CHKOBJTYPE (devclass, OZ_OBJTYPE_DEVCLASS);

  dv = oz_hw_smplock_wait (&oz_s_smplock_dv);

  /* If it already exists, just return pointer to it */

  for (devdriver = devclass -> devdrivers; devdriver != NULL; devdriver = devdriver -> next) {
    if (strcmp (drivername, devdriver -> drivername) == 0) {
      oz_hw_smplock_clr (&oz_s_smplock_dv, dv);
      return (devdriver);
    }
  }

  /* Not there, make sure the name is legal -          */
  /* We allow only alphnumerics and simple separators  */
  /* so people don't make crazy names that may be hard */
  /* for parsers to handle.                            */

  for (i = 0; (c = drivername[i]) != 0; i ++) {
    if ((c >= 'a') && (c <= 'z')) continue;
    if ((c >= 'A') && (c <= 'Z')) continue;
    if ((c == '_') || (c == '-')) continue;
    if ((c >= '0') && (c <= '9')) continue;
    oz_crash ("oz_knl_devdriver_create: device driver name %s contains invalid character (only a-z, A-Z, _, - allowed)\n", drivername);
  }
  if (i >= sizeof devdriver -> drivername) {
    oz_crash ("oz_knl_devdriver_create: device driver name %s is longer than %d characters\n", drivername, sizeof devdriver -> drivername - 1);
  }

  /* Create a new devdriver structure */

  devdriver = OZ_KNL_NPPMALLOC (sizeof *devdriver);
  devdriver -> objtype    = OZ_OBJTYPE_DEVDRIVER;
  strncpyz (devdriver -> drivername, drivername, sizeof devdriver -> drivername);
  devdriver -> devclass   = devclass;
  devdriver -> devunits   = NULL;

  /* Insert in list in alphabetical order */

  for (ldevdriver = &(devclass -> devdrivers); (xdevdriver = *ldevdriver) != NULL; ldevdriver = &(xdevdriver -> next)) {
    if (strcmp (xdevdriver -> drivername, devdriver -> drivername) > 0) break;
  }
  *ldevdriver = devdriver;
  devdriver -> next = xdevdriver;

  devdbchange = 1;

  oz_hw_smplock_clr (&oz_s_smplock_dv, dv);
  return (devdriver);
}

/************************************************************************/
/*									*/
/*  Create device unit struct						*/
/*									*/
/*    Input:								*/
/*									*/
/*	devdriver = device driver that processes this unit		*/
/*	unitname  = name to use for 'iochan_create'			*/
/*	unitdesc  = descriptive string saying what unit is		*/
/*	functable = driver entrypoint table				*/
/*	cloned    = 0 : this is not a cloned device			*/
/*	            1 : this is a cloned device				*/
/*	secattr   = security attributes for device			*/
/*									*/
/*	smplock <= dv							*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_devunit_create = pointer to devunit struct		*/
/*	                        NULL if naming failure			*/
/*									*/
/************************************************************************/

OZ_Devunit *oz_knl_devunit_create (OZ_Devdriver *devdriver, const char *unitname, const char *unitdesc, const OZ_Devfunc *functable, int cloned, OZ_Secattr *secattr)

{
  uLong dv;
  OZ_Devunit *devunit;

  OZ_KNL_CHKOBJTYPE (devdriver, OZ_OBJTYPE_DEVDRIVER);

  /* Create a new devunit structure */

  devunit = OZ_KNL_NPPMALLOQ (functable -> dev_exsize + sizeof *devunit);
  if (devunit == NULL) return (NULL);
  memset (devunit, 0, functable -> dev_exsize + sizeof *devunit);
  devunit -> objtype   = OZ_OBJTYPE_DEVUNIT;
  devunit -> iochans   = NULL;
  devunit -> alloc_obj = NULL;
  devunit -> devdriver = devdriver;
  devunit -> functable = functable;
  devunit -> refcount  = 1;
  devunit -> cloned    = cloned;
  devunit -> secattr   = secattr;
  if (secattr != NULL) oz_knl_secattr_increfc (secattr, 1);

  /* Insert in driver's list of units using null name and descriptor */

  dv = oz_hw_smplock_wait (&oz_s_smplock_dv);
  devunit -> next = devdriver -> devunits;
  devdriver -> devunits = devunit;

  devdbchange = 1;

  /* Rename it to the name the caller wants and assign it an unique alias number */

  if (oz_knl_devunit_rename (devunit, unitname, unitdesc)) {
    devunit_count ++;
    devunit -> aliasno = ++ (devdriver -> devclass -> aliasno);
    oz_hw_smplock_clr (&oz_s_smplock_dv, dv);
    return (devunit);
  }

  /* If it fails to rename, get rid of it */

  if (secattr != NULL) oz_knl_secattr_increfc (secattr, -1);
  if (devdriver -> devunits != devunit) oz_crash ("oz_knl_devunit_create: devdriver->devunits got rearranged");
  if (devunit -> refcount != 1) oz_crash ("oz_knl_devunit_create: refcount %d", devunit -> refcount);
  devdriver -> devunits = devunit -> next;
  OZ_KNL_NPPFREE (devunit);

  oz_hw_smplock_clr (&oz_s_smplock_dv, dv);
  return (NULL);  
}

/************************************************************************/
/*									*/
/*  Rename device unit							*/
/*									*/
/*    Input:								*/
/*									*/
/*	devunit  = device to be renamed					*/
/*	unitname = new name for the unit				*/
/*	           (NULL not to change it)				*/
/*	unitdesc = new description for the unit				*/
/*	           (NULL not to change it)				*/
/*									*/
/*	smplock <= dv							*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_devunit_rename = 0 : failed to rename			*/
/*	                        1 : unit renamed			*/
/*									*/
/************************************************************************/

int oz_knl_devunit_rename (OZ_Devunit *devunit, const char *unitname, const char *unitdesc)

{
  char c;
  int i;
  uLong dv;
  OZ_Devclass *xdevclass;
  OZ_Devdriver *xdevdriver;
  OZ_Devunit **ldevunit, *xdevunit;

  OZ_KNL_CHKOBJTYPE (devunit, OZ_OBJTYPE_DEVUNIT);

  dv = oz_hw_smplock_wait (&oz_s_smplock_dv);

  /* Make sure name isn't already in use */

  if (unitname != NULL) {
    for (xdevclass = devclasses; xdevclass != NULL; xdevclass = xdevclass -> next) {
      for (xdevdriver = xdevclass -> devdrivers; xdevdriver != NULL; xdevdriver = xdevdriver -> next) {
        for (xdevunit = xdevdriver -> devunits; xdevunit != NULL; xdevunit = xdevunit -> next) {
          if (xdevunit == devunit) continue;
          if (strcasecmp (xdevunit -> unitname, unitname) == 0) {
            oz_knl_printk ("oz_knl_devunit_create: device unit name %s (being created by driver %s) already exists\n", unitname, devunit -> devdriver -> drivername);
            oz_hw_smplock_clr (&oz_s_smplock_dv, dv);
            return (0);
          }
        }
      }
    }
  }

  /* Make sure device unit name consists of only numbers, letters, _, -, ., @'s */

  if (unitname != NULL) {
    for (i = 0; (c = unitname[i]) != 0; i ++) {
      if (LEGALUNITNAMECHAR (c)) continue;
      oz_knl_printk ("oz_knl_devunit_rename: device unit name %s contains invalid character (only 0-9, a-z, A-Z, _, -, ., @ allowed)\n", unitname);
      oz_hw_smplock_clr (&oz_s_smplock_dv, dv);
      return (0);
    }
    if (i >= sizeof devunit -> unitname) {
      oz_knl_printk ("oz_knl_devunit_rename: device unit name %s is longer than %d characters\n", unitname, sizeof devunit -> unitname - 1);
      oz_hw_smplock_clr (&oz_s_smplock_dv, dv);
      return (0);
    }
  }

  /* Make sure the description legal -                 */
  /* We allow only alphabetics and simple separators   */
  /* so people don't make crazy names that may be hard */
  /* for parsers to handle.                            */

  if (unitdesc != NULL) {
    for (i = 0; (c = unitdesc[i]) != 0; i ++) {
      if ((c < ' ') || (c >= 127)) {
        oz_knl_printk ("oz_knl_devunit_rename: device unit description %s contains invalid character (only printables allowed)\n", unitdesc);
        oz_hw_smplock_clr (&oz_s_smplock_dv, dv);
        return (0);
      }
    }
    if (i >= sizeof devunit -> unitdesc) {
      oz_knl_printk ("oz_knl_devunit_rename: device unit description %s is longer than %d characters\n", unitdesc, sizeof devunit -> unitdesc - 1);
      oz_hw_smplock_clr (&oz_s_smplock_dv, dv);
      return (0);
    }
  }

  /* Remove from device unit list */

  if (unitname != NULL) {
    for (ldevunit = &(devunit -> devdriver -> devunits); (xdevunit = *ldevunit) != devunit; ldevunit = &(xdevunit -> next)) {
      if (xdevunit == NULL) oz_crash ("oz_knl_devunit_rename: cannot find devunit in device list");
    }
    *ldevunit = devunit -> next;
  }

  /* Store new name and description in struct */

  if (unitname != NULL) {
    strncpyz (devunit -> unitname, unitname, sizeof devunit -> unitname);
    devunit -> unitname_l = strlen (devunit -> unitname);
  }
  if (unitdesc != NULL) strncpyz (devunit -> unitdesc, unitdesc, sizeof devunit -> unitdesc);

  /* Re-insert in alphabetical order by unit name */

  if (unitname != NULL) {
    for (ldevunit = &(devunit -> devdriver -> devunits); (xdevunit = *ldevunit) != NULL; ldevunit = &(xdevunit -> next)) {
      if (strcmp (xdevunit -> unitname, devunit -> unitname) > 0) break;
    }
    *ldevunit = devunit;
    devunit -> next = xdevunit;
  }

  oz_hw_smplock_clr (&oz_s_smplock_dv, dv);
  return (1);
}

/************************************************************************/
/*									*/
/*  Return pointer to device unit's ex area				*/
/*									*/
/************************************************************************/

void *oz_knl_devunit_ex (OZ_Devunit *devunit)

{
  OZ_KNL_CHKOBJTYPE (devunit, OZ_OBJTYPE_DEVUNIT);
  return (devunit -> devex);
}

/************************************************************************/
/*									*/
/*  Set up autogen routine entrypoint and parameter			*/
/*									*/
/*  This routine is called back when a channel assignment is made to a 	*/
/*  non-existant device, in the hopes that the autogen routine can 	*/
/*  create the device.							*/
/*									*/
/*    Input:								*/
/*									*/
/*	devunit    = device unit the autogen routine applies to		*/
/*	auto_entry = entrypoint to be called				*/
/*	auto_param = parameter to pass to it				*/
/*									*/
/************************************************************************/

void oz_knl_devunit_autogen (OZ_Devunit *host_devunit, 
                             OZ_Devunit *(*auto_entry) (void *auto_param, 
                                                        OZ_Devunit *host_devunit, 
                                                        const char *devname, 
                                                        const char *suffix), 
                             void *auto_param)

{
  OZ_KNL_CHKOBJTYPE (host_devunit, OZ_OBJTYPE_DEVUNIT);

  host_devunit -> auto_entry = auto_entry;
  host_devunit -> auto_param = auto_param;
}

/************************************************************************/
/*									*/
/*  Set device unit's security attributes				*/
/*									*/
/************************************************************************/

void oz_knl_devunit_setsecattr (OZ_Devunit *devunit, OZ_Secattr *newsecattr)

{
  OZ_Secattr *oldsecattr;
  uLong se;

  OZ_KNL_CHKOBJTYPE (devunit, OZ_OBJTYPE_DEVUNIT);
  OZ_KNL_CHKOBJTYPE (newsecattr, OZ_OBJTYPE_SECATTR);
  se = oz_hw_smplock_wait (&oz_s_smplock_se);
  oz_knl_secattr_increfc (newsecattr, 1);
  oldsecattr = devunit -> secattr;
  devunit -> secattr = newsecattr;
  oz_knl_secattr_increfc (oldsecattr, -1);
  oz_hw_smplock_clr (&oz_s_smplock_se, se);
}

/************************************************************************/
/*									*/
/*  Return pointer to device unit's security attributes			*/
/*									*/
/************************************************************************/

OZ_Secattr *oz_knl_devunit_getsecattr (OZ_Devunit *devunit)

{
  OZ_Secattr *secattr;
  uLong se;

  OZ_KNL_CHKOBJTYPE (devunit, OZ_OBJTYPE_DEVUNIT);
  se = oz_hw_smplock_wait (&oz_s_smplock_se);
  secattr = devunit -> secattr;
  oz_knl_secattr_increfc (secattr, 1);
  oz_hw_smplock_clr (&oz_s_smplock_se, se);
  return (secattr);
}

/************************************************************************/
/*									*/
/*  Return the device name for a given device unit			*/
/*									*/
/*    Input:								*/
/*									*/
/*	devunit = pointer to device unit				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_devunit_devname = null terminated device name string 	*/
/*									*/
/************************************************************************/

const char *oz_knl_devunit_devname (OZ_Devunit *devunit)

{
  OZ_KNL_CHKOBJTYPE (devunit, OZ_OBJTYPE_DEVUNIT);
  return (devunit -> unitname);
}

/************************************************************************/
/*									*/
/*  Return the device desc for a given device unit			*/
/*									*/
/*    Input:								*/
/*									*/
/*	devunit = pointer to device unit				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_devunit_devdesc = null terminated device desc string	*/
/*									*/
/************************************************************************/

const char *oz_knl_devunit_devdesc (OZ_Devunit *devunit)

{
  OZ_KNL_CHKOBJTYPE (devunit, OZ_OBJTYPE_DEVUNIT);
  return (devunit -> unitdesc);
}

/************************************************************************/
/*									*/
/*  Return alias device name string					*/
/*									*/
/*    Input:								*/
/*									*/
/*	devunit = device unit to retrieve alias name of			*/
/*	size    = size of buffer					*/
/*	buff    = buffer to return name in				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_devunit_aliasname = 0 : buffer overflowed		*/
/*	                           1 : buffer properly filled		*/
/*	*buff = filled with null-terminated alias name string		*/
/*									*/
/************************************************************************/

int oz_knl_devunit_aliasname (OZ_Devunit *devunit, uLong size, char *buff)

{
  char *p, temp[3*sizeof devunit->aliasno];
  int l;

  if (size == 0) return (0);			/* they have to give us something to work with */
  -- size;					/* ok, store the prefix character */
  *(buff ++) = OZ_DEVUNIT_ALIAS_CHAR;
  p = devunit -> devdriver -> devclass -> classname; /* point to device class name string */
  l = strlen (p);				/* get its length */
  if (l >= size) {				/* see if it would overflow buffer */
    memcpy (buff, p, size);			/* if so, copy as much as would fit */
    return (0);					/* return failure indication */
  }
  memcpy (buff, p, l);				/* ok, copy the whole class name string */
  size -= l;					/* this much less is left */
  buff += l;
  p = temp;					/* point to the temp conversion buffer */
  l = devunit -> aliasno;			/* get the alias number */
  do {
    *(p ++) = (l % 10) + '0';			/* convert a digit */
    l /= 10;
  } while (l != 0);				/* repeat until nothing left to convert */
  while ((size > 0) && (p != temp)) {		/* repeat while there is buffer space and digits left */
    size --;					/* decrease amount of buffer space left */
    *(buff ++) = *(-- p);			/* copy out a digit */
  }
  if (size == 0) return (0);			/* if we ran out of buffer space, return failure */
  *buff = 0;					/* otherwise, null terminate string */
  return (1);					/* return success indication */
}

/************************************************************************/
/*									*/
/*  Return the driver class name for a given device unit		*/
/*									*/
/*    Input:								*/
/*									*/
/*	devunit = pointer to device unit				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_devunit_devname = null terminated device class string	*/
/*									*/
/************************************************************************/

const char *oz_knl_devunit_classname (OZ_Devunit *devunit)

{
  OZ_KNL_CHKOBJTYPE (devunit, OZ_OBJTYPE_DEVUNIT);
  return (devunit -> devdriver -> devclass -> classname);
}

/************************************************************************/
/*									*/
/*  Lookup the device unit for a given device name			*/
/*									*/
/*    Input:								*/
/*									*/
/*	devname = name of device to look up				*/
/*	smplock level = softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_devunit_lookup = NULL : device not found			*/
/*	                        else : pointer to devunit		*/
/*	devunit refcount incremented					*/
/*									*/
/************************************************************************/

OZ_Devunit *oz_knl_devunit_lookup (const char *devname)

{
  char c;
  const char *p;
  int auto_msg, best_l, l;
  uLong dv;
  OZ_Devclass *devclass;
  OZ_Devdriver *devdriver;
  OZ_Devunit *devunit, *host_devunit;

  auto_msg = 0;
  dv = oz_hw_smplock_wait (&oz_s_smplock_dv);

  /* Maybe scan alias name list */

  if (devname[0] == OZ_DEVUNIT_ALIAS_CHAR) {
    for (l = 0; (c = devname[l+1]) != 0; l ++) if ((c >= '0') && (c <= '9')) break;	/* find first digit */
    if (c == 0) goto notfound;								/* if no digits, its not found */
    for (devclass = devclasses; devclass != NULL; devclass = devclass -> next) {	/* find classname that matches alphabetics */
      if (strncasecmp (devclass -> classname, devname + 1, l) == 0) break;
    }
    if (devclass == NULL) goto notfound;						/* if no match, it is not found */
    devname += l;									/* point at last alphabetic */
    l = 0;										/* clear accumulator */
    while ((c = *(++ devname)) != 0) {							/* get a digit */
      if (c < '0') goto notfound;
      if (c > '9') goto notfound;
      l = l * 10 + c - '0';								/* add to accumulator */
    }
    for (devdriver = devclass -> devdrivers; devdriver != NULL; devdriver = devdriver -> next) { /* look for devunit */
      for (devunit = devdriver -> devunits; devunit != NULL; devunit = devunit -> next) {
        if (devunit -> aliasno == l) goto unitfound;
      }
    }
    goto notfound;
  }

  /* Scan list of existing devices for it.  If found, inc ref count and return pointer. */

scan:
  for (devclass = devclasses; devclass != NULL; devclass = devclass -> next) {
    for (devdriver = devclass -> devdrivers; devdriver != NULL; devdriver = devdriver -> next) {
      for (devunit = devdriver -> devunits; devunit != NULL; devunit = devunit -> next) {
        if (strcasecmp (devunit -> unitname, devname) == 0) goto unitfound;
      }
    }
  }

  /* Doesn't exist, call driver autogen entrypoints to see if they can make the device.  If so, return pointer. */

  for (p = devname; (c = *p) != 0; p ++) if (!LEGALUNITNAMECHAR (c)) goto notfound;

  best_l       = 0;
  host_devunit = NULL;
  for (devclass = devclasses; devclass != NULL; devclass = devclass -> next) {
    for (devdriver = devclass -> devdrivers; devdriver != NULL; devdriver = devdriver -> next) {
      for (devunit = devdriver -> devunits; devunit != NULL; devunit = devunit -> next) {
        if (devunit -> auto_entry == NULL) continue;			/* skip it if it doesn't have one */
        l = devunit -> unitname_l;					/* make sure the prefix strings match */
        if (l <= best_l) continue;					/* make sure it's the best match so far */
        if (devname[l] != '.') continue;				/* (must be followed by a dot) */
        if (strncasecmp (devunit -> unitname, devname, l) != 0) continue;
        best_l       = l;
        host_devunit = devunit;
      }
    }
  }

  if (host_devunit != NULL) {
    host_devunit -> refcount ++;					/* ok, increment host devunit's ref count so it doesn't go away on us */
    oz_hw_smplock_clr (&oz_s_smplock_dv, dv);				/* release smp lock - get back to softint level */
    oz_knl_printk ("oz_knl_devunit_lookup: attempting autogen %s via %s\n", devname, host_devunit -> unitname);
    auto_msg = 1;
    devunit  = (*(host_devunit -> auto_entry)) (host_devunit -> auto_param, /* call the driver to see if it can make the device */
                                                host_devunit, 
                                                devname, 
                                                devname + best_l + 1);
    dv = oz_hw_smplock_wait (&oz_s_smplock_dv);				/* set smplock back again */
    oz_knl_devunit_increfc (host_devunit, -1);				/* decrement host devunit's refcount back */
    if (devunit != NULL) {						/* if nothing was created, try another autogen routine */
      if (strcasecmp (devunit -> unitname, devname) != 0) goto scan;	/* maybe the one we want was a side-effect, though */
									/* eg, caller wants 'lsil875_0_20.2.fs' and        */
									/* 'lsil875_0_20.2' was just created, maybe we can */
									/* now find or make 'lsil875_0_20.2.fs'            */
      goto unitfound_ni;						/* it is the exact one we want, go use it */
    }
  }

  /* Not found and could not be created, return NULL pointer */

  if (auto_msg) oz_knl_printk ("oz_knl_devunit_lookup: autogen %s failed\n", devname);
notfound:
  oz_hw_smplock_clr (&oz_s_smplock_dv, dv);
  return (NULL);

  /* Found existing unit, inc ref count and return pointer */

unitfound:
  devunit -> refcount ++;
unitfound_ni:
  oz_hw_smplock_clr (&oz_s_smplock_dv, dv);
  if (auto_msg) oz_knl_printk ("oz_knl_devunit_lookup: autogen %s succeeded\n", devname);
  return (devunit);
}

/************************************************************************/
/*									*/
/*  Mark device allocated to a user/job/process/thread			*/
/*  Then, only that user/job/process/thread can assign non-NL channels	*/
/*									*/
/*    Input:								*/
/*									*/
/*	devunit   = device to allocate					*/
/*	alloc_obj = user/job/process/thread to allocate it to		*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_devunit_alloc = OZ_SUCCESS : now allocated as requested	*/
/*	                  OZ_DEVALLOCATED : was allocd to someone else	*/
/*	                    OZ_DEVICEBUSY : had non-null channels	*/
/*	                    OZ_INVOBJTYPE : alloc_obj not a 		*/
/*	                                    user/job/process/thread	*/
/*									*/
/************************************************************************/

uLong oz_knl_devunit_alloc (OZ_Devunit *devunit, void *alloc_obj)

{
  OZ_Devunit **devalloc;
  uLong dv, sts;

  sts = OZ_INVOBJTYPE;
  devalloc = getdevalloc (alloc_obj);				// make sure it is an object we can deal with
  if (devalloc != NULL) {
    dv = oz_hw_smplock_wait (&oz_s_smplock_dv);			// ok, lock device database
    if (devunit -> alloc_obj != NULL) sts = OZ_DEVALLOCATED;	// make sure it's not already allocated to someone
    else if (devunit -> refc_read || devunit -> refc_write) sts = OZ_DEVICEBUSY; // make sure no non-null channels assigned
    else {
      devunit -> alloc_obj = alloc_obj;				// ok, mark it allocated as requested
      devunit -> alloc_nxt = *devalloc;
      devunit -> alloc_prv = devalloc;
      if (*devalloc != NULL) (*devalloc) -> alloc_prv = &(devunit -> alloc_nxt);
      *devalloc = devunit;
      sts = OZ_SUCCESS;
    }
    oz_hw_smplock_clr (&oz_s_smplock_dv, dv);			// release device database
  }
  return (sts);
}

/************************************************************************/
/*									*/
/*  Mark device allocated to a different user/job/process/thread	*/
/*									*/
/*    Input:								*/
/*									*/
/*	devunit   = device to re-allocate				*/
/*	alloc_obj = user/job/process/thread to allocate it to		*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_devunit_realloc = OZ_SUCCESS : re-allocated as requested	*/
/*	                     OZ_DEVNOTALLOC : device was not allocated	*/
/*	                    OZ_DEVALLOCATED : was allocd to someone else
/*	                      OZ_INVOBJTYPE : alloc_obj not a 		*/
/*	                                      user/job/process/thread	*/
/*									*/
/*    Note:								*/
/*									*/
/*	The device can have non-null channels assigned to it.		*/
/*									*/
/************************************************************************/

uLong oz_knl_devunit_realloc (OZ_Devunit *devunit, void *alloc_obj)

{
  OZ_Devunit **devalloc;
  uLong dv, sts;

  sts = OZ_INVOBJTYPE;
  devalloc = getdevalloc (alloc_obj);				// make sure it is an object we can deal with
  if (devalloc != NULL) {
    dv = oz_hw_smplock_wait (&oz_s_smplock_dv);			// ok, lock device database
    sts = oz_knl_devunit_dealloc (devunit, NULL);		// deallocate it from current caller
    if (sts == OZ_SUCCESS) {
      devunit -> alloc_obj = alloc_obj;				// ok, mark it allocated as requested
      devunit -> alloc_nxt = *devalloc;
      devunit -> alloc_prv = devalloc;
      if (*devalloc != NULL) (*devalloc) -> alloc_prv = &(devunit -> alloc_nxt);
      *devalloc = devunit;
    }
    oz_hw_smplock_clr (&oz_s_smplock_dv, dv);			// release device database
  }
  return (sts);
}

/************************************************************************/
/*									*/
/*  Deallocate device							*/
/*									*/
/*    Input:								*/
/*									*/
/*	devunit = device to de-allocate					*/
/*	alloc_obj = NULL : must be allocated by the current caller	*/
/*	            else : must be allocated by this exact object	*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_devunit_realloc = OZ_SUCCESS : de-allocated		*/
/*	                     OZ_DEVNOTALLOC : device was not allocated	*/
/*	                    OZ_DEVALLOCATED : was allocd to someone else
/*									*/
/*    Note:								*/
/*									*/
/*	The device can have non-null channels assigned to it.		*/
/*									*/
/************************************************************************/

uLong oz_knl_devunit_dealloc (OZ_Devunit *devunit, void *alloc_obj)

{
  uLong dv, sts;
  void *oldalloc;

  dv = oz_hw_smplock_wait (&oz_s_smplock_dv);			// ok, lock device database
  oldalloc = devunit -> alloc_obj;				// see who it is alloc'd to now
  if (oldalloc == NULL) sts = OZ_DEVNOTALLOC;			// error if no one
  else if ((alloc_obj != NULL) && (oldalloc != alloc_obj)) sts = OZ_DEVALLOCATED;
  else if ((alloc_obj == NULL) && ((sts = checkalloc (devunit)) != OZ_SUCCESS)) {}
  else {
    devunit -> alloc_obj = NULL;				// mark it deallocated if so
    *(devunit -> alloc_prv) = devunit -> alloc_nxt;
    if (devunit -> alloc_nxt != NULL) devunit -> alloc_nxt -> alloc_prv = devunit -> alloc_prv;
    devunit -> alloc_nxt = NULL;
    devunit -> alloc_prv = NULL;
    sts = OZ_SUCCESS;
  }
  oz_hw_smplock_clr (&oz_s_smplock_dv, dv);			// release device database
  return (sts);
}

/************************************************************************/
/*									*/
/*  Get who has the device allocated					*/
/*									*/
/*    Input:								*/
/*									*/
/*	devunit = device to check					*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_devunit_getalloc = NULL : device is not allocated	*/
/*	                          else : user/job/process/thread that has it allocated
/*									*/
/*    Note:								*/
/*									*/
/*	caller must dec object's ref count when done with pointer	*/
/*									*/
/************************************************************************/

void *oz_knl_devunit_getalloc (OZ_Devunit *devunit)

{
  uLong dv;
  void *alloc_obj;

  dv = oz_hw_smplock_wait (&oz_s_smplock_dv);			// lock device database
  alloc_obj = devunit -> alloc_obj;				// see what has it allocated, if anything
  if (alloc_obj != NULL) {
    switch (OZ_KNL_GETOBJTYPE (alloc_obj)) {			// increment object's ref count so it won't disappear
      case OZ_OBJTYPE_USER:    { oz_knl_user_increfc    (alloc_obj, 1); break; }
      case OZ_OBJTYPE_JOB:     { oz_knl_job_increfc     (alloc_obj, 1); break; }
      case OZ_OBJTYPE_PROCESS: { oz_knl_process_increfc (alloc_obj, 1); break; }
      case OZ_OBJTYPE_THREAD:  { oz_knl_thread_increfc  (alloc_obj, 1); break; }
    }
  }
  oz_hw_smplock_clr (&oz_s_smplock_dv, dv);			// release device database
  return (alloc_obj);						// return user/job/process/thread pointer
}

/************************************************************************/
/*									*/
/*  Deallocate all devices on a device allocation listhead		*/
/*									*/
/************************************************************************/

void oz_knl_devunit_dallocall (OZ_Devunit **devalloc)

{
  OZ_Devunit *devunit;
  uLong dv;

  dv = oz_hw_smplock_wait (&oz_s_smplock_dv);			// ok, lock device database
  while ((devunit = *devalloc) != NULL) {			// see if anything more alloc'd to user/job/process/thread
    *devalloc = devunit -> alloc_nxt;				// if so, unlink it
    devunit -> alloc_obj = NULL;				// mark it no longer allocated
    devunit -> alloc_nxt = NULL;
    devunit -> alloc_prv = NULL;
  }
  oz_hw_smplock_clr (&oz_s_smplock_dv, dv);			// release device database
}

/* Get device allocation listhead for the given object */

static OZ_Devunit **getdevalloc (void *alloc_obj)

{
  switch (OZ_KNL_GETOBJTYPE (alloc_obj)) {
    case OZ_OBJTYPE_USER:    return (oz_knl_user_getdevalloc    (alloc_obj));
    case OZ_OBJTYPE_JOB:     return (oz_knl_job_getdevalloc     (alloc_obj));
    case OZ_OBJTYPE_PROCESS: return (oz_knl_process_getdevalloc (alloc_obj));
    case OZ_OBJTYPE_THREAD:  return (oz_knl_thread_getdevalloc  (alloc_obj));
  }
  return (NULL);
}

/* Check to see if caller has access to an device */

static uLong checkalloc (OZ_Devunit *devunit)

{
  uLong sts;
  void *alloc_obj, *callr_obj;

  sts = OZ_SUCCESS;
  alloc_obj = devunit -> alloc_obj;
  if (alloc_obj != NULL) {
    switch (OZ_KNL_GETOBJTYPE (alloc_obj)) {
      case OZ_OBJTYPE_USER: {
        callr_obj = oz_knl_job_getuser (oz_knl_process_getjob (oz_knl_thread_getprocesscur ()));
        break;
      }
      case OZ_OBJTYPE_JOB: {
        callr_obj = oz_knl_process_getjob (oz_knl_thread_getprocesscur ());
        break;
      }
      case OZ_OBJTYPE_PROCESS: {
        callr_obj = oz_knl_thread_getprocesscur ();
        break;
      }
      case OZ_OBJTYPE_THREAD: {
        callr_obj = oz_knl_thread_getcur ();
        break;
      }
      default: oz_crash ("oz_knl_devio checkalloc: bad object type %d", OZ_KNL_GETOBJTYPE (alloc_obj));
    }
    if (callr_obj != alloc_obj) sts = OZ_DEVALLOCATED;
  }
  return (sts);
}

/************************************************************************/
/*									*/
/*  Increment device unit reference count				*/
/*									*/
/*    Input:								*/
/*									*/
/*	devunit = device unit to decrement ref count			*/
/*	inc = amount to increment by (+, -, or 0)			*/
/*									*/
/*	smplock <= dv							*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_devunit_increfc = new ref count				*/
/*	reference count incremented by inc				*/
/*	if zero and it was a clone, devunit is deleted			*/
/*									*/
/************************************************************************/

Long oz_knl_devunit_increfc (OZ_Devunit *devunit, Long inc)

{
  const OZ_Devfunc *devfunc;
  int oktodel;
  int (*clonedel) (OZ_Devunit *cloned_devunit, void *devex, int cloned);
  Long refc;
  uLong dv;
  OZ_Devunit **ldevunit, *xdevunit;

  dv = oz_hw_smplock_wait (&oz_s_smplock_dv);

  devunit -> refcount += inc;
  refc = devunit -> refcount;
  if (refc < 0) oz_crash ("oz_knl_devunit_increfc: ref count for %s went negative (%d)", devunit -> unitname, refc);

  if (refc == 0) {
    if (devunit -> ioopendcount != 0)  oz_crash ("oz_knl_devunit_increfc: refc 0 but ioopendcount %d", devunit -> ioopendcount);
    devfunc = devunit -> functable;
    if (devfunc != NULL) {
      clonedel = devfunc -> clonedel;
      if (clonedel != NULL) {
        oktodel = (*clonedel) (devunit, devunit -> devex, devunit -> cloned);
        if (oktodel) {
          for (ldevunit = &(devunit -> devdriver -> devunits); (xdevunit = *ldevunit) != devunit; ldevunit = &(xdevunit -> next)) {
            if (xdevunit == NULL) oz_crash ("oz_knl_devunit_increfc: can't find devunit on devdriver list");
          }
          *ldevunit = xdevunit -> next;
          OZ_KNL_NPPFREE (xdevunit);
          devunit_count --;
        }
      }
    }
  }

  oz_hw_smplock_clr (&oz_s_smplock_dv, dv);
  return (refc);
}

/************************************************************************/
/*									*/
/*  Count the total number device units in the system			*/
/*									*/
/*    Input:								*/
/*									*/
/*	smplock level <= dv						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_devunit_count = number of devunits in the system		*/
/*									*/
/************************************************************************/

uLong oz_knl_devunit_count (void)

{
  return (devunit_count);
}

/************************************************************************/
/*									*/
/*  Get next device in the device list					*/
/*									*/
/*    Input:								*/
/*									*/
/*	lastdevunit = last device unit (or NULL to start at top)	*/
/*	smplock <= dv							*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_devunit_getnext = next device unit (or NULL if no more)	*/
/*									*/
/*    Note:								*/
/*									*/
/*	ref count incremented on new devunit (if any)			*/
/*	old devunit ref count not changed				*/
/*									*/
/************************************************************************/

OZ_Devunit *oz_knl_devunit_getnext (OZ_Devunit *lastdevunit)

{
  uLong dv;
  OZ_Devclass *devclass;
  OZ_Devdriver *devdriver;
  OZ_Devunit *devunit;

  devunit = lastdevunit;

  dv = oz_hw_smplock_wait (&oz_s_smplock_dv);

  /* If devunit was NULL, find the very first device in the list */

  if (devunit == NULL) {
    for (devclass = devclasses; devclass != NULL; devclass = devclass -> next) {
      for (devdriver = devclass -> devdrivers; devdriver != NULL; devdriver = devdriver -> next) {
        devunit = devdriver -> devunits;
        if (devunit != NULL) goto done_new;
      }
    }
done_new:;
  }

  /* Otherwise, find the next device in the list */

  else {
    OZ_KNL_CHKOBJTYPE (devunit, OZ_OBJTYPE_DEVUNIT);
    devdriver = devunit -> devdriver;		/* find old devs driver */
    devclass = devdriver -> devclass;		/* find old devs class */
    devunit = devunit -> next;			/* point to next dev on driver */
    while (devunit == NULL) {
      devdriver = devdriver -> next;		/* no more devs, get next driver for class */
      while (devdriver == NULL) {
        devclass = devclass -> next;		/* no more drivers, get next class */
        if (devclass == NULL) goto done_old;	/* done if no more classes */
        devdriver = devclass -> devdrivers;	/* ok, get first driver for new class */
      }
      devunit = devdriver -> devunits;		/* get first dev for new driver */
    }
done_old:;
  }

  if (devunit != NULL) oz_knl_devunit_increfc (devunit, 1); /* if got one, inc its ref count */
  oz_hw_smplock_clr (&oz_s_smplock_dv, dv);

  return (devunit);
}

/************************************************************************/
/*									*/
/*  Determine if the given devunit is unassigned (ie, has no channels 	*/
/*  assigned to it)							*/
/*									*/
/*    Input:								*/
/*									*/
/*	devunit = device unit to make determination about		*/
/*	smplock = any level						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_devunit_unassigned = 0 : device has channels assigned to it
/*	                            1 : device has no channels assigned	*/
/*									*/
/************************************************************************/

int oz_knl_devunit_unassigned (OZ_Devunit *devunit)

{
  OZ_KNL_CHKOBJTYPE (devunit, OZ_OBJTYPE_DEVUNIT);
  return (devunit -> iochans == NULL);
}

/************************************************************************/
/*									*/
/*  Get device's function table pointer					*/
/*									*/
/************************************************************************/

const OZ_Devfunc *oz_knl_devunit_functable (OZ_Devunit *devunit)

{
  OZ_KNL_CHKOBJTYPE (devunit, OZ_OBJTYPE_DEVUNIT);

  return (devunit -> functable);
}

/************************************************************************/
/*									*/
/*  Create an I/O channel and connect it to the named device unit	*/
/*									*/
/*    Input:								*/
/*									*/
/*	devunit  = device unit						*/
/*	lockmode = lock mode for the I/O channel			*/
/*	procmode = processor mode for I/O channel			*/
/*									*/
/*	smplock  = softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_iochan_create = OZ_SUCCESS : successful			*/
/*	                             else : error status		*/
/*	*iochan_r = filled in with io channel pointer			*/
/*									*/
/************************************************************************/

uLong oz_knl_iochan_crbynm (const char *devname, OZ_Lockmode lockmode, OZ_Procmode procmode, OZ_Secattr *secattr, OZ_Iochan **iochan_r)

{
  uLong sts;
  OZ_Devunit *devunit;

  devunit = oz_knl_devunit_lookup (devname);
  if (devunit == NULL) sts = OZ_BADDEVNAME;
  else {
    sts = oz_knl_iochan_create (devunit, lockmode, procmode, secattr, iochan_r);
    oz_knl_devunit_increfc (devunit, -1);
  }

  return (sts);
}

uLong oz_knl_iochan_create (OZ_Devunit *devunit, OZ_Lockmode lockmode, OZ_Procmode procmode, OZ_Secattr *secattr, OZ_Iochan **iochan_r)

{
  uLong dv, sts;

  /* Synchronize access to device database */

  dv = oz_hw_smplock_wait (&oz_s_smplock_dv);

  /* Finish up in common routine */

  sts = assign_channel (devunit, lockmode, lockmode, procmode, secattr, dv, iochan_r);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Copy an I/O channel and connect it to the named device unit		*/
/*									*/
/*    Input:								*/
/*									*/
/*	iochan   = I/O channel to copy					*/
/*	lockmode = lock mode for the I/O channel			*/
/*	           or OZ_LOCKMODE_XX to keep the same mode		*/
/*	procmode = processor mode for I/O channel			*/
/*									*/
/*	smplock <= softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_iochan_copy = OZ_SUCCESS : successful			*/
/*	                           else : error status			*/
/*	*iochan_r = filled in with io channel pointer			*/
/*									*/
/************************************************************************/

uLong oz_knl_iochan_copy (OZ_Iochan *iochan, OZ_Lockmode lockmode, OZ_Procmode procmode, OZ_Iochan **iochan_r)

{
  uLong dv, sts;
  OZ_Devunit *devunit;

  /* Make sure procmode is not more privileged than channel being copied, maximize if so */

  if (procmode < iochan -> procmode) procmode = iochan -> procmode;

  /* Maybe we copy the lock mode */

  if (lockmode == OZ_LOCKMODE_XX) lockmode = iochan -> lockmode;

  /* Otherwise, make sure caller had at least as much access as they want to give to copy */

  else {
    if (OZ_LOCK_ALLOW_TEST (lockmode, OZ_LOCK_ALLOWS_SELF_WRITE) && !OZ_LOCK_ALLOW_TEST (iochan -> lockmode, OZ_LOCK_ALLOWS_SELF_WRITE)) return (OZ_NOWRITEACCESS);
    if (OZ_LOCK_ALLOW_TEST (lockmode, OZ_LOCK_ALLOWS_SELF_READ)  && !OZ_LOCK_ALLOW_TEST (iochan -> lockmode, OZ_LOCK_ALLOWS_SELF_READ))  return (OZ_NOREADACCESS);
  }

  /* Synchronize access to device database */

  dv = oz_hw_smplock_wait (&oz_s_smplock_dv);

  /* Get the devunit pointer from the old I/O channel */

  devunit = iochan -> devunit;

  /* Go finish up in common routine */

  sts = assign_channel (devunit, OZ_LOCKMODE_NL, lockmode, procmode, iochan -> secattr, dv, iochan_r);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Assign I/O channel to device					*/
/*									*/
/*    Input:								*/
/*									*/
/*	devunit = unit to assign channel to				*/
/*	cklockmode = check the device for conflicts with this lockmode	*/
/*	asnlockmode = assign I/O channel with this lockmode		*/
/*	procmode = processor mode to assign channel at			*/
/*	secattr = security attributes for i/o channel			*/
/*	dv = return smp lock level when unlocking oz_s_smplock_dv	*/
/*									*/
/*    Output:								*/
/*									*/
/*	assign_channel = OZ_SUCCESS : successful			*/
/*	                       else : error status			*/
/*	*iochan_r = I/O channel						*/
/*									*/
/************************************************************************/

static uLong assign_channel (OZ_Devunit *devunit, OZ_Lockmode cklockmode, OZ_Lockmode asnlockmode, OZ_Procmode procmode, OZ_Secattr *secattr, uLong dv, OZ_Iochan **iochan_r)

{
  uLong (*assign) (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode);
  uLong (*clonecre) (OZ_Devunit *template_devunit, void *template_devex, int template_cloned, OZ_Procmode procmode, OZ_Devunit **cloned_devunit);
  uLong sts;
  OZ_Iochan *iochan, *nextiochan;

  iochan = NULL;

  /* Maybe we clone the device - if there is a clonecre routine and it does not want to clone the  */
  /* device, it can simply copy the input parameter to the output parameter and return OZ_SUCCESS. */
  /* If it wants to clone the device, it must call oz_knl_devunit_create to create the clone with  */
  /* whatever attributes it wants, then returns the new devunit pointer in the output parameter.   */

  clonecre = devunit -> functable -> clonecre;
  if (clonecre != NULL) {
    sts = (*clonecre) (devunit, devunit -> devex, devunit -> cloned, procmode, &devunit);
    if (sts != OZ_SUCCESS) goto rtn_dv;
  }

  else devunit -> refcount ++;

  /* Make sure the (cloned) devunit is not allocated to someone else  */
  /* But allow an NL mode channel to be assigned so they can get info */

  if (OZ_LOCK_ALLOW_TEST (cklockmode, OZ_LOCK_ALLOWS_SELF_READ) || OZ_LOCK_ALLOW_TEST (cklockmode, OZ_LOCK_ALLOWS_SELF_WRITE)) {
    sts = checkalloc (devunit);
    if (sts != OZ_SUCCESS) goto rtn_dvi;
  }

  /* Check for lock mode access conflict (on the cloned devunit).  If it fails, maybe delete the cloned device. */

  if ((!OZ_LOCK_ALLOW_TEST (cklockmode, OZ_LOCK_ALLOWS_OTHERS_READ)  && (devunit -> refc_read  != 0)) 
   || (!OZ_LOCK_ALLOW_TEST (cklockmode, OZ_LOCK_ALLOWS_OTHERS_WRITE) && (devunit -> refc_write != 0)) 
   || (OZ_LOCK_ALLOW_TEST  (cklockmode, OZ_LOCK_ALLOWS_SELF_READ)    && (devunit -> deny_read  != 0)) 
   || (OZ_LOCK_ALLOW_TEST  (cklockmode, OZ_LOCK_ALLOWS_SELF_WRITE)   && (devunit -> deny_write != 0))) {
    sts = OZ_ACCONFLICT;
    goto rtn_dvi;
  }

  /* Allocate an I/O channel block */

  iochan = OZ_KNL_NPPMALLOQ (devunit -> functable -> chn_exsize + sizeof *iochan);

  /* Fill in channel */

  iochan -> objtype    = OZ_OBJTYPE_IOCHAN;
  iochan -> devunit    = devunit;
  iochan -> procmode   = procmode;
  iochan -> lockmode   = asnlockmode;
  iochan -> lastiotid  = oz_knl_thread_getid (NULL);
  iochan -> refcount   = 1;
  iochan -> readcount  = 0;
  iochan -> writecount = 0;
  iochan -> secattr    = secattr;
  iochan -> calldeas   = 0;
  oz_knl_secattr_increfc (secattr, 1);

  /* Link iochan to devunit */

  nextiochan = devunit -> iochans;
  iochan -> next = nextiochan;
  iochan -> prev = &(devunit -> iochans);
  if (nextiochan != NULL) nextiochan -> prev = &(iochan -> next);
  devunit -> iochans = iochan;

  if (OZ_LOCK_ALLOW_TEST  (asnlockmode, OZ_LOCK_ALLOWS_SELF_READ))    devunit -> refc_read ++;
  if (OZ_LOCK_ALLOW_TEST  (asnlockmode, OZ_LOCK_ALLOWS_SELF_WRITE))   devunit -> refc_write ++;
  if (!OZ_LOCK_ALLOW_TEST (asnlockmode, OZ_LOCK_ALLOWS_OTHERS_READ))  devunit -> deny_read ++;
  if (!OZ_LOCK_ALLOW_TEST (asnlockmode, OZ_LOCK_ALLOWS_OTHERS_WRITE)) devunit -> deny_write ++;

  /* Now that devunit refcount is incremented and channel linked, release dv smplock */

  oz_hw_smplock_clr (&oz_s_smplock_dv, dv);

  /* If there is a device dependent assign routine, call it */
  /* Undo everything if it fails                            */

  sts = OZ_SUCCESS;
  iochan -> calldeas = 1;
  assign = devunit -> functable -> assign;
  if (assign != NULL) sts = (*assign) (devunit, devunit -> devex, iochan, iochan -> chnex, procmode);
  if (sts != OZ_SUCCESS) {
    iochan -> calldeas = 0;
    oz_knl_iochan_increfc (iochan, -1);
    iochan = NULL;
  }

  goto rtn;

  /* Return I/O channel pointer and status */

rtn_dvi:
  oz_knl_devunit_increfc (devunit, -1);
rtn_dv:
  oz_hw_smplock_clr (&oz_s_smplock_dv, dv);
rtn:
  *iochan_r = iochan;
  return (sts);
}

/************************************************************************/
/*									*/
/*  Return devunit pointer for a channel				*/
/*									*/
/************************************************************************/

OZ_Devunit *oz_knl_iochan_getdevunit (OZ_Iochan *iochan)

{
  OZ_KNL_CHKOBJTYPE (iochan, OZ_OBJTYPE_IOCHAN);
  return (iochan -> devunit);
}

/************************************************************************/
/*									*/
/*  Get a channel's read or write count					*/
/*									*/
/************************************************************************/

uLong oz_knl_iochan_readcount (OZ_Iochan *iochan)

{
  OZ_KNL_CHKOBJTYPE (iochan, OZ_OBJTYPE_IOCHAN);
  return (iochan -> readcount);
}

uLong oz_knl_iochan_writecount (OZ_Iochan *iochan)

{
  OZ_KNL_CHKOBJTYPE (iochan, OZ_OBJTYPE_IOCHAN);
  return (iochan -> writecount);
}

/************************************************************************/
/*									*/
/*  Return chnex pointer for a channel					*/
/*									*/
/************************************************************************/

void *oz_knl_iochan_ex (OZ_Iochan *iochan)

{
  OZ_KNL_CHKOBJTYPE (iochan, OZ_OBJTYPE_IOCHAN);
  return (iochan -> chnex);
}

/************************************************************************/
/*									*/
/*  Return lockmode for a channel					*/
/*									*/
/************************************************************************/

OZ_Lockmode oz_knl_iochan_getlockmode (OZ_Iochan *iochan)

{
  OZ_KNL_CHKOBJTYPE (iochan, OZ_OBJTYPE_IOCHAN);
  return (iochan -> lockmode);
}

/************************************************************************/
/*									*/
/*  Get thread-id that last queued an I/O on a channel			*/
/*									*/
/************************************************************************/

OZ_Threadid oz_knl_iochan_getlastiotid (OZ_Iochan *iochan)

{
  OZ_KNL_CHKOBJTYPE (iochan, OZ_OBJTYPE_IOCHAN);
  return (iochan -> lastiotid);
}

/************************************************************************/
/*									*/
/*  Get iochan's security attributes					*/
/*									*/
/************************************************************************/

OZ_Secattr *oz_knl_iochan_getsecattr (OZ_Iochan *iochan)

{
  OZ_Secattr *secattr;
  uLong se;

  OZ_KNL_CHKOBJTYPE (iochan, OZ_OBJTYPE_IOCHAN);
  se = oz_hw_smplock_wait (&oz_s_smplock_se);		/* keep them from being changed out from under us */
  secattr = iochan -> secattr;				/* get the secattr pointer */
  oz_knl_secattr_increfc (secattr, 1);			/* inc ref count so they can't be freed off */
  oz_hw_smplock_clr (&oz_s_smplock_se, se);		/* release lock */
  return (secattr);					/* return pointer with ref count incd */
}

/************************************************************************/
/*									*/
/*  Set iochan's security attributes					*/
/*									*/
/************************************************************************/

void oz_knl_iochan_setsecattr (OZ_Iochan *iochan, OZ_Secattr *secattr)

{
  OZ_Secattr *oldsecattr;
  uLong se;

  OZ_KNL_CHKOBJTYPE (iochan, OZ_OBJTYPE_IOCHAN);
  OZ_KNL_CHKOBJTYPE (secattr, OZ_OBJTYPE_SECATTR);
  se = oz_hw_smplock_wait (&oz_s_smplock_se);		/* keep others from making changes */
  oldsecattr = iochan -> secattr;			/* get the old secattr pointer */
  iochan -> secattr = secattr;				/* store the new pointer */
  oz_knl_secattr_increfc (secattr, 1);			/* inc new ref count so it can't be deleted on us */
  oz_knl_secattr_increfc (oldsecattr, -1);		/* free off the old secattrs */
  oz_hw_smplock_clr (&oz_s_smplock_se, se);		/* release lock */
}

/************************************************************************/
/*									*/
/*  Get next channel in devunit's list					*/
/*  Last I/O chan's ref count unchanged					*/
/*  Next I/O chan's ref count incremented				*/
/*									*/
/************************************************************************/

OZ_Iochan *oz_knl_iochan_getnext (OZ_Iochan *lastiochan, OZ_Devunit *devunit)

{
  uLong dv;
  OZ_Iochan *iochan;

  iochan = lastiochan;
  dv = oz_hw_smplock_wait (&oz_s_smplock_dv);
  if (iochan == NULL) iochan = devunit -> iochans;
  else iochan = iochan -> next;
  if (iochan != NULL) oz_knl_iochan_increfc (iochan, 1);
  oz_hw_smplock_clr (&oz_s_smplock_dv, dv);
  return (iochan);
}

/************************************************************************/
/*									*/
/*  Count channels in devunit's list					*/
/*									*/
/************************************************************************/

uLong oz_knl_iochan_count (OZ_Devunit *devunit)

{
  uLong count, dv;
  OZ_Iochan *iochan;

  OZ_KNL_CHKOBJTYPE (devunit, OZ_OBJTYPE_DEVUNIT);

  dv = oz_hw_smplock_wait (&oz_s_smplock_dv);
  count = 0;
  for (iochan = devunit -> iochans; iochan != NULL; iochan = iochan -> next) count ++;
  oz_hw_smplock_clr (&oz_s_smplock_dv, dv);
  return (count);
}

/************************************************************************/
/*									*/
/*  Rundown I/O for a thread						*/
/*									*/
/*    Input:								*/
/*									*/
/*	thread   = pointer to thread block				*/
/*	ioopqp   = pointer to corresponding ioopq listhead		*/
/*	procmode = processor mode of requests to abort			*/
/*									*/
/*	smplock  = softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	all I/O for the thread aborted					*/
/*									*/
/*    Note:								*/
/*									*/
/*	This routine waits for the I/O's to complete			*/
/*									*/
/************************************************************************/

void oz_knl_iorundown (OZ_Thread *thread, OZ_Procmode procmode)

{
  OZ_Devunit *devunit;
  OZ_Event *abortevent;
  OZ_Iochan *iochan;
  OZ_Ioop *ioop, *ioops, *nioop;
  uLong dv, sts;
  void (*abort) (OZ_Devunit *devunit, void *devex, 
                 OZ_Iochan  *iochan,  void *chnex, 
                 OZ_Ioop    *ioop,    void *iopex, 
                 OZ_Procmode procmode);

  OZ_KNL_CHKOBJTYPE (thread, OZ_OBJTYPE_THREAD);

  abortevent = NULL;
  ioops = NULL;

  while (1) {

    /* Get list of ioops to abort and remove from thread's queue. */
    /* Note that when oz_knl_iodone sees abortevent set, it will  */
    /* not free the ioop, but will set the event flag instead.    */

    dv = oz_hw_smplock_wait (&oz_s_smplock_dv);
    for (ioop = *oz_knl_thread_getioopqp (thread); ioop != NULL; ioop = nioop) {
      nioop = ioop -> tq_next;						/* point to next in thread's -> ioopq list */
      if (ioop -> procmode >= procmode) {
        *(ioop -> tq_prev) = nioop;					/* remove from thread -> ioopq list */
        if (nioop != NULL) nioop -> tq_prev = ioop -> tq_prev;
        ioop -> tq_next = ioops;					/* insert on ioops list */
        ioop -> tq_prev = NULL;
        ioops = ioop;
        if (abortevent == NULL) {
          sts = oz_knl_event_create (10, "io rundown", NULL, &abortevent);
          if (sts != OZ_SUCCESS) oz_crash ("oz_knl_iorundown: error %u creating abort event flag");
        }
        ioop -> abortevent = abortevent;				/* this tells oz_knl_iodone to set this event flag and clear link */
									/* instead of removing ioop from threadq and freeing it */
      }
    }
    oz_hw_smplock_clr (&oz_s_smplock_dv, dv);

    /* If nothing to abort, we're all done */

    if (ioops == NULL) break;

    /* The selected ioops have been removed from thread -> ioopq and placed in  */
    /* ioops.  They have all had their abortevent pointer set up.  When         */
    /* oz_knl_iodone sees a non-NULL ioop -> abortevent, it will set that event */
    /* flag and clear the ioop -> abortevent link.  It will not attempt to      */
    /* remove the ioop from thread -> ioopq nor will it free off the ioop.      */

    /* Tell drivers to abort them all - as they complete, oz_knl_iodone will set the abortevent flag */

    for (ioop = ioops; ioop != NULL; ioop = ioop -> tq_next) {
      iochan  = ioop -> iochan;				/* get the I/O channel associated with request */
      if (iochan == NULL) continue;			/* maybe the last time through the loop aborted this request */
      devunit = iochan -> devunit;			/* get associated device unit */
      abort   = devunit -> functable -> abort;		/* get abort routine entrypoint */
      if (abort != NULL) {
        (*abort) (devunit, devunit -> devex, 		/* call the abort routine - the abort routine may call iodone directly or call it later on */
                  iochan,  iochan  -> chnex, 
                  ioop,    ioop    -> iopex, 
                  procmode);
      }
    }

    do {

      /* Wait for oz_knl_iodone to be called for one of the requests we're aborting */

      oz_knl_event_waitone (abortevent);
      oz_knl_event_set (abortevent, 0);

      /* Free off completed requests */

      dv = oz_hw_smplock_wait (&oz_s_smplock_dv);
      while ((ioop = ioops) != NULL) {		/* see if there are any to do */
        if (ioop -> abortevent != NULL) break;	/* if this is non-NULL, it is still in progress */
        ioops = ioop -> tq_next;		/* abortevent is NULL, free off the request */
        oz_knl_ioop_increfc (ioop, -1);
      }
      oz_hw_smplock_clr (&oz_s_smplock_dv, dv);

      /* Repeat as long as there are requests that haven't completed */

    } while (ioops != NULL);

    /* Repeat in case calling driver's abort routine started any new requests */
  }

  /* No more requests exist of the given type */

  if (abortevent != NULL) oz_knl_event_increfc (abortevent, -1);
}

/************************************************************************/
/*									*/
/*  Abort any I/O in progress on a channel				*/
/*									*/
/*    Input:								*/
/*									*/
/*	iochan = i/o channel pointer returned by oz_knl_iochan_create	*/
/*	procmode = abort requests at this and outer modes		*/
/*	ioop = NULL : abort all such requests				*/
/*	       else : abort only this request				*/
/*									*/
/*	smplevel = softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	i/o operations on channel aborted				*/
/*									*/
/*    Note:								*/
/*									*/
/*	This routine in most cases merely starts aborting the 		*/
/*	requests.  The caller must then wait for the requests to 	*/
/*	actually terminate (they will call the post-process routine).	*/
/*									*/
/*	The abort routine will get called as part of the process exit 	*/
/*	processing.  Drivers should abort the i/o operation if 		*/
/*	possible as soon as they can and call the post-process 		*/
/*	routines.							*/
/*									*/
/************************************************************************/

void oz_knl_ioabort (OZ_Iochan *iochan, OZ_Procmode procmode)

{
  oz_knl_ioabort2 (iochan, procmode, NULL);
}

void oz_knl_ioabort2 (OZ_Iochan *iochan, OZ_Procmode procmode, OZ_Ioop *ioop)

{
  OZ_Devunit *devunit;
  void (*abort) (OZ_Devunit *devunit, void *devex, 
                 OZ_Iochan  *iochan,  void *chnex, 
                 OZ_Ioop    *ioop,    void *iopex, 
                 OZ_Procmode procmode);
  void *iopex;

  OZ_KNL_CHKOBJTYPE (iochan, OZ_OBJTYPE_IOCHAN);
  OZ_KNL_CHKOBJTYPE (ioop,   OZ_OBJTYPE_IOOP);

  iopex = NULL;
  if (ioop != NULL) iopex = ioop -> iopex;

  /* Call dev unit's abort routine if there is one */

  devunit = iochan  -> devunit;
  abort   = devunit -> functable -> abort;
  if (abort != NULL) {
    (*abort) (devunit, devunit -> devex, iochan, iochan -> chnex, ioop, iopex, procmode);
  }
}

/************************************************************************/
/*									*/
/*  Return whether the given I/O operation matches the given abort 	*/
/*  parameters								*/
/*									*/
/*    Input:								*/
/*									*/
/*	ioop     = I/O operation to be tested				*/
/*	iochan   = I/O channel being aborted				*/
/*	procmode = processor mode doing the abort			*/
/*	aioop    = I/O to be aborted (or NULL if all that match)	*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_ioabortok = 0 : ioop doesn't match given parameters	*/
/*	                   1 : ioop matches given parameters		*/
/*									*/
/************************************************************************/

int oz_knl_ioabortok (OZ_Ioop *ioop, OZ_Iochan *iochan, OZ_Procmode procmode, OZ_Ioop *aioop)

{
  OZ_KNL_CHKOBJTYPE (ioop,   OZ_OBJTYPE_IOOP);
  OZ_KNL_CHKOBJTYPE (iochan, OZ_OBJTYPE_IOCHAN);
  OZ_KNL_CHKOBJTYPE (aioop,  OZ_OBJTYPE_IOOP);

  if (aioop != NULL) return (ioop == aioop);
  return ((ioop -> iochan == iochan) && (ioop -> procmode >= procmode));
}

/************************************************************************/
/*									*/
/*  Start an I/O operation on a device					*/
/*									*/
/*    Input:								*/
/*									*/
/*	sysio     = 0 : I/O will be aborted when the current thread exits
/*	                (params are in system or process space)		*/
/*	            1 : I/O will persist beyond thread exit		*/
/*	                (all parameters must be loacated in system 	*/
/*	                    address space)				*/
/*	iochan    = I/O channel returned by oz_knl_iochan_create	*/
/*	procmode  = processor mode associated with request		*/
/*	iopostent = post-processing routine entrypoint			*/
/*	iopostpar = post-processing routine parameter			*/
/*	status_r  = where to store completion status			*/
/*	event     = event flag to increment on completion		*/
/*	astentry  = ast routine entrypoint to call on completion	*/
/*	astparam  = ast routine parameter				*/
/*	funcode   = I/O function code					*/
/*	as        = argument struct size				*/
/*	ap        = argument struct pointer				*/
/*									*/
/*	smplevel  = softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_iostart = OZ_STARTED : I/O started			*/
/*	                       else : error status			*/
/*	*ioop_r = pointer to ioop					*/
/*									*/
/*    Note:								*/
/*									*/
/*	Caller will get the iopost call iff the returned status was 	*/
/*	OZ_STARTED (event if ioabort or iodeassign is called).  In all 	*/
/*	other cases the I/O should be considered comlpeted at this 	*/
/*	time (either successfully or unsuccessfully), and the iopost 	*/
/*	routine will not be called.					*/
/*									*/
/*	The iopost routine is called in kernel mode with softint 	*/
/*	delivery inhibited.						*/
/*									*/
/*	The above also equally applies to status_r, event and astentry.	*/
/*									*/
/*	If neither an status return nor an astentry is supplied, the 	*/
/*	routine waits in kernel mode for the completion of the I/O and 	*/
/*	returns the final status.					*/
/*									*/
/************************************************************************/

	/* a routine to start an I/O then wait for it in kernel mode */

uLong oz_knl_io (OZ_Iochan *iochan, uLong funcode, uLong as, void *ap)

{
  uLong sts;

  sts = oz_knl_iostart3 (1, NULL, iochan, OZ_PROCMODE_KNL, NULL, NULL, NULL, NULL, NULL, NULL, funcode, as, ap);
  return (sts);
}

	/* start an I/O with sysio=0 and ioop_r=NULL */

uLong oz_knl_iostart (OZ_Iochan *iochan, 
                      OZ_Procmode procmode, 
                      void (*iopostent) (void *iopostpar, uLong status), 
                      void *iopostpar, 
                      volatile uLong *status_r, 
                      OZ_Event *event, 
                      OZ_Astentry astentry, 
                      void *astparam, 
                      uLong funcode, 
                      uLong as, 
                      void *ap)

{
  return (oz_knl_iostart3 (0, NULL, iochan, procmode, iopostent, iopostpar, status_r, event, astentry, astparam, funcode, as, ap));
}

	/* start an I/O with ioop_r=NULL */

uLong oz_knl_iostart2 (int sysio, 
                       OZ_Iochan *iochan, 
                       OZ_Procmode procmode, 
                       void (*iopostent) (void *iopostpar, uLong status), 
                       void *iopostpar, 
                       volatile uLong *status_r, 
                       OZ_Event *event, 
                       OZ_Astentry astentry, 
                       void *astparam, 
                       uLong funcode, 
                       uLong as, 
                       void *ap)

{
  return (oz_knl_iostart3 (sysio, NULL, iochan, procmode, iopostent, iopostpar, 
                           status_r, event, astentry, astparam, funcode, as, ap));
}

	/* general start I/O routine */

uLong oz_knl_iostart3 (int sysio, 
                       OZ_Ioop **ioop_r, 
                       OZ_Iochan *iochan, 
                       OZ_Procmode procmode, 
                       void (*iopostent) (void *iopostpar, uLong status), 
                       void *iopostpar, 
                       volatile uLong *status_r, 
                       OZ_Event *event, 
                       OZ_Astentry astentry, 
                       void *astparam, 
                       uLong funcode, 
                       uLong as, 
                       void *ap)

{
  Long posting;
  OZ_Devunit *devunit;
  OZ_Ioop *ioop, **ioopqp, *nioop;
  OZ_Process *process;
  OZ_Seclock *seclock;
  OZ_Thread *thread;
  uLong dv, sts;
  uLong (*start) (OZ_Devunit *devunit, 
                  void *devex, 
                  OZ_Iochan *iochan, 
                  void *chnex, 
                  OZ_Procmode procmode, 
                  OZ_Ioop *ioop, 
                  void *iopex, 
                  uLong funcode, 
                  uLong as, 
                  void *ap);

  OZ_KNL_CHKOBJTYPE (iochan, OZ_OBJTYPE_IOCHAN);

  if (ioop_r != NULL) *ioop_r = NULL;

  if (iochan -> procmode < procmode) {
    oz_knl_printk ("oz_knl_iostart: iochan %p, iochan -> procmode %u, caller procmode %u, insufficient procmode\n", iochan, iochan -> procmode, procmode);
    return (OZ_PROCMODE);
  }

  if ((funcode & OZ_IO_FW) && !OZ_LOCK_ALLOW_TEST (iochan -> lockmode, OZ_LOCK_ALLOWS_SELF_WRITE)) {
    oz_knl_printk ("oz_knl_iostart: iochan %p, iochan -> lockmode %u, caller funcode 0x%x, no write access\n", iochan, iochan -> lockmode, funcode);
    return (OZ_NOWRITEACCESS);
  }

  if ((funcode & OZ_IO_FR) && !OZ_LOCK_ALLOW_TEST (iochan -> lockmode, OZ_LOCK_ALLOWS_SELF_READ)) {
    oz_knl_printk ("oz_knl_iostart: iochan %p, iochan -> lockmode %u, caller funcode 0x%x, no read access\n", iochan, iochan -> lockmode, funcode);
    return (OZ_NOREADACCESS);
  }

  /* Determine thread and process parameters - NULL if sysio, else current thread and process */

  process = oz_s_systemproc;			/* assume all parameters are in system space */
  thread  = NULL;				/* assume I/O will not run down when current thread exits */
  iochan -> lastiotid = 0;
  if (!sysio) {
    process = oz_knl_process_getcur ();		/* parameters may be in process space */
    thread  = oz_knl_thread_getcur ();		/* I/O will be run down when current thread exits */
    iochan -> lastiotid = oz_knl_thread_getid (thread);
    oz_knl_thread_incios (thread, 1);
  }

  /* Allocate and fill in an ioop for the function                      */
  /* Start out with refcount 1, then decrement when driver calls iodone */

  devunit = iochan -> devunit;
  ioop = OZ_KNL_NPPMALLOQ (devunit -> functable -> iop_exsize + sizeof *ioop);
  if (ioop == NULL) return (OZ_EXQUOTANPP);
  ioop -> objtype    = OZ_OBJTYPE_IOOP;
  ioop -> refcount   = 1;
  ioop -> state      = IOOP_STATE_INITING;
  ioop -> iochan     = iochan;
  ioop -> procmode   = procmode;
  ioop -> thread     = thread;
  ioop -> process    = process;
  ioop -> iopostent  = iopostent;
  ioop -> iopostpar  = iopostpar;
  ioop -> status_r   = status_r;
  ioop -> event      = event;
  ioop -> astentry   = astentry;
  ioop -> astparam   = astparam;
  ioop -> status     = OZ_PENDING;
  ioop -> finentry   = NULL;
  ioop -> nseclocks  = 0;
  ioop -> abortevent = NULL;

  /* If caller supplied an event flag, inc its ref count to keep it in memory until we're all done       */
  /* Else, if we will need an event flag below, create temp one, and it gets deleted when we're all done */

  if (ioop -> event != NULL) {
    oz_knl_event_increfc (ioop -> event, 1);
  } else if ((status_r == NULL) && (astentry == NULL) && (iopostent == NULL)) {
    sts = oz_knl_event_create (sizeof devunit -> unitname, devunit -> unitname, NULL, &(ioop -> event));
    if (sts != OZ_SUCCESS) {
      OZ_KNL_NPPFREE (ioop);
      return (sts);
    }
  }

  /* Return ioop pointer - increase refcount to 2 - caller will have to call oz_knl_ioop_increfc to decrement */

  if (ioop_r != NULL) {
    ioop -> refcount = 2;
    *ioop_r = ioop;
  }

  /* Put request on thread -> ioopq */

  ioop -> tq_next = NULL;
  ioop -> tq_prev = NULL;
  if (thread != NULL) {
    ioopqp = oz_knl_thread_getioopqp (thread);
    dv = oz_hw_smplock_wait (&oz_s_smplock_dv);
    ioop -> tq_next = nioop = *ioopqp;
    ioop -> tq_prev = ioopqp;
    *ioopqp = ioop;
    if (nioop != NULL) nioop -> tq_prev = &(ioop -> tq_next);
    oz_hw_smplock_clr (&oz_s_smplock_dv, dv);
  }

  /* Increment process reference count in case the requesting thread switches processes, we don't want this one disappearing */

  oz_knl_process_increfc (ioop -> process, 1);

  /* Call dev unit's start i/o routine */

  /* Everything is set up here so the driver's start routine could actually call */
  /* oz_knl_iodone if it wants to (but it better return with status OZ_STARTED)  */

  OZ_HW_ATOMIC_INCBY1_ULONG (devunit -> opcount);
  if (funcode & OZ_IO_FW) OZ_HW_ATOMIC_INCBY1_ULONG (iochan -> writecount);
  else if (funcode & OZ_IO_FR) OZ_HW_ATOMIC_INCBY1_ULONG (iochan -> readcount);

  oz_knl_iochan_increfc (iochan, 1);
  OZ_HW_ATOMIC_INCBY1_LONG (devunit -> ioopendcount);
  ioop -> state = IOOP_STATE_STARTED;
  start = devunit -> functable -> start;
  sts = (*start) (devunit, devunit -> devex, iochan, iochan -> chnex, procmode, ioop, ioop -> iopex, funcode, as, ap);

  /* The driver MUST eventually call oz_knl_iodone if it returned OZ_STARTED */
  /* It MUST NOT call oz_knl_iodone if it did not return status OZ_STARTED   */

  /* This status might be confusing to some wait routines */

  if (sts == OZ_PENDING) oz_crash ("oz_knl_iostart: start routine returned OZ PENDING");

  /* If it returned other than OZ_STARTED, the state had better still be STARTED (meaning iodone has not been called) */
  /* Then change the state to SYNCMPL (to indicate true synchronous completion)                                       */

  if (sts != OZ_STARTED) {
    if (!oz_hw_atomic_setif_long (&(ioop -> state), IOOP_STATE_SYNCMPL, IOOP_STATE_STARTED)) {
      oz_crash ("oz_knl_iostart: start returned %u but state was %d", sts, ioop -> state);
    }
  }

  /* If start routine returned OZ_STARTED and did not call iodone directly and      */
  /* caller did not give any way to get completion status, wait for completion here */

  else if ((status_r == NULL) && (astentry == NULL) && (iopostent == NULL) && oz_hw_atomic_setif_long (&(ioop -> state), IOOP_STATE_WAITING, IOOP_STATE_STARTED)) {
    while (ioop -> state == IOOP_STATE_WAITING) {	/* repeat until iodone gets called */
      oz_knl_event_waitone (ioop -> event);		/* wait for it */
      if (ioop -> state != IOOP_STATE_WAITING) break;	/* likely quick exit in case it was called */
      oz_knl_event_set (ioop -> event, 0);		/* not called yet, clear event flag before re-testing */
    }
    if (ioop -> state != IOOP_STATE_PSYNCMP) oz_crash ("oz_knl_iostart: went to state %d after WAITING", ioop -> state);
    oz_knl_event_set (ioop -> event, 1);		/* set event in case it is shared */
  }

  /* If start routine returned OZ_STARTED and hasn't called iodone yet but caller has a way */
  /* to get completion status, return OZ_STARTED (meaning it will complete asynchronously)  */

  else if (oz_hw_atomic_setif_long (&(ioop -> state), IOOP_STATE_INPROGR, IOOP_STATE_STARTED)) goto rtn;

  /* If start routine returned OZ_STARTED but called iodone directly it's done */

  else if (ioop -> state == IOOP_STATE_PSYNCMP) {}

  /* That should cover it */

  else oz_crash ("oz_knl_iostart: state %d, start returned %u", ioop -> state, sts);

  /* Now we have a synchronously or pseudo-synchronously completed request */

  /* If pseudo-synchronous completion, it means the start routine returned OZ_STARTED but called     */
  /* iodone directly (or maybe iodone got called via an interrupt before the start routine returned) */
  /* Anyway, iodone will realize we are going to do this and won't bother calling finentry as it is  */
  /* quicker if we do it here (because we know we don't have to switch process mapping)              */

  if (ioop -> state == IOOP_STATE_PSYNCMP) {
    if (ioop -> finentry != NULL) (*(ioop -> finentry)) (ioop -> finparam, 1, &(ioop -> status));
    sts = ioop -> status;
  }

  /* Either of these status values are bad to return as completion statuses */

  if ((sts == OZ_PENDING) || (sts == OZ_STARTED)) oz_crash ("oz_knl_iostart: bad final status %u", sts);

  /* Now free off the ioop */

  if (ioop_r != NULL) {									/* if caller wanted ioop pointer, */
    *ioop_r = NULL;									/* ... return a NULL, ... */
    ioop -> refcount --;								/* because we are about to free the ioop */
  }
  if (ioop -> thread != NULL) {
    dv = oz_hw_smplock_wait (&oz_s_smplock_dv);						/* unlink from thread's queue */
    if (ioop -> abortevent != NULL) oz_crash ("oz_knl_iostart: abortevent set");
    *(ioop -> tq_prev) = nioop = ioop -> tq_next;
    if (nioop != NULL) nioop -> tq_prev = ioop -> tq_prev;
    oz_hw_smplock_clr (&oz_s_smplock_dv, dv);
  }
  OZ_HW_ATOMIC_DECBY1_LONG (devunit -> ioopendcount);					/* one less i/o pending on the device */
  oz_knl_iochan_increfc (iochan, -1);							/* one less thing referencing channel */
											/* - iochan and devunit could disappear from decr'ing iochan refcount */
  if (ioop -> event != NULL) oz_knl_event_increfc (ioop -> event, -1);			/* one less thing referencing event flag */
											/* (if we created a temp one, it gets freed) */
  while (ioop -> nseclocks > 0) {							/* release all locked buffers */
    seclock = ioop -> seclocks[--(ioop->nseclocks)];
    ioop -> seclocks[ioop->nseclocks] = NULL;
    oz_knl_section_iounlk (seclock);
  }
  oz_knl_process_increfc (ioop -> process, -1);						/* release process pointer */
  if (ioop -> refcount != 1) oz_crash ("oz_knl_iostart: ioop refcount not 1 (%d)", ioop -> refcount);
  OZ_KNL_NPPFREE (ioop);								/* no one is ref'ing it, free it off */

rtn:
  return (sts);
}

/************************************************************************/
/*									*/
/*  Return thread associated with an I/O request			*/
/*									*/
/*  This thread is used to get security stuff and for I/O rundown	*/
/*  Returns NULL if it is not associated with any particular thread	*/
/*									*/
/************************************************************************/

OZ_Thread *oz_knl_ioop_getthread (OZ_Ioop *ioop)

{
  OZ_KNL_CHKOBJTYPE (ioop, OZ_OBJTYPE_IOOP);
  return (ioop -> thread);
}

/************************************************************************/
/*									*/
/*  Return process associated with an I/O request			*/
/*									*/
/*  This thread is used to address the parameters			*/
/*  It never returns NULL (might return system process, though)		*/
/*									*/
/************************************************************************/

OZ_Process *oz_knl_ioop_getprocess (OZ_Ioop *ioop)

{
  OZ_KNL_CHKOBJTYPE (ioop, OZ_OBJTYPE_IOOP);
  return (ioop -> process);
}

/************************************************************************/
/*									*/
/*  Same as oz_knl_ioop_lockr except it stops at a terminating null	*/
/*									*/
/*    Input:								*/
/*									*/
/*	ioop = i/o operation packet being performed			*/
/*	size = maximum string size to search for null char		*/
/*	buff = start address of the string				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_ioop_lockz = OZ_SUCCESS : successful			*/
/*	                          else : error status			*/
/*	*phypages_r = physical page array pointer			*/
/*	*npages_r   = number of physical pages that were locked		*/
/*	*byteoffs_r = offset in first physical page			*/
/*									*/
/************************************************************************/

uLong oz_knl_ioop_lockz (OZ_Ioop *ioop, uLong size, const void *buff, uLong *rlen, const OZ_Mempage **phypages_r, OZ_Mempage *npages_r, uLong *byteoffs_r)

{
  Long i;
  OZ_Seclock *seclock;
  uLong sts;

  if (size == (uLong)(-1)) size = 4096;

  sts = oz_knl_section_iolockz (ioop -> procmode, size, buff, rlen, &seclock, npages_r, phypages_r, byteoffs_r);
  if (sts != OZ_SUCCESS) return (sts);
  i = oz_hw_atomic_inc_long (&(ioop -> nseclocks), 1);
  if (i > MAXIOOPLOCKS) {
    oz_knl_section_iounlk (seclock);
    return (OZ_TOOMANYBUFFERS);
  }
  ioop -> seclocks[i-1] = seclock;
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Driver calls this routine as part of its start routine to lock 	*/
/*  parameter buffers in memory.  When the driver calls oz_knl_iodone, 	*/
/*  the buffers are unlocked.						*/
/*									*/
/*    Input:								*/
/*									*/
/*	ioop = the ioop of the call					*/
/*	size = number of bytes in users buffer				*/
/*	buff = user buffer address					*/
/*	writing = 0 : read access required				*/
/*	          1 : write access required				*/
/*									*/
/*	ipl = softint							*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_ioop_lock = OZ_SUCCESS : buffer is locked		*/
/*	                    OZ_ACCVIO : user doesn't have access	*/
/*	                         else : i/o error			*/
/*	**phypages = filled in with pointer to physical page number array
/*	*nppages = filled in with number of physical pages		*/
/*	*byteoffs = filled in with byte offset in first physical page	*/
/*									*/
/************************************************************************/

uLong oz_knl_ioop_lock (OZ_Ioop *ioop, uLong size, const void *buff, int writing, const OZ_Mempage **phypages_r, OZ_Mempage *npages_r, uLong *byteoffs_r)

{
  Long i;
  OZ_Seclock *seclock;
  uLong sts;

  sts = oz_knl_section_iolock (ioop -> procmode, size, buff, writing, &seclock, npages_r, phypages_r, byteoffs_r);
  if (sts != OZ_SUCCESS) return (sts);
  i = oz_hw_atomic_inc_long (&(ioop -> nseclocks), 1);
  if (i > MAXIOOPLOCKS) {
    oz_knl_section_iounlk (seclock);
    return (OZ_TOOMANYBUFFERS);
  }
  ioop -> seclocks[i-1] = seclock;
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Driver calls this routine when it has completed an I/O operation 	*/
/*  that was started with the iostart routine				*/
/*									*/
/*  The driver must also call this routine if the I/O operation was 	*/
/*  aborted with the ioabort routine, with an appropriate error status 	*/
/*  (like OZ_ABORTED)							*/
/*									*/
/*    Input:								*/
/*									*/
/*	ioop     = as passed to the iostart routine			*/
/*	status   = final completion status				*/
/*	kthread  = obstafusticated					*/
/*	finentry = routine to call in originating process's context	*/
/*	finparam = param to pass to 'entry' routine			*/
/*									*/
/*	smplocks = none							*/
/*	softints = inhibited						*/
/*									*/
/*    Output:								*/
/*									*/
/*	order of events:						*/
/*									*/
/*	 1) Call finentry routine					*/
/*	 2) Write status value						*/
/*	 3) Unlock buffers						*/
/*	 4) Increment event flag					*/
/*	 5) Call post-processing routine				*/
/*	 6) Queue completion ast					*/
/*									*/
/************************************************************************/

	/*****************************/
	/* high-ipl callable version */
	/*****************************/

void oz_knl_iodonehi (OZ_Ioop *ioop, uLong status, void *kthread, void (*finentry) (void *finparam, int finok, uLong *status_r), void *finparam)

{
  uLong hi;

  OZ_KNL_CHKOBJTYPE (ioop, OZ_OBJTYPE_IOOP);

  /* Don't allow status values OZ_PENDING and OZ_STARTED - they will confuse wait routines */

  if ((status == OZ_PENDING) || (status == OZ_STARTED)) oz_crash ("oz_knl_iodone: bad status value %u", status);

  /* Save completion parameters */

  ioop -> status   = status;
  ioop -> finentry = finentry;
  ioop -> finparam = finparam;

  /* Maybe oz_knl_iostart can finish it directly */

  OZ_HW_MB;																// make sure it will see the status values
  if (oz_hw_atomic_setif_long (&(ioop -> state), IOOP_STATE_PSYNCMP, IOOP_STATE_STARTED)) {}						// still in driver's start routine
  else if (oz_hw_atomic_setif_long (&(ioop -> state), IOOP_STATE_PSYNCMP, IOOP_STATE_WAITING)) oz_knl_event_set (ioop -> event, 1);	// in oz_knl_iostart's wait loop

  /* If not, queue request to lowipl queue for completion */

  else if (oz_hw_atomic_setif_long (&(ioop -> state), IOOP_STATE_ASYNCMP, IOOP_STATE_INPROGR)) {
    ioop -> donenext = NULL;
    hi = oz_hw_smplock_wait (&oz_s_smplock_hi);				/* lock queue */
    *iodonehiqt = ioop;							/* put request on end of queue */
    iodonehiqt  = &(ioop -> donenext);
    if (iodonehi_lowipl != NULL) {					/* see if routine is idle */
      oz_knl_lowipl_call (iodonehi_lowipl, iodonehi_proc, NULL);	/* if so, start it */
      iodonehi_lowipl = NULL;						/* mark it busy */
    }
    oz_hw_smplock_clr (&oz_s_smplock_hi, hi);				/* unlock queue */
  }

  /* Invalid state */

  else oz_crash ("oz_knl_iodone: bad state %d", ioop -> state);
}

static void iodonehi_proc (void *dummy, OZ_Lowipl *lowipl)

{
  OZ_Ioop *ioop;
  uLong hi;

check:
  hi = oz_hw_smplock_wait (&oz_s_smplock_hi);			/* lock queue */
  ioop = iodonehiqh;						/* see if anything in queue */
  if (ioop != NULL) {
    iodonehiqh = ioop -> donenext;				/* ok, unlink request */
    if (ioop -> donenext == NULL) iodonehiqt = &iodonehiqh;
    if (lowipl != NULL) {					/* see if first time through loop */
      if (iodonehiqh == NULL) iodonehi_lowipl = lowipl;		/* if no more to do, go idle */
      else oz_knl_lowipl_call (lowipl, iodonehi_proc, NULL);	/* if more to do, go do them maybe on another cpu */
      lowipl = NULL;						/* (only dispose of lowipl once) */
    }
    oz_hw_smplock_clr (&oz_s_smplock_hi, hi);			/* unlock queue, return to softint level */
    iodone (ioop);						/* post it */
    goto check;							/* see if there are any more to post */
  }
  if (lowipl != NULL) iodonehi_lowipl = lowipl;			/* nothing left to do, dispose of lowipl */
  oz_hw_smplock_clr (&oz_s_smplock_hi, hi);			/* release queue */
}

	/**********************************/
	/* softint level callable version */
	/**********************************/

void oz_knl_iodone (OZ_Ioop *ioop, uLong status, void *kthread, void (*finentry) (void *finparam, int finok, uLong *status_r), void *finparam)

{
  OZ_KNL_CHKOBJTYPE (ioop, OZ_OBJTYPE_IOOP);

  /* Don't allow status values OZ_PENDING and OZ_STARTED - they will confuse wait routines */

  if ((status == OZ_PENDING) || (status == OZ_STARTED)) oz_crash ("oz_knl_iodone: bad status value %u", status);

  /* Save completion parameters and post request */

  ioop -> status   = status;
  ioop -> finentry = finentry;
  ioop -> finparam = finparam;

  /* Figure out what to do with it based on its state */

  OZ_HW_MB;																// make sure it will see the status values
  if (oz_hw_atomic_setif_long (&(ioop -> state), IOOP_STATE_PSYNCMP, IOOP_STATE_STARTED)) {}						// still in driver's start routine
  else if (oz_hw_atomic_setif_long (&(ioop -> state), IOOP_STATE_PSYNCMP, IOOP_STATE_WAITING)) oz_knl_event_set (ioop -> event, 1);	// in oz_knl_iostart's wait loop
  else if (oz_hw_atomic_setif_long (&(ioop -> state), IOOP_STATE_ASYNCMP, IOOP_STATE_INPROGR)) iodone (ioop);				// oz_knl_iostart returned OZ_STARTED to caller, so post it the hard way
  else oz_crash ("oz_knl_iodone: bad state %d", ioop -> state);										// invalid state
}

/****************************************************************************************************************************************/
/* Asynchronous completion routine - we're at softint level and oz_knl_iostart has returned (or will very shortly) OZ_STARTED to caller */
/****************************************************************************************************************************************/

static void iodone (OZ_Ioop *ioop)

{
  Long posting, starting;
  OZ_Ast *ast;
  OZ_Process *curprocess, *iopprocess;
  OZ_Seclock *seclock;
  uLong dv, sts;
  volatile uLong *status_r;

  /* If a status block was given, write the status */

  /* To store the status, it must either be in system address space (which is the same */
  /* for everybody), or must be in the same process as when the request was queued     */

  /* Similar for finentry routine, except we must be in the same process (system space */
  /* doesn't matter at all because we don't know what addresses it wants to access)    */

  status_r = ioop -> status_r;
  if ((status_r != NULL) || (ioop -> finentry != NULL)) {
    iopprocess = curprocess = oz_knl_process_getcur ();							/* get our current process mapping */
    if (ioop -> process != oz_s_systemproc) {
      if ((ioop -> finentry != NULL) || !OZ_HW_ISSYSADDR (status_r)) {					/* see if status block is in system address space */
        iopprocess = ioop -> process;									/* if not, switch process context */
        if (iopprocess != curprocess) oz_knl_process_setcur (iopprocess);
      }
    }
    if (ioop -> finentry != NULL) {
      (*(ioop -> finentry)) (ioop -> finparam, 1, &(ioop -> status));					/* call the finentry routine */
      ioop -> finentry = NULL;										/* ... as the process is addressible now */
    }
    if (status_r != NULL) {
      OZ_HW_MB;												/* all other memory writes must have completed before the status is written */
													/* because we want to guarantee that the instant the status value is written, */
													/* ... everything else is good */
      if (ioop -> procmode == OZ_PROCMODE_KNL) *status_r = ioop -> status;				/* if caller is kernel mode, do it quickly */
      else {
        sts = oz_knl_section_uput (ioop -> procmode, sizeof *status_r, &(ioop -> status), (void *)status_r); /* otherwise, make it hard */
        if (sts != OZ_SUCCESS) oz_knl_printk ("oz_knl_iodone: error %u writing status block at %p\n", sts, status_r);
      }
      ioop -> status_r = NULL;
    }
    if (iopprocess != curprocess) oz_knl_process_setcur (curprocess);					/* make sure we're back to original process */
  }

  /* Unlock any memory locked by the request */

  while (ioop -> nseclocks > 0) {
    seclock = ioop -> seclocks[--(ioop->nseclocks)];
    ioop -> seclocks[ioop->nseclocks] = NULL;
    oz_knl_section_iounlk (seclock);
  }

  /* We will no longer access any buffers, etc, so disassociate process */

  oz_knl_process_increfc (ioop -> process, -1);
  ioop -> process = NULL;

  /* If an event flag was given, increment the event flag */

  if (ioop -> event != NULL) {
    oz_knl_event_inc (ioop -> event, 1);
    oz_knl_event_increfc (ioop -> event, -1);
    ioop -> event = NULL;
  }

  /* Call any post-processing routine given */

  if (ioop -> iopostent != NULL) {
    (*(ioop -> iopostent)) (ioop -> iopostpar, ioop -> status);
    ioop -> iopostent = NULL;
  }

  /* If an ast routine was given, queue the ast */

  if (ioop -> astentry != NULL) {
    sts = oz_knl_ast_create (ioop -> thread, ioop -> procmode, ioop -> astentry, ioop -> astparam, 0, &ast);
    if (sts == OZ_SUCCESS) oz_knl_thread_queueast (ast, ioop -> status);
    if (sts != OZ_SUCCESS) oz_knl_printk ("oz_knl_iodone: error %u creating procmode %u ast at %p to thread %p\n", 
                                          sts, ioop -> procmode, ioop -> astentry, ioop -> thread);
    ioop -> astentry = NULL;
  }

  /* Decrement count of pending requests for device */

  OZ_HW_ATOMIC_DECBY1_LONG (ioop -> iochan -> devunit -> ioopendcount);

  /* Disassociate I/O channel - iochan and devunit can be deleted by this call */

  oz_knl_iochan_increfc (ioop -> iochan, -1);
  ioop -> iochan = NULL;

  /* If abortevent, set the event flag and clear pointer so the iorundown routine knows it is complete */

  dv = oz_hw_smplock_wait (&oz_s_smplock_dv);
  if (ioop -> abortevent != NULL) {
    oz_knl_event_set (ioop -> abortevent, 1);
    ioop -> abortevent = NULL;
  }

  /* Otherwise, unlink from thread queue and free off the ioop */

  else {
    if (ioop -> thread != NULL) {
      *(ioop -> tq_prev) = ioop -> tq_next;
      if (ioop -> tq_next != NULL) ioop -> tq_next -> tq_prev = ioop -> tq_prev;
    }
    oz_knl_ioop_increfc (ioop, -1);
  }
  oz_hw_smplock_clr (&oz_s_smplock_dv, dv);
}

/************************************************************************/
/*									*/
/*  Increment ioop's ref count.  If zero, free it off			*/
/*									*/
/************************************************************************/

Long oz_knl_ioop_increfc (OZ_Ioop *ioop, Long inc)

{
  Long refc;

  OZ_KNL_CHKOBJTYPE (ioop, OZ_OBJTYPE_IOOP);
  refc = oz_hw_atomic_inc_long (&(ioop -> refcount), inc);
  if (refc < 0) oz_crash ("oz_knl_ioop_increfc: ref count neg (%d)", refc);
  if (refc == 0) OZ_KNL_NPPFREE (ioop);
  return (refc);
}

/************************************************************************/
/*									*/
/*  Sense any of a list of i/o devices for readiness			*/
/*									*/
/*    Input:								*/
/*									*/
/*	numchans   = number of entries in iochans and ioselcodes arrays	*/
/*	iochans    = list of io channels to be scanned			*/
/*	ioselcodes = list of readiness codes				*/
/*	senses     = where to return sense codes			*/
/*	procmode   = access mode of caller				*/
/*	selpostent = select post processing routine entrypoint		*/
/*	selpostpar = select post processing routine parameter		*/
/*									*/
/*	smplock = softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_ioselect = OZ_STARTED : condition not currently satisfied
/*	                               caller must wait for post process routine
/*	                  OZ_SUCCESS : condition immediately satisfied	*/
/*	                        else : error status			*/
/*									*/
/*	*senses = sense status when event flag finally gets set		*/
/*	          it is also set on return if other than OZ_STARTED is returned
/*									*/
/*    Note:								*/
/*									*/
/*	The caller is assumed to maintain a reference count out on the 	*/
/*	I/O channels until the operation completes.  Also, the caller 	*/
/*	must maintain the iochans, ioselcode and senses arrays.		*/
/*									*/
/************************************************************************/

uLong oz_knl_ioselect (int sysio, 
                       OZ_Ioselect **ioselect_r, 
                       uLong numchans, 
                       OZ_Iochan **iochans, 
                       uLong *ioselcodes, 
                       uLong *senses, 
                       OZ_Procmode procmode, 
                       void (*selpostent) (void *selpostpar, 
                                           uLong numchans, 
                                           OZ_Iochan **iochans, 
                                           uLong *ioselcodes, 
                                           uLong *senses, 
                                           OZ_Procmode procmode), 
                       void *selpostpar)

{
  const OZ_Devfunc *functable;
  int fin;
  OZ_Devunit *devunit;
  OZ_Iochan *iochan;
  OZ_Ioselect *ioselect;
  uLong i, *selex, sts;
  uLong (*select) (OZ_Devunit *devunit, void *devex, OZ_Iochan *iochan, void *chnex, OZ_Procmode procmode, 
                   OZ_Ioselect *ioselect, void *selex, uLong *ioselcode, int arm);

  ioselect = NULL;
  selex = ioselect -> active + ((ioselect -> numchans + 31) / 32);
  for (i = 0; i < numchans; i ++) {
    selex += (iochans[i] -> devunit -> functable -> sel_exsize + 3) / 4;
    senses[i] = OZ_PENDING;
  }
  sts = ((uByte *)selex) - (uByte *)ioselect;

  ioselect = OZ_KNL_NPPMALLOQ (sts);				/* allocate context block */
  if (ioselect == NULL) return (OZ_EXQUOTANPP);

  ioselect -> objtype    = OZ_OBJTYPE_IOSELECT;
  ioselect -> sysio      = sysio;
  ioselect -> thread     = NULL;
  ioselect -> process    = NULL;
  if (!sysio) {
    ioselect -> thread   = oz_knl_thread_getcur ();
    ioselect -> process  = oz_knl_process_getcur ();
    oz_knl_thread_increfc (ioselect -> thread, 1);
    oz_knl_process_increfc (ioselect -> process, 1);
  }
  ioselect -> numchans   = numchans;				/* save call parameters */
  ioselect -> procmode   = procmode;
  ioselect -> senses     = senses;
  ioselect -> iochans    = iochans;
  ioselect -> ioselcodes = ioselcodes;
  ioselect -> selpostent = selpostent;
  ioselect -> selpostpar = selpostpar;
  ioselect -> state      = IOSEL_STATE_STARTUP;			/* we're in the start up loop */
  ioselect -> pending    = 1;					/* count the number in progress (offset by 1 in startup loop) */
  fin = 0;							/* no one has finished synchronously */
  selex = ioselect -> active + ((ioselect -> numchans + 31) / 32);
  for (i = 0; i < ioselect -> numchans; i ++) {			/* loop through list of channels */
    if ((i % 32) == 0) ioselect -> active[i/32] = 0;		/* clear the active bits for the longword */
    iochan = ioselect -> iochans[i];				/* get I/O channel */
    sts = OZ_PROCMODE;						/* check processor mode of the channel */
    if (iochan -> procmode >= ioselect -> procmode) {
      devunit   = iochan -> devunit;				/* ok, point to corresponding devunit */
      functable = devunit -> functable;
      select    = functable -> select;				/* get its select function entrypoint */
      sts = OZ_DEVNOTSELECTABLE;				/* maybe it does not have one */
      if (select != NULL) {
        OZ_HW_ATOMIC_INCBY1_ULONG (devunit -> opcount);
        sts = (*select) (devunit, devunit -> devex, iochan, iochan -> chnex, ioselect -> procmode, 
                         ioselect, selex, ioselect -> ioselcodes + i, 1);
        if (sts == OZ_PENDING) oz_crash ("oz_knl_ioselect: arm returned OZ_PENDING");
        if (sts == OZ_STARTED) {				/* if it started, */
          OZ_HW_ATOMIC_INCBY1_LONG (ioselect -> pending);	/* ... increment the counter */
          ioselect -> active[i/32] |= 1 << (i % 32);		/* ... and set the 'active' bit */
          selex += (functable -> sel_exsize + 3) / 4;		/* ... increment selex's pointer to next */
        }
      }
    }
    if (sts != OZ_STARTED) {
      ioselect -> senses[i] = sts;				/* save final status (not OZ_STARTED because it's not final) */
      fin = 1;							/* something finished */
    }
  }

  sts = OZ_STARTED;						/* assume asynchronous finish */
  if (ioseldone (ioselect, fin)) {				/* decrement offset 'pending' value and check for all done */
    if (!(ioselect -> sysio)) {
      oz_knl_thread_increfc (ioselect -> thread, 1);
      oz_knl_process_increfc (ioselect -> process, 1);
    }
    OZ_KNL_NPPFREE (ioselect);					/* all done, free context block */
    sts = OZ_SUCCESS;						/* indicate synchronous completion */
  }

  return (sts);							/* return status indicating async or sync */
}

/************************************************************************/
/*									*/
/*  This routine is called by the drivers when a select condition has 	*/
/*  been satisfied.  Drivers should also call this routine if they 	*/
/*  get an abort call.							*/
/*									*/
/*    Input:								*/
/*									*/
/*	ioselect = as passed to driver's select routine			*/
/*	smplevel = softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	select operation terminated					*/
/*	sense array filled in						*/
/*	post-processing routine called					*/
/*									*/
/************************************************************************/

	/*****************************/
	/* High-ipl callable routine */
	/*****************************/

void oz_knl_ioseldonehi (OZ_Ioselect *ioselect, uLong *ioselcode, uLong status)

{
  int i;
  uLong hi;

  OZ_KNL_CHKOBJTYPE (ioselect, OZ_OBJTYPE_IOSELECT);
  i = ioselcode - ioselect -> ioselcodes;
  oz_hw_atomic_setif_ulong (&(ioselect -> senses[i]), status, OZ_PENDING);
  if (oz_hw_atomic_inc_long (&(ioselect -> hicount), 1) == 1) {		// increment number of times called for this select
    ioselect -> nexthi = NULL;						// if first time, link it to list
    hi = oz_hw_smplock_wait (&oz_s_smplock_hi);
    *ioseldoneqt = ioselect;
    ioseldoneqt  = &(ioselect -> nexthi);
    if (iosellowipl != NULL) {
      oz_knl_lowipl_call (iosellowipl, ioseldonelo, NULL);		// call ioseldonelo if not already active
      iosellowipl = NULL;
    }
    oz_hw_smplock_clr (&oz_s_smplock_hi, hi);
  }
}

static void ioseldonelo (void *dummy, OZ_Lowipl *lowipl)

{
  Long hicount;
  OZ_Ioselect *ioselect, *ioselects;
  uLong hi;

  hi = oz_hw_smplock_wait (&oz_s_smplock_hi);				// lock the list
  ioselects   = ioseldoneqh;						// get everything on the list
  ioseldoneqt = &ioseldoneqh;						// empty the list
  ioseldoneqh = NULL;
  iosellowipl = lowipl;							// re-arm to be called again
  oz_hw_smplock_clr (&oz_s_smplock_hi, hi);				// unlock the list

  while ((ioselect = ioselects) != NULL) {				// repeat while stuff to do
    ioselects = ioselect -> nexthi;					// unlink from list
    hicount   = oz_hw_atomic_set_long (&(ioselect -> hicount), 0);	// get count, re-arm to be queued again
    while (-- hicount >= 0) oz_knl_ioseldone (ioselect, NULL, 0);	// call ioseldone for each time ioseldonehi was called
  }
}

	/**********************************/
	/* Softint level callable routine */
	/**********************************/

void oz_knl_ioseldone (OZ_Ioselect *ioselect, uLong *ioselcode, uLong status)

{
  int i;

  OZ_KNL_CHKOBJTYPE (ioselect, OZ_OBJTYPE_IOSELECT);
  if (ioselcode != NULL) {
    i = ioselcode - ioselect -> ioselcodes;
    oz_hw_atomic_setif_ulong (&(ioselect -> senses[i]), status, OZ_PENDING);
  }
  if (ioseldone (ioselect, -1)) {
    (*(ioselect -> selpostent)) (ioselect -> selpostpar, 
                                 ioselect -> numchans, 
                                 ioselect -> iochans, 
                                 ioselect -> ioselcodes, 
                                 ioselect -> senses, 
                                 ioselect -> procmode);
    if (!(ioselect -> sysio)) {
      oz_knl_thread_increfc (ioselect -> thread, 1);
      oz_knl_process_increfc (ioselect -> process, 1);
    }
    OZ_KNL_NPPFREE (ioselect);
  }
}

/* fin = 0 : being called as part of start-up loop, no request has actually finished */
/*      -1 : being called because a driver has said a condition has been satisfied   */

static int ioseldone (OZ_Ioselect *ioselect, int fin)

{
  const OZ_Devfunc *functable;
  int alldone;
  Long newstate, oldstate;
  OZ_Devunit *devunit;
  OZ_Iochan *iochan;
  OZ_Process *oldprocess;
  uLong dv, i, *selex, sts;
  uLong (*select) (OZ_Devunit *devunit, void *devex, OZ_Iochan *iochan, void *chnex, OZ_Procmode procmode, 
                   OZ_Ioselect *ioselect, void *selex, uLong *ioselcode, int arm);

  /* Update state */

  do {
    newstate = oldstate = ioselect -> state;			// sample current state
    switch (oldstate) {

      /* Still in startup loop */

      case IOSEL_STATE_STARTUP: {
        if (fin < 0) newstate = IOSEL_STATE_ASYNINSTART;	// if a dev finishes asyncly, remember that one did
        else if (fin > 0) newstate = IOSEL_STATE_FINISHED;	// if end of start loop with sync completion, we're now finished
        else newstate = IOSEL_STATE_INPROGRESS;			// else it's end of start loop with no completion, we're in progress
        break;
      }

      /* Startup loop, but had an async completion */

      case IOSEL_STATE_ASYNINSTART: {
        if (fin >= 0) newstate = IOSEL_STATE_FINISHED;		// if end of startup loop, we're now finished
        break;
      }

      /* Returned back out to caller with OZ_STARTED, and something just finished */

      case IOSEL_STATE_INPROGRESS: {
        if (fin >= 0) oz_crash ("oz_knl_ioseldone: INPROGRESS with fin %d", fin);
        newstate = IOSEL_STATE_FINISHED;			// only way to get here is via async completion
        break;
      }

      /* We're waiting for all drivers to check back in */

      case IOSEL_STATE_FINISHED: {
        if (fin >= 0) oz_crash ("oz_knl_ioseldone: FINISHED with fin %d", fin);
        break;
      }

      /* Who knows what */

      default: oz_crash ("oz_knl_ioseldone: bad state %d", oldstate);
    }        

    /* Try to write new state value, repeat if old state changed via another cpu */

  } while ((newstate != oldstate) && !oz_hw_atomic_setif_long (&(ioselect -> state), newstate, oldstate));

  /* If this is the first time called outside of the startup loop, */
  /* tell all drivers to give up and tell us where they're at now. */

  /* Drivers should still call oz_knl_ioseldone, either right now or as soon as they can so the pending count gets decremented */

  if ((newstate == IOSEL_STATE_FINISHED) && (oldstate != IOSEL_STATE_FINISHED)) {
    if (!(ioselect -> sysio)) {
      oldprocess = oz_knl_process_getcur ();
      oz_knl_process_setcur (ioselect -> process);
    }
    selex = ioselect -> active + ((ioselect -> numchans + 31) / 32);
    for (i = 0; i < ioselect -> numchans; i ++) {		/* loop through list of channels */
      iochan    = ioselect -> iochans[i];			/* get i/o channel */
      devunit   = iochan -> devunit;				/* point to corresponding devunit */
      functable = devunit -> functable;
      if (ioselect -> active[i/32] & (1 << (i % 32))) {		/* make sure it was made active in startup loop */
        select  = devunit -> functable -> select;		/* get its select function entrypoint */
        sts = (*select) (devunit, devunit -> devex, iochan, iochan -> chnex, ioselect -> procmode, 
                         ioselect, selex, ioselect -> ioselcodes + i, 0);
        if (sts == OZ_PENDING) oz_crash ("oz_knl_ioseldone: disarm returned OZ_PENDING"); /* a very confusing status */
        if (sts == OZ_STARTED) oz_crash ("oz_knl_ioseldone: disarm returned OZ_STARTED"); /* a very confusing status */
        ioselect -> senses[i] = sts;				/* save the final sense status */
        selex += (functable -> sel_exsize + 3) / 4;		/* increment onto next driver's area */
      }
    }
    if (!(ioselect -> sysio)) oz_knl_process_setcur (oldprocess);
  }

  alldone = (oz_hw_atomic_inc_long (&(ioselect -> pending), -1) == 0);	/* one less of them is in progress */
  return (alldone);							/* tell caller if we're all done */
}

/************************************************************************/
/*									*/
/*  Increment i/o channel refcount					*/
/*									*/
/*    Input:								*/
/*									*/
/*	iochan = i/o channel pointer					*/
/*	inc = 1 : increment reference count				*/
/*	     -1 : decrement reference count				*/
/*	      0 : no-op							*/
/*									*/
/*	smp lock <= dv, but at least softint				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_iochan_increfc = new ref count				*/
/*									*/
/************************************************************************/

Long oz_knl_iochan_increfc (OZ_Iochan *iochan, Long inc)

{
  int repeat;
  Long refc;
  uLong dv;
  OZ_Devunit *devunit;
  OZ_Iochan *nextiochan;
  int (*deassign) (OZ_Devunit *devunit, void *devex, OZ_Iochan *iochan, void *chnex);

  OZ_KNL_CHKOBJTYPE (iochan, OZ_OBJTYPE_IOCHAN);

  /* Add the value to reference count and save the new value    */
  /* Do it atomically if it is staying .gt. zero for efficiency */

again:
  do {
    refc = iochan -> refcount;							// sample current refcount
    if (refc + inc <= 0) goto going_le_zero;					// if going .le. 0, get lock first
  } while (!oz_hw_atomic_setif_long (&(iochan -> refcount), refc + inc, refc));	// write new value if still has sampled value
  return (refc + inc);

  /* If going zero, do it with smplock first so refcount going zero and channel getting freed is one locked operation     */
  /* This will keep someone from finding the channel in the devunit's list and incrementing it after we decide to free it */

going_le_zero:
  if (refc + inc < 0) oz_crash ("oz_knl_iochan_increfc: iochan %p -> refcount %d+%d", iochan, refc, inc);
  dv = oz_hw_smplock_wait (&oz_s_smplock_dv);				/* lock device database */
  if (!oz_hw_atomic_setif_long (&(iochan -> refcount), 0, refc)) {	/* set chan ref count to zero */
    oz_hw_smplock_clr (&oz_s_smplock_dv, dv);				/* - it changed, unlock */
    goto again;								/*   and do it all over again */
  }
  refc = 0;								/* we just set its refcount to zero */

  /* Call the device unit's deassign routine if there is one.  We are guaranteed there    */
  /* are no I/O's pending on this channel because I/O's increment the channel's refcount. */

  devunit  = iochan -> devunit;						/* point to the device unit block */
  deassign = devunit -> functable -> deassign;				/* see if there is any deassign routine */
  if ((iochan -> calldeas) && (deassign != NULL)) {
    do {
      OZ_HW_ATOMIC_INCBY1_LONG (iochan -> refcount);			/* ok, increment channel refcount, supposedly to one, */
									/* so the channel will stay alive with lock released */
      oz_hw_smplock_clr (&oz_s_smplock_dv, dv);				/* release lock during the driver's deassign routine */
      repeat = (*deassign) (devunit, devunit -> devex, iochan, iochan -> chnex); /* call driver's deassign routine (maybe it does i/o on channel to close it up) */
      dv = oz_hw_smplock_wait (&oz_s_smplock_dv);			/* decrement reference count back, quite possibly to zero */
      refc = oz_hw_atomic_inc_long (&(iochan -> refcount), -1);
      if (refc < 0) oz_crash ("oz_knl_iochan_increfc: channel refcount went negative after driver deassign routine");
      if (refc > 0) goto rtn;						/* maybe deassign routine started some i/o that isn't done yet */
    } while (repeat);							/* repeat if deassign says to */
									/* - this is useful where the deassign routine starts an async I/O on the same channel */
									/*   which completes very quickly (perhaps on another cpu), and the I/O channel ref count */
									/*   gets decremented back down to zero, but the deassign routine needs to be called back */
									/*   to finish up */
  }

  /* Unlink I/O chan from devunit and decrement device unit reference count - maybe free it if ref count went zero */

  if (OZ_LOCK_ALLOW_TEST (iochan -> lockmode,  OZ_LOCK_ALLOWS_SELF_READ))    devunit -> refc_read --;
  if (OZ_LOCK_ALLOW_TEST (iochan -> lockmode,  OZ_LOCK_ALLOWS_SELF_WRITE))   devunit -> refc_write --;
  if (!OZ_LOCK_ALLOW_TEST (iochan -> lockmode, OZ_LOCK_ALLOWS_OTHERS_READ))  devunit -> deny_read --;
  if (!OZ_LOCK_ALLOW_TEST (iochan -> lockmode, OZ_LOCK_ALLOWS_OTHERS_WRITE)) devunit -> deny_write --;
  nextiochan = iochan -> next;
  *(iochan -> prev) = nextiochan;
  if (nextiochan != NULL) nextiochan -> prev = iochan -> prev;
  oz_knl_devunit_increfc (devunit, -1);

  /* Release security attributes */

  oz_knl_secattr_increfc (iochan -> secattr, -1);

  /* Free off the I/O channel block */

  OZ_KNL_NPPFREE (iochan);

rtn:
  oz_hw_smplock_clr (&oz_s_smplock_dv, dv);
  return (refc);
}

/************************************************************************/
/*									*/
/*  Print out the device list to the console (for debugging)		*/
/*									*/
/************************************************************************/

void oz_knl_devdump (int verbose)

{
  char aliastr[16];
  int i, j;
  uLong dv;
  OZ_Devclass *devclass;
  OZ_Devdriver *devdriver;
  OZ_Devunit *devunit;
  OZ_Iochan *iochan;

  dv = oz_hw_smplock_wait (&oz_s_smplock_dv);

  /* Non-verbose mode is used during startup so make it look nice */

  if (!verbose) {
    i = 0;
    j = 0;
    for (devclass = devclasses; devclass != NULL; devclass = devclass -> next) {
      for (devdriver = devclass -> devdrivers; devdriver != NULL; devdriver = devdriver -> next) {
        for (devunit = devdriver -> devunits; devunit != NULL; devunit = devunit -> next) {
          if (devunit -> unitname_l > i) i = devunit -> unitname_l;
          oz_sys_sprintf (sizeof aliastr, aliastr, "%c%s%u", OZ_DEVUNIT_ALIAS_CHAR, devclass -> classname, devunit -> aliasno);
          if (strlen (aliastr) > j) j = strlen (aliastr);
        }
      }
    }
    for (devclass = devclasses; devclass != NULL; devclass = devclass -> next) {
      for (devdriver = devclass -> devdrivers; devdriver != NULL; devdriver = devdriver -> next) {
        for (devunit = devdriver -> devunits; devunit != NULL; devunit = devunit -> next) {
          oz_sys_sprintf (sizeof aliastr, aliastr, "%c%s%u", OZ_DEVUNIT_ALIAS_CHAR, devclass -> classname, devunit -> aliasno);
          oz_knl_printk ("  %*s %-*s: %s\n", i, devunit -> unitname, j, aliastr, devunit -> unitdesc);
        }
      }
    }
  }

  /* Verbose mode is used in diag mode only */

  else {
    for (devclass = devclasses; devclass != NULL; devclass = devclass -> next) {
      oz_knl_printk ("devclass <%s>:\n", devclass -> classname);
      for (devdriver = devclass -> devdrivers; devdriver != NULL; devdriver = devdriver -> next) {
        oz_knl_printk ("  devdriver <%s>\n", devdriver -> drivername);
        for (devunit = devdriver -> devunits; devunit != NULL; devunit = devunit -> next) {
          oz_knl_printk ("    devunit <%s>\n", devunit -> unitname);
          oz_knl_printk ("       ioopendcount %d\n", devunit -> ioopendcount);
          oz_knl_printk ("           unitdesc '%s'\n", devunit -> unitdesc);
          oz_knl_printk ("           refcount %d\n", devunit -> refcount);
          oz_knl_printk ("             cloned %d\n", devunit -> cloned);
          oz_knl_printk ("          functable %p\n", devunit -> functable);
          oz_knl_printk ("            opcount %d\n", (uLong)(devunit -> opcount));
          for (iochan = devunit -> iochans; iochan != NULL; iochan = iochan -> next) {
            oz_knl_printk ("      iochan %p: procmode %d, lockmode %d, refcount %d\n", iochan, iochan -> procmode, iochan -> lockmode, iochan -> refcount);
          }
        }
      }
    }
  }

  oz_hw_smplock_clr (&oz_s_smplock_dv, dv);
}
