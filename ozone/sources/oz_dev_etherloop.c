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
/*  Ethernet loopback device driver					*/
/*									*/
/************************************************************************/

#include "ozone.h"

#include "oz_knl_devio.h"
#include "oz_knl_event.h"
#include "oz_knl_hw.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_phymem.h"
#include "oz_knl_procmode.h"
#include "oz_knl_sdata.h"
#include "oz_knl_section.h"
#include "oz_knl_status.h"
#include "oz_io_ether.h"

#define ARPHWTYPE 1
#define ADDRSIZE 1
#define PROTOSIZE 2
#define DATASIZE 1500
#define BUFFSIZE (2*ADDRSIZE+PROTOSIZE+DATASIZE)

#define N2HW(__nw) (((__nw)[0] << 8) + (__nw)[1])
#define CEQENADDR(__ea1,__ea2) (memcmp (__ea1, __ea2, ADDRSIZE) == 0)

typedef struct Chnex Chnex;
typedef struct Devex Devex;
typedef struct Iopex Iopex;
typedef struct Rxbuf Rxbuf;

/* Format of ethernet buffers */

typedef struct { uByte dstaddr[ADDRSIZE];
                 uByte srcaddr[ADDRSIZE];
                 uByte proto[PROTOSIZE];
                 uByte data[DATASIZE];
               } Ether_buf;

/* Format of receive buffers */

struct Rxbuf { Rxbuf *next;		/* next in receive list */
               Long refcount;		/* ref count (number of iopex -> rxbuf's pointing to it) */
               uWord dlen;		/* length of data (not incl header) */
               Ether_buf buf;		/* ethernet receive data */
             };

/* Device extension structure */

struct Devex { OZ_Devunit *devunit;		/* devunit pointer */
               OZ_Smplock smplock;		/* pointer to smp lock */

               Chnex *chnexs;			/* all open channels on the device */

               uByte enaddr[ADDRSIZE]; /* hardware address */
             };

/* Channel extension structure */

struct Chnex { Chnex *next;			/* next in devex->chnexs list */
               int rcvmissed;			/* count of messages transmitted but there was no receive ready for them */
               uWord proto;			/* protocol number (or 0 for all) */
               uWord promis;			/* promiscuous mode (0 normal, 1 promiscuous) */

               /* Receive related data */

               Iopex *rxreqh;			/* list of pending receive requests */
               Iopex **rxreqt;			/* points to rxreqh if list empty */
						/* points to last one's iopex -> next if requests in queue */
						/* NULL if channel is closed */
             };

/* I/O extension structure */

struct Iopex { Iopex *next;				/* next in list of requests */
               OZ_Ioop *ioop;				/* I/O operation block pointer */
               OZ_Procmode procmode;			/* processor mode of request */
               Devex *devex;				/* pointer to device */

               /* Receive related data */

               OZ_IO_ether_receive receive;		/* receive request I/O parameters */
               Rxbuf *rxbuf;				/* pointer to buffer received */
             };

/* Function table */

static uLong etherloop_assign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode);
static int etherloop_deassign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv);
static uLong etherloop_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode, 
                              OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap);

static const OZ_Devfunc etherloop_functable = { sizeof (Devex), sizeof (Chnex), sizeof (Iopex), 0, 
                                                NULL, NULL, NULL, etherloop_assign, etherloop_deassign, NULL, etherloop_start, NULL };

/* Internal static data */

static uByte broadcast[ADDRSIZE];
static int initialized = 0;
static uLong headerlength;
static OZ_Devclass *devclass;
static OZ_Devdriver *devdriver;

/* Internal routines */

static int close_channel (Devex *devex, Chnex *chnexx, uLong dv);
static void buffereceived (Devex *devex, Rxbuf *rxbuf);
static void receive_iodone (void *iopexv, int finok, uLong *status_r);

/************************************************************************/
/*									*/
/*  Boot-time initialization routine					*/
/*									*/
/************************************************************************/

void oz_dev_etherloop_init ()

{
  Devex *devex;
  Ether_buf *buf;
  OZ_Devunit *devunit;

  if (initialized) return;

  oz_knl_printk ("oz_dev_etherloop_init\n");
  initialized = 1;

  memset (broadcast, -1, sizeof broadcast);		/* broadcast ethernet address */

  headerlength = buf -> data - buf -> dstaddr;		/* length of an ethernet header (sizeof dstaddr + sizeof srcaddr + sizeof proto = 14) */

  devclass  = oz_knl_devclass_create (OZ_IO_ETHER_CLASSNAME, OZ_IO_ETHER_BASE, OZ_IO_ETHER_MASK, "etherloop");
  devdriver = oz_knl_devdriver_create (devclass, "etherloop");

  devunit = oz_knl_devunit_create (devdriver, "etherloop", "ethernet loopback", &etherloop_functable, 0, oz_s_secattr_sysdev); /* create device driver database entries */
  devex   = oz_knl_devunit_ex (devunit);

  devex -> devunit = devunit;									/* save devunit pointer */
  memset (devex -> enaddr, 0, sizeof devex -> enaddr);						/* save the ethernet address */
  devex -> chnexs  = NULL;									/* no I/O channels assigned to device */
  oz_hw_smplock_init (sizeof devex -> smplock, &(devex -> smplock), OZ_SMPLOCK_LEVEL_DV);	/* device database lock */
}

/************************************************************************/
/*									*/
/*  A new channel was assigned to the device				*/
/*  This routine initializes the chnex area				*/
/*									*/
/************************************************************************/

static uLong etherloop_assign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode)

{
  memset (chnexv, 0, sizeof (Chnex));
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  A channel is about to be deassigned from a device			*/
/*  Here we do a close if it is open					*/
/*									*/
/************************************************************************/

static int etherloop_deassign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv)

{
  Chnex *chnex;
  Devex *devex;
  uLong dv;

  chnex = chnexv;
  devex = devexv;

  if (chnex -> rxreqt != NULL) {
    dv = oz_hw_smplock_wait (&(devex -> smplock));
    close_channel (devex, chnex, dv);
    oz_hw_smplock_clr (&(devex -> smplock), dv);
    if (chnex -> rxreqt != NULL) oz_crash ("oz_dev_etherloop deassign: channel still open after close");
  }

  return (0);
}

/************************************************************************/
/*									*/
/*  Start performing an ethernet I/O function				*/
/*									*/
/************************************************************************/

static uLong etherloop_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode, 
                              OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap)

{
  Chnex *chnex;
  Devex *devex;
  Iopex *iopex, **liopex;
  int i;
  uLong dv, sts;
  Rxbuf *rxbuf;

  chnex = chnexv;
  devex = devexv;
  iopex = iopexv;

  switch (funcode) {

    /* Open - associates a protocol number with a channel and starts reception */

    case OZ_IO_ETHER_OPEN: {
      OZ_IO_ether_open ether_open;

      movc4 (as, ap, sizeof ether_open, &ether_open);

      /* Make sure not already open */

      dv = oz_hw_smplock_wait (&(devex -> smplock));
      if (chnex -> rxreqt != NULL) {
        oz_hw_smplock_clr (&(devex -> smplock), dv);
        return (OZ_FILEALREADYOPEN);
      }

      /* Put channel on list of open channels */

      chnex -> proto  = N2HW (ether_open.proto);
      chnex -> promis = ether_open.promis;
      chnex -> rxreqt = &(chnex -> rxreqh);
      chnex -> next   = devex -> chnexs;
      devex -> chnexs = chnex;
      oz_hw_smplock_clr (&(devex -> smplock), dv);
      return (OZ_SUCCESS);
    }

    /* Disassociates a protocol with a channel and stops reception */

    case OZ_IO_ETHER_CLOSE: {
      dv = oz_hw_smplock_wait (&(devex -> smplock));
      i = close_channel (devex, chnex, dv);
      oz_hw_smplock_clr (&(devex -> smplock), dv);

      return (i ? OZ_SUCCESS : OZ_FILENOTOPEN);
    }

    /* Receive a message */

    case OZ_IO_ETHER_RECEIVE: {

      /* Get parameter block into iopex for future reference */

      movc4 (as, ap, sizeof iopex -> receive, &(iopex -> receive));

      /* If any of the rcv... parameters are filled in, it must be called from kernel mode */

      if ((iopex -> receive.rcvfre != NULL) || (iopex -> receive.rcvdrv_r != NULL) || (iopex -> receive.rcveth_r != NULL)) {
        if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);
      }

      /* Set up the request parameters and queue request so the interrupt routine can fill the buffer with an incoming message */

      iopex -> ioop     = ioop;
      iopex -> next     = NULL;
      iopex -> procmode = procmode;
      iopex -> devex    = devex;

      rxbuf = iopex -> receive.rcvfre;					/* maybe requestor has a buffer to free off */
      if ((rxbuf != NULL) && (oz_hw_atomic_inc_long (&(rxbuf -> refcount), -1) == 0)) OZ_KNL_NPPFREE (rxbuf);

      dv = oz_hw_smplock_wait (&(devex -> smplock));			/* lock database */
      liopex = chnex -> rxreqt;
      sts = OZ_FILENOTOPEN;						/* make sure channel is still open */
      if (liopex != NULL) {
        *liopex = iopex;						/* put reqeuest on end of queue - transmit routine can now see it */
        chnex -> rxreqt = &(iopex -> next);
        sts = OZ_STARTED;
      }
      oz_hw_smplock_clr (&(devex -> smplock), dv);
      return (sts);
    }

    /* Free a receive buffer */

    case OZ_IO_ETHER_RECEIVEFREE: {
      OZ_IO_ether_receivefree ether_receivefree;

      if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);		/* can only be called from kernel mode */
      movc4 (as, ap, sizeof ether_receivefree, &ether_receivefree);	/* get the parameters */
      rxbuf = ether_receivefree.rcvfre;					/* point to the buffer */
      if (oz_hw_atomic_inc_long (&(rxbuf -> refcount), -1) == 0) OZ_KNL_NPPFREE (rxbuf); /* free it if no one else using it */
      return (OZ_SUCCESS);
    }

    /* Allocate a send buffer */

    case OZ_IO_ETHER_TRANSMITALLOC: {
      OZ_IO_ether_transmitalloc ether_transmitalloc;

      if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);		/* can only be called from kernel mode */
      movc4 (as, ap, sizeof ether_transmitalloc, &ether_transmitalloc);	/* get the parameters */
      rxbuf = OZ_KNL_NPPMALLOQ (sizeof *rxbuf);				/* allocate a receive buffer */
      if (rxbuf == NULL) return (OZ_EXQUOTANPP);
      if (ether_transmitalloc.xmtsiz_r != NULL) *(ether_transmitalloc.xmtsiz_r) = OZ_IO_ETHER_MAXDATA;	/* this is size of data it can handle */
      if (ether_transmitalloc.xmtdrv_r != NULL) *(ether_transmitalloc.xmtdrv_r) = rxbuf;		/* this is the pointer we want returned in ether_transmit.xmtdrv */
      if (ether_transmitalloc.xmteth_r != NULL) *(ether_transmitalloc.xmteth_r) = (uByte *)&(rxbuf -> buf); /* this is where they put the ethernet packet to be transmitted */
      return (OZ_SUCCESS);
    }

    /* Transmit a message */

    case OZ_IO_ETHER_TRANSMIT: {
      OZ_IO_ether_transmit ether_transmit;

      /* Get parameter block into iopex for future reference */

      movc4 (as, ap, sizeof ether_transmit, &ether_transmit);

      /* Any xmt... parameters require caller to be in kernel mode */

      if ((ether_transmit.xmtdrv != NULL) || (ether_transmit.xmtdrv_r != NULL) || (ether_transmit.xmtsiz_r != NULL) || (ether_transmit.xmteth_r != NULL)) {
        if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);
        if (ether_transmit.xmtsiz_r != NULL) *(ether_transmit.xmtsiz_r) = 0;
        if (ether_transmit.xmtdrv_r != NULL) *(ether_transmit.xmtdrv_r) = NULL;
        if (ether_transmit.xmteth_r != NULL) *(ether_transmit.xmteth_r) = NULL;
      }

      /* See if they gave us a system buffer */

      if (ether_transmit.xmtdrv != NULL) rxbuf = ether_transmit.xmtdrv;

      /* If not, allocate one */

      else if (ether_transmit.buff == NULL) return (OZ_MISSINGPARAM);
      else {
        rxbuf = OZ_KNL_NPPMALLOQ (sizeof *rxbuf);
        if (rxbuf == NULL) return (OZ_EXQUOTANPP);
      }

      /* Anyway, copy in any data that they supplied */

      if (ether_transmit.buff != NULL) {
        oz_knl_printk ("oz_dev_ip*: #1 %u < %u\n", ether_transmit.size, rxbuf -> buf.data - (uByte *)&(rxbuf -> buf));
        oz_knl_printk ("oz_dev_ip*: #2 %u > %u\n", ether_transmit.size, sizeof rxbuf -> buf);
        if (ether_transmit.size < rxbuf -> buf.data - (uByte *)&(rxbuf -> buf)) sts = OZ_BADBUFFERSIZE;							/* have to give at least the ethernet header stuff */
        else if (ether_transmit.size > sizeof rxbuf -> buf) sts = OZ_BADBUFFERSIZE;									/* can't give us more than buffer can hold */
        else sts = oz_knl_section_uget (procmode, ether_transmit.size, ether_transmit.buff, &(rxbuf -> buf));						/* copy it in */
        oz_knl_printk ("oz_dev_ip*: #3 %u < %u\n", ether_transmit.size, rxbuf -> dlen + rxbuf -> buf.data - (uByte *)&(rxbuf -> buf));
        if ((sts == OZ_SUCCESS) && (ether_transmit.size < rxbuf -> dlen + rxbuf -> buf.data - (uByte *)&(rxbuf -> buf))) sts = OZ_BADBUFFERSIZE;	/* must give enough to cover the dlen */
        if (sts != OZ_SUCCESS) {
          OZ_KNL_NPPFREE (rxbuf);
          return (sts);
        }
      }

      /* Make sure dlen (length of data not including header) not too int */

      oz_knl_printk ("oz_dev_ip*: #4 %u > %u\n", rxbuf -> dlen, DATASIZE);
      if (rxbuf -> dlen > DATASIZE) {			/* can't be longer than hardware will allow */
        OZ_KNL_NPPFREE (rxbuf);				/* free off internal buffer */
        return (OZ_BADBUFFERSIZE);			/* return error status */
      }

      /* Process it - hopefully there is a receive request to process it - if not, data is lost */

      buffereceived (devex, rxbuf);

      /* If caller requested, allocate a transmit buffer to replace one used */

      if (ether_transmit.xmtdrv_r != NULL) {
        rxbuf = OZ_KNL_NPPMALLOQ (sizeof *rxbuf);
        if (rxbuf == NULL) return (OZ_EXQUOTANPP);
        if (ether_transmit.xmtsiz_r != NULL) *(ether_transmit.xmtsiz_r) = OZ_IO_ETHER_MAXDATA;
        if (ether_transmit.xmtdrv_r != NULL) *(ether_transmit.xmtdrv_r) = rxbuf;
        if (ether_transmit.xmteth_r != NULL) *(ether_transmit.xmteth_r) = (uByte *)&(rxbuf -> buf);
      }
      return (OZ_SUCCESS);
    }

    /* Get info - part 1 */

    case OZ_IO_ETHER_GETINFO1: {
      OZ_IO_ether_getinfo1 ether_getinfo1;

      movc4 (as, ap, sizeof ether_getinfo1, &ether_getinfo1);
      if (ether_getinfo1.enaddrbuff != NULL) {
        if (ether_getinfo1.enaddrsize > ADDRSIZE) ether_getinfo1.enaddrsize = ADDRSIZE;
        sts = oz_knl_section_uput (procmode, ether_getinfo1.enaddrsize, devex -> enaddr, ether_getinfo1.enaddrbuff);
        if (sts != OZ_SUCCESS) return (sts);
      }
      ether_getinfo1.datasize   = DATASIZE;				// max length of data portion of message
      ether_getinfo1.buffsize   = BUFFSIZE;				// max length of whole message (header, data, crc)
      ether_getinfo1.dstaddrof  = 0;					// offset of dest address in packet
      ether_getinfo1.srcaddrof  = 0 + ADDRSIZE;				// offset of source address in packet
      ether_getinfo1.protooffs  = 0 + 2 * ADDRSIZE;			// offset of protocol in packet
      ether_getinfo1.dataoffset = 0 + 2 * ADDRSIZE + PROTOSIZE;		// offset of data in packet
      ether_getinfo1.arphwtype  = ARPHWTYPE;				// ARP hardware type
      ether_getinfo1.addrsize   = ADDRSIZE;				// size of each address field
      ether_getinfo1.protosize  = PROTOSIZE;				// size of protocol field
      if (as > sizeof ether_getinfo1) as = sizeof ether_getinfo1;
      sts = oz_knl_section_uput (procmode, as, &ether_getinfo1, ap);
      return (sts);
    }
  }

  return (OZ_BADIOFUNC);
}

/************************************************************************/
/*									*/
/*  Close an open channel						*/
/*									*/
/*    Input:								*/
/*									*/
/*	chnexx = channel to be closed					*/
/*	dv = previous smplock level (should be SOFTINT)			*/
/*	smplock = dv							*/
/*									*/
/*    Output:								*/
/*									*/
/*	close_channel = 0 : channel wasn't open				*/
/*	                1 : channel was open, now closed		*/
/*									*/
/*    Note:								*/
/*									*/
/*	smplock is released and re-acquired				*/
/*									*/
/************************************************************************/

static int close_channel (Devex *devex, Chnex *chnexx, uLong dv)

{
  Chnex *chnex, **lchnex;
  Iopex *iopex;

  /* Unlink from list of open channels */

  for (lchnex = &(devex -> chnexs); (chnex = *lchnex) != chnexx; lchnex = &(chnex -> next)) {
    if (chnex == NULL) return (0);
  }
  *lchnex = chnex -> next;

  /* Abort all pending receive requests and don't let any more queue */

  chnex -> rxreqt = NULL;				/* mark it closed - abort any receive requests that try to queue */
  while ((iopex = chnex -> rxreqh) != NULL) {		/* abort any receive requests we may have */
    chnex -> rxreqh = iopex -> next;
    oz_hw_smplock_clr (&(devex -> smplock), dv);
    oz_knl_iodone (iopex -> ioop, OZ_ABORTED, NULL, NULL, NULL);
    dv = oz_hw_smplock_wait (&(devex -> smplock));
  }

  return (1);
}

/************************************************************************/
/*									*/
/*  A buffer was transmitted so complete any corresponding receive 	*/
/*  requests								*/
/*									*/
/*    Input:								*/
/*									*/
/*	devex = device being processed					*/
/*	rxbuf = buffer just transmitted					*/
/*	smplock = softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	receive I/O posted						*/
/*	both buffers freed						*/
/*									*/
/************************************************************************/

static void buffereceived (Devex *devex, Rxbuf *rxbuf)

{
  Chnex *chnex;
  Iopex *iopex;
  uLong dv;
  uWord proto;

  rxbuf -> refcount = 1;							/* initialize refcount */
  proto = N2HW (rxbuf -> buf.proto);						/* get received message's protocol */

  dv = oz_hw_smplock_wait (&(devex -> smplock));
  for (chnex = devex -> chnexs; chnex != NULL; chnex = chnex -> next) {		/* find a suitable I/O channel */
    if ((chnex -> proto != 0) && (chnex -> proto != proto)) continue;		/* ... with matching protocol */
    if (!(chnex -> promis) && !CEQENADDR (rxbuf -> buf.dstaddr, devex -> enaddr) && !CEQENADDR (rxbuf -> buf.dstaddr, broadcast)) continue; /* matching enaddr */
    iopex = chnex -> rxreqh;							/* see if any receive I/O requests pending on it */
    if (iopex == NULL) chnex -> rcvmissed ++;					/* if not, it missed one */
    else {
      chnex -> rxreqh = iopex -> next;
      if (chnex -> rxreqh == NULL) chnex -> rxreqt = &(chnex -> rxreqh);
      OZ_HW_ATOMIC_INCBY1_LONG (rxbuf -> refcount);				/* increment received buffer's reference count */
      iopex -> rxbuf = rxbuf;							/* assign the received buffer to the request */
      iopex -> next  = NULL;
      oz_hw_smplock_clr (&(devex -> smplock), dv);
      oz_knl_iodone (iopex -> ioop, OZ_SUCCESS, NULL, receive_iodone, iopex);	/* post the receive request as complete */
      dv = oz_hw_smplock_wait (&(devex -> smplock));
    }
  }
  oz_hw_smplock_clr (&(devex -> smplock), dv);
  if (oz_hw_atomic_inc_long (&(rxbuf -> refcount), -1) == 0) OZ_KNL_NPPFREE (rxbuf); /* toss if no one wants it */
}

/************************************************************************/
/*									*/
/*  Back in requestor's thread - copy out data and/or pointers		*/
/*									*/
/*    Input:								*/
/*									*/
/*	iopexv = receive request's iopex				*/
/*	finok  = 0 : requesting thread has exited			*/
/*	         1 : made it back in requestor's thread			*/
/*									*/
/************************************************************************/

static void receive_iodone (void *iopexv, int finok, uLong *status_r)

{
  Devex *devex;
  Iopex *iopex;
  uLong size, sts;
  Rxbuf *rxbuf;

  iopex = iopexv;
  devex = iopex -> devex;
  rxbuf = iopex -> rxbuf;

  /* If requested, copy data to user's buffer */

  if ((iopex -> receive.buff != NULL) && (finok || OZ_HW_ISSYSADDR (iopex -> receive.buff))) {
    size = rxbuf -> dlen + headerlength;								/* size of everything we got, including header */
    if (size > iopex -> receive.size) size = iopex -> receive.size;					/* chop to user's buffer size */
    sts = oz_knl_section_uput (iopex -> procmode, size, &(rxbuf -> buf), iopex -> receive.buff);	/* copy data out to user's buffer */
    if (sts != OZ_SUCCESS) oz_knl_printk ("oz_dev_etherloop: copy to receive buf sts %u\n", sts);
  }

  /* If requested, return length of data received */

  if ((iopex -> receive.dlen != NULL) && (finok || OZ_HW_ISSYSADDR (iopex -> receive.dlen))) {
    sts = oz_knl_section_uput (iopex -> procmode, sizeof *(iopex -> receive.dlen), &(rxbuf -> dlen), iopex -> receive.dlen);
    if (sts != OZ_SUCCESS) {
      oz_knl_printk ("oz_dev_etherloop: return rlen sts %u\n", sts);
      if (*status_r == OZ_SUCCESS) *status_r = sts;
    }
  }

  /* If requested, return pointer to system buffer                                       */
  /* Note that this can only be done from kernel mode so we don't bother validating args */

  if ((iopex -> receive.rcvdrv_r != NULL) && (finok || OZ_HW_ISSYSADDR (iopex -> receive.rcvdrv_r))) {
    *(iopex -> receive.rcvdrv_r) = rxbuf;
    *(iopex -> receive.rcveth_r) = (uByte *)&(rxbuf -> buf);
  }

  /* If we didn't return the pointer, free off receive buffer */

  else if (oz_hw_atomic_inc_long (&(rxbuf -> refcount), -1) == 0) OZ_KNL_NPPFREE (rxbuf);
}
