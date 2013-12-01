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
/*  Misc replacement routines for loader				*/
/*									*/
/************************************************************************/

#include "ozone.h"

#include "oz_io_fs.h"

#include "oz_knl_ast.h"
#include "oz_knl_devio.h"
#include "oz_knl_handle.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_lock.h"
#include "oz_knl_logon.h"
#include "oz_knl_printk.h"
#include "oz_knl_quota.h"
#include "oz_knl_sdata.h"
#include "oz_knl_spte.h"
#include "oz_knl_status.h"
#include "oz_knl_thread.h"
#include "oz_knl_userjob.h"

#include "oz_sys_io_fs.h"
#include "oz_sys_misc.h"
#include "oz_sys_xprintf.h"

static OZ_Mempage tempspte_count = 0;
static OZ_Mempage tempspte_start = 0;

/************************************************************************/
/*									*/
/*  These are intended to be used by drivers to allocate sptes to map 	*/
/*  their memory-space registers.  They use the 'tempsptes'.		*/
/*									*/
/************************************************************************/

void oz_ldr_spte_init (OZ_Mempage ntempsptes, OZ_Mempage tempsptes)

{
  tempspte_count = ntempsptes;
  tempspte_start = tempsptes;
}

uLong oz_knl_spte_alloc (OZ_Mempage count, void **sysvaddr, OZ_Mempage *sysvpage, OZ_Section **section_r)

{
  if (count > tempspte_count) return (OZ_NOFREESPTES);
  if (sysvaddr  != NULL) *sysvaddr  = OZ_HW_VPAGETOVADDR (tempspte_start);
  if (sysvpage  != NULL) *sysvpage  = tempspte_start;
  if (section_r != NULL) *section_r = NULL;
  tempspte_count -= count;
  tempspte_start += count;
  return (OZ_SUCCESS);
}

void oz_knl_spte_free (OZ_Mempage count, OZ_Mempage svpage)

{ }

/************************************************************************/
/*									*/
/*  This is called by the logical name routines to get the address of 	*/
/*  the logical name tables						*/
/*									*/
/*  Always return the address of our fixed "OZ_SYSTEM_DIRECTORY"	*/
/*									*/
/************************************************************************/

OZ_Logname *oz_knl_user_getlognamdir (OZ_User *user)

{
  return (oz_s_systemdirectory);
}

OZ_Logname *oz_knl_user_getlognamtbl (OZ_User *user)

{
  return (oz_s_systemtable);
}

OZ_Logname *oz_knl_job_getlognamdir (OZ_Job *job)

{
  return (oz_s_systemdirectory);
}

OZ_Logname *oz_knl_job_getlognamtbl (OZ_Job *job)

{
  return (oz_s_systemtable);
}

uLong oz_knl_user_getmaxbasepri (OZ_User *user)

{
  return (100);
}

uLong oz_knl_ast_create (OZ_Thread *thread,
                         OZ_Procmode procmode,
                         OZ_Astentry astentry,
                         void *astparam,
                         int express, 
                         OZ_Ast **ast_r)

{
  oz_crash ("oz_knl_ast_create: not supported in the loader");
  return (OZ_NOTSUPINLDR);
}

void oz_knl_syscall_init (void)

{ }

Long oz_knl_job_increfc (OZ_Job *job, Long inc)

{
  oz_crash ("oz_knl_job_increfc: not supported in the loader");
  return (0);
}

OZ_User *oz_knl_job_getuser (OZ_Job *job)

{
  return (NULL);
}

Long oz_knl_user_increfc (OZ_User *user, Long inc)

{
  oz_crash ("oz_knl_user_increfc: not supported in the loader");
  return (0);
}

OZ_Quota *oz_knl_user_getquota (OZ_User *user)

{
  return (NULL);
}

OZ_Quota *oz_knl_quota_getcpudef (void)

{
  return (NULL);
}

OZ_Quota *oz_knl_quota_setcpudef (OZ_Quota *quota)

{
  return (NULL);
}

OZ_Quota *oz_knl_quota_fromobj (void *quotaobj)

{
  return (NULL);
}

int oz_knl_quota_debit (OZ_Quota *quota, OZ_Quotatype quotatype, Long amount)

{
  return (1);
}

void oz_knl_quota_credit (OZ_Quota *quota, OZ_Quotatype quotatype, Long amount, Long inc)

{ }

OZ_Devunit **oz_knl_user_getdevalloc (OZ_User *user)

{
  oz_crash ("oz_knl_user_getdevalloc: not available in loader");
}

OZ_Devunit **oz_knl_job_getdevalloc (OZ_Job *job)

{
  oz_crash ("oz_knl_job_getdevalloc: not available in loader");
}

/************************************************************************/
/*									*/
/*  Start logon process going on console device				*/
/*									*/
/************************************************************************/

void oz_knl_logon_devunit (OZ_Devunit *devunit)

{
  oz_knl_printk ("oz_knl_logon_devunit: not supported in loader\n");
}

void oz_knl_logon_iochan (OZ_Iochan *iochan)

{
  oz_knl_printk ("oz_knl_logon_iochan: not supported in loader\n");
}

/************************************************************************/
/*									*/
/*  Start diagnostic mode						*/
/*									*/
/************************************************************************/

void oz_knl_diag (Long cpuidx, int firstcpu, OZ_Mchargs *mchargs)

{
  oz_knl_printk ("oz_knl_diag: diagnostic mode not supported in loader\n");
}

/************************************************************************/
/*									*/
/*  Print message							*/
/*									*/
/************************************************************************/

void oz_sys_io_fs_printerror (const char *format, ...)

{
  va_list ap;

  va_start (ap, format);
  oz_knl_printkv (format, ap);
  va_end (ap);
}

uLong oz_sys_io_fs_printf (OZ_Handle h_output, const char *format, ...)

{
  oz_crash ("oz_sys_io_fs_printf: not supported in loader");
  return (OZ_NOTSUPINLDR);
}

/************************************************************************/
/*									*/
/*  Perform elaborate timezone conversion				*/
/*									*/
/************************************************************************/

uLong oz_sys_tzconv (OZ_Datebin in, OZ_Handle h_tzfilein, OZ_Datebin *out, int tznameoutl, char *tznameout)

{
  return (OZ_BADTZFILE);
}

/************************************************************************/
/*									*/
/*  Called by image loader to open the image file			*/
/*  The 'def_dir' in this case is a logical name that contains the 	*/
/*  device and directory.  The filename in the fs_open block is just 	*/
/*  the name of the file.						*/
/*									*/
/************************************************************************/

uLong oz_sys_io_fs_open2 (int fs_open_len, OZ_IO_fs_open *fs_open, int terminal, const char *def_dir, OZ_Handle *h_iochan_r)

{
  char *devname, *filename, *p;
  const char *fp;
  const OZ_Logvalue *logvalues;
  uLong sts;
  OZ_Iochan *iochan;

  /* Get device and directory from default directory logical name */

  sts = oz_knl_logname_lookup (oz_s_systemtable, OZ_PROCMODE_KNL, strlen (def_dir), def_dir, NULL, NULL, NULL, &logvalues, NULL, NULL);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_ldr_io_fs_open2: error %u translating def_dir lnm %s\n", sts, def_dir);
    return (sts);
  }

  /* Separate out device name from file name */

  fp = logvalues[0].buff;
  p  = strchr (fp, ':');
  if (p == NULL) {
    oz_knl_printk ("oz_ldr_io_fs_open2: missing : in file name %s\n", fp);
    return (OZ_BADPARAM);
  }

  /* Assign an I/O channel to the device */

  devname = OZ_KNL_NPPMALLOC (p - fp + 1);
  memcpy (devname, fp, p - fp);
  devname[p-fp] = 0;
  sts = oz_knl_iochan_crbynm (devname, OZ_LOCKMODE_CR, OZ_PROCMODE_KNL, NULL, &iochan);
  OZ_KNL_NPPFREE (devname);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_ldr_io_fs_open2: error %u assigning channel to %s\n", sts, fp);
    return (sts);
  }

  /* Open the file on the device */

  fp = fs_open -> name;
  filename = OZ_KNL_NPPMALLOC (strlen (++ p) + strlen (fp) + 1);
  strcpy (filename, p);
  strcat (filename, fp);
  fs_open -> name = filename;
  sts = oz_knl_io (iochan, OZ_IO_FS_OPEN, fs_open_len, fs_open);
  fs_open -> name = fp;
  OZ_KNL_NPPFREE (filename);

  /* Assign an handle to the I/O channel */

  if (sts == OZ_SUCCESS) {
    sts = oz_knl_handle_assign (iochan, OZ_PROCMODE_KNL, h_iochan_r);
    if (sts != OZ_SUCCESS) oz_crash ("oz_ldr_io_fs_open2: error %u assigning handle", sts);
  }
  oz_knl_iochan_increfc (iochan, -1);

  /* Return final status */

  return (sts);
}

uLong oz_sys_io (OZ_Procmode procmode, OZ_Handle h_iochan, OZ_Handle h_event, uLong funcode, uLong as, void *ap)

{
  oz_knl_printk ("oz_sys_io: not supported in loader\n");
  return (OZ_NOTSUPINLDR);
}

uLong oz_sys_io_fs_dumpmem (OZ_Handle h_output, uLong size, const void *buff)

{
  if (h_output != 0) {
    oz_knl_printk ("oz_sys_io_fs_dumpmem: non-zero handle not supported in loader\n");
    return (OZ_NOTSUPINLDR);
  }
  oz_knl_dumpmem (size, buff);
  return (OZ_SUCCESS);
}
