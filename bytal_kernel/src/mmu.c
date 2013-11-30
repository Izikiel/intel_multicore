
/* ** por compatibilidad se omiten tildes **
================================================================================
 TRABAJO PRACTICO 3 - System Programming - ORGANIZACION DE COMPUTADOR II - FCEN
================================================================================
  definicion de funciones del manejador de memoria
*/

#include "mmu.h"
#define PDE(virtual)  (virtual >> 22)
#define PTE(virtual)  ((virtual << 10) >> 22)
#define table_1	0
#define table_2 1
#define table_3 0x100
#define task_memory_section  0x5000 // 1 directorio, 3 tablas, 1 stack, 5 paginas total
#define TASK_COUNT 8

#define kernel_directory_address 0x27000
#define kernel_table_address_1 0x28000
#define kernel_table_address_2 0x29000
#define kernel_table_address_3 0x2A000

extern void mmu_mapear_pagina(
	unsigned int virtual,
	unsigned int cr3, //indica directorio
	unsigned int fisica, // supongo que viene sin shiftear
	unsigned int attrs);

extern void mmu_unmapear_pagina(unsigned int virtual, unsigned int cr3);
void memcpy(char* dst, char* src, unsigned int size);


task_indices task_memory_data[TASK_COUNT];

void mmu_inicializar_dir_kernel() {
	page_directory_entry* directory = (page_directory_entry*) kernel_directory_address; //lo dice el enunciado

	int i;
	for (i = 0; i < 1024; ++i)
		directory[i].p = 0;

	directory[table_1] = (page_directory_entry) {
		.p = 1,
		.rw = 1,
		.user_supervisor = 0,
		.pwt = 0,
		.pcd = 0,
		.a = 0,
		.ign = 0,
		.ps = 0,
		.ignored = 0,
		.table_address = kernel_table_address_1 >> 12
	};

	directory[table_2] = (page_directory_entry) {
		.p = 1,
		.rw = 1,
		.user_supervisor = 0,
		.pwt = 0,
		.pcd = 0,
		.a = 0,
		.ign = 0,
		.ps = 0,
		.ignored = 0,
		.table_address = kernel_table_address_2 >> 12 // mal el enunciado
	};

	directory[table_3] = (page_directory_entry) {
		.p = 1,
		.rw = 1,
		.user_supervisor = 0,
		.pwt = 0,
		.pcd = 0,
		.a = 0,
		.ign = 0,
		.ps = 0,
		.ignored = 0,
		.table_address = kernel_table_address_3 >> 12 // mal el enunciado
	};





	page_table_entry* table = (page_table_entry*) kernel_table_address_1;
	for (i = 0; i < 1024; ++i)
		table[i] = (page_table_entry) {
			.p = 1,
			.rw = 1,
			.user_supervisor = 0,
			.pwt = 0,
			.pcd = 0,
			.a = 0,
			.d = 0,
			.pat = 0,
			.g = 0,
			.ignored = 0,
			.page_address = i  //direcciona a la pagina en indice i desde 0 a 1024
		};

	table = (page_table_entry*) kernel_table_address_2;
	for (i = 0; i < 1024; ++i)
		table[i] = (page_table_entry) {0};

	for (i = 0; i < 0x77f-0x400+1; ++i) //ver si hay que sumarle 1 al limite
		table[i] = (page_table_entry) {
			.p = 1,
			.rw = 1,
			.user_supervisor = 0,
			.pwt = 0,
			.pcd = 0,
			.a = 0,
			.d = 0,
			.pat = 0,
			.g = 0,
			.ignored = 0,
			.page_address = i + 1024  //direcciona a la pagina en indice i
									  // paginas de 1024 a 1919
		};

	table = (page_table_entry*) kernel_table_address_3;
	for (i = 0; i < 1024; ++i)
		table[i] = (page_table_entry) {0};

	mmu_mapear_pagina(
		TASK_CODE,
		kernel_directory_address,
		TASK_IDLE_CODE_SRC_ADDR,
		1 // user = 0, rw = 0,p = 1
		);

	mmu_mapear_pagina(
		TASK_CODE + 0x1000,
		kernel_directory_address + 0x1000,
		TASK_IDLE_CODE_SRC_ADDR + 0x1000,
		1 // user = 0, rw = 0,p = 1
		);

	return;
}

void mmu_inicializar() {
	unsigned int start_address = 0x30000;
	const unsigned int next = 0x1000;

	unsigned int i;
	for (i = 0; i < TASK_COUNT; ++i){
		task_memory_data[i].pde = (page_directory_entry*) start_address;
		task_memory_data[i].stack_0 = (void*) (start_address + next + 3 * next);

		//limpiar memoria
		int j;
		for (j = 0; j < (1024*4); ++j) // 1 directorio + 3 tablas
			task_memory_data[i].pde[j] = (page_directory_entry) {0};

		for (j = 0; j < 0x1000; ++j)
			task_memory_data[i].stack_0[j] = 0;

		// asigno entradas de directorio
		task_memory_data[i].pde[table_1] = (page_directory_entry)  {
			.p = 1,
			.rw = 1,
			.user_supervisor = 1,
			.table_address = (start_address + next) >> 12
		};

		task_memory_data[i].pde[table_2] = (page_directory_entry) {
			.p = 1,
			.rw = 1,
			.user_supervisor = 1,
			.table_address = (start_address + next*2) >> 12
		};

		page_table_entry* table_entry = (page_table_entry*) (start_address + next);

		for (j = 0; j < (0x77f + 1); ++j){
			table_entry[j] = (page_table_entry){
				.p = 1,
				.rw = 1,
				.user_supervisor = 0,
				.pwt = 0,
				.pcd = 0,
				.a = 0,
				.d = 0,
				.pat = 0,
				.g = 0,
				.ignored = 0,
				.page_address = j  // mapeo de kernel para cada tarea
			};
		}

		task_memory_data[i].pde[table_3] = (page_directory_entry) {
			.p = 1,
			.rw = 1,
			.user_supervisor = 1,
			.table_address = (start_address + next*3) >> 12
		}; // esta es la tabla con las paginas de codigo ancla de la tarea

		mmu_mapear_pagina(	//Pagina Codigo 1
			TASK_CODE, //direccion virtual
			(unsigned int) task_memory_data[i].pde,
			AREA_MAR_INICIO + i * 0x2000,
			5);  // 101b, p = 1, rw=0, user = 1

		mmu_mapear_pagina(	//Pagina Codigo 2
			TASK_CODE + 0x1000,
			(unsigned int) task_memory_data[i].pde,
			AREA_MAR_INICIO + 0x1000 + i * 0x2000,
			7);  // 111b, p = 1, rw=1, user = 1

		mmu_mapear_pagina(  //Pagina Ancla
			TASK_CODE + 0x2000,
			(unsigned int) task_memory_data[i].pde,
			0x0,
			5);  // 101b, p = 1, rw=0, user = 1

		memcpy((char*) AREA_MAR_INICIO + i*0x2000,
			   (char*) TASK_1_CODE_SRC_ADDR + i*0x2000,
			   0x2000);
		//copiar codigo y ancla

		start_address += task_memory_section;
	}

}

void memcpy(char* dst, char* src, unsigned int size){
	int i;
	for (i = 0; i < size; ++i)
		dst[i] = src[i];
}


// void mmu_mapear_pagina(
// 	unsigned int virtual,
// 	unsigned int cr3, //indica directorio
// 	unsigned int fisica, // supongo que viene sin shiftear
// 	unsigned int attrs){
// 	attrs |= 1;
// 	int table_entry_val = (fisica & 0xFFFFF000) | attrs;
// 	page_directory_entry* task_directory = (page_directory_entry*) cr3;

// 	int table_index = (virtual >> 12) & 0x3FF; // 10 bits
// 	int directory_entry = virtual >> 22;
// 	page_table_entry* table_entry = (page_table_entry*) (task_directory[directory_entry].table_address << 12);
// 	table_entry[table_index] = *((page_table_entry *) &table_entry_val); // NEGRADA!

// 	tlbflush();

// }

// void mmu_unmapear_pagina(unsigned int virtual, unsigned int cr3){
// 	page_directory_entry* task_directory = (page_directory_entry*) cr3;
// 	int table_index = (virtual >> 12) & 0x3FF;
// 	int directory_entry = virtual >> 22;

// 	page_table_entry* table_entry = (page_table_entry*) (task_directory[directory_entry].table_address << 12);
// 	table_entry[table_index] = (page_table_entry) {0};

// 	tlbflush();
// }

unsigned int pagina_barco(unsigned int barco, unsigned int pagina){
	unsigned int start_address = 0x30000 + (0x5000 * barco);
	page_directory_entry* pde = (page_directory_entry*) start_address;
	page_table_entry* pte = (page_table_entry*) (pde[table_3].table_address << 12);
	unsigned int physical_address = (unsigned int) (pte[pagina].page_address << 12);
	return physical_address;
}

unsigned int directory_barco(unsigned int barco){
	return (0x30000 + 0x5000 * barco);
}