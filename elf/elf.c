#include "elf.h"

static const char elf_magic[] = { 0x7F, 'E', 'L', 'F' };

//Reemplazar por versiones posta o invertir la seleccion
//para que vos le pases el puntero, como prefieran.

void * kmalloc(uint size)
{
	return (void *) 0;
}

void kfree(void * ptr){
	return;
}

void memfill(void * dst, char value, uint length)
{
	char * _dst = dst;
	while(length-- > 0){
	   *_dst	= value;
	   _dst++;
	}
}

#define kmalloc(x) ((void *) 0)
#define kfree(x)
#define memset(dst,v,len) 

//Arma la estructura de un ELF a partir de un
//buffer de memoria
elf_file* elf_read_exec(const void* image)
{
	const char* imagep = image;
	elf_file* elf = kmalloc(sizeof(elf_file));
	elf->header = image;
	const elf_header* header = elf->header;
	if(memcmp(header->magic,elf_magic,sizeof(elf_magic))
	   || header->magic[EI_CLASS] != ELFCLASS32
	   || header->magic[EI_DATAENC] != ELFDATA2LSB) {
		
		return NULL;
	}
	if(header->type != ELF_EXEC)
		return NULL;

	elf->program_header = NULL;
	if(header->ph_offset) {
		elf->program_header = (elf_pheader*)
		                      (imagep+header->ph_offset);
	} else return NULL;
	
	return elf;
}

void elf_destroy(elf_file* elf)
{
	if(elf == NULL) return;
	kfree(elf);
}

uint elf_entry_point(const elf_file* elf)
{
	return elf->header->entry_point;
}

elf_segment * elf_get_segment(const elf_file* elf, uint index)
{
	if(index > elf->header->ph_entry_count) {
		return NULL;
	}
	const elf_pheader* ph = &elf->program_header[index];
	elf_segment* e = kmalloc(sizeof(elf_segment));
	memfill(e,0,sizeof(elf_segment));
	e->data = (char*) elf->header + ph->offset;
	e->virtual_address = ph->virtual_address;
	e->file_size = ph->file_size;
	e->mem_size = ph->memory_size;
	e->alignment = ph->align;
	e->flags = ph->flags;
	e->type = ph->type;
	return e;
}

void elf_free_segment(elf_segment* e)
{
	kfree(e);
}
