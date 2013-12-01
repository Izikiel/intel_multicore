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
/*   DNS ROUTINES							*/
/*									*/
/*  These routines are called by oz_dev_ip driver to process dns 	*/
/*  lookups and save the results in cache pages (which are given up 	*/
/*  if there is a demand for memory)					*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_dev_ipdns.h"
#include "oz_dev_timer.h"
#include "oz_io_ip.h"
#include "oz_knl_cache.h"
#include "oz_knl_devio.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_phymem.h"
#include "oz_knl_printk.h"
#include "oz_knl_sdata.h"
#include "oz_knl_section.h"
#include "oz_knl_status.h"
#include "oz_sys_dateconv.h"

#define IPADDRSIZE OZ_IO_IP_ADDRSIZE
#define PORTSIZE OZ_IO_IP_PORTSIZE

typedef uLong Ipad;
typedef uWord Sock;

#define CLRIPAD(d) *(Ipad *)(d) = 0						/* clear an ip address */
#define ZERIPAD(d) (*(Ipad *)(d) == 0)						/* test ip address for zero */
#define CEQIPAD(x,y) (*(Ipad *)(x) == *(Ipad *)(y))				/* compare ip addresses */
#define CPYIPAD(d,s) *(Ipad *)(d) = *(Ipad *)(s)				/* copy ip address */

#define CLRPORT(d) *(Sock *)(d) = 0						/* clear a port number */
#define CEQPORT(x,y) (*(Sock *)(x) == *(Sock *)(y))				/* compare port numbers */
#define CPYPORT(d,s) *(Sock *)(d) = *(Sock *)(s)				/* copy a port number */

/* DNS macros */

#define DNS_RETRIES 5				/* number of times to try sending the request to the servers */
#define DNS_RETRYINT "1.0"			/* amount of time between retries */
#define DNS_REPLYSIZE 512			/* maximum size of reply message to receive */
#define DNS_MAXNUMEL OZ_IO_IP_DNSNUMMAX		/* maximum number of ip addresses to store for each name */
#define DNS_MAXNAMLEN OZ_IO_IP_DNSNAMMAX	/* maximum name string size to look up */
#define DNS_KEYBITS 10				/* number of bits in the cache key */
						/* - too small and it will try to put too many names in a single page */
						/* - too big and the pages will be sparsely populated */

#define DNS_LOCK do { uLong lvl = oz_hw_smplock_wait (&dns_smplock); if (lvl != OZ_SMPLOCK_SOFTINT) oz_crash ("oz_dev_ip %d: dns locked from %u", __LINE__, lvl); } while (0)
#define DNS_UNLK oz_hw_smplock_clr (&dns_smplock, OZ_SMPLOCK_SOFTINT)

#define GETWORD(__w,__p) do { __w = *(__p ++) << 8; __w |= *(__p ++); } while (0)
#define GETLONG(__l,__p) do { __l = *(__p ++) << 24; __l |= *(__p ++) << 16; __l |= *(__p ++) << 8; __l |= *(__p ++); } while (0)

typedef struct Iopex Iopex;
typedef struct DNS_Pagex DNS_Pagex;
typedef struct DNS_Rp DNS_Rp;
typedef struct DNS_Rplbuf DNS_Rplbuf;

/* Read parameters - there is one of these for each read call in progress */

struct DNS_Rp { DNS_Rp *next;					/* next in rps list */
                Iopex *iopex;					/* corresponding I/O request */
                OZ_Cachepage *page;				/* cache page context where name is/goes */
                OZ_Mempage phypage;				/* ... the associated physical page */

                OZ_Timer *timer;				/* timeout timer pointer (NULL if timer cleaned up) */
                Long retries;					/* number of attempts so far (or < 0 if reply received or timed out) */
                Long transpend;					/* number of transmits pending (0 if transmitter idle) */
                DNS_Rplbuf *rplbuf;				/* pointer to reply message buffer (or NULL if timedout or still in progress) */
                uWord xident;					/* transmitted message's ident */

                uWord reqlen;					/* length of request message */
                uByte reqbuf[1];				/* request message to send to server */
              };

/* Reply buffer - there is one of these for each receive queued to the IP device */

struct DNS_Rplbuf { uLong rlen;					/* length of message received */
                    uByte srcipaddr[OZ_IO_IP_ADDRSIZE];		/* ip address of server that replied */
                    uByte srcportno[OZ_IO_IP_PORTSIZE];		/* port number of server that replied */
                    uByte data[DNS_REPLYSIZE];			/* the message received from the server */
                  };

/* Cache page extension - there is one of these for each page in the cache */

struct DNS_Pagex { uByte initted;				/* 0: page not initialized; 1: page initialized */
                   uByte inuse;					/* 0: page not in use; 1: page in use */
                 };

								/* page data format: */
								/* for each name stored: */
								/*   .asciz "host.name" */
								/*   .long ttl+basetime */
								/*   .byte num_ip_addrs */
								/*   .long ipaddr_1 */
								/*   .long ipaddr_2 */
								/* end of page: */
								/*   .byte 0 */

/* IO operation extension struct */

struct Iopex { Iopex   *next;				/* general purpose 'next iopex' pointer */
               OZ_Ioop *ioop;				/* corresponding ioop pointer */
               union { struct { OZ_IO_ip_dnslookup p; uLong numel; uByte *array; char *name; uByte type; } dnslookup;
                     } u;				/* function dependent data */
             };

/* Static data */

static DNS_Rp *dns_inprog = NULL;		/* list of read param blocks in progress */
static DNS_Rp *dns_locked = NULL;		/* list of read param blocks waiting for cache page to unlock */
static int  dns_mservers = 0;			/* number of nameservers the nsipaddrs and nsportnos arrays can hold */
static int  dns_nservers = 0;			/* number of nameservers in the nsipaddrs and nsportnos arrays */
static Long dns_reads_pending    = 0;		/* number of 'Rp' structs allocated and active */
static Long dns_receives_pending = 0;		/* number of receives pending into Rplbuf structs */
static OZ_Cache *dns_cache = NULL;		/* pointer to cache context */
static OZ_Datebin dns_basetime;			/* date/time cache was initialized */
static OZ_Datebin dns_retryint;			/* retry interval */
static OZ_Smplock dns_smplock;			/* smplock for various lists */
static OZ_Iochan *dns_iochan = NULL;		/* pointer to i/o channel */
static uByte *dns_nsipaddrs = NULL;		/* array of nameserver ip addresses */
static uByte *dns_nsportnos = NULL;		/* array of nameserver udp port numbers */
static uWord dns_lastident = 0;			/* ident number assigned to last request */

/* Internal routines */

static uLong  dns_read (Iopex *iopex);
static void   dns_start_reading (DNS_Rp *rp);
static void   dns_sendrequests (DNS_Rp *rp);
static void   dns_reqtransmitted (void *rpv, uLong status);
static void   dns_timeisup (void *rpv, OZ_Timer *timer);
static void   dns_rplreceived (void *rplbufv, uLong status);
static void   dns_processreply (DNS_Rp *rp);
static void   dns_unlock_page (OZ_Cachepage *page);
static void   dns_sanitise (uByte *cbuff);
static int    dns_scan_page (uByte *cbuff, const char *name, uByte *array, uLong maxel, uLong *numel_r);
static uLong  dns_getnowttl (void);
static char  *dns_ipbintostr (const uByte *ipaddr, char *charbuf);
static uLong  dns_port_ntoh (const uByte *portno);
static uByte *dns_skipnamestring (uByte *p, uByte *endrpl);
static void   dnslookup_done (Iopex *iopex, uLong status);
static void   dnslookup_iofin (void *iopexv, int finok, uLong *status_r);

/************************************************************************/
/*									*/
/*  Initialize								*/
/*									*/
/*    Input:								*/
/*									*/
/*	iopexsize = sizeof ip driver's iopex area it passes us		*/
/*									*/
/************************************************************************/

uLong oz_dev_ipdns_init (int iopexsize)

{
  OZ_IO_ip_udpbind ip_udpbind;
  uLong sts;

  if (iopexsize < sizeof (Iopex)) oz_crash ("oz_dev_ipdns_init: iopex size %u too small, must be at least %u", iopexsize, sizeof (Iopex));

  /* Assign channel to ip device so we can send and receive dns requests and replies */

  sts = oz_knl_iochan_crbynm (OZ_IO_IP_DEV, OZ_LOCKMODE_CW, OZ_PROCMODE_KNL, NULL, &dns_iochan);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_dev_ipdns_init: error %u assigning channel to device '%s'\n", sts, OZ_IO_IP_DEV);
    return (sts);
  }

  /* Bind channel to a socket so we can receive replies */

  memset (&ip_udpbind, 0, sizeof ip_udpbind);
  ip_udpbind.addrsize = OZ_IO_IP_ADDRSIZE;
  ip_udpbind.portsize = OZ_IO_IP_PORTSIZE;
  sts = oz_knl_iostart2 (1, dns_iochan, OZ_PROCMODE_KNL, NULL, NULL, NULL, NULL, NULL, NULL, OZ_IO_IP_UDPBIND, sizeof ip_udpbind, &ip_udpbind);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_dev_ipdns_init: error %u binding to ephemeral socket\n", sts);
    oz_knl_iochan_increfc (dns_iochan, -1);
    return (sts);
  }

  /* Finish up */

  dns_basetime = oz_hw_tod_getnow ();						/* base time for ttl stuff */
  oz_sys_datebin_encstr (strlen (DNS_RETRYINT), DNS_RETRYINT, &dns_retryint);	/* set up the retry interval */
  oz_hw_smplock_init (sizeof dns_smplock, &dns_smplock, OZ_SMPLOCK_LEVEL_VL);	/* init smp lock used to protect reply buffer data */
  dns_cache = oz_knl_cache_init ("ip dns", sizeof (DNS_Pagex), NULL, NULL);	/* create cache context */

  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Abort pending requests						*/
/*									*/
/*    Input:								*/
/*									*/
/*	iochan = i/o channel being aborted				*/
/*	ioop   = request to be aborted					*/
/*	procmode = processor mode doing the abort			*/
/*									*/
/************************************************************************/

void oz_dev_ipdns_abort (OZ_Iochan *iochan, OZ_Ioop *ioop, OZ_Procmode procmode)

{
  DNS_Rp *rp;
  Iopex *iopex, *iopexs;

  iopexs = NULL;
  DNS_LOCK;							/* lock the dns lists */
  for (rp = dns_inprog; rp != NULL; rp = rp -> next) {		/* scan through queue of 'reads in progress' */
    iopex = rp -> iopex;					/* see if it has already been aborted */
    if (iopex == NULL) continue;				/* if so, skip over it */
    if (!oz_knl_ioabortok (iopex -> ioop, iochan, procmode, ioop)) continue; /* see if it is abortable */
    rp -> iopex = NULL;						/* if so, remove link from rp (so it knows the I/O is aborted) */
    iopex -> next = iopexs;					/* put on list of stuff to abort */
    iopexs = iopex;
  }
  for (rp = dns_locked; rp != NULL; rp = rp -> next) {		/* do same for the dns_locked list (requests waiting for locked page) */
    iopex = rp -> iopex;
    if (iopex == NULL) continue;
    if (!oz_knl_ioabortok (iopex -> ioop, iochan, procmode, ioop)) continue;
    rp -> iopex = NULL;
    iopex -> next = iopexs;
    iopexs = iopex;
  }
  DNS_UNLK;							/* unlock dns lists */

  while ((iopex = iopexs) != NULL) {				/* abort all the requests we found */
    iopexs = iopex -> next;
    oz_knl_iodone (iopex -> ioop, OZ_ABORTED, NULL, NULL, NULL);
  }
}

/************************************************************************/
/*									*/
/*  Perform I/O functions						*/
/*									*/
/************************************************************************/

/****************/
/*  Add server  */
/****************/

uLong oz_dev_ipdns_dnssvradd (OZ_Procmode procmode, uLong as, void *ap, OZ_Ioop *ioop)

{
  int i;
  OZ_IO_ip_dnssvradd ip_dnssvradd;
  uByte *ti, *tp;
  uLong sts;

  if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);
  movc4 (as, ap, sizeof ip_dnssvradd, &ip_dnssvradd);				/* get parameter block */
  if (ip_dnssvradd.addrsize != IPADDRSIZE) return (OZ_BADPARAM);		/* get value sizes */
  if (ip_dnssvradd.portsize != PORTSIZE)   return (OZ_BADPARAM);
  sts = oz_knl_ioop_lockr (ioop, IPADDRSIZE, ip_dnssvradd.ipaddr, NULL, NULL, NULL);
  if (sts == OZ_SUCCESS) sts = oz_knl_ioop_lockr (ioop, PORTSIZE, ip_dnssvradd.portno, NULL, NULL, NULL);
  if (sts != OZ_SUCCESS) return (sts);
  DNS_LOCK;									/* lock dns server list */
  for (i = 0; i < dns_nservers; i ++) {						/* see if we can find this server in there already */
    if (CEQIPAD (ip_dnssvradd.ipaddr, dns_nsipaddrs + i * IPADDRSIZE) && CEQPORT (ip_dnssvradd.portno, dns_nsportnos + i * PORTSIZE)) {
      memmove (dns_nsipaddrs + i * IPADDRSIZE, dns_nsipaddrs + (i + 1) * IPADDRSIZE, (dns_nservers - i - 1) * IPADDRSIZE); /* if so, compress it out */
      memmove (dns_nsportnos + i * PORTSIZE,   dns_nsportnos + (i + 1) * PORTSIZE,   (dns_nservers - i - 1) * PORTSIZE);
      dns_nservers --;
      break;
    }
  }
  if (dns_nservers == dns_mservers) {						/* see if we have maxed out on number of servers */
    dns_mservers += 8;								/* if so, allow up to 8 more */
    ti = OZ_KNL_NPPMALLOQ (dns_mservers * IPADDRSIZE);				/* make new ip address array */
    if (ti == NULL) {
      DNS_UNLK;
      return (OZ_EXQUOTANPP);
    }
    tp = OZ_KNL_NPPMALLOQ (dns_mservers * PORTSIZE);				/* make new port number array */
    if (tp == NULL) {
      DNS_UNLK;
      OZ_KNL_NPPFREE (ti);
      return (OZ_EXQUOTANPP);
    }
    memcpy (ti, dns_nsipaddrs, dns_nservers * IPADDRSIZE);			/* copy old ip addresses to it */
    if (dns_nsipaddrs != NULL) OZ_KNL_NPPFREE (dns_nsipaddrs);			/* free off old ip address array */
    dns_nsipaddrs = ti;								/* save pointer to new ip address array */
    memcpy (tp, dns_nsportnos, dns_nservers * PORTSIZE);			/* copy old port numbers to it */
    if (dns_nsportnos != NULL) OZ_KNL_NPPFREE (dns_nsportnos);			/* free off old port number array */
    dns_nsportnos = tp;								/* save pointer to new port number array */
  }
  CPYIPAD (dns_nsipaddrs + dns_nservers * IPADDRSIZE, ip_dnssvradd.ipaddr);	/* anyway, save new server ip address */
  CPYPORT (dns_nsportnos + dns_nservers * PORTSIZE,   ip_dnssvradd.portno);	/* save new server port number */
  dns_nservers ++;								/* increment number of servers */
  DNS_UNLK;									/* unlock server list */
  return (OZ_SUCCESS);
}

/*******************/
/*  Remove server  */
/*******************/

uLong oz_dev_ipdns_dnssvrrem (OZ_Procmode procmode, uLong as, void *ap, OZ_Ioop *ioop)

{
  int i;
  OZ_IO_ip_dnssvrrem ip_dnssvrrem;
  uLong sts;

  if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);
  movc4 (as, ap, sizeof ip_dnssvrrem, &ip_dnssvrrem);			/* get parameter block */
  if (ip_dnssvrrem.addrsize != IPADDRSIZE) return (OZ_BADPARAM);		/* get value sizes */
  if (ip_dnssvrrem.portsize != PORTSIZE)   return (OZ_BADPARAM);
  sts = oz_knl_ioop_lockr (ioop, IPADDRSIZE, ip_dnssvrrem.ipaddr, NULL, NULL, NULL);
  if (sts == OZ_SUCCESS) sts = oz_knl_ioop_lockr (ioop, PORTSIZE, ip_dnssvrrem.portno, NULL, NULL, NULL);
  if (sts != OZ_SUCCESS) return (sts);
  sts = OZ_NOSUCHDNS;							/* assume we won't find it */
  DNS_LOCK;									/* lock dns server list */
  for (i = 0; i < dns_nservers; i ++) {					/* see if we can find this server in there */
    if (CEQIPAD (ip_dnssvrrem.ipaddr, dns_nsipaddrs + i * IPADDRSIZE) && CEQPORT (ip_dnssvrrem.portno, dns_nsportnos + i * PORTSIZE)) {
      memmove (dns_nsipaddrs + i * IPADDRSIZE, dns_nsipaddrs + (i + 1) * IPADDRSIZE, (dns_nservers - i - 1) * IPADDRSIZE); /* if so, compress it out */
      memmove (dns_nsportnos + i * PORTSIZE,   dns_nsportnos + (i + 1) * PORTSIZE,   (dns_nservers - i - 1) * PORTSIZE);
      dns_nservers --;
      sts = OZ_SUCCESS;
      break;
    }
  }
  DNS_UNLK;									/* unlock server list */
  return (sts);									/* return status */
}

/******************/
/*  List servers  */
/******************/

uLong oz_dev_ipdns_dnssvrlist (OZ_Procmode procmode, uLong as, void *ap, OZ_Ioop *ioop)

{
  int i;
  OZ_IO_ip_dnssvrlist ip_dnssvrlist;
  uLong sts;

  movc4 (as, ap, sizeof ip_dnssvrlist, &ip_dnssvrlist);				/* get parameter block */
  if (ip_dnssvrlist.addrsize != IPADDRSIZE) return (OZ_BADPARAM);		/* get value sizes */
  if (ip_dnssvrlist.portsize != PORTSIZE)   return (OZ_BADPARAM);
  sts = oz_knl_ioop_lockw (ioop, IPADDRSIZE, ip_dnssvrlist.ipaddr, NULL, NULL, NULL);
  if (sts == OZ_SUCCESS) sts = oz_knl_ioop_lockw (ioop, PORTSIZE, ip_dnssvrlist.portno, NULL, NULL, NULL);
  if (sts != OZ_SUCCESS) return (sts);
  DNS_LOCK;									/* lock dns server list */
  i = 0;									/* in case ip address starts out at zero */
  if (!ZERIPAD (ip_dnssvrlist.ipaddr)) {					/* see if zero ip address */
    for (i = 0; i < dns_nservers; i ++) {					/* if not, see if we can find this server in there */
      if (CEQIPAD (ip_dnssvrlist.ipaddr, dns_nsipaddrs + i * IPADDRSIZE) && CEQPORT (ip_dnssvrlist.portno, dns_nsportnos + i * PORTSIZE)) {
        i ++;									/* found, return following entry (if any) from table */
        goto dnssvrlist_found;
      }
    }
    DNS_UNLK;									/* couldn't find last server ip address and port in list */
    return (OZ_NOSUCHDNS);
  }
dnssvrlist_found:
  if (i < dns_nservers) {							/* see if we reached the end of list */
    CPYIPAD (ip_dnssvrlist.ipaddr, dns_nsipaddrs + i * IPADDRSIZE);		/* if not, return the server's ip address and port */
    CPYPORT (ip_dnssvrlist.portno, dns_nsportnos + i * PORTSIZE);
  } else {
    CLRIPAD (ip_dnssvrlist.ipaddr);						/* if end, return zeroes */
    CLRPORT (ip_dnssvrlist.portno);
  }
  DNS_UNLK;									/* unlock server list */
  return (OZ_SUCCESS);								/* successful */
}

/********************/
/*  Start a lookup  */
/********************/

uLong oz_dev_ipdns_dnslookup (OZ_Procmode procmode, uLong as, void *ap, OZ_Ioop *ioop, void *iopexv)

{
  char *p;
  Iopex *iopex;
  OZ_Seclock *sectionlock;
  uLong namel, sts;

  iopex = iopexv;
  iopex -> ioop = ioop;

  movc4 (as, ap, sizeof iopex -> u.dnslookup.p, &(iopex -> u.dnslookup.p));

  /* Haven't allocated any temp buffers yet */

  iopex -> u.dnslookup.name  = NULL;
  iopex -> u.dnslookup.array = NULL;

  /* Lock return buffers in memory and make sure caller can write them */

  sts = OZ_BADPARAM;
  if (iopex -> u.dnslookup.p.elsiz != OZ_IO_IP_ADDRSIZE) goto rtnsts;
  sts = oz_knl_ioop_lockw (ioop, iopex -> u.dnslookup.p.maxel * OZ_IO_IP_ADDRSIZE, iopex -> u.dnslookup.p.array, NULL, NULL, NULL);
  if (sts == OZ_SUCCESS) sts = oz_knl_ioop_lockw (ioop, sizeof *(iopex -> u.dnslookup.p.numel_r), iopex -> u.dnslookup.p.numel_r, NULL, NULL, NULL);

  /* Copy name string to a malloc'd buffer in system address space so it will be addressible from any process context */

  if (sts == OZ_SUCCESS) {
    uLong namel;
    OZ_Seclock *sectionlock;

    sts = oz_knl_section_iolockz (procmode, OZ_IO_IP_DNSNAMMAX, iopex -> u.dnslookup.p.name, &namel, &sectionlock, NULL, NULL, NULL);
    if (sts == OZ_SUCCESS) {
      iopex -> u.dnslookup.name = OZ_KNL_NPPMALLOQ (namel + 1);
      if (iopex -> u.dnslookup.name == NULL) sts = OZ_EXQUOTANPP;
      else {
        memcpy (iopex -> u.dnslookup.name, iopex -> u.dnslookup.p.name, namel + 1);
        p = strchr (iopex -> u.dnslookup.name, ':');
        iopex -> u.dnslookup.type = 1;						// default: A (hostname) record
        if (p != NULL) {
          if (strcasecmp (++ p, "a") == 0) iopex -> u.dnslookup.type = 1;	// A (host name) record
          else if (strcasecmp (p, "mx") == 0) iopex -> u.dnslookup.type = 15;	// MX (mail exchanger) record
          else sts = OZ_BADDNSTYPE;
        }
      }
      oz_knl_section_iounlk (sectionlock);
    }
  }

  /* If everything is a go, start looking name up.  Put results in temp array that is addressible from any process context. */

  if (sts == OZ_SUCCESS) {
    iopex -> u.dnslookup.array = OZ_KNL_NPPMALLOQ (iopex -> u.dnslookup.p.maxel * OZ_IO_IP_ADDRSIZE);
    if (iopex -> u.dnslookup.array == NULL) sts = OZ_EXQUOTANPP;
    else sts = dns_read (iopex);
  }

  /* If successful synchronous completion, copy out return values to caller */

  if (sts == OZ_SUCCESS) {
    *(iopex -> u.dnslookup.p.numel_r) = iopex -> u.dnslookup.numel;
    memcpy (iopex -> u.dnslookup.p.array, iopex -> u.dnslookup.array, iopex -> u.dnslookup.numel * OZ_IO_IP_ADDRSIZE);
  }

  /* If any type of synchronous completion, free off temp buffers */

rtnsts:
  if (sts != OZ_STARTED) {
    if (iopex -> u.dnslookup.name  != NULL) OZ_KNL_NPPFREE (iopex -> u.dnslookup.name);
    if (iopex -> u.dnslookup.array != NULL) OZ_KNL_NPPFREE (iopex -> u.dnslookup.array);
  }

  /* Anyway, return status */

  return (sts);
}

/************************************************************************/
/*									*/
/*  Lookup name entry							*/
/*									*/
/*    Input:								*/
/*									*/
/*	iopex = I/O request						*/
/*	      -> u.dnslookup.name = name string				*/
/*	      -> u.dnslookup.p.maxel = max elements to return		*/
/*	      -> u.dnslookup.array = array to return ip's in		*/
/*	      -> u.dnslookup.numel = where to put actual # of elements	*/
/*	smp level = softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	dns_read = OZ_SUCCESS : completed successfully			*/
/*	           OZ_STARTED : will complete async'ly			*/
/*	                 else : error status				*/
/*	*numel_r = number of elements actually returned			*/
/*									*/
/************************************************************************/

static uLong dns_read (Iopex *iopex)

{
  DNS_Pagex *pagex;
  DNS_Rp *rp;
  int i, k;
  OZ_Cachekey key;
  OZ_Cachepage *page;
  OZ_Mempage phypage;
  OZ_Pagentry savepte;
  uByte *cbuff;
  uLong c;

  /* Generate key as a hash of the name.  All names that match this key are put in a single cache page.  Old entries are discarded to make room, if necessary. */

  key = 0;										/* initialize key value */
  k   = 0;										/* initialize key bit counter */
  for (i = 0; (c = iopex -> u.dnslookup.name[i] & 0xff) != 0; i ++) {			/* scan through name string */
    if ((c >= 'A') && (c <= 'Z')) c += 'a' - 'A';					/* convert upper case to lower case */
    key ^= c << k;									/* modify key bits from low-order name char bits */
    k   += 8;										/* increment key bit counter */
    if (k >= DNS_KEYBITS) {								/* check for wrap around */
      k -= DNS_KEYBITS;									/* it wrapped, back it up */
      if (k > 0) key ^= c >> (8 - k);							/* modify key bits from hi-order name char bits */
    }
  }
  if (i >= DNS_MAXNAMLEN) return (OZ_DNSNAMETOOBIG);					/* error if name string too long (we don't want to overflow cache pages with long strings) */
  k &= (1 << DNS_KEYBITS) - 1;								/* mask off extra bits */

  /* Find the page in the cache (create one if none there).  Make sure I'm the only one accessing it because I may move stuff around. */

  page  = oz_knl_cache_find (dns_cache, key, OZ_LOCKMODE_NL, (void **)&pagex, &phypage); /* find the page */
  DNS_LOCK;										/* set smp lock */
  if (pagex -> inuse) {									/* see if anyone else using page now */
    rp = OZ_KNL_NPPMALLOQ (sizeof *rp + strlen (iopex -> u.dnslookup.name) + 20);	/* if so, allocate block plus room for request message */
    if (rp == NULL) {
      DNS_UNLK;
      oz_knl_cache_done (dns_cache, page, OZ_LOCKMODE_NL);
      return (OZ_EXQUOTANPP);
    }
    rp -> iopex      = iopex;								/* save call parameters */
    rp -> page       = page;
    rp -> phypage    = phypage;
    rp -> next       = dns_locked;							/* queue it to page's waiting queue */
    dns_locked = rp;
    DNS_UNLK;										/* release smp lock */
    oz_knl_cache_done (dns_cache, page, OZ_LOCKMODE_NL);				/* the 'dns_locked' queue will keep page locked in memory */
    return (OZ_STARTED);								/* finish it later */
  }
  pagex -> inuse = 1;
  cbuff = oz_hw_phys_mappage (phypage, &savepte);					/* point to cache page */
  if (!(pagex -> initted)) { *cbuff = 0; pagex -> initted = 1; }			/* maybe initialize page */
  DNS_UNLK;										/* release smp lock */

  /* We have the page that should contain the name we want */

  dns_sanitise (cbuff);									/* remove all expired entries */
  if (dns_scan_page (cbuff, iopex -> u.dnslookup.name, iopex -> u.dnslookup.array, iopex -> u.dnslookup.p.maxel, &(iopex -> u.dnslookup.numel))) {
    oz_hw_phys_unmappage (savepte);							/* it was found, unmap from virtual memory */
    dns_unlock_page (page);								/* release cache page */
    return (OZ_SUCCESS);								/* successful */
  }
  oz_hw_phys_unmappage (savepte);							/* not found, unmap from virtual memory */

  /* Not found, set up read parameter block */

  rp = OZ_KNL_NPPMALLOQ (sizeof *rp + strlen (iopex -> u.dnslookup.name) + 20);		/* allocate block plus room for request message */
  if (rp == NULL) return (OZ_EXQUOTANPP);
  rp -> iopex   = iopex;								/* save call parameters */
  rp -> page    = page;
  rp -> phypage = phypage;

  /* Start processing it */

  dns_start_reading (rp);
  return (OZ_STARTED);
}

/************************************************************************/
/*									*/
/*  Start processing read request					*/
/*									*/
/************************************************************************/

static void dns_start_reading (DNS_Rp *rp)

{
  char c;
  const char *cp;
  DNS_Rp *xrp;
  DNS_Rplbuf *rplbuf;
  OZ_IO_ip_udpreceive ip_udpreceive;
  uByte *countp, *p;
  uLong sts;
  uWord xident;

  rp -> retries   = 1;									/* set up retry counter */
  rp -> transpend = 0;									/* no transmits are going */
  rp -> rplbuf    = NULL;								/* no reply has been received yet */

  OZ_HW_ATOMIC_INCBY1_LONG (dns_reads_pending);						/* there is one more read param block floating around now */

  rp -> timer     = oz_knl_timer_alloc ();						/* allocate a timer */
  if (rp -> timer == NULL) {
    dns_rplreceived (rplbuf, OZ_EXQUOTANPP);
    return;
  }

  /* Make sure there are enough receives going to more than cover the requests */

  while (dns_receives_pending <= dns_reads_pending) {
    OZ_HW_ATOMIC_INCBY1_LONG (dns_receives_pending);
    memset (&ip_udpreceive, 0, sizeof ip_udpreceive);
    rplbuf = OZ_KNL_NPPMALLOQ (sizeof *rplbuf);
    if (rplbuf == NULL) sts = OZ_EXQUOTANPP;
    else {
      ip_udpreceive.addrsize  = OZ_IO_IP_ADDRSIZE;
      ip_udpreceive.portsize  = OZ_IO_IP_PORTSIZE;
      ip_udpreceive.rawsize   = sizeof rplbuf -> data;
      ip_udpreceive.rawbuff   = rplbuf -> data;
      ip_udpreceive.rawrlen   = &(rplbuf -> rlen);
      ip_udpreceive.srcipaddr = rplbuf -> srcipaddr;
      ip_udpreceive.srcportno = rplbuf -> srcportno;
      sts = oz_knl_iostart2 (1, dns_iochan, OZ_PROCMODE_KNL, dns_rplreceived, rplbuf, NULL, NULL, NULL, NULL, OZ_IO_IP_UDPRECEIVE, sizeof ip_udpreceive, &ip_udpreceive);
    }
    if (sts != OZ_STARTED) dns_rplreceived (rplbuf, sts);
  }

  /* Allocate an unique ident number and put on rps list */

  DNS_LOCK;
genident:
  while ((xident = ++ dns_lastident) == 0) {}						/* generate an non-zero ident number */
  for (xrp = dns_inprog; xrp != NULL; xrp = xrp -> next) {				/* scan the queue of in-progress read parameters */
    if (xrp -> xident == xident) goto genident;						/* gen again if the new ident number is already in use */
  }
  rp -> xident = xident;								/* save ident word */
  rp -> next   = dns_inprog;								/* link so receiver routine can see it */
  dns_inprog   = rp;
  DNS_UNLK;

  /* Set up request message */

											/** Header section **/
  p = rp -> reqbuf;									/* point to where message goes */
  *(p ++) = xident >> 8; *(p ++) = xident;						/* set up dummy ident word */
  *(p ++) = 0x01;        *(p ++) = 0x00;						/* set up flag word = 0x0100 = recursion desired */
  *(p ++) = 0;           *(p ++) = 1;							/* set up QDCOUNT = 1 (just one query) */
  *(p ++) = 0;           *(p ++) = 0;							/* set up ANCOUNT = 0 (no answers in this message) */
  *(p ++) = 0;           *(p ++) = 0;							/* set up NSCOUNT = 0 */
  *(p ++) = 0;           *(p ++) = 0;							/* set up ARCOUNT = 0 */

											/** Query section **/
  countp = p;										/* host name (baron.nii.net -> <5>baron<3>nii<3>net<0>) */
  *(p ++) = 0;
  for (cp = rp -> iopex -> u.dnslookup.name; (c = *cp) != 0; cp ++) {
    if (c == ':') break;
    if (c != '.') { (*countp) ++; *(p ++) = c; }
    else if (*countp != 0) { countp = p; *(p ++) = 0; }
  }
  if (*countp != 0) *(p ++) = 0;
  *(p ++) = 0; *(p ++) = rp -> iopex -> u.dnslookup.type;				/* type  = 1 (A) or 15 (MX) */
  *(p ++) = 0; *(p ++) = 1;								/* class = 1 (IN) : internet */

  rp -> reqlen = p - rp -> reqbuf;							/* save request message length */

  /* Start sending requests and start timer */

  dns_sendrequests (rp);
}

/************************************************************************/
/*									*/
/*  Start transmitting request message to servers			*/
/*									*/
/*    Input:								*/
/*									*/
/*	rp = read param block						*/
/*									*/
/*    Output:								*/
/*									*/
/*	transmits started						*/
/*	timeout timer started						*/
/*									*/
/************************************************************************/

static void dns_sendrequests (DNS_Rp *rp)

{
  int i;
  OZ_Datebin when;
  OZ_IO_ip_udptransmit ip_udptransmit;
  uLong sts;

  memset (&ip_udptransmit, 0, sizeof ip_udptransmit);					/* set up udp transmit param block */
  ip_udptransmit.addrsize = OZ_IO_IP_ADDRSIZE;
  ip_udptransmit.portsize = OZ_IO_IP_PORTSIZE;
  ip_udptransmit.rawsize  = rp -> reqlen;
  ip_udptransmit.rawbuff  = rp -> reqbuf;

  OZ_HW_ATOMIC_INCBY1_LONG (rp -> transpend);						/* artificially inflate */
											/* so we don't cleanup in middle of loop */

  for (i = 0; (i < rp -> retries) && (i < dns_nservers); i ++) {			/* loop through each defined nameserver */
											/* ... but only try the primary the first time */
											/* ... only try primary and secondary second time */
											/* ... then try the first three, etc. */
    ip_udptransmit.dstipaddr = dns_nsipaddrs + i * OZ_IO_IP_ADDRSIZE;			/* point to its ip address */
    ip_udptransmit.dstportno = dns_nsportnos + i * OZ_IO_IP_PORTSIZE;			/* point to its port number */
    OZ_HW_ATOMIC_INCBY1_LONG (rp -> transpend);						/* one more transmit will be pending */
    sts = oz_knl_iostart2 (1, dns_iochan, OZ_PROCMODE_KNL, dns_reqtransmitted, rp, NULL, NULL, NULL, NULL, /* start sending request */
                           OZ_IO_IP_UDPTRANSMIT, sizeof ip_udptransmit, &ip_udptransmit);
    if (sts != OZ_STARTED) dns_reqtransmitted (rp, sts);				/* maybe it is done now */
  }

  /* Start a timer going so we don't wait forever for a reply */

  when = oz_hw_tod_getnow ();								/* get current time */
  OZ_HW_DATEBIN_ADD (when, dns_retryint, when);						/* this is when we want it to expire */
  oz_knl_timer_insert (rp -> timer, when, dns_timeisup, rp);				/* put request in queue */

  dns_reqtransmitted (rp, OZ_SUCCESS);							/* deflate now that loop is done */
}

/************************************************************************/
/*									*/
/*  An request message has finished transmitting			*/
/*									*/
/*    Input:								*/
/*									*/
/*	rpv = read parameter block					*/
/*	status = transmit status					*/
/*									*/
/************************************************************************/

static void dns_reqtransmitted (void *rpv, uLong status)

{
  DNS_Rp *rp;

  rp = rpv;

  /* Check transmit status and announce any failure (maybe there is no route to the nameserver) */

  if (status != OZ_SUCCESS) {
    oz_knl_printk ("oz_dev_ip dns_reqtransmitted: error %u sending request to nameserver (sorry I am too stupid to know which one)\n", status);
  }

  /* Now check to see if everything has completed.  If so, process the reply. */

  DNS_LOCK;
  if (oz_hw_atomic_inc_long (&(rp -> transpend), -1) == 0) {	/* all request messages must have been transmitted */
    if ((rp -> timer == NULL) && (rp -> retries < 0)) {		/* the timer must be cleaned up and a reply received */
      DNS_UNLK;
      dns_processreply (rp);
      return;
    }
  }

  /* Something is still busy, let it finish everything off */

  DNS_UNLK;
}

/************************************************************************/
/*									*/
/*  Timer has expired							*/
/*									*/
/************************************************************************/

static void dns_timeisup (void *rpv, OZ_Timer *timer)

{
  DNS_Rp **lrp, *rp, *xrp;

  rp = rpv;

  DNS_LOCK;							/* lock database */

  /* If everything else is also done, process the reply message */

  if ((rp -> retries < 0) && (rp -> transpend == 0)) {		/* see if reply message received and transmitter all done */
    oz_knl_timer_free (rp -> timer);				/* if so, free off timer */
    rp -> timer = NULL;
    DNS_UNLK;							/* unlock database */
    dns_processreply (rp);					/* process it */
    return;							/* all done */
  }

  /* If there are retries left, re-send the requests */

  if (rp -> iopex != NULL) {					/* just time it out now if the I/O was aborted */
    if ((++ (rp -> retries) <= DNS_RETRIES) || (rp -> retries <= dns_nservers)) { /* see if any retries left */
      DNS_UNLK;							/* unlock database */
      dns_sendrequests (rp);					/* re-send requests and re-start timer */
      return;							/* all done for now */
    }
  }

  /* No retries left (or I/O was aborted), abort the request */

  for (lrp = &dns_inprog; (xrp = *lrp) != rp; lrp = &(xrp -> next)) {} /* remove rp from in-progress list */
  *lrp = rp -> next;
  oz_knl_timer_free (rp -> timer);				/* free off the timer */
  rp -> timer   = NULL;
  rp -> retries = -2;						/* mark request complete (timedout) */
								/* (rp -> rplbuf should still be NULL) */
  if (rp -> transpend == 0) {					/* see if any transmits still in progress */
    DNS_UNLK;							/* if not, unlock database */
    dns_processreply (rp);					/* and complete the request */
  } else {
    DNS_UNLK;							/* if so, just unlock, let transmit complete routine finish up */
  }
}

/************************************************************************/
/*									*/
/*  An message has been received					*/
/*									*/
/*    Input:								*/
/*									*/
/*	rplbufv  = buffer it was received into				*/
/*	status   = receive I/O status					*/
/*	smplevel = softint						*/
/*									*/
/************************************************************************/

static void dns_rplreceived (void *rplbufv, uLong status)

{
  char ipstr[4*OZ_IO_IP_ADDRSIZE];
  DNS_Rp **lrp, *rp;
  DNS_Rplbuf *rplbuf;
  int i;
  OZ_IO_ip_udpreceive ip_udpreceive;
  uWord rident;

  rplbuf = rplbufv;

  /* Check receive status */

checksts:
  if (status != OZ_SUCCESS) {
    oz_knl_printk ("oz_dev_ip dns_received: error %u receiving reply\n", status);
    goto reread;
  }

  /* Make sure the message came from one of our nameservers */

  for (i = 0; i < dns_nservers; i ++) {
    if (memcmp (rplbuf -> srcipaddr, dns_nsipaddrs + i * OZ_IO_IP_ADDRSIZE, OZ_IO_IP_ADDRSIZE) != 0) continue;
    if (memcmp (rplbuf -> srcportno, dns_nsportnos + i * OZ_IO_IP_PORTSIZE, OZ_IO_IP_PORTSIZE) != 0) continue;
    goto sourceok;
  }
  oz_knl_printk ("oz_dev_ip dns_received: reply received from bogus source %s:%u\n", dns_ipbintostr (rplbuf -> srcipaddr, ipstr), dns_port_ntoh (rplbuf -> srcportno));
  goto reread;
sourceok:

  /* Make sure we are still looking for this particular reply */

  rident = (rplbuf -> data[0] << 8) + rplbuf -> data[1];				/* get the ident of the received message */
  DNS_LOCK;										/* lock database whilst scanning */
  for (lrp = &dns_inprog; (rp = *lrp) != NULL; lrp = &(rp -> next)) {			/* search through the list of in-progress reads */
    if (rp -> xident == rident) goto received;						/* stop if found a matching request */
  }
  DNS_UNLK;										/* unlock database */

  /* The message was useless (a redundant reply to an old request), so just start another receive into same buffer */

reread:
  memset (&ip_udpreceive, 0, sizeof ip_udpreceive);
  ip_udpreceive.addrsize  = OZ_IO_IP_ADDRSIZE;
  ip_udpreceive.portsize  = OZ_IO_IP_PORTSIZE;
  ip_udpreceive.rawsize   = sizeof rplbuf -> data;
  ip_udpreceive.rawbuff   = rplbuf -> data;
  ip_udpreceive.rawrlen   = &(rplbuf -> rlen);
  ip_udpreceive.srcipaddr = rplbuf -> srcipaddr;
  ip_udpreceive.srcportno = rplbuf -> srcportno;

  status = oz_knl_iostart2 (1, dns_iochan, OZ_PROCMODE_KNL, dns_rplreceived, rplbuf, NULL, NULL, NULL, NULL, OZ_IO_IP_UDPRECEIVE, sizeof ip_udpreceive, &ip_udpreceive);
  if (status != OZ_STARTED) goto checksts;
  return;

  /* Received a reply that someone is waiting for */

received:
  OZ_HW_ATOMIC_DECBY1_LONG (dns_receives_pending);					/* one less receive pending */
  *lrp = rp -> next;									/* unlink read params from list */
  rp -> rplbuf  = rplbuf;								/* save pointer to buffer just received */
  rp -> retries = -1;									/* remember that message has been rcvd */
  if (oz_knl_timer_remove (rp -> timer)) {						/* try to remove timer from queue */
    oz_knl_timer_free (rp -> timer);							/* we did, free it off */
    rp -> timer = NULL;									/* ... and remember timer is cleaned up */
  }											/* if we didn't cancel it, it means it is just about to fire off ... */
											/* ... or is already fired off on another cpu */
  if ((rp -> timer == NULL) && (rp -> transpend == 0)) {				/* see if timer and all transmits are finished */
    DNS_UNLK;										/* everything is done, release lock */
    dns_processreply (rp);								/* process reply */
  }
  else DNS_UNLK;									/* something has yet to complete so let it finish up */
}

/************************************************************************/
/*									*/
/*  Process reply message						*/
/*									*/
/************************************************************************/

/* Three things have to have finished before we can process the reply: */
/*  1) A reply has been received (indicated by rp -> retries < 0)      */
/*  2) The timer is cleaned up (indicated by rp -> timer == NULL)      */
/*  3) All requests have finished transmitting (rp -> transpend == 0)  */

static void dns_processreply (DNS_Rp *rp)

{
  char brstr[32], ipstr[4*OZ_IO_IP_ADDRSIZE];
  DNS_Rplbuf *rplbuf;
  int i;
  Iopex *iopex;
  OZ_Pagentry savepte;
  uByte *cb, *cbuff, *endofnextone, *endofusedarea, *endrpl, *p, *q;
  uLong nowttl, numel, sizeneeded, sts, ttl;
  uWord ancount, arcount, class, flags, nscount, qdcount, rdlength, rident, type;

  OZ_HW_ATOMIC_DECBY1_LONG (dns_reads_pending);

  /* If iopex is NULL the I/O request has been aborted */

  DNS_LOCK;
  iopex = rp -> iopex;									/* get iopex pointer */
  rp -> iopex = NULL;									/* clear this so abort routine can't kill it */
  DNS_UNLK;
  if (iopex == NULL) {									/* see if abort routine already got to it */
    sts = OZ_ABORTED;
    goto rtn;
  }

  /* Get corresponding reply buffer.  If null, the operation timed out. */

  rplbuf = rp -> rplbuf;								/* point to reply buffer */
  if (rplbuf == NULL) {									/* maybe it timed out */
    sts = OZ_TIMEDOUT;
    goto rtn;
  }

  /* Copy result into cache page */

  p = rplbuf -> data;									/* point to reply buffer data */
  endrpl = p + rplbuf -> rlen;								/* point to end of reply data */
  GETWORD (rident,  p);									/* skip the ident */
  GETWORD (flags,   p);									/* get the flags */
											/*   <00:03> = rcode */
											/*             0 : no error condition */
											/*             1 : format error (badly formatted message) */
											/*             2 : server failure */
											/*             3 : name does not exist */
											/*             4 : the name server does not support this query type */
											/*             5 : server refused to perform request */
											/*   <04:06> = z (must be zeroes) */
											/*   <07>    = ra (recursion available) */
											/*   <08>    = rd (copy of 'recursion desired' from query) */
											/*   <09>    = tc (set if message was truncated) */
											/*   <10>    = aa (set if 'authoritative answer') */
											/*   <11:14> = opcode */
											/*             0 : standard query */
											/*             1 : an inverse query */
											/*             2 : a server status request */
											/*   <15>    = qr */
											/*             0 : query */
											/*             1 : response */
  if (p > endrpl) {
    strcpy (brstr, "too short for ident & flags");
    goto badreply;
  }
  if ((flags & 15) != 0) {
    sts = OZ_DNSNOSUCHNAME;
    if ((flags & 15) != 3) {
      oz_knl_printk ("oz_dev_ip dns_read_page: dns error %u looking up name '%s'\n", flags & 15, iopex -> u.dnslookup.name);
      sts = OZ_DNSLOOKUPERR;
    }
    goto rtn;
  }
  GETWORD (qdcount, p);									/* get the counts */
  GETWORD (ancount, p);
  GETWORD (nscount, p);
  GETWORD (arcount, p);
  if (p > endrpl) {
    strcpy (brstr, "too short for array counts");
    goto badreply;
  }
  for (i = 0; i < qdcount; i ++) {							/* skip over the qd stuff */
    p = dns_skipnamestring (p, endrpl);
    GETWORD (type, p);
    GETWORD (class, p);
    if (p > endrpl) {
      oz_sys_sprintf (sizeof brstr, brstr, "too short for qd element %d", i);
      goto badreply;
    }
  }
  q = p;										/* save reply buffer pointer */
  numel = 0;										/* start with no elements */
  for (i = 0; i < ancount; i ++) {							/* loop through the answers */
    p = dns_skipnamestring (p, endrpl);							/* we don't care about the name */
    GETWORD (type,  p);									/* get the type word */
    GETWORD (class, p);									/* get the class word */
    GETLONG (ttl,   p);									/* get the time-to-live long */
    GETWORD (rdlength, p);								/* get the length word */
    if (p > endrpl) {
      oz_sys_sprintf (sizeof brstr, brstr, "too short for an element %d", i);
      goto badreply;
    }
    p += rdlength;									/* skip past the string */
    if (p > endrpl) {
      oz_sys_sprintf (sizeof brstr, brstr, "too short for rd length %d", i);
      goto badreply;
    }
    if (type  != 1) continue;								/* skip entry if not type 1 */
    if (class != 1) continue;								/* skip entry if not class 1 */
    if (rdlength != OZ_IO_IP_ADDRSIZE) continue;					/* skip entry if not size 4 */
    numel ++;										/* this entry is good, count it */
  }
  if (numel > DNS_MAXNUMEL) numel = DNS_MAXNUMEL;					/* never do more than this because we use a byte to store it */
											/* and we don't want to hog the whole cache page with one entry */

  sizeneeded = strlen (iopex -> u.dnslookup.name) + 6;					/* size needed includes name, terminating null, time-to-live and byte for numel */
  i = sizeneeded + OZ_IO_IP_ADDRSIZE * numel;						/* this is how big the whole thing will be */
  if (i >= OZ_KNL_CACHE_PAGESIZE) {							/* make sure it doesn't overflow a cache page */
    numel = (OZ_KNL_CACHE_PAGESIZE - 1 - sizeneeded) / OZ_IO_IP_ADDRSIZE;
    if (numel <= 0) {
      oz_knl_printk ("oz_dev_ip dns_read_page: name can't fit in a page '%s'\n", iopex -> u.dnslookup.name);
      sts = OZ_DNSNAMETOOBIG;
      goto rtn;
    }
  }
  sizeneeded += OZ_IO_IP_ADDRSIZE * numel;						/* ok, calculate actual exact size needed */

  cbuff = oz_hw_phys_mappage (rp -> phypage, &savepte);					/* point to cache block buffer */

  for (endofusedarea = cbuff; *endofusedarea != 0; endofusedarea = endofnextone) {	/* find end of used area */
    endofnextone  = endofusedarea + strlen (endofusedarea) + 1;
    i = *(endofnextone ++);
    endofnextone += i * OZ_IO_IP_ADDRSIZE;
    if (endofnextone + sizeneeded >= cb + OZ_KNL_CACHE_PAGESIZE) break;			/* but stop if it would overflow with new one */
  }
  memmove (cbuff + sizeneeded, cbuff, endofusedarea - cbuff);				/* make room for new entry at beg, maybe trashing old entry(s) at end */
  endofusedarea[sizeneeded] = 0;

  strcpy (cbuff, iopex -> u.dnslookup.name);						/* copy in new name at beginning */
  cb = cbuff + strlen (cbuff) + 1;

  nowttl = dns_getnowttl ();

  for (i = 0; i < ancount; i ++) {							/* loop through the answers */
    q = dns_skipnamestring (q, endrpl);							/* we don't care about the name */
    GETWORD (type,  q);									/* get the type word */
    GETWORD (class, q);									/* get the class word */
    GETLONG (ttl,   q);									/* get the time-to-live long */
    GETWORD (rdlength, q);								/* get the length word */
    q += rdlength;									/* skip past the string */
    if (type  != 1) continue;								/* skip entry if not type 1 */
    if (class != 1) continue;								/* skip entry if not class 1 */
    if (rdlength != OZ_IO_IP_ADDRSIZE) continue;					/* skip entry if not size 4 */
    if (nowttl != 0) {									/* ok, see if ttl/numel needs to be stored */
      nowttl  += ttl;									/* if so, calc actual ttl expiration */
      *(cb ++) = nowttl >> 24;								/* store in cache page */
      *(cb ++) = nowttl >> 16;
      *(cb ++) = nowttl >>  8;
      *(cb ++) = nowttl;
      nowttl   = 0;									/* only do it once per entry */
      *(cb ++) = numel;									/* store number of elements */
    }
    memcpy (cb, q - rdlength, OZ_IO_IP_ADDRSIZE);					/* this entry is good, copy it */
    cb += OZ_IO_IP_ADDRSIZE;
    if (-- numel == 0) break;								/* done if that's all we can store */
  }

  /* Copy entry to caller's buffer - do not sanitise first so a ttl=0 entry will survive */

  if (!dns_scan_page (cbuff, iopex -> u.dnslookup.name, iopex -> u.dnslookup.array, iopex -> u.dnslookup.p.maxel, &(iopex -> u.dnslookup.numel))) {
    oz_crash ("oz_dev_ip dns_read_page: entry not found after insert");
  }
  oz_hw_phys_unmappage (savepte);
  sts = OZ_SUCCESS;
  goto rtn;

badreply:
  oz_knl_printk ("oz_dev_ip dns_received: bad reply received from nameserver %s:%u - %s\n", dns_ipbintostr (rplbuf -> srcipaddr, ipstr), dns_port_ntoh (rplbuf -> srcportno));
  oz_knl_printk ("                      : %s\n", brstr);
  oz_knl_dumpmem (rplbuf -> rlen, rplbuf -> data);
  sts = OZ_DNSREPLYBAD;

  /* Free off reply buffer */

rtn:
  if (rp -> rplbuf != NULL) OZ_KNL_NPPFREE (rp -> rplbuf);

  /* Unlock cache page and call completion routine */

  dns_unlock_page (rp -> page);
  if (iopex != NULL) dnslookup_done (iopex, sts);
  OZ_KNL_NPPFREE (rp);
}

/************************************************************************/
/*									*/
/*  All done processing request, unlock cache page so another request 	*/
/*  may proceed								*/
/*									*/
/************************************************************************/

static void dns_unlock_page (OZ_Cachepage *page)

{
  uByte *cbuff;
  DNS_Pagex *pagex;
  DNS_Rp **lrp, *rp;
  int found;
  Iopex *iopex;
  OZ_Mempage phypage;
  OZ_Pagentry savepte;

  pagex = oz_knl_cache_pagex (page);				/* point to page extension area */

  /* If there is no request in 'dns_locked' waiting for this page, unlock cache page and return */

unlock:
  DNS_LOCK;							/* lock database */
  lrp = &dns_locked;						/* scan list of requests waiting for locked pages */
unlock2:
  for (; (rp = *lrp) != NULL; lrp = &(rp -> next)) {
    if (rp -> page == page) break;				/* see if anything waiting for this page to be free */
  }
  if (rp == NULL) {
    pagex -> inuse = 0;						/* if not, clear 'inuse' flag */
    DNS_UNLK;							/* unlock database */
    oz_knl_cache_done (dns_cache, page, OZ_LOCKMODE_NL);	/* release page, maybe it gets unloaded from memory */
    return;
  }

  /* There is a request waiting for the page, unlink and start processing it (leave cache page locked in memory) */

  *lrp  = rp -> next;						/* if so unlink it */
  iopex = rp -> iopex;						/* see if there is an I/O request still associated with it */
  if (iopex == NULL) {						/* maybe the I/O request was cancelled */
    OZ_KNL_NPPFREE (rp);					/* if so, free read param block */
    goto unlock2;						/* go see if there are any more to process */
  }
  DNS_UNLK;							/* unlock database */

  /* The entry might be in the page now, so find out */

  phypage = oz_knl_cache_phypage (page);			/* map cache page to virtual memory */
  cbuff   = oz_hw_phys_mappage (phypage, &savepte);
  dns_sanitise (cbuff);						/* clean out expired entries */
  found   = dns_scan_page (cbuff, iopex -> u.dnslookup.name, iopex -> u.dnslookup.array, iopex -> u.dnslookup.p.maxel, &(iopex -> u.dnslookup.numel));
  oz_hw_phys_unmappage (savepte);				/* unmap cache page from virtual memory */

  /* If not, go start sending requests to servers, etc... */

  if (!found) {
    dns_start_reading (rp);
    return;
  }

  /* Otherwise, complete the request as is then check for other locked-out requests on this page */

  OZ_KNL_NPPFREE (rp);						/* free param block */
  dnslookup_done (iopex, OZ_SUCCESS);				/* call completion routine */
  goto unlock;							/* try to unlock again */
}

/************************************************************************/
/*									*/
/*  Sanitise page, ie, remove all expired entries			*/
/*									*/
/************************************************************************/

static void dns_sanitise (uByte *cbuff)

{
  uByte *cb1, *cb2, *entend;
  uLong numel, now, ttl;

  now = dns_getnowttl ();					/* get current date/time, ttl style */

  cb2 = cbuff;							/* cb2 is where we put stuff that survives */
  for (cb1 = cbuff; *cb1 != 0; cb1 = entend) {			/* loop through cache buffer page */
    entend = cb1 + strlen (cb1) + 1;				/* point past null of the name */
    GETLONG (ttl, entend);					/* get the time it expires */
    numel   = *(entend ++);					/* point to end of entry = beg of next entry */
    entend += numel * OZ_IO_IP_ADDRSIZE;
    if (ttl >= now) {						/* see if we keep it */
      if (cb1 != cb2) memmove (cb2, cb1, entend - cb1);		/* if so, move the entry */
      cb2 += entend - cb1;					/* ... and move the pointer */
    }
  }
  *cb2 = 0;							/* end the page here */
}

/************************************************************************/
/*									*/
/*  Scan a cache page for a given name and return data.  If entry 	*/
/*  found, make it first in page so it will be last to get pushed out.	*/
/*									*/
/************************************************************************/

static int dns_scan_page (uByte *cbuff, const char *name, uByte *array, uLong maxel, uLong *numel_r)

{
  uByte *cb, *entbeg, *entend;
  uLong numel;

  /* Scan page for matching entry */

  for (cb = cbuff; *cb != 0; cb += numel * OZ_IO_IP_ADDRSIZE) {	/* loop through cache buffer page */
    if (strcasecmp (name, cb) == 0) goto found;			/* see if name string matches */
    cb   += strlen (cb) + 5;					/* name doesn't match, point to number of elements */
    numel = *(cb ++);						/* get number of array elements */
  }
  return (0);							/* return failure */

  /* Found entry, copy values to caller's buffer */

found:
  entbeg = cb;							/* save beginning of entry */
  cb    += strlen (cb) + 5;					/* if so, point to number of elements */
  numel  = *(cb ++);						/* get the number of elements defined for the name */
  entend = cb + numel * OZ_IO_IP_ADDRSIZE;			/* point to end of the entry */
  if (numel > maxel) numel = maxel;				/* don't return more elements than caller wants */
  *numel_r = numel;						/* tell caller how many elements we are returning */
  memcpy (array, cb, numel * OZ_IO_IP_ADDRSIZE);		/* copy data to caller's buffer */

  /* Make this entry the first entry on the page (if not already) */

  if (entbeg != cbuff) {					/* see if entry is not already at the beginning */
    numel = entend - entbeg;					/* it's not, get number of bytes in entry */
    cb    = OZ_KNL_NPPMALLOC (numel);				/* allocate a temp buffer that will hold it */
    memcpy  (cb, entbeg, numel);				/* copy entry to temp buffer */
    memmove (cbuff + numel, cbuff, entbeg - cbuff);		/* move everything in page that was before it down */
    memcpy  (cbuff, cb, numel);					/* copy entry to beginning of page */
    OZ_KNL_NPPFREE (cb);					/* free off the temp page */
  }

  /* Successful */

  return (1);
}

/************************************************************************/
/*									*/
/*  Misc utility routines						*/
/*									*/
/************************************************************************/

/* Get current time in ttl format (number of seconds since startup + 1) */

static uLong dns_getnowttl (void)

{
  OZ_Datebin now;

  now  = oz_hw_tod_getnow ();			/* get current date/time */
  now -= dns_basetime;				/* subtract base date/time */
  now /= OZ_TIMER_RESOLUTION;			/* convert to number of seconds */
  return (now + 1);				/* return +1 (so we never return a zero value) */
}

/* Conversion of ip address from binary to string */

static char *dns_ipbintostr (const uByte *ipaddr, char *charbuf)

{
  char *p;
  int i;

  p = charbuf;
  for (i = 0; i < OZ_IO_IP_ADDRSIZE; i ++) {
    if (i != 0) *(p ++) = '.';
    oz_hw_itoa (ipaddr[i], 4, p);
    p += strlen (p);
  }
  return (charbuf);
}

/* Conversion of port number to binary */

static uLong dns_port_ntoh (const uByte *portno)

{
  int i;
  uLong p;

  p = 0;
  for (i = 0; i < OZ_IO_IP_PORTSIZE; i ++) p = (p << 8) + portno[i];
  return (p);
}

/* Skip over a name string */

static uByte *dns_skipnamestring (uByte *p, uByte *endrpl)

{
  uLong n;

  n = *(p ++);					/* get number of chars in string */
  if ((n & 0xc0) == 0xc0) return (p + 1);	/* if top 2 bits set, it means 2-byte offset to string, so just skip second byte */
  while (n != 0) {				/* as long as count is non-zero */
    p += n;					/* ... point to next count */
    if (p > endrpl) break;			/* ... stop if bad pointer */
    n  = *(p ++);				/* ... ok, get next count */
  }
  return (p);					/* return just past null */
}

/************************************************************************/
/*									*/
/*  Lookup request completion routines					*/
/*									*/
/************************************************************************/

/* This routine is called by the ncache_read routine when it has copied the results into temp buffers */
/* It is at softint level, but who knows what process we are in!                                      */

static void dnslookup_done (Iopex *iopex, uLong status)

{
  oz_knl_iodone (iopex -> ioop, status, NULL, dnslookup_iofin, iopex);
}

/* Now we are back in requestor's process space so we can copy temp buffers to requestor's buffers */

static void dnslookup_iofin (void *iopexv, int finok, uLong *status_r)

{
  Iopex *iopex;

  iopex = iopexv;

  /* If successful completion, return values to caller's buffer */

  if (finok && (*status_r == OZ_SUCCESS)) {
    *(iopex -> u.dnslookup.p.numel_r) = iopex -> u.dnslookup.numel;
    memcpy (iopex -> u.dnslookup.p.array, iopex -> u.dnslookup.array, iopex -> u.dnslookup.numel * OZ_IO_IP_ADDRSIZE);
  }

  /* Anyway, free off temp buffers */

  OZ_KNL_NPPFREE (iopex -> u.dnslookup.name);
  OZ_KNL_NPPFREE (iopex -> u.dnslookup.array);
}
