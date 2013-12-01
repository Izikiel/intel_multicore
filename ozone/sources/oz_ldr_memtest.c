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

#include "ozone.h"
#include "oz_knl_printk.h"

static char pattern0 (char *addr);
static char pattern1 (char *addr);
static char pattern2 (char *addr);

static char (*patterns[3]) (char *addr) = { pattern0, pattern1, pattern2 };

uLong oz_ldr_start ()

{
  char buff[16], *p, *start;
  int megs, readpatt, writepatt;

  do {
    oz_hw_getcon (sizeof buff, buff, 36, "\noz_ldr_memtest: megabytes (gt 16)> ");
    megs = oz_hw_atoi (buff, &megs);
  } while (megs <= 16);

  do {
    oz_hw_getcon (sizeof buff, buff, 38, "\noz_ldr_memtest: read pattern (0..2)> ");
    readpatt = buff[0] - '0';
  } while ((readpatt < 0) || (readpatt > 2) || (buff[1] != 0));

  do {
    oz_hw_getcon (sizeof buff, buff, 32, "\noz_ldr_memtest: write pattern> ");
    writepatt = buff[0] - '0';
  } while ((writepatt < 0) || (writepatt > 2) || (buff[1] != 0));

  oz_knl_printk ("oz_ldr_memtest: scanning 16..%d\n", megs);
  start = NULL;
  for (p = (char *)(16 * 0x100000); p < (char *)(megs * 0x100000); p ++) {
    if (*p == (*patterns[readpatt]) (p)) if (start == NULL) start = p;
    else {
      if (start != NULL) {
        oz_knl_printk ("oz_ldr_memtest: %p thru %p survived\n", start, p - 1);
        start = NULL;
      }
    }
    *p = (*patterns[writepatt]) (p);
  }
  if (start != NULL) oz_knl_printk ("oz_ldr_memtest: %p thru %p survived\n", start, p - 1);
  oz_knl_printk ("oz_ldr_memtest: scan complete\n");
  while (1) {}
}

static char pattern0 (char *addr)

{
  return ((char) addr);
}

static char pattern1 (char *addr)

{
  return (((char) addr) + 1);
}

static char pattern2 (char *addr)

{
  int a;

  a = (int) addr;
  return (a + (a >> 8));
}


void oz_dev_inits (void)

{ }

void oz_knl_crash_dump (OZ_Sigargs *sigargs, OZ_Mchargs *mchargs, OZ_Mchargx_knl *mchargx_knl)

{
  oz_knl_printk ("oz_knl_crash_dump: not supported in memtest\n");
  while (1) {}
}
