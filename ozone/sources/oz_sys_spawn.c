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
/*  Fork and exec a new process						*/
/*									*/
/*    Input:								*/
/*									*/
/*	h_job    = job handle, 0 for same job as caller			*/
/*	image    = image name to execute				*/
/*	h_input  = input file, 0 for caller's input file		*/
/*	h_output = output file, 0 for caller's output file		*/
/*	h_error  = error file, 0 for caller's error file		*/
/*	h_init   = init event handle, 0 if not wanted			*/
/*	h_exit   = exit event handle, 0 if not wanted			*/
/*	defdir   = default directory string, NULL to copy caller's	*/
/*	params   = parameter string to pass to executable		*/
/*	name     = process / thread name				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_spawn = OZ_SUCCESS : successfully started		*/
/*	                     else : error status			*/
/*	*thread_h_r  = thread handle					*/
/*	*process_h_r = process handle					*/
/*									*/
/************************************************************************/

#include "ozone.h"

#include "oz_knl_event.h"
#include "oz_knl_handle.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_logname.h"
#include "oz_knl_process.h"
#include "oz_knl_quota.h"
#include "oz_knl_sdata.h"
#include "oz_knl_section.h"
#include "oz_knl_status.h"
#include "oz_knl_thread.h"

#include "oz_sys_io_fs_printf.h"
#include "oz_sys_logname.h"
#include "oz_sys_spawn.h"
#include "oz_sys_syscall.h"

#define MAXARGSTRLEN (4096)

static char *image_tbls[1] = { "OZ_DEFAULT_TBL" };
static const char *defdirname = "OZ_DEFAULT_DIR";

static uLong getustrz (OZ_Procmode cprocmode, const char **ustrz);
static uLong getiochan (OZ_Handle h, OZ_Logname *deftables, char *namestr, OZ_Iochan **iochan_r, OZ_Procmode cprocmode);
static uLong spawn_thread (void *dummy);

OZ_HW_SYSCALL_DEF_13 (spawn, OZ_Handle, h_job, 
                             const char *, image, 
                             OZ_Handle, h_input, 
                             OZ_Handle, h_output, 
                             OZ_Handle, h_error, 
                             OZ_Handle, h_init, 
                             OZ_Handle, h_exit, 
                             const char *, defdir, 
                             int, nparams, 
                             const char **, paramv, 
                             const char *, name, 
                             OZ_Handle *, thread_h_r, 
                             OZ_Handle *, process_h_r)

{
  char *p;
  const char **values;
  int free_defdir, free_image, free_name, free_paramv, i, name_l, si;
  OZ_Event *event_exit, *event_init;
  OZ_Iochan *iochan_error, *iochan_input, *iochan_output;
  OZ_Job *job;
  OZ_Logname *logname_defdir, *logname_deftables, *logname_error, *logname_image, *logname_imgdir, *logname_input, *logname_output, *logname_params, *logname_parent;
  OZ_Logname *process_directory, *process_table;
  OZ_Logvalue *logvalues, *search_values;
  OZ_Process *process;
  OZ_Secattr *secattr;
  OZ_Seckeys *seckeys;
  OZ_Seclock *sl_process_h_r, *sl_thread_h_r;
  OZ_Thread *thread;
  uLong nvalues, priority, search_index, sts;

  event_exit        = NULL;
  event_init        = NULL;
  free_image        = 0;
  free_defdir       = 0;
  free_name         = 0;
  free_paramv       = 0;
  iochan_error      = NULL;
  iochan_input      = NULL;
  iochan_output     = NULL;
  job               = NULL;
  logname_defdir    = NULL;
  logname_deftables = NULL;
  logname_error     = NULL;
  logname_image     = NULL;
  logname_imgdir    = NULL;
  logname_input     = NULL;
  logname_output    = NULL;
  logname_params    = NULL;
  logname_parent    = NULL;
  logvalues         = NULL;
  process_directory = NULL;
  process_table     = NULL;
  secattr           = NULL;
  process           = NULL;
  seckeys           = NULL;
  sl_thread_h_r     = NULL;
  sl_process_h_r    = NULL;
  thread            = NULL;

  /* Can't create logical with strings larger than OZ_LOGNAME_SIZEMAX, so make sure nparams is smaller than that */

  if (nparams >= OZ_LOGNAME_SIZEMAX / 2) return (OZ_BADPARAM);

  /* Keep from being aborted so we can clean up properly */

  si = oz_hw_cpu_setsoftint (0);

  /* Probe and lock call params */

  if (cprocmode != OZ_PROCMODE_KNL) {
    sts = getustrz (cprocmode, &image);
    if (sts != OZ_SUCCESS) goto rtn;
    free_image = 1;
    if (defdir != NULL) {
      sts = getustrz (cprocmode, &defdir);
      if (sts != OZ_SUCCESS) goto rtn;
      free_defdir = 1;
    }
    if (name != NULL) {
      sts = getustrz (cprocmode, &name);
      if (sts != OZ_SUCCESS) goto rtn;
      free_name = 1;
    }
    if (nparams > 0) {
      values = OZ_KNL_PGPMALLOQ (nparams * sizeof values[0]);
      if (values == NULL) {
        sts = OZ_EXQUOTAPGP;
        goto rtn;
      }
      sts = oz_knl_section_uget (cprocmode, nparams * sizeof values[0], paramv, values);
      free_paramv = 1;
      paramv = values;
      if (sts != OZ_SUCCESS) goto rtn;
      for (i = 0; i < nparams; i ++) {
        sts = getustrz (cprocmode, paramv + i);
        if (sts != OZ_SUCCESS) goto rtn;
        free_paramv ++;
      }
    }
    if (thread_h_r  != NULL) {
      sts = oz_knl_section_blockw (cprocmode, sizeof *thread_h_r,  thread_h_r,  &sl_thread_h_r);
      if (sts != OZ_SUCCESS) goto rtn;
    }
    if (process_h_r != NULL) {
      sts = oz_knl_section_blockw (cprocmode, sizeof *process_h_r, process_h_r, &sl_process_h_r);
      if (sts != OZ_SUCCESS) goto rtn;
    }
  }
  if (name == NULL) name = image;

  /* Get job pointer */

  if (h_job == 0) job = oz_knl_process_getjob (NULL);
  else {
    sts = oz_knl_handle_takeout (h_job, cprocmode, OZ_SECACCMSK_WRITE, OZ_OBJTYPE_JOB, &job, NULL);
    if (sts != OZ_SUCCESS) goto rtn;
  }

  /* Maybe we will need the default logical name tables */

  if ((h_input == 0) || (h_output == 0) || (h_error == 0) || (defdir == NULL)) {
    sts = oz_knl_logname_lookup (NULL, cprocmode, strlen (oz_s_logname_defaulttables), oz_s_logname_defaulttables, 
                                 NULL, NULL, NULL, NULL, &logname_deftables, NULL);
    if (sts != OZ_SUCCESS) goto rtn;
  }

  /* Get input, output and error I/O channels */
  /* For those given as 0, use caller's files */

  sts = getiochan (h_input,  logname_deftables, "OZ_INPUT",  &iochan_input,  cprocmode);
  if (sts != OZ_SUCCESS) goto rtn;
  sts = getiochan (h_output, logname_deftables, "OZ_OUTPUT", &iochan_output, cprocmode);
  if (sts != OZ_SUCCESS) goto rtn;
  sts = getiochan (h_error,  logname_deftables, "OZ_ERROR",  &iochan_error,  cprocmode);
  if (sts != OZ_SUCCESS) goto rtn;

  /* Get init and exit event flags */

  if (h_init != 0) {
    sts = oz_knl_handle_takeout (h_init, cprocmode, OZ_SECACCMSK_READ, OZ_OBJTYPE_EVENT, &event_init, NULL);
    if (sts != OZ_SUCCESS) goto rtn;
  }

  if (h_exit != 0) {
    sts = oz_knl_handle_takeout (h_exit, cprocmode, OZ_SECACCMSK_WRITE, OZ_OBJTYPE_EVENT, &event_exit, NULL);
    if (sts != OZ_SUCCESS) goto rtn;
  }

  /* Get security attributes */

  secattr = oz_knl_thread_getdefcresecattr (NULL);
  sts = oz_knl_secattr_fromname (OZ_PROCESS_NAMESIZE, name, &name_l, NULL, &secattr);
  if (sts != OZ_SUCCESS) goto rtn;
  seckeys = oz_knl_thread_getseckeys (NULL);

  /* Create the process */

  sts = oz_knl_process_create (job, 0, 0, name_l, name, secattr, &process);
  if (sts != OZ_SUCCESS) goto rtn;
  process_directory = oz_knl_process_getlognamdir (process);
  process_table     = oz_knl_process_getlognamtbl (process);

  /* If it is a sub-process (ie, h_job is zero), create OZ_PARENT_TABLE to point to its parent's (ie, my) process logical name table */

  if (h_job == 0) {
    sts = oz_knl_logname_creobj (process_directory, OZ_PROCMODE_KNL, seckeys, secattr, 0, 15, "OZ_PARENT_TABLE", oz_knl_process_getlognamtbl (NULL), &logname_parent);
    if (sts != OZ_SUCCESS) goto rtn;
  }

  /* Create logical name for the image to be executed */

  sts = oz_knl_logname_crestr (process_table, OZ_PROCMODE_KNL, seckeys, secattr, 0, 8, "OZ_IMAGE", image, &logname_image);
  if (sts != OZ_SUCCESS) goto rtn;

  /* Create logical names for the input, output and error files */

  sts = oz_knl_logname_creobj (process_table, OZ_PROCMODE_KNL, seckeys, secattr, 0, 8, "OZ_INPUT",  iochan_input,  &logname_input);
  if (sts != OZ_SUCCESS) goto rtn;
  sts = oz_knl_logname_creobj (process_table, OZ_PROCMODE_KNL, seckeys, secattr, 0, 9, "OZ_OUTPUT", iochan_output, &logname_output);
  if (sts != OZ_SUCCESS) goto rtn;
  sts = oz_knl_logname_creobj (process_table, OZ_PROCMODE_KNL, seckeys, secattr, 0, 8, "OZ_ERROR",  iochan_error,  &logname_error);
  if (sts != OZ_SUCCESS) goto rtn;

  /* Create logical name for the parameter string */

  if (nparams > 0) {
    logvalues = OZ_KNL_PGPMALLOQ (nparams * sizeof *logvalues);
    if (logvalues == NULL) {
      sts = OZ_EXQUOTAPGP;
      goto rtn;
    }
    for (i = 0; i < nparams; i ++) {
      logvalues[i].attr = OZ_LOGVALATR_TERMINAL;
      logvalues[i].buff = (void *)(paramv[i]);
    }
  }

  sts = oz_knl_logname_create (process_table, OZ_PROCMODE_KNL, seckeys, secattr, 0, 9, "OZ_PARAMS", nparams, logvalues, &logname_params);
  if (sts != OZ_SUCCESS) goto rtn;

  /* Create OZ_DEFAULT_DIR logical name for the default directory string */
  /* Copy caller's if none given                                         */

  if (defdir != NULL) {
    sts = oz_knl_logname_crestr (process_table, OZ_PROCMODE_KNL, seckeys, secattr, 0, strlen (defdirname), defdirname, defdir, &logname_defdir);
    if (sts != OZ_SUCCESS) goto rtn;
  } else {
    const OZ_Logvalue *values;
    uLong lognamatr, nvalues;
    OZ_Logname *defdir_old;

    sts = oz_knl_logname_lookup (logname_deftables, cprocmode, strlen (defdirname), defdirname, NULL, &lognamatr, &nvalues, &values, &defdir_old, NULL);
    if (sts != OZ_SUCCESS) goto rtn;
    sts = oz_knl_logname_create (process_table, OZ_PROCMODE_KNL, seckeys, secattr, lognamatr, strlen (defdirname), defdirname, nvalues, values, &logname_defdir);
    oz_knl_logname_increfc (defdir_old, -1);
    if (sts != OZ_SUCCESS) goto rtn;
  }

  /* Create a thread to load and execute the image */

  priority = oz_knl_thread_getbasepri (NULL);
  sts = oz_knl_thread_create (process, priority, seckeys, event_init, event_exit, 
                              oz_s_loadparams.def_user_stack_size >> OZ_HW_L2PAGESIZE, 
                              spawn_thread, NULL, OZ_ASTMODE_ENABLE, name_l, name, secattr, 
                              &thread);
  if (sts != OZ_SUCCESS) goto rtn;

  /* If caller requested, return handles to process and/or thread */

  if (thread_h_r != NULL) {
    sts = oz_knl_handle_assign (thread, cprocmode, thread_h_r);
    if (sts != OZ_SUCCESS) goto rtn;
  }

  if (process_h_r != NULL) {
    sts = oz_knl_handle_assign (process, cprocmode, process_h_r);
    if (sts != OZ_SUCCESS) {
      if (thread_h_r != NULL) {
        oz_knl_handle_release (*thread_h_r, cprocmode);
        *thread_h_r = 0;
      }
      goto rtn;
    }
  }

  /* Clean up and return status */

rtn:
  if (free_image)  OZ_KNL_PGPFREE ((void *)image);
  if (free_defdir) OZ_KNL_PGPFREE ((void *)defdir);
  if (free_name)   OZ_KNL_PGPFREE ((void *)name);
  if (free_paramv != 0) {
    for (i = 0; i < free_paramv - 1; i ++) OZ_KNL_PGPFREE ((void *)(paramv[i]));
    OZ_KNL_PGPFREE (paramv);
  }
  if (sl_thread_h_r     != NULL) oz_knl_section_bunlock (sl_thread_h_r);
  if (sl_process_h_r    != NULL) oz_knl_section_bunlock (sl_process_h_r);

  if (event_exit        != NULL) oz_knl_handle_putback  (h_exit);
  if (event_init        != NULL) oz_knl_handle_putback  (h_init);
  if (iochan_error      != NULL) oz_knl_iochan_increfc  (iochan_error,      -1);
  if (iochan_input      != NULL) oz_knl_iochan_increfc  (iochan_input,      -1);
  if (iochan_output     != NULL) oz_knl_iochan_increfc  (iochan_output,     -1);
  if ((h_job != 0) && (job != NULL)) oz_knl_handle_putback (h_job);
  if (logname_defdir    != NULL) oz_knl_logname_increfc (logname_defdir,    -1);
  if (logname_deftables != NULL) oz_knl_logname_increfc (logname_deftables, -1);
  if (logname_error     != NULL) oz_knl_logname_increfc (logname_error,     -1);
  if (logname_image     != NULL) oz_knl_logname_increfc (logname_image,     -1);
  if (logname_imgdir    != NULL) oz_knl_logname_increfc (logname_imgdir,    -1);
  if (logname_input     != NULL) oz_knl_logname_increfc (logname_input,     -1);
  if (logname_output    != NULL) oz_knl_logname_increfc (logname_output,    -1);
  if (logname_params    != NULL) oz_knl_logname_increfc (logname_params,    -1);
  if (logname_parent    != NULL) oz_knl_logname_increfc (logname_parent,    -1);
  if (logvalues         != NULL) OZ_KNL_PGPFREE (logvalues);
  if (process           != NULL) oz_knl_process_increfc (process,           -1);
  if (secattr           != NULL) oz_knl_secattr_increfc (secattr,           -1);
  if (seckeys           != NULL) oz_knl_seckeys_increfc (seckeys,           -1);
  if (thread            != NULL) oz_knl_thread_increfc  (thread,            -1);

  oz_hw_cpu_setsoftint (si);
  return (sts);
}

/* Get a user mode null terminated string and copy into paged kernel buffer */

static uLong getustrz (OZ_Procmode cprocmode, const char **ustrz)

{
  char *kstrz;
  uLong rlen, sts;
  OZ_Seclock *sl;

  sts = oz_knl_section_blockz (cprocmode, MAXARGSTRLEN, *ustrz, &rlen, &sl); /* lock usermode string in memory and get its length */
  if (sts == OZ_SUCCESS) {
    kstrz = OZ_KNL_PGPMALLOQ (rlen + 1);				/* successful, allocate a like-sized paged kernel buffer */
    if (kstrz == NULL) sts = OZ_EXQUOTAPGP;
    else {
      memcpy (kstrz, *ustrz, rlen);					/* copy user string to kernel buffer */
      kstrz[rlen] = 0;							/* null terminate kernel string */
      *ustrz = kstrz;							/* modify caller's pointer to point to kernel string */
    }
    oz_knl_section_bunlock (sl);					/* unlock user buffer */
  }
  return (sts);								/* anyway, return status */
}

/* Get the I/O channel for OZ_INPUT, OZ_OUTPUT or OZ_ERROR.                               */
/* If given as a handle be caller, use that, otherwise get from caller's logical name.    */
/* Return the I/O channel (with ref count incremented).  No other ref counts incremented. */

static uLong getiochan (OZ_Handle h, OZ_Logname *deftables, char *namestr, OZ_Iochan **iochan_r, OZ_Procmode cprocmode)

{
  uLong sts;
  OZ_Iochan *iochan;
  OZ_Logname *logname;

  /* If handle to file was given, get the I/O channel from the handle */
  
  if (h != 0) {
    sts = oz_knl_handle_takeout (h, cprocmode, OZ_SECACCMSK_READ, OZ_OBJTYPE_IOCHAN, &iochan, NULL);
    if (sts == OZ_SUCCESS) {
      oz_knl_iochan_increfc (iochan, 1);
      oz_knl_handle_putback (h);
    }
  }

  /* Otherwise, get I/O channel from caller's logical name */

  else {
    sts = oz_knl_logname_lookup (deftables, cprocmode, strlen (namestr), namestr, NULL, NULL, NULL, NULL, &logname, NULL);
    if (sts == OZ_SUCCESS) {
      sts = oz_knl_logname_getobj (logname, 0, OZ_OBJTYPE_IOCHAN, &iochan);
      oz_knl_logname_increfc (logname, -1);
    }
    else oz_knl_printk ("oz_sys_spawn: error %u looking up logical %s\n", sts, namestr);
  }

  if (sts != OZ_SUCCESS) iochan = NULL;
  *iochan_r = iochan;
  return (sts);
}

/************************************************************************/
/*									*/
/*  This is the thread executing in the target process in user mode	*/
/*									*/
/*    Input:								*/
/*									*/
/*	lnm OZ_IMAGE       = image name string				*/
/*	    OZ_IMAGE_DIR   = image directory string			*/
/*	    OZ_INPUT       = input file i/o channel			*/
/*	    OZ_OUTPUT      = output file i/o channel			*/
/*	    OZ_ERROR       = error file i/o channel			*/
/*	    OZ_PARAMS      = parameter string				*/
/*	    OZ_DEFAULT_DIR = default directory string			*/
/*									*/
/************************************************************************/

static uLong spawn_thread (void *dummy)

{
  char imagename[256];
  uLong sts;
  uLong (*startaddr) (char *imagename);
  OZ_Handle h_image, h_logname, h_lognamtbl;
  void *baseaddr;

  h_logname   = 0;
  h_lognamtbl = 0;
  sts = oz_sys_logname_lookup (0, OZ_PROCMODE_USR, "OZ_PROCESS_TABLE", NULL, NULL, NULL, &h_lognamtbl);
  if (sts != OZ_SUCCESS) oz_sys_io_fs_printerror ("oz_sys_spawn: error %u looking up tablename OZ_PROCESS_TABLE\n", sts);
  else {
    sts = oz_sys_logname_lookup (h_lognamtbl, OZ_PROCMODE_USR, "OZ_IMAGE", NULL, NULL, NULL, &h_logname);
    oz_sys_handle_release (OZ_PROCMODE_KNL, h_lognamtbl);
    if (sts != OZ_SUCCESS) oz_sys_io_fs_printerror ("oz_sys_spawn: error %u looking up logname OZ_IMAGE\n", sts);
    else {
      sts = oz_sys_logname_getval (h_logname, 0, NULL, sizeof imagename, imagename, NULL, NULL, 0, NULL);
      oz_sys_handle_release (OZ_PROCMODE_KNL, h_logname);
      if (sts != OZ_SUCCESS) oz_sys_io_fs_printerror ("oz_sys_spawn: error %u getting imagename from OZ_IMAGE\n", sts);
      else {
        sts = oz_sys_image_load (OZ_PROCMODE_KNL, imagename, 0, &baseaddr, &startaddr, &h_image);
        if (sts != OZ_SUCCESS) oz_sys_io_fs_printerror ("oz_sys_spawn: error %u loading image %s\n", sts, imagename);
        else sts = (*startaddr) (imagename);
      }
    }
  }
  return (sts);
}
