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
/*  User mode callable routines for user & job objects			*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_knl_section.h"
#include "oz_knl_status.h"
#include "oz_knl_userjob.h"
#include "oz_sys_syscall.h"
#include "oz_sys_userjob.h"

/************************************************************************/
/*									*/
/*  Create a new job							*/
/*									*/
/*    Input:								*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_job_create = OZ_SUCCESS : successful			*/
/*	                          else : error status			*/
/*	*h_job_r = job handle						*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_2 (job_create, const char *, name, OZ_Handle *, h_job_r)

{
  char jobname[OZ_JOBNAME_MAX];
  int si;
  uLong sts;
  OZ_Handle h_job;
  OZ_Job *job;

  si = oz_hw_cpu_setsoftint (0);				/* inhibit thread deletion */
  sts = oz_knl_section_ugetz (cprocmode, sizeof jobname, name, jobname, NULL);
  if (sts == OZ_SUCCESS) {
    sts = oz_knl_job_create (NULL, jobname, &job);		/* create a job, same as current user */
    if (sts == OZ_SUCCESS) {					/* see if successful */
      sts = oz_knl_handle_assign (job, cprocmode, &h_job);	/* successful, create an handle to the new job */
      oz_knl_job_increfc (job, -1);				/* release job pointer */
      sts = oz_knl_section_uput (cprocmode, sizeof *h_job_r, &h_job, h_job_r);
      if (sts != OZ_SUCCESS) oz_knl_handle_release (h_job, cprocmode);
    }
  }
  oz_hw_cpu_setsoftint (si);					/* restore thread deletion */
  return (sts);							/* return final status */
}
