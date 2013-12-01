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
/*  Copy and Rename file utiliti					*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_crtl_malloc.h"
#include "oz_io_disk.h"
#include "oz_io_fs.h"
#include "oz_knl_devio.h"
#include "oz_knl_hw.h"
#include "oz_knl_handle.h"
#include "oz_knl_lock.h"
#include "oz_knl_status.h"
#include "oz_sys_dateconv.h"
#include "oz_sys_io.h"
#include "oz_sys_io_fs.h"
#include "oz_sys_io_fs_printf.h"
#include "oz_util_filesel.h"
#include "oz_util_start.h"

static char *pn;
static int continue_count, continue_flag;
static int newversion_flag;
static int outhasvers;
static int rename_mode;
static int update_flag;
static OZ_Filesel *filesel;

static uLong checkoutver (void *dummy, const char *devname, const char *filname, OZ_Handle h_iochan);
static uLong copyafile (void *outname, const char *indevname, const char *inwildcard, const char *ininstance, OZ_Handle h_ioch, uLong fileidsize, void *fileidbuff, OZ_FS_Subs *wildsubs, OZ_FS_Subs *instsubs);
static uLong renafile (void *outname, const char *indevname, const char *inwildcard, const char *ininstance, OZ_Handle h_ioch, uLong fileidsize, void *fileidbuff, OZ_FS_Subs *wildsubs, OZ_FS_Subs *instsubs);
static void getfname (OZ_Handle h, char name[256], const char *argname);

uLong oz_util_main (int argc, char *argv[])

{
  char *input_file, *output_file;
  int i, rc;
  uLong sts;

  /* Program name must be either 'copy' or 'rename' */

  rename_mode = 0;
  pn = "copy";
  if (argc > 0) {
    pn = argv[0];
    if (strcmp (pn, "rename") == 0) rename_mode = 1;
    else if (strcmp (pn, "copy") != 0) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: program name must be either 'copy' or 'rename'\n", pn);
      return (OZ_BADPARAM);
    }
  }

  /* Process command line arguments */

  continue_count  = 0;
  continue_flag   = 0;
  filesel         = oz_util_filesel_init ();
  input_file      = NULL;
  newversion_flag = 0;
  output_file     = NULL;
  update_flag     = 0;

  for (i = 1; i < argc; i ++) {
    if (argv[i][0] != '-') {
      if (input_file == NULL) {
        input_file = argv[i];
        continue;
      }
      if (output_file == NULL) {
        output_file = argv[i];
        continue;
      }
      goto usage;
    }
    rc = oz_util_filesel_parse (filesel, argc - i, argv + i);
    if (rc < 0) goto usage_filesel;
    if (rc > 0) {
      i += rc - 1;
      continue;
    }
    if (strcmp (argv[i] + 1, "continue") == 0) {
      continue_flag = 1;
      continue;
    }
    if (strcmp (argv[i] + 1, "newversion") == 0) {
      newversion_flag = 1;
      continue;
    }
    if (strcmp (argv[i] + 1, "update") == 0) {
      update_flag = 1;
      continue;
    }
    goto usage;
  }
  if (output_file == NULL) goto usage;

  /* See if output filesystem does version numbers */

  outhasvers = 0;
  oz_sys_io_fs_parse (output_file, 0, checkoutver, NULL);

  /* Call 'copyafile' or 'renafile' for each input file */

  sts = oz_sys_io_fs_wildscan (input_file, 0, rename_mode ? renafile : copyafile, output_file);
  if (sts == OZ_IOFSPARSECONT) sts = OZ_SUCCESS;
  else oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u processing %s\n", pn, sts, input_file);
  if ((sts == OZ_SUCCESS) && (continue_count != 0)) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: %d error%s\n", pn, continue_count, (continue_count != 1) ? "s" : "");
    sts = OZ_CONTONERROR;
  }

  return (sts);

usage_filesel:
  oz_sys_io_fs_printf (oz_util_h_error, "%s: invalid argument[%d] = '%s': %s\n", pn, i, argv[i], oz_util_filesel_errmsg (filesel));
usage:
  oz_sys_io_fs_printf (oz_util_h_error, "usage: %s [-continue] [-newversion] [-update] %s <input_file> <output_file>\n", pn, oz_util_filesel_usage);
  return (OZ_MISSINGPARAM);
}

static uLong checkoutver (void *dummy, const char *devname, const char *filname, OZ_Handle h_iochan)

{
  OZ_IO_fs_getinfo3 fs_getinfo3;
  uLong sts;

  if (h_iochan == 0) {
    sts = oz_sys_io_fs_assign (devname, OZ_LOCKMODE_NL, &h_iochan);
    if (sts == OZ_SUCCESS) {
      memset (&fs_getinfo3, 0, sizeof fs_getinfo3);
      sts = oz_sys_io (OZ_PROCMODE_KNL, h_iochan, 0, OZ_IO_FS_GETINFO3, sizeof fs_getinfo3, &fs_getinfo3);
      if (sts == OZ_SUCCESS) outhasvers = fs_getinfo3.versions;
      oz_sys_handle_release (OZ_PROCMODE_KNL, h_iochan);
    }
  }
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  This routine is called for each input file				*/
/*									*/
/*    Input:								*/
/*									*/
/*	outname = output filename spec (argv[2])			*/
/*	indevname = input device name					*/
/*	inwildcard = input wildcard spec that file matches		*/
/*	ininstance = input file instance spec that matched		*/
/*	h_ioch = non-zero if copying from an I/O channel logname	*/
/*	         in which case indevname="", inwildcard=ininstance=logname
/*									*/
/*    Output:								*/
/*									*/
/*	copyafile = OZ_IOFSWILDSCANCONT : continue wildcard scan	*/
/*	                           else : stop scanning and return	*/
/*									*/
/************************************************************************/

static uLong copyafile (void *outname, const char *indevname, const char *inwildcard, const char *ininstance, OZ_Handle h_ioch, uLong fileidsize, void *fileidbuff, OZ_FS_Subs *wildsubs, OZ_FS_Subs *instsubs)

{
  char buff[65536], iname[256], oname[256], *outpath, *p, prompt[16];
  int ateof, i, inputisadir;
  uLong kbps, oblocksize, sts;
  OZ_Datebin input_mod_date, started, stopped;
  OZ_Dbn count;
  OZ_Handle h_in, h_out;
  OZ_IO_fs_close fs_close;
  OZ_IO_fs_create fs_create;
  OZ_IO_fs_extend fs_extend;
  OZ_IO_fs_getinfo1 fs_getinfo1;
  OZ_IO_fs_open fs_open;
  OZ_IO_fs_readblocks fs_readblocks;
  OZ_IO_fs_readrec fs_readrec;
  OZ_IO_fs_writeblocks fs_writeblocks;
  OZ_IO_fs_writerec fs_writerec;
  OZ_Itmlst2 fsattrs[4];

  outpath = NULL;
  h_in    = 0;
  h_out   = 0;

  oz_sys_io_fs_printf (oz_util_h_error, "copy*:    outname %s\n", outname);
  oz_sys_io_fs_printf (oz_util_h_error, "copy*:  indevname %s\n", indevname);
  oz_sys_io_fs_printf (oz_util_h_error, "copy*: inwildcard %s\n", inwildcard);
  oz_sys_io_fs_printf (oz_util_h_error, "copy*: ininstance %s\n", ininstance);
  oz_sys_io_fs_printf (oz_util_h_error, "copy*:     h_ioch %X\n", h_ioch);

  /* Open input file for reading, allow others to read it */

  if (h_ioch != 0) {
    h_in = h_ioch;
    strncpyz (iname, ininstance, sizeof iname);
    inputisadir = 0;
    sts = OZ_BADIOFUNC;	// in case trying to use the other fs_getinfo1 results
  } else {

    oz_sys_sprintf (sizeof iname, iname, "%s:%s", indevname, ininstance);

    sts = oz_sys_io_assign (OZ_PROCMODE_KNL, &h_in, indevname, OZ_LOCKMODE_CR);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u assigning channel to input device %s\n", pn, sts, indevname);
      goto rtn;
    }

    memset (&fs_open, 0, sizeof fs_open);
    fs_open.name = ininstance;
    fs_open.lockmode = OZ_LOCKMODE_PR;
    sts = oz_sys_io (OZ_PROCMODE_KNL, h_in, 0, OZ_IO_FS_OPEN, sizeof fs_open, &fs_open);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u opening input file %s:%s\n", pn, sts, indevname, ininstance);
      goto rtn;
    }

    memset (&fs_getinfo1, 0, sizeof fs_getinfo1);
    sts = oz_sys_io (OZ_PROCMODE_KNL, h_in, 0, OZ_IO_FS_GETINFO1, sizeof fs_getinfo1, &fs_getinfo1);
    if ((sts != OZ_SUCCESS) && (sts != OZ_BADIOFUNC)) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u getting input file %s:%s characteristics\n", pn, sts, indevname, ininstance);
      goto rtn;
    }

    inputisadir = ((fs_getinfo1.filattrflags & OZ_FS_FILATTRFLAG_DIRECTORY) != 0);
  }

  /* If -update, get the mod date of the input file */

  if (update_flag) {
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u getting input file %s:%s modification date\n", pn, sts, indevname, ininstance);
      goto rtn;
    }
    input_mod_date = fs_getinfo1.modify_date;
  }

  /* Check file selection criteria */

  sts = oz_util_filesel_check (filesel, h_in, ininstance);
  if (sts == OZ_FLAGWASCLR) {
    sts = OZ_IOFSWILDSCANCONT;
    goto rtn;
  }
  if (sts != OZ_FLAGWASSET) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u %s file %s:%s\n", pn, sts, oz_util_filesel_errmsg (filesel), ininstance);
    goto rtn;
  }

  /* Make up the output path = outname + different part between ininstance and inwildcard */

  for (i = 0; ininstance[i] == inwildcard[i]; i ++) if (ininstance[i] == 0) break;

  outpath = malloc (strlen (outname) + strlen (ininstance + i) + 1);
  strcpy (outpath, outname);
  strcat (outpath, ininstance + i);
  if (!outhasvers) {
    i = strlen (outpath);
    outpath[i-instsubs->versize] = 0;
  }

  /* If -update, try to open output file and get its modification date */

  if (update_flag && !inputisadir) {
    memset (&fs_open, 0, sizeof fs_open);
    fs_open.name = outpath;
    fs_open.lockmode = OZ_LOCKMODE_NL;
    sts = oz_sys_io_fs_open (sizeof fs_open, &fs_open, 0, &h_out);
    if (sts == OZ_SUCCESS) {
      memset (&fs_getinfo1, 0, sizeof fs_getinfo1);
      sts = oz_sys_io (OZ_PROCMODE_KNL, h_out, 0, OZ_IO_FS_GETINFO1, sizeof fs_getinfo1, &fs_getinfo1);
      oz_sys_handle_release (OZ_PROCMODE_KNL, h_out);
      h_out = 0;
      if (sts == OZ_SUCCESS) {
        sts = OZ_IOFSWILDSCANCONT;
        if (OZ_HW_DATEBIN_CMP (input_mod_date, fs_getinfo1.modify_date) <= 0) goto rtn;
      }
    }
    if ((sts != OZ_NOSUCHFILE) && (sts != OZ_IOFSWILDSCANCONT)) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u probing output file %s modification date\n", pn, sts, outpath);
      goto rtn;
    }
  }

  /* Create output file or directory */

  oz_sys_io_fs_printf (oz_util_h_error, "copy*:    outpath %s\n", outpath);

  memset (&fs_create, 0, sizeof fs_create);
  fs_create.name       = outpath;						/* this is the filename to create */
  fs_create.lockmode   = OZ_LOCKMODE_PW;					/* don't let others write */
  fs_create.rnamesize  = sizeof oname;						/* get the resultant filename back */
  fs_create.rnamebuff  = buff;							/* ... but put it in this temp buffer */
  if (inputisadir) fs_create.filattrflags = OZ_FS_FILATTRFLAG_DIRECTORY;	/* ... we are creating a directory */
  fs_create.newversion = newversion_flag;
  buff[0] = 0;									/* (in case fs driver doesn't return filespec) */
  sts = oz_sys_io_fs_create (sizeof fs_create, &fs_create, 0, &h_out);		/* create it */
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u creating output file %s\n", pn, sts, outpath);
    goto rtn;
  }
  if (buff[0] == 0) {								/* see if it returned the actual filespec */
    strncpyz (oname, outpath, sizeof oname);					/* if not, use what we gave it */
  } else {
    sts = oz_sys_iochan_getunitname (h_out, sizeof oname, oname);		/* it did, get the device it was created on */
    if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
    i = strlen (oname);								/* ... followed by a colon */
    oname[i++] = ':';
    strncpyz (oname + i, buff, sizeof oname - i);				/* ... followed by the resultant filespec */
  }

  /* If directory, all done */

  if (inputisadir) {
    oz_sys_io_fs_printf (oz_util_h_output, "%s: created directory %s\n", pn, oname);
    sts = OZ_IOFSWILDSCANCONT;
    goto rtn;
  }

  /* If a logical name I/O channel, do record-style copy, because we want to start with wherever the pointer is now */

  if (h_ioch != 0) goto record_by_record;

  /* If block sizes don't match (or at least one of them can't provide their blocksize), do record-by-record copy */

  memset (&fs_getinfo1, 0, sizeof fs_getinfo1);
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_out, 0, OZ_IO_FS_GETINFO1, sizeof fs_getinfo1, &fs_getinfo1);
  if (sts == OZ_BADIOFUNC) goto record_by_record;
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u getting %s block size\n", pn, sts, oname);
    goto rtn;
  }

  oblocksize = fs_getinfo1.blocksize;
  if (oblocksize == 0) goto record_by_record;

  memset (&fs_getinfo1, 0, sizeof fs_getinfo1);
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_in, 0, OZ_IO_FS_GETINFO1, sizeof fs_getinfo1, &fs_getinfo1);
  if (sts == OZ_BADIOFUNC) goto record_by_record;
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u getting %s block size\n", pn, sts, iname);
    goto rtn;
  }

  if (oblocksize != fs_getinfo1.blocksize) goto record_by_record;

  /* Disk-to-disk copy, extend output file to size of input file */

  memset (&fs_extend, 0, sizeof fs_extend);
  fs_extend.nblocks = fs_getinfo1.eofblock;
  if ((fs_getinfo1.eofbyte == 0) && (fs_getinfo1.eofblock != 0)) fs_extend.nblocks --;
  if (fs_extend.nblocks != 0) {
    sts = oz_sys_io (OZ_PROCMODE_KNL, h_out, 0, OZ_IO_FS_EXTEND, sizeof fs_extend, &fs_extend);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u extending %s to %u\n", pn, sts, oname, fs_extend.nblocks);
      goto rtn;
    }
  }

  /* Set up block-style copy */

  memset (&fs_readblocks,  0, sizeof fs_readblocks);
  memset (&fs_writeblocks, 0, sizeof fs_writeblocks);

  fs_readblocks.size  = ((sizeof buff - 1)/ oblocksize) * oblocksize;
  fs_readblocks.buff  = buff;
  fs_readblocks.svbn  = 1;

  fs_writeblocks.size = ((sizeof buff - 1)/ oblocksize) * oblocksize;
  fs_writeblocks.buff = buff;

  count = 0;

  /* Copy whatever blocks will copy using full buffers */

  started = oz_hw_tod_getnow ();
  while (fs_getinfo1.eofblock - fs_readblocks.svbn > fs_readblocks.size / oblocksize) {
    sts = oz_sys_io (OZ_PROCMODE_KNL, h_in, 0, OZ_IO_FS_READBLOCKS, sizeof fs_readblocks, &fs_readblocks);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u reading input file %s at %u\n", pn, sts, iname, fs_readblocks.svbn);
      goto rtn;
    }

    fs_writeblocks.svbn = fs_readblocks.svbn;
    sts = oz_sys_io (OZ_PROCMODE_KNL, h_out, 0, OZ_IO_FS_WRITEBLOCKS, sizeof fs_writeblocks, &fs_writeblocks);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u writing output file %s at %u\n", pn, sts, oname, fs_writeblocks.svbn);
      goto rtn;
    }

    count += fs_writeblocks.size / oblocksize;
    fs_readblocks.svbn += fs_writeblocks.size / oblocksize;
  }

  /* Copy the last partial bit */

  fs_readblocks.size = (fs_getinfo1.eofblock - fs_readblocks.svbn) * oblocksize + fs_getinfo1.eofbyte;
  fs_readblocks.size = ((fs_readblocks.size + oblocksize - 1) / oblocksize) * oblocksize;
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_in, 0, OZ_IO_FS_READBLOCKS, sizeof fs_readblocks, &fs_readblocks);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u reading input file %s at %u\n", pn, sts, iname, fs_readblocks.svbn);
    goto rtn;
  }

  fs_writeblocks.size = fs_readblocks.size;
  fs_writeblocks.svbn = fs_readblocks.svbn;
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_out, 0, OZ_IO_FS_WRITEBLOCKS, sizeof fs_writeblocks, &fs_writeblocks);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u writing output file %s at %u\n", pn, sts, oname, fs_writeblocks.svbn);
    goto rtn;
  }
  stopped = oz_hw_tod_getnow ();

  count += fs_writeblocks.size / oblocksize;

  /* Close file and write attributes */

  memset (&fs_close, 0, sizeof fs_close);
  fsattrs[0].item = OZ_FSATTR_CREATE_DATE;
  fsattrs[0].size = sizeof fs_getinfo1.create_date;
  fsattrs[0].buff = &fs_getinfo1.create_date;
  fsattrs[1].item = OZ_FSATTR_MODIFY_DATE;
  fsattrs[1].size = sizeof fs_getinfo1.modify_date;
  fsattrs[1].buff = &fs_getinfo1.modify_date;
  fsattrs[2].item = OZ_FSATTR_EOFBLOCK;
  fsattrs[2].size = sizeof fs_getinfo1.eofblock;
  fsattrs[2].buff = &fs_getinfo1.eofblock;
  fsattrs[3].item = OZ_FSATTR_EOFBYTE;
  fsattrs[3].size = sizeof fs_getinfo1.eofbyte;
  fsattrs[3].buff = &fs_getinfo1.eofbyte;
  fs_close.numitems = 4;
  fs_close.itemlist = fsattrs;
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_out, 0, OZ_IO_FS_CLOSE, sizeof fs_close, &fs_close);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u closing output file %s\n", pn, sts, oname);
  } else {
    kbps = ((OZ_Datebin)count * oblocksize * (OZ_TIMER_RESOLUTION / 1000)) / (stopped - started);
    oz_sys_io_fs_printf (oz_util_h_error, "%s: %s copied to %s (%u block%s, %uKB/s)\n", pn, iname, oname, count, (count != 1) ? "s" : "", kbps);
    sts = OZ_IOFSWILDSCANCONT;
  }

  goto rtn;

  /* Record-by-record copy */

record_by_record:
  memset (&fs_readrec,  0, sizeof fs_readrec);
  memset (&fs_writerec, 0, sizeof fs_writerec);

  strncpyz (prompt, pn, sizeof prompt - 3);
  strcat  (prompt, ">");

  fs_readrec.size    = sizeof buff;
  fs_readrec.buff    = buff;
  fs_readrec.rlen    = &(fs_writerec.size);
  fs_readrec.trmsize = 1;
  fs_readrec.trmbuff = "\n";
  fs_readrec.pmtsize = strlen (prompt);
  fs_readrec.pmtbuff = prompt;

  fs_writerec.buff    = buff;
  fs_writerec.trmbuff = fs_readrec.trmbuff;

  count = 0;
  ateof = 0;
  oblocksize = 0;

  started = oz_hw_tod_getnow ();
  do {
    sts = oz_sys_io (OZ_PROCMODE_KNL, h_in, 0, OZ_IO_FS_READREC, sizeof fs_readrec, &fs_readrec);	/* read input record */
    fs_writerec.trmsize = 0;										/* assume terminator was not found */
    if (sts == OZ_SUCCESS) { fs_writerec.trmsize = fs_readrec.trmsize; count ++; }			/* well ok, it was */
    else if (sts == OZ_ENDOFFILE) ateof = 1;								/* maybe we hit the eof */
    else if (sts != OZ_NOTERMINATOR) goto readrecerror;							/* noterminator just means that, all else bombs */
    oblocksize += fs_writerec.size + fs_writerec.trmsize;						/* count up total bytes written */
    sts = oz_sys_io (OZ_PROCMODE_KNL, h_out, 0, OZ_IO_FS_WRITEREC, sizeof fs_writerec, &fs_writerec);	/* write the record */
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u writing output file %s\n", pn, sts, oname);
      goto rtn;
    }
  } while (!ateof);											/* repeat until we hit the end of input file */
  stopped = oz_hw_tod_getnow ();

  sts = oz_sys_io (OZ_PROCMODE_KNL, h_out, 0, OZ_IO_FS_CLOSE, 0, NULL);					/* close output file */
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u closing output file %s\n", pn, sts, oname);
  } else {
    kbps = ((OZ_Datebin)oblocksize * (OZ_TIMER_RESOLUTION / 1000)) / (stopped - started);
    oz_sys_io_fs_printf (oz_util_h_error, "%s: %s copied to %s (%u record%s, %uKB/s)\n", pn, iname, oname, count, (count != 1) ? "s" : "", kbps);
    sts = OZ_IOFSWILDSCANCONT;
  }
  goto rtn;

readrecerror:
  oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u reading input file %s\n", pn, sts, iname);

  /* Clean up and return status */

rtn:
  if (outpath != NULL) free (outpath);
  if (h_in    != h_ioch) oz_sys_handle_release (OZ_PROCMODE_KNL, h_in);
  if (h_out   != 0)      oz_sys_handle_release (OZ_PROCMODE_KNL, h_out);
  if ((sts != OZ_IOFSWILDSCANCONT) && continue_flag) {
    continue_count ++;
    sts = OZ_IOFSWILDSCANCONT;
  }
  return (sts);
}

/************************************************************************/
/*									*/
/*  This routine is called for each input file				*/
/*									*/
/*    Input:								*/
/*									*/
/*	outname = output filename spec (argv[2])			*/
/*	indevname = input device name					*/
/*	inwildcard = input wildcard spec that file matches		*/
/*	ininstance = input file instance spec that matched		*/
/*									*/
/*    Output:								*/
/*									*/
/*	renafile = OZ_IOFSWILDSCANCONT : continue wildcard scan		*/
/*	                          else : stop scanning and return	*/
/*									*/
/************************************************************************/

typedef struct { char devname[OZ_DEVUNIT_NAMESIZE];
                 char filname[OZ_FS_MAXFNLEN];
               } Rppb;

static uLong renameparse (void *rppbv, const char *devname, const char *filname, OZ_Handle h_iochan);

static uLong renafile (void *outname, const char *indevname, const char *inwildcard, const char *ininstance, OZ_Handle h_ioch, uLong fileidsize, void *fileidbuff, OZ_FS_Subs *wildsubs, OZ_FS_Subs *instsubs)

{
  char buff[65536], iname[256], oname[256], *outpath, *p;
  int i, inputisadir;
  OZ_Datebin input_mod_date;
  OZ_Handle h_in, h_out;
  OZ_IO_fs_close fs_close;
  OZ_IO_fs_getinfo1 fs_getinfo1;
  OZ_IO_fs_open fs_open;
  OZ_IO_fs_rename fs_rename;
  Rppb rppb;
  uLong sts;

  outpath = NULL;
  h_in    = 0;
  h_out   = 0;

  /* They're trying to do something like rename OZ_INPUT */

  if (h_ioch != 0) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: can't rename an I/O channel %s\n", pn, ininstance);
    return (OZ_BADPARAM);
  }

  /* Open input file, allow others to read or write it */

  oz_sys_sprintf (sizeof iname, iname, "%s:%s", indevname, ininstance);

  sts = oz_sys_io_assign (OZ_PROCMODE_KNL, &h_in, indevname, OZ_LOCKMODE_CR);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u assigning channel to input device %s\n", pn, sts, indevname);
    goto rtn;
  }

  memset (&fs_open, 0, sizeof fs_open);
  fs_open.name = ininstance;
  fs_open.lockmode = OZ_LOCKMODE_NL;
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_in, 0, OZ_IO_FS_OPEN, sizeof fs_open, &fs_open);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u opening input file %s:%s\n", pn, sts, indevname, ininstance);
    goto rtn;
  }

  memset (&fs_getinfo1, 0, sizeof fs_getinfo1);
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_in, 0, OZ_IO_FS_GETINFO1, sizeof fs_getinfo1, &fs_getinfo1);
  if ((sts != OZ_SUCCESS) && (sts != OZ_BADIOFUNC)) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u getting input file %s:%s characteristics\n", pn, sts, indevname, ininstance);
    goto rtn;
  }

  inputisadir = ((fs_getinfo1.filattrflags & OZ_FS_FILATTRFLAG_DIRECTORY) != 0);

  /* If -update, get the mod date of the input file */

  if (update_flag) {
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u getting input file %s:%s modification date\n", pn, sts, indevname, ininstance);
      goto rtn;
    }
    input_mod_date = fs_getinfo1.modify_date;
  }

  /* Check file selection criteria */

  sts = oz_util_filesel_check (filesel, h_in, ininstance);
  if (sts == OZ_FLAGWASCLR) {
    sts = OZ_IOFSWILDSCANCONT;
    goto rtn;
  }
  if (sts != OZ_FLAGWASSET) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u %s file %s:%s\n", pn, sts, oz_util_filesel_errmsg (filesel), ininstance);
    goto rtn;
  }

  /* Make up the output path = outname + different part between ininstance and inwildcard */

  for (i = 0; ininstance[i] == inwildcard[i]; i ++) if (ininstance[i] == 0) break;

  outpath = malloc (strlen (outname) + strlen (ininstance + i) + 1);
  strcpy (outpath, outname);
  strcat (outpath, ininstance + i);
  if (!outhasvers) {
    i = strlen (outpath);
    outpath[i-instsubs->versize] = 0;
  }

  /* If -update, try to open output file and get its modification date */

  rppb.devname[0] = 0;
  rppb.filname[0] = 0;
  if (update_flag && !inputisadir) {
    memset (&fs_open, 0, sizeof fs_open);
    fs_open.name      = outpath;
    fs_open.lockmode  = OZ_LOCKMODE_NL;
    fs_open.rnamesize = sizeof rppb.filname;
    fs_open.rnamebuff = rppb.filname;
    sts = oz_sys_io_fs_open (sizeof fs_open, &fs_open, 0, &h_out);
    if (sts == OZ_SUCCESS) {
      sts = oz_sys_iochan_getunitname (h_out, sizeof rppb.devname, rppb.devname);
      if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
      memset (&fs_getinfo1, 0, sizeof fs_getinfo1);
      sts = oz_sys_io (OZ_PROCMODE_KNL, h_out, 0, OZ_IO_FS_GETINFO1, sizeof fs_getinfo1, &fs_getinfo1);
      oz_sys_handle_release (OZ_PROCMODE_KNL, h_out);
      h_out = 0;
      if (sts == OZ_SUCCESS) {
        sts = OZ_IOFSWILDSCANCONT;
        if (OZ_HW_DATEBIN_CMP (input_mod_date, fs_getinfo1.modify_date) <= 0) goto rtn;
      }
    }
    if ((sts != OZ_NOSUCHFILE) && (sts != OZ_IOFSWILDSCANCONT)) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u probing output file %s modification date\n", pn, sts, outpath);
      goto rtn;
    }
  }

  /* Otherwise, separate output device and filespec */

  if (rppb.devname[0] == 0) {
    sts = oz_sys_io_fs_parse (outpath, 0, renameparse, &rppb);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u parsing output filespec %s\n", pn, sts, outpath);
      goto rtn;
    }
  }

  /* Make sure input and output are on the same volume */

  if (strcmp (indevname, rppb.devname) != 0) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: can't rename from %s to %s\n", pn, indevname, rppb.devname);
    sts = OZ_CROSSVOLREN;
    goto rtn;
  }

  /* Rename the file or directory */

  memset (&fs_rename, 0, sizeof fs_rename);
  fs_rename.oldname = ininstance;
  fs_rename.newname = rppb.filname;
  fs_rename.newrnamesize = sizeof oname;
  fs_rename.newrnamebuff = oname;
  fs_rename.newversion   = newversion_flag;

  sts = oz_sys_io (OZ_PROCMODE_KNL, h_in, 0, OZ_IO_FS_RENAME, sizeof fs_rename, &fs_rename);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u renaming %s to %s\n", pn, sts, ininstance, rppb.filname);
  } else {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: %s renamed to %s\n", pn, ininstance, oname);
    sts = OZ_IOFSWILDSCANCONT;
  }

  /* Clean up and return status */

rtn:
  if (outpath != NULL) free (outpath);
  if (h_in    != 0)    oz_sys_handle_release (OZ_PROCMODE_KNL, h_in);
  if (h_out   != 0)    oz_sys_handle_release (OZ_PROCMODE_KNL, h_out);
  if ((sts != OZ_IOFSWILDSCANCONT) && continue_flag) {
    continue_count ++;
    sts = OZ_IOFSWILDSCANCONT;
  }
  return (sts);
}

static uLong renameparse (void *rppbv, const char *devname, const char *filname, OZ_Handle h_iochan)

{
  Rppb *rppb;

  if (h_iochan != 0) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: can't rename to an I/O channel\n", pn);
    return (OZ_BADPARAM);
  }
  rppb = rppbv;
  strncpyz (rppb -> devname, devname, sizeof rppb -> devname);
  strncpyz (rppb -> filname, filname, sizeof rppb -> filname);
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Get actual filename that is open on I/O channel			*/
/*									*/
/*    Input:								*/
/*									*/
/*	h = handle that file is open on					*/
/*	argname = filename from the command line argument list		*/
/*									*/
/*    Output:								*/
/*									*/
/*	*name = filled in with actual filename				*/
/*									*/
/************************************************************************/

static void getfname (OZ_Handle h, char name[256], const char *argname)

{
  int i;
  uLong sts;
  OZ_IO_fs_getinfo2 fs_getinfo2;

  sts = oz_sys_iochan_getunitname (h, 255, name);
  if (sts != OZ_SUCCESS) strncpyz (name, argname, 256);
  else {
    i = strlen (name);
    name[i++] = ':';
    memset (&fs_getinfo2, 0, sizeof fs_getinfo2);
    fs_getinfo2.filnamsize =  256 - i;
    fs_getinfo2.filnambuff = name + i;
    sts = oz_sys_io (OZ_PROCMODE_KNL, h, 0, OZ_IO_FS_GETINFO2, sizeof fs_getinfo2, &fs_getinfo2);
    if (sts != OZ_SUCCESS) strncpyz (name + i, argname, 256 - i);
  }
}
