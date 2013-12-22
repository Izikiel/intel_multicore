#include <mmu.h>
#include <defines.h>

void* krnPML4T = (void*) KERNEL_PML4T_POINTER;
void* kernelStackPtr = (void*)KERNEL_STACK_PTR;

void init_64gb_identity_mapping(){

}