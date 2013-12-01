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
/*  Make utiliti							*/
/*									*/
/************************************************************************/

#include "ozone.h"
#include "oz_crtl_malloc.h"
#include "oz_io_fs.h"
#include "oz_io_pipe.h"
#include "oz_knl_lock.h"
#include "oz_knl_status.h"
#include "oz_sys_io.h"
#include "oz_sys_io_fs.h"
#include "oz_sys_io_fs_printf.h"
#include "oz_util_start.h"

typedef struct Var Var;
typedef struct Target Target;
typedef struct Depend Depend;
typedef struct Command Command;

struct Command { Command *next;		/* next in depend -> commands list */
                 char string[1];	/* the command string */
               };

struct Depend { Depend *next;		/* next in depends list */
                int line;		/* source line number */
                int uptodate;		/* set if file known to be up-to-date */
                OZ_Datebin date;	/* file's modification date */
                Command *commands;	/* list of commands used to make the file */
                Command **commandt;	/* (end of commands list) */
                char *valu;		/* list of files dependent on this one (points past null terminator in name) */
                char name[1];		/* this file's name */
              };

struct Target { Target *next;	/* next in targets list */
                char *name;	/* points to target name (in argv[...]) */
              };

struct Var { Var *next;		/* next in vars list */
             char *valu;	/* points to value (points past null terminator in name) */
             char name[1];	/* variable's name string */
           };

static char *make_file, *pn;
static Depend *depends;
static int make_line, quiet, verbose;
static OZ_Handle h_pipe;
static Var *vars;

static uLong find_depend (char *name, Depend **depend_r);
static uLong make_depend (Depend *depend);
static uLong find_file (char *name, OZ_Datebin *date_r);

uLong oz_util_main (int argc, char *argv[])

{
  char c, makerec[4096], *p, procbuf[65536];
  char *image_name, *output_file;
  Command *command;
  Depend *depend, **ldepend;
  int i, j, s;
  uLong exitsts, makerec_l, sts;
  OZ_Handle h_event, h_make, h_thread;
  OZ_IO_fs_create fs_create;
  OZ_IO_fs_open fs_open;
  OZ_IO_fs_readrec fs_readrec;
  Target **ltarget, *target, *targets;
  Var **lvar, *var, *vars;

  pn = "make";
  if (argc > 0) pn = argv[0];

  image_name  = NULL;
  make_line   = 0;
  make_file   = "makefile.ozmake";
  output_file = NULL;
  quiet       = 0;
  vars        = NULL;
  depends     = NULL;
  ltarget     = &targets;
  verbose     = 0;

  /* Parse command line parameters */

  for (i = 1; i < argc; i ++) {

    /* -define <varname>=<varvalue> */

    if (strcmp (argv[i], "-define") == 0) {
      if (++ i == argc) goto usage;
      var = malloc (sizeof *var + strlen (argv[i]));
      strcpy (var -> name, argv[i]);
      p = strchr (var -> name, '=');
      if (p == NULL) goto usage;
      *(p ++) = 0;
      var -> valu = p;
      var -> next = vars;
      vars = var;
      continue;
    }

    /* -image <name_of_image_to_spawn> */

    if (strcmp (argv[i], "-image") == 0) {
      if (++ i == argc) goto usage;
      output_file = NULL;
      image_name  = argv[i];
      continue;
    }

    /* -input <name_of_makefile> */

    if (strcmp (argv[i], "-input") == 0) {
      if (++ i == argc) goto usage;
      make_file = argv[i];
      continue;
    }

    /* -output <name_of_script_to_write> */

    if (strcmp (argv[i], "-output") == 0) {
      if (++ i == argc) goto usage;
      image_name  = NULL;
      output_file = argv[i];
      continue;
    }

    /* -quiet */

    if (strcmp (argv[i], "-quiet") == 0) {
      quiet = 1;
      continue;
    }

    /* -verbose */

    if (strcmp (argv[i], "-verbose") == 0) {
      verbose = 1;
      continue;
    }

    /* no other options allowed */

    if (argv[i][0] == '-') goto usage;

    /* the rest are target names */

    target = malloc (sizeof *target);
    *ltarget = target;
    ltarget = &(target -> next);
    target -> name = argv[i];
  }

  *ltarget = NULL;

  /* Open the make file */

  memset (&fs_open, 0, sizeof fs_open);
  fs_open.name = make_file;
  fs_open.lockmode = OZ_LOCKMODE_PR;
  sts = oz_sys_io_fs_open (sizeof fs_open, &fs_open, 0, &h_make);
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u opening makefile %s\n", pn, sts, make_file);
    return (sts);
  }
  if (verbose) oz_sys_io_fs_printf (oz_util_h_error, "%s: opened makefile %s\n", pn, make_file);

  /* Read it into memory */

  memset (&fs_readrec, 0, sizeof fs_readrec);
  fs_readrec.size = sizeof makerec - 1;
  fs_readrec.buff = makerec;
  fs_readrec.rlen = &makerec_l;
  fs_readrec.trmsize = 1;
  fs_readrec.trmbuff = "\n";
  fs_readrec.pmtsize = 1;
  fs_readrec.pmtbuff = ">";

  depend = NULL;

  while ((sts = oz_sys_io (OZ_PROCMODE_KNL, h_make, 0, OZ_IO_FS_READREC, sizeof fs_readrec, &fs_readrec)) == OZ_SUCCESS) {
    makerec[makerec_l] = 0;
    make_line ++;

    /* Remove redundant spaces, strip comments, substitute in variables */

    j = 0;
    s = 1;
    for (i = 0; ((c = makerec[i]) != 0) && (j < sizeof procbuf - 1); i ++) {
      if (c == '#') break;				/* all done if comment character */
      if ((c == '$') && (makerec[i+1] == '(')) {	/* $(varname) means substitute in that variable */
        i += 2;
        p = strchr (makerec + i, ')');			/* ... find the closing ) */
        if (p == NULL) {
          oz_sys_io_fs_printf (oz_util_h_error, "%s: %s %u: no ) found after $(\n", pn, make_file, make_line);
          return (OZ_BADPARAM);
        }
        s = p - makerec - i;				/* ... scan variables list for it */
        for (var = vars; var != NULL; var = var -> next) {
          if ((var -> name[s] == 0) && (memcmp (var -> name, makerec + i, s) == 0)) {
            s = strlen (var -> valu);			/* ... insert corresponding value */
            memcpy (procbuf + j, var -> valu, s);
            j += s;
            break;
          }
        }
        s = 0;						/* whatever it was, it ends with non-whitespace */
        i = p - makerec;				/* skip over the variable name in makerec */
      } else if (c == '\\') {				/* check for escape character */
        c = makerec[++i];				/* escape, get the following character */
        if (c == 0) {
          sts = oz_sys_io (OZ_PROCMODE_KNL, h_make, 0, OZ_IO_FS_READREC, sizeof fs_readrec, &fs_readrec);
          if (sts != OZ_SUCCESS) goto readerror;	/* escape followed by end-of-line, it is a continuation */
          makerec[makerec_l] = 0;
          make_line ++;
          i = -1;					/* continue processing the new buffer */
        } else {
          procbuf[j++] = c;				/* not eol after escape, copy to output as a non-whitespace char */
          s = 0;					/* and remember we output a non-whitespace character */
        }
      } else if (c > ' ') {				/* normal, see if it is whitespace */
        procbuf[j++] = c;				/* non-whitespace, copy to output buffer */
        s = 0;						/* say the last thing was not a space */
      } else if (!s) {					/* whitespace char, see if last char was a space */
        procbuf[j++] = ' ';				/* if not, output a single space */
        s = 1;						/* remember we did so we don't do more than one */
      }
    }
    if (j > 0) j -= s;					/* remove any trailing whitespace */
    if (j == 0) continue;				/* skip blank lines */
    procbuf[j] = 0;					/* terminate */

    /* If first token has an = in it or second token starts with one, it is a variable definition */

    for (i = 0; (c = procbuf[i]) != 0; i ++) {
      if (c == '=') goto gotvardef;
      if ((c == ' ') && (procbuf[i+1] == '=')) goto gotvardef;
      if (c == ' ') goto novardef;
    }
    goto novardef;
gotvardef:
    for (lvar = &vars; (var = *lvar) != NULL; lvar = &(var -> next)) {		/* scan var list for that variable */
      if ((var -> name[i] == 0) && (memcmp (var -> name, procbuf, i) == 0)) {
        *lvar = var -> next;							/* if found, delete the old value */
        free (var);
        break;
      }
    }
    var = malloc (sizeof *var + strlen (procbuf));				/* allocate a new buffer */
    strcpy (var -> name, procbuf);						/* copy in the name=value string */
    var -> name[i++] = 0;							/* null terminate the name */
    if (c == ' ') i ++;								/* maybe it was ' =' and not just '=' */
    var -> valu = var -> name + i;						/* point at the value (just past the '=') */
    while (*(var -> valu) == ' ') var -> valu ++;				/* skip any leading spaces */
    var -> next = vars;								/* link it to list */
    vars = var;
    if (verbose) oz_sys_io_fs_printf (oz_util_h_error, "%s: var %s = %s\n", pn, var -> name, var -> valu);
    continue;									/* get next input line */
novardef:

    /* Not a variable, if first token ends with an ':', it is a dependency list */

    p = strchr (procbuf, ' ');							/* find end of first token */
    if ((p != NULL) && (p[-1] == ':')) {					/* see if it ends with a colon */
      i = p - procbuf - 1;							/* ok, get length before the colon */
      for (ldepend = &depends; (depend = *ldepend) != NULL; ldepend = &(depend -> next)) {
        if ((depend -> name[i] == 0) && (memcmp (depend -> name, procbuf, i) == 0)) {
          oz_sys_io_fs_printf (oz_util_h_error, "%s: %s %u: dependency %s found more than once (other at %u)\n", pn, make_file, make_line, depend -> name, depend -> line);
          return (OZ_BADPARAM);
        }
      }
      depend = malloc (sizeof *depend + strlen (procbuf) + 1);			/* not duplicated, allocate a buffer for it */
      memset (depend, 0, sizeof *depend);					/* clear out the fixed header portion */
      depend -> line = make_line;						/* save the line number */
      depend -> commandt = &(depend -> commands);				/* initialize command list tail pointer */
      strcpy (depend -> name, procbuf);						/* copy the line buffer in */
      depend -> name[i++] = 0;							/* null terminate dependency name */
      depend -> valu = depend -> name + i + 1;					/* set up pointer to value string */
      *ldepend = depend;							/* link on to end of dependencies list */
      ldepend = &(depend -> next);
      if (verbose) oz_sys_io_fs_printf (oz_util_h_error, "%s: dependency %s: %s\n", pn, depend -> name, depend -> valu);
      continue;
    }

    /* It is a command to be executed for the current dependency */

    if (depend == NULL) {							/* make sure we have a dependency going */
      oz_sys_io_fs_printf (oz_util_h_error, "%s: %s %u: command line before any dependency\n", pn, make_file, make_line);
      return (OZ_BADPARAM);
    }
    command = malloc (sizeof *command + strlen (procbuf) + 1);			/* ok, allocate a buffer for it */
    command -> next = NULL;
    strcpy (command -> string, procbuf);					/* copy in the command string */
    *(depend -> commandt) = command;						/* link on to end of dependency's command list */
    depend -> commandt = &(command -> next);
    if (verbose) oz_sys_io_fs_printf (oz_util_h_error, "%s:   %s\n", pn, command -> string);
  }
readerror:
  oz_sys_handle_release (OZ_PROCMODE_KNL, h_make);				/* close make file */
  if (verbose) oz_sys_io_fs_printf (oz_util_h_error, "%s: end of input\n", pn);

  if (depends == NULL) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: no dependencies found in %s\n", pn, make_file);
    return (OZ_BADPARAM);
  }

  /* Create output file or pipe */

  h_thread = 0;
  if (output_file != NULL) {
    memset (&fs_create, 0, sizeof fs_create);
    fs_create.name = output_file;
    fs_create.lockmode = OZ_LOCKMODE_PW;
    sts = oz_sys_io_fs_create (sizeof fs_create, &fs_create, 0, &h_pipe);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u creating output file %s\n", pn, sts, output_file);
      return (sts);
    }
    if (verbose) oz_sys_io_fs_printf (oz_util_h_error, "%s: writing output file %s\n", pn, output_file);
  } else {
    if (image_name == NULL) image_name = "oz_cli.oz";
    sts = oz_sys_io_assign (OZ_PROCMODE_KNL, &h_pipe, OZ_IO_PIPES_TEMPLATE, OZ_LOCKMODE_EX);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u assigning channel to %s\n", pn, sts, OZ_IO_PIPES_TEMPLATE);
      return (sts);
    }
    sts = oz_sys_event_create (OZ_PROCMODE_KNL, "make cli", &h_event);
    if (sts != OZ_SUCCESS) oz_sys_condhand_signal (2, sts, 0);
    sts = oz_sys_spawn (0, image_name, h_pipe, 0, 0, 0, h_event, NULL, 0, NULL, "make cli", &h_thread, NULL);
    if (sts != OZ_SUCCESS) {
      oz_sys_io_fs_printf (oz_util_h_error, "%s: error %u spawning cli image %s\n", pn, sts, image_name);
      return (sts);
    }
  }

  /* Output initial lines to the pipe */

  oz_sys_io_fs_printf (h_pipe, "create symbol oz_make_default_h {oz_lnm_lookup (\"OZ_DEFAULT_DIR\", \"user\")}\n");
  oz_sys_io_fs_printf (h_pipe, "create symbol oz_make_default {oz_lnm_string (oz_make_default_h, 0)}\n");
  oz_sys_io_fs_printf (h_pipe, "close handle {oz_make_default_h}\n");

  /* If no target given on command line, make the first dependency */

  if (targets == NULL) sts = make_depend (depends);
  else {
    for (target = targets; target != NULL; target = target -> next) {
      sts = find_depend (target -> name, &depend);
      if (sts != OZ_SUCCESS) break;
      sts = make_depend (depend);
      if (sts != OZ_SUCCESS) break;
    }
  }

  if (verbose) oz_sys_io_fs_printf (oz_util_h_error, "%s: scan complete, status %u\n", pn, sts);

  /* If a process was spawned, wait for it to exit */

  if (sts == OZ_SUCCESS) {
    oz_sys_io_fs_printf (h_pipe, "done: create symbol sts {oz_status}\n");	/* write final commands to pipe */
    oz_sys_io_fs_printf (h_pipe, "set default {oz_make_default}\n");
    oz_sys_io_fs_printf (h_pipe, "exit {sts}\n");
    oz_sys_handle_release (OZ_PROCMODE_KNL, h_pipe);				/* close so cli will see the eof */
    if (h_thread != 0) {							/* see if we spawned a cli */
      while ((sts = oz_sys_thread_getexitsts (h_thread, &exitsts)) == OZ_FLAGWASCLR) { /* if so, see if it has exited */
        oz_sys_event_wait (OZ_PROCMODE_KNL, h_event, 0);			/* if not, wait for it */
        oz_sys_event_set (OZ_PROCMODE_KNL, h_event, 0, NULL);			/* clear event in case we have to wait again */
      }
      if (sts == OZ_SUCCESS) sts = exitsts;					/* get the exit status */
      if (sts != OZ_SUCCESS) oz_sys_io_fs_printf (oz_util_h_error, "%s: error exit status %u\n", pn, sts);
    }
  }

  /* Return final status */

  return (sts);

usage:
  oz_sys_io_fs_printf (oz_util_h_error, "%s: usage: %s [<target> ...]\n", pn, pn);
  oz_sys_io_fs_printf (oz_util_h_error, "%s:	[-define <name>=<value> ...]\n", pn);
  oz_sys_io_fs_printf (oz_util_h_error, "%s:	[-input <makefile>]\n", pn);
  oz_sys_io_fs_printf (oz_util_h_error, "%s:	[-output <scriptfile> or -image <imagename>]\n", pn);
  oz_sys_io_fs_printf (oz_util_h_error, "%s:	[-quiet]\n", pn);
  oz_sys_io_fs_printf (oz_util_h_error, "%s:	[-verbose]\n", pn);
  oz_sys_io_fs_printf (oz_util_h_error, "%s:\n", pn);
  oz_sys_io_fs_printf (oz_util_h_error, "%s:   <makefile> defaults to makefile.ozmake\n", pn);
  oz_sys_io_fs_printf (oz_util_h_error, "%s:   <scriptfile> defaults to -image setting\n", pn);
  oz_sys_io_fs_printf (oz_util_h_error, "%s:   <imagename> defaults to oz_cli.oz\n", pn);
  oz_sys_io_fs_printf (oz_util_h_error, "%s:   <target> defaults to the first one\n", pn);
  return (OZ_MISSINGPARAM);
}

/************************************************************************/
/*									*/
/*  Find a dependency given its name					*/
/*									*/
/*    Input:								*/
/*									*/
/*	name = pointer to name string					*/
/*									*/
/*    Output:								*/
/*									*/
/*	find_depend = OZ_SUCCESS : successful				*/
/*	                    else : error status				*/
/*	*depend_r = pointer to dependency				*/
/*									*/
/*    Note:								*/
/*									*/
/*	If the dependency is not in the list, but a file exists with 	*/
/*	the given name, the file is added to the dependency list and 	*/
/*	its pointer is returned.  If the file does not exist, an error 	*/
/*	message is output and an error status is returned.		*/
/*									*/
/************************************************************************/

static uLong find_depend (char *name, Depend **depend_r)

{
  Depend *depend, **ldepend;
  uLong sts;
  OZ_Datebin my_date;

  /* Find the named dependency */

  for (ldepend = &depends; (depend = *ldepend) != NULL; ldepend = &(depend -> next)) {
    if (strcmp (depend -> name, name) == 0) {
      *depend_r = depend;
      return (OZ_SUCCESS);
    }
  }

  /* Not found in list, it better be an existing file */

  sts = find_file (name, &my_date);						/* try to find the file */
  if ((sts == OZ_SUCCESS) && !OZ_HW_DATEBIN_TST (my_date)) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: %s %u: file %s doesn't exist and I don't know how to make it\n", pn, make_file, make_line, name);
    sts = OZ_NOSUCHFILE;							/* don't get away with 'no such file' */
  }

  /* If it was found, make a dependency entry for it and put on end of list */

  if (sts == OZ_SUCCESS) {
    depend = malloc (sizeof *depend + strlen (name) + 1);			/* allocate a buffer */
    memset (depend, 0, sizeof *depend);						/* clear out fixed header portion */
    strcpy (depend -> name, name);						/* copy in the name */
    depend -> valu = "";							/* this file doesn't depend on anything */
    depend -> uptodate = 1;							/* ... so it is therefore up-to-date by definition */
    depend -> date = my_date;							/* save its modification date */
    *ldepend = depend;								/* put on end of depends list */
    if (verbose) oz_sys_io_fs_printf (oz_util_h_error, "%s: file %s modified %t\n", pn, name, my_date);
  }

  *depend_r = depend;
  return (sts);
}

/************************************************************************/
/*									*/
/*  Make a dependency if it is not already up-to-date			*/
/*									*/
/*    Input:								*/
/*									*/
/*	depend = dependency to make					*/
/*									*/
/*    Output:								*/
/*									*/
/*	make_depend = OZ_SUCCESS : successful				*/
/*	                    else : error status				*/
/*	depend -> uptodate = 1						*/
/*	depend -> date = its modification date				*/
/*									*/
/************************************************************************/

static uLong make_depend (Depend *depend)

{
  char *p, *q;
  Command *command;
  Depend *child;
  int make_myself;
  uLong sts;

  /* If this one has already been made, don't make it again    */
  /* Otherwise, mark it up to date (so we don't recurse on it) */

  if (depend -> uptodate) return (OZ_SUCCESS);					/* if it has already been made, don't re-make it */
  sts = find_file (depend -> name, &(depend -> date));				/* try to find the corresponding file */
  if (sts != OZ_SUCCESS) return (sts);						/* if bad failure, return error status */
  depend -> uptodate = 1;							/* ok, mark this one as being up-to-date (do this before */
										/* we actually make it to prevent any infinite loops) */
  if (verbose) {
    if (!OZ_HW_DATEBIN_TST (depend -> date)) oz_sys_io_fs_printf (oz_util_h_error, "%s: file %s doesn't exist\n", pn, depend -> name);
    else oz_sys_io_fs_printf (oz_util_h_error, "%s: file %s modified %t\n", pn, depend -> name, depend -> date);
  }

  /* Before we make it, make any out-of-date children        */
  /* Then, if any children are newer than me, re-make myself */

  make_myself = 0;								/* assume all children are older than me */
  for (p = depend -> valu; (q = strchr (p, ' ')) != NULL; p = q) {		/* scan through list of children */
    *(q ++) = 0;								/* null terminate child name */
    sts = find_depend (p, &child);						/* find it's dependency entry */
    if (sts != OZ_SUCCESS) return (sts);
    sts = make_depend (child);							/* make sure the child is all up-to-date */
    if (sts != OZ_SUCCESS) return (sts);
    if (OZ_HW_DATEBIN_CMP (child -> date, depend -> date) >= 0) {		/* if it is newer than me, I need to be re-made */
      if (verbose) oz_sys_io_fs_printf (oz_util_h_error, "%s: %s newer than %s\n", pn, p, depend -> name);
      make_myself = 1;
    }
  }

  if (*p != 0) {								/* check out the last in the list */
    sts = find_depend (p, &child);						/* find it's dependency entry */
    if (sts != OZ_SUCCESS) return (sts);
    sts = make_depend (child);							/* make sure the child is all up-to-date */
    if (sts != OZ_SUCCESS) return (sts);
    if (OZ_HW_DATEBIN_CMP (child -> date, depend -> date) >= 0) {		/* if it is newer than me, I need to be re-made */
      if (verbose) oz_sys_io_fs_printf (oz_util_h_error, "%s: %s newer than %s\n", pn, p, depend -> name);
      make_myself = 1;
    }
  }

  /* Now if any children are newer than me, output my commands to re-make myself */

  if (make_myself) {								/* see if any children are newer than me */
    while ((command = depend -> commands) != NULL) {				/* if so, output all my commands */
      depend -> commands = command -> next;
      if (!quiet) oz_sys_io_fs_printf (oz_util_h_output, "%s\n", command -> string);
      oz_sys_io_fs_printf (h_pipe, "%s\nif {oz_status!=%u} goto done\n", command -> string, OZ_SUCCESS);
      free (command);
    }
    depend -> date = oz_hw_tod_getnow ();					/* ... then set my modification date to 'now' */
  }

  /* Successful */

  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Find a file on disk - return its modification date			*/
/*									*/
/*    Input:								*/
/*									*/
/*	name = name of file to investigate				*/
/*									*/
/*    Output:								*/
/*									*/
/*	find_file = OZ_SUCCESS : successful				*/
/*	                  else : error status				*/
/*                     							*/
/*	*date_r = zero : file was not found				*/
/*	          else : modification date				*/
/*									*/
/************************************************************************/

static uLong find_file (char *name, OZ_Datebin *date_r)

{
  uLong sts;
  OZ_Handle h_file;
  OZ_IO_fs_getinfo1 fs_getinfo1;
  OZ_IO_fs_open fs_open;

  memset (&fs_open, 0, sizeof fs_open);						/* open the file */
  fs_open.name = name;
  fs_open.lockmode = OZ_LOCKMODE_NL;						/* ... just enough to get its attributes */
  sts = oz_sys_io_fs_open (sizeof fs_open, &fs_open, 0, &h_file);
  if (sts == OZ_NOSUCHFILE) {
    OZ_HW_DATEBIN_CLR (*date_r);						/* not found, return that it is very old */
    return (OZ_SUCCESS);
  }
  if (sts != OZ_SUCCESS) {							/* other error, return error status */
    oz_sys_io_fs_printf (oz_util_h_error, "%s: %s %u: error %u looking up %s\n", pn, make_file, make_line, sts, name);
    return (sts);
  }
  memset (&fs_getinfo1, 0, sizeof fs_getinfo1);					/* get its modification date */
  sts = oz_sys_io (OZ_PROCMODE_KNL, h_file, 0, OZ_IO_FS_GETINFO1, sizeof fs_getinfo1, &fs_getinfo1);
  oz_sys_handle_release (OZ_PROCMODE_KNL, h_file);				/* close file */
  if (sts != OZ_SUCCESS) {
    oz_sys_io_fs_printf (oz_util_h_error, "%s: %s %u: error %u getting date of %s\n", pn, make_file, make_line, sts, name);
    return (sts);
  }
  *date_r = fs_getinfo1.modify_date;						/* return modification date */
  return (OZ_SUCCESS);								/* successful */
}
