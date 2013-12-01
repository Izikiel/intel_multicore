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
/*  Delete command utiliti						*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_crtl_malloc.h"
#include "oz_io_fs.h"
#include "oz_knl_lock.h"
#include "oz_knl_status.h"
#include "oz_sys_handle.h"
#include "oz_sys_io.h"
#include "oz_sys_io_fs.h"
#include "oz_sys_io_fs_printf.h"
#include "oz_util_filesel.h"
#include "oz_util_start.h"

typedef struct Spec Spec;
struct Spec { Spec *next;
              char name[1];
            };

static char *lastname, *pn;
static int continue_count, continue_flag;
static int keep_count, opt_keep;
static OZ_Dbn numblocks;
static OZ_Filesel *filesel;
static uLong numfiles;

static uLong deleteafile (void *dummy, const char *devname, const char *wildcard, const char *instance, OZ_Handle h_ioch, uLong fileidsize, void *fileidbuff, OZ_FS_Subs *wildsubs, OZ_FS_Subs *instsubs);

uLong oz_util_main (int argc, char *argv[])

{
  char *wc;
  int i, rc, usedup, what_unused;
  Spec **lspec, *spec, *specs;
  uLong sts;

  /* If program name is 'delete', default -keep to zero */
  /* If program name is 'purge', default -keep to one   */

  opt_keep = 1;
  pn = "purge";
  if (argc > 0) {
    pn = argv[0];
    if (strcmp (pn, "delete") == 0) opt_keep = 0;
    else if (strcmp (pn, "purge") != 0) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: program name must be either 'delete' or 'purge'\n", argv[0]);
      return (OZ_BADPARAM);
    }
  }

  /* Scan the command line for options */

  continue_count = 0;
  continue_flag  = 0;
  lspec   = &specs;						// haven't found any specs yet
  filesel = oz_util_filesel_init ();				// init the filesel option parser routines
  for (i = 1; i < argc; i ++) {					// scan through command line
    if (argv[i][0] != '-') {					// check for filespec
      spec = malloc (sizeof *spec + strlen (argv[i]));		// if found, add it to list
      *lspec = spec;
      lspec = &(spec -> next);
      strcpy (spec -> name, argv[i]);
      continue;
    }
    rc = oz_util_filesel_parse (filesel, argc - i, argv + i);	// see if it is a filesel option
    if (rc < 0) goto usage_filesel;				// - a bad one, print out usage message
    if (rc > 0) {						// - a good one, skip over it
      i += rc - 1;
      continue;
    }
    if (strcmp (argv[i] + 1, "continue") == 0) {		// check for -continue
      continue_flag = 1;					// ok, set flag
      continue;
    }
    if (strcmp (argv[i] + 1, "keep") == 0) {			// check for -keep
      if (++ i >= argc) goto usagex;				// ok, get number of versions to keep
      opt_keep = oz_hw_atoi (argv[i], &usedup);
      if (argv[i][usedup] != 0) goto usage;
      continue;
    }
    goto usage;							// unknown option, print usage message
  }
  if (oz_util_filesel_parse (filesel, 0, NULL) < 0) goto usage_filesel;
  *lspec = NULL;

  if (specs == NULL) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: no files specified\n", pn);
    return (OZ_MISSINGPARAM);
  }

  /* Process the command */

  numblocks = 0;
  numfiles  = 0;

  lastname = NULL;						// haven't done any files yet
  while ((spec = specs) != NULL) {				// scan through wildcard specs
    specs = spec -> next;					// unlink and process
    sts = oz_sys_io_fs_wildscan3 (spec -> name, OZ_SYS_IO_FS_WILDSCAN_DELAYDIR, NULL, deleteafile, NULL);
    if (sts == OZ_IOFSPARSECONT) sts = OZ_SUCCESS;
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u processing %s\n", pn, sts, spec -> name);
      break;
    }
  }

  if (numfiles > 1) oz_sys_io_fs_printf (oz_util_h_error, "%s: %u files deleted (%u blocks)\n", pn, numfiles, numblocks);
  if ((sts == OZ_SUCCESS) && (continue_count != 0)) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: %d error%s\n", pn, continue_count, (continue_count != 1) ? "s" : "");
    sts = OZ_CONTONERROR;
  }

  return (sts);

usage_filesel:
  oz_sys_io_fs_printf (oz_util_h_error, "%s: invalid argument[%d] = '%s': %s\n", pn, i, argv[i], oz_util_filesel_errmsg (filesel));
  goto usagey;
usagex:
  -- i;
usage:
  oz_sys_io_fs_printf (oz_util_h_error, "%s: invalid argument[%d] = '%s'\n", pn, i, argv[i]);
usagey:
  oz_sys_io_fs_printf (oz_util_h_error, "usage: %s [-continue] [-keep <n>] %s <files...>\n", pn, oz_util_filesel_usage);
  return (OZ_BADPARAM);
}

/* This routine is called for each file that matches the wildcard spec */

static uLong deleteafile (void *dummy, const char *devname, const char *wildcard, const char *instance, OZ_Handle h_ioch, uLong fileidsize, void *fileidbuff, OZ_FS_Subs *wildsubs, OZ_FS_Subs *instsubs)

{
  OZ_Handle h_file;
  OZ_IO_fs_getinfo1 fs_getinfo1;
  OZ_IO_fs_open fs_open;
  OZ_IO_fs_remove fs_remove;
  uLong sts;

  h_file = 0;

  /* Trying to do something like delete OZ_INPUT */

  if (h_ioch != 0) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: can't delete I/O channel %s\n", pn, instance);
    sts = OZ_BADPARAM;
    goto rtnsts;
  }

  /* Assign an channel to the disk drive file system */

  sts = oz_sys_io_assign (OZ_PROCMODE_KNL, &h_file, devname, OZ_LOCKMODE_CW);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u assigning channel for file %s:%s\n", pn, sts, devname, instance);
    goto rtnsts;
  }

  /* Open the file first to check selection options */

  memset (&fs_open, 0, sizeof fs_open);
  if (fileidsize != 0) {
    fs_open.fileidsize = fileidsize;
    fs_open.fileidbuff = fileidbuff;
  } else {
    fs_open.name       = instance;
  }
  fs_open.lockmode = OZ_LOCKMODE_NL;
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_file, 0, OZ_IO_FS_OPEN, sizeof fs_open, &fs_open);
  if ((sts != OZ_SUCCESS) && (sts != OZ_FILEDELETED)) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u opening file %s:%s\n", pn, sts, devname, instance);
  }

  /* Check file selection criteria */

  sts = oz_util_filesel_check (filesel, h_file, instance);
  if (sts == OZ_FLAGWASCLR) goto rtnclose;
  if (sts != OZ_FLAGWASSET) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u %s file %s:%s\n", pn, sts, oz_util_filesel_errmsg (filesel), instance);
    goto rtnsts;
  }

  /* If -keep, determine if we should delete it or not */

  if (opt_keep > 0) {

    /* If directory, name or type is different, reset keep_count */

    sts = instsubs -> dirsize + instsubs -> namsize + instsubs -> typsize;
    if ((lastname == NULL) || (lastname[sts] != 0) || (memcmp (lastname, instance, sts) != 0)) {
      if (lastname != NULL) free (lastname);
      lastname = malloc (sts + 1);
      memcpy (lastname, instance, sts);
      lastname[sts] = 0;
      keep_count    = 0;
    }

    /* Increment keep_count then don't delete if we */
    /* haven't had enough versions of the file yet  */

    if (++ keep_count <= opt_keep) goto rtnclose;
  }

  /* Get number of blocks in the file */

  memset (&fs_getinfo1, 0, sizeof fs_getinfo1);
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_file, 0, OZ_IO_FS_GETINFO1, sizeof fs_getinfo1, &fs_getinfo1);
  if (sts != OZ_SUCCESS) fs_getinfo1.hiblock = 0;

  /* Delete file - this actually just removes it from the directory. */
  /* It also deletes it if it is no longer entered in any directory. */

  memset (&fs_remove, 0, sizeof fs_remove);
  fs_remove.name = instance;
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_file, 0, OZ_IO_FS_REMOVE, sizeof fs_remove, &fs_remove);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u deleting file %s:%s\n", pn, sts, devname, instance);
    goto rtnsts;
  }

  numfiles ++;
  numblocks += fs_getinfo1.hiblock;
  if (fs_getinfo1.hiblock == 0) oz_sys_io_fs_printf (oz_util_h_error, "%s: file %s:%s deleted\n", pn, devname, instance);
  else oz_sys_io_fs_printf (oz_util_h_error, "%s: file %s:%s deleted (%u block%s)\n", pn, devname, instance, fs_getinfo1.hiblock, (fs_getinfo1.hiblock == 1) ? "" : "s");

rtnclose:
  sts = OZ_IOFSWILDSCANCONT;
rtnsts:
  oz_sys_handle_release (OZ_PROCMODE_KNL, h_file);
  if ((sts != OZ_IOFSWILDSCANCONT) && continue_flag) {
    continue_count ++;
    sts = OZ_IOFSWILDSCANCONT;
  }
  return (sts);
}
