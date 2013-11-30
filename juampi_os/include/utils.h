#ifndef __SYSTEM_H
#define __SYSTEM_H

#include <types.h>

extern void memcpy(void *dest, void *src, uint count);
extern void memset(void *dest, uchar val, uint count);

extern uint strlen(const char *str);

extern void strcpy(char * dst, const char * src);
extern void strcat(char * dst, const char * src);

extern int memcmp(const void * , const void * , uint );
extern int strcmp(const char *, const char *);

extern uint umax(uint, uint);

void num_to_str(uint, uint, char *);

void strncpy(char *,char *,unsigned int);

bool is_alpha(char c);

#define CEIL(a,b) ((a)+(b)-1)/(b)

#define BOCHSBREAK __asm__ __volatile__("xchg %bx,%bx");

#endif
