#include <mmu.h>
#include <defines.h>

void* krnPML4T = (void*) KERNEL_PML4T_POINTER;
void* kernelStackPtrBSP = (void*)KERNEL_STACK_PTR_BSP;
void* kernelStackPtrAP1 = (void*)KERNEL_STACK_PTR_AP1;

void init_64gb_identity_mapping(){

}