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
/*  Write log file data							*/
/*									*/
/************************************************************************/

#define _OZ_KNL_LOG_C

#include "ozone.h"
#include "oz_knl_devio.h"
#include "oz_knl_event.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_log.h"
#include "oz_knl_objtype.h"
#include "oz_knl_status.h"

#include <stdarg.h>

#define LOGBUFFSIZE 4096

struct OZ_Log { OZ_Objtype objtype;		// OZ_OBJTYPE_LOG
                volatile Long refcount;		// number of references to object, free when zero
                OZ_Event *event;		// pointer to associated event flag
                Long insert, remove;		// insert and remove offsets in 'buffer'
                volatile Long lostlines;	// number of output lines that have been lost
                uLong size;			// size of contig space being output to
                char *buff;			// address of contig space being output to
                OZ_Smplock smplock_lg;		// output lock
                char buffer[LOGBUFFSIZE];	// ring buffer
              };

static Long insert_data (OZ_Log *log, Long offs, Long size, const char *buff);
static uLong writelog (void *logv, uLong *size, char **buff);
static Long remove_data (Long offs, OZ_Log *log, int size, char *buff);

/************************************************************************/
/*									*/
/*  Create a log file struct						*/
/*									*/
/*    Input:								*/
/*									*/
/*	smp level <= np							*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_log_create = OZ_SUCCESS : successful			*/
/*	                          else : error status			*/
/*									*/
/************************************************************************/

uLong oz_knl_log_create (const char *name, OZ_Log **log_r)

{
  OZ_Log *log;
  uLong sts;

  log = OZ_KNL_NPPMALLOQ (sizeof *log);
  if (log == NULL) return (OZ_EXQUOTANPP);

  memset (log, 0, sizeof *log);

  log -> objtype  = OZ_OBJTYPE_LOG;
  log -> refcount = 1;

  oz_hw_smplock_init (sizeof log -> smplock_lg, &(log -> smplock_lg), OZ_SMPLOCK_LEVEL_LG);

  sts = oz_knl_event_create (strlen (name), name, NULL, &(log -> event));
  if (sts == OZ_SUCCESS) *log_r = log;
  else OZ_KNL_NPPFREE (log);

  return (sts);
}

/************************************************************************/
/*									*/
/*  Increment log file struct's reference count				*/
/*									*/
/*    Input:								*/
/*									*/
/*	log = log file struct pointer					*/
/*	inc = amount to increment by					*/
/*	smp level <= np							*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_log_increfc = new reference count			*/
/*	                     log struct freed if zero			*/
/*									*/
/************************************************************************/

Long oz_knl_log_increfc (OZ_Log *log, Long inc)

{
  Long refc;

  OZ_KNL_CHKOBJTYPE (log, OZ_OBJTYPE_LOG);
  refc = oz_hw_atomic_inc_long (&(log -> refcount), inc);
  if (refc < 0) oz_crash ("oz_knl_log_increfc: ref count %d", refc);
  if (refc == 0) OZ_KNL_NPPFREE (log);
  return (refc);
}

/************************************************************************/
/*									*/
/*  Print something to the log file					*/
/*									*/
/*    Input:								*/
/*									*/
/*	file = pointer to source file name string			*/
/*	line = source file line number					*/
/*	format = printf format string					*/
/*	... = format string's parameters				*/
/*	smplevel <= hi							*/
/*									*/
/************************************************************************/

void oz_knl_log_print (OZ_Log *log, const char *file, int line, const char *format, ...)

{
  va_list ap;

  va_start (ap, format);
  oz_knl_log_vprint (log, file, line, format, ap);
  va_end (ap);
}

void oz_knl_log_vprint (OZ_Log *log, const char *file, int line, const char *format, va_list ap)

{
  Long insert, room;
  OZ_Datebin now;
  uLong lg, sts;

  if (log == NULL) {								// see if log is open
    oz_knl_printkv (format, ap);						// if not, print to console
    return;
  }

  OZ_KNL_CHKOBJTYPE (log, OZ_OBJTYPE_LOG);

  lg = oz_hw_smplock_wait (&(log -> smplock_lg));

  insert = log -> insert;							// this is where we start inserting data
  room   = log -> remove;							// this points to what hasn't been written out yet
  if (room <= insert) room += sizeof log -> buffer;				// maybe it needs to be wrapped
  room  -= insert;								// now see how much room is left

  if (room < sizeof (OZ_Datebin) + sizeof (char *) + sizeof (int) + 2) {	// see if there is enough room
    OZ_HW_ATOMIC_INCBY1_LONG (log -> lostlines);				// if not, line is lost
  } else {
    now    = oz_hw_tod_getnow ();						// put in current date/time
    insert = insert_data (log, insert, sizeof now,  (char *)&now);
    insert = insert_data (log, insert, sizeof file, (char *)&file);		// put in pointer to source filename string
    insert = insert_data (log, insert, sizeof line, (char *)&line);		// put in source linenumber

										// there are at least 2 bytes left -
										// - one for the null terminator
										// - one for the gap byte

    log -> size = sizeof log -> buffer - insert;				// assume we can go all the way to end of buffer
    if (log -> remove > insert) log -> size = log -> remove - insert - 1;	// if not, just go to remove point but leave 1 byte
    else if (log -> remove == 0) log -> size --;				// if so, make sure there is at least 1 byte left
    log -> buff = log -> buffer + insert;					// point to where the string goes

    sts = oz_sys_vxprintf (writelog, log, log -> size, log -> buff, NULL, format, ap);

    if (sts != OZ_SUCCESS) OZ_HW_ATOMIC_INCBY1_LONG (log -> lostlines);		// if failure (buffer overflow?), the line is lost
    else {
      insert = log -> buff - log -> buffer;					// successful, update insertion point past terminating null
      log -> buffer[insert++] = 0;
      if (insert == sizeof log -> buffer) insert = 0;
      if (insert != log -> remove) log -> insert = insert;
      else OZ_HW_ATOMIC_INCBY1_LONG (log -> lostlines);
    }
  }

  oz_hw_smplock_clr (&(log -> smplock_lg), lg);					// unlock
  oz_knl_event_set (log -> event, 1);						// wake up the daemon
}

static Long insert_data (OZ_Log *log, Long offs, Long size, const char *buff)

{
  if (offs + size >= sizeof log -> buffer) {
    memcpy (log -> buffer + offs, buff, sizeof log -> buffer - offs);
    size -= sizeof log -> buffer - offs;
    buff += sizeof log -> buffer - offs;
    offs  = 0;
  }

  memcpy (log -> buffer + offs, buff, size);
  return (offs + size);
}

static uLong writelog (void *logv, uLong *size, char **buff)

{
  OZ_Log *log;

  log = logv;

  log -> buff = *buff + *size;								// increment past written data
  if (log -> buff == log -> buffer + sizeof log -> buffer) log -> buff = log -> buffer;	// maybe wrap pointer
  *buff = log -> buff;									// return pointer to new area
  *size = log -> buffer + sizeof log -> buffer - log -> buff;				// assume we can write all the way to end
  if (log -> buffer + log -> remove > log -> buff) *size = log -> buffer + log -> remove - log -> buff - 1; // maybe stop at remove spot and leave a gap
  else if (log -> remove == 0) -- (*size);						// othewise, make sure there is a gap byte on end if not at beginning
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Wait for some log data to be available				*/
/*									*/
/*    Input:								*/
/*									*/
/*	log = pointer to log struct					*/
/*	smplevel <= softint						*/
/*									*/
/************************************************************************/

void oz_knl_log_wait (OZ_Log *log)

{
  OZ_KNL_CHKOBJTYPE (log, OZ_OBJTYPE_LOG);
  oz_knl_event_waitone (log -> event);
  oz_knl_event_set (log -> event, 0);
}

/************************************************************************/
/*									*/
/*  Remove a record from log buffer					*/
/*									*/
/*    Input:								*/
/*									*/
/*	log  = pointer to log struct					*/
/*	size = size of area pointed to by 'buffer'			*/
/*	smplevel <= hi							*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_log_remove = OZ_SUCCESS : record removed			*/
/*	                    OZ_PENDING : log buffer was empty		*/
/*	                  OZ_BUFFEROVF : 'size' is too small, try again	*/
/*	*lostlines_r = number of buffers lost due to overflow		*/
/*	*datebin_r = date/time stamp of record				*/
/*	*file_r = points to source filename string in kernel		*/
/*	*line_r = source file line number				*/
/*	*buffer = filled in with message text (null terminated)		*/
/*									*/
/************************************************************************/

uLong oz_knl_log_remove (OZ_Log *log, Long *lostlines_r, OZ_Datebin *datebin_r, 
                         const char **file_r, int *line_r, int size, char *buffer)

{
  Long insert, length, remove;

  OZ_KNL_CHKOBJTYPE (log, OZ_OBJTYPE_LOG);

  insert = log -> insert;
  remove = log -> remove;
  if (insert == remove) return (OZ_PENDING);

  remove = remove_data (remove, log, sizeof *datebin_r, (char *)datebin_r);
  remove = remove_data (remove, log, sizeof *file_r,    (char *)file_r);
  remove = remove_data (remove, log, sizeof *line_r,    (char *)line_r);

  if (remove < insert) length = strnlen (log -> buffer + remove, insert - remove);
  else {
    length = strnlen (log -> buffer + remove, sizeof log -> buffer - remove);
    if (length == sizeof log -> buffer - remove) length += strnlen (log -> buffer, sizeof log -> buffer);
  }
  if (++ length > size) return (OZ_BUFFEROVF);
  log -> remove = remove_data (remove, log, length, buffer);

  *lostlines_r  = oz_hw_atomic_set_long (&(log -> lostlines), 0);

  return (OZ_SUCCESS);
}

/* Remove 'size' bytes of data from the buffer at 'offs' */

static Long remove_data (Long offs, OZ_Log *log, int size, char *buff)

{
  if (offs + size >= sizeof log -> buffer) {
    memcpy (buff, log -> buffer + offs, sizeof log -> buffer - offs);
    size -= sizeof log -> buffer - offs;
    buff += sizeof log -> buffer - offs;
    offs  = 0;
  }
  memcpy (buff, log -> buffer + offs, size);
  return (offs + size);
}
