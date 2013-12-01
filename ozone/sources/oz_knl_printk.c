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
/*  Print a debugging message to the console				*/
/*									*/
/************************************************************************/

#include <stdarg.h>

#include "ozone.h"
#include "oz_knl_hw.h"
#include "oz_knl_printk.h"
#include "oz_knl_sdata.h"
#include "oz_knl_status.h"
#include "oz_sys_io_fs.h"
#include "oz_sys_io_fs_printf.h"
#include "oz_sys_xprintf.h"

static uLong printbuf (void *dummy, uLong *size, char **buff);
static void printhex (OZ_Pointer v, int l);
static void printchar (char c);

/* Dump a block of memory */

void oz_knl_dumpmem (uLong size, const void *buff)

{
  oz_knl_dumpmem2 (size, buff, (OZ_Pointer)buff);
}

void oz_knl_dumpmem2 (uLong size, const void *buff, OZ_Pointer addr)

{
  uByte b;
  const uByte *bp;
  uLong i, j;

  union { uLong l;
          uByte b[4];
        } letest;

  bp = buff;
  letest.l = 1;
  if (letest.b[0]) {
    for (i = 0; i < size; i += 16) {
      printchar (' ');
      printchar (' ');
      for (j = 16; j > 0;) {
        if (!(j & 3)) printchar (' ');
        if (-- j + i < size) printhex (bp[i+j], 2);
        else { printchar (' '); printchar (' '); }
      }
      printchar (' ');
      printchar (':');
      printchar (' ');
      printhex (i + addr, 2 * sizeof (OZ_Pointer));
      printchar (' ');
      printchar (':');
      printchar (' ');
      for (j = 0; (j < 16) && (i + j < size); j ++) {
        b = bp[i+j];
        if ((b < ' ') || (b >= 127)) b = '.';
        printchar (b);
      }
      printchar ('\n');
    }
  } else {
    for (i = 0; i < size; i += 16) {
      printchar (' ');
      printchar (' ');
      printhex (i + addr, 2 * sizeof (OZ_Pointer));
      printchar (' ');
      printchar (':');
      printchar (' ');
      for (j = 0; j < 16; j ++) {
        if (!(j & 3)) printchar (' ');
        if (j + i < size) printhex (bp[i+j], 2);
        else { printchar (' '); printchar (' '); }
      }
      printchar (' ');
      printchar (':');
      printchar (' ');
      for (j = 0; (j < 16) && (i + j < size); j ++) {
        b = bp[i+j];
        if ((b < ' ') || (b >= 127)) b = '.';
        printchar (b);
      }
      printchar ('\n');
    }
  }
}

/* Print a message to the console with args inline */

void oz_knl_printk (const char *fmt, ...)

{
  va_list ap;

  va_start (ap, fmt);
  oz_knl_printkv (fmt, ap);
  va_end (ap);
}

/* Print a message to the console with pointer to arg list */

void oz_knl_printkv (const char *fmt, va_list ap)

{
  char buf[256];

  oz_sys_vxprintf (printbuf, NULL, sizeof buf, buf, NULL, fmt, ap);
}

static uLong printbuf (void *dummy, uLong *size, char **buff)

{
  if (oz_hw_inknlmode ()) oz_hw_putcon (*size, *buff);
  else oz_sys_io_fs_printerror ("%*.*s", *size, *size, *buff);
  return (OZ_SUCCESS);
}

static void printhex (OZ_Pointer v, int l)

{
  int i;

  for (i = l; -- i >= 0;) printchar ("0123456789ABCDEF"[(v>>(i*4))&15]);
}

static void printchar (char c)

{
  char b;

  b = c;
  if (oz_hw_inknlmode ()) oz_hw_putcon (1, &b);
  else oz_sys_io_fs_printerror ("%c", b);
}
