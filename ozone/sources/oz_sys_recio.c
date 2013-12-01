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
/*  Filesystem driver record i/o library				*/
/*									*/
/************************************************************************/

#define _OZ_RECIO_C

#include "ozone.h"
#include "oz_io_fs.h"
#include "oz_knl_hw.h"
#include "oz_knl_status.h"
#include "oz_sys_recio.h"

#define RECIO_BUFF_SIZE 4096

/* Block buffers */

typedef struct OZ_Recio_buf { struct OZ_Recio_buf *next, **prev;
                              OZ_Dbn buf_vbn;		/* starting vbn of data, always = 1 + exact multiple of blockfact */
                              uLong buf_len;		/* length of data that is valid */
                              int buf_dirty;		/* 1 = block needs to be written to disk */
                              Long refcount;		/* number of cur_buf's that reference this */
                              uByte buf_data[1];	/* the block's data (actually recbuffsize bytes long) */
                            } OZ_Recio_buf;

/* Per accessor context block */

struct OZ_Recio_chnex { void *chnex;			/* caller's context pointer */
                        uLong cur_ofs;			/* current offset */
                        OZ_Dbn cur_vbn;			/* current block number (1 = the first block, 0 is invalid) */
                        OZ_Recio_buf *cur_buf;		/* current buffer, NULL if none */
                      };

/* Per file context block */

struct OZ_Recio_filex { void *filex;			/* caller's context pointer */
                        OZ_Recio_call *call;		/* callback table */
                        OZ_Recio_buf *bufs;		/* list of blocks we have in memory */
                        uLong diskblksize;		/* disk block size (in bytes) */
                        uLong recbuffsize;		/* record buffer size (in bytes) */
                        uLong blockfact;		/* recbuffsize / diskblksize */
                        OZ_Dbn efblk;			/* end-of-file block number (always normalized to diskblksize) */
                        uLong efbyt;			/* end-of-file byte number (always normalized to diskblksize) */
                      };

static uLong putrec (OZ_Recio_chnex *recio_chnex, OZ_Recio_filex *recio_filex, uLong size, const uByte *buff, uLong *wlen);
static uLong fillrec (OZ_Recio_chnex *recio_chnex, OZ_Recio_filex *recio_filex, uLong overwrite);
static uLong flushbuf (OZ_Recio_chnex *recio_chnex, OZ_Recio_filex *recio_filex);

/************************************************************************/
/*									*/
/*  Initialze file-global structure					*/
/*									*/
/************************************************************************/

OZ_Recio_filex *oz_sys_recio_initfilex (void *filex, OZ_Recio_call *call, uLong diskblksize, OZ_Dbn efblk, uLong efbyt)

{
  OZ_Recio_filex *recio_filex;

  recio_filex = (*(call -> malloc)) (filex, NULL, sizeof *recio_filex, __FILE__, __LINE__);

  recio_filex -> filex = filex;							/* save caller's file context pointer */
  recio_filex -> call  = call;							/* save callback table pointer */
  recio_filex -> bufs  = NULL;							/* we don't have any buffers for this file yet */
  recio_filex -> diskblksize = diskblksize; 					/* save disk block size (in bytes) */
  recio_filex -> blockfact   = (RECIO_BUFF_SIZE + diskblksize - 1) / diskblksize; /* get blocking factor, ie, number of disk blocks per record buffer */
  recio_filex -> recbuffsize = diskblksize * recio_filex -> blockfact; 		/* get record buffer size = exact multiple of diskblksize and >= RECIO_BUFF_SIZE */
  efblk += efbyt / diskblksize;							/* make sure eof stuff is normalized to disk block size */
  efbyt %= diskblksize;
  recio_filex -> efblk = efblk;							/* save end-of-file block number */
  recio_filex -> efbyt = efbyt;							/* save end-of-file byte number */

  return (recio_filex);
}

/************************************************************************/
/*									*/
/*  Terminate file-global structure					*/
/*									*/
/************************************************************************/

void oz_sys_recio_termfilex (OZ_Recio_filex *recio_filex, OZ_Dbn *efblk_r, uLong *efbyt_r)

{
  if (recio_filex -> bufs != NULL) oz_crash ("oz_recio_termfilex: buffers left on list");
  if (efblk_r != NULL) *efblk_r = recio_filex -> efblk;
  if (efbyt_r != NULL) *efbyt_r = recio_filex -> efbyt;
  (*(recio_filex -> call -> free)) (recio_filex -> filex, NULL, recio_filex);
}

/************************************************************************/
/*									*/
/*  Initialize channel-specific structure				*/
/*									*/
/************************************************************************/

OZ_Recio_chnex *oz_sys_recio_initchnex (OZ_Recio_filex *recio_filex, void *chnex)

{
  OZ_Recio_chnex *recio_chnex;

  recio_chnex = (*(recio_filex -> call -> malloc)) (recio_filex -> filex, chnex, sizeof *recio_chnex, __FILE__, __LINE__);

  recio_chnex -> chnex = chnex;
  recio_chnex -> cur_ofs = 0;
  recio_chnex -> cur_vbn = 1;
  recio_chnex -> cur_buf = NULL;

  return (recio_chnex);
}

/************************************************************************/
/*									*/
/*  Terminate channel-specific structure				*/
/*									*/
/************************************************************************/

void oz_sys_recio_termchnex (OZ_Recio_chnex *recio_chnex, OZ_Recio_filex *recio_filex)

{
  flushbuf (recio_chnex, recio_filex);
  (*(recio_filex -> call -> free)) (recio_filex -> filex, recio_chnex -> chnex, recio_chnex);
}

/************************************************************************/
/*									*/
/*  Get current position block and byte					*/
/*									*/
/************************************************************************/

void oz_sys_recio_getcurrent (OZ_Recio_chnex *recio_chnex, OZ_Recio_filex *recio_filex, OZ_Dbn *curblk_r, uLong *curbyt_r)

{
  *curblk_r = recio_chnex -> cur_vbn;
  *curbyt_r = recio_chnex -> cur_ofs;
}

/************************************************************************/
/*									*/
/*  Process writerec function						*/
/*									*/
/*    Input:								*/
/*									*/
/*	writerec = write record i/o parameter block			*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_recio_write = OZ_SUCCESS : successful			*/
/*	                       else : error status			*/
/*									*/
/************************************************************************/

uLong oz_sys_recio_write (OZ_Recio_chnex *recio_chnex, OZ_Recio_filex *recio_filex, OZ_IO_fs_writerec *writerec)

{
  uLong atbyt, efbyt, ewbyt, sts, wlen;
  OZ_Dbn atblk, efblk, ewblk;

  /* If append mode, position to the eof point */

  atblk = writerec -> atblock;
  atbyt = writerec -> atbyte;
  if (writerec -> append) {
    atblk = recio_filex -> efblk;
    atbyt = recio_filex -> efbyt;
  }

  /* If explicit position given, position there */

  if (atblk != 0) {
    recio_chnex -> cur_vbn = atblk;
    recio_chnex -> cur_ofs = atbyt;
  }

  /* Extend the file if necessary to accomodate the record to be written */

  ewblk  = recio_chnex -> cur_vbn;					/* calculate where the end-of-write will be */
  ewbyt  = recio_chnex -> cur_ofs + writerec -> size + writerec -> trmsize;
  ewblk += ewbyt / recio_filex -> diskblksize;				/* normalize to disk block size for extend routine */
  ewbyt %= recio_filex -> diskblksize;
  if (ewbyt == 0) ewblk --;
  ewblk  = (((ewblk + recio_filex -> blockfact - 2) / recio_filex -> blockfact) * recio_filex -> blockfact) + 1; /* round up to blockfact boundary */

  sts = (*(recio_filex -> call -> extend)) (recio_chnex -> chnex, recio_filex -> filex, ewblk);
  if (sts != OZ_SUCCESS) return (sts);

  /* Copy record followed by terminator to block buffer */

  wlen = 0;
  sts = putrec (recio_chnex, recio_filex, writerec -> size, writerec -> buff, &wlen);
  if (sts == OZ_SUCCESS) sts = putrec (recio_chnex, recio_filex, writerec -> trmsize, writerec -> trmbuff, NULL);

  /* Return the actual length written */

  if (writerec -> wlen != NULL) *(writerec -> wlen) = wlen;

  /* Set the new eof position if truncate mode or if end-of-write went past current end-of-file */

  efblk  = recio_filex -> efblk;
  efbyt  = recio_filex -> efbyt;
  ewblk  = recio_chnex -> cur_vbn;
  ewbyt  = recio_chnex -> cur_ofs;
  ewblk += ewbyt / recio_filex -> diskblksize;
  ewbyt %= recio_filex -> diskblksize;

  if (writerec -> truncate || (ewblk > efblk) || ((ewblk == efblk) && (ewbyt > efbyt))) {
    recio_filex -> efblk = ewblk;
    recio_filex -> efbyt = ewbyt;
    (*(recio_filex -> call -> seteof)) (recio_chnex -> chnex, recio_filex -> filex, ewblk, ewbyt);
  }

  return (sts);
}

/************************************************************************/
/*									*/
/*  Process readrec function						*/
/*									*/
/*    Input:								*/
/*									*/
/*	readrec = read record i/o parameter block			*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_recio_read = OZ_SUCCESS : successful				*/
/*	                      else : error status			*/
/*									*/
/************************************************************************/

uLong oz_sys_recio_read (OZ_Recio_chnex *recio_chnex, OZ_Recio_filex *recio_filex, OZ_IO_fs_readrec *readrec)

{
  uByte *buff, *p, term_firstbyte;
  const uByte *trmbuff;
  int term_given;
  uLong comp, copy, room, saveofs, size, sts, trmsize;
  OZ_Dbn savevbn;

  /* If explicit position given, position there */

  if (readrec -> atblock != 0) {
    recio_chnex -> cur_vbn = readrec -> atblock;
    recio_chnex -> cur_ofs = readrec -> atbyte;
  }

  /* Get bytes from record buffer until we fill user's buffer or we see the terminator */

  term_given = (readrec -> trmsize != 0);					/* set this flag if we were given a terminator */
  if (term_given) term_firstbyte = ((uByte *)(readrec -> trmbuff))[0];		/* get first byte of terminator */

  size = readrec -> size;							/* get total size of user's read buffer */
  buff = readrec -> buff;							/* get address of user's read buffer  */

  while (size != 0) {

    /* Make sure we have some data to work with in the record buffer */

    sts = fillrec (recio_chnex, recio_filex, 0);				/* read in block if needed */
    if (sts != OZ_SUCCESS) goto rtn_oz;						/* abort if read error, including eof */
    room = recio_chnex -> cur_buf -> buf_len - recio_chnex -> cur_ofs;

    /* If the first byte is the first byte of the terminator, see if the following string is the terminator string */

    if (term_given && (recio_chnex -> cur_buf -> buf_data[recio_chnex->cur_ofs] == term_firstbyte)) {
      trmsize = readrec -> trmsize;						/* get user's terminator string size */
      trmbuff = readrec -> trmbuff;						/* get user's terminator string address */
      savevbn = recio_chnex -> cur_vbn;						/* save our current block number */
      saveofs = recio_chnex -> cur_ofs;						/* save our offset in the block */
      while (trmsize != 0) {							/* repeat while more comparing to do */
        sts = fillrec (recio_chnex, recio_filex, 0);				/* make sure we have something to compare to */
        if (sts != OZ_SUCCESS) goto not_terminator;				/* abort if read error, including eof */
        comp = recio_chnex -> cur_buf -> buf_len - recio_chnex -> cur_ofs;	/* see how much is in buffer to compare */
        if (comp > trmsize) comp = trmsize;					/* see how many bytes we can compare */
        sts = OZ_ENDOFFILE;							/* this status means compare mismatched */
        if (memcmp (trmbuff, recio_chnex -> cur_buf -> buf_data + recio_chnex -> cur_ofs, comp) != 0) goto not_terminator; /* if mismatch, we didn't compare */
        trmsize -= comp;							/* strings equal so far, inc terminator and record pointers */
        trmbuff += comp;
        recio_chnex -> cur_ofs += comp;
      }
										/* terminator matched, leave rec pointer past terminator */
      sts = OZ_SUCCESS;								/* successful read status */
not_terminator:
      if (sts != OZ_ENDOFFILE) goto rtn_oz;					/* success or read error, all done */
      recio_chnex -> cur_vbn = savevbn;						/* restore buffer pointer */
      recio_chnex -> cur_ofs = saveofs;						/* restore offset in the buffer */
      sts = fillrec (recio_chnex, recio_filex, 0);				/* re-read the block if needed */
      if (sts != OZ_SUCCESS) goto rtn_oz;					/* abort if read error, including eof */
      room = recio_chnex -> cur_buf -> buf_len - recio_chnex -> cur_ofs;
    }

    /* Not terminator byte, copy as much as we can up to the first terminator byte to the user's read buffer */

    if (room > size) room = size;
    copy = room;
    if (term_given) {
      p = recio_chnex -> cur_buf -> buf_data + recio_chnex -> cur_ofs;
      for (copy = 0; copy < room; copy ++) {
        if (*(p ++) == term_firstbyte) break;
      }
    }
    memcpy (buff, recio_chnex -> cur_buf -> buf_data + recio_chnex -> cur_ofs, copy);
    size -= copy;
    buff += copy;
    recio_chnex -> cur_ofs += copy;
  }
  sts = OZ_SUCCESS;
  if (term_given) sts = OZ_NOTERMINATOR;

rtn_oz:
  size = readrec -> size - size;
  if ((sts == OZ_ENDOFFILE) && (size > 0)) {
    sts = OZ_SUCCESS;
    if (term_given) sts = OZ_NOTERMINATOR;
  }
  if (readrec -> rlen != NULL) *(readrec -> rlen) = size;

  return (sts);
}

/************************************************************************/
/*									*/
/*  Process setcurpos function						*/
/*									*/
/*    Input:								*/
/*									*/
/*	setcurpos = set current position i/o parameter block		*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_recio_setcurpos = OZ_SUCCESS : successful			*/
/*	                           else : error status			*/
/*									*/
/************************************************************************/

uLong oz_sys_recio_setcurpos (OZ_Recio_chnex *recio_chnex, OZ_Recio_filex *recio_filex, OZ_IO_fs_setcurpos *setcurpos)

{
  /* If explicit position given, position there */

  if (setcurpos -> atblock != 0) {
    recio_chnex -> cur_vbn = setcurpos -> atblock;
    recio_chnex -> cur_ofs = setcurpos -> atbyte;
  }

  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Copy a buffer to the record buffer, writing it to disk as needed	*/
/*									*/
/*    Input:								*/
/*									*/
/*	size = size of user buffer to be written			*/
/*	buff = address of user buffer to be written			*/
/*	recio_chnex -> rec_vbn = vbn at start of rec_buf		*/
/*	               rec_ofs = offset in rec_buf to start copying	*/
/*									*/
/*    Output:								*/
/*									*/
/*	putrec = OZ_SUCCESS : successful				*/
/*	               else : failure status				*/
/*	block buffer updated						*/
/*	recio_chnex -> rec_vbn = incremented if old buffer overflowed	*/
/*	               rec_ofs = incremented by 'size' bytes		*/
/*	               rec_len = incremented to be at least rec_ofs	*/
/*									*/
/************************************************************************/

static uLong putrec (OZ_Recio_chnex *recio_chnex, OZ_Recio_filex *recio_filex, uLong size, const uByte *buff, uLong *wlen)

{
  uLong readsize, room, sts;

  while (size != 0) {
    sts = fillrec (recio_chnex, recio_filex, size);					/* maybe write last block out and read next block buffer in */
    if ((sts != OZ_SUCCESS) && (sts != OZ_ENDOFFILE)) return (sts);			/* return if any write or read error */
    room = recio_filex -> recbuffsize - recio_chnex -> cur_ofs;				/* see how much room is left in block buffer */
    if (room > size) room = size;							/* if more than we want, just use that much */
    recio_chnex -> cur_buf -> buf_dirty = 1;						/* buffer is about to be modified */
    memcpy (recio_chnex -> cur_buf -> buf_data + recio_chnex -> cur_ofs, buff, room);	/* copy data from user's buffer */
    size -= room;									/* decrement how much is left to do */
    buff += room;									/* increment where to get it from */
    recio_chnex -> cur_ofs += room;							/* increment offset where to put more */
    if (recio_chnex -> cur_ofs > recio_chnex -> cur_buf -> buf_len) {
      recio_chnex -> cur_buf -> buf_len = recio_chnex -> cur_ofs;			/* maybe have new 'end of valid data' offset */
    }
    if (wlen != NULL) *wlen += room;							/* add total length written */
  }

  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Fill a record buffer from disk					*/
/*									*/
/*    Input:								*/
/*									*/
/*	recio_chnex -> cur_vbn = vbn to read				*/
/*	recio_chnex -> cur_ofs = offset in vbn to read			*/
/*	overwrite = number of bytes, starting at current position, 	*/
/*	            that caller is about to overwrite (so don't bother 	*/
/*	            reading them if not necessary)			*/
/*									*/
/*    Output:								*/
/*									*/
/*	fillrec = OZ_SUCCESS : successful				*/
/*	        OZ_ENDOFFILE : we're at the end-of-file			*/
/*	                else : fill error				*/
/*	recio_chnex -> cur_buf = current buffer				*/
/*	recio_chnex -> cur_vbn = same as on input (may be normalized)	*/
/*	recio_chnex -> cur_ofs = same as on input (may be normalized)	*/
/*									*/
/************************************************************************/

static uLong fillrec (OZ_Recio_chnex *recio_chnex, OZ_Recio_filex *recio_filex, uLong overwrite)

{
  uLong buf_len, rfact, sts;
  OZ_Recio_buf *buf;

  /* Normalize the starting vbn and offset to an recbuffsize boundary */

  recio_chnex -> cur_vbn += recio_chnex -> cur_ofs / recio_filex -> diskblksize;
  recio_chnex -> cur_ofs %= recio_filex -> diskblksize;
  rfact = (recio_chnex -> cur_vbn - 1) % recio_filex -> blockfact;
  recio_chnex -> cur_vbn -= rfact;
  recio_chnex -> cur_ofs += rfact * recio_filex -> diskblksize;

  /* If our current buffer is ok, use it as is (but do check to see if pointer is at the eof) */

  buf = recio_chnex -> cur_buf;
  if ((buf != NULL) && (recio_chnex -> cur_vbn == buf -> buf_vbn)) goto rtn;

  /* Need a different buffer, make sure any pending write has been done and release current buffer */

  sts = flushbuf (recio_chnex, recio_filex);
  if (sts != OZ_SUCCESS) return (sts);

  /* See if it already exists on file's list of block buffers.  If so, point to it and inc ref count. */

  for (buf = recio_filex -> bufs; buf != NULL; buf = buf -> next) {
    if (buf -> buf_vbn == recio_chnex -> cur_vbn) {
      recio_chnex -> cur_buf = buf;
      buf -> refcount ++;
      goto rtn;
    }
  }

  /* Not on files list, allocate a new block buffer */

  buf = (*(recio_filex -> call -> malloc)) (recio_chnex -> chnex, recio_filex -> filex, recio_filex -> recbuffsize + sizeof *buf, __FILE__, __LINE__);

  /* If past eof block, don't read anything */

  buf_len = 0;
  if (recio_chnex -> cur_vbn > recio_filex -> efblk) goto linkit;

  /* If entire record block buffer is before eof, read whole record block buffer */

  buf_len = recio_filex -> recbuffsize;
  if (recio_chnex -> cur_vbn + recio_filex -> blockfact <= recio_filex -> efblk) goto readit;

  /* Eof is somewhere in the block buffer, just read up to the eof point */

  buf_len = (recio_filex -> efblk - recio_chnex -> cur_vbn) * recio_filex -> diskblksize + recio_filex -> efbyt;
  if (buf_len == 0) goto linkit;

  /* Read - only if we won't be overwriting all of it */

readit:
  if ((recio_chnex -> cur_ofs != 0) || (recio_chnex -> cur_ofs + overwrite < buf_len)) {
    sts = (*(recio_filex -> call -> read)) (recio_chnex -> chnex, recio_filex -> filex, recio_chnex -> cur_vbn, buf_len, buf -> buf_data);
    if (sts != OZ_SUCCESS) {
      (*(recio_filex -> call -> free)) (recio_chnex -> chnex, recio_filex -> filex, buf);
      return (sts);
    }
  }

  /* Link buffer up */

linkit:
  buf -> next = recio_filex -> bufs;
  buf -> prev = &(recio_filex -> bufs);
  if (buf -> next != NULL) buf -> next -> prev = &(buf -> next);
  recio_filex -> bufs = buf;
  buf -> buf_vbn   = recio_chnex -> cur_vbn;
  buf -> buf_len   = buf_len;
  buf -> buf_dirty = 0;
  buf -> refcount  = 1;

  /* Make it our current buffer */

  recio_chnex -> cur_buf = buf;

rtn:
  return ((recio_chnex -> cur_ofs >= buf -> buf_len) ? OZ_ENDOFFILE : OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Flush any pending write from the record buffer to disk		*/
/*									*/
/*    Input:								*/
/*									*/
/*	chnex -> cur_buf = buffer to write				*/
/*									*/
/*    Output:								*/
/*									*/
/*	flushbuf = OZ_SUCCESS : successful (or nothing to flush)	*/
/*	                 else : flush error				*/
/*	dirty flag cleared						*/
/*	chnex -> cur_buf = freed off					*/
/*									*/
/************************************************************************/

static uLong flushbuf (OZ_Recio_chnex *recio_chnex, OZ_Recio_filex *recio_filex)

{
  uLong sts;
  OZ_Recio_buf *cur_buf;

  cur_buf = recio_chnex -> cur_buf;

  sts = OZ_SUCCESS;
  if (cur_buf != NULL) {						/* see if we even have a buffer at all */
    if ((cur_buf -> buf_dirty) && (cur_buf -> buf_len != 0)) {		/* see if it is dirty (ie, it has modifications) */
      sts = (*(recio_filex -> call -> write)) (recio_chnex -> chnex, recio_filex -> filex, cur_buf -> buf_vbn, cur_buf -> buf_len, cur_buf -> buf_data);
    }
    cur_buf -> buf_dirty = 0;						/* it now matches what is on disk */

    cur_buf -> refcount --;						/* decrement reference count */
    if (cur_buf -> refcount == 0) {					/* if zero, */
      *(cur_buf -> prev) = cur_buf -> next;				/* unlink it from recio_filex->bufs list */
      (*(recio_filex -> call -> free)) (recio_chnex -> chnex, recio_filex -> filex, cur_buf); /* free off memory */
    }
    recio_chnex -> cur_buf = NULL;					/* channel does not have block buffer anymore */
  }

  return (sts);
}
