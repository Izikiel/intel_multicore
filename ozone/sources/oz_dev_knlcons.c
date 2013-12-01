//+++2001-10-06
//    Copyright (C) 2001, Mike Rieker, Beverly, MA USA
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
//---2001-10-06

/************************************************************************/
/*									*/
/*  Console driver that uses oz_hw_putcon and oz_hw_getcon		*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_io_console.h"
#include "oz_knl_devio.h"
#include "oz_knl_hw.h"
#include "oz_knl_procmode.h"
#include "oz_knl_sdata.h"
#include "oz_knl_status.h"

static uLong oz_dev_knlcons_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnex, OZ_Procmode procmode, 
                                   OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap);

static const OZ_Devfunc oz_dev_knlcons_functable = { 0, 0, 0, 0, NULL, NULL, NULL, NULL, NULL, NULL, oz_dev_knlcons_start, NULL };

static int initialized = 0;

static OZ_Devclass *devclass;
static OZ_Devdriver *devdriver;
static OZ_Devunit *devunit;

static uLong conwrite (uLong size, const void *buff, uLong trmsize, const void *trmbuff, OZ_Ioop *ioop);
static uLong conread (uLong size, void *buff, uLong *rlen, uLong pmtsize, const void *pmtbuff, OZ_Ioop *ioop);

/************************************************************************/
/*									*/
/*  Boot-time initialization routine					*/
/*									*/
/************************************************************************/

void oz_dev_knlcons_init ()

{
  uLong sts;

  if (!initialized) {
    oz_knl_printk ("oz_dev_knlcons_init\n");
    initialized = 1;
    devclass  = oz_knl_devclass_create (OZ_IO_CONSOLE_CLASSNAME, OZ_IO_CONSOLE_BASE, OZ_IO_CONSOLE_MASK, "oz_dev_knlcons");
    devdriver = oz_knl_devdriver_create (devclass, "oz_dev_knlcons");
    devunit   = oz_knl_devunit_create (devdriver, "console", "console via oz_hw_putcon and oz_hw_getcon", &oz_dev_knlcons_functable, 0, oz_s_secattr_sysdev);
  }
}

/************************************************************************/
/*									*/
/*  Start performing a console i/o function				*/
/*									*/
/************************************************************************/

static uLong oz_dev_knlcons_start (OZ_Devunit *devunit, void *devexv, OZ_Iochan *iochan, void *chnexv, OZ_Procmode procmode, 
                                   OZ_Ioop *ioop, void *iopexv, uLong funcode, uLong as, void *ap)

{
  uLong sts;
  OZ_IO_console_read console_read;
  OZ_IO_fs_readrec fs_readrec;
  OZ_IO_console_write console_write;
  OZ_IO_fs_writerec fs_writerec;

  switch (funcode) {

    /* Write a line to the console */

    case OZ_IO_CONSOLE_WRITE: {
      movc4 (as, ap, sizeof console_write, &console_write);
      sts = conwrite (console_write.size, console_write.buff, console_write.trmsize, console_write.trmbuff, ioop);
      return (sts);
    }

    case OZ_IO_FS_WRITEREC: {
      movc4 (as, ap, sizeof fs_writerec, &fs_writerec);
      sts = conwrite (fs_writerec.size, fs_writerec.buff, fs_writerec.trmsize, fs_writerec.trmbuff, ioop);
      return (sts);
    }

    /* Read a line from the console */

    case OZ_IO_CONSOLE_READ: {
      movc4 (as, ap, sizeof console_read, &console_read);
      sts = conread (console_read.size, console_read.buff, console_read.rlen, console_read.pmtsize, console_read.pmtbuff, ioop);
      return (sts);
    }

    case OZ_IO_FS_READREC: {
      movc4 (as, ap, sizeof fs_readrec, &fs_readrec);
      sts = conread (fs_readrec.size, fs_readrec.buff, fs_readrec.rlen, fs_readrec.pmtsize, fs_readrec.pmtbuff, ioop);
      return (sts);
    }

    /* No-op fs functions */

    case OZ_IO_FS_CREATE:
    case OZ_IO_FS_OPEN:
    case OZ_IO_FS_CLOSE: {
      return (OZ_SUCCESS);
    }

    /* Who knows what */

    default: {
      return (OZ_BADIOFUNC);
    }
  }
}

/************************************************************************/
/*									*/
/*  Process write request						*/
/*									*/
/************************************************************************/

static uLong conwrite (uLong size, const void *buff, uLong trmsize, const void *trmbuff, OZ_Ioop *ioop)

{
  uLong sts;

  sts = oz_knl_ioop_lock (ioop, size, buff, 0, NULL, NULL, NULL);
  if (sts == OZ_SUCCESS) sts = oz_knl_ioop_lock (ioop, trmsize, trmbuff, 0, NULL, NULL, NULL);
  if (sts == OZ_SUCCESS) {
    oz_hw_putcon (size, buff);
    oz_hw_putcon (trmsize, trmbuff);
  }
  return (sts);
}

/************************************************************************/
/*									*/
/*  Process read request						*/
/*									*/
/************************************************************************/

static uLong conread (uLong size, void *buff, uLong *rlen, uLong pmtsize, const void *pmtbuff, OZ_Ioop *ioop)

{
  int rc;
  uLong sts;

  sts = oz_knl_ioop_lock (ioop, size, buff, 1, NULL, NULL, NULL);
  if (sts == OZ_SUCCESS) sts = oz_knl_ioop_lock (ioop, pmtsize, pmtbuff, 0, NULL, NULL, NULL);
  if ((sts == OZ_SUCCESS) && (rlen != NULL)) sts = oz_knl_ioop_lock (ioop, sizeof *rlen, rlen, 1, NULL, NULL, NULL);
  if ((sts == OZ_SUCCESS) && (rlen != NULL)) *rlen = 0;
  if (sts == OZ_SUCCESS) {
    rc = oz_hw_getcon (size, buff, pmtsize, pmtbuff);
    if (!rc) sts = OZ_ENDOFFILE;
    else if (rlen != NULL) *rlen = strlen (buff);
  }
  return (sts);
}
