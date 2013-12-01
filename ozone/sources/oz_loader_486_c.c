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

#include "ozone.h"
#include "oz_dev_timer.h"
#include "oz_knl_hw.h"
#include "oz_knl_sdata.h"
#include "oz_ldr_loader.h"

uLong *oz_hw_ldr_memory_megabytes_p = &(((OZ_Loadparams *)OZ_LDR_PRMBASE) -> memory_megabytes);

void oz_knl_thread_timerinit ()

{ }

/* Called when kernel pages are loaded */
/* In this version we crash because we don't actually load the kernel */

oz_hw_ldr_knlpagesloaded ()

{
  oz_crash ("oz_hw_ldr_knlpagesloaded: this version does not actually load the kernel");
}

/* Called when there is nothing to do.  In this version, we just exit */
/* and let whoever check again, since we have nothing better to do.   */

void oz_hw_cpu_waitint (void *waitq)

{
  oz_hw_cpu_setsoftint (1);
  oz_hw_cpu_setsoftint (0);
}

/* Probe buffer addressibility - these assume that if the pointer is not null, the buffer is addressible. */

int oz_hw_readable_strz (void *buff, OZ_Procmode procmode)

{
  return (buff != NULL);
}

/************************************************************************/
/*									*/
/*  Move physical to/from virtual addresses				*/
/*									*/
/*  Since for us physical == virtual addresses, these are just memcpy's	*/
/*									*/
/************************************************************************/

void oz_hw_phys_movefromvirt (uLong nbytes, const void *vaddr, const OZ_Mempage *phypages, uLong byteoffs)

{
  memcpy ((void *)((phypages[0] << OZ_HW_L2PAGESIZE) + byteoffs), vaddr, nbytes);
}

void oz_hw_phys_movetovirt (uLong nbytes, void *vaddr, const OZ_Mempage *phypages, uLong byteoffs)

{
  memcpy (vaddr, (void *)((phypages[0] << OZ_HW_L2PAGESIZE) + byteoffs), nbytes);
}

void oz_hw_phys_movephys (uLong nbytes, const OZ_Mempage *src_pages, uLong src_offs, const OZ_Mempage *dst_pages, uLong dst_offs)

{
  memcpy ((void *)((dst_pages[0] << OZ_HW_L2PAGESIZE) + dst_offs), 
          (void *)((src_pages[0] << OZ_HW_L2PAGESIZE) + src_offs), 
          nbytes);
}

/************************************************************************/
/*									*/
/*  Move machine args to outermode stack				*/
/*									*/
/************************************************************************/

uLong oz_hw486_movemchargstocallerstack (void)

{
  oz_crash ("oz_hw486_movemchargstocallerstack: not supported in loader");
}

/************************************************************************/
/*									*/
/*  If the kernel is linked dynamically, this will determine the 	*/
/*  actual load address.  We base it on the number of superpages 	*/
/*  needed for the I/O system.						*/
/*									*/
/*    Input:								*/
/*									*/
/*	pageprot = OZ_HW_PAGEPROT_KW or _UR				*/
/*	npagem   = number of pages being mapped				*/
/*	svpage   = starting relative virtual page number within image	*/
/*									*/
/************************************************************************/

OZ_Mempage oz_hw_ldr_knlpage_basevp (OZ_Hw_pageprot pageprot, OZ_Mempage npagem, OZ_Mempage svpage)

{
  oz_crash ("oz_hw_ldr_knlpage_basevp: kernel must be linked statically at %p", OZ_LDR_KNLBASEVA);
  return (svpage + OZ_HW_VADDRTOVPAGE (OZ_LDR_KNLBASEVA));
}
