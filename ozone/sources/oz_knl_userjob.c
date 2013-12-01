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
/*  Create and maintain user and job blocks				*/
/*									*/
/*  There is one and only one user block for each username in use on 	*/
/*  the system.  There is one and only one job block for each console 	*/
/*  in use on the system.						*/
/*									*/
/*  There is a hierarchy of objects as follows:				*/
/*									*/
/*    (the system)							*/
/*      <user block>							*/
/*        <job block>							*/
/*          <process block>						*/
/*            <thread block>						*/
/*									*/
/************************************************************************/

#define _OZ_KNL_USERJOB_C

#include "ozone.h"

#include "oz_knl_devio.h"
#include "oz_knl_event.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_logname.h"
#include "oz_knl_objtype.h"
#include "oz_knl_process.h"
#include "oz_knl_quota.h"
#include "oz_knl_sdata.h"
#include "oz_knl_status.h"
#include "oz_knl_userjob.h"

struct OZ_User { OZ_Objtype objtype;			/* object type OZ_OBJTYPE_USER */
                 OZ_User *next;				/* next in 'users' list */
                 Long refcount;				/* reference count */
                 char username[OZ_USERNAME_MAX];	/* user's username (null terminated string) */
                 OZ_Logname *lognamdir;			/* user's logical name directory (OZ_USER_DIRECTORY) */
                 OZ_Logname *lognamtbl;			/* user's logical name table (OZ_USER_TABLE) */
                 OZ_Logname *logname;			/* user's logical name */
                 OZ_Quota *quota;			/* user's quota block */
                 OZ_Job *jobs;				/* list of jobs for this user */
                 OZ_Datebin loggedon;			/* when this user first logged on */
                 OZ_Seckeys *seckeys;			/* user's security keys from the password file */
                 uLong maxbasepri;			/* user's max allowed base priority from the password file */
                 OZ_Devunit *devalloc;			/* devices allocated to this user */
               };

struct OZ_Job { OZ_Objtype objtype;			/* object type OZ_OBJTYPE_JOB */
                OZ_Job *next;				/* next in 'user -> jobs' list */
                Long refcount;				/* reference count */
                char jobname[OZ_JOBNAME_MAX];		/* job's name (null terminated string) */
                OZ_User *user;				/* pointer to OZ_User struct */
                OZ_Logname *lognamdir;			/* job's logical name directory (OZ_JOB_DIRECTORY) */
                OZ_Logname *lognamtbl;			/* job's logical name table (OZ_JOB_TABLE) */
                OZ_Logname *logname;			/* job's logical name */
                OZ_Devunit *devalloc;			/* devices allocated to this job */
              };

static int lockcount = 0;
static uLong usercount = 0;
static OZ_Event *lockevent = NULL;
static OZ_Thread *lockthread = NULL;
static OZ_User *users = NULL;

static Long user_increfc (OZ_User *user, Long inc);
static void lock_wait (void);
static void lock_clr (void);

/************************************************************************/
/*									*/
/*  Initialize static data at boot time					*/
/*									*/
/************************************************************************/

void oz_knl_userjob_init (void)

{
  uLong sts;

  users = NULL;								/* ain't got no users */
  usercount = 0;
  sts = oz_knl_event_create (19, "oz_knl_userjob lock", NULL, &lockevent); /* create locking flag */
  if (sts != OZ_SUCCESS) oz_crash ("oz_knl_userjob_init: error %u creating lockevent", sts);
  lockcount = 0;
  oz_knl_event_set (lockevent, 1);					/* say the list is unlocked */
}

/************************************************************************/
/*									*/
/*  Create user block for a user (that just logged in)			*/
/*									*/
/*    Input:								*/
/*									*/
/*	username   = username of user logging in			*/
/*	quota      = pointer to quota for the user			*/
/*	seckeys    = security keys from the password file		*/
/*	maxbasepri = max base priority from the password file		*/
/*	*user_r    = where to return user block pointer			*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_user_create = OZ_SUCCESS : successful			*/
/*	                           else : error status			*/
/*	*user_r = pointer to user block					*/
/*									*/
/*    Note:								*/
/*									*/
/*	caller must decrement the reference count when done with the 	*/
/*	pointer returned in user_r					*/
/*									*/
/************************************************************************/

uLong oz_knl_user_create (const char *username, OZ_Quota *quota, OZ_Seckeys *seckeys, uLong maxbasepri, OZ_User **user_r)

{
  int i;
  OZ_Logvalue logvalue;
  OZ_Secattr *secattr;
  OZ_User **luser, *user;
  uLong qu, se, sts;

  i = strlen (username);
  if (i >= OZ_USERNAME_MAX) return (OZ_BADUSERNAME);

  secattr = oz_knl_thread_getdefcresecattr (NULL);				/* security attribs to give to user struct */
  lock_wait ();									/* keep others out */

  /* See if username already in list (list is sorted by username) */

  for (luser = &users; (user = *luser) != NULL; luser = &(user -> next)) {	/* scan existing list */
    i = strcmp (user -> username, username);					/* compare existing username to new username */
    if (i >= 0) break;								/* stop if existing username >= new username */
  }

  /* If not there, create a new OZ_User struct and insert in list */

  if ((user == NULL) || (i > 0)) {						/* if at end of list or existing username > new username, we need a new one */
    user = OZ_KNL_PGPMALLOC (sizeof *user);					/* allocate new user struct */
    user -> objtype    = OZ_OBJTYPE_USER;					/* set up object type */
    user -> next       = *luser;						/* set up link to next in 'users' list */
    user -> refcount   = 1;							/* this is for the pointer that is returned to caller */
    user -> seckeys    = NULL;							/* ain't got no keys yet */
    strncpyz (user -> username, username, sizeof user -> username);		/* save the username */
    user -> quota      = quota;							/* fill in null quota block */
    if (quota != NULL) oz_knl_quota_increfc (quota, 1);
    user -> jobs       = NULL;							/* it doesn't have any jobs yet */
    user -> devalloc   = NULL;							/* it doesn't have any devices allocated yet */

    sts = oz_knl_logname_create (NULL, 
                                 OZ_PROCMODE_KNL, 
                                 NULL, 
                                 secattr, 
                                 OZ_LOGNAMATR_TABLE | OZ_LOGNAMATR_NOSUPERSEDE | OZ_LOGNAMATR_NOOUTERMODE, 
                                 strlen (oz_s_logname_directorynames[2]), 
                                 oz_s_logname_directorynames[2], 
                                 0, 
                                 NULL, 
                                 &(user -> lognamdir));
    if (sts != OZ_SUCCESS) {
      oz_knl_printk ("oz_knl_userjob_create: error %u creating user %s logical name directory %s\n", sts, user -> username, oz_s_logname_directorynames[2]);
      goto rtnerr;
    }
    sts = oz_knl_logname_create (user -> lognamdir, 
                                 OZ_PROCMODE_KNL, 
                                 NULL, 
                                 secattr, 
                                 OZ_LOGNAMATR_TABLE | OZ_LOGNAMATR_NOSUPERSEDE | OZ_LOGNAMATR_NOOUTERMODE, 
                                 strlen (oz_s_logname_deftblnames[3]), 
                                 oz_s_logname_deftblnames[3], 
                                 0, 
                                 NULL, 
                                 &(user -> lognamtbl));
    if (sts != OZ_SUCCESS) {
      oz_knl_printk ("oz_knl_userjob_create: error %u creating user %s logical name table %s\n", sts, user -> username, oz_s_logname_deftblnames[3]);
      goto rtnerr1;
    }
    sts = oz_knl_logname_creobj (user -> lognamtbl, 
                                 OZ_PROCMODE_KNL, 
                                 NULL, 
                                 secattr, 
                                 OZ_LOGNAMATR_NOSUPERSEDE | OZ_LOGNAMATR_NOOUTERMODE, 
                                 12, "OZ_THIS_USER", 
                                 user, 
                                 &(user -> logname));
    if (sts != OZ_SUCCESS) {
      oz_knl_printk ("oz_knl_userjob_create: error %u creating user %s logical\n", sts, user -> username);
      goto rtnerr2;
    }
    logvalue.attr = OZ_LOGVALATR_TERMINAL;
    logvalue.buff = user -> username;
    sts = oz_knl_logname_create (user -> lognamtbl, 
                                 OZ_PROCMODE_KNL, 
                                 NULL, 
                                 secattr, 
                                 OZ_LOGNAMATR_NOSUPERSEDE | OZ_LOGNAMATR_NOOUTERMODE, 
                                 11, "OZ_USERNAME", 
                                 1, 
                                 &logvalue, 
                                 NULL);
    if (sts != OZ_SUCCESS) {
      oz_knl_printk ("oz_knl_userjob_create: error %u creating user %s logical name\n", sts, user -> username);
      goto rtnerr3;
    }

    *luser = user;								/* link it to list of users */
    usercount ++;
    user -> loggedon = oz_hw_tod_getnow ();					/* get time of logon */
    oz_knl_printk ("oz_knl_user_create: user %s logged on at %t\n", user -> username, user -> loggedon);
  }

  /* Already exists, inc ref count for the pointer that is returned to caller and update quota maxima */

  else {
    user_increfc (user, 1);
    switch (((user -> quota != NULL) << 1) + (quota != NULL)) {
      case 0: break;
      case 1: {
        user -> quota = quota;				/* no limits before and there are limits now */
        oz_knl_quota_increfc (quota, 1);
        break;
      }
      case 2: {
        qu = oz_hw_smplock_wait (&oz_s_smplock_qu);
        quota = user -> quota;				/* there were limits but/and there are no limits now */
        user -> quota = NULL;
        oz_hw_smplock_clr (&oz_s_smplock_qu, qu);
        oz_knl_quota_increfc (quota, -1);
        break;
      }
      case 3: {
        oz_knl_quota_update (user -> quota, quota);	/* there were limits and there are (possibly different) limits now */
        break;
      }
    }
  }

  /* Anyhoo, change the user's default seckeys to the given ones */

  if (oz_knl_seckeys_differ (user -> seckeys, seckeys)) {
    se = oz_hw_smplock_wait (&oz_s_smplock_se);
    oz_knl_seckeys_increfc (user -> seckeys, -1);
    user -> seckeys = seckeys;
    oz_knl_seckeys_increfc (user -> seckeys,  1);
    oz_hw_smplock_clr (&oz_s_smplock_se, se);
  }

  /* Change the max base priority to the given value */

  user -> maxbasepri = maxbasepri;

  /* Return pointer to user struct */

  *user_r = user;
  sts = OZ_SUCCESS;

  goto rtn;

rtnerr3:
  oz_knl_logname_delete (user -> logname);
rtnerr2:
  oz_knl_logname_delete (user -> lognamtbl);
rtnerr1:
  oz_knl_logname_delete (user -> lognamdir);
rtnerr:
  if (user -> quota != NULL) oz_knl_quota_increfc (user -> quota, -1);
  oz_knl_devunit_dallocall (&(user -> devalloc));
  OZ_KNL_NPPFREE (user);
rtn:

  lock_clr ();									/* allow others access to list */
  oz_knl_secattr_increfc (secattr, -1);						/* done with security attributes */
  return (sts);									/* return completion status */
}

/************************************************************************/
/*									*/
/*  Create job block							*/
/*									*/
/*    Input:								*/
/*									*/
/*	user = user that job belongs to					*/
/*	       or NULL to default to current user			*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_job_create = OZ_SUCCESS : successful				*/
/*	                      else : error status			*/
/*	*job_r  = pointer to job block					*/
/*									*/
/*    Note:								*/
/*									*/
/*	caller must decrement the job reference count when done with 	*/
/*	the pointer							*/
/*									*/
/************************************************************************/

uLong oz_knl_job_create (OZ_User *user, const char *name, OZ_Job **job_r)

{
  OZ_Job *job;
  OZ_Secattr *secattr;
  uLong sts;

  OZ_KNL_CHKOBJTYPE (user, OZ_OBJTYPE_USER);

  secattr = oz_knl_thread_getdefcresecattr (NULL);	/* security attribs to give to job struct */
  lock_wait ();						/* keep others out */

  if (user == NULL) {
    job  = oz_knl_process_getjob (NULL);
    user = job -> user;
  }

  /* Create a job struct */

  job = OZ_KNL_PGPMALLOQ (sizeof *job);			/* allocate job block (PGP because nothing in it accessed with smplocks) */
  if (job == NULL) {
    sts = OZ_EXQUOTAPGP;
    goto rtn;
  }
  job -> objtype  = OZ_OBJTYPE_JOB;			/* set up the object type */
  job -> next     = user -> jobs;			/* link it to list of jobs for the user */
  job -> refcount = 1;					/* ref count = 1 (returned pointer) */
  job -> user     = user;				/* point to the owning user block */
  job -> devalloc = NULL;				/* it doesn't have any devices allocated yet */
  strncpyz (job -> jobname, name, sizeof job -> jobname); /* save name string */

  /* Create logical name stuff */

  sts = oz_knl_logname_create (NULL, 
                               OZ_PROCMODE_KNL, 
                               NULL, 
                               secattr, 
                               OZ_LOGNAMATR_TABLE | OZ_LOGNAMATR_NOSUPERSEDE | OZ_LOGNAMATR_NOOUTERMODE, 
                               strlen (oz_s_logname_directorynames[1]), 
                               oz_s_logname_directorynames[1], 
                               0, 
                               NULL, 
                               &(job -> lognamdir));
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_knl_userjob_create: error %u creating job logical name directory %s\n", sts, oz_s_logname_directorynames[1]);
    goto rtnerr;
  }

  sts = oz_knl_logname_create (job -> lognamdir, 
                               OZ_PROCMODE_KNL, 
                               NULL, 
                               secattr, 
                               OZ_LOGNAMATR_TABLE | OZ_LOGNAMATR_NOSUPERSEDE | OZ_LOGNAMATR_NOOUTERMODE, 
                               strlen (oz_s_logname_deftblnames[2]), 
                               oz_s_logname_deftblnames[2], 
                               0, 
                               NULL, 
                               &(job -> lognamtbl));
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_knl_userjob_create: error %u creating job logical name table %s\n", sts, oz_s_logname_deftblnames[2]);
    goto rtnerr1;
  }

  sts = oz_knl_logname_creobj (job -> lognamtbl, 
                               OZ_PROCMODE_KNL, 
                               NULL, 
                               secattr, 
                               OZ_LOGNAMATR_NOSUPERSEDE | OZ_LOGNAMATR_NOOUTERMODE, 
                               11, "OZ_THIS_JOB", 
                               job, 
                               &(job -> logname));
  if (sts != OZ_SUCCESS) {
    oz_knl_printk ("oz_knl_userjob_create: error %u creating job logical OZ_THIS_JOB\n", sts);
    goto rtnerr2;
  }

  /* Finish linking it to user block */

  user -> jobs = job;					/* link it to user block */

  /* Return pointer to job struct */

  *job_r = job;
  sts = OZ_SUCCESS;
  goto rtn;

rtnerr2:
  oz_knl_logname_delete (job -> lognamtbl);
rtnerr1:
  oz_knl_logname_delete (job -> lognamdir);
rtnerr:
  oz_knl_devunit_dallocall (&(job -> devalloc));
  OZ_KNL_PGPFREE (job);

rtn:
  lock_clr ();						/* allow others access to list */
  oz_knl_secattr_increfc (secattr, -1);			/* done with security attributes */
  return (sts);						/* return completion status */
}

/************************************************************************/
/*									*/
/*  Increment user block reference count				*/
/*									*/
/*    Input:								*/
/*									*/
/*	user = points to user block					*/
/*	inc  = amount to increment by					*/
/*									*/
/*	smplock <= softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_user_increfc = new reference count			*/
/*									*/
/*    Note:								*/
/*									*/
/*	structure is deleted if new ref count is zero			*/
/*									*/
/************************************************************************/

Long oz_knl_user_increfc (OZ_User *user, Long inc)

{
  Long refc;

  OZ_KNL_CHKOBJTYPE (user, OZ_OBJTYPE_USER);

  lock_wait ();				/* keep others from mucking with list */
  refc = user_increfc (user, inc);	/* make modifications */
  lock_clr ();				/* allow others to modify list */
  return (refc);			/* return new reference count */
}

/* Internal entrypoint when caller already has lockevent */

static Long user_increfc (OZ_User *user, Long inc)

{
  Long base, refc;
  OZ_Logname *logname;
  OZ_User **luser, *xuser;

  user -> refcount += inc;				/* increment reference count */
  refc = user -> refcount;				/* get the new value */
  base = (user -> logname != NULL);
  if (refc < base) oz_crash ("oz_knl_user_increfc: ref count went negative (%d vs %d)", refc, base);
  if ((refc == base) && (user -> jobs == NULL)) {	/* see if ref count now zero and jobs list empty */
    for (luser = &users; (xuser = *luser) != NULL; luser = &(xuser -> next)) {
      if (xuser == user) {
        *luser = xuser -> next;				/* unlink from users list */
        user -> loggedon = oz_hw_tod_getnow ();
        oz_knl_printk ("oz_knl_user_increfc: user %s logged off at %t\n", user -> username, user -> loggedon);
        usercount --;
        break;
      }
    }
    if (user -> lognamtbl != NULL) {
      oz_knl_logname_delete (user -> lognamtbl);	/* mark logical name table for delete */
      user -> lognamtbl = NULL;
    }
    if (user -> lognamdir != NULL) {
      oz_knl_logname_delete (user -> lognamdir);	/* mark logical name directory for delete */
      user -> lognamdir = NULL;
    }
    logname = user -> logname;				/* see if OZ_THIS_USER logical still around */
    if (logname != NULL) {
      user -> logname = NULL;				/* if so, say it is gone */
      oz_knl_logname_delete (logname);			/* delete it - this calls us back to finish deleting user */
    } else {
      if (user -> quota != NULL) oz_knl_quota_increfc (user -> quota, -1);
      oz_knl_devunit_dallocall (&(user -> devalloc));
      OZ_KNL_PGPFREE (user);				/* already gone, free off the user block */
    }
  }

  return (refc);
}

/************************************************************************/
/*									*/
/*  Increment job block reference count					*/
/*									*/
/*    Input:								*/
/*									*/
/*	job = points to job block					*/
/*	inc = amount to increment by					*/
/*									*/
/*	smplock <= softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_job_increfc = new reference count			*/
/*									*/
/*    Note:								*/
/*									*/
/*	structure is deleted if new ref count is zero			*/
/*									*/
/************************************************************************/

Long oz_knl_job_increfc (OZ_Job *job, Long inc)

{
  Long base, refc;
  OZ_Job **ljob, *xjob;
  OZ_Logname *logname;
  OZ_User *user;

  if (job == NULL) oz_crash ("oz_knl_job_increfc: called (NULL, %d)\n", inc);

  OZ_KNL_CHKOBJTYPE (job, OZ_OBJTYPE_JOB);

  lock_wait ();						/* keep others from mucking with list */
  job -> refcount += inc;				/* increment reference count */
  base = (job -> logname != NULL);
  refc = job -> refcount;				/* get the new value */
  if (refc < base) oz_crash ("oz_knl_job_increfc: ref count went negative (%d vs %d)", refc, base);
  if (refc == base) {					/* see if ref count now zero */
    user = job -> user;					/* see if still associated with a user block */
    if (user != NULL) {
      for (ljob = &(user -> jobs); (xjob = *ljob) != job; ljob = &(xjob -> next)) {
        if (xjob == NULL) oz_crash ("oz_knl_job_increfc: job not found on user -> jobs list");
      }
      *ljob = job -> next;
      job -> user = NULL;
    }
    if (job -> lognamtbl != NULL) {
      oz_knl_logname_delete (job -> lognamtbl);		/* mark logical name table for delete */
      job -> lognamtbl = NULL;
    }
    if (job -> lognamdir != NULL) {
      oz_knl_logname_delete (job -> lognamdir);		/* mark logical name directory for delete */
      job -> lognamdir = NULL;
    }
    logname = job -> logname;				/* see if OZ_THIS_JOB logical still around */
    if (logname != NULL) {
      job -> logname = NULL;				/* if so, say it is gone */
      oz_knl_logname_delete (logname);			/* delete it - this calls us back to finish deleting job */
    } else {
      oz_knl_devunit_dallocall (&(job -> devalloc));
      OZ_KNL_PGPFREE (job);				/* already gone, free off the job block */
    }
    if (user != NULL) user_increfc (user, 0);		/* maybe user -> jobs is now null */
  }
  lock_clr ();						/* allow others to modify list */
  return (refc);					/* return new reference count */
}

/************************************************************************/
/*									*/
/*  Get various job values						*/
/*									*/
/************************************************************************/

char *oz_knl_job_getname (OZ_Job *job)

{
  if (job == NULL) job = oz_knl_process_getjob (NULL);
  OZ_KNL_CHKOBJTYPE (job, OZ_OBJTYPE_JOB);
  return (job -> jobname);
}

OZ_User *oz_knl_job_getuser (OZ_Job *job)

{
  if (job == NULL) job = oz_knl_process_getjob (NULL);
  OZ_KNL_CHKOBJTYPE (job, OZ_OBJTYPE_JOB);
  return (job -> user);
}

OZ_Logname *oz_knl_job_getlognamdir (OZ_Job *job)

{
  if (job == NULL) job = oz_knl_process_getjob (NULL);
  OZ_KNL_CHKOBJTYPE (job, OZ_OBJTYPE_JOB);
  return (job -> lognamdir);
}

OZ_Logname *oz_knl_job_getlognamtbl (OZ_Job *job)

{
  if (job == NULL) job = oz_knl_process_getjob (NULL);
  OZ_KNL_CHKOBJTYPE (job, OZ_OBJTYPE_JOB);
  return (job -> lognamtbl);
}

OZ_Job *oz_knl_job_getnext (OZ_Job *lastjob, OZ_User *user)

{
  OZ_Job *job;

  lock_wait ();
  if (lastjob == NULL) job = user -> jobs;
  else job = lastjob -> next;
  if (job != NULL) oz_knl_job_increfc (job, 1);
  lock_clr ();

  return (job);
}

uLong oz_knl_job_count (OZ_User *user)

{
  uLong count;
  OZ_Job *job;

  lock_wait ();

  count = 0;
  for (job = user -> jobs; job != NULL; job = job -> next) count ++;

  lock_clr ();

  return (count);
}

OZ_Devunit **oz_knl_job_getdevalloc (OZ_Job *job)

{
  OZ_KNL_CHKOBJTYPE (job, OZ_OBJTYPE_JOB);
  return (&(job -> devalloc));
}

/************************************************************************/
/*									*/
/*  Get various user values						*/
/*									*/
/************************************************************************/

char *oz_knl_user_getname (OZ_User *user)

{
  if (user == NULL) user = oz_knl_process_getjob (NULL) -> user;
  OZ_KNL_CHKOBJTYPE (user, OZ_OBJTYPE_USER);
  return (user -> username);
}

OZ_Quota *oz_knl_user_getquota (OZ_User *user)

{
  OZ_Quota *quota;
  uLong qu;

  if (user == NULL) user = oz_knl_process_getjob (NULL) -> user;
  OZ_KNL_CHKOBJTYPE (user, OZ_OBJTYPE_USER);
  qu = oz_hw_smplock_wait (&oz_s_smplock_qu);
  quota = user -> quota;
  if (quota != NULL) oz_knl_quota_increfc (quota, 1);
  oz_hw_smplock_clr (&oz_s_smplock_qu, qu);
  return (quota);
}

OZ_Logname *oz_knl_user_getlognamdir (OZ_User *user)

{
  if (user == NULL) user = oz_knl_process_getjob (NULL) -> user;
  OZ_KNL_CHKOBJTYPE (user, OZ_OBJTYPE_USER);
  return (user -> lognamdir);
}

OZ_Logname *oz_knl_user_getlognamtbl (OZ_User *user)

{
  if (user == NULL) user = oz_knl_process_getjob (NULL) -> user;
  OZ_KNL_CHKOBJTYPE (user, OZ_OBJTYPE_USER);
  return (user -> lognamtbl);
}

uLong oz_knl_user_count (void)

{
  return (usercount);
}

OZ_User *oz_knl_user_getnext (OZ_User *lastuser)

{
  OZ_User *user;

  lock_wait ();
  if (lastuser == NULL) user = users;
  else user = lastuser -> next;
  if (user != NULL) user_increfc (user, 1);
  lock_clr ();

  return (user);
}

OZ_Seckeys *oz_knl_user_getseckeys (OZ_User *user)

{
  OZ_Seckeys *seckeys;
  uLong se;

  if (user == NULL) user = oz_knl_process_getjob (NULL) -> user;
  OZ_KNL_CHKOBJTYPE (user, OZ_OBJTYPE_USER);
  se = oz_hw_smplock_wait (&oz_s_smplock_se);
  seckeys = user -> seckeys;
  oz_knl_seckeys_increfc (seckeys, 1);
  oz_hw_smplock_clr (&oz_s_smplock_se, se);
  return (seckeys);
}

uLong oz_knl_user_getmaxbasepri (OZ_User *user)

{
  if (user == NULL) user = oz_knl_process_getjob (NULL) -> user;
  OZ_KNL_CHKOBJTYPE (user, OZ_OBJTYPE_USER);
  return (user -> maxbasepri);
}

OZ_Devunit **oz_knl_user_getdevalloc (OZ_User *user)

{
  OZ_KNL_CHKOBJTYPE (user, OZ_OBJTYPE_USER);
  return (&(user -> devalloc));
}

/************************************************************************/
/*									*/
/*  Wait until no other thread is accessing list and lock it		*/
/*									*/
/************************************************************************/

static void lock_wait (void)

{
  OZ_Thread *thread;

  thread = oz_knl_thread_getcur ();			/* get my thread address */

  while (oz_knl_event_set (lockevent, 0) <= 0) {	/* lock it and see if anyone had it locked already */
    if (lockthread == thread) break;			/* it was locked, break out if I had it locked */
    oz_knl_event_waitone (lockevent);			/* someone else had it locked, wait for them to finish */
  }

  lockthread = thread;					/* save who has it locked */
  lockcount ++;						/* increment number of times I locked it */
}

/************************************************************************/
/*									*/
/*  De-access list							*/
/*									*/
/************************************************************************/

static void lock_clr (void)

{
  if (-- lockcount == 0) {		/* decrement count of number of times I locked it */
    lockthread = NULL;			/* if zero, say no one has it locked */
    oz_knl_event_set (lockevent, 1);	/* ... and release anyone else that is waiting */
  }
}
