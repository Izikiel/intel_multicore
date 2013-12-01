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
/*  Debugger								*/
/*									*/
/************************************************************************/

#define _OZ_SYS_DEBUG_C

#include "ozone.h"
#include "oz_knl_hw.h"
#include "oz_knl_procmode.h"
#include "oz_knl_status.h"
#include "oz_sys_debug.h"
#include "oz_sys_xprintf.h"

typedef const char *ccharp;

struct OZ_Debug {
	const OZ_Debug_callback *cb;		/* callback table pointer */
	void *cp;				/* callback routine parameter */
	Long breaklock;				/* synchronize access to 'bptsinstalled' */
	Long printlock;				/* neatens output when several cpu's halt at once */
	int bptsinstalled;			/* set if breakpoints are installed */
	volatile Long cpushalted;		/* bitmask of halted cpu's */
	volatile Long commandcpu;		/* which cpu is prompting now */
	int allrunning;				/* set if all cpu's supposed to be running */
	OZ_Breakpoint *breakaddr[OZ_DEBUG_NBREAKS]; /* address of breakpoints */
	OZ_Breakpoint breakdata[OZ_DEBUG_NBREAKS]; /* the saved breakpoint opcodes */
	OZ_Pointer nextstep;			/* saved stack from a 'next' command */
	OZ_Breakpoint *contstepbreak;		/* if a 'continue' command was given at a pc that has a breakpoint, */
						/* this is set to the address where the breakpoint goes. */
						/* Then the single cpu is stepped so as to execute that one instruction, */
						/* ... then the breakpoint is restored */
	int contstepresume;			/* set if all cpu's are to be resumed by a 'continue' command */
};

#define LOCKBREAK() (dc -> cb -> lockbreak) (dc -> cp)
#define LOCKPRINT() (dc -> cb -> lockprint) (dc -> cp)
#define UNLKBREAK() (dc -> cb -> unlkbreak) (dc -> cp)
#define UNLKPRINT() (dc -> cb -> unlkprint) (dc -> cp)

static void command (OZ_Debug *dc, OZ_Sigargs *sigargs, OZ_Mchargs *mchargs, void *mchargx, const char *prompt);
static char *getaddrexp (OZ_Debug *dc, char *cmdbuf, int *varsize, OZ_Pointer *varaddr, OZ_Mchargs *mchargs, void *mchargx, const char *prompt);
static char *getexp (OZ_Debug *dc, char *cmdbuf, int *varsize, OZ_Pointer *varaddr, OZ_Mchargs *mchargs, void *mchargx, const char *prompt);
static char *getoperand (OZ_Debug *dc, char *cmdbuf, int *varsize, OZ_Pointer *varaddr, OZ_Mchargs *mchargs, void *mchargx, const char *prompt);
static void removebreaks (OZ_Debug *dc, const char *prompt);
static int disassemble (OZ_Debug *dc, OZ_Pointer varaddr, OZ_Mchargs *mchargs, void *mchargx, OZ_Pointer *varsize_r);
static void traceback (OZ_Debug *dc, OZ_Mchargs *mchargs, void *mchargx, int framecount);
static void printstring (OZ_Debug *dc, int outsize, int varsize, OZ_Pointer varaddr);
static void printaddr (OZ_Debug *dc, OZ_Pointer varaddr, OZ_Mchargs *mchargs, void *mchargx);
static void dumpmem (OZ_Debug *dc, int size, void *addr);

/************************************************************************/
/*									*/
/*  Boot-time initialization routine					*/
/*									*/
/*    Input:								*/
/*									*/
/*	cb = pointer to callback table					*/
/*	cp = callback parameter						*/
/*	dc_size = size of allocated context block			*/
/*	dc = context block pointer					*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_debug_init = 0 : successful				*/
/*	                 else : dc_size must be at least this big	*/
/*									*/
/************************************************************************/

int oz_sys_debug_init (const char *prompt, const OZ_Debug_callback *cb, void *cp, int dc_size, OZ_Debug *dc)

{
  if (dc_size < sizeof *dc) return (sizeof *dc);			/* make sure supplied context block is big enough */
  memset (dc, 0, dc_size);						/* clear the whole thing out to zeroes */
  dc -> cb = cb;							/* save callback block pointer */
  dc -> cp = cp;							/* save callback parameter */
  dc -> allrunning = 1;							/* say all the threads are running */
  dc -> commandcpu = (dc -> cb -> getcur) (dc -> cp);			/* save this as the initial command cpu */
  (dc -> cb -> print) (dc -> cp, "%s: initialized (cb %p, cp %p, dc %p)\n", prompt, cb, cp, dc); /* print a successful init message */
  return (0);								/* successful */
}

/************************************************************************/
/*									*/
/*  This routine is called as the result of an exception		*/
/*									*/
/*    Input:								*/
/*									*/
/*	dc = debug context pointer					*/
/*	sigargs = signal args for the exception				*/
/*	          or NULL if called via control-shift-C			*/
/*	          [in this system's address space]			*/
/*	mchargs = machine args at time of exception/interrupt		*/
/*	          [in target's address space]				*/
/*	mchargx = extended machine args (kernel or user)		*/
/*	          [in target's address space]				*/
/*									*/
/*	hardware interrupt delivery inhibited				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_debug_exception = 0 : do normal exception processing	*/
/*	                         1 : return to mchargs			*/
/*	                             (mchargs might be modified)	*/
/*									*/
/************************************************************************/

int oz_sys_debug_exception (const char *prompt, OZ_Debug *dc, OZ_Sigargs *sigargs, OZ_Mchargs *mchargs, void *mchargx)

{
  char *p;
  int hitbreak, i;
  Long cpuidx, cpumsk, washalted;
  OZ_Mchargs mchargi;

  /* If we consider this cpu to already be halted, then it is a fatal error (an exception occurred during internal processing) */

  cpuidx = (dc -> cb -> getcur) (dc -> cp);						/* get my cpu index */
  cpumsk = 1 << cpuidx;									/* make a mask bit out of it */
  washalted = oz_hw_atomic_or_long (&(dc -> cpushalted), cpumsk);			/* set the bit and read the old value atomically */
  if (washalted & cpumsk) {								/* see if the bit was already set */
    if (sigargs == NULL) return (1);							/* if so, and it's a control-shift-C, just ignore it */
    if (oz_hw_atomic_or_long (&(dc -> cpushalted), -1) != -1) {
      (dc -> cb -> print) (dc -> cp, "%s: exception while halted\n", prompt);		/* else, output error message */
      (dc -> cb -> print) (dc -> cp, "%s:   sigargs:\n", prompt);			/* output any sigargs */
      dumpmem (dc, (sigargs[0] + 1) * sizeof *sigargs, sigargs);
      (dc -> cb -> print) (dc -> cp, "%s:   mchargs:\n", prompt);			/* ... and mchargs */
      dumpmem (dc, sizeof *mchargs, mchargs);
      (dc -> cb -> print) (dc -> cp, "%s:   stack:\n", prompt);				/* ... and a few lines of stack */
      dumpmem (dc, 0x100, mchargs + 1);
      traceback (dc, mchargs, mchargx, 2000000000);					/* ... and a traceback */
    }
    while (1) (dc -> cb -> abort) (dc -> cp, sigargs, mchargs, mchargx);		/* ... then abort */
  }

  /* Read the mchargs so we can do various tests */

  if (!(dc -> cb -> readmem) (dc -> cp, &mchargi, sizeof mchargi, mchargs)) {
    (dc -> cb -> print) (dc -> cp, "%s: couldn't read mchargs at %p\n", prompt, mchargs);
    goto process_commands;
  }

  /* If this is a result of a single-step done by a next command, */
  /* make sure we have returned to the same stack                 */
  /* But stop if we are at a breakpoint address                   */

  if ((sigargs != NULL) && (sigargs[1] == OZ_SINGLESTEP) && TSTSINGSTEP (mchargi) && (dc -> nextstep != 0)) {
    if (!CMPSTACKPTR (mchargi, dc -> nextstep)) {
      for (i = 0; i < OZ_DEBUG_NBREAKS; i ++) if (dc -> breakaddr[i] == GETNXTINSAD (mchargi)) goto nextstepstop;
      goto rtn1;
nextstepstop:;
    }
  }

  /* If this is the result of a single-step done by a continue command, */
  /* insert the breakpoints and resume execution where we left off      */

  if ((sigargs != NULL) && (sigargs[1] == OZ_SINGLESTEP) && TSTSINGSTEP (mchargi) && (dc -> contstepbreak != NULL)) {
    for (i = 0; i < OZ_DEBUG_NBREAKS; i ++) {
      if (dc -> breakaddr[i] == dc -> contstepbreak) {
        if (!(dc -> cb -> readbpt) (dc -> cp, dc -> contstepbreak, &(dc -> breakdata[i]))) {
          (dc -> cb -> print) (dc -> cp, "%s: cannot insert breakpoint %u at %p: it is not readable\n", prompt, i, dc -> contstepbreak);
          goto haltothers;
        }
      }
    }

    p = (dc -> cb -> writebpt) (dc -> cp, dc -> contstepbreak, OZ_OPCODE_BPT);
    if (p != NULL) {
      (dc -> cb -> print) (dc -> cp, "%s: cannot insert breakpoint at %p: %s\n", prompt, dc -> contstepbreak, p);
      goto haltothers;
    }

    dc -> contstepbreak = NULL;
    CLRSINGSTEP (mchargi);
    if (!(dc -> cb -> writemem) (dc -> cp, &mchargi, sizeof mchargi, mchargs)) {
      (dc -> cb -> print) (dc -> cp, "%s: couldn't clear single-step mode\n", prompt);
      goto haltothers;
    }
    if (dc -> contstepresume) {
      dc -> cpushalted = 0;
      (dc -> cb -> resume) (dc -> cp);
    }
    goto rtn1;
  }
haltothers:

  /* Special stuff if this one is first to halt */

  if (washalted == 0) {										/* see if any others were halted */
    if (oz_hw_atomic_set_long (&(dc -> allrunning), 0) > 0) (dc -> cb -> halt) (dc -> cp);	/* if not and there are others running, halt them */
    dc -> commandcpu = cpuidx;									/* anyway, make this one the 'command cpu' */
    removebreaks (dc, prompt);									/* clear out all the breakpoints */
  }

  /* Back up the PC to the breakpoint instruction */

  hitbreak = -1;									/* assume we did not hit a known breakpoint */
  if ((sigargs != NULL) && (sigargs[1] == OZ_BREAKPOINT)) {				/* see if sigargs say we might have */
    for (i = 0; i < OZ_DEBUG_NBREAKS; i ++) {						/* ok, loop through table of known breakpoints */
      if (GETNXTINSAD (mchargi) - BPTBACKUP == dc -> breakaddr[i]) {			/* see if the address matches (after backing it up) */
        GETNXTINSAD (mchargi) = dc -> breakaddr[i];					/* ok, back up the next instruction address */
        if (!(dc -> cb -> writemem) (dc -> cp, &mchargi, sizeof mchargi, mchargs)) {
          (dc -> cb -> print) (dc -> cp, "%s: couldn't back up instruction pointer before breakpoint\n", prompt);
        }
        hitbreak = i;									/* remember which breakpoint it was */
        break;
      }
    }
  }

  /* Display message saying we're in here and why */

  LOCKPRINT ();
  (dc -> cb -> print) (dc -> cp, "\n%s: ", prompt);
  if (sigargs == NULL) (dc -> cb -> print) (dc -> cp, "console interrupt at");
  else if (hitbreak >= 0) (dc -> cb -> print) (dc -> cp, "breakpoint %d at", hitbreak);
  else if (TSTSINGSTEP (mchargi)) (dc -> cb -> print) (dc -> cp, "stepped to");
  else (dc -> cb -> print) (dc -> cp, "exception %u at", sigargs[1]);
  (dc -> cb -> print) (dc -> cp, " %p\n  (", GETNXTINSAD (mchargi));
  disassemble (dc, (OZ_Pointer)GETNXTINSAD (mchargi), mchargs, mchargx, NULL);
  (dc -> cb -> print) (dc -> cp, ")\n");
  UNLKPRINT ();

  /* Process commands */

process_commands:
  command (dc, sigargs, mchargs, mchargx, prompt);

  /* Resume execution */

  if (!(dc -> cb -> readmem) (dc -> cp, &mchargi, sizeof mchargi, mchargs)) {
    (dc -> cb -> print) (dc -> cp, "%s: couldn't reread mchargs at %p\n", prompt, mchargs);
  } else {
    LOCKPRINT ();
    (dc -> cb -> print) (dc -> cp, "%s: resuming at %p\n  (", prompt, GETNXTINSAD (mchargi));
    disassemble (dc, (OZ_Pointer)GETNXTINSAD (mchargi), mchargs, mchargx, NULL);
    (dc -> cb -> print) (dc -> cp, ")\n");
    UNLKPRINT ();
  }

rtn1:
  oz_hw_atomic_and_long (&(dc -> cpushalted), ~cpumsk);
  return (1);
}

/************************************************************************/
/*									*/
/*  This routine is called by the other cpus in response to an 		*/
/*  (dc -> cb -> halt) call						*/
/*									*/
/*    Input:								*/
/*									*/
/*	mchargs = machine arguments at time of interrupt		*/
/*	          [in target's address space]				*/
/*	mchargx = extended machine arguments				*/
/*	          [in target's address space]				*/
/*									*/
/*	hardware interrupt delivery inhibited				*/
/*									*/
/************************************************************************/

void oz_sys_debug_halted (const char *prompt, OZ_Debug *dc, OZ_Mchargs *mchargs, void *mchargx)

{
  Long cpuidx, cpumsk, washalted;
  OZ_Mchargs mchargi;

  /* If this cpu is already in a halted state, just return back out */
  /* Otherwise, mark it halted                                      */

  cpuidx = (dc -> cb -> getcur) (dc -> cp);				/* get my cpu index */
  cpumsk = 1 << cpuidx;							/* make a mask out of it */
  washalted = oz_hw_atomic_or_long (&(dc -> cpushalted), cpumsk);	/* say I am now halted, see if I already was */
  if (washalted & cpumsk) return;					/* return if already halted, just a nested call via dc -> cb -> halt routine */

  /* Special stuff if this one is first to halt */

  if (washalted == 0) {							/* see if anyone else was halted */
    if (oz_hw_atomic_set_long (&(dc -> allrunning), 0) > 0) (dc -> cb -> halt) (dc -> cp); /* if not, halt everyone else if they are running */
    dc -> commandcpu = cpuidx;						/* make this the command cpu */
    removebreaks (dc, prompt);						/* remove installed breakpoints */
  }

  /* Process commands until command says to let this cpu go */

  if (!(dc -> cb -> readmem) (dc -> cp, &mchargi, sizeof mchargi, mchargs)) {
    (dc -> cb -> print) (dc -> cp, "%s: couldn't read mchargs at %p\n", prompt, mchargs);
  } else {
    LOCKPRINT ();
    (dc -> cb -> print) (dc -> cp, "%s: halted at %p\n  (", prompt, GETNXTINSAD (mchargi));
    disassemble (dc, (OZ_Pointer)GETNXTINSAD (mchargi), mchargs, mchargx, NULL);
    (dc -> cb -> print) (dc -> cp, ")\n");
    UNLKPRINT ();
  }

  command (dc, NULL, mchargs, mchargx, prompt);

  if (!(dc -> cb -> readmem) (dc -> cp, &mchargi, sizeof mchargi, mchargs)) {
    (dc -> cb -> print) (dc -> cp, "%s: couldn't reread mchargs at %p\n", prompt, mchargs);
  } else {
    LOCKPRINT ();
    (dc -> cb -> print) (dc -> cp, "%s: resuming at %p\n  (", prompt, GETNXTINSAD (mchargi));
    disassemble (dc, (OZ_Pointer)GETNXTINSAD (mchargi), mchargs, mchargx, NULL);
    (dc -> cb -> print) (dc -> cp, ")\n");
    UNLKPRINT ();
  }

  /* Mark the cpu as no longer halted and return to resume execution where the mchargs say to */

  oz_hw_atomic_and_long (&(dc -> cpushalted), ~cpumsk);
}

/************************************************************************/
/*									*/
/*  This routine processes commands					*/
/*									*/
/*    Input:								*/
/*									*/
/*	mchargs = machine args at time of exception/interrupt		*/
/*	          [in target's address space]				*/
/*	mchargx = extended machine arguments				*/
/*	          [in target's address space]				*/
/*									*/
/*	dc -> commandcpu = which cpu is prompting commands		*/
/*									*/
/*    Output:								*/
/*									*/
/*	mchargs, mchargx = possibly modified				*/
/*	everything else = possibly modified				*/
/*									*/
/************************************************************************/

static void command (OZ_Debug *dc, OZ_Sigargs *sigargs, OZ_Mchargs *mchargs, void *mchargx, const char *prompt)

{
  uByte ubvalue;
  char c, cmdbuf[64], *p;
  int i, j;
  Long cpuidx, cpumsk;
  uLong varsize;
  OZ_Mchargs mchargi;
  OZ_Pointer varaddr, value;

  /* Get id number of 'this' cpu */

  cpuidx = (dc -> cb -> getcur) (dc -> cp);
  cpumsk = (1 << cpuidx);

  /* Wait until we are either the command cpu or are no longer halted */

commandloop:
  while ((cpuidx != dc -> commandcpu) && (dc -> cpushalted & cpumsk)) {			/* wait while this isn't the command cpu and we are to remain halted */
    if (!(dc -> cpushalted & (1 << dc -> commandcpu))) {				/* while waiting, if the command cpu isn't halted, */
      if ((dc -> cb -> debugchk) (dc -> cp)) {						/* ... check for control-shift-C */
        (dc -> cb -> print) (dc -> cp, "%s: halting other cpu(s)\n", prompt);		/* ... and if so, then try to halt any running cpu's */
        (dc -> cb -> halt) (dc -> cp);							/* ... so hopefully the command cpu will come back in here */
      }
    }
    (dc -> cb -> suspend) (dc -> cp);							/* wait for this cpu to be resumed */
  }

  /* If this cpu is no longer to remain halted, return out to execute */

  if (!(dc -> cpushalted & cpumsk)) return;

  /* This cpu is not halted and is the command cpu, read in the command */

  (dc -> cb -> getcon) (dc -> cp, sizeof cmdbuf, cmdbuf);
  if (cmdbuf[0] == 0) goto commandloop;

  /* Continue execution until breakpoint */

  if ((cmdbuf[0] == 'C') || (cmdbuf[0] == 'c')) {
    dc -> contstepbreak  = NULL;				/* assume there is no breakpoint at the resume pc */
    dc -> contstepresume = (cmdbuf[0] == 'C');			/* resume all cpu's if capital C */
    dc -> nextstep       = 0;					/* this is not a 'next' command */

    /* Insert breakpoints, except any that are at the next instruction address              */
    /* Do a read loop followed by a write in case the same address is listed more than once */

    if (!(dc -> cb -> readmem) (dc -> cp, &mchargi, sizeof mchargi, mchargs)) {
      (dc -> cb -> print) (dc -> cp, "%s: error reading mchargs at %p\n", mchargs);
      goto commandloop;
    }

    LOCKBREAK ();
    for (i = 0; i < OZ_DEBUG_NBREAKS; i ++) {
      if (dc -> breakaddr[i] != NULL) {
        if (!(dc -> cb -> readbpt) (dc -> cp, dc -> breakaddr[i], &(dc -> breakdata[i]))) {
          (dc -> cb -> print) (dc -> cp, "%s: cannot insert breakpoint %u at %p (it is not readable)\n", prompt, i, dc -> breakaddr[i]);
          goto commandloop;
        }
      }
    }

    for (i = 0; i < OZ_DEBUG_NBREAKS; i ++) {
      if (dc -> breakaddr[i] != NULL) {
        if (dc -> breakaddr[i] == GETNXTINSAD (mchargi)) {
          dc -> contstepbreak = dc -> breakaddr[i];
        } else {
          p = (dc -> cb -> writebpt) (dc -> cp, dc -> breakaddr[i], OZ_OPCODE_BPT);
          if (p != NULL) {
            (dc -> cb -> print) (dc -> cp, "%s: cannot insert breakpoint %u at %p (%s)\n", prompt, i, dc -> breakaddr[i], p);
            goto commandloop;
          }
        }
      }
    }
    dc -> bptsinstalled = 1;					/* remember breakpoints are installed */
    UNLKBREAK ();

    CLRSINGSTEP (mchargi);					/* clear single-step bit in machine args */
    if (dc -> contstepbreak != NULL) SETSINGSTEP (mchargi);	/* if resuming right on a breakpoint, do a single-step first */

    if (!(dc -> cb -> writemem) (dc -> cp, &mchargi, sizeof mchargi, mchargs)) {
      (dc -> cb -> print) (dc -> cp, "%s: error writing mchargs at %p\n", mchargs);
      goto commandloop;
    }

    if ((dc -> contstepbreak == NULL) && dc -> contstepresume) { /* otherwise, maybe release all cpu's now */
      dc -> allrunning = 1;
      dc -> cpushalted = 0;
      (dc -> cb -> resume) (dc -> cp);
    }
    return;
  }

  /* Deposit into memory location */

  if (cmdbuf[0] == 'd') {
    p = getaddrexp (dc, cmdbuf + 1, &varsize, &varaddr, mchargs, mchargx, prompt);
    if (p == NULL) goto commandloop;
    if (*p != '=') {
      (dc -> cb -> print) (dc -> cp, "%s: variable name must be followed by = at %s\n", prompt, p);
      goto commandloop;
    }
    if (varsize > sizeof value) {
      (dc -> cb -> print) (dc -> cp, "%s: size %x too large, max allowed %x\n", prompt, varsize, sizeof value);
      goto commandloop;
    }
    p = getexp (dc, p + 1, NULL, &value, mchargs, mchargx, prompt);
    if (p == NULL) goto commandloop;
    if (*p != 0) {
      (dc -> cb -> print) (dc -> cp, "%s: value must not be followed by anything (%s)\n", prompt, p);
      goto commandloop;
    }
    if (!(dc -> cb -> writemem) (dc -> cp, &value, varsize, (void *)varaddr)) {
      (dc -> cb -> print) (dc -> cp, "%s: location %p.%x is not writable\n", prompt, varaddr, varsize);
    }
    goto commandloop;
  }

  /* Examine memory location */

  if (cmdbuf[0] == 'e') {
    p = cmdbuf + 1;
    while (((c = *p) != 0) && (c <= ' ')) p ++;
    if (c == 0) {
      varaddr += varsize;
    } else {
      p = getaddrexp (dc, p, &varsize, &varaddr, mchargs, mchargx, prompt);
      if (p == NULL) goto commandloop;
      if (*p != 0) {
        (dc -> cb -> print) (dc -> cp, "%s: variable name must not be followed by anything (%s)\n", prompt, p);
        goto commandloop;
      }
    }
    if (varsize <= sizeof value) {
      if (!(dc -> cb -> readmem) (dc -> cp, &value, sizeof value, (void *)varaddr)) {
        (dc -> cb -> print) (dc -> cp, "\n%s: location %p.%u is not readable\n", prompt, varaddr, varsize);
        goto commandloop;
      }
      (dc -> cb -> print) (dc -> cp, "  ");					/* skip over two spaces */
      for (j = varsize; -- j >= 0;) {						/* loop for each byte on the line */
        ubvalue = value >> (j * 8);
        (dc -> cb -> print) (dc -> cp, "%2.2x", ubvalue);			/* else display the bytes */
      }
      (dc -> cb -> print) (dc -> cp, " : ");
      printstring (dc, varsize, varsize, varaddr);				/* print characters */
      (dc -> cb -> print) (dc -> cp, " : %8.8x : ", varaddr);			/* print address */
      printaddr (dc, varaddr, mchargs, mchargx);				/* try to print symbolic address */
      (dc -> cb -> print) (dc -> cp, "\n");
      goto commandloop;
    }
    for (i = 0; i < varsize; i += 16) {						/* max of 16 bytes per line */
      (dc -> cb -> print) (dc -> cp, "  ");					/* skip over two spaces */
      for (j = 16; -- j >= 0;) {						/* loop for each byte on the line */
        if ((j & 3) == 3) (dc -> cb -> print) (dc -> cp, " ");			/* put a space in each longword */
        if (i + j >= varsize) (dc -> cb -> print) (dc -> cp, "  ");		/* two spaces if beyond end of area to display */
        else {
          if (!(dc -> cb -> readmem) (dc -> cp, &ubvalue, sizeof ubvalue, (void *)(varaddr + i + j))) {
            (dc -> cb -> print) (dc -> cp, "\n%s: location %p is not readable\n", prompt, varaddr + i + j);
            goto commandloop;
          }
          (dc -> cb -> print) (dc -> cp, "%2.2x", ubvalue);			/* else display the bytes */
        }
      }
      j = 16;									/* at most 16 chars to print out */
      if (i + j >= varsize) j = varsize - i;					/* but limit to actual length */
      (dc -> cb -> print) (dc -> cp, " : ");
      printstring (dc, 16, j, varaddr + i);					/* print characters */
      (dc -> cb -> print) (dc -> cp, " : %8.8x\n", varaddr + i);		/* print address */
    }
    goto commandloop;
  }

  /* Help */

  if ((cmdbuf[0] == 'h') || (cmdbuf[0] == '?')) {
    (dc -> cb -> print) (dc -> cp, "  c                    continues this one cpu only\n");
    (dc -> cb -> print) (dc -> cp, "  C                    continues on all cpus\n");
    (dc -> cb -> print) (dc -> cp, "  d address=value      deposit in memory\n");
    (dc -> cb -> print) (dc -> cp, "  e address            examine memory\n");
    (dc -> cb -> print) (dc -> cp, "  i address            disassemble instructions\n");
    (dc -> cb -> print) (dc -> cp, "  n                    single-step over subroutine calls\n");
    (dc -> cb -> print) (dc -> cp, "  q                    quit execution (abort program)\n");
    (dc -> cb -> print) (dc -> cp, "  s                    single-step into subroutine calls\n");
    (dc -> cb -> print) (dc -> cp, "  t [count] [@mchargs] print traceback call stack\n");
    (dc -> cb -> print) (dc -> cp, "  w address            watch an address for modifications\n");
    (dc -> cb -> print) (dc -> cp, "  x cpunumber          switches command prompting cpu\n");
    (dc -> cb -> print) (dc -> cp, "\n");
    (dc -> cb -> print) (dc -> cp, "  expression operands are:\n");
    (dc -> cb -> print) (dc -> cp, "    <hexadecimal number>\n");
    (dc -> cb -> print) (dc -> cp, "    <kernel symbol>\n");
    (dc -> cb -> print) (dc -> cp, "    %%<mcharg symbol>  ");
    for (i = 0; dc -> cb -> mchargsdes[i].name != NULL; i ++) {
       (dc -> cb -> print) (dc -> cp, " %s", dc -> cb -> mchargsdes[i].name);
    }
    for (i = 0; dc -> cb -> mchargxdes[i].name != NULL; i ++) {
       (dc -> cb -> print) (dc -> cp, " %s", dc -> cb -> mchargxdes[i].name);
    }
    (dc -> cb -> print) (dc -> cp, "\n");
    (dc -> cb -> print) (dc -> cp, "    $<breakpoint number>   (range 0..%u)\n", OZ_DEBUG_NBREAKS - 1);
    (dc -> cb -> print) (dc -> cp, "\n");
    (dc -> cb -> print) (dc -> cp, "  addresses may be followed by .size to specify size\n");
    goto commandloop;
  }

  /* Disassemble memory locations */

  if (cmdbuf[0] == 'i') {
    p = cmdbuf + 1;
    while (((c = *p) != 0) && (c <= ' ')) p ++;
    varsize = 1;
    if (c != 0) {
      p = getaddrexp (dc, p, &varsize, &varaddr, mchargs, mchargx, prompt);
      if (p == NULL) goto commandloop;
      if (*p != 0) {
        (dc -> cb -> print) (dc -> cp, "%s: variable name must not be followed by anything (%s)\n", prompt, p);
        goto commandloop;
      }
    }
    while (varsize > 0) {
      if (!disassemble (dc, varaddr, mchargs, mchargx, &value)) {
        (dc -> cb -> print) (dc -> cp, "%s: location %p is not readable\n", prompt, varaddr);
        break;
      }
      (dc -> cb -> print) (dc -> cp, "\n");
      if (value >= varsize) break;
      varsize -= value;
      varaddr += value;
    }
    goto commandloop;
  }

  /* Single-step an instruction, not entering any subroutine called */

  if (cmdbuf[0] == 'n') {
    if (!(dc -> cb -> readmem) (dc -> cp, &mchargi, sizeof mchargi, mchargs)) {
      (dc -> cb -> print) (dc -> cp, "%s: error reading mchargs at %p\n", mchargs);
      goto commandloop;
    }
    dc -> contstepbreak = NULL;
    dc -> nextstep = GETSTACKPTR (mchargi);
    SETSINGSTEP (mchargi);
    if (!(dc -> cb -> writemem) (dc -> cp, &mchargi, sizeof mchargi, mchargs)) {
      (dc -> cb -> print) (dc -> cp, "%s: error writing mchargs at %p\n", mchargs);
      goto commandloop;
    }
    return;
  }

  /* Quit execution (abort program) */

  if (cmdbuf[0] == 'q') {
    (dc -> cb -> abort) (dc -> cp, sigargs, mchargs, mchargx);
  }

  /* Single-step an instruction, entering any subroutine called */

  if (cmdbuf[0] == 's') {
    if (!(dc -> cb -> readmem) (dc -> cp, &mchargi, sizeof mchargi, mchargs)) {
      (dc -> cb -> print) (dc -> cp, "%s: error reading mchargs at %p\n", mchargs);
      goto commandloop;
    }
    dc -> contstepbreak = NULL;
    dc -> nextstep = 0;
    SETSINGSTEP (mchargi);
    if (!(dc -> cb -> writemem) (dc -> cp, &mchargi, sizeof mchargi, mchargs)) {
      (dc -> cb -> print) (dc -> cp, "%s: error writing mchargs at %p\n", mchargs);
      goto commandloop;
    }
    return;
  }

  /* Print a call traceback */

  if (cmdbuf[0] == 't') {
    int framecount;
    OZ_Mchargs *mchargsp, tmchargs;
    void *mchargxp;

    p = cmdbuf + 1;					/* point past the 't' */
    framecount = 2000000000;				/* default frame count */
    mchargsp = mchargs;					/* assume we will use the regular mchargs */
    mchargxp = mchargx;
    while (1) {
      while ((*p != 0) && (*p <= ' ')) p ++;		/* skip any leading spaces */
      if (*p == 0) break;				/* stop if end of line */
      if (*p == '@') {					/* check for '@' */
        p = getexp (dc, p + 1, &varsize, &varaddr, mchargs, mchargx, prompt);
        if (p == NULL) goto commandloop;
        if (!(dc -> cb -> readmem) (dc -> cp, &tmchargs, sizeof tmchargs, (void *)varaddr)) {
          (dc -> cb -> print) (dc -> cp, "%s: mchargs %p not readable\n", prompt, (void *)varaddr);
          goto commandloop;
        }
        mchargsp = &tmchargs;				/* that is the maching args to use */
      } else {
        p = getexp (dc, p, &varsize, &varaddr, mchargs, mchargx, prompt);
        if (p == NULL) goto commandloop;
        framecount = varaddr;				/* otherwise, it is the frame count */
      }
    }
    traceback (dc, mchargsp, mchargxp, framecount);	/* print the traceback */
    goto commandloop;					/* get another command */
  }

  /* Watch an address for modifications */

  if (cmdbuf[0] == 'w') {
    p = getexp (dc, cmdbuf + 1, &varsize, &varaddr, mchargs, mchargx, prompt);
    if (p == NULL) goto commandloop;
    oz_hw_debug_watch (varaddr);
    (dc -> cb -> print) (dc -> cp, "%s: watching %p\n", prompt, varaddr);
    goto commandloop;
  }

  /* Switch command cpu number */

  if (cmdbuf[0] == 'x') {
    p = getexp (dc, cmdbuf + 1, &varsize, &varaddr, mchargs, mchargx, prompt);
    if (p == NULL) goto commandloop;
    if (*p != 0) {
      (dc -> cb -> print) (dc -> cp, "%s: cpu number must be last on line (%s)\n", prompt, p);
      goto commandloop;
    }
    if ((varaddr >= 8 * sizeof dc -> cpushalted) || !(dc -> cpushalted & (1 << varaddr))) {
      (dc -> cb -> print) (dc -> cp, "%s: cpu %d not halted (%X)\n", prompt, varaddr, dc -> cpushalted);
      goto commandloop;
    }
    (dc -> cb -> print) (dc -> cp, "%s: switching to cpu %d\n", prompt, varaddr);
    dc -> commandcpu = varaddr;	/* the new cpu should break out of the wait loop */
    (dc -> cb -> resume) (dc -> cp);
    (dc -> cb -> print) (dc -> cp, "%s: switched to cpu %d\n", prompt, varaddr);
    goto commandloop;		/* ... and the old cpu should hang in the loop */
  }

  /* Unknown */

  (dc -> cb -> print) (dc -> cp, "%s: unknown command %s\n", prompt, cmdbuf);
  goto commandloop;
}

/************************************************************************/
/*									*/
/*  Get an address expression						*/
/*									*/
/*    Input:								*/
/*									*/
/*	cmdbuf  = pointer to string to evaluate				*/
/*	mchargs = standard machine arg list pointer			*/
/*	mchargx = extended machine arg list pointer			*/
/*									*/
/*    Output:								*/
/*									*/
/*	getexp = NULL : error evaluating expression			*/
/*	         else : pointer to terminating character		*/
/*	*varsize = size of location pointed to by varaddr		*/
/*	*varaddr = expression's value					*/
/*									*/
/************************************************************************/

static char *getaddrexp (OZ_Debug *dc, char *cmdbuf, int *varsize, OZ_Pointer *varaddr, OZ_Mchargs *mchargs, void *mchargx, const char *prompt)

{
  char c, *p;
  OZ_Pointer va;

  p = getexp (dc, cmdbuf, varsize, varaddr, mchargs, mchargx, prompt);
  if (p == NULL) return (NULL);

  /* It might be followed by a .size */

  while (((c = *p) != 0) && (c <= ' ')) p ++;
  if (c == '.') {
    p = getexp (dc, p + 1, NULL, &va, mchargs, mchargx, prompt);
    if (p == NULL) return (NULL);
    if (varsize != NULL) *varsize = va;
  }

  return (p);
}

/************************************************************************/
/*									*/
/*  Get an expression							*/
/*									*/
/*    Input:								*/
/*									*/
/*	cmdbuf  = pointer to string to evaluate				*/
/*	mchargs = standard machine arg list pointer			*/
/*	mchargx = extended machine arg list pointer			*/
/*									*/
/*    Output:								*/
/*									*/
/*	getexp = NULL : error evaluating expression			*/
/*	         else : pointer to terminating character		*/
/*	*varsize = default size of location pointed to by varaddr	*/
/*	*varaddr = expression's value					*/
/*									*/
/************************************************************************/

#define BOP_ADD 0
#define BOP_SUB 1
static const ccharp binary[] = { "+", "-", NULL };

static char *getexp (OZ_Debug *dc, char *cmdbuf, int *varsize, OZ_Pointer *varaddr, OZ_Mchargs *mchargs, void *mchargx, const char *prompt)

{
  char c, *p;
  int i, l, vs1, vs2;
  OZ_Pointer va1, va2;

  p = cmdbuf;

  /* Get the first operand */

  p = getoperand (dc, p, &vs1, &va1, mchargs, mchargx, prompt);
  if (p == NULL) return (NULL);

  while (1) {

    /* Check for binary operator */

    while (((c = *p) != 0) && (c <= ' ')) p ++;
    if (c == 0) break;

    for (i = 0; binary[i] != NULL; i ++) {
      l = strlen (binary[i]);
      if (memcmp (binary[i], p, l) == 0) break;
    }
    if (binary[i] == NULL) break;
    p += l;

    /* Get next operand */

    p = getoperand (dc, p, &vs2, &va2, mchargs, mchargx, prompt);
    if (p == NULL) return (NULL);

    /* Combine the two values */

    switch (i) {
      case BOP_ADD: {
        va1 += va2;
        break;
      }
      case BOP_SUB: {
        va1 -= va2;
        break;
      }
    }
  }

  /* Skip any trailing spaces */

  while (((c = *p) != 0) && (c <= ' ')) p ++;

  /* Return final values */

  if (varsize != NULL) *varsize = vs1;
  *varaddr = va1;

  return (p);
}

/************************************************************************/
/*									*/
/*  Get an operand							*/
/*									*/
/*    Input:								*/
/*									*/
/*	cmdbuf  = pointer to string to evaluate				*/
/*	mchargs = standard machine arg list pointer			*/
/*	mchargx = extended machine arg list pointer			*/
/*									*/
/*    Output:								*/
/*									*/
/*	getoperand = NULL : error evaluating expression			*/
/*	             else : pointer to terminating character		*/
/*	*varsize = default size of location pointed to by varaddr	*/
/*	*varaddr = operand's value					*/
/*									*/
/*    Note:								*/
/*									*/
/*	Mchargs symbols must be prefixed by %				*/
/*	Breakpoint registers indicies are prefixed by $			*/
/*									*/
/************************************************************************/

static const ccharp delims = " ~!@#$%^&*()+=-`{}][\"|\\';,./?><";

#define UOP_NEG 0
#define UOP_IND 1
#define UOP_BPT 2
static const ccharp unary[] = { "-", "*", "$", NULL };

static char *getoperand (OZ_Debug *dc, char *cmdbuf, int *varsize, OZ_Pointer *varaddr, OZ_Mchargs *mchargs, void *mchargx, const char *prompt)

{
  char c, *p, *q, *symname;
  int i, l, symsize;
  OZ_Pointer symaddr, va;

  p = cmdbuf;

  /* Skip leading spaces */

  while (((c = *p) != 0) && (c <= ' ')) p ++;

  /* If (), get the expression and return that as my values */

  if (c == '(') {
    p = getexp (dc, ++ p, varsize, varaddr, mchargs, mchargx, prompt);
    if (p == NULL) return (NULL);
    while (((c = *p) != 0) && (c <= ' ')) p ++;
    if (c != ')') {
      (dc -> cb -> print) (dc -> cp, "%s: missing ) at %s\n", prompt, p);
      return (NULL);
    }
  }

  /* Check for unary operator */

  for (i = 0; unary[i] != NULL; i ++) {
    l = strlen (unary[i]);
    if (memcmp (unary[i], p, l) == 0) break;
  }
  if (unary[i] != NULL) {
    p += l;
    p = getoperand (dc, p, varsize, &va, mchargs, mchargx, prompt);
    if (p == NULL) return (NULL);
    switch (i) {
      case UOP_NEG: {
        *varaddr = - va;
        break;
      }
      case UOP_IND: {
        if (!(dc -> cb -> readmem) (dc -> cp, varaddr, sizeof *varaddr, (void *)va)) {
          (dc -> cb -> print) (dc -> cp, "%s: location %p not readable\n", prompt, va);
          return (NULL);
        }
        break;
      }
      case UOP_BPT: {
        if (va > OZ_DEBUG_NBREAKS) {
          (dc -> cb -> print) (dc -> cp, "%s: breakpoint %u invalid, max is %u\n", prompt, va, OZ_DEBUG_NBREAKS);
          return (NULL);
        }
        *varaddr = (OZ_Pointer)(dc -> breakaddr + va);
      }
    }
    return (p);
  }

  /* See if it starts with a digit, if so, it's definitely numeric */

  if ((c >= '0') && (c <= '9')) goto numeric;

  /* If it starts with a %, its in the mchargs or mchargx */

  if (c == '%') {
    for (i = 0; dc -> cb -> mchargsdes[i].name != NULL; i ++) {
      l = strlen (dc -> cb -> mchargsdes[i].name);
      if ((p[l+1] != 0) && (strchr (delims, p[l+1]) == NULL)) continue;
      if (memcmp (p + 1, dc -> cb -> mchargsdes[i].name, l) == 0) break;
    }
    if (dc -> cb -> mchargsdes[i].name != NULL) {
      symsize = dc -> cb -> mchargsdes[i].size;
      symaddr = dc -> cb -> mchargsdes[i].offs + (OZ_Pointer)mchargs;
      p += l + 1;
      goto gotvalue;
    }
    for (i = 0; dc -> cb -> mchargxdes[i].name != NULL; i ++) {
      l = strlen (dc -> cb -> mchargxdes[i].name);
      if ((p[l+1] != 0) && (strchr (delims, p[l+1]) == NULL)) continue;
      if (memcmp (p + 1, dc -> cb -> mchargxdes[i].name, l) == 0) break;
    }
    if (dc -> cb -> mchargxdes[i].name == NULL) goto cantdecode;
    symsize = dc -> cb -> mchargxdes[i].size;
    symaddr = dc -> cb -> mchargxdes[i].offs + (OZ_Pointer)mchargx;
    p += l + 1;
    goto gotvalue;
  }

  /* See if a symbol matches from symbol table */

  for (q = p; (c = *q) != 0; q ++) if (strchr (delims, c) != NULL) break;
  *q = 0;
  i = (dc -> cb -> cvtsymbol) (dc -> cp, p, &symaddr, &symsize);
  *q = c;
  if (i) {
    p = q;
    goto gotvalue;
  }

  /* Not register or symbol, try to convert hexadecimal string */

numeric:
  symaddr = 0;
  symsize = 0;
  l = 0;
  while (strchr (delims, (c = *p)) == NULL) {
    if ((c >= '0') && (c <= '9')) symaddr = symaddr * 16 + c - '0';
    else if ((c >= 'A') && (c <= 'F')) symaddr = symaddr * 16 + c - 'A' + 10;
    else if ((c >= 'a') && (c <= 'f')) symaddr = symaddr * 16 + c - 'a' + 10;
    else goto cantdecode;
    p ++;
    l ++;
  }
  if (l == 0) goto cantdecode;

gotvalue:
  if (symsize == 0) symsize = sizeof (void *);

  /* All done - return values */

  if (varsize != NULL) *varsize = symsize;
  if (varaddr != NULL) *varaddr = symaddr;

  return (p);

  /* Don't know what the operand is */

cantdecode:
  (dc -> cb -> print) (dc -> cp, "%s: cannot decode operand %s\n", prompt, cmdbuf);
  return (NULL);
}

/************************************************************************/
/*									*/
/*  Remove installed breakpoints					*/
/*									*/
/************************************************************************/

static void removebreaks (OZ_Debug *dc, const char *prompt)

{
  char *p;
  int i;

  LOCKBREAK ();
  if (dc -> bptsinstalled) {

    /* Remove the breakpoints by writing the original opcode back */

    for (i = 0; i < OZ_DEBUG_NBREAKS; i ++) {
      if (dc -> breakaddr[i] != NULL) {
        p = (dc -> cb -> writebpt) (dc -> cp, dc -> breakaddr[i], dc -> breakdata[i]);
        if (p != NULL) {
          (dc -> cb -> print) (dc -> cp, "%s: cannot remove breakpoint %u at %p (%s)\n", prompt, i, dc -> breakaddr[i], p);
        }
      }
    }
    dc -> bptsinstalled = 0;
  }
  UNLKBREAK ();
}

/************************************************************************/
/*									*/
/*  Print the disassembly of an instruction				*/
/*									*/
/************************************************************************/

static int disassemble (OZ_Debug *dc, OZ_Pointer varaddr, OZ_Mchargs *mchargs, void *mchargx, OZ_Pointer *varsize_r)

{
  uByte opcode;
  int value;

  if (!(dc -> cb -> readmem) (dc -> cp, &opcode, sizeof opcode, (void *)varaddr)) return (0); /* if we can't read the single byte it is at, return error */
  printaddr (dc, varaddr, mchargs, mchargx);					/* print the address */
  (dc -> cb -> print) (dc -> cp, " : ");					/* and a colon */
  value = (dc -> cb -> printinstr) (dc -> cp, (void *)varaddr);			/* then print the instruction */
  if (value < 0) return (0);							/* if it failed, return failure status */
  if (varsize_r != NULL) *varsize_r = value;					/* ok, return the instruction's size */
  return (1);									/* successful */
}

/************************************************************************/
/*									*/
/*  Print a stack frame traceback					*/
/*									*/
/************************************************************************/

static void traceback (OZ_Debug *dc, OZ_Mchargs *mchargs, void *mchargx, int framecount)

{
  TRACEBACK
}

/************************************************************************/
/*									*/
/*  Print a string, censoring any garbage characters			*/
/*									*/
/*    Input:								*/
/*									*/
/*	outsize = size of output field					*/
/*	varsize = size of data (in bytes)				*/
/*	varaddr = external address of data				*/
/*									*/
/************************************************************************/

static void printstring (OZ_Debug *dc, int outsize, int varsize, OZ_Pointer varaddr)

{
  char c;
  int i;

  for (i = 0; (i < varsize) && (i < outsize); i ++) {
    (dc -> cb -> readmem) (dc -> cp, &c, sizeof c, (char *)(varaddr + i));
    if ((c < ' ') || (c == 0x7f)) c = '.';
    (dc -> cb -> print) (dc -> cp, "%c", c);
  }
  for (; i < outsize; i ++) (dc -> cb -> print) (dc -> cp, " ");
}

/************************************************************************/
/*									*/
/*  Print the external address symbolically if possible			*/
/*									*/
/************************************************************************/

static void printaddr (OZ_Debug *dc, OZ_Pointer varaddr, OZ_Mchargs *mchargs, void *mchargx)

{
  int i;
  OZ_Pointer offs;

  /* Maybe it is in the breakpoint table somewhere */

  offs = varaddr - (OZ_Pointer)(dc -> breakaddr);
  if ((varaddr >= (OZ_Pointer)(dc -> breakaddr)) && (offs < sizeof dc -> breakaddr)) {
    (dc -> cb -> print) (dc -> cp, "$%x", offs / sizeof *(dc -> breakaddr));
    if (offs % sizeof *(dc -> breakaddr) != 0) (dc -> cb -> print) (dc -> cp, "+%x", offs % sizeof *(dc -> breakaddr));
    return;
  }

  /* Maybe it is in the standard or extended machine arguments somewhere */

  offs = varaddr - (OZ_Pointer)mchargs;
  if ((varaddr >= (OZ_Pointer)mchargs) && (offs < sizeof *mchargs)) {
    for (i = 0; dc -> cb -> mchargsdes[i].name != NULL; i ++) {
      if ((offs >= dc -> cb -> mchargsdes[i].offs) && (offs < (dc -> cb -> mchargsdes[i].offs + dc -> cb -> mchargsdes[i].size))) {
        (dc -> cb -> print) (dc -> cp, "%%%s", dc -> cb -> mchargsdes[i].name);
        offs -= dc -> cb -> mchargsdes[i].offs;
        if (offs != 0) (dc -> cb -> print) (dc -> cp, "+%x", offs);
        return;
      }
    }
    (dc -> cb -> print) (dc -> cp, "% machine args + %x\n", offs);
    return;
  }

  offs = varaddr - (OZ_Pointer)mchargx;
  if ((varaddr >= (OZ_Pointer)mchargx) && (offs < dc -> cb -> mchargxsiz)) {
    for (i = 0; dc -> cb -> mchargxdes[i].name != NULL; i ++) {
      if ((offs >= dc -> cb -> mchargxdes[i].offs) && (offs < (dc -> cb -> mchargxdes[i].offs + dc -> cb -> mchargxdes[i].size))) {
        (dc -> cb -> print) (dc -> cp, "%%%s", dc -> cb -> mchargxdes[i].name);
        offs -= dc -> cb -> mchargxdes[i].offs;
        if (offs != 0) (dc -> cb -> print) (dc -> cp, "+%x", offs);
        return;
      }
    }
    (dc -> cb -> print) (dc -> cp, "% machine args + %x\n", offs);
    return;
  }

  /* None of that, use the (dc -> cb -> printaddr) routine */

  (dc -> cb -> printaddr) (dc -> cp, (void *)varaddr);
}

/************************************************************************/
/*									*/
/*  Dump 'size' bytes of memory starting at address 'addr'		*/
/*									*/
/************************************************************************/

static void dumpmem (OZ_Debug *dc, int size, void *addr)

{
  uByte b, b16[16], *bp;
  uLong i, j, n, pk;

  union { uLong l;
          uByte b[4];
        } letest;

  bp = addr;
  letest.l = 1;

  for (i = 0; i < size; i += 16) {

    /* Try to get up to 16 bytes of data.  Exit loop if nothing readable. */

    n = size - i;
    if (n > 16) n = 16;
    while ((n > 0) && !(dc -> cb -> readmem) (dc -> cp, b16, n, bp)) -- n;
    if (n == 0) break;

    /* Print hex dump depending on endian */

    if (letest.b[0]) {
      (dc -> cb -> print) (dc -> cp, "  ");
      for (j = 16; j > 0;) {
        if (!(j & 3)) (dc -> cb -> print) (dc -> cp, " ");
        if (-- j >= n) (dc -> cb -> print) (dc -> cp, "  ");
        else (dc -> cb -> print) (dc -> cp, "%2.2x", b16[j]);
      }
      (dc -> cb -> print) (dc -> cp, " : %*.*x : ", 2 * sizeof (OZ_Pointer), 2 * sizeof (OZ_Pointer), (OZ_Pointer)bp);
    } else {
      (dc -> cb -> print) (dc -> cp, "  %*.*x : ", 2 * sizeof (OZ_Pointer), 2 * sizeof (OZ_Pointer), (OZ_Pointer)bp);
      for (j = 0; j < 16; j ++) {
        if (!(j & 3)) (dc -> cb -> print) (dc -> cp, " ");
        if (j >= n) (dc -> cb -> print) (dc -> cp, "  ");
        else (dc -> cb -> print) (dc -> cp, "%2.2x", b16[j]);
      }
      (dc -> cb -> print) (dc -> cp, " : ");
    }

    /* Print character dump regardless of endian */

    for (j = 0; j < n; j ++) {
      if ((b16[j] < ' ') || (b16[j] >= 127)) b16[j] = '.';
    }
    (dc -> cb -> print) (dc -> cp, "%*.*s\n", n, n, b16);

    /* Increment address for next line */

    bp += 16;
  }
}
