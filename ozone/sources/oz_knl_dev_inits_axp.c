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

//extern aic78xx_start ();

void oz_dev_inits ()
{
	oz_dev_pyxis_init ();		// this must be first

//	oz_dev_cdfs_init ();
	oz_dev_conclass_init ();
	oz_dev_consrm_init ();
	oz_dev_conpseudo_init ();
	oz_dev_dectulip_init ();
	oz_dev_dfs_init ();
	oz_dev_dpt_init ();
//	oz_dev_etherloop_init ();
//	oz_dev_floppy_init ();
	oz_dev_ide8038i_init ();
	oz_dev_ip_init ();
	oz_dev_ip_fs_init ();
//	oz_dev_lsil875_init ();
	oz_dev_mutex_init ();
	oz_dev_pipe_init ();
//	oz_dev_ps2mouse_init ();
	oz_dev_ramdisk_init ();
//	oz_dev_rtl8139_init ();
//	oz_dev_scsi_disk_init ();
	oz_dev_srmdev_init ();
}
