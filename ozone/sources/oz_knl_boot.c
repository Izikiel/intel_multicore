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
/*  These routines are called at boot time.  There is one routine for 	*/
/*  the first cpu, and another routine for all the others.		*/
/*									*/
/************************************************************************/

#define _OZ_KNL_BOOT_C

#include "ozone.h"

#include "oz_knl_boot.h"
#include "oz_knl_debug.h"
#include "oz_knl_devio.h"
#include "oz_knl_event.h"
#include "oz_knl_handle.h"
#include "oz_knl_hw.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_logname.h"
#include "oz_knl_printk.h"
#include "oz_knl_process.h"
#include "oz_knl_sdata.h"
#include "oz_knl_section.h"
#include "oz_knl_startup.h"
#include "oz_knl_status.h"
#include "oz_knl_userjob.h"

#include "oz_io_console.h"
#include "oz_io_fs.h"

#define STARTUP_THREAD_PRIORITY 1000000

#define OZ_KNL_BOOT_NPPSIZE (oz_s_loadparams.nonpaged_pool_size)

static void makesection (OZ_Mempage numpages, OZ_Mempage virtpage, OZ_Hw_pageprot pageprot, const char *ppstr);
static void crelogname (OZ_Logname *lognamtbl, uLong logvalatr, char *name, char *value);

/************************************************************************/
/*									*/
/*  This routine is called at boot time by the first cpu		*/
/*									*/
/*  The cpu should have already set up its initial system page table, 	*/
/*  and turned on virtual memory mapping.				*/
/*									*/
/*  Firstfreephypage should have left room for any memory that the 	*/
/*  operating system should not mess with.				*/
/*									*/
/*  After it returns, the caller can either wait forever using 		*/
/*  wait-for-interrupt instructions or call oz_knl_thread_exit.		*/
/*									*/
/*    Input:								*/
/*									*/
/*	smplocks = none							*/
/*	softints = inhibited						*/
/*									*/
/************************************************************************/

void oz_knl_boot_firstcpu (OZ_Mempage ffvirtpage, OZ_Mempage ffphyspage)

{
  const char *ppstr;
  Long i;
  OZ_Logname *defaulttables, *jobtable;
  OZ_Logvalue logvalues[5];
  OZ_Mempage numpages, pagenum, pageoffs, phypage, sysbasevp, virtpage;
  OZ_Hw_pageprot pageprot, pageprotx;
  OZ_Procmode procmode;
  OZ_Section *section;
  OZ_Section_pagestate pagestate;
  OZ_Thread *thread;
  uLong mapsecflags, sts;

  /* Initialize static data, including static smp locks */

  oz_knl_syscall_init ();
  oz_knl_sdata_init1 ();

  /* Print message */

  oz_knl_printk ("%s\n", oz_sys_copyright);
  oz_knl_printk ("oz_knl_boot_firstcpu:\n");
  oz_knl_printk ("            total number of cpus: %d\n", oz_s_cpucount);
  oz_knl_printk ("            page size (in bytes): %u\n", 1 << OZ_HW_L2PAGESIZE);
  oz_knl_printk ("  total pages of physical memory: 0x%X (%u Megabytes)\n", oz_s_phymem_totalpages, oz_s_phymem_totalpages >> (20 - OZ_HW_L2PAGESIZE));
  oz_knl_printk ("     system base virtual address: %p\n", oz_s_sysmem_baseva);
  oz_knl_printk ("       system page table entries: 0x%X (%u Megabytes)\n", oz_s_sysmem_pagtblsz, oz_s_sysmem_pagtblsz >> (20 - OZ_HW_L2PAGESIZE));
  oz_knl_printk ("     initial non-paged pool size: 0x%X (%u Kilobytes)\n", OZ_KNL_BOOT_NPPSIZE, OZ_KNL_BOOT_NPPSIZE >> 10);
  oz_knl_printk ("            first free virt page: 0x%X\n", ffvirtpage);
  oz_knl_printk ("            first free phys page: 0x%X\n", ffphyspage);
  oz_knl_printk ("\n");

  if (oz_s_cpucount > OZ_HW_MAXCPUS) {
    oz_crash ("oz_knl_boot_firstcpu: oz_s_cpucount %u .gt. OZ HW MAXCPUS %u", oz_s_cpucount, OZ_HW_MAXCPUS);
  }

  /* Initialize debugger */

  oz_knl_debug_init ();

  /* Initialize physical memory and non-paged pool */

  oz_knl_printk ("oz_knl_boot_firstcpu: initializing physical memory\n");
  oz_knl_phymem_init (OZ_KNL_BOOT_NPPSIZE, ffvirtpage, ffphyspage);

  /* Now init data that uses npp */

  oz_knl_sdata_init2 ();

  /* Call the various other module initialization routines */

  oz_knl_printk ("oz_knl_boot_firstcpu: initializing modules\n");
  oz_knl_event_init ();
  oz_knl_handle_init ();
  oz_knl_idno_init ();
  oz_knl_process_init ();
  oz_knl_quota_init ();
  oz_knl_section_init ();
  oz_knl_thread_init ();
  oz_knl_userjob_init ();

  /* Create a system process and section to manage pageable system memory                  */
  /* Also turn us into a thread of that process, which makes that process current, which   */
  /* is important because some of the mapping routines only operate on the current process */

  /* (Any use of paged pool here will be re-directed to non-paged pool until oz_s_systemproc gets set) */
  /* (The paged-pool free routine is too st00pid to know how to free this memory so it can't be freed) */
  /* (Having object pointers to everything created will keep it from being freed)                      */

  oz_knl_printk ("oz_knl_boot_firstcpu: creating system process\n");

  sts = oz_knl_logname_create (NULL, OZ_PROCMODE_KNL, NULL, oz_s_secattr_syslogname, OZ_LOGNAMATR_TABLE, strlen (oz_s_logname_directorynames[3]), oz_s_logname_directorynames[3], 0, NULL, &oz_s_systemdirectory);
  if (sts != OZ_SUCCESS) oz_crash ("oz_knl_boot: error %u creating system logical name directory", sts);

  sts = oz_knl_logname_create (oz_s_systemdirectory, OZ_PROCMODE_KNL, NULL, oz_s_secattr_syslogname, OZ_LOGNAMATR_TABLE, strlen (oz_s_logname_deftblnames[4]), oz_s_logname_deftblnames[4], 0, NULL, &oz_s_systemtable);
  if (sts != OZ_SUCCESS) oz_crash ("oz_knl_boot: error %u creating system logical name table", sts);

  for (i = 0; i < 5; i ++) {
    logvalues[i].attr = 0;
    logvalues[i].buff = oz_s_logname_deftblnames[i];
  }
  sts = oz_knl_logname_create (oz_s_systemdirectory, OZ_PROCMODE_KNL, NULL, oz_s_secattr_syslogname, 0, strlen (oz_s_logname_defaulttables), oz_s_logname_defaulttables, 5, logvalues, &defaulttables);
  if (sts != OZ_SUCCESS) oz_crash ("oz_knl_boot: error %u creating default table logical", sts);	/* (throw-away pointer defaulttables makes sure it won't ever get freed off) */

  sts = oz_knl_user_create ("OZ_Startup", NULL, NULL, STARTUP_THREAD_PRIORITY, &oz_s_systemuser);	/* "log in" user OZ_Startup */
  if (sts != OZ_SUCCESS) oz_crash ("oz_knl_boot: error %u creating system user block", sts);
  sts = oz_knl_job_create (oz_s_systemuser, "OZ_Startup", &oz_s_systemjob);				/* create a system job block */
  if (sts != OZ_SUCCESS) oz_crash ("oz_knl_boot: error %u creating system job block", sts);
  sts = oz_knl_process_create (oz_s_systemjob, 1, 0, 6, "system", NULL, &oz_s_systemproc);		/* create a process as part of that user and job */
  if (sts != OZ_SUCCESS) oz_crash ("oz_knl_boot: error %u creating system process", sts);

  /* (Now paged pool requests will go to paged pool) */

  oz_knl_thread_cpuinit ();										/* make this stuff a thread of that process */
  thread = oz_knl_thread_getcur ();
  oz_knl_thread_setbasepri (thread, STARTUP_THREAD_PRIORITY);						/* (or else everything starts up at priority 0) */
  oz_knl_thread_setcurprio (thread, STARTUP_THREAD_PRIORITY);

  /* Now create sections for all pages that are already mapped in the SPT */

  numpages  = 0;
  virtpage  = 0;

  pageprot  = OZ_HW_PAGEPROT_NA;
  ppstr     = "";
  oz_s_sysmem_pagtblfr = oz_s_sysmem_pagtblsz;
  sysbasevp = OZ_HW_VADDRTOVPAGE (oz_s_sysmem_baseva);

  for (pagenum = 0; pagenum < oz_s_sysmem_pagtblsz; pagenum ++) {				/* loop through the spt */
    if ((oz_knl_process_getsecfromvpage (oz_s_systemproc, sysbasevp + pagenum, &section, &pageoffs, &pageprotx, &procmode, &mapsecflags) != 0) 
     || (oz_hw_pte_readany (sysbasevp + pagenum, &pagestate, &phypage, NULL, NULL) != NULL)) pagestate = OZ_SECTION_PAGESTATE_PAGEDOUT;
    switch (pagestate) {									/* process the state */
      case OZ_SECTION_PAGESTATE_VALID_R: {							/* READONLY: */
        if ((numpages != 0) && (pageprot != OZ_HW_PAGEPROT_UR)) {				/* see if we were doing something else */
          makesection (numpages, virtpage, pageprot, ppstr);					/* - make a section for it */
          numpages = 0;										/* - start over */
        }
        if (numpages == 0) {									/* see if starting something new */
          virtpage = pagenum;									/* - save starting page number */
          pageprot = OZ_HW_PAGEPROT_UR;								/* - this will be its protection */
          ppstr    = "RO";
        }
        numpages ++;										/* there is one more page in the section */
        break;
      }
      case OZ_SECTION_PAGESTATE_VALID_W: {							/* READWRITE: */
        if ((numpages != 0) && (pageprot != OZ_HW_PAGEPROT_KW)) {				/* see if we were doing something else */
          makesection (numpages, virtpage, pageprot, ppstr);					/* - make a section for it */
          numpages = 0;										/* - start over */
        }
        if (numpages == 0) {									/* see if starting something new */
          virtpage = pagenum;									/* - save starting page number */
          pageprot = OZ_HW_PAGEPROT_KW;								/* - this will be its protection */
          ppstr    = "RW";
        }
        numpages ++;										/* there is one more page in the section */
        break;
      }
      case OZ_SECTION_PAGESTATE_PAGEDOUT: {							/* NOTINUSE: */
        if (numpages != 0) {									/* see if we were doing something */
          makesection (numpages, virtpage, pageprot, ppstr);					/* - make a section for it */
          numpages = 0;										/* - start over */
          ppstr    = "";
        }
        break;
      }
      default: {										/* has to be one of those three */
        oz_crash ("oz_knl_boot_firstcpu: vpage %x pagestate %d", OZ_HW_VADDRTOVPAGE (oz_s_sysmem_baseva) + pagenum, pagestate);
      }
    }
  }
  if (numpages != 0) makesection (numpages, virtpage, pageprot, ppstr);				/* process the last section */

  /* Now that sections are created, create handle table for system process */

  sts = oz_knl_handletbl_create ();
  if (sts != OZ_SUCCESS) oz_crash ("oz_knl_boot_firstcpu: error %u creating handle table", sts);

  /* Define logical names and dump them out - do this before device drivers so device drivers can define logicals if they want. */
  /* Logical OZ_SYSTEM_DIRECTORY is already defined in the oz_s_systemproc header.  In it we put table OZ_SYSTEM_TABLE.         */

  oz_knl_printk ("oz_knl_boot_firstcpu: defining logical names\n");

  oz_s_systemdirectory = oz_knl_process_getlognamdir (oz_s_systemproc);
  oz_s_systemtable     = oz_knl_process_getlognamtbl (oz_s_systemproc);

  crelogname (oz_s_systemtable, OZ_LOGVALATR_TERMINAL, "OZ_LOAD_DEV", oz_s_loadparams.load_device);

  /* Start the other cpu's running */

  if ((oz_s_cpucount > 1) && !oz_s_loadparams.uniprocessor) {
    oz_knl_printk ("oz_knl_boot_firstcpu: starting alternate cpu's\n");
    oz_hw_cpu_bootalts (oz_s_cpucount);
  }

  /* Start the device drivers way down here as they can use just about anything */

  oz_knl_printk ("oz_knl_boot_firstcpu: starting device drivers\n");
  oz_knl_devinit ();
  oz_knl_printk ("oz_knl_boot_firstcpu: device driver init complete\n");
  oz_knl_devdump (0);

  /* Assign OZ_JOB_TABLE%OZ_CONSOLE to the console device.  Then, only */
  /* those that have access to the job table can access the console.   */
  /* Also create OZ_JOB_TABLE%OZ_ERROR for error messages.             */

  sts = oz_knl_iochan_crbynm ("console", OZ_LOCKMODE_EX, OZ_PROCMODE_USR, oz_s_secattr_syslogname, &oz_s_coniochan);
  if (sts != OZ_SUCCESS) oz_crash ("oz_knl_boot_firstcpu: error %u assigning channel to console", sts);
  jobtable = oz_knl_job_getlognamtbl (oz_s_systemjob);
  sts = oz_knl_logname_creobj (jobtable, OZ_PROCMODE_KNL, NULL, oz_s_secattr_syslogname, 0, 10, "OZ_CONSOLE", oz_s_coniochan, NULL);
  if (sts != OZ_SUCCESS) oz_crash ("oz_knl_boot_firstcpu: error %u creating job logical OZ_CONSOLE", sts);
  sts = oz_knl_logname_creobj (jobtable, OZ_PROCMODE_KNL, NULL, oz_s_secattr_syslogname, 0,  8, "OZ_ERROR",   oz_s_coniochan, NULL);
  if (sts != OZ_SUCCESS) oz_crash ("oz_knl_boot_firstcpu: error %u creating job logical OZ_ERROR", sts);
  sts = oz_knl_logname_creobj (jobtable, OZ_PROCMODE_KNL, NULL, oz_s_secattr_syslogname, 0,  9, "OZ_OUTPUT",  oz_s_coniochan, NULL);
  if (sts != OZ_SUCCESS) oz_crash ("oz_knl_boot_firstcpu: error %u creating job logical OZ_OUTPUT", sts);
  sts = oz_knl_logname_creobj (jobtable, OZ_PROCMODE_KNL, NULL, oz_s_secattr_syslogname, 0,  8, "OZ_INPUT",   oz_s_coniochan, NULL);
  if (sts != OZ_SUCCESS) oz_crash ("oz_knl_boot_firstcpu: error %u creating job logical OZ_INPUT", sts);

  /* Create the startup process to execute command-line initialization scripts, etc. */

  oz_knl_printk ("oz_knl_boot_firstcpu: creating startup process\n");
  oz_knl_startup ();

  /* Return to caller at priority 0 - the caller can either                                                                                           */
  /*  1) loop with 'wait-for-interrupt' instructions                                                                                    */
  /*     this will leave 'cpu init <n>' threads laying around but the cpu will be executing 'wait-for-interrupt' instruction while idle */
  /*  2) call oz_knl_thread_exit                                                                                                        */
  /*     there won't be 'cpu init <n>' threads, but the cpu will be executing a loop in the scheduler while idle                        */

  oz_knl_thread_setbasepri (thread, 0);
  oz_knl_thread_setcurprio (thread, 0);
  oz_hw_cpu_setsoftint (1);
}

static void makesection (OZ_Mempage numpages, OZ_Mempage virtpage, OZ_Hw_pageprot pageprot, const char *ppstr)

{
  uLong sts;
  OZ_Section *syspagtblsec;

  virtpage += OZ_HW_VADDRTOVPAGE (oz_s_sysmem_baseva);

  oz_knl_printk ("oz_knl_boot_firstcpu: %u page %s system section found at %p\n", numpages, ppstr, OZ_HW_VPAGETOVADDR (virtpage));

  sts = oz_knl_section_create (NULL, numpages, 0, OZ_SECTION_TYPE_ZEROES, NULL, &syspagtblsec);
  if (sts != OZ_SUCCESS) oz_crash ("oz_knl_boot_firstcpu: error %u creating system section", sts);

  sts = oz_knl_process_mapsection (syspagtblsec, &numpages, &virtpage, OZ_MAPSECTION_EXACT | OZ_MAPSECTION_SYSTEM, OZ_PROCMODE_KNL, pageprot);
  if (sts != OZ_SUCCESS) oz_crash ("oz_knl_boot_firstcpu: error %u mapping system section at %X", sts, virtpage);
}

static void crelogname (OZ_Logname *lognamtbl, uLong logvalatr, char *name, char *value)

{
  uLong sts;
  OZ_Logvalue logvalue;

  logvalue.attr = logvalatr;
  logvalue.buff = value;

  sts = oz_knl_logname_create (lognamtbl, OZ_PROCMODE_KNL, NULL, oz_s_secattr_syslogname, 0, strlen (name), name, 1, &logvalue, NULL);
  if (sts != OZ_SUCCESS) oz_crash ("error %u creating logical name %s", name);
}

/************************************************************************/
/*									*/
/*  This routine is called by the other cpus when the oz_hw_bootaltcpu 	*/
/*  has started them.  After it returns, the caller can either wait 	*/
/*  forever using wait-for-interrupt instructions or call 		*/
/*  oz_knl_thread_exit.							*/
/*									*/
/*    Input:								*/
/*									*/
/*	smplocks = none							*/
/*	softints = inhibited						*/
/*									*/
/************************************************************************/

void oz_knl_boot_anothercpu ()

{
  Long cpuidx;

  cpuidx = oz_hw_cpu_getcur ();
  if ((1 << cpuidx) & oz_s_loadparams.cpu_disable) {
    oz_knl_printk ("oz_knl_boot_anothercpu: cpu %d disabled via cpu_disable load parameter mask 0x%x\n", cpuidx, oz_s_loadparams.cpu_disable);
    oz_knl_halt ();
  }
  oz_knl_printk ("oz_knl_boot_anothercpu: initializing cpu %d\n", cpuidx);
  oz_knl_thread_cpuinit ();		/* this makes this cpu a thread (at priority 0) */
  oz_hw_cpu_setsoftint (1);		/* this allows software interrupts on this cpu */

  /* Return to caller - the caller can either                                                                                           */
  /*  1) loop with 'wait-for-interrupt' instructions                                                                                    */
  /*     this will leave 'cpu init <n>' threads laying around but the cpu will be executing 'wait-for-interrupt' instruction while idle */
  /*  2) call oz_knl_thread_exit                                                                                                        */
  /*     there won't be 'cpu init <n>' threads, but the cpu will be executing a loop in the scheduler while idle                        */
}
