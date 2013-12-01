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
/*  Modify disk's partition table					*/
/*									*/
/************************************************************************/

//#include <stdlib.h>
//#include <string.h>

#include "ozone.h"
#include "oz_crtl_malloc.h"
#include "oz_io_disk.h"
#include "oz_knl_devio.h"
#include "oz_knl_status.h"
#include "oz_sys_condhand.h"
#include "oz_sys_handle.h"
#include "oz_sys_handle_getinfo.h"
#include "oz_sys_io.h"
#include "oz_sys_io_fs.h"
#include "oz_sys_io_fs_printf.h"
#include "oz_util_start.h"

#define BOOTBLOCK 0
#define BLOCKSIZE 512
#define MAGIC 0xAA55

#include "ozone_part_486.h"

static char *pn = "partition";

static int getpartidx (char **cp_r);
static char *getpname (char **cp_r);
static int getptype (char **cp_r);
static OZ_Dbn getblockcount (char **cp_r, OZ_Dbn disksize);
static OZ_Dbn getblockstart (char **cp_r, OZ_Dbn disksize, OZ_Dbn count);
static int eolcheck (char *cp);
static char *skipspaces (char *cp);
static int matchkeyword (const char *cp, const char *kw, int min);

uLong oz_util_main (int argc, char *argv[])

{
  char cmdbuff[64], *cp, diskname[OZ_DEVUNIT_NAMESIZE], *diskparent, partdiskname[OZ_DEVUNIT_NAMESIZE];
  int i, itisours, j, nomodsatall;
  OZ_Dbn diskoffset, disksize;
  OZ_Handle h_disk, h_partchn, h_partdev, h_prev;
  OZ_Handle_item hitems[2];
  OZ_IO_disk_getinfo1 disk_getinfo1, part_getinfo1;
  OZ_IO_disk_readblocks disk_readblocks;
  OZ_IO_disk_setvolvalid disk_setvolvalid;
  OZ_IO_disk_writeblocks disk_writeblocks;
  OZ_IO_fs_readrec fs_readrec;
  struct { char string[OZ_DEVUNIT_NAMESIZE]; } partdisknames[4];
#pragma pack (1)
  struct { uByte code[BLOCKSIZE-2-64-32];
           struct { char string[8]; } partnames[4];
           struct { uByte flag;
                    uByte fill1[3];
                    uByte ptype;
                    uByte fill2[3];
                    uLong start;
                    uLong count;
                  } partitions[4];
           uWord magic;
         } original, working;
#pragma nopack
  uLong cmdrlen, sts;

  if (argc > 0) pn = argv[0];

  if (argc != 2) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: usage: %s <disk>\n", pn, pn);
    return (OZ_MISSINGPARAM);
  }

  memset (partdisknames, 0, sizeof partdisknames);

  memset (&fs_readrec, 0, sizeof fs_readrec);
  fs_readrec.size    = sizeof cmdbuff - 1;
  fs_readrec.buff    = cmdbuff;
  fs_readrec.trmsize = 1;
  fs_readrec.trmbuff = "\n";
  fs_readrec.rlen    = &cmdrlen;
  cp = malloc (strlen (pn) + 4);
  strcpy (cp, pn);
  strcat (cp, "> ");
  fs_readrec.pmtsize = strlen (cp);
  fs_readrec.pmtbuff = cp;

  /* Assign channel to disk and get its offical unit name */

  sts = oz_sys_io_assign (OZ_PROCMODE_KNL, &h_disk, argv[1], OZ_LOCKMODE_CW);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u assigning channel to disk %s\n", pn, sts, argv[1]);
    return (sts);
  }
  sts = oz_sys_iochan_getunitname (h_disk, sizeof diskname, diskname);
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);

  /* Make sure disk is spun up */

  memset (&disk_setvolvalid, 0, sizeof disk_setvolvalid);
  disk_setvolvalid.valid = 1;
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_disk, 0, OZ_IO_DISK_SETVOLVALID, sizeof disk_setvolvalid, &disk_setvolvalid);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u turning disk %u online\n", pn, sts, diskname);
    return (sts);
  }

  /* Read about the disk */

  memset (&disk_getinfo1, 0, sizeof disk_getinfo1);
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_disk, 0, OZ_IO_DISK_GETINFO1, sizeof disk_getinfo1, &disk_getinfo1);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u getting info about disk %u\n", pn, sts, diskname);
    return (sts);
  }

  if (disk_getinfo1.blocksize != sizeof original) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: disk has blocksize %u, we only deal wif %u\n", pn, disk_getinfo1.blocksize, sizeof original);
    return (OZ_BADBLOCKSIZE);
  }
  disksize   = disk_getinfo1.totalblocks;
  diskoffset = disk_getinfo1.parthoststartblock;
  diskparent = disk_getinfo1.parthostdevname;

  /* Read existing partition block */

  memset (&disk_readblocks, 0, sizeof disk_readblocks);
  disk_readblocks.size = sizeof original;
  disk_readblocks.buff = &original;
  disk_readblocks.slbn = BOOTBLOCK;

  sts = oz_sys_io (OZ_PROCMODE_KNL, h_disk, 0, OZ_IO_DISK_READBLOCKS, sizeof disk_readblocks, &disk_readblocks);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u reading %u byte block %u from %s\n", pn, sts, sizeof original, BOOTBLOCK, diskname);
    return (sts);
  }
  working  = original;
  itisours = 0;

  /* Make sure it has the Magic Word */

  if (original.magic != MAGIC) {
    oz_sys_io_fs_printf (oz_util_h_output, "existing block does not have magic word (%4.4x instead of %4.4x)\n", original.magic, MAGIC);
  }

  /* Now see if it is even a partition table (and not a boot block) */

  else for (i = 0; i < 4; i ++) {
    if (original.partitions[i].flag & 0x7F != 0) {
      oz_sys_io_fs_printf (oz_util_h_output, "disk does not contain a partition table (flag[%d] is %2.2x)\n", i, original.partitions[i].flag);
      working.magic = 0;
    }
  }

  /* If there are any active disks on the partitions, mark the partitions so we can't move them */

  nomodsatall = 0;
  hitems[0].code = OZ_HANDLE_CODE_DEVICE_FIRST;
  hitems[0].size = sizeof h_partdev;
  hitems[0].buff = &h_partdev;
  hitems[0].rlen = NULL;
  hitems[1].code = OZ_HANDLE_CODE_DEVICE_UNITNAME;
  hitems[1].size = sizeof partdiskname;
  hitems[1].buff = partdiskname;
  hitems[1].rlen = NULL;
  for (h_prev = 0;; h_prev = h_partdev) {

    /* Get handle to first/next device in system */

    sts = oz_sys_handle_getinfo (h_prev, 1, hitems + 0, NULL);		/* get handle to first/next device */
    if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);		/* should always be successful */
    oz_sys_handle_release (OZ_PROCMODE_KNL, h_prev);			/* release handle to previous device */
    if (h_partdev == 0) break;						/* stop if reached end of the list */
    hitems[0].code = OZ_HANDLE_CODE_DEVICE_NEXT;			/* from now on get next one (not first) */
    sts = oz_sys_handle_getinfo (h_partdev, 1, hitems + 1, NULL);	/* get new device's name */
    if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
    if (strcmp (partdiskname, diskname) == 0) continue;

    /* See if it is a disk that is one of our partitions, skip over it if it isn't */

    sts = oz_sys_io_assign (OZ_PROCMODE_KNL, &h_partchn, partdiskname, OZ_LOCKMODE_NL);
    if (sts != OZ_SUCCESS) {
      if (sts != OZ_KERNELONLY) oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u assigning channel to device %s\n", pn, sts, partdiskname);
      continue;
    }
    memset (&part_getinfo1, 0, sizeof part_getinfo1);
    sts = oz_sys_io (OZ_PROCMODE_KNL, h_partchn, 0, OZ_IO_DISK_GETINFO1, sizeof part_getinfo1, &part_getinfo1);
    oz_sys_handle_release (OZ_PROCMODE_KNL, h_partchn);
    if (sts != OZ_SUCCESS) {
      if ((sts != OZ_BADIOFUNC) && (sts != OZ_DEVOFFLINE)) oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u getting %s info\n", pn, sts, partdiskname);
      continue;
    }
    if (strcmp (part_getinfo1.parthostdevname, diskname) != 0) continue;

    /* Try to find it in the partition table */

    for (i = 0; i < 4; i ++) {
      if ((working.magic == MAGIC) 
       && (working.partitions[i].count == part_getinfo1.totalblocks) 
       && (working.partitions[i].start == part_getinfo1.parthoststartblock)) break;
    }

    /* If not found, who knows why, so don't allow any modifications */

    if (i == 4) {
      oz_sys_io_fs_printf (oz_util_h_output, "disk %s references %s for %u blocks at %u but can't be found in original table\n", 
                           partdiskname, diskname, part_getinfo1.totalblocks, part_getinfo1.parthoststartblock);
      nomodsatall = 1;
    }

    /* Found, make sure they can't change its definition */

    else strcpy (partdisknames[i].string, partdiskname);
  }

  /* See if it is our partition code or someone elses.  If it is ours, we can print out names, else we cant. */

  if (working.magic == MAGIC) {
    if (memcmp (working.code, ourcode, sizeof ourcode) == 0) itisours = 1;
    goto printout;
  }

  /* Re-initialize partition table -                                     */
  /* We always overlay the code section                                  */
  /* Reset the partition names only if it wasn't in our format           */
  /* Reset the partition definitions only if it wasn't a partition block */

reinit:
  oz_sys_io_fs_printf (oz_util_h_output, "re-initializing partition block\n");
  movc4 (sizeof ourcode, ourcode, sizeof working.code, working.code);		/* always plunk our code in there */
  if (!itisours) {								/* see if it wasn't ours */
    memset (working.partnames, 0, sizeof working.partnames);			/* it wasn't, re-init the names */
    itisours = 1;								/* (it is now our format) */
  }
  if (working.magic != MAGIC) {							/* see if table was valid */
    memset (working.partitions, 0, sizeof working.partitions);			/* if not, clear it out */
    working.magic = MAGIC;							/* table is now valid */
  } else {
    for (i = 0; i < 4; i ++) working.partitions[i].flag = 0;
  }

  /* Print out partition table */

printout:
  oz_sys_io_fs_printf (oz_util_h_output, "\nDisk %s contains %u blocks", diskname, disksize);
  if (diskoffset != 0) oz_sys_io_fs_printf (oz_util_h_output, " starting at offset %u of %s\n", diskoffset, diskparent);
  oz_sys_io_fs_printf (oz_util_h_output, "\n\n");

  for (i = 0; i < 4; i ++) {
    oz_sys_io_fs_printf (oz_util_h_output, "  %d%c", i + 1, working.partitions[i].flag ? '*' : ' ');
    if (itisours) oz_sys_io_fs_printf (oz_util_h_output, " %8s", working.partnames[i].string);
    oz_sys_io_fs_printf (oz_util_h_output, ": type %2.2x has %10u blocks at %10u", 
                         working.partitions[i].ptype, working.partitions[i].count, working.partitions[i].start);
    if (partdisknames[i].string[0] != 0) oz_sys_io_fs_printf (oz_util_h_output, "  %s", partdisknames[i].string);
    oz_sys_io_fs_printf (oz_util_h_output, "\n");
  }

  /* Read command and process it */

  do {
    sts = oz_sys_io (OZ_PROCMODE_KNL, oz_util_h_input, 0, OZ_IO_FS_READREC, sizeof fs_readrec, &fs_readrec);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u reading command input\n", pn, sts);
      return (sts);
    }
    cmdbuff[cmdrlen] = 0;
    cp = skipspaces (cmdbuff);
  } while (*cp == 0);

  /* Activate partition */

  if (i = matchkeyword (cp, "activate", 1)) {
    cp = skipspaces (cp + i);
    i  = getpartidx (&cp);
    if (i < 0) goto printout;
    if (!eolcheck (cp)) goto printout;
    if (nomodsatall) {
      oz_sys_io_fs_printf (oz_util_h_output, "no modifications allowed\n");
      goto printout;
    }
    for (j = 0; j < 4; j ++) {
      working.partitions[j].flag = 0;
      if (j == i) working.partitions[j].flag = 0x80;
    }
    goto printout;
  }

  /* Change partition parameters */

  if (i = matchkeyword (cp, "change", 1)) {
    char *pname;
    int ptype;
    OZ_Dbn count, start;

    cp = skipspaces (cp + i);
    i  = getpartidx (&cp);
    if (i < 0) goto printout;
    if (nomodsatall) {
      oz_sys_io_fs_printf (oz_util_h_output, "no modifications allowed\n");
      goto printout;
    }
    if (itisours) {
      pname = getpname (&cp);
      if (pname == NULL) goto printout;
    }
    ptype = working.partitions[i].ptype;
    count = working.partitions[i].count;
    start = working.partitions[i].start;
    if (*cp != 0) {
      ptype = getptype (&cp);
      if (ptype < 0) goto printout;
      if (*cp != 0) {
        count = getblockcount (&cp, disksize);
        if (count == 0) goto printout;
        if (*cp != 0) {
          start = getblockstart (&cp, disksize, count);
          if (start == 0) goto printout;
          if (!eolcheck (cp)) goto printout;
        }
      }
    }
    if ((partdisknames[i].string[0] != 0) && ((working.partitions[i].ptype != ptype) || (working.partitions[i].count != count) || (working.partitions[i].start != start))) {
      oz_sys_io_fs_printf (oz_util_h_output, "no modifications allowed - referenced by disk %s\n", partdisknames[i].string);
    } else {
      if (itisours) strncpyz (working.partnames[i].string, pname, sizeof working.partnames[i].string);
      working.partitions[i].ptype = ptype;
      working.partitions[i].count = count;
      working.partitions[i].start = start;
    }
    goto printout;
  }

  /* Delete partition */

  if (i = matchkeyword (cp, "delete", 3)) {
    cp = skipspaces (cp + i);
    i  = getpartidx (&cp);
    if (i < 0) goto printout;
    if (!eolcheck (cp)) goto printout;
    if (nomodsatall || (partdisknames[i].string[0] != 0)) {
      oz_sys_io_fs_printf (oz_util_h_output, "no modifications allowed\n");
      goto printout;
    }
    if (itisours) memset (working.partnames[i].string, 0, sizeof working.partnames[i].string);
    working.partitions[i].ptype = 0;
    working.partitions[i].count = 0;
    working.partitions[i].start = 0;
    goto printout;
  }

  /* Re-initialize */

  if (i = matchkeyword (cp, "initialize", 4)) {
    cp = skipspaces (cp + i);
    if (!eolcheck (cp)) goto printout;
    if (nomodsatall) {
      oz_sys_io_fs_printf (oz_util_h_output, "no modifications allowed\n");
      goto printout;
    }
    goto reinit;
  }

  /* Quit (abandon changes) */

  if (i = matchkeyword (cp, "quit", 4)) {
    cp = skipspaces (cp + i);
    if (!eolcheck (cp)) goto printout;
    return (OZ_SUCCESS);
  }

  /* Write partition table out to disk */

  if (i = matchkeyword (cp, "write", 5)) {
    cp = skipspaces (cp + i);
    if (!eolcheck (cp)) goto printout;

    if (nomodsatall) {
      oz_sys_io_fs_printf (oz_util_h_output, "no modifications allowed\n");
      goto printout;
    }

    /* Check for overlapping partitions */

    for (i = 0; i < 4; i ++) {
      for (j = 0; j < 4; j ++) {
        if (i == j) continue;
        if ((working.partitions[j].start >= working.partitions[i].start) 
         && (working.partitions[j].start < working.partitions[i].count + working.partitions[i].start)) {
          oz_sys_io_fs_printf (oz_util_h_output, "partitions %d and %d overlap\n", i + 1, j + 1);
          goto printout;
        }
      }
    }

    /* It is ok to write the partition table */

    memset (&disk_writeblocks, 0, sizeof disk_writeblocks);
    disk_writeblocks.size = sizeof working;
    disk_writeblocks.buff = &working;
    disk_writeblocks.slbn = BOOTBLOCK;

    sts = oz_sys_io (OZ_PROCMODE_KNL, h_disk, 0, OZ_IO_DISK_WRITEBLOCKS, sizeof disk_writeblocks, &disk_writeblocks);
    if (sts == OZ_SUCCESS) oz_sys_io_fs_printf (oz_util_h_output, "new partition table written to disk %s\n", diskname);
    else oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u writing %u byte block %u to %s\n", pn, sts, sizeof working, BOOTBLOCK, diskname);
    return (sts);
  }

  /* Unknown, print help text */

  oz_sys_io_fs_printf (oz_util_h_output, "\nCommands are:\n");
  oz_sys_io_fs_printf (oz_util_h_output, "  activate <number>\n");
  if (!itisours) oz_sys_io_fs_printf (oz_util_h_output, "  change <number> [<type> [<blockcount> [<start>]]]\n");
  else oz_sys_io_fs_printf (oz_util_h_output, "  change <number> <name> [<type> [<blockcount> [<start>]]]\n");
  oz_sys_io_fs_printf (oz_util_h_output, "  delete <number>\n");
  oz_sys_io_fs_printf (oz_util_h_output, "  initialize\n");
  oz_sys_io_fs_printf (oz_util_h_output, "  quit\n");
  oz_sys_io_fs_printf (oz_util_h_output, "  write\n");
  goto printout;
}

static int getpartidx (char **cp_r)

{
  char *cp;
  int i;

  cp = *cp_r;
  i  = *(cp ++) - '1';
  if ((i < 0) || (i > 3) || (*cp > ' ')) {
    oz_sys_io_fs_printf (oz_util_h_output, "partition number must be digit 1..4\n");
    return (-1);
  }
  *cp_r = skipspaces (cp);
  return (i);
}

static char *getpname (char **cp_r)

{
  char *cp;
  int i;

  cp = *cp_r;
  for (i = 0; i < 8; i ++) if (cp[i] <= ' ') break;
  if (i == 8) {
    oz_sys_io_fs_printf (oz_util_h_output, "partition name must be at most 7 chars\n");
    return (NULL);
  }
  *cp_r = cp + i;
  if (cp[i] != 0) {
    *cp_r = skipspaces (cp + i);
    cp[i] = 0;
  }
  return (cp);
}

static int getptype (char **cp_r)

{
  char *cp;
  int ptype;

  ptype = strtoul (*cp_r, &cp, 16);
  if ((*cp > ' ') || (ptype < 1) || (ptype > 255)) {
    oz_sys_io_fs_printf (oz_util_h_output, "ptype must be hex integer 01..FF\n");
    ptype = -1;
  }
  *cp_r = skipspaces (cp);
  return (ptype);
}

static OZ_Dbn getblockcount (char **cp_r, OZ_Dbn disksize)

{
  char *cp;
  OZ_Dbn count;

  count = strtoul (*cp_r, &cp, 0);
  if ((*cp > ' ') || (count == 0) || (count >= disksize)) {
    oz_sys_io_fs_printf (oz_util_h_output, "count must be integer less than disk size\n");
    count = 0;
  }
  *cp_r = skipspaces (cp);
  return (count);
}

static OZ_Dbn getblockstart (char **cp_r, OZ_Dbn disksize, OZ_Dbn count)

{
  char *cp;
  OZ_Dbn start;

  start = strtoul (*cp_r, &cp, 0);
  if ((*cp > ' ') || (start == 0) || (start + count > disksize)) {
    oz_sys_io_fs_printf (oz_util_h_output, "start must be an integer that does not run off end of disk\n");
    start = 0;
  }
  *cp_r = skipspaces (cp);
  return (start);
}

static int eolcheck (char *cp)

{
  if (*cp == 0) return (1);
  oz_sys_io_fs_printf (oz_util_h_output, "extra stuff at end of command (%s)\n", cp);
  return (0);
}

static char *skipspaces (char *cp)

{
  while ((*cp != 0) && (*cp <= ' ')) cp ++;
  return (cp);
}

static int matchkeyword (const char *cp, const char *kw, int min)

{
  int i;

  for (i = strlen (kw); i >= min; i --) if (strncasecmp (cp, kw, i) == 0) return (i);
  return (0);
}
