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
/*  Network routines							*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_io_ip.h"
#include "oz_sys_handle.h"
#include "oz_sys_io.h"
#include "oz_sys_logname.h"

#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
extern int h_errno;

/************************************************************************/
/*									*/
/*  Network <-> Host byte order conversions				*/
/*									*/
/************************************************************************/

uWord htons (uWord x)

{
  union y { uWord w; uByte b[2]; };
  y.w = x;
  return ((y.b[0] << 8) | y.b[1]);
}

uWord ntohs (uWord x)

{
  union y { uWord w; uByte b[2]; };
  y.w = x;
  return ((y.b[0] << 8) | y.b[1]);
}

uLong htonl (uLong x)

{
  union y { uLong l; uByte b[4]; };
  y.l = x;
  return ((((((y.b[0] << 8) | y.b[1]) << 8) | y.b[2]) << 8) | y.b[3]);
}

uLong ntohl (uLong x)

{
  union y { uLong l; uByte b[4]; };
  y.l = x;
  return ((((((y.b[0] << 8) | y.b[1]) << 8) | y.b[2]) << 8) | y.b[3]);
}

/************************************************************************/
/*									*/
/*  DNS related routines						*/
/*									*/
/************************************************************************/

uLong oz_crtl_gethostipaddr (const char *host, int ipsize, uByte *ipaddr)

{
  const char *cp;
  int i, usedup;
  OZ_IO_ip_dnslookup ip_dnslookup;
  uLong numel, sts;

  static OZ_Handle h_iochan_dnslookup = 0;

  /* Try to convert numerically */

  cp = host;
  for (i = 0; i < ipsize; i ++) {
    sts = oz_hw_atoi (cp, &usedup);
    if (sts > 255) goto trydns;
    ipaddr[i] = sts;
    cp += usedup;
    if (*(cp ++) != ((i < ipsize - 1) ? '.' : 0)) goto trydns;
  }
  return (OZ_SUCCESS);

  /* Failed, use DNS */

trydns:
  if (h_iochan_dnslookup == 0) {
    sts = oz_sys_io_assign (OZ_PROCMODE_KNL, &h_iochan_dnslookup, OZ_IO_IP_DEV, OZ_LOCKMODE_CR);
    if (sts != OZ_SUCCESS) return (sts);
  }

  memset (&ip_dnslookup, 0, sizeof ip_dnslookup);
  ip_dnslookup.name    = host;
  ip_dnslookup.elsiz   = ipsize;
  ip_dnslookup.maxel   = 1;
  ip_dnslookup.array   = ipaddr;
  ip_dnslookup.numel_r = &numel;

  sts = oz_sys_io (OZ_PROCMODE_KNL, h_iochan_dnslookup, 0, OZ_IO_IP_DNSLOOKUP, sizeof ip_dnslookup, &ip_dnslookup);
  if ((sts == OZ_SUCCESS) && (numel == 0)) sts = OZ_DNSNOSUCHNAME;
  return (sts);
}

/************************************************************************/
/*									*/
/*  Get this host's name.  This is done by translating logical name 	*/
/*  OZ_IP_HOSTNAME.							*/
/*									*/
/************************************************************************/

int gethostname (char *name, size_t len)

{
  OZ_Handle h_logname, h_logtable;
  uLong hostnamel, sts;

  sts = oz_sys_logname_lookup (0, OZ_PROCMODE_USR, oz_s_logname_defaulttables, NULL, NULL, NULL, &h_logtable);
  if (sts == OZ_SUCCESS) {
    sts = oz_sys_logname_lookup (h_logtable, OZ_PROCMODE_USR, "OZ_IP_HOSTNAME", NULL, NULL, NULL, &h_logname);
    oz_sys_handle_release (OZ_PROCMODE_KNL, h_logtable);
  }
  if (sts == OZ_SUCCESS) {
    sts = oz_sys_logname_getval (h_logname, 0, NULL, len - 1, name, &hostnamel, NULL, 0, NULL);
    oz_sys_handle_release (OZ_PROCMODE_KNL, h_logname);
  }
  if (sts != OZ_SUCCESS) return (NULL);
  name[hostnamel] = 0;
  return (hostnamel);
}

/************************************************************************/
/*									*/
/*  Get host ip address given its name.  This is done via the ip 	*/
/*  driver's built-in dns lookup function.				*/
/*									*/
/************************************************************************/

struct hostent *gethostbyname (const char *name)

{
  OZ_IO_ip_dnslookup ip_dnslookup;
  uLong numel, sts;

  static char he_h_addrs[OZ_IO_IP_DNSNUMMAX*OZ_IO_IP_ADDRSIZE], *he_h_addr_list[OZ_IO_IP_DNSNUMMAX+1], *he_h_alises[1], he_h_name[OZ_IO_IP_DNSNAMMAX];
  static OZ_Handle h_iochan_dnslookup = 0;
  static struct hostent he;

  if (h_iochan_dnslookup == 0) {
    sts = oz_sys_io_assign (OZ_PROCMODE_KNL, &h_iochan_dnslookup, OZ_IO_IP_DEV, OZ_LOCKMODE_CR);
    if (sts != OZ_SUCCESS) {
      h_errno = ??;
      return (NULL);
    }
  }

  memset (&ip_dnslookup, 0, sizeof ip_dnslookup);
  ip_dnslookup.name  = name;
  ip_dnslookup.elsiz = OZ_IO_IP_ADDRSIZE;
  ip_dnslookup.maxel = OZ_IO_IP_DNSNUMMAX;
  ip_dnslookup.array = (void *)he_h_addrs;

  sts = oz_sys_io (OZ_PROCMODE_KNL, h_iochan_dnslookup, 0, OZ_IO_IP_DNSLOOKUP, sizeof ip_dnslookup, &ip_dnslookup);
  if (sts != OZ_SUCCESS) {
    h_errno = ??;
    return (NULL);
  }

  for (sts = 0; sts < numel; sts ++) he_h_addr_list[0] = he_h_addrs + sts * OZ_IO_IP_ADDRSIZE;

  he_h_aliases = NULL;

  he.h_name      = he_h_name;
  he.h_aliases   = he_h_alises;
  he.h_addrtype  = AF_INET;
  he.h_length    = OZ_IO_IP_ADDRSIZE;
  he.h_addr_list = he_h_addr_list;

  return (&he);
}

/************************************************************************/
/*									*/
/*  Translate binary to numeric dotted notation				*/
/*									*/
/************************************************************************/

char *inet_ntoa (struct in_addr in)

{
  union y { uLong l; uByte b[4]; };

  static char buff[16];

  y.l = in.s_addr;
  sprintf (buff, "%u.%u.%u.%u", y.b[0], y.b[1], y.b[2], y.b[3]);
  return (buff);
}

/************************************************************************/
/*									*/
/*  Translate numeric dotted notation to binary				*/
/*									*/
/************************************************************************/

unsigned long int inet_addr (const char *cp)

{
  int i, usedup;
  uLong x;
  union y { uLong l; uByte b[4]; };

  for (i = 0; i < 4; i ++) {
    x = oz_hw_atoi (cp, &usedup);
    if (x > 255) return (-1);
    y.b[i] = x;
    cp += usedup;
    if (*(cp ++) != ((i < 3) ? '.' : 0)) return (-1);
  }

  return (y.l);
}

/************************************************************************/
/*									*/
/*  Get service by name							*/
/*									*/
/*    Looks for logicals in table 'OZ_IP_SERVICES'.  The names must be 	*/
/*    defined in all lower case.  The definitions are made like this:	*/
/*									*/
/*	create logical name OZ_IP_SERVICES%domain tcp:53 udp:53		*/
/*									*/
/************************************************************************/

struct servent *getservbyname (const char *name, const char *proto)

{
  char c;
  int protol;
  OZ_Handle h_logname, h_logtable;
  uLong i, nvalues, portstrl, sts;

  static char namestr[32], portstr[32];
  static char *aliases = NULL;
  static struct servent se;

  strncpy (namestr, name, sizeof namestr);
  for (i = 0; (c = namestr[i]) != 0; i ++) if ((c >= 'A') && (c <= 'Z')) namestr[i] = c + 'a' - 'A';
  protol = strlen (proto);

  sts = oz_sys_logname_lookup (0, OZ_PROCMODE_USR, "OZ_IP_SERVICES", NULL, NULL, NULL, &h_logtable);
  if (sts == OZ_SUCCESS) {
    sts = oz_sys_logname_lookup (h_logtable, OZ_PROCMODE_USR, namestr, NULL, NULL, &nvalues, &h_logname);
    oz_sys_handle_release (OZ_PROCMODE_KNL, h_logtable);
  }
  if (sts == OZ_SUCCESS) {
    for (i = 0; i < nvalues; i ++) {
      sts = oz_sys_logname_getval (h_logname, i, NULL, sizeof portstr - 1, portstr, &portstrl, NULL, 0, NULL);
      if (sts != OZ_SUCCESS) break;
      if ((portstr[protol] == ':') && (strncasecmp (portstr, proto, protol) == 0)) break;
    }
    oz_sys_handle_release (OZ_PROCMODE_KNL, h_logname);
  }
  if ((sts != OZ_SUCCESS) || (i == nvalues)) return (NULL);

  portstr[protol++] = 0;
  portstr[portstrl] = 0;
  se.s_name    = namestr;
  se.s_aliases = &aliases;
  se.s_port    = htons (oz_hw_atoi (portstr + protol, NULL));
  se.s_proto   = portstr;

  hostname[hostnamel] = 0;
  return (hostnamel);
}

/************************************************************************/
/*									*/
/*  I/O routines							*/
/*									*/
/************************************************************************/

socket
setsockopt
getsockname
getpeername
bind
listen
accept
shutdown
recvfrom
sendto
getsockname
connect
