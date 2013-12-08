#ifndef __BIOS_H
#define __BIOS_H

#include <types.h>

#define VIDEO_WIDTH 80
#define VIDEO_HEIGHT 25
#define TAB_WIDTH 4

enum color {
	BLACK = 0, BLUE, GREEN, CYAN, 
	RED, MAGENTA, BROWN, LIGHTGREY, 
	DARKGREY, LIGHTBLUE, LIGHTGREEN, LIGHTCYAN,
	LIGHTRED, LIGHTMAGENTA, LIGHTBROWN, WHITE};
typedef enum color color;
//Limpia la pantalla
void scrn_cls();
//Coloca el color de fondo y la fuente.
void scrn_setmode(color,color);
//Devuelve el formato de fondo y fuente
ushort scrn_getmode();
//Devuelve la fila del cursor
uchar scrn_getrow();
//Devuelve la columna del cursor
uchar scrn_getcol();
//Coloca el cursor en un lugar. 
//PRE: Las coordenadas pasadas son validas dentro de la memoria de video
void scrn_setcursor(uchar, uchar);
//Imprime un caracter a memoria de video. No utiliza format.
void scrn_putc(char, ushort);
void scrn_move_back();
//Imprime el mensaje, si es posible hacerlo
void scrn_print(const char*);
//Imprime el mensaje, con formato estilo printf de C. 
//PRE: La cantidad de parametros pasados DEBE ser correcta
void scrn_printf(const char*,...);
//Imprime el mensaje en la direccion indicada. Devuelve 0
//si esta todo bien, -1 en caso de error. Se usa en la syscall
//de impresion directa a pantalla
int scrn_pos_print(uchar row, uchar col, const char * msg);
//Como scrn_pos_print pero printf
int scrn_pos_printf(uchar row, uchar col, const char * msg,...);
bool in_video_mem(uint address);
#endif
