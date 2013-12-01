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
/*  Print a kernel address as an offset to an image:symbol		*/
/*									*/
/************************************************************************/

#include <stdarg.h>

#include "ozone.h"
#include "oz_knl_hw.h"
#include "oz_knl_printk.h"
#include "oz_knl_sdata.h"
#include "oz_knl_status.h"
#include "oz_sys_io_fs.h"
#include "oz_sys_io_fs_printf.h"
#include "oz_sys_xprintf.h"

void oz_knl_printkaddr (void *addr)

{
  char *loname, *newname;
  const char *dotinname, *imagename;
  int l;
  OZ_Image *image;
  OZ_Image_Secload *imagesecload;
  OZ_Mempage vpage;
  OZ_Pointer loaddr, newaddr, xaddr;
  void *sym;

  xaddr  = (OZ_Pointer)addr;								// get address to print
  vpage  = OZ_HW_VADDRTOVPAGE (addr);							// get its page number
  loname = NULL;									// haven't found it yet
  loaddr = 0;
  if (oz_s_systemproc != NULL) {							// see if system process even set up yet
    for (image = NULL; (image = oz_knl_image_next (image, 1)) != NULL;) {		// ok, scan thru the loaded images
      for (imagesecload = oz_knl_image_secloads (image); imagesecload != NULL; imagesecload = imagesecload -> next) {
        if (vpage < imagesecload -> svpage) continue;
        if (vpage - imagesecload -> svpage < imagesecload -> npages) break;
      }
      if (imagesecload == NULL) continue;						// it has to be in one of its sections
      for (sym = NULL; (sym = oz_knl_image_symscan (image, sym, &newname, &newaddr)) != NULL;) { // scan its symbols
        if (newaddr > xaddr) continue;							// we cant come before it
        if ((loname == NULL) || (newaddr > loaddr)) {					// see if it's better than we have
          loname = newname;								// ok, save it
          loaddr = newaddr;
        }
      }
      break;										// this is the right image, so stop scanning
    }
  }
  if (loname == NULL) oz_knl_printk ("%p", addr);					// not found at all, print absolute value
  else if (image == oz_s_kernelimage) oz_knl_printk ("%s+%p", loname, xaddr - loaddr);	// kernel image, print symbol name and offset
  else {
    imagename = oz_knl_image_name (image);						// other image, print image name, symbol name and offset
    dotinname = strchr (imagename, '.');
    if (dotinname == NULL) l = strlen (imagename);
    else l = dotinname - imagename;
    oz_knl_printk ("%*.*s:%s+%p", l, l, imagename, loname, xaddr - loaddr);
  }
}
