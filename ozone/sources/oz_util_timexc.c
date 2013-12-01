//+++2003-11-18
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
//---2003-11-18

/************************************************************************/
/*									*/
/*  This program gets the current date/time from the supplied 		*/
/*  ntpservers by using sntp described by rfc1769.  It then adjusts 	*/
/*  the clock frequency to keep better time.				*/
/*									*/
/*	timexc <tolerance> <logname> <ntpserver> ...			*/
/*		[-repeat <interval>]					*/
/*		[-set]							*/
/*		[-verbose]						*/
/*		[-writeboot <bootfile>]					*/
/*									*/
/*	-repeat     = repetitive at the given interval			*/
/*	-set        = set current time to remote system time		*/
/*	                and adjust clock frequency to track		*/
/*	              else, adjust clock frequency to converge		*/
/*	                    after equally spaced repeated runs, 	*/
/*	                    but don't change current time		*/
/*	-verbose    = print out all replies				*/
/*	-writeboot  = write bootblock with new clock freq		*/
/*	tolerance   = maximum number of seconds to adjust time by	*/
/*	logicalname = name of logical to write new time to		*/
/*	              and read last update time from			*/
/*	ntpserver ... = NTP servers					*/
/*									*/
/************************************************************************/

#include <ozone.h>
#include <oz_io_ip.h>
#include <oz_io_timer.h>
#include <oz_knl_hw.h>
#include <oz_knl_sdata.h>
#include <oz_knl_status.h>
#include <oz_ldr_params.h>
#include <oz_sys_callknl.h>
#include <oz_sys_condhand.h>
#include <oz_sys_dateconv.h>
#include <oz_sys_event.h>
#include <oz_sys_gethostipaddr.h>
#include <oz_sys_io.h>
#include <oz_sys_io_fs_printf.h>
#include <oz_sys_logname.h>
#include <oz_sys_pdata.h>
#include <oz_util_start.h>

/* Number of times to request timestamp from each host */

#define RETRIES 3

/* Give NTP server up to a second to reply */

#define TIMEOUT OZ_TIMER_RESOLUTION

/* SNTP request/reply messages */

#define PORTNO 123

typedef struct {
	uByte livnmode;			// 2msb: leap indicator = 00 : no warning
					//                        01 : last minute of day has 61 seconds
					//                        10 : last minute of day has 59 seconds
					//                        11 : alarm (clock not synchronized)
					// 3mid: version = 100 (version 4)
					// 3lsb: mode = 000 : reserved
					//              001 : symmetric active
					//              010 : symmetric passive
					//              011 : client (unicast request)
					//              100 : server (unicast reply)
					//              101 : broadcast
					//              110 : NTP control message
					//              111 : reserved
	uByte stratum;			// levels removed from standard
					//   0 : unknown
					//   1 : primary reference
					//   n : levels removed
	 Byte poll;
	 Byte precision;		// local clock precision, in seconds 2^precision
	uByte rootdelay[4];
	uByte rootdispersion[4];
	uByte referenceidentifier[4];
	uByte referencetimestamp[8];
	uByte originatetimestamp[8];	// time according to client that request was sent
	uByte receivetimestamp[8];	// time according to server that request was received
	uByte transmittimestamp[8];	// time according to server that reply was sent
	uByte keyidentifier[4];
	uByte messagedigest[16];
} Sntp;

typedef struct {
	OZ_Datebin rtt;			// what this computer's perception of the roundtrip time was
	OZ_Datebin act;			// what the server's perception of the midpoint time was
	OZ_Datebin cpu;			// what this computer's perception of the midpoint time was
	int ok;				// 1 : server access successful; 0 : server access failed
	char *host;			// server's hostname
} Ntparam;

static const char *pn = "timexc";
static OZ_Datebin basetime, thisact, thiscpu;
static OZ_Loadparams bootparams;
static uLong newfreq, oldfreq;

static void readntptime (char *hoststr, Ntparam *ntparam);
static void oztimetontptime (OZ_Datebin oztime, uByte ntptime[8]);
static uQuad ntptimetooztime (uByte ntptime[8]);
static uLong getoldfreq (OZ_Procmode cprocmode, void *dummy);
static uLong setthisact (OZ_Procmode cprocmode, void *dummy);
static uLong updatertc (OZ_Procmode cprocmode, void *dummy);
static uLong setnewfreq (OZ_Procmode cprocmode, void *dummy);
static uLong getbootparams (OZ_Procmode cprocmode, void *dummy);
static uLong scale32x64s64 (uLong n1, uQuad n2, uQuad d);
static uQuad scale64x64s64 (uQuad n1, uQuad n2, uQuad d);

uLong oz_util_main (int argc, char *argv[])

{
  char **hoststr, *logname, *p, *tolrstr, *writeboot;
  char tmpbuf[32], tmpbuf2[32];
  int i, j, k, n, nhosts, setflag, tolerance, verbose;
  Long absbehindmsec, behindmsec;
  Ntparam *ntparams;
  OZ_Datebin interval, lastact, lastcpu, nextact, nextcpu, ntotalrtts, repeat, totalrtts;
  OZ_Handle h_bootfile, h_logname, h_lognamtbl, h_repeat;
  OZ_IO_fs_open fs_open;
  OZ_IO_fs_writeblocks fs_writeblocks;
  OZ_IO_timer_waituntil timer_waituntil;
  OZ_Logvalue logvalues[2];
  Quad thiserr;
  uLong datelongs[OZ_DATELONG_ELEMENTS], freq, received_len, sts;

  if (argc > 0) pn = argv[0];

  /* Parse command line arguments */

  tolrstr   = NULL;
  logname   = NULL;
  hoststr   = NULL;
  repeat    = 0;
  setflag   = 0;
  verbose   = 0;
  writeboot = NULL;

  for (i = 1; i < argc; i ++) {
    if (strcasecmp (argv[i], "-repeat") == 0) {
      if (++ i >= argc) goto usage;
      if (oz_sys_datebin_encstr (strlen (argv[i]), argv[i], &repeat) >= 0) goto usage;
      continue;
    }
    if (strcasecmp (argv[i], "-set") == 0) {
      setflag = 1;
      continue;
    }
    if (strcasecmp (argv[i], "-verbose") == 0) {
      verbose = 1;
      continue;
    }
    if (strcasecmp (argv[i], "-writeboot") == 0) {
      if (++ i >= argc) goto usage;
      writeboot = argv[i];
      continue;
    }
    if (argv[i][0] == '-') goto usage;
    if (tolrstr == NULL) {
      tolrstr = argv[i];
      continue;
    }
    if (logname == NULL) {
      logname = argv[i];
      continue;
    }
    if (hoststr == NULL) {
      hoststr = argv + i;
      nhosts  = argc - i;
      break;
    }
    goto usage;
  }
  if (hoststr == NULL) goto usage;

  tolerance = atoi (tolrstr);

  /* If -repeat, set up repeat timer */

  sts = oz_sys_io_assign (OZ_PROCMODE_KNL, &h_repeat, OZ_IO_TIMER_DEV, OZ_LOCKMODE_NL);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u assigning channel to %s\n", pn, sts, OZ_IO_TIMER_DEV);
    return (sts);
  }
  memset (&timer_waituntil, 0, sizeof timer_waituntil);

  /* Set up stuff independent of repeatloop */

  ntparams = oz_sys_pdata_malloc (OZ_PROCMODE_KNL, nhosts * RETRIES * sizeof *ntparams);

  sts = oz_sys_logname_lookup (0, OZ_PROCMODE_USR, oz_s_logname_defaulttables, NULL, NULL, NULL, &h_lognamtbl);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u getting %s handle\n", pn, oz_s_logname_defaulttables, sts);
    return (sts);
  }

  /* NTP timestamps are relative to Jan 1, 1900 */

  memset (datelongs, 0, sizeof datelongs);
  datelongs[OZ_DATELONG_DAYNUMBER] = oz_sys_daynumber_encode ((1900 << 16) | (1 << 8) | 1);
  basetime = oz_sys_datebin_encode (datelongs);

  /* Get the date/time we were last run so we can tell how long it's been */

repeatloop:
  oz_sys_io_fs_printf (oz_util_h_output, "\n");

  lastcpu = 0;
  lastact = 0;
  oldfreq = 0;

  sts = oz_sys_logname_lookup (h_lognamtbl, OZ_PROCMODE_USR, logname, NULL, NULL, &received_len, &h_logname);
  if (sts == OZ_SUCCESS) {
    if (received_len > 0) {
      sts = oz_sys_logname_getval (h_logname, 0, NULL, sizeof tmpbuf, tmpbuf, &received_len, NULL, 0, NULL);
      if (sts != OZ_SUCCESS) {
        oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u getting %s value[0]\n", pn, sts, logname);
        return (sts);
      }
      if (oz_sys_datebin_encstr (received_len, tmpbuf, &lastcpu) <= 0) {
        oz_sys_io_fs_printf (oz_util_h_error, "%s: error converting last date %s from logical %s\n", pn, tmpbuf, logname);
        return (OZ_BADPARAM);
      }
      sts = oz_sys_logname_getval (h_logname, 1, NULL, sizeof tmpbuf, tmpbuf, &received_len, NULL, 0, NULL);
      if (sts != OZ_SUCCESS) {
        oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u getting %s value[1]\n", pn, sts, logname);
        return (sts);
      }
      if (oz_sys_datebin_encstr (received_len, tmpbuf, &lastact) <= 0) {
        oz_sys_io_fs_printf (oz_util_h_error, "%s: error converting last date %s from logical %s\n", pn, tmpbuf, logname);
        return (OZ_BADPARAM);
      }
    }
    oz_sys_handle_release (OZ_PROCMODE_KNL, h_logname);
  } else if (sts != OZ_NOLOGNAME) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u getting %s handle\n", pn, sts, logname);
    return (sts);
  }

  /* Request the current time from the servers */
  /* ntparams[host][retry]                     */

  for (i = 0; i < RETRIES; i ++) {
    for (j = 0; j < nhosts; j ++) {
      readntptime (hoststr[j], ntparams + j * RETRIES + i);
      if (verbose && ntparams[j*RETRIES+i].ok) {
        if (ntparams[j*RETRIES+i].cpu >= ntparams[j*RETRIES+i].act) {
          p = " ";
          interval = ntparams[j*RETRIES+i].cpu - ntparams[j*RETRIES+i].act;
        } else {
          p = "-";
          interval = ntparams[j*RETRIES+i].act - ntparams[j*RETRIES+i].cpu;
        }
        oz_sys_io_fs_printf (oz_util_h_output, " rtt from %s[%d] is %#t, %s%#t\n", 
							ntparams[j*RETRIES+i].host, i, ntparams[j*RETRIES+i].rtt, p, interval);
      }
    }
  }

  /* Reject any that are out of tolerance */

  for (i = 0; i < nhosts * RETRIES; i ++) {
    if (ntparams[i].ok) {

      /* See how far behind (in msec) this computer is */

      thiserr = ntparams[i].act - ntparams[i].cpu;
      behindmsec = thiserr / (OZ_TIMER_RESOLUTION / 1000);

      /* If it is way off, reject it (the server might be set wrong or something like that) */

      absbehindmsec = (behindmsec < 0) ? -behindmsec : behindmsec;
      if (absbehindmsec > tolerance * 1000) {
        oz_sys_io_fs_printf (oz_util_h_error, "%s: difference %d.%3.3d is too great\n", pn, absbehindmsec / 1000, absbehindmsec % 1000);
        oz_sys_io_fs_printf (oz_util_h_error, "%s:    thisact %t\n", pn, ntparams[i].act);
        oz_sys_io_fs_printf (oz_util_h_error, "%s:    thiscpu %t\n", pn, ntparams[i].cpu);
        oz_sys_io_fs_printf (oz_util_h_error, "%s:     server %s\n", pn, ntparams[i].host);
        ntparams[i].ok = 0;
      }
    }
  }

  /* For each different server, find the query that had the fastest roundtrip time */

  n = 0;
  totalrtts = 0;
  for (j = 0; j < nhosts; j ++) {							// loop for each server
    for (i = 0; i < RETRIES; i ++) {							// scan for 1st success
      if (ntparams[j*RETRIES+i].ok) break;
    }
    if (i < RETRIES) {
      uQuad bestrtt;

      bestrtt = ntparams[j*RETRIES+i].rtt;						// that's the best rtt so far
      for (k = i + 1; k < RETRIES; k ++) {						// scan the rest of the tries
        if (ntparams[j*RETRIES+k].ok) {
          if (bestrtt <= ntparams[j*RETRIES+k].rtt) ntparams[j*RETRIES+k].ok = 0;	// see if it is better
          else {
            ntparams[j*RETRIES+i].ok = 0;						// if so, mark old one bad
            bestrtt = ntparams[j*RETRIES+k].rtt;					// this is now the best so far
            i = k;
          }
        }
      }
      n ++;										// we have one more good server
      totalrtts += bestrtt;								// total up the best rtt's for all servers
      if (ntparams[j*RETRIES+i].cpu > ntparams[j*RETRIES+i].act) {			// print it out
        p = "ahead";
        interval = ntparams[j*RETRIES+i].cpu - ntparams[j*RETRIES+i].act;
      } else if (ntparams[j*RETRIES+i].cpu < ntparams[j*RETRIES+i].act) {
        p = "behind";
        interval = ntparams[j*RETRIES+i].act - ntparams[j*RETRIES+i].cpu;
      } else {
        p = NULL;
      }
      if (p == NULL) oz_sys_io_fs_printf (oz_util_h_output, " best rtt from %s is %#t, says we are on time\n", 
							ntparams[j*RETRIES+i].host, bestrtt);
      else oz_sys_io_fs_printf (oz_util_h_output, " best rtt from %s is %#t, says we are %s by %#t\n", 
							ntparams[j*RETRIES+i].host, bestrtt, p, interval);
    }
  }

  if (n == 0) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: no valid servers found\n", pn);
    sts = OZ_MISSINGPARAM;
    thiscpu = oz_hw_tod_getnow ();
    goto repeatwait;
  }

  /* Take average of the quickest rtt result from each server as the gospel truth */

  if (n == 1) {
    for (j = 0; j < nhosts; j ++) {
      for (i = 0; i < RETRIES; i ++) {
        if (ntparams[j*RETRIES+i].ok) break;
      }
      if (i < RETRIES) {
        thisact = ntparams[j*RETRIES+i].act;
        thiscpu = ntparams[j*RETRIES+i].cpu;
      }
    }
  } else {
    thisact    = 0;
    thiscpu    = 0;
    ntotalrtts = totalrtts * (n - 1);				// common divisor
    for (j = 0; j < nhosts; j ++) {				// loop through all servers
      for (i = 0; i < RETRIES; i ++) {				// find the best rtt entry for the server
        if (ntparams[j*RETRIES+i].ok) break;
      }
      if (i < RETRIES) {
        uQuad act, addlact, addlcpu, cpu, otherrtts, rtt;

        act = ntparams[j*RETRIES+i].act;			// get the time the server thought it was
        cpu = ntparams[j*RETRIES+i].cpu;			// get the time this cpu thought is was
        rtt = ntparams[j*RETRIES+i].rtt;			// get the round trip time
        otherrtts = totalrtts - rtt;				// get total of all the other server's rtt
        addlact   = scale64x64s64 (act, otherrtts, ntotalrtts);	// get weighted actual time
        addlcpu   = scale64x64s64 (cpu, otherrtts, ntotalrtts);	// get weighted cpu time
        thisact  += addlact;					// add weighted times to the totals
        thiscpu  += addlcpu;
      }
    }
  }

  /* Get the current estimate of cpu frequency */
  /* This should always be non-zero            */

  sts = oz_sys_callknl (getoldfreq, NULL);
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
  newfreq = oldfreq;					// assume it will stay the same

  /* See how far behind (in msec) this computer is */

  thiserr = thisact - thiscpu;
  behindmsec = thiserr / (OZ_TIMER_RESOLUTION / 1000);
  if (behindmsec == 0) goto noop;

  /* If there was a last measurement, calculate true frequency of the clock */

  if (lastcpu != 0) {
    interval = thiscpu - lastcpu;
    freq = scale32x64s64 (oldfreq, interval, thisact - lastact);
  }

  /* If -set, then set the cpu's time = actual time */
  /* Otherwise, below, we just converge on it       */

  if (setflag) {
    sts = oz_sys_callknl (setthisact, NULL);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u setting current time\n", pn, sts);
      return (sts);
    }
    thiscpu = thisact;
  }

  /* If not -set, set the time to the current time */
  /* This just updates the RTC to what cpu thinks time is */

  else {
    sts = oz_sys_callknl (updatertc, NULL);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u updating RTC\n", pn, sts);
      return (sts);
    }
  }

  /* If there was a last run, adjust our estimate of the cpu frequency to keep time better */

  if (lastcpu != 0) {
    nextcpu = interval + thiscpu;				// when we will check it out next
    nextact = thiserr / 2 + nextcpu;				// what the actual time will be then for half the error we have now
    newfreq = scale32x64s64 (freq, nextact - thisact, interval); // what counter rate needs to be to accomplish that
    if (newfreq != oldfreq) {
      sts = oz_sys_callknl (setnewfreq, NULL);
      if (sts != OZ_SUCCESS) {
        oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u adjusting cpu frequency estimate\n", pn, sts);
        return (sts);
      }
    }
  }

  /* Display updated time parameters */

noop:
  oz_sys_io_fs_printf (oz_util_h_output, "    freq = %u  ->  %u\n", oldfreq, newfreq);
  oz_sys_io_fs_printf (oz_util_h_output, " thiscpu = %t\n", thiscpu);
  oz_sys_io_fs_printf (oz_util_h_output, " thisact = %t\n", thisact);

  /* Write current date/time to logical so we know next time we run how big the interval is */

  oz_sys_datebin_decstr (0, thiscpu, sizeof tmpbuf,  tmpbuf);
  oz_sys_datebin_decstr (0, thisact, sizeof tmpbuf2, tmpbuf2);
  logvalues[0].attr = 0;
  logvalues[0].buff = tmpbuf;
  logvalues[1].attr = 0;
  logvalues[1].buff = tmpbuf2;
  sts = oz_sys_logname_create (h_lognamtbl, logname, OZ_PROCMODE_USR, 0, 2, logvalues, NULL);
  if (sts == OZ_SUPERSEDED) sts = OZ_SUCCESS;
  else if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u creating logical name %s\n", pn, logname);
  }

  /* If -writeboot, rewrite bootblock with new cpu frequency */

  if (writeboot != NULL) {

    /* Get current bootparams from kernel */

    sts = oz_sys_callknl (getbootparams, NULL);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u getting boot params from kernel\n", pn, sts);
      return (sts);
    }

    /* Set signature to new value indicating when they were written */

    memset (bootparams.signature, 0, sizeof bootparams.signature);
    oz_sys_datebin_decstr (0, thiscpu, sizeof bootparams.signature, bootparams.signature);

    /* Open the boot file */

    memset (&fs_open, 0, sizeof fs_open);
    fs_open.name = writeboot;
    fs_open.lockmode = OZ_LOCKMODE_PW;
    sts = oz_sys_io_fs_open (sizeof fs_open, &fs_open, 0, &h_bootfile);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u opening bootfile %s\n", pn, sts, writeboot);
      return (sts);
    }

    /* Write boot params to boot file */

    memset (&fs_writeblocks, 0, sizeof fs_writeblocks);
    fs_writeblocks.size = sizeof bootparams;
    fs_writeblocks.buff = &bootparams;
    fs_writeblocks.svbn = OZ_LDR_PARAMS_VBN;
    sts = oz_sys_io (OZ_PROCMODE_KNL, h_bootfile, 0, OZ_IO_FS_WRITEBLOCKS, sizeof fs_writeblocks, &fs_writeblocks);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u writing system params to boot file\n", pn, sts);
      return (sts);
    }

    /* Write bootblock to hard drive */

    sts = oz_sys_io (OZ_PROCMODE_KNL, h_bootfile, 0, OZ_IO_FS_WRITEBOOT, 0, NULL);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u writing bootblock %s\n", pn, sts, writeboot);
      return (sts);
    }
    oz_sys_io_fs_printf (oz_util_h_output, " bootblock written using %s\n", writeboot);

    /* Close file */

    oz_sys_handle_release (OZ_PROCMODE_KNL, h_bootfile);
  }

  /* If -repeat, wait for the given amount of time then do it all again */

repeatwait:
  if (repeat != 0) {
    timer_waituntil.datebin = thiscpu + repeat;
    oz_sys_io_fs_printf (oz_util_h_output, " next update at %t\n", timer_waituntil.datebin);
    sts = oz_sys_io (OZ_PROCMODE_KNL, h_repeat, 0, OZ_IO_TIMER_WAITUNTIL, sizeof timer_waituntil, &timer_waituntil);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u waiting until %t\n", pn, sts, timer_waituntil.datebin);
      return (sts);
    }
    goto repeatloop;
  }

  return (sts);

usage:
  oz_sys_io_fs_printf (oz_util_h_error, "usage: %s [-repeat <interval>] [-set] [-verbose] [-writeboot <bootfile>] <tolerance> <logname> <ntpserver> ...\n", pn);
  return (OZ_MISSINGPARAM);
}

/************************************************************************/
/*									*/
/*  Do a single query of time from a single NTP server			*/
/*									*/
/************************************************************************/

static void readntptime (char *hoststr, Ntparam *ntparam)

{
  OZ_Handle h_event, h_ipstack, h_timer;
  OZ_IO_ip_udpbind ip_udpbind;
  OZ_IO_ip_udpreceive ip_udpreceive;
  OZ_IO_ip_udptransmit ip_udptransmit;
  OZ_IO_timer_waituntil timer_waituntil;
  Sntp sendbuf, recvbuf;
  uByte server_ipaddr[OZ_IO_IP_ADDRSIZE], server_portno[OZ_IO_IP_PORTSIZE];
  uLong sts;
  volatile uLong recvsts, timersts;

  memset (ntparam, 0, sizeof *ntparam);
  ntparam -> host = hoststr;

  /* Get server's IP address */

  sts = oz_sys_gethostipaddr (hoststr, sizeof server_ipaddr, server_ipaddr);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: %s: error %u getting host ip address\n", pn, hoststr, sts);
    return;
  }

  /* NTP servers are always port 123 */

  memset (server_portno, 0, sizeof server_portno);
  server_portno[sizeof server_portno-1] = PORTNO;

  /* Create an event flag for receive & timer */

  sts = oz_sys_event_create (OZ_PROCMODE_KNL, hoststr, &h_event);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: %s: error %u creating event flag\n", pn, hoststr, sts);
    return;
  }

  /* Get an I/O channel to timer */

  sts = oz_sys_io_assign (OZ_PROCMODE_KNL, &h_timer, OZ_IO_TIMER_DEV, OZ_LOCKMODE_NL);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: %s: error %u assigning channel to %s\n", pn, hoststr, sts, OZ_IO_TIMER_DEV);
    return;
  }

  /* Get an I/O channel to access IP stack */

  sts = oz_sys_io_assign (OZ_PROCMODE_KNL, &h_ipstack, OZ_IO_IP_DEV, OZ_LOCKMODE_CW);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: %s: error %u assigning channel to %s\n", pn, hoststr, sts, OZ_IO_IP_DEV);
    return;
  }

  /* Bind port for receiving reply.  NTP servers will only reply to port 123. */

  memset (&ip_udpbind, 0, sizeof ip_udpbind);
  ip_udpbind.addrsize  = OZ_IO_IP_ADDRSIZE;
  ip_udpbind.portsize  = OZ_IO_IP_PORTSIZE;
  ip_udpbind.lclportno = server_portno;
  ip_udpbind.remipaddr = server_ipaddr;
  ip_udpbind.remportno = server_portno;

  sts = oz_sys_io (OZ_PROCMODE_KNL, h_ipstack, 0, OZ_IO_IP_UDPBIND, sizeof ip_udpbind, &ip_udpbind);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: %s: error %u binding UDP socket\n", pn, hoststr, sts);
    goto rtn;
  }

  /* Transmit request */

  memset (&ip_udptransmit, 0, sizeof ip_udptransmit);
  ip_udptransmit.addrsize  = OZ_IO_IP_ADDRSIZE;
  ip_udptransmit.portsize  = OZ_IO_IP_PORTSIZE;
  ip_udptransmit.rawsize   = sizeof sendbuf;
  ip_udptransmit.rawbuff   = &sendbuf;
  ip_udptransmit.dstipaddr = server_ipaddr;
  ip_udptransmit.dstportno = server_portno;

  memset (&sendbuf, 0, sizeof sendbuf);
  sendbuf.livnmode = 0x1B;	// li=0; vn=3; mode=3
  ntparam -> cpu = oz_hw_tod_getnow ();
  oztimetontptime (ntparam -> cpu, sendbuf.originatetimestamp);
  memcpy (sendbuf.transmittimestamp, sendbuf.originatetimestamp, 8);	// server's apparently echo this, not originatetimestamp

  sts = oz_sys_io (OZ_PROCMODE_KNL, h_ipstack, 0, OZ_IO_IP_UDPTRANSMIT, sizeof ip_udptransmit, &ip_udptransmit);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: %s: error %u transmitting UDP packet\n", pn, hoststr, sts);
    goto rtn;
  }

  /* Start timeout timer */

  memset (&timer_waituntil, 0, sizeof timer_waituntil);
  timer_waituntil.datebin = ntparam -> cpu + TIMEOUT;
  timersts = OZ_PENDING;
  sts = oz_sys_io_start (OZ_PROCMODE_KNL, h_timer, &timersts, h_event, NULL, NULL, 
                         OZ_IO_TIMER_WAITUNTIL, sizeof timer_waituntil, &timer_waituntil);
  if (sts != OZ_STARTED) timersts = sts;

  /* Start receiving reply */

  memset (&ip_udpreceive, 0, sizeof ip_udpreceive);
  ip_udpreceive.addrsize = OZ_IO_IP_ADDRSIZE;
  ip_udpreceive.portsize = OZ_IO_IP_PORTSIZE;
  ip_udpreceive.rawsize  = sizeof recvbuf;
  ip_udpreceive.rawbuff  = &recvbuf;

startreceive:
  memset (&recvbuf, 0, sizeof recvbuf);
  recvsts = OZ_PENDING;
  sts = oz_sys_io_start (OZ_PROCMODE_KNL, h_ipstack, &recvsts, h_event, NULL, NULL, 
                         OZ_IO_IP_UDPRECEIVE, sizeof ip_udpreceive, &ip_udpreceive);
  if (sts != OZ_STARTED) recvsts = sts;

  /* Wait for receive or timeout */

  while ((timersts == OZ_PENDING) && (recvsts == OZ_PENDING)) {
    oz_sys_event_wait (OZ_PROCMODE_KNL, h_event, 0);
    oz_sys_event_set (OZ_PROCMODE_KNL, h_event, 0, NULL);
  }
  sts = recvsts;
  if (sts == OZ_PENDING) {
    sts = timersts;
    if (sts == OZ_SUCCESS) sts = OZ_TIMEDOUT;
  }
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: %s: error %u receiving UDP packet\n", pn, hoststr, sts);
    goto rtn;
  }

  /* Make sure the livnmode bits are ok */

  if (((recvbuf.livnmode & 0xC0) == 0xC0) && ((recvbuf.livnmode & 0x3F) != 0x1C)) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: %s: bad livnmode byte 0x%2.2X\n", pn, hoststr, recvbuf.livnmode);
    goto rtn;
  }

  /* Make sure the stratum make sense */

  if ((recvbuf.stratum < 1) || (recvbuf.stratum > 15)) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: %s: bad stratum byte %u\n", pn, hoststr, recvbuf.stratum);
    goto rtn;
  }

  /* Make sure the originatetimestamp matches the request so we know it is for this request */
  /* If so, throw it out and try to read true reply                                         */

  if (memcmp (recvbuf.originatetimestamp, sendbuf.originatetimestamp, 8) != 0) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: %s: mismatch originate timestamps\n", pn, hoststr);
    goto startreceive;
  }

  /* Make sure there are receive and transmit timestamps */

  if ((OZ_IP_N2HL (recvbuf.receivetimestamp) == 0) || (OZ_IP_N2HL (recvbuf.transmittimestamp) == 0)) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: %s: missing receive or transmit timestamp\n", pn, hoststr);
    goto rtn;
  }

  /* Get what time this computer thought it was about when the server processed our request */
  /* Calc it in such a manner so that it can't overflow                                     */

  ntparam -> rtt  = oz_hw_tod_getnow () - ntparam -> cpu;
  ntparam -> cpu += ntparam -> rtt / 2;

  /* Get the midpoint of the server processing our request */

  ntparam -> act  = ntptimetooztime (recvbuf.receivetimestamp);
  ntparam -> act += (ntptimetooztime (recvbuf.transmittimestamp) - ntparam -> act) / 2;

  /* This one was successful */

  ntparam -> ok = 1;

  /* Get rid of everything */

rtn:
  oz_sys_handle_release (OZ_PROCMODE_KNL, h_event);
  oz_sys_handle_release (OZ_PROCMODE_KNL, h_ipstack);
  oz_sys_handle_release (OZ_PROCMODE_KNL, h_timer);
}

/************************************************************************/
/*									*/
/*  OZONE <-> NTP time conversion routines				*/
/*									*/
/************************************************************************/

static void oztimetontptime (OZ_Datebin oztime, uByte ntptime[8])

{
  uLong datelongs[OZ_DATELONG_ELEMENTS], fraction, seconds;

  oztime -= basetime;								// relative to Jan 1, 1900

  seconds  = oztime / OZ_TIMER_RESOLUTION;					// number of seconds since then
  fraction = ((oztime % OZ_TIMER_RESOLUTION) << 32) / OZ_TIMER_RESOLUTION;	// fraction of second since then

  OZ_IP_H2NL (seconds,  ntptime + 0);
  OZ_IP_H2NL (fraction, ntptime + 4);
}

static uQuad ntptimetooztime (uByte ntptime[8])

{
  OZ_Datebin oztime;
  uLong fraction, seconds;

  seconds  = OZ_IP_N2HL (ntptime + 0);
  fraction = OZ_IP_N2HL (ntptime + 4);

  oztime  = basetime;								// start with Jan 1, 1900
  oztime += ((uQuad)seconds) * OZ_TIMER_RESOLUTION;				// add seconds since Jan 1, 1900
  oztime += (((uQuad)fraction) * OZ_TIMER_RESOLUTION + 0x80000000) >> 32;	// add fraction since then

  return (oztime);
}

/************************************************************************/
/*									*/
/*  Kernel data access routines						*/
/*									*/
/************************************************************************/

static uLong getoldfreq (OZ_Procmode cprocmode, void *dummy)

{
  oldfreq = oz_hw_tod_getrate ();
  return (OZ_SUCCESS);
}

static uLong setthisact (OZ_Procmode cprocmode, void *dummy)

{
  oz_hw_tod_setnow (thisact, thiscpu);
  return (OZ_SUCCESS);
}

static uLong updatertc (OZ_Procmode cprocmode, void *dummy)

{
  OZ_Datebin now;

  now = oz_hw_tod_getnow ();
  oz_hw_tod_setnow (now, now);
  return (OZ_SUCCESS);
}

static uLong setnewfreq (OZ_Procmode cprocmode, void *dummy)

{
  if (newfreq == 0) return (OZ_DIVBYZERO);
  oz_hw_tod_setrate (newfreq);
  return (OZ_SUCCESS);
}

static uLong getbootparams (OZ_Procmode cprocmode, void *dummy)

{
  bootparams = oz_s_loadparams;
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Scale a 32-bit unsigned by the quotient of two quads		*/
/*									*/
/*  Computes u * n / d							*/
/*									*/
/************************************************************************/

static uLong scale32x64s64 (uLong u, uQuad n, uQuad d)

{
  int i;
  uLong l;
  uQuad p;

  /* pl = u x n */

#if defined (i386)
  asm ("movl %2,%%eax\n\t"					// %eax = n(lo)
       "mull %3\n\t"						// %edx:%eax = n(lo) * u
       "movl %%eax,%1\n\t"					// l = %eax
       "xorl %%eax,%%eax\n\t"					// p = 0:%edx
       "movl %%edx,%0\n\t"
       "movl %%eax,4+%0\n\t"
       : "=m" (p), "=m" (l) : "m" (n), "m" (u) : "eax", "edx");
  asm ("movl 4+%1,%%eax\n\t"					// %eax = n(hi)
       "mull %2\n\t"						// %edx:%eax = n(hi) * u
       "addl %%eax,%0\n\t"					// p += %edx:%eax
       "adcl %%edx,4+%0\n\t"
       : "+m" (p) : "m" (n), "m" (u) : "eax", "edx");
#else
  p   = (n & 0xFFFFFFFF) * u;					// get 64-bit product of two low-order longs
  l   = p & 0xFFFFFFFF;						// save the low 32-bits
  p >>= 32;							// shift and save high 32-bits
  p  += (n >> 32) * u;						// add in product of high longs
#endif

  /* pl / d = l rem p */

  if (p >= d) oz_sys_condhand_signal (2, OZ_ARITHOVER, 0);	// result must fit in 32 bits

  for (i = 32; -- i >= 0;) {					// generate 32-bit result
    p <<= 1;							// shift high-order numerator
    if (l & 0x80000000) p ++;					// shift low-order numerator
    l <<= 1;
    if (p >= d) {						// compare numerator with divisor
      l ++;							// it fits, set quotient bit
      p -= d;							// ... and subtract divisor from numerator
    }
  }

  /* Perform rounding */

  p <<= 1;
  if (p > d) l ++;
  else if ((p == d) && (l & 1)) l ++;

  /* Return resultant quotient */

  return (l);
}

/************************************************************************/
/*									*/
/*  Scale a 64-bit unsigned by the quotient of two quads		*/
/*									*/
/*  Computes n1 * n2 / d						*/
/*									*/
/************************************************************************/

static uQuad scale64x64s64 (uQuad n1, uQuad n2, uQuad d)

{
  int i, j;
  uQuad q, r;
  uLong w1[2], w2[2], p[4];

  w1[0] = n1;
  w1[1] = n1 >> 32;

  w2[0] = n2;
  w2[1] = n2 >> 32;

  /* Compute p = w1 * w2 */

  memset (p, 0, sizeof p);
  for (i = 0; i < 2; i ++) {
    q = 0;
    for (j = 0; j < 2; j ++) {
      q += ((uQuad)(w1[i])) * w2[j];
      q += p[i+j];
      p[i+j] = q;
      q >>= 32;
    }
    p[i+j] = q;
  }

  /* Compute q = p / d */

  r = (((uQuad)p[3]) << 32) + p[2];
  if (r >= d) oz_sys_condhand_signal (10, OZ_ARITHOVER, 8, r, d, n1, n2);
  q = (((uQuad)p[1]) << 32) + p[0];
  for (i = 64; -- i >= 0;) {
    j = ((r & 0x8000000000000000ULL) != 0);
    r += r;
    if (q & 0x8000000000000000ULL) r |= 1;
    q += q;
    if (j || (r >= d)) {
      r -= d;
      q |= 1;
    }
  }
  if ((r & 0x8000000000000000ULL) || (r + r >= d)) q ++;
  return (q);
}
