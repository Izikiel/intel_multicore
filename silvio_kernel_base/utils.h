#ifndef __utils_H__
#define __utils_H__

void memcpy(void* dst, void*src, unsigned int byteCount);
void fillString(char* toFill, char character, unsigned int length);
unsigned int strlen(char* str);
void strrev(char s[]);
void itoa(int n, char s[]);
void decToHexStr(unsigned int num, char* output, char* title, unsigned int hasHexPrefix);
char* getError(int codError);

#define MIN(a,b) (((a)<(b))?(a):(b))
#define MAX(a,b) (((a)>(b))?(a):(b))

/*
 * Definiciones de funciones hibridas entre C y ASM en asmutils.asm
 */
extern unsigned int getEAX();
extern unsigned int getEBX();
extern unsigned int getECX();
extern unsigned int getEDX();
extern unsigned int getESI();
extern unsigned int getEDI();
extern unsigned int getEBP();
extern unsigned int getESP();
extern unsigned int getEIP();
extern unsigned int getCS();
extern unsigned int getDS();
extern unsigned int getES();
extern unsigned int getFS();
extern unsigned int getGS();
extern unsigned int getSS();
extern unsigned int getCR0();
extern unsigned int getCR2();
extern unsigned int getCR3();
extern unsigned int getCR4();
extern unsigned int getEFLAGS();
extern void waitClock();//haltea el cpu hasta la proxima interrupcion de reloj

#endif  /* !__utils_H__ */