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
/*  Start quantum timer for thread on current cpu			*/
/*									*/
/*    Input:								*/
/*									*/
/*	quantum = delta time from now to call oz_knl_thread_quantimex	*/
/*	iotanow = the current iota time					*/
/*									*/
/*    Note:								*/
/*									*/
/*	This is a generic implementation using the system timer queue 	*/
/*	for cpu's that don't have an on-chip timer			*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_dev_timer.h"
#include "oz_knl_hw.h"
#include "oz_knl_thread.h"

static OZ_Timer *timers[OZ_HW_MAXCPUS];

static void quantimex (void *timerpv, OZ_Timer *timer);

void oz_hw_quantimer_start (OZ_Iotatime quantum, OZ_Iotatime iotanow)

{
  OZ_Datebin when;
  OZ_Timer **timerp;

#if 0000
  timerp = timers + oz_hw_cpu_getcur ();
  if (*timerp == NULL) *timerp = oz_knl_timer_alloc ();
  quantum += iotanow;
  when = oz_hw_tod_aiota2sys (quantum);
  oz_knl_timer_insert (*timerp, when, quantimex, timerp);
#endif
}

static void quantimex (void *timerpv, OZ_Timer *timer)

{
  oz_knl_thread_quantimex (((OZ_Timer **)timerpv) - timers);
}
