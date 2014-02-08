#ifndef __MMU_H__
#define __MMU_H__
#include <types.h>

/*Regiones hardcodeadas de memoria usadas por el kernel:
//pueden cambiar, ver en defines.h
*Paginacion IA32e
PML4 -> 0x140000..0x140ffff  => 512 entries * 8 bytes(64bits) cada una
solo se instancio la primera entrada de PML4

PDPT -> 0x141000..0x141ffff  => 512 entries * 8 bytes(64bits) cada una
se instanciaron las primeras 64 entradas nomas, para mapear 64gb

PDT -> 0x142000..0x181FFF => 32768 entries * 8 bytes(64bits) cada una
se instanciaron las 32768 entries para mapear 64 gb con 32768 paginas de 2 megas*/

/*Pagination IA32e pointers*/
extern uint64_t* krnPML4T;
extern uint64_t* krnPDPT;
extern uint64_t* krnPDT;
extern void* kernelStackPtrBSP;
extern void* kernelStackPtrAP1;
extern void* core_stack_ptrs[];

#endif	/* !__MMU_H__ */
