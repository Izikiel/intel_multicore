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
#include "oz_knl_hw.h"
#include "oz_knl_misc.h"
#include "oz_knl_printk.h"
#include "oz_knl_sdata.h"
#include "oz_knl_section.h"

/************************************************************************/
/*									*/
/*  Get 'extra' parameter value						*/
/*									*/
/*    Input:								*/
/*									*/
/*	name = name of parameter to get					*/
/*	dflt = default string if not found				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_misc_getextra = points to resultant string		*/
/*									*/
/************************************************************************/

const char *oz_knl_misc_getextra (const char *name, const char *dflt)

{
  int i, j, s;


  s = strlen (name);
  for (i = 0; i < sizeof oz_s_loadparams.extras; i += j) {
    j = strlen (oz_s_loadparams.extras + i) + 1;
    if ((oz_s_loadparams.extras[i+s] == '=') && (strncasecmp (oz_s_loadparams.extras + i, name, s) == 0)) {
      return (oz_s_loadparams.extras + i + s + 1);
    }
  }
  return (dflt);
}

/************************************************************************/
/*									*/
/*  Convert system virtual address to physical address			*/
/*									*/
/*    Input:								*/
/*									*/
/*	addr = system virtual address to convert			*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_misc_sva2pa = number of bytes available within the page	*/
/*	*ppn = physical page number					*/
/*	*ppo = byte offset within the physical page			*/
/*									*/
/************************************************************************/

uLong oz_knl_misc_sva2pa (void *addr, OZ_Mempage *ppn, uLong *ppo)

{
  OZ_Mempage vpage;
  OZ_Section_pagestate pagestate;
  uLong offs;

  vpage = OZ_HW_VADDRTOVPAGE (addr);

  oz_hw_pte_readsys (vpage, &pagestate, ppn, NULL, NULL);
  if ((pagestate != OZ_SECTION_PAGESTATE_WRITEINPROG) 
   && (pagestate != OZ_SECTION_PAGESTATE_VALID_R) 
   && (pagestate != OZ_SECTION_PAGESTATE_VALID_W) 
   && (pagestate != OZ_SECTION_PAGESTATE_VALID_D)) {
    oz_crash ("oz_knl_misc_sva2pa: %p pagestate %d", addr, pagestate);
  }

  offs = OZ_HW_VADDRTOPOFFS (addr);
  if (ppo != NULL) *ppo = offs;

  return ((1 << OZ_HW_L2PAGESIZE) - offs);
}

/* Checks out length as long as pages are physically contiguous (up to length = max) */

uLong oz_knl_misc_sva2pax (void *addr, OZ_Mempage *ppn, uLong *ppo, uLong max)

{
  OZ_Mempage ppage, ppagex, spage, vpage;
  OZ_Pointer len, ofs;
  OZ_Section_pagestate pagestate;

  vpage = OZ_HW_VADDRTOVPAGE (addr);							// get beginning virtual page
  spage = OZ_HW_VADDRTOVPAGE (oz_s_sysmem_baseva);					// get base system page number
  if (vpage < spage) goto badva;							// if not a system page, barf
  spage = vpage - spage;								// get system page number
  if ((oz_s_sysmem_pagtblsz > 0) && (spage >= oz_s_sysmem_pagtblsz)) goto badva;	// if off of end of table, barf

  oz_hw_pte_readsys (vpage, &pagestate, &ppage, NULL, NULL);				// read system pagetable entry
  if ((pagestate != OZ_SECTION_PAGESTATE_WRITEINPROG) 					// make sure it's valid
   && (pagestate != OZ_SECTION_PAGESTATE_VALID_R) 
   && (pagestate != OZ_SECTION_PAGESTATE_VALID_W) 
   && (pagestate != OZ_SECTION_PAGESTATE_VALID_D)) return (0);				// if not, nothing is there

  ofs = OZ_HW_VADDRTOPOFFS (addr);							// get offset in the first page
  if (ppn != NULL) *ppn = ppage;							// return first phypage number
  if (ppo != NULL) *ppo = ofs;								// return offset in first page

  len = (1 << OZ_HW_L2PAGESIZE) - ofs;							// get length remaining in first page
  while (len < max) {
    if ((oz_s_sysmem_pagtblsz > 0) && (++ spage >= oz_s_sysmem_pagtblsz)) break;	// if off of end of table, done
    oz_hw_pte_readsys (++ vpage, &pagestate, &ppagex, NULL, NULL);			// read next system pagetable entry
    if ((pagestate != OZ_SECTION_PAGESTATE_WRITEINPROG) 				// stop if it's not valid
     && (pagestate != OZ_SECTION_PAGESTATE_VALID_R) 
     && (pagestate != OZ_SECTION_PAGESTATE_VALID_W) 
     && (pagestate != OZ_SECTION_PAGESTATE_VALID_D)) break;
    if (ppagex != ++ ppage) break;							// stop if not physically contiguous
    len += (1 << OZ_HW_L2PAGESIZE);							// ok, increment contig len by a page
  }
  if (len > max) len = max;								// never return more than maximum length
  return (len);

badva:
  oz_crash ("oz_knl_misc_sva2pax: %p not system address, sysbaseva %p, spages %u", addr, oz_s_sysmem_baseva, oz_s_sysmem_pagtblsz);
  return (0);
}


/* Print prompt then pause for <enter> key, crash if <eof> */

void oz_hw_pause (const char *prompt)

{
  char buff[4];

  if (!oz_hw_getcon (sizeof buff, buff, strlen (prompt), prompt)) oz_crash ("oz_hw_pause: eof");
}

/* Print a message to the console and crash */

void oz_crash (const char *fmt, ...)

{
  va_list ap;

  va_start (ap, fmt);
  oz_knl_printkv (fmt, ap);
  va_end (ap);
  oz_knl_printk ("\n");
  oz_hw_crash ();
}

/* Print a message to the console with args inline and pause */

void oz_knl_printkp (const char *fmt, ...)

{
  va_list ap;

  va_start (ap, fmt);
  oz_knl_printkv (fmt, ap);
  va_end (ap);
  oz_hw_pause ("> ");
}
