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
/*  This module contains all the routines for manipulating ast blocks	*/
/*									*/
/*  AST's get queued to a thread to cause it to interrupt what it is 	*/
/*  currently doing and call an arbitrary ast routine.  When the ast 	*/
/*  routine returns, the interrupted thread is resumed exactly where 	*/
/*  it left off.							*/
/*									*/
/*  AST's can be queued to execute in whatever processor mode (USER, 	*/
/*  KERNEL) that is desired.						*/
/*									*/
/************************************************************************/

#define _OZ_KNL_AST_C
#include "ozone.h"

#include "oz_knl_ast.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_objtype.h"
#include "oz_knl_printk.h"
#include "oz_knl_sdata.h"
#include "oz_knl_status.h"
#include "oz_knl_thread.h"

#define MAXRTNADRS 16

/* Ast block */

struct OZ_Ast { OZ_Objtype objtype;					/* OZ_OBJTYPE_AST */
                OZ_Ast *next;						/* next in list */
                OZ_Ast **prev;						/* previous in list */
                OZ_Thread *thread;					/* target thread */
                OZ_Procmode procmode;					/* target processor mode */
                OZ_Astentry astentry;					/* target routine entrypoint */
                void *astparam;						/* target routine parameter */
                int express;						/* express delivery flag */
                uLong aststs;						/* target routine status */

#if 000
						/* Used for debugging */

                OZ_Thread_state thread_state;		/* threads state when ast queued */
                OZ_Astmode astmode;			/* ast mode when ast was queued */
                int q_wasempty;				/* set if the thread's ast queue was empty */
                Long thcpuidx;				/* threads cpu index when ast queued */
                Long mycpuidx;				/* my cpu index when ast queued */
                void *exesp;				/* if thcpuidx < 0, its exec stack pointer */
                char *rtnadrs[MAXRTNADRS];		/* return addresses when ast queued */
#endif
              };

/************************************************************************/
/*									*/
/*  Create an ast block							*/
/*									*/
/*    Input:								*/
/*									*/
/*	thread   = pointer to thread block the ast will be queued to	*/
/*	procmode = processor mode to deliver at				*/
/*	astentry = entrypoint of ast routine				*/
/*	astparam = parameter to pass to ast routine			*/
/*	express  = 0 : deliver only if ast delivery enabled		*/
/*	           1 : deliver even if ast delivery inhibited		*/
/*	               (for knl mode, delivered with softints inhibited)
/*	smplock <= np							*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_ast_create = OZ_SUCCESS : successful			*/
/*	                 OZ_EXQUOTANPP : caller exceeded npp quota	*/
/*	thread ref count incremented					*/
/*									*/
/*    Note:								*/
/*									*/
/*	Callers should always attempt to deliver the ast to the target 	*/
/*	thread, ie, do not delete the ast just because the event being 	*/
/*	ast'd did not happen but was aborted instead - deliver the ast 	*/
/*	then leave it to the caller to determine if the event happened 	*/
/*	or not.								*/
/*									*/
/************************************************************************/

uLong oz_knl_ast_create (OZ_Thread *thread, OZ_Procmode procmode, OZ_Astentry astentry, void *astparam, int express, OZ_Ast **ast_r)

{
  OZ_Ast *ast;

  ast = OZ_KNL_NPPMALLOQ (sizeof *ast);
  if (ast == NULL) return (OZ_EXQUOTANPP);
  ast -> objtype    = OZ_OBJTYPE_AST;
  ast -> next       = NULL;
  ast -> prev       = NULL;
  ast -> thread     = thread;
  ast -> procmode   = procmode;
  ast -> astentry   = astentry;
  ast -> astparam   = astparam;
  ast -> express    = express;

  oz_knl_thread_increfc (thread, 1);

  *ast_r = ast;

  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Insert ast block into list						*/
/*									*/
/*    Input:								*/
/*									*/
/*	ast  = pointer to ast block					*/
/*	list = pointer to listhead or to predecessors next field	*/
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_ast_insert = where to link next one to follow this one	*/
/*	*list modified to include ast block				*/
/*									*/
/************************************************************************/

OZ_Ast **oz_knl_ast_insert (OZ_Ast *ast, OZ_Ast **list)

{
  OZ_Ast *next;

  OZ_KNL_CHKOBJTYPE (ast, OZ_OBJTYPE_AST);

  if (ast -> prev != NULL) oz_crash ("oz_knl_ast_insert: ast %p already in a list", ast);
  ast -> prev = list;
  ast -> next = next = *list;
  *list = ast;
  if (next != NULL) next -> prev = &(ast -> next);

  return (&(ast -> next));
}

/************************************************************************/
/*									*/
/*  Remove ast block from list						*/
/*									*/
/*    Input:								*/
/*									*/
/*	ast  = pointer to ast block					*/
/*									*/
/*    Output:								*/
/*									*/
/*	ast block removed from list					*/
/*									*/
/************************************************************************/

void oz_knl_ast_remove (OZ_Ast *ast)

{
  OZ_Ast *next;

  OZ_KNL_CHKOBJTYPE (ast, OZ_OBJTYPE_AST);

  if (ast -> prev == NULL) oz_crash ("oz_knl_ast_remove: ast %p not in a list", ast);

  /* Remove from list */

  *(ast -> prev) = next = ast -> next;
  if (next != NULL) next -> prev = ast -> prev;

  /* Make its pointers null to indicate it's no longer in a list */

  ast -> next = ast;
  ast -> prev = NULL;
}

/************************************************************************/
/*									*/
/*  Delete an ast block							*/
/*									*/
/*    Input:								*/
/*									*/
/*	ast = ast block to delete					*/
/*	smplock = as required by oz_knl_thread_increfc			*/
/*									*/
/*    Output:								*/
/*									*/
/*	ast block freed off						*/
/*	*astentry = pointer to ast routine				*/
/*	*astparam = parameter for ast routine				*/
/*	*aststs   = status for ast routine				*/
/*	thread ref count decremented					*/
/*									*/
/*    Note:								*/
/*									*/
/*	This routine should only be called by the thread routines, 	*/
/*	as creators of ast's should always try to queue the ast to 	*/
/*	the target thread (via oz_thread_queueast) under all 		*/
/*	circumstances.							*/
/*									*/
/************************************************************************/

void oz_knl_ast_delete (OZ_Ast *ast, OZ_Astentry *astentry, void **astparam, uLong *aststs)

{
  OZ_KNL_CHKOBJTYPE (ast, OZ_OBJTYPE_AST);

  if (ast -> prev != NULL) oz_crash ("oz_knl_ast_delete: deleting ast block that's in a list");
  oz_knl_thread_increfc (ast -> thread, -1);
  if (astentry != NULL) *astentry = ast -> astentry;
  if (astparam != NULL) *astparam = ast -> astparam;
  if (aststs   != NULL) *aststs   = ast -> aststs;
  OZ_KNL_NPPFREE (ast);
}

/************************************************************************/
/*									*/
/*  Get and Set various fields in the Ast block				*/
/*									*/
/************************************************************************/

OZ_Ast *oz_knl_ast_getnext (OZ_Ast *ast)

{
  OZ_KNL_CHKOBJTYPE (ast, OZ_OBJTYPE_AST);
  if (ast -> prev == NULL) oz_crash ("oz_knl_ast_getnext: ast %p is not in a list", ast);
  return (ast -> next);
}

OZ_Procmode oz_knl_ast_getprocmode (OZ_Ast *ast)

{
  OZ_KNL_CHKOBJTYPE (ast, OZ_OBJTYPE_AST);
  return (ast -> procmode);
}

OZ_Thread *oz_knl_ast_getthread (OZ_Ast *ast)

{
  OZ_KNL_CHKOBJTYPE (ast, OZ_OBJTYPE_AST);
  return (ast -> thread);
}

int oz_knl_ast_getexpress (OZ_Ast *ast)

{
  OZ_KNL_CHKOBJTYPE (ast, OZ_OBJTYPE_AST);
  return (ast -> express);
}

void oz_knl_ast_setstatus (OZ_Ast *ast, uLong aststs)

{
  OZ_KNL_CHKOBJTYPE (ast, OZ_OBJTYPE_AST);
  ast -> aststs = aststs;
}

/************************************************************************/
/*									*/
/*  Validate an ast queue and count the elements in it			*/
/*									*/
/*    Input:								*/
/*									*/
/*	queueh = pointer to pointer to first ast in queue		*/
/*	queuet = NULL : this list doesn't keep track of last in queue	*/
/*	         else : pointer to last ast's next field		*/
/*	                (or same as 'queueh' if queue is supposedly empty)
/*									*/
/*    Output:								*/
/*									*/
/*	oz_knl_ast_validateq = number of ast's in the queue		*/
/*									*/
/************************************************************************/

int oz_knl_ast_validateq (OZ_Ast **queueh, OZ_Ast **queuet)

{
  int count;
  OZ_Ast *ast, **last;

  count = 0;
  for (last = queueh; (ast = *last) != NULL; last = &(ast -> next)) {
    if (ast -> prev != last) oz_crash ("oz_knl_ast_validateq: ast -> prev %p, last %p", ast -> prev, last);
    count ++;
  }
  if ((queuet != NULL) && (last != queuet)) oz_crash ("oz_knl_ast_validateq: queuet %p, last %p", queuet, last);
  return (count);
}

/************************************************************************/
/*									*/
/*  Dump an ast list to console						*/
/*									*/
/*    Input:								*/
/*									*/
/*	indent = number of spaces to indent				*/
/*	astl   = first ast in list					*/
/*									*/
/************************************************************************/

void oz_knl_ast_dumplist (int indent, OZ_Ast *astl)

{
  OZ_Ast *ast;

  OZ_KNL_CHKOBJTYPE (astl, OZ_OBJTYPE_AST);

  for (ast = astl; ast != NULL; ast = ast -> next) oz_knl_ast_dumpone (indent, ast);
}

/************************************************************************/
/*									*/
/*  Dump a single ast to console					*/
/*									*/
/*    Input:								*/
/*									*/
/*	indent = number of spaces to indent				*/
/*	ast    = pointer to ast to dump					*/
/*									*/
/************************************************************************/

void oz_knl_ast_dumpone (int indent, OZ_Ast *ast)

{
  OZ_KNL_CHKOBJTYPE (ast, OZ_OBJTYPE_AST);

  oz_knl_printk ("%*sAst %p: next=%p, prev=%p, thread=%p, procmode=%d, astentry=%p, astparam=%p, aststs=%u\n", indent, "", 
                 ast, ast -> next, ast -> prev, ast -> thread, ast -> procmode, ast -> astentry, ast -> astparam, ast -> aststs);
}

/************************************************************************/
/*									*/
/*  This routine records the state of the target thread when the ast 	*/
/*  is being queued.  Then, if the thread should hang with deliverable 	*/
/*  ast's stuck in its queue, we can see what state it was in when the 	*/
/*  ast was queued.							*/
/*									*/
/************************************************************************/

#include "oz_knl_hw.h"

#ifdef OZ_HW_TYPE_486
typedef struct { void *es0;
                 uLong ebx;
                 void *esi;
                 void *edi;
                 void *ebp;
                 void *eip;
               } Hwthsav;

typedef struct { uLong ustksiz;
                 uLong ustkvpg;
                 void *estackva;
                 Hwthsav *exesp;
                 uLong fpused;
                 uByte fpsave[120];
               } Hwthctx;
#else
typedef void Hwthctx;
#endif

void oz_knl_ast_setqinfo (OZ_Ast *ast, OZ_Thread_state thread_state, OZ_Astmode astmode, Long thcpuidx, Hwthctx *hwthctx, int wasempty)

{
#if 000
  Hwthsav *exesp;
  int i;
  void **ebp;

  extern char trace_begaddr[1], trace_endaddr[1];
  extern OZ_Thread *trace_thread;
  extern void trace_start (int itscurrent);

  ast -> thread_state = thread_state;			/* save thread's state just before queuing ast */
  ast -> astmode      = astmode;
  ast -> q_wasempty   = wasempty;
  ast -> thcpuidx     = thcpuidx;			/* save the cpu that the thread's context is loaded in (-1 if not) */
  ast -> mycpuidx     = oz_hw_cpu_getcur ();		/* get my current cpu index */
  if (thcpuidx == ast -> mycpuidx) {			/* see if thread's context is loaded on the current cpu */
    for (i = 0; i < MAXRTNADRS; i ++) {			/* if so, save some return addresses */
      ast -> rtnadrs[i] = oz_hw_getrtnadr (i + 1);
      if (wasempty && (ast -> thread == trace_thread) && (ast -> rtnadrs[i] >= trace_begaddr) && (ast -> rtnadrs[i] < trace_endaddr)) trace_start (1);
      if (ast -> rtnadrs[i] == NULL) break;
    }
  }
  else if (thcpuidx < 0) {
    exesp = hwthctx -> exesp;						// grab its saved stack pointer
    ast -> exesp = exesp;						// save it in the ast
    ast -> rtnadrs[0] = (void *)0x87654321;				// in case exec stack not writable
    if (OZ_HW_WRITABLE (sizeof *exesp, exesp, OZ_PROCMODE_KNL)) {	// make sure exec stack accessible
      ebp = exesp -> ebp;						// ok, get its frame pointer
      for (i = 0; i < MAXRTNADRS; i ++) {				// save this many frames at most
        if (!OZ_HW_WRITABLE (8, ebp, OZ_PROCMODE_KNL)) break;		// break it off if not accessible
        ast -> rtnadrs[i] = ebp[1];					// ok, save the return address
        if (wasempty && (ast -> thread == trace_thread) && (ast -> rtnadrs[i] >= trace_begaddr) && (ast -> rtnadrs[i] < trace_endaddr)) trace_start (0);
        ebp = ebp[0];							// pop out a level
      }
      if (i < MAXRTNADRS) ast -> rtnadrs[i] = ebp;			// if room, save the ebp that ended it all
    }
  }
#endif
}
