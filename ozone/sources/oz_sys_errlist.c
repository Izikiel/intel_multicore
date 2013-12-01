//+++2003-03-01
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
//---2003-03-01

#include "ozone.h"
#include "oz_knl_status.h"
#include "oz_sys_errlist.h"

globaldef const int oz_sys_sysnerrors = OZ_MAXERROR;
globaldef const Charp oz_sys_syserrors[OZ_MAXERROR+1] = {
	"PENDING operation pending", /*0*/
	"SUCCESS normal successful completion", /*1*/
	"FLAGWASSET flag was set", /*2*/
	"FLAGWASCLR flag was clear", /*3*/
	"ASTDELIVERED ast was delivered", /*4*/
	"PRIVSECMULTMAP private section already mapped", /*5*/
	"ACCVIO access violation", /*6*/
	"ABORTED operation aborted", /*7*/
	"ADDRINUSE address range already in use", /*8*/
	"ADDRSPACEFULL address space is full", /*9*/
	"NOPAGETABLE no page table exists at that address", /*10*/
	"ADDRNOTUSED address is not used", /*11*/
	"EVENTABORTED event flag was deleted", /*12*/
	"BADDEVCLASS -obsolete", /*13*/
	"BADDEVUNIT -obsolete", /*14*/
	"PROCMODE resource owned by more privileged mode", /*15*/
	"STARTED operation has been started but not yet completed", /*16*/
	"BADDEVNAME unknown or bad device name", /*17*/
	"DEVNOTSELECTABLE device driver does not support select function", /*18*/
	"BADIOFUNC device driver does not support that i/o function", /*19*/
	"SSFAILED -obsolete", /*20*/
	"FILEALREADYOPEN a file is already open on that channel", /*21*/
	"FILENOTOPEN no file is open on that channel", /*22*/
	"BADFILENAME syntax error in file name", /*23*/
	"DEVICEFULL device is full", /*24*/
	"ENDOFFILE reached the end of the file", /*25*/
	"IOFAILED i/o has failed for some low-level reason", /*26*/
	"NOTERMINATOR no terminator was found in input before buffer filled", /*27*/
	"NOSUCHFILE the requested file was not found", /*28*/
	"KERNELONLY this operation can only be invoked from kernel mode", /*29*/
	"NOFREESPTES the system page table is full", /*30*/
	"BADBLOCKSIZE bad disk block size (must be power of 2)", /*31*/
	"HANDTBLFULL handle table is full", /*32*/
	"BADHANDOBJTYPE handles do not support that object type", /*33*/
	"NULLHANDLE handle is null", /*34*/
	"BADHANDLE invalid handle number", /*35*/
	"CLOSEDHANDLE that handle has been closed", /*36*/
	"HANDREFOVERFLOW -obsolete", /*37*/
	"INVSYSCALL -obsolete", /*38*/
	"EXMAXIMGLEVEL too many nested shareable image references", /*39*/
	"LOGNAMETOOBIG logical name and all its values too long", /*40*/
	"LOGNAMATRCONF logical name attribute conflict", /*41*/
	"LOGNAMTBLNZ cannot have values when creating logical name table", /*42*/
	"NOSUPERSEDE logical name cannot be superseded", /*43*/
	"SUPERSEDED logical name superseded a previous setting", /*44*/
	"NOOUTERMODE logical name cannot be created because an innermode one exists", /*45*/
	"NOLOGNAME logical name could not be found", /*46*/
	"NOTLOGNAMTBL given logical is not a table", /*47*/
	"LOGNAMHNDNO -obsolete", /*48*/
	"NOTLOGNAMOBJ attempt to get object from non-object logical", /*49*/
	"BADLOGNOBJTYPE logical names do not support object type or object type doesn't match", /*50*/
	"NOTMOUNTED volume is not mounted", /*51*/
	"ALREADYMOUNTED volume is already mounted", /*52*/
	"FILEALREADYEXISTS file already exists and was not overwritten", /*53*/
	"VOLNAMETOOLONG volume name string was too long", /*54*/
	"OPENFILESONVOL there are open files on the volume", /*55*/
	"BADHOMEBLKVER invalid home block version number", /*56*/
	"BADHOMEBLKCKSM home block checksum error", /*57*/
	"FILENAMETOOLONG filename string too long", /*58*/
	"INVALIDFILENUM filenumber is invalid", /*59*/
	"FILEDELETED file has been deleted", /*60*/
	"DISKISFULL disk is full", /*61*/
	"INDEXEXTHEADER index file contains an extension header", /*62*/
	"BADHDRCKSM bad header block checksum", /*63*/
	"VBNZERO attempt to access vbn zero", /*64*/
	"BADVOLNAME invalid characters in volume name", /*65*/
	"FILENOTADIR supplied file is not a directory", /*66*/
	"FILEISADIR supplied file is a directory", /*67*/
	"DIRNOTEMPTY the directory is not empty", /*68*/
	"SACREDFILE cannot perform operation on sacred file", /*69*/
	"ACCONFLICT requested access not compatible with current accessors", /*70*/
	"NOREADACCESS no read access was requested", /*71*/
	"NOWRITEACCESS no write access was requested", /*72*/
	"SECATTRTOOLONG security attributes too long", /*73*/
	"SECACCDENIED -obsolete", /*74*/
	"FILENOTCONTIG requested operation requires contiguous file", /*75*/
	"UNKIMAGEFMT unknown image format", /*76*/
	"BADIMAGEFMT image format recognized but image is corrupt", /*77*/
	"BADSYSCALL invalid system call number", /*78*/
	"DIVBYZERO divide by zero attempted", /*79*/
	"SINGLESTEP single instruction step", /*80*/
	"BREAKPOINT hit debug breakpoint", /*81*/
	"ARITHOVER arithmetic overflow", /*82*/
	"SUBSCRIPT subscript out of bounds", /*83*/
	"UNDEFOPCODE undefined opcode", /*84*/
	"FLOATPOINT floating point error", /*85*/
	"ALIGNMENT memory alignment error", /*86*/
	"GENERALPROT general protection fault", /*87*/
	"DOUBLEFAULT double stack fault", /*88*/
	"RESIGNAL condition being re-signalled to outer handler", /*89*/
	"RESUME execution resuming after signal", /*90*/
	"UNWIND unwinding stack", /*91*/
	"UNALIGNEDBUFF buffer is not aligned as required", /*92*/
	"BADBUFFERSIZE buffer size is out of allowed range", /*93*/
	"UNALIGNEDXLEN rlen/wlen parameter not aligned as required", /*94*/
	"BADBLOCKNUMBER block number is out of range", /*95*/
	"WRITELOCKED device is write-locked and cannot accept any write requests", /*96*/
	"NOTSUPINLDR operation is not supported in the boot loader", /*97*/
	"LOGNAMNOTTBL -obsolete", /*98*/
	"OLDLINUXCALL old linux call", /*99*/
	"DEVOFFLINE device is offline", /*100*/
	"VOLNOTVALID volume not valid - media was switched", /*101*/
	"SIGNAL_HUP unix HUP signal detected", /*102*/
	"SIGNAL_INT unix INT signal detected", /*103*/
	"SIGNAL_QUIT unix QUIT signal detected", /*104*/
	"SIGNAL_UNUSED unix UNUSED signal detected", /*105*/
	"SIGNAL_KILL unix KILL signal detected", /*106*/
	"SIGNAL_USR1 unix USR1 signal detected", /*107*/
	"SIGNAL_USR2 unix USR2 signal detected", /*108*/
	"SIGNAL_PIPE unix PIPE signal detected", /*109*/
	"SIGNAL_ALRM unix ALRM signal detected", /*110*/
	"SIGNAL_TERM unix TERM signal detected", /*111*/
	"SIGNAL_STKFLT unix STKFLT signal detected", /*112*/
	"SIGNAL_UNKNOWN unix UNKNOWN signal detected", /*113*/
	"MAXPROCESSES -obsolete", /*114*/
	"BADUSERNAME bad username (too long)", /*115*/
	"BADQUOTATYPE unrecognized quota type specified", /*116*/
	"EXCEEDQUOTA the operation exceeded a quota", /*117*/
	"IOFSPARSECONT filesystem parsing continuing", /*118*/
	"EXMAXFILENAME device/file name too long", /*119*/
	"EXMAXLOGNAMLVL logical name nesting too deep", /*120*/
	"UNIXERROR -obsolete", /*121*/
	"CTRLCHARABORT aborted by control-character operation", /*122*/
	"ABORTEDBYCLI aborted by command", /*123*/
	"IOFSWILDSCANCONT filesystem wildcard scan continuing", /*124*/
	"MISSINGPARAM missing parameter", /*125*/
	"BADPARAM invalid parameter value", /*126*/
	"UNDEFSYMBOL undefined symbol", /*127*/
	"BUFFEROVF buffer overflowed (value doesn't fit)", /*128*/
	"UNKNOWNCOMMAND unknown command", /*129*/
	"THREADEAD thread has exited", /*130*/
	"BADITEMCODE item code specified is invalid for the given object", /*131*/
	"BADITEMSIZE item buffer size specified is invalid for the given code", /*132*/
	"INVOBJTYPE invalid object type given for requested function", /*133*/
	"LOGNAMNOTINTBL given logical name is not in a table (it has been deleted)", /*134*/
	"ZEROSIZESECTION section size is zero", /*135*/
	"NOMEMORY no memory is availble for the operation", /*136*/
	"HWNOTDEFINED hardware supplied is not defined", /*137*/
	"HWIPAMNOTDEF given hardware ip address and mask not defined", /*138*/
	"NOSUCHROUTE cannot find requested route", /*139*/
	"CHANALRBOUND channel is already bound", /*140*/
	"NOROUTETODEST no route to requested destination defined", /*141*/
	"CHANOTBOUND channel is not bound", /*142*/
	"HWALRDEFINED given hardware is already defined", /*143*/
	"NOMOREPHEMPORTS no more ephemeral ports available", /*144*/
	"CONNECTFAIL connection failed (destination host did not respond)", /*145*/
	"LINKDROPPED remote host did not respond to keepalive packets", /*146*/
	"LINKABORTED remote host reset connection", /*147*/
	"LINKDISCONNECTED -obsolete", /*148*/
	"TIMEDOUT operation timed out (took longer than expected)", /*149*/
	"USERSTACKOVF user mode stack overflowed", /*150*/
	"MAXERROR" }

uLong oz_sys_syserrlist (uLong status, uLong size, char *buff)

{
  if (status >= OZ_MAXERROR) {
    oz_sys_sprintf (size, buff, "Unknown OZ error %u\n", status);
    return (OZ_UNKNOWNSTATUS);
  }

  strncpyz (buff, oz_sys_syserrors[status], size);
  return (OZ_SUCCESS);
}
