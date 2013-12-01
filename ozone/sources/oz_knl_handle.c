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
/*  These routines maintain the handle directory			*/
/*									*/
/*  The handle table is a per-process structure pointed to by the 	*/
/*  kernel mode OZ_Pdata struct's handletbl element.  It converts 	*/
/*  'handles' that usermode (and kernelmode) programs use to refer to 	*/
/*  objects into the corresponding object pointer.			*/
/*									*/
/************************************************************************/

#define _OZ_KNL_HANDLE_C

#include "ozone.h"

#include "oz_knl_event.h"
#include "oz_knl_handle.h"
#include "oz_knl_hw.h"
#include "oz_knl_image.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_printk.h"
#include "oz_knl_sdata.h"
#include "oz_knl_security.h"
#include "oz_knl_status.h"
#include "oz_knl_thread.h"
#include "oz_knl_userjob.h"
#include "oz_sys_pdata.h"

#define INITIALCOUNT 256 // use something small like 4 to test expansion routine

#define atomic_setif_long oz_hw_atomic_setif_long
#define atomic_inc_long oz_hw_atomic_inc_long
#define atomic_or_long oz_hw_atomic_or_long

#define LOCKTABLE_SH(ht) do {								\
  Long __tl;										\
  do __tl = ht -> tablelock;								\
  while ((__tl & 1) || !atomic_setif_long (&(ht -> tablelock), __tl + 2, __tl));	\
} while (0)

#define UNLOCKTABLE_SH(ht) atomic_inc_long (&(ht -> tablelock), -2)

#define LOCKTABLE_EX(ht) do {				\
  while (atomic_or_long (&(ht -> tablelock), 1) & 1) {}	\
  while (ht -> tablelock != 1) {}			\
} while (0)

#define UNLOCKTABLE_EX(ht) ht -> tablelock = 0

#define CVTLOCKTABLE_EX_SH(ht) ht -> tablelock = 2

#define OBJINCREFC(objtype,object,inc) ((*(objincrefc[objtype])) (object, inc))

typedef struct { void *object;			/* what object it is a handle to */
                 OZ_Objtype objtype;		/* object's type */
                 OZ_Thread *thread;		/* thread that assigned handle to object */
                 OZ_Procmode procmode;		/* processor mode that assigned handle to object */
                 OZ_Secaccmsk secaccmsk;	/* security access bits that have been granted */
                 OZ_Handle reuse;		/* handle re-use counter (incs by curcount each time re-assigned) */
                 volatile Long refcount;	/* ref count - free entry when it goes zero */
						/*    <00> - set when handle assigned */
						/*           cleared when handle released */
						/* <30:01> - incremented by handle_takeout */
						/*           decremented by handle_putback */
                 OZ_Handle nextfree;		/* index of next free handle */
               } Handleent;

struct OZ_Handletbl { OZ_Objtype objtype;	/* OZ_OBJTYPE_HANDLETBL */
                      volatile Long tablelock;	/* table lock: */
						/*    <00> - locked and it may be moved */
						/* <30:01> - locked for read count */
                      OZ_Handle curcount;	/* current number of handles present (power of 2) */
                      OZ_Handle curmask;	/* curmask = curcount - 1 */
                      Handleent *ents;		/* pointer to array of handle entries */
                      OZ_Handle free_h;		/* index of first free entry in table */
                      OZ_Handle free_t;		/* index of last free entry in table */
                    };

static Long (*(objincrefc[OZ_OBJTYPE_MAX])) (void *object, Long inc);

static void *free_handle (OZ_Handletbl *handletbl, OZ_Handle hi);
static uLong expand_table (OZ_Handletbl *handletbl);
static uLong find_handle (OZ_Handletbl *handletbl, OZ_Handle handle, OZ_Procmode procmode, Handleent **handleent_r);
static Long badobjtype_increfc (void *object, Long inc);

/************************************************************************/
/*									*/
/*  Boot-time init routine						*/
/*									*/
/************************************************************************/

void oz_knl_handle_init (void)

{
  OZ_Objtype objtype;

  for (objtype = 0; objtype < OZ_OBJTYPE_MAX; objtype ++) {
    objincrefc[objtype] = badobjtype_increfc;
  }

  objincrefc[OZ_OBJTYPE_EVENT]   = oz_knl_event_increfc;
  objincrefc[OZ_OBJTYPE_PROCESS] = oz_knl_process_increfc;
  objincrefc[OZ_OBJTYPE_SECTION] = oz_knl_section_increfc;
  objincrefc[OZ_OBJTYPE_THREAD]  = oz_knl_thread_increfc;
  objincrefc[OZ_OBJTYPE_IOCHAN]  = oz_knl_iochan_increfc;
  objincrefc[OZ_OBJTYPE_IMAGE]   = oz_knl_image_increfc;
  objincrefc[OZ_OBJTYPE_LOGNAME] = oz_knl_logname_increfc;
  objincrefc[OZ_OBJTYPE_USER]    = oz_knl_user_increfc;
  objincrefc[OZ_OBJTYPE_JOB]     = oz_knl_job_increfc;
  objincrefc[OZ_OBJTYPE_DEVUNIT] = oz_knl_devunit_increfc;
}

/************************************************************************/
/*									*/
/*  Create handle table for the current process				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_handletbl_create = OZ_SUCCESS or OZ_NOMEMORY		*/
/*									*/
/************************************************************************/

uLong oz_knl_handletbl_create (void)

{
  Handleent *he;
  OZ_Handle hi;
  OZ_Handletbl *handletbl;
  OZ_Pdata *pdata;

  pdata = OZ_SYS_PDATA_FROM_KNL (OZ_PROCMODE_KNL);

  if (pdata -> handletbl == NULL) {
    handletbl = oz_sys_pdata_malloc (OZ_PROCMODE_KNL, sizeof *handletbl);	// alloc kernel mode per-process memory
    if (handletbl == NULL) return (OZ_NOMEMORY);
    handletbl -> objtype   = OZ_OBJTYPE_HANDLETBL;				// fill in the handletbl struct
    handletbl -> tablelock = 0;
    handletbl -> curcount  = INITIALCOUNT;					// this is how big the handle entry array will be
    handletbl -> curmask   = INITIALCOUNT - 1;					// mask bits to get index from an handle number
    he = oz_sys_pdata_malloc (OZ_PROCMODE_KNL, INITIALCOUNT * sizeof *he);	// allocate the entry array
    if (he == NULL) {
      oz_sys_pdata_free (OZ_PROCMODE_KNL, handletbl);
      return (OZ_NOMEMORY);
    }
    handletbl -> ents = he;							// save entry array pointer
    memset (he, 0, INITIALCOUNT * sizeof *he);					// clear entry array
    handletbl -> free_h = 1;							// index of first free entry
    handletbl -> free_t = INITIALCOUNT - 1;					// index of last free entry
    for (hi = 1; hi < INITIALCOUNT - 1; hi ++) {				// initialize links for free array entries
      he[hi].nextfree = hi + 1;
    }
    if (!oz_hw_atomic_setif_ptr ((void *volatile *)&(pdata -> handletbl), handletbl, NULL)) { // set it, unless another thread beat us to it
      oz_sys_pdata_free (OZ_PROCMODE_KNL, he);					// if so, free off what we did
      oz_sys_pdata_free (OZ_PROCMODE_KNL, handletbl);
    }
  }

  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Close the current process' handle table				*/
/*									*/
/************************************************************************/

void oz_knl_handletbl_delete (void)

{
  Handleent *he;
  OZ_Handle hi;
  OZ_Handletbl *handletbl;
  OZ_Pdata *pdata;
  void *object;

  pdata = OZ_SYS_PDATA_FROM_KNL (OZ_PROCMODE_KNL);
  handletbl = pdata -> handletbl;
  if (handletbl != NULL) {
    pdata -> handletbl = NULL;
    OZ_KNL_CHKOBJTYPE (handletbl, OZ_OBJTYPE_HANDLETBL);			// make sure we're pointing to an handle table
    he = handletbl -> ents;
    for (hi = 0; ++ hi <= handletbl -> curmask;) {				// close out all objects unconditionally
      if ((++ he) -> refcount & -2) {
        oz_knl_dumpmem (sizeof *he, he);
        oz_crash ("oz_knl_handletbl_delete: he[%u] refcount %d", hi, he -> refcount);
      }
      object = he -> object;
      if (object != NULL) OBJINCREFC (he -> objtype, object, -1);
    }
    oz_sys_pdata_free (OZ_PROCMODE_KNL, handletbl -> ents);			// free off the table
    oz_sys_pdata_free (OZ_PROCMODE_KNL, handletbl);
  }
}

/************************************************************************/
/*									*/
/*  Assign an handle to an object					*/
/*									*/
/*    Input:								*/
/*									*/
/*	object   = object to be assigned				*/
/*	procmode = processor mode associated with object		*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_handle_assign = OZ_SUCCESS : successful			*/
/*	                             else : error status		*/
/*	*handle_r = handle assigned to object				*/
/*									*/
/*    Note:								*/
/*									*/
/*	this routine increments the object's ref count, 		*/
/*	and the release routine decrements it				*/
/*									*/
/*	caller must call oz_knl_handle_release when done with handle	*/
/*									*/
/************************************************************************/

uLong oz_knl_handle_assign (void *object, OZ_Procmode procmode, OZ_Handle *handle_r)

{
  Handleent *he;
  OZ_Handle hi;
  OZ_Handletbl *handletbl;
  OZ_Objtype objtype;
  OZ_Secaccmsk secaccmsk;
  OZ_Secattr *secattr;
  OZ_Seckeys *seckeys;
  uLong sts;

#if 000
  handletbl = OZ_SYS_PDATA_FROM_KNL (OZ_PROCMODE_KNL) -> handletbl;
#else
  {
    OZ_Pdata *pdata;

    pdata = OZ_SYS_PDATA_FROM_KNL (OZ_PROCMODE_KNL);
    handletbl = pdata -> handletbl;
//    oz_knl_printk ("oz_knl_handle_assign*: %s pdata %p, handletbl %p\n", oz_knl_process_getname (NULL), pdata, handletbl);
  }
#endif
  OZ_KNL_CHKOBJTYPE (handletbl, OZ_OBJTYPE_HANDLETBL);

  /* Make sure object type is ok and get its security attributes */

  objtype = OZ_KNL_GETOBJTYPE (object);
  switch (objtype) {
    case OZ_OBJTYPE_EVENT:   { secattr = oz_knl_event_getsecattr   (object); break; }
    case OZ_OBJTYPE_PROCESS: { secattr = oz_knl_process_getsecattr (object); break; }
    case OZ_OBJTYPE_SECTION: { secattr = oz_knl_section_getsecattr (object); break; }
    case OZ_OBJTYPE_THREAD:  { secattr = oz_knl_thread_getsecattr  (object); break; }
    case OZ_OBJTYPE_IOCHAN:  { secattr = oz_knl_iochan_getsecattr  (object); break; }
    case OZ_OBJTYPE_IMAGE:   { secattr = oz_knl_image_getsecattr   (object); break; }
    case OZ_OBJTYPE_LOGNAME: { secattr = oz_knl_logname_getsecattr (object); break; }
    case OZ_OBJTYPE_USER:    { secattr = oz_knl_logname_getsecattr (oz_knl_user_getlognamdir (object)); break; }
    case OZ_OBJTYPE_JOB:     { secattr = oz_knl_logname_getsecattr (oz_knl_job_getlognamdir  (object)); break; }
    case OZ_OBJTYPE_DEVUNIT: { secattr = oz_knl_devunit_getsecattr (object); break; }
    default: return (OZ_BADHANDOBJTYPE);
  }

  seckeys   = oz_knl_thread_getseckeys (NULL);
  secaccmsk = oz_knl_security_getsecaccmsk (seckeys, secattr);
  oz_knl_secattr_increfc (secattr, -1);
  oz_knl_seckeys_increfc (seckeys, -1);

  /* Find an usable entry in the handle table, expand it if full */

  LOCKTABLE_EX (handletbl);

  hi = handletbl -> free_h;						// get first free entry
  if (hi == 0) {
    sts = expand_table (handletbl);					// expand table
    if (sts != OZ_SUCCESS) {
      UNLOCKTABLE_EX (handletbl);					// failed, unlock table
      return (sts);							// return error status
    }
    hi = handletbl -> free_h;						// expanded, get first free entry
  }
  he = handletbl -> ents + hi;						// point to entry we are about to assign
  handletbl -> free_h = he -> nextfree;					// unlink it from free list
  if (handletbl -> free_h == 0) handletbl -> free_t = 0;
  CVTLOCKTABLE_EX_SH (handletbl);					// convert from exclusive to shared access

  /* Set up the handle table entry */

  he -> object    = object;						// save object pointer
  he -> objtype   = objtype;						// save object type
  he -> thread    = oz_knl_thread_getcur ();				// save thread that allocated it
  he -> procmode  = procmode;						// save processor mode that allocated it
  he -> secaccmsk = secaccmsk;						// save granted security access bits
  he -> refcount  = 1;							// initialize entry's reference count
  *handle_r = hi + he -> reuse;						// return handle including the re-use counter
  OBJINCREFC (objtype, object, 1);					// increment object's reference count
  UNLOCKTABLE_SH (handletbl);						// release shared lock
  return (OZ_SUCCESS);							// return completion status
}

/************************************************************************/
/*									*/
/*  A process was just copied.  Since the handle table is part of the 	*/
/*  process private memory and thus a private section, the table 	*/
/*  itself is already copied.  However, we need to increment the 	*/
/*  reference counts on all the objects.  But don't inc the refcounts 	*/
/*  for process-private objects (like images), as we copied the object 	*/
/*  itself, not just the pointer.	.				*/
/*									*/
/************************************************************************/

void oz_knl_handle_tablecopied (void)

{
  Handleent *he;
  OZ_Handle hi, numentries;
  OZ_Handletbl *handletbl;
  void *object;

  handletbl = OZ_SYS_PDATA_FROM_KNL (OZ_PROCMODE_KNL) -> handletbl;
  if (handletbl != NULL) {
    OZ_KNL_CHKOBJTYPE (handletbl, OZ_OBJTYPE_HANDLETBL);
    numentries = handletbl -> curmask;
    he = handletbl -> ents;
    for (hi = 0; ++ hi < numentries;) {
      object = (++ he) -> object;
      if ((object != NULL) && OZ_HW_ISSYSADDR (object)) {
        OBJINCREFC (he -> objtype, object, 1);
      }
    }
  }
}

/************************************************************************/
/*									*/
/*  Get contents of handle table					*/
/*									*/
/*    Input:								*/
/*									*/
/*	the calling process' handle table				*/
/*	smplevel = softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_handletbl_state = 0 : no handle table present		*/
/*	                      else : number of elements			*/
/*	*objects    = array of object pointers				*/
/*	*objtypes   = array of object types				*/
/*	*threads    = array of thread pointers				*/
/*	*procmode   = array of owner processor modes			*/
/*	*secaccmsks = array of security access masks			*/
/*	*refcounts  = array of reference counts				*/
/*									*/
/************************************************************************/

OZ_Handle oz_knl_handletbl_statsget (OZ_Handle **handles, void ***objects, OZ_Objtype **objtypes, OZ_Thread ***threads, 
                                     OZ_Procmode **procmodes, OZ_Secaccmsk **secaccmsks, Long **refcounts)

{
  Handleent *he;
  Long *rcs;
  OZ_Handle hi, *hs, numentries;
  OZ_Handletbl *handletbl;
  OZ_Objtype *ots;
  OZ_Procmode *pms;
  OZ_Secaccmsk *sms;
  OZ_Thread **ts;
  void **ops;

  handletbl = OZ_SYS_PDATA_FROM_KNL (OZ_PROCMODE_KNL) -> handletbl;		// point to process' handle table
  if (handletbl == NULL) return (0);						// if not set up yet, return 0
  OZ_KNL_CHKOBJTYPE (handletbl, OZ_OBJTYPE_HANDLETBL);				// make sure pointer is ok
  LOCKTABLE_SH (handletbl);							// don't allow changes
  numentries = handletbl -> curmask;						// get number of entries

  *handles    = hs  = OZ_KNL_PGPMALLOC (numentries * sizeof **handles);		// malloc output arrays
  *objects    = ops = OZ_KNL_PGPMALLOC (numentries * sizeof **objects);
  *objtypes   = ots = OZ_KNL_PGPMALLOC (numentries * sizeof **objtypes);
  *threads    = ts  = OZ_KNL_PGPMALLOC (numentries * sizeof **threads);
  *procmodes  = pms = OZ_KNL_PGPMALLOC (numentries * sizeof **procmodes);
  *secaccmsks = sms = OZ_KNL_PGPMALLOC (numentries * sizeof **secaccmsks);
  *refcounts  = rcs = OZ_KNL_PGPMALLOC (numentries * sizeof **refcounts);

  he = handletbl -> ents;							// point to entry array
  for (hi = 0; hi < numentries; hi ++) {					// check out each entry
    *(hs ++)  = he -> reuse + hi;						// return the handle number
    *(ops ++) = he -> object;							// return the object pointer
    *(ots ++) = he -> objtype;							// return the object's type
    *(ts ++)  = he -> thread;							// return the thread pointer
    *(pms ++) = he -> procmode;							// return the owning proc mode
    *(sms ++) = he -> secaccmsk;						// return the security access mask
    *(rcs ++) = he -> refcount;							// return the reference count
    if (he -> object != NULL) OBJINCREFC (he -> objtype, he -> object, 1);	// increment the reference count
    if (he -> thread != NULL) oz_knl_thread_increfc (he -> thread, 1);		// increment thread ref count
    he ++;									// on to next table entry
  }

  UNLOCKTABLE_SH (handletbl);
  return (numentries);
}

void oz_knl_handletbl_statsfree (OZ_Handle numentries, OZ_Handle *handles, void **objects, OZ_Objtype *objtypes, 
                                 OZ_Thread **threads, OZ_Procmode *procmodes, OZ_Secaccmsk *secaccmsks, Long *refcounts)

{
  OZ_Handle hi;

  for (hi = 0; hi < numentries; hi ++) {
    if (objects[hi] != NULL) OBJINCREFC (objtypes[hi], objects[hi], -1);	// decrement the reference count
    if (threads[hi] != NULL) oz_knl_thread_increfc (threads[hi], -1);		// decrement thread ref count
  }

  OZ_KNL_PGPFREE (handles);
  OZ_KNL_PGPFREE (objects);
  OZ_KNL_PGPFREE (objtypes);
  OZ_KNL_PGPFREE (threads);
  OZ_KNL_PGPFREE (procmodes);
  OZ_KNL_PGPFREE (secaccmsks);
  OZ_KNL_PGPFREE (refcounts);
}

/************************************************************************/
/*									*/
/*  Look up the handle							*/
/*									*/
/*    Input:								*/
/*									*/
/*	handletbl = pointer to handle table				*/
/*	            NULL for process default table			*/
/*	handle    = handle to be looked up				*/
/*	procmode  = requestor's processor mode				*/
/*	secaccmsk = access required					*/
/*	objtype   = object type						*/
/*	            OZ_OBJTYPE_UNKNOWN if unknown, caller to determine type
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_handle_takeout = OZ_SUCCESS : successful			*/
/*	                              else : error status		*/
/*	*object_r    = object pointer					*/
/*	*secaccmsk_r = security access mask				*/
/*									*/
/*    Note:								*/
/*									*/
/*	caller must call oz_knl_handle_putback when done with handle	*/
/*									*/
/************************************************************************/

uLong oz_knl_handle_takeout (OZ_Handle handle, OZ_Procmode procmode, OZ_Secaccmsk secaccmsk, OZ_Objtype objtype, void **object_r, OZ_Secaccmsk *secaccmsk_r)

{
  Handleent *he;
  Long tablelock;
  OZ_Handletbl *handletbl;
  uLong sts;

  handletbl = OZ_SYS_PDATA_FROM_KNL (OZ_PROCMODE_KNL) -> handletbl;
  OZ_KNL_CHKOBJTYPE (handletbl, OZ_OBJTYPE_HANDLETBL);

  LOCKTABLE_SH (handletbl);						// lock for shared access

  sts = find_handle (handletbl, handle, procmode, &he);			/* find the handle in question */
  if ((sts == OZ_SUCCESS) 						/* make sure object type matches */
   && (objtype != OZ_OBJTYPE_UNKNOWN) 
   && (he -> objtype != objtype)) sts = OZ_BADHANDOBJTYPE;
  if ((sts == OZ_SUCCESS) && (secaccmsk & ~(he -> secaccmsk))) {	/* ok, check its security attributes */
    sts = OZ_SECACCDENIED;
  }
  if ((sts == OZ_SUCCESS) && (object_r != NULL)) {
    *object_r = he -> object;						/* ok, return object pointer */
    atomic_inc_long (&(he -> refcount), 2);				/* inc entry ref count, leave bit <00> alone */
    if (secaccmsk_r != NULL) *secaccmsk_r = he -> secaccmsk;		/* maybe return security access mask */
  }

  UNLOCKTABLE_SH (handletbl);						// release shared lock

  return (sts);
}

/* Call this routine when done with the object pointer returned by oz_knl_handle_takeout */

void oz_knl_handle_putback (OZ_Handle handle)

{
  Handleent *he;
  Long refc;
  OZ_Handle hi;
  OZ_Handletbl *handletbl;
  void *object;

  handletbl = OZ_SYS_PDATA_FROM_KNL (OZ_PROCMODE_KNL) -> handletbl;
  OZ_KNL_CHKOBJTYPE (handletbl, OZ_OBJTYPE_HANDLETBL);

  LOCKTABLE_SH (handletbl);					/* lock the handle table */
  hi = handle & handletbl -> curmask;				/* get handle's index in table */
  he = handletbl -> ents + hi;					/* point to entry in question */
  refc = atomic_inc_long (&(he -> refcount), -2);		/* decrement ref count, leave bit <00> alone */
  UNLOCKTABLE_SH (handletbl);					/* unlock the handle table */
  if (refc <= 0) {						/* see if handle needs to be freed off */
    if (refc < 0) oz_crash ("oz_knl_handle_putback: he %p -> refcount %d", he, refc);
    LOCKTABLE_EX (handletbl);					/* if so, get exclusive access to table */
    hi = handle & handletbl -> curmask;				/* point to entry again (it may have moved) */
    he = handletbl -> ents + hi;
    object = free_handle (handletbl, hi);			/* release the entry */
    UNLOCKTABLE_EX (handletbl);
    OBJINCREFC (OZ_KNL_GETOBJTYPE (object), object, -1);	/* release object pointer */
  }
}

/************************************************************************/
/*									*/
/*  Get handle's security access mask					*/
/*									*/
/************************************************************************/

uLong oz_knl_handle_getsecaccmsk (OZ_Handle handle, OZ_Procmode procmode, OZ_Secaccmsk *secaccmsk_r)

{
  Handleent *he;
  OZ_Handletbl *handletbl;
  uLong sts;

  handletbl = OZ_SYS_PDATA_FROM_KNL (OZ_PROCMODE_KNL) -> handletbl;
  OZ_KNL_CHKOBJTYPE (handletbl, OZ_OBJTYPE_HANDLETBL);

  LOCKTABLE_SH (handletbl);						/* lock the handle table */
  sts = find_handle (handletbl, handle, procmode, &he);			/* find the handle in question */
  if (sts == OZ_SUCCESS) *secaccmsk_r = he -> secaccmsk;		/* if success, get mask */
  UNLOCKTABLE_SH (handletbl);						/* unlock the handle table */

  return (sts);
}

/************************************************************************/
/*									*/
/*  Set handle's security access mask					*/
/*									*/
/************************************************************************/

uLong oz_knl_handle_setsecaccmsk (OZ_Handle handle, OZ_Procmode procmode, OZ_Secaccmsk secaccmsk)

{
  Handleent *he;
  OZ_Handletbl *handletbl;
  uLong sts;

  handletbl = OZ_SYS_PDATA_FROM_KNL (OZ_PROCMODE_KNL) -> handletbl;
  OZ_KNL_CHKOBJTYPE (handletbl, OZ_OBJTYPE_HANDLETBL);

  LOCKTABLE_SH (handletbl);						/* lock the handle table */
  sts = find_handle (handletbl, handle, procmode, &he);			/* find the handle in question */
  if (sts == OZ_SUCCESS) he -> secaccmsk = secaccmsk;			/* if success, set mask */
  UNLOCKTABLE_SH (handletbl);						/* unlock the handle table */

  return (sts);
}

/************************************************************************/
/*									*/
/*  Scan handle table for 'next' entry					*/
/*									*/
/*    Input:								*/
/*									*/
/*	handletbl = handle table pointer				*/
/*	handle = previous handle (or 0 to start at beginning)		*/
/*	procmode = requestor's processor mode				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_handle_next = 0 : end of table				*/
/*	                  else : next handle in table			*/
/*									*/
/************************************************************************/

OZ_Handle oz_knl_handle_next (OZ_Handle handle, OZ_Procmode procmode)

{
  Handleent *he;
  OZ_Handle hi;
  OZ_Handletbl *handletbl;

  handletbl = OZ_SYS_PDATA_FROM_KNL (OZ_PROCMODE_KNL) -> handletbl;
  OZ_KNL_CHKOBJTYPE (handletbl, OZ_OBJTYPE_HANDLETBL);

  LOCKTABLE_SH (handletbl);					/* lock the handle table */

  hi = handle & handletbl -> curmask;				/* get old handle's index in table */
  handle = 0;							/* assume we will hit end of table */
  while (++ hi <= handletbl -> curmask) {
    he = handletbl -> ents + hi;				/* point to entry in question */
    if (he -> object == NULL) continue;				/* see if object pointer null (it has been closed) */
    if (he -> procmode < procmode) continue;			/* skip if more privileged than caller */
    handle = he -> reuse + hi;					/* ok, use it */
    break;
  }
  UNLOCKTABLE_SH (handletbl);					/* unlock the handle table */

  return (handle);
}

/************************************************************************/
/*									*/
/*  Set the thread of an handle.  Doing this will cause the handle to 	*/
/*  be released when the thread exits.  The thread given must be a 	*/
/*  thread of the process the handletable belongs to.			*/
/*									*/
/*    Input:								*/
/*									*/
/*	handle   = handle to be modified				*/
/*	procmode = processor mode of the handle				*/
/*	thread   = thread to set it to or NULL so handle will remain 	*/
/*	           until process exits					*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_handle_setthread = OZ_SUCCESS : successful		*/
/*	                                else : error status		*/
/*									*/
/************************************************************************/

uLong oz_knl_handle_setthread (OZ_Handle handle, OZ_Procmode procmode, OZ_Thread *thread)

{
  Handleent *he;
  OZ_Handletbl *handletbl;
  uLong sts;
  void *object;

  handletbl = OZ_SYS_PDATA_FROM_KNL (OZ_PROCMODE_KNL) -> handletbl;
  OZ_KNL_CHKOBJTYPE (handletbl, OZ_OBJTYPE_HANDLETBL);

  LOCKTABLE_SH (handletbl);
  sts = find_handle (handletbl, handle, procmode, &he);
  if (sts == OZ_SUCCESS) he -> thread = thread;
  UNLOCKTABLE_SH (handletbl);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Release all handles for the current thread				*/
/*									*/
/*    Input:								*/
/*									*/
/*	thread    = NULL : close current thread's handles		*/
/*	            else : close indicated thread's handles		*/
/*	procmode  = requestor's processor mode				*/
/*	smplevel <= softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_handle_release_all = OZ_SUCCESS : successful		*/
/*	                                  else : error status		*/
/*	object's reference count decremented				*/
/*	handle table entry cleared					*/
/*									*/
/************************************************************************/

void oz_knl_handle_release_all (OZ_Thread *thread, OZ_Procmode procmode)

{
  Handleent *he;
  Long refc;
  OZ_Handle hi;
  OZ_Handletbl *handletbl;
  void *object;

  handletbl = OZ_SYS_PDATA_FROM_KNL (OZ_PROCMODE_KNL) -> handletbl;
  OZ_KNL_CHKOBJTYPE (handletbl, OZ_OBJTYPE_HANDLETBL);

  if (handletbl == NULL) return;					/* maybe this is the cleanupproc thread - there is no table */
  if (thread == NULL) thread = oz_knl_thread_getcur ();			/* default to current thread */

  LOCKTABLE_EX (handletbl);
  for (hi = 0; ++ hi <= handletbl -> curmask;) {
    he = handletbl -> ents + hi;					/* point to entry in question */
    if (he -> procmode < procmode) continue;				/* make sure processor mode allows access */
    if (he -> thread != thread) continue;				/* make sure it was assigned by given thread */
    refc = oz_hw_atomic_and_long (&(he -> refcount), -2);		/* release if not already */
    if (refc == 1) {
      object = free_handle (handletbl, hi);				/* wasn't already released, free entry */
      UNLOCKTABLE_EX (handletbl);
      OBJINCREFC (OZ_KNL_GETOBJTYPE (object), object, -1);
      LOCKTABLE_EX (handletbl);
    }
  }
  UNLOCKTABLE_EX (handletbl);
}

/************************************************************************/
/*									*/
/*  Release handle							*/
/*									*/
/*    Input:								*/
/*									*/
/*	handle   = handle previously assigned to an object		*/
/*	procmode = requestor's processor mode				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_handle_release = OZ_SUCCESS : successful			*/
/*	                              else : error status		*/
/*	object's reference count decremented				*/
/*	handle table entry cleared					*/
/*									*/
/************************************************************************/

uLong oz_knl_handle_release (OZ_Handle handle, OZ_Procmode procmode)

{
  Handleent *he;
  Long refc;
  OZ_Handletbl *handletbl;
  uLong sts;
  void *object;

  handletbl = OZ_SYS_PDATA_FROM_KNL (OZ_PROCMODE_KNL) -> handletbl;
  OZ_KNL_CHKOBJTYPE (handletbl, OZ_OBJTYPE_HANDLETBL);

  object = NULL;								// assume entry is in use by someone
  LOCKTABLE_EX (handletbl);							// get exclusive access to table so we can call free_handle
  sts = find_handle (handletbl, handle, procmode, &he);				// find entry to be released
  if (sts == OZ_SUCCESS) {
    refc = atomic_inc_long (&(he -> refcount), -1);				// if found, clear the assigned bit (refcount<00>)
    if (refc == 0) {								// check for all references gone
      object = free_handle (handletbl, handle & handletbl -> curmask);		// if so, free the entry off and get object pointer
    }
  }
  UNLOCKTABLE_EX (handletbl);							// unlock table
  if (object != NULL) {
    OBJINCREFC (OZ_KNL_GETOBJTYPE (object), object, -1);			// if we actually freed the entry, release object pointer
  }
  return (sts);
}

/* Free an handle table entry.  Table must be locked for exclusive access. */

static void *free_handle (OZ_Handletbl *handletbl, OZ_Handle hi)

{
  Handleent *he;
  void *object;

  he = handletbl -> ents + hi;					// point to entry in table to be freed

  object = he -> object;					// return object pointer
  he -> object = NULL;						// clear it out to indicate it's free
  he -> thread = NULL;						// also clear out thread pointer

  he -> reuse   += handletbl -> curcount;			// increment re-use count for next time
  he -> nextfree = 0;						// put this entry on end of free list
  if (handletbl -> free_t == 0) handletbl -> free_h = hi;
  else handletbl -> ents[handletbl->free_t].nextfree = hi;
  handletbl -> free_t = hi;

  return (object);						// return pointer to object
}

/************************************************************************/
/*									*/
/*  Expand the handle table by doubling its size			*/
/*  Table must be locked for exclusive access				*/
/*									*/
/************************************************************************/

static uLong expand_table (OZ_Handletbl *handletbl)

{
  Handleent *he, *newents;
  OZ_Handle hi, oldcount;

  oldcount = handletbl -> curcount;

  newents = oz_sys_pdata_malloc (OZ_PROCMODE_KNL, oldcount * 2 * sizeof *newents); // allocate memory for new array
  if (newents == NULL) return (OZ_NOMEMORY);
  memset (newents, 0, oldcount * 2 * sizeof *newents);		// clear it out

  handletbl -> free_h = oldcount;				// free top half entry corresponding to old 0
  handletbl -> free_t = oldcount;

  for (hi = 0; ++ hi < oldcount;) {				// loop through old array
    he = handletbl -> ents + hi;				// point to entry, assume they're all in use
    if (he -> reuse & oldcount) {				// see which half of new array it goes in
      newents[hi+oldcount] = *he;				// top half, copy it
      newents[hi+oldcount].reuse -= oldcount;
      newents[handletbl->free_t].nextfree = hi;			// free bottom half entry
      handletbl -> free_t = hi;
      newents[hi].reuse = he -> reuse + oldcount;
    } else {
      newents[hi] = *he;					// bottom half, copy it
      newents[handletbl->free_t].nextfree = hi + oldcount;	// free top half entry
      handletbl -> free_t = hi + oldcount;
      newents[hi+oldcount].reuse = he -> reuse;
    }
  }

  oz_sys_pdata_free (OZ_PROCMODE_KNL, handletbl -> ents);	// free off old table
  handletbl -> ents = newents;					// establish the new table
  handletbl -> curcount *= 2;
  handletbl -> curmask   = handletbl -> curcount - 1;
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Find handle entry for a given handle				*/
/*									*/
/*    Input:								*/
/*									*/
/*	handletbl = handle table pointer				*/
/*	handle    = handle to lookup					*/
/*	procmode  = requestor's processor mode				*/
/*									*/
/*    Output:								*/
/*									*/
/*	find_handle = OZ_SUCCESS : successful				*/
/*	                    else : error status				*/
/*	*handleent_r = handle entry pointer				*/
/*									*/
/*    Note:								*/
/*									*/
/*	This routine gets called a lot (like for doing I/O)		*/
/*	Table must be locked for either shared or exclusive access	*/
/*									*/
/************************************************************************/

static uLong find_handle (OZ_Handletbl *handletbl, OZ_Handle handle, OZ_Procmode procmode, Handleent **handleent_r)

{
  Handleent *he;
  OZ_Handle hi;

  hi = handle & handletbl -> curmask;				/* get handle's index in table */
								/* - use & instead of % because this routine is called a lot */
								/*   (we are guaranteed that curcount is a power of 2) */
  if (hi == 0) goto err_nullhandle;				/* if zero, its the null handle */
  he = handletbl -> ents + hi;					/* point to entry in question */
  if (handle != hi + he -> reuse) goto err_closedhandle;	/* see if it has been re-used */
  if (he -> object == NULL) goto err_closedhandle;		/* see if object pointer null (it has been closed) */
  if (!(he -> refcount & 1)) goto  err_closedhandle;		/* see if refcount<00> set, if not, it has been released */
  if (he -> procmode < procmode) goto err_procmode;		/* make sure processor mode allows access */
  *handleent_r = he;						/* ok, return handle entry pointer */
  return (OZ_SUCCESS);

err_nullhandle:
  return (OZ_NULLHANDLE);
err_closedhandle:
  return (OZ_CLOSEDHANDLE);
err_procmode:
  return (OZ_PROCMODE);
}

/************************************************************************/
/*									*/
/*  Increment an handle's object reference count			*/
/*									*/
/*    Input:								*/
/*									*/
/*	object   = pointer to handle's object				*/
/*	inc      = amount to increment object's ref count by		*/
/*	smplevel = SOFTINT when decrementing				*/
/*	        <= HT otherwise						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_handle_objincrefc = new reference count			*/
/*									*/
/************************************************************************/

Long oz_knl_handle_objincrefc (OZ_Objtype objtype, void *object, Long inc)

{
  if (objtype >= OZ_OBJTYPE_MAX) oz_crash ("oz_knl_handle_objincrefc: bad %p -> objtype %d", object, objtype);
  return (OBJINCREFC (objtype, object, inc));
}

static Long badobjtype_increfc (void *object, Long inc)

{
  oz_crash ("oz_knl_handle_objincrefc: bad %p -> objtype %d", object, OZ_KNL_GETOBJTYPE (object));
}
