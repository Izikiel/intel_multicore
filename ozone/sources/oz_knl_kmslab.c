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
/*  General kernel heap memory allocation routines			*/
/*									*/
/************************************************************************/

#define _OZ_KNL_KMALLOC_C

#include "ozone.h"

#include "oz_knl_hw.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_malloc.h"
#include "oz_knl_printk.h"
#include "oz_knl_sdata.h"

typedef struct Block Block;
typedef struct Chunk Chunk;

struct Block { Block *next;		// next block in chunk -> freeblocks list
               Chunk *chunk;		// pointer to corresponding Chunk struct
             };				// followed by user data object

struct Chunk { Chunk *next;		// next chunk in memslab -> emptychunks,fullchunks or partialchunks list
               Chunk **prev;		// prev chunk in memslab -> emptychunks,fullchunks or partialchunks list
               OZ_Memslab *memslab;	// pointer to corresponding OZ_Memslab struct
               Block *freeblocks;	// list of free blocks in chunk, or NULL if chunk is full
               Long allocblocks;	// number of allocated blocks in chun, or 0 if chunk is empty
             };

struct OZ_Memslab { OZ_Objtype objtype;		// OZ_OBJTYPE_MEMSLAB
                    OZ_Procmode pprocmode;	// procmode for oz_sys_pdata_malloc routine
                    uLong objsize;
                    void (*constructor) (void *object, uLong objsize);
                    void (*destructor) (void *object, uLong objsize);
                    uLong (*lock) (void *lockprm);
                    uLong (*unlk) (void *lockprm, uLong prev);
                    void *lockprm;
                    Chunk *emptychunks;
                    Chunk *fullchunks;
                    Chunk *partialchunks;
                  };

/************************************************************************/
/*									*/
/*  Initialize memory slab						*/
/*									*/
/************************************************************************/

OZ_Memslab oz_sys_memslab_init (OZ_Procmode pprocmode, uLong objsize, void (*constructor) (void *object, uLong objsize), void (*destructor) (void *object, uLong objsize), uLong (*lock) (void *lockprm), uLong (*unlk) (void *lockprm, uLong prev), void *lockprm)

{
  OZ_Memslab *memslab;

  memslab = oz_sys_pdata_malloc (pprocmode, sizeof *memslab);
  if (memslab != NULL) {
    memslab -> objtype     = OZ_OBJTYPE_MEMSLAB;
    memslab -> pprocmode   = pprocmode;
    memslab -> objsize     = objsize;
    memslab -> constructor = constructor;
    memslab -> destructor  = destructor;
    memslab -> lock        = lock;
    memslab -> unlk        = unlk;
    memslab -> lockprm     = lockprm;
    memslab -> emptychunks  = NULL;
    memslab -> fullchunks   = NULL;
    memslab -> partialchunks   = NULL;
  }
  return (memslab);
}

void *oz_sys_memslab_alloc (OZ_Memslab *memslab)

{
  uLong prev;

  prev = (*(memslab -> lock)) (memslab -> lockprm);
  if ((slab = memslab -> partialchunks) != NULL) {
    block = slab -> freeblocks;
    if (block == NULL) oz_crash ("oz_sys_memslab_alloc: partial slab %p had no free blocks", slab);
    slab -> freeblocks = *(void **)block;
    if (slab -> freeblocks == NULL) {
      memslab -> partialchunks = slab -> next;
      slab -> next = memslab -> fullchunks;
      memslab -> fullchunks = slab;
    }
    slab -> numinuse ++;
  } else if ((slab = memslab -> emptychunks) != NULL) {
    block = slab -> freeblocks;
    if (block == NULL) oz_crash ("oz_sys_memslab_alloc: empty slab %p had no free blocks", slab);
    slab -> freeblocks = block -> next;
    memslab -> emptychunks = slab -> next;
    slab -> next = NULL;
    memslab -> partialchunks = slab;
    slab -> numinuse ++;
  } else {
    slab = oz_sys_pdata_malloc (memslab -> pprocmode, ????);
    block = NULL;
    if (slab != NULL) {
      for (i = 0; i < n; i ++) {
        ?????
      }
      slab -> numinuse = 1;
    }
  }
  (*(memslab -> unlk)) (memslab -> lockprm, prev);
  return (block);
}

void oz_sys_memslab_free (void *block)

{
  uLong prev;

  slab = block -> slab;
  memslab = slab -> memslab;
  prev = (*(memslab -> lock)) (memslab -> lockprm);
  block -> next = slab -> freeblocks;
  slab -> freeblocks = block;
  slab -> numinuse --;
  if (block -> next == NULL) {
    if ((*(slab -> prev) = slab -> next) != NULL) slab -> next -> prev = slab -> prev;
    if (slab -> numinuse > 0) {
      slab -> next = memslab -> partialchunks;
      memslab -> partialchunks = slab -> next;
    } else {
      slab -> next = memslab -> emptychunks;
      memslab -> emptychunks = slab -> next;
    }
  } else if (slab -> numinuse == 0) {
    if ((*(slab -> prev) = slab -> next) != NULL) slab -> next -> prev = slab -> prev;
    slab -> next = memslab -> emptychunks;
    memslab -> emptychunks = slab -> next;
  }
  (*(memslab -> unlk)) (memslab -> lockprm, prev);
}

/************************************************************************/
/*									*/
/*  Allocate nonpaged memory						*/
/*									*/
/*    Input:								*/
/*									*/
/*	size = number of bytes wanted					*/
/*	quotaed = 0 : no quota checking, crash if can't allocate	*/
/*	          1 : quota checking, NULL if no quota or no memory	*/
/*									*/
/*	softint <= smplock <= np					*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_nppmalloq = pointer to memory block			*/
/*									*/
/************************************************************************/

void *oz_knl_nppmalloq (OZ_Memsize size, int quotaed)

{
  Long npppeak;
  Npphdr *npphdr;
  OZ_Memsize rsize;
  OZ_Quota *quota;
  uLong np;

  /* Make sure there is enough quota */

  quota = NULL;
  if (quotaed) {
    quota = OZ_KNL_QUOTA_DEFAULT;
    if ((quota != NULL) && !oz_knl_quota_debit (quota, OZ_QUOTATYPE_NPP, size + sizeof *npphdr)) return (NULL);
  }

  /* Sufficient quota, try to allocate memory block */

  npphdr = oz_malloc (nonpaged, size + sizeof *npphdr, &rsize, 0);

  /* If allocated, fill the header in */

  if (npphdr != NULL) {
    if ((quota == NULL) || oz_knl_quota_debit (quota, OZ_QUOTATYPE_NPP, rsize - size - sizeof *npphdr)) {
      oz_hw_atomic_inc_long (&oz_s_nppinuse, rsize);
      do npppeak = oz_s_npppeak;
      while ((oz_s_nppinuse > npppeak) && !oz_hw_atomic_setif_long (&oz_s_npppeak, oz_s_nppinuse, npppeak));
      npphdr -> quota = quota;
      npphdr -> rsize = rsize;
#if ENABLEDUMP
      npphdr -> rtnadr = oz_hw_getrtnadr(1);
      np = oz_hw_smplock_wait (&oz_s_smplock_np);
      npphdr -> next = allocated;
      npphdr -> prev = &allocated;
      if (npphdr -> next != NULL) npphdr -> next -> prev = &(npphdr -> next);
      allocated = npphdr;
      oz_hw_smplock_clr (&oz_s_smplock_np, np);
#endif
      return (npphdr + 1);
    }
    oz_free (nonpaged, npphdr);
  }

  /* Failed, crash if not quotaed (caller isn't prepared to handle it) */

  if (!quotaed) {
    oz_knl_printk ("oz_knl_nppmalloc: out of non-paged memory, tried to get %u bytes\n", size);
    oz_knl_npp_dump ();
    oz_crash ("oz_knl_nppmalloc: ");
  }

  /* Return NULL if caller can handle it (although system probably won't last long) */

  if (quota != NULL) oz_knl_quota_credit (quota, OZ_QUOTATYPE_NPP, size + sizeof *npphdr, -1);
  return (NULL);
}

/************************************************************************/
/*									*/
/*  Allocate physically contiguous nonpaged memory			*/
/*									*/
/*    Input:								*/
/*									*/
/*	size = number of bytes wanted					*/
/*									*/
/*	softint <= smplock <= np					*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_pcmalloc_fl = pointer to memory block			*/
/*									*/
/*    Note:								*/
/*									*/
/*	Caller can assume this routine will never return with a NULL 	*/
/*	pointer								*/
/*									*/
/************************************************************************/

void *oz_knl_pcmalloc (OZ_Memsize size)

{
  Long npppeak;
  Npphdr *npphdr;
  OZ_Memsize rsize;
  uLong np;

  npphdr = oz_malloc (nonpaged, size + sizeof *npphdr, &rsize, 1);
  if (npphdr == NULL) oz_crash ("oz_knl_pcmalloc: out of physically contiguous non-paged memory, tried to get %u bytes", size);

  oz_hw_atomic_inc_long (&oz_s_nppinuse, rsize);
  do npppeak = oz_s_npppeak;
  while ((oz_s_nppinuse > npppeak) && !oz_hw_atomic_setif_long (&oz_s_npppeak, oz_s_nppinuse, npppeak));

  npphdr -> quota = NULL;
  npphdr -> rsize = rsize;
#if ENABLEDUMP
  npphdr -> rtnadr = oz_hw_getrtnadr(1);
  np = oz_hw_smplock_wait (&oz_s_smplock_np);
  npphdr -> next = allocated;
  npphdr -> prev = &allocated;
  if (npphdr -> next != NULL) npphdr -> next -> prev = &(npphdr -> next);
  allocated = npphdr;
  oz_hw_smplock_clr (&oz_s_smplock_np, np);
#endif
  return (npphdr + 1);
}

/************************************************************************/
/*									*/
/*  Validate an area of memory previously allocated by 			*/
/*  oz_knl_nppmalloq or oz_knl_pcmalloc					*/
/*									*/
/*    Input:								*/
/*									*/
/*	adrs = address of memory to be validated as returned by 	*/
/*	       oz_knl_nppmalloq or oz_knl_pcmalloc			*/
/*									*/
/*	softint <= smplock <= np					*/
/*									*/
/************************************************************************/

void oz_knl_nppvalid (void *adrs)

{
  Npphdr *npphdr;

  npphdr = adrs;
  oz_valid (nonpaged, -- npphdr);
}

void oz_knl_nppmlvalid (void)

{
  oz_mlvalid (nonpaged);
}

/************************************************************************/
/*									*/
/*  Free an area of memory previously allocated by oz_knl_nppmalloq or 	*/
/*  oz_knl_pcmalloc							*/
/*									*/
/*    Input:								*/
/*									*/
/*	adrs = address of memory to be freed as returned by 		*/
/*	       oz_knl_nppallocd or oz_knl_pcmalloc			*/
/*									*/
/*	softint <= smplock <= np					*/
/*									*/
/*    Output:								*/
/*									*/
/*	nonpaged = updated to include the given memory			*/
/*									*/
/************************************************************************/

void oz_knl_nppfree (void *adrs)

{
  Npphdr *npphdr;
  OZ_Memsize rsize;
  OZ_Quota *quota;
  uLong np;

  npphdr = adrs;
  quota  = (-- npphdr) -> quota;
  rsize  = npphdr -> rsize;
#if ENABLEDUMP
  np = oz_hw_smplock_wait (&oz_s_smplock_np);
  *(npphdr -> prev) = npphdr -> next;
  if (npphdr -> next != NULL) npphdr -> next -> prev = npphdr -> prev;
  oz_hw_smplock_clr (&oz_s_smplock_np, np);
#endif
  oz_free (nonpaged, npphdr);
  if (quota != NULL) oz_knl_quota_credit (quota, OZ_QUOTATYPE_NPP, rsize, -2);
  oz_hw_atomic_inc_long (&oz_s_nppinuse, -rsize);
}

/************************************************************************/
/*									*/
/*  Free memory to non-paged pool					*/
/*									*/
/*    Input:								*/
/*									*/
/*	size = size (in bytes) of memory to be freed			*/
/*	adrs = address of memory to be freed				*/
/*									*/
/*	softint <= smplock <= np					*/
/*									*/
/*    Output:								*/
/*									*/
/*	nonpaged = updated to include the given memory			*/
/*									*/
/************************************************************************/

void oz_knl_nppfreesiz (OZ_Memsize size, void *adrs)

{
  uLong np;

  np = oz_hw_smplock_wait (&oz_s_smplock_np);		// lock so someone else can't update 'nonpaged' whilst we are
  oz_s_npptotal += size;				// increase the total amount of npp available
							// add this memory to the list of available memory
  nonpaged = oz_freesiz (nonpaged, size, adrs, (void *)oz_hw_smplock_wait, (void *)oz_hw_smplock_clr, &oz_s_smplock_np);
  oz_hw_smplock_clr (&oz_s_smplock_np, np);		// unlock it all
}

/************************************************************************/
/*									*/
/*  Dump non-paged pool usage						*/
/*									*/
/************************************************************************/

void oz_knl_npp_dump (void)

{
#if ENABLEDUMP
  int i;
  Npphdr *npphdr;
  uLong np, sizes[128];
  void *rtnadrs[128];

  memset (rtnadrs, 0, sizeof rtnadrs);
  memset (sizes,   0, sizeof sizes);
  np = oz_hw_smplock_wait (&oz_s_smplock_np);
  for (npphdr = allocated; npphdr != NULL; npphdr = npphdr -> next) {
    for (i = 0; i < 128; i ++) if (npphdr -> rtnadr == rtnadrs[i]) break;
    if (i < 128) sizes[i] += npphdr -> rsize;
    else {
      for (i = 0; i < 128; i ++) if (rtnadrs[i] == 0) break;
      if (i < 128) {
        rtnadrs[i] = npphdr -> rtnadr;
        sizes[i]   = npphdr -> rsize;
      }
    }
  }
  oz_hw_smplock_clr (&oz_s_smplock_np, np);
  oz_knl_printk ("oz_knl_npp_dump:");
  np = 0;
  for (i = 0; i < 128; i ++) {
    if (sizes[i] != 0) {
      if (np % 5 == 0) oz_knl_printk ("\n");
      oz_knl_printk (" %6u@%-6X", sizes[i], rtnadrs[i]);
      np ++;
    }
  }
  oz_knl_printk ("\n");
#else
  oz_knl_printk ("oz_knl_npp_dump: dummied out\n");
#endif
}

/************************************************************************/
/*									*/
/*  Allocate paged memory						*/
/*									*/
/*  This memory comes out of the system oz_sys_pdata memory which is 	*/
/*  expandable on demand						*/
/*									*/
/*    Input:								*/
/*									*/
/*	size = number of bytes wanted					*/
/*	quotaed = 0 : no quota checking, crash if can't allocate	*/
/*	          1 : quota checking, NULL if no quota or no memory	*/
/*									*/
/*	smplevel = softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_pgpmalloq = pointer to memory block			*/
/*									*/
/************************************************************************/

void *oz_knl_pgpmalloq (OZ_Memsize size, int quotaed)

{
  Long pgppeak;
  Npphdr *pgphdr;
  OZ_Memsize rsize;
  OZ_Quota *quota;

  /* Make sure there is enough quota */

  quota = NULL;
  if (quotaed) {
    quota = OZ_KNL_QUOTA_DEFAULT;
    if ((quota != NULL) && !oz_knl_quota_debit (quota, OZ_QUOTATYPE_PGP, size + sizeof *pgphdr)) return (NULL);
  }

  /* Sufficient quota, try to allocate memory block */

  pgphdr = oz_sys_pdata_malloc (OZ_PROCMODE_SYS, size + sizeof *pgphdr);

  /* If allocated, fill the header in */

  if (pgphdr != NULL) {
    pgphdr -> quota = quota;
    pgphdr -> rsize = size;
    return (pgphdr + 1);
  }

  /* Failed, crash if not quotaed (caller isn't prepared to handle it) */

  if (!quotaed) oz_crash ("oz_knl_pgpmalloq: out of non-paged memory, tried to get %u bytes", size);

  /* Return NULL if caller can handle it (although system probably won't last long) */

  if (quota != NULL) oz_knl_quota_credit (quota, OZ_QUOTATYPE_PGP, size + sizeof *pgphdr, -1);
  return (NULL);
}

/************************************************************************/
/*									*/
/*  Free paged pool memory						*/
/*									*/
/*    Input:								*/
/*									*/
/*	adrs = address of memory to be freed as returned by 		*/
/*	       oz_knl_pgpmalloq						*/
/*									*/
/*	smplevel = softint						*/
/*									*/
/************************************************************************/

void oz_knl_pgpfree (void *adrs)

{
  Npphdr *pgphdr;
  OZ_Memsize rsize;
  OZ_Quota *quota;

  pgphdr = adrs;
  quota  = (-- pgphdr) -> quota;
  rsize  = pgphdr -> rsize;
  oz_sys_pdata_free (OZ_PROCMODE_SYS, pgphdr);
  if (quota != NULL) oz_knl_quota_credit (quota, OZ_QUOTATYPE_PGP, rsize, -1);
}

void oz_knl_pgpvalid (void *adrs)

{ }
