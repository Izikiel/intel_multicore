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
/*  C Runtime library start routine					*/
/*									*/
/*    Input:								*/
/*									*/
/*	image = null terminated image name string			*/
/*	logical OZ_INPUT  = stdin file					*/
/*	        OZ_OUTPUT = stdout file					*/
/*	        OZ_ERROR  = stderr file					*/
/*	        OZ_PARAMS = command line arguments			*/
/*	        OZ_ENVARS = environmental variables			*/
/*									*/
/*    It calles a subroutine named 'main' with stdin, stdout and 	*/
/*    stderr streams opened.  It will also start the debugger if an 	*/
/*    OZ_DEBUG logical is defined and set to an odd value.		*/
/*									*/
/************************************************************************/

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

/* ?? maybe make these three completely dynamic someday ?? */

#define MAXENVC 256

#include "ozone.h"

#include "oz_crtl_fio.h"
#include "oz_crtl_malloc.h"
#include "oz_knl_hw.h"
#include "oz_knl_sdata.h"
#include "oz_knl_status.h"
#include "oz_sys_handle.h"
#include "oz_sys_logname.h"
#include "oz_usr_debug.h"
#include "oz_usr_profile.h"

char **environ;

static uLong finishup (void *imagev);

int _start (char *image)

{
  char debugb[4];
  int fd;
  uLong debugc, sts;
  OZ_Handle h_logname, h_table;

  /* Open stdin/stdout/stderr files */

  fd = open ("OZ_INPUT", O_RDONLY);
  if (fd != STDIN_FILENO) {
    oz_sys_io_fs_printerror ("oz_crtl_start: STDIN opened as fileno %d, errno %d", fd, errno);
    return (-1);
  }
  fd = open ("OZ_OUTPUT", O_WRONLY | O_APPEND);
  if (fd != STDOUT_FILENO) {
    oz_sys_io_fs_printerror ("oz_crtl_start: STDOUT opened as fileno %d, errno %d", fd, errno);
    return (-1);
  }
  fd = open ("OZ_ERROR", O_WRONLY | O_APPEND);
  if (fd != STDERR_FILENO) {
    oz_sys_io_fs_printerror ("oz_crtl_start: STDERR opened as fileno %d, errno %d", fd, errno);
    return (-1);
  }

  /* Open them as streams now */

  stdin = fdopen (STDIN_FILENO, "r");
  if (stdin == NULL) {
    oz_sys_io_fs_printerror ("oz_crtl_start: stdin open error %d\n", errno);
    return (-1);
  }

  stdout = fdopen (STDOUT_FILENO, "w+");
  if (stdout == NULL) {
    oz_sys_io_fs_printerror ("oz_crtl_start: stdout open error %d\n", errno);
    return (-1);
  }

  stderr = fdopen (STDERR_FILENO, "w+");
  if (stderr == NULL) {
    oz_sys_io_fs_printerror ("oz_crtl_start: stderr open error %d\n", errno);
    return (-1);
  }
  setvbuf (stderr, NULL, _IONBF, 0);

  /* Get OZ_DEBUG to see if the debugger should be used */

  debugb[0] = 0;

  sts = oz_sys_logname_lookup (0, OZ_PROCMODE_USR, oz_s_logname_defaulttables, NULL, NULL, NULL, &h_table);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printerror ("oz_crtl_start: error %u looking up default tables (%s)\n", sts, oz_s_logname_defaulttables);
    return (sts);
  }

  sts = oz_sys_logname_lookup (h_table, OZ_PROCMODE_USR, "OZ_DEBUG", NULL, NULL, &debugc, &h_logname);
  if ((sts != OZ_SUCCESS) && (sts != OZ_NOLOGNAME)) {
    oz_sys_io_fs_printerror ("oz_crtl_start: error %u looking up OZ_DEBUG\n", sts);
    return (sts);
  }

  if (sts == OZ_SUCCESS) {
    if (debugc > 0) {
      sts = oz_sys_logname_getval (h_logname, 0, NULL, sizeof debugb, debugb, NULL, NULL, 0, NULL);
      if (sts != OZ_SUCCESS) {
        oz_sys_io_fs_printerror ("oz_crtl_start: error %u getting OZ_DEBUG string\n", sts);
        return (sts);
      }
    }
    oz_sys_handle_release (OZ_PROCMODE_KNL, h_logname);
  }

  oz_sys_handle_release (OZ_PROCMODE_KNL, h_table);

  /* Call the debugger init routine to handle the rest */

  if (debugb[0] & 1) {
    sts = oz_usr_debug_init (finishup, image);
  } else {
    sts = finishup (image);
  }
  return (sts);
}

static uLong finishup (void *imagev)

{
  char **argv, *envv[MAXENVC+1];
  int envc, fd, rc;
  uLong argc, i, rlen, sts;
  OZ_Handle h_lookup;

  /* Initialize the 'signal' emulator routines */

  oz_crtl_signal_init ();

  /* Get logical OZ_PARAMS - it contains the command line */

  sts = oz_sys_logname_lookup (0, OZ_PROCMODE_USR, "OZ_DEFAULT_TBL%OZ_PARAMS", NULL, NULL, &argc, &h_lookup);
  if (sts == OZ_NOLOGNAME) {
    argc = 1;
    argv = malloc (2 * sizeof *argv);
    argv[0] = imagev;
    goto got_params;
  }
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printerror ("oz_ctrl_start: error %u looking up OZ_DEFAULT_TBL%OZ_PARAMS", sts);
    return (-1);
  }
  argv = malloc ((argc + 1) * sizeof *argv);
  for (i = 0; i < argc; i ++) {
    sts = oz_sys_logname_getval (h_lookup, i, NULL, 0, NULL, &rlen, NULL, 0, NULL);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printerror ("oz_ctrl_start: error %u looking up OZ_PARAMS[%d]", sts, i);
      return (-1);
    }
    argv[i] = malloc (rlen + 1);
    sts = oz_sys_logname_getval (h_lookup, i, NULL, rlen + 1, argv[i], NULL, NULL, 0, NULL);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printerror ("oz_ctrl_start: error %u looking up OZ_PARAMS[%d]", sts, i);
      return (-1);
    }
  }
  oz_sys_handle_release (OZ_PROCMODE_KNL, h_lookup);
got_params:
  argv[argc] = NULL;

  /* Get logical OZ_ENVARS - it contains the environmental variables */

  h_lookup = 0;
#if 1
  envc = 0;
#else
  for (envc = 0; envc < MAXENVC; envc ++) {
    envv[envc] = malloc (MAXLINE);
    sts = oz_sys_logname_search (&h_lookup, OZ_PROCMODE_KNL, oz_s_logname_defaulttables, "OZ_ENVARS", MAXLINE, envv[envc]);
    if (sts == OZ_NOLOGNAME) break;
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printerror ("oz_ctrl_start: error %u looking up OZ_ENVARS[%d] in %s", sts, envc, oz_s_logname_defaulttables);
      return (-1);
    }
  }
  if (h_lookup != 0) oz_sys_handle_release (OZ_PROCMODE_KNL, h_lookup);
#endif
  envv[envc] = NULL;
  environ = envv;

  /* Maybe start profiling */

#if !defined (OZ_HW_TYPE_AXP)
  oz_usr_profile_start (argc, argv);
#endif

  /* Call the 'main' function */

  rc = main (argc, argv, envv);

  /* Close the streams and exit */

  fclose (stdin);
  fclose (stdout);
  fclose (stderr);

  /* If main retuned zero, exit with OZ_SUCCESS      */
  /* Otherwise, add the return value to OZ_CRTL_EXIT */

  if (rc == 0) return (OZ_SUCCESS);
  return ((rc & 0xFF) + OZ_CRTL_EXIT);
}
