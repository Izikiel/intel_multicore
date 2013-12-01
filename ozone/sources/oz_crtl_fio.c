//+++2002-08-19
//    Copyright (C) 2001,2002 Mike Rieker, Beverly, MA USA
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
//---2002-08-19

/************************************************************************/
/*									*/
/*  Standard C-style file I/O routines					*/
/*									*/
/************************************************************************/

#define _OZ_CRTL_FIO_C

#include "ozone.h"
#include "oz_crtl_fio.h"
#include "oz_crtl_malloc.h"
#include "oz_knl_status.h"
#include "oz_sys_io_fs_printf.h"
#include "oz_sys_xprintf.h"
#include "oz_sys_xscanf.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct FILE { FILE *next;		/* next in allfiles list */
              FILE **prev;		/* previous in list */
              int fd;			/* fd the file is open on */
              int flags;		/* flags it was opened with */
              int error;		/* an I/O error has occurred */
              int eof;			/* it has hit eof on input */
              int mybuf;		/* I malloc'd the buff */
              int isconsole;		/* it is a console device */
              int bufmode;		/* buffering mode (none, full, line) */
              long buffpos;		/* file position at beg of buff */
              long filepos;		/* current file position */
					/* buffer contents modified ... */
              int dirty_low;		/* ... starting here (inclusive) */
              int dirty_high;		/* ... ending here (exclusive) */
              int valid;		/* number of chars valid in buffer */
              int offset;		/* offset to next char to process */
              int size;			/* size of buffer */
              char *buff;		/* address of buffer */
            };

FILE *stdin  = NULL;
FILE *stdout = NULL;
FILE *stderr = NULL;

FILE *allfiles = NULL;

static uLong streamvsprintf (void *streamv, uLong *size_r, char **buff_r);
static uLong streamvasprintf (void *streamv, uLong *size_r, char **buff_r);
static uLong streamvfprintf (void *streamv, uLong *size_r, char **buff_r);
static uLong streamvsscanf (void *pntpv, uLong *size_r, const char **buff_r);
static uLong streamvfscanf (void *streamv, uLong *size_r, const char **buff_r);
static int fputcnf (int c, FILE *stream);
static int modetoflags (const char *mode);
static long getfileof (FILE *stream);
static int fillinbuf (FILE *stream);
static int flushmode (FILE *stream);
static int flushoutbuf (FILE *stream, int alloc);

/************************************************************************/
/*									*/
/*  Open/Close routines							*/
/*									*/
/************************************************************************/

FILE *fopen (const char *path, const char *mode)

{
  FILE *stream;
  int fd, flags;

  flags = modetoflags (mode);
  if (flags < 0) return (NULL);
  fd = open (path, flags);
  if (fd < 0) return (NULL);
  stream = fdopen (fd, mode);
  if (strchr (mode, 'a') != NULL) stream -> buffpos = getfileof (stream);
  return (stream);
}

FILE *fdopen (int fildes, const char *mode)

{
  FILE *stream;

  stream = malloc (sizeof *stream);				/* allocate new stream block */
  memset (stream, 0, sizeof *stream);				/* by default, everything is zero */
  stream -> fd        = fildes;					/* save the fd opened on it */
  stream -> isconsole = isatty (fildes);			/* save whether or not it is a console */
  stream -> flags     = modetoflags (mode);			/* save the open flags */
  stream -> bufmode   = _IOFBF;					/* by default, full buffering */
  if (stream -> isconsole) stream -> bufmode = _IOLBF;		/* but consoles are line buffering */
  if (strchr (mode, 'a') != NULL) stream -> buffpos = getfileof (stream); /* maybe start at end of file */
  stream -> next      = allfiles;				/* link it to the 'allfiles' list */
  stream -> prev      = &allfiles;
  if (allfiles != NULL) allfiles -> prev = &(stream -> next);
  allfiles = stream;
  return (stream);						/* return stream pointer */
}

FILE *freopen (const char *path, const char *mode, FILE *stream)

{
  int fd, flags;

  if (fflush (stream) < 0) return (NULL);			/* flush the previous stream */
  close (stream -> fd);						/* close the file */
  flags = modetoflags (mode);					/* set up new flags */
  if (flags < 0) return (NULL);
  fd = open (path, flags);					/* open new file */
  if (fd < 0) return (NULL);
  stream -> fd = fd;						/* save new fd */
  stream -> flags = flags;					/* save new flags */
  stream -> isconsole = isatty (fd);				/* save new console flag */
  stream -> buffpos = 0;					/* assume start at beginning of file */
  if (strchr (mode, 'a') != NULL) stream -> buffpos = getfileof (stream); /* maybe start at end of file */
  return (stream);
}

int fclose (FILE *stream)

{
  int rc;

  rc = flushoutbuf (stream, 0);			/* flush out any unwritten data */
  close (stream -> fd);				/* close the file */
  if (stream -> mybuf) {
    free (stream -> buff);			/* free off the block buffer */
  }
  *(stream -> prev) = stream -> next;		/* remove from allfiles list */
  if (stream -> next != NULL) stream -> next -> prev = stream -> prev;
  free (stream);				/* free off the stream memory */
  return (rc ? 0 : EOF);			/* return flush status */
}

/************************************************************************/
/*									*/
/*  Binary read/write routines						*/
/*									*/
/************************************************************************/

int fread (void *ptr, size_t size, size_t nmemb, FILE *stream)

{
  int i, rc;

  for (i = 0; i < size * nmemb; i ++) {
    rc = fgetc (stream);
    if (rc == EOF) break;
    *((char *)(ptr ++)) = rc;
  }
  return (i / size);
}

int fwrite (const void *ptr, size_t size, size_t nmemb, FILE *stream)

{
  const char *p;
  int rc;
  size_t i, j;


  if (size == 0) {
    return (0);
  }

  p = ptr;
  for (i = j = size * nmemb; i > 0; -- i) {
    rc = fputcnf (*(p ++), stream);
    if (rc == EOF) break;
  }
  if (!flushmode (stream)) {
    return (EOF);
  }
  return ((j - i) / size);
}

/************************************************************************/
/*									*/
/*  Formatted output routines						*/
/*									*/
/************************************************************************/

int sprintf (char *str, const char *format, ...)

{
  va_list ap;
  int rc;

  va_start (ap, format);
  rc = vsprintf (str, format, ap);
  va_end (ap);
  return (rc);
}

int vsprintf (char *str, const char *format, va_list ap)

{
  uLong rlen, sts;

  sts = oz_sys_vxprintf (streamvsprintf, 
                         NULL, 
                         0x7FFFFFFF, 
                         str, 
                         &rlen, 
                         format, 
                         ap);
  if (sts != OZ_SUCCESS) return (-1);
  str[rlen] = 0;
  return (rlen);
}

int asprintf (char **buf, const char *format, ...)

{
  va_list ap;
  int rc;

  va_start (ap, format);
  rc = vasprintf (buf, format, ap);
  va_end (ap);
  return (rc);
}

int vasprintf (char **buf, const char *format, va_list ap)

{
  char *str;
  uLong rlen, sts;

  str = malloc (256);				// start with 256 bytes
  sts = oz_sys_vxprintf (streamvasprintf, 	// format string, realloc'ing str as needed
                         &str, 
                         256, 
                         str, 
                         &rlen, 
                         format, 
                         ap);
  if (sts != OZ_SUCCESS) {			// check for error
    free (str);					// if so, free off allocated memory
    *buf = NULL;				// return null pointer
    return (-1);				// and return error status
  }
  str[rlen] = 0;				// ok, null terminate string
  *buf = str;					// return pointer to string
  return (rlen);				// return length of string
}

int printf (const char *format, ...)

{
  va_list ap;
  int rc;

  va_start (ap, format);
  rc = vfprintf (stdout, format, ap);
  va_end (ap);
  return (rc);
}

int vprintf (const char *format, va_list ap)

{
  int rc;

  rc = vfprintf (stdout, format, ap);
  return (rc);
}

int fprintf (FILE *stream, const char *format, ...)

{
  va_list ap;
  int rc;

  va_start (ap, format);
  rc = vfprintf (stream, format, ap);
  va_end (ap);
  return (rc);
}

int vfprintf (FILE *stream, const char *format, va_list ap)

{
  uLong rlen, sts;

  sts = oz_sys_vxprintf (streamvfprintf, 
                         stream, 
                         stream -> size - stream -> offset, 
                         stream -> buff + stream -> offset, 
                         &rlen, 
                         format, 
                         ap);

  if (sts != OZ_SUCCESS) return (-1);
  if (!flushmode (stream)) return (-1);
  return (rlen);
}

/* Flush and get more space for memory based routines */

static uLong streamvsprintf (void *streamv, uLong *size_r, char **buff_r)

{
  *buff_r += *size_r;
  *size_r  = 0x7FFFFFFF;
  return (OZ_SUCCESS);
}

/* Flush and get more space for malloc based routines */

static uLong streamvasprintf (void *bufv, uLong *size_r, char **buff_r)

{
  char *newbuf, *oldbuf;
  int newlen, oldlen;

  oldbuf  = *(char **)bufv;			// get address at beginning of old buffer
  oldlen  = *buff_r + *size_r - oldbuf;		// get total length of existing string

  newlen  = oldlen + 256;			// add 256 bytes to malloc buffer
  newbuf  = realloc (oldbuf, newlen);		// realloc the buffer to the new length

  *size_r = 256;				// we now have 256 bytes of space
  *buff_r = newbuf + oldlen;			// ... on the end of the new buffer

  return (OZ_SUCCESS);
}

/* Flush and get more space for file based routines */

static uLong streamvfprintf (void *streamv, uLong *size_r, char **buff_r)

{
  FILE *stream;

  stream = streamv;

  /* Make sure the dirty_low mark includes the beginning of what was written */

  if (stream -> dirty_low == stream -> dirty_high) {
    stream -> dirty_low = stream -> dirty_high = stream -> offset;
  }
  if (stream -> offset < stream -> dirty_low) {
    stream -> dirty_low = stream -> offset;
  }

  /* Update offset to point to end of what was written */

  stream -> offset = *buff_r + *size_r - stream -> buff;

  /* Make sure the dirty_high mark includes the end of what was written */

  if (stream -> offset > stream -> dirty_high) {
    stream -> dirty_high = stream -> offset;
  }

  /* Maybe extend buffer to include the end of what was written */

  if (stream -> valid < stream -> offset) stream -> valid = stream -> offset;

  /* If block buffer is full, write it out to file */

  if (stream -> offset == stream -> size) {
    if (!flushoutbuf (stream, 1)) return (OZ_ENDOFFILE);
  }

  /* Anyway, return size and address of what's left of the block buffer */

  *size_r = stream -> size - stream -> offset;
  *buff_r = stream -> buff + stream -> offset;

  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Formatted input routines						*/
/*									*/
/************************************************************************/

int sscanf (const char *str, const char *format, ...)

{
  va_list ap;
  int rc;

  va_start (ap, format);
  rc = vsscanf (str, format, ap);
  va_end (ap);
  return (rc);
}

int vsscanf (const char *str, const char *format, va_list ap)

{
  uLong nargs, sts;

  sts = oz_sys_vxscanf (streamvsscanf, &str, &nargs, NULL, format, ap);	/* scan the buffer */
  if ((nargs == 0) && (sts != OZ_SUCCESS)) return (EOF);		/* return 'EOF' if nothing converted and input error */
  return (nargs);							/* else, return number of conversions performed */
}

int scanf (const char *format, ...)

{
  va_list ap;
  int rc;

  va_start (ap, format);
  rc = vfscanf (stdin, format, ap);
  va_end (ap);
  return (rc);
}

int fscanf (FILE *file, const char *format, ...)

{
  va_list ap;
  int rc;

  va_start (ap, format);
  rc = vfscanf (file, format, ap);
  va_end (ap);
  return (rc);
}

int vscanf (const char *format, va_list ap)

{
  return (vfscanf (stdin, format, ap));
}

int vfscanf (FILE *stream, const char *format, va_list ap)

{
  uLong nargs, sts, uncnv;

  sts = oz_sys_vxscanf (streamvfscanf, stream, &nargs, &uncnv, format, ap);	/* scan and convert from file */
  stream -> offset -= uncnv;							/* restore the unconverted chars from buffer */
  if ((nargs == 0) && (sts != OZ_SUCCESS)) return (EOF);			/* return EOF if nothing converted and error */
  return (nargs);								/* otherwise, return number of conversions */
}

/* This routine gets called by oz_sys_vxscanf to retrieve more input data for memory based routines */

static uLong streamvsscanf (void *pntpv, uLong *size_r, const char **buff_r)

{
  const char **strp;

  strp = pntpv;					/* param is pointer to input string pointer */

  if (*strp == NULL) return (OZ_ENDOFFILE);	/* return 'end of file' if input string already used up */
  *size_r = strlen (*strp);			/* return length of input string */
  *buff_r = *strp;				/* return pointer to string */
  *strp = NULL;					/* clear pointer so we will return eof next time */
  return (OZ_SUCCESS);				/* successful */
}

/* This routine gets called by oz_sys_vxscanf to retrieve more input data for file based routines */

static uLong streamvfscanf (void *streamv, uLong *size_r, const char **buff_r)

{
  FILE *stream;

  stream = streamv;
  if (stream -> offset == stream -> valid) {		/* see if buffer all used up */
    if (!fillinbuf (stream)) return (OZ_ENDOFFILE);	/* if so, get a new one */
  }
  *size_r = stream -> valid - stream -> offset;		/* return size of everything left in buffer */
  *buff_r = stream -> buff + stream -> offset;		/* return address of everything left in buffer */
  stream -> offset = stream -> valid;			/* assume the scan will use it all, fix later if not */
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Character-at-a-time input/output routines				*/
/*									*/
/************************************************************************/

char *gets (char *s)

{
  char *p;
  int rc;

  p = s;
  while (1) {					/* repeat forever */
    rc = fgetc (stdin);				/* try to get a character from stdin */
    if ((rc == EOF) || (rc == '\n')) break;	/* stop if EOF or newline */
    *(p ++) = rc;				/* store in buffer */
  }
  *p = 0;					/* done, null terminate */
  return ((rc == EOF) ? NULL : s);		/* return NULL if error, else original pointer */
}

char *fgets (char *s, int size, FILE *stream)

{
  int i, rc;

  for (i = 0; i < size - 1;) {			/* loop until buffer filled (except leave room for null) */
    rc = fgetc (stream);			/* get a character */
    if (rc == EOF) break;			/* stop if end of file */
    s[i++] = rc;				/* store the char in buffer */
    if (rc == '\n') break;			/* stop if newline */
  }
  if (i == 0) return (NULL);			/* if nothing retrieved, it is end-of-file */
  s[i] = 0;					/* otherwise, null terminate */
  return (s);					/* return beginning of buffer */
}

int fgetc (FILE *stream)

{
  FILE *f;

  /* If reading from a console, flush all console streams */

  if (stream -> isconsole) {
    for (f = allfiles; f != NULL; f = f -> next) {
      if (f -> isconsole) flushoutbuf (f, 0);
    }
  }

  /* If buffer is empty, get a new one */

  if (stream -> offset == stream -> valid) {
    if (!fillinbuf (stream)) return (EOF);
  }

  /* Return a character in buffer */

  return (((unsigned char *)(stream -> buff))[stream -> offset ++]);
}

int ungetc (int c, FILE *stream)

{
  if (stream -> offset == 0) return (EOF);	/* can only do it if there is room */
  stream -> buff[--(stream->offset)] = c;	/* ok, store char back in buffer */
  return (c);					/* return character */
}

int puts (const char *s)

{
  char c;
  int rc;

  while ((c = *(s ++)) != 0) {			/* get a char from buffer until we hit a null */
    rc = fputcnf (c, stdout);			/* write to stdout but don't flush buffer */
    if (rc == EOF) return (EOF);		/* return EOF if we get error */
  }
  rc = fputc ('\n', stdout);			/* ok, put in a newline and possibly flush */
  return (rc);					/* return final status */
}

int fputs (const char *s, FILE *stream)

{
  char c;
  int rc;

  while ((c = *(s ++)) != 0) {			/* get a char from buffer until we hit a null */
    rc = fputcnf (c, stream);			/* write to file but don't flush buffer */
    if (rc == EOF) return (EOF);		/* return EOF if we get error */
  }
  if (!flushmode (stream)) return (EOF);	/* possibly flush buffer */
  return (0);					/* return success status */
}

int fputc (int c, FILE *stream)

{
  int rc;

  rc = fputcnf (c, stream);			/* output the char without flushing */
  if (rc != EOF) {
    if (!flushmode (stream)) return (EOF);	/* now possibly flush buffer */
  }
  return (rc);
}

/* Write character to output stream but don't flush */

static int fputcnf (int c, FILE *stream)

{
  if (stream -> offset == stream -> size) {				/* flush buffer if it is completely full */
    if (!flushoutbuf (stream, 1)) return (EOF);
  }
  if (stream -> dirty_low == stream -> dirty_high) {			/* init dirty_low/dirty_high if they do not indicate any range of characters */
    stream -> dirty_low = stream -> dirty_high = stream -> offset;	/* ... make them point at current offset */
  }
  if (stream -> offset < stream -> dirty_low) {				/* if current offset < dirty_low, lower dirty_low to encompass offset */
    stream -> dirty_low = stream -> offset;
  }
  stream -> buff[stream -> offset ++] = c;				/* store character at [offset ++] */
  if (stream -> offset > stream -> dirty_high) {			/* make sure dirty_high encompasses new offset */
    stream -> dirty_high = stream -> offset;
  }
  if (stream -> offset > stream -> valid) {				/* make sure valid encompasses new offset */
    stream -> valid = stream -> offset;
  }

  return (c & 0xff);
}

/************************************************************************/
/*									*/
/*  Seeks								*/
/*									*/
/************************************************************************/

int fgetpos (FILE *stream, fpos_t *pos)

{
  *pos = ftell (stream);
  return (0);
}

int fsetpos (FILE *stream, fpos_t *pos)

{
  return (fseek (stream, *pos, SEEK_SET));
}

void rewind (FILE *stream)

{
  fseek (stream, 0, SEEK_SET);
}

long ftell (FILE *stream)

{
  return (stream -> buffpos + stream -> offset);
}

int fseek (FILE *stream, long offset, int whence)

{
  switch (whence) {
    case SEEK_SET: {
      break;
    }
    case SEEK_CUR: {
      offset += stream -> buffpos + stream -> offset;
      break;
    }
    case SEEK_END: {
      offset += getfileof (stream);
      break;
    }
    default: {
      errno = EINVAL;
      return (-1);
    }
  }

  /* If offset is within the buffer, just point to the byte */

  if ((offset >= stream -> buffpos) && (offset < stream -> buffpos + stream -> valid)) {
    stream -> offset = offset - stream -> buffpos;
    return (0);
  }

  /* Otherwise, flush current buffer and establish new position */
  /* A subsequent read or write will position the file          */

  if (!flushoutbuf (stream, 1)) return (-1);
  stream -> valid   = 0;
  stream -> offset  = 0;
  stream -> buffpos = offset;
  return (0);
}

/************************************************************************/
/*									*/
/*  Buffering								*/
/*									*/
/************************************************************************/

int fflush (FILE *stream)

{
  return (flushoutbuf (stream, 0) ? 0 : EOF);
}

int fpurge (FILE *stream)

{
  stream -> offset     = 0;
  stream -> valid      = 0;
  stream -> dirty_low  = 0;
  stream -> dirty_high = 0;
  return (0);
}

int setvbuf (FILE *stream, char *buf, int mode, size_t size)

{
  /* Flush out whatever was going on before */

  if (!flushoutbuf (stream, 0)) return (EOF);

  /* If I malloc'd a buffer, free it off */

  if (stream -> mybuf) {
    free (stream -> buff);
    stream -> mybuf = 0;
  }

  /* Don't have a buffer now (either my old one or user's old one) */

  stream -> buff = NULL;
  stream -> size = 0;

  /* Now see what user wants to do */

  stream -> bufmode = mode;
  switch (mode) {

    /* Non-buffered, we malloc a buffer internally later */

    case _IONBF: {
      break;
    }

    /* Line or full buffering, use user's buffer */

    case _IOLBF:
    case _IOFBF: {
      if (buf != NULL) {
        stream -> size  = size;
        stream -> buff  = buf;
      }
      break;
    }
    default: {
      errno = EINVAL;
      return (-1);
    }
  }

  return (0);
}

void clearerr (FILE *stream)

{
  stream -> error = 0;
  stream -> eof   = 0;
}

int feof (FILE *stream)

{
  return (stream -> eof);
}

int ferror (FILE *stream)

{
  return (stream -> error);
}

int fileno (FILE *stream)

{
  return (stream -> fd);
}

/************************************************************************/
/*									*/
/*  Convert a fopen mode parameter string to flags for the open call	*/
/*									*/
/************************************************************************/

#define MASK_A 1
#define MASK_P 2
#define MASK_R 4
#define MASK_W 8

static const int flags[16] = { /*             ____              ___a                    __+_            __+a */
                                                -1, O_CREAT|O_WRONLY,                     -1, O_CREAT|O_RDWR, 

                               /*             _r__              _r_a                    _r+_           _r+a */
                                          O_RDONLY,               -1,                 O_RDWR,             -1, 

                               /*             w___              w__a                    w_+_            w_+a */
                          O_CREAT|O_WRONLY|O_TRUNC,               -1, O_CREAT|O_RDWR|O_TRUNC,             -1, 

                               /*             wr__              wr_a                    wr+_            wr+a */
                                                -1,               -1,                     -1,             -1 };

static int modetoflags (const char *mode)

{
  int flag, i, mask;

  mask = 0;
  for (i = 0;; i ++) {
    switch (mode[i]) {
      case 0: {
        flag = flags[mask];
        if (flag == -1) {
          errno = EINVAL;
          return (-1);
        }
        return (flag);
      }
      case 'a': {
        mask |= MASK_A;
        break;
      }
      case 'b': {
        break;
      }
      case '+': {
        mask |= MASK_P;
        break;
      }
      case 'r': {
        mask |= MASK_R;
        break;
      }
      case 'w': {
        mask |= MASK_W;
        break;
      }
      default: {
        errno = EINVAL;
        return (-1);
      }
    }
  }
}

/************************************************************************/
/*									*/
/*  Get end-of-file position						*/
/*									*/
/************************************************************************/

static long getfileof (FILE *stream)

{
  struct stat statbuf;

  if (fstat (stream -> fd, &statbuf) < 0) return (-1);
  return (statbuf.st_size);
}

/************************************************************************/
/*									*/
/*  Read as much as we can from the file into the buffer		*/
/*									*/
/************************************************************************/

static int fillinbuf (FILE *stream)

{
  int rc;

  /* If there is modified data in the buffer, write it out to the file */

  if (!flushoutbuf (stream, 1)) return (0);

  /* Position file to buffer's point */

  if (stream -> filepos != stream -> buffpos) {
    rc = lseek (stream -> fd, stream -> buffpos, SEEK_SET);
    if (rc < 0) {
      stream -> error = 1;
      return (0);
    }
    stream -> filepos = stream -> buffpos;
  }

  /* Now read something from the file */

  rc = read (stream -> fd, stream -> buff, stream -> size);
  if (rc < 0) {
    stream -> error = 1;
    return (0);
  }
  if (rc == 0) {
    stream -> eof = 1;
    return (0);
  }

  /* Increment file's position by number of characters read */

  stream -> filepos += rc;

  /* Save the size of what's valid in the buffer */

  stream -> valid = rc;
  return (1);
}

/************************************************************************/
/*									*/
/*  Flush output stream depending on buffer mode			*/
/*									*/
/************************************************************************/

static int flushmode (FILE *stream)

{
  int i;

  switch (stream -> bufmode) {

    /* No buffering, flush it immediately */

    case _IONBF: return (flushoutbuf (stream, 0));

    /* Line buffering, flush it if there is a newline in it somewhere */

    case _IOLBF: {
      for (i = stream -> dirty_low; i < stream -> dirty_high; i ++) {
        if (stream -> buff[i] == '\n') {
          return (flushoutbuf (stream, 0));
        }
      }
    }

    /* Full buffering, let it flush when buffer fills */
  }

  return (1);
}

/************************************************************************/
/*									*/
/*  Write out any modified data in the buffer to file and empty it	*/
/*									*/
/************************************************************************/

static int flushoutbuf (FILE *stream, int alloc)

{
  int ofs, rc;

  /* Make sure we even have a buffer */

  if (stream -> buff == NULL) {
    if (alloc) {
      stream -> size  = BUFSIZ;
      stream -> buff  = malloc (BUFSIZ);
      stream -> mybuf = 1;
    }
    return (1);
  }

  /* See if buffer contents were modified */

  if (stream -> dirty_low != stream -> dirty_high) {

    /* If so, make sure file is positioned within the buffer */
    /* and at or before first modified byte in the buffer    */

    if ((stream -> filepos < stream -> buffpos) 
     || (stream -> filepos > stream -> buffpos + stream -> dirty_low)) {
      rc = lseek (stream -> fd, stream -> buffpos + stream -> dirty_low, SEEK_SET);
      if (rc < 0) {
        stream -> error = 1;
        return (0);
      }
      stream -> filepos = stream -> buffpos + stream -> dirty_low;
    }

    /* Write the modified data out to the file */

    for (ofs = stream -> filepos - stream -> buffpos; ofs < stream -> dirty_high; ofs += rc) {
      rc = write (stream -> fd, stream -> buff + ofs, stream -> dirty_high - ofs);
      if (rc < 0) {
        stream -> error = 1;
        return (0);
      }
      if (rc == 0) {
        stream -> eof = 1;
        return (0);
      }
      stream -> filepos += rc;
    }

    /* Nothing dirty in there now */

    stream -> dirty_low  = 0;
    stream -> dirty_high = 0;
  }

  /* Make sure there is some space in there for new stuff */

  if (stream -> offset == stream -> valid) {
    stream -> buffpos += stream -> valid;	/* the next buffer directly follows the last one in the file */
    stream -> offset   = 0;			/* wipe buffer pointers out */
    stream -> valid    = 0;
  }

  return (1);
}
