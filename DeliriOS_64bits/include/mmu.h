#ifndef __MMU_H__
#define __MMU_H__
#include <types.h>

extern uint64_t* krnPML4T;
extern uint64_t* krnPDPT;
extern uint64_t* krnPDT;
extern void* kernelStackPtrBSP;
extern void* kernelStackPtrAP1;

//memory manager
void* kmalloc(uint64_t bytes, char* description);
void kfree(void* pointer);

#endif	/* !__MMU_H__ */
