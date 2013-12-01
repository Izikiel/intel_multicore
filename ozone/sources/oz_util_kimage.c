//+++2003-03-01
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
//---2003-03-01

/************************************************************************/
/*									*/
/*  This utility loads/unloads kernel images				*/
/*
/************************************************************************/

#include "ozone.h"
#include "oz_knl_image.h"
#include "oz_knl_status.h"
#include "oz_sys_callknl.h"
#include "oz_sys_io.h"
#include "oz_sys_io_fs.h"
#include "oz_util_start.h"

static char *pn;

static uLong kimage_load (int argc, char *argv[]);

uLong oz_util_main (int argc, char *argv[])

{
  pn = "kimage";
  if (argc > 0) pn = argv[0];

  if (argc > 1) {
    if (strcasecmp (argv[1], "load") == 0) return (kimage_load (argc, argv));
  }

  oz_sys_io_fs_printf (oz_util_h_error, "%s: missing or invalid sub-command\n", pn);
  oz_sys_io_fs_printf (oz_util_h_error, "usage: %s load <imagename>\n", pn);
  return (OZ_UNKNOWNCOMMAND);
}


typedef struct { int argc;
                 char **argv;
                 char *imagename;
                 void *baseaddr;
                 void *startaddr;
               } Kilpb;

static uLong kimage_load_knl (OZ_Procmode cprocmode, void *kilpbv);

static uLong kimage_load (int argc, char *argv[])

{
  Kilpb kilpb;
  uLong sts;

  if (argc < 3) {
    oz_sys_io_fs_printf (oz_util_h_error, "usage: %s load <imagename> [<args...>]\n", pn);
    return (OZ_MISSINGPARAM);
  }

  kilpb.argc = argc - 2;
  kilpb.argv = argv + 2;
  kilpb.imagename = argv[2];

  sts = oz_sys_callknl (kimage_load_knl, &kilpb);
  if (sts != OZ_SUCCESS) oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u loading image %s\n", pn, sts, argv[2]);
  else oz_sys_io_fs_printf (oz_util_h_error, "%s: image %s loaded at %p\n", pn, argv[2], kilpb.baseaddr);

  return (sts);
}

static uLong kimage_load_knl (OZ_Procmode cprocmode, void *kilpbv)

{
  int si;
  Kilpb *kilpb;
  OZ_Image *image;
  uLong (*startaddr) (int argc, char **argv, OZ_Image *image);
  uLong sts;

  kilpb = kilpbv;

  si  = oz_hw_cpu_setsoftint (0);
  sts = oz_knl_image_load (OZ_PROCMODE_KNL, kilpb -> imagename, 1, 0, &(kilpb -> baseaddr), &(kilpb -> startaddr), &image);
  if (sts == OZ_SUCCESS) {
    startaddr = kilpb -> startaddr;				// get start address
    oz_sys_io_fs_printerror ("oz_util_kimage: %s loaded at %p, startaddr %p\n", kilpb -> imagename, kilpb -> baseaddr, startaddr);
    if (startaddr != NULL) {
      sts = (*startaddr) (kilpb -> argc, kilpb -> argv, image);	// call the image so it can start whatever it needs to start
      oz_knl_image_increfc (image, -1);				// this unloads it unless the start routine incd the image refcount
    }
  }
  oz_hw_cpu_setsoftint (si);
  return (sts);
}
