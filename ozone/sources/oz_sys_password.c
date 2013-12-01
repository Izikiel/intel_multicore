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
/*  Password file utility routines					*/
/*									*/
/*  Records in the OZ_PASSWORD_FILE are:				*/
/*									*/
/*  |username|password|seckeys..|secattr..|quotas|def/maxprio|image|defdir
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_io_fs.h"
#include "oz_knl_devio.h"
#include "oz_knl_handle.h"
#include "oz_knl_security.h"
#include "oz_knl_status.h"
#include "oz_knl_userjob.h"
#include "oz_sys_handle.h"
#include "oz_sys_hash.h"
#include "oz_sys_io.h"
#include "oz_sys_io_fs.h"
#include "oz_sys_password.h"
#include "oz_sys_syscall.h"

#define RECORD_MAX (1024)

static uLong openpwfile (void *h_pwdfile_r, const char *devname, const char *filname, OZ_Handle h_iochan);
static uLong editpwrec (OZ_Handle h_pwdfile, OZ_IO_fs_readrec *fs_readrec, int oldlen, char *oldstr, int newlen, char *newstr);
static uLong getcurpos (OZ_Handle h_pwdfile, OZ_Dbn *curblock_r, uLong *curbyte_r);

/************************************************************************/
/*									*/
/*  Read record from password file that corresponds to an username	*/
/*									*/
/*    Input:								*/
/*									*/
/*	username = null-terminated username key string			*/
/*	nitems = number of items in items				*/
/*	items = item list of items to return				*/
/*	ment = routine to call to allocate memory			*/
/*	mprm = param to pass to ment routine				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_password_getbyusername = OZ_SUCCESS : successful		*/
/*	                              OZ_ENDOFFILE : username not found	*/
/*	                                      else : other error	*/
/*	*index_r = -1 : error was in opening/reading file		*/
/*	         else : index of item causing the error			*/
/*									*/
/************************************************************************/

uLong oz_sys_password_getbyusername (const char *username, int nitems, OZ_Password_item *items, int *index_r, 
                                     void *(*ment) (void *mprm, uLong osize, void *obuff, uLong nsize), void *mprm)

{
  char *image, *p, pwdrec[RECORD_MAX], sepc;
  int i, usedup, username_l;
  OZ_Handle h_pwdfile;
  OZ_IO_fs_open fs_open;
  OZ_IO_fs_readrec fs_readrec;
  uByte hashbin[OZ_SYS_HASH_BINSIZE];
  uLong pwdrec_l, sts;

  char *pwdrec_un;
  char *pwdrec_pw;
  char *pwdrec_sk;
  char *pwdrec_sa;
  char *pwdrec_qu;
  char *pwdrec_im;
  char *pwdrec_dd;
  uLong pwdrec_defbasepri;
  uLong pwdrec_maxbasepri;

  if (index_r != NULL) *index_r = -1;

  /* Open the password file - given by logical OZ_PASSWORD_FILE */

  memset (&fs_open, 0, sizeof fs_open);
  fs_open.name = "OZ_PASSWORD_FILE";
  fs_open.lockmode = OZ_LOCKMODE_PR;
  sts = oz_sys_io_fs_open (sizeof fs_open, &fs_open, 0, &h_pwdfile);
  if (sts != OZ_SUCCESS) return (sts);

  /* Scan password file for the username */

  memset (&fs_readrec, 0, sizeof fs_readrec);
  fs_readrec.size = sizeof pwdrec - 1;
  fs_readrec.buff = pwdrec;
  fs_readrec.trmsize = 1;
  fs_readrec.trmbuff = "\n";
  fs_readrec.rlen = &pwdrec_l;

  username_l = strlen (username);

  while (1) {
    sts = oz_sys_io (OZ_PROCMODE_KNL, h_pwdfile, 0, OZ_IO_FS_READREC, sizeof fs_readrec, &fs_readrec);
    if (sts != OZ_SUCCESS) break;
    if (pwdrec_l == 0) continue;
    pwdrec[pwdrec_l] = 0;
    sepc = pwdrec[0];
    if (pwdrec[username_l+1] != sepc) continue;
    if (memcmp (pwdrec + 1, username, username_l) == 0) break;
  }

  /* Close password file */

  oz_sys_handle_release (OZ_PROCMODE_KNL, h_pwdfile);

  /* Check completion status.  OZ_ENDOFFILE means the username was not found. */

  if (sts != OZ_SUCCESS) return (sts);

  /* Divide up the record into fields */

  pwdrec_un = pwdrec + 1;		/* username directly follows separator char */
  p = strchr (pwdrec + 1, sepc);
  if (p == NULL) return (OZ_BADPWRECORD);
  *(p ++) = 0;
  pwdrec_pw = p;			/* password */
  p = strchr (p, sepc);
  if (p == NULL) return (OZ_BADPWRECORD);
  *(p ++) = 0;
  pwdrec_sk = p;			/* seckeys */
  p = strchr (p, sepc);
  if (p == NULL) return (OZ_BADPWRECORD);
  *(p ++) = 0;
  pwdrec_sa = p;			/* defcresecattr */
  p = strchr (p, sepc);
  if (p == NULL) return (OZ_BADPWRECORD);
  *(p ++) = 0;
  pwdrec_qu = p;			/* quotas */
  p = strchr (p, sepc);
  if (p == NULL) return (OZ_BADPWRECORD);
  *(p ++) = 0;
  pwdrec_defbasepri = oz_hw_atoi (p, &usedup); /* defbasepri[/maxbasepri] */
  if ((pwdrec_defbasepri == 0) || (p[usedup] != '/' && p[usedup] != sepc)) return (OZ_BADPWRECORD);
  if (p[usedup] == sepc) pwdrec_maxbasepri = pwdrec_defbasepri;
  else {
    p += usedup + 1;
    pwdrec_maxbasepri = oz_hw_atoi (p, &usedup);
    if ((pwdrec_maxbasepri == 0) || (p[usedup] != sepc)) return (OZ_BADPWRECORD);
  }
  p += usedup + 1;
  pwdrec_im = p;			/* imagename */
  p = strchr (p, sepc);
  if (p == NULL) return (OZ_BADPWRECORD);
  *(p ++) = 0;
  pwdrec_dd = p;			/* default directory is last */

  /* Return requested items */

  for (i = 0; i < nitems; i ++) {
    if (index_r != NULL) *index_r = i;
    switch (items[i].code) {
      case OZ_PASSWORD_CODE_PASSHASH: {
        if (items[i].size < OZ_SYS_HASH_STRSIZE) return (OZ_BADITEMSIZE);
        if ((strlen (pwdrec_pw) == OZ_SYS_HASH_STRSIZE - 1) && oz_sys_hash_str2bin (pwdrec_pw, hashbin)) strcpy (items[i].buff, pwdrec_pw);
        else oz_sys_password_hashit (pwdrec_pw, items[i].size, items[i].buff);
        if (items[i].rlen != NULL) *(items[i].rlen) = OZ_SYS_HASH_STRSIZE - 1;
        break;
      }
      case OZ_PASSWORD_CODE_SECKEYSP: {
        sts = oz_sys_seckeys_str2bin (strlen (pwdrec_sk), pwdrec_sk, ment, mprm, items[i].rlen, items[i].buff);
        if (sts != OZ_SUCCESS) return (sts);
        break;
      }
      case OZ_PASSWORD_CODE_DEFCRESECATTRP: {
        sts = oz_sys_secattr_str2bin (strlen (pwdrec_sa), pwdrec_sa, NULL, ment, mprm, items[i].rlen, items[i].buff);
        if (sts != OZ_SUCCESS) return (sts);
        break;
      }
      case OZ_PASSWORD_CODE_QUOTASTRP: {
        *((char **)(items[i].buff)) = (*ment) (mprm, 0, NULL, strlen (pwdrec_qu) + 1);
        strcpy (*((char **)(items[i].buff)), pwdrec_qu);
        break;
      }
      case OZ_PASSWORD_CODE_IMAGENAMEP: {
        *((char **)(items[i].buff)) = (*ment) (mprm, 0, NULL, strlen (pwdrec_im) + 1);
        strcpy (*((char **)(items[i].buff)), pwdrec_im);
        break;
      }
      case OZ_PASSWORD_CODE_DEFDIRP: {
        *((char **)(items[i].buff)) = (*ment) (mprm, 0, NULL, strlen (pwdrec_dd) + 1);
        strcpy (*((char **)(items[i].buff)), pwdrec_dd);
        break;
      }
      case OZ_PASSWORD_CODE_DEFBASEPRI: {
        if (items[i].size != sizeof (uLong)) return (OZ_BADITEMSIZE);
        *(uLong *)(items[i].buff) = pwdrec_defbasepri;
        break;
      }
      case OZ_PASSWORD_CODE_MAXBASEPRI: {
        if (items[i].size != sizeof (uLong)) return (OZ_BADITEMSIZE);
        *(uLong *)(items[i].buff) = pwdrec_maxbasepri;
        break;
      }
      default: return (OZ_BADITEMCODE);
    }
  }

  if (index_r != NULL) *index_r = i;
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Convert plaintext password string to hashed string			*/
/*									*/
/************************************************************************/

void oz_sys_password_hashit (const char *password, int hashsize, char *hashbuff)

{
  uByte hashbin[OZ_SYS_HASH_BINSIZE];

  if (hashsize < OZ_SYS_HASH_STRSIZE) oz_crash ("oz_sys_password_hashit: hashsize %d must be at least %d", hashsize, OZ_SYS_HASH_STRSIZE);
  oz_sys_hash (strlen (password), password, hashbin);
  oz_sys_hash_bin2str (hashbin, hashbuff);
}

/************************************************************************/
/*									*/
/*  Change current user's password.					*/
/*  This routine can be called by non-privileged users.			*/
/*									*/
/*    Input:								*/
/*									*/
/*	oldpw = the current user's old password string			*/
/*	newpw = what the current user wants the pw changed to		*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_password_change = OZ_SUCCESS : successful		*/
/*	                     OZ_BADPASSWORD : old password incorrect	*/
/*	                               else : i/o error			*/
/*									*/
/************************************************************************/

OZ_HW_SYSCALL_DEF_2 (password_change, const char *, oldpw, const char *, newpw)

{
  char newpwhashstr[OZ_SYS_HASH_STRSIZE], *oldstr, *p, pwdrec[RECORD_MAX], sepc;
  const char *username;
  int oldlen, si, username_l;
  OZ_Handle h_pwdfile;
  OZ_IO_fs_getinfo1 fs_getinfo1;
  OZ_IO_fs_readrec fs_readrec;
  uByte hashbin1[OZ_SYS_HASH_BINSIZE], hashbin2[OZ_SYS_HASH_BINSIZE];
  uLong pwdrec_l, sts;

  h_pwdfile = 0;
  si = oz_hw_cpu_setsoftint (0);

  /* Open the password file - given by kernel mode logical OZ_PASSWORD_FILE */

  sts = oz_sys_io_fs_parse3 ("OZ_PASSWORD_FILE", 0, NULL, OZ_PROCMODE_KNL, openpwfile, &h_pwdfile);
  if (sts != OZ_SUCCESS) goto rtnsts;

  /* Scan password file for the username */

  memset (&fs_getinfo1, 0, sizeof fs_getinfo1);
  memset (&fs_readrec, 0, sizeof fs_readrec);
  fs_readrec.size    = sizeof pwdrec - 1;
  fs_readrec.buff    = pwdrec;
  fs_readrec.rlen    = &pwdrec_l;
  fs_readrec.trmsize = 1;
  fs_readrec.trmbuff = "\n";

  username   = oz_knl_user_getname (NULL);
  username_l = strlen (username);

  while (1) {
    sts = getcurpos (h_pwdfile, &fs_readrec.atblock, &fs_readrec.atbyte);
    if (sts != OZ_SUCCESS) goto rtnsts;
    sts = oz_sys_io (OZ_PROCMODE_KNL, h_pwdfile, 0, OZ_IO_FS_READREC, sizeof fs_readrec, &fs_readrec);
    if (sts != OZ_SUCCESS) goto rtnsts;
    if (pwdrec_l == 0) continue;
    pwdrec[pwdrec_l] = 0;
    sepc = pwdrec[0];
    if (pwdrec[username_l+1] != sepc) continue;
    if (memcmp (pwdrec + 1, username, username_l) == 0) break;
  }

  /* Get what's in password record to hashbin2 */

  sts = OZ_BADPWRECORD;
  oldstr = pwdrec + username_l + 2;
  p = memchr (oldstr, sepc, pwdrec_l - username_l - 2);
  if (p == NULL) goto rtnsts;
  oldlen = p - oldstr;
  if ((oldlen != OZ_SYS_HASH_STRSIZE - 1) || !oz_sys_hash_str2bin (oldstr, hashbin2)) oz_sys_hash (oldlen, oldstr, hashbin2);

  /* Get what caller gave us for old password to hashbin1 */

  oz_sys_hash (strlen (oldpw), oldpw, hashbin1);

  /* See if they match */

  sts = OZ_BADPASSWORD;
  if (memcmp (hashbin1, hashbin2, OZ_SYS_HASH_BINSIZE) != 0) goto rtnsts;

  /* Make new password hash string */

  oz_sys_hash (strlen (newpw), newpw, hashbin1);
  oz_sys_hash_bin2str (hashbin1, newpwhashstr);

  /* Edit password record replacing the old password with new one */

  sts = editpwrec (h_pwdfile, &fs_readrec, oldlen, oldstr, OZ_SYS_HASH_STRSIZE - 1, newpwhashstr);

  /* Close password file and return final status */

rtnsts:
  oz_sys_handle_release (OZ_PROCMODE_KNL, h_pwdfile);
  oz_hw_cpu_setsoftint (si);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Open password file, but bypass caller's privs			*/
/*									*/
/*    Input:								*/
/*									*/
/*	devname  = device the password file is on			*/
/*	filename = filename of the password file			*/
/*	smplevel = softints inhibited					*/
/*									*/
/*    Output:								*/
/*									*/
/*	openpwfile = OZ_SUCCESS : successful				*/
/*	                   else : i/o error status			*/
/*	*h_pwdfile_r = handle to I/O channel				*/
/*									*/
/************************************************************************/

static uLong openpwfile (void *h_pwdfile_r, const char *devname, const char *filname, OZ_Handle h_iochan)

{
  OZ_Handle h_pwdfile;
  OZ_IO_fs_open fs_open;
  OZ_Iochan *iochan;
  uLong sts;

  /* If I/O channel handle, return that handle */

  if (h_iochan != 0) {
    *((OZ_Handle *)h_pwdfile_r) = h_iochan;
    return (OZ_SUCCESS);
  }

  /* Assign I/O channel to disk the password file is on       */
  /* Pass secattrs NULL so only kernel routines can access it */

  sts = oz_knl_iochan_crbynm (devname, OZ_LOCKMODE_CW, OZ_PROCMODE_KNL, NULL, &iochan);
  if (sts == OZ_SUCCESS) {

    /* Open the password file, don't let anyone else read it while we're in there       */
    /* The oz_knl_io routine uses the sysio flag = 1 which means it can access anything */

    memset (&fs_open, 0, sizeof fs_open);
    fs_open.name = filname;
    fs_open.lockmode = OZ_LOCKMODE_EX;
    sts = oz_knl_io (iochan, OZ_IO_FS_OPEN, sizeof fs_open, &fs_open);

    /* If successful, assign an handle to the I/O channel.  The oz_knl_handle_assign routine will find */
    /* that the caller can't access anything on the channel (because the channel has secattr NULL), so */
    /* we have to manually set the handle's secaccmsk to indicate all accesses.  The handle, however,  */
    /* is owned by kernel mode so another user mode thread in the process can't access it.             */

    if (sts == OZ_SUCCESS) sts = oz_knl_handle_assign (iochan, OZ_PROCMODE_KNL, h_pwdfile_r);
    if (sts == OZ_SUCCESS) oz_knl_handle_setsecaccmsk (*((OZ_Handle *)h_pwdfile_r), OZ_PROCMODE_KNL, -1);

    /* Anyway, all done with the I/O channel pointer */

    oz_knl_iochan_increfc (iochan, -1);
  }
  return (sts);
}

/************************************************************************/
/*									*/
/*  Edit password record						*/
/*									*/
/*    Input:								*/
/*									*/
/*	h_pwdfile  = password file, positioned just past record being edited
/*	fs_readrec = read param block used to read record being edited	*/
/*	           -> buff = points to buffer containing old contents	*/
/*	           -> rlen = points to length of old contents		*/
/*	           -> atblock,atbyte = where the old rec was read from	*/
/*	oldlen     = length of old string in the fs_readrec buffer	*/
/*	oldstr     = address of old string in the fs_readrec buffer	*/
/*	newlen     = length of new string to put in fs_readrec buffer	*/
/*	newstr     = address of new string to put in fs_readrec buffer	*/
/*									*/
/*    Output:								*/
/*									*/
/*	editpwrec = OZ_SUCCESS : successful				*/
/*	                  else : error status				*/
/*	*(fs_readrec -> buff) = updated with new contents		*/
/*									*/
/************************************************************************/

static uLong editpwrec (OZ_Handle h_pwdfile, OZ_IO_fs_readrec *fs_readrec, int oldlen, char *oldstr, int newlen, char *newstr)

{
  char *oldbuf, recbuf2[RECORD_MAX];
  OZ_IO_fs_readrec fs_readrec2;
  OZ_IO_fs_writerec fs_writerec;
  uLong oldsiz, sts;

  memset (&fs_writerec, 0, sizeof fs_writerec);

  /* If lengths are the same, we can do an in-place update */
  /* This will be a common case for changing passwords, as the hash string is always the same length */

  oldsiz = *(fs_readrec -> rlen);
  oldbuf = fs_readrec -> buff;

#if 00
  oz_knl_printk ("oz_sys_password*: old at %u.%u\n", fs_readrec -> atblock, fs_readrec -> atbyte); oz_knl_dumpmem (oldsiz, oldbuf);
  oz_knl_printk ("oz_sys_password*: oldstr\n"); oz_knl_dumpmem (oldlen, oldstr);
  oz_knl_printk ("oz_sys_password*: newstr\n"); oz_knl_dumpmem (newlen, newstr);
#endif

  if (newlen == oldlen) {
    memcpy (oldstr, newstr, oldlen);
    fs_writerec.size    = oldsiz;
    fs_writerec.buff    = oldbuf;
    fs_writerec.trmsize = fs_readrec -> trmsize;
    fs_writerec.trmbuff = fs_readrec -> trmbuff;
    fs_writerec.atblock = fs_readrec -> atblock;
    fs_writerec.atbyte  = fs_readrec -> atbyte;
    sts = oz_sys_io (OZ_PROCMODE_KNL, h_pwdfile, 0, OZ_IO_FS_WRITEREC, sizeof fs_writerec, &fs_writerec);
    return (sts);
  }

  /* Different lengths, make sure new length not too long */

  if (oldsiz - oldlen + newlen > RECORD_MAX) return (OZ_BUFFEROVF);

  /* Copy rest of records over the old one */

  memset (&fs_readrec2, 0, sizeof fs_readrec2);
  fs_readrec2.size    = sizeof recbuf2;
  fs_readrec2.buff    = recbuf2;
  fs_readrec2.rlen    = &fs_writerec.size;
  fs_readrec2.trmsize = fs_readrec -> trmsize;
  fs_readrec2.trmbuff = fs_readrec -> trmbuff;

  fs_writerec.buff    = recbuf2;
  fs_writerec.trmsize = fs_readrec -> trmsize;
  fs_writerec.trmbuff = fs_readrec -> trmbuff;
  fs_writerec.atblock = fs_readrec -> atblock;
  fs_writerec.atbyte  = fs_readrec -> atbyte;

  while (1) {
    sts = oz_sys_io (OZ_PROCMODE_KNL, h_pwdfile, 0, OZ_IO_FS_READREC, sizeof fs_readrec2, &fs_readrec2);
    if (sts == OZ_ENDOFFILE) break;
    if (sts != OZ_SUCCESS) return (sts);
    sts = getcurpos (h_pwdfile, &fs_readrec2.atblock, &fs_readrec2.atbyte);
    if (sts != OZ_SUCCESS) return (sts);
#if 00
    oz_knl_printk ("oz_sys_password*: cpy at %u.%u\n", fs_writerec.atblock, fs_writerec.atbyte); oz_knl_dumpmem (fs_writerec.size, fs_writerec.buff);
#endif
    sts = oz_sys_io (OZ_PROCMODE_KNL, h_pwdfile, 0, OZ_IO_FS_WRITEREC, sizeof fs_writerec, &fs_writerec);
    if (sts != OZ_SUCCESS) return (sts);
    sts = getcurpos (h_pwdfile, &fs_writerec.atblock, &fs_writerec.atbyte);
    if (sts != OZ_SUCCESS) return (sts);
  }

  /* Write the modified record to the end of the file */

  memmove (oldstr + newlen, oldstr + oldlen, oldbuf + oldsiz - oldstr - oldlen);
  memcpy (oldstr, newstr, newlen);
  fs_writerec.size     = oldsiz - oldlen + newlen;
  fs_writerec.buff     = oldbuf;
  fs_writerec.truncate = 1;
#if 00
  oz_knl_printk ("oz_sys_password*: end at %u.%u\n", fs_writerec.atblock, fs_writerec.atbyte); oz_knl_dumpmem (fs_writerec.size, fs_writerec.buff);
#endif
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_pwdfile, 0, OZ_IO_FS_WRITEREC, sizeof fs_writerec, &fs_writerec);
  if (sts != OZ_SUCCESS) return (sts);

  /* Close file */

  sts = oz_sys_io (OZ_PROCMODE_KNL, h_pwdfile, 0, OZ_IO_FS_CLOSE, 0, NULL);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Get the current position we are at in the file			*/
/*									*/
/*    Input:								*/
/*									*/
/*	h_pwdfile = handle to password file				*/
/*									*/
/*    Output:								*/
/*									*/
/*	getcurpos = OZ_SUCCESS : successful				*/
/*	                  else : error status				*/
/*	*curblock_r = current block in the file				*/
/*	*curbyte_r  = current byte within that block			*/
/*									*/
/************************************************************************/

static uLong getcurpos (OZ_Handle h_pwdfile, OZ_Dbn *curblock_r, uLong *curbyte_r)

{
  OZ_IO_fs_getinfo1 fs_getinfo1;
  uLong sts;

  memset (&fs_getinfo1, 0, sizeof fs_getinfo1);
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_pwdfile, 0, OZ_IO_FS_GETINFO1, sizeof fs_getinfo1, &fs_getinfo1);
  if ((sts == OZ_SUCCESS) && (fs_getinfo1.curblock == 0)) sts = OZ_BADIOFUNC;
  if (sts == OZ_SUCCESS) {
    *curblock_r = fs_getinfo1.curblock;
    *curbyte_r  = fs_getinfo1.curbyte;
  }
  return (sts);
}
