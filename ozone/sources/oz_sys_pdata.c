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
/*  These routines handle the OZ_Pdata array.  There is one element in 	*/
/*  this array per processor mode, protected at that processor's 	*/
/*  level.  The array resides at the same virtual address for all 	*/
/*  processes, but the contents are unique (like pagetables).		*/
/*									*/
/*  Elements owned by a particular processor mode cannot be accessed 	*/
/*  by an outer (less privileged) processor mode.			*/
/*									*/
/*  Elements should be considered pageable and thus can only be 	*/
/*  accessed at softint level or below.					*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_knl_event.h"
#include "oz_knl_hw.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_process.h"
#include "oz_knl_sdata.h"
#include "oz_knl_section.h"
#include "oz_knl_status.h"
#include "oz_sys_event.h"
#include "oz_sys_handle.h"
#include "oz_sys_pdata.h"
#include "oz_sys_process.h"
#include "oz_sys_section.h"
#include "oz_sys_thread.h"

#define SECTIONSIZE (1024*1024)

static uLong memlockk (void *pdatav);
static void memunlkk (void *pdatav, uLong aststs);
static uLong memlocko (void *pdatav);
static void memunlko (void *pdatav, uLong aststs);

/************************************************************************/
/*									*/
/*  Point to the appropriate pdata struct for the given processor mode	*/
/*									*/
/*    Input:								*/
/*									*/
/*	procmode = OZ_PROCMODE_SYS : point to system global struct	*/
/*	                             (only for kernel mode callers)	*/
/*	                      else : get this proc mode's data		*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_pdata_pointer = points to corresponding pdata struct	*/
/*									*/
/*    Note:								*/
/*									*/
/*	If it is known that the caller is in kernel mode and procmode	*/
/*	is known not to be OZ_PROCMODE_SYS, use the 			*/
/*	OZ_SYS_PDATA_FROM_KNL macro instead.				*/
/*									*/
/*	If it is known that the caller is in kernel mode and procmode 	*/
/*	is know to be OZ_PROCMODE_SYS, use the OZ_SYS_PDATA_SYSTEM 	*/
/*	macro instead.							*/
/*									*/
/************************************************************************/

OZ_Pdata *oz_sys_pdata_pointer (OZ_Procmode procmode)

{
  if (procmode == OZ_PROCMODE_SYS) return (&oz_s_systempdata);	// if caller wants system-global data, give it to them
  procmode = oz_hw_procmode_max (procmode);			// otherwise, maximize mode (eg, user mode caller always get user mode data)
  return (&(oz_sys_pdata_array[procmode-OZ_PROCMODE_MIN].data)); // return pointer to corresponding per-process data
}

/************************************************************************/
/*									*/
/*  Per-Process malloc/free routines					*/
/*									*/
/*    Input:								*/
/*									*/
/*	procmode = processor mode to be owner of data			*/
/*	size = number of bytes to allocate				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_pdata_malloc = NULL : allocate failed (no mem left)	*/
/*	                      else : pointer to memory block		*/
/*									*/
/*    Notes:								*/
/*									*/
/*	Buffers allocated by these routines are addressible only by 	*/
/*	this process (unless subsequently double-mapped).		*/
/*									*/
/*	These routines are completely ast/threadsafe, synchronized by 	*/
/*	atomic updates and event flags.					*/
/*									*/
/************************************************************************/

void *oz_sys_pdata_malloc (OZ_Procmode procmode, uLong size)

{
  OZ_Event *event;
  OZ_Handle h_event, h_section;
  OZ_Mempage npagem, svpage;
  OZ_Memsize pgppeak, rsize;
  OZ_Pdata *pdata;
  OZ_Section *section;
  uLong ssize, sts;
  void *buff, *sadrs;

  pdata = oz_sys_pdata_pointer (procmode);

  /* Process differently based on what pool we're actually allocating from */

  procmode = pdata -> pprocmode;
  switch (procmode) {

    /* Malloc'ing from the system common memory is the paged pool */

    case OZ_PROCMODE_SYS: {
      if (pdata -> mem_lock_event == NULL) {						// make sure lock event defined
        if (oz_s_systemproc == NULL) return (OZ_KNL_NPPMALLOC (size));			// use npp if in boot
											// - assume mem never gets freed
        sts = oz_knl_event_create (19, "oz_sys_pdata_malloc", NULL, &event);		// if not, create one
        if (sts != OZ_SUCCESS) return (NULL);
        oz_knl_event_set (event, 1);							// put it in unlocked state
        if (!oz_hw_atomic_setif_ptr (&(pdata -> mem_lock_event), event, NULL)) {	// maybe someone else got here first
          oz_knl_event_increfc (event, -1);						// if so, free ours off
        }
      }

      buff = oz_malloc (pdata -> mem_list, size, &rsize, 0);				// try to alloc from what we have left
      if (buff != NULL) {
        oz_hw_atomic_inc_long (&oz_s_pgpinuse, rsize);					// if got it, inc pgp in use
        do pgppeak = oz_s_pgppeak;							// ... maybe inc peak use
        while ((oz_s_pgpinuse > pgppeak) && !oz_hw_atomic_setif_long (&oz_s_pgppeak, oz_s_pgpinuse, pgppeak));
        break;										// ... and return pointer
      }

      pdata = &oz_s_systempdata;							// use global pointer so oz_knl_process_mapsection knows what to do
      ssize = size + (2 << OZ_HW_L2PAGESIZE);						// plenty of extra for rounding and overhead
      if (ssize < SECTIONSIZE) ssize = SECTIONSIZE;					// ... but always do at least this much
      npagem = ssize >> OZ_HW_L2PAGESIZE;						// get number of pages to allocate
      sts = oz_knl_section_create (NULL, npagem, 0, OZ_SECTION_TYPE_ZEROES, NULL, &section);
      if (sts != OZ_SUCCESS) return (NULL);
      svpage = OZ_HW_VADDRTOVPAGE (pdata);						// put it near the pdata
      sts = oz_knl_process_mapsection (section, &npagem, &svpage, OZ_MAPSECTION_ATEND, OZ_PROCMODE_KNL, OZ_HW_PAGEPROT_KW);
      oz_knl_section_increfc (section, -1);						// release section handle
      if (sts != OZ_SUCCESS) return (NULL);						// fail if it wouldn't map

      ssize = npagem << OZ_HW_L2PAGESIZE;						// get number of bytes mapped
      sadrs = OZ_HW_VPAGETOVADDR (svpage);						// get address it was mapped at
      sts = memlockk (pdata);								// lock malloc list
      pdata -> mem_list = oz_freesiz (pdata -> mem_list, ssize, sadrs, memlockk, memunlkk, pdata); // free new block to list
      buff = oz_malloc_lk (pdata -> mem_list, size, &rsize, 0);				// now we should be able to malloc
      oz_s_pgptotal += ssize;								// total paged pool increased
      oz_hw_atomic_inc_long (&oz_s_pgpinuse, rsize);					// ... and this much more in use
      do pgppeak = oz_s_pgppeak;
      while ((oz_s_pgpinuse > pgppeak) && !oz_hw_atomic_setif_long (&oz_s_pgppeak, oz_s_pgpinuse, pgppeak));
      memunlkk (pdata, sts);								// unlock malloc list
      break;
    }

    /* Use kernel object pointers for per-process kernel mode */
    /* a) it might be the handle table we're malloc'ing for   */
    /* b) it's faster                                         */

    case OZ_PROCMODE_KNL: {
      if (pdata -> mem_lock_event == NULL) {						// make sure lock event defined
        sts = oz_knl_event_create (19, "oz_sys_pdata_malloc", NULL, &event);		// if not, create one
        if (sts != OZ_SUCCESS) return (NULL);
        oz_knl_event_set (event, 1);							// put it in unlocked state
        if (!oz_hw_atomic_setif_ptr (&(pdata -> mem_lock_event), event, NULL)) {	// maybe someone else got here first
          oz_knl_event_increfc (event, -1);						// if so, free ours off
        }
      }

      buff = oz_malloc (pdata -> mem_list, size, &rsize, 0);				// try to alloc from what we have left
      if (buff != NULL) break;								// if got it, return pointer

      ssize = size + (2 << OZ_HW_L2PAGESIZE);						// plenty of extra for rounding and overhead
      if (ssize < SECTIONSIZE) ssize = SECTIONSIZE;					// ... but always do at least this much
      npagem = ssize >> OZ_HW_L2PAGESIZE;						// get number of pages to allocate
      sts = oz_knl_section_create (NULL, npagem, 0, OZ_SECTION_TYPE_ZEROES, NULL, &section);
      if (sts != OZ_SUCCESS) return (NULL);
      svpage = OZ_HW_VADDRTOVPAGE (OZ_HW_DVA_KNLHEAP_VA);				// this is per-process kernel heap
      sts = oz_knl_process_mapsection (section, &npagem, &svpage, OZ_HW_DVA_KNLHEAP_AT, OZ_PROCMODE_KNL, OZ_HW_PAGEPROT_KW);
      oz_knl_section_increfc (section, -1);						// release section handle
      if (sts != OZ_SUCCESS) return (NULL);						// fail if it wouldn't map

      ssize = npagem << OZ_HW_L2PAGESIZE;						// get number of bytes mapped
      sadrs = OZ_HW_VPAGETOVADDR (svpage);						// get address it was mapped at
      sts = memlockk (pdata);								// lock malloc list
      pdata -> mem_list = oz_freesiz (pdata -> mem_list, ssize, sadrs, memlockk, memunlkk, pdata); // free new block to list
      buff = oz_malloc_lk (pdata -> mem_list, size, &rsize, 0);				// now we should be able to malloc
      memunlkk (pdata, sts);								// unlock malloc list
      break;
    }

    /* Do it with handles for all outer modes */

    default: {
      if (pdata -> mem_lock_handle == 0) {						// make sure lock event defined
        sts = oz_sys_event_create (procmode, "oz_sys_pdata_malloc", &h_event);		// if not, create one
        if (sts != OZ_SUCCESS) return (NULL);
        oz_sys_event_set (procmode, h_event, 1, NULL);					// put it in unlocked state
        if (!oz_hw_atomic_setif_long (&(pdata -> mem_lock_handle), h_event, 0)) {		// maybe someone else got here first
          oz_sys_handle_release (OZ_PROCMODE_KNL, h_event);				// if so, free ours off
        }
      }

      buff = oz_malloc (pdata -> mem_list, size, NULL, 0);				// try to alloc from what we have left
      if (buff != NULL) break;								// if got it, return pointer

      ssize = size + (2 << OZ_HW_L2PAGESIZE);						// plenty of extra for rounding and overhead
      if (ssize < SECTIONSIZE) ssize = SECTIONSIZE;					// ... but always do at least this much
      npagem = ssize >> OZ_HW_L2PAGESIZE;						// get number of pages to allocate
      sts = oz_sys_section_create (procmode, 0, npagem, 0, OZ_SECTION_TYPE_ZEROES, &h_section);
      if (sts != OZ_SUCCESS) return (NULL);
      svpage = OZ_HW_VADDRTOVPAGE (OZ_HW_DVA_USRHEAP_VA);				// this is usermode heap
      sts = oz_sys_process_mapsection (procmode, h_section, &npagem, &svpage, OZ_HW_DVA_USRHEAP_AT, pdata -> rwpageprot);
      oz_sys_handle_release (OZ_PROCMODE_KNL, h_section);				// release section handle
      if (sts != OZ_SUCCESS) return (NULL);						// fail if it wouldn't map

      ssize = npagem << OZ_HW_L2PAGESIZE;						// get number of bytes mapped
      sadrs = OZ_HW_VPAGETOVADDR (svpage);						// get address it was mapped at
      sts = memlocko (pdata);								// lock malloc list
      pdata -> mem_list = oz_freesiz (pdata -> mem_list, ssize, sadrs, memlocko, memunlko, pdata); // free new block to list
      buff = oz_malloc_lk (pdata -> mem_list, size, NULL, 0);				// now we should be able to malloc
      memunlko (pdata, sts);								// unlock malloc list
      break;
    }
  }

  return (buff);
}

/************************************************************************/
/*									*/
/*  Free data allocated with oz_sys_pdata_malloc			*/
/*									*/
/*    Input:								*/
/*									*/
/*	procmode = as passed to oz_sys_pdata_malloc			*/
/*	buff = as returned by oz_sys_pdata_malloc			*/
/*									*/
/*    Output:								*/
/*									*/
/*	buff = no longer valid						*/
/*									*/
/************************************************************************/

void oz_sys_pdata_free (OZ_Procmode procmode, void *buff)

{
  OZ_Memsize rsize;
  OZ_Pdata *pdata;

  if (buff != NULL) {
    pdata = oz_sys_pdata_pointer (procmode);
    rsize = oz_free (pdata -> mem_list, buff);
    if (pdata == &oz_s_systempdata) oz_hw_atomic_inc_long (&oz_s_pgpinuse, -rsize);
  }
}

/************************************************************************/
/*									*/
/*  Get usable memory size						*/
/*									*/
/************************************************************************/

uLong oz_sys_pdata_valid (OZ_Procmode procmode, void *buff)

{
  OZ_Pdata *pdata;

  pdata = oz_sys_pdata_pointer (procmode);
  return (oz_valid (pdata -> mem_list, buff));
}

/************************************************************************/
/*									*/
/*  Called when this is a copy of a process to fix things up		*/
/*									*/
/************************************************************************/

uLong oz_sys_pdata_copied (void)

{
  OZ_Event *event;
  OZ_Pdata *pdata;
  uLong sts;

  pdata = OZ_SYS_PDATA_FROM_KNL (OZ_PROCMODE_KNL);	// point to kernel array element

  /* Allocate a different event flag for kernel memory allocations */

  pdata -> mem_lock_event = NULL;
  sts = oz_knl_event_create (19, "oz_sys_pdata_malloc", NULL, &event);
  if (sts == OZ_SUCCESS) {
    oz_knl_event_set (event, 1);
    pdata -> mem_lock_event = event;
  }

  return (sts);
}

/************************************************************************/
/*									*/
/*  Called at process cleanup time to free stuff off			*/
/*									*/
/************************************************************************/

void oz_sys_pdata_cleanup (void)

{
  OZ_Event *event;
  OZ_Pdata *pdata;

  pdata = OZ_SYS_PDATA_FROM_KNL (OZ_PROCMODE_KNL);
  event = pdata -> mem_lock_event;
  if ((event != NULL) && oz_hw_atomic_setif_ptr (&(pdata -> mem_lock_event), NULL, event)) {
    oz_knl_event_increfc (event, -1);
  }
}

/************************************/
/* Memory list lock/unlock routines */
/************************************/

	/* kernel mode lists */

static uLong memlockk (void *pdatav)

{
  OZ_Pdata *pdata;
  uLong aststs;

  pdata = pdatav;
  aststs = oz_hw_cpu_setsoftint (0);					// make sure softint delivery inhibited so we can't re-enter
									// - can't inhib ast delivery because we might already be at softint level
  if (!oz_hw_atomic_setif_long (&(pdata -> mem_lock_flag), 1, 0)) {	// if unlocked, lock it and get out
    while (oz_hw_atomic_set_long (&(pdata -> mem_lock_flag), 3) & 1) {	// try to lock, but tell it someone might be waiting
      oz_knl_event_waitone (pdata -> mem_lock_event);			// still locked, have to wait
      oz_knl_event_set (pdata -> mem_lock_event, 0);			// clear flag in case we need to wait again
    }
  }
  return (aststs);
}

static void memunlkk (void *pdatav, uLong aststs)

{
  OZ_Pdata *pdata;

  pdata = pdatav;
  if (oz_hw_atomic_set_long (&(pdata -> mem_lock_flag), 0) & 2) {	// unlock, see if anyone was waiting
    oz_knl_event_set (pdata -> mem_lock_event, 1);			// someone waiting, wake them up
  }
  oz_hw_cpu_setsoftint (aststs);					// restore softint level
}

	/* outer mode lists */

static uLong memlocko (void *pdatav)

{
  OZ_Pdata *pdata;
  uLong aststs;

  pdata = pdatav;
  aststs = oz_sys_thread_setast (OZ_ASTMODE_INHIBIT);			// make sure ast delivery inhibited so we can't re-enter
  if (!oz_hw_atomic_setif_long (&(pdata -> mem_lock_flag), 1, 0)) {	// if unlocked, lock it and get out
    while (oz_hw_atomic_set_long (&(pdata -> mem_lock_flag), 3) & 1) {	// try to lock, but tell it someone might be waiting
      oz_sys_event_wait (pdata -> procmode, pdata -> mem_lock_handle, 0); // still locked, have to wait
      oz_sys_event_set (pdata -> procmode, pdata -> mem_lock_handle, 0, NULL); // clear flag in case we need to wait again
    }
  }
  return (aststs);
}

static void memunlko (void *pdatav, uLong aststs)

{
  OZ_Pdata *pdata;

  pdata = pdatav;
  if (oz_hw_atomic_set_long (&(pdata -> mem_lock_flag), 0) & 2) {	// unlock, see if anyone was waiting
    oz_sys_event_set (pdata -> procmode, pdata -> mem_lock_handle, 1, NULL); // someone waiting, wake them up
  }
  if (aststs == OZ_FLAGWASSET) oz_sys_thread_setast (OZ_ASTMODE_ENABLE); // restore ast delivery
}
