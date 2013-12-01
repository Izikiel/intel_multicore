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
/*  Logical name processing routines					*/
/*									*/
/************************************************************************/

#include "ozone.h"

#include "oz_knl_event.h"
#include "oz_knl_handle.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_logname.h"
#include "oz_knl_objtype.h"
#include "oz_knl_procmode.h"
#include "oz_knl_quota.h"
#include "oz_knl_sdata.h"
#include "oz_knl_section.h"
#include "oz_knl_status.h"

#include "oz_sys_logname.h"
#include "oz_sys_syscall.h"

/************************************************************************/
/*									*/
/*  Create a logical name						*/
/*									*/
/*    Input:								*/
/*									*/
/*	h_lognamtbl = table handle					*/
/*	name = logical's name(secattr)					*/
/*	procmode = processor mode to create logical name in		*/
/*	lognamatr = logical name's attributes				*/
/*	nvalues = number of values					*/
/*	values = array of values					*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_logname_create = OZ_SUCCESS : successful			*/
/*	                     OZ_SUPERSEDED : successful, superseded old value
/*	*h_logname_r = handle to new logical name			*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_7 (logname_create, OZ_Handle, h_lognamtbl, const char *, name, OZ_Procmode, procmode, uLong, lognamatr, uLong, nvalues, OZ_Logvalue, values[], OZ_Handle *, h_logname_r)

{
  int name_l, si;
  uLong k, sts;
  OZ_Handle h_logname, h_object;
  OZ_Logname *logname, *lognamtbl;
  OZ_Logvalue *kvalues;
  OZ_Secattr *secattr;
  OZ_Seckeys *seckeys;
  OZ_Seclock **seclocks;

  secattr   = NULL;
  seckeys   = NULL;
  seclocks  = NULL;
  kvalues   = NULL;
  lognamtbl = NULL;

  if (procmode < cprocmode) procmode = cprocmode;
  si = oz_hw_cpu_setsoftint (0);

  sts = OZ_SUCCESS;
  if (h_lognamtbl != 0) {										/* get pointer to logical name table to put it in */
    sts = oz_knl_handle_takeout (h_lognamtbl, procmode, OZ_SECACCMSK_LOOK, OZ_OBJTYPE_UNKNOWN, &lognamtbl, NULL);
													/* just need LOOK access because oz_knl_logname_create  */
													/* check it and this might not be the actual table the  */
													/* logname is created in (it might be a list or logname */
													/* might contain table%name string in it)               */
  }
  if (sts == OZ_SUCCESS) {
    kvalues = OZ_KNL_PGPMALLOQ (nvalues * sizeof *kvalues);						/* copy values array to kvalues */
    if (kvalues == NULL) sts = OZ_EXQUOTAPGP;
  }
  if (sts == OZ_SUCCESS) {
    seclocks = OZ_KNL_NPPMALLOQ (nvalues * sizeof *seclocks);
    if (seclocks == NULL) sts = OZ_EXQUOTANPP;
  }
  if (sts == OZ_SUCCESS) {
    for (k = 0; k < nvalues; k ++) {
      kvalues[k] = values[k];
      if (kvalues[k].attr & OZ_LOGVALATR_OBJECT) {							/* ... converting handles to object pointers */
        h_object = (OZ_Handle)(OZ_Pointer)(kvalues[k].buff);
        sts = oz_knl_handle_takeout (h_object, procmode, OZ_SECACCMSK_LOOK, OZ_OBJTYPE_UNKNOWN, &(kvalues[k].buff), NULL);
        if (sts != OZ_SUCCESS) goto rtn;
        oz_knl_handle_objincrefc (OZ_KNL_GETOBJTYPE (kvalues[k].buff), kvalues[k].buff, 1);
        oz_knl_handle_putback (h_object);
      } else {
        sts = oz_knl_section_iolockz (procmode, OZ_LOGNAME_SIZEMAX, kvalues[k].buff, NULL, seclocks + k, NULL, NULL, NULL);
        if (sts != OZ_SUCCESS) goto rtn;
      }
    }
    secattr = oz_knl_thread_getdefcresecattr (NULL);
    sts = oz_knl_secattr_fromname (OZ_LOGNAME_MAXNAMSZ, name, &name_l, NULL, &secattr);
    if (sts != OZ_SUCCESS) goto rtn;
    seckeys = oz_knl_thread_getseckeys (NULL);
    sts = oz_knl_logname_create (lognamtbl, procmode, seckeys, secattr, lognamatr, name_l, name, nvalues, kvalues, &logname); /* create logical, get pointer to created logical */
    if ((sts == OZ_SUCCESS) || (sts == OZ_SUPERSEDED)) {
      if (h_logname_r != NULL) sts = oz_knl_handle_assign (logname, procmode, h_logname_r);
      oz_knl_logname_increfc (logname, -1);								/* release logname pointer */
    }
  }

rtn:
  if (kvalues != NULL) {
    while (k > 0) {
      if (!(kvalues[--k].attr & OZ_LOGVALATR_OBJECT)) oz_knl_section_iounlk (seclocks[k]);
      else oz_knl_handle_objincrefc (OZ_KNL_GETOBJTYPE (kvalues[k].buff), kvalues[k].buff, -1);
    }
    OZ_KNL_NPPFREE (seclocks);
    OZ_KNL_PGPFREE (kvalues);
  }
  if (lognamtbl != NULL) oz_knl_handle_putback (h_lognamtbl);
  oz_knl_secattr_increfc (secattr, -1);
  oz_knl_seckeys_increfc (seckeys, -1);

  oz_hw_cpu_setsoftint (si);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Lookup a logical name, return an handle to it			*/
/*									*/
/*    Input:								*/
/*									*/
/*	h_lognamtbl = 0 : use process then system directories		*/
/*	           else : logical name table handle			*/
/*	procmode    = outermost processor mode to consider		*/
/*	name        = name of logical to look up			*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_logname_lookup = OZ_SUCCESS : logical found		*/
/*	                              else : error status		*/
/*	*procmode_r    = logical name's processor mode			*/
/*	*lognamatr_r   = logical name's attributes			*/
/*	*nvalues_r     = logical name's number of values		*/
/*	*h_logname_r   = logical name handle				*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_7 (logname_lookup, OZ_Handle, h_lognamtbl, OZ_Procmode, procmode, const char *, name, OZ_Procmode *, procmode_r, uLong *, lognamatr_r, uLong *, nvalues_r, OZ_Handle *, h_logname_r)

{
  int si;
  uLong sts;
  OZ_Logname *logname, *lognamtbl;
  OZ_Secaccmsk secaccmsk;
  OZ_Secattr *secattr;
  OZ_Seckeys *seckeys;
  OZ_Seclock *name_lock;

  si = oz_hw_cpu_setsoftint (0);

  name_lock = NULL;
  sts = oz_knl_section_blockz (cprocmode, OZ_LOGNAME_MAXNAMSZ, name, NULL, &name_lock);
  if (sts != OZ_SUCCESS) goto rtn;

  /* Get pointer to logical name table.  NULL means use the default directories. */

  lognamtbl = NULL;
  if (h_lognamtbl != 0) {
    sts = oz_knl_handle_takeout (h_lognamtbl, cprocmode, OZ_SECACCMSK_LOOK, OZ_OBJTYPE_LOGNAME, &lognamtbl, NULL);
    if (sts != OZ_SUCCESS) goto rtn;
  }

  /* Lookup the logical name in those tables and get info */

  sts = oz_knl_logname_lookup (lognamtbl, procmode, strlen (name), name, procmode_r, lognamatr_r, nvalues_r, NULL, &logname, NULL);
  if (h_lognamtbl != 0) oz_knl_handle_putback (h_lognamtbl);
  if (sts != OZ_SUCCESS) goto rtn;

  /* If requested, return an handle to the found logical */

  if (h_logname_r != NULL) {
    sts = oz_knl_handle_assign (logname, cprocmode, h_logname_r);
  }

  /* Anyway, all done with logname pointer */

  oz_knl_logname_increfc (logname, -1);

rtn:
  if (name_lock != NULL) oz_knl_section_bunlock (name_lock);
  oz_hw_cpu_setsoftint (si);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Get logical name's attributes					*/
/*									*/
/*    Input:								*/
/*									*/
/*	h_logname = logical name handle					*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_logname_getobj = OZ_SUCCESS : successful			*/
/*	                              else : error status		*/
/*	*namebuff = filled in with logical's name			*/
/*	*namerlen = filled in with length of logical name length	*/
/*	*procmode_r = filled in with logical's processor mode		*/
/*	*lognamatr_r = filled in with logical's attributes		*/
/*	*nvalues_r = filled in with number of values			*/
/*	*h_lognamtbl_r = filled in with handle to logical name table	*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_10 (logname_getattr, OZ_Handle, h_logname, uLong, namesize, char *, namebuff, uLong *, namerlen, OZ_Procmode *, procmode_r, uLong *, lognamatr_r, uLong *, nvalues_r, OZ_Handle *, h_lognamtbl_r, uLong, index, uLong *, logvalatr_r)

{
  const char *name;
  const OZ_Logvalue *values;
  int si;
  uLong nvalues, sts;
  OZ_Logname *logname, *lognamtbl;

  if (h_lognamtbl_r != NULL) *h_lognamtbl_r = 0;

  si = oz_hw_cpu_setsoftint (0);
  sts = oz_knl_handle_takeout (h_logname, cprocmode, OZ_SECACCMSK_READ, OZ_OBJTYPE_LOGNAME, &logname, NULL); /* convert logical name handle to pointer */
  if (sts == OZ_SUCCESS) {
    if (namebuff != NULL) {									/* maybe caller wants logical's name */
      name = oz_knl_logname_getname (logname);
      strncpyz (namebuff, name, namesize);
      if (namerlen != NULL) *namerlen = strlen (namebuff);
    } else if (namerlen != NULL) {
      *namerlen = strlen (oz_knl_logname_getname (logname));
    }
    sts = oz_knl_logname_getval (logname, procmode_r, lognamatr_r, &nvalues, &values, &lognamtbl); /* get the attributes and point to values array */
    if (sts == OZ_SUCCESS) {
      if (lognamtbl != NULL) {
        if (h_lognamtbl_r != NULL) {								/* see if caller wants table handle */
          sts = oz_knl_handle_assign (lognamtbl, cprocmode, h_lognamtbl_r);			/* ok, assign handle to logical name table */
        }
        oz_knl_logname_incobjrefc (lognamtbl, -1, NULL);					/* release logical name table pointer */
      }
      if (nvalues_r != NULL) *nvalues_r = nvalues;						/* maybe return number of values in the values array */
      if ((logvalatr_r != NULL) && (index < nvalues)) *logvalatr_r = values[index].attr;	/* maybe return attributes of one of the values */
    }
    oz_knl_handle_putback (h_logname);								/* put logical name pointer back */
  }
  oz_hw_cpu_setsoftint (si);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Get logical name's string value					*/
/*									*/
/*    Input:								*/
/*									*/
/*	h_logname = logical name handle					*/
/*	index     = value index						*/
/*	size      = buffer size						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_logname_getval = OZ_SUCCESS : successful			*/
/*	                              else : error status		*/
/*	*buff = value string						*/
/*	*rlen = string length						*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_9 (logname_getval, OZ_Handle, h_logname, uLong, index, uLong *, logvalatr_r, uLong, size, char *, buff, uLong *, rlen, OZ_Handle, *h_object_r, OZ_Objtype, objtype, OZ_Objtype *, objtype_r)

{
  char *p, tmpbuf[sizeof(OZ_Pointer)*2+32];
  const OZ_Logvalue *values;
  int si;
  uLong len, lognamatr, nvalues, sts;
  OZ_Logname *logname;
  void *object;

  if (rlen != NULL) *rlen = 0;
  if (objtype_r  != NULL) *objtype_r = OZ_OBJTYPE_UNKNOWN;
  if (h_object_r != NULL) *h_object_r = 0;

  si = oz_hw_cpu_setsoftint (0);

  sts = oz_knl_handle_takeout (h_logname, cprocmode, OZ_SECACCMSK_READ, OZ_OBJTYPE_LOGNAME, &logname, NULL); /* convert logical name handle to pointer */
  if (sts != OZ_SUCCESS) goto rtn;

  sts = oz_knl_logname_getval (logname, NULL, &lognamatr, &nvalues, &values, NULL);		/* get pointer to value array */
  if (sts == OZ_SUCCESS) {
    if (index >= nvalues) sts = OZ_SUBSCRIPT;							/* if subscript out of range, return that error status */
    else {
      if (logvalatr_r != NULL) *logvalatr_r = values[index].attr;				/* ok, maybe return value and name attributes */
      p = values[index].buff;									/* assume not an object */
      if (values[index].attr & OZ_LOGVALATR_OBJECT) {
        sts = oz_knl_logname_objstr (logname, index, sizeof tmpbuf, tmpbuf);			/* object, convert to string */
        if (sts != OZ_SUCCESS) oz_crash ("oz_sys_logname_getval: error %u getting objstr", sts);
        p = tmpbuf;										/* ... and point to string */
      }
      len = strlen (p);										/* get length of string */
      if (buff != NULL) strncpyz (buff, p, size);						/* copy as much as we can */
      if (rlen != NULL) *rlen = len;								/* return string length if requested */
      if ((h_object_r != NULL) || (objtype_r != NULL)) {					/* see if caller wants object handle returned */
        if (values[index].attr & OZ_LOGVALATR_OBJECT) {						/* see if object type logical */
          sts = oz_knl_logname_getobj (logname, index, objtype, &object);			/* if so, get object pointer */
          if (sts == OZ_SUCCESS) {
            if (objtype_r  != NULL) *objtype_r = OZ_KNL_GETOBJTYPE (object);			/* maybe return object type */
            if (h_object_r != NULL) sts = oz_knl_handle_assign (object, cprocmode, h_object_r);	/* maybe assign an handle to it */
            oz_knl_logname_incobjrefc (object, -1, NULL);					/* get rid of ref count made by oz_knl_logname_getobj */
          }
        }
      }
    }
  }
  oz_knl_handle_putback (h_logname);								/* release logical name */

rtn:
  oz_hw_cpu_setsoftint (si);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Get logical tabe entry						*/
/*									*/
/*    Input:								*/
/*									*/
/*	h_lognamtbl = logical table handle				*/
/*	*h_logname_r = last logical name handle				*/
/*	               or 0 to start from beginning of table		*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_logname_gettblent = OZ_SUCCESS : successful		*/
/*	                                 else : error status		*/
/*	*h_logname_r = logical name handle, 0 if end of table		*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_2 (logname_gettblent, OZ_Handle, h_lognamtbl, OZ_Handle *, h_logname_r)

{
  int si;
  uLong sts, sts2;
  OZ_Handle h_logname;
  OZ_Logname *logname, *lognamtbl, *oldlogname;

  si = oz_hw_cpu_setsoftint (0);

  sts = oz_knl_handle_takeout (h_lognamtbl, cprocmode, OZ_SECACCMSK_READ, OZ_OBJTYPE_LOGNAME, &lognamtbl, NULL); /* convert logical table handle to pointer */
  if (sts != OZ_SUCCESS) goto rtn;

  h_logname = *h_logname_r;									/* see if this is a continuation */
  logname = NULL;										/* assume it is not a continuation */
  if (h_logname != 0) {
    sts = oz_knl_handle_takeout (h_logname, cprocmode, OZ_SECACCMSK_LOOK, OZ_OBJTYPE_LOGNAME, &logname, NULL); /* continuation, get previous logical pointer */
    if (sts != OZ_SUCCESS) goto rtn_1;
    oz_knl_logname_increfc (logname, 1);
    oz_knl_handle_putback (h_logname);
  }

getnext:
  oldlogname = logname;										/* save pointer to input logical name */
  sts = oz_knl_logname_gettblent (lognamtbl, &logname);						/* advance pointer to next logical name */
												/* this decrements input logical name ref count (to 1) */
												/* this increments output logical name ref count (to 1) */

  if (logname != oldlogname) {									/* see if pointer actually moved */
    if (oldlogname != NULL) {									/* release input logical handle */
      *h_logname_r = 0;
      oz_knl_handle_release (h_logname, cprocmode);
      h_logname = 0;
    }
    if (logname != NULL) {
      sts2 = oz_knl_handle_assign (logname, cprocmode, h_logname_r);				/* assign output logical handle */
      if (sts2 == OZ_SECACCDENIED) goto getnext;						/* repeat if they don't have access */
      if ((sts2 != OZ_SUCCESS) && (sts == OZ_SUCCESS)) sts = sts2;
    }
  }

  if (logname != NULL) oz_knl_logname_increfc (logname, -1);					/* release output logical pointer */

rtn_1:
  oz_knl_handle_putback (h_lognamtbl);								/* put logical table handle back */
rtn:
  oz_hw_cpu_setsoftint (si);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Delete logical name							*/
/*									*/
/*    Input:								*/
/*									*/
/*	h_logname = logical name handle					*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_logname_delete = OZ_SUCCESS : successful			*/
/*	                              else : error status		*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_1 (logname_delete, OZ_Handle, h_logname)

{
  int si;
  uLong sts;
  OZ_Logname *logname;

  si = oz_hw_cpu_setsoftint (0);
  sts = oz_knl_handle_takeout (h_logname, cprocmode, OZ_SECACCMSK_WRITE, OZ_OBJTYPE_LOGNAME, &logname, NULL); /* convert logical name handle to pointer */
  if (sts == OZ_SUCCESS) {
    oz_knl_logname_increfc (logname, 1);
    oz_knl_handle_putback (h_logname);
    oz_knl_logname_delete (logname);							/* delete logical name & dec ref count */
  }
  oz_hw_cpu_setsoftint (si);
  return (sts);
}
