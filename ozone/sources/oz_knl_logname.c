//+++2002-08-17
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
//---2002-08-17

/************************************************************************/
/*									*/
/*  Logical name processing routines					*/
/*									*/
/************************************************************************/

#define _OZ_KNL_LOGNAME_C

#include "ozone.h"

#include "oz_knl_event.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_logname.h"
#include "oz_knl_objtype.h"
#include "oz_knl_printk.h"
#include "oz_knl_procmode.h"
#include "oz_knl_sdata.h"
#include "oz_knl_status.h"
#include "oz_knl_thread.h"
#include "oz_knl_userjob.h"
#include "oz_sys_xprintf.h"

struct OZ_Logname { OZ_Objtype objtype;		/* object type OZ_OBJTYPE_LOGNAME */
                    OZ_Logname *next;		/* next name in table */
                    Long refcount;		/* reference count (only use oz_hw_atomic_inc_long) */
                    OZ_Logname *lognamtbl;	/* the logical table it is in */
                    int name_l;			/* strlen (name) */
                    char *name;			/* pointer to null terminated logical name string */
                    OZ_Procmode procmode;	/* processor mode it is defined as */
                    OZ_Secattr *secattr;	/* security attributes (who can access me) */
                    uLong lognamatr;		/* logical name attribute flags */
                    OZ_Event *tablevent;	/* logical name table locking event flag */
                    OZ_Logname *lognames;	/* if table, pointer to list of logical names in the table */
                    uLong nvalues;		/* number of entries in values table */
                    OZ_Logvalue values[1];	/* if not table, array of value attribute/buffer pairs */
                  };
						/* followed by the rest of the values[] array elements */
						/* followed by things pointed to by values[] array */
						/* followed by the name string */

struct OZ_Lognamesearch { OZ_Objtype objtype;
                          uLong name_lognamatr;
                          uLong name_nvalues;
                          const OZ_Logvalue *name_values;
                          OZ_Logname *name_logname;
                          uLong name_valuei;
                          OZ_Lognamesearch *recurse;
                        };


static uLong logname_search (int level, 
                             OZ_Lognamesearch **lognamesearch_r, 
                             OZ_Procmode procmode, 
                             const char *tablename, 
                             const char *name, 
                             OZ_Logname *logname, 
                             OZ_Logname **logname_r, 
                             uLong *index_r, 
                             OZ_Procmode *procmode_r, 
                             uLong *lognamatr_r, 
                             uLong *nvalues_r, 
                             const OZ_Logvalue **values_r, 
                             OZ_Logname **lognamtbl_r);
static uLong logname_create (OZ_Logname *lognamtbl, OZ_Procmode procmode, OZ_Seckeys *seckeys, OZ_Secattr *secattr, uLong lognamatr, int name_l, const char *name, uLong nvalues, const OZ_Logvalue values[], OZ_Logname **logname_r);
static uLong logname_lookup (OZ_Logname *given_lognamtbl, OZ_Procmode procmode, int given_name_l, const char *given_name, 
                             OZ_Procmode *procmode_r, uLong *lognamatr_r, uLong *nvalues_r, 
                             const OZ_Logvalue *values_r[], OZ_Logname **logname_r, OZ_Logname **lognamtbl_r, 
                             int level);
static OZ_Logname *cvtlognamtblobj (OZ_Logname *lognamtbl);
static uLong lognamtbl_lock (OZ_Logname *lognamtbl);
static void lognamtbl_unlock (OZ_Logname *lognamtbl);
static void markdelete (OZ_Logname *logname);

/************************************************************************/
/*									*/
/*  Search recursively and repetitively through the tables for a 	*/
/*  logical name.  Return the values one at a time.			*/
/*									*/
/*    Input:								*/
/*									*/
/*	*lognamesearch_r = NULL : initialize, return first value	*/
/*	procmode  = don't return anything at an outer mode than this	*/
/*	tablename = NULL : search OZ_PROCESS_DIRECTORY then OZ_SYSTEM_DIRECTORY
/*	            else : table name to search (can be a list logical)	*/
/*	name      = name to search for (can be recursive, can be a list)*/
/*	value_r   = where to return value string pointer		*/
/*	logname_r = where to return logical name block pointer		*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_logname_search = OZ_SUCCESS : value found		*/
/*	                          **value_r = value			*/
/*	                         *logname_r = logname struct		*/
/*	                                      (its refc has been incd)	*/
/*	                       OZ_NOLOGNAM : value not found, no more values
/*	                              else : error status		*/
/*									*/
/*    Note:								*/
/*									*/
/*	if both value_r and logname_r are NULL, any search context is 	*/
/*	released							*/
/*									*/
/************************************************************************/

uLong oz_knl_logname_search (OZ_Lognamesearch **lognamesearch_r, 
                             OZ_Procmode procmode, 
                             const char *tablename, 
                             const char *name, 
                             OZ_Logname **logname_r, 
                             uLong *index_r, 
                             OZ_Procmode *procmode_r, 
                             uLong *lognamatr_r, 
                             uLong *nvalues_r, 
                             const OZ_Logvalue **values_r, 
                             OZ_Logname **lognamtbl_r)

{
  return (logname_search (OZ_LOGNAME_MAXLEVEL, lognamesearch_r, procmode, tablename, name, NULL, 
		logname_r, index_r, procmode_r, lognamatr_r, nvalues_r, values_r, lognamtbl_r));
}

static uLong logname_search (int level, 
                             OZ_Lognamesearch **lognamesearch_r, 
                             OZ_Procmode procmode, 
                             const char *tablename, 
                             const char *name, 
                             OZ_Logname *logname, 
                             OZ_Logname **logname_r, 
                             uLong *index_r, 
                             OZ_Procmode *procmode_r, 
                             uLong *lognamatr_r, 
                             uLong *nvalues_r, 
                             const OZ_Logvalue **values_r, 
                             OZ_Logname **lognamtbl_r)

{
  const OZ_Logvalue *name_values, *table_values;
  uLong i, name_lognamatr, name_nvalues, sts, table_lognamatr, table_nvalues;
  OZ_Logname *name_logname, *obj_logname, *table_logname;
  OZ_Lognamesearch *lns;

  lns = *lognamesearch_r;
  OZ_KNL_CHKOBJTYPE (lns, OZ_OBJTYPE_LOGNAMESEARCH);

  if (level < 0) return (OZ_EXMAXLOGNAMLVL);

  if ((lns == NULL) && ((logname_r != NULL) || (values_r != NULL))) {

    if (logname != NULL) name_logname = logname;
    else {

      /* If table name not given, use OZ_{PROCESS,JOB,USER,SYSTEM}_DIRECTORY */

      if (tablename == NULL) {
        for (i = 0; i < sizeof oz_s_logname_directorynames / sizeof oz_s_logname_directorynames[0]; i ++) {
          sts = logname_search (level - 1, lognamesearch_r, procmode, oz_s_logname_directorynames[i], name, NULL, 
				logname_r, index_r, procmode_r, lognamatr_r, nvalues_r, values_r, lognamtbl_r);
          if (sts != OZ_NOLOGNAME) break;
        }
        return (sts);
      }

      /* Otherwise, scan the directories for the given tablename.  If not found, there's nothing we can do */

      sts = oz_knl_logname_lookup (NULL, procmode, strlen (tablename), tablename, NULL, 
                                   &table_lognamatr, &table_nvalues, &table_values, &table_logname, NULL);
      if (sts != OZ_SUCCESS) return (sts);

      /* If it is a table, look for the requested logical name.  Return on any failure. */
      /* In any case, we are done with 'table_logname' pointer, so dec table ref count. */

      sts = oz_knl_logname_lookup (table_logname, procmode, strlen (name), name, NULL, &name_lognamatr, &name_nvalues, &name_values, &name_logname, NULL);
      oz_knl_logname_increfc (table_logname, -1);
      if (sts != OZ_SUCCESS) return (sts);
    }

    /* The name was found in that table, so make a search context block so we will return the logical's value array. */
    /* Leave the logical's ref count incremented so it doesn't disappear on us.                                      */

    lns = OZ_KNL_PGPMALLOQ (sizeof *lns);	/* allocate the block */
    if (lns == NULL) return (OZ_EXQUOTAPGP);
    lns -> objtype        = OZ_OBJTYPE_LOGNAMESEARCH;
    lns -> name_lognamatr = name_lognamatr;	/* the attribute bits */
    lns -> name_nvalues   = name_nvalues;	/* number of elements in 'values' array */
    lns -> name_values    = name_values;	/* pointer to 'values' array */
    lns -> name_logname   = name_logname;	/* pointer to logname struct */
    lns -> name_valuei    = 0;			/* index of first value to return */
    lns -> recurse        = NULL;		/* we haven't recursed yet */
    *lognamesearch_r = lns;
  }

  /* If they are forcing a reset or we have reached the end of the value list, release context block and return 'No Logical Name' status */
  /* If the logical is a table, we pretend it has exactly 1 value                                                                        */

  if (((values_r == NULL) && (logname_r == NULL)) || (lns -> name_valuei >= ((lns -> name_lognamatr & OZ_LOGNAMATR_TABLE) ? 1 : lns -> name_nvalues))) {
    if (lns != NULL) {								/* see if context block exists */
      logname_search (0, &(lns -> recurse), procmode, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL); /* if so, release any recursion */
      oz_knl_logname_increfc (lns -> name_logname, -1);				/* release the logical name pointer */
      OZ_KNL_PGPFREE (lns);							/* free off the logical name block */
      *lognamesearch_r = NULL;							/* tell caller we have no more context block */
    }
    return (OZ_NOLOGNAME);
  }

  /* If logical value is not terminal, do a recursive search */
  /* If no recursive value found, pretend it was terminal    */

  if (!(lns -> name_lognamatr & OZ_LOGNAMATR_TABLE) && !(lns -> name_values[lns->name_valuei].attr & OZ_LOGVALATR_TERMINAL)) {
    if (lns -> name_values[lns->name_valuei].attr & OZ_LOGVALATR_OBJECT) {
      obj_logname = lns -> name_values[lns->name_valuei++].buff;				/* get pointer to object */
      if (OZ_KNL_GETOBJTYPE (obj_logname) == OZ_OBJTYPE_LOGNAME) {				/* see if it is a logical name object */
        oz_knl_logname_increfc (obj_logname, 1);						/* ok, process that logical name */
        sts = logname_search (level - 1, &(lns -> recurse), procmode, tablename, NULL, obj_logname, 
				logname_r, index_r, procmode_r, lognamatr_r, nvalues_r, values_r, lognamtbl_r);
        if (sts != OZ_NOLOGNAME) return (sts);
      }
    } else {
      sts = logname_search (level - 1, &(lns -> recurse), procmode, tablename, lns -> name_values[lns->name_valuei++].buff, NULL, 
				logname_r, index_r, procmode_r, lognamatr_r, nvalues_r, values_r, lognamtbl_r);
      if (sts != OZ_NOLOGNAME) return (sts);
    }
    lns -> name_valuei --;
  }

  /* Name is terminal, return value entry and increment value index for next time */

  name_logname = lns -> name_logname;
  if (logname_r   != NULL) *logname_r   = name_logname;
  if (index_r     != NULL) *index_r     = lns -> name_valuei ++;
  if (procmode_r  != NULL) *procmode_r  = name_logname -> procmode;
  if (lognamatr_r != NULL) *lognamatr_r = name_logname -> lognamatr;
  if (nvalues_r   != NULL) *nvalues_r   = name_logname -> nvalues;
  if (values_r    != NULL) *values_r    = name_logname -> values;
  if (lognamtbl_r != NULL) *lognamtbl_r = name_logname -> lognamtbl;

  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Create logical name and put in logical name table			*/
/*									*/
/*    Input:								*/
/*									*/
/*	lognamtbl = logical name table to put logical in		*/
/*	            NULL to not put in any table			*/
/*	            (used for creating directories)			*/
/*	procmode  = processor mode to create logical at			*/
/*	lognamatr = attributes to give logical name			*/
/*	name      = logical name string					*/
/*	nvalues   = number of values in array				*/
/*	values    = pointer to value array				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_logname_create = OZ_SUCCESS : new name created		*/
/*	                     OZ_SUPERSEDED : old name superseded	*/
/*	                              else : error status		*/
/*	*logname_r = pointer to logical name struct created/superseded	*/
/*									*/
/************************************************************************/

uLong oz_knl_logname_creobj (OZ_Logname *lognamtbl, 
                             OZ_Procmode procmode, 
                             OZ_Seckeys *seckeys, 
                             OZ_Secattr *secattr, 
                             uLong lognamatr, int name_l, const char *name, void *object, OZ_Logname **logname_r)

{
  OZ_Logvalue value;

  value.attr = OZ_LOGVALATR_OBJECT;
  value.buff = object;
  return (logname_create (lognamtbl, procmode, seckeys, secattr, lognamatr, name_l, name, 1, &value, logname_r));
}

uLong oz_knl_logname_crestr (OZ_Logname *lognamtbl, OZ_Procmode procmode, OZ_Seckeys *seckeys, OZ_Secattr *secattr, uLong lognamatr, int name_l, const char *name, const char *string, OZ_Logname **logname_r)

{
  OZ_Logvalue value;

  value.attr = 0;
  value.buff = (void *)string;
  return (logname_create (lognamtbl, procmode, seckeys, secattr, lognamatr, name_l, name, 1, &value, logname_r));
}

uLong oz_knl_logname_create (OZ_Logname *lognamtbl, 
                             OZ_Procmode procmode, 
                             OZ_Seckeys *seckeys, 
                             OZ_Secattr *secattr, 
                             uLong lognamatr, 
                             int name_l, 
                             const char *name, 
                             uLong nvalues, 
                             const OZ_Logvalue values[], 
                             OZ_Logname **logname_r)

{
  return (logname_create (lognamtbl, 
                          procmode, 
                          seckeys, 
                          secattr, 
                          lognamatr, 
                          name_l, 
                          name, 
                          nvalues, 
                          values, 
                          logname_r));
}

static uLong logname_create (OZ_Logname *given_lognamtbl, 
                             OZ_Procmode procmode, 
                             OZ_Seckeys *seckeys, 
                             OZ_Secattr *secattr, 
                             uLong lognamatr, 
                             int given_name_l, 
                             const char *given_name, 
                             uLong nvalues, 
                             const OZ_Logvalue values[], 
                             OZ_Logname **logname_r)

{
  char *cp, *q;
  const char *name;
  int cmp, name_l;
  uLong i, sts, totalsize;
  OZ_Event *tablevent;
  OZ_Logname **llogname, *logname, *lognamtbl, *nlogname, *xlogname;
  void *object;

  xlogname = NULL;

  /* If table attribute (ie, creating a table), nvalues must be zero */

  if ((lognamatr & OZ_LOGNAMATR_TABLE) && (nvalues != 0)) return (OZ_LOGNAMTBLNZ);

  /* Make sure logical name itself is not too big */

  given_name_l = strnlen (given_name, given_name_l);
  totalsize    = given_name_l + 1;
  if (totalsize > OZ_LOGNAME_MAXNAMSZ) return (OZ_LOGNAMETOOBIG);

  /* Check size of the array (+1 accounts for the null string terminators) */

  if (nvalues > OZ_LOGNAME_SIZEMAX / (sizeof *values + 1)) return (OZ_LOGNAMETOOBIG);
  totalsize += nvalues * (sizeof *values + 1);

  /* Make sure the total size of all strings won't be too big */

  for (i = 0; i < nvalues; i ++) {
    if (values[i].attr & OZ_LOGVALATR_OBJECT) {
      sts = oz_knl_logname_incobjrefc (values[i].buff, 0, NULL);	/* object type doesn't consume space */
      if (sts != OZ_SUCCESS) return (sts);				/* but let's validate it while we're here */
    } else {
      totalsize += strlen (values[i].buff);				/* strings consume space */
    }
    if (totalsize > OZ_LOGNAME_SIZEMAX) return (OZ_LOGNAMETOOBIG);
  }

  /* Get table to be used indicated in the name itself.  Note that this increments the table's ref count no matter if it is the given table or not. */

  sts = oz_knl_logname_parse (given_lognamtbl, procmode, given_name_l, given_name, &lognamtbl, &name_l, &name);
  if (sts != OZ_SUCCESS) return (sts);

  /* Determine table and lock it - if the given table name is a list of tables, use the first one */

  if (lognamtbl != NULL) {
    for (i = OZ_LOGNAME_MAXLEVEL; i > 0; i --) {			/* only nest this many times */
      if (lognamtbl -> lognamatr & OZ_LOGNAMATR_TABLE) goto gottable;	/* if we're at a table, we're done */
      if (lognamtbl -> nvalues == 0) {					/* not a table, it must have values */
        sts = OZ_NOLOGNAME;
        goto rtn_decrefc;
      }
      if (lognamtbl -> values[0].attr & OZ_LOGVALATR_OBJECT) {		/* ok, see if first value is an object */
        logname = lognamtbl -> values[0].buff;				/* get pointer to the object */
        logname = cvtlognamtblobj (logname);				/* convert to corresponding logical name */
        if (logname == NULL) {
          sts = OZ_INVOBJTYPE;						/* - some wacky type of object that doesn't have a table */
          goto rtn_decrefc;
        }
        oz_knl_logname_increfc (logname, 1);				/* ok, increment the table's ref count */
      } else {
        sts = logname_lookup (NULL, procmode, strlen (lognamtbl -> values[0].buff), lognamtbl -> values[0].buff, NULL, NULL, NULL, NULL, &logname, NULL, i - 1);
        if (sts != OZ_SUCCESS) goto rtn_decrefc;			/* name of a table, look up the table in the directories & inc its ref count */
      }
      oz_knl_logname_increfc (lognamtbl, -1);				/* done with previous table */
      lognamtbl = logname;						/* use this logical as the table */
    }
    sts = OZ_EXMAXLOGNAMLVL;
    goto rtn_decrefc;
gottable:    
    sts = oz_knl_security_check (OZ_SECACCMSK_WRITE, seckeys, lognamtbl -> secattr); /* got table, make sure caller can write it */
    if (sts == OZ_SUCCESS) sts = lognamtbl_lock (lognamtbl);		/* caller can write it, lock it */
    if (sts != OZ_SUCCESS) goto rtn_decrefc;
  }

  /* Search for the name already in the table -                                         */
  /* If one exists at same access mode that is marked NOSUPERSEDE, return error status  */
  /* If one exists at inner access mode that is marked NOOUTERMODE, return error status */

  if (lognamtbl != NULL) {
    for (logname = lognamtbl -> lognames; logname != NULL; logname = logname -> next) {			/* scan all logicals in table */
      if (logname -> name_l != name_l) continue;
      if (memcmp (logname -> name, name, name_l) != 0) continue;					/* skip it if the name doesn't match */
      if (logname -> procmode == procmode) {								/* see if exact procmode match */
        if (logname -> lognamatr & OZ_LOGNAMATR_NOSUPERSEDE) {						/* match, maybe existing logical can't be superseded */
          sts = OZ_NOSUPERSEDE;
          logname = NULL;
          goto unlock_it;
        }
        OZ_HW_ATOMIC_INCBY1_LONG (logname -> refcount);							/* it can be superseded, inc its ref count */
        markdelete (logname);										/* mark for delete (ie, remove from table) */
        logname -> next = xlogname;									/* link to list of superseded logicals */
        xlogname = logname;
        continue;
      }
      if ((logname -> procmode < procmode) && (logname -> lognamatr & OZ_LOGNAMATR_NOOUTERMODE)) {	/* old lnm more privileged, see if it allows outermode lnm's */
        sts = OZ_NOOUTERMODE;										/* if not, it is an error */
        logname = NULL;
        goto unlock_it;
      }
      if ((lognamatr & OZ_LOGNAMATR_NOOUTERMODE) && (logname -> procmode > procmode)) {			/* old lnm less privd and new doesn't allow outermodes, */
        OZ_HW_ATOMIC_INCBY1_LONG (logname -> refcount);							/* ... supersede it */
        markdelete (logname);
        logname -> next = xlogname;
        xlogname = logname;
      }
    }
  }

  sts = OZ_SUCCESS;
  if (xlogname != NULL) sts = OZ_SUPERSEDED;

  /* Allocate a buffer that is large enough to hold the name and all the values */

  logname = OZ_KNL_PGPMALLOQ (totalsize + sizeof *logname);
  if (logname == NULL) {
    sts = OZ_EXQUOTAPGP;
    goto unlock_it;
  }

  /* If we are creating a table, create table's locking event flag */

  tablevent = NULL;
  if (lognamatr & OZ_LOGNAMATR_TABLE) {
    sts = oz_knl_event_create (21, "oz_knl_logname create", NULL, &tablevent);
    if (sts != OZ_SUCCESS) {
      OZ_KNL_PGPFREE (logname);
      goto unlock_it;
    }
    oz_knl_event_set (tablevent, 1);
  }

  /* Point to where the the character strings will go (they follow the values array) */

  cp = (char *)(logname -> values + nvalues);

  /* Fill in fixed part of block */

  logname -> objtype   = OZ_OBJTYPE_LOGNAME;
  logname -> name_l    = name_l;
  logname -> name      = cp;
  logname -> procmode  = procmode;
  logname -> secattr   = secattr;
  logname -> lognamatr = lognamatr;
  logname -> tablevent = tablevent;
  logname -> lognames  = NULL;
  logname -> nvalues   = nvalues;
  logname -> refcount  = 1;
  oz_knl_secattr_increfc (logname -> secattr, 1);

  /* Put the name in */

  memcpy (cp, name, name_l);
  cp += name_l;
  *(cp ++) = 0;

  /* Link to its logical name table */

  logname -> next      = NULL;
  logname -> lognamtbl = lognamtbl;
  if (lognamtbl != NULL) {
    for (llogname = &(lognamtbl -> lognames); (nlogname = *llogname) != NULL; llogname = &(nlogname -> next)) {
      cmp = strcmp (nlogname -> name, logname -> name);
      if (cmp < 0) continue;
      if (cmp > 0) break;
      if (nlogname -> procmode > logname -> procmode) break;
    }
    *llogname = logname;
    logname -> next = nlogname;
  }

  /* Copy the values (if table, nvalues is zero) */

  for (i = 0; i < nvalues; i ++) {					/* loop through the values array */
    logname -> values[i].attr = values[i].attr;				/* copy the attributes */
    if (logname -> values[i].attr & OZ_LOGVALATR_OBJECT) {		/* see if it is an object pointer */
      logname -> values[i].buff = values[i].buff;			/* if so, just use pointer to object as is */
    } else {
      strcpy (cp, values[i].buff);					/* just a string, copy it in */
      logname -> values[i].buff = cp;					/* set up pointer to character string */
      cp += strlen (cp) + 1;						/* point just past the terminating null */
    }
  }

unlock_it:
  if (lognamtbl != NULL) lognamtbl_unlock (lognamtbl);			/* unlock parent table */

									/* now that parent table is unlocked it is ok to increment object ref counts */
									/* just in case an object count being incremented is the table itself we can't deadlock */

  if (logname != NULL) {
    for (i = 0; i < logname -> nvalues; i ++) {				/* loop through the values array */
      if (logname -> values[i].attr & OZ_LOGVALATR_OBJECT) {		/* see if object-type value */
        sts = oz_knl_logname_getobj (logname, i, OZ_OBJTYPE_UNKNOWN, &object); /* inc its reference count */
        if (sts != OZ_SUCCESS) oz_crash ("oz_knl_logname_create: error %u incing object %s ref count", sts, logname -> values[i].buff);
      }
    }
  }

  /* Return pointer if caller wants it - if not dec logicals ref count (its in the table with ref count 0) */

  if (logname_r != NULL) *logname_r = logname;
  else if (logname != NULL) oz_knl_logname_increfc (logname, -1);

rtn_decrefc:
  if (lognamtbl != NULL) oz_knl_logname_increfc (lognamtbl, -1);	/* this was incd by the logname_parse routine */

  while ((logname = xlogname) != NULL) {				/* finish deleting any superseded logicals */
    xlogname = logname -> next;
    oz_knl_logname_increfc (logname, -1);
  }

  return (sts);								/* return completion status */
}

/************************************************************************/
/*									*/
/*  Lookup a logical name						*/
/*									*/
/*    Input:								*/
/*									*/
/*	lognamtbl = NULL : use process then system directories		*/
/*	            else : pointer to logical name table		*/
/*	procmode  = processor mode of outermost name to consider	*/
/*	seckeys   = security keys to access logical with		*/
/*	name      = logical name to search for				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_logname_lookup = OZ_SUCCESS : name was found		*/
/*	                              else : error status		*/
/*	*procmode_r  = processor mode of name found			*/
/*	*lognamatr_r = logical name attributes				*/
/*	*nvalues_r   = number of values for the name			*/
/*	*values_r    = pointer to value list array			*/
/*	*logname_r   = pointer to logical name entry			*/
/*	*lognamtbl_r = pointer to table it was found in			*/
/*									*/
/*    Note:								*/
/*									*/
/*	logical name ref count incremented iff logname_r is not NULL	*/
/*	logical table ref count incremented iff lognamtbl_r is not NULL	*/
/*									*/
/************************************************************************/

uLong oz_knl_logname_lookup (OZ_Logname *lognamtbl, OZ_Procmode procmode, int name_l, const char *name, 
                             OZ_Procmode *procmode_r, uLong *lognamatr_r, uLong *nvalues_r, 
                             const OZ_Logvalue *values_r[], OZ_Logname **logname_r, OZ_Logname **lognamtbl_r)

{
  return (logname_lookup (lognamtbl, procmode, name_l, name, procmode_r, lognamatr_r, nvalues_r, values_r, logname_r, lognamtbl_r, OZ_LOGNAME_MAXLEVEL));
}

/* 'level' is used for recursive calls to keep from going too deep */

static uLong logname_lookup (OZ_Logname *given_lognamtbl, OZ_Procmode procmode, int given_name_l, const char *given_name, 
                             OZ_Procmode *procmode_r, uLong *lognamatr_r, uLong *nvalues_r, 
                             const OZ_Logvalue *values_r[], OZ_Logname **logname_r, OZ_Logname **lognamtbl_r, 
                             int level)

{
  const char *name;
  int name_l;
  OZ_Logname *logname, *lognamtbl, *outermost_logname;
  OZ_Procmode outermost_procmode;
  uLong i, sts;
  void *object;

  OZ_KNL_CHKOBJTYPE (given_lognamtbl, OZ_OBJTYPE_LOGNAME);

  if (logname_r   != NULL) *logname_r   = NULL;
  if (lognamtbl_r != NULL) *lognamtbl_r = NULL;

  if (level < 0) return (OZ_EXMAXLOGNAMLVL);

  /* Get table to be used indicated in the name itself.  Note that this increments the table's ref count no matter if it is the given table or not */

  sts = oz_knl_logname_parse (given_lognamtbl, procmode, given_name_l, given_name, &lognamtbl, &name_l, &name);
  if (sts != OZ_SUCCESS) return (sts);

  /* If lognamtbl is null, it means to search the directories */

  if (lognamtbl == NULL) {

    /* Maybe they are trying to access the directories themselves */

    if ((oz_s_logname_directorynames[0][name_l] == 0) && (memcmp (name, oz_s_logname_directorynames[0], name_l) == 0)) {
      logname = oz_knl_process_getlognamdir (NULL);
      goto found_it;
    }
    if ((oz_s_logname_directorynames[1][name_l] == 0) && (memcmp (name, oz_s_logname_directorynames[1], name_l) == 0)) {
      logname = oz_knl_job_getlognamdir (NULL);
      goto found_it;
    }
    if ((oz_s_logname_directorynames[2][name_l] == 0) && (memcmp (name, oz_s_logname_directorynames[2], name_l) == 0)) {
      logname = oz_knl_user_getlognamdir (NULL);
      goto found_it;
    }
    if ((oz_s_logname_directorynames[3][name_l] == 0) && (memcmp (name, oz_s_logname_directorynames[3], name_l) == 0)) {
      logname = oz_s_systemdirectory;
      goto found_it;
    }

    /* Not the directories themselves, search the directories for the name */

    sts = logname_lookup (oz_knl_process_getlognamdir (NULL), procmode, name_l, name, procmode_r, lognamatr_r, nvalues_r, values_r, logname_r, lognamtbl_r, level - 1);
    if (sts == OZ_NOLOGNAME) sts = logname_lookup (oz_knl_job_getlognamdir (NULL), procmode, name_l, name, procmode_r, lognamatr_r, nvalues_r, values_r, logname_r, lognamtbl_r, level - 1);
    if (sts == OZ_NOLOGNAME) sts = logname_lookup (oz_knl_user_getlognamdir (NULL), procmode, name_l, name, procmode_r, lognamatr_r, nvalues_r, values_r, logname_r, lognamtbl_r, level - 1);
    if (sts == OZ_NOLOGNAME) sts = logname_lookup (oz_s_systemdirectory, procmode, name_l, name, procmode_r, lognamatr_r, nvalues_r, values_r, logname_r, lognamtbl_r, level - 1);
    return (sts);
  }

  /* If lognamtbl is not a table itself, then try to get tables named of its values */

  if (!(lognamtbl -> lognamatr & OZ_LOGNAMATR_TABLE)) {

    /* Loop through each value */

    sts = OZ_NOLOGNAME;
    for (i = 0; i < lognamtbl -> nvalues; i ++) {

      /* If lognamtbl is an object logical of a logical, use the object as the table to search in */

      if (lognamtbl -> values[i].attr & OZ_LOGVALATR_OBJECT) {
        sts = oz_knl_logname_getobj (lognamtbl, i, OZ_OBJTYPE_UNKNOWN, &object);	/* get the object pointer */
        if (sts == OZ_SUCCESS) {
          logname = cvtlognamtblobj (object);						/* try to convert to a table pointer (if it's not already) */
          if (logname == NULL) sts = OZ_INVOBJTYPE;					/* - maybe it's something that doesn't have a table */
          else oz_knl_logname_increfc (logname, 1);					/* - it converted to a table, inc table's ref count */
          oz_knl_logname_incobjrefc (object, -1, NULL);					/* anyway, decrement object's ref count */
        }
      }

      /* Find table with that name in the directories */

      else sts = logname_lookup (NULL, procmode, strlen (lognamtbl -> values[i].buff), lognamtbl -> values[i].buff, NULL, NULL, NULL, NULL, &logname, NULL, level - 1);

      /* If found, use that table to lookup requested logical name */

      if (sts == OZ_SUCCESS) {
        sts = logname_lookup (logname, procmode, name_l, name, procmode_r, lognamatr_r, nvalues_r, values_r, logname_r, lognamtbl_r, level - 1);
        oz_knl_logname_increfc (logname, -1);
      }

      /* If found (or some weird error), return status */

      if (sts != OZ_NOLOGNAME) break;

      /* Not found in that table, try another */
      /* If tried all associated tables, return 'no such logical name' */
    }

    oz_knl_logname_increfc (lognamtbl, -1);
    return (sts);
  }

  /* It is a table, lock it */

  sts = lognamtbl_lock (lognamtbl);
  if (sts != OZ_SUCCESS) {
    oz_knl_logname_increfc (lognamtbl, -1);
    return (sts);
  }

  /* Search table for name.  Get the outermost name that is at or inside the given procmode. */

  outermost_procmode = OZ_PROCMODE_KNL;
  outermost_logname  = NULL;

  for (logname = lognamtbl -> lognames; logname != NULL; logname = logname -> next) {
    if ((logname -> name_l == name_l) && (memcmp (logname -> name, name, name_l) == 0)) {
      if (logname -> procmode == procmode) goto found_it;
      if ((logname -> procmode < procmode) && (logname -> procmode >= outermost_procmode)) {
        outermost_procmode = logname -> procmode;
        outermost_logname  = logname;
      }
    }
  }

  logname = outermost_logname;
  sts = OZ_NOLOGNAME;
  if (logname == NULL) goto unlock_it;

found_it:

  /* If pointer to name was requested, inc the reference count */

  if (logname_r != NULL) {
    *logname_r = logname;
    OZ_HW_ATOMIC_INCBY1_LONG (logname -> refcount);
  }

  /* If pointer to table was requested, inc the reference count */

  if (lognamtbl_r != NULL) {
    *lognamtbl_r = lognamtbl;
    oz_knl_logname_increfc (lognamtbl, 1);
  }

  /* Anyway, return any other things requested */

  if (procmode_r  != NULL) *procmode_r  = logname -> procmode;
  if (lognamatr_r != NULL) *lognamatr_r = logname -> lognamatr;
  if (nvalues_r   != NULL) *nvalues_r   = logname -> nvalues;
  if (values_r    != NULL) *values_r    = logname -> values;

  sts = OZ_SUCCESS;

unlock_it:
  if (lognamtbl != NULL) {
    lognamtbl_unlock (lognamtbl);
    oz_knl_logname_increfc (lognamtbl, -1);
  }
  return (sts);
}

/************************************************************************/
/*									*/
/*  Parse a logical name into its table and name components		*/
/*									*/
/*    Input:								*/
/*									*/
/*	lognamtbl = default table pointer				*/
/*	            (or NULL for list of directories)			*/
/*	procmode  = outermost processor mode to consider		*/
/*	name      = logical name to be parsed				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_logname_parse = OZ_SUCCESS : parse successful		*/
/*	                             else : error status		*/
/*	*lognamtbl_r = resultant table pointer (ref count incd)		*/
/*	*name_r = pointer to name portion of input string		*/
/*									*/
/*    Note:								*/
/*									*/
/*	Caller may supply the name of a Process, Job or User objtype 	*/
/*	logical name, which refers to the corresponding directory.	*/
/*									*/
/************************************************************************/

uLong oz_knl_logname_parse (OZ_Logname *lognamtbl, OZ_Procmode procmode, int name_l, const char *name, OZ_Logname **lognamtbl_r, int *name_l_r, const char **name_r)

{
  const char *p;
  uLong sts;
  OZ_Logname *logname;

  name_l = strnlen (name, name_l);
  for (p = name + name_l; (-- p) > name;) if (*p == OZ_LOGNAME_TABLECHR) break;

  if (*p == OZ_LOGNAME_TABLECHR) {
    sts = oz_knl_logname_lookup (NULL, procmode, p - name, name, NULL, NULL, NULL, NULL, &logname, NULL); /* look up table in directories */
    if (sts == OZ_SUCCESS) {
      lognamtbl = cvtlognamtblobj (logname);		/* convert to table pointer if necessary */
      if (lognamtbl == NULL) sts = OZ_INVOBJTYPE;	/* error if it won't convert */
      else oz_knl_logname_increfc (lognamtbl, 1);	/* inc table's ref count if it did convert */
      oz_knl_logname_increfc (logname, -1);		/* converted or not, done with logname */
      *lognamtbl_r = lognamtbl;				/* return pointer to table */
      *name_r = ++ p;					/* return pointer to name string (skip over the % first) */
      *name_l_r = name + name_l - p;			/* return length of name string */
    }
    return (sts);					/* return table lookup status */
  }

  /* No table name specified, use given table */

  *name_l_r = name_l;					/* return whole name string */
  *name_r   = name;

  /* Now if it's really a table, just use it as is.                                      */
  /* But it may be a process, job, or user block, and if so, get corresponding directory */

  sts = OZ_SUCCESS;					/* by default, successful */
  if (lognamtbl != NULL) {
    lognamtbl = cvtlognamtblobj (lognamtbl);		/* lognamtbl given (or something than can convert to a table pointer), convert to table pointer if necessary */
    if (lognamtbl == NULL) sts = OZ_INVOBJTYPE;		/* error if it won't convert */
    else oz_knl_logname_increfc (lognamtbl, 1);		/* inc table's ref count if it did convert */
  }
  *lognamtbl_r = lognamtbl;				/* return table pointer (or NULL if failure) */
  return (sts);						/* return status */
}

/************************************************************************/
/*									*/
/*  Convert logical name object entry to a printable string		*/
/*									*/
/*    Input:								*/
/*									*/
/*	logname = pointer to logical name				*/
/*	  index = index of value to process				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_logname_objstr = OZ_SUCCESS : successful			*/
/*	                      OZ_BUFFEROVF : output buffer overflowed	*/
/*	                      OZ_SUBSCRIPT : 'index' is too big		*/
/*	                   OZ_NOTLOGNAMOBJ : not an object logname	*/
/*	*buffer = filled in with null-terminated string			*/
/*									*/
/************************************************************************/

uLong oz_knl_logname_objstr (OZ_Logname *logname, uLong index, int buflen, char *buffer)

{
  char *objtypstr, tmpbuf[sizeof(OZ_Pointer)*2+32];
  int i;
  OZ_Pointer object;
  uLong sts;

  OZ_KNL_CHKOBJTYPE (logname, OZ_OBJTYPE_LOGNAME);

  if (index >= logname -> nvalues) return (OZ_SUBSCRIPT);

  /* Make sure it is an object type value */

  if (!(logname -> values[index].attr & OZ_LOGVALATR_OBJECT)) return (OZ_NOTLOGNAMOBJ);

  /* Convert to a string in the form <hex_address>:<object_type> */

  sts = oz_knl_logname_incobjrefc (logname -> values[index].buff, 0, &objtypstr);
  if (sts != OZ_SUCCESS) oz_crash ("oz_knl_logname objstr: bad object in %s index %u", logname -> name, index);
  i = sizeof tmpbuf - strlen (objtypstr) - 1;
  strcpy (tmpbuf + i, objtypstr);
  tmpbuf[--i] = ':';
  object = (OZ_Pointer)(logname -> values[index].buff);
  do {
    tmpbuf[--i] = "0123456789ABCDEF"[object&15];
    object >>= 4;
  } while (object != 0);
  strncpyz (buffer, tmpbuf + i, buflen);

  if (sizeof tmpbuf - i >= buflen) sts = OZ_BUFFEROVF;

  return (sts);
}

/************************************************************************/
/*									*/
/*  Get the object associated with a logical name			*/
/*									*/
/*    Input:								*/
/*									*/
/*	logname = pointer to logical name with the OBJECT attribute	*/
/*	index   = value index						*/
/*	objtype = the object's type					*/
/*	          OZ_OBJTYPE_UNKNOWN, caller must determine type	*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_logname_getobj = OZ_SUCCESS : successful			*/
/*	                              else : error status		*/
/*	*object_r = object pointer					*/
/*	            its ref count has been incremented			*/
/*									*/
/************************************************************************/

uLong oz_knl_logname_getobj (OZ_Logname *logname, uLong index, OZ_Objtype objtype, void **object_r)

{
  uLong sts;
  void *object;

  OZ_KNL_CHKOBJTYPE (logname, OZ_OBJTYPE_LOGNAME);

  /* Make sure value index is in range */

  if (index >= logname -> nvalues) return (OZ_SUBSCRIPT);

  /* Make sure it is an object type value */

  if (!(logname -> values[index].attr & OZ_LOGVALATR_OBJECT)) return (OZ_NOTLOGNAMOBJ);

  /* Ok, get the object pointer and increment reference count */

  object = logname -> values[index].buff;				/* get pointer to object */
  if ((objtype != OZ_OBJTYPE_UNKNOWN) && (OZ_KNL_GETOBJTYPE (object) != objtype)) { /* make sure it is the type they expect */
    oz_knl_printk ("oz_knl_logname_getobj: logname %s index %u object %p is type %d, not %d\n", logname -> name, index, object, OZ_KNL_GETOBJTYPE (object), objtype);
    return (OZ_BADLOGNOBJTYPE);
  }
  sts = oz_knl_logname_incobjrefc (object, 1, NULL);			/* ok, inc its reference count */
  if (sts != OZ_SUCCESS) oz_crash ("oz_knl_logname_getobj: error %u incing ref count of object %s", sts, logname -> name);
  *object_r = object;							/* return pointer to object */
  return (OZ_SUCCESS);							/* return final status */
}

/************************************************************************/
/*									*/
/*  Get the value array associated with a logical name			*/
/*									*/
/*    Input:								*/
/*									*/
/*	logname = pointer to logical name				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_logname_getval = OZ_SUCCESS : successful			*/
/*	                              else : error status		*/
/*	*lognamatr_r = logical name attributes				*/
/*	*nvalues_r   = number of elements in array			*/
/*	*values_r    = pointer to value array				*/
/*	*lognamtbl_r = logical name table pointer			*/
/*									*/
/*    Note:								*/
/*									*/
/*	if lognamtbl_r is not NULL, the table's ref count is 		*/
/*	incremented							*/
/*									*/
/************************************************************************/

uLong oz_knl_logname_getval (OZ_Logname *logname, OZ_Procmode *procmode_r, uLong *lognamatr_r, uLong *nvalues_r, 
                             const OZ_Logvalue *values_r[], OZ_Logname **lognamtbl_r)

{
  OZ_Logname *lognamtbl;

  OZ_KNL_CHKOBJTYPE (logname, OZ_OBJTYPE_LOGNAME);

  if (lognamtbl_r != NULL) {
    lognamtbl = logname -> lognamtbl;
    if (lognamtbl != NULL) oz_knl_logname_increfc (lognamtbl, 1);
    *lognamtbl_r = lognamtbl;
  }

  if (procmode_r  != NULL) *procmode_r  = logname -> procmode;
  if (lognamatr_r != NULL) *lognamatr_r = logname -> lognamatr;
  if (nvalues_r   != NULL) *nvalues_r   = logname -> nvalues;
  if (values_r    != NULL) *values_r    = logname -> values;

  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Get first/next logical name in a table				*/
/*									*/
/*    Input:								*/
/*									*/
/*	lognamtbl = logical name table pointer				*/
/*	*logname_r = previous logical name or NULL if first one		*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_logname_gettblent = OZ_SUCCESS : successful		*/
/*	                                 else : error status		*/
/*	*logname_r = new logical name or NULL if end of table		*/
/*									*/
/*    Note:								*/
/*									*/
/*	*logname_r's ref count is decremented on input and incremented 	*/
/*	on output							*/
/*									*/
/************************************************************************/

uLong oz_knl_logname_gettblent (OZ_Logname *lognamtbl, OZ_Logname **logname_r)

{
  uLong sts;
  OZ_Logname *newlogname, *oldlogname;

  oldlogname = *logname_r;					/* get input logical name pointer */
  if (oldlogname != NULL) {
    OZ_KNL_CHKOBJTYPE (oldlogname, OZ_OBJTYPE_LOGNAME);
    lognamtbl = oldlogname -> lognamtbl;			/* ... if present, get its table */
    if (lognamtbl == NULL) return (OZ_LOGNAMNOTINTBL);
  } else {
    lognamtbl = cvtlognamtblobj (lognamtbl);			/* try to get a real table */
    if (lognamtbl == NULL) return (OZ_INVOBJTYPE);
  }

  sts = lognamtbl_lock (lognamtbl);				/* keep others out of table while we link to next in table */
  if (sts != OZ_SUCCESS) return (sts);

  if (oldlogname != NULL) newlogname = oldlogname -> next;	/* get pointer to next logical name in the table */
  else newlogname = lognamtbl -> lognames;			/* get first name in table */

  if (newlogname != NULL) OZ_HW_ATOMIC_INCBY1_LONG (newlogname -> refcount); /* increment ref count on returned logical name so it can't be deleted */

  lognamtbl_unlock (lognamtbl);					/* now that ref count is incremented, unlock the table */

  if (oldlogname != NULL) oz_knl_logname_increfc (oldlogname, -1); /* decrement ref count on input logical name */

  *logname_r = newlogname;

  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Get logical's name							*/
/*									*/
/*    Input:								*/
/*									*/
/*	logname = logical name pointer					*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_logname_getname = pointer to null terminated name string	*/
/*									*/
/************************************************************************/

const char *oz_knl_logname_getname (OZ_Logname *logname)

{
  OZ_KNL_CHKOBJTYPE (logname, OZ_OBJTYPE_LOGNAME);
  return (logname -> name);
}

/************************************************************************/
/*									*/
/*  Get logical's security attributes					*/
/*									*/
/*    Input:								*/
/*									*/
/*	logname = logical name pointer					*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_logname_getsecattr = pointer to security attributes	*/
/*									*/
/************************************************************************/

OZ_Secattr *oz_knl_logname_getsecattr (OZ_Logname *logname)

{
  OZ_KNL_CHKOBJTYPE (logname, OZ_OBJTYPE_LOGNAME);
  oz_knl_secattr_increfc (logname -> secattr, 1);
  return (logname -> secattr);
}

/************************************************************************/
/*									*/
/*  Increment logical name reference count				*/
/*									*/
/*    Input:								*/
/*									*/
/*	logname = pointer to logical name to change ref count of	*/
/*	inc = value to increment by					*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_logname_increfc = new ref count				*/
/*	if neg, system crashes						*/
/*									*/
/*    Note:								*/
/*									*/
/*	Logical name is not automatically deleted when ref count goes zero
/*									*/
/************************************************************************/

Long oz_knl_logname_increfc (OZ_Logname *logname, Long inc)

{
  Long refc;
  uLong i, sts;
  OZ_Logname *xlogname;
  void *object;

  OZ_KNL_CHKOBJTYPE (logname, OZ_OBJTYPE_LOGNAME);

  /* Update reference count under hardware smp lock and get the result */

again:
  do {
    refc = logname -> refcount;
    if (refc < 0) oz_crash ("oz_knl_logname_increfc: logical name %s ref count was %d", logname -> name, refc);
    if (refc + inc <= 0) goto going_le_zero;
  } while (!oz_hw_atomic_setif_long (&(logname -> refcount), refc + inc, refc));
  return (refc + inc);

  /* Result can not be negative */

going_le_zero:
  if (refc + inc < 0) oz_crash ("oz_knl_logname_increfc: logical name %s ref count %d+%d", logname -> name, refc, inc);

  /* If the logical is not in a table, it is to be deleted */

  if (logname -> lognamtbl == NULL) {

    /* If deleting a table, delete all entries in it first */

    if (logname -> lognamatr & OZ_LOGNAMATR_TABLE) {
      while (1) {
        sts = lognamtbl_lock (logname);					/* lock myself */
        if (sts != OZ_SUCCESS) oz_crash ("oz_knl_logname_increfc: error %u locking table %s", logname -> name);
        xlogname = logname -> lognames;					/* get a logical name in me */
        if (xlogname == NULL) break;					/* done if none left */
        OZ_HW_ATOMIC_INCBY1_LONG (xlogname -> refcount);		/* ok, increment its ref count */
        logname -> lognames = xlogname -> next;				/* remove from table */
        xlogname -> lognamtbl = NULL;
        lognamtbl_unlock (logname);					/* unlock the table - we can't do recursive locking */
        oz_knl_logname_increfc (xlogname, -1);				/* recursive call to decrement ref count */
      }
      lognamtbl_unlock (logname);					/* i am empty, unlock myself */
    }

    /* Free off any locking event flag */

    if (logname -> tablevent != NULL) {
      oz_knl_event_increfc (logname -> tablevent, -1);
      logname -> tablevent = NULL;
    }

    /* Free off any security attributes */

    if (logname -> secattr != NULL) {
      oz_knl_secattr_increfc (logname -> secattr, -1);
      logname -> secattr = NULL;
    }

    /* Unlock any referenced objects */

    for (i = 0; i < logname -> nvalues; i ++) {
      if (logname -> values[i].attr & OZ_LOGVALATR_OBJECT) {
        object = logname -> values[i].buff;
        sts = oz_knl_logname_incobjrefc (object, -1, NULL);
        if (sts != OZ_SUCCESS) oz_crash ("oz_knl_logname_increfc: error %u decrement object ref count logname %s object %s", sts, logname -> name, logname -> values[i].buff);
      }
    }

    /* Free off memory block */

    if (logname -> refcount != refc) {
      oz_crash ("oz_knl_logname_increfc: %p refcount changed from %d to %d", logname, refc, logname -> refcount);
    }
    OZ_KNL_PGPFREE (logname);
  }

  /* It's in a table, so just set its refcount to zero and leave it in the table */

  else if (!oz_hw_atomic_setif_long (&(logname -> refcount), 0, refc)) goto again;

  return (0);
}

/************************************************************************/
/*									*/
/*  Delete logical name							*/
/*									*/
/*    Input:								*/
/*									*/
/*	logname = logical name entry to delete				*/
/*									*/
/*    Output:								*/
/*									*/
/*	reference count decremented					*/
/*	entry removed from table and marked for delete			*/
/*									*/
/************************************************************************/

void oz_knl_logname_delete (OZ_Logname *logname)

{
  uLong sts;
  OZ_Logname *lognamtbl;

  /* If logical is in a table, remove it */

  lognamtbl = logname -> lognamtbl;
  if (lognamtbl != NULL) {
    sts = lognamtbl_lock (lognamtbl);		/* lock table */
    if (sts != OZ_SUCCESS) oz_crash ("oz_knl_logname_delete: error %u locking table for %s", logname -> name);
    markdelete (logname);			/* remove entry from table */
    lognamtbl_unlock (lognamtbl);		/* unlock table */
  }

  /* Decrement ref count and free it off */

  oz_knl_logname_increfc (logname, -1);
}

/************************************************************************/
/*									*/
/*  Increment object reference count.  Only include objects that can 	*/
/*  be incd and decd at will.						*/
/*									*/
/*    Input:								*/
/*									*/
/*	object = object pointer						*/
/*	inc = value of increment					*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_logname_incobjrefc = OZ_SUCCESS : successful		*/
/*	                                  else : error status		*/
/*	*objtypstr = object type string					*/
/*									*/
/************************************************************************/

uLong oz_knl_logname_incobjrefc (void *object, Long inc, char **objtypstr)

{
  char *ots;

  switch (OZ_KNL_GETOBJTYPE (object)) {
    case OZ_OBJTYPE_DEVUNIT: { if (inc != 0) oz_knl_devunit_increfc (object, inc); ots = "devunit"; break; }
    case OZ_OBJTYPE_EVENT:   { if (inc != 0) oz_knl_event_increfc   (object, inc); ots = "event";   break; }
    case OZ_OBJTYPE_IOCHAN:  { if (inc != 0) oz_knl_iochan_increfc  (object, inc); ots = "iochan";  break; }
    case OZ_OBJTYPE_JOB:     { if (inc != 0) oz_knl_job_increfc     (object, inc); ots = "job";     break; }
    case OZ_OBJTYPE_LOGNAME: { if (inc != 0) oz_knl_logname_increfc (object, inc); ots = "logname"; break; }
    case OZ_OBJTYPE_PROCESS: { if (inc != 0) oz_knl_process_increfc (object, inc); ots = "process"; break; }
    case OZ_OBJTYPE_SECTION: { if (inc != 0) oz_knl_section_increfc (object, inc); ots = "section"; break; }
    case OZ_OBJTYPE_THREAD:  { if (inc != 0) oz_knl_thread_increfc  (object, inc); ots = "thread";  break; }
    case OZ_OBJTYPE_USER:    { if (inc != 0) oz_knl_user_increfc    (object, inc); ots = "user";    break; }
    default: { oz_knl_printk ("oz_knl_logname_incobjrefc: object %p is type %d\n", object, OZ_KNL_GETOBJTYPE (object)); return (OZ_BADLOGNOBJTYPE); }
  }

  if (objtypstr != NULL) *objtypstr = ots;
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Internal Routines							*/
/*									*/
/************************************************************************/

/************************************************************************/
/*									*/
/*  This routine converts an alleged table object pointer to a genuine 	*/
/*  table object pointer						*/
/*									*/
/*    Input:								*/
/*									*/
/*	lognamtbl = object pointer					*/
/*									*/
/*    Output:								*/
/*									*/
/*	cvtlognamtblobj = NULL : unrecognised object type		*/
/*	                  else : pointer to corresponding table		*/
/*									*/
/*    Note:								*/
/*									*/
/*	A Thread object converts to the process' directory		*/
/*	A Process object converts to the process' directory		*/
/*	A Job object converts to the job's directory			*/
/*	An User object converts to the user's directory			*/
/*									*/
/************************************************************************/

static OZ_Logname *cvtlognamtblobj (OZ_Logname *lognamtbl)

{
  switch (OZ_KNL_GETOBJTYPE (lognamtbl)) {
    case OZ_OBJTYPE_LOGNAME: { break; }
    case OZ_OBJTYPE_THREAD:  { lognamtbl = oz_knl_thread_getprocess    (lognamtbl); }
    case OZ_OBJTYPE_PROCESS: { lognamtbl = oz_knl_process_getlognamdir (lognamtbl); break; }
    case OZ_OBJTYPE_JOB:     { lognamtbl = oz_knl_job_getlognamdir     (lognamtbl); break; }
    case OZ_OBJTYPE_USER:    { lognamtbl = oz_knl_user_getlognamdir    (lognamtbl); break; }
    default: { lognamtbl = NULL; break; }
  }
  OZ_KNL_CHKOBJTYPE (lognamtbl, OZ_OBJTYPE_LOGNAME);
  return (lognamtbl);
}

/************************************************************************/
/*									*/
/*  Lock logical name table						*/
/*									*/
/*  No recursive table locks are allowed, a thread may only have one 	*/
/*  logical name table locked at a time to avoid deadlocks		*/
/*									*/
/************************************************************************/

static uLong lognamtbl_lock (OZ_Logname *lognamtbl)

{
  if (lognamtbl -> tablevent == NULL) return (OZ_NOTLOGNAMTBL);
  while (oz_knl_event_set (lognamtbl -> tablevent, 0) == 0) {
    oz_knl_event_waitone (lognamtbl -> tablevent);
  }
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Unlock logical name table						*/
/*									*/
/************************************************************************/

static void lognamtbl_unlock (OZ_Logname *lognamtbl)

{
  oz_knl_event_set (lognamtbl -> tablevent, 1);
}

/************************************************************************/
/*									*/
/*  Mark a logical name for delete by removing it from its table.   	*/
/*  This routine must be called with the table locked.			*/
/*									*/
/************************************************************************/

static void markdelete (OZ_Logname *logname)

{
  OZ_Logname **llogname, *xlogname;

  for (llogname = &(logname -> lognamtbl -> lognames); (xlogname = *llogname) != logname; llogname = &(xlogname -> next)) {
    if (xlogname == NULL) oz_crash ("oz_knl_logname markdel: can't find logical name %s in table", logname -> name);
  }
  *llogname = xlogname -> next;
  xlogname -> lognamtbl = NULL;
}

/************************************************************************/
/*									*/
/*  Dump out logical name to console					*/
/*									*/
/************************************************************************/

void oz_knl_logname_dump (int level, OZ_Logname *logname)

{
  char tmpbuf[sizeof(OZ_Pointer)*2+32];
  int i;
  uLong sts;
  OZ_Logname *lnm;

  for (i = 0; i < level; i ++) oz_knl_printk ("  ");
  oz_knl_printk ("  %s", logname -> name);
  if (logname -> lognamatr & OZ_LOGNAMATR_NOSUPERSEDE) oz_knl_printk (" (nosupersede)");
  if (logname -> lognamatr & OZ_LOGNAMATR_NOOUTERMODE) oz_knl_printk (" (nooutermode)");
  if (logname -> procmode == OZ_PROCMODE_KNL) oz_knl_printk (" (kernel)");
  else if (logname -> procmode == OZ_PROCMODE_USR) oz_knl_printk (" (user)");
  else oz_knl_printk (" (%d)", logname -> procmode);
  oz_knl_printk (" (ref:%d)", logname -> refcount);

  if (logname -> lognamatr & OZ_LOGNAMATR_TABLE) {
    oz_knl_printk (" (table)\n");
    sts = lognamtbl_lock (logname);
    if (sts != OZ_SUCCESS) {
      oz_knl_printk ("error %u locking table %s\n", logname -> name);
    } else {
      for (lnm = logname -> lognames; lnm != NULL; lnm = lnm -> next) {
        oz_knl_logname_dump (level + 1, lnm);
      }
      lognamtbl_unlock (logname);
    }
  } else {
    oz_knl_printk (" =");
    for (i = 0; i < logname -> nvalues; i ++) {
      if (logname -> values[i].attr & OZ_LOGVALATR_OBJECT) {
        sts = oz_knl_logname_objstr (logname, i, sizeof tmpbuf, tmpbuf);
        if (sts != OZ_SUCCESS) oz_sys_sprintf (sizeof tmpbuf, tmpbuf, "err %u", sts);
        oz_knl_printk (" '%s'", tmpbuf);
      } else {
        oz_knl_printk (" '%s'", logname -> values[i].buff);
      }
      if (logname -> values[i].attr & OZ_LOGVALATR_OBJECT) oz_knl_printk (" (object)");
      if (logname -> values[i].attr & OZ_LOGVALATR_TERMINAL) oz_knl_printk (" (terminal)");
    }
    oz_knl_printk ("\n");
  }
}
