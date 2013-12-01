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
/*  Manage an Unix-like fd array					*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_crtl_fd.h"
#include "oz_sys_threadlock.h"

#include <sys/mman.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

static int fdfirstfree   = 0;
static int fdhighestused = 0;
static OZ_Crtl_fd_array fdarray[NOFILE];
static OZ_SYS_THREADLOCK_INIT (fdlock, "oz_crtl_io fdlock");
static uLong unixbasedays = 0;

OZ_Crtl_fd_array *oz_crtl_fd_array = fdarray;

/************************************************************************/
/*									*/
/*  Allocate an unused fd						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_crtl_fd_alloc = -1 : all fd's in use				*/
/*	                        errno = EMFILE				*/
/*	                 else : fd to use				*/
/*									*/
/************************************************************************/

int oz_crtl_fd_alloc (void)

{
  int fd, tl;

  tl = OZ_SYS_THREADLOCK_WAIT (fdlock);
  for (fd = fdfirstfree; fd < fdhighestused; fd ++) if (!(fdarray[fd].allocated)) goto fd_found;
  if (fd == NOFILE) {
    fd = -1;
    errno = EMFILE;
    goto rtn;
  }
  fdhighestused = fd + 1;
fd_found:
  memset (fdarray + fd, 0, sizeof fdarray[fd]);
  fdarray[fd].allocated = 1;
  fdfirstfree = fd + 1;
rtn:
  OZ_SYS_THREADLOCK_CLR (fdlock, tl);
  return (fd);
}

/************************************************************************/
/*									*/
/*  Check to see if fd is in use					*/
/*									*/
/*    Input:								*/
/*									*/
/*	fd = fd to check						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_crtl_fd_check = 1 : fd is in use				*/
/*	                   0 : fd not in use				*/
/*	                       errno = EBADF				*/
/*									*/
/************************************************************************/

int oz_crtl_fd_check (int fd)

{
  if ((fd < NOFILE) && fdarray[fd].allocated) return (1);
  errno = EBADF;
  return (0);
}

/************************************************************************/
/*									*/
/*  Free off an fd							*/
/*									*/
/*    Input:								*/
/*									*/
/*	fd = fd to free off						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_crtl_fd_free = 0 : fd was not in use				*/
/*	                      errno = EBADF				*/
/*	                  1 : fd was in use, it is now freed off	*/
/*									*/
/************************************************************************/

int oz_crtl_fd_free (int fd)

{
  int rc, tl;

  tl = OZ_SYS_THREADLOCK_WAIT (fdlock);			/* lock fd array */
  rc = fdarray[fd].allocated;				/* see if it is in use */
  if (!rc) errno = EBADF;				/* if not, set up errno */
  else {
    if (fdarray[fd].h_event  != 0) oz_sys_handle_release (OZ_PROCMODE_KNL, fdarray[fd].h_event);
    if (fdarray[fd].h_iochan != 0) oz_sys_handle_release (OZ_PROCMODE_KNL, fdarray[fd].h_iochan);
    memset (fdarray + fd, 0, sizeof fdarray[fd]);	/* if so, clear the entry */
    if (fd < fdfirstfree) fdfirstfree = fd;		/* maybe lower the first free number */
  }
  OZ_SYS_THREADLOCK_CLR (fdlock, tl);			/* unlock fd array */

  return (rc);
}

/************************************************************************/
/*									*/
/*  Unix standard 'dup' routines to copy an fd				*/
/*									*/
/************************************************************************/

#define ALIASES(x) asm (" .globl __" #x ",__libc_" #x "\n __" #x "=" #x "\n __libc_" #x "=" #x);

int dup (int oldfd)

{
  int newfd;

  if (!oz_crtl_fd_check (oldfd)) return (-1);

  newfd = oz_crtl_fd_alloc ();
  if (newfd >= 0) fdarray[newfd] = fdarray[oldfd];

  return (newfd);
}

int dup2 (int oldfd, int newfd)

{
  int i, tl;

  if (!oz_crtl_fd_check (oldfd)) return (-1);

  if (newfd >= NOFILE) {
    errno = EBADF;
    return (-1);
  }

getnewfd:
  tl = OZ_SYS_THREADLOCK_WAIT (fdlock);
  if (newfd > fdhighestused) {
    for (i = fdhighestused; i < newfd; i ++) fdarray[i].allocated = 0;
    fdhighestused = newfd;
  }
  if ((newfd < fdhighestused) && fdarray[newfd].allocated) {
    OZ_SYS_THREADLOCK_CLR (fdlock, tl);
    close (newfd);
    goto getnewfd;
  }
  if (newfd == fdhighestused) fdhighestused ++;
  fdarray[newfd] = fdarray[oldfd];
  OZ_SYS_THREADLOCK_CLR (fdlock, tl);

  return (newfd);
}

ALIASES (dup)
ALIASES (dup2)
