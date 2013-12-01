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
/*  Scsi utility routines used by drivers				*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_dev_scsi.h"
#include "oz_io_scsi.h"
#include "oz_knl_devio.h"
#include "oz_knl_event.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_lock.h"
#include "oz_knl_section.h"
#include "oz_knl_status.h"
#include "oz_knl_thread.h"

#define INQSIZ 56

typedef struct Classdriver Classdriver;
struct Classdriver { Classdriver *next;
                     OZ_Scsi_init_entry entry;
                     void *param;
                   };

static Classdriver *classdrivers = NULL;
static int class_modseq = 0;
static OZ_Event *volatile class_event = NULL;
static OZ_Thread *volatile class_thread = NULL;

static int lock_class_list (void);
static void unlock_class_list (int unlockit);

/************************************************************************/
/*									*/
/*  This routine converts an scsi_doio to an scsi_doiopp request	*/
/*									*/
/*    Input:								*/
/*									*/
/*	procmode = processor mode of the I/O buffers			*/
/*	doio = pointer to input scsi_doio block				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_dev_scsi_cvtdoio2pp = OZ_SUCCESS : conversion successful	*/
/*	                               else : error status		*/
/*	*doiopp = filled in with equivalent request			*/
/*									*/
/*    Note:								*/
/*									*/
/*	This routine does not probe or lock the datarlen or status 	*/
/*	return locations.						*/
/*									*/
/************************************************************************/

uLong oz_dev_scsi_cvtdoio2pp (OZ_Ioop *ioop, OZ_Procmode procmode, OZ_IO_scsi_doio *doio, OZ_IO_scsi_doiopp *doiopp)

{
  uLong sts;

  memset (doiopp, 0, sizeof *doiopp);

  doiopp -> cmdlen   = doio -> cmdlen;
  doiopp -> cmdbuf   = doio -> cmdbuf;
  doiopp -> datasize = doio -> datasize;
  doiopp -> optflags = doio -> optflags;
  doiopp -> cmpflags = doio -> cmpflags;
  doiopp -> status   = doio -> status;
  doiopp -> datarlen = doio -> datarlen;
  doiopp -> timeout  = doio -> timeout;

  /* Probe and lock I/O buffers in memory */

  sts = oz_knl_ioop_lockr (ioop, doiopp -> cmdlen, doiopp -> cmdbuf, NULL, NULL, NULL);
  if ((sts == OZ_SUCCESS) && (doiopp -> datasize != 0)) {
    sts = oz_knl_ioop_lock (ioop, doio -> datasize, doio -> databuff, 
                            (doio -> optflags & OZ_IO_SCSI_OPTFLAG_WRITE) != 0, 
                            &(doiopp -> dataphypages), NULL, &(doiopp -> databyteoffs));
  }

  /* Return status */

  return (sts);
}

/************************************************************************/
/*									*/
/*  This routine is called by an scsi controller driver's init routine 	*/
/*  when it has finished initializing the controller.  It scans the 	*/
/*  controller's scsi bus for devices and calls the appropriate class 	*/
/*  level driver.							*/
/*									*/
/*    Input:								*/
/*									*/
/*	crtl_devunit  = points to controller device unit struct		*/
/*	max_scsi_id   = max allowed scsi id on the bus (exclusive)	*/
/*	smplock level = softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_dev_scsi_init called for each device found			*/
/*									*/
/*    Note:								*/
/*									*/
/*	As an alternative, the init routine can declare an autogen 	*/
/*	routine which would call oz_dev_scsi_scan1 if/when someone 	*/
/*	actually wants to access the device.				*/
/*									*/
/************************************************************************/

void oz_dev_scsi_scan (OZ_Devunit *ctrl_devunit, uLong max_scsi_id)

{
  const char *ctrl_devname;
  uLong scsi_id;

  ctrl_devname = oz_knl_devunit_devname (ctrl_devunit);
  oz_knl_printk ("oz_dev_scsi_scan: scanning %s\n", ctrl_devname);

  for (scsi_id = 0; scsi_id < max_scsi_id; scsi_id ++) {
    oz_dev_scsi_scan1 (ctrl_devunit, scsi_id);
  }

  oz_knl_printk ("oz_dev_scsi_scan: scanned %s\n", ctrl_devname);
}

/************************************************************************/
/*									*/
/*  This is an autogen routine that scsi controller drivers can use.  	*/
/*  It gets called when someone tries to access a particular device on 	*/
/*  a scsi bus that hasn't been set up yet.  It probes the scsi bus 	*/
/*  for that particular device then calls the class driver to init it.	*/
/*  It assumes the class driver will set up the default name for the 	*/
/*  device, ie, '<controller_name>.<scsi_id>'.				*/
/*									*/
/*    Input:								*/
/*									*/
/*	host_devunit = points to scsi controller devunit		*/
/*	devname = device name that is to be created			*/
/*	suffix = part of devname that follows controller name		*/
/*	         for our purposes, it points at the scsi_id		*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_dev_scsi_auto = NULL : no device created			*/
/*	                   else : pointer to created device		*/
/*									*/
/*    Note:								*/
/*									*/
/*	scsi controller driver calls ...				*/
/*	  oz_knl_devunit_autogen (devunit, oz_dev_scsi_auto, NULL)	*/
/*	... in its init routine to enable				*/
/*									*/
/************************************************************************/

OZ_Devunit *oz_dev_scsi_auto (void *dummy, OZ_Devunit *host_devunit, const char *devname, const char *suffix)

{
  int i;
  OZ_Devunit *devunit;
  uLong scsi_id;

  scsi_id = oz_hw_atoi (suffix, &i);					/* get the scsi-id number */
  if ((i == 0) || ((suffix[i] != 0) && (suffix[i] != '.'))) return (NULL); /* ... must end there or with a dot */
  devunit = oz_dev_scsi_scan1 (host_devunit, scsi_id);			/* scan for the device on the bus */
  return (devunit);							/* return pointer if we created device */
}

/************************************************************************/
/*									*/
/*  This routine is called to probe the scsi bus for a particular unit	*/
/*									*/
/*    Input:								*/
/*									*/
/*	crtl_devunit  = points to controller device unit struct		*/
/*	scsi_id       = the scsi id on the bus to check			*/
/*	smplock level = softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_dev_scsi_scan1 = NULL : nothing found there			*/
/*	                    else : devunit pointer created		*/
/*									*/
/************************************************************************/

OZ_Devunit *oz_dev_scsi_scan1 (OZ_Devunit *ctrl_devunit, uLong scsi_id)

{
  char unit_devdesc[OZ_DEVUNIT_DESCSIZE], unit_devname[OZ_DEVUNIT_NAMESIZE];
  Classdriver *classdriver;
  const char *ctrl_devname;
  int i, j;
  OZ_Devunit *devunit;
  OZ_IO_scsi_doio scsi_doio;
  OZ_IO_scsi_open scsi_open;
  OZ_Iochan *ctrl_iochan;
  OZ_Scsi_init_pb pb;
  uByte *inqbuf, scsist;
  uLong inqlen, sts;

  static const uByte inqcmd[] = { 0x12, 0, 0, 0, INQSIZ, 0 };

  ctrl_devname = oz_knl_devunit_devname (ctrl_devunit);
  devunit = NULL;
  inqbuf  = OZ_KNL_NPPMALLOC (INQSIZ);

  /* Assign an I/O channel to the controller */

  sts = oz_knl_iochan_create (ctrl_devunit, OZ_LOCKMODE_CW, OZ_PROCMODE_KNL, NULL, &ctrl_iochan);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_dev_scsi_scan: error %u assigning channel to %s for unit %u\n", sts, ctrl_devname, scsi_id);
    goto delevent;
  }

  /* Open channel to the scsi_id on it and only this channel can write */
  /* Allow others to read (so they can do getinfo's, etc)              */

  memset (&scsi_open, 0, sizeof scsi_open);
  scsi_open.scsi_id  = scsi_id;
  scsi_open.lockmode = OZ_LOCKMODE_PW;

  sts = oz_knl_io (ctrl_iochan, OZ_IO_SCSI_OPEN, sizeof scsi_open, &scsi_open);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_dev_scsi_scan: error %u opening channel to %s unit %u\n", sts, ctrl_devname, scsi_id);
    goto deaschan;
  }

  /* Do an 'IDENTIFY' command to see what's there (if anything) -       */
  /* All scsi devices are supposed to be able to process this command   */
  /* quickly and successfully even if they're busy doing something else */

  memset (&scsi_doio, 0, sizeof scsi_doio);
  scsi_doio.cmdlen   = sizeof inqcmd;
  scsi_doio.cmdbuf   = inqcmd;
  scsi_doio.datasize = INQSIZ;
  scsi_doio.databuff = inqbuf;
  scsi_doio.status   = &scsist;
  scsi_doio.datarlen = &inqlen;
  scsi_doio.timeout  = 3000;

  memset (inqbuf, 0, INQSIZ);
  sts = oz_knl_io (ctrl_iochan, OZ_IO_SCSI_DOIO, sizeof scsi_doio, &scsi_doio);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_dev_scsi_scan: error %u identifying unit %u on %s\n", sts, scsi_id, ctrl_devname);
    goto deaschan;
  }
  if ((scsist & 0x3E) != 0) {
    oz_knl_printk ("oz_dev_scsi_scan: scsi sts %2.2x identifying unit %u on %s\n", scsist, scsi_id, ctrl_devname);
    goto deaschan;
  }

  /* Now make up a device name from <controller_devname>.<scsi_id> */

  oz_sys_sprintf (sizeof unit_devname, unit_devname, "%s.%u", ctrl_devname, scsi_id);

  /* Make up a device description from the manufacturer and model number strings from the inquiry */

#if OZ_DEVUNIT_DESCSIZE < 25
  error : code assumes sizeof unitdesc > 24
#endif
  for (i =  8; i > 0; -- i) if (inqbuf[i+ 7] != ' ') break;
  for (j = 16; j > 0; -- j) if (inqbuf[j+15] != ' ') break;
  memcpy (unit_devdesc, inqbuf + 8, i);
  unit_devdesc[i++] = ' ';
  memcpy (unit_devdesc + i, inqbuf + 16, j);
  unit_devdesc[i+j] = 0;

  /* Ok, see what class driver will process it, if any - */
  /* If a driver wants it, it returns pointer to devunit */

  pb.ctrl_iochan  = ctrl_iochan;
  pb.inqlen       = inqlen;
  pb.inqbuf       = inqbuf;
  pb.unit_devname = unit_devname;
  pb.unit_devdesc = unit_devdesc;

  i = lock_class_list ();									// prevent other threads from changing list
rescan:
  j = class_modseq;										// in case class driver modifies list
  for (classdriver = classdrivers; classdriver != NULL; classdriver = classdriver -> next) {	// scan class driver list
    devunit = (*(classdriver -> entry)) (classdriver -> param, &pb);				// try a class driver
    if (devunit != NULL) {
      unlock_class_list (i);									// it's handling it, unlock list
      oz_knl_printk ("oz_dev_scsi_scan: defined %s (%s)\n", unit_devname, unit_devdesc);	// ... output message
      goto delevent;										// ... and we're done
    }
    if (class_modseq != j) goto rescan;								// re-scan if it changed list
  }
  unlock_class_list (i);									// not handled, unlock list

  /* Didn't create device, so close channel.  Someone will have to manually load a driver that can handle the device. */

deaschan:
  oz_knl_iochan_increfc (ctrl_iochan, -1);

delevent:
  OZ_KNL_NPPFREE (inqbuf);
  return (devunit);
}

/************************************************************************/
/*									*/
/*  Add scsi class driver						*/
/*									*/
/************************************************************************/

void oz_dev_scsi_class_add (OZ_Scsi_init_entry entry, void *param)

{
  Classdriver *classdriver;
  int cl;

  classdriver = OZ_KNL_NPPMALLOC (sizeof *classdriver);
  classdriver -> entry = entry;
  classdriver -> param = param;
  cl = lock_class_list ();
  classdriver -> next = classdrivers;
  classdrivers = classdriver;
  class_modseq ++;
  unlock_class_list (cl);
}

/************************************************************************/
/*									*/
/*  Remove scsi class driver						*/
/*									*/
/************************************************************************/

void oz_dev_scsi_class_rem (OZ_Scsi_init_entry entry, void *param)

{
  Classdriver *classdriver, **lclassdriver;
  int cl;

  cl = lock_class_list ();
  for (lclassdriver = &classdrivers; (classdriver = *lclassdriver) != NULL;) {
    if ((classdriver -> entry == entry) && (classdriver -> param == param)) {
      *lclassdriver = classdriver -> next;
      OZ_KNL_NPPFREE (classdriver);
    } else {
      lclassdriver = &(classdriver -> next);
    }
  }
  class_modseq ++;
  unlock_class_list (cl);
}

/************************************************************************/
/*									*/
/*  Lock scsi class driver list						*/
/*									*/
/************************************************************************/

static int lock_class_list (void)

{
  uLong sts;
  OZ_Event *newevent;
  OZ_Thread *cur_thread;

  cur_thread = oz_knl_thread_getcur ();

  if (class_event == NULL) {
    sts = oz_knl_event_create (22, "oz_dev_scsi class list", NULL, &newevent);
    if (sts != OZ_SUCCESS) oz_crash ("oz_dev_scsi lock_class_list: error %u creating event flag", sts);
    if (oz_hw_atomic_setif_ptr (&class_event, newevent, NULL)) goto gotit;
    oz_knl_event_increfc (newevent, -1);
  }

  while (oz_knl_event_set (class_event, 0) == 0) {
    if (class_thread == cur_thread) return (0);
    oz_knl_event_waitone (class_event);
  }

gotit:
  class_thread = cur_thread;
  return (1);
}

/************************************************************************/
/*									*/
/*  Unlock scsi class driver list					*/
/*									*/
/************************************************************************/

static void unlock_class_list (int unlockit)

{
  if (unlockit) {
    class_thread = NULL;
    oz_knl_event_set (class_event, 1);
  }
}
