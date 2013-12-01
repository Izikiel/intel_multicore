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
/*  Process handling routines						*/
/*									*/
/*  Processes are basically a set of pagetables with sections mapped 	*/
/*  to them, and threads attached to them.				*/
/*									*/
/************************************************************************/

#define _OZ_KNL_PROCESS_C
#include "ozone.h"

#include "oz_knl_devio.h"
#include "oz_knl_handle.h"
#include "oz_knl_hw.h"
#include "oz_knl_idno.h"
#include "oz_knl_image.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_logname.h"
#include "oz_knl_objtype.h"
#include "oz_knl_process.h"
#include "oz_knl_sdata.h"
#include "oz_knl_section.h"
#include "oz_knl_security.h"
#include "oz_knl_status.h"
#include "oz_knl_thread.h"
#include "oz_knl_userjob.h"

typedef enum { HWCTX_STATE_OFF, 	// completely uninitialized
               HWCTX_STATE_INITIP, 	// initialization in progress
               HWCTX_STATE_NORMAL, 	// normal functional state
               HWCTX_STATE_CLEANIP, 	// process cleanup in progress
					// - just like normal except don't let any threads start on it
               HWCTX_STATE_TERMIP	// termination in progress
             } Hwctx_state;

typedef struct Pagetable Pagetable;
typedef struct Secmap Secmap;

struct OZ_Process { OZ_Objtype objtype;			/* OZ_OBJTYPE_PROCESS */
                    OZ_Processid processid;		/* process id */
                    OZ_Process *next;			/* next in processes list */
                    Long refcount;			/* reference count */
                    OZ_Job *job;			/* job block pointer */
                    OZ_Thread *threadq;			/* list of threads this process owns */
                    OZ_Logname *lognamdir;		/* logical name directory */
                    OZ_Logname *lognamtbl;		/* logical name table */
                    OZ_Logname *logname;		/* logical name OZ_THIS_PROCESS */
                    OZ_Devunit *devalloc;		/* list of allocated devices */
                    Pagetable *pagetables;		/* page table list - sorted by ascending basepage */
                    OZ_Secattr *secattr;		/* who can do things to this process */
                    int handletblvalid;			/* 0 to begin with - pdata's handletbl could be set to copied data but obj refcounts not inc'd yet */
							/*            else - handletbl's object refcounts accurate */
                    int imagelistvalid;			/* same with image list - it has pointers to sections */
                    int knlpdatavalid;			/* other kernel mode pdata */
                    OZ_Smplock smplock_ps;		/* locks access to process state */
                    OZ_Smplock smplock_pt;		/* locks access to pagetables */
                    Hwctx_state hwctx_state;		/* hw context state */
                    char name[OZ_PROCESS_NAMESIZE];	/* process name string */
                    uByte hw_ctx[OZ_PROCESS_HW_CTX_SIZE]; /* hardware process context */
                  };

struct Pagetable { Pagetable *next;			/* pointer to next in process->pagetables list */
                   OZ_Pagetable_expand expand;		/* expansion direction when adding sections */
                   OZ_Mempage maxpages;			/* maximum number of pages allowed in pagetable */
                   OZ_Mempage basepage;			/* starting virtual page number in process used by section */
							/* always the low end, no matter if OZ_PAGETABLE_EXPUP or _EXPDN */
                   Secmap *secmaps_asc;			/* section map list - sorted by ascending vpage with no overlaps */
                   Secmap *secmaps_des;			/* descending section map list (opposite order to those in secmaps_asc) */
                 };

struct Secmap { Secmap *next_asc;			/* pointer to next in pagetable -> secmaps_asc list */
                Secmap *next_des;			/* pointer to next in pagetable -> secmaps_des list */
                OZ_Mempage npages;			/* number of pages being mapped in section */
							/* - initially set to same as section, but if section  */
							/*   expands, this will be less that the whole section */
                OZ_Mempage vpage;			/* starting virtual page number in process used by section */
							/* always the low end, no matter if OZ_PAGETABLE_EXPUP or _EXPDN */
                OZ_Section *section;			/* pointer to section block */
                OZ_Procmode ownermode;			/* owner's processor mode */
                OZ_Hw_pageprot pageprot;		/* default page protection */
                uLong mapsecflags;			/* flags it was mapped with */
                int nseclocks;
                int nseclkwzs;
                OZ_Seclock *seclocks;			/* list of pages locked for I/O */
                OZ_Seclkwz *seclkwzs;			/* list of pages being waited for no I/O */
              };

static int numprocesses;			// locked by smplock_pr
static OZ_Process *curprocesses[OZ_HW_MAXCPUS];	// no locking - access by current cpu only
static OZ_Process *processes;			// locked by smplock_pr
static OZ_Smplock smplock_pr;

static uLong copyprocess (OZ_Process *newprocess);
static OZ_Process *getcur (char *from);
static Pagetable *find_pagetable (OZ_Process *process, OZ_Mempage vpage);
static uLong find_secmap_spot (Pagetable *pagetable, uLong mapsecflags, OZ_Mempage npages, OZ_Mempage vpage, 
                               Secmap **lsecmap_r, Secmap **hsecmap_r, OZ_Mempage *vpage_r);
static void link_secmap_to_pagetable (Secmap *secmap, Pagetable *pagetable, Secmap *hsecmap, Secmap *lsecmap);
static void validate_secmaps (Pagetable *pagetable);
static void process_dump (OZ_Process *process, int dothreads);

/************************************************************************/
/*									*/
/*  Initialize static data at boot time					*/
/*									*/
/************************************************************************/

void oz_knl_process_init (void)

{
  int i;

  memset (curprocesses, 0, sizeof curprocesses);				/* clear list of processes mapped on cpu's */
  processes = NULL;								/* no processes yet defined */
  numprocesses = 0;
  oz_hw_smplock_init (sizeof smplock_pr, &smplock_pr, OZ_SMPLOCK_LEVEL_PR);	/* init process smp lock */
}

/************************************************************************/
/*									*/
/*  Create a process							*/
/*									*/
/*    Input:								*/
/*									*/
/*	job      = job that the process belongs to			*/
/*	sysproc  = 0 : other than oz_s_systemproc process		*/
/*	           1 : creating oz_s_systemproc process			*/
/*	copyproc = 0 : just create an empty process			*/
/*	           1 : copy current process to the new process		*/
/*	secattr  = who can do things to this process			*/
/*	name     = process name string pointer				*/
/*									*/
/*	ipl = softint (required by oz_hw_process_initctx)		*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_process_create = OZ_SUCCESS : successful			*/
/*	                              else : error status		*/
/*	*process_r = pointer to process block				*/
/*									*/
/*    Note:								*/
/*									*/
/*	Caller must call oz_knl_process_increfc (process, -1) when 	*/
/*	done with process block pointer					*/
/*									*/
/************************************************************************/

uLong oz_knl_process_create (OZ_Job *job, int sysproc, int copyproc, int name_l, const char *name, OZ_Secattr *secattr, OZ_Process **process_r)

{
  int i;
  OZ_Process *process;
  OZ_Processid pid;
  uLong pr, sts;

  *process_r = NULL;

  process = OZ_KNL_NPPMALLOQ (sizeof *process);		/* allocate block of memory */
  if (process == NULL) return (OZ_EXQUOTANPP);		/* return error if can't allocate */
  process -> objtype     = OZ_OBJTYPE_PROCESS;		/* set up object type */
  process -> job         = job;				/* save job pointer */
  process -> refcount    = 1;				/* this ref count is for the process pointer that we return */
  process -> threadq     = NULL;			/* nothing in the thread queue */
  process -> devalloc    = NULL;			/* no devices allocated yet */
  process -> pagetables  = NULL;			/* no pagetables yet */
  process -> secattr     = secattr;			/* save security attributes pointer */
  process -> hwctx_state = HWCTX_STATE_OFF;		/* hardware context not initialized yet */
  process -> lognamdir   = NULL;			/* no logical name directory created yet */
  process -> lognamtbl   = NULL;			/* no logical name table created yet */
  process -> logname     = NULL;			/* no logical name yet */
  process -> handletblvalid = 0;			/* handle table not valid yet */
  process -> imagelistvalid = 0;			/* image list not valid yet */
  process -> knlpdatavalid  = 0;			/* other knl mode pdata not valid yet */
  oz_knl_job_increfc (job, 1);				/* inc job ref count */
  if (secattr != NULL) oz_knl_secattr_increfc (secattr, 1); /* inc security attributes ref count */
  oz_hw_smplock_init (sizeof process -> smplock_ps, &(process -> smplock_ps), OZ_SMPLOCK_LEVEL_PS);
  oz_hw_smplock_init (sizeof process -> smplock_pt, &(process -> smplock_pt), OZ_SMPLOCK_LEVEL_PT);
  if (name_l > sizeof process -> name - 1) name_l = sizeof process -> name - 1;
  movc4 (name_l, name, sizeof process -> name, process -> name);

  /* Create system/process logical name directory */
  /* - if sysproc 0, use OZ_PROCESS_DIRECTORY */
  /*              1, use OZ_SYSTEM_DIRECTORY */

  if (sysproc) process -> lognamdir = oz_s_systemdirectory;
  else {
    sts = oz_knl_logname_create (NULL, OZ_PROCMODE_KNL, NULL, secattr, OZ_LOGNAMATR_TABLE, strlen (oz_s_logname_directorynames[0]), oz_s_logname_directorynames[0], 0, NULL, &(process -> lognamdir));
    if (sts != OZ_SUCCESS) {
      oz_knl_printk ("oz_knl_process_create: error %u creating process logical name directory\n", sts);
      goto rtnerr;
    }
  }

  /* Create system/process logical name table */
  /* - if sysproc 0, use OZ_PROCESS_TABLE */
  /*              1, use OZ_SYSTEM_TABLE */

  if (sysproc) process -> lognamtbl = oz_s_systemtable;
  else {
    sts = oz_knl_logname_create (process -> lognamdir, OZ_PROCMODE_KNL, NULL, secattr, OZ_LOGNAMATR_TABLE, strlen (oz_s_logname_deftblnames[0]), oz_s_logname_deftblnames[0], 0, NULL, &(process -> lognamtbl));
    if (sts != OZ_SUCCESS) {
      oz_knl_printk ("oz_knl_process_create: error %u creating process logical name table\n", sts);
      goto rtnerr;
    }
  }

  /* Create OZ_THIS_PROCESS logical to point to process block.    */
  /* Note that this increments the process block reference count. */

  sts = oz_knl_logname_creobj (process -> lognamtbl, OZ_PROCMODE_KNL, NULL, secattr, OZ_LOGNAMATR_NOSUPERSEDE | OZ_LOGNAMATR_NOOUTERMODE, 
                               17, sysproc ? "OZ_SYSTEM_PROCESS" : "OZ_THIS_PROCESS", process, &(process -> logname));
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_knl_process_create: error %u creating process logical\n", sts);
    goto rtnerr;
  }

  /* If this is the system process, make it current on all cpus                         */
  /* This is so oz_hw_process_switchctx will always get a non-null old process pointer  */
  /* Also, if hw init tries to map a pagetable section, process_mapsection will find it */

  if (sysproc) {
    for (i = 0; i < oz_s_cpucount; i ++) curprocesses[i] = process;
    oz_s_systemproc = process;
  }

  /* Initialize hardware context (stuff like create pagetables) */

  process -> hwctx_state = HWCTX_STATE_INITIP;
  sts = oz_hw_process_initctx (process -> hw_ctx, process, sysproc, copyproc);
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_knl_process_create: error %u initializing hardware context\n", sts);
    goto rtnerr;
  }
  OZ_HW_MB;
  process -> hwctx_state = HWCTX_STATE_NORMAL;

  /* Link it to the processes list and assign it a process id number - after this point, outsiders can see it so it wouldn't be easy to clean up */

  pr = oz_hw_smplock_wait (&smplock_pr);		/* lock changes to process database */
  process -> processid = oz_knl_idno_alloc (process);	/* allocate a process-id number */
  process -> next = processes;				/* link it to list of all processes */
  processes = process;
  numprocesses ++;

  oz_hw_smplock_clr (&smplock_pr, pr);			/* release process database */

  /* Now it is a complete process.  If requested, copy current process to it. */

  if (copyproc) {
    sts = copyprocess (process);
    if (sts != OZ_SUCCESS) {
      oz_knl_process_increfc (process, -1);
      return (sts);
    }
  }

  /* Otherwise, mark the pdata valid because it's all zeroes */

  else {
    process -> handletblvalid = 1;
    process -> imagelistvalid = 1;
    process -> knlpdatavalid  = 1;
  }

  /* Return pointer and success status */

  *process_r = process;
  return (OZ_SUCCESS);

  /* Error, clean up and return error status */

rtnerr:
  if (secattr != NULL) oz_knl_secattr_increfc (secattr, -1);
  if (process -> lognamdir != NULL) oz_knl_logname_delete (process -> lognamdir);
  if (process -> lognamtbl != NULL) oz_knl_logname_delete (process -> lognamtbl);
  if (process -> logname   != NULL) oz_knl_logname_delete (process -> logname);
  oz_knl_job_increfc (job, -1);
rtnerr_ht:
  oz_knl_devunit_dallocall (&(process -> devalloc));
  OZ_KNL_NPPFREE (process);
  return (sts);
}

/************************************************************************/
/*									*/
/*  The last thread of a process is about to enter zombie state.  So 	*/
/*  we do process cleanup functions before actually entering the 	*/
/*  zombie state.							*/
/*									*/
/*    Input:								*/
/*									*/
/*	the system process is current					*/
/*	smplock = ps							*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_process_cleanup = 0 : the smplock was not released	*/
/*	                         1 : the smplock was released		*/
/*	                             (now at SOFTINT level)		*/
/*									*/
/************************************************************************/

int oz_knl_process_cleanup (OZ_Process *process)

{
  OZ_Job *job;
  OZ_Logname *logname;
  OZ_Mempage vpage;
  OZ_Process *curprocess;
  Pagetable *pagetable;
  uLong pr, pt;

  /* We don't clean up the system process nor any process that has already been cleaned up */

  if ((process == oz_s_systemproc) 				// don't clean up system process
								// (?? should we generalize this with a flag for other processes ??)
   || (process -> hwctx_state != HWCTX_STATE_NORMAL)) {		// only do this once per process so we don't get this running twice at the same time
    oz_hw_smplock_clr (&(process -> smplock_ps), OZ_SMPLOCK_SOFTINT);
    return (0);
  }
  process -> hwctx_state = HWCTX_STATE_CLEANIP;			// remember it is running on some thread of the process
								// ... and prevent other threads from starting

  oz_hw_smplock_clr (&(process -> smplock_ps), OZ_SMPLOCK_SOFTINT);

  oz_knl_process_setcur (process);				// make it the process we are currently mapped to
								// so we can access its pagetables, etc

  /* Close all handles left open by threads.  This will close files, unload images, release event flags, etc          */
  /* Don't do it, though, if we are a copied process and the handltbl's objects haven't had their refcounts inc'd yet */

  if (process -> handletblvalid) oz_knl_handletbl_delete ();

  /* Now that all image handles are released, free off the imagelist struct */

  if (process -> imagelistvalid) oz_knl_imagelist_close ();

  /* Close out any other kernel mode pdata stuff */

  if (process -> knlpdatavalid) oz_sys_pdata_cleanup ();

  /* Delete the logical name stuff */

  logname = process -> lognamdir;			/* see if OZ_PROCESS_DIRECTORY is around */
  if (logname != NULL) {
    process -> lognamdir = NULL;			/* if so, hide it from oz_knl_process_validateall */
    oz_knl_logname_delete (logname);			/* delete the logical name */
  }

  logname = process -> lognamtbl;			/* same with OZ_PROCESS_TABLE */
  if (logname != NULL) {
    process -> lognamtbl = NULL;
    oz_knl_logname_delete (logname);
  }

  logname = process -> logname;				/* see if OZ_THIS_PROCESS needs to be deleted */
  if (logname != NULL) {
    process -> logname = NULL;				/* if so, clear pointer */
    oz_knl_logname_delete (logname);			/* delete logical name (this decrements process' ref count) */
  }

  /* Clean up per-process data */

  oz_sys_pdata_cleanup ();

  /* Delete any page tables */

  while (1) {
    pt = oz_hw_smplock_wait (&(process -> smplock_pt));	/* set the pt lock while accessing pagetable stuff */
    if (pt != OZ_SMPLOCK_SOFTINT) oz_crash ("oz_knl_process_cleanup: called with smp locks set");
    pagetable = process -> pagetables;			/* point to top page table */
    if (pagetable == NULL) break;			/* all done if there aren't any */
    vpage = pagetable -> basepage;			/* get starting virtual page */
    if (pagetable -> maxpages == 0) oz_crash ("oz_knl_process_cleanup: pt at vpage %u has maxpages zero", vpage);
    oz_hw_smplock_clr (&(process -> smplock_pt), OZ_SMPLOCK_SOFTINT);
    oz_knl_process_deletepagetable (vpage);		/* delete page table */
  }
  oz_hw_smplock_clr (&(process -> smplock_pt), OZ_SMPLOCK_SOFTINT); /* no page tables left, release pt lock */

  /* Release job block pointer */

  job = process -> job;
  if (job != NULL) {
    process -> job = NULL;
    oz_knl_job_increfc (job, -1);
  }

  /* Tell hardware layer this is the end of the last thread for this process so it should */
  /* do as much cleanup now as it can then finish up when oz_hw_thread_termctx is called  */

  oz_hw_smplock_wait (&smplock_pr);				// make sure oz_knl_process_validateall isn't running
  if (process -> hwctx_state != HWCTX_STATE_CLEANIP) oz_crash ("oz_knl_process_cleanup: hwctx_state %d, not CLEANIP", process -> hwctx_state);
  process -> hwctx_state = HWCTX_STATE_TERMIP;			// so oz_knl_process_validateall won't choke on it
  oz_hw_smplock_clr (&smplock_pr, OZ_SMPLOCK_SOFTINT);
  oz_hw_process_termctx (process -> hw_ctx, process);		// terminate hardware context
  process -> hwctx_state = HWCTX_STATE_OFF;			// state is wiped out

  /* Switch to the system process before returning as the old process' pagetables are unusable */

  oz_knl_process_setcur (oz_s_systemproc);
  return (1);
}

/************************************************************************/
/*									*/
/*  Increment process reference count					*/
/*									*/
/*    Input:								*/
/*									*/
/*	proc = pointer to process block					*/
/*	inc  = 1 : increment reference count				*/
/*	       0 : no-op						*/
/*	      -1 : decrement reference count				*/
/*	smplock <= pr							*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_process_increfc = new reference count			*/
/*	if zero, process block is deleted				*/
/*									*/
/************************************************************************/

Long oz_knl_process_increfc (OZ_Process *process, Long inc)

{
  Long refc;
  OZ_Process **lprocess, *xprocess;
  uLong pr;

  OZ_KNL_CHKOBJTYPE (process, OZ_OBJTYPE_PROCESS);

  /* If it is not going to zero, just increment it as is */

again:
  do {
    refc = process -> refcount;									// sample refcount
    if (refc <= 0) goto already_le_zero;							// should never be .le. zero
    if (refc + inc <= 0) goto going_le_zero;							// see if going .le. zero
  } while (!oz_hw_atomic_setif_long (&(process -> refcount), refc + inc, refc));		// staying .gt. zero, write it
												// ... assuming it hasn't changed
  return (refc + inc);										// return new value

already_le_zero:
  oz_crash ("oz_knl_process_increfc: %p refcount was %d", process, refc);

  /* It is going zero, take the pr lock first to keep the 'processes' list intact throughout */

going_le_zero:
  if (refc + inc < 0) oz_crash ("oz_knl_process_increfc: %p refcount %d+%d going neg", process, refc, inc);
  pr = oz_hw_smplock_wait (&smplock_pr);							// keep others out of 'processes' list
  if (!oz_hw_atomic_setif_long (&(process -> refcount), 0, refc)) {				// now write it zero
    oz_hw_smplock_clr (&smplock_pr, pr);							// refcount changed on us
    goto again;											// ... so go try it all again
  }
  if (process -> hwctx_state != HWCTX_STATE_OFF) oz_crash ("oz_knl_process_increfc: process %p ref count zero, hwctx_state %d", process, process -> hwctx_state);
  for (lprocess = &processes; (xprocess = *lprocess) != process; lprocess = &(xprocess -> next)) {
    if (xprocess == NULL) oz_crash ("oz_knl_process_increfc: process %p not found on processes list", process);
  }
  *lprocess = xprocess -> next;									// unlink from processes list
  numprocesses --;
  oz_knl_idno_free (process -> processid);							// release id number
  oz_hw_smplock_clr (&smplock_pr, pr);								// release smp lock
  if (process -> secattr != NULL) oz_knl_secattr_increfc (process -> secattr, -1);
  oz_knl_devunit_dallocall (&(process -> devalloc));						// dealloc all devices
  OZ_KNL_NPPFREE (process);									// free off process block
  return (0);
}

/************************************************************************/
/*									*/
/*  Copy current process to a newly created process			*/
/*									*/
/*    Input:								*/
/*									*/
/*	newprocess = newly created process to copy pages to		*/
/*	smplock = softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	copyprocess = OZ_SUCCESS : successful				*/
/*	                    else : error status				*/
/*									*/
/*    Notes:								*/
/*									*/
/*	If an error is returned, a partial copy may have been 		*/
/*	completed.  This routine does not attempt to clean up.  It is 	*/
/*	assumed that the caller will throw away the process in this 	*/
/*	case.								*/
/*									*/
/*	This routine provides no synchronization (other than to 	*/
/*	protect the system's database), so it is up to the caller to 	*/
/*	provide any required.						*/
/*									*/
/************************************************************************/

static uLong insert_secmap_into_process (Secmap *secmap, OZ_Process *process);
static uLong copylnmtbl (OZ_Logname *oldlognamtbl, OZ_Logname *newlognamtbl);

static uLong copyprocess (OZ_Process *newprocess)

{
  int i, nsecmaps;
  OZ_Process *oldprocess;
  OZ_Section **secxlate;
  Pagetable *pagetable;
  Secmap **lnewsecmap, *newsecmap, *newsecmaps, *oldsecmap;
  uLong pt, sts;

  OZ_KNL_CHKOBJTYPE (newprocess, OZ_OBJTYPE_PROCESS);

  oldprocess = oz_knl_process_getcur ();

  /* Make a copy of the secmaps list (ie, what sections are mapped where).          */
  /* Skip over pagetable sections as oz_hw_process_initctx should have copied them. */

  nsecmaps = 0;
  pt = oz_hw_smplock_wait (&(oldprocess -> smplock_pt));
  lnewsecmap = &newsecmaps;
  for (pagetable = oldprocess -> pagetables; pagetable != NULL; pagetable = pagetable -> next) {
    for (oldsecmap = pagetable -> secmaps_asc; oldsecmap != NULL; oldsecmap = oldsecmap -> next_asc) {
      if (oldsecmap -> mapsecflags & OZ_MAPSECTION_NOCOPY) continue;
      if (oz_knl_section_getsectype (oldsecmap -> section) & OZ_SECTION_TYPE_PAGTBL) continue;
      newsecmap = OZ_KNL_NPPMALLOQ (sizeof *newsecmap);
      if (newsecmap == NULL) goto nonppquota;
      *lnewsecmap = newsecmap;
      *newsecmap  = *oldsecmap;
      newsecmap -> npages      = oldsecmap -> npages;
      newsecmap -> vpage       = oldsecmap -> vpage;
      newsecmap -> section     = oldsecmap -> section;
      newsecmap -> ownermode   = oldsecmap -> ownermode;
      newsecmap -> pageprot    = oldsecmap -> pageprot;
      newsecmap -> mapsecflags = oldsecmap -> mapsecflags;
      newsecmap -> nseclocks   = 0;
      newsecmap -> nseclkwzs   = 0;
      newsecmap -> seclocks    = NULL;
      newsecmap -> seclkwzs    = NULL;
      lnewsecmap  = &(newsecmap -> next_asc);
      oz_knl_section_increfc (newsecmap -> section, 1);
      nsecmaps ++;
    }
  }
  *lnewsecmap = NULL;
  oz_hw_smplock_clr (&(oldprocess -> smplock_pt), pt);

  /* Copy the contents of each section.  Save the section translations in an array for imagelist routine. */

  secxlate = OZ_KNL_NPPMALLOQ (nsecmaps * 2 * sizeof *secxlate);
  if (secxlate == NULL) goto nonppquota_nl;
  i = 0;

  sts = OZ_SUCCESS;
  while ((newsecmap = newsecmaps) != NULL) {
    newsecmaps = newsecmap -> next_asc;

    /* Link new secmap entry to new process */

    pt  = oz_hw_smplock_wait (&(newprocess -> smplock_pt));
    sts = insert_secmap_into_process (newsecmap, newprocess);
    oz_hw_smplock_clr (&(newprocess -> smplock_pt), pt);
    if (sts != OZ_SUCCESS) break;

    /* Copy the pages (or make it look like they were copied) */

    secxlate[i] = newsecmap -> section;				// save old section pointer in case it's an image section
    sts = oz_knl_section_copypages (newprocess, newsecmap -> npages, newsecmap -> vpage, &(newsecmap -> section), 
                                    newsecmap -> ownermode, newsecmap -> pageprot, newsecmap -> mapsecflags);
    if (sts != OZ_SUCCESS) break;
    secxlate[nsecmaps+i++] = newsecmap -> section;		// save corresponding new section in 2nd half of array

    /* Now the section is mapped to the new process */

    sts = oz_knl_section_mapproc (newsecmap -> section, newprocess);
    if (sts != OZ_SUCCESS) break;
  }

  /* Fix up any kernel mode pdata */

  oz_knl_process_setcur (newprocess);
  if (sts == OZ_SUCCESS) {
    sts = oz_sys_pdata_copied ();
    if (sts == OZ_SUCCESS) newprocess -> knlpdatavalid = 1;
  }

  /* Increment refcounts on all objects pointed to by the handles */

  if (sts == OZ_SUCCESS) {
    oz_knl_handle_tablecopied ();			// inc refcounts on all referenced objects
    newprocess -> handletblvalid = 1;			// the objects' refcounts are valid now
  }

  /* Fix up section pointers in process' image list */

  if (sts == OZ_SUCCESS) {
    sts = oz_knl_imagelist_copied (nsecmaps, secxlate, secxlate + nsecmaps);
    if (sts == OZ_SUCCESS) newprocess -> imagelistvalid = 1;
  }
  OZ_KNL_NPPFREE (secxlate);
  oz_knl_process_setcur (oldprocess);

  /* Copy process' logical name directory and table */

  if (sts == OZ_SUCCESS) sts = copylnmtbl (oldprocess -> lognamdir, newprocess -> lognamdir);
  if (sts == OZ_SUCCESS) sts = copylnmtbl (oldprocess -> lognamtbl, newprocess -> lognamtbl);

  return (sts);

  /* Ran out of npp quota while creating new secmaps */

nonppquota:
  oz_hw_smplock_clr (&(oldprocess -> smplock_pt), pt);
  *lnewsecmap = NULL;
nonppquota_nl:
  while ((newsecmap = newsecmaps) != NULL) {
    newsecmaps = newsecmap -> next_asc;
    oz_knl_section_increfc (newsecmap -> section, -1);
    OZ_KNL_NPPFREE (newsecmap);
  }
  return (OZ_EXQUOTANPP);
}

/* Insert section mapping into process' pagetable list */

static uLong insert_secmap_into_process (Secmap *secmap, OZ_Process *process)

{
  OZ_Mempage npages, vpage;
  Pagetable *pagetable;
  Secmap *hsecmap, *lsecmap;
  uLong sts;

  /* Get number of pages in section that are being mapped */

  npages = secmap -> npages;
  if (npages == 0) oz_crash ("oz_knl_process insert_secmap_into_process: secmap has npages zero\n");

  /* Get starting virtual page number to map the section's lowest address at */

  vpage = secmap -> vpage;

  /* Find corresponding pagetable */

  pagetable = find_pagetable (process, vpage);
  if (pagetable == NULL) return (OZ_NOPAGETABLE);;

  /* Find where secmap goes in sorted secmap list, make sure nothing overlaps it */

  sts = find_secmap_spot (pagetable, OZ_MAPSECTION_EXACT, npages, vpage, &lsecmap, &hsecmap, &vpage);
  if (sts != OZ_SUCCESS) return (sts);

  /* hsecmap = points to secmap at higher address */
  /* lsecmap = points to secmap at lower address */

  /* Link secmap block in place */

  link_secmap_to_pagetable (secmap, pagetable, hsecmap, lsecmap);

  return (OZ_SUCCESS);
}

/* Copy a logical name table from the old process to the new one */

static uLong copylnmtbl (OZ_Logname *oldlognamtbl, OZ_Logname *newlognamtbl)

{
  const char *name;
  const OZ_Logvalue *values;
  int name_l;
  OZ_Logname *oldlogname;
  OZ_Procmode oldprocmode, newprocmode;
  OZ_Secattr *secattr;
  uLong lognamatr, nvalues, sts;

  oldlogname = NULL;
  while (((sts = oz_knl_logname_gettblent (oldlognamtbl, &oldlogname)) == OZ_SUCCESS) && (oldlogname != NULL)) {

    /* Get stuff about the name that was found */

    name   = oz_knl_logname_getname (oldlogname);
    name_l = strlen (name);
    sts    = oz_knl_logname_getval (oldlogname, &oldprocmode, &lognamatr, &nvalues, &values, NULL);
    if (sts != OZ_SUCCESS) oz_crash ("oz_knl_process_copy: error %u get logname %s attributes", sts, name);

    /* Don't copy if the name already exists in the new table (stuff like OZ_PROCESS_TABLE, OZ_THIS_PROCESS) */

    sts = oz_knl_logname_lookup (newlognamtbl, oldprocmode, name_l, name, &newprocmode, NULL, NULL, NULL, NULL, NULL);
    if ((sts == OZ_SUCCESS) && (newprocmode == oldprocmode)) continue;

    /* Copy it to the new table */

    secattr = oz_knl_logname_getsecattr (oldlogname);
    sts = oz_knl_logname_create (newlognamtbl, oldprocmode, NULL, secattr, lognamatr, name_l, name, nvalues, values, NULL);
    if (secattr != NULL) oz_knl_secattr_increfc (secattr, -1);
    if (sts != OZ_SUCCESS) {
      oz_knl_printk ("oz_knl_process_copy: error %u copying logname %s\n", sts, name);
      oz_knl_logname_increfc (oldlogname, -1);
      return (sts);
    }
  }

  if (sts != OZ_SUCCESS) oz_knl_printk ("oz_knl_process_copy: error %u scanning logname table\n", sts);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Set the current process, ie, swap pagetables the cpu is using	*/
/*									*/
/*    Input:								*/
/*									*/
/*	process  = process to set it to					*/
/*	smp lock = at least softint					*/
/*									*/
/************************************************************************/

void oz_knl_process_setcur (OZ_Process *process)

{
  Long cpuidx;
  OZ_Process *oldproc;

  OZ_KNL_CHKOBJTYPE (process, OZ_OBJTYPE_PROCESS);
  cpuidx  = oz_hw_cpu_getcur ();					/* find out what cpu we are on */
  if ((process -> hwctx_state != HWCTX_STATE_INITIP) && (process -> hwctx_state != HWCTX_STATE_NORMAL) && (process -> hwctx_state != HWCTX_STATE_CLEANIP)) {
    process = oz_s_systemproc;						/* if new one has been terminated, use system mapping */
  }
  oldproc = curprocesses[cpuidx];					/* get pointer to process it is mapped to */
  if (process != oldproc) {						/* don't bother if same process */
    curprocesses[cpuidx] = process;					/* save pointer to process it will be mapped to */
    oz_hw_process_switchctx (oldproc -> hw_ctx, process -> hw_ctx);	/* save old mapping, get new mapping */
  }
									// I considered for a while an optimization of not 
									// switching to the system process if the old process is 
									// NORMAL.  But it could be that some other thread on 
									// another cpu will terminate the process thus this cpu's 
									// pagetables will get trashed, so I don't think it can be 
									// easily done.
}

/************************************************************************/
/*									*/
/*  Get process mapped by the current cpu				*/
/*									*/
/************************************************************************/

OZ_Process *oz_knl_process_getcur (void)

{
  OZ_Process *process;

  if (oz_hw_cpu_smplevel () == OZ_SMPLOCK_NULL) {
    oz_hw_cpu_setsoftint (0);
    process = curprocesses[oz_hw_cpu_getcur()];
    oz_hw_cpu_setsoftint (1);
  } else {
    process = curprocesses[oz_hw_cpu_getcur()];
  }
  OZ_KNL_CHKOBJTYPE (process, OZ_OBJTYPE_PROCESS);
  return (process);
}

/************************************************************************/
/*									*/
/*  Return pointer to process' thread queue listhead			*/
/*									*/
/*    Input:								*/
/*									*/
/*	process  = pointer to process in question			*/
/*	ifnormal = return NULL if process not in NORMAL state		*/
/*	smplock  = ps							*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_process_threadqp = NULL : ifnormal was set and process 	*/
/*	                                 not in NORMAL state		*/
/*	                          else : address of thread queue 	*/
/*	                                 listhead			*/
/*									*/
/************************************************************************/

OZ_Thread **oz_knl_process_getthreadqp (OZ_Process *process, int ifnormal)

{
  if (process == NULL) process = getcur ("oz_knl_process_getthreadqp");
  OZ_KNL_CHKOBJTYPE (process, OZ_OBJTYPE_PROCESS);
  if (ifnormal && (process -> hwctx_state != HWCTX_STATE_NORMAL)) return (NULL);
  return (&(process -> threadq));
}

/************************************************************************/
/*									*/
/*  Get job associated with a process					*/
/*									*/
/************************************************************************/

OZ_Job *oz_knl_process_getjob (OZ_Process *process)

{
  if (process == NULL) process = getcur ("oz_knl_process_getjob");
  OZ_KNL_CHKOBJTYPE (process, OZ_OBJTYPE_PROCESS);
  return (process -> job);
}

/************************************************************************/
/*									*/
/*  Get name associated with a process					*/
/*									*/
/************************************************************************/

const char *oz_knl_process_getname (OZ_Process *process)

{
  if (process == NULL) process = getcur ("oz_knl_process_getname");
  OZ_KNL_CHKOBJTYPE (process, OZ_OBJTYPE_PROCESS);
  return (process -> name);
}

/************************************************************************/
/*									*/
/*  Get next process in list corresponding to a given job		*/
/*									*/
/*    Input:								*/
/*									*/
/*	lastprocess = NULL : get first process in job			*/
/*	              else : last process that info we got		*/
/*	job = corresponding job block					*/
/*	      NULL : get all processes, regardless of job		*/
/*	smplevel <= pr							*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_process_getnext = NULL : no more processes		*/
/*	                         else : pointer to process		*/
/*									*/
/*    Note:								*/
/*									*/
/*	Refcount of input process is not touched			*/
/*	Refcount of output process is incremented			*/
/*									*/
/************************************************************************/

OZ_Process *oz_knl_process_getnext (OZ_Process *lastprocess, OZ_Job *job)

{
  OZ_Process *process;
  uLong pr;

  process = lastprocess;
  pr = oz_hw_smplock_wait (&smplock_pr);
  if (process == NULL) process = processes;
  else process = process -> next;
  while (process != NULL) {
    if ((job == NULL) || (process -> job == job)) {
      oz_knl_process_increfc (process, 1);
      break;
    }
    process = process -> next;
  }
  oz_hw_smplock_clr (&smplock_pr, pr);
  return (process);
}

/************************************************************************/
/*									*/
/*  Count the processes on a job					*/
/*									*/
/************************************************************************/

uLong oz_knl_process_count (OZ_Job *job)

{
  OZ_Process *process;
  uLong count, pr;

  count = 0;
  pr = oz_hw_smplock_wait (&smplock_pr);
  for (process = processes; process != NULL; process = process -> next) if (process -> job == job) count ++;
  oz_hw_smplock_clr (&smplock_pr, pr);

  return (count);
}

/************************************************************************/
/*									*/
/*  Return process id number						*/
/*									*/
/*    Input:								*/
/*									*/
/*	process = pointer to process in question			*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_process_getid = process id				*/
/*									*/
/************************************************************************/

OZ_Processid oz_knl_process_getid (OZ_Process *process)

{
  if (process == NULL) process = getcur ("oz_knl_process_getid");
  OZ_KNL_CHKOBJTYPE (process, OZ_OBJTYPE_PROCESS);
  return (process -> processid);
}

/************************************************************************/
/*									*/
/*  Get process pointer given the pid					*/
/*									*/
/*    Input:								*/
/*									*/
/*	pid = process id to look up					*/
/*									*/
/*	smplock <= pr							*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_process_frompid = NULL : no such process			*/
/*	                         else : pointer to process		*/
/*									*/
/*    Note:								*/
/*									*/
/*	this routine increments the process' reference count		*/
/*									*/
/************************************************************************/

OZ_Process *oz_knl_process_frompid (OZ_Processid pid)

{
  OZ_Process *process;
  uLong pr;

  pr = oz_hw_smplock_wait (&smplock_pr);			/* lock out changes to table */
  process = oz_knl_idno_find (pid, OZ_OBJTYPE_PROCESS);		/* find the process in id table */
  if (process != NULL) oz_knl_process_increfc (process, 1);	/* if something there, inc process ref count so process doesn't go away */
  oz_hw_smplock_clr (&smplock_pr, pr);				/* allow others access to table */
  return (process);						/* return pointer to process */
}

/************************************************************************/
/*									*/
/*  Return pointer to process' logical name directory			*/
/*									*/
/*    Input:								*/
/*									*/
/*	process = pointer to process in question			*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_process_getlognamtbl = address of logical name directory	*/
/*									*/
/************************************************************************/

OZ_Logname *oz_knl_process_getlognamdir (OZ_Process *process)

{
  if (process == NULL) process = getcur ("oz_knl_process_getlognamdir");
  OZ_KNL_CHKOBJTYPE (process, OZ_OBJTYPE_PROCESS);
  return (process -> lognamdir);
}

/************************************************************************/
/*									*/
/*  Return pointer to process' logical name table			*/
/*									*/
/*    Input:								*/
/*									*/
/*	process = pointer to process in question			*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_process_getlognamtbl = address of logical name table	*/
/*									*/
/************************************************************************/

OZ_Logname *oz_knl_process_getlognamtbl (OZ_Process *process)

{
  if (process == NULL) process = getcur ("oz_knl_process_getlognamtbl");
  OZ_KNL_CHKOBJTYPE (process, OZ_OBJTYPE_PROCESS);
  return (process -> lognamtbl);
}

/************************************************************************/
/*									*/
/*  Return pointer to process' security attributes block		*/
/*									*/
/*    Input:								*/
/*									*/
/*	process = pointer to process in question			*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_process_getsecattr = security attributes block pointer	*/
/*	 - reference count has been incremented				*/
/*									*/
/************************************************************************/

OZ_Secattr *oz_knl_process_getsecattr (OZ_Process *process)

{
  OZ_Secattr *secattr;

  if (process == NULL) process = getcur ("oz_knl_process_getsecattr");
  OZ_KNL_CHKOBJTYPE (process, OZ_OBJTYPE_PROCESS);
  secattr = process -> secattr;
  oz_knl_secattr_increfc (secattr, 1);
  return (secattr);
}

/************************************************************************/
/*									*/
/*  Return pointer to process' hardware context block			*/
/*									*/
/*    Input:								*/
/*									*/
/*	process = pointer to process in question			*/
/*	          NULL means current process				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_process_gethwctx = pointer to hardware context block	*/
/*									*/
/************************************************************************/

void *oz_knl_process_gethwctx (OZ_Process *process)

{
  if (process == NULL) process = getcur ("oz_knl_process_gethwctx");
  OZ_KNL_CHKOBJTYPE (process, OZ_OBJTYPE_PROCESS);
  return (process -> hw_ctx);
}

/************************************************************************/
/*									*/
/*  Get current process - this checks the thread process vs mapped 	*/
/*  process and crashes if they don't match				*/
/*									*/
/************************************************************************/

static OZ_Process *getcur (char *from)

{
  OZ_Process *curexproc, *curmaproc;

  curexproc = oz_knl_thread_getprocesscur ();		/* get process of currently executing thread */
  curmaproc = curprocesses[oz_hw_cpu_getcur()];		/* get what process is mapped to memory */
  if (curexproc != curmaproc) oz_crash ("%s: called with NULL when current executing proc is %p and mapped proc is %p", from, curexproc, curmaproc);

  return (curmaproc);
}

/************************************************************************/
/*									*/
/*  Get listhead for allocated devices					*/
/*									*/
/************************************************************************/

OZ_Devunit **oz_knl_process_getdevalloc (OZ_Process *process)

{
  OZ_KNL_CHKOBJTYPE (process, OZ_OBJTYPE_PROCESS);
  return (&(process -> devalloc));
}

/************************************************************************/
/*									*/
/*  Create a page table for a process					*/
/*									*/
/*    Input:								*/
/*									*/
/*	process  = process to add page table to				*/
/*	expand   = expansion direction					*/
/*	maxpages = maximum number of pages possible in table		*/
/*	basepage = starting virtual page number of table		*/
/*	           (always the low address regardless of expansion dir)	*/
/*									*/
/*	smp lock < pt							*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_process_createpagetable = OZ_SUCCESS : successful	*/
/*	                                       else : error status	*/
/*									*/
/************************************************************************/

uLong oz_knl_process_createpagetable (OZ_Process *process, OZ_Pagetable_expand expand, OZ_Mempage maxpages, OZ_Mempage basepage)

{
  Pagetable **lpagetable, *pagetable;
  uLong pt;

  /* Address range can't wrap around and we must have at least one page */

  if (basepage + maxpages <= basepage) return (OZ_ADDRINUSE);

  /* Lock process' pagetable list */

  pt = oz_hw_smplock_wait (&(process -> smplock_pt));

  /* Make sure address range is not already in use */

  for (lpagetable = &(process -> pagetables); (pagetable = *lpagetable) != NULL; lpagetable = &(pagetable -> next)) {
    if (basepage + maxpages < pagetable -> basepage) break;
    if (basepage < pagetable -> basepage + pagetable -> maxpages) {
      oz_hw_smplock_clr (&(process -> smplock_pt), pt);
      return (OZ_ADDRINUSE);
    }
  }

  /* Create new table */

  pagetable = OZ_KNL_NPPMALLOQ (sizeof *pagetable);
  if (pagetable == NULL) {
    oz_hw_smplock_clr (&(process -> smplock_pt), pt);
    return (OZ_EXQUOTANPP);
  }
  pagetable -> next        = *lpagetable;
  pagetable -> expand      = expand;
  pagetable -> maxpages    = maxpages;
  pagetable -> basepage    = basepage;
  pagetable -> secmaps_asc = NULL;
  pagetable -> secmaps_des = NULL;
  *lpagetable = pagetable;

  oz_hw_smplock_clr (&(process -> smplock_pt), pt);

  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Delete a page table from current process				*/
/*									*/
/*    Input:								*/
/*									*/
/*	vpage    = virtual page number anywhere in table		*/
/*	smp lock = none or softint					*/
/*									*/
/*    Output:								*/
/*									*/
/*	page table deleted from process					*/
/*									*/
/************************************************************************/

void oz_knl_process_deletepagetable (OZ_Mempage vpage)

{
  int si;
  OZ_Process *process;
  Pagetable **lpagetable, *pagetable;
  Secmap *secmap;
  uLong pt, sts;

getlock:
  si = oz_hw_cpu_setsoftint (0);
  process = curprocesses[oz_hw_cpu_getcur()];
  pt = oz_hw_smplock_wait (&(process -> smplock_pt));
  if (si) pt = OZ_SMPLOCK_NULL;

  /* Get the page table pointer corresponding to the given address */

  for (lpagetable = &(process -> pagetables); (pagetable = *lpagetable) != NULL; lpagetable = &(pagetable -> next)) {
    if (vpage < pagetable -> basepage) continue;
    if (vpage < pagetable -> basepage + pagetable -> maxpages) break;
  }
  if (pagetable == NULL) goto unlkrtn;

  /* If there are any sections mapped to the pagetable, unmap the first one and repeat */

  secmap = pagetable -> secmaps_asc;
  if (secmap != NULL) {
    vpage = secmap -> vpage;
    if (secmap -> npages == 0) oz_crash ("oz_knl_process_deletepagetable: section mapped at %u has npages zero", vpage);
    if (pagetable -> maxpages == 0) oz_crash ("oz_knl_process_deletepagetable: pagetable at %u with section mapped at %u has npages zero", pagetable -> basepage, vpage);
    oz_hw_smplock_clr (&(process -> smplock_pt), pt);
    sts = oz_knl_process_unmapsec (vpage);
    if (sts != OZ_SUCCESS) oz_knl_printk ("oz_knl_process_deletepagetable: error %u unmapping section at vpage %u", sts, vpage);
    goto getlock;
  }

  /* Unlink and delete the page table */

  *lpagetable = pagetable -> next;
  OZ_KNL_NPPFREE (pagetable);

unlkrtn:
  oz_hw_smplock_clr (&(process -> smplock_pt), pt);
}

/************************************************************************/
/*									*/
/*  Map a section to current process' address space			*/
/*									*/
/*    Input:								*/
/*									*/
/*	section = pointer to section block				*/
/*	*npagem = number of pages to map				*/
/*	*svpage = starting virtual page					*/
/*	          (always the low address, even in EXPDN pagetables)	*/
/*	mapsecflags = OZ_MAPSECTION_... flags				*/
/*		OZ_MAPSECTION_EXACT  = must be at exact svpage given	*/
/*		OZ_MAPSECTION_SYSTEM = this is the system page table map
/*		OZ_MAPSECTION_ATBEG  = map to lowest address, even in EXPDN pagetables
/*		OZ_MAPSECTION_ATEND  = map to highest address, even in EXPUP pagetables
/*	ownermode = owner's processor mode				*/
/*	pageprot  = page protection code				*/
/*									*/
/*	smp lock = none or softint					*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_process_mapsection = OZ_SUCCESS : successful		*/
/*	                                         *npagem = number of pages mapped
/*	                                         *svpage = starting virtual page
/*	                          OZ_ADDRINUSE : explicit address already occupied
/*	                      OZ_ADDRSPACEFULL : process occupies all virtual address space already
/*	                     OZ_PRIVSECMULTMAP : trying to map private section more than once
/*	                                  else : other error status	*/
/*									*/
/************************************************************************/

uLong oz_knl_process_mapsection (OZ_Section *section, 
                                 OZ_Mempage *npagem, 
                                 OZ_Mempage *svpage, 
                                 uLong mapsecflags, 
                                 OZ_Procmode ownermode, 
                                 OZ_Hw_pageprot pageprot)

{
  int si;
  OZ_Mempage npages, vpage;
  Pagetable *pagetable;
  OZ_Pagetable_expand expand;
  OZ_Process *process;
  Secmap *hsecmap, *lsecmap, *secmap;
  uLong pt, sts;

  OZ_KNL_CHKOBJTYPE (section, OZ_OBJTYPE_SECTION);

  /* Get number of pages in section */

  npages = oz_knl_section_getsecnpages (section);
  if (npages == 0) oz_crash ("oz_knl_process_mapsection: section has npages zero\n");

  /* Get starting virtual page number to map the section's lowest address at */

  vpage = *svpage;

  /* If vpage is in system address space, use the system process, otherwise, use the current process */

  process = oz_s_systemproc;
  if (!OZ_HW_ISSYSPAGE (vpage)) {
    si = oz_hw_cpu_setsoftint (0);
    process = curprocesses[oz_hw_cpu_getcur()];
    oz_hw_cpu_setsoftint (si);
  }

  /* Find corresponding pagetable */

  pt = oz_hw_smplock_wait (&(process -> smplock_pt));

  pagetable = find_pagetable (process, vpage);
  if (pagetable == NULL) goto nopagetable;

  /* Find where secmap goes in sorted secmap list */

  sts = find_secmap_spot (pagetable, mapsecflags, npages, vpage, &lsecmap, &hsecmap, &vpage);
  if (sts != OZ_SUCCESS) {
    oz_hw_smplock_clr (&(process -> smplock_pt), pt);
    return (sts);
  }

  /* hsecmap = points to secmap at higher address */
  /* lsecmap = points to secmap at lower address */

  /* Allocate section map block */

  secmap = OZ_KNL_NPPMALLOQ (sizeof *secmap);
  if (secmap == NULL) {
    oz_hw_smplock_clr (&(process -> smplock_pt), pt);
    return (OZ_EXQUOTANPP);
  }

  /* Can only map a private section once, because the page state information is kept in the hardware */
  /* pagetable, so each page of the section can have only one hardware page table entry.  If it had  */
  /* two, then it could have two different sets of state information, which would make a big mess.   */
  /* Only do this if success is guaranteed below, because it sets a bit in the section block saying  */
  /* the private section is now mapped by the process.                                               */

  sts = oz_knl_section_mapproc (section, process);
  if (sts != OZ_SUCCESS) {
    oz_hw_smplock_clr (&(process -> smplock_pt), pt);
    OZ_KNL_NPPFREE (secmap);
    return (sts);
  }

  /* Fill in section map block */

  secmap -> npages      = npages;
  secmap -> vpage       = vpage;
  secmap -> section     = section;
  secmap -> ownermode   = ownermode;
  secmap -> pageprot    = pageprot;
  secmap -> mapsecflags = mapsecflags;
  secmap -> nseclocks   = 0;
  secmap -> nseclkwzs   = 0;
  secmap -> seclocks    = NULL;
  secmap -> seclkwzs    = NULL;

  oz_knl_section_increfc (section, 1);

  if (secmap -> npages == 0) oz_crash ("oz_knl_process_mapsection: secmap npages is zero");

  /* Link it to the pagetable */

  link_secmap_to_pagetable (secmap, pagetable, hsecmap, lsecmap);

  /* If in system address space, decrement free page counter */

  if (process == oz_s_systemproc) oz_s_sysmem_pagtblfr -= npages;

  /* Release smp lock */

  oz_hw_smplock_clr (&(process -> smplock_pt), pt);

  /* Set the page protection codes */

  if (!(mapsecflags & OZ_MAPSECTION_SYSTEM)) {
    sts = oz_knl_section_setpageprot (npages, vpage, pageprot, section, NULL);
    if (sts != OZ_SUCCESS) {
      oz_knl_process_unmapsec (vpage);
      return (sts);
    }
  }

  /* Return number of pages and starting virtual page */

  *npagem = npages;
  *svpage = vpage;
  return (OZ_SUCCESS);

  /* No page table for address range */

nopagetable:
  oz_hw_smplock_clr (&(process -> smplock_pt), pt);
  return (OZ_NOPAGETABLE);

  /* Requested address is already mapped to a section */

addrinuse:
  oz_hw_smplock_clr (&(process -> smplock_pt), pt);
  return (OZ_ADDRINUSE);

  /* Pagetable cannot be expanded sufficiently to hold section */

addrspacefull:
  oz_hw_smplock_clr (&(process -> smplock_pt), pt);
  return (OZ_ADDRSPACEFULL);
}

/************************************************************************/
/*									*/
/*  Map a list of sections to current process' address space		*/
/*									*/
/*    Input:								*/
/*									*/
/*	mapsecflags  = OZ_MAPSECTION_... flags				*/
/*	nsections    = number of elements in mapsecparams array		*/
/*	mapsecparams = map section parameters array			*/
/*	 -> section   = pointer to section block			*/
/*	 -> svpage    = starting virtual page to map section at		*/
/*	 -> ownermode = owner's processor mode				*/
/*	 -> pageprot  = page protection code				*/
/*									*/
/*	smp lock = none or softint					*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_process_mapsections = OZ_SUCCESS : successful		*/
/*	                                         *npagem = number of pages mapped
/*	                                         *svpage = starting virtual page
/*	                          OZ_ADDRINUSE : explicit address already occupied
/*	                      OZ_ADDRSPACEFULL : process occupies all virtual address space already
/*	                     OZ_PRIVSECMULTMAP : trying to map private section more than once
/*	                                  else : other error status	*/
/*									*/
/*    Note:								*/
/*									*/
/*	Either all the sections are mapped or none are			*/
/*	Relative position of all sections is maintained, even for 	*/
/*	  non-exact mappings						*/
/*									*/
/************************************************************************/

uLong oz_knl_process_mapsections (uLong mapsecflags, int nsections, OZ_Mapsecparam *mapsecparams)

{
  int i, j, si;
  OZ_Mempage npages, start_vpage, total_npages, vpage;
  Pagetable *pagetable, *pagetable0;
  OZ_Pagetable_expand expand;
  OZ_Process *process;
  Secmap *allsecmaps, *hsecmap, *lsecmap, *secmap;
  OZ_Section *section;
  uLong pt, sts;

  if (nsections == 0) return (OZ_SUCCESS);
  if (nsections == 1) {
    return (oz_knl_process_mapsection (mapsecparams -> section, 
                                     &(mapsecparams -> npagem), 
                                     &(mapsecparams -> svpage), 
                                       mapsecflags, 
                                       mapsecparams -> ownermode, 
                                       mapsecparams -> pageprot));
  }

  allsecmaps = NULL;

  /* Get number of pages in each section */

  for (i = 0; i < nsections; i ++) {
    npages = oz_knl_section_getsecnpages (mapsecparams[i].section);
    if (npages == 0) oz_crash ("oz_knl_process_mapsection: section has npages zero\n");
    if (npages + mapsecparams[i].svpage < npages) return (OZ_SECMAPADDRWRAPS);
    mapsecparams[i].npagem = npages;
  }

  /* Check for overlaps */

  for (i = 0; i < nsections; i ++) {
    for (j = i; ++ j < nsections;) {
      if (mapsecparams[i].svpage < mapsecparams[j].svpage) {
        if (mapsecparams[i].svpage + mapsecparams[i].npagem > mapsecparams[j].svpage) return (OZ_SECMAPOVERLAP);
      } else {
        if (mapsecparams[j].svpage + mapsecparams[j].npagem > mapsecparams[i].svpage) return (OZ_SECMAPOVERLAP);
      }
    }
  }

  /* Get starting virtual page number to map the section's lowest address at */

  start_vpage = mapsecparams[0].svpage;
  for (i = 0; i < nsections; i ++) {
    if (mapsecparams[i].svpage < start_vpage) start_vpage = mapsecparams[i].svpage;
  }

  /* Get total number of pages to map, including any gaps */

  total_npages = 0;
  for (i = 0; i < nsections; i ++) {
    if (mapsecparams[i].npagem + mapsecparams[i].svpage > total_npages + start_vpage) {
      total_npages = mapsecparams[i].npagem + mapsecparams[i].svpage - start_vpage;
    }
  }

  /* Allocate section map blocks */

  sts = OZ_EXQUOTANPP;
  for (i = 0; i < nsections; i ++) {
    secmap = OZ_KNL_NPPMALLOQ (sizeof *secmap);
    if (secmap == NULL) goto freeallsecmaps;
    secmap -> next_asc = allsecmaps;
    allsecmaps = secmap;
  }

  /* If start_vpage is in system address space, use the system process, otherwise, use the current process */

  process = oz_s_systemproc;
  if (!OZ_HW_ISSYSPAGE (start_vpage)) {
    si = oz_hw_cpu_setsoftint (0);
    process = curprocesses[oz_hw_cpu_getcur()];
    oz_hw_cpu_setsoftint (si);
  }

  /* Find corresponding pagetable */

  pt = oz_hw_smplock_wait (&(process -> smplock_pt));

  pagetable = find_pagetable (process, start_vpage);
  sts = OZ_NOPAGETABLE;
  if (pagetable == NULL) goto freeallsecmaps_locked;

  /* See if there is room in the pagetable for all the sections, including any gaps */

  sts = find_secmap_spot (pagetable, mapsecflags, total_npages, start_vpage, &lsecmap, &hsecmap, &vpage);
  if (sts != OZ_SUCCESS) goto freeallsecmaps_locked;
  start_vpage = vpage - start_vpage;			// offset for all sections being mapped
							// will be zero if OZ_MAPSECFLAG_EXACT is set
							// else, it is displacement from prototype vpage numbers given

  /* Can only map a private section once, because the page state information is kept in the hardware */
  /* pagetable, so each page of the section can have only one hardware page table entry.  If it had  */
  /* two, then it could have two different sets of state information, which would make a big mess.   */
  /* Only do this if success is guaranteed below, because it sets a bit in the section block saying  */
  /* the private section is now mapped by the process.                                               */

  for (i = 0; i < nsections; i ++) {
    sts = oz_knl_section_mapproc (mapsecparams[i].section, process);
    if (sts != OZ_SUCCESS) {
      while (i > 0) oz_knl_section_unmapproc (mapsecparams[--i].section, process);
      goto freeallsecmaps_locked;
    }
  }

  /* Fill in section map blocks */

  for (i = 0; i < nsections; i ++) {
    secmap = allsecmaps;
    allsecmaps = secmap -> next_asc;

    secmap -> npages      = npages  = mapsecparams[i].npagem;
    secmap -> vpage       = (mapsecparams[i].svpage += start_vpage);
    secmap -> section     = section = mapsecparams[i].section;
    secmap -> ownermode   = mapsecparams[i].ownermode;
    secmap -> pageprot    = mapsecparams[i].pageprot;
    secmap -> mapsecflags = mapsecflags;
    secmap -> nseclocks   = 0;
    secmap -> nseclkwzs   = 0;
    secmap -> seclocks    = NULL;
    secmap -> seclkwzs    = NULL;

    if (secmap -> npages == 0) oz_crash ("oz_knl_process_mapsection: secmap npages is zero");

    oz_knl_section_increfc (section, 1);

    validate_secmaps (pagetable);

    sts = find_secmap_spot (pagetable, OZ_MAPSECTION_EXACT, secmap -> npages, secmap -> vpage, &lsecmap, &hsecmap, &vpage);
    if (sts != OZ_SUCCESS) oz_crash ("oz_knl_process_mapsections: error %u finding secmap spot", sts);
    if (vpage != secmap -> vpage) oz_crash ("oz_knl_process_mapsections: mapped at vpage %x instead of %x", vpage, secmap -> vpage);

    link_secmap_to_pagetable (secmap, pagetable, hsecmap, lsecmap);

    /* If in system address space, decrement free spte counter */

    if (process == oz_s_systemproc) oz_s_sysmem_pagtblfr -= npages;
  }

  /* Release smp lock */

  oz_hw_smplock_clr (&(process -> smplock_pt), pt);

  /* Set the page protection codes */

  if (!(mapsecflags & OZ_MAPSECTION_SYSTEM)) {
    for (i = 0; i < nsections; i ++) {
      sts = oz_knl_section_setpageprot (mapsecparams[i].npagem, 	// number of pages mapped
                                        mapsecparams[i].svpage, 	// starting virtual page number
                                        mapsecparams[i].pageprot, 	// the protection we want
                                        mapsecparams[i].section, 	// this is initial mapping of this section
                                        NULL);
      if (sts != OZ_SUCCESS) {
        while (i > 0) oz_knl_process_unmapsec (mapsecparams[--i].svpage);
        return (sts);
      }
    }
  }

  return (OZ_SUCCESS);

  /* Error, free allocated secmaps and return error status */

freeallsecmaps_locked:
  oz_hw_smplock_clr (&(process -> smplock_pt), pt);
freeallsecmaps:
  while ((secmap = allsecmaps) != NULL) {
    allsecmaps = secmap -> next_asc;
    OZ_KNL_NPPFREE (secmap);
  }
  return (sts);
}

/************************************************************************/
/*									*/
/*  Find the pagetable that a given virtual page is in			*/
/*									*/
/************************************************************************/

static Pagetable *find_pagetable (OZ_Process *process, OZ_Mempage vpage)

{
  Pagetable *pagetable;
  OZ_Mempage hivpage, lovpage;

  for (pagetable = process -> pagetables; pagetable != NULL; pagetable = pagetable -> next) {
    validate_secmaps (pagetable);
    lovpage = pagetable -> basepage;			/* lowest possible vpage is given by pagetable->basepage */
    hivpage = lovpage + pagetable -> maxpages;		/* highest possible vpage is given by vpage+maxpages */
    if ((vpage >= lovpage) && (vpage < hivpage)) break;	/* if desired vpage is in range, this is the correct pagetable */
  }
  if (pagetable == NULL) {
    oz_knl_printk ("oz_knl_process_mapsection*: no pt to map pages at %x, process %p\n", vpage, process);
    for (pagetable = process -> pagetables; pagetable != NULL; pagetable = pagetable -> next) {
      oz_knl_printk ("oz_knl_process_mapsection*: %x page pt at %x\n", pagetable -> maxpages, pagetable -> basepage);
    }
  }
  return (pagetable);
}

/************************************************************************/
/*									*/
/*  Find spot in pagetable for a secmap of the given size		*/
/*									*/
/*    Input:								*/
/*									*/
/*	pagetable   = pagetable to search				*/
/*	mapsecflags = mapping flags					*/
/*	npages      = size of section in pages				*/
/*	vpage       = starting virtual page number			*/
/*	smplevel    = pt is locked					*/
/*									*/
/*    Output:								*/
/*									*/
/*	find_secmap_spot = OZ_SUCCESS : successful			*/
/*	                         else : error status			*/
/*	*lsecmap_r = points to previous secmap in the pagetable		*/
/*	*hsecmap_r = points to next secmap in pagetable			*/
/*	*vpage_r   = the virtual page it is to be mapped at		*/
/*									*/
/************************************************************************/

static uLong find_secmap_spot (Pagetable *pagetable, uLong mapsecflags, OZ_Mempage npages, OZ_Mempage vpage, 
                               Secmap **lsecmap_r, Secmap **hsecmap_r, OZ_Mempage *vpage_r)

{
  OZ_Mempage lovpage, hivpage;
  OZ_Pagetable_expand expand;
  Secmap *hsecmap, *lsecmap, *secmap;

  lovpage = pagetable -> basepage;				/* lowest possible vpage is given by pagetable->basepage */
  hivpage = lovpage + pagetable -> maxpages;			/* highest possible vpage is given by basepage+maxpages */

  lsecmap = NULL;
  expand  = pagetable -> expand;
  if (mapsecflags & OZ_MAPSECTION_EXACT) {
    if (vpage + npages < vpage) goto addrspacefull;

    /* Exact start address given, make sure nothing overlaps it */

    switch (expand) {
      case OZ_PAGETABLE_EXPUP: {
        lsecmap = NULL;
        for (hsecmap = pagetable -> secmaps_asc; hsecmap != NULL; hsecmap = hsecmap -> next_asc) {
          if (vpage + npages <= hsecmap -> vpage) break;
          if (vpage < hsecmap -> vpage + hsecmap -> npages) goto addrinuse;
          lsecmap = hsecmap;
        }
        break;
      }
      case OZ_PAGETABLE_EXPDN: {
        hsecmap = NULL;
        for (lsecmap = pagetable -> secmaps_des; lsecmap != NULL; lsecmap = lsecmap -> next_des) {
          if (vpage >= lsecmap -> vpage + lsecmap -> npages) break;
          if (vpage + npages > lsecmap -> vpage) goto addrinuse;
          hsecmap = lsecmap;
        }
        break;
      }
      default: {
        oz_crash ("oz_knl_process_mapsection: bad pagetable -> expand %d", expand);
      }
    }

  } else {

    /* None given, find first empty space that is large enough */

    if (mapsecflags & OZ_MAPSECTION_ATBEG) expand = OZ_PAGETABLE_EXPUP;
    if (mapsecflags & OZ_MAPSECTION_ATEND) expand = OZ_PAGETABLE_EXPDN;

    switch (expand) {
      case OZ_PAGETABLE_EXPUP: {
        vpage = lovpage;						/* start with lowest possible page number */
        lsecmap = NULL;
        for (hsecmap = pagetable -> secmaps_asc; hsecmap != NULL; hsecmap = hsecmap -> next_asc) {
          if (vpage >= hivpage) goto addrspacefull;			/* check for page table overflow */
          if (hivpage - vpage < npages) goto addrspacefull;		/* make sure it wouldn't go off end of page table */
          if (vpage + npages <= hsecmap -> vpage) break;		/* if new would end at or before next begins, put new before next */
          vpage = hsecmap -> vpage + hsecmap -> npages;			/* otherwise, try putting new right after next one */
          if (vpage < hsecmap -> vpage) goto addrspacefull;		/* check for aritmetic overflow */
									/* (the only overflow we should ever get is wrapping around to zero) */
          lsecmap = hsecmap;
        }
        break;
      }
      case OZ_PAGETABLE_EXPDN: {
        vpage = hivpage;						/* start with highest possible page number */
        hsecmap = NULL;
        for (lsecmap = pagetable -> secmaps_des; lsecmap != NULL; lsecmap = lsecmap -> next_des) {
          if (vpage < lovpage) goto addrspacefull;			/* check for page table overflow */
          if (vpage - lovpage < npages) goto addrspacefull;		/* make sure it wouldn't go off end of page table */
          if (vpage - npages >= lsecmap -> vpage + lsecmap -> npages) break; /* if new would begin at or after lower one ends, put new before lower */
          vpage = lsecmap -> vpage;					/* otherwise, try putting new right before lower one */
          hsecmap = lsecmap;
        }
        vpage -= npages;						/* point to beginning of block */
        break;
      }
      default: {
        oz_crash ("oz_knl_process_mapsection: bad pagetable -> expand %d", expand);
      }
    }
  }

  *lsecmap_r = lsecmap;
  *hsecmap_r = hsecmap;
  *vpage_r   = vpage;
  return (OZ_SUCCESS);

  /* Requested address is already mapped to a section */

addrinuse:
  return (OZ_ADDRINUSE);

  /* Pagetable cannot be expanded sufficiently to hold section */

addrspacefull:
  return (OZ_ADDRSPACEFULL);
}

/************************************************************************/
/*									*/
/*  Link a secmap block to a pagetable					*/
/*									*/
/*    Input:								*/
/*									*/
/*	secmap = secmap being inserted					*/
/*	pagetable = pagetable it is being linked to			*/
/*	hsecmap = next higher addressed secmap				*/
/*	lsecmap = next lower addressed secmap				*/
/*	smplock = the pagetable's pt is locked				*/
/*									*/
/************************************************************************/

static void link_secmap_to_pagetable (Secmap *secmap, Pagetable *pagetable, Secmap *hsecmap, Secmap *lsecmap)

{
  validate_secmaps (pagetable);

  secmap -> next_asc = hsecmap;
  secmap -> next_des = lsecmap;
  if (lsecmap == NULL) pagetable -> secmaps_asc = secmap;
  else lsecmap -> next_asc = secmap;
  if (hsecmap == NULL) pagetable -> secmaps_des = secmap;
  else hsecmap -> next_des = secmap;

  validate_secmaps (pagetable);
}

/************************************************************************/
/*									*/
/*  Unmap a section from current process' address space			*/
/*									*/
/*    Input:								*/
/*									*/
/*	vpage = virtual page somewhere in the section			*/
/*									*/
/*	smp lock = none or softint					*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_process_unmapsec = OZ_SUCCESS : successful		*/
/*	                     OZ_ADDRNOTINUSE : nothing was there	*/
/*									*/
/************************************************************************/

uLong oz_knl_process_unmapsec (OZ_Mempage vpage)

{
  int keptptlock, si;
  OZ_Mempage npages, spage;
  Pagetable *pagetable;
  OZ_Process *process;
  Secmap *hsecmap, *lsecmap, *secmap;
  OZ_Section *section;
  uLong pt;

  /* If vpage is in system address space, use the system process, otherwise, use the current process */

  process = oz_s_systemproc;
  if (!OZ_HW_ISSYSPAGE (vpage)) {
    si = oz_hw_cpu_setsoftint (0);
    process = curprocesses[oz_hw_cpu_getcur()];
    oz_hw_cpu_setsoftint (si);
  }

  /* Find the pagetable the virtual address is in */

  pt = oz_hw_smplock_wait (&(process -> smplock_pt));

tryagain:
  for (pagetable = process -> pagetables; pagetable != NULL; pagetable = pagetable -> next) {
    validate_secmaps (pagetable);
    if (vpage < pagetable -> basepage) continue;
    if (vpage < pagetable -> basepage + pagetable -> maxpages) break;
  }
  if (pagetable == NULL) {
    oz_knl_printk ("oz_knl_process_unmapsec: no pagetable at vpage %x\n", vpage);
    goto addrnotused;
  }

  /* Find the section map the virtual address is in */

  lsecmap = NULL;
  for (secmap = pagetable -> secmaps_asc; secmap != NULL; secmap = secmap -> next_asc) {
    if (vpage < secmap -> vpage) continue;
    if (vpage < secmap -> vpage + secmap -> npages) break;
    lsecmap = secmap;
  }
  if (secmap == NULL) {
    oz_knl_printk ("oz_knl_process_unmapsec: no section mapped at vpage %x\n", vpage);
    goto addrnotused;
  }
  hsecmap = secmap -> next_asc;
  npages  = secmap -> npages;
  section = secmap -> section;
  vpage   = secmap -> vpage;

  /* Page out all the pages mapped to the section - note that thread can be aborted in here somewhere */

  keptptlock = 1;
  for (spage = 0; spage < npages; spage ++) keptptlock &= oz_knl_section_unmappage (vpage + spage, pt);

  /* If unmapping the pages released the lock, scan again to make sure it is all gone     */
  /* Eventually we get a clean sweep guaranteeing that we are not referencing the section */

  if (!keptptlock) goto tryagain;

  /* Now there should be no I/O going on the section initiated via this map */

  if ((secmap -> nseclocks != 0) || (secmap -> seclocks != NULL)) {
    oz_crash ("oz_knl_process_unmapsec: I/O still in progress on page, nseclocks %d, seclocks %p, npages %u, svpage %x", secmap -> nseclocks, secmap -> seclocks, npages, vpage);
  }
  if ((secmap -> nseclkwzs != 0) || (secmap -> seclkwzs != NULL)) {
     oz_crash ("oz_knl_process_unmapsec: something still waiting for I/O to finish, nseclkwzs %d, seclkwzs %p, npages %u, svpage %x", secmap -> nseclkwzs, secmap -> seclkwzs, npages, vpage);
  }

  /* Tell section stuff it is being unmapped - this clears the 'private section mapped' flag  */

  oz_knl_section_unmapproc (secmap -> section, process);

  /* Unlink the secmap from the pagetable list */

  if (lsecmap == NULL) pagetable -> secmaps_asc = hsecmap;
  else lsecmap -> next_asc = hsecmap;
  if (hsecmap == NULL) pagetable -> secmaps_des = lsecmap;
  else hsecmap -> next_des = lsecmap;

  validate_secmaps (pagetable);

  /* If in system address space, increment free page counter */

  if (process == oz_s_systemproc) oz_s_sysmem_pagtblfr += npages;

  /* Decrement section reference count and free off section map block */

  oz_hw_smplock_clr (&(process -> smplock_pt), pt);
  oz_knl_section_increfc (secmap -> section, -1);
  OZ_KNL_NPPFREE (secmap);
  return (OZ_SUCCESS);

addrnotused:
  oz_hw_smplock_clr (&(process -> smplock_pt), pt);
  return (OZ_ADDRNOTUSED);
}

static void validate_secmaps (Pagetable *pagetable)

{
#ifdef OZ_DEBUG
  OZ_Mempage hivpage, lovpage;
  Secmap *psecmap, *secmap;

  switch (pagetable -> expand) {
    case OZ_PAGETABLE_EXPUP: {
      lovpage = pagetable -> basepage;
      hivpage = lovpage + pagetable -> maxpages;
      break;
    }
    case OZ_PAGETABLE_EXPDN: {
      hivpage = pagetable -> basepage + pagetable -> maxpages;
      lovpage = hivpage - pagetable -> maxpages;
      break;
    }
    default: {
      oz_crash ("oz_knl_process validate_secmaps: bad pagetable -> expand %d", pagetable -> expand);
    }
  }

  psecmap = NULL;
  for (secmap = pagetable -> secmaps_asc; secmap != NULL; secmap = secmap -> next_asc) {
    if (secmap -> vpage < lovpage) oz_crash ("oz_knl_process validate_secmaps: section vpage %u lower than %u", secmap -> vpage, lovpage);
    if (secmap -> next_des != psecmap) oz_crash ("oz_knl_process validate_secmaps: section next_des %p out of order with %p", secmap -> next_des, psecmap);
    lovpage = secmap -> vpage + secmap -> npages;
    psecmap = secmap;
  }
  if (lovpage > hivpage) {
    oz_crash ("oz_knl_process validate_secmaps: secmap ending at %u overflows %u page pagetable based at %u", lovpage, hivpage);
  }
  if (psecmap != pagetable -> secmaps_des) oz_crash ("oz_knl_process validate_secmaps: pagetable secmaps_des %p out of order with %p", pagetable -> secmaps_des, psecmap);
#endif
}

/************************************************************************/
/*									*/
/*  Given a virtual page, determine the section and the page offset 	*/
/*  within the section							*/
/*									*/
/*    Input:								*/
/*									*/
/*	process = process						*/
/*	*svpage = starting virtual address for search			*/
/*									*/
/*	smp lock <= pt							*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_process_getsecfromvpage = 0 : address not mapped		*/
/*	                              else : number of pages mapped there
/*	*svpage   = starting virtual address found, .ge. input value	*/
/*	*section  = section mapped at that address			*/
/*	*pageoffs = page offset	in the section that is there		*/
/*	*pageprot = protection it was originally mapped with		*/
/*	*procmode = processor mode of owner				*/
/*									*/
/************************************************************************/

	/* only look at this exact page */

OZ_Mempage oz_knl_process_getsecfromvpage (OZ_Process *process, OZ_Mempage vpage, OZ_Section **section, OZ_Mempage *pageoffs, OZ_Hw_pageprot *pageprot, OZ_Procmode *procmode, uLong *mapsecflags)

{
  OZ_Mempage npages, svpage;

  svpage = vpage;
  npages = oz_knl_process_getsecfromvpage2 (process, &svpage, section, pageoffs, pageprot, procmode, mapsecflags);
  if (svpage > vpage) npages = 0;
  return (npages);
}

	/* search for one .ge. the given page */

OZ_Mempage oz_knl_process_getsecfromvpage2 (OZ_Process *process, OZ_Mempage *svpage, OZ_Section **section, OZ_Mempage *pageoffs, OZ_Hw_pageprot *pageprot, OZ_Procmode *procmode, uLong *mapsecflags)

{
  OZ_Mempage npages, vpage;
  Pagetable *pagetable;
  Secmap *secmap;
  uLong pt;

  if (process == NULL) oz_crash ("oz_knl_process_getsecfromvpage: being called with NULL process pointer");

  OZ_KNL_CHKOBJTYPE (process, OZ_OBJTYPE_PROCESS);

  /* Find which section maps the given page or greater */

  vpage = *svpage;

  pt = oz_hw_smplock_wait (&(process -> smplock_pt));

  for (pagetable = process -> pagetables; pagetable != NULL; pagetable = pagetable -> next) {
    if (vpage < pagetable -> basepage + pagetable -> maxpages) {
      for (secmap = pagetable -> secmaps_asc; secmap != NULL; secmap = secmap -> next_asc) {
        if (vpage < secmap -> vpage + secmap -> npages) {

          /* Found it, return section pointer and page within section, and the protection it was mapped with */

          if (vpage < secmap -> vpage) *svpage = vpage = secmap -> vpage;
          *section     = secmap -> section;
          *pageoffs    = vpage - secmap -> vpage;
          *pageprot    = secmap -> pageprot;
          *procmode    = secmap -> ownermode;
          *mapsecflags = secmap -> mapsecflags;
          npages       = secmap -> vpage + secmap -> npages - vpage;
          oz_hw_smplock_clr (&(process -> smplock_pt), pt);
          return (npages);
        }
      }
    }
  }

  /* Could not find it, return failure status */

  oz_hw_smplock_clr (&(process -> smplock_pt), pt);
  return (0);
}

/************************************************************************/
/*									*/
/*  Lock and unlock process' page tables				*/
/*									*/
/************************************************************************/

uLong oz_knl_process_lockpt (OZ_Process *process)

{
  OZ_KNL_CHKOBJTYPE (process, OZ_OBJTYPE_PROCESS);
  return (oz_hw_smplock_wait (&(process -> smplock_pt)));
}

void oz_knl_process_unlkpt (OZ_Process *process, uLong pt)

{
  OZ_KNL_CHKOBJTYPE (process, OZ_OBJTYPE_PROCESS);
  oz_hw_smplock_clr (&(process -> smplock_pt), pt);
}

/************************************************************************/
/*									*/
/*  Find the seclock listhead that pertains to the given range of 	*/
/*  virtual pages							*/
/*									*/
/*    Input:								*/
/*									*/
/*	process = process to search					*/
/*	npages  = number of virtual pages				*/
/*	svpage  = starting virtual page number				*/
/*	smplock = pt							*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_process_seclocks = NULL : no section mapped there	*/
/*	                          else : pointer to seclock listhead	*/
/*									*/
/************************************************************************/

OZ_Seclock **oz_knl_process_seclocks (OZ_Process *process, OZ_Mempage npages, OZ_Mempage svpage)

{
  Pagetable *pagetable;
  Secmap *secmap;

  OZ_KNL_CHKOBJTYPE (process, OZ_OBJTYPE_PROCESS);

  for (pagetable = process -> pagetables; pagetable != NULL; pagetable = pagetable -> next) {
    if ((svpage >= pagetable -> basepage) && (svpage + npages <= pagetable -> basepage + pagetable -> maxpages)) {
      for (secmap = pagetable -> secmaps_asc; secmap != NULL; secmap = secmap -> next_asc) {
        if ((svpage >= secmap -> vpage) && (svpage + npages <= secmap -> vpage + secmap -> npages)) {
          return (&(secmap -> seclocks));
        }
      }
    }
  }

  return (NULL);
}

int oz_knl_process_nseclocks (OZ_Process *process, OZ_Mempage vpage, int inc)

{
  Pagetable *pagetable;
  Secmap *secmap;

  OZ_KNL_CHKOBJTYPE (process, OZ_OBJTYPE_PROCESS);

  if (oz_hw_smplock_cpu (&(process -> smplock_pt)) != oz_hw_cpu_getcur ()) oz_crash ("oz_knl_process_nseclocks: pt locked not owned");

  for (pagetable = process -> pagetables; pagetable != NULL; pagetable = pagetable -> next) {
    if ((vpage >= pagetable -> basepage) && (vpage - pagetable -> basepage < pagetable -> maxpages)) {
      for (secmap = pagetable -> secmaps_asc; secmap != NULL; secmap = secmap -> next_asc) {
        if ((vpage >= secmap -> vpage) && (vpage - secmap -> vpage < secmap -> npages)) {
          secmap -> nseclocks += inc;
          if (secmap -> nseclocks < 0) oz_crash ("oz_knl_process_nseclocks: count %d", secmap -> nseclocks);
          return (secmap -> nseclocks);
        }
      }
    }
  }

  oz_crash ("oz_knl_process_nseclocks: vpage %X not found", vpage);
  return (-999);
}

/************************************************************************/
/*									*/
/*  Find the seclkwz listhead that pertains to the given virtual page	*/
/*									*/
/*    Input:								*/
/*									*/
/*	process = process to search					*/
/*	vpage   = virtual page number					*/
/*	smplock = pt							*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_process_seclkwzs = NULL : no section mapped there	*/
/*	                          else : pointer to seclkwz listhead	*/
/*									*/
/************************************************************************/

OZ_Seclkwz **oz_knl_process_seclkwzs (OZ_Process *process, OZ_Mempage vpage)

{
  Pagetable *pagetable;
  Secmap *secmap;

  OZ_KNL_CHKOBJTYPE (process, OZ_OBJTYPE_PROCESS);

  for (pagetable = process -> pagetables; pagetable != NULL; pagetable = pagetable -> next) {
    if ((vpage >= pagetable -> basepage) && (vpage < pagetable -> basepage + pagetable -> maxpages)) {
      for (secmap = pagetable -> secmaps_asc; secmap != NULL; secmap = secmap -> next_asc) {
        if ((vpage >= secmap -> vpage) && (vpage < secmap -> vpage + secmap -> npages)) {
          return (&(secmap -> seclkwzs));
        }
      }
    }
  }

  return (NULL);
}

int oz_knl_process_nseclkwzs (OZ_Process *process, OZ_Mempage vpage, int inc)

{
  Pagetable *pagetable;
  Secmap *secmap;

  OZ_KNL_CHKOBJTYPE (process, OZ_OBJTYPE_PROCESS);

  if (oz_hw_smplock_cpu (&(process -> smplock_pt)) != oz_hw_cpu_getcur ()) oz_crash ("oz_knl_process_nseclkwzs: pt locked not owned");

  for (pagetable = process -> pagetables; pagetable != NULL; pagetable = pagetable -> next) {
    if ((vpage >= pagetable -> basepage) && (vpage - pagetable -> basepage < pagetable -> maxpages)) {
      for (secmap = pagetable -> secmaps_asc; secmap != NULL; secmap = secmap -> next_asc) {
        if ((vpage >= secmap -> vpage) && (vpage - secmap -> vpage < secmap -> npages)) {
          secmap -> nseclkwzs += inc;
          if (secmap -> nseclkwzs < 0) oz_crash ("oz_knl_process_nseclkwzs: count %d", secmap -> nseclkwzs);
          return (secmap -> nseclkwzs);
        }
      }
    }
  }

  oz_crash ("oz_knl_process_nseclkwzs: vpage %X not found", vpage);
  return (-999);
}

/************************************************************************/
/*									*/
/*  Lock and unlock process' state data					*/
/*									*/
/************************************************************************/

uLong oz_knl_process_lockps (OZ_Process *process)

{
  OZ_KNL_CHKOBJTYPE (process, OZ_OBJTYPE_PROCESS);
  return (oz_hw_smplock_wait (&(process -> smplock_ps)));
}

void oz_knl_process_unlkps (OZ_Process *process, uLong ps)

{
  OZ_KNL_CHKOBJTYPE (process, OZ_OBJTYPE_PROCESS);
  oz_hw_smplock_clr (&(process -> smplock_ps), ps);
}

/************************************************************************/
/*									*/
/*  Dump all processes							*/
/*									*/
/************************************************************************/

void oz_knl_process_dump_all (void)

{
  OZ_Process *lprocess, *process;
  uLong pr;

  lprocess = NULL;							// don't have a process locked yet
  pr = oz_hw_smplock_wait (&smplock_pr);				// lock the list
  for (process = processes; process != NULL; process = process -> next) {
    oz_knl_process_increfc (process, 1);				// keep it from leaving 'processes' list
    oz_hw_smplock_clr (&smplock_pr, pr);				// release PR lock so process_dump can lock PS
    if (lprocess != NULL) oz_knl_process_increfc (lprocess, -1);	// let last one die now if it wants
    lprocess = process;							// remember which process we have locked now
    oz_knl_process_dump (process, 1);					// dump the process
    pr = oz_hw_smplock_wait (&smplock_pr);				// get list lock back
  }
  oz_hw_smplock_clr (&smplock_pr, pr);					// done scanning list
  if (lprocess != NULL) oz_knl_process_increfc (lprocess, -1);		// let last one die now if it wants
}

/************************************************************************/
/*									*/
/*  Dump a single process						*/
/*									*/
/************************************************************************/

void oz_knl_process_dump (OZ_Process *process, int dothreads)

{
  char cpulist[12];
  int i, j;
  uLong ps;

  j = 0;
  for (i = 0; (i < oz_s_cpucount) && (i < 10); i ++) {
    if (!(oz_s_cpusavail & (1 << i))) continue;
    cpulist[j] = ' ';
    if (curprocesses[i] == process) cpulist[j] = i + '0';
    j ++;
  }
  cpulist[j] = 0;

  oz_knl_printk ("  %8p: %3d <%s> %s\n", process, process -> refcount, cpulist, process -> name);
  if (dothreads) oz_knl_thread_dump_process (process);
}

void oz_knl_process_dump_secmaps (OZ_Process *process)

{
  int si;
  Pagetable *pagetable;
  Secmap *secmap;
  uLong pt;

  if (process == NULL) {
    si = oz_hw_cpu_setsoftint (0);
    process = curprocesses[oz_hw_cpu_getcur()];
    oz_hw_cpu_setsoftint (si);
  }

  OZ_KNL_CHKOBJTYPE (process, OZ_OBJTYPE_PROCESS);

  pt = oz_hw_smplock_wait (&(process -> smplock_pt));

  oz_knl_printk ("oz_knl_process_dump_secmaps: pagetables for %p (%s):\n", process, process -> name);

  for (pagetable = process -> pagetables; pagetable != NULL; pagetable = pagetable -> next) {
    oz_knl_printk ("  pagetable: %8.8X pages at %8.8X\n", pagetable -> maxpages, pagetable -> basepage);
    for (secmap = pagetable -> secmaps_asc; secmap != NULL; secmap = secmap -> next_asc) {
      oz_knl_printk ("     secmap: %8.8X pages at %8.8X, ownermode %d, pageprot %d, flags %X\n", 
	secmap -> npages, secmap -> vpage, secmap -> ownermode, 
	secmap -> pageprot, secmap -> mapsecflags);
    }
  }

  oz_hw_smplock_clr (&(process -> smplock_pt), pt);
}

/************************************************************************/
/*									*/
/*  Validate list of processes in the system				*/
/*									*/
/*    Input:								*/
/*									*/
/*	smplevel = ts							*/
/*									*/
/************************************************************************/

void oz_knl_process_validateall (void)

{
#ifdef OZ_DEBUG
  int np;
  Long cpucount;
  OZ_Job *job;
  OZ_Process *process;
  uLong cpusactive;

  np = 0;

  /* Make a bitmask of cpus that have a current process */

  cpusactive = 0;
  if (curprocesses != NULL) {
    for (cpucount = 0; cpucount < oz_s_cpucount; cpucount ++) {
      if (curprocesses[cpucount] != NULL) cpusactive |= (1 << cpucount);
    }
  }

  /* Loop through all processes we know about */

  for (process = processes; process != NULL; process = process -> next) {

    /* Check out to make sure the fields are sane */

    OZ_KNL_CHKOBJTYPE (process, OZ_OBJTYPE_PROCESS);
    if (process -> refcount < 0) oz_crash ("oz_knl_process_validateall: %p bad refcount %d", process, process -> refcount);
    if ((process -> refcount == 0) && (process -> hwctx_state != HWCTX_STATE_OFF))  oz_crash ("oz_knl_process_validateall: %p zero refcount, but hwctx_state %d", process, process -> hwctx_state);
    OZ_KNL_CHKOBJTYPE (process -> job, OZ_OBJTYPE_JOB);
    OZ_KNL_CHKOBJTYPE (process -> handletbl, OZ_OBJTYPE_HANDLETBL);
    OZ_KNL_CHKOBJTYPE (process -> threadq, OZ_OBJTYPE_THREAD);
    OZ_KNL_CHKOBJTYPE (process -> lognamdir, OZ_OBJTYPE_LOGNAME);
    OZ_KNL_CHKOBJTYPE (process -> lognamtbl, OZ_OBJTYPE_LOGNAME);
    OZ_KNL_CHKOBJTYPE (process -> logname, OZ_OBJTYPE_LOGNAME);
    OZ_KNL_CHKOBJTYPE (process -> secattr, OZ_OBJTYPE_SECATTR);
    if (oz_hw_smplock_level (&(process -> smplock_pt)) != OZ_SMPLOCK_LEVEL_PT) oz_crash ("oz_knl_process_validateall: %p bad smplock_pt level %u", process, oz_hw_smplock_level (&(process -> smplock_pt)));
    if (process -> name[OZ_PROCESS_NAMESIZE-1] != 0) oz_crash ("oz_knl_process_validateall: %p bad name", process);

    /* The hardware context should be ok, too */

    switch (process -> hwctx_state) {
      case HWCTX_STATE_NORMAL:		// hwctx should be valid for these states
      case HWCTX_STATE_CLEANIP: oz_hw_process_validate (process -> hw_ctx, process);
      case HWCTX_STATE_OFF:		// hwctx is most likely not valid for these states
      case HWCTX_STATE_TERMIP:
      case HWCTX_STATE_INITIP: break;
      default: oz_crash ("oz_knl_process_validateall: %p bad hwctx_state %d", process, process -> hwctx_state);
    }

    /* If it is a current process, clear the bit so we know we found this one */

    if (cpusactive != 0) {
      for (cpucount = 0; cpucount < oz_s_cpucount; cpucount ++) {
        if (process == curprocesses[cpucount]) cpusactive &= ~(1 << cpucount);
      }
    }

    /* Count the process so we know how many we found */

    np ++;
  }

  /* We should know about the correct number of processes */

  if (np != numprocesses) oz_crash ("oz_knl_process_validateall: found %d processes instead of %d", np, numprocesses);

  /* We should have found all current processes */

  if (cpusactive != 0) oz_crash ("oz_knl_process_validateall: couldnt find current process for cpus 0x%x (mask)", cpusactive);
#endif
}
