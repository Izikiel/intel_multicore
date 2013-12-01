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

/************************************************************************/
/*									*/
/*  List out the handles of a process					*/
/*									*/
/*	prochandles <processid>						*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_knl_image.h"
#include "oz_knl_process.h"
#include "oz_knl_status.h"
#include "oz_sys_callknl.h"
#include "oz_sys_process.h"
#include "oz_util_start.h"

static OZ_Handle h_process;

static uLong main_knl (OZ_Procmode cprocmode, void *dummy);

uLong oz_util_main (int argc, char *argv[])

{
  OZ_Processid pid;
  uLong sts;

  if (argc != 2) {
    oz_sys_io_fs_printf (oz_util_h_error, "usage: prochandles <processid>\n");
    return (OZ_MISSINGPARAM);
  }

  pid = atoi (argv[1]);
  sts = oz_sys_process_getbyid (pid, &h_process);
  if (sts == OZ_SUCCESS) sts = oz_sys_callknl (main_knl, NULL);
  oz_sys_io_fs_printf (oz_util_h_error, "status: %u\n", sts);
  return (sts);
}

static uLong main_knl (OZ_Procmode cprocmode, void *dummy)

{
  int si;
  OZ_Image *knlimage;
  uLong sts;
  uLong (*knlprochandles) (OZ_Procmode cprocmode, OZ_Handle h_process, OZ_Handle h_output);
  void *baseaddr, *startaddr;

  si = oz_hw_cpu_setsoftint (0);
  sts = oz_knl_image_load (OZ_PROCMODE_KNL, "oz_knl_prochandles.oz", 1, 0, &baseaddr, &startaddr, &knlimage);
  if (sts == OZ_SUCCESS) {
    sts = OZ_BUGCHECK;
    if (startaddr != NULL) {
      knlprochandles = startaddr;
      sts = (*knlprochandles) (cprocmode, h_process, oz_util_h_output);
    }
    oz_knl_image_increfc (knlimage, -1);
  }
  oz_hw_cpu_setsoftint (si);
  return (sts);
}
