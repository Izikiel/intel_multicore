/* ** por compatibilidad se omiten tildes **
================================================================================
 TRABAJO PRACTICO 3 - System Programming - ORGANIZACION DE COMPUTADOR II - FCEN
================================================================================
  definicion de funciones del manejador de memoria
*/

#ifndef __MMU_H__
#define __MMU_H__

#define KERNEL_PAGEDIR_POINTER 0x00027000
#define KERNEL_FIRST_PAGETABLE_POINTER 0x00028000
#define KERNEL_SECOND_PAGETABLE_POINTER 0x00029000

#define TASK_START_OFFSET_PAGINATION_STRUCTS_POINTER 0x00030000
#define TASK_START_OFFSET_LEVEL0_STACKS_POINTER 0x00050000
#define TASK_START_CODE_SRC_ADDR 0x00010000
#define TASK_FIRST_PAGE_VIRTUAL_ADDR 0x40000000
#define TASK_SECOND_PAGE_VIRTUAL_ADDR 0x40001000
#define TASK_THIRD_PAGE_VIRTUAL_ADDR 0x40002000

#define TASK_STACK_BASE 0x40001C00
#define IDLE_TASK_STACK0_BASE 0x2B000

typedef struct str_page_dir {
    unsigned char present:1;
    unsigned char readWrite:1;
    unsigned char userSupervisor:1;
    unsigned char pageLevelWriteThrough:1;
    unsigned char pageLevelCacheDisable:1;
    unsigned char accesed:1;
    unsigned char ignored:1;
    unsigned char pageSize:1;//0=4k,1=4Mb
    unsigned char global:1;//siempre en 0(ignored)
    unsigned char disponible:3;//queda en 0
	unsigned int pageBase:20;//ptr a base del array de 1024 paginas
} __attribute__((__packed__, aligned(4))) pagedir_entry;

typedef struct str_page_table {
	unsigned char present:1;
    unsigned char readWrite:1;
    unsigned char userSupervisor:1;
    unsigned char pageLevelWriteThrough:1;
    unsigned char pageLevelCacheDisable:1;
    unsigned char accesed:1;
    unsigned char dirtyBit:1;
    unsigned char pageAttributeIndex:1;//siempre en 0
    unsigned char global:1;//siempre en 0(ignored)
    unsigned char disponible:3;//queda en 0
	unsigned int pageBase:20;
} __attribute__((__packed__, aligned(4))) pagetable_entry;

void pageDirectoryInitialize(pagedir_entry* pageDirRef, unsigned int present, unsigned int start, unsigned int end);
void pageTableInitialize(pagetable_entry* pageTable, unsigned int present, unsigned int start, unsigned int end, unsigned int mapOffset);
void mmu_inicializar_dir_kernel();
void mmu_inicializar_dir_tarea(unsigned int taskNumber);
void mmu_inicializar_dir_tareas();
void mmu_mapear_pagina(unsigned int virtual, pagedir_entry* pageDirBase, unsigned int fisica, unsigned char readWriteb, unsigned char userSupervisorb);
void mmu_unmapear_pagina(unsigned int virtual, pagedir_entry* pageDirBase);
unsigned int changePaginationContext(pagedir_entry* newDirContext);
void mmu_remap_task_page(unsigned int taskNumber, unsigned int taskPageNumber, unsigned int newPhysicalAddress);
unsigned int mmu_virtual2physic(unsigned int virtual, pagedir_entry* pageDirBase);
pagedir_entry* mmu_get_task_pageDirAddress(unsigned int taskNumber);
pagetable_entry* mmu_get_task_pageTableAddress(unsigned int taskNumber);
unsigned int mmu_get_task_physical_destination_task_page(unsigned int taskNumber);
unsigned int mmu_get_task_stack_level0(unsigned int taskNumber);
unsigned int mmu_get_flag_stack_level0(unsigned int taskNumber);

extern pagedir_entry* krnPageDir;

#endif	/* !__MMU_H__ */
