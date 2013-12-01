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
/*  This utility does magtape control commands				*/
/*									*/
/*	tape load <tape_device_name>					*/
/*	     unload <tape_device_name>					*/
/*	     rewind <tape_device_name>					*/
/*	     skipblocks <tape_device_name> <[+/-]count>			*/
/*	     skipfiles <tape_device_name> <[+/-]count>			*/
/*	     writemark <tape_device_name>				*/
/*	     getposition <tape_device_name>				*/
/*	     setposition <tape_device_name> <position>			*/
/*	     status <tape_device_name>					*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_crtl_malloc.h"
#include "oz_io_tape.h"
#include "oz_knl_devio.h"
#include "oz_knl_lock.h"
#include "oz_knl_status.h"
#include "oz_sys_handle.h"
#include "oz_sys_io.h"
#include "oz_util_start.h"

static char *pn = "tape";
static OZ_Handle h_iochan;

static uLong cmd_getposition (char *argv[]);
static uLong cmd_load (char *argv[]);
static uLong cmd_rewind (char *argv[]);
static uLong cmd_setposition (char *argv[]);
static uLong cmd_skipblocks (char *argv[]);
static uLong cmd_skipfiles (char *argv[]);
static uLong cmd_status (char *argv[]);
static uLong cmd_unload (char *argv[]);
static uLong cmd_writemark (char *argv[]);

static const struct { const char *name;
                      int argc;
                      uLong (*entry) (char *argv[]);
                      const char *help;
                    } cmdtbl[] = {
                      "getposition", 0, cmd_getposition, "", 
                      "load",        0, cmd_load,        "", 
                      "rewind",      0, cmd_rewind,      "", 
                      "setposition", 1, cmd_setposition, "<position>", 
                      "skipblocks",  1, cmd_skipblocks,  "<[+/-]count>", 
                      "skipfiles",   1, cmd_skipfiles,   "<[+/-]count>", 
                      "status",      0, cmd_status,      "", 
                      "unload",      0, cmd_unload,      "", 
                      "writemark",   0, cmd_writemark,   "", 
                               NULL, 0, NULL };

static int twohexdigits (char *ascii);

uLong oz_util_main (int argc, char *argv[])

{
  int i;
  uLong sts;

  if (argc > 0) pn = argv[0];

  if (argc > 2) {
    for (i = 0; cmdtbl[i].name != NULL; i ++) {
      if ((strcasecmp (argv[1], cmdtbl[i].name) == 0) && (argc - 2 == cmdtbl[i].argc)) {
        sts = oz_sys_io_assign (OZ_PROCMODE_KNL, &h_iochan, argv[2], OZ_LOCKMODE_EX);
        if (sts != OZ_SUCCESS) oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u assigning channel to %s\n", pn, sts, argv[2]);
        else sts = (*(cmdtbl[i].entry)) (argv + 2);
        return (sts);
      }
    }
  }

  oz_sys_io_fs_printf (oz_util_h_error, "%s: missing or invalid sub-command, usage:\n", pn);
  for (i = 0; cmdtbl[i].name != NULL; i ++) {
    oz_sys_io_fs_printf (oz_util_h_error, "  %s %s <tape_device> %s\n", pn, cmdtbl[i].name, cmdtbl[i].help);
  }
  return (OZ_UNKNOWNCOMMAND);
}

static uLong cmd_getposition (char *argv[])

{
  int i, v;
  uByte *position;
  uLong sts;
  OZ_IO_tape_getinfo1 tape_getinfo1;
  OZ_IO_tape_getpos tape_getpos;

  memset (&tape_getinfo1, 0, sizeof tape_getinfo1);
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_iochan, 0, OZ_IO_TAPE_GETINFO1, sizeof tape_getinfo1, &tape_getinfo1);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u getting tape info\n", pn, sts);
    return (sts);
  }

  if (tape_getinfo1.tappossiz == 0) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: tape drive does not support positioning\n", pn);
    return (OZ_BADIOFUNC);
  }

  position = malloc (tape_getinfo1.tappossiz);

  memset (&tape_getpos, 0, sizeof tape_getpos);
  tape_getpos.tappossiz = tape_getinfo1.tappossiz;
  tape_getpos.tapposbuf = position;
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_iochan, 0, OZ_IO_TAPE_GETPOS, sizeof tape_getpos, &tape_getpos);
  if (sts != OZ_SUCCESS) oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u setting tape position\n", pn, sts);
  else {
    for (i = 0; i < tape_getinfo1.tappossiz; i ++) {
      oz_sys_io_fs_printf (oz_util_h_output, "%2.2X", position[i]);
    }
    oz_sys_io_fs_printf (oz_util_h_output, "\n");
  }
  return (sts);
}

static uLong cmd_load (char *argv[])

{
  uLong sts;
  OZ_IO_tape_setvolvalid tape_setvolvalid;

  memset (&tape_setvolvalid, 0, sizeof tape_setvolvalid);
  tape_setvolvalid.valid = 1;
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_iochan, 0, OZ_IO_TAPE_SETVOLVALID, sizeof tape_setvolvalid, &tape_setvolvalid);
  if (sts != OZ_SUCCESS) oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u loading tape\n", pn, sts);
  return (sts);
}

static uLong cmd_rewind (char *argv[])

{
  uLong sts;

  sts = oz_sys_io (OZ_PROCMODE_KNL, h_iochan, 0, OZ_IO_TAPE_REWIND, 0, NULL);
  if (sts != OZ_SUCCESS) oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u rewinding tape\n", pn, sts);
  return (sts);
}

static uLong cmd_setposition (char *argv[])

{
  int i, v;
  uByte *position;
  uLong sts;
  OZ_IO_tape_getinfo1 tape_getinfo1;
  OZ_IO_tape_setpos tape_setpos;

  memset (&tape_getinfo1, 0, sizeof tape_getinfo1);
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_iochan, 0, OZ_IO_TAPE_GETINFO1, sizeof tape_getinfo1, &tape_getinfo1);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u getting tape info\n", pn, sts);
    return (sts);
  }

  if (tape_getinfo1.tappossiz == 0) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: tape drive does not support positioning\n", pn);
    return (OZ_BADIOFUNC);
  }

  position = malloc (tape_getinfo1.tappossiz);
  for (i = 0; i < tape_getinfo1.tappossiz; i ++) {
    v = twohexdigits (argv[0] + i * 2);
    if (v < 0) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: bad tape position string %s, must be %u hex digits\n", pn, argv[0], tape_getinfo1.tappossiz * 2);
      return (OZ_BADPARAM);
    }
    position[i] = v;
  }

  memset (&tape_setpos, 0, sizeof tape_setpos);
  tape_setpos.tappossiz = tape_getinfo1.tappossiz;
  tape_setpos.tapposbuf = position;
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_iochan, 0, OZ_IO_TAPE_SETPOS, sizeof tape_setpos, &tape_setpos);
  if (sts != OZ_SUCCESS) oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u setting tape position\n", pn, sts);
  return (sts);
}

static uLong cmd_skipblocks (char *argv[])

{
  char *p;
  int skipped;
  uLong sts;
  OZ_IO_tape_skip tape_skip;

  memset (&tape_skip, 0, sizeof tape_skip);
  tape_skip.skipped = &skipped;
  tape_skip.count   = strtol (argv[0], 0, &p);
  if (*p != 0) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: bad block count %s\n", pn, argv[0]);
    return (OZ_BADPARAM);
  }
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_iochan, 0, OZ_IO_TAPE_SKIP, sizeof tape_skip, &tape_skip);
  if (sts != OZ_SUCCESS) oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u skipping tape blocks\n", pn, sts);
  else oz_sys_io_fs_printf (oz_util_h_output, "%d\n", skipped);
  return (sts);
}

static uLong cmd_skipfiles (char *argv[])

{
  char *p;
  int skipped;
  uLong sts;
  OZ_IO_tape_skip tape_skip;

  memset (&tape_skip, 0, sizeof tape_skip);
  tape_skip.skipped = &skipped;
  tape_skip.files   = 1;
  tape_skip.count   = strtol (argv[0], 0, &p);
  if (*p != 0) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: bad file count %s\n", pn, argv[0]);
    return (OZ_BADPARAM);
  }
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_iochan, 0, OZ_IO_TAPE_SKIP, sizeof tape_skip, &tape_skip);
  if (sts != OZ_SUCCESS) oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u skipping tape files\n", pn, sts);
  else oz_sys_io_fs_printf (oz_util_h_output, "%d\n", skipped);
  return (sts);
}

static uLong cmd_status (char *argv[])

{
  char flags[32];
  uLong sts;
  OZ_IO_tape_getinfo1 tape_getinfo1;

  memset (&tape_getinfo1, 0, sizeof tape_getinfo1);
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_iochan, 0, OZ_IO_TAPE_GETINFO1, sizeof tape_getinfo1, &tape_getinfo1);
  if (sts != OZ_SUCCESS) oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u getting tape status\n", pn, sts);
  else {
    flags[0] = 0;
    if (tape_getinfo1.tapeflags & OZ_IO_TAPE_FLAG_INEOT) strcat (flags, " INEOT");
    if (tape_getinfo1.tapeflags & OZ_IO_TAPE_FLAG_ATBOT) strcat (flags, " ATBOT");
    if (tape_getinfo1.tapeflags & OZ_IO_TAPE_FLAG_ATEOD) strcat (flags, " ATEOD");
    if (flags[0] == 0) strcpy (flags, " (none)");
    oz_sys_io_fs_printf (oz_util_h_output, "         gapno: %u\n", tape_getinfo1.gapno);
    oz_sys_io_fs_printf (oz_util_h_output, "        fileno: %u\n", tape_getinfo1.fileno);
    oz_sys_io_fs_printf (oz_util_h_output, "         flags:%s\n", flags);
    oz_sys_io_fs_printf (oz_util_h_output, "     blocksize: %u..%u\n", tape_getinfo1.minblocksize, tape_getinfo1.maxblocksize);
    oz_sys_io_fs_printf (oz_util_h_output, "      capacity: %u<<%u\n", tape_getinfo1.tapcapman, tape_getinfo1.tapcapexp);
    oz_sys_io_fs_printf (oz_util_h_output, "  positionsize: %u\n", tape_getinfo1.tappossiz);
  }
  return (sts);
}

static uLong cmd_unload (char *argv[])

{
  uLong sts;
  OZ_IO_tape_setvolvalid tape_setvolvalid;

  memset (&tape_setvolvalid, 0, sizeof tape_setvolvalid);
  tape_setvolvalid.eject = 1;
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_iochan, 0, OZ_IO_TAPE_SETVOLVALID, sizeof tape_setvolvalid, &tape_setvolvalid);
  if (sts != OZ_SUCCESS) oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u unloading tape\n", pn, sts);
  return (sts);
}

static uLong cmd_writemark (char *argv[])

{
  uLong sts;
  OZ_IO_tape_writemark tape_writemark;

  memset (&tape_writemark, 0, sizeof tape_writemark);
  tape_writemark.endoftape = 0;
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_iochan, 0, OZ_IO_TAPE_WRITEMARK, sizeof tape_writemark, &tape_writemark);
  if (sts != OZ_SUCCESS) oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u marking tape\n", pn, sts);
  return (sts);
}

static int twohexdigits (char *ascii)

{
  int v;

       if ((ascii[0] >= '0') && (ascii[0] <= '9')) v  = (ascii[0] - '0') << 4;
  else if ((ascii[0] >= 'A') && (ascii[0] <= 'F')) v  = (ascii[0] - 'A' + 10) << 4;
  else if ((ascii[0] >= 'a') && (ascii[0] <= 'f')) v  = (ascii[0] - 'a' + 10) << 4;
  else return (-1);

       if ((ascii[1] >= '0') && (ascii[1] <= '9')) v += (ascii[1] - '0');
  else if ((ascii[1] >= 'A') && (ascii[1] <= 'F')) v += (ascii[1] - 'A' + 10);
  else if ((ascii[1] >= 'a') && (ascii[1] <= 'f')) v += (ascii[1] - 'a' + 10);
  else return (-1);

  return (v);
}
