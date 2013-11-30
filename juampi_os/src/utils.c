#include <utils.h>

unsigned int strlen(const char* str)
{
	unsigned int i = 0;
	while(str[i]) {
		i++;
	}
	return i;
}

void strcpy(char* dst, const char* src)
{
	unsigned int i;
	for(i = 0; src[i] != '\0'; i++) {
		dst[i] = src[i];
	}
	dst[i] = '\0';
}

void strcat(char* dst, const char* src)
{
	unsigned int i = 0, j = 0;
	while(dst[i]) {
		i++;
	}
	while(src[j]) {
		dst[i] = src[j];
		i++; j++;
	}
	dst[i] = '\0';
}

int memcmp(const void* _m1, const void* _m2, uint bytes)
{
	const char* m1 = _m1, * m2 = _m2;
	for(uint i = 0; i < bytes; i++)
		if(m1[i] != m2[i]) {
			return (m1[i]-m2[i] < 0) ? -1 : 1;
		}
	return 0;
}

int strcmp(const char* str1, const char* str2)
{
	uint i;
	for(i = 0; str1[i] == str2[i]; i++)
		if(str1[i] == '\0') {
			return 0;
		}
	return str1[i]-str2[i];
}

unsigned int umax(unsigned int a, unsigned int b)
{
	return (a < b) ? b : a;
}

static void remove_zeros(char* buffer)
{
	uint i,j;
	for(i = 0; buffer[i]; i++) {
		if(buffer[i] != '0') {
			break;
		}
	}
	if(!buffer[i]) {
		buffer[0] = '0';
		buffer[1] = '\0';
		return;
	}
	for(j = i; buffer[j]; j++) {
		buffer[j-i]=buffer[j];
	}
	buffer[j-i] = '\0';
}

void num_to_str(uint n, uint base, char* output)
{
	char buf[33];
	memset(buf,'0',sizeof(buf));
	*(buf+32) = '\0'; //Voy a imprimir con todos los ceros delante, asi que necesito 8 lugares fijos.
	uint ind = 31;
	do {
		char c = n % base;
		if(c > 9) {
			c = c-10+'A';
		} else {
			c = c+'0';
		}
		buf[ind--] = c;
		n = n/base;
	} while(n > 0);
	remove_zeros(buf);
	strcpy(output,buf);
}

void strncpy(char* dst, char* src, unsigned int len)
{
	for(int i = 0; i < len; i++) {
		if(src[i] == '\0') {
			dst[i] = '\0';
			return;
		}
		dst[i] = src[i];
	}
}

bool is_alpha(char c)
{
	if(c >= 'a' && c <= 'z') return true;
	if(c >= 'A' && c <= 'Z') return true;
	if(c >= '0' && c <= '9') return true;
	return false;
}
