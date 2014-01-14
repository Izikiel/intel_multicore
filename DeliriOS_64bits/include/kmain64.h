#ifndef __KMAIN64_H
#define __KMAIN64_H
#include <types.h>

void startKernel64_BSPMODE();
void startKernel64_APMODE();
void kernel_panic(const char* functionSender, const uint64_t lineError, const char* fileError, const char* message);
void reallocateStack(void* newBase);

#endif