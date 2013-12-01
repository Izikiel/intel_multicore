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
/*  Routine to scan the devices in search of the boot volume		*/
/*									*/
/*    Input:								*/
/*									*/
/*	oz_hw486_bootdevtype  = 'CD', 'FD', 'HD'			*/
/*	oz_hw486_bootparamlbn = lbn of block that contains loader param page
/*	oz_hw486_bootparams   = original loader param page loaded by boot block
/*	oz_ldr_paramblock.load_device = assumed to be null string	*/
/*	oz_ldr_paramblock.load_fstemp = must be filled in with boot vol's fs template device
/*									*/
/*	*abortflag = gets set to 1 externally to abort the scan		*/
/*	abortevent = gets set externally when *abortflag gets set	*/
/*	verbose    = print debug info					*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_hw_bootscan = 0 : failed to find boot volume			*/
/*	                 1 : boot vol found, oz_ldr_paramblock.load_device filled in
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_dev_dpar.h"
#include "oz_dev_scsi.h"
#include "oz_io_disk.h"
#include "oz_io_fs.h"
#include "oz_io_scsi.h"
#include "oz_knl_devio.h"
#include "oz_knl_event.h"
#include "oz_knl_hw.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_sdata.h"
#include "oz_knl_status.h"
#include "oz_ldr_params.h"


int oz_hw_bootscan (volatile int *abortflag, OZ_Event *abortevent, int verbose)

{
  return (0);
}
