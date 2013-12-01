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
/*  Formatted print routines						*/
/*									*/
/************************************************************************/

#include <stdarg.h>

#include "ozone.h"

#include "oz_knl_handle.h"
#include "oz_knl_hw.h"
#include "oz_knl_status.h"
#include "oz_io_fs.h"
#include "oz_sys_io_fs_printf.h"
#include "oz_sys_xprintf.h"

static char *printhex (uLong v, int l, char *p);
static uLong fswrite (void *h_iochan, uLong *size, char **buff);

/************************************************************************/
/*									*/
/*  Dump memory								*/
/*									*/
/*    Input:								*/
/*									*/
/*	h_iochan = output channel (or 0 for OZ_ERROR)			*/
/*	size = size of memory to dump					*/
/*	buff = address of memory to dump				*/
/*									*/
/************************************************************************/

uLong oz_sys_io_fs_dumpmem (OZ_Handle h_iochan, uLong size, const void *buff)

{
  const uByte *bp;
  char linebuf[128], *p;
  OZ_Handle h_output;
  uByte b;
  uLong i, j, pk, sts;

  union { uLong l;
          uByte b[4];
        } letest;

  h_output = h_iochan;
  if (h_iochan == 0) h_output = oz_sys_io_fs_get_h_error ();

  sts = OZ_SUCCESS;

  bp = buff;
  letest.l = 1;
  if (letest.b[0]) {

    /* Little endian */

    for (i = 0; i < size; i += 16) {
      p = linebuf;						/* point to beginning of line buffer */
      *(p ++) = ' ';						/* a couple spaces to start */
      *(p ++) = ' ';
      for (j = 16; j > 0;) {					/* up to 16 bytes of data per line */
        if (!(j & 3)) *(p ++) = ' ';				/* every fourth byte put in a space */
        if (-- j + i < size) p = printhex (bp[i+j], 2, p);	/* output a data byte as 2 hex digits */
        else { *(p ++) = ' '; *(p ++) = ' '; }			/* (or spaces if end of data) */
      }
      *(p ++) = ' ';						/* output " : " */
      *(p ++) = ':';
      *(p ++) = ' ';
      p = printhex (i + (OZ_Pointer)bp, 8, p);			/* output the address */
      *(p ++) = ' ';						/* output " : " */
      *(p ++) = ':';
      *(p ++) = ' ';
      for (j = 0; (j < 16) && (i + j < size); j ++) {		/* output characters */
        b = bp[i+j];
        if ((b < ' ') || (b >= 127)) b = '.';
        *(p ++) = b;
      }
      *p = 0;							/* output the line */
      sts = oz_sys_io_fs_printf (h_output, "%s\n", linebuf);
      if (sts != OZ_SUCCESS) break;
    }
  } else {

    /* Big endian */

    for (i = 0; i < size; i += 16) {
      p = linebuf;
      *(p ++) = ' ';
      *(p ++) = ' ';
      p = printhex (i + (OZ_Pointer)bp, 8, p);
      *(p ++) = ' ';
      *(p ++) = ':';
      *(p ++) = ' ';
      for (j = 0; j < 16; j ++) {
        if (!(j & 3)) *(p ++) = ' ';
        if (j + i < size) p = printhex (bp[i+j], 2, p);
        else { *(p ++) = ' '; *(p ++) = ' '; }
      }
      *(p ++) = ' ';
      *(p ++) = ':';
      *(p ++) = ' ';
      for (j = 0; (j < 16) && (i + j < size); j ++) {
        b = bp[i+j];
        if ((b < ' ') || (b >= 127)) b = '.';
        *(p ++) = b;
      }
      *p = 0;
      sts = oz_sys_io_fs_printf (h_output, "%s\n", linebuf);
      if (sts != OZ_SUCCESS) break;
    }
  }

  if (h_iochan == 0) oz_sys_handle_release (OZ_PROCMODE_KNL, h_output);

  return (sts);
}

static char *printhex (uLong v, int l, char *p)

{
  int i;

  for (i = l; -- i >= 0;) *(p ++) = "0123456789ABCDEF"[(v>>(i*4))&15];
  return (p);
}

/************************************************************************/
/*									*/
/*  Print error message on OZ_ERROR					*/
/*									*/
/************************************************************************/

void oz_sys_io_fs_printerror (const char *format, ...)

{
  va_list ap;

  va_start (ap, format);
  if (oz_hw_inknlmode () && (oz_hw_cpu_smplevel () != OZ_SMPLOCK_NULL)) {
    oz_knl_printkv (format, ap);
  } else {
    oz_sys_io_fs_printerrorv (format, ap);
  }
  va_end (ap);
}

void oz_sys_io_fs_printerrorv (const char *format, va_list ap)

{
  OZ_Handle h_error;

  h_error = oz_sys_io_fs_get_h_error ();
  oz_sys_io_fs_vprintf (h_error, format, ap);
  oz_sys_handle_release (OZ_PROCMODE_KNL, h_error);
}

/************************************************************************/
/*									*/
/*  Get handler to user-mode OZ_ERROR					*/
/*									*/
/************************************************************************/

OZ_Handle oz_sys_io_fs_get_h_error (void)

{
  uLong sts;
  OZ_Handle h_defaulttbl, h_error, h_logname;

  sts = oz_sys_logname_lookup (0, OZ_PROCMODE_USR, "OZ_DEFAULT_TBL", NULL, NULL, NULL, &h_defaulttbl);
  if (sts != OZ_SUCCESS) return (0);

  sts = oz_sys_logname_lookup (h_defaulttbl, OZ_PROCMODE_USR, "OZ_ERROR", NULL, NULL, NULL, &h_logname);
  oz_sys_handle_release (OZ_PROCMODE_KNL, h_defaulttbl);
  if (sts != OZ_SUCCESS) return (0);

  sts = oz_sys_logname_getval (h_logname, 0, NULL, 0, NULL, NULL, &h_error, OZ_OBJTYPE_IOCHAN, NULL);
  oz_sys_handle_release (OZ_PROCMODE_KNL, h_logname);
  if (sts != OZ_SUCCESS) return (0);
  return (h_error);
}

/************************************************************************/
/*									*/
/*  'Print' to a file							*/
/*									*/
/*    Input:								*/
/*									*/
/*	h_iochan = channel with an OZ_IO_FS class device		*/
/*	format = print format string					*/
/*	arg = argument list						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_io_fs_printf = OZ_SUCCESS : successful			*/
/*	                            else : error status			*/
/*									*/
/************************************************************************/

uLong oz_sys_io_fs_printf (OZ_Handle h_iochan, const char *format, ...)

{
  uLong sts;
  va_list ap;

  va_start (ap, format);				/* point to argument list */
  sts = oz_sys_io_fs_vprintf (h_iochan, format, ap);	/* output to the h_iochan file */
  va_end (ap);						/* end of argument list */
  return (sts);						/* return status */
}

uLong oz_sys_io_fs_vprintf (OZ_Handle h_iochan, const char *format, va_list ap)

{
  char buff[4096];
  OZ_Handle hiochan;

  hiochan = h_iochan;

  return (oz_sys_vxprintf (fswrite, &hiochan, sizeof buff, buff, NULL, format, ap));
}

/* Write data to end of file */

static uLong fswrite (void *h_iochan, uLong *size, char **buff)

{
  uLong sts;
  OZ_IO_fs_writerec fs_writerec;

  memset (&fs_writerec, 0, sizeof fs_writerec);
  fs_writerec.size = *size;
  fs_writerec.buff = *buff;
  fs_writerec.append = 1;
  sts = oz_sys_io (OZ_PROCMODE_KNL, *((OZ_Handle *)h_iochan), 0, OZ_IO_FS_WRITEREC, sizeof fs_writerec, &fs_writerec);
  return (sts);
}
