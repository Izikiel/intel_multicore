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
/*  FTP library routines						*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_crtl_malloc.h"
#include "oz_knl_devio.h"
#include "oz_knl_status.h"
#include "oz_io_ip.h"
#include "oz_sys_condhand.h"
#include "oz_sys_dateconv.h"
#include "oz_sys_handle.h"
#include "oz_sys_handle_getinfo.h"
#include "oz_sys_io.h"
#include "oz_sys_io_fs.h"
#include "oz_sys_xprintf.h"
#include "oz_util_start.h"

#define SENDFACTOR 3	/* number of 'tcpsendsize' to send at a time */
#define SIZEFACTOR 9	/* number of 'tcpsendsize' to allocate for databuff */
#define TCPSENDSIZE 1460 /* ?? make this dynamic someday to be what the link actually is */
#define WINDOW99S 0	/* 1: to do rcvwindow buff checking (debug only - causes kernel crash if error) */

typedef struct Retrsendctx Retrsendctx;

struct OZ_Ftp_ctx { char defaultdir[OZ_FS_MAXFNLEN];		/* default directory for opening/creating files */

                    char reptype;				/* representation type */
                    char repsubtype;				/* representation sub-type */
                    char filestruct;				/* file structure */
                    char transmode;				/* transfer mode */
								/* 'S': stream (default) */
								/* 'B': block */
								/* 'C': compressed */

                    int transferinprog;				/* 0: not doing transfer; 1: doing transfer; -1: transfer aborting */
                    OZ_Datebin transferstarted;			/* date/time transfer was started */
                    OZ_Handle h_datalink;			/* data link i/o channel */
                    OZ_Handle h_datafile;			/* data file i/o channel */
                    Retrsendctx *freeretrsendctxs;		/* free retrieve send context blocks */
                    uLong tcpsendsize;				/* optimal tcp send size */
                    uLong writeinprog;				/* write in progress flag */
                    uLong readinprog;				/* read in progress flag */
                    uLong writeoffset;				/* write offset in databuff */
                    uLong readoffset;				/* read offset in databuff */
                    uLong readstatus;				/* read completion status */
                    uLong writestatus;				/* write completion status */
                    uLong transfersofar;			/* number of bytes transferred so far */
                    uLong transfertotal;			/* total number of bytes to transfer */
                    uLong barelfs;				/* number of bare LF's found */
                    uLong rcvwindowrem;				/* beginning of valid data in databuff */
                    uLong nextwriteoffs;			/* offset of next data to write to disk */
                    uLong nextconvoffs;				/* offset of next data to convert */
                    uLong rcvwindownxt;				/* end of valid data in databuff */
                    uLong datarlen;				/* length of data read/received */
                    uLong datasize;				/* size of databuff */
                    uByte *databuff;				/* pointer to data buffer of size datasize */
                    uByte dataipaddr[OZ_IO_IP_ADDRSIZE];	/* data port ip address */
                    uByte dataportno[OZ_IO_IP_PORTSIZE];	/* data port port number */
                    uByte dsrcportno[OZ_IO_IP_PORTSIZE];	/* data source port number */
                  };

struct Retrsendctx { Retrsendctx *next;
                     OZ_Ftp_ctx *ftpctx;
                     uLong size;
                   };

static int opendatacon (OZ_Ftp_ctx *ftpctx, int transmit);
static void transfersuccessful (OZ_Ftp_ctx *ftpctx);
static void transfercomplete (OZ_Ftp_ctx *ftpctx);
static void closedatacon (OZ_Ftp_ctx *ftpctx);
static void sendreply (OZ_Ftp_ctx *ftpctx, const char *format, ...);

/************************************************************************/
/*									*/
/*  Init ftp context block						*/
/*									*/
/************************************************************************/

OZ_Ftp_ctx *oz_lib_ftp_init (uByte remipaddr[OZ_IO_IP_ADDRSIZE], uByte remportno[OZ_IO_IP_PORTSIZE], 
                             uByte lclportno[OZ_IO_IP_PORTSIZE], const char *defaultdir)

{
  OZ_Ftp_ctx *ftpctx;

  ftpctx = malloc (sizeof *ftpctx);
  memset (ftpctx, 0, sizeof *ftpctx);

  /* Set up default data port to be the clients */

  memcpy (ftpctx -> dataipaddr, remipaddr, OZ_IO_IP_ADDRSIZE);
  memcpy (ftpctx -> dataportno, remportno, OZ_IO_IP_PORTSIZE);

  /* Our outgoing port number is the well known port - 1 */

  sts = OZ_IP_N2HW (lclportno) - 1;
  OZ_IP_H2NW (sts, ftpctx -> dsrcportno);

  /* Save default directory */

  strncpyz (ftpctx -> defaultdir, defaultdir, sizeof ftpctx -> defaultdir);

  return (ftpctx);
}

void oz_lib_ftp_term (OZ_Ftp_ctx *ftpctx)

{
  oz_sys_handle_release (OZ_PROCMODE_KNL, ftpctx -> h_datalink);
  oz_sys_handle_release (OZ_PROCMODE_KNL, ftpctx -> h_datafile);
  free (ftpctx);
}

/************************************************************************/
/*									*/
/*  Set various data transfer parameters				*/
/*									*/
/************************************************************************/

void oz_lib_ftp_set_defaultdir (OZ_Ftp_ctx *ftpctx, const char *defaultdir)

{
  strncpyz (ftpctx -> defaultdir, defaultdir, sizeof ftpctx -> defaultdir);
}

void oz_lib_ftp_set_transmode (OZ_Ftp_ctx *ftpctx, char transmode)

{
  ftpctx -> transmode = transmode;
}

void oz_lib_ftp_set_dataport (OZ_Ftp_ctx *ftpctx, uByte ipaddr[OZ_IO_IP_ADDRSIZE], uByte portno[OZ_IO_IP_PORTSIZE])

{
  memcpy (ftpctx -> dataipaddr, ipaddr, OZ_IO_IP_ADDRSIZE);
  memcpy (ftpctx -> dataportno, portno, OZ_IO_IP_PORTSIZE);
}

void oz_lib_ftp_set_filestruct (OZ_Ftp_ctx *ftpctx, char filestruct)

{
  ftpctx -> filestruct = filestruct;
}

void oz_lib_ftp_set_reptype (OZ_Ftp_ctx *ftpctx, char reptype, char repsubtype)

{
  ftpctx -> reptype    = reptype;
  ftpctx -> repsubtype = repsubtype;
}

/************************************************************************/
/*									*/
/*  Retrieve a file							*/
/*									*/
/************************************************************************/

static void retrieveloop (OZ_Ftp_ctx *ftpctx);
static void retrieveread (void *ftpctxv, uLong status, OZ_Mchargs *mchargs);
static void retrievereadproc (OZ_Ftp_ctx *ftpctx, uLong status);
static void retrievesent (void *paramv, uLong status, OZ_Mchargs *mchargs);

void oz_lib_ftp_retrieve (OZ_Ftp_ctx *ftpctx, 
                          const char *filename, 
                          void (*astentry) (void *param, const char *complmsg), 
                          void *param)

{
  char rnamebuff[OZ_FS_MAXFNLEN], unitname[OZ_DEVUNIT_NAMESIZE];
  OZ_Handle h_file;
  OZ_IO_fs_getinfo1 fs_getinfo1;
  OZ_IO_fs_open fs_open;
  uLong sts;

  ftpctx -> astentry = astentry;
  ftpctx -> astparam = astparam;

  /* Open the file */

  memset (&fs_open, 0, sizeof fs_open);
  fs_open.name      = filename;
  fs_open.lockmode  = OZ_LOCKMODE_PR;
  fs_open.rnamesize = sizeof rnamebuff;
  fs_open.rnamebuff = rnamebuff;
  sts = oz_sys_io_fs_open2 (sizeof fs_open, &fs_open, 0, ftpctx -> defaultdir, &h_file);
  if (sts != OZ_SUCCESS) {
    sendreply (ftpctx, "550 error %u opening file %s\r\n", sts, params);
    return;
  }

  /* Find out what device it is on */

  sts = oz_sys_iochan_getunitname (h_file, sizeof unitname, unitname);
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (1, sts);

  /* Open data connection */

  if (!opendatacon (ftpctx, 1)) {
    oz_sys_handle_release (OZ_PROCMODE_KNL, h_file);
    return;
  }

  /* Print a message saying it is being retrieved and how big it is */

  ftpctx -> transfertotal = 0;
  memset (&fs_getinfo1, 0, sizeof fs_getinfo1);
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_file, 0, OZ_IO_FS_GETINFO1, sizeof fs_getinfo1, &fs_getinfo1);
  if (sts != OZ_SUCCESS) sendreply (ftpctx, "150 retrieving from %s:%s (size unknown, error %u)\r\n", unitname, rnamebuff, sts);
  else {
    ftpctx -> transfertotal = (fs_getinfo1.eofblock - 1) * fs_getinfo1.blocksize + fs_getinfo1.eofbyte;
    sendreply (ftpctx, "150 retrieving from %s:%s (%u bytes)\r\n", unitname, rnamebuff, ftpctx -> transfertotal);
  }

  /* Start retrievin' */

  ftpctx -> transferinprog  = 1;
  ftpctx -> transfersofar   = 0;
  ftpctx -> readoffset      = 0;
  ftpctx -> writeoffset     = 0;
  ftpctx -> readinprog      = 0;
  ftpctx -> writeinprog     = 0;
  ftpctx -> readstatus      = OZ_PENDING;
  ftpctx -> writestatus     = OZ_PENDING;
  ftpctx -> h_datafile      = h_file;
  ftpctx -> transferstarted = oz_hw_timer_getnow ();
  retrieveloop (ftpctx);
}

/************************************************************************/
/*  This routine is the retrieve loop.  It keeps a disk read going and 	*/
/*  as many transmits going at a time as it can.			*/
/************************************************************************/

static void retrieveloop (OZ_Ftp_ctx *ftpctx)

{
  int startedsomething;
  OZ_IO_fs_readrec fs_readrec;
  OZ_IO_ip_tcptransmit ip_tcptransmit;
  Retrsendctx *retrsendctx;
  uLong contigfreesize, offs, size, sts, totalfreesize;

  startedsomething = 1;

loop:
  if (!startedsomething) return;
  startedsomething = 0;

  /* If transmit error and no I/O is going, we're all done */

  if (ftpctx -> writestatus != OZ_PENDING) {
    if ((ftpctx -> writeinprog == 0) && !(ftpctx -> readinprog)) transfercomplete (ftpctx);
    return;
  }

  /* If read hit eof (or error) and there is no data yet to be transmitted, we're all done */

  sts = ftpctx -> readstatus;
  if ((sts != OZ_PENDING) && (ftpctx -> writeoffset == ftpctx -> readoffset)) {
    if (sts != OZ_ENDOFFILE) transfercomplete (ftpctx);				/* if not eof, some error (message already output) */
    else transfersuccessful (ftpctx);						/* eof, successful completion */
    return;
  }

  /* If ABORT command received, don't start anything more */

  if (ftpctx -> transferinprog < 0) {
    if ((ftpctx -> writeinprog == 0) && !(ftpctx -> readinprog)) {		/* if reads or transmits are going, wait till they finish */
      sendreply (ftpctx, "426 transfer aborted via abort command\r\n");		/* nothing going, terminate transfer */
      transfercomplete (ftpctx);
    }
    return;
  }

  /* If a read is already going, don't start another */

  if (ftpctx -> readinprog) goto startsend;					/* if set, there is a read already going */
  if (sts != OZ_PENDING) goto startsend;					/* if not pending, we're all done reading */

  /* See how much space is available starting at readoffset up to writeoffset or the end of the buffer */

  if (ftpctx -> readoffset < ftpctx -> writeoffset + ftpctx -> writeinprog) {
    oz_crash ("oz_lib_ftpd retrieveread: readoffset %u, writeoffset %u, writeinprog %u", ftpctx -> readoffset, ftpctx -> writeoffset, ftpctx -> writeinprog);
  }

  if (ftpctx -> writeoffset == ftpctx -> readoffset) {				/* see if buffer completely empty */
    ftpctx -> readoffset  = 0;							/* if so, reset pointers to the beginning */
    ftpctx -> writeoffset = 0;
  }
  offs = ftpctx -> readoffset;							/* get beg of available space */
  totalfreesize = ftpctx -> writeoffset;					/* get end of available space */
  if (totalfreesize <= offs) totalfreesize += ftpctx -> datasize;		/* 'unwrap it' past beg of available space */
  totalfreesize -= offs;							/* subtract beg to get the total available size */
  if (totalfreesize == 0) goto startsend;					/* can't read if no room left */
  if (offs >= ftpctx -> datasize) offs -= ftpctx -> datasize;			/* set up block buffer address */
  if (offs >= ftpctx -> datasize) oz_crash ("oz_lib_ftpd retrieveread: offs %u", offs);
  contigfreesize = totalfreesize;						/* assume it is all contiguous */
  if (contigfreesize > ftpctx -> datasize - offs) contigfreesize = ftpctx -> datasize - offs; /* maybe it hails from Compton */

  /* If not image mode, make sure there is enough room to change a whole buffer of \n's to \r\n's */

  if ((ftpctx -> reptype != 'I') && (contigfreesize > totalfreesize / 2)) {
    contigfreesize = totalfreesize / 2;
    if (contigfreesize == 0) goto startsend;
  }

  /* Start reading from the disk file */

  memset (&fs_readrec, 0, sizeof fs_readrec);
  fs_readrec.size = contigfreesize;						/* set up size to read */
  fs_readrec.buff = ftpctx -> databuff + offs;					/* set up where to read into */
  fs_readrec.rlen = &(ftpctx -> datarlen);					/* tell it where to return length actually read */

  ftpctx -> datarlen   = 0;							/* haven't read anything */
  ftpctx -> readinprog = 1;							/* remember we have a read going */

  sts = oz_sys_io_start (OZ_PROCMODE_KNL, ftpctx -> h_datafile, NULL, 0, retrieveread, ftpctx, 
                         OZ_IO_FS_READREC, sizeof fs_readrec, &fs_readrec);
  startedsomething = 1;
  if (sts != OZ_STARTED) {
    retrievereadproc (ftpctx, sts);						/* done already, process it */
    goto loop;
  }

  /* Start a write going if there is something to do */

startsend:
  offs = ftpctx -> writeoffset + ftpctx -> writeinprog;				/* get offset to stuff we haven't started yet */
  if ((offs < ftpctx -> datasize) && (ftpctx -> readoffset > ftpctx -> datasize)) { /* see if valid stuff wraps around */
    size = ftpctx -> datasize - offs;						/* if so, see how much there is up to the end */
  } else {
    size = ftpctx -> readoffset - offs;						/* no wrap, get how much total is valid */
    if (ftpctx -> readstatus == OZ_PENDING) {					/* if read still has more to go ... */
      size /= ftpctx -> tcpsendsize;						/* round transmit down to a nice tcp size */
      size *= ftpctx -> tcpsendsize;
    }
    if (size == 0) goto loop;							/* if nothing to transmit, maybe read more from disk */
    if (offs >= ftpctx -> datasize) offs -= ftpctx -> datasize;			/* wrap the starting offset */
  }
  if (size > SENDFACTOR * ftpctx -> tcpsendsize) size = SENDFACTOR * ftpctx -> tcpsendsize; /* only this much per i/o request */
										/* ... so we get notified as each part gets done */

  memset (&ip_tcptransmit, 0, sizeof ip_tcptransmit);				/* start sending the data to remote end */
  ip_tcptransmit.rawsize = size;
  ip_tcptransmit.rawbuff = ftpctx -> databuff + offs;
  ftpctx -> writeinprog += size;						/* ok, this much more is going on now */
  retrsendctx = ftpctx -> freeretrsendctxs;					/* ast needs to know size of write and context block */
  if (retrsendctx != NULL) ftpctx -> freeretrsendctxs = retrsendctx -> next;
  else {
    retrsendctx = malloc (sizeof *retrsendctx);
    retrsendctx -> ftpctx = ftpctx;
  }
  retrsendctx -> size = size;
  sts = oz_sys_io_start (OZ_PROCMODE_KNL, ftpctx -> h_datalink, NULL, 0, retrievesent, retrsendctx, 
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

static void retrieveread (void *ftpctxv, uLong status, OZ_Mchargs *mchargs)

{
  retrievereadproc (ftpctxv, status);
  retrieveloop (ftpctxv);
}

static void retrievereadproc (OZ_Ftp_ctx *ftpctx, uLong status)

{
  uByte *b, *p, *q;
  uLong i, numlfs, offs;

  ftpctx -> readinprog = 0;							/* the read is no longer going */
  if ((status != OZ_SUCCESS) && (ftpctx -> readstatus == OZ_PENDING)) {		/* save final read completion status */
    if (status != OZ_ENDOFFILE) sendreply (ftpctx, "550 error %u reading file\r\n", status);
    ftpctx -> readstatus = status;
  }
  ftpctx -> transfersofar += ftpctx -> datarlen;				/* increment amount transferred so far */

  /* If not image mode, translate all LF's to CRLF's */

  if (ftpctx -> reptype != 'I') {
    offs = ftpctx -> readoffset;
    if (offs >= ftpctx -> datasize) offs -= ftpctx -> datasize;
    numlfs = 0;									/* haven't found any LF's yet */
    b = ftpctx -> databuff;							/* point to beginning of buffer */
    p = b + offs;								/* point to beginning of data */
    for (i = ftpctx -> datarlen; i > 0; -- i) if (*(p ++) == '\n') numlfs ++;	/* count the LF's in the data */
    ftpctx -> datarlen += numlfs;						/* increase length of data read to include CR's */
    q = p + numlfs;								/* point to just past where last byte will go */
    if (q > b + ftpctx -> datasize) q -= ftpctx -> datasize;			/* wrap the pointer */
    while (q != p) {								/* repeat as long as there are CR's to insert */
      if (q == b) q += ftpctx -> datasize;					/* maybe wrap pointer */
      if ((*(-- q) = *(-- p)) == '\n') {					/* copy a character at end of buffer */
        if (q == b) q += ftpctx -> datasize;					/* copied an LF, maybe wrap pointer */
        *(-- q) = '\r';								/* ... then insert an CR before the LF */
      }
    }
  }

  /* Increment offset to include the bytes just read from the file (plus any inserted CR's). */
  /* Do not wrap it, it must always be >= writeoffs.                                         */

  ftpctx -> readoffset += ftpctx -> datarlen;
}

/************************************************************************/
/* This ast routine is called when the transmit completes       	*/
/************************************************************************/

static void retrievesent (void *paramv, uLong status, OZ_Mchargs *mchargs)

{
  OZ_Ftp_ctx *ftpctx;
  Retrsendctx *retrsendctx;
  uLong size, sts;

  retrsendctx = paramv;								/* point to retrieve send context block */
  ftpctx = retrsendctx -> ftpctx;						/* get connection context block pointer */
  if ((status != OZ_SUCCESS) && (ftpctx -> writestatus == OZ_PENDING)) {
    if (status != OZ_ENDOFFILE) sendreply (ftpctx, "451 error %u sending file\r\n", status);
    ftpctx -> writestatus = status;						/* save failure status */
  }

  /* Decrement write-in-progress counter by the size we just transmitted */

  size = retrsendctx -> size;							/* get the size that was sent */
  retrsendctx -> next = ftpctx -> freeretrsendctxs;
  ftpctx -> freeretrsendctxs = retrsendctx;
  ftpctx -> writeinprog -= size;						/* that much less is being transmitted */

  /* Increment pointer beyond what was transmitted */

  ftpctx -> writeoffset += size;
  if (ftpctx -> writeoffset >= ftpctx -> datasize) {
    ftpctx -> readoffset  -= ftpctx -> datasize;
    ftpctx -> writeoffset -= ftpctx -> datasize;
  }

  /* Maybe more can be read from disk now */

  retrieveloop (ftpctx);
}

/************************************************************************/
/*									*/
/*  Store a file							*/
/*									*/
/************************************************************************/

static void storerecvstart (OZ_Ftp_ctx *ftpctx);
static void storerecvcomplete (void *ftpctxv, uLong status, OZ_Mchargs *mchargs);
static void storewritecomplete (void *ftpctxv, uLong status, OZ_Mchargs *mchargs);

void oz_lib_ftp_store (OZ_Ftp_ctx *ftpctx, 
                       const char *filename, 
                       void (*astentry) (void *param, const char *complmsg), 
                       void *astparam)

{
  char rnamebuff[OZ_FS_MAXFNLEN], unitname[OZ_DEVUNIT_NAMESIZE];
  OZ_Handle h_file;
  OZ_IO_fs_create fs_create;
  uLong sts;

  ftpctx -> astentry = astentry;
  ftpctx -> astparam = astparam;

  /* Create the file */

  memset (&fs_create, 0, sizeof fs_create);
  fs_create.name      = filename;
  fs_create.lockmode  = OZ_LOCKMODE_PW;
  fs_create.rnamesize = sizeof rnamebuff;
  fs_create.rnamebuff = rnamebuff;
  sts = oz_sys_io_fs_create2 (sizeof fs_create, &fs_create, 0, ftpctx -> defaultdir, &h_file);
  if (sts != OZ_SUCCESS) {
    sendreply (ftpctx, "550 error %u creating file %s\r\n", sts, params);
    return;
  }

  /* Find out what device it is on */

  sts = oz_sys_iochan_getunitname (h_file, sizeof unitname, unitname);
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (1, sts);

  /* Open data connection */

  if (!opendatacon (ftpctx, 0)) {
    oz_sys_handle_release (OZ_PROCMODE_KNL, h_file);
    return;
  }

  /* Print a message saying it is being stored and how big it is */

  ftpctx -> transfertotal = 0;
  sendreply (ftpctx, "150 storing to %s:%s\r\n", unitname, rnamebuff);

  /* Start storin' */

  ftpctx -> transferinprog  = 1;		/* we have a transfer going now */
  ftpctx -> transfersofar   = 0;		/* haven't recieved any data yet */
  ftpctx -> transfertotal   = 0;		/* haven't written any to disk yet */
  ftpctx -> rcvwindowrem    = 0;		/* reset buffer offsets */
  ftpctx -> nextwriteoffs   = 0;
  ftpctx -> nextconvoffs    = 0;
  ftpctx -> rcvwindownxt    = 0;
  ftpctx -> barelfs         = 0;		/* we haven't found any bare line-feeds yet */
  ftpctx -> h_datafile      = h_file;
  ftpctx -> transferstarted = oz_hw_timer_getnow ();
  storerecvstart (ftpctx);
}

/* This routine is called to start receiving data from the link */

static void storerecvstart (OZ_Ftp_ctx *ftpctx)

{
  OZ_IO_ip_tcpreceive ip_tcpreceive;
  uLong sts;

  if (ftpctx -> transferinprog < 0) {
    sendreply (ftpctx, "426 transfer aborted via abort command\r\n");
    transfercomplete (ftpctx);
    return;
  }

  /* Release all space in buffer from rcvwindowrem..nextwriteoffs */
  /* This will set the drivers's rcvwindowrem to nextwriteoffs    */

  memset (&ip_tcpreceive, 0, sizeof ip_tcpreceive);
  ip_tcpreceive.rawsize = ftpctx -> nextwriteoffs - ftpctx -> rcvwindowrem;
  ip_tcpreceive.rawrlen = &(ftpctx -> datarlen);
  ftpctx -> datarlen = 0;

  if (ftpctx -> rcvwindowrem  >= ftpctx -> datasize)     oz_crash ("oz_lib_ftpd: rcvwindowrem %u, datasize %u", ftpctx -> rcvwindowrem, ftpctx -> datasize);
  if (ftpctx -> nextwriteoffs >= ftpctx -> datasize * 2) oz_crash ("oz_lib_ftpd: nextwriteoffs %u, datasize %u", ftpctx -> nextwriteoffs, ftpctx -> datasize);
#if WINDOW99S
  if (ftpctx -> nextwriteoffs > ftpctx -> datasize) {
    memset (ftpctx -> databuff + ftpctx -> rcvwindowrem, 0x99, ftpctx -> datasize - ftpctx -> rcvwindowrem);
    memset (ftpctx -> databuff, 0x99, ftpctx -> nextwriteoffs - ftpctx -> datasize);
  } else {
    memset (ftpctx -> databuff + ftpctx -> rcvwindowrem, 0x99, ftpctx -> nextwriteoffs - ftpctx -> rcvwindowrem);
  }
#endif

  /* Now update our rcvwindowrem.  If it wraps off end, wrap everything. */
  /* This is the only place we wrap to keep rcvwindowrem .le. nextwriteoffs .le. nextconvoffs .le. rcvwindownxt */

  ftpctx -> rcvwindowrem = ftpctx -> nextwriteoffs;
  if (ftpctx -> rcvwindowrem >= ftpctx -> datasize) {
    ftpctx -> rcvwindowrem   -= ftpctx -> datasize;
    ftpctx -> nextwriteoffs  -= ftpctx -> datasize;
    ftpctx -> nextconvoffs   -= ftpctx -> datasize;
    ftpctx -> rcvwindownxt   -= ftpctx -> datasize;
  }

  /* Start reading.  This will tell us how much data is available at rcvwindownxt. */

  sts = oz_sys_io_start (OZ_PROCMODE_KNL, ftpctx -> h_datalink, NULL, 0, storerecvcomplete, ftpctx, 
                         OZ_IO_IP_TCPRECEIVE, sizeof ip_tcpreceive, &ip_tcpreceive);
  if (sts != OZ_STARTED) storerecvcomplete (ftpctx, sts, NULL);
}

/* This routine is called when data has been received from the link */

static void storerecvcomplete (void *ftpctxv, uLong status, OZ_Mchargs *mchargs)

{
  uByte *p, *q, *r, *s;
  OZ_Ftp_ctx *ftpctx;
  OZ_IO_fs_writerec fs_writerec;
  uLong sts, writeoffs, writesize;

  ftpctx = ftpctxv;

  /* If fatal error, abort retrieval */

  if ((status != OZ_SUCCESS) && (status != OZ_ENDOFFILE)) {
    sendreply (ftpctx, "451 error %u receiving data\r\n", status);
    transfercomplete (ftpctx);
    return;
  }

  /* Count total bytes read from network so far */

  ftpctx -> transfersofar += ftpctx -> datarlen;

  /* Maybe transfer was aborted with an ABOR command */

  if (ftpctx -> transferinprog < 0) {
    sendreply (ftpctx, "426 transfer aborted via abort command\r\n");
    transfercomplete (ftpctx);
    return;
  }

  /* Increment our copy of rcvwindownxt to include the received data */
  /* Do not wrap it to keep it always .ge. nextconvoffs              */

  ftpctx -> rcvwindownxt += ftpctx -> datarlen;

  /* If non-image mode, convert all CRLF's to LF's                  */
  /* Start conversion at nextconvoffs, stop at rcvwindownxt         */
  /* During conversion, shift data up so rcvwindownxt remains as is */
  /* Increment nextwriteoffs to point to beg of converted data      */
  /* Increment nextconvoffs to point to end of converted data       */

  if (ftpctx -> reptype != 'I') {
    uByte b;
    uLong i, j, s;

    s = ftpctx -> datasize;
    for (i = j = ftpctx -> rcvwindownxt; i > ftpctx -> nextconvoffs;) {
      b = ftpctx -> databuff[(--i)%s];
      if (b == '\n') {
        if ((i > ftpctx -> nextconvoffs) && (ftpctx -> databuff[(i-1)%s] == '\r')) -- i;
        else ftpctx -> barelfs ++;
      }
      ftpctx -> databuff[(--j)%s] = b;
    }
    while (i > ftpctx -> nextwriteoffs) {
      ftpctx -> databuff[(--j)%s] = ftpctx -> databuff[(--i)%s];
    }
    ftpctx -> nextwriteoffs = j;
    ftpctx -> nextconvoffs  = ftpctx -> rcvwindownxt;
    if (ftpctx -> databuff[(ftpctx->nextconvoffs-1)%s] == '\r') -- (ftpctx -> nextconvoffs);
  }

  /* No conversion, thus it is all converted (a priori) */

  else ftpctx -> nextconvoffs = ftpctx -> rcvwindownxt;

  /* Get how much there is to write and where it is.                */
  /* If there is nothing and we are at eof on receive, we're done.  */
  /* All the data from nextwriteoffs thru nextconvoffs is writable. */

  writesize = ftpctx -> nextconvoffs - ftpctx -> nextwriteoffs;
  writeoffs = ftpctx -> nextwriteoffs;
  if (writeoffs >= ftpctx -> datasize) writeoffs -= ftpctx -> datasize;

  if (writesize == 0) {
    if (status == OZ_ENDOFFILE) {
      sts = oz_sys_io (OZ_PROCMODE_KNL, ftpctx -> h_datafile, 0, OZ_IO_FS_CLOSE, 0, NULL);
      if (sts == OZ_SUCCESS) transfersuccessful (ftpctx);
      else {
        sendreply (ftpctx, "550 error %u closing file\r\n", sts);
        transfercomplete (ftpctx);
      }
    } else {
      storerecvstart (ftpctx);
    }
    return;
  }

  /* Can't write around the wrap, so stop at wrapping point */

  if (writeoffs + writesize > ftpctx -> datasize) writesize = ftpctx -> datasize - writeoffs;

  /* Increment amount written to disk */

  ftpctx -> transfertotal += writesize;

  /* Start writing data to the file and increment pointer to next stuff to write */

  memset (&fs_writerec, 0, sizeof fs_writerec);
  fs_writerec.size = writesize;
  fs_writerec.buff = ftpctx -> databuff + writeoffs;
  ftpctx -> nextwriteoffs += writesize;
  sts = oz_sys_io_start (OZ_PROCMODE_KNL, ftpctx -> h_datafile, NULL, 0, storewritecomplete, ftpctx, 
                         OZ_IO_FS_WRITEREC, sizeof fs_writerec, &fs_writerec);
  if (sts != OZ_STARTED) storewritecomplete (ftpctx, sts, NULL);
}

/* This ast routine is called when the file write completes */

static void storewritecomplete (void *ftpctxv, uLong status, OZ_Mchargs *mchargs)

{
  OZ_Ftp_ctx *ftpctx;

  ftpctx = ftpctxv;

  /* Make sure writing completed successfully */

  if (status != OZ_SUCCESS) {
    sendreply (ftpctx, "550 error %u writing file\r\n", status);
    transfercomplete (ftpctx);
    return;
  }

  /* Transmit was ok, continue reading from network */

  storerecvstart (ftpctx);
}

/************************************************************************/
/*									*/
/*  Open data connection to ipaddr/portno specified with last PORT 	*/
/*  command or the clients ipaddr/portno				*/
/*									*/
/*    Input:								*/
/*									*/
/*	ftpctx -> dataipaddr/portno = ipaddr/portno to connect to	*/
/*	transmit = 0 : will be doing receives on this connection	*/
/*	           1 : will be doing transmits on this connection	*/
/*									*/
/*    Output:								*/
/*									*/
/*	opendatacon = 0 : failed (reply message already sent)		*/
/*	              1 : successfully connected			*/
/*	ftpctx -> h_datalink = network link i/o channel			*/
/*									*/
/************************************************************************/

static int opendatacon (OZ_Ftp_ctx *ftpctx, int transmit)

{
  char ipstr[OZ_IO_IP_ADDRSIZE*4], pnstr[OZ_IO_IP_PORTSIZE*4];
  OZ_IO_ip_tcpconnect ip_tcpconnect;
  uLong sts;

  closedatacon (ftpctx);						/* close any old connection */
  ftpctx -> databuff = NULL;						/* don't have any data buffer */

  sts = oz_sys_io_assign (OZ_PROCMODE_KNL, &(ftpctx -> h_datalink), OZ_IO_IP_DEV, OZ_LOCKMODE_CW);
  if (sts != OZ_SUCCESS) {
    sendreply (ftpctx, "425 error %u assigning channel to %s\r\n", sts, OZ_IO_IP_DEV);
    ftpctx -> h_datalink = 0;
    return (0);
  }

  ftpctx -> tcpsendsize = TCPSENDSIZE;					/* ?? query this from the link ?? */
  ftpctx -> datasize = SIZEFACTOR * TCPSENDSIZE;			/* allocate data buffer */
  ftpctx -> databuff = malloc (ftpctx -> datasize);

  memset (&ip_tcpconnect, 0, sizeof ip_tcpconnect);			/* set up connection parameter block */
  ip_tcpconnect.addrsize  = OZ_IO_IP_ADDRSIZE;
  ip_tcpconnect.portsize  = OZ_IO_IP_PORTSIZE;
  ip_tcpconnect.dstipaddr = ftpctx -> dataipaddr;
  ip_tcpconnect.dstportno = ftpctx -> dataportno;
  ip_tcpconnect.timeout   = 10000;
  ip_tcpconnect.srcportno = ftpctx -> dsrcportno;

  if (transmit) ip_tcpconnect.windowsize = 1;				/* just transmitting, get minimal receive buffer */
  else {
    ip_tcpconnect.windowsize = ftpctx -> datasize;			/* just receiving, read into the data buffer */
    ip_tcpconnect.windowbuff = ftpctx -> databuff;
#if WINDOW99S
    ip_tcpconnect.window99s  = 1;
    memset (ftpctx -> databuff, 0x99, ftpctx -> datasize);
#endif
  }

  sts = oz_sys_io (OZ_PROCMODE_KNL, ftpctx -> h_datalink, 0, OZ_IO_IP_TCPCONNECT, sizeof ip_tcpconnect, &ip_tcpconnect);
  if (sts == OZ_SUCCESS) return (1);					/* success, return success status */

  free (ftpctx -> databuff);						/* connect error, free data buffer */
  ftpctx -> databuff = NULL;
  sendreply (ftpctx, "425 error %u connecting to %s,%s\r\n", 		/* output error message on control link */
             sts, cvtipbin (ftpctx -> dataipaddr, ipstr), cvtpnbin (ftpctx -> dataportno, pnstr));
  oz_sys_handle_release (OZ_PROCMODE_KNL, ftpctx -> h_datalink);	/* close datalink channel */
  ftpctx -> h_datalink = 0;
  return (0);								/* return failure status */
}

/************************************************************************/
/*									*/
/*  The transfer is complete						*/
/*  The reply for the transfer command has already been sent		*/
/*									*/
/************************************************************************/

static void transfersuccessful (OZ_Ftp_ctx *ftpctx)

{
  OZ_Datebin transfertime;
  uLong rate;

  transfertime = oz_hw_timer_getnow () - ftpctx -> transferstarted;
  rate = (OZ_Datebin)(ftpctx -> transfersofar) * OZ_TIMER_RESOLUTION / 1024 / transfertime;
  sendreply (ftpctx, "226 transfer complete, %u bytes in %#t (%u kbytes/sec)\r\n", ftpctx -> transfersofar, transfertime, rate);

  transfercomplete (ftpctx);
}

static void transfercomplete (OZ_Ftp_ctx *ftpctx)

{
  /* Close handles related to data transfer */

  if (ftpctx -> transferinprog >= 0) oz_sys_io (OZ_PROCMODE_KNL, ftpctx -> h_datalink, 0, OZ_IO_IP_TCPCLOSE, 0, NULL);
  oz_sys_handle_release (OZ_PROCMODE_KNL, ftpctx -> h_datalink);
  oz_sys_handle_release (OZ_PROCMODE_KNL, ftpctx -> h_datafile);
  ftpctx -> h_datalink = 0;
  ftpctx -> h_datafile = 0;

  /* If abort command, send reply for the abort command now that the link is closed */

  if (ftpctx -> transferinprog < 0) sendreply (ftpctx, "226 transfer aborted, closing data link\r\n");
  ftpctx -> transferinprog = 0;

  /* Free off the data buffer */

  if (ftpctx -> databuff != NULL) {
    free (ftpctx -> databuff);
    ftpctx -> databuff = NULL;
  }

  /* If the user has entered the QUIT command, it is ok to quit now */

  if (ftpctx -> hasquit) terminate (ftpctx);
}

/************************************************************************/
/*									*/
/*  Close data connection						*/
/*									*/
/************************************************************************/

static void closedatacon (OZ_Ftp_ctx *ftpctx)

{
  oz_sys_handle_release (OZ_PROCMODE_KNL, ftpctx -> h_datalink);
  ftpctx -> h_datalink = 0;
}

/************************************************************************/
/*									*/
/*  Send reply message string						*/
/*									*/
/************************************************************************/

static void sendreply (OZ_Ftp_ctx *ftpctx, const char *format, ...)

{
  char replybuf[1024];
  va_list ap;

  va_start (ap, format);
  oz_sys_vsprintf (sizeof replybuf, replybuf, format, ap);
  va_end (ap);

  (*(ftpctx -> astentry)) (ftpctx -> astparam, replybuf);
}
