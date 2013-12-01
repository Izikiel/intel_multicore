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
/*  Unix-like I/O routines						*/
/*									*/
/************************************************************************/

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

#include "ozone.h"
#include "oz_crtl_fd.h"
#include "oz_io_console.h"
#include "oz_io_fs.h"
#include "oz_knl_lock.h"
#include "oz_knl_objtype.h"
#include "oz_knl_process.h"
#include "oz_knl_sdata.h"
#include "oz_knl_section.h"
#include "oz_knl_status.h"
#include "oz_sys_condhand.h"
#include "oz_sys_dateconv.h"
#include "oz_sys_handle.h"
#include "oz_sys_io_fs.h"
#include "oz_sys_logname.h"
#include "oz_sys_process.h"
#include "oz_sys_section.h"
#include "oz_sys_threadlock.h"
#include "oz_sys_xprintf.h"

#define EOZSYSERR 65535

#define ALIAS(x) asm (" .globl __" #x "\n __" #x "=" #x );
#define ALIAS64(x) asm (" .globl " #x "64\n " #x "64=" #x );
#define ALIASES(x) asm (" .globl __" #x ",__libc_" #x "\n __" #x "=" #x "\n __libc_" #x "=" #x);

globalref uLong errno_ozsts;

static const char *dow[ 7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *mon[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

static uLong unixbasedays = 0;

time_t oz_crtl_tounixtime (OZ_Datebin oztime);

static int fsio_fxstat (int ver, int fildes, struct stat *buf);
static off_t fsio_lseek (int fd, off_t offset, int whence);
static ssize_t fsio_read (int fd, __ptr_t buf, size_t nbytes);
static ssize_t fsio_write (int fd, const __ptr_t buf, size_t nbytes);
static int fsio_close (int fd);

static const OZ_Crtl_fd_driver fsio_driver = { fsio_fxstat, fsio_lseek, fsio_read, fsio_write, NULL, fsio_close };

static uLong io_fs_unlink (void *dummy, const char *devname, const char *filename, OZ_Handle h_iochan);
static uLong parse_newpath (void *npv, const char *devname, const char *filename, OZ_Handle h_iochan);
static uLong parse_oldpath (void *npv, const char *devname, const char *filename, OZ_Handle h_iochan);

int creat (const char *pathname, mode_t mode)

{
  return (open (pathname, O_CREAT | O_WRONLY | O_TRUNC, mode));
}

int open (const char *pathname, int flags, ...)

{
  const char *p;
  int fd, l;
  uLong sts;
  mode_t mode;
  OZ_Handle h_event, h_iochan;
  OZ_IO_fs_create fs_create;
  OZ_IO_fs_extend fs_extend;
  OZ_IO_fs_open fs_open;
  OZ_Lockmode lockmode;
  va_list ap;

  if (flags & O_CREAT) {									/* get mode iff O_CREAT */
    va_start (ap, flags);
    mode = va_arg (ap, mode_t);
    va_end (ap);
  }

  fd = oz_crtl_fd_alloc ();									/* allocate an fd */
  if (fd < 0) return (fd);

  h_event  = 0;
  h_iochan = 0;

  p = pathname;
  l = strlen (p);
  if (l >= OZ_EVENT_NAMESIZE) p += l - OZ_EVENT_NAMESIZE + 1;
  sts = oz_sys_event_create (OZ_PROCMODE_KNL, p, &h_event);					/* create an event flag for i/o */
  if (sts != OZ_SUCCESS) goto error_sys;

  lockmode = OZ_LOCKMODE_CR;
  if (((flags & O_ACCMODE) == O_WRONLY) || ((flags & O_ACCMODE) == O_RDWR)) lockmode = OZ_LOCKMODE_CW;

  if (flags & O_CREAT) {
    memset (&fs_create, 0, sizeof fs_create);							/* create the file */
    fs_create.name = pathname;
    fs_create.lockmode = lockmode;
    sts = oz_sys_io_fs_create (sizeof fs_create, &fs_create, 0, &h_iochan);
    if (sts == OZ_FILEALREADYEXISTS) {
      if (flags & O_EXCL) {
        errno = EEXIST;
        goto error;
      }
      memset (&fs_open, 0, sizeof fs_open);							/* open the file */
      fs_open.name = pathname;
      fs_open.lockmode = lockmode;
      sts = oz_sys_io_fs_open (sizeof fs_open, &fs_open, 0, &h_iochan);
    }
  } else {
    memset (&fs_open, 0, sizeof fs_open);							/* open the file */
    fs_open.name = pathname;
    fs_open.lockmode = lockmode;
    sts = oz_sys_io_fs_open (sizeof fs_open, &fs_open, 0, &h_iochan);
  }
  if (sts == OZ_NOSUCHFILE) goto error_nsf;
  if (sts != OZ_SUCCESS) goto error_sys;							/* abort processing if any error */

  if (flags & O_TRUNC) {									/* maybe truncate file */
    memset (&fs_extend, 0, sizeof fs_extend);
    sts = oz_sys_io (OZ_PROCMODE_KNL, h_iochan, h_event, OZ_IO_FS_EXTEND, sizeof fs_extend, &fs_extend);
    if (sts != OZ_SUCCESS) goto error_sys;							/* abort processing if any error */
  }

  oz_crtl_fd_array[fd].h_iochan = h_iochan;							/* success, set up fd struct element */
  oz_crtl_fd_array[fd].h_event  = h_event;
  oz_crtl_fd_array[fd].append   = ((flags & O_APPEND) != 0);
  oz_crtl_fd_array[fd].driver   = &fsio_driver;

  return (fd);

error_nsf:
  errno = ENOENT;
  goto error;

error_sys:
  errno = EOZSYSERR;
error:
  errno_ozsts = sts;
  oz_crtl_fd_free (fd);										/* release allocated fd */
  oz_sys_handle_release (OZ_PROCMODE_KNL, h_event);						/* release allocated event flag */
  oz_sys_handle_release (OZ_PROCMODE_KNL, h_iochan);						/* release allocated i/o channel */

  return (-1);
}

ALIAS64 (creat)
ALIASES (creat)
ALIAS64 (open)
ALIASES (open)

int lstat (const char *filename, struct stat *stat_buf)

{
  return (__lxstat (0, filename, stat_buf));
}

int __lxstat (int ver, __const char *filename, struct stat *stat_buf)

{
  return (_xstat (ver, filename, stat_buf));
}

int stat (const char *filename, struct stat *stat_buf)

{
  return (_xstat (0, filename, stat_buf));
}

int _xstat (int ver, __const char *filename, struct stat *stat_buf)

{
  int fd, rc;

  fd = open (filename, 0);		/* open file, flags=0 means use lockmode NL */
  if (fd < 0) return (fd);		/* if error, return now */
  rc = _fxstat (ver, fd, stat_buf);	/* ok, stat the file */
  close (fd);				/* close file */

  return (rc);				/* done */
}

int fstat (int fildes, struct stat *buf)

{
  return (_fxstat (0, fildes, buf));
}

int _fxstat (int ver, int fildes, struct stat *buf)

{
  if (!oz_crtl_fd_check (fildes)) return (-1);				/* make sure fd is valid */
  if (oz_crtl_fd_array[fildes].driver -> fxstat == NULL) {		/* see if there is an fxstat routine */
    errno = ESPIPE;							/* - this dev doesn't support it */
    return (-1);
  }
  return ((*(oz_crtl_fd_array[fildes].driver -> fxstat)) (ver, fildes, buf));
}

static int fsio_fxstat (int ver, int fildes, struct stat *buf)

{
  uLong sts;
  OZ_IO_fs_getinfo1 fs_getinfo1;

  memset (buf, 0, sizeof *buf);
  memset (&fs_getinfo1, 0, sizeof fs_getinfo1);				/* find out about file */
  sts = oz_sys_io (OZ_PROCMODE_KNL, oz_crtl_fd_array[fildes].h_iochan, oz_crtl_fd_array[fildes].h_event, 
                   OZ_IO_FS_GETINFO1, sizeof fs_getinfo1, &fs_getinfo1);
  if (sts != OZ_SUCCESS) {
    errno = EOZSYSERR;
    errno_ozsts = sts;
    return (-1);
  }

  buf -> st_dev     = 0;						/* ?? do better job filling these in ?? */
  buf -> st_ino     = 0;
  if (fs_getinfo1.filattrflags & OZ_FS_FILATTRFLAG_DIRECTORY) {
    buf -> st_mode  = S_IFDIR | 0777;
  } else {
    buf -> st_mode  = S_IFREG | 0777;
  }
  buf -> st_nlink   = 1;
  buf -> st_uid     = 0;
  buf -> st_gid     = 0;
  buf -> st_rdev    = 0;
  buf -> st_size    = (fs_getinfo1.eofblock - 1) * fs_getinfo1.blocksize + fs_getinfo1.eofbyte;
  buf -> st_blksize = fs_getinfo1.blocksize;
  buf -> st_blocks  = fs_getinfo1.hiblock;
  buf -> st_atime   = oz_crtl_tounixtime (fs_getinfo1.access_date);
  buf -> st_mtime   = oz_crtl_tounixtime (fs_getinfo1.modify_date);
  buf -> st_ctime   = oz_crtl_tounixtime (fs_getinfo1.change_date);

  return (0);
}

asm (" .globl __xstat\n __xstat=_xstat");
asm (" .globl __fxstat\n __fxstat=_fxstat");

/* Copied from includes/bits/stat.h                      */
/* Defining __USE_LARGEFILE64 didn't seem to do any good */

struct stat64
  {
    __dev_t st_dev;			/* Device.  */
    unsigned int __pad1;

    __ino_t __st_ino;			/* 32bit file serial number.	*/
    __mode_t st_mode;			/* File mode.  */
    __nlink_t st_nlink;			/* Link count.  */
    __uid_t st_uid;			/* User ID of the file's owner.	*/
    __gid_t st_gid;			/* Group ID of the file's group.*/
    __dev_t st_rdev;			/* Device number, if device.  */
    unsigned int __pad2;
    __off64_t st_size;			/* Size of file, in bytes.  */
    __blksize_t st_blksize;		/* Optimal block size for I/O.  */

    __blkcnt64_t st_blocks;		/* Number 512-byte blocks allocated. */
    __time_t st_atime;			/* Time of last access.  */
    unsigned long int __unused1;
    __time_t st_mtime;			/* Time of last modification.  */
    unsigned long int __unused2;
    __time_t st_ctime;			/* Time of last status change.  */
    unsigned long int __unused3;
    __ino64_t st_ino;			/* File serial number.		*/
  };

int __xstat64 (int ver, __const char *filename, struct stat64 *stat_buf)

{
  int fd, rc;

  fd = open64 (filename, 0);		/* open file, flags=0 means use lockmode NL */
  if (fd < 0) return (fd);		/* if error, return now */
  rc = __fxstat64 (ver, fd, stat_buf);	/* ok, stat the file */
  close (fd);				/* close file */

  return (rc);				/* done */
}

int __fxstat64 (int ver, int fildes, struct stat64 *buf)

{
  uLong sts;
  OZ_IO_fs_getinfo1 fs_getinfo1;

  if (!oz_crtl_fd_check (fildes)) return (-1);				/* make sure fd is valid */
  if (oz_crtl_fd_array[fildes].driver -> fxstat != fsio_fxstat) {	/* see if there is an fxstat routine */
    errno = ESPIPE;							/* - this dev doesn't support it */
    return (-1);
  }

  memset (buf, 0, sizeof *buf);
  memset (&fs_getinfo1, 0, sizeof fs_getinfo1);				/* find out about file */
  sts = oz_sys_io (OZ_PROCMODE_KNL, oz_crtl_fd_array[fildes].h_iochan, oz_crtl_fd_array[fildes].h_event, 
                   OZ_IO_FS_GETINFO1, sizeof fs_getinfo1, &fs_getinfo1);
  if (sts != OZ_SUCCESS) {
    errno = EOZSYSERR;
    errno_ozsts = sts;
    return (-1);
  }

  buf -> st_dev     = 0;						/* ?? do better job filling these in ?? */
  buf -> st_ino     = 0;
  if (fs_getinfo1.filattrflags & OZ_FS_FILATTRFLAG_DIRECTORY) {
    buf -> st_mode  = S_IFDIR | 0777;
  } else {
    buf -> st_mode  = S_IFREG | 0777;
  }
  buf -> st_nlink   = 1;
  buf -> st_uid     = 0;
  buf -> st_gid     = 0;
  buf -> st_rdev    = 0;
  buf -> st_size    = ((__off64_t)fs_getinfo1.eofblock - 1) * fs_getinfo1.blocksize + fs_getinfo1.eofbyte;
  buf -> st_blksize = fs_getinfo1.blocksize;
  buf -> st_blocks  = fs_getinfo1.hiblock;
  buf -> st_atime   = oz_crtl_tounixtime (fs_getinfo1.access_date);
  buf -> st_mtime   = oz_crtl_tounixtime (fs_getinfo1.modify_date);
  buf -> st_ctime   = oz_crtl_tounixtime (fs_getinfo1.change_date);

  return (0);
}

int flock (int fd, int operation)

{
  return (0); /* ?? */
}

int fcntl (int __fildes, int __cmd, ...)

{
  oz_sys_io_fs_printerror ("oz_crtl_io fcntl*: (%d, %d, )\n", __fildes, __cmd);	/* ?? */
  return (0);
}

off_t lseek (int fd, off_t offset, int whence)

{
  if (!oz_crtl_fd_check (fd)) return (-1);
  if (oz_crtl_fd_array[fd].driver -> lseek == NULL) {
    errno = ESPIPE;
    return (-1);
  }
  return ((*(oz_crtl_fd_array[fd].driver -> lseek)) (fd, offset, whence));
}

static off_t fsio_lseek (int fd, off_t offset, int whence)

{
  uLong atbyte, sts;
  OZ_Dbn atblock;
  OZ_IO_fs_getinfo1 fs_getinfo1;
  OZ_IO_fs_setcurpos fs_setcurpos;

  /* Get block size, current position, eof position, etc */

  memset (&fs_getinfo1, 0, sizeof fs_getinfo1);
  sts = oz_sys_io (OZ_PROCMODE_KNL, oz_crtl_fd_array[fd].h_iochan, oz_crtl_fd_array[fd].h_event, 
                   OZ_IO_FS_GETINFO1, sizeof fs_getinfo1, &fs_getinfo1);
  if (sts != OZ_SUCCESS) {
    errno = EOZSYSERR;
    errno_ozsts = sts;
    return (-1);
  }

  /* Get new desired position */

  switch (whence) {
    case SEEK_SET: {
      atblock = offset / fs_getinfo1.blocksize + 1;		/* move to absolute position */
      atbyte  = offset % fs_getinfo1.blocksize;
      break;
    }
    case SEEK_END: {
      fs_getinfo1.curblock = fs_getinfo1.eofblock;		/* relative to eof, position to eof for now */
      fs_getinfo1.curbyte  = fs_getinfo1.eofbyte;
								/* fall through with current position = eof position */
    }
    case SEEK_CUR: {
      atblock  = fs_getinfo1.curblock;				/* current, set up current position if not already set */
      atbyte   = fs_getinfo1.curbyte;
      atblock += offset / fs_getinfo1.blocksize;		/* move whole blocks relative to current position */
      offset %= fs_getinfo1.blocksize;				/* see what partial block to move */
      if (offset < 0) {
        atblock --;						/* if negative, make whole blocks one less */
        offset += fs_getinfo1.blocksize;			/* ... and increment partial block to compensate */
      }
      atbyte  += offset;					/* increment partial block offset */
      atblock += atbyte / fs_getinfo1.blocksize;		/* normalize partial block offset */
      atbyte  %= fs_getinfo1.blocksize;
      break;
    }
    default: {
      errno = EINVAL;
      return (-1);
    }
  }

  /* Position the file there - do this instead of using read/write atblock/atbyte in case the fd has been dup'd via dup/dup2 */

  memset (&fs_setcurpos, 0, sizeof fs_setcurpos);
  fs_setcurpos.atblock = atblock;
  fs_setcurpos.atbyte  = atbyte;
  sts = oz_sys_io (OZ_PROCMODE_KNL, oz_crtl_fd_array[fd].h_iochan, oz_crtl_fd_array[fd].h_event, OZ_IO_FS_SETCURPOS, sizeof fs_setcurpos, &fs_setcurpos);
  if (sts != OZ_SUCCESS) {
    errno = EOZSYSERR;
    errno_ozsts = sts;
    return (-1);
  }

  /* Return resultant absolute position */

  return ((atblock - 1) * fs_getinfo1.blocksize + atbyte);
}

ALIASES (lseek)

__off64_t lseek64 (int fd, __off64_t offset, int whence)

{
  uLong atbyte, sts;
  OZ_Dbn atblock;
  OZ_IO_fs_getinfo1 fs_getinfo1;
  OZ_IO_fs_setcurpos fs_setcurpos;

  if (!oz_crtl_fd_check (fd)) return (-1);
  if (oz_crtl_fd_array[fd].driver -> lseek != fsio_lseek) {
    errno = ESPIPE;
    return (-1);
  }

  /* Get block size, current position, eof position, etc */

  memset (&fs_getinfo1, 0, sizeof fs_getinfo1);
  sts = oz_sys_io (OZ_PROCMODE_KNL, oz_crtl_fd_array[fd].h_iochan, oz_crtl_fd_array[fd].h_event, 
                   OZ_IO_FS_GETINFO1, sizeof fs_getinfo1, &fs_getinfo1);
  if (sts != OZ_SUCCESS) {
    errno = EOZSYSERR;
    errno_ozsts = sts;
    return (-1);
  }

  /* Get new desired position */

  switch (whence) {
    case SEEK_SET: {
      atblock = offset / fs_getinfo1.blocksize + 1;		/* move to absolute position */
      atbyte  = offset % fs_getinfo1.blocksize;
      break;
    }
    case SEEK_END: {
      fs_getinfo1.curblock = fs_getinfo1.eofblock;		/* relative to eof, position to eof for now */
      fs_getinfo1.curbyte  = fs_getinfo1.eofbyte;
								/* fall through with current position = eof position */
    }
    case SEEK_CUR: {
      atblock  = fs_getinfo1.curblock;				/* current, set up current position if not already set */
      atbyte   = fs_getinfo1.curbyte;
      atblock += offset / fs_getinfo1.blocksize;		/* move whole blocks relative to current position */
      offset %= fs_getinfo1.blocksize;				/* see what partial block to move */
      if (offset < 0) {
        atblock --;						/* if negative, make whole blocks one less */
        offset += fs_getinfo1.blocksize;			/* ... and increment partial block to compensate */
      }
      atbyte  += offset;					/* increment partial block offset */
      atblock += atbyte / fs_getinfo1.blocksize;		/* normalize partial block offset */
      atbyte  %= fs_getinfo1.blocksize;
      break;
    }
    default: {
      errno = EINVAL;
      return (-1);
    }
  }

  /* Position the file there - do this instead of using read/write atblock/atbyte in case the fd has been dup'd via dup/dup2 */

  memset (&fs_setcurpos, 0, sizeof fs_setcurpos);
  fs_setcurpos.atblock = atblock;
  fs_setcurpos.atbyte  = atbyte;
  sts = oz_sys_io (OZ_PROCMODE_KNL, oz_crtl_fd_array[fd].h_iochan, oz_crtl_fd_array[fd].h_event, OZ_IO_FS_SETCURPOS, sizeof fs_setcurpos, &fs_setcurpos);
  if (sts != OZ_SUCCESS) {
    errno = EOZSYSERR;
    errno_ozsts = sts;
    return (-1);
  }

  /* Return resultant absolute position */

  return (((__off64_t)atblock - 1) * fs_getinfo1.blocksize + atbyte);
}

ssize_t read (int fd, __ptr_t buf, size_t nbytes)

{
  if (!oz_crtl_fd_check (fd)) return (-1);
  return ((*(oz_crtl_fd_array[fd].driver -> read)) (fd, buf, nbytes));
}

static ssize_t fsio_read (int fd, __ptr_t buf, size_t nbytes)

{
  uLong rlen, sts;
  OZ_IO_fs_readrec fs_readrec;

  memset (&fs_readrec, 0, sizeof fs_readrec);				/* set up read parameters */
  fs_readrec.size = nbytes;
  fs_readrec.buff = buf;
  fs_readrec.rlen = &rlen;
  sts = oz_sys_io (OZ_PROCMODE_KNL, oz_crtl_fd_array[fd].h_iochan, oz_crtl_fd_array[fd].h_event, 
                   OZ_IO_FS_READREC, sizeof fs_readrec, &fs_readrec);
  if ((sts != OZ_SUCCESS) && (sts != OZ_ENDOFFILE)) {
    errno = EOZSYSERR;							/* some error */
    errno_ozsts = sts;
    return (-1);
  }
  if ((rlen < nbytes) && (sts == OZ_SUCCESS) && isatty (fd)) {
    ((char *)buf)[rlen++] = '\n';					/* tack on a newline for terminals */
  }

#if 000
  {
    char threadname[32];
    oz_sys_thread_getname (0, sizeof threadname, threadname);
    oz_sys_io_fs_printerror ("oz_crtl_io read*: %s[%d] rlen %u\n", threadname, fd, rlen);
  }
#endif

  return (rlen);
}

ALIASES (read)

int unlink (const char *pathname)

{
  uLong sts;

  sts = oz_sys_io_fs_parse (pathname, 0, io_fs_unlink, NULL);
  if (sts == OZ_NOSUCHFILE) goto error_nsf;
  if (sts != OZ_SUCCESS) goto error_sys;
  return (0);

error_nsf:
  errno = ENOENT;
  goto error;

error_sys:
  errno = EOZSYSERR;
error:
  errno_ozsts = sts;
  return (-1);
}

ALIASES (unlink)

static uLong io_fs_unlink (void *dummy, const char *devname, const char *filename, OZ_Handle h_iochan)

{
  uLong sts;
  OZ_Handle h_file;
  OZ_IO_fs_remove fs_remove;

  if (h_iochan != 0) return (OZ_NOSUCHFILE);

  sts = oz_sys_io_fs_assign (devname, OZ_LOCKMODE_NL, &h_file);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printerror ("oz_crtl_io unlink: error %u assigning channel to %s\n", sts, devname);
    return (sts);
  }

  memset (&fs_remove, 0, sizeof fs_remove);
  fs_remove.name = filename;
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_file, 0, OZ_IO_FS_REMOVE, sizeof fs_remove, &fs_remove);

  oz_sys_handle_release (OZ_PROCMODE_KNL, h_file);
  return (sts);
}

mode_t umask (mode_t mask)

{
  return (0755);
}

ALIASES (umask)

int chmod (const char *path, mode_t mode)
{
  return (0);
}

ALIASES (chmod)

int brk (void *end_data_segment)

{
  oz_sys_io_fs_printerror ("oz_crtl_io brk: (%p)\n", end_data_segment);
  return (0);
}

void *sbrk (intptr_t increment)

{
  void *address;

  if (increment == 0) {
    address = mmap (0, 1, PROT_WRITE, MAP_ANON, -1, 0);
    if ((__ptr_t)address != (__ptr_t)-1) munmap (address, 1);
  } else {
    address = mmap (0, increment, PROT_WRITE, MAP_ANON, -1, 0);
  }
  return (address);
}

ALIAS (brk)
ALIAS (sbrk)

__ptr_t mmap (__ptr_t addr, size_t len, int prot, int flags, int fd, __off_t off)

{
  OZ_Handle h_section;
  OZ_Mempage npagem, svpage;
  OZ_Hw_pageprot pageprot;
  OZ_IO_fs_getinfo1 fs_getinfo1;
  OZ_Section_type sectype;
  uLong mapsecflags, sts;
  void *ra;

  if (len > 1000000) {
    oz_sys_io_fs_printerror ("oz_crtl_io mmap: bad length parameter %u at %p\n", len, oz_hw_getrtnadr (0));
    oz_sys_thread_exit (OZ_BADPARAM);
  }

  pageprot = OZ_HW_PAGEPROT_UR;						/* get protection */
  if (prot & PROT_WRITE) pageprot = OZ_HW_PAGEPROT_UW;

  sectype = OZ_SECTION_TYPE_ZEROES;					/* assume demand-zero section */
  if (!(flags & MAP_ANON)) {
    if (!oz_crtl_fd_check (fd)) return ((__ptr_t)-1);			/* not anonymous, make sure fd is valid */
    memset (&fs_getinfo1, 0, sizeof fs_getinfo1);			/* find out about file */
    sts = oz_sys_io (OZ_PROCMODE_KNL, oz_crtl_fd_array[fd].h_iochan, oz_crtl_fd_array[fd].h_event, 
                     OZ_IO_FS_GETINFO1, sizeof fs_getinfo1, &fs_getinfo1);
    if (sts != OZ_SUCCESS) {
      errno = EOZSYSERR;
      errno_ozsts = sts;
      return ((__ptr_t)-1);
    }
    if (off % fs_getinfo1.blocksize != 0) {				/* disk file offset must start on block boundary */
      errno = EINVAL;
      return ((__ptr_t)-1);
    }
    off = (off / fs_getinfo1.blocksize) + 1;				/* make it a VBN */
    sectype = 0;							/* not demand-zero section */
  }

  npagem = (len + (1 << OZ_HW_L2PAGESIZE) - 1) >> OZ_HW_L2PAGESIZE;	/* get number of pages */

  svpage = OZ_HW_VADDRTOVPAGE (addr);					/* get starting virtual page number */
  if (flags & MAP_FIXED) {
    mapsecflags = OZ_MAPSECTION_EXACT;
    if (OZ_HW_VADDRTOPOFFS (addr) != 0) {
      errno = EINVAL;
      return ((__ptr_t)-1);
    }
  } else {
    mapsecflags = OZ_HW_DVA_USRHEAP_AT;					/* if not given, put in heap area somewhere */
    svpage = OZ_HW_VADDRTOVPAGE (OZ_HW_DVA_USRHEAP_VA);
  }

  sts = oz_sys_section_create (OZ_PROCMODE_KNL, oz_crtl_fd_array[fd].h_iochan, npagem, off, sectype, &h_section);
  if (sts != OZ_SUCCESS) {
    errno = EOZSYSERR;
    errno_ozsts = sts;
    return ((__ptr_t)-1);
  }

  sts = oz_sys_process_mapsection (OZ_PROCMODE_KNL, h_section, &npagem, &svpage, mapsecflags, pageprot);
  oz_sys_handle_release (OZ_PROCMODE_KNL, h_section);
  if (sts != OZ_SUCCESS) {
    errno = EOZSYSERR;
    errno_ozsts = sts;
    return ((__ptr_t)-1);
  }

  return ((__ptr_t)OZ_HW_VPAGETOVADDR (svpage));
}

ALIASES (mmap)

int munmap (__ptr_t __addr, size_t __len)

{
  uLong sts;
  OZ_Mempage svpage;

  svpage = OZ_HW_VADDRTOVPAGE (__addr);
  sts = oz_sys_process_unmapsec (svpage);
  if (sts != OZ_SUCCESS) {
    errno = EOZSYSERR;
    errno_ozsts = sts;
    return (-1);
  }

  return (0);
}

ALIASES (munmap)

ssize_t write (int fd, const __ptr_t buf, size_t nbytes)

{
  if (!oz_crtl_fd_check (fd)) return (-1);
  return ((*(oz_crtl_fd_array[fd].driver -> write)) (fd, buf, nbytes));
}

static ssize_t fsio_write (int fd, const __ptr_t buf, size_t nbytes)

{
  uLong sts, wlen;
  OZ_IO_fs_writerec fs_writerec;

  wlen = nbytes;							/* in case driver doesn't set it */
  memset (&fs_writerec, 0, sizeof fs_writerec);				/* set up write parameters */
  fs_writerec.size   = nbytes;
  fs_writerec.buff   = buf;
  fs_writerec.wlen   = &wlen;
  fs_writerec.append = oz_crtl_fd_array[fd].append;
  sts = oz_sys_io (OZ_PROCMODE_KNL, oz_crtl_fd_array[fd].h_iochan, oz_crtl_fd_array[fd].h_event, 
                   OZ_IO_FS_WRITEREC, sizeof fs_writerec, &fs_writerec);
  if ((sts == OZ_SUCCESS) || (sts == OZ_ENDOFFILE)) {
#if 000
    {
      char threadname[32];
      oz_sys_thread_getname (0, sizeof threadname, threadname);
      oz_sys_io_fs_printerror ("oz_crtl_io write*: %s[%d] wlen %u\n", threadname, fd, wlen);
    }
#endif
    return (wlen);							/* if successful, return number of bytes actually written */
  }
  errno = EOZSYSERR;							/* some error */
  errno_ozsts = sts;
  return (-1);
}

ALIASES (write)

int ioctl (int fd, int request, ...)

{
  int rc;
  va_list ap;

  if (!oz_crtl_fd_check (fd)) return (-1);
  if (oz_crtl_fd_array[fd].driver -> vioctl == NULL) {
    errno = ENOTTY;
    return (-1);
  }
  va_start (ap, request);
  rc = (*(oz_crtl_fd_array[fd].driver -> vioctl)) (fd, request, ap);
  va_end (ap);
  return (rc);
}

ALIASES (ioctl)

int close (int fd)

{
  if (!oz_crtl_fd_check (fd)) return (-1);
  if (oz_crtl_fd_array[fd].driver -> close == NULL) {
    if (!oz_crtl_fd_free (fd)) return (-1);
    return (0);
  }
  return ((*(oz_crtl_fd_array[fd].driver -> close)) (fd));
}

static int fsio_close (int fd)

{
  uLong sts;

  sts = oz_sys_io (OZ_PROCMODE_KNL, oz_crtl_fd_array[fd].h_iochan, oz_crtl_fd_array[fd].h_event, OZ_IO_FS_CLOSE, 0, NULL);
  if (!oz_crtl_fd_free (fd)) return (-1);	/* deassign channels and free off the fd for later use */
  if (sts == OZ_SUCCESS) return (0);
  errno = EOZSYSERR;
  errno_ozsts = sts;
  return (-1);
}

ALIASES (close)

int isatty (int fd)

{
  uLong sts;
  OZ_Handle h_iochan;

  if (!oz_crtl_fd_check (fd)) return (-1);					/* make sure fd is in range and is allocated */
  if (oz_crtl_fd_array[fd].isatty == 0) {
    h_iochan = oz_crtl_fd_array[fd].h_iochan;					/* ok, get the I/O channel handle */
    sts = oz_sys_io (OZ_PROCMODE_KNL, h_iochan, 0, OZ_IO_CONSOLE_GETMODE, 0, NULL);
    if (sts == OZ_SUCCESS) oz_crtl_fd_array[fd].isatty = 1;			/* if successful, set the isatty to 1 (will return 1) */
    else if (sts == OZ_BADIOFUNC) oz_crtl_fd_array[fd].isatty = 2;		/* if driver doesn't to it, set isatty to 2 (will return 0) */
    else goto ozsyserr;								/* otherwise, don't know what it's doing */
  }

  return (oz_crtl_fd_array[fd].isatty & 1);

ozsyserr:
  errno = EOZSYSERR;
  errno_ozsts = sts;
  return (-1);
}

char *ttyname (int fd)

{
  int rc;

  static char devname[OZ_DEVUNIT_NAMESIZE];

  rc = ttyname_r (fd, devname, sizeof devname);
  if (rc != 0) return (NULL);
  return (devname);
}

int ttyname_r (int fd, char *buf, size_t buflen)

{
  char devclass[OZ_DEVCLASS_NAMESIZE];
  uLong sts;
  OZ_Handle h_iochan;

  if (!oz_crtl_fd_check (fd)) return (-1);					/* make sure fd is in range and is allocated */
  h_iochan = oz_crtl_fd_array[fd].h_iochan;					/* ok, get the I/O channel handle */

  if (isatty (fd) <= 0) {							/* see if it is a tty */
    errno = ENOTTY;								/* if not, return error status */
    return (ENOTTY);
  }

  sts = oz_sys_iochan_getunitname (h_iochan, buflen, buf);			/* console, get unit name string */
  if (sts != OZ_SUCCESS) goto ozsyserr;

  return (0);									/* return success */

ozsyserr:
  errno = EOZSYSERR;
  errno_ozsts = sts;
  return (EOZSYSERR);
}

ALIAS (isatty)
ALIAS (ttyname)
ALIAS (ttyname_r)

/************************************************************************/
/*									*/
/*  Determine if file is accessible to the 'real' uid			*/
/*  Also determines if the file exists or not				*/
/*									*/
/************************************************************************/

int access (const char *pathname, int mode)

{
  uLong sts;
  OZ_Handle h_iochan;
  OZ_IO_fs_open fs_open;

  h_iochan = 0;

  memset (&fs_open, 0, sizeof fs_open);							/* open the file */
  fs_open.name = pathname;
  fs_open.lockmode = OZ_LOCKMODE_NL;
  sts = oz_sys_io_fs_open (sizeof fs_open, &fs_open, 0, &h_iochan);
  if (sts == OZ_NOSUCHFILE) goto error_nsf;
  if (sts != OZ_SUCCESS) goto error_sys;						/* abort processing if any error */
  oz_sys_handle_release (OZ_PROCMODE_KNL, h_iochan);					/* release allocated i/o channel */
  return (0);

error_nsf:
  errno = ENOENT;
  goto error;

error_sys:
  errno = EOZSYSERR;
error:
  errno_ozsts = sts;
  oz_sys_handle_release (OZ_PROCMODE_KNL, h_iochan);					/* release allocated i/o channel */

  return (-1);
}

time_t oz_crtl_tounixtime (OZ_Datebin oztime)

{
  uLong datelong[OZ_DATELONG_ELEMENTS];

  oz_sys_datebin_decode (oztime, datelong);							/* convert oztime to longword array */
  if (unixbasedays == 0) unixbasedays = oz_sys_daynumber_encode ((1970 << 16) | (1 << 8) | 1);
  datelong[OZ_DATELONG_DAYNUMBER] -= unixbasedays;						/* subtract off unix date base of 1-JAN-1970 */
												/* ... to get number of days since then */
  return (datelong[OZ_DATELONG_DAYNUMBER] * 86400 + datelong[OZ_DATELONG_SECOND]);		/* return resultant number of seconds since then */
}


int utime (const char *filename, const struct utimbuf *buf)

{
  return (0);
}

char *ctime (const time_t *timep)

{
  struct tm *tm;
  static char timestr[32];

  tm = localtime (timep);
  oz_sys_sprintf (sizeof timestr, timestr, "%s %s %2d %2.2d:%2.2d:%2.2d %4.4d\n", 
                  dow[tm->tm_wday], mon[tm->tm_mon], tm -> tm_mday, tm -> tm_hour, tm -> tm_min, tm -> tm_sec, tm -> tm_year);

  return (timestr);
}

int rename (const char *oldpath, const char *newpath)

{
  char np[256];
  uLong sts;

  sts = oz_sys_io_fs_parse (newpath, 0, parse_newpath, np);
  if (sts != OZ_SUCCESS) {
    return (-1);
  }
  sts = oz_sys_io_fs_parse (oldpath, 0, parse_oldpath, np);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printerror ("oz_crtl_io rename: error %u renaming %s to %s\n", oldpath, newpath);
    return (-1);
  }
  return (0);
}

static uLong parse_newpath (void *npv, const char *devname, const char *filename, OZ_Handle h_iochan)

{
  oz_sys_sprintf (256, npv, "%s:%s", devname, filename);
  return (OZ_SUCCESS);
}

static uLong parse_oldpath (void *npv, const char *devname, const char *filename, OZ_Handle h_iochan)

{
  char *np;
  int i;
  uLong sts;
  OZ_Handle h_dev;
  OZ_IO_fs_rename fs_rename;

  np = npv;
  i = strlen (devname);
  if ((np[i] != ':') || (memcmp (np, devname, i) != 0)) return (OZ_BADDEVUNIT);
  sts = oz_sys_io_assign (OZ_PROCMODE_KNL, &h_dev, devname, OZ_LOCKMODE_CW);
  if (sts == OZ_SUCCESS) {
    memset (&fs_rename, 0, sizeof fs_rename);
    fs_rename.oldname = filename;
    fs_rename.newname = np + i + 1;
    sts = oz_sys_io (OZ_PROCMODE_KNL, h_dev, 0, OZ_IO_FS_RENAME, sizeof fs_rename, &fs_rename);
    oz_sys_handle_release (OZ_PROCMODE_KNL, h_dev);
  }
  return (sts);
}
