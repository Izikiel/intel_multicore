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

/******************************************************/
/* these values must match what is in oz_params_486.s */
/******************************************************/

#define ALTCPUBASE 0x00001000						// base address of alternate cpu startup routine
#define MPDBASE (ALTCPUBASE+0x1000)					// base address of the system common master page directory

#define MAXSYSPAGES (512*1024/4)					// max possible total number of system pages 
									// = 512*17/16Kb of spt/srp (so it all fits below hole) 
									// = 128K pages = 512Mbytes

/* MPD and PT entry protection bits */

#define MPD_BITS 7	// any mode has r/w access to the pages, let the individual entries restrict as needed
#define PT_KRW 3	// only kernel mode can read or write, no user mode access

/* These parameters determine process memory layout (see oz_hw_process_486.c) */

#define PROCMPDVPAGE 0x000FFFFE						// page of the process' own MPD
#define PROCMPDVADDR 0xFFFFE000						// address of the process' own MPD
									// first PROCVIRTBASE/4M entries are exact copy system MPD
									// the rest are pointers to the PROCPTRPBVADDR pages

#define ONEMEG (1024*1024)
#define pages_left_after_maxsyspages ((ONEMEG-MAXSYSPAGES)&(-16384))				// number of pages available after the max system pages
												// ... and make sure it is a multiple of 16k because we 
												// ... group pagetable pages in groups of 16 pages at a time
#define ptrp_bytes_for_pages_left_after_maxsyspages (pages_left_after_maxsyspages*17/16*4)	// this is how many bytes of pagetable we would need for those pages
												// ... leaving room for 1 reqprot page for every 16 pagetable pages
#define PROCPTRPBVADDR (PROCMPDVADDR-ptrp_bytes_for_pages_left_after_maxsyspages)		// so this is the base virtual address of those bytes
												// ... and because pages_left_after_maxsyspages is a multiple of 16k, 
												// ... this is guaranteed to be a multiple of 4k
#define PROCPTRPBVPAGE ((PROCPTRPBVADDR/4096)&0x000FFFFF)		// calculate corresponding page number (mask so it doesn't sign-extend)

#define PROCPTRPPAGES (PROCMPDVPAGE-PROCPTRPBVPAGE)			// number of pages that the process page table and reqprot bits occupy

#define PROCBASVPAGE (ONEMEG-pages_left_after_maxsyspages)		// starting virtual page of a process (0x20000)
#define PROCBASVADDR (PROCBASVPAGE*4096)				// corresponding virtual address (0x20000000)

