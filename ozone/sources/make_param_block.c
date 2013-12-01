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
/*  This utility creates the param_block.4096 file			*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_knl_handle.h"
#include "oz_knl_status.h"
#include "oz_ldr_loader.h"
#include "oz_sys_dateconv.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/time.h>

OZ_Loadparams oz_ldr_paramblock;

static void printparam (const char *format, ...);

int main (int argc, char *argv[])

{
  char *filename;
  FILE *pf;
  int i;
  OZ_Datebin now;

  if (sizeof oz_ldr_paramblock != 4096) {
    fprintf (stderr, "param block size is %d, not 4096\n", sizeof oz_ldr_paramblock);
    return (-1);
  }

  memset (&oz_ldr_paramblock, 0, sizeof oz_ldr_paramblock);	/* default everything to zeroes/null strings */
  oz_ldr_paramblock.monochrome = 1;				/* but set this in case of black-and-white screen */

  filename = NULL;
  for (i = 1; i < argc; i ++) {
    if (strcasecmp (argv[i], "-set") == 0) {
      if (i + 2 >= argc) goto usage;
      if (!oz_ldr_set (printparam, argv[i+1], argv[i+2])) goto badparam;
      i += 2;
      continue;
    }
    if (strcasecmp (argv[i], "-extra") == 0) {
      if (i + 2 >= argc) goto usage;
      if (!oz_ldr_extra (printparam, argv[i+1], argv[i+2])) goto badparam;
      i += 2;
      continue;
    }
    if (filename != NULL) goto usage;
    filename = argv[i];
  }

  if (filename == NULL) goto usage;

  pf = fopen (filename, "w");
  if (pf == NULL) {
    fprintf (stderr, "error creating %s: %s\n", filename, strerror (errno));
    return (-1);
  }

  now = oz_hw_tod_getnow ();				/* set up signature */
  oz_sys_datebin_decstr (0, now, sizeof oz_ldr_paramblock.signature, oz_ldr_paramblock.signature);

  fwrite (&oz_ldr_paramblock, sizeof oz_ldr_paramblock, 1, pf);
  fclose (pf);

  return (0);

badparam:
usage:
  fprintf (stderr, "usage: make_param_block [-set param value] [-extra param value] <filename>\n");
  return (-1);
}

static void printparam (const char *format, ...)

{
  va_list ap;

  va_start (ap, format);
  vprintf (format, ap);
  va_end (ap);
}

/* Get current utc */

OZ_Datebin oz_hw_tod_getnow (void)

{
  uLong basedaynumber;
  uLong nowlongs[OZ_DATELONG_ELEMENTS];
  OZ_Datebin nowbin;
  struct timeval nowtime;

  gettimeofday (&nowtime, NULL);
  memset (nowlongs, 0, sizeof nowlongs);
  basedaynumber = oz_sys_daynumber_encode ((1970 << 16) | (1 << 8) | 1);
  nowlongs[OZ_DATELONG_DAYNUMBER] = basedaynumber + nowtime.tv_sec / 86400;
  nowlongs[OZ_DATELONG_SECOND]    = nowtime.tv_sec % 86400;
#if OZ_TIMER_RESOLUTION < 1000000
  error : code below assumes OZ TIMER RESOLUTION >= 1,000,000
#endif
#if (OZ_TIMER_RESOLUTION % 1000000) != 0
  error : code below assumes OZ TIMER RESOLUTION is multiple of 1,000,000
#endif
  nowlongs[OZ_DATELONG_FRACTION]  = nowtime.tv_usec * (OZ_TIMER_RESOLUTION / 1000000);
  nowbin = oz_sys_datebin_encode (nowlongs);
  return (nowbin);
}

/* Use default timezone conversion */

uLong oz_sys_tzconv (OZ_Datebin in, OZ_Handle h_tzfilein, OZ_Datebin *out, int tznameoutl, char *tznameout)

{
  return (OZ_BADTZFILE);
}

/* Crash routine */

void oz_crash (const char *fmt, ...)

{
  va_list ap;

  va_start (ap, fmt);
  vfprintf (stderr, fmt, ap);
  va_end (ap);
  abort ();
}
