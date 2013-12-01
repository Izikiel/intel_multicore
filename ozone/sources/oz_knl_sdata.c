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

/************************************************************************/
/*									*/
/*  The data in this module is global to the system			*/
/*									*/
/*  All data is prefixed with oz_s_					*/
/*									*/
/************************************************************************/

#define _OZ_KNL_SDATA_C
#include "ozone.h"
#include "oz_knl_devio.h"
#include "oz_knl_handle.h"
#include "oz_knl_hw.h"
#include "oz_knl_image.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_logname.h"
#include "oz_knl_sdata.h"
#include "oz_knl_status.h"
#include "oz_sys_pdata.h"
#include "oz_sys_syscall.h"

/* Filled in by hardware layer boot routine before calling oz_knl_boot_firstcpu */

globaldef int oz_s_inloader;			/* 0: kernel; 1: loader */
globaldef Long oz_s_cpucount;			/* total number of cpu's on the system */
globaldef volatile uLong oz_s_cpusavail;	/* bitmask of cpu's that are actually booted up */
globaldef OZ_Mempage oz_s_phymem_totalpages;	/* total number of physical memory pages in system */
globaldef void *oz_s_sysmem_baseva;		/* base virtual address of system (page mapped by spte #0) */
globaldef uLong oz_s_sysmem_pagtblsz;		/* total number of entries allocated to system page table */
globaldef uLong oz_s_sysmem_pagtblfr;		/* number of entries in system page table that are available */
globaldef OZ_Loadparams oz_s_loadparams;	/* load parameter block */
globaldef OZ_Datebin oz_s_boottime;		/* date/time system was booted */

/* Physical memory */

globaldef OZ_Mempage oz_s_phymem_freepages;			/* number free of physical memory pages in system */
globaldef OZ_Phymem_page *oz_s_phymem_pages;			/* physical memory pages control array */
globaldef OZ_Mempage oz_s_phymem_l1pages, oz_s_phymem_l2pages;	/* number of pages that fit in L1,L2 caches */
globaldef OZ_Event *oz_s_freephypagevent;			/* event flag set when free pages are available, clear when not */

/* Pool statistics */

globaldef OZ_Memsize oz_s_npptotal = 0;
globaldef OZ_Memsize oz_s_nppinuse = 0;
globaldef OZ_Memsize oz_s_npppeak  = 0;

globaldef OZ_Memsize oz_s_pgptotal = 0;
globaldef OZ_Memsize oz_s_pgpinuse = 0;
globaldef OZ_Memsize oz_s_pgppeak  = 0;

/* System process created at boot time -                      */
/* It contains the system page table as a private section     */
/* And it had threads that were the original cpu boot threads */

globaldef OZ_Process *oz_s_systemproc;

/* System job and user blocks created at boot time used by oz_s_systemproc */

globaldef OZ_User *oz_s_systemuser;
globaldef OZ_Job  *oz_s_systemjob;

/* System logical name table pointer set up at boot time */

globaldef OZ_Logname *oz_s_systemdirectory;
globaldef OZ_Logname *oz_s_systemtable;

/* Device security attributes */

globaldef OZ_Secattr *oz_s_secattr_callknl;
globaldef OZ_Secattr *oz_s_secattr_sysdev;
globaldef OZ_Secattr *oz_s_secattr_syslogname;
globaldef OZ_Secattr *oz_s_secattr_tempdev;

/* Console I/O channel */

globaldef OZ_Iochan *oz_s_coniochan;

/* Shutdown flag: < 0: normal operation; else: shutting down */

globaldef int oz_s_shutdown = -1;

/* Quicky counter, increments at approximately OZ_HW_QUICKYHZ counts per second */

globaldef uLong oz_s_quickies = 0;

/* Kernel image pointer set up at boot time */

globaldef OZ_Image *oz_s_kernelimage;

/* Global SMP locks */

globaldef OZ_Smplock oz_s_smplock_sh;	/* protects shutdown handler database */
globaldef OZ_Smplock oz_s_smplock_dv;	/* protects device driver database */
globaldef OZ_Smplock oz_s_smplock_pm;	/* protects physical memory database */
globaldef OZ_Smplock oz_s_smplock_se;	/* protects security database */
globaldef OZ_Smplock oz_s_smplock_id;	/* protects id number database */
globaldef OZ_Smplock oz_s_smplock_np;	/* non-paged pool allocation */
globaldef OZ_Smplock oz_s_smplock_qu;	/* quota updates */
globaldef OZ_Smplock oz_s_smplock_hi;	/* protects lowipl database */

/* System call table */

globaldef uLong oz_s_syscallmax;
globaldef void *oz_s_syscalltbl[OZ_SYSCALL_MAX];

/* Special logical names - code assumes indices of these arrays */

globaldef const Charp oz_s_logname_directorynames[4] = { "OZ_PROCESS_DIRECTORY", "OZ_JOB_DIRECTORY", "OZ_USER_DIRECTORY", "OZ_SYSTEM_DIRECTORY" };
globaldef const Charp oz_s_logname_defaulttables = "OZ_DEFAULT_TBL";
globaldef const Charp oz_s_logname_deftblnames[5] = { "OZ_PROCESS_TABLE", "OZ_PARENT_TABLE", "OZ_JOB_TABLE", "OZ_USER_TABLE", "OZ_SYSTEM_TABLE" };

/* Security attributes for various things */

globalref OZ_Secattr *oz_s_secattr_sysdev;		/* system devices */
globalref OZ_Secattr *oz_s_secattr_syslogname;		/* system logical names */
globalref OZ_Secattr *oz_s_secattr_tempdev;		/* template devices */

/* Internal routines */

static OZ_Secattr *cresecattr (const char *str);
static void *secattrmalloc (void *dummy, uLong osize, void *obuff, uLong nsize);

/* Initialize data */

void oz_knl_sdata_init1 ()

{
  oz_s_cpusavail       = 0;
  oz_s_phymem_pages    = NULL;
  oz_s_freephypagevent = NULL;
  oz_s_systemproc      = NULL;
  oz_s_systemtable     = NULL;
  oz_s_shutdown        = -1;

  memset (&oz_s_systempdata, 0, sizeof oz_s_systempdata);
  oz_s_systempdata.rwpageprot = OZ_HW_PAGEPROT_KW;	// the data are read/write by kernel mode only
  oz_s_systempdata.procmode   = OZ_PROCMODE_KNL;	// the data are owned by kernel mode
  oz_s_systempdata.pprocmode  = OZ_PROCMODE_SYS;	// pass this value to oz_sys_pdata_... routines to get to it

  oz_hw_smplock_init (sizeof oz_s_smplock_sh, &oz_s_smplock_sh, OZ_SMPLOCK_LEVEL_SH);
  oz_hw_smplock_init (sizeof oz_s_smplock_dv, &oz_s_smplock_dv, OZ_SMPLOCK_LEVEL_DV);
  oz_hw_smplock_init (sizeof oz_s_smplock_pm, &oz_s_smplock_pm, OZ_SMPLOCK_LEVEL_PM);
  oz_hw_smplock_init (sizeof oz_s_smplock_se, &oz_s_smplock_se, OZ_SMPLOCK_LEVEL_SE);
  oz_hw_smplock_init (sizeof oz_s_smplock_id, &oz_s_smplock_id, OZ_SMPLOCK_LEVEL_ID);
  oz_hw_smplock_init (sizeof oz_s_smplock_np, &oz_s_smplock_np, OZ_SMPLOCK_LEVEL_NP);
  oz_hw_smplock_init (sizeof oz_s_smplock_qu, &oz_s_smplock_qu, OZ_SMPLOCK_LEVEL_QU);
  oz_hw_smplock_init (sizeof oz_s_smplock_hi, &oz_s_smplock_hi, OZ_SMPLOCK_LEVEL_HI);
}

void oz_knl_sdata_init2 ()

{
  oz_s_secattr_callknl    = cresecattr ("0=write");				/*   oz_sys_callknl: must have seckey 0 */
  oz_s_secattr_sysdev     = cresecattr ("1=look+read+write,0&0=look");		/*   system devices: seckey 1 has full access, everyone else can just look */
  oz_s_secattr_syslogname = cresecattr ("1=look+read+write,0&0=look+read");	/*  system lognames: seckey 1 has full access, everyone else can look and read */
  oz_s_secattr_tempdev    = cresecattr ("0&0=look+read+write");			/* template devices: everyone has full access */
}

static OZ_Secattr *cresecattr (const char *str)

{
  OZ_Secattr *secattr;
  uLong secattrsize, sts;
  void *secattrbuff;

  sts = oz_sys_secattr_str2bin (strlen (str), str, NULL, secattrmalloc, NULL, &secattrsize, &secattrbuff);
  if (sts != OZ_SUCCESS) oz_crash ("oz_knl_sdata_init: error %u parsing secattr %s", sts, str);
  sts = oz_knl_secattr_create (secattrsize, secattrbuff, NULL, &secattr);
  if (sts != OZ_SUCCESS) oz_crash ("oz_knl_sdata_init: error %u creating secattr %s", sts, str);
  OZ_KNL_NPPFREE (secattrbuff);
  return (secattr);
}

static void *secattrmalloc (void *dummy, uLong osize, void *obuff, uLong nsize)

{
  void *nbuff;

  nbuff = NULL;
  if (nsize != 0) {
    nbuff = OZ_KNL_NPPMALLOC (nsize);
    memcpy (nbuff, obuff, osize);
  }
  if (obuff != NULL) OZ_KNL_NPPFREE (obuff);
  return (nbuff);
}
