#ifndef __MMU_H__
#define __MMU_H__

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
void mmu_mapear_pagina(unsigned int virtual, pagedir_entry* pageDirBase, unsigned int fisica, unsigned char readWriteb, unsigned char userSupervisorb);
void mmu_unmapear_pagina(unsigned int virtual, pagedir_entry* pageDirBase);
unsigned int changePaginationContext(pagedir_entry* newDirContext);
unsigned int mmu_virtual2physic(unsigned int virtual, pagedir_entry* pageDirBase);

extern pagedir_entry* krnPageDir;

#endif	/* !__MMU_H__ */
