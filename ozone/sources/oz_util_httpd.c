//+++2002-08-17
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
//---2002-08-17

/************************************************************************/
/*									*/
/*  HTTP daemon								*/
/*									*/
/*	httpd [<port>]							*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_crtl_malloc.h"
#include "oz_knl_status.h"
#include "oz_io_conpseudo.h"
#include "oz_io_console.h"
#include "oz_io_ip.h"
#include "oz_sys_event.h"
#include "oz_sys_handle.h"
#include "oz_sys_io.h"
#include "oz_sys_io_fs.h"
#include "oz_sys_thread.h"
#include "oz_sys_xprintf.h"
#include "oz_util_start.h"

#define MAX_LISTENERS 5
#define MAX_THREADS 50

typedef struct { OZ_Handle tcpiochan;			/* tcp connection I/O channel */
                 OZ_IO_ip_tcplisten ip_tcplisten;	/* listening struct */
                 uByte remipaddr[OZ_IO_IP_ADDRSIZE];	/* client ip address */
                 uByte remportno[OZ_IO_IP_PORTSIZE];	/* ... and port number */
                 OZ_IO_ip_tcpreceive ip_tcpreceive;	/* tcp receive parameter block */
                 OZ_IO_ip_tcptransmit ip_tcptransmit;	/* tcp transmit parameter block */
                 int refcount;				/* when it goes zero, free off conctx block */
               } Conctx;

static char *pn = "httpd";
static OZ_Handle h_listenevent, h_threadevent;

static void connected (void *conctxv, uLong status, OZ_Mchargs *mchargs);
static uLong incoming (void *conctxv);
static char *cvtipbin (uByte ipbin[OZ_IO_IP_ADDRSIZE], char ipstr[OZ_IO_IP_ADDRSIZE*4]);

uLong oz_util_main (int argc, char *argv[])

{
  uByte lclportno[OZ_IO_IP_PORTSIZE];
  char ipstr[OZ_IO_IP_ADDRSIZE*4], threadname[OZ_THREAD_NAMESIZE];
  Conctx *conctx;
  int i, usedup;
  Long eventval;
  uLong sts;

  if (argc > 0) pn = argv[0];

  if ((argc != 1) && (argc != 2)) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: usage: %s [<port>]\n", pn, pn);
    return (OZ_MISSINGPARAM);
  }

  sts = 80;
  if (argc > 1) {
    sts = oz_hw_atoi (argv[1], &usedup);
    if ((argv[1][usedup] != 0) || (sts > 65535)) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: bad port number %s\n", pn, argv[2]);
      return (OZ_BADPARAM);
    }
  }
  OZ_IP_H2NW (sts, lclportno);

  oz_sys_io_fs_printf (oz_util_h_error, "%s: listening on port %u\n", pn, sts);

  /* Create an event flag.  Whenever the flag is <= 0, we wait as there are enough listeners going. */
  /* Whenever it is > 0, we start that many listeners going.                                        */

  sts = oz_sys_event_create (OZ_PROCMODE_KNL, "max listeners", &h_listenevent);
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
  sts = oz_sys_event_set (OZ_PROCMODE_KNL, h_listenevent, MAX_LISTENERS, NULL);
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);

  sts = oz_sys_event_create (OZ_PROCMODE_KNL, "max threads", &h_threadevent);
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
  sts = oz_sys_event_set (OZ_PROCMODE_KNL, h_threadevent, MAX_THREADS - MAX_LISTENERS, NULL);
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);

  /* Wait for both the flags to be > 0 */

listenloop:
  do {
    oz_sys_event_wait (OZ_PROCMODE_KNL, h_listenevent, 0);		/* wait for it to be > 0 */
    oz_sys_event_inc (OZ_PROCMODE_KNL, h_listenevent, 0, &eventval);	/* read it to be sure */
  } while (eventval <= 0);						/* wait again if <= 0 */

  do {
    oz_sys_event_wait (OZ_PROCMODE_KNL, h_threadevent, 0);		/* wait for it to be > 0 */
    oz_sys_event_inc (OZ_PROCMODE_KNL, h_threadevent, 0, &eventval);	/* read it to be sure */
  } while (eventval <= 0);						/* wait again if <= 0 */

  /* Create a connection context structure */

  oz_sys_event_inc (OZ_PROCMODE_KNL, h_listenevent, -1, NULL);		/* decrement number of needed listeners */
  conctx = malloc (sizeof *conctx);					/* create context block */
  memset (conctx, 0, sizeof *conctx);					/* start out wil all zeroes */

  /* Assign an I/O channel to the ip device for the connection */

  sts = oz_sys_io_assign (OZ_PROCMODE_KNL, &(conctx -> tcpiochan), OZ_IO_IP_DEV, OZ_LOCKMODE_CW);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u assigning channel to " OZ_IO_IP_DEV "\n", pn, sts);
    return (sts);
  }

  /* Start listening for an incoming connection request.  Increment the event */
  /* flag when this I/O completes so we will start a replacement going.       */

  conctx -> ip_tcplisten.addrsize  = OZ_IO_IP_ADDRSIZE;
  conctx -> ip_tcplisten.portsize  = OZ_IO_IP_PORTSIZE;
  conctx -> ip_tcplisten.lclportno = lclportno;
  conctx -> ip_tcplisten.remipaddr = conctx -> remipaddr;
  conctx -> ip_tcplisten.remportno = conctx -> remportno;

  sts = oz_sys_io_start (OZ_PROCMODE_KNL, conctx -> tcpiochan, NULL, h_listenevent, connected, conctx, 
                         OZ_IO_IP_TCPLISTEN, sizeof conctx -> ip_tcplisten, &(conctx -> ip_tcplisten));
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u starting listening\n", pn, sts);
    return (sts);
  }

  /* Go get another */

  goto listenloop;
}

/* This ast routine gets called when an inbound connection is received */

static void connected (void *conctxv, uLong status, OZ_Mchargs *mchargs)

{
  char ipstr[OZ_IO_IP_ADDRSIZE*4], threadname[OZ_THREAD_NAMESIZE];
  Conctx *conctx;
  uLong sts;

  conctx = conctxv;

  /* Check listen status - maybe client disconnected in the middle of the connection */

  if (status != OZ_SUCCESS) oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u lisening for connection\n", pn, status);

  /* Fork a thread to process it */

  else {
    oz_sys_sprintf (sizeof threadname, threadname, "%s %s.%u", pn, cvtipbin (conctx -> remipaddr, ipstr), OZ_IP_N2HW (conctx -> remportno));
    sts = oz_sys_thread_create (OZ_PROCMODE_KNL, 0, 0, 0, h_threadevent, 0, incoming, conctx, OZ_ASTMODE_ENABLE, threadname, NULL);
    if (sts == OZ_SUCCESS) oz_sys_event_inc (OZ_PROCMODE_KNL, h_threadevent, -1, NULL);
    else {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u forking thread %s\n", pn, sts, threadname);
      free (conctx);
    }
  }
}

/************************************************************************/
/*									*/
/*  This routine is forked as a thread by main when there is an 	*/
/*  incoming connection request						*/
/*									*/
/*    Input:								*/
/*									*/
/*	conctx = pointer to connection context block			*/
/*									*/
/************************************************************************/

static uLong incoming (void *conctxv)

{
  Conctx *conctx;

  conctx = conctxv;

  ?? io channel is in 'conctx -> tcpiochan' ??

  /* Read request header */

  ??

  /* Process */

  /* Transmit response */

  /* All done */

  oz_sys_handle_release (OZ_PROCMODE_KNL, conctx -> tcpiochan);
  free (conctx);
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Convert ip binary to string						*/
/*									*/
/************************************************************************/

static char *cvtipbin (uByte ipbin[OZ_IO_IP_ADDRSIZE], char ipstr[OZ_IO_IP_ADDRSIZE*4])

{
  char *p;
  int i;

  p = ipstr;
  for (i = 0; i < OZ_IO_IP_ADDRSIZE; i ++) {
    oz_hw_itoa (ipbin[i], 4, p);
    p += strlen (p);
    if (i != OZ_IO_IP_ADDRSIZE - 1) *(p ++) = '.';
  }
  return (ipstr);
}
