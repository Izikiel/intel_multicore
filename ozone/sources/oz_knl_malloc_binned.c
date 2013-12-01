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
/*  General heap memory allocation routines				*/
/*									*/
/*    The list is composed of an array of varying sized blocks, the 	*/
/*    size of which can be found in .size.  The size and state at the 	*/
/*    beginning of the block should match the size and state at the 	*/
/*    end of the block.							*/
/*									*/
/*    At the very beginning of the list is a block of type END and 	*/
/*    with .size = 1.  This is used to eliminate a special case when 	*/
/*    allocating or freeing the following block.  Since it is of type 	*/
/*    END, it will not merge with either a FREE or ALLOC block.		*/
/*									*/
/*    At the very end is a block of type END with .size 0.		*/
/*									*/
/*    The size includes the ends and the guards and is in units of 	*/
/*    sizeof (Block).							*/
/*									*/
/************************************************************************/

#define _OZ_KNL_MALLOC_C

#include "ozone.h"

#include "oz_knl_hw.h"
#include "oz_knl_malloc.h"
#include "oz_knl_printk.h"
#include "oz_knl_sdata.h"

#define PAGESIZE (1 << OZ_HW_L2PAGESIZE)

typedef enum { STATE_FREE, STATE_BIN, STATE_ALLOC, STATE_END } State;

typedef struct Block Block;
struct Block { OZ_Memsize size;		/* size of this subblock */
               State state;		/* state of the block */
               int physcontig;		/* it is from a phys contig alloc */
               uLong pad1;		/* pad to 16-byte boundary */
             };
#define L2BS 4				/* log2 (sizeof (Block)) */
					/* sizeof Block is the alignment, also */

#define NGUARD 0 /* 0, 1, 2 */	/* number of blocks to use in each guard band */
#define POISON 0 /* 0, 1, 2 */	/* 0: no poison; 1: poison but don't check; 2: poison and check */
#define FULLCHECK 0 /* 0, 1 */	/* full checking every step (very slow) */

#if FULLCHECK			/* if full checking, make sure poison is turned all the way on */
#undef POISON
#define POISON 2
#endif

						/* 1600 bytes because ethernet packets are a little over 1500 */
#define NBINS (1600 >> L2BS)			/* number of bins to have for lookaside list */
						/* bin[0]       = blocks of data size     1<<L2BS bytes */
						/* bin[1]       = blocks of data size     2<<L2BS bytes */
						/* bin[NBINS-1] = blocks of data size NBINS<<L2BS bytes */

struct OZ_Memlist { OZ_Memlist *next;		/* next in chain of areas */
                    Block *base;		/* base address of the memory area */
                    OZ_Memsize size;		/* total size of memory area (in blocks), including both ends */
                    Block *firstfree;		/* lowest addressed block known to be free (and not in a bin) */
					/* The rest of the elements are valid only on the first memlist of a chain of memlists */
                    uLong (*lock) (void *lockprm);
                    void (*unlk) (void *lockprm, uLong lk);
                    void *lockprm;
                    Block *pcbins[NBINS];	/* lookasides that are physically contiguous - atomic updates only */
                    Block *pfbins[NBINS];	/* lookasides that are physically fragmented - atomic updates only */
                  };

static Block *malloctry (OZ_Memlist *memlist, OZ_Memsize ms, int pc);
static void coalesce (OZ_Memlist *memlist);
static void release (OZ_Memsize size, Block *mp);
static void checkfirstfree (OZ_Memlist *memlist);
static void checkguards (Block *mpbeg);
static void storepoison (OZ_Memsize size, Block *mp);
static void checkpoison (OZ_Memsize size, Block *mp);

/************************************************************************/
/*									*/
/*  Free an area of memory of the given size				*/
/*									*/
/*    Input:								*/
/*									*/
/*	memlist = memory listhead or NULL to start			*/
/*	size    = size (in bytes) of memory to be freed			*/
/*	adrs    = address of memory to be freed				*/
/*	lock    = lock routine, eg, oz_hw_smplock_wait			*/
/*	unlk    = unlock routine, eg, oz_hw_smplock_clr			*/
/*	lockprm = parameter to pass to lock/unlk, eg, &oz_s_smplock_np	*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_freesiz = memlist modified					*/
/*									*/
/*    Note:								*/
/*									*/
/*	The caller should lock the lock before calling this routine 	*/
/*	and unlock it after it returns, so you don't have two threads 	*/
/*	modifying the 'memlist->next' list at the same time.		*/
/*									*/
/************************************************************************/

OZ_Memlist *oz_freesiz (OZ_Memlist *memlist, 
                        OZ_Memsize size, 
                        void *adrs, 
                        uLong (*lock) (void *lockprm), 
                        void (*unlk) (void *lockprm, uLong lk), 
                        void *lockprm)

{
  Block *mp;
  OZ_Memlist *newmemlist;
  OZ_Pointer ptr, ptr0;

#if FULLCHECK
  if (memlist != NULL) oz_mlvalid (memlist);
#endif

  if (size < sizeof *memlist + (4 + 2*NGUARD) * sizeof (Block)) return (memlist);

  newmemlist = adrs;				/* point to and clear memlist */
  memset (newmemlist, 0, sizeof *newmemlist);

  ptr0 = (OZ_Pointer)adrs;			/* point to where we started */
  ptr  = (OZ_Pointer)adrs;
  ptr += sizeof *newmemlist;			/* increment past the memlist header */
  ptr  = ((ptr + sizeof *mp - 1) / sizeof *mp) * sizeof *mp; /* round up to 'rounding' boundary */

  size = (ptr0 + size - ptr) / sizeof *mp;	/* determine size left, in (Block) units */
  mp   = (Block *)ptr;				/* point to the memory block array of 'size' elements */

  newmemlist -> base = mp;			/* save base address of everything */
  newmemlist -> size = size;			/* save size of everything, including both ends */
  newmemlist -> firstfree = mp + 1;		/* first free one will be here */

#if POISON
  storepoison (size, mp);
#endif

  mp[0].size  = 1;				/* set up an 'end' marker */
  mp[0].state = STATE_END;
  mp[1].size  = size - 2;			/* whole size less 2 for the end markers */
  mp[1].state = STATE_FREE;			/* set the big chunk state to FREE */
  mp[size-2].size  = size - 2;			/* set up the same thing at the end */
  mp[size-2].state = STATE_FREE;
  mp[size-1].size  = 0;				/* set up an end tag with size zero */
  mp[size-1].state = STATE_END;			/* and a special state */

#if FULLCHECK
  checkfirstfree (newmemlist);
#endif

  if (memlist == NULL) {			/* if it's the first, ... */
    newmemlist -> lock    = lock;		/* ... save lock/unlock routine stuff */
    newmemlist -> unlk    = unlk;
    newmemlist -> lockprm = lockprm;
#if FULLCHECK
    oz_mlvalid (newmemlist);
#endif
    return (newmemlist);			/* ... and return its pointer */
  }

  newmemlist -> next = memlist -> next;		/* link to keep the first one first */
  memlist -> next = newmemlist;			/* ... becuase it has all the bins in it */
#if FULLCHECK
  oz_mlvalid (memlist);
#endif
  return (memlist);				/* ... the order of the rest don't matter */
}

/************************************************************************/
/*									*/
/*  Allocate memory							*/
/*									*/
/*    Input:								*/
/*									*/
/*	memlist = free memory listhead					*/
/*	size    = number of bytes wanted				*/
/*	pc      = 0 : normal allocation					*/
/*	          1 : physically contiguous (ie, can't cross a page boundary)
/*									*/
/*    Output:								*/
/*									*/
/*	oz_malloc = NULL : no memory available				*/
/*	            else : pointer to memory block			*/
/*	*size_r = caller usable bytes actually allocated		*/
/*									*/
/************************************************************************/

void *oz_malloc (OZ_Memlist *memlist, OZ_Memsize size, OZ_Memsize *size_r, int pc)

{
  Block *volatile *binhd, *mp;
  uLong lk;
  OZ_Memsize ms;

  if (memlist == NULL) return (NULL);

#if FULLCHECK
  oz_mlvalid (memlist);
#endif

  /* Convert size to Block size */

  ms = (size + sizeof *mp - 1) / sizeof *mp;
  if (ms == 0) return (NULL);

  /* Maybe it can be found in a lookaside bin.  Only use atomic updates so we don't have to be locked for this stuff. */

  if (ms <= NBINS) {
    mp = NULL;								/* assume it is a physically contig malloc */
    if (!pc) {								/* see if physically contig malloc */
      binhd = memlist -> pfbins + ms - 1;				/* it isn't, try to get from fragmented list */
      do mp = *binhd;
      while ((mp != NULL) && !oz_hw_atomic_setif_ptr ((void *volatile *)binhd, *(Block **)(mp + 1), mp));
    }
    if (mp == NULL) {							/* see if phy contig or frag list was empty */
      binhd = memlist -> pcbins + ms - 1;				/* if so, try to get from contiguous list */
      do mp = *binhd;
      while ((mp != NULL) && !oz_hw_atomic_setif_ptr ((void *volatile *)binhd, *(Block **)(mp + 1), mp));
    }
    if (mp != NULL) {							/* see if we found a binned block */
#if NGUARD > 0
      storepoison (1, mp + 1);						/* if so, poison the first guard (it had the link in it) */
#endif
      mp[0].state = STATE_ALLOC;					/* change its state to 'allocated' */
      mp[ms+2*NGUARD+1].state = STATE_ALLOC;
      goto rtn;								/* return it to caller */
    }
  }

  /* Try to allocate from general population */

  lk = (*(memlist -> lock)) (memlist -> lockprm);			/* this requires external lock */
  mp = malloctry (memlist, ms, pc);					/* try to allocate */
  if (mp == NULL) {
    coalesce (memlist);							/* failed, free everything in bins to general population */
    mp = malloctry (memlist, ms, pc);					/* try to allocate again */
  }
  (*(memlist -> unlk)) (memlist -> lockprm, lk);			/* unlock */
  if (mp == NULL) return (NULL);
  mp -> physcontig = pc;						/* save whether it was a phys contig allocation request */

  /* Return pointer to data area */

#if (POISON == 0) && (NGUARD > 0)
  storepoison (NGUARD, mp + 1);						/* store poison in the guards */
  storepoison (NGUARD, mp + mp -> size - 1 - NGUARD);
#endif

rtn:
#if FULLCHECK
  oz_mlvalid (memlist);
#endif
  if (size_r != NULL) *size_r = (mp -> size - 2 - 2*NGUARD) * sizeof *mp; /* return number of usable bytes to caller */
  return (mp + 1 + NGUARD);						/* return pointer to data area of allocated block */
}

/* Same as oz_malloc, except caller already has memory list locked */

void *oz_malloc_lk (OZ_Memlist *memlist, OZ_Memsize size, OZ_Memsize *size_r, int pc)

{
  Block *volatile *binhd, *mp;
  OZ_Memsize ms;

  if (memlist == NULL) return (NULL);

#if FULLCHECK
  oz_mlvalid (memlist);
#endif

  /* Convert size to Block size */

  ms = (size + sizeof *mp - 1) / sizeof *mp;
  if (ms == 0) return (NULL);

  /* Maybe it can be found in a lookaside bin.  Only use atomic updates so we don't have to be locked for this stuff. */

  if (ms <= NBINS) {
    mp = NULL;								/* assume it is a physically contig malloc */
    if (!pc) {								/* see if physically contig malloc */
      binhd = memlist -> pfbins + ms - 1;				/* it isn't, try to get from fragmented list */
      do mp = *binhd;
      while ((mp != NULL) && !oz_hw_atomic_setif_ptr ((void *volatile *)binhd, *(Block **)(mp + 1), mp));
    }
    if (mp == NULL) {							/* see if phy contig or frag list was empty */
      binhd = memlist -> pcbins + ms - 1;				/* if so, try to get from contiguous list */
      do mp = *binhd;
      while ((mp != NULL) && !oz_hw_atomic_setif_ptr ((void *volatile *)binhd, *(Block **)(mp + 1), mp));
    }
    if (mp != NULL) {							/* see if we found a binned block */
#if NGUARD > 0
      storepoison (1, mp + 1);						/* if so, poison the first guard (it had the link in it) */
#endif
      mp[0].state = STATE_ALLOC;					/* change its state to 'allocated' */
      mp[ms+2*NGUARD+1].state = STATE_ALLOC;
      goto rtn;								/* return it to caller */
    }
  }

  /* Try to allocate from general population */

  mp = malloctry (memlist, ms, pc);					/* try to allocate */
  if (mp == NULL) {
    coalesce (memlist);							/* failed, free everything in bins to general population */
    mp = malloctry (memlist, ms, pc);					/* try to allocate again */
  }
  if (mp == NULL) return (NULL);
  mp -> physcontig = pc;						/* save whether it was a phys contig allocation request */

  /* Return pointer to data area */

#if (POISON == 0) && (NGUARD > 0)
  storepoison (NGUARD, mp + 1);						/* store poison in the guards */
  storepoison (NGUARD, mp + mp -> size - 1 - NGUARD);
#endif

rtn:
#if FULLCHECK
  oz_mlvalid (memlist);
#endif
  if (size_r != NULL) *size_r = (mp -> size - 2 - 2*NGUARD) * sizeof *mp; /* return number of usable bytes to caller */
  return (mp + 1 + NGUARD);						/* return pointer to data area of allocated block */
}

/* Try to malloc the block from the general population */

static Block *malloctry (OZ_Memlist *memlist, OZ_Memsize ms, int pc)

{
  Block *mp, *mpbeg, *mpend;
  OZ_Memlist *mlbest;
  OZ_Memsize bs, bytesize, roundup, size;
  OZ_Pointer byteaddr;

  size = ms * sizeof *mp;

  /* Add two for the overhead of the first and last boundary tags */
  /* Plus extra blocks for the guard bands at beginning and end   */

  ms += 2 + 2*NGUARD;

  if (pc) goto phycontig;

  /* Look for an element that will hold the requested size */

  mpbeg = NULL;								/* no 'best fit' found so far */
  do {
    mpend = NULL;							/* haven't found first free yet */
    for (mp = memlist -> firstfree; (bs = mp -> size) != 0; mp += bs) {	/* scan the list starting with lowest free block */
      if (mp -> state != STATE_FREE) continue;				/* skip if block is not free */
      if (mpend == NULL) mpend = mp;					/* save address of first free block in list */
      if (bs < ms) continue;						/* skip if block is too small */
      if (bs == ms) goto foundvar;					/* stop scanning if the exact size */
      if ((mpbeg == NULL) || (bs < mpbeg -> size)) { mpbeg = mp; mlbest = memlist; } /* save if it is the best fit so far */
    }
    if (mpend == NULL) mpend = mp;					/* if all memory is allocated, point first free at end */
    memlist -> firstfree = mpend;					/* save address of first free (or pointer to end of list) */
#if FULLCHECK
    checkfirstfree (memlist);
#endif
    memlist = memlist -> next;						/* try next list */
  } while (memlist != NULL);
  if (mpbeg == NULL) return (NULL);					/* if no best fit found, return failure status */
  mp = mpbeg;								/* ok, use the best fit */
  bs = mpbeg -> size;
  memlist = mlbest;
  goto foundvar_nosetff;
foundvar:
  if (mpend == NULL) mpend = mp;					/* if no free element found yet, start here next time */
  memlist -> firstfree = mpend;						/* update pointer to first free in list (or close to it) */
#if FULLCHECK
  checkfirstfree (memlist);
#endif
foundvar_nosetff:
  mpend = mp + bs - 1;							/* point to end of the whole thing */

  /* Before we give (part of) it away, make sure it is still poisoned */

  if (mpend -> state != STATE_FREE) {
    oz_crash ("oz_malloc: end state %d", mpend -> state);
  }
  if (mpend -> size != bs) {
    oz_crash ("oz_malloc: end size %u not %u", mpend -> size, bs);
  }
#if POISON > 1
  checkpoison (mp -> size - 2, mp + 1);
#endif

allocvar:

  /* If it takes the whole thing, change state and return pointer */

  if (bs <= ms + 4) mpend -> state = mp -> state = STATE_ALLOC;

  /* It only takes part of it, return the lowest addressed part */
  /* Use the lowest end because the pc alloc expects that       */

  else {
    mpbeg = mp + ms - 1;				/* point to end of part we alloc */

    mpbeg -> size  = mp -> size  = ms;			/* set up size of block we allocate */
    mpbeg -> state = mp -> state = STATE_ALLOC;		/* set its state to 'allocated' */

    mpend -> size  = mpbeg[1].size = mpend - mpbeg;	/* set up size of what's left over */
    mpbeg[1].state = STATE_FREE;			/* set its state to 'free' */
  }

  return (mp);

  /**********************************/
  /* Physically contiguous allocate */
  /**********************************/

phycontig:
  if (size > PAGESIZE) return (NULL);

  do {
    mpend = NULL;							/* haven't found first free yet */
    for (mp = memlist -> firstfree; (bs = mp -> size) != 0; mp += bs) {	/* scan the list starting with lowest known free block */
      if (mp -> state != STATE_FREE) continue;
      if (mpend == NULL) mpend = mp;					/* save address of first free memory block */
      if (bs < ms) continue;
      bytesize  = (bs - 2 - 2*NGUARD) * sizeof *mp;			/* size of data area in bytes */
      byteaddr  = (OZ_Pointer)(mp + 1 + NGUARD);			/* address of data area */
      roundup   = ((PAGESIZE - 1) & ~ byteaddr) + 1;			/* bytes to end of page */
      if (roundup >= size) goto foundvar;				/* if it fits before end of page, use it as is */
      if (roundup < (3 + NGUARD) * sizeof *mp) roundup = (3 + NGUARD) * sizeof *mp; /* must be enough room to split */
      if (bytesize <= roundup) continue;
      bytesize -= roundup;						/* move to beg of page */
      byteaddr += roundup;
      if (bytesize < size) continue;
      roundup = ((PAGESIZE - 1) & ~ byteaddr) + 1;			/* bytes to end of page */
      if (roundup >= size) goto pc_foundvar;				/* if still room, use it */
    }
    if (mpend == NULL) mpend = mp;					/* if all memory is allocated, point first free at end */
    memlist -> firstfree = mpend;					/* save address of first free (or pointer to end of list) */
#if FULLCHECK
    checkfirstfree (memlist);
#endif
    memlist = memlist -> next;						/* try next list */
  } while (memlist != NULL);
  return (NULL);

pc_foundvar:
  if (mpend == NULL) mpend = mp;					/* if no free element found yet, start here next time */
  memlist -> firstfree = mpend;						/* update pointer to first free in list (or close to it) */
#if FULLCHECK
  checkfirstfree (memlist);
#endif
  mpend = mp + bs - 1;							/* point to end of the whole thing */

  /* Before we give part of it away, make sure it is still poisoned */

  if (mpend -> state != STATE_FREE) {
    oz_crash ("oz_malloc: end state %d", mpend -> state);
  }
  if (mpend -> size != bs) {
    oz_crash ("oz_malloc: end size %u not %u", mpend -> size, bs);
  }
#if POISON > 1
  checkpoison (mp -> size - 2, mp + 1);
#endif

  /* Allocate memory such that we return 'byteaddr' as the user data pointer  */
  /* We know there is enough room between mp and byteaddr to make a new block */
  /* We also know byteaddr is 'mp' aligned                                    */

  if (byteaddr & (sizeof *mp - 1)) oz_crash ("oz_malloc: byteaddr %x not aligned", byteaddr);

  mpbeg  = (Block *)byteaddr;		/* point to beg of area to alloc */
  mpbeg -= 1 + NGUARD;

  if (mpbeg - mp < 2) oz_crash ("oz_malloc: not enough room to split");
  if (mpend + 1 < mpbeg + ms) oz_crash ("oz_malloc: not enough room left");

  mpbeg[-1].state = STATE_FREE;		/* free off little block before us */
  mp -> size = mpbeg[-1].size = mpbeg - mp;

  bs -= mpbeg - mp;			/* reduce what's left by that much */
  mp  = mpbeg;				/* point to what's left */
  mp -> state = STATE_FREE;		/* make it look nice for allocvar and validate call */
  mpend -> size = mp -> size = bs;

#if FULLCHECK
  oz_mlvalid (memlist);
#endif

  goto allocvar;			/* allocate from there */
}

/* Release everything in all bins to the general population, merging with neighbors as much as possible */

static void coalesce (OZ_Memlist *memlist)

{
  Block *mp;
  Block *volatile *bp;
  OZ_Memlist *ml;
  OZ_Memsize bs;

#if FULLCHECK
  oz_mlvalid (memlist);
#endif

  for (bp = memlist -> pfbins;; bp = memlist -> pcbins) {	/* do fragmented then contiguous lists */
    for (bs = 0; bs < NBINS; bs ++) {				/* do each blocksize within each list */
      while ((mp = *bp) != NULL) {				/* repeat as long as there are blocks in the list */
        if (oz_hw_atomic_setif_ptr ((void *volatile *)bp, *(Block **)(mp + 1), mp)) { /* try to unlink the binned block */
#if POISON
          storepoison (1, mp + 1);				/* fix that 1st guard to be all poison */
#endif
          release (bs + 3 + 2*NGUARD, mp);			/* release block to the general population */
        }
      }
      bp ++;							/* increment to next bin listhead */
    }
    if (bp == memlist -> pcbins + NBINS) break;			/* stop if we have done both sets of bins */
  }

  for (ml = memlist; ml != NULL; ml = ml -> next) {		/* reset all firstfree's */
    ml -> firstfree = ml -> base + 1;				/* point to first element in list as the first free */
#if FULLCHECK
    checkfirstfree (ml);
#endif
  }								/* it will get fixed when the allocate re-scans the list */

#if FULLCHECK
  oz_mlvalid (memlist);
#endif
}

/************************************************************************/
/*									*/
/*  Validate an area of memory previously allocated by oz_malloc	*/
/*									*/
/*    Input:								*/
/*									*/
/*	memlist = free memory listhead					*/
/*	adrs    = address of memory to be validated as returned by 	*/
/*	          oz_malloc						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_valid = number of bytes that are allocated			*/
/*									*/
/************************************************************************/

OZ_Memsize oz_valid (OZ_Memlist *memlist, void *adrs)

{
  Block *mp;

  mp  = adrs;
  mp -= 1 + NGUARD;

  checkguards (mp);

#if FULLCHECK
  oz_mlvalid (memlist);
#endif

  /* Return size (same value that corresponding oz_malloc returned) */

  return ((mp -> size - 2 - 2*NGUARD) * sizeof *mp);
}

/************************************************************************/
/*									*/
/*  Free an area of memory previously allocated by oz_malloc		*/
/*									*/
/*    Input:								*/
/*									*/
/*	memlist = free memory listhead					*/
/*	adrs    = address of memory to be freed as returned by 		*/
/*	          oz_malloc						*/
/*									*/
/*    Output:								*/
/*									*/
/*	*memlist = updated to include the given memory			*/
/*	oz_free = number of bytes that were released			*/
/*									*/
/************************************************************************/

OZ_Memsize oz_free (OZ_Memlist *memlist, void *adrs)

{
  Block *mp, *mpend;
  Block *volatile *binhd;
  uLong lk;
  OZ_Memlist *ml;
  OZ_Memsize size;
  OZ_Pointer mp1, mp2;

  mp  = adrs;
  mp -= 1 + NGUARD;

  checkguards (mp);					/* poison should still be in guards */

#if FULLCHECK
  oz_mlvalid (memlist);
#endif

  size = mp -> size;					/* get inclusive size in Block units */

#if POISON
  storepoison (size - 2 - 2*NGUARD, mp + 1 + NGUARD);	/* poison the data area, leave size/state blocks alone */
							/* we know the guards are still poison from the 'checkguards' call */
#endif

  /* If it fits in a bin, just put it in the bin (physically contiguous or fragmented) */

  if (size <= NBINS + 2 + 2*NGUARD) {			/* see if it fits in a bin */
    mp[0].state = STATE_BIN;				/* change state to 'in bin' */
    mp[size-1].state = STATE_BIN;
    size -= 2 + 2*NGUARD;				/* get size of data portion */
    binhd = memlist -> pfbins + size - 1;		/* assume we will put block on fragmented list */
							/* (that way they all get used an equal amount) */
							/* (if caller subsequently does a pc malloc and there is no memory, */
							/*  a coalesce will be done which will reset the bins) */
    if (mp -> physcontig) {				/* see if it was a physically contiguous allocation */
      binhd = memlist -> pcbins + size - 1;		/* if so, assume it is contiguous */
    }
    do *(Block **)(mp + 1) = *binhd;			/* link block to bin's list */
    while (!oz_hw_atomic_setif_ptr ((void *volatile *)binhd, mp, *(Block **)(mp + 1)));
#if FULLCHECK
    oz_mlvalid (memlist);
#endif
    return (size * sizeof *mp);
  }

  /* Doesn't fit in a bin, release to the general population */

  lk = (*(memlist -> lock)) (memlist -> lockprm);
  release (size, mp);

  /* Maybe update the firstfree to include the freed block */

  for (ml = memlist;; ml = ml -> next) if ((mp > ml -> base) && (mp < ml -> base + ml -> size)) break;
  if (ml -> firstfree > mp) ml -> firstfree = mp;
#if FULLCHECK
  checkfirstfree (ml);
#endif
  (*(memlist -> unlk)) (memlist -> lockprm, lk);

#if FULLCHECK
  oz_mlvalid (memlist);
#endif

  /* Return size freed off (same value that corresponding oz_malloc returned) */

  return ((size - 2 - 2*NGUARD) * sizeof *mp);
}

/* Release a block of memory (allocated or binned) to the general population.  Merge with neighbors where possible. */

static void release (OZ_Memsize size, Block *mp)

{
  Block *mpend;

  mpend = mp + size - 1;				/* point to end (inclusize) */

#if POISON
  storepoison (1, mp);					/* poison the headers */
  storepoison (1, mpend);
#endif

  if (mp[-1].state == STATE_FREE) {			/* if one before is free, back up to include it */
    size = mp[-1].size;
#if POISON
    storepoison (1, mp - 1);
#endif
    mp -= size;
  }
  if (mpend[1].state == STATE_FREE) {			/* if one after is free, advance to include it */
    size = mpend[1].size;
#if POISON
    storepoison (1, mpend + 1);
#endif
    mpend += size;
  }

  mpend -> size  = mp -> size  = mpend + 1 - mp;	/* set up new headers */
  mpend -> state = mp -> state = STATE_FREE;
}

/************************************************************************/
/*									*/
/*  Validate an entire memory list					*/
/*									*/
/************************************************************************/

void oz_mlvalid (OZ_Memlist *memlist)

{
  Block **bp, *mpbeg, *mpend;
  int foundfirstfree;
  uLong lk;
  OZ_Memlist *ml;
  OZ_Memsize nbinned, bs;

  lk = (*(memlist -> lock)) (memlist -> lockprm);

  nbinned = 0;
  for (ml = memlist; ml != NULL; ml = ml -> next) {

    /* Scan the list from beginning to end */

    foundfirstfree = 0;
    for (mpbeg = ml -> base; (bs = mpbeg -> size) != 0; mpbeg = mpend + 1) {

      if (bs >= ml -> size) oz_crash ("oz_mlvalid: block size %u bigger than list size %u", bs, ml -> size);

      /* Maybe we have found the first free block */

      if (mpbeg == ml -> firstfree) foundfirstfree = 1;

      /* Point to end of block (inclusive) */

      mpend = mpbeg + bs - 1;

      /* The states and sizes for both ends of block should match */

      if (mpend -> state != mpbeg -> state) {
        oz_crash ("oz_mlvalid: %p beg state %d, %p end state %d", mpbeg, mpbeg -> state, mpend, mpend -> state);
      }
      if (mpend -> size != bs) {
        oz_crash ("oz_mlvalid: %p beg size %u, %p end size %u", mpbeg, bs, mpend, mpend -> size);
      }

      /* Do other checking based on state */

      switch (mpbeg -> state) {

        /* Free's should have everything in the middle poison */

        case STATE_FREE: {
#if POISON
          checkpoison (bs - 2, mpbeg + 1);
#endif
          break;
        }

        /* Allocated should have good guards */

        case STATE_ALLOC: {
          checkguards (mpbeg);
          break;
        }

        /* Binned should have poison everywhere except the first pointer worth of the first guard */

        case STATE_BIN: {
#if POISON
          checkpoison (bs - 3, mpbeg + 2);
#endif
          nbinned ++;
          break;
        }

        /* This should only show up at the beginning and have size 1 */

        case STATE_END: {
          if (bs != 1) {
            oz_crash ("oz_mlvalid: beginning marker size %u", bs);
          }
          if (mpbeg != ml -> base) {
            oz_crash ("oz_mlvalid: beginning marker at %p", mpbeg);
          }
          break;
        }

        /* Don't know what this is */

        default: {
          oz_crash ("oz_mlvalid: unknown state %d", mpbeg -> state);
        }
      }
    }

    /* The last one should be a state END and should be at the end of the memory area */

    if (mpbeg -> state != STATE_END) oz_crash ("oz_mlvalid: size 0 block state %d", mpbeg -> state);
    if (mpbeg != ml -> base + ml -> size - 1) oz_crash ("oz_mlvalid: end %p not at %p", mpbeg, ml -> base + ml -> size - 1);

    /* Make sure we found the first free block in there somewhere */

    if (!foundfirstfree && (ml -> firstfree != mpbeg)) {
      oz_crash ("oz_mlvalid: didn't find firstfree %p, base %p, end %p", ml -> firstfree, ml -> base, mpbeg);
    }
  }

  /* Make sure all the blocks marked as 'binned' are actually in a bin */

  for (bp = memlist -> pfbins;; bp = memlist -> pcbins) {
    for (bs = 1; bs <= NBINS; bs ++) {
      for (mpbeg = bp[bs-1]; mpbeg != NULL; mpbeg = *(Block **)(mpbeg + 1)) {
        mpend = mpbeg + bs + 2 + 2*NGUARD - 1;
        if (mpbeg -> state != STATE_BIN) oz_crash ("oz_mlvalid: block on bin list %u has beg state %d", bs, mpbeg -> state);
        if (mpend -> state != STATE_BIN) oz_crash ("oz_mlvalid: block on bin list %u has end state %d", bs, mpend -> state);
        if (mpbeg -> size  != bs + 2 + 2*NGUARD) oz_crash ("oz_mlvalid: block on bin list %u has beg size %u", bs, mpbeg -> size);
        if (mpend -> size  != bs + 2 + 2*NGUARD) oz_crash ("oz_mlvalid: block on bin list %u has beg size %u", bs, mpend -> size);
        if (nbinned == 0) oz_crash ("oz_mlvalid: loop in bin list %u", bs);
        nbinned --;
      }
    }
    if (bp == memlist -> pcbins) break;
  }
  if (nbinned != 0) oz_crash ("oz_mlvalid: couldn't find %u binned blocks", nbinned);

  (*(memlist -> unlk)) (memlist -> lockprm, lk);
}

static void checkfirstfree (OZ_Memlist *memlist)

{
  Block *mp;
  OZ_Memsize bs;

  for (mp = memlist -> base; (bs = mp -> size) != 0; mp += bs) if (memlist -> firstfree == mp) return;
  if (memlist -> firstfree != mp) {
    oz_crash ("oz_knl_malloc checkfirstfree: cant find firstfree %p in %p end %p", memlist -> firstfree, memlist -> base, memlist -> base + memlist -> size);
  }
}

/************************************************************************/
/*									*/
/*  Check the guards on an allocated block of memory			*/
/*									*/
/************************************************************************/

static void checkguards (Block *mpbeg)

{
  Block *mpend;

  mpend = mpbeg + mpbeg -> size - 1;
  if (mpbeg -> state != STATE_ALLOC) {
    oz_crash ("oz_mlvalid: mpbeg state %d found in alloc sub-block", mpbeg -> state);
  }
  if (mpend -> state != STATE_ALLOC) {
    oz_crash ("oz_mlvalid: mpend state %d found in alloc sub-block", mpend -> state);
  }
  if (mpend -> size != mpbeg -> size) {
    oz_crash ("oz_mlvalid: alloc mpbeg size %u ne mpend size %u", mpbeg -> size, mpend -> size);
  }

  checkpoison (NGUARD, mpbeg + 1);
  checkpoison (NGUARD, mpend - NGUARD);
}

/************************************************************************/
/*									*/
/*  Store poison pattern in memory block				*/
/*									*/
/************************************************************************/

static void storepoison (OZ_Memsize size, Block *mp)

{
  uLong *lp;
  OZ_Memsize i;

  lp = (uLong *)mp;
  for (i = size * sizeof *mp / sizeof (uLong); i > 0; -- i) {
    *(lp ++) = 0xDEADBEEF;
  }
}

/************************************************************************/
/*									*/
/*  Make sure a block is still poisoned					*/
/*									*/
/************************************************************************/

static void checkpoison (OZ_Memsize size, Block *mp)

{
  uLong *lp;
  OZ_Memsize i;

  lp = (uLong *)mp;
  for (i = size * sizeof *mp / sizeof (uLong); i > 0; -- i) {
    if (*(lp ++) != 0xDEADBEEF) {
      oz_crash ("oz_malloc checkpoison: poison missing at %p", -- lp);
    }
  }
}
