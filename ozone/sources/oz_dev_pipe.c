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
/*  Pipe class driver							*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_dev_timer.h"
#include "oz_io_fs.h"
#include "oz_io_pipe.h"
#include "oz_knl_debug.h"
#include "oz_knl_devio.h"
#include "oz_knl_hw.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_process.h"
#include "oz_knl_procmode.h"
#include "oz_knl_sdata.h"
#include "oz_knl_status.h"
#include "oz_knl_thread.h"
#include "oz_sys_dateconv.h"

#define LOCKPIPE(__devex) while (oz_knl_event_set (__devex -> lockevent, 0) <= 0) oz_knl_event_waitone (__devex -> lockevent)
#define UNLKPIPE(__devex) oz_knl_event_set (__devex -> lockevent, 1)

typedef struct Chnex Chnex;
typedef struct Devex Devex;
typedef struct Iopex Iopex;

struct Chnex { int reader;
               int writer;
               int ignclo;
             };

struct Devex { int record;			/* 0 : stream-oriented; 1 : record-orented */
               Iopex *writeqh, **writeqt;	/* write request queue */
               Iopex *readqh,  **readqt;	/* read request queue */
               OZ_Event *lockevent;		/* locking event flag */
               int insert;			/* insertion point offset in buffer */
               int remove;			/* removal point offset in buffer */
               int readerclosed;		/* 0 : normal */
						/* 1 : reader closed its channel, block abort any writes */
               uByte buffer[OZ_IO_PIPE_BUFSIZ];	/* pipe data buffer */
             };

struct Iopex { Iopex *next;			/* next in the read or write queue */
               OZ_Ioop *ioop;			/* corresponding i/o operation */
               OZ_Thread *thread;		/* requestor thread */
               OZ_Threadid threadid;		/* thread id of thread that wrote the data (record-oriented read and write requests) */
               uLong size;			/* size of user supplied w_buff or r_buff */
               const uByte *w_buff;		/* pointer to buffer being written from (in thread's address space) */
               uByte *r_buff;			/* pointer to buffer being read into (in thread's address space) */
               uLong *rlen;			/* where to return actual length read */
               uLong trmsize;			/* terminator size */
               const uByte *trmbuff;		/* terminator buffer (in process' address space) */
               uLong threadidleft;		/* amount left to read or write of the threadid valid */
               uLong recsizeleft;		/* amount left to read of the recsize value */
               uLong sizeleft;			/* amount left to write of the size value */
               uLong trmbuffleft;		/* amount left to write from trmbuff */
               uLong buffleft;			/* amount left to write from w_buff */
               uLong recsize;			/* size of record actually written */
               uLong done;			/* number of bytes actually read so far (excluding terminator) */
             };

static uLong pipe_clonecre (OZ_Devunit *template_devunit, void *template_devex, int template_cloned, OZ_Procmode procmode, OZ_Devunit **cloned_devunit);
static uLong pipe_assign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode);
static int pipe_deassign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv);
static int pipe_clonedel (OZ_Devunit *devunit, void *devexv, int cloned);
static void pipe_abort (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Ioop *ioop, void *iopexv, OZ_Procmode procmode);
static uLong pipe_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode, OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap);
static uLong nullzero_do (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode, OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap);

static const OZ_Devfunc pipe_functable = { sizeof (Devex), sizeof (Chnex), sizeof (Iopex), 0, 
                                           NULL, pipe_clonecre, pipe_clonedel, pipe_assign, pipe_deassign, pipe_abort, pipe_start, NULL };

static const OZ_Devfunc nullzero_functable = { 0, 0, 0, 0, NULL, NULL, NULL, NULL, NULL, NULL, nullzero_do, NULL };

/* Static data and internal functions */

static int initialized = 0;
static OZ_Devclass *devclass;
static OZ_Devdriver *devdriver;
static OZ_Devunit *null_devunit;
static OZ_Devunit *record_devunit;
static OZ_Devunit *stream_devunit;
static OZ_Devunit *zero_devunit;

static void closedone (void *dummy, uLong status);
static void checkwriteq (Devex *devex);
static int writerecreq (Devex *devex, Iopex *iopex);
static uLong writerecbuf (Devex *devex, uLong size, const uByte *buff);
static int writestrreq (Devex *devex, Iopex *iopex);
static uLong writestrbuf (Devex *devex, uLong size, const uByte *buff);
static uLong writeroom (Devex *devex);
static void checkreadq (Devex *devex);
static uLong readrecreq (Devex *devex, Iopex *iopex);
static uLong readrecbuf (Devex *devex, uLong size, uByte *buff);
static uLong readstrreq (Devex *devex, Iopex *iopex);

/************************************************************************/
/*									*/
/*  Boot-time initialization routine					*/
/*									*/
/************************************************************************/

void oz_dev_pipe_init ()

{
  Devex *devex;

  if (!initialized) {
    oz_knl_printk ("oz_dev_pipe_init\n");
    initialized = 1;

    /* Create template device for port drivers to connect to */

    devclass        = oz_knl_devclass_create (OZ_IO_PIPE_CLASSNAME, OZ_IO_PIPE_BASE, OZ_IO_PIPE_MASK, "oz_dev_pipe");
    devdriver       = oz_knl_devdriver_create (devclass, "oz_dev_pipe");

    null_devunit    = oz_knl_devunit_create (devdriver, OZ_IO_NULL_DEVICE, "returns EOF for reads", &nullzero_functable, 0, oz_s_secattr_tempdev);

    record_devunit  = oz_knl_devunit_create (devdriver, OZ_IO_PIPER_TEMPLATE, "record style template", &pipe_functable, 0, oz_s_secattr_tempdev);
    devex           = oz_knl_devunit_ex (record_devunit);
    memset (devex, 0, sizeof *devex);
    devex -> record = 1;

    stream_devunit  = oz_knl_devunit_create (devdriver, OZ_IO_PIPES_TEMPLATE, "stream style template", &pipe_functable, 0, oz_s_secattr_tempdev);
    devex           = oz_knl_devunit_ex (stream_devunit);
    memset (devex, 0, sizeof *devex);

    zero_devunit    = oz_knl_devunit_create (devdriver, OZ_IO_ZERO_DEVICE, "returns zeroes for reads", &nullzero_functable, 0, oz_s_secattr_tempdev);
  }
}

/************************************************************************/
/*									*/
/*  Someone is trying to assign a channel to the template device, so 	*/
/*  we create a 'real' device for them to use				*/
/*									*/
/*    Input:								*/
/*									*/
/*	template_devunit = points to piper_devunit or pipes_devunit	*/
/*	template_devex   = points to corresponding devex area		*/
/*	template_cloned  = indicates if the template is cloned		*/
/*	procmode         = procmode that is assigning channel		*/
/*	smplock level    = dv						*/
/*									*/
/*    Output:								*/
/*									*/
/*	pipe_cloncre = OZ_SUCCESS : clone device created		*/
/*	                     else : error status			*/
/*	*cloned_devunit = cloned device unit struct pointer		*/
/*									*/
/************************************************************************/

static uLong pipe_clonecre (OZ_Devunit *template_devunit, void *template_devex, int template_cloned, OZ_Procmode procmode, OZ_Devunit **cloned_devunit)

{
  char unitname[32];
  Devex *devex;
  int i;
  OZ_Devunit *devunit;
  OZ_Event *lockevent;
  OZ_Secattr *secattr;
  uLong sts;

  static uLong seq = 0;

  if (template_cloned) {
    *cloned_devunit = template_devunit;
    oz_knl_devunit_increfc (template_devunit, 1);
  } else {
    strncpyz (unitname, oz_knl_devunit_devname (template_devunit), sizeof unitname - 2);
    i = strlen (unitname);
    unitname[i++] = '.';
    oz_hw_itoa (++ seq, sizeof unitname - i, unitname + i);
    sts = oz_knl_event_create (strlen (unitname), unitname, NULL, &lockevent);
    if (sts != OZ_SUCCESS) return (sts);
    secattr = oz_knl_thread_getdefcresecattr (NULL);
    devunit = oz_knl_devunit_create (devdriver, unitname, unitname, &pipe_functable, 1, secattr);
    oz_knl_secattr_increfc (secattr, -1);
    devex   = oz_knl_devunit_ex (devunit);
    memset (devex, 0, sizeof *devex);
    devex -> record    = ((Devex *)template_devex) -> record;
    devex -> writeqt   = &(devex -> writeqh);
    devex -> readqt    = &(devex -> readqh);
    devex -> lockevent = lockevent;
    UNLKPIPE (devex);
    *cloned_devunit = devunit;
  }
  return (OZ_SUCCESS);
}

static uLong pipe_assign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode)

{
  memset (chnexv, 0, sizeof (Chnex));
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Channel is being deassigned - close it				*/
/*  We are guaranteed there are no I/O's pending from the channel	*/
/*									*/
/************************************************************************/

static int pipe_deassign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv)

{
  Chnex *chnex;
  Devex *devex;
  uLong sts;

  chnex = chnexv;
  devex = devexv;

  if (chnex -> reader || chnex -> writer) {
    chnex -> ignclo = 0;
    sts = oz_knl_iostart3 (1, NULL, iochan, OZ_PROCMODE_KNL, closedone, NULL, NULL, NULL, NULL, NULL, OZ_IO_FS_CLOSE, 0, NULL);
    if (sts == OZ_STARTED) return (1);
    if (chnex -> reader || chnex -> writer) oz_crash ("oz_dev_pipe deassign: channel didn't close, sts %u", sts);
  }

  return (0);
}

static void closedone (void *dummy, uLong status)

{ }

/************************************************************************/
/*									*/
/*  All channels have been deassigned from pipe - delete it from 	*/
/*  system								*/
/*									*/
/*    Input:								*/
/*									*/
/*	smplock = dv							*/
/*									*/
/*    Output:								*/
/*									*/
/*	app_clonedel = 0 : retain the device don't delete it		*/
/*	               1 : delete the device				*/
/*									*/
/************************************************************************/

static int pipe_clonedel (OZ_Devunit *devunit, void *devexv, int cloned)

{
  Devex *devex;

  devex = devexv;

  if (cloned) {
    if ((devex -> readqh != NULL) || (devex -> writeqh != NULL)) {
      oz_crash ("oz_dev_pipe clonedel: still have requests in read %p/write %p queues", devex -> readqh, devex -> writeqh);
    }
    oz_knl_event_increfc (devex -> lockevent, -1);
    memset (devex, 0, sizeof *devex); /* ?? paranoia ?? */
  }

  return (cloned);
}

/************************************************************************/
/*									*/
/*  Abort an i/o request						*/
/*									*/
/************************************************************************/

static void pipe_abort (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Ioop *ioop, void *iopexv, OZ_Procmode procmode)

{
  Devex *devex;
  Iopex *iopex, **liopex;
  OZ_Process *oldprocess;

  devex = devexv;

  /* Lock pipe database */

  LOCKPIPE (devex);

  /* Abort applicable requests from read queue */

  for (liopex = &(devex -> readqh); (iopex = *liopex) != NULL;) {
    if (!oz_knl_ioabortok (iopex -> ioop, iochan, procmode, ioop)) liopex = &(iopex -> next);
    else {
      *liopex = iopex -> next;
      oldprocess = oz_knl_process_getcur ();
      oz_knl_process_setcur (oz_knl_ioop_getprocess (iopex -> ioop));
      if (iopex -> rlen != NULL) *(iopex -> rlen) = iopex -> done;
      oz_knl_iodone (iopex -> ioop, OZ_ABORTED, NULL, NULL, NULL);
      oz_knl_process_setcur (oldprocess);
    }
  }
  devex -> readqt = liopex;

  /* Abort applicable requests from write queue */

  for (liopex = &(devex -> writeqh); (iopex = *liopex) != NULL;) {
    if (!oz_knl_ioabortok (iopex -> ioop, iochan, procmode, ioop)) liopex = &(iopex -> next);
    else {
      *liopex = iopex -> next;
      oz_knl_iodone (iopex -> ioop, OZ_ABORTED, NULL, NULL, NULL);
    }
  }
  devex -> writeqt = liopex;

  /* Maybe some other request can be processed now */

  checkreadq (devex);
  checkwriteq (devex);

  /* Anyway, unlock and return */

  UNLKPIPE (devex);
}

/************************************************************************/
/*									*/
/*  Start performing an i/o request					*/
/*									*/
/************************************************************************/

static uLong pipe_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode, OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap)

{
  Chnex *chnex;
  Devex *devex;
  Iopex *iopex;
  uLong sts;

  iopex = iopexv;
  chnex = chnexv;
  devex = devexv;

  memset (iopex, 0x96, sizeof *iopex); /** ?? fill with garbazhe **/
  iopex -> ioop   = ioop;
  iopex -> thread = oz_knl_ioop_getthread (ioop);

  switch (funcode) {

    /* Open file - causes writes to abort when file is closed */

    case OZ_IO_FS_OPEN: {
      OZ_IO_fs_open fs_open;

      movc4 (as, ap, sizeof fs_open, &fs_open);
      LOCKPIPE (devex);
      chnex -> reader = 1;
      chnex -> writer = 0;
      chnex -> ignclo = fs_open.ignclose;
      devex -> readerclosed = 0;
      UNLKPIPE (devex);
      return (OZ_SUCCESS);
    }

    /* Create file - causes an eof mark to be written when file is closed */

    case OZ_IO_FS_CREATE: {
      OZ_IO_fs_create fs_create;

      movc4 (as, ap, sizeof fs_create, &fs_create);
      LOCKPIPE (devex);
      chnex -> reader = 0;
      chnex -> writer = 1;
      chnex -> ignclo = fs_create.ignclose;
      UNLKPIPE (devex);
      return (OZ_SUCCESS);
    }

    /* Write a record */

    case OZ_IO_FS_WRITEREC: {
      OZ_IO_fs_writerec fs_writerec;

      movc4 (as, ap, sizeof fs_writerec, &fs_writerec);
      LOCKPIPE (devex);

      if (devex -> readerclosed) {
        UNLKPIPE (devex);
        return (OZ_PIPECLOSED);
      }

      /* Record style: write threadid, buffer size, buffer */

      if (devex -> record) {
        sts = oz_knl_ioop_lockr (ioop, fs_writerec.size, fs_writerec.buff, NULL, NULL, NULL);
        if (sts == OZ_SUCCESS) {
          iopex -> threadid     = oz_knl_thread_getid (iopex -> thread);
          iopex -> size         = fs_writerec.size;
          iopex -> w_buff       = fs_writerec.buff;
          iopex -> threadidleft = sizeof iopex -> threadid;
          iopex -> sizeleft     = sizeof iopex -> size;
          iopex -> buffleft     = fs_writerec.size;
          if ((devex -> writeqh != NULL) || !writerecreq (devex, iopex)) {
            iopex -> next = NULL;
            *(devex -> writeqt) = iopex;
            devex -> writeqt = &(iopex -> next);
            sts = OZ_STARTED;
          }
        }
      }

      /* Stream style: write buffer, terminator */

      else {
        sts = oz_knl_ioop_lockr (ioop, fs_writerec.size, fs_writerec.buff, NULL, NULL, NULL);
        if (sts == OZ_SUCCESS) sts = oz_knl_ioop_lockr (ioop, fs_writerec.trmsize, fs_writerec.trmbuff, NULL, NULL, NULL);
        if (sts == OZ_SUCCESS) {
          iopex -> size        = fs_writerec.size;
          iopex -> w_buff      = fs_writerec.buff;
          iopex -> trmsize     = fs_writerec.trmsize;
          iopex -> trmbuff     = fs_writerec.trmbuff;
          iopex -> buffleft    = fs_writerec.size;
          iopex -> trmbuffleft = fs_writerec.trmsize;
          if ((devex -> writeqh != NULL) || !writestrreq (devex, iopex)) {
            iopex -> next = NULL;
            *(devex -> writeqt) = iopex;
            devex -> writeqt = &(iopex -> next);
            sts = OZ_STARTED;
          }
        }
      }

      /* We (probably) just wrote something, so check any pending read requests */

      checkreadq (devex);

      /* Unlock and return queuing status */

      UNLKPIPE (devex);
      return (sts);
    }

    /* Close file -                                                            */
    /* If user opened with OZ_IO_FS_CREATE, an eof mark is written to the pipe */

    case OZ_IO_FS_CLOSE: {
      sts = OZ_SUCCESS;
      LOCKPIPE (devex);
      if (!(chnex -> ignclo)) {

        /* If a reader is closing channel, abort any pending and future writes until reader re-opens */

        if (chnex -> reader) {
          devex -> readerclosed = 1;
          devex -> insert = 0;
          devex -> remove = 0;
          while ((iopex = devex -> writeqh) != NULL) {
            devex -> writeqh = iopex -> next;
            oz_knl_iodone (iopex -> ioop, OZ_PIPECLOSED, NULL, NULL, NULL);
          }
          devex -> writeqt = &(devex -> writeqh);
        }

        /* If writer is closing channel, write an eof mark to pipe */

        if (chnex -> writer) {
          if (devex -> readerclosed) sts = OZ_PIPECLOSED;
          else {

            /* Record style: write threadid, buffer size=-1, no data */

            if (devex -> record) {
              iopex -> threadid     = oz_knl_thread_getid (iopex -> thread);
              iopex -> size         = (uLong)(-1);
              iopex -> w_buff       = NULL;
              iopex -> threadidleft = sizeof iopex -> threadid;
              iopex -> sizeleft     = sizeof iopex -> size;
              iopex -> buffleft     = 0;
              if ((devex -> writeqh != NULL) || !writerecreq (devex, iopex)) {
                iopex -> next = NULL;
                *(devex -> writeqt) = iopex;
                devex -> writeqt = &(iopex -> next);
                sts = OZ_STARTED;
              }
            }

            /* Stream style: write sizeflag=255 */

            else {
              iopex -> size        = -1;
              iopex -> w_buff      = NULL;
              iopex -> trmsize     = 0;
              iopex -> trmbuff     = NULL;
              iopex -> buffleft    = 0;
              iopex -> trmbuffleft = 0;
              if ((devex -> writeqh != NULL) || !writestrreq (devex, iopex)) {
                iopex -> next = NULL;
                *(devex -> writeqt) = iopex;
                devex -> writeqt = &(iopex -> next);
                sts = OZ_STARTED;
              }
            }

            /* We (probably) just wrote something, so check any pending read requests */

            checkreadq (devex);
          }
        }

        /* At any rate, channel is no longer a reader or writer */

        chnex -> reader = 0;
        chnex -> writer = 0;
      }
      UNLKPIPE (devex);
      return (sts);
    }

    /* Read a record */

    case OZ_IO_FS_READREC: {
      OZ_IO_fs_readrec fs_readrec;

      movc4 (as, ap, sizeof fs_readrec, &fs_readrec);
      LOCKPIPE (devex);
      if (devex -> record) {
        sts = oz_knl_ioop_lockw (ioop, fs_readrec.size, fs_readrec.buff, NULL, NULL, NULL);
        if ((sts == OZ_SUCCESS) && (fs_readrec.rlen != NULL)) sts = oz_knl_ioop_lockw (ioop, sizeof *fs_readrec.rlen, fs_readrec.rlen, NULL, NULL, NULL);
        if (sts == OZ_SUCCESS) {
          iopex -> done         = 0;					/* haven't read any data into 'buff' yet */
          iopex -> rlen         = fs_readrec.rlen;			/* where to return length actually read in */
          iopex -> size         = fs_readrec.size;			/* size of 'buff' */
          iopex -> r_buff       = fs_readrec.buff;			/* where to read data into */
          iopex -> threadidleft = sizeof iopex -> threadid;		/* how much of threadid has yet to be read */
          iopex -> recsizeleft  = sizeof iopex -> recsize;		/* how much of record size has yet to be read */
          iopex -> threadid     = 0;					/* just to init it to something nice */
          iopex -> recsize      = 0;					/* ditto */
          if ((devex -> readqh != NULL) || ((sts = readrecreq (devex, iopex)) == OZ_PENDING)) {
            iopex -> next      = NULL;					/* put request on end of read queue */
            *(devex -> readqt) = iopex;					/* ... if there was already something in the queue */
            devex -> readqt    = &(iopex -> next);			/* ... or if this one can't complete right now */
            sts = OZ_STARTED;						/* it will complete asynchronously */
          }
          else if (iopex -> rlen != NULL) *(iopex -> rlen) = iopex -> done; /* it completed synchronously, maybe return length actually read in */
        }
      } else {
        sts = oz_knl_ioop_lockw (ioop, fs_readrec.size, fs_readrec.buff, NULL, NULL, NULL);
        if (sts == OZ_SUCCESS) sts = oz_knl_ioop_lockr (ioop, fs_readrec.trmsize, fs_readrec.trmbuff, NULL, NULL, NULL);
        if ((sts == OZ_SUCCESS) && (fs_readrec.rlen != NULL)) sts = oz_knl_ioop_lockw (ioop, sizeof *fs_readrec.rlen, fs_readrec.rlen, NULL, NULL, NULL);
        if (sts == OZ_SUCCESS) {
          iopex -> done    = 0;						/* haven't read any data into 'buff' yet */
          iopex -> rlen    = fs_readrec.rlen;				/* where to return actual number of bytes read, excluding terminator */
          iopex -> size    = fs_readrec.size;				/* size of the 'buff' */
          iopex -> r_buff  = fs_readrec.buff;				/* address of 'buff' */
          iopex -> trmsize = fs_readrec.trmsize;			/* terminator size */
          iopex -> trmbuff = fs_readrec.trmbuff;			/* terminator address */
          if ((devex -> readqh != NULL) || ((sts = readstrreq (devex, iopex)) == OZ_PENDING)) {
            iopex -> next      = NULL;					/* put request on end of read queue */
            *(devex -> readqt) = iopex;					/* ... if there was already something in the queue */
            devex -> readqt    = &(iopex -> next);			/* ... or if this one can't complete right now */
            sts = OZ_STARTED;						/* it will complete asynchronously */
          }
          else {
            if (iopex -> rlen != NULL) *(iopex -> rlen) = iopex -> done; /* it completed synchronously, maybe return length actually read in */
          }
        }
      }
      checkwriteq (devex);						/* there may be room in pipe buffer for more data, so check pending writes */
      UNLKPIPE (devex);							/* unlock pipe buffer */
      return (sts);							/* return queuing status */
    }

    /* Who knows what */

    default: return (OZ_BADIOFUNC);
  }
}

/************************************************************************/
/*									*/
/*  Process any requests lingering in the write queue			*/
/*									*/
/************************************************************************/

static void checkwriteq (Devex *devex)

{
  int done;
  Iopex *iopex;
  OZ_Process *oldprocess;
  uLong sts;

  while ((iopex = devex -> writeqh) != NULL) {

    /* Try to complete the request now */

    oldprocess = oz_knl_process_getcur ();
    oz_knl_process_setcur (oz_knl_ioop_getprocess (iopex -> ioop));
    if (devex -> record) done = writerecreq (devex, iopex);
    else done = writestrreq (devex, iopex);
    oz_knl_process_setcur (oldprocess);

    /* If it didn't complete, don't try to do any more, just leave it on top of queue */

    if (!done) break;

    /* It completed, remove from queue and post completion, then try to complete another request */

    devex -> writeqh = iopex -> next;
    if (devex -> writeqh == NULL) devex -> writeqt = &(devex -> writeqh);
    oz_knl_iodone (iopex -> ioop, OZ_SUCCESS, NULL, NULL, NULL);
  }
}

/************************************************************************/
/*									*/
/*  Write to a record-oriented pipe					*/
/*									*/
/*    Input:								*/
/*									*/
/*	devex = pipe to write to					*/
/*	iopex -> threadidleft = how many bytes of 'threadid' left to do	*/
/*	iopex -> threadid     = the thread that is writing data		*/
/*	iopex -> sizeleft     = how many bytes of 'size' left to do	*/
/*	iopex -> size         = total data buffer size being written	*/
/*	iopex -> buffleft     = how many bytes of 'w_buff' left to do	*/
/*	iopex -> w_buff       = points to data buffer			*/
/*									*/
/*    Output:								*/
/*									*/
/*	writerecreq = 0 : there is more to do				*/
/*	              1 : the write has completed			*/
/*	iopex -> threadidleft = updated					*/
/*	iopex -> sizeleft     = updated					*/
/*	iopex -> buffleft     = updated					*/
/*									*/
/*    Note:								*/
/*									*/
/*	To write an eof, set size=-1, and buffleft=0			*/
/*									*/
/************************************************************************/

static int writerecreq (Devex *devex, Iopex *iopex)

{
  iopex -> threadidleft = writerecbuf (devex, iopex -> threadidleft, ((uByte *)&(iopex -> threadid)) + sizeof iopex -> threadid - iopex -> threadidleft);
  iopex -> sizeleft     = writerecbuf (devex, iopex -> sizeleft,     ((uByte *)&(iopex -> size))     + sizeof iopex -> size     - iopex -> sizeleft);
  iopex -> buffleft     = writerecbuf (devex, iopex -> buffleft,     iopex -> w_buff                 + iopex -> size            - iopex -> buffleft);

  return ((iopex -> threadidleft | iopex -> sizeleft | iopex -> buffleft) == 0);
}

static uLong writerecbuf (Devex *devex, uLong size, const uByte *buff)

{
  uLong room;

  while (size > 0) {						/* repeat while there is stuff to do */
    room = writeroom (devex);					/* see how much room left in buffer */
    if (room == 0) break;					/* if none, just stop here */
    if (room > size) room = size;				/* don't use more than buffer has */
    memcpy (devex -> buffer, buff, room);			/* copy it */
    devex -> insert += room;					/* this is how much is in there now */
    size -= room;						/* this is how much we copied */
    buff += room;
    if (devex -> insert == OZ_IO_PIPE_BUFSIZ) devex -> insert = 0; /* maybe wrap pointer */
  }
  return (size);						/* return how much is left to be done later */
}

/************************************************************************/
/*									*/
/*  Write to a stream-oriented pipe					*/
/*									*/
/*    Input:								*/
/*									*/
/*	devex = pipe to write to					*/
/*	iopex -> size        = total data buffer size being written	*/
/*	iopex -> buffleft    = how many bytes of 'buff' left to do	*/
/*	iopex -> buff        = points to data buffer			*/
/*	iopex -> trmsize     = total data buffer size being written	*/
/*	iopex -> trmbuffleft = how many bytes of 'buff' left to do	*/
/*	iopex -> trmbuff     = points to data buffer			*/
/*									*/
/*    Output:								*/
/*									*/
/*	writestr = 0 : there is more to do				*/
/*	           1 : the write has completed				*/
/*	iopex -> buffleft    = updated					*/
/*	iopex -> trmbuffleft = updated					*/
/*									*/
/*    Note:								*/
/*									*/
/*	To write an eof, set size=-1					*/
/*									*/
/************************************************************************/

static int writestrreq (Devex *devex, Iopex *iopex)

{
  uLong room;

  if (iopex -> size == (uLong)(-1)) {
    room = writeroom (devex);			/* see how much room in output buffer left */
    if (room == 0) return (0);			/* if no room, we failed */
    devex -> buffer[devex->insert++] = 255;	/* there is room for at least one byte, store the eof flag */
    if (devex -> insert == OZ_IO_PIPE_BUFSIZ) devex -> insert = 0; /* maybe wrap insert offset */
    return (1);					/* successful */
  }

  iopex -> buffleft    = writestrbuf (devex, iopex -> buffleft,    iopex -> w_buff  + iopex -> size    - iopex -> buffleft);
  iopex -> trmbuffleft = writestrbuf (devex, iopex -> trmbuffleft, iopex -> trmbuff + iopex -> trmsize - iopex -> trmbuffleft);
  return ((iopex -> buffleft | iopex -> trmbuffleft) == 0);
}

static uLong writestrbuf (Devex *devex, uLong size, const uByte *buff)

{
  uLong room;

  while (size > 0) {					/* repeat while buffer left to process */
    room = writeroom (devex);				/* see how much room in output buffer left */
    if (room == 0) break;				/* stop if absolutely full */
    if (room == 1) {					/* see if just one byte left */
      if (devex -> insert == OZ_IO_PIPE_BUFSIZ - 1) {	/* one byte, see if at very end of buffer */
        devex -> buffer[devex->insert] = 0;		/* ... at very end, pad in a zero so we don't get stuck here */
        devex -> insert = 0;				/* ... then wrap pointer around */
        continue;					/* ... and try again */
      }
      break;						/* one byte but not at end, come back when there's more room */
    }
    -- room;						/* ok, subtract 1 for the length byte */
    if (room > size) room = size;			/* output no more than we have in buffer */
    if (room > 254)  room = 254;			/* output no more than 254 at a time (reserve 255 for the eof flag) */
    devex -> buffer[devex->insert++] = room;		/* store the bytecount in pipe buffer */
    memcpy (devex -> buffer + devex -> insert, buff, room); /* store the corresponding data in pipe buffer */
    devex -> insert += room;				/* increment pipe buffer insert offset */
    size -= room;					/* decrement remaining bytes to transfer */
    buff += room;					/* increment address of remaining bytes */
    if (devex -> insert == OZ_IO_PIPE_BUFSIZ) devex -> insert = 0; /* maybe wrap insert offset */
  }							/* repeat to see if there's more to transfer */

  return (size);
}

/************************************************************************/
/*									*/
/*  See how much room is left in write buffer				*/
/*									*/
/*    Input:								*/
/*									*/
/*	devex = pipe to write to					*/
/*									*/
/*    Output:								*/
/*									*/
/*	writeroom = number of bytes remaining in write buffer		*/
/*									*/
/************************************************************************/

static uLong writeroom (Devex *devex)

{
  Long room;

  room = devex -> remove - devex -> insert;			/* see how much room left in buffer (includes separator byte) */
  if (room == 0) {						/* if both pointers are equal, buffer is completely empty */
    devex -> remove = devex -> insert = 0;			/* ... so reset to beginning of buffer to make copy easier */
    return (OZ_IO_PIPE_BUFSIZ - 1);				/* ... we can't use last byte as it distinguishes between empty and full */
  }
  if ((room == 1) || (room + OZ_IO_PIPE_BUFSIZ == 1)) return (0); /* if insert is exactly 1 behind remove, it is completely full */
  if (room > 0) return (room - 1);				/* if remove is after insert, copy up to remove - 1 */
								/* otherwise, insert follows remove */
  room = OZ_IO_PIPE_BUFSIZ - devex -> insert;			/* ... insert to end of buffer */
  if (devex -> remove == 0) -- room;				/* ... make sure we leave a byte before remove */
  return (room);
}

/************************************************************************/
/*									*/
/*  Process any requests lingering in the read queue			*/
/*									*/
/************************************************************************/

static void checkreadq (Devex *devex)

{
  Iopex *iopex;
  OZ_Process *oldprocess;
  uLong sts;

  while ((iopex = devex -> readqh) != NULL) {

    /* Try to complete the request now */

    oldprocess = oz_knl_process_getcur ();
    oz_knl_process_setcur (oz_knl_ioop_getprocess (iopex -> ioop));
    if (devex -> record) sts = readrecreq (devex, iopex);
    else sts = readstrreq (devex, iopex);

    /* If it didn't complete, don't try to do any more, just leave it on top of queue */

    if (sts == OZ_PENDING) {
      oz_knl_process_setcur (oldprocess);
      break;
    }

    /* It completed, remove from queue and post completion, then try to complete another request */

    devex -> readqh = iopex -> next;
    if (devex -> readqh == NULL) devex -> readqt = &(devex -> readqh);
    if (iopex -> rlen != NULL) *(iopex -> rlen) = iopex -> done;
    oz_knl_iodone (iopex -> ioop, sts, NULL, NULL, NULL);
    oz_knl_process_setcur (oldprocess);
  }
}

/************************************************************************/
/*									*/
/*  Read from a record-oriented pipe					*/
/*									*/
/*    Input:								*/
/*									*/
/*	devex = pipe to read from					*/
/*	iopex -> threadidleft = how many bytes of 'threadid' left to do	*/
/*	iopex -> recsizeleft  = how many bytes of 'recsize' left to do	*/
/*	iopex -> r_buff       = points to data buffer			*/
/*	iopex -> done         = how many data bytes read so far		*/
/*									*/
/*    Output:								*/
/*									*/
/*	readrecreq = OZ_PENDING : there is more to do			*/
/*	                   else : request completion status		*/
/*	iopex -> done = updated						*/
/*	iopex -> threadidleft = updated					*/
/*	iopex -> recsizeleft  = updated					*/
/*	iopex -> threadid = filled in with who wrote it			*/
/*	iopex -> size     = size of record that was written		*/
/*	iopex -> done     = updated					*/
/*									*/
/************************************************************************/

static uLong readrecreq (Devex *devex, Iopex *iopex)

{
  uLong sizeleft, sizetodo;

  /* First, get the threadid that wrote the data, followed by the size of the record that thread wrote */

  iopex -> threadidleft = readrecbuf (devex, iopex -> threadidleft, ((uByte *)&(iopex -> threadid)) + sizeof iopex -> threadid - iopex -> threadidleft);
  iopex -> sizeleft     = readrecbuf (devex, iopex -> sizeleft,     ((uByte *)&(iopex -> size))     + sizeof iopex -> size     - iopex -> sizeleft);
  if ((iopex -> threadidleft != 0) || (iopex -> sizeleft != 0)) return (OZ_PENDING);

  /* If the size of the record written is -1, that is the 'end-of-file' flag */
  /* Otherwise, it is an actual data record                                  */

  if (iopex -> recsize == (uLong)(-1)) return (OZ_ENDOFFILE);

  /* Process as much data as is in the pipe buffer that we can */

  while (iopex -> done < iopex -> recsize) {				/* repeat until we have gone through all that writer wrote */
    sizetodo = iopex -> recsize - iopex -> done;			/* see how much of what writer wrote we have yet to do */
    if (iopex -> r_buff != NULL) {					/* see if we still have a live data buffer */
      if (iopex -> done >= iopex -> size) iopex -> r_buff = NULL;	/* if we are beyond end of data buffer, forget about data buffer and just skip over data */
      else if (sizetodo > iopex -> size - iopex -> done) sizetodo = iopex -> size - iopex -> done; /* otherwise, don't do more than buffer can hold */
    }
    sizeleft = readrecbuf (devex, sizetodo, iopex -> r_buff);		/* read into buffer (or skip if iopex -> buff is NULL) */
    if (iopex -> r_buff != NULL) iopex -> r_buff += sizetodo - sizeleft; /* increment buffer pointer */
    iopex -> done += sizetodo - sizeleft;				/* increment how much has been done so far */
    if (sizeleft != 0) return (OZ_PENDING);				/* stop if the pipe buffer is empty */
  }
  if (iopex -> done <= iopex -> size) return (OZ_SUCCESS);		/* we got it all, see if we skipped anything */
  iopex -> done = iopex -> size;					/* we skipped stuff, return full buffer size */
  return (OZ_BUFFEROVF);						/* ... and return that the buffer overflowed */
}

static uLong readrecbuf (Devex *devex, uLong size, uByte *buff)

{
  int room;

  while (size > 0) {						/* repeat while there is stuff to do */
    room = devex -> insert - devex -> remove;			/* see how much data left in buffer */
    if (room < 0) room = OZ_IO_PIPE_BUFSIZ - devex -> remove;	/* maybe there is wrap */
    if (room == 0) break;					/* if none, just stop here */
    if (room > size) room = size;				/* don't use more than buffer has */
    if (buff != NULL) {
      memcpy (buff, devex -> buffer + devex -> remove, room);	/* copy it */
      buff += room;
    }
    devex -> remove += room;					/* this is how much is in there now */
    size -= room;						/* this is how much we copied */
    if (devex -> remove == OZ_IO_PIPE_BUFSIZ) devex -> remove = 0; /* maybe wrap pointer */
  }
  return (size);						/* return how much is left to be done later */
}

/************************************************************************/
/*									*/
/*  Read from a stream-oriented pipe					*/
/*									*/
/*    Input:								*/
/*									*/
/*	devex = pipe to read from					*/
/*	iopex -> done    = number of data bytes read so far		*/
/*	iopex -> size    = total data buffer size being read		*/
/*	iopex -> r_buff  = points to data buffer			*/
/*	iopex -> trmsize = total terminator size			*/
/*	iopex -> trmbuff = points to terminator buffer			*/
/*									*/
/*    Output:								*/
/*									*/
/*	readstrreq = OZ_PENDING : there is more to do			*/
/*	                   else : request completion status		*/
/*	iopex -> done = updated						*/
/*									*/
/************************************************************************/

static uLong readstrreq (Devex *devex, Iopex *iopex)

{
  int trmofs, trmrem;
  uByte databyte, dlen, termbyte;
  uLong todo;

  /* Terminator-less request */

  if (iopex -> trmsize == 0) {
    while (devex -> remove != devex -> insert) {				/* repeat while there is data in pipe buffer */
      dlen = devex -> buffer[devex->remove++];					/* get length of data block in pipe buffer */
      if (devex -> remove == OZ_IO_PIPE_BUFSIZ) devex -> remove = 0;		/* maybe wrap pointer */
      if (dlen == 0) continue;							/* if zero, just ignore it */
      if (dlen == 255) {							/* 255 is an eof mark */
        if (iopex -> done == 0) return (OZ_ENDOFFILE);				/* if we hit it first off, return eof status */
        -- (devex -> remove);							/* we have data, put eof mark back in pipe buffer */
        return (OZ_SUCCESS);							/* successsful for how much we got anyway */
      }
      todo = iopex -> size - iopex -> done;
      if (dlen >= todo) {							/* see if we got enough to complete request */
        memcpy (iopex -> r_buff, devex -> buffer + devex -> remove, todo);	/* if so, copy as much as wanted */
        devex -> remove += todo;						/* increment past that data */
        dlen -= todo;								/* see how much is left over */
        if (dlen > 0) devex -> buffer[--(devex->remove)] = dlen;		/* if something left over, put its size back */
        if (devex -> remove == OZ_IO_PIPE_BUFSIZ) devex -> remove = 0;		/* maybe wrap pointer */
        iopex -> done += todo;							/* we got all they asked for */
        return (OZ_SUCCESS);
      }
      memcpy (iopex -> r_buff, devex -> buffer + devex -> remove, dlen);	/* copy all we have */
      devex -> remove += dlen;							/* increment pipe buffer offset */
      iopex -> done   += dlen;							/* increment how much we have done */
      iopex -> r_buff += dlen;							/* increment to where to put more */
      if (devex -> remove == OZ_IO_PIPE_BUFSIZ) devex -> remove = 0;		/* maybe wrap pipe buffer offset */
    }
    return (OZ_PENDING);							/* we need more to complete request */
  }

  /* Mono-byte terminator */

  termbyte = iopex -> trmbuff[0];						/* get the first terminator byte */
  if (iopex -> trmsize == 1) {
    while (devex -> remove != devex -> insert) {				/* repeat while there is data in pipe buffer */
      dlen = devex -> buffer[devex->remove++];					/* get length of data block in pipe buffer */
      if (devex -> remove == OZ_IO_PIPE_BUFSIZ) devex -> remove = 0;		/* maybe wrap pointer */
      if (dlen == 0) continue;							/* if zero, just ignore it */
      if (dlen == 255) {							/* 255 is an eof mark */
        if (iopex -> done == 0) return (OZ_ENDOFFILE);				/* if we hit it first off, return eof status */
        if (devex -> remove == 0) devex -> remove = OZ_IO_PIPE_BUFSIZ;		/* we have data, put eof mark back in pipe buffer */
        -- (devex -> remove);
        return (OZ_NOTERMINATOR);						/* no terminator for the data we did get, though */
      }
      while (dlen > 0) {
        -- dlen;								/* get a byte of data from pipe buffer */
        databyte = devex -> buffer[devex->remove++];
        if (databyte == termbyte) {						/* see if it is the terminator */
          if (dlen > 0) devex -> buffer[--(devex->remove)] = dlen;		/* if something left over, put its size back */
          if (devex -> remove == OZ_IO_PIPE_BUFSIZ) devex -> remove = 0;	/* maybe wrap pointer */
          return (OZ_SUCCESS);							/* successful completion with terminator */
        }
        if (iopex -> done == iopex -> size) {					/* not terminator, see if room for databyte */
          devex -> remove -= 2;							/* if not, put databyte back in pipe buffer */
          devex -> buffer[devex->remove] = dlen + 1;				/* ... and put the datalength back in pipe buffer */
          return (OZ_NOTERMINATOR);						/* request is complete, but no terminator seen */
        }
        iopex -> r_buff[iopex->done++] = databyte;				/* ok, store byte in read buffer */
      }
      if (devex -> remove == OZ_IO_PIPE_BUFSIZ) devex -> remove = 0;		/* maybe wrap pointer */
    }
    return (OZ_PENDING);							/* request not yet complete */
  }

  /* Multi-byte terminator */

  while (devex -> remove != devex -> insert) {					/* repeat while there is data in pipe buffer */
    dlen = devex -> buffer[devex->remove++];					/* get length of data block in pipe buffer */
    if (devex -> remove == OZ_IO_PIPE_BUFSIZ) devex -> remove = 0;		/* maybe wrap pointer */
    if (dlen == 0) continue;							/* if zero, just ignore it */
    if (dlen == 255) {								/* 255 is an eof mark */
      if (iopex -> done == 0) return (OZ_ENDOFFILE);				/* if we hit it first off, return eof status */
      -- (devex -> remove);							/* we have data, put eof mark back in pipe buffer */
      return (OZ_NOTERMINATOR);							/* no terminator for the data we did get, though */
    }
    while (dlen > 0) {
      databyte = devex -> buffer[devex->remove];				/* get a byte of data from pipe buffer */
      if (databyte == termbyte) {						/* see if it is the first terminator byte */
        devex -> buffer[--(devex->remove)] = dlen;				/* ok, put terminator and dlen back in buffer */
        trmrem = devex -> remove;						/* use this to remove term bytes from pipe buffer */
        trmofs = 0;								/* we haven't seen any terminator bytes yet */
        while (trmrem != devex -> insert) {					/* repeat while there is data in pipe buffer */
          dlen = devex -> buffer[trmrem++];					/* get length of data block in pipe buffer */
          if (trmrem == OZ_IO_PIPE_BUFSIZ) trmrem = 0;				/* maybe wrap pointer */
          if (dlen == 0) continue;						/* if zero, just ignore it */
          if (dlen == 255) goto multibytenoterm;				/* if 255, eof, so we didn't find terminator */
          while (dlen > 0) {
            databyte = devex -> buffer[trmrem];					/* get a byte of data from pipe buffer */
            if (databyte != iopex -> trmbuff[trmofs++]) goto multibytenoterm;	/* if isn't next term byte, don't have terminator */
            trmrem ++;								/* it matches, increment past it */
            if (trmofs == iopex -> trmsize) {					/* see if we have the whole terminator */
              devex -> remove = trmrem;						/* ok, wipe it from pipe buffer */
              if (dlen > 0) devex -> buffer[--(devex->remove)] = dlen;		/* maybe re-insert remaining data length */
              if (devex -> remove == OZ_IO_PIPE_BUFSIZ) devex -> remove = 0;	/* maybe wrap pointer */
              return (OZ_SUCCESS);						/* successful completion with terminator */
            }
          }
          if (trmrem == OZ_IO_PIPE_BUFSIZ) trmrem = 0;				/* maybe wrap offset */
        }
        return (OZ_PENDING);							/* not enuf data to finish terminator yet */
multibytenoterm:								/* terminator did not match pipe buffer contents */
        dlen = devex -> buffer[devex->remove++];				/* get the length of pipe buffer block */
        databyte = devex -> buffer[devex->remove];				/* get the first byte of pipe buffer block */
										/* use it as data even though it is same as terminator byte */
										/* ... because the rest of the terminator didn't match up */
      }
      if (iopex -> done == iopex -> size) {					/* not terminator, see if room for databyte */
        devex -> buffer[--(devex->remove)] = dlen;				/* if not, put the datalength back in pipe buffer */
        return (OZ_NOTERMINATOR);						/* request is complete, but no terminator seen */
      }        
      -- dlen;									/* ok, remove byte from pipe buffer */
      devex -> remove ++;
      iopex -> r_buff[iopex->done++] = databyte;				/* store byte in read buffer */
    }
  }
  return (OZ_PENDING);								/* request not yet complete */
}

/************************************************************************/
/*									*/
/*  Perform I/O request for null/zero device				*/
/*									*/
/************************************************************************/

static uLong nullzero_do (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode, OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap)

{
  uLong sts;

  switch (funcode) {

    /* These are always successful nop's */

    case OZ_IO_FS_OPEN:
    case OZ_IO_FS_CREATE:
    case OZ_IO_FS_WRITEREC:
    case OZ_IO_FS_CLOSE: {
      return (OZ_SUCCESS);
    }

    /* Read a record */

    case OZ_IO_FS_READREC: {
      OZ_IO_fs_readrec fs_readrec;

      movc4 (as, ap, sizeof fs_readrec, &fs_readrec);
      sts = oz_knl_ioop_lockw (ioop, fs_readrec.size, fs_readrec.buff, NULL, NULL, NULL);
      if ((sts == OZ_SUCCESS) && (fs_readrec.rlen != NULL)) sts = oz_knl_ioop_lockw (ioop, sizeof *fs_readrec.rlen, fs_readrec.rlen, NULL, NULL, NULL);
      if (sts == OZ_SUCCESS) {
        if (devunit == null_devunit) {
          if (fs_readrec.rlen != NULL) *fs_readrec.rlen = 0;
          sts = OZ_ENDOFFILE;
        } else {
          memset (fs_readrec.buff, 0, fs_readrec.size);
          if (fs_readrec.rlen != NULL) *fs_readrec.rlen = fs_readrec.size;
          if (fs_readrec.trmsize != 0) sts = OZ_NOTERMINATOR;
        }
      }
      return (sts);
    }

    /* Who knows what */

    default: return (OZ_BADIOFUNC);
  }
}
