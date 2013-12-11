/* ** por compatibilidad se omiten tildes **
================================================================================
 TRABAJO PRACTICO 3 - System Programming - ORGANIZACION DE COMPUTADOR II - FCEN
================================================================================
*/

#include "colors.h"
#include "defines.h"
#include "game.h"
#include "syscall.h"

#define BANDERA_BUFFER  0x40001000

/*
 * Estructura de la tarea :
 *
 *  0x40000000 - 0x40001000 : codigo tarea          (   5 KB)
 *  0x40001000 - 0x40001400 : area de banderas      (   1 KB)
 *  0x40001400 - 0x40001c00 : pila tarea            (   2 KB)
 *  0x40001c00 - 0x40001ffc : pila bandera          (1020 B )
 *  0x40001ffc - 0x40002000 : dir. funcion bandera  (   4 B )
 */

/* Estructura de para acceder a memoria de video */
typedef struct {
    unsigned char c;
    unsigned char a;
} ca;

unsigned char * bandera();

// global var
#define GLOBAL_START 0x40000000  
#define var_B GLOBAL_START+0x2000-0x200

void task() {
    /* Tarea 8 */
    //NOSE, navega muchas paginas y en una de esas rompe todo
    int i;
    while(1) {
       for(i=0;i<=10000000000;i=i+0x1000) {
          int a = (int)(0x00100000+i); 
          int b = (int)(0x00103000+i); 
          syscall_navegar((unsigned int)a,(unsigned int)b);
       }
    };
}

unsigned char * bandera() {
    ca (*buffer)[10] = (ca (*)[10]) (BANDERA_BUFFER);
    int *b = (int*)(var_B);
    unsigned int fil;
    unsigned int col;
    (*b)++; if (*b == 6) *b = 0;
    for (fil = 0; fil < 5; fil++) {
        for (col = 0; col < 10; col++) {
            buffer[fil][col].c = 222;
            if( col > *b || fil > *b )
               buffer[fil][col].a = C_BG_BROWN | C_FG_LIGHT_BROWN;
            else
               buffer[fil][col].a = C_BG_MAGENTA | C_FG_LIGHT_MAGENTA;
        }
    } 
    syscall_bandera_fin((unsigned int) buffer);
    /* Para que el compilador no tire warning... */
    return 0;
}
