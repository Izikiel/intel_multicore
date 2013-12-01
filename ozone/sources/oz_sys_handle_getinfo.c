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
/*  Get information from an handle					*/
/*									*/
/*    Input:								*/
/*									*/
/*	    h = handle to retrieve info about				*/
/*	count = number of elements in 'items'				*/
/*	items = pointer to array of items to retrieve			*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_sys_handle_getinfo = OZ_SUCCESS : successful			*/
/*	                              else : error status		*/
/*	*index_r = number of elements successfully processed		*/
/*									*/
/************************************************************************/

#include "ozone.h"

#include "oz_knl_cache.h"
#include "oz_knl_devio.h"
#include "oz_knl_handle.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_phymem.h"
#include "oz_knl_process.h"
#include "oz_knl_quota.h"
#include "oz_knl_sdata.h"
#include "oz_knl_section.h"
#include "oz_knl_security.h"
#include "oz_knl_status.h"
#include "oz_knl_thread.h"
#include "oz_knl_userjob.h"

#include "oz_sys_handle_getinfo.h"
#include "oz_sys_syscall.h"

static OZ_Job *job_getnext (OZ_Job *job);
static OZ_Process *process_getnext (OZ_Process *process);
static OZ_Thread *thread_getnext (OZ_Thread *thread);
static OZ_Iochan *iochan_getnext (OZ_Iochan *iochan);
static uLong getthreadwevent (void *object, OZ_Handle_item *x, OZ_Procmode cprocmode, uLong index);
static uLong assignhandle (void *object, OZ_Handle_item *item, OZ_Procmode cprocmode, void *(*getnext) (void *object));
static OZ_User *getuserobj (void *object);
static OZ_Job *getjobobj (void *object);
static OZ_Process *getprocobj (void *object);
static OZ_Thread *getthreadobj (void *object);
static OZ_Devunit *getdevobj (void *object);
static OZ_Iochan *getiochanobj (void *object);

OZ_HW_SYSCALL_DEF_4 (handle_getinfo, OZ_Handle, h, uLong, count, const OZ_Handle_item *, items, uLong *, index_r)

{
  int si;
  uLong index, rlen, sts;
  OZ_Handle_item x;
  OZ_Quota *quota;
  OZ_Secaccmsk secaccmsk;
  OZ_Secattr *secattr;
  OZ_Seckeys *seckeys;
  OZ_Seclock *isl, *xsl;
  OZ_Thread_state threadstate;
  void *ob, *object;

  index  = 0;
  isl    = NULL;
  object = NULL;

  si = oz_hw_cpu_setsoftint (0);

  /* Get handle object pointer.  If handle is zero, use NULL.  This operation increments the object's   */
  /* reference count, so softints must be disabled so we will be sure to decrement it when we're done.  */

  object = NULL;
  secaccmsk = -1;
  if (h != 0) {
    sts = oz_knl_handle_takeout (h, cprocmode, 0, OZ_OBJTYPE_UNKNOWN, &object, &secaccmsk);
    if (sts != OZ_SUCCESS) goto rtn;
    oz_knl_handle_objincrefc (OZ_KNL_GETOBJTYPE (object), object, 1);
    oz_knl_handle_putback (h);
  }

  /* Lock the item list in memory so the caller can't unmap it on us */

  sts = oz_knl_section_blockr (cprocmode, count * sizeof *items, items, &isl);
  if (sts != OZ_SUCCESS) goto rtn;

  /* Step through the item list obtaining required info */

  for (index = 0; index < count; index ++) {

    /* Copy the itemlist element and lock the corresponding buffer */

    x = items[index];
    sts = oz_knl_section_blockw (cprocmode, x.size, x.buff, &xsl);
    if (sts != OZ_SUCCESS) break;

    /* Process the item */

    rlen = x.size;
    threadstate = OZ_THREAD_STATE_INI;
    /* (some routines assume sts already equals OZ_SUCCESS) */

    switch (x.code) {

      /* Return the object type of the originally supplied handle */
      /* Object can be anything                                   */

      case OZ_HANDLE_CODE_OBJTYPE: {
        if (object == NULL) goto baditemcode;
        if (x.size != sizeof (OZ_Objtype)) goto baditemsize;
        *(OZ_Objtype *)(x.buff) = OZ_KNL_GETOBJTYPE (object);
        break;
      }

      /* Return the address of the object pointed to by the originally supplied handle */
      /* Object can be anything                                                        */

      case OZ_HANDLE_CODE_OBJADDR: {
        if (x.size != sizeof (void *)) goto baditemsize;
        *(void **)(x.buff) = object;
        break;
      }

      /* Get list of users on system */

      case OZ_HANDLE_CODE_USER_FIRST: {
        ob  = oz_knl_user_getnext (NULL);
        sts = assignhandle (ob, &x, cprocmode, oz_knl_user_getnext);
        if (ob != NULL) oz_knl_user_increfc (ob, -1);
        break;
      }

      case OZ_HANDLE_CODE_USER_NEXT: {
        ob  = getuserobj (object);
        if (ob == NULL) goto baditemcode;
        ob  = oz_knl_user_getnext (ob);
        sts = assignhandle (ob, &x, cprocmode, oz_knl_user_getnext);
        if (ob != NULL) oz_knl_user_increfc (ob, -1);
        break;
      }

      /* This is stuff about a user                         */
      /* Object can be a thread, process, job or user block */
      /* If object is NULL, the current thread is used      */

      case OZ_HANDLE_CODE_USER_HANDLE: {
        sts = assignhandle (getuserobj (object), &x, cprocmode, NULL);
        break;
      }

      case OZ_HANDLE_CODE_USER_REFCOUNT: {
        ob  = getuserobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        if (x.size != sizeof (Long)) goto baditemsize;
        *(Long *)(x.buff) = oz_knl_user_increfc (ob, 0);
        break;
      }

      case OZ_HANDLE_CODE_USER_LOGNAMDIR: {
        ob  = getuserobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        ob  = oz_knl_user_getlognamdir (ob);
        sts = assignhandle (ob, &x, cprocmode, NULL);
        break;
      }

      case OZ_HANDLE_CODE_USER_LOGNAMTBL: {
        ob  = getuserobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        ob  = oz_knl_user_getlognamtbl (ob);
        sts = assignhandle (ob, &x, cprocmode, NULL);
        break;
      }

      case OZ_HANDLE_CODE_USER_NAME: {
        ob  = getuserobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        strncpyz (x.buff, oz_knl_user_getname (ob), x.size);
        break;
      }

      case OZ_HANDLE_CODE_USER_JOBCOUNT: {
        ob  = getuserobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        if (x.size != sizeof (uLong)) goto baditemsize;
        *(uLong *)(x.buff) = oz_knl_job_count (ob);
        break;
      }

      case OZ_HANDLE_CODE_USER_SECATTR: {
        ob  = getuserobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        secattr = oz_knl_logname_getsecattr (oz_knl_user_getlognamdir (ob));
        rlen = oz_knl_secattr_getsize (secattr);
        movc4 (rlen, oz_knl_secattr_getbuff (secattr), x.size, x.buff);
        oz_knl_secattr_increfc (secattr, -1);
        break;
      }

      case OZ_HANDLE_CODE_USER_QUOTA_USE: {
        ob  = getuserobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        quota = oz_knl_user_getquota (ob);
        sts = oz_knl_quota_string (quota, 1, x.size, x.buff);
        if (quota != NULL) oz_knl_quota_increfc (quota, -1);
        break;
      }

      case OZ_HANDLE_CODE_JOB_FIRST: {
        ob  = getuserobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        ob  = oz_knl_job_getnext (NULL, ob);
        sts = assignhandle (ob, &x, cprocmode, job_getnext);
        if (ob != NULL) oz_knl_job_increfc (ob, -1);
        break;
      }

      case OZ_HANDLE_CODE_JOB_NEXT: {
        ob  = getjobobj (object);
        if (ob == NULL) goto baditemcode;
        ob  = job_getnext (ob);
        sts = assignhandle (ob, &x, cprocmode, job_getnext);
        if (ob != NULL) oz_knl_job_increfc (ob, -1);
        break;
      }

      /* This is stuff about a job                     */
      /* Object can be a thread, process or job block  */
      /* If object is NULL, the current thread is used */

      case OZ_HANDLE_CODE_JOB_HANDLE: {
        sts = assignhandle (getjobobj (object), &x, cprocmode, NULL);
        break;
      }

      case OZ_HANDLE_CODE_JOB_REFCOUNT: {
        ob  = getjobobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        if (x.size != sizeof (Long)) goto baditemsize;
        *(Long *)(x.buff) = oz_knl_job_increfc (ob, 0);
        break;
      }

      case OZ_HANDLE_CODE_JOB_LOGNAMDIR: {
        ob  = getjobobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        ob  = oz_knl_job_getlognamdir (ob);
        sts = assignhandle (ob, &x, cprocmode, NULL);
        break;
      }

      case OZ_HANDLE_CODE_JOB_LOGNAMTBL: {
        ob  = getjobobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        ob  = oz_knl_job_getlognamtbl (ob);
        sts = assignhandle (ob, &x, cprocmode, NULL);
        break;
      }

      case OZ_HANDLE_CODE_JOB_NAME: {
        ob  = getjobobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        strncpyz (x.buff, oz_knl_job_getname (ob), x.size);
        break;
      }

      case OZ_HANDLE_CODE_JOB_PROCESSCOUNT: {
        ob  = getjobobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        if (x.size != sizeof (uLong)) goto baditemsize;
        *(uLong *)(x.buff) = oz_knl_process_count (ob);
        break;
      }

      case OZ_HANDLE_CODE_JOB_SECATTR: {
        ob  = getjobobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        secattr = oz_knl_logname_getsecattr (oz_knl_job_getlognamdir (ob));
        rlen = oz_knl_secattr_getsize (secattr);
        movc4 (rlen, oz_knl_secattr_getbuff (secattr), x.size, x.buff);
        oz_knl_secattr_increfc (secattr, -1);
        break;
      }

      case OZ_HANDLE_CODE_PROCESS_FIRST: {
        ob  = getjobobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        ob  = oz_knl_process_getnext (NULL, ob);
        sts = assignhandle (ob, &x, cprocmode, process_getnext);
        if (ob != NULL) oz_knl_process_increfc (ob, -1);
        break;
      }

      case OZ_HANDLE_CODE_PROCESS_NEXT: {
        ob  = getprocobj (object);
        if (ob == NULL) goto baditemcode;
        ob  = process_getnext (ob);
        sts = assignhandle (ob, &x, cprocmode, process_getnext);
        if (ob != NULL) oz_knl_process_increfc (ob, -1);
        break;
      }

      /* This is stuff about a process                 */
      /* Object can be a thread or process block       */
      /* If object is NULL, the current thread is used */

      case OZ_HANDLE_CODE_PROCESS_HANDLE: {
        sts = assignhandle (getprocobj (object), &x, cprocmode, NULL);
        break;
      }

      case OZ_HANDLE_CODE_PROCESS_REFCOUNT: {
        ob  = getprocobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        if (x.size != sizeof (Long)) goto baditemsize;
        *(Long *)(x.buff) = oz_knl_process_increfc (ob, 0);
        break;
      }

      case OZ_HANDLE_CODE_PROCESS_NAME: {
        ob  = getprocobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        strncpyz (x.buff, oz_knl_process_getname (ob), x.size);
        break;
      }

      case OZ_HANDLE_CODE_PROCESS_ID: {
        ob  = getprocobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        if (x.size != sizeof (OZ_Processid)) goto baditemsize;
        *(OZ_Processid *)(x.buff) = oz_knl_process_getid (ob);
        break;
      }

      case OZ_HANDLE_CODE_PROCESS_LOGNAMDIR: {
        ob  = getprocobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        ob  = oz_knl_process_getlognamdir (ob);
        sts = assignhandle (ob, &x, cprocmode, NULL);
        break;
      }

      case OZ_HANDLE_CODE_PROCESS_LOGNAMTBL: {
        ob  = getprocobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        ob  = oz_knl_process_getlognamtbl (ob);
        sts = assignhandle (ob, &x, cprocmode, NULL);
        break;
      }

      case OZ_HANDLE_CODE_PROCESS_THREADCOUNT: {
        ob  = getprocobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        if (x.size != sizeof (uLong)) goto baditemsize;
        *(uLong *)(x.buff) = oz_knl_thread_count (ob);
        break;
      }

      case OZ_HANDLE_CODE_PROCESS_SECATTR: {
        ob  = getprocobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        secattr = oz_knl_process_getsecattr (ob);
        rlen = oz_knl_secattr_getsize (secattr);
        movc4 (rlen, oz_knl_secattr_getbuff (secattr), x.size, x.buff);
        oz_knl_secattr_increfc (secattr, -1);
        break;
      }

      case OZ_HANDLE_CODE_THREAD_FIRST: {
        ob  = getprocobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        ob  = oz_knl_thread_getnext (NULL, ob);
        sts = assignhandle (ob, &x, cprocmode, thread_getnext);
        if (ob != NULL) oz_knl_thread_increfc (ob, -1);
        break;
      }

      case OZ_HANDLE_CODE_THREAD_NEXT: {
        ob  = getthreadobj (object);
        if (ob == NULL) goto baditemcode;
        ob  = thread_getnext (ob);
        sts = assignhandle (ob, &x, cprocmode, thread_getnext);
        if (ob != NULL) oz_knl_thread_increfc (ob, -1);
        break;
      }

      /* This is stuff about a thread                  */
      /* Object must be a thread block                 */
      /* If object is NULL, the current thread is used */

      case OZ_HANDLE_CODE_THREAD_HANDLE: {
        sts = assignhandle (getthreadobj (object), &x, cprocmode, NULL);
        break;
      }

      case OZ_HANDLE_CODE_THREAD_REFCOUNT: {
        ob  = getthreadobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        if (x.size != sizeof (Long)) goto baditemsize;
        *(Long *)(x.buff) = oz_knl_thread_increfc (ob, 0);
        break;
      }

      case OZ_HANDLE_CODE_THREAD_STATE: {
        ob  = getthreadobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        if (x.size != sizeof (OZ_Thread_state)) goto baditemsize;
        *(OZ_Thread_state *)(x.buff) = oz_knl_thread_getstate (ob);
        break;
      }

      case OZ_HANDLE_CODE_THREAD_BASEPRI: {
        ob  = getthreadobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        if (x.size != sizeof (uLong)) goto baditemsize;
        *(uLong *)(x.buff) = oz_knl_thread_getbasepri (ob);
        break;
      }

      case OZ_HANDLE_CODE_THREAD_CURPRIO: {
        ob  = getthreadobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        if (x.size != sizeof (uLong)) goto baditemsize;
        *(uLong *)(x.buff) = oz_knl_thread_getcurprio (ob);
        break;
      }

      case OZ_HANDLE_CODE_THREAD_TIS_RUN: if (threadstate == OZ_THREAD_STATE_INI) threadstate = OZ_THREAD_STATE_RUN;
      case OZ_HANDLE_CODE_THREAD_TIS_COM: if (threadstate == OZ_THREAD_STATE_INI) threadstate = OZ_THREAD_STATE_COM;
      case OZ_HANDLE_CODE_THREAD_TIS_WEV: if (threadstate == OZ_THREAD_STATE_INI) threadstate = OZ_THREAD_STATE_WEV;
      case OZ_HANDLE_CODE_THREAD_TIS_ZOM: if (threadstate == OZ_THREAD_STATE_INI) threadstate = OZ_THREAD_STATE_ZOM;
      case OZ_HANDLE_CODE_THREAD_TIS_INI: {
        ob  = getthreadobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        if (x.size != sizeof (OZ_Datebin)) goto baditemsize;
        *(OZ_Datebin *)(x.buff) = oz_knl_thread_gettis (ob, threadstate);
        break;
      }

      case OZ_HANDLE_CODE_THREAD_EXITSTS: {
        ob  = getthreadobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_READ)) goto secaccdenied;
        if (x.size != sizeof (uLong)) goto baditemsize;
        sts = oz_knl_thread_getexitsts (ob, x.buff);
        break;
      }

      case OZ_HANDLE_CODE_THREAD_EXITEVENT: {
        ob  = getthreadobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        sts = assignhandle (oz_knl_thread_getexitevent (ob), &x, cprocmode, NULL);
        break;
      }

      case OZ_HANDLE_CODE_THREAD_NAME: {
        ob  = getthreadobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        strncpyz (x.buff, oz_knl_thread_getname (ob), x.size);
        break;
      }

      case OZ_HANDLE_CODE_THREAD_ID: {
        ob  = getthreadobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        if (x.size != sizeof (OZ_Threadid)) goto baditemsize;
        *(OZ_Threadid *)(x.buff) = oz_knl_thread_getid (ob);
        break;
      }

      case OZ_HANDLE_CODE_THREAD_NUMIOS: {
        ob  = getthreadobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        if (x.size != sizeof (uLong)) goto baditemsize;
        *(uLong *)(x.buff) = oz_knl_thread_incios (ob, 0);
        break;
      }

      case OZ_HANDLE_CODE_THREAD_NUMPFS: {
        ob  = getthreadobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        if (x.size != sizeof (uLong)) goto baditemsize;
        *(uLong *)(x.buff) = oz_knl_thread_incpfs (ob, 0);
        break;
      }

      case OZ_HANDLE_CODE_THREAD_WEVENT0: {
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        sts = getthreadwevent (object, &x, cprocmode, 0);
        break;
      }

      case OZ_HANDLE_CODE_THREAD_WEVENT1: {
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        sts = getthreadwevent (object, &x, cprocmode, 1);
        break;
      }

      case OZ_HANDLE_CODE_THREAD_WEVENT2: {
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        sts = getthreadwevent (object, &x, cprocmode, 2);
        break;
      }

      case OZ_HANDLE_CODE_THREAD_WEVENT3: {
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        sts = getthreadwevent (object, &x, cprocmode, 3);
        break;
      }

      case OZ_HANDLE_CODE_THREAD_SECATTR: {
        ob  = getthreadobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        secattr = oz_knl_thread_getsecattr (ob);
        rlen = oz_knl_secattr_getsize (secattr);
        movc4 (rlen, oz_knl_secattr_getbuff (secattr), x.size, x.buff);
        oz_knl_secattr_increfc (secattr, -1);
        break;
      }

      case OZ_HANDLE_CODE_THREAD_DEFCRESECATTR: {
        ob  = getthreadobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        secattr = oz_knl_thread_getdefcresecattr (ob);
        rlen = oz_knl_secattr_getsize (secattr);
        movc4 (rlen, oz_knl_secattr_getbuff (secattr), x.size, x.buff);
        oz_knl_secattr_increfc (secattr, -1);
        break;
      }

      case OZ_HANDLE_CODE_THREAD_SECKEYS: {
        ob  = getthreadobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        seckeys = oz_knl_thread_getseckeys (ob);
        rlen = oz_knl_seckeys_getsize (seckeys);
        movc4 (rlen, oz_knl_seckeys_getbuff (seckeys), x.size, x.buff);
        oz_knl_seckeys_increfc (seckeys, -1);
        break;
      }

      case OZ_HANDLE_CODE_THREAD_PARENT: {
        ob  = getthreadobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        ob  = oz_knl_thread_getparent (ob);
        sts = assignhandle (ob, &x, cprocmode, NULL);
        if (ob != NULL) oz_knl_thread_increfc (ob, -1);
        break;
      }

      /* This is stuff about a device             */
      /* Object can be an I/O channel or a device */

      case OZ_HANDLE_CODE_DEVICE_HANDLE: {
        sts = assignhandle (getdevobj (object), &x, cprocmode, NULL);
        break;
      }

      case OZ_HANDLE_CODE_DEVICE_REFCOUNT: {
        ob  = getdevobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        if (x.size != sizeof (Long)) goto baditemsize;
        *(Long *)(x.buff) = oz_knl_devunit_increfc (ob, 0);
        break;
      }

      case OZ_HANDLE_CODE_DEVICE_IOCHANCOUNT: {
        ob  = getdevobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        if (x.size != sizeof (uLong)) goto baditemsize;
        *(uLong *)(x.buff) = oz_knl_iochan_count (ob);
        break;
      }

      case OZ_HANDLE_CODE_DEVICE_FIRST: {
        ob  = oz_knl_devunit_getnext (NULL);
        sts = assignhandle (ob, &x, cprocmode, oz_knl_devunit_getnext);
        if (ob != NULL) oz_knl_devunit_increfc (ob, -1);
        break;
      }

      case OZ_HANDLE_CODE_DEVICE_NEXT: {
        ob  = getdevobj (object);
        if (ob == NULL) goto baditemcode;
        ob  = oz_knl_devunit_getnext (ob);
        sts = assignhandle (ob, &x, cprocmode, oz_knl_devunit_getnext);
        if (ob != NULL) oz_knl_devunit_increfc (ob, -1);
        break;
      }

      case OZ_HANDLE_CODE_DEVICE_UNITNAME: {
        ob  = getdevobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        strncpyz (x.buff, oz_knl_devunit_devname (ob), x.size);
        break;
      }

      case OZ_HANDLE_CODE_DEVICE_ALIASNAME: {
        ob  = getdevobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        if (!oz_knl_devunit_aliasname (ob, x.size, x.buff)) sts = OZ_BUFFEROVF;
        break;
      }

      case OZ_HANDLE_CODE_DEVICE_UNITDESC: {
        ob  = getdevobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        strncpyz (x.buff, oz_knl_devunit_devdesc (ob), x.size);
        break;
      }

      case OZ_HANDLE_CODE_DEVICE_CLASSNAME: {
        ob  = getdevobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        strncpyz (x.buff, oz_knl_devunit_classname (ob), x.size);
        break;
      }

      case OZ_HANDLE_CODE_DEVICE_SECATTR: {
        ob  = getdevobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        secattr = oz_knl_devunit_getsecattr (ob);
        rlen = oz_knl_secattr_getsize (secattr);
        movc4 (rlen, oz_knl_secattr_getbuff (secattr), x.size, x.buff);
        oz_knl_secattr_increfc (secattr, -1);
        break;
      }

      /* This is stuff about an I/O channel */
      /* Object must be an I/O channel      */

      case OZ_HANDLE_CODE_IOCHAN_HANDLE: {
        sts = assignhandle (getiochanobj (object), &x, cprocmode, NULL);
        break;
      }

      case OZ_HANDLE_CODE_IOCHAN_REFCOUNT: {
        ob  = getiochanobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        if (x.size != sizeof (Long)) goto baditemsize;
        *(Long *)(x.buff) = oz_knl_iochan_increfc (ob, 0);
        break;
      }

      case OZ_HANDLE_CODE_IOCHAN_READCOUNT: {
        ob  = getiochanobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        if (x.size != sizeof (uLong)) goto baditemsize;
        *(uLong *)(x.buff) = oz_knl_iochan_readcount (ob);
        break;
      }

      case OZ_HANDLE_CODE_IOCHAN_WRITECOUNT: {
        ob  = getiochanobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        if (x.size != sizeof (uLong)) goto baditemsize;
        *(uLong *)(x.buff) = oz_knl_iochan_writecount (ob);
        break;
      }

      case OZ_HANDLE_CODE_IOCHAN_LOCKMODE: {
        ob  = getiochanobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        if (x.size != sizeof (OZ_Lockmode)) goto baditemsize;
        *(OZ_Lockmode *)(x.buff) = oz_knl_iochan_getlockmode (ob);
        break;
      }

      case OZ_HANDLE_CODE_IOCHAN_LASTIOTID: {
        ob  = getiochanobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        if (x.size != sizeof (OZ_Threadid)) goto baditemsize;
        *(OZ_Threadid *)(x.buff) = oz_knl_iochan_getlastiotid (ob);
        break;
      }

      case OZ_HANDLE_CODE_IOCHAN_SECATTR: {
        ob  = getiochanobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        secattr = oz_knl_iochan_getsecattr (ob);
        rlen = oz_knl_secattr_getsize (secattr);
        movc4 (rlen, oz_knl_secattr_getbuff (secattr), x.size, x.buff);
        oz_knl_secattr_increfc (secattr, -1);
        break;
      }

      case OZ_HANDLE_CODE_IOCHAN_FIRST: {
        ob  = getdevobj (object);
        if (ob == NULL) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        ob  = oz_knl_iochan_getnext (NULL, ob);
        sts = assignhandle (ob, &x, cprocmode, iochan_getnext);
        if (ob != NULL) oz_knl_iochan_increfc (ob, -1);
        break;
      }

      case OZ_HANDLE_CODE_IOCHAN_NEXT: {
        ob  = getiochanobj (object);
        if (ob == NULL) goto baditemcode;
        ob  = iochan_getnext (ob);
        sts = assignhandle (ob, &x, cprocmode, iochan_getnext);
        if (ob != NULL) oz_knl_iochan_increfc (ob, -1);
        break;
      }

      /* This is stuff about the system */
      /* The object is not used         */

      case OZ_HANDLE_CODE_SYSTEM_BOOTTIME: {
        if (x.size != sizeof (OZ_Datebin)) goto baditemsize;
        *(OZ_Datebin *)(x.buff) = oz_s_boottime;
        break;
      }
        
      case OZ_HANDLE_CODE_SYSTEM_PHYPAGETOTAL: {
        if (x.size != sizeof (OZ_Mempage)) goto baditemsize;
        *(OZ_Mempage *)(x.buff) = oz_s_phymem_totalpages;
        break;
      }

      case OZ_HANDLE_CODE_SYSTEM_PHYPAGEFREE: {
        if (x.size != sizeof (OZ_Mempage)) goto baditemsize;
        *(OZ_Mempage *)(x.buff) = oz_s_phymem_freepages;
        break;
      }

      case OZ_HANDLE_CODE_SYSTEM_PHYPAGEL2SIZE: {
        if (x.size != sizeof (uLong)) goto baditemsize;
        *(uLong *)(x.buff) = OZ_HW_L2PAGESIZE;
        break;
      }

      case OZ_HANDLE_CODE_SYSTEM_NPPTOTAL: {
        if (x.size != sizeof (OZ_Memsize)) goto baditemsize;
        *(OZ_Memsize *)(x.buff) = oz_s_npptotal;
        break;
      }

      case OZ_HANDLE_CODE_SYSTEM_NPPINUSE: {
        if (x.size != sizeof (OZ_Memsize)) goto baditemsize;
        *(OZ_Memsize *)(x.buff) = oz_s_nppinuse;
        break;
      }

      case OZ_HANDLE_CODE_SYSTEM_NPPPEAK: {
        if (x.size != sizeof (OZ_Memsize)) goto baditemsize;
        *(OZ_Memsize *)(x.buff) = oz_s_npppeak;
        break;
      }

      case OZ_HANDLE_CODE_SYSTEM_PGPTOTAL: {
        if (x.size != sizeof (OZ_Memsize)) goto baditemsize;
        *(OZ_Memsize *)(x.buff) = oz_s_pgptotal;
        break;
      }

      case OZ_HANDLE_CODE_SYSTEM_PGPINUSE: {
        if (x.size != sizeof (OZ_Memsize)) goto baditemsize;
        *(OZ_Memsize *)(x.buff) = oz_s_pgpinuse;
        break;
      }

      case OZ_HANDLE_CODE_SYSTEM_PGPPEAK: {
        if (x.size != sizeof (OZ_Memsize)) goto baditemsize;
        *(OZ_Memsize *)(x.buff) = oz_s_pgppeak;
        break;
      }

      case OZ_HANDLE_CODE_SYSTEM_CPUCOUNT: {
        if (x.size != sizeof (Long)) goto baditemsize;
        *(Long *)(x.buff) = oz_s_cpucount;
        break;
      }

      case OZ_HANDLE_CODE_SYSTEM_CPUSAVAIL: {
        if (x.size != sizeof (uLong)) goto baditemsize;
        *(uLong *)(x.buff) = oz_s_cpusavail;
        break;
      }

      case OZ_HANDLE_CODE_SYSTEM_SYSPAGETOTAL: {
        if (x.size != sizeof (uLong)) goto baditemsize;
        *(uLong *)(x.buff) = oz_s_sysmem_pagtblsz;
        break;
      }

      case OZ_HANDLE_CODE_SYSTEM_SYSPAGEFREE: {
        if (x.size != sizeof (uLong)) goto baditemsize;
        *(uLong *)(x.buff) = oz_s_sysmem_pagtblfr;
        break;
      }

      case OZ_HANDLE_CODE_SYSTEM_USERCOUNT: {
        if (x.size != sizeof (uLong)) goto baditemsize;
        *(uLong *)(x.buff) = oz_knl_user_count ();
        break;
      }

      case OZ_HANDLE_CODE_SYSTEM_DEVICECOUNT: {
        if (x.size != sizeof (uLong)) goto baditemsize;
        *(uLong *)(x.buff) = oz_knl_devunit_count ();
        break;
      }

      case OZ_HANDLE_CODE_SYSTEM_CACHEPAGES: {
        if (x.size != sizeof (uLong)) goto baditemsize;
        *(uLong *)(x.buff) = oz_knl_cache_pagecount;
        break;
      }

      /* This is stuff about an event flag */

      case OZ_HANDLE_CODE_EVENT_VALUE: {
        if ((object == NULL) || (OZ_KNL_GETOBJTYPE (object) != OZ_OBJTYPE_EVENT)) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        if (x.size != sizeof (Long)) goto baditemsize;
        *(Long *)(x.buff) = oz_knl_event_inc (object, 0);
        break;
      }

      case OZ_HANDLE_CODE_EVENT_NAME: {
        if ((object == NULL) || (OZ_KNL_GETOBJTYPE (object) != OZ_OBJTYPE_EVENT)) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        strncpyz (x.buff, oz_knl_event_getname (object), x.size);
        break;
      }

      case OZ_HANDLE_CODE_EVENT_GETIMINT: {
        if ((object == NULL) || (OZ_KNL_GETOBJTYPE (object) != OZ_OBJTYPE_EVENT)) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_READ)) goto secaccdenied;
        if (x.size != sizeof (OZ_Datebin)) goto baditemsize;
        *(OZ_Datebin *)(x.buff) = oz_knl_event_getimint (object);
        break;
      }

      case OZ_HANDLE_CODE_EVENT_GETIMNXT: {
        if ((object == NULL) || (OZ_KNL_GETOBJTYPE (object) != OZ_OBJTYPE_EVENT)) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_READ)) goto secaccdenied;
        if (x.size != sizeof (OZ_Datebin)) goto baditemsize;
        *(OZ_Datebin *)(x.buff) = oz_knl_event_getimnxt (object);
        break;
      }

      /* This is stuff about an logical name */

      case OZ_HANDLE_CODE_LOGNAME_SECATTR: {
        if ((object == NULL) || (OZ_KNL_GETOBJTYPE (object) != OZ_OBJTYPE_LOGNAME)) goto baditemcode;
        if (!(secaccmsk & OZ_SECACCMSK_LOOK)) goto secaccdenied;
        secattr = oz_knl_logname_getsecattr (object);
        rlen = oz_knl_secattr_getsize (secattr);
        movc4 (rlen, oz_knl_secattr_getbuff (secattr), x.size, x.buff);
        oz_knl_secattr_increfc (secattr, -1);
        break;
      }

      /* Don't know what it is */

      default: goto baditemcode;
    }
    goto unlockx;

baditemcode:
    sts = OZ_BADITEMCODE;
    goto unlockx;
secaccdenied:
    sts = OZ_SECACCDENIED;
    goto unlockx;
baditemsize:
    sts = OZ_BADITEMSIZE;

    /* Unlock the buffer and stop if error ('index' has index of bad item) */

unlockx:
    oz_knl_section_bunlock (xsl);
    if (sts != OZ_SUCCESS) break;

    /* Maybe return the return length */

    if (x.rlen != NULL) {
      sts = oz_knl_section_uput (cprocmode, sizeof *(x.rlen), &rlen, x.rlen);
      if (sts != OZ_SUCCESS) break;
    }
  }

  /* All done, unlock buffers and return final status */

rtn:
  if (isl != NULL) oz_knl_section_bunlock (isl);
  if (index_r != NULL) oz_knl_section_uput (cprocmode, sizeof index, &index, index_r);
  if ((h != 0) && (object != NULL)) oz_knl_handle_objincrefc (OZ_KNL_GETOBJTYPE (object), object, -1);
  oz_hw_cpu_setsoftint (si);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Get the next object in a list					*/
/*									*/
/************************************************************************/

	/* get the next job of the same user */

static OZ_Job *job_getnext (OZ_Job *job)

{
  return (oz_knl_job_getnext (job, NULL));
}

	/* get the next process of the same job */

static OZ_Process *process_getnext (OZ_Process *process)

{
  return (oz_knl_process_getnext (process, oz_knl_process_getjob (process)));
}

	/* get the next thread of the same process */

static OZ_Thread *thread_getnext (OZ_Thread *thread)

{
  return (oz_knl_thread_getnext (thread, NULL));
}

	/* get the next iochan of the same devunit */

static OZ_Iochan *iochan_getnext (OZ_Iochan *iochan)

{
  return (oz_knl_iochan_getnext (iochan, NULL));
}

/* Get handle to thread wait event flag */

static uLong getthreadwevent (void *object, OZ_Handle_item *item, OZ_Procmode cprocmode, uLong index)

{
  uLong sts;
  OZ_Event *event;
  OZ_Thread *thread;

  sts    = OZ_BADITEMCODE;
  thread = getthreadobj (object);
  if (thread != NULL) {
    event = oz_knl_thread_getwevent (thread, index);
    sts   = assignhandle (event, item, cprocmode, NULL);
    if (event != NULL) oz_knl_event_increfc (event, -1);
  }
  return (sts);
}

/* Assign an handle to an object.  If object is NULL, return a zero handle.         */
/* If doing a list and the new object is inaccessible, get the next accessible one. */

static uLong assignhandle (void *object, OZ_Handle_item *item, OZ_Procmode cprocmode, void *(*getnext) (void *object))

{
  int didnext;
  OZ_Handle handle;
  OZ_Secaccmsk secaccmsk;
  uLong sts;
  void *nextobj;

  didnext = 0;										/* haven't called the 'getnext' routine yet */
  if (item -> size != sizeof (OZ_Handle)) return (OZ_BADITEMSIZE);			/* must always have an handle sized buffer */
  sts = OZ_SUCCESS;
  handle = 0;
  while (object != NULL) {
    sts = oz_knl_handle_assign (object, cprocmode, &handle);				/* non-NULL, attempt to assign a new handle to it */
    if (sts != OZ_SUCCESS) break;							/* done if can't assign the handle */
    if (getnext == NULL) break;								/* done if not scanning a list of objects */
    secaccmsk = 0;									/* scanning object list, get the access caller has to object */
    oz_knl_handle_getsecaccmsk (handle, cprocmode, &secaccmsk);
    if (secaccmsk & OZ_SECACCMSK_LOOK) break;						/* done if caller can look at object */
    oz_knl_handle_release (handle, cprocmode);						/* caller can't look at it, release the handle */
    nextobj = (*getnext) (object);							/* get the next object in the list */
    if (didnext) oz_knl_handle_objincrefc (OZ_KNL_GETOBJTYPE (object), object, -1);	/* if 'object' was was from a getnext call, release it */
    object  = nextobj;									/* anyway, point to next in list */
    didnext = 1;									/* remember 'object' is from a getnext call */
    handle  = 0;									/* just in case new object is NULL */
  }
  if (didnext && (object != NULL)) oz_knl_handle_objincrefc (OZ_KNL_GETOBJTYPE (object), object, -1); /* if 'object' was was from a getnext call, release it */
  if (sts == OZ_SUCCESS) *(OZ_Handle *)(item -> buff) = handle;				/* return handle to caller */
  return (sts);										/* return status from the assign call */
}

/* Translate the given object to the corresponding user object. */
/* Return NULL if not possible.                                 */

static OZ_User *getuserobj (void *object)

{
  if (object == NULL) object = oz_knl_thread_getcur ();
  switch (OZ_KNL_GETOBJTYPE (object)) {
    case OZ_OBJTYPE_THREAD:  object = oz_knl_thread_getprocess (object);
    case OZ_OBJTYPE_PROCESS: object = oz_knl_process_getjob (object);
    case OZ_OBJTYPE_JOB:     object = oz_knl_job_getuser (object);
    case OZ_OBJTYPE_USER:    return (object);
  }
  return (NULL);
}

/* Translate the given object to the corresponding job object. */
/* Return NULL if not possible.                                */

static OZ_Job *getjobobj (void *object)

{
  if (object == NULL) object = oz_knl_thread_getcur ();
  switch (OZ_KNL_GETOBJTYPE (object)) {
    case OZ_OBJTYPE_THREAD:  object = oz_knl_thread_getprocess (object);
    case OZ_OBJTYPE_PROCESS: object = oz_knl_process_getjob (object);
    case OZ_OBJTYPE_JOB:     return (object);
  }
  return (NULL);
}

/* Translate the given object to the corresponding process object. */
/* Return NULL if not possible.                                    */

static OZ_Process *getprocobj (void *object)

{
  if (object == NULL) object = oz_knl_thread_getcur ();
  switch (OZ_KNL_GETOBJTYPE (object)) {
    case OZ_OBJTYPE_THREAD:  object = oz_knl_thread_getprocess (object);
    case OZ_OBJTYPE_PROCESS: return (object);
  }
  return (NULL);
}

/* Translate the given object to the corresponding thread object. */
/* Return NULL if not possible.                                   */

static OZ_Thread *getthreadobj (void *object)

{
  if (object == NULL) object = oz_knl_thread_getcur ();
  switch (OZ_KNL_GETOBJTYPE (object)) {
    case OZ_OBJTYPE_THREAD: return (object);
  }
  return (NULL);
}

/* Translate the given object to the corresponding devunit object. */
/* Return NULL if not possible.                                    */

static OZ_Devunit *getdevobj (void *object)

{
  if (object == NULL) return (NULL);
  switch (OZ_KNL_GETOBJTYPE (object)) {
    case OZ_OBJTYPE_IOCHAN:  object = oz_knl_iochan_getdevunit (object);
    case OZ_OBJTYPE_DEVUNIT: return (object);
  }
  return (NULL);
}

/* Translate the given object to the corresponding iochan object. */
/* Return NULL if not possible.                                   */

static OZ_Devunit *getiochanobj (void *object)

{
  if (object == NULL) return (NULL);
  switch (OZ_KNL_GETOBJTYPE (object)) {
    case OZ_OBJTYPE_IOCHAN: return (object);
  }
  return (NULL);
}
