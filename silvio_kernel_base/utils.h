#ifndef __utils_H__
#define __utils_H__

void memcpy(void* dst, void*src, unsigned int byteCount);
void fillString(char* toFill, char character, unsigned int length);
unsigned int strlen(char* str);
void strrev(char s[]);
void itoa(int n, char s[]);
void decToHexStr(unsigned int num, char* output, char* title, unsigned int hasHexPrefix);
char* getError(int codError);

#define min(a,b) (((a)<(b))?(a):(b))
#define max(a,b) (((a)>(b))?(a):(b))

#endif  /* !__utils_H__ */