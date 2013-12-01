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
/*  Exit handler routines						*/
/*									*/
/************************************************************************/

#include "ozone.h"

#include "oz_knl_status.h"
#include "oz_knl_thread.h"
#include "oz_sys_exhand.h"
#include "oz_sys_syscall.h"

OZ_HW_SYSCALL_DEF_3 (exhand_create, OZ_Procmode, procmode, OZ_Exhand_entry, entry, void *, param)

{
  int si;
  OZ_Exhand *exhand;
  uLong sts;

  if (procmode < cprocmode) procmode = cprocmode;
  si  = oz_hw_cpu_setsoftint (0);
  sts = oz_knl_exhand_create (entry, param, procmode, NULL, &exhand);
  oz_hw_cpu_setsoftint (si);
  return (sts);
}
