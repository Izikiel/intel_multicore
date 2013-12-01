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
/*  Directory command utiliti						*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_crtl_malloc.h"
#include "oz_io_fs.h"
#include "oz_knl_lock.h"
#include "oz_knl_security.h"
#include "oz_knl_status.h"
#include "oz_sys_handle.h"
#include "oz_sys_io.h"
#include "oz_sys_io_fs.h"
#include "oz_sys_io_fs_printf.h"
#include "oz_util_filesel.h"
#include "oz_util_start.h"

typedef struct Print Print;
struct Print { Print *next;			// next in printhead list
               char *devname;			// device name string (in 'strings')
               char *instance;			// instance string (in 'strings')
               char *fileidstr;			// fileid string (in 'strings')
               char *secattrbuf;		// security attributes (binary, in 'strings')
               uLong secattrlen, secattrsts;	// length and status of security attributes;
               OZ_FS_Subs instsubs;		// instance string substrings
               OZ_IO_fs_getinfo1 fsinfo1;	// file information
               char strings[1];			// variable strings
             };

typedef struct Spec Spec;
struct Spec { Spec *next;
              char name[1];
            };

static char *lastdev, *lastdir, *pn;
static int opt_continue, opt_long, opt_security, opt_short;
static int (*opt_sort) (const void *, const void *);
static int printcount;
static OZ_Filesel *filesel;
static Print *printhead, **printtail;

static uLong dirofafile (void *dummy, const char *devname, const char *wildcard, const char *instance, OZ_Handle h_ioch, uLong fileidsize, void *fileidbuff, OZ_FS_Subs *wildsubs, OZ_FS_Subs *instsubs);
static void printit (Print *print);
static void *secmalloc (void *dummy, uLong osize, void *obuff, uLong nsize);
static int sort_accessed (const void *v1, const void *v2);
static int sort_archived (const void *v1, const void *v2);
static int sort_backedup (const void *v1, const void *v2);
static int sort_changed (const void *v1, const void *v2);
static int sort_created (const void *v1, const void *v2);
static int sort_eof (const void *v1, const void *v2);
static int sort_expired (const void *v1, const void *v2);
static int sort_modified (const void *v1, const void *v2);
static int sort_size (const void *v1, const void *v2);

uLong oz_util_main (int argc, char *argv[])

{
  char *wc;
  int i, rc;
  Print *print, **printsort;
  Spec **lspec, *spec, *specs;
  uLong sts;

  pn = "dir";
  if (argc > 0) pn = argv[0];

  /* Process command line */

  opt_continue = 0;
  opt_long     = 0;
  opt_short    = 0;
  opt_sort     = NULL;
  lspec        = &specs;
  filesel      = oz_util_filesel_init ();
  for (i = 1; i < argc; i ++) {
    if (argv[i][0] != '-') {
      spec = malloc (sizeof *spec + strlen (argv[i]));
      *lspec = spec;
      lspec  = &(spec -> next);
      strcpy (spec -> name, argv[i]);
      continue;
    }
    rc = oz_util_filesel_parse (filesel, argc - i, argv + i);
    if (rc < 0) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: invalid argument[%d] = '%s': %s\n", pn, i, argv[i], oz_util_filesel_errmsg (filesel));
      goto usage;
    }
    if (rc > 0) {
      i += rc - 1;
      continue;
    }
         if (strcmp (argv[i] + 1, "continue") == 0) opt_continue = 1;
    else if (strcmp (argv[i] + 1, "long")     == 0) { opt_long  = 1; opt_short = 0; }
    else if (strcmp (argv[i] + 1, "security") == 0) opt_security = 1;
    else if (strcmp (argv[i] + 1, "short")    == 0) { opt_short = 1; opt_long = 0; }
    else if (strcmp (argv[i] + 1, "sort")     == 0) {
      if (++ i >= argc) {
        oz_sys_io_fs_printf (oz_util_h_error, "%s: missing sort type\n", pn);
        goto usage;
      }
           if (strcasecmp (argv[i], "accessed") == 0) opt_sort = sort_accessed;
      else if (strcasecmp (argv[i], "archived") == 0) opt_sort = sort_archived;
      else if (strcasecmp (argv[i], "backedup") == 0) opt_sort = sort_backedup;
      else if (strcasecmp (argv[i], "changed")  == 0) opt_sort = sort_changed;
      else if (strcasecmp (argv[i], "created")  == 0) opt_sort = sort_created;
      else if (strcasecmp (argv[i], "eof")      == 0) opt_sort = sort_eof;
      else if (strcasecmp (argv[i], "expired")  == 0) opt_sort = sort_expired;
      else if (strcasecmp (argv[i], "modified") == 0) opt_sort = sort_modified;
      else if (strcasecmp (argv[i], "size")     == 0) opt_sort = sort_size;
      else {
        oz_sys_io_fs_printf (oz_util_h_error, "%s: bad sort type %s\n", pn, argv[i]);
        goto usage;
      }
    }
    else goto usage;
  }
  if (oz_util_filesel_parse (filesel, 0, NULL) < 0) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: %s\n", oz_util_filesel_errmsg (filesel));
    goto usage;
  }
  *lspec = NULL;

  /* Process the supplied filespecs */

  lastdev = NULL;
  lastdir = NULL;

  printcount = 0;
  printtail  = &printhead;

  if (specs == NULL) {
    sts = oz_sys_io_fs_wildscan3 ("", OZ_SYS_IO_FS_WILDSCAN_DIRLIST, NULL, dirofafile, NULL);
    if (sts == OZ_IOFSPARSECONT) sts = OZ_SUCCESS;
    if (sts != OZ_SUCCESS) oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u processing default directory\n", pn, sts);
  }

  else {
    while ((spec = specs) != NULL) {
      specs = spec -> next;
      sts = oz_sys_io_fs_wildscan3 (spec -> name, OZ_SYS_IO_FS_WILDSCAN_DIRLIST, NULL, dirofafile, NULL);
      if (sts == OZ_IOFSPARSECONT) sts = OZ_SUCCESS;
      if (sts != OZ_SUCCESS) {
        oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u processing %s\n", pn, sts, spec -> name);
        break;
      }
    }
  }

  /* If -sort, sort and print results */

  if ((opt_sort != NULL) && (printcount > 0)) {
    *printtail = NULL;
    printsort  = malloc (printcount * sizeof *printsort);
    i = 0;
    for (print = printhead; print != NULL; print = print -> next) printsort[i++] = print;
    qsort (printsort, printcount, sizeof *printsort, opt_sort);
    for (i = 0; i < printcount; i ++) printit (printsort[i]);
  }

  return (sts);

usage:
  oz_sys_io_fs_printf (oz_util_h_error, "usage: %s [-continue] [-long] [-security] [-short] [-sort accessed|archived|backedup|changed|created|eof|expired|modified|size] %s <files...>\n", pn, oz_util_filesel_usage);
  return (OZ_BADPARAM);
}

/************************************************************************/
/*									*/
/*  This routine is called for each file found				*/
/*									*/
/************************************************************************/

static uLong dirofafile (void *dummy, const char *devname, const char *wildcard, const char *instance, OZ_Handle h_ioch, uLong fileidsize, void *fileidbuff, OZ_FS_Subs *wildsubs, OZ_FS_Subs *instsubs)

{
  char *p;
  OZ_FS_Subs subs;
  OZ_Handle h_file;
  OZ_IO_fs_getinfo1 fs_getinfo1;
  OZ_IO_fs_getinfo3 fs_getinfo3;
  OZ_IO_fs_getsecattr fs_getsecattr;
  OZ_IO_fs_open fs_open;
  Print *print;
  uLong sts;

  memset (&fs_getinfo3, 0, sizeof fs_getinfo3);
  memset (&subs,        0, sizeof subs);
  memset (&fs_open,     0, sizeof fs_open);
  memset (&fs_getinfo1, 0, sizeof fs_getinfo1);

  if (h_ioch != 0) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: can't do dir of an I/O channel %s\n", pn, instance);
    return (OZ_BADPARAM);
  }

  /* Assign an I/O handle to the disk */

  sts = oz_sys_io_assign (OZ_PROCMODE_KNL, &h_file, devname, OZ_LOCKMODE_CR);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u assigning channel to %s\n", pn, sts, devname);
    return (sts);
  }

  /* Get some stuff about the volume, if it will be needed */

  if (opt_long && (fileidsize != 0)) {
    sts = oz_sys_io (OZ_PROCMODE_KNL, h_file, 0, OZ_IO_FS_GETINFO3, sizeof fs_getinfo3, &fs_getinfo3);
    if ((sts != OZ_SUCCESS) && (sts != OZ_BADIOFUNC)) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u getting volume info about %s\n", pn, sts, devname);
      goto rtnsts;
    }
  }

  /* See if the file meets selection criteria */

  if (fileidsize != 0) {
    fs_open.fileidsize = fileidsize;
    fs_open.fileidbuff = fileidbuff;
  } else {
    fs_open.name       = instance;
  }
  fs_open.lockmode = OZ_LOCKMODE_NL;
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_file, 0, OZ_IO_FS_OPEN, sizeof fs_open, &fs_open);
  if (sts == OZ_SUCCESS) sts = oz_sys_io (OZ_PROCMODE_KNL, h_file, 0, OZ_IO_FS_GETINFO1, sizeof fs_getinfo1, &fs_getinfo1);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u looking at %s\n", pn, sts, instance);
    if (opt_continue) goto rtncont;
    goto rtnsts;
  }

  sts = oz_util_filesel_check (filesel, h_file, instance);
  if (sts == OZ_FLAGWASCLR) goto rtncont;
  if (sts != OZ_FLAGWASSET) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u selecting %s\n", pn, sts, instance);
    if (opt_continue) goto rtncont;
    goto rtnsts;
  }

  /* Pack it up in a print node for printing */

  print = malloc (sizeof *print + strlen (devname) + strlen (instance) + fs_getinfo3.fileidstrsz + opt_security * fs_getinfo1.secattrsize + 2);
  print -> instsubs = *instsubs;
  print -> fsinfo1  = fs_getinfo1;

  p = print -> strings;

  print -> devname = p;
  strcpy (p, devname);
  p += strlen (p) + 1;

  print -> instance = p;
  strcpy (p, instance);
  p += strlen (p) + 1;

  print -> fileidstr = NULL;
  if (opt_long && (fileidsize != 0) && (fs_getinfo3.fileidstrsz != 0)) {
    print -> fileidstr = p;
    (*(fs_getinfo3.fidtoa)) (fileidbuff, fs_getinfo3.fileidstrsz, p);
    p += strlen (p) + 1;
  }

  print -> secattrbuf = NULL;
  if (opt_security) {
    print -> secattrbuf = p;
    memset (&fs_getsecattr, 0, sizeof fs_getsecattr);
    fs_getsecattr.size = fs_getinfo1.secattrsize;
    fs_getsecattr.buff = p;
    fs_getsecattr.rlen = &(print -> secattrlen);
    print -> secattrsts = oz_sys_io (OZ_PROCMODE_KNL, h_file, 0, OZ_IO_FS_GETSECATTR, sizeof fs_getsecattr, &fs_getsecattr);
    p += fs_getinfo1.secattrsize;
  }

  /* If -sort, save the data for printing later after sorting it */

  if (opt_sort != NULL) {
    *printtail = print;
    printtail  = &(print -> next);
    printcount ++;
  }

  /* Otherwise, just print it now */

  else {
    printit (print);
    free (print);
  }

  /* Anyway, continue scanning */

rtncont:
  sts = OZ_IOFSWILDSCANCONT;

rtnsts:
  oz_sys_handle_release (OZ_PROCMODE_KNL, h_file);
  if (fs_getinfo1.fileidbuff != NULL) free (fs_getinfo1.fileidbuff);
  return (sts);
}

/* Print out a file listing */

static void printit (Print *print)

{
  char *secattrstr;
  uLong sts;

  /* If different disk or directory, save the new ones and print them out */

  if ((lastdir == NULL) 
   || (strcmp (lastdev, print -> devname) != 0) 
   || (lastdir[print -> instsubs.dirsize] != 0) 
   || (memcmp (lastdir, print -> instance, print -> instsubs.dirsize) != 0)) {
    if (lastdev != NULL) free (lastdev);
    if (lastdir != NULL) free (lastdir);
    lastdev = malloc (strlen (print -> devname) + 1);
    lastdir = malloc (print -> instsubs.dirsize + 1);
    strcpy (lastdev, print -> devname);
    memcpy (lastdir, print -> instance, print -> instsubs.dirsize);
    lastdir[print -> instsubs.dirsize] = 0;
    oz_sys_io_fs_printf (oz_util_h_output, "%s:%s\n", lastdev, lastdir);
  }

  /* If either -short or -long, print the filename out on a line by itself */

  if (opt_short || opt_long) oz_sys_io_fs_printf (oz_util_h_output, "  %s\n", print -> instance + print -> instsubs.dirsize);

  /* Print out attributes */

  if (opt_long) {
    if (print -> fileidstr != NULL) oz_sys_io_fs_printf (oz_util_h_output, "	   fileid: %s\n", print -> fileidstr);
    oz_sys_io_fs_printf (oz_util_h_output, "	endoffile: %u.%u/%u\n", print -> fsinfo1.eofblock, print -> fsinfo1.eofbyte, print -> fsinfo1.blocksize);
    oz_sys_io_fs_printf (oz_util_h_output, "	allocated: %u\n", print -> fsinfo1.hiblock);
    oz_sys_io_fs_printf (oz_util_h_output, "	 accessed: %t\n", print -> fsinfo1.access_date);
    oz_sys_io_fs_printf (oz_util_h_output, "	  created: %t\n", print -> fsinfo1.create_date);
    oz_sys_io_fs_printf (oz_util_h_output, "	  changed: %t\n", print -> fsinfo1.change_date);
    oz_sys_io_fs_printf (oz_util_h_output, "	 modified: %t\n", print -> fsinfo1.modify_date);
    oz_sys_io_fs_printf (oz_util_h_output, "	backed up: %t\n", print -> fsinfo1.backup_date);
    oz_sys_io_fs_printf (oz_util_h_output, "	 archived: %t\n", print -> fsinfo1.archive_date);
  } else if (!opt_short) {
    if (print -> fsinfo1.eofbyte == 0) print -> fsinfo1.eofblock --;
    oz_sys_io_fs_printf (oz_util_h_output, "  %8u/%-8u  %19.19t  %s\n", print -> fsinfo1.eofblock, print -> fsinfo1.hiblock, print -> fsinfo1.change_date, print -> instance + print -> instsubs.dirsize);
  }

  /* If -security, try to print out security attributes */

  if (opt_security) {
    secattrstr = NULL;
    sts = print -> secattrsts;
    if (sts == OZ_SUCCESS) sts = oz_sys_secattr_bin2str (print -> secattrlen, print -> secattrbuf, NULL, secmalloc, NULL, &secattrstr);
    if (sts == OZ_SUCCESS) oz_sys_io_fs_printf (oz_util_h_output, "	  secattr: (%s)\n", secattrstr);
    else oz_sys_io_fs_printf (oz_util_h_output, "	  secattr: (size %u, error %u)\n", print -> fsinfo1.secattrsize, sts);
    if (secattrstr != NULL) free (secattrstr);
  }
}

/* Alloc memory for security structs */

static void *secmalloc (void *dummy, uLong osize, void *obuff, uLong nsize)

{
  void *nbuff;

  nbuff = NULL;
  if (nsize != 0) {
    nbuff = malloc (nsize);
    memcpy (nbuff, obuff, osize);
  }
  if (obuff != NULL) free (obuff);
  return (nbuff);
}

/************************************************************************/
/*									*/
/*  -sort option routines						*/
/*									*/
/************************************************************************/

static int sort_accessed (const void *v1, const void *v2)

{
  Print *p1, *p2;

  p1 = *(Print **)v1;
  p2 = *(Print **)v2;
  if (p1 -> fsinfo1.access_date < p2 -> fsinfo1.access_date) return (-1);
  if (p1 -> fsinfo1.access_date > p2 -> fsinfo1.access_date) return (1);
  return (0);
}

static int sort_archived (const void *v1, const void *v2)

{
  Print *p1, *p2;

  p1 = *(Print **)v1;
  p2 = *(Print **)v2;
  if (p1 -> fsinfo1.archive_date < p2 -> fsinfo1.archive_date) return (-1);
  if (p1 -> fsinfo1.archive_date > p2 -> fsinfo1.archive_date) return (1);
  return (0);
}

static int sort_backedup (const void *v1, const void *v2)

{
  Print *p1, *p2;

  p1 = *(Print **)v1;
  p2 = *(Print **)v2;
  if (p1 -> fsinfo1.backup_date < p2 -> fsinfo1.backup_date) return (-1);
  if (p1 -> fsinfo1.backup_date > p2 -> fsinfo1.backup_date) return (1);
  return (0);
}

static int sort_changed (const void *v1, const void *v2)

{
  Print *p1, *p2;

  p1 = *(Print **)v1;
  p2 = *(Print **)v2;
  if (p1 -> fsinfo1.change_date < p2 -> fsinfo1.change_date) return (-1);
  if (p1 -> fsinfo1.change_date > p2 -> fsinfo1.change_date) return (1);
  return (0);
}

static int sort_created (const void *v1, const void *v2)

{
  Print *p1, *p2;

  p1 = *(Print **)v1;
  p2 = *(Print **)v2;
  if (p1 -> fsinfo1.create_date < p2 -> fsinfo1.create_date) return (-1);
  if (p1 -> fsinfo1.create_date > p2 -> fsinfo1.create_date) return (1);
  return (0);
}

static int sort_eof (const void *v1, const void *v2)

{
  Print *p1, *p2;

  p1 = *(Print **)v1;
  p2 = *(Print **)v2;
  if (p1 -> fsinfo1.eofblock < p2 -> fsinfo1.eofblock) return (-1);
  if (p1 -> fsinfo1.eofblock > p2 -> fsinfo1.eofblock) return (1);
  if (p1 -> fsinfo1.eofbyte  < p2 -> fsinfo1.eofbyte)  return (-1);
  if (p1 -> fsinfo1.eofbyte  > p2 -> fsinfo1.eofbyte)  return (1);
  return (0);
}

static int sort_expired (const void *v1, const void *v2)

{
  Print *p1, *p2;

  p1 = *(Print **)v1;
  p2 = *(Print **)v2;
  if (p1 -> fsinfo1.expire_date < p2 -> fsinfo1.expire_date) return (-1);
  if (p1 -> fsinfo1.expire_date > p2 -> fsinfo1.expire_date) return (1);
  return (0);
}

static int sort_modified (const void *v1, const void *v2)

{
  Print *p1, *p2;

  p1 = *(Print **)v1;
  p2 = *(Print **)v2;
  if (p1 -> fsinfo1.modify_date < p2 -> fsinfo1.modify_date) return (-1);
  if (p1 -> fsinfo1.modify_date > p2 -> fsinfo1.modify_date) return (1);
  return (0);
}

static int sort_size (const void *v1, const void *v2)

{
  Print *p1, *p2;

  p1 = *(Print **)v1;
  p2 = *(Print **)v2;
  if (p1 -> fsinfo1.hiblock < p2 -> fsinfo1.hiblock) return (-1);
  if (p1 -> fsinfo1.hiblock > p2 -> fsinfo1.hiblock) return (1);
  return (0);
}
