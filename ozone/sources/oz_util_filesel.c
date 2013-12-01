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
/*  File selection routine						*/
/*									*/
/************************************************************************/

#include <stdarg.h>

#include "ozone.h"
#include "oz_crtl_malloc.h"
#include "oz_io_fs.h"
#include "oz_knl_hw.h"
#include "oz_knl_status.h"
#include "oz_sys_dateconv.h"
#include "oz_sys_handle.h"
#include "oz_sys_io.h"
#include "oz_sys_io_fs.h"
#include "oz_sys_xprintf.h"
#include "oz_util_filesel.h"

struct OZ_Filesel { OZ_Datebin *opt_date_what;		// points to date field in fs_getinfo1 for last -whatevered seen
                    const char *what_unused;		// NULL means the -whatevered was used, else points to -whatevered string
                    OZ_Datebin opt_before_when;		// date from -before option
                    OZ_Datebin *opt_before_what;	// -before date's -whatevered option (points to field in fs_getinfo1 struct)
                    OZ_Datebin opt_since_when;		// date from -since option
                    OZ_Datebin *opt_since_what;		// -since date's -whatevered option (points to field in fs_getinfo1 struct)
                    OZ_IO_fs_getinfo1 fs_getinfo1;	// the file's attributes that we will check
                    char *errmsg;			// malloc'd error message string pointer
                  };

const char *const oz_util_filesel_usage = "[-accessed] [-archived] [-backedup] [-before <date>] [-changed] [-created] [-expired] [-modified] [-since <date>]";

static int datebin_encstr (const char *argv, OZ_Datebin *datebin_r, OZ_Datebin nowutc);
static void seterrmsg (OZ_Filesel *filesel, int extra, const char *format, ...);

/************************************************************************/
/*									*/
/*  Allocate an filesel context block					*/
/*									*/
/************************************************************************/

OZ_Filesel *oz_util_filesel_init (void)

{
  OZ_Filesel *filesel;

  filesel = malloc (sizeof *filesel);
  memset (filesel, 0, sizeof *filesel);

  filesel -> opt_date_what = &(filesel -> fs_getinfo1.create_date);

  return (filesel);
}

/************************************************************************/
/*									*/
/*  Retrieve pointer to error message text				*/
/*									*/
/************************************************************************/

const char *oz_util_filesel_errmsg (OZ_Filesel *filesel)

{
  return (filesel -> errmsg);
}

/************************************************************************/
/*									*/
/*  Free off filesel context block					*/
/*									*/
/************************************************************************/

void oz_util_filesel_term (OZ_Filesel *filesel)

{
  if (filesel -> errmsg != NULL) free (filesel -> errmsg);
  free (filesel);
}

/************************************************************************/
/*									*/
/*  Process next argument						*/
/*									*/
/*    Input:								*/
/*									*/
/*	filesel = as returned by oz_util_filesel_init			*/
/*	argc = 0: no more args to process, check for errors		*/
/*	    else: number of arguments remaining to be processed		*/
/*	argv = points to next element to be processed			*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_util_filesel_parse = -1: error detected			*/
/*	                      else: number of arguments processed	*/
/*	*filesel = updated						*/
/*									*/
/************************************************************************/

int oz_util_filesel_parse (OZ_Filesel *filesel, int argc, char *argv[])

{
  int rc;
  OZ_Datebin now;

  if (argc == 0) {
    if (filesel -> what_unused != NULL) {
      seterrmsg (filesel, strlen (filesel -> what_unused), "%s must be followed by -before or -since", filesel -> what_unused);
      return (-1);
    }
    return (0);
  }

  now = oz_hw_tod_getnow ();

  if (strcmp (argv[0], "-accessed") == 0) {
    filesel -> opt_date_what = &(filesel -> fs_getinfo1.access_date);
    filesel -> what_unused   = argv[0];
    return (1);
  }
  if (strcmp (argv[0], "-archived") == 0) {
    filesel -> opt_date_what = &(filesel -> fs_getinfo1.archive_date);
    filesel -> what_unused   = argv[0];
    return (1);
  }
  if (strcmp (argv[0], "-backedup") == 0) {
    filesel -> opt_date_what = &(filesel -> fs_getinfo1.backup_date);
    filesel -> what_unused   = argv[0];
    return (1);
  }
  if (strcmp (argv[0], "-before") == 0) {
    if (argc < 2) {
      seterrmsg (filesel, 0, "missing date after -before");
      return (-1);
    }
    rc = datebin_encstr (argv[1], &(filesel -> opt_before_when), now);
    if (rc == 0) {
      seterrmsg (filesel, 0, "invalid or missing -before date");
      return (-1);
    }
    filesel -> opt_before_what = filesel -> opt_date_what;
    filesel -> what_unused     = 0;
    return (2);
  }
  if (strcmp (argv[0], "-changed") == 0) {
    filesel -> opt_date_what = &(filesel -> fs_getinfo1.change_date);
    filesel -> what_unused   = argv[0];
    return (1);
  }
  if (strcmp (argv[0], "-created") == 0) {
    filesel -> opt_date_what = &(filesel -> fs_getinfo1.create_date);
    filesel -> what_unused   = argv[0];
    return (1);
  }
  if (strcmp (argv[0], "-expired") == 0) {
    filesel -> opt_date_what = &(filesel -> fs_getinfo1.expire_date);
    filesel -> what_unused   = argv[0];
    return (1);
  }
  if (strcmp (argv[0], "-modified") == 0) {
    filesel -> opt_date_what = &(filesel -> fs_getinfo1.modify_date);
    filesel -> what_unused   = argv[0];
    return (1);
  }
  if (strcmp (argv[0], "-since") == 0) {
    if (argc < 2) {
      seterrmsg (filesel, 0, "missing date after -since");
      return (-1);
    }
    rc = datebin_encstr (argv[1], &(filesel -> opt_since_when), now);
    if (rc == 0) {
      seterrmsg (filesel, 0, "invalid or missing -since date");
      return (-1);
    }
    filesel -> opt_since_what = filesel -> opt_date_what;
    filesel -> what_unused    = 0;
    return (2);
  }

  /* Nothing I know about */

  return (0);
}

/* Convert date string 'argv' to binary, interpreting string as absolute local time or delta time in the past */

static int datebin_encstr (const char *argv, OZ_Datebin *datebin_r, OZ_Datebin nowutc)

{
  int rc;
  OZ_Datebin nowlcl;

  nowlcl = oz_sys_datebin_tzconv (nowutc, OZ_DATEBIN_TZCONV_UTC2LCL, 0);
  rc = oz_sys_datebin_encstr2 (strlen (argv), argv, datebin_r, nowlcl);
  if (rc < 0) *datebin_r = nowutc - *datebin_r;
  if (rc > 0) *datebin_r = oz_sys_datebin_tzconv (*datebin_r, OZ_DATEBIN_TZCONV_LCL2UTC, 0);

  return (rc);
}

/************************************************************************/
/*									*/
/*  Check the file against selection criteria				*/
/*									*/
/*    Input:								*/
/*									*/
/*	filesel = as returned by oz_util_filesel_init			*/
/*	h_file  = handle with file open on it				*/
/*	fspec   = filespec						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_util_filesel_check = OZ_FLAGWASCLR : file not selected	*/
/*	                        OZ_FLAGWASSET : file is selected	*/
/*	                                 else : error status		*/
/*									*/
/************************************************************************/

uLong oz_util_filesel_check (OZ_Filesel *filesel, OZ_Handle h_file, const char *fspec)

{
  uLong sts;

  /* Maybe we need to get info about the file to check some options */

  if ((filesel -> opt_before_when != 0) || (filesel -> opt_since_when != 0)) {
    memset (&(filesel -> fs_getinfo1), 0, sizeof filesel -> fs_getinfo1);
    sts = oz_sys_io (OZ_PROCMODE_KNL, h_file, 0, OZ_IO_FS_GETINFO1, sizeof filesel -> fs_getinfo1, &(filesel -> fs_getinfo1));
    if (sts != OZ_SUCCESS) {
      seterrmsg (filesel, 0, "getting file info");
      return (sts);
    }
  }

  /* If -before specified, make sure the file was whatevered before the given date/time */

  if ((filesel -> opt_before_when != 0) && (*(filesel -> opt_before_what) >= filesel -> opt_before_when)) return (OZ_FLAGWASCLR);

  /* If -since specified, make sure the file was whatevered since the given date/time */

  if ((filesel -> opt_since_when != 0) && (*(filesel -> opt_since_what) < filesel -> opt_since_when)) return (OZ_FLAGWASCLR);

  /* It passes all tests, process it */

  return (OZ_FLAGWASSET);
}

/************************************************************************/
/*									*/
/*  Set the error message string in the filesel context block		*/
/*									*/
/************************************************************************/

static void seterrmsg (OZ_Filesel *filesel, int extra, const char *format, ...)

{
  va_list ap;

  if (filesel -> errmsg != NULL) free (filesel -> errmsg);
  filesel -> errmsg = malloc (strlen (format) + extra);
  va_start (ap, format);
  oz_sys_vsprintf (strlen (format) + extra, filesel -> errmsg, format, ap);
  va_end (ap);
}
