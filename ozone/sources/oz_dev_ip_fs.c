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
/*  This driver is used to boot the system over an IP connection	*/
/*									*/
/*  It uses a protocol similar to TFTP but allows for random block 	*/
/*  numbers.								*/
/*									*/
/*  It calls the ip driver to perform the network I/O.			*/
/*									*/
/*  The mount string is:						*/
/*									*/
/*	mount <see_below> oz_ip_fs					*/
/*									*/
/*      [etherdev/hwipaddr/[nwipaddr]/nwipmask/[gwipaddr]/]serverip,port
/*									*/
/*	etherdev = hardware device name, eg, dec21041_0_11		*/
/*	hwipaddr = ip address of etherdev, eg, 209.113.172.94		*/
/*	nwipaddr = ip address of network, if different			*/
/*	nwipmask = ip mask of etherdev, eg, 255.255.255.192		*/
/*	gwipaddr = ip address of gateway to server, or omit if none	*/
/*	serverip = ip address of boot server, eg, 209.113.172.16	*/
/*	port     = udp port number of boot server, eg, 2290		*/
/*									*/
/*  On mount, this driver will attempt to add the etherdev, 		*/
/*  hwipaddr/nwipaddr/nwipmask and gwipaddr to the ip driver's 		*/
/*  database before attempting to connect to the server.		*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_dev_ip_fs.h"
#include "oz_dev_timer.h"
#include "oz_io_fs.h"
#include "oz_io_ip.h"
#include "oz_knl_devio.h"
#include "oz_knl_event.h"
#include "oz_knl_hw.h"
#include "oz_knl_procmode.h"
#include "oz_knl_sdata.h"
#include "oz_knl_status.h"
#include "oz_sys_dateconv.h"
#include "oz_sys_xprintf.h"

#define DISK_BLOCK_SIZE 512

#define DRIVERNAME "oz_ip_fs"
#define DEVICENAME "oz_ip_fs"

typedef struct Devex Devex;
typedef struct Chnex Chnex;
typedef struct Iopex Iopex;

/* Device extension structure */

struct Devex { OZ_Smplock smplock_vl;		/* smp lock */
               Long iocount;			/* number of I/O requests in progress (don't dismount if non-zero) */
               OZ_Iochan *ipchan;		/* ip device I/O channel (NULL if not mounted) */
               Long seq;			/* request/reply sequence number */
               uByte serveripaddr[OZ_IO_IP_ADDRSIZE]; /* server's ip address */
               uByte serverportno[OZ_IO_IP_PORTSIZE]; /* server's port number */
             };

/* Channel extension structure */

struct Chnex { Iopex *iopexs;			/* list of I/O requests */
               uLong handle;			/* server defined handle for open file */
               int ignclose;			/* ignore close calls */
             };

/* I/O Operation extension structure */

struct Iopex { OZ_Ioop *ioop;			/* i/o operation struct pointer */
               Devex *devex;			/* corresponding devex struct pointer */
               Chnex *chnex;			/* corresponding chnex struct pointer */
               OZ_Procmode procmode;		/* requestor's processor mode */
               uLong funcode;			/* i/o function code */
               union { struct { OZ_IO_fs_mountvol p; OZ_Devunit *devunit; } mountvol;
                       struct { OZ_IO_fs_dismount p; } dismount;
                       struct { OZ_IO_fs_create p; } create;
                       struct { OZ_IO_fs_open p; } open;
                       struct { OZ_IO_fs_enter p; } enter;
                       struct { OZ_IO_fs_remove p; } remove;
                       struct { OZ_IO_fs_rename p; } rename;
                       struct { OZ_IO_fs_extend p; } extend;
                       struct { OZ_IO_fs_writeblocks p; } writeblocks;
                       struct { OZ_IO_fs_readblocks p; 
                                const OZ_Mempage *buff_pp; uLong buff_bo;
                              } readblocks;
                       struct { OZ_IO_fs_writerec p; } writerec;
                       struct { OZ_IO_fs_readrec p; 
                                const OZ_Mempage *buff_pp; uLong buff_bo;
                                const OZ_Mempage *rlen_pp; uLong rlen_bo;
                              } readrec;
                       struct { OZ_IO_fs_pagewrite p; } pagewrite;
                       struct { OZ_IO_fs_pageread p; } pageread;
                       struct { OZ_IO_fs_getinfo1 p; uLong as; const OZ_Mempage *buff_pp; uLong buff_bo; } getinfo1;
                       struct { OZ_IO_fs_readdir p; const OZ_Mempage *filenambuff_pp; uLong filenambuff_bo; } readdir;
                       struct { OZ_IO_fs_writeboot p; } writeboot;
                       struct { OZ_IO_fs_wildscan p; const OZ_Mempage *buff_pp; uLong buff_bo; } wildscan;
                       struct { OZ_IO_fs_getinfo2 p; } getinfo2;
                       struct { OZ_IO_fs_setcurpos p; } setcurpos;
                     } u;

               Iopex *next;			/* next in chnex -> iopexs */
               Iopex **prev;			/* prev in chnex -> iopexs */
               Long refcount;			/* ref count (ast's, etc) */
               uLong status;			/* completion status */
               OZ_Timer *timer;			/* timer struct pointer */
               int retries;			/* retries counter */

               OZ_Ip_fs_req req;		/* request message */
               OZ_Ip_fs_rpl rpl;		/* reply message */
             };

/* Function table */

static uLong ip_fs_clonecre (OZ_Devunit *template_devunit, void *template_devex, int template_cloned, OZ_Procmode procmode, OZ_Devunit **cloned_devunit);
static int ip_fs_clonedel (OZ_Devunit *cloned_devunit, void *devexv, int cloned);
static uLong ip_fs_assign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode);
static int ip_fs_deassign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv);
static void ip_fs_abort (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Ioop *ioop, void *iopexv, OZ_Procmode procmode);
static uLong ip_fs_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode, 
                               OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap);

static const OZ_Devfunc ip_fs_functable = { sizeof (Devex), sizeof (Chnex), sizeof (Iopex), 0, NULL, 
                                            ip_fs_clonecre, ip_fs_clonedel, 
                                            ip_fs_assign, ip_fs_deassign, 
                                            ip_fs_abort, ip_fs_start, NULL };

/* Driver static data */

static int initialized = 0;
static uLong clonumber = 0;
static OZ_Devclass *devclass;
static OZ_Devdriver *devdriver;
static OZ_Devunit *devunit;
static OZ_Datebin retryinterval;

/* Internal routines */

static uLong performreq (Iopex *iopex, int repeat);
static void performsend (Iopex *iopex);
static void performrcv (Iopex *iopex);
static void sendcomplete (void *iopexv, uLong status);
static void timedout (void *iopexv, OZ_Timer *timer);
static void receivecomplete (void *iopexv, uLong status);
static int setiopexsts (Iopex *iopex, uLong sts);
static void deciopexrefc (Iopex *iopex);
static int getipaddr (const char *ipstr, uByte ipbin[OZ_IO_IP_ADDRSIZE], const char **p_r, char term);
static char *cvtipbin (uByte ipbin[OZ_IO_IP_ADDRSIZE], char ipstr[OZ_IO_IP_ADDRSIZE*4]);

/************************************************************************/
/*									*/
/*  Boot-time initialization routine					*/
/*									*/
/************************************************************************/

void oz_dev_ip_fs_init ()

{
  Devex *devex;
  uLong datelongs[OZ_DATELONG_ELEMENTS];

  if (!initialized) {
    oz_knl_printk ("oz_dev_ip_fs_init\n");
    initialized = 1;

    /* Set up template device data structures */

    devclass  = oz_knl_devclass_create (OZ_IO_FS_CLASSNAME, OZ_IO_FS_BASE, OZ_IO_FS_MASK, DRIVERNAME);
    devdriver = oz_knl_devdriver_create (devclass, DRIVERNAME);
    devunit   = oz_knl_devunit_create (devdriver, DEVICENAME, "mount [etherdev/hwip/[nwip]/nwmask/[gwip]/]serverip,port", &ip_fs_functable, 0, oz_s_secattr_tempdev);
    devex     = oz_knl_devunit_ex (devunit);
    memset (devex, 0, sizeof *devex);

    /* Calculate retry interval = 0.25 sec */

    memset (datelongs, 0, sizeof datelongs);
    datelongs[OZ_DATELONG_FRACTION] = OZ_TIMER_RESOLUTION / 4;
    retryinterval = oz_sys_datebin_encode (datelongs);
  }
}

/************************************************************************/
/*									*/
/*  An I/O channel is being assigned and the devio routines want to 	*/
/*  know if this device is to be cloned.				*/
/*									*/
/*  In this driver, we only clone the original template device.		*/
/*									*/
/*    Input:								*/
/*									*/
/*	template_devunit = pointer to existing device unit		*/
/*	template_devex   = device extension data			*/
/*	template_cloned  = template's cloned flag			*/
/*	procmode         = processor mode doing the assign		*/
/*									*/
/*    Output:								*/
/*									*/
/*	ip_fs_clonecre = OZ_SUCCESS : ok to assign channel		*/
/*	                       else : error status			*/
/*	**cloned_devunit = cloned device unit				*/
/*									*/
/************************************************************************/

static uLong ip_fs_clonecre (OZ_Devunit *template_devunit, void *template_devex, int template_cloned, OZ_Procmode procmode, OZ_Devunit **cloned_devunit)

{
  char unitname[12+sizeof DEVICENAME];
  Devex *devex;
  int i;
  OZ_Secattr *secattr;

  /* If this is an already cloned devunit, don't clone anymore, just use the original device */

  if (template_cloned) {
    *cloned_devunit = template_devunit;
    oz_knl_devunit_increfc (template_devunit, 1);
  }

  /* This is the original template device, clone a unit.  The next thing the caller should do is an OZ_IO_FS_MOUNTVOL call. */

  else {
    strcpy (unitname, DEVICENAME);
    strcat (unitname, "_");
    i = strlen (unitname);
    oz_hw_itoa (++ clonumber, sizeof unitname - i, unitname + i);

    secattr = oz_knl_thread_getdefcresecattr (NULL);
    *cloned_devunit = oz_knl_devunit_create (devdriver, unitname, "Not yet mounted", &ip_fs_functable, 1, secattr);
    if (secattr != NULL) oz_knl_secattr_increfc (secattr, -1);
    devex = oz_knl_devunit_ex (*cloned_devunit);
    memset (devex, 0, sizeof *devex);
    oz_hw_smplock_init (sizeof devex -> smplock_vl, &(devex -> smplock_vl), OZ_SMPLOCK_LEVEL_VL);
  }

  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  The last channel was deassigned from a devunit.  This routine is 	*/
/*  called to see if the unit should be deleted.			*/
/*									*/
/*  In this driver, we only delete devices that are not mounted.  We 	*/
/*  also never delete the template device (duh!).			*/
/*									*/
/*    Input:								*/
/*									*/
/*	cloned_devunit = cloned device's devunit struct			*/
/*	devex = the devex of cloned_devunit				*/
/*	cloned = the cloned_devunit's cloned flag			*/
/*									*/
/*    Output:								*/
/*									*/
/*	ip_fs_clonedel = 0 : keep device in device table		*/
/*	                 1 : delete device from table			*/
/*									*/
/************************************************************************/

static int ip_fs_clonedel (OZ_Devunit *cloned_devunit, void *devexv, int cloned)

{
  Devex *devex;

  devex = devexv;

  return (cloned && (devex -> ipchan == NULL));
}

/************************************************************************/
/*									*/
/*  An I/O channel was just assigned to the unit			*/
/*									*/
/*  Clear out the channel extension block				*/
/*									*/
/************************************************************************/

static uLong ip_fs_assign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode)

{
  Chnex *chnex;

  chnex = chnexv;
  memset (chnex, 0, sizeof *chnex);
//  oz_hwaxp_watch_set (4, sizeof chnex -> iopexs, &(chnex -> iopexs));
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  An I/O channel was just deassigned from a unit			*/
/*									*/
/*    Input:								*/
/*									*/
/*	devunit = device unit that is being deassigned from		*/
/*	devexv  = corresponding devex pointer				*/
/*	iochan  = i/o channel being deassigned				*/
/*	chnexv  = corresponding chnex pointer				*/
/*									*/
/************************************************************************/

static int ip_fs_deassign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv)

{
  Chnex *chnex;
  uLong sts;

  chnex = chnexv;

  if ((chnex -> handle != 0) || (chnex -> iopexs != NULL)) {
    chnex -> ignclose = 0;
    sts = oz_knl_iostart2 (1, iochan, OZ_PROCMODE_KNL, NULL, NULL, NULL, NULL, NULL, NULL, OZ_IO_FS_CLOSE, 0, NULL);
    if (sts == OZ_STARTED) return (1);
    if ((chnex -> handle != 0) || (chnex -> iopexs != NULL)) oz_crash ("oz_dev_ip_fs deassign: channel still open after close (sts %u)", sts);
  }
//  oz_hwaxp_watch_clr (4, sizeof chnex -> iopexs, &(chnex -> iopexs));

  return (0);
}

/************************************************************************/
/*									*/
/*  Abort I/O request in progress					*/
/*									*/
/************************************************************************/

static void ip_fs_abort (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Ioop *ioop, void *iopexv, OZ_Procmode procmode)

{
  Chnex *chnex;
  Devex *devex;
  int foundone;
  Iopex *iopex;
  uLong vl;

  chnex = chnexv;
  devex = devexv;

  /* Increment I/O count on devex so the ipchan won't go away */

  vl = oz_hw_smplock_wait (&(devex -> smplock_vl));
  if (devex -> ipchan == NULL) {
    oz_hw_smplock_clr (&(devex -> smplock_vl), vl);
    return;
  }
  devex -> iocount ++;

  /* Loop through list of all I/O's and mark appropriate ones as aborted */

  for (iopex = chnex -> iopexs; iopex != NULL; iopex = iopex -> next) {
    if (oz_knl_ioabortok (iopex -> ioop, iochan, procmode, ioop)) {
      foundone = 1;
      setiopexsts (iopex, OZ_ABORTED);
    }
  }
  oz_hw_smplock_clr (&(devex -> smplock_vl), vl);

  /* If we marked any as aborted, abort any ip I/O's so they will really abort */

  if (foundone) oz_knl_ioabort (devex -> ipchan, OZ_PROCMODE_KNL);

  /* All done with devex -> ipchan */

  vl = oz_hw_smplock_wait (&(devex -> smplock_vl));
  devex -> iocount --;
  oz_hw_smplock_clr (&(devex -> smplock_vl), vl);
}

/************************************************************************/
/*									*/
/*  This routine is called as a result of an oz_knl_iostart call to 	*/
/*  start performing an i/o request					*/
/*									*/
/************************************************************************/

static uLong ip_fs_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode,
                          OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap)

{
  uByte gwipaddr[OZ_IO_IP_ADDRSIZE], hwipaddr[OZ_IO_IP_ADDRSIZE], nwipaddr[OZ_IO_IP_ADDRSIZE], nwipmask[OZ_IO_IP_ADDRSIZE];
  uByte ipstr[OZ_IO_IP_ADDRSIZE*4], nullipad[OZ_IO_IP_ADDRSIZE];
  uByte serveripaddr[OZ_IO_IP_ADDRSIZE], serverportno[OZ_IO_IP_PORTSIZE];
  char unitname[OZ_DEVUNIT_NAMESIZE];
  const char *p;
  Chnex *chnex;
  Devex *devex;
  int gwpresent, i, usedup;
  Iopex *iopex;
  uLong sts, vl;
  OZ_IO_ip_hwadd ip_hwadd;
  OZ_IO_ip_hwipamadd ip_hwipamadd;
  OZ_IO_ip_routeadd ip_routeadd;
  OZ_IO_ip_udpbind ip_udpbind;
  OZ_Iochan *ipchan;

  chnex = chnexv;
  devex = devexv;
  iopex = iopexv;

  /* Set up stuff in iopex that is common to just about all functions */

  iopex -> ioop     = ioop;
  iopex -> funcode  = funcode;
  iopex -> chnex    = chnex;
  iopex -> devex    = devex;
  iopex -> procmode = procmode;

  movc4 (as, ap, sizeof iopex -> u, &(iopex -> u));
  memset (&(iopex -> req), 0x96, sizeof iopex -> req);
  iopex -> req.func   = funcode;
  iopex -> req.handle = chnex -> handle;

  switch (funcode) {

    /* Mount - sets up IP configuration parameters */

    case OZ_IO_FS_MOUNTVOL: {

      p = iopex -> u.mountvol.p.devname;
      unitname[0] = 0;
      if (strchr (p, '/') != NULL) {

        /* Add the device name to ip */

        for (i = 0; p[i] != '/'; i ++) {
          if (i >= sizeof unitname - 1) {
            oz_knl_printk ("oz_dev_ip_fs: missing / after etherdevice or too long\n");
            return (OZ_BADPARAM);
          }
          unitname[i] = p[i];
        }
        unitname[i++] = 0;
        p += i;

        /* Add IP address and mask to ip definition of device */

        if (!getipaddr (p, hwipaddr, &p, '/')) {
          oz_knl_printk ("oz_dev_ip_fs: bad ip address %s\n", p);
          return (OZ_BADPARAM);
        }
        if (*p == '/') {
          memcpy (nwipaddr, hwipaddr, OZ_IO_IP_ADDRSIZE);
          p ++;
        } else if (!getipaddr (p, nwipaddr, &p, '/')) {
          oz_knl_printk ("oz_dev_ip_fs: bad ip address %s\n", p);
          return (OZ_BADPARAM);
        }
        if (!getipaddr (p, nwipmask, &p, '/')) {
          oz_knl_printk ("oz_dev_ip_fs: bad ip mask %s\n", p);
          return (OZ_BADPARAM);
        }

        /* Add optional gateway definition */

        gwpresent = 0;
        if (*p == '/') p ++;
        else {
          if (!getipaddr (p, gwipaddr, &p, '/')) {
            oz_knl_printk ("oz_dev_ip_fs: bad default gateway %s\n", p);
            return (OZ_BADPARAM);
          }
          gwpresent = 1;
        }
      }

      /* Get server ip address */

      if (!getipaddr (p, serveripaddr, &p, ',')) {
        oz_knl_printk ("oz_dev_ip_fs: bad server ip address %s\n", p);
        return (OZ_BADPARAM);
      }

      /* Get server port number */

      sts = oz_hw_atoi (p, &usedup);
      if ((usedup == 0) || (p[usedup] != 0)) {
        oz_knl_printk ("oz_dev_ip_fs: bad server port number %s\n", p);
        return (OZ_BADPARAM);
      }
      OZ_IP_H2NW (sts, serverportno);

      if (unitname[0] != 0) {
        oz_knl_printk ("oz_dev_ip_fs mount: ether devname %s\n", unitname);
        oz_knl_printk ("oz_dev_ip_fs_mount:     hw ipaddr %s\n", cvtipbin (hwipaddr, ipstr));
        oz_knl_printk ("oz_dev_ip_fs mount:     nw ipaddr %s\n", cvtipbin (nwipaddr, ipstr));
        oz_knl_printk ("oz_dev_ip_fs mount:     nw ipmask %s\n", cvtipbin (nwipmask, ipstr));
        oz_knl_printk ("oz_dev_ip_fs mount:     gw ipaddr %s\n", gwpresent ? cvtipbin (gwipaddr, ipstr) : "none");
      }
      oz_knl_printk ("oz_dev_ip_fs mount:   server ipad %s\n", cvtipbin (serveripaddr, ipstr));
      oz_knl_printk ("oz_dev_ip_fs mount:   server port %u\n", OZ_IP_N2HW (serverportno));

      /* Open a channel to IP stack */

      sts = oz_knl_iochan_crbynm (OZ_IO_IP_DEV, OZ_LOCKMODE_CW, OZ_PROCMODE_KNL, NULL, &ipchan);
      if (sts != OZ_SUCCESS) {
        oz_knl_printk ("oz_dev_ip_fs: error %u assigning channel to 'ip' device\n", sts);
        return (sts);
      }

      /* Maybe perform configuration of ethernet device */

      if (unitname[0] != 0) {
        memset (&ip_hwadd, 0, sizeof ip_hwadd);
        ip_hwadd.devname = unitname;
        sts = oz_knl_io (ipchan, OZ_IO_IP_HWADD, sizeof ip_hwadd, &ip_hwadd);
        if ((sts != OZ_SUCCESS) && (sts != OZ_HWALRDEFINED)) {
          oz_knl_printk ("oz_dev_ip_fs: error %u adding hardware interface to ip\n", sts);
          oz_knl_iochan_increfc (ipchan, -1);
          return (sts);
        }

        memset (&ip_hwipamadd, 0, sizeof ip_hwipamadd);
        ip_hwipamadd.addrsize = OZ_IO_IP_ADDRSIZE;
        ip_hwipamadd.devname  = unitname;
        ip_hwipamadd.hwipaddr = hwipaddr;
        ip_hwipamadd.nwipaddr = nwipaddr;
        ip_hwipamadd.nwipmask = nwipmask;
        sts = oz_knl_io (ipchan, OZ_IO_IP_HWIPAMADD, sizeof ip_hwipamadd, &ip_hwipamadd);
        if (sts != OZ_SUCCESS) {
          oz_knl_printk ("oz_dev_ip_fs: error %u adding hardware ip address and mask definition\n", sts);
          oz_knl_iochan_increfc (ipchan, -1);
          return (sts);
        }

        if (gwpresent) {
          memset (&ip_routeadd, 0, sizeof ip_routeadd);
          memset (nullipad, 0, sizeof nullipad);
          ip_routeadd.addrsize = OZ_IO_IP_ADDRSIZE;
          ip_routeadd.gwipaddr = gwipaddr;
          ip_routeadd.nwipaddr = nullipad;
          ip_routeadd.nwipmask = nullipad;
          sts = oz_knl_io (ipchan, OZ_IO_IP_ROUTEADD, sizeof ip_routeadd, &ip_routeadd);
          if (sts != OZ_SUCCESS) {
            oz_knl_printk ("oz_dev_ip_fs: error %u adding routing definition\n", sts);
            oz_knl_iochan_increfc (ipchan, -1);
            return (sts);
          }
        }
      }

      /* Bind socket to receive from only the given socket */

      memset (&ip_udpbind, 0, sizeof ip_udpbind);
      ip_udpbind.addrsize  = OZ_IO_IP_ADDRSIZE;
      ip_udpbind.portsize  = OZ_IO_IP_PORTSIZE;
      ip_udpbind.remipaddr = serveripaddr;
      ip_udpbind.remportno = serverportno;

      sts = oz_knl_io (ipchan, OZ_IO_IP_UDPBIND, sizeof ip_udpbind, &ip_udpbind);
      if (sts != OZ_SUCCESS) {
        oz_knl_printk ("oz_dev_ip_fs: error %u binding socket\n", sts);
        oz_knl_iochan_increfc (ipchan, -1);
        return (sts);
      }

      /* Now we can let requests queue */

      vl = oz_hw_smplock_wait (&(devex -> smplock_vl));
      if (devex -> ipchan != NULL) {
        oz_hw_smplock_clr (&(devex -> smplock_vl), vl);
        oz_knl_iochan_increfc (ipchan, -1);
        return (OZ_ALREADYMOUNTED);
      }
      memcpy (devex -> serveripaddr, serveripaddr, sizeof devex -> serveripaddr);
      memcpy (devex -> serverportno, serverportno, sizeof devex -> serverportno);
      devex -> ipchan = ipchan;
      oz_hw_smplock_clr (&(devex -> smplock_vl), vl);

      /* Successfully mounted, rename device to something useful */

      oz_sys_sprintf (sizeof unitname, unitname, "%s.%u", cvtipbin (serveripaddr, ipstr), OZ_IP_N2HW (serverportno));
      oz_knl_devunit_rename (devunit, unitname, "Waiting for server");

      /* Tell server to wipe its saved request list */

      iopex -> u.mountvol.devunit = devunit;
      sts = performreq (iopex, 0);
      return (sts);
    }

    /* Dismount - just close the IP channel to prevent any more requests from queueing */

    case OZ_IO_FS_DISMOUNT: {

      /* Make sure there are no I/O's in progress */

      vl = oz_hw_smplock_wait (&(devex -> smplock_vl));
      if (devex -> iocount > 0) {
        oz_hw_smplock_clr (&(devex -> smplock_vl), vl);
        return (OZ_OPENFILESONVOL);
      }

      /* Ok, mark it closed by clearing ipchan */

      ipchan = devex -> ipchan;
      devex -> ipchan = NULL;
      oz_hw_smplock_clr (&(devex -> smplock_vl), vl);
      if (ipchan == NULL) return (OZ_NOTMOUNTED);

      /* Close ipchan and return success status */

      oz_knl_iochan_increfc (ipchan, -1);
      return (OZ_SUCCESS);
    }

    /* Open a file */

    case OZ_IO_FS_OPEN: {
      if (chnex -> handle != 0) return (OZ_FILEALREADYOPEN);
      chnex -> ignclose = iopex -> u.open.p.ignclose;
      strncpyz (iopex -> req.u.open.name, iopex -> u.open.p.name, sizeof iopex -> req.u.open.name);
      sts = performreq (iopex, 0);
      return (sts);
    }

    /* Read blocks */

    case OZ_IO_FS_READBLOCKS: {

      /* Lock read buffer in memory and get its physical page(s) */

      sts = oz_knl_ioop_lockw (ioop, iopex -> u.readblocks.p.size, iopex -> u.readblocks.p.buff, 
                               &(iopex -> u.readblocks.buff_pp), NULL, &(iopex -> u.readblocks.buff_bo));

      /* Perform request */

      if (sts == OZ_SUCCESS) {
        iopex -> req.u.readblocks.size = iopex -> u.readblocks.p.size;
        iopex -> req.u.readblocks.svbn = iopex -> u.readblocks.p.svbn;
        sts = performreq (iopex, 0);
      }
      return (sts);
    }

    /* Read record */

    case OZ_IO_FS_READREC: {
      if (iopex -> u.readrec.p.trmsize > sizeof iopex -> req.u.readrec.trmbuff) {
        oz_knl_printk ("oz_dev_ip_fs: terminator size %u too big, max %u\n", iopex -> u.readrec.p.trmsize, sizeof iopex -> req.u.readrec.trmbuff);
        return (OZ_BADPARAM);
      }
      if (iopex -> u.readrec.p.pmtsize > sizeof iopex -> req.u.readrec.pmtbuff) {
        oz_knl_printk ("oz_dev_ip_fs: prompt size %u too big, max %u\n", iopex -> u.readrec.p.pmtsize, sizeof iopex -> req.u.readrec.pmtbuff);
        return (OZ_BADPARAM);
      }
      sts = oz_knl_ioop_lockw (ioop, iopex -> u.readrec.p.size, iopex -> u.readrec.p.buff, 
                               &(iopex -> u.readrec.buff_pp), NULL, &(iopex -> u.readrec.buff_bo));
      if ((sts == OZ_SUCCESS) && (iopex -> u.readrec.p.rlen != NULL)) {
        sts = oz_knl_ioop_lockw (ioop, sizeof *(iopex -> u.readrec.p.rlen), iopex -> u.readrec.p.rlen, 
                                 &(iopex -> u.readrec.rlen_pp), NULL, &(iopex -> u.readrec.rlen_bo));
      }
      if (sts == OZ_SUCCESS) {
        iopex -> req.u.readrec.size    = iopex -> u.readrec.p.size;
        iopex -> req.u.readrec.trmsize = iopex -> u.readrec.p.trmsize;
        iopex -> req.u.readrec.pmtsize = iopex -> u.readrec.p.pmtsize;
        iopex -> req.u.readrec.atblock = iopex -> u.readrec.p.atblock;
        iopex -> req.u.readrec.atbyte  = iopex -> u.readrec.p.atbyte;
        memcpy (iopex -> req.u.readrec.trmbuff, iopex -> u.readrec.p.trmbuff, iopex -> u.readrec.p.trmsize);
        memcpy (iopex -> req.u.readrec.pmtbuff, iopex -> u.readrec.p.pmtbuff, iopex -> u.readrec.p.pmtsize);
        sts = performreq (iopex, 0);
      }
      return (sts);
    }

    /* Read into physical pages */

    case OZ_IO_FS_PAGEREAD: {
      if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);

      /* Convert to a READBLOCKS request */

      iopex -> req.func = OZ_IO_FS_READBLOCKS;
      iopex -> req.u.readblocks.size = iopex -> u.pageread.p.pagecount << OZ_HW_L2PAGESIZE;
      iopex -> req.u.readblocks.svbn = iopex -> u.pageread.p.startblock;
      iopex -> u.readblocks.buff_pp = iopex -> u.pageread.p.pagearray;
      iopex -> u.readblocks.buff_bo = 0;
      iopex -> u.readblocks.p.size  = iopex -> req.u.readblocks.size;

      /* Perform the request */

      sts = performreq (iopex, 0);
      return (sts);
    }

    /* Close file */

    case OZ_IO_FS_CLOSE: {
      if (chnex -> ignclose) return (OZ_SUCCESS);
      sts = performreq (iopex, 0);
      return (sts);
    }

    /* Get info, part 1 */

    case OZ_IO_FS_GETINFO1: {
      sts = oz_knl_ioop_lockw (ioop, as, ap, &(iopex -> u.getinfo1.buff_pp), NULL, &(iopex -> u.getinfo1.buff_bo));
      if (sts == OZ_SUCCESS) {
        memset (ap, 0, as);
        iopex -> u.getinfo1.as = as;
        sts = performreq (iopex, 0);
      }
      return (sts);
    }

    /* Read directory entry */

    case OZ_IO_FS_READDIR: {
      sts = oz_knl_ioop_lockw (ioop, iopex -> u.readdir.p.filenamsize, iopex -> u.readdir.p.filenambuff, 
                               &(iopex -> u.readdir.filenambuff_pp), NULL, &(iopex -> u.readdir.filenambuff_bo));
      if (sts == OZ_SUCCESS) {
        sts = performreq (iopex, 0);
      }
      return (sts);
    }

    /* Wildcard directory scan */

    case OZ_IO_FS_WILDSCAN: {
      sts = oz_knl_ioop_lockw (ioop, iopex -> u.wildscan.p.size, iopex -> u.wildscan.p.buff, 
                               &(iopex -> u.wildscan.buff_pp), NULL, &(iopex -> u.wildscan.buff_bo));
      if (sts == OZ_SUCCESS) {
        iopex -> req.u.wildscan.init = iopex -> u.wildscan.p.init;
        strncpyz (iopex -> req.u.wildscan.wild, iopex -> u.wildscan.p.wild, sizeof iopex -> req.u.wildscan.wild);
        sts = performreq (iopex, 0);
      }
      return (sts);
    }

    /* Set current position */

    case OZ_IO_FS_SETCURPOS: {

      /* Perform request */

      iopex -> req.u.setcurpos.atblock = iopex -> u.setcurpos.p.atblock;
      iopex -> req.u.setcurpos.atbyte  = iopex -> u.setcurpos.p.atbyte;
      sts = performreq (iopex, 0);
      return (sts);
    }

    /* Unknown / Unsupported I/O function */

    default: return (OZ_BADIOFUNC);
  }
}

/************************************************************************/
/*									*/
/*  Perform request							*/
/*									*/
/*    Input:								*/
/*									*/
/*	iopex -> req = all filled in with request message		*/
/*									*/
/************************************************************************/

static uLong performreq (Iopex *iopex, int repeat)

{
  Chnex *chnex;
  Devex *devex;
  uLong vl;

  /* Make sure volume is mounted then prevent dismount by incing the iocount */

  devex = iopex -> devex;
  vl = oz_hw_smplock_wait (&(devex -> smplock_vl));
  if (devex -> ipchan == NULL) {
    oz_hw_smplock_clr (&(devex -> smplock_vl), vl);
    return (OZ_NOTMOUNTED);
  }
  devex -> iocount ++;

  /* Set up sequence number for the request */

  iopex -> req.seq = ++ (devex -> seq);
  oz_hw_smplock_clr (&(devex -> smplock_vl), vl);

  /* Allocate a timer block and init retry counter */

  if (!repeat) {
    iopex -> timer = oz_knl_timer_alloc ();
    if (iopex -> timer == NULL) return (OZ_EXQUOTANPP);
  }
  iopex -> retries = 50;

  /* Set the ref count to 2.  It gets dec'd at the end of this routine and when the status is set. */
  /* If repeating, the count is already incremented for setting the status, leave it alone.        */

  if (!repeat) iopex -> refcount = 2;
  iopex -> status = OZ_PENDING;

  /* Link to chnex -> iopexs.  It is now subject to ioabort calls. */

  chnex = iopex -> chnex;
  if (!repeat) {
    vl = oz_hw_smplock_wait (&(devex -> smplock_vl));
    iopex -> next   = chnex -> iopexs;
    iopex -> prev   = &(chnex -> iopexs);
    chnex -> iopexs = iopex;
    if (iopex -> next != NULL) iopex -> next -> prev = &(iopex -> next);
    oz_hw_smplock_clr (&(devex -> smplock_vl), vl);
  }

  /* Start a receive to get the reply, then start sending the request */

  performrcv (iopex);
  performsend (iopex);

  /* All done with iopex pointer */

  if (!repeat) deciopexrefc (iopex);

  /* It completes asynchronously (ie, via oz_knl_iodone) */

  return (OZ_STARTED);
}

/* This routine starts transmitting the request */

static void performsend (Iopex *iopex)

{
  Devex *devex;
  uLong sts;
  OZ_Datebin when;
  OZ_IO_ip_udptransmit ip_udptransmit;

  devex = iopex -> devex;

  /* Inc ref count to be decremented when ast completes */

  OZ_HW_ATOMIC_INCBY1_LONG (iopex -> refcount);

  /* Start timing it */

  if (!oz_knl_timer_remove (iopex -> timer)) OZ_HW_ATOMIC_INCBY1_LONG (iopex -> refcount);
  when = oz_hw_tod_getnow ();
  OZ_HW_DATEBIN_ADD (when, when, retryinterval);
  oz_knl_timer_insert (iopex -> timer, when, timedout, iopex);

  /* Set up transmit parameters */

  memset (&ip_udptransmit, 0, sizeof ip_udptransmit);
  ip_udptransmit.addrsize  = OZ_IO_IP_ADDRSIZE;
  ip_udptransmit.portsize  = OZ_IO_IP_PORTSIZE;
  ip_udptransmit.rawsize   = sizeof iopex -> req;
  ip_udptransmit.rawbuff   = &(iopex -> req);
  ip_udptransmit.dstipaddr = devex -> serveripaddr;
  ip_udptransmit.dstportno = devex -> serverportno;

  /* Start transmitting request */

  sts = oz_knl_iostart2 (1, iopex -> devex -> ipchan, OZ_PROCMODE_KNL, sendcomplete, iopex, NULL, 
                         NULL, NULL, NULL, OZ_IO_IP_UDPTRANSMIT, sizeof ip_udptransmit, &ip_udptransmit);
  if (sts != OZ_STARTED) sendcomplete (iopex, sts);
}

/* This routine starts receiving the reply */

static void performrcv (Iopex *iopex)

{
  uLong sts;
  OZ_IO_ip_udpreceive ip_udpreceive;

  /* Inc ref count to be decremented when ast completes */

  OZ_HW_ATOMIC_INCBY1_LONG (iopex -> refcount);

  /* Set up receive parameters */

  memset (&ip_udpreceive, 0, sizeof ip_udpreceive);
  ip_udpreceive.addrsize = OZ_IO_IP_ADDRSIZE;
  ip_udpreceive.portsize = OZ_IO_IP_PORTSIZE;
  ip_udpreceive.rawsize  = sizeof iopex -> rpl;
  ip_udpreceive.rawbuff  = &(iopex -> rpl);

  /* Start receiving reply */

  sts = oz_knl_iostart2 (1, iopex -> devex -> ipchan, OZ_PROCMODE_KNL, receivecomplete, iopex, NULL, 
                         NULL, NULL, NULL, OZ_IO_IP_UDPRECEIVE, sizeof ip_udpreceive, &ip_udpreceive);
  if (sts != OZ_STARTED) receivecomplete (iopex, sts);
}

/* Ast routine called when transmit has completed */

static void sendcomplete (void *iopexv, uLong status)

{
  Iopex *iopex;

  iopex = iopexv;

  /* See if transmit failed */

  if (status != OZ_SUCCESS) {

    /* Maybe it was aborted by an ioabort call for another request */

    if ((status == OZ_ABORTED) && (iopex -> status == OZ_PENDING)) {
      performsend (iopex);
    }

    /* Not so, abort the I/O request */

    else {
      setiopexsts (iopex, status);
      oz_knl_printk ("oz_dev_ip_fs: error %u sending request\n", status);
      oz_knl_ioabort (iopex -> devex -> ipchan, OZ_PROCMODE_KNL);
    }
  }

  /* Anyway, the ast has completed */

  deciopexrefc (iopex);
}

/* Ast routine called when timeout happens */

static void timedout (void *iopexv, OZ_Timer *timer)

{
  Iopex *iopex;

  iopex = iopexv;

  /* If more retries left, re-send the request */

  if (-- (iopex -> retries) > 0) performsend (iopex);

  /* Otherwise, abort the request */

  else if (setiopexsts (iopex, OZ_TIMEDOUT)) {
    oz_knl_printk ("oz_dev_ip_fs: request timed out\n");
    oz_knl_ioabort (iopex -> devex -> ipchan, OZ_PROCMODE_KNL);
  }

  /* Anyway, the ast has completed */

  deciopexrefc (iopex);
}

/* Ast routine called when the receive has completed */

static void receivecomplete (void *iopexv, uLong status)

{
  Iopex *iopex;
  uLong len, sts;

  iopex = iopexv;

  /* Make sure the receive was successful */

  if (status != OZ_SUCCESS) {

    /* Maybe it was aborted by an ioabort call for another request */

    if ((status == OZ_ABORTED) && (iopex -> status == OZ_PENDING)) {
      performrcv (iopex);
      goto rtn;
    }

    /* Not so, abort I/O request */

    oz_knl_printk ("oz_dev_ip_fs: error %u receiving reply\n", status);
    sts = status;
    goto done;
  }

  /* Make sure the sequence number matches, if not, try to read again */

  if (iopex -> rpl.seq != iopex -> req.seq) {
    performrcv (iopex);
    goto rtn;
  }

  /* Ok, get disk I/O status */

  sts = iopex -> rpl.status;

  /* Finish processing based on function code */

  switch (iopex -> req.func) {

    /* No-op on our part for mount - server just wipes its saved requests */

    case OZ_IO_FS_MOUNTVOL: {
      oz_knl_devunit_rename (iopex -> u.mountvol.devunit, NULL, "Mounted");
      break;
    }

    /* Open - save the returned handle number in the 'chnex' struct */

    case OZ_IO_FS_OPEN: {
      if (iopex -> chnex -> handle != 0) sts = OZ_FILEALREADYOPEN;
      else iopex -> chnex -> handle = iopex -> rpl.u.open.handle;
      break;
    }

    /* Read - memcpy back the results.  If we did not get it all, request reading of following block(s). */

    case OZ_IO_FS_READBLOCKS: {

      /* If we didn't get it all, round back the size we got to a block boundary so we can resume on a block boundary */

      if ((sts == OZ_SUCCESS) && (iopex -> rpl.u.readblocks.rlen < iopex -> req.u.readblocks.size)) {
        iopex -> rpl.u.readblocks.rlen &= -DISK_BLOCK_SIZE;
      }

      /* Make sure we didn't get more than we asked for */

      if (iopex -> rpl.u.readblocks.rlen > iopex -> req.u.readblocks.size) iopex -> rpl.u.readblocks.rlen = iopex -> req.u.readblocks.size;

      /* Copy what we got to the user's buffer */

      oz_hw_phys_movefromvirt (iopex -> rpl.u.readblocks.rlen, iopex -> rpl.u.readblocks.buff, iopex -> u.readblocks.buff_pp, iopex -> u.readblocks.buff_bo);

      /* If we didn't get it all, queue another read request to get the rest */

      iopex -> req.u.readblocks.size -= iopex -> rpl.u.readblocks.rlen;
      if ((sts == OZ_SUCCESS) && (iopex -> req.u.readblocks.size > 0)) {
        iopex -> u.readblocks.buff_bo += iopex -> rpl.u.readblocks.rlen;
        iopex -> u.readblocks.buff_pp += iopex -> u.readblocks.buff_bo >> OZ_HW_L2PAGESIZE;
        iopex -> u.readblocks.buff_bo &= ~ (-1 << OZ_HW_L2PAGESIZE);
        iopex -> req.u.readblocks.svbn += iopex -> rpl.u.readblocks.rlen / DISK_BLOCK_SIZE;
        sts = performreq (iopex, 1);
        if (sts == OZ_STARTED) goto rtn;
      }

      /* All done, return the completion status */

      break;
    }

    /* Read - memcpy back the results.  If we did not get it all, request reading of following block(s). */

    case OZ_IO_FS_READREC: {

      /* Make sure we didn't get more than we asked for */

      if (iopex -> rpl.u.readrec.rlen > iopex -> req.u.readrec.size) iopex -> rpl.u.readrec.rlen = iopex -> req.u.readrec.size;

      /* Copy what we got to the user's buffer */

      oz_hw_phys_movefromvirt (iopex -> rpl.u.readrec.rlen, iopex -> rpl.u.readrec.buff, iopex -> u.readrec.buff_pp, iopex -> u.readrec.buff_bo);

      /* If we didn't get it all, queue another read request to get the rest */

      iopex -> req.u.readrec.size -= iopex -> rpl.u.readrec.rlen;
      if ((iopex -> req.u.readrec.size > 0) && ((sts == OZ_NOTERMINATOR) || ((sts == OZ_SUCCESS) && (iopex -> req.u.readrec.trmsize == 0)))) {
        iopex -> u.readrec.buff_bo += iopex -> rpl.u.readrec.rlen;
        iopex -> u.readrec.buff_pp += iopex -> u.readrec.buff_bo >> OZ_HW_L2PAGESIZE;
        iopex -> u.readrec.buff_bo &= ~ (-1 << OZ_HW_L2PAGESIZE);
        if (iopex -> req.u.readrec.atblock != 0) {
          iopex -> req.u.readrec.atbyte  += iopex -> rpl.u.readrec.rlen;
          iopex -> req.u.readrec.atblock += iopex -> req.u.readrec.atbyte / DISK_BLOCK_SIZE;
          iopex -> req.u.readrec.atbyte  %= DISK_BLOCK_SIZE;
        }
        sts = performreq (iopex, 1);
        if (sts == OZ_STARTED) goto rtn;
      }

      /* All done, return length read and the completion status */

      if (iopex -> u.readrec.p.rlen != NULL) {
        iopex -> u.readrec.p.size -= iopex -> req.u.readrec.size;
        oz_hw_phys_movefromvirt (sizeof *(iopex -> u.readrec.p.rlen), &(iopex -> u.readrec.p.size), iopex -> u.readrec.rlen_pp, iopex -> u.readrec.rlen_bo);
      }

      break;
    }

    /* Close - clear chnex handle */

    case OZ_IO_FS_CLOSE: {
      iopex -> chnex -> handle = 0;
      break;
    }

    /* Getinfo1 - copy data back to user's buffer */

    case OZ_IO_FS_GETINFO1: {
      OZ_IO_fs_getinfo1 fs_getinfo1;

      memset (&fs_getinfo1, 0, sizeof fs_getinfo1);
      fs_getinfo1.blocksize    = iopex -> rpl.u.getinfo1.blocksize;
      fs_getinfo1.eofblock     = iopex -> rpl.u.getinfo1.eofblock;
      fs_getinfo1.eofbyte      = iopex -> rpl.u.getinfo1.eofbyte;
      fs_getinfo1.hiblock      = iopex -> rpl.u.getinfo1.hiblock;
      fs_getinfo1.curblock     = iopex -> rpl.u.getinfo1.curblock;
      fs_getinfo1.curbyte      = iopex -> rpl.u.getinfo1.curbyte;
      fs_getinfo1.filattrflags = iopex -> rpl.u.getinfo1.filattrflags;
      fs_getinfo1.create_date  = iopex -> rpl.u.getinfo1.modify_date;
      fs_getinfo1.access_date  = iopex -> rpl.u.getinfo1.access_date;
      fs_getinfo1.change_date  = iopex -> rpl.u.getinfo1.change_date;
      fs_getinfo1.modify_date  = iopex -> rpl.u.getinfo1.modify_date;
      if (iopex -> u.getinfo1.as > sizeof fs_getinfo1) iopex -> u.getinfo1.as = sizeof fs_getinfo1;
      oz_hw_phys_movefromvirt (iopex -> u.getinfo1.as, &fs_getinfo1, iopex -> u.getinfo1.buff_pp, iopex -> u.getinfo1.buff_bo);
      break;
    }

    /* Readdir - copy filename string back to user's buffer */

    case OZ_IO_FS_READDIR: {
      len = strlen (iopex -> rpl.u.readdir.name) + 1;
      if (len > iopex -> u.readdir.p.filenamsize) len = iopex -> u.readdir.p.filenamsize;
      oz_hw_phys_movefromvirt (len, iopex -> rpl.u.readdir.name, iopex -> u.readdir.filenambuff_pp, iopex -> u.readdir.filenambuff_bo);
      break;
    }

    /* Wildscan - save any handle modification and copy resultant string back to user's buffer */

    case OZ_IO_FS_WILDSCAN: {
      iopex -> chnex -> handle = iopex -> rpl.u.wildscan.handle;
      len = strlen (iopex -> rpl.u.wildscan.spec) + 1;
      if (len > iopex -> u.wildscan.p.size) len = iopex -> u.wildscan.p.size;
      oz_hw_phys_movefromvirt (len, iopex -> rpl.u.wildscan.spec, iopex -> u.wildscan.buff_pp, iopex -> u.wildscan.buff_bo);
      break;
    }
  }

  /* Post request completion */

done:
  setiopexsts (iopex, sts);
rtn:
  deciopexrefc (iopex);
}

/* Set completion status if not already set and dec ref count */

static int setiopexsts (Iopex *iopex, uLong sts)

{
  Devex *devex;
  int wesetit;
  uLong vl;

  devex = iopex -> devex;

  wesetit = 0;
  vl = oz_hw_smplock_wait (&(devex -> smplock_vl));
  if (iopex -> status == OZ_PENDING) {
    iopex -> status = sts;
    if (oz_hw_atomic_inc_long (&(iopex -> refcount), -1) == 0) {
      oz_crash ("oz_dev_ip_fs setiopexsts: refcount went zero");
    }
    wesetit = 1;
  }
  oz_hw_smplock_clr (&(devex -> smplock_vl), vl);
  if (oz_knl_timer_remove (iopex -> timer)) deciopexrefc (iopex);

  return (wesetit);
}

/* Decrement iopex's ref count.  If zero, we have accounted for */
/* all ast's and set the status, so post request completion.    */

static void deciopexrefc (Iopex *iopex)

{
  Devex *devex;
  Long refc;
  uLong vl;

again:
  do {
    refc = iopex -> refcount;
    if (refc <= 1) goto going_le_zero;
  } while (!oz_hw_atomic_setif_long (&(iopex -> refcount), refc - 1, refc));
  return;

going_le_zero:
  if (refc <= 0) oz_crash ("oz_dev_ip_fs deciopexrefc: refcount was %d", refc);
  devex = iopex -> devex;
  vl = oz_hw_smplock_wait (&(devex -> smplock_vl));
  if (!oz_hw_atomic_setif_long (&(iopex -> refcount), 0, 1)) {
    oz_hw_smplock_clr (&(devex -> smplock_vl), vl);
    goto again;
  }
  devex -> iocount --;
  *(iopex -> prev) = iopex -> next;
  if (iopex -> next != NULL) iopex -> next -> prev = iopex -> prev;
  oz_hw_smplock_clr (&(devex -> smplock_vl), vl);
  oz_knl_timer_free (iopex -> timer);
  iopex -> timer = NULL;
  oz_knl_iodone (iopex -> ioop, iopex -> status, NULL, NULL, NULL);
}

/************************************************************************/
/*									*/
/*  Convert ip string to binary						*/
/*									*/
/************************************************************************/

static int getipaddr (const char *ipstr, uByte ipbin[OZ_IO_IP_ADDRSIZE], const char **p_r, char term)

{
  const char *p;
  int i, usedup;
  uLong v;

  p = ipstr;
  for (i = 0; i < OZ_IO_IP_ADDRSIZE; i ++) {
    v = oz_hw_atoi (p, &usedup);
    p += usedup;
    if ((v > 255) || (*p != ((i == OZ_IO_IP_ADDRSIZE - 1) ? term : '.'))) return (0);
    p ++;
    ipbin[i] = v;
  }
  *p_r = p;
  return (1);
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
