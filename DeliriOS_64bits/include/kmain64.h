#ifndef __KMAIN64_H
#define __KMAIN64_H

void startKernel64_BSPMODE();
void startKernel64_APMODE();
void kernel_panic(const char* functionSender, const char* message);

#endif