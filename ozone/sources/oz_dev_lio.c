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
/*  Layered driver I/O routines						*/
/*									*/
/************************************************************************/

#define _OZ_DEV_LIO_C

#include "ozone.h"
#include "oz_dev_lio.h"
#include "oz_knl_devio.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_objtype.h"
#include "oz_knl_sdata.h"
#include "oz_knl_status.h"

struct OZ_Liod { OZ_Objtype objtype;		/* OZ_OBJTYPE_LIOD */
                 OZ_Iochan *iochan;		/* I/O channel to scsi controller */
                 OZ_Lior *liors;		/* list of all I/O's in progress on device */
                 OZ_Smplock smplock;		/* smp lock for misc use */
               };

struct OZ_Lior { OZ_Objtype objtype;		/* OZ_OBJTYPE_LIOR */
                 OZ_Liod *liod;			/* the liod this request is for */
                 OZ_Ioop *ioop;			/* corresponding disk ioop */
                 OZ_Procmode procmode;		/* caller's procmode */
                 OZ_Lior *next;			/* next in devex->liors queue */
                 OZ_Lior **prev;		/* prev in devex->liors queue */
						/* (NULL if not in devex->liors queue) */
                 OZ_Ioop *scsiioop;		/* scsi controller ioop */
						/* (NULL if none, else i have a refcount on it) */
                 int aborted;			/* request has been aborted */
                 Long refcount;			/* reference count */
                 uLong status;			/* completion status */
                 void (*finentry) (void *finparam, int finok, uLong *status_r);
                 void *finparam;
               };

static void decrefcount (OZ_Lior *lior);

/************************************************************************/
/*									*/
/*  This routine is called in the disk device's init routine to set up 	*/
/*  the layered io device struct					*/
/*									*/
/*    Input:								*/
/*									*/
/*	iochan = scsi controller i/o channel				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_dev_lio_init = device context				*/
/*									*/
/************************************************************************/

OZ_Liod *oz_dev_lio_init (OZ_Iochan *iochan)

{
  OZ_Liod *liod;

  liod = OZ_KNL_NPPMALLOC (sizeof *liod);
  liod -> objtype = OZ_OBJTYPE_LIOD;
  liod -> iochan  = iochan;
  liod -> liors   = NULL;
  oz_hw_smplock_init (sizeof liod -> smplock, &(liod -> smplock), OZ_SMPLOCK_LEVEL_VL);

  return (liod);
}

/************************************************************************/
/*									*/
/*  Disk device's clonedel routine can call this when the disk device 	*/
/*  is about to be deleted						*/
/*									*/
/************************************************************************/

void oz_dev_lio_term (OZ_Liod *liod)

{
  OZ_KNL_CHKOBJTYPE (liod, OZ_OBJTYPE_LIOD);
  if (liod -> liors != NULL) oz_crash ("oz_dev_lio_term: there are requeusts on the queue");
  oz_knl_iochan_increfc (liod -> iochan, -1);
  OZ_KNL_NPPFREE (liod);
}

/************************************************************************/
/*									*/
/*  This routine is called by the disk driver's abort routine.  It 	*/
/*  will abort any I/O going to the scsi controller.			*/
/*									*/
/*    Input:								*/
/*									*/
/*	liod = as returned by oz_dev_lio_init for this disk device	*/
/*	iochan = disk i/o channel being aborted				*/
/*	ioop = disk i/o request being aborted				*/
/*	procmode = processor mode of the abort				*/
/*									*/
/*    Output:								*/
/*									*/
/*	scsi i/o requests tagged for abort				*/
/*									*/
/************************************************************************/

void oz_dev_lio_abort (OZ_Liod *liod, OZ_Iochan *iochan, OZ_Ioop *ioop, OZ_Procmode procmode)

{
  int stillinqueue;
  OZ_Lior *lior, **llior;
  OZ_Ioop *scsiioop;
  uLong dx;

  OZ_KNL_CHKOBJTYPE (liod, OZ_OBJTYPE_LIOD);

  dx = oz_hw_smplock_wait (&(liod -> smplock));				/* lock the queue */
scan:
  for (llior = &(liod -> liors); (lior = *llior) != NULL; llior = &(lior -> next)) { /* scan the queue */
    if (lior -> prev != llior) oz_crash ("oz_dev_lio_abort: lior -> prev is corrupt");
    if (!oz_knl_ioabortok (lior -> ioop, iochan, procmode, ioop)) continue; /* skip till we find something to abort */
    lior -> aborted = 1;						/* ok, mark it aborted */
    scsiioop = lior -> scsiioop;					/* see if there is any scsi i/o going on */
    if (scsiioop == NULL) continue;					/* if not, that's all we do for this one */
    oz_knl_ioop_increfc (lior -> ioop, 1);				/* make sure either ioop doesn't get freed on us */
    oz_knl_ioop_increfc (scsiioop, 1);
    oz_hw_smplock_clr (&(liod -> smplock), dx);				/* release the lock */
    oz_knl_ioabort2 (liod -> iochan, procmode, scsiioop);		/* tell scsi controller to abort the request */
    dx = oz_hw_smplock_wait (&(liod -> smplock));			/* lock the queue */
    stillinqueue = (lior -> prev != NULL);				/* see if request is still in the queue */
    oz_knl_ioop_increfc (lior -> ioop, -1);				/* decrement ref count back (maybe it gets freed off) */
    oz_knl_ioop_increfc (scsiioop, -1);
    if (!stillinqueue) goto scan;					/* if it was removed from queue, start scan over */
  }
  oz_hw_smplock_clr (&(liod -> smplock), dx);
}

/************************************************************************/
/*									*/
/*  This routine is called in the disk driver's start i/o routine to 	*/
/*  init the struct used to keep track of requests to the scsi 		*/
/*  controller								*/
/*									*/
/*    Input:								*/
/*									*/
/*	liod = liod of disk the new i/o request is for			*/
/*	ioop = disk i/o operation struct pointer			*/
/*	procmode = processor mode of the request			*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_dev_lio_start = passed to oz_dev_lio_io to start an scsi i/o	*/
/*	                   passed to oz_dev_lio_done when req complete	*/
/*									*/
/************************************************************************/

OZ_Lior *oz_dev_lio_start (OZ_Liod *liod, OZ_Ioop *ioop, OZ_Procmode procmode)

{
  OZ_Lior *lior;

  OZ_KNL_CHKOBJTYPE (liod, OZ_OBJTYPE_LIOD);

  lior = OZ_KNL_NPPMALLOC (sizeof *lior);
  lior -> objtype  = OZ_OBJTYPE_LIOR;
  lior -> liod     = liod;
  lior -> ioop     = ioop;
  lior -> procmode = procmode;

  lior -> prev     = NULL;		/* it's not in the devex->liors queue yet */
  lior -> scsiioop = NULL;		/* it doesn't have an scsi i/o going */
  lior -> aborted  = 0;			/* it hasn't been aborted yet via scsi_disk_abort */
  lior -> refcount = 1;			/* iodone has yet to be called */

  return (lior);
}

/************************************************************************/
/*									*/
/*  Start an I/O on the scsi controller					*/
/*									*/
/*    Input:								*/
/*									*/
/*	lior     = as returned by oz_dev_lio_start			*/
/*	procmode = processor mode of the request			*/
/*	astentry = completion routine to be called			*/
/*	astparam = param to pass to astentry routine			*/
/*	funcode  = function code to be performed			*/
/*	as,ap    = arg block size and pointer				*/
/*									*/
/*    Output:								*/
/*									*/
/*	astentry will be called when operation completes		*/
/*									*/
/************************************************************************/

void oz_dev_lio_io (OZ_Lior *lior, OZ_Procmode procmode, 
                     void (*astentry) (void *astparam, uLong status), void *astparam, 
                     uLong funcode, uLong as, void *ap)

{
  OZ_Ioop *scsiioop;
  OZ_Liod *liod;
  OZ_Lior *nlior;
  uLong dx, sts;

  OZ_KNL_CHKOBJTYPE (lior, OZ_OBJTYPE_LIOR);
  liod = lior -> liod;

  /* If the disk request was aborted, don't start anything new */

  if (lior -> aborted) {
    (*astentry) (astparam, OZ_ABORTED);
    return;
  }

  /* If there was a prior scsi io for this request, free it off */

  scsiioop = lior -> scsiioop;
  if (scsiioop != NULL) {
    lior -> scsiioop = NULL;
    OZ_HW_MB;
    oz_knl_ioop_increfc (scsiioop, -1);
  }

  /* Make sure I/O doesn't get posted on us (the astentry routine might get */
  /* called during the iostart3 routine and it might call oz_dev_lio_done)  */

  OZ_HW_ATOMIC_INCBY1_LONG (lior -> refcount);

  /* Start the I/O operation on the scsi controller */

  sts = oz_knl_iostart3 (1, &scsiioop, liod -> iochan, procmode, astentry, astparam, NULL, NULL, NULL, NULL, funcode, as, ap);

  /* If scsi I/O will (or has) complete(d) asynchronously, link so it can be aborted */

  if (sts == OZ_STARTED) {
    dx = oz_hw_smplock_wait (&(liod -> smplock));		/* lock the queue */

    if (lior -> prev != NULL) {					/* make sure lior is in liod -> liors queue */
      lior -> next  = nlior = liod -> liors;			/* ... so the abort routine can see it */
      lior -> prev  = &(liod -> liors);
      liod -> liors = lior;
      if (nlior != NULL) nlior -> prev = &(lior -> next);
    }

    if (lior -> scsiioop == NULL) lior -> scsiioop = scsiioop;	/* if astentry didn't start something new, link it */
    else oz_knl_ioop_increfc (scsiioop, -1);			/* otherwise, free the scsiioop off */

    oz_hw_smplock_clr (&(liod -> smplock), dx);			/* unlock the queue */
  }

  /* We no longer need lior to be valid so dec its ref count - if the ast entry routine */
  /* has already called oz_dev_lio_done then the request will be freed off here         */

  decrefcount (lior);						/* ok to post I/O completion now */

  /* If it completed synchronously, pretend like it just completed asynchronously */

  if (sts != OZ_STARTED) (*astentry) (astparam, sts);
}

/************************************************************************/
/*									*/
/*  This routine is called when the disk I/O operation is complete, in 	*/
/*  place of (en lieu de) calling oz_knl_iodone				*/
/*									*/
/*    Input:								*/
/*									*/
/*	lior     = disk i/o that is now complete			*/
/*	status   = the completion status				*/
/*	finentry = NULL : no caller-space callback			*/
/*	           else : caller-space callback entrypoint		*/
/*	finparam = param to pass to finentry routine			*/
/*									*/
/*    Output:								*/
/*									*/
/*	request is posted for completion				*/
/*	lior is no longer valid (as far as the caller is concerned)	*/
/*									*/
/************************************************************************/

void oz_dev_lio_done (OZ_Lior *lior, uLong status, void (*finentry) (void *finparam, int finok, uLong *status_r), void *finparam)

{
  OZ_KNL_CHKOBJTYPE (lior, OZ_OBJTYPE_LIOR);

  /* Save the status and finish routine stuff */

  lior -> status   = status;
  lior -> finentry = finentry;
  lior -> finparam = finparam;

  /* Dec ref count.  We might not be able to post completion yet because there still might be refernces to it. */

  decrefcount (lior);
}

/************************************************************************/
/*									*/
/*  Decrement requests reference count.  If zero, post the i/o for 	*/
/*  completion and free off request.					*/
/*									*/
/************************************************************************/

static void decrefcount (OZ_Lior *lior)

{
  Long refc;
  OZ_Liod *liod;
  OZ_Lior *nlior;
  uLong dx;

again:
  do {
    refc = lior -> refcount;
    if (refc <= 1) goto going_le_zero;
  } while (!oz_hw_atomic_setif_long (&(lior -> refcount), refc - 1, refc));
  return;

going_le_zero:
  if (refc <= 0) oz_crash ("oz_dev_lio decrefcount: refcount was %d", refc);
  liod = lior -> liod;
  dx = oz_hw_smplock_wait (&(liod -> smplock));				// lock abort queue first
  if (!oz_hw_atomic_setif_long (&(lior -> refcount), 0, 1)) {		// now set it to zero
    oz_hw_smplock_clr (&(liod -> smplock), dx);				// (refcount changed on us)
    goto again;								// (so go try it all again)
  }

  /* Remove from abort queue if it is in there */

  if (lior -> prev != NULL) {
    *(lior -> prev) = nlior = lior -> next;
    if (nlior != NULL) nlior -> prev = lior -> prev;
    lior -> prev = NULL;
  }

  oz_hw_smplock_clr (&(liod -> smplock), dx);

  /* If there is a scsiioop there, dec its ref count */

  if (lior -> scsiioop != NULL) oz_knl_ioop_increfc (lior -> scsiioop, -1);

  /* Now it is ok to post the request for completion */

  oz_knl_iodone (lior -> ioop, lior -> status, NULL, lior -> finentry, lior -> finparam);

  OZ_KNL_NPPFREE (lior);
}
