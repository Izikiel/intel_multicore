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
/*  Sections are areas of virtual memory that have a common bond of 	*/
/*  some sort.								*/
/*									*/
/*    GLOBAL sections:							*/
/*									*/
/*	- can be mapped by more than one process			*/
/*	- all pages are common to all accessors				*/
/*	- cannot be expanded beyond original size			*/
/*	- page state maintained in array				*/
/*									*/
/*    PRIVATE sections:							*/
/*									*/
/*	- can only be mapped once					*/
/*	- can be expanded						*/
/*	- page state maintained in process' page table			*/
/*									*/
/************************************************************************/

#define _OZ_KNL_SECTION_C
#include "ozone.h"

#include "oz_knl_hw.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_lowipl.h"
#include "oz_knl_objtype.h"
#include "oz_knl_phymem.h"
#include "oz_knl_process.h"
#include "oz_knl_quota.h"
#include "oz_knl_sdata.h"
#include "oz_knl_security.h"
#include "oz_knl_section.h"
#include "oz_knl_status.h"

#include "oz_io_fs.h"

/* For global sections (OZ_SECTION_TYPE_GLOBAL), there is an array of Gblpage's, with one element per  */
/* section page.  There may be many hardware pte's that have a copy of the information.  The Secpage   */
/* info is never downgraded if there are referencing pte's.  It may be upgraded at any time, and those */
/* pte's that have old information will be updated when a fault occurrs that references the old pte.   */

/* For private sections, there is no Gblpage array.  Instead, the information is stored in the hardware  */
/* pte array for the process to which the section belongs.  This way, a process can have an HUGE private */
/* section without having a double-memory penalty of both an hardware pte array and the Gblpage array.   */
/* Also, it allows for the expansion of the section by only having to expand the pte array and not the   */
/* Gblpage array.  The hardware pte's must be able to store and retrieve the state information to some   */
/* degree.  Here is a table giving the requirements for what is set up via oz_hw_pte_write:              */

/*	_VALID_R			_VALID_R will always be accompanied by a read-only 	*/
/*	physical page number		protection code.  _VALID_W will always be accompanied 	*/
/*					by a read/write protection code.  The hardware can 	*/
/*	_VALID_W			use this fact to distinguish the two states.		*/
/*	physical page number									*/
/*					note: Pages that contain page table entries never go 	*/
/*					      into _VALID_R, they go directly to _VALID_W.	*/
/*												*/
/*	_WRITEINPROG			The hardware can let cpu's access the page during 	*/
/*	physical page number		_WRITEINPROG.  If it can't do this and distinguish 	*/
/*					between that and _VALID_R / _VALID_W, it can make this 	*/
/*					a no-access state.					*/
/*												*/
/*					The _WRITEINPROG state will be accompanied only by a 	*/
/*					non-write protection code.				*/
/*												*/
/*	_PAGEDOUT			The remaining four states must not allow access to the 	*/
/*	pagefile page number		page by the cpu's.  The hardware pte's need not preserve 
/*					the protection codes for these states.			*/
/*	_READINPROG										*/
/*	physical page number									*/
/*												*/
/*	_READFAILED										*/
/*	I/O error status									*/
/*												*/
/*	_WRITEFAILED										*/
/*	I/O error status									*/

/* Thus, the minimum requirements of the hardware pte's would be:                     */
/*                                                                                    */
/*  A physical page field is always required, valid or invalid                        */
/*                                                                                    */
/*  When page is valid, a protection code,                                            */
/*    If there is a bit available, use it to distinguish _VALID_* from _WRITEINPROG   */
/*    Otherwise, the _WRITEINPROG state will have to be considered invalid            */
/*                                                                                    */
/*  When page is invalid, enough bits to distinguish the invalid states.  There will  */
/*  be either four invalid states or five.  Possibly the page protection bits could   */
/*  be used for this purpose.                                                         */

typedef struct { OZ_Section_pagestate pagestate;	/* page's state */
                 OZ_Mempage phypage;			/* current physical page */
							/* or if paged out, 0 = section file, else page file page number */
               } Gblpage;

struct OZ_Section { OZ_Objtype objtype;			/* OZ_OBJTYPE_SECTION */
                    OZ_Section_type sectype;		/* section type */
                    Long refcount;			/* reference count */
                    OZ_Process *privproc;		/* set when private section gets mapped */
                    OZ_Iochan *file;			/* corresponding disk file */
							/* or NULL if initially all zeroes */
                    OZ_Dbn vbnstart;			/* starting block in file */
                    uLong l2blocksize;			/* LOG2 (file's block size) */
                    OZ_Iochan *pagefile;		/* pagefile pointer */
							/* initially NULL to indicate no pagefile assigned */
                    uLong pagefile_l2blocksize;		/* LOG2 (pagefile's block size) */

                    uLong (*knlpfmap) (OZ_Iochan *file, OZ_Dbn vbn, OZ_Mempage *phypage_r);	/* get cache page */
                    uLong (*knlpfupd) (OZ_Iochan *file, OZ_Dbn vbn, OZ_Mempage phypage);	/* update cache page */
                    void (*knlpfrel) (OZ_Iochan *file, OZ_Mempage phypage);			/* release cache page */

                    OZ_Mempage pfrefcount;		/* number of pages currently allocated in pagefile */
                    OZ_Event *pfevent;			/* pagefault read event flag */
                    OZ_Secattr *secattr;		/* security attributes block pointer */
                    OZ_Quota *quota;			/* quota block to be charged for faulted-in pages */
                    OZ_Mempage npages;			/* number of pages in section */
                    OZ_Smplock smplock_gp;		/* gblpages array lock */
                    Gblpage gblpages[1];		/* global sections only : array of Gblpage, one per page */
                  };

/* Section buffer lock routine page array block */

struct OZ_Seclock { OZ_Objtype objtype;			/* object type OZ_OBJTYPE_SECLOCK */
                    OZ_Seclock *next;			/* next in sectionmap's list */
                    OZ_Seclock **prev;			/* previous in sectionmap's list */
                    OZ_Process *process;		/* process the page beints to */
                    OZ_Mempage npages;			/* number of pages that are locked */
                    OZ_Mempage svpage;			/* starting virtual page number */
                    union { OZ_Mempage phypages[1];	/* array of physical page numbers */
                            OZ_Ieeedma32 ieeedma32s[1];	/* array of IEEE 32-bit dma descriptors */
                            OZ_Ieeedma64 ieeedma64s[1];	/* array of IEEE 64-bit dma descriptors */
                          } a;
                  };

struct OZ_Seclkwz { OZ_Objtype objtype;
                    OZ_Seclkwz *next;
                    OZ_Seclkwz **prev;
                    OZ_Mempage vpage;
                    OZ_Event *event;
                  };

/* Internal static data */

static char protable[OZ_HW_PAGEPROT_MAX][OZ_PROCMODE_MAX+1][2]; /* 0 = illegal access attempt, 1 = legal access attempt */
static OZ_Hw_pageprot ppro[OZ_HW_PAGEPROT_MAX];	/* translate page protection to equivalent read-only page protection */

/* Internal routines */

static void vfyseclocks (OZ_Process *process, OZ_Mempage vpage);
static uLong readprivatepage (OZ_Section *section, OZ_Mempage secpage, OZ_Process *process, uLong pt, 
                              OZ_Mempage vpage, OZ_Hw_pageprot reqprot, OZ_Mempage pfpage, int writing, OZ_Procmode procmode);
static uLong readglobalpage (OZ_Section *section, uLong gp, Gblpage *gblpage, OZ_Mempage secpage, OZ_Process *process, uLong pt, OZ_Mempage pfpage);
static int itsadzvaddr (void *vaddr);
static int waitioidle (int wait, OZ_Mempage vpage, OZ_Process *process);
static uLong writepage_to_secfile (OZ_Section *section, OZ_Mempage secpage, OZ_Mempage phypage);
static uLong writepage_to_pagefile (OZ_Section *section, OZ_Mempage pfpage, OZ_Mempage phypage);
static OZ_Process *getprocess (OZ_Mempage vpage);
static uLong alloc_phypage (OZ_Section *section, OZ_Mempage *phypage_r, OZ_Mempage vpage);
static void free_phypage (OZ_Section *section, OZ_Mempage phypage);
static void free_pfpage (OZ_Section *section, OZ_Mempage pfpage);

/************************************************************************/
/*									*/
/*  Initialize section static data at boot time				*/
/*									*/
/************************************************************************/

void oz_knl_section_init ()

{
  /* This table defines, given the page's protection and the processor mode */
  /* and the access mode, whether or not the attempted access is legal      */

	/* No Access by anyone */
  
  protable[OZ_HW_PAGEPROT_NA][OZ_PROCMODE_KNL][0] = 0;
  protable[OZ_HW_PAGEPROT_NA][OZ_PROCMODE_KNL][1] = 0;
  protable[OZ_HW_PAGEPROT_NA][OZ_PROCMODE_USR][0] = 0;
  protable[OZ_HW_PAGEPROT_NA][OZ_PROCMODE_USR][1] = 0;

	/* Kernel can Read */

#ifdef OZ_HW_PAGEPROT_KR
  protable[OZ_HW_PAGEPROT_KR][OZ_PROCMODE_KNL][0] = 1;
  protable[OZ_HW_PAGEPROT_KR][OZ_PROCMODE_KNL][1] = 0;
  protable[OZ_HW_PAGEPROT_KR][OZ_PROCMODE_USR][0] = 0;
  protable[OZ_HW_PAGEPROT_KR][OZ_PROCMODE_USR][1] = 0;
#endif

	/* Kernel can Write */

  protable[OZ_HW_PAGEPROT_KW][OZ_PROCMODE_KNL][0] = 1;
  protable[OZ_HW_PAGEPROT_KW][OZ_PROCMODE_KNL][1] = 1;
  protable[OZ_HW_PAGEPROT_KW][OZ_PROCMODE_USR][0] = 0;
  protable[OZ_HW_PAGEPROT_KW][OZ_PROCMODE_USR][1] = 0;

	/* Kernel can Write, User can Read */

#ifdef OZ_HW_PAGEPROT_KWUR
  protable[OZ_HW_PAGEPROT_KWUR][OZ_PROCMODE_KNL][0] = 1;
  protable[OZ_HW_PAGEPROT_KWUR][OZ_PROCMODE_KNL][1] = 1;
  protable[OZ_HW_PAGEPROT_KWUR][OZ_PROCMODE_USR][0] = 1;
  protable[OZ_HW_PAGEPROT_KWUR][OZ_PROCMODE_USR][1] = 0;
#endif

	/* User (and kernel) can Read */

  protable[OZ_HW_PAGEPROT_UR][OZ_PROCMODE_KNL][0] = 1;
  protable[OZ_HW_PAGEPROT_UR][OZ_PROCMODE_KNL][1] = 0;
  protable[OZ_HW_PAGEPROT_UR][OZ_PROCMODE_USR][0] = 1;
  protable[OZ_HW_PAGEPROT_UR][OZ_PROCMODE_USR][1] = 0;

	/* User (and kernel) can Write */

  protable[OZ_HW_PAGEPROT_UW][OZ_PROCMODE_KNL][0] = 1;
  protable[OZ_HW_PAGEPROT_UW][OZ_PROCMODE_KNL][1] = 1;
  protable[OZ_HW_PAGEPROT_UW][OZ_PROCMODE_USR][0] = 1;
  protable[OZ_HW_PAGEPROT_UW][OZ_PROCMODE_USR][1] = 1;

  /* This table translates a given protection code into it's equivalent read-only code */
  /* Read/Write codes must be different, Read-Only codes must be the same because this */
  /* table is used to keep track of when a page has been modified, and is used to      */
  /* determine if a given code is read-only or read/write.                             */

  ppro[OZ_HW_PAGEPROT_NA]   = OZ_HW_PAGEPROT_NA;
#ifdef OZ_HW_PAGEPROT_KR
  ppro[OZ_HW_PAGEPROT_KR]   = OZ_HW_PAGEPROT_KR;
  ppro[OZ_HW_PAGEPROT_KW]   = OZ_HW_PAGEPROT_KR;
#else
  ppro[OZ_HW_PAGEPROT_KW]   = OZ_HW_PAGEPROT_NA;
#endif
#ifdef OZ_HW_PAGEPROT_KWUR
  ppro[OZ_HW_PAGEPROT_KWUR] = OZ_HW_PAGEPROT_UR;
#endif
  ppro[OZ_HW_PAGEPROT_UW]   = OZ_HW_PAGEPROT_UR;
  ppro[OZ_HW_PAGEPROT_UR]   = OZ_HW_PAGEPROT_UR;
}

/************************************************************************/
/*									*/
/*  Create section							*/
/*									*/
/*    Input:								*/
/*									*/
/*	file     = file block pointer associated with section		*/
/*	npages   = number of pages associated with section		*/
/*	           0 for the whole file					*/
/*	vbnstart = relative block number in file for section		*/
/*	sectype  = section type						*/
/*	secattr  = security attributes block pointer			*/
/*									*/
/*	smplocks =  none						*/
/*	ipl = null or softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_section_create = completion status			*/
/*	*section_r = section pointer					*/
/*									*/
/*    Note:								*/
/*									*/
/*	This routine may perform I/O to get info about file		*/
/*									*/
/************************************************************************/

uLong oz_knl_section_create (OZ_Iochan *file, 
                             OZ_Mempage npages, 
                             OZ_Dbn vbnstart, 
                             OZ_Section_type sectype, 
                             OZ_Secattr *secattr, 
                             OZ_Section **section_r)

{
  OZ_Event *event;
  OZ_IO_fs_getinfo1 fs_getinfo1;
  OZ_Mempage i, gpas;
  OZ_Section *section;
  uLong l2blocksize, sts;

  *section_r = NULL;

  /* Check for illegal combinations of flags - they are illegal because they would    */
  /* produce unpredictable results (or in some of the pagetable cases, cause a crash) */

  /* The following sane combinations are allowed, try to pick from one of these: */

  /*  <nothing> - private image section                       */
  /*              pages are read from the given file          */
  /*              each reader and writer has own copy of page */
  /*              dirty pages go out to page file             */

  /*  <GLOBAL> - global image section             */
  /*             read pages are shared            */
  /*             each writer has own copy of page */
  /*             dirty pages go out to page file  */

  /*  <GLOBAL SECFIL SHRWRT> - global section file                       */
  /*                           pages that are read or written are shared */
  /*                           writes go back to the section file        */

  /*  <ZEROES [EXPDWN]> - heap/stack section                  */
  /*                      may expand up or down               */
  /*                      zeroes are supplied on first access */
  /*                      each writer has own copy of page    */
  /*                      dirty pages go out to page file     */

  /*  <ZEROES GLOBAL SHRWRT> - shared memory                        */
  /*                           zeroes are supplied on first access  */
  /*                           all pages (read or write) are shared */
  /*                           dirty pages go out to page file      */

  /*  <PAGTBL ZEROES [EXPDWN]> - pagetable section                   */
  /*                             may expand up or down               */
  /*                             zeroes are supplied on first access */
  /*                             each writer has own copy of page    */
  /*                             dirty pages go out to page file     */

  /* - SHRWRT can only be used with GLOBAL sections.  SHRWRT can't work with private sections because if    */
  /*   a page is paged out, and then one process pages it in, the other processes would have no way to      */
  /*   know what phypage the page was loaded to, as they still point to the pagefile page.  Global sections */
  /*   have a common array that keep the current location of the page updated for everyone to see.          */

  if (((sectype & OZ_SECTION_TYPE_SHRWRT) && !(sectype & OZ_SECTION_TYPE_GLOBAL)) 

  /* - SECFIL can only be used with SHRWRT sections.  This is because if there are         */
  /*   multiple users (either after a fork or by multiple mappings of a global section),   */
  /*   there would be no way to tell whose copy would get written back to the section file */

   || ((sectype & OZ_SECTION_TYPE_SECFIL) && !(sectype & OZ_SECTION_TYPE_SHRWRT)) 

  /* - PAGTBL sections must originate from zeroes, not from the contents of some file */

   || ((sectype & OZ_SECTION_TYPE_PAGTBL) && !(sectype & OZ_SECTION_TYPE_ZEROES)) 

  /* - PAGTBL sections cannot be global as each process has its own pagetable(s) */

   || ((sectype & OZ_SECTION_TYPE_PAGTBL) &&  (sectype & OZ_SECTION_TYPE_GLOBAL)) 

  /* - ZEROES sections cannot be paged back to the file they came from because they didn't come from a file */

   || ((sectype & OZ_SECTION_TYPE_ZEROES) &&  (sectype & OZ_SECTION_TYPE_SECFIL))

  /* - must have either ZEROES or a file was given */

   || (!(sectype & OZ_SECTION_TYPE_ZEROES) ^ (file != NULL))) return (OZ_BADSECTIONTYPE);

  /* Create an event flag for pagefaulting.  Also used here temporarily for i/o. */

  sts = oz_knl_event_create (12, "pagefaulting", NULL, &event);
  if (sts != OZ_SUCCESS) return (sts);

  /* If doing a file, get info about it */

  if (!(sectype & OZ_SECTION_TYPE_ZEROES)) {
    memset (&fs_getinfo1, 0, sizeof fs_getinfo1);	/* clear fields we don't know about */
    sts = oz_knl_io (file, OZ_IO_FS_GETINFO1, sizeof fs_getinfo1, &fs_getinfo1);
    if (sts != OZ_SUCCESS) goto err;			/* check resultant status */
    l2blocksize = 0;					/* get LOG2 of the block size */
    sts = OZ_BADBLOCKSIZE;				/* (must be integral log2) */
    if (fs_getinfo1.blocksize == 0) goto err;
    while (!(fs_getinfo1.blocksize & 1)) {
      l2blocksize ++;
      fs_getinfo1.blocksize /= 2;
    }
    if (fs_getinfo1.blocksize != 1) goto err;
    if (l2blocksize > OZ_HW_L2PAGESIZE) goto err;	/* its blocksize must be <= my page size */
    sts = OZ_ENDOFFILE;					/* make sure they aren't trying to map past the eof */
    if (vbnstart > fs_getinfo1.eofblock) goto err;
    i = (fs_getinfo1.eofblock - vbnstart) >> (OZ_HW_L2PAGESIZE - l2blocksize); /* number of pages in file following vbnstart */
    if (i == 0) goto err;
    if (npages == 0) npages = i;			/* set default npages if not given */
    else if (npages > i) goto err;			/* make sure number given doesn't go past eof */
  }

  /* Make sure we have some pages */

  sts = OZ_ZEROSIZESECTION;
  if (npages == 0) goto err;

  /* Allocate a new section block and fill it in */

  gpas = 0;						/* calc global page array size */
  if (sectype & OZ_SECTION_TYPE_GLOBAL) gpas = npages;
  section = OZ_KNL_NPPMALLOQ ((uByte *)(section -> gblpages + gpas) - (uByte *)section); /* get basic block plus all gblpages */
  if (section == NULL) {
    sts = OZ_EXQUOTANPP;
    goto err;
  }
  section -> objtype     = OZ_OBJTYPE_SECTION;		/* set up objtype */
  section -> refcount    = 1;				/* initial ref count = 1 */
  section -> file        = NULL;			/* no file if just a bunch of zeroes */
  section -> l2blocksize = 0;
  if (!(sectype & OZ_SECTION_TYPE_ZEROES)) {
    section -> file      = file;			/* otherwise, save file pointer */
    section -> l2blocksize = l2blocksize;
    oz_knl_iochan_increfc (file, 1);			/* increment file ref count */
    section -> knlpfmap  = fs_getinfo1.knlpfmap;	/* maybe the fs will give us direct access to the file's cache pages */
    section -> knlpfupd  = fs_getinfo1.knlpfupd;
    section -> knlpfrel  = fs_getinfo1.knlpfrel;
  }
  section -> pagefile    = NULL;			/* don't have a pagefile yet */
  section -> pagefile_l2blocksize = 0;
  section -> pfrefcount  = 0;				/* don't have any pages in pagefile yet */
  section -> npages      = npages;			/* number of pages */
  section -> vbnstart    = vbnstart;			/* starting block in file */
  section -> sectype     = sectype;			/* section type */
  section -> pfevent     = event;			/* create pagefault event flag */
  section -> privproc    = NULL;			/* private section not yet mapped */
  section -> secattr     = secattr;			/* security attributes */
  if (section -> secattr != NULL) oz_knl_secattr_increfc (section -> secattr, 1);
  section -> quota       = OZ_KNL_QUOTA_DEFAULT;	/* get quota block to be charged for faulted-in pages */
  if (section -> quota != NULL) oz_knl_quota_increfc (section -> quota, 1);
  if (sectype & OZ_SECTION_TYPE_GLOBAL) oz_hw_smplock_init (sizeof section -> smplock_gp, &(section -> smplock_gp), OZ_SMPLOCK_LEVEL_GP);

  /* Mark all the global pages as 'initial load'                        */
  /* Private sections don't have anything to mark until they are mapped */

  for (i = 0; i < gpas; i ++) {
    section -> gblpages[i].pagestate = OZ_SECTION_PAGESTATE_PAGEDOUT;
    section -> gblpages[i].phypage   = 0;
  }

  *section_r = section;

  return (OZ_SUCCESS);

  /* Error return with status in sts */

err:
  oz_knl_event_increfc (event, -1);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Increment section reference count					*/
/*									*/
/*    Input:								*/
/*									*/
/*	section = pointer to section block				*/
/*	inc  = 1 : increment reference count				*/
/*	       0 : no-op						*/
/*	      -1 : decrement reference count				*/
/*									*/
/*	smplock = SOFTINT if decrementing 				*/
/*	            (because we call oz_knl_iochan_increfc)		*/
/*	          else, it doesn't matter				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_section_increfc = new reference count			*/
/*	if zero, section block is freed off				*/
/*									*/
/************************************************************************/

Long oz_knl_section_increfc (OZ_Section *section, Long inc)

{
  Long refc;
  OZ_Iochan *file;
  OZ_Mempage gblpage;
  OZ_Secattr *secattr;

  OZ_KNL_CHKOBJTYPE (section, OZ_OBJTYPE_SECTION);

  refc = oz_hw_atomic_inc_long (&(section -> refcount), inc);		/* update reference count and get new value */
  if (refc < 0) oz_crash ("oz_knl_section_increfc: section ref count negative"); /* should never be negative */
  if (refc == 0) {							/* check for zero */
    if (section -> sectype & OZ_SECTION_TYPE_GLOBAL) {			/* make sure all global pages are out */
      for (gblpage = 0; gblpage < section -> npages; gblpage ++) {
        if ((section -> gblpages[gblpage].pagestate != OZ_SECTION_PAGESTATE_PAGEDOUT) || 
            (section -> gblpages[gblpage].phypage   != 0)) {
          oz_crash ("oz_knl_section_increfc: refcount zero but global pagestate %d, phypage %x", section -> gblpages[gblpage].pagestate, section -> gblpages[gblpage].phypage);
        }
      }
    }
    file = section -> file;						/* all done with the section file */
    if (file != NULL) oz_knl_iochan_increfc (file, -1);
    secattr = section -> secattr;					/* all done with the security attributes */
    if (secattr != NULL) oz_knl_secattr_increfc (secattr, -1);
    if (section -> quota != NULL) oz_knl_quota_increfc (section -> quota, -1); /* all done with the quota block */
    oz_knl_event_increfc (section -> pfevent, -1);			/* all done with the pagefault event flag */
    OZ_KNL_NPPFREE (section);						/* all done with the section struct */
  }

  return (refc);							/* return new reference count */
}

/************************************************************************/
/*									*/
/*  Copy pages from the current process to the new process		*/
/*									*/
/*    Input:								*/
/*									*/
/*	newprocess  = process to copy the pages to			*/
/*	npages      = number of pages to copy				*/
/*	vpage       = starting page to copy				*/
/*	section     = section the pages are in				*/
/*	ownermode   = procmode that mapped the section			*/
/*	mapprot     = protection the section was mapped with		*/
/*	mapsecflags = flags the section was mapped with			*/
/*									*/
/*	smplevel = SOFTINT						*/
/*									*/
/************************************************************************/

uLong oz_knl_section_copypages (OZ_Process *newprocess, OZ_Mempage npages, OZ_Mempage vpage, OZ_Section **section_r, 
                                OZ_Procmode ownermode, OZ_Hw_pageprot mapprot, uLong mapsecflags)

{
  int nseclocks;
  OZ_Hw_pageprot curprot, reqprot, xcurprot, xreqprot;
  OZ_Mempage phypage, xphypage;
  OZ_Process *oldprocess;
  OZ_Section *newsection, *oldsection;
  OZ_Section_pagestate pagestate, xpagestate;
  uLong pt, sts;
  void *ptevaddr;

  oldsection = *section_r;
  OZ_KNL_CHKOBJTYPE (oldsection, OZ_OBJTYPE_SECTION);

  /* If global section, just use same exact section */

  if (oldsection -> sectype & OZ_SECTION_TYPE_GLOBAL) return (OZ_SUCCESS);

  /* Private section, create an identical section */

  sts = oz_knl_section_create (oldsection -> file, oldsection -> npages, oldsection -> vbnstart, 
                               oldsection -> sectype, oldsection -> secattr, &newsection);
  if (sts != OZ_SUCCESS) return (sts);

  newsection -> pagefile = oldsection -> pagefile;
  if (newsection -> pagefile != NULL) oz_knl_pagefile_increfc (newsection -> pagefile, 1);
  newsection -> pagefile_l2blocksize = oldsection -> pagefile_l2blocksize;

  /* Get rid of the old section and point to the new one */

  oz_knl_section_increfc (oldsection, -1);
  *section_r = newsection;

  /* Make it look like we copied the pages from the old to the new */

  oldprocess = oz_knl_process_getcur ();

  while (npages > 0) {

    /* Lock the old process' pagetable so it can't change on us */

lockoldpt:
    pt = oz_knl_process_lockpt (oldprocess);

    /* Read the old process' pte */

    ptevaddr = oz_hw_pte_readany (vpage, &pagestate, &phypage, &curprot, &reqprot);
    if (ptevaddr != NULL) {
      oz_knl_process_unlkpt (oldprocess, pt);
      sts = oz_knl_section_faultpage (OZ_PROCMODE_KNL, OZ_HW_VADDRTOVPAGE (ptevaddr), 0);
      if (sts != OZ_SUCCESS) {
        oz_knl_printk ("oz_knl_section_copypages: error %u faulting in old pte %p for page %x\n", sts, ptevaddr, vpage);
        return (sts);
      }
      goto lockoldpt;
    }

    /* Process based on the old page's state */

    switch (pagestate) {

      /* If it is in a pagefile page, increment ref count to the pagefile page */
      /* Note that each accessor will fault it separately from the pagefile    */

      case OZ_SECTION_PAGESTATE_PAGEDOUT: {
        if (phypage != 0) {
          oz_knl_pagefile_incpfrefc (newsection -> pagefile, phypage, 1);
          newsection -> pfrefcount ++;
        }
        break;
      }

      /* If the read or write failed, just copy the failure status as is */

      case OZ_SECTION_PAGESTATE_READFAILED:
      case OZ_SECTION_PAGESTATE_WRITEFAILED: {
        break;
      }

      /* If a read or write pagiing operation is in progress, wait for it to finish, then re-process the page */

      case OZ_SECTION_PAGESTATE_READINPROG:
      case OZ_SECTION_PAGESTATE_WRITEINPROG: {
        oz_knl_event_set (oldsection -> pfevent, 0);
        oz_knl_process_unlkpt (oldprocess, pt);
        oz_knl_event_waitone (oldsection -> pfevent);
        goto lockoldpt;
      }

      /* Page is in memory - mark it readonly for both parties and inc the phypage's ref count */
      /* Then, when someone tries to write to the page, and the ref count is still > 1, they   */
      /* will make a copy of the page first (and write to the copy), and they will dec this    */
      /* page's ref count.  If they find the ref count .eq. 1, then they can just write-enable */
      /* the page as is, as no one else is referencing it.                                     */

      case OZ_SECTION_PAGESTATE_VALID_D:
      case OZ_SECTION_PAGESTATE_VALID_R: {
        if (curprot != ppro[curprot]) oz_crash ("oz_knl_section_copypages: pagestate %d, curprot %d is not read-only", pagestate, curprot);
        if (phypage < oz_s_phymem_totalpages) {						/* in case it is some I/O controller or ramdisk page */
          if (oz_s_phymem_pages[phypage].state == OZ_PHYMEM_PAGESTATE_ALLOCSECT) {
            OZ_HW_ATOMIC_INCBY1_LONG (oz_s_phymem_pages[phypage].u.s.ptrefcount);	/* increment page's reference count */
          } else if (oz_s_phymem_pages[phypage].state == OZ_PHYMEM_PAGESTATE_ALLOCACHE) {
            oz_knl_cachepage_increfcby1 (phypage);					/* ... one way or the other */
          } else {
            oz_crash ("oz_knl_section_copypages: unknown phypage %u state %d", phypage, oz_s_phymem_pages[phypage].state);
          }
        }
        break;
      }

      case OZ_SECTION_PAGESTATE_VALID_W: {
        if (phypage < oz_s_phymem_totalpages) {						/* in case it is some I/O controller or ramdisk page */
          nseclocks = oz_knl_process_nseclocks (oldprocess, vpage, 0);			/* it is currently writable, make sure no I/O going on it */
          if (nseclocks != 0) {								/* ... because we are about to make it read-only */
            oz_knl_process_unlkpt (oldprocess, pt);
            oz_knl_printk ("oz_knl_section_copypages: secmap at vpage 0x%X has %d seclock(s)\n", vpage, nseclocks);
            return (OZ_PAGEIOBUSY);
          }
          pagestate = OZ_SECTION_PAGESTATE_VALID_D;					/* it is dirty, remember it is dirty (but allow it to be read-only) */
          curprot = ppro[reqprot];							/* get equivalent read-only prot code */
          oz_hw_pte_writeall (vpage, pagestate, phypage, curprot, reqprot);		/* set protection on old page to read-only */
          if (oz_s_phymem_pages[phypage].state == OZ_PHYMEM_PAGESTATE_ALLOCSECT) {
            OZ_HW_ATOMIC_INCBY1_LONG (oz_s_phymem_pages[phypage].u.s.ptrefcount);	/* increment page's reference count */
          } else if (oz_s_phymem_pages[phypage].state == OZ_PHYMEM_PAGESTATE_ALLOCACHE) {
            oz_knl_cachepage_increfcby1 (phypage);					/* ... one way or the other */
          } else {
            oz_crash ("oz_knl_section_copypages: unknown phypage %u state %d", phypage, oz_s_phymem_pages[phypage].state);
          }
        }
        break;
      }

      default: {
        oz_crash ("oz_knl_section_copypages: unknown page state %d", pagestate);
      }
    }

    oz_knl_process_unlkpt (oldprocess, pt);

    /* Write pte contents to new process */

    oz_knl_process_setcur (newprocess);							/* switch the pagetable we are using */
    pt = oz_knl_process_lockpt (newprocess);						/* lock that pagetable */
    while ((ptevaddr = oz_hw_pte_readany (vpage, &xpagestate, &xphypage, &xcurprot, &xreqprot)) != NULL) { /* make sure the pt we want is in */
      oz_knl_process_unlkpt (newprocess, pt);
      sts = oz_knl_section_faultpage (OZ_PROCMODE_KNL, OZ_HW_VADDRTOVPAGE (ptevaddr), 0); /* it's not in, fault it in */
      if (sts != OZ_SUCCESS) {
        oz_knl_process_setcur (oldprocess);
        oz_knl_printk ("oz_knl_section_copypages: error %u faulting in new pte %p for page %x\n", sts, ptevaddr, vpage);
        return (sts);
      }
      pt = oz_knl_process_lockpt (newprocess);
      if (oz_knl_process_getcur () != newprocess) oz_crash ("oz_knl_section_copypages: incorrect process after faulting new pagetable");
    }
    if ((xpagestate != OZ_SECTION_PAGESTATE_PAGEDOUT) || (xphypage != 0) || (xcurprot != OZ_HW_PAGEPROT_NA)) {
      oz_crash ("oz_knl_section_copypages: new pte is already in use, pagestate %d, phypage %X, curprot %d", xpagestate, xphypage, xcurprot);
											/** ?? we can recover this - it might happen ?? **/
    }
    oz_hw_pte_writecur (vpage, pagestate, phypage, curprot, reqprot);			/* write new pte contents */
    oz_knl_process_unlkpt (newprocess, pt);						/* unlock the new pagetable */
    oz_knl_process_setcur (oldprocess);							/* switch back to old pagetable */

    /* One less page to do */

    -- npages;
    vpage ++;
  }

  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Get number of pages in the section					*/
/*									*/
/************************************************************************/

OZ_Mempage oz_knl_section_getsecnpages (OZ_Section *section)

{
  OZ_KNL_CHKOBJTYPE (section, OZ_OBJTYPE_SECTION);
  return (section -> npages);
}

/************************************************************************/
/*									*/
/*  Get section type bitmask						*/
/*									*/
/************************************************************************/

OZ_Section_type oz_knl_section_getsectype (OZ_Section *section)

{
  OZ_KNL_CHKOBJTYPE (section, OZ_OBJTYPE_SECTION);
  return (section -> sectype);
}

/************************************************************************/
/*									*/
/*  Get section security attributes block pointer			*/
/*									*/
/************************************************************************/

OZ_Secattr *oz_knl_section_getsecattr (OZ_Section *section)

{
  OZ_KNL_CHKOBJTYPE (section, OZ_OBJTYPE_SECTION);
  oz_knl_secattr_increfc (section -> secattr, 1);
  return (section -> secattr);
}

/************************************************************************/
/*									*/
/*  Get section quota block pointer					*/
/*									*/
/************************************************************************/

OZ_Quota *oz_knl_section_getquota (OZ_Section *section)

{
  OZ_KNL_CHKOBJTYPE (section, OZ_OBJTYPE_SECTION);
  return (section -> quota);
}

/************************************************************************/
/*									*/
/*  Expand private demand-zero section					*/
/*									*/
/*    Input:								*/
/*									*/
/*	section = section pointer					*/
/*	npages  = number of pages to add to section			*/
/*									*/
/*    Note:								*/
/*									*/
/*	it is up to the caller to set the pte's for the new pages 	*/
/*	to OZ_SECTION_PAGESTATE_PAGEDOUT with phypage 0			*/
/*									*/
/************************************************************************/

void oz_knl_section_expand (OZ_Section *section, OZ_Mempage npages)

{
  OZ_KNL_CHKOBJTYPE (section, OZ_OBJTYPE_SECTION);

  /* Check for conflicting section type flags */

  if (section -> sectype & OZ_SECTION_TYPE_SECFIL) oz_crash ("oz_knl_section_expand: can't expand section file section");
  if (section -> sectype & OZ_SECTION_TYPE_GLOBAL) oz_crash ("oz_knl_section_expand: can't expand global section");
  if (!(section -> sectype & OZ_SECTION_TYPE_ZEROES)) oz_crash ("oz_knl_section_expand: can't expand non-zeroed section");

  /* Set up new number of pages */

  section -> npages += npages;
}

/************************************************************************/
/*									*/
/*  This routine is called by the section mapping routines of process.c	*/
/*  If it is a private section, it checks to see if it has already 	*/
/*  been mapped.  If so, it returns an error.  It also sees if the 	*/
/*  correct process is trying to map it, and returns an error if not.   */
/*  Finally, global or private, it increments the reference count.	*/
/*									*/
/*    Input:								*/
/*									*/
/*	section = section being mapped					*/
/*	process = process it is being mapped to				*/
/*									*/
/*	smp lock <= ps							*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_section_mapproc = OZ_SUCCESS : successful		*/
/*	                  OZ_PRIVSECMULTMAP : trying to map private section more than once
/*									*/
/************************************************************************/

uLong oz_knl_section_mapproc (OZ_Section *section, OZ_Process *process)

{
  OZ_KNL_CHKOBJTYPE (section, OZ_OBJTYPE_SECTION);
  OZ_KNL_CHKOBJTYPE (process, OZ_OBJTYPE_PROCESS);

  if (!(section -> sectype & OZ_SECTION_TYPE_GLOBAL)) {		/* see if private section */
    if (!oz_hw_atomic_setif_ptr (&(section -> privproc), process, NULL)) return (OZ_PRIVSECMULTMAP);
  }
  return (OZ_SUCCESS);						/* successful */
}

/************************************************************************/
/*									*/
/*  This routine is called by process.c when it has unmapped a 		*/
/*  section.  For private sections, the section is marked unmapped.  	*/
/*									*/
/*    Input:								*/
/*									*/
/*	section = section being mapped					*/
/*	process = process it is being unmapped from			*/
/*									*/
/*	smp lock <= ps							*/
/*									*/
/************************************************************************/

void oz_knl_section_unmapproc (OZ_Section *section, OZ_Process *process)

{
  uLong ps;

  OZ_KNL_CHKOBJTYPE (process, OZ_OBJTYPE_PROCESS);
  OZ_KNL_CHKOBJTYPE (section, OZ_OBJTYPE_SECTION);

  if (!(section -> sectype & OZ_SECTION_TYPE_GLOBAL)) {		/* check for private section */
    do {
      if (section -> privproc != process) oz_crash ("private section being unmapped by wrong process (or was never mapped)");
      section -> npages = 0;					/* it no longer has any pages */
    } while (!oz_hw_atomic_setif_ptr (&(section -> privproc), NULL, process));
  }
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
  const uByte *sp;
  OZ_Process *process;
  uLong bc, cc, pt, sts;
  uByte *dp;

  bc = size;						/* bytecount to copy */
  sp = usrc;						/* source procmode buffer pointer */
  dp = kdst;						/* dest kernel buffer pointer */

  process = getprocess (OZ_HW_VADDRTOVPAGE (usrc));

  pt = oz_knl_process_lockpt (process);			/* don't let other threads change mapping */
  while (bc != 0) {					/* repeat while more to do */
    while (!OZ_HW_READABLE (1, sp, procmode)) {		/* see if source readable by given access mode */
      oz_knl_process_unlkpt (process, pt);		/* not readable, release lock */
      sts = oz_knl_section_faultpage (procmode, OZ_HW_VADDRTOVPAGE (sp), 0); /* fault it in */
      if (sts != OZ_SUCCESS) return (sts);		/* if it can't fault it in, return error status */
      pt = oz_knl_process_lockpt (process);		/* maybe it is faulted in now, try again */
    }
    cc = (1 << OZ_HW_L2PAGESIZE) - (((OZ_Pointer)sp) & ((1 << OZ_HW_L2PAGESIZE) - 1)); /* readable, get number of bytes left in source page */
    if (cc > bc) cc = bc;				/* if more than wanted, just get number wanted */
    memcpy (dp, sp, cc);				/* copy the data */
    bc -= cc;						/* decrement amount left to do */
    sp += cc;						/* increment pointers */
    dp += cc;
  }
  oz_knl_process_unlkpt (process, pt);			/* all copied, release lock */
  return (OZ_SUCCESS);					/* return success status */
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
  const uByte *sp;
  OZ_Process *process;
  uByte *dp, xx;
  uLong bc, cc, pt, sts;

  bc = size;						/* bytecount to copy */
  sp = usrc;						/* source procmode buffer pointer */
  dp = kdst;						/* dest kernel buffer pointer */

  process = getprocess (OZ_HW_VADDRTOVPAGE (usrc));

  pt = oz_knl_process_lockpt (process);			/* don't let other threads change mapping */
  while (bc != 0) {					/* repeat while more to do */
    while (!OZ_HW_READABLE (1, sp, procmode)) {		/* see if source readable by given access mode */
      oz_knl_process_unlkpt (process, pt);		/* not readable, release lock */
      sts = oz_knl_section_faultpage (procmode, OZ_HW_VADDRTOVPAGE (sp), 0); /* fault it in */
      if (sts != OZ_SUCCESS) return (sts);		/* if it can't fault it in, return error status */
      pt = oz_knl_process_lockpt (process);		/* maybe it is faulted in now, try again */
    }
    cc = (1 << OZ_HW_L2PAGESIZE) - (((OZ_Pointer)sp) & ((1 << OZ_HW_L2PAGESIZE) - 1)); /* readable, get number of bytes left in source page */
    if (cc > bc) cc = bc;				/* if more than wanted, just get number wanted */
    do { *(dp ++) = xx = *(sp ++); -- bc; } while ((-- cc != 0) && (xx != 0)); /* copy data up to a null or end */
  }
  oz_knl_process_unlkpt (process, pt);			/* all copied, release lock */
  if (len_r != NULL) *len_r = size - bc;		/* maybe return actual number of bytes copied */
  return (OZ_SUCCESS);					/* return success status */
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
  uByte *dp;
  const uByte *sp;
  OZ_Process *process;
  uLong bc, cc, pt, sts;

  bc = size;						/* bytecount to copy */
  sp = ksrc;						/* source kernel buffer pointer */
  dp = udst;						/* dest procmode buffer pointer */

  process = getprocess (OZ_HW_VADDRTOVPAGE (udst));

  pt = oz_knl_process_lockpt (process);			/* don't let other threads change mapping */
  while (bc != 0) {					/* repeat while more to do */
    while (!OZ_HW_WRITABLE (1, dp, procmode)) {		/* see if destination writable by given access mode */
      oz_knl_process_unlkpt (process, pt);		/* not writable, release lock */
      sts = oz_knl_section_faultpage (procmode, OZ_HW_VADDRTOVPAGE (dp), 1); /* fault it in */
      if (sts != OZ_SUCCESS) return (sts);		/* if it can't fault it in, return error status */
      pt = oz_knl_process_lockpt (process);		/* maybe it is faulted in now, try again */
    }
    cc = (1 << OZ_HW_L2PAGESIZE) - (((OZ_Pointer)dp) & ((1 << OZ_HW_L2PAGESIZE) - 1)); /* writable, get number of bytes left in destination page */
    if (cc > bc) cc = bc;				/* if more than wanted, just get number wanted */
    memcpy (dp, sp, cc);				/* copy the data */
    bc -= cc;						/* decrement amount left to do */
    sp += cc;						/* increment pointers */
    dp += cc;
  }
  oz_knl_process_unlkpt (process, pt);			/* all copied, release lock */
  return (OZ_SUCCESS);					/* return success status */
}

/************************************************************************/
/*									*/
/*  These routines lock a buffer of the current process in memory	*/
/*									*/
/************************************************************************/

/************************************************************************/
/*									*/
/*  Same as oz_knl_section_iolock except it stops at a terminating null	*/
/*  and is only for read-only access to the buffer			*/
/*									*/
/*    Input:								*/
/*									*/
/*	procmode = processor mode of access				*/
/*	size = maximum string size to search for null char		*/
/*	buff = start address of the string				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_section_iolockz = OZ_SUCCESS : successful		*/
/*	                               else : error status		*/
/*	*rlen       = number of bytes in string (excluding the null)	*/
/*	*phypages_r = physical page array pointer			*/
/*	*npages_r   = number of physical pages that were locked		*/
/*	*byteoffs_r = offset in first physical page			*/
/*									*/
/************************************************************************/

uLong oz_knl_section_iolockz (OZ_Procmode procmode, uLong size, const void *buff, uLong *rlen, 
                              OZ_Seclock **seclock_r, OZ_Mempage *npages_r, const OZ_Mempage **phypages_r, uLong *byteoffs_r)

{
  const uByte *b;
  OZ_Hw_pageprot curprot, reqprot;
  OZ_Mempage i, npages, svpage;
  OZ_Process *process;
  OZ_Section_pagestate pagestate;
  OZ_Seclock **lastseclock, *seclock;
  uLong m, n, pt, s, sts;

  /* If size is zero, nothing to lock, so return a null pointer */

  *seclock_r = NULL;
  if (npages_r   != NULL) *npages_r   = 0;
  if (phypages_r != NULL) *phypages_r = NULL;
  if (byteoffs_r != NULL) *byteoffs_r = 0;
  if (size == 0) return (OZ_SUCCESS);

  /* Make sure size doesn't wrap */

  if (((uByte *)buff) + size < (uByte *)buff) return (OZ_BADBUFFERSIZE);

  /* Get number of pages and starting virtual page */

  svpage  = OZ_HW_VADDRTOVPAGE (buff);							/* get starting virtual page */
  npages  = OZ_HW_VADDRTOVPAGE (((uByte *)buff) + size - 1) - svpage + 1;		/* get number of pages to lock */
  process = getprocess (svpage);							/* get the process (either current or system) */

  /* Allocate seclock block to hold the info */

  seclock = OZ_KNL_NPPMALLOQ (npages * sizeof seclock -> a.phypages[0] + sizeof *seclock);
  if (seclock == NULL) return (OZ_EXQUOTANPP);						/* allocate seclock big enough to hold phypages array */
  seclock -> objtype = OZ_OBJTYPE_SECLOCK;
  seclock -> process = process;								/* the process the seclock belongs to */
  seclock -> svpage  = svpage;								/* save starting virtual page */

  /* Lock the pagetables so they can't change on us */

scanit:
  pt = oz_knl_process_lockpt (process);

  /* See if all the pages are accessible and what the corresponding physical page is.  If a page is not     */
  /* accessible, fault it in then re-scan the whole list (maybe the idiot thing faulted out a page we want) */

  s = size;
  b = buff;
  for (i = 0; i < npages; i ++) {							/* scan each page in question */
    if ((oz_hw_pte_readany (svpage + i, &pagestate, seclock -> a.phypages + i, &curprot, &reqprot) != NULL) /* see if it is readable */
     || !protable[curprot][procmode][0]) {
      oz_knl_process_unlkpt (process, pt);						/* it's not, unlock page table */
      sts = oz_knl_section_faultpage (procmode, svpage + i, 0);				/* try to fault it in */
      if (sts == OZ_SUCCESS) goto scanit;						/* retry if it says to */
      OZ_KNL_NPPFREE (seclock);								/* hard failure, release block */
      oz_knl_printk ("oz_knl_section_iolockz*: error %u locking %X (%p) for %p\n", sts, svpage + i, OZ_HW_VPAGETOVADDR (svpage + i), oz_hw_getrtnadr (0));
      return (sts);									/* return failure status */
    }
    m = (1 << OZ_HW_L2PAGESIZE) - (((OZ_Pointer)b) % (1 << OZ_HW_L2PAGESIZE));		/* get max length to scan this time */
    if (m > s) m = s;
    n  = strnlen (b, m);								/* get num bytes till null or reached max */
    s -= n;										/* subtract from size remaining to scan */
    b += n;										/* add to buffer address to resume scanning */
    if ((n < m) || (s == 0)) {								/* stop scanning if hit null or end of buffer */
      npages = i + 1;
      break;
    }
  }
  seclock -> npages = npages;								/* save actual number of pages being locked */

  /* Find out if there is a section mapping to lock to now that we know how many pages are involved */

  lastseclock = oz_knl_process_seclocks (process, npages, svpage);
  if (lastseclock == NULL) {
    oz_knl_printk ("oz_knl_section_iolockz*: can't find secmap for 0x%x pages at 0x%x\n", npages, svpage);
    oz_knl_process_unlkpt (process, pt);
    OZ_KNL_NPPFREE (seclock);
    return (OZ_ACCVIO);
  }

  /* All the pages are accessible without having released the lock, so link the seclock     */
  /* onto the section map's seclock list.  This is what actually locks the pages in memory. */

  vfyseclocks (process, svpage);
  seclock -> next = *lastseclock;
  seclock -> prev = lastseclock;
  *lastseclock = seclock;
  if (seclock -> next != NULL) seclock -> next -> prev = &(seclock -> next);
  oz_knl_process_nseclocks (process, svpage, 1);
  vfyseclocks (process, svpage);

  /* Now the pages are locked, so release the pagetable lock and return success status.  The  */
  /* pages can't be unmapped from the virtual address as long as the seclock is in place.     */

  oz_knl_process_unlkpt (process, pt);
  *seclock_r = seclock;
  if (rlen       != NULL) *rlen       = size - s;
  if (npages_r   != NULL) *npages_r   = npages;
  if (phypages_r != NULL) *phypages_r = seclock -> a.phypages;
  if (byteoffs_r != NULL) *byteoffs_r = ((OZ_Pointer)(buff)) & ((1 << OZ_HW_L2PAGESIZE) - 1);
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  This routine locks a buffer in memory				*/
/*									*/
/*    Input:								*/
/*									*/
/*	procmode = processor mode of the access				*/
/*	size = number of bytes in users buffer				*/
/*	buff = user buffer address					*/
/*	writing = 0 : read access required				*/
/*	          1 : write access required				*/
/*									*/
/*	ipl = softint							*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_section_iolock = OZ_SUCCESS : buffer is locked		*/
/*	                        OZ_ACCVIO : user doesn't have access	*/
/*	                             else : i/o error			*/
/*	*seclock_r  = param to pass to oz_knl_section_iounlk		*/
/*									*/
/*	*nelements_r     = total number of elements in ieeedmaXXs 	*/
/*	                   array, including all link elements		*/
/*	*ieeedma32s_va_r = virtual address of ieeedmaXX array		*/
/*	*ieeedma32s_pa_r = physical address of first element		*/
/*									*/
/*	*npages_r   = number of pages locked in memory			*/
/*	*phypages_r = pointer to array of physical page numbers		*/
/*	*byteoffs_r = byte offset in first physical page		*/
/*									*/
/************************************************************************/

	/*************************************************/
	/* This version returns an IEEE 32-bit DMA array */
	/*************************************************/

uLong oz_knl_section_iolock_ieeedma32 (OZ_Procmode procmode, uLong size, const void *buff, int writing, OZ_Seclock **seclock_r, 
                                       OZ_Mempage *nelements_r, const OZ_Ieeedma32 **ieeedma32s_va_r, OZ_Phyaddr *ieeedma32s_pa_r)

{
  OZ_Ieeedma32 *ieeedma32s;
  OZ_Mempage i, j, npages, phypage;
  OZ_Phyaddr array_pa;
  OZ_Seclock *seclock;
  uLong array_bo, array_ln, byteoffs, sts;

  if (sizeof seclock -> a.phypages[0] > sizeof seclock -> a.ieeedma32s[0]) {
    oz_crash ("oz_knl_section_iolock_ieeedma32: array element size too small");
  }

  *nelements_r     = 0;
  *ieeedma32s_va_r = NULL;
  *ieeedma32s_pa_r = 0;

  sts = oz_knl_section_iolock (procmode, size, buff, writing, seclock_r, &npages, NULL, &byteoffs);

  if ((sts == OZ_SUCCESS) && (npages != 0)) {
    seclock = *seclock_r;							// point to allocated seclock struct
    ieeedma32s = seclock -> a.ieeedma32s + 1;					// get where to put IEEE 32-bit DMA array
    (OZ_Pointer)ieeedma32s &= - sizeof *ieeedma32s;				// make sure it is naturally aligned
										// ... so an element can't straddle a page boundary
    for (i = npages; i > 0;) {							// start at end so we don't overwrite phypages
      phypage = seclock -> a.phypages[--i];					// get a physical page number
      ieeedma32s[i].bytecnt = 1 << OZ_HW_L2PAGESIZE;				// assume element is for an whole page
      ieeedma32s[i].phyaddr = phypage << OZ_HW_L2PAGESIZE;			// get physical page's corresponding address
      if ((ieeedma32s[i].phyaddr >> OZ_HW_L2PAGESIZE) != phypage) goto badaddr;	// barf if it doesn't fit in 32 bits
    }
    ieeedma32s[0].bytecnt -= byteoffs;						// this fewer bytes in the first page
    ieeedma32s[0].phyaddr += byteoffs;						// ... and it starts this much into page
    ieeedma32s[npages-1].bytecnt -= (1 << OZ_HW_L2PAGESIZE) - ((size + byteoffs) & ((1 << OZ_HW_L2PAGESIZE) - 1)); // chop last page
    j = 0;									// initialize output index
    for (i = 1; i < npages; i ++) {						// scan through input elements
      if ((ieeedma32s[i].phyaddr == (ieeedma32s[j].phyaddr + ieeedma32s[j].bytecnt)) // see if they mate up exactly
       && ((ieeedma32s[j].bytecnt + ieeedma32s[i].bytecnt) <= 0x7FFFFFFF)) {	// ... and the combined size doesn't overflow
        ieeedma32s[j].bytecnt += ieeedma32s[i].bytecnt;				// if so, consolidate them
      } else if (i != ++ j) {
        ieeedma32s[j] = ieeedma32s[i];						// if not, start a new one
      }
    }
    ++ j;
    for (i = 0;;) {
      array_ln = oz_knl_misc_sva2pa (ieeedma32s + i, &phypage, &array_bo);	// see how much is left in the page
      array_pa = (phypage << OZ_HW_L2PAGESIZE) + array_bo;			// and see what its physical address is
      if (i == 0) *ieeedma32s_pa_r = array_pa;					// (return 1st one to caller)
      i += array_ln / sizeof *ieeedma32s;					// increment i to end of the page
      if (i >= j) break;							// if the array is all on the page, we're done
      memmove (ieeedma32s + i, ieeedma32s + i - 1, (++ j - i) * sizeof *ieeedma32s); // it overflows page, make room for link
      if (i == 1) {								// see if it's the very first element
        -- j;									// if so, just discard it
        i = 0;									// (no sense making first element a link)
        ieeedma32s ++;
      } else {
        ieeedma32s[i-1].phyaddr  = array_pa;					// if not, link old page to new page
        ieeedma32s[i-1].bytecnt  = (j - i) * sizeof *ieeedma32s;		// it's at least this far to end or next link
        ieeedma32s[i-1].bytecnt |= 0x80000000UL;				// set the 'link' bit
      }
    }
    *nelements_r = j;								// return number of resulting entries
    *ieeedma32s_va_r = ieeedma32s + 1;						// return pointer to IEEE 32-bit dma array
  }

  return (sts);

  /* Physical address doesn't fit in 32 bits */

badaddr:
  oz_knl_section_iounlk (seclock);
  return (OZ_BADIEEEPHYADR);
}

	/*************************************************/
	/* This version returns an IEEE 64-bit DMA array */
	/*************************************************/

uLong oz_knl_section_iolock_ieeedma64 (OZ_Procmode procmode, uLong size, const void *buff, int writing, OZ_Seclock **seclock_r, 
                                       OZ_Mempage *nelements_r, const OZ_Ieeedma64 **ieeedma64s_va_r, OZ_Phyaddr *ieeedma64s_pa_r)

{
  OZ_Ieeedma64 *ieeedma64s;
  OZ_Mempage i, j, npages, phypage;
  OZ_Phyaddr array_pa;
  OZ_Seclock *seclock;
  uLong array_bo, array_ln, byteoffs, sts;

  if (sizeof seclock -> a.phypages[0] > sizeof seclock -> a.ieeedma64s[0]) {
    oz_crash ("oz_knl_section_iolock_ieeedma64: array element size too small");
  }

  *nelements_r     = 0;
  *ieeedma64s_va_r = NULL;
  *ieeedma64s_pa_r = 0;

  sts = oz_knl_section_iolock (procmode, size, buff, writing, seclock_r, &npages, NULL, &byteoffs);

  if ((sts == OZ_SUCCESS) && (npages != 0)) {
    seclock = *seclock_r;							// point to allocated seclock struct
    ieeedma64s = seclock -> a.ieeedma64s + 1;					// get where to put IEEE 64-bit DMA array
    (OZ_Pointer)ieeedma64s &= - sizeof *ieeedma64s;				// make sure it is naturally aligned
										// ... so an element can't straddle a page boundary
    for (i = npages; i > 0;) {							// start at end so we don't overwrite phypages
      phypage = seclock -> a.phypages[--i];					// get a physical page number
      ieeedma64s[i].bytecnt = 1 << OZ_HW_L2PAGESIZE;				// assume element is for an whole page
      ieeedma64s[i].phyaddr = phypage << OZ_HW_L2PAGESIZE;			// get physical page's corresponding address
      if ((ieeedma64s[i].phyaddr >> OZ_HW_L2PAGESIZE) != phypage) goto badaddr;	// barf if it doesn't fit in 64 bits
    }
    ieeedma64s[0].bytecnt -= byteoffs;						// this fewer bytes in the first page
    ieeedma64s[0].phyaddr += byteoffs;						// ... and it starts this much into page
    ieeedma64s[npages-1].bytecnt -= (1 << OZ_HW_L2PAGESIZE) - ((size + byteoffs) & ((1 << OZ_HW_L2PAGESIZE) - 1)); // chop last page
    j = 0;									// initialize output index
    for (i = 1; i < npages; i ++) {						// scan through input elements
      if ((ieeedma64s[i].phyaddr == (ieeedma64s[j].phyaddr + ieeedma64s[j].bytecnt)) // see if they mate up exactly
       && ((ieeedma64s[j].bytecnt + ieeedma64s[i].bytecnt) <= 0x7FFFFFFF)) {	// ... and the combined size doesn't overflow
        ieeedma64s[j].bytecnt += ieeedma64s[i].bytecnt;				// if so, consolidate them
      } else if (i != ++ j) {
        ieeedma64s[j] = ieeedma64s[i];						// if not, start a new one
      }
    }
    ++ j;
    for (i = 0;;) {
      array_ln = oz_knl_misc_sva2pa (ieeedma64s + i, &phypage, &array_bo);	// see how much is left in the page
      array_pa = (phypage << OZ_HW_L2PAGESIZE) + array_bo;			// and see what its physical address is
      if (i == 0) *ieeedma64s_pa_r = array_pa;					// (return 1st one to caller)
      i += array_ln / sizeof *ieeedma64s;					// increment i to end of the page
      if (i >= j) break;							// if the array is all on the page, we're done
      memmove (ieeedma64s + i, ieeedma64s + i - 1, (++ j - i) * sizeof *ieeedma64s); // it overflows page, make room for link
      if (i == 1) {								// see if it's the very first element
        -- j;									// if so, just discard it
        i = 0;									// (no sense making first element a link)
        ieeedma64s ++;
      } else {
        ieeedma64s[i-1].phyaddr  = array_pa;					// if not, link old page to new page
        ieeedma64s[i-1].bytecnt  = (j - i) * sizeof *ieeedma64s;		// it's at least this far to end or next link
        ieeedma64s[i-1].bytecnt |= 0x8000000000000000ULL;			// set the 'link' bit
      }
    }
    *nelements_r = j;								// return number of resulting entries
    *ieeedma64s_va_r = ieeedma64s + 1;						// return pointer to IEEE 64-bit dma array
  }

  return (sts);

  /* Physical address doesn't fit in 64 bits */

badaddr:
  oz_knl_section_iounlk (seclock);
  return (OZ_BADIEEEPHYADR);
}

	/*******************************************************************************************/
	/* This version returns an array of physical page numbers with starting page's byte offset */
	/*******************************************************************************************/

uLong oz_knl_section_iolock (OZ_Procmode procmode, uLong size, const void *buff, int writing, OZ_Seclock **seclock_r, 
                             OZ_Mempage *npages_r, const OZ_Mempage **phypages_r, uLong *byteoffs_r)

{
  OZ_Hw_pageprot curprot, reqprot;
  OZ_Mempage i, npages, svpage;
  OZ_Process *process;
  OZ_Section_pagestate pagestate;
  OZ_Seclock **lastseclock, *seclock;
  uLong pt, sts;

  /* If size is zero, nothing to lock, so return a null pointer */

  *seclock_r = NULL;
  if (npages_r   != NULL) *npages_r   = 0;
  if (phypages_r != NULL) *phypages_r = NULL;
  if (byteoffs_r != NULL) *byteoffs_r = 0;
  if (size == 0) return (OZ_SUCCESS);

  /* Make sure size doesn't wrap */

  if ((OZ_Pointer)buff + size < (OZ_Pointer)buff) return (OZ_BADBUFFERSIZE);

  /* Get number of pages and starting virtual page */

  svpage  = OZ_HW_VADDRTOVPAGE (buff);							/* get starting virtual page */
  npages  = OZ_HW_VADDRTOVPAGE (((uByte *)buff) + size - 1) - svpage + 1;		/* get number of pages to lock */
  process = getprocess (svpage);							/* get the process (either current or system) */

  /* Allocate seclock block to hold the info.  Allocate for the worst case scenario, ie, an IEEE 64-bit dma descriptor array. */

  sts  = (npages + 1) * (sizeof seclock -> a);						/* this is the size of the array portion, with extra element for alignment */
  sts += (sts >> OZ_HW_L2PAGESIZE) * (sizeof seclock -> a);				/* this adds room for IEEE dma link elements, one per page of the array */
											/* - the + sizeof *seclock in the next line takes care of having */
											/*   to round up because it includes one element of the array */
  seclock = OZ_KNL_NPPMALLOQ (sts + sizeof *seclock);					/* allocate seclock big enough to hold the entire struct */
  if (seclock == NULL) return (OZ_EXQUOTANPP);
  seclock -> objtype = OZ_OBJTYPE_SECLOCK;
  seclock -> process = process;								/* the process the seclock belongs to */
  seclock -> npages  = npages;								/* save number of pages */
  seclock -> svpage  = svpage;								/* save starting virtual page */

  /* Lock the pagetables so they can't change on us */

scanit:
  pt = oz_knl_process_lockpt (process);

  /* Find out if there is a section mapping to lock to */

  lastseclock = oz_knl_process_seclocks (process, npages, svpage);
  if (lastseclock == NULL) {
    oz_knl_process_unlkpt (process, pt);
    OZ_KNL_NPPFREE (seclock);
    return (OZ_ACCVIO);
  }

  /* See if all the pages are accessible and what the corresponding physical page is.  If a page is not     */
  /* accessible, fault it in then re-scan the whole list (maybe the idiot thing faulted out a page we want) */

  for (i = 0; i < npages; i ++) {							/* scan each page in question */
    if ((oz_hw_pte_readany (svpage + i, &pagestate, seclock -> a.phypages + i, &curprot, &reqprot) != NULL) /* see if it is accessible */
     || !protable[curprot][procmode][writing]) {
      oz_knl_process_unlkpt (process, pt);						/* it's not, unlock page table */
      sts = oz_knl_section_faultpage (procmode, svpage + i, writing);			/* try to fault it in */
      if (sts == OZ_SUCCESS) goto scanit;						/* retry if it says to */
      OZ_KNL_NPPFREE (seclock);								/* hard failure, release block */
      return (sts);									/* return failure status */
    }
  }

  /* All the pages are accessible without having released the lock, so link the seclock     */
  /* onto the section map's seclock list.  This is what actually locks the pages in memory. */

  vfyseclocks (process, svpage);
  seclock -> next = *lastseclock;
  seclock -> prev = lastseclock;
  *lastseclock = seclock;
  if (seclock -> next != NULL) seclock -> next -> prev = &(seclock -> next);
  oz_knl_process_nseclocks (process, svpage, 1);
  vfyseclocks (process, svpage);

  /* Now the pages are locked, so release the pagetable lock and return success status.  The  */
  /* pages can't be unmapped from the virtual address as long as the seclock is in place.     */

  oz_knl_process_unlkpt (process, pt);
  *seclock_r = seclock;
  if (npages_r   != NULL) *npages_r   = npages;
  if (phypages_r != NULL) *phypages_r = seclock -> a.phypages;
  if (byteoffs_r != NULL) *byteoffs_r = ((OZ_Pointer)(buff)) & ((1 << OZ_HW_L2PAGESIZE) - 1);
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Unlock previously locked buffer					*/
/*									*/
/*    Input:								*/
/*									*/
/*	seclock = as returned by oz_knl_section_iolock			*/
/*	smplevel < pt							*/
/*									*/
/*    Output:								*/
/*									*/
/*	pages are unlocked						*/
/*	seclock no longer usable					*/
/*									*/
/************************************************************************/

void oz_knl_section_iounlk (OZ_Seclock *seclock)

{
  OZ_Process *process;
  OZ_Seclkwz **lseclkwz, *seclkwz;
  uLong pt;

  if (seclock != NULL) {
    OZ_KNL_CHKOBJTYPE (seclock, OZ_OBJTYPE_SECLOCK);				/* make sure they passed a seclock */
    process = seclock -> process;						/* get the process (either current or system) that has the pagetable */
    pt = oz_knl_process_lockpt (process);					/* lock the pagetable so stuff can't get added while we remove the seclock */
    vfyseclocks (process, seclock -> svpage);
    *(seclock -> prev) = seclock -> next;					/* unlink seclock, this is what unlocks the pages */
    if (seclock -> next != NULL) seclock -> next -> prev = seclock -> prev;
    seclock -> process = NULL;
    seclock -> prev    = NULL;
    oz_knl_process_nseclocks (process, seclock -> svpage, -1);
    vfyseclocks (process, seclock -> svpage);
    lseclkwz = oz_knl_process_seclkwzs (process, seclock -> svpage);		/* wake those waiting for a zero io reference count */
    while ((seclkwz = *lseclkwz) != NULL) {
      if ((seclkwz -> vpage < seclock -> svpage) || (seclkwz -> vpage - seclock -> svpage >= seclock -> npages)) lseclkwz = &(seclkwz -> next);
      else {
        *lseclkwz = seclkwz -> next;						/* just unlink but don't free it */
        if (seclkwz -> next != NULL) seclkwz -> next -> prev = lseclkwz;
        oz_knl_process_nseclkwzs (process, seclock -> svpage, -1);
        seclkwz -> prev = NULL;							/* let waitioidle routine know we unlinked it */
        vfyseclocks (process, seclock -> svpage);
        oz_knl_event_set (seclkwz -> event, 1);					/* and set the event flag to wake waiter */
      }
    }
    vfyseclocks (process, seclock -> svpage);
    oz_knl_process_unlkpt (process, pt);					/* release the pagetable */
    OZ_KNL_NPPFREE (seclock);							/* free off the seclock */
  }
}

/* Verify a process' seclocks and seclkwzs lists for a given vpage */

static void vfyseclocks (OZ_Process *process, OZ_Mempage vpage)

{
  int i, n;
  OZ_Seclkwz **lseclkwz, *seclkwz;
  OZ_Seclock **lseclock, *seclock;

  lseclock = oz_knl_process_seclocks (process, 1, vpage);
  i = 0;
  while ((seclock = *lseclock) != NULL) {
    if (seclock -> prev != lseclock) oz_crash ("oz_knl_section vfyseclocks: seclock %p -> prev = %p, should be %p", seclock, seclock -> prev, lseclock);
    if (seclock -> process != process) oz_crash ("oz_knl_section vfyseclocks: seclock %p -> process = %p, should be %p", seclock, seclock -> process, process);
    lseclock = &(seclock -> next);
    i ++;
  }
  n = oz_knl_process_nseclocks (process, vpage, 0);
  if (i != n) oz_crash ("oz_knl_section vfyseclocks: %d seclocks, should have %d", i, n);

  lseclkwz = oz_knl_process_seclkwzs (process, vpage);
  i = 0;
  while ((seclkwz = *lseclkwz) != NULL) {
    if (seclkwz -> prev != lseclkwz) oz_crash ("oz_knl_section vfyseclkwzs: seclkwz %p -> prev = %p, should be %p", seclkwz, seclkwz -> prev, lseclkwz);
    lseclkwz = &(seclkwz -> next);
    i ++;
  }
  n = oz_knl_process_nseclkwzs (process, vpage, 0);
  if (i != n) oz_crash ("oz_knl_section vfyseclkwzs: %d seclkwzs, should have %d", i, n);
}

/************************************************************************/
/*									*/
/*  Get a page's protection code					*/
/*									*/
/*    Input:								*/
/*									*/
/*	vpage = page to get protection for				*/
/*	smplevel = null or softint					*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_section_getpageprot = OZ_SUCCESS : successful		*/
/*	                                   else : error status		*/
/*	*pageprot_r = page's protection code				*/
/*									*/
/************************************************************************/

uLong oz_knl_section_getpageprot (OZ_Mempage vpage, OZ_Hw_pageprot *pageprot_r)

{
  OZ_Mempage pageoffs;
  OZ_Process *process;
  OZ_Procmode procmode;
  OZ_Section *section;
  uLong mapsecflags, pt, sts;
  void *ptevaddr;

  process = getprocess (vpage);								/* get process that owns the tables */
start:
  pt = oz_knl_process_lockpt (process);							/* lock the process' pagetables */
  ptevaddr = oz_hw_pte_readany (vpage, NULL, NULL, NULL, pageprot_r);			/* read the current pte, get reqprot */
  if (ptevaddr != NULL) {

    /* The pagetable page is out.  If the page is is demand zero and there is a section mapped   */
    /* at 'vpage', we can use the default section protection without having to read the page in. */

    if (!itsadzvaddr (ptevaddr) 
     || (oz_knl_process_getsecfromvpage (process, 
                                         vpage, 
                                         &section, 
                                         &pageoffs, 
                                         pageprot_r, 
                                         &procmode, 
                                         &mapsecflags) == 0)) {
      oz_knl_process_unlkpt (process, pt);						/* it's faulted out, get it back */
      sts = oz_knl_section_faultpage (OZ_PROCMODE_KNL, OZ_HW_VADDRTOVPAGE (ptevaddr), 1);
      if (sts == OZ_SUCCESS) goto start;						/* repeat now that it is back */
      return (sts);									/* can't get it back, return error sts */
    }
  }
  oz_knl_process_unlkpt (process, pt);							/* done, unlock pagetables */
  return (OZ_SUCCESS);									/* success */
}

/************************************************************************/
/*									*/
/*  Set a page's protection code					*/
/*									*/
/*    Input:								*/
/*									*/
/*	npages   = number of pages to set the protection of		*/
/*	svpage   = starting virtual page number				*/
/*	pageprot = new protection code					*/
/*	initsec  = NULL : this is not the initial mapping of the section
/*	           else : this is the initial mapping of the section	*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_section_setpageprot = OZ_SUCCESS : successful		*/
/*	                                   else : error status		*/
/*	*pageprot_r = old protection codes (an array of npages elements)
/*									*/
/************************************************************************/

uLong oz_knl_section_setpageprot (OZ_Mempage npages, OZ_Mempage svpage, OZ_Hw_pageprot pageprot, OZ_Section *initsec, OZ_Hw_pageprot *pageprot_r)

{
  OZ_Hw_pageprot curprot, reqprot;
  OZ_Mempage phypage;
  OZ_Process *process;
  OZ_Section_pagestate pagestate;
  uLong pt, sts;
  void *ptevaddr;

  OZ_KNL_CHKOBJTYPE (initsec, OZ_OBJTYPE_SECTION);

  process = getprocess (svpage);							/* get process that owns the tables */
start:
  pt = oz_knl_process_lockpt (process);							/* lock the process' pagetables */
  while (npages > 0) {									/* repeat while there are pages to do */

    /* If this is the initial mapping of a private section, and the pagetable page that maps the section */
    /* is a demand-zero page, don't bother faulting the pagetable page in just to write the reqprot.     */
    /* oz_hw_phys_initpage will fill the pagetable page with default reqprot when the page comes in.     */

    if ((initsec != NULL) && (initsec -> privproc != NULL)) {
      ptevaddr = oz_hw_pte_readany (svpage, NULL, NULL, NULL, &reqprot);
      if ((ptevaddr != NULL) && itsadzvaddr (ptevaddr)) goto nextpage;
    }

    /* Otherwise, read the current pte contents, faulting in the pagetable page if necessary */

    ptevaddr = oz_hw_pte_readany (svpage, &pagestate, &phypage, &curprot, &reqprot);	/* read the current pte */
    if (ptevaddr != NULL) {
      oz_knl_process_unlkpt (process, pt);						/* it's faulted out, get it back */
      sts = oz_knl_section_faultpage (OZ_PROCMODE_KNL, OZ_HW_VADDRTOVPAGE (ptevaddr), 1);
      if (sts == OZ_SUCCESS) goto start;						/* repeat now that it is back */
      return (sts);									/* can't get it back, return error sts */
    }

    /* If this is initial mapping, there should be a lot of things like we expect */

    if (initsec != NULL) {
      if (pagestate != OZ_SECTION_PAGESTATE_PAGEDOUT) oz_crash ("oz_knl_section_setpageprot: vpage %x initial pagestate %d (%p)", svpage, pagestate, oz_hw_getrtnadr (0));
      if (phypage   != 0)                             oz_crash ("oz_knl_section_setpageprot: vpage %x initial phypage %u",   svpage, phypage);
      if (curprot   != OZ_HW_PAGEPROT_NA)             oz_crash ("oz_knl_section_setpageprot: vpage %x initial curprot %d",   svpage, curprot);
      if (reqprot   != OZ_HW_PAGEPROT_NA)             oz_crash ("oz_knl_section_setpageprot: vpage %x initial reqprot %d",   svpage, reqprot);
    }

    /* Maybe change current protection as well as requested protection */

    switch (pagestate) {

      /* The page is not accessible right now, so don't bother changing the current protection (it should be NA) */

      case OZ_SECTION_PAGESTATE_PAGEDOUT:
      case OZ_SECTION_PAGESTATE_READINPROG:
      case OZ_SECTION_PAGESTATE_WRITEFAILED:
      case OZ_SECTION_PAGESTATE_READFAILED: {
        if (curprot != OZ_HW_PAGEPROT_NA) oz_crash ("oz_knl_section_setpageprot: pagestate %d, curprot %d", pagestate, curprot);
        break;
      }

      /* The page should be read-only or not-accessible while being written to disk */

      case OZ_SECTION_PAGESTATE_WRITEINPROG: {
        if (curprot == OZ_HW_PAGEPROT_NA) break;
        if (curprot != ppro[curprot]) oz_crash ("oz_knl_section_setpageprot: pagestate %d, curprot %d", pagestate, curprot);

        /* Fall through to _VALID_R processing */
      }

      /* The page is currently mapped readonly, so set the current protection = readonly (requested   */
      /* protection).  If the new protection is a readwrite code, then the current protection will    */
      /* get changed when they actually try to write to the page (and then we know it is dirty, etc). */

      case OZ_SECTION_PAGESTATE_VALID_D:
      case OZ_SECTION_PAGESTATE_VALID_R: {
        curprot = ppro[pageprot];
        break;
      }

      /* The page is currently mapped readwrite, so set the current protection = requested protection */
      /* Leave pagestate _VALID_W even if new code is readonly, because the page is still dirty       */

      case OZ_SECTION_PAGESTATE_VALID_W: {
        curprot = pageprot;
        break;
      }

      default: oz_crash ("oz_knl_section_setpageprot: bad pagestate %d", pagestate);
    }

    /* Write new contents back to pte */

    oz_hw_pte_writeall (svpage, pagestate, phypage, curprot, pageprot);			/* ok, write new protection code */

    /* On to next page */

    if (pageprot_r != NULL) *(pageprot_r ++) = reqprot;					/* maybe return the old prot code */

nextpage:
    -- npages;										/* one less page to process */
    svpage ++;										/* start with the next page */
  }
  oz_knl_process_unlkpt (process, pt);							/* done, unlock pagetables */
  return (OZ_SUCCESS);									/* success */
}

/************************************************************************/
/*									*/
/*  Fault a page of the section into memory				*/
/*									*/
/*  This routine is typically called by the hardware pagefault 		*/
/*  handling routine							*/
/*									*/
/*    Input:								*/
/*									*/
/*	procmode = processor mode of attempted access			*/
/*	           (OZ_PROCMODE_KNL or _USR)				*/
/*	vpage    = virtual page of attempted access			*/
/*	writing  = 0 : read access required				*/
/*	           1 : write access required				*/
/*									*/
/*	smp lock = none							*/
/*	ipl = none or softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_section_faultpage = OZ_SUCCESS : successful completion	*/
/*	                                        retry the access	*/
/*	                            OZ_ACCVIO : caller isn't supposed 	*/
/*	                                        to be able to access 	*/
/*	                                        the location		*/
/*	                                 else : i/o error		*/
/*									*/
/*    Note:								*/
/*									*/
/*	A return of OZ_SUCCESS does not necessarily mean the page is 	*/
/*	now actually in memory.  You must retry.  The only way to 	*/
/*	guarantee access is do something like:				*/
/*									*/
/*		get the pt lock						*/
/*		while (page is faulted out) {				*/
/*			release the pt lock				*/
/*			call oz_knl_section_faultpage			*/
/*			if error status, quit				*/
/*			get the pt lock					*/
/*		}							*/
/*		perform desired access					*/
/*		release the pt lock					*/
/*									*/
/*	A writable page will have its hardware access mode set to read-	*/
/*	only at first.  Then, when an attempt is made to write it, it 	*/
/*	will cause a pagefault.  This routine will then set the page to */
/*	read/write mode.  This will indicate, when it is time to page 	*/
/*	the page out, that the page needs to be written back to the 	*/
/*	section file or page file.					*/
/*									*/
/************************************************************************/

uLong oz_knl_section_faultpage (OZ_Procmode procmode, OZ_Mempage vpage, int writing)

{
  OZ_Hw_pageprot curprot, defprot, reqprot;
  OZ_Mempage newpage, phypage, secpage;
  OZ_Process *process;
  OZ_Procmode ownmode;
  OZ_Section *section;
  OZ_Section_pagestate pagestate;
  Gblpage *gblpage;
  uLong gp, mapsecflags, pt, sts;
  void *ptevaddr;

  oz_knl_thread_incpfs (NULL, 1);

  /* If page is in system address range, use system process to check it */

restart:
  process = getprocess (vpage);

  /* Lock process' page tables so they can't change on us (like by another  */
  /* thread in the same process, possibly running right now on another cpu) */

  pt = oz_knl_process_lockpt (process);

  /* Get section and page offset in section from the virtual page of the process */
  /* If there is no section mapped at that vpage, barf them out.                 */

  if (oz_knl_process_getsecfromvpage (process, vpage, &section, &secpage, &defprot, &ownmode, &mapsecflags) == 0) {
    oz_knl_printk ("oz_knl_section_faultpage*: no section at vpage %X from %p\n", vpage, oz_hw_getrtnadr (0));
//    oz_knl_process_dump_secmaps (process);
    goto rtnaccvio;
  }

  /* Read the faulting pte.  If it is not readable, fault in the pte first.  */
  /* Note that pagetable pages are always read/write, so readable also means */
  /* writable (which we depend on later when we write the updated pte).      */

  ptevaddr = oz_hw_pte_readany (vpage, &pagestate, &phypage, &curprot, &reqprot);	// read pagetable entry
  if (ptevaddr != NULL) {								// see if it's in memory
    oz_knl_process_unlkpt (process, pt);						// if not, unlock pagetables
    procmode = OZ_PROCMODE_KNL;								// set up a fault of pagetable page
    vpage    = OZ_HW_VADDRTOVPAGE (ptevaddr);
    writing  = 1;									// we intend to write to the pagetable page
    goto restart;									// ... then fault in pagetable page
  }

  /* If there is no OZ_HW_PAGEPROT_KR defined, then we have to force writing mode if the page is set to OZ_HW_PAGEPROT_KW */
  /* Otherwise, the page would never be enabled by the _VALID_R processing (ppro[OZ_HW_PAGEPROT_KW] is _NA)               */

#ifndef OZ_HW_PAGEPROT_KR
  if (reqprot == OZ_HW_PAGEPROT_KW) writing = 1;
#endif

  /* See if they are supposed to be able to access the page.  If not, barf them out. */

  if (!protable[reqprot][procmode][writing]) {
    oz_knl_printk ("oz_knl_section_faultpage*: no access allowed to vpage %X (%p)\n", vpage, OZ_HW_VPAGETOVADDR (vpage));
    oz_knl_printk ("oz_knl_section_faultpage*: - reqprot %u, procmode %u, writing %u\n", reqprot, procmode, writing);
    oz_knl_printk ("oz_knl_section_faultpage*: - rtnadr %p\n", oz_hw_getrtnadr (0));
    oz_hw_pte_print (OZ_HW_VPAGETOVADDR (vpage));
//    oz_hwaxp_dumppagetables ();
//    oz_knl_process_dump_secmaps (process);
    goto rtnaccvio;
  }

  /* Maybe it faulted in by some other thread since the fault happened */

  if (curprot == reqprot) goto rtnsuc;

  /* If it is a global section, maybe the page is valid in the global array */

  if (section -> sectype & OZ_SECTION_TYPE_GLOBAL) goto faultglobal;

  /********************************************************************************************/
  /* Faulting in a process-private page.  The only state information available for these      */
  /* pages is what was read from the pte.  The only synchronizing is provided by the pt lock. */
  /********************************************************************************************/

  /* ... so we have to be in the correct process so we read the correct pagestate information */

  if (section -> privproc != process) oz_crash ("oz_knl_section_faultpage: private section %p owned by %p faulted by non-owning process %p", section, section -> privproc, process);

  /* Check out page state */

  switch (pagestate) {

    /* It is to be read in from section file or page file */

    case OZ_SECTION_PAGESTATE_PAGEDOUT: {
      sts = readprivatepage (section, secpage, process, pt, vpage, reqprot, phypage, writing, procmode);	/* read page and write new pte */
      goto rtnsts;												/* return the read status to caller */
    }

    /* See if pagiing is in progress on another thread - only the thread */
    /* that puts a page into _READINPROG or _WRITEINPROG can take it out */

    case OZ_SECTION_PAGESTATE_WRITEINPROG:
    case OZ_SECTION_PAGESTATE_READINPROG: {
      oz_knl_event_set (section -> pfevent, 0);		/* clear event flag before releasing smp lock in case readprivatepage routine is just about to finish up */
      oz_knl_process_unlkpt (process, pt);		/* release smp lock */
      oz_knl_event_waitone (section -> pfevent);	/* wait for event, hopefully that page read completed */
      return (OZ_SUCCESS);				/* go do it all again assuming the thread even bothers to pagefault again */
    }							/* for all we know, the page isn't even mapped anymore */

    /* If page read or write had previously failed, return the error status */

    case OZ_SECTION_PAGESTATE_WRITEFAILED:
    case OZ_SECTION_PAGESTATE_READFAILED: {
      sts = phypage;
      goto rtnsts;
    }

    /* This state is entered when a page that was in _VALID_W state exists and a fork happens to it.  The    */
    /* page is placed in _VALID_D state and set to read-only protection.  Then, when a write comes along, it */
    /* causes a fault and the page is copied before the write is allowed to proceed.  If this is not a write */
    /* attempt, the page is treated as if it were in _VALID_R state, ie, it is set to read-only access.      */

    case OZ_SECTION_PAGESTATE_VALID_R:
    case OZ_SECTION_PAGESTATE_VALID_D:
    case OZ_SECTION_PAGESTATE_VALID_W: {

      /* If not a write attempt, or page is supposed to be read-only, just read enable it - */
      /* Then, if anyone subsequently tries to write it, we will get another pagefault, at  */
      /* which time we will write-enable the page (assuming it is ok to write-enable it),   */
      /* and thus we will know that the caller is modifying the page, so when it comes time */
      /* to page it out, we know that a write will be required to write it to disk.         */

      if (!writing) {

        /* Only write the pte on the current cpu.  If this process is active on other cpu's, they will */
        /* get the same type of fault, and they will end up right here to get the new copy of the pte. */
        /* Slim chance of that happening, so take the easy way out for now.                            */

        oz_hw_pte_writecur (vpage, pagestate, phypage, ppro[reqprot], reqprot);
        if (!OZ_HW_READABLE (sizeof (uLong), OZ_HW_VPAGETOVADDR (vpage), procmode)) {
          oz_crash ("oz_knl_section_faultpage: vpage %x, phypage %x, curprot %u, reqprot %u not readable", vpage, phypage, ppro[reqprot], reqprot);
        }
        sts = *(uLong *)(OZ_HW_VPAGETOVADDR (vpage));			/* - test that it is now readable, crash if not */
      }

      /* Caller wants to write page.  We need to copy page if writes don't go to section file and: */
      /*  1) it is a 'cache' page, so our writes don't go back to disk                             */
      /*  2) it is a 'section' page (ie, demand-zero or read from file)                            */
      /*     and ptrefcount > 1 (ie, there is more than one pagetable pointing to it)              */
      /*     so our writes don't go to the common page                                             */

      else if (!(section -> sectype & OZ_SECTION_TYPE_SECFIL) 
            && (phypage < oz_s_phymem_totalpages) 
            && ((oz_s_phymem_pages[phypage].state == OZ_PHYMEM_PAGESTATE_ALLOCACHE) 
            || ((oz_s_phymem_pages[phypage].state == OZ_PHYMEM_PAGESTATE_ALLOCSECT)
             && (oz_s_phymem_pages[phypage].u.s.ptrefcount > 1)))) {
        sts = alloc_phypage (section, &newpage, vpage);							/* allocate a new memory page */
        if (sts != OZ_SUCCESS) goto rtnsts;
        if (curprot == OZ_HW_PAGEPROT_NA) {								/* see if we can read old page by its virt address */
          oz_hw_phys_movephys (1 << OZ_HW_L2PAGESIZE, &phypage, 0, &newpage, 0);			/* copy old page to new page */
													/* - can't use old page's virt mapping because it */
													/*   is set no-access, like when copying a page */
													/*   that's supposed to be KW but the hardware */
													/*   doesn't have a KR so the best it can do is NA */
        } else {
          oz_hw_phys_movefromvirt (1 << OZ_HW_L2PAGESIZE, OZ_HW_VPAGETOVADDR (vpage), &newpage, 0);	/* hw allows access, copy the old to the new */
        }
        oz_hw_pte_writecur (vpage, OZ_SECTION_PAGESTATE_VALID_W, newpage, reqprot, reqprot); 		/* map new page with write access */
        if (oz_s_phymem_pages[phypage].state == OZ_PHYMEM_PAGESTATE_ALLOCACHE) {
          (*(section -> knlpfrel)) (section -> file, phypage);						/* now there is one less reference to the cache page */
        }
        else if (oz_s_phymem_pages[phypage].state == OZ_PHYMEM_PAGESTATE_ALLOCSECT) {
          if (oz_hw_atomic_inc_long (&(oz_s_phymem_pages[phypage].u.s.ptrefcount), -1) == 0) {		/* now there is one less reference to the old page */
            if (oz_s_phymem_pages[phypage].u.s.pfpage != 0) {						/* well, we didn't need to copy it after all, */
              free_pfpage (section, oz_s_phymem_pages[phypage].u.s.pfpage);				/* ... dump the old pagefile page */
              oz_s_phymem_pages[phypage].u.s.pfpage = 0;
            }
            free_phypage (section, phypage);								/* free off the old page */
          }
        }
      }

      /* We are the only user or it is to be written back to section file, so just write-enable the page */

      else {
//      oz_knl_printk ("oz_knl_section_faultpage*: write enable, sectype %X, vpage %X, ppage %X, state %d, ptrefcount %d\n", 
//		section -> sectype, vpage, phypage, oz_s_phymem_pages[phypage].state, 
//		oz_s_phymem_pages[phypage].state, oz_s_phymem_pages[phypage].u.s.ptrefcount);
        oz_hw_pte_writecur (vpage, OZ_SECTION_PAGESTATE_VALID_W, phypage, reqprot, reqprot);
      }

      goto rtnsuc;
    }

    /* Unknown state */

    default: oz_crash ("oz_knl_section_faultpage: invalid private section page state %d", pagestate);
  }

  /********************************************************************************************************/
  /* Process fault on a global section.  The gblpages array contains the 'true' state information for the */
  /* pages.  So our job is to update the hardware pte from the gblpages array after faulting in the page. */
  /********************************************************************************************************/

  /* There are two pagestates to consider here, that in the pte and that in the gblpage array entry.                                                */
  /* The pte pagestate can only be PAGEDOUT, VALID_R, VALID_W, while the gblpage state can be anything but VALID_D.                                 */
  /* When pte pagestate is PAGEDOUT, it means get the state from gblpage entry                                                                      */
  /* When pte pagestate is VALID_R, it means this process has read access to the page already, so the global page must be either VALID_R or VALID_W */
  /* When pte pagestate is VALID_W, it means this process has write access to the page and has written to it, so the global page must be VALID_W    */

faultglobal:
  oz_crash ("oz_knl_section_faultpage*: we haven't tested global sections yet");
  gp = oz_hw_smplock_wait (&(section -> smplock_gp));	/* keep section's gblpages array stable */
  gblpage = section -> gblpages + secpage;		/* get the page's state */

  switch (gblpage -> pagestate) {

    /* Page is stuck out on disk somewhere (either in the original section file or in page file) */

    case OZ_SECTION_PAGESTATE_PAGEDOUT: {
      if (pagestate != OZ_SECTION_PAGESTATE_PAGEDOUT) oz_crash ("oz_knl_section_faultpage: gbl pagestate %d, pt pagestate %d", gblpage -> pagestate, pagestate);
      sts = readglobalpage (section, gp, gblpage, secpage, process, pt, gblpage -> phypage);	/* try to read it in */
      oz_hw_smplock_clr (&(section -> smplock_gp), gp);						/* release smp locks */
      goto rtnsts;										/* if read failed, tell caller */
												/* if success, let caller re-fault */
												/* ... in case things are different now */
    }

    /* See if pagiing is in progress on another thread - only the thread */
    /* that puts a page into _READINPROG or _WRITEINPROG can take it out */

    case OZ_SECTION_PAGESTATE_READINPROG:
    case OZ_SECTION_PAGESTATE_WRITEINPROG: {
      if (pagestate != OZ_SECTION_PAGESTATE_PAGEDOUT) oz_crash ("oz_knl_section_faultpage: gbl pagestate %d, pt pagestate %d", gblpage -> pagestate, pagestate);
      oz_knl_event_set (section -> pfevent, 0);		/* clear event flag in case we have to wait */
      oz_hw_smplock_clr (&(section -> smplock_gp), gp);	/* release section smp lock */
      oz_knl_process_unlkpt (process, pt);		/* release pagetable smp lock */
      oz_knl_event_waitone (section -> pfevent);	/* wait for event, hopefully that page read completed */
      return (OZ_SUCCESS);				/* go do it all again assuming the thread even bothers to pagefault again */
    }

    /* If a previous fault failed, return the error status */

    case OZ_SECTION_PAGESTATE_READFAILED:
    case OZ_SECTION_PAGESTATE_WRITEFAILED: {
      if (pagestate != OZ_SECTION_PAGESTATE_PAGEDOUT) oz_crash ("oz_knl_section_faultpage: gbl pagestate %d, pt pagestate %d", gblpage -> pagestate, pagestate);
      sts = gblpage -> phypage;
      oz_hw_smplock_clr (&(section -> smplock_gp), gp);
      goto rtnsts;
    }

    /* Page is loaded in memory but is clean.  If the fault happened because of a read attempt, just set   */
    /* our pte with the phypage.  Use read-only protection though for the current protection.  Now if      */
    /* reqprot is a read/write code, it will fault again with a write attempt when they try to write the   */
    /* page.  If the fault happened because of a write attempt, convert the page to read/write protection. */

    case OZ_SECTION_PAGESTATE_VALID_R:
    case OZ_SECTION_PAGESTATE_VALID_D:
    case OZ_SECTION_PAGESTATE_VALID_W: {
      if ((pagestate != OZ_SECTION_PAGESTATE_PAGEDOUT) 
       && (pagestate != OZ_SECTION_PAGESTATE_VALID_R) 
       && (pagestate != OZ_SECTION_PAGESTATE_VALID_D) 
       && (pagestate != OZ_SECTION_PAGESTATE_VALID_W)) {
        oz_crash ("oz_knl_section_faultpage: global VALID_R/D/W page pt pagestate %d", pagestate);
      }

      /* If not a write attempt, or page is supposed to be read-only, just read enable it - */
      /* Then, if anyone subsequently tries to write it, we will get another pagefault, at  */
      /* which time we will write-enable the page (assuming it is ok to write-enable it),   */
      /* and thus we will know that the caller is modifying the page, so when it comes time */
      /* to page it out, we know that a write will be required to write it to disk.         */

      if (!writing) {

        /* Only write the pte on the current cpu.  If this process is active on other cpu's, they will */
        /* get the same type of fault, and they will end up right here to get the new copy of the pte. */
        /* Slim chance of that happening, so take the easy way out for now.                            */

        oz_hw_pte_writecur (vpage, gblpage -> pagestate, gblpage -> phypage, ppro[reqprot], reqprot);
      }

      /* If they are trying to write to a page that does not have SECFIL (ie, write back to section file),   */
      /* make a copy if:                                                                                     */
      /*  1) it is a disk cache page                                                                         */
      /*  2) it is a section page and there is more than one pte pointing to it and we aren't sharing writes */

      else if (!(section -> sectype & OZ_SECTION_TYPE_SECFIL) 
            && (phypage < oz_s_phymem_totalpages) 
            && ((oz_s_phymem_pages[phypage].state == OZ_PHYMEM_PAGESTATE_ALLOCACHE) 
            || ((oz_s_phymem_pages[phypage].state == OZ_PHYMEM_PAGESTATE_ALLOCSECT)
             && (oz_s_phymem_pages[phypage].u.s.ptrefcount > 1) 
             && !(section -> sectype & OZ_SECTION_TYPE_SHRWRT)))) {
        sts = alloc_phypage (section, &newpage, vpage);							/* allocate a new memory page */
        if (sts != OZ_SUCCESS) {
          oz_hw_smplock_clr (&(section -> smplock_gp), gp);
          goto rtnsts;
        }
        oz_hw_phys_movefromvirt (1 << OZ_HW_L2PAGESIZE, OZ_HW_VPAGETOVADDR (vpage), &newpage, 0);	/* copy the old to the new */
        oz_hw_pte_writecur (vpage, OZ_SECTION_PAGESTATE_VALID_W, newpage, reqprot, reqprot); 		/* map it with write access */
        if (oz_s_phymem_pages[phypage].state == OZ_PHYMEM_PAGESTATE_ALLOCACHE) {
          (*(section -> knlpfrel)) (section -> file, phypage);						/* now there is one less reference to the cache page */
        }
        else if (oz_s_phymem_pages[phypage].state == OZ_PHYMEM_PAGESTATE_ALLOCSECT) {
          if (oz_hw_atomic_inc_long (&(oz_s_phymem_pages[phypage].u.s.ptrefcount), -1) == 0) {		/* now there is one less reference to the old page */
            if (oz_s_phymem_pages[phypage].u.s.pfpage != 0) {						/* well, we didn't need to copy it after all, */
              free_pfpage (section, oz_s_phymem_pages[phypage].u.s.pfpage);				/* ... dump the old pagefile page */
              oz_s_phymem_pages[phypage].u.s.pfpage = 0;
            }
            free_phypage (section, phypage);								/* free off the old page */
          }
        }
      }

      /* Either the page is to be shared amongst all users or we are the only user, so just write-enable the page */

      else {
        gblpage -> pagestate = OZ_SECTION_PAGESTATE_VALID_W;						/* the global page is about to be modified */
        oz_hw_pte_writecur (vpage, OZ_SECTION_PAGESTATE_VALID_W, phypage, reqprot, reqprot);		/* allow the global page to be modified */
      }

      oz_hw_smplock_clr (&(section -> smplock_gp), gp);
      goto rtnsuc;
    }

    default: oz_crash ("oz_knl_section_faultpage: bad global page state %d", gblpage -> pagestate);
  }

  /***********/
  /* Returns */
  /***********/

rtnaccvio:
  sts = OZ_ACCVIO;
  goto rtnsts;

rtnsuc:
  sts = OZ_SUCCESS;

rtnsts:
  oz_knl_process_unlkpt (process, pt);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Read in a private page from section or page file			*/
/*									*/
/*    Input:								*/
/*									*/
/*	section = section the page is a member of			*/
/*	pt = smplevel of fault, either NULL or SOFTINT			*/
/*	vpage = virtual page that caused the fault			*/
/*	reqprot = requested page protection				*/
/*	pfpage = 0 : read from section file				*/
/*	      else : read from page file				*/
/*	smplevel = pt							*/
/*									*/
/*    Output:								*/
/*									*/
/*	readprivatepage = OZ_SUCCESS : successfully read, retry the fault
/*	                        else : read error, abort fault		*/
/*	smplevel = pt (but it was released and re-locked)		*/
/*									*/
/************************************************************************/

static uLong readprivatepage (OZ_Section *section, OZ_Mempage secpage, OZ_Process *process, uLong pt, 
                              OZ_Mempage vpage, OZ_Hw_pageprot reqprot, OZ_Mempage pfpage, int writing, OZ_Procmode procmode)

{
  OZ_Hw_pageprot validprot;
  OZ_IO_fs_pageread fs_pageread;
  OZ_Iochan *readfile;
  OZ_Section_pagestate validstate;
  OZ_Mempage phypage;
  uLong sts;
  volatile uLong status;

  /* Get what the valid state and protection codes will be */

  validstate = OZ_SECTION_PAGESTATE_VALID_R;
  validprot  = ppro[reqprot];
  if (writing || (section -> sectype & OZ_SECTION_TYPE_PAGTBL)) {
    validstate = OZ_SECTION_PAGESTATE_VALID_W;
    validprot  = reqprot;
  }

  /* See if fs supports direct cache access.  If so, just grab the cache page directly. */
  /* But don't if we're writing and writes don't go back to section file.               */

  if ((!writing || (section -> sectype & OZ_SECTION_TYPE_SECFIL)) 
   && (pfpage == 0) && !(section -> sectype & OZ_SECTION_TYPE_ZEROES) && (section -> knlpfmap != NULL)) {

    /* Mark the page 'read-in-progress' and unlock.  If someone else wants this page, they will see this state and wait. */

    oz_hw_pte_writecur (vpage, OZ_SECTION_PAGESTATE_READINPROG, phypage, OZ_HW_PAGEPROT_NA, reqprot);
    oz_knl_process_unlkpt (process, OZ_SMPLOCK_SOFTINT);

    /* Get the page from the cache and lock it in memory until we call knlpfrel            */
    /* If it is ramdisk at fixed memory location, phypage may be >= oz_s_phymem_totalpages */

    sts = (*(section -> knlpfmap)) (section -> file, 
                                    (secpage << (OZ_HW_L2PAGESIZE - section -> l2blocksize)) + section -> vbnstart, 
                                    &phypage);
    if (sts != OZ_SUCCESS) phypage = OZ_PHYPAGE_NULL;
  }

  /* Otherwise, allocate a page and read page in */

  else {

    /* Get a free page */

    sts = alloc_phypage (section, &phypage, vpage);
    if (sts != OZ_SUCCESS) return (sts);

    /* If it is a demand-zero section and the page is to be read from the section, just return the page after zero filling it */

    if ((pfpage == 0) && (section -> sectype & OZ_SECTION_TYPE_ZEROES)) {
      oz_hw_phys_initpage (phypage, (section -> sectype & OZ_SECTION_TYPE_PAGTBL) ? vpage : 0);	/* zero fill page */
      oz_hw_pte_writecur (vpage, validstate, phypage, validprot, reqprot);			/* it is now valid */
      return (OZ_SUCCESS);									/* return with success status */
    }

    /* Mark page state as 'read in progress' - this will prevent other threads from starting to  */
    /* read it and will keep it mapped at this address during the wait.  If others try to access */
    /* the page, they will fault too, but then they will wait for us to set the event flag.      */

    oz_hw_pte_writecur (vpage, OZ_SECTION_PAGESTATE_READINPROG, phypage, OZ_HW_PAGEPROT_NA, reqprot);

    /* We need to release the lock because I/O system must not be called with any smp locks.                            */
    /* Keep software ints inhibited so this thread can't be aborted (after all, we did say we are reading the page in). */

    oz_knl_process_unlkpt (process, OZ_SMPLOCK_SOFTINT);

    /* Now we just read the page in from disk */

    memset (&fs_pageread, 0, sizeof fs_pageread);
    if (pfpage == 0) {
      readfile = section -> file;				/* pfpage zero, read from section file */
      fs_pageread.startblock = (secpage << (OZ_HW_L2PAGESIZE - section -> l2blocksize)) + section -> vbnstart;
    } else {
      readfile = section -> pagefile;				/* pfpage non-zero, read from page file */
      fs_pageread.startblock = ((pfpage - 1) << (OZ_HW_L2PAGESIZE - section -> pagefile_l2blocksize)) + 1;
    }
    fs_pageread.pagecount = 1;					/* - just one physical page number present */
    fs_pageread.pagearray = &phypage;				/* - point to physical page number (array of one element) */
    status = OZ_PENDING;
    sts = oz_knl_iostart3 (1, NULL, readfile, OZ_PROCMODE_KNL, NULL, NULL, &status, section -> pfevent, 
                           NULL, NULL, OZ_IO_FS_PAGEREAD, sizeof fs_pageread, &fs_pageread);
    if (sts == OZ_STARTED) {
      while ((sts = status) == OZ_PENDING) {
        oz_knl_event_waitone (section -> pfevent);
        oz_knl_event_set (section -> pfevent, 0);
      }
    }
  }

  /* Get smp lock back so we can write the new page's state */

  oz_knl_process_lockpt (process);

  /* Update page's state */

  if (sts == OZ_SUCCESS) {
    oz_hw_pte_writecur (vpage, validstate, phypage, validprot, reqprot);
    if (writing) {
      if (!OZ_HW_WRITABLE (sizeof (uLong), OZ_HW_VPAGETOVADDR (vpage), procmode)) {
        oz_crash ("oz_knl_section readprivatepage: vpage %x, phypage %x, curprot %u, reqprot %u not writeable", vpage, phypage, validprot, reqprot);
      }
      status = oz_hw_atomic_or_long (OZ_HW_VPAGETOVADDR (vpage), 0);	/* ?? this should be harmless under all circumstances ?? */
    } else {
      if (!OZ_HW_READABLE (sizeof (uLong), OZ_HW_VPAGETOVADDR (vpage), procmode)) {
        oz_crash ("oz_knl_section readprivatepage: vpage %x, phypage %x, curprot %u, reqprot %u not readable", vpage, phypage, validprot, reqprot);
      }
      status = *(uLong *)(OZ_HW_VPAGETOVADDR (vpage));			/* ?? this should be harmless under all circumstances ?? */
    }
  } else {
    oz_knl_printk ("oz_knl_section readprivatepage: section/page file read error status %u, phypage %u\n", sts, phypage);
    oz_hw_pte_writecur (vpage, OZ_SECTION_PAGESTATE_READFAILED, sts, OZ_HW_PAGEPROT_NA, reqprot); /* failed, save failure status */
    if (phypage != OZ_PHYPAGE_NULL) {
      oz_s_phymem_pages[phypage].u.s.ptrefcount = 0;
      free_phypage (section, phypage);					/* free the page */
    }
  }

  /* Set faulting event flag (in case someone else was waiting for us to finish reading the page in) */

  oz_knl_event_set (section -> pfevent, 1);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Read in a global page from section or page file			*/
/*									*/
/*    Input:								*/
/*									*/
/*	section = section the page is a member of			*/
/*	gp = level to return to when releasing gp lock			*/
/*	gblpage = contains state info					*/
/*	pt = smplevel of fault, either NULL or SOFTINT			*/
/*	pfpage = 0 : read from section file				*/
/*	      else : read from page file				*/
/*	smplevel = gp and pt						*/
/*									*/
/*    Output:								*/
/*									*/
/*	readglobalpage = OZ_SUCCESS : successfully read, retry the fault
/*	                       else : read error, abort fault		*/
/*	smplevel = gp and pt (but they were released and re-locked)	*/
/*									*/
/************************************************************************/

static uLong readglobalpage (OZ_Section *section, uLong gp, Gblpage *gblpage, OZ_Mempage secpage, OZ_Process *process, uLong pt, OZ_Mempage pfpage)

{
  OZ_IO_fs_pageread fs_pageread;
  OZ_Iochan *readfile;
  OZ_Mempage phypage;
  uLong sts;
  volatile uLong status;

  if (section -> sectype & OZ_SECTION_TYPE_PAGTBL) oz_crash ("oz_knl_section readglobalpage: can't have a global pagetable");

  /* See if fs supports direct cache access.  If so, just grab the cache page directly.    */
  /* Do not allow this for SHRWRT but not SECFIL, as we can't later shift from using the   */
  /* cache page with multiple readers to a section page with multiple readers and writers. */

  if ((pfpage == 0) && !(section -> sectype & OZ_SECTION_TYPE_ZEROES) && (section -> knlpfmap != NULL) 
   && ((section -> sectype & (OZ_SECTION_TYPE_SHRWRT | OZ_SECTION_TYPE_SECFIL)) != OZ_SECTION_TYPE_SHRWRT)) {

    /* Mark the page 'read-in-progress' and unlock.  If someone else wants this page, they will see this state and wait. */

    gblpage -> pagestate = OZ_SECTION_PAGESTATE_READINPROG;
    gblpage -> phypage   = phypage;

    oz_hw_smplock_clr (&(section -> smplock_gp), gp);
    oz_knl_process_unlkpt (process, OZ_SMPLOCK_SOFTINT);

    /* Get the page from the cache and lock it in memory until we call knlpfrel            */
    /* If it is ramdisk at fixed memory location, phypage may be >= oz_s_phymem_totalpages */

    sts = (*(section -> knlpfmap)) (section -> file, 
                                    (secpage << (OZ_HW_L2PAGESIZE - section -> l2blocksize)) + section -> vbnstart, 
                                    &phypage);

    if (sts != OZ_SUCCESS) phypage = OZ_PHYPAGE_NULL;
  }

  /* Otherwise, allocate a page and read page in */

  else {

    /* Get a free page */

    sts = alloc_phypage (section, &phypage, secpage);
    if (sts != OZ_SUCCESS) return (sts);

    /* If it is a demand-zero section and the page is to be read from the section, just return the page after zero filling it */

    if ((pfpage == 0) && (section -> sectype & OZ_SECTION_TYPE_ZEROES)) {
      oz_hw_phys_initpage (phypage, 0);				/* zero fill the page */
      gblpage -> pagestate = OZ_SECTION_PAGESTATE_VALID_R;	/* it is now valid */
      gblpage -> phypage   = phypage;
      return (OZ_SUCCESS);					/* return with success status */
    }

    /* Mark page state as 'read in progress' - this will prevent other threads from starting to  */
    /* read it and will keep it mapped at this address during the wait.  If others try to access */
    /* the page, they will fault too, but then they will wait for us to set the event flag.      */

    gblpage -> pagestate = OZ_SECTION_PAGESTATE_READINPROG;
    gblpage -> phypage   = phypage;

    /* We need to release locks because I/O system must not be called with any smp locks.                               */
    /* Keep software ints inhibited so this thread can't be aborted (after all, we did say we are reading the page in). */

    oz_hw_smplock_clr (&(section -> smplock_gp), gp);
    oz_knl_process_unlkpt (process, OZ_SMPLOCK_SOFTINT);

    /* Now we just read the page in from disk */

    memset (&fs_pageread, 0, sizeof fs_pageread);
    if (pfpage == 0) {
      readfile = section -> file;				/* pfpage zero, read from section file */
      fs_pageread.startblock = (secpage << (OZ_HW_L2PAGESIZE - section -> l2blocksize)) + section -> vbnstart;
    } else {
      readfile = section -> pagefile;				/* pfpage non-zero, read from page file */
      fs_pageread.startblock = ((pfpage - 1) << (OZ_HW_L2PAGESIZE - section -> pagefile_l2blocksize)) + 1;
    }
    fs_pageread.pagecount = 1;					/* - just one physical page number present */
    fs_pageread.pagearray = &phypage;				/* - point to physical page number (array of one element) */
    status = OZ_PENDING;
    sts = oz_knl_iostart3 (1, NULL, readfile, OZ_PROCMODE_KNL, NULL, NULL, &status, section -> pfevent, 
                           NULL, NULL, OZ_IO_FS_PAGEREAD, sizeof fs_pageread, &fs_pageread);
    if (sts == OZ_STARTED) {
      while ((sts = status) == OZ_PENDING) {
        oz_knl_event_waitone (section -> pfevent);
        oz_knl_event_set (section -> pfevent, 0);
      }
    }
  }

  /* Get smp locks back so we can write the new page's state */

  oz_knl_process_lockpt (process);
  oz_hw_smplock_wait (&(section -> smplock_gp));

  if (sts == OZ_SUCCESS) gblpage -> pagestate = OZ_SECTION_PAGESTATE_VALID_R; /* successful, page is now valid (phypage already has physical page number in it) */
  else {
    oz_knl_printk ("oz_knl_section readglobalpage: section/page file read error status %u, phypage %u\n", sts, phypage);
    gblpage -> pagestate = OZ_SECTION_PAGESTATE_READFAILED;	/* failed, save failure status */
    gblpage -> phypage   = sts;
    if (phypage != OZ_PHYPAGE_NULL) {
      oz_s_phymem_pages[phypage].u.s.ptrefcount = 0;
      free_phypage (section, phypage);				/* free the page */
    }
  }

  /* Set faulting event flag (in case someone else was waiting for us to finish reading the page in) */

  oz_knl_event_set (section -> pfevent, 1);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Unmap a page from the current process				*/
/*									*/
/*    Input:								*/
/*									*/
/*	vpage = virtual page number being unmapped			*/
/*	        - if it needs to be written to page file, don't bother	*/
/*	        - wait if necessary for paging to complete		*/
/*	        - never return a 0					*/
/*	ptsave = smp lock level before pt 				*/
/*	         (either OZ_SMPLOCK_NULL or OZ_SMPLOCK_SOFTINT)		*/
/*									*/
/*	smp lock = pt							*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_section_unmappage = 1 : page is out, pt lock was held throughout
/*	                           0 : page is out, pt lock was released and re-acquired
/*									*/
/*    Note:								*/
/*									*/
/*	The goal of this routine is to set the page to PAGEPROT_NA, 	*/
/*	and phypage=0, ie, what it was before it was ever used.		*/
/*									*/
/*	The use and design of the oz_knl_section_iolock routine 	*/
/*	requires that this routine not unmap any page until all I/O 	*/
/*	started on it has completed.					*/
/*									*/
/************************************************************************/

int oz_knl_section_unmappage (OZ_Mempage vpage, uLong ptsave)

{
  Gblpage *gblpage;
  int keptptlock, rc;
  OZ_Hw_pageprot curprot, mapprot, reqprot;
  OZ_Mempage pagedinvpage, pfpage, phypage, ptevpage, secpage;
  OZ_Process *process;
  OZ_Procmode ownmode;
  OZ_Section *section;
  OZ_Section_pagestate pagestate;
  uLong gp, mapsecflags, sts;
  void *ptevaddr;

  process = getprocess (vpage);

  keptptlock = 1;

  /* Read about it.  If pt page is out, then fault it in and try again.  But don't bother faulting in the   */
  /* pagefile page if it is still 'demand-zero', that means nothing has ever been faulted in on that        */
  /* pagetable page, so we can assume that the target page is faulted out, too.  This optimization saves    */
  /* us from faulting in pagetable pages for areas of memory that were mapped to an section but never used. */

tryagain:
  ptevaddr = oz_hw_pte_readany (vpage, &pagestate, &phypage, &curprot, &reqprot);			/* read the page's state from the pte */
  if (ptevaddr != NULL) {										/* see if page containg pte is faulted out */
    do {
      ptevpage = OZ_HW_VADDRTOVPAGE (ptevaddr);								/* page containg pte is faulted out, get pte's vpage */
      ptevaddr = oz_hw_pte_readany (ptevpage, &pagestate, &phypage, &curprot, &reqprot);		/* read the pte for the pagetable page */
    } while (ptevaddr != NULL);										/* repeat up a level if we can't read that one either */
    if ((pagestate == OZ_SECTION_PAGESTATE_PAGEDOUT) && (phypage == 0)) return (keptptlock);		/* if pagetable page was never used, target wasn't either */
    oz_knl_process_unlkpt (process, ptsave);								/* pagetable page was used, we need to fault it in */
    sts = oz_knl_section_faultpage (OZ_PROCMODE_KNL, ptevpage, 0);					/* fault it in, this may be an upper level page */
    if (sts != OZ_SUCCESS) oz_crash ("oz_knl_section_unmapage: error %u faulting in pagetable entry address %p", sts, ptevaddr);
    ptsave = oz_knl_process_lockpt (process);								/* get lock back */
    keptptlock = 0;
    goto tryagain;											/* start all over with original target page */
  }

  /* If I/O controller or fixed ramdisk page, just mark pte invalid */

  if ((phypage >= oz_s_phymem_totalpages) 
   && ((pagestate == OZ_SECTION_PAGESTATE_VALID_R)
    || (pagestate == OZ_SECTION_PAGESTATE_VALID_D)
    || (pagestate == OZ_SECTION_PAGESTATE_VALID_W))) {
    oz_hw_pte_writeall (vpage, OZ_SECTION_PAGESTATE_PAGEDOUT, 0, OZ_HW_PAGEPROT_NA, OZ_HW_PAGEPROT_NA);
    return (keptptlock);
  }

  /* Find out the associated section and page within the section */

  if (!oz_knl_process_getsecfromvpage (process, vpage, &section, &secpage, &mapprot, &ownmode, &mapsecflags)) return (keptptlock);
  if (section -> sectype & OZ_SECTION_TYPE_GLOBAL) goto unmapglobal;

  /***********************************/
  /* Private section page processing */
  /***********************************/

  /* Process it based on page state found in the pte */

  switch (pagestate) {

    /* It is paged out to disk */

    case OZ_SECTION_PAGESTATE_PAGEDOUT: {

      /* If it is not using a pagefile page, we don't have to do anything.  Pagetable pages    */
      /* are still in demand-zero state and therefore have never had any pages mapped to them. */

      if (phypage != 0) {

        /* If we are unmapping a pagetable page, we have to fault it in first */
        /* to make sure it doesn't contain any pages that need unmapping      */

        if (section -> sectype & OZ_SECTION_TYPE_PAGTBL) {
          oz_knl_process_unlkpt (process, ptsave);
          oz_knl_section_faultpage (OZ_PROCMODE_KNL, vpage, 1);
          oz_knl_process_lockpt (process);
          keptptlock = 0;
          goto tryagain;
        }

        /* Not a pagetable page, release the pagefile page.  Now if there are other forked copies of this page      */
        /* referencing the pagefile page, then it will have a non-zero refcount and will not actually be freed off. */

        oz_hw_pte_writecur (vpage, OZ_SECTION_PAGESTATE_PAGEDOUT, 0, OZ_HW_PAGEPROT_NA, OZ_HW_PAGEPROT_NA); /* update pte with phypage = 0 */
												/* ... indicating no more pagfile page */
        free_pfpage (section, phypage);								/* free off reference to pagefile page */
      }

      /* If the reqprot is not NA, write it */

      else if (reqprot != OZ_HW_PAGEPROT_NA) oz_hw_pte_writecur (vpage, OZ_SECTION_PAGESTATE_PAGEDOUT, 0, OZ_HW_PAGEPROT_NA, OZ_HW_PAGEPROT_NA);

      return (keptptlock);
    }

    /* It is on its way in or out - wait for the other thread to complete processing then try again */

    case OZ_SECTION_PAGESTATE_READINPROG:
    case OZ_SECTION_PAGESTATE_WRITEINPROG: {
      oz_knl_event_set (section -> pfevent, 0);		/* clear event flag before releasing smp lock in case readprivatepage routine is just about to finish up */
      oz_knl_process_unlkpt (process, ptsave);		/* release smp lock */
      oz_knl_event_waitone (section -> pfevent);	/* wait for event, hopefully that page read completed */
      ptsave = oz_knl_process_lockpt (process);		/* lock the pagetables again */
      keptptlock = 0;
      goto tryagain;					/* go process its new state */
    }

    /* These states occupy no memory, so we can just mark them paged out and return */

    case OZ_SECTION_PAGESTATE_READFAILED:		/* failed to read it - this is a dead-end state */
    case OZ_SECTION_PAGESTATE_WRITEFAILED: {		/* failed to write it - this is a dead-end state */
      oz_hw_pte_writecur (vpage, OZ_SECTION_PAGESTATE_PAGEDOUT, 0, OZ_HW_PAGEPROT_NA, OZ_HW_PAGEPROT_NA);
      return (keptptlock);
    }

    /* The page doesn't match what is on disk, so we need to write it out first.  But don't   */
    /* bother writing it to the pagefile as we are going to discard the pagefile page anyway. */

    case OZ_SECTION_PAGESTATE_VALID_R:
    case OZ_SECTION_PAGESTATE_VALID_D:
    case OZ_SECTION_PAGESTATE_VALID_W: {

      /* If it is a page of a pagetable, make sure all the pages it points to are unmapped first */

      if (section -> sectype & OZ_SECTION_TYPE_PAGTBL) {
        if (oz_s_phymem_pages[phypage].u.s.ptrefcount != 1) {
          oz_crash ("oz_knl_section_unmappage: pagetable vpage %X page %X refcount %d", vpage, phypage, oz_s_phymem_pages[phypage].u.s.ptrefcount);
        }
        while ((pagedinvpage = oz_hw_pte_checkptpage (vpage, 1)) != 0) {
          keptptlock &= oz_knl_section_unmappage (pagedinvpage, ptsave);
        }
      }

      /* Wait for all I/O initiated via this pagetable on this page to cease */

      rc = waitioidle (1, vpage, process);
      if (rc < 0) keptptlock = 0;

      /* If it is mapped to a disk cache page, tell cache if it has been modified then release it */

      if (oz_s_phymem_pages[phypage].state == OZ_PHYMEM_PAGESTATE_ALLOCACHE) {
        if (pagestate != OZ_SECTION_PAGESTATE_VALID_R) {
          if (!(section -> sectype & OZ_SECTION_TYPE_SECFIL)) oz_crash ("oz_knl_section_unmappage: wrote to cache page that doesn't get written to disk");
          oz_hw_pte_writeall (vpage, OZ_SECTION_PAGESTATE_WRITEINPROG, phypage, OZ_HW_PAGEPROT_NA, reqprot); /* make page inaccessible so no one can modify it or start any I/O going on it */
													/* - this will cause anyone who wants to access the page to wait for us to finish with it */
          oz_knl_process_unlkpt (process, OZ_SMPLOCK_SOFTINT);						/* release lock but stay at softint so we can't be aborted */
          sts = (*(section -> knlpfupd)) (section -> file, 
                                          (secpage << (OZ_HW_L2PAGESIZE - section -> l2blocksize)) + section -> vbnstart, 
                                          phypage);
          if (sts != OZ_SUCCESS) oz_knl_printk ("oz_knl_section_unmappage: error %u writing page %u back to cache", sts, phypage);
          oz_knl_process_lockpt (process);								/* lock the pagetable */
          oz_knl_event_set (section -> pfevent, 1);							/* in case some other thread is waiting for us to finish writing it */
        }
        oz_hw_pte_writeall (vpage, OZ_SECTION_PAGESTATE_PAGEDOUT, 0, OZ_HW_PAGEPROT_NA, OZ_HW_PAGEPROT_NA); 
        oz_knl_process_unlkpt (process, OZ_SMPLOCK_SOFTINT);						/* release lock but stay at softint so we can't be aborted */
        (*(section -> knlpfrel)) (section -> file, phypage);						/* release cache page */
        oz_knl_process_lockpt (process);								/* lock the pagetable */
        return (0);											/* tell caller we released smp lock */
      }

      /* Section page, there shall soon be one less pte pointing to the page */

      if (oz_hw_atomic_inc_long (&(oz_s_phymem_pages[phypage].u.s.ptrefcount), -1) == 0) {

        /* If it is to be written back to the section file, write it */

        if ((pagestate != OZ_SECTION_PAGESTATE_VALID_R) && (section -> sectype & OZ_SECTION_TYPE_SECFIL)) {
          oz_hw_pte_writeall (vpage, OZ_SECTION_PAGESTATE_WRITEINPROG, phypage, OZ_HW_PAGEPROT_NA, reqprot); /* make page inaccessible so no one can modify it or start any I/O going on it */
													/* - this will cause anyone who wants to access the page to wait for us to finish with it */
          oz_knl_process_unlkpt (process, OZ_SMPLOCK_SOFTINT);						/* release lock but stay at softint so we can't be aborted */
          writepage_to_secfile (section, secpage, phypage);						/* write page back to section file */
													/* note that we don't bother with WRITEFAILED state because our job is to do what it takes to get to PAGEDOUT state */
          oz_knl_process_lockpt (process);								/* lock the pagetable */
          oz_knl_event_set (section -> pfevent, 1);							/* in case some other thread is waiting for us to finish writing it */
          keptptlock = 0;										/* tell the caller we released the lock */
        }

        /* No one else is accessing the page either, so free it off */

        free_phypage (section, phypage);
      }

      /* We are no longer accessing it so mark the entry no longer accessible */

      oz_hw_pte_writeall (vpage, OZ_SECTION_PAGESTATE_PAGEDOUT, 0, OZ_HW_PAGEPROT_NA, OZ_HW_PAGEPROT_NA); 
      return (keptptlock);
    }

    /* No other state should be possible */

    default: oz_crash ("oz_knl_section_pageout: invalid page state %d", pagestate);
  }

  /************************************/
  /*  Global section page processing  */
  /************************************/

unmapglobal:

  switch (pagestate) {

    /* It is already paged out to disk - we don't have to do anything.  The last one to go to pagedout */
    /* state should have written the global page back out to disk and unloaded it from memory.         */

    case OZ_SECTION_PAGESTATE_PAGEDOUT: {
      if (phypage != 0) {									/* maybe free pagefile page */
        oz_hw_pte_writecur (vpage, OZ_SECTION_PAGESTATE_PAGEDOUT, 0, OZ_HW_PAGEPROT_NA, OZ_HW_PAGEPROT_NA); /* update pte with phypage = 0 */
												/* ... indicating no more pagfile page */
        free_pfpage (section, phypage);								/* free off reference to pagefile page */
      }
      return (keptptlock);
    }

    /* We are referencing the page.  So decrement the reference count, then, if we were the last one referencing it, write it   */
    /* to disk (if it was dirty), then mark the global page as being paged out and free the memory, because no one is using it. */

    case OZ_SECTION_PAGESTATE_VALID_R:
    case OZ_SECTION_PAGESTATE_VALID_W: {
      rc = waitioidle (1, vpage, process);								/* wait for I/O to finish */
      if (rc < 0) keptptlock = 0;

      /* If it is mapped to a disk cache page, tell cache if it has been modified then release it */

      if (oz_s_phymem_pages[phypage].state == OZ_PHYMEM_PAGESTATE_ALLOCACHE) {
        if (pagestate != OZ_SECTION_PAGESTATE_VALID_R) {
          if (!(section -> sectype & OZ_SECTION_TYPE_SECFIL)) oz_crash ("oz_knl_section_unmappage: wrote to cache page that doesn't get written to disk");
          oz_hw_pte_writeall (vpage, OZ_SECTION_PAGESTATE_WRITEINPROG, phypage, OZ_HW_PAGEPROT_NA, reqprot); /* make page inaccessible so no one can modify it or start any I/O going on it */
													/* - this will cause anyone who wants to access the page to wait for us to finish with it */
          oz_knl_process_unlkpt (process, OZ_SMPLOCK_SOFTINT);						/* release lock but stay at softint so we can't be aborted */
          sts = (*(section -> knlpfupd)) (section -> file, 
                                          (secpage << (OZ_HW_L2PAGESIZE - section -> l2blocksize)) + section -> vbnstart, 
                                          phypage);
          if (sts != OZ_SUCCESS) oz_knl_printk ("oz_knl_section_unmappage: error %u writing page %u back to cache", sts, phypage);
          oz_knl_process_lockpt (process);								/* lock the pagetable */
          oz_knl_event_set (section -> pfevent, 1);							/* in case some other thread is waiting for us to finish writing it */
        }
        oz_hw_pte_writeall (vpage, OZ_SECTION_PAGESTATE_PAGEDOUT, 0, OZ_HW_PAGEPROT_NA, OZ_HW_PAGEPROT_NA); 
        oz_knl_process_unlkpt (process, OZ_SMPLOCK_SOFTINT);						/* release lock but stay at softint so we can't be aborted */
        (*(section -> knlpfrel)) (section -> file, phypage);						/* release cache page */
        oz_knl_process_lockpt (process);								/* lock the pagetable */
        return (0);											/* tell caller we released smp lock */
      }

      /* Section page */

      gp = oz_hw_smplock_wait (&(section -> smplock_gp));
      if (-- (oz_s_phymem_pages[phypage].u.s.ptrefcount) != 0) {					/* see if we were the last pagetable to reference it */
        oz_hw_smplock_clr (&(section -> smplock_gp), gp);
        oz_hw_pte_writeall (vpage, OZ_SECTION_PAGESTATE_PAGEDOUT, 0, OZ_HW_PAGEPROT_NA, OZ_HW_PAGEPROT_NA); /* someone else is accessing it, turn off our access */
        return (keptptlock);										/* we're all done with it */
      }
      pfpage = oz_s_phymem_pages[phypage].u.s.pfpage;
      if (gblpage -> pagestate == OZ_SECTION_PAGESTATE_VALID_W) {					/* see if page needs to be written to disk */
        gblpage -> pagestate = OZ_SECTION_PAGESTATE_WRITEINPROG;					/* tell anyone in the whole system to wait while we write this page out. */
													/* note that there currently isn't anyone else referencing this page */
													/* (because ptrefcount = 0), but once we release the gp lock, others could */
													/* could come along and want to access the page while we are trying to */
													/* copy it out to disk. */
        oz_hw_smplock_clr (&(section -> smplock_gp), gp);
        oz_hw_pte_writeall (vpage, OZ_SECTION_PAGESTATE_WRITEINPROG, phypage, ppro[reqprot], reqprot);	/* make page readonly for this process and tell other threads */
													/* in this process they have to wait if they want to write */
        oz_knl_process_unlkpt (process, OZ_SMPLOCK_SOFTINT);						/* release lock but stay at softint so we can't be aborted */
        if (pfpage == 0) sts = writepage_to_secfile (section, secpage, phypage);			/* write page back to section file */
        else sts = writepage_to_pagefile (section, pfpage, phypage);					/* ... or write to page file */
        oz_knl_process_lockpt (process);								/* lock the pagetable */
        gp = oz_hw_smplock_wait (&(section -> smplock_gp));
        if (sts == OZ_SUCCESS) {
          gblpage -> pagestate = OZ_SECTION_PAGESTATE_VALID_R;						/* if successful, tell others that the disk matches the memory now */
          oz_hw_pte_writecur (vpage, OZ_SECTION_PAGESTATE_VALID_R, phypage, ppro[reqprot], reqprot);	/* put it in VALID_R state */
        } else {
          gblpage -> pagestate = OZ_SECTION_PAGESTATE_WRITEFAILED;					/* if failure, tell others that the disk write failed */
          gblpage -> phypage   = sts;
          oz_hw_pte_writeall (vpage, OZ_SECTION_PAGESTATE_WRITEFAILED, sts, OZ_HW_PAGEPROT_NA, reqprot); /* put in WRITEFAILED state */
        }
        oz_hw_smplock_clr (&(section -> smplock_gp), gp);
        oz_knl_event_set (section -> pfevent, 1);							/* in case some other thread is waiting for us to finish writing it */
        keptptlock = 0;
        goto tryagain;											/* go process its new state */
      }
      if (gblpage -> pagestate != OZ_SECTION_PAGESTATE_VALID_R) oz_crash ("oz_knl_section_unmappage: bad global page state %d", gblpage -> pagestate);
      oz_hw_pte_writeall (vpage, OZ_SECTION_PAGESTATE_PAGEDOUT, 0, OZ_HW_PAGEPROT_NA, OZ_HW_PAGEPROT_NA); /* don't allow access by this pagetable anymore */
      gblpage -> pagestate = OZ_SECTION_PAGESTATE_PAGEDOUT;						/* don't allow access by anyone new */
      gblpage -> phypage   = pfpage;									/* remember where the page is stored, though */
      oz_hw_smplock_clr (&(section -> smplock_gp), gp);							/* let other's have at it */
      return (keptptlock);
    }

    /* No other state should be possible. _VALID_D is not used for global pages. */

    default: oz_crash ("oz_knl_section_pageout: invalid page state %d", pagestate);
  }
}

/************************************************************************/
/*									*/
/*  Assuming that the vaddr is paged out, determine if it would fault 	*/
/*  in as a demand zero page						*/
/*									*/
/*    Input:								*/
/*									*/
/*	vaddr = virtual address of current process to test		*/
/*	        must be in a private section				*/
/*	smplock = this process' pt is locked				*/
/*									*/
/*    Output:								*/
/*									*/
/*	itsadzvaddr = 0 : it might not be all zeroes			*/
/*	              1 : it will be all zeroes				*/
/*									*/
/************************************************************************/

static int itsadzvaddr (void *vaddr)

{
  OZ_Hw_pageprot curprot, pageprot, reqprot;
  OZ_Mempage pageoffs, phypage, vpage;
  OZ_Process *process;
  OZ_Procmode procmode;
  OZ_Section *section;
  OZ_Section_pagestate pagestate;
  uLong mapsecflags;

  /* See what section is at that virtual address */

checkitout:
  vpage = OZ_HW_VADDRTOVPAGE (vaddr);
  process = getprocess (vpage);
  if (oz_knl_process_getsecfromvpage (process, vpage, &section, &pageoffs, &pageprot, &procmode, &mapsecflags) == 0) return (0);

  /* If it's global, someone else might be scribbling on it */

  if (section -> sectype & OZ_SECTION_TYPE_GLOBAL) return (0);

  /* If it's not a demand-zero section, who knows what's on disk */

  if (!(section -> sectype & OZ_SECTION_TYPE_ZEROES)) return (0);

  /* Try to read the pagetable entry.  If it's out, then assume we're zero iff it is zero.  Otherwise we can't be sure. */

  vaddr = oz_hw_pte_readany (vpage, &pagestate, &phypage, &curprot, &reqprot);
  if (vaddr != NULL) goto checkitout;

  /* If the page is out and its the initial load, then it's full of zeroes.  Otherwise we can't be sure. */

  return ((pagestate == OZ_SECTION_PAGESTATE_PAGEDOUT) && (phypage == 0));
}

/************************************************************************/
/*									*/
/*  This routine is called to wait for all i/o on a virtual page to 	*/
/*  finish								*/
/*									*/
/*    Input:								*/
/*									*/
/*	wait = 0 : return immediately if page is busy			*/
/*	       1 : wait if page is busy					*/
/*	vpage = virtual page to check					*/
/*	smp lock = pt							*/
/*									*/
/*    Output:								*/
/*									*/
/*	waitioidle = 1 : page was not busy to begin with		*/
/*	             0 : page is busy					*/
/*	            -1 : page was busy, but now it is not		*/
/*									*/
/************************************************************************/

static int waitioidle (int wait, OZ_Mempage vpage, OZ_Process *process)

{
  OZ_Seclkwz *seclkwz, **seclkwzs;
  OZ_Seclock *seclock, **seclocks;
  uLong sts;

  seclocks = oz_knl_process_seclocks (process, 1, vpage);	/* get list of locks pertaining to this address */
								/* - should always be non-NULL as we should already know there is a section mapped at this vpage */
  for (seclock = *seclocks; seclock != NULL; seclock = seclock -> next) { /* scan the list for lock pertaining to this address */
    if ((vpage >= seclock -> svpage) && (vpage - seclock -> svpage < seclock -> npages)) goto locked;
  }
  return (1);							/* no lock found, the page is not busy, and we did not release the pt lock */

locked:
  if (!wait) return (0);					/* busy, if not unmapping, return a 0 saying it's busy without unlocking */

  seclkwz = OZ_KNL_NPPMALLOC (sizeof *seclkwz);			/* unmapping a busy page, allocate an wait for lock zero block */
  seclkwz -> objtype = OZ_OBJTYPE_SECLKWZ;
  seclkwz -> vpage   = vpage;
  sts = oz_knl_event_create (29, "oz_knl_section waitioidle", NULL, &(seclkwz -> event)); /* create an event flag for the i/o wait event block */
  if (sts != OZ_SUCCESS) oz_crash ("oz_knl_section waitioidle: error %u creating event flag", sts);

linkit:
  vfyseclocks (process, vpage);
  seclkwzs = oz_knl_process_seclkwzs (process, vpage);		/* get listhead corresponding to the virtual page for this process */
  seclkwz -> next = *seclkwzs;					/* tell oz_knl_section_iounlk that someone is waiting for this page to go idle */
  seclkwz -> prev = seclkwzs;
  if (seclkwz -> next != NULL) seclkwz -> next -> prev = &(seclkwz -> next);
  *seclkwzs = seclkwz;
  oz_knl_process_nseclkwzs (process, vpage, 1);
  vfyseclocks (process, vpage);

  oz_knl_process_unlkpt (process, OZ_SMPLOCK_SOFTINT);		/* unlock pagetables so the I/O can finish */
  do oz_knl_event_waitone (seclkwz -> event);			/* wait for I/O to unlock the page */
  while (seclkwz -> prev != NULL);
  oz_knl_process_lockpt (process);

  seclocks = oz_knl_process_seclocks (process, 1, vpage);	/* there might not even be a section mapped there anymore, */
  if (seclocks != NULL) {					/* ... so fetch this again and make sure it is not NULL */
    for (seclock = *seclocks; seclock != NULL; seclock = seclock -> next) {
      if ((vpage >= seclock -> svpage) && (vpage - seclock -> svpage < seclock -> npages)) {
        oz_knl_event_set (seclkwz -> event, 0);			/* page still has I/O going on it, clear the event flag so we will wait */
        goto linkit;						/* ... then go back and wait again for I/O on the page to finish */
      }
    }
  }

  oz_knl_event_increfc (seclkwz -> event, -1);			/* the page is no longer locked, free off the event flag we used for waiting */
  OZ_KNL_NPPFREE (seclkwz);					/* free off the block we used for waiting */
  return (-1);							/* return -1 saying it is not busy now, but we released the pt lock and re-locked it */
}

/************************************************************************/
/*									*/
/*  Write a page back to the section file				*/
/*									*/
/*    Input:								*/
/*									*/
/*	section = section being written					*/
/*	secpage = page within the section being written			*/
/*	phypage = physical page being written				*/
/*	smplock = softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	writepage_to_secfile = write status				*/
/*									*/
/************************************************************************/

static uLong writepage_to_secfile (OZ_Section *section, OZ_Mempage secpage, OZ_Mempage phypage)

{
  OZ_IO_fs_pagewrite fs_pagewrite;
  uLong sts;
  volatile uLong status;

  memset (&fs_pagewrite, 0, sizeof fs_pagewrite);
  fs_pagewrite.startblock = (secpage << (OZ_HW_L2PAGESIZE - section -> l2blocksize)) + section -> vbnstart;
  fs_pagewrite.pagecount  = 1;
  fs_pagewrite.pagearray  = &phypage;
  status = OZ_PENDING;
  sts = oz_knl_iostart3 (1, NULL, section -> file, OZ_PROCMODE_KNL, NULL, NULL, &status, section -> pfevent, 
                         NULL, NULL, OZ_IO_FS_PAGEWRITE, sizeof fs_pagewrite, &fs_pagewrite);
  if (sts == OZ_STARTED) {
    while ((sts = status) == OZ_PENDING) {
      oz_knl_event_waitone (section -> pfevent);
      oz_knl_event_set (section -> pfevent, 0);
    }
  }
  if (sts != OZ_SUCCESS) oz_knl_printk ("oz_knl_section writepage_to_secfile: section file write error status %u, phypage %u\n", sts, phypage);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Write a page to the pagefile					*/
/*									*/
/************************************************************************/

static uLong writepage_to_pagefile (OZ_Section *section, OZ_Mempage pfpage, OZ_Mempage phypage)

{
  oz_crash ("oz_knl_section: writepage_to_pagefile not implemented");
  return (OZ_ABORTED);
}

/************************************************************************/
/*									*/
/*  Get process for the given virtual page				*/
/*									*/
/************************************************************************/

static OZ_Process *getprocess (OZ_Mempage vpage)

{
  OZ_Process *process;

  process = oz_s_systemproc;						// system process if system common page
  if (!OZ_HW_ISSYSPAGE (vpage)) process = oz_knl_process_getcur ();	// otherwise, current process

  return (process);
}

/************************************************************************/
/*									*/
/*  Allocate physical memory page					*/
/*									*/
/*    Input:								*/
/*									*/
/*	section = section the page is for				*/
/*									*/
/*    Output:								*/
/*									*/
/*	alloc_phypage = OZ_SUCCESS : successfully allocated		*/
/*	                             *phypage_r = physical page number	*/
/*	               OZ_NOMEMORY : no memory available		*/
/*	                      else : error status (no quota)		*/
/*									*/
/************************************************************************/

static uLong alloc_phypage (OZ_Section *section, OZ_Mempage *phypage_r, OZ_Mempage vpage)

{
  OZ_Mempage phypage;
  uLong pm;

  /* Debit the quota for the physical page.  If none available, return error status. */
  /* ?? allow this to go over quota if being accessed in kernel mode so we dont crash ?? */

  if ((section -> quota != NULL) && !oz_knl_quota_debit (section -> quota, OZ_QUOTATYPE_PHM, 1)) {
    return (OZ_EXQUOTAPHM);
  }

  /* Try to allocate page.  If got one, return the page number and success status. */

  vpage  ^= oz_knl_process_getid (oz_knl_process_getcur ());			// mix around a little so we aren't always pounding on same free list
  pm      = oz_hw_smplock_wait (&oz_s_smplock_pm);				// lock free memory lists
  phypage = oz_knl_phymem_allocpage (OZ_PHYMEM_PAGESTATE_ALLOCSECT, vpage);	// try to allocate a page
  oz_hw_smplock_clr (&oz_s_smplock_pm, pm);					// unlock free memory lists
  if (phypage != OZ_PHYPAGE_NULL) {
    *phypage_r = phypage;							// return physical page number
    oz_s_phymem_pages[phypage].u.s.pfpage     = 0;				// no pagefile page for it yet
    oz_s_phymem_pages[phypage].u.s.ptrefcount = 1;				// only caller's pte will be pointing to it
    return (OZ_SUCCESS);
  }

  /* Failed to allocate page, credit back the quota */
  /* ?? someday, page something out to get a page ?? */

  if (section -> quota != NULL) oz_knl_quota_credit (section -> quota, OZ_QUOTATYPE_PHM, 1, -1);
  return (OZ_NOMEMORY);
}

/************************************************************************/
/*									*/
/*  Free memory page							*/
/*									*/
/*    Input:								*/
/*									*/
/*	section = section the page was part of				*/
/*	phypage = page being freed off					*/
/*	smplevel <= pm							*/
/*									*/
/************************************************************************/

static void free_phypage (OZ_Section *section, OZ_Mempage phypage)

{
  uLong pm;

  if (phypage >= oz_s_phymem_totalpages) oz_crash ("oz_knl_section free_phypage: bad phypage %u", phypage);
  pm = oz_hw_smplock_wait (&oz_s_smplock_pm);
  if (oz_s_phymem_pages[phypage].state != OZ_PHYMEM_PAGESTATE_ALLOCSECT) oz_crash ("oz_knl_section free_phypage: page %u state %d", phypage, oz_s_phymem_pages[phypage].state);
  if (oz_s_phymem_pages[phypage].u.s.pfpage != 0) oz_crash ("oz_knl_section free_phypage: page %u pfpage %u", phypage, oz_s_phymem_pages[phypage].u.s.pfpage);
  if (oz_s_phymem_pages[phypage].u.s.ptrefcount != 0) oz_crash ("oz_knl_section free_phypage: page %u ptrefcount %u", phypage, oz_s_phymem_pages[phypage].u.s.ptrefcount);
  oz_knl_phymem_freepage (phypage);
  oz_hw_smplock_clr (&oz_s_smplock_pm, pm);
  if (section -> quota != NULL) oz_knl_quota_credit (section -> quota, OZ_QUOTATYPE_PHM, 1, -1);
}

/************************************************************************/
/*									*/
/*  Free off reference to pagefile page.  If refcount goes zero, then 	*/
/*  free off the pagefile page for use by somone else.			*/
/*									*/
/*    Input:								*/
/*									*/
/*	section  = section the pagefile page was used for		*/
/*	pfpage   = pagefile page that is being freed			*/
/*	smplevel = pt							*/
/*									*/
/************************************************************************/

static void free_pfpage (OZ_Section *section, OZ_Mempage pfpage)

{
  oz_knl_pagefile_incpfrefc (section -> pagefile, pfpage, -1);
}
