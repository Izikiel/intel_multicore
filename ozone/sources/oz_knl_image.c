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
/*  This routine loads an image into the memory of the current process	*/
/*									*/
/************************************************************************/

#define _OZ_KNL_IMAGE_C

#include "ozone.h"

#include "oz_io_fs.h"
#include "oz_knl_devio.h"
#include "oz_knl_event.h"
#include "oz_knl_handle.h"
#include "oz_knl_hw.h"
#include "oz_knl_image.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_lock.h"
#include "oz_knl_printk.h"
#include "oz_knl_process.h"
#include "oz_knl_sdata.h"
#include "oz_knl_section.h"
#include "oz_knl_security.h"
#include "oz_knl_status.h"
#include "oz_sys_io_fs.h"
#include "oz_sys_io_fs_printf.h"

#define MAX_IMAGE_LEVELS (16)

struct OZ_Image { OZ_Objtype objtype;		/* object type OZ_OBJTYPE_IMAGE */
                  OZ_Image *next;		/* next image in list */
                  OZ_Imagelist *imagelist;	/* imagelist this image is in */
                  OZ_Procmode mprocmode;	/* procmode of memory structs (OZ_PROCMODE_KNL or _SYS) */
                  void *baseaddr;		/* this image's base address */
                  void *startaddr;		/* this image's start address */
                  volatile Long refcount;	/* number of references to this image */
                  OZ_Image_Refd_image *refd_images; /* what images this image references */
                  OZ_Image_Secload *secloads;	/* sections that are loaded */
                  OZ_Image_Hand *imagehand;	/* pointer to image format specific handler entrypoint table */
                  void *imagex;			/* image extension struct (defined by image format specific routine) */
                  OZ_Secattr *secattr;		/* who can access me */
                  char name[1];			/* image name (must be last in struct) */
                };

struct OZ_Imagelist { OZ_Image *images;		/* list of images that are loaded */
                      OZ_Event *event;		/* locking event flag: 0=someone is accessing 'images' list; 1=no one is accessing list */
                    };

globalref OZ_Image_Hand oz_knl_image_oz;

static OZ_Image_Hand *imagehands[] = { &oz_knl_image_oz, NULL };

static uLong open_image_file (const char *filename, OZ_Iochan **iochan_r);
static uLong iowait (OZ_Event *ioevent, OZ_Iochan *iochan, uLong funcode, uLong as, void *ap);
static void lockimagelist (OZ_Imagelist *imagelist);
static void unlkimagelist (OZ_Imagelist *imagelist);

/************************************************************************/
/*									*/
/*  Load image in memory (if it isn't already loaded)			*/
/*									*/
/*    Input:								*/
/*									*/
/*	procmode  = processor mode to load it for			*/
/*	imagename = imagename, eg, LIBRTL				*/
/*	sysimage  = 0 : loading a process symbol			*/
/*	            1 : loading a system image				*/
/*	            2 : loading a system image, but don't actually 	*/
/*	                create sections or map the image		*/
/*	                (used to load the kernel image's symbol table)	*/
/*	level     = outermost call should be a zero			*/
/*									*/
/*	ipl <= softint							*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_image_load = OZ_SUCCESS : successfully loaded		*/
/*	                          else : load error			*/
/*	image added to the 'images' list				*/
/*	*baseaddr_r  = base address of loaded image (lowest mem addr used)
/*	*startaddr_r = image's start address				*/
/*	*image_r     = pointer to image struct				*/
/*	               only valid in loading process context		*/
/*									*/
/************************************************************************/

uLong oz_knl_image_load (OZ_Procmode procmode, char *imagename, int sysimage, int level, void **baseaddr_r, void **startaddr_r, OZ_Image **image_r)

{
  char *imageloaddir;
  const OZ_Logvalue *logvalues;
  int i;
  OZ_Image *image;
  OZ_Image_Args *imageargs;
  OZ_Image_Hand *imagehand;
  OZ_Image_Refd_image *refd_image;
  OZ_Image_Secload *secload;
  OZ_Imagelist *imagelist, *sysimagelist;
  OZ_IO_fs_getinfo1 fs_getinfo1;
  OZ_IO_fs_getsecattr fs_getsecattr;
  OZ_Pdata *pdata;
  OZ_Process *process;
  OZ_Procmode mprocmode;
  uLong logindex, rlen, sts;

  /* If too many levels, abort */

  if (level >= MAX_IMAGE_LEVELS) return (OZ_EXMAXIMGLEVEL);

  /* If system image, load in system image list.  Otherwise, load in process image list. */

  if (sysimage == 0) {								// see if loading image in system area
    process   = oz_knl_thread_getprocesscur ();					// if not, load it in current process area
    mprocmode = OZ_PROCMODE_KNL;						// use process-private kernel memory for its structs
  }
  else if (procmode == OZ_PROCMODE_KNL) {					// can only do kernel mode images in system area
    process   = oz_s_systemproc;						// ok, load it in system area
    mprocmode = OZ_PROCMODE_SYS;						// use system-global kernel memory for its structs
  }
  else return (OZ_KERNELONLY);

  /* Point to image list (or create one if not there) */

  pdata     = oz_sys_pdata_pointer (mprocmode);					// point to image's data
  imagelist = pdata -> imagelist;						// get current imagelist struct
  if (imagelist == NULL) {
    imagelist = oz_sys_pdata_malloc (mprocmode, sizeof *imagelist);		// none there, allocate one
    if (imagelist == NULL) return (OZ_NOMEMORY);
    imagelist -> images = NULL;							// doesn't have any images loaded
    sts = oz_knl_event_create (21, "oz_knl_imagelist lock", NULL, &(imagelist -> event)); // create lock event flag
    if (sts != OZ_SUCCESS) {
      oz_sys_pdata_free (mprocmode, imagelist);
      return (sts);
    }
    unlkimagelist (imagelist);							// mark it unlocked
    if (!oz_hw_atomic_setif_ptr ((void *volatile *)&(pdata -> imagelist), imagelist, NULL)) { // try to set this as new one
      oz_knl_event_increfc (imagelist -> event, -1);				// someone else got there first, free it off
      oz_sys_pdata_free (mprocmode, imagelist);
      imagelist = pdata -> imagelist;						// ... and use the other one
    }
  }

  /* If level 0, lock the image list so others can't access or modify it */

  if (level == 0) lockimagelist (imagelist);

  /* Init stuff that we clean up at the end */

  sts = OZ_NOMEMORY;
  imageargs = oz_sys_pdata_malloc (OZ_PROCMODE_KNL, sizeof *imageargs);
  if (imageargs == NULL) goto cleanup;
  memset (imageargs, 0, sizeof *imageargs);
  imageargs -> imagename = imagename;

  memset (&fs_getinfo1,   0, sizeof fs_getinfo1);
  memset (&fs_getsecattr, 0, sizeof fs_getsecattr);

  /* See if the image is already loaded.  If so, just return the info directly. */

  sts = OZ_SUCCESS;
  for (image = imagelist -> images; image != NULL; image = image -> next) {
    if (strcmp (image -> name, imagename) == 0) goto return_info;
  }

  /* If we just scanned the process' imagelist and couldn't find it, scan the system's imagelist, too */

  if (sysimage == 0) {
    sysimagelist = OZ_SYS_PDATA_SYSTEM -> imagelist;
    lockimagelist (sysimagelist);
    for (image = sysimagelist -> images; image != NULL; image = image -> next) {	/* scan system image list */
      if (strcmp (image -> name, imagename) == 0) {					/* see if the name matches */
        OZ_HW_ATOMIC_INCBY1_LONG (image -> refcount);					/* if so, increment ref count before releasing lock */
        unlkimagelist (sysimagelist);							/* unlock system image list */
        goto return_info_ni;								/* go return the info without incrementing ref count again */
      }
    }
    unlkimagelist (sysimagelist);							/* not found, unlock system image list */
  }

  /* Create an event flag for I/O */

  sts = oz_knl_event_create (25, "oz_knl_image_load ioevent", NULL, &(imageargs -> ioevent));
  if (sts != OZ_SUCCESS) goto cleanup;

  /* Open the image */

  sts = open_image_file (imagename, &(imageargs -> iochan));
  if (sts != OZ_SUCCESS) goto cleanup;

  /* Get the security attributes from the section file to apply to the section */

  sts = iowait (imageargs -> ioevent, imageargs -> iochan, OZ_IO_FS_GETINFO1, sizeof fs_getinfo1, &fs_getinfo1);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printerror ("oz_knl_image_load: error %u getting section file info for image %s\n", sts, imagename);
    goto cleanup;
  }
  rlen = 0;
  if (fs_getinfo1.secattrsize != 0) {
    fs_getsecattr.size = fs_getinfo1.secattrsize;
    fs_getsecattr.buff = oz_sys_pdata_malloc (OZ_PROCMODE_KNL, fs_getinfo1.secattrsize);
    if (fs_getsecattr.buff == NULL) {
      oz_sys_io_fs_printerror ("oz_knl_image_load: %s: no memory for %u byte security attributes\n", imagename, fs_getinfo1.secattrsize);
      sts = OZ_NOMEMORY;
      goto cleanup;
    }
    fs_getsecattr.rlen = &rlen;
    sts = iowait (imageargs -> ioevent, imageargs -> iochan, OZ_IO_FS_GETSECATTR, sizeof fs_getsecattr, &fs_getsecattr);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printerror ("oz_knl_image_load: error %u getting security attributes for image %s\n", sts, imagename);
      goto cleanup;
    }
  }
  sts = oz_knl_secattr_create (rlen, fs_getsecattr.buff, NULL, &(imageargs -> secattr));
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printerror ("oz_knl_image_load: error %u creating security attributes for image %s\n", sts, imagename);
    goto cleanup;
  }

  /* Read the base image header into imageargs -> hdrbuf */

  sts = oz_knl_image_read (imageargs, 1, sizeof imageargs -> hdrbuf, imageargs -> hdrbuf);
  if (sts != OZ_SUCCESS) goto cleanup;

  /* Loop through list of image loaders until we find one than can process it */

  imageargs -> sysimage    = sysimage;
  imageargs -> level       = level;
  imageargs -> procmode    = procmode;
  imageargs -> mprocmode   = mprocmode;
  imageargs -> fs_getinfo1 = &fs_getinfo1;

  for (i = 0; (imagehand = imagehands[i]) != NULL; i ++) {
    imageargs -> baseaddr  = NULL;				/* clear the base address */
    imageargs -> startaddr = NULL;				/* clear the start address */
    sts = (*(imagehand -> load)) (imageargs);			/* try this image loader to see if it works */
    if (sts == OZ_SUCCESS) goto successful;			/* if successful, we're all done */
    while ((secload = imageargs -> secloads) != NULL) {		/* otherwise release any loaded sections it did */
      imageargs -> secloads = secload -> next;
      oz_knl_process_unmapsec (secload -> svpage);
      oz_knl_section_increfc (secload -> section, -1);
      oz_sys_pdata_free (mprocmode, secload);
    }
    while ((refd_image = imageargs -> refd_images) != NULL) {	/* ... and release any referenced images */
      imageargs -> refd_images = refd_image -> next;
      oz_knl_image_increfc (refd_image -> image, -1);
      oz_sys_pdata_free (mprocmode, refd_image);
    }
    if (sts != OZ_UNKIMAGEFMT) goto cleanup;			/* if funky error, we abort */
  }
  sts = OZ_UNKIMAGEFMT;						/* nothing can load it, it must be unknown image format */
  goto cleanup;

  /* Now that we're loaded, make up the image struct */

successful:
  image = oz_sys_pdata_malloc (mprocmode, strlen (imagename) + sizeof *image);
  if (image == NULL) {
    sts = OZ_NOMEMORY;
    goto cleanup;
  }
  image -> objtype     = OZ_OBJTYPE_IMAGE;
  image -> next        = imagelist -> images;
  image -> imagelist   = imagelist;
  image -> baseaddr    = imageargs -> baseaddr;
  image -> startaddr   = imageargs -> startaddr;
  image -> refcount    = 0;
  image -> refd_images = imageargs -> refd_images;
  image -> secloads    = imageargs -> secloads;
  image -> imagex      = imageargs -> imagex;
  image -> imagehand   = imagehand;
  image -> mprocmode   = mprocmode;
  image -> secattr     = oz_knl_thread_getdefcresecattr (NULL);
  strcpy (image -> name, imagename);

  imagelist -> images = image;

  imageargs -> refd_images = NULL;
  imageargs -> secloads    = NULL;

  /* Inc ref count and return requested information about the image */

return_info:
  OZ_HW_ATOMIC_INCBY1_LONG (image -> refcount);
return_info_ni:
  *image_r = image;
  if (baseaddr_r  != NULL) *baseaddr_r  = image -> baseaddr;
  if (startaddr_r != NULL) *startaddr_r = image -> startaddr;

  /* Return status after cleaning up */

cleanup:
  if (fs_getsecattr.buff != NULL) oz_sys_pdata_free (OZ_PROCMODE_KNL, fs_getsecattr.buff);
  if (imageargs != NULL) {
    if (imageargs -> secattr != NULL) oz_knl_secattr_increfc (imageargs -> secattr, -1);
    if (imageargs -> iochan  != NULL) oz_knl_iochan_increfc  (imageargs -> iochan,  -1);
    if (imageargs -> ioevent != NULL) oz_knl_event_increfc   (imageargs -> ioevent, -1);
    oz_sys_pdata_free (OZ_PROCMODE_KNL, imageargs);
  }
  if (level == 0) unlkimagelist (imagelist);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Lookup global symbol in image					*/
/*									*/
/************************************************************************/

int oz_knl_image_lookup (OZ_Image *image, char *symname, OZ_Pointer *symvalu)

{
  /* Call the handler's specific lookup routine */

  OZ_KNL_CHKOBJTYPE (image, OZ_OBJTYPE_IMAGE);
  return ((*(image -> imagehand -> lookup)) (image -> imagex, symname, symvalu));
}

/************************************************************************/
/*									*/
/*  Scan image symbol table						*/
/*									*/
/************************************************************************/

void *oz_knl_image_symscan (OZ_Image *image, void *lastsym, char **symname_r, OZ_Pointer *symvalu_r)

{
  OZ_KNL_CHKOBJTYPE (image, OZ_OBJTYPE_IMAGE);
  return ((*(image -> imagehand -> symscan)) (image -> imagex, lastsym, symname_r, symvalu_r));
}

/************************************************************************/
/*									*/
/*  Increment image ref count						*/
/*  Image gets unloaded when ref count goes zero			*/
/*									*/
/************************************************************************/

Long oz_knl_image_increfc (OZ_Image *image, Long inc)

{
  Long refc;
  OZ_Image **limage, *ximage;
  OZ_Imagelist *imagelist;
  OZ_Image_Refd_image *refd_image;
  OZ_Image_Secload *secload;

  OZ_KNL_CHKOBJTYPE (image, OZ_OBJTYPE_IMAGE);

  /* Decrement ref count.  If negative, crash.  If positive, return.  If zero, unload the image then return. */

again:
  do {
    refc = image -> refcount;
    if (refc + inc <= 0) goto going_le_zero;
  } while (!oz_hw_atomic_setif_long (&(image -> refcount), refc + inc, refc));
  return (refc + inc);

going_le_zero:
  if (refc + inc < 0) oz_crash ("oz_knl_image_increfc: %s ref count negative (%d+%d)", image -> name, refc, inc);
  imagelist = image -> imagelist;
  lockimagelist (imagelist);
  if (!oz_hw_atomic_setif_long (&(image -> refcount), 0, refc)) {
    unlkimagelist (imagelist);
    goto again;
  }

  /* Unlink the image from the image list */

  for (limage = &(imagelist -> images); (ximage = *limage) != image; limage = &(ximage -> next)) {
    if (ximage == NULL) oz_crash ("image %s not found in its image list", image -> name);
  }
  *limage = ximage -> next;

  /* Unmap and delete all sections this image has mapped */

  while ((secload = image -> secloads) != NULL) {
    image -> secloads = secload -> next;
    if (secload -> seclock != NULL) oz_knl_section_iounlk (secload -> seclock);
    oz_knl_process_unmapsec (secload -> svpage);
    oz_knl_section_increfc (secload -> section, -1);
    oz_sys_pdata_free (image -> mprocmode, secload);
  }

  /* Unlock now so we can call our self to unload other images on the list   */
  /* This is ok because 'image' is the only pointer to the image struct left */

  unlkimagelist (imagelist);

  /* Unload all images this image refers to */

  while ((refd_image = image -> refd_images) != NULL) {
    image -> refd_images = refd_image -> next;
    oz_knl_image_increfc (refd_image -> image, -1);
    oz_sys_pdata_free (image -> mprocmode, refd_image);
  }

  /* Call the handler's specific unload routine */

  (*(image -> imagehand -> unload)) (image -> imagex, image -> mprocmode);

  /* Finally, free off the main image block */

  oz_sys_pdata_free (image -> mprocmode, image);
  return (0);
}

/************************************************************************/
/*									*/
/*  Lock all of the image's sections in memory				*/
/*									*/
/************************************************************************/

uLong oz_knl_image_lockinmem (OZ_Image *image, OZ_Procmode procmode)

{
  OZ_Image_Secload *secload;
  uLong sts;

  OZ_KNL_CHKOBJTYPE (image, OZ_OBJTYPE_IMAGE);

  for (secload = image -> secloads; secload != NULL; secload = secload -> next) {	// loop through the image's sections
    if (secload -> seclock != NULL) continue;						// if already locked, skip it
    sts = oz_knl_section_iolock (procmode, 						// try to lock section in memory
                                 secload -> npages << OZ_HW_L2PAGESIZE, 		// - this many bytes
                                 OZ_HW_VPAGETOVADDR (secload -> svpage), 		// - starting at this address
                                 secload -> writable, 					// - maybe make sure it's read/write
                                 &(secload -> seclock), 				// - this is the unlock key
                                 NULL, NULL, NULL);					// - we don't care about physical pages
    if (sts != OZ_SUCCESS) return (sts);						// abort if error, maybe it's been unmapped
  }
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Get image's security attributes					*/
/*									*/
/************************************************************************/

OZ_Secattr *oz_knl_image_getsecattr (OZ_Image *image)

{
  OZ_KNL_CHKOBJTYPE (image, OZ_OBJTYPE_IMAGE);
  oz_knl_secattr_increfc (image -> secattr, 1);
  return (image -> secattr);
}

/************************************************************************/
/*									*/
/*  Open image file and return i/o channel				*/
/*									*/
/************************************************************************/

static uLong open_image_file (const char *filename, OZ_Iochan **iochan_r)

{
  uLong sts;
  OZ_Handle h_image_file;
  OZ_IO_fs_open fs_open;

  *iochan_r = NULL;
  memset (&fs_open, 0, sizeof fs_open);
  fs_open.name = filename;
  fs_open.lockmode = OZ_LOCKMODE_PR;
  sts = oz_sys_io_fs_open2 (sizeof fs_open, &fs_open, 0, "OZ_IMAGE_DIR", &h_image_file);
  if (sts == OZ_SUCCESS) {
    sts = oz_knl_handle_takeout (h_image_file, OZ_PROCMODE_KNL, OZ_SECACCMSK_READ, OZ_OBJTYPE_IOCHAN, iochan_r, NULL);
    if (sts != OZ_SUCCESS) oz_crash ("oz_knl_image_load: image file handle lookup error %u", sts);
    oz_knl_iochan_increfc (*iochan_r, 1);
    oz_knl_handle_putback (h_image_file);
    oz_knl_handle_release (h_image_file, OZ_PROCMODE_KNL);
  } else if (sts != OZ_NOSUCHFILE) {
    oz_sys_io_fs_printerror ("oz_knl_image_load: error %u opening file %s in OZ_IMAGE_DIR\n", sts, filename);
  }
  return (sts);
}

/************************************************************************/
/*									*/
/*  Read a number of bytes from the image file starting at a given vbn	*/
/*									*/
/*    Input:								*/
/*									*/
/*	imageargs = image arg list pointer				*/
/*	vbn  = starting virtual block to read from			*/
/*	offs = offset within that block to start reading		*/
/*	size = number of bytes to read					*/
/*	buff = where to put the data					*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_image_read = OZ_SUCCESS : successful			*/
/*	                          else : error status			*/
/*	*buff = filled in with data					*/
/*									*/
/************************************************************************/

uLong oz_knl_image_read (OZ_Image_Args *imageargs, OZ_Dbn vbn, uLong size, void *buff)

{
  return (oz_knl_image_read2 (imageargs, vbn, 0, size, buff));
}

uLong oz_knl_image_read2 (OZ_Image_Args *imageargs, OZ_Dbn vbn, uLong offs, uLong size, void *buff)

{
  uLong sts;
  OZ_IO_fs_readblocks fs_readblocks;

  memset (&fs_readblocks, 0, sizeof fs_readblocks);
  fs_readblocks.size = size;
  fs_readblocks.buff = buff;
  fs_readblocks.svbn = vbn;
  fs_readblocks.offs = offs;
  sts = iowait (imageargs -> ioevent, imageargs -> iochan, OZ_IO_FS_READBLOCKS, sizeof fs_readblocks, &fs_readblocks);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printerror ("oz_knl_image_read: error %u reading %u bytes from image file %s at block %u\n", sts, size, imageargs -> imagename, vbn);
  }
  return (sts);
}

/************************************************************************/
/*									*/
/*  Do I/O and wait for it to complete					*/
/*									*/
/************************************************************************/

static uLong iowait (OZ_Event *ioevent, OZ_Iochan *iochan, uLong funcode, uLong as, void *ap)

{
  uLong sts;
  volatile uLong status;

  status = OZ_PENDING;
  sts = oz_knl_iostart (iochan, OZ_PROCMODE_KNL, NULL, NULL, &status, ioevent, NULL, NULL, funcode, as, ap);
  if (sts == OZ_STARTED) {
    while ((sts = status) == OZ_PENDING) {
      oz_knl_event_waitone (ioevent);
      oz_knl_event_set (ioevent, 0);
    }
  }
  return (sts);
}

/************************************************************************/
/*									*/
/*  Copy image list from old process to new process.  Actually, the 	*/
/*  list itself has already been copied as a result of the 		*/
/*  oz_knl_process_copy operation.  So all we have to do is increment 	*/
/*  reference counts.  The new process is current on the cpu.		*/
/*									*/
/*  Image list doesn't have to be locked as no one outside the caller 	*/
/*  knows the process exists yet					*/
/*									*/
/************************************************************************/

uLong oz_knl_imagelist_copied (int nsecmaps, OZ_Section *oldsections[], OZ_Section *newsections[])

{
  int i;
  OZ_Image *image;
  OZ_Image_Refd_image *refd_image;
  OZ_Image_Secload *secload;
  OZ_Imagelist *imagelist;
  uLong sts;

  imagelist = OZ_SYS_PDATA_FROM_KNL (OZ_PROCMODE_KNL) -> imagelist;			// point to new process' image list

  imagelist -> event = NULL;								// we don't want to share lock event flag
  sts = oz_knl_event_create (21, "oz_knl_imagelist lock", NULL, &(imagelist -> event)); // create new lock event flag
  if (sts != OZ_SUCCESS) return (sts);
  oz_knl_event_set (imagelist -> event, 1);						// mark it unlocked

  for (image = imagelist -> images; image != NULL; image = image -> next) {		// loop through all its images
    for (secload = image -> secloads; secload != NULL; secload = secload -> next) {	// loop through all image sections
      secload -> seclock = NULL;							// new pages aren't locked by us
      for (i = nsecmaps; -- i >= 0;) {							// scan the given arrays
        if (secload -> section == oldsections[i]) break;				// see if we found the old section
      }
      if (i < 0) secload -> section = NULL;						// section no longer mapped to process, forget about it
      else {
        secload -> section = newsections[i];						// save new section pointer
        oz_knl_section_increfc (newsections[i],  1);					// increment new refcount
      }
    }
    for (refd_image = image -> refd_images; refd_image != NULL; refd_image = refd_image -> next) {
      oz_knl_image_increfc (refd_image -> image, 1);					// increment referenced images' refcount
    }
  }
  return (OZ_SUCCESS);									// successful
}

/************************************************************************/
/*									*/
/*  Called by process termination routine to get rid of imagelist 	*/
/*  struct.  It unloads any images that might still be there.		*/
/*									*/
/************************************************************************/

void oz_knl_imagelist_close (void)

{
  OZ_Image *image;
  OZ_Imagelist *imagelist;
  OZ_Pdata *pdata;

  pdata     = OZ_SYS_PDATA_FROM_KNL (OZ_PROCMODE_KNL);
  imagelist = pdata -> imagelist;							// point to current process' image list
  if (imagelist != NULL) {								// see if it has one
    while ((image = imagelist -> images) != NULL) oz_knl_image_increfc (image, -1);	// ok, release any loaded images
    oz_knl_event_increfc (imagelist -> event, -1);					// release the locking event flag
    oz_sys_pdata_free (OZ_PROCMODE_KNL, imagelist);					// free off the struct
    pdata -> imagelist = NULL;								// (in case it matters)
  }
}

/************************************************************************/
/*									*/
/*  Return first/next image on list					*/
/*									*/
/************************************************************************/

OZ_Image *oz_knl_image_next (OZ_Image *lastimage, int sysimage)

{
  OZ_Imagelist *imagelist;
  OZ_Pdata *pdata;

  OZ_KNL_CHKOBJTYPE (lastimage, OZ_OBJTYPE_IMAGE);
  if (lastimage != NULL) return (lastimage -> next);
  pdata     = oz_sys_pdata_pointer (sysimage ? OZ_PROCMODE_SYS : OZ_PROCMODE_KNL);
  imagelist = pdata -> imagelist;
  if (imagelist == NULL) return (NULL);
  return (imagelist -> images);
}

/************************************************************************/
/*									*/
/*  Return pointer to list of sections loaded by this image		*/
/*									*/
/************************************************************************/

OZ_Image_Secload *oz_knl_image_secloads (OZ_Image *image)

{
  OZ_KNL_CHKOBJTYPE (image, OZ_OBJTYPE_IMAGE);
  return (image -> secloads);
}

/************************************************************************/
/*									*/
/*  Get an image's name							*/
/*									*/
/************************************************************************/

const char *oz_knl_image_name (OZ_Image *image)

{
  OZ_KNL_CHKOBJTYPE (image, OZ_OBJTYPE_IMAGE);
  return (image -> name);
}

/************************************************************************/
/*									*/
/*  Lock/Unlock an Image List						*/
/*									*/
/************************************************************************/

static void lockimagelist (OZ_Imagelist *imagelist)

{
  while (oz_knl_event_set (imagelist -> event, 0) == 0) {
    oz_knl_event_waitone (imagelist -> event);
  }
}

static void unlkimagelist (OZ_Imagelist *imagelist)

{
  oz_knl_event_set (imagelist -> event, 1);
}
