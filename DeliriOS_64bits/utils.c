#include <utils.h>

void memcpy(void* dst, const void* src, uint32_t byteCount){	
	char* dst8 = (char*)dst;
	char* src8 = (char*)src;
	while(byteCount--){
		*(dst8++)=*(src8++);
	}
}

void memset(void* dst, uint8_t value, uint32_t length){
	uint8_t* ptr = (uint8_t*) dst;
	for(uint32_t idx = 0; idx < length; idx++){
		ptr[idx] = value;
	}
}

uint32_t strlen(const char* str)
{
	uint32_t i = 0;
	while(str[i]) {
		i++;
	}
	return i;
}

 void itoa(uint32_t number, char s[])
 {
 	uint32_t idx = 0;
 	uint32_t sign = number;
    if (sign < 0){
    	number = -number;   
    }     
    do {       
    	s[idx++] = (number % 10) + '0';   
    	number = number / 10;
    }while (number > 0);
     if (sign < 0){
         s[idx++] = '-';
     }
     s[idx] = '\0';
     strrev(s);
 }

void strrev(char s[])
{
	uint32_t i=0;
	uint32_t j=0;
	char c=0;
	//hace un swap con dos indices, uno creciente y otro decreciente
	for (i=0,j=strlen(s)-1; i<j;i++,j--) {
		c = s[i];
		s[i] = s[j];
		s[j] = c;
	}
}

void decToHexStr(uint32_t num, char* output, char* title, uint8_t hasHexPrefix){
	uint32_t idx = 0;
	while(num>0){
		uint32_t remainder = num%16;
		switch(remainder){
			      case 10:
			          output[idx] = 'A';
			          break;
			      case 11:
			          output[idx] = 'B';
			          break;
			      case 12:
			          output[idx] = 'C';
			          break;
			      case 13:
			          output[idx] = 'D';
			          break;
			      case 14:
			          output[idx] = 'E';
			          break;
			      case 15:
			          output[idx] = 'F';
			          break;
			      default :
			         output[idx] = remainder + 48;//48 es el ascii de 0
			         break;
		}
		num=(uint32_t) (num/16);
		idx++;		
	}
	
	//padding hasta completar el hexa de 64 bits
	while(idx<16/*16 chars hexa = 64 bits number*/){
		output[idx++]='0';
	}

	//pongo prefijo hexa
	if(hasHexPrefix!=0){
		output[idx++] = 'x';
		output[idx++] = '0';
	}
	//dejo espacio entre numero y epigrafe
	output[idx++] = ' ';	
	
	//imprimo epigrafe del hexa
	uint32_t lenTitle=strlen(title);
	while(lenTitle--){
		output[idx++] = title[lenTitle];
	}
	
	//pongo el caracter nulo al final de la string
	output[idx]='\0';	
	//invierto la string(armamos todo al reves ;] )
	strrev(output);
}

char* getError(uint32_t codError){
char* descripcion;
	switch(codError)
	{
		case 0:
			descripcion = "#DE Divide Error";
		break;
		case 1:
			descripcion = "#DB RESERVED";
		break;
		case 2:
			descripcion = "NMI Interrupt";
		break;
		case 3:
			descripcion = "#BP Breakpoint";
		break;
		case 4:
			descripcion = "#OF Overflow";
		break;
		case 5:
			descripcion = "#BR BOUND Range Exceeded";
		break;
		case 6:
			descripcion = "#UD Invalid Opcode (Undefined Opcode)";
		break;
		case 7:
			descripcion = "#NM Device Not Available (No Math Coprocessor)";
		break;
		case 8:
			descripcion = "#DF Double Fault";
		break;
		case 9:
			descripcion = "Coprocessor Segment Overrun (reserved)";
		break;
		case 10:
			descripcion = "#TS Invalid TSS";
		break;
		case 11:
			descripcion = "#NP Segment Not Present";
		break;
		case 12:
			descripcion = "#SS Stack-Segment Fault";
		break;
		case 13:
			descripcion = "#GP General Protection";
		break;
		case 14:
			descripcion = "#PF Page Fault";
		break;
		case 15:
			descripcion = "(Intel reserved. Do not use.)";
		break;
		case 16:
			descripcion = "#MF x87 FPU Floating-Point Error (Math Fault)";
		break;
		case 17:
			descripcion = "#AC Alignment Check";
		break;
		case 18:
			descripcion = "#MC Machine Check";
		break;
		case 19:
			descripcion = "#XM SIMD Floating-Point Exception";
		break;
		case 20:
			descripcion = "#VE Virtualization Exception";
		break;
		default:
			descripcion = "";
		break;
	}
	return descripcion;
}

//Tomado de juampiOS utils

void strcpy(char* dst, const char* src)
{
        uint32_t i;
        for(i = 0; src[i] != '\0'; i++) {
                dst[i] = src[i];
        }
        dst[i] = '\0';
}

void strcat(char* dst, const char* src)
{
        uint32_t i = 0, j = 0;
        while(dst[i]) {
                i++;
        }
        while(src[j]) {
                dst[i] = src[j];
                i++; j++;
        }
        dst[i] = '\0';
}

int memcmp(const void* _m1, const void* _m2, uint32_t bytes)
{
        const char* m1 = _m1, * m2 = _m2;
        for(uint32_t i = 0; i < bytes; i++)
                if(m1[i] != m2[i]) {
                        return (m1[i]-m2[i] < 0) ? -1 : 1;
                }
        return 0;
}

int strcmp(const char* str1, const char* str2)
{
        uint32_t i;
        for(i = 0; str1[i] == str2[i]; i++)
                if(str1[i] == '\0') {
                        return 0;
                }
        return str1[i]-str2[i];
}

void strncpy(char* dst, const char* src, uint32_t len)
{
        for(uint32_t i = 0; i < len; i++) {
                if(src[i] == '\0') {
                        dst[i] = '\0';
                        return;
                }
                dst[i] = src[i];
        }
}

//Devuelve la cantidad de apariciones del caracter en la string 
//comenzando desde str[startIndex].
uint32_t needleCount(char *str, char needle, uint32_t startIndex)
{
	uint32_t j=0;
	uint32_t i=startIndex;
	while(str[i] != '\0'){
		if(str[i] == needle){
			j++;
		}
		i++;
	}
	return j;
}

//Devuelve el indice de la primera aparicion del caracter en la string 
//comenzando desde str[startIndex]. En caso de no encontrarlo, devuelve -1
int32_t nextTokenIdx(char *str, char delimiter, uint32_t startIndex)
{
	uint32_t i=startIndex;
	while(str[i] != '\0'){
		if(str[i] == delimiter){
			return i;
		}
		i++;
	}
	return i;
}