//+++2003-03-01
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
//---2003-03-01

/************************************************************************/
/*									*/
/*  Chain to a new executable						*/
/*									*/
/*    Input:								*/
/*									*/
/*	image    = image name to execute				*/
/*	params   = parameter string to pass to executable		*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_chain = OZ_SUCCESS : successfully started		*/
/*	                     else : error status			*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_knl_status.h"
#include "oz_sys_chain.h"
#include "oz_sys_image.h"
#include "oz_sys_logname.h"
#include "oz_sys_thread.h"

uLong oz_sys_chain (const char *image, int nparams, const char **paramv)

{
  const char *imagecopy;
  int i, l;
  OZ_Logvalue imagelogvalues[1], *paramlogvalues;
  uLong sts;
  uLong (*startaddr) (char *imagename);
  void *baseaddr;

  // Make sure parameters are on the stack because we release all other memory

  l = strlen (image) + 1;
  imagecopy = alloca (l);
  memcpy (imagecopy, image, l);

  if (nparams > 0) {
    paramlogvalues = alloca (nparams * sizeof *paramlogvalues);
    for (i = 0; i < nparams; i ++) {
      l = strlen (paramv[i]) + 1;
      paramlogvalues[i].attr = 0;
      paramlogvalues[i].buff = alloca (l);
      memcpy (paramlogvalues[i].buff, paramv[i], l);
    }
  }

  // Do everything like we exited except don't actually exit
  // All we keep is the kernel and user stacks

  sts = oz_sys_thread_rundown ();
  if (sts != OZ_SUCCESS) return (sts);

  // Create OZ_IMAGE and OZ_PARAMS logical names

  imagelogvalues[0].attr = 0;
  imagelogvalues[0].buff = imagecopy;
  sts = oz_sys_logname_create (h_lognamtbl, "OZ_IMAGE", OZ_PROCMODE_KNL, 0, 1, imagelogvalues, NULL);
  if (sts != OZ_SUCCESS) return (sts);

  if (nvalues <= 0) {
    paramlogvalues = imagelogvalues;
    paramlogvalues[0].attr = 0;
    paramlogvalues[0].buff = "";
    nvalues = 1;
  }
  sts = oz_sys_logname_create (h_lognamtbl, "OZ_PARAMS", OZ_PROCMODE_KNL, 0, nvalues, paramlogvalues, NULL);
  if (sts != OZ_SUCCESS) return (sts);

  // Load the image

  sts = oz_sys_image_load (OZ_PROCMODE_KNL, imagecopy, 0, &baseaddr, &startaddr, &h_image);
  if (sts != OZ_SUCCESS) return (sts);

  // Call its 'startaddr' routine

  sts = (*startaddr) (imagecopy);
  sts = oz_sys_thread_exit (sts);
  return (sts);
}
