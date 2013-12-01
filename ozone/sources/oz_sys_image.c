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

#include "oz_sys_syscall.h"

#include "oz_sys_image.h"

#include "oz_knl_image.h"
#include "oz_knl_handle.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_procmode.h"
#include "oz_knl_status.h"

#define MAXIMAGENAMELEN (4096)

/************************************************************************/
/*									*/
/*  Load an image							*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_6 (image_load, OZ_Procmode, procmode, char *, imagename, int, sysimage, void **, baseaddr_r, void **, startaddr_r, OZ_Handle *, h_image_r)

{
  int si;
  uLong sts;
  OZ_Image *image;
  OZ_Seclock *seclock_baseaddr_r, *seclock_h_image_r, *seclock_imagename, *seclock_startaddr_r;

  if (procmode < cprocmode) procmode = cprocmode;				/* maximise processor mode */

  seclock_h_image_r   = NULL;
  seclock_startaddr_r = NULL;
  seclock_baseaddr_r  = NULL;
  seclock_imagename   = NULL;

  si = oz_hw_cpu_setsoftint (0);						/* keep thread from being deleted */
  sts = oz_knl_section_iolockw (cprocmode, sizeof *h_image_r, h_image_r, &seclock_h_image_r, NULL, NULL, NULL);
  if (sts == OZ_SUCCESS) sts = oz_knl_section_iolockw (cprocmode, sizeof *startaddr_r, startaddr_r, &seclock_startaddr_r, NULL, NULL, NULL);
  if (sts == OZ_SUCCESS) sts = oz_knl_section_iolockw (cprocmode, sizeof *baseaddr_r, baseaddr_r, &seclock_baseaddr_r, NULL, NULL, NULL);
  if (sts == OZ_SUCCESS) sts = oz_knl_section_iolockz (cprocmode, MAXIMAGENAMELEN, imagename, NULL, &seclock_imagename, NULL, NULL, NULL);
  if (sts == OZ_SUCCESS) sts = oz_knl_image_load (procmode, imagename, sysimage, 0, baseaddr_r, startaddr_r, &image);
  if (sts == OZ_SUCCESS) {
    sts = oz_knl_handle_assign (image, procmode, h_image_r);			/* assign an handle to it */
    oz_knl_image_increfc (image, -1);						/* release image pointer */
  }
  if (seclock_h_image_r   != NULL) oz_knl_section_iounlk (seclock_h_image_r);
  if (seclock_startaddr_r != NULL) oz_knl_section_iounlk (seclock_startaddr_r);
  if (seclock_baseaddr_r  != NULL) oz_knl_section_iounlk (seclock_baseaddr_r);
  if (seclock_imagename   != NULL) oz_knl_section_iounlk (seclock_imagename);
  oz_hw_cpu_setsoftint (si);							/* restore software interrupt enable */
  return (sts);									/* return composite status */
}
