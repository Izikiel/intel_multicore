#include "utils.h"

void memcpy(void* dst, void* src, uint32_t byteCount){	
	char* dst8 = (char*)dst;
	char* src8 = (char*)src;
	while(byteCount--){
		*(dst8++)=*(src8++);
	}
}

void fillString(char* toFill, char character, uint32_t length){
	do{
		toFill[length--] = character;
	}while(length);
}

uint32_t strlen(char* str){
	uint32_t overflowThreshold = 2000;//cualquier string con mas de este largo es truncada a 2000, igualmente no entran mas caracteres en la pantalla
	uint32_t result=0;
	while((result<overflowThreshold) && (*(str + result)!='\0')){
		result++;
	}
	return result;
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
	
	//padding hasta completar el hexa de 32 bits
	while(idx<8){
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