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
/*  Kernel debugger							*/
/*									*/
/************************************************************************/

#define _OZ_SYS_DEBUG_C

#include "ozone.h"
#include "oz_knl_debug.h"
#include "oz_knl_hw.h"
#include "oz_knl_procmode.h"
#include "oz_knl_sdata.h"
#include "oz_knl_status.h"
#include "oz_sys_debug.h"
#include "oz_sys_xprintf.h"

static Long primarycpu;
static volatile Long breaklock = 0;
static volatile Long printlock = 0;

static int crash_inprog = 0;
static int initialized  = 0;
static OZ_Mchargs *halted_mchargs[OZ_HW_MAXCPUS];
static OZ_Mchargx_knl *halted_mchargx_knl[OZ_HW_MAXCPUS];
static uByte dc[OZ_DEBUG_SIZE];

static void crash_dump (OZ_Sigargs *sigargs, OZ_Mchargs *mchargs, OZ_Mchargx_knl *mchargx_knl);

/************************************************************************/
/*									*/
/*  Callback routines							*/
/*									*/
/************************************************************************/

static Long kdb_getcur (void *cp) { return (oz_hw_cpu_getcur ()); }

static void kdb_print (void *cp, char *fmt, ...)

{
  va_list ap;

  va_start (ap, fmt);
  oz_knl_printkv (fmt, ap);
  va_end (ap);
}

static void kdb_abort (void *cp, OZ_Sigargs *sigargs, OZ_Mchargs *mchargs, void *mchargx)

{
  char buff[4];
  static const char *prmt = "oz_knl_debug abort: press ^Z to reboot, CR to crashdump> ";

  if (oz_hw_getcon (sizeof buff, buff, strlen (prmt), prmt)) crash_dump (sigargs, mchargs, mchargx);
  oz_hw_reboot ();
}

static int kdb_readbpt (void *cp, OZ_Breakpoint *bptaddr, OZ_Breakpoint *oldcontents)

{
  if (!OZ_HW_READABLE (sizeof *bptaddr, bptaddr, OZ_PROCMODE_KNL)) return (0);
  *oldcontents = *bptaddr;
  return (1);
}

static char const *kdb_writebpt (void *cp, OZ_Breakpoint *bptaddr, OZ_Breakpoint opcode) { return (oz_hw_debug_writebpt (bptaddr, opcode)); }

static void kdb_halt (void *cp) { oz_hw_debug_halt (); }

static int kdb_debugchk (void *cp) { return ((oz_hw_cpu_getcur () == primarycpu) && oz_knl_console_debugchk ()); }

static int kdb_getcon (void *cp, uLong size, char *buff)

{
  char prompt[24];

  oz_sys_sprintf (sizeof prompt, prompt, "\noz_knl_debug %d> ", oz_hw_cpu_getcur ());
  return (oz_hw_getcon (size, buff, strlen (prompt), prompt));
}

static void kdb_printaddr (void *cp, void *addr) { oz_knl_printkaddr (addr); }

static int kdb_printinstr (void *cp, uByte *pc)

{
  int len;

  len = 1 << OZ_HW_L2PAGESIZE;						/* assume instruction is less than a page long */
  if (!OZ_HW_READABLE (len, pc, OZ_PROCMODE_KNL)) {			/* see if that much is readable */
    len -= (len - 1) & (OZ_Pointer)pc;					/* it isn't, then just use to the end of the page it is in */
    if (!OZ_HW_READABLE (len, pc, OZ_PROCMODE_KNL)) return (-1);	/* hopefully that is readable */
  }
  len = oz_knl_printinstr (len, pc, pc);				/* ok, decode and print the instruction */
  return (len);								/* return how many bytes the opcode took up */
}

static int kdb_cvtsymbol (void *cp, char *name, OZ_Pointer *symaddr, int *symsize)

{
  char *colon, *symname;
  const char *found;
  int l;
  OZ_Image *image;
  void *sym;

  *symsize = 0;

  /* If system process not set up, return failure */

  if (oz_s_systemproc == NULL) return (0);

  /* If no colon given, scan all symbol tables */

  colon = strchr (name, ':');
  if (colon == NULL) {
    found = NULL;									// haven't found it anywhere yet
    for (image = NULL; (image = oz_knl_image_next (image, 1)) != NULL;) {		// scan list of images
      if (oz_knl_image_lookup (image, name, symaddr)) {					// lookup symbol in that image
        if (found != NULL) {								// see if it was found somewhere else, too
          oz_knl_printk ("oz_knl_debug: %s found in %s and %s\n", name, found, oz_knl_image_name (image));
          return (0);									// if so, fail
        }
        found = oz_knl_image_name (image);						// remember image name it was found in
      }
    }
    if (found != NULL) return (1);							// if it was found, successful
    oz_knl_printk ("oz_knl_debug: %s not found in any image\n", name);			// else, fail
    return (0);
  }

  /* Colon given, just scan that image's symbol table */

  l = colon - name;									// get length of image name
  for (image = NULL; (image = oz_knl_image_next (image, 1)) != NULL;) {			// scan list of images
    found = oz_knl_image_name (image);							// get image name
    if ((found[l] != 0) && (found[l] != '.')) continue;					// must be of correct length
    if (memcmp (found, name, l) == 0) break;						// characters must match
  }
  if (image == NULL) {
    oz_knl_printk ("oz_knl_debug: image %*.*s not found\n", l, l, name);
    return (0);
  }
  if (oz_knl_image_lookup (image, colon + 1, symaddr)) return (1);			// lookup symbol in that image
  oz_knl_printk ("oz_knl_debug: symbol %s not found in image %*.*s\n", colon + 1, l, l, name);
  return (0);
}

static int kdb_readmem (void *cp, void *buff, uLong size, void *addr)

{
  if (!OZ_HW_READABLE (size, addr, OZ_PROCMODE_KNL)) return (0);
  if (size == 1) *(uByte *)buff = *(uByte *)addr;
  else if ((size == 2) && ((((OZ_Pointer)addr) & 1) == 0)) *(uWord *)buff = *(uWord *)addr;
  else if ((size == 4) && ((((OZ_Pointer)addr) & 3) == 0)) *(uLong *)buff = *(uLong *)addr;
  else if ((size == 8) && ((((OZ_Pointer)addr) & 7) == 0)) *(uQuad *)buff = *(uQuad *)addr;
  else memcpy (buff, addr, size);
  return (1);
}

static int kdb_writemem (void *cp, void *buff, uLong size, void *addr)

{
  OZ_Mchargx_knl *mchargx_knl;

  if (!OZ_HW_WRITABLE (size, addr, OZ_PROCMODE_KNL)) return (0);

  mchargx_knl = halted_mchargx_knl[oz_hw_cpu_getcur()];
  if (((OZ_Pointer)addr >= (OZ_Pointer)mchargx_knl) 
   && (((OZ_Pointer)addr) + size <= (OZ_Pointer)(mchargx_knl + 1))) {
    memset (((uByte *)addr) + sizeof mchargx_knl[0], -1, size);
  }

  if (size == 1) *(uByte *)addr = *(uByte *)buff;
  else if ((size == 2) && ((((OZ_Pointer)addr) & 1) == 0)) *(uWord *)addr = *(uWord *)buff;
  else if ((size == 4) && ((((OZ_Pointer)addr) & 3) == 0)) *(uLong *)addr = *(uLong *)buff;
  else if ((size == 8) && ((((OZ_Pointer)addr) & 7) == 0)) *(uQuad *)addr = *(uQuad *)buff;
  else memcpy (addr, buff, size);
  return (1);
}

static void kdb_lockbreak (void *cp) { do {} while (oz_hw_atomic_set_long (&breaklock, 1) != 0); }

static void kdb_lockprint (void *cp) { do {} while (oz_hw_atomic_set_long (&printlock, 1) != 0); }

static void kdb_unlkbreak (void *cp) { breaklock = 0; }

static void kdb_unlkprint (void *cp) { printlock = 0; }

static void kdb_suspend (void *cp) {}

static void kdb_resume (void *cp) {}

static const OZ_Debug_callback cb = {
	kdb_getcur, 
	kdb_print, 
	kdb_abort, 
	kdb_readbpt, 
	kdb_writebpt, 
	kdb_halt, 
	kdb_debugchk, 
	kdb_getcon, 
	kdb_printaddr, 
	kdb_printinstr, 
	kdb_cvtsymbol, 
	kdb_readmem, 
	kdb_writemem, 
	kdb_lockbreak, 
	kdb_lockprint, 
	kdb_unlkbreak, 
	kdb_unlkprint, 
	kdb_suspend, 
	kdb_resume, 
	oz_hw_mchargs_des, 
	oz_hw_mchargx_knl_des, 
	sizeof (OZ_Mchargx_knl)
};

/************************************************************************/
/*									*/
/*  Boot-time initialization routine					*/
/*									*/
/************************************************************************/

void oz_knl_debug_init (void)

{
  char prompt[24];
  int rc;

  memset (halted_mchargs,     0, sizeof halted_mchargs);
  memset (halted_mchargx_knl, 0, sizeof halted_mchargx_knl);

  primarycpu = oz_hw_cpu_getcur ();
  oz_sys_sprintf (sizeof prompt, prompt, "oz_knl_debug %d", primarycpu);
  rc = oz_sys_debug_init (prompt, &cb, NULL, sizeof dc, (void *)dc);
  if (rc != 0) oz_crash ("oz_knl_debug_init: dc size is %d, should be %d", sizeof dc, rc);
  initialized = 1;
  if (oz_s_loadparams.debug_init) oz_hw_debug_bpt ();
}

/************************************************************************/
/*									*/
/*  This routine is called as the result of an exception		*/
/*									*/
/*    Input:								*/
/*									*/
/*	sigargs = signal args for the exception				*/
/*	          or NULL if called via control-shift-C			*/
/*	mchargs = machine args at time of exception/interrupt		*/
/*									*/
/*	hardware interrupt delivery inhibited				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_debug_exception = 0 : do normal exception processing	*/
/*	                         1 : return to mchargs to try again	*/
/*	                             (mchargs might be modified)	*/
/*									*/
/************************************************************************/

int oz_knl_debug_exception (OZ_Sigargs *sigargs, OZ_Mchargs *mchargs)

{
  char prompt[24];
  int rc;
  Long cpuidx;
  OZ_Mchargx_knl mchargx_knl[2];

  /* Get the extended kernel machine arguments asap */

  oz_hw_mchargx_knl_fetch (mchargx_knl + 0);

  /* Initialize if we aren't already initialized */

  if (!initialized) {
    oz_s_loadparams.debug_init = 0;
    oz_knl_debug_init ();
  }

  /* If knl_exception sysgen parameter is 1 or 2, don't go to the debugger */

  if (sigargs != NULL) switch (oz_s_loadparams.knl_exception) {
    case 1: {
      oz_s_loadparams.knl_exception = 0;			/* in case of exception writing dump */
      crash_dump (sigargs, mchargs, mchargx_knl + 0);
      break;							/* dump failed, go to debugger */
    }
    case 2: {
      if (sigargs != NULL) {
        oz_knl_printk ("oz_knl_debug_exception: exception %u at %p\n", sigargs[1], GETNXTINSAD ((*mchargs)));
      }
      oz_knl_printk ("oz_knl_debug_exception: kernel exception, rebooting in 10 seconds ...\n");
      oz_hw_stl_microwait (10000000, NULL, NULL);		/* pause for a few seconds with message on screen */
      oz_hw_reboot ();						/* reboot the system */
    }
  }

  /* Any other value goes to the debugger */

  cpuidx = oz_hw_cpu_getcur ();
  oz_hw_mchargx_knl_fetch (mchargx_knl + 0);
  halted_mchargx_knl[cpuidx] = mchargx_knl + 0;
  memset (mchargx_knl + 1, 0, sizeof mchargx_knl[1]);

  oz_sys_sprintf (sizeof prompt, prompt, "oz_knl_debug %d", oz_hw_cpu_getcur ());
  rc = oz_sys_debug_exception (prompt, (void *)dc, sigargs, mchargs, mchargx_knl + 0);
  oz_hw_mchargx_knl_store (mchargx_knl + 0, mchargx_knl + 1);
  halted_mchargx_knl[cpuidx] = NULL;
  return (rc);
}

static void crash_dump (OZ_Sigargs *sigargs, OZ_Mchargs *mchargs, OZ_Mchargx_knl *mchargx_knl)

{
  Long cpuidx;

  if (oz_hw_cpu_smplevel () == OZ_SMPLOCK_NULL) oz_hw_cpu_setsoftint (0);	/* crash dump requires softints inhibited */
  if (sigargs != NULL) {
    oz_knl_printk ("oz_knl_debug_exception: exception %u at %p\n", sigargs[1], GETNXTINSAD ((*mchargs)));
  }
  crash_inprog = 1;								/* make other cpu's hang in a loop */
  OZ_HW_MB;									/* make sure they see that crash_inprog flag */
  oz_hw_debug_halt ();								/* force them to hang in a loop */
  oz_hw_stl_microwait (100000, NULL, NULL);					/* wait 100mS for them to hang */
  cpuidx = oz_hw_cpu_getcur ();							/* see which cpu is crashing */
  oz_knl_printk ("oz_knl_debug_exception: writing crash dump on cpu %d ...\n", cpuidx);
  halted_mchargs[cpuidx]     = mchargs;						/* save mchargs/mchargx_knl pointers for the cpu */
  halted_mchargx_knl[cpuidx] = mchargx_knl;
  if (oz_knl_crash_dump (cpuidx, sigargs, halted_mchargs, halted_mchargx_knl)) { /* do the crash dump */
    oz_knl_printk ("oz_knl_debug_exception: rebooting in 10 seconds ...");
    oz_hw_stl_microwait (10000000, NULL, NULL);
    oz_hw_reboot ();								/* ... then reboot */
  }
  oz_knl_printk ("oz_knl_debug_exception: crash dump failed\n");
  oz_hw_stl_microwait (3000000, NULL, NULL);
}

/************************************************************************/
/*									*/
/*  This routine is called by the other cpus in response to an 		*/
/*  oz_hw_debug_halt call						*/
/*									*/
/*    Input:								*/
/*									*/
/*	mchargs = machine arguments at time of interrupt		*/
/*									*/
/*	hardware interrupt delivery inhibited				*/
/*									*/
/************************************************************************/

void oz_knl_debug_halted (OZ_Mchargs *mchargs)

{
  char prompt[24];
  Long cpuidx;
  OZ_Mchargx_knl mchargx_knl[2];

  /* Save pointers to mchargs/mchargx_knl if we're crash dumping */

  if (crash_inprog) {
    cpuidx = oz_hw_cpu_getcur ();
    oz_hw_mchargx_knl_fetch (mchargx_knl + 0);
    OZ_HW_MB;
    halted_mchargs[cpuidx]     = mchargs;
    halted_mchargx_knl[cpuidx] = mchargx_knl + 0;
    oz_knl_printk ("oz_knl_debug_halted: cpu %d halting\n", cpuidx);
    oz_hw_halt ();
  }

  /* Not doing a crash dump, process through the debugger */

  cpuidx = oz_hw_cpu_getcur ();
  oz_hw_mchargx_knl_fetch (mchargx_knl + 0);
  halted_mchargx_knl[cpuidx] = mchargx_knl + 0;
  memset (mchargx_knl + 1, 0, sizeof mchargx_knl[1]);

  oz_sys_sprintf (sizeof prompt, prompt, "oz_knl_debug %d", cpuidx);
  oz_sys_debug_halted (prompt, (void *)dc, mchargs, mchargx_knl + 0);
  oz_hw_mchargx_knl_store (mchargx_knl + 0, mchargx_knl + 1);
  halted_mchargx_knl[cpuidx] = NULL;
}
