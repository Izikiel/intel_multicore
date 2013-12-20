#include "screen.h"
#include "defines.h"
#include "utils.h"

//NOTA:ojo que como es puntero a short, incrementar el puntero corre solo sizeof(uint16_t) = 2 bytes
static uint16_t* _outputMemoryPtr = (uint16_t*) VIDEO_MEMORY;

void printChar(char caracter, uint8_t format, uint8_t posX, uint8_t posY)
{
	uint16_t offset = posX + posY * VIDEO_COLS;	
	uint16_t pixel = (format << 8) | caracter;
	*(_outputMemoryPtr + offset) = pixel;
}

void printString(char* cadena, uint8_t format, uint8_t posX, uint8_t posY)
{
	uint32_t strlength = strlen(cadena);
	uint8_t idx = 0;
	while(idx < strlength)
	{
		printChar(cadena[idx], format, posX + idx, posY);
		idx++;
	}
}

void printInteger(uint32_t number, uint8_t format, uint8_t posX, uint8_t posY){
	char buffer[VIDEO_COLS*VIDEO_FILS];
	itoa(number, buffer);
	printString(buffer, format, posX, posY);
}

static void clearBuffer(unsigned short int* outputBufferPtr){
	uint8_t _modoEscritura = modoEscrituraTexto;//fondo negro, letras blancas
	uint8_t _caracter = ' ';//espacio en blanco

	uint32_t offset = 0;
	while(offset<VIDEO_FILS*VIDEO_COLS)
	{
		uint16_t pixel = (_modoEscritura << 8) | _caracter;
		*(outputBufferPtr + offset) = pixel;
		offset++;
	}
}

void clrscr(){
	clearBuffer(_outputMemoryPtr);
}

void test(){

}