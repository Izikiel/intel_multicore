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
/*  Reboot the system after shutting devices down			*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_knl_hw.h"
#include "oz_knl_printk.h"
#include "oz_knl_shuthand.h"
#include "oz_knl_status.h"
#include "oz_sys_callknl.h"
#include "oz_util_start.h"

static char *pn = "shutdown";
static int reboot;

static uLong shutdown (OZ_Procmode cprocmode, void *dummy);

uLong oz_util_main (int argc, char *argv[])

{
  int i;
  uLong sts;

  if (argc > 0) pn = argv[0];
  reboot = 0;
  for (i = 1; i < argc; i ++) {
    if (strcasecmp (argv[i], "-reboot") == 0) {
      reboot = 1;
      continue;
    }
    goto usage;
  }

  sts = oz_sys_callknl (shutdown, NULL);
  oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u shutting down\n", pn, sts);
  return (sts);

usage:
  oz_sys_io_fs_printf (oz_util_h_error, "usage: %s [-reboot]\n", pn);
  return (OZ_BADPARAM);
}

static uLong shutdown (OZ_Procmode cprocmode, void *dummy)

{
  oz_hw_cpu_setsoftint (0);
  oz_knl_shutdown ();
  if (reboot) {
    oz_knl_printk ("%s: rebooting...\n", pn);
    oz_hw_stl_microwait (3000000, NULL, NULL);
    oz_hw_reboot ();
  } else {
    oz_knl_printk ("%s: halting...\n", pn);
    oz_knl_halt (OZ_PROCMODE_KNL, NULL);
  }
  return (OZ_SUCCESS);
}
