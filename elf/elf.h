#ifndef __ELF_H
#define __ELF_H

#include "types.h"

typedef struct {
	char magic[16];
	unsigned short type;
	unsigned short machine;
	unsigned int version;
	unsigned int entry_point;
	unsigned int ph_offset;
	unsigned int sh_offset;
	unsigned int flags;
	unsigned short header_size;
	unsigned short ph_entry_size;
	unsigned short ph_entry_count;
	unsigned short sh_entry_size;
	unsigned short sh_entry_count;
	unsigned short sh_string_table_index;
} __attribute__((__packed__)) elf_header;

#define EI_CLASS 4
#define EI_DATAENC 5

#define EM_386 3

#define ELFCLASSNONE 0
#define ELFCLASS32 1
#define ELFCLASS64 2

#define ELFDATANONE 0
#define ELFDATA2LSB 1
#define ELFDATA2MSB 2

typedef struct {
	unsigned int name;
	unsigned int type;
	unsigned int flags;
	unsigned int address;
	unsigned int offset;
	unsigned int size;
	unsigned int link;
	unsigned int info;
	unsigned int address_align;
	unsigned int entry_size;
} __attribute__((__packed__)) elf_sheader;

#define SHN_UNDEF 0

#define ELF_EXEC 2

#define ELF_NONE 0
#define ELF_LOAD 1

#define ELF_ATTR_XB 1
#define ELF_ATTR_WB 2
#define ELF_ATTR_RB 4

typedef struct { 
	unsigned int type;
	unsigned int offset;
	unsigned int virtual_address;
	unsigned int physical_address;
	unsigned int file_size;
	unsigned int memory_size;
	unsigned int flags;
	unsigned int align;
} __attribute__((__packed__)) elf_pheader;

typedef struct {
	void * data;
	uint virtual_address;
	uint type;
	uint file_size,mem_size;
	uint attributes;
	uint flags;
	uint alignment;
} __attribute__((__packed__)) elf_segment;

typedef struct {
	const elf_header  * header;
	const elf_pheader * program_header;
} __attribute__((__packed__)) elf_file;

elf_file * elf_read_exec(const void * image);
void elf_destroy(elf_file * elf);

unsigned int elf_entry_point(const elf_file * elf);
elf_segment * elf_get_segment(const elf_file * elf, uint index);
void elf_free_segment(elf_segment * e);

#endif
