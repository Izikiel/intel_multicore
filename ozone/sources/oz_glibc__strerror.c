//+++2001-10-06
//    Copyright (C) 2001, Mike Rieker, Beverly, MA USA
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
//---2001-10-06

/* Copyright (C) 1991, 1993, 1995, 1996 Free Software Foundation, Inc.
This file is part of the GNU C Library.

The GNU C Library is free software; you can redistribute it and/or
modify it under the terms of the GNU Library General Public License as
published by the Free Software Foundation; either version 2 of the
License, or (at your option) any later version.

The GNU C Library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Library General Public License for more details.

You should have received a copy of the GNU Library General Public
License along with the GNU C Library; see the file COPYING.LIB.  If
not, write to the Free Software Foundation, Inc., 675 Mass Ave,
Cambridge, MA 02139, USA.  */

#include <errno.h>
#include <libintl.h>
#include <stdio.h>
#include <string.h>

#ifndef HAVE_GNU_LD
#define _sys_errlist sys_errlist
#define _sys_nerr sys_nerr
#endif

/* It is critical here that we always use the `dcgettext' function for
   the message translation.  Since <libintl.h> only defines the macro
   `dgettext' to use `dcgettext' for optimizing programs this is not
   always guaranteed.  */
#ifndef dgettext
# include <locale.h>		/* We need LC_MESSAGES.  */
# define dgettext(domainname, msgid) dcgettext (domainname, msgid, LC_MESSAGES)
#endif

extern unsigned int errno_ozsts;
extern int _sys_nerr;
extern const char *const _sys_errlist[];

/* Return a string describing the errno code in ERRNUM.  */
char *
_strerror_internal (int errnum, char *buf, size_t buflen)
{
  if (errnum == 65535) {
    oz_sys_sprintf (buflen, buf, "OZ error %u", errno_ozsts);
    return (buf);
  }
  if (errnum < 0 || errnum >= _sys_nerr)
    {
      oz_sys_sprintf (buflen, buf, "Unknown error %d", errnum);
      return buf;
    }

  return (char *) (_sys_errlist[errnum]);
}
