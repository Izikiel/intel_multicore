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
/*  This is the general disk filesystem driver, fs independent layer	*/
/*									*/
/*  It processes file system I/O requestes and generates the necessary 	*/
/*  disk I/O requests.							*/
/*									*/
/************************************************************************/

#define OZ_VDFS_VERSION 4

#include "ozone.h"
#include "oz_dev_timer.h"
#include "oz_dev_vdfs.h"
#include "oz_hw_bootblock.h"
#include "oz_io_console.h"
#include "oz_io_disk.h"
#include "oz_io_fs.h"
#include "oz_knl_dcache.h"
#include "oz_knl_devio.h"
#include "oz_knl_event.h"
#include "oz_knl_hw.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_procmode.h"
#include "oz_knl_sdata.h"
#include "oz_knl_section.h"
#include "oz_knl_security.h"
#include "oz_knl_shuthand.h"
#include "oz_knl_spte.h"
#include "oz_knl_status.h"
#include "oz_knl_thread.h"
#include "oz_knl_userjob.h"
#include "oz_sys_dateconv.h"
#include "oz_sys_xprintf.h"

#define THREAD_PRIORITY oz_knl_user_getmaxbasepri (NULL)

typedef struct OZ_VDFS_Chnex    Chnex;
typedef struct OZ_VDFS_Devex    Devex;
typedef struct OZ_VDFS_File     File;
typedef struct OZ_VDFS_Fileid   Fileid;
typedef struct OZ_VDFS_Iopex    Iopex;
typedef struct OZ_VDFS_Vector   Vector;
typedef struct OZ_VDFS_Volume   Volume;
typedef struct OZ_VDFS_Wildscan Wildscan;

/* Test the directory bit */

#define IS_DIRECTORY(__file) ((*(__file -> volume -> devex -> vector -> is_directory)) (__file))

/* File-id structure */

struct OZ_VDFS_Fileid { uByte opaque[OZ_FS_MAXFIDLN]; };

/* Function table */

static uLong oz_disk_fs_clonecre (OZ_Devunit *template_devunit, void *template_devexv, int template_cloned, OZ_Procmode procmode, OZ_Devunit **cloned_devunit);
static int oz_disk_fs_clonedel (OZ_Devunit *cloned_devunit, void *devexv, int cloned);
static uLong oz_disk_fs_assign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode);
static int oz_disk_fs_deassign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv);
static void oz_disk_fs_abort (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Ioop *ioop, void *iopexv, OZ_Procmode procmode);
static uLong oz_disk_fs_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode, 
                               OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap);

static const OZ_Devfunc oz_disk_fs_functable = { sizeof (Devex), sizeof (Chnex), sizeof (Iopex), 0, 
                                                 NULL, oz_disk_fs_clonecre, oz_disk_fs_clonedel, 
                                                 oz_disk_fs_assign, oz_disk_fs_deassign, 
                                                 oz_disk_fs_abort, oz_disk_fs_start, NULL };

/* Driver static data */

static File *crash_file = NULL;
static OZ_IO_disk_crash crash_disk;


/* I/O request processing routine table */

	/* - these routines are their own kernel threads */

static uLong kt_initvol (void *iopexv);
static uLong kt_mountvol (void *iopexv);
static void shuthand (void *devexv);

	/* - the be_ routines are called by the kt_mountvol thread to process requests queued to devex->iopexqh/iopexqt */
	/*   the sc_ routines are called directly by the startup routines                                               */

static uLong be_shutdown (Iopex *iopex, Chnex *chnex, Devex *devex);
static uLong be_dismount (Iopex *iopex, Chnex *chnex, Devex *devex);
static uLong be_verifyvol (Iopex *iopex, Chnex *chnex, Devex *devex);
static uLong be_create (Iopex *iopex, Chnex *chnex, Devex *devex);
static uLong be_open (Iopex *iopex, Chnex *chnex, Devex *devex);
static uLong be_close (Iopex *iopex, Chnex *chnex, Devex *devex);
static uLong be_enter (Iopex *iopex, Chnex *chnex, Devex *devex);
static uLong be_remove (Iopex *iopex, Chnex *chnex, Devex *devex);
static uLong be_rename (Iopex *iopex, Chnex *chnex, Devex *devex);
static uLong be_extend (Iopex *iopex, Chnex *chnex, Devex *devex);
static uLong sc_writeblocks (Iopex *iopex, Chnex *chnex, Devex *devex);
static uLong sc_readblocks (Iopex *iopex, Chnex *chnex, Devex *devex);
static uLong sc_ixdeb (Iopex *iopex, Chnex *chnex, Devex *devex);
static uLong sc_writerec (Iopex *iopex, Chnex *chnex, Devex *devex);
static uLong sc_readrec (Iopex *iopex, Chnex *chnex, Devex *devex);
static uLong sc_getinfo1 (Iopex *iopex, Chnex *chnex, Devex *devex);
static uLong sc_readdir (Iopex *iopex, Chnex *chnex, Devex *devex);
static uLong be_getsecattr (Iopex *iopex, Chnex *chnex, Devex *devex);
static uLong be_writeboot (Iopex *iopex, Chnex *chnex, Devex *devex);
static uLong be_setcurpos (Iopex *iopex, Chnex *chnex, Devex *devex);
static uLong sc_wildscan (Iopex *iopex, Chnex *chnex, Devex *devex);
static void wildscan_topdiropen (void *chnexv, uLong status);
static void wildscan_subdiropen (void *chnexv, uLong status);
static void wildscan_startdir (Chnex *chnex);
static void wildscan_lowipl (void *chnexv, OZ_Lowipl *lowipl);
static void wildscan_dirread (void *chnexv, uLong status);
static int wildscan_match (const char *wildcard, const char *filename, int tilde);
static void wildscan_unlink (Wildscan *wildscan);
static void wildscan_lockdir (void (*entry) (Chnex *chnex), Chnex *chnex);
static void wildscan_trytolock (void *chnexv, OZ_Event *event);
static void wildscan_unlkdir (Chnex *chnex);
static uLong be_getinfo2 (Iopex *iopex, Chnex *chnex, Devex *devex);
static uLong be_getinfo3 (Iopex *iopex, Chnex *chnex, Devex *devex);
static uLong be_crash (Iopex *iopex, Chnex *chnex, Devex *devex);
static uLong be_parse (Iopex *iopex, Chnex *chnex, Devex *devex);

/* Internal routines */

static uLong queuerecio (Iopex *iopex, Chnex *chnex, Devex *devex);
static void finishrecio (Iopex *iopex, uLong status, void (*finentry) (void *iopexv, int finok, uLong *status_r));
static uLong startshortcut (Chnex *chnex, Devex *devex, Iopex *iopex);
static void finishortcut (Chnex *chnex, Devex *devex, Iopex *iopex);
static void validate_shortcuts (Chnex *chnex);
static void iodonex (Iopex *iopex, uLong status, void (*finentry) (void *finparam, int finok, uLong *status_r), void *finparam);
static uLong assign_by_name (const char *devname, OZ_Lockmode lockmode, Iopex *iopex, OZ_Iochan **iochan_r);
static uLong init_volume (int volnamelen, const char *volname, uLong clusterfactor, uLong secattrsize, const void *secattrbuff, uLong initflags, Iopex *iopex);
static uLong write_init_header (Volume *volume, uByte *rootdirbuff, uLong filenum, char *name, uLong secattrsize, const void *secattrbuff, 
                                uLong filattrflags, OZ_Dbn efblk, OZ_Dbn count, OZ_Dbn start, OZ_Dbn index_header_start, Iopex *iopex);
static void check_init_alloc (Iopex *iopex, OZ_Dbn cluster, uLong *clusterbuff, Volume *volume, OZ_Dbn count, OZ_Dbn start);
static uLong mount_volume (Volume **volume_r, uLong mountflags, Iopex *iopex);
static void calc_home_block (Volume *volume);
static uLong dismount_volume (Volume *volume, int unload, int shutdown, Iopex *iopex);
static uLong getdirid (Volume *volume, int fspeclen, const char *fspec, Fileid *dirid_r, int *fnamelen_r, const char **fname_r, char *dname_r, Iopex *iopex);
static uLong lookup_file (File *dirfile, int namelen, const char *name, Fileid *fileid_r, char *name_r, Iopex *iopex);
static uLong enter_file (File *dirfile, const char *dirname, int namelen, const char *name, int newversion, File *file, const Fileid *fileid, char *name_r, Iopex *iopex);
static uLong remove_file (File *dirfile, const char *name, char *name_r, Iopex *iopex);
static int dirisnotempty (File *dirfile, Iopex *iopex);
static uLong create_file (Volume *volume, int namelen, const char *name, uLong filattrflags, OZ_Secattr *secattr, Fileid *dirid, File **file_r, Fileid **fileid_r, Iopex *iopex);
static uLong getcresecattr (Iopex *iopex, uLong secattrsize, const void *secattrbuff, OZ_Secattr **secattr_r);
static uLong set_file_attrs (File *file, uLong numitems, const OZ_Itmlst2 *itemlist, Iopex *iopex);
static uLong extend_file (File *file, OZ_Dbn nblocks, uLong extflags, Iopex *iopex);
static void write_dirty_homeboy (Volume *volume, Iopex *iopex);
static uLong disk_fs_crash (void *dummy, OZ_Dbn vbn, uLong size, OZ_Mempage phypage, uLong offset);
static uLong vdfs_knlpfmap (OZ_Iochan *iochan, OZ_Dbn vbn, OZ_Mempage *phypage_r);
static uLong vdfs_knlpfupd (OZ_Iochan *iochan, OZ_Dbn vbn, OZ_Mempage phypage);
static void vdfs_knlpfrel (OZ_Iochan *iochan, OZ_Mempage phypage);
static uLong diskio (uLong funcode, uLong as, void *ap, Iopex *iopex, OZ_Iochan *iochan);
static uLong revalidate (void *devexv, OZ_Dcache *dcache);
static void revaltimer (void *kthreadv, OZ_Timer *timer);
static uLong map_vbn_to_lbn (File *file, OZ_Dbn virtblock, OZ_Dbn *nblocks_r, OZ_Dbn *logblock_r);
static void printe (Iopex *iopex, const char *format, ...);
static uLong startprintk (void *iochanv, uLong *size, char **buff);

/************************************************************************/
/*									*/
/*  Boot-time initialization routine					*/
/*									*/
/************************************************************************/

void oz_dev_vdfs_init (int version, const char *drivername, const OZ_VDFS_Vector *vector)

{
  Devex *devex;
  OZ_Devclass *devclass;
  OZ_Devdriver *devdriver;
  OZ_Devunit *devunit;

  if (version != OZ_VDFS_VERSION) {
    oz_knl_printk ("oz_dev_vdfs_init: caller '%s' is version %d, but I am version %d\n", drivername, version, OZ_VDFS_VERSION);
    return;
  }

  if (vector -> fileid_size > sizeof (Fileid)) {
    oz_knl_printk ("oz_dev_vdfs_init: caller '%s' fileid size %d, max allowed %d\n", drivername, vector -> fileid_size, sizeof (Fileid));
    return;
  }

  /* Set up template device data structures */

  devunit = oz_knl_devunit_lookup (drivername);
  if (devunit != NULL) oz_knl_devunit_increfc (devunit, -1);
  else {
    oz_knl_printk ("oz_dev_vdfs_init (%s)\n", drivername);
    devclass  = oz_knl_devclass_create (OZ_IO_FS_CLASSNAME, OZ_IO_FS_BASE, OZ_IO_FS_MASK, drivername);
    devdriver = oz_knl_devdriver_create (devclass, drivername);
    devunit   = oz_knl_devunit_create (devdriver, drivername, "init and mount template", &oz_disk_fs_functable, 0, oz_s_secattr_tempdev);
    if (devunit != NULL) {
      devex   = oz_knl_devunit_ex (devunit);
      memset (devex, 0, sizeof *devex);
      devex -> devdriver  = devdriver;
      devex -> drivername = drivername;
      devex -> vector     = vector;
    }
  }
}

/************************************************************************/
/*									*/
/*  An I/O channel is being assigned and the devio routines want to 	*/
/*  know if this device is to be cloned.				*/
/*									*/
/*  In this driver, we only clone the original template device.		*/
/*									*/
/*    Input:								*/
/*									*/
/*	template_devunit = pointer to existing device unit		*/
/*	template_devex   = device extension data			*/
/*	template_cloned  = template's cloned flag			*/
/*	procmode         = processor mode doing the assign		*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_disk_fs_clonecre = OZ_SUCCESS : ok to assign channel		*/
/*	                            else : error status			*/
/*	**cloned_devunit = cloned device unit				*/
/*									*/
/************************************************************************/

static uLong oz_disk_fs_clonecre (OZ_Devunit *template_devunit, void *template_devexv, int template_cloned, OZ_Procmode procmode, OZ_Devunit **cloned_devunit)

{
  char *unitname;
  Devex *devex, *template_devex;
  int i;
  OZ_Event *ioevent, *shortcutev;
  OZ_Secattr *secattr;
  uLong sts;

  /* If this is an already cloned devunit, don't clone anymore, just use the original device */

  if (template_cloned) {
    *cloned_devunit = template_devunit;
    oz_knl_devunit_increfc (template_devunit, 1);
  }

  /* This is the original template device, clone a unit.  The next thing the caller should do is an OZ_IO_FS_INITVOL or OZ_IO_FS_MOUNTVOL call. */

  else {
    sts = oz_knl_event_create (14, "shortcut close", NULL, &shortcutev);
    if (sts != OZ_SUCCESS) return (sts);
    sts = oz_knl_event_create (8, "disk i/o", NULL, &ioevent);
    if (sts != OZ_SUCCESS) {
      oz_knl_event_increfc (shortcutev, -1);
      return (sts);
    }

    template_devex = template_devexv;

    i = strlen (template_devex -> drivername);
    unitname = OZ_KNL_NPPMALLOC (i + 12);
    memcpy (unitname, template_devex -> drivername, i);
    unitname[i++] = '_';
    oz_hw_itoa (oz_hw_atomic_inc_long (&(template_devex -> clonumber), 1), 11, unitname + i);
    secattr = oz_knl_thread_getdefcresecattr (NULL);
    *cloned_devunit = oz_knl_devunit_create (template_devex -> devdriver, unitname, "Not yet mounted", &oz_disk_fs_functable, 1, secattr);
    OZ_KNL_NPPFREE (unitname);
    oz_knl_secattr_increfc (secattr, -1);

    devex = oz_knl_devunit_ex (*cloned_devunit);
    memset (devex, 0, sizeof *devex);
    oz_hw_smplock_init (sizeof devex -> smplock_vl, &(devex -> smplock_vl), OZ_SMPLOCK_LEVEL_VL);
    devex -> devdriver  = template_devex -> devdriver;
    devex -> drivername = template_devex -> drivername;
    devex -> vector     = template_devex -> vector;
    devex -> devunit    = *cloned_devunit;
    devex -> ioevent    = ioevent;
    devex -> shortcutev = shortcutev;
    devex -> iopexqt    = &(devex -> iopexqh);
  }

  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  The last channel was deassigned from a devunit.  This routine is 	*/
/*  called to see if the unit should be deleted.			*/
/*									*/
/*  In this driver, we only delete devices that are not mounted.  We 	*/
/*  also never delete the template device (duh!).			*/
/*									*/
/*    Input:								*/
/*									*/
/*	cloned_devunit = cloned device's devunit struct			*/
/*	devex = the devex of cloned_devunit				*/
/*	cloned = the cloned_devunit's cloned flag			*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_disk_fs_clonedel = 0 : keep device in device table		*/
/*	                      1 : delete device from table		*/
/*									*/
/************************************************************************/

static int oz_disk_fs_clonedel (OZ_Devunit *cloned_devunit, void *devexv, int cloned)

{
  Devex *devex;
  int ok_to_del;

  devex = devexv;

  ok_to_del = (cloned && (devex -> volume == NULL) && (devex -> event == NULL) && (devex -> iopexqh == NULL));
  if (ok_to_del) {
    if (devex -> shortcutev != NULL) {
      oz_knl_event_increfc (devex -> shortcutev, -1);
      devex -> shortcutev = NULL;
    }
    if (devex -> ioevent != NULL) {
      oz_knl_event_increfc (devex -> ioevent, -1);
      devex -> ioevent = NULL;
    }
  }
  return (ok_to_del);
}

/************************************************************************/
/*									*/
/*  An I/O channel was just assigned to the unit			*/
/*									*/
/*  Clear out the channel extension block				*/
/*									*/
/************************************************************************/

static uLong oz_disk_fs_assign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode)

{
  Chnex *chnex;

  chnex = chnexv;
  memset (chnex, 0, sizeof *chnex);
  chnex -> iochan = iochan;
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  An I/O channel was just deassigned from a unit			*/
/*									*/
/*    Input:								*/
/*									*/
/*	devunit  = device unit that is being deassigned from		*/
/*	devexv   = corresponding devex pointer				*/
/*	iochan   = i/o channel being deassigned				*/
/*	chnexv   = corresponding chnex pointer				*/
/*									*/
/************************************************************************/

static void dummydone (void *paramv, uLong status);

static int oz_disk_fs_deassign (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv)

{
  Chnex *chnex;
  uLong sts;

  chnex = chnexv;

  /* If a file or wildcard scan is open or shortcuts busy, close it */

  if ((chnex -> file != NULL) || (chnex -> wildscan != NULL) || (chnex -> shortcuts > 0)) {
    chnex -> ignclose = 0;					/* don't ignore closes anymore */
    sts = oz_knl_iostart2 (1, iochan, OZ_PROCMODE_KNL, dummydone, NULL, NULL, NULL, NULL, NULL, OZ_IO_FS_CLOSE, 0, NULL); /* close */
    if (sts == OZ_STARTED) return (1);				/* if it completes asynchronously, come back when it calls oz_knl_iodone */
    if (sts != OZ_SYSHUTDOWN) {
      if (chnex -> file     != NULL) oz_crash ("oz_dev_vdfs deassign: file left open did not close, status %u", sts); /* make sure it closed */
      if (chnex -> wildscan != NULL) oz_crash ("oz_dev_vdfs deassign: wild left open did not close, status %u", sts); /* make sure it closed */
      if (chnex -> shortcuts > 0) oz_crash ("oz_dev_vdfs deassign: shortcuts did not close, status %u", sts); /* make sure it closed */
    }
  }

  /* The scassochnex should be NULL.  If not it means the associated shortcut routine is still running, */
  /* as the shortcut routine that set this link should also clear it before releasing the channel.      */

  if (chnex -> scassochnex != NULL) oz_crash ("oz_dev_vdfs deassign: chnex %p -> scassochnex %p", chnex, chnex -> scassochnex);

  /* Free off the wildscan lowipl struct, if any present */

  if (chnex -> wild_lowipl != NULL) {
    oz_knl_lowipl_free (chnex -> wild_lowipl);
    chnex -> wild_lowipl = NULL;
  }

  return (0);
}

/* Dummy I/O done routine needed so iostart will know we want async completion */

static void dummydone (void *paramv, uLong status)

{}

/************************************************************************/
/*									*/
/*  Abort I/O request in progress					*/
/*									*/
/************************************************************************/

static void oz_disk_fs_abort (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Ioop *ioop, void *iopexv, OZ_Procmode procmode)

{
  Chnex *chnex;
  Devex *devex;
  File *file;
  Iopex *iopex, **liopex, temp_iopex, *xiopex;
  uLong vl;

  devex  = devexv;
  chnex  = chnexv;
  xiopex = NULL;

  /* Remove any matching requests from kt_mountvol thread queue */

  vl = oz_hw_smplock_wait (&(devex -> smplock_vl));				// lock iopexqh/iopexqt queue
  for (liopex = &(devex -> iopexqh); (iopex = *liopex) != NULL;) {		// scan the queue
    if (oz_knl_ioabortok (iopex -> ioop, iochan, procmode, ioop)) { 		// see if this is something we should abort
      *liopex = iopex -> next;							// if so, unlink from queue
      if (devex -> iopexqh == NULL) devex -> iopexqt = &(devex -> iopexqh);
      iopex -> next = xiopex;
      xiopex = iopex;
    } else {
      liopex = &(iopex -> next);
    }
  }
  devex -> iopexqt = liopex;							// set up possibly new tail pointer

  /* If wildcard scan in progress, tell it is aborted */

  iopex = chnex -> wild_iopex;
  if ((iopex != NULL) && oz_knl_ioabortok (iopex -> ioop, iochan, procmode, ioop)) iopex -> aborted = 1;

  oz_hw_smplock_clr (&(devex -> smplock_vl), vl);				// queue is clean, release lock

  /* Remove any matching requests from the corresponding file's recio request queue */

  memset (&temp_iopex, 0xF0, sizeof temp_iopex);
  temp_iopex.shortcut_prev = NULL;
  if (startshortcut (chnex, devex, &temp_iopex) == OZ_SUCCESS) {		// prevent file from closing on us
    file = chnex -> file;							// see if any file is open on channel
    if (file != NULL) {
      vl = oz_hw_smplock_wait (&(file -> recio_vl));				// ok, lock its recio request queue
      for (liopex = &(file -> recio_qh); (iopex = *liopex) != NULL;) {		// scan the queue
        if (oz_knl_ioabortok (iopex -> ioop, iochan, procmode, ioop)) {		// see if it should be aborted
          *liopex = iopex -> next;						// if so, unlink it
          if (devex -> iopexqh == NULL) devex -> iopexqt = &(devex -> iopexqh);
          iopex -> next = xiopex;						// link to abort list
          xiopex = iopex;
        } else {
          liopex = &(iopex -> next);						// if not, skip over it
        }
      }
      file -> recio_qt = liopex;						// fix queue tail pointer
      oz_hw_smplock_clr (&(file -> recio_vl), vl);				// unlock queue
    }
    finishortcut (chnex, devex, &temp_iopex);					// let file be closed
  }

  /* Now that we're back at softint level, abort all the requests we found */

  while ((iopex = xiopex) != NULL) {
    xiopex = iopex -> next;
    iodonex (iopex, OZ_ABORTED, NULL, NULL);
  }
}

typedef struct { uLong size;
                 OZ_Dbn svbn;
                 OZ_Handle hout;
               } Ixdeb;

#define IXDEB_FC 0x99

void oz_ixdeb_user (OZ_Handle h_iochan, uLong size, OZ_Dbn svbn, OZ_Handle h_output)

{
  Ixdeb ixdeb;
  uLong sts;

  oz_sys_io_fs_printf (h_output, "oz_ixdeb_user (%X, %u, %u)\n", h_iochan, size, svbn);
  ixdeb.size = size;
  ixdeb.svbn = svbn;
  ixdeb.hout = h_output;
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_iochan, 0, IXDEB_FC, sizeof ixdeb, &ixdeb);
  oz_sys_io_fs_printf (h_output, "oz_ixdeb_user status %u\n", sts);
}

/************************************************************************/
/*									*/
/*  This routine is called as a result of an oz_knl_iostart call to 	*/
/*  start performing an i/o request					*/
/*									*/
/************************************************************************/

static uLong oz_disk_fs_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode,
                               OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap)

{
  char threadname[OZ_THREAD_NAMESIZE];
  Chnex *chnex;
  Devex *devex;
  Iopex *iopex;
  OZ_Thread *thread;
  uLong sts, vl;

  chnex = chnexv;
  devex = devexv;
  iopex = iopexv;

  /* Maybe device is being shut down */

  if (devex -> shutdown) return (OZ_SYSHUTDOWN);

  /* Set up stuff in iopex that is common to just about all functions */

  iopex -> ioop     = ioop;
  iopex -> funcode  = funcode;
  iopex -> chnex    = chnex;
  iopex -> devex    = devex;
  iopex -> procmode = procmode;
  iopex -> as       = as;
  iopex -> ap       = ap;
  iopex -> aborted  = 0;
#if 111
  iopex -> shortcut_next = iopex;	// it's not linked to chnex -> shortcut_iopexs yet
  iopex -> shortcut_prev = NULL;
#endif

  movc4 (as, ap, sizeof iopex -> u, &(iopex -> u));

  sts = OZ_SUCCESS;

  switch (funcode) {

    /* Initialize volume - start a kernel thread - the thread stays around just long enough to init the volume */

    case OZ_IO_FS_INITVOL: {
      sts = oz_knl_ioop_lockz (iopex -> ioop, -1, iopex -> u.initvol.p.devname, NULL, NULL, NULL, NULL);
      if (sts == OZ_SUCCESS) sts = oz_knl_ioop_lockz (iopex -> ioop, -1, iopex -> u.initvol.p.volname, NULL, NULL, NULL, NULL);
      if (sts == OZ_SUCCESS) sts = oz_knl_ioop_lockr (iopex -> ioop, iopex -> u.initvol.p.secattrsize, iopex -> u.initvol.p.secattrbuff, NULL, NULL, NULL);
      if (sts != OZ_SUCCESS) return (sts);
      oz_sys_sprintf (sizeof threadname, threadname, "disk_fs init %s", iopex -> u.initvol.p.devname);
      sts = oz_knl_thread_create (oz_s_systemproc, THREAD_PRIORITY, NULL, NULL, NULL, 0, 
                                  kt_initvol, iopex, OZ_ASTMODE_INHIBIT, sizeof threadname, threadname, NULL, &thread);
      if (sts == OZ_SUCCESS) {
        oz_knl_thread_orphan (thread);
        oz_knl_thread_increfc (thread, -1);
        sts = OZ_STARTED;
      }
      return (sts);
    }

    /* Mount volume - start a kernel thread - the thread stays around until the volume is dismounted */

    case OZ_IO_FS_MOUNTVOL: {
      sts = oz_knl_ioop_lockz (iopex -> ioop, -1, iopex -> u.mountvol.p.devname, NULL, NULL, NULL, NULL);
      if (sts != OZ_SUCCESS) return (sts);
      oz_sys_sprintf (sizeof threadname, threadname, "disk_fs %s", iopex -> u.mountvol.p.devname);
      sts = oz_knl_thread_create (oz_s_systemproc, THREAD_PRIORITY, NULL, NULL, NULL, 0, 
                                  kt_mountvol, iopex, OZ_ASTMODE_INHIBIT, sizeof threadname, threadname, NULL, &thread);
      if (sts == OZ_SUCCESS) {
        oz_knl_thread_orphan (thread);
        oz_knl_thread_increfc (thread, -1);
        sts = OZ_STARTED;
      }
      return (sts);
    }

    /* These requests are primarily processed in the calling thread's context.  This */
    /* is ok because they do not interact with anything else in the filesystem,      */
    /* other than to require that a file have already been opened on the channel.    */

    case OZ_IO_FS_WRITEBLOCKS: {
      sts = oz_knl_ioop_lockr (iopex -> ioop, iopex -> u.writeblocks.p.size, iopex -> u.writeblocks.p.buff, 
                               &(iopex -> u.writeblocks.phypages), NULL, &(iopex -> u.writeblocks.phyoffs));
      if (sts == OZ_SUCCESS) sts = sc_writeblocks (iopex, chnex, devex);
      return (sts);
    }

    case OZ_IO_FS_READBLOCKS: {
      sts = oz_knl_ioop_lockw (iopex -> ioop, iopex -> u.readblocks.p.size, iopex -> u.readblocks.p.buff, 
                               &(iopex -> u.readblocks.phypages), NULL, &(iopex -> u.readblocks.phyoffs));
      if (sts == OZ_SUCCESS) sts = sc_readblocks (iopex, chnex, devex);
      return (sts);
    }

    case IXDEB_FC: {
      sts = sc_ixdeb (iopex, chnex, devex);
      return (sts);
    }

    case OZ_IO_FS_PAGEWRITE: {
      const OZ_Mempage *pagearray;
      OZ_Dbn virtblock;
      OZ_Mempage pagecount;

      if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);
      virtblock = iopex -> u.pagewrite.p.startblock;
      pagecount = iopex -> u.pagewrite.p.pagecount;
      pagearray = iopex -> u.pagewrite.p.pagearray;
      memset (&(iopex -> u.writeblocks.p), 0, sizeof iopex -> u.writeblocks.p);
      iopex -> u.writeblocks.p.size   = pagecount << OZ_HW_L2PAGESIZE;
      iopex -> u.writeblocks.p.svbn   = virtblock;
      iopex -> u.writeblocks.phypages = pagearray;
      sts = sc_writeblocks (iopex, chnex, devex);
      return (sts);
    }

    case OZ_IO_FS_PAGEREAD: {
      const OZ_Mempage *pagearray;
      OZ_Dbn virtblock;
      OZ_Mempage pagecount;

      if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);
      virtblock = iopex -> u.pageread.p.startblock;
      pagecount = iopex -> u.pageread.p.pagecount;
      pagearray = iopex -> u.pageread.p.pagearray;
      memset (&(iopex -> u.readblocks.p), 0, sizeof iopex -> u.readblocks.p);
      iopex -> u.readblocks.p.size   = pagecount << OZ_HW_L2PAGESIZE;
      iopex -> u.readblocks.p.svbn   = virtblock;
      iopex -> u.readblocks.phypages = pagearray;
      sts = sc_readblocks (iopex, chnex, devex);
      return (sts);
    }

    /* These requests are processed primarily as 'lowipl' routines not in any particular thread's context. */
    /* There is one of these active per file at a time because they share the end-of-file pointer.         */

    case OZ_IO_FS_WRITEREC: {

      // Lock the data buffer in memory

      sts = oz_knl_ioop_lockr (iopex -> ioop, iopex -> u.writerec.p.size, iopex -> u.writerec.p.buff, 
                               &(iopex -> u.writerec.phypages), NULL, &(iopex -> u.writerec.byteoffs));

      // Lock return length in memory and clear it

      if ((sts == OZ_SUCCESS) && (iopex -> u.writerec.p.wlen != NULL)) {
        sts = oz_knl_ioop_lockw (iopex -> ioop, sizeof *(iopex -> u.writerec.p.wlen), iopex -> u.writerec.p.wlen, 
                                 &(iopex -> u.writerec.wlen_phypages), NULL, &(iopex -> u.writerec.wlen_byteoffs));
        if (sts == OZ_SUCCESS) *(iopex -> u.writerec.p.wlen) = 0;
      }

      // Lock the terminator buffer in memory (or copy small ones to trmdata)

      if (sts == OZ_SUCCESS) {
        if (iopex -> u.writerec.p.trmsize > sizeof iopex -> u.writerec.trmdata) {
          sts = oz_knl_ioop_lockr (iopex -> ioop, iopex -> u.writerec.p.trmsize, iopex -> u.writerec.p.trmbuff, 
                                   &(iopex -> u.writerec.trmphypages), NULL, &(iopex -> u.writerec.trmbyteoffs));
        } else {
          sts = oz_knl_section_uget (procmode, iopex -> u.writerec.p.trmsize, iopex -> u.writerec.p.trmbuff, 
                                     iopex -> u.writerec.trmdata);
        }
      }

      // Queue the request for processing

      if (sts == OZ_SUCCESS) {
        iopex -> backend = sc_writerec;
        sts = queuerecio (iopex, chnex, devex);
      }
      return (sts);
    }

    case OZ_IO_FS_READREC: {

      // Lock the data buffer in memory

      sts = oz_knl_ioop_lockw (iopex -> ioop, iopex -> u.readrec.p.size, iopex -> u.readrec.p.buff, 
                               &(iopex -> u.readrec.phypages), NULL, &(iopex -> u.readrec.byteoffs));

      // Lock return length in memory and clear it

      if ((sts == OZ_SUCCESS) && (iopex -> u.readrec.p.rlen != NULL)) {
        sts = oz_knl_ioop_lockw (iopex -> ioop, sizeof *(iopex -> u.readrec.p.rlen), iopex -> u.readrec.p.rlen, 
                                 &(iopex -> u.readrec.rlen_phypages), NULL, &(iopex -> u.readrec.rlen_byteoffs));
        if (sts == OZ_SUCCESS) *(iopex -> u.readrec.p.rlen) = 0;
      }

      // Copy the terminator buffer in to temp buffer

      iopex -> u.readrec.trmbuff = NULL;
      if ((sts == OZ_SUCCESS) && (iopex -> u.readrec.p.trmsize != 0)) {
        iopex -> u.readrec.trmbuff = iopex -> u.readrec.trmdata;
        if (iopex -> u.readrec.p.trmsize > sizeof iopex -> u.readrec.trmdata) {
          iopex -> u.readrec.trmbuff = OZ_KNL_PGPMALLOQ (iopex -> u.readrec.p.trmsize);
          if (iopex -> u.readrec.trmbuff != NULL) return (OZ_EXQUOTAPGP);
        }
        sts = oz_knl_section_uget (procmode, iopex -> u.readrec.p.trmsize, iopex -> u.readrec.p.trmbuff, iopex -> u.readrec.trmbuff);
        if ((sts != OZ_SUCCESS) && (iopex -> u.readrec.p.trmsize > sizeof iopex -> u.readrec.trmdata)) OZ_KNL_PGPFREE (iopex -> u.readrec.trmbuff);
      }

      // Queue the request for processing

      if (sts == OZ_SUCCESS) {
        iopex -> backend = sc_readrec;
        sts = queuerecio (iopex, chnex, devex);
      }
      return (sts);
    }

    /* All others get queued to the existing kernel thread that was created with a previous mount request. */
    /* If there isn't such a thread, it means the volume is not mounted, so reject the request.            */

    /* First, though, we lock all the buffers in memory.  Do this now */
    /* just in case the pagefaulting requires I/O on this disk drive. */

    case OZ_IO_FS_DISMOUNT: {
      iopex -> backend = be_dismount;
      break;
    }

    case OZ_IO_FS_VERIFYVOL: {
      iopex -> backend = be_verifyvol;
      break;
    }

    case OZ_IO_FS_CREATE: {
      sts = oz_knl_ioop_lockz (iopex -> ioop, -1, iopex -> u.create.p.name, NULL, NULL, NULL, NULL);
      if (sts == OZ_SUCCESS) sts = oz_knl_ioop_lockr (iopex -> ioop, iopex -> u.create.p.secattrsize, iopex -> u.create.p.secattrbuff, NULL, NULL, NULL);
      if (sts == OZ_SUCCESS) sts = oz_knl_ioop_lockw (iopex -> ioop, iopex -> u.create.p.rnamesize, iopex -> u.create.p.rnamebuff, NULL, NULL, NULL);
      if ((sts == OZ_SUCCESS) && (iopex -> u.create.p.rnamesubs != NULL)) {
        sts = oz_knl_ioop_lockw (iopex -> ioop, sizeof *(iopex -> u.create.p.rnamesubs), iopex -> u.create.p.rnamesubs, NULL, NULL, NULL);
      }
      if ((sts == OZ_SUCCESS) && (iopex -> u.create.p.fileidsize != 0)) {
        if (iopex -> u.create.p.fileidsize != devex -> vector -> fileid_size) return (OZ_BADBUFFERSIZE);
        sts = oz_knl_ioop_lockw (iopex -> ioop, iopex -> u.create.p.fileidsize, iopex -> u.create.p.fileidbuff, NULL, NULL, NULL);
      }
      iopex -> backend = be_create;
      break;
    }

    case OZ_IO_FS_OPEN: {
      if ((iopex -> u.open.p.fileidsize != 0) && (iopex -> u.open.p.fileidsize != devex -> vector -> fileid_size)) return (OZ_BADBUFFERSIZE);
      if (iopex -> u.open.p.name != NULL) {
        sts = oz_knl_ioop_lockz (iopex -> ioop, -1, iopex -> u.open.p.name, NULL, NULL, NULL, NULL);
        if ((sts == OZ_SUCCESS) && (iopex -> u.open.p.fileidsize != 0)) {
          sts = oz_knl_ioop_lockw (iopex -> ioop, iopex -> u.open.p.fileidsize, iopex -> u.open.p.fileidbuff, NULL, NULL, NULL);
        }
      }
      else if (iopex -> u.open.p.fileidsize == 0) return (OZ_MISSINGPARAM);
      else sts = oz_knl_ioop_lockr (iopex -> ioop, iopex -> u.open.p.fileidsize, iopex -> u.open.p.fileidbuff, NULL, NULL, NULL);
      if (sts == OZ_SUCCESS) sts = oz_knl_ioop_lockw (iopex -> ioop, iopex -> u.open.p.rnamesize, iopex -> u.open.p.rnamebuff, NULL, NULL, NULL);
      if ((sts == OZ_SUCCESS) && (iopex -> u.open.p.rnamesubs != NULL)) {
        sts = oz_knl_ioop_lockw (iopex -> ioop, sizeof *(iopex -> u.open.p.rnamesubs), iopex -> u.open.p.rnamesubs, NULL, NULL, NULL);
      }
      iopex -> backend = be_open;
      break;
    }

    case OZ_IO_FS_CLOSE: {
      if (chnex -> ignclose) return (OZ_SUCCESS);
      sts = oz_knl_ioop_lockr (iopex -> ioop, iopex -> u.close.p.numitems * sizeof *(iopex -> u.close.p.itemlist), iopex -> u.close.p.itemlist, NULL, NULL, NULL);
      iopex -> backend = be_close;
      break;
    }

    case OZ_IO_FS_ENTER: {
      sts = oz_knl_ioop_lockz (iopex -> ioop, -1, iopex -> u.enter.p.name, NULL, NULL, NULL, NULL);
      if (sts == OZ_SUCCESS) sts = oz_knl_ioop_lockw (iopex -> ioop, iopex -> u.enter.p.rnamesize, iopex -> u.enter.p.rnamebuff, NULL, NULL, NULL);
      if ((sts == OZ_SUCCESS) && (iopex -> u.enter.p.rnamesubs != NULL)) {
        sts = oz_knl_ioop_lockw (iopex -> ioop, sizeof *(iopex -> u.enter.p.rnamesubs), iopex -> u.enter.p.rnamesubs, NULL, NULL, NULL);
      }
      iopex -> backend = be_enter;
      break;
    }

    case OZ_IO_FS_REMOVE: {
      sts = oz_knl_ioop_lockz (iopex -> ioop, -1, iopex -> u.remove.p.name, NULL, NULL, NULL, NULL);
      if (sts == OZ_SUCCESS) sts = oz_knl_ioop_lockw (iopex -> ioop, iopex -> u.remove.p.rnamesize, iopex -> u.remove.p.rnamebuff, NULL, NULL, NULL);
      if ((sts == OZ_SUCCESS) && (iopex -> u.remove.p.rnamesubs != NULL)) {
        sts = oz_knl_ioop_lockw (iopex -> ioop, sizeof *(iopex -> u.remove.p.rnamesubs), iopex -> u.remove.p.rnamesubs, NULL, NULL, NULL);
      }
      iopex -> backend = be_remove;
      break;
    }

    case OZ_IO_FS_RENAME: {
      sts = oz_knl_ioop_lockz (iopex -> ioop, -1, iopex -> u.rename.p.oldname, NULL, NULL, NULL, NULL);
      if (sts == OZ_SUCCESS) sts = oz_knl_ioop_lockz (iopex -> ioop, -1, iopex -> u.rename.p.newname, NULL, NULL, NULL, NULL);
      if (sts == OZ_SUCCESS) sts = oz_knl_ioop_lockw (iopex -> ioop, iopex -> u.rename.p.oldrnamesize, iopex -> u.rename.p.oldrnamebuff, NULL, NULL, NULL);
      if (sts == OZ_SUCCESS) sts = oz_knl_ioop_lockw (iopex -> ioop, iopex -> u.rename.p.newrnamesize, iopex -> u.rename.p.newrnamebuff, NULL, NULL, NULL);
      if ((sts == OZ_SUCCESS) && (iopex -> u.rename.p.oldrnamesubs != NULL)) {
        sts = oz_knl_ioop_lockw (iopex -> ioop, sizeof *(iopex -> u.rename.p.oldrnamesubs), iopex -> u.rename.p.oldrnamesubs, NULL, NULL, NULL);
      }
      if ((sts == OZ_SUCCESS) && (iopex -> u.rename.p.newrnamesubs != NULL)) {
        sts = oz_knl_ioop_lockw (iopex -> ioop, sizeof *(iopex -> u.rename.p.newrnamesubs), iopex -> u.rename.p.newrnamesubs, NULL, NULL, NULL);
      }
      iopex -> backend = be_rename;
      break;
    }

    case OZ_IO_FS_EXTEND: {
      iopex -> backend = be_extend;
      break;
    }

    case OZ_IO_FS_GETINFO1: {
      sts = oz_knl_ioop_lockw (iopex -> ioop, iopex -> as, iopex -> ap, NULL, NULL, NULL);
      if (sts == OZ_SUCCESS) sts = oz_knl_ioop_lockw (iopex -> ioop, iopex -> u.getinfo1.p.fileidsize, iopex -> u.getinfo1.p.fileidbuff, NULL, NULL, NULL);
      if (sts == OZ_SUCCESS) sts = sc_getinfo1 (iopex, chnex, devex);
      return (sts);
    }

    case OZ_IO_FS_READDIR: {
      sts = oz_knl_ioop_lockw (iopex -> ioop, iopex -> u.readdir.p.filenamsize, iopex -> u.readdir.p.filenambuff, NULL, NULL, NULL);
      if (sts == OZ_SUCCESS) sts = oz_knl_ioop_lockw (iopex -> ioop, iopex -> u.readdir.p.fileidsize, iopex -> u.readdir.p.fileidbuff, NULL, NULL, NULL);
      if ((sts == OZ_SUCCESS) && (iopex -> u.readdir.p.filenamsubs != NULL)) {
        sts = oz_knl_ioop_lockw (iopex -> ioop, sizeof *(iopex -> u.readdir.p.filenamsubs), iopex -> u.readdir.p.filenamsubs, NULL, NULL, NULL);
      }
      if (sts == OZ_SUCCESS) sts = sc_readdir (iopex, chnex, devex);
      return (sts);
    }

    case OZ_IO_FS_GETSECATTR: {
      sts = oz_knl_ioop_lockw (iopex -> ioop, iopex -> u.getsecattr.p.size, iopex -> u.getsecattr.p.buff, NULL, NULL, NULL);
      if ((sts == OZ_SUCCESS) && (iopex -> u.getsecattr.p.rlen != NULL)) {
        sts = oz_knl_ioop_lockw (iopex -> ioop, sizeof *(iopex -> u.getsecattr.p.rlen), iopex -> u.getsecattr.p.rlen, NULL, NULL, NULL);
      }
      iopex -> backend = be_getsecattr;
      break;
    }

    case OZ_IO_FS_WRITEBOOT: {
      iopex -> backend = be_writeboot;
      break;
    }

    case OZ_IO_FS_SETCURPOS: {
      iopex -> backend = be_setcurpos;
      break;
    }

    case OZ_IO_FS_WILDSCAN: {
      sts = oz_knl_ioop_lockw (iopex -> ioop, iopex -> u.wildscan.p.size, iopex -> u.wildscan.p.buff, NULL, NULL, NULL);
      if (sts == OZ_SUCCESS) sts = oz_knl_ioop_lockw (iopex -> ioop, iopex -> u.wildscan.p.fileidsize, iopex -> u.wildscan.p.fileidbuff, NULL, NULL, NULL);
      if (sts == OZ_SUCCESS) sts = oz_knl_ioop_lockw (iopex -> ioop, iopex -> u.wildscan.p.wildsize,   iopex -> u.wildscan.p.wildbuff,   NULL, NULL, NULL);
      if ((sts == OZ_SUCCESS) && (iopex -> u.wildscan.p.subs != NULL)) {
        sts = oz_knl_ioop_lockw (iopex -> ioop, sizeof *(iopex -> u.wildscan.p.subs), iopex -> u.wildscan.p.subs, NULL, NULL, NULL);
      }
      if ((sts == OZ_SUCCESS) && (iopex -> u.wildscan.p.wildsubs != NULL)) {
        sts = oz_knl_ioop_lockw (iopex -> ioop, sizeof *(iopex -> u.wildscan.p.wildsubs), iopex -> u.wildscan.p.wildsubs, NULL, NULL, NULL);
      }
      if (sts == OZ_SUCCESS) sts = sc_wildscan (iopex, chnex, devex);
      return (sts);
    }

    case OZ_IO_FS_GETINFO2: {
      sts  = oz_knl_ioop_lockw (iopex -> ioop, iopex -> u.getinfo2.p.filnamsize, iopex -> u.getinfo2.p.filnambuff, NULL, NULL, NULL);
      if ((sts == OZ_SUCCESS) && (iopex -> u.getinfo2.p.filnamsubs != NULL)) {
        sts = oz_knl_ioop_lockw (iopex -> ioop, sizeof *(iopex -> u.getinfo2.p.filnamsubs), iopex -> u.getinfo2.p.filnamsubs, NULL, NULL, NULL);
      }
      iopex -> backend = be_getinfo2;
      break;
    }

    case OZ_IO_FS_GETINFO3: {
      sts = oz_knl_ioop_lockw (iopex -> ioop, iopex -> as, iopex -> ap, NULL, NULL, NULL);
      if ((sts == OZ_SUCCESS) && (iopex -> u.getinfo3.p.underbuff != NULL)) {
        sts = oz_knl_ioop_lockw (iopex -> ioop, iopex -> u.getinfo3.p.undersize, iopex -> u.getinfo3.p.underbuff, NULL, NULL, NULL);
      }
      iopex -> backend = be_getinfo3;
      break;
    }

    case OZ_IO_FS_SHUTDOWN: {
      if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);	/* can only be called from kernel mode */
      devex -> shutdown = 1;					/* don't ever let any more requests queue */
      iopex -> backend  = be_shutdown;				/* queue to mount thread */
      break;
    }

    case OZ_IO_FS_CRASH: {
      if (procmode != OZ_PROCMODE_KNL) return (OZ_KERNELONLY);
      iopex -> backend = be_crash;
      break;
    }

    case OZ_IO_FS_PARSE: {
      sts = oz_knl_ioop_lockz (iopex -> ioop, -1, iopex -> u.parse.p.name, NULL, NULL, NULL, NULL);
      if (sts == OZ_SUCCESS) sts = oz_knl_ioop_lockw (iopex -> ioop, iopex -> u.parse.p.rnamesize, iopex -> u.parse.p.rnamebuff, NULL, NULL, NULL);
      if ((sts == OZ_SUCCESS) && (iopex -> u.parse.p.rnamesubs != NULL)) {
        sts = oz_knl_ioop_lockw (iopex -> ioop, sizeof *(iopex -> u.parse.p.rnamesubs), iopex -> u.parse.p.rnamesubs, NULL, NULL, NULL);
      }
      iopex -> backend = be_parse;
      break;
    }

    /* Don't know what the function is */

    default: {
      sts = OZ_BADIOFUNC;
    }
  }

  /* Now that everything checks out, queue it to the thread */

  if (sts == OZ_SUCCESS) {
    vl = oz_hw_smplock_wait (&(devex -> smplock_vl));	/* lock the volume data */
    if (devex -> event == NULL) sts = OZ_NOTMOUNTED;	/* error status if no thread (ie, it's not mounted) */
    else if ((chnex -> shortcuts < 0) && (iopex -> backend != be_close)) sts = OZ_CLOSEINPROG;
							/* maybe the channel is being closed, ... */
							/* don't let anyone (especially shortcuts) queue anything */
							/* but allow redundant closes in case of deassign routine */
    else if ((chnex -> scassochnex != NULL) && (chnex -> scassochnex -> shortcuts < 0) && (iopex -> backend != be_close)) sts = OZ_CLOSEINPROG;
							/* also don't allow shortcut routines to queue new */
							/* requests on any channel they are using for I/O */
    else {
      iopex -> next = NULL;				/* it's mounted (or is being mounted), queue iopex to thread */
      *(devex -> iopexqt) = iopex;
      devex -> iopexqt = &(iopex -> next);
      if (devex -> iopexqh == iopex) {			/* if queue was empty, wake the thread */
        oz_knl_event_set (devex -> event, 1);
      }
      sts = OZ_STARTED;					/* request will complete asynchronously */
    }
    oz_hw_smplock_clr (&(devex -> smplock_vl), vl);	/* unlock database */
  }

  return (sts);
}

/************************************************************************/
/*									*/
/*  Initialize volume							*/
/*									*/
/************************************************************************/

static uLong kt_initvol (void *iopexv)

{
  Chnex *chnex;
  Devex *devex;
  int si, volnamelen;
  Iopex *iopex, *iopexx;
  OZ_Iochan *iochan;
  OZ_Secattr *secattr;
  uLong sts, vl;

  iopex = iopexv;
  chnex = iopex -> chnex;
  devex = iopex -> devex;
  secattr = NULL;

  si = oz_hw_cpu_setsoftint (0);

  /* Attach to requestor's process to address its parameters */

  oz_knl_process_setcur (oz_knl_ioop_getprocess (iopex -> ioop));

  /* Assign an I/O channel to the disk drive - it will be deassigned when the deassign routine is called */
  /* Allow others to read but no other writing, allow myself to write                                    */
  /* If it is already mounted, this will return an access conflict status                                */

  sts = assign_by_name (iopex -> u.initvol.p.devname, OZ_LOCKMODE_PW, iopex, &iochan);
  if (sts != OZ_SUCCESS) goto rtn;
  if (!oz_hw_atomic_setif_ptr (&(devex -> master_iochan), iochan, NULL)) {
    oz_knl_iochan_increfc (iochan, -1);
    sts = OZ_ALREADYMOUNTED;
    goto rtn;
  }
  oz_knl_event_rename (devex -> ioevent, OZ_EVENT_NAMESIZE, oz_knl_devunit_devname (oz_knl_iochan_getdevunit (iochan)));

  /* Parse the given secattrs */

  sts = getcresecattr (iopex, iopex -> u.initvol.p.secattrsize, iopex -> u.initvol.p.secattrbuff, &secattr);
  if (sts != OZ_SUCCESS) goto rtn;
  sts = oz_knl_secattr_fromname (strlen (iopex -> u.initvol.p.volname) + 1, iopex -> u.initvol.p.volname, &volnamelen, NULL, &secattr);
  if (sts != OZ_SUCCESS) goto rtn;

  /* Write the disk */

  sts = init_volume (volnamelen, iopex -> u.initvol.p.volname, iopex -> u.initvol.p.clusterfactor, 
                     oz_knl_secattr_getsize (secattr), oz_knl_secattr_getbuff (secattr), iopex -> u.initvol.p.initflags, iopex);

  /* Reject any i/o requests that were queued while writing disk */

  vl = oz_hw_smplock_wait (&(devex -> smplock_vl));
  while ((iopexx = devex -> iopexqh) != NULL) {
    devex -> iopexqh = iopexx -> next;
    if (devex -> iopexqh == NULL) devex -> iopexqt = &(devex -> iopexqh);
    oz_hw_smplock_clr (&(devex -> smplock_vl), vl);
    iodonex (iopexx, OZ_NOTMOUNTED, NULL, NULL);
    vl = oz_hw_smplock_wait (&(devex -> smplock_vl));
  }

  /* Close the I/O channel to the disk drive */

  oz_knl_iochan_increfc (devex -> master_iochan, -1);
  devex -> master_iochan = NULL;
  oz_hw_smplock_clr (&(devex -> smplock_vl), vl);

rtn:
  oz_knl_secattr_increfc (secattr, -1);
  iodonex (iopex, sts, NULL, NULL);
  oz_knl_process_setcur (oz_s_systemproc);
  oz_hw_cpu_setsoftint (si);
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Mount volume							*/
/*									*/
/************************************************************************/

static uLong kt_mountvol (void *iopexv)

{
  char unitdesc[OZ_DEVUNIT_DESCSIZE], unitname[OZ_DEVUNIT_NAMESIZE];
  Chnex *chnex;
  Devex *devex;
  int si;
  Iopex *iopex, *iopexx;
  OZ_Devunit *devunit;
  OZ_Event *event;
  OZ_Iochan *iochan;
  OZ_Lockmode lockmode;
  OZ_Secattr *secattr;
  uLong sts, vl;

  iopex = iopexv;
  chnex = iopex -> chnex;
  devex = iopex -> devex;

  si = oz_hw_cpu_setsoftint (0);

  /* Attach to requestor's process to address its parameters */

  oz_knl_process_setcur (oz_knl_ioop_getprocess (iopex -> ioop));

  /* Assign an I/O channel to the disk drive -                   */
  /* Allow others to read disk but no-one else can write         */
  /* If already mounted, this will return access confilct status */

  lockmode = OZ_LOCKMODE_PW;
  if (iopex -> u.mountvol.p.mountflags & OZ_FS_MOUNTFLAG_READONLY) lockmode = OZ_LOCKMODE_PR;
  sts = assign_by_name (iopex -> u.mountvol.p.devname, lockmode, iopex, &iochan);
  if (sts != OZ_SUCCESS) goto done;
  if (!oz_hw_atomic_setif_ptr (&(devex -> master_iochan), iochan, NULL)) {
    oz_knl_iochan_increfc (iochan, -1);
    sts = OZ_ALREADYMOUNTED;
    goto done;
  }
  oz_knl_event_rename (devex -> ioevent, OZ_EVENT_NAMESIZE, oz_knl_devunit_devname (oz_knl_iochan_getdevunit (iochan)));

  /* Declare shutdown handler so it will get dismounted on shutdown */

  devex -> shuthand = oz_knl_shuthand_create (shuthand, devex);

  /* Open the sacred files */

  sts = mount_volume (&(devex -> volume), iopex -> u.mountvol.p.mountflags, iopex);	/* open the sacred files and set up the volume struct */
  if (sts != OZ_SUCCESS) goto done;							/* abort mount request if any error */

  /* Mount successful, rename the unit to something predictable */
  /* Unit name is <disk_unitname>.<template_unitname>           */
  /* Unit description is Volume <volume_name>                   */

  devunit = oz_knl_iochan_getdevunit (chnex -> iochan);
  oz_sys_sprintf (sizeof unitname, unitname, "%s.%s", oz_knl_devunit_devname (oz_knl_iochan_getdevunit (devex -> master_iochan)), devex -> drivername);
  oz_sys_sprintf (sizeof unitdesc, unitdesc, "Volume %s", (*(devex -> vector -> get_volname)) (devex -> volume));
  oz_knl_devunit_rename (devunit, unitname, unitdesc);

  /* If the caller supplied any security attributes, apply them to the device now */

  if (iopex -> u.mountvol.p.secattrsize != 0) {
    sts = oz_knl_secattr_create (iopex -> u.mountvol.p.secattrsize, iopex -> u.mountvol.p.secattrbuff, NULL, &secattr);
    if (sts != OZ_SUCCESS) {
      oz_dev_vdfs_printk (iopex, "error %u creating mount security attributes\n", sts);
      dismount_volume (devex -> volume, 0, 0, iopex);
      devex -> volume = NULL;
      goto done;
    }
    oz_knl_devunit_setsecattr (devunit, secattr);
    oz_knl_secattr_increfc (secattr, -1);
  }

  /* Process I/O requests for the volume.  Eventually we get a dismount request which clears devex -> volume. */

  sts = oz_knl_event_create (sizeof unitname, unitname, NULL, &(devex -> event)); /* create event flag */
  if (sts != OZ_SUCCESS) oz_crash ("oz_dev_ip kt_mountvol: error %u creating event flag", sts); /* so requests will queue */

  while (devex -> volume != NULL) {						/* keep looping while volume is mounted */
    oz_dev_vdfs_write_dirty_headers (devex -> volume, iopex);			/* write dirty headers to disk */
    write_dirty_homeboy (devex -> volume, iopex);				/* write dirty homeblock to disk */
    iodonex (iopex, sts, NULL, NULL);						/* post the last request for completion */
    oz_knl_process_setcur (oz_s_systemproc);					/* detach from reqeusting process address space */
dequeue:
    oz_knl_event_set (devex -> event, 0);					/* clear event flag in case queue is empty */
    vl = oz_hw_smplock_wait (&(devex -> smplock_vl));				/* lock the i/o request queue */
    iopex = devex -> iopexqh;							/* see if there is anything in the queue */
    if (iopex == NULL) {							/* if the queue is empty, */
      oz_hw_smplock_clr (&(devex -> smplock_vl), vl);				/* release the lock */
      oz_knl_event_waitone (devex -> event);					/* wait for something in the queue */
      goto dequeue;								/* go back an check queue again */
    }
    devex -> iopexqh = iopex -> next;						/* queue not empty, unlink the request */
    if (devex -> iopexqh == NULL) devex -> iopexqt = &(devex -> iopexqh);
    oz_hw_smplock_clr (&(devex -> smplock_vl), vl);				/* release the lock */
    oz_knl_process_setcur (oz_knl_ioop_getprocess (iopex -> ioop));		/* attach to requestor process to address parameters */
    sts = (*(iopex -> backend)) (iopex, iopex -> chnex, devex);			/* call the backend (be_...) routine */
  }

  /* Clear any shutdown handler we had set up */

done:
  if (devex -> shuthand != NULL) {
    oz_knl_shuthand_delete (devex -> shuthand);
    devex -> shuthand = NULL;
  }

  /* Don't let any new requeusts queue */

  event = devex -> event;
  devex -> event = NULL;
  if (event != NULL) oz_knl_event_increfc (event, 1);

  /* Reject any I/O requests that were queued while mounting or dismounting disk */

reject:
  vl = oz_hw_smplock_wait (&(devex -> smplock_vl));
  if ((iopexx = devex -> iopexqh) != NULL) {
    devex -> iopexqh = iopexx -> next;
    if (devex -> iopexqh == NULL) devex -> iopexqt = &(devex -> iopexqh);
    oz_hw_smplock_clr (&(devex -> smplock_vl), vl);
    iodonex (iopexx, OZ_NOTMOUNTED, NULL, NULL);
    goto reject;
  }
  oz_hw_smplock_clr (&(devex -> smplock_vl), vl);

  /* Complete the mount/dismount request */

rtn:
  if (devex -> master_iochan != NULL) {
    oz_knl_iochan_increfc (devex -> master_iochan, -1);
    devex -> master_iochan = NULL;
  }
  iodonex (iopex, sts, NULL, NULL);
  oz_knl_process_setcur (oz_s_systemproc);
  oz_hw_cpu_setsoftint (si);
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  The system is being shut down - terminate all activity now		*/
/*									*/
/*    Input:								*/
/*									*/
/*	devex = device to shut down					*/
/*	smplevel = softint						*/
/*									*/
/************************************************************************/

static void shuthand (void *devexv)

{
  Devex *devex;
  uLong sts;
  OZ_Iochan *iochan;

  devex = devexv;

  /* The shutdown handler is no longer queued */

  devex -> shuthand = NULL;

  /* Use an I/O function code so it gets synchronized with the thread */

  sts = oz_knl_iochan_create (devex -> devunit, OZ_LOCKMODE_NL, OZ_PROCMODE_KNL, NULL, &iochan);
  if (sts != OZ_SUCCESS) oz_crash ("oz_disk_fs_shutdown: error %u assigning I/O channel", sts);
  sts = oz_knl_io (iochan, OZ_IO_FS_SHUTDOWN, 0, NULL);
  if ((sts != OZ_SUCCESS) && (sts != OZ_NOTMOUNTED)) {
    oz_crash ("oz_disk_fs_shutdown: error %u shutting down", sts);
  }
  oz_knl_iochan_increfc (iochan, -1);
}

/************************************************************************/
/*									*/
/*  System is being shutdown - close all files and dismount volume	*/
/*									*/
/************************************************************************/

static uLong be_shutdown (Iopex *iopex, Chnex *chnex, Devex *devex)

{
  uLong sts;

  oz_knl_printk ("oz_dev_vdfs: shutting down %s\n", oz_knl_devunit_devname (devex -> devunit));
  sts = dismount_volume (devex -> volume, 0, 1, iopex);
  if (sts == OZ_SUCCESS) devex -> volume = NULL;

  return (sts);
}

/************************************************************************/
/*									*/
/*  Dismount volume							*/
/*									*/
/************************************************************************/

static uLong be_dismount (Iopex *iopex, Chnex *chnex, Devex *devex)

{
  uLong sts;

  sts = dismount_volume (devex -> volume, iopex -> u.dismount.p.unload, 0, iopex);
  if (sts == OZ_SUCCESS) devex -> volume = NULL;

  return (sts);
}

/************************************************************************/
/*									*/
/*  Verify volume							*/
/*									*/
/************************************************************************/

static uLong be_verifyvol (Iopex *iopex, Chnex *chnex, Devex *devex)

{
  return ((*(devex -> vector -> verifyvol)) (iopex, devex));
}

/************************************************************************/
/*									*/
/*  Create file								*/
/*									*/
/************************************************************************/

static uLong be_create (Iopex *iopex, Chnex *chnex, Devex *devex)

{
  char rname[2*OZ_FS_MAXFNLEN];
  const char *fname;
  File *dirfile, *file;
  Fileid dirid, *fileid;
  int fnamelen, rnamelen;
  OZ_Lockmode lockmode;
  OZ_Secattr *secattr;
  uLong sts;

  dirfile = NULL;
  file    = NULL;
  secattr = NULL;

  if (chnex -> file != NULL) sts = OZ_FILEALREADYOPEN;						/* error if something already open on channel */
  else sts = getcresecattr (iopex, iopex -> u.create.p.secattrsize, iopex -> u.create.p.secattrbuff, &secattr); /* get secattrs to create it with */
  if (sts == OZ_SUCCESS) sts = oz_knl_secattr_fromname (strlen (iopex -> u.create.p.name) + 1, iopex -> u.create.p.name, &fnamelen, NULL, &secattr); /* maybe there are secattrs in the name itself */
  if (sts == OZ_SUCCESS) sts = getdirid (devex -> volume, fnamelen, iopex -> u.create.p.name, &dirid, &fnamelen, &fname, rname, iopex); /* get directory id the file is in */
  if (sts == OZ_SUCCESS) sts = oz_dev_vdfs_open_file (devex -> volume, &dirid, OZ_SECACCMSK_WRITE, &dirfile, iopex); /* open the directory (must be able to write it) */
  if (sts == OZ_SUCCESS) {
    sts = create_file (devex -> volume, fnamelen, fname, iopex -> u.create.p.filattrflags, secattr, &dirid, &file, &fileid, iopex); /* create */
    if (sts != OZ_SUCCESS) oz_dev_vdfs_printk (iopex, "oz_dev_vdfs be_create: error %u creating file\n", sts);
  }
  if (sts == OZ_SUCCESS) {
    rnamelen = strlen (rname);
    sts = enter_file (dirfile, rname, fnamelen, fname, iopex -> u.create.p.newversion, file, fileid, rname + rnamelen, iopex); /* enter in directory */
    if ((sts != OZ_SUCCESS) && (sts != OZ_FILEALREADYEXISTS)) {
      rname[rnamelen] = 0;
      oz_dev_vdfs_printk (iopex, "oz_dev_vdfs be_create: error %u entering file %*.*S in %s\n", sts, fnamelen, fnamelen, fname, rname);
    }
  }
  if (dirfile != NULL) oz_dev_vdfs_close_file (dirfile, iopex);					/* close the directory */
  if (sts == OZ_SUCCESS) {
    lockmode = iopex -> u.create.p.lockmode;							/* get channel lock mode */
    chnex -> file     = file;									/* set up file pointer */
    chnex -> lockmode = lockmode;								/* set up channel file open lock mode */
    chnex -> ignclose = iopex -> u.create.p.ignclose;						/* maybe ignore closes */
    if  (OZ_LOCK_ALLOW_TEST (lockmode, OZ_LOCK_ALLOWS_SELF_READ))    file -> refc_read  ++;	/* increment read and/or write counts */
    if  (OZ_LOCK_ALLOW_TEST (lockmode, OZ_LOCK_ALLOWS_SELF_WRITE))   file -> refc_write ++;
    if (!OZ_LOCK_ALLOW_TEST (lockmode, OZ_LOCK_ALLOWS_OTHERS_READ))  file -> deny_read  ++;
    if (!OZ_LOCK_ALLOW_TEST (lockmode, OZ_LOCK_ALLOWS_OTHERS_WRITE)) file -> deny_write ++;
    chnex -> cur_blk = 1;									/* set record I/O context to beg of file */
    chnex -> cur_byt = 0;
    memcpy (iopex -> u.create.p.fileidbuff, (*(devex -> vector -> get_fileid)) (file), iopex -> u.create.p.fileidsize);
    (*(devex -> vector -> returnspec)) (rname, iopex -> u.create.p.rnamesize, iopex -> u.create.p.rnamebuff, iopex -> u.create.p.rnamesubs);
  }
  else if (file != NULL) oz_dev_vdfs_close_file (file, iopex);					/* failed to enter, delete file (its dircount is zero) */
  oz_knl_secattr_increfc (secattr, -1);								/* anyway, we're done with secattrs */

  return (sts);
}

/************************************************************************/
/*									*/
/*  Open file								*/
/*									*/
/************************************************************************/

static uLong be_open (Iopex *iopex, Chnex *chnex, Devex *devex)

{
  char rname[2*OZ_FS_MAXFNLEN];
  const char *fname;
  File *dirfile, *file;
  Fileid dirid, fileid;
  int fnamelen;
  OZ_Lockmode lockmode;
  OZ_Secaccmsk secaccmsk;
  uLong sts;

  dirfile = NULL;
  file    = NULL;

  lockmode  = iopex -> u.create.p.lockmode;							/* get file lock mode */
  secaccmsk = OZ_SECACCMSK_LOOK;								/* determine security access mask */
  if (OZ_LOCK_ALLOW_TEST (lockmode, OZ_LOCK_ALLOWS_SELF_READ))  secaccmsk |= OZ_SECACCMSK_READ;
  if (OZ_LOCK_ALLOW_TEST (lockmode, OZ_LOCK_ALLOWS_SELF_WRITE)) secaccmsk |= OZ_SECACCMSK_WRITE;

  if (chnex -> file != NULL) return (OZ_FILEALREADYOPEN);					/* error if something already open on channel */
  if (iopex -> u.open.p.name != NULL) {								/* if name supplied, open it by name */
    sts = getdirid (devex -> volume, strlen (iopex -> u.open.p.name), iopex -> u.open.p.name, &dirid, &fnamelen, &fname, rname, iopex);	/* get directory id the file is in */
    if (sts == OZ_SUCCESS) {
      sts = oz_dev_vdfs_open_file (devex -> volume, &dirid, OZ_SECACCMSK_LOOK, &dirfile, iopex); /* open the directory (must be able to look in it for a specific file) */
    }
    if (sts == OZ_SUCCESS) {
      sts = lookup_file (dirfile, strlen (fname), fname, &fileid, rname + strlen (rname), iopex); /* lookup the file in the directory */
    }
    if (dirfile != NULL) oz_dev_vdfs_close_file (dirfile, iopex);				/* close the directory */
    if (sts == OZ_SUCCESS) {
      sts = oz_dev_vdfs_open_file (devex -> volume, &fileid, secaccmsk, &file, iopex); /* open the file */
    }
  } else {
    sts = oz_dev_vdfs_open_file (devex -> volume, iopex -> u.open.p.fileidbuff, secaccmsk, &file, iopex); /* open the file by the fileid */
  }
  if (sts == OZ_SUCCESS) {
    if  ((OZ_LOCK_ALLOW_TEST (lockmode, OZ_LOCK_ALLOWS_SELF_READ)    && (file -> deny_read  != 0)) 
     ||  (OZ_LOCK_ALLOW_TEST (lockmode, OZ_LOCK_ALLOWS_SELF_WRITE)   && (file -> deny_write != 0)) 
     || (!OZ_LOCK_ALLOW_TEST (lockmode, OZ_LOCK_ALLOWS_OTHERS_READ)  && (file -> refc_read  != 0)) 
     || (!OZ_LOCK_ALLOW_TEST (lockmode, OZ_LOCK_ALLOWS_OTHERS_WRITE) && (file -> refc_write != 0))) {
      oz_dev_vdfs_close_file (file, iopex);
      sts = OZ_ACCONFLICT;
    } else {
      chnex -> file     = file;									/* set up file pointer */
      chnex -> lockmode = lockmode;								/* set up channel file open lock mode */
      chnex -> ignclose = iopex -> u.open.p.ignclose;						/* maybe ignore closes */
      if  (OZ_LOCK_ALLOW_TEST (lockmode, OZ_LOCK_ALLOWS_SELF_READ))    file -> refc_read  ++;	/* increment read and/or write counts */
      if  (OZ_LOCK_ALLOW_TEST (lockmode, OZ_LOCK_ALLOWS_SELF_WRITE))   file -> refc_write ++;
      if (!OZ_LOCK_ALLOW_TEST (lockmode, OZ_LOCK_ALLOWS_OTHERS_READ))  file -> deny_read  ++;
      if (!OZ_LOCK_ALLOW_TEST (lockmode, OZ_LOCK_ALLOWS_OTHERS_WRITE)) file -> deny_write ++;
      chnex -> cur_blk = 1;									/* set record I/O context to beg of file */
      chnex -> cur_byt = 0;
    }
    if (iopex -> u.open.p.name != NULL) {
      memcpy (iopex -> u.open.p.fileidbuff, &fileid, iopex -> u.open.p.fileidsize);
      (*(devex -> vector -> returnspec)) (rname, iopex -> u.open.p.rnamesize, iopex -> u.open.p.rnamebuff, iopex -> u.open.p.rnamesubs);
    }
  }

  return (sts);
}

/************************************************************************/
/*									*/
/*  Close file, delete it if marked for delete				*/
/*									*/
/************************************************************************/

static Iopex *abort_requests (Iopex **iopex_qh, Iopex ***iopex_qt, Chnex *chnex);

static uLong be_close (Iopex *iopex, Chnex *chnex, Devex *devex)

{
  File *file;
  int waited;
  Iopex *niopex, *xiopex;
  Long shortcuts;
  OZ_Dbn efblk;
  uLong efbyt, sts, vl;
  Wildscan *wildscan;

  sts = OZ_FILENOTOPEN;								// error status to return if nothing open

  /* See how many shortcut requests are in progress and block anything more (shortcut and normal) from queuing */

  xiopex = NULL;
  vl = oz_hw_smplock_wait (&(devex -> smplock_vl));
  validate_shortcuts (chnex);
  chnex -> closeshorts = shortcuts = oz_hw_atomic_set_long (&(chnex -> shortcuts), -1);

  /* Abort any requests in the thread's queue for this channel.  This is    */
  /* in case some shortcut routine queued a request before we blocked them. */

  xiopex = abort_requests (&(devex -> iopexqh), &(devex -> iopexqt), chnex);
  oz_hw_smplock_clr (&(devex -> smplock_vl), vl);				// get back to softint level
  while ((niopex = xiopex) != NULL) {						// abort the requests
    xiopex = niopex -> next;
    iodonex (niopex, OZ_CLOSEINPROG, NULL, NULL);
  }

  /* Remove all requests from the queuerecio queue for this channel.  Just in case there is some request ahead of them   */
  /* on a different channel that will queue a normal request (like an extend) and thus our requests will never complete. */

  file = chnex -> file;
  if (file != NULL) {
    vl = oz_hw_smplock_wait (&(file -> recio_vl));				// lock the file's record-io request queue
    xiopex = abort_requests (&(file -> recio_qh), &(file -> recio_qt), chnex);	// find requests to abort
    oz_hw_smplock_clr (&(file -> recio_vl), vl);				// unlock the request queue
    while ((niopex = xiopex) != NULL) {						// abort the requests
      xiopex = niopex -> next;
      iodonex (niopex, OZ_CLOSEINPROG, NULL, NULL);
      finishortcut (chnex, devex, niopex);
    }
  }


  /* Make sure all shortcut requests have finished.  We already blocked any more from starting. */
  /* Note that queuerecio requests count as shortcuts, so this will wait for them, too.         */

  waited = 0;
  vl = oz_hw_smplock_wait (&(devex -> smplock_vl));
  while (1) {									// see if any shortcuts are still going
#if 111
    for (xiopex = chnex -> shortcut_iopexs; xiopex != NULL; xiopex = xiopex -> shortcut_next) {
      oz_knl_printk ("oz_dev_vdfs be_close*: chnex %p -> shortcut_iopex %p -> backend %p\n", chnex, xiopex, xiopex -> backend);
    }
#endif
    if (chnex -> shortcuts + shortcuts == -1) break;
    oz_knl_printk ("oz_dev_vdfs be_close*: chnex %p -> shortcuts %d + shortcuts %d > -1\n", chnex, chnex -> shortcuts, shortcuts);
    oz_hw_smplock_clr (&(devex -> smplock_vl), vl);
    oz_knl_event_waitone (devex -> shortcutev);					// if so, wait for them to finish
    oz_knl_event_set (devex -> shortcutev, 0);
    vl = oz_hw_smplock_wait (&(devex -> smplock_vl));
    waited = 1;
  }
  if (waited) oz_knl_printk ("oz_dev_vdfs be_close*: resuming\n");
  oz_hw_smplock_clr (&(devex -> smplock_vl), vl);
										// if shortcuts was 3, chnex->shortcuts is now -4
										// ... indicating that all 3 have completed

  /* Now close any file that might be open on channel */

  if (file != NULL) {								// see if anything open
    if (OZ_LOCK_ALLOW_TEST (chnex -> lockmode, OZ_LOCK_ALLOWS_SELF_WRITE)) {	// maybe set some file attributes
      if (iopex -> u.close.p.numitems != 0) {
        sts = set_file_attrs (file, iopex -> u.close.p.numitems, iopex -> u.close.p.itemlist, iopex);
      }
    }
    if  (OZ_LOCK_ALLOW_TEST (chnex -> lockmode, OZ_LOCK_ALLOWS_SELF_READ))    file -> refc_read  --;
    if  (OZ_LOCK_ALLOW_TEST (chnex -> lockmode, OZ_LOCK_ALLOWS_SELF_WRITE))   file -> refc_write --;
    if (!OZ_LOCK_ALLOW_TEST (chnex -> lockmode, OZ_LOCK_ALLOWS_OTHERS_READ))  file -> deny_read  --;
    if (!OZ_LOCK_ALLOW_TEST (chnex -> lockmode, OZ_LOCK_ALLOWS_OTHERS_WRITE)) file -> deny_write --;
    sts = oz_dev_vdfs_close_file (file, iopex);					// close file
    chnex -> file = NULL;							// mark channel closed
  }

  /* Close any wildcard search context that might be open on channel                               */
  /* We know wildcard scanning is not in progress because we have locked out all shortcut routines */

  while ((wildscan = chnex -> wildscan) != NULL) {
    (*(devex -> vector -> wildscan_terminate)) (chnex);				// close out fs-dependent context
    wildscan_unlink (wildscan);							// make sure wildscan is unlinked from dirfile->wildscans list
    chnex -> wildscan = wildscan -> nextouter;					// save pointer to next outer level
    ((Chnex *)oz_knl_iochan_ex (wildscan -> iochan)) -> scassochnex = NULL;	// we won't be queuing any more I/O's to the channel
    oz_knl_iochan_increfc (wildscan -> iochan, -1);				// close the directory file
    OZ_KNL_PGPFREE (wildscan);							// free off the struct
    if (sts == OZ_FILENOTOPEN) sts = OZ_SUCCESS;
  }

  /* Close is complete.  Set shortcuts back to zero to allow new requests to start on channel. */

  OZ_HW_MB;									// make sure chnex->file=NULL, etc, seen first
  chnex -> shortcuts = 0;							// let stuff go wild again

  /* Also clear wild_iopex so wildcard scans can be done */

  vl = oz_hw_smplock_wait (&(devex -> smplock_vl));
  chnex -> wild_iopex = NULL;
  oz_hw_smplock_clr (&(devex -> smplock_vl), vl);

  return (sts);
}

/* Find requests in the queue that match the given channel and remove them for aborting              */
/* Do likewise for any requests sitting in the queue that are for shortcut processing on the channel */

static Iopex *abort_requests (Iopex **iopex_qh, Iopex ***iopex_qt, Chnex *chnex)

{
  Iopex **liopex, *niopex, *xiopex;

  xiopex = NULL;
  for (liopex = iopex_qh; (niopex = *liopex) != NULL;) {			// scan the request queue
    if ((niopex -> chnex != chnex) && (niopex -> chnex -> scassochnex != chnex)) liopex = &(niopex -> next); // leave request in queue if different channel
    else {
      *liopex = niopex -> next;							// same channel, unlink request from queue
      niopex -> next = xiopex;							// link it to list of requests to be aborted
      xiopex = niopex;
    }
  }
  *iopex_qt = liopex;								// the tail of the queue might be different now
  return (xiopex);								// return the list of requests to abort
}

/************************************************************************/
/*									*/
/*  Enter file alias name						*/
/*									*/
/************************************************************************/

static uLong be_enter (Iopex *iopex, Chnex *chnex, Devex *devex)

{
  char rname[2*OZ_FS_MAXFNLEN];
  const char *fname;
  File *dirfile;
  Fileid dirid, fileid;
  int fnamelen;
  uLong sts;

  dirfile = NULL;

  if (chnex -> file == NULL) sts = OZ_FILENOTOPEN;						/* make sure we have an open file */
  else sts = getdirid (devex -> volume, strlen (iopex -> u.enter.p.name), iopex -> u.enter.p.name, &dirid, &fnamelen, &fname, rname, iopex); /* get directory id the new filename is in */
  if (sts == OZ_SUCCESS) sts = oz_dev_vdfs_open_file (devex -> volume, &dirid, OZ_SECACCMSK_WRITE, &dirfile, iopex); /* open the directory (must be able to write it) */
  if (sts == OZ_SUCCESS) {
    sts = lookup_file (dirfile, fnamelen, fname, &fileid, NULL, iopex);				/* lookup the new filename in the directory */
    if (sts == OZ_SUCCESS) sts = OZ_FILEALREADYEXISTS;
    else if (sts == OZ_NOSUCHFILE) sts = OZ_SUCCESS;
  }
  if (sts == OZ_SUCCESS) sts = enter_file (dirfile, rname, fnamelen, fname, iopex -> u.enter.p.newversion, chnex -> file, (*(devex -> vector -> get_fileid)) (chnex -> file), rname + strlen (rname), iopex); /* enter in directory */
  if (sts == OZ_SUCCESS) (*(devex -> vector -> returnspec)) (rname, iopex -> u.enter.p.rnamesize, iopex -> u.enter.p.rnamebuff, iopex -> u.enter.p.rnamesubs);
  if (dirfile != NULL) oz_dev_vdfs_close_file (dirfile, iopex);					/* close the directory */

  return (sts);
}

/************************************************************************/
/*									*/
/*  Remove file name from directory.  If last entry, delete file.	*/
/*									*/
/************************************************************************/

static uLong be_remove (Iopex *iopex, Chnex *chnex, Devex *devex)

{
  char rname[2*OZ_FS_MAXFNLEN];
  const char *fname;
  File *dirfile;
  Fileid dirid;
  int fnamelen;
  uLong sts;

  dirfile = NULL;

  sts = getdirid (devex -> volume, strlen (iopex -> u.remove.p.name), iopex -> u.remove.p.name, &dirid, &fnamelen, &fname, rname, iopex); /* get directory id the filename is in */
  if (sts == OZ_SUCCESS) sts = oz_dev_vdfs_open_file (devex -> volume, &dirid, OZ_SECACCMSK_WRITE, &dirfile, iopex); /* open the directory (must be able to write it) */
  if (sts == OZ_SUCCESS) sts = remove_file (dirfile, fname, rname + strlen (rname), iopex);	/* remove the filename from the directory */
  if (sts == OZ_SUCCESS) (*(devex -> vector -> returnspec)) (rname, iopex -> u.remove.p.rnamesize, iopex -> u.remove.p.rnamebuff, iopex -> u.remove.p.rnamesubs);
  if (dirfile != NULL) oz_dev_vdfs_close_file (dirfile, iopex);					/* close the directory */

  return (sts);
}

/************************************************************************/
/*									*/
/*  Rename file								*/
/*									*/
/************************************************************************/

static uLong be_rename (Iopex *iopex, Chnex *chnex, Devex *devex)

{
  char newrname[2*OZ_FS_MAXFNLEN], oldrname[2*OZ_FS_MAXFNLEN];
  const char *newfname, *oldfname;
  File *file, *newdirfile, *olddirfile;
  Fileid fileid, newdirid, olddirid;
  int newfnamelen, oldfnamelen;
  uLong sts;

  file = NULL;
  newdirfile = NULL;
  olddirfile = NULL;

  sts = getdirid (devex -> volume, strlen (iopex -> u.rename.p.oldname), iopex -> u.rename.p.oldname, &olddirid, &oldfnamelen, &oldfname, oldrname, iopex); /* get directory the old filename is in */
  if (sts == OZ_SUCCESS) sts = oz_dev_vdfs_open_file (devex -> volume, &olddirid, OZ_SECACCMSK_WRITE, &olddirfile, iopex); /* open the directory the old filename is in (must be able to write it) */
  if (sts == OZ_SUCCESS) sts = lookup_file (olddirfile, oldfnamelen, oldfname, &fileid, oldrname + strlen (oldrname), iopex); /* lookup the old filename to get the fileid */
  if (sts == OZ_SUCCESS) sts = oz_dev_vdfs_open_file (devex -> volume, &fileid, 0, &file, iopex); /* open the file being renamed */

  if (sts == OZ_SUCCESS) sts = getdirid (devex -> volume, strlen (iopex -> u.rename.p.newname), iopex -> u.rename.p.newname, &newdirid, &newfnamelen, &newfname, newrname, iopex); /* get directory id the new filename goes in */
  if (sts == OZ_SUCCESS) sts = oz_dev_vdfs_open_file (devex -> volume, &newdirid, OZ_SECACCMSK_WRITE, &newdirfile, iopex); /* open the directory the new filename goes in (must be able to write it) */
  if (sts == OZ_SUCCESS) sts = enter_file (newdirfile, newrname, newfnamelen, newfname, iopex -> u.rename.p.newversion, NULL, &fileid, newrname + strlen (newrname), iopex); /* enter new name in new directory */
  if (sts == OZ_SUCCESS) sts = remove_file (olddirfile, oldfname, NULL, iopex);			/* remove old name from old directory */

  if (sts == OZ_SUCCESS) {
    (*(devex -> vector -> returnspec)) (oldrname, iopex -> u.rename.p.oldrnamesize, iopex -> u.rename.p.oldrnamebuff, iopex -> u.rename.p.oldrnamesubs);
    (*(devex -> vector -> returnspec)) (newrname, iopex -> u.rename.p.newrnamesize, iopex -> u.rename.p.newrnamebuff, iopex -> u.rename.p.newrnamesubs);
  }

  if (olddirfile != NULL) oz_dev_vdfs_close_file (olddirfile, iopex);				/* close the old directory */
  if (newdirfile != NULL) oz_dev_vdfs_close_file (newdirfile, iopex);				/* close the new directory */
  if (file != NULL) oz_dev_vdfs_close_file (file, iopex);					/* close the file */

  return (sts);											/* return composite status */
}

/************************************************************************/
/*									*/
/*  Extend file								*/
/*									*/
/************************************************************************/

static uLong be_extend (Iopex *iopex, Chnex *chnex, Devex *devex)

{
  File *file;
  uLong efbyt, sts, vl;
  OZ_Dbn efblk;

  file = chnex -> file;
  sts = OZ_FILENOTOPEN;
  if (file != NULL) {
    if (IS_DIRECTORY (file)) sts = OZ_FILEISADIR;
    else if (!OZ_LOCK_ALLOW_TEST (chnex -> lockmode, OZ_LOCK_ALLOWS_SELF_WRITE)) sts = OZ_NOWRITEACCESS;
    else if (iopex -> u.extend.p.nblocks < file -> allocblocks) {
      if (!(iopex -> u.extend.p.extflags & OZ_FS_EXTFLAG_NOTRUNC)) {
        file -> truncpend = 1;
        file -> truncblocks = iopex -> u.extend.p.nblocks;
      }
      return (OZ_SUCCESS);
    }
    else sts = extend_file (file, iopex -> u.extend.p.nblocks, iopex -> u.extend.p.extflags, iopex);
    if ((sts == OZ_SUCCESS) && ((efblk = iopex -> u.extend.p.eofblock) != 0)) {
      efbyt  = iopex -> u.extend.p.eofbyte;
      efblk += efbyt / devex -> blocksize;
      efbyt %= devex -> blocksize;
      vl = oz_hw_smplock_wait (&(file -> attrlock_vl));
      file -> attrlock_efblk  = efblk;
      file -> attrlock_efbyt  = efbyt;
      file -> attrlock_flags |= OZ_VDFS_ALF_M_EOF;
      oz_hw_smplock_clr (&(file -> attrlock_vl), vl);
      oz_dev_vdfs_mark_header_dirty (file);
    }
  }

  return (sts);
}

/************************************************************************/
/*									*/
/*  Write blocks							*/
/*									*/
/*  This request takes a shortcut around the queue and is processed 	*/
/*  immediately upon issue.  Ideally, it will just need to copy the 	*/
/*  user's buffer to cache blocks and return synchronous completion.	*/
/*  Otherwise, it may have to start a disk read then complete later.	*/
/*									*/
/*    Input:								*/
/*									*/
/*	iopex = I/O request extension					*/
/*	chnex = corresponding I/O channel extension			*/
/*	devex = corresponding device extension				*/
/*	smplevel = softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	sc_writeblocks = OZ_STARTED : request queued, will complete 	*/
/*	                              asynchronously			*/
/*	                       else : completion status			*/
/*									*/
/************************************************************************/

static uLong dc_writeblocks (OZ_Dcmpb *dcmpb, uLong status);
static uLong wt_writeblocks (OZ_Dcmpb *dcmpb, uLong status);

static uLong sc_writeblocks (Iopex *iopex, Chnex *chnex, Devex *devex)

{
  File *file;
  uLong sts;

  // Increment channel's shortcut count, to prevent a close from being queued.
  // But if the count is already negative, that means a close has already been queued on the channel.

  sts = startshortcut (chnex, devex, iopex);
  if (sts != OZ_SUCCESS) return (sts);

  // Ok, closes are blocked, make sure the write can proceed

  sts = OZ_FILENOTOPEN;						// the channel has to have a file open on it
  file = chnex -> file;
  if (file == NULL) goto rtnsts;
  sts = OZ_FILEISADIR;						// don't allow writing to a directory
  if (IS_DIRECTORY (file)) goto rtnsts;
  sts = OZ_NOWRITEACCESS;					// and we have to have write access to file
  if (!OZ_LOCK_ALLOW_TEST (chnex -> lockmode, OZ_LOCK_ALLOWS_SELF_WRITE)) goto rtnsts;

  // Maybe volume or file attributes force write-thru mode

  if ((*(iopex -> devex -> vector -> vis_writethru)) (file -> volume) || (*(iopex -> devex -> vector -> fis_writethru)) (file, iopex -> u.writeblocks.p.svbn, iopex -> u.writeblocks.p.offs, iopex -> u.writeblocks.p.size, NULL)) iopex -> u.writeblocks.p.writethru = 1;

  // Start processing via cache

  iopex -> u.writeblocks.dcmpb.writing   = 1;					// disk write operation
  iopex -> u.writeblocks.dcmpb.virtblock = iopex -> u.writeblocks.p.svbn;	// starting virtual block number
  iopex -> u.writeblocks.dcmpb.nbytes    = iopex -> u.writeblocks.p.size;	// number of bytes to be written to disk
  iopex -> u.writeblocks.dcmpb.blockoffs = iopex -> u.writeblocks.p.offs;	// byte offset in starting virtual block
  iopex -> u.writeblocks.dcmpb.entry     = dc_writeblocks;			// call this routine when cache blocks ready
  iopex -> u.writeblocks.dcmpb.param     = iopex;
  iopex -> u.writeblocks.dcmpb.writethru = iopex -> u.writeblocks.p.writethru;	// set up writethru mode flag
  iopex -> u.writeblocks.dcmpb.ix4kbuk   = 0;
  iopex -> u.writeblocks.status          = OZ_SUCCESS;				// in case nbytes is zero

  sts = oz_dev_vdfs_dcache_map_vbn_to_lbn (&(iopex -> u.writeblocks.dcmpb), chnex -> file);	// map virtual-to-logical block number
  if (sts == OZ_SUCCESS) sts = oz_dev_vdfs_dcache_map_blocks (&(iopex -> u.writeblocks.dcmpb)); // process the request
  if (sts != OZ_STARTED) dc_writeblocks (&(iopex -> u.writeblocks.dcmpb), sts);
  return (OZ_STARTED);

rtnsts:
  finishortcut (iopex -> chnex, iopex -> devex, iopex);
  return (sts);
}

/************************************************************************/
/*									*/
/*  The disk cache system calls this routine when it has access to the 	*/
/*  cache blocks for the disk						*/
/*									*/
/************************************************************************/

static uLong dc_writeblocks (OZ_Dcmpb *dcmpb, uLong status)

{
  Chnex *chnex;
  const OZ_Mempage *phypages;
  File *file;
  Iopex *iopex;
  uLong size, skip, vl;

  iopex = dcmpb -> param;
  chnex = iopex -> chnex;
  file  = chnex -> file;
  size  = 0;

  /* Maybe the write request is complete now */

  if (status != OZ_PENDING) {
    if (status == OZ_SUCCESS) status = iopex -> u.writeblocks.status;
    vl = oz_hw_smplock_wait (&(file -> attrlock_vl));
    file -> attrlock_date   = oz_hw_tod_getnow ();
    file -> attrlock_flags |= OZ_VDFS_ALF_M_ADT | OZ_VDFS_ALF_M_CDT | OZ_VDFS_ALF_M_MDT;
    oz_hw_smplock_clr (&(file -> attrlock_vl), vl);
    finishortcut (chnex, iopex -> devex, iopex);
    iodonex (iopex, status, NULL, NULL);
  }

  /* Copy to cache page from user buffer and increment parameters for next transfer                         */
  /* If new nbytes is zero, means we're done, and cache system will call us back when it's ok to free dcmpb */

  else {
    skip = (dcmpb -> virtblock - iopex -> u.writeblocks.p.svbn) * iopex -> devex -> blocksize 
         + (dcmpb -> blockoffs - iopex -> u.writeblocks.p.offs);
    size = iopex -> u.writeblocks.p.size - skip;
    if (size > dcmpb -> nbytes) size = dcmpb -> nbytes;
    oz_hw_phys_movephys (size, 
                         iopex -> u.writeblocks.phypages, 
                         skip + iopex -> u.writeblocks.phyoffs, 
                         &(dcmpb -> phypage), 
                         dcmpb -> pageoffs);
    skip += size;
    dcmpb -> virtblock = iopex -> u.writeblocks.p.svbn;
    dcmpb -> nbytes    = iopex -> u.writeblocks.p.size - skip;
    dcmpb -> blockoffs = iopex -> u.writeblocks.p.offs + skip;
    iopex -> u.writeblocks.status = oz_dev_vdfs_dcache_map_vbn_to_lbn (dcmpb, file);
    if (iopex -> u.writeblocks.status != OZ_SUCCESS) dcmpb -> nbytes = 0;
  }

  /* Return number of bytes we modified */

  return (size);
}

/************************************************************************/
/*									*/
/*  Read blocks								*/
/*									*/
/*  This request takes a shortcut around the queue and is processed 	*/
/*  immediately upon issue.  Ideally, it will just need to copy the 	*/
/*  user's buffer from cache blocks and return synchronous completion.	*/
/*  Otherwise, it may have to start a disk read then complete later.	*/
/*									*/
/*    Input:								*/
/*									*/
/*	iopex = I/O request extension					*/
/*	chnex = corresponding I/O channel extension			*/
/*	devex = corresponding device extension				*/
/*	smplevel = softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	sc_readblocks = OZ_STARTED : request queued, will complete 	*/
/*	                             asynchronously			*/
/*	                      else : completion status			*/
/*									*/
/************************************************************************/

static uLong dc_readblocks (OZ_Dcmpb *dcmpb, uLong status);
static uLong nc_readblocks_start (Iopex *iopex);
static void nc_readblocks_done (void *iopexv, uLong status);

static uLong sc_readblocks (Iopex *iopex, Chnex *chnex, Devex *devex)

{
  uLong sts;

  // Increment channel's shortcut count, to prevent a close from being queued.
  // But if the count is already negative, that means a close has already been queued on the channel.

  sts = startshortcut (chnex, devex, iopex);
  if (sts != OZ_SUCCESS) return (sts);

  // Ok, closes are blocked, make sure the read can proceed

  sts = OZ_FILENOTOPEN;						// the channel has to have a file open on it
  if (chnex -> file == NULL) goto rtnsts;
  sts = OZ_NOREADACCESS;					// and we have to have read access to file
  if (!OZ_LOCK_ALLOW_TEST (chnex -> lockmode, OZ_LOCK_ALLOWS_SELF_READ)) goto rtnsts;

  // Start processing via cache

  if (devex -> dcache != NULL) {
    iopex -> u.readblocks.dcmpb.writing   = 0;					// disk read operation
    iopex -> u.readblocks.dcmpb.virtblock = iopex -> u.readblocks.p.svbn;	// starting virtual block number
    iopex -> u.readblocks.dcmpb.nbytes    = iopex -> u.readblocks.p.size;	// number of bytes to be written to disk
    iopex -> u.readblocks.dcmpb.blockoffs = iopex -> u.readblocks.p.offs;	// byte offset in starting virtual block
    iopex -> u.readblocks.dcmpb.entry     = dc_readblocks;			// call this routine when cache blocks ready
    iopex -> u.readblocks.dcmpb.param     = iopex;
    iopex -> u.readblocks.dcmpb.ix4kbuk   = iopex -> u.readblocks.p.ix4kbuk;
    iopex -> u.readblocks.status          = OZ_SUCCESS;				// in case nbytes is zero
    sts = oz_dev_vdfs_dcache_map_vbn_to_lbn (&(iopex -> u.readblocks.dcmpb), chnex -> file); // map virtual-to-logical block number
    if (sts == OZ_SUCCESS) sts = oz_dev_vdfs_dcache_map_blocks (&(iopex -> u.readblocks.dcmpb)); // process request
    if (sts != OZ_STARTED) dc_readblocks (&(iopex -> u.readblocks.dcmpb), sts);
    return (OZ_STARTED);
  }

  // No cache, start processing 'manually'

  iopex -> u.readblocks.temp = NULL;				// haven't allocated a temp buffer yet
  sts = nc_readblocks_start (iopex);				// start reading directly from disk into caller's buffer
  if (sts != OZ_STARTED) nc_readblocks_done (iopex, sts);	// if sync compl, call compl routine so it can clean up
  return (OZ_STARTED);

rtnsts:
  finishortcut (chnex, devex, iopex);
  return (sts);
}

/************************************************************************/
/*									*/
/*  The disk cache system calls this routine when it has access to the 	*/
/*  cache blocks for the disk						*/
/*									*/
/************************************************************************/

static uLong dc_readblocks (OZ_Dcmpb *dcmpb, uLong status)

{
  Chnex *chnex;
  const OZ_Mempage *phypages;
  Devex *devex;
  File *file;
  Iopex *iopex;
  OZ_Dbn prefetch;
  uLong size, skip, vl;

  iopex = dcmpb -> param;
  chnex = iopex -> chnex;
  devex = iopex -> devex;
  file  = chnex -> file;

  /* Maybe the read request is complete now */

  if (status != OZ_PENDING) {
    int ix4kbuk;

    if (status == OZ_SUCCESS) status = iopex -> u.readblocks.status;
    if (!(file -> volume -> mountflags & OZ_FS_MOUNTFLAG_READONLY)) {
      vl = oz_hw_smplock_wait (&(file -> attrlock_vl));
      file -> attrlock_date   = oz_hw_tod_getnow ();
      file -> attrlock_flags |= OZ_VDFS_ALF_M_ADT;
      oz_hw_smplock_clr (&(file -> attrlock_vl), vl);
    }
#if 000
    prefetch = 0;
    if (status == OZ_SUCCESS) {
      dcmpb -> nbytes = 1;
      if ((ix4kbuk = dcmpb -> ix4kbuk) != 0) dcmpb -> nbytes = 4096;
      if (oz_dev_vdfs_dcache_map_vbn_to_lbn (dcmpb, chnex -> file) == OZ_SUCCESS) prefetch = dcmpb -> logblock;
    }
#endif
    finishortcut (chnex, devex, iopex);
    iodonex (iopex, status, NULL, NULL);
#if 000
    if (prefetch != 0) oz_knl_dcache_prefetch (devex -> dcache, prefetch, ix4kbuk);
#endif
  }

  /* Copy from cache page to user buffer and increment parameters for next transfer                         */
  /* If new nbytes is zero, means we're done, and cache system will call us back when it's ok to free dcmpb */

  else {
    skip = (dcmpb -> virtblock - iopex -> u.readblocks.p.svbn) * devex -> blocksize 
         + (dcmpb -> blockoffs - iopex -> u.readblocks.p.offs);
    size = iopex -> u.readblocks.p.size - skip;
    if (size > dcmpb -> nbytes) size = dcmpb -> nbytes;

    if (iopex -> u.readblocks.p.ix4kbuk) {
      if (size != 4096) oz_knl_printk ("oz_dev_vdfs dc_readblocks %d: size %u\n", __LINE__, size);
      else ix4kbuk_validate_phypage (&(dcmpb -> phypage), dcmpb -> pageoffs, __FILE__, __LINE__);
    }

    oz_hw_phys_movephys (size, 
                         &(dcmpb -> phypage), 
                         dcmpb -> pageoffs, 
                         iopex -> u.readblocks.phypages, 
                         skip + iopex -> u.readblocks.phyoffs);

    if (iopex -> u.readblocks.p.ix4kbuk) {
      if (size != 4096) oz_knl_printk ("oz_dev_vdfs dc_readblocks %d: size %u\n", __LINE__, size);
      else ix4kbuk_validate_phypage (iopex -> u.readblocks.phypages, skip + iopex -> u.readblocks.phyoffs, __FILE__, __LINE__);
    }

    skip += size;
    dcmpb -> virtblock = iopex -> u.readblocks.p.svbn;
    dcmpb -> nbytes    = iopex -> u.readblocks.p.size - skip;
    dcmpb -> blockoffs = iopex -> u.readblocks.p.offs + skip;
    iopex -> u.readblocks.status = oz_dev_vdfs_dcache_map_vbn_to_lbn (dcmpb, file);
    if (iopex -> u.readblocks.status != OZ_SUCCESS) dcmpb -> nbytes = 0;
  }

  /* Return number of bytes we modified */

  return (0);
}

/************************************************************************/
/*									*/
/*  No cache, start reading blocks directly from disk			*/
/*									*/
/*    Input:								*/
/*									*/
/*	iopex -> u.readblocks.p.size = size of transfer			*/
/*	                        svbn = starting virt block number	*/
/*	                        offs = offset in svbn block		*/
/*	                     .phypages = physical page array pointer	*/
/*	                     .phyoffs  = offset in first physical page	*/
/*									*/
/*    Output:								*/
/*									*/
/*	nc_readblocks_start = OZ_STARTED : will complete asyncly	*/
/*	                            else : error status			*/
/*									*/
/************************************************************************/

static uLong nc_readblocks_start (Iopex *iopex)

{
  Chnex *chnex;
  Devex *devex;
  OZ_Dbn logblock, nblocks;
  OZ_IO_disk_readblocks disk_readblocks;
  OZ_IO_disk_readpages disk_readpages;
  uLong sts;

  chnex = iopex -> chnex;
  devex = iopex -> devex;

  /* Normalize starting virtual block number */

  iopex -> u.readblocks.p.svbn += iopex -> u.readblocks.p.offs / devex -> blocksize;
  iopex -> u.readblocks.p.offs %= devex -> blocksize;

  /* Normalize starting physical page number */

  iopex -> u.readblocks.phypages += iopex -> u.readblocks.phyoffs >> OZ_HW_L2PAGESIZE;
  iopex -> u.readblocks.phyoffs  %= 1 << OZ_HW_L2PAGESIZE;

  /* Convert starting virtual block number to logical block number */

  sts = (*(devex -> vector -> map_vbn_to_lbn)) (chnex -> file, iopex -> u.readblocks.p.svbn, &nblocks, &logblock);
  if (sts != OZ_SUCCESS) return (sts);

  /* Disk can only do block sized transfers to properly aligned memory buffers */
  /* If request doesn't meet those criteria, use a temp buffer                 */

  if ((iopex -> u.readblocks.p.offs != 0) 					// must start at beginning of disk block
   || (iopex -> u.readblocks.phyoffs & devex -> bufalign) 			// must be long/quad/whatever aligned
   || ((iopex -> u.readblocks.p.size % devex -> blocksize) != 0)) {		// must be an exact number of blocks
    if (iopex -> u.readblocks.temp == NULL) {					// no, malloc a block-sized buffer
      iopex -> u.readblocks.temp = OZ_KNL_NPPMALLOQ (devex -> blocksize);
      if (iopex -> u.readblocks.temp == NULL) return (OZ_EXQUOTANPP);
    }
    iopex -> u.readblocks.nblocks = 1;						// we just read one block
    memset (&disk_readblocks, 0, sizeof disk_readblocks);
    disk_readblocks.size = devex -> blocksize;
    disk_readblocks.buff = iopex -> u.readblocks.temp;
    disk_readblocks.slbn = logblock;
    sts = oz_knl_iostart3 (1, NULL, devex -> master_iochan, OZ_PROCMODE_KNL, 	// start reading into temp buffer
                           nc_readblocks_done, iopex, NULL, NULL, NULL, NULL, 
                           OZ_IO_DISK_READBLOCKS, sizeof disk_readblocks, &disk_readblocks);
  } else {
    if (iopex -> u.readblocks.temp != NULL) {					// ok, free temp buffer so nc_readblocks_done won't try to copy from it
      OZ_KNL_NPPFREE (iopex -> u.readblocks.temp);
      iopex -> u.readblocks.temp = NULL;
    }
    if (nblocks > iopex -> u.readblocks.p.size / devex -> blocksize) {		// don't read more than caller wants
      nblocks = iopex -> u.readblocks.p.size / devex -> blocksize;
    }
    iopex -> u.readblocks.nblocks = nblocks;					// this is now much we're reading
    memset (&disk_readpages, 0, sizeof disk_readpages);
    disk_readpages.size   = nblocks * devex -> blocksize;
    disk_readpages.pages  = iopex -> u.readblocks.phypages;
    disk_readpages.offset = iopex -> u.readblocks.phyoffs;
    disk_readpages.slbn   = logblock;
    sts = oz_knl_iostart3 (1, NULL, devex -> master_iochan, OZ_PROCMODE_KNL, 	// start reading directly into caller's buffer
                           nc_readblocks_done, iopex, NULL, NULL, NULL, NULL, 
                           OZ_IO_DISK_READPAGES, sizeof disk_readpages, &disk_readpages);
  }
  return (sts);
}

/************************************************************************/
/*									*/
/*  A direct read of the disk has completed (there is no cache active 	*/
/*  on the disk)							*/
/*									*/
/*    Input:								*/
/*									*/
/*	iopex -> u.readblocks.p.size = size remaining (incl this one)	*/
/*	iopex -> u.readblocks.p.svbn = vbn just read in			*/
/*	iopex -> u.readblocks.nblocks = number of blocks just read	*/
/*	iopex -> u.readblocks.temp = NULL : read into phypages		*/
/*	                             else : read into this temp buff	*/
/*	iopex -> u.readblocks.phypages = where data was read into	*/
/*	iopex -> u.readblocks.phyoffs  = where data was read into	*/
/*	status = disk read status					*/
/*									*/
/*    Output:								*/
/*									*/
/*	I/O completion posted or another disk read started		*/
/*									*/
/************************************************************************/

static void nc_readblocks_done (void *iopexv, uLong status)

{
  Devex *devex;
  Iopex *iopex;
  uLong size, sts;

  iopex = iopexv;
  devex = iopex -> devex;

  /* Abort if read error */

  sts = status;
  while (sts == OZ_SUCCESS) {

    /* Get length read.  If using temp buffer, it is the size of the temp buffer.  Else, it shouldn't be more than caller wants. */

    size = iopex -> u.readblocks.nblocks * devex -> blocksize;

    /* If read into temp buffer, copy to caller's buffer */

    if (iopex -> u.readblocks.temp != NULL) {
      size -= iopex -> u.readblocks.p.offs;							// we ignore this much at beg of temp buffer
      if (size > iopex -> u.readblocks.p.size) size = iopex -> u.readblocks.p.size;		// don't copy back more than caller wants
      oz_hw_phys_movefromvirt (size, iopex -> u.readblocks.temp + iopex -> u.readblocks.p.offs, // copy from temp buffer to caller's buffer
                               iopex -> u.readblocks.phypages, iopex -> u.readblocks.phyoffs);
    }

    /* Maybe we're all done */

    iopex -> u.readblocks.p.size -= size;
    if (iopex -> u.readblocks.p.size == 0) break;

    /* If not, increment and start another read */

    iopex -> u.readblocks.p.offs  += size;
    iopex -> u.readblocks.phyoffs += size;
    sts = nc_readblocks_start (iopex);

    /* Repeat if successful synchronous completion */
  }

  /* If we're not waiting for another read, request is complete */

  if (sts != OZ_STARTED) {
    if (iopex -> u.readblocks.temp != NULL) OZ_KNL_NPPFREE (iopex -> u.readblocks.temp);
    finishortcut (iopex -> chnex, devex, iopex);
    iodonex (iopex, sts, NULL, NULL);
  }
}

/************************************************************************/
/*									*/
/*  IX DEBUG routine							*/
/*									*/
/*  Dump out stats about cache blocks pertaining to the given size and 	*/
/*  starting virtual block number					*/
/*									*/
/*    Input:								*/
/*									*/
/*	iopex = I/O request extension					*/
/*	chnex = corresponding I/O channel extension			*/
/*	devex = corresponding device extension				*/
/*	smplevel = softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	sc_ixdeb = completion status					*/
/*									*/
/************************************************************************/

static uLong sc_ixdeb (Iopex *iopex, Chnex *chnex, Devex *devex)

{
  File *file;
  Ixdeb *ixdeb;
  OZ_Dbn logblock, nblocks, svbn;
  OZ_Handle h_output;
  OZ_IO_disk_readblocks disk_readblocks;
  uByte *blockbuff;
  uLong blocksize, size, sts;

  // Increment channel's shortcut count, to prevent a close from being queued.
  // But if the count is already negative, that means a close has already been queued on the channel.

  sts = startshortcut (chnex, devex, iopex);
  if (sts != OZ_SUCCESS) return (sts);

  // Ok, closes are blocked, make sure the read can proceed

  sts = OZ_FILENOTOPEN;						// the channel has to have a file open on it
  file = chnex -> file;
  if (file == NULL) goto rtnsts;
  sts = OZ_NOREADACCESS;					// and we have to have read access to file
  if (!OZ_LOCK_ALLOW_TEST (chnex -> lockmode, OZ_LOCK_ALLOWS_SELF_READ)) goto rtnsts;

  // Make sure cache is active

  sts = 999;
  if (devex -> dcache == NULL) goto rtnsts;

  // Get parameters of part of file to dump stats for

  ixdeb = (Ixdeb *)&(iopex -> u);

  size     = ixdeb -> size;
  svbn     = ixdeb -> svbn;
  h_output = ixdeb -> hout;

  // Dump cache stats for those blocks

  blocksize = devex -> blocksize;

  while (size > 0) {
    sts = (*(devex -> vector -> map_vbn_to_lbn)) (file, svbn, &nblocks, &logblock);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (h_output, "oz_dev_vdfs sc_ixdeb*: error %u mapping vbn %u\n", sts, svbn);
      goto rtnsts;
    }
    if (nblocks > (size + blocksize - 1) / blocksize) nblocks = (size + blocksize - 1) / blocksize;
    oz_sys_io_fs_printf (h_output, "oz_dev_vdfs sc_ixdeb*: vbn %u -> %u @ %u\n", svbn, nblocks, logblock);
    nblocks = oz_knl_dcache_ixdeb (devex -> dcache, nblocks, logblock, h_output);
    if (nblocks == 0) {
      oz_sys_io_fs_printf (h_output, "oz_dev_vdfs sc_ixdeb*: oz_knl_dcache_ixdeb returned 0\n");
      break;
    }
    size -= nblocks * blocksize;
    svbn += nblocks;
  }

  // Dump out the logical blocks right from the hard drive

  size = ixdeb -> size;
  svbn = ixdeb -> svbn;
  blockbuff = OZ_KNL_PGPMALLOC (blocksize);

  memset (&disk_readblocks, 0, sizeof disk_readblocks);
  disk_readblocks.size = blocksize;
  disk_readblocks.buff = blockbuff;

  while (size > 0) {
    sts = (*(devex -> vector -> map_vbn_to_lbn)) (file, svbn, &nblocks, &logblock);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (h_output, "oz_dev_vdfs sc_ixdeb*: error %u mapping vbn %u\n", sts, svbn);
      goto rtnsts;
    }
    oz_sys_io_fs_printf (h_output, "oz_dev_vdfs sc_ixdeb*: vbn %u -> lbn %u:\n", svbn, logblock);
    disk_readblocks.slbn = logblock;
    sts = oz_knl_io (devex -> master_iochan, OZ_IO_DISK_READBLOCKS, sizeof disk_readblocks, &disk_readblocks);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (h_output, "oz_dev_vdfs sc_ixdeb*: error %u reading lbn %u\n", sts, logblock);
      goto rtnsts;
    }
    oz_sys_io_fs_dumpmem (h_output, blocksize, blockbuff);
    size -= blocksize;
    svbn ++;
  }

  OZ_KNL_PGPFREE (blockbuff);

  sts = OZ_SUCCESS;

rtnsts:
  finishortcut (chnex, devex, iopex);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Validate an IX database 4K bucket					*/
/*									*/
/************************************************************************/

int ix4kbuk_validate_phypage (const OZ_Mempage *phypages, uLong phyoffs, const char *file, int line)

{
  int i;
  OZ_Pagentry savepte;
  uLong cksm, cksm0, cksmx, *vaddr;

  // Normalize page offset

  phypages += phyoffs >> 12;
  phyoffs  &= 0xFFF;

  // Map first part of buffer to virtual memory

  vaddr = oz_hw_phys_mappage (*phypages, &savepte);
  (OZ_Pointer)vaddr += phyoffs;

  // Faster for normal case of longword alignment

  if ((phyoffs & 3) == 0) {
    cksm0 = *vaddr;
    cksm  = 0;
    for (i = 1; i < 1024; i ++) {
      ++ vaddr;
      if ((((OZ_Pointer)vaddr) & 0xFFF) == 0) {
        vaddr = oz_hw_phys_mappage (*(++ phypages), NULL);
      }
      cksm += *vaddr;
    }
  }

  // Slower for unaligned buffers

  else {
    for (i = 0; i < 1024; i ++) {
      if ((((OZ_Pointer)vaddr) & 0xFFF) > 0xFFC) {
        memcpy (&cksmx, vaddr, 4 - (phyoffs & 3));
        vaddr = oz_hw_phys_mappage (*(++ phypages), NULL);
        memcpy (((uByte *)&cksmx) + 4 - (phyoffs & 3), vaddr, phyoffs & 3);
        (OZ_Pointer)vaddr += phyoffs & 3;
      } else {
        cksmx = *(vaddr ++);
      }
      if (i == 0) {
        cksm0 = cksmx;
        cksm  = 0;
      } else {
        cksm += cksmx;
      }
    }
  }
  oz_hw_phys_unmappage (savepte);
  if (cksm != cksm0) oz_knl_printk ("%s %d: checksum %8.8X calculated %8.8X\n", file, line, cksm0, cksm);
  return (cksm == cksm0);
}

/************************************************************************/
/*									*/
/*  Write record							*/
/*									*/
/*  This is a shortcut routine, but there is only one request active 	*/
/*  per file at a time, as they go through file -> recio_q.		*/
/*									*/
/************************************************************************/

static void sc_writerec_extended (void *iopexv, uLong status);
static uLong dc_writerec (OZ_Dcmpb *dcmpb, uLong status);

static uLong sc_writerec (Iopex *iopex, Chnex *chnex, Devex *devex)

{
  const OZ_VDFS_Vector *vector;
  File *file;
  OZ_Dbn efblk, ewblk;
  OZ_IO_fs_extend fs_extend;
  uLong blocksize, efbyt, ewbyt, sts, vl;

  blocksize = devex -> blocksize;
  vector    = devex -> vector;

  /* Make sure we have write access to the file */

  file = chnex -> file;
  sts  = OZ_NOWRITEACCESS;
  if (!OZ_LOCK_ALLOW_TEST (chnex -> lockmode, OZ_LOCK_ALLOWS_SELF_WRITE)) goto rtnsts;

  /* If atblock specified, position there */

restart:
  if (iopex -> u.writerec.p.atblock != 0) {
    chnex -> cur_blk = iopex -> u.writerec.p.atblock;
    chnex -> cur_byt = iopex -> u.writerec.p.atbyte;
  }
  chnex -> cur_blk += chnex -> cur_byt / blocksize;
  chnex -> cur_byt %= blocksize;

  /* Maybe position to or set the end-of-file marker for the file */

  iopex -> u.writerec.updateof = 0;					// so far, we don't update the eof position

  if (iopex -> u.writerec.p.append) {					// if we're appending, position to the end-of-file
    vl = oz_hw_smplock_wait (&(file -> attrlock_vl));
    chnex -> cur_blk = file -> attrlock_efblk;
    chnex -> cur_byt = file -> attrlock_efbyt;
    oz_hw_smplock_clr (&(file -> attrlock_vl), vl);
  }

  ewblk  = chnex -> cur_blk;						// calculate where our write will end
  ewbyt  = chnex -> cur_byt;
  ewbyt += iopex -> u.writerec.p.size + iopex -> u.writerec.p.trmsize;
  ewblk += ewbyt / blocksize;
  ewbyt %= blocksize;

  if (ewbyt == 0) ewblk --;
  if (ewblk > file -> allocblocks) {					// see if write will go beyond end of allocated space
    memset (&fs_extend, 0, sizeof fs_extend);				// if so, extend file
    fs_extend.nblocks = ewblk;
    sts = oz_knl_iostart3 (1, NULL, chnex -> iochan, OZ_PROCMODE_KNL, sc_writerec_extended, iopex, 
                           NULL, NULL, NULL, NULL, OZ_IO_FS_EXTEND, sizeof fs_extend, &fs_extend);
    if (sts == OZ_SUCCESS) goto restart;				// ... then start over
    return (sts);
  }

  // Maybe volume or file attributes force write-thru mode

  if ((*(vector -> vis_writethru)) (file -> volume) || (*(vector -> fis_writethru)) (file, chnex -> cur_blk, chnex -> cur_byt, iopex -> u.writerec.p.size + iopex -> u.writerec.p.trmsize, NULL)) iopex -> u.writerec.p.writethru = 1;

  /* Write as much as we can to current position from caller's buffer */

  iopex -> u.writerec.wlen    = 0;					// nothing written from user's buffer yet
  iopex -> u.writerec.trmwlen = 0;					// nothing written from terminator yet

  iopex -> u.writerec.dcmpb.writing   = 1;				// this is a disk write operation
  iopex -> u.writerec.dcmpb.nbytes    = iopex -> u.writerec.p.size + iopex -> u.writerec.p.trmsize;
  iopex -> u.writerec.dcmpb.virtblock = chnex -> cur_blk;		// virtual block we want to start writing at
  iopex -> u.writerec.dcmpb.blockoffs = chnex -> cur_byt;		// byte we want to start writing at
  iopex -> u.writerec.dcmpb.entry     = dc_writerec;			// write routine entrypoint
  iopex -> u.writerec.dcmpb.param     = iopex;				// write routine parameter
  iopex -> u.writerec.dcmpb.writethru = iopex -> u.writerec.p.writethru; // set up writethru mode flag
  iopex -> u.writerec.dcmpb.ix4kbuk   = 0;
  iopex -> u.writerec.status          = OZ_SUCCESS;			// in case of zero bytes

  sts = oz_dev_vdfs_dcache_map_vbn_to_lbn (&(iopex -> u.writerec.dcmpb), file);	// map virtual-to-logical block
  if (sts == OZ_SUCCESS) sts = oz_dev_vdfs_dcache_map_blocks (&(iopex -> u.writerec.dcmpb)); // process the request
  if (sts != OZ_STARTED) dc_writerec (&(iopex -> u.writerec.dcmpb), sts);
  return (OZ_STARTED);

rtnsts:
  return (sts);
}

/* The file has been exteded sufficiently to permit the data to be written. */
/* If the extend failed, terminate the request.  Else, restart the request. */

static void sc_writerec_extended (void *iopexv, uLong status)

{
  Iopex *iopex;

  iopex = iopexv;
  if (status != OZ_SUCCESS) finishrecio (iopex, status, NULL);
  else {
    status = sc_writerec (iopex, iopex -> chnex, iopex -> devex);
    if (status != OZ_STARTED) finishrecio (iopex, status, NULL);
  }
}

/************************************************************************/
/*									*/
/*  The disk cache system calls this routine when it has access to the 	*/
/*  cache blocks for the disk						*/
/*									*/
/************************************************************************/

static uLong dc_writerec (OZ_Dcmpb *dcmpb, uLong status)

{
  Chnex *chnex;
  const OZ_Mempage *phypages;
  File *file;
  Iopex *iopex;
  Long alf;
  uLong blocksize, modified, skip, size, vl;

  iopex = dcmpb -> param;
  chnex = iopex -> chnex;
  file  = chnex -> file;
  modified = 0;

  /* Maybe we are all finished up */

  if (status != OZ_PENDING) {
    if (status == OZ_SUCCESS) status = iopex -> u.writerec.status;

    /* Ok, update the current and end-of-file pointers */

    if (status == OZ_SUCCESS) {
      blocksize = iopex -> devex -> blocksize;
      chnex -> cur_byt += iopex -> u.writerec.p.size + iopex -> u.writerec.p.trmsize;
      chnex -> cur_blk += chnex -> cur_byt / blocksize;
      chnex -> cur_byt %= blocksize;
      alf = OZ_VDFS_ALF_M_MDT | OZ_VDFS_ALF_M_CDT | OZ_VDFS_ALF_M_ADT;
      vl  = oz_hw_smplock_wait (&(file -> attrlock_vl));
      if (iopex -> u.writerec.p.truncate || (chnex -> cur_blk > file -> attrlock_efblk) || ((chnex -> cur_blk == file -> attrlock_efblk) && (chnex -> cur_byt > file -> attrlock_efbyt))) {
        file -> attrlock_efblk = chnex -> cur_blk;
        file -> attrlock_efbyt = chnex -> cur_byt;
        alf |= OZ_VDFS_ALF_M_EOF;
      }
      file -> attrlock_date   = oz_hw_tod_getnow ();
      file -> attrlock_flags |= alf;
      oz_hw_smplock_clr (&(file -> attrlock_vl), vl);
      if (alf & OZ_VDFS_ALF_M_EOF) oz_dev_vdfs_mark_header_dirty (file);
    }

    /* Copy the written length back to user's buffer */

    if (iopex -> u.writerec.p.wlen != NULL) oz_hw_phys_movefromvirt (sizeof *(iopex -> u.writerec.p.wlen), 
                                                                    &(iopex -> u.writerec.wlen), 
                                                                    iopex -> u.writerec.wlen_phypages, 
                                                                    iopex -> u.writerec.wlen_byteoffs);

    /* Post I/O request's completion and maybe start another recio on the file */

    finishrecio (iopex, status, NULL);
    return (0);
  }

  /* Copy unwritten data bytes from user's buffer to cache page */

  size = iopex -> u.writerec.p.size - iopex -> u.writerec.wlen;		// this is how much of the user buffer we have yet to put
  if (size > dcmpb -> nbytes) size = dcmpb -> nbytes;			// can't put more than cache is making available
  if (size > 0) {
    oz_hw_phys_movephys (size, iopex -> u.writerec.phypages, iopex -> u.writerec.byteoffs + iopex -> u.writerec.wlen, &(dcmpb -> phypage), dcmpb -> pageoffs);
    iopex -> u.writerec.wlen += size;					// that much more has been written to cache
    dcmpb -> nbytes   -= size;						// remove from what cache has made available to us
    dcmpb -> pageoffs += size;
    modified = size;
  }

  /* Copy unwritten terminator bytes to cache page */

  size = iopex -> u.writerec.p.trmsize - iopex -> u.writerec.trmwlen;	// this is how much of the terminator buffer we have yet to put
  if (size > dcmpb -> nbytes) size = dcmpb -> nbytes;			// can't put more than cache is making available
  if (size > 0) {
    if (iopex -> u.writerec.p.trmsize > sizeof iopex -> u.writerec.trmdata) {
      oz_hw_phys_movephys (size, iopex -> u.writerec.trmphypages,  iopex -> u.writerec.trmbyteoffs + iopex -> u.writerec.trmwlen, &(dcmpb -> phypage), dcmpb -> pageoffs);
    } else {
      oz_hw_phys_movefromvirt (size, iopex -> u.writerec.trmdata + iopex -> u.writerec.trmwlen, &(dcmpb -> phypage), dcmpb -> pageoffs);
    }
    iopex -> u.writerec.trmwlen += size;				// that much more has been written to cache
    dcmpb -> nbytes   -= size;						// remove from what cache has made available to us
    dcmpb -> pageoffs += size;
    modified += size;
  }

  /* Get more from cache if we haven't written it all yet.  Else set nbytes=0 so oz_knl_dcache_map will know we're all done. */

  size = iopex -> u.writerec.p.size + iopex -> u.writerec.p.trmsize;
  skip = iopex -> u.writerec.wlen   + iopex -> u.writerec.trmwlen;

  iopex -> u.writerec.dcmpb.nbytes    = size - skip;
  iopex -> u.writerec.dcmpb.virtblock = chnex -> cur_blk;
  iopex -> u.writerec.dcmpb.blockoffs = chnex -> cur_byt + skip;
  iopex -> u.writerec.status = oz_dev_vdfs_dcache_map_vbn_to_lbn (dcmpb, file);
  if (iopex -> u.writerec.status != OZ_SUCCESS) dcmpb -> nbytes = 0;

  /* Return number of bytes we modified in the cache page */

  return (modified);
}

/************************************************************************/
/*									*/
/*  Read record								*/
/*									*/
/*  This is a shortcut routine, but there is only one request active 	*/
/*  per file at a time, as they go through file -> recio_q.		*/
/*									*/
/************************************************************************/

static uLong dc_readrec (OZ_Dcmpb *dcmpb, uLong status);

static uLong sc_readrec (Iopex *iopex, Chnex *chnex, Devex *devex)

{
  File *file;
  uLong blocksize, dcsize, sts, vl;

  /* Make sure we have read access to the file */

  file = chnex -> file;
  sts  = OZ_NOREADACCESS;
  if (!OZ_LOCK_ALLOW_TEST (chnex -> lockmode, OZ_LOCK_ALLOWS_SELF_READ)) goto rtnerr;

  /* If atblock specified, position there */

  blocksize = devex -> blocksize;
  if (iopex -> u.readrec.p.atblock != 0) {
    chnex -> cur_blk = iopex -> u.readrec.p.atblock;
    chnex -> cur_byt = iopex -> u.readrec.p.atbyte;
  }
  chnex -> cur_blk += chnex -> cur_byt / blocksize;
  chnex -> cur_byt %= blocksize;

  /* If we're at or past the eof, return eof status */

  vl = oz_hw_smplock_wait (&(file -> attrlock_vl));
  iopex -> u.readrec.efblk = file -> attrlock_efblk;
  iopex -> u.readrec.efbyt = file -> attrlock_efbyt;
  oz_hw_smplock_clr (&(file -> attrlock_vl), vl);

  sts = OZ_ENDOFFILE;
  if (chnex -> cur_blk > iopex -> u.readrec.efblk) goto rtnerr;
  if ((chnex -> cur_blk == iopex -> u.readrec.efblk) && (chnex -> cur_byt >= iopex -> u.readrec.efbyt)) goto rtnerr;

  /* Read as much as we can from current position into caller's buffer */

  iopex -> u.readrec.rlen    = 0;					// nothing read into user's buffer yet
  iopex -> u.readrec.trmseen = 0;					// haven't seen any of the terminator yet

  iopex -> u.readrec.dcmpb.writing   = 0;				// this is a disk read operation
  iopex -> u.readrec.dcmpb.nbytes    = iopex -> u.readrec.p.size + iopex -> u.readrec.p.trmsize;
  iopex -> u.readrec.dcmpb.virtblock = chnex -> cur_blk;		// virtual block we want to start reading at
  iopex -> u.readrec.dcmpb.blockoffs = chnex -> cur_byt;		// byte we want to start reading at
  iopex -> u.readrec.dcmpb.entry     = dc_readrec;			// read routine entrypoint
  iopex -> u.readrec.dcmpb.param     = iopex;				// read routine parameter
  iopex -> u.readrec.dcmpb.ix4kbuk   = 0;
  iopex -> u.readrec.status          = OZ_SUCCESS;			// in case nbytes is zero

  sts = oz_dev_vdfs_dcache_map_vbn_to_lbn (&(iopex -> u.readrec.dcmpb), file);	// map virtual-to-logical block
  if (sts == OZ_SUCCESS) sts = oz_dev_vdfs_dcache_map_blocks (&(iopex -> u.readrec.dcmpb)); // process the request
  if (sts != OZ_STARTED) dc_readrec (&(iopex -> u.readrec.dcmpb), sts);
  return (OZ_STARTED);

rtnerr:
  if (iopex -> u.readrec.p.trmsize > sizeof iopex -> u.readrec.trmdata) OZ_KNL_PGPFREE (iopex -> u.readrec.trmbuff);
  return (sts);
}

/************************************************************************/
/*									*/
/*  The disk cache system calls this routine when it has access to the 	*/
/*  cache blocks for the disk						*/
/*									*/
/************************************************************************/

static uLong dc_readrec (OZ_Dcmpb *dcmpb, uLong status)

{
  Chnex *chnex;
  const OZ_Mempage *phypages;
  File *file;
  Iopex *iopex;
  OZ_Dbn efblk, epblk, prefetch;
  OZ_Pagentry savepte;
  uByte *dcbuff, *p;
  uLong blocksize, efbyt, epbyt, skip, size, sts, vl;

  iopex = dcmpb -> param;
  chnex = iopex -> chnex;
  file  = chnex -> file;
  blocksize = iopex -> devex -> blocksize;

  /* Maybe oz_knl_dcache_map is all done with dcmpb.  If so, post request completion. */

  if (status != OZ_PENDING) {
    if (status == OZ_SUCCESS) status = iopex -> u.readrec.status;
    chnex -> cur_byt += iopex -> u.readrec.rlen + iopex -> u.readrec.trmseen;
    chnex -> cur_blk += chnex -> cur_byt / blocksize;
    chnex -> cur_byt %= blocksize;

    if (iopex -> u.readrec.p.trmsize > sizeof iopex -> u.readrec.trmdata) OZ_KNL_PGPFREE (iopex -> u.readrec.trmbuff);

    if (!(file -> volume -> mountflags & OZ_FS_MOUNTFLAG_READONLY)) {
      vl = oz_hw_smplock_wait (&(file -> attrlock_vl));
      file -> attrlock_date   = oz_hw_tod_getnow ();
      file -> attrlock_flags |= OZ_VDFS_ALF_M_ADT;
      oz_hw_smplock_clr (&(file -> attrlock_vl), vl);
    }

    if (iopex -> u.readrec.p.rlen != NULL) oz_hw_phys_movefromvirt (sizeof *(iopex -> u.readrec.p.rlen), 
                                                                    &(iopex -> u.readrec.rlen), 
                                                                    iopex -> u.readrec.rlen_phypages, 
                                                                    iopex -> u.readrec.rlen_byteoffs);
#if 000
    prefetch = 0;
    if (status == OZ_SUCCESS) {
      dcmpb -> nbytes = 1;
      if (oz_dev_vdfs_dcache_map_vbn_to_lbn (dcmpb, chnex -> file) == OZ_SUCCESS) prefetch = dcmpb -> logblock;
    }
#endif
    finishrecio (iopex, status, NULL);
#if 000
    if (prefetch != 0) oz_knl_dcache_prefetch (iopex -> devex -> dcache, prefetch, 0);
#endif
    return (0);
  }

  /* Chop nbytes off at the end-of-file */

  skip   = iopex -> u.readrec.rlen - iopex -> u.readrec.trmseen;	// calculate where nbytes of the cache would put us
  epblk  = chnex -> cur_blk;
  epbyt  = chnex -> cur_byt + skip + dcmpb -> nbytes;
  epblk += epbyt / blocksize;
  epbyt %= blocksize;
  efblk  = iopex -> u.readrec.efblk;					// get end-of-file pointer
  efbyt  = iopex -> u.readrec.efbyt;
  if ((epblk > efblk) || ((epblk == efblk) && (epbyt > efbyt))) {	// if nbytes goes beyond eof ...
    dcmpb -> nbytes = (efblk - chnex -> cur_blk) * blocksize + efbyt - chnex -> cur_byt - skip; // just read up to the eof
    if (dcmpb -> nbytes == 0) {
      skip     = iopex -> u.readrec.rlen + iopex -> u.readrec.byteoffs;
      phypages = iopex -> u.readrec.phypages;
      if (iopex -> u.readrec.rlen + iopex -> u.readrec.trmseen <= iopex -> u.readrec.p.size) {
        iopex -> u.readrec.rlen += iopex -> u.readrec.trmseen;
        iopex -> u.readrec.status = OZ_ENDOFFILE;			// return end-of-file as there is nothing more to read
      } else {
        iopex -> u.readrec.trmseen = iopex ->  u.readrec.p.size - iopex -> u.readrec.rlen;
        iopex -> u.readrec.rlen = iopex -> u.readrec.p.size;		// return success so they come back and 
									// get the rest of partial terminator
      }
      oz_hw_phys_movefromvirt (iopex -> u.readrec.trmseen, iopex -> u.readrec.trmbuff, phypages, skip);
      iopex -> u.readrec.trmseen = 0;					// partial terminator was put in data buffer
      goto rtn;
    }
  }

  /* Maybe this is a continuation of a multi-byte terminator sequence       */
  /* iopex -> u.readrec.trmseen has how many bytes have been matched so far */

  if (iopex -> u.readrec.trmseen != 0) {				// see if part of terminator seen last time
    size = iopex -> u.readrec.p.trmsize - iopex -> u.readrec.trmseen;	// get how much of the terminator we have yet to find
    if (size > dcmpb -> nbytes) size = dcmpb -> nbytes;			// ... but we won't find more than what cache has given us
    dcbuff  = oz_hw_phys_mappage (dcmpb -> phypage, &savepte);		// point to cache buffer data
    dcbuff += dcmpb -> pageoffs;					// offset to the byte we want to start with
    p = iopex -> u.readrec.trmbuff + iopex -> u.readrec.trmseen;	// point to what's left of terminator to match
    for (sts = 0; sts < size; sts ++) if (*(dcbuff ++) != *(p ++)) break; // see how much of it matches
    oz_hw_phys_unmappage (savepte);
    iopex -> u.readrec.trmseen += sts;					// this much more of the terminator has been matched up
    if (iopex -> u.readrec.trmseen == iopex -> u.readrec.p.trmsize) {	// read is complete if whole terminator found
      iopex -> u.readrec.status = OZ_SUCCESS;
      goto readdone;
    }
    if (sts < size) {
      iopex -> u.readrec.trmseen = 0;					// terminator broken, forget about it
      skip      = iopex -> u.readrec.rlen + iopex -> u.readrec.byteoffs;
      phypages  = iopex -> u.readrec.phypages;
      oz_hw_phys_movefromvirt (1, iopex -> u.readrec.trmbuff, phypages, skip); // but copy first byte to data buffer
      iopex -> u.readrec.rlen ++;
    }
    goto continuereading;
  }

  /* See how much of what it gave will fit in user data buffer */

  size = iopex -> u.readrec.p.size - iopex -> u.readrec.rlen;
  if (size > dcmpb -> nbytes) size = dcmpb -> nbytes;

  /* If no terminator specified, copy all of it to user buffer */

  if (iopex -> u.readrec.p.trmsize == 0) {
    oz_hw_phys_movephys (size, &(dcmpb -> phypage), dcmpb -> pageoffs, iopex -> u.readrec.phypages, iopex -> u.readrec.byteoffs + iopex -> u.readrec.rlen);
    iopex -> u.readrec.rlen += size;					// accumulate whatever was read in
    if (iopex -> u.readrec.rlen < iopex -> u.readrec.p.size) goto continuereading; // continue reading if we haven't filled buffer
  }

  /* Terminator processing */

  else {
    dcbuff  = oz_hw_phys_mappage (dcmpb -> phypage, &savepte);		// point to cache buffer data we just copied from
    dcbuff += dcmpb -> pageoffs;
    p = memchr (dcbuff, iopex -> u.readrec.trmbuff[0], size);		// search for the first byte of the terminator

    while (p != NULL) {
      sts = dcbuff + dcmpb -> nbytes - p;				// this is how much remains at first terminator byte
      if (sts > iopex -> u.readrec.p.trmsize) sts = iopex -> u.readrec.p.trmsize; // only match up to whole terminator
      if (memcmp (p, iopex -> u.readrec.trmbuff, sts) == 0) break;	// stop if successful comparison
      p = memchr (p + 1, iopex -> u.readrec.trmbuff[0], sts - 1);	// mismatch, look for first byte again
    }

    oz_hw_phys_unmappage (savepte);					// unmap cache page

    if (p == NULL) {
      oz_hw_phys_movephys (size, &(dcmpb -> phypage), dcmpb -> pageoffs, iopex -> u.readrec.phypages, iopex -> u.readrec.byteoffs + iopex -> u.readrec.rlen);
      iopex -> u.readrec.rlen += size;					// no match found, so it is all data
      if (iopex -> u.readrec.rlen < iopex -> u.readrec.p.size) goto continuereading; // continue reading
      iopex -> u.readrec.status = OZ_NOTERMINATOR;			// user buffer filled with no terminator, return semi-error status
      goto readdone;
    }
    size = p - dcbuff;							// length up to but not including start of terminator
    oz_hw_phys_movephys (size, &(dcmpb -> phypage), dcmpb -> pageoffs, iopex -> u.readrec.phypages, iopex -> u.readrec.byteoffs + iopex -> u.readrec.rlen);
    iopex -> u.readrec.rlen   += size;
    iopex -> u.readrec.trmseen = sts;					// set the size matched so far
    if (sts < iopex -> u.readrec.p.trmsize) goto continuereading;	// haven't found the whole thing, continue reading
  }

  iopex -> u.readrec.status = OZ_SUCCESS;
  goto readdone;

  /* Request requires more data to complete */

continuereading:
  size = iopex -> u.readrec.p.size + iopex -> u.readrec.p.trmsize;
  skip = iopex -> u.readrec.rlen   + iopex -> u.readrec.trmseen;

  dcmpb -> nbytes = size - skip;
  dcmpb -> virtblock = chnex -> cur_blk;
  dcmpb -> blockoffs = chnex -> cur_byt + skip;
  iopex -> u.readrec.status = oz_dev_vdfs_dcache_map_vbn_to_lbn (dcmpb, file);
  if (iopex -> u.readrec.status == OZ_SUCCESS) goto rtn;

  /* Read request is complete */

readdone:
  dcmpb -> nbytes = 0;
rtn:
  return (0);
}

/************************************************************************/
/*									*/
/*  Get information part 1						*/
/*									*/
/************************************************************************/

static uLong sc_getinfo1 (Iopex *iopex, Chnex *chnex, Devex *devex)

{
  OZ_VDFS_File *file;
  uLong clusterfactor, sts;

  sts = startshortcut (chnex, devex, iopex);		// block file from being closed
  if (sts == OZ_SUCCESS) {
    file = chnex -> file;
    if (file == NULL) sts = OZ_FILENOTOPEN;		// make sure a file is open
    else {						// ok, get info about the file

      /* Get fs independent info */

      iopex -> u.getinfo1.p.blocksize   = devex -> blocksize;
      iopex -> u.getinfo1.p.eofblock    = file -> attrlock_efblk;
      iopex -> u.getinfo1.p.eofbyte     = file -> attrlock_efbyt;
      iopex -> u.getinfo1.p.hiblock     = file -> allocblocks;
      iopex -> u.getinfo1.p.curblock    = chnex -> cur_blk;
      iopex -> u.getinfo1.p.curbyte     = chnex -> cur_byt;
      iopex -> u.getinfo1.p.secattrsize = oz_knl_secattr_getsize (file -> secattr);

      /* If caching enabled and cluster factor is a multiple of page size (so file */
      /* pages align with cache pages), we can give direct access to cache pages.  */

      if ((devex -> dcache != NULL) 
       && ((clusterfactor = devex -> volume -> clusterfactor) != 0) 
       && (((clusterfactor * devex -> blocksize) & ((1 << OZ_HW_L2PAGESIZE) - 1)) == 0)) {
        iopex -> u.getinfo1.p.knlpfmap  = vdfs_knlpfmap;
        iopex -> u.getinfo1.p.knlpfupd  = vdfs_knlpfupd;
        iopex -> u.getinfo1.p.knlpfrel  = vdfs_knlpfrel;
      }

      /* Fill in fs-dependent info */

      (*(devex -> vector -> getinfo1)) (iopex);

      /* Store block back to caller */

      movc4 (sizeof iopex -> u.getinfo1.p, &(iopex -> u.getinfo1.p), iopex -> as, iopex -> ap);
    }
    finishortcut (chnex, devex, iopex);			// allow a close to proceed
  }
  return (sts);
}

/************************************************************************/
/*									*/
/*  Read next entry from directory that is open on channel		*/
/*									*/
/************************************************************************/

static uLong sc_readdir (Iopex *iopex, Chnex *chnex, Devex *devex)

{
  OZ_IO_fs_readdir fs_readdir;

  fs_readdir = iopex -> u.readdir.p;					// copy the input parameter block

  memset (&(iopex -> u.wildscan.p), 0, sizeof iopex -> u.wildscan.p);	// clear out the wildscan param block

  iopex -> u.wildscan.p.init       = (chnex -> wildscan == NULL);	// init if there is no scanning going on there
  iopex -> u.wildscan.p.size       = fs_readdir.filenamsize;		// where to return the filename found
  iopex -> u.wildscan.p.buff       = fs_readdir.filenambuff;
  iopex -> u.wildscan.p.fileidsize = fs_readdir.fileidsize;		// where to return the fileid found
  iopex -> u.wildscan.p.fileidbuff = fs_readdir.fileidbuff;
  iopex -> u.wildscan.p.subs       = fs_readdir.filenamsubs;		// where to return substring sizes

  return (sc_wildscan (iopex, chnex, devex));				// process as a wildscan function
}

/************************************************************************/
/*									*/
/*  Get security attributes						*/
/*									*/
/************************************************************************/

static uLong be_getsecattr (Iopex *iopex, Chnex *chnex, Devex *devex)

{
  File *file;
  uLong size;

  file = chnex -> file;
  if (file == NULL) return (OZ_FILENOTOPEN);

  size = oz_knl_secattr_getsize (file -> secattr);
  if (size > iopex -> u.getsecattr.p.size) return (OZ_BUFFEROVF);
  memcpy (iopex -> u.getsecattr.p.buff, oz_knl_secattr_getbuff (file -> secattr), size);
  if (iopex -> u.getsecattr.p.rlen != NULL) *(iopex -> u.getsecattr.p.rlen) = size;
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Write boot block							*/
/*									*/
/************************************************************************/

static uLong be_writeboot (Iopex *iopex, Chnex *chnex, Devex *devex)

{
  uByte *bootblock;
  File *file;
  uLong sts;
  OZ_Dbn bb_nblocks, bb_logblock, eofblock, logblock, nblocks, part_logblock;
  OZ_IO_disk_getinfo1 disk_getinfo1, host_getinfo1;
  OZ_Iochan *partiochan;

  file = chnex -> file;
  if (file == NULL) return (OZ_FILENOTOPEN);
  if (IS_DIRECTORY (file)) return (OZ_FILEISADIR);
  if (!OZ_LOCK_ALLOW_TEST (chnex -> lockmode, OZ_LOCK_ALLOWS_SELF_WRITE)) return (OZ_NOWRITEACCESS);

  /* Get info about disk drive */

  memset (&disk_getinfo1, 0, sizeof disk_getinfo1);
  sts = diskio (OZ_IO_DISK_GETINFO1, sizeof disk_getinfo1, &disk_getinfo1, iopex, devex -> master_iochan);
  if (sts != OZ_SUCCESS) {
    oz_dev_vdfs_printk (iopex, "oz_dev_vdfs: error %u getting disk info\n", sts);
    return (sts);
  }

  /* Get number of blocks in the hardware boot block */

  sts = oz_hw_bootblock_nblocks (&disk_getinfo1, devex -> master_iochan, &bb_nblocks, &bb_logblock);
  if (sts != OZ_SUCCESS) return (sts);

  /* Make sure loader image file has just one pointer past its copy of the bootblock(s) (ie, it is contiguous), and get starting logical block number */

  eofblock = file -> attrlock_efblk;
  if (file -> attrlock_efbyt == 0) eofblock --;
  sts = (*(devex -> vector -> map_vbn_to_lbn)) (file, 1 + bb_nblocks, &nblocks, &logblock);
  if ((sts == OZ_SUCCESS) && (nblocks < eofblock - bb_nblocks)) sts = OZ_FILENOTCONTIG;
  if (sts != OZ_SUCCESS) return (sts);

  /* If disk is partition of another disk, relocate lbn by the starting block of the partition */
  /* 'logblock' must be an absolute value for the physical disk drive                          */
  /* Also get sec/trk/cyl of physical drive                                                    */

  part_logblock = disk_getinfo1.parthoststartblock;
  host_getinfo1 = disk_getinfo1;
  while (host_getinfo1.parthostdevname[0] != 0) {							/* if no partition host, we're done scanning */
													/* note: linux_dev_disk driver returns a parthoststartblock */
													/*       without a parthostdevname implying that the contents */
													/*       are at sometime in the future going to be copied to */
													/*       a partitioned disk */
    sts = oz_knl_iochan_crbynm (host_getinfo1.parthostdevname, OZ_LOCKMODE_NL, OZ_PROCMODE_KNL, NULL, &partiochan); /* assign channel to host disk device */
    if (sts != OZ_SUCCESS) {
      oz_dev_vdfs_printk (iopex, "oz_dev_vdfs: error %u assigning channel to partition host disk %s\n", sts, disk_getinfo1.parthostdevname);
      return (sts);
    }
    memset (&host_getinfo1, 0, sizeof host_getinfo1);							/* find out about it */
    sts = diskio (OZ_IO_DISK_GETINFO1, sizeof host_getinfo1, &host_getinfo1, iopex, partiochan);
    if (sts != OZ_SUCCESS) {										/* abort if error getting info */
      oz_dev_vdfs_printk (iopex, "oz_dev_vdfs: error %u getting info for disk %s\n", sts, oz_knl_iochan_getdevunit (partiochan));
      oz_knl_iochan_increfc (partiochan, -1);
      return (sts);
    }
    oz_knl_iochan_increfc (partiochan, -1);								/* deassign channel to host disk */
  }													/* repeat in case of nested partitioning */

  /* A copy of the boot block is the first block of the image file.  So read it from loader image file, modify it then write it to actual bootblock. */

  bootblock = OZ_KNL_PGPMALLOQ (bb_nblocks * disk_getinfo1.blocksize);					/* allocate a temp buffer for bootblock(s) */
  if (bootblock == NULL) return (OZ_EXQUOTAPGP);
  sts = oz_dev_vdfs_readvirtblock (file, 1, 0, bb_nblocks * disk_getinfo1.blocksize, bootblock, iopex, 1); /* read boot block image */
  if (sts == OZ_SUCCESS) {
    sts = oz_hw_bootblock_modify (bootblock, nblocks, logblock, part_logblock, &(iopex -> u.writeboot.p), &disk_getinfo1, &host_getinfo1, devex -> master_iochan);
    if (sts == OZ_SUCCESS) sts = oz_dev_vdfs_writelogblock (bb_logblock, 0, bb_nblocks * disk_getinfo1.blocksize, bootblock, 0, iopex); /* write to volume's actual boot block */
  }
  OZ_KNL_PGPFREE (bootblock);										/* free off temp buffer */

  return (sts);
}

/************************************************************************/
/*									*/
/*  Set current file position						*/
/*									*/
/************************************************************************/

static uLong be_setcurpos (Iopex *iopex, Chnex *chnex, Devex *devex)

{
  File *file;
  uLong blocksize;

  file = chnex -> file;
  if (file ==  NULL) return (OZ_FILENOTOPEN);
  if (IS_DIRECTORY (file)) return (OZ_FILEISADIR);

  blocksize = devex -> blocksize;

  chnex -> cur_blk  = iopex -> u.setcurpos.p.atblock;
  chnex -> cur_byt  = iopex -> u.setcurpos.p.atbyte;

  chnex -> cur_blk += chnex -> cur_byt / blocksize;
  chnex -> cur_byt %= blocksize;

  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Wildcard scanner							*/
/*									*/
/*  This is a shortcut routine that operates outside the thread so it 	*/
/*  must provide its own synchronization				*/
/*									*/
/*    Some examples:							*/
/*									*/
/*	"/~" means the whole disk, files and directories		*/
/*	"/~;*" means all files on the whole disk			*/
/*	"/~/" means all directories on the whole disk			*/
/*	"/*" means all files and directories in the root directory	*/
/*	"/*;*" means all files in the root directory			*/
/*									*/
/*    The version number can be:					*/
/*									*/
/*	1) A positive integer meaning exactly that number		*/
/*	3) A ';0' or just a ';', meaning the latest version		*/
/*	4) A ';*' meaning all versions (same as no ';' at all, except 	*/
/*	   it excludes directories)					*/
/*	5) A negative integer meaning that many back from latest	*/
/*									*/
/************************************************************************/

static uLong sc_wildscan (Iopex *iopex, Chnex *chnex, Devex *devex)

{
  char c, *p, *q, *r;
  File *dirfile;
  int usedup;
  OZ_IO_fs_open fs_open;
  uLong sts, vl;
  Volume *volume;
  Wildscan *wildscan;

  volume = devex -> volume;

  /* Declare this as a shortcut-in-progress on the channel.  This will block the be_close routine from closing this channel */
  /* up.  But be_close won't let us queue any I/O's to open or read directories, so it shouldn't be blocked for very long.  */

  sts = startshortcut (chnex, devex, iopex);
  if (sts != OZ_SUCCESS) return (sts);

  /* Store iopex in channel as current wildcard operation                      */
  /* There can be only one going on a channel so we don't have to make a queue */

  vl = oz_hw_smplock_wait (&(devex -> smplock_vl));				// lock wild_iopex
  if (chnex -> wild_iopex != NULL) {						// see if there is an I/O going on it
    oz_hw_smplock_clr (&(devex -> smplock_vl), vl);				// if so, release lock
    finishortcut (chnex, devex, iopex);
    return (OZ_CHANNELBUSY);							// ... and return error status
  }
  chnex -> wild_iopex = iopex;							// none going, say the scan is in progress
  oz_hw_smplock_clr (&(devex -> smplock_vl), vl);				// release lock
  
  /* Maybe initialize wildcard search context */

  wildscan = chnex -> wildscan;
  chnex -> wild_nested = 0;
  if (iopex -> u.wildscan.p.init) {

    /* If there is an existing wildscan search context, tell them to close it */

    sts = OZ_FILEALREADYOPEN;
    if (wildscan != NULL) goto rtnerrnf;

    /* Create a new wildcard context */

    sts = OZ_EXQUOTAPGP;
    wildscan = OZ_KNL_PGPMALLOQ (sizeof *wildscan + volume -> dirblocksize);
    if (wildscan == NULL) goto rtnerrnf;
    memset (wildscan, 0, sizeof *wildscan);

    /* If a wild spec is given, use it to open the directory and start scanning from there */

    if (iopex -> u.wildscan.p.wild != NULL) {
      strncpyz (wildscan -> lastname, iopex -> u.wildscan.p.wild, sizeof wildscan -> lastname);	// get wildcard spec to scan for
      sts = OZ_BADFILENAME;									// it must begin with a '/'
      if (wildscan -> lastname[0] != '/') goto rtnerr;

      if (iopex -> u.wildscan.p.dirlist) {							// see if they are doing a directory listing
        usedup = strlen (wildscan -> lastname);							// get length of name provided
        if ((wildscan -> lastname[usedup-1] == '/') && (usedup < sizeof wildscan -> lastname - 1)) { // if name ends in a '/' ...
          wildscan -> lastname[usedup] = '*';							// ... tack on an '*' ...
          wildscan -> lastname[usedup+1] = 0;							// ... to list out the dir contents
        }
      }

      if (devex -> vector -> versions) {
        p = strchr (wildscan -> lastname, ';');							// check for version specification
        if (p == NULL) {
          wildscan -> ver_incldirs = 1;								// none given, include directories
          wildscan -> ver_inclallfiles = 1;							// ... and all versions of files
        } else {
          *(p ++) = 0;										// chop lastname off at the ';'
          if (strcmp (p, "*") == 0) wildscan -> ver_inclallfiles = 1;				// ';*' means all versions of files
          else {
            if (*p != '-') wildscan -> ver_number = oz_hw_atoi (p, &usedup);			// ';n' means exactly version 'n'
            else wildscan -> ver_number = - oz_hw_atoi (++ p, &usedup);				// ';-n' means the nth oldest version
            sts = OZ_BADFILEVER;
            if (p[usedup] != 0) goto rtnerr;
          }
        }
      } else {
        wildscan -> ver_incldirs = 1;								// none given, include directories
        wildscan -> ver_inclallfiles = 1;							// ... and all versions of files
      }

      q = wildscan -> lastname + strlen (wildscan -> lastname);					// point to the end of spec (where ';' was)
      p = strchr (wildscan -> lastname, '*');							// see if there is an '*' in there
      if ((p != NULL) && (p < q)) q = p;							// if so, back up to it
      p = strchr (wildscan -> lastname, '?');							// see if there is an '?' in there
      if ((p != NULL) && (p < q)) q = p;							// if so, back up to it
      p = strchr (wildscan -> lastname, '~');							// see if there is an '~' in there
      if ((p != NULL) && (p < q)) q = p;							// if so, back up to it
      while (q[-1] != '/') -- q;								// now back up to the slash before all that
      r = wildscan -> wildspec;									// everything after that '/' is the wildspec
      for (p = q; (c = *(p ++)) != 0;) {							// but remove redundant '*'s and '~'s
        if ((c == '*') || (c == '~')) {
          while ((*p == '*') || (*p == '~')) if (*(p ++) == '~') c = '~';
        }
        *(r ++) = c;
      }
      *r = 0;											// null terminate fixed wildscan spec
      *q = 0;											// everything up to and including the '/' is the name of the top level directory

      sts = oz_knl_iochan_create (devex -> devunit, OZ_LOCKMODE_CR, OZ_PROCMODE_KNL, NULL, &(wildscan -> iochan));
      if (sts != OZ_SUCCESS) goto rtnerr;
      ((Chnex *)oz_knl_iochan_ex (wildscan -> iochan)) -> scassochnex = chnex;			// any close of chnex will also abort any I/O's done on this new channel

      chnex -> wildscan = wildscan;								// save wildcard scan context

      memset (&fs_open, 0, sizeof fs_open);							// start opening the top directory
      fs_open.name      = wildscan -> lastname;
      fs_open.lockmode  = OZ_LOCKMODE_CR;
      fs_open.rnamesize = sizeof wildscan -> basespec;						// ... put its actual name here
      fs_open.rnamebuff = wildscan -> basespec;

      sts = oz_knl_iostart2 (1, wildscan -> iochan, OZ_PROCMODE_KNL, wildscan_topdiropen, chnex, 
                             NULL, NULL, NULL, NULL, OZ_IO_FS_OPEN, sizeof fs_open, &fs_open);
      if (sts != OZ_STARTED) wildscan_topdiropen (chnex, sts);
    }

    /* If no wild spec given, assume there is a directory open on the channel and return all its entries */
    /* This is used by sc_readdir as a direct conversion from readdir to wildscan                        */

    else {
      sts = OZ_SUCCESS;										// assume it will pass all tests
      dirfile = chnex -> file;									// point to directory file
      if (dirfile == NULL) sts = OZ_FILENOTOPEN;						// there must be one open here
      if (!IS_DIRECTORY (dirfile)) sts = OZ_FILENOTADIR;					// it must be a directory
      if (!OZ_LOCK_ALLOW_TEST (chnex -> lockmode, OZ_LOCK_ALLOWS_SELF_READ)) sts = OZ_NOREADACCESS; // it must be readable
      if (sts != OZ_SUCCESS) goto rtnerr;							// fail if any not true
      wildscan -> wildspec[0] = '*';								// match everything in the directory
      wildscan -> ver_incldirs = 1;								// include directories therein
      wildscan -> ver_inclallfiles = 1;								// include all versions of files
      chnex -> wildscan  = wildscan;								// scanning context now active on channel
      wildscan -> iochan = chnex -> iochan;							// this is the channel it's open on
      oz_knl_iochan_increfc (wildscan -> iochan, 1);						// one more ref to that channel
      wildscan_topdiropen (chnex, OZ_SUCCESS);							// it has successfully been opened
    }
  }

  /* If not initializing, continue wildcard scanning from where we left off */

  else if (wildscan == NULL) {						// maybe we hit the end last time
    sts = OZ_ENDOFFILE;
    goto rtnerrnf;
  }
  else if (wildscan -> blockvbn != 0) {
    wildscan_lockdir (devex -> vector -> wildscan_continue, chnex);	// maybe we are in the middle of a block
  }
  else wildscan_lockdir (wildscan_startdir, chnex);			// start at beginning of directory

  return (OZ_STARTED);

  /* Initialization error, free struct and return error status */

rtnerr:
  if (wildscan -> wildx != NULL) oz_crash ("oz_dev_vdfs wildscan: error %u with non-null wildx", sts);
  OZ_KNL_PGPFREE (wildscan);
rtnerrnf:
  vl = oz_hw_smplock_wait (&(devex -> smplock_vl));			// make sure abort isn't slipping in here
  chnex -> wild_iopex = NULL;						// mark request no longer in progress
  oz_hw_smplock_clr (&(devex -> smplock_vl), vl);			// let abort check it out now if it wants
  finishortcut (chnex, devex, iopex);					// shortcut no longer in progress on channel
  return (sts);
}

/**************************************/
/* The top directory file is now open */
/**************************************/

static void wildscan_topdiropen (void *chnexv, uLong status)

{
  char *wildspec;
  Chnex *chnex;
  Devex *devex;
  Iopex *iopex;
  OZ_Process *oldprocess;
  uLong vl;
  Wildscan *wildscan;

  chnex    = chnexv;
  iopex    = chnex -> wild_iopex;
  wildscan = chnex -> wildscan;
  devex    = iopex -> devex;

  /* If top directory failed to open, return failure status */

  if (status != OZ_SUCCESS) {
    oz_dev_vdfs_wildscan_iodonex (iopex, status);
    return;
  }

  /* Return the parsed wildcard spec if requested */

  if (iopex -> u.wildscan.p.wildsize > 0) {

    /* Malloc a buffer to build the complete wildcard spec */

    wildspec = OZ_KNL_PGPMALLOQ (strlen (wildscan -> basespec) + strlen (wildscan -> wildspec) + 16);
    if (wildspec == NULL) {
      oz_dev_vdfs_wildscan_iodonex (iopex, OZ_EXQUOTAPGP);
      return;
    }

    /* Put the base and wildcard strings in it */

    strcpy (wildspec, wildscan -> basespec);
    strcat (wildspec, wildscan -> wildspec);

    /* Put the version number stuff in it */

    if (!(wildscan -> ver_incldirs)) {
      if (wildscan -> ver_inclallfiles) strcat (wildspec, ";*");
      else if (wildscan -> ver_number == 0) strcat (wildspec, ";");
      else oz_sys_sprintf (16, wildspec + strlen (wildspec), ";%d", wildscan -> ver_number);
    }

    /* Copy to caller's buffer */

    oldprocess = oz_knl_process_getcur ();
    oz_knl_process_setcur (oz_knl_ioop_getprocess (iopex -> ioop));
    (*(devex -> vector -> returnspec)) (wildspec, iopex -> u.wildscan.p.wildsize, iopex -> u.wildscan.p.wildbuff, iopex -> u.wildscan.p.wildsubs);
    oz_knl_process_setcur (oldprocess);

    /* Free off temp buffer */

    OZ_KNL_PGPFREE (wildspec);
  }

  /* Start reading top directory from the beginning */

  wildscan_lockdir (wildscan_startdir, chnex);
}

/*************************************************************/
/* A sub-directory file is now open                          */
/*  The outer directory is still locked, but this unlocks it */
/*************************************************************/

static void wildscan_subdiropen (void *chnexv, uLong status)

{
  Chnex *chnex;
  Devex *devex;
  Iopex *iopex;
  uLong vl;
  Wildscan *wildscan;

  chnex    = chnexv;
  iopex    = chnex -> wild_iopex;
  wildscan = chnex -> wildscan;
  devex    = iopex -> devex;

  /* If directory failed to open, return failure status */

  if (status != OZ_SUCCESS) oz_dev_vdfs_wildscan_iodonex (iopex, status);

  /* If we need to return the directory name itself to caller, do it now */

  else if (wildscan -> rtndirnametocaller && !(iopex -> u.wildscan.p.delaydir)) {
    wildscan -> blockvbn = 0;				// when we come back, start reading at the beginning
    oz_dev_vdfs_wildscan_output (chnex, "", 0, NULL);	// ... and output to caller
  }

  /* Start reading directory from the beginning */

  else {
    wildscan_unlkdir (chnex);				// unlock outer directory
    wildscan_lockdir (wildscan_startdir, chnex);	// lock new directory and start reading it
  }
}

/***********************************************************/
/* Start reading the directory starting from the beginning */
/*   The directory is locked throughout                    */
/***********************************************************/

static void wildscan_startdir (Chnex *chnex)

{
  Chnex *dirchnex;
  File *dirfile;
  uLong vl;
  Wildscan *next_wildscan, *wildscan;

  wildscan = chnex -> wildscan;
  dirchnex = oz_knl_iochan_ex (wildscan -> iochan);			// point to directory file's chnex block
  dirfile  = dirchnex -> file;						// point to directory file's block
  if (wildscan -> prev_dirfile == NULL) {				// see if wildscan context is already linked up
    vl = oz_hw_smplock_wait (&(dirfile -> recio_vl));			// if not, lock the dirfile's list of wildscans
    wildscan -> next_dirfile = next_wildscan = dirfile -> wildscans;	// link this wildscan on to it
    wildscan -> prev_dirfile = &(dirfile -> wildscans);
    dirfile -> wildscans = wildscan;
    if (next_wildscan != NULL) next_wildscan -> prev_dirfile = &(wildscan -> next_dirfile);
    oz_hw_smplock_clr (&(dirfile -> recio_vl), vl);			// unlock the list
  }
  wildscan -> lastname[0] = 0;						// reset as there is no 'last name processed' in this directory
  wildscan -> blockvbn = 1;						// start reading directory file at the beginning
  oz_dev_vdfs_wildscan_readdir (chnex);
}

/********************************************/
/* Start reading a block from the directory */
/********************************************/

void oz_dev_vdfs_wildscan_readdir (Chnex *chnex)

{
  Devex *devex;
  Iopex *iopex;
  OZ_IO_fs_readblocks fs_readblocks;
  uLong nestlevel, sts, vl;
  Volume *volume;
  Wildscan *wildscan;

  iopex    = chnex -> wild_iopex;
  wildscan = chnex -> wildscan;
  devex    = iopex -> devex;
  volume   = devex -> volume;

  /* If user has aborted scan request, give up */

  if (iopex -> aborted) oz_dev_vdfs_wildscan_iodonex (iopex, OZ_ABORTED);

  /* Otherwise, start reading from the directory */

  else {
    nestlevel = chnex -> wild_nested;					// save call nesting level
    chnex -> wild_nested = 0;						// clear it in case of async completion
    memset (&fs_readblocks, 0, sizeof fs_readblocks);			// set up read parameters
    fs_readblocks.size = volume -> dirblocksize;
    fs_readblocks.buff = wildscan -> blockbuff;
    fs_readblocks.svbn = wildscan -> blockvbn;
    sts = oz_knl_iostart2 (1, wildscan -> iochan, OZ_PROCMODE_KNL, wildscan_dirread, chnex, 
                           NULL, NULL, NULL, NULL, OZ_IO_FS_READBLOCKS, sizeof fs_readblocks, &fs_readblocks);
    if (sts != OZ_STARTED) {
      chnex -> wild_nested = nestlevel + 1;				// sync completion, inc nesting level
      if ((chnex -> wild_nested < 4) || (sts != OZ_SUCCESS)) {		// if still low or error, 
        wildscan_dirread (chnex, sts);					// ... just call completion routine
      } else {
        if (chnex -> wild_lowipl == NULL) chnex -> wild_lowipl = oz_knl_lowipl_alloc ();
        oz_knl_lowipl_call (chnex -> wild_lowipl, wildscan_lowipl, chnex); // deep, call via lowipl to reset call stack
      }
    }
  }
}

/**************************************************************************/
/* A directory block read completed synchronously, but the call level was */
/* nested too deep.  So this lowipl routine is called with a reset stack. */
/**************************************************************************/

static void wildscan_lowipl (void *chnexv, OZ_Lowipl *lowipl)

{
  ((Chnex *)chnexv) -> wild_nested = 0;		// reset the nested call level
  wildscan_dirread (chnexv, OZ_SUCCESS);	// call the read complete routine
}

/****************************************/
/* A directory block has just been read */
/****************************************/

static void wildscan_dirread (void *chnexv, uLong status)

{
  Chnex *chnex;
  Iopex *iopex;

  chnex = chnexv;
  iopex = chnex -> wild_iopex;

  /* If read was successful, process the block */

  if (status == OZ_SUCCESS) {
    chnex -> wildscan -> blockoffs = 0;
    (*(iopex -> devex -> vector -> wildscan_continue)) (chnex);
  }

  /* If hit end-of-file, pop out a level and continue with the outer directory */

  else if (status == OZ_ENDOFFILE) oz_dev_vdfs_wildscan_direof (chnex);

  /* Other read failure, abort request */

  else oz_dev_vdfs_wildscan_iodonex (iopex, status);
}

/********************************************************************************************************************/
/* Either wildscan_continue routine detected logical eof on directory, or the dirread routine detected physical eof */
/********************************************************************************************************************/

void oz_dev_vdfs_wildscan_direof (Chnex *chnex)

{
  Chnex *oldchnex;
  Iopex *iopex;
  Wildscan *wildscan;

  iopex    = chnex -> wild_iopex;
  wildscan = chnex -> wildscan;

  if (wildscan -> rtndirnametocaller && iopex -> u.wildscan.p.delaydir) {	// maybe return the directory name itself to caller
    wildscan -> rtndirnametocaller = 0;						// only return it once for this directory
    wildscan -> blockoffs = iopex -> devex -> volume -> dirblocksize;		// force a new read next time through
    wildscan -> blockvbn  = (OZ_Dbn)(-1);					// ... but make sure we get an eof error
    oz_dev_vdfs_wildscan_output (chnex, "", 0, NULL);				// output it, when we come back, we get another ...
										//   eof error, but rtndirnametocaller will be zero
  } else {
    (*(iopex -> devex -> vector -> wildscan_terminate)) (chnex);		// close out fs dependent context
    wildscan_unlink (wildscan);							// make sure wildscan is unlinked from dirfile->wildscans list
    wildscan_unlkdir (chnex);							// unlock directory so it can be written to
    chnex -> wildscan = wildscan -> nextouter;					// pop this level
    oldchnex = oz_knl_iochan_ex (wildscan -> iochan);				// get dirfile's chnex pointer
    oldchnex -> scassochnex = NULL;						// it is no longer associated
    oz_knl_iochan_increfc (wildscan -> iochan, -1);				// close it out
    OZ_KNL_PGPFREE (wildscan);
    if (chnex -> wildscan == NULL) oz_dev_vdfs_wildscan_iodonex (iopex, OZ_ENDOFFILE); // no, resume with next outer level, if any
    else wildscan_lockdir (iopex -> devex -> vector -> wildscan_continue, chnex); // ... after re-locking the directory
  }
}

/************************************************************************/
/*									*/
/*  Start scanning a sub-directory					*/
/*									*/
/*    Input:								*/
/*									*/
/*	name = NULL or directory's full path name			*/
/*	fileid = NULL or directory's file-id				*/
/*	the current directory is locked					*/
/*									*/
/*    Output:								*/
/*									*/
/*	no directory is locked						*/
/*									*/
/*    Note:								*/
/*									*/
/*	Exactly one of name or fileid should be non-NULL		*/
/*									*/
/************************************************************************/

void oz_dev_vdfs_wildscan_startsubdir (Chnex *chnex, const char *name, Fileid *fileid, int wildscan_match_rc)

{
  Iopex *iopex;
  OZ_IO_fs_open fs_open;
  OZ_VDFS_Volume *volume;
  uLong sts;
  Wildscan *innerwild, *wildscan;

  iopex    = chnex -> wild_iopex;
  volume   = iopex -> devex -> volume;
  wildscan = chnex -> wildscan;

  innerwild = OZ_KNL_PGPMALLOQ (sizeof *innerwild + volume -> dirblocksize);		// allocate sub-directory scan context block
  if (innerwild == NULL) {
    oz_dev_vdfs_wildscan_iodonex (iopex, OZ_EXQUOTAPGP);
    return;
  }
  *innerwild = *wildscan;								// copy current context block to it
  innerwild -> wildx = NULL;
  strcat (innerwild -> basespec, name);							// concat on new sub-dir name
  strcpy (innerwild -> wildspec, wildscan -> wildspec + (wildscan_match_rc >> 2));	// save remaining wildcard spec
  innerwild -> nextouter   = wildscan;							// link it to the chain
  innerwild -> rtndirnametocaller = (wildscan_match_rc & wildscan -> ver_incldirs);	// maybe return dir name to caller
  if ((innerwild -> hastilde == NULL) && (innerwild -> wildspec[0] == '~')) {
    innerwild -> hastilde  = wildscan;							// save outermost level that has tilde
  }

  sts = oz_knl_iochan_create (iopex -> devex -> devunit, OZ_LOCKMODE_CR, OZ_PROCMODE_KNL, NULL, &(innerwild -> iochan));
  if (sts != OZ_SUCCESS) {
    OZ_KNL_PGPFREE (innerwild);								// can't assign channel, free new block
    oz_dev_vdfs_wildscan_iodonex (iopex, sts);						// abort the I/O request
    return;
  }
  ((Chnex *)oz_knl_iochan_ex (wildscan -> iochan)) -> scassochnex = chnex;		// any close of chnex will also abort any I/O's done on this new channel

  chnex -> wildscan = innerwild;							// have channel, link up context block

  memset (&fs_open, 0, sizeof fs_open);							// start opening sub-directory
  fs_open.lockmode = OZ_LOCKMODE_CR;							// we can read, others can read and write
  if (fileid == NULL) {
    fs_open.name = name;								// maybe open it by directory's name
  } else {
    fs_open.fileidsize = volume -> devex -> vector -> fileid_size;			// ... or open by directory's fileid
    fs_open.fileidbuff = fileid;
  }
  sts = oz_knl_iostart2 (1, innerwild -> iochan, OZ_PROCMODE_KNL, wildscan_subdiropen, chnex, 
                         NULL, NULL, NULL, NULL, OZ_IO_FS_OPEN, sizeof fs_open, &fs_open);
  if (sts != OZ_STARTED) wildscan_subdiropen (chnex, sts);
}

/************************************************************************/
/*									*/
/*  Match a filename given a wildspec with a tilde in it		*/
/*									*/
/*    Input:								*/
/*									*/
/*	wildscan = current wildscan context that filename was found in	*/
/*	         -> hastilde = wildscan context that has outermost '~'	*/
/*	filename = filename to be tested				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_dev_vdfs_wildscan_match<0> = 0 : filename doesn't match	*/
/*	                                1 : filename matches
/*	oz_dev_vdfs_wildscan_match<1> = 1 : (always, to say to scan subdir)
/*									*/
/*    Example:								*/
/*									*/
/*	hastilde -> wildspec = "ab~gh/ijk"				*/
/*	            basespec = "/whatever/"				*/
/*	  (next) -> wildspec = don't care				*/
/*	            basespec = "/whatever/abcd/"			*/
/*	wildscan -> wildspec = don't care				*/
/*	            basespec = "/whatever/abcd/fgh/"			*/
/*	filename = "ijk"						*/
/*									*/
/************************************************************************/

int oz_dev_vdfs_wildscan_match (Wildscan *wildscan, const char *filename)

{
  char filespec[OZ_FS_MAXFNLEN];
  Wildscan *hastilde;

  hastilde = wildscan -> hastilde;
  if (hastilde == NULL) return (wildscan_match (wildscan -> wildspec, filename, 0));

  /* Build the complete filespec from the hastilde level up */
  /* Using the example input, we get "abcd/fgh/ijk"         */

  strcpy (filespec, wildscan -> basespec + strlen (hastilde -> basespec));
  if (strlen (filespec) + strlen (filename) >= sizeof filespec) return (0);
  strcat (filespec, filename);

  /* Now match it.  The hastilde -> wildspec from the example is "ab~de/fgh/ijk". */
  /* Force output to do all subdirs in case tilde matches a subdir somehow.       */

  return (wildscan_match (hastilde -> wildspec, filespec, 1) | 2);
}

/************************************************************************/
/*									*/
/*  Return whether the 'filename' matches 'wildcard'			*/
/*									*/
/*    Input:								*/
/*									*/
/*	wildcard = wildcard string pointer				*/
/*	filename = filename string pointer				*/
/*	tilde = 0 : stop scanning on '/' in wildcard			*/
/*	        1 : don't stop scanning on '/' in wildcard		*/
/*									*/
/*    Output:								*/
/*									*/
/*	wildscan_match<0> = 0 : don't output it				*/
/*	                    1 : output it				*/
/*	              <1> = 0 : don't scan it				*/
/*	                    1 : scan it after skipping <:2> chars	*/
/*	if <1> is set and the string has a '~' in it, the <:2> has the 	*/
/*	offset of the '~'						*/
/*									*/
/************************************************************************/

static int wildscan_match (const char *wildcard, const char *filename, int tilde)

{
  char c;
  const char *fn, *wc;
  int rc;

  wc = wildcard;
  fn = filename;

  /* Skip chars at the beginning that match exactly        */
  /* Also skip non-delimiters in fn that match a '?' in wc */

skip:
  while (((c = *wc) != 0) && ((*fn == c) || ((c == '?') && (*fn != '/')))) { wc ++; fn ++; }

  /* If we reached the end of the wildcard string and the filename, then it matches, else it doesn't */

  if (c == 0) return (*fn == 0);				// return if we used the whole wildcard string

  /* An '*' matches any number of non-delimiters in filename */

  if (c == '*') {
    c = *(++ wc);						// get character following the '*'
								// optimizations ...
    if (tilde) {
      if (c == 0) return (strchr (fn, '/') == NULL);		// if '*' was last thing, then match iff no more '/'s in filename
      if (c == '/') {						// if '*' followed by '/', match all in filename up to '/'
        fn = strchr (fn, '/');
        if (fn != NULL) goto skip;
        return (0);						// ... but fail if there is no '/' in filename
      }
    } else {
      if (c == 0) return (1);					// if '*' was last thing in wildcard, it matches
      if (c == '/') return (fn[strlen(fn)-1] == '/');		// also a match if '*/' and filename is a directory
    }
    do {							// pound it out the hard way ...
      rc = wildscan_match (wc, fn, tilde);			// see if we get a match with rest of filename as is
      if (rc & 3) {
        rc += (wc - wildcard) << 2;				// if so, return total wildcard chars to skip
        return (rc);
      }
      c = *(++ fn);						// otherwise, swallow a filename char
    } while ((c != 0) && (c != '/'));				// ... and try matching if there was one to swallow
    return (0);							// nothing left to swallow, return failure status
  }

  /* An '~' matches any number of chars in filename, including delimiters */

  if (c == '~') {
    rc = (wc - wildcard) << 2;					// number of chars to skip, not including the '~'
    c  = *(++ wc);						// get character following the '~'
								// optimizations ...
    if (c == 0) return (rc | 3);				// if '~' was last thing in wildcard, it matches, output and scan it
    do {							// pound it out the hard way ...
      if (wildscan_match (wc, fn, tilde) & 1) {			// see if we get a match with rest of filename as is
        return (rc | 3);					// if so, output and scan it
      }
      c = *(++ fn);						// otherwise, swallow a filename char
    } while ((c != 0) && (tilde || (c != '/')));		// ... and try matching if there was one to swallow
    if (c == 0) return (0);					// if end of filename and still no match, it doesn't match
    return (rc | 2);						// filename is a directory, it could possibly match if we scan it too
  }

  /* If we reached the end of filename string, and it was a directory, then return saying that it ought to be scanned */
  /* This accounts for the case of wildcard='abc/def' and filename='abc/', we want to scan directory 'abc/' for 'def' */

  if ((fn > filename) && (fn[0] == 0) && (fn[-1] == '/')) {
    rc = (wc - wildcard) << 2;
    return (rc | 2);
  }

  /* A simple non-matching character */

  return (0);
}

/*********************************************************************/
/* Output the basespec/filename/version and complete the I/O request */
/*  If fileid is NULL, output the fileid of the directory on chnex   */
/*  The directory is locked on input and unlocked on output          */
/*********************************************************************/

void oz_dev_vdfs_wildscan_output (Chnex *chnex, char *filename, uLong version, const Fileid *fileid)

{
  char *filespec;
  Devex *devex;
  int l;
  Iopex *iopex;
  OZ_Process *oldprocess;
  uLong sts, vl;
  Wildscan *wildscan;

  iopex    = chnex -> wild_iopex;
  wildscan = chnex -> wildscan;
  devex    = iopex -> devex;

  /* Output values to caller's buffers */

  sts = OZ_EXQUOTAPGP;									// assume malloc will fail
  l = strlen (wildscan -> basespec) + strlen (filename);				// get length of both input strings
  filespec = OZ_KNL_PGPMALLOQ (l + 16);							// malloc a temp buffer for output string
  if (filespec != NULL) {
    strcpy (filespec, wildscan -> basespec);						// copy in base spec
    strcat (filespec, filename);							// copy in filename spec
    if (version != 0) {									// if version present, ...
      oz_sys_sprintf (16, filespec + l, ";%u", version);				// ... copy in version string
    }

    oldprocess = oz_knl_process_getcur ();						// map caller's address space
    oz_knl_process_setcur (oz_knl_ioop_getprocess (iopex -> ioop));
    if (iopex -> u.wildscan.p.fileidbuff != NULL) {					// maybe return fileid */
      if (fileid == NULL) fileid = (devex -> vector -> get_fileid) (((Chnex *)oz_knl_iochan_ex (wildscan -> iochan)) -> file);
      memcpy (iopex -> u.wildscan.p.fileidbuff, fileid, iopex -> u.wildscan.p.fileidsize);
    }
    (*(devex -> vector -> returnspec)) (filespec, iopex -> u.wildscan.p.size, iopex -> u.wildscan.p.buff, iopex -> u.wildscan.p.subs); // return filespec
    oz_knl_process_setcur (oldprocess);							// restore mapping

    OZ_KNL_PGPFREE (filespec);								// free temp buffer
    sts = OZ_SUCCESS;									// successful
  }

  /* Post I/O request completion */

  oz_dev_vdfs_wildscan_iodonex (iopex, sts);
}

/********************************************/
/* Post the wildscan I/O request completion */
/*  The directory is unlocked               */
/********************************************/

void oz_dev_vdfs_wildscan_iodonex (Iopex *iopex, uLong status)

{
  Chnex *chnex;
  Devex *devex;
  uLong vl;

  chnex = iopex -> chnex;
  devex = iopex -> devex;

  wildscan_unlkdir (chnex);					// unlock any directory we may have locked

  vl = oz_hw_smplock_wait (&(devex -> smplock_vl));		// make sure abort isn't slipping in here
  if (chnex -> wild_iopex != iopex) oz_crash ("oz_dev_vdfs oz_dev_vdfs_wildscan_iodonex: iopex being posted is not in progress");
  chnex -> wild_iopex = NULL;					// mark request no longer in progress
  oz_hw_smplock_clr (&(devex -> smplock_vl), vl);		// let abort check it out now if it wants
  finishortcut (chnex, devex, iopex);				// shortcut is done, let close proceed on channel
  iodonex (iopex, status, NULL, NULL);				// post the I/O request
}

/***************************************/
/* Unlink from dirfile->wildscans list */
/***************************************/

static void wildscan_unlink (Wildscan *wildscan)

{
  Chnex *dirchnex;
  File *dirfile;
  uLong vl;
  Wildscan *next_wildscan, **prev_wildscan;

  dirchnex = oz_knl_iochan_ex (wildscan -> iochan);				// point to dirfile's chnex struct
  dirfile  = dirchnex -> file;							// point to dirfile's file struct
  if (dirfile != NULL) {							// (maybe the directory never actually opened)
    vl = oz_hw_smplock_wait (&(dirfile -> recio_vl));				// lock its dirfile->wildscans list
    prev_wildscan = wildscan -> prev_dirfile;					// see if this wildscan is linked up
    if (prev_wildscan != NULL) {
      *prev_wildscan = next_wildscan = wildscan -> next_dirfile;		// if so, unlink it
      if (next_wildscan != NULL) next_wildscan -> prev_dirfile = wildscan -> prev_dirfile;
    }
    oz_hw_smplock_clr (&(dirfile -> recio_vl), vl);				// unlink dirfile->wildscans list
  }
}

/********************************************************************************************************/
/* Lock the directory currently being operated on to prevent it from being modified on us by the thread */
/********************************************************************************************************/

static void wildscan_lockdir (void (*entry) (Chnex *chnex), Chnex *chnex)

{
  Chnex *dirchnex;

  if (chnex -> wild_dirlocked != NULL) oz_crash ("oz_dev_vdfs wildscan_lockdir: dir already locked");
  dirchnex = oz_knl_iochan_ex (chnex -> wildscan -> iochan);
  chnex -> wild_dirlocked = dirchnex -> file;		// save pointer to directory file to be locked
  chnex -> wild_lockentry = entry;			// save callback routine entrypoint
  wildscan_trytolock (chnex, NULL);			// try to lock, call callback when successful
}

static void wildscan_trytolock (void *chnexv, OZ_Event *event)

{
  Chnex *chnex;
  File *dirfile;
  Long dirlockr;
  uLong sts;

  chnex   = chnexv;				// get channel the wildscan is being done on
  dirfile = chnex -> wild_dirlocked;		// get the directory file to be locked

  do {
    while ((dirlockr = dirfile -> dirlockr) < 0) {					// see if directory is being written
      oz_knl_event_set (dirfile -> dirlockf, 0);					// in case it is still locked
      dirlockr = dirfile -> dirlockr;							// see if writer is still active
      if (dirlockr >= 0) break;								// break out if no longer active
      sts = oz_knl_event_queuecb (dirfile -> dirlockf, wildscan_trytolock, chnex);	// still active, get a callback when done
      if (sts == OZ_FLAGWASCLR) return;
      if (sts != OZ_FLAGWASSET) oz_knl_event_waitone (dirfile -> dirlockf);		// if error, manually wait here
    }
  } while (!oz_hw_atomic_setif_long (&(dirfile -> dirlockr), dirlockr + 1, dirlockr));	// one more reader active
  (*(chnex -> wild_lockentry)) (chnex);							// got it, call the callback
}

/*****************************************************/
/* Unlock the directory to allow thread to modify it */
/*****************************************************/

static void wildscan_unlkdir (Chnex *chnex)

{
  if (chnex -> wild_dirlocked != NULL) {							// see if anything locked
    if (oz_hw_atomic_inc_long (&(chnex -> wild_dirlocked -> dirlockr), -1) == 0x80000000) {	// ok, decrement read count
      oz_knl_event_set (chnex -> wild_dirlocked -> dirlockf, 1);				// wake any waiting writer
    }
    chnex -> wild_dirlocked = NULL;								// nothing locked now
  }
}

/************************************************************************/
/*									*/
/*  Get information part 2						*/
/*									*/
/************************************************************************/

static uLong be_getinfo2 (Iopex *iopex, Chnex *chnex, Devex *devex)

{
  uLong sts;

  if (chnex -> file == NULL) sts = OZ_FILENOTOPEN;
  else sts = (*(devex -> vector -> getinfo2)) (iopex);
  if (sts == OZ_SUCCESS) (*(devex -> vector -> returnspec)) (iopex -> u.getinfo2.p.filnambuff, 0, NULL, iopex -> u.getinfo2.p.filnamsubs);

  return (sts);
}

/************************************************************************/
/*									*/
/*  Get information part 3 (no file need be open)			*/
/*									*/
/*  Don't make this a shortcut as we want to guarantee the volume 	*/
/*  doesn't get dismounted on us					*/
/*									*/
/************************************************************************/

static uLong be_getinfo3 (Iopex *iopex, Chnex *chnex, Devex *devex)

{
  const char *unitname;
  OZ_Devunit *devunit;
  uLong sts;

  if (iopex -> u.getinfo3.p.underbuff != NULL) {
    devunit  = oz_knl_iochan_getdevunit (devex -> master_iochan);
    unitname = oz_knl_devunit_devname (devunit);
    strncpyz (iopex -> u.getinfo3.p.underbuff, unitname, iopex -> u.getinfo3.p.undersize);
  }
  if (devex -> dcache != NULL) oz_knl_dcache_stats (devex -> dcache, &(iopex -> u.getinfo3.p.nincache), 
                                                                     &(iopex -> u.getinfo3.p.ndirties), 
                                                                     &(iopex -> u.getinfo3.p.dirty_interval), 
                                                                     &(iopex -> u.getinfo3.p.avgwriterate));
  iopex -> u.getinfo3.p.fileidsize = devex -> vector -> fileid_size;
  iopex -> u.getinfo3.p.mountflags = devex -> volume -> mountflags;
  iopex -> u.getinfo3.p.versions   = devex -> vector -> versions;

  sts = (*(devex -> vector -> getinfo3)) (iopex);
  movc4 (sizeof iopex -> u.getinfo3.p, &(iopex -> u.getinfo3.p), iopex -> as, iopex -> ap);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Set up crash dump file						*/
/*									*/
/************************************************************************/

static uLong be_crash (Iopex *iopex, Chnex *chnex, Devex *devex)

{
  File *file;
  OZ_Dbn efblk;
  uLong sts;

  file = chnex -> file;
  if (file == NULL) return (OZ_FILENOTOPEN);
  if (IS_DIRECTORY (file)) return (OZ_FILEISADIR);
  if (!OZ_LOCK_ALLOW_TEST (chnex -> lockmode, OZ_LOCK_ALLOWS_SELF_WRITE)) return (OZ_NOWRITEACCESS);

  sts = OZ_SUCCESS;
  if (crash_file != NULL) {
    crash_file -> refcount --;
    crash_file -> refc_read --;
    crash_file -> refc_write --;
    crash_file = NULL;

    sts = diskio (OZ_IO_DISK_CRASH, 0, NULL, iopex, devex -> master_iochan);
    if (sts != OZ_SUCCESS) {
      oz_dev_vdfs_printk (iopex, "oz_dev_vdfs: error %u disabling crash disk\n", sts);
    }
  }

  if (iopex -> ap != NULL) {
    if (iopex -> as != sizeof (OZ_IO_fs_crash)) return (OZ_BADBUFFERSIZE);

    /* Get disk drive crash info */

    memset (&crash_disk, 0, sizeof crash_disk);
    sts = diskio (OZ_IO_DISK_CRASH, sizeof crash_disk, &crash_disk, iopex, devex -> master_iochan);
    if (sts != OZ_SUCCESS) {
      oz_dev_vdfs_printk (iopex, "oz_dev_vdfs: error %u enabling crash disk\n", sts);
      return (sts);
    }

    /* Save file pointer and lock it in memory */

    crash_file = file;
    file -> refcount   ++;
    file -> refc_read  ++;
    file -> refc_write ++;

    /* Return file info to caller */

    efblk = file -> attrlock_efblk;
    if (file -> attrlock_efbyt == 0) efblk --;

    ((OZ_IO_fs_crash *)(iopex -> ap)) -> crashentry = disk_fs_crash;
    ((OZ_IO_fs_crash *)(iopex -> ap)) -> blocksize  = file -> volume -> devex -> blocksize;
    ((OZ_IO_fs_crash *)(iopex -> ap)) -> filesize   = efblk;
  }

  return (sts);
}

/************************************************************************/
/*									*/
/*  Parse name string							*/
/*									*/
/************************************************************************/

static uLong be_parse (Iopex *iopex, Chnex *chnex, Devex *devex)

{
  char rname[2*OZ_FS_MAXFNLEN];
  const char *fname;
  Fileid dirid;
  int fnamelen, rnamelen;
  uLong sts;

  sts = getdirid (devex -> volume, strlen (iopex -> u.parse.p.name), iopex -> u.parse.p.name, &dirid, &fnamelen, &fname, rname, iopex);
  if (sts == OZ_SUCCESS) {
    rnamelen = strlen (rname);
    if (rnamelen + fnamelen >= sizeof rname) sts = OZ_FILENAMETOOLONG;
    else {
      memcpy (rname + rnamelen, fname, fnamelen);
      rname[rnamelen+fnamelen] = 0;
      (*(devex -> vector -> returnspec)) (rname, iopex -> u.parse.p.rnamesize, iopex -> u.parse.p.rnamebuff, iopex -> u.parse.p.rnamesubs);
    }
  }

  return (sts);
}

/************************************************************************/
/*									*/
/*  Enqueue an record I/O request to channel and start it when ready	*/
/*									*/
/************************************************************************/

static uLong queuerecio (Iopex *iopex, Chnex *chnex, Devex *devex)

{
  File *file;
  uLong sts, vl;

  sts = startshortcut (chnex, devex, iopex);			// it is a 'shortcut' request, ie, don't allow channel to close on us
								// ... but we are not using the kernel thread to process the request
  if (sts == OZ_SUCCESS) {
    iopex -> next = NULL;					// it will go on end of the queue
    file = chnex -> file;
    sts = OZ_FILENOTOPEN;					// there has to be a file open to queue to
    if (file == NULL) finishortcut (chnex, devex, iopex);
    else {
      vl = oz_hw_smplock_wait (&(file -> recio_vl));		// lock the channel's record-io request queue
      if (file -> recio_ip == NULL) {				// see if there is q record-io request already in progress on the channel
        file -> recio_ip = iopex;				// if not, make this one the 'in progress' request
      } else {
        *(file -> recio_qt) = iopex;				// if so, put this request on the end of the queue
        file -> recio_qt = &(iopex -> next);
        iopex = NULL;
      }
      oz_hw_smplock_clr (&(file -> recio_vl), vl);		// unlock the queue
      if (iopex != NULL) {
        sts = (*(iopex -> backend)) (iopex, chnex, devex);	// we marked this one 'in progress' so start processing it
        if (sts != OZ_STARTED) finishrecio (iopex, sts, NULL);	// maybe it finished synchronously
      }
      sts = OZ_STARTED;
    }
  }
  return (sts);
}

/************************************************************************/
/*									*/
/*  Finish the 'in progress' recio requeust for the channel		*/
/*									*/
/************************************************************************/

static void finishrecio (Iopex *iopex, uLong status, void (*finentry) (void *iopexv, int finok, uLong *status_r))

{
  Chnex *chnex;
  Devex *devex;
  File *file;
  Iopex *newiopex;
  uLong sts, vl;

  chnex = iopex -> chnex;
  devex = iopex -> devex;
  file  = chnex -> file;
  sts   = status;

  // Complete the 'in progress' request and try to dequeue the next one

loop:
  vl = oz_hw_smplock_wait (&(file -> recio_vl));
  if (file -> recio_ip != iopex) oz_crash ("oz_dev_vdfs finishrecio: request %p not in progress (%p is)", iopex, file -> recio_ip);
  newiopex = file -> recio_qh;
  if (newiopex != NULL) {
    if ((file -> recio_qh = newiopex -> next) == NULL) file -> recio_qt = &(file -> recio_qh);
  }
  file -> recio_ip = newiopex;
  oz_hw_smplock_clr (&(file -> recio_vl), vl);

  finishortcut (chnex, devex, iopex);
  iodonex (iopex, sts, finentry, iopex);
  finentry = NULL;

  // See if there are any waiting in the queue

  iopex = newiopex;

  // If so, call its routine to start it going.  If it completes synchronously, then finish it off, too.

  if (iopex != NULL) {
    chnex = iopex -> chnex;
    sts   = (*(iopex -> backend)) (iopex, chnex, devex);
    if (sts != OZ_STARTED) goto loop;
  }
}

/************************************************************************/
/*									*/
/*  Start shortcut request.  Prevent a close from happening until 	*/
/*  finished.								*/
/*									*/
/************************************************************************/

static uLong startshortcut (Chnex *chnex, Devex *devex, Iopex *iopex)

{
#if 111
  Long shortcuts;
  uLong sts, vl;

  vl = oz_hw_smplock_wait (&(devex -> smplock_vl));
  sts = OZ_CLOSEINPROG;
  validate_shortcuts (chnex);
  if (chnex -> shortcuts >= 0) {
    chnex -> shortcuts ++;
    if (iopex -> shortcut_prev != NULL) oz_crash ("oz_dev_vdfs startshortcut: iopex %p -> shortcut_prev %p", iopex, iopex -> shortcut_prev);
    iopex -> shortcut_next = chnex -> shortcut_iopexs;
    iopex -> shortcut_prev = &(chnex -> shortcut_iopexs);
    if (iopex -> shortcut_next != NULL) iopex -> shortcut_next -> shortcut_prev = &(iopex -> shortcut_next);
    chnex -> shortcut_iopexs = iopex;
    sts = OZ_SUCCESS;
  }
  validate_shortcuts (chnex);
  oz_hw_smplock_clr (&(devex -> smplock_vl), vl);
  return (sts);
#else
  Long shortcuts;

  do {
    shortcuts = chnex -> shortcuts;				// sample the current count
    if (shortcuts < 0) return (OZ_CLOSEINPROG);			// give up if a close is in progress
  } while (!oz_hw_atomic_setif_long (&(chnex -> shortcuts), 	// increment iff it still is what we sampled two lines ago, 
                                     shortcuts + 1, 		// else, re-sample and try again
                                     shortcuts));
  return (OZ_SUCCESS);
#endif
}

/************************************************************************/
/*									*/
/*  Shortcut request is finished, allow channel to be closed.		*/
/*									*/
/************************************************************************/

static void finishortcut (Chnex *chnex, Devex *devex, Iopex *iopex)

{
#if 111
  Long shortcuts;
  uLong sts, vl;

  vl = oz_hw_smplock_wait (&(devex -> smplock_vl));
  if (iopex -> shortcut_prev == NULL) oz_crash ("oz_dev_vdfs finishortcut: iopex %p -> shortcut_prev null", iopex);
  validate_shortcuts (chnex);
  if (chnex -> shortcuts == 0) oz_crash ("oz_dev_vdfs finishortcut: chnex %p -> shortcuts is zero (iopex %p)", chnex, iopex);
  shortcuts = -- (chnex -> shortcuts);
  *(iopex -> shortcut_prev) = iopex -> shortcut_next;
  if (iopex -> shortcut_next != NULL) iopex -> shortcut_next -> shortcut_prev = iopex -> shortcut_prev;
  iopex -> shortcut_next = iopex;
  iopex -> shortcut_prev = NULL;
  validate_shortcuts (chnex);
  oz_hw_smplock_clr (&(devex -> smplock_vl), vl);
#else
  Long shortcuts;

  shortcuts = oz_hw_atomic_inc_long (&(chnex -> shortcuts), -1);
#endif
  if (shortcuts < 0) oz_knl_event_set (devex -> shortcutev, 1);
}

#if 111
static void validate_shortcuts (Chnex *chnex)

{
  Iopex *iopex, **liopex;
  Long n;

  n = 0;
  for (liopex = &(chnex -> shortcut_iopexs); (iopex = *liopex) != NULL; liopex = &(iopex -> shortcut_next)) {
    if (iopex -> shortcut_prev != liopex) oz_crash ("oz_dev_vdfs validate_shortcuts: corrupt list");
    n ++;
  }
  if ((chnex -> shortcuts >= 0) && (n != chnex -> shortcuts)) {
    oz_crash ("oz_dev_vdfs validate_shortcuts: n %d, chnex %p -> shortcuts %d", n, chnex, chnex -> shortcuts);
  }
  if ((chnex -> shortcuts < 0) && (n != chnex -> shortcuts + chnex -> closeshorts + 1)) {
    oz_crash ("oz_dev_vdfs validate_shortcuts: n %d, chnex %p -> shortcuts %d, closeshorts %d", n, chnex, chnex -> shortcuts, chnex -> closeshorts);
  }
}
#endif

/************************************************************************/
/*									*/
/*  Iodone wrapper routine to print out message for debugging		*/
/*									*/
/************************************************************************/

static void iodonex (Iopex *iopex, uLong status, void (*finentry) (void *finparam, int finok, uLong *status_r), void *finparam)

{
  oz_knl_iodone (iopex -> ioop, status, NULL, finentry, finparam);
}

/************************************************************************/
/*									*/
/*  Assign I/O channel for internal use to device by name		*/
/*									*/
/*    Input:								*/
/*									*/
/*	devname  = device name to assign channel to			*/
/*	lockmode = lock mode to assign channel at			*/
/*	iopex    = current i/o request					*/
/*									*/
/*    Output:								*/
/*									*/
/*	assign_by_name = OZ_SUCCESS : successful			*/
/*	                       else : error status			*/
/*	*iochan_r = i/o channel						*/
/*									*/
/************************************************************************/

static uLong assign_by_name (const char *devname, OZ_Lockmode lockmode, Iopex *iopex, OZ_Iochan **iochan_r)

{
  uLong sts;
  OZ_Devunit *devunit;
  OZ_Secaccmsk secaccmsk;
  OZ_Secattr *secattr;
  OZ_Seckeys *seckeys;
  OZ_Thread *thread;

  /* Look for disk device that volume is on */

  devunit = oz_knl_devunit_lookup (devname);
  if (devunit == NULL) return (OZ_BADDEVNAME);

  /* Make sure issuer of initialize or mount volume has access to the device */

  secaccmsk = OZ_SECACCMSK_LOOK;
  if (OZ_LOCK_ALLOW_TEST (lockmode, OZ_LOCK_ALLOWS_SELF_READ))  secaccmsk |= OZ_SECACCMSK_READ;
  if (OZ_LOCK_ALLOW_TEST (lockmode, OZ_LOCK_ALLOWS_SELF_WRITE)) secaccmsk |= OZ_SECACCMSK_WRITE;
  secattr = oz_knl_devunit_getsecattr (devunit);
  seckeys = NULL;
  thread  = oz_knl_ioop_getthread (iopex -> ioop);
  if (thread != NULL) seckeys = oz_knl_thread_getseckeys (thread);
  sts = oz_knl_security_check (secaccmsk, seckeys, secattr);

  /* If ok, create an I/O channel for my internal use to point to the device */

  if (sts == OZ_SUCCESS) sts = oz_knl_iochan_create (devunit, lockmode, OZ_PROCMODE_KNL, NULL, iochan_r);

  /* Release stuff and return status */

  oz_knl_devunit_increfc (devunit, -1);
  oz_knl_secattr_increfc (secattr, -1);
  oz_knl_seckeys_increfc (seckeys, -1);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Initialize a volume							*/
/*									*/
/*    Input:								*/
/*									*/
/*	volnamelen = length of volume name string			*/
/*	volname    = volume name string (not null terminated)		*/
/*	clusterfactor = cluster factor					*/
/*	secattrsize/buff = security attributes for the volume		*/
/*									*/
/*    Output:								*/
/*									*/
/*	init_volume = OZ_SUCCESS : successful				*/
/*	                    else : error status				*/
/*									*/
/************************************************************************/

static uLong init_volume (int volnamelen, const char *volname, uLong clusterfactor, uLong secattrsize, const void *secattrbuff, uLong initflags, Iopex *iopex)

{
  char c;
  Devex *devex;
  int i;
  uLong sts;
  OZ_IO_disk_getinfo1 disk_getinfo1;
  OZ_IO_disk_setvolvalid disk_setvolvalid;
  Volume *volume;

  devex = iopex -> devex;

  /* Make sure they give us sane stuff for the volume name */

  if (volnamelen >= devex -> vector -> volname_max) return (OZ_VOLNAMETOOLONG);
  if (volnamelen <= 0) return (OZ_BADVOLNAME);
  for (i = 0; i < volnamelen; i ++) {
    c = volname[i];
    if ((c < ' ') || (c == '(') || (c == ')') || (c >= 127) || (c == '/')) return (OZ_BADVOLNAME);
  }

  /* No structs to free on cleanup yet */

  volume = NULL;

  /* Set volume valid flag */

  memset (&disk_setvolvalid, 0, sizeof disk_setvolvalid);
  disk_setvolvalid.valid = 1;
  sts = diskio (OZ_IO_DISK_SETVOLVALID, sizeof disk_setvolvalid, &disk_setvolvalid, iopex, devex -> master_iochan);
  if (sts != OZ_SUCCESS) {
    oz_dev_vdfs_printk (iopex, "oz_dev_vdfs init_volume: error %u setting volume valid\n", sts);
    goto cleanup;
  }

  /* Get info from disk driver like blocksize and number of blocks */

  memset (&disk_getinfo1, 0, sizeof disk_getinfo1);
  sts = diskio (OZ_IO_DISK_GETINFO1, sizeof disk_getinfo1, &disk_getinfo1, iopex, devex -> master_iochan);
  if (sts != OZ_SUCCESS) {
    oz_dev_vdfs_printk (iopex, "oz_dev_vdfs init_volume: error %u getting disk info\n", sts);
    goto cleanup;
  }
  devex -> blocksize   = disk_getinfo1.blocksize;
  devex -> bufalign    = disk_getinfo1.bufalign;
  devex -> totalblocks = disk_getinfo1.totalblocks;

  /* Malloc a temp volume struct */

  volume = OZ_KNL_PGPMALLOQ (sizeof *volume);
  if (volume == NULL) {
    sts = OZ_EXQUOTAPGP;
    goto cleanup;
  }
  memset (volume, 0, sizeof *volume);
  volume -> devex = devex;

  /* Get bootblock(s) size and location */

  sts = oz_hw_bootblock_nblocks (&disk_getinfo1, devex -> master_iochan, &(volume -> bb_nblocks), &(volume -> bb_logblock));
  if (sts != OZ_SUCCESS) {
    oz_dev_vdfs_printk (iopex, "oz_dev_vdfs init_volume: error %u getting logical block location\n", sts);
    goto cleanup;
  }

  /* Format the filesystem on the volume */

  sts = (*(devex -> vector -> init_volume)) (devex, volume, volnamelen, volname, clusterfactor, secattrsize, secattrbuff, initflags, iopex);

  /* Clean up and return status - always turn drive offline but don't spin it down */

cleanup:
  if (volume != NULL) OZ_KNL_PGPFREE (volume);
  diskio (OZ_IO_DISK_SETVOLVALID, 0, NULL, iopex, devex -> master_iochan);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Mount a volume							*/
/*									*/
/*    Output:								*/
/*									*/
/*	mount_volume = OZ_SUCCESS : successful				*/
/*	                     else : error status			*/
/*	*volume_r = volume pointer					*/
/*									*/
/************************************************************************/

static uLong mount_volume (Volume **volume_r, uLong mountflags, Iopex *iopex)

{
  Devex *devex;
  Fileid fileid;
  OZ_IO_disk_getinfo1 disk_getinfo1;
  OZ_IO_disk_setvolvalid disk_setvolvalid;
  OZ_Secaccmsk secaccmsk;
  Volume *volume;
  uLong i, sts;
  uWord cksm;

  devex  = iopex -> devex;
  volume = NULL;

  /* Set volume valid flag */

  memset (&disk_setvolvalid, 0, sizeof disk_setvolvalid);
  disk_setvolvalid.valid = 1;
  sts = diskio (OZ_IO_DISK_SETVOLVALID, sizeof disk_setvolvalid, &disk_setvolvalid, iopex, devex -> master_iochan);
  if (sts != OZ_SUCCESS) {
    oz_dev_vdfs_printk (iopex, "oz_dev_vdfs mount_volume: error %u setting volume valid bit\n", sts);
    goto cleanup;
  }

  /* Get disk geometry */

  memset (&disk_getinfo1, 0, sizeof disk_getinfo1);
  sts = diskio (OZ_IO_DISK_GETINFO1, sizeof disk_getinfo1, &disk_getinfo1, iopex, devex -> master_iochan);
  if (sts != OZ_SUCCESS) {
    oz_dev_vdfs_printk (iopex, "oz_dev_vdfs mount_volume: error %u getting disk information\n", sts);
    goto cleanup;
  }
  devex -> blocksize   = disk_getinfo1.blocksize;
  devex -> bufalign    = disk_getinfo1.bufalign;
  devex -> totalblocks = disk_getinfo1.totalblocks;

  /* Malloc a volume struct */

  volume = OZ_KNL_PGPMALLOQ (sizeof *volume);
  if (volume == NULL) {
    sts = OZ_EXQUOTAPGP;
    goto cleanup;
  }
  memset (volume, 0, sizeof *volume);
  volume -> devex = devex;
  volume -> mountflags = mountflags;
  oz_hw_smplock_init (sizeof volume -> dirtyfiles_vl, &(volume -> dirtyfiles_vl), OZ_SMPLOCK_LEVEL_VL);
  devex -> volume = volume;

  /* Calculate home block location */

  sts = oz_hw_bootblock_nblocks (&disk_getinfo1, devex -> master_iochan, &(volume -> bb_nblocks), &(volume -> bb_logblock));
  if (sts != OZ_SUCCESS) {
    oz_dev_vdfs_printk (iopex, "oz_dev_vdfs mount_volume: error %u getting bootblock location\n", sts);
    goto cleanup;
  }

  /* Open the filesystem */

  sts = (*(devex -> vector -> mount_volume)) (devex, volume, mountflags, iopex);
  if (sts != OZ_SUCCESS) goto cleanup;

  /* Activate disk cache routines */

  if (!(mountflags & OZ_FS_MOUNTFLAG_NOCACHE)) {
    devex -> dcache = oz_knl_dcache_init (devex -> master_iochan, devex -> blocksize, revalidate, devex);
  }

  /* We're mounted */

  *volume_r = volume;
  return (OZ_SUCCESS);

  /* Something failed, clean up and return error status */

cleanup:
  if (volume != NULL) {
    volume -> mountflags |= OZ_FS_MOUNTFLAG_READONLY;
    dismount_volume (volume, 0, 0, iopex);
  }
  devex -> volume = NULL;
  return (sts);
}

/* Calculate volume's home block location */

static void calc_home_block (Volume *volume)

{
  if (volume -> bb_logblock > 0) volume -> hb_logblock = volume -> bb_logblock - 1;	/* if there's room just before the boot block, put it there */
  else volume -> hb_logblock = volume -> bb_logblock + volume -> bb_nblocks;		/* otherwise, put it just after the boot block */
}

/************************************************************************/
/*									*/
/*  Dismount volume							*/
/*									*/
/*    Input:								*/
/*									*/
/*	volume = volume to be dismounted				*/
/*	unload = 0 : leave volume online				*/
/*	         1 : unload volume (if possible)			*/
/*									*/
/*    Output:								*/
/*									*/
/*	volume dismounted						*/
/*									*/
/************************************************************************/

static uLong dismount_volume (Volume *volume, int unload, int shutdown, Iopex *iopex)

{
  Devex *devex;
  uLong sts;
  OZ_IO_disk_setvolvalid disk_setvolvalid;

  devex = iopex -> devex;

  sts = (*(devex -> vector -> dismount_volume)) (devex, volume, unload, shutdown, iopex);
  if (sts != OZ_SUCCESS) return (sts);

  /* Flush and deactivate cache */

  if (devex -> dcache != NULL) {
    oz_knl_dcache_term (devex -> dcache, 0);
    devex -> dcache = NULL;
  }

  /* Clear volume valid flag and maybe unload volume */

  memset (&disk_setvolvalid, 0, sizeof disk_setvolvalid);
  disk_setvolvalid.unload = unload;
  sts = diskio (OZ_IO_DISK_SETVOLVALID, sizeof disk_setvolvalid, &disk_setvolvalid, iopex, devex -> master_iochan);
  if (sts != OZ_SUCCESS) oz_dev_vdfs_printk (iopex, "oz_dev_vdfs dismount: error %u unloading volume\n", sts);

  /* Free off volume block */

  OZ_KNL_PGPFREE (volume);

  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Get directory id from filespec string				*/
/*									*/
/*    Input:								*/
/*									*/
/*	volume   = volume block pointer					*/
/*	fspeclen = length of filespec string				*/
/*	fspec    = filespec string (not null terminated)		*/
/*	           /<dir>/<dir>/.../<fname>[/]				*/
/*									*/
/*    Output:								*/
/*									*/
/*	getdirid = OZ_SUCCESS : success					*/
/*	                 else : error status				*/
/*	*dirid_r = directory-id						*/
/*	*fnamelen_r = length of <fname>[/] portion of input string	*/
/*	*fname_r = pointer to <fname>[/] portion of input string	*/
/*	*dname_r = filled in with actual directory name (null terminated)
/*									*/
/*    Note:								*/
/*									*/
/*	If input string ends with directory ./ or ../, the output 	*/
/*	*fname_r string will be null and *dname_r will contain the 	*/
/*	equivalent directory name string.  If input string ends with 	*/
/*	an actual directory name, like foo/, *fname_r will be 'foo/' 	*/
/*	and *dname_r will contain what came before foo/ in the input 	*/
/*	string.  This has the following desirable effects:		*/
/*									*/
/*	  1) You cannot create something ending in /./ or /../ because 	*/
/*	     enter_file requires a non-null fname (it doesn't make 	*/
/*	     sense to create the 'current' or 'parent' directory).	*/
/*	  2) You can open something ending in /./ or /../ because the 	*/
/*	     lookup routine will treat a null fname as meaning to open 	*/
/*	     the dname directory.					*/
/*	  3) You can either create or open a named directory because 	*/
/*	     its name will be returned in fname.	 		*/
/*									*/
/*	Likewise, if they supply simply "/" designating the root 	*/
/*	directory, we return "/" as the dname and a null fname.		*/
/*									*/
/************************************************************************/

typedef struct Dirid { struct Dirid *next;
                       Fileid dirid;
                       char *dirnm;
                     } Dirid;

static uLong getdirid (Volume *volume, int fspeclen, const char *fspec, Fileid *dirid_r, int *fnamelen_r, const char **fname_r, char *dname_r, Iopex *iopex)

{
  const char *e, *p, *q;
  char *d;
  Dirid *dirid, *dirids;
  File *dirfile;
  uLong sts;

  dirids = NULL;

  /* Make sure it starts with an '/' */

  p = fspec;
  e = fspec + fspeclen;
  if ((p == e) || (*p != '/')) {
    oz_dev_vdfs_printk (iopex, "oz_dev_vdfs getdirid: spec %*.*s doesn't start with /\n", fspeclen, fspeclen, fspec);
    return (OZ_BADFILENAME);
  }

  /* Find last occurrence of '//'.  Start at that point.  This will allow fs independent parsers to just */
  /* tack a full fspec to follow a default directory, ie, if the default directory is /h1/mike/, and     */
  /* the user specifies /etc/passwd, the parser can simply put /h1/mike//etc/passwd and get /etc/passwd. */
  /* If the user specifies .profile, the parser simply puts /h1/mike/.profile to get the desired file.   */

  for (q = p; q < e - 1; q ++) if ((q[0] == '/') && (q[1] == '/')) p = q + 1;
  p ++;								/* point at the name string (just past any /'s) */

  /* Start with the root directory id */

  sts = (*(volume -> devex -> vector -> get_rootdirid)) (volume -> devex, dirid_r);
  if (sts != OZ_SUCCESS) return (sts);

  d = dname_r;
  if (d != NULL) *(d ++) = '/';

  /* Keep repeating as long as there are more '/'s in the filespec */

  while ((q = memchr (p, '/', e - p)) != NULL) {		/* point q at the next slash */
    if ((q == p + 1) && (p[0] == '.')) {			/* './' is a no-op */
      p = q + 1;
      continue;
    }
    if ((q == p + 2) && (p[0] == '.') && (p[1] == '.')) {	/* '../' goes up one level, if not at root */
      if (dirids != NULL) {
        dirid    = dirids -> next;
        *dirid_r = dirids -> dirid;
        d        = dirids -> dirnm;
        OZ_KNL_PGPFREE (dirids);
        dirids   = dirid;
      }
      p = q + 1;
      continue;
    }
    if (q + 1 == e) break;					/* if the / is at the very end (and it's not ./ or ../) stop here */
								/* ... out of /foo/bar/, this returns bar/ as the filename (and /foo/ as the directory) */
    dirid = OZ_KNL_PGPMALLOQ (sizeof *dirid);			/* push the previous directory's directory id on stack in case of subsequent '../' */
    if (dirid == NULL) {
      sts = OZ_EXQUOTAPGP;
      goto cleanup;
    }
    dirid -> next  = dirids;
    dirid -> dirid = *dirid_r;
    dirid -> dirnm = d;
    dirids = dirid;
    sts = oz_dev_vdfs_open_file (volume, dirid_r, OZ_SECACCMSK_LOOK, &dirfile, iopex); /* open the previous directory (must be able to lookup a specific file) */
    if (sts != OZ_SUCCESS) goto cleanup;			/* abort if failed to open */
    q ++;							/* increment past the slash */
    sts = lookup_file (dirfile, q - p, p, dirid_r, NULL, iopex); /* lookup that file in the previous directory */
    oz_dev_vdfs_close_file (dirfile, iopex);			/* close previous directory */
    if (sts != OZ_SUCCESS) goto cleanup;			/* abort if failed to find new directory in previous directory */
    if (d != NULL) {
      sts = OZ_FILENAMETOOLONG;
      if (d - dname_r + q - p >= OZ_FS_MAXFNLEN) goto cleanup;
      memcpy (d, p, q - p);					/* accumulate returned directory name string */
      d += q - p;
    }
    p = q;							/* point to next thing in input string */
  }

  /* Return pointer to <fname> and return success status */

  *fnamelen_r = e - p;
  *fname_r = p;
  if (d != NULL) *d = 0;
  sts = OZ_SUCCESS;

cleanup:
  while ((dirid = dirids) != NULL) {
    dirids = dirid -> next;
    OZ_KNL_PGPFREE (dirid);
  }
  return (sts);
}

/************************************************************************/
/*									*/
/*  Lookup a file in a directory					*/
/*									*/
/*    Input:								*/
/*									*/
/*	dirfile = directory file					*/
/*	namelen = length of *name string				*/
/*	name    = name to lookup (not necessarily null terminated)	*/
/*									*/
/*    Output:								*/
/*									*/
/*	lookup_file = OZ_SUCCESS : successful				*/
/*	           OZ_NOSUCHFILE : entry not found			*/
/*	                    else : error status				*/
/*	*fileid_r = file-id of found file				*/
/*	*name_r   = name found						*/
/*									*/
/*    Note:								*/
/*									*/
/*	This routine does not do wildcard scanning, it just finds a 	*/
/*	particular file (like for an 'open' type request).		*/
/*									*/
/************************************************************************/

static uLong lookup_file (File *dirfile, int namelen, const char *name, Fileid *fileid_r, char *name_r, Iopex *iopex)

{
  return ((*(iopex -> devex -> vector -> lookup_file)) (dirfile, namelen, name, fileid_r, name_r, iopex));
}

/************************************************************************/
/*									*/
/*  Enter a file in a directory						*/
/*									*/
/*    Input:								*/
/*									*/
/*	dirfile    = directory file					*/
/*	dirname    = directory name (diag only)				*/
/*	namelen    = length of name to enter				*/
/*	name       = name to enter					*/
/*	newversion = make sure name is the highest version		*/
/*	file       = open file pointer (or NULL if not open)		*/
/*	fileid     = the file's id					*/
/*									*/
/*    Output:								*/
/*									*/
/*	enter_file = OZ_SUCCESS : successful				*/
/*	                   else : error status				*/
/*	*name_r = filled in with resultant name (incl version)		*/
/*									*/
/************************************************************************/

static uLong enter_file (File *dirfile, const char *dirname, int namelen, const char *name, int newversion, File *file, const Fileid *fileid, char *name_r, Iopex *iopex)

{
  return ((*(iopex -> devex -> vector -> enter_file)) (dirfile, dirname, namelen, name, newversion, file, fileid, name_r, iopex));
}

/************************************************************************/
/*									*/
/*  Remove a file from a directory					*/
/*									*/
/*    Input:								*/
/*									*/
/*	dirfile = directory file					*/
/*	name    = name to remove (must include absolute version number)	*/
/*									*/
/*    Output:								*/
/*									*/
/*	remove_file = OZ_SUCCESS : successful				*/
/*	                    else : error status				*/
/*									*/
/************************************************************************/

static uLong remove_file (File *dirfile, const char *name, char *name_r, Iopex *iopex)

{
  return ((*(iopex -> devex -> vector -> remove_file)) (dirfile, name, name_r, iopex));
}

/************************************************************************/
/*									*/
/*  This routine is called by thread routines to lock a directory for 	*/
/*  write.  It makes sure no shortcut routine is currently reading it.	*/
/*									*/
/*  Shortcut routines are not allowed to write a directory.		*/
/*									*/
/*  Thread routines do not need to lock the directory for read as 	*/
/*  only the thread is allowed to modify a directory.			*/
/*									*/
/************************************************************************/

void oz_dev_vdfs_lock_dirfile_for_write (File *dirfile)

{
  Long dirlockr;

  dirlockr = oz_hw_atomic_or_long (&(dirfile -> dirlockr), 0x80000000);			// flag writer active 
  if (dirlockr < 0) oz_crash ("oz_dev_vdfs_lock_dirfile_for_write: dirlockr was 0x%X", dirlockr); // crash if another writer is waiting
  if (dirlockr > 0) {									// see if any readers active
    do {
      oz_knl_event_waitone (dirfile -> dirlockf);					// if so, wait for readers to finish
      oz_knl_event_set (dirfile -> dirlockf, 0);
    } while (dirfile -> dirlockr & 0x7FFFFFFF);
  }
}

/* Unlock the directory to allow reading by shortcuts, and wake any waiting shortcuts */

void oz_dev_vdfs_unlk_dirfile_for_write (File *dirfile)

{
  oz_hw_atomic_and_long (&(dirfile -> dirlockr), 0x7FFFFFFF);
  oz_knl_event_set (dirfile -> dirlockf, 1);
}

/************************************************************************/
/*									*/
/*  Create file								*/
/*									*/
/*    Input:								*/
/*									*/
/*	volume  = volume struct pointer					*/
/*	namelen = length of file name string				*/
/*	name    = file name string (not null terminated)		*/
/*	filattrflags = file attribute flags				*/
/*	secattr = security attributes					*/
/*	dirid   = directory id						*/
/*									*/
/*    Output:								*/
/*									*/
/*	create_file = OZ_SUCCESS : successful creation			*/
/*	                    else : error status				*/
/*	*file_r = file block pointer					*/
/*									*/
/************************************************************************/

static uLong create_file (Volume *volume, int namelen, const char *name, uLong filattrflags, OZ_Secattr *secattr, Fileid *dirid, File **file_r, Fileid **fileid_r, Iopex *iopex)

{
  char c;
  const OZ_VDFS_Vector *vector;
  File *file;
  int i;
  OZ_Event *dirlockf;
  uLong secattrsize, sts;

  vector = volume -> devex -> vector;

  /* Make sure they give us sane stuff            */
  /* Directory names must have a slash at the end */

  if (namelen >= vector -> filename_max) return (OZ_FILENAMETOOLONG);
  if (namelen <= 0) return (OZ_BADFILENAME);
  if (((filattrflags & OZ_FS_FILATTRFLAG_DIRECTORY) != 0) ^ (name[namelen-1] == '/')) return (OZ_BADFILENAME);
  for (i = 0; i < namelen; i ++) {
    c = name[i];
    if ((c < ' ') || (c >= 127) || ((c == '/') && (i != namelen - 1))) return (OZ_BADFILENAME);
  }

  /* Allocate a file struct for it */

  file = OZ_KNL_PGPMALLOQ (sizeof *file);
  if (file == NULL) return (OZ_EXQUOTAPGP);
  memset (file, 0, sizeof *file);
  file -> volume = volume;

  /* Set up some file struct stuff */

  file -> secattr = secattr;
  file -> attrlock_efblk = 1;

  /* Create an empty file */

  sts = (*(vector -> create_file)) (volume, namelen, name, filattrflags, dirid, file, fileid_r, iopex);
  if (sts != OZ_SUCCESS) return (sts);

  /* If creating a directory, allocate an event flag for it */

  dirlockf = NULL;
  if (filattrflags & OZ_FS_FILATTRFLAG_DIRECTORY) {
    sts = oz_knl_event_create (14, "directory sync", NULL, &dirlockf);
    if (sts != OZ_SUCCESS) return (sts);
    oz_knl_event_set (dirlockf, 1);
  }

  /* Link it to volume's open file list and increment open file count    */
  /* Reset the read/write ref counts, caller will have to set as desired */

  file -> next = volume -> openfiles;
  file -> prev = &(volume -> openfiles);
  volume -> openfiles = file;
  if (file -> next != NULL) file -> next -> prev = &(file -> next);
  volume -> nopenfiles ++;
  file -> recio_qh = NULL;
  file -> recio_qt = &(file -> recio_qh);
  file -> attrlock_flags = 0;
  oz_hw_smplock_init (sizeof file -> recio_vl,    &(file -> recio_vl),    OZ_SMPLOCK_LEVEL_VL);
  oz_hw_smplock_init (sizeof file -> attrlock_vl, &(file -> attrlock_vl), OZ_SMPLOCK_LEVEL_VL);
  file -> refcount   = 1;
  file -> refc_read  = 0;
  file -> refc_write = 0;
  file -> secattr    = secattr;
  file -> dirlockf   = dirlockf;
  oz_knl_secattr_increfc (secattr, 1);
  *file_r = file;

  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Get secattrs to create the file with				*/
/*									*/
/*    Input:								*/
/*									*/
/*	iopex = requests iopex						*/
/*	secattrsize = secattrs supplied by user				*/
/*	secattrbuff = secattrs supplied by user				*/
/*									*/
/*    Output:								*/
/*									*/
/*	getcresecattr = OZ_SUCCESS : successful				*/
/*	                      else : error status			*/
/*	*secattr_r = pointer to secattr struct (ref count already incd)	*/
/*									*/
/************************************************************************/

static uLong getcresecattr (Iopex *iopex, uLong secattrsize, const void *secattrbuff, OZ_Secattr **secattr_r)

{
  OZ_Secattr *secattr;
  OZ_Seckeys *seckeys;
  OZ_Thread *thread;
  uLong sts;

  thread = oz_knl_ioop_getthread (iopex -> ioop);				/* get the user's thread */
  secattr = NULL;								/* assume 'kernel only' access */
  if (secattrbuff == NULL) {							/* see if user said to use the default */
    if (thread != NULL) secattr = oz_knl_thread_getdefcresecattr (thread);	/* if so, get default create secattrs for that thread */
    secattrsize = oz_knl_secattr_getsize (secattr);				/* and make sure it's not too long */
    if (secattrsize > iopex -> devex -> vector -> secattr_max) {
      oz_knl_secattr_increfc (secattr, -1);
      return (OZ_SECATTRTOOLONG);
    }
  } else {
    if (secattrsize > iopex -> devex -> vector -> secattr_max) return (OZ_SECATTRTOOLONG); /* user supplied, make sure not too long to fit in an header */
    sts = oz_knl_secattr_create (secattrsize, secattrbuff, NULL, &secattr);	/* save in secattr struct */
    if (sts != OZ_SUCCESS) return (sts);					/* return error status if corrupted */
  }
  seckeys = NULL;								/* now make sure caller would have full access */
  if (thread != NULL) seckeys = oz_knl_thread_getseckeys (thread);		/* ... to the file we're about to create */
  sts = oz_knl_security_check (OZ_SECACCMSK_LOOK | OZ_SECACCMSK_READ | OZ_SECACCMSK_WRITE, seckeys, secattr);
  if (sts == OZ_SUCCESS) *secattr_r = secattr;					/* ok, return pointer to the secattrs */
  else oz_knl_secattr_increfc (secattr, -1);					/* no access, free secattrs off */
  return (sts);									/* anyway, return status */
}

/************************************************************************/
/*									*/
/*  Open file by file id						*/
/*									*/
/*    Input:								*/
/*									*/
/*	volume    = volume struct pointer				*/
/*	fileid    = file id to be opened				*/
/*	secaccmsk = security access mask bits				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_dev_vdfs_open_file = OZ_SUCCESS : successful completion	*/
/*	                              else : error status		*/
/*	*file_r = pointer to file struct				*/
/*									*/
/************************************************************************/

uLong oz_dev_vdfs_open_file (Volume *volume, const Fileid *fileid, OZ_Secaccmsk secaccmsk, File **file_r, Iopex *iopex)

{
  const OZ_VDFS_Vector *vector;
  OZ_Seckeys *seckeys;
  OZ_Thread *thread;
  OZ_VDFS_File *file;
  uLong sts;

  vector  = iopex -> devex -> vector;
  *file_r = NULL;

  /* See if same file is already open */

  file = (*(vector -> findopenfile)) (volume, fileid);
  if (file != NULL) goto found_it;

  /* If not, read its header from disk */

  file = OZ_KNL_PGPMALLOQ (sizeof *file);				// malloc a file struct
  if (file == NULL) return (OZ_EXQUOTAPGP);				// return error if no quota left
  memset (file, 0, sizeof *file);					// clear the file struct
  file -> volume = volume;
  sts = (*(vector -> open_file)) (volume, fileid, file, iopex);		// open file, partially fill in file struct
  if (sts != OZ_SUCCESS) {
    OZ_KNL_PGPFREE (file);						// error opening, free off file struct
    return (sts);							// return error status
  }

  /* If it's a directory, set up the dirlockf event flag and set it to 'unlocked' state */

  if (IS_DIRECTORY (file)) {
    sts = oz_knl_event_create (14, "directory sync", NULL, &(file -> dirlockf));
    if (sts != OZ_SUCCESS) {
      (*(vector -> close_file)) (file, iopex);
      OZ_KNL_PGPFREE (file);
      return (sts);
    }
    oz_knl_event_set (file -> dirlockf, 1);
  }

  /* Set up various other things in file struct */

  file -> recio_qh = NULL;
  file -> recio_qt = &(file -> recio_qh);
  oz_hw_smplock_init (sizeof file -> recio_vl,    &(file -> recio_vl),    OZ_SMPLOCK_LEVEL_VL);
  oz_hw_smplock_init (sizeof file -> attrlock_vl, &(file -> attrlock_vl), OZ_SMPLOCK_LEVEL_VL);

  /* Link it to volume's open file list and increment volume's open file count */

  file -> next = volume -> openfiles;
  file -> prev = &(volume -> openfiles);
  volume -> openfiles = file;
  if (file -> next != NULL) file -> next -> prev = &(file -> next);
  volume -> nopenfiles ++;

  /* Now make sure the caller has a key that grants the access required by the secaccmsk bits */

found_it:
  file -> refcount ++;								// assume we'll be successful (gets decr'd by close_file if not)
  seckeys = NULL;								/* assume 'superman' if iopex->ioop->thread is NULL */
  thread  = oz_knl_ioop_getthread (iopex -> ioop);				/* get the thread, if NULL, it is a sysio */
  if (thread != NULL) seckeys = oz_knl_thread_getseckeys (thread);		/* get the corresponding security keys */
  sts = oz_knl_security_check (secaccmsk, seckeys, file -> secattr);		/* now check to see if they can access the file */
  oz_knl_seckeys_increfc (seckeys, -1);						/* all done with the keys */
  if (sts == OZ_SUCCESS) *file_r = file;					/* if successful, return file pointer */
  else oz_dev_vdfs_close_file (file, iopex);					/* else, close the file */

  return (sts);
}

/************************************************************************/
/*									*/
/*  Set file attributes							*/
/*									*/
/*    Input:								*/
/*									*/
/*	file = file to have the attributes set				*/
/*	numitems = number of elements in itemlist array			*/
/*	itemlist = array of items to set				*/
/*	iopex = I/O request being processed				*/
/*									*/
/*    Output:								*/
/*									*/
/*	set_file_attrs = completion status				*/
/*									*/
/************************************************************************/

static uLong set_file_attrs (File *file, uLong numitems, const OZ_Itmlst2 *itemlist, Iopex *iopex)

{
  return ((*(iopex -> devex -> vector -> set_file_attrs)) (file, numitems, itemlist, iopex));
}

/************************************************************************/
/*									*/
/*  Close file, delete if marked for delete				*/
/*									*/
/*    Input:								*/
/*									*/
/*	file = file to be closed					*/
/*									*/
/*    Output:								*/
/*									*/
/*	file is closed and possibly deleted				*/
/*									*/
/************************************************************************/

uLong oz_dev_vdfs_close_file (File *file, Iopex *iopex)

{
  uLong sts;
  Volume *volume;

  volume = file -> volume;

  /* Make sure there are no open channels to it.  If there are, just return success    */
  /* status.  When the other channels close, we will proceed with the close operation. */

  file -> refcount --;
  if (file -> refcount != 0) return (OZ_SUCCESS);

  /* Ref count zero (all channels closed) */

  /* Free off security attributes buffer */

  oz_knl_secattr_increfc (file -> secattr, -1);
  file -> secattr = NULL;

  /* If truncate pending, perform it */

  if (file -> truncpend) (*(iopex -> devex -> vector -> extend_file)) (file, file -> truncblocks, 0, iopex);

  /* If any attribute changes pending, mark header dirty */

  if (file -> attrlock_flags != 0) oz_dev_vdfs_mark_header_dirty (file);

  /* Flush out dirty headers so we know it is not on dirtyfiles list */

  oz_dev_vdfs_write_dirty_headers (volume, iopex);

  /* Unlink from volume's list of open files */

  *(file -> prev) = file -> next;
  if (file -> next != NULL) file -> next -> prev = file -> prev;
  volume -> nopenfiles --;

  /* Free off directory sync event flag */

  if (file -> dirlockf != NULL) oz_knl_event_increfc (file -> dirlockf, -1);

  /* Free off the filex stuff */

  sts = (*(iopex -> devex -> vector -> close_file)) (file, iopex);

  /* Free off the file struct */

  OZ_KNL_PGPFREE (file);

  return (sts);
}

/************************************************************************/
/*									*/
/*  Extend or truncate a file						*/
/*									*/
/*    Input:								*/
/*									*/
/*	file     = file block pointer of file to extended / truncated	*/
/*	nblocks  = new total number of blocks				*/
/*	extflags = EXTEND_NOTRUNC : don't truncate			*/
/*	          EXTEND_NOEXTHDR : no extension header			*/
/*									*/
/*    Output:								*/
/*									*/
/*	extend_file = OZ_SUCCESS : extend was successful		*/
/*	                    else : error status				*/
/*									*/
/************************************************************************/

static uLong extend_file (File *file, OZ_Dbn nblocks, uLong extflags, Iopex *iopex)

{
  return ((*(iopex -> devex -> vector -> extend_file)) (file, nblocks, extflags, iopex));
}

/************************************************************************/
/*									*/
/*  Mark file header is dirty so it will be written to disk		*/
/*									*/
/*    Input:								*/
/*									*/
/*	file = header block to write					*/
/*									*/
/*    Note:								*/
/*									*/
/*	This routine may be called from outside the kernel thread as 	*/
/*	it provides the required synchronization			*/
/*									*/
/************************************************************************/

void oz_dev_vdfs_mark_header_dirty (File *file)

{
  uLong vl;
  Volume *volume;

  volume = file -> volume;

  vl = oz_hw_smplock_wait (&(volume -> dirtyfiles_vl));

  if (!(file -> headerdirty)) {
    file -> headerdirty  = 1;
    file -> nextdirty    = volume -> dirtyfiles;
    volume -> dirtyfiles = file;
  }

  oz_hw_smplock_clr (&(volume -> dirtyfiles_vl), vl);

  (*(volume -> devex -> vector -> mark_header_dirty)) (file);
}

/* This routine is called by the thread to write out all headers that are marked dirty */

void oz_dev_vdfs_write_dirty_headers (Volume *volume, Iopex *iopex)

{
  File *dirtyfile;
  Long alf;
  OZ_Datebin now;
  uLong sts, vl;

checklist:
  vl = oz_hw_smplock_wait (&(volume -> dirtyfiles_vl));			// lock the list so oz_dev_vdfs_mark_header_dirty can't mess us up
  if ((dirtyfile = volume -> dirtyfiles) != NULL) {			// see if anything is on the list
    volume -> dirtyfiles = dirtyfile -> nextdirty;			// ok, unlink it
    dirtyfile -> nextdirty   = (void *)(0xDEADBEEF);
    dirtyfile -> headerdirty = 0;					// remember that it is not on the list anymore
    oz_hw_smplock_clr (&(volume -> dirtyfiles_vl), vl);			// let oz_dev_vdfs_mark_header_dirty modify list now
    vl  = oz_hw_smplock_wait (&(dirtyfile -> attrlock_vl));
    alf = dirtyfile -> attrlock_flags;					// see if any of these are set and reset them all
    now = dirtyfile -> attrlock_date;
    dirtyfile -> attrlock_flags = 0;
    oz_hw_smplock_clr (&(dirtyfile -> attrlock_vl), vl);
    sts = (*(volume -> devex -> vector -> write_dirty_header)) (dirtyfile, alf, now, volume, iopex);
    goto checklist;							// see if there are any more to write
  }
  oz_hw_smplock_clr (&(volume -> dirtyfiles_vl), vl);			// list is empty, release lock
}

/************************************************************************/
/*									*/
/*  Write homeblock if it is dirty					*/
/*									*/
/************************************************************************/

static void write_dirty_homeboy (Volume *volume, Iopex *iopex)

{
  uLong sts;

  if (volume -> dirty) {
    sts = (*(iopex -> devex -> vector -> writehomeblock)) (volume, iopex);
    if (sts != OZ_SUCCESS) {
      oz_dev_vdfs_printk (iopex, "oz_dev_vdfs write_dirty_homeboy: error %u writing homeblock\n", sts);
    }
    volume -> dirty = 0;
  }
}

/************************************************************************/
/*									*/
/*  Write segment of crash dump file					*/
/*									*/
/************************************************************************/

static uLong disk_fs_crash (void *dummy, OZ_Dbn vbn, uLong size, OZ_Mempage phypage, uLong offset)

{
  OZ_Dbn lbn, nblocks;
  OZ_VDFS_Vector *vector;
  uLong blocksize, sts, wsize;

  if (crash_file == NULL) return (OZ_FILENOTOPEN);

  blocksize = crash_disk.blocksize;						/* get file = disk's block size */
  nblocks   = 0;								/* we don't have any blocks mapped */
  while (size > 0) {								/* repeat as long as there's stuff to do */
    if (nblocks == 0) {								/* see if we need to map more lbn's */
      sts = (*(crash_file -> volume -> devex -> vector -> map_vbn_to_lbn)) (crash_file, vbn, &nblocks, &lbn); /* ok, convert vbn to lbn */
      if (sts != OZ_SUCCESS) {
        oz_knl_printk ("oz_dev_vdfs: error %u mapping crash file vbn %u\n", sts, vbn);
        return (sts);
      }
    }
    wsize = size;								/* try to write all that's left to do */
    if (wsize > nblocks * blocksize) wsize = nblocks * blocksize;		/* but don't write more than is mapped */
    sts = (*(crash_disk.crashentry)) (crash_disk.crashparam, lbn, wsize, phypage, offset); /* write to disk */
    if (sts != OZ_SUCCESS) {
      oz_knl_printk ("oz_dev_vdfs: error %u writing crash file vbn %u\n", sts, vbn);
      return (sts);
    }
    size    -= wsize;								/* subtract what was written */
    offset  += wsize;								/* increment page offset */
    phypage += offset >> OZ_HW_L2PAGESIZE;					/* add overflow to page number */
    offset  &= (1 << OZ_HW_L2PAGESIZE) - 1;					/* get offset within the page */

    wsize   /= blocksize;							/* see how many blocks were written */
    nblocks -= wsize;								/* decrement number of blocks mapped */
    lbn     += wsize;								/* increment the next lbn */
    vbn     += wsize;								/* increment the next vbn */
  }

  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Provide direct access to cache page					*/
/*									*/
/*    Input:								*/
/*									*/
/*	iochan = I/O channel that file is opened on			*/
/*	vbn    = virtual block number at start of cache page		*/
/*									*/
/*    Output:								*/
/*									*/
/*	vdfs_knlpfmap = OZ_SUCCESS : successful				*/
/*	                      else : error status			*/
/*	*phypage_r = physical page number of cache page			*/
/*									*/
/************************************************************************/

static uLong vdfs_knlpfmap (OZ_Iochan *iochan, OZ_Dbn vbn, OZ_Mempage *phypage_r)

{
  Devex *devex;
  File *file;
  OZ_Dbn lbn, nbl;
  uLong sts;

  file  = ((Chnex *)(oz_knl_iochan_ex (iochan))) -> file;
  devex = file -> volume -> devex;

  sts = (*(devex -> vector -> map_vbn_to_lbn)) (file, vbn, &nbl, &lbn);
  if ((sts == OZ_SUCCESS) && (nbl < ((1 << OZ_HW_L2PAGESIZE) / devex -> blocksize))) sts = OZ_BADBLOCKNUMBER;
  if (sts == OZ_SUCCESS) sts = oz_knl_dcache_pfmap (devex -> dcache, lbn, phypage_r);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Tell cache the page has been modified, so it needs to be written 	*/
/*  back out to disk							*/
/*									*/
/************************************************************************/

static uLong vdfs_knlpfupd (OZ_Iochan *iochan, OZ_Dbn vbn, OZ_Mempage phypage)

{
  Devex *devex;
  File *file;
  int writethru;
  OZ_Dbn lbn, nbl;
  uLong sts;

  file  = ((Chnex *)(oz_knl_iochan_ex (iochan))) -> file;
  devex = file -> volume -> devex;

  writethru = (*(devex -> vector -> vis_writethru)) (file -> volume) 
           || (*(devex -> vector -> fis_writethru)) (file, vbn, 0, OZ_KNL_CACHE_PAGESIZE, NULL);

  sts = (*(devex -> vector -> map_vbn_to_lbn)) (file, vbn, &nbl, &lbn);
  if ((sts == OZ_SUCCESS) && (nbl < ((1 << OZ_HW_L2PAGESIZE) / devex -> blocksize))) sts = OZ_BADBLOCKNUMBER;
  if (sts == OZ_SUCCESS) sts = oz_knl_dcache_pfupd (devex -> dcache, lbn, phypage, writethru);

  return (sts);
}

/************************************************************************/
/*									*/
/*  Release page back to cache						*/
/*									*/
/************************************************************************/

static void vdfs_knlpfrel (OZ_Iochan *iochan, OZ_Mempage phypage)

{
  File *file;

  file = ((Chnex *)(oz_knl_iochan_ex (iochan))) -> file;
  oz_knl_dcache_pfrel (file -> volume -> devex -> dcache, phypage);
}

/************************************************************************/
/*									*/
/*  Read virtual blocks from a file					*/
/*									*/
/*    Input:								*/
/*									*/
/*	file = file struct pointer					*/
/*	virtblock = starting virtual block number			*/
/*	blockoffs = byte offset in first virtual block			*/
/*	size = number of bytes to read					*/
/*	buff = buffer to read into					*/
/*	updates = 0 : don't change file access date			*/
/*	          1 : update file access date				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_dev_vdfs_readvirtblock = OZ_SUCCESS : all blocks successfully read
/*	                          OZ_ENDOFFILE : some blocks read	*/
/*	                                  else : I/O error status	*/
/*	*buff = filled in with data from disk				*/
/*									*/
/************************************************************************/

uLong oz_dev_vdfs_readvirtblock (File *file, OZ_Dbn virtblock, uLong blockoffs, uLong size, void *buff, Iopex *iopex, int updates)

{
  OZ_Dbn logblock, nblocks;
  uLong blocksize, size_read, size_to_read, sts, vl;

  blocksize = iopex -> devex -> blocksize;

  /* Loop through until we read as much as was asked for (or until we get an error) */

  size_read = 0;
  sts = OZ_SUCCESS;
  while (size > 0) {

    /* Make sure blockoffs is less than a block and adjust virtblock likewise */

    virtblock += blockoffs / blocksize;
    blockoffs %= blocksize;

    /* Map the requested vbn to an lbn, see how many contiguous blocks it maps */

    sts = (*(iopex -> devex -> vector -> map_vbn_to_lbn)) (file, virtblock, &nblocks, &logblock);
    if (sts != OZ_SUCCESS) break;

    /* Read the whole amount or just as much as is left to go. */
    /* This is convoluted in case nblocks*blocksize > 32 bits, */
    /* or size_to_read + blockoffs > 32 bits.                  */

    size_to_read = size;
    if ((size_to_read / blocksize >= nblocks) || ((size_to_read + blockoffs) / blocksize >= nblocks)) {
      size_to_read = nblocks * blocksize - blockoffs;
      if (size_to_read > size) size_to_read = size;
    }

    sts = oz_dev_vdfs_readlogblock (logblock, blockoffs, size_to_read, buff, iopex);
    if (sts != OZ_SUCCESS) break;

    /* Update by how much was read */

    size -= size_to_read;			/* decrement how much left to do */
    ((uByte *)buff) += size_to_read;		/* increment where to put it */
    blockoffs += size_to_read;			/* increment offset in starting block */
    size_read += size_to_read;			/* increment total processed so far */
  }

  /* Maybe update the last access date */

  if (updates && (size_read != 0) && !(file -> volume -> mountflags & OZ_FS_MOUNTFLAG_READONLY)) {
    vl = oz_hw_smplock_wait (&(file -> attrlock_vl));
    file -> attrlock_date   = oz_hw_tod_getnow ();
    file -> attrlock_flags |= OZ_VDFS_ALF_M_ADT;
    oz_hw_smplock_clr (&(file -> attrlock_vl), vl);
  }

  return (sts);
}

/************************************************************************/
/*									*/
/*  Read logical blocks from the disk					*/
/*									*/
/*    Input:								*/
/*									*/
/*	logblock = starting logical block number			*/
/*	size = number of bytes to read					*/
/*	buff = buffer to read into					*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_dev_vdfs_readlogblock = OZ_SUCCESS : block successfully read	*/
/*	                                 else : error status		*/
/*	*buff = filled in with data from disk				*/
/*									*/
/************************************************************************/

uLong oz_dev_vdfs_readlogblock (OZ_Dbn logblock, uLong blockoffs, uLong size, void *buff, Iopex *iopex)

{
  uByte *temp;
  Devex *devex;
  uLong blocksize, sts;
  OZ_IO_disk_readblocks disk_readblocks;

  devex = iopex -> devex;

  /* If cache is active, read from cache */

  if (devex -> dcache != NULL) sts = oz_knl_dcache_readw (devex -> dcache, size, buff, logblock, blockoffs);

  /* Otherwise, read directly from disk */

  else {
    blocksize  = devex -> blocksize;					/* get the block size */
    temp = NULL;
    while (size != 0) {
      logblock  += blockoffs / blocksize;				/* make sure blockoffs < blocksize and adjust logblock accordingly */
      blockoffs %= blocksize;
      if ((blockoffs != 0) || (size < blocksize) || (size & devex -> bufalign) || (((OZ_Pointer)buff) & devex -> bufalign)) {
        if (temp == NULL) {						/* if so, allocate temp block buffer */
          temp = OZ_KNL_NPPMALLOQ (blocksize);
          if (temp == NULL) return (OZ_EXQUOTANPP);
        }
        memset (&disk_readblocks, 0, sizeof disk_readblocks);		/* read the first logical block into temp buffer */
        disk_readblocks.size = blocksize;
        disk_readblocks.buff = temp;
        disk_readblocks.slbn = logblock;
        sts = diskio (OZ_IO_DISK_READBLOCKS, sizeof disk_readblocks, &disk_readblocks, iopex, devex -> master_iochan);
        if (sts != OZ_SUCCESS) goto readerror;
        sts = blocksize - blockoffs;					/* ok, see how many bytes after offset we have */
        if (sts > size) sts = size;					/* if that's more than caller wants, just get what caller wants */
        memcpy (buff, temp + blockoffs, sts);				/* copy to caller's buffer */
      } else {
        memset (&disk_readblocks, 0, sizeof disk_readblocks);		/* read logical blocks directly to caller's buffer */
        disk_readblocks.size = (size / blocksize) * blocksize;
        disk_readblocks.buff = buff;
        disk_readblocks.slbn = logblock;
        sts = diskio (OZ_IO_DISK_READBLOCKS, sizeof disk_readblocks, &disk_readblocks, iopex, devex -> master_iochan);
        if (sts != OZ_SUCCESS) goto readerror;
        sts = disk_readblocks.size;
      }
      size -= sts;							/* reduce size left to transfer */
      ((uByte *)buff) += sts;						/* increment pointer for rest of buffer */
      blockoffs += sts;							/* increment offset for next read */
    }
    sts = OZ_SUCCESS;
readerror:
    if (temp != NULL) OZ_KNL_NPPFREE (temp);				/* free temp buffer */
  }

  return (sts);
}

/************************************************************************/
/*									*/
/*  Write virtual blocks to a file					*/
/*									*/
/*    Input:								*/
/*									*/
/*	file = file struct pointer					*/
/*	virtblock = starting virtual block number			*/
/*	size = number of bytes to write					*/
/*	buff = buffer to write from					*/
/*	updates = 0 : don't change file modification dates		*/
/*	          1 : update file modification dates			*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_dev_vdfs_writevirtblock = OZ_SUCCESS : block(s) successfully written
/*	                                   else : error status		*/
/*									*/
/************************************************************************/

uLong oz_dev_vdfs_writevirtblock (File *file, OZ_Dbn virtblock, uLong blockoffs, uLong size, const void *buff, Iopex *iopex, int updates)

{
  int writethru;
  OZ_Dbn logblock, nblocks;
  uLong blocksize, size_written, size_to_write, sts, vl;

  blocksize = iopex -> devex -> blocksize;
  writethru = (*(iopex -> devex -> vector -> fis_writethru)) (file, virtblock, blockoffs, size, buff);

  /* Loop through until we write all the data */

  size_written = 0;
  sts = OZ_SUCCESS;
  while (size > 0) {

    /* Make sure blockoffs is less than a block and adjust virtblock likewise */

    virtblock += blockoffs / blocksize;
    blockoffs %= blocksize;

    /* Map the requested vbn to an lbn, see how many contiguous blocks it maps */

    sts = (*(iopex -> devex -> vector -> map_vbn_to_lbn)) (file, virtblock, &nblocks, &logblock);
    if (sts != OZ_SUCCESS) break;

    /* Write the whole amount or just as much as is left to go. */
    /* This is convoluted in case nblocks*blocksize > 32 bits,  */
    /* or size_to_write + blockoffs > 32 bits.                  */

    size_to_write = size;
    if ((size_to_write / blocksize >= nblocks) || ((size_to_write + blockoffs) / blocksize >= nblocks)) {
      size_to_write = nblocks * blocksize - blockoffs;
      if (size_to_write > size) size_to_write = size;
    }

    sts = oz_dev_vdfs_writelogblock (logblock, blockoffs, size_to_write, buff, writethru, iopex);
    if (sts != OZ_SUCCESS) break;

    /* Increment buffer pointer by how much was written */

    size            -= size_to_write;		/* decrement how much left to do */
    ((uByte *)buff) += size_to_write;		/* increment where to put it */
    blockoffs       += size_to_write;		/* increment offset in starting block */
    size_written    += size_to_write;		/* increment total processed so far */
  }

  /* Maybe update the last modification date */

  if (updates && (size_written != 0)) {
    vl = oz_hw_smplock_wait (&(file -> attrlock_vl));
    file -> attrlock_date   = oz_hw_tod_getnow ();
    file -> attrlock_flags |= OZ_VDFS_ALF_M_MDT | OZ_VDFS_ALF_M_CDT | OZ_VDFS_ALF_M_ADT;
    oz_hw_smplock_clr (&(file -> attrlock_vl), vl);
  }

  return (sts);
}

/************************************************************************/
/*									*/
/*  Write logical blocks to a disk					*/
/*									*/
/*    Input:								*/
/*									*/
/*	virtblock = starting virtual block number			*/
/*	size = number of bytes to write					*/
/*	buff = buffer to write from					*/
/*	writethru = 0 : tell dcache to write it back whenever		*/
/*	            1 : tell dcache to write it immediately		*/
/*									*/
/*    Output:								*/
/*									*/
/*	writelogblock = OZ_SUCCESS : block(s) successfully written	*/
/*	                      else : error status			*/
/*									*/
/************************************************************************/

uLong oz_dev_vdfs_writelogblock (OZ_Dbn logblock, uLong blockoffs, uLong size, const void *buff, int writethru, Iopex *iopex)

{
  uByte *temp;
  Devex *devex;
  OZ_IO_disk_readblocks disk_readblocks;
  OZ_IO_disk_writeblocks disk_writeblocks;
  uLong blocksize, sts;

  devex = iopex -> devex;
  if ((devex -> volume != NULL) && (*(devex -> vector -> vis_writethru)) (devex -> volume)) writethru = 1;

  /* If cache is active, use it */

  if (devex -> dcache != NULL) sts = oz_knl_dcache_writew (devex -> dcache, size, buff, logblock, blockoffs, writethru);

  /* Otherwise, write directly to disk */

  else {
    temp = NULL;							/* haven't used temp buffer yet */
    blocksize  = devex -> blocksize;					/* get the block size */
    logblock  += blockoffs / blocksize;					/* make sure blockoffs < blocksize and adjust logblock accordingly */
    blockoffs %= blocksize;
    sts = OZ_SUCCESS;							/* in case size and blockoffs are both zero */

    /* If starting in the middle of a block or unaligned transfer, read that whole block, modify the bytes and write it back out */

    while ((size != 0) && ((blockoffs != 0) || (size & devex -> bufalign) || (((OZ_Pointer)buff) & devex -> bufalign))) {
      if (temp == NULL) {						/* if so, allocate temp block buffer */
        temp = OZ_KNL_NPPMALLOQ (blocksize);
        if (temp == NULL) return (OZ_EXQUOTANPP);
      }
      if ((blockoffs != 0) || (size < blocksize)) {
        memset (&disk_readblocks, 0, sizeof disk_readblocks);		/* read the first logical block into temp buffer */
        disk_readblocks.size = blocksize;
        disk_readblocks.buff = temp;
        disk_readblocks.slbn = logblock;
        sts = diskio (OZ_IO_DISK_READBLOCKS, sizeof disk_readblocks, &disk_readblocks, iopex, devex -> master_iochan);
        if (sts != OZ_SUCCESS) goto writedone;
      }
      sts = blocksize - blockoffs;					/* ok, see how many bytes after offset we have in the temp buffer */
      if (sts > size) sts = size;					/* if that's more than caller has, just modify what caller has */
      memcpy (temp + blockoffs, buff, sts);				/* copy from caller's buffer */
      size -= sts;							/* reduce size left to transfer */
      ((uByte *)buff) += sts;						/* increment pointer for rest of buffer */
      memset (&disk_writeblocks, 0, sizeof disk_writeblocks);		/* write the first logical block back out from temp buffer */
      disk_writeblocks.size = blocksize;
      disk_writeblocks.buff = temp;
      disk_writeblocks.slbn = logblock;
      sts = diskio (OZ_IO_DISK_WRITEBLOCKS, sizeof disk_writeblocks, &disk_writeblocks, iopex, devex -> master_iochan);
      if (sts != OZ_SUCCESS) goto writedone;
      logblock ++;							/* increment block number for rest of buffer */
      blockoffs = 0;							/* start at very beginning of next block */
    }									/* repeat in case of unaligned transfer */

    /* Directly write any whole blocks */

    if (size >= blocksize) {
      memset (&disk_writeblocks, 0, sizeof disk_writeblocks);		/* write whole logical blocks directly from caller's buffer */
      disk_writeblocks.size = (size / blocksize) * blocksize;
      disk_writeblocks.buff = buff;
      disk_writeblocks.slbn = logblock;
      sts = diskio (OZ_IO_DISK_WRITEBLOCKS, sizeof disk_writeblocks, &disk_writeblocks, iopex, devex -> master_iochan);
      if (sts != OZ_SUCCESS) goto writedone;
      size -= disk_writeblocks.size;					/* decrement size remaining */
      ((uByte *)buff) += disk_writeblocks.size;				/* increment buffer pointer */
      logblock += disk_writeblocks.size / blocksize;			/* increment block number */
    }

    /* If writing partial last block, read the whole block, modify the bytes and write it back out */

    if (size != 0) {							/* see if there is a last partial block */
      if (temp == NULL) {						/* if so, allocate temp block buffer */
        temp = OZ_KNL_NPPMALLOQ (blocksize);
        if (temp == NULL) return (OZ_EXQUOTANPP);
      }
      memset (&disk_readblocks, 0, sizeof disk_readblocks);		/* read the last logical block into temp buffer */
      disk_readblocks.size = blocksize;
      disk_readblocks.buff = temp;
      disk_readblocks.slbn = logblock;
      sts = diskio (OZ_IO_DISK_READBLOCKS, sizeof disk_readblocks, &disk_readblocks, iopex, devex -> master_iochan);
      if (sts != OZ_SUCCESS) goto writedone;
      memcpy (temp, buff, size);					/* copy last bit from caller's buffer */
      memset (&disk_writeblocks, 0, sizeof disk_writeblocks);		/* write the last logical block back out from temp buffer */
      disk_writeblocks.size = blocksize;
      disk_writeblocks.buff = temp;
      disk_writeblocks.slbn = logblock;
      sts = diskio (OZ_IO_DISK_WRITEBLOCKS, sizeof disk_writeblocks, &disk_writeblocks, iopex, devex -> master_iochan);
    }
writedone:
    if (temp != NULL) OZ_KNL_NPPFREE (temp);				/* free temp buffer off if there is one */
  }

  return (sts);
}

/************************************************************************/
/*									*/
/*  Perform virtual-to-logical translation for an oz_knl_dcache_map 	*/
/*  parameter block							*/
/*									*/
/*    Input:								*/
/*									*/
/*	dcmpb -> virtblock = virtual block number to read/write		*/
/*	dcmpb -> nbytes    = number of bytes to read/write		*/
/*	dcmpb -> blockoffs = byte offset in first virtual block		*/
/*	dcmpb -> param     = iopex					*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_dev_vdfs_dcache_map_vbn_to_lbn = OZ_SUCCESS : translation performed
/*	                                          else : error status	*/
/*	dcmp -> nbytes    = possibly reduced if file is fragmented	*/
/*	dcmp -> blockoffs = normalized to disk's block size		*/
/*	dcmp -> virtblock = possibly incremented from normalization	*/
/*	dcmp -> logblock  = corresponding logical block number		*/
/*									*/
/************************************************************************/

uLong oz_dev_vdfs_dcache_map_vbn_to_lbn (OZ_Dcmpb *dcmpb, File *file)

{
  Iopex *iopex;
  OZ_Dbn nblocks;
  uLong blocksize, sts;

  if (dcmpb -> nbytes == 0) return (OZ_SUCCESS);							// avoid eof error

  iopex = dcmpb -> param;										// point to I/O parameter extension block
  blocksize = iopex -> devex -> blocksize;								// get disk's block size
  dcmpb -> virtblock += dcmpb -> blockoffs / blocksize;							// normalize blockoffs to blocksize
  dcmpb -> blockoffs %= blocksize;
  sts = (*(iopex -> devex -> vector -> map_vbn_to_lbn)) (file, dcmpb -> virtblock, &nblocks, &(dcmpb -> logblock)); // translate virtblock to logblock

  if (dcmpb -> ix4kbuk && (sts == OZ_SUCCESS)) {							// check IX db 4k bucket read
    if (dcmpb -> blockoffs != 0) {
      oz_knl_printk ("oz_dev_vdfs_dcache_map_vbn_to_lbn: ix4kbuk blockoffs %u\n", dcmpb -> blockoffs);	// must start on a disk block boundary
      dcmpb -> ix4kbuk = 0;
    } else if ((dcmpb -> logblock % 8) != 0) {
      oz_knl_printk ("oz_dev_vdfs_dcache_map_vbn_to_lbn: ix4kbuk logblock %u\n", dcmpb -> logblock);	// must start on a disk page boundary
      dcmpb -> ix4kbuk = 0;
    } else if (nblocks < 8) {
      oz_knl_printk ("oz_dev_vdfs_dcache_map_vbn_to_lbn: ix4kbuk nblocks %u\n", nblocks);		// must have at least a page of blocks
      dcmpb -> ix4kbuk = 0;
    } else if (dcmpb -> nbytes != 4096) {
      oz_knl_printk ("oz_dev_vdfs_dcache_map_vbn_to_lbn: ix4kbuk nbytes %u\n", dcmpb -> nbytes);	// must be reading exactly one page
      dcmpb -> ix4kbuk = 0;
    }
  }

  if ((sts == OZ_SUCCESS) && ((dcmpb -> nbytes + dcmpb -> blockoffs) / blocksize >= nblocks)) {		// see if request crosses a file fragment
    dcmpb -> nbytes = nblocks * blocksize - dcmpb -> blockoffs;						// if so, truncate requested number of bytes
  }
  return (sts);
}

/************************************************************************/
/*									*/
/*  Process disk cache map blocks request				*/
/*									*/
/*    Input:								*/
/*									*/
/*	dcmpb = as needed for oz_knl_dcache_map				*/
/*									*/
/*    Output:								*/
/*									*/
/*	same as oz_knl_dcache_map					*/
/*									*/
/*    Note:								*/
/*									*/
/*	This routine just calls oz_knl_dcache_map if caching is 	*/
/*	enabled on the volume.  Otherwise, it emulates it.		*/
/*									*/
/************************************************************************/

static uLong dcache_map_blocks_read_start (OZ_Dcmpb *dcmpb);
static void dcache_map_blocks_read_done (void *dcmpbv, uLong status);
static void dcache_map_blocks_write_done (void *dcmpbv, uLong status);

uLong oz_dev_vdfs_dcache_map_blocks (OZ_Dcmpb *dcmpb)

{
  Devex *devex;
  uLong sts;

  devex = ((Iopex *)(dcmpb -> param)) -> devex;

  dcmpb -> dcache = devex -> dcache;				// set up dcache context
  if (dcmpb -> dcache != NULL) {
    sts = oz_knl_dcache_map (dcmpb);				// there is one set up, call oz_knl_dcache_map to process request
  } else {
    dcmpb -> cachepagex = OZ_KNL_PCMALLOC (devex -> blocksize);	// there isn't one, allocate a temp block buffer
    sts = dcache_map_blocks_read_start (dcmpb);			// start a disk read to read into it
  }
}

static uLong dcache_map_blocks_read_start (OZ_Dcmpb *dcmpb)

{
  OZ_Dbn logblock;
  OZ_IO_disk_readblocks disk_readblocks;
  OZ_IO_disk_writeblocks disk_writeblocks;
  uLong blocksize, modified, sts;

  blocksize = ((Iopex *)(dcmpb -> param)) -> devex -> blocksize;
  sts = OZ_SUCCESS;
  while (dcmpb -> nbytes > 0) {

    /* If they want to process beyond the end of the block, chop them off at end of the block */

    if (dcmpb -> nbytes > blocksize - dcmpb -> blockoffs) dcmpb -> nbytes = blocksize - dcmpb -> blockoffs;

    /* Get physical address of temp block buffer and point to the requested byte within */

    sts = oz_knl_misc_sva2pa (dcmpb -> cachepagex, &(dcmpb -> phypage), &(dcmpb -> pageoffs));
    if (sts < blocksize) oz_crash ("oz_dev_vdfs dcache_map_read_blocks_start: temp buffer %p not phys contig", dcmpb -> cachepagex);
    dcmpb -> pageoffs += dcmpb -> blockoffs;

    /* Start a read going if they aren't writing the whole block */

    if (!(dcmpb -> writing) || (dcmpb -> blockoffs != 0) || (dcmpb -> nbytes != blocksize)) {
      memset (&disk_readblocks, 0, sizeof disk_readblocks);
      disk_readblocks.size = blocksize;
      disk_readblocks.buff = dcmpb -> cachepagex;
      disk_readblocks.slbn = dcmpb -> logblock;
      sts = oz_knl_iostart3 (1, NULL, ((Iopex *)(dcmpb -> param)) -> devex -> master_iochan, 
                             OZ_PROCMODE_KNL, dcache_map_blocks_read_done, dcmpb, NULL, NULL, NULL, NULL, 
                             OZ_IO_DISK_READBLOCKS, sizeof disk_readblocks, &disk_readblocks);
      if (sts != OZ_SUCCESS) break;
    }

    /* The read completed synchronously (or wasn't required) */

    /* Call the requestor's completion routine to process data in the temp block buffer */

    logblock = dcmpb -> logblock;
    modified = (*(dcmpb -> entry)) (dcmpb, OZ_PENDING);

    /* If they modified the data, write it out to disk.  If not, pretend we did. */

    if (modified != 0) {
      memset (&disk_writeblocks, 0, sizeof disk_writeblocks);
      disk_writeblocks.size = blocksize;
      disk_writeblocks.buff = dcmpb -> cachepagex;
      disk_writeblocks.slbn = logblock;
      sts = oz_knl_iostart3 (1, NULL, ((Iopex *)(dcmpb -> param)) -> devex -> master_iochan, 
                             OZ_PROCMODE_KNL, dcache_map_blocks_write_done, dcmpb, NULL, NULL, NULL, NULL, 
                             OZ_IO_DISK_WRITEBLOCKS, sizeof disk_writeblocks, &disk_writeblocks);
      if (sts != OZ_SUCCESS) break;
    }
  }

  /* We get here if either nbytes is zero or a disk I/O had an error or will complete asynchronously */

  if (sts != OZ_STARTED) OZ_KNL_NPPFREE (dcmpb -> cachepagex);
  return (sts);
}

/* The temp block has been read in from disk */

static void dcache_map_blocks_read_done (void *dcmpbv, uLong status)

{
  OZ_Dbn logblock;
  OZ_Dcmpb *dcmpb;
  OZ_IO_disk_writeblocks disk_writeblocks;
  uLong modified, sts;

  dcmpb = dcmpbv;

  /* If read failure, tell caller then exit */

  if (status != OZ_SUCCESS) {
    (*(dcmpb -> entry)) (dcmpb, status);
    OZ_KNL_NPPFREE (dcmpb -> cachepagex);
    return;
  }

  /* Call the caller's completion routine to process data in the temp block buffer */

  logblock = dcmpb -> logblock;
  modified = (*(dcmpb -> entry)) (dcmpb, OZ_PENDING);

  /* If they modified the data, write it out to disk.  If not, pretend we did. */

  sts = OZ_SUCCESS;
  if (modified != 0) {
    memset (&disk_writeblocks, 0, sizeof disk_writeblocks);
    disk_writeblocks.size = ((Iopex *)(dcmpb -> param)) -> devex -> blocksize;
    disk_writeblocks.buff = dcmpb -> cachepagex;
    disk_writeblocks.slbn = logblock;
    sts = oz_knl_iostart3 (1, NULL, ((Iopex *)(dcmpb -> param)) -> devex -> master_iochan, 
                           OZ_PROCMODE_KNL, dcache_map_blocks_write_done, dcmpb, NULL, NULL, NULL, NULL, 
                           OZ_IO_DISK_WRITEBLOCKS, sizeof disk_writeblocks, &disk_writeblocks);
    if (sts == OZ_STARTED) return;
  }

  /* Write completed synchronously, call completion routine */

  dcache_map_blocks_write_done (dcmpb, sts);
}

/* The temp block has been written back out to disk */

static void dcache_map_blocks_write_done (void *dcmpbv, uLong status)

{
  OZ_Dcmpb *dcmpb;
  uLong sts;

  dcmpb = dcmpbv;

  if (status != OZ_SUCCESS) oz_dev_vdfs_printk (dcmpb -> param, "oz_dev_vdfs dcache_map_blocks_write_done: error %u writing logblock %u\n", status, dcmpb -> logblock);

  sts = dcache_map_blocks_read_start (dcmpb);				// start reading next block
  if (sts != OZ_STARTED) {						// check for synchronous completion
    (*(dcmpb -> entry)) (dcmpb, sts);					// ok, do final callback
  }
}

/************************************************************************/
/*									*/
/*  Perform an i/o operation and wait for it to complete		*/
/*									*/
/*    Input:								*/
/*									*/
/*	funcode = function code						*/
/*	as = argument block size					*/
/*	ap = argument block pointer					*/
/*	iopex = original i/o request (NULL if none)			*/
/*	iochan = disk i/o channel (normally devex -> master_iochan)	*/
/*									*/
/*    Output:								*/
/*									*/
/*	diskio = queuing / completion status				*/
/*									*/
/************************************************************************/

static uLong diskio (uLong funcode, uLong as, void *ap, Iopex *iopex, OZ_Iochan *iochan)

{
  uLong sts;
  OZ_Event *event;
  volatile uLong status;

  if (iopex != NULL) event = iopex -> devex -> ioevent;		/* just use thread's event flag for I/O waits */
  else {
    sts = oz_knl_event_create (21, "oz_dev_dfs diskio", NULL, &event);
    if (sts != OZ_SUCCESS) return (sts);
  }
  status = OZ_PENDING;						/* I/O operation not complete yet */
  sts = oz_knl_iostart (iochan, OZ_PROCMODE_KNL, NULL, NULL, &status, event, NULL, NULL, funcode, as, ap);
  if (sts == OZ_STARTED) {
    while ((sts = status) == OZ_PENDING) {			/* asynchronous, see if complete yet */
      oz_knl_event_waitone (event);				/* wait for event flag if not */
      oz_knl_event_set (event, 0);				/* clear in case we have to wait again */
    }
  }
  if (iopex == NULL) oz_knl_event_increfc (event, -1);
  return (sts);
}

/************************************************************************/
/*									*/
/*  This routine is called by the dcache routines and it revalidates 	*/
/*  that the volume in the drive is actually the one we want.  It is 	*/
/*  called when the disk driver returns OZ_VOLNOTVALID status 		*/
/*  (indicating the volume was removed and/or switched).		*/
/*									*/
/*    Input:								*/
/*									*/
/*	volume = volume to be revalidated				*/
/*									*/
/*    Output:								*/
/*									*/
/*	revalidate = OZ_SUCCESS : retry the failed I/O operation	*/
/*	                   else : return this error status		*/
/*									*/
/************************************************************************/

#if 00
static uLong revalidate (void *devexv, OZ_Dcache *dcache)

{
  int i, retries;
  uLong sts;
  OZ_Datebin datebin;
  OZ_IO_disk_readblocks disk_readblocks;
  OZ_IO_disk_setvolvalid disk_setvolvalid;
  OZ_FS_Homeblock newhomeblock;
  OZ_Timer *timer;
  Volume *volume;
  uWord cksm;

  retries = revalretries;
  volume  = volumev;

  /* Set hardware's volume valid bit and see if media is online */

retry:
  memset (&disk_setvolvalid, 0, sizeof disk_setvolvalid);
  disk_setvolvalid.valid = 1;
  sts = diskio (OZ_IO_DISK_SETVOLVALID, sizeof disk_setvolvalid, &disk_setvolvalid, NULL, devex -> master_iochan);
  if (sts != OZ_SUCCESS) goto error;

  /* Volume is online, read the home block directly from disk (bypassing cache) and make sure it matches the old one */

  memset (&disk_readblocks, 0, sizeof disk_readblocks);
  disk_readblocks.size = sizeof newhomeblock;
  disk_readblocks.buff = &newhomeblock;
  disk_readblocks.slbn = volume -> hb_logblock;
  sts = diskio (OZ_IO_DISK_READBLOCKS, sizeof disk_readblocks, &disk_readblocks, NULL, devex -> master_iochan);
  if (sts != OZ_SUCCESS) goto error;

  sts = OZ_BADHOMEBLKVER;
  if (newhomeblock.homeversion != HOMEBLOCK_VERSION) goto error;
  cksm = 0;
  for (i = 0; i < sizeof newhomeblock / sizeof (uWord); i ++) {
    cksm += ((uWord *)&(newhomeblock))[i];
  }
  sts = OZ_BADHOMEBLKCKSM;
  if (cksm != 0) goto error;
  sts = OZ_SUCCESS;
  if (memcmp (&newhomeblock, &(volume -> homeblock), sizeof newhomeblock) == 0) return (sts);

  /* Something failed, should we wait or bomb off? */

error:
  memset (&disk_setvolvalid, 0, sizeof disk_setvolvalid);
  diskio (OZ_IO_DISK_SETVOLVALID, sizeof disk_setvolvalid, &disk_setvolvalid, NULL, devex -> master_iochan);

  if (sts == OZ_SUCCESS) oz_knl_printk ("oz_dev_vdfs revalidate: volume %s is offline, volume %s is in drive\n", volume -> homeblock.volname, newhomeblock.volname);
  else oz_knl_printk ("oz_dev_vdfs revalidate: volume %s is offline, error %u\n", volume -> homeblock.volname, sts);

  if (-- retries <= 0) {
    if (sts == OZ_SUCCESS) sts = OZ_VOLNOTVALID;
    return (sts);
  }

  oz_knl_kthread_increfc (iopex -> devex -> kthread, 1);
  timer   = oz_knl_timer_alloc (NULL);
  datebin = oz_hw_tod_getnow ();
  OZ_HW_DATEBIN_ADD (datebin, datebin, revalinterval);
  oz_knl_timer_insert (timer, datebin, revaltimer, iopex -> devex -> kthread);
  oz_knl_kthread_wait (iopex -> devex -> kthread);
  goto retry;
}

/* Timer expired, wake kernel thread and decrement its ref count */

static void revaltimer (void *kthreadv, OZ_Timer *timer)

{
  oz_knl_timer_free (timer);
  oz_knl_kthread_waked (kthreadv);
}
#else
static uLong revalidate (void *devexv, OZ_Dcache *dcache)

{
  return (OZ_VOLNOTVALID);
}
#endif

/************************************************************************/
/*									*/
/*  Print error message to both OZ_ERROR and the kernel console		*/
/*									*/
/************************************************************************/

void oz_dev_vdfs_printk (Iopex *iopex, const char *format, ...)

{
  va_list ap;

  va_start (ap, format);
  oz_dev_vdfs_vprintk (iopex, format, ap);
  va_end (ap);
}

void oz_dev_vdfs_vprintk (Iopex *iopex, const char *format, va_list ap)

{
  char buff[256];
  uLong sts;
  OZ_Iochan *iochan;
  OZ_Logname *logname, *logtable;
  OZ_Process *process;
  OZ_Thread *thread;

  /* Print to console */

  oz_knl_printkv (format, ap);

  /* Get process table that the I/O request came from */

  thread = oz_knl_ioop_getthread (iopex -> ioop);
  if (thread == NULL) return;
  process = oz_knl_thread_getprocess (thread);
  if (process == NULL) return;
  logtable = oz_knl_process_getlognamtbl (process);
  if (logtable == NULL) return;

  /* Find OZ_ERROR logical name */

  sts = oz_knl_logname_lookup (logtable, OZ_PROCMODE_KNL, 8, "OZ_ERROR", NULL, NULL, NULL, NULL, &logname, NULL);
  if (sts != OZ_SUCCESS) {
    if (sts != OZ_NOLOGNAME) oz_knl_printk ("oz_dev_vdfs printk: error %u looking up OZ_ERROR\n", sts);
    return;
  }

  /* Get I/O channel assigned to OZ_ERROR */

  sts = oz_knl_logname_getobj (logname, 0, OZ_OBJTYPE_IOCHAN, &iochan);
  oz_knl_logname_increfc (logname, -1);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_dev_vdfs printk: error %u converting OZ_ERROR to io channel\n", sts);
    return;
  }

  /* Output message to OZ_ERROR console */

  oz_sys_vxprintf (startprintk, iochan, sizeof buff, buff, NULL, format, ap);
  oz_knl_iochan_increfc (iochan, -1);
}

/* Output to OZ_ERROR only */

static void printe (Iopex *iopex, const char *format, ...)

{
  char buff[256];
  uLong sts;
  OZ_Iochan *iochan;
  OZ_Logname *logname, *logtable;
  OZ_Process *process;
  OZ_Thread *thread;
  va_list ap;

  va_start (ap, format);

  /* Get process table that the I/O request came from */

  thread = oz_knl_ioop_getthread (iopex -> ioop);
  if (thread == NULL) return;
  process = oz_knl_thread_getprocess (thread);
  if (process == NULL) return;
  logtable = oz_knl_process_getlognamtbl (process);
  if (logtable == NULL) return;

  /* Find OZ_ERROR logical name */

  sts = oz_knl_logname_lookup (logtable, OZ_PROCMODE_KNL, 8, "OZ_ERROR", NULL, NULL, NULL, NULL, &logname, NULL);
  if (sts != OZ_SUCCESS) {
    if (sts != OZ_NOLOGNAME) oz_knl_printk ("oz_dev_vdfs printk: error %u looking up OZ_ERROR\n", sts);
    return;
  }

  /* Get I/O channel assigned to OZ_ERROR */

  sts = oz_knl_logname_getobj (logname, 0, OZ_OBJTYPE_IOCHAN, &iochan);
  oz_knl_logname_increfc (logname, -1);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_dev_vdfs printk: error %u converting OZ_ERROR to io channel\n", sts);
    return;
  }

  /* Output message to OZ_ERROR console */

  oz_sys_vxprintf (startprintk, iochan, sizeof buff, buff, NULL, format, ap);
  oz_knl_iochan_increfc (iochan, -1);

  va_end (ap);
}

static uLong startprintk (void *iochanv, uLong *size, char **buff)

{
  uLong sts;
  OZ_IO_console_write console_write;

  /* Use CONSOLE_WRITE function so we can't possibly be writing to ourself */

  memset (&console_write, 0, sizeof console_write);
  console_write.size = *size;
  console_write.buff = *buff;
  sts = oz_knl_io (iochanv, OZ_IO_CONSOLE_WRITE, sizeof console_write, &console_write);
  if (sts != OZ_SUCCESS) oz_knl_printk ("oz_dev_vdfs printk: error %u writing to OZ_ERROR\n", sts);
  return (sts);
}
