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
/*  IP configuration utility						*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_crtl_malloc.h"
#include "oz_io_console.h"
#include "oz_io_ether.h"
#include "oz_io_fs.h"
#include "oz_io_ip.h"
#include "oz_io_timer.h"
#include "oz_knl_hw.h"
#include "oz_knl_status.h"
#include "oz_knl_thread.h"
#include "oz_sys_callknl.h"
#include "oz_sys_dateconv.h"
#include "oz_sys_event.h"
#include "oz_sys_gethostipaddr.h"
#include "oz_sys_handle.h"
#include "oz_sys_handle_getinfo.h"
#include "oz_sys_io.h"
#include "oz_sys_io_fs.h"
#include "oz_sys_io_fs_printf.h"
#include "oz_sys_process.h"
#include "oz_util_start.h"

#define TCP_FLAGS_FIN 0x0001	/* sender is finished sending data */
#define TCP_FLAGS_SYN 0x0002	/* synchronize sequence numbers */
#define TCP_FLAGS_RST 0x0004	/* reset the connection */
#define TCP_FLAGS_PSH 0x0008	/* push data to application asap */
#define TCP_FLAGS_ACK 0x0010	/* the acknowledgment number is valid */
#define TCP_FLAGS_URG 0x0020	/* the urgent pointer is valid */
#define TCP_FLAGS_HLNMSK 0xF000	/* header length (in uLongs) */
#define TCP_FLAGS_HLNSHF 12

#define WS(w) (((w >> 8) | (w << 8)) & 0xFFFF)

typedef struct Command Command;

struct Command { const char *name;
                 uLong (*entry) (const char *name, void *param, int argc, char *argv[]);
                 void *param;
                 const char *help;
               };

uLong int_arp_add (const char *name, void *dummy, int argc, char *argv[]);
uLong int_arp_list (const char *name, void *dummy, int argc, char *argv[]);
static uLong arplistdev (const char *devname);
uLong int_arp_rem (const char *name, void *dummy, int argc, char *argv[]);
uLong int_debug (const char *name, void *dummy, int argc, char *argv[]);
static uLong setdebug (OZ_Procmode cprocmode, void *debugv);
uLong int_ether_dump (const char *name, void *dummy, int argc, char *argv[]);
static char *decodenhexbytes (uLong n, char *s, uByte *outdata, uByte *outmask);
static char *printdecimal (char *outpnt, uLong w, uLong value);
static char *printnhexbytes (char *outpnt, uLong n, uByte *bytes, char sep);
uLong int_filter_add (const char *name, void *dummy, int argc, char *argv[]);
uLong int_filter_list (const char *name, void *dummy, int argc, char *argv[]);
uLong int_filter_rem (const char *name, void *dummy, int argc, char *argv[]);
uLong int_hw_add (const char *name, void *dummy, int argc, char *argv[]);
uLong int_hw_rem (const char *name, void *dummy, int argc, char *argv[]);
uLong int_hw_ipam_add (const char *name, void *dummy, int argc, char *argv[]);
uLong int_hw_ipam_rem (const char *name, void *dummy, int argc, char *argv[]);
uLong int_hw_list (const char *name, void *dummy, int argc, char *argv[]);
uLong int_ping (const char *name, void *dummy, int argc, char *argv[]);
uLong int_route_add (const char *name, void *dummy, int argc, char *argv[]);
uLong int_route_list (const char *name, void *dummy, int argc, char *argv[]);
uLong int_route_rem (const char *name, void *dummy, int argc, char *argv[]);
uLong int_status (const char *name, void *dummy, int argc, char *argv[]);
uLong int_dns_svradd (const char *name, void *dummy, int argc, char *argv[]);
uLong int_dns_svrrem (const char *name, void *dummy, int argc, char *argv[]);
uLong int_dns_svrlist (const char *name, void *dummy, int argc, char *argv[]);
uLong int_dns_lookup (const char *name, void *dummy, int argc, char *argv[]);

static const Command intcmd[] = {
	"arp add",         int_arp_add,     NULL, "<device_name> <ip_address> <ether_address> [<timeout_mS/never>]", 
	"arp list",        int_arp_list,    NULL, "[<device_name>]", 
	"arp rem",         int_arp_rem,     NULL, "<device_name> <ip_address> <ether_address>", 
	"debug",           int_debug,       NULL, "<debug_hex_mask>", 
	"dns server add",  int_dns_svradd,  NULL, "<ip_address> <port_number>",
	"dns server rem",  int_dns_svrrem,  NULL, "<ip_address> <port_number>", 
	"dns server list", int_dns_svrlist, NULL, "",
	"dns lookup",      int_dns_lookup,  NULL, "<host_name>", 
	"ether dump",      int_ether_dump,  NULL, "<options...> <device_name>", 
	"filter add",      int_filter_add,  NULL, "<listname> <index> <terms...>", 
	"filter rem",      int_filter_rem,  NULL, "<listname> <index>", 
	"filter list",     int_filter_list, NULL, "<listname>", 
	"hw add",          int_hw_add,      NULL, "<device_name>", 
	"hw ipam add",     int_hw_ipam_add, NULL, "<device_name> <hw_ip_address> [<net_ip_address>] <net_ip_mask>", 
	"hw ipam rem",     int_hw_ipam_rem, NULL, "<device_name> <hw_ip_address> [<net_ip_address>]", 
	"hw list",         int_hw_list,     NULL, "", 
	"hw rem",          int_hw_rem,      NULL, "<device_name>", 
	"ping",            int_ping,        NULL, "[-count <count>] <ip_address>", 
	"route add",       int_route_add,   NULL, "<net_ip_address> <net_ip_mask> <gw_ip_address>", 
	"route list",      int_route_list,  NULL, "", 
	"route rem",       int_route_rem,   NULL, "<net_ip_address> <net_ip_mask> <gw_ip_address>", 
	"status",          int_status,      NULL, "", 
	NULL, NULL, NULL, NULL };

static const char hextab[] = "0123456789ABCDEF";

static char *pn;
static OZ_Handle h_ioevent, h_ipchan;

static const Command *decode_command (int argc, char **argv, const Command *cmdtbl, int *argc_r);;
static int cmpcmdname (int argc, char **argv, const char *name);
static uLong knlio (uLong funcode, uLong as, void *ap);
static uLong knlioknl (OZ_Procmode cprocmode, void *knlioprmv);
static uLong cvtipstr (char *ipstr, uByte ipbuf[OZ_IO_IP_ADDRSIZE]);
static char *cvtipbin (uByte ipbin[OZ_IO_IP_ADDRSIZE], char ipstr[OZ_IO_IP_ADDRSIZE*4]);
static uLong cvtpnstr (char *pnstr, uByte pnbin[OZ_IO_IP_PORTSIZE]);
static char *cvtpnbin (uByte pnbin[OZ_IO_IP_PORTSIZE], char pnstr[OZ_IO_IP_PORTSIZE*4]);
static uLong cvtenstr (const char *enstr, uByte enbinsize, uByte enbin[OZ_IO_ETHER_MAXADDR]);
static char *cvtenbin (uByte enbinsize, uByte enbin[OZ_IO_ETHER_MAXADDR], char enstr[OZ_IO_ETHER_MAXADDR*3]);
static uByte getenaddrsize (const char *devname);

uLong oz_util_main (int argc, char *argv[])

{
  const Command *command;
  int i;
  uLong sts;

  pn = "ipconfig";
  if (argc > 0) {
    -- argc;
    pn = *(argv ++);
  }

  /* Assign an handle to the 'ip' device */

  sts = oz_sys_io_assign (OZ_PROCMODE_KNL, &h_ipchan, "ip", OZ_LOCKMODE_CW);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u assigning channel to 'ip'\n", pn, sts);
    return (sts);
  }

  /* Make an event flag for the knlio routine */

  sts = oz_sys_event_create (OZ_PROCMODE_KNL, "ip util knlio", &h_ioevent);
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);

  /* Decode and execute command */

  command = NULL;
  if (argc > 0) command = decode_command (argc, argv, intcmd, &i);
  if (command != NULL) {
    sts = (*(command -> entry)) (command -> name, command -> param, argc - i, argv + i);
  } else {
    for (i = 0; intcmd[i].name != NULL; i ++) {
      oz_sys_io_fs_printf (oz_util_h_output, "  %s %s\n", intcmd[i].name, intcmd[i].help);
    }
    sts = OZ_BADPARAM;
  }

  return (sts);
}

/************************************************************************/
/*									*/
/*  Decode keyword from command table					*/
/*									*/
/*    Input:								*/
/*									*/
/*	name = command string to decode					*/
/*	cmdtbl = table to decode it from				*/
/*									*/
/*    Output:								*/
/*									*/
/*	decode_command = NULL : keyword not found			*/
/*	                 else : pointer to entry in keyword table	*/
/*									*/
/************************************************************************/

static const Command *decode_command (int argc, char **argv, const Command *cmdtbl, int *argc_r)

{
  const char *p;
  const Command *cmd1;
  int i, j, len1;

  cmd1 = NULL;					/* no entry matched so far */
  len1 = 0;					/* length of last matched entry */

  for (i = 0; (p = cmdtbl[i].name) != NULL; i ++) { /* loop through table */
    j = cmpcmdname (argc, argv, p);		/* compare the name */
    if (j > len1) {				/* see if better match than last */
      cmd1 = cmdtbl + i;			/* ok, save table pointer */
      len1 = j;					/* save number of words used */
      *argc_r = j;
    }
  }

  return (cmd1);				/* return pointer to best match entry (or NULL) */
}

/************************************************************************/
/*									*/
/*  Compare argc/argv to a multiple-word command name string		*/
/*									*/
/*    Input:								*/
/*									*/
/*	argc = number of entries in the argv array			*/
/*	argv = pointer to array of char string pointers			*/
/*	name = multiple-word command name string			*/
/*	       each word must be separated by exactly one space		*/
/*									*/
/*    Output:								*/
/*									*/
/*	cmpcmdname = 0 : doesn't match					*/
/*	          else : number of elements of argv used up		*/
/*									*/
/************************************************************************/

static int cmpcmdname (int argc, char **argv, const char *name)

{
  const char *p;
  int i, j, l;

  p = name;					/* point to multiple-word command name string */
  for (j = 0; j < argc;) {			/* compare to given command words */
    l = strlen (argv[j]);			/* get length of command word */
    if (strncasecmp (argv[j], p, l) != 0) return (0); /* compare */
    p += l;					/* word matches, point to next in multiple-word name */
    j ++;					/* increment to next argv */
    if (*(p ++) != ' ') {			/* see if more words in multiple-word string */
      if (*(-- p) != 0) return (0);		/* if not, fail if not an exact end */
      return (j);				/* success, return number of words */
    }
  }
  return (0);					/* argv too short to match multiple-word name */
}

/************************************************************************/
/*									*/
/*	arp add <device_name> <ipaddr> <enaddr> [<timeout/never>]	*/
/*									*/
/************************************************************************/

uLong int_arp_add (const char *name, void *dummy, int argc, char *argv[])

{
  int usedup;
  OZ_IO_ip_arpadd ip_arpadd;
  uByte enaddr[OZ_IO_ETHER_MAXADDR], enaddrsize, ipaddr[OZ_IO_IP_ADDRSIZE];
  uLong sts, timeout;

  if ((argc != 3) && (argc != 4)) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: arp add <device_name> <ipaddr> <enaddr> [<timeout_mS/never>]\n", pn);
    return (OZ_MISSINGPARAM);
  }

  enaddrsize = getenaddrsize (argv[0]);

  sts = cvtipstr (argv[1], ipaddr);
  if (sts != OZ_SUCCESS) return (sts);
  sts = cvtenstr (argv[2], enaddrsize, enaddr);
  if (sts != OZ_SUCCESS) return (sts);
  timeout = 0;
  if (argc == 4) {
    if (strcasecmp (argv[3], "never") == 0) timeout = -1;
    else timeout = oz_hw_atoi (argv[3], &usedup);
  }

  memset (&ip_arpadd, 0, sizeof ip_arpadd);
  ip_arpadd.addrsize   = OZ_IO_IP_ADDRSIZE;
  ip_arpadd.enaddrsize = enaddrsize;
  ip_arpadd.devname    = argv[0];
  ip_arpadd.ipaddr     = ipaddr;
  ip_arpadd.enaddr     = enaddr;
  ip_arpadd.timeout    = timeout;
  sts = knlio (OZ_IO_IP_ARPADD, sizeof ip_arpadd, &ip_arpadd);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u adding arp entry\n", pn, sts);
  }
  return (sts);
}

/************************************************************************/
/*									*/
/*	arp list [<device_name>]					*/
/*									*/
/************************************************************************/

uLong int_arp_list (const char *name, void *dummy, int argc, char *argv[])

{
  char devname[OZ_DEVUNIT_NAMESIZE];
  OZ_IO_ip_hwlist ip_hwlist;
  uLong sts;

  if ((argc != 0) && (argc != 1)) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: arp list [<device_name>]\n", pn);
    return (OZ_MISSINGPARAM);
  }

  if (argc == 0) {
    devname[0] = 0;
    memset (&ip_hwlist, 0, sizeof ip_hwlist);
    ip_hwlist.devnamesize = sizeof devname;
    ip_hwlist.devnamebuff = devname;
    while (1) {
      sts = oz_sys_io (OZ_PROCMODE_KNL, h_ipchan, 0, OZ_IO_IP_HWLIST, sizeof ip_hwlist, &ip_hwlist);
      if (sts != OZ_SUCCESS) {
        oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u listing hardware devices\n", pn, sts);
        return (sts);
      }
      if (devname[0] == 0) break;
      oz_sys_io_fs_printf (oz_util_h_output, "%s:\n", devname);
      sts = arplistdev (devname);
      if (sts != OZ_SUCCESS) break;
    }
  }
  if (argc == 1) sts = arplistdev (argv[0]);
  return (sts);
}

static uLong arplistdev (const char *devname)

{
  char enstr[OZ_IO_ETHER_MAXADDR*3], ipstr[OZ_IO_IP_ADDRSIZE*4];
  int i;
  OZ_IO_ip_arplist ip_arplist;
  uByte enaddr[OZ_IO_ETHER_MAXADDR], enaddrsize, ipaddr[OZ_IO_IP_ADDRSIZE];
  uLong sts, timeout;

  memset (enaddr, 0, sizeof enaddr);
  memset (ipaddr, 0, sizeof ipaddr);
  timeout = 0;

  enaddrsize = getenaddrsize (devname);

  memset (&ip_arplist, 0, sizeof ip_arplist);
  ip_arplist.addrsize   = OZ_IO_IP_ADDRSIZE;
  ip_arplist.enaddrsize = enaddrsize;
  ip_arplist.devname    = devname;
  ip_arplist.enaddr     = enaddr;
  ip_arplist.ipaddr     = ipaddr;
  ip_arplist.timeout    = &timeout;

  while (1) {
    sts = oz_sys_io (OZ_PROCMODE_KNL, h_ipchan, 0, OZ_IO_IP_ARPLIST, sizeof ip_arplist, &ip_arplist);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u listing arp entry\n", pn, sts);
      return (sts);
    }
    for (i = 0; i < OZ_IO_IP_ADDRSIZE; i ++) if (ipaddr[i] != 0) break;
    if (i == OZ_IO_IP_ADDRSIZE) return (OZ_SUCCESS);
    if (timeout == (uLong)(-1)) oz_sys_io_fs_printf (oz_util_h_output, "  %s  at  %s  (never)\n", cvtipbin (ipaddr, ipstr), cvtenbin (enaddrsize, enaddr, enstr));
    else oz_sys_io_fs_printf (oz_util_h_output, "  %s  at  %s  (%u ms)\n", cvtipbin (ipaddr, ipstr), cvtenbin (enaddrsize, enaddr, enstr), timeout);
  }
}

/************************************************************************/
/*									*/
/*	arp rem <device_name> <ipaddr> <enaddr>				*/
/*									*/
/************************************************************************/

uLong int_arp_rem (const char *name, void *dummy, int argc, char *argv[])

{
  OZ_IO_ip_arprem ip_arprem;
  uByte enaddr[OZ_IO_ETHER_MAXADDR], enaddrsize, ipaddr[OZ_IO_IP_ADDRSIZE];
  uLong sts;

  if (argc != 3) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: arp rem <device_name> <ipaddr> <enaddr>\n", pn);
    return (OZ_MISSINGPARAM);
  }

  enaddrsize = getenaddrsize (argv[0]);

  sts = cvtipstr (argv[1], ipaddr);
  if (sts != OZ_SUCCESS) return (sts);
  sts = cvtenstr (argv[2], enaddrsize, enaddr);
  if (sts != OZ_SUCCESS) return (sts);

  memset (&ip_arprem, 0, sizeof ip_arprem);
  ip_arprem.addrsize   = OZ_IO_IP_ADDRSIZE;
  ip_arprem.enaddrsize = enaddrsize;
  ip_arprem.devname    = argv[0];
  ip_arprem.ipaddr     = ipaddr;
  ip_arprem.enaddr     = enaddr;
  sts = knlio (OZ_IO_IP_ARPREM, sizeof ip_arprem, &ip_arprem);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u removing arp entry\n", pn, sts);
  }
  return (sts);
}

/************************************************************************/
/*									*/
/*	hw add <device_name>						*/
/*									*/
/************************************************************************/

uLong int_hw_add (const char *name, void *dummy, int argc, char *argv[])

{
  uLong sts;
  OZ_IO_ip_hwadd ip_hwadd;

  if (argc != 1) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: hw add <device_name>\n", pn);
    return (OZ_MISSINGPARAM);
  }

  memset (&ip_hwadd, 0, sizeof ip_hwadd);
  ip_hwadd.devname = argv[0];
  sts = knlio (OZ_IO_IP_HWADD, sizeof ip_hwadd, &ip_hwadd);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u adding hardware '%s'\n", pn, sts, ip_hwadd.devname);
  }
  return (sts);
}

/************************************************************************/
/*									*/
/*	debug <debug_hex_mask>						*/
/*									*/
/************************************************************************/

uLong int_debug (const char *name, void *dummy, int argc, char *argv[])

{
  int usedup;
  uLong debug, newdebug, sts;

  /* There should be exactly one parameter */

  if (argc != 1) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: debug <debug_hex_mask>\n", pn);
    return (OZ_MISSINGPARAM);
  }

  /* Decode the hexadecimal number */

  newdebug = debug = oz_hw_atoz (argv[0], &usedup);
  if (argv[0][usedup] != 0) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: bad debug hex mask %s\n", pn, argv[0]);
    return (OZ_BADPARAM);
  }

  /* Set the new bits in kernel mode, return the previous value */

  sts = oz_sys_callknl (setdebug, &debug);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u setting debug mask 0x%X\n", pn, sts, debug);
  } else {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: new mask 0x%x, old debug was 0x%X\n", pn, newdebug, debug);
  }
  return (sts);
}

static uLong setdebug (OZ_Procmode cprocmode, void *debugv)

{
  uLong *debugp, oldebug;

  debugp = debugv;		/* point to debug flags on user stack */
  oldebug = oz_dev_ip_debug;	/* save the current setting */
  oz_dev_ip_debug = *debugp;	/* set up the new setting */
  *debugp = oldebug;		/* return the old setting to user stack */
  return (OZ_SUCCESS);		/* successful */
}

/************************************************************************/
/*									*/
/*  ether dump <device_name>						*/
/*									*/
/************************************************************************/

#define EDB_INPROG_MAX 3

typedef struct Edb Edb;

/* Ether Dump Parameters */

typedef struct {
  char *ethername;
  int bufcount, opt_count, opt_data, opt_data_ip, opt_dst_quiet, opt_proto_quiet, opt_src_quiet, opt_time;
  OZ_Handle h_ether, h_event;
  OZ_IO_ether_getinfo1 ether_getinfo1;
  OZ_IO_ether_receive ether_receive;
  OZ_IO_fs_writerec fs_writerec;
  uByte dstdata[OZ_IO_ETHER_MAXADDR], dstmask[OZ_IO_ETHER_MAXADDR];
  uByte protodata[OZ_IO_ETHER_MAXPROTO], protomask[OZ_IO_ETHER_MAXPROTO];
  uByte srcdata[OZ_IO_ETHER_MAXADDR], srcmask[OZ_IO_ETHER_MAXADDR];
  volatile uLong exitsts, writeinprog;
  Edb *free_edbs;
  Edb *out_edbqh;
  Edb **out_edbqt;
} Edp;

/* Ether Dump Buffer */

struct Edb {
  Edb *next;
  Edp *edp;
  uLong rcvlen, outlen;
  uByte rcvbuf[OZ_IO_ETHER_MAXBUFF];
  char outbuf[32+8+OZ_IO_ETHER_MAXBUFF*3];
};

static void etherdump_ctrlcast (void *edpv, uLong status, OZ_Mchargs *mchargs);
static void etherdump_startrcv (Edb *edb, Edp *edp);
static void etherdump_received (void *edbv, uLong status, OZ_Mchargs *mchargs);
static void etherdump_startwrite (Edp *edp);
static void etherdump_written (void *edbv, uLong status, OZ_Mchargs *mchargs);

uLong int_ether_dump (const char *name, void *dummy, int argc, char *argv[])

{
  char *p;
  const char *em;
  Edp edp;
  int i;
  OZ_IO_console_ctrlchar ctrlc;
  OZ_IO_ether_open ether_open;
  uLong sts;

  memset (&edp, 0, sizeof edp);

  for (i = 0; i < argc; i ++) {
    if (argv[i][0] == '-') {

      /* Specify max number of packets to display */

      if (strncasecmp (argv[i], "-count:", 7) == 0) {
        edp.opt_count = atoi (argv[i] + 7);
        continue;
      }

      /* Specify max number of bytes of data to display */

      if (strncasecmp (argv[i], "-data", 5) == 0) {
        em = "bad data length";
        p = argv[i] + 5;
        edp.opt_data = OZ_IO_ETHER_MAXBUFF;
        while (*p == ':') {
          if (strncasecmp (++ p, "ip", 2) == 0) {
            edp.opt_data_ip = 1;
            p += 2;
            continue;
          }
          edp.opt_data = strtol (p, &p, 0);
        }
        if (*p != 0) goto usage;
        continue;
      }

      /* Specify destination address mask */

      if (strncasecmp (argv[i], "-dst:", 5) == 0) {
        em = "bad dst addr";
        p = argv[i] + 4;
        while (*p == ':') {
          if (strncasecmp (++ p, "quiet", 5) == 0) {
            edp.opt_dst_quiet = 1;
            p += 5;
            continue;
          }
          p = decodenhexbytes (sizeof edp.dstmask, p, edp.dstdata, edp.dstmask);
          if (p == NULL) goto usage;
        }
        if (*p != 0) goto usage;
        continue;
      }

      /* Specify protocol number mask */

      if (strncasecmp (argv[i], "-proto:", 7) == 0) {
        em = "bad proto number";
        p = argv[i] + 6;
        while (*p == ':') {
          if (strncasecmp (++ p, "quiet", 5) == 0) {
            edp.opt_proto_quiet = 1;
            p += 5;
            continue;
          }
          p = decodenhexbytes (sizeof edp.protomask, p, edp.protodata, edp.protomask);
          if (p == NULL) goto usage;
        }
        if (*p != 0) goto usage;
        continue;
      }

      /* Specify source address mask */

      if (strncasecmp (argv[i], "-src:", 5) == 0) {
        em = "bad src addr";
        p = argv[i] + 4;
        while (*p == ':') {
          if (strncasecmp (++ p, "quiet", 5) == 0) {
            edp.opt_src_quiet = 1;
            p += 5;
            continue;
          }
          p = decodenhexbytes (sizeof edp.srcmask, p, edp.srcdata, edp.srcmask);
          if (p == NULL) goto usage;
        }
        if (*p != 0) goto usage;
        continue;
      }

      /* Display time with each packet */

      if (strcasecmp (argv[i], "-time") == 0) {
        edp.opt_time = 1;
        continue;
      }
      em = "unknown option";
      goto usage;
    }

    /* The only parameter is the ethernet device name */

    em = "unknown parameter";
    if (edp.ethername != NULL) goto usage;
    edp.ethername = argv[i];
  }
  i = -1;
  em = "missing ethernet device";
  if (edp.ethername == NULL) goto usage;

  /* Assign I/O channel to ethernet device */

  sts = oz_sys_io_assign (OZ_PROCMODE_KNL, &edp.h_ether, edp.ethername, OZ_LOCKMODE_CW);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u assigning channel to %s\n", pn, sts, edp.ethername);
    return (sts);
  }

  /* Get parameters regarding its sizes */

  sts = oz_sys_io (OZ_PROCMODE_KNL, edp.h_ether, 0, OZ_IO_ETHER_GETINFO1, sizeof edp.ether_getinfo1, &edp.ether_getinfo1);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u getting ethernet %s parameters\n", pn, sts, edp.ethername);
    return (sts);
  }

  /* Open it such that we get all packets received.  But if complete proto number given, restrict to that protocol. */

  for (i = edp.ether_getinfo1.protosize; -- i >= 0;) if (edp.protomask[i] != 0xFF) break;

  memset (&ether_open, 0, sizeof ether_open);
  ether_open.promis = 1;
  if (i < 0) memcpy (ether_open.proto, edp.protodata, edp.ether_getinfo1.protosize);
  sts = oz_sys_io (OZ_PROCMODE_KNL, edp.h_ether, 0, OZ_IO_ETHER_OPEN, sizeof ether_open, &ether_open);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u opening ethernet on %s\n", pn, sts, edp.ethername);
    return (sts);
  }

  /* Start some receives going */

  edp.exitsts = OZ_PENDING;
  edp.out_edbqt = &edp.out_edbqh;

  oz_sys_thread_setast (OZ_ASTMODE_INHIBIT);
  for (sts = 0; sts < EDB_INPROG_MAX; sts ++) etherdump_startrcv (NULL, &edp);
  oz_sys_thread_setast (OZ_ASTMODE_ENABLE);

  /* Set up control-C handler */

  memset (&ctrlc, 0, sizeof ctrlc);
  ctrlc.mask[0] = 1 << ('C' - '@');
  sts = oz_sys_io_start (OZ_PROCMODE_KNL, oz_util_h_input, NULL, 0, etherdump_ctrlcast, &edp, OZ_IO_CONSOLE_CTRLCHAR, sizeof ctrlc, &ctrlc);
  if (sts != OZ_STARTED) oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u enabling ctrl-C detection\n", pn, sts);

  /* Wait for exit condition */

  while ((edp.exitsts == OZ_PENDING) || (edp.writeinprog != 0)) {
    oz_sys_event_wait (OZ_PROCMODE_KNL, edp.h_event, 0);
    oz_sys_event_set (OZ_PROCMODE_KNL, edp.h_event, 0, NULL);
  }

  /* Get number of packets we missed */

  memset (&edp.ether_getinfo1, 0, sizeof edp.ether_getinfo1);
  sts = oz_sys_io (OZ_PROCMODE_KNL, edp.h_ether, 0, OZ_IO_ETHER_GETINFO1, sizeof edp.ether_getinfo1, &edp.ether_getinfo1);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u getting missed receive count\n", pn, sts);
    edp.ether_getinfo1.rcvmissed = 0;
  }

  /* Print summary line */

  if (edp.ether_getinfo1.rcvmissed == 0) oz_sys_io_fs_printf (oz_util_h_output, "%s: %u packet%s displayed\n", 
	pn,  edp.bufcount, (edp.bufcount == 1) ? "" : "s");
  else oz_sys_io_fs_printf (oz_util_h_output, "%s: %u packet%s displayed, %u missed\n", 
	pn, edp.bufcount, (edp.bufcount == 1) ? "" : "s", edp.ether_getinfo1.rcvmissed);

  return (edp.exitsts);

usage:
  if (i < 0) oz_sys_io_fs_printf (oz_util_h_error, "%s: %s\n", pn, em);
  else oz_sys_io_fs_printf (oz_util_h_error, "%s: %s %s\n", pn, em, argv[i]);
  oz_sys_io_fs_printf (oz_util_h_error, "%s: ether dump [-count:<packets>] [-data[:<maxlen>]] [-dst[:quiet][:<dstaddr>]] [-proto[:quiet][:<protocol>]] [-src[:quiet][:<srcaddr>]] [-time] <device_name>\n", pn);
  oz_sys_io_fs_printf (oz_util_h_error, "%s:   packets = max number of packets to display, then exit\n", pn);
  oz_sys_io_fs_printf (oz_util_h_error, "%s:   maxlen  = max length of data to display per packet\n", pn);
  oz_sys_io_fs_printf (oz_util_h_error, "%s:   dstaddr, protocol, srcaddr = in form nn-nn-nn-..., where n is a hex digit or x for don't care\n", pn);
  return (OZ_MISSINGPARAM);
}

/* Control-C was pressed, set exit status and wake main loop */

static void etherdump_ctrlcast (void *edpv, uLong status, OZ_Mchargs *mchargs)

{
  Edp *edp;

  edp = edpv;
  oz_sys_io (OZ_PROCMODE_KNL, edp -> h_ether, 0, OZ_IO_ETHER_CLOSE, 0, NULL);
  edp -> exitsts = status;
  oz_sys_event_set (OZ_PROCMODE_KNL, edp -> h_event, 1, NULL);
}

/* Start a receive */

static void etherdump_startrcv (Edb *edb, Edp *edp)

{
  OZ_IO_ether_receive ether_receive;
  uLong sts;

  /* If we're terminating, don't start anything more */

  if (edp -> exitsts != OZ_PENDING) return;

  /* Allocate a buffer if we don't have one already */

  if (edb == NULL) {
    edb = edp -> free_edbs;
    if (edb != NULL) edp -> free_edbs = edb -> next;
    else {
      edb = malloc (sizeof *edb);
      edb -> edp = edp;
    }
  }

  /* Start receiving */

  edp -> ether_receive.size = sizeof edb -> rcvbuf;
  edp -> ether_receive.buff = edb -> rcvbuf;
  edp -> ether_receive.dlen = &(edb -> rcvlen);
  sts = oz_sys_io_start (OZ_PROCMODE_KNL, edp -> h_ether, NULL, 0, etherdump_received, edb, 
                         OZ_IO_ETHER_RECEIVE, sizeof edp -> ether_receive, &(edp -> ether_receive));
  if (sts != OZ_STARTED) etherdump_received (edb, sts, NULL);
}

/* We received a packet */

static void etherdump_received (void *edbv, uLong status, OZ_Mchargs *mchargs)

{
  char datebuf[32], *outpnt;
  Edb *edb;
  Edp *edp;
  int i;
  uByte *databufpnt, *dstaddrpnt, *protonopnt, *srcaddrpnt;
  uLong sts, totalen, udplen;

  edb = edbv;
  edp = edb -> edp;

  /* Check status.  Stop if error. */

  if (status != OZ_SUCCESS) {
    if (edp -> exitsts == OZ_PENDING) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u receiving on %s\n", pn, status, edp -> ethername);
      oz_sys_io (OZ_PROCMODE_KNL, edp -> h_ether, 0, OZ_IO_ETHER_CLOSE, 0, NULL);
      edp -> exitsts = status;
      oz_sys_event_set (OZ_PROCMODE_KNL, edp -> h_event, 1, NULL);
    }
    return;
  }

  /* Do packet matching on it */

  dstaddrpnt = edb -> rcvbuf + edp -> ether_getinfo1.dstaddrof;
  srcaddrpnt = edb -> rcvbuf + edp -> ether_getinfo1.srcaddrof;
  protonopnt = edb -> rcvbuf + edp -> ether_getinfo1.protooffs;
  databufpnt = edb -> rcvbuf + edp -> ether_getinfo1.dataoffset;

  for (i = 0; i < edp -> ether_getinfo1.addrsize; i ++) {
    if (((dstaddrpnt[i] ^ edp -> dstdata[i]) & edp -> dstmask[i]) != 0) goto skipacket;
    if (((srcaddrpnt[i] ^ edp -> srcdata[i]) & edp -> srcmask[i]) != 0) goto skipacket;
  }
  for (i = 0; i < edp -> ether_getinfo1.protosize; i ++) {
    if (((protonopnt[i] ^ edp -> protodata[i]) & edp -> protomask[i]) != 0) goto skipacket;
  }

  /* Start a replacement receive */

  etherdump_startrcv (NULL, edp);

  /* Format this packet's output */

#define ippkt ((OZ_IO_ip_ippkt *)databufpnt)

  outpnt = edb -> outbuf;
  if (edp -> opt_time) {
    oz_sys_datebin_decstr (0, oz_sys_datebin_tzconv (oz_hw_tod_getnow (), OZ_DATEBIN_TZCONV_UTC2LCL, 0), sizeof datebuf, datebuf);
    strcpy (outpnt, strchr (datebuf, '@') + 1);
    outpnt += strlen (outpnt);
  }
  if (!(edp -> opt_dst_quiet))   { *(outpnt ++) = ' '; outpnt = printnhexbytes (outpnt, edp -> ether_getinfo1.addrsize,  dstaddrpnt, '-'); }
  if (!(edp -> opt_src_quiet))   { *(outpnt ++) = ' '; outpnt = printnhexbytes (outpnt, edp -> ether_getinfo1.addrsize,  srcaddrpnt, '-'); }
  if (!(edp -> opt_proto_quiet)) { *(outpnt ++) = ' '; outpnt = printnhexbytes (outpnt, edp -> ether_getinfo1.protosize, protonopnt, '-'); }
  outpnt = printdecimal (outpnt, 6, edb -> rcvlen);
  if (edp -> opt_data > 0) {
    if (edb -> rcvlen > edp -> opt_data) edb -> rcvlen = edp -> opt_data;
    if (edp -> opt_data_ip && (protonopnt[0] == 0x08) && (protonopnt[1] == 0x00) && (edb -> rcvlen >= ippkt -> dat.raw - databufpnt)) {
      *(outpnt ++) = ' ';
      outpnt = printnhexbytes (outpnt, 1, &(ippkt -> hdrlenver), 0);
      *(outpnt ++) = ' ';
      outpnt = printnhexbytes (outpnt, 1, &(ippkt -> typeofserv), 0);
      *(outpnt ++) = ' ';
      outpnt = printnhexbytes (outpnt, sizeof ippkt -> totalen, ippkt -> totalen, 0);
      *(outpnt ++) = ' ';
      outpnt = printnhexbytes (outpnt, sizeof ippkt -> ident, ippkt -> ident, 0);
      *(outpnt ++) = ' ';
      outpnt = printnhexbytes (outpnt, sizeof ippkt -> flags, ippkt -> flags, 0);
      *(outpnt ++) = ' ';
      outpnt = printnhexbytes (outpnt, 1, &(ippkt -> ttl), 0);
      *(outpnt ++) = ' ';
           if (ippkt -> proto ==  1) { *(outpnt ++) = 'i'; *(outpnt ++) = 'c'; }
      else if (ippkt -> proto ==  6) { *(outpnt ++) = 't'; *(outpnt ++) = 'c'; }
      else if (ippkt -> proto == 17) { *(outpnt ++) = 'u'; *(outpnt ++) = 'd'; }
      else outpnt = printnhexbytes (outpnt, 1, &(ippkt -> proto), 0);
      *(outpnt ++) = ' ';
      outpnt = printnhexbytes (outpnt, sizeof ippkt -> hdrcksm, ippkt -> hdrcksm, 0);
      *(outpnt ++) = ' ';
      cvtipbin (ippkt -> srcipaddr, outpnt);
      for (i = strlen (outpnt); i < OZ_IO_IP_ADDRSIZE * 4 - 1; i ++) outpnt[i] = ' ';
      outpnt += i;
      *(outpnt ++) = '>';
      cvtipbin (ippkt -> dstipaddr, outpnt);
      for (i = strlen (outpnt); i < OZ_IO_IP_ADDRSIZE * 4 - 1; i ++) outpnt[i] = ' ';
      outpnt += i;
      totalen = OZ_IP_N2HW (ippkt -> totalen);
      if (edb -> rcvlen > totalen) edb -> rcvlen = totalen;
      if (edb -> rcvlen > ippkt -> dat.raw - databufpnt) {
        edb -> rcvlen -= ippkt -> dat.raw - databufpnt;
        *(outpnt ++) = ' ';
        if ((ippkt -> proto ==  1) && (edb -> rcvlen >= ippkt -> dat.icmp.raw - ippkt -> dat.raw)) {
          outpnt = printnhexbytes (outpnt, 1, &(ippkt -> dat.icmp.type), 0);
          *(outpnt ++) = ' ';
          outpnt = printnhexbytes (outpnt, 1, &(ippkt -> dat.icmp.code), 0);
          *(outpnt ++) = ' ';
          outpnt = printnhexbytes (outpnt, sizeof ippkt -> dat.icmp.cksm, ippkt -> dat.icmp.cksm, 0);
          edb -> rcvlen -= ippkt -> dat.icmp.raw - ippkt -> dat.raw;
          *(outpnt ++) = ' ';
          outpnt = printnhexbytes (outpnt, edb -> rcvlen, ippkt -> dat.icmp.raw, '-');
        }
        else if ((ippkt -> proto ==  6) && (edb -> rcvlen >= ippkt -> dat.tcp.raw - ippkt -> dat.raw)) {
          outpnt = printdecimal (outpnt, 6, OZ_IP_N2HW (ippkt -> dat.tcp.sport));
          *(outpnt ++) = '>';
          outpnt = printdecimal (outpnt, 5, OZ_IP_N2HW (ippkt -> dat.tcp.dport));
          *(outpnt ++) = ' ';
          outpnt = printnhexbytes (outpnt, sizeof ippkt -> dat.tcp.seq, ippkt -> dat.tcp.seq, 0);
          *(outpnt ++) = ' ';
          outpnt = printnhexbytes (outpnt, sizeof ippkt -> dat.tcp.ack, ippkt -> dat.tcp.ack, 0);
          *(outpnt ++) = ' ';
          outpnt = printnhexbytes (outpnt, sizeof ippkt -> dat.tcp.flags, ippkt -> dat.tcp.flags, 0);
          *(outpnt ++) = ' ';
          outpnt = printnhexbytes (outpnt, sizeof ippkt -> dat.tcp.wsize, ippkt -> dat.tcp.wsize, 0);
          *(outpnt ++) = ' ';
          outpnt = printnhexbytes (outpnt, sizeof ippkt -> dat.tcp.cksm, ippkt -> dat.tcp.cksm, 0);
          *(outpnt ++) = ' ';
          outpnt = printnhexbytes (outpnt, sizeof ippkt -> dat.tcp.urgent, ippkt -> dat.tcp.urgent, 0);
          edb -> rcvlen -= ippkt -> dat.tcp.raw - ippkt -> dat.raw;
          *(outpnt ++) = ' ';
          outpnt = printnhexbytes (outpnt, edb -> rcvlen, ippkt -> dat.tcp.raw, '-');
        }
        else if ((ippkt -> proto == 17) && (edb -> rcvlen >= ippkt -> dat.udp.raw - ippkt -> dat.raw)) {
          outpnt = printdecimal (outpnt, 6, OZ_IP_N2HW (ippkt -> dat.udp.sport));
          *(outpnt ++) = '>';
          outpnt = printdecimal (outpnt, 5, OZ_IP_N2HW (ippkt -> dat.udp.dport));
          *(outpnt ++) = ' ';
          outpnt = printdecimal (outpnt, 5, OZ_IP_N2HW (ippkt -> dat.udp.length));
          *(outpnt ++) = ' ';
          outpnt = printnhexbytes (outpnt, sizeof ippkt -> dat.udp.cksm, ippkt -> dat.udp.cksm, 0);
          edb -> rcvlen -= ippkt -> dat.udp.raw - ippkt -> dat.raw;
          udplen = OZ_IP_N2HW (ippkt -> dat.udp.length);
          if (edb -> rcvlen > udplen) edb -> rcvlen = udplen;
          *(outpnt ++) = ' ';
          outpnt = printnhexbytes (outpnt, edb -> rcvlen, ippkt -> dat.udp.raw, '-');
        }
        else outpnt = printnhexbytes (outpnt, edb -> rcvlen, ippkt -> dat.raw, '-');
      }
    } else {
      *(outpnt ++) = ' ';
      outpnt = printnhexbytes (outpnt, edb -> rcvlen, databufpnt, '-');
    }
  }
  *(outpnt ++) = '\n';

#undef ippkt

  /* Start displaying this packet's output */

  edb -> outlen = outpnt - edb -> outbuf;
  edb -> next = NULL;
  *(edp -> out_edbqt) = edb;
  edp -> out_edbqt = &(edb -> next);
  etherdump_startwrite (edp);
  return;

  /* Packet was filtered out, just start receiving into it again */

skipacket:
  etherdump_startrcv (edb, edp);
}

/* Start displaying a buffer */

static void etherdump_startwrite (Edp *edp)

{
  Edb *edb;
  uLong sts;

  if (edp -> exitsts != OZ_PENDING) return;
  if (edp -> writeinprog >= EDB_INPROG_MAX) return;

  edb = edp -> out_edbqh;
  if (edb == NULL) return;
  if ((edp -> out_edbqh = edb -> next) == NULL) edp -> out_edbqt = &(edp -> out_edbqh);

  edp -> writeinprog ++;
  edp -> fs_writerec.size = edb -> outlen;
  edp -> fs_writerec.buff = edb -> outbuf;
  sts = oz_sys_io_start (OZ_PROCMODE_KNL, oz_util_h_output, NULL, 0, etherdump_written, edb, 
                         OZ_IO_FS_WRITEREC, sizeof edp -> fs_writerec, &(edp -> fs_writerec));
  if (sts != OZ_STARTED) etherdump_written (edb, sts, NULL);
}

/* The packet has been displayed, free off the buffer and finish if we've displayed requested count */

static void etherdump_written (void *edbv, uLong status, OZ_Mchargs *mchargs)

{
  Edb *edb;
  Edp *edp;

  edb = edbv;
  edp = edb -> edp;

  /* Put buffer on free list */

  edb -> next = edp -> free_edbs;
  edp -> free_edbs = edb;

  /* One more buffer was displayed */

  edp -> bufcount ++;

  /* One less write is in progress */

  edp -> writeinprog --;

  /* If we've hit the termination count, stop */

  if ((edp -> exitsts == OZ_PENDING) && (edp -> opt_count > 0) && (edp -> bufcount >= edp -> opt_count)) {
    oz_sys_io (OZ_PROCMODE_KNL, edp -> h_ether, 0, OZ_IO_ETHER_CLOSE, 0, NULL);
    edp -> exitsts = OZ_SUCCESS;
  }

  /* Otherwise, maybe start another write */

  else etherdump_startwrite (edp);

  /* If all finished, wake main loop */

  if ((edp -> exitsts != OZ_PENDING) && (edp -> writeinprog == 0)) {
    oz_sys_event_set (OZ_PROCMODE_KNL, edp -> h_event, 1, NULL);
  }
}

/* Decode 'n' hex databytes from the command line */

static char *decodenhexbytes (uLong n, char *s, uByte *outdata, uByte *outmask)

{
  char c;
  uByte databyte, maskbyte;

  databyte = 0;					// start off with no data
  maskbyte = 0;					// start off with no valid bits
  while (1) {
    c = *(s ++);				// get input character
    if ((c == 'x') || (c == 'X')) {		// see if 'dont care' indicator
      if (maskbyte > 15) return (NULL);		// if so, make sure mask not overflowed by too many digits
      databyte <<= 4;				// shift data over
      maskbyte <<= 4;				// shift mask over, leave low bits clear for 'dont care'
      continue;
    }
    if ((c >= '0') && (c <= '9')) {		// see if decimal digit
      if (maskbyte > 15) return (NULL);		// if so, make sure mask not overflowed by too many digits
      databyte <<= 4;				// shift data over
      databyte  += c - '0';			// insert new digit
      maskbyte <<= 4;				// shift mask over
      maskbyte  += 15;				// set all bits so it's a 'do care'
      continue;
    }
    if ((c >= 'A') && (c <= 'F')) {		// see if extended hex digit
      if (maskbyte > 15) return (NULL);		// if so, make sure mask not overflowed by too many digits
      databyte <<= 4;				// shift data over
      databyte  += c - 'A' + 10;		// insert new digit
      maskbyte <<= 4;				// shift mask over
      maskbyte  += 15;				// set all bits so it's a 'do care'
      continue;
    }
    if ((c >= 'a') && (c <= 'f')) {		// see if extended hex digit
      if (maskbyte > 15) return (NULL);		// if so, make sure mask not overflowed by too many digits
      databyte <<= 4;				// shift data over
      databyte  += c - 'a' + 10;		// insert new digit
      maskbyte <<= 4;				// shift mask over
      maskbyte  += 15;				// set all bits so it's a 'do care'
      continue;
    }

    if (n == 0) return (NULL);			// got a whole byte, make sure we didn't overflow
    -- n;
    *(outdata ++) = databyte;			// ok, store the data byte
    *(outmask ++) = maskbyte;			// store the mask byte
    databyte = 0;				// reset both for next time through loop
    maskbyte = 0;
    if (c != '-') break;			// continue if it's a hyphen
  }
  memset (outmask, 0, n);			// all done, pad mask with 'dont care'
  return (-- s);				// return pointer to terminator character
}

/* Format a decimal number for output */

static char *printdecimal (char *outpnt, uLong w, uLong value)

{
  char tmp[16];
  int i;

  i = 0;				// no output digits formatted yet
  do {					// output at least one digit
    tmp[i++] = (value % 10) + '0';	// output the least sig digit
    value /= 10;			// shift it out
  } while (value != 0);			// repeat if there are any digits left
  while (w > i) {			// pad output if needed
    *(outpnt ++) = ' ';
    -- w;
  }
  while (i > 0) *(outpnt ++) = tmp[--i]; // output the digits in proper order
  return (outpnt);			// return revised output pointer
}

/* Format 'n' hex bytes for output */

static char *printnhexbytes (char *outpnt, uLong n, uByte *bytes, char sep)

{
  char c;
  uByte b;

  c = 0;				// start with no separator
  while (n > 0) {			// repeat while there's input to process
    -- n;
    b = *(bytes ++);			// get a databyte
    if (c != 0) *(outpnt ++) = c;	// output separator
    *(outpnt ++) = hextab[b/16];	// output the two hex digits
    *(outpnt ++) = hextab[b%16];
    c = sep;				// change separator for next loop
  }
  return (outpnt);			// return updated output pointer
}

/************************************************************************/
/*									*/
/*  Filter command tables						*/
/*									*/
/************************************************************************/

#define FILTER_LISTNAMES 3
static const struct { OZ_IO_ip_filter_listid listid; const char *listname; } filter_listnames[FILTER_LISTNAMES] = {
                              OZ_IO_IP_FILTER_INPUT, "input", 
                            OZ_IO_IP_FILTER_FORWARD, "forward", 
                             OZ_IO_IP_FILTER_OUTPUT, "output" };

#define FILTER_ACTNAMES 2
static const struct { uLong action; const char *actname; } filter_actnames[FILTER_ACTNAMES] = {
                                 0, "deny", 
     OZ_IO_IP_FILTER_ACTION_ACCEPT, "accept" };

static OZ_IO_ip_ippkt const filter_termpkt;

static int filter_addterm_ipaddr  (int index, int argc, char *argv[], int *argi, uLong *offset_r, uLong *mask_r, uLong *data_r);
static int filter_addterm_proto   (int index, int argc, char *argv[], int *argi, uLong *offset_r, uLong *mask_r, uLong *data_r);
static int filter_addterm_icmp    (int index, int argc, char *argv[], int *argi, uLong *offset_r, uLong *mask_r, uLong *data_r);
static int filter_addterm_udpport (int index, int argc, char *argv[], int *argi, uLong *offset_r, uLong *mask_r, uLong *data_r);
static int filter_addterm_tcpport (int index, int argc, char *argv[], int *argi, uLong *offset_r, uLong *mask_r, uLong *data_r);
static int filter_addterm_tcpflag (int index, int argc, char *argv[], int *argi, uLong *offset_r, uLong *mask_r, uLong *data_r);

static void filter_listterm_ipaddr  (int index, uLong mask, uLong offset, uByte *filterdata, uByte *filtermask);
static void filter_listterm_proto   (int index, uLong mask, uLong offset, uByte *filterdata, uByte *filtermask);
static void filter_listterm_icmp    (int index, uLong mask, uLong offset, uByte *filterdata, uByte *filtermask);
static void filter_listterm_udpport (int index, uLong mask, uLong offset, uByte *filterdata, uByte *filtermask);
static void filter_listterm_tcpport (int index, uLong mask, uLong offset, uByte *filterdata, uByte *filtermask);
static void filter_listterm_tcpflag (int index, uLong mask, uLong offset, uByte *filterdata, uByte *filtermask);

#define FILTER_TERMS 13
static const struct { uByte const *field;
                      uLong mask;
                      uByte proto;
                      int (*add) (int index, int argc, char *argv[], int *argi, uLong *offset_r, uLong *mask_r, uLong *data_r);
                      void (*list) (int index, uLong mask, uLong offset, uByte *filterdata, uByte *filtermask);
                      const char *termname;
                      const char *termhelp;
                    } filter_termnames[FILTER_TERMS] = {
                 filter_termpkt.srcipaddr,                  0,  0, filter_addterm_ipaddr,  filter_listterm_ipaddr,  "srcipaddr",  " <ipaddress>[/<maskbitcount>]'  source ip address", 
                 filter_termpkt.dstipaddr,                  0,  0, filter_addterm_ipaddr,  filter_listterm_ipaddr,  "dstipaddr",  " <ipaddress>[/<maskbitcount>]'  destination ip address", 
           (uByte *)&filter_termpkt.proto,               0xFF,  0, filter_addterm_proto,   filter_listterm_proto,   "proto",      " <ip_protocol_number>'  ip protocol number", 

   (uByte *)&filter_termpkt.dat.icmp.type,               0xFF,  1, filter_addterm_icmp,    filter_listterm_icmp,    "icmptype",   " <number>'  icmp type number", 
   (uByte *)&filter_termpkt.dat.icmp.code,               0xFF,  1, filter_addterm_icmp,    filter_listterm_icmp,    "icmpcode",   " <number>'  icmp code number", 

             filter_termpkt.dat.udp.sport,             0xFFFF, 17, filter_addterm_udpport, filter_listterm_udpport, "udpsport",   " <number>'  udp source port number", 
             filter_termpkt.dat.udp.dport,             0xFFFF, 17, filter_addterm_udpport, filter_listterm_udpport, "udpdport",   " <number>'  udp destination port number", 

             filter_termpkt.dat.tcp.sport,             0xFFFF,  6, filter_addterm_tcpport, filter_listterm_tcpport, "tcpsport",   " <number>'  tcp source port number", 
             filter_termpkt.dat.tcp.dport,             0xFFFF,  6, filter_addterm_tcpport, filter_listterm_tcpport, "tcpdport",   " <number>'  tcp destination port number",
             filter_termpkt.dat.tcp.flags, WS (TCP_FLAGS_FIN),  6, filter_addterm_tcpflag, filter_listterm_tcpflag, "tcpflagfin", " [not]'  tcp FIN (close) flag", 
             filter_termpkt.dat.tcp.flags, WS (TCP_FLAGS_SYN),  6, filter_addterm_tcpflag, filter_listterm_tcpflag, "tcpflagsyn", " [not]'  tcp SYN (connect) flag", 
             filter_termpkt.dat.tcp.flags, WS (TCP_FLAGS_RST),  6, filter_addterm_tcpflag, filter_listterm_tcpflag, "tcpflagrst", " [not]'  tcp RST (reset) flag", 
             filter_termpkt.dat.tcp.flags, WS (TCP_FLAGS_ACK),  6, filter_addterm_tcpflag, filter_listterm_tcpflag, "tcpflagack", " [not]'  tcp ACK (acknowlege) flag" };

/************************************************************************/
/*									*/
/*	filter add <listname> <entryindex> <action> [trace] <terms...>	*/
/*									*/
/************************************************************************/

uLong int_filter_add (const char *name, void *dummy, int argc, char *argv[])

{
  int i, j, k, savei;
  OZ_IO_ip_filteradd ip_filteradd;
  uByte filterdata[OZ_IO_ETHER_MAXDATA], filtermask[OZ_IO_ETHER_MAXDATA];
  uLong data, mask, offset, sts;

  if (argc < 3) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: filter add <listname> <entryindex> <action> <terms...>\n", pn);
    return (OZ_MISSINGPARAM);
  }

  memset (&ip_filteradd, 0, sizeof ip_filteradd);

  /* Decode list name */

  for (i = 0; i < FILTER_LISTNAMES; i ++) {
    if (strcasecmp (argv[0], filter_listnames[i].listname) == 0) goto gotlistname;
  }
  oz_sys_io_fs_printf (oz_util_h_error, "%s: invalid filter listname %s\n", pn, argv[0]);
  for (i = 0; i < FILTER_LISTNAMES; i ++) {
    oz_sys_io_fs_printf (oz_util_h_error, "	%s\n", filter_listnames[i].listname);
  }
  return (OZ_BADPARAM);
gotlistname:
  ip_filteradd.listid = filter_listnames[i].listid;

  /* Decode entry index */

  ip_filteradd.index = oz_hw_atoi (argv[1], &i);
  if ((argv[1][i] != 0) || (ip_filteradd.index < 0)) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: invalid filter index %s, must be integer >= 0\n", pn, argv[1]);
    return (OZ_BADPARAM);
  }

  /* Decode action name */

  for (i = 0; i < FILTER_ACTNAMES; i ++) {
    if (strcasecmp (argv[2], filter_actnames[i].actname) == 0) goto gotactname;
  }
  oz_sys_io_fs_printf (oz_util_h_error, "%s: invalid filter action %s\n", pn, argv[2]);
  for (i = 0; i < FILTER_ACTNAMES; i ++) {
    oz_sys_io_fs_printf (oz_util_h_error, "	%s\n", filter_actnames[i].actname);
  }
  return (OZ_BADPARAM);
gotactname:
  ip_filteradd.action = filter_actnames[i].action;
  i = 3;
  if ((i < argc) && (strcasecmp (argv[i], "trace") == 0)) {
    ip_filteradd.action |= OZ_IO_IP_FILTER_ACTION_TRACE;
    i ++;
  }

  /* Decode terms into the filterdata/filtermask arrays */

  memset (filterdata, 0, sizeof filterdata);			// start out with all zeroes ...
  memset (filtermask, 0, sizeof filtermask);			// ... meaning anything will match

  for (; i < argc; i ++) {					// loop through all the term arguments
    savei = i;							// save index at beginning of term
    if (argv[i][0] == '(') {					// check for (offset&mask=data)
      offset = oz_hw_atoz (argv[i] + 1, &j);			//   ok, get the offset
      if (argv[i][1+j] != '&') goto badterm;			//   must end with '&'
      mask = oz_hw_atoz (argv[i] + 1 + j, &k);			//   get the mask
      j += k;
      if (argv[i][1+j] != '=') goto badterm;			//   must end with '='
      data = oz_hw_atoz (argv[i] + 1 + j, &k);			//   get the data
      j += k;
      if (argv[i][1+j] != ')') goto badterm;			//   must end with ')' and nothing more
      if (argv[i][2+j] != 0) goto badterm;
    } else {
      for (j = 0; j < FILTER_TERMS; j ++) {			// check for term by name
        if (strcasecmp (argv[i], filter_termnames[j].termname) == 0) goto gottermname;
      }
      goto badterm;
gottermname:
      offset = filter_termnames[j].field - (uByte *)&filter_termpkt; // ok, save given offset
      mask   = filter_termnames[j].mask;			// save given mask
      data   = 0;						// clear out data
      if (!(*(filter_termnames[j].add)) (j, argc, argv, &i, &offset, &mask, &data)) goto badterm;
      if (filter_termnames[j].proto != 0) {			// see if a specific ip protocol is required for this term
        if (((OZ_IO_ip_ippkt *)filtermask) -> proto == 0) {	// if so, force checking for it if they haven't already
          ((OZ_IO_ip_ippkt *)filterdata) -> proto = filter_termnames[j].proto;
          ((OZ_IO_ip_ippkt *)filtermask) -> proto = 0xFF;
        }
        else if ((((OZ_IO_ip_ippkt *)filtermask) -> proto != 0xFF) // ... or make sure they are checking for correct one
              || (((OZ_IO_ip_ippkt *)filterdata) -> proto != filter_termnames[j].proto)) {
          oz_sys_io_fs_printf (oz_util_h_error, "%s: using %s requires protocol %u, not %u\n", pn, argv[savei], filter_termnames[j].proto, ((OZ_IO_ip_ippkt *)filterdata) -> proto);
          return (OZ_BADPARAM);
        }
      }
    }
    while (mask != 0) {						// either way, merge given offset,mask,data into buffer
      if (offset >= sizeof filtermask) goto badterm;		//   must not overflow packets
      if ((filtermask[offset] & mask & (filterdata[offset] ^ data)) != 0) goto duplicate;
      filterdata[offset] |= data;
      filtermask[offset] |= mask;
      offset ++;
      data >>= 8;
      mask >>= 8;
    }
  }

  /* Perform the I/O request to add the filter */

  ip_filteradd.size = sizeof filterdata;
  ip_filteradd.data = (OZ_IO_ip_ippkt *)filterdata;
  ip_filteradd.mask = (OZ_IO_ip_ippkt *)filtermask;

  sts = knlio (OZ_IO_IP_FILTERADD, sizeof ip_filteradd, &ip_filteradd);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u adding filter %d\n", pn, sts, ip_filteradd.index);
  }
  return (sts);

  /* A bad data/mask term was given, say which one then print out the help */

badterm:
  oz_sys_io_fs_printf (oz_util_h_error, "%s: bad term %s\n", pn, argv[i]);
  oz_sys_io_fs_printf (oz_util_h_error, "	'(offset&mask=value)'  to specify an arbitrary ip packet offset, mask and value (hexadecimal, host order)\n");
  for (i = 0; i < FILTER_TERMS; i ++) {
    oz_sys_io_fs_printf (oz_util_h_error, "	'%s%s\n", filter_termnames[i].termname, filter_termnames[i].termhelp);
  }
  return (OZ_BADPARAM);

duplicate:
  oz_sys_io_fs_printf (oz_util_h_error, "%s: duplicate term %s\n", pn, argv[savei]);
  return (OZ_BADPARAM);
}

/******************************/
/* Decode ip address and mask */
/******************************/

static int filter_addterm_ipaddr (int index, int argc, char *argv[], int *argi, uLong *offset_r, uLong *mask_r, uLong *data_r)

{
  char *p;
  int i, j, k;
  uByte ipbin[OZ_IO_IP_ADDRSIZE];
  uLong sts;

  i = *argi;					// must have an ip address
  if (++ i >= argc) return (0);			// fail if end of arg list
  p = strchr (argv[i], '/');			// see if it has a '/' for the mask
  if (p != NULL) *(p ++) = 0;			// if so, null terminate ip address part
  sts = cvtipstr (argv[i], ipbin);		// convert to binary
  if (sts != OZ_SUCCESS) return (0);		// fail if not successful
  for (j = 0; j < OZ_IO_IP_ADDRSIZE; j ++) {
    *data_r |= ((uLong)(ipbin[j])) << (8 * j);
  }
  *mask_r = -1;					// assume a mask of /32
  if (p != NULL) {				// see if a /mask provided
    j = oz_hw_atoi (p, &k);			// if so, decode it
    if (p[k] != 0) return (0);			// must be just a number
    if (j > 32) return (0);			// and .le. 32
    sts = (-1 << (32 - j));			// ok, make the bitmask
    *mask_r = 0;
    while (sts != 0) {
      *mask_r <<= 8;
      *mask_r  |= sts & 0xFF;
      sts     >>= 8;
    }
  }
  *argi = i;
  return (1);					// successful
}

/*****************************/
/* Decode ip protocol number */
/*****************************/

static int filter_addterm_proto (int index, int argc, char *argv[], int *argi, uLong *offset_r, uLong *mask_r, uLong *data_r)

{
  int i, j;

  i = *argi;				// get argument index
  if (++ i >= argc) return (0);		// must have a number following
  *data_r = oz_hw_atoi (argv[i], &j);	// decode the protocol number
  if (argv[i][j] != 0) return (0);	// must be a valid number
  if (*data_r > 0xFF) return (0);	// must fit in a byte
  *argi = i;				// increment over the number
  return (1);				// successful
}

/**********************************/
/* Decode icmp message type value */
/**********************************/

static int filter_addterm_icmp (int index, int argc, char *argv[], int *argi, uLong *offset_r, uLong *mask_r, uLong *data_r)

{
  int i, j;

  i = *argi;				// get argument index
  if (++ i >= argc) return (0);		// must have a number following
  *data_r = oz_hw_atoi (argv[i], &j);	// decode the message type number
  if (argv[i][j] != 0) return (0);	// must be a valid number
  if (*data_r > 0xFF) return (0);	// must fit in a byte
  *argi = i;				// increment over the number
  return (1);				// successful
}

/**********************************/
/* Decode UDP and TCP port number */
/**********************************/

static int filter_addterm_udpport (int index, int argc, char *argv[], int *argi, uLong *offset_r, uLong *mask_r, uLong *data_r)

{
  int i, j;
  uWord port;

  i = *argi;				// get argument index
  if (++ i >= argc) return (0);		// must have a number following
  port = oz_hw_atoi (argv[i], &j);	// decode the port number
  if (argv[i][j] != 0) return (0);	// must be a valid number
  if (port > 0xFFFF) return (0);	// must fit in a word
  *data_r = WS (port);			// return the word in network order
  *argi   = i;				// increment over the number
  return (1);				// successful
}

static int filter_addterm_tcpport (int index, int argc, char *argv[], int *argi, uLong *offset_r, uLong *mask_r, uLong *data_r)

{
  int i, j;
  uWord port;

  i = *argi;				// get argument index
  if (++ i >= argc) return (0);		// must have a number following
  port = oz_hw_atoi (argv[i], &j);	// decode the port number
  if (argv[i][j] != 0) return (0);	// must be a valid number
  if (port > 0xFFFF) return (0);	// must fit in a word
  *data_r = WS (port);			// return the word in network order
  *argi   = i;				// increment over the number
  return (1);				// successful
}

/**********************************************/
/* TCP flag bit (optionally followed by 'not' */
/**********************************************/

static int filter_addterm_tcpflag (int index, int argc, char *argv[], int *argi, uLong *offset_r, uLong *mask_r, uLong *data_r)

{
  int i;

  *data_r = *mask_r;						// assume the flag bit has to be set to match filter
  i = *argi;							// get argument index
  if ((++ i < argc) && (strcasecmp (argv[i], "not") == 0)) {	// see if next word is 'not'
    *data_r = 0;						// if so, the flag bit must be clear to match filter
    *argi = i;							// increment over the 'not'
  }
  return (1);							// either way, we're successful
}

/************************************************************************/
/*									*/
/*	filter list <listname>						*/
/*									*/
/************************************************************************/

uLong int_filter_list (const char *name, void *dummy, int argc, char *argv[])

{
  int i, j, k;
  uLong action;
  OZ_IO_ip_filterlist ip_filterlist;
  uByte filterdata[OZ_IO_ETHER_MAXDATA], filtermask[OZ_IO_ETHER_MAXDATA], proto;
  uLong data, mask, offset, sts;

  if (argc != 1) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: filter list <listname>\n", pn);
    return (OZ_MISSINGPARAM);
  }

  memset (&ip_filterlist, 0, sizeof ip_filterlist);

  /* Decode list name */

  for (i = 0; i < FILTER_LISTNAMES; i ++) {
    if (strcasecmp (argv[0], filter_listnames[i].listname) == 0) goto gotlistname;
  }
  oz_sys_io_fs_printf (oz_util_h_error, "%s: invalid filter listname %s\n", pn, argv[0]);
  for (i = 0; i < FILTER_LISTNAMES; i ++) {
    oz_sys_io_fs_printf (oz_util_h_error, "	%s\n", filter_listnames[i].listname);
  }
  return (OZ_BADPARAM);
gotlistname:
  ip_filterlist.listid = filter_listnames[i].listid;

  /* Loop through all indicies until we get an error */

  oz_sys_io_fs_printf (oz_util_h_output, "%s: list of %s filters:\n", pn, filter_listnames[i].listname);

  ip_filterlist.action_r = &action;
  ip_filterlist.size     = sizeof filtermask;
  ip_filterlist.data     = (OZ_IO_ip_ippkt *)filterdata;
  ip_filterlist.mask     = (OZ_IO_ip_ippkt *)filtermask;

  for (ip_filterlist.index = 0;; ip_filterlist.index ++) {
    sts = knlio (OZ_IO_IP_FILTERLIST, sizeof ip_filterlist, &ip_filterlist);
    if (sts != OZ_SUCCESS) break;

    oz_sys_io_fs_printf (oz_util_h_output, "%6d: %s%s", ip_filterlist.index, 
                         action & OZ_IO_IP_FILTER_ACTION_ACCEPT ? "accept" : "deny", 
                         action & OZ_IO_IP_FILTER_ACTION_TRACE  ? " trace" : "");

    /* Decode terms from the filterdata/filtermask arrays */

    proto = 0xFF;
    if (((OZ_IO_ip_ippkt *)filtermask) -> proto == 0xFF) proto = ((OZ_IO_ip_ippkt *)filterdata) -> proto;
    for (i = 0; i < FILTER_TERMS; i ++) {
      if ((filter_termnames[i].proto != 0) && (proto != filter_termnames[i].proto)) continue; // don't use it if filter test for another protocol
      offset = filter_termnames[i].field - (uByte *)&filter_termpkt;		// ok, get offset of data
      mask   = filter_termnames[i].mask;					// get the data mask
      while (mask != 0) {							// filter must test for all mask bits to use this term
        if ((filtermask[offset] & mask & 0xFF) != (mask & 0xFF)) goto nextterm;
        mask >>= 8;
        offset ++;
      }
      offset = filter_termnames[i].field - (uByte *)&filter_termpkt;		// ok, list out this term
      mask   = filter_termnames[i].mask;
      (*(filter_termnames[i].list)) (i, mask, offset, filterdata, filtermask);
      while (mask != 0) {							// clear mask bits so we don't use catch-all on it later
        filtermask[offset] &= ~mask;
        mask >>= 8;
        offset ++;
      }
nextterm:
    }
    for (i = 0; i < sizeof filtermask; i ++) {					// scan for any non-standard term bits
      if (filtermask[i] != 0) {							// look for a non-zero mask byte
        mask = 0;								// accumulate non-zero mask bytes
        data = 0;
        for (j = 0; (j < 4) && (i + j < sizeof filtermask); j ++) {		// ... up to four in a row
          if (filtermask[i+j] == 0) break;					//   stop when we hit a zero mask byte
          mask |= (uLong)(filtermask[i+j]) << (8 * j);				//   accumulate mask byte
          data |= (uLong)(filterdata[i+j]) << (8 * j);				//   accumulate corresponding data byte
          filtermask[i+j] = 0;							//   clear mask so we don't do it again
        }
        oz_sys_io_fs_printf (oz_util_h_output, "  (%X&%X=%X)", i, mask, data);	// print it out
      }
    }
    oz_sys_io_fs_printf (oz_util_h_output, "\n");
  }

  if (sts == OZ_SUBSCRIPT) { oz_sys_io_fs_printf (oz_util_h_output, "%s: end of list\n", pn); sts = OZ_SUCCESS; }
  else oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u reading filter list\n", pn, sts);
  return (sts);
}

static void filter_listterm_ipaddr  (int index, uLong mask, uLong offset, uByte *filterdata, uByte *filtermask)

{
  int i, j, nbits;
  uByte maskbyte;

  /* Count number of continuous 1 bits out of the mask bits            */
  /* The 1 bits must be followed by continuous 0's to fill the 32 bits */

  nbits = 0;					// no 1-bits found yet
  for (i = 0; i < OZ_IO_IP_ADDRSIZE; i ++) {	// loop through 32 bits
    maskbyte = filtermask[offset+i];		// get a byte of mask bits
    if ((nbits & 7) && (maskbyte != 0)) return;	// if we found 0's in prev byte, this must be all zeroes
    for (j = 8; -- j >= 0;) {			// check each bit
      if (!(maskbyte & 0x80)) break;		// if clear, done counting 1's
      nbits ++;					// if set, another one
      maskbyte <<= 1;				// shift for next bit test
    }
    if (maskbyte != 0) return;			// found a zero bit, the rest must be zeroes
  }

  /* If no 1-bits found, there is nothing to print here */

  if (nbits == 0) return;

  /* Ok, output the field and clear the mask bits */

  oz_sys_io_fs_printf (oz_util_h_output, "  %s", filter_termnames[index].termname);
  for (i = 0; i < OZ_IO_IP_ADDRSIZE; i ++) {
    oz_sys_io_fs_printf (oz_util_h_output, "%c%u", (i == 0) ? ' ' : '.', filterdata[offset+i]);
    filtermask[offset+i] = 0;
  }
  if (nbits != 32) oz_sys_io_fs_printf (oz_util_h_output, "/%d", nbits);
}

static void filter_listterm_proto   (int index, uLong mask, uLong offset, uByte *filterdata, uByte *filtermask)

{
  oz_sys_io_fs_printf (oz_util_h_output, "  %s %u", filter_termnames[index].termname, filterdata[offset]);
  filtermask[offset] = 0;
}

static void filter_listterm_icmp    (int index, uLong mask, uLong offset, uByte *filterdata, uByte *filtermask)

{
  oz_sys_io_fs_printf (oz_util_h_output, "  %s %u", filter_termnames[index].termname, filterdata[offset]);
  filtermask[offset] = 0;
}

static void filter_listterm_udpport (int index, uLong mask, uLong offset, uByte *filterdata, uByte *filtermask)

{
  uWord port;

  port   = filterdata[offset+0];
  port <<= 8;
  port  |= filterdata[offset+1];
  oz_sys_io_fs_printf (oz_util_h_output, "  %s %u", filter_termnames[index].termname, port);
  filtermask[offset+0] = 0;
  filtermask[offset+1] = 0;
}

static void filter_listterm_tcpport (int index, uLong mask, uLong offset, uByte *filterdata, uByte *filtermask)

{
  uWord port;

  port   = filterdata[offset+0];
  port <<= 8;
  port  |= filterdata[offset+1];
  oz_sys_io_fs_printf (oz_util_h_output, "  %s %u", filter_termnames[index].termname, port);
  filtermask[offset+0] = 0;
  filtermask[offset+1] = 0;
}

static void filter_listterm_tcpflag (int index, uLong mask, uLong offset, uByte *filterdata, uByte *filtermask)

{
  oz_sys_io_fs_printf (oz_util_h_output, "  %s%s", filter_termnames[index].termname, (*(uWord *)(filterdata + offset) & *(uWord *)(filtermask + offset)) ? "" : " not");
}

/************************************************************************/
/*									*/
/*	filter rem <listname> <entryindex>				*/
/*									*/
/************************************************************************/

uLong int_filter_rem (const char *name, void *dummy, int argc, char *argv[])

{
  int i;
  OZ_IO_ip_filterrem ip_filterrem;
  uLong sts;

  if (argc != 2) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: filter rem <listname> <entryindex>\n", pn);
    return (OZ_MISSINGPARAM);
  }

  memset (&ip_filterrem, 0, sizeof ip_filterrem);

  /* Decode list name */

  for (i = 0; i < FILTER_LISTNAMES; i ++) {
    if (strcasecmp (argv[0], filter_listnames[i].listname) == 0) goto gotlistname;
  }
  oz_sys_io_fs_printf (oz_util_h_error, "%s: invalid filter listname %s\n", pn, argv[0]);
  for (i = 0; i < FILTER_LISTNAMES; i ++) {
    oz_sys_io_fs_printf (oz_util_h_error, "	%s\n", filter_listnames[i].listname);
  }
  return (OZ_BADPARAM);
gotlistname:
  ip_filterrem.listid = filter_listnames[i].listid;

  /* Decode entry index */

  ip_filterrem.index = oz_hw_atoi (argv[1], &i);
  if ((argv[1][i] != 0) || (ip_filterrem.index < 0)) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: invalid filter index %s, must be integer >= 0\n", pn, argv[1]);
  }

  /* Perform the I/O request to remove the filter */

  sts = knlio (OZ_IO_IP_FILTERREM, sizeof ip_filterrem, &ip_filterrem);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u removing filter %d\n", pn, sts, ip_filterrem.index);
  }
  return (sts);
}

/************************************************************************/
/*									*/
/*	hw ipam add <device_name> <hwipaddr> [<nwipaddr>] <nwipmask>	*/
/*									*/
/************************************************************************/

uLong int_hw_ipam_add (const char *name, void *dummy, int argc, char *argv[])

{
  uByte hwipaddr[OZ_IO_IP_ADDRSIZE], nwipaddr[OZ_IO_IP_ADDRSIZE], nwipmask[OZ_IO_IP_ADDRSIZE];
  uLong sts;
  OZ_IO_ip_hwipamadd ip_hwipamadd;

  if ((argc != 3) && (argc != 4)) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: hw ipam add <device_name> <hwipaddr> [<nwipaddr>] <nwipmask>\n", pn);
    return (OZ_MISSINGPARAM);
  }

  sts = cvtipstr (argv[1], hwipaddr);
  if (sts != OZ_SUCCESS) return (sts);
  if (argc == 3) {
    memcpy (nwipaddr, hwipaddr, OZ_IO_IP_ADDRSIZE);
    sts = cvtipstr (argv[2], nwipmask);
    if (sts != OZ_SUCCESS) return (sts);
  }
  if (argc == 4) {
    sts = cvtipstr (argv[2], nwipaddr);
    if (sts != OZ_SUCCESS) return (sts);
    sts = cvtipstr (argv[3], nwipmask);
    if (sts != OZ_SUCCESS) return (sts);
  }

  memset (&ip_hwipamadd, 0, sizeof ip_hwipamadd);
  ip_hwipamadd.addrsize = OZ_IO_IP_ADDRSIZE;
  ip_hwipamadd.devname  = argv[0];
  ip_hwipamadd.hwipaddr = hwipaddr;
  ip_hwipamadd.nwipaddr = nwipaddr;
  ip_hwipamadd.nwipmask = nwipmask;
  sts = knlio (OZ_IO_IP_HWIPAMADD, sizeof ip_hwipamadd, &ip_hwipamadd);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u adding hardware ipam\n", pn, sts);
  }
  return (sts);
}

/************************************************************************/
/*									*/
/*	hw ipam rem <device_name> <hwipaddr> [<nwipaddr>]		*/
/*									*/
/************************************************************************/

uLong int_hw_ipam_rem (const char *name, void *dummy, int argc, char *argv[])

{
  uByte hwipaddr[OZ_IO_IP_ADDRSIZE], nwipaddr[OZ_IO_IP_ADDRSIZE];
  uLong sts;
  OZ_IO_ip_hwipamrem ip_hwipamrem;

  if ((argc != 2) && (argc != 3)) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: hw ipam rem <device_name> [<hwipaddr>] [<nwipaddr>]\n", pn);
    return (OZ_MISSINGPARAM);
  }

  sts = cvtipstr (argv[1], hwipaddr);
  if (sts != OZ_SUCCESS) return (sts);
  if (argc == 2) memcpy (nwipaddr, hwipaddr, OZ_IO_IP_ADDRSIZE);
  if (argc == 3) {
    sts = cvtipstr (argv[2], nwipaddr);
    if (sts != OZ_SUCCESS) return (sts);
  }

  memset (&ip_hwipamrem, 0, sizeof ip_hwipamrem);
  ip_hwipamrem.devname  = argv[0];
  ip_hwipamrem.hwipaddr = hwipaddr;
  ip_hwipamrem.nwipaddr = nwipaddr;
  sts = knlio (OZ_IO_IP_HWIPAMREM, sizeof ip_hwipamrem, &ip_hwipamrem);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u removing hardware ipam\n", pn, sts);
  }
  return (sts);
}

/************************************************************************/
/*									*/
/*	hw list								*/
/*									*/
/************************************************************************/

uLong int_hw_list (const char *name, void *dummy, int argc, char *argv[])

{
  uByte hwipaddr[OZ_IO_IP_ADDRSIZE], nwipaddr[OZ_IO_IP_ADDRSIZE], nwipmask[OZ_IO_IP_ADDRSIZE];
  char buf1[OZ_IO_IP_ADDRSIZE*4], buf2[OZ_IO_IP_ADDRSIZE*4], buf3[OZ_IO_IP_ADDRSIZE*4], devname[OZ_DEVUNIT_NAMESIZE];
  int i;
  uLong sts;
  OZ_IO_ip_hwipamlist ip_hwipamlist;
  OZ_IO_ip_hwlist ip_hwlist;

  if (argc != 0) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: hw list\n", pn);
    return (OZ_MISSINGPARAM);
  }

  devname[0] = 0;
  memset (&ip_hwlist, 0, sizeof ip_hwlist);
  memset (&ip_hwipamlist, 0, sizeof ip_hwipamlist);
  ip_hwlist.devnamesize  = sizeof devname;
  ip_hwlist.devnamebuff  = devname;
  ip_hwipamlist.addrsize = OZ_IO_IP_ADDRSIZE;
  ip_hwipamlist.devname  = devname;
  ip_hwipamlist.hwipaddr = hwipaddr;
  ip_hwipamlist.nwipaddr = nwipaddr;
  ip_hwipamlist.nwipmask = nwipmask;

  while (1) {

    /* Get next device name in list */

    sts = oz_sys_io (OZ_PROCMODE_KNL, h_ipchan, 0, OZ_IO_IP_HWLIST, sizeof ip_hwlist, &ip_hwlist);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u listing hardware\n", pn, sts);
      return (sts);
    }

    /* If null string, we're done */

    if (devname[0] == 0) break;

    /* Ok, print it out */

    oz_sys_io_fs_printf (oz_util_h_output, "%s:\n", devname);

    /* Then print out all ipams for it */

    memset (hwipaddr, 0, sizeof hwipaddr);
    memset (nwipaddr, 0, sizeof nwipaddr);
    memset (nwipmask, 0, sizeof nwipmask);
    while (1) {
      sts = oz_sys_io (OZ_PROCMODE_KNL, h_ipchan, 0, OZ_IO_IP_HWIPAMLIST, sizeof ip_hwipamlist, &ip_hwipamlist);
      if (sts != OZ_SUCCESS) {
        oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u listing hardware ipams\n", pn, sts);
        return (sts);
      }
      for (i = 0; i < sizeof hwipaddr; i ++) if (hwipaddr[i] != 0) break;
      if (i == sizeof hwipaddr) break;
      oz_sys_io_fs_printf (oz_util_h_output, "  %s network %s mask %s\n", cvtipbin (hwipaddr, buf1), cvtipbin (nwipaddr, buf2), cvtipbin (nwipmask, buf3));
    }
  }

  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*	hw rem <device_name>						*/
/*									*/
/************************************************************************/

uLong int_hw_rem (const char *name, void *dummy, int argc, char *argv[])

{
  uLong sts;
  OZ_IO_ip_hwrem ip_hwrem;

  if (argc != 1) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: hw rem <device_name>\n", pn);
    return (OZ_MISSINGPARAM);
  }

  memset (&ip_hwrem, 0, sizeof ip_hwrem);
  ip_hwrem.devname = argv[0];
  sts = knlio (OZ_IO_IP_HWREM, sizeof ip_hwrem, &ip_hwrem);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u removing hardware '%s'\n", pn, sts, ip_hwrem.devname);
  }
  return (sts);
}

/************************************************************************/
/*									*/
/*	ping [-count <count>] [-length <ippacketlength>] <ip_address>	*/
/*									*/
/************************************************************************/

#define PROTO_IP_ICMP 1
#define PROTO_IP_ICMP_PINGRPL 0
#define PROTO_IP_ICMP_PINGREQ 8

typedef struct { OZ_Datebin sent;	/* date/time ping request was sent */
                 uLong seq;		/* sequence number being sent */
                 OZ_Processid pid;	/* my process id */
               } Pingbuf;

static OZ_Processid pingpid;

static int ping_size;
static uLong ping_nsends, ping_nrecvs;
static OZ_Datebin ping_avgtime, ping_maxtime, ping_mintime;
static uLong start_ping_rcv (void);
static void ping_receive_ast (void *rcvpktv, uLong status, OZ_Mchargs *mchargs);

uLong int_ping (const char *name, void *dummy, int argc, char *argv[])

{
  char *ipaddrstr, ipstr[OZ_IO_IP_ADDRSIZE*4];
  int i, xmtlen;
  OZ_Datebin now;
  OZ_Handle h_timer;
  OZ_IO_console_ctrlchar ctrlc;
  OZ_IO_ip_ipbind ip_ipbind;
  OZ_IO_ip_ippkt *xmtpkt;
  OZ_IO_ip_iptransmit ip_iptransmit;
  OZ_IO_timer_waituntil timer_waituntil;
  uByte ipaddr[OZ_IO_IP_ADDRSIZE];
  uLong count, seq, sts;
  uWord icmpcksm;
  volatile uLong ctrlcsts;

  /* Check number of args and get target IP address */

  ipaddrstr = NULL;
  ping_size = 1;
  count     = -1;
  for (i = 0; i < argc; i ++) {
    if (argv[i][0] == '-') {
      if (strcasecmp (argv[i], "-count") == 0) {
        if (++ i >= argc) goto usage;
        count = atoi (argv[i]);
        continue;
      }
      if (strcasecmp (argv[i], "-length") == 0) {
        if (++ i >= argc) goto usage;
        ping_size = atoi (argv[i]);
        if (ping_size < 1) ping_size = 1;
        continue;
      }
      goto usage;
    }
    if (ipaddrstr != NULL) goto usage;
    ipaddrstr = argv[i];
  }
  if (ipaddrstr == NULL) goto usage;
  sts = cvtipstr (ipaddrstr, ipaddr);
  if (sts != OZ_SUCCESS) return (sts);

  /* Get process id to uniquely mark the packets */

  sts = oz_sys_process_getid (OZ_PROCMODE_KNL, 0, &pingpid);
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);

  /* Assign a timer channel */

  sts = oz_sys_io_assign (OZ_PROCMODE_KNL, &h_timer, "timer", OZ_LOCKMODE_CW);
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);

  /* Bind the IP channel to receive incoming ICMP packets from the target IP address.  Tell driver to pass them on in case it isn't for us. */

  memset (&ip_ipbind, 0, sizeof ip_ipbind);
  ip_ipbind.addrsize  = OZ_IO_IP_ADDRSIZE;
  ip_ipbind.remipaddr = ipaddr;
  ip_ipbind.passiton  = 1;
  ip_ipbind.proto     = PROTO_IP_ICMP;
  sts = knlio (OZ_IO_IP_IPBIND, sizeof ip_ipbind, &ip_ipbind);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u binding ip port\n", pn, sts);
    return (sts);
  }

  memset (&ip_iptransmit, 0, sizeof ip_iptransmit);
  memset (&timer_waituntil, 0, sizeof timer_waituntil);

  /* Allocate and fill in transmit buffer */

  xmtlen = (uByte *)xmtpkt + ping_size - xmtpkt -> dat.icmp.raw; // length of the icmp part

  if (xmtlen < (int)(sizeof (Pingbuf))) {			// make sure enough room for our Pingbuf stuff
    xmtlen = sizeof (Pingbuf);
    ping_size = xmtpkt -> dat.icmp.raw + xmtlen - (uByte *)xmtpkt;
  }

  oz_sys_io_fs_printf (oz_util_h_error, "%s: pinging %s, ip packet length %d, icmp length %d\n", 
	pn, cvtipbin (ipaddr, ipstr), ping_size, xmtlen);

  xmtpkt = malloc (ping_size);					// allocate a packet
  memset (xmtpkt, 0, ping_size);				// zero it out

  ip_iptransmit.pktsize = ping_size;
  ip_iptransmit.ippkt   = xmtpkt;

  for (seq = 0; seq < xmtlen; seq ++) {				// fill it with non-zero bytes
    xmtpkt -> dat.icmp.raw[seq] = seq;
  }

  xmtpkt -> hdrlenver  = 0x45;					// ip header length and version
  OZ_IP_H2NW (ping_size, xmtpkt -> totalen);			// total ip packet length
  xmtpkt -> ttl        = 255;					// time-to-live = max allowed
  xmtpkt -> proto      = PROTO_IP_ICMP;				// icmp packet
  memset (xmtpkt -> srcipaddr, 0, sizeof xmtpkt -> srcipaddr);	// where it's coming from = let ip driver fill this in
  memcpy (xmtpkt -> dstipaddr, ipaddr, sizeof xmtpkt -> dstipaddr); // where it's going to
  xmtpkt -> dat.icmp.type = PROTO_IP_ICMP_PINGREQ;		// it's a ping request
  ((Pingbuf *)(xmtpkt -> dat.icmp.raw)) -> pid = pingpid;	// set our unique marker in case this computer is doing a ping somewhere else

  /* Set up control-C handler */

  ctrlcsts = OZ_PENDING;
  memset (&ctrlc, 0, sizeof ctrlc);
  ctrlc.mask[0] = 1 << ('C' - '@');
  sts = oz_sys_io_start (OZ_PROCMODE_KNL, oz_util_h_input, &ctrlcsts, 0, NULL, NULL, OZ_IO_CONSOLE_CTRLCHAR, sizeof ctrlc, &ctrlc);
  if ((sts != OZ_STARTED) && ((sts != OZ_BADIOFUNC) || (count == (uLong)(-1)))) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u enabling ctrl-C detection\n", pn, sts);
  }

  /* Start 5 receives */

  for (i = 0; i < 5; i ++) {
    sts = start_ping_rcv ();
    if (sts != OZ_SUCCESS) return (sts);
  }

  /* Send out packets */

  ping_nsends = 0;
  ping_nrecvs = 0;
  OZ_HW_DATEBIN_CLR (ping_maxtime);
  OZ_HW_DATEBIN_CLR (ping_mintime);
  OZ_HW_DATEBIN_CLR (ping_avgtime);

  for (seq = 0; (seq < count) && ((ctrlcsts == OZ_PENDING) || (ctrlcsts == OZ_BADIOFUNC)); seq ++) {
    ((Pingbuf *)(xmtpkt -> dat.icmp.raw)) -> seq  = seq;				// set up new sequence
    ((Pingbuf *)(xmtpkt -> dat.icmp.raw)) -> sent = oz_hw_tod_getnow ();		// timestamp it
    memset (xmtpkt -> hdrcksm, 0, sizeof xmtpkt -> hdrcksm);				// compute ICMP checksum
    memset (xmtpkt -> dat.icmp.cksm, 0, sizeof xmtpkt -> dat.icmp.cksm);
    icmpcksm = oz_dev_ip_icmpcksm (xmtpkt);
    OZ_IP_H2NW (icmpcksm, xmtpkt -> dat.icmp.cksm);
    sts = knlio (OZ_IO_IP_IPTRANSMIT, sizeof ip_iptransmit, &ip_iptransmit);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u sending ping packet\n", pn, sts);
      return (sts);
    }
    ping_nsends ++;
    OZ_HW_DATEBIN_ADD (timer_waituntil.datebin, ((Pingbuf *)(xmtpkt -> dat.icmp.raw)) -> sent, OZ_TIMER_RESOLUTION);
    sts = oz_sys_io (OZ_PROCMODE_KNL, h_timer, 0, OZ_IO_TIMER_WAITUNTIL, sizeof timer_waituntil, &timer_waituntil);
    if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
  }

  if (ping_nrecvs != 0) ping_avgtime /= ping_nrecvs;
  oz_sys_io_fs_printf (oz_util_h_output, "%s: %u sent, %u rcvd, min/avg/max %#t/%#t/%#t\n", pn, ping_nsends, ping_nrecvs, ping_mintime, ping_avgtime, ping_maxtime);

  return (OZ_SUCCESS);

usage:
  oz_sys_io_fs_printf (oz_util_h_error, "%s: ping [-count <count>] [-length <ippacketlength>] <ip_address>\n", pn);
  return (OZ_MISSINGPARAM);
}

/* This routine is called to start receiving an ICMP packet */

static uLong start_ping_rcv (void)

{
  uLong rcvsiz, sts;
  OZ_IO_ip_ippkt *rcvpkt;
  OZ_IO_ip_ipreceive ip_ipreceive;

  rcvpkt = malloc (ping_size);

  ip_ipreceive.addrsize = OZ_IO_IP_ADDRSIZE;
  ip_ipreceive.pktsize  = ping_size;
  ip_ipreceive.ippkt    = rcvpkt;

  sts = oz_sys_io_start (OZ_PROCMODE_KNL, h_ipchan, NULL, 0, ping_receive_ast, rcvpkt, OZ_IO_IP_IPRECEIVE, sizeof ip_ipreceive, &ip_ipreceive);
  if (sts == OZ_SUCCESS) ping_receive_ast (rcvpkt, OZ_SUCCESS, NULL);
  else if (sts == OZ_STARTED) sts = OZ_SUCCESS;
  else oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u starting ping receive\n", pn, sts);

  return (sts);
}

static void ping_receive_ast (void *rcvpktv, uLong status, OZ_Mchargs *mchargs)

{
  char ipstr[OZ_IO_IP_ADDRSIZE*4];
  int i, j;
  OZ_Datebin delta, now;
  OZ_IO_ip_ippkt *rcvpkt;

  now = oz_hw_tod_getnow ();

  rcvpkt = rcvpktv;

  /* Start a replacement read */

  start_ping_rcv ();

  /* Print out results */

  if ((status != OZ_SUCCESS) && (status != OZ_BUFFEROVF)) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u receiving ping reply\n", pn, status);
  } else if ((rcvpkt -> dat.icmp.type == PROTO_IP_ICMP_PINGRPL) && ((Pingbuf *)(rcvpkt -> dat.icmp.raw)) -> pid == pingpid) {
    OZ_HW_DATEBIN_SUB (delta, now, ((Pingbuf *)(rcvpkt -> dat.icmp.raw)) -> sent);
    oz_sys_io_fs_printf (oz_util_h_output, "%s  seq %6u  ttl %3u  time %#t\n", 
	cvtipbin (rcvpkt -> srcipaddr, ipstr), ((Pingbuf *)(rcvpkt -> dat.icmp.raw)) -> seq, rcvpkt -> ttl, delta);
    if (ping_nrecvs == 0) ping_avgtime = ping_mintime = ping_maxtime = delta;
    else {
      if (OZ_HW_DATEBIN_CMP (delta, ping_mintime) < 0) ping_mintime = delta;
      if (OZ_HW_DATEBIN_CMP (delta, ping_maxtime) > 0) ping_maxtime = delta;
      OZ_HW_DATEBIN_ADD (ping_avgtime, ping_avgtime, delta);
    }
    ping_nrecvs ++;
  }

  /* Free off packet */

  free (rcvpkt);
}

/************************************************************************/
/*									*/
/*	route add <ip_addr> <ip_mask> <gw_ipad>				*/
/*									*/
/************************************************************************/

uLong int_route_add (const char *name, void *dummy, int argc, char *argv[])

{
  uByte gwipaddr[OZ_IO_IP_ADDRSIZE], nwipaddr[OZ_IO_IP_ADDRSIZE], nwipmask[OZ_IO_IP_ADDRSIZE];
  uLong sts;
  OZ_IO_ip_routeadd ip_routeadd;

  if (argc != 3) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: route add <nwipaddr> <nwipmask> <gwipaddr>\n", pn);
    return (OZ_MISSINGPARAM);
  }

  sts = cvtipstr (argv[0], nwipaddr);
  if (sts != OZ_SUCCESS) return (sts);
  sts = cvtipstr (argv[1], nwipmask);
  if (sts != OZ_SUCCESS) return (sts);
  sts = cvtipstr (argv[2], gwipaddr);
  if (sts != OZ_SUCCESS) return (sts);

  memset (&ip_routeadd, 0, sizeof ip_routeadd);
  ip_routeadd.addrsize = OZ_IO_IP_ADDRSIZE;
  ip_routeadd.nwipaddr = nwipaddr;
  ip_routeadd.nwipmask = nwipmask;
  ip_routeadd.gwipaddr = gwipaddr;
  sts = knlio (OZ_IO_IP_ROUTEADD, sizeof ip_routeadd, &ip_routeadd);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u adding route\n", pn, sts);
  }
  return (sts);
}

/************************************************************************/
/*									*/
/*	route list							*/
/*									*/
/************************************************************************/

uLong int_route_list (const char *name, void *dummy, int argc, char *argv[])

{
  uByte gwipaddr[OZ_IO_IP_ADDRSIZE], hwipaddr[OZ_IO_IP_ADDRSIZE], nwipaddr[OZ_IO_IP_ADDRSIZE], nwipmask[OZ_IO_IP_ADDRSIZE];
  char buf1[OZ_IO_IP_ADDRSIZE*4], buf2[OZ_IO_IP_ADDRSIZE*4], buf3[OZ_IO_IP_ADDRSIZE*4], devname[OZ_DEVUNIT_NAMESIZE];
  int i;
  uLong sts;
  OZ_IO_ip_routelist ip_routelist;

  if (argc != 0) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: route list\n", pn);
    return (OZ_MISSINGPARAM);
  }

  memset (&ip_routelist, 0, sizeof ip_routelist);
  ip_routelist.addrsize = OZ_IO_IP_ADDRSIZE;
  ip_routelist.nwipaddr = nwipaddr;
  ip_routelist.nwipmask = nwipmask;
  ip_routelist.gwipaddr = gwipaddr;
  ip_routelist.devnamesize = sizeof devname;
  ip_routelist.devnamebuff = devname;
  ip_routelist.hwipaddr = hwipaddr;

  memset (nwipaddr, 0, sizeof nwipaddr);
  memset (nwipmask, 0, sizeof nwipmask);
  memset (gwipaddr, 0, sizeof gwipaddr);

  while (1) {
    sts = oz_sys_io (OZ_PROCMODE_KNL, h_ipchan, 0, OZ_IO_IP_ROUTELIST, sizeof ip_routelist, &ip_routelist);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u listing routes\n", pn, sts);
      return (sts);
    }
    for (i = 0; i < sizeof gwipaddr; i ++) if (gwipaddr[i] != 0) break;
    if (i == sizeof gwipaddr) break;
    oz_sys_io_fs_printf (oz_util_h_output, "  gateway %s network %s mask %s\n", cvtipbin (gwipaddr, buf1), cvtipbin (nwipaddr, buf2), cvtipbin (nwipmask, buf3));
    if (devname[0] == 0) oz_sys_io_fs_printf (oz_util_h_output, "    (not reachable)\n");
    else oz_sys_io_fs_printf (oz_util_h_output, "    (via %s at %s)\n", devname, cvtipbin (hwipaddr, buf1));
  }

  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*	route rem <nwipaddr> <nwipmask> <gwipaddr>			*/
/*									*/
/************************************************************************/

uLong int_route_rem (const char *name, void *dummy, int argc, char *argv[])

{
  uByte gwipaddr[OZ_IO_IP_ADDRSIZE], nwipaddr[OZ_IO_IP_ADDRSIZE], nwipmask[OZ_IO_IP_ADDRSIZE];
  uLong sts;
  OZ_IO_ip_routerem ip_routerem;

  if (argc != 3) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: route rem <nwipaddr> <nwipmask> <gwipaddr>\n", pn);
    return (OZ_MISSINGPARAM);
  }

  sts = cvtipstr (argv[0], nwipaddr);
  if (sts != OZ_SUCCESS) return (sts);
  sts = cvtipstr (argv[1], nwipmask);
  if (sts != OZ_SUCCESS) return (sts);
  sts = cvtipstr (argv[2], gwipaddr);
  if (sts != OZ_SUCCESS) return (sts);

  memset (&ip_routerem, 0, sizeof ip_routerem);
  ip_routerem.nwipaddr = nwipaddr;
  ip_routerem.nwipmask = nwipmask;
  ip_routerem.gwipaddr = gwipaddr;
  sts = knlio (OZ_IO_IP_ROUTEREM, sizeof ip_routerem, &ip_routerem);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u removing route\n", pn, sts);
  }
  return (sts);
}

/************************************************************************/
/*									*/
/*	dns lookup <name>						*/
/*									*/
/************************************************************************/

uLong int_dns_lookup (const char *name, void *dummy, int argc, char *argv[])

{
  char buf1[OZ_IO_IP_ADDRSIZE*4];
  OZ_IO_ip_dnslookup ip_dnslookup;
  uByte array[OZ_IO_IP_ADDRSIZE*OZ_IO_IP_DNSNUMMAX];
  uLong i, numel, sts;

  if (argc != 1) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: dns lookup <name>\n", pn);
    return (OZ_MISSINGPARAM);
  }

  memset (&ip_dnslookup, 0, sizeof ip_dnslookup);
  ip_dnslookup.name    = argv[0];
  ip_dnslookup.elsiz   = OZ_IO_IP_ADDRSIZE;
  ip_dnslookup.maxel   = OZ_IO_IP_DNSNUMMAX;
  ip_dnslookup.array   = array;
  ip_dnslookup.numel_r = &numel;
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_ipchan, 0, OZ_IO_IP_DNSLOOKUP, sizeof ip_dnslookup, &ip_dnslookup);
  if (sts != OZ_SUCCESS) oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u looking up name\n", pn, sts);
  else {
    for (i = 0; i < numel; i ++) {
      oz_sys_io_fs_printf (oz_util_h_output, "  %s\n", cvtipbin (array + i * OZ_IO_IP_ADDRSIZE, buf1));
    }
  }
  return (sts);
}

/************************************************************************/
/*									*/
/*	dns server add <ip_addr> <port_no>				*/
/*									*/
/************************************************************************/

uLong int_dns_svradd (const char *name, void *dummy, int argc, char *argv[])

{
  uByte ipaddr[OZ_IO_IP_ADDRSIZE], portno[OZ_IO_IP_PORTSIZE];
  uLong sts;
  OZ_IO_ip_dnssvradd ip_dnssvradd;

  if (argc != 2) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: dns server add <ip_addr> <port_no>\n", pn);
    return (OZ_MISSINGPARAM);
  }

  sts = cvtipstr (argv[0], ipaddr);
  if (sts != OZ_SUCCESS) return (sts);
  sts = cvtpnstr (argv[1], portno);
  if (sts != OZ_SUCCESS) return (sts);

  memset (&ip_dnssvradd, 0, sizeof ip_dnssvradd);
  ip_dnssvradd.addrsize = OZ_IO_IP_ADDRSIZE;
  ip_dnssvradd.portsize = OZ_IO_IP_PORTSIZE;
  ip_dnssvradd.ipaddr = ipaddr;
  ip_dnssvradd.portno = portno;
  sts = knlio (OZ_IO_IP_DNSSVRADD, sizeof ip_dnssvradd, &ip_dnssvradd);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u adding dns server\n", pn, sts);
  }
  return (sts);
}

/************************************************************************/
/*									*/
/*	dns server list							*/
/*									*/
/************************************************************************/

uLong int_dns_svrlist (const char *name, void *dummy, int argc, char *argv[])

{
  uByte ipaddr[OZ_IO_IP_ADDRSIZE], portno[OZ_IO_IP_PORTSIZE];
  char buf1[OZ_IO_IP_ADDRSIZE*4], buf2[OZ_IO_IP_PORTSIZE*4];
  int i;
  uLong sts;
  OZ_IO_ip_dnssvrlist ip_dnssvrlist;

  if (argc != 0) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: dns server list\n", pn);
    return (OZ_MISSINGPARAM);
  }

  memset (&ip_dnssvrlist, 0, sizeof ip_dnssvrlist);
  ip_dnssvrlist.addrsize = OZ_IO_IP_ADDRSIZE;
  ip_dnssvrlist.portsize = OZ_IO_IP_PORTSIZE;
  ip_dnssvrlist.ipaddr   = ipaddr;
  ip_dnssvrlist.portno   = portno;

  memset (ipaddr, 0, sizeof ipaddr);
  memset (portno, 0, sizeof portno);

  while (1) {
    sts = oz_sys_io (OZ_PROCMODE_KNL, h_ipchan, 0, OZ_IO_IP_DNSSVRLIST, sizeof ip_dnssvrlist, &ip_dnssvrlist);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u listing dns servers\n", pn, sts);
      return (sts);
    }
    for (i = 0; i < sizeof ipaddr; i ++) if (ipaddr[i] != 0) break;
    if (i == sizeof ipaddr) break;
    oz_sys_io_fs_printf (oz_util_h_output, "  %s:%s\n", cvtipbin (ipaddr, buf1), cvtpnbin (portno, buf2));
  }

  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*	dns server rem <ip_addr> <port_no>				*/
/*									*/
/************************************************************************/

uLong int_dns_svrrem (const char *name, void *dummy, int argc, char *argv[])

{
  uByte ipaddr[OZ_IO_IP_ADDRSIZE], portno[OZ_IO_IP_PORTSIZE];
  uLong sts;
  OZ_IO_ip_dnssvrrem ip_dnssvrrem;

  if (argc != 2) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: dns server rem <ip_addr> <port_no>\n", pn);
    return (OZ_MISSINGPARAM);
  }

  sts = cvtipstr (argv[0], ipaddr);
  if (sts != OZ_SUCCESS) return (sts);
  sts = cvtpnstr (argv[1], portno);
  if (sts != OZ_SUCCESS) return (sts);

  memset (&ip_dnssvrrem, 0, sizeof ip_dnssvrrem);
  ip_dnssvrrem.addrsize = OZ_IO_IP_ADDRSIZE;
  ip_dnssvrrem.portsize = OZ_IO_IP_PORTSIZE;
  ip_dnssvrrem.ipaddr = ipaddr;
  ip_dnssvrrem.portno = portno;
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_ipchan, 0, OZ_IO_IP_DNSSVRREM, sizeof ip_dnssvrrem, &ip_dnssvrrem);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u removing dns server\n", pn, sts);
  }
  return (sts);
}

/************************************************************************/
/*									*/
/*  Print out the status of stuff					*/
/*									*/
/************************************************************************/

uLong int_status (const char *name, void *dummy, int argc, char *argv[])

{
  char ipstr1[OZ_IO_IP_ADDRSIZE*4], ipstr2[OZ_IO_IP_ADDRSIZE*4];
  char pnstr1[OZ_IO_IP_PORTSIZE*4], pnstr2[OZ_IO_IP_PORTSIZE*4];
  OZ_Handle h_lastiochan, h_nextiochan, h_saveipchan;
  OZ_IO_ip_ipgetinfo1  ip_ipgetinfo1;
  OZ_IO_ip_udpgetinfo1 ip_udpgetinfo1;
  OZ_IO_ip_tcpgetinfo1 ip_tcpgetinfo1;
  uByte ip_lclipaddr[OZ_IO_IP_ADDRSIZE],  ip_remipaddr[OZ_IO_IP_ADDRSIZE];
  uByte udp_lclipaddr[OZ_IO_IP_ADDRSIZE], udp_lclportno[OZ_IO_IP_PORTSIZE], udp_remipaddr[OZ_IO_IP_ADDRSIZE], udp_remportno[OZ_IO_IP_PORTSIZE];
  uByte tcp_lclipaddr[OZ_IO_IP_ADDRSIZE], tcp_lclportno[OZ_IO_IP_PORTSIZE], tcp_remipaddr[OZ_IO_IP_ADDRSIZE], tcp_remportno[OZ_IO_IP_PORTSIZE];
  uLong index, ipsts, sts, tcpsts, udpsts;
  void *objaddr;

  OZ_Handle_item i_items[2] = { OZ_HANDLE_CODE_IOCHAN_FIRST, sizeof h_nextiochan, &h_nextiochan, NULL, 
                                OZ_HANDLE_CODE_OBJADDR, sizeof objaddr, &objaddr, NULL };

  memset (&ip_ipgetinfo1,  0, sizeof ip_ipgetinfo1);
  memset (&ip_udpgetinfo1, 0, sizeof ip_udpgetinfo1);
  memset (&ip_tcpgetinfo1, 0, sizeof ip_tcpgetinfo1);

  ip_ipgetinfo1.addrsize  = OZ_IO_IP_ADDRSIZE;
  ip_ipgetinfo1.lclipaddr = ip_lclipaddr;
  ip_ipgetinfo1.remipaddr = ip_remipaddr;

  ip_udpgetinfo1.addrsize  = OZ_IO_IP_ADDRSIZE;
  ip_udpgetinfo1.portsize  = OZ_IO_IP_PORTSIZE;
  ip_udpgetinfo1.lclipaddr = udp_lclipaddr;
  ip_udpgetinfo1.lclportno = udp_lclportno;
  ip_udpgetinfo1.remipaddr = udp_remipaddr;
  ip_udpgetinfo1.remportno = udp_remportno;

  ip_tcpgetinfo1.addrsize  = OZ_IO_IP_ADDRSIZE;
  ip_tcpgetinfo1.portsize  = OZ_IO_IP_PORTSIZE;
  ip_tcpgetinfo1.lclipaddr = tcp_lclipaddr;
  ip_tcpgetinfo1.lclportno = tcp_lclportno;
  ip_tcpgetinfo1.remipaddr = tcp_remipaddr;
  ip_tcpgetinfo1.remportno = tcp_remportno;

  sts = oz_sys_handle_getinfo (h_ipchan, 1, i_items, &index);
  i_items[0].code = OZ_HANDLE_CODE_IOCHAN_NEXT;
  while ((sts == OZ_SUCCESS) && (h_nextiochan != 0)) {
    h_lastiochan = h_nextiochan;
    sts = oz_sys_handle_getinfo (h_lastiochan, 2, i_items, &index);
    if (sts != OZ_SUCCESS) break;

    oz_sys_io_fs_printf (oz_util_h_output, "\nchannel %p:\n", objaddr);

    h_saveipchan = h_ipchan;
    h_ipchan = h_lastiochan;
    ipsts  = knlio (OZ_IO_IP_IPGETINFO1,  sizeof ip_ipgetinfo1,  &ip_ipgetinfo1);
    udpsts = knlio (OZ_IO_IP_UDPGETINFO1, sizeof ip_udpgetinfo1, &ip_udpgetinfo1);
    tcpsts = knlio (OZ_IO_IP_TCPGETINFO1, sizeof ip_tcpgetinfo1, &ip_tcpgetinfo1);
    h_ipchan = h_saveipchan;

    if (ipsts != OZ_SUCCESS) oz_sys_io_fs_printf (oz_util_h_output, "  ipsts %u\n", ipsts);
    else if ((ip_lclipaddr[0] != 0) || (ip_remipaddr[0] != 0) || (ip_ipgetinfo1.recvpend != -1) || (ip_ipgetinfo1.recvcount != 0) || (ip_ipgetinfo1.transcount != 0)) {
      oz_sys_io_fs_printf (oz_util_h_output, "  ip threadid %u:\n", ip_ipgetinfo1.threadid);
      oz_sys_io_fs_printf (oz_util_h_output, "    recvcount %u, transcount %u\n", ip_ipgetinfo1.recvcount, ip_ipgetinfo1.transcount);
      oz_sys_io_fs_printf (oz_util_h_output, "    lclipaddr %s,  remipaddr %s\n", cvtipbin (ip_lclipaddr, ipstr1), cvtipbin (ip_remipaddr, ipstr2));
      oz_sys_io_fs_printf (oz_util_h_output, "    proto %u, passiton %d, recvpend %d\n", ip_ipgetinfo1.proto, ip_ipgetinfo1.passiton, ip_ipgetinfo1.recvpend);
    }

    if (udpsts != OZ_SUCCESS) oz_sys_io_fs_printf (oz_util_h_output, "  udpsts %u\n", udpsts);
    else if ((udp_lclipaddr[0] != 0) || (udp_remipaddr[0] != 0) || (ip_udpgetinfo1.recvpend != -1) || (ip_udpgetinfo1.recvcount != 0) || (ip_udpgetinfo1.transcount != 0)) {
      oz_sys_io_fs_printf (oz_util_h_output, "  udp threadid %u:\n", ip_udpgetinfo1.threadid);
      oz_sys_io_fs_printf (oz_util_h_output, "    recvcount %u, transcount %u\n", ip_udpgetinfo1.recvcount, ip_udpgetinfo1.transcount);
      oz_sys_io_fs_printf (oz_util_h_output, "    lclipaddr %s.%s,  remipaddr %s.%s\n", cvtipbin (udp_lclipaddr, ipstr1), cvtpnbin (udp_lclportno, pnstr1), cvtipbin (udp_remipaddr, ipstr2), cvtpnbin (udp_remportno, pnstr2));
      oz_sys_io_fs_printf (oz_util_h_output, "    recvpend %d\n", ip_udpgetinfo1.recvpend);
    }

    if (tcpsts != OZ_SUCCESS) oz_sys_io_fs_printf (oz_util_h_output, "  tcpsts %u\n", tcpsts);
    else if ((tcp_lclipaddr[0] != 0) || (tcp_remipaddr[0] != 0) || (ip_tcpgetinfo1.recvpend != -1) || (ip_tcpgetinfo1.recvcount != 0) || (ip_tcpgetinfo1.transcount != 0)) {
      oz_sys_io_fs_printf (oz_util_h_output, "  tcp threadid %u:\n", ip_tcpgetinfo1.threadid);
      oz_sys_io_fs_printf (oz_util_h_output, "    recvcount %u, transcount %u\n", ip_tcpgetinfo1.recvcount, ip_tcpgetinfo1.transcount);
      oz_sys_io_fs_printf (oz_util_h_output, "    lclipaddr %s.%s,  remipaddr %s.%s\n", cvtipbin (tcp_lclipaddr, ipstr1), cvtpnbin (tcp_lclportno, pnstr1), cvtipbin (tcp_remipaddr, ipstr2), cvtpnbin (tcp_remportno, pnstr2));
      oz_sys_io_fs_printf (oz_util_h_output, "    recvpend %d, transpend %d\n", ip_tcpgetinfo1.recvpend, ip_tcpgetinfo1.transpend);
      oz_sys_io_fs_printf (oz_util_h_output, "    state");
      if (ip_tcpgetinfo1.state & OZ_IO_IP_TCPSTATE_CLOSED)  oz_sys_io_fs_printf (oz_util_h_output, " CLOSED");
      if (ip_tcpgetinfo1.state & OZ_IO_IP_TCPSTATE_RESET)   oz_sys_io_fs_printf (oz_util_h_output, " RESET");
      if (ip_tcpgetinfo1.state & OZ_IO_IP_TCPSTATE_SENTSYN) oz_sys_io_fs_printf (oz_util_h_output, " SENTSYN");
      if (ip_tcpgetinfo1.state & OZ_IO_IP_TCPSTATE_SENTFIN) oz_sys_io_fs_printf (oz_util_h_output, " SENTFIN");
      if (ip_tcpgetinfo1.state & OZ_IO_IP_TCPSTATE_SENTRST) oz_sys_io_fs_printf (oz_util_h_output, " SENTRST");
      if (ip_tcpgetinfo1.state & OZ_IO_IP_TCPSTATE_RCVDSYN) oz_sys_io_fs_printf (oz_util_h_output, " RCVDSYN");
      if (ip_tcpgetinfo1.state & OZ_IO_IP_TCPSTATE_RCVDFIN) oz_sys_io_fs_printf (oz_util_h_output, " RCVDFIN");
      oz_sys_io_fs_printf (oz_util_h_output, "\n");
      oz_sys_io_fs_printf (oz_util_h_output, "    rcvwindowsize %u, buff %p, processid %u\n", ip_tcpgetinfo1.rcvwindowsize, ip_tcpgetinfo1.rcvwindowbuff, ip_tcpgetinfo1.rcvwindowpid);
      oz_sys_io_fs_printf (oz_util_h_output, "    rcvwindowrem %u, rcvwindownxt %u, rcvwindowins %u\n", ip_tcpgetinfo1.rcvwindowrem, ip_tcpgetinfo1.rcvwindownxt, ip_tcpgetinfo1.rcvwindowins);
      oz_sys_io_fs_printf (oz_util_h_output, "    maxsendsize %u, lastwsizesent %u\n", ip_tcpgetinfo1.maxsendsize, ip_tcpgetinfo1.lastwsizesent);
      oz_sys_io_fs_printf (oz_util_h_output, "    lastacksent %u, receivedok %u\n", ip_tcpgetinfo1.seq_lastacksent, ip_tcpgetinfo1.seq_receivedok);
      oz_sys_io_fs_printf (oz_util_h_output, "    lastrcvdack %u, nexttransmit %u\n", ip_tcpgetinfo1.seq_lastrcvdack, ip_tcpgetinfo1.seq_nexttransmit);
      oz_sys_io_fs_printf (oz_util_h_output, "    lastrcvdwsize %u, nextuserdata %u\n", ip_tcpgetinfo1.seq_lastrcvdwsize, ip_tcpgetinfo1.seq_nextuserdata);
    }
    oz_sys_handle_release (OZ_PROCMODE_KNL, h_lastiochan);
  }
}

/************************************************************************/
/*									*/
/*  Perform an I/O from kernel mode as required by the driver for 	*/
/*  protection from silly people					*/
/*									*/
/************************************************************************/

typedef struct { uLong funcode;
                 uLong as;
                 void *ap;
                 volatile uLong status;
               } Knlioprm;

static uLong knlio (uLong funcode, uLong as, void *ap)

{
  Knlioprm knlioprm;
  uLong sts;

  knlioprm.funcode = funcode;
  knlioprm.as      = as;
  knlioprm.ap      = ap;
  knlioprm.status  = OZ_PENDING;
  sts = oz_sys_callknl (knlioknl, &knlioprm);
  if (sts == OZ_STARTED) {
    while ((sts = knlioprm.status) == OZ_PENDING) {
      oz_sys_event_wait (OZ_PROCMODE_KNL, h_ioevent, 0);
      oz_sys_event_set (OZ_PROCMODE_KNL, h_ioevent, 0, NULL);
    }
  }
  return (sts);
}

static uLong knlioknl (OZ_Procmode cprocmode, void *knlioprmv)

{
  Knlioprm *knlioprm;

  knlioprm = knlioprmv;
  return (oz_sys_io_start (OZ_PROCMODE_KNL, h_ipchan, &(knlioprm -> status), h_ioevent, NULL, NULL, 
                           knlioprm -> funcode, knlioprm -> as, knlioprm -> ap));
}

/************************************************************************/
/*									*/
/*  Convert ip string to binary						*/
/*									*/
/************************************************************************/

static uLong cvtipstr (char *ipstr, uByte ipbin[OZ_IO_IP_ADDRSIZE])

{
  uLong sts;

  sts = oz_sys_gethostipaddr (ipstr, OZ_IO_IP_ADDRSIZE, ipbin);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u converting ip address or mask %s\n", pn, sts, ipstr);
  }
  return (sts);
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

/************************************************************************/
/*									*/
/*  Convert port number string to binary				*/
/*									*/
/************************************************************************/

static uLong cvtpnstr (char *pnstr, uByte pnbin[OZ_IO_IP_PORTSIZE])

{
  int i, j;
  uLong v;

  v = oz_hw_atoi (pnstr, &j);
  if (pnstr[j] != 0) goto bad;
  for (i = OZ_IO_IP_PORTSIZE; -- i >= 0;) {
    pnbin[i] = v;
    v >>= 8;
  }
  if (v != 0) goto bad;
  return (OZ_SUCCESS);

bad:
  oz_sys_io_fs_printf (oz_util_h_error, "%s: bad port number %s\n", pn, pnstr);
  return (OZ_BADPARAM);
}

/************************************************************************/
/*									*/
/*  Convert port number binary to string				*/
/*									*/
/************************************************************************/

static char *cvtpnbin (uByte pnbin[OZ_IO_IP_PORTSIZE], char pnstr[OZ_IO_IP_PORTSIZE*4])

{
  int i;
  uLong v;

  v = 0;
  for (i = 0; i < OZ_IO_IP_PORTSIZE; i ++) v = (v << 8) + pnbin[i];
  oz_hw_itoa (v, OZ_IO_IP_PORTSIZE * 4, pnstr);
  return (pnstr);
}

/************************************************************************/
/*									*/
/*  Convert ethernet string to binary					*/
/*									*/
/************************************************************************/

static uLong cvtenstr (const char *enstr, uByte enbinsize, uByte enbin[OZ_IO_ETHER_MAXADDR])

{
  const char *p;
  int i, usedup;
  uLong v;

  p = enstr;
  for (i = 0; i < enbinsize; i ++) {
    v = oz_hw_atoz (p, &usedup);
    if ((usedup == 0) || (v > 255)) goto badparam;
    p += usedup;
    if (*p != ((i == enbinsize - 1) ? 0 : '-')) goto badparam;
    p ++;
    enbin[i] = v;
  }
  return (OZ_SUCCESS);

badparam:
  oz_sys_io_fs_printf (oz_util_h_error, "%s: error converting ethernet address at %s\n", pn, p);
  return (OZ_BADPARAM);
}

/************************************************************************/
/*									*/
/*  Convert ethernet binary to string					*/
/*									*/
/************************************************************************/

static char *cvtenbin (uByte enbinsize, uByte enbin[OZ_IO_ETHER_MAXADDR], char enstr[OZ_IO_ETHER_MAXADDR*3])

{
  char *p;
  int i;

  p = enstr;
  for (i = 0; i < enbinsize; i ++) {
    *(p ++) = hextab[enbin[i]>>4];
    *(p ++) = hextab[enbin[i]&15];
    if (i != enbinsize - 1) *(p ++) = '-';
  }
  *p = 0;
  return (enstr);
}

/************************************************************************/
/*									*/
/*  Get number of bytes in a binary ethernet address			*/
/*									*/
/************************************************************************/

static uByte getenaddrsize (const char *devname)

{
  OZ_Handle h_enchan;
  OZ_IO_ether_getinfo1 ether_getinfo1;
  uLong sts;

  sts = oz_sys_io_assign (OZ_PROCMODE_KNL, &h_enchan, devname, OZ_LOCKMODE_NL);
  if (sts != OZ_SUCCESS) return (0);
  memset (&ether_getinfo1, 0, sizeof ether_getinfo1);
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_enchan, 0, OZ_IO_ETHER_GETINFO1, sizeof ether_getinfo1, &ether_getinfo1);
  oz_sys_handle_release (OZ_PROCMODE_KNL, h_enchan);
  if (sts != OZ_SUCCESS) return (0);
  return (ether_getinfo1.addrsize);
}
