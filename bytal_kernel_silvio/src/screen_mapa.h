/* ** por compatibilidad se omiten tildes **
================================================================================
 TRABAJO PRACTICO 3 - System Programming - ORGANIZACION DE COMPUTADOR II - FCEN
================================================================================
  definicion de funciones de la pantalla mapa
*/

#ifndef __SCREEN_MAPA_H__
#define __SCREEN_MAPA_H__


/* Definicion de la pantalla */
#define VIDEO_FILS 25
#define VIDEO_COLS 80

void pintarPantallaMapa();
void inicializarBuffersMapa();
void pintarPosicionMapa(unsigned int mapPos, char formato, char caracter);
//void pintarPosicionMemoria(unsigned int memoryPos, char formato, char caracter);
void remapearAnclaTarea(unsigned int mapPos, unsigned int taskNumber);
void navegarTarea(unsigned int firstMapPos, unsigned int secondMapPos, unsigned int taskNumber);
void sacarMisil();
void dibujarMisil();

//Esta funcion saca 1 pagina de la tarea parametrizada de la posicion del mapa dejando
//la posicion visualmente correcta
void sacarElementoTareaPosicion(unsigned int mapPos,unsigned int taskNumber, char formato, unsigned char emptyChar);
void agregarElementoTareaPosicion(unsigned int mapPos,unsigned int taskNumber, char formato );
void borrarTareaDelMapa(unsigned int tarea);
#endif  /* !__SCREEN_MAPA_H__ */
