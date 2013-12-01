//+++2002-03-18
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
//---2002-03-18

/************************************************************************/
/*									*/
/*  This routine serves as a main program for loadable drivers		*/
/*									*/
/*  Link a driver with this as its main program then load it with 	*/
/*  'kimage load' command						*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_knl_image.h"
#include "oz_knl_procmode.h"
#include "oz_knl_status.h"

uLong _start (int argc, char **argv, OZ_Image *image)

{
  oz_knl_image_lockinmem (image, OZ_PROCMODE_KNL);
  oz_knl_image_increfc (image, 1);
#ifdef PARAMETERS
  ENTRYPOINT (PARAMETERS);
#else
  ENTRYPOINT ();
#endif
  return (OZ_SUCCESS);
}
