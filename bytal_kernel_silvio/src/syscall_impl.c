/* ** por compatibilidad se omiten tildes **
================================================================================
 TRABAJO PRACTICO 3 - System Programming - ORGANIZACION DE COMPUTADOR II - FCEN
================================================================================
  
*/
#include "syscall_impl.h"
#include "screen_utils.h"
#include "utils.h"
#include "colors.h"
#include "defines.h"
#include "mmu.h"
#include "screen_utils.h"
#include "screen_mapa.h"
#include "screen_estado.h"
#include "contextManager.h"

void impl_syscall80_handler(unsigned int eax, unsigned int ebx, unsigned int ecx){
	switch(eax){
		case 0x923:
			impl_syscall_fondear(ebx);
			break;
		case 0x83A:
			impl_syscall_canonear(ebx, ecx);
			break;
		case 0xAEF:
			impl_syscall_navegar(ebx, ecx);
			break;
	}
}

void impl_syscall_fondear(unsigned int dirFisicaAMapear){
    //cambio la pagina en tierra
    mmu_remap_task_page(getTareaActual(), 3, dirFisicaAMapear);
}

void impl_syscall_canonear(unsigned int dirFisicaTarget, unsigned int offsetRelativoContenidoMisil){
	unsigned int virtualBuffer = TASK_VIRTUAL_P1 + offsetRelativoContenidoMisil;/*tarea + offset*/
	unsigned int bufferPtr = mmu_virtual2physic(virtualBuffer, (pagedir_entry*) getCR3());
    memcpy((void*)dirFisicaTarget, (void*) bufferPtr, 97/*97 bytes de misil*/);
    
    //notifico a context_manager
	notificarCanonazo(dirFisicaTarget);
}

void impl_syscall_navegar(unsigned int dirFisicaPrimerPagina, unsigned int dirFisicaSegundaPagina){
	pagedir_entry* taskPageDir = mmu_get_task_pageDirAddress(getTareaActual());
	//obtengo dir fisicas de las primeras 2 paginas
	void* fstCode = (void*) (mmu_virtual2physic(TASK_VIRTUAL_P1/*0x40000000*/, taskPageDir));
	void* sndCode = (void*) (mmu_virtual2physic(TASK_VIRTUAL_P2/*0x40001000*/, taskPageDir));

	//las replico a destino
	memcpy((void*) dirFisicaPrimerPagina, fstCode, 4096/*4k*/);
	memcpy((void*) dirFisicaSegundaPagina, sndCode, 4096/*4k*/);

	//remapeo las paginas al nuevo destino
	mmu_remap_task_page(getTareaActual(), 1, dirFisicaPrimerPagina);
	mmu_remap_task_page(getTareaActual(), 2, dirFisicaSegundaPagina);
}
