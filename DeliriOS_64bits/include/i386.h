#ifndef __i386_H__
#define __i386_H__

#include <types.h>

extern uint64_t getRAX();
extern uint64_t getRBX();
extern uint64_t getRCX();
extern uint64_t getRDX();
extern uint64_t getRSI();
extern uint64_t getRDI();
extern uint64_t getRBP();
extern uint64_t getRSP();
extern uint64_t getR8();
extern uint64_t getR9();
extern uint64_t getR10();
extern uint64_t getR11();
extern uint64_t getR12();
extern uint64_t getR13();
extern uint64_t getR14();
extern uint64_t getR15();
extern uint64_t getRIP();
extern uint64_t getCS();
extern uint64_t getDS();
extern uint64_t getES();
extern uint64_t getFS();
extern uint64_t getGS();
extern uint64_t getSS();
extern uint64_t getCR0();
extern uint64_t getCR2();
extern uint64_t getCR3();
extern uint64_t getCR4();
extern uint64_t getCR8();
extern uint64_t getRFLAGS();
extern void haltCpu();

#endif  /* !__i386_H__ */
