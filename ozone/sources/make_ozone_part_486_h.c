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
/*  This program takes the linked ozone_part_486.s and creates an 	*/
/*  ozone_part_486.h file for inclusion in oz_util_partition.c		*/
/*									*/
/************************************************************************/

#include <errno.h>
#include <stdio.h>

int main ()

{
  unsigned char blockbuff[512-2-64-32];
  FILE *hfile, *ofile;
  int blocksize, i;

  ofile = fopen ("ozone_part_486", "r");
  if (ofile == NULL) {
    perror ("error opening ozone_part_486");
    return (-1);
  }
  hfile = fopen ("ozone_part_486.h", "w");
  if (hfile == NULL) {
    perror ("error opening ozone_part_486.h");
    return (-1);
  }

  fseek (ofile, 32, SEEK_SET);

  blocksize = fread (blockbuff, 1, sizeof blockbuff, ofile);
  if (blocksize < 0) {
    perror ("error reading ozone_part_486");
    return (-1);
  }

  fprintf (hfile, "static const uByte ourcode[] = {\n");
  for (i = 0; i < blocksize - 1; i ++) {
    fprintf (hfile, " 0x%2.2x,", blockbuff[i]);
    if ((i & 15) == 15) fprintf (hfile, "\n");
  }
  fprintf (hfile, " 0x%2.2x };\n", blockbuff[i]);
  if (fclose (hfile) < 0) {
    perror ("error closing ozone_part_486.h");
    return (-1);
  }
  return (0);
}
