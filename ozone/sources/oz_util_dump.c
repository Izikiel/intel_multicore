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

/************************************************************************/
/*									*/
/*  Dump utiliti							*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_crtl_malloc.h"
#include "oz_io_fs.h"
#include "oz_knl_lock.h"
#include "oz_knl_status.h"
#include "oz_sys_io.h"
#include "oz_sys_io_fs.h"
#include "oz_sys_io_fs_printf.h"
#include "oz_util_start.h"

static char *pn;

uLong oz_util_main (int argc, char *argv[])

{
  uByte b;
  char *filename;
  int i, j;
  uLong blockcount, blocklength, blocknumber, blocksize, blockskip, bytesperline, sts;
  OZ_Handle h_file;
  OZ_IO_fs_getinfo1 fs_getinfo1;
  OZ_IO_fs_open fs_open;
  OZ_IO_fs_readrec fs_readrec;

  pn = "dump";
  if (argc > 0) pn = argv[0];

  blockcount   = -1;
  blocksize    = 0;
  blockskip    = 0;
  bytesperline = 16;
  filename     = NULL;

  for (i = 1; i < argc; i ++) {
    if (argv[i][0] == '-') {
      if (strcmp (argv[i], "-blockcount") == 0) {
        if (++ i >= argc) goto usage;
        blockcount = oz_hw_atoi (argv[i], NULL);
        continue;
      }
      if (strcmp (argv[i], "-blocksize") == 0) {
        if (++ i >= argc) goto usage;
        blocksize = oz_hw_atoi (argv[i], NULL);
        continue;
      }
      if (strcmp (argv[i], "-blockskip") == 0) {
        if (++ i >= argc) goto usage;
        blockskip = oz_hw_atoi (argv[i], NULL);
        continue;
      }
      if (strcmp (argv[i], "-bytesperline") == 0) {
        if (++ i >= argc) goto usage;
        bytesperline = oz_hw_atoi (argv[i], NULL);
        continue;
      }
      goto usage;
    }
    if (filename != NULL) goto usage;
    filename = argv[i];
  }
  if (filename == NULL) goto usage;

  memset (&fs_open, 0, sizeof fs_open);
  fs_open.name = filename;
  fs_open.lockmode = OZ_LOCKMODE_CR;
  sts = oz_sys_io_fs_open (sizeof fs_open, &fs_open, 0, &h_file);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u opening file %s\n", pn, sts, filename);
    return (sts);
  }

  if (blocksize == 0) {
    memset (&fs_getinfo1, 0, sizeof fs_getinfo1);
    sts = oz_sys_io (OZ_PROCMODE_KNL, h_file, 0, OZ_IO_FS_GETINFO1, sizeof fs_getinfo1, &fs_getinfo1);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u getting %s blocksize\n", pn, sts, filename);
      return (sts);
    }
    blocksize = fs_getinfo1.blocksize;
    if (blocksize == 0) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: blocksize for %s is zero\n", pn, filename);
      return (OZ_BADBLOCKSIZE);
    }
  }

  memset (&fs_readrec, 0, sizeof fs_readrec);								/* use record I/O with no terminator */
  fs_readrec.size    = blocksize;									/* ... to read blocks of arbitrary size */
  fs_readrec.buff    = malloc (blocksize);								/* ... and also to stop at the eof pointer */
  fs_readrec.rlen    = &blocklength;									/* get length read here */
  fs_readrec.atblock = blockskip + 1;									/* block to start at */
  blocknumber = blockskip;
  while (blockcount > 0) {
    sts = oz_sys_io (OZ_PROCMODE_KNL, h_file, 0, OZ_IO_FS_READREC, sizeof fs_readrec, &fs_readrec);
    if ((sts != OZ_SUCCESS) && (sts != OZ_ENDOFFILE)) break;
    if ((sts == OZ_ENDOFFILE) && (blocklength == 0)) break;
    ++ blocknumber;											/* increment block number */
    -- blockcount;											/* one less block to print out */
    fs_readrec.atblock = 0;										/* just sequential from now on */
    oz_sys_io_fs_printf (oz_util_h_output, "\nVbn %8.8x (%u)\n\n", blocknumber, blocknumber);		/* print out block number */
    for (i = 0; i < blocklength; i += bytesperline) {							/* loop through block, 'bytesperline' bytes at a time */
      oz_sys_io_fs_printf (oz_util_h_output, "  ");							/* a couple spaces at beginning of line */
      for (j = bytesperline; -- j >= 0;) {								/* loop through each byte of line */
        if (i + j >= blocklength) oz_sys_io_fs_printf (oz_util_h_output, "  ");				/* spaces if past end of block */
        else oz_sys_io_fs_printf (oz_util_h_output, "%2.2x", ((uByte *)(fs_readrec.buff))[i+j]);	/* otherwise, two hex digits */
        if ((j & 3) == 0) oz_sys_io_fs_printf (oz_util_h_output, " ");					/* a space for every longword output */
      }
      oz_sys_io_fs_printf (oz_util_h_output, " %4.4x  '", i);						/* output offset */
      for (j = 0; (j < bytesperline) && (i + j < blocklength); j ++) {					/* output character version */
        b = ((uByte *)(fs_readrec.buff))[i+j];
        if ((b < ' ') || (b > 126)) b = '.';
        oz_sys_io_fs_printf (oz_util_h_output, "%c", b);
      }
      oz_sys_io_fs_printf (oz_util_h_output, "'\n");
    }
    oz_sys_io_fs_printf (oz_util_h_output, "\n");							/* extra blank line at end of block */
  }
  if (sts == OZ_ENDOFFILE) sts = OZ_SUCCESS;
  if (sts != OZ_SUCCESS) oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u reading file %s\n", pn, sts, filename);

  return (sts);

usage:
  oz_sys_io_fs_printf (oz_util_h_error, "usage: %s <filename>\n", pn);
  oz_sys_io_fs_printf (oz_util_h_error, "	[-blockcount <numberofblocks>]\n");
  oz_sys_io_fs_printf (oz_util_h_error, "	[-blocksize <bytesperblock>]\n");
  oz_sys_io_fs_printf (oz_util_h_error, "	[-blockskip <blockstoskip>]\n");
  oz_sys_io_fs_printf (oz_util_h_error, "	[-bytesperline <bytesperline>]\n");
  return (OZ_BADPARAM);
}
