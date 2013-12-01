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
/*  Implementation of unix-like signals					*/
/*									*/
/************************************************************************/

#include <errno.h>
#include <signal.h>
#include <sys/types.h>

#include "ozone.h"
#include "oz_crtl_malloc.h"
#include "oz_knl_hw.h"
#include "oz_knl_sdata.h"
#include "oz_knl_status.h"
#include "oz_sys_condhand.h"
#include "oz_sys_event.h"
#include "oz_sys_handle.h"

#define ALIAS(x) asm (" .globl __" #x "\n __" #x "=" #x );

#define OZ_SYS_THREAD_LOCK_WAIT(__lock) 0
#define OZ_SYS_THREAD_LOCK_CLR(__lock,__prev)

static OZ_Handle h_sigevsusp;
static OZ_Handle h_sigevstop;
static OZ_Handle h_sigevextern;

#ifdef NSIG
#undef NSIG
#endif

static sigset_t sigpendmask;
static sigset_t sigblockmask;

#define NVAL (sizeof sigpendmask / sizeof sigpendmask.__val[0])
#define NSIG (8 * sizeof sigpendmask)
#define NBPV (8 * sizeof sigpendmask.__val[0])

static struct sigaction *sigactions;

static void sigevast (void *dummy, uLong status, OZ_Mchargs *mchargs);
static void setsigmask (sigset_t newblockmask);
static void call_signal (int signum);

/************************************************************************/
/*									*/
/*  Initialize signal routines						*/
/*									*/
/************************************************************************/

#if 0
static uLong dumplnms (void *dummy)

{
  OZ_Logname *lognamdir;

  lognamdir = oz_knl_process_getlognamdir (NULL);
  oz_knl_logname_dump (0, lognamdir);
  oz_hw_pause ("oz_crtl_signal_init> ");
  return (OZ_SUCCESS);
}
#endif

void oz_crtl_signal_init (void)

{
  int i;
  uLong sts;
  OZ_Handle h_lognamtbl;
  OZ_Logvalue values;

  memset (&sigpendmask,  0, sizeof sigpendmask);		/* no signals pending yet */
  memset (&sigblockmask, 0, sizeof sigblockmask);		/* no signals blocked */

#if 0
  oz_sys_callknl (dumplnms, NULL);
#endif

  sigactions = malloc (NSIG * sizeof *sigactions);		/* alloc signal actions array */
  memset (sigactions, 0, sizeof sigactions);			/* no handlers declared */
  for (i = 0; i < NSIG; i ++) sigactions[i].sa_handler = SIG_DFL;

  sts = oz_sys_event_create (OZ_PROCMODE_KNL, "oz_crtl_signal sigsuspend", &h_sigevsusp); /* create event flag used by sigsuspend  */
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (1, sts);
  sts = oz_sys_event_create (OZ_PROCMODE_KNL, "oz_crtl_signal sigstop", &h_sigevstop); /* create event flag used by SIGSTOP/SIGCONT */
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (1, sts);

  sts = oz_sys_event_create (OZ_PROCMODE_KNL, "oz_crtl_signal kill", &h_sigevextern); /* create event flag used by external 'kill' calls to signal us */
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (1, sts);
  sts = oz_sys_event_set (OZ_PROCMODE_KNL, h_sigevextern, 0, NULL);
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (1, sts);
  sts = oz_sys_event_ast (OZ_PROCMODE_KNL, h_sigevextern, sigevast, NULL, 0);
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (1, sts);
  sts = oz_sys_logname_lookup (0, OZ_PROCMODE_USR, "OZ_PROCESS_TABLE", NULL, NULL, NULL, &h_lognamtbl);
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (1, sts);
  values.attr = OZ_LOGVALATR_OBJECT;
  values.buff = (void *)(OZ_Pointer)h_sigevextern;
  sts = oz_sys_logname_create (h_lognamtbl, "OZ_CRTL_SIGNAL", OZ_PROCMODE_KNL, 0, 1, &values, NULL);
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (1, sts);
  oz_sys_handle_release (OZ_PROCMODE_KNL, h_lognamtbl);
}

/************************************************************************/
/*									*/
/*  Standard condition handler to convert a condition to a signal	*/
/*									*/
/************************************************************************/

uLong oz_sys_signal_condhand (void *chparam, int unwinding, OZ_Sigargs sigargs[], OZ_Mchargs *mchargs)

{
  uLong sts;

  switch (sigargs[1]) {

    /* Ones that do have equivalent signals get processed by their signal handlers */

    case OZ_ACCVIO: {
      call_signal (SIGSEGV);
      break;
    }
    case OZ_SINGLESTEP:
    case OZ_BREAKPOINT: {
      call_signal (SIGTRAP);
      break;
    }
    case OZ_UNDEFOPCODE: {
      call_signal (SIGILL);
      break;
    }
    case OZ_FLOATPOINT: {
      call_signal (SIGFPE);
      break;
    }

    /* Ones that don't have equivalent signals get processed by outer handler (traceback/coredump) */

    default: {
    }
  }
  return (OZ_RESIGNAL);
}

/************************************************************************/
/*									*/
/*  Cause the 'sigevast' routine to be called on a given process	*/
/*									*/
/************************************************************************/

int kill (pid_t pid, int sig)

{
  uLong sts;

  if (pid != 0) oz_crash ("oz_crtl_signal kill: only support pid 0 for now");

  sts = oz_sys_event_set (OZ_PROCMODE_KNL, h_sigevextern, sig + 1, NULL);	/* store signal number + 1 in event flag */
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (1, sts);
  return (0);
}

ALIAS (kill)

/************************************************************************/
/*									*/
/*  Set the action to be taken on a given signal			*/
/*									*/
/************************************************************************/

int sigaction (int signum, const struct sigaction *act, struct sigaction *oldact)

{
  int tl;

  if ((signum < 0) || (signum >= NSIG)) {		/* make sure signal number is ok */
    errno = EINVAL;
    return (-1);
  }

  tl = OZ_SYS_THREAD_LOCK_WAIT (&siglock);		/* block other threads from changing array */
  if (oldact != NULL) *oldact = sigactions[signum];	/* get old element if caller wants it */
  if (act != NULL) sigactions[signum] = *act;		/* set new action if caller specified it */
  OZ_SYS_THREAD_LOCK_CLR (&siglock, tl);		/* release other threads */
  return (0);						/* successful */
}

ALIAS (sigaction)

/************************************************************************/
/*									*/
/*  Modify the signal blocking mask					*/
/*									*/
/************************************************************************/

int sigprocmask (int how, const sigset_t *set, sigset_t *oldset)

{
  int i, rc, tl;

  rc = 0;
  tl = OZ_SYS_THREAD_LOCK_WAIT (&siglock);		/* lock other threads out */
  if (oldset != NULL) *oldset = sigblockmask;		/* return current mask if requested */
  if (set != NULL) switch (how) {			/* set new mask if specified */
    case SIG_BLOCK: {					/* - block the given signals */
      for (i = 0; i < NVAL; i ++) sigblockmask.__val[i] |= set -> __val[i];
      break;
    }
    case SIG_UNBLOCK: {					/* - unblock the given signals */
      for (i = 0; i < NVAL; i ++) sigblockmask.__val[i] &= ~ set -> __val[i];
      setsigmask (sigblockmask);			/* - ... and call enabled routines */
      break;
    }
    case SIG_SETMASK: {
      setsigmask (*set);				/* - set to given set */
      break;
    }
    default: {
      rc = -1;
      errno = EINVAL;
      break;
    }
  }
  OZ_SYS_THREAD_LOCK_CLR (&siglock, tl);		/* enable other threads */
  return (rc);						/* return status */
}

ALIAS (sigprocmask)

/************************************************************************/
/*									*/
/*  See what blocked signals are pending				*/
/*									*/
/************************************************************************/

int sigpending (sigset_t *set)

{
  if (set != NULL) *set = sigpendmask;
  return (0);
}

ALIAS (sigpending)

/************************************************************************/
/*									*/
/*  Suspend until a signal is delivered					*/
/*									*/
/************************************************************************/

int sigsuspend (const sigset_t *mask)

{
  int tl;
  uLong sts;
  sigset_t savemask;

  tl = OZ_SYS_THREAD_LOCK_WAIT (&siglock);				/* block other threads while changing mask */
  sts = oz_sys_event_set (OZ_PROCMODE_KNL, h_sigevsusp, 0, NULL);	/* clear event flag so we will wait for a signal */
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (1, sts);
  savemask = sigblockmask;						/* save current mask */
  setsigmask (*mask);							/* set up new mask */
  OZ_SYS_THREAD_LOCK_CLR (&siglock, tl);				/* release other threads */
  sts = oz_sys_event_wait (OZ_PROCMODE_KNL, h_sigevsusp, 1);		/* wait for event flag to be set by a signal */
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (1, sts);
  tl = OZ_SYS_THREAD_LOCK_WAIT (&siglock);				/* block other threads while changing mask */
  setsigmask (savemask);						/* restore signal mask */
  OZ_SYS_THREAD_LOCK_CLR (&siglock, tl);				/* release other threads */
  return (0);
}

ALIAS (sigsuspend)

/************************************************************************/
/*									*/
/*  This ast routine is called when someone sets the OZ_CRTL_SIGNAL 	*/
/*  event flag to a signal number + 1					*/
/*									*/
/************************************************************************/

static void sigevast (void *dummy, uLong status, OZ_Mchargs *mchargs)

{
  uLong sts, ulsignum;

  while (1) {
    sts = oz_sys_event_set (OZ_PROCMODE_KNL, h_sigevextern, 0, &ulsignum);	/* get signal number + 1, reset event flag */
    if (sts != OZ_SUCCESS) oz_sys_condhand_signal (1, sts);
    if (ulsignum == 0) break;							/* if zero, we're all done */
    call_signal (ulsignum - 1);							/* non-zero, call signal handler */
  }

  sts = oz_sys_event_ast (OZ_PROCMODE_KNL, h_sigevextern, sigevast, NULL, 0);	/* re-arm the ast for another signal */
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (1, sts);
}

/************************************************************************/
/*									*/
/*  Set new blocking mask and call any pending signal handlers		*/
/*									*/
/************************************************************************/

static void setsigmask (sigset_t newblockmask)

{
  int i, signum;
  sigset_t callmask;

  sigblockmask = newblockmask;					/* set new value */

  for (i = 0; i < NVAL; i ++) {
    callmask.__val[i] = sigpendmask.__val[i] & ~ sigblockmask.__val[i]; /* get mask of pending but not blocked signals */
    sigpendmask.__val[i] &= sigblockmask.__val[i];		/* save mask of pending but blocked signals */
  }

  for (signum = 0; signum < NSIG; signum ++) {			/* loop though all possible signal numbers */
    if (callmask.__val[signum/NBPV] & (1 << (signum % NBPV))) {	/* if it was pending but not not called, call it */
      call_signal (signum);					/* call the signal handler */
    }
  }
}

/************************************************************************/
/*									*/
/*  Call the currently defined signal handler for the given signal	*/
/*									*/
/************************************************************************/

static void call_signal (int signum)

{
  int i, tl;
  uLong sts;
  sigset_t savemask;
  void (*handler) ();

  /* If the signal is blocked, save the bit in the sigpendmask and return */

  tl = OZ_SYS_THREAD_LOCK_WAIT (&siglock);
  if (sigblockmask.__val[signum/NBPV] & (1 << (signum % NBPV))) {
    sigpendmask.__val[signum/NBPV] |= 1 << (signum % NBPV);
    OZ_SYS_THREAD_LOCK_CLR (&siglock, tl);
    return;
  }

  /* Not blocked, call the handling routine */

  /* Default processing */

  if (sigactions[signum].sa_handler == SIG_DFL) {
    switch (signum) {
      case SIGCONT:   sts = oz_sys_event_set (OZ_PROCMODE_KNL, h_sigevstop, 1, NULL); break;
      case SIGSTOP:   sts = oz_sys_event_wait (OZ_PROCMODE_KNL, h_sigevstop, 1); sts = oz_sys_event_set (OZ_PROCMODE_KNL, h_sigevstop, 0, NULL); break;

      case SIGHUP:    oz_sys_condhand_signal (1, OZ_SIGNAL_HUP);     break;
      case SIGINT:    oz_sys_condhand_signal (1, OZ_SIGNAL_INT);     break;
      case SIGQUIT:   oz_sys_condhand_signal (1, OZ_SIGNAL_QUIT);    break;
      case SIGUNUSED: oz_sys_condhand_signal (1, OZ_SIGNAL_UNUSED);  break;
      case SIGKILL:   oz_sys_condhand_signal (1, OZ_SIGNAL_KILL);    break;
      case SIGUSR1:   oz_sys_condhand_signal (1, OZ_SIGNAL_USR1);    break;
      case SIGUSR2:   oz_sys_condhand_signal (1, OZ_SIGNAL_USR2);    break;
      case SIGPIPE:   oz_sys_condhand_signal (1, OZ_SIGNAL_PIPE);    break;
      case SIGALRM:   oz_sys_condhand_signal (1, OZ_SIGNAL_ALRM);    break;
      case SIGTERM:   oz_sys_condhand_signal (1, OZ_SIGNAL_TERM);    break;
      case SIGSTKFLT: oz_sys_condhand_signal (1, OZ_SIGNAL_STKFLT);  break;

      case SIGILL:    oz_sys_condhand_signal (1, OZ_UNDEFOPCODE);    break;
      case SIGTRAP:   oz_sys_condhand_signal (1, OZ_BREAKPOINT);     break;
      case SIGABRT:   oz_sys_condhand_signal (1, OZ_ABORTED);        break;
      case SIGFPE:    oz_sys_condhand_signal (1, OZ_FLOATPOINT);     break;
      case SIGSEGV:   oz_sys_condhand_signal (1, OZ_ACCVIO);         break;
      default:        oz_sys_condhand_signal (1, OZ_SIGNAL_UNKNOWN); break;
    }
  }

  /* Forced ignore, do nothing */

  else if (sigactions[signum].sa_handler == SIG_DFL) { }

  /* Otherwise, call handler */

  else {
    savemask = sigblockmask;
    for (i = 0; i < NVAL; i ++) sigblockmask.__val[i] |= sigactions[signum].sa_mask.__val[i];
    handler = sigactions[signum].sa_handler;
    sigactions[signum].sa_handler = SIG_DFL;
    OZ_SYS_THREAD_LOCK_CLR (&siglock, tl);
    (*handler) (signum);
    tl = OZ_SYS_THREAD_LOCK_WAIT (&siglock);
    setsigmask (savemask);
  }

  OZ_SYS_THREAD_LOCK_CLR (&siglock, tl);

  /* Set event flag in case thread is in sigsuspend routine */

  sts = oz_sys_event_set (OZ_PROCMODE_KNL, h_sigevsusp, 1, NULL);
  if (sts != OZ_SUCCESS) oz_sys_condhand_signal (1, sts);
}

/* I have no clue what these do */

int sigemptyset (sigset_t *set)

{
  return (0);
}
