//+++2002-08-17
//    Copyright (C) 2001,2002  Mike Rieker, Beverly, MA USA
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
//---2002-08-17

/************************************************************************/
/*									*/
/*  Phymem replacement routines (for cache use only)			*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_knl_cache.h"
#include "oz_knl_event.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_phymem.h"
#include "oz_knl_printk.h"
#include "oz_knl_sdata.h"
#include "oz_knl_status.h"

static uLong      phymem_count = 0;
static OZ_Mempage phymem_page0 = OZ_PHYPAGE_NULL;

void oz_ldr_phymem_init (uLong cacheareasize, OZ_Mempage cacheareabase)

{
  uLong i;

  phymem_count = cacheareasize;
  phymem_page0 = cacheareabase;
  oz_s_phymem_totalpages = 0;
  oz_s_phymem_pages = NULL;
  if (phymem_count != 0) {
    oz_knl_event_create (20, "oz_s_freephypagevent", NULL, &oz_s_freephypagevent);
    oz_s_phymem_pages  = OZ_KNL_NPPMALLOC (cacheareasize * sizeof *oz_s_phymem_pages);
    memset (oz_s_phymem_pages, 0, cacheareasize * sizeof *oz_s_phymem_pages);
    for (i = 0; i < cacheareasize; i ++) oz_s_phymem_pages[i].state = OZ_PHYMEM_PAGESTATE_FREE;
    oz_s_phymem_totalpages = cacheareasize + cacheareabase;
    oz_s_phymem_pages -= cacheareabase;
  }
}

OZ_Mempage oz_knl_phymem_allocpage (OZ_Phymem_pagestate pagestate, OZ_Mempage virtpage)

{
  OZ_Mempage i;

  for (i = phymem_page0; i < phymem_page0 + phymem_count; i ++) {
    if (oz_s_phymem_pages[i].state == OZ_PHYMEM_PAGESTATE_FREE) {
      oz_s_phymem_pages[i].state = pagestate;
      return (i);
    }
  }
  return (oz_knl_cache_freepage (virtpage & (oz_s_phymem_l2pages - 1)));
}

OZ_Mempage oz_knl_phymem_allocontig (OZ_Mempage count, OZ_Phymem_pagestate pagestate, OZ_Mempage virtpage)

{
  OZ_Mempage p0, p1;

  for (p0 = phymem_page0; p0 < phymem_page0 + phymem_count; p0 ++) {
    if (oz_s_phymem_pages[p0].state == OZ_PHYMEM_PAGESTATE_FREE) {
      for (p1 = p0; (p1 < phymem_page0 + phymem_count) && (p1 < p0 + count); p1 ++) {
        if (oz_s_phymem_pages[p1].state != OZ_PHYMEM_PAGESTATE_FREE) break;
      }
      if (p1 - p0 >= count) {
        for (p1 = p0; p1 < p0 + count; p1 ++) {
          oz_s_phymem_pages[p1].state = pagestate;
        }
        return (p0);
      }
    }
  }

  return (OZ_PHYPAGE_NULL);
}

void oz_knl_phymem_freepage (OZ_Mempage i)

{
  if ((i < phymem_page0) || (i >= phymem_page0 + phymem_count)) {
    oz_crash ("oz_knl_phymem_freepage: mempage %u out of range %u..%u", i, phymem_page0, phymem_page0 + phymem_count - 1);
  }
  if (oz_s_phymem_pages[i].state == OZ_PHYMEM_PAGESTATE_FREE) {
    oz_crash ("oz_knl_phymem_freepage: mempage %u already marked free", i);
  }
  oz_s_phymem_pages[i].state = OZ_PHYMEM_PAGESTATE_FREE;
}
