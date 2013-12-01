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
/*  Mutex driver							*/
/*									*/
/************************************************************************/

#define _OZ_KNL_LOCK_TABLES

#include "ozone.h"
#include "oz_io_mutex.h"
#include "oz_knl_devio.h"
#include "oz_knl_hw.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_lock.h"
#include "oz_knl_objtype.h"
#include "oz_knl_sdata.h"
#include "oz_knl_status.h"

#define HASHCOUNT (256)

#define LOCKMUTICIES(__devex) while (oz_knl_event_set (__devex -> lockevent, 0) <= 0) oz_knl_event_waitone (__devex -> lockevent)
#define UNLKMUTICIES(__devex) oz_knl_event_set (__devex -> lockevent, 1)

typedef struct Chnex Chnex;
typedef struct Devex Devex;
typedef struct Iopex Iopex;
typedef struct Mutex Mutex;

struct Iopex { Iopex *next;			/* next in waiterq or unblockq */
               Chnex *chnex;			/* corresponding channel */
               OZ_Ioop *ioop;			/* corresponding i/o operation */
               int express;			/* setmode express flag */
               OZ_Lockmode oldmode;		/* mutex's old mode */
               OZ_Lockmode newmode;		/* mutex's new mode */
             };

struct Mutex { Mutex *next;			/* next in devex->muticies[hashindex] list */
               Long refcount;			/* number of chnex's that point to this mutex */
               Long active_readers;		/* number of locks granted that allow self to read */
               Long active_writers;		/* number of locks granted that allow self to write */
               Long block_readers;		/* number of locks granted that block others from reading */
               Long block_writers;		/* number of locks granted that block others from writing */
               Iopex *waiterqh;			/* list of waiting requests */
               Iopex **waiterqt;
               Iopex *unblockerqh;		/* list of those willing to give up lock */
               Iopex **unblockerqt;
               uLong namesize;			/* number of bytes in namebuff */
               uByte namebuff[1];		/* name string (binary, not null-terminated) */
             };

struct Chnex { Mutex *mutex;			/* NULL when closed, else points to mutex that is open */
               OZ_Lockmode curmode;		/* lock mode that this channel has mutex at */
             };

struct Devex { OZ_Event *lockevent;		/* locking event flag */
               Mutex *muticies[HASHCOUNT];	/* hash table of muticies */
             };

static uLong mutex_clonecre (OZ_Devunit *template_devunit, void *template_devex, int template_cloned, OZ_Procmode procmode, OZ_Devunit **cloned_devunit);
static int mutex_clonedel (OZ_Devunit *devunit, void *devexv, int cloned);
static uLong mutex_assign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode);
static int mutex_deassign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv);
static void mutex_abort (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Ioop *ioop, void *iopexv, OZ_Procmode procmode);
static uLong mutex_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode,
                          OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap);
static uLong create (Devex *devex, Chnex *chnex, uLong namesize, const uByte *name, OZ_Ioop *ioop);
static uLong lookup (Devex *devex, Chnex *chnex, uLong namesize, const uByte *name);
static int namehash (uLong namesize, const uByte *namebuff);
static Long increfc (Devex *devex, Mutex *mutex, Long inc);
static uLong setmode (Iopex *iopex, int express, int noqueue);
static uLong trytoconvert (Mutex *mutex, OZ_Lockmode oldmode, OZ_Lockmode newmode);
static uLong unblock (Iopex *iopex);

static const OZ_Devfunc mutex_functable = { sizeof (Devex), sizeof (Chnex), sizeof (Iopex), 0, NULL, mutex_clonecre, mutex_clonedel, mutex_assign, mutex_deassign, mutex_abort, mutex_start, NULL };

static int initialized = 0;
static OZ_Devclass *devclass;
static OZ_Devdriver *devdriver;
static OZ_Devunit *devunit;

/************************************************************************/
/*									*/
/*  Boot-time initialization routine					*/
/*									*/
/************************************************************************/

void oz_dev_mutex_init (void)

{
  if (!initialized) {
    oz_knl_printk ("oz_dev_mutex_init\n");
    initialized = 1;
    devclass    = oz_knl_devclass_create (OZ_IO_MUTEX_CLASSNAME, OZ_IO_MUTEX_BASE, OZ_IO_MUTEX_MASK, "oz_dev_mutex");
    devdriver   = oz_knl_devdriver_create (devclass, "oz_dev_mutex");
    devunit     = oz_knl_devunit_create (devdriver, "mutex", "mutex template", &mutex_functable, 0, oz_s_secattr_tempdev);
  }
}

/************************************************************************/
/*									*/
/*  Someone is trying to assign a channel to the template device, so	*/
/*  we create a 'real' device for them to use				*/
/*									*/
/*    Input:                                                            */
/*                                                                      */
/*      template_devunit = points to mutex template_devunit		*/
/*      template_devex   = points to corresponding devex area           */
/*      template_cloned  = indicates if the template is cloned          */
/*      procmode         = procmode that is assigning channel           */
/*      smplock level    = dv                                           */
/*                                                                      */
/*    Output:                                                           */
/*                                                                      */
/*      mutex_cloncre = OZ_SUCCESS : clone device created		*/
/*                            else : error status			*/
/*      *cloned_devunit = cloned device unit struct pointer		*/
/*                                                                      */
/************************************************************************/

static uLong mutex_clonecre (OZ_Devunit *template_devunit, void *template_devex, int template_cloned, OZ_Procmode procmode, OZ_Devunit **cloned_devunit)

{
  char unitname[32];
  Devex *devex;
  int i;
  OZ_Devunit *devunit;
  OZ_Event *lockevent;
  OZ_Secattr *secattr;
  uLong sts;

  static uLong seq = 0;

  if (template_cloned) {									/* maybe someone is assigning to one of the clones */
    *cloned_devunit = template_devunit;								/* if so, just use the device as it is */
    oz_knl_devunit_increfc (template_devunit, 1);						/* ... but increment its ref count first */
  } else {
    strncpyz (unitname, oz_knl_devunit_devname (template_devunit), sizeof unitname - 2);	/* no, make up a new clone unit name */
    i = strlen (unitname);
    unitname[i++] = '.';
    oz_hw_itoa (++ seq, sizeof unitname - i, unitname + i);
    sts = oz_knl_event_create (10, "mutex lock", NULL, &lockevent);				/* create an locking event flag */
    if (sts != OZ_SUCCESS) return (sts);
    secattr = oz_knl_thread_getdefcresecattr (NULL);
    devunit = oz_knl_devunit_create (devdriver, unitname, unitname, &mutex_functable, 1, secattr); /* create the cloned device */
    oz_knl_secattr_increfc (secattr, -1);
    devex   = oz_knl_devunit_ex (devunit);							/* point to its extension */
    devex -> lockevent = lockevent;								/* create an locking event flag */
    memset (devex -> muticies, 0, sizeof devex -> muticies);					/* it has no muticies yet */
    UNLKMUTICIES (devex);									/* unlock it */
    *cloned_devunit = devunit;									/* return pointer to it */
  }
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  All channels have been deassigned from mutex - delete it from 	*/
/*  system								*/
/*									*/
/*    Input:                                                            */
/*                                                                      */
/*      smplock = dv                                                    */
/*                                                                      */
/*    Output:                                                           */
/*									*/
/*      mutex_clonedel = 0 : retain the device don't delete it		*/
/*                       1 : delete the device				*/
/*									*/
/************************************************************************/

static int mutex_clonedel (OZ_Devunit *devunit, void *devexv, int cloned)

{
  Devex *devex;
  int i;
  OZ_Event *event;

  devex = devexv;

  if (cloned) {							/* see if this is one of the cloned devices */
    LOCKMUTICIES (devex);					/* ok, lock everything out (should always happen immediately as there are no channels assigned */
    for (i = 0; i < HASHCOUNT; i ++) {				/* make sure all the muticies are cleared out (since there are no channels, there can't be any muticies) */
      if (devex -> muticies[i] != NULL) oz_crash ("oz_dev_mutex clonedel: non-null devex->muticies entry");
    }
    event = devex -> lockevent;					/* free off the locking event flag */
    devex -> lockevent = NULL;
    if (event != NULL) oz_knl_event_increfc (event, -1);
  }

  return (cloned);
}

/************************************************************************/
/*									*/
/*  Channel is being assigned - clear out the chnex area		*/
/*									*/
/************************************************************************/

static uLong mutex_assign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode)

{
  memset (chnexv, 0, sizeof (Chnex));
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Channel is being deassigned - abort any pending requests and close	*/
/*									*/
/************************************************************************/

static int mutex_deassign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv)

{
  Chnex *chnex;
  Devex *devex;
  uLong sts;

  chnex = chnexv;
  devex = devexv;

  mutex_abort (devunit, devexv, iochan, chnexv, NULL, NULL, OZ_PROCMODE_KNL);

  if (chnex -> mutex != NULL) {
    sts = oz_knl_iostart2 (1, iochan, OZ_PROCMODE_KNL, NULL, NULL, NULL, NULL, NULL, NULL, OZ_IO_MUTEX_CLOSE, 0, NULL);
    if (sts == OZ_PENDING) return (1);
    if (sts != OZ_SUCCESS) oz_crash ("oz_dev_mutex deassign: error %u closing mutex");
    if (chnex -> mutex != NULL) oz_crash ("oz_dev_mutex deassign: channel didn't close");
  }

  return (0);
}

/************************************************************************/
/*                                                                      */
/*  Abort an i/o request                                                */
/*                                                                      */
/************************************************************************/

static void mutex_abort (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Ioop *ioop, void *iopexv, OZ_Procmode procmode)

{
  Chnex *chnex;
  Devex *devex;
  Iopex *iopex, **liopex;
  Mutex *mutex;

  chnex = chnexv;
  devex = devexv;

  /* See what mutex (if any) is open on the channel */

  LOCKMUTICIES (devex);
  mutex = chnex -> mutex;
  if (mutex != NULL) {

    /* Abort applicable requests from mutex's waiter queue */

    for (liopex = &(mutex -> waiterqh); (iopex = *liopex) != NULL;) {
      if (!oz_knl_ioabortok (iopex -> ioop, iochan, procmode, ioop)) liopex = &(iopex -> next);
      else {
        *liopex = iopex -> next;
        oz_knl_iodone (iopex -> ioop, OZ_ABORTED, NULL, NULL, NULL);
      }
    }
    mutex -> waiterqt = liopex;

    /* Abort applicable requests from mutex's unblocker queue */

    for (liopex = &(mutex -> unblockerqh); (iopex = *liopex) != NULL;) {
      if (!oz_knl_ioabortok (iopex -> ioop, iochan, procmode, ioop)) liopex = &(iopex -> next);
      else {
        *liopex = iopex -> next;
        oz_knl_iodone (iopex -> ioop, OZ_ABORTED, NULL, NULL, NULL);
      }
    }
    mutex -> unblockerqt = liopex;
  }
  UNLKMUTICIES (devex);
}

/************************************************************************/
/*									*/
/*  Start performing a mutex i/o function				*/
/*									*/
/************************************************************************/

static uLong mutex_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode,
                          OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap)

{
  Chnex *chnex;
  Devex *devex;
  Iopex *iopex;
  uLong sts;
  OZ_IO_mutex_create   mutex_create;
  OZ_IO_mutex_getinfo1 mutex_getinfo1;
  OZ_IO_mutex_lookup   mutex_lookup;
  OZ_IO_mutex_setmode  mutex_setmode;
  Mutex *mutex;

  chnex = chnexv;
  devex = devexv;
  iopex = iopexv;
  iopex -> ioop  = ioop;
  iopex -> chnex = chnex;

  switch (funcode) {

    /* Create - creates the named lock and points channel to it */

    case OZ_IO_MUTEX_CREATE: {
      movc4 (as, ap, sizeof mutex_create, &mutex_create);
      sts = oz_knl_ioop_lockr (ioop, mutex_create.namesize, mutex_create.namebuff, NULL, NULL, NULL);
      if (sts == OZ_SUCCESS) {
        LOCKMUTICIES (devex);
        sts = create (devex, chnex, mutex_create.namesize, mutex_create.namebuff, ioop);
        UNLKMUTICIES (devex);
      }
      return (sts);
    }

    /* Lookup - looks up the named lock and points channel to it */

    case OZ_IO_MUTEX_LOOKUP: {
      movc4 (as, ap, sizeof mutex_lookup, &mutex_lookup);
      sts = oz_knl_ioop_lockr (ioop, mutex_lookup.namesize, mutex_lookup.namebuff, NULL, NULL, NULL);
      if (sts == OZ_SUCCESS) {
        LOCKMUTICIES (devex);
        sts = lookup (devex, chnex, mutex_lookup.namesize, mutex_lookup.namebuff);
        UNLKMUTICIES (devex);
      }
      return (sts);
    }

    /* Set the mode of the lock */

    case OZ_IO_MUTEX_SETMODE: {
      movc4 (as, ap, sizeof mutex_setmode, &mutex_setmode);
      if (mutex_setmode.newmode >= OZ_LOCKMODE_XX) return (OZ_BADPARAM);
      iopex -> newmode = mutex_setmode.newmode;
      LOCKMUTICIES (devex);
      sts = setmode (iopex, (mutex_setmode.flags & OZ_IO_MUTEX_SETMODE_FLAG_EXPRESS) != 0, (mutex_setmode.flags & OZ_IO_MUTEX_SETMODE_FLAG_NOQUEUE) != 0);
      UNLKMUTICIES (devex);
      return (sts);
    }

    /* Declare that we are willing to give up lock if someone else wants it */

    case OZ_IO_MUTEX_UNBLOCK: {
      LOCKMUTICIES (devex);
      sts = unblock (iopex);
      UNLKMUTICIES (devex);
      return (sts);
    }

    /* All done with lock */

    case OZ_IO_MUTEX_CLOSE: {
      LOCKMUTICIES (devex);
      if (chnex -> curmode != OZ_LOCKMODE_NL) {								/* ok, make sure it is null mode */
        iopex -> newmode = OZ_LOCKMODE_NL;								/* not null, convert to null */
        sts = setmode (iopex, 1, 1);
        if (sts != OZ_SUCCESS) oz_crash ("oz_dev_mutex close: error %u setting mutex to NL", sts);	/* should always be an immediate success */
      }
      mutex = chnex -> mutex;
      if (mutex != NULL) {
        chnex -> mutex = NULL;										/* anyway, all done with channel */
        increfc (devex, mutex, -1);									/* ... and all done with mutex */
      }
      UNLKMUTICIES (devex);
      return (OZ_SUCCESS);
    }

    /* Get info */

    case OZ_IO_MUTEX_GETINFO1: {
      movc4 (as, ap, sizeof mutex_getinfo1, &mutex_getinfo1);
      sts = oz_knl_ioop_lockw (ioop, as, ap, NULL, NULL, NULL);
      if ((sts == OZ_SUCCESS) && (mutex_getinfo1.namebuff != NULL)) sts = oz_knl_ioop_lockw (ioop, mutex_getinfo1.namesize, mutex_getinfo1.namebuff, NULL, NULL, NULL);
      if ((sts == OZ_SUCCESS) && (mutex_getinfo1.namerlen != NULL)) sts = oz_knl_ioop_lockw (ioop, sizeof *mutex_getinfo1.namerlen, mutex_getinfo1.namerlen, NULL, NULL, NULL);
      if (sts == OZ_SUCCESS) {
        LOCKMUTICIES (devex);
        mutex = chnex -> mutex;
        sts   = OZ_FILENOTOPEN;
        if (mutex != NULL) {
          if (mutex_getinfo1.namebuff != NULL) {
            sts = mutex_getinfo1.namesize;
            if (sts > mutex -> namesize) sts = mutex -> namesize;
            memcpy (mutex_getinfo1.namebuff, mutex -> namebuff, sts);
            if (mutex_getinfo1.namerlen != NULL) *mutex_getinfo1.namerlen = sts;
          }
          else if (mutex_getinfo1.namerlen != NULL) *mutex_getinfo1.namerlen = mutex -> namesize;
          mutex_getinfo1.curmode = chnex -> curmode;
          mutex_getinfo1.active_readers = mutex -> active_readers;
          mutex_getinfo1.active_writers = mutex -> active_writers;
          mutex_getinfo1.block_readers  = mutex -> block_readers;
          mutex_getinfo1.block_writers  = mutex -> block_writers;
          movc4 (sizeof mutex_getinfo1, &mutex_getinfo1, as, ap);
          sts = OZ_SUCCESS;
        }
        UNLKMUTICIES (devex);
      }
      return (sts);
    }

    /* Who knows what they want */

    default: {
      return (OZ_BADIOFUNC);
    }
  }
}

/************************************************************************/
/*									*/
/*  Create a new mutex							*/
/*									*/
/*    Input:								*/
/*									*/
/*	name = pointer to null-terminated name string			*/
/*									*/
/*    Output:								*/
/*									*/
/*	create = completion status					*/
/*									*/
/*    Note:								*/
/*									*/
/*	if like named mutex already exists, its ref count is 		*/
/*	incremented and a pointer to the existing struct is returned	*/
/*									*/
/************************************************************************/

static uLong create (Devex *devex, Chnex *chnex, uLong namesize, const uByte *namebuff, OZ_Ioop *ioop)

{
  int hashindex;
  Mutex *mutex;

  if (chnex -> mutex != NULL) return (OZ_FILEALREADYOPEN);

  hashindex = namehash (namesize, namebuff);
  for (mutex = devex -> muticies[hashindex]; mutex != NULL; mutex = mutex -> next) {
    if ((namesize == mutex -> namesize) && (memcmp (namebuff, mutex -> namebuff, namesize) == 0)) break;
  }

  if (mutex == NULL) {
    mutex = OZ_KNL_PGPMALLOQ (sizeof *mutex + namesize);
    if (mutex == NULL) return (OZ_EXQUOTAPGP);
    memset (mutex, 0, sizeof *mutex);
    mutex -> next        = devex -> muticies[hashindex];
    mutex -> waiterqt    = &(mutex -> waiterqh);
    mutex -> unblockerqt = &(mutex -> unblockerqh);
    mutex -> namesize    = namesize;
    memcpy (mutex -> namebuff, namebuff, namesize);
    devex -> muticies[hashindex] = mutex;
  }

  mutex -> refcount ++;
  chnex -> mutex = mutex;
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Look up existing mutex						*/
/*									*/
/*    Input:								*/
/*									*/
/*	name = pointer to null-terminated name string			*/
/*									*/
/*    Output:								*/
/*									*/
/*	lookup = completion status					*/
/*									*/
/************************************************************************/

static uLong lookup (Devex *devex, Chnex *chnex, uLong namesize, const uByte *namebuff)

{
  int hashindex;
  Mutex *mutex;

  if (chnex -> mutex != NULL) return (OZ_FILEALREADYOPEN);

  hashindex = namehash (namesize, namebuff);
  for (mutex = devex -> muticies[hashindex]; mutex != NULL; mutex = mutex -> next) {
    if ((namesize == mutex -> namesize) && (memcmp (namebuff, mutex -> namebuff, namesize) == 0)) {
      mutex -> refcount ++;
      chnex -> mutex = mutex;
      return (OZ_SUCCESS);
    }
  }
  return (OZ_NOSUCHFILE);
}

static int namehash (uLong namesize, const uByte *namebuff)

{
  int h;
  uLong i;

  h = 0;
  for (i = 0; i < namesize; i ++) h += namebuff[i];
  return (h % HASHCOUNT);
}

/************************************************************************/
/*									*/
/*  Increment mutex ref count						*/
/*									*/
/*    Input:								*/
/*									*/
/*	mutex = mutex in question					*/
/*	inc = amount to increment by (neg to decrement)			*/
/*									*/
/*    Output:								*/
/*									*/
/*	increfc = new ref count						*/
/*	          if zero, mutex is freed off				*/
/*									*/
/************************************************************************/

static Long increfc (Devex *devex, Mutex *mutex, Long inc)

{
  int hashindex;
  Long refc;
  Mutex **lmutex, *xmutex;

  mutex -> refcount += inc;
  refc = mutex -> refcount;
  if (refc < 0) oz_crash ("oz_dev_mutex increfc: ref count went neg (%d)", refc);
  if (refc == 0) {
    if (mutex -> waiterqh != NULL) oz_crash ("oz_dev_mutex increfc: ref count zero but waiterqh not empty");
    if (mutex -> unblockerqh != NULL) oz_crash ("oz_dev_mutex increfc: ref count zero but unblockerqh not empty");
    hashindex = namehash (mutex -> namesize, mutex -> namebuff);
    for (lmutex = devex -> muticies + hashindex; (xmutex = *lmutex) != mutex; lmutex = &(xmutex -> next)) {
      if (xmutex == NULL) oz_crash ("oz_dev_mutex increfc: can't find mutex on list");
    }
    *lmutex = xmutex -> next;
    OZ_KNL_PGPFREE (xmutex);
  }
  return (refc);
}

/************************************************************************/
/*									*/
/*  Set the mode of a mutex, wait if necessary				*/
/*									*/
/*    Input:								*/
/*									*/
/*	iopex -> chnex -> mutex = mutex to be changed			*/
/*	iopex -> chnex -> oldmode = old mode the requestor has		*/
/*	iopex -> newmode = new mode the requestor wants			*/
/*	express = 0 : if upconverting and others are waiting, wait for them first
/*	          1 : if upconverting, try to convert, even if others are waiting
/*	noqueue = 1 : dont queue, just error if blocked			*/
/*	          0 : queue iopex if blocked				*/
/*									*/
/*    Output:								*/
/*									*/
/*	setmode = OZ_ACCONFLICT : denied, access conflict		*/
/*	             OZ_SUCCESS : granted immediately, no waiting	*/
/*	             OZ_STARTED : denied, request queued		*/
/*									*/
/************************************************************************/

static uLong setmode (Iopex *iopex, int express, int noqueue)

{
  Chnex *chnex;
  uLong sts;
  Iopex **lwaiter, *waiter;
  Iopex **lunblocker, *unblocker;
  Mutex *mutex;
  OZ_Lockmode newmode, oldmode;

  chnex   = iopex -> chnex;
  newmode = iopex -> newmode;
  mutex   = chnex -> mutex;
  oldmode = chnex -> curmode;
  if (mutex == NULL) return (OZ_FILENOTOPEN);

  /* If not express, and it is an up-convert (ie, we might get blocked), and there are others waiting, go to end of wait queue */

  if (!express && ((oz_lock_upcnvt[oldmode] >> newmode) & 1) && (mutex -> waiterqh != NULL)) sts = OZ_ACCONFLICT;

  /* Otherwise, determine thumbs up or down right now */

  else sts = trytoconvert (mutex, oldmode, newmode);

  /* If it was successful, set new mode in channel */

  if (sts == OZ_SUCCESS) chnex -> curmode = newmode;

  /* If it was an successful down-convert (ie, we are allowing something more than we were before), maybe some waiters can go now */

  if ((sts == OZ_SUCCESS) && ((oz_lock_dncnvt[oldmode] >> newmode) & 1)) {
    for (lwaiter = &(mutex -> waiterqh); (waiter = *lwaiter) != NULL;) {	/* loop through list of waiters for the mutex */
      if ((waiter -> express) || (waiter == mutex -> waiterqh)) {		/* make sure it is either express or the first in the queue */
        sts = trytoconvert (mutex, waiter -> oldmode, waiter -> newmode);	/* ok, attempt the conversion */
        if (sts == OZ_ACCONFLICT) lwaiter = &(waiter -> next);			/* blocked, on to next in list */
        else {
          *lwaiter = waiter -> next;						/* success (or fatal error), unlink from list */
          if (*lwaiter == NULL) mutex -> waiterqt = lwaiter;
          if (sts == OZ_SUCCESS) waiter -> chnex -> curmode = waiter -> newmode; /* post it for completion */
          oz_knl_iodone (waiter -> ioop, sts, NULL, NULL, NULL);
        }
      }
    }
    sts = OZ_SUCCESS;
  }

  /* If blocked and not noqueue, queue it on end of wait queue for the mutex */

  if ((sts == OZ_ACCONFLICT) && !noqueue) {

    /* Make a new waiter entry on end of mutex's wait queue */

    iopex -> next    = NULL;
    iopex -> express = express;
    iopex -> oldmode = oldmode;
    iopex -> newmode = newmode;
    *(mutex -> waiterqt) = iopex;
    mutex -> waiterqt = &(iopex -> next);

    /* Call any applicable unblocker routines - maybe they will downconvert sufficiently to let this one go */

    for (lunblocker = &(mutex -> unblockerqh); (unblocker = *lunblocker) != NULL;) {
      if (!((oz_lock_compat[newmode] >> unblocker -> oldmode) & 1)) {		/* see if unblocker is blocking this one */
        *lunblocker = unblocker -> next;					/* if so, unlink unblocker from list */
        if (*lunblocker == NULL) mutex -> unblockerqt = lunblocker;
        oz_knl_iodone (unblocker -> ioop, OZ_SUCCESS, NULL, NULL, NULL);	/* post it for completion */
      } else {
        lunblocker = &(unblocker -> next);					/* if not, check out next in list */
      }
    }

    /* Say that the request was queued */

    sts = OZ_STARTED;
  }

  /* Return status of conversion attempt */

  return (sts);
}

/************************************************************************/
/*									*/
/*  Try to perform lock mode conversion					*/
/*									*/
/*    Input:								*/
/*									*/
/*	mutex = lock to convert						*/
/*	oldmode = mode caller currently has lock in			*/
/*	newmode = mode the caller wants to convert to			*/
/*									*/
/*    Output:								*/
/*									*/
/*	trytoconvert = OZ_SUCCESS : conversion successful		*/
/*	            OZ_ACCONFLICT : someone is blocking it		*/
/*									*/
/************************************************************************/

static uLong trytoconvert (Mutex *mutex, OZ_Lockmode oldmode, OZ_Lockmode newmode)

{
  Long active_readers, active_writers, block_readers, block_writers;
  Long others_read, others_write, self_read, self_write;

  /* Get what counts would be if we gave up the lock mode that we currently have */

  active_readers = mutex -> active_readers - OZ_LOCK_ALLOW_TEST (oldmode, OZ_LOCK_ALLOWS_SELF_READ);
  active_writers = mutex -> active_writers - OZ_LOCK_ALLOW_TEST (oldmode, OZ_LOCK_ALLOWS_SELF_WRITE);
  block_readers  = mutex -> block_readers - !OZ_LOCK_ALLOW_TEST (oldmode, OZ_LOCK_ALLOWS_OTHERS_READ);
  block_writers  = mutex -> block_writers - !OZ_LOCK_ALLOW_TEST (oldmode, OZ_LOCK_ALLOWS_OTHERS_WRITE);

  /* Of what's remaining, see if that would be compatible with the mode we want */

  self_read    =  OZ_LOCK_ALLOW_TEST (newmode, OZ_LOCK_ALLOWS_SELF_READ);
  self_write   =  OZ_LOCK_ALLOW_TEST (newmode, OZ_LOCK_ALLOWS_SELF_WRITE);
  others_read  = !OZ_LOCK_ALLOW_TEST (newmode, OZ_LOCK_ALLOWS_OTHERS_READ);
  others_write = !OZ_LOCK_ALLOW_TEST (newmode, OZ_LOCK_ALLOWS_OTHERS_WRITE);

  if ((self_read    && (block_readers  != 0)) 				/* can't if i want to read but readers are blocked */
   || (self_write   && (block_writers  != 0)) 				/* can't if i want to write but writers are blocked */
   || (others_read  && (active_readers != 0)) 				/* can't if i wont let others read but there are active readers */
   || (others_write && (active_writers != 0))) return (OZ_ACCONFLICT);	/* can't if i wont let others write but there are active writers */

  /* If so, modify the counts in the lock */

  mutex -> active_readers = active_readers + self_read;
  mutex -> active_writers = active_writers + self_write;
  mutex -> block_readers  = block_readers  + others_read;
  mutex -> block_writers  = block_writers  + others_write;

  /* Say we were successful at the conversion */

  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Declare an unblock routine to be called if someone wants the lock 	*/
/*  and we are blocking them						*/
/*									*/
/*    Input:								*/
/*									*/
/*	mutex = mutex							*/
/*	oldmode = mode i have it locked at				*/
/*									*/
/*    Output:								*/
/*									*/
/*	unblock = OZ_SUCCESS : there is already someone blocked		*/
/*	          OZ_STARTED : no one was blocked, queued		*/
/*									*/
/************************************************************************/

static uLong unblock (Iopex *iopex)

{
  Chnex *chnex;
  Iopex *waiter;
  Long active_readers, active_writers, ar, aw, block_readers, block_writers, br, bw;
  Mutex *mutex;
  OZ_Lockmode oldmode;

  chnex   = iopex -> chnex;
  mutex   = chnex -> mutex;
  oldmode = chnex -> curmode;
  if (mutex == NULL) return (OZ_FILENOTOPEN);

  /* Get what counts would be if we gave up the lock mode that we currently have */

  active_readers = mutex -> active_readers - OZ_LOCK_ALLOW_TEST (oldmode, OZ_LOCK_ALLOWS_SELF_READ);
  active_writers = mutex -> active_writers - OZ_LOCK_ALLOW_TEST (oldmode, OZ_LOCK_ALLOWS_SELF_WRITE);
  block_readers  = mutex -> block_readers - !OZ_LOCK_ALLOW_TEST (oldmode, OZ_LOCK_ALLOWS_OTHERS_READ);
  block_writers  = mutex -> block_writers - !OZ_LOCK_ALLOW_TEST (oldmode, OZ_LOCK_ALLOWS_OTHERS_WRITE);

  /* Given those counts, see if there are any waiters that could go */

  for (waiter = mutex -> waiterqh; waiter != NULL; waiter = waiter -> next) {

    /* Get what counts would be after getting rid of what waiter is currently doing */

    ar = active_readers - OZ_LOCK_ALLOW_TEST (waiter -> oldmode, OZ_LOCK_ALLOWS_SELF_READ);
    aw = active_writers - OZ_LOCK_ALLOW_TEST (waiter -> oldmode, OZ_LOCK_ALLOWS_SELF_WRITE);
    br = block_readers - !OZ_LOCK_ALLOW_TEST (waiter -> oldmode, OZ_LOCK_ALLOWS_OTHERS_READ);
    bw = block_writers - !OZ_LOCK_ALLOW_TEST (waiter -> oldmode, OZ_LOCK_ALLOWS_OTHERS_WRITE);

    /* See if it would still be blocked */

    if ( OZ_LOCK_ALLOW_TEST (waiter -> newmode, OZ_LOCK_ALLOWS_SELF_READ)    && (br != 0)) continue;
    if ( OZ_LOCK_ALLOW_TEST (waiter -> newmode, OZ_LOCK_ALLOWS_SELF_WRITE)   && (bw != 0)) continue;
    if (!OZ_LOCK_ALLOW_TEST (waiter -> newmode, OZ_LOCK_ALLOWS_OTHERS_READ)  && (ar != 0)) continue;
    if (!OZ_LOCK_ALLOW_TEST (waiter -> newmode, OZ_LOCK_ALLOWS_OTHERS_WRITE) && (aw != 0)) continue;

    /* It wouldn't be blocked, so the condition is satisfied */

    return (OZ_SUCCESS);
  }

  /* No one is blocked, queue request */

  iopex -> next = NULL;
  iopex -> oldmode = oldmode;
  *(mutex -> unblockerqt) = iopex;
  mutex -> unblockerqt = &(iopex -> next);
  return (OZ_STARTED);
}
