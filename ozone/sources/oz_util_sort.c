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
/*  Sort utiliti							*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_crtl_malloc.h"
#include "oz_io_fs.h"
#include "oz_knl_hw.h"
#include "oz_knl_status.h"
#include "oz_sys_io.h"
#include "oz_sys_io_fs.h"
#include "oz_sys_io_fs_printf.h"
#include "oz_sys_process.h"
#include "oz_sys_section.h"
#include "oz_util_start.h"

#include <stdlib.h>

typedef struct Key Key;

struct Key { Key *next;
             int beg, end, rev;
           };

typedef struct Record Record;

struct Record { Record *next;
                uLong size;
                uByte data[256];
                uByte newline;
              };

typedef struct { uLong size;
                 uByte *buff;
               } Sortarray;

static char *pn = "sort";
static Key *keys;

static int compare (const void *sa1v, const void *sa2v);

uLong oz_util_main (int argc, char *argv[])

{
  char *in, *out;
  int beg, end, i, j, nlines, rev;
  Key *key, **keyt;
  OZ_Handle h_in, h_out;
  OZ_IO_fs_create fs_create;
  OZ_IO_fs_getinfo1 fs_getinfo1;
  OZ_IO_fs_open fs_open;
  OZ_IO_fs_readrec fs_readrec;
  OZ_IO_fs_writerec fs_writerec;
  Record **lrecord, *record, *records;
  Sortarray *sortarray;
  uByte *inputfilevaddr;
  uLong inputfilebytes, readlen, sts;

  if (argc > 0) pn = argv[0];

  h_in  = oz_util_h_input;						// default input file is stdin
  h_out = oz_util_h_output;						// default output is stdout
  in    = NULL;								// haven't seen input filename yet
  out   = NULL;								// haven't seen output filename yet
  keyt  = &keys;							// init key list tail pointer

  for (i = 1; i < argc; i ++) {						// loop through arguments
    if (argv[i][0] == '-') {						// check for options
      if (strcasecmp (argv[i], "-key") == 0) {				// -key
        if (++ i >= argc) goto usage;					// requires a parameter
        beg = oz_hw_atoi (argv[i], &j);					// <beg> column number
        if (argv[i][j] != ',') goto usage;				// must end with a comma
        argv[i] += ++ j;						// increment past the comma
        end = oz_hw_atoi (argv[i], &j);					// <end> column number
        rev = 0;							// assume not reverse
        if (argv[i][j] != 0) {						// see if any key options
          if (strcasecmp (argv[i] + j, ",rev") != 0) goto usage;	// see if ,rev option
          rev = 1;							// ok, set rev flag
        }
        if (end <= beg) goto usage;					// <end> must be after <beg>
        key = malloc (sizeof *key);					// malloc a struct for it
        key -> beg  = beg;						// fill it in
        key -> end  = end;
        key -> rev  = rev;
        *keyt = key;							// link onto tail of keys list
        keyt = &(key -> next);
        continue;
      }
      goto usage;							// unknown option
    }
    if (in == NULL) in = argv[i];					// not option, first arg is input filename
    else if (out == NULL) out = argv[i];				// second arg is output filename
    else goto usage;							// don't have a third arg
  }
  *keyt = NULL;								// terminate key list

  if (keys == NULL) {							// see if we got any key options at all
    keys = malloc (sizeof *keys);					// if not, use a default
    keys -> next = NULL;
    keys -> beg  = 0;
    keys -> end  = (((uLong)(-1)) >> 2);
    keys -> rev  = 0;
  }

  if (in != NULL) {							// see if input file was specified
    memset (&fs_open, 0, sizeof fs_open);				// if so, open it
    fs_open.name = in;
    fs_open.lockmode = OZ_LOCKMODE_PR;
    sts = oz_sys_io_fs_open (sizeof fs_open, &fs_open, 0, &h_in);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u opening input file %s\n", pn, sts, in);
      return (sts);
    }
  }

  if (out != NULL) {							// see if output file was specified
    memset (&fs_create, 0, sizeof fs_create);				// if so, create it
    fs_create.name = out;
    fs_create.lockmode = OZ_LOCKMODE_PW;
    sts = oz_sys_io_fs_create (sizeof fs_create, &fs_create, 0, &h_out);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u creating output file %s\n", pn, sts, out);
      return (sts);
    }
  }

  /* Try to read input file into memory all at once */

  memset (&fs_getinfo1, 0, sizeof fs_getinfo1);
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_in, 0, OZ_IO_FS_GETINFO1, sizeof fs_getinfo1, &fs_getinfo1);
  if ((sts != OZ_SUCCESS) && (sts != OZ_BADIOFUNC)) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u getting input file size\n", pn, sts);
    return (sts);
  }

  if ((sts == OZ_SUCCESS) && (fs_getinfo1.eofblock != 0)) {

    inputfilebytes = (fs_getinfo1.eofblock - 1) * fs_getinfo1.blocksize + fs_getinfo1.eofbyte;
    inputfilevaddr = malloc (inputfilebytes);

    memset (&fs_readrec, 0, sizeof fs_readrec);
    fs_readrec.size = inputfilebytes;
    fs_readrec.buff = inputfilevaddr;
    fs_readrec.rlen = &readlen;
    sts = oz_sys_io (OZ_PROCMODE_KNL, h_in, 0, OZ_IO_FS_READREC, sizeof fs_readrec, &fs_readrec);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u reading input file\n", pn, sts);
      return (sts);
    }
    if (readlen != inputfilebytes) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: only read %u bytes of %u from input\n", pn, readlen, inputfilebytes);
      return (OZ_ENDOFFILE);
    }

    nlines = 0;
    for (i = 0; i < inputfilebytes; i ++) if (inputfilevaddr[i] == '\n') nlines ++;

    sortarray = malloc ((nlines + 1) * sizeof *sortarray);
    nlines = 0;
    sortarray[0].buff = inputfilevaddr;
    for (i = 1; i <= inputfilebytes; i ++) if (inputfilevaddr[i-1] == '\n') {
      sortarray[nlines].size = i - (sortarray[nlines].buff - inputfilevaddr);
      sortarray[++nlines].buff = inputfilevaddr + i;
    }
  }

  /* Otherwise, read it a line at a time */

  else {
    nlines = 0;
    memset (&fs_readrec, 0, sizeof fs_readrec);
    fs_readrec.size = sizeof record -> data;
    fs_readrec.rlen = &readlen;
    fs_readrec.trmsize = 1;
    fs_readrec.trmbuff = "\n";
    lrecord = &records;
    while (1) {
      record = malloc (sizeof *record);
      record -> size  = 0;
      fs_readrec.buff = record -> data;
      sts = oz_sys_io (OZ_PROCMODE_KNL, h_in, 0, OZ_IO_FS_READREC, sizeof fs_readrec, &fs_readrec);
      while (sts == OZ_NOTERMINATOR) {
        record -> size += readlen;
        record = realloc (record, record -> size + sizeof *record);
        fs_readrec.buff = record -> data + record -> size;
        sts = oz_sys_io (OZ_PROCMODE_KNL, h_in, 0, OZ_IO_FS_READREC, sizeof fs_readrec, &fs_readrec);
      }
      if ((sts != OZ_SUCCESS) && (sts != OZ_ENDOFFILE)) {
        oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u reading input\n", pn, sts);
        return (sts);
      }
      record -> size += readlen;
      if (sts == OZ_SUCCESS) record -> data[record->size++] = '\n';
      if (record -> size == 0) break;
      *lrecord = record;
      lrecord = &(record -> next);
      nlines ++;
    }
    *lrecord = NULL;
    sortarray = malloc (nlines * sizeof sortarray[0]);
    nlines = 0;
    for (record = records; record != NULL; record = record -> next) {
      sortarray[nlines].size = record -> size;
      sortarray[nlines].buff = record -> data;
      nlines ++;
    }
  }

  /* Do the sort */

  if (nlines > 1) qsort (sortarray, nlines, sizeof sortarray[0], compare);

  /* Write output */

  memset (&fs_writerec, 0, sizeof fs_writerec);
  for (i = 0; i < nlines; i ++) {
    fs_writerec.size = sortarray[i].size;
    fs_writerec.buff = sortarray[i].buff;
    sts = oz_sys_io (OZ_PROCMODE_KNL, h_out, 0, OZ_IO_FS_WRITEREC, sizeof fs_writerec, &fs_writerec);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u writing output file\n", pn, sts);
      return (sts);
    }
  }

  return (OZ_SUCCESS);

usage:
  oz_sys_io_fs_printf (oz_util_h_error, "usage: %s -key <beg>,<end>[,rev] [<inputfile> [<outputfile>]]\n", pn);
  return (OZ_BADPARAM);
}

/* Compare two records according to the given keys */

static int compare (const void *sa1v, const void *sa2v)

{
  const Sortarray *sa1, *sa2;
  int l, s1, s2, x;
  Key *key;

  sa1 = sa1v;
  sa2 = sa2v;

  for (key = keys; key != NULL; key = key -> next) {
    s1 = sa1 -> size;
    s2 = sa2 -> size;

    /* Chop sizes off at end of key */

    if (s1 > key -> end) s1 = key -> end;
    if (s2 > key -> end) s2 = key -> end;

    /* Now get size after beginning of key (might be negative if rec doesn't reach key) */

    s1 -= key -> beg;
    if (s1 < 0) s1 = 0;
    s2 -= key -> beg;
    if (s2 < 0) s2 = 0;

    /* Determine length to compare = smaller of the two sizes */

    l = s1;
    if (l > s2) l = s2;

    /* Compare what we have of the two strings */

    x = memcmp (sa1 -> buff + key -> beg, sa2 -> buff + key -> beg, l);

    /* If exact match, and one had left over chars, consider it greater */

    if (x == 0) x = s1 - s2;

    /* If non-zero, return result (after applying reverse factor) */

    if (x != 0) {
      if (key -> rev) return (-x);
      return (x);
    }
  }

  return (0);
}
