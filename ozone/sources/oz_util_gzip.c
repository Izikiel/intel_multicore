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
/*  GZIP utiliti							*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_crtl_malloc.h"
#include "oz_io_fs.h"
#include "oz_knl_lock.h"
#include "oz_knl_status.h"
#include "oz_sys_io.h"
#include "oz_sys_io_fs.h"
#include "oz_sys_io_fs_printf.h"
#include "oz_util_start.h"

#include "gzip.h"

static char *pn = "gzip";

static char *input_file, *output_file;
static OZ_Handle h_in, h_out;
static uLong exstat;

static int readroutine (void *dummy, int siz, char *buf, int *len, char **pnt);
static int writeroutine (void *dummy, int siz, char *buf);
static void errorroutine (void *dummy, int code, char *msg);
static void *mallocroutine (void *dummy, int size);
static void freeroutine (void *dummy, void *buff);

uLong oz_util_main (int argc, char *argv[])

{
  int fc;
  OZ_IO_fs_create fs_create;
  OZ_IO_fs_open fs_open;
  uLong sts;

  if (argc > 0) pn = argv[0];
  if (argc != 3) {
    oz_sys_io_fs_printf (oz_util_h_error, "usage: %s <input> <output>\n", pn);
    return (OZ_MISSINGPARAM);
  }

  fc = GZIP_FUNC_DUMMY;
  if (strcasecmp (pn, "gzip") == 0) fc = GZIP_FUNC_COMPRESS;
  if (strcasecmp (pn, "gunzip") == 0) fc = GZIP_FUNC_EXPAND;
  if (fc == GZIP_FUNC_DUMMY) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: program name must be gzip or gunzip\n", pn);
    return (OZ_BADPARAM);
  }

  input_file = argv[1];
  memset (&fs_open, 0, sizeof fs_open);
  fs_open.name     = input_file;
  fs_open.lockmode = OZ_LOCKMODE_PR;
  sts = oz_sys_io_fs_open (sizeof fs_open, &fs_open, 0, &h_in);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u opening input file %s\n", pn, sts, input_file);
    return (sts);
  }

  output_file = argv[2];
  memset (&fs_create, 0, sizeof fs_create);
  fs_create.name     = output_file;
  fs_create.lockmode = OZ_LOCKMODE_PW;
  sts = oz_sys_io_fs_create (sizeof fs_create, &fs_create, 0, &h_out);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u creating output file %s\n", pn, sts, output_file);
    return (sts);
  }

  exstat = OZ_SUCCESS;
  gzip (readroutine, writeroutine, errorroutine, mallocroutine, freeroutine, NULL, fc, 6);

  if (exstat == OZ_SUCCESS) {
    sts = oz_sys_io (OZ_PROCMODE_KNL, h_out, 0, OZ_IO_FS_CLOSE, 0, NULL);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u closing output file %s\n", pn, sts, output_file);
      exstat = sts;
    }
  }

  return (exstat);
}

static int readroutine (void *dummy, int siz, char *buf, int *len, char **pnt)

{
  OZ_IO_fs_readrec fs_readrec;
  uLong rlen, sts;

  memset (&fs_readrec, 0, sizeof fs_readrec);
  fs_readrec.size = siz;
  fs_readrec.buff = buf;
  fs_readrec.rlen = &rlen;
  do sts = oz_sys_io (OZ_PROCMODE_KNL, h_in, 0, OZ_IO_FS_READREC, sizeof fs_readrec, &fs_readrec);
  while ((sts == OZ_SUCCESS) && (rlen == 0));
  if ((sts != OZ_SUCCESS) && (sts != OZ_ENDOFFILE)) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u reading %s\n", pn, sts, input_file);
    exstat = sts;
    return (0);
  }

  *len = rlen;
  *pnt = buf;
  return (1);
}

static int writeroutine (void *dummy, int siz, char *buf)

{
  OZ_IO_fs_writerec fs_writerec;
  uLong sts;

  memset (&fs_writerec, 0, sizeof fs_writerec);
  fs_writerec.size = siz;
  fs_writerec.buff = buf;
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_out, 0, OZ_IO_FS_WRITEREC, sizeof fs_writerec, &fs_writerec);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u writing %s\n", pn, sts, output_file);
    exstat = sts;
    return (0);
  }
  return (1);
}

static void errorroutine (void *dummy, int code, char *msg)

{
  oz_sys_io_fs_printf (oz_util_h_error, "%s: gzip internal error %d: %s\n", pn, code, msg);
  exstat = OZ_BUGCHECK;
}

static void *mallocroutine (void *dummy, int size)

{
  return (malloc (size));
}

static void freeroutine (void *dummy, void *buff)

{
  free (buff);
}
