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

#include "oz_sys_syscall.h"

#include "oz_sys_section.h"

#include "oz_knl_handle.h"
#include "oz_knl_hw.h"
#include "oz_knl_process.h"
#include "oz_knl_sdata.h"
#include "oz_knl_section.h"
#include "oz_knl_status.h"

/************************************************************************/
/*									*/
/*  Create a section							*/
/*									*/
/*	h_file = 0 : no section file					*/
/*	      else : section file handle (iochan)			*/
/*	npages = number of pages to map, 0 = whole file			*/
/*	vbnstart = virt block number to start at			*/
/*	sectype = section type						*/
/*	pageprot = page protection					*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_section_create = OZ_SUCCESS : section created		*/
/*	                              else : error status		*/
/*	*h_section_r = section handle					*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_6 (section_create, OZ_Procmode, procmode, OZ_Handle, h_file, OZ_Mempage, npages, OZ_Dbn, vbnstart, OZ_Section_type, sectype, OZ_Handle *, h_section_r)

{
  int si;
  uLong sts;
  OZ_Iochan *file;
  OZ_Secattr *secattr;
  OZ_Seclock *seclock_h_section_r;
  OZ_Section *section;

  if ((cprocmode != OZ_PROCMODE_KNL) && (sectype & OZ_SECTION_TYPE_PAGTBL)) return (OZ_KERNELONLY);

  file = NULL;
  secattr = NULL;
  section = NULL;
  seclock_h_section_r = NULL;

  if (procmode < cprocmode) procmode = cprocmode;

  si = oz_hw_cpu_setsoftint (0);

  sts = oz_knl_section_iolockw (cprocmode, sizeof *h_section_r, h_section_r, &seclock_h_section_r, NULL, NULL, NULL);
  if (sts != OZ_SUCCESS) goto rtn;

  /* Lookup file handle */

  if (h_file != 0) {
    sts = oz_knl_handle_takeout (h_file, procmode, OZ_SECACCMSK_READ | OZ_SECACCMSK_WRITE, OZ_OBJTYPE_IOCHAN, &file, NULL);
    if (sts != OZ_SUCCESS) goto rtn;
  }

  /* Look up security attributes */

  secattr = oz_knl_thread_getdefcresecattr (NULL);

  /* Create a section structure */

  sts = oz_knl_section_create (file, npages, vbnstart, sectype, secattr, &section);
  if (sts != OZ_SUCCESS) goto rtn;

  /* Assign an handle to it */

  sts = oz_knl_handle_assign (section, procmode, h_section_r);

rtn:
  oz_knl_section_iounlk (seclock_h_section_r);
  if (section != NULL) oz_knl_section_increfc (section, -1);
  if (secattr != NULL) oz_knl_secattr_increfc (secattr, -1);
  if (file    != NULL) oz_knl_handle_putback  (h_file);
  oz_hw_cpu_setsoftint (si);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Set page protection							*/
/*									*/
/*	npages   = number of pages to set				*/
/*	svpage   = starting virtual page to set				*/
/*	pageprot = new page protection					*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_section_setpageprot = OZ_SUCCESS : protection changed	*/
/*	                                   else : error status		*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_4 (section_setpageprot, OZ_Mempage, npages, OZ_Mempage, svpage, OZ_Hw_pageprot, pageprot, OZ_Hw_pageprot *, pageprot_r)

{
  int si;
  OZ_Hw_pageprot mapprot;
  OZ_Mempage pagecount, pageoffs, syspage;
  OZ_Process *process;
  OZ_Procmode mapprocmode;
  OZ_Section *section;
  OZ_Hw_pageprot oldprot;
  uLong mapsecflags, sts;

  if ((pageprot_r != NULL) && !OZ_HW_WRITABLE (npages * sizeof *pageprot_r, pageprot_r, cprocmode)) return (OZ_ACCVIO);

  si  = oz_hw_cpu_setsoftint (0);

  syspage = OZ_HW_VADDRTOVPAGE (oz_s_sysmem_baseva);
  process = oz_s_systemproc;
  if ((svpage < syspage) || (svpage - syspage >= oz_s_sysmem_pagtblsz)) process = oz_knl_process_getcur ();

  sts = OZ_SUCCESS;							/* successful if no pages to change */
  while (npages > 0) {							/* repeat while there are pages to change */
    pagecount = oz_knl_process_getsecfromvpage (process, svpage, &section, &pageoffs, &mapprot, &mapprocmode, &mapsecflags);
    sts = OZ_ADDRNOTUSED;						/* error if nothing is mapped there */
    if (pagecount == 0) break;
    sts = OZ_PROCMODE;							/* error if mapped by an inner processor mode */
    if (mapprocmode < cprocmode) break;
    if (pagecount > npages) pagecount = npages;				/* ok, do as much as we can this time around */
    sts = oz_knl_section_setpageprot (pagecount, svpage, pageprot, NULL, pageprot_r); /* modify the page's protection code */
    if (sts != OZ_SUCCESS) break;					/* stop if error */
    npages -= pagecount;						/* on to next mapped section */
    svpage += pagecount;
    if (pageprot_r != NULL) pageprot_r += pagecount;
  }

  oz_hw_cpu_setsoftint (si);

  return (sts);
}
