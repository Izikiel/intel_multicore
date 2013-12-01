//+++2004-01-03
//    Copyright (C) 2001,2002,2003,2004  Mike Rieker, Beverly, MA USA
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
//---2004-01-03

void oz_dev_inits ()

{
	oz_dev_pyxis_init ();		// this must be first to finish pyxis init

//	oz_dev_cdfs_init ();		// CDROM filesystem driver
	oz_dev_conclass_init ();	// the big console driver, part 1
	oz_dev_consrm_init ();		// the big console driver, part 2
	oz_dev_dectulip_init ();	// dec tulip ethernet driver
	oz_dev_dfs_init ();		// ozone filesystem driver
//	oz_dev_dpt_init ();		// ozone disk pass-thru fs driver
//	oz_dev_etherloop_init ();	// ethernet loopback driver
//	oz_dev_floppy_init ();		// generic floppy driver
//	oz_dev_ide8038i_init ();	// generic ide driver
	oz_dev_ip_init ();		// tcp/ip stack driver
	oz_dev_ip_fs_init ();		// nfs-like client driver (ip_fs_server_linux is the server on linux)
//	oz_dev_lsil875_init ();		// lsilogic 53C875 scsi driver
//	oz_dev_ramdisk_init ();		// system memory ramdisk driver
//	oz_dev_rtl8139_init ();		// realtek 8139 driver
//	oz_dev_scsi_disk_init ();	// scsi disk class driver
	oz_dev_srmdev_init ();		// access to devices via SRM
}
