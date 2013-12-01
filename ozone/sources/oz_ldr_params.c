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
/*  Loader parameter block table and routines				*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_ldr_params.h"

OZ_Loadparams oz_ldr_paramblock;

const OZ_Loadparamtable oz_ldr_paramtable[] = {

"load_device",		1, ptype_string,	"floppy.p0",		sizeof oz_ldr_paramblock.load_device,		oz_ldr_paramblock.load_device,
"load_directory",	1, ptype_string,	"/ozone/binaries/",	sizeof oz_ldr_paramblock.load_dir,		oz_ldr_paramblock.load_dir,
"load_fstemplate",	1, ptype_string,	"oz_dfs",		sizeof oz_ldr_paramblock.load_fstemp,		oz_ldr_paramblock.load_fstemp,
"load_script",		1, ptype_string,	"script.ldr",		sizeof oz_ldr_paramblock.load_script,		oz_ldr_paramblock.load_script,
"kernel_image",		1, ptype_string,	"oz_kernel_486.oz",	sizeof oz_ldr_paramblock.kernel_image,		oz_ldr_paramblock.kernel_image,

"startup_image",	1, ptype_string,	"image",		sizeof oz_ldr_paramblock.startup_image,		oz_ldr_paramblock.startup_image,
"startup_input",	1, ptype_string,	"input",		sizeof oz_ldr_paramblock.startup_input,		oz_ldr_paramblock.startup_input,
"startup_output",	1, ptype_string,	"output",		sizeof oz_ldr_paramblock.startup_output,	oz_ldr_paramblock.startup_output,
"startup_error",	1, ptype_string,	"error",		sizeof oz_ldr_paramblock.startup_error,		oz_ldr_paramblock.startup_error,
"startup_params",	1, ptype_string,	"params",		sizeof oz_ldr_paramblock.startup_params,	oz_ldr_paramblock.startup_params,
	
"kernel_stack_size",	1, ptype_ulong,		"12288",		sizeof oz_ldr_paramblock.kernel_stack_size,	&oz_ldr_paramblock.kernel_stack_size,
"def_user_stack_size",	1, ptype_ulong,		"1048576",		sizeof oz_ldr_paramblock.def_user_stack_size,	&oz_ldr_paramblock.def_user_stack_size,

"clock_rate",		1, ptype_ulong,		"0",			sizeof oz_ldr_paramblock.clock_rate,		&oz_ldr_paramblock.clock_rate,

"nonpaged_pool_size",	1, ptype_ulong,		"1048576",		sizeof oz_ldr_paramblock.nonpaged_pool_size,	&oz_ldr_paramblock.nonpaged_pool_size,
"system_pages",		1, ptype_ulong,		"8192",			sizeof oz_ldr_paramblock.system_pages,		&oz_ldr_paramblock.system_pages,
"cpu_disable",		1, ptype_ulong,		"0",			sizeof oz_ldr_paramblock.cpu_disable,		&oz_ldr_paramblock.cpu_disable,
"debug_init",           1, ptype_ulong,         "0",                    sizeof oz_ldr_paramblock.debug_init,        	&oz_ldr_paramblock.debug_init,
"knl_exception",        1, ptype_ulong,         "0",                    sizeof oz_ldr_paramblock.knl_exception,		&oz_ldr_paramblock.knl_exception,

"tz_offset_rtc",	1, ptype_long,		"-18000",		sizeof oz_ldr_paramblock.tz_offset_rtc,		&oz_ldr_paramblock.tz_offset_rtc,

"uniprocessor",		1, ptype_ulong,		"0",			sizeof oz_ldr_paramblock.uniprocessor,		&oz_ldr_paramblock.uniprocessor,
"memory_megabytes",     1, ptype_ulong,         "0",                    sizeof oz_ldr_paramblock.memory_megabytes,  	&oz_ldr_paramblock.memory_megabytes,
"monochrome",           1, ptype_ulong,         "0",                    sizeof oz_ldr_paramblock.monochrome,        	&oz_ldr_paramblock.monochrome,
"signature",            0, ptype_string,	"yyyy-mm-dd@hh:mm:ss.fffffff", sizeof oz_ldr_paramblock.signature,	oz_ldr_paramblock.signature,

NULL, 0, ptype_string, NULL, 0, NULL };

/************************************************************************/
/*									*/
/*  Define an 'extra' parameter string					*/
/*									*/
/************************************************************************/

int oz_ldr_extra (void (*print) (const char *format, ...), const char *name, const char *valu)

{
  int i, j, s;

  /* Make sure name contains only sane characters */

  s = strlen (name);
  for (i = 0; i < s; i ++) {
    if (strchr ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._", name[i]) == NULL) {
      (*print) ("oz_ldr_extra: name can contain only alphabetics, numerics, dot, underscore\n");
      return (0);
    }
  }

  /* Remove the name if it already exists */

  for (i = 0; (i < sizeof oz_ldr_paramblock.extras) && (oz_ldr_paramblock.extras[i] != 0); i += j) {
    j = strlen (oz_ldr_paramblock.extras + i) + 1;
    if ((oz_ldr_paramblock.extras[i+s] == '=') && (strncasecmp (name, oz_ldr_paramblock.extras + i, s) == 0)) {
      memmove (oz_ldr_paramblock.extras + i, 
               oz_ldr_paramblock.extras + i + j, 
               sizeof oz_ldr_paramblock.extras - i - j);
      memset (oz_ldr_paramblock.extras + sizeof oz_ldr_paramblock.extras - j, 0, j);
      j = 0;
    }
  }

  /* Put name=valu in list */

  if (valu != NULL) {
    j = strlen (valu);										// get length of value string
    if (i + s + j + 2 >= sizeof oz_ldr_paramblock.extras) {					// see if it will all fit
      (*print) ("oz_ldr_extra: extras memory buffer too full to contain %s=%s\n", name, valu);
      return (0);
    }
    memcpy (oz_ldr_paramblock.extras + i, name, s);						// put name in buffer
    i += s;											// inc past name
    oz_ldr_paramblock.extras[i++] = '=';							// put in an '='
    memcpy (oz_ldr_paramblock.extras + i, valu, j);						// put in the value
    i += j;											// null fill buffer
    memset (oz_ldr_paramblock.extras + i, 0, sizeof oz_ldr_paramblock.extras - i);
  }

  return (1);
}

/************************************************************************/
/*									*/
/*  Print out the 'extra' parameter strings				*/
/*									*/
/************************************************************************/

void oz_ldr_extras (void (*print) (const char *format, ...), volatile int *abortit)

{
  int i, j, s, usedup;

  usedup = 0;
  for (i = 0; (i < sizeof oz_ldr_paramblock.extras) && (oz_ldr_paramblock.extras[i] != 0); i += j) {
    j = strlen (oz_ldr_paramblock.extras + i) + 1;
    s = strchr (oz_ldr_paramblock.extras + i, '=') - (oz_ldr_paramblock.extras + i);
    if (s > usedup) usedup = s;
  }
  if (usedup > 0) {
    (*print) ("\n");
    for (i = 0; (i < sizeof oz_ldr_paramblock.extras) && (oz_ldr_paramblock.extras[i] != 0); i += j) {
      j = strlen (oz_ldr_paramblock.extras + i) + 1;
      s = strchr (oz_ldr_paramblock.extras + i, '=') - (oz_ldr_paramblock.extras + i);
      if (s < 0) s = 0;
      if (s > usedup) s = usedup;
      (*print) ("  %*s%s\n", usedup - s, "", oz_ldr_paramblock.extras + i);
      if (*abortit) break;
    }
  }
  if (!*abortit) (*print) ("\n  %u character(s) available\n", sizeof oz_ldr_paramblock.extras - i);
}

/************************************************************************/
/*									*/
/*  Set a standard parameter						*/
/*									*/
/*    Input:								*/
/*									*/
/*	oz_ldr_paramtable = parameter descriptor table			*/
/*	print = pointer to routine to print an error message		*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_ldr_set = 0 : error (message already output)			*/
/*	             1 : success					*/
/*	*oz_ldr_paramtable = modified with new value			*/
/*									*/
/************************************************************************/

int oz_ldr_set (void (*print) (const char *format, ...), const char *name, const char *valu)

{
  char c;
  int i, j, s;
  uLong ul;

  for (i = 0; oz_ldr_paramtable[i].pname != NULL; i ++) {
    if (strcasecmp (name, oz_ldr_paramtable[i].pname) == 0) break;
  }
  if (oz_ldr_paramtable[i].pname == NULL) {
    (*print) ("oz_ldr_set: unknown parameter %s\n", name);
    return (0);
  }
  if (!(oz_ldr_paramtable[i].pmods)) {
    (*print) ("oz_ldr_set: parameter %s cannot be changed\n", oz_ldr_paramtable[i].pname);
    return (0);
  }
  switch (oz_ldr_paramtable[i].ptype) {
    case ptype_string: {
      if (strlen (valu) < oz_ldr_paramtable[i].psize) strcpy (oz_ldr_paramtable[i].paddr, (valu));
      else {
        (*print) ("oz_ldr_set: string for parameter %s too long, max length %u\n", oz_ldr_paramtable[i].pname, oz_ldr_paramtable[i].psize - 1);
        return (0);
      }
      break;
    }
    case ptype_ulong: {
      ul = 0;
      for (j = 0; (c = valu[j]) != 0; j ++) {
        if ((c < '0') || (c > '9')) break;
        ul = ul * 10 + c - '0';
      }
      if (c == 0) *((uLong *)(oz_ldr_paramtable[i].paddr)) = ul;
      else {
        (*print) ("oz_ldr_set: invalid numeric value %s for parameter %s\n", valu, oz_ldr_paramtable[i].pname);
        return (0);
      }
      break;
    }
    case ptype_long: {
      ul = 0;
      s = 1;
      for (j = 0; (c = valu[j]) != 0; j ++) {
        if (c == '-') {
          s = -1;
          continue;
        }
        if ((c < '0') || (c > '9')) break;
        ul = ul * 10 + c - '0';
      }
      if (c != 0) {
        (*print) ("oz_ldr_set: invalid numeric value %s for parameter %s\n", valu, oz_ldr_paramtable[i].pname);
        return (0);
      }
      *((Long *)(oz_ldr_paramtable[i].paddr)) = ((Long)ul) * s;
      break;
    }
    case ptype_pointer: {
      ul = 0;
      for (j = 0; (c = valu[j]) != 0; j ++) {
        if ((c >= 'A') && (c <= 'F')) c += 'a' - 'A';
        if ((c >= 'a') && (c <= 'f')) c -= 'a' - '0' - 10;
        else if ((c < '0') || (c > '9')) break;
        ul = ul * 10 + c - '0';
      }
      if (c == 0) *((uLong *)(oz_ldr_paramtable[i].paddr)) = ul;
      else {
        (*print) ("oz_ldr_set: invalid numeric value %s for parameter %s\n", valu, oz_ldr_paramtable[i].pname);
        return (0);
      }
      break;
    }
  }

  return (1);
}

/************************************************************************/
/*									*/
/*  Show standard parameters						*/
/*									*/
/*    Input:								*/
/*									*/
/*	oz_ldr_paramtable = parameter descriptor table			*/
/*	print = pointer to routine to do the printing			*/
/*	*abortit = set to abort printing				*/
/*									*/
/************************************************************************/

void oz_ldr_show (void (*print) (const char *format, ...), volatile int *abortit)

{
  int i, j;

  j = 1;
  for (i = 0; oz_ldr_paramtable[i].pname != NULL; i ++) {
    if (strlen (oz_ldr_paramtable[i].pname) > j) j = strlen (oz_ldr_paramtable[i].pname);
  }
  for (i = 0; oz_ldr_paramtable[i].pname != NULL; i ++) {
    switch (oz_ldr_paramtable[i].ptype) {
      case ptype_string: {
        (*print) ("    %*s  =  '%s'\n", j, oz_ldr_paramtable[i].pname, oz_ldr_paramtable[i].paddr);
        break;
      }
      case ptype_ulong: {
        (*print) ("    %*s  =  %u\n", j, oz_ldr_paramtable[i].pname, *((uLong *)(oz_ldr_paramtable[i].paddr)));
        break;
      }
      case ptype_long: {
        (*print) ("    %*s  =  %d\n", j, oz_ldr_paramtable[i].pname, *((Long *)(oz_ldr_paramtable[i].paddr)));
        break;
      }
      case ptype_pointer: {
        (*print) ("    %*s  =  %p\n", j, oz_ldr_paramtable[i].pname, *((void **)(oz_ldr_paramtable[i].paddr)));
        break;
      }
    }
    if (*abortit) break;
  }
}
