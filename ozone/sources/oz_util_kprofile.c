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
/*  Start/Stop kernel profiling						*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_crtl_malloc.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_status.h"
#include "oz_sys_callknl.h"
#include "oz_sys_io.h"
#include "oz_sys_io_fs.h"
#include "oz_sys_io_fs_printf.h"
#include "oz_sys_pdata.h"
#include "oz_util_start.h"

extern OZ_Smplock *oz_hw_profile_lock;
extern uLong *oz_hw_profile_array;
extern uByte *oz_hw_profile_base;
extern uLong  oz_hw_profile_count;
extern uLong  oz_hw_profile_scale;
extern uLong  oz_hw_profile_size;

static uLong *newarray, *oldarray;
static uByte *newbase,  *oldbase;
static uLong  newcount,  oldcount;
static uLong  newscale,  oldscale;
static uLong  newsize,   oldsize;

static char *pn = "kprofile";

static uLong profknl (OZ_Procmode cprocmode, void *dummy);

uLong oz_util_main (int argc, char *argv[])

{
  int usedup;
  uLong begaddr, code_offset, endaddr, n, resolution, sts;

  if (argc > 0) pn = argv[0];

  if ((argc != 1) && (argc != 4)) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: usage: %s [<begaddr_incl> <endaddr_incl> <resolution_bytes>]\n", pn, pn);
    return (OZ_BADPARAM);
  }

  newarray = NULL;
  newbase  = NULL;
  newscale = 0;
  newsize  = 0;

  if (argc == 4) {
    begaddr = oz_hw_atoz (argv[1], &usedup);
    if ((usedup == 0) || (argv[1][usedup] != 0)) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: bad beginning address %s\n", argv[1]);
      return (OZ_BADPARAM);
    }
    endaddr = oz_hw_atoz (argv[2], &usedup);
    if ((usedup == 0) || (argv[2][usedup] != 0)) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: bad end address %s\n", argv[2]);
      return (OZ_BADPARAM);
    }
    resolution = oz_hw_atoz (argv[3], &usedup);
    if ((usedup == 0) || (argv[3][usedup] != 0)) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: bad resolution %s\n", argv[3]);
      return (OZ_BADPARAM);
    }
    if (endaddr <= begaddr) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: end address (%x) must be .gt. beg address (%x)\n", pn, endaddr, begaddr);
      return (OZ_BADPARAM);
    }
    if (resolution == 0) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: resolution must not be zero\n", pn);
      return (OZ_BADPARAM);
    }

    newbase  = (uByte *)begaddr;
    newsize  = endaddr - begaddr;
    newscale = resolution;

    if (newsize / newscale >= 256*1024) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: array size too big (%u)\n", newsize / newscale * 4);
      return (OZ_BADPARAM);
    }
  }

  oldarray = NULL;

  sts = oz_sys_callknl (profknl, NULL);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u doing profile\n", pn, sts);
    return (sts);
  }

  if (oldarray != NULL) {
    oz_sys_io_fs_printf (oz_util_h_output, "Count: %10u\n", oldcount);
    while ((oldsize >= oldscale) && (oldarray[(oldsize-1)/oldscale] == 0)) oldsize -= oldscale;
    for (code_offset = 0; code_offset < oldsize; code_offset += oldscale) {
      if (n != 0) break;
    }
    for (; code_offset < oldsize; code_offset += oldscale) {
      n = oldarray[code_offset/oldscale];
      oz_sys_io_fs_printf (oz_util_h_output, "  %3u%%  %10u : %8.8x\n", n * 100 / oldcount, n, code_offset + oldbase);
    }
  }

  return (OZ_SUCCESS);
}

static uLong profknl (OZ_Procmode cprocmode, void *dummy)

{
  int si;
  uLong lock, *olduser;

  if (oz_hw_profile_lock == NULL) return (OZ_NOTIMPLEMENTED);

  si = oz_hw_cpu_setsoftint (0);

  /* Read current values and shut off counting */

  lock = oz_hw_smplock_wait (oz_hw_profile_lock);
  oldarray = oz_hw_profile_array;
  oldbase  = oz_hw_profile_base;
  oldcount = oz_hw_profile_count;
  oldscale = oz_hw_profile_scale;
  oldsize  = oz_hw_profile_size;
  oz_hw_profile_array = NULL;
  oz_hw_smplock_clr (oz_hw_profile_lock, lock);

  /* See if they are setting up a new array */

  if (newscale != 0) {
    newarray = OZ_KNL_NPPMALLOQ (newsize / newscale * 4 + 4);	/* ok, malloc the new array */
    if (newarray == NULL) {
      if (oldarray != NULL) OZ_KNL_NPPFREE (oldarray);
      oz_hw_cpu_setsoftint (si);
      return (OZ_EXQUOTANPP);
    }
    memset (newarray, 0, newsize / newscale * 4);		/* clear all the counters */

    lock = oz_hw_smplock_wait (oz_hw_profile_lock);
    oz_hw_profile_size  = newsize;				/* set up new array's parameters */
    oz_hw_profile_scale = newscale;
    oz_hw_profile_count = 0;
    oz_hw_profile_base  = newbase;
    oz_hw_profile_array = newarray;				/* turn on new array */
    oz_hw_smplock_clr (oz_hw_profile_lock, lock);
  }

  /* Copy old kernel array (if any) to user mode array */

  if (oldarray != NULL) {					/* see if there was an old array */
    olduser  = oz_sys_pdata_malloc (cprocmode, oldsize / oldscale * 4); /* if so, malloc equivalent size user mode array */
    memcpy (olduser, oldarray, oldsize / oldscale * 4);		/* copy to the user mode array */
    OZ_KNL_NPPFREE (oldarray);					/* free off the kernel memory */
    oldarray = olduser;						/* return pointer to user mode array */
  }

  oz_hw_cpu_setsoftint (si);
  return (OZ_SUCCESS);
}
