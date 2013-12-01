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
/*  This is the general disk filesystem driver				*/
/*  It is the fs dependent layer, implementing the native ozone disk fs	*/
/*									*/
/************************************************************************/

#define OZ_VDFS_VERSION 4

#include "ozone.h"
#include "oz_dev_timer.h"
#include "oz_dev_vdfs.h"
#include "oz_hw_bootblock.h"
#include "oz_io_console.h"
#include "oz_io_disk.h"
#include "oz_io_fs.h"
#include "oz_knl_dcache.h"
#include "oz_knl_devio.h"
#include "oz_knl_event.h"
#include "oz_knl_hw.h"
#include "oz_knl_kmalloc.h"
#include "oz_knl_procmode.h"
#include "oz_knl_sdata.h"
#include "oz_knl_section.h"
#include "oz_knl_security.h"
#include "oz_knl_shuthand.h"
#include "oz_knl_spte.h"
#include "oz_knl_status.h"
#include "oz_knl_thread.h"
#include "oz_knl_userjob.h"
#include "oz_sys_dateconv.h"
#include "oz_sys_xprintf.h"

#define VOLNAME_MAX 64
#define FILENAME_MAX 64
#define SECATTR_MAX 256

#define HOMEBLOCK_VERSION 1
#define HEADER_VERSION 1

#define EXTEND_NOTRUNC OZ_FS_EXTFLAG_NOTRUNC
#define EXTEND_NOEXTHDR OZ_FS_EXTFLAG_NOEXTHDR

#define EXT_DIRCOUNT ((uLong)(-1))	// header.dircount value indicating it is an extension header
					// otherwise it is a prime header

/* These filenumbers are special use and are the same for all volumes       */
/* They all have the seq == 0, so they can't be entered, removed or deleted */
/* They are all entered in the root directory of the volume                 */

/* The numbers start at 1 and count from there.  SACRED_FIDNUM_COUNT is the highest numbered (inclusive). */

#define SACRED_FIDNUM_ROOTDIRECTORY (1)
#define SACRED_FIDNUM_INDEXHEADERS  (2)
#define SACRED_FIDNUM_INDEXBITMAP   (3)
#define SACRED_FIDNUM_STORAGEBITMAP (4)
#define SACRED_FIDNUM_BOOTHOMEBLOCK (5)

#define SACRED_FIDNUM_COUNT (5)

/* Variable areas in the file header */

#define OZ_FS_HEADER_AREA_FILNAME (0)
#define OZ_FS_HEADER_AREA_FILATTR (1)
#define OZ_FS_HEADER_AREA_SECATTR (2)
#define OZ_FS_HEADER_AREA_POINTER (3)
#define OZ_FS_HEADER_NAREAS (4)

/* Point to the pointers in a file block */

#define POINTERS(__filex) (Pointer *)(__filex -> header.area + __filex -> header.areas[OZ_FS_HEADER_AREA_POINTER].offs)

/* Point to filename string in an header */

#define FILENAME(__header) (char *)((__header).area + (__header).areas[OZ_FS_HEADER_AREA_FILNAME].offs)

/* Get the end-of-file block and byte numbers from an header */

#define FILATTRS(__header) ((Filattr *)((__header).area + (__header).areas[OZ_FS_HEADER_AREA_FILATTR].offs))

/* Test the directory bit */

#define GET_FATTRFL(__header) FILATTRS (__header) -> filattrflags
#define IS_DIRECTORY(__header) ((GET_FATTRFL (__header) & OZ_FS_FILATTRFLAG_DIRECTORY) != 0)

/* Test the write-thru flag bits */

#define FIS_WRITETHRU(__header) ((GET_FATTRFL (__header) & OZ_FS_FILATTRFLAG_WRITETHRU) != 0)
#define VIS_WRITETHRU(__volume) ((__volume -> mountflags & OZ_FS_MOUNTFLAG_WRITETHRU) != 0)

/* Our File-id structure */

#define Seq 24
struct OZ_VDFS_Fileid { uLong num;		/* vbn in indexheader file */
                        unsigned seq : Seq;	/* re-use sequence number for the header block */
                        uByte rvn;		/* volume number (starting at 1) in the volume set */
                      };

/* On-disk directory pointers */

	/* Directories are made of an array of cluster-sized 'blocks'.  */
	/* The eof pointer always includes all allocated blocks.        */

        /* In the directory as a whole as well as within each block, */
	/* filenames (excluding version number) are sorted in        */
	/* ascending order by name, then descending version number.  */

	/* Each block starts with a null-terminated filename (excluding    */
	/* version nubmer).  The null-terminated filename is followed by   */
	/* an zero-terminated array of Dirpnt's, sorted by descending      */
	/* version number.  The last element of a Dirpnt array is a dummy  */
        /* with a version number of zero.  The array of Dirpnt's are       */
        /* followed by the next null-terminated filename, etc., until      */
        /* either a null filename is found or the end-of-block is reached. */

	/* Filenames do no span block boundaries.  Dirpnt arrays do not span  */
	/* block boundaries.  If a filename/dirpnt_array does not fit in a    */
	/* single directory block, the Dirpnt array is split into more than   */
	/* one array and a separate filename/dirpnt_array entry is made for   */
	/* each fragment.  Thus, a given filename may appear in the directory */
	/* more than once (but with different version numbers).               */

typedef struct { uLong version;			/* file version number */
                 OZ_VDFS_Fileid fileid;		/* corresponding file-id */
               } Dirpnt;

/* On-disk file header structure */

typedef struct { OZ_Datebin create_date;		/* date the file was created */
                 OZ_Datebin access_date;		/* date the file was last accessed (set by oz_dev_vdfs_readvirtblock, oz_dev_vdfs_writevirtblock, writefilattr) */
                 OZ_Datebin change_date;		/* date the attributes or the data were last changed (set by oz_dev_vdfs_writevirtblock, writefilattr) */
                 OZ_Datebin modify_date;		/* date the data was last changed (set by oz_dev_vdfs_writevirtblock only) */
                 OZ_Datebin expire_date;		/* date the file will expire (external archiving system use) */
                 OZ_Datebin backup_date;		/* date the file was last backed up (external backup system use) */
                 OZ_Datebin archive_date;		/* date the file was archived (external archiving system use) */
                 uLong eofblock;			/* last virtual block number that contains valid data */
                 uWord eofbyte;				/* number of bytes in the eofblock that contain valid data */
							/* this number is in range 0..blocksize-1, inclusive */
                 uWord filattrflags;			/* file attribute flags, OZ_FS_FILATTRFLAG_... */
               } Filattr;

typedef struct { OZ_Dbn blockcount;			/* number of contiguous blocks */
                 OZ_Dbn logblock;			/* starting block number */
               } Pointer;

typedef struct { uWord headerver;			/* header version */
                 uWord checksum;			/* checksum such that all words total to zero */
                 OZ_VDFS_Fileid fileid;			/* file id */
                 OZ_VDFS_Fileid extid;			/* extension id, zero if none */
                 OZ_VDFS_Fileid dirid;			/* dircount=EXT_DIRCOUNT : previous header's fileid */
							/*                  else : (original) directory fileid */
                 uLong dircount;			/* EXT_DIRCOUNT : this is an extension header */
							/*         else : number of directory entries that point to file */
                 struct { uWord size, offs; } areas[OZ_FS_HEADER_NAREAS];
                 uByte area[1];
               } Header;

/* On-disk home block */

typedef struct { uWord homeversion;			/* file system version number */
                 uWord checksum;			/* checksum such that all uWords in homeblock total to zero */
                 char volname[VOLNAME_MAX];		/* volume name (null terminated) */
                 uLong blocksize;			/* size in bytes of disk blocks on this volume */
                 uLong clusterfactor;			/* storage bitmap cluster factor */
                 OZ_Dbn clustertotal;			/* total number of clusters on this volume */
                 OZ_Dbn clustersfree;			/* number of free clusters on this volume */
                 OZ_Dbn indexhdrlbn;			/* logical block number of index header file header */
                 OZ_Datebin lastwritemount;		/* date/time last mounted for write */
							/* set when mounted, cleared to zeroes when dismounted */
                 uLong initflags;			/* initialization flags */
               } Homeblock;

/* In-memory volume extension info */

struct OZ_VDFS_Volex { OZ_VDFS_File *indexbitmap;	/* pointer to index bitmap header in openfiles list */
                       OZ_VDFS_File *indexheaders;	/* pointer to index file header in openfiles list */
                       OZ_VDFS_File *rootdirectory;	/* pointer to root directory header in openfiles list */
                       OZ_VDFS_File *storagebitmap;	/* pointer to storage bitmap header in openfiles list */
                       uByte *dirblockbuff3;		/* address of dirblockbuff (used by enter_file and remove_file) */
                       union {				/* used by various mutually exclusive functions: */
                         struct {			// lookup_file function
                           char fname[FILENAME_MAX];	// - filename we're looking for, without ;version, but with null
                           char *name_r;		// - where to return the resultant filename, including ;version
                           Dirpnt partialdirpnt;	// - partially build dirpnt
                           volatile enum {		// - scan state
                             LOOKUP_FILE_STATE_GATHERNAME, 
                             LOOKUP_FILE_STATE_SKIPDIRPNTS, 
                             LOOKUP_FILE_STATE_MATCHVERSION, 
                             LOOKUP_FILE_STATE_DONE
                           } state;
                           OZ_VDFS_File *dirfile;	// - directory we're scanning
                           int dirpntbytesaved;		// - number of bytes of dirpnt saved in partialdirpnt
                           int level;			// - level we're at in the directory tree
                           int namelen;			// - length of fname, including the null
                           int nbytes_name_gathered;	// - number of bytes of the fname that have been gathered so far
							//   -1 if we haven't read the 'same chars' byte yet
                           int versign;			// - 0: positive version, -1: negative version
                           OZ_Dbn nclusters;		// - number of clusters in the directory
                           OZ_Dbn multiplier;		// - cluster we're currently scanning
                           OZ_Dcmpb dcmpb;		// - disk cache map parameter block
                           OZ_VDFS_Fileid *fileid_r;	// - where to return the found file-id
                           uLong version;		// - version number (abs value)
                           uLong status;		// - completion status
                           OZ_Dbn lastbuckvbn;		// - last bucket vbn we looked at
                           OZ_Dbn thisbuckvbn;		// - this bucket vbn we're looking at
                           char lastname[FILENAME_MAX];	// - last filename scanned in bucket
                         } lf;
                       } v;

                       Homeblock homeblock;		// homeblock - MUST be last
                     };

/* In-memory file extension info */

struct OZ_VDFS_Filex { OZ_VDFS_Filex *next;		// next extension file pointer
                       volatile Long headerdirty;	// this extension header is dirty
                       OZ_Dbn blocksinhdr;		// number of blocks mapped by this header
							// (total of all its pointer blockcounts)
                       Header header;			// on-disk header contents
                     };



/* Vector routines */

static int dfs_is_directory (OZ_VDFS_File *file);
static int dfs_fis_writethru (OZ_VDFS_File *file, OZ_Dbn virtblock, uLong blockoffs, uLong size, const void *buff);
static int dfs_vis_writethru (OZ_VDFS_Volume *volume);
static const char *dfs_get_volname (OZ_VDFS_Volume *volume);
static uLong dfs_getinfo1 (OZ_VDFS_Iopex *iopex);
static void dfs_wildscan_continue (OZ_VDFS_Chnex *chnex);
static void dfs_wildscan_terminate (OZ_VDFS_Chnex *chnex);
static uLong dfs_getinfo2 (OZ_VDFS_Iopex *iopex);
static uLong dfs_getinfo3 (OZ_VDFS_Iopex *iopex);
static uLong dfs_init_volume (OZ_VDFS_Devex *devex, OZ_VDFS_Volume *volume, int volnamelen, const char *volname, uLong clusterfactor, uLong secattrsize, const void *secattrbuff, uLong initflags, OZ_VDFS_Iopex *iopex);
static uLong dfs_mount_volume (OZ_VDFS_Devex *devex, OZ_VDFS_Volume *volume, uLong mountflags, OZ_VDFS_Iopex *iopex);
static uLong dfs_dismount_volume (OZ_VDFS_Devex *devex, OZ_VDFS_Volume *volume, int unload, int shutdown, OZ_VDFS_Iopex *iopex);
static uLong dfs_verify_volume (OZ_VDFS_Iopex *iopex, OZ_VDFS_Devex *devex);
static uLong dfs_get_rootdirid (OZ_VDFS_Devex *devex, OZ_VDFS_Fileid *rootdirid_r);
static const OZ_VDFS_Fileid *dfs_get_fileid (OZ_VDFS_File *file);
static uLong dfs_lookup_file (OZ_VDFS_File *dirfile, int namelen, const char *name, OZ_VDFS_Fileid *fileid_r, char *name_r, OZ_VDFS_Iopex *iopex);
static uLong dfs_enter_file (OZ_VDFS_File *dirfile, const char *dirname, int namelen, const char *name, int newversion, OZ_VDFS_File *file, const OZ_VDFS_Fileid *fileid, char *name_r, OZ_VDFS_Iopex *iopex);
static uLong dfs_remove_file (OZ_VDFS_File *dirfile, const char *name, char *name_r, OZ_VDFS_Iopex *iopex);
static void dfs_returnspec (char *spec, uLong size, char *buff, OZ_FS_Subs *subs);
static uLong dfs_create_file (OZ_VDFS_Volume *volume, int namelen, const char *name, uLong filattrflags, OZ_VDFS_Fileid *dirid, OZ_VDFS_File *file, OZ_VDFS_Fileid **fileid_r, OZ_VDFS_Iopex *iopex);
static OZ_VDFS_File *dfs_findopenfile (OZ_VDFS_Volume *volume, const OZ_VDFS_Fileid *fileid);
static uLong dfs_open_file (OZ_VDFS_Volume *volume, const OZ_VDFS_Fileid *fileid, OZ_VDFS_File *file, OZ_VDFS_Iopex *iopex);
static uLong dfs_set_file_attrs (OZ_VDFS_File *file, uLong numitems, const OZ_Itmlst2 *itemlist, OZ_VDFS_Iopex *iopex);
static uLong dfs_close_file (OZ_VDFS_File *file, OZ_VDFS_Iopex *iopex);
static uLong dfs_extend_file (OZ_VDFS_File *file, OZ_Dbn nblocks, uLong extflags, OZ_VDFS_Iopex *iopex);
static uLong dfs_write_dirty_header (OZ_VDFS_File *dirtyfile, Long alf, OZ_Datebin now, OZ_VDFS_Volume *volume, OZ_VDFS_Iopex *iopex);
static uLong dfs_writehomeblock (OZ_VDFS_Volume *volume, OZ_VDFS_Iopex *iopex);
static uLong dfs_map_vbn_to_lbn (OZ_VDFS_File *file, OZ_Dbn virtblock, OZ_Dbn *nblocks_r, OZ_Dbn *logblock_r);
static void dfs_mark_header_dirty (OZ_VDFS_File *dirtyfile);

/* Internal routines */

static uLong cf_default (OZ_Dbn totalblocks, uLong blocksize, uLong secattrsize);
static uLong write_init_header (OZ_VDFS_Volume *volume, uByte *rootdirbuff, uLong filenum, char *name, uLong secattrsize, const void *secattrbuff, 
                                uLong filattrflags, OZ_Dbn efblk, OZ_Dbn count, OZ_Dbn start, OZ_Dbn index_header_start, OZ_VDFS_Iopex *iopex);
static void check_init_alloc (OZ_VDFS_Iopex *iopex, OZ_Dbn cluster, uLong *clusterbuff, OZ_VDFS_Volume *volume, OZ_Dbn count, OZ_Dbn start);
static void calc_home_block (OZ_VDFS_Volume *volume);
static uLong setbitsinfile (OZ_VDFS_File *file, uLong nbits, uLong bitno, OZ_VDFS_Iopex *iopex, uLong blocksize, uLong *blockbuff);
static uLong lookup_file (OZ_VDFS_File *dirfile, int namelen, const char *name, OZ_Dbn *dirvbn_r, OZ_VDFS_Fileid *fileid_r, char *name_r, OZ_VDFS_Iopex *iopex);
static int dirisnotempty (OZ_VDFS_File *dirfile, OZ_VDFS_Iopex *iopex);
static void adj_wildscans_ins (OZ_VDFS_File *dirfile, OZ_Dbn dirvbn, uLong from, uLong to, OZ_Dbn blocksinserted, uByte *dirbuff, char *fname);
static void adj_wildscans_rem (OZ_VDFS_File *dirfile, OZ_Dbn dirvbn, uLong from, uLong to, uByte *dirbuff);
static uLong getcresecattr (OZ_VDFS_Iopex *iopex, uLong secattrsize, const void *secattrbuff, OZ_Secattr **secattr_r);
static uLong insert_blocks (OZ_VDFS_File *file, OZ_Dbn nblocks, OZ_Dbn atblock, OZ_VDFS_Iopex *iopex);
static uLong remove_blocks (OZ_VDFS_File *file, OZ_Dbn nblocks, OZ_Dbn atblock, OZ_VDFS_Iopex *iopex);
static uLong makeroomforapointer (OZ_VDFS_File *file, OZ_VDFS_Filex *extfilex, Pointer **pointer_p, OZ_VDFS_Filex **extfilex_r, Pointer **pointer_r, OZ_VDFS_Iopex *iopex);
static OZ_Dbn countblocksinfile (OZ_VDFS_File *file, OZ_Dbn **blockarray_r);
static void compareblockarrays (OZ_Dbn nbsmall, OZ_Dbn *basmall, OZ_Dbn nbbig, OZ_Dbn *babig, OZ_Dbn atblock, OZ_Dbn logblock);
static uLong make_header_room (OZ_VDFS_File *file, OZ_VDFS_Filex *filex, uWord roomsize, int narea, int exthdr, OZ_VDFS_Filex **filex_r, OZ_VDFS_Iopex *iopex);
static uLong allocate_header (OZ_VDFS_Volume *volume, OZ_VDFS_Filex **filex_r, OZ_VDFS_Iopex *iopex);
static void delete_header (OZ_VDFS_Filex *filex, OZ_VDFS_Iopex *iopex);
static uLong allocate_blocks (OZ_VDFS_Volume *volume, OZ_Dbn nblocks, OZ_Dbn startlbn, OZ_Dbn *nblocks_r, OZ_Dbn *logblock_r, OZ_VDFS_Iopex *iopex);
static uLong free_blocks (OZ_VDFS_Volume *volume, OZ_Dbn nblocks, OZ_Dbn logblock, OZ_VDFS_Iopex *iopex);
static uLong read_header_block (const OZ_VDFS_Fileid *fileid, OZ_Dbn hdrlbn, int exthdr, OZ_VDFS_Fileid *lastfid, OZ_VDFS_Filex *filex, OZ_VDFS_Iopex *iopex);
static void mark_exthdr_dirty (OZ_VDFS_Filex *dirtyfilex, OZ_VDFS_File *dirtyfile);
static uLong write_header_block (OZ_VDFS_Volume *volume, OZ_VDFS_Filex *filex, OZ_VDFS_Iopex *iopex);
static void write_dirty_homeboy (OZ_VDFS_Volume *volume, OZ_VDFS_Iopex *iopex);
static int validirbuf (uLong totalsize, uLong blocksize, uByte *dirbuffer, OZ_VDFS_Iopex *iopex);
static void validate_volume (OZ_VDFS_Volume *volume, int line);
static void validate_file (OZ_VDFS_File *file, OZ_VDFS_Volume *volume, int linef, int linev);
static int validate_header (Header *header, OZ_VDFS_Volume *volume, OZ_VDFS_Iopex *iopex);
static uLong verify_volume (int readonly, uLong blocksize, OZ_Dbn totalblocks, OZ_VDFS_Iopex *iopex);

/************************************************************************/
/*									*/
/*  Boot-time initialization routine					*/
/*									*/
/************************************************************************/

static const OZ_VDFS_Vector vector = { sizeof (OZ_VDFS_Fileid), 
                                       VOLNAME_MAX, FILENAME_MAX, SECATTR_MAX, 
                                       1, 			// it does versions

                                       dfs_close_file, 		// close a file
                                       dfs_create_file, 	// create a new file
                                       dfs_dismount_volume, 	// dismount volume
                                       dfs_enter_file, 		// enter a new name in a directory
                                       dfs_extend_file, 	// extend a file
                                       dfs_findopenfile, 	// see if a file is already open
                                       dfs_get_rootdirid, 	// get root directory id
                                       dfs_get_volname, 	// get volume name
                                       dfs_getinfo2, 		// get name of file open on a channel
                                       dfs_init_volume, 	// initialize a volume
                                       dfs_lookup_file, 	// look up a particular file in a directory
                                       dfs_mount_volume, 	// mount volume
                                       dfs_open_file, 		// open a file
                                       dfs_remove_file, 	// remove name from directory
                                       dfs_set_file_attrs, 	// write a file's attributes
                                       dfs_write_dirty_header, 	// flush file's header(s) to disk
                                       dfs_writehomeblock, 	// flush volume's header to disk
                                       dfs_verify_volume, 	// verify volume structures

                                       dfs_fis_writethru, 	// see if file is a 'writethru' file
                                       dfs_get_fileid, 		// get file id
                                       dfs_getinfo1, 		// get info about the file open on channel
                                       dfs_getinfo3, 		// get info about the volume
                                       dfs_is_directory, 	// see if file is a directory
                                       dfs_map_vbn_to_lbn, 	// map a file's vbn to equivalent lbn 
                                       dfs_mark_header_dirty, 	// mark (prime) header dirty
                                       dfs_returnspec, 		// return filespec string/substrings
                                       dfs_vis_writethru, 	// see if volume is a 'writethru' volume
                                       dfs_wildscan_continue, 	// scan directory block for a particular wildcard match
                                       dfs_wildscan_terminate }; // terminate wildcard scan

void oz_dev_dfs_init ()

{
  oz_dev_vdfs_init (OZ_VDFS_VERSION, "oz_dfs", &vector);
}

/************************************************************************/
/*									*/
/*  Return whether the file is a directory or not			*/
/*									*/
/*    Input:								*/
/*									*/
/*	file = file to make determination about				*/
/*									*/
/*    Output:								*/
/*									*/
/*	dfs_is_directory = 0 : file is not a directory			*/
/*	                   1 : file is a directory			*/
/*									*/
/************************************************************************/

static int dfs_is_directory (OZ_VDFS_File *file)

{
  return (IS_DIRECTORY (file -> filex -> header));
}

/************************************************************************/
/*									*/
/*  Return whether the file forces cache writethru mode			*/
/*									*/
/*    Input:								*/
/*									*/
/*	file = file to make determination about				*/
/*	virtblock = virtual block being written				*/
/*	blockoffs = offset in the block we're starting at		*/
/*	size = size of the 'buff' being written				*/
/*	buff = data being written					*/
/*									*/
/*    Output:								*/
/*									*/
/*	dfs_fis_writethru = 0 : file can use writeback mode		*/
/*	                    1 : file forces writethru mode		*/
/*									*/
/************************************************************************/

static int dfs_fis_writethru (OZ_VDFS_File *file, OZ_Dbn virtblock, uLong blockoffs, uLong size, const void *buff)

{
  if (FIS_WRITETHRU (file -> filex -> header)) return (1);		// maybe the file forces writethru mode
  if (file != file -> volume -> volex -> indexheaders) return (0);	// see if we are writing an index header
  if (blockoffs != 0) return (0);
  if (size != file -> volume -> volex -> homeblock.blocksize) return (0);
  return (FIS_WRITETHRU (*(Header *)buff));				// if so, force writethru if the corresponding file does
}

/************************************************************************/
/*									*/
/*  Return whether the volume forces cache writethru mode		*/
/*									*/
/*    Input:								*/
/*									*/
/*	volume = volume to make determination about			*/
/*									*/
/*    Output:								*/
/*									*/
/*	dfs_vis_writethru = 0 : volume can use writeback mode		*/
/*	                    1 : volume forces writethru mode		*/
/*									*/
/************************************************************************/

static int dfs_vis_writethru (OZ_VDFS_Volume *volume)

{
  return (VIS_WRITETHRU (volume));
}

/************************************************************************/
/*									*/
/*  Retrieve the volume's label						*/
/*									*/
/*    Input:								*/
/*									*/
/*	volume = volume to get the label of				*/
/*									*/
/*    Output:								*/
/*									*/
/*	dfs_get_volname = pointer to null-terminated label string	*/
/*									*/
/************************************************************************/

static const char *dfs_get_volname (OZ_VDFS_Volume *volume)

{
  return (volume -> volex -> homeblock.volname);
}

/************************************************************************/
/*									*/
/*  Get information part 1						*/
/*									*/
/************************************************************************/

static uLong dfs_getinfo1 (OZ_VDFS_Iopex *iopex)

{
  Filattr filattr;
  OZ_VDFS_Filex *filex;

  filex = iopex -> chnex -> file -> filex;

  movc4 (filex -> header.areas[OZ_FS_HEADER_AREA_FILATTR].size, filex -> header.area + filex -> header.areas[OZ_FS_HEADER_AREA_FILATTR].offs, sizeof filattr, &filattr);
  iopex -> u.getinfo1.p.filattrflags = filattr.filattrflags;
  iopex -> u.getinfo1.p.create_date  = filattr.create_date;
  iopex -> u.getinfo1.p.access_date  = filattr.access_date;
  iopex -> u.getinfo1.p.change_date  = filattr.change_date;
  iopex -> u.getinfo1.p.modify_date  = filattr.modify_date;
  iopex -> u.getinfo1.p.expire_date  = filattr.expire_date;
  iopex -> u.getinfo1.p.backup_date  = filattr.backup_date;
  iopex -> u.getinfo1.p.archive_date = filattr.archive_date;
  if (iopex -> u.getinfo1.p.fileidbuff != NULL) {
    movc4 (sizeof filex -> header.fileid, &(filex -> header.fileid), iopex -> u.getinfo1.p.fileidsize, iopex -> u.getinfo1.p.fileidbuff);
  }
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Continue scanning the current directory block			*/
/*									*/
/*  The directory is locked on input and is unlocked on return		*/
/*									*/
/************************************************************************/

static void dfs_wildscan_continue (OZ_VDFS_Chnex *chnex)

{
  char c, *p;
  Dirpnt *dirpnts;
  int filename_l, newname, nver, rc;
  OZ_IO_fs_open fs_open;
  OZ_VDFS_Chnex *dirchnex;
  OZ_VDFS_Devex *devex;
  OZ_VDFS_Iopex *iopex;
  OZ_VDFS_Volume *volume;
  OZ_VDFS_Wildscan *outerwild, *wildscan;
  uLong sts, vl;

  iopex    = chnex -> wild_iopex;
  volume   = iopex -> devex -> volume;
  wildscan = chnex -> wildscan;
  devex    = iopex -> devex;

  /* Maybe we have more wild versions to return                                     */
  /* Never leave blockoffs pointing at the ;0 entry or adj_wildscans_rem will break */

  if (wildscan -> ver_output) {
    dirpnts = (Dirpnt *)(wildscan -> blockbuff + wildscan -> blockoffs);	// point to the one to be output
    wildscan -> blockoffs += sizeof (Dirpnt);					// increment blockoffs just past it
    if (wildscan -> blockoffs + sizeof (Dirpnt) > volume -> dirblocksize) {	// see if there is room for another
      wildscan -> blockoffs  = volume -> dirblocksize;				// if not, we've used up the whole block
      wildscan -> ver_output = 0;						// ... and next block starts with a filename
    } else if (dirpnts[1].version == 0) {					// see if this is the last one of the array
      wildscan -> blockoffs += sizeof (Dirpnt);					// if so, point past the zero terminator
      wildscan -> ver_output = 0;						// ... and it is followed by a filename
    }
    oz_dev_vdfs_wildscan_output (chnex, wildscan -> lastname, dirpnts -> version, &(dirpnts -> fileid)); // anyway, output this version entry
    return;
  }

  /* Assume we will have new names throughout the bucket.  However, the name will be the same as last time if we are */
  /* at the beginning of a bucket and the file has too many versions to fit in a bucket.  So we check for that here. */

  newname = ((wildscan -> blockoffs != 0) || (strcmp (wildscan -> lastname, wildscan -> blockbuff + 1) != 0));

  /* Scan block for matching filename entry */

  while (wildscan -> blockoffs < volume -> dirblocksize) {
    if (wildscan -> blockoffs != 0) newname = 1;

    /* Point to the filename in the directory block buffer */

    p  = wildscan -> blockbuff + wildscan -> blockoffs;					// point to the 'same chars' byte
    rc = *(p ++);									// get it
    if (rc == 0) break;									// if zero, end of directory block
    filename_l = strnlen (p, volume -> dirblocksize - wildscan -> blockoffs - 1);	// get length of different chars
    if ((rc - 1 + filename_l + 1 > sizeof wildscan -> lastname) || (wildscan -> blockoffs + filename_l + 2 > volume -> dirblocksize)) {
      oz_dev_vdfs_printk (iopex, "oz_dev_dfs wildscan_continue: filename %s+%s in directory %s too long\n", wildscan -> lastname, p, wildscan -> basespec);
      goto dircorrupt;
    }
    memcpy (wildscan -> lastname + rc - 1, p, filename_l + 1);				// copy to buffer to make complete name
    wildscan -> blockoffs += filename_l + 2;						// point past filename's null terminator
    filename_l += rc - 1;								// length of complete name, without null

    /* Count the number of versions we have of this file and move the pointer past all of them for next scan */

    dirpnts = (Dirpnt *)(wildscan -> blockbuff + wildscan -> blockoffs);		// save where the dirpnt array starts
    nver = 0;										// so far we don't have any versions
    while (wildscan -> blockoffs + sizeof (Dirpnt) <= volume -> dirblocksize) {		// repeat as long as there is room for one
      wildscan -> blockoffs += sizeof (Dirpnt);						// increment pointer past it
      if (dirpnts[nver].version == 0) break;						// stop counting if version is zero
      nver ++;										// non-zero version, count it
    }
    if (nver == 0) {									// there has to be at least one
      oz_dev_vdfs_printk (iopex, "oz_dev_dfs wildscan_continue: filename %s in directory %s has no versions\n", wildscan -> lastname, wildscan -> basespec);
      goto dircorrupt;
    }

    /* See if the filename in the blockbuff matches the wildcard spec, and see if we should scan the sub-sirectory */

    rc = oz_dev_vdfs_wildscan_match (wildscan, wildscan -> lastname);

    /* Maybe scan sub-directory.  If we also output this directory, we do it either after or before the directory */
    /* contents (depending on the setting of delaydir), so we let the subdir output stuff handle that.            */

    if ((wildscan -> lastname[filename_l-1] == '/') && (rc & 2)) {
      if (iopex -> aborted) {								// don't bother if I/O request is aborted
        oz_dev_vdfs_wildscan_iodonex (iopex, OZ_ABORTED);
        return;
      }
      if (dirpnts[0].fileid.num != SACRED_FIDNUM_ROOTDIRECTORY) {			// don't nest into the root directory
        if (strlen (wildscan -> basespec) + filename_l >= sizeof wildscan -> basespec) { // make sure directory name not too long
          oz_dev_vdfs_wildscan_iodonex (iopex, OZ_FILENAMETOOLONG);
          return;
        }
        for (outerwild = wildscan; outerwild != NULL; outerwild = outerwild -> nextouter) { // don't nest into same directory twice
          dirchnex = oz_knl_iochan_ex (outerwild -> iochan);
          if (memcmp (&(dirpnts[0].fileid), &(dirchnex -> file -> filex -> header.fileid), sizeof dirpnts[0].fileid) == 0) {
            oz_dev_vdfs_wildscan_iodonex (iopex, OZ_DIRECTORYLOOP);
            return;
          }
        }
        oz_dev_vdfs_wildscan_startsubdir (chnex, wildscan -> lastname, &(dirpnts[0].fileid), rc); // start processing sub-directory
        return;
      }
    }

    /* Return match to caller */

    if (!(rc & 1)) continue;								// don't if name didn't match

    if (wildscan -> lastname[filename_l-1] == '/') {					// see if it is a dir name
      if (!(wildscan -> ver_incldirs)) continue;					// skip if we don't include directories
      oz_dev_vdfs_wildscan_output (chnex, wildscan -> lastname, 0, &(dirpnts -> fileid)); // ok, output it
      return;
    }

    if (wildscan -> ver_inclallfiles) {							// see if incl all versions of files
      if (nver > 1) {									// ok, see if more than one version
        wildscan -> ver_output = 1;							// if so, do more on next call
        wildscan -> blockoffs  = (char *)(dirpnts + 1) - wildscan -> blockbuff;		// ... and here is where the next one is
      }
      oz_dev_vdfs_wildscan_output (chnex, wildscan -> lastname, dirpnts -> version, &(dirpnts -> fileid)); // output this version
      return;
    }

    if (wildscan -> ver_number > 0) {							// see if looking for one particular version
      while (-- nver >= 0) {								// ok, search the array for it
        if (dirpnts -> version == wildscan -> ver_number) {
          oz_dev_vdfs_wildscan_output (chnex, wildscan -> lastname, dirpnts -> version, &(dirpnts -> fileid)); // found, output the entry
          return;
        }
        dirpnts ++;
      }
      continue;										// not found, skip the entry
    }

    if (newname) {									// relative version, see if new filename
      wildscan -> ver_count = wildscan -> ver_number;					// if so, reset version counter
    }
    while ((wildscan -> ver_count != 0) && (nver > 0)) {				// see if any more versions to skip
      wildscan -> ver_count ++; -- nver; dirpnts ++;					// if so, skip over one more
    }
    if (nver <= 0) continue;								// skip this entry if didn't get count
    wildscan -> ver_count = 1;								// only do one from this name
    oz_dev_vdfs_wildscan_output (chnex, wildscan -> lastname, dirpnts -> version, &(dirpnts -> fileid)); // output it
    return;
  }

  /* Reached end of currrent block, start reading a new one */

  wildscan -> blockvbn += volume -> dirblocksize / iopex -> devex -> blocksize;
  oz_dev_vdfs_wildscan_readdir (chnex);
  return;

  /* Something is corrupt about the directory block */

dircorrupt:
  oz_dev_vdfs_wildscan_iodonex (iopex, OZ_DIRCORRUPT);
}

static void dfs_wildscan_terminate (OZ_VDFS_Chnex *chnex)

{ }

/************************************************************************/
/*									*/
/*  Get information part 2						*/
/*									*/
/************************************************************************/

static uLong dfs_getinfo2 (OZ_VDFS_Iopex *iopex)

{
  char *buff, *p;
  OZ_VDFS_Chnex *chnex;
  OZ_VDFS_File *dirfile, *file;
  OZ_VDFS_Filex *filex;
  uLong i, l, size, sts;

  chnex = iopex -> chnex;
  file  = chnex -> file;

  /* Get the complete filespec by looking back through the dirid links */

  size = iopex -> u.getinfo2.p.filnamsize;							/* get size of user supplied filename buffer */
  buff = iopex -> u.getinfo2.p.filnambuff;							/* get its address */

  i = size;
  filex = file -> filex;
  while (filex -> header.fileid.num != SACRED_FIDNUM_ROOTDIRECTORY) {				/* repeat until we reach the root directory */
    p = FILENAME (filex -> header);								/* point to the filename string therein */
    l = strlen (p);										/* get the filename string length */
    if (i <= l) {										/* see if there is room for it */
      memcpy (buff, p + l - i, i);								/* if not, copy what of it will fit */
      i = 0;											/* no room left in output buffer */
      break;											/* stop scanning */
    }
    i -= l;											/* enough room, back up output buffer offset */
    memcpy (buff + i, p, l);									/* copy in string */
    sts = oz_dev_vdfs_open_file (file -> volume, &(filex -> header.dirid), OZ_SECACCMSK_LOOK, &dirfile, iopex); /* try to open parent directory (must be able to look up a specific file) */
    if (file != chnex -> file) oz_dev_vdfs_close_file (file, iopex);				/* close prior parent directory */
    if (sts != OZ_SUCCESS) {
      oz_dev_vdfs_printk (iopex, "oz_dev_dfs: getinfo2: error %u getting directory name\n", sts);
      return (sts);
    }
    file  = dirfile;
    filex = file -> filex;
  }
  if (file != chnex -> file) oz_dev_vdfs_close_file (file, iopex);				/* close parent directory */
  if (i > 0) buff[--i] = '/';									/* put in the '/' that represents root directory */
  if (i > 0) {
    l = size - i;										/* room left at beginning, see how long string is */
    memmove (buff, buff + i, l);								/* move string to beginning of buffer */
    buff[l] = 0;										/* null terminate it */
  }

  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Get information part 3 (no file need be open)			*/
/*									*/
/************************************************************************/

static void dfs_fidtoa (const void *fileid, int size, char *buff);
static int dfs_atofid (const char *buff, void *fileid);

static uLong dfs_getinfo3 (OZ_VDFS_Iopex *iopex)

{
  const char *unitname;
  OZ_Devunit *devunit;
  OZ_VDFS_Volex *volex;

  volex = iopex -> devex -> volume -> volex;
  iopex -> u.getinfo3.p.blocksize     = volex -> homeblock.blocksize;
  iopex -> u.getinfo3.p.clusterfactor = volex -> homeblock.clusterfactor;
  iopex -> u.getinfo3.p.clustersfree  = volex -> homeblock.clustersfree;
  iopex -> u.getinfo3.p.clustertotal  = volex -> homeblock.clustertotal;
  iopex -> u.getinfo3.p.fileidstrsz   = 32;
  iopex -> u.getinfo3.p.fidtoa        = dfs_fidtoa;
  iopex -> u.getinfo3.p.atofid        = dfs_atofid;

  return (OZ_SUCCESS);
}

static void dfs_fidtoa (const void *fileid, int size, char *buff)

{
  const OZ_VDFS_Fileid *fid;

  fid = fileid;
  oz_sys_sprintf (size, buff, "(%u,%u,%u)", fid -> num, fid -> rvn, fid -> seq);
}

static int dfs_atofid (const char *buff, void *fileid)

{
  const char *p;
  int i;
  OZ_VDFS_Fileid *fid;

  fid = fileid;
  p = buff;
  if (*(p ++) != '(') return (-1);
  fid -> num = oz_hw_atoi (p, &i);
  p += i;
  if (*(p ++) != ',') return (-1);
  fid -> rvn = oz_hw_atoi (p, &i);
  p += i;
  if (*(p ++) != ',') return (-1);
  fid -> seq = oz_hw_atoi (p, &i);
  p += i;
  if (*(p ++) != ')') return (-1);
  return (p - buff);
}

/************************************************************************/
/*									*/
/*  Initialize a volume							*/
/*									*/
/*    Input:								*/
/*									*/
/*	volnamelen = length of volume name string			*/
/*	volname    = volume name string (not null terminated)		*/
/*	clusterfactor = cluster factor					*/
/*	secattrsize/buff = security attributes for the volume		*/
/*									*/
/*    Output:								*/
/*									*/
/*	init_volume = OZ_SUCCESS : successful				*/
/*	                    else : error status				*/
/*									*/
/************************************************************************/

static uLong dfs_init_volume (OZ_VDFS_Devex *devex, OZ_VDFS_Volume *volume, int volnamelen, const char *volname, uLong clusterfactor, uLong secattrsize, const void *secattrbuff, uLong initflags, OZ_VDFS_Iopex *iopex)

{
  OZ_Dbn boot_home_count, boot_home_start, cluster, clusters_needed, indexclusters, index_bitmap_count, index_bitmap_start, index_header_count, index_header_start;
  OZ_Dbn low_alloc_cluster, root_directory_count, root_directory_start, storage_bitmap_count, storage_bitmap_start, totalblocks;
  OZ_VDFS_Volex *volex;
  uByte *rootdirbuff;
  uLong bitsperblock, bitspercluster, bytespercluster, *clusterbuff, sts;

  clusterbuff = NULL;
  rootdirbuff = NULL;

  /* Round total blocks down to page size so caching will work on the last page without any crazy code */

  totalblocks  = devex -> totalblocks;
  totalblocks /= (1 << OZ_HW_L2PAGESIZE) / devex -> blocksize;
  totalblocks *= (1 << OZ_HW_L2PAGESIZE) / devex -> blocksize;

  /* Allocate a temporary volex (volume extension) struct */

  sts   = ((uByte *)&(volex -> homeblock)) + devex -> blocksize - (uByte *)volex;
  volex = OZ_KNL_PGPMALLOQ (sts);
  if (volex == NULL) return (OZ_EXQUOTAPGP);
  memset (volex, 0, sts);
  volume -> volex = volex;

  /* Determine default cluster factor if none specified */

  if (clusterfactor == 0) clusterfactor = cf_default (totalblocks, devex -> blocksize, secattrsize);

  /* Fill in part of the homey */

  volex -> homeblock.homeversion   = HOMEBLOCK_VERSION;
  volex -> homeblock.blocksize     = devex -> blocksize;
  volex -> homeblock.clusterfactor = clusterfactor;
  volex -> homeblock.clustertotal  = totalblocks / clusterfactor;
  volex -> homeblock.initflags     = initflags & OZ_FS_INITFLAG_WRITETHRU;
  volume -> dirblocksize           = volex -> homeblock.clusterfactor * volex -> homeblock.blocksize;
  volume -> clusterfactor          = volex -> homeblock.clusterfactor;
  movc4 (volnamelen, volname, sizeof volex -> homeblock.volname, volex -> homeblock.volname);

  /* Calculate the homey's logical block number - it puts it either right after or right before the boot block(s) */

  calc_home_block (volume);

  boot_home_count = volume -> bb_nblocks;					/* get number of blocks in bootblock */
  boot_home_start = volume -> bb_logblock;					/* get starting block number of bootblock */
  if (volume -> hb_logblock == boot_home_start - 1) boot_home_start --;		/* if home block is before bootblock, adjust starting block number */
  else if (volume -> hb_logblock != boot_home_count + boot_home_start) {	/* otherwise, it had better be just after it! */
    oz_crash ("oz_dev_dfs init_volume: hb_logblock %u, boot_home_count %u, boot_home_start %u", volume -> hb_logblock, boot_home_count, boot_home_start);
  }
  boot_home_count += boot_home_start;						/* point boot_home_count at last block in boot/home combination, inclusive */
										/* (note that boot_home_count did not include the home block to start with) */
  boot_home_start /= clusterfactor;						/* convert starting block number to cluster number */
  boot_home_count /= clusterfactor;						/* convert ending block number to cluster number (inclusive) */
  boot_home_count -= boot_home_start - 1;					/* convert boot_home_count to cluster count */

  /* Get number of blocks needed for initial files:                            */
  /*         index header file : bitspercluster blocks = bitsperblock clusters */
  /*         index bitmap file : 1 cluster                                     */
  /*       storage bitmap file : clustertotal / bitspercluster clusters        */
  /*   boot and homeblock file : 0                                             */
  /*       root directory file : 1 cluster                                     */

  bitsperblock    = volex -> homeblock.blocksize * 8;
  bytespercluster = volex -> homeblock.blocksize * clusterfactor;
  bitspercluster  = bitsperblock * clusterfactor;
  indexclusters   = bitsperblock;
  if (indexclusters > volex -> homeblock.clustertotal / (clusterfactor + 1)) {		// make sure we leave room for that many files, though
    indexclusters = volex -> homeblock.clustertotal / (clusterfactor + 1);
  }
  if (volex -> homeblock.clustertotal <= 5760) {					// fudge for small disks (floppies), don't take up more than 1/16th
    if (indexclusters > volex -> homeblock.clustertotal / 16) indexclusters = volex -> homeblock.clustertotal / 16;
  }
  clusters_needed = indexclusters + 1 + volex -> homeblock.clustertotal / bitspercluster + 0 + 1;

  /* Position all that stuff in the middle of the volume - be sure to miss the boot and home blocks */
  /* But for small disks (floppies), put it all at the beginning so we can have a big contig file   */

  if (volex -> homeblock.clustertotal > 5760) {
    low_alloc_cluster = (volex -> homeblock.clustertotal - clusters_needed) / 2;
    if (low_alloc_cluster >= boot_home_count + boot_home_start) goto it_misses;	/* if it starts after boot/home blocks end, it's ok */
    if (low_alloc_cluster + clusters_needed <= boot_home_start) goto it_misses;	/* if it ends before boot/home blocks start, it's ok */
  }

  low_alloc_cluster = boot_home_count + boot_home_start;			/* maybe it can go just after boot/home blocks */
  if (clusters_needed <= volex -> homeblock.clustertotal - low_alloc_cluster) goto it_misses;
  low_alloc_cluster = boot_home_start - clusters_needed;			/* maybe it can go just before boot/home blocks */
  if (clusters_needed <= boot_home_start) goto it_misses;

  oz_dev_vdfs_printk (iopex, "oz_dev_dfs: not enough room on disk for initialization data\n");
  oz_dev_vdfs_printk (iopex, "                boot_home_count %u, boot_home_start %u, clusters_needed %u\n", boot_home_count, boot_home_start, clusters_needed);
  sts = OZ_DISKISFULL;
  goto cleanup;

it_misses:
  index_header_count   = indexclusters * clusterfactor;
  index_header_start   = low_alloc_cluster * clusterfactor;

  index_bitmap_count   = clusterfactor;
  index_bitmap_start   = index_header_start + index_header_count;

  storage_bitmap_count = ((volex -> homeblock.clustertotal + bitspercluster - 1) / bitspercluster) * clusterfactor;
  storage_bitmap_start = index_bitmap_start + index_bitmap_count;

  root_directory_count = clusterfactor;
  root_directory_start = storage_bitmap_start + storage_bitmap_count;

  /* Set up root directory buffer */

  rootdirbuff = OZ_KNL_PGPMALLOQ (clusterfactor * volex -> homeblock.blocksize);
  if (rootdirbuff == NULL) {
    sts = OZ_EXQUOTAPGP;
    goto cleanup;
  }
  memset (rootdirbuff, 0, volume -> dirblocksize);

  /* Write the file headers and fill in root directory buffer */

  boot_home_count *= clusterfactor;
  boot_home_start *= clusterfactor;
                         sts = write_init_header (volume, rootdirbuff, SACRED_FIDNUM_ROOTDIRECTORY, "oz_fs_rootdirectory/", secattrsize, secattrbuff, 
                        OZ_FS_FILATTRFLAG_DIRECTORY, root_directory_count, root_directory_count, root_directory_start, index_header_start, iopex);
  if (sts == OZ_SUCCESS) sts = write_init_header (volume, rootdirbuff, SACRED_FIDNUM_INDEXHEADERS,  "oz_fs_indexheaders",   secattrsize, secattrbuff, 
                                                  0, SACRED_FIDNUM_COUNT, index_header_count, index_header_start, index_header_start, iopex);
  if (sts == OZ_SUCCESS) sts = write_init_header (volume, rootdirbuff, SACRED_FIDNUM_INDEXBITMAP,   "oz_fs_indexbitmap",    secattrsize, secattrbuff, 
                                                  0, index_bitmap_count, index_bitmap_count, index_bitmap_start, index_header_start, iopex);
  if (sts == OZ_SUCCESS) sts = write_init_header (volume, rootdirbuff, SACRED_FIDNUM_STORAGEBITMAP, "oz_fs_storagebitmap",  secattrsize, secattrbuff, 
                                                  0, storage_bitmap_count, storage_bitmap_count, storage_bitmap_start, index_header_start, iopex);
  if (sts == OZ_SUCCESS) sts = write_init_header (volume, rootdirbuff, SACRED_FIDNUM_BOOTHOMEBLOCK, "oz_fs_boothomeblock",  secattrsize, secattrbuff, 
                                                  0, boot_home_count, boot_home_count, boot_home_start, index_header_start, iopex);
  if (sts != OZ_SUCCESS) goto cleanup;

  /* Write the index bitmap file contents = mark the first SACRED_FIDNUM_COUNT headers as allocated, the rest are free */

  clusterbuff = OZ_KNL_PGPMALLOQ (bytespercluster);
  if (clusterbuff == NULL) {
    sts = OZ_EXQUOTAPGP;
    goto cleanup;
  }
  memset (clusterbuff, 0, bytespercluster);
  clusterbuff[0] = (1 << SACRED_FIDNUM_COUNT) - 1;
  sts = oz_dev_vdfs_writelogblock (index_bitmap_start, 0, bytespercluster, clusterbuff, 0, iopex);
  if (sts != OZ_SUCCESS) {
    oz_dev_vdfs_printk (iopex, "oz_dev_dfs init_volume: error %u writing initial index bitmap file block at %u\n", sts, index_bitmap_start);
    goto cleanup;
  }

  /* Write the storage bitmap file contents = mark the initial stuff from the three files that take room as being allocated, the rest are free. */
  /* Also mark the boot and home blocks as being allocated, and anything at the end of the last storage bitmap that is off the end of the disk. */

  volex -> homeblock.clustersfree = 0;
  for (cluster = 0; cluster < volex -> homeblock.clustertotal; cluster += bitspercluster) {
    volex -> homeblock.clustersfree += bitspercluster;
    memset (clusterbuff, 0, bytespercluster);
    check_init_alloc (iopex, cluster, clusterbuff, volume,      boot_home_count, boot_home_start);
    check_init_alloc (iopex, cluster, clusterbuff, volume,   index_header_count, index_header_start);
    check_init_alloc (iopex, cluster, clusterbuff, volume,   index_bitmap_count, index_bitmap_start);
    check_init_alloc (iopex, cluster, clusterbuff, volume, storage_bitmap_count, storage_bitmap_start);
    check_init_alloc (iopex, cluster, clusterbuff, volume, root_directory_count, root_directory_start);
    check_init_alloc (iopex, cluster, clusterbuff, volume,       bitspercluster * clusterfactor, 
                                                volex -> homeblock.clustertotal * clusterfactor);
    sts = oz_dev_vdfs_writelogblock (storage_bitmap_start + cluster / bitsperblock, 0, bytespercluster, clusterbuff, 0, iopex);
    if (sts != OZ_SUCCESS) {
      oz_dev_vdfs_printk (iopex, "oz_dev_dfs init_volume: error %u writing initial storage bitmap file block at %u\n", sts, storage_bitmap_start + cluster * clusterfactor);
      goto cleanup;
    }
  }

  /* Erase the boot block */

  for (cluster = 0; cluster < volex -> homeblock.blocksize / sizeof (uLong); cluster ++) {
    clusterbuff[cluster] = 0xDEADBEEF;
  }
  for (cluster = 0; cluster < volume -> bb_nblocks; cluster ++) {
    sts = oz_dev_vdfs_writelogblock (volume -> bb_logblock + cluster, 0, volex -> homeblock.blocksize, clusterbuff, 0, iopex);
    if (sts != OZ_SUCCESS) {
      oz_dev_vdfs_printk (iopex, "oz_dev_dfs init_volume: error %u erasing boot block %u\n", sts, volume -> bb_logblock + cluster);
      goto cleanup;
    }
  }

  /* Write out the root directory cluster */

  if (!validirbuf (bytespercluster, bytespercluster, rootdirbuff, iopex)) {
    oz_dev_vdfs_printk (iopex, "oz_dev_dfs init_volume: initial root directory block corrupt\n");
    sts = OZ_DIRCORRUPT;
    goto cleanup;
  }
  sts = oz_dev_vdfs_writelogblock (root_directory_start, 0, bytespercluster, rootdirbuff, 0, iopex);
  if (sts != OZ_SUCCESS) {
    oz_dev_vdfs_printk (iopex, "oz_dev_dfs init_volume: error %u writing initial root directory file block at %u\n", sts, root_directory_start);
    goto cleanup;
  }

  /* Fill in the rest of the homey */

  volex -> homeblock.indexhdrlbn = index_header_start - 1 + SACRED_FIDNUM_INDEXHEADERS;

  /* Finally, write the homey */

  sts = dfs_writehomeblock (volume, iopex);
  if (sts != OZ_SUCCESS) {
    oz_dev_vdfs_printk (iopex, "oz_dev_dfs init_volume: error %u writing home block at %u\n", sts, volume -> hb_logblock);
    goto cleanup;
  }

  /* Clean up and return status */

cleanup:
  if (clusterbuff != NULL) OZ_KNL_PGPFREE (clusterbuff);
  if (rootdirbuff != NULL) OZ_KNL_PGPFREE (rootdirbuff);
  OZ_KNL_PGPFREE (volex);
  volume -> volex = NULL;
  return (sts);
}

/************************************************************************/
/*									*/
/*  Determine default cluster factor for disk				*/
/*									*/
/*  We do this by figuring out what cluster factor would let us fill 	*/
/*  the disk just before running out of index header space, assuming 	*/
/*  the files are at least one cluster each				*/
/*									*/
/*  Also, we assume that the indexbitmap file is extended in one 	*/
/*  cluster increments each time the indexheaders file fills		*/
/*									*/
/************************************************************************/

static uLong cf_default (OZ_Dbn totalblocks, uLong blocksize, uLong secattrsize)

{
  OZ_Dbn bitsinablock, maxpointersinindexheader;
  uLong cf;

  bitsinablock = blocksize * 8;

  /* Caclulate the number of pointers that fit into the index file header */
  /* The '24' is for the length of "oz_fs_indexheaders;1"                 */

  maxpointersinindexheader = (blocksize - 24 - sizeof (Header) - sizeof (Filattr) - secattrsize) / sizeof (Pointer);

  /* So the index file header can hold maxpointersinindexheader                                                             */
  /* Each indexbitmap extension will be one cluster, so that is 'bitsinablock * cf' bits                                    */
  /* Each indexbitmap bit corresponds to one indexheaders block, so each indexheaders pointer is 'bitsinablock * cf' blocks */
  /* We assume each file will be just one cluster in size, plus one block for its header                                    */

  /* So we want to select a cluster factor as small as possible that the */
  /* index header won't overflow (the disk will run out of space first)  */

  /* Start with 1, 2, 4, 8, etc, up to cache page size so a integer number of clusters fit in a cache page */

  for (cf = 1; cf * blocksize <= OZ_KNL_CACHE_PAGESIZE; cf *= 2) {
    if (maxpointersinindexheader * bitsinablock * cf * (1 + cf) >= totalblocks) return (cf);
  }

  /* Now just do multiples of cache page size until we get a big enough number */

  do cf += OZ_KNL_CACHE_PAGESIZE / blocksize;
  while (maxpointersinindexheader * bitsinablock * cf * (1 + cf) < totalblocks);

  return (cf);
}

/************************************************************************/
/*									*/
/*  Write an initialization header					*/
/*									*/
/*    Input:								*/
/*									*/
/*	volume = pointer to volume block				*/
/*	rootdirbuff = root directory buffer				*/
/*	              or NULL to not enter the file			*/
/*	filenum = sacred filenum of header being written		*/
/*	name = file name string						*/
/*	secattrsize/buff = security attributes buffer size and address	*/
/*	filattrflags = file attribute flags				*/
/*	efblk = end-of-file block number				*/
/*	count = number of blocks allocated to file			*/
/*	start = first logical block allocated to file			*/
/*	index_header_start = logical block number for header #1		*/
/*									*/
/*    Output:								*/
/*									*/
/*	write_init_header = OZ_SUCCESS : successfully written		*/
/*	                          else : error status			*/
/*									*/
/************************************************************************/

static uLong write_init_header (OZ_VDFS_Volume *volume, uByte *rootdirbuff, uLong filenum, char *name, uLong secattrsize, const void *secattrbuff, 
                                uLong filattrflags, OZ_Dbn efblk, OZ_Dbn count, OZ_Dbn start, OZ_Dbn index_header_start, OZ_VDFS_Iopex *iopex)

{
  char lastname[FILENAME_MAX], nextname[FILENAME_MAX];
  Dirpnt dirpnt[2];
  int cmp;
  uLong areaoffs, cksm, i, j, k, l, m, sts;
  Filattr filattr;
  Header *header;
  OZ_VDFS_Volex *volex;

  volex = volume -> volex;

  /* Allocate a temp header memory block and fill in fixed items */

  header = OZ_KNL_PGPMALLOQ (volex -> homeblock.blocksize);
  if (header == NULL) return (OZ_EXQUOTAPGP);
  memset (header, 0, volex -> homeblock.blocksize);
  header -> headerver  = HEADER_VERSION;
  header -> fileid.num = filenum;
  header -> fileid.rvn = 1;

  /* Put entry in root directory block */

  header -> dirid.num  = SACRED_FIDNUM_ROOTDIRECTORY;
  header -> dirid.rvn  = 1;
  header -> dircount   = 1;

  memset (dirpnt, 0, sizeof dirpnt);				// set up the dirpnts
  dirpnt[0].version = 1;
  dirpnt[0].fileid  = header -> fileid;

  memset (lastname, 0, sizeof lastname);			// clear 'last name in bucket' buffer
  for (i = 0; rootdirbuff[i] != 0; i += sizeof dirpnt) {	// scan as long as there are entries to look at
    memcpy (nextname, lastname,  rootdirbuff[i] - 1);		// make up the full name we're looking at
    strcpy (nextname + rootdirbuff[i] - 1, rootdirbuff + i + 1);
    cmp = strcmp (nextname, name);				// compare it to the one we want to enter
    if (cmp > 0) break;						// if it is .gt., stop scanning
    i += strlen (rootdirbuff + i) + 1;				// .lt., skip to next entry
    strcpy (lastname, nextname);
  }
  for (j = i; rootdirbuff[j] != 0; j += sizeof dirpnt) {	// skip to end of existing buffer
    j += strlen (rootdirbuff + j) + 1;
  }
  for (k = 0;; k ++) if (name[k] != lastname[k]) break;		// see how much we match with last entry
  if (j == i) {
    m = strlen (name) + 1;					// see how long string is we are going to enter
    if (j + 1 + m - k + sizeof dirpnt > volume -> dirblocksize) {
      oz_dev_vdfs_printk (iopex, "oz_dev_dfs write_init_header: cluster not big enough to hold all initial filenames\n");
      return (OZ_DIRCORRUPT);
    }
    rootdirbuff[i++] = k + 1;					// store 'same as last + 1' byte
    memcpy (rootdirbuff + i, name + k, m - k);			// copy the different string
    i += m - k;							// increment to where dirpnts go
    memcpy (rootdirbuff + i, dirpnt, sizeof dirpnt);		// copy them in
  } else {
    for (l = 0;; l ++) if (name[l] != nextname[l]) break;	// see how much we match with next entry
    m = strlen (name) + 1;					// see how long string is we are going to enter
    if (j + 1 + m - k + sizeof dirpnt + rootdirbuff[i] - l - 1 > volume -> dirblocksize) {
      oz_dev_vdfs_printk (iopex, "oz_dev_dfs write_init_header: cluster not big enough to hold all initial filenames\n");
      return (OZ_DIRCORRUPT);
    }
    memmove (rootdirbuff + i + 1 + m - k + sizeof dirpnt + 1, 	// make room for new entry
             rootdirbuff + i + l + 2 - rootdirbuff[i], 
             j - i - l - 2 + rootdirbuff[i]);
    rootdirbuff[i++] = k + 1;					// store 'same as last + 1' byte
    memcpy (rootdirbuff + i, name + k, m - k);			// copy the different string
    i += m - k;							// increment to where dirpnts go
    memcpy (rootdirbuff + i, dirpnt, sizeof dirpnt);		// copy them in
    i += sizeof dirpnt;
    rootdirbuff[i] = l + 1;					// how much of next entry is same as new one
  }
  if (!validirbuf (volume -> dirblocksize, volume -> dirblocksize, rootdirbuff, iopex)) return (OZ_DIRCORRUPT);

  /* Fill in variable areas of the header */

  areaoffs = 0;
  for (i = 0; i < OZ_FS_HEADER_NAREAS; i ++) {
    header -> areas[i].offs = areaoffs;
    switch (i) {

      /* Fill in filename including the null */

      case OZ_FS_HEADER_AREA_FILNAME: {
        header -> areas[OZ_FS_HEADER_AREA_FILNAME].size = strlen (name) + 1;
        memcpy (header -> area + areaoffs, name, header -> areas[OZ_FS_HEADER_AREA_FILNAME].size);
        break;
      }

      /* Fill in file attributes */

      case OZ_FS_HEADER_AREA_FILATTR: {
        memset (&filattr, 0, sizeof filattr);
        filattr.create_date  = oz_hw_tod_getnow ();
        filattr.change_date  = filattr.create_date;
        filattr.modify_date  = filattr.create_date;
        filattr.access_date  = filattr.create_date;
        filattr.eofblock     = efblk + 1;
        filattr.filattrflags = filattrflags;
        header -> areas[OZ_FS_HEADER_AREA_FILATTR].size = sizeof filattr;
        memcpy (header -> area + areaoffs, &filattr, sizeof filattr);
        break;
      }

      /* Fill in the security attributes, if any */

      case OZ_FS_HEADER_AREA_SECATTR: {
        header -> areas[OZ_FS_HEADER_AREA_SECATTR].size = secattrsize;
        memcpy (header -> area + areaoffs, secattrbuff, secattrsize);
        break;
      }

      /* Fill in the pointer(s), if any */

      case OZ_FS_HEADER_AREA_POINTER: {
        if (count != 0) {
          header -> areas[OZ_FS_HEADER_AREA_POINTER].size = sizeof (Pointer);
          ((Pointer *)(header -> area + areaoffs)) -> blockcount = count;
          ((Pointer *)(header -> area + areaoffs)) -> logblock   = start;
        }
        break;
      }
    }
    areaoffs += (header -> areas[i].size + 3) & -4;
  }

  /* Fill in the checksum */

  cksm = 0;
  for (i = 0; i < volex -> homeblock.blocksize / sizeof (uWord); i ++) {
    cksm -= ((uWord *)header)[i];
  }
  header -> checksum = cksm;

  /* Write the header to disk */

  sts = oz_dev_vdfs_writelogblock (index_header_start + filenum - 1, 0, volex -> homeblock.blocksize, header, 0, iopex);
  if (sts != OZ_SUCCESS) {
    oz_dev_vdfs_printk (iopex, "oz_dev_dfs init_volume: error %u writing %s initial header to block %u\n", sts, name, index_header_start + filenum - 1);
  }

  /* Free temp memory and return write status */

  OZ_KNL_PGPFREE (header);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Check to see if storage bitmap block has some bits allocated in it	*/
/*									*/
/*    Input:								*/
/*									*/
/*	cluster = cluster number represented by first bit of clusterbuff
/*	clusterbuff = buffer of storage bitmap bits			*/
/*	volume = volume block pointer					*/
/*	count = number of blocks that are allocated			*/
/*	start = starting logical block number of allocated blocks	*/
/*									*/
/*    Output:								*/
/*									*/
/*	bits set in clusterbuff for any allocated blocks		*/
/*									*/
/************************************************************************/

static void check_init_alloc (OZ_VDFS_Iopex *iopex, OZ_Dbn cluster, uLong *clusterbuff, OZ_VDFS_Volume *volume, OZ_Dbn count, OZ_Dbn start)

{
  uLong bitsincluster;
  OZ_Dbn clustercount, clusterstart;
  OZ_VDFS_Volex *volex;

  volex = volume -> volex;

  bitsincluster = volex -> homeblock.clusterfactor * volex -> homeblock.blocksize * 8;

  /* Get the clustercount and clusterstart corresponding to the given block count and start */

  clustercount = count;
  clusterstart = start;
  while (clusterstart % volex -> homeblock.clusterfactor != 0) {
    clusterstart --;
    clustercount ++;
  }
  while (clustercount % volex -> homeblock.clusterfactor != 0) clustercount ++;
  clustercount /= volex -> homeblock.clusterfactor;
  clusterstart /= volex -> homeblock.clusterfactor;

  /* If they are not within the clusterbuff, get out now */

  if (clusterstart + clustercount <= cluster) return;
  if (cluster + bitsincluster <= clusterstart) return;

  /* Ok, determine bit number within clusterbuff to start at */

  if (clusterstart >= cluster) clusterstart -= cluster;
  else {
    clustercount -= cluster - clusterstart;
    clusterstart  = 0;
  }

  /* Make sure it doesn't run off end */

  if (clusterstart + clustercount > bitsincluster) {
    clustercount = bitsincluster - clusterstart;
  }

  /* Set the bits as long as there are clusters to do */

  while (clustercount > 0) {
    if (!((1 << (clusterstart & 31)) & clusterbuff[clusterstart/32])) {
      clusterbuff[clusterstart/32] |= 1 << (clusterstart & 31);
      if (volex -> homeblock.clustersfree == 0) {
        oz_dev_vdfs_printk (iopex, "oz_dev_dfs check_init_alloc: clustersfree is zero\n");
      } else {
        volex -> homeblock.clustersfree --;
      }
    }
    clustercount --;
    clusterstart ++;
  }
}

/************************************************************************/
/*									*/
/*  Mount a volume							*/
/*									*/
/*    Output:								*/
/*									*/
/*	mount_volume = OZ_SUCCESS : successful				*/
/*	                     else : error status			*/
/*	*volume_r = volume pointer					*/
/*									*/
/************************************************************************/

static uLong dfs_mount_volume (OZ_VDFS_Devex *devex, OZ_VDFS_Volume *volume, uLong mountflags, OZ_VDFS_Iopex *iopex)

{
  Header *headerbuff;
  OZ_Dbn fno, vbn;
  OZ_VDFS_Fileid fileid;
  OZ_VDFS_Volex *volex;
  OZ_Secaccmsk secaccmsk;
  Pointer *pointer;
  uLong *blockbuff, i, sts;
  uWord cksm;

  /* Allocate volex (volume extension) struct */

  volex = OZ_KNL_PGPMALLOQ (((uByte *)&(volex -> homeblock)) + devex -> blocksize - (uByte *)volex);
  if (volex == NULL) return (OZ_EXQUOTAPGP);
  memset (volex, 0, ((uByte *)&(volex -> homeblock)) - (uByte *)volex);
  volume -> volex = volex;

  /* Calculate homeblock location */

  calc_home_block (volume);

  /* Read the homey and validate it */

  sts = oz_dev_vdfs_readlogblock (volume -> hb_logblock, 0, devex -> blocksize, &(volex -> homeblock), iopex);
  if (sts != OZ_SUCCESS) goto rtnerr;
  sts = OZ_BADHOMEBLKVER;
  if (volex -> homeblock.homeversion != HOMEBLOCK_VERSION) goto rtnerr;
  cksm = 0;
  for (i = 0; i < sizeof volex -> homeblock / sizeof (uWord); i ++) {
    cksm += ((uWord *)&(volex -> homeblock))[i];
  }
  sts = OZ_BADHOMEBLKCKSM;
  if (cksm != 0) goto rtnerr;
  if (volex -> homeblock.blocksize != devex -> blocksize) {
    oz_dev_vdfs_printk (iopex, "oz_dev_dfs mount: volume blocksize %u, disk blocksize %u\n", volex -> homeblock.blocksize, devex -> blocksize);
    sts = OZ_BADBLOCKSIZE;
    goto rtnerr;
  }

  /* Check the last write mount date.  If non-zero, volume was not dismounted, so rebuild index and storage bitmaps. */
  /* Also perform verification if OZ_FS_MOUNTFLAG_VERIFY set.                                                        */

  if (OZ_HW_DATEBIN_TST (volex -> homeblock.lastwritemount) || (mountflags & OZ_FS_MOUNTFLAG_VERIFY)) {
    if (OZ_HW_DATEBIN_TST (volex -> homeblock.lastwritemount)) {
      oz_dev_vdfs_printk (iopex, "oz_dev_dfs: volume %s mounted at %t was not dismounted\n", 
		volex -> homeblock.volname, volex -> homeblock.lastwritemount);
    }
    if (!(mountflags & OZ_FS_MOUNTFLAG_READONLY) || (mountflags & OZ_FS_MOUNTFLAG_VERIFY)) {

      /* Scan and fix the volume */

      sts = verify_volume ((mountflags & OZ_FS_MOUNTFLAG_READONLY) != 0, devex -> blocksize, devex -> totalblocks, iopex);
      if (sts != OZ_SUCCESS) goto rtnerr;

      /* Re-read the homeblock */

      sts = oz_dev_vdfs_readlogblock (volume -> hb_logblock, 0, devex -> blocksize, &(volex -> homeblock), iopex);
      if (sts != OZ_SUCCESS) goto rtnerr;
    }
  }

  /* If homeblock indicates WRITETHRU mode, force mountflags to do WRITETHRU mode so we only have to test one bit */

  if (volex -> homeblock.initflags & OZ_FS_INITFLAG_WRITETHRU) volume -> mountflags |= OZ_FS_MOUNTFLAG_WRITETHRU;

  /* Allocate directory block buffer (used by lookup_file, extend_file and remove_file routines) */

  volume -> dirblocksize  = volex -> homeblock.clusterfactor * volex -> homeblock.blocksize;
  volume -> clusterfactor = volex -> homeblock.clusterfactor;
  volex  -> dirblockbuff3 = OZ_KNL_PGPMALLOQ (volume -> dirblocksize * 3);
  if (volex -> dirblockbuff3 == NULL) {
    oz_dev_vdfs_printk (iopex, "oz_dev_dfs mount: no quota for %u byte dirblockbuff3\n", volume -> dirblocksize * 3);
    sts = OZ_EXQUOTAPGP;
    goto rtnerr;
  }

  /* Determine security access mask to open files with */

  secaccmsk = OZ_SECACCMSK_LOOK | OZ_SECACCMSK_READ | OZ_SECACCMSK_WRITE;
  if (mountflags & OZ_FS_MOUNTFLAG_READONLY) secaccmsk = OZ_SECACCMSK_LOOK | OZ_SECACCMSK_READ;

  /* Open the indexheaders file.  This must be the first file opened as it is how we figure out where all other file headers are. */

  fileid.num = SACRED_FIDNUM_INDEXHEADERS;
  fileid.seq = 0;
  fileid.rvn = 1;
  sts = oz_dev_vdfs_open_file (volume, &fileid, secaccmsk, &(volex -> indexheaders), iopex);
  if (sts != OZ_SUCCESS) goto rtnerr;

  /* Open the index bitmap file */

  fileid.num = SACRED_FIDNUM_INDEXBITMAP;
  fileid.seq = 0;
  fileid.rvn = 1;
  sts = oz_dev_vdfs_open_file (volume, &fileid, secaccmsk, &(volex -> indexbitmap), iopex);
  if (sts != OZ_SUCCESS) goto rtnerr;

  /* Open the storage bitmap file */

  fileid.num = SACRED_FIDNUM_STORAGEBITMAP;
  fileid.seq = 0;
  fileid.rvn = 1;
  sts = oz_dev_vdfs_open_file (volume, &fileid, secaccmsk, &(volex -> storagebitmap), iopex);
  if (sts != OZ_SUCCESS) goto rtnerr;

  /* Open the root directory file (we only need lookup access, though).  This is done as an optimisation */
  /* so we aren't constantly doing an open/close on the root directory for every lookup_file operation.  */

  fileid.num = SACRED_FIDNUM_ROOTDIRECTORY;
  fileid.seq = 0;
  fileid.rvn = 1;
  sts = oz_dev_vdfs_open_file (volume, &fileid, OZ_SECACCMSK_LOOK, &(volex -> rootdirectory), iopex);
  if (sts != OZ_SUCCESS) goto rtnerr;

  /* Now write the home block back if we are write enabled (with the current datebin) */

  if (!(mountflags & OZ_FS_MOUNTFLAG_READONLY)) {
    volex -> homeblock.lastwritemount = oz_hw_tod_getnow ();
    sts = dfs_writehomeblock (volume, iopex);
    if (sts == OZ_WRITELOCKED) {
      oz_dev_vdfs_printk (iopex, "oz_dev_dfs: volume is write-locked - proceeding in read-only mode\n");
      volume -> mountflags |= OZ_FS_MOUNTFLAG_READONLY;
    } else if (sts != OZ_SUCCESS) {
      oz_dev_vdfs_printk (iopex, "oz_dev_dfs: error %u marking volume dirty\n", sts);
      goto rtnerr;
    }
  }

  return (OZ_SUCCESS);

rtnerr:
  if (volex -> dirblockbuff3 != NULL) OZ_KNL_PGPFREE (volex -> dirblockbuff3);
  OZ_KNL_PGPFREE (volex);
  volume -> volex = NULL;
  return (sts);
}

/* Calculate volume's home block location */

static void calc_home_block (OZ_VDFS_Volume *volume)

{
  if (volume -> bb_logblock > 0) volume -> hb_logblock = volume -> bb_logblock - 1;	/* if there's room just before the boot block, put it there */
  else volume -> hb_logblock = volume -> bb_logblock + volume -> bb_nblocks;		/* otherwise, put it just after the boot block */
}

/* Set 'nbits' starting at 'bitno' in 'file' */

static uLong setbitsinfile (OZ_VDFS_File *file, uLong nbits, uLong bitno, OZ_VDFS_Iopex *iopex, uLong blocksize, uLong *blockbuff)

{
  uLong bitinblock, bitsinblock, sts;
  OZ_Dbn vbn;

  bitsinblock = blocksize * 8;
  while (nbits > 0) {
    vbn = (bitno / bitsinblock) + 1;
    sts = oz_dev_vdfs_readvirtblock (file, vbn, 0, blocksize, blockbuff, iopex, 0);
    if (sts != OZ_SUCCESS) {
      oz_dev_vdfs_printk (iopex, "oz_dev_dfs setbitsinfile: error %u reading vbn %u\n", sts, vbn);
      return (sts);
    }
    bitinblock = bitno % bitsinblock;
    while ((nbits > 0) && (bitinblock < bitsinblock)) {
      blockbuff[bitinblock/32] |= 1 << (bitinblock % 32);
      -- nbits;
      bitno ++;
      bitinblock ++;
    }
    sts = oz_dev_vdfs_writevirtblock (file, vbn, 0, blocksize, blockbuff, iopex, 0);
    if (sts != OZ_SUCCESS) {
      oz_dev_vdfs_printk (iopex, "oz_dev_dfs setbitsinfile: error %u writing vbn %u\n", sts, vbn);
      return (sts);
    }
  }
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Dismount volume							*/
/*									*/
/*    Input:								*/
/*									*/
/*	volume = volume to be dismounted				*/
/*	unload = 0 : leave volume online				*/
/*	         1 : unload volume (if possible)			*/
/*									*/
/*    Output:								*/
/*									*/
/*	volume dismounted						*/
/*									*/
/************************************************************************/

static uLong dfs_dismount_volume (OZ_VDFS_Devex *devex, OZ_VDFS_Volume *volume, int unload, int shutdown, OZ_VDFS_Iopex *iopex)

{
  OZ_VDFS_Volex *volex;
  uLong n, sts;

  volex = volume -> volex;
  if (volex == NULL) return (OZ_NOTMOUNTED);

  /* Make sure just our internal files are open */

  if (!shutdown) {
    n = 0;
    if (volex -> indexbitmap   != NULL) n ++;
    if (volex -> indexheaders  != NULL) n ++;
    if (volex -> rootdirectory != NULL) n ++;
    if (volex -> storagebitmap != NULL) n ++;
    if (volume -> nopenfiles != n) return (OZ_OPENFILESONVOL);
  }

  /* Close all those files - in shutdown mode there may be channels pointing */
  /* to them but having devex -> shutdown set will prevent all access        */

  while (volume -> openfiles != NULL) {
    oz_dev_vdfs_close_file (volume -> openfiles, iopex);
  }

  /* If we were write enabled, clear lastwritemount in the home block to indicate properly dismounted */

  if (!(volume -> mountflags & OZ_FS_MOUNTFLAG_READONLY)) {
    OZ_HW_DATEBIN_CLR (volex -> homeblock.lastwritemount);
    sts = dfs_writehomeblock (volume, iopex);
    if (sts != OZ_SUCCESS) oz_dev_vdfs_printk (iopex, "oz_dev_dfs dismount: error %u clearing lastwritemount date in header\n", sts);
  }

  /* Free off directory block buffer */

  if (volex -> dirblockbuff3 != NULL) {
    OZ_KNL_PGPFREE (volex -> dirblockbuff3);
    volex -> dirblockbuff3 = NULL;
  }

  /* Free off the volex struct */

  OZ_KNL_PGPFREE (volex);
  volume -> volex = NULL;

  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Get root directory's fileid						*/
/*									*/
/************************************************************************/

static uLong dfs_get_rootdirid (OZ_VDFS_Devex *devex, OZ_VDFS_Fileid *rootdirid_r)

{
  rootdirid_r -> num = SACRED_FIDNUM_ROOTDIRECTORY;
  rootdirid_r -> rvn = 1;
  rootdirid_r -> seq = 0;
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Get pointer to a file's fileid					*/
/*									*/
/************************************************************************/

static const OZ_VDFS_Fileid *dfs_get_fileid (OZ_VDFS_File *file)

{
  return (&(file -> filex -> header.fileid));
}

/************************************************************************/
/*									*/
/*  Lookup a file in a directory					*/
/*									*/
/*    Input:								*/
/*									*/
/*	dirfile = directory file					*/
/*	namelen = length of *name string				*/
/*	name    = name to lookup (not necessarily null terminated)	*/
/*									*/
/*    Output:								*/
/*									*/
/*	lookup_file = OZ_SUCCESS : successful				*/
/*	           OZ_NOSUCHFILE : entry not found			*/
/*	                    else : error status				*/
/*	*dirvbn_r = dir vbn that the entry was found in (or last vbn looked at)
/*	*fileid_r = file-id of found file				*/
/*	*name_r   = name found						*/
/*									*/
/*    Note:								*/
/*									*/
/*	This routine does not do wildcard scanning, it just finds a 	*/
/*	particular file (like for an 'open' type request).		*/
/*									*/
/************************************************************************/

static uLong lookup_file_scan (OZ_Dcmpb *dcmpb, uLong status);

static uLong dfs_lookup_file (OZ_VDFS_File *dirfile, int namelen, const char *name, OZ_VDFS_Fileid *fileid_r, char *name_r, OZ_VDFS_Iopex *iopex)

{
  OZ_Dbn dirvbn;

  return (lookup_file (dirfile, namelen, name, &dirvbn, fileid_r, name_r, iopex));
}

static uLong lookup_file (OZ_VDFS_File *dirfile, int namelen, const char *name, OZ_Dbn *dirvbn_r, OZ_VDFS_Fileid *fileid_r, char *name_r, OZ_VDFS_Iopex *iopex)

{
  char c, *p;
  OZ_VDFS_File *file;
  int cmp, naml;
  uLong i, sts;
  OZ_Event *event;
  OZ_VDFS_Filex *dirfilex;
  OZ_VDFS_Volex *volex;
  OZ_VDFS_Volume *volume;

  dirfilex = dirfile -> filex;
  volume   = dirfile -> volume;
  volex    = volume -> volex;

  if (!IS_DIRECTORY (dirfilex -> header)) return (OZ_FILENOTADIR);

  /* An null name string means looking up the directory itself */

  if (namelen == 0) {
    *fileid_r = dirfilex -> header.fileid;				/* return the fileid of the directory */
    if (name_r != NULL) *name_r = 0;					/* return a null name string */
    return (OZ_SUCCESS);						/* always successful */
  }

  /* Parse name into <fname>;<versign><version> */

  /* omitted, ;, ;0, ;-0 all refer to the very newest           */
  /* ;n means that exact version                                */
  /* ;-n means next to n'th newest, eg, ;-1 means second newest */

  naml = namelen;
  if (naml >= sizeof volex -> v.lf.fname) return (OZ_FILENAMETOOLONG);
  memcpy (volex -> v.lf.fname, name, naml);				/* copy the filename string */
  volex -> v.lf.fname[naml++] = 0;					/* null terminate and count the null in the length */
  p = strchr (volex -> v.lf.fname, ';');				/* scan given spec for version number */
  volex -> v.lf.version = 0;						/* default to looking for ;0, ie, the very newest version */
  volex -> v.lf.versign = 0;
  if (p != NULL) {							/* see if ; is present */
    *(p ++) = 0;							/* replace ; with a null */
    naml = p - volex -> v.lf.fname;					/* set new length excluding ;version but including terminating null */
    c = *p;								/* get first character following ; */
    if (c == '-') volex -> v.lf.versign = -1;				/* if -, set the flag */
    if (volex -> v.lf.versign != 0) c = *(++ p);			/* get character following the - */
    while ((c >= '0') && (c <= '9')) {					/* repeat as long as we have numeric characters */
      volex -> v.lf.version = volex -> v.lf.version * 10 + c - '0';	/* convert character and put in accumulator */
      c = *(++ p);							/* get next character in version string */
    }
    if (c != 0) return (OZ_BADFILEVER);					/* non-numeric character in version, error */
  }
  if (naml < 2) {
    oz_dev_vdfs_printk (iopex, "oz_dev_dfs lookup_file: name must contain at least one character in %*.*s\n", namelen, namelen, name);
    return (OZ_BADFILENAME);
  }

  /* Scan directory for the file */

  volex -> v.lf.nclusters = dirfile -> allocblocks / volex -> homeblock.clusterfactor; // get number of clusters in directory file
  if (volex -> v.lf.nclusters == 0) return (OZ_NOSUCHFILE);			// - that sure made the job easy
  volex -> v.lf.multiplier  = 1;						// start with middle
  volex -> v.lf.level       = 1;						// ... ie, cluster=(nclusters*1)>>1
  volex -> v.lf.dirfile     = dirfile;
  volex -> v.lf.fileid_r    = fileid_r;
  volex -> v.lf.name_r      = name_r;
  volex -> v.lf.namelen     = naml;
  volex -> v.lf.state       = LOOKUP_FILE_STATE_GATHERNAME;
  volex -> v.lf.lastbuckvbn = 0;
  volex -> v.lf.thisbuckvbn = ((volex -> v.lf.nclusters >> 1) * volex -> homeblock.clusterfactor) + 1;

  volex -> v.lf.dcmpb.virtblock = volex -> v.lf.thisbuckvbn;
  volex -> v.lf.dcmpb.nbytes    = volex -> homeblock.clusterfactor * volex -> homeblock.blocksize;
  volex -> v.lf.dcmpb.blockoffs = 0;						// start at beginning of block
  volex -> v.lf.dcmpb.entry     = lookup_file_scan;				// call this scanning routine
  volex -> v.lf.dcmpb.param     = iopex;					// this is its parameter
  volex -> v.lf.dcmpb.ix4kbuk   = 0;
  volex -> v.lf.status          = OZ_PENDING;					// assume async completion
  sts = oz_dev_vdfs_dcache_map_vbn_to_lbn (&(volex -> v.lf.dcmpb), dirfile);	// get lbn of first directory block to scan
  if (sts == OZ_SUCCESS) sts = oz_dev_vdfs_dcache_map_blocks (&(volex -> v.lf.dcmpb)); // process the request
  if (sts == OZ_STARTED) {
    event = iopex -> devex -> event;						// wait for async completion
    while (volex -> v.lf.state != LOOKUP_FILE_STATE_DONE) {
      oz_knl_event_waitone (event);
      oz_knl_event_set (event, 0);
    }
    sts = volex -> v.lf.status;
  }
  else if (sts == OZ_SUCCESS) sts = volex -> v.lf.status;			// get completion status
  *dirvbn_r = volex -> v.lf.dcmpb.virtblock;					// return last vbn looked at
  return (sts);									// all done
}

/* But the kettle's on the boil, and we're so easy called away */

/************************************************************************/
/*									*/
/*  This routine is called by the disk cache routines when a page of 	*/
/*  the directory is available						*/
/*									*/
/*    Input:								*/
/*									*/
/*	dcmpb -> virtblock = virtual block number of directory		*/
/*	dcmpb -> blockoffs = offset in block to start at (should be zero)
/*	dcmpb -> phypage   = cache page's physical page number		*/
/*	dcmpb -> pageoffs  = byte offset in the phypage of virtblock	*/
/*	dcmpb -> nbytes    = number of bytes that we have to work with	*/
/*	volex -> v.lf.fname = filename we're looking for		*/
/*	volex -> v.lf.nclusters = number of clusters in directory	*/
/*	volex -> v.lf.multiplier = multiplier factor for current cluster number
/*	volex -> v.lf.level = search level we're at			*/
/*	volex -> v.lf.dirfile = directory file				*/
/*	volex -> v.lf.fileid_r = where to return found file-id		*/
/*	volex -> v.lf.name_r = where to return resultant filename	*/
/*	volex -> v.lf.namelen = length of fname including null		*/
/*	volex -> v.lf.versign = 0: ;n form, -1: ;-n form		*/
/*	volex -> v.lf.version = version number (abs value)		*/
/*	volex -> v.lf.dcmpb = same as dcmpb				*/
/*	volex -> v.lf.nbytes_name_matched = how many bytes of fname matched on last page
/*	volex -> v.lf.state = current scanning state			*/
/*	volex -> v.lf.partialdirpnt = saved partial dirpnt when it crosses page boundary
/*									*/
/************************************************************************/

static uLong lookup_file_scan (OZ_Dcmpb *dcmpb, uLong status)

{
  char c, *dirpagebuff, *p;
  Dirpnt *dirpnt;
  int begofbucket, cmp;
  OZ_VDFS_Iopex *iopex;
  OZ_Dbn deltacluster;
  OZ_Pagentry savepte;
  uLong cmplen, dirpagesize, i, sts;
  OZ_VDFS_Volex *volex;
  OZ_VDFS_Volume *volume;

  iopex  = dcmpb -> param;
  volume = iopex -> devex -> volume;
  volex  = volume -> volex;

  /* Maybe this is the final call to finish up */

  if (status != OZ_PENDING) {
    if (volex -> v.lf.status == OZ_PENDING) volex -> v.lf.status = status;	// save completion status
    OZ_HW_MB;									// make sure main loop sees all written
    volex -> v.lf.state = LOOKUP_FILE_STATE_DONE;				// ... before it sees we're all done
    oz_knl_event_set (iopex -> devex -> event, 1);				// wake it up
    return (0);
  }

  /* We always work on block boundaries */

  if (dcmpb -> blockoffs != 0) oz_crash ("oz_dev_dfs lookup_file_scan: blockoffs %u", dcmpb -> blockoffs);
  if ((dcmpb -> nbytes % volex -> homeblock.blocksize) != 0) {
    oz_crash ("oz_dev_dfs lookup_file_scan: nbytes %u", dcmpb -> nbytes);
  }

  /* Map the cache page to virtual memory */

  dirpagesize  = dcmpb -> nbytes;						// this is how much we have to work with
  if (dirpagesize > volex -> homeblock.clusterfactor * volex -> homeblock.blocksize) oz_crash ("oz_dev_dfs lookup_file: nbytes %u", dirpagesize);
  dirpagebuff  = oz_hw_phys_mappage (dcmpb -> phypage, &savepte);		// map physical page to virtual memory
  dirpagebuff += dcmpb -> pageoffs;						// point to the first byte of 'virtblock'

  /* If we're at the beginning of a bucket, reset search state        */
  /* If we're in the middle of a bucket, retain previous search state */

  begofbucket = 0;
  if (((dcmpb -> virtblock - 1) % volex -> homeblock.clusterfactor) == 0) {
    volex -> v.lf.nbytes_name_gathered = -1;					// we need to get the 'same as' byte
    volex -> v.lf.dirpntbytesaved      = 0;					// no bytes saved in partialdirpnt yet
    volex -> v.lf.state = LOOKUP_FILE_STATE_GATHERNAME;				// start by gathering up a filename string
    if (dirpagebuff[0] != 1) {							// it must start with a complete filename
      oz_dev_vdfs_printk (iopex, "oz_dev_dfs lookup_file: vbn %u doesn't begin with a complete filename\n", dcmpb -> virtblock);
      volex -> v.lf.status = OZ_DIRCORRUPT;
      goto alldone;
    }
    begofbucket = 1;
  }

  /* Scan the directory page for correct filename */

check_state:
  switch (volex -> v.lf.state) {

    /* Gathering up a name string and putting it in v.lf.lastname.  Then, if we have it all, compare it to requested name string. */

    case LOOKUP_FILE_STATE_GATHERNAME: {
      if (volex -> v.lf.nbytes_name_gathered < 0) {						// if we need the 'same as' byte
        if (dirpagesize == 0) goto on_to_next_page;						// see if reached end-of-block
        -- dirpagesize;
        volex -> v.lf.nbytes_name_gathered = *(dirpagebuff ++);					// get 'same as' byte
        if (-- (volex -> v.lf.nbytes_name_gathered) < 0) goto on_to_next_bucket;		// if zero, hit end-of-bucket
      }
      do {
        if (volex -> v.lf.nbytes_name_gathered == sizeof volex -> v.lf.lastname) {		// check for buffer overflow
          oz_dev_vdfs_printk (iopex, "oz_dev_dfs lookup_file: vbn %u filename too long\n", dcmpb -> virtblock);
          volex -> v.lf.status = OZ_DIRCORRUPT;
          goto alldone;
        }
        if (dirpagesize == 0) goto on_to_next_page;						// see if reached end-of-page
        -- dirpagesize;										// if not, copy the char
      } while ((volex -> v.lf.lastname[volex->v.lf.nbytes_name_gathered++] = *(dirpagebuff ++)) != 0);
      volex -> v.lf.dirpntbytesaved = 0;							// we are at beg of dirpnts now
      cmp = strcmp (volex -> v.lf.fname, volex -> v.lf.lastname);				// compare the names
      if (cmp < 0) goto on_to_prev_bucket;							// if name .lt. directory, back up some
      if (cmp == 0) {
        volex -> v.lf.state = LOOKUP_FILE_STATE_MATCHVERSION;					// if name .eq. directory, find matching version
        if (!begofbucket) goto check_state;							// if not at beg of bucket, it can't have a same-named entry in prev bucket
        begofbucket = 0;									// (any other names we find in bucket aren't at the beginning)
        if (volex -> v.lf.thisbuckvbn == volex -> v.lf.lastbuckvbn + volex -> homeblock.clusterfactor) goto check_state; // if we are already in sequential mode, find matching version
        if (volex -> v.lf.thisbuckvbn <=  volex -> homeblock.clusterfactor) goto check_state;	// nothing prior to this bucket, find matching version
        if ((volex -> v.lf.versign == 0) && (volex -> v.lf.version > 0)) {			// see if looking for an explicit version number
          if (dirpagesize < sizeof (Dirpnt)) goto check_state;					// do it the hard way if we don't have a version to check
          if (((Dirpnt *)dirpagebuff) -> version >= volex -> v.lf.version) goto check_state;	// check the array if it's possible we're in there
        }
        volex -> v.lf.lastbuckvbn  = volex -> v.lf.thisbuckvbn;					// it might have started in bucket immediately before this one
        volex -> v.lf.thisbuckvbn -= volex -> homeblock.clusterfactor;
        goto read_bucket;
      }
      volex -> v.lf.state = LOOKUP_FILE_STATE_SKIPDIRPNTS;					// if name .gt. directory, skip all its versions
      begofbucket = 0;										// (any other names we find in bucket aren't at the beginning)
      // fall through ...
    }

    /* Skip what's left of a dirpnt array from last time, it terminates on an entry with a zero version number */

    case LOOKUP_FILE_STATE_SKIPDIRPNTS: {

      /* Optimise if there is nothing in partialdirpnt and the page contains a whole dirpnt */

      if ((volex -> v.lf.dirpntbytesaved == 0) && (dirpagesize >= sizeof (Dirpnt))) {
        dirpnt = (Dirpnt *)dirpagebuff;				// point to a complete dirpnt entry
        dirpagesize -= sizeof (Dirpnt);				// skip pointers past it
        dirpagebuff += sizeof (Dirpnt);
      }

      /* Either there is something in partialdirpnt or we have to put something in there */

      else {
        i = sizeof (Dirpnt) - volex -> v.lf.dirpntbytesaved;
        if (i > dirpagesize) i = dirpagesize;
        memcpy (((char *)&(volex -> v.lf.partialdirpnt)) + volex -> v.lf.dirpntbytesaved, dirpagebuff, i);
        volex -> v.lf.dirpntbytesaved += i;

        /* Read another directory page if we don't have a whole dirpnt yet */

        if (volex -> v.lf.dirpntbytesaved < sizeof (Dirpnt)) goto on_to_next_page;

        /* We have the whole thing, point to it */

        dirpagesize -= i;
        dirpagebuff += i;
        volex -> v.lf.dirpntbytesaved = 0;
        dirpnt = &(volex -> v.lf.partialdirpnt);
      }

      /* Stop skipping if we found a version zero entry, otherwise continue skipping */

      if (dirpnt -> version == 0) {				// see if it was version zero
        volex -> v.lf.state = LOOKUP_FILE_STATE_GATHERNAME;	// last one had version zero, so we're not skipping dirpnt array
        volex -> v.lf.nbytes_name_gathered = -1;		// now we start looking for the filename again
      }
      goto check_state;
    }

    /* Name matches, find matching dirpnt array entry             */
    /* omitted, ;, ;0, ;-0 all refer to the very newest           */
    /* ;n means that exact version                                */
    /* ;-n means next to n'th newest, eg, ;-1 means second newest */

    case LOOKUP_FILE_STATE_MATCHVERSION: {
      while (1) {

        /* Point dirpnt at entry to check and advance dirpagesize/dirpagebuff on to next entry */

        if ((volex -> v.lf.dirpntbytesaved == 0) && (dirpagesize >= sizeof (Dirpnt))) {		// see if wholly contained within dirpagebuff
          dirpnt = (Dirpnt *)dirpagebuff;							// if so, just point to it
          dirpagesize -= sizeof (Dirpnt);							// ... and advance dirpagebuff pointer
          dirpagebuff += sizeof (Dirpnt);
        } else {
          i = sizeof (Dirpnt) - volex -> v.lf.dirpntbytesaved;					// if not, see how much we need from dirpagebuff
          if (i > dirpagesize) i = dirpagesize;							// don't take more than it has
          memcpy (((char *)&(volex -> v.lf.partialdirpnt)) + volex -> v.lf.dirpntbytesaved, dirpagebuff, i);
          volex -> v.lf.dirpntbytesaved += i;							// we have this much more now
          if (volex -> v.lf.dirpntbytesaved < sizeof (Dirpnt)) goto on_to_next_page;		// go read more if we don't have enough yet
          dirpagesize -= i;									// ok, remove the last bytes from dirpagebuff
          dirpagebuff += i;
          dirpnt = &(volex -> v.lf.partialdirpnt);						// ... and point to temp dirpnt
          volex -> v.lf.dirpntbytesaved = 0;							// we're using it up so it's no longer saved
        }

        /* Check for end of dirpnt array indicated by a version number of zero.  If so, the version we want might be in the very next */
        /* bucket.  But don't bother checking if we're not at the end of this bucket or we've already looked in the very next bucket. */

        if (dirpnt -> version == 0) {								// maybe we're at end of dirpnt array
          if ((dirpagesize == 0) || (*dirpagebuff == 0)) {					// if so, see if we're at end of directory bucket
            if ((volex -> v.lf.versign < 0) || (volex -> v.lf.lastbuckvbn != volex -> v.lf.thisbuckvbn + volex -> homeblock.clusterfactor)) { // ... and we haven't already looked in next bucket
              volex -> v.lf.lastbuckvbn  = volex -> v.lf.thisbuckvbn;				// ... then go on to next bucket as it might have our version
              volex -> v.lf.thisbuckvbn += volex -> homeblock.clusterfactor;
              goto read_bucket;
            }
          }
          goto not_found;									// ... otherwise, the name is not found
        }

        /* See if we have found the correct version */

        if (volex -> v.lf.version == 0) goto found_it;				// finding ;-n form, and we've counted enough of them
        if (volex -> v.lf.versign < 0) volex -> v.lf.version --;		// skipping over one more for the ;-n form
        else {
          if (dirpnt -> version == volex -> v.lf.version) goto found_it;	// maybe we found an exact match for ;n form
          if (dirpnt -> version < volex -> v.lf.version) goto not_found;	// ... or maybe we aren't going to find it
        }
      }
    }

    /* Who knows what */

    default: oz_crash ("oz_dev_dfs lookup_file: bad state %d", volex -> v.lf.state);
  }

  /* Go on to next bucket in the directory */

on_to_next_bucket:
  if (volex -> v.lf.lastbuckvbn == volex -> v.lf.thisbuckvbn + volex -> homeblock.clusterfactor) {
    volex -> v.lf.lastbuckvbn  = volex -> v.lf.thisbuckvbn;
    volex -> v.lf.thisbuckvbn += volex -> homeblock.clusterfactor;
    goto read_bucket;
  }
  volex -> v.lf.multiplier += volex -> v.lf.multiplier + 1;
  goto on_to_bucket;

on_to_prev_bucket:
  if (volex -> v.lf.thisbuckvbn == volex -> v.lf.lastbuckvbn + volex -> homeblock.clusterfactor) goto not_found;
  volex -> v.lf.multiplier += volex -> v.lf.multiplier - 1;

on_to_bucket:
  volex -> v.lf.lastbuckvbn = volex -> v.lf.thisbuckvbn;			// save what bucket we've just finished
  volex -> v.lf.level ++;							// we're one level deeper into tree, now
#ifdef OZ_HW_TYPE_486
  asm ("\n"
       "movl %1,%%eax\n"	// get nclusters in %eax
       "mull %2\n"		// mult %eax by multiplier, put in %edx:%eax
       "movb %3,%%cl\n"		// get level in %cl
       "shrdl %%edx,%%eax\n"	// shift %edx:%eax right by %cl
       "mull %4\n"		// mult %eax by clusterfactor, put in %edx:%eax
       "incl %%eax\n"		// increment because vbn's are 1-based
       "movl %%eax,%0\n"	// store result in thisbuckvbn
       :  "=m" (volex -> v.lf.thisbuckvbn)		// %0
       :   "m" (volex -> v.lf.nclusters), 		// %1
           "m" (volex -> v.lf.multiplier), 		// %2
           "m" (volex -> v.lf.level), 			// %3
           "m" (volex -> homeblock.clusterfactor)	// %4
       : "eax", "ecx", "edx");
#else
  volex -> v.lf.thisbuckvbn = ((((uQuad)(volex -> v.lf.nclusters) * volex -> v.lf.multiplier) >> volex -> v.lf.level) 
                            * volex -> homeblock.clusterfactor) + 1;
#endif
  if (volex -> v.lf.thisbuckvbn == volex -> v.lf.lastbuckvbn) goto not_found;
read_bucket:
  volex -> v.lf.dcmpb.virtblock = volex -> v.lf.thisbuckvbn;
  goto read_again;

  /* Go on to next page in the same directory block */

on_to_next_page:
  dcmpb -> virtblock += dcmpb -> nbytes / volex -> homeblock.blocksize;		// increment block number by how much we got
										// nbytes is always a multiple of blocksize

  /* Start reading 'dcmpb.virtblock' from disk.  Call myself back when it's available. */

read_again:
  oz_hw_phys_unmappage (savepte);						// unmap the old cache page
  dcmpb -> nbytes = volex -> homeblock.clusterfactor * volex -> homeblock.blocksize; // map a whole cluster at a time if possible
  sts = oz_dev_vdfs_dcache_map_vbn_to_lbn (dcmpb, volex -> v.lf.dirfile);	// get lbn of directory page to scan
  if (sts != OZ_SUCCESS) {							// if ok, return to start reading
    if (sts == OZ_ENDOFFILE) sts = OZ_NOSUCHFILE;
    volex -> v.lf.status = sts;							// map failed, return error status
    dcmpb -> nbytes = 0;							// we don't want any more pages read from disk
  }
  return (0);									// we (hopefully) didn't modify the page at all

  /* Correct entry will not be found */

not_found:
  volex -> v.lf.status = OZ_NOSUCHFILE;
  goto alldone;

  /* Correct entry has been found */

found_it:
  *(volex -> v.lf.fileid_r) = dirpnt -> fileid;				// return the file-id
  if (volex -> v.lf.name_r != NULL) {					// see if caller wants resultant name string
    memcpy (volex -> v.lf.name_r, volex -> v.lf.fname, volex -> v.lf.namelen); // if so, copy out name w/out version
    if (volex -> v.lf.fname[volex -> v.lf.namelen-2] != '/') {		// see if it is a directory name
      volex -> v.lf.name_r[volex -> v.lf.namelen-1] = ';';		// if not, append ;version
      oz_hw_itoa (dirpnt -> version, 
                  FILENAME_MAX - volex -> v.lf.namelen, 
                  volex -> v.lf.name_r + volex -> v.lf.namelen);
    }
  }
  volex -> v.lf.status = OZ_SUCCESS;					// successful

alldone:
  dcmpb -> nbytes = 0;							// we don't want any more pages read from disk
  oz_hw_phys_unmappage (savepte);					// unmap the cache page
  return (0);								// we (hopefully) didn't modify the page at all
}

/************************************************************************/
/*									*/
/*  Enter a file in a directory						*/
/*									*/
/*    Input:								*/
/*									*/
/*	dirfile    = directory file					*/
/*	dirname    = directory name (diag only)				*/
/*	namelen    = length of name to enter				*/
/*	name       = name to enter					*/
/*	newversion = make sure name is the highest version		*/
/*	file       = open file pointer (or NULL if not open)		*/
/*	fileid     = the file's id					*/
/*									*/
/*    Output:								*/
/*									*/
/*	enter_file = OZ_SUCCESS : successful				*/
/*	                   else : error status				*/
/*	*name_r = filled in with resultant name (incl version)		*/
/*									*/
/************************************************************************/

static uLong dfs_enter_file (OZ_VDFS_File *dirfile, const char *dirname, int namelen, const char *name, int newversion, OZ_VDFS_File *file, const OZ_VDFS_Fileid *fileid, char *name_r, OZ_VDFS_Iopex *iopex)

{
  uByte *dirblockbuff, *dirblockbuff2, *dirblockbuff3;
  char c, fname[FILENAME_MAX], lastname[FILENAME_MAX], nextname[FILENAME_MAX], *p, vstr[12];
  Dirpnt dirpnt[2];
  OZ_VDFS_File *entfile;
  OZ_VDFS_Filex *extfilex;
  int casx, cmp, i, j, k, l, match_last, match_next, n, prev_cmp;
  OZ_Dbn direfblk, dirvbn, extendsize, extendvbn;
  OZ_Dbn prev_vbn;
  OZ_VDFS_Fileid dummy_fileid;
  uLong adj_from, adj_to, dirblocksize, sts, v, version, vl, writesize;
  uLong prev_i, prev_j, prev_k;
  OZ_VDFS_Volex *volex;
  OZ_VDFS_Volume *volume;

  volume = dirfile -> volume;
  volex  = volume -> volex;

  if (name_r != NULL) *name_r = 0;
  vstr[0]  = 0;
  casx     = 0;
  adj_from = 0;
  adj_to   = 0;

  /* Must be a directory we are entering into */
  /* Cannot enter a sacred file, either       */

  if (!IS_DIRECTORY (dirfile -> filex -> header)) return (OZ_FILENOTADIR);
  if (fileid -> seq == 0) return (OZ_SACREDFILE);

  /* Make sure they give us sane stuff and count length including trailing null */
  /* Only allow printable characters, and allow a slash only as the last char   */
  /* Don't allow wildcard characters in the filename, either (stupid nerds)     */

  if (namelen >= sizeof fname) return (OZ_FILENAMETOOLONG);
  for (i = 0; i < namelen; i ++) {
    c = name[i];
    if ((c < ' ') || (c >= 127)) {
      oz_dev_vdfs_printk (iopex, "oz_dev_dfs enter_file: can't have non-printable character (0x%2.2X) in a filename\n", ((uLong)c) & 0xFF);
      return (OZ_BADFILENAME);
    }
    if ((c == '*') || (c == '?') || (c == '~')) {
      oz_dev_vdfs_printk (iopex, "oz_dev_dfs enter_file: can't have wildcard character '%c' in a filename\n", c);
      return (OZ_BADFILENAME);
    }
    if ((c == '(') || (c == ')')) {
      oz_dev_vdfs_printk (iopex, "oz_dev_dfs enter_file: can't have parentheses in a filename (looks like secattrs)\n");
      return (OZ_BADFILENAME);
    }
    if ((c == '/') && (i != namelen - 1)) {
      oz_dev_vdfs_printk (iopex, "oz_dev_dfs enter_file: can't have embedded '/'s in a filename\n");
      return (OZ_BADFILENAME);
    }
  }
  memcpy (fname, name, namelen);
  fname[namelen] = 0;

  /* Don't allow a null name */

  if ((namelen == 0) || ((namelen == 1) && (fname[0] == '/'))) {
    oz_dev_vdfs_printk (iopex, "oz_dev_dfs enter_file: can't have a null filename\n");
    return (OZ_BADFILENAME);
  }

  /* Don't allow name of '.', './', '..' or '../' so they can't create files named as such, because we use those names specially */

  if ((fname[0] == '.') && ((namelen == 1) || (fname[1] == '/') || ((fname[1] == '.') && ((namelen == 2) || (fname[2] == '/'))))) {
    oz_dev_vdfs_printk (iopex, "oz_dev_dfs enter_file: can't have ., ./ or ../ as name in %*.*s\n", namelen, namelen, name);
    return (OZ_BADFILENAME);
  }

  /* Have 'namelen' include the trailing null */

  namelen ++;

  /* Open the file to be entered so we can increment its dircount - this also validates its fileid */

  entfile = file;
  if (entfile == NULL) {
    sts = oz_dev_vdfs_open_file (volume, fileid, 0, &entfile, iopex);
    if (sts != OZ_SUCCESS) goto cleanup;
  }

  /* If the file being entered is a directory then the name must end in a slash, otherwise it must not */

  sts = OZ_BADFILENAME;
  if (IS_DIRECTORY (entfile -> filex -> header) ^ (name[namelen-2] == '/')) goto cleanup;

  /* Get version number (only allow nothing, ;, ;0, ;n (no +/- stuff)) */

  p = strchr (fname, ';');
  version = 0;
  if (p != NULL) {
    *(p ++) = 0;
    namelen = p - fname;
    c = *p;
    while ((c >= '0') && (c <= '9')) {
      version = version * 10 + c - '0';
      c = *(++ p);
    }
    sts = OZ_BADFILEVER;
    if (c != 0) goto cleanup;
  }
  memset (dirpnt, 0, sizeof dirpnt);
  dirpnt[0].version = version;
  dirpnt[0].fileid  = *fileid;
  sts = OZ_BADFILENAME;
  if (namelen < 2) goto cleanup;

  /* Directories are always version 1 */

  /* Note that the above stuff precludes specifying a version number */
  /* with a directory, eg, you can't give dirname;5/ or dirname/;5   */

  if (IS_DIRECTORY (entfile -> filex -> header)) {
    dirpnt[0].version = version = 1;
    newversion = 0;
  }

  /* Look in directory for a spot to enter it */

  dirblocksize  = volume -> dirblocksize;					/* get the 'block size' = one cluster at a time */
  dirblockbuff  = volex -> dirblockbuff3;					/* point to block buffer for reading the directory */
  dirblockbuff2 = dirblockbuff  + dirblocksize;					/* ... plus two extras for extending it */
  dirblockbuff3 = dirblockbuff2 + dirblocksize;

  direfblk = dirfile -> allocblocks + 1;					/* compute eof pointer = very end of the file */

  dirvbn = 1;
  if (direfblk == 1) {
    if (dirpnt[0].version == 0) dirpnt[0].version = 1;
    dirblockbuff[0] = 1;							/* directory is empty, create a new block with this one entry */
    memcpy (dirblockbuff + 1, fname, namelen);
    memcpy (dirblockbuff + 1 + namelen, dirpnt, sizeof dirpnt);
    memset (dirblockbuff + 1 + namelen + sizeof dirpnt, 0, dirblocksize - 1 - namelen - sizeof dirpnt);
    casx = 1;
    extendsize = 1;
    extendvbn  = 1;
    writesize  = dirblocksize;
    oz_hw_itoa (dirpnt[0].version, sizeof vstr, vstr);
    if (namelen + strlen (vstr) + 1 >= FILENAME_MAX) {
      sts = OZ_FILENAMETOOLONG;
      goto cleanup;
    }
    adj_from = 0;
    adj_to   = 1 + namelen + sizeof dirpnt;
    goto write_dir_blocks;
  }

  if (direfblk >= volex -> homeblock.clusterfactor * 8) {					// check for hefty sized directory
    sts = lookup_file (dirfile, namelen - 1, fname, &dirvbn, &dummy_fileid, NULL, iopex);	// if so, find insertion spot quickly
    if ((sts != OZ_SUCCESS) && (sts != OZ_NOSUCHFILE)) goto cleanup;				// abort if some bad error
    dirvbn = (dirvbn - 1) / volex -> homeblock.clusterfactor;					// get zero-based bucket number
    if (dirvbn > 0) -- dirvbn;									// back up in case name fits in prior bucket
    dirvbn = (dirvbn * volex -> homeblock.clusterfactor) + 1;					// get one-based block number
  }

  prev_cmp = -1;								/* say prev name was .lt. new name */
  prev_vbn = 0;									/* it wasn't really in any block */
  prev_i   = dirblocksize;							/* say the prev block was completely full */
										/* j and k aren't really valid when prev_cmp .lt. 0 */

  while (1) {									/* loop through the whole directory */

    /* Read in block from directory file */

    sts = oz_dev_vdfs_readvirtblock (dirfile, dirvbn, 0, dirblocksize, dirblockbuff, iopex, 0); /* read a whole dirblockbuff */
    if (sts != OZ_SUCCESS) {
      oz_dev_vdfs_printk (iopex, "oz_dev_dfs enter_file: error %u reading directory block %u\n", sts, dirvbn);
      goto cleanup;
    }

    /* Validate existing directory block contents */

    if (!validirbuf (dirblocksize, dirblocksize, dirblockbuff, iopex)) {
      oz_dev_vdfs_printk (iopex, "oz_dev_dfs enter_file: existing directory block %u corrupt\n", dirvbn);
      sts = OZ_DIRCORRUPT;
      goto cleanup;
    }

    /* Scan through the block */

    lastname[0] = 0;
    for (i = 0; i < dirblocksize;) {						/* scan the dirblockbuff */
      if (dirblockbuff[i] == 0) break;						/* null filename string means end of block */
      j = i + strlen (dirblockbuff + i) + 1;					/* get index to dirpnt array for this filename */
      memcpy (nextname + dirblockbuff[i] - 1, dirblockbuff + i + 1, j - i - 1);	/* make up the complete filename string */
      k = j;									/* set dirpnt index to first dirpnt array element */

      /* Compare name in directory block to name being entered */

      cmp = strcmp (nextname, fname);

      /* If name in dir is .gt. name being entered, enter the new name just before this name */

      if (cmp > 0) goto foundspotgt;

      /* If name in dir is .eq. name being entered, enter the new name */
      /* in this one's array of versions by descending version number  */

      if (cmp == 0) {

        /* If no version was specified, make the version */
        /* one greater than the current highest version. */
        /* Make sure the incrementing doesn't overflow.  */

        if ((version == 0) || newversion) {					/* see if they specified ;0 or equiv */
          v = ((Dirpnt *)(dirblockbuff + j))[0].version;			/* set up version = lastest existing version + 1 */
          if (dirpnt[0].version <= v) dirpnt[0].version = v + 1;		/* ... unless caller supplied a higher number */
          if (dirpnt[0].version != 0) goto foundspoteq;				/* if it didn't overflow, enter it */
          sts = OZ_BADFILEVER;							/* bad version number if overflow */
          goto cleanup;
        }

        /* A specific version was specified, scan through the array until     */
        /* we find one that is lower than what was specified.  If we don't    */
        /* find one in this array, we look at the next spec in the directory. */
        /* If we find an equal one, that is an error.                         */

        while ((k < dirblocksize) && ((v = ((Dirpnt *)(dirblockbuff + k)) -> version) != 0)) { /* scan version array until we hit version 0 */
          if (v  < version) goto foundspoteq;					/* stop if dirpnt version .lt. new file's version */
										/* (so we enter the new file just before it) */
          if (v == version) {							/* if exact match, error */
            sts = OZ_FILEALREADYEXISTS;
            goto cleanup;
          }
          k += sizeof (Dirpnt);							/* dirpnt version .gt. new file version, check out next dirpnt */
        }

        /* We didn't find a lower entry, save pointer to end of this */
        /* array.  If the name in the next entry in the directory is */
        /* .eq. the name being entered, we continue scanning its     */
        /* version array.  If it is .gt. name being entered, then we */
        /* want to insert the new one at the spot being saved here.  */

        prev_cmp = 0;
        prev_i   = i;
        prev_j   = j;
        prev_k   = k;
        prev_vbn = dirvbn;
      }

      /* Skip 'i' forward to next filename in the directory block */

      i = k;
      do i += sizeof (Dirpnt);
      while ((i < dirblocksize) && (((Dirpnt *)(dirblockbuff + i))[-1].version != 0));

      /* Save the last name we compared */

      strcpy (lastname, nextname);
    }

    /* If prev thing in this directory block was .lt. name being entered,  */
    /* save index at end of block where we might insert new name (provided */
    /* the first name of the next block is .gt. name being entered).       */

    if (cmp < 0) {
      prev_cmp = -1;
      prev_i   = i;
      prev_vbn = dirvbn;
    }

    /* If we hit end of directory, pretend we just found */
    /* a name that is .gt. the name being entered.       */

    if (dirvbn + volex -> homeblock.clusterfactor == direfblk) goto foundspotgt; /* if this is the prev block, insert here */

    /* Otherwise, continue on with the next block in the directory */

    dirvbn += volex -> homeblock.clusterfactor;				/* otherwise, go on to read next block */
  }

  /*****************************************************************/
  /* Found name in directory greater than name to be entered       */
  /*   dirvbn = virtual block number to enter it                   */
  /*   i = offset in dirvbn where .gt. filename string starts      */
  /*   prev_cmp == 0 : prev entry had matching filename            */
  /*             < 0 : prev entry's filename didn't match either   */
  /*   prev_i = start of prev entry                                */
  /*   prev_j = start of prev entry's dirpnts                      */
  /*   prev_k = end of prev entry's dirpnts                        */
  /*   prev_vbn = prev entry's vbn                                 */
  /*****************************************************************/

foundspotgt:
#if 000
  oz_knl_printk ("oz_dev_dfs enter_file*: fname %s\n", fname);
  oz_knl_printk ("                       dirvbn %u\n", dirvbn);
  oz_knl_printk ("                            i %d\n", i);
  oz_knl_printk ("                     prev_cmp %d\n", prev_cmp);
  oz_knl_printk ("                       prev_i %d\n", prev_i);
  oz_knl_printk ("                       prev_j %d\n", prev_j);
  oz_knl_printk ("                       prev_k %d\n", prev_k);
  oz_knl_printk ("                     prev_vbn %u\n", prev_vbn);
  oz_knl_printk ("                     lastname %s\n", lastname);
  oz_knl_printk ("                     nextname %s\n", nextname);
  oz_knl_dumpmem2 (dirblocksize, dirblockbuff, dirvbn << 16);
#endif

  /* Maybe this is really the oldest (lowest numbered) version of the prev entry */

  /* prev_cmp .eq. 0 indicates that new name matched prev directory entry */

  if (prev_cmp == 0) {
    casx = 2;
    i = prev_i;									/* offset of name in prev directory block */
    j = prev_j;									/* offset of version array in prev directory block */
    k = prev_k;									/* index in version array to insert new entry */
    dirvbn = prev_vbn;								/* previous directory block number */
    sts = oz_dev_vdfs_readvirtblock (dirfile, dirvbn, 0, dirblocksize, dirblockbuff, iopex, 0);
    if (sts == OZ_SUCCESS) goto foundspoteq;
    oz_dev_vdfs_printk (iopex, "oz_dev_dfs enter_file: error %u reading directory block %u\n", sts, dirvbn);
    goto cleanup;
  }

  /* It is a completely new name - make sure we have a version number and make sure result doesn't overflow filename string */

  if (dirpnt[0].version == 0) dirpnt[0].version = 1;

  oz_hw_itoa (dirpnt[0].version, sizeof vstr, vstr);
  sts = OZ_FILENAMETOOLONG;
  if (namelen + strlen (vstr) + 1 >= FILENAME_MAX) goto cleanup;

  /* Find out how much of it matches the last and next entries */

  for (match_last = 0;; match_last ++) if (fname[match_last] != lastname[match_last]) break;
  for (match_next = 0;; match_next ++) if (fname[match_next] != nextname[match_next]) break;

  /* If at offset 0 in this block, maybe it will fit on end of previous block            */
  /* This keeps us from unnecessarily extending directory when adding entries on the end */

  /* i .eq. 0 means we are trying to insert at beginning of this block */
  /* prev_i is how much room is already used up in previous block      */

  if ((i == 0) && (prev_i + namelen - match_last + 1 + sizeof dirpnt <= dirblocksize)) {
    casx = 3;
    dirvbn = prev_vbn;
    sts = oz_dev_vdfs_readvirtblock (dirfile, dirvbn, 0, dirblocksize, dirblockbuff, iopex, 0);
    if (sts != OZ_SUCCESS) {
      oz_dev_vdfs_printk (iopex, "oz_dev_dfs enter_file: error %u reading directory block %u\n", sts, dirvbn);
      goto cleanup;
    }
    i = prev_i;
  }

  /* Set 'j' to number of bytes in current directory block that are used */

  for (j = i; (j < dirblocksize) && (dirblockbuff[j] != 0);) {
    j += strlen (dirblockbuff + j) + 1;
    do j += sizeof (Dirpnt);
    while ((j < dirblocksize) && (((Dirpnt *)(dirblockbuff + j))[-1].version != 0));
  }

  /* If there is no next name in the block, insert at end of block */

  if (j == i) goto ins_at_eob;

  /* See if there is enough room in current block to insert it */

  if (j + namelen - match_last + 1 + sizeof dirpnt + dirblockbuff[i] - 1 - match_next <= dirblocksize) {
    casx = 4;
    extendsize = 0;								/* ok, no extension required */
    memmove (dirblockbuff + i + 1 + namelen - match_last + sizeof dirpnt + 1, 
             dirblockbuff + i + 1 + match_next + 1 - dirblockbuff[i], 
             j - i - (1 + match_next + 1 - dirblockbuff[i]));
    dirblockbuff[i] = match_last + 1;
    memcpy (dirblockbuff + i + 1, fname + match_last, namelen - match_last);
    memcpy (dirblockbuff + i + 1 + namelen - match_last, dirpnt, sizeof dirpnt);
    dirblockbuff[i+1+namelen-match_last+sizeof dirpnt] = match_next + 1;
    adj_from = i;								/* shift any directory reads on this block */
    adj_to   = i + 1 + namelen - match_last + sizeof dirpnt;			/* ... so they point to the same name */
  }

  /* See if name will fit in old block with just the stuff that comes before it */
  /* Stuff that comes after it gets placed in a new block                       */

  else if (i + 1 + namelen - match_last + sizeof dirpnt <= dirblocksize) {
    casx = 5;
    extendsize = 1;								/* ok, extend to create second block */
    dirblockbuff2[0] = 1;							/* put part that follows in second block */
    memcpy (dirblockbuff2 + 1, nextname, dirblockbuff[i] - 1);
    memcpy (dirblockbuff2 + 1 + dirblockbuff[i] - 1, dirblockbuff + i + 1, j - i - 1);
    memset (dirblockbuff2 + 1 + dirblockbuff[i] - 1 + j - i - 1, 0, 		/* zero fill second block */
            dirblocksize - (1 + dirblockbuff[i] - 1 + j - i - 1));
    dirblockbuff[i] = match_last + 1;						/* copy in new name */
    memcpy (dirblockbuff + i + 1, fname + match_last, namelen - match_last);
    memcpy (dirblockbuff + i + 1 + namelen - match_last, dirpnt, sizeof dirpnt); /* copy in new dirpnt followed by a null dirpnt */
    k = i + 1 + namelen - match_last + sizeof dirpnt;				/* get index past the new entry */
    if (k < j) memset (dirblockbuff + k, 0, j - k);				/* zero fill over the old stuff that was there */
    adj_from = i;								/* shift any directory reads on old block to new block */
    adj_to   = dirblocksize;
  }

  /* If not, see if it will fit in a new block with the stuff that comes after it */

  else if (1 + namelen + sizeof dirpnt + 1 + j - i - 1 - (match_next + 1 - dirblockbuff[i]) <= dirblocksize) {
    casx = 6;
    extendsize = 1;								/* ok, extend to create second block */
    dirblockbuff2[0] = 1;
    memcpy (dirblockbuff2 + 1, fname, namelen);
    memcpy (dirblockbuff2 + 1 + namelen, dirpnt, sizeof dirpnt);
    dirblockbuff2[1+namelen+sizeof dirpnt] = match_next + 1;
    memcpy (dirblockbuff2 + 1 + namelen + sizeof dirpnt + 1, 
            dirblockbuff + i + 1 + match_next + 1 - dirblockbuff[i], 
            j - (i + 1 + match_next + 1 - dirblockbuff[i]));
    memset (dirblockbuff2 + 1 + namelen + sizeof dirpnt + j + 1 - (i + 1 + match_next + 1 - dirblockbuff[i]), 0, 
            dirblocksize - (1 + namelen + sizeof dirpnt + j + 1 - (i + 1 + match_next + 1 - dirblockbuff[i])));
    memset (dirblockbuff + i, 0, j - i);					/* wipe stuff from first block */
    adj_from = i;								/* shift any directory reads on old block to new block */
    adj_to   = dirblocksize + 1 + namelen + sizeof dirpnt;
  }

  /* Put it in a block all by itself */

  else {
    casx = 7;
    extendsize = 2;								/* extend to create second and third blocks */
    dirblockbuff3[0] = 1;							/* copy part that follows to third block */
    memcpy (dirblockbuff3 + 1, nextname, dirblockbuff[i] - 1);
    memcpy (dirblockbuff3 + 1 + dirblockbuff[i] - 1, dirblockbuff + i + 1, j - i - 1);
    memset (dirblockbuff3 + 1 + dirblockbuff[i] - 1 + j - i - 1, 0, dirblocksize - (1 + dirblockbuff[i] - 1 + j - i - 1));
    dirblockbuff2[0] = 1;							/* put new name in second block by itself */
    memcpy (dirblockbuff2 + 1, fname, namelen);
    memcpy (dirblockbuff2 + 1 + namelen, dirpnt, sizeof dirpnt);
    memset (dirblockbuff2 + 1 + namelen + sizeof dirpnt, 0, dirblocksize - (1 + namelen + sizeof dirpnt));
    memset (dirblockbuff + i, 0, j - i);					/* clear junk out of first block */
    adj_from = i;								/* shift any directory reads on old block to second new block */
    adj_to   = 2 * dirblocksize;
  }
  goto write_dir_blocks_ex;

  /* Name is being inserted at the end of the block */
  /* i = offset to end of bucket                    */

ins_at_eob:

  /* Maybe it just fits at the end of the block */

  if (i + 1 + namelen - match_last + sizeof dirpnt <= dirblocksize) {
    casx = 20;
    extendsize = 0;
    dirblockbuff[i] = match_last + 1;
    memcpy (dirblockbuff + i + 1, fname + match_last, namelen - match_last);
    memcpy (dirblockbuff + i + 1 + namelen - match_last, dirpnt, sizeof dirpnt);
    adj_from = i;
    adj_to   = i + namelen - match_last + sizeof dirpnt;
  }

  /* Put it in its own block */

  else {
    casx = 21;
    extendsize = 1;
    dirblockbuff2[0] = 1;
    memcpy (dirblockbuff2 + 1, fname, namelen);
    memcpy (dirblockbuff2 + 1 + namelen, dirpnt, sizeof dirpnt);
    memset (dirblockbuff2 + 1 + namelen + sizeof dirpnt, 0, dirblocksize - (1 + namelen + sizeof dirpnt));
    adj_from = i;
    adj_to   = dirblocksize + 1 + namelen + sizeof dirpnt;
  }
  goto write_dir_blocks_ex;

  /********************************************************************/
  /* The filename is already in the directory with different versions */
  /*   dirvbn = virtual block number to enter it                      */
  /*   i = offset in dirvbn where .eq. filename string starts         */
  /*   j = offset in dirvbn where its first pointer is                */
  /*   k = index of pointers where next lower version is              */
  /*   prev_cmp == 0 : prev entry had matching filename               */
  /*             < 0 : prev entry's filename didn't match             */
  /*   prev_i = start of prev entry                                   */
  /*   prev_j = start of prev entry's dirpnts                         */
  /*   prev_k = index of end of prev entry's dirpnts (the 0 entry)    */
  /*   prev_vbn = prev entry's vbn                                    */
  /********************************************************************/

foundspoteq:

  oz_hw_itoa (dirpnt[0].version, sizeof vstr, vstr);
  sts = OZ_FILENAMETOOLONG;
  if (namelen + strlen (vstr) + 1 >= FILENAME_MAX) goto cleanup;

  /* If there is room in prev_vbn's block (and it can go there) do so */

  /* prev_cmp .eq. 0 means the previous entry matched new entry's name */
  /* k .eq. j means we can insert new entry at beginning of this entry */
  /*   implying it can be entered at end of previous entry             */
  /* i .eq. 0 means this is at beginning of block, so prev must have   */
  /*   been at end of its block                                        */

  if ((prev_cmp == 0) && (k == j) && (i == 0) && (prev_k + sizeof dirpnt <= dirblocksize)) {
    casx = 8;
    dirvbn = prev_vbn;
    sts = oz_dev_vdfs_readvirtblock (dirfile, dirvbn, 0, dirblocksize, dirblockbuff, iopex, 0);
    if (sts != OZ_SUCCESS) {
      oz_dev_vdfs_printk (iopex, "oz_dev_dfs enter_file: error %u reading directory block %u\n", sts, dirvbn);
      goto cleanup;
    }
    memcpy (dirblockbuff + prev_k, dirpnt, sizeof dirpnt);
    adj_from = prev_k;
    adj_to   = prev_k + sizeof dirpnt;
    extendsize = 0;
    goto write_dir_blocks_ex;
  }

  /* Get offset to next name entry in directory block */

  for (n = k + sizeof (Dirpnt); n < dirblocksize; n += sizeof (Dirpnt)) {
    if (((Dirpnt *)(dirblockbuff + n))[-1].version == 0) break;
  }

  /* Get total length of all stuff in directory block */

  for (l = n; (l < dirblocksize) && (dirblockbuff[l] != 0);) {
    l += strlen (dirblockbuff + l) + 1;
    do l += sizeof (Dirpnt);
    while ((l < dirblocksize) && (((Dirpnt *)(dirblockbuff + l))[-1].version != 0));
  }

  /* Assuming we are entering 'mno;5' : */
  
  /* def 5 4 0  mno 8 7 6 4 3 0  pqr 2 0  <zeroes> */
  /*            i   j     k      n        l        */

  /* See if there is enough room in current block to insert new pointer */

  if (l + sizeof dirpnt[0] <= dirblocksize) {
    casx = 9;
    memmove (dirblockbuff + k + sizeof dirpnt[0], dirblockbuff + k, l - k);	/* ok, move rest of stuff to make room for dirpnt[0] */
    memcpy (dirblockbuff + k, dirpnt, sizeof dirpnt[0]);			/* copy in dirpnt[0] which has new file's version and file-id */
    extendsize = 0;								/* no extension required */
    adj_from = k;								// get existing offset of mno;4
    adj_to   = k + sizeof dirpnt[0];						// new offset of mno;4
  }

  /* See if version will fit in old block with just the stuff that comes before it */

  else if (k + sizeof dirpnt <= dirblocksize) {
    extendsize = 1;								/* extend to create second block */
    if (((Dirpnt *)(dirblockbuff + k))[0].version == 0) {			/* see if being inserted at end of dirpnt array */
      casx = 10;
      dirblockbuff2[0] = 1;
      memcpy (dirblockbuff2 + 1, fname, dirblockbuff[n] - 1);
      memcpy (dirblockbuff2 + 1 + dirblockbuff[n] - 1, dirblockbuff + n + 1, l - n - 1);
      memset (dirblockbuff2 + 1 + dirblockbuff[n] - 1 + l - n - 1, 0, 
              dirblocksize - (1 + dirblockbuff[n] - 1 + l - n - 1));
      memcpy (dirblockbuff + k, dirpnt, sizeof dirpnt);
      memset (dirblockbuff + k + sizeof dirpnt, 0, l - k - sizeof dirpnt);
      adj_from = n;								// old offset of pqr20
      adj_to   = dirblocksize;							// new offset of pqr20
    } else {
      casx = 11;
      dirblockbuff2[0] = 1;
      memcpy (dirblockbuff2 + 1, fname, namelen);
      memcpy (dirblockbuff2 + 1 + namelen, dirblockbuff + k, l - k);
      memset (dirblockbuff2 + 1 + namelen + l - k, 0, dirblocksize - (1 + namelen + l - k));
      memcpy (dirblockbuff + k, dirpnt, sizeof dirpnt);
      memset (dirblockbuff + k + sizeof dirpnt, 0, l - k - sizeof dirpnt);
      adj_from = k;								// old offset of 430
      adj_to   = dirblocksize + 1 + namelen;					// new offset of 430
    }
  }

  /* If not, see if it will fit in new block with the stuff that comes after it */

  else if (namelen + sizeof dirpnt[0] + l - k <= dirblocksize) {
    extendsize = 1;								/* ok, extend to create second block */
    dirblockbuff2[0] = 1;
    memcpy (dirblockbuff2 + 1, fname, namelen);
    memcpy (dirblockbuff2 + 1 + namelen, dirpnt + 0, sizeof dirpnt[0]);
    memcpy (dirblockbuff2 + 1 + namelen + sizeof dirpnt[0], dirblockbuff + k, l - k);
    memset (dirblockbuff2 + 1 + namelen + sizeof dirpnt[0] + l - k, 0, dirblocksize - (1 + namelen + sizeof dirpnt[0] + l - k));
    if (k == j) {								/* see if new entry is the very newest */
      casx = 12;
      memset (dirblockbuff + i, 0, l - i);					/* if so, zero fill starting at filename in first block */
    } else {
      casx = 13;
      memset (dirblockbuff + k, 0, l - k);					/* if not, zero fill starting at lower version number in first block */
    }
    adj_from = k;								// old offset of mno;4
    adj_to   = dirblocksize + 1 + namelen + sizeof dirpnt[0];			// new offset of mno;4
  }

  /* See if the name, with its new version and all old versions, can fit in a block by itself       */
  /* Hopefully, the conditions of i.eq.0 or n.eq.l were taken care of above so we don't waste space */

  else if (1 + namelen + n - j <= dirblocksize) {
    casx = 14;
    extendsize = 2;								/* ok, extend directory by two blocks */
    dirblockbuff3[0] = 1;
    memcpy (dirblockbuff3 + 1, fname, dirblockbuff[n] - 1);
    memcpy (dirblockbuff3 + 1 + dirblockbuff[n] - 1, dirblockbuff + n, l - n);
    memset (dirblockbuff3 + 1 + dirblockbuff[n] - 1 + l - n, 0, dirblocksize - (1 + dirblockbuff[n] - 1 + l - n));
    dirblockbuff2[0] = 1;
    memcpy (dirblockbuff2 + 1, fname, namelen);
    memcpy (dirblockbuff2 + 1 + namelen, dirblockbuff + j, k - j);
    memcpy (dirblockbuff2 + 1 + namelen + k - j, dirpnt + 0, sizeof dirpnt[0]);
    memcpy (dirblockbuff2 + 1 + namelen + k - j + sizeof dirpnt[0], dirblockbuff + k, n - k);
    memset (dirblockbuff2 + 1 + namelen + k - j + sizeof dirpnt[0] + n - k, 0, 
            dirblocksize - (1 + namelen + k - j + sizeof dirpnt[0] + n - k));
    memset (dirblockbuff + i, 0, l - i);
    adj_from = n;								// old offset of pqr
    adj_to   = 2 * dirblocksize;						// new offset of pqr
  }

  /* See if the name with its new version and all higher versions can fit in a block */

  else if (1 + namelen + k - j + sizeof dirpnt <= dirblocksize) {
    casx = 15;
    extendsize = 2;								/* ok, extend directory by two blocks */
    dirblockbuff3[0] = 1;
    memcpy (dirblockbuff3 + 1, fname, namelen);
    memcpy (dirblockbuff3 + 1 + namelen, dirblockbuff + k, l - k);
    memset (dirblockbuff3 + 1 + namelen + l - k, 0, dirblocksize - (1 + namelen + l - k));
    dirblockbuff2[0] = 1;
    memcpy (dirblockbuff2 + 1, fname, namelen);
    memcpy (dirblockbuff2 + 1 + namelen, dirblockbuff + j, k - j);
    memcpy (dirblockbuff2 + 1 + namelen + k - j, dirpnt, sizeof dirpnt);
    memset (dirblockbuff2 + 1 + namelen + k - j + sizeof dirpnt, 0, 
            dirblocksize - (1 + namelen + k - j + sizeof dirpnt));
    memset (dirblockbuff + i, 0, l - i);					/* wipe starting at filename from first block */
    adj_from = k;								// old offset of mno;4
    adj_to   = 2 * dirblocksize + 1 + namelen;					// new offset of mno;4
  }

  /* See if the name with its new version and all lower versions can fit in a block */

  else if (1 + namelen + sizeof dirpnt[0] + n - k <= dirblocksize) {
    extendsize = 2;								/* ok, extend directory by two blocks */
    dirblockbuff3[0] = 1;
    memcpy (dirblockbuff3 + 1, fname, dirblockbuff[n] - 1);
    memcpy (dirblockbuff3 + 1 + dirblockbuff[n] - 1, dirblockbuff + n + 1, l - n - 1);
    memset (dirblockbuff3 + 1 + dirblockbuff[n] - 1 + l - n - 1, 0, 
            dirblocksize - (1 + dirblockbuff[n] - 1 + l - n - 1));
    dirblockbuff2[0] = 1;
    memcpy (dirblockbuff2 + 1, fname, namelen);
    memcpy (dirblockbuff2 + 1 + namelen, dirpnt + 0, sizeof dirpnt[0]);
    memcpy (dirblockbuff2 + 1 + namelen + sizeof dirpnt[0], dirblockbuff + k, n - k);
    memset (dirblockbuff2 + 1 + namelen + sizeof dirpnt[0] + n - k, 0, 
            dirblocksize - (1 + namelen + sizeof dirpnt[0] + n - k));
    if (k == j) {								/* see if the new version is the very latest */
      casx = 16;
      memset (dirblockbuff + i, 0, l - i);					/* if so, wipe the filename from the first block */
    } else {
      casx = 17;
      memset (dirblockbuff + k, 0, l - k);					/* else, just wipe the lower versions from the first block */
    }
    adj_from = n;								// old offset of pqr
    adj_to   = 2 * dirblocksize;						// new offset of pqr
  }

  /* Put it in a block all by itself */

  else {
    extendsize = 2;								/* extend directory by two blocks */
    dirblockbuff3[0] = 1;
    memcpy (dirblockbuff3 + 1, fname, namelen);
    memcpy (dirblockbuff3 + 1 + namelen, dirblockbuff + k, l - k);
    memset (dirblockbuff3 + 1 + namelen + l - k, 0, dirblocksize - (1 + namelen + l - k));
    dirblockbuff2[0] = 1;
    memcpy (dirblockbuff2 + 1, fname, namelen);
    memcpy (dirblockbuff2 + 1 + namelen, dirpnt, sizeof dirpnt);
    memset (dirblockbuff2 + 1 + namelen + sizeof dirpnt, 0, dirblocksize - (1 + namelen + sizeof dirpnt));
    if (k == j) {								/* see if the new version is the very latest */
      casx = 18;
      memset (dirblockbuff + i, 0, l - i);					/* if so, wipe the filename from the first block */
    } else {
      casx = 19;
      memset (dirblockbuff + k, 0, l - k);					/* else, just wipe the lower versions from the first block */
    }
    adj_from = k;								// old offset of mno;4
    adj_to   = 2 * dirblocksize + 1 + namelen;					// new offset of mno;4
  }

  /***************************************/
  /* Write the modified block(s) to disk */
  /***************************************/

write_dir_blocks_ex:
  extendvbn = dirvbn + volex -> homeblock.clusterfactor;			// determine block to extend at
  writesize = (extendsize + 1) * dirblocksize;					// determine how much to write

write_dir_blocks:
  if (adj_from >= adj_to) {
    oz_dev_vdfs_printk (iopex, "oz_dev_dfs enter_file: adj_from %u >= adj_to %u (case %d)\n", adj_from, adj_to, casx);
    oz_dev_vdfs_printk (iopex, "oz_dev_dfs enter_file: entering '%s' into '%s' at vbn %u\n", name, dirname, dirvbn);
    sts = OZ_DIRCORRUPT;
    goto cleanup;
  }

  /* Validate new dir block contents */

#if 000
  oz_knl_printk ("oz_dev_dfs enter_file*: case %d, adj_from %u, _to %u\n", casx, adj_from, adj_to);
  for (i = 0; i < writesize; i += dirblocksize) {
    oz_knl_dumpmem2 (dirblocksize, dirblockbuff + i, (dirvbn + (i / dirblocksize)) << 16);
  }
#endif

  if (!validirbuf (writesize, dirblocksize, dirblockbuff, iopex)) {
    oz_dev_vdfs_printk (iopex, "oz_dev_dfs enter_file: new directory blocks corrupt (case %d)\n", casx);
    oz_dev_vdfs_printk (iopex, "oz_dev_dfs enter_file: entering '%s' into '%s' at vbn %u\n", name, dirname, dirvbn);
    sts = OZ_DIRCORRUPT;
    goto cleanup;
  }

  /* Maybe we need to extend */

  oz_dev_vdfs_lock_dirfile_for_write (dirfile);					/* block shortcuts from reading directory */
  if (extendsize != 0) {
    extendsize *= volex -> homeblock.clusterfactor;				/* get number of blocks to insert */
    sts = insert_blocks (dirfile, extendsize, extendvbn, iopex);		/* insert following the first one */
    if (sts != OZ_SUCCESS) {
      oz_dev_vdfs_printk (iopex, "oz_dev_dfs enter_file: error %u extending directory %s by %u at %u\n", sts, dirname, extendsize, extendvbn);
      oz_dev_vdfs_unlk_dirfile_for_write (dirfile);
      goto cleanup;
    }
    direfblk = dirfile -> allocblocks + 1;					/* compute new eof pointer = very end of the file */
    vl = oz_hw_smplock_wait (&(dirfile -> attrlock_vl));
    dirfile -> attrlock_efblk  = direfblk;
    dirfile -> attrlock_efbyt  = 0;
    dirfile -> attrlock_date   = oz_hw_tod_getnow ();
    dirfile -> attrlock_flags |= OZ_VDFS_ALF_M_EOF | OZ_VDFS_ALF_M_ADT | OZ_VDFS_ALF_M_CDT | OZ_VDFS_ALF_M_MDT;
    oz_hw_smplock_clr (&(dirfile -> attrlock_vl), vl);
    oz_dev_vdfs_mark_header_dirty (dirfile);
  }

  /* Adjust any wildscans going on */

  adj_wildscans_ins (dirfile, dirvbn, adj_from, adj_to, extendsize, dirblockbuff, fname);

  /* Write out updated block(s) */

  sts = oz_dev_vdfs_writevirtblock (dirfile, dirvbn, 0, writesize, dirblockbuff, iopex, 1);
  oz_dev_vdfs_unlk_dirfile_for_write (dirfile);					/* let shortcuts read directory now */
  if (sts != OZ_SUCCESS) {
    oz_dev_vdfs_printk (iopex, "oz_dev_dfs enter_file: error %u writing %u bytes to directory at block %u\n", sts, writesize, dirvbn);
    goto cleanup;
  }

  /* Increment the number of directory entries that refer to this file */

  entfile -> filex -> header.dircount ++;
  oz_dev_vdfs_mark_header_dirty (entfile);

  /* Maybe return resultant filename to caller */

  if (name_r != NULL) {
    memcpy (name_r, fname, namelen);						/* copy name w/ terminating null */
    if (fname[namelen-2] != '/') {						/* see if it's a directory name */
      name_r[namelen-1] = ';';							/* if not, replace null with ; */
      strcpy (name_r + namelen, vstr);						/* ... followed by version number */
    }
  }

  /* Clean up and return final status */

cleanup:
  if (entfile != file) oz_dev_vdfs_close_file (entfile, iopex);			/* close file being entered */
  return (sts);									/* all done */
}

/************************************************************************/
/*									*/
/*  Remove a file from a directory					*/
/*									*/
/*    Input:								*/
/*									*/
/*	dirfile = directory file					*/
/*	name    = name to remove (must include absolute version number)	*/
/*									*/
/*    Output:								*/
/*									*/
/*	remove_file = OZ_SUCCESS : successful				*/
/*	                    else : error status				*/
/*									*/
/************************************************************************/

static uLong dfs_remove_file (OZ_VDFS_File *dirfile, const char *name, char *name_r, OZ_VDFS_Iopex *iopex)

{
  char c, fname[FILENAME_MAX], *p, thisname[FILENAME_MAX];
  int cmp, i, j, k, namelen;
  OZ_VDFS_File *file;
  OZ_Dbn clusterfactor, direfblk, dirvbn;
  OZ_VDFS_Fileid fileid;
  OZ_VDFS_Volex *volex;
  OZ_VDFS_Volume *volume;
  uByte *dirblockbuff;
  uLong dirblocksize, sts, v, version, vl;

  volume = dirfile -> volume;
  volex  = volume -> volex;

  file = NULL;

  if (!IS_DIRECTORY (dirfile -> filex -> header)) return (OZ_FILENOTADIR);

  /* Parse name into fname;version.  Directories don't have        */
  /* ;version but end in / and are stored internally as version 1. */

  namelen = strlen (name) + 1;
  if (namelen > FILENAME_MAX) return (OZ_BADFILENAME);
  memcpy (fname, name, namelen);
  p = strchr (fname, ';');
  version = 0;
  if (p != NULL) {
    *(p ++) = 0;
    namelen = p - fname;
    c = *p;
    while ((c >= '0') && (c <= '9')) {
      version = version * 10 + c - '0';
      c = *(++ p);
    }
    if (c != 0) return (OZ_BADFILEVER);
  }
  if (fname[namelen-2] == '/') version = 1;

  /* Scan directory for given filename;version */

  dirblocksize = volume -> dirblocksize;					/* get the 'block size' = one cluster at a time */
  dirblockbuff = volex -> dirblockbuff3;					/* point to block buffer for reading the directory */

  direfblk = dirfile -> allocblocks + 1;					/* compute eof pointer = very end of the file */

  clusterfactor = volex -> homeblock.clusterfactor;
  dirvbn = 1;										// start scanning at beg of directory
  if (direfblk >= clusterfactor * 8) {							// check for hefty sized directory
    sts = lookup_file (dirfile, namelen - 1, fname, &dirvbn, &fileid, NULL, iopex);	// if so, find file quickly
    if (sts != OZ_SUCCESS) return (sts);						// abort if some error
    dirvbn = (dirvbn - 1) / clusterfactor;						// get zero-based bucket number
    dirvbn = (dirvbn * clusterfactor) + 1;						// get one-based block number
  }

  for (; dirvbn < direfblk; dirvbn += clusterfactor) {						/* loop through the whole directory */
    sts = oz_dev_vdfs_readvirtblock (dirfile, dirvbn, 0, dirblocksize, dirblockbuff, iopex, 0);	/* read a whole dirblockbuff */
    if (sts != OZ_SUCCESS) {
      if (sts != OZ_ENDOFFILE) oz_dev_vdfs_printk (iopex, "oz_dev_dfs remove_file: error %u reading directory\n", sts);
      goto cleanup;
    }
    if (!validirbuf (dirblocksize, dirblocksize, dirblockbuff, iopex)) {	/* make sure we didn't read a corrupt block */
      oz_dev_vdfs_printk (iopex, "oz_dev_dfs remove_file: old directory block %u corrupt\n", dirvbn);
      sts = OZ_DIRCORRUPT;
      goto cleanup;
    }
    for (i = 0; i < dirblocksize;) {						/* scan the dirblockbuff */
      if (dirblockbuff[i] == 0) break;						/* a null terminates the block, on to next one */
      k = j = i + strlen (dirblockbuff + i) + 1;				/* get offset to dirpnt array */
      memcpy (thisname + dirblockbuff[i] - 1, dirblockbuff + i + 1, j - i - 1);	/* get complete filename string */
      cmp = strcmp (thisname, fname);						/* compare dir's filename to given filename */
      if (cmp > 0) goto notfound;						/* if dir filename .gt. given filename, name is not found */
      if (cmp == 0) {								/* if dir filename .eq. given filename, ... */
        while (k < dirblocksize) {						/* ... scan for the given version number */
          v = ((Dirpnt *)(dirblockbuff + k)) -> version;
          if (v == 0) break;
          if (version == 0) goto found_file;
          if (v < version) goto notfound;					/* if a lower version is found, the file is not found */
          if (v == version) goto found_file;					/* if an exact match is found, go remove the file */
          k += sizeof (Dirpnt);
        }
      }
      i = k;									/* skip on to next filename in directory block */
      do i += sizeof (Dirpnt);
      while ((i < dirblocksize) && (((Dirpnt *)(dirblockbuff + i))[-1].version != 0));
    }
  }
notfound:
  sts = OZ_NOSUCHFILE;
  goto cleanup;

found_file:
  fileid = ((Dirpnt *)(dirblockbuff + k)) -> fileid;				/* get the file id */
  sts = OZ_SACREDFILE;								/* check for sacred files */
  if (fileid.seq == 0) goto cleanup;

  if (name_r != NULL) {
    if (dirblockbuff[j-2] == '/') strncpyz (name_r, thisname, FILENAME_MAX);
    else oz_sys_sprintf (FILENAME_MAX, name_r, "%s;%u", thisname, v);
  }

  sts = oz_dev_vdfs_open_file (volume, &fileid, 0, &file, iopex);		/* open the file being removed */
  if (sts != OZ_SUCCESS) oz_dev_vdfs_printk (iopex, "oz_dev_dfs remove_file: error %u opening file %s to decrement dircount\n", sts, name);
  else if ((file -> filex -> header.dircount == 1) && dirisnotempty (file, iopex)) sts = OZ_DIRNOTEMPTY; /* don't delete non-empty directory */

  if (sts != OZ_DIRNOTEMPTY) {
    if ((j == k) && (((Dirpnt *)(dirblockbuff + k))[1].version == 0)) {		/* see if this is the only version of the file */
      k += 2 * sizeof (Dirpnt);							// ok, point to next filename in the bucket
      if ((k < dirblocksize) && (dirblockbuff[k] != 0)) {			// see if there is a next filename in bucket
        j = dirblockbuff[k] - dirblockbuff[i];					// see if next assumed more matching chars
        if (j > 0) {
          memmove (dirblockbuff + i + 1 + j, dirblockbuff + k + 1, dirblocksize - (k + 1));
          memset (dirblockbuff + dirblocksize + i + j - k, 0, k - i - j);
        } else {
          memmove (dirblockbuff + i, dirblockbuff + k, dirblocksize - k);	// it didn't, so just copy it down over old one
          memset (dirblockbuff + i + dirblocksize - k, 0, k - i);
        }
      } else {
        memset (dirblockbuff + i, 0, k - i);					// nothing follows, just clear it out
      }
    } else {
      i  = k;									/* other versions, just remove this one entry */
      k += sizeof (Dirpnt);
      memmove (dirblockbuff + i, dirblockbuff + k, dirblocksize - k);		/* move the rest of whats in block down over it */
      memset (dirblockbuff + i + dirblocksize - k, 0, sizeof (Dirpnt));		/* ... then zero fill back to original size */
    }
    if (dirblockbuff[0] == 0) {							/* see if bucket is completely empty now */
      oz_dev_vdfs_lock_dirfile_for_write (dirfile);				/* block shortcuts from reading directory */
      adj_wildscans_rem (dirfile, dirvbn, 0, 0, NULL);				/* adjust any shortcut wildscans going on */
      sts = remove_blocks (dirfile, clusterfactor, dirvbn, iopex);		/* remove empty directory bucket */
      if (sts == OZ_SUCCESS) {
        vl = oz_hw_smplock_wait (&(dirfile -> attrlock_vl));			/* update end-of-file marker */
        dirfile -> attrlock_efblk  = dirfile -> allocblocks + 1;
        dirfile -> attrlock_efbyt  = 0;
        dirfile -> attrlock_date   = oz_hw_tod_getnow ();
        dirfile -> attrlock_flags |= OZ_VDFS_ALF_M_EOF | OZ_VDFS_ALF_M_ADT | OZ_VDFS_ALF_M_CDT | OZ_VDFS_ALF_M_MDT;
        oz_hw_smplock_clr (&(dirfile -> attrlock_vl), vl);
        oz_dev_vdfs_mark_header_dirty (dirfile);
      }
    } else {
      if (!validirbuf (dirblocksize, dirblocksize, dirblockbuff, iopex)) {	/* make sure we didn't screw it up */
        oz_dev_vdfs_printk (iopex, "oz_dev_dfs remove_file: new directory block %u corrupt\n", dirvbn);
        sts = OZ_DIRCORRUPT;
        goto cleanup;
      }
      oz_dev_vdfs_lock_dirfile_for_write (dirfile);				/* block shortcuts from reading directory */
      adj_wildscans_rem (dirfile, dirvbn, k, i, dirblockbuff);			/* adjust any shortcut wildscans going on */
      sts = oz_dev_vdfs_writevirtblock (dirfile, dirvbn, 0, dirblocksize, dirblockbuff, iopex, 1); /* write directory bucket back */
    }
    oz_dev_vdfs_unlk_dirfile_for_write (dirfile);				/* let shortcuts read directory now */
    if (sts != OZ_SUCCESS) {
      oz_dev_vdfs_printk (iopex, "oz_dev_dfs remove_file: error %u %sing directory block after remove\n", sts, (dirblockbuff[0] != 0) ? "writ" : "remov");
      goto cleanup;
    }
    if (file != NULL) {								/* if file was successfully opened, ... */
      if (file -> filex -> header.dircount != 0) file -> filex -> header.dircount --; /* decrement directory reference count */
      oz_dev_vdfs_mark_header_dirty (file);
    }
  }

  if (file != NULL) {								/* if file was successfully opened, ... */
    v = oz_dev_vdfs_close_file (file, iopex);					/* close file and delete if dircount is zero */
    if (v != OZ_SUCCESS) oz_dev_vdfs_printk (iopex, "oz_dev_dfs remove_file: error %u deleting file %s after it was removed from directory\n", sts, name);
    file = NULL;
    if (sts == OZ_SUCCESS) sts = v;
  }

cleanup:
  if (file != NULL) oz_dev_vdfs_close_file (file, iopex);			/* maybe file is still open */
  return (sts);									/* all done */
}

/************************************************************************/
/*									*/
/*  Determine if the given file is a directory, and if so, if it has 	*/
/*  any entries in it							*/
/*									*/
/************************************************************************/

static int dirisnotempty (OZ_VDFS_File *dirfile, OZ_VDFS_Iopex *iopex)

{
  uByte dirblockbuff[4];
  uLong sts;
  OZ_Dbn direfblk, dirvbn;
  OZ_VDFS_Volume *volume;

  volume = dirfile -> volume;

  /* If it is not a directory, then it does not have any entries in it */

  if (!IS_DIRECTORY (dirfile -> filex -> header)) return (0);

  /* Get size of directory file */

  direfblk = dirfile -> attrlock_efblk;
  if (dirfile -> attrlock_efbyt == 0) direfblk --;

  /* Scan the whole directory for entries                                          */
  /* All we need to do is check the first byte of each 'dirblockbuff' for non-zero */

  for (dirvbn = 1; dirvbn <= direfblk; dirvbn += volume -> volex -> homeblock.clusterfactor) {
    sts = oz_dev_vdfs_readvirtblock (dirfile, dirvbn, 0, sizeof dirblockbuff, dirblockbuff, iopex, 0);
    if (sts != OZ_SUCCESS) {
      oz_dev_vdfs_printk (iopex, "oz_dev_dfs dirisnotempty: error %u reading directory\n", sts);
      break;
    }
    if (dirblockbuff[0] != 0) return (1);
  }

  /* All blocks begin with a null byte, so the directory is empty */

  return (0);
}

/************************************************************************/
/*									*/
/*  Stuff was moved by enter/remove routines from offset 'from' in 	*/
/*  block 'dirvbn' to offset 'to'					*/
/*									*/
/*    Input:								*/
/*									*/
/*	dirfile = directory file block					*/
/*	dirvbn  = block containing 'from' data				*/
/*	from    = offset in dirvbn block where data used to be		*/
/*	to      = offset in dirvbn block where data is now		*/
/*	blocksinserted = number of directory blocks inserted		*/
/*	dirbuff = contents of dirvbn block				*/
/*	          plus more to cover the block that contains 'to'	*/
/*	dirfile -> dirlockr = set to write access thus blocking 	*/
/*	                      shortcut wildscan routines from 		*/
/*	                      accessing directory			*/
/*									*/
/*    Output:								*/
/*									*/
/*	wildscan contexts adjusted accordingly				*/
/*									*/
/************************************************************************/

  /* It is ok to scan the wildscans list without the recio_vl lock because we have the dirlockr */
  /* lock set to write access which will block all sc_wildscan routines from accessing the list */

  /* The wildscan context should have:                                                  */
  /*    wildscan -> blockvbn = vbn of data in blockbuff                                 */
  /*   wildscan -> blockbuff = contents of the directory block                          */
  /*   wildscan -> blockoffs = offset in blockbuff of next data to process              */
  /*  wildscan -> ver_output = 0 : blockoffs points to a filename                       */
  /*                           1 : blockoffs points to a non-zero version array element */

static void adj_wildscans_ins (OZ_VDFS_File *dirfile, OZ_Dbn dirvbn, uLong from, uLong to, OZ_Dbn blocksinserted, uByte *dirbuff, char *fname)

{
  OZ_Dbn clusterfactor;
  OZ_VDFS_Wildscan *wildscan;
  uLong dirblocksize, vl;

  clusterfactor = dirfile -> volume -> volex -> homeblock.clusterfactor;
  dirblocksize  = dirfile -> volume -> dirblocksize;

  for (wildscan = dirfile -> wildscans; wildscan != NULL; wildscan = wildscan -> next_dirfile) {

    /* If the wildscan routine hasn't even gotten to the modified blocks yet, just leave it alone */

    if (wildscan -> blockvbn < dirvbn) continue;

    /* If the wildscan routine is completely past all modified blocks, just inc its vbn. */
    /* The contents of its block are the same, but it has a different vbn now.           */

    if (wildscan -> blockvbn > dirvbn) {
      wildscan -> blockvbn += blocksinserted * clusterfactor;
      continue;
    }

    /* It is scanning the modified block */

    /* If it is before any modifications, it hasn't seen anthing that */
    /* has been modified yet, so just give it the new block contents  */

    if ((wildscan -> blockoffs <= from) && (wildscan -> blockoffs <= to)) {
      memcpy (wildscan -> blockbuff, dirbuff, dirblocksize);
    }

    /* Otherwise, advance pointers to equivalent position after new data */

    else {
      wildscan -> blockoffs += to - from;
      wildscan -> blockvbn  += wildscan -> blockoffs / dirblocksize;
      wildscan -> blockoffs %= dirblocksize;
      memcpy (wildscan -> blockbuff, dirbuff + (wildscan -> blockvbn - dirvbn) * dirblocksize, dirblocksize);

      /* If it is pointing right at the next filename following the one that was just inserted,   */
      /* tell it the last name in the directory was the inserted name, so it will have the proper */
      /* 'same chars' to start with.                                                              */

      if ((wildscan -> blockoffs == to) && !(wildscan -> ver_output)) strcpy (wildscan -> lastname, fname);
    }
  }
}

/* This one is called when an entry has been removed from the directory */
/* If dirbuff==NULL, the whole block was removed from the directory     */

static void adj_wildscans_rem (OZ_VDFS_File *dirfile, OZ_Dbn dirvbn, uLong from, uLong to, uByte *dirbuff)

{
  OZ_Dbn clusterfactor;
  OZ_VDFS_Wildscan *wildscan;
  uLong dirblocksize;

  clusterfactor = dirfile -> volume -> volex -> homeblock.clusterfactor;
  dirblocksize  = dirfile -> volume -> dirblocksize;

  for (wildscan = dirfile -> wildscans; wildscan != NULL; wildscan = wildscan -> next_dirfile) {

    /* If the wildscan routine hasn't even gotten to the modified block yet, just leave it alone */

    if (wildscan -> blockvbn < dirvbn) continue;

    /* If the wildscan routine is completely past the modified block, just dec its vbn. */
    /* The contents of its block are the same, but it has a different vbn now.          */

    if (wildscan -> blockvbn > dirvbn) {
      if (dirbuff == NULL) wildscan -> blockvbn -= clusterfactor;
      continue;
    }

    /* It is scanning the modified block */

    /* If the block was removed, point wildscan to the beginning of the next block */

    if (dirbuff == NULL) {
      wildscan -> blockoffs  = dirblocksize;	// well, really point it to the end of the previous block
      wildscan -> blockvbn  -= clusterfactor;	// ... so it will read in the next block when it executes
      wildscan -> ver_output = 0;
      continue;
    }

    /* Block wasn't removed, just changed.  Get modified block contents. */

    memcpy (wildscan -> blockbuff, dirbuff, dirblocksize);

    /* If wildscan is after removed area, just decrement pointer and let it go from there */

    if (wildscan -> blockoffs >= from) wildscan -> blockoffs -= from - to;

    /* If it is 'in' the removed area, just point to end of removed area.  The only way this can happen is if, say we    */
    /* started with 'abc ;1 ;0' and we're pointing at the ';1', then the 'abc;1' entry is removed.  Well, the remove     */
    /* routine wipes out the whole 'abc ;1 ;0' thing, and so we adjust the wildscan to continue with the following file. */

    else if (wildscan -> blockoffs > to) {
      wildscan -> blockoffs  = to;
      wildscan -> ver_output = 0;
    }

    /* Now if it is right at the beginning of the removed area, it could have been pointing to either a filename or a version */
    /* number entry.  If it was pointing to a filename, it still is as the remove routine would only remove the whole entry   */
    /* for the file.  If it was pointing to a version, it still is, as wildscan_continue never leaves the pointer pointing to */
    /* version 0.  But if it is now pointing to the ;0 entry, we have to increment it to the next filename in the buffer.     */

    else if ((wildscan -> blockoffs == to) && (wildscan -> ver_output) && (((Dirpnt *)(wildscan -> blockbuff + wildscan -> blockoffs)) -> version == 0)) {
      wildscan -> blockoffs += sizeof (Dirpnt);
      wildscan -> ver_output = 0;
    }
  }
}

/************************************************************************/
/*									*/
/*  Return parsed filespec string					*/
/*									*/
/************************************************************************/

static void dfs_returnspec (char *spec, uLong size, char *buff, OZ_FS_Subs *subs)

{
  char *p, *q, *r;

  if (size > 0) movc4 (strlen (spec), spec, size, buff);	/* if buffer given, return the string */

  if (subs != NULL) {						/* see if substring sizes wanted */
    memset (subs, 0, sizeof *subs);
    if (strcmp (spec, "/") == 0) {				/* make sure we handle this case correctly */
      subs -> namsize = 1;					/* ... it is the root directory as a file */
      return;
    }
    p = strrchr (spec, '/');					/* find the last / in the spec */
    if (p == NULL) p = spec;					/* if none, point to beginning */
    else if (p[1] != 0) p ++;					/* ... but make sure we're after it */
    else {
      while (p > spec) {					/* last char was last slash, ... */
        if (p[-1] == '/') break;				/* ... so we consider the last dirname */
        -- p;							/* ... to be the file name */
      }
      subs -> dirsize = p - spec;				/* ... no type or version */
      subs -> namsize = strlen (p);				/* ... even if the name has a . in it */
      return;
    }
    subs -> dirsize = p - spec;					/* directory is all up to that point including last / */
    q = strrchr (p, ';');					/* find the last ; in the spec */
    if (q == NULL) q = p + strlen (p);				/* if none, point to end of spec */
    subs -> versize = strlen (q);				/* version is length of that part */
    r = strrchr (p, '.');					/* find the last . in the string */
    if ((r == NULL) || (r > q)) r = q;				/* if none, point at ; */
    subs -> typsize = q - r;					/* type is starting at . up to ; */
    subs -> namsize = r - p;					/* name is after directory up to . */
  }
}

/************************************************************************/
/*									*/
/*  Create file								*/
/*									*/
/*    Input:								*/
/*									*/
/*	volume  = volume struct pointer					*/
/*	namelen = length of file name string				*/
/*	name    = file name string (not null terminated)		*/
/*	filattrflags = file attribute flags				*/
/*	dirid   = directory id						*/
/*	file    = file block pointer					*/
/*	file -> secattr = security attributes				*/
/*									*/
/*    Output:								*/
/*									*/
/*	create_file = OZ_SUCCESS : successful creation			*/
/*	                    else : error status				*/
/*	file -> filex = filled in					*/
/*									*/
/************************************************************************/

static uLong dfs_create_file (OZ_VDFS_Volume *volume, int namelen, const char *name, uLong filattrflags, OZ_VDFS_Fileid *dirid, OZ_VDFS_File *file, OZ_VDFS_Fileid **fileid_r, OZ_VDFS_Iopex *iopex)

{
  char c;
  int i;
  Filattr filattr;
  OZ_Event *dirlockf;
  OZ_VDFS_Filex *extfilex, *filex;
  uLong secattrsize, sts;

  /* Allocate a new file header */

  sts = allocate_header (volume, &filex, iopex);
  if (sts != OZ_SUCCESS) return (sts);
  file -> filex = filex;

  /* Set up directory id - do not set file -> dircount, it will be set by the enter_file routine */

  filex -> header.dirid = *dirid;

  /* Return pointer to its fileid */

  *fileid_r = &(filex -> header.fileid);

  /* Copy filename string into header */

  sts = make_header_room (file, filex, namelen + 1, OZ_FS_HEADER_AREA_FILNAME, 0, &extfilex, iopex);
  if (sts != OZ_SUCCESS) goto rtnerr;

  extfilex -> header.areas[OZ_FS_HEADER_AREA_FILNAME].size = namelen + 1;
  i = extfilex -> header.areas[OZ_FS_HEADER_AREA_FILNAME].offs;
  memcpy (extfilex -> header.area + i, name, namelen);
  extfilex -> header.area[i+namelen] = 0;
  mark_exthdr_dirty (extfilex, file);

  /* Copy file attributes into header */

  sts = make_header_room (file, filex, sizeof filattr, OZ_FS_HEADER_AREA_FILATTR, 0, &extfilex, iopex);
  if (sts != OZ_SUCCESS) goto rtnerr;

  memset (&filattr, 0, sizeof filattr);
  filattr.create_date  = oz_hw_tod_getnow ();
  filattr.change_date  = filattr.create_date;
  filattr.modify_date  = filattr.create_date;
  filattr.access_date  = filattr.create_date;
  filattr.eofblock     = 1;
  filattr.filattrflags = filattrflags;

  extfilex -> header.areas[OZ_FS_HEADER_AREA_FILATTR].size = sizeof filattr;
  *(Filattr *)(extfilex -> header.area + extfilex -> header.areas[OZ_FS_HEADER_AREA_FILATTR].offs) = filattr;
  mark_exthdr_dirty (extfilex, file);

  /* Copy security attributes into header */

  secattrsize = oz_knl_secattr_getsize (file -> secattr);
  sts = make_header_room (file, filex, secattrsize, OZ_FS_HEADER_AREA_SECATTR, 1, &extfilex, iopex);
  if (sts != OZ_SUCCESS) goto rtnerr;

  extfilex -> header.areas[OZ_FS_HEADER_AREA_SECATTR].size = secattrsize;
  memcpy (extfilex -> header.area + extfilex -> header.areas[OZ_FS_HEADER_AREA_SECATTR].offs, oz_knl_secattr_getbuff (file -> secattr), secattrsize);

  /* Header needs to be written out */

  mark_exthdr_dirty (extfilex, file);
  if (!validate_header (&(filex -> header), volume, iopex)) oz_crash ("oz_dev_dfs create_file: creating corrupt header %p", &(filex -> header));
  return (OZ_SUCCESS);

  /* Error, free filex struct and header, then return error status */

rtnerr:
  delete_header (filex, iopex);
  file -> filex = NULL;
  return (sts);
}

/************************************************************************/
/*									*/
/*  Get secattrs to create the file with				*/
/*									*/
/*    Input:								*/
/*									*/
/*	iopex = requests iopex						*/
/*	secattrsize = secattrs supplied by user				*/
/*	secattrbuff = secattrs supplied by user				*/
/*									*/
/*    Output:								*/
/*									*/
/*	getcresecattr = OZ_SUCCESS : successful				*/
/*	                      else : error status			*/
/*	*secattr_r = pointer to secattr struct (ref count already incd)	*/
/*									*/
/************************************************************************/

static uLong getcresecattr (OZ_VDFS_Iopex *iopex, uLong secattrsize, const void *secattrbuff, OZ_Secattr **secattr_r)

{
  OZ_Secattr *secattr;
  OZ_Seckeys *seckeys;
  OZ_Thread *thread;
  uLong sts;

  thread = oz_knl_ioop_getthread (iopex -> ioop);				/* get the user's thread */
  secattr = NULL;								/* assume 'kernel only' access */
  if (secattrbuff == NULL) {							/* see if user said to use the default */
    if (thread != NULL) secattr = oz_knl_thread_getdefcresecattr (thread);	/* if so, get default create secattrs for that thread */
    secattrsize = oz_knl_secattr_getsize (secattr);				/* and make sure it's not too long */
    if (secattrsize > SECATTR_MAX) {
      oz_knl_secattr_increfc (secattr, -1);
      return (OZ_SECATTRTOOLONG);
    }
  } else {
    if (secattrsize > SECATTR_MAX) return (OZ_SECATTRTOOLONG);			/* user supplied, make sure not too long to fit in an header */
    sts = oz_knl_secattr_create (secattrsize, secattrbuff, NULL, &secattr);	/* save in secattr struct */
    if (sts != OZ_SUCCESS) return (sts);					/* return error status if corrupted */
  }
  seckeys = NULL;								/* now make sure caller would have full access */
  if (thread != NULL) seckeys = oz_knl_thread_getseckeys (thread);		/* ... to the file we're about to create */
  sts = oz_knl_security_check (OZ_SECACCMSK_LOOK | OZ_SECACCMSK_READ | OZ_SECACCMSK_WRITE, seckeys, secattr);
  if (sts == OZ_SUCCESS) *secattr_r = secattr;					/* ok, return pointer to the secattrs */
  else oz_knl_secattr_increfc (secattr, -1);					/* no access, free secattrs off */
  return (sts);									/* anyway, return status */
}

/************************************************************************/
/*									*/
/*  See if the given file is already open				*/
/*									*/
/*    Input:								*/
/*									*/
/*	volume = volume to check					*/
/*	fileid = file to check for					*/
/*									*/
/*    Output:								*/
/*									*/
/*	dfs_findopenfile = NULL : file is not already open		*/
/*	                   else : pointer to file struct		*/
/*									*/
/************************************************************************/

static OZ_VDFS_File *dfs_findopenfile (OZ_VDFS_Volume *volume, const OZ_VDFS_Fileid *fileid)

{
  OZ_VDFS_File *file;

  for (file = volume -> openfiles; file != NULL; file = file -> next) {
    if (memcmp (&(file -> filex -> header.fileid), fileid, sizeof *fileid) == 0) break;
  }
  return (file);
}

/************************************************************************/
/*									*/
/*  Open file by file id						*/
/*									*/
/*    Input:								*/
/*									*/
/*	volume    = volume struct pointer				*/
/*	fileid    = file id to be opened				*/
/*	secaccmsk = security access mask bits				*/
/*									*/
/*    Output:								*/
/*									*/
/*	dfs_open_file = OZ_SUCCESS : successful completion		*/
/*	                      else : error status			*/
/*	file -> filex = filled in with fs dependent struct		*/
/*	     -> secattr = filled in with file's secattrs		*/
/*	     -> attrlock_efblk,_efbyt = file's end-of-file pointer	*/
/*	     -> allocblocks = number of blocks allocated to file	*/
/*									*/
/************************************************************************/

static uLong dfs_open_file (OZ_VDFS_Volume *volume, const OZ_VDFS_Fileid *fileid, OZ_VDFS_File *file, OZ_VDFS_Iopex *iopex)

{
  OZ_Dbn hdrlbn, nblocks;
  OZ_VDFS_Filex *extfilex, *filex;
  OZ_VDFS_Volex *volex;
  uByte *secbuff;
  uLong i, secsize, sts;

  volex = volume -> volex;

  /* Figure out where the header's logical block is.  For the index file, it is in */
  /* the home block.  All others use the file number as the vbn in the index file. */

  if ((fileid -> num == SACRED_FIDNUM_INDEXHEADERS) && (fileid -> seq == 0)) {
    hdrlbn = volex -> homeblock.indexhdrlbn;
  } else {
    sts = dfs_map_vbn_to_lbn (volume -> volex -> indexheaders, fileid -> num, &nblocks, &hdrlbn);
    if (sts != OZ_SUCCESS) return (OZ_INVALIDFILENUM);
  }

  /* Read the header from disk */

  filex = OZ_KNL_PGPMALLOQ (((uByte *)&(filex -> header)) + volex -> homeblock.blocksize - (uByte *)filex);
  if (filex == NULL) return (OZ_EXQUOTAPGP);
  file -> filex = filex;
  sts = read_header_block (fileid, hdrlbn, 0, NULL, filex, iopex);
  if (sts != OZ_SUCCESS) goto rtnerr;

  /* Return the end-of-file pointer */

  file -> attrlock_efblk = FILATTRS (filex -> header) -> eofblock;
  file -> attrlock_efbyt = FILATTRS (filex -> header) -> eofbyte;

  /* Read in all the extension headers.  If error anywhere, undo everything.  Also count the number of allocated blocks. */

  file -> allocblocks = filex -> blocksinhdr;
  secsize = filex -> header.areas[OZ_FS_HEADER_AREA_SECATTR].size;
  while (filex -> header.extid.num != 0) {

    /* Get lbn of extension header */

    sts = dfs_map_vbn_to_lbn (volume -> volex -> indexheaders, filex -> header.extid.num, &nblocks, &hdrlbn);
    if (sts != OZ_SUCCESS) goto rtnerr;

    /* Allocate a struct to read the header into */

    extfilex = OZ_KNL_PGPMALLOQ (((uByte *)&(extfilex -> header)) + volex -> homeblock.blocksize - ((uByte *)extfilex));
    sts = OZ_EXQUOTAPGP;
    if (extfilex == NULL) goto rtnerr;
    filex -> next = extfilex;

    /* Read in the header and make sure it is valid */

    sts = read_header_block (&(filex -> header.extid), hdrlbn, 1, &(filex -> header.fileid), extfilex, iopex);
    if (sts != OZ_SUCCESS) goto rtnerr;

    /* Accumulate various things and link to next extension header */

    file -> allocblocks += extfilex -> blocksinhdr;
    secsize += extfilex -> header.areas[OZ_FS_HEADER_AREA_SECATTR].size;
    filex = extfilex;
  }
  filex = file -> filex;

  /* Make an secattr struct based on what's in the file's header(s) */

  file -> secattr = NULL;
  if (secsize != 0) {
    secbuff = OZ_KNL_PGPMALLOQ (secsize);					/* malloc a temp buffer */
    sts = OZ_EXQUOTAPGP;
    if (secbuff == NULL) goto rtnerr;
    i = 0;									/* copy in all the attributes */
    for (extfilex = filex; extfilex != NULL; extfilex = extfilex -> next) {
      memcpy (secbuff + i, extfilex -> header.area + extfilex -> header.areas[OZ_FS_HEADER_AREA_SECATTR].offs, extfilex -> header.areas[OZ_FS_HEADER_AREA_SECATTR].size);
      i += extfilex -> header.areas[OZ_FS_HEADER_AREA_SECATTR].size;
    }
    if (i != secsize) oz_crash ("oz_disk_fs open_by_lbn: security attribute size changed");
    sts = oz_knl_secattr_create (secsize, secbuff, NULL, &(file -> secattr));	/* create a kernel struct for it */
    OZ_KNL_PGPFREE (secbuff);							/* free off temp buffer */
    if (sts != OZ_SUCCESS) goto rtnerr;
  }

  return (OZ_SUCCESS);

  /* Error, free stuff off and return error status */

rtnerr:
  while ((filex = file -> filex) != NULL) {
    file -> filex = filex -> next;
    OZ_KNL_PGPFREE (filex);
  }
  return (sts);
}

/************************************************************************/
/*									*/
/*  Set file attributes							*/
/*									*/
/*    Input:								*/
/*									*/
/*	file = file to have the attributes set				*/
/*	numitems = number of elements in itemlist array			*/
/*	itemlist = array of items to set				*/
/*	iopex = I/O request being processed				*/
/*									*/
/*    Output:								*/
/*									*/
/*	set_file_attrs = completion status				*/
/*									*/
/************************************************************************/

static uLong dfs_set_file_attrs (OZ_VDFS_File *file, uLong numitems, const OZ_Itmlst2 *itemlist, OZ_VDFS_Iopex *iopex)

{
  OZ_VDFS_Filex *filex;
  uLong i, size, sts, vl;
  void *addr;

  filex = file -> filex;

  /* Scan through given item list */

  for (i = 0; i < numitems; i ++) {
    switch (itemlist[i].item) {

      /* Set file's various dates */

      case OZ_FSATTR_CREATE_DATE: {
        size = sizeof FILATTRS (filex -> header) -> create_date;
        addr = &(FILATTRS (filex -> header) -> create_date);
        break;
      }

      case OZ_FSATTR_ACCESS_DATE: {
        size = sizeof FILATTRS (filex -> header) -> access_date;
        addr = &(FILATTRS (filex -> header) -> access_date);
        vl = oz_hw_smplock_wait (&(file -> attrlock_vl));
        file -> attrlock_flags &= ~ OZ_VDFS_ALF_M_ADT;
        oz_hw_smplock_clr (&(file -> attrlock_vl), vl);
        break;
      }

      case OZ_FSATTR_CHANGE_DATE: {
        size = sizeof FILATTRS (filex -> header) -> change_date;
        addr = &(FILATTRS (filex -> header) -> change_date);
        vl = oz_hw_smplock_wait (&(file -> attrlock_vl));
        file -> attrlock_flags &= ~ OZ_VDFS_ALF_M_CDT;
        oz_hw_smplock_clr (&(file -> attrlock_vl), vl);
        break;
      }

      case OZ_FSATTR_MODIFY_DATE: {
        size = sizeof FILATTRS (filex -> header) -> modify_date;
        addr = &(FILATTRS (filex -> header) -> modify_date);
        vl = oz_hw_smplock_wait (&(file -> attrlock_vl));
        file -> attrlock_flags &= ~ OZ_VDFS_ALF_M_MDT;
        oz_hw_smplock_clr (&(file -> attrlock_vl), vl);
        break;
      }

      case OZ_FSATTR_EXPIRE_DATE: {
        size = sizeof FILATTRS (filex -> header) -> expire_date;
        addr = &(FILATTRS (filex -> header) -> expire_date);
        break;
      }

      case OZ_FSATTR_BACKUP_DATE: {
        size = sizeof FILATTRS (filex -> header) -> backup_date;
        addr = &(FILATTRS (filex -> header) -> backup_date);
        break;
      }

      case OZ_FSATTR_ARCHIVE_DATE: {
        size = sizeof FILATTRS (filex -> header) -> archive_date;
        addr = &(FILATTRS (filex -> header) -> archive_date);
        break;
      }

      /* Set file's end-of-file position */

      case OZ_FSATTR_EOFBLOCK: {
        size = sizeof file -> attrlock_efblk;
        addr = &(file -> attrlock_efblk);
        vl = oz_hw_smplock_wait (&(file -> attrlock_vl));
        file -> attrlock_flags |= OZ_VDFS_ALF_M_EOF;
        oz_hw_smplock_clr (&(file -> attrlock_vl), vl);
        break;
      }

      case OZ_FSATTR_EOFBYTE: {
        size = sizeof file -> attrlock_efbyt;
        addr = &(file -> attrlock_efbyt);
        vl = oz_hw_smplock_wait (&(file -> attrlock_vl));
        file -> attrlock_flags |= OZ_VDFS_ALF_M_EOF;
        oz_hw_smplock_clr (&(file -> attrlock_vl), vl);
        break;
      }

      /* Modify file attribute flags (the only one we allow them to change is 'WRITETHRU') */

      case OZ_FSATTR_FILATTRFLAGS: {
        uLong newattrflags;

        newattrflags = 0;
        size = sizeof newattrflags;
        if (size > itemlist[i].size) size = itemlist[i].size;
        sts = oz_knl_section_uget (iopex -> procmode, size, itemlist[i].buff, &newattrflags);
        if (sts != OZ_SUCCESS) {
          oz_dev_vdfs_printk (iopex, "oz_dev_dfs set_file_attrs: bad item address %p\n", itemlist[i].buff);
          return (sts);
        }
        GET_FATTRFL (filex -> header) = (newattrflags & OZ_FS_FILATTRFLAG_WRITETHRU) 
                                     | (GET_FATTRFL (filex -> header) & ~OZ_FS_FILATTRFLAG_WRITETHRU);
        size = 0;
        addr = NULL;
        break;
      }

      /* Modify security attributes */

#if 000
      case OZ_FSATTR_SECATTR: {
        ???????;
      }
#endif

      /* Who knows what they want modified */

      default: {
        oz_dev_vdfs_printk (iopex, "oz_dev_dfs set_file_attrs: unsupported item code %u\n", itemlist[i].item);
        size = 0;
        addr = NULL;
        break;
      }
    }

    /* Modify attribute by copying user's buffer to size/addr */

    if (size > 0) {
      if (size > itemlist[i].size) size = itemlist[i].size;
      sts = oz_knl_section_uget (iopex -> procmode, size, itemlist[i].buff, addr);
      if (sts != OZ_SUCCESS) {
        oz_dev_vdfs_printk (iopex, "oz_dev_dfs set_file_attrs: bad item address %p\n", itemlist[i].buff);
        return (sts);
      }
    }
  }

  /* Queue the header to be written back to disk */

  oz_dev_vdfs_mark_header_dirty (file);
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Close file, delete if marked for delete				*/
/*									*/
/*    Input:								*/
/*									*/
/*	file = file to be closed					*/
/*									*/
/*    Output:								*/
/*									*/
/*	all filex structs freed off					*/
/*	file possibly deleted						*/
/*									*/
/************************************************************************/

static uLong dfs_close_file (OZ_VDFS_File *file, OZ_VDFS_Iopex *iopex)

{
  OZ_VDFS_Filex *filex;
  uLong sts;

  filex = file -> filex;

  /* If directory reference count is zero, actually delete file (but don't even try a sacred file) */

  if ((filex -> header.dircount == 0) && (filex -> header.fileid.seq != 0)) {
    dfs_extend_file (file, 0, 0, iopex);
    delete_header (filex, iopex);
  }

  /* Otherwise, just free off the filex struct(s) */

  else {
    while ((filex = file -> filex) != NULL) {
      file -> filex = filex -> next;
      OZ_KNL_PGPFREE (filex);
    }
  }

  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Extend or truncate a file						*/
/*									*/
/*    Input:								*/
/*									*/
/*	file     = file block pointer of file to extended / truncated	*/
/*	nblocks  = new total number of blocks				*/
/*	extflags = EXTEND_NOTRUNC : don't truncate			*/
/*	          EXTEND_NOEXTHDR : no extension header			*/
/*									*/
/*    Output:								*/
/*									*/
/*	extend_file = OZ_SUCCESS : extend was successful		*/
/*	                    else : error status				*/
/*									*/
/************************************************************************/

static uLong dfs_extend_file (OZ_VDFS_File *file, OZ_Dbn nblocks, uLong extflags, OZ_VDFS_Iopex *iopex)

{
  OZ_Dbn logblock, relblock, startlbn;
  OZ_VDFS_Filex *extfilex, *extfilex2;
  OZ_VDFS_Volex *volex;
  OZ_VDFS_Volume *volume;
  Pointer *pointer;
  uLong i, nfound, savei, sts;

  volume = file -> volume;
  volex  = volume -> volex;

  /* Get number of blocks rounded up by cluster factor */

  relblock = ((nblocks + volex -> homeblock.clusterfactor - 1) / volex -> homeblock.clusterfactor) * volex -> homeblock.clusterfactor;

  /* Find the extension to extend at or truncate at */

  extfilex2 = NULL;
  for (extfilex = file -> filex; extfilex != NULL; extfilex = extfilex -> next) {
    if (extfilex -> blocksinhdr >= relblock) goto truncate_it;
    relblock -= extfilex -> blocksinhdr;
    extfilex2 = extfilex;
  }

  /*****************************/
  /* File needs to be extended */
  /*****************************/

  while (relblock != 0) {

    /* Get lbn last used in file */

    pointer  = POINTERS (extfilex2);
    startlbn = 0;
    if (extfilex2 -> header.areas[OZ_FS_HEADER_AREA_POINTER].size > 0) {
      pointer += extfilex2 -> header.areas[OZ_FS_HEADER_AREA_POINTER].size / sizeof *pointer;
      startlbn = pointer[-1].logblock + pointer[-1].blockcount;
    }

    /* Make room in the header for the new pointer */

    sts = make_header_room (file, extfilex2, sizeof (Pointer), OZ_FS_HEADER_AREA_POINTER, !(extflags & EXTEND_NOEXTHDR), &extfilex2, iopex);
    if (sts != OZ_SUCCESS) return (sts);

    /* Allocate the blocks */

    sts = allocate_blocks (volume, relblock, startlbn, &nfound, &logblock, iopex);
    if (sts != OZ_SUCCESS) return (sts);

    /* Set up the new pointer */

    pointer = POINTERS (extfilex2) + extfilex2 -> header.areas[OZ_FS_HEADER_AREA_POINTER].size / sizeof *pointer; /* point to where to put new pointer */

    if ((extfilex2 -> header.areas[OZ_FS_HEADER_AREA_POINTER].size >= sizeof *pointer) 				/* if there is at least one pointer there ... */
     && (pointer[-1].blockcount + pointer[-1].logblock == logblock)) {						/* ... and it ends where new one starts ... */
      pointer[-1].blockcount += nfound;										/* it's contiguous with the last allocation */
    } else {
      pointer -> blockcount = nfound;										/* not contiguous, make a new pointer */
      pointer -> logblock   = logblock;
      extfilex2 -> header.areas[OZ_FS_HEADER_AREA_POINTER].size += sizeof *pointer;				/* increase pointer area size */
    }

    /* Increment number of blocks in the header */

    file -> allocblocks      += nfound;
    extfilex2 -> blocksinhdr += nfound;

    /* Decrement number of blocks wanted */

    relblock -= nfound;
  }

  return (OZ_SUCCESS);

  /******************************/
  /* File needs to be truncated */
  /******************************/

truncate_it:
  if (extflags & EXTEND_NOTRUNC) return (OZ_SUCCESS);

  /* Find the pointer to truncate at */

  pointer = POINTERS (extfilex);
  i = 0;
  if (relblock == 0) goto trunc_found_hdr;
  for (; i < extfilex -> header.areas[OZ_FS_HEADER_AREA_POINTER].size / sizeof (Pointer); i ++) {
    if (pointer -> blockcount >= relblock) goto trunc_found_ptr;
    relblock -= pointer -> blockcount;
    pointer ++;
  }

  oz_dev_vdfs_printk (iopex, "oz_dev_dfs extend_file: residual relblock %u, extfilex %p\n", relblock, extfilex);

  relblock = ((nblocks + volex -> homeblock.clusterfactor - 1) / volex -> homeblock.clusterfactor) * volex -> homeblock.clusterfactor;
  oz_dev_vdfs_printk (iopex, "oz_dev_dfs extend_file: original nblocks %u, extflags 0x%x, relblock %u\n", nblocks, extflags, relblock);

  for (extfilex = file -> filex; extfilex != NULL; extfilex = extfilex -> next) {
    oz_dev_vdfs_printk (iopex, "oz_dev_dfs extend_file: extfilex %p, blocksinhdr %u\n", extfilex, extfilex -> blocksinhdr);
    pointer = POINTERS (extfilex);
    for (i = 0; i < extfilex -> header.areas[OZ_FS_HEADER_AREA_POINTER].size / sizeof (Pointer); i ++) {
      oz_dev_vdfs_printk (iopex, "oz_dev_dfs extend_file:    pointer[%u] blockcount %u\n", i, pointer -> blockcount);
    }
    pointer ++;
  }

  oz_crash ("oz_dev_dfs extend_file: could not find pointer to truncate at");
trunc_found_ptr:

  /* Mark the extension header needs to be written out */

  mark_exthdr_dirty (extfilex, file);

  /* Free off the last part of the pointer blocks and keep the first part.            */
  /* Note that in the case relblock = pointer -> blockcount, we keep the whole thing. */

  if (pointer -> blockcount > relblock) {
    nfound = pointer -> blockcount - relblock;
    file -> allocblocks     -= nfound;
    extfilex -> blocksinhdr -= nfound;
    free_blocks (volume, nfound, pointer -> blockcount + relblock, iopex);
  }
  (pointer ++) -> blockcount = relblock;
  extfilex -> header.areas[OZ_FS_HEADER_AREA_POINTER].size = (++ i) * sizeof (Pointer);

  /* Release all the blocks for whatever is left in the header */

trunc_found_hdr:
  savei = i;
  for (; i < extfilex -> header.areas[OZ_FS_HEADER_AREA_POINTER].size / sizeof (Pointer); i ++) {
    nfound = pointer -> blockcount;
    file -> allocblocks     -= nfound;
    extfilex -> blocksinhdr -= nfound;
    free_blocks (volume, nfound, pointer -> logblock, iopex);
    pointer ++;
  }
  extfilex -> header.areas[OZ_FS_HEADER_AREA_POINTER].size = savei * sizeof (Pointer);

  /* Release all the blocks for all extension headers following this one */

  while ((extfilex2 = extfilex -> next) != NULL) {
    pointer = POINTERS (extfilex2);
    for (i = extfilex2 -> header.areas[OZ_FS_HEADER_AREA_POINTER].size / sizeof (Pointer); i > 0; -- i) {
      nfound = pointer -> blockcount;
      file -> allocblocks      -= nfound;
      extfilex2 -> blocksinhdr -= nfound;
      free_blocks (volume, nfound, pointer -> logblock, iopex);
      pointer ++;
    }
    extfilex2 -> header.areas[OZ_FS_HEADER_AREA_POINTER].size = 0;
    extfilex -> next = extfilex2 -> next;
    delete_header (extfilex2, iopex);
  }

  /* Set my extension file-id to zeroes */

  memset (&(extfilex -> header.extid), 0, sizeof extfilex -> header.extid);

  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Insert blocks into a file						*/
/*									*/
/*    Input:								*/
/*									*/
/*	file     = file block pointer					*/
/*	nblocks  = number of blocks to insert				*/
/*	atblock  = where to insert them (vbn of first new block)	*/
/*									*/
/*    Output:								*/
/*									*/
/*	insert_blocks = OZ_SUCCESS : extend was successful		*/
/*	                      else : error status			*/
/*									*/
/************************************************************************/

static uLong insert_blocks (OZ_VDFS_File *file, OZ_Dbn nblocks, OZ_Dbn atblock, OZ_VDFS_Iopex *iopex)

{
  OZ_Dbn *ba1, *ba2, logblock, nb1, nb2, nbi, nfound, relblock;
  OZ_VDFS_Filex *extfilex, *extfilex2, *extfilex3;
  OZ_VDFS_Volex *volex;
  OZ_VDFS_Volume *volume;
  Pointer *pointer, *pointer2, *pointer3;
  uLong numpoints, sts;

  volume = file -> volume;
  volex  = volume -> volex;

  if (nblocks == 0) return (OZ_SUCCESS);
  if (nblocks % volex -> homeblock.clusterfactor != 0) return (OZ_BADBLOCKNUMBER);

#if 000
  nb1 = countblocksinfile (file, &ba1);
  nbi = nblocks;
#endif

  /* Find the extension to insert into */

  relblock = atblock;
  if (relblock == 0) return (OZ_VBNZERO);
  relblock --;
  if (relblock % volex -> homeblock.clusterfactor != 0) return (OZ_BADBLOCKNUMBER);
  for (extfilex = file -> filex; extfilex != NULL; extfilex = extfilex -> next) {
    if (extfilex -> blocksinhdr >= relblock) goto insert_it;
    relblock -= extfilex -> blocksinhdr;
  }
  return (OZ_ENDOFFILE);

insert_it:

  /* Find the pointer we want to insert just before */

  numpoints = extfilex -> header.areas[OZ_FS_HEADER_AREA_POINTER].size / sizeof *pointer;
  pointer   = POINTERS (extfilex);
  logblock  = 0;
  while ((relblock != 0) && (pointer -> blockcount <= relblock)) {
    if (numpoints == 0) oz_crash ("oz_dev_dfs insert_blocks: bad filex %p -> blocksinhdr %u", extfilex, extfilex -> blocksinhdr);
    logblock  = pointer -> logblock + pointer -> blockcount;
    relblock -= pointer -> blockcount;
    numpoints --;
    pointer ++;
  }

  /* Allocate the required space.  Assume we can do it contiguously, as we are really only ever asked for one or two clusters. */

  sts = allocate_blocks (volume, nblocks, logblock, &nfound, &logblock, iopex);
  if (sts != OZ_SUCCESS) return (sts);
  if (nfound != nblocks) {
    oz_dev_vdfs_printk (iopex, "oz_dev_dfs insert_blocks: nfound %u, nblocks %u\n", nfound, nblocks);
    free_blocks (volume, nfound, logblock, iopex);
    return (OZ_DISKISFULL);
  }

  /* Maybe the new blocks get inserted in the middle of an existing pointer */

  if (relblock != 0) {

    /* If so, triplicate the pointer */

    sts = makeroomforapointer (file, extfilex, &pointer, &extfilex2, &pointer2, iopex);
    if (sts != OZ_SUCCESS) goto rtnerr;
    sts = makeroomforapointer (file, extfilex2, &pointer2, &extfilex3, &pointer3, iopex);
    if (sts != OZ_SUCCESS) {
      extfilex -> blocksinhdr -= pointer -> blockcount;
      extfilex -> header.areas[OZ_FS_HEADER_AREA_POINTER].size -= sizeof *pointer;
      memmove (pointer, pointer + 1, extfilex -> header.area 
                                   + extfilex -> header.areas[OZ_FS_HEADER_AREA_POINTER].offs 
                                   + extfilex -> header.areas[OZ_FS_HEADER_AREA_POINTER].size 
                                   - (uByte *)pointer);
      goto rtnerr;
    }

    /* The original covers the old blocks before the new blocks  */
    /* The second one covers the new blocks                      */
    /* The third one convers the old blocks after the new blocks */

    extfilex -> blocksinhdr -= pointer -> blockcount - relblock;
    extfilex2 -> blocksinhdr -= pointer2 -> blockcount - nfound;
    extfilex3 -> blocksinhdr -= relblock;

    pointer -> blockcount   = relblock;
    pointer2 -> blockcount  = nfound;
    pointer2 -> logblock    = logblock;
    pointer3 -> blockcount -= relblock;
    pointer3 -> logblock   += relblock;

    goto rtnsuc;
  }

  /* Maybe new blocks fit exactly on end of the last pointer in header */

  if ((pointer > POINTERS (extfilex)) && (pointer[-1].blockcount + pointer[-1].logblock == logblock)) {
    pointer[-1].blockcount  += nfound;
    extfilex -> blocksinhdr += nfound;
    goto rtnsuc;
  }

  /* Maybe new blocks fit exactly before the next pointer in header */

  if ((numpoints > 0) && (nfound + logblock == pointer[0].logblock)) {
    pointer[0].blockcount   += nfound;
    pointer[0].logblock      = logblock;
    extfilex -> blocksinhdr += nfound;
    goto rtnsuc;
  }

  /* Duplicate the pointer pointed to by 'pointer' */

  extfilex2 = (void *)0x2D2D2D2D;
  pointer2  = (void *)0xD2D2D2D2;
  sts = makeroomforapointer (file, extfilex, &pointer, &extfilex2, &pointer2, iopex);
  if (sts != OZ_SUCCESS) goto rtnerr;

  /* Replace the original one with the new blocks */

  if (pointer == NULL) {
    extfilex = extfilex2;
    pointer  = pointer2;
  }
  extfilex -> blocksinhdr += nfound - pointer -> blockcount;
  pointer -> blockcount    = nfound;
  pointer -> logblock      = logblock;

  /* File now has that many more blocks */

rtnsuc:
  file -> allocblocks += nfound;
#if 000
  nb2 = countblocksinfile (file, &ba2);
  if (nb2 != nb1 + nbi) oz_crash ("oz_dev_dfs insert_blocks: initial count %u, inserted %u, now have %u", nb1, nbi, nb2);
  compareblockarrays (nb1, ba1, nb2, ba2, atblock, logblock);
#endif
  return (OZ_SUCCESS);

  /* Couldn't make room in header for pointer, free blocks and return error status */

rtnerr:
  free_blocks (volume, nfound, logblock, iopex);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Remove blocks from a file						*/
/*									*/
/*    Input:								*/
/*									*/
/*	file     = file block pointer					*/
/*	nblocks  = number of blocks to remove				*/
/*	atblock  = first block to be removed				*/
/*									*/
/*    Output:								*/
/*									*/
/*	remove_blocks = OZ_SUCCESS : remove was successful		*/
/*	                      else : error status			*/
/*									*/
/************************************************************************/

static uLong remove_blocks (OZ_VDFS_File *file, OZ_Dbn nblocks, OZ_Dbn atblock, OZ_VDFS_Iopex *iopex)

{
  OZ_Dbn *ba1, *ba2, freedblock, nb1, nb2, nbr, numblocks, relblock;
  OZ_VDFS_Filex *extfilex, *extfilex2;
  OZ_VDFS_Volex *volex;
  OZ_VDFS_Volume *volume;
  Pointer *pointer, *pointer2;
  uLong nfound, numpoints, numtempoints, sts;

  volume = file -> volume;
  volex  = volume -> volex;

  if (nblocks == 0) return (OZ_SUCCESS);
  if (nblocks % volex -> homeblock.clusterfactor != 0) return (OZ_BADBLOCKNUMBER);

#if 000
  nb1 = countblocksinfile (file, &ba1);
  nbr = nblocks;
#endif

  /* Find the extension to remove from */

  relblock = atblock;
  if (relblock == 0) return (OZ_VBNZERO);
  relblock --;
  if (relblock % volex -> homeblock.clusterfactor != 0) return (OZ_BADBLOCKNUMBER);
  for (extfilex = file -> filex; extfilex != NULL; extfilex = extfilex -> next) {
    if (extfilex -> blocksinhdr > relblock) goto remove_it;
    relblock -= extfilex -> blocksinhdr;
  }
  return (OZ_ENDOFFILE);

  /* Remove the blocks from the appropriate header */

remove_it:
  numpoints = extfilex -> header.areas[OZ_FS_HEADER_AREA_POINTER].size / sizeof *pointer;	// get number of pointers in the header
  pointer   = POINTERS (extfilex);								// point to the pointers
  while (nblocks != 0) {

    /* Make sure we have a pointer to look at.  If not, jump to next extension header. */

    while (numpoints == 0) {
      extfilex = extfilex -> next;
      if (extfilex == NULL) return (OZ_ENDOFFILE);
      numpoints = extfilex -> header.areas[OZ_FS_HEADER_AREA_POINTER].size / sizeof *pointer;
      pointer   = POINTERS (extfilex);
    }

    /* See if we are removing anything in this pointer */

    if (pointer -> blockcount <= relblock) {
      relblock -= pointer -> blockcount;
      -- numpoints;
      pointer ++;
      continue;
    }

    /* See if removal point is at beginning of current pointer */

    if (relblock == 0) {

      /* See if there will be anything left after we remove what we want to */

      if (pointer -> blockcount > nblocks) {
        freedblock = pointer -> logblock;
        sts = free_blocks (volume, nblocks, pointer -> logblock, iopex);	// remove wanted number of blocks from beginning
        if (sts != OZ_SUCCESS) return (sts);
        pointer -> blockcount -= nblocks;					// reduce number of remaining blocks
        pointer -> logblock   += nblocks;					// increment address of remaining blocks
        mark_exthdr_dirty (extfilex, file);					// this header is dirty
        file -> allocblocks     -= nblocks;					// file has this fewer blocks now
        extfilex -> blocksinhdr -= nblocks;					// header has this fewer blocks now
        break;									// we removed all we want to
      }

      /* Nothing will be left, remove the whole pointer */

      freedblock = pointer -> logblock;
      sts = free_blocks (volume, pointer -> blockcount, pointer -> logblock, iopex);	// free all blocks in pointer
      if (sts != OZ_SUCCESS) return (sts);
      nblocks -= pointer -> blockcount;							// reduce the number left to remove
      file -> allocblocks     -= pointer -> blockcount;					// file has this fewer blocks now
      extfilex -> blocksinhdr -= pointer -> blockcount;					// header has this fewer blocks now
      memmove (pointer, pointer + 1, (-- numpoints) * sizeof *pointer);			// shuffle the pointers down
      extfilex -> header.areas[OZ_FS_HEADER_AREA_POINTER].size -= sizeof *pointer;	// reduce number of pointers in header
      mark_exthdr_dirty (extfilex, file);						// this header is dirty
      continue;										// continue to remove the rest, if any
    }

    /* See if we are removing from somewhere in the middle all the way thru the end of the pointer */

    if (pointer -> blockcount <= relblock + nblocks) {
      freedblock = pointer -> logblock + relblock;
      sts = free_blocks (volume, pointer -> blockcount - relblock, pointer -> logblock + relblock, iopex);
      if (sts != OZ_SUCCESS) return (sts);
      file -> allocblocks     -= pointer -> blockcount - relblock;			// file has this fewer blocks now
      extfilex -> blocksinhdr -= pointer -> blockcount - relblock;			// header has this fewer blocks now
      nblocks -= pointer -> blockcount - relblock;					// we have yet to remove these blocks
      pointer -> blockcount = relblock;							// the pointer has only this many blocks now
      mark_exthdr_dirty (extfilex, file);						// this header is dirty
      relblock = 0;									// we start at beginning of next pointer
      -- numpoints;
      pointer ++;
      continue;										// continue to remove the rest, if any
    }

    /* We are removing some blocks from the middle of a pointer, we have to split it */

    sts = makeroomforapointer (file, extfilex, &pointer, &extfilex2, &pointer2, iopex);
    if (sts != OZ_SUCCESS) return (sts);

    freedblock = pointer -> logblock + relblock;
    sts = free_blocks (volume, nblocks, pointer -> logblock + relblock, iopex);		// free the blocks off
    if (sts != OZ_SUCCESS) return (sts);

    pointer -> blockcount   = relblock;							// old pointer is up to removal point
    pointer2 -> blockcount -= relblock + nblocks;					// new pointer is everything after
    pointer2 -> logblock   += relblock + nblocks;

    extfilex -> blocksinhdr  -= pointer2 -> blockcount + nblocks;			// old header has this fewer blocks now
    extfilex2 -> blocksinhdr -= relblock + nblocks;					// new header has this fewer blocks now

    mark_exthdr_dirty (extfilex,  file);						// mark both headers dirty
    mark_exthdr_dirty (extfilex2, file);
    break;
  }

#if 000
  nb2 = countblocksinfile (file, &ba2);
  if (nb2 + nbr != nb1) oz_crash ("oz_dev_dfs remove_blocks: initial count %u, removed %u, now have %u", nb1, nbr, nb2);
  compareblockarrays (nb2, ba2, nb1, ba1, atblock, freedblock);
#endif

  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Make room in a file extension header for another pointer at an 	*/
/*  arbitrary point							*/
/*									*/
/*    Input:								*/
/*									*/
/*	file        = file the extension header belongs to		*/
/*	extfile     = extension header to put the new pointer in	*/
/*	**pointer_p = spot in extension header where new pointer goes	*/
/*									*/
/*    Output:								*/
/*									*/
/*	makeroomforapointer = OZ_SUCCESS : successful completion	*/
/*	                            else : error status			*/
/*	pointer[0] = same as on input					*/
/*	*pointer_p = points to original copy of pointer			*/
/*	*pointer_r = points to copy of original pointer, but in the 	*/
/*	             next spot, possibly in a new extension header	*/
/*	*extfilex_r = the extension header that contains *pointer_r	*/
/*									*/
/*	the blocksinhdr has been updated to includes block from 	*/
/*	duplicated pointer						*/
/*									*/
/************************************************************************/

static uLong makeroomforapointer (OZ_VDFS_File *file, OZ_VDFS_Filex *extfilex, Pointer **pointer_p, OZ_VDFS_Filex **extfilex_r, Pointer **pointer_r, OZ_VDFS_Iopex *iopex)

{
  OZ_VDFS_Filex *extfilex2, *extfilex3;
  Pointer *pointer, *pointer2;
  uLong numpoints, pointidx, sts;

  /* Get number of pointers in the existing header starting with 'pointer' */

  pointer  = *pointer_p;
  pointidx = pointer - POINTERS (extfilex);
  if (pointidx > extfilex -> header.areas[OZ_FS_HEADER_AREA_POINTER].size / sizeof *pointer) {
    oz_crash ("oz_dev_dfs makeroomforapointer: bad pointer %p for filex %p", pointer, extfilex);
  }
  numpoints = extfilex -> header.areas[OZ_FS_HEADER_AREA_POINTER].size / sizeof *pointer - pointidx;

  /* Hopefully there is room in the current header for the new pointer */

  sts = make_header_room (file, extfilex, sizeof *pointer, OZ_FS_HEADER_AREA_POINTER, 0, &extfilex2, iopex);
  if (sts == OZ_SUCCESS) {
    if (extfilex2 != extfilex) oz_crash ("oz_dev_dfs makeroomforapointer: extfilex %p, extfilex2 %p", extfilex, extfilex2);
    extfilex -> header.areas[OZ_FS_HEADER_AREA_POINTER].size += sizeof *pointer;	// header has another pointer now
    mark_exthdr_dirty (extfilex, file);							// header is dirty now
    pointer = POINTERS (extfilex) + pointidx;						// maybe make_header_room moved pointers
    if (numpoints == 0) {
      pointer -> blockcount = 0;
      pointer -> logblock   = 0;
      *pointer_p  = NULL;
      *extfilex_r = extfilex;
      *pointer_r  = pointer;
    } else {
      memmove (pointer + 1, pointer, numpoints * sizeof *pointer);			// shift everything down
											// leave original pointer intact
      extfilex -> blocksinhdr += pointer[0].blockcount;					// account for more blocks now
      *pointer_p  = pointer;								// return where original is
      *extfilex_r = extfilex;								// return where copy is
      *pointer_r  = pointer + 1;
    }
  }
  if (sts != OZ_FILEHDRFULL) return (sts);						// abort if serious error

  /* Maybe there already is an extension header that has room for another pointer */

  extfilex2 = extfilex -> next;
  if (extfilex2 != NULL) {
    sts = make_header_room (file, extfilex2, sizeof *pointer, OZ_FS_HEADER_AREA_POINTER, 0, &extfilex3, iopex);
    if (sts == OZ_SUCCESS) {
      if (extfilex3 != extfilex2) oz_crash ("oz_dev_dfs makeroomforapointer: extfilex3 != extfilex2 = %p", extfilex2);
      goto use_ext2;
    }
    if (sts != OZ_FILEHDRFULL) return (sts);
  }

  /* Need a completely new header */

  sts = make_header_room (file, extfilex, sizeof *pointer, OZ_FS_HEADER_AREA_POINTER, 1, &extfilex2, iopex);
  if (sts != OZ_SUCCESS) return (sts);
  if (extfilex2 == extfilex) oz_crash ("oz_dev_dfs makeroomforapointer: extfilex2=extfilex=%p", extfilex);

use_ext2:
  pointer  = POINTERS (extfilex) + pointidx;							// maybe make_header_room moved pointers
  pointer2 = POINTERS (extfilex2);								// get where new pointer goes
  memmove (pointer2 + 1, pointer2, extfilex2 -> header.areas[OZ_FS_HEADER_AREA_POINTER].size);	// shift things down to make room
  extfilex2 -> header.areas[OZ_FS_HEADER_AREA_POINTER].size += sizeof *pointer;			// new header now has another pointer
  if (numpoints == 0) {										// see if inserting at eof
    *pointer_p  = NULL;										// if so, there is no original ptr
    *extfilex_r = extfilex2;									// set up new pointer
    *pointer_r  = pointer2;
    pointer2 -> blockcount = 0;									// ... and make it zero blocks
    pointer2 -> logblock   = 0;
  } else {
    *pointer_p  = pointer;									// return original pointer
    pointer2[0] = pointer[--numpoints];								// move last from old hdr to new hdr
    if (numpoints == 0) {									// see if that was the one to duplicate
      *extfilex_r = extfilex2;									// if so, that's where the new one is
      *pointer_r  = pointer2;
      extfilex2 -> blocksinhdr += pointer2[0].blockcount;
    } else {
      extfilex  -> blocksinhdr -= pointer2[0].blockcount;					// no, remove the blocks from old header
      extfilex2 -> blocksinhdr += pointer2[0].blockcount;					// and add the blocks to new header
      memmove (pointer + 1, pointer, numpoints * sizeof *pointer);				// shift old pointers down a spot
      extfilex  -> blocksinhdr += pointer[0].blockcount;					// ... and that duplicates the wanted pointer
      *extfilex_r = extfilex;									// this is where the copy is
      *pointer_r  = pointer + 1;
    }
  }
  mark_exthdr_dirty (extfilex, file);								// headers are dirty now
  mark_exthdr_dirty (extfilex2, file);
  return (OZ_SUCCESS);
}

static OZ_Dbn countblocksinfile (OZ_VDFS_File *file, OZ_Dbn **blockarray_r)

{
  OZ_Dbn *blockarray, nblocksinhdr, nblockstotal;
  OZ_VDFS_Filex *filex;
  Pointer *pointer;
  uLong i, j;

  nblockstotal = 0;
  for (filex = file -> filex; filex != NULL; filex = filex -> next) {
    nblocksinhdr = 0;
    pointer = POINTERS (filex);
    for (i = filex -> header.areas[OZ_FS_HEADER_AREA_POINTER].size / sizeof *pointer; i > 0; -- i) {
      nblocksinhdr += pointer -> blockcount;
      pointer ++;
    }
    if (nblocksinhdr != filex -> blocksinhdr) oz_crash ("oz_dev_dfs countblocksinfile: filex %p -> blocksinhdr %u, but counted %u", filex, filex -> blocksinhdr, nblocksinhdr);
    nblockstotal += nblocksinhdr;
  }
  if (nblockstotal != file -> allocblocks) oz_crash ("oz_dev_dfs countblocksinfile: file %p -> allocblocks %u, but counted %u", file, file -> allocblocks, nblockstotal);

  blockarray = NULL;
  if (nblockstotal != 0) {
    blockarray = OZ_KNL_PGPMALLOC (nblockstotal * sizeof *blockarray);
    nblockstotal = 0;
    for (filex = file -> filex; filex != NULL; filex = filex -> next) {
      pointer = POINTERS (filex);
      for (i = 0; i < filex -> header.areas[OZ_FS_HEADER_AREA_POINTER].size / sizeof *pointer; i ++) {
        for (j = 0; j < pointer[i].blockcount; j ++) blockarray[nblockstotal++] = pointer[i].logblock + j;
      }
    }
    for (i = 0; i < nblockstotal; i ++) {
      for (j = i; ++ j < nblockstotal;) {
        if (blockarray[i] == blockarray[j]) {
          oz_crash ("oz_dev_dfs countblocksinfile: blockarray[%u] = [%u], nblockstotal %u", i, j, nblockstotal);
        }
      }
    }
  }
  if (blockarray_r != NULL) *blockarray_r = blockarray;
  else if (blockarray != NULL) OZ_KNL_PGPFREE (blockarray);

  return (nblockstotal);
}

static void compareblockarrays (OZ_Dbn nbsmall, OZ_Dbn *basmall, OZ_Dbn nbbig, OZ_Dbn *babig, OZ_Dbn atblock, OZ_Dbn logblock)

{
  OZ_Dbn i, j;

  -- atblock;

  if (atblock > nbsmall) oz_crash ("oz_dev_dfs compareblockarrays: atblock %u, nbsmall %u", atblock, nbsmall);
  if (atblock > nbbig)   oz_crash ("oz_dev_dfs compareblockarrays: atblock %u, nbbig %u", atblock, nbbig);
  for (i = 0; i < atblock; i ++) {
    if (basmall[i] != babig[i]) oz_crash ("oz_dev_dfs compareblockarrays: [%u] bad", i);
  }
  for (j = 0; j < nbbig - nbsmall; j ++) {
    if (babig[i+j] != logblock + j) oz_crash ("oz_dev_dfs compareblockarrays: babig[%u] bad", i + j);
  }
  for (; i < nbsmall; i ++) {
    if (basmall[i] != babig[i+j]) oz_crash ("oz_dev_dfs compareblockarrays: basmall[%u] vs babig[%u] bad", i, i + j);
  }

  if (basmall != NULL) OZ_KNL_PGPFREE (basmall);
  if (babig   != NULL) OZ_KNL_PGPFREE (babig);
}

/************************************************************************/
/*									*/
/*  Make room in header for one of the size/offs items			*/
/*									*/
/*    Input:								*/
/*									*/
/*	filex    = header to make room in				*/
/*	roomsize = amount of room to add				*/
/*	narea    = area to add room size to				*/
/*	exthdr   = 0 : don't add extension header			*/
/*	           1 : ok to add extension header if needed		*/
/*									*/
/*    Output:								*/
/*									*/
/*	make_header_room = OZ_SUCCESS : room successfully created	*/
/*	                         else : error status			*/
/*	**filex_r = file extension header the room is in		*/
/*	size not updated to include new room, caller must do that	*/
/*									*/
/************************************************************************/

static uLong hdrareausedsize (OZ_VDFS_Filex *filex);

static uLong make_header_room (OZ_VDFS_File *file, OZ_VDFS_Filex *filex, uWord roomsize, int narea, int exthdr, OZ_VDFS_Filex **filex_r, OZ_VDFS_Iopex *iopex)

{
  int i;
  OZ_VDFS_Filex *extfilex;
  OZ_VDFS_Volume *volume;
  uLong areasize, areaoffs, size, sts;

  volume = iopex -> devex -> volume;

  /* Round up roomsize to longword boundary */

  roomsize = (roomsize + 3) & -4;

  /* Calculate size of the entire 'area[]' in the header for the blocksize, rounded down to longword size */

  areasize = (((uByte *)&(filex -> header)) + volume -> volex -> homeblock.blocksize - filex -> header.area) & -4;

  /* Make sure they're not asking for too much */

  if (roomsize > areasize) oz_crash ("oz_dev_dfs make_header_room: area %u too big for header (max %u)", roomsize, areasize);

  /* If there isn't enough room for new stuff, allocate and link up an extension header */

  if (hdrareausedsize (filex) + roomsize > areasize) {
    if (!exthdr) return (OZ_FILEHDRFULL);				// maybe caller doesn't want one

    /* If there already is an extension header that doesn't have anything for */
    /* this area, and it has enough room for what we're adding, just use it   */

    if (((extfilex = filex -> next) != NULL) 
     && (extfilex -> header.areas[narea].size == 0) 
     && (hdrareausedsize (extfilex) + roomsize <= areasize)) filex = extfilex;

    /* Otherwise, try to allocate a new one and link it up */

    else {
      sts = allocate_header (volume, &extfilex, iopex);			// allocate an file header
      if (sts != OZ_SUCCESS) return (sts);

      extfilex -> next = filex -> next;					// set up its in-memory link to next extension header
      extfilex -> header.extid    = filex -> header.extid;		// set up its on-disk link to next extension header
      extfilex -> header.dirid    = filex -> header.fileid;		// set up its on-disk link to prev extension header
      extfilex -> header.dircount = EXT_DIRCOUNT;			// mark it as an extension header
      filex -> next = extfilex;						// make new header follow previous header
      filex -> header.extid = extfilex -> header.fileid;		// (also set previous header's on-disk pointer)
      mark_exthdr_dirty (filex, file);					// write mods to previous header

      filex = extfilex;							// it is the header with the room in it

      extfilex = filex -> next;						// see if there is an following extension header
      if (extfilex != NULL) {
        extfilex -> header.dirid = filex -> header.fileid;		// if so, point its on-disk back pointer to new header
        mark_exthdr_dirty (extfilex, file);
      }
    }
  }

  *filex_r = filex;

  /* It will fit in the current header */

  /* Move stuff that follows it all the way to the end of the header.  This will    */
  /* allow for maximal expansion of the area without having to move anything again. */

  for (i = OZ_FS_HEADER_NAREAS; -- i > narea;) {
    size = filex -> header.areas[i].size;
    if (size != 0) {
      size = (size + 3) & -4;
      areasize -= size;
      if (filex -> header.areas[i].offs != areasize) {
        memmove (filex -> header.area + areasize, filex -> header.area + filex -> header.areas[i].offs, size);
      }
    }
    filex -> header.areas[i].offs = areasize;
  }

  /* Move it and all stuff before it to the beginning of the header (same reason) */

  areaoffs = 0;
  for (i = 0; i <= narea; i ++) {
    size = (filex -> header.areas[i].size + 3) & -4;
    if ((size != 0) && (filex -> header.areas[i].offs != areaoffs)) {
      memmove (filex -> header.area + areaoffs, filex -> header.area + filex -> header.areas[i].offs, size);
    }
    filex -> header.areas[i].offs = areaoffs;
    areaoffs += size;
  }

  /* Now there should be at least 'roomsize' bytes left */

  if (areaoffs + roomsize > areasize) oz_crash ("oz_dev_dfs make_header_room: failed to make room");

  /* Garbage fill all the new room that was made */

  memset (filex -> header.area + areaoffs, 0xEA, areasize - areaoffs);

  /* Header needs to be written back to disk */

  mark_exthdr_dirty (filex, file);

  return (OZ_SUCCESS);
}

static uLong hdrareausedsize (OZ_VDFS_Filex *filex)

{
  int i;
  uLong usedsize;

  usedsize = 0;
  for (i = 0; i < OZ_FS_HEADER_NAREAS; i ++) usedsize += (filex -> header.areas[i].size + 3) & -4;
  return (usedsize);
}

/************************************************************************/
/*									*/
/*  Allocate a file header from the index header file			*/
/*									*/
/*    Input:								*/
/*									*/
/*	volume = volume block pointer					*/
/*									*/
/*    Output:								*/
/*									*/
/*	allocate_header = OZ_SUCCESS : successful			*/
/*	                        else : error status			*/
/*	*filex_r = filled in with new header				*/
/*									*/
/************************************************************************/

static uLong allocate_header (OZ_VDFS_Volume *volume, OZ_VDFS_Filex **filex_r, OZ_VDFS_Iopex *iopex)

{
  OZ_Dbn extblocks, indexheaderefblk, indexheaderhiblk;
  OZ_VDFS_File *indexbitmap, *indexheaders;
  OZ_VDFS_Fileid savefid;
  OZ_VDFS_Filex *filex;
  OZ_VDFS_Volex *volex;
  uLong bitsinblock, bytesinblock, filenum, *indexbitmapblock, sts, vl;

  volex = volume -> volex;

  bytesinblock = volex -> homeblock.blocksize;
  bitsinblock  = bytesinblock * 8;
  indexbitmap  = volex -> indexbitmap;
  indexheaders = volex -> indexheaders;

  /* Allocate a filex struct for the file header and clear the fixed part */

  filex = OZ_KNL_PGPMALLOQ (((uByte *)&(filex -> header)) + bytesinblock - (uByte *)filex);
  if (filex == NULL) return (OZ_EXQUOTAPGP);
  memset (filex, 0, ((uByte *)&(filex -> header)) - (uByte *)filex);

  /* We use the header buffer for the index bitmap block buffer */

  indexbitmapblock = (uLong *)&(filex -> header);

  /* Get index header block number that contains the last full index header block  */
  /* If there are any partial bytes left in the last block, ignore them            */
  /* This is the number of headers in the file that have been properly initialized */

  indexheaderefblk  = indexheaders -> attrlock_efblk;
  indexheaderefblk += indexheaders -> attrlock_efbyt / bytesinblock;
  indexheaderefblk --;

  /* Scan through index bitmap for a free header just up through the end of the existing index header file */
  /* Note that filenum used here is based at zero, not the customary one.                                  */

rescan:
  for (filenum = 0; filenum + 31 < indexheaderefblk; filenum += 32) {
    if ((filenum % bitsinblock) == 0) {
      sts = oz_dev_vdfs_readvirtblock (indexbitmap, filenum / bitsinblock + 1, 0, bytesinblock, indexbitmapblock, iopex, 0);
      if (sts != OZ_SUCCESS) goto bmreaderr;
    }
    if (indexbitmapblock[(filenum%bitsinblock)/32] != 0xFFFFFFFF) goto found_free_header;
  }
  if (filenum < indexheaderefblk) {
    if ((filenum % bitsinblock) == 0) {
      sts = oz_dev_vdfs_readvirtblock (indexbitmap, filenum / bitsinblock + 1, 0, bytesinblock, indexbitmapblock, iopex, 0);
      if (sts != OZ_SUCCESS) goto bmreaderr;
    }
    if ((indexbitmapblock[(filenum%bitsinblock)/32] | (0xFFFFFFFF << (indexheaderefblk % 32))) != 0xFFFFFFFF) goto found_free_header;
  }
  filenum = indexheaderefblk;

  /* Extend index header file by the number of bits in a cluster so we hopefully can get a whole bitmap cluster worth of bits             */
  /* Note that the index header file may already have extra blocks left over from an earlier extension attempt                            */
  /* We can't extend the index header itself, as it would lead to an infinite recursion, but the cf_default routine tries to prevent this */

  indexheaderhiblk = indexheaders -> allocblocks;
  if (indexheaderhiblk == indexheaderefblk) {
    extblocks = ((volex -> homeblock.clustersfree * volex -> homeblock.clusterfactor) / (volex -> homeblock.clusterfactor + 1)) + 1;	// make sure leave room for one cluster of file data per header
    if (extblocks > bitsinblock * volex -> homeblock.clusterfactor) extblocks = bitsinblock * volex -> homeblock.clusterfactor;		// ... but only extend for at most one cluster of indexbitmap bits
    sts = dfs_extend_file (indexheaders, indexheaderhiblk + extblocks, EXTEND_NOTRUNC | EXTEND_NOEXTHDR, iopex);			// try to extend the index header file
    if (sts != OZ_SUCCESS) {
      oz_dev_vdfs_printk (iopex, "oz_dev_dfs allocate_header: error %u extending index header file\n", sts);
      goto rtnerr;
    }
    indexheaderhiblk = indexheaders -> allocblocks;											// get the new number of blocks in index header file
  }

  /* Now extend the index bitmap file to include all the blocks that are in the index header file */
  /* Note that it may already be long enough from an earlier extension attempt                    */

  sts = dfs_extend_file (indexbitmap, (indexheaderhiblk + bitsinblock - 1) / bitsinblock, EXTEND_NOTRUNC, iopex);
  if (sts != OZ_SUCCESS) {
    oz_dev_vdfs_printk (iopex, "oz_dev_dfs allocate_header: error %u extending index bitmap file\n", sts);
    goto rtnerr;
  }

  /* Both have been successfully extended */

  /* If we are starting a new index bitmap block, clear the whole thing to zeroes and move the end-of-file pointer */
  /* If not, we will just continue using the existing index bitmap block we already have.                          */

  if ((filenum % bitsinblock) == 0) {
    memset (indexbitmapblock, 0, bytesinblock);
    vl = oz_hw_smplock_wait (&(indexbitmap -> attrlock_vl));
    indexbitmap -> attrlock_efblk  = filenum / bitsinblock + 2;
    indexbitmap -> attrlock_efbyt  = 0;
    indexbitmap -> attrlock_date   = oz_hw_tod_getnow ();
    indexbitmap -> attrlock_flags |= OZ_VDFS_ALF_M_EOF | OZ_VDFS_ALF_M_ADT | OZ_VDFS_ALF_M_CDT | OZ_VDFS_ALF_M_MDT;
    oz_hw_smplock_clr (&(indexbitmap -> attrlock_vl), vl);
    oz_dev_vdfs_mark_header_dirty (indexbitmap);
  }

  /* filenum is the number of a free index header */

found_free_header:
  while (indexbitmapblock[(filenum%bitsinblock)/32] & (1 << (filenum % 32))) filenum ++;

  /* Set index bitmap file bit saying the header is now in use */

  indexbitmapblock[(filenum%bitsinblock)/32] |= 1 << (filenum % 32);
  sts = oz_dev_vdfs_writevirtblock (indexbitmap, filenum / bitsinblock + 1, 0, bytesinblock, indexbitmapblock, iopex, 0);
  if (sts != OZ_SUCCESS) {
    oz_dev_vdfs_printk (iopex, "oz_dev_dfs allocate_header: error %u writing index bit map block for file %u\n", sts, filenum + 1);
    goto rtnerr;
  }

  /* filenum above was based at zero.  Now base it at one like everybody else uses it. */

  filenum ++;

  /* If the new header is within the eof boundary of the index file, read existing block to make sure it is free and to increment */
  /* the sequence number.  If the new header is beyond the index file eof, start sequence number at one, don't bother reading it. */

  if (filenum > indexheaderefblk) {
    vl = oz_hw_smplock_wait (&(indexheaders -> attrlock_vl));
    indexheaders -> attrlock_efblk  = filenum + 1;
    indexheaders -> attrlock_efbyt  = 0;
    indexheaders -> attrlock_date   = oz_hw_tod_getnow ();
    indexheaders -> attrlock_flags |= OZ_VDFS_ALF_M_EOF | OZ_VDFS_ALF_M_ADT | OZ_VDFS_ALF_M_CDT | OZ_VDFS_ALF_M_MDT;
    oz_hw_smplock_clr (&(indexheaders -> attrlock_vl), vl);
    oz_dev_vdfs_mark_header_dirty (indexheaders);
    savefid.seq = 0;
  } else {
    sts = oz_dev_vdfs_readvirtblock (indexheaders, filenum, 0, bytesinblock, &(filex -> header), iopex, 0);
    if (sts != OZ_SUCCESS) {
      oz_dev_vdfs_printk (iopex, "oz_dev_dfs allocate_header: error %u reading block %u from index file\n", sts, filenum);
      goto rtnerr;
    }
    if (filex -> header.fileid.num != 0) {
      oz_dev_vdfs_printk (iopex, "oz_dev_dfs allocate_header: header %u was marked in index bitmap as free but fileid.num is non-zero\n", filex -> header.fileid.num);
      goto rescan;
    }
    savefid = filex -> header.fileid;
  }

  /* Set up skeleton values for the header */

  memset (&(filex -> header), 0, bytesinblock);

  if (++ savefid.seq == 0) ++ savefid.seq;

  filex -> header.headerver  = HEADER_VERSION;
  filex -> header.fileid.num = filenum;
  filex -> header.fileid.seq = savefid.seq;
  filex -> header.fileid.rvn = 1;

  *filex_r = filex;

  return (OZ_SUCCESS);

bmreaderr:
  oz_dev_vdfs_printk (iopex, "oz_dev_dfs allocate_header: error %u reading index bitmap block %u\n", sts, filenum / bitsinblock + 1);
rtnerr:
  OZ_KNL_PGPFREE (filex);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Delete file header on disk - make it available for re-allocation	*/
/*									*/
/*    Input:								*/
/*									*/
/*	filex = file header to be deleted				*/
/*									*/
/*    Output:								*/
/*									*/
/*	filex struct freed off						*/
/*	header made available for re-use on disk			*/
/*									*/
/*    Note:								*/
/*									*/
/*	Header is assumed to contain no blocks				*/
/*									*/
/************************************************************************/

static void delete_header (OZ_VDFS_Filex *filex, OZ_VDFS_Iopex *iopex)

{
  uLong bitinblock, bitsinblock, *blockbuffer, filenum, sts;
  OZ_Dbn blocknumber;
  OZ_VDFS_Volex *volex;
  OZ_VDFS_Volume *volume;

  volume = iopex -> devex -> volume;
  volex  = volume -> volex;

  if (filex -> header.areas[OZ_FS_HEADER_AREA_POINTER].size != 0) oz_crash ("oz_dev_dfs delete_header: called with blocks allocated");

  filenum = filex -> header.fileid.num;
  if (filenum == 0) oz_crash ("oz_dev_dfs delete_header: trying to delete file number zero");

  /* Flush header writes in case this file is on dirtyfiles list */

  oz_dev_vdfs_write_dirty_headers (volume, iopex);

  /* Set file number to zero, leave checksum as it was         */
  /* This will give us a bad header error if we try to read it */

  filex -> header.fileid.num = 0;

  /* Write block back to index file */

  sts = oz_dev_vdfs_writevirtblock (volex -> indexheaders, filenum, 0, volex -> homeblock.blocksize, &(filex -> header), iopex, 0);
  if (sts != OZ_SUCCESS) {
    oz_dev_vdfs_printk (iopex, "oz_dev_dfs delete_header: error %u writing deleted header for file %u\n", sts, filenum);
    goto rtn;
  }

  /* Clear index bitmap file bit */

  bitsinblock = volex -> homeblock.blocksize * 8;
  blockbuffer = (uLong *)&(filex -> header);
  blocknumber = (filenum - 1) / bitsinblock + 1;
  sts = oz_dev_vdfs_readvirtblock (volex -> indexbitmap, blocknumber, 0, volex -> homeblock.blocksize, blockbuffer, iopex, 0);
  if (sts != OZ_SUCCESS) {
    oz_dev_vdfs_printk (iopex, "oz_dev_dfs delete_header: error %u reading index bitmap block for file %u\n", sts, filenum);
    goto rtn;
  }

  bitinblock = (filenum - 1) % bitsinblock;
  blockbuffer[bitinblock/32] &= ~(1 << (bitinblock % 32));

  sts = oz_dev_vdfs_writevirtblock (volex -> indexbitmap, blocknumber, 0, volex -> homeblock.blocksize, blockbuffer, iopex, 0);
  if (sts != OZ_SUCCESS) {
    oz_dev_vdfs_printk (iopex, "oz_dev_dfs delete_header: error %u writing index bitmap block for file %u\n", sts, filenum);
  }

rtn:
  OZ_KNL_PGPFREE (filex);
}

/************************************************************************/
/*									*/
/*  Allocate storage blocks						*/
/*									*/
/*    Input:								*/
/*									*/
/*	volume   = volume block pointer					*/
/*	nblocks  = number of wanted blocks				*/
/*	startlbn = requested starting lbn				*/
/*									*/
/*    Output:								*/
/*									*/
/*	allocate_blocks = OZ_SUCCESS : successful allocation		*/
/*	                        else : error status			*/
/*	*nblocks_r  = number of blocks actually allocated		*/
/*	*logblock_r = starting logical block number			*/
/*									*/
/************************************************************************/

static uLong allocate_blocks (OZ_VDFS_Volume *volume, OZ_Dbn nblocks, OZ_Dbn startlbn, OZ_Dbn *nblocks_r, OZ_Dbn *logblock_r, OZ_VDFS_Iopex *iopex)

{
  int bitmapblockdirty, need_bitmap_block;
  OZ_Dbn best_cluster, best_count, bitinbmblock, bitmapvbn, cluster, ncontig_clusters, nstart_cluster, nwanted_clusters, startcluster;
  OZ_VDFS_Volex *volex;
  uLong *bitmapblock, bitsinblock, sts;

  volex = volume -> volex;

  bitsinblock = volex -> homeblock.blocksize * 8;
  bitmapblock = OZ_KNL_PGPMALLOQ (volex -> homeblock.blocksize);
  if (bitmapblock == NULL) return (OZ_EXQUOTAPGP);

  /* Get cluster number to start looking at - 1 */

  startcluster = startlbn / volex -> homeblock.clusterfactor;
  if (startcluster == 0) startcluster = volex -> homeblock.clustertotal;
  startcluster --;

  /* Calculate the number of wanted clusters */

  nwanted_clusters = (nblocks + volex -> homeblock.clusterfactor - 1) / volex -> homeblock.clusterfactor;

  /* Search the bitmap file for the number of wanted clusters, starting with the cluster wanted */

  best_count = 0;								/* haven't found anything yet */
  need_bitmap_block = 1;							/* we need to read the bitmap block */
  ncontig_clusters = 0;								/* no contiguous free clusters found yet */
  for (cluster = startcluster + 1; cluster != startcluster; cluster ++) {	/* start at requested cluster, loop through till we're back at same spot */
    if (cluster == volex -> homeblock.clustertotal) {				/* wrap around the cluster number */
      cluster = 0;
      ncontig_clusters = 0;							/* if we do wrap, start all over counting free contiguous blocks */
    }
    bitinbmblock = cluster % bitsinblock;					/* compute which bit in current bitmap block we want to test */
    if (need_bitmap_block || (bitinbmblock == 0)) {				/* read bitmap block if first time through loop or reached start of new one */
      bitmapvbn = cluster / bitsinblock + 1;
      sts = oz_dev_vdfs_readvirtblock (volex -> storagebitmap, bitmapvbn, 0, volex -> homeblock.blocksize, bitmapblock, iopex, 0);
      if (sts != OZ_SUCCESS) {
        oz_dev_vdfs_printk (iopex, "oz_dev_dfs allocate_blocks: error %u reading storage bitmap block %u\n", sts, bitmapvbn);
        goto cleanup;
      }
      need_bitmap_block = 0;
    }
    if (bitmapblock[bitinbmblock/32] & (1 << (bitinbmblock % 32))) {		/* if bit is set, cluster is allocated, ... */
      ncontig_clusters = 0;							/* ... so start counting contiguous free clusters over again */
    } else {
      if (ncontig_clusters == 0) nstart_cluster = cluster;			/* if first free cluster in a row, save the starting cluster number */
      ncontig_clusters ++;							/* anyway, increment number of free contiguous clusters */
      if (ncontig_clusters == nwanted_clusters) goto found_it;			/* if we have as many as requested, stop looking now */
      if (ncontig_clusters > best_count) {					/* otherwise, if this is the best we have found so far, remember where it is */
        best_count   = ncontig_clusters;
        best_cluster = nstart_cluster;
      }
    }
  }

  /* Couldn't find as much as requested, use what we got (if anything) */

  if (best_count == 0) return (OZ_DISKISFULL);

  ncontig_clusters = best_count;
  nstart_cluster   = best_cluster;

  /* Use ncontig_clusters starting at nstart_cluster */

found_it:

  /* Return the block count and starting block number */

  *nblocks_r  = ncontig_clusters * volex -> homeblock.clusterfactor;
  *logblock_r = nstart_cluster * volex -> homeblock.clusterfactor;

  /* Set the bits in the bit map */

  bitmapblockdirty = 0;
  for (cluster = nstart_cluster; cluster < nstart_cluster + ncontig_clusters; cluster ++) {
    bitinbmblock = cluster % bitsinblock;
    if (cluster / bitsinblock + 1 != bitmapvbn) {
      if (bitmapblockdirty) {
        sts = oz_dev_vdfs_writevirtblock (volex -> storagebitmap, bitmapvbn, 0, volex -> homeblock.blocksize, bitmapblock, iopex, 0);
        if (sts != OZ_SUCCESS) {
          oz_dev_vdfs_printk (iopex, "oz_dev_dfs allocate_blocks: error %u writing storage bitmap block %u\n", sts, cluster / bitsinblock);
          goto cleanup;
        }
      }
      bitmapblockdirty = 0;
      bitmapvbn = cluster / bitsinblock + 1;
      sts = oz_dev_vdfs_readvirtblock (volex -> storagebitmap, bitmapvbn, 0, volex -> homeblock.blocksize, bitmapblock, iopex, 0);
      if (sts != OZ_SUCCESS) {
        oz_dev_vdfs_printk (iopex, "oz_dev_dfs allocate_blocks: error %u reading storage bitmap block %u\n", sts, cluster / bitsinblock + 1);
        goto cleanup;
      }
    }
    if ((1 << (bitinbmblock % 32)) & bitmapblock[bitinbmblock/32]) {
      oz_dev_vdfs_printk (iopex, "oz_dev_dfs allocate_blocks: bitmap bit already set\n");
      oz_dev_vdfs_printk (iopex, "oz_dev_dfs allocate_blocks:   nblocks %u, startlbn %u\n", nblocks, startlbn);
      oz_dev_vdfs_printk (iopex, "oz_dev_dfs allocate_blocks:   *nblocks_r %u, *logblock_r %u\n", *nblocks_r, *logblock_r);
      oz_dev_vdfs_printk (iopex, "oz_dev_dfs allocate_blocks:   cluster %u, bitinbmblock %u\n", cluster, bitinbmblock);
      sts = OZ_BUGCHECK;
      goto cleanup;
    }
    bitmapblock[bitinbmblock/32] |= 1 << (bitinbmblock % 32);
    if (volex -> homeblock.clustersfree == 0) {
      oz_dev_vdfs_printk (iopex, "oz_dev_dfs allocate_blocks: clustersfree was zero\n");
      sts = OZ_BUGCHECK;
      goto cleanup;
    }
    volex -> homeblock.clustersfree --;
    volume -> dirty  = 1;
    bitmapblockdirty = 1;
  }

  if (bitmapblockdirty) {
    sts = oz_dev_vdfs_writevirtblock (volex -> storagebitmap, bitmapvbn, 0, volex -> homeblock.blocksize, bitmapblock, iopex, 0);
    if (sts != OZ_SUCCESS) {
      oz_dev_vdfs_printk (iopex, "oz_dev_dfs allocate_blocks: error %u writing storage bit map block %u\n", sts, cluster / bitsinblock + 1);
    }
  }

cleanup:
  OZ_KNL_PGPFREE (bitmapblock);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Free storage blocks for re-use					*/
/*									*/
/*    Input:								*/
/*									*/
/*	volume   = volume block pointer					*/
/*	nblocks  = number of blocks to free				*/
/*	logblock = starting logical block number			*/
/*									*/
/*    Output:								*/
/*									*/
/*	free_blocks = OZ_SUCCESS : successful				*?
/*	                    else : error status				*/
/*									*/
/************************************************************************/

static uLong free_blocks (OZ_VDFS_Volume *volume, OZ_Dbn nblocks, OZ_Dbn logblock, OZ_VDFS_Iopex *iopex)

{
  int need_bitmap_block;
  OZ_Dbn cluster, nclusters, startcluster;
  OZ_VDFS_Volex *volex;
  uLong *bitmapblock, bitsinblock, blocksize, clusterbit, sts;

  volex = volume -> volex;

  /* Get cluster number to start freeing at */

  if (logblock % volex -> homeblock.clusterfactor != 0) {
    oz_dev_vdfs_printk (iopex, "oz_dev_dfs free_blocks: starting block number not multiple of cluster factor");
    return (OZ_FILECORRUPT);
  }
  startcluster = logblock / volex -> homeblock.clusterfactor;

  /* Calculate the number of clusters to free */

  if (nblocks % volex -> homeblock.clusterfactor != 0) {
    oz_dev_vdfs_printk (iopex, "oz_dev_dfs free_blocks: number of blocks not multiple of cluster factor");
    return (OZ_FILECORRUPT);
  }
  nclusters = nblocks / volex -> homeblock.clusterfactor;

  /* Mallocate a temp block buffer for bitmap */

  blocksize   = volex -> homeblock.blocksize;
  bitsinblock = blocksize * 8;
  bitmapblock = OZ_KNL_PGPMALLOQ (blocksize);
  if (bitmapblock == NULL) return (OZ_EXQUOTAPGP);

  /* Clear the bits in the bit map */

  need_bitmap_block = 1;
  for (cluster = startcluster; cluster < startcluster + nclusters; cluster ++) {
    clusterbit = cluster % bitsinblock;
    if (need_bitmap_block || (clusterbit == 0)) {
      if (!need_bitmap_block) {
        sts = oz_dev_vdfs_writevirtblock (volex -> storagebitmap, cluster / bitsinblock, 0, blocksize, bitmapblock, iopex, 0);
        if (sts != OZ_SUCCESS) {
          oz_dev_vdfs_printk (iopex, "oz_dev_dfs free_blocks: error %u writing storage bitmap block %u\n", sts, cluster / bitsinblock);
          goto cleanup;
        }
      }
      sts = oz_dev_vdfs_readvirtblock (volex -> storagebitmap, cluster / bitsinblock + 1, 0, blocksize, bitmapblock, iopex, 0);
      if (sts != OZ_SUCCESS) {
        oz_dev_vdfs_printk (iopex, "oz_dev_dfs free_blocks: error %u reading storage bitmap block %u\n", sts, cluster / bitsinblock + 1);
        goto cleanup;
      }
      need_bitmap_block = 0;
    }
    bitmapblock[clusterbit/32] &= ~ (1 << (clusterbit % 32));
  }
  cluster --;
  sts = oz_dev_vdfs_writevirtblock (volex -> storagebitmap, cluster / bitsinblock + 1, 0, blocksize, bitmapblock, iopex, 0);
  if (sts != OZ_SUCCESS) {
    oz_dev_vdfs_printk (iopex, "oz_dev_dfs free_blocks: error %u writing storage bit map block %u\n", sts, cluster / bitsinblock + 1);
  }
  volex -> homeblock.clustersfree += nclusters;
  volume -> dirty = 1;

cleanup:
  OZ_KNL_PGPFREE (bitmapblock);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Read header block into the given file struct			*/
/*									*/
/*    Input:								*/
/*									*/
/*	fileid = file-id of the header block				*/
/*	hdrlbn = header logical block number				*/
/*	exthdr = 0 : prime header, 1 : extension header			*/
/*	filex  = where to read header block into			*/
/*									*/
/*    Output:								*/
/*									*/
/*	read_header_block = OZ_SUCCESS : successfully read		*/
/*	                          else : read error status		*/
/*	fixed portion of file struct cleared				*/
/*	header read into filex -> header				*/
/*									*/
/************************************************************************/

static uLong read_header_block (const OZ_VDFS_Fileid *fileid, OZ_Dbn hdrlbn, int exthdr, OZ_VDFS_Fileid *lastfid, OZ_VDFS_Filex *filex, OZ_VDFS_Iopex *iopex)

{
  uLong i, sts;
  OZ_Dbn nblocks;
  OZ_VDFS_Volex *volex;
  OZ_VDFS_Volume *volume;
  Pointer *pointer;
  uWord cksm;

  volume = iopex -> devex -> volume;
  volex  = volume -> volex;

  /* Clear out fixed portion of header */

  memset (filex, 0, ((uByte *)&(filex -> header)) - (uByte *)filex);

  /* Read the header block from the indexheaders file on disk into the filex struct */

  sts = oz_dev_vdfs_readlogblock (hdrlbn, 0, volex -> homeblock.blocksize, &(filex -> header), iopex);
  if (sts != OZ_SUCCESS) return (sts);

  /* Validate the header block's checksum */

  cksm = 0;
  for (i = volex -> homeblock.blocksize / sizeof (uWord); i > 0;) {
    cksm += ((uWord *)&(filex -> header))[--i];
  }
  if (cksm != 0) {
    oz_dev_vdfs_printk (iopex, "oz_dev_dfs read_header_block: bad header checksum (%u,%u,%u)  hdrlbn %u\n", fileid -> num, fileid -> rvn, fileid -> seq, hdrlbn);
    oz_knl_dumpmem2 (volex -> homeblock.blocksize, &(filex -> header), 0);
    return (OZ_BADHDRCKSM);
  }

  /* Make sure its file-id is ok */

  if (memcmp (&(filex -> header.fileid), fileid, sizeof *fileid) != 0) {
    oz_dev_vdfs_printk (iopex, "oz_dev_dfs read_header_block: expected fid (%u,%u,%u) got (%u,%u,%u)  hdrlbn %u\n", 
                        fileid -> num, fileid -> rvn, fileid -> seq, 
                        filex -> header.fileid.num, filex -> header.fileid.rvn, filex -> header.fileid.seq, 
                        hdrlbn);
    return (OZ_FILEDELETED);
  }

  /* Make sure the header looks nice */

  if (!validate_header (&(filex -> header), volume, iopex)) return (OZ_FILECORRUPT);

  /* Make sure its header extension sequence is ok */

  if ((filex -> header.dircount == EXT_DIRCOUNT) ^ exthdr) {
    oz_dev_vdfs_printk (iopex, "oz_dev_dfs read_header_block: ext header (%u,%u,%u) dircount is %u\n", fileid -> num, fileid -> rvn, fileid -> seq, filex -> header.dircount);
    return (OZ_FILECORRUPT);
  }
  if ((lastfid != NULL) && (memcmp (&(filex -> header.dirid), lastfid, sizeof *lastfid) != 0)) {
    oz_dev_vdfs_printk (iopex, "oz_dev_dfs read_header_block: ext header (%u,%u,%u) says it follows (%u,%u,%u) instead of (%u,%u,%u)\n", 
	fileid -> num, fileid -> rvn, fileid -> seq, 
	filex -> header.dirid.num, filex -> header.dirid.rvn, filex -> header.dirid.seq, 
	lastfid -> num, lastfid -> rvn, lastfid -> seq);
    return (OZ_FILECORRUPT);
  }

  /* Set up other filex struct stuff */

  pointer = POINTERS (filex);
  for (i = filex -> header.areas[OZ_FS_HEADER_AREA_POINTER].size / sizeof (Pointer); i > 0; -- i) {
    filex -> blocksinhdr += (pointer ++) -> blockcount;
  }

  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Mark file header is dirty so it will be written to disk		*/
/*									*/
/*    Input:								*/
/*									*/
/*	file = header block to write					*/
/*									*/
/*    Note:								*/
/*									*/
/*	This routine may be called from outside the kernel thread as 	*/
/*	it provides the required synchronization			*/
/*									*/
/************************************************************************/

/* This internal version marks the specific prime or extension header dirty */

static void mark_exthdr_dirty (OZ_VDFS_Filex *dirtyfilex, OZ_VDFS_File *dirtyfile)

{
  oz_dev_vdfs_mark_header_dirty (dirtyfile);				// put on queue of files that need header written out
  if (dirtyfilex != dirtyfile -> filex) {				// see if we're marking prime header dirty
    OZ_HW_ATOMIC_DECBY1_LONG (dirtyfile -> filex -> headerdirty);	// if not, decrement prime header's dirty count
    OZ_HW_ATOMIC_INCBY1_LONG (dirtyfilex -> headerdirty);		// ... and increment extension header's dirty count
  }
}

/* This routine is called by oz_dev_vdfs to mark the prime header dirty (it changed a date or eof position, etc) */

static void dfs_mark_header_dirty (OZ_VDFS_File *dirtyfile)

{
  OZ_HW_ATOMIC_INCBY1_LONG (dirtyfile -> filex -> headerdirty);
}

/* This routine is called by oz_dev_vdfs to write out all headers that are marked dirty    */
/* The individual filex->headerdirty flags will be set for those headers that need writing */

static uLong dfs_write_dirty_header (OZ_VDFS_File *dirtyfile, Long alf, OZ_Datebin now, OZ_VDFS_Volume *volume, OZ_VDFS_Iopex *iopex)

{
  uByte *cleanheader;
  Filattr *filattr;
  OZ_VDFS_Filex *dirtyfilex;
  uLong sts, vl;

  dirtyfilex = dirtyfile -> filex;
  filattr = FILATTRS (dirtyfilex -> header);

  /* Maybe the eof pointer in the header needs updating */

  if (alf & OZ_VDFS_ALF_M_EOF) {					// see if recio needs us to update the eof poistion
    vl = oz_hw_smplock_wait (&(dirtyfile -> attrlock_vl));		// ok, lock it so we get consistent efblk/efbyt values
    filattr -> eofblock = dirtyfile -> attrlock_efblk;			// store them in file's header block
    filattr -> eofbyte  = dirtyfile -> attrlock_efbyt;
    oz_hw_smplock_clr (&(dirtyfile -> attrlock_vl), vl);
    dirtyfilex -> headerdirty = 1;
  }

  /* Maybe some dates in the header need updating */

  if (alf & (OZ_VDFS_ALF_M_MDT | OZ_VDFS_ALF_M_CDT | OZ_VDFS_ALF_M_ADT)) { // see if any of the dates are to be modified
    if (alf & OZ_VDFS_ALF_M_MDT) filattr -> modify_date = now;		// if so, modify the requested values
    if (alf & OZ_VDFS_ALF_M_CDT) filattr -> change_date = now;
    if (alf & OZ_VDFS_ALF_M_ADT) filattr -> access_date = now;
    dirtyfilex -> headerdirty = 1;
  }

  /* Loop through the headers (prime and extension), writing those what are dirty */

#if 000
  countblocksinfile (dirtyfile, NULL);
  cleanheader = OZ_KNL_PGPMALLOC (volume -> volex -> homeblock.blocksize);
#endif
  do {
    if (dirtyfilex -> headerdirty) {					// see if the header is dirty
      sts = write_header_block (volume, dirtyfilex, iopex);		// write the header out to disk
      if (sts != OZ_SUCCESS) {
        oz_dev_vdfs_printk (iopex, "oz_dev_dfs write_dirty_header: error %u writing header (%u,%u,%u)\n", 
                            sts, dirtyfilex -> header.fileid.num, dirtyfilex -> header.fileid.rvn, dirtyfilex -> header.fileid.seq);
      }
    } else {
#if 000
      sts = oz_dev_vdfs_readvirtblock (volume -> volex -> indexheaders, dirtyfilex -> header.fileid.num, 0, volume -> volex -> homeblock.blocksize, cleanheader, iopex, 0);
      if (sts != OZ_SUCCESS) oz_dev_vdfs_printk (iopex, "oz_dev_dfs write_dirty_header: error %u reading header (%u,%u,%u)\n", 
			sts, dirtyfilex -> header.fileid.num, dirtyfilex -> header.fileid.rvn, dirtyfilex -> header.fileid.seq);
      else if (memcmp (cleanheader, &(dirtyfilex -> header), volume -> volex -> homeblock.blocksize) != 0) {
        oz_crash ("oz_dev_dfs write_dirty_header: header (%u,%u,%u) not marked dirty", dirtyfilex -> header.fileid.num, dirtyfilex -> header.fileid.rvn, dirtyfilex -> header.fileid.seq);
      }
#endif
    }
  } while ((dirtyfilex = dirtyfilex -> next) != NULL);			// check next extension in file
#if 000
  OZ_KNL_PGPFREE (cleanheader);
#endif

  return (sts);
}

static uLong write_header_block (OZ_VDFS_Volume *volume, OZ_VDFS_Filex *filex, OZ_VDFS_Iopex *iopex)

{
  OZ_VDFS_Volex *volex;
  uLong i, sts;
  uWord cksm;

  volex = volume -> volex;

  /* Calculate the header block's new checksum */

  cksm = 0;
  filex -> header.checksum = 0;
  for (i = volex -> homeblock.blocksize / sizeof (uWord); i > 0;) {
    cksm -= ((uWord *)&(filex -> header))[--i];
  }
  filex -> header.checksum = cksm;

  /* We should only be writing nice headers */

  if (!validate_header (&(filex -> header), volume, iopex)) oz_crash ("oz_dev_dfs write_header_block: writing corrupt header");

  /* It is no longer dirty */

  filex -> headerdirty = 0;

  /* Write to disk */

  sts = oz_dev_vdfs_writevirtblock (volex -> indexheaders, filex -> header.fileid.num, 0, volex -> homeblock.blocksize, &(filex -> header), iopex, 0);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Write homeblock to volume						*/
/*									*/
/*    Input:								*/
/*									*/
/*	volume = volume who's homeblock to write			*/
/*	iopex = I/O operation in progress				*/
/*									*/
/*    Output:								*/
/*									*/
/*	writehomeblock = write status					*/
/*									*/
/************************************************************************/

static uLong dfs_writehomeblock (OZ_VDFS_Volume *volume, OZ_VDFS_Iopex *iopex)

{
  int i;
  OZ_VDFS_Volex *volex;
  uLong sts;
  uWord cksm;

  volex = volume -> volex;

  volex -> homeblock.checksum = 0;
  cksm = 0;
  for (i = 0; i < (sizeof volex -> homeblock) / sizeof (uWord); i ++) {
    cksm -= ((uWord *)&(volex -> homeblock))[i];
  }
  volex -> homeblock.checksum = cksm;

  sts = oz_dev_vdfs_writelogblock (volume -> hb_logblock, 0, iopex -> devex -> blocksize, &(volex -> homeblock), 0, iopex);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Verify volume structures						*/
/*									*/
/************************************************************************/

static uLong dfs_verify_volume (OZ_VDFS_Iopex *iopex, OZ_VDFS_Devex *devex)

{
  uLong sts;

  // ?? flush any dirty headers out to disk

  iopex -> u.verifyvol.p.readonly = 1; //?? force readonly because we don't fix internal file structs

  sts = verify_volume (iopex -> u.verifyvol.p.readonly, devex -> blocksize, devex -> totalblocks, iopex);

  // ?? re-read headers for all open files from disk

  return (sts);
}

/************************************************************************/
/*									*/
/*  Validate directory block contents					*/
/*									*/
/*    Input:								*/
/*									*/
/*	totalsize = total size (in bytes) given				*/
/*	blocksize = size of one directory block				*/
/*	dirbuffer = directory buffer to validate			*/
/*									*/
/*    Output:								*/
/*									*/
/*	validirbuf = 0 : contents invalid				*/
/*	             1 : contents valid					*/
/*									*/
/************************************************************************/

static int validirbuf (uLong totalsize, uLong blocksize, uByte *dirbuffer, OZ_VDFS_Iopex *iopex)

{
  char c, lastname[FILENAME_MAX], thisname[FILENAME_MAX];
  uLong i, j, u, v;

  /* The total size must be an exact multiple of the block size */

  if (totalsize % blocksize != 0) {
    oz_dev_vdfs_printk (iopex, "oz_dev_dfs validirbuf: totalsize %u, blocksize %u\n", totalsize, blocksize);
    return (0);
  }

  lastname[0] = 0;
  u = -1;

  while (totalsize > 0) {

    /* First byte must be a 1, indicating that no chars match with previous */
    /* entry in the block, as there is no previous entry in the block       */

    if (dirbuffer[0] != 1) {
      oz_dev_vdfs_printk (iopex, "oz_dev_dfs validirbuf: block doesn't begin with a 1\n");
      return (0);
    }

    /* Process a block in the buffer */

    for (i = 0; i < blocksize;) {

      /* Get number of characters that match with previous entry in the block */

      j = dirbuffer[i++];
      if (j == 0) break;
      if (-- j > strlen (lastname)) {
        oz_dev_vdfs_printk (iopex, "oz_dev_dfs validirbuf: same count .gt. last name's length\n");
        return (0);
      }

      /* Make this complete filename */

      memcpy (thisname, lastname, j);
      while (i < blocksize) {
        c = dirbuffer[i++];
        if (c == 0) goto endofname;
        if ((c < ' ') || (c > 126)) {
          oz_dev_vdfs_printk (iopex, "oz_dev_dfs validirbuf: non-printable character in filename\n");
          return (0);
        }
        if (j >= sizeof thisname - 1) {
          oz_dev_vdfs_printk (iopex, "oz_dev_dfs validirbuf: filename string too long\n");
          return (0);
        }
        thisname[j++] = c;
      }
      oz_dev_vdfs_printk (iopex, "oz_dev_dfs validirbuf: no null terminator on filename\n");
      return (0);
endofname:
      thisname[j] = 0;

      /* Names should not be descending */

      if (strcmp (thisname, lastname) < 0) {
        oz_dev_vdfs_printk (iopex, "oz_dev_dfs validirbuf: descending names ('%s' followed by '%s')\n", lastname, thisname);
        return (0);
      }

      /* The version numbers should be in decreasing order */

      if (strcmp (thisname, lastname) > 0) u = -1;
      while (i <= blocksize - sizeof (Dirpnt)) {
        v = ((Dirpnt *)(dirbuffer + i)) -> version;
        i += sizeof (Dirpnt);
        if (v == 0) goto endofver;
        if (v >= u) {
          oz_dev_vdfs_printk (iopex, "oz_dev_dfs validirbuf: non-descending version numbers (%s;%u followed by ;%u)\n", thisname, u, v);
          return (0);
        }
        u = v;
      }
      oz_dev_vdfs_printk (iopex, "oz_dev_dfs validirbuf: no end of version numbers (last was %s;%u)\n", thisname, u);
      return (0);
endofver:

      /* Process next name in block */

      strcpy (lastname, thisname);
    }

    /* Skip to next block in the buffer */

    totalsize -= blocksize;
    dirbuffer += blocksize;
  }
  return (1);
}

/************************************************************************/
/*									*/
/*  Map a virtual block number to a logical block number		*/
/*									*/
/*    Input:								*/
/*									*/
/*	file = pointer to file node of the file				*/
/*	virtblock = virtual block number				*/
/*	            vbns start at 1 for first block in file		*/
/*									*/
/*    Output:								*/
/*									*/
/*	map_vbn_to_lbn = OZ_SUCCESS : successful			*/
/*	                 OZ_VBNZERO : vbn zero requested		*/
/*	               OZ_ENDOFFILE : virtblock is past end of file	*/
/*	*nblocks_r  = number of blocks mapped by pointer		*/
/*	*logblock_r = first logical block number			*/
/*									*/
/************************************************************************/

static uLong dfs_map_vbn_to_lbn (OZ_VDFS_File *file, OZ_Dbn virtblock, OZ_Dbn *nblocks_r, OZ_Dbn *logblock_r)

{
  OZ_VDFS_Filex *extfilex;
  uLong i, n;
  OZ_Dbn relblock;
  Pointer *pointer;

  if (virtblock == 0) return (OZ_VBNZERO);
  relblock = virtblock - 1;

  /* Find extension header that maps the virtual block */

  for (extfilex = file -> filex; extfilex != NULL; extfilex = extfilex -> next) {
    if (extfilex -> blocksinhdr > relblock) goto found_extension;
    relblock -= extfilex -> blocksinhdr;
  }
  return (OZ_ENDOFFILE);
found_extension:

  /* Find the pointer that maps the virtual block */

  pointer = POINTERS (extfilex);
  n = extfilex -> header.areas[OZ_FS_HEADER_AREA_POINTER].size / sizeof *pointer;
  for (i = 0; i < n; i ++) {
    if (pointer -> blockcount > relblock) goto found_pointer;
    relblock -= pointer -> blockcount;
    pointer ++;
  }
  oz_crash ("oz_dev_dfs map_vbn_to_lbn: ran off end of pointers");
found_pointer:

  /* Found pointer, relblock is number of blocks to skip in the pointer */

  *nblocks_r  = pointer -> blockcount - relblock;	/* return number of blocks in the pointer starting with requested block */
  *logblock_r = pointer -> logblock + relblock;		/* return logical block number corresponding with the requested vbn */

  return (OZ_SUCCESS);
}  

/************************************************************************/
/*									*/
/*  Validation routines							*/
/*									*/
/************************************************************************/

static void validate_volume (OZ_VDFS_Volume *volume, int line)

{
#ifdef OZ_DEBUG
  int found_ibm, found_ih, found_sbm, i;
  OZ_VDFS_File *extfile, *file, **lfile;
  OZ_VDFS_Fileid *lastfid;
  OZ_VDFS_Volex *volex;
  uLong extseq, ndirtyfiles, ndirtyfiles2, nopenfiles, vl;
  uWord cksm;

  volex = volume -> volex;

  if ((volume -> bb_nblocks != 1) || (volume -> bb_logblock != 0) || (volume -> hb_logblock != 1)) {
    oz_crash ("oz_dev_dfs validate_volume: bad boot/home block values");
  }
  if (!(volume -> dirty)) {
    if (volex -> homeblock.homeversion != HOMEBLOCK_VERSION) {
      oz_crash ("oz_dev_dfs validate_volume: homeversion %u", volex -> homeblock.homeversion);
    }
    cksm = 0;
    for (i = 0; i < (sizeof volex -> homeblock) / sizeof (uWord); i ++) {
      cksm += ((uWord *)&(volex -> homeblock))[i];
    }
    if (cksm != 0) {
      oz_crash ("oz_dev_dfs validate_volume: bad homeblock checksum");
    }
  }

  found_ibm    = 0;
  found_ih     = 0;
  found_sbm    = 0;
  nopenfiles   = 0;
  ndirtyfiles2 = 0;
  vl = oz_hw_smplock_wait (&(volume -> dirtyfiles_vl));
  for (lfile = &(volume -> openfiles); (file = *lfile) != NULL; lfile = &(file -> next)) {
    if (file -> prev != lfile) {
      oz_crash ("oz_dev_dfs validate_volume: %u file prev/next list corrupt", line);
    }
    if (file == volex -> indexbitmap)   found_ibm = 1;
    if (file == volex -> indexheaders)  found_ih  = 1;
    if (file == volex -> storagebitmap) found_sbm = 1;
    validate_file (file, volume, __LINE__, line);
    if (filex -> header.extseq != 1) oz_crash ("oz_dev_dfs validate_volume: %u prime header extseq %u", line, filex -> header.extseq);
    nopenfiles ++;
    if (filex -> headerdirty) ndirtyfiles2 ++;
    extseq = 1;
    lastfid = &(filex -> header.fileid);
    for (extfile = file -> extfile; extfile != NULL; extfile = extfile -> extfile) {
      validate_file (extfile, volume, __LINE__, line);
      if (extfilex -> header.extseq != ++ extseq)  oz_crash ("oz_dev_dfs validate_volume: %u ext header out of sequence", line);
      if (memcmp (&(extfilex -> header.dirid), lastfid, sizeof *lastfid) != 0) {
        oz_crash ("oz_dev_dfs validate_volume: %u ext header (%u,%u,%u) dirid (%u,%u,%u) doesnt point to prev ext header (%u,%u,%u), prime (%u,%u,%u)", 
		line, extfilex -> header.fileid.num, extfilex -> header.fileid.rvn, extfilex -> header.fileid.seq, 
		      extfilex -> header.dirid.num,  extfilex -> header.dirid.rvn,  extfilex -> header.dirid.seq, 
		      lastfid -> num,               lastfid -> rvn,               lastfid -> seq, 
		      filex -> header.fileid.num,    filex -> header.fileid.rvn,    filex -> header.fileid.seq);
      }
      if (extfilex -> headerdirty) ndirtyfiles2 ++;
      lastfid = &(extfilex -> header.fileid);
    }
  }
  if (nopenfiles != volume -> nopenfiles) {
    oz_crash ("oz_dev_dfs validate_volume: %u nopenfiles %u, but there were %u", line, volume -> nopenfiles, nopenfiles);
  }
  if (((volex -> indexbitmap   != NULL) && !found_ibm) 
   || ((volex -> indexheaders  != NULL) && !found_ih) 
   || ((volex -> storagebitmap != NULL) && !found_sbm)) {
    oz_crash ("oz_dev_dfs validate_volume: %u found_ibm %d, _ih %d, _sbm %d", line, found_ibm, found_ih, found_sbm);
  }
  ndirtyfiles = 0;
  for (file = volume -> dirtyfiles; file != NULL; file = file -> nextdirty) {
    if (!(filex -> headerdirty)) {
      oz_crash ("oz_dev_dfs validate_volume: %u on dirtyfiles list but not tagged dirty", line);
    }
    ndirtyfiles ++;
    if (ndirtyfiles > volume -> nopenfiles) {
      oz_crash ("oz_dev_dfs validate_volume: %u dirtyfiles list corrupt", line);
    }
    validate_file (file, volume, __LINE__, line);
  }
  if (ndirtyfiles != ndirtyfiles2) {
    oz_crash ("oz_dev_dfs validate_volume: %u ndirtyfiles %u, ndirtyfiles2 %u", line, ndirtyfiles, ndirtyfiles2);
  }
  oz_hw_smplock_clr (&(volume -> dirtyfiles_vl), vl);
#endif
}

static void validate_file (OZ_VDFS_File *file, OZ_VDFS_Volume *volume, int linef, int linev)

{
#ifdef OZ_DEBUG
  uLong areasize, blocksinhdr, i, npointers, offs;
  Pointer *pointers;
  uWord cksm;

  if (file -> volume != volume) {
    oz_crash ("oz_dev_dfs validate_file: file volume %p not volume %p", file -> volume, volume);
  }

  if (!(filex -> headerdirty)) {
    cksm = 0;
    for (i = 0; i < volex -> homeblock.blocksize / sizeof (uWord); i ++) {
      cksm += ((uWord *)&(filex -> header))[i];
    }
    if (cksm != 0) {
      oz_crash ("oz_dev_dfs validate_file: bad header checksum");
    }
  }

  offs = 0;
  areasize = (((uByte *)&(filex -> header)) + volex -> homeblock.blocksize - filex -> header.area) & -4;
  for (i = 0; i < OZ_FS_HEADER_NAREAS; i ++) {
    if (filex -> header.areas[i].offs < offs) {
      oz_crash ("oz_dev_dfs validate_file: area[%u] offset %u follows %u", i, filex -> header.areas[i].offs, offs);
    }
    offs = ((filex -> header.areas[i].size + 3) & -4) + filex -> header.areas[i].offs;
    if (offs > areasize) {
      oz_crash ("oz_dev_dfs validate_file: area[%u] ends at %u", i, offs);
    }
  }

  blocksinhdr = 0;
  npointers   = filex -> header.areas[OZ_FS_HEADER_AREA_POINTER].size / sizeof *pointers;
  pointers    = POINTERS (file);
  for (i = 0; i < npointers; i ++) {
    blocksinhdr += pointers[i].blockcount;
  }
  if (blocksinhdr != file -> filex -> blocksinhdr) {
    oz_crash ("oz_dev_dfs validate_file: blocksinhdr is %u, but there are %u", file -> filex -> blocksinhdr, blocksinhdr);
  }
#endif
}

/************************************************************************/
/*									*/
/*  Validate an file header						*/
/*									*/
/*    Note:  This routine is called by the file open routine to check 	*/
/*	the on-disk header						*/
/*									*/
/************************************************************************/

static int validate_header (Header *header, OZ_VDFS_Volume *volume, OZ_VDFS_Iopex *iopex)

{
  char *p;
  int i;
  Filattr filattr;
  OZ_VDFS_Volex *volex;
  Pointer *pointer;
  uLong lastoffs;

  volex = volume -> volex;

  /* Make sure areas are in order and don't overlap */

  lastoffs = 0;
  for (i = 0; i < OZ_FS_HEADER_NAREAS; i ++) {
    if (header -> areas[i].offs < lastoffs) {
      oz_dev_vdfs_printk (iopex, "oz_dev_dfs validate_header: area[%d].offs %u before last offset %u\n", i, header -> areas[i].offs, lastoffs);
      return (0);
    }
    lastoffs = header -> areas[i].offs + header -> areas[i].size;
  }
  if (header -> area + lastoffs - (uByte *)header > volex -> homeblock.blocksize) {
    oz_dev_vdfs_printk (iopex, "oz_dev_dfs validate_header: last offset %u beyond end of header\n", lastoffs);
    return (0);
  }

  /* Make sure each area contents are ok */

  if (header -> areas[OZ_FS_HEADER_AREA_FILNAME].size != 0) {
    p = (char *)(header -> area + header -> areas[OZ_FS_HEADER_AREA_FILNAME].offs);
    if (strlen (p) >= header -> areas[OZ_FS_HEADER_AREA_FILNAME].size) {
      oz_dev_vdfs_printk (iopex, "oz_dev_dfs validate_header: filename missing terminating null\n");
      return (0);
    }
  }

#if 00
  if (header -> areas[OZ_FS_HEADER_AREA_FILATTR].size != 0) {
    movc4 (header -> areas[OZ_FS_HEADER_AREA_FILATTR].size, header -> area + header -> areas[OZ_FS_HEADER_AREA_FILATTR].offs, sizeof filattr, &filattr);
    ??
  }
#endif

#if 00
  if (header -> areas[OZ_FS_HEADER_AREA_SECATTR].size != 0) ??
#endif

  if (header -> areas[OZ_FS_HEADER_AREA_POINTER].size != 0) {
    if (header -> areas[OZ_FS_HEADER_AREA_POINTER].size % sizeof *pointer != 0) {
      oz_dev_vdfs_printk (iopex, "oz_dev_dfs validate_header: pointer area size %u not multiple of pointer size\n", header -> areas[OZ_FS_HEADER_AREA_POINTER].size);
      return (0);
    }
    pointer = (Pointer *)(header -> area + header -> areas[OZ_FS_HEADER_AREA_POINTER].offs);
    for (i = 0; i < header -> areas[OZ_FS_HEADER_AREA_POINTER].size / sizeof *pointer; i ++) {
      if (pointer -> blockcount == 0) {
        oz_dev_vdfs_printk (iopex, "oz_dev_dfs validate_header: pointer has blockcount zero\n");
        return (0);
      }
      if (pointer -> blockcount % volex -> homeblock.clusterfactor != 0) {
        oz_dev_vdfs_printk (iopex, "oz_dev_dfs validate_header: pointer has blockcount %u not multiple of clusterfactor\n", pointer -> blockcount);
        return (0);
      }
      if (pointer -> blockcount / volex -> homeblock.clusterfactor >= volex -> homeblock.clustertotal) {
        oz_dev_vdfs_printk (iopex, "oz_dev_dfs validate_header: pointer has blockcount %u larger than whole disk\n", pointer -> blockcount);
        return (0);
      }
      if (pointer -> logblock % volex -> homeblock.clusterfactor != 0) {
        oz_dev_vdfs_printk (iopex, "oz_dev_dfs validate_header: pointer has logblock %u not multiple of clusterfactor\n", pointer -> logblock);
        return (0);
      }
      if (pointer -> logblock / volex -> homeblock.clusterfactor >= volex -> homeblock.clustertotal) {
        oz_dev_vdfs_printk (iopex, "oz_dev_dfs validate_header: pointer has logblock %u off end of disk\n", pointer -> logblock);
        return (0);
      }
      if (pointer -> blockcount / volex -> homeblock.clusterfactor + pointer -> logblock / volex -> homeblock.clusterfactor > volex -> homeblock.clustertotal) {
        oz_dev_vdfs_printk (iopex, "oz_dev_dfs validate_header: pointer has logblock %u+%u off end of disk\n", pointer -> logblock, pointer -> blockcount);
        return (0);
      }
    }
  }
  return (1);
}

/*************************************************************************/
/*************************************************************************/
/**									**/
/**	VOLUME VERIFICATION ROUTINE					**/
/**									**/
/*************************************************************************/
/*************************************************************************/

typedef struct { OZ_VDFS_Iopex *iopex;
                 int rdonly;
                 uLong blocksize;
                 OZ_Dbn totalblocks;
                 const char *devname;
                 void *allmem;
               } Vpb;

typedef struct Vfymem Vfymem;
struct Vfymem { Vfymem *next;
                Vfymem **prev;
              };

/* Stuff that is saved from each in-use file header for cross-checking */

typedef struct { OZ_VDFS_Fileid fileid;
                 OZ_VDFS_Fileid extid;
                 OZ_VDFS_Fileid dirid;
                 uLong dircount;
                 uLong dirfound;
                 uLong filattrflags;
               } Headersave;

/* In-memory File struct */

typedef struct Vfyfile Vfyfile;
struct Vfyfile { Vfyfile *extfile;
                 Header header;
               };

/* Macros for vpb stuff */

#define BLOCKSIZE (vpb -> blocksize)
#define MALLOC(size) vfy_malloc (vpb, size)
#define FREE(buff) vfy_free (vpb, buff)
#define PRINTF oz_dev_vdfs_printk (vpb -> iopex, 
#define RLB(dbn,size,buff) oz_dev_vdfs_readlogblock (dbn, 0, size, buff, vpb -> iopex)
#define WLB(dbn,size,buff) ((vpb -> rdonly) ? OZ_WRITELOCKED : oz_dev_vdfs_writelogblock (dbn, 0, size, buff, 1, vpb -> iopex))

#define EOFBLOCK(file) ((Filattr *)(file -> header.area + file -> header.areas[OZ_FS_HEADER_AREA_FILATTR].offs)) -> eofblock

/* Bit macros */

#define BIT_CLEAR(ubytearray,bitnumber) ubytearray[(bitnumber)/8] &= ~ (1 << ((bitnumber) & 7))
#define BIT_ISCLEAR(ubytearray,bitnumber) !BIT_ISSET(ubytearray,bitnumber)
#define BIT_ISSET(ubytearray,bitnumber) ((ubytearray[(bitnumber)/8] >> ((bitnumber) & 7)) & 1)
#define BIT_SET(ubytearray,bitnumber) ubytearray[(bitnumber)/8] |= 1 << ((bitnumber) & 7)

/* Internal routines */

static uLong verify_thread (void *vpbv);
static void vfy_printname (Vpb *vpb, Vfyfile *indexheaders, OZ_Dbn filenumber);
static uLong vfy_openfile (Vpb *vpb, OZ_Dbn filenumber, Vfyfile **file_r, Homeblock *homeblock, Vfyfile *indexheaders);
static void vfy_closefile (Vpb *vpb, Vfyfile *file);
static uLong vfy_writeheader (Vpb *vpb, Vfyfile *indexheaders, Header *header);
static uLong vfy_rvb (Vpb *vpb, Vfyfile *file, OZ_Dbn vbn, uLong size, void *buff);
static uLong vfy_wvb (Vpb *vpb, Vfyfile *file, OZ_Dbn vbn, uLong size, void *buff);
static uLong vfy_vbn2lbn (Vfyfile *file, OZ_Dbn virtblock, OZ_Dbn *nblocks_r, OZ_Dbn *logblock_r);
static void *vfy_malloc (Vpb *vpb, uLong size);
static void vfy_free (Vpb *vpb, void *buff);

static uLong verify_volume (int readonly, uLong blocksize, OZ_Dbn totalblocks, OZ_VDFS_Iopex *iopex)

{
  OZ_Event *event;
  OZ_Process *process;
  OZ_Thread *thread;
  uLong exitsts, sts;
  Vpb vpb;

  /* Set up parameter block for verify routine */

  vpb.iopex       = iopex;
  vpb.blocksize   = blocksize;
  vpb.totalblocks = totalblocks;
  vpb.devname     = oz_knl_devunit_devname (iopex -> devex -> devunit);
  vpb.rdonly      = readonly;

  /* Run it in its own process so it has a full address space to use and that we can throw out when it is done */

  sts = oz_knl_process_create (oz_s_systemjob, 0, 0, strlen (vpb.devname), vpb.devname, NULL, &process);
  if (sts != OZ_SUCCESS) {
    oz_dev_vdfs_printk (iopex, "error %u creating process\n", sts);
    goto rtnsts;
  }

  sts = oz_knl_event_create (strlen (vpb.devname), vpb.devname, NULL, &event);
  if (sts != OZ_SUCCESS) {
    oz_dev_vdfs_printk (iopex, "error %u creating event\n", sts);
    goto rtnsts_pr;
  }

  sts = oz_knl_thread_create (process, oz_knl_thread_getbasepri (NULL), NULL, NULL, event, 0, verify_thread, &vpb, 
                              OZ_ASTMODE_INHIBIT, strlen (vpb.devname), vpb.devname, NULL, &thread);
  if (sts != OZ_SUCCESS) {
    oz_dev_vdfs_printk (iopex, "error %u creating thread\n", sts);
    goto rtnsts_ev;
  }

  /* Wait for verification to complete */

  while ((sts = oz_knl_thread_getexitsts (thread, &exitsts)) == OZ_FLAGWASCLR) {
    oz_knl_event_waitone (event);
    oz_knl_event_set (event, 0);
  }
  if (sts == OZ_SUCCESS) sts = exitsts;

  /* Clean up and return final status */

  oz_knl_thread_increfc (thread, -1);
rtnsts_ev:
  oz_knl_event_increfc (event, -1);
rtnsts_pr:
  oz_knl_process_increfc (process, -1);
rtnsts:
  return (sts);
}

/************************************************************************/
/*									*/
/*  This is the main verification thread				*/
/*  It runs in kernel mode in its own process space			*/
/*  It uses per-process kernel memory for its mallocs so it won't try 	*/
/*  to hog kernel pool memory						*/
/*									*/
/************************************************************************/

static uLong verify_thread (void *vpbv)

{
  char filenamebuf[256], lastname[FILENAME_MAX], thisname[FILENAME_MAX];
  Dirpnt *dirpnt;
  Vfyfile *directory, *indexbitmap, *indexheaders, *storagebitmap;
  Header *header;
  Headersave *headersaves;
  Homeblock *homeblock;
  int duplicateclusters, fixeddupblocks, fixedheader, i, indexbitmapfixed, indexbitmapsize, l, s, si, storagebitmapsize;
  OZ_Dbn allocluster, allocount, allocstart;
  OZ_Dbn begcluster, clustercount, clusternumber, dirnumber, divfy_rvbn, endcluster, extnumber, filenumber, freeclusters, numberoffiles;
  Pointer *pointers;
  uByte *dirbucket, *duplicateclusterbits, *clusterbuff, *firstuseclusterbits, *indexbitmapbits;
  uLong lastoffs, lastversion, sts;
  uWord cksm;
  Vpb *vpb;
  Vfymem *mem;

  si = oz_hw_cpu_setsoftint (0);

  vpb = vpbv;
  vpb -> allmem = NULL;

  /* Read and validate homey */

  PRINTF "\n*** Reading and validating home block\n");

  homeblock = MALLOC (BLOCKSIZE);
  sts = RLB (1, BLOCKSIZE, homeblock);
  if (sts != OZ_SUCCESS) {
    PRINTF "error %u reading home block\n", sts);
    goto rtnsts;
  }
  if (homeblock -> homeversion != HOMEBLOCK_VERSION) {
    PRINTF "homeblock version is %u, should be %u\n", homeblock -> homeversion, HOMEBLOCK_VERSION);
    sts = OZ_BADHOMEBLKVER;
    goto rtnsts;
  }
  cksm = 0;
  for (i = 0; i < sizeof *homeblock / sizeof (uWord); i ++) {
    cksm += ((uWord *)homeblock)[i];
  }
  if (cksm != 0) {
    PRINTF "bad homeblock checksum\n");
    sts = OZ_BADHOMEBLKCKSM;
    goto rtnsts;
  }
  if (homeblock -> blocksize != BLOCKSIZE) {
    PRINTF "homeblock blocksize is %u, but disk block size is %u\n", homeblock -> blocksize, BLOCKSIZE);
    sts = OZ_BADBLOCKSIZE;
    goto rtnsts;
  }
  if (homeblock -> clusterfactor == 0) {
    PRINTF "homeblock clusterfactor is zero\n");
    sts = OZ_BADBLOCKSIZE;
    goto rtnsts;
  }
  clustercount  = vpb -> totalblocks;
  clustercount /= (SACRED_FIDNUM_COUNT + 1) / 2;
  if (homeblock -> clusterfactor > clustercount) {
    PRINTF "homeblock clusterfactor %u .gt. max allowable count %u\n", homeblock -> clusterfactor, clustercount);
    sts = OZ_BADBLOCKSIZE;
    goto rtnsts;
  }
  if (homeblock -> clusterfactor * homeblock -> clustertotal > vpb -> totalblocks) {
    PRINTF "homeblock clusterfactor*total %u*%u .gt. disk block count %u\n", homeblock -> clusterfactor, homeblock -> clustertotal, vpb -> totalblocks);
    sts = OZ_BADBLOCKSIZE;
    goto rtnsts;
  }

  /* Scan index header file:                                           */
  /*   Make sure index header bitmap bit is correct                    */
  /*   Check for duplicate allocated blocks, compare to storage bitmap */
  /*   Make sure extid/dirid match up                                  */

  PRINTF "\n*** Opening sacred files\n");

  sts = vfy_openfile (vpb, SACRED_FIDNUM_INDEXHEADERS, &indexheaders, homeblock, NULL);
  if (sts != OZ_SUCCESS) {
    PRINTF "error %u opening index header file\n", sts);
    goto rtnsts;
  }
  sts = vfy_openfile (vpb, SACRED_FIDNUM_INDEXBITMAP, &indexbitmap, NULL, indexheaders);
  if (sts != OZ_SUCCESS) {
    PRINTF "error %u opening index bitmap file\n", sts);
    goto rtnsts;
  }

  PRINTF "\n*** Reading bitmaps\n");

  numberoffiles   = EOFBLOCK (indexheaders) - 1;
  indexbitmapsize = ((numberoffiles + BLOCKSIZE * 8) / BLOCKSIZE / 8) * BLOCKSIZE;
  indexbitmapbits = MALLOC (indexbitmapsize);
  sts = vfy_rvb (vpb, indexbitmap, 1, indexbitmapsize, indexbitmapbits);
  if (sts != OZ_SUCCESS) {
    PRINTF "error %u reading %u bytes from index bitmap file\n", sts, indexbitmapsize);
    goto rtnsts;
  }
  indexbitmapfixed = 0;
  headersaves = MALLOC (numberoffiles * sizeof *headersaves);
  memset (headersaves, 0, numberoffiles * sizeof *headersaves);

  storagebitmapsize    = (homeblock -> clustertotal + 7) / 8;
  storagebitmapsize    = ((storagebitmapsize + BLOCKSIZE - 1) / BLOCKSIZE) * BLOCKSIZE;
  firstuseclusterbits  = MALLOC (storagebitmapsize);
  duplicateclusterbits = MALLOC (storagebitmapsize);

  PRINTF "\n*** Scanning file headers\n");

  header = MALLOC (BLOCKSIZE);
scanheaders:
  memset (firstuseclusterbits,  0, storagebitmapsize);
  memset (duplicateclusterbits, 0, storagebitmapsize);
  duplicateclusters = 0;
  freeclusters = homeblock -> clustertotal;
  for (clusternumber = freeclusters; clusternumber < storagebitmapsize * 8; clusternumber ++) {
    BIT_SET (firstuseclusterbits, clusternumber);
  }

  for (filenumber = 1; filenumber <= numberoffiles; filenumber ++) {

    /* Read header block */

    sts = vfy_rvb (vpb, indexheaders, filenumber, BLOCKSIZE, header);
    if (sts != OZ_SUCCESS) {
      PRINTF "error %u reading file header %u\n", sts, filenumber);
      goto rtnsts;
    }

    /* If wrong version, we don't know what is in there */

    if (header -> headerver != HEADER_VERSION) {
      PRINTF "header %u version %u, should be %u\n", filenumber, header -> headerver, HEADER_VERSION);
      continue;
    }

    /* If file number is zero, it means the file header is free */

    if (header -> fileid.num == 0) {
      if (BIT_ISSET (indexbitmapbits, filenumber - 1)) {
        PRINTF "header %u is deleted, but bitmap says it is active\n", filenumber);
        BIT_CLEAR (indexbitmapbits, filenumber - 1);
        indexbitmapfixed = 1;
      }
      continue;
    }

    /* Otherwise, make sure it is marked in use and validate its checksum */

    if (BIT_ISCLEAR (indexbitmapbits, filenumber - 1)) {
      PRINTF "header %u is active, but bitmap says it is deleted\n", filenumber);
      BIT_SET (indexbitmapbits, filenumber - 1);
      indexbitmapfixed = 1;
    }

    cksm = 0;
    for (i = 0; i < BLOCKSIZE / 2; i ++) {
      cksm += ((uWord *)homeblock)[i];
    }
    if (cksm != 0) {
      PRINTF "bad header %u checksum\n", filenumber);
      continue;
    }

    /* Make sure areas are in ascending and non-overlapping order */

    fixedheader = 0;
    lastoffs = 0;
    for (i = 0; i < OZ_FS_HEADER_NAREAS; i ++) {
      if (header -> areas[i].offs < lastoffs) {
        PRINTF "header %u header area %u overlaps\n", filenumber, i);
        if (header -> areas[i].size + header -> areas[i].offs <= lastoffs) {
          header -> areas[i].size = 0;
        } else {
          header -> areas[i].size -= lastoffs - header -> areas[i].offs;
        }
        header -> areas[i].offs = lastoffs;
        fixedheader = 1;
        continue;
      }
      lastoffs = header -> areas[i].size + header -> areas[i].offs;
      if (header -> area + lastoffs > (uByte *)header + BLOCKSIZE) {
        PRINTF "header %u header area %u runs off end of block\n", filenumber, i);
        header -> areas[i].size = ((uByte *)header + BLOCKSIZE) - (header -> area + header -> areas[i].offs);
        lastoffs = header -> areas[i].size + header -> areas[i].offs;
        fixedheader = 1;
      }
    }

    /* Mark blocks allocated and check for duplicate block mapping */

    pointers = (Pointer *)(header -> area + header -> areas[OZ_FS_HEADER_AREA_POINTER].offs);
    for (i = 0; i < header -> areas[OZ_FS_HEADER_AREA_POINTER].size / sizeof *pointers; i ++) {
      if ((pointers[i].blockcount % homeblock -> clusterfactor != 0) 
       || (pointers[i].logblock   % homeblock -> clusterfactor != 0)) {
        vfy_printname (vpb, indexheaders, filenumber);
        PRINTF "header %u pointer %u@%u not cluster aligned\n", filenumber, pointers[i].blockcount, pointers[i].logblock);
        begcluster = (pointers[i].logblock + homeblock -> clusterfactor - 1) / homeblock -> clusterfactor;
        endcluster = (pointers[i].logblock + pointers[i].blockcount - 1)     / homeblock -> clusterfactor + 1;
        pointers[i].blockcount = (endcluster - begcluster) * homeblock -> clusterfactor;
        pointers[i].logblock   = begcluster * homeblock -> clusterfactor;
        fixedheader = 1;
      }
      clustercount  = pointers[i].blockcount / homeblock -> clusterfactor;
      clusternumber = pointers[i].logblock / homeblock -> clusterfactor;
      if (clustercount + clusternumber > homeblock -> clustertotal) {
        vfy_printname (vpb, indexheaders, filenumber);
        PRINTF "header %u pointer %u@%u runs off end of volume\n", filenumber, pointers[i].blockcount, pointers[i].logblock);
        if (clusternumber >= homeblock -> clustertotal) {
          clustercount = 0;
          pointers[i].blockcount = 0;
          pointers[i].logblock   = 0;
        } else {
          clustercount = homeblock -> clustertotal - clusternumber;
          pointers[i].blockcount = clustercount * homeblock -> clusterfactor;
        }
        break;
      }
      while (clustercount > 0) {
        if (BIT_ISSET (firstuseclusterbits, clusternumber)) {
          BIT_SET (duplicateclusterbits, clusternumber);
          duplicateclusters = 1;
        } else {
          BIT_SET (firstuseclusterbits, clusternumber);
          freeclusters --;
        }
        clustercount  --;
        clusternumber ++;
      }
    }

    /* Save the links to other headers for later analysis */

    headersaves[filenumber-1].fileid   = header -> fileid;
    headersaves[filenumber-1].dirid    = header -> dirid;
    headersaves[filenumber-1].extid    = header -> extid;
    headersaves[filenumber-1].dircount = header -> dircount;
    headersaves[filenumber-1].filattrflags = ((Filattr *)(header -> area + header -> areas[OZ_FS_HEADER_AREA_FILATTR].offs)) -> filattrflags;

    /* If we 'fixed' the header, write it back */

    if (fixedheader && !(vpb -> rdonly)) {
      sts = vfy_writeheader (vpb, indexheaders, header);
      if (sts != OZ_SUCCESS) goto rtnsts;
    }
  }

  /* For any duplicate blocks, find all file headers that map to them and copy the blocks */

  if (duplicateclusters && !(vpb -> rdonly)) {
    PRINTF "\n*** Copying duplicated blocks\n");
    fixeddupblocks = 0;
    clusterbuff = MALLOC (homeblock -> clusterfactor * BLOCKSIZE);
    for (filenumber = 1; filenumber <= numberoffiles; filenumber ++) {

      /* Read existing header into 'header' */

      sts = vfy_rvb (vpb, indexheaders, filenumber, BLOCKSIZE, header);
      if (sts != OZ_SUCCESS) {
        PRINTF "error %u reading file header %u\n", sts, filenumber);
        goto rtnsts;
      }

      /* Scan through all pointers in the header */

      fixedheader = 0;
      pointers = (Pointer *)(header -> area + header -> areas[OZ_FS_HEADER_AREA_POINTER].offs);
      for (i = 0; i < header -> areas[OZ_FS_HEADER_AREA_POINTER].size / sizeof *pointers; i ++) {
        clustercount  = pointers[i].blockcount / homeblock -> clusterfactor;
        clusternumber = pointers[i].logblock / homeblock -> clusterfactor;

        /* See if any clusters used by the pointer are in duplicate mapped blocks */

        while (clustercount > 0) {
          if (BIT_ISSET (duplicateclusterbits, clusternumber)) break;
          clustercount  --;
          clusternumber ++;
        }
        if (clustercount > 0) {

          /* If so, output message for the file */

          if (!fixedheader) {
            vfy_printname (vpb, indexheaders, filenumber);
            PRINTF "header %u has duplicate blocks\n", filenumber);
          }

          /* Reset the count and starting cluster number */

          clustercount  = pointers[i].blockcount / homeblock -> clusterfactor;
          clusternumber = pointers[i].logblock / homeblock -> clusterfactor;

          /* Find some identicallly sized unused space */

          allocount = 0;
          for (allocluster = 0; allocluster < homeblock -> clustertotal; allocluster ++) {
            if (BIT_ISSET (firstuseclusterbits, allocluster)) allocount = 0;
            else {
              if (allocount == 0) allocstart = allocluster;
              if (++ allocount == clustercount) break;
            }
          }
          if (allocount < clustercount) {
            PRINTF "  couldn't allocate %u clusters for copying\n", clustercount);
            continue;
          }

          /* Found something big enough, mark the space in use */

          for (allocluster = allocstart; allocluster < allocstart + allocount; allocluster ++) {
            BIT_SET (firstuseclusterbits, allocluster);
          }
          fixeddupblocks = 1;	// make sure we re-scan now that we've changed storage bitmap bits

          /* Copy the old clusters to the new ones */

          PRINTF "  copying clusters %u@%u to %u\n", clustercount, clusternumber, allocstart);
          for (allocount = 0; allocount < clustercount; allocount ++) {
            sts = RLB ((clusternumber + allocount) * homeblock -> clusterfactor, homeblock -> clusterfactor * BLOCKSIZE, clusterbuff);
            if (sts != OZ_SUCCESS) {
              PRINTF "error %u reading disk block %u\n", sts, (clusternumber + allocount) * homeblock -> clusterfactor);
              break;
            }
            sts = WLB ((allocstart + allocount) * homeblock -> clusterfactor, homeblock -> clusterfactor * BLOCKSIZE, clusterbuff);
            if (sts != OZ_SUCCESS) {
              PRINTF "error %u writing disk block %u\n", sts, (allocstart + allocount) * homeblock -> clusterfactor);
              break;
            }
          }
          if (allocount < clustercount) continue;

          /* Modify pointer in the header to point to new blocks */

          pointers[i].logblock = allocstart * homeblock -> clusterfactor;
          fixedheader = 1;
        }
      }
      if (fixedheader) {
        sts = vfy_writeheader (vpb, indexheaders, header);
        if (sts != OZ_SUCCESS) goto rtnsts;
      }
    }
    FREE (clusterbuff);

    /* If we made any changes, re-scan so we get a proper storage bitmap and free block count, as well as to double-check */

    if (fixeddupblocks) {
      PRINTF "\n*** Duplicate block copying complete, re-scanning headers\n");
      goto scanheaders;
    }
  }

  /* Make sure all extension headers are linked properly */

  PRINTF "\n*** Checking extension header links\n");
  for (filenumber = 1; filenumber <= numberoffiles; filenumber ++) {
    if (headersaves[filenumber-1].dircount == 0) continue;
    extnumber = headersaves[filenumber-1].extid.num;
    if (extnumber != 0) {
      if (extnumber > numberoffiles) {
        vfy_printname (vpb, indexheaders, filenumber);
        PRINTF "header %u has bad extid.num %u\n", filenumber, extnumber);
      } else if (memcmp (&(headersaves[filenumber-1].extid), &(headersaves[extnumber-1].fileid), sizeof (OZ_VDFS_Fileid)) != 0) {
        vfy_printname (vpb, indexheaders, filenumber);
        PRINTF "header %u extid %u links to wrong file\n", filenumber, extnumber);
      } else if (headersaves[extnumber-1].dircount != EXT_DIRCOUNT) {
        vfy_printname (vpb, indexheaders, filenumber);
        PRINTF "header %u extid %u links to non-extension header\n", filenumber, extnumber);
      } else if (memcmp (&(headersaves[extnumber-1].dirid), &(headersaves[filenumber-1].fileid), sizeof (OZ_VDFS_Fileid)) != 0) {
        vfy_printname (vpb, indexheaders, filenumber);
        PRINTF "header %u extid %u links back to wrong file\n", filenumber, extnumber);
      }
    }
    if (headersaves[filenumber-1].dircount == EXT_DIRCOUNT) {
      dirnumber = headersaves[filenumber-1].dirid.num;
      if ((dirnumber == 0) || (dirnumber > numberoffiles)) {
        vfy_printname (vpb, indexheaders, filenumber);
        PRINTF "extension header %u has bad dirid.num %u\n", filenumber, dirnumber);
      } else {
        if (memcmp (&(headersaves[filenumber-1].dirid), &(headersaves[dirnumber-1].fileid), sizeof (OZ_VDFS_Fileid)) != 0) {
          vfy_printname (vpb, indexheaders, filenumber);
          PRINTF "extension header %u dirid %u links back to wrong fileid\n", filenumber, dirnumber);
        } else if (memcmp (&(headersaves[dirnumber-1].extid), &(headersaves[filenumber-1].fileid), sizeof (OZ_VDFS_Fileid)) != 0) {
          vfy_printname (vpb, indexheaders, filenumber);
          PRINTF "extension header %u extid %u links to wrong fileid\n", dirnumber, filenumber);
        }
      }
    }
  }

  /* Scan all directories to find the files */

  PRINTF "\n*** Checking directories\n");
  dirbucket = MALLOC (BLOCKSIZE * homeblock -> clusterfactor);
  for (dirnumber = 1; dirnumber <= numberoffiles; dirnumber ++) {

    /* Skip the file if it isn't a directory */

    if (headersaves[dirnumber-1].dircount == EXT_DIRCOUNT) continue;
    if (!(headersaves[dirnumber-1].filattrflags & OZ_FS_FILATTRFLAG_DIRECTORY)) continue;

    /* Open the directory */

    sts = vfy_openfile (vpb, dirnumber, &directory, NULL, indexheaders);
    if (sts != OZ_SUCCESS) {
      PRINTF "error %u opening directory %u\n", sts, dirnumber);
      continue;
    }

    /* Read a bucket at a time from the directory */

    lastname[0] = 0;
    for (divfy_rvbn = 1; divfy_rvbn < EOFBLOCK (directory); divfy_rvbn += homeblock -> clusterfactor) {

      /* Read the bucket */

      sts = vfy_rvb (vpb, directory, divfy_rvbn, BLOCKSIZE * homeblock -> clusterfactor, dirbucket);
      if (sts != OZ_SUCCESS) {
        vfy_printname (vpb, indexheaders, dirnumber);
        PRINTF "error %u reading bucket %u from directory %u\n", sts, divfy_rvbn, dirnumber);
        break;
      }

      /* Scan through the bucket */

      thisname[0] = 0;
      for (i = 0; i < BLOCKSIZE * homeblock -> clusterfactor;) {

        /* Get number of bytes that are the same as last name+1.  If zero, it's the end of the bucket. */

        s = dirbucket[i];
        if (s == 0) break;
        if (-- s > strlen (thisname)) {
          vfy_printname (vpb, indexheaders, dirnumber);
          PRINTF "directory %u bucket %u offset %d skip %u longer than string %s\n", dirnumber, divfy_rvbn, i, s, thisname);
          break;
        }

        /* Get length of the new characters (null terminated) */

        l = strlen (dirbucket + (++ i));
        if (s + l >= sizeof thisname) {
          vfy_printname (vpb, indexheaders, dirnumber);
          PRINTF "directory %u bucket %u offset %d name %s%s too long\n", dirnumber, divfy_rvbn, i, thisname, dirbucket + i);
          break;
        }

        /* Overlay them on old string, skipping the chars that are the same */

        memcpy (thisname + s, dirbucket + i, ++ l);
        i += l;

        /* The name should not be .lt. the last name we got from the whole directory                          */
        /* If we want to nitpick, the only time they should be the same is when new one is from beg of bucket */

        s  = strcmp (thisname, lastname);
        if (s < 0) {
          vfy_printname (vpb, indexheaders, dirnumber);
          PRINTF "directory %u bucket %u name %s comes after %s\n", dirnumber, divfy_rvbn, thisname, lastname);
        }
        if (s > 0) {
          lastversion = -1;
          strcpy (lastname, thisname);
        }

        /* Scan through version array */

        while ((dirpnt = (Dirpnt *)(dirbucket + i)) -> version != 0) {
          i += sizeof *dirpnt;

          /* The new entry's version should be .lt. last entry's */

          if (dirpnt -> version < lastversion) lastversion = dirpnt -> version;
          else {
            vfy_printname (vpb, indexheaders, dirnumber);
            PRINTF "directory %u bucket %u name %s version %u comes after %u\n", dirnumber, divfy_rvbn, thisname, dirpnt -> version, lastversion);
          }

          /* Make sure the dirpnt's file number is valid */

          filenumber = dirpnt -> fileid.num;
          if ((filenumber == 0) || (filenumber > numberoffiles)) {
            vfy_printname (vpb, indexheaders, dirnumber);
            PRINTF "directory %u bucket %u name %s;%u filenumber %u invalid\n", dirnumber, divfy_rvbn, thisname, dirpnt -> version, filenumber);
            continue;
          }

          /* Make sure the whole fileid matches (the header hasn't been re-used) */

          if (memcmp (&(dirpnt -> fileid), &(headersaves[filenumber-1].fileid), sizeof (OZ_VDFS_Fileid)) != 0) {
            vfy_printname (vpb, indexheaders, dirnumber);
            PRINTF "directory %u bucket %u name %s;%u fileid doesn't match\n", dirnumber, divfy_rvbn, thisname, dirpnt -> version);
            continue;
          }

          /* Make sure it isn't an extension header */

          if (headersaves[filenumber-1].dircount == EXT_DIRCOUNT) {
            vfy_printname (vpb, indexheaders, dirnumber);
            PRINTF "directory %u bucket %u name %s;%u points to extension header\n", dirnumber, divfy_rvbn, thisname, dirpnt -> version);
            continue;
          }

          /* Ok, increment number of directory entries found for the file */

          headersaves[filenumber-1].dirfound ++;
        }
        i += sizeof *dirpnt;
      }
    }
    vfy_closefile (vpb, directory);
  }

  /* Maybe there are files that need their dircounts fixed */

  for (filenumber = 1; filenumber <= numberoffiles; filenumber ++) {
    if (headersaves[filenumber-1].fileid.num == 0) continue;
    if (headersaves[filenumber-1].dircount == EXT_DIRCOUNT) continue;
    if (headersaves[filenumber-1].dircount == headersaves[filenumber-1].dirfound) continue;
    vfy_printname (vpb, indexheaders, filenumber);
    PRINTF "file %u claimed to be in %u directories but was found in %u\n", filenumber, headersaves[filenumber-1].dircount, headersaves[filenumber-1].dirfound);
    if (vpb -> rdonly) continue;							// if readonly mode, don't fix it
    sts = vfy_rvb (vpb, indexheaders, filenumber, BLOCKSIZE, header);			// read the existing header
    if (sts != OZ_SUCCESS) {
      PRINTF "error %u reading file header %u\n", sts, filenumber);
      goto rtnsts;
    }
    header -> dircount = headersaves[filenumber-1].dirfound;				// write it back with proper count
    sts = vfy_writeheader (vpb, indexheaders, header);
    if (sts != OZ_SUCCESS) goto rtnsts;
  }

  /* If there are any files marked for delete that aren't deleted, delete them */

  for (filenumber = 1; filenumber <= numberoffiles; filenumber ++) {			// scan through all headers
    if (headersaves[filenumber-1].fileid.num == 0) continue;				// skip if it is deleted
    if (headersaves[filenumber-1].dircount != 0) continue;				// skip if it is in use
    vfy_printname (vpb, indexheaders, filenumber);						// should be deleted but isn't,
    PRINTF "file %u is marked for delete\n", filenumber);				// ... output a message
    if (vpb -> rdonly) continue;							// if readonly mode, don't fix it
    for (extnumber = filenumber; extnumber != 0; extnumber = header -> extid.num) {	// scan through all extensions
      sts = vfy_rvb (vpb, indexheaders, extnumber, BLOCKSIZE, header);			// read the extension header
      if (sts != OZ_SUCCESS) {
        PRINTF "error %u reading file header %u\n", sts, filenumber);
        goto rtnsts;
      }
      pointers = (Pointer *)(header -> area + header -> areas[OZ_FS_HEADER_AREA_POINTER].offs);
      for (i = 0; i < header -> areas[OZ_FS_HEADER_AREA_POINTER].size / sizeof *pointers; i ++) {
        clustercount  = pointers[i].blockcount / homeblock -> clusterfactor;		// get pointer's cluster count
        clusternumber = pointers[i].logblock   / homeblock -> clusterfactor;		// get pointer's block number
        freeclusters += clustercount;							// we're going to have that much more free
        while (clustercount > 0) {							// free off the clusters
          BIT_CLEAR (firstuseclusterbits, clusternumber);
          clustercount  --;
          clusternumber ++;
        }
      }
      headersaves[extnumber-1].fileid.num = 0;						// mark header as actually deleted now
      header -> fileid.num = 0;
      sts = vfy_wvb (vpb, indexheaders, extnumber, BLOCKSIZE, header);			// write it out to disk
      if (sts != OZ_SUCCESS) {
        PRINTF "error %u writing file header %u\n", sts, filenumber);
        goto rtnsts;
      }
      BIT_CLEAR (indexbitmapbits, extnumber - 1);					// mark the header free in bitmap
      indexbitmapfixed = 1;
    }
  }

  /* Maybe write out a fixed index bitmap */

  if (!(vpb -> rdonly)) {
    PRINTF "\n*** Writing bitmaps\n");

    if (indexbitmapfixed) {
      sts = vfy_wvb (vpb, indexbitmap, 1, indexbitmapsize, indexbitmapbits);
      if (sts != OZ_SUCCESS) {
        PRINTF "error %u writing %u bytes from index bitmap file\n", sts, indexbitmapsize);
        goto rtnsts;
      }
    }
  }

  /* Write out the storage bitmap (we don't care about the old contents) */

  if (!(vpb -> rdonly)) {
    sts = vfy_openfile (vpb, SACRED_FIDNUM_STORAGEBITMAP, &storagebitmap, NULL, indexheaders);
    if (sts != OZ_SUCCESS) {
      PRINTF "error %u opening storage bitmap\n", sts);
      goto rtnsts;
    }
    sts = vfy_wvb (vpb, storagebitmap, 1, storagebitmapsize, firstuseclusterbits);
    if (sts != OZ_SUCCESS) {
      PRINTF "error %u writing storage bitmap\n", sts);
      goto rtnsts;
    }
  }

  /* Maybe fix homeblock's free block count */

  if (homeblock -> clustersfree != freeclusters) {
    PRINTF "homeblock indicated %u free clusters, but there are actually %u\n", homeblock -> clustersfree, freeclusters);
  }

  if (!(vpb -> rdonly) && (OZ_HW_DATEBIN_TST (homeblock -> lastwritemount) || (homeblock -> clustersfree != freeclusters))) {
    OZ_HW_DATEBIN_CLR (homeblock -> lastwritemount);
    homeblock -> clustersfree = freeclusters;

    cksm = 0;
    homeblock -> checksum = 0;
    for (i = 0; i < sizeof *homeblock / sizeof (uWord); i ++) {
      cksm -= ((uWord *)homeblock)[i];
    }
    homeblock -> checksum = cksm;

    sts = WLB (1, BLOCKSIZE, homeblock);
    if (sts != OZ_SUCCESS) {
      PRINTF "error %u writing home block\n", sts);
      goto rtnsts;
    }
  }

  sts = OZ_SUCCESS;

  /* Free off any left-over malloc'd memory and return status */

rtnsts:
  while ((mem = vpb -> allmem) != NULL) {
    vpb -> allmem = mem -> next;
    oz_sys_pdata_free (OZ_PROCMODE_KNL, mem);
  }

  oz_hw_cpu_setsoftint (si);

  return (sts);
}

/************************************************************************/
/*									*/
/*  Print filename for a given filenumber				*/
/*									*/
/*    Input:								*/
/*									*/
/*	vpb          = verify parameter block pointer			*/
/*	indexheaders = index header file				*/
/*	filenumber   = number of file to get the name of		*/
/*									*/
/************************************************************************/

static void vfy_printname (Vpb *vpb, Vfyfile *indexheaders, OZ_Dbn filenumber)

{
  char *filename, filenamebuff[256];
  Header *header;
  int i, l;
  OZ_VDFS_Fileid dirid;
  uLong sts;

  header = MALLOC (BLOCKSIZE);

  i = sizeof filenamebuff;
  filenamebuff[--i] = 0;

  memset (&dirid, 0, sizeof dirid);

  while (vfy_rvb (vpb, indexheaders, filenumber, BLOCKSIZE, header) == OZ_SUCCESS) {
    if ((dirid.num != 0) && (memcmp (&(header -> fileid), &dirid, sizeof dirid) != 0)) break;
    if (header -> dircount == EXT_DIRCOUNT) {
      filenumber = header -> dirid.num;
      continue;
    }
    filename = (char *)(header -> area + header -> areas[OZ_FS_HEADER_AREA_FILNAME].offs);
    l = strlen (filename);
    if (l > i) break;
    i -= l;
    memcpy (filenamebuff + i, filename, l);
    if (i == 0) break;
    filenumber = header -> dirid.num;
    if (filenumber == SACRED_FIDNUM_ROOTDIRECTORY) {
      filenamebuff[--i] = '/';
      break;
    }
    dirid = header -> dirid;
  }

  FREE (header);

  if (filenamebuff[i] != 0) PRINTF "%s:\n", filenamebuff + i);
}

/************************************************************************/
/*									*/
/*  Open a file								*/
/*									*/
/************************************************************************/

static uLong vfy_openfile (Vpb *vpb, OZ_Dbn filenumber, Vfyfile **file_r, Homeblock *homeblock, Vfyfile *indexheaders)

{
  int i;
  OZ_Dbn headerlbn, nblocks;
  uLong sts;
  uWord cksm;
  Vfyfile *file;

  do {

    /* Get file's header block lbn */

    if (filenumber == SACRED_FIDNUM_INDEXHEADERS) headerlbn = homeblock -> indexhdrlbn;
    else {
      sts = vfy_vbn2lbn (indexheaders, filenumber, &nblocks, &headerlbn);
      if (sts != OZ_SUCCESS) return (sts);
    }

    /* Allocate file struct and read header into it */

    file = MALLOC (BLOCKSIZE + sizeof *file);
    memset (file, 0, sizeof *file);
    sts  = RLB (headerlbn, BLOCKSIZE, &(file -> header));
    if (sts != OZ_SUCCESS) return (sts);

    /* Validate header version and checksum */

    if (file -> header.headerver != HEADER_VERSION) return (OZ_BADFILEVER);
    cksm = 0;
    for (i = 0; i < BLOCKSIZE / 2; i ++) {
      cksm += ((uWord *)&(file -> header))[i];
    }
    if (cksm != 0) return (OZ_BADHDRCKSM);

    /* Return file struct and continue with any extension headers */

    *file_r = file;
    file_r = &(file -> extfile);
    filenumber = file -> header.extid.num;
  } while (filenumber != 0);

  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Close a file							*/
/*									*/
/************************************************************************/

static void vfy_closefile (Vpb *vpb, Vfyfile *file)

{
  Vfyfile *extfile;

  while (file != NULL) {
    extfile = file -> extfile;
    FREE (file);
    file = extfile;
  }
}

/************************************************************************/
/*									*/
/*  Write file header back to index file				*/
/*									*/
/*    Input:								*/
/*									*/
/*	indexheaders = index header file				*/
/*	header = header to write out					*/
/*									*/
/************************************************************************/

static uLong vfy_writeheader (Vpb *vpb, Vfyfile *indexheaders, Header *header)

{
  int i;
  uLong sts;
  uWord cksm;

  cksm = 0;
  header -> checksum = 0;
  for (i = 0; i < BLOCKSIZE / sizeof (uWord); i ++) {
    cksm -= ((uWord *)header)[i];
  }
  header -> checksum = cksm;

  sts = vfy_wvb (vpb, indexheaders, header -> fileid.num, BLOCKSIZE, header);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Read virtual blocks from a file					*/
/*									*/
/************************************************************************/

static uLong vfy_rvb (Vpb *vpb, Vfyfile *file, OZ_Dbn vbn, uLong size, void *buff)

{
  OZ_Dbn lbn, nblocks;
  uLong len, sts;

  while (size > 0) {
    sts = vfy_vbn2lbn (file, vbn, &nblocks, &lbn);
    if (sts != OZ_SUCCESS) return (sts);
    len = nblocks * BLOCKSIZE;
    if (len > size) len = size;
    sts = RLB (lbn, len, buff);
    if (sts != OZ_SUCCESS) return (sts);
    vbn  += len / BLOCKSIZE;
    size -= len;
    (OZ_Pointer)buff += len;
  }
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Write virtual blocks to a file					*/
/*									*/
/************************************************************************/

static uLong vfy_wvb (Vpb *vpb, Vfyfile *file, OZ_Dbn vbn, uLong size, void *buff)

{
  OZ_Dbn lbn, nblocks;
  uLong len, sts;

  while (size > 0) {
    sts = vfy_vbn2lbn (file, vbn, &nblocks, &lbn);
    if (sts != OZ_SUCCESS) return (sts);
    len = nblocks * BLOCKSIZE;
    if (len > size) len = size;
    sts = WLB (lbn, len, buff);
    if (sts != OZ_SUCCESS) return (sts);
    vbn  += len / BLOCKSIZE;
    size -= len;
    (OZ_Pointer)buff += len;
  }
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Map virtual block number in a file to logical block number on the 	*/
/*  disk								*/
/*									*/
/*    Input:								*/
/*									*/
/*	file = file that virtual block is in				*/
/*	virtblock = virtual block to locate				*/
/*									*/
/*    Output:								*/
/*									*/
/*	vfy_vbn2lbn = OZ_SUCCESS : conversion successful		*/
/*	                    else : error status				*/
/*	*nblocks_r  = number of contig blocks at *logblock_r		*/
/*	*logblock_r = starting logical block number			*/
/*									*/
/************************************************************************/

static uLong vfy_vbn2lbn (Vfyfile *file, OZ_Dbn virtblock, OZ_Dbn *nblocks_r, OZ_Dbn *logblock_r)

{
  int i;
  OZ_Dbn nblocks;
  Pointer *pointers;

  if (virtblock == 0) return (OZ_VBNZERO);
  virtblock --;

  do {
    pointers = (Pointer *)(file -> header.area + file -> header.areas[OZ_FS_HEADER_AREA_POINTER].offs);
    for (i = 0; i < file -> header.areas[OZ_FS_HEADER_AREA_POINTER].size / sizeof *pointers; i ++) {
      nblocks = pointers[i].blockcount;
      if (nblocks > virtblock) {
        *nblocks_r  = nblocks - virtblock;
        *logblock_r = pointers[i].logblock + virtblock;
        return (OZ_SUCCESS);
      }
      virtblock -= nblocks;
    }
    file = file -> extfile;
  } while (file != NULL);

  return (OZ_ENDOFFILE);
}

/************************************************************************/
/*									*/
/*  Malloc/Free wrappers so we can be sloppy about freeing stuff	*/
/*									*/
/************************************************************************/

static void *vfy_malloc (Vpb *vpb, uLong size)

{
  Vfymem *mem;

  mem = oz_sys_pdata_malloc (OZ_PROCMODE_KNL, size + sizeof *mem);	// malloc block including our header
  mem -> next = vpb -> allmem;						// link header to list of all memory allocated
  mem -> prev = (Vfymem **)&(vpb -> allmem);
  if (vpb -> allmem != NULL) ((Vfymem *)(vpb -> allmem)) -> prev = &(mem -> next);
  vpb -> allmem = mem;
  return (mem + 1);							// return pointer just past our header
}

static void vfy_free (Vpb *vpb, void *buff)

{
  Vfymem *mem;

  mem = ((Vfymem *)buff) - 1;						// point to our header
  *(mem -> prev) = mem -> next;						// unlink it from allmem list
  if (mem -> next != NULL) mem -> next -> prev = mem -> prev;
  oz_sys_pdata_free (OZ_PROCMODE_KNL, mem);				// free it off
}
