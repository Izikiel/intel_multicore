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
/*  Manage (process, thread) id numbers					*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_knl_hw.h"
#include "oz_knl_idno.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_sdata.h"

#define INITIAL 256 // use something small like 4 to test extension stuff

typedef struct Idno Idno;

struct Idno { void *object;	// object pointer, NULL when free
              uLong this;	// this id number + reuse sequence
              uLong next;	// index of next free id number (0 if none)
            };

static uLong idnomax    = 0;	// number of elements in idnolist array
static uLong idnofree_h = 0;	// first free index in idnolist array
static uLong idnofree_t = 0;	// last free index in idnolist array
static Idno *idnolist   = NULL;	// list of id numbers

static void expand_list (void);

/************************************************************************/
/*									*/
/*  Boot-time init routine						*/
/*									*/
/************************************************************************/

void oz_knl_idno_init (void)

{
  uLong idno;

  idnomax  = INITIAL;							// this is how many elements are in the array
  idnolist = OZ_KNL_NPPMALLOC (INITIAL * sizeof *idnolist);		// allocate initial array
  oz_knl_printk ("oz_knl_idno_init: max %u at %p\n", INITIAL, idnolist);
  memset (idnolist, 0, INITIAL * sizeof *idnolist);			// clear it

  for (idno = 0; ++ idno < INITIAL;) {					// gather up list of free id numbers
    idnolist[idno].this = idno;
    idnolist[idno].next = idno + 1;
  }
  idnofree_h = 1;							// use entry 1 as the first free
  idnofree_t = INITIAL - 1;						// use last entry as the last free
  idnolist[INITIAL-1].next = 0;
}

/************************************************************************/
/*									*/
/*  Allocate id number and assign object to it				*/
/*									*/
/*    Input:								*/
/*									*/
/*	object = pointer to object to assign id number to		*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_idno_alloc = id number					*/
/*									*/
/*    Note:								*/
/*									*/
/*	This routine assumes the system will run out of NPP long 	*/
/*	before all 4,000,000,000+ id numbers are in use			*/
/*									*/
/************************************************************************/

uLong oz_knl_idno_alloc (void *object)

{
  uLong id, idno;

  id = oz_hw_smplock_wait (&oz_s_smplock_id);				// lock array in place
  while ((idno = idnofree_h) == 0) expand_list ();			// get free element index
  if (idnolist[idno].object != NULL) oz_crash ("oz_knl_idno_alloc: giving %u away when already in use", idno);
  idnofree_h = idnolist[idno].next;					// unlink number from free list
  if (idnofree_h == 0) idnofree_t = 0;					// maybe it was the last one
  idnolist[idno].object = object;					// save object pointer
  idnolist[idno].next   = idno;						// link it back to itself when allocated
  idno = idnolist[idno].this;						// get actual number with re-use count
  oz_hw_smplock_clr (&oz_s_smplock_id, id);				// unlock array, it's free to move about the cabin
  return (idno);							// return id number
}

static void expand_list (void)

{
  Idno *newidnolist;
  uLong idno, newmax;

  newmax = idnomax * 2;
  newidnolist = OZ_KNL_NPPMALLOC (newmax * sizeof *newidnolist);	// allocate a new array
  oz_knl_printk ("oz_knl_idno_alloc: new max %u at %p\n", newmax, newidnolist);
  memset (newidnolist, 0, newmax * sizeof *newidnolist);		// clear it

  newidnolist[idnomax].this = idnomax;					// free off upper one that matches lower entry 0
  idnofree_h = idnomax;
  idnofree_t = idnomax;

  for (idno = 0; ++ idno < idnomax;) {					// transfer old entries to new array
    if (idnolist[idno].this & idnomax) {				// see which half of new array it goes into
      newidnolist[idno+idnomax] = idnolist[idno];			// upper half, transfer the entry
      newidnolist[idnofree_t].next = idno;				// free off corresponding lower half entry
      newidnolist[idno].this = idnolist[idno].this + idnomax;
      idnofree_t = idno;
    } else {
      newidnolist[idno] = idnolist[idno];				// lower half, transfer the entry
      newidnolist[idnofree_t].next = idno + idnomax;			// free off corresponding upper half entry
      newidnolist[idno+idnomax].this = idnolist[idno].this + idnomax;
      idnofree_t = idno + idnomax;
    }
  }
  OZ_KNL_NPPFREE (idnolist);						// free off old array

  idnomax  = newmax;							// save new array parameters
  idnolist = newidnolist;
}

/************************************************************************/
/*									*/
/*  Find the object a particular id number is assigned to		*/
/*									*/
/*    Input:								*/
/*									*/
/*	idno = id number						*/
/*	objtype = OZ_OBJTYPE_UNKNOWN : don't check object's type	*/
/*	                        else : object must be this type		*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_idno_find = NULL : idno not assigned			*/
/*	                   else : pointer to object			*/
/*									*/
/************************************************************************/

void *oz_knl_idno_find (uLong idno, OZ_Objtype objtype)

{
  uLong ididx;
  uLong id;
  void *object;

  object = NULL;							// assume we will fail
  id = oz_hw_smplock_wait (&oz_s_smplock_id);				// lock array in place
  ididx = idno & idnomax - 1;						// get index w/out re-use count
  if (idnolist[ididx].this == idno) object = idnolist[ididx].object;	// if re-use count matches, get object pointer
  oz_hw_smplock_clr (&oz_s_smplock_id, id);				// release array
  if ((object != NULL) && (objtype != OZ_OBJTYPE_UNKNOWN) && (OZ_KNL_GETOBJTYPE (object) != objtype)) object = NULL;
  return (object);							// return object pointer
}

/************************************************************************/
/*									*/
/*  Free off an id number slot for re-use				*/
/*									*/
/*    Input:								*/
/*									*/
/*	idno = id number being released					*/
/*									*/
/*    Output:								*/
/*									*/
/*	idno no longer valid						*/
/*									*/
/************************************************************************/

void oz_knl_idno_free (uLong idno)

{
  uLong id, idnoidx;

  id = oz_hw_smplock_wait (&oz_s_smplock_id);				// lock array in place
  idnoidx = idno & (idnomax - 1);
  if (idnolist[idnoidx].object == NULL) oz_crash ("oz_knl_idno_free: freeing %u when not in use", idno);
  if (idnolist[idnoidx].this   != idno) oz_crash ("oz_knl_idno_free: freeing %u when [%u] is %u", idno, idnoidx, idnolist[idnoidx].this);
  idnolist[idnoidx].object = NULL;					// mark it free
  idnolist[idnoidx].this  += idnomax;					// increment re-use count for next alloc
  idnolist[idnoidx].next   = 0;						// it will be on end of list
  if (idnofree_t == 0) idnofree_h = idnoidx;				// see if it is the only one on list
  else idnolist[idnofree_t].next  = idnoidx;				// if not, link it on the end of list
  idnofree_t = idnoidx;							// anyway, it is on the end now
  oz_hw_smplock_clr (&oz_s_smplock_id, id);				// release array
}
