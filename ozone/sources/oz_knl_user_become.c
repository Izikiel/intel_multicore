//+++2001-10-06
//    Copyright (C) 2001, Mike Rieker, Beverly, MA USA
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
//---2001-10-06

/************************************************************************/
/*									*/
/*  Become a user							*/
/*									*/
/*    Input:								*/
/*									*/
/*	username = null terminated username string			*/
/*	smplevel = softint						*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_user_become = OZ_SUCCESS : successful			*/
/*	                                  *user_r = user block		*/
/*	                           else : error status			*/
/*	                                  *em_r = error message		*/
/*									*/
/*    Note:								*/
/*									*/
/*	This routine does not move the current thread over to the 	*/
/*	given user's user block, but it does set up the given user's 	*/
/*	security environment for the current thread, and makes it 	*/
/*	look like that user is logged on (by creating its user block).	*/
/*	When the caller is done playing that user, it should free off 	*/
/*	the 'user' block pointer, thus making it look like that user 	*/
/*	logged off.							*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_quota.h"
#include "oz_knl_security.h"
#include "oz_knl_status.h"
#include "oz_knl_thread.h"
#include "oz_knl_user_become.h"
#include "oz_knl_userjob.h"
#include "oz_sys_password.h"

static void *secmalloc (void *dummy, uLong osize, void *obuff, uLong nsize);

uLong oz_knl_user_become (const char *username, const char **em_r, OZ_User **user_r)

{
  char *quotastrbuff;
  int index;
  OZ_Quota *quota;
  OZ_Secattr *secattr;
  OZ_Seckeys *seckeys;
  OZ_Thread *thread;
  OZ_User *user;
  uByte *secattrbuff, *seckeysbuff;
  uLong defbasepri, maxbasepri, secattrsize, seckeyssize, sts;

  OZ_Password_item pwitems[5] = { OZ_PASSWORD_CODE_SECKEYSP,       sizeof seckeysbuff,  &seckeysbuff,  &seckeyssize,
                                  OZ_PASSWORD_CODE_DEFCRESECATTRP, sizeof secattrbuff,  &secattrbuff,  &secattrsize,
                                  OZ_PASSWORD_CODE_QUOTASTRP,      sizeof quotastrbuff, &quotastrbuff, NULL, 
                                  OZ_PASSWORD_CODE_DEFBASEPRI,     sizeof defbasepri,   &defbasepri,   NULL, 
                                  OZ_PASSWORD_CODE_MAXBASEPRI,     sizeof maxbasepri,   &maxbasepri,   NULL };

  quota   = NULL;
  seckeys = NULL;
  secattr = NULL;
  thread  = oz_knl_thread_getcur ();
  *user_r = NULL;

  /* Read user's security environment from the password file */

  index = -1;
  sts   = oz_sys_password_getbyusername (username, 5, pwitems, &index, secmalloc, NULL);
  *em_r = "finding password record";
  if (sts != OZ_SUCCESS) goto rtnsts;

  /* Set my security keys to what the user is allowed to access */

  sts   = oz_knl_seckeys_create (seckeyssize, seckeysbuff, NULL, &seckeys);
  *em_r = "setting security keys";
  if (sts != OZ_SUCCESS) goto rtnsts;

  /* Set my default create security attributes to what the user is set to */

  sts = oz_knl_secattr_create (secattrsize, secattrbuff, NULL, &secattr);
  *em_r = "setting security attributes";
  if (sts != OZ_SUCCESS) goto rtnsts;

  /* Create quota block for the user */

  sts = oz_knl_quota_create (strlen (quotastrbuff), quotastrbuff, &quota);
  *em_r = "creating quota block";
  if (sts != OZ_SUCCESS) goto rtnsts;

  /* Create user block for the user */

  oz_knl_thread_setdefcresecattr (thread, secattr);		/* set security attributes to create it with */
								/* (and for everything we create from now on) */
  sts = oz_knl_user_create (username, quota, seckeys, maxbasepri, &user); /* create the block (in case not already logged in) */
  if (quota != NULL) {						/* release this quota block in case user was already logged in */
    oz_knl_quota_increfc (quota, -1);
    quota = NULL;
  }
  *em_r = "creating user block";				/* set up error message in case of error */
  if (sts != OZ_SUCCESS) goto rtnsts;				/* return error status if error */

  /* Finish up by setting this thread's keys, quota and priority = the user's keys, quota and priority */

  oz_knl_thread_setseckeys (thread, seckeys);			/* set this thread's security keys to the ones from password file */
  quota = oz_knl_user_getquota (user);				/* get user's quota block pointer (and inc its ref count) */
								/* (will be different from above block if user was already logged in) */
  quota = oz_knl_quota_setcpudef (quota);			/* set this thread's quota block to user's quota block and get rid of my old one */
  oz_knl_thread_setbasepri (thread, defbasepri);		/* set this thread's priority to the user's default priority */
  oz_knl_thread_setcurprio (thread, defbasepri);

  *em_r   = NULL;						/* no error message */
  *user_r = user;						/* return pointer to user block */
  sts = OZ_SUCCESS;						/* successful */

  /* Return with status after cleaning up */

rtnsts:
  if (seckeys != NULL) oz_knl_seckeys_increfc (seckeys, -1);
  if (secattr != NULL) oz_knl_secattr_increfc (secattr, -1);
  if (quota   != NULL) oz_knl_quota_increfc   (quota,   -1);
  if ((index >= 1) && (seckeysbuff  != NULL)) OZ_KNL_NPPFREE (seckeysbuff);
  if ((index >= 2) && (secattrbuff  != NULL)) OZ_KNL_NPPFREE (secattrbuff);
  if ((index >= 3) && (quotastrbuff != NULL)) OZ_KNL_NPPFREE (quotastrbuff);
  return (sts);
}

/* Allocate memory for password file stuff */

static void *secmalloc (void *dummy, uLong osize, void *obuff, uLong nsize)

{
  void *nbuff;

  nbuff = NULL;
  if (nsize != 0) {
    nbuff = OZ_KNL_NPPMALLOC (nsize);
    memcpy (nbuff, obuff, osize);
  }
  if (obuff != NULL) OZ_KNL_NPPFREE (obuff);
  return (nbuff);
}
