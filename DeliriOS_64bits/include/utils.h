#ifndef __utils_H__
#define __utils_H__

#include <types.h>

#define min(a,b) (((a)<(b))?(a):(b))
#define max(a,b) (((a)>(b))?(a):(b))

//impl en i386.asm
extern void breakpoint();

void memcpy(void* dst, const void *src, uint32_t byteCount);
void memset(void* dst, uint8_t value, uint32_t length);
uint32_t strlen(const char* str);
void strrev(char s[]);
void itoa(uint32_t n, char s[]);
uint64_t atoi(const char* string);
void decToHexStr(uint32_t num, char* output, char* title, uint8_t hasHexPrefix);
char* getError(uint32_t codError);

void strcpy(char* dst, const char* src);
void strcat(char* dst, const char* src);
int memcmp(const void* _m1, const void* _m2, uint64_t bytes);
int strcmp(const char* str1, const char* str2);
void strncpy(char* dst, const char* src, uint64_t len);
//-----------------------------------------------

//Devuelve el indice de la primera aparicion del caracter en la string 
//comenzando desde str[startIndex]. En caso de no encontrarlo, devuelve -1
int32_t nextTokenIdx(const char *str, const char delimiter, const uint32_t startIndex);

//Devuelve la cantidad de apariciones del caracter en la string 
//comenzando desde str[startIndex].
uint32_t needleCount(const char *str, const char needle, const uint32_t startIndex);

#endif  /* !__utils_H__ */