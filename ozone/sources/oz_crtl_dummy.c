//+++2002-08-19
//    Copyright (C) 2001,2002 Mike Rieker, Beverly, MA USA
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
//---2002-08-19

int __stop___libc_subinit ()

{
  oz_sys_io_fs_printerror ("oz_crtl_dummy: __stop___libc_subinit called");
  return (0);
}

int __start___libc_subinit ()

{
  oz_sys_io_fs_printerror ("oz_crtl_dummy: __start___libc_subinit called");
  return (0);
}

int _dl_starting_up ()

{
  oz_sys_io_fs_printerror ("oz_crtl_dummy: _dl_starting_up called");
  return (0);
}

int __start___libc_atexit ()

{
  oz_sys_io_fs_printerror ("oz_crtl_dummy: __start___libc_atexit called");
  return (0);
}

int __stop___libc_atexit ()

{
  oz_sys_io_fs_printerror ("oz_crtl_dummy: __stop___libc_atexit called");
  return (0);
}

int nice (int inc)

{
  return (0);
}

int chown (const char *path, int owner, int group)

{
  return (0);
}

char *setlocale (int category, const char *locale)

{
  return ("");
}
