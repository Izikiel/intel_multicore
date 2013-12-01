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
/*  Shut down the system nicely						*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_knl_hw.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_objtype.h"
#include "oz_knl_sdata.h"
#include "oz_knl_shuthand.h"
#include "oz_knl_status.h"

struct OZ_Shuthand { OZ_Objtype objtype;		/* OZ_OBJTYPE_SHUTHAND */
                     OZ_Shuthand *next;			/* next in shuthands list */
                     void (*entry) (void *param);	/* entrypoint to call */
                     void *param;			/* param to pass to routine */
                   };

static OZ_Shuthand *shuthands = NULL;

/************************************************************************/
/*									*/
/*  This routine is called to create a shutdown handler table entry	*/
/*									*/
/*    Input:								*/
/*									*/
/*	entry = entrypoint to be called back at softint level		*/
/*	param = parameter to pass to shutdown handler			*/
/*	smplevel <= sh							*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_shuthand_create = shutdown handler entry			*/
/*									*/
/************************************************************************/

OZ_Shuthand *oz_knl_shuthand_create (void (*entry) (void *param), void *param)

{
  OZ_Shuthand *shuthand;
  uLong sh;

  shuthand = OZ_KNL_NPPMALLOC (sizeof *shuthand);
  shuthand -> objtype = OZ_OBJTYPE_SHUTHAND;
  shuthand -> entry   = entry;
  shuthand -> param   = param;
  sh = oz_hw_smplock_wait (&oz_s_smplock_sh);
  shuthand -> next = shuthands;
  shuthands = shuthand;
  oz_hw_smplock_clr (&oz_s_smplock_sh, sh);
  return (shuthand);
}

/************************************************************************/
/*									*/
/*  Delete previously declared entry					*/
/*									*/
/*    Input:								*/
/*									*/
/*	shuthand = as returned by oz_knl_shuthand_create		*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_shuthand_delete = 0 : entry was not found in table	*/
/*	                         1 : entry was found and deleted	*/
/*									*/
/************************************************************************/

int oz_knl_shuthand_delete (OZ_Shuthand *shuthand)

{
  OZ_Shuthand **lshuthand, *xshuthand;
  uLong sh;

  sh = oz_hw_smplock_wait (&oz_s_smplock_sh);
  for (lshuthand = &shuthands; (xshuthand = *lshuthand) != shuthand; lshuthand = &(xshuthand -> next)) {
    if (xshuthand == NULL) {
      oz_hw_smplock_clr (&oz_s_smplock_sh, sh);
      return (0);
    }
  }
  *lshuthand = xshuthand -> next;
  oz_hw_smplock_clr (&oz_s_smplock_sh, sh);
  OZ_KNL_NPPFREE (xshuthand);
  return (1);
}

/************************************************************************/
/*									*/
/*  Shut the system down						*/
/*									*/
/*    Input:								*/
/*									*/
/*	smplevel = softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	system ready to halt or reboot					*/
/*									*/
/************************************************************************/

void oz_knl_shutdown (void)

{
  char *p, waitstr[OZ_HW_MAXCPUS*4];
  Long cpuidx, mycpuidx;
  OZ_Shuthand *shuthand;
  uLong sh;

  /* Indicate to anyone who cares that a shutdown is in progress */

  mycpuidx = oz_hw_cpu_getcur ();
  oz_s_shutdown = mycpuidx;
  OZ_HW_MB;

  /* Call the shutdown handlers if LIFO order to do stuff like flush disk caches, etc. */

#ifdef OZ_HW_TYPE_486
  oz_dev_vgavideo_blank (0);							/* make sure the video screen is on */
#endif
  oz_knl_printk ("oz_knl_shutdown: calling handlers...\n");			/* output message to screen */
check:
  sh = oz_hw_smplock_wait (&oz_s_smplock_sh);					/* lock the list */
  shuthand = shuthands;								/* see if anything there */
  if (shuthand != NULL) {
    shuthands = shuthand -> next;						/* if so, unlink it */
    oz_hw_smplock_clr (&oz_s_smplock_sh, sh);					/* release lock */
    (*(shuthand -> entry)) (shuthand -> param);					/* call handler at softint level */
    OZ_KNL_NPPFREE (shuthand);							/* free it off */
    goto check;									/* check for more handlers */
  }
  oz_hw_smplock_clr (&oz_s_smplock_sh, sh);					/* no more handlers, unlock list */

  /* Halt all other cpu's at softint level */

  mycpuidx = oz_hw_cpu_getcur ();						/* maybe we're on a different cpu now */
  oz_s_shutdown = mycpuidx;
  OZ_HW_MB;

  if (!oz_s_inloader) {								/* there is no oz_knl_diag in the loader */
    waitstr[0] = 0;								/* assume there's nothing else to halt */
    for (cpuidx = 0; cpuidx < OZ_HW_MAXCPUS; cpuidx ++) {			/* scan through all possible cpu's */
      if ((cpuidx != mycpuidx) && (oz_s_cpusavail & (1 << cpuidx))) {		/* see if that cpu is online */
        if (waitstr[0] != 0) strcat (waitstr, ",");				/* ok, say we will wait for it */
        p = waitstr + strlen (waitstr);
        oz_hw_itoa (cpuidx, 3, p);
      }
    }
    if (waitstr[0] != 0) {							/* see if we found any other cpu's to halt */
      p = "";
      if (strchr (waitstr, ',') != NULL) p = "'s";
      oz_knl_printk ("oz_knl_shutdown: halting cpu%s %s...\n", p, waitstr);	/* announce that we're halting the others */
      oz_hw_diag ();								/* halt the others (cause them to call oz_knl_diag at softint level) */
      while (oz_s_cpusavail & ~(1 << mycpuidx)) {}				/* wait for the others to halt */
    }
  }

  /* Call device drivers to reset hardware controllers (in case this is the loader about to jump to the kernel) */
  /* Do this after halting all other cpus to simplify the shutdown routines                                     */

  oz_knl_printk ("oz_knl_shutdown: shutting down devices...\n");
  oz_knl_devshut ();

  /* We're done */

  oz_knl_printk ("oz_knl_shutdown: shutdown complete\n");
}
