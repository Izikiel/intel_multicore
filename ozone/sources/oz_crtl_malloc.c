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
/*  General heap memory allocation routines				*/
/*  They call the corresponding oz_sys_pdata_... routines		*/
/*  They can be called from user or kernel modes			*/
/*									*/
/************************************************************************/

#define _OZ_CRTL_MALLOC_C

#include "ozone.h"
#include "oz_crtl_malloc.h"
#include "oz_knl_procmode.h"
#include "oz_sys_pdata.h"

#define ALIAS(x) asm (" .globl __" #x "\n __" #x "=" #x );

void *malloc (size_t size)

{
  if (size == 0) return (NULL);
  return (oz_sys_pdata_malloc (OZ_PROCMODE_KNL, size));
}

void free (void *adrs)

{
  if (adrs != NULL) oz_sys_pdata_free (OZ_PROCMODE_KNL, adrs);
}

void *calloc (size_t nmemb, size_t size)

{
  void *mem;

  mem = oz_sys_pdata_malloc (OZ_PROCMODE_KNL, nmemb * size);
  memset (mem, 0, nmemb * size);
  return (mem);
}

void *realloc (void *ptr, size_t size)

{
  size_t oldsize;
  void *mem;

  if (ptr == NULL) return (oz_sys_pdata_malloc (OZ_PROCMODE_KNL, size));
  oldsize = oz_sys_pdata_valid (OZ_PROCMODE_KNL, ptr);
  if (size <= oldsize) return (ptr);
  mem = malloc (size);
  memcpy (mem, ptr, oldsize);
  free (ptr);
  return (mem);
}

char *strdup (const char *s)

{
  char *d;

  d = oz_sys_pdata_malloc (OZ_PROCMODE_KNL, strlen (s) + 1);
  strcpy (d, s);
  return (d);
}

ALIAS (strdup)
