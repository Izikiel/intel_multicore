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
/*  Routines common to loader and kernel				*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_knl_hw.h"
#include "oz_knl_procmode.h"
#include "oz_knl_sdata.h"
#include "oz_knl_section.h"
#include "oz_knl_status.h"
#include "oz_ldr_params.h"
#include "oz_sys_dateconv.h"

/* Constants needed by assembler routines */

const uLong oz_ACCVIO       = OZ_ACCVIO;
const uLong oz_ALIGNMENT    = OZ_ALIGNMENT;
const uLong oz_ARITHOVER    = OZ_ARITHOVER;
const uLong oz_BADPARAM     = OZ_BADPARAM;
const uLong oz_BADSYSCALL   = OZ_BADSYSCALL;
const uLong oz_BREAKPOINT   = OZ_BREAKPOINT;
const uLong oz_DIVBYZERO    = OZ_DIVBYZERO;
const uLong oz_DOUBLEFAULT  = OZ_DOUBLEFAULT;
const uLong oz_FLAGWASCLR   = OZ_FLAGWASCLR;
const uLong oz_FLAGWASSET   = OZ_FLAGWASSET;
const uLong oz_FLOATPOINT   = OZ_FLOATPOINT;
const uLong oz_GENERALPROT  = OZ_GENERALPROT;
const uLong oz_OLDLINUXCALL = OZ_OLDLINUXCALL;
const uLong oz_RESIGNAL     = OZ_RESIGNAL;
const uLong oz_RESUME       = OZ_RESUME;
const uLong oz_SINGLESTEP   = OZ_SINGLESTEP;
const uLong oz_SUBSCRIPT    = OZ_SUBSCRIPT;
const uLong oz_SUCCESS      = OZ_SUCCESS;
const uLong oz_UNDEFOPCODE  = OZ_UNDEFOPCODE;
const uLong oz_UNWIND       = OZ_UNWIND;
const uLong oz_USERSTACKOVF = OZ_USERSTACKOVF;

const uLong oz_HW_QUICKIES                = OZ_HW_QUICKIES;
const uLong oz_LDR_PARAMS_SIZ             = OZ_LDR_PARAMS_SIZ;
const uLong oz_MAPSECTION_ATEND           = OZ_MAPSECTION_ATEND;
const uLong oz_MAPSECTION_EXACT           = OZ_MAPSECTION_EXACT;
const uLong oz_PROCMODE_KNL               = OZ_PROCMODE_KNL;
const uLong oz_PROCMODE_USR               = OZ_PROCMODE_USR;
const uLong oz_SECTION_EXPUP              = OZ_SECTION_EXPUP;
const uLong oz_SECTION_PAGESTATE_PAGEDOUT = OZ_SECTION_PAGESTATE_PAGEDOUT;
const uLong oz_SECTION_PAGESTATE_VALID_R  = OZ_SECTION_PAGESTATE_VALID_R;
const uLong oz_SECTION_PAGESTATE_VALID_W  = OZ_SECTION_PAGESTATE_VALID_W;
const uLong oz_SECTION_TYPE_PAGTBL        = OZ_SECTION_TYPE_PAGTBL;
const uLong oz_SECTION_TYPE_ZEROES        = OZ_SECTION_TYPE_ZEROES;
const uLong oz_TIMER_RESOLUTION           = OZ_TIMER_RESOLUTION;

/* Pointer to fields in loader param block needed by assembler routines */

uLong *const oz_hw486_ldr_clock_rate_ptr = &oz_s_loadparams.clock_rate;

/* Convert integer to asciz string */

void oz_hw_itoa (uLong valu, uLong size, char *buff)

{
  char temp[3*sizeof valu];
  int i;

  i = sizeof temp;
  temp[--i] = 0;
  do {
    temp[--i] = (valu % 10) + '0';
    valu /= 10;
  } while (valu != 0);
  strncpyz (buff, temp + i, size);
}

/* Convert integer to hexadecimal string */

void oz_hw_ztoa (uLong valu, uLong size, char *buff)

{
  char temp[3*sizeof valu];
  int i;

  i = sizeof temp;
  temp[--i] = 0;
  do {
    temp[--i] = (valu % 16) + '0';
    if (temp[i] > '9') temp[i] += 'A' - '9' - 1;
    valu /= 16;
  } while (valu != 0);
  strncpyz (buff, temp + i, size);
}

/* Convert string to decimal integer */

uLong oz_hw_atoi (const char *s, int *usedup)

{
  char c;
  const char *p;
  uLong accum;

  p = s;
  if ((p[0] == '0') && ((p[1] == 'x') || (p[1] == 'X'))) {
    accum = oz_hw_atoz (p + 2, usedup);
    if (usedup != NULL) *usedup += 2;
    return (accum);
  }

  accum = 0;
  for (; (c = *p) != 0; p ++) {
    if (c < '0') break;
    if (c > '9') break;
    accum = accum * 10 + c - '0';
  }

  if (usedup != NULL) *usedup = p - s;
  return (accum);
}

/* Convert string to hexadecimal integer */

uLong oz_hw_atoz (const char *s, int *usedup)

{
  char c;
  const char *p;
  uLong accum;

  accum = 0;
  for (p = s; (c = *p) != 0; p ++) {
    if ((c >= 'A') && (c <= 'F')) c -= 'A' - 10;
    else if ((c >= 'a') && (c <= 'f')) c -= 'a' - 10;
    else if ((c < '0') || (c > '9')) break;
    else c -= '0';
    accum = accum * 16 + c;
  }

  if (usedup != NULL) *usedup = p - s;
  return (accum);
}

/* Get rtc timezone conversion factor */

Long oz_hw_getrtcoffset (void)

{
  return (oz_s_loadparams.tz_offset_rtc);
}

/* Get uniprocessor mode flag */

int oz_hw_getuniprocessor (void)

{
  return (oz_s_loadparams.uniprocessor);
}

/* Print out the machine arguments */

uLong oz_hw_mchargs_print (uLong (*entry) (void *param, const char *format, ...), void *param, int full, OZ_Mchargs *mchargs)

{
  uLong sts;

  if (full) {
    sts = (*entry) (param, "    eip=%8.8x  ec1=%8.8x  ec2=%8.8x\n"
                           "    eax=%8.8x  ebx=%8.8x  ecx=%8.8x  edx=%8.8x\n"
                           "    esi=%8.8x  edi=%8.8x  ebp=%8.8x  esp=%8.8x\n"
                           "     ef=%8.8x   cs=%4.4x\n", 
                                           mchargs -> eip, mchargs -> ec1, mchargs -> ec2, 
                           mchargs -> eax, mchargs -> ebx, mchargs -> ecx, mchargs -> edx, 
                           mchargs -> esi, mchargs -> edi, mchargs -> ebp, mchargs -> esp, 
                           mchargs -> eflags, mchargs -> cs);
  } else {
    sts = (*entry) (param, "    %8.8x  %8.8x\n", mchargs -> eip, mchargs -> ebp);
  }
  return (sts);
}

/* Machine arguments (standard and extended) descriptors (for the debugger) */

static const OZ_Mchargs *mchargs_proto;
const OZ_Debug_mchargsdes oz_hw_mchargs_des[] = {
	OZ_DEBUG_MD (mchargs_proto, "ec2", ec2), 
	OZ_DEBUG_MD (mchargs_proto, "ec1", ec1), 
	OZ_DEBUG_MD (mchargs_proto, "cs",  cs), 
	OZ_DEBUG_MD (mchargs_proto, "edi", edi), 
	OZ_DEBUG_MD (mchargs_proto, "esi", esi), 
	OZ_DEBUG_MD (mchargs_proto, "esp", esp), 
	OZ_DEBUG_MD (mchargs_proto, "ebp", ebp), 
	OZ_DEBUG_MD (mchargs_proto, "eip", eip), 
	OZ_DEBUG_MD (mchargs_proto, "eax", eax), 
	OZ_DEBUG_MD (mchargs_proto, "ebx", ebx), 
	OZ_DEBUG_MD (mchargs_proto, "ecx", ecx), 
	OZ_DEBUG_MD (mchargs_proto, "edx", edx), 
	OZ_DEBUG_MD (mchargs_proto, "ef",  eflags), 
	NULL, 0, 0 };

static const OZ_Mchargx_knl *mchargx_knl_proto;
const OZ_Debug_mchargsdes oz_hw_mchargx_knl_des[] = {
	OZ_DEBUG_MD (mchargx_knl_proto, "ds",  ds), 
	OZ_DEBUG_MD (mchargx_knl_proto, "es",  es), 
	OZ_DEBUG_MD (mchargx_knl_proto, "fs",  fs), 
	OZ_DEBUG_MD (mchargx_knl_proto, "gs",  gs), 
	OZ_DEBUG_MD (mchargx_knl_proto, "ss",  ss), 
	OZ_DEBUG_MD (mchargx_knl_proto, "cr0", cr0), 
	OZ_DEBUG_MD (mchargx_knl_proto, "cr2", cr2), 
	OZ_DEBUG_MD (mchargx_knl_proto, "cr3", cr3), 
	OZ_DEBUG_MD (mchargx_knl_proto, "cr4", cr4), 
	NULL, 0, 0 };

static const OZ_Mchargx_usr *mchargx_usr_proto;
const OZ_Debug_mchargsdes oz_hw_mchargx_usr_des[] = {
	OZ_DEBUG_MD (mchargx_usr_proto, "ds",  ds), 
	OZ_DEBUG_MD (mchargx_usr_proto, "es",  es), 
	OZ_DEBUG_MD (mchargx_usr_proto, "fs",  fs), 
	OZ_DEBUG_MD (mchargx_usr_proto, "gs",  gs), 
	OZ_DEBUG_MD (mchargx_usr_proto, "ss",  ss), 
	NULL, 0, 0 };

