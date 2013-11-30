/* ** por compatibilidad se omiten tildes **
================================================================================
 TRABAJO PRACTICO 3 - System Programming - ORGANIZACION DE COMPUTADOR II - FCEN
================================================================================
  definicion de funciones del manejador de memoria
*/

#ifndef __MMU_H__
#define __MMU_H__

#include "defines.h"
#include "i386.h"

typedef struct
{
	char p:1;
	char rw:1;
	char user_supervisor:1;
	char pwt:1;
	char pcd:1;
	char a:1;
	char ign:1;
	char ps:1;
	char ignored:4;
	int table_address:20;
} __attribute__((__packed__, aligned (4))) page_directory_entry;

typedef struct
{
	char p:1;
	char rw:1;
	char user_supervisor:1;
	char pwt:1;
	char pcd:1;
	char a:1;
	char d:1;
	char pat:1;
	char g:1; //define si la pagina es global!!!
	char ignored:3;
	int page_address:20;
} __attribute__((__packed__, aligned (4))) page_table_entry;

typedef struct
{
	page_directory_entry* pde;
	char*	stack_0;
} task_indices;

unsigned int pagina_barco(unsigned int barco, unsigned int pagina);
unsigned int directory_barco(unsigned int barco);
#endif	/* !__MMU_H__ */
