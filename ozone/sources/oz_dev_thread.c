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
/*  This is a platform independent thread control driver		*/
/*  (used to implement a debugger)					*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_io_thread.h"
#include "oz_knl_hw.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_lowipl.h"
#include "oz_knl_objtype.h"
#include "oz_knl_printk.h"
#include "oz_knl_sdata.h"
#include "oz_knl_status.h"
#include "oz_knl_thread.h"

typedef struct Chnex Chnex;
typedef struct Iopex Iopex;

struct Iopex { Iopex *next;
               Iopex **prev;
               OZ_Ioop *ioop;
             };

struct Chnex { Iopex *queue;
               OZ_Thread *thread;
               OZ_Mchargs mchargs;
             };
                 
static uLong oz_dev_thread_assign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode);
static void oz_dev_thread_abort (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Ioop *ioop, void *iopexv, OZ_Procmode procmode);
static uLong oz_dev_thread_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnex, OZ_Procmode procmode,
                                  OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap);

static const OZ_Devfunc oz_dev_thread_functable = { 0, sizeof (Chnex), sizeof (Iopex), 0, NULL, NULL, NULL, oz_dev_thread_assign, NULL, oz_dev_thread_abort, oz_dev_thread_start, NULL };

static int initialized = 0;
static OZ_Devclass *devclass;
static OZ_Devdriver *devdriver;
static OZ_Devunit *devunit;

/************************************************************************/
/*									*/
/*  Boot-time initialization routine					*/
/*									*/
/************************************************************************/

void oz_dev_thread_init (void)

{
  if (!initialized) {
    oz_knl_printk ("oz_dev_thread_init\n");
    initialized  = 1;
    devclass     = oz_knl_devclass_create (OZ_IO_THREAD_CLASSNAME, OZ_IO_THREAD_BASE, OZ_IO_THREAD_MASK, "oz_dev_thread");
    devdriver    = oz_knl_devdriver_create (devclass, "oz_dev_thread");
    devunit      = oz_knl_devunit_create (devdriver, "thread", "thread control", &oz_dev_thread_functable, 0, oz_s_secattr_tempdev);
  }
}

/************************************************************************/
/*									*/
/*  An new channel is being assigned - clear it out			*/
/*									*/
/************************************************************************/

static uLong oz_dev_thread_assign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode)

{
  memset (chnexv, 0, sizeof (Chnex));
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Abort an thread I/O request						*/
/*									*/
/************************************************************************/

static void oz_dev_thread_abort (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Ioop *ioop, void *iopexv, OZ_Procmode procmode)

{
  Chnex *chnex;
  Iopex *aborted, *iopex, **liopex, *niopex;
  uLong tm;

  chnex   = chnexv;
  aborted = NULL;
  tm = oz_hw_smplock_wait (oz_hw_smplock_tm);	/* lock database */
  for (liopex = &(chnex -> queue); (iopex = *liopex) != NULL;) {
    if (!oz_knl_ioabortok (iopex -> ioop, iochan, procmode, ioop) || !oz_knl_thread_remove (&(iopex -> thread))) {
      liopex = &(iopex -> next);		/* leave it as is, on to next */
    } else {
      niopex = iopex -> next;			/* unlink from chnex -> queue */
      if (niopex != NULL) niopex -> prev = liopex;
      *liopex = niopex;
      iopex -> next = aborted;			/* link to aborted */
      aborted = iopex;
    }
  }
  oz_hw_smplock_clr (oz_hw_smplock_tm, tm);	/* release database */

  while ((iopex = aborted) != NULL) {		/* abort all we found */
    aborted = iopex -> next;
    oz_knl_iodone (iopex -> ioop, OZ_ABORTED, NULL, NULL, NULL);
  }
}

/************************************************************************/
/*									*/
/*  Start performing a thread i/o function				*/
/*									*/
/************************************************************************/

static uLong oz_dev_thread_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode,
                                  OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap)

{
  Chnex *chnex;
  Iopex *iopex;

  chnex = chnexv;
  iopex = iopexv;

  iopex -> ioop = ioop;

  switch (funcode) {

    /* Open connection to a thread */

    case OZ_IO_THREAD_OPEN: {
      OZ_Ast *ast;
      OZ_IO_thread_open thread_open;
      OZ_Thread *thread;
      uLong ts;

      movc4 (as, ap, sizeof thread_open, &thread_open);
      sts = oz_knl_ioop_lockw (ioop, sizeof *thread_open.mchargs, thread_open.mchargs, 
                               &(iopex -> mchargs_phypages), NULL, &(iopex -> mchargs_byteoffs));
      if (sts == OZ_SUCCESS) sts = oz_knl_handle_lookup (NULL, thread_open.h_thread, procmode, OZ_SECACCMSK_WRITE, OZ_OBJTYPE_THREAD, &thread);
      if (sts != OZ_SUCCESS) return (sts);

      ts = oz_hw_smplock_wait (&oz_s_smplock_ts);
      sts = oz_knl_ast_create (thread, procmode, thread_opened, iopex, 1, NULL, &ast);
      if (sts == OZ_SUCCESS) oz_knl_thread_queueast (ast, OZ_SUCCESS);
      oz_hw_smplock_clr (&oz_s_smplock_ts, ts);
      if (sts != OZ_SUCCESS) {
        oz_knl_thread_increfc (thread, -1);
        return (sts);
      }
      return (OZ_STARTED);
    }



    case OZ_IO_THREAD_WAITUNTIL: {
      movc4 (as, ap, sizeof thread_waituntil, &thread_waituntil);
      iopex -> thread.objtype = OZ_OBJTYPE_THREAD;
      iopex -> thread.next    = &(iopex -> thread);
      iopex -> ioop = ioop;
      tm = oz_hw_smplock_wait (oz_hw_smplock_tm);
      iopex -> next = chnex -> queue;
      iopex -> prev = &(chnex -> queue);
      if (iopex -> next != NULL) iopex -> next -> prev = &(iopex -> next);
      chnex -> queue = iopex;
      oz_knl_thread_insert (&(iopex -> thread), thread_waituntil.datebin, iothreadisup, iopex);
      oz_hw_smplock_clr (oz_hw_smplock_tm, tm);
      return (OZ_STARTED);
    }

    default: {
      return (OZ_BADIOFUNC);
    }
  }
}

/************************************************************************/
/*									*/
/*  An thread has halted due to an open ast being queued		*/
/*  We are in the target thread context as an express ast		*/
/*									*/
/*    Input:								*/
/*									*/
/*	iopexv  = iopex pointer						*/
/*	status  = OZ_SUCCESS						*/
/*	mchargs = mchargs where the program was interrupted		*/
/*									*/
/************************************************************************/

static void thread_opened (void *iopexv, uLong status, OZ_Mchargs *mchargs)

{
  Chnex *chnex;
  Iopex *iopex;

  iopex = iopexv;
  chnex = iopex -> chnex;

  if (status != OZ_SUCCESS) {
    oz_knl_iodone (iopex -> ioop, status, NULL, NULL, NULL);
    return;
  }

  /* Return the mchargs to caller */

  chnex -> mchargs = *mchargs;
  oz_hw_phys_movefromvirt (sizeof chnex -> mchargs, &(chnex -> mchargs), iopex -> mchargs_phypages, iopex -> mchargs_byteoffs);

  /* Complete the I/O request */

  chnex -> waiting = 1;
  oz_knl_iodone (iopex -> ioop, OZ_SUCCESS, NULL, NULL, NULL);

  /* Wait for an OZ_IO_THREAD_CONT call */

  while (chnex -> waiting) {
    oz_knl_event_waitone (event);
    oz_knl_event_set (event, 0);
  }

  /* Get new mchargs from caller */

  *mchargs = chnex -> mchargs;

  /* Resume processing with new mchargs */

  oz_knl_thread_debhalt (thread_halted, chnex);
}

static void thread_halted (void *chnexv, OZ_Sigargs sigargs[], OZ_Mchargs *mchargs)

{
  Chnex *chnex;

  chnex = chnexv;

  
}
