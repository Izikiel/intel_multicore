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
/*  Call an arbitrary routine in kernel mode				*/
/*									*/
/*    Input:								*/
/*									*/
/*	entry = routine entrypoint					*/
/*	param = routine parameter					*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_callknl = status as returned by routine			*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_knl_hw.h"
#include "oz_knl_sdata.h"
#include "oz_knl_status.h"
#include "oz_sys_callknl.h"
#include "oz_sys_syscall.h"

OZ_HW_SYSCALL_DEF_2 (callknl, OZ_Callknl_entry, entry, void *, param)

{
  int si;
  OZ_Seckeys *seckeys;
  uLong sts;

  si = oz_hw_cpu_setsoftint (0);
  seckeys = oz_knl_thread_getseckeys (NULL);
  sts = oz_knl_security_check (OZ_SECACCMSK_WRITE, seckeys, oz_s_secattr_callknl);
  oz_knl_seckeys_increfc (seckeys, -1);
  oz_hw_cpu_setsoftint (si);
  if (sts == OZ_SUCCESS) sts = (*entry) (cprocmode, param);
  return (sts);
}
