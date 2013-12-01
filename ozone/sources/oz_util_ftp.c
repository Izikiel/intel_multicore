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
/*  FTP client utility							*/
/*									*/
/*	ftp <ip_address> [<port_number>]				*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_knl_status.h"
#include "oz_io_console.h"
#include "oz_io_ip.h"
#include "oz_lib_ftp.h"
#include "oz_sys_handle.h"
#include "oz_sys_io.h"
#include "oz_sys_io_fs.h"
#include "oz_sys_io_fs_printf.h"
#include "oz_util_start.h"

static int c_get (char *params);
static int c_put (char *params);
static int c_user (char *params);

static const struct { const char *name, int (*entry) (char *params), const char *help } 
         cmdtbl[] = { "get",  c_get,  "<remote_filename> [<local_filename>]", 
                      "put",  c_put,  "<local_filename> [<remote_filename>]", 
                      "user", c_user, "[<username> [<password>]]", 
                    };

static char defaultdir[256];
static char *pn = "ftp";
static volatile uLong listenstatus;
static OZ_Handle h_listenevent, h_listensocket;
static uByte dstipaddr[OZ_IO_IP_ADDRSIZE], dstportno[OZ_IO_IP_PORTSIZE];

static void readprompt (int size, char *buff, const char *prompt, int noecho);

uLong oz_util_main (int argc, char *argv[])

{
  char cmdbuff[256], *p;
  int i, usedup;
  OZ_IO_fs_readrec fs_readrec;
  OZ_IO_ip_tcpconnect ip_tcpconnect;
  uLong cmdrlen, sts;

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

  sts = 21;
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

  defaultdir = ??;

  sts = oz_sys_io (OZ_PROCMODE_KNL, h_tcpiochan, 0, OZ_IO_IP_TCPCONNECT, sizeof ip_tcpconnect, &ip_tcpconnect);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u connecting\n", pn, sts);
    return (sts);
  }
  oz_sys_io_fs_printf (oz_util_h_error, "%s: connected\n", pn);

  memset (&fs_readrec, 0, sizeof fs_readrec);
  fs_readrec.size = sizeof cmdbuff - 1;
  fs_readrec.buff = cmdbuff;
  fs_readrec.rlen = &cmdrlen;
  fs_readrec.trmsize = 1;
  fs_readrec.trmbuff = "\n";
  fs_readrec.pmtsize = 5;
  fs_readrec.pmtbuff = "ftp> ";

  while ((sts = oz_sys_io (OZ_PROCMODE_KNL, oz_util_h_input, 0, OZ_IO_FS_READREC, sizeof fs_readrec, &fs_readrec)) == OZ_SUCCESS) {
    cmdbuff[cmdrlen] = 0;
    for (p = cmdbuff; *p != 0; p ++) if (*p > ' ') break;
    for (i = 0; cmdtbl[i].name != NULL; i ++) {
      l = strlen (cmdtbl[i].name);
      if ((p[l] <= ' ') && (strncasecmp (p, cmdtbl[i].name, l) == 0)) break;
    }
    if (cmdtbl[i].name == NULL) oz_sys_io_fs_printf (oz_util_h_error, "%s: unknown command %s\n", pn, p);
    else {
      p += l;
      while ((*p != 0) && (*p <= ' ')) p ++;
      (*(cmdtbl[i].entry)) (p);
    }
  }

  if (sts == OZ_ENDOFFILE) sts = OZ_SUCCESS;
  else oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u reading command from input\n", pn, sts);
  return (sts);
}

static int c_get (char *params)

{
  char *lclfile, *remfile;
  int rc;
  OZ_Ftp_ctx *ftpctx;
  OZ_Handle h_datalink;
  volatile int donerc;

  /* Get remote and local filenames from command line */

  if (*params == 0) {
    oz_sys_io_fs_printf ("%s: missing local and remote filenames\n", pn);
    return (-1);
  }

  lclfile = remfile = params;
  while (*params > ' ') params ++;
  if (*params != 0) {
    *(params ++) = 0;
    while (*params > 0 && *params <= ' ') params ++;
    if (*params != 0) {
      lclfile = params;
      while (*params > ' ') params ++;
      if (*params != 0) {
        *(params ++) = 0;
        while (*params > 0 && *params <= ' ') params ++;
        if (*params != 0) {
          oz_sys_io_fs_printf (oz_util_h_error, "%s: extra params at end of line %s\n", pn, params);
          return (-1);
        }
      }
    }
  }

  /* Set up socket for server to connect to */

  if (!setupsocket ()) return (-1);

  /* Tell server to start sending file */

  sendcmd ("RETR %s\r\n", remfile);

  /* Wait for server to connect to us */

  rc = waitforserver ();
  if (rc != 0) return (rc);

  /* Start receiving file */

  donerc = 0;
  ftpctx = oz_lib_ftp_init (dstipaddr, dstportno, ??lclportno, defaultdir);
  oz_lib_ftp_receive (ftpctx, lclfile, getast, &donerc);

  /* Wait for completion */

  rc = recvrpl ();
  if (rc == ??) {
    while ((rc = donerc) == 0) {
      oz_sys_event_wait (OZ_PROCMODE_KNL, h_event);
      oz_sys_event_set (OZ_PROCMODE_KNL, h_event, 0, NULL);
    }
  }

  oz_lib_ftp_term (ftpctx);

  return (rc);
}

static int c_put (char *params)

{
  char *lclfile, *remfile;
  int rc;
  OZ_Ftp_ctx *ftpctx;
  OZ_Handle h_datalink;
  volatile int donerc;

  /* Get remote and local filenames from command line */

  if (*params == 0) {
    oz_sys_io_fs_printf ("%s: missing local and remote filenames\n", pn);
    return (-1);
  }

  lclfile = remfile = params;
  while (*params > ' ') params ++;
  if (*params != 0) {
    *(params ++) = 0;
    while (*params > 0 && *params <= ' ') params ++;
    if (*params != 0) {
      remfile = params;
      while (*params > ' ') params ++;
      if (*params != 0) {
        *(params ++) = 0;
        while (*params > 0 && *params <= ' ') params ++;
        if (*params != 0) {
          oz_sys_io_fs_printf (oz_util_h_error, "%s: extra params at end of line %s\n", pn, params);
          return (-1);
        }
      }
    }
  }

  /* Set up socket for server to connect to */

  if (!setupsocket ()) return (-1);

  /* Tell server to start receiving file */

  sendcmd ("STOR %s\r\n", remfile);

  /* Start sending file */

  donerc = 0;
  ftpctx = oz_lib_ftp_init (dstipaddr, dstportno, ??lclportno, defaultdir);
  oz_lib_ftp_send (ftpctx, lclfile, getast, &donerc);

  /* Wait for completion */

  rc = recvrpl ();
  if (rc == ??) {
    while ((rc = donerc) == 0) {
      oz_sys_event_wait (OZ_PROCMODE_KNL, h_event);
      oz_sys_event_set (OZ_PROCMODE_KNL, h_event, 0, NULL);
    }
  }

  oz_lib_ftp_term (ftpctx);

  return (rc);
}

static int c_user (char *params)

{
  char password[256], username[256];
  int i, rc;

  /* Get username and password either from command line or from prompting */

  if (*params == 0) {
    readprompt (sizeof username, username, "Username: ", 0);
    if (username[0] == 0) return;
  } else {
    for (i = 0; *params > ' '; params ++) if (i < sizeof username - 1) username[i++] = *params;
    username[i] = 0;
    while ((*params != 0) && (params <= ' ')) params ++;
  }

  if (*params == 0) {
    readprompt (sizeof password, password, "Password: ", 0);
    if (password[0] == 0) return;
  } else {
    for (i = 0; *params > ' '; params ++) if (i < sizeof password - 1) password[i++] = *params;
    password[i] = 0;
    while ((*params != 0) && (params <= ' ')) params ++;
  }

  /* Send them to the server */

  rc = sendcmdgetreply ("USER %s\r\n", username);
  if (rc == ??) rc = sendcmdgetreply ("PASS %s\r\n", password);
  return (rc);
}

/************************************************************************/
/*									*/
/*  Create a socket and listen for a connection from the server		*/
/*									*/
/************************************************************************/

static int setupsocket (void)

{
  OZ_Handle h_socket;
  uLong sts;

  sts = oz_sys_io_assign (OZ_PROCMODE_KNL, &h_listensocket, OZ_IO_IP_DEV, OZ_LOCKMODE_CW);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u creating %s socket\n", pn, sts, OZ_IO_IP_DEV);
    return (0);
  }

  memset (&ip_tcplisten, 0, sizeof ip_tcplisten);
  ip_tcplisten.addrsize  = OZ_IO_IP_ADDRSIZE;
  ip_tcplisten.portsize  = OZ_IO_IP_PORTSIZE;
  ip_tcplisten.lclipaddr = lclipaddr;
  ip_tcplisten.lclportno = lclportno;
  ip_tcplisten.remipaddr = dstipaddr;
  ip_tcplisten.remportno = remportno_data;

  listenstatus = OZ_PENDING;
  sts = oz_sys_io_start (OZ_PROCMODE_KNL, h_listensocket, &listenstatus, &h_listenevent, NULL, NULL, 
                         OZ_IO_IP_TCPLISTEN, sizeof ip_tcplisten, &ip_tcplisten);
  if (sts != OZ_STARTED) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u listening on socket\n", pn, sts);
    oz_sys_handle_release (OZ_PROCMODE_KNL, h_listensocket);
    return (0);
  }

  return (1);
}

/************************************************************************/
/*									*/
/*  Read parameter string from the console with a prompt		*/
/*									*/
/************************************************************************/

static void readprompt (int size, char *buff, const char *prompt, int noecho)

{
  OZ_IO_console_read console_read;
  uLong rlen, sts;

  memset (&console_read, 0, sizeof console_read);
  console_read.size    = size - 1;
  console_read.buff    = buff;
  console_read.rlen    = &rlen;
  console_read.pmtsize = strlen (prompt);
  console_read.pmtbuff = prompt;
  console_read.noecho  = noecho;

  sts = oz_sys_io (OZ_PROCMODE_KNL, oz_util_h_console, 0, OZ_IO_CONSOLE_READ, sizeof console_read, &console_read);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u reading '%s' from console\n", pn, prompt);
    oz_sys_thread_exit (sts);
  }
  buff[rlen] = 0;
}
