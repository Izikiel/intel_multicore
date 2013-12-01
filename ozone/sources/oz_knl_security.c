//+++2001-11-18
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
//---2001-11-18

/************************************************************************/
/*									*/
/*  These routines process the security objects, secattr and seckeys	*/
/*									*/
/************************************************************************/

#define _OZ_KNL_SECURITY_C

#include "ozone.h"
#include "oz_knl_hw.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_objtype.h"
#include "oz_knl_printk.h"
#include "oz_knl_quota.h"
#include "oz_knl_sdata.h"
#include "oz_knl_security.h"
#include "oz_knl_status.h"

#define SECATTRVER 1
#define SECKEYSVER 1

const OZ_Secacclas oz_sys_secacclas_default[] = { "look", OZ_SECACCMSK_LOOK, 
                                                  "read", OZ_SECACCMSK_READ, 
                                                 "write", OZ_SECACCMSK_WRITE, 
                                                    NULL, 0 };

static const OZ_Secacclas actions[] = { "deny", OZ_SECACTION_DENY, 
                                       "alarm", OZ_SECACTION_ALARM, 
                                          NULL, 0 };

/* Security attributes - these are assigned to an object by the object's creator to indicate who can access the object */

typedef struct { uLong secattrver;				/* version number of the struct */
                 uLong nentries;				/* number of entries in e[] array */
                 struct { OZ_Secident secident, secidmsk;	/* identifier and identifier mask */
                          OZ_Secaccmsk secaccmsk;		/* access mask - ie, what access types are granted for the identifier */
                          OZ_Secaction secaction;		/* action flags - ie, what special action to take if this entry is used */
                        } e[1];
               } Secattrbuf;

struct OZ_Secattr { OZ_Objtype objtype;				/* object type OZ_OBJTYPE_SECATTR */
                    Long refcount;				/* number of references to this struct in memory */
                    Secattrbuf s;
                  };

/* Security keys - these are typically read from the password file at logon time and are assigned to all threads created from that */

typedef struct { uLong seckeysver;				/* version number of struct */
                 uLong nentries;				/* number of entries in e[] array */
                 struct { OZ_Secident secident;			/* security identifier */
                        } e[1];
               } Seckeysbuf;

struct OZ_Seckeys { OZ_Objtype objtype;				/* object type OZ_OBJTYPE_SECKEYS */
                    Long refcount;				/* number of references to this struct in memory */
                    Seckeysbuf s;
                  };

#define WHYINCREFC 0 // 1024
#define WHYINCREFC_FROM 4

#if WHYINCREFC
static struct { uLong repeat;
                OZ_Secattr *secattr;
                void *from[WHYINCREFC_FROM];
              } whyincrefc_array[WHYINCREFC];
static int whyincrefc_index = 0;
#endif

static void *secmallocnpp (void *dummy, uLong osize, void *obuff, uLong nsize);
static void concat (void *(*ment) (void *mprm, uLong osize, void *obuff, uLong nsize), void *mprm, char **strp, uLong *sizp, const char *add);

/************************************************************************/
/*									*/
/*  Extract and decode an security attributes string from an object's 	*/
/*  name string								*/
/*									*/
/*    Input:								*/
/*									*/
/*	namesiz = max size of object name (incl terminating null)	*/
/*	namestr = name string buffer, including possible '(secattr)'	*/
/*	*secattr_r = default secattrs (ref count incremented)		*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_secattr_fromname = OZ_SUCCESS : successful		*/
/*	                                else : error status		*/
/*	*namelen_r = resultant name string length (always < namesiz)	*/
/*	*secattr_r = resultant secattrs (ref count incremented)		*/
/*									*/
/************************************************************************/

uLong oz_knl_secattr_fromname (int namesiz, const char *namestr, int *namelen_r, const OZ_Secacclas *secacclas, OZ_Secattr **secattr_r)

{
  int attrlen, namelen;
  uLong secattrsize, sts;
  void *secattrbuff;

  sts = OZ_SUCCESS;								/* success if no ( or ) */
  namelen = attrlen = strlen (namestr);						/* point to end of given name string */
  if ((attrlen > 0) && (namestr[--attrlen] == ')')) {
    oz_knl_secattr_increfc (*secattr_r, -1);					/* ends in ), release default value */
    *secattr_r = NULL;								/* clear it so caller knows */
    while ((-- namelen) > 0) if (namestr[namelen-1] == '(') break;		/* now look for initial ( */
    if (namelen == 0) return (OZ_BADSECATTRSTR);				/* if none, secattr string is bad */
    sts = oz_sys_secattr_str2bin (attrlen - namelen, namestr + namelen, secacclas, secmallocnpp, NULL, &secattrsize, &secattrbuff);
    if (sts == OZ_SUCCESS) {
      sts = oz_knl_secattr_create (secattrsize, secattrbuff, secacclas, secattr_r);
      OZ_KNL_NPPFREE (secattrbuff);
    }
    -- namelen;									/* don't include '(' in output */
  }
  if (sts == OZ_SUCCESS) {							/* make sure name string isn't too long */
    *namelen_r = namelen;
    if (namelen >= namesiz) sts = OZ_NAMETOOLONG;
  }
  if (sts != OZ_SUCCESS) {							/* if error, release secattrs */
    oz_knl_secattr_increfc (*secattr_r, -1);
    *secattr_r = NULL;
  }
  return (sts);
}

static void *secmallocnpp (void *dummy, uLong osize, void *obuff, uLong nsize)

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

/************************************************************************/
/*									*/
/*  Convert a security attributes string to binary			*/
/*									*/
/*    Input:								*/
/*									*/
/*	str = string to be parsed					*/
/*	secacclas = security access class string table			*/
/*	ment = memory allocator entrypoint				*/
/*	mprm = param to pass to ment routine				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_secattr_str2bin = OZ_SUCCESS : successful		*/
/*	                               else : error			*/
/*	*size_r = size of resultant buffer				*/
/*	          (error char offset in str if error)			*/
/*	*buff_r = resultant buffer					*/
/*	          (NULL if error)					*/
/*									*/
/*    String format:							*/
/*									*/
/*	ident[&mask]=accessbit+accessbit+actionbit+actionbit+...,...	*/
/*									*/
/************************************************************************/

uLong oz_sys_secattr_str2bin (int str_l, 
                              const char *str, 
                              const OZ_Secacclas *secacclas, 
                              void *(*ment) (void *mprm, uLong osize, void *obuff, uLong nsize), 
                              void *mprm, 
                              uLong *size_r, 
                              void **buff_r)

{
  const char *p;
  const OZ_Secacclas *s;
  int usedup;
  OZ_Secaccmsk accbits;
  OZ_Secaction actbits;
  OZ_Secident ident, mask;
  Secattrbuf *buff;
  uLong indx, reqd, size;

  if (secacclas == NULL) secacclas = oz_sys_secacclas_default;

  size 	= 0;						/* haven't allocated any buffer yet */
  buff 	= NULL;
  indx 	= 0;						/* haven't parsed anything yet */
  p    	= str;						/* point to the string */
  str_l = strnlen (p, str_l);				/* get string length */
  if (str_l == 0) goto bad;				/* must have something there */
  reqd  = 0;						/* nothing required for '*' (kernel access only) */
  if ((str_l == 1) && (*p == '*')) goto done;
  -- p;
  do {
    ident = oz_hw_atoz (++ p, &usedup);			/* convert the ident number string to binary */
    if (usedup == 0) goto bad;				/* it must have at least one digit */
    p += usedup;					/* skip over ident number */
    mask = (uLong)(-1);					/* assume a mask of all ones */
    if (*p == '&') {					/* see if mask is present */
      mask = oz_hw_atoz (++ p, &usedup);		/* if so, convert it to binary */
      if (usedup == 0) goto bad;			/* it must have at least one digit */
      p += usedup;					/* skip over mask digits */
    }
    if (*p != '=') goto bad;				/* anyway, we should have an = here */
    accbits = 0;					/* good, no access bits so far */
    actbits = 0;					/* and no action bits so far */
    do {
      p ++;						/* skip over the = or the + */
      for (s = secacclas; s -> name != NULL; s ++) {	/* try to decode access name (READ, WRITE, etc) */
        if (strncasecmp (p, s -> name, strlen (s -> name)) == 0) {
          accbits |= s -> valu;
          break;
        }
      }
      if (s -> name == NULL) {
        for (s = actions; s -> name != NULL; s ++) {	/* try to decode action name (DENY, ALARM) */
          if (strncasecmp (p, s -> name, strlen (s -> name)) == 0) {
            actbits |= s -> valu;
            break;
          }
        }
      }
      if (s -> name == NULL) goto bad;			/* neither, error */
      p += strlen (s -> name);				/* skip over the access or action name */
    } while (*p == '+');				/* repeat if more access/action names follow */
    reqd = (OZ_Pointer)(buff -> e + indx + 1) - (OZ_Pointer)buff; /* see how much storage required for this one */
    while (reqd > size) {				/* make sure we have enough */
      buff  = (*ment) (mprm, size, buff, size + 32);
      size += 32;
    }
    buff -> e[indx].secident  = ident;			/* store it */
    buff -> e[indx].secidmsk  = mask;
    buff -> e[indx].secaccmsk = accbits;
    buff -> e[indx].secaction = actbits;
    indx ++;
  } while ((p < str + str_l) && (*p == ','));		/* repeat if more identifiers follow */
  if (p < str + str_l) goto bad;			/* error if not end-of-string */
  buff -> secattrver = SECATTRVER;			/* ok, set the version number */
  buff -> nentries   = indx;				/* save final number of entries */
done:
  *size_r = reqd;					/* return required size */
  *buff_r = buff;					/* return buffer pointer */
  return (OZ_SUCCESS);					/* return success status */

bad:
  (*ment) (mprm, size, buff, 0);			/* some error, free off buffer */
  *size_r = p - str;					/* return offset to error character */
  *buff_r = NULL;					/* return null buffer pointer */
  return (OZ_BADSECATTRSTR);				/* return error status */
}

/************************************************************************/
/*									*/
/*  Convert a security attributes buffer to a string			*/
/*									*/
/*    Input:								*/
/*									*/
/*	size  = size of binary buffer					*/
/*	buffv = binary buffer						*/
/*	secacclas = security access class string table			*/
/*	ment  = memory allocator entrypoint				*/
/*	mprm  = param to pass to ment routine				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_secattr_bin2str = OZ_SUCCESS : successful		*/
/*	                               else : error			*/
/*	*buff_r = resultant string buffer				*/
/*									*/
/*    String format:							*/
/*									*/
/*	ident[&mask]=accessbit+accessbit+actionbit+actionbit+...,...	*/
/*									*/
/************************************************************************/

uLong oz_sys_secattr_bin2str (uLong size, 
                              const void *buffv, 
                              const OZ_Secacclas *secacclas, 
                              void *(*ment) (void *mprm, uLong osize, void *obuff, uLong nsize), 
                              void *mprm, 
                              char **buff_r)

{
  char hexbuf[12], *str;
  const char *sep;
  const OZ_Secacclas *s;
  const Secattrbuf *buff;
  OZ_Secaccmsk accbits;
  OZ_Secaction actbits;
  uLong indx, reqd, siz;

  /* If null buffer supplied, return '*' */

  siz = 0;
  str = NULL;
  if (size == 0) {
    concat (ment, mprm, &str, &siz, "*");
    goto done;
  }

  /* Set up default access class */

  if (secacclas == NULL) secacclas = oz_sys_secacclas_default;

  /* Check buffer size */

  buff = buffv;
  if (size < sizeof *buff) return (OZ_BADBUFFERSIZE);	/* should have at least the header and one entry */
  if (buff -> secattrver != SECATTRVER) return (OZ_BADSECATTRVER);
  indx = buff -> nentries;
  reqd = (OZ_Pointer)(buff -> e + indx) - (OZ_Pointer)buff; /* see how much storage required for this one */
  if (size < reqd) return (OZ_BADBUFFERSIZE);		/* make sure it is long enough */


  for (indx = 0; indx < buff -> nentries; indx ++) {			/* scan through entries */
    if (indx != 0) concat (ment, mprm, &str, &siz, ",");		/* if not the first, put in a comma */
    oz_hw_ztoa (buff -> e[indx].secident, sizeof hexbuf, hexbuf);	/* convert ident to hex string */
    concat (ment, mprm, &str, &siz, hexbuf);				/* concat to output */
    if (buff -> e[indx].secidmsk != (uLong)(-1)) {			/* see if mask is all ones */
      concat (ment, mprm, &str, &siz, "&");				/* if not, put in '&mask' */
      oz_hw_ztoa (buff -> e[indx].secidmsk, sizeof hexbuf, hexbuf);
      concat (ment, mprm, &str, &siz, hexbuf);
    }
    accbits = buff -> e[indx].secaccmsk;				/* get access bits */
    actbits = buff -> e[indx].secaction;				/* get action bits */
    sep = "=";								/* start with '=' separator */
    for (s = secacclas; s -> name != NULL; s ++) {			/* scan through all possible access types */
      if (s -> valu & accbits) {					/* see if access bit is set */
        accbits -= s -> valu;						/* if so, remove it from binary mask */
        concat (ment, mprm, &str, &siz, sep);				/* concat the string with separator */
        concat (ment, mprm, &str, &siz, s -> name);
        sep = "+";							/* next one gets a '+' in front of it */
      }
    }
    if (accbits != 0) goto bad;						/* bad if something undecodable */
    for (s = actions; s -> name != NULL; s ++) {			/* now scan the action bits the same way */
      if (s -> valu & actbits) {
        actbits -= s -> valu;
        concat (ment, mprm, &str, &siz, sep);
        concat (ment, mprm, &str, &siz, s -> name);
        sep = "+";
      }
    }
    if (actbits != 0) goto bad;
  }
done:
  *buff_r = str;
  return (OZ_SUCCESS);					/* return success status */

bad:
  (*ment) (mprm, siz, str, 0);				/* some error, free off buffer */
  *buff_r = NULL;					/* return null buffer pointer */
  return (OZ_BADPARAM);					/* return error status */
}

static void concat (void *(*ment) (void *mprm, uLong osize, void *obuff, uLong nsize), void *mprm, char **strp, uLong *sizp, const char *add)

{
  char *str;
  uLong len;

  if (ment != NULL) {
    str = *strp;
    len = 0;
    if (str != NULL) len = strlen (str);
    while (len + strlen (add) >= *sizp) {
      str    = (*ment) (mprm, *sizp, str, *sizp + 32);
      *sizp += 32;
      *strp  = str;
    }
    strcpy (str + len, add);
  }
}

/************************************************************************/
/*									*/
/*  Make sure the buffer contains a valid security attributes list	*/
/*									*/
/************************************************************************/

uLong oz_knl_secattr_validate (uLong size, const void *buff, const OZ_Secacclas *secacclas)

{
  char *dummy;

  return (oz_sys_secattr_bin2str (size, buff, NULL, NULL, NULL, &dummy));
}

/************************************************************************/
/*									*/
/*  Create a security attributes structure from the given buffer	*/
/*									*/
/************************************************************************/

uLong oz_knl_secattr_create (uLong size, const void *buff, const OZ_Secacclas *secacclas, OZ_Secattr **secattr_r)

{
  uLong sts;
  OZ_Secattr *secattr;

  /* If null attributes, it can only be accessed by the kernel */

  if (size == 0) {
    *secattr_r = NULL;
    return (OZ_SUCCESS);
  }

  /* Copy the list into struct */

  secattr = OZ_KNL_NPPMALLOQ (((uByte *)&(secattr -> s)) - ((uByte *)secattr) + size);
  if (secattr == NULL) return (OZ_EXQUOTANPP);
  secattr -> objtype  = OZ_OBJTYPE_SECATTR;
  secattr -> refcount = 1;
  memcpy (&(secattr -> s), buff, size);

  /* Make sure they are proper */

  sts = oz_knl_secattr_validate (size, &(secattr -> s), secacclas);
  if (sts == OZ_SUCCESS) *secattr_r = secattr;
  else OZ_KNL_NPPFREE (secattr);

  return (sts);
}

/************************************************************************/

#if WHYINCREFC
void oz_knl_secattr_whyincrefc (OZ_Secattr *secattr)

{
  int i, j;

  oz_knl_printk ("oz_knl_secattr*: secattr %p:\n", secattr);
  for (i = whyincrefc_index; (i > 0) && (i > whyincrefc_index - WHYINCREFC);) {
    if (whyincrefc_array[--i].secattr != secattr) continue;
    oz_knl_printk ("  %6u:\n", whyincrefc_array[i].repeat);
    for (j = 0; j < WHYINCREFC_FROM; j ++) {
      oz_knl_printk (" %p\n", whyincrefc_array[i].from[j]);
    }
    oz_knl_printk ("\n");
  }
}
#endif

/************************************************************************/
/*									*/
/*  Increment security attributes structure reference count		*/
/*									*/
/************************************************************************/

Long oz_knl_secattr_increfc (OZ_Secattr *secattr, Long inc)

{
#if WHYINCREFC
  int i, j;
  Long refc;
  uLong se;

  if (secattr == NULL) return (0);

  if (OZ_KNL_GETOBJTYPE (secattr) != OZ_OBJTYPE_SECATTR) {
    oz_knl_secattr_whyincrefc (secattr);
  }

  OZ_KNL_CHKOBJTYPE (secattr, OZ_OBJTYPE_SECATTR);

  se = oz_hw_smplock_wait (&oz_s_smplock_se);

  for (i = 0; (i < WHYINCREFC) && (i < whyincrefc_index); i ++) {
    if (secattr != whyincrefc_array[i].secattr) continue;
    for (j = 0; j < WHYINCREFC_FROM; j ++) {
      if (oz_hw_getrtnadr (j + 1) != whyincrefc_array[i].from[j]) break;
    }
    if (j == WHYINCREFC_FROM) {
      whyincrefc_array[i].repeat ++;
      break;
    }
  }
  if ((i == WHYINCREFC) || (i == whyincrefc_index)) {
    i = whyincrefc_index % WHYINCREFC;
    whyincrefc_array[i].repeat = 1;
    for (j = 0; j < WHYINCREFC_FROM; j ++) {
      whyincrefc_array[i].from[j] = oz_hw_getrtnadr (j + 1);
    }
    whyincrefc_index ++;
    whyincrefc_index %= 2 * WHYINCREFC;
  }

  secattr -> refcount += inc;
  refc = secattr -> refcount;
  if (refc < 0) oz_crash ("oz_knl_secattr_increfc: ref count went negative (%d)\n", refc);
  oz_hw_smplock_clr (&oz_s_smplock_se, se);

#if 00
  {
    const char *threadname;
    OZ_Thread *thread;

    thread = oz_knl_thread_getcur ();
    if (thread != NULL) {
      threadname = oz_knl_thread_getname (thread);
      if ((threadname != NULL) && (strstr (threadname, "util_top") != NULL)) {
        oz_knl_printk ("oz_knl_secattr_increfc*: %p+%d=%d ", secattr, inc, refc);
        for (i = 1; i <= 4; i ++) oz_knl_printk (" %p", oz_hw_getrtnadr (i));
        oz_knl_printk ("\n");
      }
    }
  }
#endif
#else
  Long refc;

  if (secattr == NULL) return (0);
  OZ_KNL_CHKOBJTYPE (secattr, OZ_OBJTYPE_SECATTR);
  refc = oz_hw_atomic_inc_long (&(secattr -> refcount), inc);
#endif

  if (refc == 0) OZ_KNL_NPPFREE (secattr);
  return (refc);
}

/************************************************************************/
/*									*/
/*  Get size and address of security attributes buffer			*/
/*									*/
/************************************************************************/

uLong oz_knl_secattr_getsize (OZ_Secattr *secattr)

{
  if (secattr == NULL) return (0);
  OZ_KNL_CHKOBJTYPE (secattr, OZ_OBJTYPE_SECATTR);
  return ((OZ_Pointer)(secattr -> s.e + secattr -> s.nentries) - (OZ_Pointer)&(secattr -> s));
}

void *oz_knl_secattr_getbuff (OZ_Secattr *secattr)

{
  if (secattr == NULL) return (NULL);
  OZ_KNL_CHKOBJTYPE (secattr, OZ_OBJTYPE_SECATTR);
  return (&(secattr -> s));
}

/************************************************************************/
/*									*/
/*  Convert a security keys string to binary				*/
/*									*/
/*    Input:								*/
/*									*/
/*	str  = string to be parsed					*/
/*	ment = memory allocator entrypoint				*/
/*	mprm = param to pass to ment routine				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_seckeys_str2bin = OZ_SUCCESS : successful		*/
/*	                               else : error			*/
/*	*size_r = size of resultant buffer				*/
/*	          (error char offset in str if error)			*/
/*	*buff_r = resultant buffer					*/
/*	          (NULL if error)					*/
/*									*/
/*    String format:							*/
/*									*/
/*	ident,...							*/
/*									*/
/************************************************************************/

uLong oz_sys_seckeys_str2bin (int str_l, 
                              const char *str, 
                              void *(*ment) (void *mprm, uLong osize, void *obuff, uLong nsize), 
                              void *mprm, 
                              uLong *size_r, 
                              void **buff_r)

{
  const char *p;
  int usedup;
  OZ_Secident ident;
  Seckeysbuf *buff;
  uLong indx, reqd, size;

  size 	= 0;						/* haven't allocated any buffer yet */
  buff 	= NULL;
  indx 	= 0;						/* haven't parsed anything yet */
  p    	= str;						/* point to the string */
  str_l = strnlen (p, str_l);				/* get string length */
  if (str_l == 0) goto bad;				/* must have something there */
  reqd  = 0;						/* nothing required for '*' (universal access) */
  if ((str_l == 1) && (*p == '*')) goto done;
  -- p;
  do {
    ident = oz_hw_atoz (++ p, &usedup);			/* convert the ident number string to binary */
    if (usedup == 0) goto bad;				/* it must have at least one digit */
    p += usedup;					/* skip over it */
    reqd = (OZ_Pointer)(buff -> e + indx + 1) - (OZ_Pointer)buff; /* see how much storage required for this one */
    while (reqd > size) {				/* make sure we have enough */
      buff  = (*ment) (mprm, size, buff, size + 32);
      size += 32;
    }
    buff -> e[indx].secident = ident;			/* store it */
    indx ++;
  } while (*p == ',');					/* repeat if more identifiers follow */
  if (*p != 0) goto bad;				/* error if not end-of-string */
  buff -> seckeysver = SECKEYSVER;			/* ok, set the version number */
  buff -> nentries   = indx;				/* save final number of entries */
done:
  *size_r = reqd;					/* return required size */
  *buff_r = buff;					/* return buffer pointer */
  return (OZ_SUCCESS);					/* return success status */

bad:
  (*ment) (mprm, size, buff, 0);			/* some error, free off buffer */
  *size_r = p - str;					/* return offset to error character */
  *buff_r = NULL;					/* return null buffer pointer */
  return (OZ_BADPARAM);					/* return error status */
}

/************************************************************************/
/*									*/
/*  Convert a security keys buffer to a string				*/
/*									*/
/*    Input:								*/
/*									*/
/*	size  = size of binary buffer					*/
/*	buffv = binary buffer						*/
/*	ment  = memory allocator entrypoint				*/
/*	mprm  = param to pass to ment routine				*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_seckeys_bin2str = OZ_SUCCESS : successful		*/
/*	                               else : error			*/
/*	*buff_r = resultant string buffer				*/
/*									*/
/*    String format:							*/
/*									*/
/*	ident,...							*/
/*									*/
/************************************************************************/

uLong oz_sys_seckeys_bin2str (uLong size, 
                              const void *buffv, 
                              void *(*ment) (void *mprm, uLong osize, void *obuff, uLong nsize), 
                              void *mprm, 
                              char **buff_r)

{
  char hexbuf[12], *str;
  const Seckeysbuf *buff;
  uLong indx, reqd, siz;

  /* If null buffer supplied, return '*' */

  siz = 0;
  str = NULL;
  if (size == 0) {
    concat (ment, mprm, &str, &siz, "*");
    goto done;
  }

  buff = buffv;
  if (size < sizeof *buff) return (OZ_BADBUFFERSIZE);			/* should have at least the header and one entry */
  if (buff -> seckeysver != SECKEYSVER) return (OZ_BADSECKEYSVER);
  indx = buff -> nentries;
  reqd = (OZ_Pointer)(buff -> e + indx) - (OZ_Pointer)buff;		/* see how much storage required for this one */
  if (size < reqd) return (OZ_BADBUFFERSIZE);				/* make sure it is long enough */

  siz = 0;								/* haven't allocated any buffer yet */
  str = NULL;

  for (indx = 0; indx < buff -> nentries; indx ++) {			/* scan through entries */
    if (indx != 0) concat (ment, mprm, &str, &siz, ",");		/* if not the first, put in a comma */
    oz_hw_ztoa (buff -> e[indx].secident, sizeof hexbuf, hexbuf);	/* convert ident to hex string */
    concat (ment, mprm, &str, &siz, hexbuf);				/* concat to output */
  }
done:
  *buff_r = str;
  return (OZ_SUCCESS);							/* return success status */
}

/************************************************************************/
/*									*/
/*  Create security keys struct						*/
/*									*/
/************************************************************************/

uLong oz_knl_seckeys_create (uLong size, const void *buff, OZ_Seckeys *seckey_limit, OZ_Seckeys **seckeys_r)

{
  char *dummy;
  OZ_Seckeys *seckeys;
  uLong i, j, k, sts;

  OZ_KNL_CHKOBJTYPE (seckey_limit, OZ_OBJTYPE_SECKEYS);

  /* If null size given, return the limit */

  if (size == 0) {
    oz_knl_seckeys_increfc (seckey_limit, 1);
    *seckeys_r = seckey_limit;
    return (OZ_SUCCESS);
  }

  /* Otherwise, copy the list into struct */

  seckeys = OZ_KNL_NPPMALLOQ (((uByte *)&(seckeys -> s)) - ((uByte *)seckeys) + size);
  if (seckeys == NULL) return (OZ_EXQUOTANPP);
  seckeys -> objtype  = OZ_OBJTYPE_SECKEYS;
  seckeys -> refcount = 1;
  memcpy (&(seckeys -> s), buff, size);

  /* Make sure what they gave us is valid */

  sts = oz_sys_seckeys_bin2str (size, &(seckeys -> s), NULL, NULL, &dummy);
  if (sts != OZ_SUCCESS) {
    OZ_KNL_NPPFREE (seckeys);
    return (sts);
  }

  /* If a limit was given, only keep those entries that are in the limit */

  if (seckey_limit != NULL) {
    k = 0;
    for (i = 0; i < seckeys -> s.nentries; i ++) {
      for (j = 0; j < seckey_limit -> s.nentries; j ++) {
        if (seckeys -> s.e[i].secident == seckey_limit -> s.e[j].secident) {
          seckeys -> s.e[k++] = seckeys -> s.e[i];
          break;
        }
      }
    }
    seckeys -> s.nentries = k;
  }

  *seckeys_r = seckeys;
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Increment security keys structure reference count			*/
/*									*/
/************************************************************************/

Long oz_knl_seckeys_increfc (OZ_Seckeys *seckeys, Long inc)

{
  Long refc;
  uLong se;

  if (seckeys == NULL) return (0);

  OZ_KNL_CHKOBJTYPE (seckeys, OZ_OBJTYPE_SECKEYS);

  se = oz_hw_smplock_wait (&oz_s_smplock_se);
  seckeys -> refcount += inc;
  refc = seckeys -> refcount;
  if (refc < 0) oz_crash ("oz_knl_seckeys_increfc: ref count went negative (%d)\n", refc);
  oz_hw_smplock_clr (&oz_s_smplock_se, se);
  if (refc == 0) OZ_KNL_NPPFREE (seckeys);
  return (refc);
}

/************************************************************************/
/*									*/
/*  Get size and address of security keys buffer			*/
/*									*/
/************************************************************************/

uLong oz_knl_seckeys_getsize (OZ_Seckeys *seckeys)

{
  if (seckeys == NULL) return (0);
  OZ_KNL_CHKOBJTYPE (seckeys, OZ_OBJTYPE_SECKEYS);
  return ((OZ_Pointer)(seckeys -> s.e + seckeys -> s.nentries) - (OZ_Pointer)&(seckeys -> s));
}

void *oz_knl_seckeys_getbuff (OZ_Seckeys *seckeys)

{
  if (seckeys == NULL) return (NULL);
  OZ_KNL_CHKOBJTYPE (seckeys, OZ_OBJTYPE_SECKEYS);
  return (&(seckeys -> s));
}

/************************************************************************/
/*									*/
/*  Tell if two security keys are different				*/
/*									*/
/************************************************************************/

int oz_knl_seckeys_differ (OZ_Seckeys *oldkeys, OZ_Seckeys *newkeys)

{
  if ((oldkeys != NULL) ^ (newkeys != NULL)) return (1);		/* if one is null and other isn't, they're different */
  if (newkeys == NULL) return (0);					/* if both are null, they're the same */
  if (newkeys -> s.nentries != oldkeys -> s.nentries) return (1);	/* if number of entries are different, they're different */
  return (memcmp (newkeys -> s.e, oldkeys -> s.e, oldkeys -> s.nentries * sizeof oldkeys -> s.e[0]) != 0); /* check actual keys */
}

/************************************************************************/
/*									*/
/*  Get security access to an object					*/
/*									*/
/*    Input:								*/
/*									*/
/*	seckeys   = NULL : can access anything				*/
/*	            else : pointer to security keys			*/
/*	secattr   = NULL : can only be accessed with seckeys = NULL	*/
/*	            else : pointer to security attributes		*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_security_getsecaccmsk = what accesses are allowed	*/
/*									*/
/************************************************************************/

uLong oz_knl_security_getsecaccmsk (const OZ_Seckeys *seckeys, const OZ_Secattr *secattr)

{
  OZ_Secaccmsk secaccmskdeny, secaccmskperm;
  uLong a, k;

  OZ_KNL_CHKOBJTYPE (seckeys, OZ_OBJTYPE_SECKEYS);
  OZ_KNL_CHKOBJTYPE (secattr, OZ_OBJTYPE_SECATTR);

  if (seckeys == NULL) return (-1);
  if (secattr == NULL) return (0);

  secaccmskdeny = 0;
  secaccmskperm = 0;

  for (a = 0; a < secattr -> s.nentries; a ++) {				/* scan the attribute list */
    for (k = 0; k < seckeys -> s.nentries; k ++) {				/* scan my key list */
      if (((seckeys -> s.e[k].secident ^ secattr -> s.e[a].secident) & secattr -> s.e[a].secidmsk) != 0) continue;
      if (secattr -> s.e[a].secaction & OZ_SECACTION_DENY) secaccmskdeny |= (secattr -> s.e[a].secaccmsk) & ~ secaccmskperm;
      else secaccmskperm |= (secattr -> s.e[a].secaccmsk) & ~ secaccmskdeny;
      break;
    }
  }

  return (secaccmskperm);
}

/************************************************************************/
/*									*/
/*  Check security access to an object					*/
/*									*/
/*    Input:								*/
/*									*/
/*	secaccmsk = security access mask				*/
/*	            indicates what type of access(es) required		*/
/*	seckeys   = NULL : can access anything				*/
/*	            else : pointer to security keys			*/
/*	secattr   = NULL : can only be accessed with seckeys = NULL	*/
/*	            else : pointer to security attributes		*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_security_check = OZ_SUCCESS : access is allowed		*/
/*	                              else : error status		*/
/*									*/
/*    Note:								*/
/*									*/
/*	If more than one bit in secaccmsk is set, it is as if each one 	*/
/*	were checked individually.  They ALL must be successful in 	*/
/*	order to grant access to the object.				*/
/*									*/
/************************************************************************/

uLong oz_knl_security_check (OZ_Secaccmsk secaccmsk, const OZ_Seckeys *seckeys, const OZ_Secattr *secattr)

{
  uLong a, k;

  OZ_KNL_CHKOBJTYPE (seckeys, OZ_OBJTYPE_SECKEYS);
  OZ_KNL_CHKOBJTYPE (secattr, OZ_OBJTYPE_SECATTR);

  if (seckeys == NULL) return (OZ_SUCCESS);					/* can access anything, secattr or not */
  if (secattr == NULL) return (OZ_KERNELONLY);					/* can only be accessed if seckeys is NULL */

  for (a = 0; (secaccmsk != 0) && (a < secattr -> s.nentries); a ++) {		/* scan the attribute list until we get every type of access we want */
    if ((secattr -> s.e[a].secaccmsk & secaccmsk) == 0) continue;		/* skip if it doesn't have any access mask bits match */
    for (k = 0; k < seckeys -> s.nentries; k ++) {				/* it has some bits, scan my key list */
      if (((seckeys -> s.e[k].secident ^ secattr -> s.e[a].secident) & secattr -> s.e[a].secidmsk) != 0) continue;
      if (secattr -> s.e[a].secaction & OZ_SECACTION_ALARM) {			/* got a match, maybe output an alarm message */
        /* ?? output alarm ?? */
        oz_knl_printk ("oz_knl_security_check: alarm\n");
      }
      if (secattr -> s.e[a].secaction & OZ_SECACTION_DENY) goto deny;		/* if it says to deny, deny */
      secaccmsk &= ~ secattr -> s.e[a].secaccmsk;				/* otherwise, it allows these bits */
      break;
    }
  }

  if (secaccmsk == 0) return (OZ_SUCCESS);					/* if we got all accesses, we succeeded */
deny:
  return (OZ_SECACCDENIED);							/* if there are left over access types, we are implicitly denied */
}
