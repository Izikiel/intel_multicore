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

typedef unsigned int uLong;
typedef unsigned long uQuad;

int isitexxx6_compare (uLong ques)

{
  return (ques == (uLong)0xE0000006UL);
}

int isitexxx6_switch (uLong ques)

{
  int rc;

  switch (ques) {
    case 0xE0000006ULL: {
      rc = 1;
      break;
    }
    default: {
      rc = 0;
      break;
    }
  }
  return (rc);
}
