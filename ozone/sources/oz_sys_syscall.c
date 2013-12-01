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
/*  Initialize system call table					*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_knl_hw.h"
#include "oz_knl_sdata.h"
#include "oz_knl_status.h"

#include "oz_sys_syscall.h"

#include "oz_sys_callknl.h"
#include "oz_sys_event.h"
#include "oz_sys_exhand.h"
#include "oz_sys_handle.h"
#include "oz_sys_image.h"
#include "oz_sys_io.h"
#include "oz_sys_logname.h"
#include "oz_sys_misc.h"
#include "oz_sys_password.h"
#include "oz_sys_process.h"
#include "oz_sys_section.h"
#include "oz_sys_spawn.h"
#include "oz_sys_thread.h"
#include "oz_sys_tzconv.h"
#include "oz_sys_userjob.h"

static uLong obsolete ()

{
  return (OZ_INVSYSCALL);
}

void oz_knl_syscall_init (void)

{
  oz_s_syscalltbl[OZ_SYSCALL_thread_deqast]           = oz_syscall_thread_deqast;
  oz_s_syscalltbl[OZ_SYSCALL_thread_setast]           = oz_syscall_thread_setast;
  oz_s_syscalltbl[OZ_SYSCALL_thread_exiteh]           = oz_syscall_thread_exiteh;
  oz_s_syscalltbl[OZ_SYSCALL_event_create]            = oz_syscall_event_create;
  oz_s_syscalltbl[OZ_SYSCALL_event_inc]               = oz_syscall_event_inc;
  oz_s_syscalltbl[OZ_SYSCALL_event_set]               = oz_syscall_event_set;
  oz_s_syscalltbl[OZ_SYSCALL_event_wait]              = oz_syscall_event_wait;
  oz_s_syscalltbl[OZ_SYSCALL_event_nwait]             = oz_syscall_event_nwait;
  oz_s_syscalltbl[OZ_SYSCALL_io_assign]               = oz_syscall_io_assign;
  oz_s_syscalltbl[OZ_SYSCALL_io_abort]                = oz_syscall_io_abort;
  oz_s_syscalltbl[OZ_SYSCALL_io_start]                = oz_syscall_io_start;
  oz_s_syscalltbl[OZ_SYSCALL_process_create]          = oz_syscall_process_create;
  oz_s_syscalltbl[OZ_SYSCALL_process_mapsection]      = oz_syscall_process_mapsection;
  oz_s_syscalltbl[OZ_SYSCALL_handle_release]          = oz_syscall_handle_release;
  oz_s_syscalltbl[OZ_SYSCALL_section_create]          = oz_syscall_section_create;
  oz_s_syscalltbl[OZ_SYSCALL_thread_create]           = oz_syscall_thread_create;
  oz_s_syscalltbl[OZ_SYSCALL_image_load]              = oz_syscall_image_load;
  oz_s_syscalltbl[OZ_SYSCALL_spawn]                   = oz_syscall_spawn;
  oz_s_syscalltbl[OZ_SYSCALL_logname_create]          = oz_syscall_logname_create;
  oz_s_syscalltbl[OZ_SYSCALL_callknl]                 = oz_syscall_callknl;
  oz_s_syscalltbl[OZ_SYSCALL_logname_getattr]         = oz_syscall_logname_getattr;
  oz_s_syscalltbl[OZ_SYSCALL_event_ast]               = oz_syscall_event_ast;
  oz_s_syscalltbl[OZ_SYSCALL_iochan_getunitname]      = oz_syscall_iochan_getunitname;
  oz_s_syscalltbl[OZ_SYSCALL_iochan_getclassname]     = oz_syscall_iochan_getclassname;
  oz_s_syscalltbl[OZ_SYSCALL_process_getid]           = oz_syscall_process_getid;
  oz_s_syscalltbl[OZ_SYSCALL_logname_lookup]          = oz_syscall_logname_lookup;
  oz_s_syscalltbl[OZ_SYSCALL_logname_getval]          = oz_syscall_logname_getval;
  oz_s_syscalltbl[OZ_SYSCALL_thread_abort]            = oz_syscall_thread_abort;
  oz_s_syscalltbl[OZ_SYSCALL_thread_getexitsts]       = oz_syscall_thread_getexitsts;
  oz_s_syscalltbl[OZ_SYSCALL_logname_gettblent]       = oz_syscall_logname_gettblent;
  oz_s_syscalltbl[OZ_SYSCALL_logname_delete]          = oz_syscall_logname_delete;
  oz_s_syscalltbl[OZ_SYSCALL_thread_getexitevent]     = oz_syscall_thread_getexitevent;
  oz_s_syscalltbl[OZ_SYSCALL_thread_suspend]          = oz_syscall_thread_suspend;
  oz_s_syscalltbl[OZ_SYSCALL_thread_resume]           = oz_syscall_thread_resume;
  oz_s_syscalltbl[OZ_SYSCALL_thread_getname]          = oz_syscall_thread_getname;
  oz_s_syscalltbl[OZ_SYSCALL_handle_getinfo]          = oz_syscall_handle_getinfo;
  oz_s_syscalltbl[OZ_SYSCALL_process_unmapsec]        = oz_syscall_process_unmapsec;
  oz_s_syscalltbl[OZ_SYSCALL_handle_next]             = oz_syscall_handle_next;
  oz_s_syscalltbl[OZ_SYSCALL_gettimezonex]            = obsolete;
  oz_s_syscalltbl[OZ_SYSCALL_section_setpageprot]     = oz_syscall_section_setpageprot;
  oz_s_syscalltbl[OZ_SYSCALL_ast]                     = obsolete;
  oz_s_syscalltbl[OZ_SYSCALL_exhand_create]           = oz_syscall_exhand_create;
  oz_s_syscalltbl[OZ_SYSCALL_thread_getbyid]          = oz_syscall_thread_getbyid;
  oz_s_syscalltbl[OZ_SYSCALL_iosel_start]             = oz_syscall_iosel_start;
  oz_s_syscalltbl[OZ_SYSCALL_thread_setseckeys]       = oz_syscall_thread_setseckeys;
  oz_s_syscalltbl[OZ_SYSCALL_thread_setdefcresecattr] = oz_syscall_thread_setdefcresecattr;
  oz_s_syscalltbl[OZ_SYSCALL_thread_setsecattr]       = oz_syscall_thread_setsecattr;
  oz_s_syscalltbl[OZ_SYSCALL_password_change]         = oz_syscall_password_change;
  oz_s_syscalltbl[OZ_SYSCALL_thread_setbasepri]       = oz_syscall_thread_setbasepri;
  oz_s_syscalltbl[OZ_SYSCALL_handle_setthread]        = oz_syscall_handle_setthread;
  oz_s_syscalltbl[OZ_SYSCALL_thread_queueast]         = oz_syscall_thread_queueast;
  oz_s_syscalltbl[OZ_SYSCALL_process_makecopy]        = oz_syscall_process_makecopy;
  oz_s_syscalltbl[OZ_SYSCALL_thread_orphan]           = oz_syscall_thread_orphan;
  oz_s_syscalltbl[OZ_SYSCALL_io_alloc]                = oz_syscall_io_alloc;
  oz_s_syscalltbl[OZ_SYSCALL_io_realloc]              = oz_syscall_io_realloc;
  oz_s_syscalltbl[OZ_SYSCALL_io_dealloc]              = oz_syscall_io_dealloc;
  oz_s_syscalltbl[OZ_SYSCALL_job_create]              = oz_syscall_job_create;
  oz_s_syscalltbl[OZ_SYSCALL_event_setimint]          = oz_syscall_event_setimint;
  oz_s_syscalltbl[OZ_SYSCALL_process_getbyid]         = oz_syscall_process_getbyid;
  oz_s_syscalltbl[OZ_SYSCALL_process_mapsections]     = oz_syscall_process_mapsections;
  oz_s_syscalltbl[OZ_SYSCALL_io_chancopy]             = oz_syscall_io_chancopy;
  oz_s_syscalltbl[OZ_SYSCALL_tzconv]                  = oz_syscall_tzconv;
  oz_s_syscalltbl[OZ_SYSCALL_io_wait]                 = oz_syscall_io_wait;
  oz_s_syscalltbl[OZ_SYSCALL_io_waitagain]            = oz_syscall_io_waitagain;
  oz_s_syscalltbl[OZ_SYSCALL_io_waitsetef]            = oz_syscall_io_waitsetef;
  oz_s_syscalltbl[OZ_SYSCALL_handle_retrieve]         = oz_syscall_handle_retrieve;

  oz_s_syscallmax = OZ_SYSCALL_MAX;
}
