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
/*  This utility does special things with a scsi controller		*/
/*									*/
/*    Send a scsi command:						*/
/*	scsi doio <scsi_device_name> <scsi_target_id> ...		*/
/*									*/
/*    Get controller info:						*/
/*	scsi getinfo <scsi_device_name> [<scsi_target_id>]		*/
/*									*/
/*    Reset the bus:							*/
/*	scsi reset <scsi_device_name>					*/
/*									*/
/*    Scan the bus for new devices:					*/
/*	scsi scan <scsi_device_name>					*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_crtl_malloc.h"
#include "oz_dev_scsi.h"
#include "oz_io_scsi.h"
#include "oz_knl_devio.h"
#include "oz_knl_handle.h"
#include "oz_knl_lock.h"
#include "oz_knl_status.h"
#include "oz_sys_handle.h"
#include "oz_sys_io.h"
#include "oz_util_start.h"

static char *pn = "scsi";
static OZ_Handle h_iochan;

static uLong scsidoio (int argc, char *argv[]);
static int getbytebuff (char *arg, uLong *size_r, uByte **buff_r);
static uLong scsigetinfo (int argc, char *argv[]);
static uLong do_getinfo1 (void *giv);
static uLong scsireset (int argc, char *argv[]);
static uLong scsiscan (int argc, char *argv[]);
static uLong scsiscanknl (OZ_Procmode cprocmode, void *dummy);

uLong oz_util_main (int argc, char *argv[])

{
  if (argc > 0) pn = argv[0];

  if (argc > 1) {
    if (strcasecmp (argv[1], "doio")    == 0) return (scsidoio    (argc, argv));
    if (strcasecmp (argv[1], "getinfo") == 0) return (scsigetinfo (argc, argv));
    if (strcasecmp (argv[1], "reset")   == 0) return (scsireset   (argc, argv));
    if (strcasecmp (argv[1], "scan")    == 0) return (scsiscan    (argc, argv));
  }

  oz_sys_io_fs_printf (oz_util_h_error, "%s: missing or invalid sub-command\n", pn);
  oz_sys_io_fs_printf (oz_util_h_error, "usage: %s doio ...    # send a scsi command to a device\n", pn);
  oz_sys_io_fs_printf (oz_util_h_error, "       %s getinfo ... # get scsi controller info\n", pn);
  oz_sys_io_fs_printf (oz_util_h_error, "       %s reset ...   # reset scsi bus\n", pn);
  oz_sys_io_fs_printf (oz_util_h_error, "       %s scan ...    # scan scsi bus for new devices\n", pn);
  return (OZ_UNKNOWNCOMMAND);
}

static uLong scsidoio (int argc, char *argv[])

{
  char c, *device, *ermsg, *p;
  int disconnect, i, negosynch, negowidth, scsi_id, timeout, write;
  OZ_IO_scsi_doio scsi_doio;
  OZ_IO_scsi_open scsi_open;
  uByte *cbuff, *dbuff, status;
  uLong csize, drlen, dsize, sts;

  csize      = 0;
  device     = NULL;
  disconnect = 0;
  dsize      = 0;
  negosynch  = 0;
  negowidth  = 0;
  scsi_id    = -1;
  timeout    = 0;
  write      = 0;

  for (i = 2; i < argc; i ++) {
    if (strcmp (argv[i], "-disconnect") == 0) {
      disconnect = 1;
      continue;
    }
    if (strcmp (argv[i], "-negosynch") == 0) {
      negosynch = 1;
      continue;
    }
    if (strcmp (argv[i], "-negowidth") == 0) {
      negowidth = 1;
      continue;
    }
    if (strcmp (argv[i], "-read") == 0) {
      ermsg = "missing read data size";
      if (++ i >= argc) goto usage;
      dsize = atoi (argv[i]);
      dbuff = malloc (dsize);
      continue;
    }
    if (strcmp (argv[i], "-timeout") == 0) {
      ermsg = "missing timeout milliseconds";
      if (++ i >= argc) goto usage;
      timeout = atoi (argv[i]);
      continue;
    }
    if (strcmp (argv[i], "-write") == 0) {
      ermsg = "missing write data";
      if (++ i >= argc) goto usage;
      write = 1;
      ermsg = "bad write data";
      if (!getbytebuff (argv[i], &dsize, &dbuff)) goto usage;
      continue;
    }
    ermsg = "unknown option";
    if (argv[i][0] == '-') goto usage;
    if (device == NULL) {
      device = argv[i];
      continue;
    }
    if (scsi_id == -1) {
      scsi_id = strtoul (argv[i], &p, 0);
      if (*p == 0) continue;			/* see if it converted ok */
      scsi_id = -2;				/* if not, assume the scsi_id is omitted */
						/* ... and try to process as the command bytes */
    }
    if (csize == 0) {
      ermsg = "bad command bytes";
      if (!getbytebuff (argv[i], &csize, &cbuff)) goto usage;
      continue;
    }
    ermsg = "extra parameters";
    goto usage;
  }
  ermsg = "missing scsi device name";
  if (device == NULL) goto usage;
  ermsg = "missing scsi_id number";
  if (scsi_id == -1) goto usage;
  ermsg = "missing command bytes";
  if (csize == 0) goto usage;

  /* Assign an I/O channel to the device */

  sts = oz_sys_io_assign (OZ_PROCMODE_KNL, &h_iochan, device, OZ_LOCKMODE_CW);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u assigning channel to %s\n", pn, sts, device);
    return (sts);
  }

  /* If scsi_id was given, open it on the channel.  No scsi_id should be used when accessing a device via class driver. */

  if (scsi_id != -2) {
    memset (&scsi_open, 0, sizeof scsi_open);
    scsi_open.scsi_id  = scsi_id;
    scsi_open.lockmode = OZ_LOCKMODE_CW;

    sts = oz_sys_io (OZ_PROCMODE_KNL, h_iochan, 0, OZ_IO_SCSI_OPEN, sizeof scsi_open, &scsi_open);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u opening scsi id %u\n", pn, sts, scsi_id);
      return (sts);
    }
  }

  /* Now perform the requested I/O */

  memset (&scsi_doio, 0, sizeof scsi_doio);
  scsi_doio.cmdlen   = csize;
  scsi_doio.cmdbuf   = cbuff;
  scsi_doio.datasize = dsize;
  scsi_doio.databuff = dbuff;
  if (disconnect) scsi_doio.optflags |= OZ_IO_SCSI_OPTFLAG_DISCONNECT;
  if (negosynch)  scsi_doio.optflags |= OZ_IO_SCSI_OPTFLAG_NEGO_SYNCH;
  if (negowidth)  scsi_doio.optflags |= OZ_IO_SCSI_OPTFLAG_NEGO_WIDTH;
  if (write)      scsi_doio.optflags |= OZ_IO_SCSI_OPTFLAG_WRITE;
  scsi_doio.status   = &status;
  scsi_doio.datarlen = &drlen;

  sts = oz_sys_io (OZ_PROCMODE_KNL, h_iochan, 0, OZ_IO_SCSI_DOIO, sizeof scsi_doio, &scsi_doio);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u doing scsi command\n", pn, sts);
    return (sts);
  }

  /* Print out the results */

  oz_sys_io_fs_printf (oz_util_h_output, "status byte %2.2x\n", sts);
  if (dsize > 0) {
    oz_sys_io_fs_printf (oz_util_h_output, "data rlen %u (%8.8x)\n", drlen, drlen);
    if (!write) {
      for (dsize = 0; dsize < drlen; dsize += 16) {
        for (csize = 16; csize > 0;) {
          -- csize;
          if (dsize + csize >= drlen) oz_sys_io_fs_printf (oz_util_h_output, "   ");
          else oz_sys_io_fs_printf (oz_util_h_output, " %2.2x", dbuff[dsize+csize]);
        }
        oz_sys_io_fs_printf (oz_util_h_output, " :%4.4x: '", dsize);
        for (csize = 0; (csize < 16) && (dsize + csize < drlen); csize ++) {
          c = dbuff[dsize+csize];
          if ((c < ' ') || (c >= 127)) c = '.';
          oz_sys_io_fs_printf (oz_util_h_output, "%c", c);
        }
        oz_sys_io_fs_printf (oz_util_h_output, "'\n");
      }
    }
  }

  return (sts);

  /* Something missing from command line, output usage message */

usage:
  oz_sys_io_fs_printf (oz_util_h_error, "%s: %s '%s'\n", pn, ermsg, ((i >= argc) || (argv[i] == NULL)) ? "" : argv[i]);
  oz_sys_io_fs_printf (oz_util_h_error, "usage: %s doio <device_name> [<scsi_id>] <command_bytes-xx-xx-...>\n", pn);
  oz_sys_io_fs_printf (oz_util_h_error, "	[-disconnect]\n");
  oz_sys_io_fs_printf (oz_util_h_error, "	[-negosynch]\n");
  oz_sys_io_fs_printf (oz_util_h_error, "	[-negowidth]\n");
  oz_sys_io_fs_printf (oz_util_h_error, "	[-read <nbytes>]\n");
  oz_sys_io_fs_printf (oz_util_h_error, "	[-timeout <milliseconds>]\n");
  oz_sys_io_fs_printf (oz_util_h_error, "	[-write <data_bytes-xx-xx-...>]\n");
  return (OZ_MISSINGPARAM);
}

/* Get a data byte string from command line arg */

static int getbytebuff (char *arg, uLong *size_r, uByte **buff_r)

{
  uByte *buff;
  uLong size, valu;

  *buff_r = buff = malloc (strlen (arg));
  size = 0;
  while (*arg != 0) {
    valu = strtoul (arg, &arg, 16);
    if (valu > 0xFF) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s doio: bad byte value %x\n", pn, valu);
      return (0);
    }
    if (*arg != 0) {
      if (*arg != '-') {
        oz_sys_io_fs_printf (oz_util_h_error, "%s doio: bad byte char %s\n", pn, arg);
        return (0);
      }
      arg ++;
    }
    buff[size++] = valu;
  }
  *size_r = size;
  return (1);
}

static uLong scsigetinfo (int argc, char *argv[])

{
  char c, *device, *ermsg, *p;
  int i, scsi_id;
  OZ_IO_scsi_getinfo1 scsi_getinfo1;
  OZ_IO_scsi_open scsi_open;
  uByte *cbuff, *dbuff, speed, status;
  uLong csize, drlen, dsize, sts;

  static const uByte speed_table[] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 40, 33, 20 };

  csize     = 0;
  device    = NULL;
  dsize     = 0;
  scsi_id   = -1;

  for (i = 2; i < argc; i ++) {
    ermsg = "unknown option";
    if (argv[i][0] == '-') goto usage;
    if (device == NULL) {
      device = argv[i];
      continue;
    }
    if (scsi_id == -1) {
      scsi_id = atoi (argv[i]);
      continue;
    }
    ermsg = "extra parameters";
    goto usage;
  }
  ermsg = "missing scsi device name";
  if (device == NULL) goto usage;

  /* Assign an I/O channel to the device */

  sts = oz_sys_io_assign (OZ_PROCMODE_KNL, &h_iochan, device, OZ_LOCKMODE_NL);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u assigning channel to %s\n", pn, sts, device);
    return (sts);
  }

  /* If scsi_id was given, open it on the channel.  No scsi_id should be used when accessing a device via class driver. */

  if (scsi_id != -1) {
    memset (&scsi_open, 0, sizeof scsi_open);
    scsi_open.scsi_id  = scsi_id;
    scsi_open.lockmode = OZ_LOCKMODE_NL;

    sts = oz_sys_io (OZ_PROCMODE_KNL, h_iochan, 0, OZ_IO_SCSI_OPEN, sizeof scsi_open, &scsi_open);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u opening scsi id %u\n", pn, sts, scsi_id);
      return (sts);
    }
  }

  /* Now perform the requested I/O */

  memset (&scsi_getinfo1, 0, sizeof scsi_getinfo1);

  sts = do_getinfo1 (&scsi_getinfo1);
  if (sts == OZ_PROCMODE) sts = oz_sys_callknl (do_getinfo1, &scsi_getinfo1);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u getting scsi controller info\n", pn, sts);
    return (sts);
  }

  /* Print out the results */

  oz_sys_io_fs_printf (oz_util_h_output, " Max scsi id allowed: %u\n", scsi_getinfo1.max_scsi_id - 1);
  oz_sys_io_fs_printf (oz_util_h_output, "Controller's scsi id: %u\n", scsi_getinfo1.ctrl_scsi_id);
  if (scsi_getinfo1.open_scsi_id < scsi_getinfo1.max_scsi_id) {
    speed = scsi_getinfo1.open_speed;
    if (speed < sizeof speed_table) speed = speed_table[speed];
    else speed = 250 / speed;
    oz_sys_io_fs_printf (oz_util_h_output, "        Open scsi id: %u\n", scsi_getinfo1.open_scsi_id);
    oz_sys_io_fs_printf (oz_util_h_output, "      Transfer width: %u bits\n", 8 << scsi_getinfo1.open_width);
    oz_sys_io_fs_printf (oz_util_h_output, "      Transfer speed: %u MHz\n", speed);
  }
  return (OZ_SUCCESS);

  /* Something missing from command line, output usage message */

usage:
  oz_sys_io_fs_printf (oz_util_h_error, "%s: %s '%s'\n", pn, ermsg, argv[i] == NULL ? "" : argv[i]);
  oz_sys_io_fs_printf (oz_util_h_error, "usage: %s getinfo <device_name> [<scsi_id>]\n", pn);
  return (OZ_MISSINGPARAM);
}

static uLong do_getinfo1 (void *giv)

{
  uLong sts;

  sts = oz_sys_io (OZ_PROCMODE_KNL, h_iochan, 0, OZ_IO_SCSI_GETINFO1, 
                   sizeof (OZ_IO_scsi_getinfo1), giv);
  return (sts);
}

static uLong scsireset (int argc, char *argv[])

{
  char *device, *ermsg;
  int i;
  uLong sts;

  device = NULL;

  for (i = 2; i < argc; i ++) {
    ermsg = "unknown option";
    if (argv[i][0] == '-') goto usage;
    if (device == NULL) {
      device = argv[i];
      continue;
    }
    ermsg = "extra parameters";
    goto usage;
  }
  ermsg = "missing scsi device name";
  if (device == NULL) goto usage;

  sts = oz_sys_io_assign (OZ_PROCMODE_KNL, &h_iochan, device, OZ_LOCKMODE_CW);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s reset: error %u assigning channel to %s\n", pn, sts, device);
    return (sts);
  }

  sts = oz_sys_io (OZ_PROCMODE_KNL, h_iochan, 0, OZ_IO_SCSI_RESET, 0, NULL);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s reset: error %u resetting scsi bus\n", pn, sts);
  }
  return (sts);

  /* Something missing from command line, output usage message */

usage:
  oz_sys_io_fs_printf (oz_util_h_error, "%s reset: %s '%s'\n", pn, ermsg, argv[i] == NULL ? "" : argv[i]);
  oz_sys_io_fs_printf (oz_util_h_error, "usage: %s reset <scsi_controller_device_name>\n", pn);
  return (OZ_MISSINGPARAM);
}

static uLong scsiscan (int argc, char *argv[])

{
  char *device, *ermsg;
  int i, scsi_id, usedup;
  OZ_IO_scsi_getinfo1 scsi_getinfo1;
  uLong sts;

  device  = NULL;
  scsi_id = -1;

  for (i = 2; i < argc; i ++) {
    ermsg = "unknown option";
    if (argv[i][0] == '-') goto usage;
    if (device == NULL) {
      device = argv[i];
      continue;
    }
    if (scsi_id < 0) {
      scsi_id = oz_hw_atoi (argv[i], &usedup);
      if ((argv[i][usedup] != 0) || (scsi_id < 0)) {
        ermsg = "bad scsi-id number";
        goto usage;
      }
      continue;
    }
    ermsg = "extra parameters";
    goto usage;
  }
  ermsg = "missing scsi device name";
  if (device == NULL) goto usage;

  sts = oz_sys_io_assign (OZ_PROCMODE_KNL, &h_iochan, device, OZ_LOCKMODE_CW);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s scan: error %u assigning channel to %s\n", pn, sts, device);
    return (sts);
  }

  memset (&scsi_getinfo1, 0, sizeof scsi_getinfo1);
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_iochan, 0, OZ_IO_SCSI_GETINFO1, sizeof scsi_getinfo1, &scsi_getinfo1);
  if (sts != OZ_SUCCESS) {
     oz_sys_io_fs_printf (oz_util_h_error, "%s scan: error %u getting %s info\n", pn, sts, device);
    return (sts);
  }

  if (scsi_id < 0) scsi_id = -((int)(scsi_getinfo1.max_scsi_id));
  else if (scsi_id >= scsi_getinfo1.max_scsi_id) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s scan: %s allows max scsi id of %u\n", pn, device, scsi_getinfo1.max_scsi_id - 1);
    return (OZ_BADPARAM);
  }

  sts = oz_sys_callknl (scsiscanknl, &scsi_id);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s scan: error %u scanning %s\n", pn, sts, device);
  }
  return (sts);

  /* Something missing from command line, output usage message */

usage:
  oz_sys_io_fs_printf (oz_util_h_error, "%s scan: %s '%s'\n", pn, ermsg, (argv[i] == NULL) ? "" : argv[i]);
  oz_sys_io_fs_printf (oz_util_h_error, "usage: %s scan <scsi_controller_device_name> [<scsi_id>]\n", pn);
  return (OZ_MISSINGPARAM);
}

/* Do the work in kernel mode */

static uLong scsiscanknl (OZ_Procmode cprocmode, void *scsi_idv)

{
  int scsi_id, si;
  uLong sts;
  OZ_Devunit *devunit;
  OZ_Iochan *iochan;

  scsi_id = *(int *)scsi_idv;
  si  = oz_hw_cpu_setsoftint (0);							/* prevent thread aborts */
  sts = oz_knl_handle_takeout (h_iochan, cprocmode, OZ_SECACCMSK_WRITE, OZ_OBJTYPE_IOCHAN, &iochan, NULL);
  if (sts == OZ_SUCCESS) {
    devunit = oz_knl_iochan_getdevunit (iochan);					/* convert iochan to devunit */
    if (scsi_id >= 0) oz_dev_scsi_scan1 (devunit, scsi_id);				/* scan just one unit */
    else oz_dev_scsi_scan (devunit, -scsi_id);						/* scan the scsi bus */
    oz_knl_handle_putback (h_iochan);							/* all done with iochan */
  }
  oz_hw_cpu_setsoftint (1);								/* allow thread aborts now */
  return (sts);										/* return status */
}
