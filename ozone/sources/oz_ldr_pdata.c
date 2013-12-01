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
/*  Loader version always uses oz_s_systempdata and non-paged pool	*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_sdata.h"
#include "oz_sys_pdata.h"

OZ_Pdata *oz_sys_pdata_pointer (OZ_Procmode procmode)

{
  return (&oz_s_systempdata);
}

void *oz_sys_pdata_malloc (OZ_Procmode procmode, uLong size)

{
  return (OZ_KNL_NPPMALLOC (size));
}

void oz_sys_pdata_free (OZ_Procmode procmode, void *buff)

{
  OZ_KNL_NPPFREE (buff);
}
