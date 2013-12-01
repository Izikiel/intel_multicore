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
/*  This is the CDROM filesystem driver					*/
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

#define BLOCKSIZE 2048
#define FILENAME_MAX 256
#define VOLNAME_MAX 36

#if OZ_HW_LITTLEENDIAN
typedef struct { uLong fwd;
                 uLong rev;
               } uLong_both_endian;

typedef struct { uWord fwd;
                 uWord rev;
               } uWord_both_endian;

typedef struct { uLong fwd;
               } uLong_little_endian;

typedef struct { uLong rev;
               } uLong_big_endian;
#endif

#if OZ_HW_BIGENDIAN
typedef struct { uLong rev;
                 uLong fwd;
               } uLong_both_endian;

typedef struct { uWord rev;
                 uWord fwd;
               } uWord_both_endian;

typedef struct { uLong rev;
               } uLong_little_endian;

typedef struct { uLong fwd;
               } uLong_big_endian;
#endif

typedef struct { char year[4];		// 0001..9999
                 char month[2];		// 01..12
                 char day[2];		// 01..31
                 char hour[2];		// 00..23
                 char minute[2];	// 00..59
                 char second[2];	// 00..59
                 char hundths[2];	// 00..99
                 Byte gmtoffs;		// mult of 15 min, -48(west) to +52(East)
               } Datetime17;

typedef struct { uByte yearssince1900;
                 uByte month;
                 uByte day;
                 uByte hour;
                 uByte minute;
                 uByte second;
                 signed char gmtoffs;
               } Datetime7;

/* Directory record */

#define DIRECTORY_FILEFLAG_HIDDEN 0x01
#define DIRECTORY_FILEFLAG_DIRECTORY 0x02
#define DIRECTORY_FILEFLAG_ASSOCIATEDFILE 0x04
#define DIRECTORY_FILEFLAG_NZRECORDFORMAT 0x08
#define DIRECTORY_FILEFLAG_PROTECTION 0x10
#define DIRECTORY_FILEFLAG_MULTIEXTENT 0x80

#pragma pack (1)
typedef struct {
  uByte directoryrecordlength;			// 22
  uByte extendedattributerecordlength;		// 00
  uLong_both_endian locationofextent;		// 1d 00 00 00 00 00 00 1d
  uLong_both_endian datalength;			// 00 10 00 00 00 00 10 00
  Datetime7 recordingdatetime;			// 65 09 18 11 26 15 f0
  uByte fileflags;				// 02
  uByte fileunitsize;				// interleave file unit size (0 if not interleaved)
  uByte interleavegapsize;			// interleave gap size (0 if not interleaved)
  uWord_both_endian volumesequencenumber;	// which volume of the set this file's extent is recorded on
  uByte fileidentifierlength;			// no null terminator
  char fileidentifier[1];			// "AUTORUN.;1", "BOOT.CAT;1"
} Directoryrecord;
#pragma nopack

/* Volume descriptors form an array starting at Logical Sector Number 16 */

#define LSN_PRIVOLDES 16

#define VOLDESTYPE_BOOT 0
#define VOLDESTYPE_PRIM 1
#define VOLDESTYPE_SUPP 2
#define VOLDESTYPE_PART 3
#define VOLDESTYPE_TERM 255

#define VOLDESVER_BOOT 1
#define VOLDESVER_PRIM 1
#define VOLDESVER_SUPP_SUPPLEMENTARY 1
#define VOLDESVER_SUPP_ENHANCED 2
#define VOLDESVER_PART 1

#define FILESTRUCTUREVERSION_STANDARD 1
#define FILESTRUCTUREVERSION_ENHANCED 2

#pragma pack (1)
typedef struct {
  uByte voldestype;		// volume descriptor type
  char cd001[5];		// contains "CD001"
  uByte voldesver;		// volume descriptor version
  union {

    /* Boot volume descriptor */

    struct {
      char bootsystemidentifier[32];	// boot system identifier (CPU type?)
      char bootidentifier[32];		// operating system name being booted
      uByte bootsystemuse[1977];	// boot block contents
    } boot;

    /* Primary volume descriptor */

    struct {
      char unused1[1];
      char systemidentifier[32];
      char volumeidentifier[32];
      char unused2[8];
      uLong_both_endian volumespacesize;		// 1c 0d 05 00 00 05 0d 1c
      char unused3[32];
      uWord_both_endian volumesetsize;			// 01 00 00 01
      uWord_both_endian volumesequencenumber;		// 01 00 00 01
      uWord_both_endian logicalblocksize;		// 00 08 08 00
      uLong_both_endian pathtablesize;			// 0a 01 00 00 00 00 01 0a
      uLong_little_endian loctypelpathtable;		// 15 00 00 00
      uLong_little_endian locopttypelpathtable;		// 00 00 00 00
      uLong_big_endian loctypempathtable;		// 00 00 00 17
      uLong_big_endian locopttypempathtable;		// 00 00 00 00
      Directoryrecord rootdirectoryrecord;		// 809C: 
      char volumesetidentifier[128];			// 
      char publisheridentifier[128];
      char dataprepareridentifier[128];
      char applicationidentifier[128];
      char copyrightfileidentifier[37];
      char abstractfileidentifier[37];
      char bibliographicfileidentifier[37];
      Datetime17 volumecreationdatetime;
      Datetime17 volumemodificationdatetime;
      Datetime17 volumeexpirationdatetime;
      Datetime17 volumeeffectivedatetime;
      char filestructureversion[1];
      char unused4[1];
      char applicationuse[512];
      char unused5[653];
    } prim;

    /* Supplementary volume descriptor */

#define SUPP_VOLUMEFLAGS_NONISO2375_ESCAPESEQUENCES 0x01

    struct {
      uByte volumeflags;
      char systemidentifier[32];
      char volumeidentifier[32];
      char unused2[8];
      uLong_both_endian volumespacesize;
      char escapesequences[32];
      uWord_both_endian volumesetsize;
      uWord_both_endian volumesequencenumber;
      uWord_both_endian logicalblocksize;
      uLong_both_endian pathtablesize;
      uLong_little_endian loctypelpathtable;
      uLong_little_endian locopttypelpathtable;
      uLong_big_endian loctypempathtable;
      uLong_big_endian locopttypempathtable;
      Directoryrecord rootdirectoryrecord;
      char volumesetidentifier[128];
      char publisheridentifier[128];
      char dataprepareridentifier[128];
      char applicationidentifier[128];
      char copyrightfileidentifier[37];
      char abstractfileidentifier[37];
      char bibliographicfileidentifier[37];
      Datetime17 volumecreationdatetime;
      Datetime17 volumemodificationdatetime;
      Datetime17 volumeexpirationdatetime;
      Datetime17 volumeeffectivedatetime;
      char filestructureversion[1];
      char unused4[1];
      char applicationuse[512];
      char unused5[653];
    } supp;

    /* Volume Partition Descriptor */

    struct {
      char unused1[1];
      char systemidentifier[32];
      char volumepartitionidentifier[32];
      uLong_both_endian volumepartitionlocation;
      uLong_both_endian volumepartitionsize;
      char notspecified[1960];
    } part;

    /* Terminator volume descriptor */

    struct {
      char zeroes[2041];
    } term;
  } vd;
} Volumedescriptor;
#pragma nopack

/* Get file size */

#define EFBLK(__dirent) (((__dirent) -> datalength.fwd / BLOCKSIZE) + 1)
#define EFBYT(__dirent) (((__dirent) -> datalength.fwd % BLOCKSIZE))
#define HIBLK(__dirent) (((__dirent) -> datalength.fwd + BLOCKSIZE - 1) / BLOCKSIZE)

/* Test the directory bit */

#define IS_DIRECTORY(__dirent) (((__dirent) -> fileflags & DIRECTORY_FILEFLAG_DIRECTORY) != 0)

/* Our File-id structure */

#define FIDLEVEL_MAX (OZ_FS_MAXFIDLN / sizeof (Fidlvl))

typedef struct { OZ_Dbn lbn;		// logical block number of directory entry
                 uLong ofs;		// offset in that logical block of directory entry
                 uWord vol;		// volume sequence number of the directory entry
                 uWord pad;		// padding to 12-byte boundary
               } Fidlvl;

struct OZ_VDFS_Fileid { Fidlvl fidlvl[FIDLEVEL_MAX]; };	// [0] = directory record for the file itself
							// [1] = directory record for the file's directory
							// [2] = directory record for the next outer level
							// ...
							// [n] = root directory record in primary volume descriptor
							// ... = zeroes

/* In-memory volume extension info */

struct OZ_VDFS_Volex { OZ_VDFS_File *rootdirectory;	// pointer to root directory header in openfiles list
                       char volumelabel[VOLNAME_MAX];	// null-terminated volume name
                       union {				// used by various mutually exclusive functions:
                         struct {			// lookup_file function
                           OZ_VDFS_Filex *dirfilex;	// - directory being searched
                           OZ_Dbn vbn, lbn;		// - directory block number being searched
                           char fname[FILENAME_MAX];	// - filename we're looking for, without ;version, but with null
                           char *name_r;		// - where to return the resultant filename, including ;version
                           int namelen;			// - length of fname, including the null
                           int versign;			// - 0: positive version, -1: negative version
                           OZ_VDFS_Fileid *fileid_r;	// - where to return the found file-id
                           uLong version;		// - version number (abs value)
                           char lastname[FILENAME_MAX];	// - last filename scanned in bucket
                         } lf;
                       } v;

                       Volumedescriptor privoldes;	// primary volume descriptor
                     };

/* In-memory file extension info */

struct OZ_VDFS_Filex { OZ_VDFS_Filex *next;		// next extension file pointer
                       OZ_VDFS_Fileid fileid;		// it's fileid
                       Directoryrecord directoryrecord;	// copy of directory record
                       char padding[255];		// padding for max filename string length
                     };


static OZ_VDFS_Fileid rootfid;

/* Vector routines */

static int cdfs_is_directory (OZ_VDFS_File *file);
static int cdfs_fis_writethru (OZ_VDFS_File *file, OZ_Dbn virtblock, uLong blockoffs, uLong size, const void *buff);
static int cdfs_vis_writethru (OZ_VDFS_Volume *volume);
static const char *cdfs_get_volname (OZ_VDFS_Volume *volume);
static uLong cdfs_getinfo1 (OZ_VDFS_Iopex *iopex);
static void cdfs_wildscan_continue (OZ_VDFS_Chnex *chnex);
static void cdfs_wildscan_terminate (OZ_VDFS_Chnex *chnex);
static uLong getfnfromdirrec (Directoryrecord *dirrec, int namesize, char *namebuff);
static uLong cdfs_getinfo2 (OZ_VDFS_Iopex *iopex);
static uLong cdfs_getinfo3 (OZ_VDFS_Iopex *iopex);
static uLong cdfs_init_volume (OZ_VDFS_Devex *devex, OZ_VDFS_Volume *volume, int volnamelen, const char *volname, uLong clusterfactor, uLong secattrsize, const void *secattrbuff, uLong initflags, OZ_VDFS_Iopex *iopex);
static uLong cdfs_mount_volume (OZ_VDFS_Devex *devex, OZ_VDFS_Volume *volume, uLong mountflags, OZ_VDFS_Iopex *iopex);
static uLong cdfs_dismount_volume (OZ_VDFS_Devex *devex, OZ_VDFS_Volume *volume, int unload, int shutdown, OZ_VDFS_Iopex *iopex);
static uLong cdfs_verify_volume (OZ_VDFS_Iopex *iopex, OZ_VDFS_Devex *devex);
static uLong cdfs_get_rootdirid (OZ_VDFS_Devex *devex, OZ_VDFS_Fileid *rootdirid_r);
static const OZ_VDFS_Fileid *cdfs_get_fileid (OZ_VDFS_File *file);
static uLong cdfs_lookup_file (OZ_VDFS_File *dirfile, int namelen, const char *name, OZ_VDFS_Fileid *fileid_r, char *name_r, OZ_VDFS_Iopex *iopex);
static uLong cdfs_enter_file (OZ_VDFS_File *dirfile, const char *dirname, int namelen, const char *name, int newversion, OZ_VDFS_File *file, const OZ_VDFS_Fileid *fileid, char *name_r, OZ_VDFS_Iopex *iopex);
static uLong cdfs_remove_file (OZ_VDFS_File *dirfile, const char *name, char *name_r, OZ_VDFS_Iopex *iopex);
static void cdfs_returnspec (char *spec, uLong size, char *buff, OZ_FS_Subs *subs);
static uLong cdfs_create_file (OZ_VDFS_Volume *volume, int namelen, const char *name, uLong filattrflags, OZ_VDFS_Fileid *dirid, OZ_VDFS_File *file, OZ_VDFS_Fileid **fileid_r, OZ_VDFS_Iopex *iopex);
static OZ_VDFS_File *cdfs_findopenfile (OZ_VDFS_Volume *volume, const OZ_VDFS_Fileid *fileid);
static uLong cdfs_open_file (OZ_VDFS_Volume *volume, const OZ_VDFS_Fileid *fileid, OZ_VDFS_File *file, OZ_VDFS_Iopex *iopex);
static uLong cdfs_set_file_attrs (OZ_VDFS_File *file, uLong numitems, const OZ_Itmlst2 *itemlist, OZ_VDFS_Iopex *iopex);
static uLong cdfs_close_file (OZ_VDFS_File *file, OZ_VDFS_Iopex *iopex);
static uLong cdfs_extend_file (OZ_VDFS_File *file, OZ_Dbn nblocks, uLong extflags, OZ_VDFS_Iopex *iopex);
static uLong cdfs_write_dirty_header (OZ_VDFS_File *dirtyfile, Long alf, OZ_Datebin now, OZ_VDFS_Volume *volume, OZ_VDFS_Iopex *iopex);
static uLong cdfs_writehomeblock (OZ_VDFS_Volume *volume, OZ_VDFS_Iopex *iopex);
static uLong cdfs_map_vbn_to_lbn (OZ_VDFS_File *file, OZ_Dbn virtblock, OZ_Dbn *nblocks_r, OZ_Dbn *logblock_r);
static void cdfs_mark_header_dirty (OZ_VDFS_File *dirtyfile);

/* Internal routines */

static uLong lookup_file (OZ_VDFS_File *dirfile, int namelen, const char *name, OZ_Dbn *dirvbn_r, OZ_VDFS_Fileid *fileid_r, char *name_r, OZ_VDFS_Iopex *iopex);
static uLong read_header_block (const OZ_VDFS_Fileid *fileid, OZ_VDFS_Filex *filex, OZ_VDFS_Iopex *iopex);
static void write_dirty_homeboy (OZ_VDFS_Volume *volume, OZ_VDFS_Iopex *iopex);

/************************************************************************/
/*									*/
/*  Boot-time initialization routine					*/
/*									*/
/************************************************************************/

static const OZ_VDFS_Vector vector = { sizeof (OZ_VDFS_Fileid), 
                                       VOLNAME_MAX, FILENAME_MAX, 0, 
                                       1, 				// it does versions

                                       cdfs_close_file, 		// close a file
                                       cdfs_create_file, 		// create a new file
                                       cdfs_dismount_volume, 		// dismount volume
                                       cdfs_enter_file, 		// enter a new name in a directory
                                       cdfs_extend_file, 		// extend a file
                                       cdfs_findopenfile, 		// see if a file is already open
                                       cdfs_get_rootdirid, 		// get root directory id
                                       cdfs_get_volname, 		// get volume name
                                       cdfs_getinfo2, 			// get name of file open on a channel
                                       cdfs_init_volume, 		// initialize a volume
                                       cdfs_lookup_file, 		// look up a particular file in a directory
                                       cdfs_mount_volume, 		// mount volume
                                       cdfs_open_file, 			// open a file
                                       cdfs_remove_file, 		// remove name from directory
                                       cdfs_set_file_attrs, 		// write a file's attributes
                                       cdfs_write_dirty_header, 	// flush file's header(s) to disk
                                       cdfs_writehomeblock, 		// flush volume's header to disk
                                       cdfs_verify_volume, 		// verify volume structures

                                       cdfs_fis_writethru, 		// see if file is a 'writethru' file
                                       cdfs_get_fileid, 		// get file id
                                       cdfs_getinfo1, 			// get info about the file open on channel
                                       cdfs_getinfo3, 			// get info about the volume
                                       cdfs_is_directory, 		// see if file is a directory
                                       cdfs_map_vbn_to_lbn, 		// map a file's vbn to equivalent lbn 
                                       cdfs_mark_header_dirty, 		// mark (prime) header dirty
                                       cdfs_returnspec, 		// return filespec string/substrings
                                       cdfs_vis_writethru, 		// see if volume is a 'writethru' volume
                                       cdfs_wildscan_continue, 		// scan directory block for a particular wildcard match
                                       cdfs_wildscan_terminate };	// terminate wildcard scan

void oz_dev_cdfs_init ()

{
  OZ_VDFS_Volex *volex;

  volex = NULL;
  memset (&rootfid, 0, sizeof rootfid);
  rootfid.fidlvl[0].vol = 1;
  rootfid.fidlvl[0].lbn = LSN_PRIVOLDES;
  rootfid.fidlvl[0].ofs = (OZ_Pointer)&(volex -> privoldes.vd.prim.rootdirectoryrecord) - (OZ_Pointer)&(volex -> privoldes);

  oz_dev_vdfs_init (OZ_VDFS_VERSION, "oz_cdfs", &vector);
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

static int cdfs_is_directory (OZ_VDFS_File *file)

{
  return (IS_DIRECTORY (&(file -> filex -> directoryrecord)));
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

static int cdfs_fis_writethru (OZ_VDFS_File *file, OZ_Dbn virtblock, uLong blockoffs, uLong size, const void *buff)

{
  return (0);
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

static int cdfs_vis_writethru (OZ_VDFS_Volume *volume)

{
  return (0);
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
/*	cdfs_get_volname = pointer to null-terminated label string	*/
/*									*/
/************************************************************************/

static const char *cdfs_get_volname (OZ_VDFS_Volume *volume)

{
  return (volume -> volex -> volumelabel);
}

/************************************************************************/
/*									*/
/*  Get information part 1						*/
/*									*/
/************************************************************************/

static uLong cdfs_getinfo1 (OZ_VDFS_Iopex *iopex)

{
  OZ_VDFS_Filex *filex;
  uLong datelongs[OZ_DATELONG_ELEMENTS], yyyymmdd;

  filex = iopex -> chnex -> file -> filex;

  iopex -> u.getinfo1.p.filattrflags = IS_DIRECTORY (&(filex -> directoryrecord)) ? OZ_FS_FILATTRFLAG_DIRECTORY : 0;

  memset (datelongs, 0, sizeof datelongs);
  yyyymmdd  = (filex -> directoryrecord.recordingdatetime.yearssince1900 + 1900) << 16;
  yyyymmdd +=  filex -> directoryrecord.recordingdatetime.month << 8;
  yyyymmdd +=  filex -> directoryrecord.recordingdatetime.day;
  datelongs[OZ_DATELONG_DAYNUMBER]   = oz_sys_daynumber_encode (yyyymmdd);
  datelongs[OZ_DATELONG_SECOND]      = filex -> directoryrecord.recordingdatetime.hour * 3600 
                                     + filex -> directoryrecord.recordingdatetime.minute * 60 
                                     + filex -> directoryrecord.recordingdatetime.second;
  iopex -> u.getinfo1.p.create_date  = oz_sys_datebin_encode (datelongs);
  iopex -> u.getinfo1.p.create_date -= filex -> directoryrecord.recordingdatetime.gmtoffs * 15 * 60 * OZ_TIMER_RESOLUTION;
  iopex -> u.getinfo1.p.access_date  = iopex -> u.getinfo1.p.create_date;
  iopex -> u.getinfo1.p.change_date  = iopex -> u.getinfo1.p.create_date;
  iopex -> u.getinfo1.p.modify_date  = iopex -> u.getinfo1.p.create_date;
  iopex -> u.getinfo1.p.expire_date  = 0;
  iopex -> u.getinfo1.p.backup_date  = 0;
  iopex -> u.getinfo1.p.archive_date = 0;
  if (iopex -> u.getinfo1.p.fileidbuff != NULL) {
    movc4 (sizeof filex -> fileid, &(filex -> fileid), iopex -> u.getinfo1.p.fileidsize, iopex -> u.getinfo1.p.fileidbuff);
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

static void cdfs_wildscan_continue (OZ_VDFS_Chnex *chnex)

{
  char c, *p;
  Directoryrecord *dirrec;
  int filename_l, newname, nver, rc;
  OZ_Dbn nblocks;
  OZ_IO_fs_open fs_open;
  OZ_VDFS_Chnex *dirchnex;
  OZ_VDFS_Devex *devex;
  OZ_VDFS_File *dirfile;
  OZ_VDFS_Fileid fileid;
  OZ_VDFS_Iopex *iopex;
  OZ_VDFS_Volume *volume;
  OZ_VDFS_Wildscan *outerwild, *wildscan;
  uLong sts, version, vl;

  iopex    = chnex -> wild_iopex;
  volume   = iopex -> devex -> volume;
  wildscan = chnex -> wildscan;
  devex    = iopex -> devex;
  dirfile  = ((OZ_VDFS_Chnex *)oz_knl_iochan_ex (wildscan -> iochan)) -> file;

  fileid.fidlvl[0].vol = dirfile -> filex -> fileid.fidlvl[0].vol;
  fileid.fidlvl[0].pad = 0;

  sts = cdfs_map_vbn_to_lbn (dirfile, wildscan -> blockvbn, &nblocks, &fileid.fidlvl[0].lbn);
  if (sts != OZ_SUCCESS) {
    oz_dev_vdfs_wildscan_iodonex (iopex, sts);
    return;
  }

  if (dirfile -> filex -> fileid.fidlvl[FIDLEVEL_MAX-1].vol != 0) {
    oz_dev_vdfs_wildscan_iodonex (iopex, OZ_DIRECTORYLOOP);
    return;
  }
  memcpy (fileid.fidlvl + 1, dirfile -> filex -> fileid.fidlvl + 0, sizeof fileid.fidlvl - sizeof fileid.fidlvl[0]);

  /* Scan block for matching filename entry */

  while (wildscan -> blockoffs < BLOCKSIZE) {

    dirrec = (Directoryrecord *)(wildscan -> blockbuff + wildscan -> blockoffs);
    if (dirrec -> directoryrecordlength == 0) break;

    fileid.fidlvl[0].ofs = wildscan -> blockoffs;

    filename_l = dirrec -> directoryrecordlength;					// get length of different chars
    if ((filename_l >= sizeof wildscan -> lastname) || (filename_l + wildscan -> blockoffs >= BLOCKSIZE)) {
      oz_dev_vdfs_printk (iopex, "oz_dev_dfs wildscan_continue: filename %s in directory %s too long\n", p, wildscan -> basespec);
      goto dircorrupt;
    }
    wildscan -> blockoffs += filename_l;						// point to next record in block
    if ((dirrec -> fileidentifier[0] == 0) || (dirrec -> fileidentifier[0] == 1)) continue; // skip the self and parent entries

    /* Point to the filename in the directory block buffer */

    version = getfnfromdirrec (dirrec, sizeof wildscan -> lastname, wildscan -> lastname);
    filename_l = strlen (wildscan -> lastname);

    /* See if the filename in the blockbuff matches the wildcard spec, and see if we should scan the sub-sirectory */

    rc = oz_dev_vdfs_wildscan_match (wildscan, wildscan -> lastname);

    /* Maybe scan sub-directory.  If we also output this directory, we do it either after or before the directory */
    /* contents (depending on the setting of delaydir), so we let the subdir output stuff handle that.            */

    if ((wildscan -> lastname[filename_l-1] == '/') && (rc & 2)) {
      if (iopex -> aborted) {								// don't bother if I/O request is aborted
        oz_dev_vdfs_wildscan_iodonex (iopex, OZ_ABORTED);
        return;
      }
      if (memcmp (&fileid, &rootfid, sizeof fileid) != 0) {				// don't nest into the root directory
											// ... to prevent infinite loop
        if (strlen (wildscan -> basespec) + filename_l >= sizeof wildscan -> basespec) { // make sure directory name not too long
          oz_dev_vdfs_wildscan_iodonex (iopex, OZ_FILENAMETOOLONG);
          return;
        }
        for (outerwild = wildscan; outerwild != NULL; outerwild = outerwild -> nextouter) { // don't nest into same directory twice
          dirchnex = oz_knl_iochan_ex (outerwild -> iochan);
          if (memcmp (&fileid, &(dirchnex -> file -> filex -> fileid), sizeof fileid) == 0) {
            oz_dev_vdfs_wildscan_iodonex (iopex, OZ_DIRECTORYLOOP);
            return;
          }
        }
        oz_dev_vdfs_wildscan_startsubdir (chnex, wildscan -> lastname, &fileid, rc);	// start processing sub-directory
        return;
      }
    }

    /* Return match to caller */

    if (!(rc & 1)) continue;								// don't if name didn't match

    if (wildscan -> lastname[filename_l-1] == '/') {					// see if it is a dir name
      if (!(wildscan -> ver_incldirs)) continue;					// skip if we don't include directories
      oz_dev_vdfs_wildscan_output (chnex, wildscan -> lastname, 0, &fileid);		// ok, output it
      return;
    }

    if (wildscan -> ver_inclallfiles) {							// see if incl all versions of files
      if (nver > 1) {									// ok, see if more than one version
        wildscan -> ver_output = 1;							// if so, do more on next call
      }
      oz_dev_vdfs_wildscan_output (chnex, wildscan -> lastname, version, &fileid);	// output this version
      return;
    }

    if (wildscan -> ver_number > 0) {							// see if looking for one particular version
      if (version == wildscan -> ver_number) {
        oz_dev_vdfs_wildscan_output (chnex, wildscan -> lastname, version, &fileid);	// found, output the entry
        return;
      }
      continue;										// not found, skip the entry
    }

    if (newname) {									// relative version, see if new filename
      wildscan -> ver_count = wildscan -> ver_number;					// if so, reset version counter
    }
    if ((wildscan -> ver_count != 0) && (nver > 0)) continue;				// see if any more versions to skip
    if (nver <= 0) continue;								// skip this entry if didn't get count
    wildscan -> ver_count = 1;								// only do one from this name
    oz_dev_vdfs_wildscan_output (chnex, wildscan -> lastname, version, &fileid);	// output it
    return;
  }

  /* Reached end of currrent block, start reading a new one */

  wildscan -> blockvbn ++;
  oz_dev_vdfs_wildscan_readdir (chnex);
  return;

  /* Something is corrupt about the directory block */

dircorrupt:
  oz_dev_vdfs_wildscan_iodonex (iopex, OZ_DIRCORRUPT);
}

static void cdfs_wildscan_terminate (OZ_VDFS_Chnex *chnex)

{ }

/************************************************************************/
/*									*/
/*  Get information part 2						*/
/*									*/
/************************************************************************/

static uLong cdfs_getinfo2 (OZ_VDFS_Iopex *iopex)

{
  char *buff, *p, namebuf[FILENAME_MAX];
  Directoryrecord *dirrec;
  OZ_VDFS_Chnex *chnex;
  OZ_VDFS_File *dirfile, *file;
  OZ_VDFS_Filex *filex;
  OZ_VDFS_Volex *volex;
  uLong i, j, l, size, sts, version;
  union { Directoryrecord contents;
          uByte padding[256];
        } dirrec256;

  chnex = iopex -> chnex;
  file  = chnex -> file;

  /* Get the complete filespec by looking back through the dirid links */

  size = iopex -> u.getinfo2.p.filnamsize;							/* get size of user supplied filename buffer */
  buff = iopex -> u.getinfo2.p.filnambuff;							/* get its address */

  i = size;
  filex = file -> filex;

  for (j = 0; j < FIDLEVEL_MAX; j ++) {

    /* Stop if hit root directory pointer */

    if (memcmp (filex -> fileid.fidlvl + j, rootfid.fidlvl + 0, sizeof filex -> fileid.fidlvl[j]) == 0) break;

    /* Otherwise, get directory record */

    if (j == 0) dirrec = &(filex -> directoryrecord);
    else {
      l = BLOCKSIZE - filex -> fileid.fidlvl[j].ofs;
      if (l > sizeof dirrec256) l = sizeof dirrec256;
      sts = oz_dev_vdfs_readlogblock (filex -> fileid.fidlvl[j].lbn, filex -> fileid.fidlvl[j].ofs, l, &dirrec256, iopex);
      if (sts != OZ_SUCCESS) return (sts);
      dirrec = &dirrec256.contents;
    }

    /* Put name string in namebuf */

    version = getfnfromdirrec (dirrec, sizeof namebuf, namebuf);
    if (!IS_DIRECTORY (dirrec)) {
      l = strlen (namebuf);
      oz_sys_sprintf (sizeof namebuf - l, namebuf + l, ";%u", version);
    }

    /* Put name string in front of everything else in there */

    l = strlen (namebuf);
    if (i <= l) {										/* see if there is room for it */
      memcpy (buff, namebuf + l - i, i);							/* if not, copy what of it will fit */
      i = 0;											/* no room left in output buffer */
      break;											/* stop scanning */
    }
    i -= l;											/* enough room, back up output buffer offset */
    memcpy (buff + i, namebuf, l);								/* copy in string */
  }
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

//static void cdfs_fidtoa (const void *fileid, int size, char *buff);
//static int cdfs_atofid (const char *buff, void *fileid);

static uLong cdfs_getinfo3 (OZ_VDFS_Iopex *iopex)

{
  OZ_VDFS_Volex *volex;

  volex = iopex -> devex -> volume -> volex;
  iopex -> u.getinfo3.p.blocksize     = BLOCKSIZE;
  iopex -> u.getinfo3.p.clusterfactor = 1;
  iopex -> u.getinfo3.p.clustersfree  = 0;
  iopex -> u.getinfo3.p.clustertotal  = volex -> privoldes.vd.prim.volumespacesize.fwd;

//  iopex -> u.getinfo3.p.fileidstrsz   = 32;
//  iopex -> u.getinfo3.p.fidtoa        = cdfs_fidtoa;
//  iopex -> u.getinfo3.p.atofid        = cdfs_atofid;

  return (OZ_SUCCESS);
}

//static void cdfs_fidtoa (const void *fileid, int size, char *buff)
//
//{
//  const OZ_VDFS_Fileid *fid;
//
//  fid = fileid;
//  oz_sys_sprintf (size, buff, "(%u.%u.%u)", fid -> fidlvl[0].vol, fid -> fidlvl[0].lbn, fid -> fidlvl[0].ofs);
//}

//static int cdfs_atofid (const char *buff, void *fileid)
//
//{
//  const char *p;
//  int i;
//  OZ_VDFS_Fileid *fid;
//
//  fid = fileid;
//  p = buff;
//  if (*(p ++) != '(') return (-1);
//  fid -> fidlvl[0].vol = oz_hw_atoi (p, &i);
//  p += i;
//  if (*(p ++) != '.') return (-1);
//  fid -> fidlvl[0].lbn = oz_hw_atoi (p, &i);
//  p += i;
//  if (*(p ++) != '.') return (-1);
//  fid -> fidlvl[0].ofs = oz_hw_atoi (p, &i);
//  p += i;
//  if (*(p ++) != ')') return (-1);
//  return (p - buff);
//}

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

static uLong cdfs_init_volume (OZ_VDFS_Devex *devex, OZ_VDFS_Volume *volume, int volnamelen, const char *volname, uLong clusterfactor, uLong secattrsize, const void *secattrbuff, uLong initflags, OZ_VDFS_Iopex *iopex)

{
  return (OZ_WRITELOCKED);
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

static uLong cdfs_mount_volume (OZ_VDFS_Devex *devex, OZ_VDFS_Volume *volume, uLong mountflags, OZ_VDFS_Iopex *iopex)

{
  int i;
  OZ_VDFS_Volex *volex;
  uLong sts;

  if (devex -> blocksize != BLOCKSIZE) {
    oz_dev_vdfs_printk (iopex, "oz_dev_dfs mount: disk blocksize %u must be %u\n", devex -> blocksize, BLOCKSIZE);
    return (OZ_BADBLOCKSIZE);
  }

  /* Allocate volex (volume extension) struct */

  volex = OZ_KNL_PGPMALLOQ (((uByte *)&(volex -> privoldes)) + BLOCKSIZE - (uByte *)volex);
  if (volex == NULL) return (OZ_EXQUOTAPGP);
  memset (volex, 0, ((uByte *)&(volex -> privoldes)) - (uByte *)volex);
  volume -> volex = volex;

  /* Read the homey and validate it */

  sts = oz_dev_vdfs_readlogblock (LSN_PRIVOLDES, 0, BLOCKSIZE, &(volex -> privoldes), iopex);
  if (sts != OZ_SUCCESS) goto rtnerr;

  sts = OZ_BADHOMEBLKVER;
  if (volex -> privoldes.voldestype                       != VOLDESTYPE_PRIM) goto rtnerr;
  if (memcmp (volex -> privoldes.cd001, "CD001", 5)       != 0)               goto rtnerr;
  if (volex -> privoldes.voldesver                        != VOLDESVER_PRIM)  goto rtnerr;
  if (volex -> privoldes.vd.prim.volumesetsize.fwd        != 1)               goto rtnerr;
  if (volex -> privoldes.vd.prim.volumesequencenumber.fwd != 1)               goto rtnerr;
  if (volex -> privoldes.vd.prim.logicalblocksize.fwd     != BLOCKSIZE)       goto rtnerr;

  for (i = sizeof volex -> privoldes.vd.prim.volumeidentifier; i > 0; -- i) if (volex -> privoldes.vd.prim.volumeidentifier[i-1] != ' ') break;
  movc4 (i, volex -> privoldes.vd.prim.volumeidentifier, sizeof volex -> volumelabel, volex -> volumelabel);

  /* Open the root directory file.  This must be the first file opened as it is how we figure out where all other files are. */

  sts = oz_dev_vdfs_open_file (volume, &rootfid,  OZ_SECACCMSK_LOOK, &(volex -> rootdirectory), iopex);
  if (sts != OZ_SUCCESS) goto rtnerr;

  /* Set up various volume parameters */

  volume -> dirblocksize  = BLOCKSIZE;
  volume -> clusterfactor = 1;
  return (OZ_SUCCESS);

rtnerr:
  OZ_KNL_PGPFREE (volex);
  volume -> volex = NULL;
  return (sts);
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

static uLong cdfs_dismount_volume (OZ_VDFS_Devex *devex, OZ_VDFS_Volume *volume, int unload, int shutdown, OZ_VDFS_Iopex *iopex)

{
  OZ_VDFS_Volex *volex;
  uLong n, sts;

  volex = volume -> volex;
  if (volex == NULL) return (OZ_NOTMOUNTED);

  /* Make sure just our internal files are open */

  if (!shutdown) {
    n = 0;
    if (volex -> rootdirectory != NULL) n ++;
    if (volume -> nopenfiles != n) return (OZ_OPENFILESONVOL);
  }

  /* Close all those files - in shutdown mode there may be channels pointing */
  /* to them but having devex -> shutdown set will prevent all access        */

  while (volume -> openfiles != NULL) {
    oz_dev_vdfs_close_file (volume -> openfiles, iopex);
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

static uLong cdfs_get_rootdirid (OZ_VDFS_Devex *devex, OZ_VDFS_Fileid *rootdirid_r)

{
  OZ_VDFS_Volex *volex;

  *rootdirid_r = rootfid;

  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Get pointer to a file's fileid					*/
/*									*/
/************************************************************************/

static const OZ_VDFS_Fileid *cdfs_get_fileid (OZ_VDFS_File *file)

{
  return (&(file -> filex -> fileid));
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

static uLong lookup_file_scan (char *dirpagebuff, OZ_VDFS_Iopex *iopex);

static uLong cdfs_lookup_file (OZ_VDFS_File *dirfile, int namelen, const char *name, OZ_VDFS_Fileid *fileid_r, char *name_r, OZ_VDFS_Iopex *iopex)

{
  OZ_Dbn dirvbn;

  return (lookup_file (dirfile, namelen, name, &dirvbn, fileid_r, name_r, iopex));
}

static uLong lookup_file (OZ_VDFS_File *dirfile, int namelen, const char *name, OZ_Dbn *dirvbn_r, OZ_VDFS_Fileid *fileid_r, char *name_r, OZ_VDFS_Iopex *iopex)

{
  char c, *dirpagebuff, *p;
  int cmp, naml;
  OZ_Dbn nblocks;
  OZ_VDFS_File *file;
  OZ_VDFS_Filex *dirfilex;
  OZ_VDFS_Volex *volex;
  OZ_VDFS_Volume *volume;
  uLong i, sts;

  dirfilex = dirfile -> filex;
  volume   = dirfile -> volume;
  volex    = volume -> volex;

  if (!IS_DIRECTORY (&(dirfilex -> directoryrecord))) return (OZ_FILENOTADIR);

  /* An null name string means looking up the directory itself */

  if (namelen == 0) {
    *fileid_r = dirfilex -> fileid;					/* return the fileid of the directory */
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

  /* Scan directory for the file - We don't make any assumption about the order of the names */

  dirpagebuff = OZ_KNL_PGPMALLOQ (BLOCKSIZE);
  if (dirpagebuff == NULL) return (OZ_EXQUOTAPGP);

  volex -> v.lf.dirfilex = dirfilex;
  volex -> v.lf.fileid_r = fileid_r;
  volex -> v.lf.name_r   = name_r;
  volex -> v.lf.namelen  = naml - 1;

  for (volex -> v.lf.vbn = 1;; volex -> v.lf.vbn ++) {
    sts = cdfs_map_vbn_to_lbn (dirfile, volex -> v.lf.vbn, &nblocks, &(volex -> v.lf.lbn));
    if ((sts == OZ_ENDOFFILE) || ((sts == OZ_SUCCESS) && (nblocks == 0))) sts = OZ_NOSUCHFILE;
    if (sts != OZ_SUCCESS) break;
    sts = oz_dev_vdfs_readlogblock (volex -> v.lf.lbn, 0, BLOCKSIZE, dirpagebuff, iopex);
    if (sts != OZ_SUCCESS) break;
    sts = lookup_file_scan (dirpagebuff, iopex);
    if (sts != OZ_NOSUCHFILE) break;
  }

  OZ_KNL_PGPFREE (dirpagebuff);
  return (sts);
}

/************************************************************************/
/*									*/
/*  This routine actually scans the directory block buffer		*/
/*									*/
/*    Input:								*/
/*									*/
/*	volex -> v.lf.fname = filename we're looking for		*/
/*	volex -> v.lf.fileid_r = where to return found file-id		*/
/*	volex -> v.lf.name_r = where to return resultant filename	*/
/*	volex -> v.lf.namelen = length of fname excluding null		*/
/*	volex -> v.lf.versign = 0: ;n form, -1: ;-n form		*/
/*	volex -> v.lf.version = version number (abs value)		*/
/*	volex -> v.lf.vbn = directory vbn being looked at		*/
/*	volex -> v.lf.lbn = corresponding logical block number		*/
/*									*/
/************************************************************************/

static uLong lookup_file_scan (char *dirpagebuff, OZ_VDFS_Iopex *iopex)

{
  char c, *p;
  Directoryrecord *dirrec;
  OZ_VDFS_Filex *dirfilex;
  OZ_VDFS_Volex *volex;
  uLong dirpageoffs, i, version;

  volex = iopex -> devex -> volume -> volex;

  /* Scan the directory page for correct filename */

  dirpageoffs = 0;
  while (dirpageoffs < BLOCKSIZE) {								// see if reached end-of-bucket
    dirrec = (Directoryrecord *)(dirpagebuff + dirpageoffs);					// point to directory record
    if (dirrec -> directoryrecordlength == 0) break;						// check for end of bucket
    if (dirrec -> directoryrecordlength > BLOCKSIZE - dirpageoffs) {				// check for buffer overflow
      oz_dev_vdfs_printk (iopex, "oz_dev_dfs lookup_file: vbn %u ofs %u record too long\n", volex -> v.lf.vbn, dirpageoffs);
      return (OZ_DIRCORRUPT);
    }
    dirpageoffs += dirrec -> directoryrecordlength;						// advance pointers for next loop
    if (dirrec -> fileidentifierlength == 0) continue;						// skip entry with null name
    if ((dirrec -> fileidentifier[0] == 0) || (dirrec -> fileidentifier[0] == 1)) continue;	// skip self and parent entries

    version = getfnfromdirrec (dirrec, sizeof volex -> v.lf.lastname, volex -> v.lf.lastname);	// get filename from dirrec

    if (strcmp (volex -> v.lf.fname, volex -> v.lf.lastname) != 0) continue;			// compare the names

    /* Name matches, find matching version                        */
    /* omitted, ;, ;0, ;-0 all refer to the very newest           */
    /* ;n means that exact version                                */
    /* ;-n means next to n'th newest, eg, ;-1 means second newest */

    if (volex -> v.lf.version == 0) goto found_it;				// finding ;-n form, and we've counted enough of them
    if (volex -> v.lf.versign < 0) volex -> v.lf.version --;			// skipping over one more for the ;-n form
    else if (version == volex -> v.lf.version) goto found_it;			// maybe we found an exact match for ;n form
  }

  return (OZ_NOSUCHFILE);

  /* Correct entry has been found */

found_it:
  volex -> v.lf.fileid_r -> fidlvl[0].vol = 1;
  volex -> v.lf.fileid_r -> fidlvl[0].pad = 1;
  volex -> v.lf.fileid_r -> fidlvl[0].lbn = volex -> v.lf.lbn;
  volex -> v.lf.fileid_r -> fidlvl[0].ofs = ((char *)dirrec) - dirpagebuff;
  dirfilex = volex -> v.lf.dirfilex;
  memcpy (volex -> v.lf.fileid_r -> fidlvl + 1, 
          dirfilex -> fileid.fidlvl + 0, 
          (sizeof dirfilex -> fileid.fidlvl) - (sizeof dirfilex -> fileid.fidlvl[0]));

  if (volex -> v.lf.name_r != NULL) {					// see if caller wants resultant name string
    memcpy (volex -> v.lf.name_r, volex -> v.lf.fname, volex -> v.lf.namelen); // if so, copy out name w/out version
    if (volex -> v.lf.fname[volex -> v.lf.namelen-1] != '/') {		// see if it is a directory name
      volex -> v.lf.name_r[volex -> v.lf.namelen++] = ';';		// if not, append ;version
      oz_hw_itoa (version, 
                  FILENAME_MAX - volex -> v.lf.namelen, 
                  volex -> v.lf.name_r + volex -> v.lf.namelen);
    } else {
      volex -> v.lf.name_r[volex -> v.lf.namelen] = 0;
    }
  }
  return (OZ_SUCCESS);							// successful
}

/************************************************************************/
/*									*/
/*  Get filename from directory record					*/
/*									*/
/*    Input:								*/
/*									*/
/*	dirrec = points to directory record in block buffer		*/
/*	namesize = size of namebuff					*/
/*	namebuff = where to return filename string without version	*/
/*									*/
/*    Output:								*/
/*									*/
/*	getfnfromdirrec = entry's version number			*/
/*	*namebuff = filled in with filename string (no version)		*/
/*									*/
/************************************************************************/

static uLong getfnfromdirrec (Directoryrecord *dirrec, int namesize, char *namebuff)

{
  char *p;
  int usedup;
  uByte l;
  uLong version;

  /* Look for Rock Ridge extension filename */

  p = dirrec -> fileidentifier + dirrec -> fileidentifierlength;		// point past end of given identifier
  if ((p - (char *)dirrec) & 1) p ++;						// make sure the offset is even
  while (p < ((char *)dirrec) + dirrec -> directoryrecordlength) {		// repeat to end of record
    l = p[2];									// get length of entry
    if (l < 4) break;								// stop if we wouldn't make progress
    if (p + l > ((char *)dirrec) + dirrec -> directoryrecordlength) break;	// stop if runs off end of record
    if ((p[0] == 'N') && (p[1] == 'M') && (l > 5) && (p[3] == 1) && !(p[4] & 1)) { // check for a good filename field
      l -= 5;									// ok, get length of string part
      p += 5;									// point to the name string
      goto foundfn;
    }
    p += l;									// not a good name field, on to next field
  }

  /* Not found, use ISO9660 filename */

  l = dirrec -> fileidentifierlength;
  p = dirrec -> fileidentifier;

  /* Either way, copy name to caller's buffer */
  /* Append a '/' if it is a directory        */

foundfn:
  if (IS_DIRECTORY (dirrec)) namesize --;					// because we add a '/' on the end
  if (l >= namesize) l = namesize - 1;						// make sure name not too long
  memcpy (namebuff, p, l);							// copy to buffer to make complete name
  if (IS_DIRECTORY (dirrec)) namebuff[l++] = '/';				// add the '/' to the end
  namebuff[l] = 0;								// null terminate

  /* Separate version number from filename */

  version = 1;									// use '1' if none found
  p = strchr (namebuff, ';');							// see if version found in there
  if (p != NULL) {
    *(p ++) = 0;								// if so, chop it off filename
    version = oz_hw_atoi (p, &usedup);						// convert to binary
  }
  return (version);
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

static uLong cdfs_enter_file (OZ_VDFS_File *dirfile, const char *dirname, int namelen, const char *name, int newversion, OZ_VDFS_File *file, const OZ_VDFS_Fileid *fileid, char *name_r, OZ_VDFS_Iopex *iopex)

{
  return (OZ_WRITELOCKED);
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

static uLong cdfs_remove_file (OZ_VDFS_File *dirfile, const char *name, char *name_r, OZ_VDFS_Iopex *iopex)

{
  return (OZ_WRITELOCKED);
}

/************************************************************************/
/*									*/
/*  Return parsed filespec string					*/
/*									*/
/************************************************************************/

static void cdfs_returnspec (char *spec, uLong size, char *buff, OZ_FS_Subs *subs)

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

static uLong cdfs_create_file (OZ_VDFS_Volume *volume, int namelen, const char *name, uLong filattrflags, OZ_VDFS_Fileid *dirid, OZ_VDFS_File *file, OZ_VDFS_Fileid **fileid_r, OZ_VDFS_Iopex *iopex)

{
  return (OZ_WRITELOCKED);
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
/*	cdfs_findopenfile = NULL : file is not already open		*/
/*	                    else : pointer to file struct		*/
/*									*/
/************************************************************************/

static OZ_VDFS_File *cdfs_findopenfile (OZ_VDFS_Volume *volume, const OZ_VDFS_Fileid *fileid)

{
  OZ_VDFS_File *file;

  for (file = volume -> openfiles; file != NULL; file = file -> next) {
    if (memcmp (&(file -> filex -> fileid), fileid, sizeof *fileid) == 0) break;
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

static uLong cdfs_open_file (OZ_VDFS_Volume *volume, const OZ_VDFS_Fileid *fileid, OZ_VDFS_File *file, OZ_VDFS_Iopex *iopex)

{
  OZ_VDFS_Filex *filex;
  uLong sts;

  /* Read the header from disk */

  filex = OZ_KNL_PGPMALLOQ (sizeof *filex);
  if (filex == NULL) return (OZ_EXQUOTAPGP);
  file -> filex = filex;

  sts = read_header_block (fileid, filex, iopex);
  if (sts != OZ_SUCCESS) goto rtnerr;

  /* Set up file-id in struct for future reference */

  filex -> fileid = *fileid;

  /* Return the end-of-file pointer and number of allocated blocks */

  file -> allocblocks    = HIBLK (&(filex -> directoryrecord));
  file -> attrlock_efblk = EFBLK (&(filex -> directoryrecord));
  file -> attrlock_efbyt = EFBYT (&(filex -> directoryrecord));

  return (OZ_SUCCESS);

  /* Error, free stuff off and return error status */

rtnerr:
  OZ_KNL_PGPFREE (filex);
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

static uLong cdfs_set_file_attrs (OZ_VDFS_File *file, uLong numitems, const OZ_Itmlst2 *itemlist, OZ_VDFS_Iopex *iopex)

{
  return (OZ_WRITELOCKED);
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

static uLong cdfs_close_file (OZ_VDFS_File *file, OZ_VDFS_Iopex *iopex)

{
  OZ_VDFS_Filex *filex;
  uLong sts;

  filex = file -> filex;

  /* Free off the filex struct(s) */

  while ((filex = file -> filex) != NULL) {
    file -> filex = filex -> next;
    OZ_KNL_PGPFREE (filex);
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

static uLong cdfs_extend_file (OZ_VDFS_File *file, OZ_Dbn nblocks, uLong extflags, OZ_VDFS_Iopex *iopex)

{
  return (OZ_WRITELOCKED);
}

/************************************************************************/
/*									*/
/*  Read header block into the given file struct			*/
/*									*/
/*    Input:								*/
/*									*/
/*	fileid = file-id of the header block				*/
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

static uLong read_header_block (const OZ_VDFS_Fileid *fileid, OZ_VDFS_Filex *filex, OZ_VDFS_Iopex *iopex)

{
  uLong sts;
  OZ_VDFS_Volex *volex;
  OZ_VDFS_Volume *volume;

  volume = iopex -> devex -> volume;
  volex  = volume -> volex;

  /* Clear out fixed portion of header */

  memset (filex, 0, ((uByte *)&(filex -> directoryrecord)) - (uByte *)filex);

  /* Read the file's directory entry - the fileid points to where it is */

  if (memcmp (fileid -> fidlvl + 0, rootfid.fidlvl + 0, sizeof rootfid.fidlvl[0]) == 0) {
    memcpy (&(filex -> directoryrecord), &(volex -> privoldes.vd.prim.rootdirectoryrecord), sizeof volex -> privoldes.vd.prim.rootdirectoryrecord);
  } else {
    sts = BLOCKSIZE - fileid -> fidlvl[0].ofs;
    if (sts > 256) sts = 256;
    sts = oz_dev_vdfs_readlogblock (fileid -> fidlvl[0].lbn, fileid -> fidlvl[0].ofs, sts, &(filex -> directoryrecord), iopex);
    if (sts != OZ_SUCCESS) return (sts);
  }

  /* Validate directory record's contents */

#if 000
  if (filex -> directoryrecord.directoryrecordlength > ????) goto ??
  if (filex -> directoryrecord.
  if (filex -> directoryrecord.
  if (filex -> directoryrecord.
  if (filex -> directoryrecord.

  uByte directoryrecordlength;			// 22
  uByte extendedattributerecordlength;		// 00
  uLong_both_endian locationofextent;		// 1d 00 00 00 00 00 00 1d
  uLong_both_endian datalength;			// 00 10 00 00 00 00 10 00
  Datetime7 recordingdatetime;			// 65 09 18 11 26 15 f0
  uByte fileflags;				// 02
  uByte fileunitsize;				// interleave file unit size (0 if not interleaved)
  uByte interleavegapsize;			// interleave gap size (0 if not interleaved)
  uWord_both_endian volumesequencenumber;	// which volume of the set this file's extent is recorded on
  uByte fileidentifierlength;			// no null terminator
  char fileidentifier[1];			// "AUTORUN.;1", "BOOT.CAT;1"
} Directoryrecord;



  if (cksm != 0) {
    oz_dev_vdfs_printk (iopex, "oz_dev_dfs read_header_block: bad header checksum (%u,%u,%u)  hdrlbn %u\n", fileid -> num, fileid -> rvn, fileid -> seq, hdrlbn);
    oz_knl_dumpmem2 (volex -> homeblock.blocksize, &(filex -> header), 0);
    return (OZ_BADHDRCKSM);
  }
#endif

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

/* This routine is called by oz_dev_vdfs to mark the prime header dirty (it changed a date or eof position, etc) */

static void cdfs_mark_header_dirty (OZ_VDFS_File *dirtyfile)

{ }

/* This routine is called by oz_dev_vdfs to write out all headers that are marked dirty    */
/* The individual filex->headerdirty flags will be set for those headers that need writing */

static uLong cdfs_write_dirty_header (OZ_VDFS_File *dirtyfile, Long alf, OZ_Datebin now, OZ_VDFS_Volume *volume, OZ_VDFS_Iopex *iopex)

{
  return (OZ_WRITELOCKED);
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

static uLong cdfs_writehomeblock (OZ_VDFS_Volume *volume, OZ_VDFS_Iopex *iopex)

{
  return (OZ_WRITELOCKED);
}

/************************************************************************/
/*									*/
/*  Verify volume structures						*/
/*									*/
/************************************************************************/

static uLong cdfs_verify_volume (OZ_VDFS_Iopex *iopex, OZ_VDFS_Devex *devex)

{
  return (OZ_WRITELOCKED);
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

static uLong cdfs_map_vbn_to_lbn (OZ_VDFS_File *file, OZ_Dbn virtblock, OZ_Dbn *nblocks_r, OZ_Dbn *logblock_r)

{
  OZ_VDFS_Filex *filex;

  filex = file -> filex;

  if (virtblock == 0) return (OZ_VBNZERO);
  if (virtblock > HIBLK (&(filex -> directoryrecord))) return (OZ_ENDOFFILE);

  if ((filex -> directoryrecord.fileunitsize != 0) || (filex -> directoryrecord.interleavegapsize != 0)) {
    oz_knl_printk ("oz_dev_cdfs_map_vbn_to_lbn: too stoopid to do interleaved files\n");
    return (OZ_FILEHDRFULL);
  }

  *nblocks_r  = HIBLK (&(filex -> directoryrecord)) + 1 - virtblock;
  *logblock_r = filex -> directoryrecord.locationofextent.fwd + virtblock - 1;
  return (OZ_SUCCESS);
}
