/* ** por compatibilidad se omiten tildes **
================================================================================
 TRABAJO PRACTICO 3 - System Programming - ORGANIZACION DE COMPUTADOR II - FCEN
================================================================================
  definicion de funciones de la pantalla estado
*/

#ifndef __SCREEN_ESTADO_H__
#define __SCREEN_ESTADO_H__

/* Definicion de la pantalla */
#define VIDEO_FILS 25
#define VIDEO_COLS 80
#define FRAME_BANDERAS_ALTO 15
#define FRAME_BANDERAS_ANCHO 50
#define BANDERAS_ALTO 5
#define BANDERAS_ANCHO 10
#define FRAME_CONSOLA_ANCHO 29
#define FRAME_CONSOLA_ALTO 14
#define REGS_PER_COLUMN 13

void pintarPantallaEstado();
void dibujarBanderas();
void drawBandera(char* bandera, char* nombreBandera, unsigned int startPos);
char* getEstadoTitle();
void dibujarConsolaRegistros(unsigned int EDI, unsigned int ESI, unsigned int EBP, unsigned int ESP, unsigned int EBX, unsigned int EDX, unsigned int ECX, unsigned int EAX, unsigned int EFLAGS, unsigned int EIP);
void dibujarConsolaTareas();
void notificarRegistro(char* registro, unsigned int valor, int indice);
void dibujarLineaUltimoError(int codError, unsigned int taskNumber);
void actualizarLineaTarea(int tarea, int error);

#endif  /* !__SCREEN_ESTADO_H__ */
