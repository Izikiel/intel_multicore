//+++2003-12-12
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
//---2003-12-12

/************************************************************************/
/*									*/
/*  This kernel image is used to print out the handle table of a 	*/
/*  process.								*/
/*									*/
/*    Input:								*/
/*									*/
/*	cprocmode = caller's process mode				*/
/*	h_process = target process handle				*/
/*	h_output  = handle of output device				*/
/*	smplevel <= softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_prochandles = OZ_SUCCESS : successful			*/
/*	                           else : error status			*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_knl_devio.h"
#include "oz_knl_event.h"
#include "oz_knl_handle.h"
#include "oz_knl_hw.h"
#include "oz_knl_image.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_logname.h"
#include "oz_knl_objtype.h"
#include "oz_knl_process.h"
#include "oz_knl_section.h"
#include "oz_knl_status.h"
#include "oz_knl_thread.h"
#include "oz_knl_userjob.h"

#define BUFFSIZE 256

uLong _start (OZ_Procmode cprocmode, OZ_Handle h_process, OZ_Handle h_output)

{
  char *buff, *bufp;
  int bufl, si;
  Long *refcounts;
  OZ_Handle *handles, hi, numentries;
  OZ_IO_fs_writerec fs_writerec;
  OZ_Iochan *iochan;
  OZ_Objtype *objtypes;
  OZ_Process *myproc, *process;
  OZ_Procmode *procmodes;
  OZ_Thread **threads;
  OZ_Secaccmsk *secaccmsks;
  uLong sts;
  void *object, **objects;

  memset (&fs_writerec, 0, sizeof fs_writerec);
  si = oz_hw_cpu_setsoftint (0);
  sts = oz_knl_handle_takeout (h_process, cprocmode, OZ_SECACCMSK_WRITE, OZ_OBJTYPE_PROCESS, &process, NULL);
  if (sts == OZ_SUCCESS) {
    sts = oz_knl_handle_takeout (h_output, cprocmode, OZ_SECACCMSK_WRITE, OZ_OBJTYPE_IOCHAN, &iochan, NULL);
    if (sts == OZ_SUCCESS) {
      buff = OZ_KNL_PGPMALLOC (BUFFSIZE);
      fs_writerec.buff = buff;
      myproc = oz_knl_process_getcur ();
      oz_knl_process_setcur (process);
      numentries = oz_knl_handletbl_statsget (&handles, &objects, &objtypes, &threads, &procmodes, &secaccmsks, &refcounts);
      for (hi = 0; hi < numentries; hi ++) {
        object = objects[hi];
        if (object != NULL) {

          /* Format common portion at beginning of line buffer */

          oz_sys_sprintf (12, buff, "%8.8X", handles[hi]);
          buff[ 8] = '?';
          buff[ 9] = ' ';
          buff[10] = ' ';
          if (procmodes[hi] == OZ_PROCMODE_KNL)    buff[ 8] = 'k';
          if (procmodes[hi] == OZ_PROCMODE_USR)    buff[ 8] = 'u';
          if (secaccmsks[hi] & OZ_SECACCMSK_READ)  buff[ 9] = 'r';
          if (secaccmsks[hi] & OZ_SECACCMSK_WRITE) buff[10] = 'w';
          oz_sys_sprintf (BUFFSIZE - 11, buff + 11, " %p  ", object);
          bufl = strlen (buff);
          bufp = buff + bufl;
          bufl = BUFFSIZE - bufl;

          /* Format object-specific portion thereafter */

          switch (objtypes[hi]) {
            case OZ_OBJTYPE_DEVUNIT: {
              oz_sys_sprintf (bufl, bufp, " device: %s\n", oz_knl_devunit_devname (object));
              break;
            }
            case OZ_OBJTYPE_EVENT: {
              oz_sys_sprintf (bufl, bufp, "  event: (%d) %s\n", oz_knl_event_inc (object, 0), oz_knl_event_getname (object));
              break;
            }
            case OZ_OBJTYPE_IMAGE: {
              oz_sys_sprintf (bufl, bufp, "  image: %s\n", oz_knl_image_name (object));
              break;
            }
            case OZ_OBJTYPE_IOCHAN: {
              char filename[128];
              uLong reads, writes;
              OZ_Devunit *devunit;
              OZ_IO_fs_getinfo2 fs_getinfo2;

              devunit = oz_knl_iochan_getdevunit (object);
              reads   = oz_knl_iochan_readcount  (object);
              writes  = oz_knl_iochan_writecount (object);
              memset (&fs_getinfo2, 0, sizeof fs_getinfo2);
              fs_getinfo2.filnamsize = sizeof filename;
              fs_getinfo2.filnambuff = filename;
              sts = oz_knl_io (object, OZ_IO_FS_GETINFO2, sizeof fs_getinfo2, &fs_getinfo2);
              if (sts == OZ_SUCCESS) {
                oz_sys_sprintf (bufl, bufp, " iochan: (%u/%u) %s:%s\n", reads, writes, oz_knl_devunit_devname (devunit), filename);
              } else if (sts == OZ_BADIOFUNC) {
                oz_sys_sprintf (bufl, bufp, " iochan: (%u/%u) %s\n", reads, writes, oz_knl_devunit_devname (devunit));
              } else {
                oz_sys_sprintf (bufl, bufp, " iochan: (%u/%u) %s(%u)\n", reads, writes, oz_knl_devunit_devname (devunit), sts);
              }
              break;
            }
            case OZ_OBJTYPE_JOB: {
              oz_sys_sprintf (bufl, bufp, "    job: %s\n", oz_knl_job_getname (object));
              break;
            }
            case OZ_OBJTYPE_LOGNAME: {
              oz_sys_sprintf (bufl, bufp, "logname: %s\n", oz_knl_logname_getname (object));
              break;
            }
            case OZ_OBJTYPE_PROCESS: {
              oz_sys_sprintf (bufl, bufp, "process: (%u) %s\n", oz_knl_process_getid (object), oz_knl_process_getname (object));
              break;
            }
            case OZ_OBJTYPE_SECTION: {
              oz_sys_sprintf (bufl, bufp, "section\n");
              break;
            }
            case OZ_OBJTYPE_THREAD: {
              oz_sys_sprintf (bufl, bufp, " thread: (%u) %s\n", oz_knl_thread_getid (object), oz_knl_thread_getname (object));
              break;
            }
            case OZ_OBJTYPE_USER: {
              oz_sys_sprintf (bufl, bufp, "   user: %s\n", oz_knl_user_getname (object));
              break;
            }
            default: {
              oz_sys_sprintf (bufl, bufp, "%d\n", objtypes[hi]);
              break;
            }
          }

          /* Write result to output */

          fs_writerec.size = strlen (buff);
          sts = oz_knl_io (iochan, OZ_IO_FS_WRITEREC, sizeof fs_writerec, &fs_writerec);
          if (sts != OZ_SUCCESS) break;
        }
      }
      oz_knl_handletbl_statsfree (numentries, handles, objects, objtypes, threads, procmodes, secaccmsks, refcounts);
      oz_knl_process_setcur (myproc);
      OZ_KNL_PGPFREE (buff);
      oz_knl_handle_putback (h_output);
    }
    oz_knl_handle_putback (h_process);
  }
  oz_hw_cpu_setsoftint (si);
  return (sts);
}
