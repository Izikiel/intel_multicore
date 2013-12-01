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
/*  This routine is called as the last part of the boot process		*/
/*  Its job is to create the startup process that processes the users 	*/
/*  startup command script						*/
/*									*/
/************************************************************************/

#include "ozone.h"

#include "oz_io_fs.h"
#include "oz_knl_devio.h"
#include "oz_knl_hw.h"
#include "oz_knl_image.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_lock.h"
#include "oz_knl_logname.h"
#include "oz_knl_printk.h"
#include "oz_knl_sdata.h"
#include "oz_knl_status.h"
#include "oz_sys_io.h"
#include "oz_sys_spawn.h"
#include "oz_sys_thread.h"

static void cresyslogname (const char *name, const char *value);
static void openfile (char *defdir, char *file, OZ_Lockmode lockmode, OZ_Handle *handle_r);

void oz_knl_startup ()

{
  char *defdir;
  const char *loadfsdevname, *p;
  uLong sts;
  OZ_Handle h_error, h_input, h_output, h_thread;
  OZ_IO_fs_mountvol fs_mountvol;
  OZ_Iochan *loadiochan;

  /* Mount the boot device */

  oz_knl_printk ("oz_knl_startup: mounting %s via %s\n", oz_s_loadparams.load_device, oz_s_loadparams.load_fstemp);

  sts = oz_knl_iochan_crbynm (oz_s_loadparams.load_fstemp, OZ_LOCKMODE_PW, OZ_PROCMODE_KNL, NULL, &loadiochan);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_knl_startup: error %u assigning channel to %s\n", sts, oz_s_loadparams.load_fstemp);
    return;
  }
  memset (&fs_mountvol, 0, sizeof fs_mountvol);
  fs_mountvol.devname     = oz_s_loadparams.load_device;
  fs_mountvol.secattrsize = oz_knl_secattr_getsize (oz_s_secattr_tempdev);
  fs_mountvol.secattrbuff = oz_knl_secattr_getbuff (oz_s_secattr_tempdev);
  sts = oz_knl_io (loadiochan, OZ_IO_FS_MOUNTVOL, sizeof fs_mountvol, &fs_mountvol);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_knl_startup: error %u mounting volume\n", sts);
    oz_knl_iochan_increfc (loadiochan, -1);
    return;
  }
  loadfsdevname = oz_knl_devunit_devname (oz_knl_iochan_getdevunit (loadiochan));
  oz_knl_printk ("oz_knl_startup: volume mounted on device %s\n", loadfsdevname);
  oz_knl_iochan_increfc (loadiochan, -1);

  /* Create logical names - */
  /*   OZ_LOAD_FS = just the device name from the mount call          */
  /*   OZ_LOAD_DIR = the directory the system was booted from         */
  /*   OZ_IMAGE_DIR = same thing - set it up as a system-wide default */
  /*   OZ_DEFAULT_DIR = ditto                                         */

  defdir = OZ_KNL_NPPMALLOC (strlen (loadfsdevname) + strlen (oz_s_loadparams.load_dir) + 2);
  strcpy (defdir, loadfsdevname);
  strcat (defdir, ":");
  cresyslogname ("OZ_LOAD_FS",     defdir);
  strcat (defdir, oz_s_loadparams.load_dir);
  cresyslogname ("OZ_LOAD_DIR",    defdir);
  cresyslogname ("OZ_IMAGE_DIR",   defdir);
  cresyslogname ("OZ_DEFAULT_DIR", defdir);

  oz_knl_logname_dump (0, oz_s_systemdirectory);

  /* Load the kernel image symbol table - just leave the image handle hanging so it will never unload */

  oz_knl_printk ("oz_knl_startup: loading kernel image (%s) symbol table\n", oz_s_loadparams.kernel_image);
  sts = oz_knl_image_load (OZ_PROCMODE_KNL, oz_s_loadparams.kernel_image, 2, 0, NULL, NULL, &oz_s_kernelimage);
  if (sts != OZ_SUCCESS) oz_crash ("oz_knl_startup: error %u loading kernel image symbol table", sts);

  /* Create OZ_INPUT, _OUTPUT, _ERROR file handles as given by the load parameters */

  openfile (defdir, oz_s_loadparams.startup_input,  OZ_LOCKMODE_CR, &h_input);
  openfile (defdir, oz_s_loadparams.startup_output, OZ_LOCKMODE_CW, &h_output);
  openfile (defdir, oz_s_loadparams.startup_error,  OZ_LOCKMODE_CW, &h_error);

  /* Load and execute the startup command */

  oz_knl_printk ("oz_knl_startup: spawning startup process\n");
  p = oz_s_loadparams.startup_params;
  sts = oz_sys_spawn (0, oz_s_loadparams.startup_image, h_input, h_output, h_error, 0, 0, defdir, 1, &p, "startup", &h_thread, NULL);
  if (sts != OZ_SUCCESS) oz_knl_printk ("oz_knl_startup: error %u spawning startup process\n");
  else oz_knl_printk ("oz_knl_startup: startup process spawned\n");

  /* Orphan it so it doesn't get aborted when we exit */

  oz_sys_thread_orphan (h_thread);
  oz_sys_handle_release (OZ_PROCMODE_KNL, h_thread);

  /* Close OZ_INPUT, _OUTPUT, _ERROR file handles */

  oz_sys_handle_release (OZ_PROCMODE_KNL, h_input);
  oz_sys_handle_release (OZ_PROCMODE_KNL, h_output);
  oz_sys_handle_release (OZ_PROCMODE_KNL, h_error);

  /* All done with default directory string */

  OZ_KNL_NPPFREE (defdir);
}

/* Create logical name in system logical name table */

static void cresyslogname (const char *name, const char *value)

{
  uLong sts;
  OZ_Logvalue logvalue;

  logvalue.attr = OZ_LOGVALATR_TERMINAL;
  logvalue.buff = (void *)value;

  sts = oz_knl_logname_create (oz_s_systemtable, OZ_PROCMODE_KNL, NULL, oz_s_secattr_syslogname, 0, strlen (name), name, 1, &logvalue, NULL);
  if (sts != OZ_SUCCESS) oz_crash ("oz_knl_startup cresyslogname: error %u creating system logical name %s", name);
}

/* Open/Create INPUT/OUTPUT/ERROR file */

static void openfile (char *defdir, char *file, OZ_Lockmode lockmode, OZ_Handle *handle_r)

{
  char *devname, *filename, *p, *q;
  uLong sts;
  OZ_IO_fs_create fs_create;
  OZ_IO_fs_open fs_open;

  devname  = NULL;
  filename = NULL;

  /* If 'console:', just use the console I/O channel.  Assigning */
  /* an handle to it increments its ref count, then when the     */
  /* handle is released, the ref count gets decremented.         */

  if (strcmp (file, "console:") == 0) {
    sts = oz_knl_handle_assign (oz_s_coniochan, OZ_PROCMODE_KNL, handle_r);
    if (sts != OZ_SUCCESS) oz_crash ("oz_knl_startup openfile: error %u assigning handle to console", sts);
    return;
  }

  /* Assign I/O channel to device.  If no device in file, use device in defdir. */

  p = file;
  q = strchr (p, ':');
  if (q == NULL) { p = defdir; q = strchr (p, ':'); }
  devname = OZ_KNL_PGPMALLOC (q - p + 1);
  memcpy (devname, p, q - p);
  devname[q-p] = 0;
  sts = oz_sys_io_assign (OZ_PROCMODE_USR, handle_r, devname, lockmode);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_knl_startup openfile: error %u assigning channel to %s for %s (default %s)\n", sts, devname, file, defdir);
    goto usecon;
  }

  /* Concat the defdir+file strings, except for any device: that may be in either one */

  filename = OZ_KNL_PGPMALLOC (strlen (defdir) + strlen (file) + 1);	/* allocate buffer big enough to hold both */
  q = strchr (defdir, ':');						/* get colon in defdir string (there ALWAYS is one) */
  strcpy (filename, q + 1);						/* copy everything after it */
  q = strchr (file, ':');						/* look for colon in file string */
  if (q != NULL) strcat (filename, q + 1);				/* if there is one, copy everything after it */
  else strcat (filename, file);						/* otherwise, copy the whole file string */

  /* Open or create the file */

  if (lockmode == OZ_LOCKMODE_CR) {					/* read mode means open existing file */
    memset (&fs_open, 0, sizeof fs_open);				/* clear param block */
    fs_open.name     = filename;					/* fill in filename string pointer */
    fs_open.lockmode = OZ_LOCKMODE_CR;					/* fill in lock mode */
    sts = oz_sys_io (OZ_PROCMODE_KNL, *handle_r, 0, OZ_IO_FS_OPEN, sizeof fs_open, &fs_open);
  }
  if (lockmode == OZ_LOCKMODE_CW) {					/* write mode means create new file */
    memset (&fs_create, 0, sizeof fs_create);				/* clear param block */
    fs_create.name     = filename;					/* fill in filename string pointer */
    fs_create.lockmode = OZ_LOCKMODE_CW;				/* fill in lock mode */
    sts = oz_sys_io (OZ_PROCMODE_KNL, *handle_r, 0, OZ_IO_FS_CREATE, sizeof fs_create, &fs_create);
  }

  /* If successful, just return as is */

  if (sts == OZ_SUCCESS) goto rtn;
  oz_knl_printk ("oz_knl_startup openfile: error %u %sing %s on %s\n", sts, (lockmode == OZ_LOCKMODE_CW) ? "creat" : "open", filename, devname);
  oz_sys_handle_release (OZ_PROCMODE_KNL, *handle_r);

  /* Error, use the console (if that fails, not much we can do to fix it!) */

usecon:
  oz_knl_printk ("oz_knl_startup openfile: using console instead\n");
  sts = oz_sys_io_assign (OZ_PROCMODE_USR, handle_r, "console", lockmode);
  if (sts != OZ_SUCCESS) oz_crash ("oz_knl_startup openfile: error %u assigning channel to console", sts);
rtn:
  if (devname  != NULL) OZ_KNL_PGPFREE (devname);
  if (filename != NULL) OZ_KNL_PGPFREE (filename);
}
