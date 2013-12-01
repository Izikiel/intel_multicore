//+++2002-05-10
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
//---2002-05-10

/************************************************************************/
/*									*/
/*  In-memory resource usage limits					*/
/*									*/
/************************************************************************/

#define _OZ_KNL_QUOTA_C

#include "ozone.h"

#include "oz_knl_devio.h"
#include "oz_knl_hw.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_objtype.h"
#include "oz_knl_process.h"
#include "oz_knl_quota.h"
#include "oz_knl_sdata.h"
#include "oz_knl_status.h"
#include "oz_knl_thread.h"
#include "oz_knl_userjob.h"

struct OZ_Quota { OZ_Objtype objtype;
                  Long refcount;
                  Long max[OZ_QUOTATYPE_MAX];
                  Long use[OZ_QUOTATYPE_MAX];
                };

static OZ_Quota *cpudefquota[OZ_HW_MAXCPUS];	/* array of pointers to what quota block to use for a given cpu */

/************************************************************************/
/*									*/
/*  Boot time data initialization					*/
/*									*/
/************************************************************************/

void oz_knl_quota_init (void)

{
  memset (cpudefquota, 0, sizeof cpudefquota);
}

/************************************************************************/
/*									*/
/*  Create a quota block						*/
/*									*/
/*    Input:								*/
/*									*/
/*	quotastr = string of quota limits				*/
/*	smplevel <= np							*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_quota_create = OZ_SUCCESS : successfully created		*/
/*	                            else : error status			*/
/*	*quota_r = pointer to quota block				*/
/*									*/
/*    Note:								*/
/*									*/
/*	quotastr is of form: <type>=<value>,...				*/
/*									*/
/************************************************************************/

static const struct { int size; const char *name; OZ_Quotatype type; } quotatbl[] = {
                             3,            "npp", OZ_QUOTATYPE_NPP, 
                             3,            "pgp", OZ_QUOTATYPE_PGP, 
                             3,            "phm", OZ_QUOTATYPE_PHM, 
                             0,             NULL, OZ_QUOTATYPE_END };

uLong oz_knl_quota_create (int quotastr_l, const char *quotastr, OZ_Quota **quota_r)

{
  int i, j, usedup;
  Long max;
  OZ_Quota *quota;
  OZ_Quotatype type;

  quota = NULL;
  if ((quotastr_l != 1) || (quotastr[0] != '*')) {
    quota = OZ_KNL_NPPMALLOC (sizeof *quota);				/* allocate memory block */
    memset (quota, 0, sizeof *quota);					/* clear it to zeroes so unspecified quotas are zero and all usages are zero */
    quota -> objtype  = OZ_OBJTYPE_QUOTA;				/* set the object type */
    quota -> refcount = 1;						/* reference count = 1 (for the returned pointer) */
    for (i = 0; i < quotastr_l;) {
      for (j = 0; quotatbl[j].name != NULL; j ++) {
        if ((quotastr[i+quotatbl[j].size] == '=') && (strncasecmp (quotatbl[j].name, quotastr + i, quotatbl[j].size) == 0)) break;
      }
      if (quotatbl[j].name == NULL) goto badquotatype;
      i   += quotatbl[j].size + 1;
      type = quotatbl[j].type;
      max  = oz_hw_atoi (quotastr + i, &usedup);
      if (usedup == 0) goto badquotatype;
      i += usedup;
      if ((i < quotastr_l) && (quotastr[i++] != ',')) goto badquotatype;
      quota -> max[type] = max;
    }
  }
  oz_knl_printk ("oz_knl_quota_create*: %p: %*.*s\n", quota, quotastr_l, quotastr_l, quotastr);
  *quota_r = quota;							/* return pointer */
  return (OZ_SUCCESS);							/* successful */

badquotatype:
  OZ_KNL_NPPFREE (quota);
  return (OZ_BADQUOTATYPE);
}

/************************************************************************/
/*									*/
/*  Convert quota block to printable string				*/
/*									*/
/*    Input:								*/
/*									*/
/*	quota  = quota block to convert					*/
/*	usages = 0 : just print the maximum for each type		*/
/*	         1 : print size/maximum for each type			*/
/*	bufsize = size of string buffer					*/
/*	bufaddr = address of string buffer				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_quota_string = OZ_SUCCESS : sucessfully converted	*/
/*	                    OZ_BUFFEROVF : string buffer overflowed	*/
/*	*bufaddr = null terminated printable quota string		*/
/*									*/
/************************************************************************/

uLong oz_knl_quota_string (OZ_Quota *quota, int usages, int bufsize, char *bufaddr)

{
  const char *p;
  int i, j;
  uLong sts;

  OZ_KNL_CHKOBJTYPE (quota, OZ_OBJTYPE_QUOTA);

  if (quota == NULL) {
    strncpyz (bufaddr, "*", bufsize);
    if (bufsize < 2) return (OZ_BUFFEROVF);
    return (OZ_SUCCESS);
  }

  i = 0;
  p = "";
  for (j = 0; quotatbl[j].name != NULL; j ++) {
    if (!usages) sts = oz_sys_sprintf (bufsize - i, bufaddr + i, "%s%s=%u", p, quotatbl[j].name, quota -> max[quotatbl[j].type]);
    else sts = oz_sys_sprintf (bufsize - i, bufaddr + i, "%s%s=%u/%u", p, quotatbl[j].name, quota -> use[quotatbl[j].type], quota -> max[quotatbl[j].type]);
    if (sts != OZ_SUCCESS) break;
    i += strlen (bufaddr + i);
    p  = ",";
  }
  return (sts);
}

/************************************************************************/
/*									*/
/*  Update exising quota block with maxima from a new quota block	*/
/*									*/
/************************************************************************/

void oz_knl_quota_update (OZ_Quota *quota, OZ_Quota *newquota)

{
  OZ_Quotatype i;

  for (i = 0; i < OZ_QUOTATYPE_MAX; i ++) quota -> max[i] = newquota -> max[i];
}

/************************************************************************/
/*									*/
/*  Increment quota block reference count				*/
/*									*/
/*    Input:								*/
/*									*/
/*	quota     = quota block to inc refcount of			*/
/*	inc       = amount to increment by				*/
/*	smplevel <= np (in case refcount goes zero)			*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_quota_increfc = new ref count				*/
/*	quota block deleted if ref count goes to zero			*/
/*	all usages must be zero						*/
/*									*/
/************************************************************************/

Long oz_knl_quota_increfc (OZ_Quota *quota, Long inc)

{
  int i;
  Long refc;

  OZ_KNL_CHKOBJTYPE (quota, OZ_OBJTYPE_QUOTA);
  refc = oz_hw_atomic_inc_long (&(quota -> refcount), inc);		/* update reference count */
  if (refc < 0) oz_crash ("oz_knl_quota_increfc: ref count negative (%d)", refc); /* never should be negative */
  if (refc == 0) {
    oz_knl_printk ("oz_knl_quota_increfc*: %p refc zero\n", quota);
    for (i = 0; i < OZ_QUOTATYPE_MAX; i ++) {				/* (all usages should be zero) */
      if (quota -> use[i] != 0) oz_crash ("oz_knl_quota_increfc: ref count zero but quotatype %d use is %u", i, quota -> use[i]);
    }
    OZ_KNL_NPPFREE (quota);						/* everything zero, free off the block */
  }
  return (refc);							/* return updated reference count */
}

/************************************************************************/
/*									*/
/*  Set default quota block for the current cpu				*/
/*									*/
/*    Input:								*/
/*									*/
/*	quota = new quota block for the current cpu			*/
/*	smplevel >= softint (so cpu doesn't switch on us)		*/
/*									*/
/************************************************************************/

OZ_Quota *oz_knl_quota_setcpudef (OZ_Quota *quota)

{
  Long cpuid;
  OZ_Quota *oldcpuquota;

  OZ_KNL_CHKOBJTYPE (quota, OZ_OBJTYPE_QUOTA);

  cpuid = oz_hw_cpu_getcur ();			/* get this cpu's id */
  oldcpuquota = cpudefquota[cpuid];		/* get the old quota block pointer */
  cpudefquota[cpuid] = quota;			/* set up new quota block pointer */
  return (oldcpuquota);
}

/************************************************************************/
/*									*/
/*  Get default quota block for the current cpu				*/
/*									*/
/*    Input:								*/
/*									*/
/*	smplevel >= softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_quota_getcpudef = quota block for current cpu		*/
/*									*/
/************************************************************************/

OZ_Quota *oz_knl_quota_getcpudef (void)

{
  Long cpuid;

  cpuid = oz_hw_cpu_getcur ();				/* get this cpu's id */
  return (cpudefquota[cpuid]);				/* return block pointer */
}

/************************************************************************/
/*									*/
/*  Get quota block associated with an object				*/
/*  This routine increments the quota's reference count			*/
/*									*/
/************************************************************************/

OZ_Quota *oz_knl_quota_fromobj (void *quotaobj)

{
  if (quotaobj != NULL) {
    switch (OZ_KNL_GETOBJTYPE (quotaobj)) {
      case OZ_OBJTYPE_IOOP: {
        quotaobj = oz_knl_ioop_getthread (quotaobj);
        if (quotaobj == NULL) return (NULL);
      }
      case OZ_OBJTYPE_THREAD:  quotaobj = oz_knl_thread_getprocess (quotaobj);
      case OZ_OBJTYPE_PROCESS: quotaobj = oz_knl_process_getjob (quotaobj);
      case OZ_OBJTYPE_JOB:     quotaobj = oz_knl_job_getuser (quotaobj);
      case OZ_OBJTYPE_USER:    return (oz_knl_user_getquota (quotaobj));
      case OZ_OBJTYPE_QUOTA:   oz_knl_quota_increfc (quotaobj, 1); return (quotaobj);
      default: oz_crash ("oz_knl_quota_fromobj: cannot determine quota from object type %d", OZ_KNL_GETOBJTYPE (quotaobj));
    }
  }
  return (NULL);
}

/************************************************************************/
/*									*/
/*  Debit a quota							*/
/*									*/
/*    Input:								*/
/*									*/
/*	quota     = quota block						*/
/*	quotatype = quota type						*/
/*	amount    = amount of quota to debit				*/
/*	smplevel  = anything (uses atomic operations)			*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_quota_debit = 1 : quota successfully debited		*/
/*	                     0 : exceeded quota limit			*/
/*									*/
/************************************************************************/

int oz_knl_quota_debit (OZ_Quota *quota, OZ_Quotatype quotatype, Long amount)

{
  Long newuse, olduse;

  OZ_KNL_CHKOBJTYPE (quota, OZ_OBJTYPE_QUOTA);
  if (quotatype >= OZ_QUOTATYPE_MAX) oz_crash ("oz_knl_quota_debit: bad quota type %u", quotatype);
  if (amount < 0) oz_crash ("oz_knl_quota_debit: negative amount (%d)", amount);
  if (quota == NULL) return (1);

  if (amount > quota -> max[quotatype]) return (0);				/* check this way first to prevent wrap-around */
  do {
    olduse = quota -> use[quotatype];						/* get the currrent usage */
    if (olduse + amount > quota -> max[quotatype]) return (0);			/* fail if it would max out */
  } while (!oz_hw_atomic_setif_long (&(quota -> use[quotatype]), olduse + amount, olduse)); /* ok, write new value */
  OZ_HW_ATOMIC_INCBY1_LONG (quota -> refcount);					/* inc quota block ref count */
  return (1);									/* successful */
}

/************************************************************************/
/*									*/
/*  Credit a previously debited quota					*/
/*									*/
/*    Input:								*/
/*									*/
/*	quota     = quota block						*/
/*	quotatype = quota type						*/
/*	amount    = amount of quota to credit				*/
/*	inc       = amount to inc refcount by (normally -1)		*/
/*	smplevel <= np (in case refcount goes zero)			*/
/*									*/
/************************************************************************/

void oz_knl_quota_credit (OZ_Quota *quota, OZ_Quotatype quotatype, Long amount, Long inc)

{
  OZ_KNL_CHKOBJTYPE (quota, OZ_OBJTYPE_QUOTA);
  if (quotatype >= OZ_QUOTATYPE_MAX) oz_crash ("oz_knl_quota_credit: bad quota type %u", quotatype);
  if (amount < 0) oz_crash ("oz_knl_quota_credit: negative amount (%d)", amount);

  if (quota != NULL) {
    if (amount > quota -> use[quotatype]) oz_crash ("oz_knl_quota_credit: quota use underflowed"); /* make sure it won't underflow */
    oz_hw_atomic_inc_long (&(quota -> use[quotatype]), -amount);
    oz_knl_quota_increfc (quota, inc);
  }
}

void oz_knl_quota_dump (OZ_Quota *quota)

{
  OZ_Quotatype i;

  OZ_KNL_CHKOBJTYPE (quota, OZ_OBJTYPE_QUOTA);

  oz_knl_printk ("oz_knl_quota_dump: %p, refc %d\n", quota, quota -> refcount);
  for (i = 0; i < OZ_QUOTATYPE_MAX; i ++) {
    oz_knl_printk ("  [%d]: %u/%u\n", i, quota -> use[i], quota -> max[i]);
  }
}
