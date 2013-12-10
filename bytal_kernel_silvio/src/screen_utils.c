#include "utils.h"
#include "screen_utils.h"
#include "defines.h"
#include "colors.h"
#include "contextManager.h"

//NOTA:ojo que como es puntero a short, incrementar el puntero corre solo sizeof(unsigned short int) = 2 bytes
unsigned short int* _videoMemoryPtr = (unsigned short int*) 0xB8000;
unsigned short int* _videoBufferEstadoPtr = (unsigned short int*) 0x2D000;
unsigned short int* _videoBufferMapaPtr = (unsigned short int*) 0x2F000;
char* clockChars = "|/-\\";

void clearBuffer(unsigned short int* outputBufferPtr){
	unsigned char _modoEscritura = _modoEscrituraTexto;//fondo negro, letras blancas		
	unsigned char _caracter = ' ';//espacio en blanco

	unsigned int offset = 0;
	while(offset<VIDEO_FILS*VIDEO_COLS)
	{
		unsigned short int pixel = (_modoEscritura << 8) | _caracter;
		*(outputBufferPtr + offset) = pixel;
		offset++;
	}
}

unsigned short int readFormattedCharFromBuffer(unsigned short int* inputBufferPtr, unsigned int offset){	
	return *(inputBufferPtr + offset);
}

void writeBytesToBuffer(unsigned short int* outputBufferPtr, char* dataSrcPtr, unsigned char _modoEscritura,  unsigned int offset, unsigned int length){
	unsigned int index = 0;//indice de la linea
	while(index<length)
	{
		unsigned char _caracter = *(dataSrcPtr + index);
		unsigned short int pixel = (_modoEscritura << 8) | _caracter;
		*(outputBufferPtr + offset + index) = pixel;
		index++;
	}
}

void writeFormattedBytesToBuffer(unsigned short int* outputBufferPtr, char* dataSrcPtr, char* dataSrcFormatPtr,  unsigned int offset, unsigned int length){
	unsigned int index = 0;//indice de la linea
	while(index<length)
	{
		unsigned char _caracter = *(dataSrcPtr + index);
		unsigned short int pixel = (*(dataSrcFormatPtr + index) << 8) | _caracter;
		*(outputBufferPtr + offset + index) = pixel;
		index++;
	}
}

void flushEstadoBufferToScreen(){
	memcpy(_videoMemoryPtr, _videoBufferEstadoPtr, VIDEO_FILS*VIDEO_COLS*2);
}

void flushMapaBufferToScreen(){
	memcpy(_videoMemoryPtr, _videoBufferMapaPtr, VIDEO_FILS*VIDEO_COLS*2);
}

unsigned int convertMemory2MapPos(unsigned int memoryPosition){
	//hay 80*24 = 1920 posiciones en pantalla de 2 bytes cada una
	//quiero mapear los primeros 7.5mb de memoria fisica con una biyeccion entre paginas de 4k y una posicion en pantalla
	return memoryPosition/4096;
}

unsigned int convertMap2MemoryPos(unsigned int mapPosition){
	return mapPosition*4096;
}

void dibujarRectangulo(Rectangulo* rectangulo){

	int filIdx, offset = 0;

	char test[rectangulo->ancho];
	fillString(&(test[0]), 176/*' '*/, rectangulo->ancho);
	for(filIdx=0; filIdx < rectangulo->alto; filIdx++){
		offset = filIdx * VIDEO_COLS + rectangulo->x + rectangulo->y * VIDEO_COLS;
		//En el siguiente test[] va 1.. numero magico para que no pasen cosas malas
		//que flasheas? va 0, porque escribe rectangulo->ancho bytes, sino te pasas de rango en test[] !
		//el maestruli se reiria...
		//te dije que la densidad del helio es 1..te lo dije
		writeBytesToBuffer(_videoBufferEstadoPtr, &(test[1]), rectangulo->color, offset, rectangulo->ancho);
	}
}

void printColoredString(short unsigned int* bufferOut, char* string, unsigned short int color, int x, int y){
	writeBytesToBuffer(bufferOut, string, color, x + y * VIDEO_COLS, strlen(string));
}

void refreshIdleClock(){
	//pongo el char nulo para strlen!
	char* strBuf = " \0";
	strBuf[0] = clockChars[3-getIdleClock()];
	printColoredString(_videoBufferEstadoPtr, strBuf, blackOnCyan, 0, 24);
	printColoredString(_videoBufferMapaPtr, strBuf, blackOnCyan, 0, 24);

	flushPartialScreenBuffer(getSelectedScreen(), 79, 24, 1);
}

void refreshGeneralClock(){	
	//pongo el char nulo para strlen!
	char* strBuf = " \0";
	strBuf[0] = clockChars[3-getGeneralClockNumber()];
	printColoredString(_videoBufferEstadoPtr, strBuf, blackOnCyan, 79, 24);
	printColoredString(_videoBufferMapaPtr, strBuf, blackOnCyan, 79, 24);

	flushPartialScreenBuffer(getSelectedScreen(), 79, 24, 1);
}

void flushPartialScreenBuffer(int screenId/*0 mapa 1 estado*/, int startX, int startY, unsigned int length){
	memcpy(_videoMemoryPtr + startX + startY * VIDEO_COLS, (screenId==0) ? _videoBufferMapaPtr + startX + startY * VIDEO_COLS : _videoBufferEstadoPtr + startX + startY * VIDEO_COLS, length*2/*2 bytes por char en pantalla*/);
}

void flushPartialScreenBufferOffset(int screenId/*0 mapa 1 estado*/, unsigned int offset, unsigned int length){
	memcpy(_videoMemoryPtr + offset, (screenId==0) ? _videoBufferMapaPtr + offset : _videoBufferEstadoPtr + offset, length*2/*2 bytes por char en pantalla*/);
}


void dibujarBarraRelojes(){

	int y = VIDEO_FILS-1;
	int x = 0;
	//-------Fondo Negro---------------------//
	Rectangulo barraClocks;
	barraClocks.color = whiteOnBlack;
	barraClocks.ancho = VIDEO_COLS;
	barraClocks.alto = 1;
	barraClocks.x = 0;
	barraClocks.y = VIDEO_FILS-1;

	dibujarRectangulo(&barraClocks);

	//--------Numeritos---------------------//
	int i=0;
	for(i=0;i<CANT_TAREAS;i++){
		char numero[3]=" \0";
		itoa(i + 1, numero);

		x=6 + (3 * i);//padding
		printColoredString(_videoBufferEstadoPtr, numero, blackOnWhite, x, y);
		printColoredString(_videoBufferMapaPtr, numero, blackOnWhite, x, y);
		x=x+28;//padding bandera
		printColoredString(_videoBufferMapaPtr, numero, blackOnWhite, x, y);
		printColoredString(_videoBufferEstadoPtr, numero, blackOnWhite, x, y);

	}

	
}

void refreshClock(unsigned int tarea){

	int y = VIDEO_FILS-1;
	int x = 0;

	char* strBuf = " \0";
	int indexClock = getClock(tarea);
	strBuf[0] = (indexClock>=0) ? clockChars[indexClock] : 'x';

	if (tarea>=0){//porque != 0 ? si le pasas la tarea entre 0 y 7...
		x = x + 7 + 3 * tarea;//padding
	}
	printColoredString(_videoBufferEstadoPtr, strBuf, (indexClock>=0) ? blackOnWhite : whiteOnRed, x, y);
	printColoredString(_videoBufferMapaPtr, strBuf, (indexClock>=0) ? blackOnWhite : whiteOnRed, x, y);

	//llamo a esto para que se repinte la pantalla parcialmente!	
	if(getSelectedScreen()>=0){
		flushPartialScreenBuffer(getSelectedScreen(), x, y, 1);
	}
}


void refreshFlagClock(unsigned int tarea){

	int y = VIDEO_FILS-1;
	int x = 0;

	char* strBuf = " \0";
	int indexClock = getFlagClock(tarea);
	strBuf[0] = (indexClock>=0) ? clockChars[indexClock] : 'x';

	if (tarea>=0){//porque != 0 ? si le pasas la tarea entre 0 y 7...
		x+=35 + 3 * tarea;//padding
	}
	printColoredString(_videoBufferEstadoPtr, strBuf, (indexClock>=0) ? blackOnWhite : whiteOnRed, x, y);
	printColoredString(_videoBufferMapaPtr, strBuf, (indexClock>=0) ? blackOnWhite : whiteOnRed, x, y);

	//llamo a esto para que se repinte la pantalla parcialmente!	
	if(getSelectedScreen()>=0){
		flushPartialScreenBuffer(getSelectedScreen(), x, y, 1);
	}
}
