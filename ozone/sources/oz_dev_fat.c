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
/*  It is the fs dependent layer, implementing FAT			*/
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

#define SEPCHR '/'
#define SEPSTR "/"

#define VOLNAME_MAX 12
#define FILENAME_MAX OZ_FS_MAXFNLEN
#define SECATTR_MAX 0

#define LONGNAME_MAX (FILENAME_MAX-1)/13*26

/* Test the directory bit */

#define IS_DIRECTORY(__filex) ((__filex -> fileid.direntlbn == 0) || ((__filex -> header.DIR_Attr & ATTR_DIRECTORY) != 0))

/* Cluster <-> LBN conversions */

#define CLUS_TO_LBN(__cluster) (((__cluster - 2) << volex -> l2clusterfactor) + volex -> FirstDataSector)
#define LBN_TO_CLUS(__lbn) (((__lbn - volex -> FirstDataSector) >> volex -> l2clusterfactor) + 2)

/* Our File-id structure - it indicates where the one-and-only directory entry starts for the file */
/* A completely zeroed structure indicates the root directory                                      */

struct OZ_VDFS_Fileid { uLong direntlbn;	/* lbn of beg of cluster where start of file's directory entry is */
                        uWord direntidx;	/* index of the file's entry in that cluster */
                        uByte direntnum;	/* number of entries in directory (=1:short name, >1:long name) */

						/* to prevent accidental re-use of entry: */
                        uByte crtenth;		/* - create tenths */
                        uWord crttime;		/* - create time */
                        uWord crtdate;		/* - create date */
                      };

/* On-disk directory entries */

#define ATTR_READ_ONLY 0x01
#define ATTR_HIDDEN 0x02
#define ATTR_SYSTEM 0x04
#define ATTR_VOLUME_ID 0x08
#define ATTR_DIRECTORY 0x10
#define ATTR_ARCHIVE 0x20
#define ATTR_LONG_NAME (ATTR_READ_ONLY | ATTR_HIDDEN | ATTR_SYSTEM | ATTR_VOLUME_ID)
#define ATTR_LONG_NAME_MASK (ATTR_READ_ONLY | ATTR_HIDDEN | ATTR_SYSTEM | ATTR_VOLUME_ID | ATTR_DIRECTORY | ATTR_ARCHIVE)

static const uByte badnamechars[] = { 0x22, 0x2A, 0x2B, 0x2C, 0x2E, 0x2F, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x5B, 0x5C, 0x5D, 0x7C };

#pragma pack (1)
typedef struct { uByte DIR_Name[11];		// 8.3 name
                 uByte DIR_Attr;		// ATTR_* attribute bits
                 uByte DIR_NTRes;		// reserved for NT (ignored)
                 uByte DIR_CrtTimeTenth;	// create time tenths (0..199)
                 uWord DIR_CrtTime;		// create time
                 uWord DIR_CrtDate;		// create date
                 uWord DIR_LstAccDate;		// last access date
                 uWord DIR_FstClusHI;		// first cluster (31..16)
                 uWord DIR_WrtTime;		// last write time
                 uWord DIR_WrtDate;		// last write date
                 uWord DIR_FstClusLO;		// first cluster (15..00)
                 uLong DIR_FileSize;		// file size in bytes
               } Sdirentry;

typedef struct { uByte LDIR_Ord;		// order of the entry
                 uByte LDIR_Name1[10];		// first sub-segment of name
                 uByte LDIR_Attr;		// attributes = ATTR_LONG_NAME
                 uByte LDIR_Type;		// = 0
                 uByte LDIR_Chksum;		// checksum of corresponding short entry
                 uByte LDIR_Name2[12];		// second sub-segment of name
                 uWord LDIR_FstClusLO;		// = 0
                 uByte LDIR_Name3[4];		// third sub-segment of name
               } Ldirentry;
#pragma nopack

typedef union { Sdirentry s;
                Ldirentry l;
              } Direntry;

/* On-disk home block = Same as the boot block */

#pragma pack (1)
typedef struct { char BS_jmpBoot[3];			// 0xEB or 0xE9 followed by jump offset
                 char BS_OEMName[8];			// operating system
                 uWord BPB_BytesPerSec;			// bytes per sector, 512, 1024, 2048 or 4096
                 uByte BPB_SecPerClus;			// sectors per cluster, 1, 2, 4, 8, 16, 32, 64, or 128
							// BUT: BPB_BytesPerSec * BPB_SecPerClus must be .le. 32768
                 uWord BPB_ResvdSecCnt;			// reserved sector count at beginning of volume
							// - must be non-zero
							// - for FAT12, FAT16 : must be =1
                 uByte BPB_NumFATs;			// number of copies of the FAT structures
							// - always =2
                 uWord BPB_RootEntCnt;			// FAT32: always =0
							// FAT12,16: number of 32-byte root directory entries
							//           should always be on a sector boundary
							//           recommended value =512
                 uWord BPB_TotSec16;			// FAT32: always =0
							// FAT12,16: 16-bit total sector count
                 uByte BPB_Media;			// media type: 0xF8=fixed; 0xF0=removable
                 uWord BPB_FATSz16;			// FAT32: always =0
							// FAT12,16: sectors occupied by one FAT
                 uWord BPB_SecPerTrk;			// sectors per track
                 uWord BPB_NumHeads;			// number of heads
                 uLong BPB_HiddSec;			// number of hidden sectors preceding the partition (usually 0)
                 uLong BPB_TotSec32;			// used when BPB_TotSec16 is zero
                 union { struct { uByte BS_DrvNum;	// drive number for int 0x13 calls
                                  uByte BS_Reserved1;	// fill, value is zeroes
                                  uByte BS_BootSig;	// extended boot signature (0x29)
                                  uLong BS_VolID;	// volume serial number (created from date/time)
                                  char BS_VolLab[11];	// volume label (space padded)
                                  char BS_FilSysType[8]; // "FAT12", "FAT16" or "FAT" (space padded)
                                } s1216;
                         struct { uLong BPB_FATSz32;	// number of sectors occupied by ONE FAT (BPB_FATSz16 must be 0)
                                  uWord BPB_ExtFlags;	// <3:0> zero-based number of active FAT
							// <7> : 0 = the FAT is mirrored at runtime into all FATs
							//       1 = only the <3:0> FAT is active
                                  uWord BPB_FSVer;	// MAJOR:MINOR revision number (0:0)
                                  uLong BPB_RootClus;	// cluster number of the first cluster of the root directory (usually 2)
                                  uWord BPB_FSInfo;	// sector number of FSINFO structure in the reserved area (usually 1)
                                  uWord BPB_BkBootSec;	// if non-zero, sector number in reserved area of copy of boot record (usually 6)
                                  char BPB_Reserved[12];
                                  uByte BS_DrvNum;	// drive number for int 0x13
                                  uByte BS_Reserved1;

                                  uByte BS_BootSig;	// extended boot signature (0x29)
                                  uLong BS_VolID;	// volume serial number (created from date/time)
                                  char BS_VolLab[11];	// volume label (space padded)
                                  char BS_FilSysType[8]; // "FAT12", "FAT16" or "FAT" (space padded)
                                } s32;
                       } f;
               } Bpb;

typedef struct { uLong FSI_LeadSig;			// 0x41615252 signature
                 char FSI_Reserved1[480];		// filler, value is zeroes
                 uLong FSI_StrucSig;			// 0x61417272 signature
                 uLong FSI_Free_Count;			// free cluster count (0xFFFFFFFF=unknown)
                 uLong FSI_Nxt_Free;			// where to look for next free cluster
                 char FSI_Reserved2[12];		// filler, value is zeroes
                 uLong FSI_TrailSig;			// 0xAA550000 signature
               } FSInfo;
#pragma nopack

/* In-memory volume extension info */

typedef enum { FATTYPE_12, FATTYPE_16, FATTYPE_32 } Fattype;

struct OZ_VDFS_Volex { Fattype fattype;			// size (in bits) of entries in FAT
                       OZ_VDFS_File *rootdirectory;	// root directory file
                       uLong RootDirSectors;		// number of sectors occupied by root directory (FAT-12,16 only)
                       uLong FirstDataSector;		// start of the data region
                       Direntry *dirblockbuff;		// address of dirblockbuff (one cluster in size)
                       uLong fatsecurlb;		// current fat sector lbn
                       OZ_Mempage fatphypage;		// current fat physical page (if using cache)
                       uByte *fatsecbuff;		// current fat sector buffer (if no cache)
                       uLong dataclusters;		// number of data clusters, starting with [2]
                       uLong eoc;			// 'end-of-chain' value used by the volume
                       uLong TotSec;			// total sectors on volume
                       uLong FATSz;			// number of sectors occupied by single copy of FAT
                       uLong l2clusterfactor;		// log2 (BPB_SecPerClus)
                       uLong blocksperpage;		// number of blocks per memory page
                       uLong fatloidx, fathiidx;	// low/high fat indicies
                       char volname[12];		// null-terminated volname string
                       Bpb bpb;				// homeblock
                       FSInfo fsi;			// filesystem info block (FAT-32 only)
                     };

/* In-memory file extension info */

struct OZ_VDFS_Filex { OZ_VDFS_Fileid fileid;		// the file's id
                       int markedfordel;		// set to delete file when closed
                       OZ_Dbn headerlbn;		// header's lbn = lbn of short directory entry
                       uLong headerofs;			// header's ofs = offset of short directory entry
                       Sdirentry header;		// header contents = short directory entry contents
                     };


/* Vector routines */

static int fat_is_directory (OZ_VDFS_File *file);
static int fat_fis_writethru (OZ_VDFS_File *file, OZ_Dbn virtblock, uLong blockoffs, uLong size, const void *buff);
static int fat_vis_writethru (OZ_VDFS_Volume *volume);
static const char *fat_get_volname (OZ_VDFS_Volume *volume);
static uLong fat_getinfo1 (OZ_VDFS_Iopex *iopex);
static void fat_wildscan_continue (OZ_VDFS_Chnex *chnex);
static void fat_wildscan_terminate (OZ_VDFS_Chnex *chnex);
static uLong fat_getinfo2 (OZ_VDFS_Iopex *iopex);
static uLong fat_getinfo3 (OZ_VDFS_Iopex *iopex);
static uLong fat_init_volume (OZ_VDFS_Devex *devex, OZ_VDFS_Volume *volume, int volnamelen, const char *volname, uLong clusterfactor, uLong secattrsize, const void *secattrbuff, uLong initflags, OZ_VDFS_Iopex *iopex);
static uLong fat_mount_volume (OZ_VDFS_Devex *devex, OZ_VDFS_Volume *volume, uLong mountflags, OZ_VDFS_Iopex *iopex);
static uLong fat_dismount_volume (OZ_VDFS_Devex *devex, OZ_VDFS_Volume *volume, int unload, int shutdown, OZ_VDFS_Iopex *iopex);
static uLong fat_verify_volume (OZ_VDFS_Iopex *iopex, OZ_VDFS_Devex *devex);
static uLong fat_get_rootdirid (OZ_VDFS_Devex *devex, OZ_VDFS_Fileid *rootdirid_r);
static const OZ_VDFS_Fileid *fat_get_fileid (OZ_VDFS_File *file);
static uLong fat_lookup_file (OZ_VDFS_File *dirfile, int namelen, const char *name, OZ_VDFS_Fileid *fileid_r, char *name_r, OZ_VDFS_Iopex *iopex);
static uLong fat_enter_file (OZ_VDFS_File *dirfile, const char *dirname, int namelen, const char *name, int newversion, OZ_VDFS_File *file, const OZ_VDFS_Fileid *fileid, char *name_r, OZ_VDFS_Iopex *iopex);
static uLong fat_remove_file (OZ_VDFS_File *dirfile, const char *name, char *name_r, OZ_VDFS_Iopex *iopex);
static void fat_returnspec (char *spec, uLong size, char *buff, OZ_FS_Subs *subs);
static uLong fat_create_file (OZ_VDFS_Volume *volume, int namelen, const char *name, uLong filattrflags, OZ_VDFS_Fileid *dirid, OZ_VDFS_File *file, OZ_VDFS_Fileid **fileid_r, OZ_VDFS_Iopex *iopex);
static OZ_VDFS_File *fat_findopenfile (OZ_VDFS_Volume *volume, const OZ_VDFS_Fileid *fileid);
static uLong fat_open_file (OZ_VDFS_Volume *volume, const OZ_VDFS_Fileid *fileid, OZ_VDFS_File *file, OZ_VDFS_Iopex *iopex);
static uLong fat_set_file_attrs (OZ_VDFS_File *file, uLong numitems, const OZ_Itmlst2 *itemlist, OZ_VDFS_Iopex *iopex);
static uLong fat_close_file (OZ_VDFS_File *file, OZ_VDFS_Iopex *iopex);
static uLong fat_extend_file (OZ_VDFS_File *file, OZ_Dbn nblocks, uLong extflags, OZ_VDFS_Iopex *iopex);
static uLong fat_write_dirty_header (OZ_VDFS_File *dirtyfile, Long alf, OZ_Datebin now, OZ_VDFS_Volume *volume, OZ_VDFS_Iopex *iopex);
static uLong fat_writehomeblock (OZ_VDFS_Volume *volume, OZ_VDFS_Iopex *iopex);
static uLong fat_map_vbn_to_lbn (OZ_VDFS_File *file, OZ_Dbn virtblock, OZ_Dbn *nblocks_r, OZ_Dbn *logblock_r);
static void fat_mark_header_dirty (OZ_VDFS_File *dirtyfile);

/* Internal routines */

static int dirisnotempty (OZ_VDFS_File *dirfile, OZ_VDFS_Iopex *iopex);
static uLong findfreecluster (uLong lastcluster, uLong *nextcluster, OZ_VDFS_Iopex *iopex);
static uLong extend_file (OZ_VDFS_File *file, OZ_Dbn nblocks, uLong extflags, OZ_VDFS_Iopex *iopex);
static uLong delete_header (OZ_VDFS_File *file, OZ_VDFS_Iopex *iopex);
static uLong read_header (const OZ_VDFS_Fileid *fileid, OZ_VDFS_Filex *filex, OZ_VDFS_Iopex *iopex);
static int validate_header (Sdirentry *header, OZ_VDFS_Iopex *iopex);
static int validirbuf (uLong totalsize, Direntry *dirbuffer, OZ_VDFS_Iopex *iopex);
static uLong dirlbn_init (OZ_VDFS_Filex *dirfilex, OZ_VDFS_Volex *volex);
static uLong dirlbn_next (uLong dirlbn, OZ_VDFS_Iopex *iopex);
static uLong getfatentrych (uLong cluster, uLong *fatentry, OZ_VDFS_Iopex *iopex);
static uLong getfatentry (uLong cluster, uLong *fatentry, OZ_VDFS_Iopex *iopex);
static uLong putfatentry (uLong fatentry, uLong cluster, OZ_VDFS_Iopex *iopex);
static uLong readfat (int code, uLong byteoffset, uLong *data_r, OZ_VDFS_Iopex *iopex);
static uLong writefat (uLong data, int code, uLong byteoffset, OZ_VDFS_Iopex *iopex);
static void validate_volume (OZ_VDFS_Volume *volume, int line);
static void validate_file (OZ_VDFS_File *file, OZ_VDFS_Volume *volume, int linef, int linev);
static uWord oz_to_fat_date (OZ_Datebin datebin);
static uWord oz_to_fat_time (OZ_Datebin datebin);
static OZ_Datebin fat_dt_to_oz (uWord fatdate, uWord fattime, uByte fatenth);
static int makeshortname (int namelen, const char *name, uByte *shortname, uByte *dirattr_r);
static int makelongname (int namelen, const char *name, uByte *longnamebuf, uByte *shortname, uByte *dirattr_r);
static int matchlongname (Ldirentry *ldirentry, int longnamelen, const uByte *longnamebuf);
static void incshortname (uByte *shortname, const uByte *template);
static uByte shortcksm (const uByte *shortname);
static int shortcmp (const uByte *s1, const uByte *s2, int len);
static int longcmp (const uByte *s1, const uByte *s2, int len);
static uLong verify_volume (int readonly, uLong blocksize, OZ_Dbn totalblocks, OZ_VDFS_Iopex *iopex);

/************************************************************************/
/*									*/
/*  Boot-time initialization routine					*/
/*									*/
/************************************************************************/

static const OZ_VDFS_Vector vector = { sizeof (OZ_VDFS_Fileid), // file-id's are temporary use only
                                       VOLNAME_MAX, FILENAME_MAX, SECATTR_MAX, 
                                       0, 			// it doesn't do versions

                                       fat_close_file, 		// close a file
                                       fat_create_file, 	// create a new file
                                       fat_dismount_volume, 	// dismount volume
                                       fat_enter_file, 		// enter a new name in a directory
                                       fat_extend_file, 	// extend a file
                                       fat_findopenfile, 	// see if a file is already open
                                       fat_get_rootdirid, 	// get root directory id
                                       fat_get_volname, 	// get volume name
                                       fat_getinfo2, 		// get name of file open on a channel
                                       fat_init_volume, 	// initialize a volume
                                       fat_lookup_file, 	// look up a particular file in a directory
                                       fat_mount_volume, 	// mount volume
                                       fat_open_file, 		// open a file
                                       fat_remove_file, 	// remove name from directory
                                       fat_set_file_attrs, 	// write a file's attributes
                                       fat_write_dirty_header, 	// flush file's header(s) to disk
                                       fat_writehomeblock, 	// flush volume's header to disk
                                       fat_verify_volume, 	// verify volume structures

                                       fat_fis_writethru, 	// see if file is a 'writethru' file
                                       fat_get_fileid, 		// get file id
                                       fat_getinfo1, 		// get info about the file open on channel
                                       fat_getinfo3, 		// get info about the volume
                                       fat_is_directory, 	// see if file is a directory
                                       fat_map_vbn_to_lbn, 	// map a file's vbn to equivalent lbn
                                       fat_mark_header_dirty, 	// mark (prime) header dirty
                                       fat_returnspec, 		// return filespec string/substrings
                                       fat_vis_writethru, 	// see if volume is a 'writethru' volume
                                       fat_wildscan_continue, 	// scan directory block for a particular wildcard match
                                       fat_wildscan_terminate }; // terminate wildscan scan

void oz_dev_fat_init ()

{
  oz_dev_vdfs_init (OZ_VDFS_VERSION, "oz_fat", &vector);
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
/*	fat_is_directory = 0 : file is not a directory			*/
/*	                   1 : file is a directory			*/
/*									*/
/************************************************************************/

static int fat_is_directory (OZ_VDFS_File *file)

{
  return (IS_DIRECTORY (file -> filex));
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
/*	fat_fis_writethru = 0 : file can use writeback mode		*/
/*	                    1 : file forces writethru mode		*/
/*									*/
/************************************************************************/

static int fat_fis_writethru (OZ_VDFS_File *file, OZ_Dbn virtblock, uLong blockoffs, uLong size, const void *buff)

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
/*	fat_vis_writethru = 0 : volume can use writeback mode		*/
/*	                    1 : volume forces writethru mode		*/
/*									*/
/************************************************************************/

static int fat_vis_writethru (OZ_VDFS_Volume *volume)

{
  return ((volume -> mountflags & OZ_FS_MOUNTFLAG_WRITETHRU) != 0);
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
/*	fat_get_volname = pointer to null-terminated label string	*/
/*									*/
/************************************************************************/

static const char *fat_get_volname (OZ_VDFS_Volume *volume)

{
  return (volume -> volex -> volname);
}

/************************************************************************/
/*									*/
/*  Get information part 1						*/
/*									*/
/************************************************************************/

static uLong fat_getinfo1 (OZ_VDFS_Iopex *iopex)

{
  OZ_VDFS_Filex *filex;

  filex = iopex -> chnex -> file -> filex;

  iopex -> u.getinfo1.p.filattrflags = 0;
  if (filex -> header.DIR_Attr & ATTR_DIRECTORY) iopex -> u.getinfo1.p.filattrflags = OZ_FS_FILATTRFLAG_DIRECTORY;

  iopex -> u.getinfo1.p.create_date  = fat_dt_to_oz (filex -> header.DIR_CrtDate, filex -> header.DIR_CrtTime, filex -> header.DIR_CrtTimeTenth);
  iopex -> u.getinfo1.p.access_date  = fat_dt_to_oz (filex -> header.DIR_LstAccDate, 0, 0);
  iopex -> u.getinfo1.p.change_date  = fat_dt_to_oz (filex -> header.DIR_WrtDate, filex -> header.DIR_WrtTime, 0);;
  iopex -> u.getinfo1.p.modify_date  = fat_dt_to_oz (filex -> header.DIR_WrtDate, filex -> header.DIR_WrtTime, 0);;
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

typedef struct { int longname;
                 OZ_VDFS_Fileid fileid;
                 uByte lncksm;
               } Wildx;

static void fat_wildscan_continue (OZ_VDFS_Chnex *chnex)

{
  Direntry *direntry;
  int i, rc;
  OZ_Dbn nblocks;
  OZ_VDFS_Chnex *dirchnex;
  OZ_VDFS_Devex *devex;
  OZ_VDFS_Iopex *iopex;
  OZ_VDFS_Volex *volex;
  OZ_VDFS_Volume *volume;
  OZ_VDFS_Wildscan *outerwild, *wildscan;
  uLong sts, vl;
  Wildx *wildx;

  iopex    = chnex -> wild_iopex;
  volume   = iopex -> devex -> volume;
  volex    = volume -> volex;
  wildscan = chnex -> wildscan;
  devex    = iopex -> devex;

  /* Make sure we have an extension struct for this context block */

  wildx = wildscan -> wildx;
  if (wildx == NULL) {
    wildx = OZ_KNL_PGPMALLOC (sizeof *wildx);
    memset (wildx, 0, sizeof *wildx);
    wildscan -> wildx = wildx;
  }

  /* Scan block for matching filename entry */

  while (wildscan -> blockoffs < volume -> dirblocksize) {

    /* Point to the entry in question */

    direntry = (Direntry *)(wildscan -> blockbuff + wildscan -> blockoffs);

    /* If long, see if we have the whole name yet */

    if ((direntry -> s.DIR_Attr & ATTR_LONG_NAME_MASK) == ATTR_LONG_NAME) {	// see if long name entry
      if (direntry -> l.LDIR_Ord & 0x40) {					// see if starting entry
        memset (wildscan -> lastname, 0, sizeof wildscan -> lastname);		// ok, reset the name string buffer
        wildx -> longname = 1;							// remember we're doing a long name
        wildx -> lncksm = direntry -> l.LDIR_Chksum;				// save the short entry's checksum
        dirchnex = oz_knl_iochan_ex (wildscan -> iochan);			// save lbn at start of this dir cluster
        sts = fat_map_vbn_to_lbn (dirchnex -> file, wildscan -> blockvbn, &nblocks, &(wildx -> fileid.direntlbn));
        if (sts != OZ_SUCCESS) {
          oz_dev_vdfs_wildscan_iodonex (iopex, sts);
          return;
        }
        if ((nblocks % volex -> bpb.BPB_SecPerClus) != 0) {			// make sure it's at the beg of cluster
          oz_dev_vdfs_printk (iopex, "oz_dev_fat wildscan_continue: %11.11s vbn %u not cluster aligned\n", 
					dirchnex -> file -> filex -> header.DIR_Name, wildscan -> blockvbn);
          oz_dev_vdfs_wildscan_iodonex (iopex, OZ_BUGCHECK);
          return;
        }
        wildx -> fileid.direntidx = wildscan -> blockoffs / sizeof (Direntry);	// save index within the cluster
        wildx -> fileid.direntnum = 1;						// this counts the corresponding short entry
      }
      i = ((direntry -> l.LDIR_Ord & 0x3F) - 1) * 13;				// get index in lastname for the segment we got
      if (!(wildx -> longname) 							// make sure we're doing a long name
       || (direntry -> l.LDIR_Chksum != wildx -> lncksm) 			// make sure it's the same long name
       || (i + 13 >= sizeof wildscan -> lastname - 1)) goto resetlong;		// make sure the name isn't too long
      wildscan -> lastname[i++] = direntry -> l.LDIR_Name1[ 0];			// save the chars of the name
      wildscan -> lastname[i++] = direntry -> l.LDIR_Name1[ 2];
      wildscan -> lastname[i++] = direntry -> l.LDIR_Name1[ 4];
      wildscan -> lastname[i++] = direntry -> l.LDIR_Name1[ 6];
      wildscan -> lastname[i++] = direntry -> l.LDIR_Name1[ 8];
      wildscan -> lastname[i++] = direntry -> l.LDIR_Name2[ 0];
      wildscan -> lastname[i++] = direntry -> l.LDIR_Name2[ 2];
      wildscan -> lastname[i++] = direntry -> l.LDIR_Name2[ 4];
      wildscan -> lastname[i++] = direntry -> l.LDIR_Name2[ 6];
      wildscan -> lastname[i++] = direntry -> l.LDIR_Name2[ 8];
      wildscan -> lastname[i++] = direntry -> l.LDIR_Name2[10];
      wildscan -> lastname[i++] = direntry -> l.LDIR_Name3[ 0];
      wildscan -> lastname[i++] = direntry -> l.LDIR_Name3[ 2];
      wildx -> fileid.direntnum ++;						// one more dir entry consumed by file
      goto nextentry;
    }

    /* Make sure it's not free */

    if (direntry -> s.DIR_Attr & ATTR_VOLUME_ID) goto resetlong;		// a 'volume-id' entry resets any long name
    if (direntry -> s.DIR_Name[0] == 0xE5) goto resetlong;			// a 'free' entry resets any long name
    if (direntry -> s.DIR_Name[0] == 0) {					// an 'eof mark' ends the scan
      oz_dev_vdfs_wildscan_direof (chnex);					// so we're at the eof
      return;
    }

    /* We have a short entry.  If we were doing a long entry, use the long name accumulated in 'lastname'. */

    if (direntry -> s.DIR_Name[0] == '.') goto resetlong;			// skip over the '.' and '..' entries
    if (wildx -> longname && (shortcksm (direntry -> s.DIR_Name) != wildx -> lncksm)) wildx -> longname = 0; // if long, make sure short entry matches
    if (!(wildx -> longname)) {							// see if it's a lone short entry
      memcpy (wildscan -> lastname, direntry -> s.DIR_Name, 8);			// ok, copy the first 8 chars
      for (i = 8; i > 0; -- i) {						// trim the trailing spaces
        if (wildscan -> lastname[i-1] != ' ') break;
      }
      if (direntry -> s.DIR_Name[8] != ' ') {					// see if there is any filetype
        wildscan -> lastname[i++] = '.';					// ok, append .filetype
        wildscan -> lastname[i++] = direntry -> s.DIR_Name[ 8];
        wildscan -> lastname[i++] = direntry -> s.DIR_Name[ 9];
        wildscan -> lastname[i++] = direntry -> s.DIR_Name[10];
        while (wildscan -> lastname[i-1] == ' ') -- i;				// ... then trim trailing spaces again
      }
      wildscan -> lastname[i] = 0;						// anyway, null terminate it
      dirchnex = oz_knl_iochan_ex (wildscan -> iochan);				// save lbn at start of this dir cluster
      sts = fat_map_vbn_to_lbn (dirchnex -> file, wildscan -> blockvbn, &nblocks, &(wildx -> fileid.direntlbn));
      if (sts != OZ_SUCCESS) {
        oz_dev_vdfs_wildscan_iodonex (iopex, sts);
        return;
      }
      if ((nblocks % volex -> bpb.BPB_SecPerClus) != 0) {			// make sure it's at the beg of cluster
        oz_dev_vdfs_printk (iopex, "oz_dev_fat wildscan_continue: vbn %u not cluster aligned\n", wildscan -> blockvbn);
        oz_dev_vdfs_wildscan_iodonex (iopex, OZ_BUGCHECK);
        return;
      }
      wildx -> fileid.direntidx = wildscan -> blockoffs / sizeof (Direntry);	// save index within the cluster
      wildx -> fileid.direntnum = 1;						// it just has the one short entry
    }

    /* Increment to next entry in case we return out */

    wildx -> longname = 0;
    wildscan -> blockoffs += sizeof *direntry;

    /* Finish making up the fileid */

    wildx -> fileid.crtenth = direntry -> s.DIR_CrtTimeTenth;
    wildx -> fileid.crttime = direntry -> s.DIR_CrtTime;
    wildx -> fileid.crtdate = direntry -> s.DIR_CrtDate;

    /* If directory, append the '/' */

    if (direntry -> s.DIR_Attr & ATTR_DIRECTORY) strcat (wildscan -> lastname, SEPSTR);

    /* See if the filename in 'lastname' matches the wildcard spec, and see if we should scan the sub-sirectory */

    rc = oz_dev_vdfs_wildscan_match (wildscan, wildscan -> lastname);

    /* Maybe scan sub-directory.  If we also output this directory, we do it either after or before the directory */
    /* contents (depending on the setting of delaydir), so we let the subdir output stuff handle that.            */

    if ((direntry -> s.DIR_Attr & ATTR_DIRECTORY) && (rc & 2)) {
      if (iopex -> aborted) {								// don't bother if I/O request is aborted
        oz_dev_vdfs_wildscan_iodonex (iopex, OZ_ABORTED);
        return;
      }
      if (strlen (wildscan -> basespec) + strlen (wildscan -> lastname) >= sizeof wildscan -> basespec) { // make sure directory name not too long
        oz_dev_vdfs_wildscan_iodonex (iopex, OZ_FILENAMETOOLONG);
        return;
      }
      for (outerwild = wildscan; outerwild != NULL; outerwild = outerwild -> nextouter) { // don't nest into same directory twice
        dirchnex = oz_knl_iochan_ex (outerwild -> iochan);
        if (memcmp (&(wildx -> fileid), &(dirchnex -> file -> filex -> fileid), sizeof wildx -> fileid) == 0) {
          oz_dev_vdfs_wildscan_iodonex (iopex, OZ_DIRECTORYLOOP);
          return;
        }
      }
      oz_dev_vdfs_wildscan_startsubdir (chnex, wildscan -> lastname, &(wildx -> fileid), rc); // start processing sub-directory
      return;
    }

    /* Return match to caller */

    if (!(rc & 1)) continue;								// don't if name didn't match
    if ((direntry -> s.DIR_Attr & ATTR_DIRECTORY) && !(wildscan -> ver_incldirs)) continue; // maybe we skip directories
    oz_dev_vdfs_wildscan_output (chnex, wildscan -> lastname, 0, &(wildx -> fileid));	// output the file/directory
    return;

    /* Increment offset to directory's next entry */

resetlong:
    wildx -> longname = 0;
nextentry:
    wildscan -> blockoffs += sizeof *direntry;
  }

  /* Reached end of currrent block, start reading a new one */

  wildscan -> blockvbn += volume -> dirblocksize / iopex -> devex -> blocksize;
  oz_dev_vdfs_wildscan_readdir (chnex);
  return;

  /* Something is corrupt about the directory block */

dircorrupt:
  oz_dev_vdfs_wildscan_iodonex (iopex, OZ_DIRCORRUPT);
}

/* Wildscan struct is being terminated - free off wildx struct, if any */

static void fat_wildscan_terminate (OZ_VDFS_Chnex *chnex)

{
  OZ_VDFS_Wildscan *wildscan;
  Wildx *wildx;

  wildscan = chnex -> wildscan;

  wildx = wildscan -> wildx;
  if (wildx != NULL) {
    wildscan -> wildx = NULL;
    OZ_KNL_PGPFREE (wildx);
  }
}

/************************************************************************/
/*									*/
/*  Get information part 2						*/
/*									*/
/************************************************************************/

static uLong fat_getinfo2 (OZ_VDFS_Iopex *iopex)

{
  char *buff, *p;
  OZ_VDFS_Chnex *chnex;
  OZ_VDFS_File *dirfile, *file;
  OZ_VDFS_Filex *filex;
  uLong i, l, size, sts;

#if 111
  oz_dev_vdfs_printk (iopex, "oz_dev_fat getinfo2*: dummied out\n");
  return (OZ_BADIOFUNC);
#else

  chnex = iopex -> chnex;
  file  = chnex -> file;

  /* Get the complete filespec by looking back through the dirid links */

  size = iopex -> u.getinfo2.p.filnamsize;							/* get size of user supplied filename buffer */
  buff = iopex -> u.getinfo2.p.filnambuff;							/* get its address */

  i = size;
  filex = file -> filex;
  while (filex -> fileid.direntlbn != 0) {							/* repeat until we reach the root directory */
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
      oz_dev_vdfs_printk (iopex, "oz_dev_fat: getinfo2: error %u getting directory name\n", sts);
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
#endif
}

/************************************************************************/
/*									*/
/*  Get information part 3 (no file need be open)			*/
/*									*/
/************************************************************************/

static uLong fat_getinfo3 (OZ_VDFS_Iopex *iopex)

{
  const char *unitname;
  OZ_Devunit *devunit;
  OZ_VDFS_Volex *volex;

  volex = iopex -> devex -> volume -> volex;

  iopex -> u.getinfo3.p.blocksize     = volex -> bpb.BPB_BytesPerSec;
  iopex -> u.getinfo3.p.clusterfactor = volex -> bpb.BPB_SecPerClus;
  iopex -> u.getinfo3.p.clustersfree  = volex -> fsi.FSI_Free_Count;
  iopex -> u.getinfo3.p.clustertotal  = volex -> dataclusters;
  iopex -> u.getinfo3.p.fileidstrsz   = 0;
  iopex -> u.getinfo3.p.fidtoa        = NULL;
  iopex -> u.getinfo3.p.atofid        = NULL;

  return (OZ_SUCCESS);
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

static const struct { uLong disksize, clusterfactor; } clusterfactortable[] = {
	      8400,  1, 
	     32680,  2, 
	    262144,  4, 
	  16777216,  8, 
	  33554432, 16, 
	  67108864, 32, 
	0xFFFFFFFF, 64 };

static uLong calcdataclusters (OZ_VDFS_Volex *volex, uLong bitsperfatentry, uLong RsvdSecCnt, uLong RootEntCnt);
static uLong makevolid (void);

static uLong fat_init_volume (OZ_VDFS_Devex *devex, OZ_VDFS_Volume *volume, int volnamelen, const char *volname, uLong clusterfactor, uLong secattrsize, const void *secattrbuff, uLong initflags, OZ_VDFS_Iopex *iopex)

{
  OZ_Datebin now;
  OZ_VDFS_Volex *volex;
  uByte *blockbuff;
  uLong dc12, dc16, dc32, firstrootlbn, i, sts, x;

  if (devex -> blocksize < 512) {			// must be at least 512 to hold boot block
    oz_dev_vdfs_printk (iopex, "oz_dev_fat init_volume: block size %u must be at least 512\n", devex -> blocksize);
    return (OZ_BADBLOCKSIZE);
  }

  volex = OZ_KNL_PGPMALLOQ (devex -> blocksize + sizeof *volex); // alloc a volex used by some routines
  if (volex == NULL) return (OZ_EXQUOTAPGP);
  memset (volex, 0, sizeof *volex);
  blockbuff = (void *)(volex + 1);

  /* This stuff is the same no matter the fattype or clusterfactor */

  volex -> bpb.BS_jmpBoot[0] = 0xEB;
  volex -> bpb.BS_jmpBoot[1] = -2;
  memcpy (volex -> bpb.BS_OEMName, "OZONE   ", 8);
  volex -> TotSec = devex -> totalblocks;		// number of blocks (sectors) on the disk
  volex -> bpb.BPB_BytesPerSec = devex -> blocksize;	// number of bytes per sector
  volex -> bpb.BPB_NumFATs     = 2;			// we always make two copies of the FAT
  volex -> bpb.BPB_Media       = 0xF0;			// always mark it removable
  volnamelen = strnlen (volname, volnamelen);		// null-terminated volume label string
  if (volnamelen > 11) volnamelen = 11;
  memcpy (volex -> volname, volname, volnamelen);
  volex -> blocksperpage = (1 << OZ_HW_L2PAGESIZE) / devex -> blocksize;

  /* If no clusterfactor given, make one up according to the table */

  if (clusterfactor == 0) {
    for (i = 0; clusterfactortable[i].disksize != 0; i ++) {
      if (volex -> TotSec <= clusterfactortable[i].disksize) {
        clusterfactor =  clusterfactortable[i].clusterfactor;
        break;
      }
    }
  }

  /* If given a clusterfator, it must be 1,2,4,8,16,32,64 or 128 */

  volex -> bpb.BPB_SecPerClus = clusterfactor;
  for (volex -> l2clusterfactor = 0; volex -> l2clusterfactor < 8; volex -> l2clusterfactor ++) {
    if ((1 << volex -> l2clusterfactor) == clusterfactor) break;
  }
  if (volex -> l2clusterfactor == 8) {
    OZ_KNL_PGPFREE (volex);
    oz_dev_vdfs_printk (iopex, "oz_dev_fat init_volume: invalid clusterfactor %u\n", clusterfactor);
    return (OZ_BADBLOCKSIZE);
  }

  /* Calculate number of dataclusters for the different FAT types */

calcdc:
  dc12 = calcdataclusters (volex, 12,  1, 512);
  dc16 = calcdataclusters (volex, 16,  1, 512);
  dc32 = calcdataclusters (volex, 32, 32,   0);

  /* If it's small enough to use FAT-12, do that */

  if (dc12 < 0xFF5) goto usefat12;			// FAT-12 has at most 4084 dataclusters

  /* If it's small enough to use FAT-16, do that */

  if (dc16 < 0xFFF5) {					// FAT-16 has at most 65524 dataclusters
    if (dc16 < 0xFF5) {					// ... but must have at least 4085
      if (volex -> l2clusterfactor == 0) {		//   if clusterfactor is 1
        volex -> l2clusterfactor = 1;			//     make it 2 so it will be in FAT-12 range
        volex -> bpb.BPB_SecPerClus = 2;
        dc12 = calcdataclusters (volex, 12,  1, 512);	//     and make sure FAT-12 is ok now
        if (dc12 >= 0xFF5) oz_crash ("oz_dev_fat init_volume: dc12 %u", dc12);
        goto usefat12;
      }
      volex -> l2clusterfactor --;			//   else, try half the clusterfactor
      volex -> bpb.BPB_SecPerClus /= 2;
      dc16 = calcdataclusters (volex, 16,  1, 512);	//   should give us approx twice the clusters
      if ((dc16 < 0xFF5) || (dc16 >= 0xFFF5)) oz_crash ("oz_dev_fat init_volume: dc16 %u", dc16);
    }
    goto usefat16;
  }

  /* Otherwise, use FAT-32 */

  if (dc32 < 0xFFF5) {					// it mustn't look like a FAT-16 though
    if (volex -> l2clusterfactor == 0) {		//   if clusterfactor is 1
      volex -> l2clusterfactor = 1;			//     make it 2 so it will be in FAT-16 range
      volex -> bpb.BPB_SecPerClus = 2;
      dc12 = calcdataclusters (volex, 16,  1, 512);	//     and make sure FAT-16 is ok now
      if (dc16 >= 0xFFF5) oz_crash ("oz_dev_fat init_volume: dc16 %u", dc16);
      goto usefat16;
    }
    volex -> l2clusterfactor --;			//   else, try half the clusterfactor
    volex -> bpb.BPB_SecPerClus /= 2;
    dc32 = calcdataclusters (volex, 32,  32, 0);	//   should give us approx twice the clusters
    if (dc32 < 0xFFF5) oz_crash ("oz_dev_fat init_volume: dc32 %u", dc32);
  }

usefat32:
  volex -> fattype = FATTYPE_32;
  volex -> RootDirSectors = 0;					// root doesn't occupy any sectors before data clusters
  volex -> eoc = 0xFFFFFF8;					// end-of-cluster marker value for FAT-32
  volex -> bpb.BPB_ResvdSecCnt = 32;				// size of area reserved for bpb and fsinfo blocks
  volex -> bpb.BPB_RootEntCnt  = 0;				// root doesn't occupy any sectors before data clusters
  volex -> bpb.f.s32.BPB_RootClus = 2;				// put root directory in very first data cluster

  volex -> dataclusters = dc32;					// number of dataclusters on the volume
  volex -> FATSz = ((dc32 + 2) * 4 + volex -> bpb.BPB_BytesPerSec - 1) / volex -> bpb.BPB_BytesPerSec;

  volex -> FirstDataSector = volex -> bpb.BPB_ResvdSecCnt + (volex -> bpb.BPB_NumFATs * volex -> FATSz);

  if (volex -> bpb.BPB_SecPerClus >= volex -> blocksperpage) {	// if page sized clusters, make sure data is cluster aligned
    x = volex -> FirstDataSector % volex -> blocksperpage;	// ... so page mapper can directly map cache pages
    if (x > 0) {
      if (x < 16) {
        volex -> FirstDataSector     -= x;
        volex -> bpb.BPB_ResvdSecCnt -= x;
      } else {
        volex -> FirstDataSector     += volex -> blocksperpage - x;
        volex -> bpb.BPB_ResvdSecCnt += volex -> blocksperpage - x;
      }
    }
  }

  volex -> bpb.f.s32.BPB_FATSz32 = volex -> FATSz;		// number of sectors for each copy of the FAT
  volex -> bpb.f.s32.BPB_FSInfo  = 1;				// sector (lbn) that the fsinfo gets written to
  volex -> bpb.f.s32.BS_BootSig  = 0x29;
  volex -> bpb.f.s32.BS_VolID    = makevolid ();
  memset (volex -> bpb.f.s32.BS_VolLab, ' ', 11);
  memcpy (volex -> bpb.f.s32.BS_VolLab, volex -> volname, volnamelen);
  memcpy (volex -> bpb.f.s32.BS_FilSysType, "FAT     ", sizeof volex -> bpb.f.s32.BS_FilSysType);

  volex -> fsi.FSI_LeadSig    = 0x41615252;
  volex -> fsi.FSI_StrucSig   = 0x61417272;
  volex -> fsi.FSI_Free_Count = volex -> dataclusters - 1;	// minus 1 for the root directory
  volex -> fsi.FSI_Nxt_Free   = 2;				// let it scan from the beginning
  volex -> fsi.FSI_TrailSig   = 0xAA550000;

  firstrootlbn = volex -> FirstDataSector;

  goto fattypesetup;

usefat16:
  volex -> fattype = FATTYPE_16;
  volex -> eoc = 0xFFF8;
  volex -> dataclusters = dc16;
  volex -> FATSz = ((dc16 + 2) * 2 + volex -> bpb.BPB_BytesPerSec - 1) / volex -> bpb.BPB_BytesPerSec;
  memcpy (volex -> bpb.f.s1216.BS_FilSysType, "FAT16   ", sizeof volex -> bpb.f.s1216.BS_FilSysType);
  goto finish1216;

usefat12:
  volex -> fattype = FATTYPE_12;
  volex -> eoc = 0xFF8;
  volex -> dataclusters = dc12;
  volex -> FATSz = (((dc12 + 2) * 3 + 1) / 2 + volex -> bpb.BPB_BytesPerSec - 1) / volex -> bpb.BPB_BytesPerSec;
  memcpy (volex -> bpb.f.s1216.BS_FilSysType, "FAT12   ", sizeof volex -> bpb.f.s1216.BS_FilSysType);

finish1216:
  volex -> bpb.BPB_FATSz16     = volex -> FATSz;
  volex -> bpb.BPB_ResvdSecCnt = 1;
  volex -> bpb.BPB_RootEntCnt  = 512;
  volex -> RootDirSectors      = volex -> bpb.BPB_RootEntCnt * sizeof (Direntry) / volex -> bpb.BPB_BytesPerSec;
  volex -> FirstDataSector     = volex -> bpb.BPB_ResvdSecCnt 
                               + (volex -> bpb.BPB_NumFATs * volex -> FATSz) 
                               + volex -> RootDirSectors;
  if (((volex -> bpb.BPB_BytesPerSec * volex -> bpb.BPB_SecPerClus) % (1 << OZ_HW_L2PAGESIZE)) == 0) {
    x = volex -> FirstDataSector % volex -> blocksperpage;		// page sized clusters, make sure data is page aligned
    if (x != 0) {
      volex -> bpb.BPB_ResvdSecCnt += (volex -> blocksperpage - x);	// ... by padding reserved area as needed
      volex -> FirstDataSector     += (volex -> blocksperpage - x);
    }
  }
  volex -> bpb.f.s1216.BS_BootSig = 0x29;
  volex -> bpb.f.s1216.BS_VolID   = makevolid ();
  memset (volex -> bpb.f.s1216.BS_VolLab, ' ', 11);
  memcpy (volex -> bpb.f.s1216.BS_VolLab, volex -> volname, volnamelen);

  firstrootlbn = volex -> bpb.BPB_ResvdSecCnt + (volex -> bpb.BPB_NumFATs * volex -> FATSz);

fattypesetup:

  /* Verify that our computations worked */

  sts = OZ_BUGCHECK;

  x = volex -> bpb.BPB_ResvdSecCnt 							// reserved sectors (bpb, fsi)
    + (volex -> bpb.BPB_NumFATs * volex -> FATSz) 					// all the FAT copies
    + (volex -> bpb.BPB_RootEntCnt * sizeof (Direntry) / volex -> bpb.BPB_BytesPerSec);	// root directory (FAT-12,16 only)
  if (x != volex -> FirstDataSector) {
    oz_dev_vdfs_printk (iopex, "oz_dev_fat init_volume: check #1 failed, x %u, firstdatasector %u\n", x, volex -> FirstDataSector);
    goto rtnsts;
  }

  if ((1 << volex -> l2clusterfactor) != volex -> bpb.BPB_SecPerClus) {
    oz_dev_vdfs_printk (iopex, "oz_dev_fat init_volume: check #2 failed, l2clusterfactor %u, secperclus %u\n", volex -> l2clusterfactor, volex -> bpb.BPB_SecPerClus);
    goto rtnsts;
  }

  if (volex -> fattype == FATTYPE_12) {
    if (volex -> dataclusters >= 0xFF5) {
      oz_dev_vdfs_printk (iopex, "oz_dev_fat init_volume: check #3.12 failed, dataclusters %u\n", x, volex -> dataclusters);
      goto rtnsts;
    }
    if ((volex -> dataclusters * 3 + 1) / 2 > volex -> FATSz * volex -> bpb.BPB_BytesPerSec) {
      oz_dev_vdfs_printk (iopex, "oz_dev_fat init_volume: check #4.12 failed, dataclusters %u, fatsz %u\n", volex -> dataclusters, volex -> FATSz);
      goto rtnsts;
    }
  }

  if (volex -> fattype == FATTYPE_16) {
    if (volex -> dataclusters >= 0xFFF5) {
      oz_dev_vdfs_printk (iopex, "oz_dev_fat init_volume: check #3.16 failed, dataclusters %u\n", x, volex -> dataclusters);
      goto rtnsts;
    }
    if (volex -> dataclusters * 2 > volex -> FATSz * volex -> bpb.BPB_BytesPerSec) {
      oz_dev_vdfs_printk (iopex, "oz_dev_fat init_volume: check #4.16 failed, dataclusters %u, fatsz %u\n", volex -> dataclusters, volex -> FATSz);
      goto rtnsts;
    }
  }

  if (volex -> fattype == FATTYPE_32) {
    if (volex -> dataclusters >= 0xFFFFFF5) {
      oz_dev_vdfs_printk (iopex, "oz_dev_fat init_volume: check #3.16 failed, dataclusters %u\n", x, volex -> dataclusters);
      goto rtnsts;
    }
    if (volex -> dataclusters * 4 > volex -> FATSz * volex -> bpb.BPB_BytesPerSec) {
      oz_dev_vdfs_printk (iopex, "oz_dev_fat init_volume: check #4.16 failed, dataclusters %u, fatsz %u\n", volex -> dataclusters, volex -> FATSz);
      goto rtnsts;
    }
  }

  x += volex -> bpb.BPB_SecPerClus * volex -> dataclusters;
  if (x > devex -> totalblocks) {
    oz_dev_vdfs_printk (iopex, "oz_dev_fat init_volume: check #5 failed, x %u, totalblocks %u\n", x, devex -> totalblocks);
    goto rtnsts;
  }
  volex -> TotSec = x;									// revise so mount will get correct answer
  if ((volex -> fattype == FATTYPE_32) || (x > 65535)) volex -> bpb.BPB_TotSec32 = x;	// in case we moved stuff for page alignment
                                                  else volex -> bpb.BPB_TotSec16 = x;

  /* Zero out the reserved, FAT, root directory areas */

  memset (blockbuff, 0, volex -> bpb.BPB_BytesPerSec);					// fill it with zeroes
  x = volex -> FirstDataSector;								// size up to first data sector
  if (volex -> fattype == FATTYPE_32) x += volex -> bpb.BPB_SecPerClus;			// for FAT-32, include first cluster of root directory
  for (i = 0; i < x; i ++) {								// do blocks one at a time
    sts = oz_dev_vdfs_writelogblock (i, 0, volex -> bpb.BPB_BytesPerSec, blockbuff, 0, iopex); // write zeroes to the block
    if (sts != OZ_SUCCESS) {
      oz_dev_vdfs_printk (iopex, "oz_dev_fat init_volume: error %u writing zeroes to block %u\n", sts, i);
      goto rtnsts;
    }
  }

  /* Write BPB and FSInfo */

  memcpy (blockbuff, &(volex -> bpb), sizeof volex -> bpb);				// put bpb at beginning of zeroes
  blockbuff[510] = 0x55;								// put in signature word
  blockbuff[511] = 0xAA;								// it goes here regardless of block size
  sts = oz_dev_vdfs_writelogblock (0, 0, volex -> bpb.BPB_BytesPerSec, blockbuff, 0, iopex);
  if (sts != OZ_SUCCESS) {
    oz_dev_vdfs_printk (iopex, "oz_dev_fat init_volume: error %u writing bpb (block 0)\n", sts);
    goto rtnsts;
  }
  if (volex -> fattype == FATTYPE_32) {
    memset (blockbuff, 0, 512);
    memcpy (blockbuff, &(volex -> fsi), sizeof volex -> fsi);
    sts = oz_dev_vdfs_writelogblock (volex -> bpb.f.s32.BPB_FSInfo, 0, volex -> bpb.BPB_BytesPerSec, blockbuff, 0, iopex);
    if (sts != OZ_SUCCESS) {
      oz_dev_vdfs_printk (iopex, "oz_dev_fat init_volume: error %u writing fsi (block %u)\n", sts, volex -> bpb.f.s32.BPB_FSInfo);
      goto rtnsts;
    }
  }

  /* Write initial cluster blocks */

  memset (blockbuff, 0, volex -> bpb.BPB_BytesPerSec);

  switch (volex -> fattype) {
    case FATTYPE_12: {
      ((uLong *)blockbuff)[0] = 0xF00 | volex -> bpb.BPB_Media | (volex -> eoc << 12);
      break;
    }
    case FATTYPE_16: {
      ((uWord *)blockbuff)[0] = 0xFF00 | volex -> bpb.BPB_Media;
      ((uWord *)blockbuff)[1] = volex -> eoc;
      break;
    }
    case FATTYPE_32: {
      ((uLong *)blockbuff)[0] = 0xFFFFF00 | volex -> bpb.BPB_Media;
      ((uLong *)blockbuff)[1] = volex -> eoc;
      ((uLong *)blockbuff)[volex->bpb.f.s32.BPB_RootClus] = volex -> eoc;
      break;
    }
  }

  x = volex -> bpb.BPB_ResvdSecCnt;
  for (i = 0; i < volex -> bpb.BPB_NumFATs; i ++) {
    sts = oz_dev_vdfs_writelogblock (x, 0, volex -> bpb.BPB_BytesPerSec, blockbuff, 0, iopex);
    if (sts != OZ_SUCCESS) {
      oz_dev_vdfs_printk (iopex, "oz_dev_fat init_volume: error %u writing fat block %u\n", sts, x);
      goto rtnsts;
    }
    x += volex -> FATSz;
    memset (blockbuff, 0, volex -> bpb.BPB_BytesPerSec);
  }

  /* Write Volume ID record to root directory */

  memset (((Sdirentry *)blockbuff)[0].DIR_Name, ' ', sizeof ((Sdirentry *)blockbuff)[0].DIR_Name);
  memcpy (((Sdirentry *)blockbuff)[0].DIR_Name, volname, volnamelen);
  ((Sdirentry *)blockbuff)[0].DIR_Attr         = ATTR_VOLUME_ID;
  now = oz_hw_tod_getnow ();
  ((Sdirentry *)blockbuff)[0].DIR_CrtTimeTenth = (now / (OZ_TIMER_RESOLUTION / 10)) % 200;
  ((Sdirentry *)blockbuff)[0].DIR_CrtTime      = oz_to_fat_time (now);
  ((Sdirentry *)blockbuff)[0].DIR_CrtDate      = oz_to_fat_date (now);
  sts = oz_dev_vdfs_writelogblock (firstrootlbn, 0, volex -> bpb.BPB_BytesPerSec, blockbuff, 0, iopex);
  if (sts != OZ_SUCCESS) {
    oz_dev_vdfs_printk (iopex, "oz_dev_fat init_volume: error %u writing volid block %u\n", sts, firstrootlbn);
    goto rtnsts;
  }

  oz_dev_vdfs_printk (iopex, "oz_dev_fat init_volume: formatted as FAT %u, clusterfactor %u\n", 
	(volex -> fattype == FATTYPE_12) ? 12 : ((volex -> fattype == FATTYPE_16) ? 16 : 32), volex -> bpb.BPB_SecPerClus);
  oz_dev_vdfs_printk (iopex, "oz_dev_fat init_volume:  reserved sectors 0x%X\n", volex -> bpb.BPB_ResvdSecCnt);
  oz_dev_vdfs_printk (iopex, "oz_dev_fat init_volume:        FAT size %u*0x%X\n", volex -> bpb.BPB_NumFATs, volex -> FATSz);
  oz_dev_vdfs_printk (iopex, "oz_dev_fat init_volume:  root dir sectors 0x%X\n", volex -> RootDirSectors);
  oz_dev_vdfs_printk (iopex, "oz_dev_fat init_volume: first data sector 0x%X\n", volex -> FirstDataSector);

rtnsts:
  OZ_KNL_PGPFREE (volex);
  return (sts);
}

/* Calculate the number of dataclusters resulting from a given set of parameters */

static uLong calcdataclusters (OZ_VDFS_Volex *volex, uLong bitsperfatentry, uLong ResvdSecCnt, uLong RootEntCnt)

{
  uLong FATSz, RootDirSectors, x;

  /* Calculate number of sectors for the fixed-size root directory (zero for FAT-32) */

  RootDirSectors = RootEntCnt * sizeof (Direntry) / volex -> bpb.BPB_BytesPerSec;

  /* Calculate number of sectors required for each copy of the FAT */

  x = ((volex -> bpb.BPB_BytesPerSec * 8) << volex -> l2clusterfactor) + volex -> bpb.BPB_NumFATs * bitsperfatentry;
  FATSz = (bitsperfatentry * (volex -> TotSec - RootDirSectors - ResvdSecCnt + (2 << volex -> l2clusterfactor)) + x - 1) / x;

  /* Calculate the corresponding number of dataclusters */

  return ((volex -> TotSec - RootDirSectors - ResvdSecCnt - volex -> bpb.BPB_NumFATs * FATSz) >> volex -> l2clusterfactor);
}

static uLong makevolid (void)

{
  return (oz_hw_tod_getnow () / OZ_TIMER_RESOLUTION);
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

static uLong fat_mount_volume (OZ_VDFS_Devex *devex, OZ_VDFS_Volume *volume, uLong mountflags, OZ_VDFS_Iopex *iopex)

{
  int dirty, i;
  OZ_VDFS_Fileid fileid;
  OZ_VDFS_Volex *volex;
  uLong cluster, datasectors, fatentry, fatentry1, sts;

  /* Allocate volex (volume extension) struct */

  volex = OZ_KNL_PGPMALLOQ (sizeof *volex);
  if (volex == NULL) return (OZ_EXQUOTAPGP);
  memset (volex, 0, sizeof *volex);
  volume -> volex = volex;
  volex -> fatphypage = OZ_PHYPAGE_NULL;

  /* Read the homey and validate what we can */

  sts = oz_dev_vdfs_readlogblock (0, 0, sizeof volex -> bpb, &(volex -> bpb), iopex);
  if (sts != OZ_SUCCESS) goto rtnerr;

  if (volex -> bpb.BPB_BytesPerSec != devex -> blocksize) {
    oz_dev_vdfs_printk (iopex, "oz_dev_fat mount: volume blocksize %u, disk blocksize %u\n", 
	volex -> bpb.BPB_BytesPerSec, devex -> blocksize);
    sts = OZ_BADBLOCKSIZE;
    goto rtnerr;
  }

  volex -> blocksperpage = (1 << OZ_HW_L2PAGESIZE) / devex -> blocksize;

  /* Get LOG2 (clusterfactor) */

  for (i = 8; -- i >= 0;) {
    if (volex -> bpb.BPB_SecPerClus == (1 << i)) break;
  }
  if (i < 0) {
    oz_dev_vdfs_printk (iopex, "oz_dev_fat mount: invalid clusterfactor %u\n", volex -> bpb.BPB_SecPerClus);
    sts = OZ_BADHOMEBLKVER;
    goto rtnerr;
  }
  volex -> l2clusterfactor = i;

  /* Compute number of sectors reserved for the root directory */
  /* FAT-12 and FAT-16 only, FAT-32 will come up with zero     */

  volex -> RootDirSectors = ((volex -> bpb.BPB_RootEntCnt * sizeof (Direntry)) + (volex -> bpb.BPB_BytesPerSec - 1)) / volex -> bpb.BPB_BytesPerSec;

  /* Compute the size of each FAT */

  volex -> FATSz = volex -> bpb.BPB_FATSz16;
  if (volex -> FATSz == 0) volex -> FATSz = volex -> bpb.f.s32.BPB_FATSz32;

  /* Compute the first data sector, it corresponds to cluster 2 */

  volex -> FirstDataSector = volex -> bpb.BPB_ResvdSecCnt + (volex -> bpb.BPB_NumFATs * volex -> FATSz) + volex -> RootDirSectors;

  /* Compute the total sectors in the volume, including reserved, fats, rootdir and data */

  volex -> TotSec = volex -> bpb.BPB_TotSec16;
  if (volex -> TotSec == 0) volex -> TotSec = volex -> bpb.BPB_TotSec32;
  if (volex -> TotSec > devex -> totalblocks) {
    oz_dev_vdfs_printk (iopex, "oz_dev_fat mount: homeblock shows %s total blocks, but only have %u%u\n", volex -> TotSec, devex -> totalblocks);
    sts = OZ_BADHOMEBLKVER;
    goto rtnerr;
  }

  /* Compute the number of clusters used for data */

  datasectors = volex -> TotSec - (volex -> bpb.BPB_ResvdSecCnt + (volex -> bpb.BPB_NumFATs * volex -> FATSz) + volex -> RootDirSectors);
  volex -> dataclusters = datasectors >> volex -> l2clusterfactor;

  /* Compute the type of FAT:  12, 16 or 32-bit */
  /* Also set the 'end-of-chain' cluster value  */

  volex -> fatloidx = 0;
  volex -> fathiidx = volex -> bpb.BPB_NumFATs;
  if (volex -> dataclusters < 0xFF5) {
    volex -> fattype = FATTYPE_12;
    volex -> eoc = 0xFF8;
    memcpy (volex -> volname, volex -> bpb.f.s1216.BS_VolLab, 11);
  } else if (volex -> dataclusters < 0xFFF5) {
    volex -> fattype = FATTYPE_16;
    volex -> eoc = 0xFFF8;
    memcpy (volex -> volname, volex -> bpb.f.s1216.BS_VolLab, 11);
  } else {
    volex -> fattype = FATTYPE_32;
    volex -> eoc = 0xFFFFFF8;
    memcpy (volex -> volname, volex -> bpb.f.s32.BS_VolLab, 11);
    if (volex -> bpb.f.s32.BPB_ExtFlags & 0x80) {
      volex -> fatloidx = volex -> bpb.f.s32.BPB_ExtFlags & 0x0F;
      volex -> fathiidx = volex -> fatloidx + 1;
    }
  }

  /* Save the volume name, null terminated */

  volex -> volname[11] = 0;
  for (i = 11; -- i >= 0;) {
    if (volex -> volname[i] != ' ') break;
    volex -> volname[i] = 0;
  }

  /* Allocate fat sector buffer */

  volex -> fatsecbuff = OZ_KNL_PGPMALLOC (volex -> bpb.BPB_BytesPerSec);
  if (volex -> fatsecbuff == NULL) {
    sts = OZ_EXQUOTAPGP;
    goto rtnerr;
  }

  /* Check for 'dirty' volume, ie, mounted read/write but not dismounted. */

  sts = getfatentry (1, &fatentry1, iopex);
  if (sts != OZ_SUCCESS) goto rtnerr;
  if (volex -> fattype == FATTYPE_16) dirty = ((fatentry1 & 0x8000) == 0);
  else if (volex -> fattype == FATTYPE_32) dirty = ((fatentry1 & 0x8000000) == 0);
  else dirty = 0;

  /* Perform verification if OZ_FS_MOUNTFLAG_VERIFY set */

  if (dirty || (mountflags & OZ_FS_MOUNTFLAG_VERIFY)) {
    if (dirty) oz_dev_vdfs_printk (iopex, "oz_dev_fat: volume %s mounted was not dismounted\n", volex -> volname);
    if (!(mountflags & OZ_FS_MOUNTFLAG_READONLY) || (mountflags & OZ_FS_MOUNTFLAG_VERIFY)) {

      /* Scan and fix the volume */

      sts = verify_volume ((mountflags & OZ_FS_MOUNTFLAG_READONLY) != 0, devex -> blocksize, devex -> totalblocks, iopex);
      if (sts != OZ_SUCCESS) goto rtnerr;

      /* Re-read the homeblock */

      sts = oz_dev_vdfs_readlogblock (0, 0, devex -> blocksize, &(volex -> bpb), iopex);
      if (sts != OZ_SUCCESS) goto rtnerr;
    }
  }

  /* Allocate directory block buffer (used by lookup_file, extend_file and remove_file routines) */

  volume -> dirblocksize  = volex -> bpb.BPB_SecPerClus * volex -> bpb.BPB_BytesPerSec;
  volume -> clusterfactor = volex -> bpb.BPB_SecPerClus;
  volex  -> dirblockbuff  = OZ_KNL_PGPMALLOQ (volume -> dirblocksize);
  if (volex -> dirblockbuff == NULL) {
    oz_dev_vdfs_printk (iopex, "oz_dev_fat mount: no quota for %u byte dirblockbuff\n", volume -> dirblocksize);
    sts = OZ_EXQUOTAPGP;
    goto rtnerr;
  }

  /* Get number of free clusters on volume */

  if (volex -> fattype == FATTYPE_32) {
    sts = oz_dev_vdfs_readlogblock (volex -> bpb.f.s32.BPB_FSInfo, 0, sizeof volex -> fsi, &(volex -> fsi), iopex);
    if (sts != OZ_SUCCESS) {
      oz_dev_vdfs_printk (iopex, "oz_dev_fat mount: error %u reading FSInfo block %u\n", sts, volex -> bpb.f.s32.BPB_FSInfo);
      goto rtnerr;
    }
    if ((volex -> fsi.FSI_LeadSig  != 0x41615252) 
     || (volex -> fsi.FSI_StrucSig != 0x61417272) 
     || (volex -> fsi.FSI_TrailSig != 0xAA550000)) {
      oz_dev_vdfs_printk (iopex, "oz_dev_fat mount: bad FSInfo block signature(s) %8.8X %8.8X %8.8X\n", 
                                 volex -> fsi.FSI_LeadSig, volex -> fsi.FSI_StrucSig , volex -> fsi.FSI_TrailSig);
      sts = OZ_BADHOMEBLKVER;
      goto rtnerr;
    }
  } else {
    memset (&(volex -> fsi), 0, sizeof volex -> fsi);
    volex -> fsi.FSI_Nxt_Free = volex -> dataclusters + 2;
    for (cluster = 2; cluster < volex -> dataclusters + 2; cluster ++) {
      sts = getfatentry (cluster, &fatentry, iopex);
      if (sts != OZ_SUCCESS) goto rtnerr;
      if (fatentry == 0) {
        volex -> fsi.FSI_Free_Count ++;
        if (volex -> fsi.FSI_Nxt_Free == volex -> dataclusters + 2) volex -> fsi.FSI_Nxt_Free = cluster;
      }
    }
  }

  oz_knl_printk ("oz_dev_fat mount_volume*:           fattype %u\n", (volex -> fattype == FATTYPE_12) ? 12 : ((volex -> fattype == FATTYPE_16) ? 16 : 32));
  oz_knl_printk ("oz_dev_fat mount_volume*:  reserved sectors %u\n", volex -> bpb.BPB_ResvdSecCnt);
  oz_knl_printk ("oz_dev_fat mount_volume*:             fatsz %u\n", volex -> FATSz);
  oz_knl_printk ("oz_dev_fat mount_volume*:           numfats %u\n", volex -> bpb.BPB_NumFATs);
  oz_knl_printk ("oz_dev_fat mount_volume*:   rootdiresectors %u\n", volex -> RootDirSectors);
  oz_knl_printk ("oz_dev_fat mount_volume*: first data sector %u\n", volex -> FirstDataSector);
  oz_knl_printk ("oz_dev_fat mount_volume*:     clusterfactor %u\n", volex -> bpb.BPB_SecPerClus);
  oz_knl_printk ("oz_dev_fat mount_volume*:     free clusters %u\n", volex -> fsi.FSI_Free_Count);

  /* Open the root directory file (we only need lookup access, though).  This is done as an optimisation */
  /* so we aren't constantly doing an open/close on the root directory for every lookup_file operation.  */

  memset (&fileid, 0, sizeof fileid);
  sts = oz_dev_vdfs_open_file (volume, &fileid, OZ_SECACCMSK_LOOK, &(volex -> rootdirectory), iopex);
  if (sts != OZ_SUCCESS) goto rtnerr;

  /* Now set the 'volume dirty' bit if we are write enabled */

  if (!(mountflags & OZ_FS_MOUNTFLAG_READONLY)) {
    if (volex -> fattype == FATTYPE_16) fatentry1 &= ~0x8000;
    else if (volex -> fattype == FATTYPE_32) fatentry1 &= ~0x8000000;
    else goto dontmarkdirty;
    sts = putfatentry (fatentry1, 1, iopex);
    if (sts != OZ_SUCCESS) goto rtnerr;
dontmarkdirty:;
  }

  return (OZ_SUCCESS);

rtnerr:
  if (volex -> fatsecbuff   != NULL) OZ_KNL_PGPFREE (volex -> fatsecbuff);
  if (volex -> dirblockbuff != NULL) OZ_KNL_PGPFREE (volex -> dirblockbuff);
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

static uLong fat_dismount_volume (OZ_VDFS_Devex *devex, OZ_VDFS_Volume *volume, int unload, int shutdown, OZ_VDFS_Iopex *iopex)

{
  OZ_VDFS_Volex *volex;
  uLong dirtybit, fatentry, n, sts;

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

  /* If we were write enabled, clear dirty bit */

  if (!(volume -> mountflags & OZ_FS_MOUNTFLAG_READONLY)) {
    if (volex -> fattype == FATTYPE_32) {
      sts = oz_dev_vdfs_writelogblock (volex -> bpb.f.s32.BPB_FSInfo, 0, sizeof volex -> fsi, &(volex -> fsi), 0, iopex);
      if (sts != OZ_SUCCESS) {
        oz_dev_vdfs_printk (iopex, "oz_dev_fat dismount: error %u writing FSInfo block %u\n", sts, volex -> bpb.f.s32.BPB_FSInfo);
      }
      dirtybit = 0x8000000;
    }
    else if (volex -> fattype == FATTYPE_16) dirtybit = 0x8000;
    else goto dontcleardirty;
    sts = getfatentry (1, &fatentry, iopex);
    if (sts == OZ_SUCCESS) {
      fatentry |= dirtybit;
      sts = putfatentry (fatentry, 1, iopex);
    }
    if (sts != OZ_SUCCESS) oz_dev_vdfs_printk (iopex, "oz_dev_fat dismount: error %u clearing dirty bit\n", sts);
dontcleardirty:;
  }

  /* If cache enabled, free off any fat cache page */

  if (volex -> fatphypage != OZ_PHYPAGE_NULL) {
    oz_knl_dcache_pfrel (devex -> dcache, volex -> fatphypage);
    volex -> fatphypage = OZ_PHYPAGE_NULL;
  }

  /* Free off block buffers */

  if (volex -> fatsecbuff   != NULL) OZ_KNL_PGPFREE (volex -> fatsecbuff);
  if (volex -> dirblockbuff != NULL) OZ_KNL_PGPFREE (volex -> dirblockbuff);

  /* Free off the volex struct */

  volume -> volex = NULL;
  OZ_KNL_PGPFREE (volex);

  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Get root directory's fileid						*/
/*									*/
/************************************************************************/

static uLong fat_get_rootdirid (OZ_VDFS_Devex *devex, OZ_VDFS_Fileid *rootdirid_r)

{
  memset (rootdirid_r, 0, sizeof *rootdirid_r);
  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Get pointer to a file's fileid					*/
/*									*/
/************************************************************************/

static const OZ_VDFS_Fileid *fat_get_fileid (OZ_VDFS_File *file)

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
/*	*fileid_r = file-id of found file				*/
/*	*name_r   = name found						*/
/*									*/
/*    Note:								*/
/*									*/
/*	This routine does not do wildcard scanning, it just finds a 	*/
/*	particular file (like for an 'open' type request).		*/
/*									*/
/************************************************************************/

static uLong fat_lookup_file (OZ_VDFS_File *dirfile, int namelen, const char *name, OZ_VDFS_Fileid *fileid_r, char *name_r, OZ_VDFS_Iopex *iopex)

{
  char *p;
  int contigmatch, longnamelen;
  OZ_Dbn dirlbn;
  OZ_VDFS_Filex *dirfilex;
  OZ_VDFS_Volex *volex;
  OZ_VDFS_Volume *volume;
  uByte dirattr, lncksm, longnamebuf[LONGNAME_MAX], shortname[11];
  uLong i, j, sts;

  dirfilex = dirfile -> filex;
  volume   = dirfile -> volume;
  volex    = volume -> volex;

  if (!IS_DIRECTORY (dirfilex)) return (OZ_FILENOTADIR);

  /* An null name string means looking up the directory itself */

  if (namelen == 0) {
    *fileid_r = dirfilex -> fileid;					/* return the fileid of the directory */
    if (name_r != NULL) *name_r = 0;					/* return a null name string */
    return (OZ_SUCCESS);						/* always successful */
  }

  /* Scan directory for the name */

  dirlbn = dirlbn_init (dirfilex, volex);								// get starting lbn
  if (makeshortname (namelen, name, shortname, &dirattr)) {
    while (dirlbn != 0) {
      sts = oz_dev_vdfs_readlogblock (dirlbn, 0, volume -> dirblocksize, volex -> dirblockbuff, iopex);	// read dir cluster
      if (sts != OZ_SUCCESS) goto dirreaderr;
      sts = OZ_DIRCORRUPT;
      if (!validirbuf (volume -> dirblocksize, volex -> dirblockbuff, iopex)) goto dirreaderr;
      for (i = 0; i < volume -> dirblocksize / sizeof *(volex -> dirblockbuff); i ++) {			// scan all entries of cluster
        if (volex -> dirblockbuff[i].s.DIR_Name[0] == 0) goto endofdir;					// check for end
        if (volex -> dirblockbuff[i].s.DIR_Name[0] == 0xE5) continue;					// skip free entry
        if ((volex -> dirblockbuff[i].s.DIR_Attr & (ATTR_DIRECTORY | ATTR_VOLUME_ID)) != dirattr) continue; // skip all volid, and make sure dirattr is ok
        if (shortcmp (volex -> dirblockbuff[i].s.DIR_Name, shortname, sizeof shortname)) {		// check for the name
          fileid_r -> direntlbn = dirlbn;								// matches, save dirent params
          fileid_r -> direntidx = i;
          fileid_r -> direntnum = 1;
          if (name_r != NULL) {
            memcpy (name_r + 0, volex -> dirblockbuff[i].s.DIR_Name + 0,  8);
            for (j = 8; j > 0; -- j) if (name_r[j-1] != ' ') break;
            if (memcmp (volex -> dirblockbuff[i].s.DIR_Name + 8, "   ", 3) != 0) {
              name_r[j++] = '.';
              memcpy (name_r + 8, volex -> dirblockbuff[i].s.DIR_Name + 8, 3);
              for (j += 3; j > 0; -- j) if (name_r[j-1] != ' ') break;
            }
            name_r[j] = 0;
          }
          goto foundit;
        }
      }
      dirlbn = dirlbn_next (dirlbn, iopex);								// on to next cluster
    }
  } else {
    longnamelen = makelongname (namelen, name, longnamebuf, shortname, &dirattr);			// long name, get it (and corresponding short name)
    fileid_r -> direntnum = (longnamelen + 51) / 26;							// get number of dir entries, including short one
    contigmatch = fileid_r -> direntnum + 0x3F;								// the LDIR_Ord we're looking for
    while (dirlbn != 0) {
      sts = oz_dev_vdfs_readlogblock (dirlbn, 0, volume -> dirblocksize, volex -> dirblockbuff, iopex);	// read dir cluster
      if (sts != OZ_SUCCESS) goto dirreaderr;
      sts = OZ_DIRCORRUPT;
      if (!validirbuf (volume -> dirblocksize, volex -> dirblockbuff, iopex)) goto dirreaderr;
      for (i = 0; i < volume -> dirblocksize / sizeof *(volex -> dirblockbuff); i ++) {			// scan all entries of cluster
        if (volex -> dirblockbuff[i].s.DIR_Name[0] == 0) goto endofdir;					// check for end-of-directory
        if (volex -> dirblockbuff[i].s.DIR_Name[0] == 0xE5) goto resetmatch;				// check for single free entry
        if (contigmatch == 0) {										// maybe we're looking for the short entry now
          if (((volex -> dirblockbuff[i].s.DIR_Attr & (ATTR_VOLUME_ID | ATTR_DIRECTORY)) == dirattr) 	// if so, make sure it is correct
           && (shortcksm (volex -> dirblockbuff[i].s.DIR_Name) == lncksm)) goto foundit;
          goto resetmatch;										// if not, start all over
        }
        if ((volex -> dirblockbuff[i].s.DIR_Attr & ATTR_LONG_NAME_MASK) != ATTR_LONG_NAME) goto resetmatch; // ignore all but long name entries
        if (volex -> dirblockbuff[i].l.LDIR_Ord != contigmatch) goto resetmatch;			// make sure we have the right order
        if (contigmatch & 0x40) {									// see if this is first longname entry
          fileid_r -> direntlbn = dirlbn;								// if so, save lbn at start of dir cluster
          fileid_r -> direntidx = i;									// ... and save index within that cluster
          lncksm = volex -> dirblockbuff[i].l.LDIR_Chksum;						// ... and get shortname checksum for all entries
        }
        else if (lncksm != volex -> dirblockbuff[i].l.LDIR_Chksum) goto resetmatch;			// not first, it should have same checksum
        contigmatch &= 0x3F;										// ok, calc sequence for next long name entry
        contigmatch --;
        if (!matchlongname (&(volex -> dirblockbuff[i].l), longnamelen, longnamebuf)) goto resetmatch;	// see if segment name matches
        if (name_r != NULL) {										// ok, maybe return matching portion
          p = (volex -> dirblockbuff[i].l.LDIR_Ord & 0x3F) * 13 + name_r;
          for (j = 0; j < sizeof volex -> dirblockbuff[i].l.LDIR_Name1 / 2; j += 2) {
            if ((*(p ++) = volex -> dirblockbuff[i].l.LDIR_Name1[j]) == 0) goto gotlongname;
          }
          for (j = 0; j < sizeof volex -> dirblockbuff[i].l.LDIR_Name2 / 2; j += 2) {
            if ((*(p ++) = volex -> dirblockbuff[i].l.LDIR_Name2[j]) == 0) goto gotlongname;
          }
          for (j = 0; j < sizeof volex -> dirblockbuff[i].l.LDIR_Name3 / 2; j += 2) {
            if ((*(p ++) = volex -> dirblockbuff[i].l.LDIR_Name3[j]) == 0) goto gotlongname;
          }
gotlongname:;
        }
        continue;
resetmatch:
        contigmatch = fileid_r -> direntnum + 0x3F;							// something bad, reset match counter
      }
      dirlbn = dirlbn_next (dirlbn, iopex);								// on to next cluster
    }
  }

  /* Reached the end of directory without finding the file, return error status */

endofdir:
  return (OZ_NOSUCHFILE);

  /* Found the file's entry, set up rest of fileid and return success status */

foundit:
  fileid_r -> crtdate = volex -> dirblockbuff[i].s.DIR_CrtDate;
  fileid_r -> crttime = volex -> dirblockbuff[i].s.DIR_CrtTime;
  fileid_r -> crtenth = volex -> dirblockbuff[i].s.DIR_CrtTimeTenth;
  return (OZ_SUCCESS);

  /* Error reading directory */

dirreaderr:
  oz_dev_vdfs_printk (iopex, "oz_dev_fat lookup_file: error %u reading directory lbn %u\n", sts, dirlbn);
  return (sts);
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

static uLong fat_enter_file (OZ_VDFS_File *dirfile, const char *dirname, int namelen, const char *name, int newversion, OZ_VDFS_File *file, const OZ_VDFS_Fileid *fileid, char *name_r, OZ_VDFS_Iopex *iopex)

{
  OZ_VDFS_Fileid tempfid;
  uLong sts;

  /* Since the fat_file_create function already entered the file in the directory, we just detect here if it really is there */
  /* If not, this is a funky 'enter' call and is rejected.  Otherwise, we just nop it.                                       */

  oz_knl_printk ("oz_dev_fat enter_file*: %11.11s/%*.*s:\n", dirfile -> filex -> header.DIR_Name, namelen, namelen, name);
  sts = fat_lookup_file (dirfile, namelen, name, &tempfid, name_r, iopex);
  oz_knl_printk ("oz_dev_fat enter_file*: sts %u\n", sts);
  if ((sts == OZ_SUCCESS) && (memcmp (&tempfid, fileid, sizeof tempfid) != 0)) sts = OZ_NOSUCHFILE;
  return (sts);
}

/************************************************************************/
/*									*/
/*  Remove a file from a directory					*/
/*									*/
/*    Input:								*/
/*									*/
/*	dirfile = directory file					*/
/*	name    = name to remove					*/
/*									*/
/*    Output:								*/
/*									*/
/*	remove_file = OZ_SUCCESS : successful				*/
/*	                    else : error status				*/
/*									*/
/************************************************************************/

static uLong fat_remove_file (OZ_VDFS_File *dirfile, const char *name, char *name_r, OZ_VDFS_Iopex *iopex)

{
  OZ_VDFS_File *file;
  OZ_VDFS_Fileid fileid;
  uLong sts;

  /* Look up the file in the directory */

  oz_knl_printk ("oz_dev_fat remove_file*: %11.11s/%s\n", dirfile -> filex -> header.DIR_Name, name);
  sts = fat_lookup_file (dirfile, strlen (name), name, &fileid, name_r, iopex);
  oz_knl_printk ("oz_dev_fat remove_file*: #1 %u\n", sts);
  if (sts != OZ_SUCCESS) return (sts);

  /* Open it, check for conflicts */

  sts = oz_dev_vdfs_open_file (dirfile -> volume, &fileid, OZ_SECACCMSK_WRITE, &file, iopex);
  oz_knl_printk ("oz_dev_fat remove_file*: #2 %u\n", sts);
  if (sts != OZ_SUCCESS) return (sts);

  /* If it is a directory that has stuff in it, don't delete it */

  if (dirisnotempty (file, iopex)) {
    oz_dev_vdfs_close_file (file, iopex);
    return (OZ_DIRNOTEMPTY);
  }

  /* Since our files can only be in one directory, mark it for delete */

  file -> filex -> markedfordel = 1;

  /* Close it, if/when everyone closes it, it gets deleted */

  sts = oz_dev_vdfs_close_file (file, iopex);
  oz_knl_printk ("oz_dev_fat remove_file*: #3 %u\n", sts);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Determine if the given file is a directory, and if so, if it has 	*/
/*  any entries in it							*/
/*									*/
/************************************************************************/

static int dirisnotempty (OZ_VDFS_File *dirfile, OZ_VDFS_Iopex *iopex)

{
  int skip;
  OZ_Dbn logblock;
  OZ_VDFS_Filex *dirfilex;
  OZ_VDFS_Volex *volex;
  OZ_VDFS_Volume *volume;
  uLong i, sts;

  volume = dirfile -> volume;
  volex  = volume -> volex;

  /* If it is not a directory, then it does not have any entries in it */

  dirfilex = dirfile -> filex;
  if (!IS_DIRECTORY (dirfilex)) return (0);

  /* Loop through directory */

  if (dirfilex -> fileid.direntlbn == 0) return (1);					// root directory always has stuff in it
  logblock = dirlbn_init (dirfilex, volex);						// get starting cluster
  skip = 2;										// skip over . and .. entries
  while (logblock != 0) {								// repeat while clusters to check out
    sts = oz_dev_vdfs_readlogblock (logblock, 0, volume -> dirblocksize, volex -> dirblockbuff, iopex); // read cluster
    if (sts != OZ_SUCCESS) {
      oz_dev_vdfs_printk (iopex, "oz_dev_fat dirisnotempty: error %u reading directory\n", sts);
      return (1);									// can't read, assume it has entries
    }
    for (i = 0; i < volume -> dirblocksize / sizeof (Direntry); i ++) {			// loop through all entries in block
      if ((-- skip >= 0) && (volex -> dirblockbuff[i].s.DIR_Attr == ATTR_DIRECTORY) 	// skip . and .. entries
                         && (volex -> dirblockbuff[i].s.DIR_Name[0] == '.')) continue;
      if ((volex -> dirblockbuff[i].s.DIR_Attr & ATTR_LONG_NAME_MASK) == ATTR_LONG_NAME) return (1); // if long name, it has something
      if (volex -> dirblockbuff[i].s.DIR_Name[0] == 0) return (0);			// if dir eof mark, it is empty
      if (volex -> dirblockbuff[i].s.DIR_Name[0] != 0xE5) return (1);			// if not empty mark, it isn't empty
    }
    skip = 0;										// . and .. are only in first cluster
    logblock = dirlbn_next (logblock, iopex);
  }
  return (0);										// all the way through without finding anything
}

/************************************************************************/
/*									*/
/*  Return parsed filespec string					*/
/*									*/
/************************************************************************/

static void fat_returnspec (char *spec, uLong size, char *buff, OZ_FS_Subs *subs)

{
  char *p, *q, *r;

  if (size > 0) movc4 (strlen (spec), spec, size, buff);	/* if buffer given, return the string */

  if (subs != NULL) {						/* see if substring sizes wanted */
    memset (subs, 0, sizeof *subs);
    if (strcmp (spec, SEPSTR) == 0) {				/* make sure we handle this case correctly */
      subs -> namsize = 1;					/* ... it is the root directory as a file */
      return;
    }
    p = strrchr (spec, SEPCHR);					/* find the last / in the spec */
    if (p == NULL) p = spec;					/* if none, point to beginning */
    else if (p[1] != 0) p ++;					/* ... but make sure we're after it */
    else {
      while (p > spec) {					/* last char was last slash, ... */
        if (p[-1] == SEPCHR) break;				/* ... so we consider the last dirname */
        -- p;							/* ... to be the file name */
      }
      subs -> dirsize = p - spec;				/* ... no type or version */
      subs -> namsize = strlen (p);				/* ... even if the name has a . in it */
      return;
    }
    subs -> dirsize = p - spec;					/* directory is all up to that point including last / */
    q = p + strlen (p);						/* point to end of spec */
    subs -> versize = 0;					/* we don't do versions, so version size is zero */
    r = strrchr (p, '.');					/* find the last . in the string */
    if ((r == NULL) || (r > q)) r = q;				/* if none, point at end of string */
    subs -> typsize = q - r;					/* type is starting at . to end of string */
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

static uLong fat_create_file (OZ_VDFS_Volume *volume, int namelen, const char *name, uLong filattrflags, OZ_VDFS_Fileid *dirid, OZ_VDFS_File *file, OZ_VDFS_Fileid **fileid_r, OZ_VDFS_Iopex *iopex)

{
  int i, longnamelen, zeroeod;
  OZ_Datebin now;
  OZ_Dbn direntlbn, dirlbn, dirvbn, nblocks;
  OZ_VDFS_File *dirfile;
  OZ_VDFS_Filex *filex;
  OZ_VDFS_Volex *volex;
  uByte dirattr, lncksm, longnamebuf[LONGNAME_MAX], *longnameseg, shortname[11];
  uLong bytesleftincluster, bytestowrite, cluster, contigfree, contigmatch, direntofs, sts;

  volex = volume -> volex;

  /* Allocate the filex struct */

  filex = OZ_KNL_PGPMALLOQ (sizeof *filex);
  if (filex == NULL) return (OZ_EXQUOTAPGP);
  memset (filex, 0, sizeof *filex);

  /* Open the directory */

  sts = oz_dev_vdfs_open_file (volume, dirid, OZ_SECACCMSK_WRITE, &dirfile, iopex);
  if (sts != OZ_SUCCESS) {
    oz_dev_vdfs_printk (iopex, "oz_dev_fat create_file: error %u opening directory\n", sts);
    goto rtnerr;
  }

  /* Scan directory for a place to put the filename and check for duplicate name */

  dirlbn = dirlbn_init (dirfile -> filex, volex);							// get first cluster in directory
  dirvbn = 1;												// get corresponding virt block number
  if (makeshortname (namelen, name, shortname, &dirattr)) {
    filex -> fileid.direntnum = 1;									// short names require just one entry
    contigfree = 0;
    while (dirlbn != 0) {
      sts = oz_dev_vdfs_readlogblock (dirlbn, 0, volume -> dirblocksize , volex -> dirblockbuff, iopex); // read dir cluster
      if (sts != OZ_SUCCESS) goto dirreaderr;
      sts = OZ_DIRCORRUPT;
      if (!validirbuf (volume -> dirblocksize, volex -> dirblockbuff, iopex)) goto dirreaderr;
      for (i = 0; i < volume -> dirblocksize / sizeof (Direntry); i ++) {				// scan all entries of block
        if (volex -> dirblockbuff[i].s.DIR_Attr & ATTR_VOLUME_ID) continue;				// skip all volid (incl long name) entries
        if ((volex -> dirblockbuff[i].s.DIR_Name[0] == 0) || (volex -> dirblockbuff[i].s.DIR_Name[0] == 0xE5)) { // check for end/free entry
          if (filex -> fileid.direntlbn == 0) {								// ok, there's room here
            filex -> fileid.direntlbn = dirlbn;
            filex -> fileid.direntidx = i;
          }
          if (volex -> dirblockbuff[i].s.DIR_Name[0] == 0) goto endofdir;				// eof, done scanning
          continue;											// free, continue scanning
        }
        if (shortcmp (volex -> dirblockbuff[i].s.DIR_Name, shortname, sizeof shortname)) goto dupname;	// check for duplicate name
      }
      dirlbn  = dirlbn_next (dirlbn, iopex);								// on to next cluster in dir
      dirvbn += volex -> bpb.BPB_SecPerClus;								// calc corresponding vbn
    }
  } else {
    longnamelen = makelongname (namelen, name, longnamebuf, shortname, &dirattr);			// long name, get it (and corresponding short name)
    filex -> fileid.direntnum = (longnamelen + 51) / 26;						// get number of dir entries, including short one
    memset (longnamebuf + longnamelen, 0xFF, (filex -> fileid.direntnum - 1) * 26 - longnamelen);	// FF fill the last entry for create
    contigfree  = 0;											// haven't found any yet
    contigmatch = filex -> fileid.direntnum + 0x3F;							// the LDIR_Ord we're looking for
    while (dirlbn != 0) {
      sts = oz_dev_vdfs_readlogblock (dirlbn, 0, volume -> dirblocksize , volex -> dirblockbuff, iopex); // read dir cluster
      if (sts != OZ_SUCCESS) goto dirreaderr;
      sts = OZ_DIRCORRUPT;
      if (!validirbuf (volume -> dirblocksize, volex -> dirblockbuff, iopex)) goto dirreaderr;
      for (i = 0; i < volume -> dirblocksize / sizeof (Direntry); i ++) {				// scan through the block
        if (volex -> dirblockbuff[i].s.DIR_Name[0] == 0xE5) {						// check for single free entry
          if ((++ contigfree == filex -> fileid.direntnum) && (filex -> fileid.direntlbn == 0)) {	// ok, maybe we have enuf contig for it
            filex -> fileid.direntlbn = dirlbn;
            filex -> fileid.direntidx = i;
          }
          continue;											// ... anyway, we continue scanning
        }
        if (volex -> dirblockbuff[i].s.DIR_Name[0] == 0) {						// check for end-of-directory
          contigfree += volex -> bpb.BPB_BytesPerSec / sizeof *(volex -> dirblockbuff) - i;		// ok, free entries to end-of-block
          if ((contigfree >= filex -> fileid.direntnum) && (filex -> fileid.direntlbn == 0)) {		// maybe we have enuf contig for it
            filex -> fileid.direntlbn = dirlbn;
            filex -> fileid.direntidx = i;
          }
          goto endofdir;										// ... anyway, we reached the end
        }
        contigfree = 0;											// not free, end of any contig free entries
        if ((volex -> dirblockbuff[i].s.DIR_Attr & ATTR_LONG_NAME_MASK) != ATTR_LONG_NAME) {		// ignore all but long name entries
          contigmatch = filex -> fileid.direntnum + 0x3F;
          if (!(volex -> dirblockbuff[i].s.DIR_Attr & ATTR_VOLUME_ID)) {
            incshortname (shortname, volex -> dirblockbuff[i].s.DIR_Name);				// short name, maybe inc around it
          }
          continue;
        }
        if (volex -> dirblockbuff[i].l.LDIR_Ord != contigmatch) {					// make sure we have the right order
          contigmatch = filex -> fileid.direntnum + 0x3F;
          continue;
        }
        contigmatch &= 0x3F;										// ok, calc sequence for next long name entry
        contigmatch --;
        if (!matchlongname (&(volex -> dirblockbuff[i].l), longnamelen, longnamebuf)) {			// see if the segment of name matches
          contigmatch = filex -> fileid.direntnum + 0x3F;						// if not, start comparing again
          continue;
        }
        if (contigmatch == 0) goto dupname;								// if it all matches, duplicate name
      }
      dirlbn  = dirlbn_next (dirlbn, iopex);								// on to next cluster in dir
      dirvbn += volex -> bpb.BPB_SecPerClus;								// calc corresponding vbn
    }
  }
endofdir:

  /* If we didn't find room, extend directory             */
  /* filex -> fileid.direntnum = number of entries needed */
  /* if sts == OZ_ENDOFFILE,                              */
  /*   dirvbn = end-of-file block number + 1              */
  /* if sts == OZ_SUCCESS,                                */
  /*   dirvbn = end-of-file block number                  */
  /*        i = index of eof-marker entry                 */

  zeroeod = 0;
  if (filex -> fileid.direntlbn == 0) {
    if (sts == OZ_ENDOFFILE) i = 0;
    filex -> fileid.direntidx = i;						// save index for new entry
    i += filex -> fileid.direntnum;						// calculate where our new end will be
    nblocks = dirvbn - 1 + (i * sizeof (Direntry) + volex -> bpb.BPB_BytesPerSec - 1) / volex -> bpb.BPB_BytesPerSec; // number of blocks we need
    sts = fat_extend_file (dirfile, nblocks, OZ_FS_EXTFLAG_NOTRUNC, iopex);	// make sure directory is that big
    if (sts != OZ_SUCCESS) goto direxterr;
    sts = fat_map_vbn_to_lbn (dirfile, dirvbn, &nblocks, &dirlbn);		// get corresponding lbn
    if (sts != OZ_SUCCESS) goto direxterr;
    filex -> fileid.direntlbn = dirlbn;						// save lbn for new entry
    zeroeod = 1;								// zero out all after new entry
  }

  /* Fill in filex's header data */

  if (((filattrflags & OZ_FS_FILATTRFLAG_DIRECTORY) != 0) ^ (dirattr != 0)) {	// iff directory, name must end with '/'
    oz_dev_vdfs_printk (iopex, "oz_dev_fat create_file: filattrflag %X and dirattr %X don't match for %*.*s\n", 
                                filattrflags, dirattr, namelen, namelen, name);
    sts = OZ_BADFILENAME;
    goto rtnerr;
  }

  memcpy (filex -> header.DIR_Name, shortname, 11);
  filex -> header.DIR_Attr |= dirattr;
  now = oz_hw_tod_getnow ();
  filex -> header.DIR_CrtTimeTenth = (now / (OZ_TIMER_RESOLUTION / 10)) % 200;
  filex -> header.DIR_WrtTime = filex -> header.DIR_CrtTime = oz_to_fat_time (now);
  filex -> header.DIR_WrtDate = filex -> header.DIR_CrtDate = filex -> header.DIR_LstAccDate = oz_to_fat_date (now);

  /* Finish creating the file-id using the create date/time to make this id unique */

  filex -> fileid.crtenth = filex -> header.DIR_CrtTimeTenth;
  filex -> fileid.crttime = filex -> header.DIR_CrtTime;
  filex -> fileid.crtdate = filex -> header.DIR_CrtDate;

  /* If it's a directory, we have to set up the first block */

  if (filattrflags & OZ_FS_FILATTRFLAG_DIRECTORY) {

    /* Find a free cluster */

    sts = findfreecluster (0, &cluster, iopex);
    if (sts != OZ_SUCCESS) goto diriniterr;

    /* Write an EOC mark on it */

    sts = putfatentry (volex -> eoc, cluster, iopex);
    if (sts != OZ_SUCCESS) goto diriniterr;

    /* Set up the entry for that first cluster */

    filex -> header.DIR_FstClusHI = cluster >> 16;
    filex -> header.DIR_FstClusLO = cluster;

    /* File now has a cluster */

    file -> allocblocks = volex -> bpb.BPB_SecPerClus;
    file -> attrlock_efblk = file -> allocblocks + 1;

    /* Fill in the block buffer for it */

    memset (volex -> dirblockbuff, 0, volume -> dirblocksize);
    volex -> dirblockbuff[0].s = filex -> header;
    volex -> dirblockbuff[1].s = filex -> header;
    memcpy (volex -> dirblockbuff[0].s.DIR_Name, ".          ", 11);
    memcpy (volex -> dirblockbuff[1].s.DIR_Name, "..         ", 11);

    volex -> dirblockbuff[1].s.DIR_FstClusHI = dirfile -> filex -> header.DIR_FstClusHI;
    volex -> dirblockbuff[1].s.DIR_FstClusLO = dirfile -> filex -> header.DIR_FstClusLO;

    /* Write it out */

    dirlbn = CLUS_TO_LBN (cluster);
    sts = oz_dev_vdfs_writelogblock (dirlbn, 0, volume -> dirblocksize, volex -> dirblockbuff, 0, iopex);
    if (sts != OZ_SUCCESS) goto diriniterr;
  }

  /* Write directory entry for created file                                 */
  /* We write 'filex -> fileid.direntnum' entries (including the short one) */
  /* First entry gets written at filex -> fileid.direntlbn:direntidx        */

  /* Format all the entries into dirblockbuff */

  memset (volex -> dirblockbuff, 0, filex -> fileid.direntnum * sizeof volex -> dirblockbuff[0]);

  lncksm = shortcksm (filex -> header.DIR_Name);

  for (i = 0; i < filex -> fileid.direntnum - 1; i ++) {			// loop for each table entry except the last one
    volex -> dirblockbuff[i].s.DIR_Attr = ATTR_LONG_NAME;			// set up the long entry flag
    volex -> dirblockbuff[i].l.LDIR_Ord = filex -> fileid.direntnum - i - 1;	// set up the long entry order number
    if (i == 0) volex -> dirblockbuff[i].l.LDIR_Ord |= 0x40;
    longnameseg = (filex -> fileid.direntnum - i - 2) * 26 + longnamebuf;	// point to segment of name for this entry
    memcpy (volex -> dirblockbuff[i].l.LDIR_Name1, longnameseg +  0, 10);	// store the segment of name in the entry
    memcpy (volex -> dirblockbuff[i].l.LDIR_Name2, longnameseg + 10, 12);
    memcpy (volex -> dirblockbuff[i].l.LDIR_Name3, longnameseg + 22,  4);
    volex -> dirblockbuff[i].l.LDIR_Chksum = lncksm;				// store the short name's checksum number
  }
  volex -> dirblockbuff[i].s = filex -> header;					// put in the short entry at the end of it all

  /* Write them to the directorie */

  direntlbn = filex -> fileid.direntlbn;					// get lbn of cluster to write first entry to
  direntofs = filex -> fileid.direntidx * sizeof (Direntry);			// get offset in that cluster for first entry
  for (i = 0;;) {								// loop through to get them all
    bytesleftincluster = volume -> dirblocksize - direntofs;			// bytes left in cluster from where we want to write
    bytestowrite = (filex -> fileid.direntnum - i) * sizeof (Direntry);		// number of bytes we have left to write
    if (bytestowrite > bytesleftincluster) bytestowrite = bytesleftincluster;	// maybe truncate at end of cluster
    sts = oz_dev_vdfs_writelogblock (direntlbn, direntofs, bytestowrite, volex -> dirblockbuff + i, 0, iopex); // write that much
    if (sts != OZ_SUCCESS) goto dirwriterr;
    direntofs += bytestowrite;							// in case we zero to eod, inc this past what we wrote
    i += bytestowrite / sizeof (Direntry);					// increment to next entry to write
    if (i == filex -> fileid.direntnum) break;					// stop if we're all done
    direntlbn = dirlbn_next (direntlbn, iopex);					// link to next cluster in directory
    sts = OZ_ENDOFFILE;
    if (direntlbn == 0) goto dirwriterr;
    direntofs = 0;								// we start writing at the beg of the cluster
  }
  filex -> headerlbn = direntlbn;						// this is where we just wrote the short entry to
  filex -> headerofs = direntofs - sizeof (Direntry);

  /* Maybe we need to zero to the end of the directory (because we just extended it) */
  /* direntlbn = starting lbn, direntofs = starting offset to zero                   */

  if (zeroeod) {
    memset (volex -> dirblockbuff, 0, volume -> dirblocksize);			// set up a cluster buffer of zeroes
    while (1) {
      bytesleftincluster = volume -> dirblocksize - direntofs;			// bytes left in cluster from where we want to write
      if (bytesleftincluster == 0) {						// see if at end of cluster
        direntlbn = dirlbn_next (direntlbn, iopex);				// if so, see if there is a next one
        if (direntlbn == 0) break;						// ... and stop if end of directory
        direntofs = 0;								// we start writing at the beg of the sector
      }
      sts = oz_dev_vdfs_writelogblock (direntlbn, direntofs, volume -> dirblocksize - direntofs, volex -> dirblockbuff, 0, iopex);
      if (sts != OZ_SUCCESS) {
        oz_dev_vdfs_printk (iopex, "error %u padding directory with zeroes at lbn %u\n", sts, direntlbn);
        goto rtnerr;
      }
      direntofs = volume -> dirblocksize;					// we zeroed to the end of the cluster
    }
  }

  /* Success, return values */

  file -> filex = filex;
  *fileid_r = &(filex -> fileid);
  return (OZ_SUCCESS);

  /* Error returns */

dirreaderr:
  oz_dev_vdfs_printk (iopex, "oz_dev_fat create_file: error %u reading directory block %u\n", sts, dirlbn);
  goto rtnerr;
dupname:
  sts = OZ_FILEALREADYEXISTS;
  goto rtnerr;
direxterr:
  oz_dev_vdfs_printk (iopex, "oz_dev_fat create_file: error %u extending directory to %u blocks\n", sts, nblocks);
  goto rtnerr;
diriniterr:
  oz_dev_vdfs_printk (iopex, "oz_dev_fat create_file: error %u writing initial directory block\n", sts);
  goto rtnerr;
dirwriterr:
  oz_dev_vdfs_printk (iopex, "oz_dev_fat create_file: error %u writing directory block %u\n", sts, direntlbn);
rtnerr:
  OZ_KNL_PGPFREE (filex);
  return (sts);
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
/*	fat_findopenfile = NULL : file is not already open		*/
/*	                   else : pointer to file struct		*/
/*									*/
/************************************************************************/

static OZ_VDFS_File *fat_findopenfile (OZ_VDFS_Volume *volume, const OZ_VDFS_Fileid *fileid)

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
/*	fat_open_file = OZ_SUCCESS : successful completion		*/
/*	                      else : error status			*/
/*	file -> filex = filled in with fs dependent struct		*/
/*	     -> secattr = filled in with file's secattrs		*/
/*	     -> attrlock_efblk,_efbyt = file's end-of-file pointer	*/
/*	     -> allocblocks = number of blocks allocated to file	*/
/*									*/
/************************************************************************/

static uLong fat_open_file (OZ_VDFS_Volume *volume, const OZ_VDFS_Fileid *fileid, OZ_VDFS_File *file, OZ_VDFS_Iopex *iopex)

{
  OZ_VDFS_Filex *filex;
  OZ_VDFS_Volex *volex;
  uLong cluster, sts;

  volex = volume -> volex;

  /* Read the directory entry from the disk */

  filex = OZ_KNL_PGPMALLOQ (sizeof *filex);
  if (filex == NULL) return (OZ_EXQUOTAPGP);
  sts = read_header (fileid, filex, iopex);
  if (sts != OZ_SUCCESS) {
    oz_dev_vdfs_printk (iopex, "oz_dev_fat open_file: error %u reading header block\n", sts);
    goto rtnerr;
  }

  /* No security attributes */

  file -> secattr = NULL;

  /* Count number of blocks allocated to the file */

  file -> allocblocks = 0;
  cluster = (filex -> header.DIR_FstClusHI << 16) + filex -> header.DIR_FstClusLO;
  while ((cluster != 0) && (cluster < volex -> eoc)) {		// repeat until we hit the end-of-chain
    file -> allocblocks += volex -> bpb.BPB_SecPerClus;		// increment file size by one cluster
    if (file -> allocblocks >= volex -> TotSec) break;		// stop in case of loop in cluster chain
    sts = getfatentrych (cluster, &cluster, iopex);		// link to next cluster in chain
    if (sts != OZ_SUCCESS) {
      oz_dev_vdfs_printk (iopex, "oz_dev_fat open_file: error %u counting blocks at cluster %u\n", sts, cluster);
      goto rtnerr;
    }
  }

  /* Return the end-of-file pointer */

  if (filex -> header.DIR_Attr & ATTR_DIRECTORY) {
    file -> attrlock_efblk = file -> allocblocks + 1;
    file -> attrlock_efbyt = 0;
  } else {
    file -> attrlock_efblk = (filex -> header.DIR_FileSize / volex -> bpb.BPB_BytesPerSec) + 1;
    file -> attrlock_efbyt = filex -> header.DIR_FileSize % volex -> bpb.BPB_BytesPerSec;
  }

  file -> filex = filex;
  return (OZ_SUCCESS);

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

static uLong fat_set_file_attrs (OZ_VDFS_File *file, uLong numitems, const OZ_Itmlst2 *itemlist, OZ_VDFS_Iopex *iopex)

{
  OZ_Datebin tmpdate;
  OZ_Dbn tmpdbn;
  OZ_VDFS_Filex *filex;
  OZ_VDFS_Volex *volex;
  uLong i, size, sts, tmpulong, vl;
  void *addr;

  filex = file -> filex;
  volex = iopex -> devex -> volume -> volex;

  /* Scan through given item list */

  for (i = 0; i < numitems; i ++) {
    switch (itemlist[i].item) {

      /* Set file's various dates */

      case OZ_FSATTR_CREATE_DATE: {
        tmpdate = 0;
        size = itemlist[i].size;
        if (size > sizeof tmpdate) size = sizeof tmpdate;
        sts = oz_knl_section_uget (iopex -> procmode, size, itemlist[i].buff, &tmpdate);
        if (sts != OZ_SUCCESS) {
          oz_dev_vdfs_printk (iopex, "oz_dev_fat set_file_attrs: bad item address %p\n", itemlist[i].buff);
          return (sts);
        }
        filex -> fileid.crtenth = filex -> header.DIR_CrtTimeTenth = (tmpdate / (OZ_TIMER_RESOLUTION / 10)) % 200;
        filex -> fileid.crttime = filex -> header.DIR_CrtTime = oz_to_fat_time (tmpdate);
        filex -> fileid.crtdate = filex -> header.DIR_CrtDate = oz_to_fat_date (tmpdate);
        break;
      }

      case OZ_FSATTR_ACCESS_DATE: {
        tmpdate = 0;
        size = itemlist[i].size;
        if (size > sizeof tmpdate) size = sizeof tmpdate;
        sts = oz_knl_section_uget (iopex -> procmode, size, itemlist[i].buff, &tmpdate);
        if (sts != OZ_SUCCESS) {
          oz_dev_vdfs_printk (iopex, "oz_dev_fat set_file_attrs: bad item address %p\n", itemlist[i].buff);
          return (sts);
        }
        filex -> header.DIR_LstAccDate = oz_to_fat_date (tmpdate);
        vl = oz_hw_smplock_wait (&(file -> attrlock_vl));
        file -> attrlock_flags &= ~ OZ_VDFS_ALF_M_ADT;
        oz_hw_smplock_clr (&(file -> attrlock_vl), vl);
        break;
      }

      case OZ_FSATTR_MODIFY_DATE:
      case OZ_FSATTR_CHANGE_DATE: {
        tmpdate = 0;
        size = itemlist[i].size;
        if (size > sizeof tmpdate) size = sizeof tmpdate;
        sts = oz_knl_section_uget (iopex -> procmode, size, itemlist[i].buff, &tmpdate);
        if (sts != OZ_SUCCESS) {
          oz_dev_vdfs_printk (iopex, "oz_dev_fat set_file_attrs: bad item address %p\n", itemlist[i].buff);
          return (sts);
        }
        filex -> header.DIR_WrtTime = oz_to_fat_time (tmpdate);
        filex -> header.DIR_WrtDate = oz_to_fat_date (tmpdate);
        vl = oz_hw_smplock_wait (&(file -> attrlock_vl));
        file -> attrlock_flags &= ~ (OZ_VDFS_ALF_M_CDT | OZ_VDFS_ALF_M_MDT);
        oz_hw_smplock_clr (&(file -> attrlock_vl), vl);
        break;
      }

      /* Set file's end-of-file position */

      case OZ_FSATTR_EOFBLOCK: {
        tmpdbn = 0;
        size = itemlist[i].size;
        if (size > sizeof tmpdbn) size = sizeof tmpdbn;
        sts = oz_knl_section_uget (iopex -> procmode, size, itemlist[i].buff, &tmpdbn);
        if (sts != OZ_SUCCESS) {
          oz_dev_vdfs_printk (iopex, "oz_dev_fat set_file_attrs: bad item address %p\n", itemlist[i].buff);
          return (sts);
        }
        vl = oz_hw_smplock_wait (&(file -> attrlock_vl));
        if (tmpdbn != 0) file -> attrlock_efblk = tmpdbn;
        file -> attrlock_flags |= OZ_VDFS_ALF_M_EOF;
        oz_hw_smplock_clr (&(file -> attrlock_vl), vl);
        break;
      }

      case OZ_FSATTR_EOFBYTE: {
        tmpulong = 0;
        size = itemlist[i].size;
        if (size > sizeof tmpulong) size = sizeof tmpulong;
        sts = oz_knl_section_uget (iopex -> procmode, size, itemlist[i].buff, &tmpulong);
        if (sts != OZ_SUCCESS) {
          oz_dev_vdfs_printk (iopex, "oz_dev_fat set_file_attrs: bad item address %p\n", itemlist[i].buff);
          return (sts);
        }
        vl = oz_hw_smplock_wait (&(file -> attrlock_vl));
        file -> attrlock_efbyt  = tmpulong;
        file -> attrlock_flags |= OZ_VDFS_ALF_M_EOF;
        oz_hw_smplock_clr (&(file -> attrlock_vl), vl);
        break;
      }

      /* Who knows what they want modified */

      default: {
        oz_dev_vdfs_printk (iopex, "oz_dev_fat set_file_attrs: unsupported item code %u\n", itemlist[i].item);
        break;
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

static uLong fat_close_file (OZ_VDFS_File *file, OZ_VDFS_Iopex *iopex)

{
  OZ_VDFS_Filex *filex;
  uLong sts;

  filex = file -> filex;

  /* Delete file if marked for delete */

  sts = OZ_SUCCESS;
  if (filex -> markedfordel) sts = delete_header (file, iopex);

  /* Free off the filex struct */

  file -> filex = NULL;
  OZ_KNL_PGPFREE (filex);
  return (sts);
}

/************************************************************************/
/*									*/
/*  Extend or truncate a file						*/
/*									*/
/*    Input:								*/
/*									*/
/*	file     = file block pointer of file to extended / truncated	*/
/*	nblocks  = new total number of blocks				*/
/*	extflags = OZ_FS_EXTFLAG_NOTRUNC : don't truncate		*/
/*									*/
/*    Output:								*/
/*									*/
/*	extend_file = OZ_SUCCESS : extend was successful		*/
/*	                    else : error status				*/
/*	file -> allocblocks = updated					*/
/*									*/
/************************************************************************/

static uLong fat_extend_file (OZ_VDFS_File *file, OZ_Dbn nblocks, uLong extflags, OZ_VDFS_Iopex *iopex)

{
  uLong sts;

  sts = extend_file (file, nblocks, extflags, iopex);
  oz_dev_vdfs_mark_header_dirty (file);
  return (sts);
}

static uLong extend_file (OZ_VDFS_File *file, OZ_Dbn nblocks, uLong extflags, OZ_VDFS_Iopex *iopex)

{
  OZ_VDFS_Filex *filex;
  OZ_VDFS_Volex *volex;
  OZ_VDFS_Volume *volume;
  uLong cluster, lastcluster, nextcluster, numclusters, sts, sts2;

  filex  = file -> filex;
  volume = file -> volume;
  volex  = volume -> volex;

  /* For FAT-12 and FAT-16, the root directory is what it is */

  if ((volex -> fattype != FATTYPE_32) && (filex -> fileid.direntlbn == 0)) {
    if (nblocks > volex -> bpb.BPB_RootEntCnt * sizeof (Direntry) / volex -> bpb.BPB_BytesPerSec) return (OZ_DISKISFULL);
    return (OZ_SUCCESS);
  }

  /* Get number of clusters wanted for the whole file */

  numclusters = (nblocks + volex -> bpb.BPB_SecPerClus - 1) >> volex -> l2clusterfactor;
  nblocks = numclusters << volex -> l2clusterfactor;

  /* FAT filesizes are limited to 4GB */

  if (nblocks > 0xFFFFFFFF / volex -> bpb.BPB_BytesPerSec) return (OZ_FILETOOBIG);

  /* Directories are limited to 65535 slots */

  if ((filex -> header.DIR_Attr & ATTR_DIRECTORY) 
   && (nblocks > 65535 * sizeof (Direntry) / volex -> bpb.BPB_BytesPerSec)) return (OZ_FILETOOBIG);

  /* Find the spot in existing FAT chain */

  lastcluster = 0;

  if (filex -> fileid.direntlbn == 0) cluster = volex -> bpb.f.s32.BPB_RootClus;
  else cluster = (((uLong)(file -> filex -> header.DIR_FstClusHI)) << 16) + file -> filex -> header.DIR_FstClusLO;

  while (numclusters != 0) {
    if ((cluster == 0) || (cluster >= volex -> eoc)) goto extendfile;
    sts = getfatentrych (cluster, &nextcluster, iopex);
    if (sts != OZ_SUCCESS) return (sts);
    -- numclusters;
    lastcluster = cluster;
    cluster = nextcluster;
  }

  /******************************/
  /* File needs to be truncated */
  /******************************/

truncatefile:
  if (extflags & OZ_FS_EXTFLAG_NOTRUNC) return (OZ_SUCCESS);

  if (lastcluster == 0) {						// write eof mark at truncation point
    file -> filex -> header.DIR_FstClusHI = 0;
    file -> filex -> header.DIR_FstClusLO = 0;
  } else {
    sts = putfatentry (volex -> eoc, lastcluster, iopex);
    if (sts != OZ_SUCCESS) return (sts);
  }
  file -> allocblocks = nblocks;					// update number of blocks in file

  while ((cluster != 0) && (cluster < volex -> eoc)) {			// repeat until all clusters freed off
    sts = getfatentrych (cluster, &nextcluster, iopex);			// read next cluster in file's old chain
    if (sts != OZ_SUCCESS) return (sts);
    sts = putfatentry (0, cluster, iopex);				// write a zero to indicate it is free
    if (sts != OZ_SUCCESS) return (sts);
    volex -> fsi.FSI_Free_Count ++;					// one more free cluster
    if (volex -> fsi.FSI_Nxt_Free > cluster) volex -> fsi.FSI_Nxt_Free = cluster; // maybe it is the lowest free cluster
    cluster = nextcluster;
  }
  return (OZ_SUCCESS);

  /*****************************/
  /* File needs to be extended */
  /*****************************/

extendfile:
  sts = OZ_SUCCESS;
  while (numclusters != 0) {						// repeat while there is more to get
    sts = findfreecluster (lastcluster, &nextcluster, iopex);		// get a free cluster
    if (sts != OZ_SUCCESS) break;
    if (lastcluster == 0) {						// append onto previous cluster
      file -> filex -> header.DIR_FstClusHI = nextcluster >> 16;
      file -> filex -> header.DIR_FstClusLO = nextcluster;
    } else {
      sts = putfatentry (nextcluster, lastcluster, iopex);
      if (sts != OZ_SUCCESS) return (sts);
    }
    file -> allocblocks += volex -> bpb.BPB_SecPerClus;			// file has one more cluster
    lastcluster = nextcluster;
    -- numclusters;							// one less cluster needed
  }
  if (lastcluster == 0) {						// write new eof mark on last cluster
    file -> filex -> header.DIR_FstClusHI = 0;
    file -> filex -> header.DIR_FstClusLO = 0;
  } else {
    sts2 = putfatentry (volex -> eoc, lastcluster, iopex);
    if (sts == OZ_SUCCESS) sts = sts2;
  }
  return (sts);
}

static uLong findfreecluster (uLong lastcluster, uLong *nextcluster, OZ_VDFS_Iopex *iopex)

{
  OZ_VDFS_Volex *volex;
  uLong cluster, fatentry, sts;

  volex = iopex -> devex -> volume -> volex;

  while (1) {
    for (cluster = volex -> fsi.FSI_Nxt_Free; cluster < volex -> dataclusters + 2; cluster ++) {
      sts = getfatentry (cluster, &fatentry, iopex);			// read the cluster contents
      if (sts != OZ_SUCCESS) return (sts);				// return if failure status
      if (fatentry == 0) {						// if zero, it is free
        *nextcluster = cluster;						// return cluster number
        volex -> fsi.FSI_Nxt_Free = cluster + 1;			// start searching here next time
        volex -> fsi.FSI_Free_Count --;					// one less free cluster
        return (OZ_SUCCESS);						// successful
      }
    }
    if (volex -> fsi.FSI_Nxt_Free == 2) return (OZ_DISKISFULL);		// none available, disk is full
    volex -> fsi.FSI_Nxt_Free = 2;					// make sure we've scanned the whole thing
  }
}

/************************************************************************/
/*									*/
/*  Delete a file's header from its directory				*/
/*									*/
/************************************************************************/

static uLong delete_header (OZ_VDFS_File *file, OZ_VDFS_Iopex *iopex)

{
  OZ_VDFS_Filex *filex;
  OZ_VDFS_Volex *volex;
  OZ_VDFS_Volume *volume;
  uLong cluster, sts;

  filex  = file -> filex;
  volume = file -> volume;
  volex  = volume -> volex;

  extend_file (file, 0, 0, iopex);	// free all its blocks but don't put on dirty headers list

  while (1) {

    /* Read existing directory cluster */

    sts = oz_dev_vdfs_readlogblock (filex -> fileid.direntlbn, 0, volume -> dirblocksize, volex -> dirblockbuff, iopex);
    if (sts != OZ_SUCCESS) break;

    /* Set the file's entries to 0xE5 records */

    while ((filex -> fileid.direntnum != 0) && (filex -> fileid.direntidx < volume -> dirblocksize / sizeof (Direntry))) {
      memset (volex -> dirblockbuff + filex -> fileid.direntidx, 0, sizeof (Direntry));
      volex -> dirblockbuff[filex->fileid.direntidx].s.DIR_Name[0] = 0xE5;
      filex -> fileid.direntnum --;
      filex -> fileid.direntidx ++;
    }

    /* Write the directory cluster back to disk */

    sts = oz_dev_vdfs_writelogblock (filex -> fileid.direntlbn, 0, volume -> dirblocksize, volex -> dirblockbuff, 0, iopex);
    if (sts != OZ_SUCCESS) break;

    /* If we wrote them all, we're done */

    if (filex -> fileid.direntnum == 0) break;

    /* Not done yet, link to next cluster in directory */

    filex -> fileid.direntlbn = dirlbn_next (filex -> fileid.direntlbn, iopex);
    if (filex -> fileid.direntlbn == 0) break;
    filex -> fileid.direntidx = 0;
  }

  return (sts);
}

/************************************************************************/
/*									*/
/*  Read header from directory into the given file struct		*/
/*									*/
/*    Input:								*/
/*									*/
/*	fileid = file-id of the header					*/
/*									*/
/*    Output:								*/
/*									*/
/*	read_header = OZ_SUCCESS : successfully read			*/
/*	                    else : read error status			*/
/*	*filex = filled in						*/
/*									*/
/************************************************************************/

static uLong read_header (const OZ_VDFS_Fileid *fileid, OZ_VDFS_Filex *filex, OZ_VDFS_Iopex *iopex)

{
  OZ_Dbn lbn, reldatasector;
  OZ_VDFS_Volex *volex;
  OZ_VDFS_Volume *volume;
  uLong cluster, idx, num, sts;

  volume = iopex -> devex -> volume;
  volex  = volume -> volex;

  memset (filex, 0, sizeof *filex);

  /* Root directory's fileid is all zeroes, do it special as it has no real header */

  if ((fileid -> direntlbn == 0) && (fileid -> direntidx == 0) && (fileid -> direntnum == 0)) return (OZ_SUCCESS);

  /* Otherwise, the fileid values must make sense */

  num = volex -> bpb.BPB_ResvdSecCnt + volex -> bpb.BPB_NumFATs * volex -> FATSz;
  if ((fileid -> direntlbn < num) 						// it can't start in reserved or FAT areas
   || (fileid -> direntnum == 0) 						// it must occupy some slots in directory
   || (fileid -> direntidx >= volume -> dirblocksize / sizeof (Direntry))) {	// it can't start beyond end of a cluster
    return (OZ_BADFILEID);
  }

  /* Read the header entry from the directory */

  lbn = fileid -> direntlbn;							// first entry (perhaps long name) is in this block
  idx = fileid -> direntidx;							// ... at this index
  num = fileid -> direntnum;							// total number of entries (incl short name entry)
  while (-- num > 0) {								// repeat while long name entries to skip over
    idx ++;									// increment index to next entry
    if (idx == volume -> dirblocksize / sizeof (Direntry)) {			// see if we hit the end of a directory cluster
      idx = 0;									// ok, reset to beginning
      lbn = dirlbn_next (lbn, iopex);						// ... of next cluster
      if (lbn == 0) return (OZ_ENDOFFILE);					// make sure we didn't hit directory's eof
    }
  }

  idx *= sizeof (Direntry);							// get offset within the 'lbn' block
  sts  = oz_dev_vdfs_readlogblock (lbn, idx, sizeof filex -> header, &(filex -> header), iopex); // read directory entry = header
  if (sts != OZ_SUCCESS) return (sts);

  /* Make sure the entry is still valid */

  if ((filex -> header.DIR_Name[0] == 0xE5) 					// it must not have been freed off
   || (filex -> header.DIR_Name[0] == 0) 					// it must not be past end-of-file
   || (filex -> header.DIR_Attr & ATTR_VOLUME_ID) 				// it must not be vol-id or long name entry
   || (filex -> header.DIR_CrtDate != fileid -> crtdate) 			// it must have the same create date/time
   || (filex -> header.DIR_CrtTime != fileid -> crttime) 
   || (filex -> header.DIR_CrtTimeTenth != fileid -> crtenth)) {
    oz_dev_vdfs_printk (iopex, "oz_dev_fat read_header: fileid/header mismatch\n");
    oz_knl_printk ("oz_dev_fat read_header: fileid:\n"); oz_knl_dumpmem (sizeof *fileid, fileid);
    oz_knl_printk ("oz_dev_fat read_header: header lbn %u, ofs %u:\n", lbn, idx); oz_knl_dumpmem (sizeof filex -> header, &(filex -> header));
    return (OZ_FILEDELETED);
  }

  /* Fill in rest of filex and return success status */

  filex -> fileid = *fileid;							// save the file-id
  filex -> headerlbn = lbn;							// save where the short dir entry is
  filex -> headerofs = idx;

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

static void fat_mark_header_dirty (OZ_VDFS_File *dirtyfile)

{ }

/* This routine is called by oz_dev_vdfs to write out a dirty header */

static uLong fat_write_dirty_header (OZ_VDFS_File *dirtyfile, Long alf, OZ_Datebin now, OZ_VDFS_Volume *volume, OZ_VDFS_Iopex *iopex)

{
  OZ_VDFS_Filex *filex;
  OZ_VDFS_Volex *volex;
  uLong sts, vl;

  filex = dirtyfile -> filex;
  volex = volume -> volex;

  /* Maybe we're trying to write the root directory's attributes */
  /* Since we don't have a place to write them, ignore the call  */

  if (filex -> headerlbn == 0) return (OZ_SUCCESS);

  /* Maybe the eof pointer in the header needs updating */

  if (alf & OZ_VDFS_ALF_M_EOF) {					// see if recio needs us to update the eof poistion
    vl = oz_hw_smplock_wait (&(dirtyfile -> attrlock_vl));		// ok, lock it so we get consistent efblk/efbyt values
    filex -> header.DIR_FileSize = (dirtyfile -> attrlock_efblk - 1) * volex -> bpb.BPB_BytesPerSec + dirtyfile -> attrlock_efbyt;
    oz_hw_smplock_clr (&(dirtyfile -> attrlock_vl), vl);
  }

  /* Maybe some dates in the header need updating */

  if (alf & (OZ_VDFS_ALF_M_MDT | OZ_VDFS_ALF_M_CDT | OZ_VDFS_ALF_M_ADT)) { // see if any of the dates are to be modified
    if (alf & (OZ_VDFS_ALF_M_MDT | OZ_VDFS_ALF_M_CDT)) {		// modify last write date/time
      filex -> header.DIR_WrtTime = oz_to_fat_time (now);
      filex -> header.DIR_WrtDate = oz_to_fat_date (now);
    }
    if (alf & OZ_VDFS_ALF_M_ADT) {					// modify last access date
      filex -> header.DIR_LstAccDate = oz_to_fat_date (now);
    }
  }

  /* We should only be writing nice headers */

  if (!validate_header (&(filex -> header), iopex)) oz_crash ("oz_dev_fat write_dirty_header: writing corrupt header");

  /* Write to disk */

  sts = oz_dev_vdfs_writelogblock (filex -> headerlbn, filex -> headerofs, sizeof filex -> header, &(filex -> header), 0, iopex);
  if (sts != OZ_SUCCESS) oz_dev_vdfs_printk (iopex, "oz_dev_fat write_dirty_header: error %u writing header\n", sts);
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

static uLong fat_writehomeblock (OZ_VDFS_Volume *volume, OZ_VDFS_Iopex *iopex)

{
  OZ_VDFS_Volex *volex;
  uLong sts;

  volex = volume -> volex;

  sts = oz_dev_vdfs_writelogblock (0, 0, sizeof volex -> bpb, &(volex -> bpb), 0, iopex);
  if ((sts == OZ_SUCCESS) && (volex -> fattype == FATTYPE_32)) {
    sts = oz_dev_vdfs_writelogblock (volex -> bpb.f.s32.BPB_FSInfo, 0, sizeof volex -> fsi, &(volex -> fsi), 0, iopex);
  }
  return (sts);
}

/************************************************************************/
/*									*/
/*  Verify volume structures						*/
/*									*/
/************************************************************************/

static uLong fat_verify_volume (OZ_VDFS_Iopex *iopex, OZ_VDFS_Devex *devex)

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
/*  Validate an file header						*/
/*									*/
/*    Note:  This routine is called by the file open routine to check 	*/
/*	the on-disk header						*/
/*									*/
/************************************************************************/

static int validate_header (Sdirentry *header, OZ_VDFS_Iopex *iopex)

{
  if ((header -> DIR_Attr & ATTR_LONG_NAME_MASK) == ATTR_LONG_NAME) return (0);	// can't be a long entry
  if (header -> DIR_Name[0] == 0xE5) return (0);				// can't be a free entry
  if (header -> DIR_Name[0] == 0) return (0);					// can't be the eod mark
  return (validirbuf (sizeof (Direntry), (Direntry *)header, iopex));		// must be valid dir entry otherwise
}

/************************************************************************/
/*									*/
/*  Validate directory block contents					*/
/*									*/
/*    Input:								*/
/*									*/
/*	totalsize = total size (in bytes) given				*/
/*	dirbuffer = directory buffer to validate			*/
/*									*/
/*    Output:								*/
/*									*/
/*	validirbuf = 0 : contents invalid				*/
/*	             1 : contents valid					*/
/*	            -1 : valid, and hit eof mark			*/
/*									*/
/************************************************************************/

#define VALIDDIRDATE(__d) ((__d==0) || (((__d&0x1F)>0) && ((__d&0x1F)<32) && ((__d&0x1E0)>0) && ((__d&0x1E0)<(13<<5))))
#define VALIDDIRTIME(__t)              (((__t&0x1F)<30) && ((__t&0x7E0)<(60<<5)) && ((__t&0xF800)<(23<<11)))

static int validirbuf (uLong totalsize, Direntry *dirbuffer, OZ_VDFS_Iopex *iopex)

{
  int ec, i, j, ldirseq;
  OZ_VDFS_Volex *volex;
  uByte c, cksm, lncksm;
  uLong firstcluster, numentries;

  volex = iopex -> devex -> volume -> volex;

  numentries = totalsize / sizeof *dirbuffer;

  ldirseq = -1;									// we might start in the middle of a long sequence

  for (i = 0; i < numentries; i ++) {
    if (dirbuffer[i].s.DIR_Name[0] == 0xE5) continue;				// skip over free entry
    if (dirbuffer[i].s.DIR_Name[0] == 0) return (-1);				// hit end of file

    // Check long directory entry

    if ((dirbuffer[i].s.DIR_Attr & ATTR_LONG_NAME_MASK) == ATTR_LONG_NAME) {	// check for long name entry
      ec = 1;
      if (dirbuffer[i].l.LDIR_FstClusLO != 0) goto return0;			// must have 'first cluster' of zero
      ec = 2;
      if (dirbuffer[i].l.LDIR_Ord == 0) goto return0;				// the sequence must be non-zero
      if (ldirseq <= 0) {							// see if this is the first long name entry
        ec = 3;
        if ((ldirseq == 0) && !(dirbuffer[i].l.LDIR_Ord & 0x40)) goto return0;	// if so, it must have flag bit in sequence
        ldirseq = dirbuffer[i].l.LDIR_Ord & 0x3F;
        lncksm  = dirbuffer[i].l.LDIR_Chksum;					// get the short name's checksum
      } else {
        ec = 4;
        if (dirbuffer[i].l.LDIR_Ord != -- ldirseq) goto return0;		// not, the sequence must be in order
        ec = 5;
        if (dirbuffer[i].l.LDIR_Chksum != lncksm) goto return0;			// ... and the short name checksum must match
      }
    }

    // Maybe we were expecting a long entry

    else if (ldirseq > 1) {
      ec = 6;
      goto return0;
    }

    // Check short directory entry

    else {
      ec = 7;
      if (dirbuffer[i].s.DIR_Name[0] == 0x20) goto return0;			// name can't begin with a space
      if (dirbuffer[i].s.DIR_Attr & ATTR_VOLUME_ID) continue;			// skip over volume-id entries
      cksm = shortcksm (dirbuffer[i].s.DIR_Name);
      if ((memcmp (dirbuffer[i].s.DIR_Name, ".          ", 11) != 0) 
       && (memcmp (dirbuffer[i].s.DIR_Name, "..         ", 11) != 0)) {
        for (j = 0; j < sizeof dirbuffer[i].s.DIR_Name; j ++) {
          c = dirbuffer[i].s.DIR_Name[j];
          if ((j == 0) && (c == 0x05)) continue;				// first char can be 0x05
          ec = 8;
          if (c < 0x20) goto return0;						// otherwise, can't be a control char
          ec = 9;
          if ((c >= 'a') && (c <= 'z')) goto return0;				// never allow lower case
          ec = 10;
          if (memchr (badnamechars, c, sizeof badnamechars) != NULL) goto return0; // never allow any of these
        }
      }
      ec = 11;
      if ((ldirseq > 0) && (cksm != lncksm)) goto return0;			// long name checksum must match
      ec = 12;
      if (dirbuffer[i].s.DIR_CrtTimeTenth > 199) goto return0;			// tenths of 2-sec must be in range
      ec = 13;
      if (!VALIDDIRTIME (dirbuffer[i].s.DIR_CrtTime)) goto return0;		// create time fields must be in range
      ec = 14;
      if (!VALIDDIRDATE (dirbuffer[i].s.DIR_CrtDate)) goto return0;		// create date fields must be in range
      ec = 15;
      if (!VALIDDIRDATE (dirbuffer[i].s.DIR_LstAccDate)) goto return0;		// last access date fields must be ok
      ec = 16;
      if (!VALIDDIRTIME (dirbuffer[i].s.DIR_WrtTime)) goto return0;		// last write time fields must be ok
      ec = 17;
      if (!VALIDDIRDATE (dirbuffer[i].s.DIR_WrtDate)) goto return0;		// last write date fields must be ok
      firstcluster = (((uLong)(dirbuffer[i].s.DIR_FstClusHI)) << 16) + dirbuffer[i].s.DIR_FstClusLO;
      ec = 18;
      if ((firstcluster == 1) || ((firstcluster >= volex -> dataclusters + 2) && (firstcluster < volex -> eoc))) goto return0; // bad starting cluster number
      ldirseq = 0;								// not doing long name stuff anymore
    }
  }

  return (1);

return0:
  oz_dev_vdfs_printk (iopex, "oz_dev_fat validirbuf: failed test %d at index %u\n", ec, i);
  oz_knl_dumpmem (sizeof dirbuffer[i], dirbuffer + i);
  return (0);
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

static uLong fat_map_vbn_to_lbn (OZ_VDFS_File *file, OZ_Dbn virtblock, OZ_Dbn *nblocks_r, OZ_Dbn *logblock_r)

{
  OZ_Dbn cluster, relblock;
  OZ_VDFS_Iopex iopex;
  OZ_VDFS_Volex *volex;
  uLong sts;

  if (virtblock == 0) return (OZ_VBNZERO);
  relblock = virtblock - 1;
  volex = file -> volume -> volex;

  /* Special case for root directory */

  if (file -> filex -> fileid.direntlbn == 0) {

    /* FAT12/16 have contig fixed area for the root directory just after the FATs */

    if (volex -> fattype != FATTYPE_32) {
      *nblocks_r  = volex -> bpb.BPB_RootEntCnt * 32 / volex -> bpb.BPB_BytesPerSec;
      if (*nblocks_r <= relblock) return (OZ_ENDOFFILE);
      *nblocks_r -= relblock;
      *logblock_r = (volex -> bpb.BPB_NumFATs * volex -> bpb.BPB_FATSz16) 
                  + volex -> bpb.BPB_ResvdSecCnt + relblock;
      return (OZ_SUCCESS);
    }

    /* FAT32 has normal FAT cluster chain starting with cluster given in homeblock */

    cluster = volex -> bpb.f.s32.BPB_RootClus;
  }

  /* Otherwise, starting cluster number is in the directory entry */

  else cluster = (((uLong)(file -> filex -> header.DIR_FstClusHI)) << 16) + file -> filex -> header.DIR_FstClusLO;

  /* Count clusters from there */

  memset (&iopex, 0, sizeof iopex);
  iopex.devex = file -> volume -> devex;
  while (relblock >= volex -> bpb.BPB_SecPerClus) {
    if ((cluster == 0) || (cluster >= volex -> eoc)) return (OZ_ENDOFFILE);
    sts = getfatentrych (cluster, &cluster, &iopex);
    if (sts != OZ_SUCCESS) return (sts);
    relblock -= volex -> bpb.BPB_SecPerClus;
  }

  *nblocks_r  = volex -> bpb.BPB_SecPerClus - relblock;
  *logblock_r = CLUS_TO_LBN (cluster) + relblock;

  return (OZ_SUCCESS);
}

/************************************************************************/
/*									*/
/*  Calculate the initial lbn of a directory				*/
/*									*/
/*    Input:								*/
/*									*/
/*	dirfilex = directory to get the first lbn of			*/
/*	volex    = volume extension that directory is on		*/
/*									*/
/*    Output:								*/
/*									*/
/*	dirlbn_init = 0 : directory is empty				*/
/*	           else : initial lbn of directory			*/
/*									*/
/************************************************************************/

static uLong dirlbn_init (OZ_VDFS_Filex *dirfilex, OZ_VDFS_Volex *volex)

{
  /* Non-root directories are always just like files */

  if (dirfilex -> fileid.direntlbn != 0) {
    return (CLUS_TO_LBN ((dirfilex -> header.DIR_FstClusHI << 16) + dirfilex -> header.DIR_FstClusLO));
  }

  /* FAT-32 root directories are like files, but the first cluster is in the home block */

  if (volex -> fattype == FATTYPE_32) {
    return (CLUS_TO_LBN (volex -> bpb.f.s32.BPB_RootClus));
  }

  /* FAT-12 and FAT-16 root directories have a fixed area on disk */

  return ((volex -> bpb.BPB_NumFATs * volex -> bpb.BPB_FATSz16) + volex -> bpb.BPB_ResvdSecCnt);
}

/************************************************************************/
/*									*/
/*  Get LBN of next cluster in directory				*/
/*									*/
/*    Input:								*/
/*									*/
/*	dirlbn = lbn at start of previous cluster			*/
/*	iopex  = current I/O operation					*/
/*									*/
/*    Output:								*/
/*									*/
/*	dirlbn_next = 0 : reached end-of-directory			*/
/*	           else : lbn of next cluster in directory		*/
/*									*/
/************************************************************************/

static uLong dirlbn_next (uLong dirlbn, OZ_VDFS_Iopex *iopex)

{
  OZ_VDFS_Volex *volex;
  uLong cluster, sts;

  volex = iopex -> devex -> volume -> volex;

  /* FAT-12 and FAT-16 root directories have a fixed area on disk */

  if (volex -> fattype != FATTYPE_32) {
    cluster = dirlbn - ((volex -> bpb.BPB_NumFATs * volex -> bpb.BPB_FATSz16) + volex -> bpb.BPB_ResvdSecCnt);
    if (cluster < volex -> bpb.BPB_RootEntCnt * sizeof (Direntry) / volex -> bpb.BPB_BytesPerSec) {
      if (cluster + volex -> bpb.BPB_SecPerClus == volex -> bpb.BPB_RootEntCnt * sizeof (Direntry) / volex -> bpb.BPB_BytesPerSec) {
        return (0);
      }
      return (dirlbn + volex -> bpb.BPB_SecPerClus);
    }
  }

  /* All others follow normal cluster chaining */

  cluster = LBN_TO_CLUS (dirlbn);
  sts = getfatentrych (cluster, &cluster, iopex);
  if ((sts != OZ_SUCCESS) || (cluster >= volex -> eoc)) cluster = 0;
  return (CLUS_TO_LBN (cluster));
}

/************************************************************************/
/*									*/
/*  Get entry from FAT							*/
/*									*/
/*    Input:								*/
/*									*/
/*	cluster  = cluster number					*/
/*	iopex    = current io on the volume				*/
/*									*/
/*    Output:								*/
/*									*/
/*	getfatentry = OZ_SUCCESS : successful				*/
/*	       OZ_BADBLOCKNUMBER : cluster out of range			*/
/*	                    else : I/O error status			*/
/*	*fatentry = value of fat entry					*/
/*									*/
/************************************************************************/

	/* This version is used for scanning a file chain.  It checks the range of the resultant value. */

static uLong getfatentrych (uLong cluster, uLong *fatentry, OZ_VDFS_Iopex *iopex)

{
  OZ_VDFS_Volex *volex;
  uLong sts;

  sts = getfatentry (cluster, fatentry, iopex);
  if (sts == OZ_SUCCESS) {
    cluster = *fatentry;
    if (cluster < 2) return (OZ_BADBLOCKNUMBER);		// cluster 0 or 1 bad in a chain
    volex = iopex -> devex -> volume -> volex;
    if ((cluster >= volex -> dataclusters + 2) 			// maybe it's off end of disk
     && (cluster < volex -> eoc)) return (OZ_BADBLOCKNUMBER);
  }
  return (sts);
}

	/* This version will read any cluster value */

static uLong getfatentry (uLong cluster, uLong *fatentry, OZ_VDFS_Iopex *iopex)

{
  OZ_VDFS_Volex *volex;
  uLong sts;

  volex = iopex -> devex -> volume -> volex;

  if (cluster >= volex -> dataclusters + 2) return (OZ_BADBLOCKNUMBER);

  switch (volex -> fattype) {

    // The FAT is an array of 12-bit values

    case FATTYPE_12: {
      sts = readfat ((cluster & 1) + 2, (cluster / 2) + cluster, fatentry, iopex);
      break;
    }

    // The FAT is an array of 16-bit values

    case FATTYPE_16: {
      sts = readfat (1, cluster * 2, fatentry, iopex);
      break;
    }

    // The FAT is an array of 32-bit values
    // But only the low 28-bits of each entry are used

    case FATTYPE_32: {
      sts = readfat (0, cluster * 4, fatentry, iopex);
      break;
    }

    // Who knows what the fat type is

    default: oz_crash ("oz_dev_fat getfatentry: bad fattype %d", volex -> fattype);
  }

  return (sts);
}

/************************************************************************/
/*									*/
/*  Put entry to FAT							*/
/*									*/
/*    Input:								*/
/*									*/
/*	fatentry = value to put in FAT array				*/
/*	cluster  = cluster number					*/
/*	iopex    = current io on the volume				*/
/*									*/
/*    Output:								*/
/*									*/
/*	putfatentry = OZ_SUCCESS : successful				*/
/*	       OZ_BADBLOCKNUMBER : cluster or fatentry out of range	*/
/*	                    else : I/O error status			*/
/*									*/
/************************************************************************/

static uLong putfatentry (uLong fatentry, uLong cluster, OZ_VDFS_Iopex *iopex)

{
  OZ_VDFS_Volex *volex;
  uLong sts;

  volex = iopex -> devex -> volume -> volex;

  if (cluster >= volex -> dataclusters + 2) return (OZ_BADBLOCKNUMBER);

  switch (volex -> fattype) {

    // The FAT is an array of 12-bit values

    case FATTYPE_12: {
      if (fatentry > 0xFFF) return (OZ_BADBLOCKNUMBER);
      sts = writefat (fatentry, (cluster & 1) + 2, (cluster / 2) + cluster, iopex);
      break;
    }

    // The FAT is an array of 16-bit values

    case FATTYPE_16: {
      if (fatentry > 0xFFFF) return (OZ_BADBLOCKNUMBER);
      sts = writefat (fatentry, 1, cluster * 2, iopex);
      break;
    }

    // The FAT is an array of 32-bit values
    // But only the low 28-bits of each entry are used

    case FATTYPE_32: {
      if (fatentry > 0xFFFFFFF) return (OZ_BADBLOCKNUMBER);
      sts = writefat (fatentry, 0, cluster * 4, iopex);
      break;
    }

    // Who knows what the fat type is

    default: oz_crash ("oz_dev_fat putfatentry: bad fattype %d", volex -> fattype);
  }

  return (sts);
}

/************************************************************************/
/*									*/
/*  Read from the FAT							*/
/*									*/
/*    Input:								*/
/*									*/
/*	code = 0 : 28-bit						*/
/*	       1 : 16-bit						*/
/*	       2 : 12-bit, lower 12 bits of word			*/
/*	       3 : 12-bit, upper 12 bits of word			*/
/*	byteoffset = byte offset within the fat				*/
/*	             28/16 bit assumed naturally aligned		*/
/*	             12 bit need not be aligned				*/
/*									*/
/*    Output:								*/
/*									*/
/*	readfat = OZ_SUCCESS : read successful				*/
/*	                else : error status				*/
/*	*data_r = data read from the fat				*/
/*									*/
/************************************************************************/

static int fetfat (int code, uByte *va, uLong *data_r, int lastbyte);

static uLong readfat (int code, uLong byteoffset, uLong *data_r, OZ_VDFS_Iopex *iopex)

{
  OZ_Pagentry savepte;
  OZ_VDFS_Devex *devex;
  OZ_VDFS_Volex *volex;
  uByte *va;
  uLong fatpaglb, fatpagof, fatseclb, fatsecof, i, sts;

  devex = iopex -> devex;
  volex = devex -> volume -> volex;

  /* Compute the FAT lbn for the first FAT copy and the offset within that block */

calcit:
  fatseclb = (byteoffset + (code >> 2)) / volex -> bpb.BPB_BytesPerSec + volex -> bpb.BPB_ResvdSecCnt;
  fatsecof = (byteoffset + (code >> 2)) % volex -> bpb.BPB_BytesPerSec;

  /* If cache enabled, get the data directly out of the cache page */

  sts = OZ_SUCCESS;
  if (devex -> dcache != NULL) {
    for (i = volex -> fatloidx; i < volex -> fathiidx; i ++) {						// try each copy of FAT
      fatpaglb = (fatseclb + i * volex -> FATSz) & - volex -> blocksperpage;				// get lbn at beg of page
      fatpagof = (fatseclb + i * volex -> FATSz - fatpaglb) * volex -> bpb.BPB_BytesPerSec + fatsecof;	// get offset within page
      if (fatpaglb != volex -> fatsecurlb) {								// see if already mapped
        if (volex -> fatphypage != OZ_PHYPAGE_NULL) {							// no, release old page
          oz_knl_dcache_pfrel (devex -> dcache, volex -> fatphypage);
          volex -> fatphypage = OZ_PHYPAGE_NULL;
        }
        volex -> fatsecurlb = 0;									// map new page
        sts = oz_knl_dcache_pfmap (devex -> dcache, fatpaglb, &(volex -> fatphypage));
        if (sts == OZ_SUCCESS) volex -> fatsecurlb = fatpaglb;
      }
      if (fatpaglb == volex -> fatsecurlb) {								// make sure it's mapped now
        va = oz_hw_phys_mappage (volex -> fatphypage, &savepte) + fatpagof;				// point to data in page
        code = fetfat (code, va, data_r, fatpagof == (1 << OZ_HW_L2PAGESIZE) - 1);			// fetch data
        oz_hw_phys_unmappage (savepte);									// unmap page
        if (code & 4) goto calcit;									// repeat for 12-bit split
        break;
      }
    }
  }

  /* Otherwise, read the data directly from the disk */

  else {
    if (volex -> fatsecurlb != fatseclb) {								// see if block in memory
      volex -> fatsecurlb = 0;										// if not, read it
      for (i = volex -> fatloidx; i < volex -> fathiidx; i ++) {					// ... any copy is ok
        sts = oz_dev_vdfs_readlogblock (fatseclb + i * volex -> FATSz, 0, volex -> bpb.BPB_BytesPerSec, volex -> fatsecbuff, iopex);
        if (sts == OZ_SUCCESS) {
          volex -> fatsecurlb = fatseclb;
          break;
        }
      }
    }
    if (volex -> fatsecurlb == fatseclb) {								// make sure we got it
      va = volex -> fatsecbuff + fatsecof;								// point to data
      code = fetfat (code, va, data_r, fatsecof == volex -> bpb.BPB_BytesPerSec - 1);			// get it
      if (code & 4) goto calcit;									// repeat for 12-bit split
    }
  }

  return (sts);
}

/* Fetch FAT data from buffer */

static int fetfat (int code, uByte *va, uLong *data_r, int lastbyte)

{
  switch (code) {
    case 0: {		// 28-bit
      *data_r = *(uLong *)va & 0xFFFFFFF;
      break;
    }
    case 1: {		// 16-bit
      *data_r = *(uWord *)va;
      break;
    }
    case 2: {		// 12-bit lower
      if (lastbyte) {
        *data_r = *va;
        code = 4;
      }
      else *data_r = *(uWord *)va & 0xFFF;
      break;
    }
    case 3: {		// 12-bit upper
      if (lastbyte) {
        *data_r = *va >> 4;
        code = 5;
      }
      else *data_r = *(uWord *)va >> 4;
      break;
    }
    case 4: {		// 12-bit lower, 2nd half
      *data_r |= ((uLong)(*va & 0x0F)) << 8;
      code = 2;
      break;
    }
    case 5: {		// 12-bit upper, 2nd half
      *data_r |= ((uLong)*va) << 4;
      code = 3;
      break;
    }
  }

  return (code);
}

/************************************************************************/
/*									*/
/*  Write to the FAT							*/
/*									*/
/*    Input:								*/
/*									*/
/*	code = 0 : 28-bit						*/
/*	       1 : 16-bit						*/
/*	       2 : 12-bit, lower 12 bits of word			*/
/*	       3 : 12-bit, upper 12 bits of word			*/
/*	byteoffset = byte offset within the fat				*/
/*	             28/16 bit assumed naturally aligned		*/
/*	             12 bit need not be aligned				*/
/*	data = data to write						*/
/*									*/
/*    Output:								*/
/*									*/
/*	writefat = OZ_SUCCESS : write successful			*/
/*	                 else : error status				*/
/*									*/
/************************************************************************/

static int modfat (int code, uByte *va, uLong data, int lastbyte);

static uLong writefat (uLong data, int code, uLong byteoffset, OZ_VDFS_Iopex *iopex)

{
  OZ_Pagentry savepte;
  OZ_VDFS_Devex *devex;
  OZ_VDFS_Volex *volex;
  uByte *va;
  uLong basefatseclb, fatpaglb, fatpagof, fatseclb, fatsecof, i, sts, sts2;

  devex = iopex -> devex;
  volex = devex -> volume -> volex;

  /* Try to write the update to all FAT copies */

  sts = OZ_SUCCESS;
  for (i = volex -> fatloidx; i < volex -> fathiidx; i ++) {

    /* Compute the FAT lbn for this FAT copy and the offset within that block */

calcit:
    basefatseclb = (byteoffset + (code >> 2)) / volex -> bpb.BPB_BytesPerSec + volex -> bpb.BPB_ResvdSecCnt;
    fatseclb = basefatseclb + i * volex -> FATSz;
    fatsecof = (byteoffset + (code >> 2)) % volex -> bpb.BPB_BytesPerSec;

    /* If cache enabled, put the data directly into the cache pages */

    if (devex -> dcache != NULL) {
      fatpaglb = fatseclb & - volex -> blocksperpage;					// lbn at beginning of the FAT page
      fatpagof = (fatseclb - fatpaglb) * volex -> bpb.BPB_BytesPerSec + fatsecof;	// offset within that page
      if (fatpaglb != volex -> fatsecurlb) {						// see if currently mapped
        if (volex -> fatphypage != OZ_PHYPAGE_NULL) {					// if not, release the old page
          oz_knl_dcache_pfrel (devex -> dcache, volex -> fatphypage);
          volex -> fatphypage = OZ_PHYPAGE_NULL;
        }
        volex -> fatsecurlb = 0;
        sts2 = oz_knl_dcache_pfmap (devex -> dcache, fatpaglb, &(volex -> fatphypage));	// ... and try to map the new page
        if (sts2 == OZ_SUCCESS) volex -> fatsecurlb = fatpaglb;
        if (sts == OZ_SUCCESS) sts = sts2;
      }
      if (fatpaglb == volex -> fatsecurlb) {						// see if correct page now mapped
        va = oz_hw_phys_mappage (volex -> fatphypage, &savepte) + fatpagof;		// ok, point to data to modify
        code = modfat (code, va, data, fatpagof == (1 << OZ_HW_L2PAGESIZE) - 1);	// modify it
        oz_hw_phys_unmappage (savepte);							// unmap the page
        sts2 = oz_knl_dcache_pfupd (devex -> dcache, fatpaglb, volex -> fatphypage, 0);	// tag it to be written to disk
        if (sts == OZ_SUCCESS) sts = sts2;
        if (code & 4) goto calcit;							// maybe repeat for 12-bit split page
      }
      if (sts != OZ_SUCCESS) break;
    }

    /* Otherwise, write the data directly to the disk */

    else {
      if (volex -> fatsecurlb != basefatseclb) {					// see if we have the block in memory
        volex -> fatsecurlb = 0;							// if not, read it
        sts2 = oz_dev_vdfs_readlogblock (fatseclb, 0, volex -> bpb.BPB_BytesPerSec, volex -> fatsecbuff, iopex);
        if (sts2 == OZ_SUCCESS) volex -> fatsecurlb = basefatseclb;
        if (sts == OZ_SUCCESS) sts = sts2;
      }
      if (volex -> fatsecurlb == basefatseclb) {					// make sure we got it
        va = volex -> fatsecbuff + fatsecof;						// ok, point to data to modify
        code = modfat (code, va, data, fatsecof == volex -> bpb.BPB_BytesPerSec - 1);	// modify it
        sts2 = oz_dev_vdfs_writelogblock (fatseclb, 0, volex -> bpb.BPB_BytesPerSec, volex -> fatsecbuff, 0, iopex); // write to disk
        if (sts == OZ_SUCCESS) sts = sts2;
        if (code & 4) goto calcit;							// maybe repeat for 12-bit split page
      }
    }
  }

  return (sts);
}

/* Modify value in FAT buffer */

static int modfat (int code, uByte *va, uLong data, int lastbyte)

{
  switch (code) {
    case 0: {										// 28-bit
      *(uLong *)va = (*(uLong *)va & 0xF0000000) | data;
      break;
    }
    case 1: {										// 16-bit
      *(uWord *)va = data;
      break;
    }
    case 2: {										// lower 12 bits of word
      if (lastbyte) {									// see if we cross page boundary
        *va = data;									// if so, just modify low byte
        code = 4;									// come back to write upper 4 bits
      }
      else *(uWord *)va = (*(uWord *)va & 0xF000) | data;				// write the lower 12 bits of word
      break;
    }
    case 3: {										// upper 12 bits of word
      if (lastbyte) {									// see if we cross page boundary
        *va = (*va & 0x0F) | (data << 4);						// if so, just modify low 4 bits
        code = 5;									// come back to write upper 8 bits
      }
      else *(uWord *)va = (*(uWord *)va & 0x000F) | (data << 4);			// write the upper 12 bits of word
      break;
    }
    case 4: {										// upper 4 bits of 12-bit value
      *va = (*va & 0xF0) | (data >> 8);
      code = 2;										// restore for next copy
      break;
    }
    case 5: {										// upper 8 bits of 12-bit value
      *va = data >> 4;
      code = 3;										// restore for next copy
      break;
    }
  }
  return (code);
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
    oz_crash ("oz_dev_fat validate_volume: bad boot/home block values");
  }
  if (!(volume -> dirty)) {
    if (volex -> bpb.homeversion != HOMEBLOCK_VERSION) {
      oz_crash ("oz_dev_fat validate_volume: homeversion %u", volex -> bpb.homeversion);
    }
    cksm = 0;
    for (i = 0; i < (sizeof volex -> bpb) / sizeof (uWord); i ++) {
      cksm += ((uWord *)&(volex -> bpb))[i];
    }
    if (cksm != 0) {
      oz_crash ("oz_dev_fat validate_volume: bad homeblock checksum");
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
      oz_crash ("oz_dev_fat validate_volume: %u file prev/next list corrupt", line);
    }
    if (file == volex -> indexbitmap)   found_ibm = 1;
    if (file == volex -> indexheaders)  found_ih  = 1;
    if (file == volex -> storagebitmap) found_sbm = 1;
    validate_file (file, volume, __LINE__, line);
    if (filex -> header.extseq != 1) oz_crash ("oz_dev_fat validate_volume: %u prime header extseq %u", line, filex -> header.extseq);
    nopenfiles ++;
    if (filex -> headerdirty) ndirtyfiles2 ++;
    extseq = 1;
    lastfid = &(filex -> header.fileid);
    for (extfile = file -> extfile; extfile != NULL; extfile = extfile -> extfile) {
      validate_file (extfile, volume, __LINE__, line);
      if (extfilex -> header.extseq != ++ extseq)  oz_crash ("oz_dev_fat validate_volume: %u ext header out of sequence", line);
      if (memcmp (&(extfilex -> header.dirid), lastfid, sizeof *lastfid) != 0) {
        oz_crash ("oz_dev_fat validate_volume: %u ext header (%u,%u,%u) dirid (%u,%u,%u) doesnt point to prev ext header (%u,%u,%u), prime (%u,%u,%u)", 
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
    oz_crash ("oz_dev_fat validate_volume: %u nopenfiles %u, but there were %u", line, volume -> nopenfiles, nopenfiles);
  }
  if (((volex -> indexbitmap   != NULL) && !found_ibm) 
   || ((volex -> indexheaders  != NULL) && !found_ih) 
   || ((volex -> storagebitmap != NULL) && !found_sbm)) {
    oz_crash ("oz_dev_fat validate_volume: %u found_ibm %d, _ih %d, _sbm %d", line, found_ibm, found_ih, found_sbm);
  }
  ndirtyfiles = 0;
  for (file = volume -> dirtyfiles; file != NULL; file = file -> nextdirty) {
    if (!(filex -> headerdirty)) {
      oz_crash ("oz_dev_fat validate_volume: %u on dirtyfiles list but not tagged dirty", line);
    }
    ndirtyfiles ++;
    if (ndirtyfiles > volume -> nopenfiles) {
      oz_crash ("oz_dev_fat validate_volume: %u dirtyfiles list corrupt", line);
    }
    validate_file (file, volume, __LINE__, line);
  }
  if (ndirtyfiles != ndirtyfiles2) {
    oz_crash ("oz_dev_fat validate_volume: %u ndirtyfiles %u, ndirtyfiles2 %u", line, ndirtyfiles, ndirtyfiles2);
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
    oz_crash ("oz_dev_fat validate_file: file volume %p not volume %p", file -> volume, volume);
  }

  if (!(filex -> headerdirty)) {
    cksm = 0;
    for (i = 0; i < volex -> bpb.BPB_BytesPerSec / sizeof (uWord); i ++) {
      cksm += ((uWord *)&(filex -> header))[i];
    }
    if (cksm != 0) {
      oz_crash ("oz_dev_fat validate_file: bad header checksum");
    }
  }

  offs = 0;
  areasize = (((uByte *)&(filex -> header)) + volex -> bpb.BPB_BytesPerSec - filex -> header.area) & -4;
  for (i = 0; i < OZ_FS_HEADER_NAREAS; i ++) {
    if (filex -> header.areas[i].offs < offs) {
      oz_crash ("oz_dev_fat validate_file: area[%u] offset %u follows %u", i, filex -> header.areas[i].offs, offs);
    }
    offs = ((filex -> header.areas[i].size + 3) & -4) + filex -> header.areas[i].offs;
    if (offs > areasize) {
      oz_crash ("oz_dev_fat validate_file: area[%u] ends at %u", i, offs);
    }
  }

  blocksinhdr = 0;
  npointers   = filex -> header.areas[OZ_FS_HEADER_AREA_POINTER].size / sizeof *pointers;
  pointers    = POINTERS (file);
  for (i = 0; i < npointers; i ++) {
    blocksinhdr += pointers[i].blockcount;
  }
  if (blocksinhdr != file -> filex -> blocksinhdr) {
    oz_crash ("oz_dev_fat validate_file: blocksinhdr is %u, but there are %u", file -> filex -> blocksinhdr, blocksinhdr);
  }
#endif
}

/************************************************************************/
/*									*/
/*  Convert OZONE date/time to FAT					*/
/*									*/
/************************************************************************/

static uWord oz_to_fat_date (OZ_Datebin datebin)

{
  uLong datelongs[OZ_DATELONG_ELEMENTS], dd, mm, yy, yyyymmdd;
  uWord fatdate;

  datebin = oz_sys_datebin_tzconv (datebin, OZ_DATEBIN_TZCONV_UTC2LCL, 0);
  oz_sys_datebin_decode (datebin, datelongs);

  yyyymmdd = oz_sys_daynumber_decode (datelongs[OZ_DATELONG_DAYNUMBER]);

  yy = yyyymmdd >> 16;
  if (yy < 1980) yy = 0;
  else yy -= 1980;
  if (yy > 127) yy = 127;

  mm = (yyyymmdd >> 8) & 0x0F;

  dd = yyyymmdd & 0x1F;

  return ((yy << 9) | (mm << 5) | dd);
}

static uWord oz_to_fat_time (OZ_Datebin datebin)

{
  uLong datelongs[OZ_DATELONG_ELEMENTS], hh, mm, ss;

  datebin = oz_sys_datebin_tzconv (datebin, OZ_DATEBIN_TZCONV_UTC2LCL, 0);
  oz_sys_datebin_decode (datebin, datelongs);

  ss = datelongs[OZ_DATELONG_SECOND] % 60;
  datelongs[OZ_DATELONG_SECOND] /= 60;
  mm = datelongs[OZ_DATELONG_SECOND] % 60;
  hh = datelongs[OZ_DATELONG_SECOND] / 60;

  return ((hh << 11) | (mm << 5) | (ss >> 1));
}

/************************************************************************/
/*									*/
/*  Convert FAT date/time to OZONE					*/
/*									*/
/*    Input:								*/
/*									*/
/*	fatdate<15:09> = years from 1980				*/
/*	       <08:05> = month, 1..12					*/
/*	       <04:00> = day of month, 1..31				*/
/*	fattime<15:11> = hour, 0..23					*/
/*	       <10:05> = minute, 0..59					*/
/*	       <04:00> = two second count, 0..29			*/
/*	fatenth = tenths of second, 0..199				*/
/*									*/
/*    Output:								*/
/*									*/
/*	fat_dt_to_oz = OZONE date/time					*/
/*									*/
/************************************************************************/

static OZ_Datebin fat_dt_to_oz (uWord fatdate, uWord fattime, uByte fatenth)

{
  OZ_Datebin datebin;
  uLong datelongs[OZ_DATELONG_ELEMENTS], yyyymmdd;

  if (fatdate == 0) yyyymmdd = (1980 << 16) | (1 << 8) | 1;
  else {
    yyyymmdd  = (((fatdate >> 9) & 0x7F) + 1980) << 16;
    yyyymmdd |=  ((fatdate >> 5) & 0x0F)         <<  8;
    yyyymmdd |=    fatdate       & 0x1F;
  }

  datelongs[OZ_DATELONG_DAYNUMBER] = oz_sys_daynumber_encode (yyyymmdd);
  datelongs[OZ_DATELONG_SECOND]    = ((fattime >> 11) & 0x1F) * 3600;
  datelongs[OZ_DATELONG_SECOND]   += ((fattime >>  5) & 0x3F) *   60;
  datelongs[OZ_DATELONG_SECOND]   +=  (fattime        & 0x1F) *    2;
  datelongs[OZ_DATELONG_FRACTION]  = fatenth * (OZ_TIMER_RESOLUTION / 10);

  datebin = oz_sys_datebin_encode (datelongs);
  datebin = oz_sys_datebin_tzconv (datebin, OZ_DATEBIN_TZCONV_LCL2UTC, 0);
  return (datebin);
}

/************************************************************************/
/*									*/
/*  Determine if a given name string is a valid short name		*/
/*									*/
/*    Input:								*/
/*									*/
/*	namelen = length of 'name' string				*/
/*	name = given file name string					*/
/*	shortname = 11-byte buffer to store output in			*/
/*									*/
/*    Output:								*/
/*									*/
/*	makeshortname = 0 : given name is not a valid short name	*/
/*	                1 : given name is a valid short name		*/
/*	*shortname = filled in with short name, space filled		*/
/*									*/
/************************************************************************/

static int makeshortname (int namelen, const char *name, uByte *shortname, uByte *dirattr_r)

{
  int dots, i, j;
  uByte c;

  namelen = strnlen (name, namelen);
  if (namelen > 12) return (0);			// 8.3 can't be longer than 12 chars
  if (namelen == 0) return (0);			// can't be empty string
  if (name[0] == '.') return (0);		// can't begin with a dot
  *dirattr_r = 0;				// assume it's not a directory name
  if (name[namelen-1] == SEPCHR) {		// see if it ends with '/'
    if (-- namelen == 0) return (0);		// ok, that can't be all it is
    *dirattr_r = ATTR_DIRECTORY;		// return directory flag
  }
  if (name[namelen-1] == '.') return (0);	// can't end with a '.' (it'd be the same as no dot there)
  if (memchr (name, SEPCHR, namelen) != NULL) return (0); // can't otherwise contain a '/'
  dots = 0;					// haven't found any dots yet
  j = 0;					// no output yet
  for (i = 0; i < namelen; i ++) {		// scan through the string
    c = name[i];				// get a character
    if (c == 0) break;				// stop if null terminator
    if (c == '.') {				// check for a dot
      if (++ dots == 2) return (0);		// if two, can't be a short name
      while (j < 8) shortname[j++] = ' ';	// space fill to filetype position
      continue;
    }
    if (c <= 0x20) return (0);			// no control characters or spaces
    if ((c >= 'a') && (c <= 'z')) return (0);	// no lower case letters
    if (memchr (badnamechars, c, sizeof badnamechars) != NULL) return (0); // none of these special chars, either
    if (j == dots * 3 + 8) return (0);		// check for field overflow (8 chars before dot, 3 chars after dot)
    shortname[j++] = c;				// store in output
  }
  while (j < 11) shortname[j++] = ' ';		// space fill to the end
  return (1);
}

/************************************************************************/
/*									*/
/*  Determine if a given name string is a valid short name		*/
/*									*/
/*    Input:								*/
/*									*/
/*	namelen = length of 'name' string				*/
/*	name = given file name string					*/
/*	longnamebuf = LONGNAME_MAX byte buffer to store output in	*/
/*	shortname = 11-byte buffer to store output in			*/
/*									*/
/*    Output:								*/
/*									*/
/*	makelongname = length in bytes of longnamebuf used		*/
/*	*longnamebuf = filled in with null-terminated long name		*/
/*	*shortname   = filled in with suitable short name, space filled	*/
/*									*/
/************************************************************************/

static int makelongname (int namelen, const char *name, uByte *longnamebuf, uByte *shortname, uByte *dirattr_r)

{
  int i, j, k, longnamenum;
  uByte c;

  namelen = strnlen (name, namelen);			// get true length of name string
  if (namelen == 0) return (0);				// can't be a null string
  if (namelen >= LONGNAME_MAX / 2) return (0);		// can't overflow output buffer
  *dirattr_r = 0;					// assume it's not a directory name
  if (name[namelen-1] == SEPCHR) {			// see if it ends with '/'
    if (-- namelen == 0) return (0);			// can't be a null string
    *dirattr_r = ATTR_DIRECTORY;			// return directory flag
  }
  if (memchr (name, SEPCHR, namelen) != NULL) return (0); // can't otherwise contain a '/'
  k = 0;						// haven't used long name yet

  j = 0;						// haven't used the short name yet

  for (i = 0; i < namelen; i ++) {			// scan through input string
    c = name[i];					// get input character
    longnamebuf[k++] = c;				// copy to output
    longnamebuf[k++] = 0;				// ... unicode (idiot) style
    if (c == '.') {					// if a dot, reset to start of dot stuff in short name
      if (j > 8) j = 8;
      while (j < 8) shortname[j++] = ' ';
      continue;
    }
    if ((c >= 'a') && (c <= 'z')) {			// see if it's usable as a short-name char
      c -= 'a' - 'A';
      goto snc;
    }
    if ((c >= 'A') && (c <= 'Z')) goto snc;
    if ((c >= '0') && (c <= '9')) goto snc;
    if (c != '_') continue;
snc:
    if (j < 11) shortname[j++] = c;			// valid reasonable short name char, store it if room
  }
  while (j < 11) shortname[j++] = ' ';			// space fill short name
  longnamebuf[k++] = 0;					// null terminate long name
  longnamebuf[k++] = 0;
  return (k);
}

/************************************************************************/
/*									*/
/*  Match a segment of a long name					*/
/*									*/
/*    Input:								*/
/*									*/
/*	ldirentry = points to long directory entry			*/
/*	longnamelen = length of longnamebuf, including null terminator	*/
/*	longnamebuf = long name to compare				*/
/*									*/
/*    Output:								*/
/*									*/
/*	matchlongname = 0 : doesn't match				*/
/*	                1 : matches					*/
/*									*/
/************************************************************************/

static int matchlongname (Ldirentry *ldirentry, int longnamelen, const uByte *longnamebuf)

{
  const uByte *longnameseg;
  int segoffset;

  /* Calc offset within longnamebuf of the segment to compare */

  segoffset   = ((ldirentry -> LDIR_Ord & 0x3F) - 1) * 26;
  longnameseg = segoffset + longnamebuf;

  /* If name ends before this segment, this segment matches (it should be all 00's or FF's or who cares) */

  if (longnamelen <= segoffset) return (1);

  /* Maybe we only need to compare the first part */

  if (longnamelen <= segoffset + 10) {
    return (longcmp (ldirentry -> LDIR_Name1, longnameseg, longnamelen - segoffset));
  }

  /* Maybe we only need to compare the first and second parts */

  if (longnamelen <= segoffset + 22) {
    return (longcmp (ldirentry -> LDIR_Name1, longnameseg +  0, 10) 
         && longcmp (ldirentry -> LDIR_Name2, longnameseg + 10, longnamelen - segoffset - 10));
  }

  /* Maybe we only need to compare the first and second parts, and part of the third */

  if (longnamelen < segoffset + 26) {
    return (longcmp (ldirentry -> LDIR_Name1, longnameseg +  0, 10) 
         && longcmp (ldirentry -> LDIR_Name2, longnameseg + 10, 12) 
         && longcmp (ldirentry -> LDIR_Name3, longnameseg + 22, longnamelen - segoffset - 22));
  }

  /* We need to compare all three parts */

  return (longcmp (ldirentry -> LDIR_Name1, longnameseg +  0, 10) 
       && longcmp (ldirentry -> LDIR_Name2, longnameseg + 10, 12) 
       && longcmp (ldirentry -> LDIR_Name3, longnameseg + 22,  4));
}

/************************************************************************/
/*									*/
/*  Increment shortname so it can't interfere with template		*/
/*									*/
/************************************************************************/

static void incshortname (uByte *shortname, const uByte *template)

{
  char number[12];
  int i, minimum_n, n;
  uByte c, *p, z8;

  /* If template type doesn't match shortname type, it can't possibly interfere with it */

  if (!shortcmp (shortname + 8, template + 8, 3)) return;

  /* If template could be a possible increment of shortname, get the number from template as a minimum for our 'n' */

  for (i = 0; i < 8; i ++) {					// scan through name portion (but not type)
    c = template[i];						// get template name character
    if (c == '~') {						// see if it is ~number
      minimum_n = 0;						// ok, decode the number
      while (++ i < 8) {
        c = template[i];
        if (c == ' ') break;
        if ((c < '0') && (c > '9')) return;			// if ~non-number, it can't interfere with shortname
        minimum_n = minimum_n * 10 + c - '0';
      }
      goto gotminn;
    }
    if (shortname[i] != c) return;
  }
  if (i == 8) minimum_n = 1;
gotminn:

  /* If input shortname already contains a number, and it's greater than existing minimum, use it for a minimum */

  for (i = 0; i < 8; i ++) {
    if (shortname[i] == '~') break;
  }
  if (i < 8) {
    n = 0;
    while (++ i < 8) {
      c = shortname[i];
      if ((c < '0') || (c > '9')) break;
      n = n * 10 + c - '0';
    }
    if (minimum_n < n) minimum_n = n;
  }

  /* Now assign one greater than the minimum as the number */

  oz_hw_itoa (++ minimum_n, sizeof number, number);		// convert the number to ascii string
  n = strlen (number);						// get length (number of digits)

  for (i = 0; i < 8; i ++) {					// scan through the name portion (but not the type)
    if (shortname[i] == ' ') break;				// stop if reached end of the name
    if (shortname[i] == '~') break;				// stop if reached the ~oldnumber
  }
  if (i + 1 + n > 8) i = 8 - 1 - n;				// make sure there is room for ~newnumber
  shortname[i++] = '~';						// ok, put in ~
  memcpy (shortname + i, number, n);				// followed by the number
  memset (shortname + i + n, ' ', 8 - i - n);			// space fill (probably redundant)
}

/************************************************************************/
/*									*/
/*  Calculate short name's checksum suitable to put in long name 	*/
/*  record								*/
/*									*/
/*    Input:								*/
/*									*/
/*	shortname = points to 11-byte short name string			*/
/*									*/
/*    Output:								*/
/*									*/
/*	shortcksm = corresponding checksum value			*/
/*									*/
/************************************************************************/

static uByte shortcksm (const uByte *shortname)

{
  uByte cksm;
  int i;

  cksm = 0;
  for (i = 0; i < 11; i ++) cksm = ((cksm & 1) ? 0x80 : 0) + (cksm >> 1) + shortname[i];
  return (cksm);
}

/************************************************************************/
/*									*/
/*  Case insensitive compare for equality				*/
/*									*/
/*    Input:								*/
/*									*/
/*	s1 = pointer to one short name string				*/
/*	s2 = pointer to other short name string				*/
/*	len = number of chars to compare in strings			*/
/*									*/
/*    Output:								*/
/*									*/
/*	shortcmp = 0 : strings are not equal				*/
/*	           1 : strings are equal				*/
/*									*/
/************************************************************************/

static int shortcmp (const uByte *s1, const uByte *s2, int len)

{
  uByte c1, c2;

  while (-- len >= 0) {
    c1 = *(s1 ++);
    c2 = *(s2 ++);
    if (c1 == c2) continue;
    if ((c1 >= 'a') && (c1 <= 'z') && (c1 - c2 == 'a' - 'A')) continue;
    if ((c2 >= 'a') && (c2 <= 'z') && (c2 - c1 == 'a' - 'A')) continue;
    return (0);
  }
  return (1);
}

static int longcmp (const uByte *s1, const uByte *s2, int len)

{
  uWord c1, c2;

  len /= 2;
  while (-- len >= 0) {
    c1 = *(((uWord *)s1) ++);
    c2 = *(((uWord *)s2) ++);
    if (c1 == c2) continue;
    if ((c1 >= 'a') && (c1 <= 'z') && (c1 - c2 == 'a' - 'A')) continue;
    if ((c2 >= 'a') && (c2 <= 'z') && (c2 - c1 == 'a' - 'A')) continue;
    return (0);
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

#if 111

static uLong verify_volume (int readonly, uLong blocksize, OZ_Dbn totalblocks, OZ_VDFS_Iopex *iopex)

{
  oz_dev_vdfs_printk (iopex, "oz_dev_fat verify_volume*: dummied out\n");
  return (OZ_SUCCESS);
}

#else

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

/* Macros for vpb stuff */

#define BLOCKSIZE (volex -> bpb.BPB_BytesPerSec)
#define MALLOC(size) vfy_malloc (vpb, size)
#define FREE(buff) vfy_free (vpb, buff)
#define RLB(dbn,size,buff) oz_dev_vdfs_readlogblock (dbn, 0, size, buff, iopex)
#define WLB(dbn,size,buff) ((vpb -> rdonly) ? OZ_WRITELOCKED : oz_dev_vdfs_writelogblock (dbn, 0, size, buff, 1, iopex))

/* Bit macros */

#define BIT_CLEAR(ubytearray,bitnumber) ubytearray[(bitnumber)/8] &= ~ (1 << ((bitnumber) & 7))
#define BIT_ISCLEAR(ubytearray,bitnumber) !BIT_ISSET(ubytearray,bitnumber)
#define BIT_ISSET(ubytearray,bitnumber) ((ubytearray[(bitnumber)/8] >> ((bitnumber) & 7)) & 1)
#define BIT_SET(ubytearray,bitnumber) ubytearray[(bitnumber)/8] |= 1 << ((bitnumber) & 7)

/* Internal routines */

static uLong verify_thread (void *vpbv);
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

typedef struct Dirpnt Dirpnt;
struct Dirpnt { Dirpnt *next;
                uLong slbn;
                char name[1];
              };

static uLong verify_thread (void *vpbv)

{
  Direntry *dirbucketbuff, *direntry;
  Dirpnt *alldirpnts, *curdirpnt, **dirpntend, *newdirpnt;
  int duplicateclusters, i;
  OZ_VDFS_Filex *rootdirfilex;
  OZ_VDFS_Iopex *iopex;
  OZ_VDFS_Volex *volex;
  uByte *duplicateclusterbits, *firstuseclusterbits;
  uLong dirbucketsize, fatcopy, sts;
  Vpb *vpb;
  Vfymem *mem;

  si = oz_hw_cpu_setsoftint (0);

  vpb = vpbv;
  vpb -> allmem = NULL;
  iopex = vpb -> iopex;
  volex = iopex -> devex -> volume -> volex;

  /* Volex is already set up from the mount */

  storagebitmapsize    = volex -> FATSz * BLOCKSIZE;					// number of bytes in a FAT copy
  switch (volex -> fattype) {
    case FATTYPE_12: storagebitmapsize = (storagebitmapsize + 11) / 12; break;		// number of 12-bit clusters that would fit
    case FATTYPE_16: storagebitmapsize = (storagebitmapsize + 15) / 16; break;		// number of 16-bit clusters that would fit
    case FATTYPE_32: storagebitmapsize = (storagebitmapsize + 31) / 32; break;		// number of 32-bit clusters that would fit
    default: oz_crash ("oz_dev_fat verify_thread: bad fattype %d", volex -> fattype);
  }
  storagebitmapsize    = (storagebitmapsize + 7) / 8;					// we only need 1 bit per cluster
  firstuseclusterbits  = MALLOC (storagebitmapsize);
  duplicateclusterbits = MALLOC (storagebitmapsize);

  oz_dev_vdfs_printk (iopex, "\n*** Scanning file headers\n");

  dirbucketsize = BLOCKSIZE << volex -> l2clusterfactor;
  dirbucketbuff = MALLOC (BLOCKSIZE << volex -> l2clusterfactor);

  rootdirfilex = MALLOC (sizeof *rootdirfilex);
  memset (rootdirfilex, 0, sizeof *rootdirfilex);

scanheaders:
  memset (firstuseclusterbits,  0, storagebitmapsize);
  memset (duplicateclusterbits, 0, storagebitmapsize);
  duplicateclusters = 0;

  curdirpnt = MALLOC (2 + sizeof *curdirpnt);
  curdirpnt -> next    = NULL;
  curdirpnt -> slbn    = dirlbn_init (rootdirfilex, volex);
  curdirpnt -> name[0] = SEPCHR;
  curdirpnt -> name[1] = 0;

  alldirpnts = curdirpnt;

  do {
    oz_dev_vdfs_printk (iopex, "oz_dev_fat verify_thread: scanning %s\n", curdirpnt -> name);
    dirpntend = &(curdirpnt -> next);
    dirlbn = curdirpnt -> slbn;
    while (dirlbn != 0) {
      sts = vfy_rlb (vpb, dirlbn, dirbucketsize, dirbucketbuff);
      if (sts != OZ_SUCCESS) {
        oz_dev_vdfs_printk (iopex, "oz_dev_fat verify_thread: error %u reading directory bucket %u\n", sts, dirlbn);
        break;
      }

      if (!validirbuf (dirbucketsize, dirbucketbuff, &iopex)) {
        oz_dev_vdfs_printk (iopex, "oz_dev_fat verify_thread: directory block %u invalid\n", dirlbn);
      } else {
        for (i = 0; i < dirbucketsize / sizeof (Direntry); i ++) {
          direntry = dirbucketbuff + i;
          if (direntry -> DIR_Attr & ATTR_VOLUME_ID) continue;
          if (direntry -> DIR_Name[0] == 0xE5) continue;
          if (direntry -> DIR_Name[0] == 0) goto direof;
          cluster = (direntry -> DIR_FstClusHI << 16) + direntry -> DIR_FstClusLO;
          if (direntry -> DIR_Attr & ATTR_DIRECTORY) {
            l = strlen (curdirpnt -> name);
            newdirpnt = MALLOC (l + 14 + sizeof *dirpnt);
            newdirpnt -> next = *dirpntend;
            newdirpnt -> slbn = CLUS_TO_LBN (cluster);
            memcpy (newdirpnt -> name, curdirpnt -> name, l);
            memcpy (newdirpnt -> name + l, direntry -> DIR_Name, 8);
            l += 8;
            while (newdirpnt -> name[l-1] == ' ') -- l;
            if (memcmp (direntry -> DIR_Name + 8, "   ", 3) != 0) {
              newdirpnt -> name[l++] = '.';
              memcpy (newdirpnt -> name + l, direntry -> DIR_Name + 8, 3);
              l += 3;
              while (newdirpnt -> name[l-1] == ' ') -- l;
            }
            newdirpnt -> name[l++] = SEPCHR;
            newdirpnt -> name[l++] = 0;
            *dirpntend = newdirpnt;
            dirpntend = &(newdirpnt -> next);
          }
          while (cluster != 0) {
            if (BITISSET (firstuseclusterbits, cluster)) {
              BITSET (duplicateclusterbits, cluster);
              duplicateclusters ++;
            } else {
              BITSET (firstuseclusterbits, cluster);
            }
            sts = vfy_getfatentry (vpb, cluster, &newcluster);
            if (sts != OZ_SUCCESS) break;
            if (newcluster >= eoc) break;
            if ((newcluster < 2) || (newcluster >= highestclusternumber)) {
              oz_dev_vdfs_printk (iopex, "oz_dev_fat verify_thread: bad cluster number %u in chain following %u\n", newcluster, cluster);
              if (!readonly) vfy_putfatentry (vpb, eoc, cluster);
              break;
            }
            cluster = newcluster;
          }
        }
      }
      dirlbn = dirlbn_next (dirlbn, iopex);
    }
direof:
    alldirpnts = curdirpnt -> next;
    FREE (curdirpnt);
    curdirpnt = alldirpnts;
  } while (curdirpnt != NULL);

  /* Repair duplicate clusters */

  if (duplicateclusters) {
    ??????
  }

  /* Write out FATs */

  freeclusters = 0;
  for (fatblock = 0; fatblock < volex -> FATSz; fatblock ++) {

    /* Read a FAT block from any copy that is good */

    for (fatcopy = 0; fatcopy < volex -> bpb.BPB_NumFATs; fatcopy ++) {
      sts = vfy_rlb (vpb, volex -> bpb.BPB_ResvdSecCnt + fatblock + fatcopy * volex -> FATSz, blocksize, fatblockbuff);
      if (sts == OZ_SUCCESS) break;
      oz_dev_vdfs_printk (iopex, "oz_dev_fat verify_thread: error %u reading fat block %u\n", sts, volex -> bpb.BPB_ResvdSecCnt + fatblock + fatcopy * volex -> FATSz);
    }
    if (fatcopy == volex -> bpb.BPB_NumFATs) break;

    /* Scan through it.  For any block that is marked free in the bitmap, mark it free in the FAT */

    switch (fattype) {
      case FATTYPE_12: {
        clusternumber = fatblock * (blocksize / 2);			// get the corresponding cluster number
        for (i = 0; i < blocksize; i += 2) {				// step through all bits in the fatblock
          if (!BIT_ISSET (firstuseclusterbits, clusternumber)) {	// see if it is used by any file
            *((uWord *)(fatblockbuff + i)) = 0;				// if not, clear it
          }
          clusternumber ++;
        }
        break;
      }
      case FATTYPE_16: {
        clusternumber = fatblock * (blocksize / 2);			// get the corresponding cluster number
        for (i = 0; i < blocksize; i += 2) {				// step through all bits in the fatblock
          if (!BIT_ISSET (firstuseclusterbits, clusternumber)) {	// see if it is used by any file
            *((uWord *)(fatblockbuff + i)) = 0;				// if not, clear it
          }
          clusternumber ++;
        }
        break;
      }
      case FATTYPE_32: {
        clusternumber = fatblock * (blocksize / 4);			// get the corresponding cluster number
        for (i = 0; i < blocksize; i += 4) {				// step through all bits in the fatblock
          if (!BIT_ISSET (firstuseclusterbits, clusternumber)) {	// see if it is used by any file
            *((uLong *)(fatblockbuff + i)) &= 0xF0000000;		// if not, clear it
          }
          clusternumber ++;
        }
        break;
      }
    }

    /* Write FAT block to all copies */

    for (fatcopy = 0; fatcopy < volex -> bpb.BPB_NumFATs; fatcopy ++) {
      sts = vfy_wlb (vpb, volex -> bpb.BPB_ResvdSecCnt + fatblock + fatcopy * volex -> FATSz, blocksize, fatblockbuff);
      if (sts != OZ_SUCCESS) {
        oz_dev_vdfs_printk (iopex, "oz_dev_fat verify_thread: error %u writing fat block %u\n", sts, volex -> bpb.BPB_ResvdSecCnt + fatblock + fatcopy * volex -> FATSz,);
      }
    }
  }

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
#endif
