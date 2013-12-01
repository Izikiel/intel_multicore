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
/*  Unix 'pipe'-I/O routines						*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_crtl_fd.h"
#include "oz_io_pipe.h"
#include "oz_knl_status.h"

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

#define EOZSYSERR 65535

globalref uLong errno_ozsts;

static ssize_t pipeio_read (int fd, __ptr_t buf, size_t nbytes);
static ssize_t pipeio_write (int fd, const __ptr_t buf, size_t nbytes);

static const OZ_Crtl_fd_driver pipeio_driver = { NULL, NULL, pipeio_read, pipeio_write, NULL, NULL };

int pipe (int filedes[2])

{
  int fd1, fd2;
  OZ_Handle h_pipe1, h_pipe2;
  uLong sts;

  /* Allocate two fd's */

  fd1 = oz_crtl_fd_alloc ();
  if (fd1 < 0) return (fd1);

  fd2 = oz_crtl_fd_alloc ();
  if (fd2 < 0) {
    oz_crtl_fd_free (fd1);
    return (fd2);
  }

  /* Create a pipe and assign two channels to it */

  sts = oz_sys_io_assign (OZ_PROCMODE_KNL, &h_pipe1, OZ_IO_PIPES_TEMPLATE, OZ_LOCKMODE_EX);
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);

  sts = oz_sys_io_chancopy (OZ_PROCMODE_KNL, h_pipe1, OZ_LOCKMODE_XX, &h_pipe2);
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);

  /* Create an event flag for each fd */

  sts = oz_sys_event_create (OZ_PROCMODE_KNL, "oz_crtl_pipeio", &(oz_crtl_fd_array[fd1].h_event));
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);

  sts = oz_sys_event_create (OZ_PROCMODE_KNL, "oz_crtl_pipeio", &(oz_crtl_fd_array[fd2].h_event));
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);

  /* Set up fd array entries */

  oz_crtl_fd_array[fd1].h_iochan = h_pipe1;
  oz_crtl_fd_array[fd1].flags1    = 0;			// this one is for fildes[0]
  oz_crtl_fd_array[fd1].driver   = &pipeio_driver;	// special read/write routines

  oz_crtl_fd_array[fd2].h_iochan = h_pipe2;
  oz_crtl_fd_array[fd2].flags1    = 1;			// this one is for fildes[1]
  oz_crtl_fd_array[fd2].driver   = &pipeio_driver;	// special read/write routines

  /* Return the allocated fd's */

  filedes[0] = fd1;
  filedes[1] = fd2;
  return (0);
}

static ssize_t pipeio_read (int fd, __ptr_t buf, size_t nbytes)

{
  OZ_IO_fs_readrec fs_readrec;
  uLong rlen, sts;

  if (oz_crtl_fd_array[fd].flags1 != 0) {
    errno = EINVAL;
    return (-1);
  }

  memset (&fs_readrec, 0, sizeof fs_readrec);
  fs_readrec.size = nbytes;
  fs_readrec.buff = buf;
  fs_readrec.rlen = &rlen;

  sts = oz_sys_io (OZ_PROCMODE_KNL, oz_crtl_fd_array[fd].h_iochan, oz_crtl_fd_array[fd].h_event, 
                   OZ_IO_FS_READREC, sizeof fs_readrec, &fs_readrec);

  switch (sts) {
    case   OZ_SUCCESS: return (rlen);
    case OZ_ENDOFFILE: return (0);
    case     OZ_CHANOTBOUND: errno = ENOTCONN;     break;
    case          OZ_ACCVIO: errno = EFAULT;       break;
    case   OZ_NOROUTETODEST: errno = ENETUNREACH;  break;
    case     OZ_LINKDROPPED:
    case     OZ_LINKABORTED: 
    case         OZ_ABORTED: errno = ECONNRESET;   break;
    default: errno = EOZSYSERR; errno_ozsts = sts; break;
  }
  return (-1);
}

static ssize_t pipeio_write (int fd, const __ptr_t buf, size_t nbytes)

{
  OZ_IO_fs_writerec fs_writerec;
  uLong sts, wlen;

  if (oz_crtl_fd_array[fd].flags1 != 1) {
    errno = EINVAL;
    return (-1);
  }

  memset (&fs_writerec, 0, sizeof fs_writerec);
  fs_writerec.size = nbytes;
  fs_writerec.buff = buf;
  fs_writerec.wlen = &wlen;

  sts = oz_sys_io (OZ_PROCMODE_KNL, oz_crtl_fd_array[fd].h_iochan, oz_crtl_fd_array[fd].h_event, 
                   OZ_IO_FS_WRITEREC, sizeof fs_writerec, &fs_writerec);

  switch (sts) {
    case   OZ_SUCCESS: return (wlen);
    case OZ_ENDOFFILE: return (0);
    case     OZ_CHANOTBOUND: errno = ENOTCONN;     break;
    case          OZ_ACCVIO: errno = EFAULT;       break;
    case   OZ_NOROUTETODEST: errno = ENETUNREACH;  break;
    case     OZ_LINKDROPPED:
    case     OZ_LINKABORTED: 
    case         OZ_ABORTED: errno = ECONNRESET;   break;
    default: errno = EOZSYSERR; errno_ozsts = sts; break;
  }
  return (-1);
}
