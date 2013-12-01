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
/*  This utility writes log files.  It reads input from standard input	*/
/*  (which is usually a pipe) and writes records to the output file.	*/
/*  It creates a new version of the output file every day so they can 	*/
/*  easily be purged.							*/
/*									*/
/*	logwrite [-echo] [-kernel <log_symbol>] [-timestamp] <outputfile>
/*									*/
/*  The current date is appended directly to the supplied <outputfile>	*/
/*  thus you must supply any desired punctuation.			*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_crtl_malloc.h"
#include "oz_io_fs.h"
#include "oz_knl_hw.h"
#include "oz_knl_image.h"
#include "oz_knl_log.h"
#include "oz_knl_process.h"
#include "oz_knl_sdata.h"
#include "oz_knl_status.h"
#include "oz_sys_dateconv.h"
#include "oz_sys_io.h"
#include "oz_sys_io_fs.h"
#include "oz_sys_io_fs_printf.h"
#include "oz_sys_xprintf.h"
#include "oz_util_start.h"

#include <stdarg.h>
#include <stdlib.h>

static char *pn = "logwrite";

static char *kernel_buff, *kernel_sym, *logfile;
static char outfile[256], outname[256];
static const char *kernel_sourcefile;
static int flag_echo, flag_timestamp, kernel_sourceline;
static Long kernel_lostlines;
static OZ_Datebin kernel_datetime;
static OZ_Handle h_out;
static OZ_IO_fs_create fs_create;
static OZ_IO_fs_open fs_open;
static OZ_IO_fs_readrec fs_readrec;
static OZ_IO_fs_writerec fs_writerec;
static OZ_Log *kernel_log;
static uLong kernel_size, version_wehave;

static uLong writeline (OZ_Datebin whenutc, const char *file, int line, uLong size, char *buff);
static uLong find_kernel_sym (OZ_Procmode cprocmode, void *dummy);
static uLong read_kernel_log (OZ_Procmode cprocmode, void *dummy);

static void barfcheck (uLong sts, uLong rlen, char *buff);
static uLong barfcheck_knl (OZ_Procmode cprocmode, void *dummy);

uLong oz_util_main (int argc, char *argv[])

{
  char *buff, *p;
  int i;
  uLong rlen, size, sts;

  if (argc > 0) pn = argv[0];

  flag_echo      = 0;
  flag_timestamp = 0;
  kernel_sym     = NULL;
  logfile        = NULL;

  for (i = 1; i < argc; i ++) {
    if (argv[i][0] == '-') {
      if (strcasecmp (argv[i], "-echo") == 0) {
        flag_echo = 1;
        continue;
      }
      if (strcasecmp (argv[i], "-kernel") == 0) {
        if (++ i >= argc) goto usage;
        kernel_sym = argv[i];
        continue;
      }
      if (strcasecmp (argv[i], "-timestamp") == 0) {
        flag_timestamp = 1;
        continue;
      }
      goto usage;
    }
    if (logfile != NULL) goto usage;
    logfile = argv[i];
  }

  /* Make sure they specified a logfile */

  if (logfile == NULL) goto usage;

  /* If -kernel, look up the symbol and get 'log' object pointer */

  kernel_log = NULL;
  if (kernel_sym != NULL) {
    sts = oz_sys_callknl (find_kernel_sym, NULL);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u accessing kernel log %s\n", pn, sts, kernel_sym);
      return (sts);
    }
  }

  /* Set up I/O parameter blocks */

  version_wehave = 0;
  h_out = 0;

  memset (&fs_create, 0, sizeof fs_create);
  fs_create.name      = outname;
  fs_create.lockmode  = OZ_LOCKMODE_CW;
  fs_create.rnamesize = sizeof outfile;
  fs_create.rnamebuff = outfile;

  memset (&fs_open, 0, sizeof fs_open);
  fs_open.name      = outname;
  fs_open.lockmode  = OZ_LOCKMODE_CW;
  fs_open.rnamesize = sizeof outfile;
  fs_open.rnamebuff = outfile;

  memset (&fs_readrec, 0, sizeof fs_readrec);
  fs_readrec.rlen    = &rlen;
  fs_readrec.trmsize = 1;
  fs_readrec.trmbuff = "\n";

  memset (&fs_writerec, 0, sizeof fs_writerec);
  fs_writerec.trmsize = 1;
  fs_writerec.trmbuff = "\n";
  fs_writerec.append  = 1;

  /* Malloc a buffer and say kernel buffer is empty */

  size = 256;
  buff = malloc (size);
  kernel_size = 0;

  while (1) {

    /* Read a whole record from input up to a newline */

    if (kernel_log == NULL) {
      fs_readrec.size = size;
      fs_readrec.buff = buff;
      rlen = 0;
      sts  = oz_sys_io (OZ_PROCMODE_KNL, oz_util_h_input, 0, OZ_IO_FS_READREC, sizeof fs_readrec, &fs_readrec);
      barfcheck (sts, rlen, fs_readrec.buff);
      while (sts == OZ_NOTERMINATOR) {			// buffer filled but no newline
        size += 256;					// malloc another 256 bytes
        buff  = realloc (buff, size);
        fs_readrec.size = 256;				// read the next part into new bytes
        fs_readrec.buff = buff + rlen;
        rlen  = 0;
        sts   = oz_sys_io (OZ_PROCMODE_KNL, oz_util_h_input, 0, OZ_IO_FS_READREC, sizeof fs_readrec, &fs_readrec);
        barfcheck (sts, rlen, fs_readrec.buff);
        rlen += ((char *)fs_readrec.buff) - buff;	// get total length including old stuff
      }
      if (sts != OZ_SUCCESS) break;			// stop if some read error
      sts = writeline (oz_hw_tod_getnow (), NULL, 0, rlen, buff);
      if (sts != OZ_SUCCESS) return (sts);
    }

    /* Read a record from kernel log buffer and output a line at a time */

    else {
      memmove (buff, kernel_buff, kernel_size);		// shift partial line to beginning of buffer
      kernel_buff = buff + kernel_size;			// get address of what's left of buffer
      kernel_size = size - kernel_size;			// get size of what's left of buffer
      while ((sts = oz_sys_callknl (read_kernel_log, NULL)) == OZ_BUFFEROVF) { // get null-terminated string
        size += 256;					// it would overflow, expand buffer
        buff  = realloc (buff, size);
        kernel_size += 256;				// get new size of leftover space
        kernel_buff  = buff + size - kernel_size;	// get new address of leftover space
      }
      if (sts != OZ_SUCCESS) break;			// stop if other error reading
      kernel_size = strlen (buff);			// get total size of string
      kernel_buff = buff;				// point to total string
      while (kernel_size > 0) {				// repeat as long as there's something there
        p = strchr (kernel_buff, '\n');			// check for eol char
        if ((p == NULL) || (p >= kernel_buff + kernel_size)) break; // stop if not there
        sts = writeline (kernel_datetime, kernel_sourcefile, kernel_sourceline, p - kernel_buff, kernel_buff);
        if (sts != OZ_SUCCESS) return (sts);
        p ++;						// increment over the newline char
        kernel_size -= p - kernel_buff;			// that much less to do
        kernel_buff  = p;
      }
    }
  }

  if (sts == OZ_ENDOFFILE) sts = OZ_SUCCESS;
  else oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u reading input\n", pn, sts);
  return (sts);

usage:
  oz_sys_io_fs_printf (oz_util_h_error, "usage: %s [-echo] [-kernel <log_symbol>] [-timestamp] <logfile>\n", pn);
  return (OZ_MISSINGPARAM);
}

/************************************************************************/
/*									*/
/*  Write single line to log file					*/
/*									*/
/*    Input:								*/
/*									*/
/*	whenutc = when the record was written to log file		*/
/*	file = source file name (or NULL for none)			*/
/*	line = source line number (or 0 for none)			*/
/*	size = size of line (excluding \n and null)			*/
/*	buff = line data (excluding \n and null)			*/
/*									*/
/*    Output:								*/
/*									*/
/*	writeline = write status					*/
/*									*/
/************************************************************************/

static uLong writeline (OZ_Datebin whenutc, const char *file, int line, uLong size, char *buff)

{
  OZ_Datebin whenlcl;
  uLong datelongs[OZ_DATELONG_ELEMENTS], sts, version_needed, yyyymmdd;

  /* Get current date/time and the corresponding version number */

  whenlcl = oz_sys_datebin_tzconv (whenutc, OZ_DATEBIN_TZCONV_UTC2LCL, 0);
  oz_sys_datebin_decode (whenlcl, datelongs);
  yyyymmdd = oz_sys_daynumber_decode (datelongs[OZ_DATELONG_DAYNUMBER]);
  version_needed = ((yyyymmdd >> 16) * 10000) + (((yyyymmdd >> 8) & 0xFF) * 100) + (yyyymmdd & 0xFF);

  /* If it's a different version, close old file and create new one */

  if (version_wehave != version_needed) {
    if (h_out != 0) oz_sys_handle_release (OZ_PROCMODE_KNL, h_out);
    oz_sys_sprintf (sizeof outname, outname, "%s%u", logfile, version_needed);
    sts = oz_sys_io_fs_create (sizeof fs_create, &fs_create, 0, &h_out);
    if (sts == OZ_FILEALREADYEXISTS) sts = oz_sys_io_fs_open (sizeof fs_open, &fs_open, 0, &h_out);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u opening/creating %s\n", pn, sts, outname);
      return (sts);
    }
    version_wehave = version_needed;
  }

  /* Write record to file */

  if (flag_timestamp) {
    sts = oz_sys_io_fs_printf (h_out, "[%t]	%*.*s\n", whenutc, size, size, buff);
  } else {
    fs_writerec.size = size;
    fs_writerec.buff = buff;
    sts = oz_sys_io (OZ_PROCMODE_KNL, h_out, 0, OZ_IO_FS_WRITEREC, sizeof fs_writerec, &fs_writerec);
  }
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u writing %s\n", pn, sts, outfile);
    return (sts);
  }

  /* Maybe echo to output */

  if (flag_echo) {
    if (flag_timestamp) sts = oz_sys_io_fs_printf (oz_util_h_output, "[%t]	%*.*s\n", whenutc, size, size, buff);
    else sts = oz_sys_io (OZ_PROCMODE_KNL, oz_util_h_output, 0, OZ_IO_FS_WRITEREC, sizeof fs_writerec, &fs_writerec);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u writing output\n", pn, sts);
      return (sts);
    }
  }

  return (OZ_SUCCESS);
}

static uLong find_kernel_sym (OZ_Procmode cprocmode, void *dummy)

{
  int si;
  OZ_Image *image;
  OZ_Pointer log_p;
  uLong sts;

  si = oz_hw_cpu_setsoftint (0);						// keep cpu from switching so we can get current process
  for (image = NULL; (image = oz_knl_image_next (image, 1)) != NULL;) {		// scan list of system images
    sts = oz_knl_image_lookup (image, kernel_sym, &log_p);			// see if symbol exists therein
    if (sts == OZ_SUCCESS) {
      kernel_log = *(OZ_Log **)log_p;						// ok, get log pointer from it
      if (kernel_log != NULL) oz_knl_log_increfc (kernel_log, 1);		// inc ref count so it can't die on us
      else {
        sts = oz_knl_log_create (kernel_sym, (OZ_Log **)log_p);			// otherwise, create one
        if (sts == OZ_SUCCESS) kernel_log = *(OZ_Log **)log_p;
      }
      oz_hw_cpu_setsoftint (si);
      return (sts);
    }
  }
  oz_hw_cpu_setsoftint (si);
  return (OZ_UNDEFSYMBOL);
}

static uLong read_kernel_log (OZ_Procmode cprocmode, void *dummy)

{
  int si;
  uLong sts;

  si = oz_hw_cpu_setsoftint (0);
  while (1) {
    sts = oz_knl_log_remove (kernel_log, &kernel_lostlines, &kernel_datetime, &kernel_sourcefile, &kernel_sourceline, kernel_size, kernel_buff);
    if (sts != OZ_PENDING) break;
    oz_knl_log_wait (kernel_log);
  }
  oz_hw_cpu_setsoftint (si);
  return (sts);
}

static void barfcheck (uLong sts, uLong rlen, char *buff)

{
  uLong i;

  for (i = 0; i < rlen; i ++) {
    if (buff[i] & 0x80) oz_sys_callknl (barfcheck_knl, NULL);
  }
}

static uLong barfcheck_knl (OZ_Procmode cprocmode, void *dummy)

{
  oz_crash ("oz_util_logwrite: bad data read from pipe");
}
