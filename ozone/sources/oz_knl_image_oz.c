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
/*  Load oz image in memory						*/
/*									*/
/*    Input:								*/
/*									*/
/*	imageargs = image load arguments				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_image_oz = OZ_SUCCESS : successfully loaded		*/
/*	              OZ_UNKIMAGEFMT : not an oz image			*/
/*	                        else : load error			*/
/*									*/
/************************************************************************/

#define _OZ_KNL_IMAGE_C

#define MAX_HEADER_K_SIZE (128) /* we set a max size just so we don't try to hog memory */

#include "ozone.h"

#include "oz_knl_hw.h"
#include "oz_knl_image.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_printk.h"
#include "oz_knl_process.h"
#include "oz_knl_sdata.h"
#include "oz_knl_section.h"
#include "oz_knl_status.h"

#include "oz_image.h"

extern uByte OZ_IMAGE_BASEADDR[];

static uLong ozimage_load (OZ_Image_Args *imageargs);
static int ozimage_lookup (void *imagexv, char *symname, OZ_Pointer *symvalu);
static void ozimage_unload (void *imagexv, OZ_Procmode mprocmode);
static void *ozimage_symscan (void *imagexv, void *lastsym, char **symname_r, OZ_Pointer *symvalu_r);

globaldef const OZ_Image_Hand oz_knl_image_oz = { ".oz", ozimage_load, ozimage_lookup, ozimage_unload, ozimage_symscan };

void oz_knl_image_oz_textbase (void)
{ }

static uLong ozimage_load (OZ_Image_Args *imageargs)

{
  char *globlname, *imagex, *refimname, *undefname;
  OZ_Dbn imagevbn, startvbn;
  OZ_Image_Globl *ozimggbldes;
  OZ_Image_Headr *ozimghdr;
  OZ_Image_Refd_image *refd_image;
  OZ_Image_Refim *ozimgrefdes;
  OZ_Image_Reloc *ozimgreldes;
  OZ_Image_Secload *secload;
  OZ_Image_Sectn *ozimgsecdes;
  OZ_Image_Undef *ozimgunddes;
  OZ_Mapsecparam *mapsecparams;
  OZ_Mempage basepage, npagem, relocvpage0, relocvpage1, svpage;
  OZ_Pointer *relocvaddr;
  OZ_Hw_pageprot pageprot;
  OZ_Seclock *knlsymtblseclck;
  OZ_Section *section;
  uLong blocksize, i, j, mapsecflags, sts, vbnpervpage;

  blocksize = imageargs -> fs_getinfo1 -> blocksize;
  imagex = NULL;
  mapsecparams = NULL;

  /* Make sure imageargs -> hdrbuf contains the whole oz header */

  if (sizeof *ozimghdr > OZ_IMAGE_HDRBUFSIZE) {
    oz_crash ("oz_knl_image_oz: OZ IMAGE HDRBUFSIZE (%d) too small, must be at least %d\n", OZ_IMAGE_HDRBUFSIZE, sizeof *ozimghdr);
  }

  ozimghdr = (OZ_Image_Headr *)(imageargs -> hdrbuf);

  /* Check for oz image magic number */

#ifdef OZ_HW_TYPE_486
  if (memcmp (ozimghdr -> magic, "oz_image", sizeof ozimghdr -> magic) == 0) goto magic_ok;
#endif
#ifdef OZ_HW_TYPE_AXP
  if (memcmp (ozimghdr -> magic, "oz_imaxp", sizeof ozimghdr -> magic) == 0) goto magic_ok;
#endif
  return (OZ_UNKIMAGEFMT);
magic_ok:

  /* Check structure sizes in header */

  i = 0;
  if ((1 << OZ_HW_L2PAGESIZE) % blocksize != 0) {
    oz_knl_printk ("oz_knl_image_oz: page size %u not a multiple of block size %u\n", 1 << OZ_HW_L2PAGESIZE, blocksize);
    i = 1;
  }
  if ((ozimghdr -> imageoffs % (1 << OZ_HW_L2PAGESIZE)) != 0) {
    oz_knl_printk ("oz_knl_image_oz: header size %u not multiple of page size %u\n", ozimghdr -> imageoffs, 1 << OZ_HW_L2PAGESIZE);
    i = 1;
  }
  if (ozimghdr -> imageoffs % blocksize != 0) {
    oz_knl_printk ("oz_knl_image_oz: image offset %u not multiple of disk block size %u\n", ozimghdr -> imageoffs, blocksize);
    i = 1;
  }
  if (ozimghdr -> fxhdrsize != sizeof *ozimghdr) {
    oz_knl_printk ("oz_knl_image_oz: header fxhdrsize %u not %u\n", ozimghdr -> fxhdrsize, sizeof *ozimghdr);
    i = 1;
  }
  if (ozimghdr -> refdessiz != sizeof *ozimgrefdes) {
    oz_knl_printk ("oz_knl_image_oz: header refdessiz %u not %u\n", ozimghdr -> refdessiz, sizeof *ozimgrefdes);
    i = 1;
  }
  if (ozimghdr -> gbldessiz != sizeof *ozimggbldes) {
    oz_knl_printk ("oz_knl_image_oz: header gbldessiz %u not %u\n", ozimghdr -> gbldessiz, sizeof *ozimggbldes);
    i = 1;
  }
  if (ozimghdr -> unddessiz != sizeof *ozimgunddes) {
    oz_knl_printk ("oz_knl_image_oz: header unddessiz %u not %u\n", ozimghdr -> unddessiz, sizeof *ozimgunddes);
    i = 1;
  }
  if (ozimghdr -> reldessiz != sizeof *ozimgreldes) {
    oz_knl_printk ("oz_knl_image_oz: header reldessiz %u not %u\n", ozimghdr -> reldessiz, sizeof *ozimgreldes);
    i = 1;
  }
  if (ozimghdr -> secdessiz != sizeof *ozimgsecdes) {
    oz_knl_printk ("oz_knl_image_oz: header secdessiz %u not %u\n", ozimghdr -> secdessiz, sizeof *ozimgsecdes);
    i = 1;
  }

  /* Check offsets in header */

  if ((ozimghdr -> refdesoff > ozimghdr -> imageoffs) || (ozimghdr -> refdesnum > (ozimghdr -> imageoffs - ozimghdr -> refdesoff) / sizeof *ozimgrefdes)) {
    oz_knl_printk ("oz_knl_image_oz: header refdesoff %u num %u overflows header size %u\n", ozimghdr -> refdesoff, ozimghdr -> refdesnum, ozimghdr -> imageoffs);
    i = 1;
  }
  if ((ozimghdr -> gbldesoff > ozimghdr -> imageoffs) || (ozimghdr -> gbldesnum > (ozimghdr -> imageoffs - ozimghdr -> gbldesoff) / sizeof *ozimggbldes)) {
    oz_knl_printk ("oz_knl_image_oz: header gbldesoff %u num %u overflows header size %u\n", ozimghdr -> gbldesoff, ozimghdr -> gbldesnum, ozimghdr -> imageoffs);
    i = 1;
  }
  if ((ozimghdr -> unddesoff > ozimghdr -> imageoffs) || (ozimghdr -> unddesnum > (ozimghdr -> imageoffs - ozimghdr -> unddesoff) / sizeof *ozimgunddes)) {
    oz_knl_printk ("oz_knl_image_oz: header unddesoff %u num %u overflows header size %u\n", ozimghdr -> unddesoff, ozimghdr -> unddesnum, ozimghdr -> imageoffs);
    i = 1;
  }
  if ((ozimghdr -> reldesoff > ozimghdr -> imageoffs) || (ozimghdr -> reldesnum > (ozimghdr -> imageoffs - ozimghdr -> reldesoff) / sizeof *ozimgreldes)) {
    oz_knl_printk ("oz_knl_image_oz: header reldesoff %u num %u overflows header size %u\n", ozimghdr -> reldesoff, ozimghdr -> reldesnum, ozimghdr -> imageoffs);
    i = 1;
  }
  if ((ozimghdr -> secdesoff > ozimghdr -> imageoffs) || (ozimghdr -> secdesnum > (ozimghdr -> imageoffs - ozimghdr -> secdesoff) / sizeof *ozimgsecdes)) {
    oz_knl_printk ("oz_knl_image_oz: header secdesoff %u num %u overflows header size %u\n", ozimghdr -> secdesoff, ozimghdr -> secdesnum, ozimghdr -> imageoffs);
    i = 1;
  }
  if (i) return (OZ_BADIMAGEFMT);

  /* Map the header to high end of memory */

  if (oz_s_inloader) {
    imagex = oz_sys_pdata_malloc (imageargs -> mprocmode, ozimghdr -> imageoffs);	// allocate memory block
    if (imagex == NULL) {
      sts = OZ_NOMEMORY;
      goto cleanup;
    }
    sts = oz_knl_image_read (imageargs, 1, ozimghdr -> imageoffs, imagex);		// read it in
    if (sts != OZ_SUCCESS) goto cleanup;
  } else {
    npagem = ozimghdr -> imageoffs >> OZ_HW_L2PAGESIZE;					// get number of pages in header
    sts = oz_knl_section_create (imageargs -> iochan, npagem, 1, 0, NULL, &section);	// create section
    if (sts != OZ_SUCCESS) goto cleanup;
    svpage = OZ_HW_VADDRTOVPAGE (OZ_HW_DVA_KNLHEAP_VA);					// map in kernel heap area
    if (imageargs -> sysimage) svpage = OZ_HW_VADDRTOVPAGE (&oz_s_systempdata);
    sts = oz_knl_process_mapsection (section, &npagem, &svpage, OZ_HW_DVA_KNLHEAP_AT, OZ_PROCMODE_KNL, OZ_HW_PAGEPROT_KW);
    oz_knl_section_increfc (section, -1);						// if it mapped, that keeps it in memory
    if (sts != OZ_SUCCESS) goto cleanup;
    imagex = OZ_HW_VPAGETOVADDR (svpage);						// get the virt address it mapped at
    if (imageargs -> sysimage == 2) {							// lock kernel symbol table in memory
      sts = oz_knl_section_iolock (OZ_PROCMODE_KNL, 					// - it's owned by kernel mode
                                   ozimghdr -> imageoffs, 				// - its size in bytes
                                   imagex, 						// - its starting address
                                   1, 							// - we might need to write (reloc) it
                                   &knlsymtblseclck, 					// - the unlock key
                                   NULL, NULL, NULL);					// - we don't care about phypages
      if (sts != OZ_SUCCESS) {
        oz_knl_printk ("oz_knl_image_oz: error %u locking kernel symbol table in memory\n", sts);
        goto cleanup;
      }
    }
  }

  /* Map the sections to memory.  But don't bother if we are just reading the kernel symbol table. */

  if (imageargs -> sysimage != 2) {
    vbnpervpage = (1 << OZ_HW_L2PAGESIZE) / blocksize;					/* number of disk blocks per virtual page */
    imagevbn    = ozimghdr -> imageoffs / blocksize + 1;				/* virtual block number of start of image data */

    mapsecparams = OZ_KNL_NPPMALLOQ (ozimghdr -> secdesnum * sizeof *mapsecparams);	/* alloc array for section mapping parameters */
    if (mapsecparams == NULL) {
      sts = OZ_EXQUOTANPP;
      goto cleanup;
    }

    for (i = 0; i < ozimghdr -> secdesnum; i ++) {					/* loop through all the descriptors */
      ozimgsecdes = (OZ_Image_Sectn *)(imagex + ozimghdr -> secdesoff) + i;		/* point to descriptor */

      startvbn    = imagevbn + (ozimgsecdes -> sectn_vpage * vbnpervpage);		/* starting virtual block number */
      ozimgsecdes -> sectn_vpage += ozimghdr -> basevpage;				/* add base vpage of the image */
      npagem      = ozimgsecdes -> sectn_pages;						/* number of pages to map */
      svpage      = ozimgsecdes -> sectn_vpage;						/* virtual memory page to start at (0 for dynamic) */
      mapsecflags = OZ_MAPSECTION_EXACT;						/* assume static image */
      if (ozimghdr -> basevpage == 0) {							/* check for dynamic image */
        if (imageargs -> sysimage != 0) {
          mapsecflags = OZ_HW_DVA_SIMAGES_AT;						/*   system dynamic images go here */
          svpage     += OZ_HW_VADDRTOVPAGE (OZ_HW_DVA_SIMAGES_VA);
        } else {
          mapsecflags = OZ_HW_DVA_PIMAGES_AT;						/*   process dynamic images go here */
          svpage     += OZ_HW_VADDRTOVPAGE (OZ_HW_DVA_PIMAGES_VA);
        }
      }

      sts = oz_knl_section_create (imageargs -> iochan, npagem, startvbn, 0, imageargs -> secattr, &section);
      if (sts != OZ_SUCCESS) {
        oz_knl_printk ("oz_knl_image_oz: error %u creating %u page section for vbn %u of image %s\n", sts, npagem, startvbn, imageargs -> imagename);
        goto cleanup;
      }
      mapsecparams[i].section   = section;						/* section to map to memory */
      mapsecparams[i].npagem    = npagem;						/* how many pages to map */
      mapsecparams[i].svpage    = svpage;						/* starting virtual page number */
      mapsecparams[i].ownermode = imageargs -> procmode;				/* who owns them */
      mapsecparams[i].pageprot  = OZ_HW_PAGEPROT_KW;					/* set them to this protection for now */
											/* ... so we can do relocations */
    }

    sts = oz_knl_process_mapsections (mapsecflags, ozimghdr -> secdesnum, mapsecparams);
    if (sts != OZ_SUCCESS) {
      oz_knl_printk ("oz_knl_image_oz: error %u mapping sections to memory for image %s\n", sts, imageargs -> imagename);
      oz_knl_printk ("oz_knl_image_oz: %u section(s), %s\n", ozimghdr -> secdesnum, (ozimghdr -> basevpage == 0) ? "dynamic" : "exact");
      for (i = 0; i < ozimghdr -> secdesnum; i ++) {
        ozimgsecdes = (OZ_Image_Sectn *)(imagex + ozimghdr -> secdesoff) + i;
        oz_knl_printk ("oz_knl_image_oz:   [%d] 0x%X %s pages at 0x%X\n", 
		i, ozimgsecdes -> sectn_pages, (ozimgsecdes -> sectn_write) ? "read/write" : "read-only", 
		ozimgsecdes -> sectn_vpage);
      }
      goto cleanup;
    }

    ozimgsecdes = (OZ_Image_Sectn *)(imagex + ozimghdr -> secdesoff);			/* point to first section descriptor */
    basepage = mapsecparams[0].svpage - ozimgsecdes -> sectn_vpage + ozimghdr -> basevpage; /* get image's base virt page number */
    imageargs -> baseaddr = OZ_HW_VPAGETOVADDR (basepage);				/* return base load address of image */

    for (i = 0; i < ozimghdr -> secdesnum; i ++) {					/* loop through all the descriptors */
      ozimgsecdes = (OZ_Image_Sectn *)(imagex + ozimghdr -> secdesoff) + i;		/* point to descriptor */
      ozimgsecdes -> sectn_vpage = mapsecparams[i].svpage;				/* save vpage that it was mapped at */
      secload = oz_sys_pdata_malloc (imageargs -> mprocmode, sizeof *secload);		/* put on list of sections that were loaded for the image */
      if (secload == NULL) {
        sts = OZ_EXQUOTAPGP;
        goto cleanup;
      }
      secload -> next        = imageargs -> secloads;
      secload -> section     = mapsecparams[i].section;
      secload -> npages      = mapsecparams[i].npagem;
      secload -> svpage      = mapsecparams[i].svpage;
      secload -> writable    = ozimgsecdes -> sectn_write;
      secload -> seclock     = NULL;
      imageargs -> secloads  = secload;
      mapsecparams[i].npagem = 0;							// tell cleanup routine that this one 
											// ... is in secloads list now
    }

    OZ_KNL_NPPFREE (mapsecparams);
    mapsecparams = NULL;
  }

  /* If reading kernel symbol table, just build list of secload (with section=NULL) */

  else {
    basepage = ozimghdr -> basevpage;							/* get image's base virt page number */
    imageargs -> baseaddr = OZ_HW_VPAGETOVADDR (basepage);				/* return base load address of image */
    if (basepage == 0) imageargs -> baseaddr = OZ_IMAGE_BASEADDR;			/* if dynamic, return relocated load addr */
    for (i = 0; i < ozimghdr -> secdesnum; i ++) {					/* loop through all the descriptors */
      ozimgsecdes = (OZ_Image_Sectn *)(imagex + ozimghdr -> secdesoff) + i;		/* point to descriptor */
      secload = oz_sys_pdata_malloc (imageargs -> mprocmode, sizeof *secload);		/* put on list of sections that were loaded for the image */
      if (secload == NULL) {								/* ... so it will get cleaned up */
        sts = OZ_NOMEMORY;
        goto cleanup;
      }
      secload -> next       = imageargs -> secloads;
      secload -> section    = NULL;
      secload -> npages     = ozimgsecdes -> sectn_pages;
      secload -> svpage     = ozimgsecdes -> sectn_vpage + OZ_HW_VADDRTOVPAGE (imageargs -> baseaddr);
      secload -> writable   = ozimgsecdes -> sectn_write;
      secload -> seclock    = NULL;
      imageargs -> secloads = secload;
    }
  }

  /* Return any start address given in image                              */
  /* If it is an dynamic image and there is an start address, relocate it */

  imageargs -> startaddr = (void *)(ozimghdr -> startaddr);
  if (ozimghdr -> startaddr == OZ_IMAGE_NULLSTARTADDR) imageargs -> startaddr = NULL;
  else if (ozimghdr -> basevpage == 0) (OZ_Pointer)(imageargs -> startaddr) += (OZ_Pointer)(imageargs -> baseaddr);

  /* If dynamic image, relocate global symbols by base address        */
  /* Scan all global symbol descriptors to check name offset in range */

  for (i = 0; i < ozimghdr -> gbldesnum; i ++) {					/* loop through all the descriptors */
    ozimggbldes = (OZ_Image_Globl *)(imagex + ozimghdr -> gbldesoff) + i;		/* point to descriptor */
    globlname = imagex + ozimghdr -> gblstroff + ozimggbldes -> globl_namof;		/* point to global name string */
    if (globlname - imagex >= ozimghdr -> imageoffs) {					/* make sure it doesn't go off end of header */
      oz_knl_printk ("oz_knl_image_oz: global symbol index %u name overflows header\n", i);
      goto badimagefmt;
    }
    if (ozimggbldes -> globl_flags & OZ_IMAGE_GLOBL_FLAG_RELOC) {
      ozimggbldes -> globl_value += (OZ_Pointer)(imageargs -> baseaddr);		/* relocate global symbol */
    }
  }

  /* Load all referenced images in memory */

  for (i = 0; i < ozimghdr -> refdesnum; i ++) {					/* loop through all the descriptors */
    ozimgrefdes = (OZ_Image_Refim *)(imagex + ozimghdr -> refdesoff) + i;		/* point to descriptor */
    refimname = imagex + ozimghdr -> refstroff + ozimgrefdes -> refim_namof;		/* point to image name string */
    if (refimname - imagex >= ozimghdr -> imageoffs) {					/* make sure it doesn't go off end of header */
      oz_knl_printk ("oz_knl_image_oz: referenced image index %u name overflows header\n", i);
      goto badimagefmt;
    }
    sts = oz_knl_image_load (imageargs -> procmode, 					/* same processor mode as image I am loading */
                             refimname, 						/* this is the images name */
                             imageargs -> sysimage, 					/* same system image flag as image I am loading */
                             imageargs -> level + 1, 					/* it is one level deeper than image I am loading */
                             NULL, 							/* don't care about its base load address */
                             NULL, 							/* don't care about its start address */
                             (OZ_Image **)&(ozimgrefdes -> refim_image));		/* save the image struct pointer (so we can unload it) */
    if (sts != OZ_SUCCESS) {
      oz_knl_printk ("oz_knl_image_oz: error %u loading image %s referenced by %s\n", sts, refimname, imageargs -> imagename);
      goto cleanup;
    }
    refd_image = oz_sys_pdata_malloc (imageargs -> mprocmode, sizeof *refd_image);	/* link it as ref'd image so it will get cleaned up */
    if (refd_image == NULL) {
      oz_knl_image_increfc ((OZ_Image *)(ozimgrefdes -> refim_image), -1);
      sts = OZ_NOMEMORY;
      goto cleanup;
    }
    refd_image -> next  = imageargs -> refd_images;
    refd_image -> image = (OZ_Image *)(ozimgrefdes -> refim_image);
    imageargs -> refd_images = refd_image;
  }

  /* Look up all undefined symbols in the referenced images */

  for (i = 0; i < ozimghdr -> unddesnum; i ++) {					/* loop through all the descriptors */
    ozimgunddes = (OZ_Image_Undef *)(imagex + ozimghdr -> unddesoff) + i;		/* point to descriptor */
    undefname = imagex + ozimghdr -> undstroff + ozimgunddes -> undef_namof;		/* point to symbol name string */
    if (undefname - imagex >= ozimghdr -> imageoffs) {					/* make sure it doesn't go off end of header */
      oz_knl_printk ("oz_knl_image_oz: undefined symbol index %u name overflows header\n", i);
      goto badimagefmt;
    }
    if (ozimgunddes -> undef_refim >= ozimghdr -> refdesnum) {				/* make sure the referenced image index is in range */
      oz_knl_printk ("oz_knl_image_oz: undefined symbol index %u ref image index %u overflows header\n", i, ozimgunddes -> undef_refim);
      goto badimagefmt;
    }
    ozimgrefdes = (OZ_Image_Refim *)(imagex + ozimghdr -> refdesoff) + ozimgunddes -> undef_refim; /* point to descriptor */
    if (!oz_knl_image_lookup ((OZ_Image *)(ozimgrefdes -> refim_image), undefname, &(ozimgunddes -> undef_value))) { /* look up the symbol */
      oz_knl_printk ("oz_knl_image_oz: undefined symbol %s not defined by image %s\n", 
		undefname, imagex + ozimghdr -> refstroff + ozimgrefdes -> refim_namof);
      goto badimagefmt;
    }
  }

  /* Perform relocations */

  if (imageargs -> sysimage != 2) {							/* don't bother if reading kernel symbol table */
    ozimgunddes = (OZ_Image_Undef *)(imagex + ozimghdr -> unddesoff);			/* point to base of undefined symbol descriptor array */

    for (i = 0; i < ozimghdr -> reldesnum; i ++) {
      ozimgreldes = (OZ_Image_Reloc *)(imagex + ozimghdr -> reldesoff) + i;		/* point to descriptor */
      relocvaddr  = (OZ_Pointer *)(ozimgreldes -> reloc_vaddr);				/* get address of location to be modified */
      if (ozimghdr -> basevpage == 0) (OZ_Pointer)relocvaddr += (OZ_Pointer)(imageargs -> baseaddr);
      relocvpage0 = OZ_HW_VADDRTOVPAGE (relocvaddr);					/* calculate vpage at beg of reloc */
      relocvpage1 = OZ_HW_VADDRTOVPAGE (((OZ_Pointer)(relocvaddr + 1)) - 1);		/* calculate vpage at end of reloc (inclusive) */
      for (j = 0; j < ozimghdr -> secdesnum; j ++) {					/* loop through all the descriptors */
        ozimgsecdes = (OZ_Image_Sectn *)(imagex + ozimghdr -> secdesoff) + j;		/* point to descriptor */
        if (relocvpage0 < ozimgsecdes -> sectn_vpage) continue;
        if (relocvpage1 < ozimgsecdes -> sectn_vpage) continue;
        if (relocvpage0 >= ozimgsecdes -> sectn_pages + ozimgsecdes -> sectn_vpage) continue;
        if (relocvpage1 >= ozimgsecdes -> sectn_pages + ozimgsecdes -> sectn_vpage) continue;
        goto oktoreloc;
      }
      oz_knl_printk ("oz_knl_image_oz: reloc address %p (%p) not within section of image %s\n", 
	(OZ_Pointer *)(ozimgreldes -> reloc_vaddr), relocvaddr, imageargs -> imagename);
      goto badimagefmt;
oktoreloc:
      switch (ozimgreldes -> reloc_type) {						/* dispatch based on relocation type */
        case OZ_IMAGE_RELOC_IMGBASE: {
          *relocvaddr += (OZ_Pointer)(imageargs -> baseaddr);				/* - add the base address of the image */
          break;
        }
        case OZ_IMAGE_RELOC_IMGBASE_NEGPC: {
          break;
        }
        case OZ_IMAGE_RELOC_UNDEF: {
          *relocvaddr += ozimgunddes[ozimgreldes->reloc_indx].undef_value;		/* - add value of the undefined symbol */
          break;
        }
        case OZ_IMAGE_RELOC_UNDEF_NEGPC: {
          *relocvaddr += ozimgunddes[ozimgreldes->reloc_indx].undef_value - (OZ_Pointer)relocvaddr; /* - add value of the undefined symbol minus base addr */
          break;
        }
        default: {
          oz_knl_printk ("oz_knl_image_oz: bad relocation type %u for address 0x%x in image %x\n", 
		ozimgreldes -> reloc_type, ozimgreldes -> reloc_vaddr, imageargs -> imagename);
          goto badimagefmt;
        }
      }
    }
  }

  /* Change protections of read-only sections to OZ_HW_PAGEPROT_UR        */
  /* If usermode read/write, change those sections to ..._UW              */
  /* Otherwise, just leave them ..._KW (system image read/write sections) */

  if (imageargs -> sysimage != 2) {
    for (i = 0; i < ozimghdr -> secdesnum; i ++) {					/* loop through all the descriptors */
      ozimgsecdes = (OZ_Image_Sectn *)(imagex + ozimghdr -> secdesoff) + i;		/* point to descriptor */
      if (ozimgsecdes -> sectn_pages == 0) continue;
      if (ozimgsecdes -> sectn_write && (imageargs -> sysimage != 0)) continue;		/* leave it _KW if writable part of system image */
      pageprot = ozimgsecdes -> sectn_write ? OZ_HW_PAGEPROT_UW : OZ_HW_PAGEPROT_UR;	/* set up new protection */
      oz_knl_section_setpageprot (ozimgsecdes -> sectn_pages, ozimgsecdes -> sectn_vpage, pageprot, NULL, NULL);
    }
  }

  /* Also change the header to read-only */

  if (!oz_s_inloader) {
    npagem = ozimghdr -> imageoffs >> OZ_HW_L2PAGESIZE;					// get number of pages in header
    svpage = OZ_HW_VADDRTOVPAGE (imagex);						// starting virtual page number
    sts = oz_knl_section_setpageprot (npagem, svpage, OZ_HW_PAGEPROT_UR, NULL, NULL);	// set protection so usermode can read it
    if (sts != OZ_SUCCESS) goto cleanup;
  }

  /* Successful */

  imageargs -> imagex = imagex;
  return (OZ_SUCCESS);

badimagefmt:
  sts = OZ_BADIMAGEFMT;
cleanup:
  /* ?? unload refim's ?? */
  if (mapsecparams != NULL) {
    for (i = 0; i < ozimghdr -> secdesnum; i ++) {
      if (mapsecparams[i].section != NULL) {
        if (mapsecparams[i].npagem == 0) oz_knl_process_unmapsec (mapsecparams[i].svpage);
        oz_knl_section_increfc (mapsecparams[i].section, -1);
      }
    }
    OZ_KNL_NPPFREE (mapsecparams);
  }
  if (imagex != NULL) {
    if (oz_s_inloader) oz_sys_pdata_free (imageargs -> mprocmode, imagex);
    else oz_knl_process_unmapsec (OZ_HW_VADDRTOVPAGE (imagex));
  }
  return (sts);
}

/************************************************************************/
/*									*/
/*  Look up a symbol in the symbol table				*/
/*									*/
/*    Input:								*/
/*									*/
/*	imagexv = image extension as returned by the load routine	*/
/*	symname = symbol name (null terminated string)			*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_lookup = 0 : symbol not found				*/
/*	            1 : symbol found					*/
/*	*symvalu = symbol value						*/
/*									*/
/************************************************************************/

static int ozimage_lookup (void *imagexv, char *symname, OZ_Pointer *symvalu)

{
  char *globlname, *imagex;
  uLong i;
  OZ_Image_Headr *ozimghdr;
  OZ_Image_Globl *ozimggbldes;

  imagex   = imagexv;					/* point to imagex struct */
  ozimghdr = imagexv;					/* point to image header */

  for (i = 0; i < ozimghdr -> gbldesnum; i ++) {
    ozimggbldes = (OZ_Image_Globl *)(imagex + ozimghdr -> gbldesoff) + i;
    globlname = imagex + ozimghdr -> gblstroff + ozimggbldes -> globl_namof;
    if (strcmp (globlname, symname) == 0) {		/* see if the string matches */
      *symvalu = ozimggbldes -> globl_value;		/* if so, return the value */
      return (1);					/* ... and a success status */
    }
  }
  return (0);						/* can't find the symbol, return failure */
}

/************************************************************************/
/*									*/
/*  Scan the images symbol table					*/
/*									*/
/*    Input:								*/
/*									*/
/*	imagexv = image extension as returned by the load routine	*/
/*	lastsym = NULL : start at the beginning				*/
/*	          else : value returned by last call			*/
/*									*/
/*    Output:								*/
/*									*/
/*	ozimage_symscan = NULL : no more symbols			*/
/*	                  else : scan context				*/
/*	*symname_r = points to null terminated symbol name string	*/
/*	*symvalu_r = symbol value					*/
/*									*/
/************************************************************************/

static void *ozimage_symscan (void *imagexv, void *lastsym, char **symname_r, OZ_Pointer *symvalu_r)

{
  char *imagex;
  uLong i;
  OZ_Image_Headr *ozimghdr;
  OZ_Image_Globl *ozimggbldes;

  imagex   = imagexv;							/* point to imagex struct */
  ozimghdr = imagexv;							/* point to image header */

  i = 0;								/* get index of next symbol to fetch */
  if (lastsym != NULL) i = ((OZ_Image_Globl *)lastsym - (OZ_Image_Globl *)(imagex + ozimghdr -> gbldesoff)) + 1;
  if (i >= ozimghdr -> gbldesnum) return (NULL);
  ozimggbldes = (OZ_Image_Globl *)(imagex + ozimghdr -> gbldesoff) + i;	/* point to symbol entry */
  *symname_r = imagex + ozimghdr -> gblstroff + ozimggbldes -> globl_namof;
  *symvalu_r = ozimggbldes -> globl_value;				/* if so, return the value */
  return (ozimggbldes);							/* ... and context pointer */
}

/************************************************************************/
/*									*/
/*  Finish unloading an image						*/
/*  The sections have all been unmapped					*/
/*  All that remains is to clean up the imagex struct			*/
/*									*/
/************************************************************************/

static void ozimage_unload (void *imagexv, OZ_Procmode mprocmode)

{
  if (oz_s_inloader) oz_sys_pdata_free (mprocmode, imagexv);
  else oz_knl_process_unmapsec (OZ_HW_VADDRTOVPAGE (imagexv));
}
