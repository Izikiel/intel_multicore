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
/*  This routine gets called by the hardware layer at boot time		*/
/*									*/
/*  Its job is to read the loader parameter file, then load the 	*/
/*  kernel into memory.  When it returns, the hardware layer will 	*/
/*  enable virtual memory then call the kernel.				*/
/*									*/
/*  Although this routine calls stuff like oz_knl_iochan_create, 	*/
/*  oz_knl_iostart, oz_sys_image_load, they unlerlying routines are 	*/
/*  not the usual stuff.  At this point, the computer is not in 	*/
/*  virtual memory mode, and does not have a 'current thread', etc.  	*/
/*  So many of the underlying kernel modules have been replaced with 	*/
/*  stripped-down versions.						*/
/*									*/
/************************************************************************/

#include <stdarg.h>

#include "gzip.h"

#include "ozone.h"

#include "oz_io_console.h"
#include "oz_io_disk.h"
#include "oz_io_fs.h"

#include "oz_knl_cache.h"
#include "oz_knl_devio.h"
#include "oz_knl_event.h"
#include "oz_knl_handle.h"
#include "oz_knl_image.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_lock.h"
#include "oz_knl_logname.h"
#include "oz_knl_logon.h"
#include "oz_knl_lowipl.h"
#include "oz_knl_phymem.h"
#include "oz_knl_printk.h"
#include "oz_knl_process.h"
#include "oz_knl_quota.h"
#include "oz_knl_sdata.h"
#include "oz_knl_shuthand.h"
#include "oz_knl_spte.h"
#include "oz_knl_status.h"
#include "oz_knl_thread.h"
#include "oz_knl_userjob.h"

#include "oz_ldr_loader.h"

#include "oz_sys_dateconv.h"
#include "oz_sys_io_fs.h"
#include "oz_sys_misc.h"
#include "oz_sys_thread.h"
#include "oz_sys_xprintf.h"

/************************************************************************/
/*									*/
/*  Internal static data and routines					*/
/*									*/
/************************************************************************/

static int getcon_timeout = 5000;
static OZ_Iochan    *con_iochan       = NULL;
static OZ_Event     *ctrlcy_event     = NULL;
static OZ_Event     *event            = NULL;
static uLong         phymem_count     = 0;
static volatile int ctrlcy_hit = 0;
static char ctrlt_msg[256];

static char load_fs[OZ_DEVUNIT_NAMESIZE];

static int ctrlcy_enable (void);
static void ctrlcy_ast (void *dummy, uLong status);
static int ctrlt_enable (void);
static void ctrlt_ast (void *dummy, uLong status);
static OZ_Iochan *mount_boot_device (int readwrite);
static void dism_boot_device (OZ_Iochan *load_iochan);
static void crelogname (OZ_Logname *lognamtbl, uLong logvalatr, char *name, char *value);
static int read_param_file (OZ_Iochan *iochan);
static uLong copyfile (char *iname, char *oname);
static uLong deletefile (char *name);
static int dogzip (int gzipfc, char *iname, char *oname);
static char *quadmemsize (uQuad size, char *buff);

/************************************************************************/
/*									*/
/*    Input:								*/
/*									*/
/*	 loadparams_p = points to load param block			*/
/*	    sysbaseva = base virtual address of system global area	*/
/*	                copied into oz_s_sysmem_baseva			*/
/*	 startphypage = (obsolete)					*/
/*	     phypages = (obsolete)					*/
/*	  tempmemsize = size of temporary non-paged pool		*/
/*	  tempmemaddr = address of temporary non-paged pool		*/
/*	cacheareasize = some number of physical pages for temp cache	*/
/*	cacheareabase = base physical page number			*/
/*	   ntempsptes = number of temp sptes available			*/
/*	    tempsptes = vpage mapped by first temp spte			*/
/*									*/
/*    Output:								*/
/*									*/
/*	*loadparams_p = quite possibly modified				*/
/*	  *kstacksize = kernel stack size (in bytes)			*/
/*	 *systempages = number of system pages				*/
/*									*/
/************************************************************************/

void *oz_ldr_start (OZ_Loadparams *loadparams_p, 

                    void *sysbaseva, 
                    OZ_Mempage startphypage, 
                    OZ_Mempage phypages,
                    uLong tempmemsize, 
                    void *tempmemaddr, 

                    uLong *kstacksize, 
                    uLong *systempages, 

                    OZ_Mempage cacheareasize, 
                    OZ_Mempage cacheareabase, 

                    OZ_Mempage ntempsptes, 
                    OZ_Mempage tempsptes)

{
  char param_filespec[256];
  int i, j;
  uLong status, sts;
  OZ_Image *knlimage;
  OZ_IO_fs_open fs_open;
  OZ_Iochan *load_iochan;
  void *baseaddr, *startaddr;

  oz_s_inloader  = 1;
  oz_hw_putcon (24, "\noz_ldr_start: starting\n");	// print a message before inhibiting softints
  oz_hw_cpu_setsoftint (0);				// we're not pre-emptive anyway, but in case 
							// ... some routine tests, we make it official
  oz_knl_printk ("%s\n", oz_sys_copyright);		// print another message after softints inhibited

  oz_knl_sdata_init1 ();
  oz_s_cpucount  = 1;
  oz_s_cpusavail = 1;
  oz_s_phymem_l1pages = 1;
  oz_s_phymem_l2pages = 1;

  oz_knl_printk ("oz_ldr_start: param block at %p\n", loadparams_p);

  oz_ldr_paramblock = *loadparams_p;

  oz_knl_printk ("oz_ldr_start: boot device %s\n",      oz_ldr_paramblock.load_device);
  oz_knl_printk ("oz_ldr_start: boot fs template %s\n", oz_ldr_paramblock.load_fstemp);
  oz_knl_printk ("oz_ldr_start: boot directory %s\n",   oz_ldr_paramblock.load_dir);

  oz_knl_printk ("oz_ldr_start: system virtual base address %p\n", sysbaseva);
  oz_knl_printk ("oz_ldr_start: page size %u\n", 1 << OZ_HW_L2PAGESIZE);
  oz_knl_printk ("oz_ldr_start: there are %u bytes of temp memory at %p\n", tempmemsize, tempmemaddr);
  oz_knl_printk ("oz_ldr_start: there are %u physical pages for cache at 0x%X\n", cacheareasize, cacheareabase);
  oz_knl_printk ("oz_ldr_start: there are %u temp sptes at page 0x%X (vaddr %p)\n", ntempsptes, tempsptes, OZ_HW_VPAGETOVADDR (tempsptes));

  oz_s_sysmem_baseva = sysbaseva;

  OZ_KNL_NPPFREESIZ (tempmemsize, tempmemaddr);

  oz_knl_sdata_init2 ();

  phymem_count = cacheareasize;
  oz_ldr_phymem_init (cacheareasize, cacheareabase);
  oz_ldr_spte_init (ntempsptes, tempsptes);

  oz_knl_event_init ();
  oz_knl_handle_init ();
  oz_knl_event_create (13, "oz_ldr_loader", NULL, &event);
  oz_knl_event_create (8, "ctrl-C/Y", NULL, &ctrlcy_event);
  oz_knl_thread_cpuinit ();

  oz_knl_handletbl_create ();

  /* Initialize logical name tables for the drivers */

  sts = oz_knl_logname_create (NULL, OZ_PROCMODE_KNL, NULL, NULL, OZ_LOGNAMATR_TABLE, 19, "OZ_SYSTEM_DIRECTORY", 0, NULL, &oz_s_systemdirectory);
  if (sts != OZ_SUCCESS) oz_crash ("oz_ldr_loader: error %u creating loader logical name directory");

  sts = oz_knl_logname_create (oz_s_systemdirectory, OZ_PROCMODE_KNL, NULL, NULL, OZ_LOGNAMATR_TABLE, 15, "OZ_SYSTEM_TABLE", 0, NULL, &oz_s_systemtable);
  if (sts != OZ_SUCCESS) oz_crash ("oz_ldr_loader: error %u creating logical OZ_SYSTEM_TABLE in system directory", sts);

  crelogname (oz_s_systemtable, OZ_LOGVALATR_TERMINAL, "OZ_BOOT_DEV", oz_ldr_paramblock.load_device);
  crelogname (oz_s_systemtable, OZ_LOGVALATR_TERMINAL, "OZ_BOOT_DIR", oz_ldr_paramblock.load_dir);

  /* Initialize drivers that we will use */

  oz_knl_printk ("oz_ldr_start: initializing device drivers\n");
  oz_knl_devinit ();
  oz_knl_devdump (0);
  oz_knl_printk ("\n");

  /* Should be able to assign I/O channel to console - If failure, */
  /* set con_iochan to NULL so read routine will use oz_hw_getcon  */

  sts = oz_knl_iochan_crbynm ("console", OZ_LOCKMODE_CW, OZ_PROCMODE_KNL, NULL, &con_iochan);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_ldr_start: error %u assigning channel to console\n", sts);
    con_iochan = NULL;
  }

  /* Set up an control-C/control-Y handler and control-T handler */

  else {
    if (ctrlcy_enable ()) oz_knl_printk ("oz_ldr_start: press control-C or control-Y to abort command\n");
    ctrlt_msg[0] = 0;
    if (ctrlt_enable ()) oz_knl_printk ("oz_ldr_start: press control-T for thread status display\n");
  }

  /* Read parameter modifications from console */

  oz_knl_printk ("oz_ldr_start: type HELP for help, EXIT when done\n");
  if ((con_iochan != NULL) && (getcon_timeout != 0)) {
    oz_knl_printk ("oz_ldr_start: first prompt will timeout in %d seconds\n", getcon_timeout / 1000);
  }

read_param_from_con:
  while (!read_param_file (NULL)) {}	// if a command is bad, just prompt again

  /* If no boot device defined, scan for it */

  if (oz_ldr_paramblock.load_device[0] == 0) {
    ctrlcy_hit = 0;
    if (!oz_hw_bootscan (&ctrlcy_hit, ctrlcy_event, 0)) goto read_param_from_con;
  }

  /* Open the load_script file in the load directory and process it */

  if (oz_ldr_paramblock.load_script[0] != 0) {
    load_iochan = mount_boot_device (1);
    if (load_iochan == NULL) goto read_param_from_con;
    memset (&fs_open, 0, sizeof fs_open);
    strcpy (param_filespec, oz_ldr_paramblock.load_dir);
    strcat (param_filespec, oz_ldr_paramblock.load_script);
    oz_knl_printk ("oz_ldr_start: reading parameter file %s\n\n", param_filespec);
    fs_open.name = param_filespec;
    fs_open.lockmode = OZ_LOCKMODE_PR;
    sts = oz_knl_io (load_iochan, OZ_IO_FS_OPEN, sizeof fs_open, &fs_open);
    if (sts != OZ_SUCCESS) {
      oz_knl_printk ("oz_ldr_start: error %u opening loader param file %s on device %s\n", sts, param_filespec, oz_ldr_paramblock.load_device);
      dism_boot_device (load_iochan);
      goto read_param_from_con;
    }
    i = read_param_file (load_iochan);				// process commands from script file
    oz_knl_io (load_iochan, OZ_IO_FS_CLOSE, 0, NULL);		// close script file
    dism_boot_device (load_iochan);				// maybe script changed boot device, so dismount old one
    if (!i) goto read_param_from_con;				// if error, read commands from console
  }

  /* If no boot device defined, scan for it (maybe load script reset it) */

  if (oz_ldr_paramblock.load_device[0] == 0) {
    ctrlcy_hit = 0;
    if (!oz_hw_bootscan (&ctrlcy_hit, ctrlcy_event, 0)) goto read_param_from_con;
  }

  /* Mount boot device read-only */

  load_iochan = mount_boot_device (0);
  if (load_iochan == NULL) goto read_param_from_con;

  /* Create logical name for loader to get the kernel */

  strcpy (param_filespec, load_fs);
  strcat (param_filespec, ":");
  strcat (param_filespec, oz_ldr_paramblock.load_dir);

  crelogname (oz_s_systemtable, OZ_LOGVALATR_TERMINAL, "OZ_IMAGE_DIR", param_filespec);

  /* Load kernel image */

  oz_knl_printk ("oz_ldr_start: loading kernel image %s from %s\n", oz_ldr_paramblock.kernel_image, param_filespec);
  sts = oz_knl_image_load (OZ_PROCMODE_KNL, oz_ldr_paramblock.kernel_image, 1, 0, &baseaddr, &startaddr, &knlimage);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_ldr_start: error %u loading kernel image\n", sts);
    dism_boot_device (load_iochan);
    goto read_param_from_con;
  }
  oz_knl_printk ("oz_ldr_start: kernel image loaded, start address %p\n", startaddr);

  /* Dismount the load volume */

  dism_boot_device (load_iochan);

  /* Shut down everything nicely */

  oz_knl_shutdown ();

  /* Return values to hardware layer */

  *loadparams_p = oz_ldr_paramblock;
  *kstacksize   = oz_ldr_paramblock.kernel_stack_size;
  *systempages  = oz_ldr_paramblock.system_pages;

  return (startaddr);
}

/************************************************************************/
/*									*/
/*  Enable control-C/control-Y detection.  Set ctrlcy_hit when hit.	*/
/*									*/
/************************************************************************/

static int ctrlcy_enable (void)

{
  OZ_IO_console_ctrlchar ctrlcy;
  uLong sts;

  memset (&ctrlcy, 0, sizeof ctrlcy);
  ctrlcy.mask[0] = (1 << ('C' - '@')) | (1 << ('Y' - '@'));
  sts = oz_knl_iostart (con_iochan, OZ_PROCMODE_KNL, ctrlcy_ast, NULL, NULL, NULL, NULL, NULL, 
                        OZ_IO_CONSOLE_CTRLCHAR, sizeof ctrlcy, &ctrlcy);
  if (sts != OZ_STARTED) oz_knl_printk ("oz_ldr_start: error %u enabling ctrl-C/-Y detection\n", sts);
  return (sts == OZ_STARTED);
}

static void ctrlcy_ast (void *dummy, uLong status)

{
  if (status == OZ_SUCCESS) {
    ctrlcy_hit = 1;
    oz_knl_event_set (ctrlcy_event, 1);
  }
  else if (status != OZ_ABORTED) oz_knl_printk ("oz_ldr_start: error %u ctrl-C/-Y completion\n", status);
  ctrlcy_enable ();
}

/************************************************************************/
/*									*/
/*  Enable control-T detection.  Call thread_dump when hit.		*/
/*									*/
/************************************************************************/

static int ctrlt_enable (void)

{
  OZ_IO_console_ctrlchar ctrlt;
  uLong sts;

  memset (&ctrlt, 0, sizeof ctrlt);
  ctrlt.mask[0] = (1 << ('T' - '@'));
  sts = oz_knl_iostart (con_iochan, OZ_PROCMODE_KNL, ctrlt_ast, NULL, NULL, NULL, NULL, NULL, 
                        OZ_IO_CONSOLE_CTRLCHAR, sizeof ctrlt, &ctrlt);
  if (sts != OZ_STARTED) oz_knl_printk ("oz_ldr_start: error %u enabling ctrl-T detection\n", sts);
  return (sts == OZ_STARTED);
}

static void ctrlt_ast (void *dummy, uLong status)

{
  if (status == OZ_SUCCESS) {
    oz_ldr_thread_dump ();
    oz_knl_printk ("%s", ctrlt_msg);
  }
  else if (status != OZ_ABORTED) oz_knl_printk ("oz_ldr_start: error %u ctrl-T completion\n", status);
  ctrlt_enable ();
}

/************************************************************************/
/*									*/
/*  Mount the boot device						*/
/*									*/
/*    Input:								*/
/*									*/
/*	oz_ldr_paramblock.load_device = disk device to be mounted	*/
/*	oz_ldr_paramblock.load_fstemp = filesystem template device	*/
/*									*/
/*    Output:								*/
/*									*/
/*	mount_boot_device = NULL : error				*/
/*	                    else : filesystem I/O channel		*/
/*	load_fs = filesystem device name				*/
/*	logical OZ_BOOT_FS = filesystem device name			*/
/*									*/
/************************************************************************/

static OZ_Iochan *mount_boot_device (int readwrite)

{
  uLong sts;
  OZ_IO_fs_mountvol fs_mountvol;
  OZ_Iochan *load_iochan;

  /* If template device string is null, the load device does not */
  /* need mounting, it will accept OZ_IO_FS_OPEN calls as is     */

  if (oz_ldr_paramblock.load_fstemp[0] == 0) {
    strncpyz (load_fs, oz_ldr_paramblock.load_device, sizeof load_fs);
    oz_knl_printk ("oz_ldr_start: assigning channel to load device %s\n", load_fs);
    sts = oz_knl_iochan_crbynm (load_fs, readwrite ? OZ_LOCKMODE_CW : OZ_LOCKMODE_CR, OZ_PROCMODE_KNL, NULL, &load_iochan);
    if (sts != OZ_SUCCESS) {
      oz_knl_printk ("oz_ldr_start: error %u assigning channel to load device %s\n", sts, load_fs);
      return (NULL);
    }
  }

  /* Load_fstemp string supplied, assign I/O channel to template, then mount load device on it     */
  /* Mount it readonly, and don't bother with cache because we are just reading kernel into memory */

  else {
    oz_knl_printk ("oz_ldr_start: mounting boot device %s on fs %s\n", oz_ldr_paramblock.load_device, oz_ldr_paramblock.load_fstemp);
    sts = oz_knl_iochan_crbynm (oz_ldr_paramblock.load_fstemp, readwrite ? OZ_LOCKMODE_CW : OZ_LOCKMODE_CR, OZ_PROCMODE_KNL, NULL, &load_iochan);
    if (sts != OZ_SUCCESS) {
      oz_knl_printk ("oz_ldr_start: error %u assigning channel to load fs template %s\n", sts, oz_ldr_paramblock.load_fstemp);
      return (NULL);
    }
    memset (&fs_mountvol, 0, sizeof fs_mountvol);
    fs_mountvol.devname    = oz_ldr_paramblock.load_device;
    fs_mountvol.mountflags = OZ_FS_MOUNTFLAG_READONLY | OZ_FS_MOUNTFLAG_NOCACHE;
    if (readwrite) fs_mountvol.mountflags = 0;
    sts = oz_knl_io (load_iochan, OZ_IO_FS_MOUNTVOL, sizeof fs_mountvol, &fs_mountvol);
    if (sts != OZ_SUCCESS) {
      oz_knl_printk ("oz_ldr_start: error %u mounting load device %s on fs %s\n", sts, oz_ldr_paramblock.load_device, oz_ldr_paramblock.load_fstemp);
      oz_knl_iochan_increfc (load_iochan, -1);
      return (NULL);
    }
    strncpyz (load_fs, oz_knl_devunit_devname (oz_knl_iochan_getdevunit (load_iochan)), sizeof load_fs);
    oz_knl_printk ("oz_ldr_start: load device %s mounted as %s\n", oz_ldr_paramblock.load_device, load_fs);
  }

  /* Either way, create logical name OZ_BOOT_FS to point to resultant filesystem device */

  crelogname (oz_s_systemtable, OZ_LOGVALATR_TERMINAL, "OZ_BOOT_FS", load_fs);
  oz_knl_printk ("oz_ldr_start: logical OZ_BOOT_FS assigned to %s\n", load_fs);
  return (load_iochan);
}

static void dism_boot_device (OZ_Iochan *load_iochan)

{
  uLong sts;

  /* Dismount if there is a template device */

  if (oz_ldr_paramblock.load_fstemp[0] != 0) {
    sts = oz_knl_io (load_iochan, OZ_IO_FS_DISMOUNT, 0, NULL);
    if (sts != OZ_SUCCESS) oz_knl_printk ("oz_ldr_start: error %u dismounting load volume\n", sts);
  }

  /* Anyway, deassign the i/o channel */

  oz_knl_iochan_increfc (load_iochan, -1);
}

/************************************************************************/
/*									*/
/*  Create logical name							*/
/*									*/
/************************************************************************/

static void crelogname (OZ_Logname *lognamtbl, uLong logvalatr, char *name, char *value)

{
  uLong sts;
  OZ_Logvalue lnmvalue;

  lnmvalue.attr = logvalatr;
  lnmvalue.buff = value;

  sts = oz_knl_logname_create (lognamtbl, OZ_PROCMODE_KNL, NULL, NULL, 0, strlen (name), name, 1, &lnmvalue, NULL);
  if ((sts != OZ_SUCCESS) && (sts != OZ_SUPERSEDED)) {
    oz_crash ("oz_ldr_loader crelogname: error %u creating logical name %s", sts, name);
  }
}

/************************************************************************/
/*									*/
/*  Read parameters and store in oz_ldr_paramblock			*/
/*									*/
/*    Input:								*/
/*									*/
/*	iochan = NULL : read from console terminal			*/
/*	         else : i/o channel of parameter file			*/
/*									*/
/*    Output:								*/
/*									*/
/*	results stored in oz_ldr_paramblock				*/
/*									*/
/************************************************************************/

#define COPY_SIZE 4096

static int read_param_file (OZ_Iochan *iochan)

{
  char *argv[128], c, filenambuff[64], linebuf[256], *p, rnamebuff[OZ_FS_MAXFNLEN], *secattrstr, tempbuf[256];
  int argc, i, j, s, usedup;
  OZ_Datebin now;
  OZ_IO_console_read console_read;
  OZ_IO_fs_create fs_create;
  OZ_IO_fs_getinfo1 fs_getinfo1;
  OZ_IO_fs_getsecattr fs_getsecattr;
  OZ_IO_fs_initvol fs_initvol;
  OZ_IO_fs_mountvol fs_mountvol;
  OZ_IO_fs_open fs_open;
  OZ_IO_fs_readblocks fs_readblocks;
  OZ_IO_fs_readdir fs_readdir;
  OZ_IO_fs_readrec fs_readrec;
  OZ_IO_fs_remove fs_remove;
  OZ_IO_fs_writeblocks fs_writeblocks;
  OZ_IO_fs_writeboot fs_writeboot;
  OZ_IO_fs_writerec fs_writerec;
  OZ_Iochan *fileiochan, *tempiochan;
  uLong count, linelen, sts, svbn, ul;

  ctrlcy_hit = 0;

  while (1) {
    if (ctrlcy_hit) return (0);

    /* Read a record either from console or from loader parameter file */

    if (iochan == NULL) {

      /* If available use the console driver (so we will process */
      /* timeout and so interrupts will be enabled during read)  */

      if (con_iochan != NULL) {
        memset (&console_read, 0, sizeof console_read);
        console_read.size    = sizeof linebuf - 1;
        console_read.buff    = linebuf;
        console_read.pmtsize = 16;
        console_read.pmtbuff = "\noz_ldr_start> ";
        console_read.rlen    = &linelen;
        console_read.timeout = getcon_timeout;
        sts = oz_knl_io (con_iochan, OZ_IO_CONSOLE_READ, sizeof console_read, &console_read);
        if (sts == OZ_SUCCESS) linebuf[linelen] = 0;
        else if ((sts == OZ_TIMEDOUT) || (sts == OZ_ENDOFFILE)) {
          oz_knl_printk ("*EXIT*\n");
          strcpy (linebuf, "EXIT");
        } else {
          oz_knl_printk ("oz_ldr_start: error %u reading from console\n", sts);
          oz_knl_iochan_increfc (con_iochan, -1);
          con_iochan = NULL;
          getcon_timeout = 0;
          return (0);
        }
      }

      /* Otherwise, use hardware routine to read from console (which  */
      /* typically inhibits interrupts and has no timeout processing) */

      if (con_iochan == NULL) {
        if (!oz_hw_getcon (sizeof linebuf, linebuf, 16, "\noz_ldr_start> ")) {
          oz_knl_printk ("*EXIT*\n");
          strcpy (linebuf, "EXIT");
        }
      }

      /* In any case, inhibit timeouts next time around - they only apply to the first prompt */

      getcon_timeout = 0;
    } else {

      /* Reading from file, no timeout business */

      memset (&fs_readrec, 0, sizeof fs_readrec);
      fs_readrec.size = sizeof linebuf - 1;
      fs_readrec.buff = linebuf;
      fs_readrec.trmsize = 1;
      fs_readrec.trmbuff = "\n";
      fs_readrec.rlen = &linelen;
      sts = oz_knl_io (iochan, OZ_IO_FS_READREC, sizeof fs_readrec, &fs_readrec);
      if (sts != OZ_SUCCESS) {
        if (sts == OZ_ENDOFFILE) return (1);
        oz_knl_printk ("error %u reading parameter file\n", sts);
        return (0);
      }
      linebuf[linelen] = 0;
      oz_knl_printk ("> %s\n", linebuf);
    }

    /* Parse it up into an argc/argv array */

    argc = 0;
    s = 1;
    for (i = 0; (c = linebuf[i]) != 0; i ++) {
      if ((c == '!') || (c == '#')) break;
      if (c > ' ') {
        if (c == '\\') {
          for (j = i; linebuf[j] != 0; j ++) linebuf[j] = linebuf[j+1];
          if (linebuf[i] == 0) break;
        }
        if (s) {
          argv[argc++] = linebuf + i;
          s = 0;
        }
      } else {
        s = 1;
        linebuf[i] = 0;
      }
    }
    argv[argc] = NULL;

    /* Ignore blank lines */

    if (argc == 0) continue;

    /* Process BARF command */

    if (strcasecmp (argv[0], "BARF") == 0) {
      oz_crash ("BARFing");
    }

    /* Process BOOTSCAN command */

    if (strcasecmp (argv[0], "BOOTSCAN") == 0) {
      if ((argc != 1) && ((argc != 2) || (strcasecmp (argv[1], "-verbose") != 0))) {
        oz_knl_printk ("BOOTSCAN can have 1 option, -verbose\n");
        return (0);
      }
      oz_knl_printk ("Scanning for boot device...\n");
      ctrlcy_hit = 0;
      argc = oz_hw_bootscan (&ctrlcy_hit, ctrlcy_event, (argc == 2));
      oz_knl_printk ("Scanning %s\n", argc ? "successful" : "failed");
      if (argc) continue;
      return (0);
    }

    /* Process COPY command */

    if (strcasecmp (argv[0], "COPY") == 0) {
      if (argc != 3) {
        oz_knl_printk ("COPY command requires 2 arguments: COPY <from> <to>\n");
        oz_knl_printk ("  for example: copy floppy.p0.oz_dfs:/ ide_pm.3.oz_dfs:/\n");
        return (0);
      }
      sts = copyfile (argv[1], argv[2]);
      if (sts != OZ_SUCCESS) return (0);
      continue;
    }

    /* Process DELETE command */

    if (strcasecmp (argv[0], "DELETE") == 0) {
      if (argc != 2) {
        oz_knl_printk ("DELETE command requires 1 argument: DELETE <file_or_directory>\n");
        oz_knl_printk ("  for example: delete floppy.p0.oz_dfs:/ozone/startup/commonstartup.cli\n");
        return (0);
      }
      sts = deletefile (argv[1]);
      if (sts != OZ_SUCCESS) return (0);
      continue;
    }

    /* Process DEVICES command */

    if (strcasecmp (argv[0], "DEVICES") == 0) {
      if ((argc == 1) || ((argc == 2) && (strcasecmp (argv[1], "-FULL") == 0))) oz_knl_devdump (argc == 2);
      else {
        oz_knl_printk ("DEVICES [-full]\n");
        return (0);
      }
      continue;
    }

    /* Process DIR command */

    if (strcasecmp (argv[0], "DIR") == 0) {
      if (argc != 2) {
        oz_knl_printk ("DIR command requires 1 argument: DIR <directory>\n");
        oz_knl_printk ("  for example: dir floppy.p0.oz_dfs:/\n");
        return (0);
      }
      p = strchr (argv[1], ':');
      if (p == NULL) {
        oz_knl_printk ("missing : in %s\n", argv[1]);
        return (0);
      }
      *p = 0;
      sts = oz_knl_iochan_crbynm (argv[1], OZ_LOCKMODE_CW, OZ_PROCMODE_KNL, NULL, &tempiochan);
      if (sts != OZ_SUCCESS) {
        oz_knl_printk ("error %u assigning channel to filesystem device %s\n", sts, argv[1]);
        return (0);
      }
      sts = oz_knl_iochan_crbynm (argv[1], OZ_LOCKMODE_CW, OZ_PROCMODE_KNL, NULL, &fileiochan);
      if (sts != OZ_SUCCESS) {
        oz_knl_printk ("error %u assigning second channel to filesystem device %s\n", sts, argv[1]);
        oz_knl_iochan_increfc (tempiochan, -1);
        return (0);
      }
      *(p ++) = ':';
      memset (&fs_open, 0, sizeof fs_open);
      fs_open.name      = p;
      fs_open.lockmode  = OZ_LOCKMODE_CR;
      fs_open.rnamesize = sizeof rnamebuff;
      fs_open.rnamebuff = rnamebuff;
      rnamebuff[0] = 0;
      sts = oz_knl_io (tempiochan, OZ_IO_FS_OPEN, sizeof fs_open, &fs_open);
      if (sts != OZ_SUCCESS) {
        oz_knl_printk ("error %u opening directory %s\n", sts, argv[1]);
        oz_knl_iochan_increfc (tempiochan, -1);
        oz_knl_iochan_increfc (fileiochan, -1);
        return (0);
      }
      if (rnamebuff[0] == 0) strncpyz (rnamebuff, argv[1], sizeof rnamebuff);
      oz_knl_printk ("listing directory %s\n", rnamebuff);
      strcpy (filenambuff, p);
      i = strlen (filenambuff);
      memset (&fs_readdir, 0, sizeof fs_readdir);
      fs_readdir.filenamsize = sizeof filenambuff - i;
      fs_readdir.filenambuff = filenambuff + i;
      while ((sts = oz_knl_io (tempiochan, OZ_IO_FS_READDIR, sizeof fs_readdir, &fs_readdir)) == OZ_SUCCESS) {
        memset (&fs_open, 0, sizeof fs_open);
        fs_open.name     = filenambuff;
        fs_open.lockmode = OZ_LOCKMODE_NL;
        sts = oz_knl_io (fileiochan, OZ_IO_FS_OPEN, sizeof fs_open, &fs_open);
        if (sts != OZ_SUCCESS) oz_knl_printk ("  error %u opening file %s\n", sts, filenambuff);
        else {
          memset (&fs_getinfo1, 0, sizeof fs_getinfo1);
          sts = oz_knl_io (fileiochan, OZ_IO_FS_GETINFO1, sizeof fs_getinfo1, &fs_getinfo1);
          if (sts != OZ_SUCCESS) oz_knl_printk ("  error %u getting attributes for %s\n", sts, filenambuff);
          else oz_knl_printk ("  %6u.%-3u/%-6u  %19.19t  %s\n", fs_getinfo1.eofblock, fs_getinfo1.eofbyte, fs_getinfo1.hiblock, fs_getinfo1.modify_date, filenambuff + i);
          oz_knl_io (fileiochan, OZ_IO_FS_CLOSE, 0, NULL);
        }
        sts = OZ_ABORTEDBYCLI;
        if (ctrlcy_hit) break;
      }
      oz_knl_iochan_increfc (tempiochan, -1);
      oz_knl_iochan_increfc (fileiochan, -1);
      if (sts != OZ_ENDOFFILE) {
        oz_knl_printk ("error %u reading directory\n", sts);
        return (0);
      }
      oz_knl_printk ("end of directory\n");
      continue;
    }

    /* Process DISMOUNT command */

    if ((strcasecmp (argv[0], "DISM") == 0) || (strcasecmp (argv[0], "DISMOUNT") == 0)) {
      if (argc != 2) {
        oz_knl_printk ("DISMOUNT command requires 1 argument: DISMOUNT <fsdevice>\n");
        oz_knl_printk ("  for example: dismount floppy.p0.oz_dfs\n");
        return (0);
      }
      sts = oz_knl_iochan_crbynm (argv[1], OZ_LOCKMODE_CR, OZ_PROCMODE_KNL, NULL, &tempiochan);
      if (sts != OZ_SUCCESS) {
        oz_knl_printk ("error %u assigning channel to filesystem device %s\n", sts, argv[1]);
        return (0);
      }
      sts = oz_knl_io (tempiochan, OZ_IO_FS_DISMOUNT, 0, NULL);
      oz_knl_iochan_increfc (tempiochan, -1);
      if (sts != OZ_SUCCESS) {
        oz_knl_printk ("error %u dismounting fsdevice %s\n", sts, argv[1]);
        return (0);
      }
      oz_knl_printk ("fsdevice %s dismounted\n", argv[1]);
      continue;
    }

    /* Process DUMP command */

    if (strcasecmp (argv[0], "DUMP") == 0) {
      if (argc != 4) {
        oz_knl_printk ("DUMP command requires 3 arguments: DUMP <file> <count> <startvbn>\n");
        oz_knl_printk ("  for example: dump floppy.p0.oz_dfs:/oz_loader_486.bb 4 1\n");
        return (0);
      }

      /* Open input file */

      p = strchr (argv[1], ':');
      if (p == NULL) {
        oz_knl_printk ("missing : in %s\n", argv[1]);
        return (0);
      }
      *p = 0;
      sts = oz_knl_iochan_crbynm (argv[1], OZ_LOCKMODE_CR, OZ_PROCMODE_KNL, NULL, &tempiochan);
      if (sts != OZ_SUCCESS) {
        oz_knl_printk ("error %u assigning channel to %s\n", sts, argv[1]);
        return (0);
      }
      *(p ++) = ':';
      memset (&fs_open, 0, sizeof fs_open);
      fs_open.name     = p;
      fs_open.lockmode = OZ_LOCKMODE_CR;
      sts = oz_knl_io (tempiochan, OZ_IO_FS_OPEN, sizeof fs_open, &fs_open);
      if (sts != OZ_SUCCESS) {
        oz_knl_printk ("error %u opening file %s\n", sts, argv[1]);
        oz_knl_iochan_increfc (tempiochan, -1);
        return (0);
      }
      memset (&fs_getinfo1, 0, sizeof fs_getinfo1);
      sts = oz_knl_io (tempiochan, OZ_IO_FS_GETINFO1, sizeof fs_getinfo1, &fs_getinfo1);
      if ((sts != OZ_SUCCESS) && (sts != OZ_BADIOFUNC)) {
        oz_knl_printk ("error %u getting file %s size\n", sts, argv[1]);
        oz_knl_iochan_increfc (tempiochan, -1);
        return (0);
      }

      /* Get number of blocks and starting vbn */

      count = oz_hw_atoi (argv[2], NULL);
      svbn  = oz_hw_atoi (argv[3], NULL);

      p = OZ_KNL_NPPMALLOC (fs_getinfo1.blocksize);

      while (count > 0) {
        -- count;
        memset (&fs_readblocks, 0, sizeof fs_readblocks);
        fs_readblocks.size = fs_getinfo1.blocksize;
        fs_readblocks.buff = p;
        fs_readblocks.svbn = svbn ++;
        sts = oz_knl_io (tempiochan, OZ_IO_FS_READBLOCKS, sizeof fs_readblocks, &fs_readblocks);
        if (sts != OZ_SUCCESS) break;
        oz_knl_printk ("\nblock %u:\n", fs_readblocks.svbn);
        oz_knl_dumpmem (fs_readblocks.size, p);
        if (iochan == NULL) oz_knl_printkp ("block %u", fs_readblocks.svbn);
        if (ctrlcy_hit) break;
      }

      OZ_KNL_NPPFREE (p);
      oz_knl_iochan_increfc (tempiochan, -1);
      continue;
    }

    /* Process EXIT command */

    if (strcasecmp (argv[0], "EXIT") == 0) return (1);

    /* Process EXTRA command */

    if (strcasecmp (argv[0], "EXTRA") == 0) {
      if (argc < 2) {
        oz_knl_printk ("EXTRA command requires at least 1 argument: EXTRA <name> [<value>]\n");
        return (0);
      }

      if (!oz_ldr_extra (oz_knl_printk, argv[1], (argc > 2) ? argv[2] : NULL)) return (0);
      continue;
    }

    /* Process EXTRAS command */

    if (strcasecmp (argv[0], "EXTRAS") == 0) {
      if (argc != 1) {
        oz_knl_printk ("EXTRAS command requires no arguments\n");
        return (0);
      }
      oz_ldr_extras (oz_knl_printk, &ctrlcy_hit);
      continue;
    }

    /* Process GUNZIP/GZIP commands */

    i = GZIP_FUNC_DUMMY;
    if (strcasecmp (argv[0], "GUNZIP") == 0) i = GZIP_FUNC_EXPAND;
    if (strcasecmp (argv[0], "GZIP") == 0) i = GZIP_FUNC_COMPRESS;
    if (i != GZIP_FUNC_DUMMY) {
      if (argc != 3) {
        oz_knl_printk ("GUNZIP/GZIP commands require two arguments, GUNZIP/GZIP <input> <output>\n");
        return (0);
      }
      if (!dogzip (i, argv[1], argv[2])) return (0);
      continue;
    }

    /* Process HELP command */

    if (strcasecmp (argv[0], "HELP") == 0) {
      oz_knl_printk ("\nCommands are:\n"
                     "  BARF                             - force access violation (ie, crash)\n"
                     "  BOOTSCAN                         - scan for boot device\n"
                     "  COPY <from> <to>                 - copy file or directory\n"
                     "  DELETE <file>                    - delete file or directory\n"
                     "  DEVICES [-FULL]                  - list out devices\n"
                     "  DIR <directory>                  - list out directory\n"
                     "  DISMOUNT <fsdevice>              - dismount filesystem device\n"
                     "  DUMP <file> <count> <startvbn>   - dump a file\n"
                     "  EXIT                             - continue boot process\n"
                     "  EXTRA <name> <value>             - set extra name's value\n"
                     "  EXTRAS                           - list out all extras\n"
                     "  GUNZIP <input> <output>          - GNU-unzip a file\n"
                     "  GZIP <input> <output>            - GNU-zip a file\n"
                     "  HELP                             - print help message\n"
                     "  INIT <disk> <template> <volname> - initialize <disk> \n"
                     "                                          using <template> \n"
                     "                                             to <volname>\n"
                     "  LOGICALS                         - list out all logical names\n"
                     "  MKDIR <directory>                - create a directory\n"
                     "  MOUNT [-NOCACHE] [-READONLY] [-VERIFY] [-WRITETHRU] <disk> <template> \n"
                     "                                   - mount <disk> device \n"
                     "                                        on <template> filesystem\n"
                     "  RENAME <oldname> <newname>       - rename a file\n"
                     "  SET <parameter> <value>          - set a parameter to the given value\n"
                     "  SHOW                             - display all the parameters\n"
                     "  THREADS                          - display threads and their status\n"
                     "  TIME                             - display current date/time\n"
                     "  TYPE <file>                      - display a file's contents\n"
                     "  VOLUME <fsdev>                   - display volume parameters\n"
                     "  WRITEBOOT <loaderimage>          - write bootblock for given loader image\n"
                    );
      continue;
    }

    /* Process INIT command */

    if (strcasecmp (argv[0], "INIT") == 0) {
      memset (&fs_initvol, 0, sizeof fs_initvol);
init_countargs:
      if (argc < 4) goto init_usage;
      if (strcasecmp (argv[1], "-writethru") == 0) {
        fs_initvol.initflags |= OZ_FS_INITFLAG_WRITETHRU;
        -- argc;
        memmove (argv + 1, argv + 2, (argc - 1) * sizeof argv[0]);
        goto init_countargs;
      }
      if (strcasecmp (argv[1], "-clusterfactor") == 0) {
        fs_initvol.clusterfactor = oz_hw_atoi (argv[2], &i);
        if (argv[2][i] != 0) {
          oz_knl_printk ("bad clusterfactor %s\n", argv[2]);
          return (0);
        }
        argc -= 2;
        memmove (argv + 1, argv + 3, (argc - 1) * sizeof argv[0]);
        goto init_countargs;
      }
      sts = oz_knl_iochan_crbynm (argv[2], OZ_LOCKMODE_CW, OZ_PROCMODE_KNL, NULL, &tempiochan);
      if (sts != OZ_SUCCESS) {
        oz_knl_printk ("error %u assigning channel to template filesystem device %s\n", sts, argv[2]);
        return (0);
      }
      fs_initvol.devname = argv[1];
      fs_initvol.volname = argv[2];
      sts = oz_knl_io (tempiochan, OZ_IO_FS_INITVOL, sizeof fs_initvol, &fs_initvol);
      if (sts != OZ_SUCCESS) {
        oz_knl_printk ("error %u initializing disk %s via filesystem %s\n", sts, argv[1], argv[2]);
        oz_knl_iochan_increfc (tempiochan, -1);
        return (0);
      }
      oz_knl_printk ("disk %s initialized\n", argv[1]);
      oz_knl_iochan_increfc (tempiochan, -1);
      continue;

init_usage:
      oz_knl_printk ("INIT command requires 3 arguments: INIT [-clusterfactor <clusterfactor>] [-writethru] <disk> <template> <volname>\n");
      oz_knl_printk ("  for example: init floppy.p0 oz_dfs my_floppy\n");
      return (0);
    }

    /* Process LOGICALS command */

    if (strcasecmp (argv[0], "LOGICALS") == 0) {
      oz_knl_logname_dump (0, oz_knl_process_getlognamtbl (oz_s_systemproc));
      continue;
    }

    /* Process MKDIR command */

    if (strcasecmp (argv[0], "MKDIR") == 0) {
      if (argc != 2) {
        oz_knl_printk ("MKDIR command requires 1 argument: MKDIR <directory>\n");
        oz_knl_printk ("  for example: mkdir floppy.p0.oz_dfs:/startup/\n");
        return (0);
      }

      p = strchr (argv[1], ':');
      if (p == NULL) {
        oz_knl_printk ("missing : in %s\n", argv[1]);
        return (0);
      }
      *p = 0;
      sts = oz_knl_iochan_crbynm (argv[1], OZ_LOCKMODE_CW, OZ_PROCMODE_KNL, NULL, &fileiochan);
      if (sts != OZ_SUCCESS) {
        oz_knl_printk ("error %u assigning channel to %s\n", sts, argv[1]);
        return (0);
      }
      *(p ++) = ':';
      memset (&fs_create, 0, sizeof fs_create);
      fs_create.name         = p;
      fs_create.lockmode     = OZ_LOCKMODE_PW;
      fs_create.filattrflags = OZ_FS_FILATTRFLAG_DIRECTORY;
      fs_create.rnamesize    = sizeof rnamebuff;
      fs_create.rnamebuff    = rnamebuff;
      rnamebuff[0] = 0;
      sts = oz_knl_io (fileiochan, OZ_IO_FS_CREATE, sizeof fs_create, &fs_create);
      if (sts != OZ_SUCCESS) {
        oz_knl_printk ("error %u creating directory %s\n", sts, argv[1]);
        if (sts != OZ_FILEALREADYEXISTS) return (0);
      } else {
        if (rnamebuff[0] == 0) strncpyz (rnamebuff, argv[1], sizeof rnamebuff);
        oz_knl_printk ("directory %s created\n", rnamebuff);
      }
      oz_knl_iochan_increfc (fileiochan, -1);
      continue;
    }

    /* Process MOUNT command */

    if (strcasecmp (argv[0], "MOUNT") == 0) {
      memset (&fs_mountvol, 0, sizeof fs_mountvol);
      if (phymem_count == 0) fs_mountvol.mountflags = OZ_FS_MOUNTFLAG_NOCACHE; /* force -nocache if there is no physical memory to use for it */
mount_countargs:
      if (argc < 3) goto mount_usage;
      if (strcasecmp (argv[1], "-nocache") == 0) {		/* check for -nocache option */
        fs_mountvol.mountflags |= OZ_FS_MOUNTFLAG_NOCACHE;	/* ok, remember it */
        -- argc;						/* shift arg list over */
        memmove (argv + 1, argv + 2, (argc - 1) * sizeof argv[0]);
        goto mount_countargs;					/* make sure we still have enough args */
      }
      if (strcasecmp (argv[1], "-readonly") == 0) {
        fs_mountvol.mountflags |= OZ_FS_MOUNTFLAG_READONLY;
        -- argc;
        memmove (argv + 1, argv + 2, (argc - 1) * sizeof argv[0]);
        goto mount_countargs;
      }
      if (strcasecmp (argv[1], "-verify") == 0) {
        fs_mountvol.mountflags |= OZ_FS_MOUNTFLAG_VERIFY;
        -- argc;
        memmove (argv + 1, argv + 2, (argc - 1) * sizeof argv[0]);
        goto mount_countargs;
      }
      if (strcasecmp (argv[1], "-writethru") == 0) {
        fs_mountvol.mountflags |= OZ_FS_MOUNTFLAG_WRITETHRU;
        -- argc;
        memmove (argv + 1, argv + 2, (argc - 1) * sizeof argv[0]);
        goto mount_countargs;
      }
      sts = oz_knl_iochan_crbynm (argv[2], OZ_LOCKMODE_CW, OZ_PROCMODE_KNL, NULL, &tempiochan);
      if (sts != OZ_SUCCESS) {
        oz_knl_printk ("error %u assigning channel to template filesystem device %s\n", sts, argv[2]);
        return (0);
      }
      fs_mountvol.devname = argv[1];
      sts = oz_knl_io (tempiochan, OZ_IO_FS_MOUNTVOL, sizeof fs_mountvol, &fs_mountvol);
      if (sts != OZ_SUCCESS) {
        oz_knl_printk ("error %u mounting disk %s on filesystem %s\n", sts, argv[1], argv[2]);
        oz_knl_iochan_increfc (tempiochan, -1);
        return (0);
      }
      oz_knl_printk ("disk %s mounted as filesystem device %s\n", argv[1], oz_knl_devunit_devname (oz_knl_iochan_getdevunit (tempiochan)));
      oz_knl_iochan_increfc (tempiochan, -1);
      continue;

mount_usage:
      oz_knl_printk ("MOUNT command requires 2 arguments: MOUNT [-NOCACHE] [-READONLY] [-VERIFY] [-WRITETHRU] <disk> <template>\n");
      oz_knl_printk ("  for example: mount -readonly floppy.p0 oz_dfs\n");
      return (0);
    }

    /* Process RENAME command */

    if (strcasecmp (argv[0], "RENAME") == 0) {
      OZ_IO_fs_rename fs_rename;

      if (argc != 3) {
        oz_knl_printk ("RENAME command requires 2 arguments: RENAME <oldname> <newname>\n");
        oz_knl_printk ("  for example: rename floppy.p0.oz_dfs:/x.tmp /trash/y.tmp\n");
        return (0);
      }

      memset (&fs_rename, 0, sizeof fs_rename);

      p = strchr (argv[1], ':');
      if (p == NULL) {
        oz_knl_printk ("missing : in %s\n", argv[1]);
        return (0);
      }
      *(p ++) = 0;
      fs_rename.oldname = p;

      p = strchr (argv[2], ':');
      if (p == NULL) p = argv[2];
      else {
        *(p ++) = 0;
        if (strcmp (argv[1], argv[2]) != 0) {
          oz_knl_printk ("device names must be the same (or omitted from newname)\n");
          return (0);
        }
      }
      fs_rename.newname = p;

      sts = oz_knl_iochan_crbynm (argv[1], OZ_LOCKMODE_CW, OZ_PROCMODE_KNL, NULL, &fileiochan);
      if (sts != OZ_SUCCESS) {
        oz_knl_printk ("error %u assigning channel to %s\n", sts, argv[1]);
        return (0);
      }

      sts = oz_knl_io (fileiochan, OZ_IO_FS_RENAME, sizeof fs_rename, &fs_rename);
      if (sts != OZ_SUCCESS) {
        oz_knl_printk ("error %u renaming %s:%s to %s\n", sts, argv[1], fs_rename.oldname, fs_rename.newname);
        return (0);
      }
      oz_knl_printk ("renamed %s:%s to %s\n", argv[1], fs_rename.oldname, fs_rename.newname);
      continue;
    }

    /* Process SET command */

    if (strcasecmp (argv[0], "SET") == 0) {
      if (argc != 3) {
        oz_knl_printk ("SET command requires 2 arguments: SET <param> <value>\n");
        oz_knl_printk ("  for example: set load_directory /newstuff/binaries/\n");
        oz_knl_printk ("           or: set load_device ''\n");
        return (0);
      }
      if (strcmp (argv[2], "''") == 0) argv[2] = "";
      if (!oz_ldr_set (oz_knl_printk, argv[1], argv[2])) return (0);
      continue;
    }

    /* Process SHOW command */

    if (strcasecmp (argv[0], "SHOW") == 0) {
      oz_ldr_show (oz_knl_printk, &ctrlcy_hit);
      continue;
    }

    /* Process THREADS command */

    if (strcasecmp (argv[0], "THREADS") == 0) {
      oz_ldr_thread_dump ();
      continue;
    }

    /* Process TIME command */

    if (strcasecmp (argv[0], "TIME") == 0) {
      now = oz_hw_tod_getnow ();
      oz_knl_printk ("UTC time is %##t\n", now);
      oz_knl_printk ("Lcl time is %t\n", now);
      continue;
    }

    /* Process TYPE command */

    if (strcasecmp (argv[0], "TYPE") == 0) {
      if (argc != 2) {
        oz_knl_printk ("TYPE command requires 1 argument: TYPE <file>\n");
        oz_knl_printk ("  for example: type floppy.p0.oz_dfs:/commonstartup.cli\n");
        return (0);
      }

      /* Open input file */

      p = strchr (argv[1], ':');
      if (p == NULL) {
        oz_knl_printk ("missing : in %s\n", argv[1]);
        return (0);
      }
      *p = 0;
      sts = oz_knl_iochan_crbynm (argv[1], OZ_LOCKMODE_CR, OZ_PROCMODE_KNL, NULL, &tempiochan);
      if (sts != OZ_SUCCESS) {
        oz_knl_printk ("error %u assigning channel to %s\n", sts, argv[1]);
        return (0);
      }
      *(p ++) = ':';
      memset (&fs_open, 0, sizeof fs_open);
      fs_open.name     = p;
      fs_open.lockmode = OZ_LOCKMODE_CR;
      sts = oz_knl_io (tempiochan, OZ_IO_FS_OPEN, sizeof fs_open, &fs_open);
      if (sts != OZ_SUCCESS) {
        oz_knl_printk ("error %u opening file %s\n", sts, argv[1]);
        oz_knl_iochan_increfc (tempiochan, -1);
        return (0);
      }
      memset (&fs_getinfo1, 0, sizeof fs_getinfo1);
      sts = oz_knl_io (tempiochan, OZ_IO_FS_GETINFO1, sizeof fs_getinfo1, &fs_getinfo1);
      if ((sts != OZ_SUCCESS) && (sts != OZ_BADIOFUNC)) {
        oz_knl_printk ("error %u getting file %s size\n", sts, argv[1]);
        oz_knl_iochan_increfc (tempiochan, -1);
        return (0);
      }

      memset (&fs_readrec, 0, sizeof fs_readrec);
      fs_readrec.size = 79;
      fs_readrec.buff = tempbuf;
      fs_readrec.rlen = &count;
      fs_readrec.trmsize = 1;
      fs_readrec.trmbuff = "\n";
      while (!ctrlcy_hit) {
        for (i = 0; i < 20; i ++) {
          sts = oz_knl_io (tempiochan, OZ_IO_FS_READREC, sizeof fs_readrec, &fs_readrec);
          if ((sts != OZ_SUCCESS) && (sts != OZ_NOTERMINATOR)) goto type_done;
          tempbuf[count] = 0;
          oz_knl_printk ("%s%s\n", tempbuf, (sts == OZ_SUCCESS) ? "" : "-");
          if (ctrlcy_hit) break;
        }
        if ((iochan == NULL) && !oz_hw_getcon (4, tempbuf, 3, ">> ")) break;
      }
type_done:
      oz_knl_iochan_increfc (tempiochan, -1);
      continue;
    }

    /* Process VOLUME command */

    if (strcasecmp (argv[0], "VOLUME") == 0) {
      char freebytesstr[32], totalbytesstr[32];
      OZ_IO_fs_getinfo3 fs_getinfo3;
      uLong clusterbytes;
      uQuad freebytes, totalbytes;

      if (argc != 2) {
        oz_knl_printk ("VOLUME command requires 1 argument: VOLUME <fsdev>\n");
        oz_knl_printk ("  for example: volume floppy.p0.oz_dfs\n");
        return (0);
      }

      /* Access fs device */

      sts = oz_knl_iochan_crbynm (argv[1], OZ_LOCKMODE_CR, OZ_PROCMODE_KNL, NULL, &tempiochan);
      if (sts != OZ_SUCCESS) {
        oz_knl_printk ("error %u assigning channel to %s\n", sts, argv[1]);
        return (0);
      }

      /* Get the info */

      memset (&fs_getinfo3, 0, sizeof fs_getinfo3);
      sts = oz_knl_io (tempiochan, OZ_IO_FS_GETINFO3, sizeof fs_getinfo3, &fs_getinfo3);
      oz_knl_iochan_increfc (tempiochan, -1);
      if (sts != OZ_SUCCESS) {
        oz_knl_printk ("error %u getting volume info\n", sts);
        return (0);
      }

      clusterbytes = fs_getinfo3.clusterfactor * fs_getinfo3.blocksize;
      freebytes    = fs_getinfo3.clustersfree;
      freebytes   *= clusterbytes;
      totalbytes   = fs_getinfo3.clustertotal;
      totalbytes  *= clusterbytes;
      oz_knl_printk ("       Blocksize: %u bytes\n"
                     "   Clusterfactor: %u blocks, %u bytes\n"
                     "    Clustersfree: %u, %u blocks, %s\n"
                     "    Clustertotal: %u, %u blocks, %s\n",
                                   fs_getinfo3.blocksize,
                                   fs_getinfo3.clusterfactor, clusterbytes,
                                   fs_getinfo3.clustersfree,  fs_getinfo3.clustersfree  * fs_getinfo3.clusterfactor, quadmemsize (freebytes,  freebytesstr), 
                                   fs_getinfo3.clustertotal,  fs_getinfo3.clustertotal  * fs_getinfo3.clusterfactor, quadmemsize (totalbytes, totalbytesstr));
      if (fs_getinfo3.nincache != 0) {
        oz_knl_printk ("        In cache: %u page(s)\n"
                       "           Dirty: %u page(s)\n",
                                     fs_getinfo3.nincache,
                                     fs_getinfo3.ndirties);
      }
      if (fs_getinfo3.avgwriterate != 0) {
        oz_knl_printk ("  Avg write rate: %u page(s)/sec\n", fs_getinfo3.avgwriterate);
      }
      if (fs_getinfo3.dirty_interval != 0) {
        oz_knl_printk ("  Dirty interval: %#t\n", fs_getinfo3.dirty_interval);
      }
      oz_knl_printk ("     Mount flags:");
      if (fs_getinfo3.mountflags == 0)  oz_knl_printk (" (none)\n");
      else {
        if (fs_getinfo3.mountflags & OZ_FS_MOUNTFLAG_NOCACHE)   oz_knl_printk (" -nocache");
        if (fs_getinfo3.mountflags & OZ_FS_MOUNTFLAG_READONLY)  oz_knl_printk (" -readonly");
        if (fs_getinfo3.mountflags & OZ_FS_MOUNTFLAG_WRITETHRU) oz_knl_printk (" -writethru");
        oz_knl_printk ("\n");
      }
      continue;
    }

    /* Process WRITEBOOT command */

    if (strcasecmp (argv[0], "WRITEBOOT") == 0) {
      if ((argc < 2) || (argc > 4)) {
        oz_knl_printk ("WRITEBOOT command requires 1 argument: WRITEBOOT <loaderimage>\n");
        oz_knl_printk ("  for example: writeboot floppy.p0.oz_dfs:/oz_loader_486.bb\n");
        return (0);
      }

      now = oz_hw_tod_getnow ();
      memset (oz_ldr_paramblock.signature, 0, sizeof oz_ldr_paramblock.signature);
      oz_sys_datebin_decstr (0, now, sizeof oz_ldr_paramblock.signature, oz_ldr_paramblock.signature);

      memset (&fs_writeboot, 0, sizeof fs_writeboot);
      if (argc > 2) {
        fs_writeboot.secpertrk = oz_hw_atoi (argv[2], &usedup);
        if (argv[2][usedup] != 0) {
          oz_knl_printk ("bad sectors/track %s\n", argv[2]);
          return (0);
        }
      }
      if (argc > 3) {
        fs_writeboot.trkpercyl = oz_hw_atoi (argv[3], &usedup);
        if (argv[3][usedup] != 0) {
          oz_knl_printk ("bad tracks/cylinder %s\n", argv[3]);
          return (0);
        }
      }

      p = strchr (argv[1], ':');
      if (p == NULL) {
        oz_knl_printk ("missing : in %s\n", argv[1]);
        return (0);
      }
      *p = 0;
      sts = oz_knl_iochan_crbynm (argv[1], OZ_LOCKMODE_CW, OZ_PROCMODE_KNL, NULL, &fileiochan);
      if (sts != OZ_SUCCESS) {
        oz_knl_printk ("error %u assigning channel to %s\n", sts, argv[1]);
        return (0);
      }
      *(p ++) = ':';
      memset (&fs_open, 0, sizeof fs_open);
      fs_open.name     = p;
      fs_open.lockmode = OZ_LOCKMODE_PW;
      sts = oz_knl_io (fileiochan, OZ_IO_FS_OPEN, sizeof fs_open, &fs_open);
      if (sts != OZ_SUCCESS) {
        oz_knl_printk ("error %u opening loader image %s\n", sts, argv[1]);
        oz_knl_iochan_increfc (fileiochan, -1);
        return (0);
      }
      memset (&fs_writeblocks, 0, sizeof fs_writeblocks);
      fs_writeblocks.size = OZ_LDR_PARAMS_SIZ;
      fs_writeblocks.buff = &oz_ldr_paramblock;
      fs_writeblocks.svbn = OZ_LDR_PARAMS_VBN;
      sts = oz_knl_io (fileiochan, OZ_IO_FS_WRITEBLOCKS, sizeof fs_writeblocks, &fs_writeblocks);
      if (sts != OZ_SUCCESS) {
        oz_knl_printk ("error %u writing parameter block\n", sts);
        oz_knl_iochan_increfc (fileiochan, -1);
        return (0);
      }
      sts = oz_knl_io (fileiochan, OZ_IO_FS_WRITEBOOT, sizeof fs_writeboot, &fs_writeboot);
      oz_knl_iochan_increfc (fileiochan, -1);
      if (sts != OZ_SUCCESS) {
        oz_knl_printk ("error %u writing boot block\n", sts);
        return (0);
      }
      continue;
    }

    oz_knl_printk ("unknown command or parameter '%s'\n", linebuf);
  }
}

/************************************************************************/
/*									*/
/*  Copy a file (or directory)						*/
/*									*/
/*    Input:								*/
/*									*/
/*	iname = input file name						*/
/*	oname = output file name					*/
/*									*/
/*    Output:								*/
/*									*/
/*	copyfile = OZ_SUCCESS : successful				*/
/*	                 else : error status				*/
/*									*/
/************************************************************************/

static uLong copyfile (char *iname, char *oname)

{
  char *p, *q;
  char cbuff[OZ_FS_MAXFNLEN], ibuff[OZ_FS_MAXFNLEN], obuff[OZ_FS_MAXFNLEN];
  int ateof, isadir;
  uLong inputblocksize, outputblocksize, sts, svbn, ul;
  OZ_Datebin finished, started;
  OZ_Dbn inputendoffile;
  OZ_IO_fs_create fs_create;
  OZ_IO_fs_extend fs_extend;
  OZ_IO_fs_getinfo1 fs_getinfo1;
  OZ_IO_fs_open fs_open;
  OZ_IO_fs_readblocks fs_readblocks;
  OZ_IO_fs_readdir fs_readdir;
  OZ_IO_fs_readrec fs_readrec;
  OZ_IO_fs_writeblocks fs_writeblocks;
  OZ_IO_fs_writeboot fs_writeboot;
  OZ_IO_fs_writerec fs_writerec;
  OZ_Iochan *ochan, *ichan;

  /* Assign I/O channel to input file's device */

  p = strchr (iname, ':');
  if (p == NULL) {
    oz_knl_printk ("missing : in %s\n", iname);
    return (OZ_BADPARAM);
  }
  *p = 0;
  sts = oz_knl_iochan_crbynm (iname, OZ_LOCKMODE_CR, OZ_PROCMODE_KNL, NULL, &ichan);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("error %u assigning channel to %s\n", sts, iname);
    return (sts);
  }
  *(p ++) = ':';

  /* Open the input file (or directory) */

  memset (&fs_open, 0, sizeof fs_open);
  fs_open.name      = p;
  fs_open.lockmode  = OZ_LOCKMODE_CR;
  fs_open.rnamesize = sizeof ibuff;
  fs_open.rnamebuff = ibuff;
  ibuff[0] = 0;
  sts = oz_knl_io (ichan, OZ_IO_FS_OPEN, sizeof fs_open, &fs_open);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("error %u opening file %s\n", sts, iname);
    goto copy_done1;
  }
  if (ibuff[0] == 0) strncpyz (ibuff, p, sizeof ibuff);
  oz_knl_printk ("opened file %s\n", ibuff);

  /* Get input file's size and determine if it is a directory or not */

  memset (&fs_getinfo1, 0, sizeof fs_getinfo1);
  sts = oz_knl_io (ichan, OZ_IO_FS_GETINFO1, sizeof fs_getinfo1, &fs_getinfo1);
  if ((sts != OZ_SUCCESS) && (sts != OZ_BADIOFUNC)) {
    oz_knl_printk ("error %u getting file %s size\n", sts, ibuff);
    goto copy_done1;
  }
  inputblocksize = 0;
  inputendoffile = 0;
  isadir = 0;
  if (sts == OZ_SUCCESS) {
    inputblocksize = fs_getinfo1.blocksize;
    inputendoffile = fs_getinfo1.eofblock;
    if ((inputendoffile != 0) && (fs_getinfo1.eofbyte == 0)) inputendoffile --;
    isadir = ((fs_getinfo1.filattrflags & OZ_FS_FILATTRFLAG_DIRECTORY) != 0);
  }

  /* Assign I/O channel to output file's device */

  p = strchr (oname, ':');
  if (p == NULL) {
    oz_knl_printk ("missing : in %s\n", oname);
    sts = OZ_BADPARAM;
    goto copy_done1;
  }
  *p = 0;
  sts = oz_knl_iochan_crbynm (oname, OZ_LOCKMODE_CW, OZ_PROCMODE_KNL, NULL, &ochan);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("error %u assigning channel to %s\n", sts, oname);
    goto copy_done1;
  }
  *(p ++) = ':';

  /* Create the output file (or directory) */

  memset (&fs_create, 0, sizeof fs_create);
  fs_create.name         = p;
  fs_create.lockmode     = OZ_LOCKMODE_PW;
  if (isadir) fs_create.filattrflags = OZ_FS_FILATTRFLAG_DIRECTORY;
  fs_create.rnamesize    = sizeof obuff;
  fs_create.rnamebuff    = obuff;
  obuff[0] = 0;
  sts = oz_knl_io (ochan, OZ_IO_FS_CREATE, sizeof fs_create, &fs_create);
  if (isadir) {
    if (sts == OZ_SUCCESS) goto copy_dir;
    if (sts == OZ_FILEALREADYEXISTS) goto copy_dir;
  }
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("error %u creating %s %s\n", sts, isadir ? "directory" : "file", oname);
    goto copy_done2;
  }
  if (obuff[0] == 0) strncpyz (obuff, p, sizeof obuff);
  oz_knl_printk ("created file %s\n", obuff);

  /* Get output file block size */

  memset (&fs_getinfo1, 0, sizeof fs_getinfo1);
  sts = oz_knl_io (ochan, OZ_IO_FS_GETINFO1, sizeof fs_getinfo1, &fs_getinfo1);
  if ((sts != OZ_SUCCESS) && (sts != OZ_BADIOFUNC)) {
    oz_knl_printk ("error %u getting file %s size\n", sts, obuff);
    goto copy_done1;
  }
  outputblocksize = 0;
  if (sts == OZ_SUCCESS) outputblocksize = fs_getinfo1.blocksize;

  /* Try to extend the file all at once (to make it as contig as possible) */
  /* Loader image needs to be completely contiguous                        */

  if ((outputblocksize != 0) && (inputblocksize != 0)) {
    memset (&fs_extend, 0, sizeof fs_extend);
    fs_extend.nblocks = inputendoffile;
    if (outputblocksize > inputblocksize) {
      fs_extend.nblocks += outputblocksize / inputblocksize - 1;
      fs_extend.nblocks /= outputblocksize / inputblocksize;
    }
    if (inputblocksize > outputblocksize) fs_extend.nblocks *= inputblocksize / outputblocksize;
    sts = oz_knl_io (ochan, OZ_IO_FS_EXTEND, sizeof fs_extend, &fs_extend);
    if ((sts != OZ_SUCCESS) && (sts != OZ_BADIOFUNC)) {
      oz_knl_printk ("error %u extending file %s to %u blocks\n", sts, obuff, fs_extend.nblocks);
      goto copy_done2;
    }
    oz_knl_printk ("extended %s to %u blocks\n", obuff, fs_extend.nblocks);
  }

  /* Copy records - crude but always effective */

  p = OZ_KNL_NPPMALLOC (COPY_SIZE);
  memset (&fs_readrec,  0, sizeof fs_readrec);
  memset (&fs_writerec, 0, sizeof fs_writerec);
  fs_readrec.size     = COPY_SIZE;
  fs_readrec.buff     = p;
  fs_readrec.trmsize  = 1;
  fs_readrec.trmbuff  = "\n";
  fs_readrec.rlen     = &fs_writerec.size;
  fs_readrec.pmtsize  = 1;
  fs_readrec.pmtbuff  = ">";
  fs_writerec.buff    = p;
  fs_writerec.trmbuff = fs_readrec.trmbuff;
  ul = 0;
  ateof = 0;

  /* If input and output are both block-oriented (ie, they have a */
  /* block size) use terminatorless copying as it is much faster. */

  if ((inputblocksize != 0) && (outputblocksize != 0)) fs_readrec.trmsize = 0;

  /* Copy */

  started = oz_hw_tod_getnow ();
  while (((sts = oz_knl_io (ichan, OZ_IO_FS_READREC, sizeof fs_readrec, &fs_readrec)) == OZ_SUCCESS) || (sts == OZ_NOTERMINATOR) || (sts == OZ_ENDOFFILE)) {
    if (sts == OZ_ENDOFFILE) ateof = 1;
    fs_writerec.trmsize = 0;
    if (sts == OZ_SUCCESS) fs_writerec.trmsize = fs_readrec.trmsize;
    ul += fs_writerec.size + fs_writerec.trmsize;
    sts = oz_knl_io (ochan, OZ_IO_FS_WRITEREC, sizeof fs_writerec, &fs_writerec);
    if (sts != OZ_SUCCESS) {
      oz_knl_printk ("error %u writing to %s\n", sts, obuff);
      goto copy_done;
    }
    oz_sys_sprintf (sizeof ctrlt_msg, ctrlt_msg, "%u bytes copied\n", ul);
    if (ateof) break;
    sts = OZ_ABORTEDBYCLI;
    if (ctrlcy_hit) break;
  }
  finished = oz_hw_tod_getnow ();
  if (sts != OZ_SUCCESS) oz_knl_printk ("error %u reading from %s\n", sts, ibuff);
  else {
    sts = oz_knl_io (ochan, OZ_IO_FS_CLOSE, 0, NULL);
    if (sts != OZ_SUCCESS) oz_knl_printk ("error %u closing %s\n", sts, obuff);
    else {
      OZ_HW_DATEBIN_SUB (finished, finished, started);
      svbn = ((OZ_Datebin)ul * OZ_TIMER_RESOLUTION) / 1000 / finished;
      oz_knl_printk ("%u bytes copied in %#t (%u KB/sec)\n", ul, finished, svbn);
    }
  }

copy_done:
  ctrlt_msg[0] = 0;
  OZ_KNL_NPPFREE (p);
copy_done2:
  oz_knl_iochan_increfc (ochan, -1);
copy_done1:
  oz_knl_iochan_increfc (ichan, -1);
  return (sts);

  /* Copy directory, file-by-file */

copy_dir:
  oz_knl_iochan_increfc (ochan, -1);
  p = OZ_KNL_NPPMALLOC ((sizeof cbuff) * 2 + strlen (iname) + strlen (oname) + 2);
  memset (&fs_readdir, 0, sizeof fs_readdir);
  fs_readdir.filenamsize = sizeof cbuff;
  fs_readdir.filenambuff = cbuff;
  while ((sts = oz_knl_io (ichan, OZ_IO_FS_READDIR, sizeof fs_readdir, &fs_readdir)) == OZ_SUCCESS) {
    strcpy (p, iname);
    strcat (p, cbuff);
    q = p + strlen (p) + 1;
    strcpy (q, oname);
    strcat (q, cbuff);
    sts = copyfile (p, q);
    if (sts != OZ_SUCCESS) goto copy_dir_done;
    sts = OZ_ABORTEDBYCLI;
    if (ctrlcy_hit) break;
  }
  if (sts == OZ_ENDOFFILE) sts = OZ_SUCCESS;
  else oz_knl_printk ("error %u reading input directory %s\n", sts, iname);
copy_dir_done:
  OZ_KNL_NPPFREE (p);
  oz_knl_iochan_increfc (ichan, -1);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Delete a file (or directory)					*/
/*									*/
/*    Input:								*/
/*									*/
/*	name   = name of file or directory to delete			*/
/*									*/
/*    Output:								*/
/*									*/
/*	deletefile = OZ_SUCCESS : successful				*/
/*	                   else : error status				*/
/*									*/
/************************************************************************/

static uLong deletefile (char *name)

{
  char cbuff[OZ_FS_MAXFNLEN], ibuff[OZ_FS_MAXFNLEN], *p, rnamebuff[OZ_FS_MAXFNLEN];
  OZ_IO_fs_open fs_open;
  OZ_IO_fs_readdir fs_readdir;
  OZ_IO_fs_remove fs_remove;
  OZ_Iochan *tempiochan;
  uLong sts;

  p = strchr (name, ':');
  if (p == NULL) {
    oz_knl_printk ("missing : in %s\n", name);
    return (OZ_BADPARAM);
  }
  *p = 0;
  sts = oz_knl_iochan_crbynm (name, OZ_LOCKMODE_CW, OZ_PROCMODE_KNL, NULL, &tempiochan);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("error %u assigning channel to filesystem device %s\n", sts, name);
    return (sts);
  }
  *(p ++) = ':';

  memset (&fs_remove, 0, sizeof fs_remove);
  fs_remove.name      = p;
  fs_remove.rnamesize = sizeof rnamebuff;
  fs_remove.rnamebuff = rnamebuff;
  rnamebuff[0] = 0;
  sts = oz_knl_io (tempiochan, OZ_IO_FS_REMOVE, sizeof fs_remove, &fs_remove);
  if (sts == OZ_DIRNOTEMPTY) {
    oz_knl_printk ("cleaning out directory %s\n", name);
    memset (&fs_open, 0, sizeof fs_open);
    fs_open.name      = p;
    fs_open.lockmode  = OZ_LOCKMODE_CR;
    sts = oz_knl_io (tempiochan, OZ_IO_FS_OPEN, sizeof fs_open, &fs_open);
    if (sts != OZ_SUCCESS) {
      oz_knl_printk ("error %u opening directory %s\n", sts, p);
      goto delete_done;
    }
    p = OZ_KNL_NPPMALLOC ((sizeof cbuff) + strlen (name) + 1);
    memset (&fs_readdir, 0, sizeof fs_readdir);
    fs_readdir.filenamsize = sizeof cbuff;
    fs_readdir.filenambuff = cbuff;
    while ((sts = oz_knl_io (tempiochan, OZ_IO_FS_READDIR, sizeof fs_readdir, &fs_readdir)) == OZ_SUCCESS) {
      strcpy (p, name);
      strcat (p, cbuff);
      sts = deletefile (p);
      if (sts != OZ_SUCCESS) {
        OZ_KNL_NPPFREE (p);
        goto delete_done;
      }
      sts = OZ_ABORTEDBYCLI;
      if (ctrlcy_hit) break;
    }
    OZ_KNL_NPPFREE (p);
    if (sts != OZ_ENDOFFILE) {
      oz_knl_printk ("error %u reading directory %s\n", sts, name);
      goto delete_done;
    }
    sts = oz_knl_io (tempiochan, OZ_IO_FS_REMOVE, sizeof fs_remove, &fs_remove);
  }
  if (sts != OZ_SUCCESS) oz_knl_printk ("error %u deleting %s\n", sts, name);
  else oz_knl_printk ("%s deleted\n", rnamebuff);
  if (sts == OZ_NOSUCHFILE) sts = OZ_SUCCESS;
delete_done:
  oz_knl_iochan_increfc (tempiochan, -1);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Perform GUNZIP/GZIP operation					*/
/*									*/
/************************************************************************/

typedef struct { OZ_Iochan *ichan, *ochan;
                 char ibuff[OZ_FS_MAXFNLEN], obuff[OZ_FS_MAXFNLEN];
                 uLong ibytes, obytes;
               } Gzpb;

static int   gzip_read   (void *gzpbv, int siz, char *buf, int *len, char **pnt);
static int   gzip_write  (void *gzpbv, int siz, char *buf);
static void  gzip_error  (void *gzpbv, int code, char *msg);
static void *gzip_malloc (void *gzpbv, int size);
static void  gzip_free   (void *gzpvb, void *buff);

static int dogzip (int gzipfc, char *iname, char *oname)

{
  char *p;
  Gzpb gzpb;
  int rc;
  OZ_IO_fs_create fs_create;
  OZ_IO_fs_open fs_open;
  uLong sts;

  /* Assign I/O channel to input file's device */

  p = strchr (iname, ':');
  if (p == NULL) {
    oz_knl_printk ("missing : in %s\n", iname);
    return (0);
  }
  *p = 0;
  sts = oz_knl_iochan_crbynm (iname, OZ_LOCKMODE_CR, OZ_PROCMODE_KNL, NULL, &gzpb.ichan);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("error %u assigning channel to %s\n", sts, iname);
    return (0);
  }
  *(p ++) = ':';

  /* Open the input file */

  memset (&fs_open, 0, sizeof fs_open);
  fs_open.name      = p;
  fs_open.lockmode  = OZ_LOCKMODE_CR;
  fs_open.rnamesize = sizeof gzpb.ibuff;
  fs_open.rnamebuff = gzpb.ibuff;
  gzpb.ibuff[0] = 0;
  sts = oz_knl_io (gzpb.ichan, OZ_IO_FS_OPEN, sizeof fs_open, &fs_open);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("error %u opening %s\n", sts, iname);
    goto copy_done1;
  }
  if (gzpb.ibuff[0] == 0) strncpyz (gzpb.ibuff, p, sizeof gzpb.ibuff);
  oz_knl_printk ("opened %s\n", gzpb.ibuff);

  /* Assign I/O channel to output file's device */

  p = strchr (oname, ':');
  if (p == NULL) {
    oz_knl_printk ("missing : in %s\n", oname);
    sts = OZ_BADPARAM;
    goto copy_done1;
  }
  *p = 0;
  sts = oz_knl_iochan_crbynm (oname, OZ_LOCKMODE_CW, OZ_PROCMODE_KNL, NULL, &gzpb.ochan);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("error %u assigning channel to %s\n", sts, oname);
    goto copy_done1;
  }
  *(p ++) = ':';

  /* Create the output file */

  memset (&fs_create, 0, sizeof fs_create);
  fs_create.name         = p;
  fs_create.lockmode     = OZ_LOCKMODE_PW;
  fs_create.rnamesize    = sizeof gzpb.obuff;
  fs_create.rnamebuff    = gzpb.obuff;
  gzpb.obuff[0] = 0;
  sts = oz_knl_io (gzpb.ochan, OZ_IO_FS_CREATE, sizeof fs_create, &fs_create);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("error %u creating %s\n", sts, oname);
    goto copy_done2;
  }
  if (gzpb.obuff[0] == 0) strncpyz (gzpb.obuff, p, sizeof gzpb.obuff);
  oz_knl_printk ("created %s\n", gzpb.obuff);

  /* (UN)Zip input file to output file */

  gzpb.ibytes = 0;
  gzpb.obytes = 0;
  rc = gzip (gzip_read, gzip_write, gzip_error, gzip_malloc, gzip_free, &gzpb, gzipfc, 6);
  if (rc == GZIP_RTN_OK) oz_knl_printk ("\n");

  /* Close files */

  sts = oz_knl_io (gzpb.ochan, OZ_IO_FS_CLOSE, 0, NULL);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("error %u closing %s\n", sts, gzpb.obuff);
    goto copy_done2;
  }

  oz_knl_iochan_increfc (gzpb.ichan, -1);
  oz_knl_iochan_increfc (gzpb.ochan, -1);

  /* Output summary message */

  if (gzpb.ibytes != 0) {
    if (gzipfc == GZIP_FUNC_COMPRESS) oz_knl_printk ("%u bytes in %s compressed to %u bytes in %s (%u%%)\n", 
				gzpb.ibytes, gzpb.ibuff, gzpb.obytes, gzpb.obuff, (gzpb.obytes * 100) / gzpb.ibytes);
    if (gzipfc == GZIP_FUNC_EXPAND) oz_knl_printk ("%u bytes in %s expanded to %u bytes in %s (%u%%)\n", 
				gzpb.ibytes, gzpb.ibuff, gzpb.obytes, gzpb.obuff, (gzpb.obytes * 100) / gzpb.ibytes);
  }

  return (rc == GZIP_RTN_OK);

  /* Error, close channels and return error status */

copy_done2:
  oz_knl_iochan_increfc (gzpb.ochan, -1);
copy_done1:
  oz_knl_iochan_increfc (gzpb.ichan, -1);
  return (0);
}

/* Read input file */

static int gzip_read (void *gzpbv, int siz, char *buf, int *len, char **pnt)

{
  Gzpb *gzpb;
  OZ_IO_fs_readrec fs_readrec;
  uLong rlen, sts;

  gzpb = gzpbv;

  oz_knl_printk ("r");

  memset (&fs_readrec, 0, sizeof fs_readrec);
  fs_readrec.size = siz;
  fs_readrec.buff = buf;
  fs_readrec.rlen = &rlen;

  do sts = oz_knl_io (gzpb -> ichan, OZ_IO_FS_READREC, sizeof fs_readrec, &fs_readrec);
  while ((sts == OZ_SUCCESS) && (rlen == 0));
  if ((sts != OZ_SUCCESS) && (sts != OZ_ENDOFFILE)) {
    oz_knl_printk ("\nerror %u reading input file %s\n", sts, gzpb -> ibuff);
    return (0);
  }
  gzpb -> ibytes += rlen;
  *len = rlen;
  *pnt = buf;
  return (1);
}

/* Write output file */

static int gzip_write (void *gzpbv, int siz, char *buf)

{
  Gzpb *gzpb;
  OZ_IO_fs_writerec fs_writerec;
  uLong sts;

  gzpb = gzpbv;

  oz_knl_printk ("w");

  memset (&fs_writerec, 0, sizeof fs_writerec);
  fs_writerec.size = siz;
  fs_writerec.buff = buf;

  sts = oz_knl_io (gzpb -> ochan, OZ_IO_FS_WRITEREC, sizeof fs_writerec, &fs_writerec);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("\nerror %u writing output file %s\n", sts, gzpb -> obuff);
    return (0);
  }
  gzpb -> obytes += siz;
  return (1);
}

static void gzip_error (void *gzpbv, int code, char *msg)

{
  oz_knl_printk ("\ninternal gzip error %d: %s\n", code, msg);
}

static void *gzip_malloc (void *gzpbv, int size)

{
  return (OZ_KNL_NPPMALLOC (size));
}

static void gzip_free (void *gzpvb, void *buff)

{
  return (OZ_KNL_NPPFREE (buff));
}

static char *quadmemsize (uQuad size, char *buff)

{
  uQuad tenths;

  if (size >= ((uQuad)10) << 30) {
    tenths   = size & 0x3FFFFFFF;
    tenths  *= 10;
    tenths >>= 30;
    oz_sys_sprintf (32, buff, "%u.%u Gigabytes", (uLong)(size >> 30), (uLong)tenths);
  } else if (size >= 100*1024*1024) {
    oz_sys_sprintf (32, buff, "%u Megabytes", (uLong)(size >> 20));
  } else {
    oz_sys_sprintf (32, buff, "%u Kilobytes", (uLong)(size >> 10));
  }
  return (buff);
}
