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
/*  Crash dump writing routines						*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_io_fs.h"
#include "oz_knl_crash.h"
#include "oz_knl_devio.h"
#include "oz_knl_hw.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_misc.h"
#include "oz_knl_sdata.h"
#include "oz_knl_section.h"
#include "oz_knl_status.h"

static OZ_Iochan *crash_iochan = NULL;		/* i/o channel to filesystem with crash file open */
static OZ_IO_fs_crash fs_crash;			/* filesystem crash writing routine & parameters */
static uLong crashblocksiz = 0;			/* crashblockbuf size (in bytes) (a multiple of block size) */
static OZ_Crash_block *crashblockbuf = NULL;	/* crash header block buffer (physically contiguous) */
static OZ_Mempage crashblockppn;		/* crashblockbuf starting physical page number */
static uLong crashblockppo;			/* crashblockbuf offset in first physical page */

/************************************************************************/
/*									*/
/*  Set up the file to have crash dump written to it			*/
/*									*/
/*  This routine is called during normal system operation (usually 	*/
/*  during startup).							*/
/*									*/
/*    Input:								*/
/*									*/
/*	iochan = i/o channel with crash dump file open on it		*/
/*	smplevel = softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	the file is set up to have crash dump written to it		*/
/*									*/
/************************************************************************/

uLong oz_knl_crash_set (OZ_Iochan *iochan)

{
  OZ_Pointer beg, end;
  OZ_Section_pagestate pagestate;
  uLong size, sts;

  /* Get rid of any existing crash file */

  if (crash_iochan != NULL) {
    oz_knl_io (crash_iochan, OZ_IO_FS_CRASH, 0, NULL);	/* tell all drivers to forget about it */
    oz_knl_iochan_increfc (crash_iochan, -1);		/* close the channel out */
    OZ_KNL_NPPFREE (crashblockbuf);			/* free off the block buffer */
    crash_iochan  = NULL;				/* clear pointers so we don't try to use them */
    crashblockbuf = NULL;
    memset (&fs_crash, 0, sizeof fs_crash);
  }

  /* Tell device drivers to set themselves up to process crash dump should it be necessary */
  /* Also retrieve the entrypoint of the filesystem's crash dump routine                   */

  memset (&fs_crash, 0, sizeof fs_crash);
  sts = oz_knl_io (iochan, OZ_IO_FS_CRASH, sizeof fs_crash, &fs_crash);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_knl_crash_set: error %u setting crash file system I/O channel\n", sts);
    return (sts);
  }

  /* Determine size to allocate for crash header block                         */
  /* Must be at least big enough for the struct and rounded up to a block size */

  size = fs_crash.blocksize;
  if (size < sizeof *crashblockbuf) {
    size = (sizeof *crashblockbuf + fs_crash.blocksize - 1) / fs_crash.blocksize * fs_crash.blocksize;
  }

  /* Allocate a physically contiguous buffer for it */

  crashblocksiz = size;
  crashblockbuf = OZ_KNL_PCMALLOC (size);

  /* Get physical page number and physical page offset */

  if (oz_knl_misc_sva2pa (crashblockbuf, &crashblockppn, &crashblockppo) < size) {
    oz_crash ("oz_knl_crash_set: buf %p size %u not physically contiguous", crashblockbuf, size);
  }

  /* Save the I/O channel that we set up so we can take it all down if the crashdump file gets changed */

  oz_knl_iochan_increfc (iochan, 1);
  crash_iochan = iochan;

  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Write crash dump file						*/
/*									*/
/*  This routine is called when the system has actually crashed		*/
/*									*/
/*    Input:								*/
/*									*/
/*	cpuidx  = cpu index that's crashing (this cpu)			*/
/*	sigargs = its crash signal arguments				*/
/*	*mchargs = crash machine arguments				*/
/*	*mchargx_knl = crash kernel machine argument extensions		*/
/*	smplevel = hardware interrupts disabled				*/
/*	           software interrupts disabled				*/
/*	           all other cpus halted				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_crash_dump = 0 : write failed				*/
/*	                    1 : successful				*/
/*	crash dump header & trailer written to crash dump file		*/
/*	all physical memory pages written to crash dump file		*/
/*									*/
/************************************************************************/

int oz_knl_crash_dump (Long cpuidx, OZ_Sigargs *sigargs, OZ_Mchargs **mchargs, OZ_Mchargx_knl **mchargx_knl)

{
  OZ_Dbn vbn;
  OZ_Mempage mempage;
  uLong sts;

  /* Make sure crash file defined */

  if (crash_iochan == NULL) {
    oz_knl_printk ("oz_knl_crash_dump: no crash file defined\n");
    return (0);
  }

  /* Write initial header block to crash file */

  oz_knl_printk ("oz_knl_crash_dump: writing crash header ...\n");
  memset (crashblockbuf, 0, crashblocksiz);
  memcpy (crashblockbuf -> magic, "oz_crash", sizeof crashblockbuf -> magic);
  crashblockbuf -> version         = 1;
  crashblockbuf -> when            = oz_hw_tod_getnow ();
  crashblockbuf -> headersize      = crashblocksiz;
  crashblockbuf -> blocksize       = fs_crash.blocksize;
  crashblockbuf -> l2pagesize      = OZ_HW_L2PAGESIZE;
  crashblockbuf -> filesize        = fs_crash.filesize;
  crashblockbuf -> cpuidx          = cpuidx;
  crashblockbuf -> sigargs         = sigargs;
  crashblockbuf -> mchargs         = mchargs;
  crashblockbuf -> mchargx_knl     = mchargx_knl;
  crashblockbuf -> holebeg         = OZ_HW_CRASH_HOLEBEG;
  crashblockbuf -> holeend         = OZ_HW_CRASH_HOLEEND;
  crashblockbuf -> numpages        = oz_s_phymem_totalpages;
  crashblockbuf -> mchargs_cpy     = *(mchargs[cpuidx]);
  crashblockbuf -> mchargx_knl_cpy = *(mchargx_knl[cpuidx]);
  sts = (*(fs_crash.crashentry)) (fs_crash.crashparam, 1, crashblocksiz, crashblockppn, crashblockppo);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_knl_crash_dump: error %u writing crash dump header\n", sts);
    return (0);
  }

  /* Write physical memory pages to crash file */

  oz_knl_printk ("oz_knl_crash_dump: writing physical memory pages ...\n");
  vbn = crashblocksiz / fs_crash.blocksize + 1;
  for (mempage = 0; mempage < oz_s_phymem_totalpages; mempage ++) {
    oz_knl_printk ("\r%8.8X/%8.8X", mempage, oz_s_phymem_totalpages);
    if ((mempage >= OZ_HW_CRASH_HOLEBEG) && (mempage < OZ_HW_CRASH_HOLEEND)) continue;
    sts = (*(fs_crash.crashentry)) (fs_crash.crashparam, vbn, 1 << OZ_HW_L2PAGESIZE, mempage, 0);
    if (sts != OZ_SUCCESS) {
      oz_knl_printk ("\noz_knl_crash_dump: error %u writing physical page 0x%x at vbn %u\n", sts, mempage, vbn);
      return (0);
    }
    vbn += (1 << OZ_HW_L2PAGESIZE) / fs_crash.blocksize;
  }
  oz_knl_printk (" done!\n");

  /* Write trailer block to crash dump file - same as the header but at the end */
  /* This way the analiser knows we wrote the whole thing                       */

  oz_knl_printk ("oz_knl_crash_dump: writing crash dump trailer ...\n");
  sts = (*(fs_crash.crashentry)) (fs_crash.crashparam, vbn, crashblocksiz, crashblockppn, crashblockppo);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_knl_crash_dump: error %u writing crash dump trailer\n", sts);
    return (0);
  }

  /* That's it */

  oz_knl_printk ("oz_knl_crash_dump: crash dump complete\n");
  return (1);
}
