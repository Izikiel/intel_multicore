#include <mmu.h>
#include <defines.h>

void* kernelStackPtrBSP = (void*)KERNEL_STACK_PTR_BSP;
void* kernelStackPtrAP1 = (void*)KERNEL_STACK_PTR_AP1;

/*
	Estoy usando paginacion IA-32e => bits CR0.PG=1 + CR4.PAE=1 + EFER.LME=1
	paginas de 2mb
*/

uint64_t* krnPML4T = (uint64_t*) KERNEL_PML4T_POINTER;
uint64_t* krnPDPT = (uint64_t*) KERNEL_PDPT_POINTER;
uint64_t* krnPDT = (uint64_t*) KERNEL_PDT_POINTER;

void* kmalloc(uint64_t bytes, char* description){
	return NULL;
}

void kfree(void* pointer){

}