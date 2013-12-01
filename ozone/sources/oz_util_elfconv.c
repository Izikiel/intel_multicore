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
/*  Convert an elf object to an oz image				*/
/*  Elf object must have been linked with -d and -r			*/
/*									*/
/*	oz_util_elfconv { -raw <base_addr> | -dynamic | -base <base_addr> }
/*		<oz_image> <elf_object> [<shareables...>]		*/
/*									*/
/*	-raw outputs image without headers, the byte at base_addr is 	*/
/*	the first byte in the output file				*/
/*									*/
/*	-dynamic makes the image relocatable				*/
/*									*/
/*	-base assigns a specific base address				*/
/*									*/
/*	otherwise, image is based at the default per-process address 	*/
/*	space								*/
/*									*/
/************************************************************************/

#include <errno.h>
#include <fcntl.h>
#ifdef _OZONE
#include "oz_crtl_fio.h"
#else
#include <stdio.h>
#endif
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "elf.h"

#include "oz_image.h"

#define MEMORY_PAGE_SIZE (1 << OZ_HW_L2PAGESIZE) /* used for rounding sizes of sections */

typedef struct Global  Global;
typedef struct Refdshr Refdshr;
typedef struct Reloc   Reloc;
typedef struct Section Section;
typedef struct Undef   Undef;

struct Global { Global *next;			/* next in globals list */
                int index;			/* index in elf sh_... struct list */
                int gblent;			/* set if function entrypoint */
                int stbind;			/* STB_LOCAL, STB_GLOBAL, STB_WEAK */
                Section *section;		/* NULL : value is absolute, else : value relative to start of section */
                OZ_Pointer value;		/* symbol's value */
                char *name;			/* symbol's name (points to name in objbuf) */
              };

typedef enum { INDEX_NULL, INDEX_GLOBAL, INDEX_SECTION, INDEX_UNDEF } Indextype;
typedef struct { Indextype type;
                 void *pntr;
               } Index;

struct Refdshr { Refdshr *next;			/* next in refdshrs list */
                 char *name;			/* shareable's name (points to argv[i]) */
                 int index;			/* index in array */
                 int used;			/* 0 : no symbols have been referenced, 1 : symbols have been referenced */
                 int nglobals;			/* number of global symbols in globaldes array */
                 OZ_Image_Globl *globaldes;	/* array of global symbol descriptors */
                 char *globalstr;		/* all the symbol strings (each is null-terminated) */
               };

struct Reloc { Reloc *next;			/* next in relocs list */
               Section *section;		/* section that relocation is done in */
               OZ_Image_Reloc imrel;		/* output image format relocation info */
             };

struct Section { Section *next;			/* next in sections list */
                 int index;			/* index in the elf file of the sh_... struct */
                 OZ_Pointer vaddr;		/* virtual address to map the section at */
                 OZ_Pointer fileoffs;		/* offset in objbuf of data */
                 OZ_Pointer size;		/* number of bytes in section */
                 int writable;			/* section is writable */
                 int nobits;			/* demand-zero section */
               };

struct Undef { Undef *next;			/* next in undefs list */
               int index;			/* index in elf sh_... struct list */
               int undentry;			/* 0: unknown/object; 1: function entrypoint */
               int vecindex;			/* -1: not vectored, else: vector array index */
               Refdshr *refdshr;		/* pointer to shareable that defines it */
               OZ_Pointer value;		/* value relative to base of shareable */
               char *name;			/* symbol's name (points to name in objbuf) */
             };

typedef struct Gotentry Gotentry;

struct Gotentry { Gotentry *next;
                  OZ_Pointer gotofs;
                  OZ_Pointer addend;
                  int indexk;
                };

#if defined (OZ_HW_TYPE_486)
#define MACHINE_TYPE 3
#pragma pack (1)
typedef struct { uByte jmprel32;
                 uLong entrypoint;
                 uByte zeroes[3];
               } Vector;
#pragma nopack
static const Vector vectorfill = { 0xE9, -4, 0, 0, 0 };
#define VEC_RELOC_TYPE OZ_IMAGE_RELOC_UNDEF_NEGPC
static const char magic[8] = { 'o', 'z', '_', 'i', 'm', 'a', 'g', 'e' };
#elif defined (OZ_HW_TYPE_AXP)
#define MACHINE_TYPE 0x9026
typedef struct { uLong ldqr27;	// ldq r27,8(r27)
                 uLong jmpr27;	// jmp (r27)
                 uQuad entrypoint;
               } Vector;
static const Vector vectorfill = { 0xA77B0008, 0x6BFB0000, 0 };
#define VEC_RELOC_TYPE OZ_IMAGE_RELOC_UNDEF
static const char magic[8] = { 'o', 'z', '_', 'i', 'm', 'a', 'x', 'p' };
#else
  error : define vector entry for machine type
#endif

#ifdef OZ_HW_32BIT
#define PTRFMT "%8.8X"
#define PTRSWP(p) (p)
#define DOTSIZE ".long"
#define ELFxx_R_SYM ELF32_R_SYM
#define ELFxx_R_TYPE ELF32_R_TYPE
#endif
#ifdef OZ_HW_64BIT
#define PTRFMT "%8.8X%8.8X"
#define PTRSWP(p) (uLong)((p) >> 32),(uLong)(p)
#define DOTSIZE ".quad"
#define ELFxx_R_SYM(x) ((uLong)((x) >> 32))
#define ELFxx_R_TYPE(x) ((uLong)(x))
#endif

static char *pn = "elfconv";
static int debug;

static Global  *globals  = NULL;
static Refdshr *refdshrs = NULL;
static Reloc   *relocs   = NULL;
static Section *sections = NULL;
static Undef   *undefs   = NULL;

static Gotentry *gotentries = NULL;

static OZ_Pointer hextoptr (char *p, char **r);
static int setindex (Index *indexl, int i, Indextype type, void *pntr);
static void printindex (char *prefix, Indextype type, void *pntr);
static void writeat (int outfd, int pos, int size, void *buff);
static int global_compare (const void *v1, const void *v2);

int main (int argc, char *argv[])

{
  char *arg_image, *arg_reloc, **arg_shares;
  char *elfsymnam, imagename[256], *ozimggblstr, *ozimgrefstr, *ozimgundstr;
  char c, *p, zeroes[MEMORY_PAGE_SIZE];
  uByte *objbuf;
  Global *global, **globall;
  Gotentry *gotentry, **lgotentry;
  Index *indexl;
  int arg_sharec, dynamic, elfstbind, errorflag, flag_raw, have_readonly, have_readwrite;
  int highestindex, i, j, k, ngotentries, nvectors, objfd, objsiz, outfd, rc, shrfd;
  OZ_Image_Globl *ozimggbldes;
  OZ_Image_Headr ozimghdr;
  OZ_Image_Refim *ozimgrefdes;
  OZ_Image_Reloc *ozimgreldes;
  OZ_Image_Sectn *ozimgsecdes;
  OZ_Image_Undef *ozimgunddes;
  OZ_Mempage pages, vpage;
  OZ_Pointer alignment, baseaddr, nextaddr, sectoffs, *valuepnt;
  OZ_Pointer *gotdata;
  Refdshr *refdshr;
  Reloc *reloc;
  Section *gotsection, **lsection, *section, *vecsection;
  struct stat objstat;
  Undef *undef, *undef_baseaddr, *undef_nextaddr;
  Vector *vectors;

#ifdef OZ_HW_32BIT
  Elf32_Ehdr *elfhdrpnt;
  Elf32_Rel  *elfrel;
  Elf32_Rela *elfrela;
  Elf32_Shdr *elfshpnt;
  Elf32_Sym  *elfsym;
#endif
#ifdef OZ_HW_64BIT
  Elf64_Ehdr *elfhdrpnt;
  Elf64_Rel  *elfrel;
  Elf64_Rela *elfrela;
  Elf64_Shdr *elfshpnt;
  Elf64_Sym  *elfsym;
#endif

  if (argc > 0) pn = argv[0];

  arg_image  = NULL;
  arg_reloc  = NULL;
  arg_sharec = 0;
  arg_shares = NULL;
  flag_raw   = 0;
  dynamic    = 0;
  baseaddr   = OZ_HW_PROCLINKBASE;
  debug      = 0;

  for (i = 1; i < argc; i ++) {

    /* -base <base_address> */

    if (strcmp (argv[i], "-base") == 0) {
      if (++ i >= argc) goto usage;
      baseaddr = hextoptr (argv[i], &p);
      if (*p != 0) {
        fprintf (stderr, "%s: bad base address %s\n", pn, argv[i]);
        return (11);
      }
      dynamic  = 0;
      flag_raw = 0;
      continue;
    }

    /* -debug */

    if (strcmp (argv[i], "-debug") == 0) {
      debug = 1;
      continue;
    }

    /* -dynamic */

    if (strcmp (argv[i], "-dynamic") == 0) {
      dynamic  = 1;
      flag_raw = 0;
      baseaddr = 0;
      continue;
    }

    /* -raw <base_address> */

    if (strcmp (argv[i], "-raw") == 0) {
      if (++ i >= argc) goto usage;
      baseaddr = hextoptr (argv[i], &p);
      if (*p != 0) {
        fprintf (stderr, "%s: bad base address %s\n", pn, argv[i]);
        return (12);
      }
      dynamic  = 0;
      flag_raw = 1;
      continue;
    }

    /* those are the only options we do */

    if (argv[i][0] == '-') {
      fprintf (stderr, "%s: unknown option %s\n", pn, argv[i]);
      goto usage;
    }

    /* next is image name */

    if (arg_image == NULL) {
      arg_image = argv[i];
      continue;
    }

    /* next is object name */

    if (arg_reloc == NULL) {
      arg_reloc = argv[i];
      continue;
    }

    /* the rest are the shareables */

    if (arg_shares == NULL) {
      arg_sharec = argc - i;
      arg_shares = argv + i;
      break;
    }
  }

  if (debug) fprintf (stderr, "#1\n");

  if (arg_reloc == NULL) {
    fprintf (stderr, "%s: no relocatable file specified\n", pn);
    goto usage;
  }

  if (flag_raw) {
    if (arg_shares != NULL) {
      fprintf (stderr, "%s: -raw is incompatible with having shareables\n", pn);
      return (13);
    }
    if (baseaddr == 0) {
      fprintf (stderr, "%s: a non-zero base address must be given with -raw\n", pn);
      return (14);
    }
  }

  /* Read elf object file into memory */

  if (debug) fprintf (stderr, "#2\n");

  objfd = open (arg_reloc, O_RDONLY);
  if (objfd < 0) {
    fprintf (stderr, "%s: error opening input object file %s: %s\n", pn, arg_reloc, strerror (errno));
    return (15);
  }
  if (fstat (objfd, &objstat) < 0) {
    fprintf (stderr, "%s: error statting input object file %s: %s\n", pn, arg_reloc, strerror (errno));
    return (16);
  }
  objsiz = objstat.st_size;
  objbuf = malloc (objsiz);
  rc = read (objfd, objbuf, objsiz);
  if (rc < 0) {
    fprintf (stderr, "%s: error reading input object file %s: %s\n", pn, arg_reloc, strerror (errno));
    return (17);
  }
  if (rc != objsiz) {
    fprintf (stderr, "%s: only read %d of %u bytes from input object file %s: %s\n", pn, rc, objsiz, arg_reloc);
    return (18);
  }
  close (objfd);

  elfhdrpnt = (void *)objbuf;
  if ((elfhdrpnt -> e_ident[0] != 127) 
   || (elfhdrpnt -> e_ident[1] != 'E') 
   || (elfhdrpnt -> e_ident[2] != 'L') 
   || (elfhdrpnt -> e_ident[3] != 'F') 
   || (elfhdrpnt -> e_type != ET_REL)) {
    fprintf (stderr, "%s: input file %s is not an elf relocatable\n", pn, arg_reloc);
    return (19);
  }
  if (elfhdrpnt -> e_machine != MACHINE_TYPE) {
    fprintf (stderr, "%s: build for machine type %u, but object is %u\n", pn, MACHINE_TYPE, elfhdrpnt -> e_machine);
    return (19);
  }

  /* Build list of referenced shareables from command line */

  if (debug) fprintf (stderr, "#3\n");

  refdshrs = NULL;
  for (i = 0; i < arg_sharec; i ++) {
    refdshr = malloc (sizeof *refdshr);								/* allocate a block of memory */
    refdshr -> name = arg_shares[i];
#ifdef _OZONE
    strcpy (imagename, "OZ_IMAGE_DIR:");
    strcat (imagename, arg_shares[i]);
#else
    strcpy (imagename, arg_shares[i]);
#endif
    if (debug) fprintf (stderr, "#3.1 %s\n", imagename);
    shrfd = open (imagename, O_RDONLY);								/* open the shareable */
    if (debug) fprintf (stderr, "#3.2 %d\n", shrfd);
    if (shrfd < 0) {
      fprintf (stderr, "%s: error opening %s: %s\n", pn, imagename, strerror (errno));
      return (20);
    }
    rc = read (shrfd, &ozimghdr, sizeof ozimghdr);						/* read fixed portion of header */
    if (debug) fprintf (stderr, "#3.3 %d\n", rc);
    if (rc < 0) {
      fprintf (stderr, "%s: error reading %s: %s\n", pn, imagename, strerror (errno));
      return (21);
    }
    if (rc != sizeof ozimghdr) {								/* make sure we got it all */
      fprintf (stderr, "%s: only read %d of %d bytes of image header in %s\n", pn, rc, sizeof ozimghdr, imagename);
      return (22);
    }
    if (memcmp (ozimghdr.magic, magic, sizeof ozimghdr.magic) != 0) {
      fprintf (stderr, "%s: %s is not an oz_image\n", pn, imagename);
      return (23);
    }
    memset (ozimghdr.fxhdrsize + (uByte *)&ozimghdr, 0, sizeof ozimghdr - ozimghdr.fxhdrsize);	/* null fill it */
    refdshr -> nglobals  = ozimghdr.gbldesnum;							/* number of elements in refdshr -> globaldes */
    j = ozimghdr.gbldesnum * sizeof *(refdshr -> globaldes);					/* allocate a block for global symbol descriptors */
    refdshr -> globaldes = malloc (j);
    refdshr -> globalstr = malloc (ozimghdr.gblstrsiz);						/* allocate a block for global symbol strings */
    if (debug) fprintf (stderr, "#3.4\n");
    if (lseek (shrfd, ozimghdr.gbldesoff, SEEK_SET) < 0) {					/* seek file to the global symbol descriptors */
      fprintf (stderr, "%s: error positioning %s to %u: %s\n", pn, imagename, ozimghdr.gbldesoff, strerror (errno));
      return (24);
    }
    if (debug) fprintf (stderr, "#3.5\n");
    rc = read (shrfd, refdshr -> globaldes, j);							/* read in the global symbol table descriptors */
    if (debug) fprintf (stderr, "#3.6 %d\n", rc);
    if (rc < 0) {
      fprintf (stderr, "%s: error reading global symbols from %s: %s\n", pn, imagename, strerror (errno));
      return (25);
    }
    if (rc < ozimghdr.gbldesnum * sizeof *(refdshr -> globaldes)) {				/* make sure we got it all */
      fprintf (stderr, "%s: only read %d of %d bytes from %s\n", pn, rc, ozimghdr.gbldesnum * sizeof *(refdshr -> globaldes), imagename);
      return (26);
    }
    if (lseek (shrfd, ozimghdr.gblstroff, SEEK_SET) < 0) {					/* seek file to the global symbol descriptors */
      fprintf (stderr, "%s: error positioning %s to %u: %s\n", pn, imagename, ozimghdr.gblstroff, strerror (errno));
      return (27);
    }
    if (debug) fprintf (stderr, "#3.7\n");
    rc = read (shrfd, refdshr -> globalstr, ozimghdr.gblstrsiz);				/* read in the global symbol table strings */
    if (debug) fprintf (stderr, "#3.8 %d\n", rc);
    if (rc < 0) {
      fprintf (stderr, "%s: error reading global symbols from %s: %s\n", pn, imagename, strerror (errno));
      return (28);
    }
    if (rc < ozimghdr.gblstrsiz) {								/* make sure we got it all */
      fprintf (stderr, "%s: only read %d of %d bytes from %s\n", pn, rc, ozimghdr.gblstrsiz, imagename);
      return (29);
    }
    close (shrfd);										/* close shareable image */
    if (debug) fprintf (stderr, "#3.9\n");
    refdshr -> next = refdshrs;									/* link shareable image to list */
    refdshr -> used = 0;									/* hasn't actually been referenced yet */
    refdshrs = refdshr;
  }

  if (debug) fprintf (stderr, "#4\n");

  /* Create two undefined symbols giving the base and next available address */

  undef = malloc (sizeof *undef);
  undef -> index   = -1;
  undef -> refdshr = NULL;
  undef -> name    = "OZ_IMAGE_NEXTADDR";
  undef_nextaddr   = undef;

  undef = malloc (sizeof *undef);
  undef -> index   = -1;
  undef -> refdshr = NULL;
  undef -> name    = "OZ_IMAGE_BASEADDR";
  undef_baseaddr   = undef;

  if (debug) fprintf (stderr, "#5\n");

  /* Build lists of stuff from the input object file */

  highestindex = 0;

  globals  = NULL;
  relocs   = NULL;
  undefs   = NULL;
  sections = NULL;
  nvectors = 0;

  errorflag = 0;

  for (i = 0; i < elfhdrpnt -> e_shnum; i ++) {
    elfshpnt = (void *)(objbuf + elfhdrpnt -> e_shoff + i * elfhdrpnt -> e_shentsize);

    /* Memory sections */

    if (((elfshpnt -> sh_type == SHT_PROGBITS) || (elfshpnt -> sh_type == SHT_NOBITS)) && (elfshpnt -> sh_flags & SHF_ALLOC) && (elfshpnt -> sh_size != 0)) {
      if (elfshpnt -> sh_addr != 0) {
        fprintf (stderr, "%s: input section %d has address " PTRFMT "\n", pn, i, PTRSWP (elfshpnt -> sh_addr));
        errorflag = 1;
        continue;
      }
      if (i >= highestindex) highestindex = i + 1;
      for (lsection = &sections; (section = *lsection) != NULL; lsection = &(section -> next)) {}
      section = malloc (sizeof *section);
      section -> next     = NULL;
      section -> index    = i;
      section -> vaddr    = 0;
      section -> fileoffs = elfshpnt -> sh_offset;
      section -> size     = elfshpnt -> sh_size;
      section -> writable = ((elfshpnt -> sh_flags & SHF_WRITE) != 0);
      section -> nobits   = (elfshpnt -> sh_type == SHT_NOBITS);
      *lsection = section;
    }

    /* Symbol table */

    if ((elfshpnt[0].sh_type == SHT_SYMTAB) && (elfshpnt[1].sh_type == SHT_STRTAB)) {
      for (j = 0; j < elfshpnt[0].sh_size / elfshpnt[0].sh_entsize; j ++) {			/* loop through each symbol */
        elfsym = (void *)(objbuf + elfshpnt[0].sh_offset + j * elfshpnt[0].sh_entsize);		/* point to elf symbol table entry */
        elfsymnam = objbuf + elfshpnt[1].sh_offset + elfsym -> st_name;				/* point to symbol name string */
        if (elfsymnam[0] == 0) continue;
        elfstbind = ELF_ST_BIND (elfsym -> st_info);
        if ((Word)(elfsym -> st_shndx) < 0) {							/* check for negative sh index */
          if (j >= highestindex) highestindex = j + 1;
          global = malloc (sizeof *global);							/* if so, it is an absolute symbol value definition */
          global -> next    = globals;
          global -> index   = j;
          global -> gblent  = (ELF32_ST_TYPE (elfsym -> st_info) == STT_FUNC);
          global -> section = NULL;
          global -> value   = elfsym -> st_value;
          global -> name    = elfsymnam;
          global -> stbind  = elfstbind;
          globals = global;
        } else if (elfsym -> st_shndx == 0) {							/* check for zero sh index */
          if (strcmp (elfsymnam, undef_baseaddr -> name) == 0) {
            if (j >= highestindex) highestindex = j + 1;
            undef_baseaddr -> index = j;
            continue;
          }
          if (strcmp (elfsymnam, undef_nextaddr -> name) == 0) {
            if (j >= highestindex) highestindex = j + 1;
            undef_nextaddr -> index = j;
            continue;
          }
          for (refdshr = refdshrs; refdshr != NULL; refdshr = refdshr -> next) {		/* if so, it is undefined, scan the referenced shareable list */
            for (k = 0; k < refdshr -> nglobals; k ++) {
              if (strcmp (elfsymnam, refdshr -> globalstr + refdshr -> globaldes[k].globl_namof) == 0) goto got_undef;
            }
          }
          fprintf (stderr, "%s: undefined symbol %s\n", pn, elfsymnam);				/* can't find it at all, barf */
          errorflag = 1;
          continue;
got_undef:;
          refdshr -> used = 1;									/* found it, mark that shareable as being used */
          if (j >= highestindex) highestindex = j + 1;
          undef = malloc (sizeof *undef);							/* put undefined symbol on list of undefined symbols */
          undef -> next     = undefs;
          undef -> index    = j;
          undef -> refdshr  = refdshr;
          undef -> value    = refdshr -> globaldes[k].globl_value;
          undef -> name     = elfsymnam;
          undef -> undentry = ((refdshr -> globaldes[k].globl_flags & OZ_IMAGE_GLOBL_FLAG_ENTRY) != 0);
          undef -> vecindex = 0;
          if (undef -> undentry) undef -> vecindex = nvectors ++;
          undefs = undef;
        } else {
          for (section = sections; section != NULL; section = section -> next) {		/* section index > 0, find it in list */
            if (section -> index == (Word)(elfsym -> st_shndx)) break;
          }
          if (section == NULL) {
            fprintf (stderr, "%s: bad section index %d for symbol %s\n", pn, elfsym -> st_shndx, elfsymnam); /* not found, barf */
            errorflag = 1;
            continue;
          }
          if (j >= highestindex) highestindex = j + 1;
          global = malloc (sizeof *global);							/* ok, put relocatable global on list */
          global -> next    = globals;
          global -> index   = j;
          global -> gblent  = (ELF32_ST_TYPE (elfsym -> st_info) == STT_FUNC);
          global -> section = section;
          global -> value   = elfsym -> st_value;
          global -> name    = elfsymnam;
          global -> stbind  = elfstbind;
          globals = global;
        }
      }
    }
  }

  if (debug) fprintf (stderr, "#6\n");

  /* Scan the relocation entries for the file and create a global offset table */

  gotentries = NULL;
  ngotentries = 0;

  for (i = 0; i < elfhdrpnt -> e_shnum; i ++) {
    elfshpnt = (void *)(objbuf + elfhdrpnt -> e_shoff + i * elfhdrpnt -> e_shentsize);

    for (section = sections; section != NULL; section = section -> next) {			/* find memory section the relocation table applies to */
      if (section -> index == elfshpnt -> sh_info) break;
    }
    if (section == NULL) continue;								/* sometimes there are bogus sections we don't care about */
												/* ... so we don't care about their relocations either */
    switch (elfshpnt -> sh_type) {
      case SHT_RELA: {
        for (j = 0; j < elfshpnt -> sh_size / elfshpnt -> sh_entsize; j ++) {			/* loop through each relocation */
          elfrela = (void *)(objbuf + elfshpnt -> sh_offset + j * elfshpnt -> sh_entsize);	/* point to relocation */
          sectoffs = elfrela -> r_offset;							/* offset of relocation in section */
          valuepnt = (OZ_Pointer *)(objbuf + section -> fileoffs + sectoffs);			/* point to value to be relocated */
          k = ELFxx_R_SYM (elfrela -> r_info);							/* get index of what to relocate by */
          switch (ELFxx_R_TYPE (elfrela -> r_info)) {						/* decode relocation type */
#if MACHINE_TYPE == 0x9026
            case 4: {										/* LITERAL: GP relative 16 bit w/optimization */
              for (lgotentry = &gotentries; (gotentry = *lgotentry) != NULL; lgotentry = &(gotentry -> next)) {
                if ((gotentry -> indexk == k) && (gotentry -> addend == elfrela -> r_addend)) break;
              }
              if (gotentry == NULL) {
                gotentry = malloc (sizeof *gotentry);
                gotentry -> next   = NULL;
                gotentry -> gotofs = ngotentries * sizeof *gotdata;
                gotentry -> indexk = k;
                gotentry -> addend = elfrela -> r_addend;
                *lgotentry = gotentry;
                lgotentry = &(gotentry -> next);
                ngotentries ++;
              }
              break;
            }
#endif
          }
        }
        break;
      }
    }
  }

  gotsection = NULL;
  if (ngotentries > 0) {

    /* AXP can only handle +/- 32K offset in table */

    if (ngotentries * sizeof *gotdata > 65536) {
      fprintf (stderr, "%s: too many global offset table entries (%d)\n", pn, ngotentries);
      errorflag = 1;
    }

    /* Make a read-only section for it */

    for (lsection = &sections; (section = *lsection) != NULL; lsection = &(section -> next)) {}
    gotsection = malloc (sizeof *gotsection);
    gotsection -> next     = NULL;
    gotsection -> index    = highestindex ++;
    gotsection -> vaddr    = 0;
    gotsection -> fileoffs = 0;
    gotsection -> size     = ngotentries * sizeof *gotdata;
    gotsection -> writable = 0;
    gotsection -> nobits   = 0;
    *lsection = gotsection;

    /* Allocate memory for the entries and null fill it */

    gotdata = malloc (ngotentries * sizeof *gotdata);
    memset (gotdata, 0, ngotentries * sizeof *gotdata);
  }

  /* If there are any undefined symbols referenced that are functions, create a vector section for them */

  vecsection = NULL;
  if (nvectors > 0) {

    /* Make a read-only section for it */

    for (lsection = &sections; (section = *lsection) != NULL; lsection = &(section -> next)) {}
    vecsection = malloc (sizeof *vecsection);
    vecsection -> next     = NULL;
    vecsection -> index    = highestindex ++;
    vecsection -> vaddr    = 0;
    vecsection -> fileoffs = 0;
    vecsection -> size     = nvectors * sizeof *vectors;
    vecsection -> writable = 0;
    vecsection -> nobits   = 0;
    *lsection = vecsection;

    /* Allocate memory for the vectors and null fill it */

    vectors = malloc (nvectors * sizeof *vectors);
    for (i = 0; i < nvectors; i ++) vectors[i] = vectorfill;
  }

  /* Assign memory addresses to sections (including got and vector sections, if any) */

  nextaddr = baseaddr;
  for (i = 0; i < 2; i ++) {							/* do read-only, then read/write sections */
    nextaddr = (nextaddr + MEMORY_PAGE_SIZE - 1) & -MEMORY_PAGE_SIZE;		/* page-align for section type */
    for (section = sections; section != NULL; section = section -> next) {	/* scan for all matching sections */
      if (section -> writable == i) {
        if (section == vecsection) alignment = sizeof *vectors;
        else if (section == gotsection) alignment = sizeof *gotdata;
        else {
          elfshpnt  = (void *)(objbuf + elfhdrpnt -> e_shoff + section -> index * elfhdrpnt -> e_shentsize);
          alignment = elfshpnt -> sh_addralign;
        }
        nextaddr = ((nextaddr + alignment - 1) / alignment) * alignment;	/* align virtual address */
        section -> vaddr = nextaddr;						/* assign virtual address */
        nextaddr += section -> size;						/* increment for next section */
      }
    }
  }
  nextaddr = (nextaddr + MEMORY_PAGE_SIZE - 1) & -MEMORY_PAGE_SIZE;		/* page-align */

  if (debug) fprintf (stderr, "#7\n");

  /* If they were referenced, fill in values for base and next address symbols */

  if (undef_nextaddr -> index >= 0) {
    undef_nextaddr -> next  = undefs;
    undef_nextaddr -> value = nextaddr;
    undefs = undef_nextaddr;
  }

  if (undef_baseaddr -> index >= 0) {
    undef_baseaddr -> next  = undefs;
    undef_baseaddr -> value = baseaddr;
    undefs = undef_baseaddr;
  }

  if (debug) fprintf (stderr, "#8\n");

  /* Fill in index list from sections, globals, undefines, and check for duplicate use */

  indexl = malloc (highestindex * sizeof *indexl);
  memset (indexl, 0, highestindex * sizeof *indexl);

  for (global = globals; global != NULL; global = global -> next) {
    errorflag |= setindex (indexl, global -> index, INDEX_GLOBAL, global);
  }

  for (section = sections; section != NULL; section = section -> next) {
    errorflag |= setindex (indexl, section -> index, INDEX_SECTION, section);
  }

  for (undef = undefs; undef != NULL; undef = undef -> next) {
    errorflag |= setindex (indexl, undef -> index, INDEX_UNDEF, undef);
  }

  if (debug) fprintf (stderr, "#9\n");

  /* Print out global offset table */

#if 0000
  for (gotentry = gotentries; gotentry != NULL; gotentry = gotentry -> next) {
    OZ_Pointer value;

    value = gotsection -> vaddr + gotentry -> gotofs;
    fprintf (stderr, "*: got+%4.4X " PTRFMT ": ", (unsigned int)(gotentry -> gotofs), PTRSWP (value));
    k = gotentry -> indexk;
    switch (indexl[k].type) {
      case INDEX_NULL: {
        fprintf (stderr, ".quad " PTRFMT "\n", PTRSWP (gotentry -> addend));
        break;
      }
      case INDEX_GLOBAL: {
        global = indexl[k].pntr;
        value  = global -> value;
        if (global -> section != NULL) value += global -> section -> vaddr;
        fprintf (stderr, ".%s " PTRFMT "(%s)+" PTRFMT "\n", 
		(dynamic && (global -> section == NULL)) ? "address" : "quad", 
		PTRSWP (value), global -> name, PTRSWP (gotentry -> addend));
        break;
      }
      case INDEX_SECTION: {
        section = indexl[k].pntr;
        fprintf (stderr, ".%s " PTRFMT "+" PTRFMT "\n", 
		dynamic ? "address" : "quad", 
		PTRSWP (section -> vaddr), PTRSWP (gotentry -> addend));
        break;
      }
      case INDEX_UNDEF: {
        undef = indexl[k].pntr;
        if (undef -> refdshr == NULL) {
          fprintf (stderr, ".quad " PTRFMT "(%s)+" PTRFMT "\n", 
		PTRSWP (undef -> value), undef -> name, PTRSWP (gotentry -> addend));
        } else if (undef -> undentry) {
          value = vecsection -> vaddr + undef -> vecindex * sizeof *vectors;
          fprintf (stderr, ".%s " PTRFMT "(@%s)+" PTRFMT "\n", 
		dynamic ? "address" : "quad", PTRSWP (value), undef -> name, PTRSWP (gotentry -> addend));
        } else {
          fprintf (stderr, ".quad %s+" PTRFMT "\n", undef -> name, PTRSWP (gotentry -> addend));
        }
        break;
      }
      default: {
        fprintf (stderr, "??\n");
      }
    }
  }
#endif

  /* Make global symbols relative to beginning of image, not each section */
  /* This way, there are fewer relocations to be done at load time        */

  for (global = globals; global != NULL; global = global -> next) {
    section = global -> section;
    if (section == NULL) continue;					/* leave symbols that have no section alone */
    global -> value += section -> vaddr;				/* ok, add section base to symbol value */
    if (!dynamic) global -> section = NULL;				/* if not dynamic image, the symbol is now at its final resting place */
  }

  if (debug) fprintf (stderr, "#10\n");

  /* Process relocations after everything else is in place */

  for (i = 0; i < elfhdrpnt -> e_shnum; i ++) {
    elfshpnt = (void *)(objbuf + elfhdrpnt -> e_shoff + i * elfhdrpnt -> e_shentsize);

    for (section = sections; section != NULL; section = section -> next) {			/* find memory section the relocation table applies to */
      if (section -> index == elfshpnt -> sh_info) break;
    }
    if (section == NULL) continue;								/* sometimes there are bogus sections we don't care about */
												/* ... so we don't care about their relocations either */
    switch (elfshpnt -> sh_type) {
      case SHT_RELA: {
        for (j = 0; j < elfshpnt -> sh_size / elfshpnt -> sh_entsize; j ++) {			/* loop through each relocation */
          elfrela = (void *)(objbuf + elfshpnt -> sh_offset + j * elfshpnt -> sh_entsize);	/* point to relocation */
          sectoffs = elfrela -> r_offset;							/* offset of relocation in section */
          valuepnt = (OZ_Pointer *)(objbuf + section -> fileoffs + sectoffs);			/* point to value to be relocated */
          k = ELFxx_R_SYM (elfrela -> r_info);							/* get index of what to relocate by */
          reloc = malloc (sizeof *reloc);
          reloc -> imrel.reloc_vaddr = sectoffs + section -> vaddr;				/* output image address of data to be relocated */
          switch (ELFxx_R_TYPE (elfrela -> r_info)) {						/* decode relocation type */
#if MACHINE_TYPE == 0x9026
            case 2: {										/* .QUAD symbol */
              switch (indexl[k].type) {
                case INDEX_NULL: {
                  *valuepnt = elfrela -> r_addend;
                  goto skip_rela;
                }
                case INDEX_GLOBAL: {
                  global = indexl[k].pntr;
                  *valuepnt = elfrela -> r_addend + global -> value;				/* - its a global defined by this image */
                  if (global -> section == NULL) goto skip_rela;				/* - if absolute value, no more relocing */
                  reloc -> imrel.reloc_type = OZ_IMAGE_RELOC_IMGBASE;				/* - relocate by image base */
                  break;
                }
                case INDEX_SECTION: {
                  *valuepnt = elfrela -> r_addend + ((Section *)(indexl[k].pntr)) -> vaddr;
                  if (!dynamic) goto skip_rela;
                  reloc -> imrel.reloc_type = OZ_IMAGE_RELOC_IMGBASE;				/* - relocate by image base */
                  break;
                }
                case INDEX_UNDEF: {
                  undef = (Undef *)(indexl[k].pntr);
                  if (undef -> refdshr == NULL) {
                    *valuepnt = elfrela -> r_addend + undef -> value;				/* - OZ_IMAGE_BASEADDR or _NEXTADDR */
                    if (!dynamic) goto skip_rela;
                    reloc -> imrel.reloc_type = OZ_IMAGE_RELOC_IMGBASE;
                    break;
                  }
                  if (undef -> undentry) {							/* - see if vectored */
                    *valuepnt = elfrela -> r_addend + vecsection -> vaddr + undef -> vecindex * sizeof *vectors; /* - vectored, point to vector table entry */
                    if (!dynamic) goto skip_rela;
                    reloc -> imrel.reloc_type = OZ_IMAGE_RELOC_IMGBASE;				/*   relocated by image base */
                  } else {
                    reloc -> imrel.reloc_type = OZ_IMAGE_RELOC_UNDEF;				/* - not vectored, relocate by value of undefined symbol */
                    reloc -> imrel.reloc_indx = undef -> index;
                  }
                  break;
                }
              }
              break;
            }
            case 3: {										/* GPREL32: .LONG value-gotvaddr */
              switch (indexl[k].type) {
                case INDEX_NULL: {
                  fprintf (stderr, "%s: GPREL32 on absolute\n");
                  errorflag = 1;
                  goto skip_rela;
                }
                case INDEX_GLOBAL: {
                  global = indexl[k].pntr;
                  *(Long *)valuepnt = elfrela -> r_addend + global -> value - gotsection -> vaddr - 0x8000; /* - its a global defined by this image */
                  if (global -> section == NULL) {
                    fprintf (stderr, "%s: GPREL32 on absolute global\n");
                    errorflag = 1;
                  }
                  goto skip_rela;
                }
                case INDEX_SECTION: {
                  *(Long *)valuepnt = elfrela -> r_addend + ((Section *)(indexl[k].pntr)) -> vaddr - gotsection -> vaddr - 0x8000;
                  goto skip_rela;
                }
                case INDEX_UNDEF: {
                  undef = (Undef *)(indexl[k].pntr);
                  if (undef -> refdshr == NULL) {
                    *(Long *)valuepnt = elfrela -> r_addend + undef -> value - gotsection -> vaddr - 0x8000; /* - OZ_IMAGE_BASEADDR or _NEXTADDR */
                    if (!dynamic) goto skip_rela;
                    reloc -> imrel.reloc_type = OZ_IMAGE_RELOC_IMGBASE;
                    break;
                  }
                  fprintf (stderr, "%s: GPREL32 on undefined symbol\n");
                  errorflag = 1;
                  goto skip_rela;
                }
              }
              break;
            }
            case 4: {										/* LITERAL: GP relative 16 bit w/optimization */
              for (gotentry = gotentries; gotentry != NULL; gotentry = gotentry -> next) {	/* find corresponding entry in gotentry list */
                if ((gotentry -> indexk == k) && (gotentry -> addend == elfrela -> r_addend)) break;
              }
              if (gotentry == NULL) {
                fprintf (stderr, "%s: skipped %u.%u LITERAL at " PTRFMT "\n", pn, i, j, PTRSWP (reloc -> imrel.reloc_vaddr));
                errorflag = 1;
              } else {
                *(Word *)valuepnt = gotentry -> gotofs - 0x8000;				/* store displacement in instruction */
              }
              goto skip_rela;
            }
            case 6: {										/* GPDISP: LDAH/LDA pair for global offset table */
              Quad chk_disp, got_disp;
              Word lda_disp, ldah_disp;

              if (elfrela -> r_addend != 4) {							// LDA must be 4 bytes after LDAH
                fprintf (stderr, "%s: gpdisp ldah->lda offset %u\n", elfrela -> r_addend);
                errorflag = 1;
                goto skip_rela;
              }
              if ((*valuepnt & 0xFC000000FC000000ULL) != 0x2000000024000000ULL) {		// check the opcodes for LDAH/LDA
                fprintf (stderr, "%s: not LDAH/LDA pair\n", pn);
                errorflag = 1;
                goto skip_rela;
              }
              got_disp   = gotsection -> vaddr + 0x8000 - reloc -> imrel.reloc_vaddr;		// displacement to the global offset table
              lda_disp   = got_disp;								// split it up for LDA and LDAH instructions
              ldah_disp  = (got_disp - (Quad)lda_disp) >> 16;
              chk_disp   = (Quad)ldah_disp;							// put back together like the CPU does
              chk_disp <<= 16;
              chk_disp  += (Quad)lda_disp;
              if (chk_disp != got_disp) {							// make sure it didn't overflow
                fprintf (stderr, "%s: got_disp " PTRFMT " too large\n", pn, PTRSWP (got_disp));
                errorflag = 1;
                goto skip_rela;
              }
              ((Word *)valuepnt)[0] = ldah_disp;						// store the displacements
              ((Word *)valuepnt)[2] = lda_disp;
              goto skip_rela;									// it's all resolved now
            }
            case 7: {										/* branch displacement */
              uLong brdest;

              switch (indexl[k].type) {
                case INDEX_NULL: {
                  if (dynamic) {
                    fprintf (stderr, "%s: branch displacement to absolute value at " PTRFMT "\n", pn, PTRSWP (reloc -> imrel.reloc_vaddr));
                    errorflag = 1;
                    goto skip_rela;
                  }
                  brdest = elfrela -> r_addend;
                  break;
                }
                case INDEX_GLOBAL: {
                  global = indexl[k].pntr;
                  if (dynamic && (global -> section == NULL)) {
                    fprintf (stderr, "%s: branch displacement to absolute global at " PTRFMT "\n", pn, PTRSWP (reloc -> imrel.reloc_vaddr));
                    errorflag = 1;
                    goto skip_rela;
                  }
                  brdest = elfrela -> r_addend + global -> value;
                  break;
                }
                case INDEX_SECTION: {
                  brdest = elfrela -> r_addend + ((Section *)(indexl[k].pntr)) -> vaddr;
                  break;
                }
                case INDEX_UNDEF: {
                  undef = (Undef *)(indexl[k].pntr);
                  if (undef -> refdshr == NULL) {
                    brdest = elfrela -> r_addend + undef -> value;				/* - OZ_IMAGE_BASEADDR or _NEXTADDR */
                  } else {
                    fprintf (stderr, "%s: branch displacement to undefined at " PTRFMT "\n", pn, PTRSWP (reloc -> imrel.reloc_vaddr));
                    errorflag = 1;
                    goto skip_rela;
                  }
                  break;
                }
              }
              brdest -= reloc -> imrel.reloc_vaddr + 4;						// make it relative to next instruction
              if (brdest & 3) {									// has to be longword aligned
                fprintf (stderr, "%s: branch displacement %X not multiple of 4 at " PTRFMT "\n", pn, brdest, PTRSWP (reloc -> imrel.reloc_vaddr));
                errorflag = 1;
              } else if ((brdest >= 0x00400000) && (brdest < 0xFFC00000)) {			// can't be more than 21+2 bits signed
                fprintf (stderr, "%s: branch displacement %X too far at " PTRFMT "\n", pn, brdest, PTRSWP (reloc -> imrel.reloc_vaddr));
                errorflag = 1;
              } else {
                *(uLong *)valuepnt = (*(uLong *)valuepnt & 0xFFE00000) | ((brdest & 0x007FFFFF) >> 2); // merge with branch opcode
              }
              goto skip_rela;
            }
            case 5:
            case 8: {										/* hints, safely ignored */
              goto skip_rela;
            }
#endif
            default: {
              fprintf (stderr, "%s: unknown relocation %u.%u type %u at " PTRFMT "\n", pn, i, j, ELFxx_R_TYPE (elfrela -> r_info), PTRSWP (reloc -> imrel.reloc_vaddr));
              errorflag = 1;
              goto skip_rela;
            }
          }
          reloc -> next = relocs;
          relocs = reloc;
          continue;
skip_rela:
          free (reloc);
        }
        break;
      }
      case SHT_REL: {
        for (j = 0; j < elfshpnt -> sh_size / elfshpnt -> sh_entsize; j ++) {			/* loop through each relocation */
          elfrel = (void *)(objbuf + elfshpnt -> sh_offset + j * elfshpnt -> sh_entsize);	/* point to relocation */
          sectoffs = elfrel -> r_offset;							/* offset of relocation in section */
          valuepnt = (OZ_Pointer *)(objbuf + section -> fileoffs + sectoffs);			/* point to value to be relocated */
          k = ELFxx_R_SYM (elfrel -> r_info);							/* get index of what to relocate by */
          reloc = malloc (sizeof *reloc);
          reloc -> imrel.reloc_vaddr = sectoffs + section -> vaddr;				/* output image address of data to be relocated */
          switch (ELFxx_R_TYPE (elfrel -> r_info)) {						/* decode relocation type */
#if MACHINE_TYPE == 3
            case 1: {										/* .LONG SYMBOL */
              switch (indexl[k].type) {
                case INDEX_NULL: {
                  goto skip_rel;
                }
                case INDEX_GLOBAL: {
                  global = indexl[k].pntr;
                  *valuepnt += global -> value;							/* - its a global defined by this image */
                  if (global -> section == NULL) goto skip_rel;					/* - if absolute value, no more relocing */
                  reloc -> imrel.reloc_type = OZ_IMAGE_RELOC_IMGBASE;				/* - relocate by image base */
                  break;
                }
                case INDEX_SECTION: {
                  *valuepnt += ((Section *)(indexl[k].pntr)) -> vaddr;
                  if (!dynamic) goto skip_rel;
                  reloc -> imrel.reloc_type = OZ_IMAGE_RELOC_IMGBASE;				/* - relocate by image base */
                  break;
                }
                case INDEX_UNDEF: {
                  undef = (Undef *)(indexl[k].pntr);
                  if (undef -> refdshr == NULL) {
                    *valuepnt += undef -> value;						/* - OZ_IMAGE_BASEADDR or _NEXTADDR */
                    if (!dynamic) goto skip_rel;
                    reloc -> imrel.reloc_type = OZ_IMAGE_RELOC_IMGBASE;
                    break;
                  }
                  if (undef -> undentry) {							/* - see if vectored */
                    *valuepnt += vecsection -> vaddr + undef -> vecindex * sizeof *vectors;	/* - vectored, point to vector table entry */
                    if (!dynamic) goto skip_rel;
                    reloc -> imrel.reloc_type = OZ_IMAGE_RELOC_IMGBASE;				/*   relocated by image base */
                  } else {
                    reloc -> imrel.reloc_type = OZ_IMAGE_RELOC_UNDEF;				/* - not vectored, relocate by value of undefined symbol */
                    reloc -> imrel.reloc_indx = undef -> index;
                  }
                  break;
                }
              }
              break;
            }
            case 2: {										/* .LONG SYMBOL-. */
              switch (indexl[k].type) {
                case INDEX_NULL: {
                  *valuepnt -= section -> vaddr + sectoffs;
                  goto skip_rel;
                }
                case INDEX_GLOBAL: {
                  global = indexl[k].pntr;
                  *valuepnt -= section -> vaddr + sectoffs;
                  *valuepnt += global -> value;							/* - its a global defined by this image */
                  if ((global -> section != NULL) || (baseaddr != 0)) goto skip_rel;		/* - if relative value, no more relocing */
                  reloc -> imrel.reloc_type = OZ_IMAGE_RELOC_IMGBASE_NEGPC;			/* - relocate by negative image base */
                  break;
                }
                case INDEX_SECTION: {
                  *valuepnt -= section -> vaddr + sectoffs;
                  *valuepnt += ((Section *)(indexl[k].pntr)) -> vaddr;
                  goto skip_rel;
                }
                case INDEX_UNDEF: {
                  undef = (Undef *)(indexl[k].pntr);
                  if (undef -> undentry) {							/* - see if vectored */
                    *valuepnt += vecsection -> vaddr + undef -> vecindex * sizeof *vectors;	/* - vectored, point to vector table entry */
                    *valuepnt -= section -> vaddr + sectoffs;
                    goto skip_rel;
                  }
                  reloc -> imrel.reloc_type = OZ_IMAGE_RELOC_UNDEF_NEGPC;			/* - not vectored, relocate by value of undefined symbol - location being relocd */
                  reloc -> imrel.reloc_indx = undef -> index;
                  break;
                }
              }
              break;
            }
            case 8: {										/* .LONG .+x */
              *valuepnt += section -> vaddr;
              goto skip_rel;
            }
#endif
            default: {
              fprintf (stderr, "%s: unknown relocation %u.%u type %u at " PTRFMT "\n", pn, i, j, ELFxx_R_TYPE (elfrela -> r_info), PTRSWP (reloc -> imrel.reloc_vaddr));
              errorflag = 1;
              goto skip_rel;
            }
          }
          reloc -> next = relocs;
          relocs = reloc;
          continue;
skip_rel:
          free (reloc);
        }
        break;
      }
    }
  }

  if (errorflag) return (30);

  if (debug) fprintf (stderr, "#11\n");

  /* Add relocation entries for the global offset table entries */

  for (gotentry = gotentries; gotentry != NULL; gotentry = gotentry -> next) {
    k = gotentry -> indexk;
    valuepnt = gotdata + (gotentry -> gotofs / sizeof *gotdata);
    reloc = malloc (sizeof *reloc);
    reloc -> imrel.reloc_vaddr = gotsection -> vaddr + gotentry -> gotofs;
    switch (indexl[k].type) {
      case INDEX_NULL: {
        *valuepnt = gotentry -> addend;
        goto skip_rgot;
      }
      case INDEX_GLOBAL: {
        global = indexl[k].pntr;
        *valuepnt = gotentry -> addend + global -> value;			/* - its a global defined by this image */
        if (global -> section == NULL) goto skip_rgot;				/* - if absolute value, no more relocing */
        reloc -> imrel.reloc_type = OZ_IMAGE_RELOC_IMGBASE;			/* - relocate by image base */
        break;
      }
      case INDEX_SECTION: {
        *valuepnt = gotentry -> addend + ((Section *)(indexl[k].pntr)) -> vaddr;
        if (!dynamic) goto skip_rgot;
        reloc -> imrel.reloc_type = OZ_IMAGE_RELOC_IMGBASE;			/* - relocate by image base */
        break;
      }
      case INDEX_UNDEF: {
        undef = (Undef *)(indexl[k].pntr);
        if (undef -> refdshr == NULL) {
          *valuepnt = gotentry -> addend + undef -> value;			/* - OZ_IMAGE_BASEADDR or _NEXTADDR */
          if (!dynamic) goto skip_rgot;
          reloc -> imrel.reloc_type = OZ_IMAGE_RELOC_IMGBASE;
          break;
        }
        if (undef -> undentry) {						/* - see if vectored */
          *valuepnt = gotentry -> addend + vecsection -> vaddr + undef -> vecindex * sizeof *vectors; /* - vectored, point to vector table entry */
          if (!dynamic) goto skip_rgot;
          reloc -> imrel.reloc_type = OZ_IMAGE_RELOC_IMGBASE;			/*   relocated by image base */
        } else {
          reloc -> imrel.reloc_type = OZ_IMAGE_RELOC_UNDEF;			/* - not vectored, relocate by value of undefined symbol */
          reloc -> imrel.reloc_indx = undef -> index;
        }
        break;
      }
    }
    reloc -> next = relocs;
    relocs = reloc;
    continue;
skip_rgot:
    free (reloc);
  }

  /* Add relocation entries for the vectors to point to symbols in external images */

  for (undef = undefs; undef != NULL; undef = undef -> next) {
    if (undef -> undentry) {
      reloc = malloc (sizeof *reloc);
      reloc -> next    = relocs;
      reloc -> section = vecsection;
      reloc -> imrel.reloc_vaddr = ((char *)&(vectors[undef->vecindex].entrypoint)) - ((char *)vectors) + vecsection -> vaddr;
      reloc -> imrel.reloc_type  = VEC_RELOC_TYPE;
      reloc -> imrel.reloc_indx  = undef -> index;
      relocs = reloc;
    }
  }

  /* Sort global symbol list 'alphabetically'                     */
  /* This way, the loader can use a binary search to look them up */

  i = 0;											/* count number of global symbols we have */
  for (global = globals; global != NULL; global = global -> next) i ++;
  globall = malloc (i * sizeof *globall);							/* allocate memory for an array of pointers */
  i = 0;											/* fill in the array with pointers */
  for (global = globals; global != NULL; global = global -> next) globall[i++] = global;
  qsort (globall, i, sizeof *globall, global_compare);						/* sort the elements using the symbol names */
  globals = NULL;										/* re-build the linked list */
  while (i > 0) {
    global = globall[--i];
    global -> next = globals;
    globals = global;
  }
  free (globall);										/* free off the array of pointers */

  if (debug) fprintf (stderr, "#12\n");

  /* Print out in-memory structs */

  printf ("\n");
  printf ("Base memory address " PTRFMT "\n", PTRSWP (baseaddr));
  printf ("Next memory address " PTRFMT "\n", PTRSWP (nextaddr));

  printf ("\nGlobals:\n");
  for (global = globals; global != NULL; global = global -> next) {
    if ((global -> stbind != STB_GLOBAL) && (global -> stbind != STB_WEAK)) continue;
    printf ("  " PTRFMT "  %c%c %s\n", PTRSWP (global -> value), 
	(global -> section != NULL) ? '+' : ' ', (global -> gblent) ? '@' : ' ', 
	global -> name);
  }

  printf ("\nRefdshrs:\n");
  for (refdshr = refdshrs; refdshr != NULL; refdshr = refdshr -> next) {
    printf ("%d  %4u  %s\n", refdshr -> used, refdshr -> nglobals, refdshr -> name);
  }

  printf ("\nRelocs:\n");
  for (reloc = relocs; reloc != NULL; reloc = reloc -> next) {
    switch (reloc -> imrel.reloc_type) {
      case OZ_IMAGE_RELOC_IMGBASE: {
        printf ("  " PTRFMT ":  " DOTSIZE "  x+imagebase\n", PTRSWP (reloc -> imrel.reloc_vaddr));
        break;
      }
      case OZ_IMAGE_RELOC_IMGBASE_NEGPC: {
        printf ("  " PTRFMT ":  " DOTSIZE "  x-.\n", PTRSWP (reloc -> imrel.reloc_vaddr));
        break;
      }
      case OZ_IMAGE_RELOC_UNDEF: {
        undef = indexl[reloc->imrel.reloc_indx].pntr;
        printf ("  " PTRFMT ":  " DOTSIZE "  %s\n", PTRSWP (reloc -> imrel.reloc_vaddr), undef -> name);
        break;
      }
      case OZ_IMAGE_RELOC_UNDEF_NEGPC: {
        undef = indexl[reloc->imrel.reloc_indx].pntr;
        printf ("  " PTRFMT ":  " DOTSIZE "  %s-.\n", PTRSWP (reloc -> imrel.reloc_vaddr), undef -> name);
        break;
      }
    }
  }

  if (debug) fprintf (stderr, "#13\n");

  printf ("\nSections:\n");
  for (section = sections; section != NULL; section = section -> next) {
    printf ("%5u  " PTRFMT "  " PTRFMT "  " PTRFMT "  %s  %s\n", 
		section -> index, PTRSWP (section -> size), PTRSWP (section -> vaddr), PTRSWP (section -> fileoffs), 
		section -> writable ? "rw" : "ro", section -> nobits ? "dz" : "");
  }

  printf ("\nUndefs:\n");
  for (undef = undefs; undef != NULL; undef = undef -> next) {
    c = (undef -> undentry) ? '@' : ' ';
    if (undef -> refdshr == NULL) printf ("%5u  " PTRFMT "  %c %s\n", undef -> index, PTRSWP (undef -> value), c, undef -> name);
    else printf ("%5u  %s+" PTRFMT "  %c %s\n", undef -> index, undef -> refdshr -> name, PTRSWP (undef -> value), c, undef -> name);
  }

  if (debug) fprintf (stderr, "#14\n");

  /* Renumber undefined symbol indicies to be sequential the same way the loader wants them */
  /* This way, the reloc index will be a proper index into the undef array                  */

  i = 0;
  for (undef = undefs; undef != NULL; undef = undef -> next) undef -> index = i ++;
  for (reloc = relocs; reloc != NULL; reloc = reloc -> next) {
    if ((reloc -> imrel.reloc_type == OZ_IMAGE_RELOC_UNDEF) || (reloc -> imrel.reloc_type == OZ_IMAGE_RELOC_UNDEF_NEGPC)) {
      undef = indexl[reloc->imrel.reloc_indx].pntr;
      reloc -> imrel.reloc_indx = undef -> index;
    }
  }

  if (debug) fprintf (stderr, "#15\n");

  /* Create output header struct */

  memcpy (ozimghdr.magic, magic, sizeof ozimghdr.magic);
  ozimghdr.fxhdrsize = sizeof ozimghdr;					/* size of the fixed portion of header */
  ozimghdr.imagesize = nextaddr - baseaddr;				/* total number of memory bytes in image */
  ozimghdr.basevpage = OZ_HW_VADDRTOVPAGE (baseaddr);			/* base virtual page of image (0 for dynamic image) */
  ozimghdr.startaddr = OZ_IMAGE_NULLSTARTADDR;				/* get start address */
  for (global = globals; global != NULL; global = global -> next) {
    if (strcmp (global -> name, "_start") == 0) {
      ozimghdr.startaddr = global -> value;
    }
  }

  if (debug) fprintf (stderr, "#16\n");

  /* Create output referenced shareable image struct array */

  ozimghdr.refdessiz = sizeof *ozimgrefdes;				/* sizeof referenced shareables struct */
  ozimghdr.refdesnum = 0;						/* number of referenced shareables */
  ozimghdr.refstrsiz = 0;						/* total length of all referenced shareable name strings */
  for (refdshr = refdshrs; refdshr != NULL; refdshr = refdshr -> next) {
    if (!(refdshr -> used)) continue;
    ozimghdr.refdesnum ++;
    ozimghdr.refstrsiz += strlen (refdshr -> name) + 1;
  }

  ozimgrefdes = malloc (ozimghdr.refdesnum * sizeof *ozimgrefdes);
  ozimgrefstr = malloc (ozimghdr.refstrsiz);
  i = 0;
  j = 0;
  for (refdshr = refdshrs; refdshr != NULL; refdshr = refdshr -> next) {
    if (!(refdshr -> used)) continue;
    refdshr -> index = i;
    ozimgrefdes[i].refim_namof = j;
    k = strlen (refdshr -> name) + 1;
    memcpy (ozimgrefstr + j, refdshr -> name, k);
    i ++;
    j += k;
  }

  if (debug) fprintf (stderr, "#17\n");

  /* Create output global symbol struct array */

  ozimghdr.gbldessiz = sizeof *ozimggbldes;				/* sizeof global symbols struct */
  ozimghdr.gbldesnum = 0;						/* number of global symbols */
  ozimghdr.gblstrsiz = 0;						/* total length of all global symbol name strings */
  for (global = globals; global != NULL; global = global -> next) {
    if ((global -> stbind != STB_GLOBAL) && (global -> stbind != STB_WEAK)) continue;
    ozimghdr.gbldesnum ++;
    ozimghdr.gblstrsiz += strlen (global -> name) + 1;
  }

  ozimggbldes = malloc (ozimghdr.gbldesnum * sizeof *ozimggbldes);
  ozimggblstr = malloc (ozimghdr.gblstrsiz);
  i = 0;
  j = 0;
  for (global = globals; global != NULL; global = global -> next) {
    if ((global -> stbind != STB_GLOBAL) && (global -> stbind != STB_WEAK)) continue;
    global -> index = i;
    ozimggbldes[i].globl_namof = j;
    ozimggbldes[i].globl_value = global -> value;
    ozimggbldes[i].globl_flags = 0;
    if (global -> section != NULL) ozimggbldes[i].globl_flags |= OZ_IMAGE_GLOBL_FLAG_RELOC;
    if (global -> gblent) ozimggbldes[i].globl_flags |= OZ_IMAGE_GLOBL_FLAG_ENTRY;
    k = strlen (global -> name) + 1;
    memcpy (ozimggblstr + j, global -> name, k);
    i ++;
    j += k;
  }

  if (debug) fprintf (stderr, "#18\n");

  /* Create output undefined symbol struct array */

  ozimghdr.unddessiz = sizeof *ozimgunddes;				/* sizeof undefined symbols struct */
  ozimghdr.unddesnum = 0;						/* number of undefined symbols */
  ozimghdr.undstrsiz = 0;						/* total length of all undefined symbol name strings */
  for (undef = undefs; undef != NULL; undef = undef -> next) {
    if (undef -> refdshr == NULL) continue;
    ozimghdr.unddesnum ++;
    ozimghdr.undstrsiz += strlen (undef -> name) + 1;
  }

  ozimgunddes = malloc (ozimghdr.unddesnum * sizeof *ozimgunddes);
  ozimgundstr = malloc (ozimghdr.undstrsiz);
  i = 0;
  j = 0;
  for (undef = undefs; undef != NULL; undef = undef -> next) {
    if (undef -> refdshr == NULL) continue;
    undef -> index = i;
    ozimgunddes[i].undef_namof = j;
    ozimgunddes[i].undef_refim = undef -> refdshr -> index;
    k = strlen (undef -> name) + 1;
    memcpy (ozimgundstr + j, undef -> name, k);
    i ++;
    j += k;
  }

  if (debug) fprintf (stderr, "#19\n");

  /* Create output relocation struct array */

  ozimghdr.reldessiz = sizeof *ozimgreldes;
  ozimghdr.reldesnum = 0;
  for (reloc = relocs; reloc != NULL; reloc = reloc -> next) ozimghdr.reldesnum ++;

  ozimgreldes = malloc (ozimghdr.reldesnum * sizeof *ozimgreldes);
  i = 0;
  for (reloc = relocs; reloc != NULL; reloc = reloc -> next) ozimgreldes[i++] = reloc -> imrel;

  if (debug) fprintf (stderr, "#20\n");

  /* Create output memory section struct array */

  have_readonly  = 0;
  have_readwrite = 0;
  for (section = sections; section != NULL; section = section -> next) {				/* scan list of sections */
    if (section -> size == 0) continue;									/* skip null sections */
    if (section -> writable) have_readwrite = 1;
    else have_readonly = 1;
  }

  ozimghdr.secdessiz = sizeof *ozimgsecdes;								/* sizeof section struct */
  ozimghdr.secdesnum = have_readonly + have_readwrite;							/* number of memory sections ([0] is read-only, [1] is read/write) */
  ozimgsecdes = malloc (ozimghdr.secdesnum * sizeof *ozimgsecdes);
  memset (ozimgsecdes, 0, ozimghdr.secdesnum * sizeof *ozimgsecdes);
  if (have_readwrite) ozimgsecdes[have_readonly].sectn_write = 1;					/* set read/write sections writable flag */
  for (section = sections; section != NULL; section = section -> next) {				/* scan list of sections */
    if (section -> size == 0) continue;									/* skip null sections */
    i = section -> writable;										/* get writable flag*/
    if (i) i = have_readonly;										/* if set, get index of read/write output section */
    vpage = OZ_HW_VADDRTOVPAGE (section -> vaddr - baseaddr);						/* virt page at start of section */
    pages = (section -> vaddr - baseaddr + section -> size + MEMORY_PAGE_SIZE - 1) / MEMORY_PAGE_SIZE;	/* last page needed to map it */
    if (ozimgsecdes[i].sectn_pages == 0) {								/* see if first time for descriptor */
      ozimgsecdes[i].sectn_pages = pages;								/* number of pages in section */
      ozimgsecdes[i].sectn_vpage = vpage;								/* starting virtual page of section */
    } else {
      if (ozimgsecdes[i].sectn_vpage > vpage) ozimgsecdes[i].sectn_vpage = vpage;			/* maybe set new starting page */
      if (ozimgsecdes[i].sectn_pages < pages) ozimgsecdes[i].sectn_pages = pages;			/* maybe set new ending page */
    }
  }
  printf ("\nOutput sections:\n");
  for (i = 0; i < ozimghdr.secdesnum; i ++) {
    ozimgsecdes[i].sectn_pages -= ozimgsecdes[i].sectn_vpage;						/* change end page number to number of pages */
    printf ("  %s  %8.8x  %8.8x\n", ozimgsecdes[i].sectn_write ? "rw" : "ro", ozimgsecdes[i].sectn_pages, ozimgsecdes[i].sectn_vpage);
  }

  if (debug) fprintf (stderr, "#21\n");

  /* Assign output file offsets for all struct arrays */

  if (flag_raw) memset (&ozimghdr, 0, sizeof ozimghdr);
  else {
    i  = ozimghdr.fxhdrsize;				// start with fixed header
							// - the loader modifies this stuff
							//   so group it all together on its own pages
    ozimghdr.refdesoff = i;				// followed by referenced image descriptors
    i += ozimghdr.refdesnum * sizeof *ozimgrefdes;
    ozimghdr.gbldesoff = i;				// followed by global symbol descriptors
    i += ozimghdr.gbldesnum * sizeof *ozimggbldes;
    ozimghdr.unddesoff = i;				// followed by undefined symbol descriptors
    i += ozimghdr.unddesnum * sizeof *ozimgunddes;
    ozimghdr.secdesoff = i;				// followed by section descriptors
    i += ozimghdr.secdesnum * sizeof *ozimgsecdes;
							// - the loader doesn't modify this
							//   so if it's all together, the pages will share
    ozimghdr.reldesoff = i;				// followed by relocation descriptors
    i += ozimghdr.reldesnum * sizeof *ozimgreldes;
    ozimghdr.refstroff = i;				// followed by referenced image strings
    i += ozimghdr.refstrsiz;
    ozimghdr.gblstroff = i;				// followed by global symbol strings
    i += ozimghdr.gblstrsiz;
    ozimghdr.undstroff = i;				// followed by undefined symbol strings
    i += ozimghdr.undstrsiz;

    i +=  MEMORY_PAGE_SIZE - 1;				// followed by the image sections
    i &= -MEMORY_PAGE_SIZE;
    ozimghdr.imageoffs = i;
  }

  if (debug) fprintf (stderr, "#22\n");

  /* Write output file */

  outfd = open (arg_image, O_CREAT | O_TRUNC | O_WRONLY, 0640);					/* create output file */
  if (outfd < 0) {
    fprintf (stderr, "%s: error creating output file %s: %s\n", pn, arg_image, strerror (errno));
    return (31);
  }

  if (debug) fprintf (stderr, "#23\n");

  memset (zeroes, 0, MEMORY_PAGE_SIZE);								/* zero fill file so we get zeroes in the gaps */
  for (i = 0; i < ozimghdr.imageoffs + nextaddr - baseaddr; i += MEMORY_PAGE_SIZE) writeat (outfd, i, MEMORY_PAGE_SIZE, zeroes);

  if (!flag_raw) {
    writeat (outfd, 0, sizeof ozimghdr, &ozimghdr);						/* write the fixed portion of the header */

    writeat (outfd, ozimghdr.refdesoff, ozimghdr.refdesnum * sizeof *ozimgrefdes, ozimgrefdes);	/* write the 'referenced shareables' list */
    writeat (outfd, ozimghdr.refstroff, ozimghdr.refstrsiz,                       ozimgrefstr);

    writeat (outfd, ozimghdr.gbldesoff, ozimghdr.gbldesnum * sizeof *ozimggbldes, ozimggbldes);	/* write the 'global symbols' list */
    writeat (outfd, ozimghdr.gblstroff, ozimghdr.gblstrsiz,                       ozimggblstr);

    writeat (outfd, ozimghdr.unddesoff, ozimghdr.unddesnum * sizeof *ozimgunddes, ozimgunddes);	/* write the 'undefined symbols' list */
    writeat (outfd, ozimghdr.undstroff, ozimghdr.undstrsiz,                       ozimgundstr);

    writeat (outfd, ozimghdr.reldesoff, ozimghdr.reldesnum * sizeof *ozimgreldes, ozimgreldes);	/* write the 'relocations' list */

    writeat (outfd, ozimghdr.secdesoff, ozimghdr.secdesnum * sizeof *ozimgsecdes, ozimgsecdes);	/* write the 'memory sections' list */
  }

  if (debug) fprintf (stderr, "#24\n");

  for (section = sections; section != NULL; section = section -> next) {			/* write the section data */
    if (!(section -> nobits)) {
      if (section == vecsection) writeat (outfd, ozimghdr.imageoffs + section -> vaddr - baseaddr, section -> size, vectors);
      else if (section == gotsection) writeat (outfd, ozimghdr.imageoffs + section -> vaddr - baseaddr, section -> size, gotdata);
      else writeat (outfd, ozimghdr.imageoffs + section -> vaddr - baseaddr, section -> size, objbuf + section -> fileoffs);
    }
  }

  close (outfd);

  if (debug) fprintf (stderr, "#25\n");
  return (0);

usage:
  i = strlen (pn);
  fprintf (stderr, "usage: %*s [<basing>] <output_oz_image_file> <input_elf_reloc_file> [ <oz_shareables> ... ] \n", i, pn);
  fprintf (stderr, "       %*s   basing =         omitted : defaults to %p\n", i, "", (void *)OZ_HW_BASE_PROC_VA);
  fprintf (stderr, "       %*s            -base <address> : loads at given address\n", i, "");
  fprintf (stderr, "       %*s             -raw <address> : loads at given address, but just the raw image data\n", i, "");
  fprintf (stderr, "       %*s                   -dynamic : loads at lowest avaiable address\n", i, "");
  return (32);
}

static OZ_Pointer hextoptr (char *p, char **r)

{
  char c;
  OZ_Pointer v;

  v = 0;
  while ((c = *p) != 0) {
    if ((c >= '0') && (c <= '9')) v = v * 16 + c - '0';
    else if ((c >= 'a') && (c <= 'f')) v = v * 16 + c - 'a' + 10;
    else if ((c >= 'A') && (c <= 'F')) v = v * 16 + c - 'A' + 10;
    else break;
    p ++;
  }
  *r = p;
  return (v);
}

/* Set index array entry */

static int setindex (Index *indexl, int i, Indextype type, void *pntr)

{
  if (indexl[i].type == 0) {
    indexl[i].type = type;
    indexl[i].pntr = pntr;
    return (0);
  }
  fprintf (stderr, "%s: duplicate use of index %d\n", pn, i);
  printindex ("old", indexl[i].type, indexl[i].pntr);
  printindex ("new", type, pntr);
  return (1);
}

static void printindex (char *prefix, Indextype type, void *pntr)

{
  switch (type) {
    case INDEX_NULL: {
      fprintf (stderr, "  %s: null\n", prefix);
      break;
    }
    case INDEX_GLOBAL: {
      fprintf (stderr, "  %s: global %s\n", prefix, ((Global *)pntr) -> name);
      break;
    }
    case INDEX_SECTION: {
      fprintf (stderr, "  %s: section " PTRFMT "\n", prefix, PTRSWP (((Section *)pntr) -> fileoffs));
      break;
    }
    case INDEX_UNDEF: {
      fprintf (stderr, "  %s: undefined %s\n", prefix, ((Undef *)pntr) -> name);
      break;
    }
    default: {
      fprintf (stderr, "  %s: unknown %d\n", prefix, type);
      break;
    }
  }
}

/* Write to output file at a specific position */

static void writeat (int outfd, int pos, int size, void *buff)

{
  int rc;

  if (lseek (outfd, pos, SEEK_SET) < 0) {
    fprintf (stderr, "%s: error positioning to %d: %s\n", pn, pos, strerror (errno));
    exit (33);
  }
  rc = write (outfd, buff, size);
  if (rc < 0) {
    fprintf (stderr, "%s: error writing %d bytes at position %d: %s\n", pn, size, pos, strerror (errno));
    exit (34);
  }
  if (rc < size) {
    fprintf (stderr, "%s: only wrote %d bytes of %d at position %d\n", pn, rc, size, pos);
    exit (35);
  }
}

/* qsort compare routine for global symbols */

static int global_compare (const void *v1, const void *v2)

{
  Global *g1, *g2;

  g1 = *(Global **)v1;
  g2 = *(Global **)v2;

  return (strcmp (g1 -> name, g2 -> name));
}
