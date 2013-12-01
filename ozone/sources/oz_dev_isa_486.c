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
/*  Support for ISA interrupts						*/
/*									*/
/************************************************************************/

#define _OZ_DEV_ISA_C

#include "ozone.h"
#include "oz_dev_isa.h"
#include "oz_knl_hw.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_objtype.h"

struct OZ_Dev_Isa_Irq { OZ_Objtype objtype;
                        void (*entry) (void *param, OZ_Mchargs *mchargs);
                        void *param;
                        OZ_Smplock *smplock;
                        OZ_Hw486_irq_many irq_many;
                        uLong irq;
                      };

static int isa_interrupt (void *isairqv, OZ_Mchargs *mchargs);

/************************************************************************/
/*									*/
/*  Allocate an IRQ for a ISA device					*/
/*									*/
/*    Input:								*/
/*									*/
/*	irq   = irq number						*/
/*	entry = entrypoint of routine to call upon interrupt		*/
/*	param = parameter to pass to 'entry' routine			*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_dev_isa_irq_alloc = interrupt struct pointer			*/
/*									*/
/************************************************************************/

OZ_Dev_Isa_Irq *oz_dev_isa_irq_alloc (uLong irq, void (*entry) (void *param, OZ_Mchargs *mchargs), void *param)

{
  OZ_Dev_Isa_Irq *isairq;

  isairq = OZ_KNL_NPPMALLOC (sizeof *isairq);
  isairq -> objtype = OZ_OBJTYPE_ISAIRQ;
  isairq -> entry = entry;
  isairq -> param = param;
  isairq -> irq_many.entry = isa_interrupt;
  isairq -> irq_many.param = isairq;
  isairq -> irq_many.descr = "isa_interrupt";

  isairq -> irq = irq;
  isairq -> smplock = oz_hw486_irq_many_add (irq, &(isairq -> irq_many));

  return (isairq);
}

/************************************************************************/
/*									*/
/*  Get ISA device's smplock						*/
/*									*/
/*    Input:								*/
/*									*/
/*	isairq = as returned by oz_dev_isa_irq_alloc			*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_dev_isa_irq_smplock = points to corresponding smplock	*/
/*									*/
/************************************************************************/

OZ_Smplock *oz_dev_isa_irq_smplock (OZ_Dev_Isa_Irq *isairq)

{
  OZ_KNL_CHKOBJTYPE (isairq, OZ_OBJTYPE_ISAIRQ);
  return (isairq -> smplock);
}

/************************************************************************/
/*									*/
/*  Set new entry/param for a ISA interrupt				*/
/*									*/
/*    Input:								*/
/*									*/
/*	isairq = as returned by oz_dev_isa_irq_alloc			*/
/*	entry  = entrypoint of routine to call upon interrupt		*/
/*	param  = parameter to pass to 'entry' routine			*/
/*									*/
/************************************************************************/

void oz_dev_isa_irq_reset (OZ_Dev_Isa_Irq *isairq, void (*entry) (void *param, OZ_Mchargs *mchargs), void *param)

{
  uLong ll;

  OZ_KNL_CHKOBJTYPE (isairq, OZ_OBJTYPE_ISAIRQ);

  ll = oz_hw_smplock_wait (isairq -> smplock);
  isairq -> entry = entry;
  isairq -> param = param;
  oz_hw_smplock_clr (isairq -> smplock, ll);
}

/************************************************************************/
/*									*/
/*  Free off an ISA interrupt block					*/
/*									*/
/*    Input:								*/
/*									*/
/*	isairq = as returned by oz_dev_isa_irq_alloc			*/
/*									*/
/*    Output:								*/
/*									*/
/*	isairq no longer valid						*/
/*									*/
/************************************************************************/

void oz_dev_isa_irq_free (OZ_Dev_Isa_Irq *isairq)

{
  OZ_KNL_CHKOBJTYPE (isairq, OZ_OBJTYPE_ISAIRQ);
  oz_hw486_irq_many_rem (isairq -> irq, &(isairq -> irq_many));
  OZ_KNL_NPPFREE (isairq);
}

/************************************************************************/
/*									*/
/*  Internal ISA interrupt wrapper routine				*/
/*									*/
/************************************************************************/

static int isa_interrupt (void *isairqv, OZ_Mchargs *mchargs)

{
  OZ_Dev_Isa_Irq *isairq;

  isairq = isairqv;
  OZ_KNL_CHKOBJTYPE (isairq, OZ_OBJTYPE_ISAIRQ);
  (*(isairq -> entry)) (isairq -> param, mchargs);
  return (0);
}
