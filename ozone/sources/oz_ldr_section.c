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
/*  Section and process replacement routines for loader			*/
/*									*/
/************************************************************************/

#define _OZ_KNL_SECTION_C

#include "ozone.h"

#include "oz_knl_devio.h"
#include "oz_knl_image.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_lock.h"
#include "oz_knl_printk.h"
#include "oz_knl_process.h"
#include "oz_knl_sdata.h"
#include "oz_knl_section.h"
#include "oz_knl_status.h"

struct OZ_Section { OZ_Objtype objtype;	/* OZ_OBJTYPE_SECTION */
                    OZ_Iochan *file;
                    OZ_Mempage npages;
                    OZ_Dbn rbnstart;
                    OZ_Section_type sectype;
                    OZ_Hw_pageprot pageprot;
                    OZ_Mempage npagem;
                    OZ_Mempage svpage;
                  };

static OZ_Process *curproc = NULL;

/************************************************************************/
/*									*/
/*  This routine is called by oz_knl_image_load to declare the 		*/
/*  location of a section in the kernel image file			*/
/*									*/
/*    Input:								*/
/*									*/
/*	file = kernel image file pointer				*/
/*	npages = number of pages in the section				*/
/*	rbnstart = relative block number in file to start		*/
/*	sectype = section type						*/
/*	pageprot = page protection					*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_section_create = OZ_SUCCESS				*/
/*	*section_r = params saved for call to process_mapsection	*/
/*									*/
/************************************************************************/

uLong oz_knl_section_create (OZ_Iochan *file,
                             OZ_Mempage npages,
                             OZ_Dbn rbnstart,
                             OZ_Section_type sectype,
                             OZ_Secattr *secattr,
                             OZ_Section **section_r)

{
  OZ_Section *section;

  /* Save all params for mapsection call */

  section = OZ_KNL_NPPMALLOC (sizeof *section);
  section -> objtype  = OZ_OBJTYPE_SECTION;
  section -> file     = file;		/* image file */
  section -> npages   = npages;		/* number of pages in section */
  section -> rbnstart = rbnstart;	/* virtual block number to start at */
  section -> sectype  = sectype;	/* section type */

  section -> npagem = 0;		/* number of pages mapped = 0 */
  section -> svpage = 0;		/* starting virtual page = 0 */

  *section_r = section;			/* don't really have a section pointer as such */

  return (OZ_SUCCESS);
}

Long oz_knl_section_increfc (OZ_Section *section, Long inc)

{
  oz_crash ("oz_knl_section_increfc: not valid in loader");
  return (0);
}

/************************************************************************/
/*									*/
/*  This routine is called by oz_knl_image_load to change the 		*/
/*  protection of a section						*/
/*									*/
/************************************************************************/

uLong oz_knl_section_setpageprot (OZ_Mempage npages, OZ_Mempage svpage, OZ_Hw_pageprot pageprot, OZ_Section *initsec, OZ_Hw_pageprot *pageprot_r)

{
  if ((pageprot != OZ_HW_PAGEPROT_KW) && (pageprot != OZ_HW_PAGEPROT_UR)) {
    oz_crash ("oz_knl_section_setprot: only handle OZ_HW_PAGEPROT_KW and OZ_HW_PAGEPROT_UR, not %u", pageprot);
  }

  if ((svpage != 0) && (pageprot != OZ_HW_PAGEPROT_KW)) {
    oz_knl_printk ("oz_ldr_section_setpageprot: setting %u pages at %p read-only\n", npages, OZ_HW_VPAGETOVADDR (svpage));
    oz_hw_ldr_knlpage_setro (npages, svpage);
  }

  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Copy buffers to/from outer processor mode				*/
/*									*/
/************************************************************************/

/************************************************************************/
/*									*/
/*  Get buffer from outer mode						*/
/*									*/
/*    Input:								*/
/*									*/
/*	procmode = outer (or same) processor mode			*/
/*	size     = size to be copied					*/
/*	usrc     = outer mode source buffer address			*/
/*	kdst     = kernel mode dest buffer address			*/
/*									*/
/*	smplock <= softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_section_uget = OZ_SUCCESS : successful completion	*/
/*	                            else : error status			*/
/*									*/
/************************************************************************/

uLong oz_knl_section_uget (OZ_Procmode procmode, uLong size, const void *usrc, void *kdst)

{
  memcpy (kdst, usrc, size);
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Get buffer from outer mode, stop on null byte			*/
/*									*/
/*    Input:								*/
/*									*/
/*	procmode = outer (or same) processor mode			*/
/*	size     = max size to be copied				*/
/*	usrc     = outer mode source buffer address			*/
/*	kdst     = kernel mode dest buffer address			*/
/*									*/
/*	smplock <= softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_section_ugetz = OZ_SUCCESS : successful completion	*/
/*	                             else : error status		*/
/*	*len_r = num bytes copied, including the null			*/
/*									*/
/************************************************************************/

uLong oz_knl_section_ugetz (OZ_Procmode procmode, uLong size, const void *usrc, void *kdst, uLong *len_r)

{
  uByte *dp, xx;
  const uByte *sp;
  uLong bc;

  bc = size;
  sp = usrc;
  dp = kdst;

  while (bc != 0) { *(dp ++) = xx = (*sp ++); if (xx == 0) break; }
  if (len_r != NULL) *len_r = size - bc;
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Put buffer to outer mode						*/
/*									*/
/*    Input:								*/
/*									*/
/*	procmode = outer (or same) processor mode			*/
/*	size     = size to be copied					*/
/*	ksrc     = kernel mode source buffer address			*/
/*	udst     = outer mode dest buffer address			*/
/*									*/
/*	smplock <= softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_section_uput = OZ_SUCCESS : successful completion	*/
/*	                            else : error status			*/
/*									*/
/************************************************************************/

uLong oz_knl_section_uput (OZ_Procmode procmode, uLong size, const void *ksrc, void *udst)

{
  memcpy (udst, ksrc, size);
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  General buffer locking routines					*/
/*  All dummied out in the loader					*/
/*									*/
/************************************************************************/

uLong oz_knl_section_iolockz (OZ_Procmode procmode, uLong size, const void *buff, uLong *rlen, OZ_Seclock **seclock_r, OZ_Mempage *npages_r, const OZ_Mempage **phypages_r, uLong *byteoffs_r)

{
  uLong zsiz;

  if (buff == NULL) return (OZ_ACCVIO);
  zsiz = strlen (buff);				/* get length of string */
  if (zsiz > size) zsiz = size;			/* if more than max, stop there */
  if (rlen != NULL) *rlen = zsiz;		/* return length, not including null */
  if (zsiz < size) zsiz ++;			/* include null in what we lock, though */
  return (oz_knl_section_block (procmode, zsiz, buff, 0, seclock_r));
}

uLong oz_knl_section_iolock  (OZ_Procmode procmode, uLong size, const void *buff, int writing, OZ_Seclock **seclock_r, OZ_Mempage *npages_r, const OZ_Mempage **phypages_r, uLong *byteoffs_r)

{
  OZ_Mempage i, npages, *phypages, vpage;
  OZ_Section_pagestate pagestate;

  *seclock_r = NULL;
  if (npages_r   != NULL) *npages_r   = 0;					// set up null values in case of null size
  if (phypages_r != NULL) *phypages_r = NULL;
  if (byteoffs_r != NULL) *byteoffs_r = 0;
  if (size == 0) return (OZ_SUCCESS);						// null size is instant success
  if (buff == NULL) return (OZ_ACCVIO);						// null buffer is instant failure
  if (phypages_r != NULL) {
    vpage  = OZ_HW_VADDRTOVPAGE (buff);						// starting virtual page number
    npages = OZ_HW_VADDRTOVPAGE (((OZ_Pointer)buff) + size - 1) - vpage + 1;	// number of pages in buffer
    phypages = OZ_KNL_NPPMALLOC (npages * sizeof *phypages);			// allocate phypage array
    for (i = 0; i < npages; i ++) {						// loop for each page
      oz_hw_pte_readsys (vpage + i, &pagestate, phypages + i, NULL, NULL);	// read corresponding physical page number
      if ((pagestate != OZ_SECTION_PAGESTATE_WRITEINPROG) 			// make sure it's valid
       && (pagestate != OZ_SECTION_PAGESTATE_VALID_R) 
       && (pagestate != OZ_SECTION_PAGESTATE_VALID_W) 
       && (pagestate != OZ_SECTION_PAGESTATE_VALID_D)) {
        OZ_KNL_NPPFREE (phypages);						// if not, failure
        oz_knl_printk ("oz_knl_section_iolock: vpage %X pagestate %d\n", vpage + i, pagestate);
        return (OZ_ACCVIO);
      }
    }
    *seclock_r  = (OZ_Seclock *)phypages;					// return section lock object
    *phypages_r = phypages;							// return phypage array pointer
    if (npages_r   != NULL) *npages_r   = npages;				// return number of pages
    if (byteoffs_r != NULL) *byteoffs_r = OZ_HW_VADDRTOPOFFS (buff);		// return byte offset in first page
  }
  return (OZ_SUCCESS);
}

void oz_knl_section_iounlk (OZ_Seclock *seclock)

{
  if (seclock != NULL) OZ_KNL_NPPFREE (seclock);
}

OZ_Secattr *oz_knl_section_getsecattr (OZ_Section *section)

{
  return (NULL);
}

/************************************************************************/
/*									*/
/*  We only have the one system process, so everything goes in it	*/
/*									*/
/************************************************************************/

uLong oz_knl_process_create (OZ_Job *job, int sysproc, int copyproc, int name_l, const char *name, OZ_Secattr *secattr, OZ_Process **process_r)

{
  *process_r = oz_s_systemproc;
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  The oz_knl_image_load routine calls this routine when it wants 	*/
/*  the pages loaded in memory.						*/
/*									*/
/*    Input:								*/
/*									*/
/*	section = section block pointer					*/
/*	*npagem = number of pages to load				*/
/*	*svpage = starting virtual page number (see exact)		*/
/*	exact = 0 : map at next available address, rtn page in *svpage	*/
/*	        1 : map at exact page specified by *svpage		*/
/*	ownermode = always OZ_PROCMODE_KNL				*/
/*									*/
/*    Output:								*/
/*									*/
/*	blocks read into memory						*/
/*									*/
/************************************************************************/

uLong oz_knl_process_mapsections (uLong mapsecflags, 
                                  int nsections, 
                                  OZ_Mapsecparam *mapsecparams)

{
  int i;
  uLong sts;

  for (i = 0; i < nsections; i ++) {
    sts = oz_knl_process_mapsection (mapsecparams[i].section, 
                                   &(mapsecparams[i].npagem), 
                                   &(mapsecparams[i].svpage), 
                                     mapsecflags, 
                                     mapsecparams[i].ownermode, 
                                     mapsecparams[i].pageprot);
    if (sts != OZ_SUCCESS) return (sts);
  }
  return (OZ_SUCCESS);
}

uLong oz_knl_process_mapsection (OZ_Section *section,
                                 OZ_Mempage *npagem,
                                 OZ_Mempage *svpage,
                                 uLong mapsecflags,
                                 OZ_Procmode ownermode,
                                 OZ_Hw_pageprot pageprot)

{
  uLong sts;
  OZ_IO_fs_readblocks fs_readblocks;

  OZ_KNL_CHKOBJTYPE (section, OZ_OBJTYPE_SECTION);

  /* We only handle ..._KW or ..._UR pages */

  if ((pageprot != OZ_HW_PAGEPROT_KW) && (pageprot != OZ_HW_PAGEPROT_UR)) {
    oz_crash ("oz_knl_section_create: only handle OZ_HW_PAGEPROT_KW and OZ_HW_PAGEPROT_UR, not %u", pageprot);
  }
  section -> pageprot = pageprot;

  /* If dynamic mapping, get the base address for the kernel section */

  if (!(mapsecflags & OZ_MAPSECTION_EXACT)) *svpage = oz_hw_ldr_knlpage_basevp (pageprot, *npagem, *svpage);

  /* Set up disk read parameters */

  memset (&fs_readblocks, 0, sizeof fs_readblocks);

  fs_readblocks.size = *npagem << OZ_HW_L2PAGESIZE;		/* get number of bytes to read */
  fs_readblocks.buff = OZ_HW_VPAGETOVADDR (*svpage);		/* get address in memory to read into */
  fs_readblocks.svbn = section -> rbnstart;			/* get block number in file to read from */

  oz_knl_printk ("oz_ldr_process_mapsection: reading 0x%X bytes (%uKb) into %p from block %u\n", 
	fs_readblocks.size, fs_readblocks.size >> 10, fs_readblocks.buff, fs_readblocks.svbn);

  /* Tell hardware to set up some read/write memory to read into */

  oz_hw_ldr_knlpage_maprw (*npagem, *svpage);

  /* Start reading and wait for the read to complete */

  sts = oz_knl_io (section -> file, OZ_IO_FS_READBLOCKS, sizeof fs_readblocks, &fs_readblocks);

  /* If read failed, output an error message */

  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_ldr_process_mapsection: error %u reading %u pages from kernel image at block %u\n", sts, *npagem, section -> rbnstart);
  }

  /* Otherwise, maybe set the pages read-only */

  else if (pageprot != OZ_HW_PAGEPROT_KW) {
    oz_knl_printk ("oz_ldr_process_mapsection: setting %u pages at %p read-only\n", *npagem, OZ_HW_VPAGETOVADDR (*svpage));
    oz_hw_ldr_knlpage_setro (*npagem, *svpage);
  }

  /* Save number of page mapped and starting virtual page number (for oz_knl_section_setprot routine) */

  section -> npagem = *npagem;
  section -> svpage = *svpage;

  return (sts);
}

uLong oz_knl_process_unmapsec (OZ_Mempage vpage)

{
  oz_crash ("oz_knl_process_unmapsec: not valid in loader");
  return (OZ_NOTSUPINLDR);
}

Long oz_knl_process_increfc (OZ_Process *proc, Long inc)

{
  return (1);
}

void oz_knl_process_setcur (OZ_Process *process)

{
  curproc = process;
}

OZ_Process *oz_knl_process_getcur (void)

{
  return (curproc);
}

OZ_Logname *oz_knl_process_getlognamdir (OZ_Process *process)

{
  return (oz_s_systemdirectory);
}

OZ_Logname *oz_knl_process_getlognamtbl (OZ_Process *process)

{
  return (oz_s_systemtable);
}

OZ_Secattr *oz_knl_process_getsecattr (OZ_Process *process)

{
  return (NULL);
}

OZ_Job *oz_knl_process_getjob (OZ_Process *process)

{
  return (NULL);
}

void oz_knl_process_validateall (void)

{ }

OZ_Processid oz_knl_process_getid (OZ_Process *process)

{
  return (0);
}

const char *oz_knl_process_getname (OZ_Process *process)

{
  return ("loader");
}

OZ_Devunit **oz_knl_process_getdevalloc (OZ_Process *process)

{
  oz_crash ("oz_knl_process_getdevalloc: not available in loader");
}

OZ_Mempage oz_knl_process_getsecfromvpage2 (OZ_Process *process,
                                            OZ_Mempage *svpage,
                                            OZ_Section **section,
                                            OZ_Mempage *pageoffs,
                                            OZ_Hw_pageprot *pageprot,
                                            OZ_Procmode *procmode,
                                            uLong *mapsecflags)

{
  oz_crash ("oz_knl_process_getsecfromvpage2: not available in loader");
  return (0);
}
