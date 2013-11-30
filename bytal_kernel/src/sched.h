/* ** por compatibilidad se omiten tildes **
================================================================================
 TRABAJO PRACTICO 3 - System Programming - ORGANIZACION DE COMPUTADOR II - FCEN
================================================================================
  definicion de funciones del scheduler
*/

#ifndef __SCHED_H__
#define __SCHED_H__
#include "defines.h"


void sched_inicializar();
unsigned short sched_proximo_indice();
unsigned char bandera_time();
void next_flag();
int index;
unsigned char bandera_mode;
char get_live_tareas();
int current_flag;

unsigned char is_alive();
void kill_task(int t_index);
#endif	/* !__SCHED_H__ */
