; void mmu_mapear_pagina(
; 	unsigned int virtual,
; 	unsigned int cr3, //indica directorio
; 	unsigned int fisica, // supongo que viene sin shiftear
; 	unsigned int attrs){
; 	attrs |= 1;
; 	int table_entry_val = (fisica & 0xFFFFF000) | attrs;
; 	page_directory_entry* task_directory = (page_directory_entry*) cr3;

; 	int table_index = (virtual >> 12) & 0x3FF; // 9 bits
; 	int directory_entry = virtual >> 22;
; 	page_table_entry* table_entry = (page_table_entry*) (task_directory[directory_entry].table_address << 12);
; 	table_entry[table_index] = *((page_table_entry *) &table_entry_val); // NEGRADA!

; 	tlbflush();

; }

%define virtual [esp + 4]
%define directorio [esp + 8]
%define fisica [esp + 12]
%define attrs [esp + 16]

%macro tlbflush 0
	mov eax, cr3
	mov cr3, eax
%endmacro

extern tlbflush

global mmu_mapear_pagina
global mmu_unmapear_pagina

mmu_mapear_pagina: ; parametros en stack
	mov edx, fisica
	and edx, 0xFFFFF000
	or edx, attrs
	or edx, 0x1 ; la seteo en presente por las dudas

	mov eax, virtual
	mov ecx, directorio

	shr eax, 22
	mov ecx, [ecx + 4 * eax] ; elijo tabla
	and ecx, 0xFFFFF000 ; direccion tabla sin atributos

	mov eax, virtual
	shr eax, 12
	and eax, 0x3FF ; entrada tabla es de 10bits de largo 3 (11) FF(1111 1111), en eax queda indice de entrada a la tabla
	lea ecx, [ecx + 4 * eax]; cargo direccion indice
	mov [ecx], edx ; guardo direccion fisica pagina + attrs

	tlbflush

	ret

; void mmu_unmapear_pagina(unsigned int virtual, unsigned int cr3){
; 	page_directory_entry* task_directory = (page_directory_entry*) cr3;
; 	int table_index = (virtual >> 12) & 0x3FF;
; 	int directory_entry = virtual >> 22;

; 	page_table_entry* table_entry = (page_table_entry*) (task_directory[directory_entry].table_address << 12);
; 	table_entry[table_index] = (page_table_entry) {0};

; 	tlbflush();
; }

mmu_unmapear_pagina:
	mov eax, virtual
	mov ecx, directorio

	shr eax, 22
	mov ecx, [ecx + 4 * eax]
	and ecx, 0xFFFFF000

	mov eax, virtual
	shr eax, 12
	and eax, 0x3FF ; entrada tabla es de 10bits de largo 3 (11) FF(1111 1111)
	lea ecx, [ecx + 4 * eax]
	mov dword [ecx], 0

	tlbflush

	ret