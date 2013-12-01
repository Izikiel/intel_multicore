//+++2002-08-17
//    Copyright (C) 2001,2002  Mike Rieker, Beverly, MA USA
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
//---2002-08-17

/************************************************************************/
/*									*/
/*  This program prompts a user for username and password, then spawns 	*/
/*  the cli for the user, creating new user and job blocks		*/
/*									*/
/*  If arg 1 is supplied, it is taken as the username			*/
/*  If arg 2 is supplied, it is taken as the password			*/
/*									*/
/*  This program is spawned by oz_knl_logon and runs as part of the 	*/
/*  system job and thus has all privileges.				*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_crtl_malloc.h"
#include "oz_io_console.h"
#include "oz_knl_devio.h"
#include "oz_knl_handle.h"
#include "oz_knl_lock.h"
#include "oz_knl_logname.h"
#include "oz_knl_procmode.h"
#include "oz_knl_security.h"
#include "oz_knl_status.h"
#include "oz_knl_user_become.h"
#include "oz_knl_userjob.h"
#include "oz_sys_callknl.h"
#include "oz_sys_handle.h"
#include "oz_sys_io.h"
#include "oz_sys_io_fs.h"
#include "oz_sys_password.h"
#include "oz_sys_spawn.h"
#include "oz_sys_thread.h"
#include "oz_util_start.h"

typedef struct { char *username;
                 uLong seckeyssize;
                 void *seckeysbuff;
                 uLong secattrsize;
                 void *secattrbuff;
                 char *quotastr;
                 OZ_Handle h_job;
               } Creprm;

static void *secmalloc (void *dummy, uLong osize, void *obuff, uLong nsize);
static uLong create_userjob (OZ_Procmode cprocmode, void *creprmv);

uLong oz_util_main (int argc, char *argv[])

{
  char msgbuf[4096], passhash[OZ_PASSWORD_HASHSIZE], password[OZ_PASSWORD_MAX], username[OZ_USERNAME_MAX];
  char *pw_defdirp, *pw_imagenamep, pw_passhash[OZ_PASSWORD_HASHSIZE];
  const char *spawn_argv[2];
  Creprm creprm;
  int index;
  OZ_Handle h_logname, h_thread;
  OZ_IO_console_read console_read;
  OZ_IO_console_write console_write;
  uLong msglen, password_l, sts, username_l;

  OZ_Password_item pwitems[3] = { OZ_PASSWORD_CODE_PASSHASH,   sizeof pw_passhash,    pw_passhash,   NULL, 
                                  OZ_PASSWORD_CODE_IMAGENAMEP, sizeof pw_imagenamep, &pw_imagenamep, NULL, 
                                  OZ_PASSWORD_CODE_DEFDIRP,    sizeof pw_defdirp,    &pw_defdirp,    NULL };

  /* If logical OZ_UTIL_LOGON_MSG is defined, print out the message contained therein */

  memset (&console_write, 0, sizeof console_write);
  console_write.trmsize = 2;
  console_write.trmbuff = "\r\n";
  sts = oz_sys_logname_lookup (0, OZ_PROCMODE_USR, "OZ_SYSTEM_TABLE%OZ_UTIL_LOGON_MSG", NULL, NULL, NULL, &h_logname);
  if (sts == OZ_SUCCESS) {
    for (index = 0;; index ++) {
      sts = oz_sys_logname_getval (h_logname, index, NULL, sizeof msgbuf, msgbuf, &msglen, NULL, 0, NULL);
      if (sts != OZ_SUCCESS) break;
      console_write.size = msglen;
      console_write.buff = msgbuf;
      oz_sys_io (OZ_PROCMODE_KNL, oz_util_h_output, 0, OZ_IO_CONSOLE_WRITE, sizeof console_write, &console_write);
    }
    oz_sys_handle_release (OZ_PROCMODE_KNL, h_logname);
  }

  /* Read username from console if not supplied as arg[1] */

  if (argc > 1) {
    strncpyz (username, argv[1], sizeof username);
    username_l = strlen (username);
  } else {
    console_write.size = 0;
    oz_sys_io (OZ_PROCMODE_KNL, oz_util_h_input, 0, OZ_IO_CONSOLE_WRITE, sizeof console_write, &console_write);
    do {
      memset (&console_read, 0, sizeof console_read);
      console_read.size    = sizeof username - 1;
      console_read.buff    = username;
      console_read.rlen    = &username_l;
      console_read.pmtsize = 10;
      console_read.pmtbuff = "Username: ";
      console_read.timeout = 30000;

      sts = oz_sys_io (OZ_PROCMODE_KNL, oz_util_h_input, 0, OZ_IO_CONSOLE_READ, sizeof console_read, &console_read);
      if (sts != OZ_SUCCESS) {
        oz_sys_io_fs_printf (oz_util_h_error, "oz_util_logon: error %u reading username\n", sts);
        return (sts);
      }
    } while (username_l == 0);
    username[username_l] = 0;
  }

  /* Read password from console if not supplied as argv[2] */

  if (argc > 2) {
    strncpyz (password, argv[2], sizeof password);
    password_l = strlen (password);
  } else {
    console_read.size    = sizeof password - 1;
    console_read.buff    = password;
    console_read.rlen    = &password_l;
    console_read.pmtsize = 10;
    console_read.pmtbuff = "Password: ";
    console_read.timeout = 30000;
    console_read.noecho  = 1;

    sts = oz_sys_io (OZ_PROCMODE_KNL, oz_util_h_input, 0, OZ_IO_CONSOLE_READ, sizeof console_read, &console_read);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "oz_util_logon: error %u reading password\n", sts);
      return (sts);
    }
    password[password_l] = 0;

    memset (&console_write, 0, sizeof console_write);
    console_write.trmsize = 1;
    console_write.trmbuff = "\n";
    oz_sys_io (OZ_PROCMODE_KNL, oz_util_h_input, 0, OZ_IO_CONSOLE_WRITE, sizeof console_write, &console_write);
  }
  oz_sys_password_hashit (password, sizeof passhash, passhash);

  /* Read and decode password record */

  sts = oz_sys_password_getbyusername (username, 3, pwitems, &index, secmalloc, NULL);
  if (sts == OZ_ENDOFFILE) goto badpw;
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "oz_util_logon: error %u[%d] scanning password file\n", sts, index);
    return (sts);
  }

  /* Validate the password */

  if (strcmp (pw_passhash, passhash) != 0) goto badpw;

  /* Create USER and JOB blocks */

  creprm.username = username;
  sts = oz_sys_callknl (create_userjob, &creprm);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "oz_util_logon: error %u creating user job\n", sts);
    return (sts);
  }

  /* Spawn the cli */

  spawn_argv[0] = pw_imagenamep;
  spawn_argv[1] = "-interactive";
  sts = oz_sys_spawn (creprm.h_job, pw_imagenamep, oz_util_h_input, oz_util_h_output, 
                      oz_util_h_error, 0, 0, pw_defdirp, 2, spawn_argv, NULL, &h_thread, NULL);
  if (sts != OZ_SUCCESS) oz_sys_io_fs_printf (oz_util_h_error, "oz_util_logon: error %u spawning cli %s\n", sts, pw_imagenamep);

  /* Orphan it so it doesn't die when we exit */

  else oz_sys_thread_orphan (h_thread);

  /* All done */

  return (sts);

  /* Bad username or password - output same error message */
  /* to make it harder for someone to guess usernames     */

badpw:
  oz_sys_io_fs_printf (oz_util_h_error, "oz_util_logon: bad username / password\n");
  return (OZ_ENDOFFILE);
}

/* Alloc memory for security structs */

static void *secmalloc (void *dummy, uLong osize, void *obuff, uLong nsize)

{
  void *nbuff;

  nbuff = NULL;
  if (nsize != 0) {
    nbuff = malloc (nsize);
    memcpy (nbuff, obuff, osize);
  }
  if (obuff != NULL) free (obuff);
  return (nbuff);
}

/* This routine runs in kernel mode to create the username and job blocks                                              */
/* It also sets this thread to look like the user that logged in so the thread gets spawned under that user's security */

static uLong create_userjob (OZ_Procmode cprocmode, void *creprmv)

{
  const char *em, *jobname;
  Creprm *creprm;
  int si;
  uLong sts;
  OZ_Iochan *error_iochan, *input_iochan, *output_iochan;
  OZ_Logname *job_table;
  OZ_Job *job;
  OZ_Secattr *secattr;
  OZ_User *user;

  creprm = creprmv;

  error_iochan  = NULL;
  input_iochan  = NULL;
  output_iochan = NULL;
  job     = NULL;
  secattr = NULL;
  user    = NULL;

  si = oz_hw_cpu_setsoftint (0);

  /* Set up this thread's security to look like person who just logged in */

  sts = oz_knl_user_become (creprm -> username, &em, &user);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "oz_util_logon: error %u %s for %s\n", sts, em, creprm -> username);
    goto done;
  }

  /* Get console device name to make job name */

  sts = oz_knl_handle_takeout (oz_util_h_input, cprocmode, OZ_SECACCMSK_READ, OZ_OBJTYPE_IOCHAN, &input_iochan, NULL); /* get input channel */
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "oz_util_logon: error %u getting input channel\n", sts);
    goto done;
  }

  sts = oz_knl_handle_takeout (oz_util_h_output, cprocmode, OZ_SECACCMSK_WRITE, OZ_OBJTYPE_IOCHAN, &output_iochan, NULL); /* get output channel */
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "oz_util_logon: error %u getting output channel\n", sts);
    goto done;
  }

  sts = oz_knl_handle_takeout (oz_util_h_error, cprocmode, OZ_SECACCMSK_WRITE, OZ_OBJTYPE_IOCHAN, &error_iochan, NULL); /* get error channel */
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "oz_util_logon: error %u getting error channel\n", sts);
    goto done;
  }

  jobname = "";
  if ((error_iochan == input_iochan) && (output_iochan == input_iochan)) {					/* see if all three match */
    jobname = oz_knl_devunit_devname (oz_knl_iochan_getdevunit (input_iochan));
  }

  /* Create job block */

  sts = oz_knl_job_create (user, jobname, &job);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "oz_util_logon: error %u creating job block\n", sts);
    goto done;
  }

  /* Create OZ_CONSOLE logical name */

  secattr = oz_knl_thread_getdefcresecattr (NULL);
  job_table = oz_knl_job_getlognamtbl (job);									/* it goes in the job table */
  if ((error_iochan == input_iochan) && (output_iochan == input_iochan)) {					/* see if all three match */
    sts = oz_knl_logname_creobj (job_table, OZ_PROCMODE_KNL, NULL, secattr, 0, 10, "OZ_CONSOLE", input_iochan, NULL); /* if so, set OZ_CONSOLE to that channel */
  } else {
    sts = oz_knl_logname_crestr (job_table, OZ_PROCMODE_KNL, NULL, secattr, 0, 10, "OZ_CONSOLE", "", NULL);	/* otherwise, use a null string */
  }
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "oz_util_logon: error %u creating OZ_CONSOLE logical\n", sts);
    goto done;
  }

  /* Return handle to job block */

  sts = oz_knl_handle_assign (job, cprocmode, &(creprm -> h_job));
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "oz_util_logon: error %u assigning job handle\n");
    goto done;
  }

  /* Finally, change all I/O channels to user's security attributes so he can access them */

  if (error_iochan  != NULL) oz_knl_iochan_setsecattr (error_iochan,  secattr);
  if (input_iochan  != NULL) oz_knl_iochan_setsecattr (input_iochan,  secattr);
  if (output_iochan != NULL) oz_knl_iochan_setsecattr (output_iochan, secattr);

  /* Done, clean up and return status */

done:
  if (error_iochan  != NULL) oz_knl_handle_putback (oz_util_h_error);
  if (input_iochan  != NULL) oz_knl_handle_putback (oz_util_h_input);
  if (output_iochan != NULL) oz_knl_handle_putback (oz_util_h_output);
  if (job     != NULL) oz_knl_job_increfc     (job,     -1);
  if (secattr != NULL) oz_knl_secattr_increfc (secattr, -1);
  if (user    != NULL) oz_knl_user_increfc    (user,    -1);

  oz_hw_cpu_setsoftint (si);
  return (sts);
}
