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
/*  This routine is called by oz_crtl_start or oz_util_start to maybe 	*/
/*  start profiling for the image					*/
/*									*/
/*  Profiling is enabled by creating a logical name OZ_PROFILE and 	*/
/*  setting it to these values:						*/
/*									*/
/*	OZ_PROFILE[0] = name of file to write profiling to on exit	*/
/*	          [1] = address to start profiling at (hex, inclusive)	*/
/*	          [2] = address to end profiling at (hex, inclusive)	*/
/*	          [3] = step value for the addresses			*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_crtl_malloc.h"
#include "oz_io_fs.h"
#include "oz_knl_hw.h"
#include "oz_knl_status.h"
#include "oz_sys_dateconv.h"
#include "oz_sys_event.h"
#include "oz_sys_exhand.h"
#include "oz_sys_handle.h"
#include "oz_sys_io.h"
#include "oz_sys_io_fs.h"
#include "oz_sys_io_fs_printf.h"
#include "oz_sys_logname.h"
#include "oz_usr_profile.h"

static OZ_Handle h_event, h_file;
static OZ_Pointer hilimit, lolimit, step;
static uLong *hitarray = NULL;
static uLong totalhits = 0;

static void profile_ast (void *dummy, uLong status, OZ_Mchargs *mchargs);
static void profile_exit (void *dummy, uLong status);

void oz_usr_profile_start (int argc, char *argv[])

{
  char filename[256], hilimitstr[32], lolimitstr[32], stepstr[32];
  int i, usedup;
  OZ_Handle h_name, h_table;
  OZ_IO_fs_create fs_create;
  uLong sts;

  uLong buflens[4] = { sizeof filename, sizeof lolimitstr, sizeof hilimitstr, sizeof stepstr };
  char *bufadrs[4] = {        filename,        lolimitstr,        hilimitstr,        stepstr };

  /* Get handle to default logical name tables */

  sts = oz_sys_logname_lookup (0, OZ_PROCMODE_USR, "OZ_DEFAULT_TBL", NULL, NULL, NULL, &h_table);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printerror ("oz_usr_profile_start: error %u getting logical name table handle\n", sts);
    return;
  }

  /* Look for the OZ_PROFILE logical name.  If not found, quietly exit. */

  sts = oz_sys_logname_lookup (h_table, OZ_PROCMODE_USR, "OZ_PROFILE", NULL, NULL, NULL, &h_name);
  oz_sys_handle_release (OZ_PROCMODE_KNL, h_table);
  if (sts != OZ_SUCCESS) {
    if (sts != OZ_NOLOGNAME) oz_sys_io_fs_printerror ("oz_usr_profile_start: error %u getting logical name table handle\n", sts);
    return;
  }

  /* Get the values of all four strings */

  for (i = 0; i < 4; i ++) {
    sts = oz_sys_logname_getval (h_name, i, NULL, buflens[i], bufadrs[i], NULL, NULL, 0, NULL);
    if (sts != OZ_SUCCESS) {
      oz_sys_handle_release (OZ_PROCMODE_KNL, h_name);
      oz_sys_io_fs_printerror ("oz_usr_profile_start: error %u getting logical name value[%d]\n", sts, i);
      return;
    }
  }

  /* All done with logical name */

  oz_sys_handle_release (OZ_PROCMODE_KNL, h_name);

  /* Try to create the profiling file */

  memset (&fs_create, 0, sizeof fs_create);
  fs_create.name = filename;
  fs_create.lockmode = OZ_LOCKMODE_PW;
  sts = oz_sys_io_fs_create (sizeof fs_create, &fs_create, 0, &h_file);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printerror ("oz_usr_profile_start: error %u creating file %s\n", sts, filename);
    return;
  }

  /* Decode the address parameters and write to profiling file */

  lolimit = oz_hw_atoz (lolimitstr, &usedup);
  hilimit = oz_hw_atoz (hilimitstr, &usedup);
  step    = oz_hw_atoz (stepstr, &usedup);

  oz_sys_io_fs_printf (h_file, "0x%8.8X..%8.8X  /  0x%X\n", lolimit, hilimit, step);

  /* Write command line args to profiling file so they know what it is for */

  for (i = 0; i < argc; i ++) {
    oz_sys_io_fs_printf (h_file, " argv[%3d]: %s\n", i, argv[i]);
  }

  /* Make sure address parameters make sense */

  if (hilimit <= lolimit) {
    oz_sys_io_fs_printerror ("oz_usr_profile_start: high limit %8.8X must be .ge. low limit %8.8X\n", hilimit, lolimit);
    return;
  }
  if (step == 0) {
    oz_sys_io_fs_printerror ("oz_usr_profile_start: step must be non-zero\n");
    return;
  }

  /* Malloc and clear the hit counter array */

  hitarray = malloc (((hilimit - lolimit) / step + 1) * sizeof hitarray[0]);
  memset (hitarray, 0, ((hilimit - lolimit) / step + 1) * sizeof hitarray[0]);

  /* Create an handler to be called on exit so it can write the file from the array */

  sts = oz_sys_exhand_create (OZ_PROCMODE_USR, profile_exit, NULL);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printerror ("oz_usr_profile_start: error %u creating exit handler\n", sts);
    return;
  }

  /* Create an event flag for the periodic ast's */

  sts = oz_sys_event_create (OZ_PROCMODE_KNL, "usr profile", &h_event);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printerror ("oz_usr_profile_start: error %u creating event flag\n", sts);
    return;
  }

  /* Set up a periodic increment of the event flag */

  sts = oz_sys_event_setimint (h_event, OZ_TIMER_RESOLUTION / 10, oz_hw_tod_getnow ());
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printerror ("oz_usr_profile_start: error %u starting timer\n", sts);
    return;
  }

  /* Set up an express ast when the event flag gets incremented */

  sts = oz_sys_event_ast (OZ_PROCMODE_KNL, h_event, profile_ast, NULL, 1);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printerror ("oz_usr_profile_start: error %u queuing ast\n", sts);
    return;
  }

  oz_sys_io_fs_printerror ("oz_usr_profile_start: profiling started\n");
  return;
}

static void profile_ast (void *dummy, uLong status, OZ_Mchargs *mchargs)

{
  int i;
  Long incby;
  OZ_Pointer pc;
  uLong sts;

  /* Clear the event flag and get how many intervals have passed */

  sts = oz_sys_event_set (OZ_PROCMODE_KNL, h_event, 0, &incby);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printerror ("oz_usr_profile_start: error %u clearing event\n", sts);
    return;
  }

  /* Increment the hit counter for each call level */

  for (i = 1;; i ++) {
    pc = (OZ_Pointer)oz_hw_getrtnadr (i);
    if (pc == 0) break;
    if (pc < lolimit) continue;
    if (pc >= hilimit) continue;
    hitarray[(pc-lolimit)/step] += incby;
  }

  /* Increment total number of hits taken */

  totalhits += incby;

  /* Set up express ast for next interval */

  sts = oz_sys_event_ast (OZ_PROCMODE_KNL, h_event, profile_ast, NULL, 1);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printerror ("oz_usr_profile_start: error %u queuing ast\n", sts);
    return;
  }
}

/* This routine is called when the main thread exits */

static void profile_exit (void *dummy, uLong status)

{
  OZ_Pointer pc;
  uLong hits;

  /* Clear timer */

  oz_sys_event_setimint (h_event, 0, 0);

  /* Write hit counts out to file and close it */

  oz_sys_io_fs_printerror ("oz_usr_profile_start: writing profile\n");
  if (totalhits != 0) {
    for (pc = lolimit; pc <= hilimit; pc += step) {
      hits = hitarray[(pc-lolimit)/step];
      if (hits != 0) oz_sys_io_fs_printf (h_file, "    %3u%%  %8u : %8.8X\n", hits * 100 / totalhits, hits, pc);
    }
  }
  oz_sys_io_fs_printf (h_file, " [total hits %u, exit status %u]\n", totalhits, status);
  oz_sys_io (OZ_PROCMODE_KNL, h_file, 0, OZ_IO_FS_CLOSE, 0, NULL);
  oz_sys_io_fs_printerror ("oz_usr_profile_start: profile complete\n");
}
