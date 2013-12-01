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
/*  Get an host's ip address						*/
/*									*/
/*    Input:								*/
/*									*/
/*	host = host name string						*/
/*	ipsize = size of ipaddr (OZ_IO_IP_ADDRSIZE)			*/
/*	ipaddr = where to return ip address				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_gethostipaddr = OZ_SUCCESS : successful			*/
/*	                             else : error status		*/
/*	*ipaddr = filled in with host's ip address			*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_io_ip.h"
#include "oz_knl_status.h"
#include "oz_sys_gethostipaddr.h"
#include "oz_sys_handle.h"
#include "oz_sys_io.h"
#include "oz_sys_logname.h"

uLong oz_sys_gethostipaddr (const char *host, int ipsize, uByte *ipaddr)

{
  char c, hostname[OZ_LOGNAME_MAXNAMSZ], *p;
  const char *cp;
  int i, loglevel, usedup;
  OZ_Handle h_iochan, h_name, h_table;
  OZ_IO_ip_dnslookup ip_dnslookup;
  uLong logvalatr, numel, sts;

  loglevel  = OZ_LOGNAME_MAXLEVEL;
  logvalatr = 0;

  /* Try to convert numerically */

decode:
  cp = host;
  for (i = 0; i < ipsize; i ++) {
    sts = oz_hw_atoi (cp, &usedup);
    if (sts > 255) goto not_numeric;
    ipaddr[i] = sts;
    cp += usedup;
    if (*(cp ++) != ((i < ipsize - 1) ? '.' : 0)) goto not_numeric;
  }
  return (OZ_SUCCESS);
not_numeric:

  /* If we're looping on a terminal logical name translation, fail */

  if (logvalatr & OZ_LOGVALATR_TERMINAL) return (OZ_DNSNOSUCHNAME);

  /* Try to get from OZ_IP_HOSTS logical name table */
  /* Host names are assumed to be all upper case    */

  if (-- loglevel >= 0) {
    sts = oz_sys_logname_lookup (0, OZ_PROCMODE_USR, "OZ_IP_HOSTS", NULL, NULL, NULL, &h_table);
    if (sts == OZ_SUCCESS) {
      if (host != hostname) strncpyz (hostname, host, sizeof hostname);
      for (p = hostname; (c = *p) != 0; p ++) {
        if ((c >= 'a') && (c <= 'z')) *p = c - 'a' + 'A';
      }
      sts = oz_sys_logname_lookup (h_table, OZ_PROCMODE_USR, hostname, NULL, NULL, NULL, &h_name);
      oz_sys_handle_release (OZ_PROCMODE_KNL, h_table);
      if (sts == OZ_SUCCESS) {
        sts = oz_sys_logname_getval (h_name, 0, &logvalatr, sizeof hostname, hostname, NULL, NULL, 0, NULL);
        oz_sys_handle_release (OZ_PROCMODE_KNL, h_name);
        if (sts == OZ_SUCCESS) {
          host = hostname;
          goto decode;
        }
      }
    }
  }

  /* Finally, try DNS */

  sts = oz_sys_io_assign (OZ_PROCMODE_KNL, &h_iochan, OZ_IO_IP_DEV, OZ_LOCKMODE_CR);
  if (sts != OZ_SUCCESS) return (sts);

  memset (&ip_dnslookup, 0, sizeof ip_dnslookup);
  ip_dnslookup.name    = host;
  ip_dnslookup.elsiz   = ipsize;
  ip_dnslookup.maxel   = 1;
  ip_dnslookup.array   = ipaddr;
  ip_dnslookup.numel_r = &numel;

  sts = oz_sys_io (OZ_PROCMODE_KNL, h_iochan, 0, OZ_IO_IP_DNSLOOKUP, sizeof ip_dnslookup, &ip_dnslookup);
  if ((sts == OZ_SUCCESS) && (numel == 0)) sts = OZ_DNSNOSUCHNAME;
  oz_sys_handle_release (OZ_PROCMODE_KNL, h_iochan);
  return (sts);
}
