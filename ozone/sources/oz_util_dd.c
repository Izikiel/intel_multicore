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
/*  DD utiliti								*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_crtl_malloc.h"
#include "oz_io_disk.h"
#include "oz_io_fs.h"
#include "oz_knl_lock.h"
#include "oz_knl_status.h"
#include "oz_sys_io.h"
#include "oz_sys_io_fs.h"
#include "oz_sys_io_fs_printf.h"
#include "oz_util_start.h"

#define CONV_SYNC 1

static char *pn = "dd";

uLong oz_util_main (int argc, char *argv[])

{
  char *buff, *input_file, *output_file;
  int i, usedup;
  OZ_Handle h_in, h_out;
  OZ_IO_fs_create fs_create;
  OZ_IO_fs_extend fs_extend;
  OZ_IO_fs_getinfo1 fs_getinfo1_in, fs_getinfo1_out;
  OZ_IO_fs_open fs_open;
  OZ_IO_fs_readrec fs_readrec;
  OZ_IO_fs_writerec fs_writerec;
  uLong blocksize, convflags, countblocks, skipblocks, sts;
  uQuad inputbytes, outputbytes;

  if (argc > 0) pn = argv[0];

  /* Set up defaults */

  h_in  = oz_util_h_input;
  h_out = oz_util_h_output;
  input_file  = "OZ_INPUT";
  output_file = "OZ_OUTPUT";
  blocksize   = 1;
  skipblocks  = 0;
  countblocks = 0;
  convflags   = 0;

  /* Process command line arguments */

  for (i = 1; i < argc; i ++) {
    if (strncasecmp (argv[i], "if=", 3) == 0) {
      input_file = argv[i] + 3;
      memset (&fs_open, 0, sizeof fs_open);
      fs_open.name     = input_file;
      fs_open.lockmode = OZ_LOCKMODE_PR;
      sts = oz_sys_io_fs_open (sizeof fs_open, &fs_open, 0, &h_in);
      if (sts != OZ_SUCCESS) {
        oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u opening input file %s\n", pn, sts, input_file);
        return (sts);
      }
      continue;
    }
    if (strncasecmp (argv[i], "of=", 3) == 0) {
      output_file = argv[i] + 3;
      memset (&fs_create, 0, sizeof fs_create);
      fs_create.name     = output_file;
      fs_create.lockmode = OZ_LOCKMODE_PW;
      sts = oz_sys_io_fs_create (sizeof fs_create, &fs_create, 0, &h_out);
      if (sts != OZ_SUCCESS) {
        oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u creating output file %s\n", pn, sts, output_file);
        return (sts);
      }
      continue;
    }
    if (strncasecmp (argv[i], "bs=", 3) == 0) {
      blocksize = oz_hw_atoi (argv[i] + 3, &usedup);
      if ((usedup == 0) || (argv[i][3+usedup] != 0) || (blocksize == 0)) {
        oz_sys_io_fs_printf (oz_util_h_error, "%s: bad blocksize %s\n", pn, sts, argv[i] + 3);
        return (OZ_BADPARAM);
      }
      continue;
    }
    if (strncasecmp (argv[i], "skip=", 5) == 0) {
      skipblocks = oz_hw_atoi (argv[i] + 5, &usedup);
      if ((usedup == 0) || (argv[i][5+usedup] != 0)) {
        oz_sys_io_fs_printf (oz_util_h_error, "%s: bad block skip count %s\n", pn, sts, argv[i] + 5);
        return (OZ_BADPARAM);
      }
      continue;
    }
    if (strncasecmp (argv[i], "count=", 6) == 0) {
      countblocks = oz_hw_atoi (argv[i] + 6, &usedup);
      if ((usedup == 0) || (argv[i][6+usedup] != 0) || (countblocks == 0)) {
        oz_sys_io_fs_printf (oz_util_h_error, "%s: bad block count %s\n", pn, sts, argv[i] + 6);
        return (OZ_BADPARAM);
      }
      continue;
    }
    if (strcasecmp (argv[i], "conv=sync") == 0) {
      convflags |= CONV_SYNC;
      continue;
    }
    oz_sys_io_fs_printf (oz_util_h_error, "%s: unknown parameter %s\n", pn, argv[i]);
    oz_sys_io_fs_printf (oz_util_h_error, "usage: %s if=file of=file bs=bytes skip=blocks count=blocks conv=sync\n", pn);
    return (OZ_BADPARAM);
  }

  /* Allocate block buffer */

  buff = malloc (blocksize);

  /* Get input file size then extend output file to required size                              */
  /* But don't bother if we can't get output file size (because we won't be able to extend it) */

  memset (&fs_getinfo1_out, 0, sizeof fs_getinfo1_out);
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_out, 0, OZ_IO_FS_GETINFO1, sizeof fs_getinfo1_out, &fs_getinfo1_out);
  if ((sts == OZ_SUCCESS) && (fs_getinfo1_out.blocksize > 0) && (fs_getinfo1_out.eofblock > 0) && (fs_getinfo1_out.curblock > 0)) {

    /* Good, we have the output file block size (so it is extendable) */
    /* Now if a block count was not given, get the size of the input  */

    inputbytes = countblocks * blocksize;
    if (inputbytes == 0) {
      memset (&fs_getinfo1_in, 0, sizeof fs_getinfo1_in);
      sts = oz_sys_io (OZ_PROCMODE_KNL, h_in, 0, OZ_IO_FS_GETINFO1, sizeof fs_getinfo1_in, &fs_getinfo1_in);
      if ((sts == OZ_SUCCESS) && (fs_getinfo1_in.blocksize > 0) && (fs_getinfo1_in.curblock > 0) && (fs_getinfo1_in.eofblock >= fs_getinfo1_in.curblock)) {
        inputbytes  = (fs_getinfo1_in.eofblock - fs_getinfo1_in.curblock);
        inputbytes *= fs_getinfo1_in.blocksize;
        inputbytes += fs_getinfo1_in.eofbyte;
        inputbytes -= fs_getinfo1_in.curbyte;
      }
    }

    /* If we have number of input bytes now, extend the output file */

    if (inputbytes != 0) {
      outputbytes  = fs_getinfo1_out.curblock - 1;
      outputbytes *= fs_getinfo1_out.blocksize;
      outputbytes += fs_getinfo1_out.curbyte;
      outputbytes += inputbytes;
      outputbytes += fs_getinfo1_out.blocksize - 1;
      memset (&fs_extend, 0, sizeof fs_extend);
      fs_extend.nblocks = outputbytes / fs_getinfo1_out.blocksize;
      if (fs_extend.nblocks > fs_getinfo1_out.hiblock) {
        sts = oz_sys_io (OZ_PROCMODE_KNL, h_out, 0, OZ_IO_FS_EXTEND, sizeof fs_extend, &fs_extend);
        if (sts != OZ_SUCCESS) {
          oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u extending %s from %u to %u blocks\n", pn, sts, output_file, fs_getinfo1_out.hiblock, fs_extend.nblocks);
          return (sts);
        }
      }
    }
  }

  /* Set copying parameter blocks */

  memset (&fs_readrec,  0, sizeof fs_readrec);
  memset (&fs_writerec, 0, sizeof fs_writerec);
  fs_readrec.size  = blocksize;
  fs_readrec.buff  = buff;
  fs_readrec.rlen  = &fs_writerec.size;
  fs_writerec.buff = buff;

  /* Copy input file to output file */

  while (((sts = oz_sys_io (OZ_PROCMODE_KNL, h_in, 0, OZ_IO_FS_READREC, sizeof fs_readrec, &fs_readrec)) == OZ_SUCCESS) || (sts == OZ_ENDOFFILE)) {

    /* If no data, it really is end-of-file */

    if (fs_writerec.size == 0) break;

    /* Check the skip input block counter */

    if (skipblocks > 0) {
      skipblocks --;
      continue;
    }

    /* If 'conv=sync' and input block is short, pad it with nulls */

    if ((convflags & CONV_SYNC) && (fs_writerec.size < blocksize)) {
      memset (buff + fs_writerec.size, 0, blocksize - fs_writerec.size);
      fs_writerec.size = blocksize;
    }

    /* Write the output block */

    sts = oz_sys_io (OZ_PROCMODE_KNL, h_out, 0, OZ_IO_FS_WRITEREC, sizeof fs_writerec, &fs_writerec);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u writing %s\n", pn, sts, output_file);
      return (sts);
    }

    /* If we have written all output blocks, we're all done */

    if (-- countblocks == 0) break;
  }

  /* Check read status and close input file */

  if ((sts != OZ_ENDOFFILE) && (sts != OZ_SUCCESS)) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u reading %s\n", pn, sts, argv[i]);
    return (sts);
  }
  oz_sys_handle_release (OZ_PROCMODE_KNL, h_in);

  /* Close output file and return final status */

  sts = OZ_SUCCESS;
  if (h_out != oz_util_h_output) {
    sts = oz_sys_io (OZ_PROCMODE_KNL, h_out, 0, OZ_IO_FS_CLOSE, 0, NULL);
    if (sts != OZ_SUCCESS) oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u closing %s\n", pn, sts, output_file);
  }

  return (sts);
}
