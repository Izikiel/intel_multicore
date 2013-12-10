/* ** por compatibilidad se omiten tildes **
================================================================================
 TRABAJO PRACTICO 3 - System Programming - ORGANIZACION DE COMPUTADOR II - FCEN
================================================================================
  definicion de funciones de la pantalla utiles
*/

#ifndef __SCREEN_UTILS_H__
#define __SCREEN_UTILS_H__

/* Definicion de la pantalla */
#define VIDEO_FILS 25
#define VIDEO_COLS 80

typedef struct str_rectangulo{
     unsigned int ancho;
     unsigned int alto;
     unsigned int x;
     unsigned int y;
     unsigned short int color;
} __attribute__((__packed__)) Rectangulo ;

void clearBuffer(unsigned short int* outputBufferPtr);
unsigned short int readFormattedCharFromBuffer(unsigned short int* inputBufferPtr, unsigned int offset);
void writeBytesToBuffer(unsigned short int* outputBufferPtr, char* dataSrcPtr, unsigned char _modoEscritura,  unsigned int offset, unsigned int length);
void writeFormattedBytesToBuffer(unsigned short int* outputBufferPtr, char* dataSrcPtr, char* dataSrcFormatPtr,  unsigned int offset, unsigned int length);
void dibujarRectangulo(Rectangulo* r);
void flushEstadoBufferToScreen();
void flushMapaBufferToScreen();
void flushPartialScreenBuffer(int screenId/*0 mapa 1 estado*/, int startX, int startY, unsigned int length);
void flushPartialScreenBufferOffset(int screenId/*0 mapa 1 estado*/, unsigned int offset, unsigned int length);
unsigned int convertMemory2MapPos(unsigned int memoryPosition);
unsigned int convertMap2MemoryPos(unsigned int mapPosition);
void printColoredString(short unsigned int* bufferOut, char* string, unsigned short int color, int x, int y);
void refreshGeneralClock();
void refreshIdleClock();
void dibujarBarraRelojes();
void refreshClock(unsigned int  tarea);
void refreshFlagClock(unsigned int  tarea);
#endif  /* !__SCREEN_UTILS_H__ */

