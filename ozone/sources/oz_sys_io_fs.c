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
/*  Filesystem I/O routines						*/
/*									*/
/*  They can take logical names as filename specs and they process the 	*/
/*  default directory, either supplied or taken from OZ_DEFAULT_DIR	*/
/*									*/
/************************************************************************/

#include "ozone.h"

#include "oz_io_fs.h"
#include "oz_knl_lock.h"
#include "oz_knl_status.h"
#include "oz_sys_event.h"
#include "oz_sys_handle.h"
#include "oz_sys_io.h"
#include "oz_sys_io_fs.h"
#include "oz_sys_io_fs_printf.h"
#include "oz_sys_logname.h"

typedef struct { int fs_create_len;
                 OZ_IO_fs_create *fs_create;
                 const char *name;
                 OZ_Handle h_iochan;
               } Iofscrepar;

typedef struct { int fs_open_len;
                 OZ_IO_fs_open *fs_open;
                 const char *name;
                 OZ_Handle h_iochan;
               } Iofsopnpar;

#define OZ_LOGNAME_MAXLEVEL 8

static uLong io_fs_create (void *iofscreparv, const char *devname, const char *filname, OZ_Handle h_iochan);
static uLong io_fs_open (void *iofsopnparv, const char *devname, const char *filname, OZ_Handle h_iochan);
static uLong io_fs_wildscan (void *wsctxv, const char *devname, const char *wildcard, OZ_Handle h_iochan);
static uLong io_fs_parse (const char *name, int terminal, const char *def_dir, OZ_Procmode lnmprocmode, int level, uLong (*entry) (void *param, const char *devname, const char *filname, OZ_Handle h_iochan), void *param);

/************************************************************************/
/*									*/
/*  Create an new file							*/
/*									*/
/*    Input:								*/
/*									*/
/*	fs_create = file create parameters				*/
/*	terminal  = 0 : the 'name' might contain a logical name		*/
/*	            1 : do not attempt to translate 'name' as a logical	*/
/*	                it contains the device and filename as is	*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_io_fs_create = resultant status (error message already output)
/*	*h_iochan_r = i/o channel handle				*/
/*									*/
/************************************************************************/

uLong oz_sys_io_fs_create (int fs_create_len, OZ_IO_fs_create *fs_create, int terminal, OZ_Handle *h_iochan_r)

{
  return (oz_sys_io_fs_create2 (fs_create_len, fs_create, terminal, NULL, h_iochan_r));
}

uLong oz_sys_io_fs_create2 (int fs_create_len, OZ_IO_fs_create *fs_create, int terminal, const char *def_dir, OZ_Handle *h_iochan_r)

{
  Iofscrepar iofscrepar;
  uLong sts;

  iofscrepar.fs_create_len = fs_create_len;
  iofscrepar.fs_create     = fs_create;
  iofscrepar.name          = fs_create -> name;

  *h_iochan_r = 0;

  sts = oz_sys_io_fs_parse2 (iofscrepar.name, terminal, def_dir, io_fs_create, &iofscrepar);
  fs_create -> name = iofscrepar.name;
  if (sts == OZ_SUCCESS) *h_iochan_r = iofscrepar.h_iochan;
  return (sts);
}

static uLong io_fs_create (void *iofscreparv, const char *devname, const char *filname, OZ_Handle h_iochan)

{
  Iofscrepar *iofscrepar;
  uLong sts;

  iofscrepar = iofscreparv;

  /* If it was an I/O channel object name, just use that */

  if (h_iochan != 0) {
    iofscrepar -> h_iochan = h_iochan;
    return (OZ_SUCCESS);
  }

  /* Assign I/O channel to the device */

  sts = oz_sys_io_fs_assign (devname, iofscrepar -> fs_create -> lockmode, &(iofscrepar -> h_iochan));
  if (sts != OZ_SUCCESS) return (sts);

  /* Create the file */

  iofscrepar -> fs_create -> name = filname;
  sts = oz_sys_io (OZ_PROCMODE_KNL, iofscrepar -> h_iochan, 0, OZ_IO_FS_CREATE, iofscrepar -> fs_create_len, iofscrepar -> fs_create);
  if (sts != OZ_SUCCESS) {
    if (sts == OZ_NOSUCHFILE) sts = OZ_IOFSPARSECONT;
    oz_sys_handle_release (OZ_PROCMODE_KNL, iofscrepar -> h_iochan);
  }
  return (sts);
}

/************************************************************************/
/*									*/
/*  Open an existing file						*/
/*									*/
/*    Input:								*/
/*									*/
/*	fs_open  = file open parameters					*/
/*	terminal = 0 : the 'name' might contain a logical name		*/
/*	           1 : do not attempt to translate 'name' as a logical	*/
/*	               it contains the device and filename as is	*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_io_fs_open = resultant status (error message already output)
/*	*h_iochan_r = i/o channel handle				*/
/*									*/
/************************************************************************/

uLong oz_sys_io_fs_open (int fs_open_len, OZ_IO_fs_open *fs_open, int terminal, OZ_Handle *h_iochan_r)

{
  return (oz_sys_io_fs_open2 (fs_open_len, fs_open, terminal, NULL, h_iochan_r));
}

uLong oz_sys_io_fs_open2 (int fs_open_len, OZ_IO_fs_open *fs_open, int terminal, const char *def_dir, OZ_Handle *h_iochan_r)

{
  Iofsopnpar iofsopnpar;
  uLong sts;

  iofsopnpar.fs_open_len = fs_open_len;
  iofsopnpar.fs_open     = fs_open;
  iofsopnpar.name        = fs_open -> name;

  *h_iochan_r = 0;

  sts = oz_sys_io_fs_parse2 (iofsopnpar.name, terminal, def_dir, io_fs_open, &iofsopnpar);
  fs_open -> name = iofsopnpar.name;
  if (sts == OZ_SUCCESS) *h_iochan_r = iofsopnpar.h_iochan;
  if (sts == OZ_IOFSPARSECONT) sts = OZ_NOSUCHFILE;
  return (sts);
}

static uLong io_fs_open (void *iofsopnparv, const char *devname, const char *filname, OZ_Handle h_iochan)

{
  Iofsopnpar *iofsopnpar;
  uLong sts;

  iofsopnpar = iofsopnparv;

  /* If it was an I/O channel object name, just use that */

  if (h_iochan != 0) {
    iofsopnpar -> h_iochan = h_iochan;
    return (OZ_SUCCESS);
  }

  /* Assign I/O channel to the device */

  sts = oz_sys_io_fs_assign (devname, iofsopnpar -> fs_open -> lockmode, &(iofsopnpar -> h_iochan));
  if (sts != OZ_SUCCESS) return (sts);

  /* Open the file */

  iofsopnpar -> fs_open -> name = filname;
  sts = oz_sys_io (OZ_PROCMODE_KNL, iofsopnpar -> h_iochan, 0, OZ_IO_FS_OPEN, iofsopnpar -> fs_open_len, iofsopnpar -> fs_open);
  if (sts != OZ_SUCCESS) {
    if (sts == OZ_NOSUCHFILE) sts = OZ_IOFSPARSECONT;
    oz_sys_handle_release (OZ_PROCMODE_KNL, iofsopnpar -> h_iochan);
  }
  return (sts);
}

/************************************************************************/
/*									*/
/*  Scan through wildcard specification					*/
/*									*/
/*    Input:								*/
/*									*/
/*	name = wildcard spec to process					*/
/*	terminal = 0 : the 'name' might contain a logical name		*/
/*	           1 : do not attempt to translate 'name' as a logical	*/
/*	               it contains the device and filename as is	*/
/*	entry = routine to call with each successive match		*/
/*	param = parameter to pass to 'entry' routine			*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_io_fs_wildscan = resultant status			*/
/*	                        (OZ_IOFSPARSECONT is normal end)	*/
/*									*/
/*    Note:								*/
/*									*/
/*	entry routine called with:					*/
/*									*/
/*	    Input:							*/
/*									*/
/*		param      = as passed to oz_sys_io_fs_wildscan		*/
/*		devname    = device name string				*/
/*		wildcard   = wildcard file spec string			*/
/*		instance   = resultant file spec string			*/
/*		h_iochan   = I/O channel if wildspec was an iochan	*/
/*		fileidsize = size of fileidbuff				*/
/*		fileidbuff = pointer to fileid				*/
/*									*/
/*	    Output:							*/
/*									*/
/*		(*entry) = OZ_IOFSWILDSCANCONT : continue on to next value
/*		                          else : return with this status
/*									*/
/************************************************************************/

typedef struct { uLong (*entry) (void *param, const char *devname, const char *wildcard, const char *instance, OZ_Handle h_ioch, uLong fileidsize, void *fileidbuff, OZ_FS_Subs *wildsubs, OZ_FS_Subs *instsubs);
                 void *param;
                 const char *name;
                 uLong options;
                 OZ_Handle h_event;
               } Wsctx;

uLong oz_sys_io_fs_wildscan (const char *name, int terminal, uLong (*entry) (void *param, const char *devname, const char *wildcard, const char *instance, OZ_Handle h_ioch, uLong fileidsize, void *fileidbuff, OZ_FS_Subs *wildsubs, OZ_FS_Subs *instsubs), void *param)

{
  return (oz_sys_io_fs_wildscan3 (name, (terminal != 0) ? OZ_SYS_IO_FS_WILDSCAN_TERMINAL : 0, NULL, entry, param));
}

uLong oz_sys_io_fs_wildscan2 (const char *name, int terminal, const char *def_dir, uLong (*entry) (void *param, const char *devname, const char *wildcard, const char *instance, OZ_Handle h_ioch, uLong fileidsize, void *fileidbuff, OZ_FS_Subs *wildsubs, OZ_FS_Subs *instsubs), void *param)

{
  return (oz_sys_io_fs_wildscan3 (name, (terminal != 0) ? OZ_SYS_IO_FS_WILDSCAN_TERMINAL : 0, NULL, entry, param));
}

uLong oz_sys_io_fs_wildscan3 (const char *name, uLong options, const char *def_dir, uLong (*entry) (void *param, const char *devname, const char *wildcard, const char *instance, OZ_Handle h_ioch, uLong fileidsize, void *fileidbuff, OZ_FS_Subs *wildsubs, OZ_FS_Subs *instsubs), void *param)

{
  uLong sts;
  Wsctx wsctx;

  wsctx.entry   = entry;
  wsctx.param   = param;
  wsctx.name    = name;
  wsctx.options = options;
  sts = oz_sys_event_create (OZ_PROCMODE_KNL, "oz_sys_io_fs_wildscan", &wsctx.h_event);
  if (sts == OZ_SUCCESS) {
    sts = oz_sys_io_fs_parse2 (name, ((options & OZ_SYS_IO_FS_WILDSCAN_TERMINAL) != 0), def_dir, io_fs_wildscan, &wsctx);
    oz_sys_handle_release (OZ_PROCMODE_KNL, wsctx.h_event);
  }
  return (sts);
}

static uLong io_fs_wildscan (void *wsctxv, const char *devname, const char *wildcard, OZ_Handle h_ioch)

{
  char nambuf[OZ_FS_MAXFNLEN], wildcardbuff[OZ_FS_MAXFNLEN];
  OZ_FS_Subs instsubs, wildsubs;
  OZ_Handle h_iochan;
  OZ_IO_fs_getinfo3 fs_getinfo3;
  OZ_IO_fs_wildscan fs_wildscan;
  Wsctx *wsctx;
  uByte fileidbuff[OZ_FS_MAXFIDLN];
  uLong sts;

  wsctx = wsctxv;

  /* If I/O channel, it's the one-and-only 'file' */

  if (h_ioch != 0) {
    sts = (*(wsctx -> entry)) (wsctx -> param, "", wsctx -> name, wsctx -> name, h_ioch, 0, NULL, NULL, NULL);
    if (sts == OZ_IOFSWILDSCANCONT) sts = OZ_IOFSPARSECONT;
    goto done;
  }

  /* Assign I/O channel to device */

  sts = oz_sys_io_assign (OZ_PROCMODE_KNL, &h_iochan, devname, OZ_LOCKMODE_CR);
  if (sts != OZ_SUCCESS) return (sts);

  /* Get fileid buffer size */

  memset (&fs_getinfo3, 0, sizeof fs_getinfo3);
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_iochan, wsctx -> h_event, OZ_IO_FS_GETINFO3, sizeof fs_getinfo3, &fs_getinfo3);
  if ((sts != OZ_SUCCESS) || (fs_getinfo3.fileidsize > sizeof fileidbuff)) fs_getinfo3.fileidsize = 0;

  /* Init wildcard searching context */

  memset (&wildsubs, 0, sizeof wildsubs);
  memset (&instsubs, 0, sizeof instsubs);

  memset (&fs_wildscan, 0, sizeof fs_wildscan);
  fs_wildscan.wild       = wildcard;
  fs_wildscan.init       = 1;
  fs_wildscan.size       = sizeof nambuf;
  fs_wildscan.buff       = nambuf;
  fs_wildscan.wildsize   = sizeof wildcardbuff;
  fs_wildscan.wildbuff   = wildcardbuff;
  fs_wildscan.delaydir   = ((wsctx -> options & OZ_SYS_IO_FS_WILDSCAN_DELAYDIR) != 0);
  fs_wildscan.dirlist    = ((wsctx -> options & OZ_SYS_IO_FS_WILDSCAN_DIRLIST)  != 0);
  fs_wildscan.fileidsize = fs_getinfo3.fileidsize;
  fs_wildscan.fileidbuff = fileidbuff;
  fs_wildscan.wildsubs   = &wildsubs;
  fs_wildscan.subs       = &instsubs;
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_iochan, wsctx -> h_event, OZ_IO_FS_WILDSCAN, sizeof fs_wildscan, &fs_wildscan);

  /* If it doesn't know how to wildscan, then that's the one-and-only 'file' */

  if (sts == OZ_BADIOFUNC) {
    sts = (*(wsctx -> entry)) (wsctx -> param, devname, wildcard, wildcard, 0, 0, NULL, NULL, NULL);
    if (sts == OZ_IOFSWILDSCANCONT) sts = OZ_IOFSPARSECONT;
    goto done;
  }

  /* Continue processing as long as callback returns OZ_IOFSWILDSCANCONT and the search returns more filenames to process */

  fs_wildscan.init = 0;
  while (sts == OZ_SUCCESS) {
    sts = (*(wsctx -> entry)) (wsctx -> param, devname, wildcardbuff, nambuf, 0, fs_wildscan.fileidsize, fileidbuff, &wildsubs, &instsubs);
    if (sts != OZ_IOFSWILDSCANCONT) goto done;
    sts = oz_sys_io (OZ_PROCMODE_KNL, h_iochan, wsctx -> h_event, OZ_IO_FS_WILDSCAN, sizeof fs_wildscan, &fs_wildscan);
  }
  if (sts == OZ_ENDOFFILE) sts = OZ_IOFSPARSECONT;

  /* Release the I/O channel to the device */

done:
  oz_sys_handle_release (OZ_PROCMODE_KNL, h_iochan);

  return (sts);
}

/************************************************************************/
/*									*/
/*  Parse a file spec into device name and file name			*/
/*									*/
/*    Input:								*/
/*									*/
/*	name = file spec to parse					*/
/*	terminal = 0 : the 'name' might contain a logical name		*/
/*	           1 : do not attempt to translate 'name' as a logical	*/
/*	               it contains the device and filename as is	*/
/*	def_dir = default directory spec				*/
/*	lnmprocmode = outermost processor mode for logical names	*/
/*	entry = routine to call with each successive parse		*/
/*	param = parameter to pass to 'entry' routine			*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_io_fs_parse = resultant status				*/
/*									*/
/*    Note:								*/
/*									*/
/*	entry routine called with:					*/
/*									*/
/*	    Input:							*/
/*									*/
/*		param    = as passed to oz_sys_io_fs_parse		*/
/*		devname  = device name string				*/
/*		filname  = file name string				*/
/*		h_iochan = I/O channel already assigned and opened	*/
/*									*/
/*	    Output:							*/
/*									*/
/*		(*entry) = OZ_IOFSPARSECONT : continue on to next value	*/
/*		                       else : return with this status	*/
/*									*/
/*	    Note:							*/
/*									*/
/*		If h_iochan is zero, devname and filname will not be 	*/
/*		NULL.  If h_iochan is non-zero, devname and filname 	*/
/*		will be NULL.						*/
/*									*/
/************************************************************************/

uLong oz_sys_io_fs_parse (const char *name, int terminal, uLong (*entry) (void *param, const char *devname, const char *filname, OZ_Handle h_iochan), void *param)

{
  return (io_fs_parse (name, terminal, NULL, OZ_PROCMODE_USR, 0, entry, param));
}

uLong oz_sys_io_fs_parse2 (const char *name, int terminal, const char *def_dir, uLong (*entry) (void *param, const char *devname, const char *filname, OZ_Handle h_iochan), void *param)

{
  return (io_fs_parse (name, terminal, def_dir, OZ_PROCMODE_USR, 0, entry, param));
}

uLong oz_sys_io_fs_parse3 (const char *name, int terminal, const char *def_dir, OZ_Procmode lnmprocmode, uLong (*entry) (void *param, const char *devname, const char *filname, OZ_Handle h_iochan), void *param)

{
  return (io_fs_parse (name, terminal, def_dir, lnmprocmode, 0, entry, param));
}

static uLong io_fs_parse (const char *name, int terminal, const char *def_dir, OZ_Procmode lnmprocmode, int level, uLong (*entry) (void *param, const char *devname, const char *filname, OZ_Handle h_iochan), void *param)

{
  char nambuf[OZ_DEVUNIT_NAMESIZE+OZ_FS_MAXFNLEN];
  const char *p, *q;
  int fnlen;
  uLong ivalue, logvalatr, nvalues, sts;
  OZ_Handle h_defaulttbl, h_iochan, h_logname;

  if (def_dir == NULL) def_dir = "OZ_DEFAULT_DIR";

  /* Get device name in 'nambuf', and point 'p' at the filename */

  q = "";
  if (terminal) {
    p = strchr (name, ':');
    if (p == NULL) {
      oz_sys_io_fs_printerror ("oz_sys_io_fs_parse: missing : in filename %s\n", name);
      return (OZ_BADFILENAME);
    }
    if (p - name >= sizeof nambuf) {
      oz_sys_io_fs_printerror ("oz_sys_io_fs_parse: device name in %s too long\n", name);
      return (OZ_EXMAXFILENAME);
    }
    memcpy (nambuf, name, p - name);
    nambuf[p-name] = 0;
    p ++;
  }

  else if (level > OZ_LOGNAME_MAXLEVEL) return (OZ_EXMAXLOGNAMLVL);
  else {

    /* Not terminal, try to translate the whole thing as a logical name */

    sts = oz_sys_logname_lookup (0, lnmprocmode, "OZ_DEFAULT_TBL", NULL, NULL, NULL, &h_defaulttbl);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printerror ("oz_sys_io_fs_parse: error %u looking up OZ_DEFAULT_TBL\n", sts);
      return (sts);
    }

    sts = oz_sys_logname_lookup (h_defaulttbl, lnmprocmode, name, NULL, NULL, &nvalues, &h_logname);
    if (sts == OZ_SUCCESS) {

      /* Try to open each successive value until we found the file */

      sts = OZ_NOLOGNAME;
      for (ivalue = 0; ivalue < nvalues; ivalue ++) {

        sts = oz_sys_logname_getval (h_logname, ivalue, &logvalatr, sizeof nambuf, nambuf, NULL, &h_iochan, 0, NULL);
        if (sts != OZ_SUCCESS) {
          oz_sys_io_fs_printerror ("oz_sys_io_fs_parse: error %u getting logical %s value %u\n", sts, name, ivalue);
          break;
        }

        /* If it is an I/O channel object, just use it as is */

        if (logvalatr & OZ_LOGVALATR_OBJECT) {
          sts = (*entry) (param, NULL, NULL, h_iochan);
          break;
        }

        /* Try to open it, keep looping if failure */

        sts = io_fs_parse (nambuf, (logvalatr & OZ_LOGVALATR_TERMINAL) != 0, def_dir, lnmprocmode, level + 1, entry, param);
        if (sts != OZ_IOFSPARSECONT) break;
      }

      /* Return final open status */

      oz_sys_handle_release (OZ_PROCMODE_KNL, h_logname);
      oz_sys_handle_release (OZ_PROCMODE_KNL, h_defaulttbl);
      return (sts);
    }

    /* Whole thing is not a logical name, separate device and filename into 'nambuf' and 'p' */
    /* If no colon present, use default for the device name                                  */

    p = strchr (name, ':');
    if (p != NULL) {
      if (p - name >= sizeof nambuf) {
        oz_sys_io_fs_printerror ("oz_sys_io_fs_parse: device name in %s too long\n", name);
        return (OZ_EXMAXFILENAME);
      }
      memcpy (nambuf, name, p - name);
      nambuf[p-name] = 0;
      p ++;
    } else {
      p = name;
      q = strchr (def_dir, ':');
      strncpyz (nambuf, def_dir, sizeof nambuf);
      if (q == NULL) q = "";
      else nambuf[q++-def_dir] = 0;
    }

    /* Now if device name translates as a logical name, open using the translated string followed by the filename */

    sts = oz_sys_logname_lookup (h_defaulttbl, lnmprocmode, nambuf, NULL, NULL, &nvalues, &h_logname);
    if (sts == OZ_SUCCESS) {
      fnlen = strlen (q) + strlen (p);
      if (fnlen > OZ_FS_MAXFNLEN - 2) {
        oz_sys_io_fs_printerror ("oz_sys_io_fs_parse: filename string %s:%s%s too long\n", nambuf, q, p);
        oz_sys_handle_release (OZ_PROCMODE_KNL, h_defaulttbl);
        return (OZ_EXMAXFILENAME);
      }

      /* Name translates, loop through values until the file is found */

      if (nvalues == 0) {
        /* It has no values, just pretend it doesn't exist */
        oz_sys_handle_release (OZ_PROCMODE_KNL, h_logname);
        oz_sys_handle_release (OZ_PROCMODE_KNL, h_defaulttbl);
        return (OZ_NOLOGNAME);
      }

      for (ivalue = 0; ivalue < nvalues; ivalue ++) {

        /* Get a value */

        sts = oz_sys_logname_getval (h_logname, ivalue, &logvalatr, sizeof nambuf - strlen (p), nambuf, NULL, NULL, 0, NULL);
        if (sts != OZ_SUCCESS) {
          oz_sys_io_fs_printerror ("oz_sys_io_fs_parse: error %u translating logical name %s index %u\n", sts, name, ivalue);
          break;
        }

        /* Concat the filename onto the end of the translated string */

        strcat (nambuf, q);
        strcat (nambuf, p);

        /* Try to open the result, stop looping if successful */

        sts = io_fs_parse (nambuf, (logvalatr & OZ_LOGVALATR_TERMINAL) != 0, def_dir, lnmprocmode, level + 1, entry, param);
        if (sts != OZ_IOFSPARSECONT) break;
      }
      oz_sys_handle_release (OZ_PROCMODE_KNL, h_logname);
      oz_sys_handle_release (OZ_PROCMODE_KNL, h_defaulttbl);
      return (sts);
    }

    /* Device name does not translate, treat as if it were terminal */

    oz_sys_handle_release (OZ_PROCMODE_KNL, h_defaulttbl);
  }

  /* Terminal or does not translate, call the processing routine with device and filenames, return resultant status */

  if (q[0] != 0) {			/* see if there is a little bit of a directory from def_dir */
    fnlen = strlen (nambuf) + 1;	/* ok, find some free space in nambuf */
    strcpy (nambuf + fnlen, q);		/* put the directory part from def_dir in there */
    strcat (nambuf + fnlen, p);		/* concat on the filename */
    p = nambuf + fnlen;			/* point to the result */
  }
  sts = (*entry) (param, nambuf, p, 0);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Assign I/O channel to device for a filesystem operation		*/
/*									*/
/*    Input:								*/
/*									*/
/*	devname = device name string (null terminated)			*/
/*	lockmode = lockmode that file will be opened/created with	*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_io_fs_assign = assignment status				*/
/*	*h_iochan_r = I/O channel assigned to device			*/
/*									*/
/************************************************************************/

uLong oz_sys_io_fs_assign (const char *devname, OZ_Lockmode lockmode, OZ_Handle *h_iochan_r)

{
  uLong sts;
  OZ_Lockmode alm;

  /* Determine lock mode to assign channel in - use minimal for lockmode that file will be opened/created with */

  if (OZ_LOCK_ALLOW_TEST (lockmode, OZ_LOCK_ALLOWS_SELF_WRITE)) alm = OZ_LOCKMODE_CW;
  else if (OZ_LOCK_ALLOW_TEST (lockmode, OZ_LOCK_ALLOWS_SELF_READ)) alm = OZ_LOCKMODE_CR;
  else alm = OZ_LOCKMODE_NL;

  /* Now assign the I/O channel */

  sts = oz_sys_io_assign (OZ_PROCMODE_KNL, h_iochan_r, devname, alm);
  return (sts);
}
