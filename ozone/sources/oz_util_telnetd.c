//+++2003-03-01
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
//---2003-03-01

/************************************************************************/
/*									*/
/*  Telnet daemon							*/
/*									*/
/*	telnetd [<port>]						*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_crtl_malloc.h"
#include "oz_io_conpseudo.h"
#include "oz_io_console.h"
#include "oz_io_ip.h"
#include "oz_io_timer.h"
#include "oz_knl_status.h"
#include "oz_sys_dateconv.h"
#include "oz_sys_event.h"
#include "oz_sys_handle.h"
#include "oz_sys_io.h"
#include "oz_sys_io_fs.h"
#include "oz_sys_thread.h"
#include "oz_sys_xprintf.h"
#include "oz_util_start.h"

#define WINDOWSIZE 1460*3

typedef struct Conctx Conctx;

struct Conctx { OZ_Handle tcpiochan;					/* tcp connection I/O channel */
                OZ_Handle ptiochan;					/* pseudo-terminal I/O channel */
                OZ_IO_ip_tcpreceive ip_tcpreceive;			/* tcp receive parameter block */
                OZ_IO_ip_tcptransmit ip_tcptransmit;			/* tcp transmit parameter block */
                OZ_IO_conpseudo_getevent conpseudo_getevent;		/* gets console event from ptdriver, puts code in 'ptevent' */
                OZ_IO_conpseudo_getscrdata conpseudo_getscrdata;	/* gets screen data from pt driver, puts length in 'screenrln' */
									/* if .size is zero, no get is in progress, else get is in prog */
                OZ_IO_conpseudo_putkbddata conpseudo_putkbddata;	/* puts keyboard data to pt driver */
                OZ_Conpseudo_event ptevent;				/* pseudo-terminal event code */
                int kbd_readinprog;					/* set when a pseudo-terminal keyboard read is in progress */
                volatile int refcount;					/* when it goes zero, terminate processing */
                uWord naws_width, naws_height;				/* negotiate-about-window-size width and height (RFC 1073) */
                OZ_Handle termevent;					/* termination event flag */
                uLong termsts;						/* termination status */
                uLong keybdrlen;					/* length of data in keybdbuff */
                uLong keybdoffs;					/* offset in keybdbuff for next data */
                uLong screenxip;					/* how many bytes are being transmitted starting at screenrem */
                uLong screenrem;					/* offset of first byte in screenbuf being transmitted */
                uLong screenrln;					/* length of screen data read into screenbuf at screenins */
                uLong screenins;					/* offset in screenbuf to insert data */
                int screenflushbusy;					/* set when the screenflushtime timer is queued */
                OZ_IO_timer_waituntil screenflushtime;			/* when to flush the screen */
                char keybdbuff[WINDOWSIZE];				/* data received from network to go to pt keyboard */
                char screenbuf[WINDOWSIZE];				/* data from pt screen to be sent over network */
                char ptdevname[OZ_DEVUNIT_NAMESIZE];			/* pseudo-terminal device name (telnetd.<ipaddress>.<portno>) */
              };

static char *pn = "telnetd";

static char *cvtipbin (uByte ipbin[OZ_IO_IP_ADDRSIZE], char ipstr[OZ_IO_IP_ADDRSIZE*4]);
static OZ_Handle timeriochan;
static uLong incoming (void *conctxv);
static void gotptevent (void *conctxv, uLong status, OZ_Mchargs *mchargs);
static void getscreendata (Conctx *conctx);
static void gotscreendata (void *conctxv, uLong status, OZ_Mchargs *mchargs);
static void flushscreendata (void *conctxv, uLong status, OZ_Mchargs *mchargs);
static void sentscreendata (void *xipv, uLong status, OZ_Mchargs *mchargs);
static void gotkeybdata (void *conctxv, uLong status, OZ_Mchargs *mchargs);
static void terminate (Conctx *conctx, int inc, uLong status);

uLong oz_util_main (int argc, char *argv[])

{
  uByte lclportno[OZ_IO_IP_PORTSIZE];
  uByte remipaddr[OZ_IO_IP_ADDRSIZE];
  uByte remportno[OZ_IO_IP_PORTSIZE];
  char ipstr[OZ_IO_IP_ADDRSIZE*4];
  Conctx *conctx;
  int i, usedup;
  uLong sts;
  OZ_IO_ip_tcplisten ip_tcplisten;

  if (argc > 0) pn = argv[0];

  if ((argc != 1) && (argc != 2)) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: usage: %s [<port>]\n", pn);
    return (OZ_MISSINGPARAM);
  }

  sts = 23;
  if (argc > 1) {
    sts = oz_hw_atoi (argv[1], &usedup);
    if ((argv[1][usedup] != 0) || (sts > 65535)) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: bad port number %s\n", pn, argv[2]);
      return (OZ_BADPARAM);
    }
  }
  OZ_IP_H2NW (sts, lclportno);

  oz_sys_io_fs_printf (oz_util_h_error, "%s: listening on port %u\n", pn, sts);

  /* Assign an I/O channel to the timer */

  sts = oz_sys_io_assign (OZ_PROCMODE_KNL, &timeriochan, OZ_IO_TIMER_DEV, OZ_LOCKMODE_NL);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u assigning channel to " OZ_IO_TIMER_DEV "\n", pn, sts);
    return (sts);
  }

  /* Set up listening parameter block */

  memset (&ip_tcplisten, 0, sizeof ip_tcplisten);
  ip_tcplisten.addrsize  = OZ_IO_IP_ADDRSIZE;
  ip_tcplisten.portsize  = OZ_IO_IP_PORTSIZE;
  ip_tcplisten.lclportno = lclportno;

  /* Create a connection context structure */

listenloop:
  conctx = malloc (sizeof *conctx);
  memset (conctx, 0, sizeof *conctx);

  /* Assign an I/O channel to the ip device for the connection */

  sts = oz_sys_io_assign (OZ_PROCMODE_KNL, &(conctx -> tcpiochan), OZ_IO_IP_DEV, OZ_LOCKMODE_CW);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u assigning channel to " OZ_IO_IP_DEV "\n", pn, sts);
    return (sts);
  }

  /* Listen for an incoming connection request */

  ip_tcplisten.remipaddr  = remipaddr;
  ip_tcplisten.remportno  = remportno;
  ip_tcplisten.windowsize = sizeof conctx -> keybdbuff;
  ip_tcplisten.windowbuff = conctx -> keybdbuff;
  memset (remipaddr, 0, sizeof remipaddr);
  memset (remportno, 0, sizeof remportno);

  sts = oz_sys_io (OZ_PROCMODE_KNL, conctx -> tcpiochan, 0, OZ_IO_IP_TCPLISTEN, sizeof ip_tcplisten, &ip_tcplisten);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u receiving connection request\n", pn, sts);
    return (sts);
  }

  /* Make pseudo-terminal device name 'telnetd.<ipaddress>.<portno>' */

  oz_sys_sprintf (sizeof conctx -> ptdevname, conctx -> ptdevname, "telnetd.%s.%u", cvtipbin (remipaddr, ipstr), OZ_IP_N2HW (remportno));
  oz_sys_io_fs_printf (oz_util_h_error, "%s: connection received for %s\n", pn, conctx -> ptdevname);

  /* Fork a thread to process it */

  sts = oz_sys_thread_create (OZ_PROCMODE_KNL, 0, 0, 0, 0, 0, incoming, conctx, OZ_ASTMODE_ENABLE, conctx -> ptdevname, NULL);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u forking thread for %s\n", pn, sts, conctx -> ptdevname);
    free (conctx);
  }

  /* Go get another */

  goto listenloop;
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
  Long refc;
  OZ_IO_conpseudo_setup conpseudo_setup;
  OZ_IO_ip_tcptransmit ip_tcptransmit;
  uLong sts;

  static const uByte send_options[] = { 
	255, 251,  3, 		/* IAC, WILL, SUPPRESS_GO_AHEAD */
	255, 251,  1, 		/* IAC, WILL, ECHO */
	255, 251,  0, 		/* IAC, WILL, BINARY */
	255, 253,  0, 		/* IAC, DO, BINARY */
	255, 253, 31		/* IAC, DO, NAWS (negotiate about window size) */
  };

  conctx = conctxv;

  /* Create an pseudo-terminal */

  sts = oz_sys_io_assign (OZ_PROCMODE_KNL, &(conctx -> ptiochan), OZ_IO_CONPSEUDO_DEV, OZ_LOCKMODE_NL);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u assigning channel to " OZ_IO_CONPSEUDO_DEV " for %s\n", pn, sts, conctx -> ptdevname);
    conctx -> ptiochan = 0;
    terminate (conctx, 0, sts);
    goto rtn;
  }

  memset (&conpseudo_setup, 0, sizeof conpseudo_setup);
  conpseudo_setup.portname  = conctx -> ptdevname;
  conpseudo_setup.classname = OZ_IO_CONSOLE_SETUPDEV;

  sts = oz_sys_io (OZ_PROCMODE_KNL, conctx -> ptiochan, 0, OZ_IO_CONPSEUDO_SETUP, sizeof conpseudo_setup, &conpseudo_setup);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u setting up console device %s\n", pn, sts, conctx -> ptdevname);
    terminate (conctx, 0, sts);
    goto rtn;
  }

  /* Transmit our requested option list */

  conctx -> ip_tcptransmit.rawsize = sizeof send_options;
  conctx -> ip_tcptransmit.rawbuff = send_options;
  sts = oz_sys_io (OZ_PROCMODE_KNL, conctx -> tcpiochan, 0, OZ_IO_IP_TCPTRANSMIT, sizeof conctx -> ip_tcptransmit, &(conctx -> ip_tcptransmit));
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u transmitting telnet options %s\n", pn, sts, conctx -> ptdevname);
    terminate (conctx, 0, sts);
    goto rtn;
  }

  /* Init refcount to 3 for the 3 ast chains we get going */
  /* - reading network for keyboard data                  */
  /* - reading pseduo terminal for events                 */
  /* - reading pt for screen data                         */

  conctx -> refcount = 3;

  /* Set termination status = not terminated and create termination event flag */

  conctx -> termsts = OZ_PENDING;
  sts = oz_sys_event_create (OZ_PROCMODE_KNL, "idle loop", &(conctx -> termevent));
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);

  /* Start getting pseudo-terminal event */

  oz_sys_thread_setast (OZ_ASTMODE_INHIBIT);

  conctx -> conpseudo_getevent.event = &(conctx -> ptevent);

  sts = oz_sys_io_start (OZ_PROCMODE_KNL, conctx -> ptiochan, NULL, 0, gotptevent, conctx, 
                         OZ_IO_CONPSEUDO_GETEVENT, sizeof conctx -> conpseudo_getevent, &(conctx -> conpseudo_getevent));
  if (sts != OZ_STARTED) gotptevent (conctx, sts, NULL);

  /* Start getting some screen data to be sent over network */

  conctx -> conpseudo_getscrdata.rlen = &(conctx -> screenrln);
  getscreendata (conctx);

  /* Send a <CR> as keyboard data to pseudo-terminal to start logon prompt */

  conctx -> conpseudo_putkbddata.size = 1;
  conctx -> conpseudo_putkbddata.buff = "\r";
  sts = oz_sys_io (OZ_PROCMODE_KNL, conctx -> ptiochan, 0, OZ_IO_CONPSEUDO_PUTKBDDATA, sizeof conctx -> conpseudo_putkbddata, &(conctx -> conpseudo_putkbddata));
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u sending keyboard data to %s\n", pn, sts, conctx -> ptdevname);
    terminate (conctx, -1, sts);
  }

  /* Start receiving data from the network link to be used as keyboard data */

  else {
    conctx -> ip_tcpreceive.rawsize = sizeof conctx -> keybdbuff;
    conctx -> ip_tcpreceive.rawrlen = &(conctx -> keybdrlen);
    sts = oz_sys_io_start (OZ_PROCMODE_KNL, conctx -> tcpiochan, NULL, 0, gotkeybdata, conctx, 
                           OZ_IO_IP_TCPRECEIVE, sizeof conctx -> ip_tcpreceive, &(conctx -> ip_tcpreceive));
    if (sts != OZ_STARTED) gotkeybdata (conctx, sts, NULL);
  }

  oz_sys_thread_setast (OZ_ASTMODE_ENABLE);

  /* Wait for termination */

  while (((sts = conctx -> termsts) == OZ_PENDING) || (conctx -> refcount > 0)) {
    oz_sys_event_wait (OZ_PROCMODE_KNL, conctx -> termevent, 1);
    oz_sys_event_set (OZ_PROCMODE_KNL, conctx -> termevent, 0, NULL);
  }

  oz_sys_handle_release (OZ_PROCMODE_KNL, conctx -> termevent);

  /* Free context block and return final status (not that anyone cares) */

rtn:
  free (conctx);
  return (sts);
}

/************************************************************************/
/*									*/
/*  There is an event from the pseudo terminal				*/
/*									*/
/*    Input:								*/
/*									*/
/*	conctx = connection context pointer				*/
/*									*/
/************************************************************************/

static void gotptevent (void *conctxv, uLong status, OZ_Mchargs *mchargs)

{
  Conctx *conctx;
  uLong sts;

  conctx = conctxv;

  do {

    /* Check status */

    if (status != OZ_SUCCESS) {
      if (conctx -> termsts == OZ_PENDING) {
        oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u getting event from %s\n", pn, status, conctx -> ptdevname);
      }
      terminate (conctx, -1, status);
      return;
    }

    /* Process event */

    switch (conctx -> ptevent) {
      case OZ_CONPSEUDO_SCR_SUSPEND: {
        break;
      }
      case OZ_CONPSEUDO_SCR_RESUME: {
        break;
      }
      case OZ_CONPSEUDO_SCR_ABORT: {
        break;
      }
      case OZ_CONPSEUDO_KBD_SUSPEND: {
        break;
      }
      case OZ_CONPSEUDO_KBD_RESUME: {
        break;
      }
      case OZ_CONPSEUDO_KBD_READSTARTED: {
        conctx -> kbd_readinprog = 1;
        break;
      }
      case OZ_CONPSEUDO_KBD_READFINISHED: {
        conctx -> kbd_readinprog = 0;
        break;
      }
      case OZ_CONPSEUDO_TERMINATE: {
        oz_sys_io (OZ_PROCMODE_KNL, conctx -> tcpiochan, 0, OZ_IO_IP_TCPCLOSE, 0, NULL);
        terminate (conctx, -1, OZ_SUCCESS);
        return;
      }
      case OZ_CONPSEUDO_GETSETMODE: {
        OZ_Console_modebuff modebuff;
        OZ_IO_conpseudo_fetchgsmodereq conpseudo_fetchgsmodereq;
        OZ_IO_conpseudo_postgsmodereq conpseudo_postgsmodereq;

        /* Here they are trying to set the characteristics of the terminal, like with a 'set console -columns 132' command */

        memset (&conpseudo_fetchgsmodereq, 0, sizeof conpseudo_fetchgsmodereq);
        memset (&conpseudo_postgsmodereq,  0, sizeof conpseudo_postgsmodereq);
        conpseudo_fetchgsmodereq.size  = sizeof modebuff;
        conpseudo_fetchgsmodereq.buff  = &modebuff;
        conpseudo_fetchgsmodereq.reqid_r = &(conpseudo_postgsmodereq.reqid);
        conpseudo_postgsmodereq.size   = sizeof modebuff;
        conpseudo_postgsmodereq.buff   = &modebuff;
        conpseudo_postgsmodereq.status = OZ_SUCCESS;

        sts = oz_sys_io (OZ_PROCMODE_KNL, conctx -> ptiochan, 0, OZ_IO_CONPSEUDO_FETCHGSMODEREQ, 
                         sizeof conpseudo_fetchgsmodereq, &conpseudo_fetchgsmodereq);
        if (sts != OZ_SUCCESS) oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u fetching get/set mode request\n", pn, sts);
        else {
          if (conctx -> naws_width  != 0) modebuff.columns = conctx -> naws_width;
          if (conctx -> naws_height != 0) modebuff.rows    = conctx -> naws_height;
          sts = oz_sys_io (OZ_PROCMODE_KNL, conctx -> ptiochan, 0, OZ_IO_CONPSEUDO_POSTGSMODEREQ, 
                           sizeof conpseudo_postgsmodereq, &conpseudo_postgsmodereq);
          if (sts != OZ_SUCCESS) oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u posting get/set mode request\n", pn, sts);
        }
        break;
      }
    }

    /* See if there are any more events waiting to be processed */

    status = oz_sys_io_start (OZ_PROCMODE_KNL, conctx -> ptiochan, NULL, 0, gotptevent, conctx, 
                              OZ_IO_CONPSEUDO_GETEVENT, sizeof conctx -> conpseudo_getevent, &(conctx -> conpseudo_getevent));

    /* Repeat if so, else come back when there is */

  } while (status != OZ_STARTED);
}

/************************************************************************/
/*									*/
/*  Start reading screen data from the pseudo-terminal			*/
/*									*/
/************************************************************************/

static void getscreendata (Conctx *conctx)

{
  uLong sts;

  if (conctx -> conpseudo_getscrdata.size == 0) {
    sts = conctx -> screenins;									// offset to start reading at
    if (sts == conctx -> screenrem) {								// check for empty ring buffer
      conctx -> screenins = 0;									// if so, reset indicies
      conctx -> screenrem = 0;
      sts = 0;											// ... and the whole thing is available
      conctx -> conpseudo_getscrdata.size = sizeof conctx -> screenbuf;
    } else if (sts < sizeof conctx -> screenbuf) {						// see if available space wraps around the end
      conctx -> conpseudo_getscrdata.size = sizeof conctx -> screenbuf - conctx -> screenins;	// if so, read up to the end of the buffer
      if (conctx -> screenrem >= conctx -> conpseudo_getscrdata.size) conctx -> conpseudo_getscrdata.size *= 2; // the CR's can push data around the wrap
      else conctx -> conpseudo_getscrdata.size += conctx -> screenrem;
    } else {
      sts -= sizeof conctx -> screenbuf;							// no wrap, read up to removal point
      conctx -> conpseudo_getscrdata.size = conctx -> screenrem - sts;
    }
    conctx -> conpseudo_getscrdata.size /= 2;							// have to leave room for CR's in case we get a buffer full of LF's
    if (conctx -> conpseudo_getscrdata.size != 0) {						// see if we have any space now
      conctx -> conpseudo_getscrdata.buff = conctx -> screenbuf + sts;				// ok, read into buffer at the insertion point
      sts = oz_sys_io_start (OZ_PROCMODE_KNL, conctx -> ptiochan, NULL, 0, gotscreendata, conctx, 
                             OZ_IO_CONPSEUDO_GETSCRDATA, sizeof conctx -> conpseudo_getscrdata, &(conctx -> conpseudo_getscrdata));
      if (sts != OZ_STARTED) oz_sys_thread_queueast (OZ_PROCMODE_KNL, 0, gotscreendata, conctx, 0, sts);
    }
  }
}

/************************************************************************/
/*									*/
/*  There is some screen data that needs to be sent over the network	*/
/*									*/
/*    Input:								*/
/*									*/
/*	conctx = connection context pointer				*/
/*									*/
/************************************************************************/

typedef struct { Conctx *conctx;
                 uLong size;
               } Xip;

static void xmitscreendata2 (Conctx *conctx, int end);
static void xmitscreendata (Conctx *conctx, int size, int offs);

static void gotscreendata (void *conctxv, uLong status, OZ_Mchargs *mchargs)

{
  char c;
  Conctx *conctx;
  int barelfs, i, j, k, lastlf;
  uLong sts;
  Xip *xip;

  conctx = conctxv;

  /* No inter reading from pseudo terminal screen */

  conctx -> conpseudo_getscrdata.size = 0;

  /* Abort connection if failure (pseudo-terminal died) */

  if (status != OZ_SUCCESS) {
    if (conctx -> termsts == OZ_PENDING) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u getting screen data from %s\n", pn, status, conctx -> ptdevname);
    }
    terminate (conctx, -1, status);
  }

  /* Otherwise, queue data to be transmitted over network */

  else {

    /* Convert LF's to CRLF's */

    barelfs = 0;									// haven't seen any LF's that don't already have CR's
    lastlf  = conctx -> screenrem + conctx -> screenxip;				// assume last LF was at end of last stuff transmitted
    j = conctx -> screenins;								// point to beginning of data
    if (j >= sizeof conctx -> screenbuf) j -= sizeof conctx -> screenbuf;
    for (i = 0; i < conctx -> screenrln; i ++) {					// scan through the screen data we got
      if (conctx -> screenbuf[j+i] == '\n') {						// see if it is an LF
        barelfs += ((i == 0) || (conctx -> screenbuf[j+i-1] != '\r'));			// maybe an CR needs to be inserted before LF
        lastlf   = conctx -> screenins + i + 1;						// save offset that includes last LF in buffer
      }
    }
    j = conctx -> screenins + conctx -> screenrln;					// get index where end of data is now
    i = j + barelfs;									// get index where new end will be
    if (j > sizeof conctx -> screenbuf) j -= sizeof conctx -> screenbuf;
    if (i > sizeof conctx -> screenbuf) i -= sizeof conctx -> screenbuf;
    for (k = conctx -> screenrln; (i != j) && (-- k >= 0);) {
      c = conctx -> screenbuf[--i] = conctx -> screenbuf[--j];				// move a char
      if (i == 0) i = sizeof conctx -> screenbuf;					// maybe wrap indicies
      if (j == 0) j = sizeof conctx -> screenbuf;
      if ((c == '\n') && (k > 0) && (conctx -> screenbuf[j-1] != '\r')) {		// see if it was an LF that doesn't already have a CR
        conctx -> screenbuf[--i] = '\r';						// if so, insert the CR before it
        if (i == 0) i = sizeof conctx -> screenbuf;					// ... and maybe wrap index
      }
    }
    lastlf += barelfs;									// this is offset including last LF and all inserted CR's

    /* Increment pointer to end of valid data */

    conctx -> screenins += conctx -> screenrln + barelfs;

    /* Start transmitting buffer up to and including last LF over the network.  But if a  */
    /* kb read is active, transmit the whole thing (it's probably just a character echo). */

    if (conctx -> kbd_readinprog) lastlf = conctx -> screenins;
    xmitscreendata2 (conctx, lastlf);

    /* If there's a last little bit of a partial line, flush it in a tenth second unless something better comes along */

    conctx -> screenflushtime.datebin = oz_hw_tod_getnow () + (OZ_TIMER_RESOLUTION / 10);
    if ((lastlf != conctx -> screenins) && !(conctx -> screenflushbusy)) {
      conctx -> screenflushbusy = 1;
      conctx -> refcount ++;
      sts = oz_sys_io_start (OZ_PROCMODE_KNL, timeriochan, NULL, 0, flushscreendata, conctx, 
                             OZ_IO_TIMER_WAITUNTIL, sizeof conctx -> screenflushtime, &(conctx -> screenflushtime));
      if (sts != OZ_STARTED) oz_sys_thread_queueast (OZ_PROCMODE_KNL, 0, flushscreendata, conctx, 0, sts);
    }

    /* Start reading more from pseudo terminal for the screen */

    getscreendata (conctx);
  }
}

/* This timer ast routine is called when there is a last little bit of screen data to flush */

static void flushscreendata (void *conctxv, uLong status, OZ_Mchargs *mchargs)

{
  Conctx *conctx;
  uLong sts;

  conctx = conctxv;

  if (status != OZ_SUCCESS) oz_sys_condhand_signal (2, status, 0);

  if (-- (conctx -> refcount) == 0) oz_sys_event_set (OZ_PROCMODE_KNL, conctx -> termevent, 1, NULL);
  else if (oz_hw_tod_getnow () < conctx -> screenflushtime.datebin) {
    conctx -> refcount ++;
    sts = oz_sys_io_start (OZ_PROCMODE_KNL, timeriochan, NULL, 0, flushscreendata, conctx, 
                           OZ_IO_TIMER_WAITUNTIL, sizeof conctx -> screenflushtime, &(conctx -> screenflushtime));
    if (sts != OZ_STARTED) oz_sys_thread_queueast (OZ_PROCMODE_KNL, 0, flushscreendata, conctx, 0, sts);
  } else {
    conctx -> screenflushbusy = 0;
    xmitscreendata2 (conctx, conctx -> screenins);
  }
}

/* Start transmitting some data over the network link */
/*   end = offset in buffer just past end of data     */

static void xmitscreendata2 (Conctx *conctx, int end)

{
  int beg;

  beg = conctx -> screenrem + conctx -> screenxip;			// point to beginning of data to transmit
  if (beg >= sizeof conctx -> screenbuf) {				// see if the start is wrapped off end
    beg -= sizeof conctx -> screenbuf;					// if so, unwrap the offsets
    end -= sizeof conctx -> screenbuf;
  }
  if (end > sizeof conctx -> screenbuf) {				// see if it wraps around end of buffer
    xmitscreendata (conctx, sizeof conctx -> screenbuf - beg, beg);	// if so, transmit as two pieces
    xmitscreendata (conctx, end - sizeof conctx -> screenbuf, 0);
  } else {
    xmitscreendata (conctx, end - beg, beg);				// else, transmit as one piece
  }
}

/* Start transmitting data, 'size' bytes starting at 'offs' */

static void xmitscreendata (Conctx *conctx, int size, int offs)

{
  uLong sts;
  Xip *xip;

  if (size != 0) {						// make sure there is something to transmit
    conctx -> screenxip += size;				// this much more is being transmitted
    conctx -> refcount  ++;					// don't let conctx be deleted meanwhile
    xip = malloc (sizeof *xip);					// allocate a transmit-in-progress struct
    xip -> conctx = conctx;					// save context block pointer
    xip -> size   = size;					// save transmit size
    conctx -> ip_tcptransmit.rawsize = size;			// start transmitting the data
    conctx -> ip_tcptransmit.rawbuff = conctx -> screenbuf + offs;
    sts = oz_sys_io_start (OZ_PROCMODE_KNL, conctx -> tcpiochan, NULL, 0, sentscreendata, xip, 
                           OZ_IO_IP_TCPTRANSMIT, sizeof conctx -> ip_tcptransmit, &(conctx -> ip_tcptransmit));
    if (sts != OZ_STARTED) oz_sys_thread_queueast (OZ_PROCMODE_KNL, 0, sentscreendata, xip, 0, sts);
  }
}

/************************************************************************/
/*									*/
/*  Ast routine when the transmit has completed				*/
/*									*/
/*    Input:								*/
/*									*/
/*	sendctxv = send context pointer					*/
/*	status   = completion status					*/
/*									*/
/************************************************************************/

static void sentscreendata (void *xipv, uLong status, OZ_Mchargs *mchargs)

{
  Conctx *conctx;
  uLong size, sts;
  Xip *xip;

  xip = xipv;							// point to transmit-in-progress struct
  size   = xip -> size;						// get the size transmitted
  conctx = xip -> conctx;					// get the normal context block pointer
  free (xip);							// free transmit-in-progress struct

  /* The transmit incremented the refcount, so decrement and terminate if refcount now zero */

  if (-- (conctx -> refcount) == 0) oz_sys_event_set (OZ_PROCMODE_KNL, conctx -> termevent, 1, NULL);

  /* Abort if error transmitting screen data */

  else if (status != OZ_SUCCESS) {				// check status, maybe client aborted
    if (conctx -> termsts == OZ_PENDING) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u transmitting screen data for %s\n", pn, status, conctx -> ptdevname);
    }
    terminate (conctx, -1, status);				// terminate the connection
  }

  /* Transmit is ok, increment the 'remove' pointer past the transmitted data and make sure we have a 'read from screen' going */

  else {
    conctx -> screenxip -= size;				// we have that much less being transmitted now
    conctx -> screenrem += size;				// increment past the transmitted data
    if (conctx -> screenrem >= sizeof conctx -> screenbuf) {	// see if we need to wrap the pointers
      conctx -> screenrem -= sizeof conctx -> screenbuf;
      conctx -> screenins -= sizeof conctx -> screenbuf;
    }
    getscreendata (conctx);					// now see if we need to start reading from pseudo terminal screen
  }
}

/************************************************************************/
/*									*/
/*  Some tcp data was received over the network - send it to the 	*/
/*  pseudo-terminal as keyboard data					*/
/*									*/
/*    Input:								*/
/*									*/
/*	conctx = connection context block				*/
/*	status = receive status						*/
/*									*/
/************************************************************************/

static void gotkeybdata (void *conctxv, uLong status, OZ_Mchargs *mchargs)

{
  char c, *p, *q, *r;
  Conctx *conctx;
  int i;
  OZ_Console_modebuff modebuff;
  OZ_IO_conpseudo_setmode conpseudo_setmode;
  uLong sts;

  conctx = conctxv;

  do {

    /* Check network receive status */

    if (status != OZ_SUCCESS) {
      if (conctx -> termsts == OZ_PENDING) {
        oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u receiving keyboard data for %s\n", pn, status, conctx -> ptdevname);
      }
      terminate (conctx, -1, status);
      return;
    }

    /* Strip out any telnet control strings, except get the window size if its in there */

    p = q = r = conctx -> keybdbuff + conctx -> keybdoffs;
    for (i = conctx -> keybdrlen; -- i >= 0;) {
      if ((c = *(p ++)) != (char)255) *(q ++) = c;
      else if ((p[0] == (char)250) && (p[1] == (char)31) && (p[6] == (char)255) && (p[7] == (char)240)) {

        /* The terminal is telling us about itself, so pass it on to the pseudo-terminal driver */

        if (i < 8) break;
        conctx -> naws_width  = (p[2] << 8) | (p[3] & 0xFF);		// get new width and height
        conctx -> naws_height = (p[4] << 8) | (p[5] & 0xFF);
        memset (&conpseudo_setmode, 0, sizeof conpseudo_setmode);	// tell pseudo driver what they are now
        conpseudo_setmode.size = sizeof modebuff;
        conpseudo_setmode.buff = &modebuff;
        memset (&modebuff, 0, sizeof modebuff);
        modebuff.columns = conctx -> naws_width;
        modebuff.rows    = conctx -> naws_height;
        sts = oz_sys_io (OZ_PROCMODE_KNL, conctx -> ptiochan, 0, OZ_IO_CONPSEUDO_SETMODE, 
                         sizeof conpseudo_setmode, &conpseudo_setmode);
        if (sts != OZ_SUCCESS) oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u setting new width and height\n", pn, sts);
        i -= 8;								// skip over the values
        p += 8;
      } else if (p[0] == (char)255) {
        *(q ++) = (char)255;
        i --;
        p ++;
      } else {
        i -= 2;
        p += 2;
      }
    }

    /* Pass it to pseudo-terminal driver */

    conctx -> conpseudo_putkbddata.size = q - r;
    conctx -> conpseudo_putkbddata.buff = r;
    sts = oz_sys_io (OZ_PROCMODE_KNL, conctx -> ptiochan, 0, OZ_IO_CONPSEUDO_PUTKBDDATA, sizeof conctx -> conpseudo_putkbddata, &(conctx -> conpseudo_putkbddata));
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u sending keyboard data to %s\n", pn, sts, conctx -> ptdevname);
      terminate (conctx, -1, sts);
      return;
    }

    /* Increment receive window buffer pointer */

    conctx -> keybdoffs += conctx -> keybdrlen;
    if (conctx -> keybdoffs == sizeof conctx -> keybdbuff) conctx -> keybdoffs = 0;

    /* Start reading more from network */

    status = oz_sys_io_start (OZ_PROCMODE_KNL, conctx -> tcpiochan, NULL, 0, gotkeybdata, conctx, 
                              OZ_IO_IP_TCPRECEIVE, sizeof conctx -> ip_tcpreceive, &(conctx -> ip_tcpreceive));
  } while (status != OZ_STARTED);
}

/************************************************************************/
/*									*/
/*  Terminate the connection						*/
/*									*/
/************************************************************************/

static void terminate (Conctx *conctx, int inc, uLong status)

{
  if (conctx -> termsts == OZ_PENDING) {				// see if this routine has been called yet
    conctx -> termsts = status;						// if not, save the termination status
    oz_sys_io_abort (OZ_PROCMODE_KNL, conctx -> tcpiochan);		// abort any I/O queued to the network
    oz_sys_io_abort (OZ_PROCMODE_KNL, conctx -> ptiochan);		// abort any I/O queued to the pseudo-terminal
    oz_sys_handle_release (OZ_PROCMODE_KNL, conctx -> tcpiochan);	// release handle to network I/O channel
    oz_sys_handle_release (OZ_PROCMODE_KNL, conctx -> ptiochan);	// release handle to pseudo-terminal I/O channel
    conctx -> tcpiochan = 0;						// clear handles so we can't use them again
    conctx -> ptiochan  = 0;
  }
  conctx -> refcount += inc;						// dec ref count, if zero, wake thread to free conctx
  if (conctx -> refcount == 0) oz_sys_event_set (OZ_PROCMODE_KNL, conctx -> termevent, 1, NULL);
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
