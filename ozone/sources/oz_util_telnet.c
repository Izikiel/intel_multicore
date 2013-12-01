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
/*  Telnet client utility						*/
/*									*/
/*	telnet <ip_address> [<port_number>]				*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_knl_status.h"
#include "oz_io_console.h"
#include "oz_io_ip.h"
#include "oz_sys_condhand.h"
#include "oz_sys_handle.h"
#include "oz_sys_io.h"
#include "oz_sys_io_fs.h"
#include "oz_util_start.h"

static char *pn = "telnet";

uLong oz_util_main (int argc, char *argv[])

{
  uByte dstipaddr[OZ_IO_IP_ADDRSIZE], dstportno[OZ_IO_IP_PORTSIZE];
  char conreadbuf[256], tcpreadbuf[4096];
  int didsomething, i, savelinewrap, usedup;
  uLong conreadlen, conreadsts, sts, tcpreadlen, tcpreadsts;
  OZ_Console_modebuff modebuff;
  OZ_Handle h_event, h_tcpiochan;
  OZ_IO_console_getdat console_getdat;
  OZ_IO_console_getmode console_getmode;
  OZ_IO_console_putdat console_putdat;
  OZ_IO_console_setmode console_setmode;
  OZ_IO_ip_tcpconnect ip_tcpconnect;
  OZ_IO_ip_tcpreceive ip_tcpreceive;
  OZ_IO_ip_tcptransmit ip_tcptransmit;

  if (argc > 0) pn = argv[0];

  if ((argc != 2) && (argc != 3)) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: usage: %s <ipaddr> [<portno>]\n", pn);
    return (OZ_MISSINGPARAM);
  }

  sts = oz_sys_gethostipaddr (argv[1], sizeof dstipaddr, dstipaddr);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u getting ip address for host %s\n", pn, sts, argv[1]);
    return (sts);
  }

  sts = 23;
  if (argc > 2) {
    sts = oz_hw_atoi (argv[2], &usedup);
    if ((argv[2][usedup] != 0) || (sts > 65535)) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: bad port number %s\n", pn, argv[2]);
      return (OZ_BADPARAM);
    }
  }
  for (i = OZ_IO_IP_PORTSIZE; -- i >= 0;) {
    dstportno[i] = sts & 0xff;
    sts >>= 8;
  }

  sts = oz_sys_io_assign (OZ_PROCMODE_KNL, &h_tcpiochan, OZ_IO_IP_DEV, OZ_LOCKMODE_CW);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u assigning channel to " OZ_IO_IP_DEV "\n", pn, sts);
    return (sts);
  }

  memset (&ip_tcpconnect, 0, sizeof ip_tcpconnect);
  ip_tcpconnect.addrsize  = OZ_IO_IP_ADDRSIZE;
  ip_tcpconnect.portsize  = OZ_IO_IP_PORTSIZE;
  ip_tcpconnect.dstipaddr = dstipaddr;
  ip_tcpconnect.dstportno = dstportno;
  ip_tcpconnect.timeout   = 30000;

  sts = oz_sys_io (OZ_PROCMODE_KNL, h_tcpiochan, 0, OZ_IO_IP_TCPCONNECT, sizeof ip_tcpconnect, &ip_tcpconnect);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u connecting\n", pn, sts);
    return (sts);
  }
  oz_sys_io_fs_printf (oz_util_h_error, "%s: connected\n", pn);

  sts = oz_sys_event_create (OZ_PROCMODE_KNL, "telnet read complete", &h_event);
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);

  memset (&console_getmode, 0, sizeof console_getmode);
  console_getmode.size = sizeof modebuff;
  console_getmode.buff = &modebuff;
  sts = oz_sys_io (OZ_PROCMODE_KNL, oz_util_h_input, h_event, OZ_IO_CONSOLE_GETMODE, sizeof console_getmode, &console_getmode);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u getting console mode\n", pn, sts);
    return (sts);
  }

  memset (&console_setmode, 0, sizeof console_setmode);
  console_setmode.size = sizeof modebuff;
  console_setmode.buff = &modebuff;

  if (modebuff.linewrap > 0) {
    savelinewrap = modebuff.linewrap;
    modebuff.linewrap = -1;
    sts = oz_sys_io (OZ_PROCMODE_KNL, oz_util_h_input, h_event, OZ_IO_CONSOLE_SETMODE, sizeof console_setmode, &console_setmode);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u setting console mode\n", pn, sts);
      return (sts);
    }
    modebuff.linewrap = savelinewrap;
  }

  memset (&console_getdat, 0, sizeof console_getdat);
  console_getdat.size = (sizeof conreadbuf) - 2;
  console_getdat.buff = conreadbuf;
  console_getdat.rlen = &conreadlen;

  conreadsts = OZ_PENDING;
  sts = oz_sys_io_start (OZ_PROCMODE_KNL, oz_util_h_input, &conreadsts, h_event, NULL, NULL, OZ_IO_CONSOLE_GETDAT, sizeof console_getdat, &console_getdat);
  if (sts != OZ_STARTED) conreadsts = sts;

  memset (&ip_tcpreceive, 0, sizeof ip_tcpreceive);
  ip_tcpreceive.rawsize = sizeof tcpreadbuf;
  ip_tcpreceive.rawbuff = tcpreadbuf;
  ip_tcpreceive.rawrlen = &tcpreadlen;

  tcpreadsts = OZ_PENDING;
  sts = oz_sys_io_start (OZ_PROCMODE_KNL, h_tcpiochan, &tcpreadsts, h_event, NULL, NULL, OZ_IO_IP_TCPRECEIVE, sizeof ip_tcpreceive, &ip_tcpreceive);
  if (sts != OZ_STARTED) tcpreadsts = sts;

  memset (&console_putdat, 0, sizeof console_putdat);
  console_putdat.buff = tcpreadbuf;

  memset (&ip_tcptransmit, 0, sizeof ip_tcptransmit);
  ip_tcptransmit.rawbuff = conreadbuf;

waitloop:
  if (!didsomething) {
    sts = oz_sys_event_wait (OZ_PROCMODE_KNL, h_event, 0);
    if ((sts != OZ_FLAGWASSET) && (sts != OZ_FLAGWASCLR) && (sts != OZ_ASTDELIVERED)) oz_sys_condhand_signal (2, sts, 0);
    sts = oz_sys_event_set (OZ_PROCMODE_KNL, h_event, 0, NULL);
    if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
  }

  didsomething = 0;

  sts = conreadsts;
  if (sts != OZ_PENDING) {
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u reading from console\n", pn, sts);
      goto rtnsts;
    }
    ip_tcptransmit.rawsize = conreadlen;
    sts = oz_sys_io (OZ_PROCMODE_KNL, h_tcpiochan, 0, OZ_IO_IP_TCPTRANSMIT, sizeof ip_tcptransmit, &ip_tcptransmit);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u writing to network\n", pn, sts);
      goto rtnsts;
    }
    conreadsts = OZ_PENDING;
    sts = oz_sys_io_start (OZ_PROCMODE_KNL, oz_util_h_input, &conreadsts, h_event, NULL, NULL, OZ_IO_CONSOLE_GETDAT, sizeof console_getdat, &console_getdat);
    if (sts != OZ_STARTED) conreadsts = sts;
    didsomething = 1;
  }

  sts = tcpreadsts;
  if (sts != OZ_PENDING) {
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u reading from network\n", pn, sts);
      goto rtnsts;
    }
    console_putdat.size = tcpreadlen;
    sts = oz_sys_io (OZ_PROCMODE_KNL, oz_util_h_output, 0, OZ_IO_CONSOLE_PUTDAT, sizeof console_putdat, &console_putdat);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u writing to console\n", pn, sts);
      goto rtnsts;
    }
    tcpreadsts = OZ_PENDING;
    sts = oz_sys_io_start (OZ_PROCMODE_KNL, h_tcpiochan, &tcpreadsts, h_event, NULL, NULL, OZ_IO_IP_TCPRECEIVE, sizeof ip_tcpreceive, &ip_tcpreceive);
    if (sts != OZ_STARTED) tcpreadsts = sts;
    didsomething = 1;
  }

  goto waitloop;

rtnsts:
  oz_sys_io (OZ_PROCMODE_KNL, oz_util_h_input, h_event, OZ_IO_CONSOLE_SETMODE, sizeof console_setmode, &console_setmode);
  return (sts);
}
