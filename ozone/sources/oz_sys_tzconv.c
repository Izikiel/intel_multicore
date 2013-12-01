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
/*  Timezone convertion							*/
/*									*/
/************************************************************************/

#include "oz_knl_hw.h"
#include "oz_knl_process.h"
#include "oz_knl_section.h"
#include "oz_knl_status.h"
#include "oz_knl_threadlock.h"
#include "oz_sys_dateconv.h"
#include "oz_sys_io_fs.h"
#include "oz_sys_pdata.h"
#include "oz_sys_process.h"
#include "oz_sys_section.h"
#include "oz_sys_syscall.h"

typedef struct { uByte gmtoffs[4];
                 uByte isdst;
                 uByte nameidx;
               } Tzinfo;

typedef struct { uLong filesize;			// size of filebuff
                 uByte *filebuff;			// original contents of the file
                 OZ_Procmode mprocmode;			// memory the struct is malloced in
                 Long tzh_timecnt;			// number of elements in transitiontimes, transitionindxs arrays
                 Long tzh_typecnt;			// number of elements in zoneinfos array
                 Long tzh_charcnt;			// number of elements in zonenames array
                 const uByte *transitionindxs;		// corresponding indices into zoneinfos array
                 const Tzinfo *zoneinfos;		// information about each type
                 const char *zonenames;			// null-terminated name strings
                 OZ_Datebin *quadgmtoffs;
                 OZ_Datebin quadtransitiontimes[1];	// quadword date/times corresponding to longtransitiontimes
               } Tzfile;

#define TZLONG(ptrub4) (((((((ptrub4)[0] << 8) + (ptrub4)[1]) << 8) + (ptrub4)[2]) << 8) + (ptrub4)[3])

static OZ_Threadlock *defaultzlock = NULL;
static Tzfile *defaultzfile = NULL;

static uLong readtzfile (OZ_Handle h_tzfile, OZ_Procmode mprocmode, Tzfile **tzfile_r);
static void closetzfile (Tzfile *tzfile);

/************************************************************************/
/*									*/
/*  This routine is called in kernel mode to set the default timezone	*/
/*									*/
/*    Input:								*/
/*									*/
/*	tzname = name of timezone file					*/
/*	smplevel <= softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_tzconv_setdefault = OZ_SUCCESS : successful		*/
/*	                                 else : error status		*/
/*									*/
/************************************************************************/

uLong oz_knl_tzconv_setdefault (const char *tzname)

{
  int si;
  OZ_Handle h_iochan;
  OZ_IO_fs_open fs_open;
  OZ_Threadlock *threadlock;
  Tzfile *newtzfile, *oldtzfile;
  uLong sts;

  si = oz_hw_cpu_setsoftint (0);				// prevent thread from being aborted

  /* Open the given timezone file */

  memset (&fs_open, 0, sizeof fs_open);
  fs_open.name = tzname;
  fs_open.lockmode = OZ_LOCKMODE_PR;
  sts = oz_sys_io_fs_open2 (sizeof fs_open, &fs_open, 0, "OZ_TIMEZONE_DIR", &h_iochan);
  if (sts != OZ_SUCCESS) goto rtnsts;

  /* Create a Tzfile struct for it */

  sts = readtzfile (h_iochan, OZ_PROCMODE_SYS, &newtzfile);	// read the new timezone file into system global paged memory
  oz_sys_handle_release (OZ_PROCMODE_KNL, h_iochan);		// close the file
  if (sts != OZ_SUCCESS) goto rtnsts;				// if failure, return error status

  /* If there is no threadlock, create one */

  if (defaultzlock == NULL) {
    sts = oz_knl_threadlock_create ("default tzlock", &threadlock);
    if (sts != OZ_SUCCESS) goto rtnsts;
    if (!oz_hw_atomic_setif_ptr (&defaultzlock, threadlock, NULL)) oz_knl_threadlock_delete (threadlock);
  }

  /* Set new zone as the new default, close out any old default tzfile */

  oz_knl_threadlock_ex (defaultzlock);				// block out any readers
  oldtzfile = defaultzfile;					// save old file so we can close it
  defaultzfile = newtzfile;					// set up new file
  oz_knl_threadunlk_ex (defaultzlock);				// release so it can be used
  if (oldtzfile != NULL) closetzfile (oldtzfile);		// close old file

rtnsts:
  oz_hw_cpu_setsoftint (si);					// it's ok to be aborted now
  return (sts);
}

/************************************************************************/
/*									*/
/*  Convert an UTC time to local timezone				*/
/*									*/
/*    Input:								*/
/*									*/
/*	tzconvtype = OZ_DATEBIN_TZCONV_UTC2LCL				*/
/*	             OZ_DATEBIN_TZCONV_LCL2UTC				*/
/*	in = input date/time						*/
/*	tznamein = NULL : use current system default			*/
/*	           else : use the given timezone info			*/
/*	out = where to return resultant date/time			*/
/*	tznameoutl = length of tznameout buffer				*/
/*	tznameout  = where to return timezone name string		*/
/*	smplevel  <= softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_tzconv = OZ_SUCCESS : successful				*/
/*	                      else : error status			*/
/*	*out = filled in with resultant date/time			*/
/*	*tznameout = filled in with null-terminated timezone string	*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_6 (tzconv, OZ_Datebin_tzconv, tzconvtype, 
                                    OZ_Datebin, in, 
                                     OZ_Handle, h_tzfilein, 
                                  OZ_Datebin *, out, 
                                           int, tznameoutl, 
                                        char *, tznameout)

{
  const char *p;
  int si;
  Long i, tzseq;
  Tzfile *tzfile;
  uByte j;
  uLong sts;

  if ((tzconvtype != OZ_DATEBIN_TZCONV_UTC2LCL) && (tzconvtype != OZ_DATEBIN_TZCONV_LCL2UTC)) return (OZ_BADPARAM);

  si = oz_hw_cpu_setsoftint (0);

  /* Get pointer to timezone file in memory */

  if (h_tzfilein != 0) {					// see if caller supplied a timezone file
    sts = readtzfile (h_tzfilein, OZ_PROCMODE_KNL, &tzfile);	// if so, try to read it into proc private knl memory
    if (sts != OZ_SUCCESS) goto rtnsts;
  } else {
    sts = OZ_BADTZFILE;						// if not, assume it's not set up yet
    if (defaultzlock == NULL) goto rtnsts;
    oz_knl_threadlock_pr (defaultzlock);			// ... then lock default from changing
    tzfile = defaultzfile;
    if (tzfile == NULL) {
      oz_knl_threadunlk_pr (defaultzlock);			// ... no default set up yet
      goto rtnsts;
    }
  }

  /* Scan quadtransitiontimes array for timerange to be converted */

  if (tzconvtype == OZ_DATEBIN_TZCONV_UTC2LCL) {		// UTC -> LCL transformation
    for (i = tzfile -> tzh_timecnt; --i > 0;) {			// scan the table
      if (in >= tzfile -> quadtransitiontimes[i]) break;	// for first entry that matches
    }
    j   = tzfile -> transitionindxs[i];				// get type array index
    in += tzfile -> quadgmtoffs[j];				// convert UTC to LCL
  }

  if (tzconvtype == OZ_DATEBIN_TZCONV_LCL2UTC) {		// LCL -> UTC transformation
    for (i = tzfile -> tzh_timecnt; --i > 0;) {			// scan the table
      j = tzfile -> transitionindxs[i];				// get type array index
      if (in - tzfile -> quadgmtoffs[j] >= tzfile -> quadtransitiontimes[i]) break; // for first entry that would match
    }
    in -= tzfile -> quadgmtoffs[j];				// ok, convert LCL to UTC
  }

  /* Return conversion results */

  sts = OZ_SUCCESS;						// assume all outputs successful

  if (out != NULL) sts = oz_knl_section_uput (cprocmode, sizeof *out, &in, out);

  if ((sts == OZ_SUCCESS) && (tznameoutl > 0)) {
    p = tzfile -> zonenames + tzfile -> zoneinfos[j].nameidx;
    i = strnlen (p, tznameoutl - 1) + 1;
    sts = oz_knl_section_uput (cprocmode, i, p, tznameout);
  }

  /* Close or unlock file */

  if (h_tzfilein != 0) closetzfile (tzfile);
  else oz_knl_threadunlk_pr (defaultzlock);

rtnsts:
  oz_hw_cpu_setsoftint (si);
  return (sts);
}

static uLong readtzfile (OZ_Handle h_tzfile, OZ_Procmode mprocmode, Tzfile **tzfile_r)

{
  Long i, lastlong, thislong, timecnt, typecnt;
  OZ_Handle h_section;
  OZ_IO_fs_getinfo1 fs_getinfo1;
  OZ_IO_fs_readblocks fs_readblocks;
  OZ_Mempage npagem, svpage, svpage2;
  OZ_Section *section;
  Tzfile *tzfile;
  uByte *filebuff, *p;
  uLong datelongs[OZ_DATELONG_ELEMENTS], filesize, sts;

  static OZ_Datebin basetime = 0;

  /* Read TZ file into memorie */

  memset (&fs_getinfo1, 0, sizeof fs_getinfo1);
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_tzfile, 0, OZ_IO_FS_GETINFO1, sizeof fs_getinfo1, &fs_getinfo1);
  if (sts != OZ_SUCCESS) return (sts);

  filesize = (fs_getinfo1.eofblock - 1) * fs_getinfo1.blocksize + fs_getinfo1.eofbyte;
  filebuff = oz_sys_pdata_malloc (mprocmode, filesize);

  memset (&fs_readblocks, 0, sizeof fs_readblocks);
  fs_readblocks.size = filesize;
  fs_readblocks.buff = filebuff;
  fs_readblocks.svbn = 1;
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_tzfile, 0, OZ_IO_FS_READBLOCKS, sizeof fs_readblocks, &fs_readblocks);
  if (sts != OZ_SUCCESS) {
    oz_sys_pdata_free (mprocmode, filebuff);
    return (sts);
  }

  /* Fill in control struct */

  p  = filebuff;
  if (memcmp (p, "TZif", 4) != 0) goto badtzfile2;

  /* Malloc a control struct */

  timecnt = TZLONG (p + 32);
  typecnt = TZLONG (p + 36);
  if ((timecnt <= 0) || (typecnt <= 0)) goto badtzfile;
  i = (timecnt + typecnt) * sizeof tzfile -> quadtransitiontimes[0] + sizeof *tzfile;

  tzfile = oz_sys_pdata_malloc (mprocmode, i);
  if (tzfile == NULL) {
    oz_sys_pdata_free (mprocmode, filebuff);
    return (OZ_NOMEMORY);
  }
  tzfile -> mprocmode   = mprocmode;
  tzfile -> filesize    = filesize;
  tzfile -> filebuff    = filebuff;
  tzfile -> quadgmtoffs = tzfile -> quadtransitiontimes + timecnt;

  /* Fill in control struct */

  p += 32;	// skip TZif, 16 reserved bytes, ttisgmtcnt, ttisstdcnt, leapcnt

  /* Get the array lengths */

  tzfile -> tzh_timecnt = timecnt;    p += 4;
  tzfile -> tzh_typecnt = typecnt;    p += 4;
  tzfile -> tzh_charcnt = TZLONG (p); p += 4;
  if (tzfile -> tzh_charcnt <= 0) goto badtzfile;

  /* Make sure the arrays don't go off the end of the file */

  if (44 + timecnt * sizeof (Long) 
         + timecnt * sizeof (uByte) 
         + typecnt * sizeof (Tzinfo) 
         + tzfile -> tzh_charcnt > filesize) goto badtzfile;

  /* Get the transition times.  They are a bunch of unix-style time longs  */
  /* in big-endian format.  They are signed to indicate dates before 1970. */

  if (basetime == 0) {
    memset (datelongs, 0, sizeof datelongs);
    datelongs[OZ_DATELONG_DAYNUMBER] = oz_sys_daynumber_encode ((1970 << 16) | (1 << 8) | 1);
    basetime = oz_sys_datebin_encode (datelongs);
  }

  for (i = 0; i < timecnt; i ++) {
    thislong = TZLONG (p);
    if ((i > 0) && (thislong <= lastlong)) goto badtzfile;
    tzfile -> quadtransitiontimes[i] = basetime + OZ_TIMER_RESOLUTION * (Quad)thislong;
    lastlong = thislong;
    p += 4;
  }

  /* Next is an array of corresponding timezone type indicies, one byte each */

  tzfile -> transitionindxs = p;
  for (i = 0; i < timecnt; i ++) {
    if (*(p ++) >= typecnt) goto badtzfile;
  }

  /* Then come the Tzinfo structs that have the gmt offset, isdst and name string index */

  tzfile -> zoneinfos = (void *)p;
  p += typecnt * sizeof *(tzfile -> zoneinfos);
  for (i = 0; i < typecnt; i ++) {
    if (tzfile -> zoneinfos[i].isdst > 1) goto badtzfile;
    if (tzfile -> zoneinfos[i].nameidx >= tzfile -> tzh_charcnt) goto badtzfile;
    tzfile -> quadgmtoffs[i] = OZ_TIMER_RESOLUTION * (Quad)TZLONG (tzfile -> zoneinfos[i].gmtoffs);
  }

  /* Finally are the name strings */

  tzfile -> zonenames = (void *)p;
  p += tzfile -> tzh_charcnt;

  *tzfile_r = tzfile;
  return (OZ_SUCCESS);

  /* Various error detections jump here */

badtzfile:
  oz_sys_pdata_free (mprocmode, tzfile);
badtzfile2:
  oz_sys_pdata_free (mprocmode, filebuff);
  return (OZ_BADTZFILE);
}

static void closetzfile (Tzfile *tzfile)

{
  oz_sys_pdata_free (tzfile -> mprocmode, tzfile -> filebuff);
  oz_sys_pdata_free (tzfile -> mprocmode, tzfile);
}
