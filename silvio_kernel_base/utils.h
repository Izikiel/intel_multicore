#ifndef __utils_H__
#define __utils_H__

#include <stdint.h>

void memcpy(void* dst, void*src, uint32_t byteCount);
void memset(void* dst, uint8_t value, uint32_t length);
uint32_t strlen(char* str);
void strrev(char s[]);
void itoa(uint32_t n, char s[]);
void decToHexStr(uint32_t num, char* output, char* title, uint8_t hasHexPrefix);
char* getError(uint32_t codError);

//Tomado de juampiOS utils
void strcpy(char* dst, const char* src);
void strcat(char* dst, const char* src);
int memcmp(const void* _m1, const void* _m2, uint32_t bytes);
int strcmp(const char* str1, const char* str2);
void strncpy(char* dst, const char* src, uint32_t len);

#define min(a,b) (((a)<(b))?(a):(b))
#define max(a,b) (((a)>(b))?(a):(b))

#endif  /* !__utils_H__ */