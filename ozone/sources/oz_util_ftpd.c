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
/*  FTP daemon								*/
/*									*/
/*	ftpd [<port>] -restrictports -verbose				*/
/*									*/
/*	-restrictports: restricts PORT and PASV commands to the same 	*/
/*	                host as client					*/
/*	-verbose: verbose logging					*/
/*									*/
/************************************************************************/

#include <stdarg.h>

#include "ozone.h"
#include "oz_crtl_malloc.h"
#include "oz_knl_devio.h"
#include "oz_knl_handle.h"
#include "oz_knl_status.h"
#include "oz_knl_user_become.h"
#include "oz_io_console.h"
#include "oz_io_ip.h"
#include "oz_sys_condhand.h"
#include "oz_sys_dateconv.h"
#include "oz_sys_event.h"
#include "oz_sys_fork.h"
#include "oz_sys_handle.h"
#include "oz_sys_handle_getinfo.h"
#include "oz_sys_io.h"
#include "oz_sys_io_fs.h"
#include "oz_sys_logname.h"
#include "oz_sys_password.h"
#include "oz_sys_thread.h"
#include "oz_sys_xprintf.h"
#include "oz_util_start.h"

#define NLISTENS 5	/* number of listens to have active */
#define NTHREADS 1000	/* max number of threads to have active */
#define SENDFACTOR 3	/* number of 'tcpsendsize' to send at a time */
#define SIZEFACTOR 36	/* number of 'tcpsendsize' to allocate for databuff */
#define TCPSENDSIZE 1460

typedef struct Retrsendctx Retrsendctx;

typedef struct { OZ_Handle h_ctrllink;			/* tcp connection I/O channel */
                 char clientname[OZ_DEVUNIT_NAMESIZE];	/* client name (<ipaddress>.<portno>) */
                 uByte clientipaddr[OZ_IO_IP_ADDRSIZE];	/* client ip address */
                 uByte clientportno[OZ_IO_IP_PORTSIZE];	/* client port number */

                 OZ_Handle h_user;			/* handle to user block that we logged in */
                 char loggedin;				/* 0: haven't logged in yet; 1: logged in */
                 char hasquit;				/* 0: hasn't QUIT yet; 1: has QUIT */
                 char username[OZ_USERNAME_MAX];	/* - their username */
                 char defaultdir[OZ_FS_MAXFNLEN];	/* - their (current) default directory */

                 char reptype;				/* representation type */
                 char repsubtype;			/* representation sub-type */
                 char filestruct;			/* file structure */
                 char transmode;			/* transfer mode */
							/* 'S': stream (default) */
							/* 'B': block */
							/* 'C': compressed */
                 int keepversions;			/* 0: suppress version number display */
							/* 1: enable version number display */
                 char lastfilename[OZ_FS_MAXFNLEN];	/* last filename listed out */
                 char oldfilename[OZ_FS_MAXFNLEN];	/* rename from name */

                 int transferinprog;			/* 0: not doing transfer; 1: doing transfer; -1: transfer aborting */
                 OZ_Datebin transferstarted;		/* date/time transfer was started */
                 OZ_Handle h_datalink;			/* data link i/o channel */
                 OZ_Handle h_datafile;			/* data file i/o channel */
                 OZ_Handle h_pasvlink;			/* passive mode listening channel (or 0 for active) */
                 OZ_Handle h_pasvwait;			/* passive mode listening event flag */
                 Retrsendctx *freeretrsendctxs;		/* free retrieve send context blocks */
                 Retrsendctx *retrsendctx_qh, **retrsendctx_qt;
                 uLong tcpsendsize;			/* optimal tcp send size */
                 uLong writeinprog;			/* write in progress flag */
                 uLong readinprog;			/* read in progress flag */
                 uLong writeoffset;			/* write offset in databuff */
                 uLong readoffset;			/* read offset in databuff */
                 volatile uLong readstatus;		/* read completion status */
                 volatile uLong writestatus;		/* write completion status */
                 uLong diskiosofar;			/* number of disk bytes read/written so far */
                 uLong transfersofar;			/* number of bytes transferred so far */
                 uLong transfertotal;			/* total number of bytes to transfer */
                 uLong barelfs;				/* number of bare LF's found */
                 uLong rcvwindowrem;			/* beginning of valid data in databuff */
                 uLong nextwriteoffs;			/* offset of next data to write to disk */
                 uLong nextconvoffs;			/* offset of next data to convert */
                 uLong rcvwindownxt;			/* end of valid data in databuff */
                 uLong datarlen;			/* length of data read/received */
                 uLong datasize;			/* size of databuff */
                 volatile uLong pasvstatus;		/* passive mode listening status */
                 uByte *databuff;			/* pointer to data buffer of size datasize */
                 uByte dataipaddr[OZ_IO_IP_ADDRSIZE];	/* data port ip address */
                 uByte dataportno[OZ_IO_IP_PORTSIZE];	/* data port port number */
                 uByte dsrcportno[OZ_IO_IP_PORTSIZE];	/* data source port number */
                 uByte pasvipaddr[OZ_IO_IP_ADDRSIZE];	/* passive mode listening ip address */
                 uByte pasvportno[OZ_IO_IP_PORTSIZE];	/* passive mode listening port number */
               } Conctx;

struct Retrsendctx { Retrsendctx *next;
                     Conctx *conctx;
                     uLong status;
                     uLong size;
                     uLong offs;
                   };

static char myusername[OZ_USERNAME_MAX];
static char *pn = "ftpd";
static const char *const months[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
static int flag_restrictports;
static int flag_verbose;
static OZ_Handle h_event_listen;
static uByte lclportno[OZ_IO_IP_PORTSIZE];

#define DEBDATASTS(conctx) // debdatasts (conctx, __LINE__);
static void debdatasts (Conctx *conctx, int line);
static void debstatus (Conctx *conctx, const char *class, const char *format, ...);

static uLong incoming (void *conctxv);
static int opendatacon (Conctx *conctx, int transmit);
static void transfersuccessful (Conctx *conctx);
static void transfercomplete (Conctx *conctx);
static void closedatacon (Conctx *conctx);
static void sendreply (Conctx *conctx, const char *format, ...);
static char *cvtipbin (uByte ipbin[OZ_IO_IP_ADDRSIZE], char ipstr[OZ_IO_IP_ADDRSIZE*4]);
static char *cvtpnbin (uByte pnbin[OZ_IO_IP_PORTSIZE], char pnstr[OZ_IO_IP_PORTSIZE*4]);
static void pause (const char *prompt);

uLong oz_util_main (int argc, char *argv[])

{
  char ipstr[OZ_IO_IP_ADDRSIZE*4], pnstr[OZ_IO_IP_PORTSIZE*4], threadname[OZ_THREAD_NAMESIZE];
  Conctx *conctx, *conctxs[NLISTENS];
  int i, nconctx, usedup;
  OZ_Handle h_thread;
  OZ_IO_ip_tcplisten ip_tcplisten;
  uLong sts;

  OZ_Handle_item items[] = { OZ_HANDLE_CODE_USER_NAME, sizeof myusername, myusername, NULL };

  if (argc > 0) pn = argv[0];

  flag_restrictports = 0;
  flag_verbose = 0;
  OZ_IP_H2NW (21, lclportno);
  for (i = 1; i < argc; i ++) {
    if (argv[i][0] == '-') {
      if (strcasecmp (argv[i] + 1, "restrictports") == 0) {
        flag_restrictports = 1;
        continue;
      }
      if (strcasecmp (argv[i] + 1, "verbose") == 0) {
        flag_verbose = 1;
        continue;
      }
      goto usage;
    }
    sts = oz_hw_atoi (argv[1], &usedup);
    if ((argv[1][usedup] != 0) || (sts > 65535)) goto usage;
    OZ_IP_H2NW (sts, lclportno);
  }

  oz_sys_io_fs_printf (oz_util_h_error, "%s: listening on port %u%s\n", pn, sts, flag_restrictports ? " -restrictports" : "");

  /* Get my username */

  sts = oz_sys_handle_getinfo (0, 1, items, &usedup);
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);

  /* Get event flag to use for listening */

  sts = oz_sys_event_create (OZ_PROCMODE_KNL, "listening", &h_event_listen);
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);

  /* Set up listening parameter block */

  memset (&ip_tcplisten, 0, sizeof ip_tcplisten);
  ip_tcplisten.addrsize  = OZ_IO_IP_ADDRSIZE;
  ip_tcplisten.portsize  = OZ_IO_IP_PORTSIZE;
  ip_tcplisten.lclportno = lclportno;

  /* Start listening */

  memset (conctxs, 0, sizeof conctxs);

listenloop:
  for (nconctx = 0; nconctx < NLISTENS; nconctx ++) {

    /* Start a listen if none going on this slot */

    conctx = conctxs[nconctx];
    if (conctx == NULL) {

      /* Create a connection context structure */

      conctx = malloc (sizeof *conctx);
      memset (conctx, 0, sizeof *conctx);

      /* Assign an I/O channel to the ip device for the connection */

      sts = oz_sys_io_assign (OZ_PROCMODE_KNL, &(conctx -> h_ctrllink), OZ_IO_IP_DEV, OZ_LOCKMODE_CW);
      if (sts != OZ_SUCCESS) {
        oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u assigning channel to " OZ_IO_IP_DEV "\n", pn, sts);
        return (sts);
      }

      /* Start listening for an incoming connection request */

      ip_tcplisten.remipaddr = conctx -> clientipaddr;
      ip_tcplisten.remportno = conctx -> clientportno;

      conctx -> readstatus = OZ_PENDING;
      sts = oz_sys_io_start (OZ_PROCMODE_KNL, 
                             conctx -> h_ctrllink, 
                             &(conctx -> readstatus), 
                             h_event_listen, 
                             NULL, NULL, 
                             OZ_IO_IP_TCPLISTEN, 
                             sizeof ip_tcplisten, 
                             &ip_tcplisten);
      if (sts != OZ_STARTED) conctx -> readstatus = sts;

      conctxs[nconctx] = conctx;
    }

    /* See if the listen is complete */

    sts = conctx -> readstatus;
    if (sts == OZ_PENDING) continue;
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u receiving connection request\n", pn, sts);
      return (sts);
    }
    conctxs[nconctx] = NULL;
    oz_sys_event_set (OZ_PROCMODE_KNL, h_event_listen, 1, NULL);

    /* Make client name string '<ipaddress>,<portno>' */

    oz_sys_sprintf (sizeof conctx -> clientname, conctx -> clientname, "%s,%s", 
                    cvtipbin (conctx -> clientipaddr, ipstr), cvtpnbin (conctx -> clientportno, pnstr));
    oz_sys_io_fs_printf (oz_util_h_error, "%s: connection received from %s\n", pn, conctx -> clientname);

    /* Fork to process it */

    oz_sys_sprintf (sizeof threadname, threadname, "ftpd %s", conctx -> clientname);
    sts = oz_sys_fork (0, 0, 0, threadname, &h_thread);
    if (sts == OZ_SUBPROCESS) oz_sys_thread_exit (incoming (conctx));
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u forking for %s\n", pn, sts, conctx -> clientname);
    }
    oz_sys_handle_release (OZ_PROCMODE_KNL, conctx -> h_ctrllink);
    free (conctx);
    oz_sys_handle_release (OZ_PROCMODE_KNL, h_thread);
  }
  oz_sys_event_wait (OZ_PROCMODE_KNL, h_event_listen, 0);
  oz_sys_event_set (OZ_PROCMODE_KNL, h_event_listen, 0, NULL);
  goto listenloop;

usage:
  oz_sys_io_fs_printf (oz_util_h_error, "%s: usage: %s [<port>] [-restrictports] [-verbose]\n", pn);
  return (OZ_BADPARAM);
}

/************************************************************************/
/*									*/
/*  Command decoding table						*/
/*									*/
/************************************************************************/

static void c_abort (Conctx *conctx, char *params);
static void c_changeup (Conctx *conctx, char *params);
static void c_changedir (Conctx *conctx, char *params);
static void c_delete (Conctx *conctx, char *params);
static void c_help (Conctx *conctx, char *params);
static void c_listdir (Conctx *conctx, char *params);
static void c_mkdir (Conctx *conctx, char *params);
static void c_transmode (Conctx *conctx, char *params);
static void c_namelist (Conctx *conctx, char *params);
static void c_noop (Conctx *conctx, char *params);
static void c_password (Conctx *conctx, char *params);
static void c_passive (Conctx *conctx, char *params);
static void c_dataport (Conctx *conctx, char *params);
static void c_printwd (Conctx *conctx, char *params);
static void c_quit (Conctx *conctx, char *params);
static void c_retrieve (Conctx *conctx, char *params);
static void c_renamefrom (Conctx *conctx, char *params);
static void c_renameto (Conctx *conctx, char *params);
static void c_filesize (Conctx *conctx, char *params);
static void c_status (Conctx *conctx, char *params);
static void c_filestruct (Conctx *conctx, char *params);
static void c_store (Conctx *conctx, char *params);
static void c_system (Conctx *conctx, char *params);
static void c_reptype (Conctx *conctx, char *params);
static void c_username (Conctx *conctx, char *params);

#define FL_LI 0x1	/* must be logged in to use this command */
#define FL_DL 0x2	/* datalink connection must not be busy for this command */

static const struct { const char *name;
                      void (*entry) (Conctx *conctx, char *params);
                      uLong flags;
                      const char *help;
                    } cmdtbl[] = {
                      "ABOR", c_abort,                 0, "(abort current file transfer)", 
                      "ACCT", NULL,                    0, NULL, 
                      "ALLO", NULL,                FL_LI, "(set file allocation size) <filesize>", 
                      "APPE", NULL,                FL_LI, "(append to file) <filename>", 
                      "CDUP", c_changeup,          FL_LI, "(change directory up one level)", 
                      "CWD",  c_changedir,         FL_LI, "(change directory) <directory>", 
                      "DELE", c_delete,            FL_LI, "(delete files) <wildcardspec>", 
                      "HELP", c_help,                  0, "(list help messages)", 
                      "LIST", c_listdir,     FL_DL|FL_LI, "(list directory) <wildcardspec>", 
                      "MKD",  c_mkdir,             FL_LI, "(make directory) <directoryspec>", 
                      "MODE", c_transmode,   FL_DL|FL_LI, "(set transfer mode) S/B/C", 
                      "NLST", c_namelist,    FL_DL|FL_LI, "(list filenames) <wildcardspec>", 
                      "NOOP", c_noop,                  0, "(no-operation)", 
                      "PASS", c_password,              0, "(supply password) <password>", 
                      "PASV", c_passive,     FL_DL|FL_LI, "(passive mode)", 
                      "PORT", c_dataport,    FL_DL|FL_LI, "(open alternate dataport) ipaddr,portno", 
                      "PWD",  c_printwd,           FL_LI, "(print current directory)", 
                      "QUIT", c_quit,                  0, "(close control connection)", 
                      "REIN", NULL,                    0, NULL, 
                      "REST", NULL,          FL_DL|FL_LI, "(restart transfer) <resume point>", 
                      "RETR", c_retrieve,    FL_DL|FL_LI, "(retrieve from file) <filename>", 
                      "RMD",  NULL,                FL_LI, "(remove directory) <directoryname>", 
                      "RNFR", c_renamefrom,        FL_LI, "(rename file from) <fromfilename>", 
                      "RNTO", c_renameto,          FL_LI, "(rename file to) <tofilename>", 
                      "SITE", NULL,                    0, NULL, 
                      "SIZE", c_filesize,          FL_LI, "(file size) <filename>", 
                      "STAT", c_status,            FL_LI, "(status) [<wildcardspec>]", 
                      "SMNT", NULL,                    0, NULL, 
                      "STRU", c_filestruct,  FL_DL|FL_LI, "(file structure) ??", 
                      "STOR", c_store,       FL_DL|FL_LI, "(store to file) <filename>", 
                      "STOU", NULL,                    0, "(store to unique file) <filename>", 
                      "SYST", c_system,                0, "(show operating system type)", 
                      "TYPE", c_reptype,           FL_LI, "(representation type) ??", 
                      "USER", c_username,              0, "(username to log in) <username>", 
                        NULL, NULL, 0, NULL
                    };

/************************************************************************/
/*									*/
/*  This routine is forked as a process by main when there is an 	*/
/*  incoming connection request						*/
/*									*/
/*    Input:								*/
/*									*/
/*	conctx = pointer to connection context block			*/
/*									*/
/************************************************************************/

static uLong incoming (void *conctxv)

{
  char commandbuf[4096], *p;
  const char *q;
  Conctx *conctx;
  int i, l;
  OZ_IO_ip_tcpreceive ip_tcpreceive;
  uLong commandlen, commandofs, sts;

  conctx = conctxv;

  /* Set up default data port to be the client's */

  memcpy (conctx -> dataipaddr, conctx -> clientipaddr, OZ_IO_IP_ADDRSIZE);
  memcpy (conctx -> dataportno, conctx -> clientportno, OZ_IO_IP_PORTSIZE);

  /* Our outgoing port number is the well known port - 1 */

  sts = OZ_IP_N2HW (lclportno) - 1;
  OZ_IP_H2NW (sts, conctx -> dsrcportno);

  /* Send reply saying we're ready */

  sendreply (conctx, "220 OZONE %s ready to service %s\r\n", pn, conctx -> clientname);

  /* Read and process commands from the client */

  memset (&ip_tcpreceive, 0, sizeof ip_tcpreceive);
  ip_tcpreceive.rawrlen = &commandlen;

read_command:
  for (commandofs = 0; commandofs < sizeof commandbuf - 1;) {
    ip_tcpreceive.rawsize = sizeof commandbuf - 1 - commandofs;
    ip_tcpreceive.rawbuff = commandbuf + commandofs;
    sts = oz_sys_io (OZ_PROCMODE_KNL, conctx -> h_ctrllink, 0, OZ_IO_IP_TCPRECEIVE, sizeof ip_tcpreceive, &ip_tcpreceive);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u reading command from %s\n", pn, sts, conctx -> clientname);
      return (sts);
    }
    commandofs += commandlen;
    if ((commandofs > 0) && (commandbuf[commandofs-1] == '\n')) break;
  }

  /* Remove trailing spaces from command line and null terminate it */

  while ((commandofs > 0) && (commandbuf[commandofs-1] <= ' ')) commandofs --;
  commandbuf[commandofs] = 0;

  /* Find first non-blank char in command line and decode command */

  for (p = commandbuf; *p != 0; p ++) if (*p > ' ') break;

  for (i = 0; (q = cmdtbl[i].name) != NULL; i ++) {
    l = strlen (q);
    if ((p[l] <= ' ') && (strncasecmp (p, q, l) == 0)) break;
  }

  if (flag_verbose) {
    if (cmdtbl[i].entry != c_password) oz_sys_io_fs_printf (oz_util_h_output, "%s: %s >%s\n", pn, conctx -> clientname, p);
    else oz_sys_io_fs_printf (oz_util_h_output, "%s: %s >%s ...\n", pn, conctx -> clientname, cmdtbl[i].name);
  }

  /* Process given command line */

  if (q == NULL) {
    sendreply (conctx, "500 unknown command %s\r\n", p);			/* couldn't find command in table */
  } else if (cmdtbl[i].entry == NULL) {
    sendreply (conctx, "502 command %s not implemented\r\n", cmdtbl[i].name);	/* in table, but routine is not there */
  } else if ((cmdtbl[i].flags & FL_LI) && !(conctx -> loggedin)) {
    sendreply (conctx, "530 command requires being logged in\r\n");		/* you have to be logged in to do this one */
  } else if ((cmdtbl[i].flags & FL_DL) && (conctx -> transferinprog)) {
    sendreply (conctx, "420 data transfer already in progress\r\n");		/* the datalink can't already be in use */
  } else {
    p += l;									/* ok, point to non-blank parameter */
    while ((*p != 0) && (*p <= ' ')) p ++;
    oz_sys_thread_setast (OZ_ASTMODE_INHIBIT);					/* inhibit ast so context won't change on us */
    (*(cmdtbl[i].entry)) (conctx, p);						/* process the command */
    oz_sys_thread_setast (OZ_ASTMODE_ENABLE);					/* enable ast delivery */
  }

  /* Get another command */

  goto read_command;
}

/************************************************************************/
/*									*/
/*  ABOR								*/
/*									*/
/************************************************************************/

static void c_abort (Conctx *conctx, char *params)

{
  if (conctx -> transferinprog) {					/* see if transfer is in progress */
    conctx -> transferinprog = -1;					/* if so, flag that an abort command was received */
    oz_sys_io_abort (OZ_PROCMODE_KNL, conctx -> h_datalink);		/* abort any network i/o in progress */
    oz_sys_io_abort (OZ_PROCMODE_KNL, conctx -> h_datafile);		/* abort any file i/o in progress */
  } else {
    closedatacon (conctx);						/* no transfer going, make sure datalink is closed */
    sendreply (conctx, "226 no transfer was in progress\r\n");		/* ... then reply */
  }
}

/************************************************************************/
/*									*/
/*  CHUP								*/
/*									*/
/************************************************************************/

static void c_changeup (Conctx *conctx, char *params)

{
  c_changedir (conctx, "../");
}

/************************************************************************/
/*									*/
/*  CWD <directory>							*/
/*									*/
/************************************************************************/

static void c_changedir (Conctx *conctx, char *params)

{
  char devnambuff[OZ_DEVUNIT_NAMESIZE], dirnambuff[OZ_FS_MAXFNLEN];
  OZ_Handle h_dir;
  OZ_IO_fs_getinfo1 fs_getinfo1;
  OZ_IO_fs_open fs_open;
  uLong sts;

  /* Open the directory as a file - but we don't need access to the contents */

  if ((params[0] == '/') && (strchr (params, ':') != NULL)) params ++;

  memset (&fs_open, 0, sizeof fs_open);
  fs_open.name      = params;
  fs_open.lockmode  = OZ_LOCKMODE_NL;
  fs_open.rnamesize = sizeof dirnambuff;
  fs_open.rnamebuff = dirnambuff;
  sts = oz_sys_io_fs_open2 (sizeof fs_open, &fs_open, 0, conctx -> defaultdir, &h_dir);
  if (sts != OZ_SUCCESS) {
    sendreply (conctx, "550 error %u accessing directory %s\r\n", sts, params);
    return;
  }

  /* Make sure it is a directory */

  memset (&fs_getinfo1, 0, sizeof fs_getinfo1);
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_dir, 0, OZ_IO_FS_GETINFO1, sizeof fs_getinfo1, &fs_getinfo1);
  if (sts != OZ_SUCCESS) {
    sendreply (conctx, "550 error %u getting info for directory %s\r\n", sts, params);
    goto closedir;
  }

  if (!(fs_getinfo1.filattrflags & OZ_FS_FILATTRFLAG_DIRECTORY)) {
    sendreply (conctx, "550 file %s is not a directory\r\n", params);
    goto closedir;
  }

  /* Set up new name */

  sts = oz_sys_iochan_getunitname (h_dir, sizeof devnambuff, devnambuff);
  if (sts != OZ_SUCCESS) {
    sendreply (conctx, "550 error %u getting directory's device\r\n", sts);
    goto closedir;
  }

  oz_sys_sprintf (sizeof conctx -> defaultdir, conctx -> defaultdir, "%s:%s", devnambuff, dirnambuff);
  sendreply (conctx, "250 default directory now %s\r\n", conctx -> defaultdir);

closedir:
  oz_sys_handle_release (OZ_PROCMODE_KNL, h_dir);
}

/************************************************************************/
/*									*/
/*  DELE <wildcardspec>							*/
/*									*/
/************************************************************************/

static uLong deleteafile (void *conctxv, const char *devname, const char *wildcard, const char *instance, OZ_Handle h_ioch, uLong fileidsize, void *fileidbuff, OZ_FS_Subs *wildsubs, OZ_FS_Subs *instsubs);

static void c_delete (Conctx *conctx, char *params)

{
  uLong sts;

  sts = oz_sys_io_fs_wildscan3 (params, OZ_SYS_IO_FS_WILDSCAN_DELAYDIR, conctx -> defaultdir, deleteafile, conctx);
  if (sts == OZ_IOFSPARSECONT) sendreply (conctx, "250 delete completed\r\n");
  else sendreply (conctx, "550 error %u deleting %s\r\n", sts, params);
}

static uLong deleteafile (void *conctxv, const char *devname, const char *wildcard, const char *instance, OZ_Handle h_ioch, uLong fileidsize, void *fileidbuff, OZ_FS_Subs *wildsubs, OZ_FS_Subs *instsubs)

{
  char rnamebuff[OZ_FS_MAXFNLEN];
  Conctx *conctx;
  OZ_Handle h_iochan;
  OZ_IO_fs_remove fs_remove;
  uLong sts;

  conctx = conctxv;

  if (h_ioch != 0) {
    sendreply (conctx, "550-can't delete an I/O channel %s\r\n", instance);
    return (OZ_BADPARAM);
  }

  sts = oz_sys_io_assign (OZ_PROCMODE_KNL, &h_iochan, devname, OZ_LOCKMODE_CW);
  if (sts != OZ_SUCCESS) {
    sendreply (conctx, "550-error %u assigning channel to %s\r\n", sts, devname);
    return (sts);
  }

  memset (&fs_remove, 0, sizeof fs_remove);
  fs_remove.name      = instance;
  fs_remove.rnamesize = sizeof rnamebuff;
  fs_remove.rnamebuff = rnamebuff;
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_iochan, 0, OZ_IO_FS_REMOVE, sizeof fs_remove, &fs_remove);
  oz_sys_handle_release (OZ_PROCMODE_KNL, h_iochan);
  if (sts != OZ_SUCCESS) {
    sendreply (conctx, "550-error %u deleting %s:%s\r\n", sts, devname, instance);
    return (sts);
  }
  sendreply (conctx, "250-deleted %s:%s\r\n", devname, rnamebuff);
  return (OZ_IOFSWILDSCANCONT);
}

/************************************************************************/
/*									*/
/*  HELP								*/
/*									*/
/************************************************************************/

static void c_help (Conctx *conctx, char *params)

{
  int i;

  sendreply (conctx, "211-available commands\r\n\r\n");
  for (i = 0; cmdtbl[i].name != NULL; i ++) {
    if (cmdtbl[i].entry == NULL) continue;
    sendreply (conctx, "  %8s  %s\r\n", cmdtbl[i].name, cmdtbl[i].help);
  }
  sendreply (conctx, "\r\n211 completed\r\n");
}

/************************************************************************/
/*									*/
/*  LIST <wildcardspec>							*/
/*									*/
/************************************************************************/

static uLong listdirfile (void *conctxv, const char *devname, const char *wildcard, const char *instance, OZ_Handle h_ioch, uLong fileidsize, void *fileidbuff, OZ_FS_Subs *wildsubs, OZ_FS_Subs *instsubs);

static void c_listdir (Conctx *conctx, char *params)

{
  uLong sts;

  /* Make sure data connection open */

  if ((conctx -> h_datalink == 0) && !opendatacon (conctx, 1)) return;

  /* Start scannin' */

  conctx -> keepversions = 1;
  if (params[0] == 0) conctx -> keepversions = 0;
  conctx -> lastfilename[0] = 0;
  sendreply (conctx, "150 scanning %s\r\n", params);
  conctx -> transferinprog = 1;
  sts = oz_sys_io_fs_wildscan3 (params, OZ_SYS_IO_FS_WILDSCAN_DIRLIST, conctx -> defaultdir, listdirfile, conctx);
  if (sts == OZ_IOFSPARSECONT) sendreply (conctx, "250 scan complete\r\n");
  else sendreply (conctx, "550 error %u scanning %s\r\n", sts, params);

  /* Close data link */

  transfercomplete (conctx);
}

static uLong listdirfile (void *conctxv, const char *devname, const char *wildcard, const char *instance, OZ_Handle h_ioch, uLong fileidsize, void *fileidbuff, OZ_FS_Subs *wildsubs, OZ_FS_Subs *instsubs)

{
  char *p, yearbuf[8];
  Conctx *conctx;
  int i, j;
  OZ_Handle h_iochan;
  OZ_IO_fs_getinfo1 fs_getinfo1;
  OZ_IO_fs_open fs_open;
  OZ_IO_ip_tcptransmit ip_tcptransmit;
  uLong itsdatelongs[OZ_DATELONG_ELEMENTS], itsyyyymmdd, nowdatelongs[OZ_DATELONG_ELEMENTS], nowyyyymmdd, sts;

  conctx = conctxv;

  /* Can't list out an I/O channel logical name */

  if (h_ioch != 0) {
    sendreply (conctx, "550-can't list out an I/O channel %s\r\n", instance);
    return (OZ_BADPARAM);
  }

  /* If we're just outputting the latest versions of stuff (ie, like unix), make sure this is a different filename */

  if (!(conctx -> keepversions)) {
    p = strchr (instance, ';');
    if ((p != NULL) && (memcmp (conctx -> lastfilename, instance, p - instance) == 0)) return (OZ_IOFSWILDSCANCONT);
    strncpyz (conctx -> lastfilename, instance, sizeof conctx -> lastfilename);
  }

  /* Assign and I/O channel to the disk being listed out */

  sts = oz_sys_io_assign (OZ_PROCMODE_KNL, &h_iochan, devname, OZ_LOCKMODE_NL);
  if (sts != OZ_SUCCESS) {
    sendreply (conctx, "550-error %u assigning channel to device %s\r\n", sts, devname);
    return (sts);
  }

  /* Open the file */

  memset (&fs_open, 0, sizeof fs_open);
  if (fileidsize != 0) {
    fs_open.fileidsize = fileidsize;
    fs_open.fileidbuff = fileidbuff;
  } else {
    fs_open.name       = instance;
  }
  fs_open.lockmode = OZ_LOCKMODE_NL;
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_iochan, 0, OZ_IO_FS_OPEN, sizeof fs_open, &fs_open);
  if (sts != OZ_SUCCESS) {
    sendreply (conctx, "550-error %u opening file %s:%s\r\n", sts, devname, instance);
    oz_sys_handle_release (OZ_PROCMODE_KNL, h_iochan);
    return (sts);
  }

  /* Get the file's attributes */

  memset (&fs_getinfo1, 0, sizeof fs_getinfo1);
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_iochan, 0, OZ_IO_FS_GETINFO1, sizeof fs_getinfo1, &fs_getinfo1);
  oz_sys_handle_release (OZ_PROCMODE_KNL, h_iochan);
  if (sts != OZ_SUCCESS) {
    sendreply (conctx, "550-error %u getting info for file %s:%s\r\n", sts, devname, instance);
    return (sts);
  }

  /* Convert the date/time quadwords to a unix-like string */

  oz_sys_datebin_decode (oz_hw_tod_getnow (), nowdatelongs);
  nowyyyymmdd = oz_sys_daynumber_decode (nowdatelongs[OZ_DATELONG_DAYNUMBER]);
  oz_sys_datebin_decode (fs_getinfo1.modify_date, itsdatelongs);
  itsyyyymmdd = oz_sys_daynumber_decode (itsdatelongs[OZ_DATELONG_DAYNUMBER]);

  if ((itsyyyymmdd >> 16) != (nowyyyymmdd >> 16)) oz_sys_sprintf (sizeof yearbuf, yearbuf, " %4.4u", itsyyyymmdd >> 16);
  else oz_sys_sprintf (sizeof yearbuf, yearbuf, "%2.2u:%2.2u", itsdatelongs[OZ_DATELONG_SECOND] / 3600, (itsdatelongs[OZ_DATELONG_SECOND] / 60) % 60);

  /* Output only the portion that differs from the wildcard string */

  for (i = 0; (wildcard[i] != 0) && (instance[i] != 0); i ++) if (wildcard[i] != instance[i]) break;

  /* If not outputting version numbers, chop off the version number */

  if (!(conctx -> keepversions)) {
    p = strchr (instance, ';');
    if (p != NULL) *p = 0;
  }

  /* Create the output string */

  oz_sys_sprintf (conctx -> datasize, conctx -> databuff, "%crwxrwxrwx   1 owner    group      %10u %3s %2u %5s %s\r\n", 
                  (fs_getinfo1.filattrflags & OZ_FS_FILATTRFLAG_DIRECTORY) ? 'd' : '-', 
                  ((fs_getinfo1.eofblock - 1) * fs_getinfo1.blocksize) + fs_getinfo1.eofbyte, 
                  months[((itsyyyymmdd>>8)&0xFF)-1], itsyyyymmdd & 0xFF, yearbuf, instance + i);

  /* Transmit it */

  memset (&ip_tcptransmit, 0, sizeof ip_tcptransmit);
  ip_tcptransmit.rawsize = strlen (conctx -> databuff);
  ip_tcptransmit.rawbuff = conctx -> databuff;
  sts = oz_sys_io (OZ_PROCMODE_KNL, conctx -> h_datalink, 0, OZ_IO_IP_TCPTRANSMIT, sizeof ip_tcptransmit, &ip_tcptransmit);

  /* If successful, continue scanning */

  if (sts == OZ_SUCCESS) sts = OZ_IOFSWILDSCANCONT;  
  return (sts);
}

/************************************************************************/
/*									*/
/*  MKD <directory>							*/
/*									*/
/************************************************************************/

static void c_mkdir (Conctx *conctx, char *params)

{
  char device[OZ_DEVUNIT_NAMESIZE], result[OZ_FS_MAXFNLEN];
  OZ_Handle h_iochan;
  OZ_IO_fs_create fs_create;
  uLong sts;

  memset (&fs_create, 0, sizeof fs_create);				// clear stuff we don't use
  fs_create.name = params;						// set up name of directory we're creating
  fs_create.lockmode = OZ_LOCKMODE_EX;					// what mode to put channel in
  fs_create.filattrflags = OZ_FS_FILATTRFLAG_DIRECTORY;			// say that is is a directory (not a regular file)
  fs_create.rnamesize = sizeof result;					// how big the resultant name string buffer is
  fs_create.rnamebuff = result;						// where the resultant name string buffer is

  sts = oz_sys_io_fs_create2 (sizeof fs_create, &fs_create, 0, conctx -> defaultdir, &h_iochan);
  if (sts != OZ_SUCCESS) sendreply (conctx, "550 error %u creating %s\r\n", sts, params);
  else {
    sts = oz_sys_iochan_getunitname (h_iochan, sizeof device, device);	// get name of disk it was created on
    if (sts != OZ_SUCCESS) device[0] = 0;
    oz_sys_handle_release (OZ_PROCMODE_KNL, h_iochan);			// close the directory
    sendreply (conctx, "250 created directory %s:%s\r\n", device, result);
  }
}

/************************************************************************/
/*									*/
/*  MODE S/B/C								*/
/*									*/
/************************************************************************/

static void c_transmode (Conctx *conctx, char *params)

{
  if ((*params != 'S') /** && (*params != 'B') && (*params != 'C') **/) {
    sendreply (conctx, "504 invalid transfer mode %c\r\n", *params);
    return;
  }
  conctx -> transmode = *params;
  sendreply (conctx, "200 transfer mode set to %c\r\n", conctx -> transmode);
}

/************************************************************************/
/*									*/
/*  NLST <wildcardspec>							*/
/*									*/
/************************************************************************/

static uLong namelistfile (void *conctxv, const char *devname, const char *wildcard, const char *instance, OZ_Handle h_ioch, uLong fileidsize, void *fileidbuff, OZ_FS_Subs *wildsubs, OZ_FS_Subs *instsubs);

static void c_namelist (Conctx *conctx, char *params)

{
  uLong sts;

  /* Make sure data connection open */

  if ((conctx -> h_datalink == 0) && !opendatacon (conctx, 1)) return;

  /* Start scannin' */

  sendreply (conctx, "150 scanning %s\r\n", params);
  conctx -> transferinprog = 1;
  sts = oz_sys_io_fs_wildscan3 (params, OZ_SYS_IO_FS_WILDSCAN_DIRLIST, conctx -> defaultdir, namelistfile, conctx);
  if (sts != OZ_IOFSPARSECONT) sendreply (conctx, "550 error %u scanning %s\r\n", sts, params);
  else sendreply (conctx, "250 scan complete\r\n");

  /* Close data link */

  transfercomplete (conctx);
}

static uLong namelistfile (void *conctxv, const char *devname, const char *wildcard, const char *instance, OZ_Handle h_ioch, uLong fileidsize, void *fileidbuff, OZ_FS_Subs *wildsubs, OZ_FS_Subs *instsubs)

{
  Conctx *conctx;
  OZ_IO_ip_tcptransmit ip_tcptransmit;
  uLong sts;

  conctx = conctxv;

  if (h_ioch != 0) {
    sendreply (conctx, "550-can't scan an I/O channel %s\r\n", instance);
    return (OZ_BADPARAM);
  }

  oz_sys_sprintf (conctx -> datasize, conctx -> databuff, "%s:%s\r\n", devname, instance);
  memset (&ip_tcptransmit, 0, sizeof ip_tcptransmit);
  ip_tcptransmit.rawsize = strlen (conctx -> databuff);
  ip_tcptransmit.rawbuff = conctx -> databuff;
  sts = oz_sys_io (OZ_PROCMODE_KNL, conctx -> h_datalink, 0, OZ_IO_IP_TCPTRANSMIT, sizeof ip_tcptransmit, &ip_tcptransmit);
  if (sts == OZ_SUCCESS) sts = OZ_IOFSWILDSCANCONT;  
  return (sts);
}

/************************************************************************/
/*									*/
/*  NOOP								*/
/*									*/
/************************************************************************/

static void c_noop (Conctx *conctx, char *params)

{
  sendreply (conctx, "200 ok\r\n");
}

/************************************************************************/
/*									*/
/*  PASS <password>							*/
/*									*/
/************************************************************************/

typedef struct { char *username;
                 const char *em;
                 OZ_Handle h_user;
               } Bup;

static void *secmalloc (void *dummy, uLong osize, void *obuff, uLong nsize);
static uLong becomeuser (OZ_Procmode cprocmode, void *bupv);

static void c_password (Conctx *conctx, char *params)

{
  Bup bup;
  char *defdirbuff, passhash[OZ_PASSWORD_HASHSIZE], pwhash[OZ_PASSWORD_HASHSIZE];
  int index;
  OZ_Datebin now;
  uLong sts;

  OZ_Password_item pwitems[2] = { OZ_PASSWORD_CODE_PASSHASH, sizeof pwhash,      pwhash,     NULL, 
                                  OZ_PASSWORD_CODE_DEFDIRP,  sizeof defdirbuff, &defdirbuff, NULL };

  oz_sys_handle_release (OZ_PROCMODE_KNL, conctx -> h_user);
  conctx -> loggedin = 0;
  conctx -> h_user   = 0;

  oz_sys_password_hashit (params, sizeof passhash, passhash);

  /* Validate username/password and get the users default directory */

  index = -1;
  sts = oz_sys_password_getbyusername (conctx -> username, 2, pwitems, &index, secmalloc, NULL);
  if ((sts != OZ_SUCCESS) || (strcmp (passhash, pwhash) != 0)) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: user %s via %s pwd error %u [%d]\n", pn, conctx -> username, conctx -> clientname, sts, index);
    sendreply (conctx, "530 invalid login\r\n");
    goto cleanup;
  }

  /* Password validates, set default directory */

  strncpyz (conctx -> defaultdir, defdirbuff, sizeof conctx -> defaultdir);

  /* If different user than me, try to set user's security environment */

  if (strcmp (conctx -> username, myusername) != 0) {
    bup.username = conctx -> username;
    sts = oz_sys_callknl (becomeuser, &bup);
    if (sts != OZ_SUCCESS) {
      sendreply (conctx, "530 error %u %s\r\n", sts, bup.em);
      goto cleanup;
    }
  }

  /* Success, mark us logged in */

  sendreply (conctx, "230 user %s logged in\r\n", conctx -> username);
  conctx -> loggedin = 1;
  now = oz_hw_tod_getnow ();
  oz_sys_io_fs_printf (oz_util_h_error, "%s: user %s connected via %s at %19.19t\n", pn, conctx -> username, conctx -> clientname, now);

  /* Free off temp buffers */

cleanup:
  if (!(conctx -> loggedin)) conctx -> username[0] = 0;
  if (index >= 2) free (defdirbuff);
}

/* Alloc memory for security structs */

static void *secmalloc (void *dummy, uLong osize, void *obuff, uLong nsize)

{
  void *nbuff;

  nbuff = NULL;
  if (nsize != 0) {
    nbuff = malloc (nsize);
    memcpy (nbuff, obuff, osize);
  }
  if (obuff != NULL) free (obuff);
  return (nbuff);
}

/* Modify current thread to the security environment of the user that just logged in.  If  */
/* successful, return an handle to the user block of that user, which, when released, will */
/* make the user appear to have logged out (assuming he's not logged in somewhere else).   */

static uLong becomeuser (OZ_Procmode cprocmode, void *bupv)

{
  Bup *bup;
  int si;
  OZ_User *user;
  uLong sts;

  bup = bupv;

  si  = oz_hw_cpu_setsoftint (0);						/* we don't want to be aborted in here anywhere */
  sts = oz_knl_user_become (bup -> username, &(bup -> em), &user);		/* become the user, return pointer to user block with ref count incd */
  if (sts == OZ_SUCCESS) {
    sts = oz_knl_handle_assign (user, cprocmode, &(bup -> h_user));		/* assign handle to user block, when thread exits, handle gets released */
    if (sts != OZ_SUCCESS) bup -> em = "assigning handle to user struct";
    oz_knl_user_increfc (user, -1);						/* dec refcount to user block (user_become incd it) */
  }
  oz_hw_cpu_setsoftint (si);
  return (sts);
}

/************************************************************************/
/*									*/
/*  PASV								*/
/*									*/
/************************************************************************/

static void c_passive (Conctx *conctx, char *params)

{
  char *p, strbuf[(OZ_IO_IP_ADDRSIZE+OZ_IO_IP_PORTSIZE)*4];
  int i;
  OZ_IO_ip_tcpgetinfo1 ip_tcpgetinfo1;
  OZ_IO_ip_tcplisten ip_tcplisten;
  uLong sts;

  /* Close off any existing data connection */

  closedatacon (conctx);

  /* Close off any old passive link stuff */

  oz_sys_handle_release (OZ_PROCMODE_KNL, conctx -> h_pasvlink);
  oz_sys_handle_release (OZ_PROCMODE_KNL, conctx -> h_pasvwait);
  conctx -> h_pasvlink = 0;
  conctx -> h_pasvwait = 0;

  /* Get local ip address of the control link (ie, what interface they are connected on) */

  memset (&ip_tcpgetinfo1, 0, sizeof ip_tcpgetinfo1);
  ip_tcpgetinfo1.addrsize  = OZ_IO_IP_ADDRSIZE;
  ip_tcpgetinfo1.portsize  = OZ_IO_IP_PORTSIZE;
  ip_tcpgetinfo1.lclipaddr = conctx -> pasvipaddr;
  sts = oz_sys_io (OZ_PROCMODE_KNL, conctx -> h_ctrllink, 0, OZ_IO_IP_TCPGETINFO1, sizeof ip_tcpgetinfo1, &ip_tcpgetinfo1);
  if (sts != OZ_SUCCESS) {
    sendreply (conctx, "451 error %u getting control link ip address\r\n", sts);
    return;
  }

  /* Create a new socket for client to connect to */

  sts = oz_sys_io_assign (OZ_PROCMODE_KNL, &(conctx -> h_pasvlink), OZ_IO_IP_DEV, OZ_LOCKMODE_CW);
  if (sts != OZ_SUCCESS) {
    sendreply (conctx, "451 error %u creating passive socket\r\n", sts);
    conctx -> h_pasvlink = 0;
    return;
  }

  /* Create event flag to be set when listen is complete */

  sts = oz_sys_event_create (OZ_PROCMODE_KNL, "passive listening", &(conctx -> h_pasvwait));
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);

  /* Start listening on an ephemeral socket on that same interface */
  /* If started with -restrictports, only listen to client's ip    */

  memset (&ip_tcplisten, 0, sizeof ip_tcplisten);
  memset (conctx -> pasvportno, 0, sizeof conctx -> pasvportno);
  ip_tcplisten.addrsize   = OZ_IO_IP_ADDRSIZE;
  ip_tcplisten.portsize   = OZ_IO_IP_PORTSIZE;
  ip_tcplisten.lclipaddr  = conctx -> pasvipaddr;			// listen only on the same interface
  ip_tcplisten.lclportno  = conctx -> pasvportno;			// tell it to use ephemeral socket number
  ip_tcplisten.windowsize = 1;						// minimal window for now, we fix it later
  if (flag_restrictports) ip_tcplisten.remipaddr = conctx -> clientipaddr;

  conctx -> pasvstatus = OZ_PENDING;
  sts = oz_sys_io_start (OZ_PROCMODE_KNL, 
                         conctx -> h_pasvlink, 
                         &(conctx -> pasvstatus), 
                         conctx -> h_pasvwait, 
                         NULL, NULL, 
                         OZ_IO_IP_TCPLISTEN, 
                         sizeof ip_tcplisten, 
                         &ip_tcplisten);
  if (sts != OZ_STARTED) {
    sendreply (conctx, "451 error %u starting listen on passive socket\r\n", sts);
    oz_sys_handle_release (OZ_PROCMODE_KNL, conctx -> h_pasvlink);
    oz_sys_handle_release (OZ_PROCMODE_KNL, conctx -> h_pasvwait);
    conctx -> h_pasvlink = 0;
    conctx -> h_pasvwait = 0;
    return;
  }

  /* Get the ephemeral socket number that we are listening on now */

  memset (&ip_tcpgetinfo1, 0, sizeof ip_tcpgetinfo1);
  ip_tcpgetinfo1.addrsize  = OZ_IO_IP_ADDRSIZE;
  ip_tcpgetinfo1.portsize  = OZ_IO_IP_PORTSIZE;
  ip_tcpgetinfo1.lclportno = conctx -> pasvportno;
  sts = oz_sys_io (OZ_PROCMODE_KNL, conctx -> h_pasvlink, 0, OZ_IO_IP_TCPGETINFO1, sizeof ip_tcpgetinfo1, &ip_tcpgetinfo1);
  if (sts != OZ_SUCCESS) {
    sendreply (conctx, "451 error %u getting data link port number\r\n", sts);
    oz_sys_handle_release (OZ_PROCMODE_KNL, conctx -> h_pasvlink);
    oz_sys_handle_release (OZ_PROCMODE_KNL, conctx -> h_pasvwait);
    conctx -> h_pasvlink = 0;
    conctx -> h_pasvwait = 0;
    return;
  }

  /* Send resultant ip address and port number back to client so it knows what to connect to */

  oz_hw_itoa (conctx -> pasvipaddr[0], 4, strbuf);
  p = strbuf + strlen (strbuf);
  for (i = 1; i < OZ_IO_IP_ADDRSIZE; i ++) {
    *(p ++) = ',';
    oz_hw_itoa (conctx -> pasvipaddr[i], 4, p);
    p += strlen (p);
  }
  for (i = 0; i < OZ_IO_IP_PORTSIZE; i ++) {
    *(p ++) = ',';
    oz_hw_itoa (conctx -> pasvportno[i], 4, p);
    p += strlen (p);
  }
  sendreply (conctx, "227 entering passive mode (%s)\r\n", strbuf);
}

/************************************************************************/
/*									*/
/*  PORT i,p,a,d,p,n							*/
/*									*/
/************************************************************************/

static void c_dataport (Conctx *conctx, char *params)

{
  char ipstr[OZ_IO_IP_ADDRSIZE*4], pnstr[OZ_IO_IP_PORTSIZE*4];
  int i, j;
  uByte ipaddr[OZ_IO_IP_ADDRSIZE], portno[OZ_IO_IP_PORTSIZE];
  uLong v;

  /* Close off any existing data connection */

  closedatacon (conctx);

  /* Close off any old passive link stuff */

  oz_sys_handle_release (OZ_PROCMODE_KNL, conctx -> h_pasvlink);
  oz_sys_handle_release (OZ_PROCMODE_KNL, conctx -> h_pasvwait);
  conctx -> h_pasvlink = 0;
  conctx -> h_pasvwait = 0;

  /* Parse new ip address and port number */

  for (i = 0; i < OZ_IO_IP_ADDRSIZE; i ++) {
    v = oz_hw_atoi (params, &j);
    if (v > 255) goto badv;
    if (params[j++] != ',') goto badv;
    ipaddr[i] = v;
    params   += j;
  }
  for (i = 0; i < OZ_IO_IP_PORTSIZE; i ++) {
    v = oz_hw_atoi (params, &j);
    if (v > 255) goto badv;
    if (params[j++] != ((i == OZ_IO_IP_PORTSIZE - 1) ? 0 : ',')) goto badv;
    portno[i] = v;
    params   += j;
  }

  /* If -restrictports, make sure the ip address is the client's */

  if (flag_restrictports && (memcmp (conctx -> clientipaddr, ipaddr, OZ_IO_IP_ADDRSIZE) != 0)) {
    sendreply (conctx, "501 '-restrictports' mode, can only connect back to requestor\n", params);
    return;
  }

  /* Values ok, copy to context block and acknowledge command */

  memcpy (conctx -> dataipaddr, ipaddr, OZ_IO_IP_ADDRSIZE);
  memcpy (conctx -> dataportno, portno, OZ_IO_IP_PORTSIZE);
  sendreply (conctx, "200 data port set to %s,%s\r\n", cvtipbin (ipaddr, ipstr), cvtpnbin (portno, pnstr));
  return;

badv:
  sendreply (conctx, "501 bad ipaddress/portnumber %s\n", params);
}

/************************************************************************/
/*									*/
/*  PWD									*/
/*									*/
/************************************************************************/

static void c_printwd (Conctx *conctx, char *params)

{
  sendreply (conctx, "257 \"%s\" is current directory\n", conctx -> defaultdir);
}

/************************************************************************/
/*									*/
/*  QUIT								*/
/*									*/
/************************************************************************/

static void terminate (Conctx *conctx);

static void c_quit (Conctx *conctx, char *params)

{
  terminate (conctx);
}

static void terminate (Conctx *conctx)

{
  Retrsendctx *retrsendctx;

  conctx -> hasquit = 1;			/* remember we got a QUIT command */
  if (conctx -> transferinprog) {		/* wait for any transfer going before we quit */
    sendreply (conctx, "100 waiting for transfer to complete\r\n");
    return;
  }
  sendreply (conctx, "221 closing connection\r\n");
  oz_sys_io (OZ_PROCMODE_KNL, conctx -> h_ctrllink, 0, OZ_IO_IP_TCPCLOSE, 0, NULL);
  oz_sys_handle_release (OZ_PROCMODE_KNL, conctx -> h_ctrllink); /* ok, close the control connection */
  oz_sys_io_fs_printf (oz_util_h_error, "%s: user %s disconnected %s\n", pn, conctx -> username, conctx -> clientname);
  oz_sys_handle_release (OZ_PROCMODE_KNL, conctx -> h_user); /* make sure user is logged out */
  while ((retrsendctx = conctx -> freeretrsendctxs) != NULL) {
    conctx -> freeretrsendctxs = retrsendctx -> next;
    free (retrsendctx);
  }
  debstatus (conctx, "ctrl", NULL);
  debstatus (conctx, "data", NULL);

  free (conctx);				/* release the context block */
  oz_sys_thread_exit (OZ_SUCCESS);		/* exit the thread */
}

/************************************************************************/
/*									*/
/*  RETR <filename>							*/
/*									*/
/************************************************************************/

static void retrieveloop (Conctx *conctx);
static void retrieveread (void *conctxv, uLong status, OZ_Mchargs *mchargs);
static void retrievereadproc (Conctx *conctx, uLong status);
static void retrievesent (void *paramv, uLong status, OZ_Mchargs *mchargs);

static void c_retrieve (Conctx *conctx, char *params)

{
  char rnamebuff[OZ_FS_MAXFNLEN], unitname[OZ_DEVUNIT_NAMESIZE];
  OZ_Handle h_file;
  OZ_IO_fs_getinfo1 fs_getinfo1;
  OZ_IO_fs_open fs_open;
  uLong sts;

  /* Open the file */

  if ((params[0] == '/') && (strchr (params, ':') != NULL)) params ++;

  memset (&fs_open, 0, sizeof fs_open);
  fs_open.name      = params;
  fs_open.lockmode  = OZ_LOCKMODE_PR;
  fs_open.rnamesize = sizeof rnamebuff;
  fs_open.rnamebuff = rnamebuff;
  sts = oz_sys_io_fs_open2 (sizeof fs_open, &fs_open, 0, conctx -> defaultdir, &h_file);
  if (sts != OZ_SUCCESS) {
    sendreply (conctx, "550 error %u opening file %s\r\n", sts, params);
    return;
  }

  /* Find out what device it is on */

  sts = oz_sys_iochan_getunitname (h_file, sizeof unitname, unitname);
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (1, sts);

  /* Open data connection */

  debstatus (conctx, "ctrl", "opening data connection");

  if (!opendatacon (conctx, 1)) {
    oz_sys_handle_release (OZ_PROCMODE_KNL, h_file);
    return;
  }

  /* Print a message saying it is being retrieved and how big it is */

  conctx -> transfertotal = 0;
  memset (&fs_getinfo1, 0, sizeof fs_getinfo1);
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_file, 0, OZ_IO_FS_GETINFO1, sizeof fs_getinfo1, &fs_getinfo1);
  if (sts != OZ_SUCCESS) sendreply (conctx, "150 retrieving from %s:%s (size unknown, error %u)\r\n", unitname, rnamebuff, sts);
  else {
    conctx -> transfertotal = (fs_getinfo1.eofblock - 1) * fs_getinfo1.blocksize + fs_getinfo1.eofbyte;
    sendreply (conctx, "150 retrieving from %s:%s (%u bytes)\r\n", unitname, rnamebuff, conctx -> transfertotal);
  }

  /* Start retrievin' */

  debstatus (conctx, "ctrl", "retrieving");

  conctx -> transferinprog  = 1;			// a transfer is now in progress
  conctx -> diskiosofar     = 0;			// no bytes read from disk so far
  conctx -> transfersofar   = 0;			// no bytes written to network so far
  conctx -> readoffset      = 0;			// data being read from disk goes here
  conctx -> writeoffset     = 0;			// data being transmitted is here
  conctx -> readinprog      = 0;			// set to 1 when a disk read (to readoffset) is in progress
  conctx -> writeinprog     = 0;			// number of bytes starting at writeoffset being transmitted
  conctx -> readstatus      = OZ_PENDING;		// changed when disk reads have all completed
  conctx -> writestatus     = OZ_PENDING;		// changed when error transmitting
  conctx -> h_datafile      = h_file;
  conctx -> transferstarted = oz_hw_tod_getnow ();
  conctx -> retrsendctx_qh  = NULL;
  conctx -> retrsendctx_qt  = &(conctx -> retrsendctx_qh);

  retrieveloop (conctx);
}

/************************************************************************/
/*  This routine is the retrieve loop.  It keeps a disk read going and 	*/
/*  as many transmits going at a time as it can.			*/
/************************************************************************/

static void retrieveloop (Conctx *conctx)

{
  int startedsomething;
  OZ_IO_fs_readrec fs_readrec;
  OZ_IO_ip_tcptransmit ip_tcptransmit;
  Retrsendctx *retrsendctx;
  uLong contigfreesize, offs, size, sts, totalfreesize;

  DEBDATASTS (conctx);
  startedsomething = 1;

loop:
  if (!startedsomething) return;
  startedsomething = 0;

  /* If transmit error and no I/O is going, we're all done */

  if (conctx -> writestatus != OZ_PENDING) {
    if ((conctx -> writeinprog == 0) && !(conctx -> readinprog)) transfercomplete (conctx);
    return;
  }

  /* If read hit eof (or error) and there is no data yet to be transmitted, we're all done */

  sts = conctx -> readstatus;
  if ((sts != OZ_PENDING) && (conctx -> writeoffset == conctx -> readoffset)) {
    if (sts != OZ_ENDOFFILE) transfercomplete (conctx);				/* if not eof, some error (message already output) */
    else transfersuccessful (conctx);						/* eof, successful completion */
    return;
  }

  /* If ABORT command received, don't start anything more */

  if (conctx -> transferinprog < 0) {
    if ((conctx -> writeinprog == 0) && !(conctx -> readinprog)) {		/* if reads or transmits are going, wait till they finish */
      sendreply (conctx, "426 transfer aborted via abort command\r\n");		/* nothing going, terminate transfer */
      transfercomplete (conctx);
    }
    return;
  }

  /* If a read is already going, don't start another */

  if (conctx -> readinprog) goto startsend;					/* if set, there is a read already going */
  if (sts != OZ_PENDING) goto startsend;					/* if not pending, we're all done reading */

  /* See how much space is available starting at readoffset up to writeoffset or the end of the buffer */

  if (conctx -> readoffset < conctx -> writeoffset + conctx -> writeinprog) {
    oz_crash ("oz_util_ftpd retrieveread: readoffset %u, writeoffset %u, writeinprog %u", conctx -> readoffset, conctx -> writeoffset, conctx -> writeinprog);
  }

  if (conctx -> writeoffset == conctx -> readoffset) {				/* see if buffer completely empty */
    conctx -> readoffset  = 0;							/* if so, reset pointers to the beginning */
    conctx -> writeoffset = 0;
    DEBDATASTS (conctx);
  }
  offs = conctx -> readoffset;							/* get beg of available space */
  totalfreesize = conctx -> writeoffset;					/* get end of available space */
  if (totalfreesize <= offs) totalfreesize += conctx -> datasize;		/* 'unwrap it' past beg of available space */
  totalfreesize -= offs;							/* subtract beg to get the total available size */
  if (totalfreesize == 0) goto startsend;					/* can't read if no room left */
  if (offs >= conctx -> datasize) offs -= conctx -> datasize;			/* set up block buffer address */
  if (offs >= conctx -> datasize) oz_crash ("oz_util_ftpd retrieveread: offs %u, datasize %u, orig readoffset %u", offs, conctx -> datasize, conctx -> readoffset);
  contigfreesize = totalfreesize;						/* assume it is all contiguous */
  if (contigfreesize > conctx -> datasize - offs) contigfreesize = conctx -> datasize - offs; /* maybe it hails from Compton */

  /* If not image mode, make sure there is enough room to change a whole buffer of \n's to \r\n's */

  if ((conctx -> reptype != 'I') && (contigfreesize > totalfreesize / 2)) {
    contigfreesize = totalfreesize / 2;
    if (contigfreesize == 0) goto startsend;
  }

  /* Start reading from the disk file */

  memset (&fs_readrec, 0, sizeof fs_readrec);
  fs_readrec.size = contigfreesize;						/* set up size to read */
  fs_readrec.buff = conctx -> databuff + offs;					/* set up where to read into */
  fs_readrec.rlen = &(conctx -> datarlen);					/* tell it where to return length actually read */

  conctx -> datarlen   = 0;							/* haven't read anything */
  conctx -> readinprog = 1;							/* remember we have a read going */
  DEBDATASTS (conctx);

  sts = oz_sys_io_start (OZ_PROCMODE_KNL, conctx -> h_datafile, NULL, 0, retrieveread, conctx, 
                         OZ_IO_FS_READREC, sizeof fs_readrec, &fs_readrec);
  startedsomething = 1;
  if (sts != OZ_STARTED) {
    retrievereadproc (conctx, sts);						/* done already, process it */
    goto loop;
  }

  /* Start a write going if there is something to do */

startsend:
  offs = conctx -> writeoffset + conctx -> writeinprog;				/* get offset to stuff we haven't started yet */
  if ((offs < conctx -> datasize) && (conctx -> readoffset > conctx -> datasize)) { /* see if valid stuff wraps around */
    size = conctx -> datasize - offs;						/* if so, see how much there is up to the end */
  } else {
    size = conctx -> readoffset - offs;						/* no wrap, get how much total is valid */
    if (conctx -> readstatus == OZ_PENDING) {					/* if read still has more to go ... */
      size /= conctx -> tcpsendsize;						/* round transmit down to a nice tcp size */
      size *= conctx -> tcpsendsize;
    }
    if (size == 0) goto loop;							/* if nothing to transmit, maybe read more from disk */
    if (offs >= conctx -> datasize) offs -= conctx -> datasize;			/* wrap the starting offset */
  }
  if (size > SENDFACTOR * conctx -> tcpsendsize) size = SENDFACTOR * conctx -> tcpsendsize; /* only this much per i/o request */
										/* ... so we get notified as each part gets done */

  memset (&ip_tcptransmit, 0, sizeof ip_tcptransmit);				/* start sending the data to remote end */
  ip_tcptransmit.rawsize = size;
  ip_tcptransmit.rawbuff = conctx -> databuff + offs;
  retrsendctx = conctx -> freeretrsendctxs;					/* ast needs to know size and offset of write */
  if (retrsendctx != NULL) conctx -> freeretrsendctxs = retrsendctx -> next;
  else {
    retrsendctx = malloc (sizeof *retrsendctx);
    retrsendctx -> conctx = conctx;						/* ... and context block pointer */
  }
  retrsendctx -> next   = NULL;
  retrsendctx -> status = OZ_PENDING;
  retrsendctx -> size   = size;
  retrsendctx -> offs   = offs;
  *(conctx -> retrsendctx_qt) = retrsendctx;
  conctx -> retrsendctx_qt = &(retrsendctx -> next);
  conctx -> writeinprog += size;						/* this much more is going on now */
  DEBDATASTS (conctx);
  sts = oz_sys_io_start (OZ_PROCMODE_KNL, conctx -> h_datalink, NULL, 0, retrievesent, retrsendctx, 
                         OZ_IO_IP_TCPTRANSMIT, sizeof ip_tcptransmit, &ip_tcptransmit);
  startedsomething = 1;
  if (sts != OZ_STARTED) {
    sts = oz_sys_thread_queueast (OZ_PROCMODE_KNL, 0, retrievesent, retrsendctx, 0, sts); /* sync compl, do it via ast */
    if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);			/* ... so they always complete in order */
  }
  goto startsend;								/* see if we can send more */
}

/************************************************************************/
/*  This ast routine is called when a disk read completes		*/
/************************************************************************/

static void retrieveread (void *conctxv, uLong status, OZ_Mchargs *mchargs)

{
  retrievereadproc (conctxv, status);
  retrieveloop (conctxv);
}

static void retrievereadproc (Conctx *conctx, uLong status)

{
  uByte *b, *p, *q;
  uLong i, numlfs, offs;

  conctx -> readinprog = 0;							/* the read is no longer going */
  DEBDATASTS (conctx);
  if ((status != OZ_SUCCESS) && (conctx -> readstatus == OZ_PENDING)) {		/* save final read completion status */
    if (status != OZ_ENDOFFILE) sendreply (conctx, "550 error %u reading file\r\n", status);
    conctx -> readstatus = status;
  }
  conctx -> diskiosofar += conctx -> datarlen;					/* this many more bytes have been read from disk */
  DEBDATASTS (conctx);

  /* If not image mode, translate all LF's to CRLF's */

  numlfs = -1;
  if (conctx -> reptype != 'I') {
    offs = conctx -> readoffset;
    if (offs >= conctx -> datasize) offs -= conctx -> datasize;
    numlfs = 0;									/* haven't found any LF's yet */
    b = conctx -> databuff;							/* point to beginning of buffer */
    p = b + offs;								/* point to beginning of data */
    for (i = conctx -> datarlen; i > 0; -- i) if (*(p ++) == '\n') numlfs ++;	/* count the LF's in the data */
    conctx -> datarlen += numlfs;						/* increase length of data read to include CR's */
    conctx -> transfertotal += numlfs;						/* we're increasing the size of the total transfer that much */
    q = p + numlfs;								/* point to just past where last byte will go */
    if (q > b + conctx -> datasize) q -= conctx -> datasize;			/* wrap the pointer */
    while (q != p) {								/* repeat as long as there are CR's to insert */
      if (q == b) q += conctx -> datasize;					/* maybe wrap pointer */
      if ((*(-- q) = *(-- p)) == '\n') {					/* copy a character at end of buffer */
        if (q == b) q += conctx -> datasize;					/* copied an LF, maybe wrap pointer */
        *(-- q) = '\r';								/* ... then insert an CR before the LF */
      }
    }
  }

  /* Increment offset to include the bytes just read from the file (plus any inserted CR's). */
  /* Do not wrap it, it must always be >= writeoffs.                                         */

  conctx -> readoffset += conctx -> datarlen;
  if (conctx -> readoffset >= conctx -> datasize * 2) oz_crash ("oz_util_ftpd retrievereadproc: datarlen %u, readoffset %u, writeoffset %u, datasize %u, numlfs %u", conctx -> datarlen, conctx -> readoffset, conctx -> writeoffset, conctx -> datasize, numlfs);
  DEBDATASTS (conctx);
}

/************************************************************************/
/* This ast routine is called when the transmit completes       	*/
/************************************************************************/

static void retrievesent (void *paramv, uLong status, OZ_Mchargs *mchargs)

{
  Conctx *conctx;
  Retrsendctx *retrsendctx;
  uLong size, sts;

  retrsendctx = paramv;								/* point to retrieve send context block */
  conctx = retrsendctx -> conctx;						/* get connection context block pointer */
  if (retrsendctx -> status != OZ_PENDING) oz_crash ("oz_util_ftpd retrievesent: status %u already finished was %u", status, retrsendctx -> status);
  retrsendctx -> status = status;						/* save completion status */
  if (retrsendctx != conctx -> retrsendctx_qh) return;				/* just ignore for now if not first on queue */

  do {
    conctx -> retrsendctx_qh = retrsendctx -> next;				/* ok, remove from head of queue */
    if (conctx -> retrsendctx_qh == NULL) conctx -> retrsendctx_qt = &(conctx -> retrsendctx_qh);

    /* Check the transmit status.  If error (like link disconnected), abort the transfer. */

    if ((status != OZ_SUCCESS) && (conctx -> writestatus == OZ_PENDING)) {
      if (status != OZ_ENDOFFILE) sendreply (conctx, "451 error %u sending file\r\n", status);
      conctx -> writestatus = status;						/* save failure status */
    }

    /* Make sure they are completing in order (redundant with queuing) */

    if (retrsendctx -> offs != conctx -> writeoffset) {
      oz_crash ("oz_util_ftpd retrievesent: completed offset %u before %u", retrsendctx -> offs, conctx -> writeoffset);
    }

    /* Decrement write-in-progress counter by the size we just transmitted */

    size = retrsendctx -> size;							/* get the size that was sent */
    conctx -> writeinprog -= size;						/* that much less is being transmitted */
    retrsendctx -> next = conctx -> freeretrsendctxs;
    conctx -> freeretrsendctxs = retrsendctx;

    /* Increment pointer beyond what was transmitted */

    conctx -> writeoffset += size;
    if (conctx -> writeoffset >= conctx -> datasize) {
      conctx -> readoffset  -= conctx -> datasize;
      conctx -> writeoffset -= conctx -> datasize;
    }
    conctx -> transfersofar += size;
    DEBDATASTS (conctx);

    /* If there are more completed transmits on queue, process them */

    retrsendctx = conctx -> retrsendctx_qh;
  } while ((retrsendctx != NULL) && ((status = retrsendctx -> status) != OZ_PENDING));

  /* Maybe more can be read from disk now */

  retrieveloop (conctx);
}

static void debdatasts (Conctx *conctx, int line)

{
  debstatus (conctx, "data", "line %u\ntip %d\nwip %u\nrip %u\nwof %u\nrof %u\nwst %u\nrst %u\nnet %u\ndisk %u", 
	line, conctx -> transferinprog, conctx -> writeinprog, conctx -> readinprog, conctx -> writeoffset, 
	conctx -> readoffset, conctx -> writestatus, conctx -> readstatus, conctx -> transfersofar, 
	conctx -> diskiosofar);
}

static void debstatus (Conctx *conctx, const char *class, const char *format, ...)

{
  char nambuf[128], *p, strbuf[256];
  OZ_Handle h_logname;
  OZ_Logvalue logvalues[16];
  uLong nvalues, sts;
  va_list ap;

  oz_sys_sprintf (sizeof nambuf, nambuf, "OZ_JOB_TABLE%%ftpd_%s_%s", conctx -> clientname, class);

  if (format != NULL) {
    va_start (ap, format);
    oz_sys_vsprintf (sizeof strbuf, strbuf, format, ap);
    va_end (ap);
    memset (logvalues, 0, sizeof logvalues);
    p = strbuf;
    for (nvalues = 0; nvalues < 16;) {
      logvalues[nvalues++].buff = p;
      p = strchr (p, '\n');
      if (p == NULL) break;
      *(p ++) = 0;
    }
    sts = oz_sys_logname_create (0, nambuf, OZ_PROCMODE_USR, 0, nvalues, logvalues, NULL);
    if ((sts != OZ_SUCCESS) && (sts != OZ_SUPERSEDED)) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u creating logical %s\n", pn, sts, nambuf);
    }
  } else {
    sts = oz_sys_logname_lookup (0, OZ_PROCMODE_USR, nambuf, NULL, NULL, NULL, &h_logname);
    if (sts == OZ_SUCCESS) {
      oz_sys_logname_delete (h_logname);
      oz_sys_handle_release (OZ_PROCMODE_KNL, h_logname);
    }
  }
}

/************************************************************************/
/*									*/
/*  RNFR <oldfilename>							*/
/*  RNTO <newfilename>							*/
/*									*/
/************************************************************************/

static void c_renamefrom (Conctx *conctx, char *params)

{
  OZ_Handle h_file;
  OZ_IO_fs_open fs_open;
  uLong sts;

  /* Open the file */

  if ((params[0] == '/') && (strchr (params, ':') != NULL)) params ++;

  memset (&fs_open, 0, sizeof fs_open);
  fs_open.name      = params;
  fs_open.lockmode  = OZ_LOCKMODE_NL;
  sts = oz_sys_io_fs_open2 (sizeof fs_open, &fs_open, 0, conctx -> defaultdir, &h_file);
  if (sts != OZ_SUCCESS) {
    sendreply (conctx, "550 error %u opening file %s\r\n", sts, params);
    return;
  }
  oz_sys_handle_release (OZ_PROCMODE_KNL, h_file);

  strncpyz (conctx -> oldfilename, params, sizeof conctx -> oldfilename);
  sendreply (conctx, "350 file exists, ready for destination name\r\n");
}

static void c_renameto (Conctx *conctx, char *params)

{
  char newnamebuff[OZ_FS_MAXFNLEN], oldrnamebuff[OZ_FS_MAXFNLEN];
  OZ_FS_Subs oldrnamesubs;
  OZ_Handle h_file;
  OZ_IO_fs_open fs_open;
  OZ_IO_fs_rename fs_rename;
  uLong sts;

  /* Make sure we got an RNFR command */

  if (conctx -> oldfilename[0] == 0) {
    sendreply (conctx, "503 requires RNFR command first\r\n");
    return;
  }

  /* Open the file */

  if ((params[0] == '/') && (strchr (params, ':') != NULL)) params ++;

  memset (&fs_open, 0, sizeof fs_open);
  fs_open.name      = conctx -> oldfilename;
  fs_open.lockmode  = OZ_LOCKMODE_NL;
  fs_open.rnamesize = sizeof oldrnamebuff;
  fs_open.rnamebuff = oldrnamebuff;
  fs_open.rnamesubs = &oldrnamesubs;
  sts = oz_sys_io_fs_open2 (sizeof fs_open, &fs_open, 0, conctx -> defaultdir, &h_file);
  if (sts != OZ_SUCCESS) {
    sendreply (conctx, "550 error %u opening file %s\r\n", sts, conctx -> defaultdir);
    return;
  }

  /* Rename the file */

  if (params[0] != '/') {
    memcpy (newnamebuff, oldrnamebuff, oldrnamesubs.dirsize);
    strncpyz (newnamebuff + oldrnamesubs.dirsize, params, sizeof newnamebuff - oldrnamesubs.dirsize);
  } else {
    strncpyz (newnamebuff, params, sizeof newnamebuff);
  }

  memset (&fs_rename, 0, sizeof fs_rename);
  fs_rename.oldname = oldrnamebuff;
  fs_rename.newname = newnamebuff;
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_file, 0, OZ_IO_FS_RENAME, sizeof fs_rename, &fs_rename);
  if (sts == OZ_SUCCESS) sendreply (conctx, "250 rename successful\r\n");
  else sendreply (conctx, "550 error %u renaming file %s to %s\r\n", sts, oldrnamebuff, newnamebuff);

  oz_sys_handle_release (OZ_PROCMODE_KNL, h_file);
  conctx -> oldfilename[0] = 0;
}

/************************************************************************/
/*									*/
/*  SIZE <filename>							*/
/*									*/
/************************************************************************/

static void c_filesize (Conctx *conctx, char *params)

{
  OZ_Handle h_file;
  OZ_IO_fs_getinfo1 fs_getinfo1;
  OZ_IO_fs_open fs_open;
  uLong sts;

  /* Open the file */

  if ((params[0] == '/') && (strchr (params, ':') != NULL)) params ++;

  memset (&fs_open, 0, sizeof fs_open);
  fs_open.name      = params;
  fs_open.lockmode  = OZ_LOCKMODE_NL;
  sts = oz_sys_io_fs_open2 (sizeof fs_open, &fs_open, 0, conctx -> defaultdir, &h_file);
  if (sts != OZ_SUCCESS) {
    sendreply (conctx, "550 error %u opening file %s\r\n", sts, params);
    return;
  }

  /* Get and display its size */

  memset (&fs_getinfo1, 0, sizeof fs_getinfo1);
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_file, 0, OZ_IO_FS_GETINFO1, sizeof fs_getinfo1, &fs_getinfo1);
  oz_sys_handle_release (OZ_PROCMODE_KNL, h_file);
  if (sts != OZ_SUCCESS) sendreply (conctx, "550 error %u getting size of file %s\r\n", sts, params);
  else sendreply (conctx, "213 %u\r\n", (fs_getinfo1.eofblock - 1) * fs_getinfo1.blocksize + fs_getinfo1.eofbyte);
}

/************************************************************************/
/*									*/
/*  STAT								*/
/*									*/
/************************************************************************/

static void c_status (Conctx *conctx, char *params)

{
  if (!(conctx -> transferinprog)) sendreply (conctx, "211 server idle\r\n");
  else sendreply (conctx, "213 transferred %u out of %u bytes so far\r\n", conctx -> transfersofar, conctx -> transfertotal);
}

/************************************************************************/
/*									*/
/*  STRU F/R/P								*/
/*	F - file oriented						*/
/*	R - record oriented						*/
/*	P - page oriented						*/
/*									*/
/************************************************************************/

static void c_filestruct (Conctx *conctx, char *params)

{
  if ((*params != 'F') /** && (*params != 'R') && (*params != 'P') **/) {
    sendreply (conctx, "504 invalid file structure %c\r\n", *params);
    return;
  }
  conctx -> filestruct = *params;
  sendreply (conctx, "200 file structure set to %c\r\n", conctx -> filestruct);
}

/************************************************************************/
/*									*/
/*  STOR <filename>							*/
/*									*/
/************************************************************************/

static void storerecvstart (Conctx *conctx);
static void storerecvcomplete (void *conctxv, uLong status, OZ_Mchargs *mchargs);
static void storewritecomplete (void *conctxv, uLong status, OZ_Mchargs *mchargs);

static void c_store (Conctx *conctx, char *params)

{
  char rnamebuff[OZ_FS_MAXFNLEN], unitname[OZ_DEVUNIT_NAMESIZE];
  OZ_Handle h_file;
  OZ_IO_fs_create fs_create;
  uLong sts;

  /* Create the file */

  memset (&fs_create, 0, sizeof fs_create);
  fs_create.name      = params;
  fs_create.lockmode  = OZ_LOCKMODE_PW;
  fs_create.rnamesize = sizeof rnamebuff;
  fs_create.rnamebuff = rnamebuff;
  sts = oz_sys_io_fs_create2 (sizeof fs_create, &fs_create, 0, conctx -> defaultdir, &h_file);
  if (sts != OZ_SUCCESS) {
    sendreply (conctx, "550 error %u creating file %s\r\n", sts, params);
    return;
  }

  /* Find out what device it is on */

  sts = oz_sys_iochan_getunitname (h_file, sizeof unitname, unitname);
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (1, sts);

  /* Make sure data connection open */

  if ((conctx -> h_datalink == 0) && !opendatacon (conctx, 0)) return;

  /* Print a message saying it is being stored and how big it is */

  conctx -> transfertotal = 0;
  sendreply (conctx, "150 storing to %s:%s\r\n", unitname, rnamebuff);

  /* Start storin' */

  conctx -> transferinprog  = 1;		/* we have a transfer going now */
  conctx -> transfersofar   = 0;		/* haven't recieved any data yet */
  conctx -> diskiosofar     = 0;		/* haven't written any data yet */
  conctx -> transfertotal   = 0;		/* haven't written any to disk yet */
  conctx -> rcvwindowrem    = 0;		/* reset buffer offsets */
  conctx -> nextwriteoffs   = 0;
  conctx -> nextconvoffs    = 0;
  conctx -> rcvwindownxt    = 0;
  conctx -> barelfs         = 0;		/* we haven't found any bare line-feeds yet */
  conctx -> h_datafile      = h_file;
  conctx -> transferstarted = oz_hw_tod_getnow ();
  storerecvstart (conctx);
}

/* This routine is called to start receiving data from the link */

static void storerecvstart (Conctx *conctx)

{
  OZ_IO_ip_tcpreceive ip_tcpreceive;
  uLong sts;

  if (conctx -> transferinprog < 0) {
    sendreply (conctx, "426 transfer aborted via abort command\r\n");
    transfercomplete (conctx);
    return;
  }

  /* Release all space in buffer from rcvwindowrem..nextwriteoffs */
  /* This will set the drivers's rcvwindowrem to nextwriteoffs    */

  memset (&ip_tcpreceive, 0, sizeof ip_tcpreceive);
  ip_tcpreceive.rawsize = conctx -> nextwriteoffs - conctx -> rcvwindowrem;
  ip_tcpreceive.rawrlen = &(conctx -> datarlen);
  conctx -> datarlen = 0;

  if (conctx -> rcvwindowrem  >= conctx -> datasize)     oz_crash ("oz_util_ftpd: rcvwindowrem %u, datasize %u", conctx -> rcvwindowrem, conctx -> datasize);
  if (conctx -> nextwriteoffs >= conctx -> datasize * 2) oz_crash ("oz_util_ftpd: nextwriteoffs %u, datasize %u", conctx -> nextwriteoffs, conctx -> datasize);
  if (conctx -> nextwriteoffs > conctx -> datasize) {
    memset (conctx -> databuff + conctx -> rcvwindowrem, 0x99, conctx -> datasize - conctx -> rcvwindowrem);
    memset (conctx -> databuff, 0x99, conctx -> nextwriteoffs - conctx -> datasize);
  } else {
    memset (conctx -> databuff + conctx -> rcvwindowrem, 0x99, conctx -> nextwriteoffs - conctx -> rcvwindowrem);
  }

  /* Now update our rcvwindowrem.  If it wraps off end, wrap everything. */
  /* This is the only place we wrap to keep rcvwindowrem .le. nextwriteoffs .le. nextconvoffs .le. rcvwindownxt */

  conctx -> rcvwindowrem = conctx -> nextwriteoffs;
  if (conctx -> rcvwindowrem >= conctx -> datasize) {
    conctx -> rcvwindowrem   -= conctx -> datasize;
    conctx -> nextwriteoffs  -= conctx -> datasize;
    conctx -> nextconvoffs   -= conctx -> datasize;
    conctx -> rcvwindownxt   -= conctx -> datasize;
  }

  /* Start reading.  This will tell us how much data is available at rcvwindownxt. */

  sts = oz_sys_io_start (OZ_PROCMODE_KNL, conctx -> h_datalink, NULL, 0, storerecvcomplete, conctx, 
                         OZ_IO_IP_TCPRECEIVE, sizeof ip_tcpreceive, &ip_tcpreceive);
  if (sts != OZ_STARTED) storerecvcomplete (conctx, sts, NULL);
}

/* This routine is called when data has been received from the link */

static void storerecvcomplete (void *conctxv, uLong status, OZ_Mchargs *mchargs)

{
  uByte *p, *q, *r, *s;
  Conctx *conctx;
  OZ_IO_fs_writerec fs_writerec;
  uLong sts, writeoffs, writesize;

  conctx = conctxv;

  /* If fatal error, abort retrieval */

  if ((status != OZ_SUCCESS) && (status != OZ_ENDOFFILE)) {
    sendreply (conctx, "451 error %u receiving data\r\n", status);
    transfercomplete (conctx);
    return;
  }

  /* Count total bytes read from network so far */

  conctx -> transfersofar += conctx -> datarlen;

  /* Maybe transfer was aborted with an ABOR command */

  if (conctx -> transferinprog < 0) {
    sendreply (conctx, "426 transfer aborted via abort command\r\n");
    transfercomplete (conctx);
    return;
  }

  /* Increment our copy of rcvwindownxt to include the received data */
  /* Do not wrap it to keep it always .ge. nextconvoffs              */

  conctx -> rcvwindownxt += conctx -> datarlen;

  /* If non-image mode, convert all CRLF's to LF's                  */
  /* Start conversion at nextconvoffs, stop at rcvwindownxt         */
  /* During conversion, shift data up so rcvwindownxt remains as is */
  /* Increment nextwriteoffs to point to beg of converted data      */
  /* Increment nextconvoffs to point to end of converted data       */

  if (conctx -> reptype != 'I') {
    uByte b;
    uLong i, j, s;

    s = conctx -> datasize;
    for (i = j = conctx -> rcvwindownxt; i > conctx -> nextconvoffs;) {
      b = conctx -> databuff[(--i)%s];
      if (b == '\n') {
        if ((i > conctx -> nextconvoffs) && (conctx -> databuff[(i-1)%s] == '\r')) -- i;
        else conctx -> barelfs ++;
      }
      conctx -> databuff[(--j)%s] = b;
    }
    while (i > conctx -> nextwriteoffs) {
      conctx -> databuff[(--j)%s] = conctx -> databuff[(--i)%s];
    }
    conctx -> nextwriteoffs = j;
    conctx -> nextconvoffs  = conctx -> rcvwindownxt;
    if (conctx -> databuff[(conctx->nextconvoffs-1)%s] == '\r') -- (conctx -> nextconvoffs);
  }

  /* No conversion, thus it is all converted (a priori) */

  else conctx -> nextconvoffs = conctx -> rcvwindownxt;

  /* Get how much there is to write and where it is.                */
  /* If there is nothing and we are at eof on receive, we're done.  */
  /* All the data from nextwriteoffs thru nextconvoffs is writable. */

  writesize = conctx -> nextconvoffs - conctx -> nextwriteoffs;
  writeoffs = conctx -> nextwriteoffs;
  if (writeoffs >= conctx -> datasize) writeoffs -= conctx -> datasize;

  if (writesize == 0) {
    if (status == OZ_ENDOFFILE) {
      sts = oz_sys_io (OZ_PROCMODE_KNL, conctx -> h_datafile, 0, OZ_IO_FS_CLOSE, 0, NULL);
      if (sts == OZ_SUCCESS) transfersuccessful (conctx);
      else {
        sendreply (conctx, "550 error %u closing file\r\n", sts);
        transfercomplete (conctx);
      }
    } else {
      storerecvstart (conctx);
    }
    return;
  }

  /* Can't write around the wrap, so stop at wrapping point */

  if (writeoffs + writesize > conctx -> datasize) writesize = conctx -> datasize - writeoffs;

  /* Increment amount written to disk */

  conctx -> transfertotal += writesize;

  /* Start writing data to the file and increment pointer to next stuff to write */

  memset (&fs_writerec, 0, sizeof fs_writerec);
  fs_writerec.size = writesize;
  fs_writerec.buff = conctx -> databuff + writeoffs;
  conctx -> diskiosofar   += writesize;
  conctx -> nextwriteoffs += writesize;
  sts = oz_sys_io_start (OZ_PROCMODE_KNL, conctx -> h_datafile, NULL, 0, storewritecomplete, conctx, 
                         OZ_IO_FS_WRITEREC, sizeof fs_writerec, &fs_writerec);
  if (sts != OZ_STARTED) storewritecomplete (conctx, sts, NULL);
}

/* This ast routine is called when the file write completes */

static void storewritecomplete (void *conctxv, uLong status, OZ_Mchargs *mchargs)

{
  Conctx *conctx;

  conctx = conctxv;

  /* Make sure writing completed successfully */

  if (status != OZ_SUCCESS) {
    sendreply (conctx, "550 error %u writing file\r\n", status);
    transfercomplete (conctx);
    return;
  }

  /* Transmit was ok, continue reading from network */

  storerecvstart (conctx);
}

/************************************************************************/
/*									*/
/*  SYST								*/
/*									*/
/************************************************************************/

static void c_system (Conctx *conctx, char *params)

{
  if (conctx -> reptype    == 0) conctx -> reptype    = 'A';
  if (conctx -> repsubtype == 0) conctx -> repsubtype = 'N';
  if (conctx -> filestruct == 0) conctx -> filestruct = 'F';
  if (conctx -> transmode  == 0) conctx -> transmode  = 'S';

  sendreply (conctx, "215 OZONE system ftp daemon, representation %c %c, file structure %c, transfer mode %c\r\n", 
                      conctx -> reptype, conctx -> repsubtype, conctx -> filestruct, conctx -> transmode);
}

/************************************************************************/
/*									*/
/*  TYPE A/I N/C/T							*/
/*									*/
/************************************************************************/

static void c_reptype (Conctx *conctx, char *params)

{
  switch (*params) {
    case 'A': {
      conctx -> reptype = *(params ++);
      while ((*params != 0) && (*params <= ' ')) params ++;
      if ((*params != 0) && (*params != 'N') && (*params != 'T') /** && (*params != 'C') **/) {
        sendreply (conctx, "504 invalid %c subtype %s\r\n", conctx -> reptype, params);
        return;
      }
      conctx -> repsubtype = *params;
      if (*params == 0) sendreply (conctx, "200 representation type set to %c\r\n", conctx -> reptype);
      else sendreply (conctx, "200 representation type set to %c, subtype %c\r\n", conctx -> reptype, conctx -> repsubtype);
      return;
    }
    case 'I': {
      conctx -> reptype = *(params ++);
      conctx -> repsubtype = 0;
      sendreply (conctx, "200 representation type set to I\r\n");
      return;
    }
  }
  sendreply (conctx, "504 invalid type %s\r\n", params);
}

/************************************************************************/
/*									*/
/*  USER <username>							*/
/*									*/
/************************************************************************/

static void c_username (Conctx *conctx, char *params)

{
  oz_sys_handle_release (OZ_PROCMODE_KNL, conctx -> h_user);		/* release handle to log user out */
  conctx -> loggedin = 0;						/* no one is logged in now */
  conctx -> h_user   = 0;
  strncpyz (conctx -> username, params, sizeof conctx -> username);	/* save the supplied username */
  sendreply (conctx, "330 send password next\r\n");			/* tell them to send password next */
}

/************************************************************************/
/*									*/
/*  Open data connection to ipaddr/portno specified with last PORT 	*/
/*  command or the clients ipaddr/portno, or wait on the passive 	*/
/*  connection for a connection.					*/
/*									*/
/*    Input:								*/
/*									*/
/*	conctx -> h_pasvlink = 0: do active connect to client		*/
/*	                    else: wait for inbound connect		*/
/*	conctx -> dataipaddr/portno = ipaddr/portno to connect to	*/
/*	transmit = 0 : will be doing receives on this connection	*/
/*	           1 : will be doing transmits on this connection	*/
/*									*/
/*    Output:								*/
/*									*/
/*	opendatacon = 0 : failed (reply message already sent)		*/
/*	              1 : successfully connected			*/
/*	conctx -> h_datalink = network link i/o channel			*/
/*	                       (h_pasvlink cleared)			*/
/*									*/
/************************************************************************/

static int opendatacon (Conctx *conctx, int transmit)

{
  char ipstr[OZ_IO_IP_ADDRSIZE*4], pnstr[OZ_IO_IP_PORTSIZE*4];
  OZ_IO_ip_tcpconnect ip_tcpconnect;
  OZ_IO_ip_tcpwindow ip_tcpwindow;
  uLong sts;

  closedatacon (conctx);						/* close any old connection */

  /* Allocate data buffer */

  conctx -> tcpsendsize = TCPSENDSIZE;					/* ?? query this from the link ?? */
  conctx -> datasize = SIZEFACTOR * TCPSENDSIZE;			/* allocate data buffer */
  conctx -> databuff = malloc (conctx -> datasize);

  /* If passive mode, wait for the inbound connection */

  if (conctx -> h_pasvlink != 0) {
    while ((sts = conctx -> pasvstatus) == OZ_PENDING) {		// wait for listen to complete
      oz_sys_event_wait (OZ_PROCMODE_KNL, conctx -> h_pasvwait, 0);
      oz_sys_event_set (OZ_PROCMODE_KNL, conctx -> h_pasvwait, 0, NULL);
    }
    if (sts != OZ_SUCCESS) {						// fail if error listening
      sendreply (conctx, "425 error %u listening for passive connect\r\n", sts);
      free (conctx -> databuff);
      conctx -> databuff = NULL;
      return (0);
    }
    conctx -> h_datalink = conctx -> h_pasvlink;			// use the link for data link
    conctx -> h_pasvlink = 0;						// clear passive mode flag, we're no longer listening
    oz_sys_handle_release (OZ_PROCMODE_KNL, conctx -> h_pasvwait);	// get rid of event flag
    conctx -> h_pasvwait = 0;
    if (!transmit) {
      memset (&ip_tcpwindow, 0, sizeof ip_tcpwindow);
      ip_tcpwindow.windowsize = conctx -> datasize;			// just receiving, read directly into the data buffer
      ip_tcpwindow.windowbuff = conctx -> databuff;
      ip_tcpwindow.window99s  = 1;					// (we will put 0x99's where it's ok for debugging)
      memset (conctx -> databuff, 0x99, conctx -> datasize);
      sts = oz_sys_io (OZ_PROCMODE_KNL, conctx -> h_datalink, 0, OZ_IO_IP_TCPWINDOW, sizeof ip_tcpwindow, &ip_tcpwindow);
      if (sts != OZ_SUCCESS) {
        sendreply (conctx, "425 error %u setting receive window\r\n", sts);
        free (conctx -> databuff);
        conctx -> databuff = NULL;
        return (0);
      }
    }
    return (1);
  }

  /* Active mode, create a socket to connect with */

  sts = oz_sys_io_assign (OZ_PROCMODE_KNL, &(conctx -> h_datalink), OZ_IO_IP_DEV, OZ_LOCKMODE_CW);
  if (sts != OZ_SUCCESS) {
    sendreply (conctx, "425 error %u assigning channel to %s\r\n", sts, OZ_IO_IP_DEV);
    conctx -> h_datalink = 0;
    free (conctx -> databuff);
    conctx -> databuff = NULL;
    return (0);
  }

  /* Connect back to client */

  memset (&ip_tcpconnect, 0, sizeof ip_tcpconnect);			/* set up connection parameter block */
  ip_tcpconnect.addrsize  = OZ_IO_IP_ADDRSIZE;
  ip_tcpconnect.portsize  = OZ_IO_IP_PORTSIZE;
  ip_tcpconnect.dstipaddr = conctx -> dataipaddr;
  ip_tcpconnect.dstportno = conctx -> dataportno;
  ip_tcpconnect.timeout   = 10000;
  ip_tcpconnect.srcportno = conctx -> dsrcportno;

  if (transmit) ip_tcpconnect.windowsize = 1;				/* just transmitting, get minimal receive buffer */
  else {
    ip_tcpconnect.windowsize = conctx -> datasize;			/* just receiving, read into the data buffer */
    ip_tcpconnect.windowbuff = conctx -> databuff;
    ip_tcpconnect.window99s  = 1;
    memset (conctx -> databuff, 0x99, conctx -> datasize);
  }

  sts = oz_sys_io (OZ_PROCMODE_KNL, conctx -> h_datalink, 0, OZ_IO_IP_TCPCONNECT, sizeof ip_tcpconnect, &ip_tcpconnect);
  if (sts == OZ_SUCCESS) return (1);					/* success, return success status */

  free (conctx -> databuff);						/* connect error, free data buffer */
  conctx -> databuff = NULL;
  sendreply (conctx, "425 error %u connecting to %s,%s\r\n", 		/* output error message on control link */
             sts, cvtipbin (conctx -> dataipaddr, ipstr), cvtpnbin (conctx -> dataportno, pnstr));
  oz_sys_handle_release (OZ_PROCMODE_KNL, conctx -> h_datalink);	/* close datalink channel */
  conctx -> h_datalink = 0;
  return (0);								/* return failure status */
}

/************************************************************************/
/*									*/
/*  The transfer is complete						*/
/*  The reply for the transfer command has already been sent		*/
/*									*/
/************************************************************************/

static void transfersuccessful (Conctx *conctx)

{
  OZ_Datebin transfertime;
  uLong rate;

  transfertime = oz_hw_tod_getnow () - conctx -> transferstarted;
  rate = (OZ_Datebin)(conctx -> transfersofar) * OZ_TIMER_RESOLUTION / 1024 / transfertime;
  sendreply (conctx, "226 transfer complete, %u/%u bytes in %#t (%u kbytes/sec)\r\n", 
	conctx -> transfersofar, conctx -> diskiosofar, transfertime, rate);
  transfercomplete (conctx);
}

static void transfercomplete (Conctx *conctx)

{
  /* Close handles related to data transfer */

  if (conctx -> transferinprog >= 0) oz_sys_io (OZ_PROCMODE_KNL, conctx -> h_datalink, 0, OZ_IO_IP_TCPCLOSE, 0, NULL);
  oz_sys_handle_release (OZ_PROCMODE_KNL, conctx -> h_datalink);
  oz_sys_handle_release (OZ_PROCMODE_KNL, conctx -> h_datafile);
  conctx -> h_datalink = 0;
  conctx -> h_datafile = 0;

  /* If abort command, send reply for the abort command now that the link is closed */

  if (conctx -> transferinprog < 0) sendreply (conctx, "226 transfer aborted, closing data link\r\n");
  conctx -> transferinprog = 0;

  /* Free off the data buffer */

  if (conctx -> databuff != NULL) {
    free (conctx -> databuff);
    conctx -> databuff = NULL;
  }

  /* If the user has entered the QUIT command, it is ok to quit now */

  if (conctx -> hasquit) terminate (conctx);
}

/************************************************************************/
/*									*/
/*  Close data connection						*/
/*									*/
/************************************************************************/

static void closedatacon (Conctx *conctx)

{
  oz_sys_handle_release (OZ_PROCMODE_KNL, conctx -> h_datalink);
  conctx -> h_datalink = 0;
}

/************************************************************************/
/*									*/
/*  Send reply message string						*/
/*									*/
/************************************************************************/

static void sendreply (Conctx *conctx, const char *format, ...)

{
  char replybuf[1024];
  OZ_IO_ip_tcptransmit ip_tcptransmit;
  uLong sts;
  va_list ap;

  va_start (ap, format);
  oz_sys_vsprintf (sizeof replybuf, replybuf, format, ap);
  va_end (ap);

  if (flag_verbose) oz_sys_io_fs_printf (oz_util_h_output, "%s: %s <%s", pn, conctx -> clientname, replybuf);

  memset (&ip_tcptransmit, 0, sizeof ip_tcptransmit);
  ip_tcptransmit.rawsize = strlen (replybuf);
  ip_tcptransmit.rawbuff = replybuf;

  sts = oz_sys_io (OZ_PROCMODE_KNL, conctx -> h_ctrllink, 0, OZ_IO_IP_TCPTRANSMIT, sizeof ip_tcptransmit, &ip_tcptransmit); 
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u transmitting reply to %s\n", pn, sts, conctx -> clientname);
    oz_sys_thread_exit (sts);
  }
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
/*  Convert port number binary to string				*/
/*									*/
/************************************************************************/

static char *cvtpnbin (uByte pnbin[OZ_IO_IP_PORTSIZE], char pnstr[OZ_IO_IP_PORTSIZE*4])

{
  int i;
  uLong pn;

  pn = 0;
  for (i = 0; i < OZ_IO_IP_PORTSIZE; i ++) {
    pn <<= 8;
    pn  |= pnbin[i];
  }
  oz_hw_itoa (pn, OZ_IO_IP_PORTSIZE*4, pnstr);
  return (pnstr);
}

static void pause (const char *prompt)

{
  char buf[16];
  OZ_IO_console_read console_read;

  static uLong sts = OZ_SUCCESS;

  memset (&console_read, 0, sizeof console_read);
  console_read.size = sizeof buf;
  console_read.buff = buf;
  console_read.pmtsize = strlen (prompt);
  console_read.pmtbuff = prompt;
  console_read.noecho  = 1;

  if (sts == OZ_SUCCESS) {
    sts = oz_sys_io (OZ_PROCMODE_KNL, oz_util_h_console, 0, OZ_IO_CONSOLE_READ, sizeof console_read, &console_read);
    if (sts != OZ_SUCCESS) oz_sys_io_fs_printf (oz_util_h_console, "\nsts %u, pause off", sts);
  }
  oz_sys_io_fs_printf (oz_util_h_console, "\n");
}
