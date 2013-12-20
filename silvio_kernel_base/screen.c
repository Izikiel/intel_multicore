#include "screen.h"
#include "defines.h"
#include "utils.h"

void printChar(char caracter, uint8_t format, uint8_t posX, uint8_t posY)
{
	uint8_t offset = posY * VIDEO_COLS + posX;
	*((char*)VIDEO_MEMORY + offset) = caracter;
}

void printString(char* cadena, uint8_t format, uint8_t posX, uint8_t posY)
{
	uint8_t strlength = strlen(cadena);
	uint8_t idx = 0;
	while(idx < strlength)
	{
		printChar(cadena[idx], blackOnWhite, posX + idx, posY);
		idx++;
	}
}
