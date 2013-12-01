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
/*  Bootblock setup routines for Alpha computers			*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_hw_bootblock.h"
#include "oz_knl_status.h"

/************************************************************************/
/*									*/
/*  Get number of blocks in the boot block and say what logical block 	*/
/*  number it starts on							*/
/*									*/
/*    Input:								*/
/*									*/
/*	*disk_getinfo1 = parameters from OZ_IO_DISK_GETINFO1 call	*/
/*	diskiochan = I/O channel to disk drive				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_hw_bootblock_nblocks = OZ_SUCCESS : successful		*/
/*	                                else : error status		*/
/*	*bb_nblocks = 0 : disk is not a bootable device			*/
/*	           else : number of blocks in the bootblock		*/
/*	*bb_logblock = starting logical block number of bootblock	*/
/*									*/
/************************************************************************/

uLong oz_hw_bootblock_nblocks (OZ_IO_disk_getinfo1 *disk_getinfo1, OZ_Iochan *diskiochan, OZ_Dbn *bb_nblocks, OZ_Dbn *bb_logblock)

{
  *bb_nblocks  = 1;	/* bootblocks are always one block long */
  *bb_logblock = 0;	/* ... and they always are at LBN 0 */
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Modify bootblock contents for the given loader			*/
/*									*/
/*    Input:								*/
/*									*/
/*	*bootblock = current bootblock contents				*/
/*	ldr_nblocks = number of blocks in the loader image		*/
/*	              (not counting its bootblock copy)			*/
/*	ldr_logblock = starting logical block number of loader image	*/
/*	               (past its bootblock copy)			*/
/*	part_logblock = logical block number of partition		*/
/*	*fs_writeboot = parameters from OZ_IO_FS_WRITEBOOT call		*/
/*	*disk_getinfo1 = parameters from OZ_IO_DISK_GETINFO1 call of	*/
/*	                 the logical drive the loader image is on	*/
/*	*host_getinfo1 = parameters from OZ_IO_DISK_GETINFO1 call of 	*/
/*	                 the outermost (physical) drive			*/
/*	diskiochan = I/O channel to disk drive				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_hw_bootblock_modify = OZ_SUCCESS : successful		*/
/*	                               else : error status		*/
/*	*bootblock = modified as needed					*/
/*									*/
/************************************************************************/

uLong oz_hw_bootblock_modify (uByte *bootblock, 
                              OZ_Dbn ldr_nblocks, 
                              OZ_Dbn ldr_logblock, 
                              OZ_Dbn part_logblock, 
                              OZ_IO_fs_writeboot *fs_writeboot, 
                              OZ_IO_disk_getinfo1 *disk_getinfo1, 
                              OZ_IO_disk_getinfo1 *host_getinfo1, 
                              OZ_Iochan *diskiochan)

{
  /* Modify bootblock values and return success status */

  memset (bootblock, 0, 512);					/* most of the block is zeroes */
  *(uQuad *)(bootblock + 480) = ldr_nblocks;			/* number of blocks */
  *(uQuad *)(bootblock + 488) = ldr_logblock + part_logblock;	/* starting lbn on physical disk */
  *(uQuad *)(bootblock + 504) = *(uQuad *)(bootblock + 480) + *(uQuad *)(bootblock + 488); /* checksum */
  return (OZ_SUCCESS);
}
