#ifndef __utils_H__
#define __utils_H__

#include <stdint.h>

void memcpy(void* dst, void*src, uint32_t byteCount);
void fillString(char* toFill, char character, uint32_t length);
uint32_t strlen(char* str);
void strrev(char s[]);
void itoa(uint32_t n, char s[]);
void decToHexStr(uint32_t num, char* output, char* title, uint8_t hasHexPrefix);
char* getError(uint32_t codError);

#define min(a,b) (((a)<(b))?(a):(b))
#define max(a,b) (((a)>(b))?(a):(b))

#endif  /* !__utils_H__ */