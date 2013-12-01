//+++2001-11-18
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
//---2001-11-18

/************************************************************************/
/*									*/
/*  Pseudo console port driver						*/
/*									*/
/*  User (or kernel) mode programs, like telnetd, use this driver to 	*/
/*  create console-like devices.					*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_knl_devio.h"
#include "oz_knl_hw.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_logon.h"
#include "oz_knl_sdata.h"
#include "oz_knl_status.h"
#include "oz_knl_thread.h"
#include "oz_io_comport.h"
#include "oz_io_conpseudo.h"
#include "oz_io_console.h"
#include "oz_sys_xprintf.h"

#define SENDDATQMAX 5	/* max number of requests to allow in senddatq */

typedef struct Chnex Chnex;
typedef struct Devex Devex;
typedef struct Eventdat Eventdat;
typedef struct Gsreq Gsreq;
typedef struct Iopex Iopex;
typedef struct Senddat Senddat;

struct Chnex { uByte class_area[1];			/* class driver's area */
             };

struct Devex { OZ_Iochan *classiochan;			/* console class I/O channel (assigned to devunit "conclass") */
               OZ_IO_comport_setup comport_setup;	/* console port setup parameters */
               OZ_Event *lockevent;			/* database locking event flag */
               OZ_Thread *lockthread;			/* thread that has database locked */
               int terminated;				/* we have gotten a terminate call from the class driver */

               Iopex *sendreqqh, **sendreqqt;		/* queue of OZ_IO_CONPSEUDO_GETSCRDATA requests waiting for screen data */
               Senddat *senddatqh, **senddatqt;		/* queue of screen data waiting for OZ_IO_CONPSEUDO_GETSCRDATA requests */
               int senddatqs;				/* number of requests in senddatq */

               Iopex *eventreqqh, **eventreqqt;
               Eventdat *eventdatqh, **eventdatqt;

               Gsreq *gsreqh, **gsreqt;			/* list of pending GETMODE/SETMODE requests */

               OZ_Iochan *masteriochan;			/* master I/O channel - allowed to do OZ_IO_CONPSEUDO_... calls */
							/* when telnetd (or whoever) deassigns this channel, the device gets shut down */

               OZ_Devfunc *port_functab;		/* copy of function table (pt_functable) with xxx_exsize's modified for class driver */
             };

struct Eventdat { Eventdat *next;			/* next in eventdatq */
                  OZ_Conpseudo_event event;		/* event number */
                };

struct Gsreq { Gsreq *next;				/* next in gsreqh/t */
               void *param;				/* param to return to class driver on completion */
               uLong size;				/* size of mode buffer */
               int fetched;				/* 0: waiting to be fetched; 1: waiting to be posted */
               OZ_Console_modebuff *buff;		/* address of mode buffer */
             };

struct Iopex { Iopex *next;
               OZ_Ioop *ioop;
               Devex *devex;

               union { struct { uLong size;		/* size of user's buffer */
                                char *buff;		/* address of user's buffer */
                                uLong *rlen;		/* where to return length to display */
                              } getscrdata;

                       struct { Eventdat *eventdat;	/* corresponding event data */
                                OZ_Conpseudo_event *event; /* where to return event data */
                              } getevent;
                     } u;

               uByte class_area[1];			/* class driver's area */
             };

struct Senddat { Senddat *next;				/* next in senddatq */
                 uLong size;				/* size of data to display */
                 char *buff;				/* address of data to display */
                 void *write_param;			/* parameter to pass to display complete routine */
               };

static uLong pt_clonecre (OZ_Devunit *template_devunit, void *template_devex, int template_cloned, OZ_Procmode procmode, OZ_Devunit **cloned_devunit);
static int pt_clonedel (OZ_Devunit *cloned_devunit, void *devexv, int cloned);
static uLong pt_assign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode);
static int pt_deassign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv);
static void pt_abort (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Ioop *ioop, void *iopexv, OZ_Procmode procmode);
static uLong pt_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode, 
                       OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap);

static const OZ_Devfunc pt_functable = { sizeof (Devex), sizeof (Chnex), sizeof (Iopex), 0, 
                                         NULL, pt_clonecre, pt_clonedel, pt_assign, pt_deassign, pt_abort, pt_start, NULL };

static int initialized = 0;
static OZ_Devclass  *devclass   = NULL;
static OZ_Devdriver *devdriver  = NULL;
static OZ_Devunit   *devunit    = NULL;

static uLong io_pt_setup (OZ_Ioop *ioop, OZ_Devunit *devunit, Devex *devex, const char *portname, const char *classname);
static uLong io_pt_getscrdata (Devex *devex, Iopex *iopex, uLong size, char *buff, uLong *rlen);
static uLong io_pt_putkbddata (Devex *devex, uLong size, const char *buff);
static void kbd_rah_full (void *devexv, int full);
static uLong getsetmode (void *devexv, void *getset_param, uLong size, OZ_Console_modebuff *buff);
static uLong io_pt_getevent (Devex *devex, Iopex *iopex, OZ_Conpseudo_event *event);
static uLong io_pt_fetchgsmodereq (Devex *devex, OZ_IO_conpseudo_fetchgsmodereq *pt_fetchgsmodereq);
static uLong io_pt_postgsmodereq (Devex *devex, OZ_IO_conpseudo_postgsmodereq *pt_postgsmodereq);
static void terminate (void *devexv);
static void read_start (void *devexv, int start);
static uLong disp_start (void *devexv, void *write_param, uLong size, char *buff);
static void sendreq_iodone (void *sendreqv, int finok, uLong *status_r);
static void disp_suspend (void *devexv, int suspend);
static void gotevent (Devex *devex, OZ_Conpseudo_event event);
static void event_iodone (void *eventreqv, int finok, uLong *status_r);
static uLong lockdb (void *devexv);
static void unlkdb (void *devexv, uLong iplsav);

/************************************************************************/
/*									*/
/*  Boot-time initialization routine					*/
/*									*/
/************************************************************************/

void oz_dev_conpseudo_init ()

{
  if (!initialized) {
    initialized = 1;
    oz_knl_printk ("oz_dev_conpseudo_init\n");
    devclass  = oz_knl_devclass_create (OZ_IO_CONPSEUDO_CLASSNAME, OZ_IO_CONPSEUDO_BASE, OZ_IO_CONPSEUDO_MASK, "oz_dev_conpseudo");
    devdriver = oz_knl_devdriver_create (devclass, "oz_dev_conpseudo");
    devunit   = oz_knl_devunit_create (devdriver, OZ_IO_CONPSEUDO_DEV, "pseudo console template", &pt_functable, 0, oz_s_secattr_tempdev);
  }
}

/************************************************************************/
/*									*/
/*  A device is to be created.  This is done by a program such as 	*/
/*  telnetd assigning a channel to the template device.			*/
/*									*/
/*  This routine runs with the dv smplock set				*/
/*									*/
/*  We make a copy of the template device and give it a temporary 	*/
/*  name.  Also, we init the devex area with a functab and lockevent.	*/
/*									*/
/************************************************************************/

static uLong pt_clonecre (OZ_Devunit *template_devunit, void *template_devex, int template_cloned, OZ_Procmode procmode, OZ_Devunit **cloned_devunit)

{
  char unitname[OZ_DEVUNIT_NAMESIZE];
  Devex *devex;
  OZ_Devfunc *functab;
  OZ_Devunit *devunit;
  OZ_Event *lockevent;
  OZ_Secattr *secattr;
  uLong sts;

  static uLong seq = 0;

  if (template_cloned) {									/* maybe someone is assigning to one of the clones */
    *cloned_devunit = template_devunit;								/* if so, just use the device as it is */
    oz_knl_devunit_increfc (template_devunit, 1);						/* ... but increment the unit's ref count */
  } else {
    oz_sys_sprintf (sizeof unitname, unitname, "%s.%u", oz_knl_devunit_devname (template_devunit), ++ seq);
    functab = OZ_KNL_NPPMALLOQ (sizeof *functab);						/* create a copy of the function table for it */
    if (functab == NULL) return (OZ_EXQUOTANPP);
    sts = oz_knl_event_create (strlen (unitname), unitname, NULL, &lockevent);			/* get an event flag set up for locking the database */
    if (sts != OZ_SUCCESS) {
      OZ_KNL_NPPFREE (functab);
      return (sts);
    }
    *functab = pt_functable;
    secattr  = oz_knl_thread_getdefcresecattr (NULL);
    devunit  = oz_knl_devunit_create (devdriver, unitname, "not set up", functab, 1, secattr);	/* create the cloned device */
    oz_knl_secattr_increfc (secattr, -1);
    devex    = oz_knl_devunit_ex (devunit);							/* point to its extension */
    memset (devex, 0, sizeof *devex);								/* clear out the extension */
    *cloned_devunit       = devunit;								/* return pointer to new device */
    devex -> port_functab = functab;								/* remember where function table is */
    devex -> lockevent    = lockevent;								/* mark database 'unlocked' */
    oz_knl_event_set (lockevent, 1);
  }
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  All channels, including the telnetd channel, have been deassigned.	*/
/*  The device will be deleted from the system iff a 1 is returned.	*/
/*									*/
/*  This routine runs with the dv smplock set				*/
/*									*/
/************************************************************************/

static int pt_clonedel (OZ_Devunit *cloned_devunit, void *devexv, int cloned)

{
  Devex *devex;

  devex = devexv;

  if (cloned) {
    OZ_KNL_NPPFREE (devex -> port_functab);		/* free off the functab copy */
    oz_knl_event_increfc (devex -> lockevent, -1);	/* free off the database locking event flag */
    devex -> port_functab = NULL;
    devex -> lockevent    = NULL;
  }

  return (cloned);					/* delete only 'cloned' devices - don't delete the template */
}

/************************************************************************/
/*									*/
/*  An channel was just assigned to the 'pt' device			*/
/*									*/
/*  For us, it is a nop, but the class driver might want to hear about 	*/
/*  it.									*/
/*									*/
/************************************************************************/

static uLong pt_assign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode)

{
  Devex *devex;

  devex = devexv;

  if (devex -> classiochan == NULL) return (OZ_SUCCESS);
  return ((*(devex -> comport_setup.class_functab -> assign)) (devunit, devex -> comport_setup.class_devex, iochan, ((Chnex *)chnexv) -> class_area, procmode));
}

/************************************************************************/
/*									*/
/*  Channel to device is being deassigned 				*/
/*									*/
/************************************************************************/

static int pt_deassign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv)

{
  Devex *devex;
  Eventdat *eventdat;
  Gsreq *gsreq;
  Iopex *iopex;
  uLong db;
  OZ_Iochan *classiochan;
  Senddat *senddat;

  devex = devexv;

  /* If not set up at all, just return 'kill it now' status */

  if (devex -> classiochan == NULL) return (0);

  /* If it is not the 'master channel' just pass it to the class driver.  We don't do the */
  /* master channel because the assign routine did not get called for the master channel. */

  if (iochan != devex -> masteriochan) {
    return ((*(devex -> comport_setup.class_functab -> deassign)) (devunit, devex -> comport_setup.class_devex, iochan, ((Chnex *)chnexv) -> class_area));
  }

  /* Master channel, process it internally - we have to undo everything the io_pt_setup call did as well as any I/O's that are going on */

  db = lockdb (devex);							/* lock the database */
  oz_knl_devunit_rename (devunit, NULL, "master channel deassigned");

  /* Close the console class device - we must not call any of the class_ setup */
  /* routines anymore, and it must not call any of our port_ routines anymore  */

  classiochan = devex -> classiochan;					/* get iochan to 'conclass' device */
  if (devex -> classiochan != NULL) {					/* see if it was ever set up (via OZ_IO_CONPSEUDO_SETUP) */
    devex -> classiochan = NULL;					/* this is our indicator to not call anymore class_ setup routines */
    oz_knl_iochan_increfc (classiochan, -1);				/* close the channel - this aborts all application I/O's to the console device */
    memset (&(devex -> comport_setup), 0, sizeof devex -> comport_setup); /* make sure we can't call the class_ setup routines anymore */
  }

  /* Abort all OZ_IO_CONPSEUDO_GETSCRDATA and OZ_IO_CONPSEUDO_GETEVENT requests */

  devex -> sendreqqt = NULL;						/* anything what tries to queue with this will crash */
  while ((iopex = devex -> sendreqqh) != NULL) {			/* abort any I/O's remaining in the queue */
    devex -> sendreqqh = iopex -> next;
    oz_knl_iodone (iopex -> ioop, OZ_ABORTED, NULL, NULL, NULL);
  }

  devex -> eventreqqt = NULL;						/* ditto */
  while ((iopex = devex -> eventreqqh) != NULL) {
    devex -> eventreqqh = iopex -> next;
    oz_knl_iodone (iopex -> ioop, OZ_ABORTED, NULL, NULL, NULL);
  }

  /* Free off any left over data and left over events */

  devex -> senddatqt = NULL;
  while ((senddat = devex -> senddatqh) != NULL) {
    devex -> senddatqh = senddat -> next;
    devex -> senddatqs --;
    OZ_KNL_PGPFREE (senddat);
  }

  devex -> eventdatqt = NULL;
  while ((eventdat = devex -> eventdatqh) != NULL) {
    devex -> eventdatqh = eventdat -> next;
    OZ_KNL_PGPFREE (eventdat);
  }

  devex -> gsreqt = NULL;
  while ((gsreq = devex -> gsreqh) != NULL) {
    devex -> gsreqh = gsreq -> next;
    OZ_KNL_PGPFREE (gsreq);
  }

  /* We don't have an master anymore.  Unlock database */

  devex -> masteriochan = NULL;
  unlkdb (devex, db);

  return (0);
}

/************************************************************************/
/*									*/
/*  Abort any requests to the device					*/
/*									*/
/************************************************************************/

static void pt_abort (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Ioop *ioop, void *iopexv, OZ_Procmode procmode)

{
  Devex *devex;
  Iopex *iopex, **liopex;
  uLong db;

  devex = devexv;							/* get pointer to the devex struct */
  db = lockdb (devex);							/* lock the database */

  /* See if it is the master I/O channel (ie, the one something like telnetd is using) */

  if (iochan == devex -> masteriochan) {

    /* Abort all OZ_IO_CONPSEUDO_GETSCRDATA requests */

    for (liopex = &(devex -> sendreqqh); (iopex = *liopex) != NULL;) {
      if (!oz_knl_ioabortok (iopex -> ioop, iochan, procmode, ioop)) liopex = &(iopex -> next);
      else {
        *liopex = iopex -> next;
        oz_knl_iodone (iopex -> ioop, OZ_ABORTED, NULL, NULL, NULL);
      }
    }
    devex -> sendreqqt = liopex;

    /* Abort all OZ_IO_CONPSEUDO_GETEVENT requests */

    for (liopex = &(devex -> eventreqqh); (iopex = *liopex) != NULL;) {
      if (!oz_knl_ioabortok (iopex -> ioop, iochan, procmode, ioop)) liopex = &(iopex -> next);
      else {
        *liopex = iopex -> next;
        oz_knl_iodone (iopex -> ioop, OZ_ABORTED, NULL, NULL, NULL);
      }
    }
    devex -> eventreqqt = liopex;
  }

  /* Not master, call the class driver (if any) */

  else if (devex -> classiochan != NULL) {
    iopex = iopexv;
    (*(devex -> comport_setup.class_functab -> abort)) (devunit, devex -> comport_setup.class_devex, iochan, ((Chnex *)chnexv) -> class_area, 
                                                        ioop, (iopex == NULL) ? NULL : iopex -> class_area, procmode);
  }

  unlkdb (devex, db);
}

/************************************************************************/
/*									*/
/*  An new I/O request is to be processed				*/
/*									*/
/************************************************************************/

static uLong pt_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode, 
                       OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap)

{
  char classname[OZ_DEVUNIT_NAMESIZE], portname[OZ_DEVUNIT_NAMESIZE];
  Devex *devex;
  Iopex *iopex;
  uLong db, sts;

  devex = devexv;
  iopex = iopexv;

  iopex -> next  = NULL;
  iopex -> ioop  = ioop;
  iopex -> devex = devex;

  switch (funcode) {

    /* Set up the device */

    case OZ_IO_CONPSEUDO_SETUP: {
      OZ_IO_conpseudo_setup pt_setup;

      movc4 (as, ap, sizeof pt_setup, &pt_setup);
      sts = oz_knl_section_ugetz (procmode, sizeof portname, pt_setup.portname, portname, NULL);
      if (sts == OZ_SUCCESS) oz_knl_section_ugetz (procmode, sizeof classname, pt_setup.classname, classname, NULL);
      if (sts == OZ_SUCCESS) {
        db = lockdb (devex);
        sts = io_pt_setup (ioop, devunit, devex, portname, classname);	/* do all the hard work */
        if (sts == OZ_SUCCESS) devex -> masteriochan = iochan;		/* if successful, remember who set it up */
        unlkdb (devex, db);
      }
      return (sts);
    }

    /* Get some data from the class driver to be displayed on the screen */

    case OZ_IO_CONPSEUDO_GETSCRDATA: {
      OZ_IO_conpseudo_getscrdata pt_getscrdata;

      sts = OZ_BADIOFUNC;
      if (iochan == devex -> masteriochan) {			/* only accessible to channel that set it up */
        movc4 (as, ap, sizeof pt_getscrdata, &pt_getscrdata);
        sts = oz_knl_ioop_lockw (ioop, pt_getscrdata.size, pt_getscrdata.buff, NULL, NULL, NULL);
        if (sts == OZ_SUCCESS) sts = oz_knl_ioop_lockw (ioop, sizeof *(pt_getscrdata.rlen), pt_getscrdata.rlen, NULL, NULL, NULL);
        if (sts == OZ_SUCCESS) {
          db = lockdb (devex);
          sts = io_pt_getscrdata (devex, iopex, pt_getscrdata.size, pt_getscrdata.buff, pt_getscrdata.rlen);
          unlkdb (devex, db);
        }
      }
      return (sts);
    }

    /* Put some data to the class driver that came from the keyboard */

    case OZ_IO_CONPSEUDO_PUTKBDDATA: {
      OZ_IO_conpseudo_putkbddata pt_putkbddata;

      sts = OZ_BADIOFUNC;
      if (iochan == devex -> masteriochan) {			/* only accessible to channel that set it up */
        movc4 (as, ap, sizeof pt_putkbddata, &pt_putkbddata);
        sts = oz_knl_ioop_lockr (ioop, pt_putkbddata.size, pt_putkbddata.buff, NULL, NULL, NULL);
        if (sts == OZ_SUCCESS) {
          db = lockdb (devex);
          sts = io_pt_putkbddata (devex, pt_putkbddata.size, pt_putkbddata.buff);
          unlkdb (devex, db);
        }
      }
      return (sts);
    }

    /* Get an event from the class driver */

    case OZ_IO_CONPSEUDO_GETEVENT: {
      OZ_IO_conpseudo_getevent pt_getevent;

      sts = OZ_BADIOFUNC;
      if (iochan == devex -> masteriochan) {			/* only accessible to channel that set it up */
        movc4 (as, ap, sizeof pt_getevent, &pt_getevent);
        sts = oz_knl_ioop_lockw (ioop, sizeof *(pt_getevent.event), pt_getevent.event, NULL, NULL, NULL);
        if (sts == OZ_SUCCESS) {
          db = lockdb (devex);
          sts = io_pt_getevent (devex, iopex, pt_getevent.event);
          unlkdb (devex, db);
        }
      }
      return (sts);
    }

    /* Fetch an get/setmode request from the queue */

    case OZ_IO_CONPSEUDO_FETCHGSMODEREQ: {
      OZ_IO_conpseudo_fetchgsmodereq pt_fetchgsmodereq;

      sts = OZ_BADIOFUNC;
      if (iochan == devex -> masteriochan) {			/* only accessible to channel that set it up */
        movc4 (as, ap, sizeof pt_fetchgsmodereq, &pt_fetchgsmodereq);
        sts = oz_knl_ioop_lockw (ioop, pt_fetchgsmodereq.size, pt_fetchgsmodereq.buff, NULL, NULL, NULL);
        if (sts == OZ_SUCCESS) sts = oz_knl_ioop_lockw (ioop, sizeof *(pt_fetchgsmodereq.reqid_r), pt_fetchgsmodereq.reqid_r, NULL, NULL, NULL);
        if (sts == OZ_SUCCESS) {
          db  = lockdb (devex);
          sts = io_pt_fetchgsmodereq (devex, &pt_fetchgsmodereq);
          unlkdb (devex, db);
        }
      }
      return (sts);
    }

    /* Post an get/setmode request from the queue */

    case OZ_IO_CONPSEUDO_POSTGSMODEREQ: {
      OZ_IO_conpseudo_postgsmodereq pt_postgsmodereq;

      sts = OZ_BADIOFUNC;
      if (iochan == devex -> masteriochan) {			/* only accessible to channel that set it up */
        movc4 (as, ap, sizeof pt_postgsmodereq, &pt_postgsmodereq);
        sts = oz_knl_ioop_lockr (ioop, pt_postgsmodereq.size, pt_postgsmodereq.buff, NULL, NULL, NULL);
        if (sts == OZ_SUCCESS) {
          db  = lockdb (devex);
          sts = io_pt_postgsmodereq (devex, &pt_postgsmodereq);
          unlkdb (devex, db);
        }
      }
      return (sts);
    }

    /* Tell class driver what modes we are doing (like maybe the telnet client changed the window size) */

    case OZ_IO_CONPSEUDO_SETMODE: {
      OZ_IO_conpseudo_setmode pt_setmode;

      sts = OZ_BADIOFUNC;
      if (iochan == devex -> masteriochan) {			/* only accessible to channel that set it up */
        movc4 (as, ap, sizeof pt_setmode, &pt_setmode);
        sts = oz_knl_ioop_lockr (ioop, pt_setmode.size, pt_setmode.buff, NULL, NULL, NULL);
        if (sts == OZ_SUCCESS) {
          db  = lockdb (devex);
          sts = (*(devex -> comport_setup.class_setmode)) (devex -> comport_setup.class_param, &pt_setmode);
          unlkdb (devex, db);
        }
      }
      return (sts);
    }

    /* All others go to the class driver - dont let masteriochan channel do these because the */
    /* pt_deassign routine won't call the class driver's deassign routine for this channel    */

    default: {
      sts = OZ_DEVOFFLINE;
      if ((iochan != devex -> masteriochan) && (devex -> classiochan != NULL)) {
        sts = (*(devex -> comport_setup.class_functab -> start)) (devunit, devex -> comport_setup.class_devex, iochan, ((Chnex *)chnexv) -> class_area, 
                                                                  procmode, ioop, iopex -> class_area, funcode, as, ap);
      }
      return (sts);
    }
  }
}

/************************************************************************/
/*									*/
/*  Process an OZ_IO_CONPSEUDO_SETUP request				*/
/*  This creates the class device and activates it			*/
/*									*/
/************************************************************************/

static uLong io_pt_setup (OZ_Ioop *ioop, OZ_Devunit *devunit, Devex *devex, const char *portname, const char *classname)

{
  char unitdesc[OZ_DEVUNIT_DESCSIZE];
  OZ_Iochan *classiochan;
  OZ_Thread *thread;
  uLong sts;

  if (devex -> classiochan != NULL) return (OZ_FILEALREADYOPEN);
  if (devex -> terminated) return (OZ_DEVOFFLINE);

  if (!oz_knl_devunit_rename (devunit, portname, NULL)) {
    oz_knl_printk ("oz_dev_conpseudo: failed to rename %s to %s\n", oz_knl_devunit_devname (devunit), portname);
  }

  /* Set up application level console class device by assigning a channel to 'conclass' and passing our setup parameters */

  sts = oz_knl_iochan_crbynm (classname, OZ_LOCKMODE_CW, OZ_PROCMODE_KNL, NULL, &classiochan);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_dev_conpseudo: error %u assigning channel to class device %s for %s\n", sts, classname, portname);
    return (sts);
  }

  devex -> comport_setup.port_devunit      = devunit;			/* port device unit */
  devex -> comport_setup.port_param        = devex;			/* this is what the class driver is to pass back to my port routines */
  devex -> comport_setup.port_terminate    = terminate;			/* class driver calls this routine when last channel is deassigned from the app level device */
  devex -> comport_setup.port_read_start   = read_start;		/* class driver calls this routine when starting/finishing a read request */
  devex -> comport_setup.port_disp_start   = disp_start;		/* class driver calls this when there is data to be displayed on the screen */
  devex -> comport_setup.port_disp_suspend = disp_suspend;		/* class driver calls this when it wants us to suspend/resume/abort screen output operations */
  devex -> comport_setup.port_kbd_rah_full = kbd_rah_full;		/* class driver calls this when its keyboard buffer fills/empties */
  devex -> comport_setup.port_getsetmode   = getsetmode;		/* class driver calls this to process get/setmode function */
  devex -> comport_setup.port_lkprm        = devex;			/* this is what the class driver is to pass back to my lockdb/unlkdb routines */
  devex -> comport_setup.port_lockdb       = lockdb;			/* class driver calls this to lock the database */
  devex -> comport_setup.port_unlkdb       = unlkdb;			/* class driver calls this to unlock the database */

  sts = oz_knl_io (classiochan, OZ_IO_COMPORT_SETUP, sizeof devex -> comport_setup, &(devex -> comport_setup));
  if (sts != OZ_SUCCESS) {
    oz_knl_iochan_increfc (classiochan, -1);
    oz_knl_printk ("oz_dev_conpseudo: error %u setting up device %s via %s\n", sts, portname, classname);
  } else {
    *(devex -> port_functab) = pt_functable;				/* re-init func table and modify extension area sizes for class driver */
    devex -> port_functab -> chn_exsize += devex -> comport_setup.class_functab -> chn_exsize;
    devex -> port_functab -> iop_exsize += devex -> comport_setup.class_functab -> iop_exsize;
    devex -> port_functab -> sel_exsize += devex -> comport_setup.class_functab -> sel_exsize;
    devex -> senddatqt   = &(devex -> senddatqh);			/* it is ok to queue stuff now */
    devex -> sendreqqt   = &(devex -> sendreqqh);
    devex -> eventdatqt  = &(devex -> eventdatqh);
    devex -> eventreqqt  = &(devex -> eventreqqh);
    devex -> gsreqt      = &(devex -> gsreqh);
    devex -> classiochan = classiochan;					/* turn it 'online' */
    thread = oz_knl_ioop_getthread (ioop);
    if (thread == NULL) strcpy (unitdesc, "online");
    else oz_sys_sprintf (sizeof unitdesc, unitdesc, "online (threadid %u)", oz_knl_thread_getid (thread));
    oz_knl_devunit_rename (devunit, NULL, unitdesc);
  }

  return (sts);
}

/************************************************************************/
/*									*/
/*  Process OZ_IO_CONPSEUDO_GETSCRDATA request				*/
/*									*/
/************************************************************************/

static uLong io_pt_getscrdata (Devex *devex, Iopex *iopex, uLong size, char *buff, uLong *rlen)

{
  Senddat *senddat;

  /* Don't allow if channel has not been set up yet or has been deassigned */

  if (devex -> classiochan == NULL) return (OZ_DEVOFFLINE);

  /* See if there is any data for it right now */

  senddat = devex -> senddatqh;

  /* If not, queue request to wait for some screen data to arrive from class driver */

  if (senddat == NULL) {
    iopex -> next = NULL;
    iopex -> u.getscrdata.size = size;			/* save caller's buffer size */
    iopex -> u.getscrdata.buff = buff;			/* save caller's buffer address */
    iopex -> u.getscrdata.rlen = rlen;			/* save where caller wants length returned */
    *(devex -> sendreqqt) = iopex;			/* put request on end of queue */
    devex -> sendreqqt = &(iopex -> next);
    return (OZ_STARTED);				/* tell caller it will complete later */
  }

  /* If so, dequeue the data, return corresponding values and complete synchronously */

  if (senddat -> size > size) {
    memcpy (buff, senddat -> buff, size);		/* have more data than request can take, */
    senddat -> size -= size;				/* so just get as much as we can return */
    senddat -> buff += size;
  } else {
    devex -> senddatqh = senddat -> next;		/* request can take all the data, dequeue data */
    if (devex -> senddatqh == NULL) devex -> senddatqt = &(devex -> senddatqh);
    devex -> senddatqs --;
    size = senddat -> size;				/* take all the data */
    memcpy (buff, senddat -> buff, size);
    (*(devex -> comport_setup.class_displayed)) (devex -> comport_setup.class_param, senddat -> write_param, OZ_SUCCESS);
    OZ_KNL_PGPFREE (senddat);
  }
  *rlen = size;						/* anyway, tell caller how much we returned */
  return (OZ_SUCCESS);					/* completed synchronously */
}

/************************************************************************/
/*									*/
/*  Send data to the pseudo-terminal as keyboard data			*/
/*									*/
/*    Input:								*/
/*									*/
/*	size    = number of characters in buff				*/
/*	buff    = address of characters					*/
/*	smplock = softint delivery inhibited				*/
/*									*/
/************************************************************************/

static uLong io_pt_putkbddata (Devex *devex, uLong size, const char *buff)

{
  uLong i;

  if (devex -> classiochan == NULL) return (OZ_DEVOFFLINE);
  for (i = 0; i < size; i ++) {
    (*(devex -> comport_setup.class_kbd_char)) (devex -> comport_setup.class_param, buff[i]);
  }

  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Called by the class driver when too much keyboard data has been 	*/
/*  received								*/
/*									*/
/************************************************************************/

static void kbd_rah_full (void *devexv, int full)

{
  /* Tell the server program to suspend or resume sending keyboard data */

  gotevent (devexv, full ? OZ_CONPSEUDO_KBD_SUSPEND : OZ_CONPSEUDO_KBD_RESUME);
}

/************************************************************************/
/*									*/
/*  Called by the class driver to process the get/setmode function	*/
/*									*/
/************************************************************************/

static uLong getsetmode (void *devexv, void *getset_param, uLong size, OZ_Console_modebuff *buff)

{
  Devex *devex;
  Gsreq *gsreq;

  devex = devexv;

  gsreq = OZ_KNL_PGPMALLOQ (sizeof *gsreq);	/* remember about this request */
  if (gsreq == NULL) return (OZ_EXQUOTAPGP);
  *(devex -> gsreqt) = gsreq;
  gsreq -> next    = NULL;
  gsreq -> param   = getset_param;
  gsreq -> size    = size;
  gsreq -> buff    = buff;
  gsreq -> fetched = 0;
  devex -> gsreqt  = &(gsreq -> next);

  gotevent (devex, OZ_CONPSEUDO_GETSETMODE);	/* tell telnetd that someone is trying to get/set the modes */

  return (OZ_STARTED);				/* wait for telnetd to get back to us on that */
}

/************************************************************************/
/*									*/
/*  Process OZ_IO_CONPSEUDO_GETEVENT request				*/
/*  This function returns the next event to telnetd (or whoever)	*/
/*									*/
/************************************************************************/

static uLong io_pt_getevent (Devex *devex, Iopex *iopex, OZ_Conpseudo_event *event)

{
  Eventdat *eventdat;

  if (devex -> classiochan == NULL) return (OZ_DEVOFFLINE);

  /* See if there is any event for it right now */

  eventdat = devex -> eventdatqh;

  /* If not, queue request for later event */

  if (eventdat == NULL) {
    iopex -> next = NULL;
    iopex -> u.getevent.event = event;
    *(devex -> eventreqqt) = iopex;
    devex -> eventreqqt = &(iopex -> next);
    return (OZ_STARTED);
  }

  /* If so, dequeue the event, return corresponding value and complete synchronously */

  devex -> eventdatqh = eventdat -> next;
  if (devex -> eventdatqh == NULL) devex -> eventdatqt = &(devex -> eventdatqh);
  *event = eventdat -> event;
  OZ_KNL_PGPFREE (eventdat);
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Fetch / Post Get / Set mode request					*/
/*									*/
/************************************************************************/

static uLong io_pt_fetchgsmodereq (Devex *devex, OZ_IO_conpseudo_fetchgsmodereq *pt_fetchgsmodereq)

{
  Gsreq *gsreq;

  if (devex -> classiochan == NULL) return (OZ_DEVOFFLINE);

  /* Find the first request that hasn't been fetched yet */

  for (gsreq = devex -> gsreqh; gsreq != NULL; gsreq = gsreq -> next) if (!(gsreq -> fetched)) break;
  if (gsreq == NULL) return (OZ_QUEUEMPTY);

  /* Copy the class driver's data to the daemon's buffer and mark the request as fetched */

  movc4 (gsreq -> size, gsreq -> buff, pt_fetchgsmodereq -> size, pt_fetchgsmodereq -> buff);
  *(pt_fetchgsmodereq -> reqid_r) = gsreq;
  gsreq -> fetched = 1;

  /* Successful */

  return (OZ_SUCCESS);
}

static uLong io_pt_postgsmodereq (Devex *devex, OZ_IO_conpseudo_postgsmodereq *pt_postgsmodereq)

{
  Gsreq *gsreq, **lgsreq;

  /* Scan the queue for the given reqid */

  for (lgsreq = &(devex -> gsreqh); (gsreq = *lgsreq) != NULL; lgsreq = &(gsreq -> next)) {
    if (pt_postgsmodereq -> reqid == gsreq) {

      /* Request found, it better be marked as 'fetched' or it might be a re-cycle of the same memory address */

      if (!(gsreq -> fetched)) return (OZ_NOSUCHREQ);

      /* Ok, remove from queue */

      *lgsreq = gsreq -> next;
      if (gsreq -> next == NULL) devex -> gsreqt = &(devex -> gsreqh);

      /* Copy daemon's data back to class driver's buffer and tell class driver request is complete */

      movc4 (pt_postgsmodereq -> size, pt_postgsmodereq -> buff, gsreq -> size, gsreq -> buff);
      (*(devex -> comport_setup.class_gotsetmode)) (devex -> comport_setup.class_param, gsreq -> param, pt_postgsmodereq -> status);

      /* Clean up and return success status to daemon */

      OZ_KNL_PGPFREE (gsreq);
      return (OZ_SUCCESS);
    }
  }
  return (OZ_NOSUCHREQ);
}

/************************************************************************/
/*									*/
/*  Called by class driver to terminate operations			*/
/*									*/
/*  The class driver shall not make any other calls to us afther this 	*/
/*  except to unlock the database					*/
/*									*/
/*    Input:								*/
/*									*/
/*	chnex = context block pointer					*/
/*	database locked							*/
/*									*/
/************************************************************************/

static void terminate (void *devexv)

{
  Devex *devex;

  devex = devexv;
  devex -> terminated = 1;			/* set flag to remember we got a terminate call */
  gotevent (devex, OZ_CONPSEUDO_TERMINATE);	/* send a terminate event to the server program */
						/* (eg, tell telnetd the user has logged out) */
}

/************************************************************************/
/*									*/
/*  Called by the class driver when it starts/finishes a read request	*/
/*									*/
/*    Input:								*/
/*									*/
/*	chnex   = connection context pointer				*/
/*	smplock = softint delivery inhibited, database locked		*/
/*									*/
/************************************************************************/

static void read_start (void *devexv, int start)

{
  gotevent (devexv, start ? OZ_CONPSEUDO_KBD_READSTARTED : OZ_CONPSEUDO_KBD_READFINISHED);
}

/************************************************************************/
/*									*/
/*  Called by the class driver to start displaying some data		*/
/*									*/
/*    Input:								*/
/*									*/
/*	chnex       = connection context pointer			*/
/*	write_param = value to pass to completion routine		*/
/*	size        = number of characters to write			*/
/*	buff        = address of characters to write			*/
/*	smplock     = softint delivery inhibited, database locked	*/
/*									*/
/*    Output:								*/
/*									*/
/*	disp_start = OZ_STARTED : will complete asynchronously		*/
/*	                   else : completion status			*/
/*									*/
/************************************************************************/

static uLong disp_start (void *devexv, void *write_param, uLong size, char *buff)

{
  Devex *devex;
  Iopex *sendreq;
  Senddat *senddat;
  uLong sts;

  devex = devexv;

  if (devex -> terminated) oz_crash ("oz_dev_conpseudo: disp_start called after terminated");
  sts = OZ_DEVOFFLINE;
  if (devex -> classiochan == NULL) goto rtn;

  /* Don't let the queue get too big - if we return OZ_QUEUEFULL status, the caller will expect us to call its class_displayed routine when we are ready for more */

  sts = OZ_QUEUEFULL;
  if (devex -> senddatqs >= SENDDATQMAX) goto rtn;

  /* Queue send data */

  senddat = OZ_KNL_PGPMALLOQ (sizeof *senddat);		/* set up descriptor for screen data */
  sts = OZ_EXQUOTAPGP;
  if (senddat == NULL) goto rtn;
  senddat -> next        = NULL;
  senddat -> write_param = write_param;
  senddat -> size        = size;
  senddat -> buff        = buff;

  *(devex -> senddatqt) = senddat;			/* queue it for later output */
  devex -> senddatqt = &(senddat -> next);
  devex -> senddatqs ++;

  sendreq = devex -> sendreqqh;				/* see if any I/O request waiting for it */
  if (sendreq != NULL) {
    devex -> sendreqqh = sendreq -> next;		/* if so, dequeue I/O request */
    if (devex -> sendreqqh == NULL) devex -> sendreqqt = &(devex -> sendreqqh);
    oz_knl_iodone (sendreq -> ioop, OZ_SUCCESS, NULL, sendreq_iodone, sendreq);
  }

  sts = OZ_STARTED;					/* either way, it's not done yet (we still have to access the data in the sendreq_iodone routine) */

rtn:
  return (sts);
}

/* This routine is called when back in the calling process' address space */

static void sendreq_iodone (void *sendreqv, int finok, uLong *status_r)

{
  Devex *devex;
  Iopex *sendreq;
  uLong db;
  Senddat *senddat;

  sendreq = sendreqv;
  devex   = sendreq -> devex;

  if (!finok) return;									/* see if we made it back to requestor's process */

  *(sendreq -> u.getscrdata.rlen) = 0;							/* ok, zero out the length in case there is no data */
  db = lockdb (devex);									/* lock the database */
  if (devex -> classiochan != NULL) {							/* see if we're still allowed to call class_displayed routine */
											/* (if not, we return a length of zero) */
    senddat = devex -> senddatqh;							/* see if there is any data ready for us */
											/* (if not, we return a length of zero) */
    if (senddat != NULL) {
      if (senddat -> size > sendreq -> u.getscrdata.size) {				/* ok, see if bigger than requestor's buffer */
        memcpy (sendreq -> u.getscrdata.buff, senddat -> buff, sendreq -> u.getscrdata.size); /* it is, just copy what will fit */
        senddat -> size -= sendreq -> u.getscrdata.size;				/* ... and reduce data by that much */
        senddat -> buff += sendreq -> u.getscrdata.size;				/* ... and leave what's left on the queue */
      } else {
        devex -> senddatqh = senddat -> next;						/* it will all fit, remove data from queue */
        if (devex -> senddatqh == NULL) devex -> senddatqt = &(devex -> senddatqh);
        devex -> senddatqs --;
        sendreq -> u.getscrdata.size = senddat -> size;					/* set size returned = all the data */
        memcpy (sendreq -> u.getscrdata.buff, senddat -> buff, senddat -> size);	/* copy all the data out */
        (*(devex -> comport_setup.class_displayed)) (devex -> comport_setup.class_param, senddat -> write_param, OZ_SUCCESS); /* tell class driver we finished with data */
        OZ_KNL_PGPFREE (senddat);							/* free off the data block */
      }
      *(sendreq -> u.getscrdata.rlen) = sendreq -> u.getscrdata.size;			/* return how much data we copied out */
    }
  }
  unlkdb (devex, db);
}

/************************************************************************/
/*									*/
/*  Called by the class driver to suspend any output operation that 	*/
/*  might be in progress						*/
/*									*/
/*  In this implementation, we just pass an event on to the program, 	*/
/*  and hopefully it will stop sending stuff.				*/
/*									*/
/************************************************************************/

static void disp_suspend (void *devexv, int suspend)

{
  Devex *devex;
  OZ_Conpseudo_event event;

  devex = devexv;

  if (devex -> terminated) oz_crash ("oz_dev_conpseudo: disp_suspend called after terminated");

  event = OZ_CONPSEUDO_SCR_RESUME;			/* resume output (ie, got a control-Q from keyboard) */
  if (suspend < 0) event = OZ_CONPSEUDO_SCR_ABORT;	/* abort output (ie, got a control-O from keyboard) */
  if (suspend > 0) event = OZ_CONPSEUDO_SCR_SUSPEND;	/* suspend output (ie, got a control-S from keyboard) */

  gotevent (devex, event);
}

/************************************************************************/
/*									*/
/*  We got an event so satisfy a pending event request			*/
/*  (or queue the event for later retrieval if no request pending)	*/
/*									*/
/************************************************************************/

static void gotevent (Devex *devex, OZ_Conpseudo_event event)

{
  Eventdat *eventdat;
  Iopex *eventreq;

  if (devex -> classiochan == NULL) return;						/* ignore event if I/O channel has been deassigned */

  eventdat = OZ_KNL_PGPMALLOC (sizeof *eventdat);					/* ok, save the event code in a block */
  eventdat -> next  = NULL;
  eventdat -> event = event;

  eventreq = devex -> eventreqqh;							/* see if there is a pending I/O request to receive it */
  if (eventreq == NULL) {
    *(devex -> eventdatqt) = eventdat;							/* if not, queue for later retrieval */
    devex -> eventdatqt = &(eventdat -> next);
  } else {
    devex -> eventreqqh = eventreq -> next;						/* if so, dequeue the I/O request */
    if (devex -> eventreqqh == NULL) devex -> eventreqqt = &(devex -> eventreqqh);
    eventreq -> u.getevent.eventdat = eventdat;						/* post request for completion */
    oz_knl_iodone (eventreq -> ioop, OZ_SUCCESS, NULL, event_iodone, eventreq);
  }
}

/* This routine executes in the thread context to deliver the event code */

static void event_iodone (void *eventreqv, int finok, uLong *status_r)

{
  Eventdat *eventdat;
  Iopex *eventreq;

  eventreq = eventreqv;
  eventdat = eventreq -> u.getevent.eventdat;

  if (finok) *(eventreq -> u.getevent.event) = eventdat -> event;
  OZ_KNL_PGPFREE (eventdat);
}

/************************************************************************/
/*									*/
/*  Lock access to database						*/
/*									*/
/************************************************************************/

static uLong lockdb (void *devexv)

{
  Devex *devex;
  Long prev;
  OZ_Thread *thread;

  devex  = devexv;
  thread = oz_knl_thread_getcur ();

tryit:
  prev = oz_knl_event_set (devex -> lockevent, 0);	/* clear flag to mark it locked */
  if (prev > 0) devex -> lockthread = thread;		/* if not previously locked, say this thread has it now */
  if (devex -> lockthread == thread) return (prev);	/* if this thread has it locked now, return with prev state */
  oz_knl_event_waitone (devex -> lockevent);		/* another thread has is locked, wait for it to be released */
  goto tryit;						/* try again to lock it */
}

/************************************************************************/
/*									*/
/*  Unlock access to database						*/
/*									*/
/************************************************************************/

static void unlkdb (void *devexv, uLong iplsav)

{
  Devex *devex;

  devex = devexv;

  if (iplsav > 0) {					/* see if it's really being unlocked */
    devex -> lockthread = NULL;				/* if so, clear thread pointer so all 'if (lockthread == thread)' compares will fail causing them to wait */
    oz_knl_event_set (devex -> lockevent, iplsav);	/* unlock by setting the flag */
  }
}
