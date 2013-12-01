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

#include "ozone.h"
#include "oz_io_console.h"
#include "oz_io_timer.h"
#include "oz_knl_hw.h"
#include "oz_knl_sdata.h"
#include "oz_knl_status.h"
#include "oz_sys_condhand.h"
#include "oz_sys_dateconv.h"
#include "oz_sys_handle.h"
#include "oz_sys_io_fs.h"
#include "oz_sys_logname.h"
#include "oz_sys_process.h"

#include <errno.h>
#include <time.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/utsname.h>

#define EOZSYSERR 65535

typedef uLong uid_t;
typedef uLong gid_t;
typedef OZ_Processid pid_t;

#ifdef errno
#undef errno
#endif

globaldef int errno;
globaldef uLong errno_ozsts;

#define ALIAS(x) asm (" .globl __" #x "\n __" #x "=" #x );

void *malloc ();

int* __errno_location (void)

{
  return (&errno);
}

void abort (void)

{
  while (1) oz_sys_condhand_signal (1, OZ_ABORTED);
}

uid_t getuid (void) { return (0); }

uid_t geteuid (void) { return (0); }

gid_t getgid (void) { return (0); }

gid_t getegid (void) { return (0); }

int getgroups (int size, gid_t list[]) { return (0); }

/* ?? hardcoded values - get from a system logical or something like that ?? */

int uname (struct utsname *buf)

{
  memset (buf, 0, sizeof *buf);
  strncpyz (buf -> sysname, "OZONE",   sizeof buf -> sysname);
  strncpyz (buf -> version, "0.0",     sizeof buf -> version);
  strncpyz (buf -> machine, "Dual P2", sizeof buf -> machine);

  return (0);
}

ALIAS (abort)
ALIAS (getuid)
ALIAS (geteuid)
ALIAS (getgid)
ALIAS (getegid)
ALIAS (getgroups)
ALIAS (time)
ALIAS (uname)

typedef struct { size_t size; char *buf; } Getcwd_p;

static uLong getcwd_parse (void *param, const char *devname, const char *filname, OZ_Handle h_iochan)

{
  Getcwd_p *p;
  int i, j;

  p = param;
  i = strlen (devname);
  j = strlen (filname);
  if (p -> buf == NULL) {
    if (((int)(p -> size)) < 0) p -> size = i + j + 1;
    p -> buf = malloc (p -> size);
  }
  if (i > p -> size) i = p -> size;
  if (j > p -> size - i) j = p -> size - i;
  memcpy (p -> buf, devname, i);
  memcpy (p -> buf + i, filname, j);
  if (i + j < p -> size) p -> buf[i+j] = 0;
  return (OZ_SUCCESS);
}

char *getcwd (char *buf, size_t size)

{
  Getcwd_p p;
  uLong sts;

  p.buf = buf;
  p.size = size;

  sts = oz_sys_io_fs_parse ("", 0, getcwd_parse, &p);
  if (sts != OZ_SUCCESS) {
    errno = EOZSYSERR;
    errno_ozsts = sts;
    return (NULL);
  }
  return (buf);
}

ALIAS (getcwd)

#ifdef __USE_GNU
char *get_current_working_dir_name (void)

{
  char *envar;

#if 0
  envar = getenv ("PWD");
  if (envar != NULL) return (envar);
#endif

  return (getcwd (NULL, -1));
}
#endif

/************************************************************************/
/*									*/
/*  For the 'get process id' stuff, we return the thread-id that is 	*/
/*  the parent-most within the same process as the caller.  The 	*/
/*  assumption being that the pid returned will be used to kill 	*/
/*  processes, and we don't kill processes, just threads.		*/
/*									*/
/************************************************************************/

pid_t getpid (void)

{
  OZ_Handle h_thread, h_thread_parent;
  OZ_Processid firstpid, processid;
  OZ_Threadid lasthreadid, threadid;
  uLong sts;

  OZ_Handle_item items[3] = { OZ_HANDLE_CODE_THREAD_ID,     sizeof threadid,        &threadid,        NULL, 
                              OZ_HANDLE_CODE_PROCESS_ID,    sizeof processid,       &processid,       NULL, 
                              OZ_HANDLE_CODE_THREAD_PARENT, sizeof h_thread_parent, &h_thread_parent, NULL };

  h_thread = 0;								// start with current thread
  do {
    sts = oz_sys_handle_getinfo (h_thread, 3, items, NULL);		// get thread info
    if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);		// barf if error
    if (h_thread == 0) firstpid = processid;				// save current thread's pid
    else {
      oz_sys_handle_release (OZ_PROCMODE_KNL, h_thread);
      if (processid != firstpid) {					// if thread's parent in a different process, we're done
        oz_sys_handle_release (OZ_PROCMODE_KNL, h_thread_parent);
        break;
      }
    }
    lasthreadid = threadid;
    h_thread = h_thread_parent;						// check out parent thread
  } while (h_thread != 0);						// repeat if there is a parent thread

  return (lasthreadid);
}

/* Return top thread-id of parent process                                        */
/* If there is no parent process, the current thread's top thread-id is returned */

pid_t getppid (void)

{
  OZ_Handle h_thread, h_thread_parent;
  OZ_Processid firstpid, processid, secondpid;
  OZ_Threadid lasthreadid, threadid;
  uLong sts;

  OZ_Handle_item items[3] = { OZ_HANDLE_CODE_THREAD_ID,     sizeof threadid,        &threadid,        NULL, 
                              OZ_HANDLE_CODE_PROCESS_ID,    sizeof processid,       &processid,       NULL, 
                              OZ_HANDLE_CODE_THREAD_PARENT, sizeof h_thread_parent, &h_thread_parent, NULL };

  secondpid = 0;							// haven't found parent process yet
  h_thread = 0;								// start with current thread
  do {
    sts = oz_sys_handle_getinfo (h_thread, 3, items, NULL);		// get thread info
    if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);		// barf if error
    if (h_thread == 0) firstpid = processid;				// save current thread's pid
    else {
      oz_sys_handle_release (OZ_PROCMODE_KNL, h_thread);
      if (processid != firstpid) {					// if thread's parent in a different process, we're done
        if (secondpid == 0) secondpid = processid;
        else if (processid != secondpid) {
          oz_sys_handle_release (OZ_PROCMODE_KNL, h_thread_parent);
          break;
        }
      }
    }
    lasthreadid = threadid;
    h_thread = h_thread_parent;						// check out parent thread
  } while (h_thread != 0);						// repeat if there is a parent thread

  return (lasthreadid);
}

/* Return top thread-id in the same job as caller */

pid_t getpgrp (void)

{
  OZ_Handle h_job, h_thread, h_thread_parent;
  OZ_Job *firstjob, *job;
  OZ_Threadid lasthreadid, threadid;
  uLong sts;

  OZ_Handle_item items[3] = { OZ_HANDLE_CODE_THREAD_ID,     sizeof threadid,        &threadid,        NULL, 
                              OZ_HANDLE_CODE_JOB_HANDLE,    sizeof h_job,           &h_job,           NULL, 
                              OZ_HANDLE_CODE_THREAD_PARENT, sizeof h_thread_parent, &h_thread_parent, NULL };

  OZ_Handle_item jtems[1] = { OZ_HANDLE_CODE_OBJADDR, sizeof job, &job, NULL };

  h_thread = 0;								// start with current thread
  do {
    sts = oz_sys_handle_getinfo (h_thread, 3, items, NULL);		// get thread info
    if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);		// barf if error
    sts = oz_sys_handle_getinfo (h_job, 1, jtems, NULL);		// get job struct pointer
    if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);		// barf if error
    oz_sys_handle_release (OZ_PROCMODE_KNL, h_job);			// don't need job handle anymore
    if (h_thread == 0) firstjob = job;					// save current thread's job
    else {
      oz_sys_handle_release (OZ_PROCMODE_KNL, h_thread);
      if (job != firstjob) {						// if thread's parent in a different job, we're done
        oz_sys_handle_release (OZ_PROCMODE_KNL, h_thread_parent);
        break;
      }
    }
    lasthreadid = threadid;
    h_thread = h_thread_parent;						// check out parent thread
  } while (h_thread != 0);						// repeat if there is a parent thread

  return (lasthreadid);
}

ALIAS (getpid)
ALIAS (getppid)
ALIAS (getpgrp)

/* ?? we do nothing with this ?? */

int getrlimit (int resource, struct rlimit *rlim)

{
  memset (rlim, 0, sizeof *rlim);
  return (0);
}

ALIAS (getrlimit)

/* ?? same here ?? */

int getrusage (int who, struct rusage *usage)

{
  memset (usage, 0, sizeof *usage);
  return (0);
}

/* Return cpu time */

clock_t times (struct tms *buf)

{
  memset (buf, 0, sizeof *buf);
  return (0);
}

ALIAS (times)

clock_t clock (void)

{
  return (0);
}

ALIAS (clock)

void exit (int status)

{
  if (status == 0) oz_sys_thread_exit (OZ_SUCCESS);
  while (1) oz_sys_thread_exit ((status & 0xff) + OZ_CRTL_EXIT);
}

ALIAS (exit)

long int __strtol_internal (const char *nptr, char **endptr, int base, int dummy)

{
  char c;
  const char *p;
  int negative;
  long int accum, accumb, accumn;
  unsigned long int pmo;

  pmo = 0;								/* largest possible integer value */
  pmo --;
  pmo /= 2;
  p = nptr;
  while ((c = *p) != 0 && (c <= ' ')) p ++;				/* skip leading spaces */
  negative = 0;								/* assume result is positive */
  if ((c == '+') || (c == '-')) {					/* check for a leading sign */
    if (c == '-') negative = 1;						/* if a -, set negative flag */
    c = *(++ p);							/* get the following char */
  }
  if ((base == 0) || (base == 16)) {					/* if base 0 or 16, ... */
    if ((c == '0') && ((p[1] == 'x') || (p[1] == 'X'))) {		/* check for leading 0x or 0X */
      base = 16;							/* in which case force base 16 */
      p += 2;								/* skip over the 0x or 0X */
      c = *p;								/* get the following char */
    }
  }
  if ((base == 0) && (c == '0')) base = 8;				/* if base 0 and begins with a '0', force base 8 */
  if (base == 0) base = 10;						/* if still no base, use base 10 */
  pmo /= base;								/* pre-multiply overflow value */
  accum = 0;								/* clear the accumulator */
  while (accum <= pmo) {						/* make sure we can possibly take another digit */
    if ((c >= '0') && (c <= '9') && (c < '0' + base)) {			/* see if 0..9 and within range of base */
      c -= '0';								/* ok, convert digit */
    } else if ((base > 10) && (c >= 'a') && (c < 'a' + base - 10)) {	/* maybe the base requires a letter */
      c -= 'a' - 10;
    } else if ((base > 10) && (c >= 'A') && (c < 'A' + base - 10)) {	/* ... allow capitals, too */
      c -= 'A' - 10;
    } else break;							/* doesn't convert, done scanning */
    accumb = accum * base;						/* multiply accumulator value */
    accumn = accumb + c;						/* add in new digit */
    if (accumn < accumb) break;						/* stop if overflow */
    c = *(++ p);							/* ok, get next character */
    accum = accumn;
  }
  if (endptr != NULL) *endptr = (char *)p;				/* maybe return pointer to first unconverted char */
  if (negative) accum = - accum;					/* maybe result is negative */
  return (accum);							/* return result */
}

long int strtol (const char *nptr, char **endptr, int base)

{
  return (__strtol_internal (nptr, endptr, base, 0));
}

unsigned long int __strtoul_internal (const char *nptr, char **endptr, int base, int dummy)

{
  char c;
  const char *p;
  unsigned long int accum, accumb, accumn, pmo;

  pmo = 0;								/* largest possible integer value */
  pmo --;
  p = nptr;
  while ((c = *p) != 0 && (c <= ' ')) p ++;				/* skip leading spaces */
  if ((base == 0) || (base == 16)) {					/* if base 0 or 16, ... */
    if ((c == '0') && ((p[1] == 'x') || (p[1] == 'X'))) {		/* check for leading 0x or 0X */
      base = 16;							/* in which case force base 16 */
      p += 2;								/* skip over the 0x or 0X */
      c = *p;								/* get the following char */
    }
  }
  if ((base == 0) && (c == '0')) base = 8;				/* if base 0 and begins with a '0', force base 8 */
  if (base == 0) base = 10;						/* if still no base, use base 10 */
  pmo /= base;								/* pre-multiply overflow value */
  accum = 0;								/* clear the accumulator */
  while (accum <= pmo) {						/* make sure we can possibly take another digit */
    if ((c >= '0') && (c <= '9') && (c < '0' + base)) {			/* see if 0..9 and within range of base */
      c -= '0';								/* ok, convert digit */
    } else if ((base > 10) && (c >= 'a') && (c < 'a' + base - 10)) {	/* maybe the base requires a letter */
      c -= 'a' - 10;
    } else if ((base > 10) && (c >= 'A') && (c < 'A' + base - 10)) {	/* ... allow capitals, too */
      c -= 'A' - 10;
    } else break;							/* doesn't convert, done scanning */
    accumb = accum * base;						/* multiply accumulator value */
    accumn = accumb + c;						/* add in new digit */
    if (accumn < accumb) break;						/* stop if overflow */
    c = *(++ p);							/* ok, get next character */
    accum = accumn;
  }
  if (endptr != NULL) *endptr = (char *)p;				/* maybe return pointer to first unconverted char */
  return (accum);							/* return result */
}

unsigned long int strtoul (const char *nptr, char **endptr, int base)

{
  return (__strtoul_internal (nptr, endptr, base, 0));
}

/************************************************************************/
/*									*/
/*  Get environmental variable - get value from logical name table	*/
/*									*/
/************************************************************************/

char *getenv (const char *name)

{
  char *buff;
  uLong rlen, sts;
  OZ_Handle h_logical, h_table;

  buff = NULL;
  sts = oz_sys_logname_lookup (0, OZ_PROCMODE_USR, oz_s_logname_defaulttables, NULL, NULL, NULL, &h_table);
  if (sts == OZ_SUCCESS) {
    sts = oz_sys_logname_lookup (h_table, OZ_PROCMODE_USR, name, NULL, NULL, NULL, &h_logical);
    if (sts == OZ_SUCCESS) {
      sts = oz_sys_logname_getval (h_logical, 0, NULL, 0, NULL, &rlen, NULL, 0, NULL);
      if (sts == OZ_SUCCESS) {
        buff = malloc (rlen + 1);
        sts = oz_sys_logname_getval (h_logical, 0, NULL, rlen + 1, buff, NULL, NULL, 0, NULL);
        if (sts != OZ_SUCCESS) {
          free (buff);
          buff = NULL;
        }
      }
      oz_sys_handle_release (OZ_PROCMODE_KNL, h_logical);
    }
    oz_sys_handle_release (OZ_PROCMODE_KNL, h_table);
  }
  return (buff);
}

/************************************************************************/
/*									*/
/*  Create environmental variable by creating logical name		*/
/*									*/
/************************************************************************/

int putenv (const char *string)

{
  char *namex;
  const char *name, *p;
  uLong sts;
  OZ_Handle h_table;
  OZ_Logvalue values[1];

  namex = NULL;
  p = strchr (string, '=');			// see if it contains an '='
  if (p == NULL) {
    name = string;				// if not, name is the whole string
    p = "";					// ... and value is a null string
  } else {
    namex = malloc (p - string + 1);		// if so, get just the name in namex
    memcpy (namex, string, p - string);
    namex[p-string] = 0;			// ... null terminated
    name = namex;
    p ++;					// ... and everything after the '=' is the value
  }

  sts = oz_sys_logname_lookup (0, OZ_PROCMODE_USR, oz_s_logname_defaulttables, NULL, NULL, NULL, &h_table);
  if (sts == OZ_SUCCESS) {
    values[0].attr = 0;
    values[0].buff = (void *)p;
    sts = oz_sys_logname_create (h_table, name, OZ_PROCMODE_USR, 0, 1, values, NULL);
    oz_sys_handle_release (OZ_PROCMODE_KNL, h_table);
  }
  if (namex != NULL) free (namex);
  if ((sts == OZ_SUCCESS) || (sts == OZ_SUPERSEDED)) return (0);
  errno = EOZSYSERR;
  errno_ozsts = sts;
  return (-1);
}

/************************************************************************/
/*									*/
/*  Read a password string from console					*/
/*									*/
/************************************************************************/

char *getpass (const char *prompt)

{
  static char buff[256];
  uLong rlen, sts;
  OZ_Handle h_console;
  OZ_IO_console_read console_read;

  sts = oz_sys_io_assign (OZ_PROCMODE_KNL, &h_console, "OZ_CONSOLE", OZ_LOCKMODE_CW);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printerror ("oz_crtl_misc getpass: error %u assigning channel to OZ_CONSOLE\n", sts);
    return (NULL);
  }

  memset (&console_read, 0, sizeof console_read);
  console_read.size    = sizeof buff - 1;
  console_read.buff    = buff;
  console_read.rlen    = &rlen;
  console_read.pmtsize = strlen (prompt);
  console_read.pmtbuff = prompt;
  console_read.noecho  = 1;
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_console, 0, OZ_IO_CONSOLE_READ, sizeof console_read, &console_read);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printerror ("oz_crtl_misc_sleep: error %u reading from OZ_CONSOLE\n", sts);
    return (NULL);
  }

  oz_sys_handle_release (OZ_PROCMODE_KNL, h_console);
  buff[rlen] = 0;
  return (buff);
}

int atoi (const char *nptr)

{
  int i;

  return (oz_hw_atoi (nptr, &i));
}

char *mktemp (char *template)

{
  int i, j;
  uLong n, num;

  static int seq = 0;
  static const char cvt[] = "abcdefghijklmnopqrstuvwxyz-_0123456789";

  i = strlen (template) - 6;
  if ((i < 0) || (strcmp (template + i, "XXXXXX") != 0)) {
    errno = EINVAL;
    return (NULL);
  }

  num  = (int)oz_hw_atomic_inc_long (&seq, 1);
  num += oz_hw_tod_getnow ();

  for (j = 0; j < 6; j ++) {
    n = num % 38;
    num /= 38;
    template[i+j] = cvt[n];
  }
}

size_t strspn (const char *s, const char *accept)

{
  char c;
  size_t i;

  for (i = 0; (c = s[i]) != 0; i ++) if (strchr (accept, c) == NULL) break;
  return (i);
}

size_t strcspn (const char *s, const char *reject)

{
  char c;
  size_t i;

  for (i = 0; (c = s[i]) != 0; i ++) if (strchr (reject, c) != NULL) break;
  return (i);
}

char *strpbrk (const char *s, const char *accept)

{
  char c;
  const char *p;

  for (p = s; (c = *p) != 0; p ++) {
    if (strchr (accept, c) != NULL) return ((char *)p);
  }
  return (NULL);
}


char *strtok (char *s, const char *delim)

{
  char *token;

  static char *olds = NULL;

  if (s == NULL) s = olds;

  s += strspn (s, delim);			/* scan leading delimiters */
  if (*s == 0) return (NULL);

  token = s;					/* find the end of the token */
  s = strpbrk (token, delim);
  if (s == NULL) olds = strchr (token, 0);	/* this token finishes the string */
  else {
   *s = 0;					/* terminate the token and make olds point past it */
   olds = s + 1;
  }
  return (token);
}

char *dirname (char *path)

{
  char *p;

  if ((path == NULL) || (*path == 0)) return (".");
  if (strcmp (path, "/") == 0) return ("/");
  p = strrchr (path, '/');
  if (p == NULL) return (".");
  *p = 0;
  return (path);
}

char *basename (char *path)

{
  char *p;

  if ((path == NULL) || (*path == 0)) return (".");
  if (strcmp (path, "/") == 0) return ("/");
  p = strrchr (path, '/');
  if (p == NULL) return (path);
  return (++ p);
}
